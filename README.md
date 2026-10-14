# System Blackbox — OS Telemetry & Forensic Kernel Auditor

A Project-Based Learning (PBL) system integrating low-level **Operating Systems** concepts with **Database Management Systems (DBMS)** architecture.

---

## Architecture Overview

```
   ┌─────────────────────────────────────────────────────────────┐
   │                  C++ NATIVE ENGINE                          │
   │                                                             │
   │  [Thread 1: Sensor & Alerts]   [Thread 2: Inotify Worker]   │
   │  • /proc/stat   (CPU ticks)    • inotify_init1() syscall    │
   │  • /proc/meminfo (RAM usage)   • Watches ./test_watch/      │
   │  • coretemp hwmon (CPU °C)     • Deletions / Moves / Renames│
   │  • Threshold hysteresis alerts • Cookie-based rename pairing│
   │              │                              │               │
   │              └──────────────┬───────────────┘               │
   │                             ▼                               │
   │                [DatabaseManager (std::mutex)]               │
   │                • MySQL Client API (libmariadb / mysql.h)    │
   │                • SQLite WAL Fallback Engine                 │
   │                • Session Tracking & Crash Detection         │
   │                • Schema Verification & Indexing             │
   └─────────────────────────────┬───────────────────────────────┘
                                 │
                     Concurrent Writes (MySQL / WAL)
                                 ▼
                     [ MySQL (blackbox) / SQLite ]
                                 ▲
                     Concurrent Reads (Read-Only User)
                                 │
   ┌─────────────────────────────┴───────────────────────────────┐
   │               PYTHON WEB SERVER (server.py)                 │
   │  • Reads live vitals, alerts, sessions, downsampled history │
   │  • Connects via pymysql (reader role) or sqlite3            │
   │  • Real-time daemon liveness validation (no stale status)   │
   │  • Hosts Dark-Mode Web Dashboard with Sparkline Mini-Charts │
   └─────────────────────────────────────────────────────────────┘
```

---

## Academic Concepts Demonstrated

### Operating Systems (OS)
* **Linux Virtual Filesystems (`/proc` & `/sys`)**: Parsing tick counters from `/proc/stat`, virtual memory accounting from `/proc/meminfo`, and hardware drivers (Intel `coretemp` / AMD `k10temp`) via `/sys/class/hwmon`.
* **Kernel Syscalls (`inotify` & `poll`)**: Using `inotify_init1(IN_NONBLOCK | IN_CLOEXEC)` to trap real-time file deletions (`IN_DELETE`), moves (`IN_MOVED_FROM`), and pairing cookie events with `IN_MOVED_TO` to audit directory renames without polling overhead.
* **Crash & Session Detection**: Each daemon execution registers a kernel PID and session row. If power is lost or the process is killed (`kill -9`), the subsequent run flags the unclosed session, pinpointing the exact timestamp of termination.
* **POSIX Multithreading & Synchronization**: Concurrent worker threads (`std::thread`) synchronized via `std::mutex` and RAII `std::lock_guard` critical sections.
* **Signal Safety**: Using `sigaction` with atomic state flags (`std::atomic<bool>`) ensuring async-signal-safe termination.

### Database Management Systems (DBMS)
* **Relational Database Engine (MySQL / MariaDB)**: Full client-server database architecture via C client API (`mysql.h` / `libmariadb`) with ACID guarantees.
* **Principle of Least Privilege**: Distinct MySQL user accounts (`blackbox_writer` for the C++ engine, `blackbox_reader` with `SELECT`-only privileges for the web UI).
* **Write-Ahead Logging (WAL Fallback)**: SQLite WAL mode (`PRAGMA journal_mode = WAL;`) allowing concurrent non-blocking reads and writes.
* **Index-Assisted Time-Series Queries**: Multi-column B-tree indexes (`idx_vitals_ts`, `idx_vitals_session`, `idx_files_ts`, `idx_alerts_ts`) for high-throughput dashboard telemetry and aggregation.
* **Automated Data Retention**: Automated purging of records exceeding configured retention thresholds using SQL date functions (`DATE_SUB(NOW(), INTERVAL N DAY)`).

---

## Repository Structure

```
.
├── blackbox.conf       # Database credentials, ports, intervals, and thresholds
├── Makefile            # Build orchestration (auto-deps, MySQL & SQLite flags, setup-db)
├── alerts.hpp          # Threshold alert monitor with sustain & hysteresis
├── sensors.hpp         # Hardware telemetry interface
├── sensors.cpp         # Low-level OS parser with coretemp sensor autodetection
├── db.hpp              # Database manager interface (MySQL + SQLite fallback)
├── db.cpp              # Dual database implementation (MySQL C API + SQLite WAL)
├── main.cpp            # Multithreaded daemon, crash detection, inotify auditor
├── server.py           # Python web server with REST endpoints (pymysql + sqlite3)
├── sql/
│   ├── setup.sql       # One-time MySQL admin setup script (creates DB & users)
│   └── schema.sql      # Standard table DDL & index definitions (MySQL / Postgres)
├── static/
│   ├── index.html      # Responsive dashboard with sparklines & forensic tabs
│   ├── style.css       # Clean dark-mode styles & badge pills
│   └── app.js          # Polling client rendering gauges, sparklines, and tables
└── test_watch/         # Watched directory for inotify file events
```

---

## Quick Start

### 1. Set Up the Database (MySQL / MariaDB)
```bash
# Start MySQL / MariaDB service
sudo systemctl enable --now mariadb

# Run one-time setup (creates database & users)
sudo mariadb < sql/setup.sql
```
*(If MySQL is not installed, the engine will automatically fall back to `blackbox.db` via SQLite!)*

### 2. Build the C++ Engine
```bash
make
```

### 3. Run the System Blackbox Daemon
```bash
./system_blackbox
```
*Optional flags:*
* `--backend [mysql|sqlite]` : Database engine to use
* `--interval 1` : Sampling interval in seconds (default: 1)
* `--retention-days 7` : Purge vitals older than N days (default: 7)
* `--cpu-alert 90` : CPU % alert threshold
* `--ram-alert 90` : RAM % alert threshold
* `--temp-alert 85` : Temp °C alert threshold
* `--quiet` : Log alerts and file events only

### 4. Launch the Web Dashboard
```bash
make ui
```
Open your browser at: **`http://localhost:8080`**

### 5. Test Forensic Auditing & Crash Detection
* **Test file deletion / rename:**
  ```bash
  touch test_watch/secret.txt
  mv test_watch/secret.txt test_watch/renamed.txt
  rm test_watch/renamed.txt
  ```
* **Test crash detection:**
  Kill the running daemon forcefully:
  ```bash
  kill -9 $(pgrep system_blackbox)
  ```
  Restart `./system_blackbox` — the engine and dashboard will immediately flag:
  `[WARNING] Previous session #X ended UNEXPECTEDLY. Last recorded data: ...`

---

## Project Milestones & Roadmap

- [x] **Milestone 1: Database Engine & MySQL Integration (Phase 1 & 2 - 50% Complete)**
- [x] **Milestone 2: Low-Level Virtual Filesystem Readers & Coretemp Sensors (Phase 2 - 50% Complete)**
- [x] **Milestone 3: Kernel Inotify File & Directory Auditing (Phase 2 - 50% Complete)**
- [x] **Milestone 4: Crash Detection via Session Lifecycle (Phase 2 - 50% Complete)**
- [x] **Milestone 5: Hysteresis Threshold Alert Engine & Data Retention (Phase 2 - 50% Complete)**
- [ ] **Milestone 6: Per-Process Snapshotting at Alert Trigger Time (`/proc/[pid]/stat`) (Phase 3 - 75%)**
- [ ] **Milestone 7: Recursive Directory Watch Tree (Phase 3 - 75%)**
- [ ] **Milestone 8: Forensic Incident PDF/CSV Export & Systemd Integration (Phase 4 - 100%)**
