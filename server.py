#!/usr/bin/env python3
# web telemetry server exposing rest endpoints for dashboard

import argparse
import datetime
import http.server
import json
import os
import sqlite3
import sys
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
}


# parse key = value settings from blackbox.conf
def load_config():
    if not CONFIG_FILE.exists():
        return
    try:
        with open(CONFIG_FILE, "r", encoding="utf-8") as f:
            for line in f:
                line = line.strip()
                if not line or line.startswith("#"):
                    continue
                if "=" in line:
                    k, v = line.split("=", 1)
                    k = k.strip()
                    v = v.strip()
                    if k in CONFIG:
                        if k == "mysql_port":
                            CONFIG[k] = int(v)
                        else:
                            CONFIG[k] = v
    except Exception as e:
        print(f"[SERVER WARNING] Error reading {CONFIG_FILE}: {e}")


# connects to mysql if available, else falls back to sqlite; caller closes connection
def get_db():
    # 1. try mysql connection if enabled
    if CONFIG["db_backend"] != "sqlite" and HAVE_PYMYSQL:
        # try reader credentials first, then fall back to writer
        users_to_try = [
            (CONFIG["mysql_reader_user"], CONFIG["mysql_reader_password"]),
            (CONFIG["mysql_user"], CONFIG["mysql_password"]),
        ]
        for user, pwd in users_to_try:
            try:
                conn = pymysql.connect(
                    host=CONFIG["mysql_host"],
                    port=int(CONFIG["mysql_port"]),
                    user=user,
                    password=pwd,
                    database=CONFIG["mysql_database"],
                    cursorclass=pymysql.cursors.DictCursor,
                    connect_timeout=1.5,
                    read_timeout=1.5,
                    autocommit=True,
                )
                return conn, "MySQL"
            except Exception:
                continue

    # 2. sqlite fallback
    sqlite_path = BASE_DIR / CONFIG["sqlite_path"]
    if sqlite_path.exists():
        try:
            uri = f"file:{sqlite_path.resolve()}?mode=ro"
            conn = sqlite3.connect(uri, uri=True, timeout=1.5)
            conn.row_factory = sqlite3.Row
            return conn, "SQLite"
        except Exception:
            pass

    return None, "None"


# gets latest hardware vitals reading and checks if daemon is actively logging
def get_latest_vitals_from_db():
    conn, backend = get_db()
    if not conn:
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

    try:
        cursor = conn.cursor()
        if backend == "MySQL":
            cursor.execute(
                """
                SELECT cpu_usage, ram_usage, cpu_temp, fan_speed, timestamp, session_id,
                       TIMESTAMPDIFF(SECOND, timestamp, NOW()) AS age_seconds
                FROM vitals_log 
                ORDER BY id DESC LIMIT 1;
                """
            )
            row = cursor.fetchone()

            active_session = None
            try:
                cursor.execute(
                    "SELECT id, start_time, pid, hostname FROM sessions WHERE end_time IS NULL ORDER BY id DESC LIMIT 1;"
                )
                active_session = cursor.fetchone()
            except Exception:
                pass

        else:
            cursor.execute(
                """
                SELECT cpu_usage, ram_usage, cpu_temp, fan_speed, timestamp, session_id,
                       (strftime('%s', 'now', 'localtime') - strftime('%s', timestamp)) AS age_seconds
                FROM vitals_log 
                ORDER BY id DESC LIMIT 1;
                """
            )
            row = cursor.fetchone()

            active_session = None
            try:
                cursor.execute(
                    "SELECT id, start_time, pid, hostname FROM sessions WHERE end_time IS NULL ORDER BY id DESC LIMIT 1;"
                )
                s_row = cursor.fetchone()
                if s_row:
                    active_session = dict(s_row)
            except Exception:
                pass

        if row:
            age = row["age_seconds"] if row["age_seconds"] is not None else 0
            is_live = (age is not None and age <= 4)

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
        else:
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
    except Exception as e:
        return {
            "cpu_usage": 0.0,
            "ram_usage": 0.0,
            "cpu_temp": 0.0,
            "fan_speed": 0,
            "timestamp": "DB Read Error",
            "cpp_engine_active": False,
            "db_backend": backend,
            "error": str(e),
        }
    finally:
        conn.close()


# gets recent file audit events from inotify along with total count
def get_deleted_files_from_db():
    conn, backend = get_db()
    if not conn:
        return {"items": [], "total_count": 0, "db_backend": "None"}

    try:
        cursor = conn.cursor()
        cursor.execute("SELECT COUNT(*) AS cnt FROM deleted_files;")
        r_cnt = cursor.fetchone()
        total_count = r_cnt["cnt"] if isinstance(r_cnt, dict) else r_cnt[0]

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
        return {"items": items, "total_count": total_count, "db_backend": backend}
    except Exception:
        return {"items": [], "total_count": 0, "db_backend": backend}
    finally:
        conn.close()


# gets recent threshold alerts and counts currently active triggers
def get_alerts_from_db():
    conn, backend = get_db()
    if not conn:
        return {"items": [], "active_count": 0, "db_backend": "None"}

    try:
        cursor = conn.cursor()
        cursor.execute(
            """
            SELECT id, timestamp, metric, value, threshold, state, session_id
            FROM alerts
            ORDER BY id DESC LIMIT 20;
            """
        )
        rows = cursor.fetchall()
        items = [
            {
                "id": r["id"],
                "timestamp": str(r["timestamp"]),
                "metric": r["metric"],
                "value": round(float(r["value"]), 1),
                "threshold": round(float(r["threshold"]), 1),
                "state": r["state"],
                "session_id": r["session_id"],
            }
            for r in rows
        ]

        active_count = 0
        try:
            cursor.execute(
                """
                WITH RankedAlerts AS (
                    SELECT metric, state,
                           ROW_NUMBER() OVER(PARTITION BY metric ORDER BY id DESC) as rn
                    FROM alerts
                )
                SELECT COUNT(*) AS cnt FROM RankedAlerts WHERE rn = 1 AND state = 'TRIGGERED';
                """
            )
            r_act = cursor.fetchone()
            active_count = r_act["cnt"] if isinstance(r_act, dict) else r_act[0]
        except Exception:
            pass

        return {"items": items, "active_count": active_count, "db_backend": backend}
    except Exception:
        return {"items": [], "active_count": 0, "db_backend": backend}
    finally:
        conn.close()


# pulls session log history to display daemon runs and crash status
def get_sessions_from_db():
    conn, backend = get_db()
    if not conn:
        return {"items": [], "has_crashed": False, "db_backend": "None"}

    try:
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
    except Exception:
        return {"items": [], "has_crashed": False, "db_backend": backend}
    finally:
        conn.close()


# gets historical vitals sampled down for sparkline charts
def get_history_from_db(minutes=10):
    minutes = max(1, min(int(minutes), 120))
    conn, backend = get_db()
    if not conn:
        return {"points": [], "db_backend": "None"}

    try:
        cursor = conn.cursor()
        if backend == "MySQL":
            cursor.execute(
                f"""
                SELECT timestamp, cpu_usage, ram_usage, cpu_temp, fan_speed
                FROM vitals_log
                WHERE timestamp >= DATE_SUB(NOW(), INTERVAL {minutes} MINUTE)
                ORDER BY id ASC;
                """
            )
        else:
            cursor.execute(
                f"""
                SELECT timestamp, cpu_usage, ram_usage, cpu_temp, fan_speed
                FROM vitals_log
                WHERE timestamp >= datetime('now', 'localtime', '-{minutes} minutes')
                ORDER BY id ASC;
                """
            )
        rows = list(cursor.fetchall())
        total_rows = len(rows)

        # downsample to roughly 50 points if dataset is large
        target_pts = 50
        if total_rows > target_pts:
            step = total_rows / target_pts
            selected = [rows[int(i * step)] for i in range(target_pts)]
            if rows[-1] not in selected:
                selected.append(rows[-1])
            rows = selected

        points = [
            {
                "t": str(r["timestamp"]).split(" ")[-1] if " " in str(r["timestamp"]) else str(r["timestamp"]),
                "cpu": round(float(r["cpu_usage"]), 1),
                "ram": round(float(r["ram_usage"]), 1),
                "temp": round(float(r["cpu_temp"]), 1) if r["cpu_temp"] is not None else 0.0,
                "fan": int(r["fan_speed"]) if r["fan_speed"] is not None else 0,
            }
            for r in rows
        ]
        return {"points": points, "db_backend": backend}
    except Exception:
        return {"points": [], "db_backend": backend}
    finally:
        conn.close()


# http request handler routing rest endpoints and serving static assets
class BlackboxHttpHandler(http.server.SimpleHTTPRequestHandler):
    def __init__(self, *args, **kwargs):
        super().__init__(*args, directory=str(STATIC_DIR), **kwargs)

    # route api endpoints to their json generators
    def do_GET(self):
        parsed = urllib.parse.urlparse(self.path)
        path = parsed.path
        query = urllib.parse.parse_qs(parsed.query)

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

    # helper to serialize dictionary and write json response headers
    def send_json(self, data, status=200):
        body = json.dumps(data, indent=2).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Access-Control-Allow-Origin", "*")
        self.send_header("Cache-Control", "no-store, no-cache, must-revalidate")
        self.end_headers()
        self.wfile.write(body)

    # silence default request logging to keep terminal tidy
    def log_message(self, format, *args):
        pass


# starts http server, attempting nearby ports if default port is occupied
def run_server(host=DEFAULT_HOST, port=DEFAULT_PORT):
    load_config()
    for p in range(port, port + 10):
        try:
            server = http.server.ThreadingHTTPServer((host, p), BlackboxHttpHandler)
            print("=" * 64)
            print("  System Blackbox Web Dashboard (Phase 2 - 50% PBL Milestone)")
            print(f"  URL:                    http://localhost:{p}")
            print(f"  Primary Backend:        {CONFIG['db_backend'].upper()}")
            print(f"  MySQL Server:           {CONFIG['mysql_host']}:{CONFIG['mysql_port']}")
            print(f"  SQLite Fallback:        {CONFIG['sqlite_path']}")
            print("=" * 64)
            try:
                server.serve_forever()
            except KeyboardInterrupt:
                server.server_close()
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
