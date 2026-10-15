# System Blackbox — Database Documentation & Guide

> **PBL Component:** Database Management Systems (DBMS)  
> **Topic:** Dual-Engine Relational Storage, Principle of Least Privilege, Schema Architecture & Practical Query Guide  
> **Written In:** Simple, plain English with practical examples.

---

## Table of Contents
1. [What is the Database Component? (In Plain English)](#1-what-is-the-database-component-in-plain-english)
2. [Dual-Engine Architecture: How It Works](#2-dual-engine-architecture-how-it-works)
3. [All Files Involved in the Database](#3-all-files-involved-in-the-database)
4. [User Accounts & Principle of Least Privilege](#4-user-accounts--principle-of-least-privilege)
5. [The 4 Database Tables Explained](#5-the-4-database-tables-explained)
6. [How to Access the Database (Step-by-Step)](#6-how-to-access-the-database-step-by-step)
7. [SQL Queries Cheat Sheet (From Basic to Forensic)](#7-sql-queries-cheat-sheet)
8. [How the C++ Engine Talks to the Database](#8-how-the-c-engine-talks-to-the-database)
9. [Automated Data Retention (Garbage Collection)](#9-automated-data-retention-garbage-collection)
10. [Troubleshooting & Common Questions](#10-troubleshooting--common-questions)

---

## 1. What is the Database Component? (In Plain English)

Imagine an airplane's black box. It doesn't just display sensor readings on a screen; it writes them permanently to a crash-resistant vault.

In our project:
- The **Operating System (OS) C++ code** constantly reads system stats (CPU, RAM, temperatures) and watches a folder for deleted/renamed files.
- The **Database (DBMS)** acts as the **blackbox vault**.
- Instead of dumping raw data into a slow, messy text file or CSV, we use a **Relational Database** (`MySQL / MariaDB` or `SQLite`).

### Why use a database instead of a simple `.txt` or `.csv` file?
1. **Speed & Indexing:** Even with 100,000+ rows of telemetry, B-Tree indexes let us find the exact second an event occurred in milliseconds.
2. **Concurrency & Thread Safety:** Two separate C++ threads (sensor thread + file auditor thread) and a Python web dashboard can access the data at the same time without corrupting the file.
3. **Data Integrity (ACID):** If power is pulled, database transactions ensure half-written garbage data doesn't corrupt your storage.
4. **Security & Permissions (DCL):** We can create user accounts with restricted permissions so a web viewer cannot accidentally delete forensic records.

---

## 2. Dual-Engine Architecture: How It Works

Our project features an intelligent **Dual-Engine** design:

```
                  ┌───────────────────────────────┐
                  │    C++ Engine (main.cpp)      │
                  └──────────────┬────────────────┘
                                 │
                     Can it connect to MariaDB?
                                 │
                 ┌───────────────┴───────────────┐
                 │ YES                           │ NO (or configured as sqlite)
                 ▼                               ▼
       ┌───────────────────┐           ┌───────────────────┐
       │   MySQL / MariaDB │           │   SQLite Fallback │
       │ (Port 3306 / TCP) │           │   (blackbox.db)   │
       │                   │           │                   │
       │ Enterprise Server │           │ Embedded WAL File │
       └───────────────────┘           └───────────────────┘
```

1. **Primary Backend — MySQL / MariaDB:**
   - Full client-server database architecture.
   - Runs as a system background service (`systemctl status mariadb`).
   - Uses the official C client API (`mysql.h` / `libmariadb`).
2. **Fallback Backend — SQLite (`blackbox.db`):**
   - If MariaDB is stopped, not installed, or unreachable, the C++ engine does **not** crash.
   - It gracefully falls back to a local SQLite database file (`blackbox.db`) using **WAL mode (Write-Ahead Logging)** so monitoring never stops.

---

## 3. All Files Involved in the Database

Here is the exact list of files in this repository that interact with the database:

| File | What It Does (Plain English) |
| :--- | :--- |
| [`blackbox.conf`](file:///home/mjfriedrice/Documents/OS%20AND%20DBMS%20PBL/blackbox.conf) | **Configuration file.** Stores the database choice (`mysql` or `sqlite`), host (`127.0.0.1`), port (`3306`), usernames, passwords, database name, and retention days. Both C++ and Python read this. |
| [`sql/setup.sql`](file:///home/mjfriedrice/Documents/OS%20AND%20DBMS%20PBL/sql/setup.sql) | **One-time administrator script.** Run once (`sudo mariadb < sql/setup.sql`) to create the `blackbox` database and the two user accounts (`blackbox_writer` and `blackbox_reader`). |
| [`sql/schema.sql`](file:///home/mjfriedrice/Documents/OS%20AND%20DBMS%20PBL/sql/schema.sql) | **DDL (Table Definitions).** Contains the exact SQL `CREATE TABLE` and `INDEX` statements for the 4 tables. |
| [`db.hpp`](file:///home/mjfriedrice/Documents/OS%20AND%20DBMS%20PBL/db.hpp) | **C++ Header file.** Defines the `DatabaseManager` class, connection structs, session structures, and function declarations. |
| [`db.cpp`](file:///home/mjfriedrice/Documents/OS%20AND%20DBMS%20PBL/db.cpp) | **C++ Implementation.** The core database code! Connects via MySQL C API / SQLite, executes SQL queries, manages `std::mutex` thread safety, creates schemas automatically, and handles data insertion. |
| [`main.cpp`](file:///home/mjfriedrice/Documents/OS%20AND%20DBMS%20PBL/main.cpp) | **Main OS Daemon.** Calls `db.init()`, starts a session record, periodically logs vitals and file audit events to the database, and marks the session closed on exit. |
| [`server.py`](file:///home/mjfriedrice/Documents/OS%20AND%20DBMS%20PBL/server.py) | **Python Web Server.** Connects to the database using the read-only user (`blackbox_reader`) via `pymysql` (or `sqlite3`), runs SQL `SELECT` queries, and sends JSON data to the web dashboard. |
| [`Makefile`](file:///home/mjfriedrice/Documents/OS%20AND%20DBMS%20PBL/Makefile) | **Build tool.** Links MySQL (`-lmariadb`) and SQLite (`-lsqlite3`) libraries, and includes handy commands like `make setup-db`. |

---

## 4. User Accounts & Principle of Least Privilege

In a professional DBMS, you never give every program full administrator (`root`) privileges. We implement the **Principle of Least Privilege**:

### Account 1: `blackbox_writer` (The Engine)
- **Used by:** The C++ daemon (`system_blackbox`).
- **Password:** `writer_pass`
- **Permissions:** Full control (`ALL PRIVILEGES`) on `blackbox.*`.
- **Why:** The C++ daemon needs to create tables if they don't exist, insert vitals every second, update crash states, and delete old rows when data expires.

### Account 2: `blackbox_reader` (The Web Dashboard & Human)
- **Used by:** The Python web dashboard (`server.py`) and students/analysts inspecting data.
- **Password:** `reader_pass`
- **Permissions:** `SELECT` only on `blackbox.*`.
- **Why:** Even if the Python server has a security vulnerability or a user makes a typo in their SQL query, they physically **cannot delete, drop, or corrupt** forensic records.

---

## 5. The 4 Database Tables Explained

The `blackbox` database stores data across 4 relational tables:

```
┌────────────────────────────────────────────────────────┐
│                        sessions                        │
│  (Tracks each boot of the C++ daemon & crash states)   │
└───────────────────────────┬────────────────────────────┘
                            │ (session_id)
        ┌───────────────────┼───────────────────┐
        ▼                   ▼                   ▼
┌──────────────┐    ┌───────────────┐    ┌─────────────┐
│  vitals_log  │    │ deleted_files │    │   alerts    │
│ (Telemetry)  │    │ (Forensics)   │    │(Thresholds) │
└──────────────┘    └───────────────┘    └─────────────┘
```

### Table 1: `sessions`
Tracks the lifecycle of the C++ daemon process.
- **`id`** (`BIGINT AUTO_INCREMENT PRIMARY KEY`): Unique session number (Session #1, Session #2, etc.).
- **`start_time`** (`VARCHAR(25)`): ISO timestamp when the daemon started.
- **`end_time`** (`VARCHAR(25)`): Timestamp when the daemon stopped.
- **`pid`** (`INT`): Linux Process ID of the running engine.
- **`hostname`** (`VARCHAR(255)`): Computer hostname.
- **`clean_exit`** (`TINYINT`):
  - `NULL`: Currently running.
  - `1`: Normal, clean exit (user pressed `Ctrl+C` or sent `SIGINT` / `SIGTERM`).
  - `0`: **CRASHED or KILLED (`kill -9`)!** The engine detects unclosed sessions upon restart and flags them.

### Table 2: `vitals_log`
High-frequency time-series system telemetry (recorded every second by default).
- **`id`**: Unique reading ID.
- **`timestamp`**: Date and time of the reading (`YYYY-MM-DD HH:MM:SS`).
- **`cpu_usage`**: CPU load percentage (e.g. `14.5`%).
- **`ram_usage`**: RAM load percentage (e.g. `62.1`%).
- **`cpu_temp`**: CPU temperature in degrees Celsius (e.g. `48.0`°C).
- **`fan_speed`**: Cooling fan speed in RPM (e.g. `1800` RPM).
- **`session_id`**: Foreign key linking this reading to a row in `sessions`.

### Table 3: `deleted_files`
Forensic audit trail captured directly from the Linux Kernel via `inotify`.
- **`id`**: Unique event ID.
- **`timestamp`**: Time of file modification or deletion.
- **`file_name`**: Name of the affected file or folder.
- **`event_type`**: Type of action (`DELETED`, `MOVED_OUT`, `RENAMED`, `CREATED`).
- **`is_dir`**: `1` if it was a directory, `0` if a file.
- **`new_name`**: New name if the file was renamed.
- **`session_id`**: Session during which the action happened.

### Table 4: `alerts`
Logs whenever hardware usage breaches safe limits.
- **`id`**: Unique alert ID.
- **`timestamp`**: Time of threshold event.
- **`metric`**: Which sensor tripped (`CPU_USAGE`, `RAM_USAGE`, `CPU_TEMP`).
- **`value`**: The value that caused the alert (e.g. `94.2`).
- **`threshold`**: The threshold configured in `blackbox.conf` (e.g. `90.0`).
- **`state`**: `TRIGGERED` (exceeded limit) or `RESOLVED` (cooled back down).
- **`session_id`**: Session during which the alert occurred.

### What are the Indexes?
In `sql/schema.sql`, you will notice statements like:
```sql
INDEX idx_vitals_ts (timestamp),
INDEX idx_vitals_session (session_id)
```
**Why do we have them?**  
Without an index, finding the last 10 records in a table of 500,000 rows requires scanning all 500,000 rows (a Full Table Scan, $O(N)$). With a B-Tree index on `timestamp` and `session_id`, the database finds the records in $O(\log N)$ time (instantly!).

---

## 6. How to Access the Database (Step-by-Step)

### Option A: Interactive Command-Line Interface (CLI)

Open your Linux terminal and type:

#### 1. Read-Only Access (Recommended for inspecting)
```bash
mariadb -u blackbox_reader -preader_pass blackbox
```
*(If your system uses MySQL branding instead of MariaDB, replace `mariadb` with `mysql`)*.

#### 2. Writer / Admin Access (If you need to insert or update)
```bash
mariadb -u blackbox_writer -pwriter_pass blackbox
```

#### 3. Run a quick one-liner without opening the prompt:
```bash
mariadb -u blackbox_reader -preader_pass -D blackbox -e "SELECT * FROM vitals_log ORDER BY id DESC LIMIT 5;"
```

---

### Option B: Built-in Web Dashboard

You don't even have to write SQL to see your database:
```bash
make ui
# Or: python3 server.py
```
Open **`http://localhost:8080`** in any browser. It queries the database in real time and draws live gauges and charts!

---

### Option C: GUI Tools (DBeaver, VS Code MySQL, DataGrip, HeidiSQL)

If you prefer a graphical window with clickable tables:
- **Host / IP:** `127.0.0.1`
- **Port:** `3306`
- **Database Name:** `blackbox`
- **Username:** `blackbox_reader` (or `blackbox_writer`)
- **Password:** `reader_pass` (or `writer_pass`)

---

### Option D: Accessing the SQLite Fallback File (If used)

If you ran `./system_blackbox --backend sqlite`, it created a file called `blackbox.db`. You can query it using:
```bash
sqlite3 blackbox.db
```
Inside the sqlite prompt:
```sql
.tables
SELECT * FROM vitals_log LIMIT 5;
.quit
```

---

## 7. SQL Queries Cheat Sheet

Once you are inside the database (`mariadb -u blackbox_reader -preader_pass blackbox`), here are the most useful queries:

### 1. Basic Inspection
```sql
-- See all tables in the database
SHOW TABLES;

-- See the structure (columns and data types) of a table
DESCRIBE vitals_log;
DESCRIBE sessions;
DESCRIBE deleted_files;
DESCRIBE alerts;
```

### 2. Check Total Record Counts
```sql
SELECT 'sessions' AS table_name, COUNT(*) AS total_rows FROM sessions
UNION ALL
SELECT 'vitals_log', COUNT(*) FROM vitals_log
UNION ALL
SELECT 'deleted_files', COUNT(*) FROM deleted_files
UNION ALL
SELECT 'alerts', COUNT(*) FROM alerts;
```

### 3. Query Recent Hardware Telemetry
```sql
-- View the 10 most recent sensor readings
SELECT timestamp, cpu_usage, ram_usage, cpu_temp, fan_speed 
FROM vitals_log 
ORDER BY id DESC 
LIMIT 10;
```

### 4. Forensic Crash Detection Queries
```sql
-- See all daemon sessions and their exit states
SELECT id, start_time, end_time, pid, hostname,
       CASE 
         WHEN clean_exit = 1 THEN 'Clean Exit (Ctrl+C)'
         WHEN clean_exit = 0 THEN 'CRASHED / KILLED (kill -9)'
         ELSE 'Currently Running'
       END AS session_status
FROM sessions
ORDER BY id DESC;

-- Find ONLY crashed sessions
SELECT * FROM sessions WHERE clean_exit = 0;

-- What were the hardware vitals right before the last crash?
-- (Takes the last recorded readings of session #1)
SELECT * FROM vitals_log 
WHERE session_id = 1 
ORDER BY id DESC 
LIMIT 5;
```

### 5. Forensic File Audit Queries (Kernel inotify events)
```sql
-- See all deleted, moved, or renamed files
SELECT timestamp, event_type, file_name, new_name, session_id 
FROM deleted_files 
ORDER BY id DESC;

-- Check only deletions
SELECT * FROM deleted_files WHERE event_type = 'DELETED';
```

### 6. Threshold Alert Analysis
```sql
-- See all alerts that have ever triggered
SELECT timestamp, metric, value, threshold, state 
FROM alerts 
ORDER BY id DESC;

-- Count how many times CPU exceeded safe limits
SELECT COUNT(*) AS total_cpu_alerts 
FROM alerts 
WHERE metric = 'CPU_USAGE' AND state = 'TRIGGERED';
```

### 7. Analytical Aggregate Queries (Great for PBL Viva!)
```sql
-- Calculate Average, Minimum, and Maximum CPU and RAM usage
SELECT 
    ROUND(AVG(cpu_usage), 2) AS avg_cpu,
    ROUND(MAX(cpu_usage), 2) AS max_cpu,
    ROUND(MIN(cpu_usage), 2) AS min_cpu,
    ROUND(AVG(ram_usage), 2) AS avg_ram,
    ROUND(MAX(ram_usage), 2) AS max_ram
FROM vitals_log;

-- Telemetry grouped by session
SELECT 
    session_id,
    COUNT(*) AS total_samples,
    ROUND(AVG(cpu_usage), 1) AS avg_cpu,
    ROUND(MAX(cpu_temp), 1)  AS max_temp
FROM vitals_log
GROUP BY session_id;
```

---

## 8. How the C++ Engine Talks to the Database

Here is the technical journey of a sensor reading from C++ into the database:

1. **Mutex Protection (`std::mutex`):**
   ```cpp
   std::lock_guard<std::mutex> lock(db_mutex_);
   ```
   Before any SQL statement is sent, a mutex lock is acquired. This prevents race conditions between the sensor thread and the file auditor thread.
2. **Escaping Input (Preventing SQL Injection):**
   Even though system metrics are internal numbers, strings like file names could contain quotes (`'`). The engine uses `mysql_real_escape_string()` (in MySQL) and parameterized queries (in SQLite) to ensure filenames can never inject malicious SQL.
3. **Execution:**
   In MySQL, it calls `mysql_query(mysql_handle_, sql.c_str())`.  
   In SQLite, it binds variables to a pre-compiled bytecode statement (`sqlite3_bind_double`, `sqlite3_step`).

---

## 9. Automated Data Retention (Garbage Collection)

If your computer runs for months, the `vitals_log` table would grow by ~86,400 rows every single day.

To prevent the hard drive from filling up:
- `blackbox.conf` defines `retention_days = 7` (keep 7 days of data).
- Every 1,000 recording cycles, the C++ engine runs an automated purge query:
  ```sql
  -- In MySQL:
  DELETE FROM vitals_log 
  WHERE timestamp < DATE_SUB(NOW(), INTERVAL 7 DAY);

  -- In SQLite:
  DELETE FROM vitals_log 
  WHERE timestamp < datetime('now', '-7 days');
  ```
- This guarantees the database stays lean, fast, and self-cleaning.

---

## 10. Troubleshooting & Common Questions

### Q1: When I try to run the CLI, I get: `ERROR 2002 (HY000): Can't connect to local server`
- **Cause:** MariaDB / MySQL service is not running.
- **Fix:** Start the service using:
  ```bash
  sudo systemctl start mariadb
  ```

### Q2: I get: `ERROR 1045 (28000): Access denied for user 'blackbox_reader'`
- **Cause:** The database setup script hasn't been run yet.
- **Fix:** Run the one-time setup:
  ```bash
  make setup-db
  # or: sudo mariadb < sql/setup.sql
  ```

### Q3: How do I completely wipe and reset the database?
- **For MySQL / MariaDB:**
  ```bash
  sudo mariadb -e "DROP DATABASE IF EXISTS blackbox;"
  make setup-db
  ```
- **For SQLite:**
  ```bash
  make cleandb
  ```

### Q4: How do I switch to SQLite instead of MySQL?
- In [`blackbox.conf`](file:///home/mjfriedrice/Documents/OS%20AND%20DBMS%20PBL/blackbox.conf), change:
  ```ini
  db_backend = sqlite
  ```
- Or run the C++ daemon with the CLI flag:
  ```bash
  ./system_blackbox --backend sqlite
  ```
