#!/usr/bin/env python3
# web telemetry server exposing rest endpoints for dashboard

import argparse
import contextlib
import http.server
import json
import os
import queue
import sqlite3
import threading
import time
import urllib.parse
from pathlib import Path

# try loading pymysql for mysql connectivity
try:
    import pymysql
    import pymysql.cursors
    HAVE_PYMYSQL = True
except ImportError:
    HAVE_PYMYSQL = False

# base paths and server defaults
BASE_DIR = Path(__file__).resolve().parent
STATIC_DIR = BASE_DIR / "static"
CONFIG_FILE = BASE_DIR / "blackbox.conf"
DEFAULT_PORT = 8080
DEFAULT_HOST = "0.0.0.0"

# default configuration dictionary
CONFIG = {
    "db_backend": "mysql",
    "mysql_host": "127.0.0.1",
    "mysql_port": 3306,
    "mysql_user": "blackbox_writer",
    "mysql_password": "writer_pass",
    "mysql_reader_user": "blackbox_reader",
    "mysql_reader_password": "reader_pass",
    "mysql_database": "blackbox",
    "mysql_socket": "/run/mysqld/mysqld.sock",
    "sqlite_path": "blackbox.db",
    "interval": 1.0,
}

# persistent connection pool and negative-cache state to avoid per-request TCP/auth handshakes
_POOL_MAX = 8
_db_pool = queue.LifoQueue(maxsize=_POOL_MAX)
_state_lock = threading.Lock()
_preferred_mysql_idx = 0
_mysql_retry_after = 0.0
_config_loaded = False

# lightweight caches to avoid redundant COUNT(*) scans and multi-tab duplicate queries
_deletions_count_cache = {"key": None, "count": 0}
_response_cache = {}
_response_cache_lock = threading.Lock()


# drain pooled connections when config or backend changes
def _clear_db_pool():
    global _mysql_retry_after
    with _state_lock:
        _mysql_retry_after = 0.0
    while True:
        try:
            conn, _ = _db_pool.get_nowait()
            try:
                conn.close()
            except Exception:
                pass
        except queue.Empty:
            break


# parse key = value settings from blackbox.conf
def load_config():
    global _config_loaded
    _config_loaded = True
    _clear_db_pool()
    if not CONFIG_FILE.exists():
        return
    try:
        with open(CONFIG_FILE, "r", encoding="utf-8") as f:
            for line in f:
                if "#" in line:
                    line = line.split("#", 1)[0]
                line = line.strip()
                if not line:
                    continue
                if "=" in line:
                    k, v = line.split("=", 1)
                    k = k.strip()
                    v = v.strip()
                    if k in CONFIG:
                        if k == "mysql_port":
                            CONFIG[k] = int(v)
                        elif k == "interval":
                            CONFIG[k] = float(v)
                        else:
                            CONFIG[k] = v
    except Exception as e:
        print(f"[SERVER WARNING] Error reading {CONFIG_FILE}: {e}")


# opens a new mysql connection (preferring local unix domain socket when available) or sqlite fallback
def get_db():
    global _preferred_mysql_idx, _mysql_retry_after

    now = time.monotonic()
    # 1. try mysql connection if enabled and not in failure cooldown
    if CONFIG["db_backend"] != "sqlite" and HAVE_PYMYSQL and now >= _mysql_retry_after:
        users = [
            (CONFIG["mysql_reader_user"], CONFIG["mysql_reader_password"]),
            (CONFIG["mysql_user"], CONFIG["mysql_password"]),
        ]
        pref = _preferred_mysql_idx
        order = [pref, 1 - pref] if pref in (0, 1) else [0, 1]

        sock = CONFIG.get("mysql_socket") or ""
        use_socket = (
            CONFIG["mysql_host"] in ("127.0.0.1", "localhost")
            and bool(sock)
            and os.path.exists(sock)
        )

        for idx in order:
            user, pwd = users[idx]
            if not user:
                continue
            # try unix domain socket first on localhost for faster kernel IPC, then TCP fallback
            attempts = []
            if use_socket:
                attempts.append({"unix_socket": sock})
            attempts.append({"host": CONFIG["mysql_host"], "port": int(CONFIG["mysql_port"])})

            for target_kwargs in attempts:
                try:
                    conn = pymysql.connect(
                        user=user,
                        password=pwd,
                        database=CONFIG["mysql_database"],
                        cursorclass=pymysql.cursors.DictCursor,
                        connect_timeout=1.0,
                        read_timeout=1.5,
                        autocommit=True,
                        **target_kwargs,
                    )
                    with _state_lock:
                        _preferred_mysql_idx = idx
                        _mysql_retry_after = 0.0
                    return conn, "MySQL"
                except Exception:
                    continue

        # back off mysql connection attempts briefly so sqlite fallback doesn't stall every poll
        with _state_lock:
            _mysql_retry_after = time.monotonic() + 5.0

    # 2. sqlite fallback
    sqlite_cfg = Path(CONFIG["sqlite_path"])
    sqlite_path = sqlite_cfg if sqlite_cfg.is_absolute() else (BASE_DIR / sqlite_cfg)
    if sqlite_path.exists():
        try:
            uri = f"file:{sqlite_path.resolve()}?mode=ro"
            conn = sqlite3.connect(uri, uri=True, timeout=1.5, check_same_thread=False)
            conn.row_factory = sqlite3.Row
            return conn, "SQLite"
        except Exception:
            pass

    return None, "None"


# borrows a persistent connection from the pool and returns it cleanly after use
@contextlib.contextmanager
def borrow_db():
    desired_sqlite = (CONFIG["db_backend"] == "sqlite")
    conn = None
    backend = "None"

    while True:
        try:
            cand_conn, cand_backend = _db_pool.get_nowait()
        except queue.Empty:
            break

        # discard pooled connection if backend mode changed or if mysql became eligible after sqlite fallback
        if (desired_sqlite and cand_backend != "SQLite") or (
            not desired_sqlite
            and cand_backend == "SQLite"
            and HAVE_PYMYSQL
            and time.monotonic() >= _mysql_retry_after
        ):
            try:
                cand_conn.close()
            except Exception:
                pass
            continue

        if cand_backend == "MySQL":
            try:
                cand_conn.ping(reconnect=True)
            except Exception:
                try:
                    cand_conn.close()
                except Exception:
                    pass
                continue

        conn, backend = cand_conn, cand_backend
        break

    if conn is None:
        conn, backend = get_db()

    healthy = True
    try:
        yield conn, backend
    except Exception:
        healthy = False
        raise
    finally:
        if conn is not None:
            if healthy:
                try:
                    _db_pool.put_nowait((conn, backend))
                except queue.Full:
                    try:
                        conn.close()
                    except Exception:
                        pass
            else:
                try:
                    conn.close()
                except Exception:
                    pass


# micro-TTL cache helper to deduplicate bursts across multiple open tabs
def _cached_call(key, ttl_seconds, builder):
    now = time.monotonic()
    with _response_cache_lock:
        entry = _response_cache.get(key)
        if entry is not None and (now - entry[0]) < ttl_seconds:
            return entry[1]
    result = builder()
    with _response_cache_lock:
        _response_cache[key] = (time.monotonic(), result)
    return result


def _disconnected_vitals():
    return {
        "cpu_usage": 0.0,
        "ram_usage": 0.0,
        "cpu_temp": 0.0,
        "fan_speed": 0,
        "timestamp": "DB Not Connected",
        "cpp_engine_active": False,
        "age_seconds": None,
        "db_backend": "Disconnected",
        "message": "Start the C++ engine (./system_blackbox) or initialize MySQL (sudo mariadb < sql/setup.sql)",
    }


def _query_latest_vitals(conn, backend):
    cursor = conn.cursor()
    if backend == "MySQL":
        cursor.execute(
            """
            SELECT v.cpu_usage, v.ram_usage, v.cpu_temp, v.fan_speed, v.timestamp, v.session_id,
                   TIMESTAMPDIFF(SECOND, v.timestamp, NOW()) AS age_seconds,
                   s.id AS s_id, s.start_time AS s_start_time, s.pid AS s_pid,
                   s.hostname AS s_hostname, s.end_time AS s_end_time
            FROM vitals_log v
            LEFT JOIN sessions s ON s.id = v.session_id
            ORDER BY v.id DESC LIMIT 1;
            """
        )
    else:
        cursor.execute(
            """
            SELECT v.cpu_usage, v.ram_usage, v.cpu_temp, v.fan_speed, v.timestamp, v.session_id,
                   (strftime('%s', 'now', 'localtime') - strftime('%s', v.timestamp)) AS age_seconds,
                   s.id AS s_id, s.start_time AS s_start_time, s.pid AS s_pid,
                   s.hostname AS s_hostname, s.end_time AS s_end_time
            FROM vitals_log v
            LEFT JOIN sessions s ON s.id = v.session_id
            ORDER BY v.id DESC LIMIT 1;
            """
        )
    row = cursor.fetchone()

    if not row:
        return {
            "cpu_usage": 0.0,
            "ram_usage": 0.0,
            "cpu_temp": 0.0,
            "fan_speed": 0,
            "timestamp": "No Data Yet",
            "cpp_engine_active": False,
            "db_backend": backend,
            "message": f"{backend} tables are ready. Start ./system_blackbox to record data.",
        }

    active_session = None
    if row["s_id"] is not None and row["s_end_time"] is None:
        active_session = {
            "id": row["s_id"],
            "start_time": str(row["s_start_time"]),
            "pid": row["s_pid"],
            "hostname": row["s_hostname"],
        }
    else:
        try:
            cursor.execute(
                "SELECT id, start_time, pid, hostname FROM sessions WHERE end_time IS NULL ORDER BY id DESC LIMIT 1;"
            )
            s_row = cursor.fetchone()
            if s_row:
                active_session = dict(s_row)
        except Exception:
            pass

    age = row["age_seconds"] if row["age_seconds"] is not None else 0
    live_window = max(4, int(float(CONFIG.get("interval", 1.0)) * 3) + 1)
    is_live = (age is not None and age <= live_window)

    return {
        "cpu_usage": round(float(row["cpu_usage"]), 1),
        "ram_usage": round(float(row["ram_usage"]), 1),
        "cpu_temp": round(float(row["cpu_temp"]), 1) if row["cpu_temp"] is not None else 0.0,
        "fan_speed": int(row["fan_speed"]) if row["fan_speed"] is not None else 0,
        "timestamp": str(row["timestamp"]),
        "session_id": row["session_id"],
        "age_seconds": max(0, age),
        "cpp_engine_active": is_live,
        "active_session": active_session,
        "db_backend": backend,
    }


def _query_deleted_files(conn, backend):
    cursor = conn.cursor()
    cursor.execute(
        """
        SELECT id, timestamp, file_name,
               COALESCE(event_type, 'DELETED') AS event_type,
               COALESCE(is_dir, 0) AS is_dir,
               new_name, session_id
        FROM deleted_files
        ORDER BY id DESC LIMIT 50;
        """
    )
    rows = cursor.fetchall()
    items = [
        {
            "id": r["id"],
            "timestamp": str(r["timestamp"]),
            "file_name": r["file_name"],
            "event_type": r["event_type"],
            "is_dir": bool(r["is_dir"]),
            "new_name": r["new_name"],
            "session_id": r["session_id"],
        }
        for r in rows
    ]

    # skip full COUNT(*) scan whenever fewer than 50 rows exist or newest id hasn't changed
    if len(items) < 50:
        total_count = len(items)
    else:
        cache_key = (backend, items[0]["id"], items[-1]["id"])
        with _state_lock:
            cached = _deletions_count_cache["count"] if _deletions_count_cache["key"] == cache_key else None
        if cached is not None:
            total_count = cached
        else:
            cursor.execute("SELECT COUNT(*) AS cnt FROM deleted_files;")
            r_cnt = cursor.fetchone()
            total_count = r_cnt["cnt"] if isinstance(r_cnt, dict) else r_cnt[0]
            with _state_lock:
                _deletions_count_cache["key"] = cache_key
                _deletions_count_cache["count"] = total_count

    return {"items": items, "total_count": total_count, "db_backend": backend}


def _query_alerts(conn, backend):
    cursor = conn.cursor()
    cursor.execute(
        """
        SELECT id, timestamp, metric, value, threshold, state, session_id
        FROM alerts
        ORDER BY id DESC LIMIT 20;
        """
    )
    rows = cursor.fetchall()
    items = []
    seen_metrics = set()
    active_count = 0

    for r in rows:
        metric = r["metric"]
        state = r["state"]
        if metric not in seen_metrics:
            seen_metrics.add(metric)
            if state == "TRIGGERED":
                active_count += 1
        items.append(
            {
                "id": r["id"],
                "timestamp": str(r["timestamp"]),
                "metric": metric,
                "value": round(float(r["value"]), 1),
                "threshold": round(float(r["threshold"]), 1),
                "state": state,
                "session_id": r["session_id"],
            }
        )

    # only query older alert rows if table has >= 20 rows and not all 3 monitored metrics appeared yet
    if len(rows) == 20 and not {"cpu_usage", "ram_usage", "cpu_temp"}.issubset(seen_metrics):
        try:
            cursor.execute(
                """
                SELECT COUNT(*) AS cnt
                FROM alerts a
                INNER JOIN (
                    SELECT MAX(id) AS max_id FROM alerts GROUP BY metric
                ) latest ON a.id = latest.max_id
                WHERE a.state = 'TRIGGERED';
                """
            )
            r_act = cursor.fetchone()
            active_count = r_act["cnt"] if isinstance(r_act, dict) else r_act[0]
        except Exception:
            pass

    return {"items": items, "active_count": active_count, "db_backend": backend}


def _query_sessions(conn, backend):
    cursor = conn.cursor()
    cursor.execute(
        """
        SELECT id, start_time, end_time, pid, hostname, clean_exit
        FROM sessions
        ORDER BY id DESC LIMIT 10;
        """
    )
    rows = cursor.fetchall()
    items = [
        {
            "id": r["id"],
            "start_time": str(r["start_time"]),
            "end_time": str(r["end_time"]) if r["end_time"] else None,
            "pid": r["pid"],
            "hostname": r["hostname"],
            "clean_exit": r["clean_exit"],  # 1 = clean, 0 = crashed, null = running
        }
        for r in rows
    ]
    has_crashed = any(s["clean_exit"] == 0 for s in items)
    return {"items": items, "has_crashed": has_crashed, "db_backend": backend}


def _query_history(conn, backend, minutes=10):
    minutes = max(1, min(int(minutes), 120))
    if backend == "MySQL":
        # use tuple cursor to avoid allocating thousands of dicts and compare VARCHAR timestamp against formatted string for B-tree range scan
        cursor = conn.cursor(pymysql.cursors.Cursor)
        cursor.execute(
            """
            SELECT timestamp, cpu_usage, ram_usage, cpu_temp, fan_speed
            FROM vitals_log
            WHERE timestamp >= DATE_FORMAT(DATE_SUB(NOW(), INTERVAL %s MINUTE), '%%Y-%%m-%%d %%H:%%i:%%s')
            ORDER BY timestamp ASC, id ASC;
            """,
            (minutes,),
        )
        rows = cursor.fetchall()
    else:
        cursor = conn.cursor()
        cursor.execute(
            """
            SELECT timestamp, cpu_usage, ram_usage, cpu_temp, fan_speed
            FROM vitals_log
            WHERE timestamp >= datetime('now', 'localtime', ?)
            ORDER BY timestamp ASC, id ASC;
            """,
            (f"-{minutes} minutes",),
        )
        rows = cursor.fetchall()

    total_rows = len(rows)

    # downsample to roughly 50 points if dataset is large
    target_pts = 50
    if total_rows > target_pts:
        step = total_rows / target_pts
        selected = [rows[int(i * step)] for i in range(target_pts)]
        if selected[-1] is not rows[-1]:
            selected.append(rows[-1])
        rows = selected

    points = []
    for r in rows:
        ts_str = str(r[0])
        points.append(
            {
                "t": ts_str.split(" ")[-1] if " " in ts_str else ts_str,
                "cpu": round(float(r[1]), 1),
                "ram": round(float(r[2]), 1),
                "temp": round(float(r[3]), 1) if r[3] is not None else 0.0,
                "fan": int(r[4]) if r[4] is not None else 0,
            }
        )
    return {"points": points, "db_backend": backend}


# gets latest hardware vitals reading and checks if daemon is actively logging
def get_latest_vitals_from_db():
    def _fetch():
        try:
            with borrow_db() as (conn, backend):
                if not conn:
                    return _disconnected_vitals()
                return _query_latest_vitals(conn, backend)
        except Exception as e:
            return {
                "cpu_usage": 0.0,
                "ram_usage": 0.0,
                "cpu_temp": 0.0,
                "fan_speed": 0,
                "timestamp": "DB Read Error",
                "cpp_engine_active": False,
                "db_backend": "Error",
                "error": str(e),
            }

    return _cached_call("metrics", 0.25, _fetch)


# gets recent file audit events from inotify along with total count
def get_deleted_files_from_db():
    def _fetch():
        try:
            with borrow_db() as (conn, backend):
                if not conn:
                    return {"items": [], "total_count": 0, "db_backend": "None"}
                return _query_deleted_files(conn, backend)
        except Exception:
            return {"items": [], "total_count": 0, "db_backend": "Error"}

    return _cached_call("deletions", 0.6, _fetch)


# gets recent threshold alerts and counts currently active triggers
def get_alerts_from_db():
    def _fetch():
        try:
            with borrow_db() as (conn, backend):
                if not conn:
                    return {"items": [], "active_count": 0, "db_backend": "None"}
                return _query_alerts(conn, backend)
        except Exception:
            return {"items": [], "active_count": 0, "db_backend": "Error"}

    return _cached_call("alerts", 0.8, _fetch)


# pulls session log history to display daemon runs and crash status
def get_sessions_from_db():
    def _fetch():
        try:
            with borrow_db() as (conn, backend):
                if not conn:
                    return {"items": [], "has_crashed": False, "db_backend": "None"}
                return _query_sessions(conn, backend)
        except Exception:
            return {"items": [], "has_crashed": False, "db_backend": "Error"}

    return _cached_call("sessions", 1.0, _fetch)


# gets historical vitals sampled down for sparkline charts
def get_history_from_db(minutes=10):
    minutes = max(1, min(int(minutes), 120))

    def _fetch():
        try:
            with borrow_db() as (conn, backend):
                if not conn:
                    return {"points": [], "db_backend": "None"}
                return _query_history(conn, backend, minutes)
        except Exception:
            return {"points": [], "db_backend": "Error"}

    return _cached_call(f"history:{minutes}", 0.8, _fetch)


# unified snapshot fetching dashboard sections on a single borrowed connection
def get_dashboard_snapshot(full=True, minutes=15):
    minutes = max(1, min(int(minutes), 120))
    if not full:
        return {"metrics": get_latest_vitals_from_db()}

    def _fetch_full():
        try:
            with borrow_db() as (conn, backend):
                if not conn:
                    return {
                        "metrics": _disconnected_vitals(),
                        "deletions": {"items": [], "total_count": 0, "db_backend": "None"},
                        "alerts": {"items": [], "active_count": 0, "db_backend": "None"},
                        "sessions": {"items": [], "has_crashed": False, "db_backend": "None"},
                        "history": {"points": [], "db_backend": "None"},
                    }
                return {
                    "metrics": _query_latest_vitals(conn, backend),
                    "deletions": _query_deleted_files(conn, backend),
                    "alerts": _query_alerts(conn, backend),
                    "sessions": _query_sessions(conn, backend),
                    "history": _query_history(conn, backend, minutes),
                }
        except Exception as e:
            return {
                "metrics": {
                    "cpu_usage": 0.0,
                    "ram_usage": 0.0,
                    "cpu_temp": 0.0,
                    "fan_speed": 0,
                    "timestamp": "DB Read Error",
                    "cpp_engine_active": False,
                    "db_backend": "Error",
                    "error": str(e),
                },
                "deletions": {"items": [], "total_count": 0, "db_backend": "Error"},
                "alerts": {"items": [], "active_count": 0, "db_backend": "Error"},
                "sessions": {"items": [], "has_crashed": False, "db_backend": "Error"},
                "history": {"points": [], "db_backend": "Error"},
            }

    return _cached_call(f"snapshot:full:{minutes}", 0.4, _fetch_full)


# threaded http server with socket reuse and daemon worker threads for instant Ctrl+C exit
class BlackboxHTTPServer(http.server.ThreadingHTTPServer):
    daemon_threads = True
    allow_reuse_address = True


# http request handler routing rest endpoints and serving static assets over HTTP/1.1 keep-alive
class BlackboxHttpHandler(http.server.SimpleHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def __init__(self, *args, **kwargs):
        super().__init__(*args, directory=str(STATIC_DIR), **kwargs)

    # route api endpoints to their json generators
    def do_GET(self):
        parsed = urllib.parse.urlparse(self.path)
        path = parsed.path
        query = urllib.parse.parse_qs(parsed.query)

        if path == "/api/snapshot":
            full = query.get("full", ["1"])[0] not in ("0", "false", "False")
            mins = query.get("minutes", [15])[0]
            self.send_json(get_dashboard_snapshot(full=full, minutes=mins))
            return

        if path in ("/api/metrics", "/api/data", "/api/data/"):
            self.send_json(get_latest_vitals_from_db())
            return

        if path == "/api/deletions":
            self.send_json(get_deleted_files_from_db())
            return

        if path == "/api/alerts":
            self.send_json(get_alerts_from_db())
            return

        if path == "/api/sessions":
            self.send_json(get_sessions_from_db())
            return

        if path == "/api/history":
            mins = query.get("minutes", [10])[0]
            self.send_json(get_history_from_db(mins))
            return

        if path in ("/", ""):
            self.path = "/index.html"
        return super().do_GET()

    # helper to serialize dictionary and write compact json response headers
    def send_json(self, data, status=200):
        body = json.dumps(data, separators=(",", ":")).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Access-Control-Allow-Origin", "*")
        self.end_headers()
        self.wfile.write(body)

    # ensure static assets (html/css/js) and api responses are never served stale from browser disk cache
    def end_headers(self):
        self.send_header("Cache-Control", "no-store, no-cache, must-revalidate, max-age=0")
        self.send_header("Pragma", "no-cache")
        self.send_header("Expires", "0")
        super().end_headers()

    # silence default request logging to keep terminal tidy
    def log_message(self, format, *args):
        pass


# starts http server, attempting nearby ports if default port is occupied
def run_server(host=DEFAULT_HOST, port=DEFAULT_PORT):
    if not _config_loaded:
        load_config()
    for p in range(port, port + 10):
        try:
            server = BlackboxHTTPServer((host, p), BlackboxHttpHandler)
            print("=" * 64)
            print("  System Blackbox Web Dashboard")
            print(f"  URL:                    http://localhost:{p}")
            print(f"  Primary Backend:        {CONFIG['db_backend'].upper()}")
            print(f"  MySQL Server:           {CONFIG['mysql_host']}:{CONFIG['mysql_port']}")
            print(f"  SQLite Fallback:        {CONFIG['sqlite_path']}")
            print("=" * 64)
            try:
                server.serve_forever()
            except KeyboardInterrupt:
                pass
            finally:
                server.server_close()
                _clear_db_pool()
            return
        except OSError as e:
            if e.errno == 98:  # address in use
                continue
            raise


# parse cli arguments and start web dashboard server
def main():
    parser = argparse.ArgumentParser(description="System Blackbox Telemetry Web Server")
    parser.add_argument("--backend", type=str, choices=["mysql", "sqlite"], help="Force database backend")
    parser.add_argument("--port", type=int, default=DEFAULT_PORT, help="Port to listen on (default: 8080)")
    parser.add_argument("--host", type=str, default=DEFAULT_HOST, help="Host to bind to (default: 0.0.0.0)")
    args = parser.parse_args()

    load_config()
    if args.backend:
        CONFIG["db_backend"] = args.backend

    run_server(args.host, args.port)


if __name__ == "__main__":
    main()
