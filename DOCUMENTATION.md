# System Blackbox — Project Documentation (PBL 50% Milestone)

> **Course:** Operating Systems (OS) & Database Management Systems (DBMS)  
> **Project Title:** System Blackbox — Native Kernel Telemetry & Forensic Relational Auditor  
> **Milestone Status:** **50% Completed (Phase 2 Submission)**  
> **Language & Stack:** Modern C++23 (OS Layer) + MySQL / MariaDB & SQLite (DBMS Layer) + Python/HTML5 Dashboard

---

## Table of Contents
1. [What is This Project? (Explained in Very Simple Words)](#1-what-is-this-project-in-very-simple-words)
2. [Why Did We Build It? (OS + DBMS Integration)](#2-why-did-we-build-it)
3. [Exact Steps to Store Data in MySQL (or PostgreSQL)](#3-exact-steps-to-store-data-in-mysql-or-postgresql)
4. [50% PBL Milestone Breakdown (What is Done?)](#4-50-pbl-milestone-breakdown)
5. [File Map (What Every Single File Does in Plain English)](#5-file-map)
6. [Detailed Function-by-Function Breakdown](#6-detailed-function-by-function-breakdown)
   - [Hardware Telemetry (`sensors.hpp` & `sensors.cpp`)](#61-hardware-telemetry-sensorshpp--sensorscpp)
   - [Threshold Alerts with Hysteresis (`alerts.hpp`)](#62-threshold-alerts-with-hysteresis-alertshpp)
   - [Database Manager & Crash Detection (`db.hpp` & `db.cpp`)](#63-database-manager--crash-detection-dbhpp--dbcpp)
   - [Multithreaded Engine & Kernel Inotify (`main.cpp`)](#64-multithreaded-engine--kernel-inotify-maincpp)
   - [Web Server & REST Endpoints (`server.py`)](#65-web-server--rest-endpoints-serverpy)
   - [Web Dashboard UI (`static/`)](#66-web-dashboard-ui-static)
7. [Step-by-Step Demo Guide (How to Present to Your Professor)](#7-step-by-step-demo-guide)
8. [Viva / Oral Exam Questions & Answers (Cheat Sheet)](#8-viva--oral-exam-questions--answers)
9. [Roadmap to 100% Completion (Phases 3 & 4)](#9-roadmap-to-100-completion)

---

## 1. What is This Project? (In Very Simple Words)

Think of an **airplane black box**:
- When an airplane flies, a black box continuously records flight instruments (altitude, engine temperature, speed) and cockpit voice communications.
- If an engine overheats or if the plane crashes, engineers open the black box to see **exactly what happened in the seconds leading up to the disaster**.

**System Blackbox does the exact same thing for your Linux computer:**
1. **Health Monitor (Telemetry):** Every second, a C++ program directly asks the Linux Operating System kernel for:
   - CPU usage percentage (%)
   - RAM usage percentage (%)
   - CPU processor temperature (°C)
   - Cooling fan rotation speed (RPM)
2. **Watchdog (File Audit):** It watches a secure folder (`test_watch/`). If anyone deletes a file, moves a file out, or secretly renames a file, the Linux kernel alerts our C++ engine immediately.
3. **Database Vault:** Every single vital sign and file deletion event is permanently saved into a relational database (**MySQL / MariaDB**, with an automatic local SQLite fallback).
4. **Crash Detective:** If your computer suddenly loses power, crashes, or someone runs `kill -9` on the blackbox process, the engine automatically detects this upon restart. It flags the unclosed session as **CRASHED** and reports the exact timestamp and hardware state right before the system died!
5. **Live Web Cockpit:** A sleek web dashboard shows live gauges, historical trend sparklines, and forensic audit logs in your web browser.

---

## 2. Why Did We Build It?

In university computer science, OS and DBMS are often taught as isolated subjects:
- In OS, you learn about processes, threads, virtual filesystems, and kernel syscalls.
- In DBMS, you learn about schemas, SQL queries, transactions, indexes, and user permissions.

**System Blackbox merges both subjects into one real-world, high-performance project:**
- The **OS layer** must be ultra-fast and low-level, which is why it is written in **C++23** communicating directly with Linux kernel data structures (`/proc` and `/sys`) and POSIX system calls (`inotify_init1`, `poll`, `sigaction`, `std::thread`, `std::mutex`).
- The **DBMS layer** must be reliable and structured, which is why data is stored in **MySQL / MariaDB** with normalized relational tables, B-tree secondary indexes, prepared statements, and user privilege separation (DCL).

---

## 3. Exact Steps to Store Data in MySQL (or PostgreSQL)

Your project is built with dual database support. By default, it connects to **MySQL / MariaDB**. If MySQL is not running or not yet configured, it automatically falls back to **SQLite (`blackbox.db`)** so the program never crashes.

Here are the exact steps to enable MySQL storage:

### Step 1: Ensure MariaDB / MySQL is Running
On your Linux machine (CachyOS / Arch / Ubuntu / Fedora), start the database service:
```bash
sudo systemctl enable --now mariadb   # On Arch / CachyOS / Fedora
# OR
sudo systemctl enable --now mysql     # On Ubuntu / Debian
```

### Step 2: Run the One-Time Setup Script
We have created a pre-configured setup script: `sql/setup.sql`.  
Execute it with one command:
```bash
sudo mariadb < sql/setup.sql
# OR
make setup-db
```

#### What does `sql/setup.sql` do?
1. Creates the database `blackbox`.
2. Creates two dedicated users following the **Principle of Least Privilege**:
   - `blackbox_writer` (password: `writer_pass`): Used by the C++ engine to create tables and insert logs.
   - `blackbox_reader` (password: `reader_pass`): Used by the Python web dashboard with **`SELECT`-only permissions**. Even if the web dashboard was compromised, an attacker cannot modify or delete your forensic logs!
3. Grants all necessary table permissions and flushes privileges.

### Step 3: Run the C++ Engine
Now simply build and run:
```bash
make
./system_blackbox
```
You will immediately see:
```text
[DBMS] Connecting to MySQL server at 127.0.0.1:3306 (user: blackbox_writer, db: blackbox)...
[DBMS] Successfully connected to MySQL database 'blackbox'.
[DBMS] MySQL tables 'sessions', 'vitals_log', 'deleted_files', 'alerts' verified.
[INIT] Database persistence engine active: MySQL
[INIT] Session #1 started (pid 12345).
```

### Step 4: Run the Web Dashboard
In another terminal:
```bash
make ui
```
Open **`http://localhost:8080`** in your browser. The status badge will display:  
`● C++ Engine Live [MySQL] (Session #1)`

---

### Alternative: What if you want to use PostgreSQL instead?
If your university lab requires PostgreSQL instead of MySQL:
1. Install PostgreSQL: `sudo pacman -S postgresql libpqxx` (or `sudo apt install postgresql libpq-dev`).
2. Run PostgreSQL init: `sudo -u postgres initdb -D /var/lib/postgres/data` and start `sudo systemctl start postgresql`.
3. Create database and user:
   ```sql
   CREATE DATABASE blackbox;
   CREATE USER blackbox_writer WITH PASSWORD 'writer_pass';
   GRANT ALL PRIVILEGES ON DATABASE blackbox TO blackbox_writer;
   ```
4. The schema provided in `sql/schema.sql` uses standard SQL types (`BIGINT`, `VARCHAR`, `DOUBLE PRECISION`, `INT`) that run directly in PostgreSQL.

---

## 4. 50% PBL Milestone Breakdown

For a 5th-semester University Project-Based Learning review, this codebase represents **Phase 2 (50% Completion Milestone)**. Here is the progress breakdown:

| Milestone | Target | Status | Concepts Demonstrated |
| :--- | :--- | :---: | :--- |
| **Milestone 1** | Relational Database & Schemas | **100% Complete** | MySQL InnoDB tables, B-tree indexes, ACID transactions, SQLite WAL fallback |
| **Milestone 2** | Kernel Virtual Filesystem Readers | **100% Complete** | Direct `/proc/stat` CPU tick parsing, `/proc/meminfo` RAM calculation, `/sys/class/hwmon` temp & fan detection |
| **Milestone 3** | Linux Kernel Inotify File Auditing | **100% Complete** | `inotify_init1`, `poll` non-blocking event loop, cookie-based rename detection |
| **Milestone 4** | Process Crash & Power Loss Detection | **100% Complete** | Session PID lifecycle, unexpected exit detection, last-known vitals snapshot |
| **Milestone 5** | Threshold Alerts & Data Retention | **100% Complete** | Sustain counter + hysteresis margin to eliminate alert spam; automated age-based data purging |
| **Milestone 6** | Per-Process Snapshotting | *Phase 3 (75%)* | Recording top CPU-consuming PIDs during an alert |
| **Milestone 7** | Recursive Watch Tree | *Phase 3 (75%)* | Watching nested subdirectories dynamically |
| **Milestone 8** | Forensic PDF/CSV Export & Systemd Unit | *Phase 4 (100%)* | One-click incident reporting & background system service |

---

## 5. File Map

Here is what every file in the project does in simple terms:

```
.
├── blackbox.conf       # Configuration file: DB credentials, port, intervals, alert thresholds
├── Makefile            # Build recipe: compiles C++ with MySQL & SQLite flags, clean, setup
├── alerts.hpp          # Smart alarm logic: uses hysteresis so alarms don't flicker on/off
├── sensors.hpp         # Sensor interface: declares functions for reading hardware metrics
├── sensors.cpp         # Low-level OS code: reads /proc/stat, /proc/meminfo, and /sys/class/hwmon
├── db.hpp              # Database header: defines DatabaseManager class supporting MySQL & SQLite
├── db.cpp              # Database logic: connects to MySQL/SQLite, creates tables, executes queries
├── main.cpp            # Project conductor: manages OS threads, inotify auditor, and signals
├── server.py           # Web server: reads the database and serves JSON endpoints to the browser
├── sql/
│   ├── setup.sql       # One-time script to create the MySQL database & least-privilege users
│   └── schema.sql      # DDL definitions of all tables and indexes for MySQL / PostgreSQL
├── static/
│   ├── index.html      # Dashboard layout: vitals cards, canvas sparklines, diagnostic tabs
│   ├── style.css       # Premium dark-mode styling, badge pills, animations
│   └── app.js          # JavaScript client: polls REST endpoints and updates charts live
└── test_watch/         # Folder monitored by inotify for file deletion & rename events
```

---

## 6. Detailed Function-by-Function Breakdown

### 6.1 Hardware Telemetry (`sensors.hpp` & `sensors.cpp`)

- **`read_proc_stat_ticks(idle_ticks, total_ticks)`**:
  - Opens `/proc/stat`. The Linux kernel exposes raw CPU time slices ("jiffies" or ticks) on the first line.
  - Reads `user`, `nice`, `system`, `idle`, `iowait`, `irq`, `softirq`, and `steal`.
  - Calculates `idle_ticks = idle + iowait`.
  - Calculates `total_ticks = user + nice + system + idle + iowait + irq + softirq + steal`.
  - *(Bug fix applied)*: In Linux, `guest` ticks are already counted inside `user`. We avoided double-counting guest ticks.
- **`read_cpu_usage()`**:
  - Compares the ticks between the previous reading and the current reading.
  - Formula: $\text{CPU \%} = \frac{\Delta \text{total} - \Delta \text{idle}}{\Delta \text{total}} \times 100$.
- **`read_ram_usage()`**:
  - Opens `/proc/meminfo`.
  - Reads `MemTotal` and `MemAvailable`.
  - Unlike naive programs that read `MemFree`, using `MemAvailable` correctly accounts for reclaimable disk caches and buffers, showing the exact memory pressure seen in `htop`.
- **`detect_cpu_temp_sensor()` & `read_cpu_temp()`**:
  - Scans `/sys/class/hwmon` looking for hardware drivers: `coretemp` (Intel), `k10temp` / `zenpower` (AMD), or `cpu_thermal` (ARM).
  - Finds the package temperature label (`Package id 0` or `Tctl`).
  - Reads the millidegree value (e.g., `54000`) and divides by 1000 to get `54.0 °C`.
- **`read_fan_speed()`**:
  - Scans all fan inputs (`fan*_input`) in `/sys/class/hwmon` and returns the highest RPM reading.

---

### 6.2 Threshold Alerts with Hysteresis (`alerts.hpp`)

- **`ThresholdMonitor::update(value)`**:
  - Evaluates whether a metric (like CPU usage > 90%) warrants an alert.
  - **Sustain Counter:** The value must remain above the threshold for **3 consecutive samples** before triggering an alert. This stops a 0.5-second CPU burst from filling the database with junk alerts.
  - **Hysteresis Margin:** Once triggered, the alert does **not** turn off the instant the metric drops to 89.9%. It requires the metric to drop below `threshold - margin` (e.g., below 80%) before marking the alert as `RESOLVED`. This prevents rapid toggling when hovering near limits.

---

### 6.3 Database Manager & Crash Detection (`db.hpp` & `db.cpp`)

- **`init(cfg)`**:
  - Reads configuration; connects to MySQL via `mysql_real_connect()`.
  - Automatically executes table creation DDL (`CREATE TABLE IF NOT EXISTS`).
  - Sets up secondary B-tree indexes (`idx_vitals_ts`, `idx_files_ts`, `idx_alerts_ts`).
  - If MySQL is not reachable, falls back to SQLite, enabling WAL mode (`PRAGMA journal_mode = WAL`) and `synchronous = NORMAL`.
- **`begin_session(pid, hostname)`**:
  - When the daemon starts, it inserts a new row into the `sessions` table: `start_time = NOW()`, `pid = getpid()`, `clean_exit = NULL`, and `end_time = NULL`.
- **`end_session()`**:
  - When you press Ctrl+C or send SIGTERM, the graceful shutdown sets `end_time = NOW()` and `clean_exit = 1`.
- **`find_open_sessions()` & `mark_session_crashed(session)`**:
  - At startup, the engine queries the database for any previous session where `end_time IS NULL`.
  - If an unclosed session is found, it means the previous run **crashed, suffered sudden power loss, or was killed via `kill -9`**!
  - It marks the session as `clean_exit = 0` (crashed) and sets its `end_time` to the timestamp of the last recorded telemetry reading.
- **`log_vitals(...)` & `log_file_event(...)` & `log_alert(...)`**:
  - Thread-safe inserts guarded by `std::lock_guard<std::mutex>`.
  - All string parameters are sanitized via `escape_mysql` or prepared statement bindings, completely eliminating SQL injection risks.
- **`purge_old_vitals(days)`**:
  - Runs a retention query deleting rows older than $N$ days (`DELETE FROM vitals_log WHERE timestamp < DATE_SUB(NOW(), INTERVAL N DAY)`), preventing disk overflow.

---

### 6.4 Multithreaded Engine & Kernel Inotify (`main.cpp`)

- **`signal_handler(signum)`**:
  - Registered via `sigaction()`. When Ctrl+C is pressed, it sets an atomic flag `g_running.store(false)`. It performs zero allocations or I/O, ensuring POSIX async-signal-safety.
- **`Thread 1 (sensor_worker)`**:
  - Wakes up every 1 second without timing drift using `std::chrono::steady_clock`.
  - Gathers CPU, RAM, temperature, and fan speed, checks threshold alerts, and inserts them into the database.
- **`Thread 2 (inotify_worker)`**:
  - Creates an inotify file descriptor: `inotify_init1(IN_NONBLOCK | IN_CLOEXEC)`.
  - Adds a kernel watch on `test_watch/` for `IN_DELETE`, `IN_MOVED_FROM`, `IN_MOVED_TO`, and `IN_DELETE_SELF`.
  - Uses the `poll()` system call with a 200ms timeout so it consumes **0% CPU** while waiting for events.
  - **Rename Detection:** When a file is renamed inside the directory, the Linux kernel generates an `IN_MOVED_FROM` event followed by an `IN_MOVED_TO` event carrying the **exact same cookie integer**. Thread 2 pairs these cookies to record an exact `RENAMED: old_name -> new_name` audit entry.

---

### 6.5 Web Server & REST Endpoints (`server.py`)

- Reads `blackbox.conf`. Connects to MySQL using the read-only credentials (`blackbox_reader`), or falls back to SQLite.
- Provides five JSON endpoints:
  - `GET /api/metrics`: Current CPU%, RAM%, Temp, Fan, active session, and active DB backend name.
  - `GET /api/deletions`: List of forensic file deletion, move, and rename events.
  - `GET /api/alerts`: List of threshold breaches and count of currently active alerts.
  - `GET /api/sessions`: List of daemon runs and crash indicators.
  - `GET /api/history?minutes=15`: Downsampled points (up to 50 samples) for smooth canvas sparkline charts.

---

### 6.6 Web Dashboard UI (`static/`)

- **`index.html`**: Clean semantic HTML structure displaying 4 vitals cards, canvas charts, and diagnostic tabs.
- **`style.css`**: Professional dark mode using JetBrains Mono and Outfit typography with CSS grid and badge animations.
- **`app.js`**: Lightweight async JavaScript client polling `/api/metrics` every 1s, `/api/deletions` every 2s, and rendering live canvas sparklines without external heavy dependencies.

---

## 7. Step-by-Step Demo Guide

Follow these steps when demonstrating the project to your evaluator or professor:

### Step 1: Compile the Project
```bash
make clean
make
```
*Show the examiner that the C++ compiler links with `-lmariadb -lsqlite3 -pthread` using modern C++23.*

### Step 2: Initialize MySQL Database
```bash
sudo mariadb < sql/setup.sql
```
*Show the examiner that `blackbox` database was created along with separate writer and reader users.*

### Step 3: Start the Daemon
```bash
./system_blackbox
```
*Show the examiner the startup banner, the active database (`[INIT] Database persistence engine active: MySQL`), the sensor autodetection (`coretemp`), and the live terminal telemetry log.*

### Step 4: Open the Web Dashboard
In a second terminal window:
```bash
make ui
```
Open **`http://localhost:8080`** in your browser.  
*Show the examiner the green live pulse, the `[MySQL]` status badge, the gauges moving in real-time, and the canvas trend sparklines.*

### Step 5: Test Forensic Inotify File Auditing
In a third terminal window:
```bash
touch test_watch/secret_file.txt
mv test_watch/secret_file.txt test_watch/renamed_file.txt
rm test_watch/renamed_file.txt
```
*Switch to the browser and click the **Inotify Audits** tab. Point out how the file rename (`secret_file.txt -> renamed_file.txt`) and subsequent deletion (`DELETED: renamed_file.txt`) appeared instantly with exact timestamps!*

### Step 6: Test Sudden Crash Detection
Kill the running daemon forcefully to simulate a sudden kernel panic or power loss:
```bash
kill -9 $(pgrep system_blackbox)
```
Now start the daemon again:
```bash
./system_blackbox
```
*Watch the terminal and dashboard. The system will immediately output:*  
`[WARNING] Previous session #X ended UNEXPECTEDLY. Last recorded data: ... <- crash / kill -9 / power loss happened after this`  
*Click the **Crash & Session History** tab on the web dashboard to show the red `Crashed / Abrupt Termination` badge!*

---

## 8. Viva / Oral Exam Questions & Answers

### OS Questions
1. **Q: Why did you read `/proc` instead of running CLI commands like `top` or `ps`?**  
   *A:* Running a CLI command requires `fork()` and `exec()`, which is slow and wastes CPU cycles. `/proc` is a virtual filesystem generated on-demand by the kernel in RAM. Reading `/proc/stat` and `/proc/meminfo` directly via standard C++ file streams has near-zero overhead.
2. **Q: How does `inotify` work, and why is it better than a `while(true)` directory scan?**  
   *A:* Scanning a folder in a loop wastes CPU and disk I/O. `inotify` is an event-driven Linux kernel mechanism. The kernel notifies our process only when a directory change occurs. By using the `poll()` system call, our thread sleeps and uses 0% CPU until an event arrives.
3. **Q: How does your code detect process crashes?**  
   *A:* Each run creates an open session record in the database with `end_time = NULL`. On graceful termination (Ctrl+C), our async-signal-safe `sigaction` handler triggers `end_session()` to mark `clean_exit = 1`. If the process terminates abnormally (power failure or `kill -9`), `end_time` remains `NULL`. The subsequent startup detects the unclosed session, marks it as crashed (`clean_exit = 0`), and identifies the exact last-seen timestamp.
4. **Q: Why did you use `std::mutex`?**  
   *A:* Thread 1 (telemetry) and Thread 2 (inotify auditor) run concurrently. If both threads attempt to write to the database handle at the same time, memory corruption would occur. The `std::mutex` with RAII `std::lock_guard` creates a critical section ensuring only one thread accesses the database connection at a time.

### DBMS Questions
1. **Q: What is the Principle of Least Privilege, and how is it implemented?**  
   *A:* The C++ engine uses `blackbox_writer` with `INSERT`, `UPDATE`, and `CREATE` permissions. The web dashboard uses `blackbox_reader` with **`SELECT`-only permissions**. If an attacker compromised the web application, they cannot alter or wipe the forensic logs.
2. **Q: Why did you use Secondary B-Tree Indexes?**  
   *A:* The dashboard frequently queries time ranges (e.g. `WHERE timestamp >= NOW() - INTERVAL 15 MINUTE`). Without an index, the database would perform a full table scan. The B-tree index on `timestamp` enables $O(\log N)$ time-series lookups.
3. **Q: What is Write-Ahead Logging (WAL) in SQLite?**  
   *A:* In our SQLite fallback engine, WAL mode allows concurrent readers (the web dashboard) to read without blocking writers (the C++ engine), and writers to commit without locking out readers.

---

## 9. Roadmap to 100% Completion

- [x] **50% Milestone (Phase 2 - Current Submission)**:
  - Multithreaded C++ engine with `/proc` and `/sys` hardware parsers
  - MySQL / MariaDB relational database integration with SQLite fallback
  - Kernel inotify file audit with cookie-based rename detection
  - Session lifecycle crash detection
  - Hysteresis threshold alert monitor
  - Real-time web cockpit with canvas sparklines
- [ ] **75% Milestone (Phase 3)**:
  - **Per-Process Snapshotting:** When CPU usage breaches 90%, snapshot the top 5 CPU-consuming PIDs from `/proc/[pid]/stat` into a new `process_snapshots` table.
  - **Recursive Inotify Tree:** Automatically attach watches to newly created subdirectories inside `test_watch/`.
- [ ] **100% Milestone (Phase 4)**:
  - **Incident Report Export:** One-click PDF / CSV export for incident post-mortems.
  - **Systemd Daemon Service:** Installation unit file (`system_blackbox.service`) to run as a native Linux background service on boot.
