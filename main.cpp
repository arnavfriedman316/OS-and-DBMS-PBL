// system blackbox main daemon entrypoint and worker thread orchestration

#include "db.hpp"
#include "sensors.hpp"
#include "alerts.hpp"

#include <iostream>
#include <sstream>
#include <iomanip>
#include <thread>
#include <atomic>
#include <mutex>
#include <chrono>
#include <csignal>
#include <filesystem>
#include <fstream>
#include <unordered_map>
#include <cstring>
#include <cerrno>

#include <sys/inotify.h>
#include <poll.h>
#include <unistd.h>

namespace fs = std::filesystem;
using namespace std::chrono_literals;

// flag for threads to exit loops when a termination signal arrives
static std::atomic<bool> g_running{true};
// stores the numeric signal that told us to shut down
static std::atomic<int>  g_signal{0};

// mutex so output from sensor and inotify threads don't get interleaved
static std::mutex g_out_mutex;
// thread-safe console output printer
static void say(const std::string& line, bool to_stderr = false) {
    std::lock_guard<std::mutex> lock(g_out_mutex);
    (to_stderr ? std::cerr : std::cout) << line << std::endl;
}

// async-signal-safe handler that just sets atomics on SIGINT or SIGTERM
static void signal_handler(int signum) {
    g_signal.store(signum);
    g_running.store(false);
}

// runtime configuration parsed from blackbox.conf and cli arguments
struct Config {
    std::string db_backend     = "mysql"; // "mysql" or "sqlite"
    std::string mysql_host     = "127.0.0.1";
    int         mysql_port     = 3306;
    std::string mysql_user     = "blackbox_writer";
    std::string mysql_pass     = "writer_pass";
    std::string mysql_db       = "blackbox";
    std::string mysql_socket   = "/run/mysqld/mysqld.sock";
    bool        fallback_sqlite= true;

    std::string db_path;           // default: <folder of the binary>/blackbox.db
    std::string watch_dir;         // default: <folder of the binary>/test_watch
    double interval      = 1.0;    // seconds between sensor samples
    int    retention_days = 7;     // purge vitals older than this (0 = keep forever)
    double cpu_alert     = 90.0;   // %   (0 disables)
    double ram_alert     = 90.0;   // %   (0 disables)
    double temp_alert    = 85.0;   // celsius (0 disables)
    bool   quiet         = false;  // don't print a line for every sample
};

// print available command line options
static void print_usage(const char* prog) {
    std::cout <<
        "Usage: " << prog << " [options]\n"
        "  --backend [mysql|sqlite] Database engine (default: mysql with sqlite fallback)\n"
        "  --mysql-host HOST        MySQL host (default: 127.0.0.1)\n"
        "  --mysql-port PORT        MySQL port (default: 3306)\n"
        "  --mysql-user USER        MySQL username (default: blackbox_writer)\n"
        "  --mysql-pass PASS        MySQL password (default: writer_pass)\n"
        "  --mysql-db DB            MySQL database name (default: blackbox)\n"
        "  --no-fallback            Do not fall back to SQLite if MySQL fails\n"
        "  --db PATH                SQLite database file (default: blackbox.db)\n"
        "  --watch DIR              Directory to audit (default: test_watch)\n"
        "  --interval SECONDS       Sensor sampling interval (default: 1, min 0.2)\n"
        "  --retention-days N       Purge vitals older than N days (default: 7)\n"
        "  --cpu-alert PCT          CPU alert threshold (default: 90)\n"
        "  --ram-alert PCT          RAM alert threshold (default: 90)\n"
        "  --temp-alert C           Temperature alert threshold (default: 85)\n"
        "  --quiet                  Only print alerts and file events\n"
        "  -h, --help               Show this help\n";
}

// locate directory containing the running executable using /proc/self/exe
static fs::path exe_dir() {
    char buf[4096];
    ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n <= 0) return fs::current_path();
    buf[n] = '\0';
    return fs::path(buf).parent_path();
}

// parse key = value settings from blackbox.conf
static void load_config_file(const fs::path& conf_path, Config& cfg) {
    std::ifstream ifs(conf_path);
    if (!ifs.is_open()) return;

    std::string line;
    while (std::getline(ifs, line)) {
        // strip comments and whitespace
        auto hash_pos = line.find('#');
        if (hash_pos != std::string::npos) line = line.substr(0, hash_pos);
        auto eq_pos = line.find('=');
        if (eq_pos == std::string::npos) continue;

        auto trim = [](std::string s) {
            size_t start = s.find_first_not_of(" \t\r\n");
            if (start == std::string::npos) return std::string();
            size_t end = s.find_last_not_of(" \t\r\n");
            return s.substr(start, end - start + 1);
        };

        std::string key = trim(line.substr(0, eq_pos));
        std::string val = trim(line.substr(eq_pos + 1));
        if (key.empty() || val.empty()) continue;

        if (key == "db_backend")          cfg.db_backend = val;
        else if (key == "mysql_host")     cfg.mysql_host = val;
        else if (key == "mysql_port")     cfg.mysql_port = std::stoi(val);
        else if (key == "mysql_user")     cfg.mysql_user = val;
        else if (key == "mysql_password") cfg.mysql_pass = val;
        else if (key == "mysql_database") cfg.mysql_db = val;
        else if (key == "mysql_socket")   cfg.mysql_socket = val;
        else if (key == "sqlite_path")    cfg.db_path = val;
        else if (key == "interval")       cfg.interval = std::stod(val);
        else if (key == "retention_days") cfg.retention_days = std::stoi(val);
        else if (key == "cpu_alert")      cfg.cpu_alert = std::stod(val);
        else if (key == "ram_alert")      cfg.ram_alert = std::stod(val);
        else if (key == "temp_alert")     cfg.temp_alert = std::stod(val);
    }
}

// process command line options and override config defaults
static int parse_args(int argc, char** argv, Config& cfg) {
    const fs::path base = exe_dir();
    cfg.db_path   = (base / "blackbox.db").string();
    cfg.watch_dir = (base / "test_watch").string();

    // check for blackbox.conf next to binary or in current directory
    if (fs::exists(base / "blackbox.conf")) {
        load_config_file(base / "blackbox.conf", cfg);
    } else if (fs::exists("blackbox.conf")) {
        load_config_file("blackbox.conf", cfg);
    }

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        std::string val;

        // support both "--opt value" and "--opt=value"
        auto eq = arg.find('=');
        if (arg.rfind("--", 0) == 0 && eq != std::string::npos) {
            val = arg.substr(eq + 1);
            arg = arg.substr(0, eq);
        }
        auto need_value = [&]() -> bool {
            if (!val.empty()) return true;
            if (i + 1 < argc) { val = argv[++i]; return true; }
            std::cerr << "Missing value for " << arg << "\n";
            return false;
        };

        try {
            if (arg == "-h" || arg == "--help")      { print_usage(argv[0]); return 1; }
            else if (arg == "--quiet")               { cfg.quiet = true; }
            else if (arg == "--backend")             { if (!need_value()) return 2; cfg.db_backend = val; }
            else if (arg == "--mysql-host")          { if (!need_value()) return 2; cfg.mysql_host = val; }
            else if (arg == "--mysql-port")          { if (!need_value()) return 2; cfg.mysql_port = std::stoi(val); }
            else if (arg == "--mysql-user")          { if (!need_value()) return 2; cfg.mysql_user = val; }
            else if (arg == "--mysql-pass")          { if (!need_value()) return 2; cfg.mysql_pass = val; }
            else if (arg == "--mysql-db")            { if (!need_value()) return 2; cfg.mysql_db = val; }
            else if (arg == "--no-fallback")         { cfg.fallback_sqlite = false; }
            else if (arg == "--db")                  { if (!need_value()) return 2; cfg.db_path = val; }
            else if (arg == "--watch")               { if (!need_value()) return 2; cfg.watch_dir = val; }
            else if (arg == "--interval")            { if (!need_value()) return 2; cfg.interval = std::stod(val); }
            else if (arg == "--retention-days")      { if (!need_value()) return 2; cfg.retention_days = std::stoi(val); }
            else if (arg == "--cpu-alert")           { if (!need_value()) return 2; cfg.cpu_alert = std::stod(val); }
            else if (arg == "--ram-alert")           { if (!need_value()) return 2; cfg.ram_alert = std::stod(val); }
            else if (arg == "--temp-alert")          { if (!need_value()) return 2; cfg.temp_alert = std::stod(val); }
            else {
                std::cerr << "Unknown option: " << arg << "\n";
                print_usage(argv[0]);
                return 2;
            }
        } catch (const std::exception&) {
            std::cerr << "Invalid number for " << arg << ": '" << val << "'\n";
            return 2;
        }
    }

    if (cfg.interval < 0.2) cfg.interval = 0.2;
    if (cfg.retention_days < 0) cfg.retention_days = 0;
    return 0;
}

// check if a PID belongs to an active blackbox process
static bool process_is_blackbox(int pid) {
    if (pid <= 0) return false;
    if (kill(pid, 0) != 0 && errno != EPERM) return false;
    std::ifstream comm("/proc/" + std::to_string(pid) + "/comm");
    std::string name;
    std::getline(comm, name);
    return name == "system_blackbox";
}

// get system hostname for session accounting
static std::string get_hostname() {
    char buf[256] = {0};
    if (gethostname(buf, sizeof(buf) - 1) != 0) return "unknown";
    return buf;
}

// thread 1: periodic hardware telemetry sampler and threshold watcher
void sensor_worker(DatabaseManager& db, HardwareSensorReader& reader, const Config& cfg) {
    {
        std::ostringstream os;
        os << "[THREAD 1] Hardware Sensor thread started. Polling every " << cfg.interval << "s...";
        say(os.str());
    }

    // monitor metrics against configured threshold limits
    ThresholdMonitor monitors[] = {
        {"cpu_usage", cfg.cpu_alert,  10.0, 3},
        {"ram_usage", cfg.ram_alert,   5.0, 3},
        {"cpu_temp",  cfg.temp_alert,  5.0, 3},
    };

    const auto interval   = std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                                std::chrono::duration<double>(cfg.interval));
    auto next_tick        = std::chrono::steady_clock::now() + interval;
    auto last_purge       = std::chrono::steady_clock::now();

    while (g_running.load()) {
        // sleep in short slices so ctrl+c or kill signals break immediately
        while (g_running.load() && std::chrono::steady_clock::now() < next_tick) {
            auto left = next_tick - std::chrono::steady_clock::now();
            std::this_thread::sleep_for(std::min<std::chrono::steady_clock::duration>(left, 100ms));
        }
        if (!g_running.load()) break;

        // advance target tick without letting timer drift accumulate
        next_tick += interval;
        if (next_tick < std::chrono::steady_clock::now()) next_tick = std::chrono::steady_clock::now() + interval;

        // grab snapshot of cpu, ram, temp, and fan metrics
        SystemVitals v = reader.read_all();

        // persist readings to database
        db.log_vitals(v.cpu_usage, v.ram_usage, v.cpu_temp, v.fan_speed);

        // evaluate threshold alert triggers and resolutions
        for (auto& m : monitors) {
            double value = (m.metric() == "cpu_usage") ? v.cpu_usage
                         : (m.metric() == "ram_usage") ? v.ram_usage
                         : v.cpu_temp;
            auto change = m.update(value);
            if (change == ThresholdMonitor::Change::None) continue;

            const char* state = (change == ThresholdMonitor::Change::Triggered) ? "TRIGGERED" : "RESOLVED";
            db.log_alert(m.metric(), value, m.threshold(), state);

            std::ostringstream os;
            os << std::fixed << std::setprecision(1)
               << "[ALERT] " << m.metric() << " " << state << ": " << value
               << " (threshold " << m.threshold() << ")";
            say(os.str());
        }

        // print metrics to stdout if not in quiet mode
        if (!cfg.quiet) {
            std::ostringstream os;
            os << std::fixed << std::setprecision(1)
               << "[SENSOR] CPU: " << std::setw(5) << v.cpu_usage << "% | "
               << "RAM: "  << std::setw(5) << v.ram_usage << "% | "
               << "Temp: " << std::setw(5) << v.cpu_temp  << "°C | "
               << "Fan: "  << std::setw(4) << v.fan_speed << " RPM";
            say(os.str());
        }

        // run retention purge once an hour to clean up old telemetry
        if (cfg.retention_days > 0 && std::chrono::steady_clock::now() - last_purge >= 1h) {
            int n = db.purge_old_vitals(cfg.retention_days);
            if (n > 0) say("[DBMS] Retention: purged " + std::to_string(n) + " old vitals rows.");
            last_purge = std::chrono::steady_clock::now();
        }
    }

    say("[THREAD 1] Hardware Sensor thread exiting.");
}

// thread 2: linux kernel inotify event listener for file operations
void inotify_worker(DatabaseManager& db, const std::string& watch_dir) {
    say("[THREAD 2] Inotify thread started. Watching directory: '" + watch_dir + "'");

    // create inotify file descriptor with non-blocking mode
    int inotify_fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (inotify_fd < 0) {
        say(std::string("[INOTIFY ERROR] Failed to initialize inotify: ") + std::strerror(errno), true);
        return;
    }

    const uint32_t mask = IN_DELETE | IN_MOVED_FROM | IN_MOVED_TO | IN_DELETE_SELF | IN_MOVE_SELF;
    int watch_desc = -1;
    auto last_rewatch_try = std::chrono::steady_clock::time_point{};

    // helper to recreate directory and arm inotify watch mask
    auto arm_watch = [&]() {
        std::error_code ec;
        fs::create_directories(watch_dir, ec);
        watch_desc = inotify_add_watch(inotify_fd, watch_dir.c_str(), mask);
        if (watch_desc < 0) {
            say("[INOTIFY ERROR] Cannot watch '" + watch_dir + "': " + std::strerror(errno), true);
        }
        return watch_desc >= 0;
    };
    if (!arm_watch()) {
        close(inotify_fd);
        return;
    }

    // temporary buffer for pending move events to distinguish rename from move-out
    struct PendingMove {
        std::string name;
        bool is_dir;
        std::chrono::steady_clock::time_point when;
    };
    std::unordered_map<uint32_t, PendingMove> pending_moves;

    // helper to persist audit record and log alert to console
    auto record = [&](const std::string& name, const char* type, bool is_dir, const std::string& new_name = {}) {
        db.log_file_event(name, type, is_dir, new_name);
        std::string what = std::string(is_dir ? "directory" : "file") + " '" + name + "'";
        if (!new_name.empty()) what += " -> '" + new_name + "'";
        say(std::string("[ALERT][INOTIFY] ") + type + ": " + what);
    };

    constexpr size_t BUF_LEN = 4096;
    // buffer sized for reading batched inotify events
    alignas(struct inotify_event) char buffer[BUF_LEN];

    // poll inotify file descriptor with a short timeout
    struct pollfd pfd;
    pfd.fd = inotify_fd;
    pfd.events = POLLIN;

    while (g_running.load()) {
        // if watched directory vanished, try to recreate and re-arm watch
        if (watch_desc < 0 && std::chrono::steady_clock::now() - last_rewatch_try >= 1s) {
            last_rewatch_try = std::chrono::steady_clock::now();
            if (arm_watch()) say("[INOTIFY] Watch re-armed on '" + watch_dir + "'");
        }

        // wait up to 200ms for event or timeout to check g_running
        int ret = poll(&pfd, 1, 200);
        if (ret < 0) {
            if (errno == EINTR) continue;  // signal interrupted us, retry
            say(std::string("[INOTIFY ERROR] poll failed: ") + std::strerror(errno), true);
            break;
        }

        if (ret > 0 && (pfd.revents & POLLIN)) {
            // process all events queued in the inotify buffer
            ssize_t num_read;
            while ((num_read = read(inotify_fd, buffer, BUF_LEN)) > 0) {
                ssize_t offset = 0;
                while (offset < num_read) {
                    auto* event = reinterpret_cast<struct inotify_event*>(&buffer[offset]);
                    std::string name = (event->len > 0) ? std::string(event->name) : std::string();
                    bool is_dir = (event->mask & IN_ISDIR) != 0;

                    if (event->mask & IN_Q_OVERFLOW) {
                        // kernel queue overflowed, some events were dropped
                        record("*", "QUEUE_OVERFLOW", false);
                    } else if (event->mask & IN_DELETE) {
                        // file or directory deleted inside watched folder
                        record(name, "DELETED", is_dir);
                    } else if (event->mask & IN_MOVED_FROM) {
                        // file moved away, record pending move to see if matching IN_MOVED_TO arrives
                        pending_moves[event->cookie] = {name, is_dir, std::chrono::steady_clock::now()};
                    } else if (event->mask & IN_MOVED_TO) {
                        // file arrived, if cookie matches pending move then it was a rename
                        auto it = pending_moves.find(event->cookie);
                        if (it != pending_moves.end()) {
                            record(it->second.name, "RENAMED", is_dir, name);
                            pending_moves.erase(it);
                        }
                    } else if (event->mask & (IN_DELETE_SELF | IN_MOVE_SELF)) {
                        // watched folder itself was removed or moved away
                        if (event->wd == watch_desc) {
                            record(watch_dir, "WATCH_DIR_REMOVED", true);
                            inotify_rm_watch(inotify_fd, watch_desc);
                            watch_desc = -1;
                        }
                    } else if (event->mask & IN_IGNORED) {
                        if (event->wd == watch_desc) watch_desc = -1;
                    }

                    offset += static_cast<ssize_t>(sizeof(struct inotify_event) + event->len);
                }
            }
        }

        // pending moves that received no matching destination within timeout are moves out
        auto now = std::chrono::steady_clock::now();
        for (auto it = pending_moves.begin(); it != pending_moves.end();) {
            if (now - it->second.when >= 300ms) {
                record(it->second.name, "MOVED_OUT", it->second.is_dir);
                it = pending_moves.erase(it);
            } else {
                ++it;
            }
        }
    }

    // flush any remaining pending moves on shutdown
    for (auto& [cookie, p] : pending_moves) record(p.name, "MOVED_OUT", p.is_dir);

    // detach watch and close file descriptor
    if (watch_desc >= 0) inotify_rm_watch(inotify_fd, watch_desc);
    close(inotify_fd);
    say("[THREAD 2] Inotify thread exiting.");
}

// daemon entrypoint: parse args, check prior crashes, spawn threads, and wait
int main(int argc, char** argv) {
    Config cfg;
    if (int rc = parse_args(argc, argv, cfg); rc != 0) {
        return rc == 1 ? 0 : rc;
    }

    // print startup banner
    std::cout << "╔══════════════════════════════════════════════════════════════╗\n";
    std::cout << "║    System Blackbox — Native OS Telemetry & DBMS Engine       ║\n";
    std::cout << "║       Operating Systems & Database Management Systems        ║\n";
    std::cout << "╚══════════════════════════════════════════════════════════════╝\n\n";

    // install signal action for graceful shutdown without SA_RESTART so poll wakes up
    struct sigaction sa{};
    sa.sa_handler = signal_handler;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT,  &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);

    // make sure watch folder exists before starting inotify thread
    std::error_code ec;
    if (!fs::exists(cfg.watch_dir, ec)) {
        fs::create_directories(cfg.watch_dir, ec);
        std::cout << "[INIT] Created watch directory: " << cfg.watch_dir << "\n";
    }

    // initialize database backend based on config
    DatabaseConfig db_cfg;
    db_cfg.backend = (cfg.db_backend == "sqlite") ? BackendType::SQLite : BackendType::MySQL;
    db_cfg.mysql_host = cfg.mysql_host;
    db_cfg.mysql_port = cfg.mysql_port;
    db_cfg.mysql_user = cfg.mysql_user;
    db_cfg.mysql_pass = cfg.mysql_pass;
    db_cfg.mysql_db = cfg.mysql_db;
    db_cfg.mysql_socket = cfg.mysql_socket;
    db_cfg.sqlite_path = cfg.db_path;
    db_cfg.fallback_to_sqlite = cfg.fallback_sqlite;

    DatabaseManager db;
    if (!db.init(db_cfg)) {
        std::cerr << "[FATAL] Failed to initialize database engine. Exiting.\n";
        return 1;
    }
    std::cout << "[INIT] Database persistence engine active: " << db.backend_name() << "\n";

    // crash forensics: find previous sessions that never cleanly exited
    const std::string host = get_hostname();
    for (const auto& s : db.find_open_sessions()) {
        if (s.hostname == host && s.pid != getpid() && process_is_blackbox(s.pid)) {
            std::cerr << "[FATAL] Another system_blackbox (pid " << s.pid
                      << ") is already recording to this database. Exiting.\n";
            return 1;
        }
        db.mark_session_crashed(s);
        std::cout << "\n[WARNING] Previous session #" << s.id << " (pid " << s.pid
                  << ") ended UNEXPECTEDLY.\n"
                  << "          Started: " << s.start_time << "\n"
                  << "          Last recorded data: " << s.last_seen
                  << "   <- crash / kill -9 / power loss happened after this\n\n";
    }
    if (!db.begin_session(getpid(), host)) {
        std::cerr << "[FATAL] Could not create a session record. Exiting.\n";
        return 1;
    }
    std::cout << "[INIT] Session #" << db.session_id() << " started (pid " << getpid() << ").\n";

    // purge expired vitals once at startup
    if (cfg.retention_days > 0) {
        int n = db.purge_old_vitals(cfg.retention_days);
        if (n > 0) std::cout << "[DBMS] Retention: purged " << n << " vitals rows older than "
                             << cfg.retention_days << " days.\n";
    }

    // create sensor reader and log detected temp source
    HardwareSensorReader reader;
    std::cout << "[INIT] CPU temperature source: " << reader.temp_source() << "\n";
    std::cout << "[INIT] Alerts: CPU >= " << cfg.cpu_alert << "%, RAM >= " << cfg.ram_alert
              << "%, Temp >= " << cfg.temp_alert << "°C (0 = disabled)\n";
    std::cout << "[INIT] Spawning concurrent worker threads...\n";

    // spawn worker threads and wait for termination signal
    std::thread sensor_th(sensor_worker, std::ref(db), std::ref(reader), std::cref(cfg));
    std::thread inotify_th(inotify_worker, std::ref(db), cfg.watch_dir);

    say("\n[INFO] Daemon running. Press Ctrl+C to terminate cleanly.");
    say("[INFO] Telemetry is being written to " + db.backend_name() + " in real-time.");
    say("[INFO] Run 'make ui' in another terminal to open the web dashboard.\n");

    // wait until threads complete their loops after signal
    if (sensor_th.joinable())  sensor_th.join();
    if (inotify_th.joinable()) inotify_th.join();

    std::cout << "[SHUTDOWN] Received signal " << g_signal.load() << ". Closing session...\n";

    // cleanly close session record and database handles before exit
    db.end_session();
    db.close();

    std::cout << "[EXIT] System Blackbox shutdown complete.\n";
    return 0;
}
