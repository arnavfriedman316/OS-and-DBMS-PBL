// database manager handling dual backend persistence: mysql and sqlite fallback

#include "db.hpp"

#include <iostream>
#include <sstream>
#include <chrono>
#include <ctime>

// constructor and destructor
DatabaseManager::DatabaseManager() = default;

// cleans up open database handles on destruction
DatabaseManager::~DatabaseManager() {
    close();
}

// formats current system time into yyyy-mm-dd hh:mm:ss string using reentrant localtime_r
std::string DatabaseManager::current_timestamp() {
    std::time_t now_c = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm tm_buf{};
    localtime_r(&now_c, &tm_buf);

    char out[20];
    std::strftime(out, sizeof(out), "%Y-%m-%d %H:%M:%S", &tm_buf);
    return out;
}

// escapes strings using mysql_real_escape_string to block sql injection
std::string DatabaseManager::escape_mysql(std::string_view s) {
    if (!mysql_handle_ || s.empty()) return std::string(s);
    std::string out;
    out.resize(s.size() * 2 + 1);
    unsigned long len = mysql_real_escape_string(mysql_handle_, out.data(), s.data(), s.size());
    out.resize(len);
    return out;
}

// pick active backend based on DatabaseConfig, with graceful fallback to sqlite if mysql is down
bool DatabaseManager::init(const DatabaseConfig& cfg) {
    std::lock_guard<std::mutex> lock(db_mutex_);

    if (cfg.backend == BackendType::MySQL) {
        if (init_mysql(cfg)) {
            active_backend_ = BackendType::MySQL;
            return true;
        }
        if (cfg.fallback_to_sqlite) {
            std::cout << "[DBMS INFO] Tip: To set up MySQL, run: sudo mariadb < sql/setup.sql\n";
            std::cout << "[DBMS INFO] Falling back to SQLite (" << cfg.sqlite_path << ")...\n";
            if (init_sqlite(cfg.sqlite_path)) {
                active_backend_ = BackendType::SQLite;
                return true;
            }
        }
        return false;
    }

    // explicit sqlite request
    if (init_sqlite(cfg.sqlite_path)) {
        active_backend_ = BackendType::SQLite;
        return true;
    }
    return false;
}

// quick overload for backwards compatibility defaulting to mysql with fallback
bool DatabaseManager::init(const std::string& db_path) {
    DatabaseConfig cfg;
    cfg.sqlite_path = db_path;
    cfg.backend = BackendType::MySQL;  // try mysql first, fall back to db_path
    return init(cfg);
}

// connect to mysql daemon via socket or tcp and create schema
bool DatabaseManager::init_mysql(const DatabaseConfig& cfg) {
    mysql_handle_ = mysql_init(nullptr);
    if (!mysql_handle_) {
        std::cerr << "[DBMS ERROR] mysql_init failed.\n";
        return false;
    }

    unsigned int timeout = 3;
    mysql_options(mysql_handle_, MYSQL_OPT_CONNECT_TIMEOUT, &timeout);
    bool reconnect = true;
    mysql_options(mysql_handle_, MYSQL_OPT_RECONNECT, &reconnect);

    const char* socket = cfg.mysql_socket.empty() ? nullptr : cfg.mysql_socket.c_str();

    std::cout << "[DBMS] Connecting to MySQL server at " << cfg.mysql_host << ":"
              << cfg.mysql_port << " (user: " << cfg.mysql_user << ", db: " << cfg.mysql_db << ")...\n";

    if (!mysql_real_connect(mysql_handle_,
                            cfg.mysql_host.c_str(),
                            cfg.mysql_user.c_str(),
                            cfg.mysql_pass.c_str(),
                            cfg.mysql_db.c_str(),
                            cfg.mysql_port,
                            socket,
                            CLIENT_MULTI_STATEMENTS)) {
        std::cerr << "[DBMS WARNING] MySQL connection failed: " << mysql_error(mysql_handle_) << "\n";
        mysql_close(mysql_handle_);
        mysql_handle_ = nullptr;
        return false;
    }

    mysql_set_character_set(mysql_handle_, "utf8mb4");
    std::cout << "[DBMS] Successfully connected to MySQL database '" << cfg.mysql_db << "'.\n";

    return create_mysql_schema();
}

// create required tables and secondary indexes in mysql if they do not exist
bool DatabaseManager::create_mysql_schema() {
    const char* ddl = R"SQL(
        CREATE TABLE IF NOT EXISTS sessions (
            id BIGINT AUTO_INCREMENT PRIMARY KEY,
            start_time VARCHAR(25) NOT NULL,
            end_time VARCHAR(25) NULL,
            pid INT NOT NULL,
            hostname VARCHAR(255) NULL,
            clean_exit TINYINT NULL
        ) ENGINE=InnoDB;

        CREATE TABLE IF NOT EXISTS vitals_log (
            id BIGINT AUTO_INCREMENT PRIMARY KEY,
            timestamp VARCHAR(25) NOT NULL,
            cpu_usage DOUBLE NOT NULL,
            ram_usage DOUBLE NOT NULL,
            cpu_temp DOUBLE DEFAULT 0.0,
            fan_speed INT DEFAULT 0,
            session_id BIGINT NULL,
            INDEX idx_vitals_ts (timestamp),
            INDEX idx_vitals_session (session_id)
        ) ENGINE=InnoDB;

        CREATE TABLE IF NOT EXISTS deleted_files (
            id BIGINT AUTO_INCREMENT PRIMARY KEY,
            timestamp VARCHAR(25) NOT NULL,
            file_name VARCHAR(512) NOT NULL,
            event_type VARCHAR(32) DEFAULT 'DELETED',
            is_dir TINYINT DEFAULT 0,
            new_name VARCHAR(512) NULL,
            session_id BIGINT NULL,
            INDEX idx_files_ts (timestamp),
            INDEX idx_files_session (session_id)
        ) ENGINE=InnoDB;

        CREATE TABLE IF NOT EXISTS alerts (
            id BIGINT AUTO_INCREMENT PRIMARY KEY,
            timestamp VARCHAR(25) NOT NULL,
            metric VARCHAR(64) NOT NULL,
            value DOUBLE NOT NULL,
            threshold DOUBLE NOT NULL,
            state VARCHAR(32) NOT NULL,
            session_id BIGINT NULL,
            INDEX idx_alerts_ts (timestamp),
            INDEX idx_alerts_session (session_id)
        ) ENGINE=InnoDB;
    )SQL";

    if (mysql_query(mysql_handle_, ddl) != 0) {
        std::cerr << "[DBMS ERROR] MySQL schema creation failed: " << mysql_error(mysql_handle_) << "\n";
        return false;
    }

    // consume all multi-statement results from ddl batch
    while (mysql_next_result(mysql_handle_) == 0) {
        MYSQL_RES* res = mysql_store_result(mysql_handle_);
        if (res) mysql_free_result(res);
    }

    std::cout << "[DBMS] MySQL tables 'sessions', 'vitals_log', 'deleted_files', 'alerts' verified.\n";
    return true;
}

// execute raw sqlite query string and log errors if it fails
bool DatabaseManager::execute_sqlite_query(const char* sql) {
    char* err_msg = nullptr;
    int rc = sqlite3_exec(sqlite_handle_, sql, nullptr, nullptr, &err_msg);
    if (rc != SQLITE_OK) {
        std::cerr << "[DBMS ERROR] SQLite query failed: " << (err_msg ? err_msg : "unknown") << "\n";
        sqlite3_free(err_msg);
        return false;
    }
    return true;
}

// check pragma table_info and run alter table if an older database lacks new columns
bool DatabaseManager::add_sqlite_column_if_missing(const char* table, const char* column, const char* decl) {
    std::string pragma = std::string("PRAGMA table_info(") + table + ");";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(sqlite_handle_, pragma.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
        return false;
    }

    bool found = false;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        const auto* name = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
        if (name && std::string(name) == column) {
            found = true;
            break;
        }
    }
    sqlite3_finalize(stmt);

    if (found) return true;

    std::string alter = std::string("ALTER TABLE ") + table + " ADD COLUMN " + column + " " + decl + ";";
    return execute_sqlite_query(alter.c_str());
}

// compile a prepared statement and check return code
bool DatabaseManager::prepare_sqlite_stmt(const char* sql, sqlite3_stmt** out) {
    if (sqlite3_prepare_v2(sqlite_handle_, sql, -1, out, nullptr) != SQLITE_OK) {
        std::cerr << "[DBMS ERROR] SQLite prepare failed: " << sqlite3_errmsg(sqlite_handle_)
                  << "\n             SQL: " << sql << "\n";
        return false;
    }
    return true;
}

// open local sqlite file, turn on wal mode for concurrency, and prepare statements
bool DatabaseManager::init_sqlite(const std::string& db_path) {
    int rc = sqlite3_open(db_path.c_str(), &sqlite_handle_);
    if (rc != SQLITE_OK) {
        std::cerr << "[DBMS ERROR] Cannot open SQLite database '" << db_path
                  << "': " << sqlite3_errmsg(sqlite_handle_) << "\n";
        sqlite3_close(sqlite_handle_);
        sqlite_handle_ = nullptr;
        return false;
    }
    std::cout << "[DBMS] SQLite database file '" << db_path << "' opened.\n";

    sqlite3_busy_timeout(sqlite_handle_, 2000);

    // wal mode allows concurrent readers while daemon writes
    if (!execute_sqlite_query("PRAGMA journal_mode = WAL;")) {
        std::cerr << "[DBMS ERROR] Failed to enable SQLite WAL mode.\n";
        return false;
    }
    execute_sqlite_query("PRAGMA synchronous = NORMAL;");

    return create_sqlite_schema();
}

// create tables, migrate missing columns, and prepare insert statements in sqlite
bool DatabaseManager::create_sqlite_schema() {
    const char* schema_sql = R"SQL(
        CREATE TABLE IF NOT EXISTS sessions (
            id          INTEGER PRIMARY KEY AUTOINCREMENT,
            start_time  TEXT    NOT NULL,
            end_time    TEXT,
            pid         INTEGER NOT NULL,
            hostname    TEXT,
            clean_exit  INTEGER
        );

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

        CREATE TABLE IF NOT EXISTS alerts (
            id          INTEGER PRIMARY KEY AUTOINCREMENT,
            timestamp   TEXT    NOT NULL,
            metric      TEXT    NOT NULL,
            value       REAL    NOT NULL,
            threshold   REAL    NOT NULL,
            state       TEXT    NOT NULL,
            session_id  INTEGER REFERENCES sessions(id)
        );
    )SQL";

    if (!execute_sqlite_query(schema_sql)) {
        return false;
    }

    // handle schema migrations on existing database files
    add_sqlite_column_if_missing("vitals_log",    "cpu_temp",   "REAL DEFAULT 0.0");
    add_sqlite_column_if_missing("vitals_log",    "fan_speed",  "INTEGER DEFAULT 0");
    add_sqlite_column_if_missing("vitals_log",    "session_id", "INTEGER REFERENCES sessions(id)");
    add_sqlite_column_if_missing("deleted_files", "event_type", "TEXT DEFAULT 'DELETED'");
    add_sqlite_column_if_missing("deleted_files", "is_dir",     "INTEGER DEFAULT 0");
    add_sqlite_column_if_missing("deleted_files", "new_name",   "TEXT");
    add_sqlite_column_if_missing("deleted_files", "session_id", "INTEGER REFERENCES sessions(id)");

    // create indexes for faster time-series and session filtering
    execute_sqlite_query("CREATE INDEX IF NOT EXISTS idx_vitals_ts      ON vitals_log(timestamp);");
    execute_sqlite_query("CREATE INDEX IF NOT EXISTS idx_vitals_session ON vitals_log(session_id);");
    execute_sqlite_query("CREATE INDEX IF NOT EXISTS idx_files_ts       ON deleted_files(timestamp);");
    execute_sqlite_query("CREATE INDEX IF NOT EXISTS idx_alerts_ts      ON alerts(timestamp);");

    // pre-compile insert statements for high-throughput logging
    bool ok =
        prepare_sqlite_stmt("INSERT INTO vitals_log (timestamp, cpu_usage, ram_usage, cpu_temp, fan_speed, session_id) VALUES (?, ?, ?, ?, ?, ?);", &stmt_vitals_) &&
        prepare_sqlite_stmt("INSERT INTO deleted_files (timestamp, file_name, event_type, is_dir, new_name, session_id) VALUES (?, ?, ?, ?, ?, ?);", &stmt_file_) &&
        prepare_sqlite_stmt("INSERT INTO alerts (timestamp, metric, value, threshold, state, session_id) VALUES (?, ?, ?, ?, ?, ?);", &stmt_alert_);

    if (!ok) {
        finalize_sqlite_statements();
        return false;
    }

    std::cout << "[DBMS] SQLite tables & prepared statements ready (WAL mode).\n";
    return true;
}

// find previous runs that were interrupted without closing cleanly
std::vector<SessionInfo> DatabaseManager::find_open_sessions() {
    std::lock_guard<std::mutex> lock(db_mutex_);
    std::vector<SessionInfo> result;

    if (active_backend_ == BackendType::MySQL) {
        if (!mysql_handle_) return result;
        const char* q =
            "SELECT s.id, s.start_time, s.pid, IFNULL(s.hostname, ''), "
            "       IFNULL(MAX(v.timestamp), s.start_time) AS last_seen "
            "FROM sessions s LEFT JOIN vitals_log v ON v.session_id = s.id "
            "WHERE s.end_time IS NULL "
            "GROUP BY s.id, s.start_time, s.pid, s.hostname ORDER BY s.id;";

        if (mysql_query(mysql_handle_, q) == 0) {
            MYSQL_RES* res = mysql_store_result(mysql_handle_);
            if (res) {
                MYSQL_ROW row;
                while ((row = mysql_fetch_row(res))) {
                    SessionInfo s;
                    s.id         = std::stoll(row[0]);
                    s.start_time = row[1] ? row[1] : "";
                    s.pid        = row[2] ? std::stoi(row[2]) : 0;
                    s.hostname   = row[3] ? row[3] : "";
                    s.last_seen  = row[4] ? row[4] : "";
                    result.push_back(std::move(s));
                }
                mysql_free_result(res);
            }
        }
        return result;
    }

    // fallback query for sqlite
    if (!sqlite_handle_) return result;
    const char* sql =
        "SELECT s.id, s.start_time, s.pid, IFNULL(s.hostname, ''), "
        "       IFNULL(MAX(v.timestamp), s.start_time) "
        "FROM sessions s LEFT JOIN vitals_log v ON v.session_id = s.id "
        "WHERE s.end_time IS NULL "
        "GROUP BY s.id ORDER BY s.id;";

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(sqlite_handle_, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            SessionInfo s;
            s.id         = sqlite3_column_int64(stmt, 0);
            s.start_time = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
            s.pid        = sqlite3_column_int(stmt, 2);
            s.hostname   = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3));
            s.last_seen  = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 4));
            result.push_back(std::move(s));
        }
        sqlite3_finalize(stmt);
    }
    return result;
}

// mark an abandoned session as crashed with clean_exit = 0
bool DatabaseManager::mark_session_crashed(const SessionInfo& s) {
    std::lock_guard<std::mutex> lock(db_mutex_);

    if (active_backend_ == BackendType::MySQL) {
        if (!mysql_handle_) return false;
        std::string q = "UPDATE sessions SET end_time = '" + escape_mysql(s.last_seen) +
                        "', clean_exit = 0 WHERE id = " + std::to_string(s.id) + ";";
        return mysql_query(mysql_handle_, q.c_str()) == 0;
    }

    if (!sqlite_handle_) return false;
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(sqlite_handle_,
            "UPDATE sessions SET end_time = ?, clean_exit = 0 WHERE id = ?;",
            -1, &stmt, nullptr) != SQLITE_OK) {
        return false;
    }
    sqlite3_bind_text (stmt, 1, s.last_seen.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 2, s.id);
    bool ok = (sqlite3_step(stmt) == SQLITE_DONE);
    sqlite3_finalize(stmt);
    return ok;
}

// write a new row in sessions table for this running process
bool DatabaseManager::begin_session(int pid, const std::string& hostname) {
    std::lock_guard<std::mutex> lock(db_mutex_);
    const std::string ts = current_timestamp();

    if (active_backend_ == BackendType::MySQL) {
        if (!mysql_handle_) return false;
        std::string q = "INSERT INTO sessions (start_time, pid, hostname) VALUES ('" +
                        ts + "', " + std::to_string(pid) + ", '" + escape_mysql(hostname) + "');";
        if (mysql_query(mysql_handle_, q.c_str()) == 0) {
            session_id_ = mysql_insert_id(mysql_handle_);
            return true;
        }
        return false;
    }

    if (!sqlite_handle_) return false;
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(sqlite_handle_,
            "INSERT INTO sessions (start_time, pid, hostname) VALUES (?, ?, ?);",
            -1, &stmt, nullptr) != SQLITE_OK) {
        return false;
    }
    sqlite3_bind_text(stmt, 1, ts.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int (stmt, 2, pid);
    sqlite3_bind_text(stmt, 3, hostname.c_str(), -1, SQLITE_TRANSIENT);
    bool ok = (sqlite3_step(stmt) == SQLITE_DONE);
    sqlite3_finalize(stmt);

    if (ok) session_id_ = sqlite3_last_insert_rowid(sqlite_handle_);
    return ok;
}

// mark current session cleanly closed with clean_exit = 1
bool DatabaseManager::end_session() {
    std::lock_guard<std::mutex> lock(db_mutex_);
    if (session_id_ == 0) return false;
    const std::string ts = current_timestamp();

    if (active_backend_ == BackendType::MySQL) {
        if (!mysql_handle_) return false;
        std::string q = "UPDATE sessions SET end_time = '" + ts +
                        "', clean_exit = 1 WHERE id = " + std::to_string(session_id_) + ";";
        return mysql_query(mysql_handle_, q.c_str()) == 0;
    }

    if (!sqlite_handle_) return false;
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(sqlite_handle_,
            "UPDATE sessions SET end_time = ?, clean_exit = 1 WHERE id = ?;",
            -1, &stmt, nullptr) != SQLITE_OK) {
        return false;
    }
    sqlite3_bind_text (stmt, 1, ts.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 2, session_id_);
    bool ok = (sqlite3_step(stmt) == SQLITE_DONE);
    sqlite3_finalize(stmt);
    return ok;
}

// append a telemetry sample to vitals_log table
bool DatabaseManager::log_vitals(double cpu_usage, double ram_usage, double cpu_temp, int fan_speed) {
    std::lock_guard<std::mutex> lock(db_mutex_);
    const std::string ts = current_timestamp();

    if (active_backend_ == BackendType::MySQL) {
        if (!mysql_handle_) return false;
        std::string s_id = (session_id_ > 0) ? std::to_string(session_id_) : "NULL";
        std::string q = "INSERT INTO vitals_log (timestamp, cpu_usage, ram_usage, cpu_temp, fan_speed, session_id) VALUES ('" +
                        ts + "', " + std::to_string(cpu_usage) + ", " + std::to_string(ram_usage) + ", " +
                        std::to_string(cpu_temp) + ", " + std::to_string(fan_speed) + ", " + s_id + ");";
        return mysql_query(mysql_handle_, q.c_str()) == 0;
    }

    if (!sqlite_handle_ || !stmt_vitals_) return false;
    sqlite3_reset(stmt_vitals_);
    sqlite3_clear_bindings(stmt_vitals_);

    sqlite3_bind_text  (stmt_vitals_, 1, ts.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_double(stmt_vitals_, 2, cpu_usage);
    sqlite3_bind_double(stmt_vitals_, 3, ram_usage);
    sqlite3_bind_double(stmt_vitals_, 4, cpu_temp);
    sqlite3_bind_int   (stmt_vitals_, 5, fan_speed);
    if (session_id_ > 0) sqlite3_bind_int64(stmt_vitals_, 6, session_id_);
    else                sqlite3_bind_null (stmt_vitals_, 6);

    return (sqlite3_step(stmt_vitals_) == SQLITE_DONE);
}

// insert a file event like deletion, rename, or move into deleted_files table
bool DatabaseManager::log_file_event(std::string_view file_name, std::string_view event_type,
                                     bool is_dir, std::string_view new_name) {
    std::lock_guard<std::mutex> lock(db_mutex_);
    const std::string ts = current_timestamp();

    if (active_backend_ == BackendType::MySQL) {
        if (!mysql_handle_) return false;
        std::string s_id = (session_id_ > 0) ? std::to_string(session_id_) : "NULL";
        std::string new_name_sql = new_name.empty() ? "NULL" : ("'" + escape_mysql(new_name) + "'");
        std::string q = "INSERT INTO deleted_files (timestamp, file_name, event_type, is_dir, new_name, session_id) VALUES ('" +
                        ts + "', '" + escape_mysql(file_name) + "', '" + escape_mysql(event_type) + "', " +
                        (is_dir ? "1" : "0") + ", " + new_name_sql + ", " + s_id + ");";
        return mysql_query(mysql_handle_, q.c_str()) == 0;
    }

    if (!sqlite_handle_ || !stmt_file_) return false;
    sqlite3_reset(stmt_file_);
    sqlite3_clear_bindings(stmt_file_);

    sqlite3_bind_text(stmt_file_, 1, ts.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt_file_, 2, file_name.data(), static_cast<int>(file_name.size()), SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt_file_, 3, event_type.data(), static_cast<int>(event_type.size()), SQLITE_TRANSIENT);
    sqlite3_bind_int (stmt_file_, 4, is_dir ? 1 : 0);
    if (new_name.empty()) sqlite3_bind_null(stmt_file_, 5);
    else sqlite3_bind_text(stmt_file_, 5, new_name.data(), static_cast<int>(new_name.size()), SQLITE_TRANSIENT);
    if (session_id_ > 0) sqlite3_bind_int64(stmt_file_, 6, session_id_);
    else                sqlite3_bind_null (stmt_file_, 6);

    return (sqlite3_step(stmt_file_) == SQLITE_DONE);
}

// backward compatible alias for file deletion
bool DatabaseManager::log_deleted_file(std::string_view file_name) {
    return log_file_event(file_name, "DELETED");
}

// log an alert trigger or resolve state transition
bool DatabaseManager::log_alert(std::string_view metric, double value, double threshold,
                                std::string_view state) {
    std::lock_guard<std::mutex> lock(db_mutex_);
    const std::string ts = current_timestamp();

    if (active_backend_ == BackendType::MySQL) {
        if (!mysql_handle_) return false;
        std::string s_id = (session_id_ > 0) ? std::to_string(session_id_) : "NULL";
        std::string q = "INSERT INTO alerts (timestamp, metric, value, threshold, state, session_id) VALUES ('" +
                        ts + "', '" + escape_mysql(metric) + "', " + std::to_string(value) + ", " +
                        std::to_string(threshold) + ", '" + escape_mysql(state) + "', " + s_id + ");";
        return mysql_query(mysql_handle_, q.c_str()) == 0;
    }

    if (!sqlite_handle_ || !stmt_alert_) return false;
    sqlite3_reset(stmt_alert_);
    sqlite3_clear_bindings(stmt_alert_);

    sqlite3_bind_text  (stmt_alert_, 1, ts.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text  (stmt_alert_, 2, metric.data(), static_cast<int>(metric.size()), SQLITE_TRANSIENT);
    sqlite3_bind_double(stmt_alert_, 3, value);
    sqlite3_bind_double(stmt_alert_, 4, threshold);
    sqlite3_bind_text  (stmt_alert_, 5, state.data(), static_cast<int>(state.size()), SQLITE_TRANSIENT);
    if (session_id_ > 0) sqlite3_bind_int64(stmt_alert_, 6, session_id_);
    else                sqlite3_bind_null (stmt_alert_, 6);

    return (sqlite3_step(stmt_alert_) == SQLITE_DONE);
}

// delete telemetry older than given days to keep disk usage reasonable
int DatabaseManager::purge_old_vitals(int days) {
    std::lock_guard<std::mutex> lock(db_mutex_);
    if (days <= 0) return 0;

    if (active_backend_ == BackendType::MySQL) {
        if (!mysql_handle_) return -1;
        std::string q = "DELETE FROM vitals_log WHERE timestamp < DATE_SUB(NOW(), INTERVAL " +
                        std::to_string(days) + " DAY);";
        if (mysql_query(mysql_handle_, q.c_str()) == 0) {
            return static_cast<int>(mysql_affected_rows(mysql_handle_));
        }
        return -1;
    }

    if (!sqlite_handle_) return -1;
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(sqlite_handle_,
            "DELETE FROM vitals_log WHERE timestamp < datetime('now', 'localtime', ?);",
            -1, &stmt, nullptr) != SQLITE_OK) {
        return -1;
    }
    const std::string modifier = "-" + std::to_string(days) + " days";
    sqlite3_bind_text(stmt, 1, modifier.c_str(), -1, SQLITE_TRANSIENT);

    int deleted = -1;
    if (sqlite3_step(stmt) == SQLITE_DONE) {
        deleted = sqlite3_changes(sqlite_handle_);
    }
    sqlite3_finalize(stmt);
    return deleted;
}

// clean up compiled sqlite statements
void DatabaseManager::finalize_sqlite_statements() {
    sqlite3_finalize(stmt_vitals_);
    sqlite3_finalize(stmt_file_);
    sqlite3_finalize(stmt_alert_);
    stmt_vitals_ = stmt_file_ = stmt_alert_ = nullptr;
}

// close mysql and sqlite connections safely
void DatabaseManager::close() {
    std::lock_guard<std::mutex> lock(db_mutex_);

    if (mysql_handle_) {
        mysql_close(mysql_handle_);
        mysql_handle_ = nullptr;
        std::cout << "[DBMS] MySQL connection closed.\n";
    }

    if (sqlite_handle_) {
        finalize_sqlite_statements();
        sqlite3_close(sqlite_handle_);
        sqlite_handle_ = nullptr;
        std::cout << "[DBMS] SQLite database connection closed.\n";
    }
}
