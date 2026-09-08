# System Blackbox — OS Telemetry & Forensic Kernel Auditor

A Project-Based Learning (PBL) system integrating low-level **Operating Systems** concepts with **Database Management Systems (DBMS)** architecture.

---

## 📌 Architecture Overview

```
   ┌─────────────────────────────────────────────────────────────┐
   │                  C++ NATIVE ENGINE                          │
   │                                                             │
   │  [Thread 1: Sensor Worker]     [Thread 2: Inotify Worker]   │
   │  • /proc/stat   (CPU ticks)    • inotify_init1() syscall    │
   │  • /proc/meminfo (RAM usage)   • Watches ./test_watch/      │
   │  • /sys/class/hwmon (Temp/RPM) • Detects file deletions     │
   │              │                              │               │
   │              └──────────────┬───────────────┘               │
   │                             ▼                               │
   │                [DatabaseManager (std::mutex)]               │
   │                • Prepared Statements                        │
   │                • SQLite C Engine Interface                  │
   └─────────────────────────────┬───────────────────────────────┘
                                 │
                     Concurrent Writes (WAL Mode)
                                 ▼
                         [ blackbox.db ]
                                 ▲
                     Concurrent Reads (Read-Only)
                                 │
   ┌─────────────────────────────┴───────────────────────────────┐
   │               PYTHON WEB SERVER (server.py)                 │
   │  • Reads latest vitals & audit logs from SQLite             │
   │  • Serves REST API (/api/metrics, /api/deletions)           │
   │  • Hosts Dark-Mode Web Dashboard (HTML5 / CSS3 / Vanilla JS)│
   └─────────────────────────────────────────────────────────────┘
```

---

## 🎯 Academic Concepts Demonstrated

### Operating Systems (OS)
* **Linux Virtual Filesystems (`/proc` & `/sys`)**: Directly parsing kernel tick counters from `/proc/stat`, memory layout from `/proc/meminfo`, and sysfs hardware thermal/fan inputs from `/sys/class/hwmon`.
* **Kernel Syscalls (`inotify`)**: Utilizing `inotify_init1(IN_NONBLOCK)` and `inotify_add_watch` with `poll()` to trap real-time file deletion events (`IN_DELETE`, `IN_MOVED_FROM`).
* **POSIX Multithreading**: Spawning true concurrent worker threads (`std::thread`) running simultaneously across CPU cores.
* **Thread Synchronization**: Using `std::mutex` and `std::lock_guard` critical sections to prevent race conditions during concurrent database writes.
* **RAII & Signal Handling**: Graceful shutdown on `SIGINT`/`SIGTERM` ensuring clean thread joins and resource release.

### Database Management Systems (DBMS)
* **SQLite C API**: Direct low-level integration with `<sqlite3.h>`, managing raw database connections and prepared statements (`sqlite3_prepare_v2`, `sqlite3_bind_*`).
* **Write-Ahead Logging (WAL)**: Configured with `PRAGMA journal_mode = WAL;` and `PRAGMA synchronous = NORMAL;`. Ensures concurrent readers (Python web server) never block writers (C++ daemon), and writers never block readers.
* **ACID Guarantees**: Atomic execution of hardware telemetry insertions and forensic audit logging.

---

## 📁 Repository Structure

```
.
├── Makefile            # Build orchestration (compilation & UI runner)
├── sensors.hpp         # Hardware telemetry interface
├── sensors.cpp         # Low-level OS parser (/proc & /sys)
├── db.hpp              # SQLite database manager interface (thread-safe WAL)
├── db.cpp              # Database implementation with prepared statements
├── main.cpp            # Multithreaded daemon & inotify watcher
├── server.py           # Python telemetry server (queries blackbox.db)
├── static/
│   ├── index.html      # Modern dashboard with live gauges & audit table
│   ├── style.css       # Clean dark-mode styles & micro-animations
│   └── app.js          # Polling client updating UI via SQLite endpoints
└── test_watch/         # Watched directory for inotify file deletion events
```

---

## 🚀 Quick Start

### 1. Prerequisites (Ubuntu / Debian)
```bash
sudo apt update
sudo apt install -y build-essential libsqlite3-dev python3 sqlite3
```

### 2. Build the C++ Engine
```bash
make
```

### 3. Run the System Blackbox (Terminal 1)
```bash
./system_blackbox
```
*The engine will start sampling OS vitals every 1 second and watch `./test_watch/` for deletion events.*

### 4. Launch the Web Dashboard (Terminal 2)
```bash
make ui
```
Open the URL shown in the terminal (default: `http://localhost:8080` or `http://localhost:8081`).

### 5. Test File Deletion Auditing (Terminal 3)
In another terminal, create and delete a file inside `test_watch`:
```bash
touch test_watch/classified_data.txt
rm test_watch/classified_data.txt
```
* The C++ engine logs an `[ALERT][INOTIFY]` to the terminal and records it in `blackbox.db`.
* The web dashboard immediately displays the deletion event in the **Kernel inotify Deletion Audits** table.

---

## 🔍 Inspecting the Database Manually
You can query the SQLite database at any time:
```bash
# View recent system vitals
sqlite3 -box blackbox.db "SELECT * FROM vitals_log ORDER BY id DESC LIMIT 5;"

# View file deletion audits
sqlite3 -box blackbox.db "SELECT * FROM deleted_files;"
```
