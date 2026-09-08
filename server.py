#!/usr/bin/env python3
"""
server.py — System Blackbox Web Telemetry Server
Exclusively reads metrics from blackbox.db (populated by the native C++ engine).
Zero external dependencies.
"""

import http.server
import json
import os
import sqlite3
import sys
import urllib.parse
from pathlib import Path

PORT = 8080
DB_FILE = Path(__file__).resolve().parent / "blackbox.db"
STATIC_DIR = Path(__file__).resolve().parent / "static"


def get_latest_vitals_from_db():
    """
    Query the latest system vitals record written by the C++ engine to SQLite.
    WAL mode ensures we never block the C++ daemon's concurrent writes.
    """
    if not DB_FILE.exists():
        return {
            "cpu_usage": 0.0,
            "ram_usage": 0.0,
            "cpu_temp": 0.0,
            "fan_speed": 0,
            "timestamp": "DB Not Found",
            "cpp_engine_active": False,
            "message": "Waiting for C++ system_blackbox daemon to start...",
        }

    try:
        uri = f"file:{DB_FILE.resolve()}?mode=ro"
        with sqlite3.connect(uri, uri=True, timeout=1.0) as conn:
            cursor = conn.cursor()
            cursor.execute(
                """
                SELECT cpu_usage, ram_usage, cpu_temp, fan_speed, timestamp 
                FROM vitals_log 
                ORDER BY id DESC LIMIT 1;
                """
            )
            row = cursor.fetchone()
            if row:
                return {
                    "cpu_usage": round(row[0], 1),
                    "ram_usage": round(row[1], 1),
                    "cpu_temp": round(row[2], 1),
                    "fan_speed": int(row[3]),
                    "timestamp": row[4],
                    "cpp_engine_active": True,
                    "db_source": "blackbox.db",
                }
    except Exception as e:
        return {
            "cpu_usage": 0.0,
            "ram_usage": 0.0,
            "cpu_temp": 0.0,
            "fan_speed": 0,
            "timestamp": "DB Read Error",
            "cpp_engine_active": False,
            "error": str(e),
        }

    return {
        "cpu_usage": 0.0,
        "ram_usage": 0.0,
        "cpu_temp": 0.0,
        "fan_speed": 0,
        "timestamp": "No Data Yet",
        "cpp_engine_active": False,
        "message": "blackbox.db is empty. Run ./system_blackbox",
    }


def get_deleted_files_from_db():
    """Query recent file deletion forensic events recorded by C++ inotify."""
    if not DB_FILE.exists():
        return []

    try:
        uri = f"file:{DB_FILE.resolve()}?mode=ro"
        with sqlite3.connect(uri, uri=True, timeout=1.0) as conn:
            cursor = conn.cursor()
            cursor.execute(
                """
                SELECT id, timestamp, file_name 
                FROM deleted_files 
                ORDER BY id DESC LIMIT 20;
                """
            )
            rows = cursor.fetchall()
            return [
                {"id": r[0], "timestamp": r[1], "file_name": r[2]}
                for r in rows
            ]
    except Exception:
        return []


class PrimitiveVitalsHandler(http.server.SimpleHTTPRequestHandler):
    def __init__(self, *args, **kwargs):
        super().__init__(*args, directory=str(STATIC_DIR), **kwargs)

    def do_GET(self):
        parsed = urllib.parse.urlparse(self.path)
        path = parsed.path

        if path in ("/api/metrics", "/api/data", "/api/data/"):
            data = get_latest_vitals_from_db()
            body = json.dumps(data, indent=2).encode("utf-8")
            self.send_response(200)
            self.send_header("Content-Type", "application/json; charset=utf-8")
            self.send_header("Content-Length", str(len(body)))
            self.send_header("Access-Control-Allow-Origin", "*")
            self.send_header("Cache-Control", "no-store, no-cache, must-revalidate")
            self.end_headers()
            self.wfile.write(body)
            return

        if path == "/api/deletions":
            data = get_deleted_files_from_db()
            body = json.dumps(data, indent=2).encode("utf-8")
            self.send_response(200)
            self.send_header("Content-Type", "application/json; charset=utf-8")
            self.send_header("Content-Length", str(len(body)))
            self.send_header("Access-Control-Allow-Origin", "*")
            self.send_header("Cache-Control", "no-store, no-cache, must-revalidate")
            self.end_headers()
            self.wfile.write(body)
            return

        if path in ("/", ""):
            self.path = "/index.html"
        return super().do_GET()

    def log_message(self, format, *args):
        # Keep server console clean
        pass


def run_server(port=PORT):
    for p in range(port, port + 10):
        try:
            server = http.server.ThreadingHTTPServer(("0.0.0.0", p), PrimitiveVitalsHandler)
            print("=" * 60)
            print(f"  System Blackbox Web Dashboard: http://localhost:{p}")
            print(f"  Reading live vitals from:     {DB_FILE.name}")
            print("=" * 60)
            try:
                server.serve_forever()
            except KeyboardInterrupt:
                server.server_close()
            return
        except OSError as e:
            if e.errno == 98:
                continue
            raise


if __name__ == "__main__":
    port = PORT
    if len(sys.argv) > 1 and sys.argv[1].isdigit():
        port = int(sys.argv[1])
    run_server(port)
