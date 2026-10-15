#pragma once

#include <string>
#include <string_view>
#include <vector>
#include <mutex>

// database client headers
#include <mysql/mysql.h>
#include <sqlite3.h>

// database manager supporting mysql and local sqlite fallback

// which database engine is currently carrying the workload
enum class BackendType {
    MySQL,
    SQLite
};

// connection options for both database backends
struct DatabaseConfig {
    BackendType backend{BackendType::MySQL};  // default to mysql
    std::string mysql_host{"127.0.0.1"};
    int         mysql_port{3306};
    std::string mysql_user{"blackbox_writer"};
    std::string mysql_pass{"writer_pass"};
    std::string mysql_db{"blackbox"};
    std::string mysql_socket{"/run/mysqld/mysqld.sock"};
    std::string sqlite_path{"blackbox.db"};
    bool        fallback_to_sqlite{true};      // auto switch to sqlite if mysql is unreachable
};

// metadata for an execution run used to detect crashes across reboots
struct SessionInfo {
    long long   id{0};
    std::string start_time;
    std::string last_seen;   // timestamp of newest vitals row logged before shutdown
    int         pid{0};
    std::string hostname;
};

// thread-safe wrapper around database operations protected by a mutex
class DatabaseManager {
public:
    DatabaseManager();
    ~DatabaseManager();

    // don't allow copying because handles shouldn't be duplicated
    DatabaseManager(const DatabaseManager&)            = delete;
    DatabaseManager& operator=(const DatabaseManager&) = delete;

    // initialize database connection with explicit DatabaseConfig struct
    bool init(const DatabaseConfig& cfg);

    // quick overload for sqlite or default fallback path
    bool init(const std::string& db_path = "blackbox.db");

    // active backend helpers
    BackendType backend() const { return active_backend_; }
    std::string backend_name() const { return active_backend_ == BackendType::MySQL ? "MySQL" : "SQLite"; }
    long long   session_id() const { return session_id_; }

    // scan for sessions that never set end_time so we can report unexpected crashes
    std::vector<SessionInfo> find_open_sessions();

    // flag an abandoned session as crashed and stamp last_seen as its end_time
    bool mark_session_crashed(const SessionInfo& s);

    // register this daemon process run in sessions table
    bool begin_session(int pid, const std::string& hostname);

    // mark current session as cleanly exited on SIGINT or SIGTERM
    bool end_session();

    // insert sensor readings into vitals_log table
    bool log_vitals(double cpu_usage, double ram_usage, double cpu_temp = 0.0, int fan_speed = 0);

    // insert filesystem audit entry from inotify watcher
    bool log_file_event(std::string_view file_name, std::string_view event_type,
                        bool is_dir = false, std::string_view new_name = {});

    // backward compatible wrapper for file deletions
    bool log_deleted_file(std::string_view file_name);

    // insert threshold breach or resolution into alerts table
    bool log_alert(std::string_view metric, double value, double threshold, std::string_view state);

    // delete vitals older than given days to keep database compact
    int purge_old_vitals(int days);

    // shut down connection handles cleanly
    void close();

private:
    std::mutex   db_mutex_;                    // mutex serializing queries so concurrent threads don't collide
    BackendType  active_backend_{BackendType::SQLite};
    long long    session_id_{0};               // active session id generated at startup

    // raw mysql client connection handle
    MYSQL*       mysql_handle_{nullptr};

    // sqlite connection pointer and prepared statements for fast inserts
    sqlite3*      sqlite_handle_{nullptr};
    sqlite3_stmt* stmt_vitals_{nullptr};
    sqlite3_stmt* stmt_file_{nullptr};
    sqlite3_stmt* stmt_alert_{nullptr};

    // internal helpers for backend setup and schema creation
    bool init_mysql(const DatabaseConfig& cfg);
    bool init_sqlite(const std::string& db_path);
    bool create_mysql_schema();
    bool create_sqlite_schema();

    // helpers for sqlite statement execution and migration
    bool execute_sqlite_query(const char* sql);
    bool add_sqlite_column_if_missing(const char* table, const char* column, const char* decl);
    bool prepare_sqlite_stmt(const char* sql, sqlite3_stmt** out);
    void finalize_sqlite_statements();

    // escape strings to stop sql injection attacks
    std::string escape_mysql(std::string_view s);
    // format local time string yyyy-mm-dd hh:mm:ss
    static std::string current_timestamp();
};
