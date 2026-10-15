# System Blackbox — Complete Database Architecture & Code Guide

> **PBL Course:** Operating Systems (OS) & Database Management Systems (DBMS)  
> **Topic:** Dual-Engine Database Architecture, Code Breakdown, Schema Design, and Query Guide  
> **Audience:** Written in simple, clear English so any teammate can understand every single function and easily explain it to a professor during a viva or presentation.

---

## Table of Contents
1. [The Big Picture: What is the Database Component?](#1-the-big-picture-what-is-the-database-component)
2. [Dual-Engine Architecture: How MySQL and SQLite Work Together](#2-dual-engine-architecture-how-mysql-and-sqlite-work-together)
3. [Complete File-by-File Breakdown](#3-complete-file-by-file-breakdown)
   - [File 1: `blackbox.conf` (Configuration)](#file-1-blackboxconf-system-configuration)
   - [File 2: `sql/setup.sql` (One-Time Admin Setup)](#file-2-sqlsetupsql-one-time-admin-setup)
   - [File 3: `sql/schema.sql` (Tables & Indexes DDL)](#file-3-sqlschemasql-table-definitions--indexes)
   - [File 4: `db.hpp` (Database Manager C++ Header)](#file-4-dbhpp-database-manager-interface)
   - [File 5: `db.cpp` (Database Manager C++ Implementation - Function by Function)](#file-5-dbcpp-core-database-implementation---deep-dive)
   - [File 6: `main.cpp` (How the C++ Daemon Uses the Database)](#file-6-maincpp-how-the-daemon-interacts-with-the-database)
   - [File 7: `server.py` (How the Web Dashboard Queries the Database)](#file-7-serverpy-python-web-server--read-only-queries)
4. [Principle of Least Privilege (Security Explained)](#4-principle-of-least-privilege-security-explained)
5. [The 4 Database Tables Explained in Depth](#5-the-4-database-tables-explained-in-depth)
6. [Crash Detection: How the Database Detects `kill -9` or Power Loss](#6-crash-detection-how-the-database-detects-kill--9-or-power-loss)
7. [How to Access the Database (Step-by-Step Commands)](#7-how-to-access-the-database-step-by-step-commands)
8. [SQL Queries Cheat Sheet (From Basic to Advanced Forensics)](#8-sql-queries-cheat-sheet)
9. [Automated Data Retention (Garbage Collection)](#9-automated-data-retention-garbage-collection)
10. [Viva / Presentation Cheat Sheet (Questions Teammates Will Be Asked)](#10-viva--presentation-cheat-sheet-questions-teammates-will-be-asked)

---

## 1. The Big Picture: What is the Database Component?

Imagine an **airplane flight recorder (Black Box)**:
- During a flight, flight computers measure altitude, speed, and fuel.
- But if that data is only displayed on an LCD screen and never saved, it is completely useless when an engine explodes or the plane loses power.
- The black box solves this by saving every second of data into a **hardened, crash-proof vault**.

In our project:
- The **Operating System (OS) C++ code** is the airplane sensor. It constantly measures CPU %, RAM %, temperatures, and watches a secret folder (`test_watch/`) for deleted or renamed files.
- The **Database (DBMS)** is the **flight recorder vault**. It stores every heartbeat, every alert, every file change, and tracks whether the computer shut down safely or crashed.

### Why use a Relational Database instead of a `.txt` or `.csv` file?
If a professor asks: *"Why didn't you just write `std::ofstream file("data.csv")`?"*, here are the 4 core DBMS reasons:
1. **Concurrency & Thread Safety:** In our C++ engine, Thread 1 (Telemetry) and Thread 2 (Inotify File Auditor) write simultaneously, while Python reads for the web dashboard. Writing to a plain `.csv` file from multiple threads causes **file locking errors, race conditions, and corrupted text**. A DBMS handles concurrent reads and writes safely.
2. **B-Tree Indexing ($O(\log N)$ Speed):** If the computer runs for a few days, you will have 250,000+ rows. Finding the telemetry for a specific second in a CSV file requires reading all 250,000 lines from the hard drive (a slow $O(N)$ scan). In MySQL/SQLite, **B-Tree indexes** find that exact record in under 1 millisecond.
3. **Crash Recovery & ACID Transactions:** If power is pulled while writing, a CSV file ends up with half-written, corrupted lines. A DBMS uses **Write-Ahead Logging (WAL)** to guarantee Atomicity and Durability.
4. **Security & Principle of Least Privilege:** You cannot assign passwords or permissions to individual columns in a CSV file. With a DBMS, the web server is given **read-only (`SELECT`) rights**, meaning even if a hacker attacks the web page, they cannot delete or alter the forensic logs!

---

## 2. Dual-Engine Architecture: How MySQL and SQLite Work Together

Our project does not rely on just one database engine; it implements an intelligent **Dual-Engine Architecture**:

```
                       ┌───────────────────────────────┐
                       │     C++ Engine (main.cpp)     │
                       └───────────────┬───────────────┘
                                       │
                         Calls DatabaseManager::init()
                                       │
                      Can it connect to MySQL/MariaDB?
                                       │
                     ┌─────────────────┴─────────────────┐
                     │ YES                               │ NO (or configured as sqlite)
                     ▼                                   ▼
        ┌─────────────────────────┐         ┌─────────────────────────┐
        │   MySQL / MariaDB       │         │   SQLite Fallback       │
        │   (Port 3306 / TCP)     │         │   (blackbox.db)         │
        │                         │         │                         │
        │ • Enterprise DBMS       │         │ • Embedded File DBMS    │
        │ • Client-Server Model   │         │ • Zero Installation     │
        │ • libmariadb C API      │         │ • WAL Journal Mode      │
        └─────────────────────────┘         └─────────────────────────┘
```

1. **Primary Backend — MySQL / MariaDB:**
   - Runs as an independent system daemon (`sudo systemctl start mariadb`).
   - The C++ engine talks to it using native socket/TCP connections through the official C client API (`mysql.h` / `libmariadb`).
   - Designed for production, high throughput, and multi-user privilege separation.
2. **Fallback Backend — SQLite (`blackbox.db`):**
   - If MariaDB is stopped, not installed, or has wrong credentials, the C++ engine **does not crash**.
   - It prints a friendly warning and automatically switches to **SQLite**, creating a local file called `blackbox.db`.
   - It enables **WAL mode (Write-Ahead Logging)** so that Python can read while C++ writes without blocking each other.

---

## 3. Complete File-by-File Breakdown

This section breaks down **every single database file** in the project, explaining what each line and function does in plain English.

---

### File 1: `blackbox.conf` (System Configuration)
[`blackbox.conf`](file:///home/mjfriedrice/Documents/OS%20AND%20DBMS%20PBL/blackbox.conf) is the central configuration file. Both the C++ engine and the Python web dashboard read this file on startup so you don't have to recompile code when settings change.

- **`db_backend = mysql`**: Which database to try first (`mysql` or `sqlite`).
- **`mysql_host = 127.0.0.1`**: IP address of the MySQL server (`127.0.0.1` means localhost).
- **`mysql_port = 3306`**: Standard TCP port for MySQL/MariaDB.
- **`mysql_user = blackbox_writer`**: The high-privilege account used by the C++ engine.
- **`mysql_password = writer_pass`**: Password for the writer account.
- **`mysql_database = blackbox`**: The name of the database that contains our tables.
- **`mysql_socket = /run/mysqld/mysqld.sock`**: Local Linux UNIX domain socket (faster than TCP).
- **`mysql_reader_user = blackbox_reader`**: The low-privilege account used by the Python web UI.
- **`mysql_reader_password = reader_pass`**: Password for the reader account.
- **`sqlite_path = blackbox.db`**: Where to save the SQLite file if MySQL is offline.
- **`retention_days = 7`**: How many days of vitals to keep before auto-deleting old rows.

---

### File 2: `sql/setup.sql` (One-Time Admin Setup)
[`sql/setup.sql`](file:///home/mjfriedrice/Documents/OS%20AND%20DBMS%20PBL/sql/setup.sql) is the database initialization script run once with administrator privileges (`sudo mariadb < sql/setup.sql`).

#### What each block does:
1. **`CREATE DATABASE IF NOT EXISTS blackbox CHARACTER SET utf8mb4 ...;`**
   - Creates the database container named `blackbox`.
   - `utf8mb4` ensures that file names with spaces, emojis, or international symbols don't cause encoding crashes.
2. **User Creation:**
   ```sql
   CREATE USER IF NOT EXISTS 'blackbox_writer'@'localhost' IDENTIFIED BY 'writer_pass';
   CREATE USER IF NOT EXISTS 'blackbox_writer'@'127.0.0.1' IDENTIFIED BY 'writer_pass';
   CREATE USER IF NOT EXISTS 'blackbox_reader'@'localhost' IDENTIFIED BY 'reader_pass';
   CREATE USER IF NOT EXISTS 'blackbox_reader'@'127.0.0.1' IDENTIFIED BY 'reader_pass';
   ```
   - **Why create for both `localhost` and `127.0.0.1`?** In MySQL, `localhost` connects via Linux UNIX sockets (`.sock` file), while `127.0.0.1` connects via network loopback (TCP). MySQL treats them as two completely separate hosts! Creating both prevents weird connection bugs.
3. **Privilege Assignment (DCL - Data Control Language):**
   ```sql
   GRANT ALL PRIVILEGES ON blackbox.* TO 'blackbox_writer'@'localhost';
   GRANT SELECT ON blackbox.* TO 'blackbox_reader'@'localhost';
   ```
   - Gives the writer full insert/update/table creation powers.
   - Restricts the reader strictly to `SELECT` queries (no drops, no deletes).
4. **`FLUSH PRIVILEGES;`**
   - Forces MariaDB to immediately reload the user permission tables in RAM.

---

### File 3: `sql/schema.sql` (Table Definitions & Indexes)
[`sql/schema.sql`](file:///home/mjfriedrice/Documents/OS%20AND%20DBMS%20PBL/sql/schema.sql) defines the physical structure of our 4 tables.

#### Table Breakdown:
1. **`sessions` Table:**
   - `id BIGINT AUTO_INCREMENT PRIMARY KEY`: Unique integer for each boot of the C++ daemon.
   - `start_time VARCHAR(25) NOT NULL`: Exact time the engine booted (`YYYY-MM-DD HH:MM:SS`).
   - `end_time VARCHAR(25) NULL`: Time the engine stopped. Stays `NULL` while running!
   - `pid INT NOT NULL`: Linux Process ID assigned by the OS kernel.
   - `hostname VARCHAR(255)`: Machine name.
   - `clean_exit TINYINT NULL`:
     - `NULL`: Currently running.
     - `1`: Normal exit (`Ctrl+C` or `SIGTERM`).
     - `0`: **CRASHED or KILLED (`kill -9`)!**
2. **`vitals_log` Table:**
   - `id BIGINT AUTO_INCREMENT PRIMARY KEY`: Unique ID per reading.
   - `timestamp VARCHAR(25) NOT NULL`: Date and time.
   - `cpu_usage DOUBLE NOT NULL`: CPU percentage (e.g. `24.5`).
   - `ram_usage DOUBLE NOT NULL`: RAM percentage (e.g. `58.2`).
   - `cpu_temp DOUBLE DEFAULT 0.0`: CPU core temperature in °C.
   - `fan_speed INT DEFAULT 0`: Fan RPM.
   - `session_id BIGINT NULL`: Foreign key pointing to `sessions(id)`.
3. **`deleted_files` Table:**
   - `file_name VARCHAR(512)`: Path or name of the file touched.
   - `event_type VARCHAR(32)`: `DELETED`, `MOVED_OUT`, `RENAMED`, etc.
   - `is_dir TINYINT`: 1 if directory, 0 if regular file.
   - `new_name VARCHAR(512)`: New name if the file was renamed.
4. **`alerts` Table:**
   - `metric VARCHAR(64)`: e.g. `CPU_USAGE` or `CPU_TEMP`.
   - `value DOUBLE`: The reading that tripped the alert.
   - `threshold DOUBLE`: The limit configured in `blackbox.conf`.
   - `state VARCHAR(32)`: `TRIGGERED` (went above limit) or `RESOLVED` (cooled back down).

#### What do the Indexes do?
Notice lines like:
```sql
INDEX idx_vitals_ts (timestamp),
INDEX idx_vitals_session (session_id)
```
- **Plain English:** Think of an index like the index at the back of a 1,000-page textbook. Instead of reading every page to find the word "CPU", you jump straight to page 412.
- In DBMS terms, these create **B-Trees**. When the dashboard asks: *"Give me records where `session_id = 4`"*, MariaDB searches the B-Tree in $O(\log N)$ time instead of scanning every single row in the table.

---

### File 4: `db.hpp` (Database Manager Interface)
[`db.hpp`](file:///home/mjfriedrice/Documents/OS%20AND%20DBMS%20PBL/db.hpp) is the C++ header file that defines the data structures and class methods.

#### Key Structs:
- **`enum class BackendType { MySQL, SQLite };`**: Strongly typed enum telling the program which database engine is currently running.
- **`struct DatabaseConfig`**: Holds connection parameters (`mysql_host`, `mysql_port`, `mysql_user`, `mysql_pass`, `mysql_db`, `sqlite_path`, `fallback_to_sqlite`).
- **`struct SessionInfo`**: Holds information about a session (`id`, `start_time`, `last_seen`, `pid`, `hostname`). Used specifically for crash forensic recovery.

#### Class `DatabaseManager`:
- Manages connection handles (`MYSQL* mysql_handle_` and `sqlite3* sqlite_handle_`).
- Contains `std::mutex db_mutex_`: A mutual exclusion lock that serializes all database calls so that concurrent C++ threads never write at the exact same microsecond and corrupt the socket.

---

### File 5: `db.cpp` (Core Database Implementation - Deep Dive)
[`db.cpp`](file:///home/mjfriedrice/Documents/OS%20AND%20DBMS%20PBL/db.cpp) is the heart of the DBMS layer (640 lines of modern C++23). Here is **every single function** explained in detail:

#### 1. Constructor & Destructor
- **`DatabaseManager::DatabaseManager()`**:
  - Sets handles to `nullptr` and initializes the class.
- **`DatabaseManager::~DatabaseManager()`**:
  - Automatically calls `close()` when the object goes out of scope to prevent socket leaks or dangling file handles.

#### 2. Utility & Helper Functions
- **`current_timestamp()`**:
  - Generates the current time string formatted as `YYYY-MM-DD HH:MM:SS`.
  - **OS Concept:** Uses the POSIX thread-safe function `localtime_r(&now_c, &tm_buf)` instead of the standard `std::localtime()`, which uses a shared static buffer and causes race conditions in multithreaded apps.
- **`escape_mysql(string_view s)`**:
  - Calls `mysql_real_escape_string(mysql_handle_, ...)` to escape quotes and control characters in file names.
  - **DBMS Concept (SQL Injection Prevention):** If someone deletes a file named `hack'; DROP TABLE vitals_log; --.txt`, this function escapes the single quote so MySQL treats it as a harmless string literal, not executable code!

#### 3. Initialization Functions
- **`bool init(const DatabaseConfig& cfg)`**:
  - Acquires `std::lock_guard<std::mutex> lock(db_mutex_)`.
  - If `cfg.backend == MySQL`, it calls `init_mysql(cfg)`.
  - If MySQL fails and `fallback_to_sqlite` is `true`, it prints an informative message and calls `init_sqlite(cfg.sqlite_path)`.
  - Returns `true` if either backend succeeded.
- **`bool init_mysql(const DatabaseConfig& cfg)`**:
  - Calls `mysql_init(nullptr)` to allocate the client structure.
  - Sets connection timeout to 3 seconds (`MYSQL_OPT_CONNECT_TIMEOUT`).
  - Enables auto-reconnect (`MYSQL_OPT_RECONNECT`).
  - Calls `mysql_real_connect()` using host, port, credentials, and socket.
  - Sets encoding to `utf8mb4` via `mysql_set_character_set()`.
  - Calls `create_mysql_schema()` to ensure tables exist.
- **`bool create_mysql_schema()`**:
  - Runs the multi-statement SQL DDL script to create `sessions`, `vitals_log`, `deleted_files`, and `alerts` if they do not exist.
  - Consumes any result sets via `mysql_next_result()` to keep the connection clean.
- **`bool init_sqlite(const string& db_path)`**:
  - Calls `sqlite3_open(db_path.c_str(), &sqlite_handle_)`.
  - Sets busy timeout to 2,000ms (`sqlite3_busy_timeout`) so that if Python is reading the file, C++ will wait up to 2 seconds instead of failing immediately with `SQLITE_BUSY`.
  - **Executes `PRAGMA journal_mode = WAL;`**: Enables Write-Ahead Logging mode. In standard rollback journal mode, readers block writers. In WAL mode, reads and writes can happen at the exact same time!
  - Executes `PRAGMA synchronous = NORMAL;`: Accelerates disk syncing while preserving durability.
  - Calls `create_sqlite_schema()`.
- **`bool create_sqlite_schema()`**:
  - Creates the 4 SQLite tables.
  - Uses `add_sqlite_column_if_missing()` to automatically migrate older versions of the database without losing data.
  - Creates the B-Tree indexes (`idx_vitals_ts`, etc.).
  - Prepares compiled SQLite bytecode statements (`stmt_vitals_`, `stmt_file_`, `stmt_alert_`) using `sqlite3_prepare_v2()` for maximum insert speed.

#### 4. Session & Crash Recovery Functions
- **`vector<SessionInfo> find_open_sessions()`**:
  - **The Crash Detective Query!**
  - Runs a SQL query looking for sessions where `end_time IS NULL`.
  - Performs an outer join (`LEFT JOIN vitals_log ON v.session_id = s.id`) and uses `MAX(v.timestamp)` to discover the **exact last second the computer was alive** before the crash!
- **`bool mark_session_crashed(const SessionInfo& s)`**:
  - Updates the abandoned session:
    ```sql
    UPDATE sessions SET end_time = 'LAST_SEEN_TIME', clean_exit = 0 WHERE id = SESSION_ID;
    ```
  - Formally documents that this session ended abruptly (power cut or `kill -9`).
- **`bool begin_session(int pid, const string& hostname)`**:
  - Inserts a new row into `sessions` with `start_time = NOW()`, `pid`, `hostname`, and `clean_exit = NULL`.
  - Retrieves the newly generated auto-increment ID (`mysql_insert_id()` or `sqlite3_last_insert_rowid()`) and stores it in `session_id_`.
- **`bool end_session()`**:
  - Called during clean shutdown (e.g. user pressed `Ctrl+C`).
  - Updates the active session:
    ```sql
    UPDATE sessions SET end_time = NOW(), clean_exit = 1 WHERE id = session_id_;
    ```

#### 5. Telemetry & Event Logging Functions
- **`bool log_vitals(double cpu_usage, double ram_usage, double cpu_temp, int fan_speed)`**:
  - Locks `db_mutex_`.
  - Gets current timestamp.
  - In MySQL: Executes `INSERT INTO vitals_log (...) VALUES (...)`.
  - In SQLite: Binds values to prepared statement `stmt_vitals_` using `sqlite3_bind_double()` and steps it with `sqlite3_step()`.
- **`bool log_file_event(string_view file_name, string_view event_type, bool is_dir, string_view new_name)`**:
  - Locks `db_mutex_`.
  - Inserts a row into `deleted_files` with filename, event type (`DELETED`, `RENAMED`, etc.), and whether it was a folder.
- **`bool log_alert(string_view metric, double value, double threshold, string_view state)`**:
  - Locks `db_mutex_`.
  - Inserts a row into `alerts` recording the threshold breach (`TRIGGERED` or `RESOLVED`).

#### 6. Maintenance & Cleanup Functions
- **`int purge_old_vitals(int days)`**:
  - **Automated Data Retention.**
  - Deletes telemetry older than $N$ days to prevent disk exhaustion:
    ```sql
    -- MySQL:
    DELETE FROM vitals_log WHERE timestamp < DATE_SUB(NOW(), INTERVAL 7 DAY);
    -- SQLite:
    DELETE FROM vitals_log WHERE timestamp < datetime('now', 'localtime', '-7 days');
    ```
  - Returns how many rows were purged.
- **`void close()`**:
  - Finalizes SQLite prepared statements (`sqlite3_finalize`).
  - Closes MySQL connection (`mysql_close`) and SQLite connection (`sqlite3_close`).
  - Sets pointers to `nullptr`.

---

### File 6: `main.cpp` (How the Daemon Interacts with the Database)
[`main.cpp`](file:///home/mjfriedrice/Documents/OS%20AND%20DBMS%20PBL/main.cpp) orchestrates the whole project. Here is how it uses `DatabaseManager` across its lifecycle:

1. **Phase 1: Boot & DB Init (Lines 515–520):**
   - Reads `blackbox.conf`.
   - Calls `db.init(db_cfg)`. If DB connection fails completely, the engine terminates with an error.
2. **Phase 2: Crash Detection Check (Lines 522–532):**
   - Calls `db.find_open_sessions()`.
   - If any unclosed session is returned, it prints:
     `[WARNING] Previous session #X ended UNEXPECTEDLY. Last recorded data: ...`
   - Calls `db.mark_session_crashed(s)` to flag `clean_exit = 0`.
3. **Phase 3: Session Registration (Lines 535–545):**
   - Calls `db.begin_session(getpid(), hostname)`.
   - Calls `db.purge_old_vitals(cfg.retention_days)` to clean up old data from past runs.
4. **Phase 4: Runtime Concurrent Logging (Lines 288, 299, 381):**
   - **Thread 1 (Telemetry Loop):** Every 1 second, reads CPU/RAM/Temp sensors and calls `db.log_vitals(...)`. If an alert fires, it calls `db.log_alert(...)`.
   - **Thread 2 (Inotify Watcher):** When the Linux kernel flags a file deletion or rename in `test_watch/`, it immediately calls `db.log_file_event(...)`.
5. **Phase 5: Graceful Shutdown (Lines 568–572):**
   - When the user presses `Ctrl+C`, the `sigaction` signal handler sets `running = false`.
   - The engine exits the thread loop, calls `db.end_session()` (`clean_exit = 1`), and cleanly calls `db.close()`.

---

### File 7: `server.py` (Python Web Server & Read-Only Queries)
[`server.py`](file:///home/mjfriedrice/Documents/OS%20AND%20DBMS%20PBL/server.py) is a zero-dependency Python HTTP server that acts as the backend for the browser dashboard.

#### Key Functions & Queries:
- **`get_db()` (Lines 75–116):**
  - Tries connecting to MySQL using the read-only user (`blackbox_reader` / `reader_pass`).
  - If MySQL is offline, it falls back to reading `blackbox.db` via SQLite in read-only URI mode (`file:blackbox.db?mode=ro`).
- **`get_latest_vitals_from_db()` (Lines 119–185):**
  - Runs: `SELECT * FROM vitals_log ORDER BY id DESC LIMIT 1;`
  - Checks if the reading is newer than 4 seconds. If older, it reports `daemon_running: false` so the web dashboard accurately shows if the background engine stopped!
- **`get_deleted_files_from_db()` (Lines 221–255):**
  - Runs: `SELECT * FROM deleted_files ORDER BY id DESC LIMIT 50;`
  - Returns the forensic audit trail in JSON format.
- **`get_alerts_from_db()` (Lines 263–310):**
  - Runs: `SELECT * FROM alerts ORDER BY id DESC LIMIT 50;`
  - Counts active alerts to render red warning pills on the dashboard.
- **`get_sessions_from_db()` (Lines 316–345):**
  - Runs: `SELECT * FROM sessions ORDER BY id DESC LIMIT 20;`
  - Formats crash states so the web dashboard displays clean green badges for normal exits and bright red badges for crashes.
- **`get_history_from_db(minutes=10)` (Lines 351–402):**
  - Queries historical records for the last 10 minutes and downsamples them to render smooth SVG sparkline charts on the dashboard.

---

## 4. Principle of Least Privilege (Security Explained)

In enterprise DBMS engineering, you never connect every application with `root`. We implement strict **Data Control Language (DCL)** separation:

```
┌──────────────────────────────────────┐     ┌──────────────────────────────────────┐
│       C++ Engine (Daemon)            │     │       Web Server (server.py)         │
│                                      │     │                                      │
│ • Runs as local root/system daemon   │     │ • Exposed to local network / browser │
│ • Connects as: blackbox_writer       │     │ • Connects as: blackbox_reader       │
│ • Permissions: ALL PRIVILEGES        │     │ • Permissions: SELECT ONLY           │
│   (CREATE, INSERT, UPDATE, DELETE)   │     │   (Read-only, cannot alter data)     │
└──────────────────────────────────────┘     └──────────────────────────────────────┘
```

### Why is this important?
If an attacker discovers a vulnerability in the Python web dashboard or injects malicious parameters into a URL, the database user physically **lacks the permission** to run `DROP TABLE` or `DELETE FROM`. The database engine itself rejects the query!

---

## 5. The 4 Database Tables Explained in Depth

Here is the Entity-Relationship structure connecting our 4 tables:

```
   ┌────────────────────────────────────────────────────────┐
   │                        sessions                        │
   │  PK: id                                                │
   │  start_time, end_time, pid, hostname, clean_exit       │
   └───────────────────────────┬────────────────────────────┘
                               │
               1-to-Many via Foreign Key (session_id)
                               │
         ┌─────────────────────┼─────────────────────┐
         ▼                     ▼                     ▼
┌──────────────────┐  ┌──────────────────┐  ┌──────────────────┐
│    vitals_log    │  │  deleted_files   │  │      alerts      │
│ PK: id           │  │ PK: id           │  │ PK: id           │
│ FK: session_id   │  │ FK: session_id   │  │ FK: session_id   │
│ timestamp        │  │ timestamp        │  │ timestamp        │
│ cpu_usage        │  │ file_name        │  │ metric           │
│ ram_usage        │  │ event_type       │  │ value            │
│ cpu_temp         │  │ is_dir           │  │ threshold        │
│ fan_speed        │  │ new_name         │  │ state            │
└──────────────────┘  └──────────────────┘  └──────────────────┘
```

1. **`sessions` (Parent Table):** Represents one continuous run of the program.
2. **`vitals_log` (Child Table):** Millions of telemetry rows linked to the parent `session_id`.
3. **`deleted_files` (Child Table):** Inotify audit records linked to the specific session during which the file was tampered with.
4. **`alerts` (Child Table):** High-temperature or high-CPU alert events tied to the session.

---

## 6. Crash Detection: How the Database Detects `kill -9` or Power Loss

A standout feature in this project is **Kernel & Process Crash Forensics**:

### Scenario A: Normal Shutdown (`Ctrl+C` or `SIGTERM`)
1. User presses `Ctrl+C`.
2. Linux sends signal `SIGINT` to the C++ process.
3. The signal handler sets `running = false`.
4. The main thread breaks its loop and calls `db.end_session()`.
5. Database updates: `clean_exit = 1`, `end_time = NOW()`.
6. Status: **CLEAN EXIT (Green badge)**.

### Scenario B: Hard Crash, Power Loss, or Force Kill (`kill -9`)
1. Someone pulls the power cord or runs `kill -9 <pid>`.
2. The Linux kernel kills the process **instantly** in kernel space.
3. The C++ process is given zero CPU cycles to run cleanup functions, catch signals, or close connections.
4. The row in `sessions` remains with `end_time = NULL` and `clean_exit = NULL`.
5. **The Magic Recovery:** When you start `./system_blackbox` the next time:
   - It runs `find_open_sessions()`.
   - It finds the abandoned session!
   - It queries `MAX(vitals_log.timestamp)` for that `session_id` to determine the exact second the computer lost power.
   - It marks `clean_exit = 0` and sets `end_time` to that last recorded vital.
   - The terminal and web dashboard flag:
     `[WARNING] Previous session #X ended UNEXPECTEDLY. Last recorded data: 2026-10-15 18:22:04`

---

## 7. How to Access the Database (Step-by-Step Commands)

### 1. Interactive Command Line (CLI)

#### Open Read-Only Terminal (Reader User)
```bash
mariadb -u blackbox_reader -preader_pass blackbox
# or: mysql -u blackbox_reader -preader_pass blackbox
```

#### Open Full-Access Terminal (Writer User)
```bash
mariadb -u blackbox_writer -pwriter_pass blackbox
# or: mysql -u blackbox_writer -pwriter_pass blackbox
```

#### Run a Single Query Directly from Bash:
```bash
mariadb -u blackbox_reader -preader_pass -D blackbox -e "SELECT * FROM vitals_log ORDER BY id DESC LIMIT 5;"
```

---

### 2. Built-in Web UI Dashboard
```bash
make ui
# or: python3 server.py
```
Open your browser at **`http://localhost:8080`**.

---

### 3. Graphical Database Tools (DBeaver, DataGrip, VS Code MySQL)
Use these parameters:
- **Host:** `127.0.0.1`
- **Port:** `3306`
- **Database:** `blackbox`
- **User:** `blackbox_reader` *(read-only)* or `blackbox_writer` *(read-write)*
- **Password:** `reader_pass` or `writer_pass`

---

### 4. SQLite Fallback File (If used)
If you ran `./system_blackbox --backend sqlite`, inspect the local file with:
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

## 8. SQL Queries Cheat Sheet

Use these queries when presenting or inspecting data:

### 1. Table Overview & Row Counts
```sql
-- See all tables
SHOW TABLES;

-- Check total rows across all 4 tables
SELECT 'sessions' AS tbl, COUNT(*) AS cnt FROM sessions
UNION ALL
SELECT 'vitals_log', COUNT(*) FROM vitals_log
UNION ALL
SELECT 'deleted_files', COUNT(*) FROM deleted_files
UNION ALL
SELECT 'alerts', COUNT(*) FROM alerts;
```

### 2. Real-Time Telemetry Queries
```sql
-- View the 10 most recent sensor readings
SELECT id, timestamp, cpu_usage, ram_usage, cpu_temp, fan_speed, session_id
FROM vitals_log 
ORDER BY id DESC 
LIMIT 10;
```

### 3. Crash Forensics Queries
```sql
-- View all sessions and their human-readable status
SELECT id, start_time, end_time, pid, hostname,
       CASE 
         WHEN clean_exit = 1 THEN 'Clean Shutdown'
         WHEN clean_exit = 0 THEN 'CRASHED / KILLED (-9)'
         ELSE 'Currently Running'
       END AS status
FROM sessions
ORDER BY id DESC;

-- What were the exact vitals right before session #1 died?
SELECT * FROM vitals_log 
WHERE session_id = 1 
ORDER BY id DESC 
LIMIT 5;
```

### 4. Forensic File Audit Queries
```sql
-- See all files deleted, renamed, or moved
SELECT timestamp, event_type, file_name, new_name, session_id
FROM deleted_files 
ORDER BY id DESC;
```

### 5. Threshold Alerts
```sql
-- See recent threshold alert events
SELECT timestamp, metric, value, threshold, state, session_id
FROM alerts 
ORDER BY id DESC;
```

### 6. Analytical Aggregate Queries (Viva Favorites!)
```sql
-- Calculate overall system averages and maximums
SELECT 
    ROUND(AVG(cpu_usage), 2) AS avg_cpu,
    ROUND(MAX(cpu_usage), 2) AS peak_cpu,
    ROUND(AVG(ram_usage), 2) AS avg_ram,
    ROUND(MAX(cpu_temp), 2)  AS peak_temp
FROM vitals_log;

-- Breakdown by session
SELECT 
    session_id,
    COUNT(*) AS total_samples,
    ROUND(AVG(cpu_usage), 1) AS avg_cpu,
    ROUND(MAX(cpu_temp), 1)  AS max_temp
FROM vitals_log
GROUP BY session_id;
```

---

## 9. Automated Data Retention (Garbage Collection)

If your computer runs continuously, recording every second produces **86,400 rows per day**. In 1 month, that is over 2.5 million rows!

To prevent the hard drive from filling up:
- [`blackbox.conf`](file:///home/mjfriedrice/Documents/OS%20AND%20DBMS%20PBL/blackbox.conf) defines:
  ```ini
  retention_days = 7
  ```
- Every 1,000 cycles, the C++ engine runs:
  ```sql
  -- MySQL:
  DELETE FROM vitals_log WHERE timestamp < DATE_SUB(NOW(), INTERVAL 7 DAY);

  -- SQLite:
  DELETE FROM vitals_log WHERE timestamp < datetime('now', 'localtime', '-7 days');
  ```
- **DBMS Concept:** This automated purging acts as database garbage collection, ensuring that old telemetry is continuously pruned while preserving historical forensic incident tables (`deleted_files` and `alerts`).

---

## 10. Viva / Presentation Cheat Sheet (Questions Teammates Will Be Asked)

| Viva Question | Simple Answer to Give |
| :--- | :--- |
| **"Why did you use MySQL instead of just a file or CSV?"** | A CSV file cannot handle concurrent writes from multiple C++ threads and simultaneous reads from a Python web dashboard without file lock corruption. MySQL provides ACID guarantees, multi-client concurrency, and B-Tree indexes for instant time-series lookups. |
| **"What is the Principle of Least Privilege in your project?"** | We created two database accounts: `blackbox_writer` for the C++ daemon (full insert/create privileges) and `blackbox_reader` for the Python web dashboard (`SELECT` only). Even if the web dashboard is compromised, an attacker cannot delete or modify forensic logs. |
| **"How does your crash detection work?"** | Each run inserts a row in `sessions` with `clean_exit = NULL`. If the program is killed via `kill -9` or a power outage, it never runs the cleanup code. On the next boot, the program queries for unclosed sessions (`WHERE end_time IS NULL`), links with `vitals_log` to find the exact last recorded second of life, and flags it as `clean_exit = 0`. |
| **"What happens if MariaDB is stopped on the computer?"** | The C++ engine implements an automatic fallback mechanism. If MariaDB is unreachable, it logs a warning and automatically initializes an embedded **SQLite** database (`blackbox.db`) in WAL mode, so monitoring never interrupts. |
| **"What is SQLite WAL mode?"** | WAL stands for Write-Ahead Logging. In traditional SQLite, a write locks the entire database file, preventing reads. In WAL mode, changes are appended to a separate `.db-wal` file, allowing the Python dashboard to read continuously while C++ writes. |
| **"Why did you add indexes on `timestamp` and `session_id`?"** | Without indexes, querying the latest readings or grouping by session requires a Full Table Scan ($O(N)$), which gets slower every second. B-Tree indexes allow $O(\log N)$ searches, returning results in under a millisecond regardless of table size. |
| **"How do you prevent SQL Injection in C++?"** | We use `mysql_real_escape_string()` for MySQL and parameterized prepared statements (`sqlite3_bind_*`) for SQLite, ensuring file names with quotes cannot inject arbitrary SQL commands. |
