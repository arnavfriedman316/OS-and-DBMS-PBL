// ============================================================================
// db.hpp — Database Manager Interface
// ============================================================================
// System Blackbox — Phase 1
// University PBL: Operating Systems + Database Management Systems
//
// PURPOSE:
//   Provides a thread-safe C++ wrapper around SQLite3.  Every public method
//   acquires a mutex before touching the database handle, which lets our
//   hardware-sensor thread and file-monitor thread write concurrently
//   without corrupting the database.
//
// DBMS CONCEPTS DEMONSTRATED:
//   • WAL (Write-Ahead Logging)  — readers never block writers.
//   • Prepared statements        — parameterised queries prevent SQL injection.
//   • ACID guarantees            — each INSERT is an implicit transaction.
//
// OS CONCEPTS DEMONSTRATED:
//   • Mutex-based critical sections — serialise access to a shared resource.
//   • RAII (Resource Acquisition Is Initialisation) — the destructor
//     guarantees the DB handle is released even if an exception is thrown.
// ============================================================================

#pragma once          // Modern include-guard (supported by every major compiler)

#include <string>
#include <string_view>
#include <mutex>
#include <sqlite3.h>

class DatabaseManager {
public:
    // ── Construction / Destruction ──────────────────────────────────────────
    DatabaseManager();
    ~DatabaseManager();

    // Prevent copying — only ONE object should own the sqlite3* handle.
    DatabaseManager(const DatabaseManager&)            = delete;
    DatabaseManager& operator=(const DatabaseManager&) = delete;

    // ── Core API ────────────────────────────────────────────────────────────

    /// Opens (or creates) the database file, enables WAL mode, and
    /// provisions the schema (vitals_log + deleted_files tables).
    /// @param db_path  Filesystem path to the SQLite file (default: blackbox.db).
    /// @return true on success, false on any error.
    bool init(const std::string& db_path = "blackbox.db");

    /// Inserts a CPU + RAM + Temp + Fan reading into the vitals_log table.
    /// @param cpu_usage  CPU utilisation percentage  [0.0 – 100.0].
    /// @param ram_usage  RAM utilisation percentage  [0.0 – 100.0].
    /// @param cpu_temp   CPU temperature in Celsius  (default: 0.0).
    /// @param fan_speed  Fan speed in RPM            (default: 0).
    /// @return true on successful INSERT.
    bool log_vitals(double cpu_usage, double ram_usage, double cpu_temp = 0.0, int fan_speed = 0);

    /// Inserts a deleted-file event into the deleted_files table.
    /// @param file_name  Name of the file that was deleted (from inotify).
    /// @return true on successful INSERT.
    bool log_deleted_file(std::string_view file_name);

    /// Cleanly closes the database connection.
    void close();

private:
    sqlite3*    db_handle_{nullptr};   // Raw SQLite connection pointer
    std::mutex  db_mutex_;             // Guards ALL database operations

    /// Convenience wrapper: executes a one-shot SQL string (DDL / PRAGMA).
    bool execute_query(const char* sql);

    /// Returns the current wall-clock time as an ISO-8601 string
    /// (e.g. "2026-08-31 19:05:00").
    static std::string current_timestamp();
};
