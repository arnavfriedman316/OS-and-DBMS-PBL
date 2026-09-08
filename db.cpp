// ============================================================================
// db.cpp — Database Manager Implementation
// ============================================================================
// System Blackbox — Phase 1
// University PBL: Operating Systems + Database Management Systems
//
// This file implements every method declared in db.hpp.
// Read the comments top-to-bottom; they are written so you can walk your
// professor through the code line by line.
// ============================================================================

#include "db.hpp"

#include <iostream>
#include <chrono>
#include <sstream>
#include <iomanip>       // std::put_time
#include <ctime>         // std::localtime, std::time_t

// ────────────────────────────────────────────────────────────────────────────
// Construction / Destruction
// ────────────────────────────────────────────────────────────────────────────

DatabaseManager::DatabaseManager() = default;

DatabaseManager::~DatabaseManager() {
    // RAII: if the caller forgot to call close(), the destructor handles it.
    close();
}

// ────────────────────────────────────────────────────────────────────────────
// current_timestamp  — helper
// ────────────────────────────────────────────────────────────────────────────
// We use the C++ <chrono> library to get the current wall-clock time and
// format it as an ISO-8601 string.  This is what gets stored in the
// "timestamp" columns of both tables.
// ────────────────────────────────────────────────────────────────────────────

std::string DatabaseManager::current_timestamp() {
    // 1. Grab the current point in time from the system clock.
    auto now   = std::chrono::system_clock::now();

    // 2. Convert it to a C-style time_t so we can format it.
    std::time_t now_c = std::chrono::system_clock::to_time_t(now);

    // 3. Convert to local-time struct and format via put_time.
    std::ostringstream oss;
    oss << std::put_time(std::localtime(&now_c), "%Y-%m-%d %H:%M:%S");

    return oss.str();
}

// ────────────────────────────────────────────────────────────────────────────
// execute_query  — runs a one-shot SQL string
// ────────────────────────────────────────────────────────────────────────────
// Used for DDL (CREATE TABLE) and PRAGMAs.  NOT used for INSERTs — those
// use prepared statements for safety and performance.
// ────────────────────────────────────────────────────────────────────────────

bool DatabaseManager::execute_query(const char* sql) {
    char* err_msg = nullptr;

    int rc = sqlite3_exec(db_handle_, sql, nullptr, nullptr, &err_msg);

    if (rc != SQLITE_OK) {
        std::cerr << "[DBMS ERROR] " << (err_msg ? err_msg : "unknown") << "\n";
        sqlite3_free(err_msg);   // SQLite allocates err_msg; we must free it.
        return false;
    }
    return true;
}

// ────────────────────────────────────────────────────────────────────────────
// init  — open DB, enable WAL, create tables
// ────────────────────────────────────────────────────────────────────────────

bool DatabaseManager::init(const std::string& db_path) {
    // Acquire the mutex.  std::lock_guard is RAII: it unlocks automatically
    // when this function returns (even on an early `return false`).
    std::lock_guard<std::mutex> lock(db_mutex_);

    // ┌─────────────────────────────────────────────────────────────────────┐
    // │ Step 1 — Open (or create) the SQLite database file.                │
    // │                                                                     │
    // │ sqlite3_open() creates the file if it doesn't exist.  The second   │
    // │ argument receives the connection handle we'll use everywhere else.  │
    // └─────────────────────────────────────────────────────────────────────┘
    int rc = sqlite3_open(db_path.c_str(), &db_handle_);
    if (rc != SQLITE_OK) {
        std::cerr << "[DBMS ERROR] Cannot open database '" << db_path
                  << "': " << sqlite3_errmsg(db_handle_) << "\n";
        return false;
    }
    std::cout << "[DBMS] Database file '" << db_path << "' opened.\n";

    // ┌─────────────────────────────────────────────────────────────────────┐
    // │ Step 2 — Enable WAL (Write-Ahead Logging) mode.                    │
    // │                                                                     │
    // │ WHY THIS MATTERS (explain to your professors):                      │
    // │                                                                     │
    // │ Default SQLite uses a ROLLBACK JOURNAL.  Every write locks the      │
    // │ entire database, so concurrent readers are blocked.                  │
    // │                                                                     │
    // │ WAL flips the model:                                                │
    // │   • Writes go into a separate  -wal  file (append-only log).        │
    // │   • Readers continue reading the original database file.            │
    // │   • A periodic "checkpoint" merges the WAL back into the main file. │
    // │                                                                     │
    // │ Result: readers NEVER block writers, and writers NEVER block         │
    // │ readers — exactly what we need when two OS threads are writing      │
    // │ sensor data concurrently.                                           │
    // │                                                                     │
    // │ Trade-off: WAL creates two extra files on disk (-wal and -shm),     │
    // │ and is slightly slower for write-heavy workloads on network drives. │
    // │ For a local embedded system like ours, WAL is strictly better.      │
    // └─────────────────────────────────────────────────────────────────────┘
    if (!execute_query("PRAGMA journal_mode = WAL;")) {
        std::cerr << "[DBMS ERROR] Failed to enable WAL mode.\n";
        return false;
    }
    std::cout << "[DBMS] WAL (Write-Ahead Logging) mode enabled.\n";

    // ┌─────────────────────────────────────────────────────────────────────┐
    // │ Step 2b — Set synchronous = NORMAL.                                 │
    // │                                                                     │
    // │ In WAL mode, NORMAL is safe and avoids an fsync on every commit.    │
    // │ FULL would be safer against OS crashes but costs ~10× more I/O.     │
    // └─────────────────────────────────────────────────────────────────────┘
    execute_query("PRAGMA synchronous = NORMAL;");

    // ┌─────────────────────────────────────────────────────────────────────┐
    // │ Step 3 — Create tables (DDL).                                       │
    // │                                                                     │
    // │ IF NOT EXISTS makes this idempotent — safe to call on every run.    │
    // │                                                                     │
    // │ vitals_log:   time-series CPU & RAM readings from /proc.            │
    // │ deleted_files: audit log of files deleted inside our watched dir.   │
    // └─────────────────────────────────────────────────────────────────────┘
    const char* schema_sql = R"SQL(
        CREATE TABLE IF NOT EXISTS vitals_log (
            id         INTEGER PRIMARY KEY AUTOINCREMENT,
            timestamp  TEXT    NOT NULL,
            cpu_usage  REAL    NOT NULL,
            ram_usage  REAL    NOT NULL,
            cpu_temp   REAL    DEFAULT 0.0,
            fan_speed  INTEGER DEFAULT 0
        );

        CREATE TABLE IF NOT EXISTS deleted_files (
            id         INTEGER PRIMARY KEY AUTOINCREMENT,
            timestamp  TEXT    NOT NULL,
            file_name  TEXT    NOT NULL
        );
    )SQL";

    if (!execute_query(schema_sql)) {
        std::cerr << "[DBMS ERROR] Schema creation failed.\n";
        return false;
    }

    // Idempotent schema migrations for existing DBs (silently ignore duplicate column)
    char* alter_err = nullptr;
    sqlite3_exec(db_handle_, "ALTER TABLE vitals_log ADD COLUMN cpu_temp REAL DEFAULT 0.0;", nullptr, nullptr, &alter_err);
    if (alter_err) sqlite3_free(alter_err);
    alter_err = nullptr;
    sqlite3_exec(db_handle_, "ALTER TABLE vitals_log ADD COLUMN fan_speed INTEGER DEFAULT 0;", nullptr, nullptr, &alter_err);
    if (alter_err) sqlite3_free(alter_err);

    std::cout << "[DBMS] Tables 'vitals_log' and 'deleted_files' are ready.\n";

    return true;
}

// ────────────────────────────────────────────────────────────────────────────
// log_vitals  — INSERT a CPU + RAM + Temp + Fan reading
// ────────────────────────────────────────────────────────────────────────────
// Uses a PREPARED STATEMENT (sqlite3_prepare_v2 + bind) instead of string
// concatenation.  This is critical for two reasons your professor will ask:
//
//   1. Security:  prevents SQL injection (no raw user strings in the query).
//   2. Performance: SQLite compiles the query once; subsequent calls just
//      re-bind the parameters, skipping the parser entirely.
// ────────────────────────────────────────────────────────────────────────────

bool DatabaseManager::log_vitals(double cpu_usage, double ram_usage, double cpu_temp, int fan_speed) {
    std::lock_guard<std::mutex> lock(db_mutex_);

    if (!db_handle_) {
        std::cerr << "[DBMS ERROR] log_vitals called on a closed database.\n";
        return false;
    }

    const std::string ts = current_timestamp();

    // The '?' placeholders are filled in by sqlite3_bind_* below.
    const char* sql =
        "INSERT INTO vitals_log (timestamp, cpu_usage, ram_usage, cpu_temp, fan_speed) "
        "VALUES (?, ?, ?, ?, ?);";

    sqlite3_stmt* stmt = nullptr;

    // Compile the SQL into a prepared statement.
    if (sqlite3_prepare_v2(db_handle_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        std::cerr << "[DBMS ERROR] prepare (vitals): "
                  << sqlite3_errmsg(db_handle_) << "\n";
        return false;
    }

    // Bind each parameter (1-indexed).
    // SQLITE_TRANSIENT tells SQLite to make its own copy of the string data,
    // so it's safe even if our local variable goes out of scope.
    sqlite3_bind_text  (stmt, 1, ts.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_double(stmt, 2, cpu_usage);
    sqlite3_bind_double(stmt, 3, ram_usage);
    sqlite3_bind_double(stmt, 4, cpu_temp);
    sqlite3_bind_int   (stmt, 5, fan_speed);

    // Execute the statement.  SQLITE_DONE means "finished successfully".
    bool ok = (sqlite3_step(stmt) == SQLITE_DONE);
    if (!ok) {
        std::cerr << "[DBMS ERROR] step (vitals): "
                  << sqlite3_errmsg(db_handle_) << "\n";
    }

    // Always finalise the statement to free SQLite's internal resources.
    sqlite3_finalize(stmt);
    return ok;
}

// ────────────────────────────────────────────────────────────────────────────
// log_deleted_file  — INSERT a deletion event
// ────────────────────────────────────────────────────────────────────────────

bool DatabaseManager::log_deleted_file(std::string_view file_name) {
    std::lock_guard<std::mutex> lock(db_mutex_);

    if (!db_handle_) {
        std::cerr << "[DBMS ERROR] log_deleted_file called on a closed database.\n";
        return false;
    }

    const std::string ts = current_timestamp();

    const char* sql =
        "INSERT INTO deleted_files (timestamp, file_name) "
        "VALUES (?, ?);";

    sqlite3_stmt* stmt = nullptr;

    if (sqlite3_prepare_v2(db_handle_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        std::cerr << "[DBMS ERROR] prepare (deleted_files): "
                  << sqlite3_errmsg(db_handle_) << "\n";
        return false;
    }

    sqlite3_bind_text(stmt, 1, ts.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, file_name.data(),
                      static_cast<int>(file_name.size()), SQLITE_TRANSIENT);

    bool ok = (sqlite3_step(stmt) == SQLITE_DONE);
    if (!ok) {
        std::cerr << "[DBMS ERROR] step (deleted_files): "
                  << sqlite3_errmsg(db_handle_) << "\n";
    }

    sqlite3_finalize(stmt);
    return ok;
}

// ────────────────────────────────────────────────────────────────────────────
// close  — shut down the database connection
// ────────────────────────────────────────────────────────────────────────────

void DatabaseManager::close() {
    std::lock_guard<std::mutex> lock(db_mutex_);

    if (db_handle_) {
        sqlite3_close(db_handle_);
        db_handle_ = nullptr;
        std::cout << "[DBMS] Database connection closed.\n";
    }
}
