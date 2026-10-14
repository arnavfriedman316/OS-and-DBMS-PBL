// ============================================================================
// db.hpp — Database Manager Interface (MySQL / MariaDB + SQLite Fallback)
// ============================================================================
// System Blackbox — Phase 2 (50% Completion Milestone)
// University PBL: Operating Systems + Database Management Systems
//
// PURPOSE:
//   Provides a thread-safe C++ wrapper supporting both enterprise relational
//   databases (MySQL / MariaDB) and lightweight embedded databases (SQLite3).
//   Every public method acquires a mutex before touching database handles,
//   ensuring concurrent OS threads (sensor telemetry & inotify kernel events)
//   write safely without data races or corruption.
//
// DBMS CONCEPTS DEMONSTRATED:
//   • Client-Server DBMS Architecture (MySQL via libmariadb / C client library)
//   • Embedded ACID Database (SQLite3 in Write-Ahead Logging WAL mode)
//   • Principle of Least Privilege (Separate Writer and Reader user roles)
//   • SQL Injection Prevention (Parameterized statements & mysql_real_escape_string)
//   • Secondary B-Tree Indexes (Optimized time-series lookups on timestamp/session_id)
//   • Crash Recovery & Unclosed Session Accounting (clean_exit flag tracking)
//   • Automatic Data Retention & Garbage Collection (DATE_SUB purging)
//
// OS CONCEPTS DEMONSTRATED:
//   • Multithread Synchronization (std::mutex critical sections & RAII lock guards)
//   • Process Accounting & State Recovery (Tracking PID & ungraceful terminations)
// ============================================================================

#pragma once

#include <string>
#include <string_view>
#include <vector>
#include <mutex>

// Database client headers
#include <mysql/mysql.h>
#include <sqlite3.h>

// Supported database backends
enum class BackendType {
    MySQL,
    SQLite
};

// Configuration options for database connectivity
struct DatabaseConfig {
    BackendType backend{BackendType::MySQL};  // Default to MySQL
    std::string mysql_host{"127.0.0.1"};
    int         mysql_port{3306};
    std::string mysql_user{"blackbox_writer"};
    std::string mysql_pass{"writer_pass"};
    std::string mysql_db{"blackbox"};
    std::string mysql_socket{"/run/mysqld/mysqld.sock"};
    std::string sqlite_path{"blackbox.db"};
    bool        fallback_to_sqlite{true};      // Graceful fallback if MySQL is unreachable
};

// Represents one execution session of the daemon (used for crash forensics)
struct SessionInfo {
    long long   id{0};
    std::string start_time;
    std::string last_seen;   // timestamp of the latest vitals row recorded in this session
    int         pid{0};
    std::string hostname;
};

// Thread-safe database manager
class DatabaseManager {
public:
    DatabaseManager();
    ~DatabaseManager();

    // Prevent copying (handles should never be duplicated)
    DatabaseManager(const DatabaseManager&)            = delete;
    DatabaseManager& operator=(const DatabaseManager&) = delete;

    // Initializes database connection using the provided configuration
    bool init(const DatabaseConfig& cfg);

    // Convenience overload for backwards compatibility (defaults to SQLite or config path)
    bool init(const std::string& db_path = "blackbox.db");

    // Query active backend info
    BackendType backend() const { return active_backend_; }
    std::string backend_name() const { return active_backend_ == BackendType::MySQL ? "MySQL" : "SQLite"; }
    long long   session_id() const { return session_id_; }

    // ── Sessions & Crash Detection ──────────────────────────────────────────
    // Returns sessions with end_time IS NULL (crashed or abruptly terminated)
    std::vector<SessionInfo> find_open_sessions();

    // Marks an abandoned session as crashed (clean_exit = 0, end_time = last_seen)
    bool mark_session_crashed(const SessionInfo& s);

    // Starts a new session record for current daemon PID
    bool begin_session(int pid, const std::string& hostname);

    // Marks current session cleanly completed (clean_exit = 1, end_time = NOW)
    bool end_session();

    // ── Telemetry & Forensic Event Logging ──────────────────────────────────
    // Inserts a hardware telemetry sample (CPU, RAM, Temp, Fan)
    bool log_vitals(double cpu_usage, double ram_usage, double cpu_temp = 0.0, int fan_speed = 0);

    // Inserts a kernel filesystem event (DELETED, MOVED_OUT, RENAMED, etc.)
    bool log_file_event(std::string_view file_name, std::string_view event_type,
                        bool is_dir = false, std::string_view new_name = {});

    // Backwards-compatible alias for log_file_event(name, "DELETED")
    bool log_deleted_file(std::string_view file_name);

    // Inserts a threshold crossing event (TRIGGERED / RESOLVED)
    bool log_alert(std::string_view metric, double value, double threshold, std::string_view state);

    // ── Maintenance ─────────────────────────────────────────────────────────
    // Deletes vitals older than `days`. Returns number of rows deleted (-1 on error)
    int purge_old_vitals(int days);

    // Safely closes open database handles
    void close();

private:
    std::mutex   db_mutex_;                    // Mutex serializing database writes across threads
    BackendType  active_backend_{BackendType::SQLite};
    long long    session_id_{0};               // Current session ID in the database

    // MySQL connection state
    MYSQL*       mysql_handle_{nullptr};

    // SQLite connection state & prepared statements
    sqlite3*      sqlite_handle_{nullptr};
    sqlite3_stmt* stmt_vitals_{nullptr};
    sqlite3_stmt* stmt_file_{nullptr};
    sqlite3_stmt* stmt_alert_{nullptr};

    // Internal initialization helpers
    bool init_mysql(const DatabaseConfig& cfg);
    bool init_sqlite(const std::string& db_path);
    bool create_mysql_schema();
    bool create_sqlite_schema();

    // Helpers
    bool execute_sqlite_query(const char* sql);
    bool add_sqlite_column_if_missing(const char* table, const char* column, const char* decl);
    bool prepare_sqlite_stmt(const char* sql, sqlite3_stmt** out);
    void finalize_sqlite_statements();

    std::string escape_mysql(std::string_view s);
    static std::string current_timestamp();
};
