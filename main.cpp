// system blackbox main daemon entrypoint and worker thread orchestration

#include "db.hpp"
#include "sensors.hpp"
#include "scheduler.hpp"
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

#include <sys/eventfd.h>
#include <sys/inotify.h>
#include <poll.h>
#include <unistd.h>

namespace fs = std::filesystem;
using namespace std::chrono_literals;

// flag for threads to exit loops when a termination signal arrives
static std::atomic<bool> g_running{true};
// stores the numeric signal that told us to shut down
static std::atomic<int>  g_signal{0};
// eventfd descriptor signaled from signal_handler to wake blocking poll() calls immediately
static int               g_stop_fd = -1;

// mutex so output from sensor and inotify threads don't get interleaved
static std::mutex g_out_mutex;
// thread-safe console output printer
static void say(const std::string& line, bool to_stderr = false) {
    std::lock_guard<std::mutex> lock(g_out_mutex);
    (to_stderr ? std::cerr : std::cout) << line << std::endl;
}

// async-signal-safe handler that sets atomics and pokes eventfd on SIGINT or SIGTERM
static void signal_handler(int signum) {
    int saved_errno = errno;
    g_signal.store(signum);
    g_running.store(false);
    if (g_stop_fd >= 0) {
        const uint64_t one = 1;
        [[maybe_unused]] ssize_t r = write(g_stop_fd, &one, sizeof(one));
    }
    errno = saved_errno;
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

    SchedPolicy sched_policy = SchedPolicy::SRTF; // OS CPU scheduling policy (SRTF/FCFS/SJF/RR)
    bool        sched_demo   = false;             // run standalone OS scheduler comparison & exit
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
        "  --sched [srtf|fcfs|sjf|rr] OS CPU scheduler policy (default: srtf)\n"
        "  --sched-demo             Run OS CPU Scheduler benchmark (SRTF/FCFS/SJF/RR) and exit\n"
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
static void load_config_file(const fs::path& conf_path, const fs::path& base_dir, Config& cfg) {
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
        else if (key == "sqlite_path") {
            fs::path p(val);
            cfg.db_path = p.is_relative() ? (base_dir / p).string() : p.string();
        }
        else if (key == "interval")       cfg.interval = std::stod(val);
        else if (key == "retention_days") cfg.retention_days = std::stoi(val);
        else if (key == "cpu_alert")      cfg.cpu_alert = std::stod(val);
        else if (key == "ram_alert")      cfg.ram_alert = std::stod(val);
        else if (key == "temp_alert")     cfg.temp_alert = std::stod(val);
        else if (key == "sched_policy")   cfg.sched_policy = parse_sched_policy(val);
    }
}

// process command line options and override config defaults
static int parse_args(int argc, char** argv, Config& cfg) {
    const fs::path base = exe_dir();
    cfg.db_path   = (base / "blackbox.db").string();
    cfg.watch_dir = (base / "test_watch").string();

    // check for blackbox.conf next to binary or in current directory
    if (fs::exists(base / "blackbox.conf")) {
        load_config_file(base / "blackbox.conf", base, cfg);
    } else if (fs::exists("blackbox.conf")) {
        load_config_file("blackbox.conf", fs::current_path(), cfg);
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
            else if (arg == "--sched-demo")          { cfg.sched_demo = true; }
            else if (arg == "--sched")               { if (!need_value()) return 2; cfg.sched_policy = parse_sched_policy(val); }
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

// standalone OS CPU scheduling benchmark comparing SRTF, SJF, FCFS, and Round Robin
static void run_scheduler_demo(HardwareSensorReader& reader) {
    // run two live hardware probe cycles so exponential burst predictions (tau_n)
    // reflect real microsecond syscall timings on this Linux machine
    reader.read_all();
    reader.read_all();

    auto base_tasks = reader.build_probe_tasks(nullptr);

    std::cout << "┌───────────────────────────────────────────────────────────────────────────────────┐\n";
    std::cout << "│      OS CPU SCHEDULER BENCHMARK — KERNEL TELEMETRY PROBE DISPATCH COMPARISON      │\n";
    std::cout << "│   Algorithms: SRTF (Preemptive) | SJF | FCFS | Round Robin (Time Quantum = 4us)   │\n";
    std::cout << "└───────────────────────────────────────────────────────────────────────────────────┘\n\n";

    const SchedPolicy policies[] = {
        SchedPolicy::SRTF,
        SchedPolicy::SJF,
        SchedPolicy::FCFS,
        SchedPolicy::RR
    };

    for (SchedPolicy pol : policies) {
        ScheduleResult res = OsTaskScheduler::schedule_with(pol, base_tasks, 4);
        std::cout << "▸ " << sched_policy_name(pol) << "\n";
        std::cout << OsTaskScheduler::format_report(res) << "\n";
    }
}

// thread 1: periodic hardware telemetry sampler and threshold watcher
void sensor_worker(DatabaseManager& db, HardwareSensorReader& reader, const Config& cfg) {
    {
        std::ostringstream os;
        os << "[THREAD 1] Hardware Sensor thread started. Polling every " << cfg.interval
           << "s (OS Scheduler: " << sched_policy_short(cfg.sched_policy) << ")...";
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
        // wait until next_tick in a single kernel sleep, waking immediately on shutdown eventfd
        while (g_running.load()) {
            auto now = std::chrono::steady_clock::now();
            if (now >= next_tick) break;
            if (g_stop_fd >= 0) {
                auto wait_ms = std::chrono::duration_cast<std::chrono::milliseconds>(next_tick - now).count();
                struct pollfd stop_pfd{g_stop_fd, POLLIN, 0};
                int rc = poll(&stop_pfd, 1, static_cast<int>(std::max<long long>(1, wait_ms)));
                if (rc > 0) break;
            } else {
                std::this_thread::sleep_for(std::min<std::chrono::steady_clock::duration>(next_tick - now, 100ms));
            }
        }
        if (!g_running.load()) break;

        // advance target tick without letting timer drift accumulate
        next_tick += interval;
        if (next_tick < std::chrono::steady_clock::now()) next_tick = std::chrono::steady_clock::now() + interval;

        // grab snapshot of cpu, ram, temp, and fan metrics dispatched via OsTaskScheduler
        SystemVitals v = reader.read_all();
        const ScheduleResult& sched = reader.last_schedule();

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

        // print metrics to stdout if not in quiet mode (with live OS scheduler WT/TAT metrics)
        if (!cfg.quiet) {
            std::ostringstream os;
            os << std::fixed << std::setprecision(1)
               << "[SENSOR] CPU: " << std::setw(5) << v.cpu_usage << "% | "
               << "RAM: "  << std::setw(5) << v.ram_usage << "% | "
               << "Temp: " << std::setw(5) << v.cpu_temp  << "°C | "
               << "Fan: "  << std::setw(4) << v.fan_speed << " RPM | "
               << "Sched[" << sched_policy_short(sched.policy) << "] "
               << "AvgWT:" << sched.avg_waiting_time << "us "
               << "TAT:"   << sched.avg_turnaround_time << "us";
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

// thread 2: linux kernel inotify event listener for file and folder operations
void inotify_worker(DatabaseManager& db, const std::string& watch_dir, SchedPolicy sched_policy) {
    say("[THREAD 2] Inotify thread started. Watching directory tree: '" + watch_dir + "' (+ user folders & Trash)");

    // create inotify file descriptor with non-blocking mode
    int inotify_fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (inotify_fd < 0) {
        say(std::string("[INOTIFY ERROR] Failed to initialize inotify: ") + std::strerror(errno), true);
        return;
    }

    // include IN_CREATE so newly created subdirectories are automatically watched recursively
    const uint32_t mask = IN_CREATE | IN_DELETE | IN_MOVED_FROM | IN_MOVED_TO | IN_DELETE_SELF | IN_MOVE_SELF;

    std::error_code ec;
    const fs::path primary_root = fs::weakly_canonical(watch_dir, ec);
    const char* home_env = std::getenv("HOME");
    const fs::path home_dir = home_env ? fs::path(home_env) : fs::path();
    const fs::path trash_files_dir = home_dir.empty() ? fs::path() : (home_dir / ".local/share/Trash/files");

    // bidirectional watch descriptor <-> directory path tables
    std::unordered_map<int, fs::path> wd_to_path;
    std::unordered_map<std::string, int> path_to_wd;
    int primary_wd = -1;
    int trash_wd   = -1;
    auto last_rewatch_try = std::chrono::steady_clock::time_point{};

    // skip noisy build/system directories when recursively arming watches outside test_watch
    auto should_skip_dir = [](const std::string& dir_name) -> bool {
        if (dir_name.empty() || dir_name[0] == '.') return true;
        return dir_name == "node_modules" || dir_name == "__pycache__" ||
               dir_name == "target" || dir_name == "build" || dir_name == "dist" ||
               dir_name == "venv" || dir_name == "Antigravity IDE";
    };

    // filter out compiler/database temporary files outside primary watch_dir
    auto is_ignored_artifact = [](const std::string& fname, bool in_primary_watch) -> bool {
        if (fname.empty() || fname[0] == '.') return true;
        if (fname.ends_with('~') || fname.ends_with(".swp") || fname.ends_with(".swx") || fname.ends_with(".tmp"))
            return true;
        if (!in_primary_watch) {
            if (fname.ends_with(".o") || fname.ends_with(".d") || fname.ends_with(".pyc") ||
                fname.ends_with(".db") || fname.ends_with(".db-wal") ||
                fname.ends_with(".db-shm") || fname.ends_with(".db-journal") ||
                fname == "system_blackbox") {
                return true;
            }
        }
        return false;
    };

    // format a clean display path relative to watch_dir or HOME
    auto format_display_path = [&](const fs::path& dir_path, const std::string& name) -> std::string {
        fs::path full = dir_path / name;
        std::error_code rel_ec;
        if (!primary_root.empty()) {
            auto rel = fs::relative(full, primary_root, rel_ec);
            if (!rel_ec && !rel.empty() && rel.native().rfind("..", 0) != 0) {
                return rel.string();
            }
        }
        if (!home_dir.empty()) {
            auto rel = fs::relative(full, home_dir, rel_ec);
            if (!rel_ec && !rel.empty() && rel.native().rfind("..", 0) != 0) {
                return rel.string();
            }
        }
        return full.string();
    };

    // add a single directory watch descriptor
    auto add_single_watch = [&](const fs::path& dir_path) -> int {
        std::error_code c_ec;
        fs::path norm = fs::weakly_canonical(dir_path, c_ec);
        if (c_ec || norm.empty()) norm = dir_path;
        std::string key = norm.string();

        auto existing = path_to_wd.find(key);
        if (existing != path_to_wd.end()) return existing->second;

        int wd = inotify_add_watch(inotify_fd, key.c_str(), mask);
        if (wd >= 0) {
            wd_to_path[wd] = norm;
            path_to_wd[key] = wd;
        }
        return wd;
    };

    // recursively arm watches on a directory tree up to max_depth
    auto add_watch_tree = [&](const fs::path& root, int max_depth, bool is_primary) -> bool {
        std::error_code d_ec;
        if (is_primary) {
            fs::create_directories(root, d_ec);
        }
        if (!fs::exists(root, d_ec) || !fs::is_directory(root, d_ec)) return false;

        int root_wd = add_single_watch(root);
        if (is_primary) {
            primary_wd = root_wd;
            if (root_wd < 0) {
                say("[INOTIFY ERROR] Cannot watch '" + root.string() + "': " + std::strerror(errno), true);
                return false;
            }
        }
        if (root_wd < 0 || max_depth <= 0) return root_wd >= 0;

        fs::recursive_directory_iterator it(
            root, fs::directory_options::skip_permission_denied, d_ec
        ), end;
        while (!d_ec && it != end) {
            if (it.depth() >= max_depth) {
                it.disable_recursion_pending();
            } else if (it->is_directory(d_ec)) {
                std::string dname = it->path().filename().string();
                if (!is_primary && should_skip_dir(dname)) {
                    it.disable_recursion_pending();
                } else {
                    add_single_watch(it->path());
                }
            }
            it.increment(d_ec);
        }
        return true;
    };

    // arm primary watch_dir (recursive)
    if (!add_watch_tree(watch_dir, 16, true)) {
        close(inotify_fd);
        return;
    }

    // also arm watches on the project directory, user folders (Desktop/Downloads/Documents/etc.), and Trash
    add_watch_tree(exe_dir(), 3, false);
    if (!home_dir.empty()) {
        add_single_watch(home_dir);
        add_watch_tree(home_dir / "Desktop",   4, false);
        add_watch_tree(home_dir / "Downloads", 4, false);
        add_watch_tree(home_dir / "Documents", 4, false);
        add_watch_tree(home_dir / "Pictures",  3, false);
        add_watch_tree(home_dir / "Videos",    3, false);
        add_watch_tree(home_dir / "Music",     3, false);
        if (!trash_files_dir.empty()) {
            fs::create_directories(trash_files_dir, ec);
            trash_wd = add_single_watch(trash_files_dir);
        }
    }

    // temporary buffer for pending move events to distinguish rename, trash deletion, or move-out
    struct PendingMove {
        int         wd;
        std::string display_name;
        std::string raw_name;
        bool        is_dir;
        bool        in_primary;
        std::chrono::steady_clock::time_point when;
    };
    std::unordered_map<uint32_t, PendingMove> pending_moves;
    std::unordered_map<std::string, std::chrono::steady_clock::time_point> recent_deletions;
    std::vector<FileEvent> batch;
    batch.reserve(32);

    // stage audit record for batched DB persistence (with 400ms deduplication for Trash moves)
    auto stage_record = [&](const std::string& name, const char* type, bool is_dir, const std::string& new_name = {}) {
        auto now_ts = std::chrono::steady_clock::now();
        std::string dedup_key = std::string(type) + ":" + name;
        auto it = recent_deletions.find(dedup_key);
        if (it != recent_deletions.end() && (now_ts - it->second) < 400ms) {
            return;
        }
        recent_deletions[dedup_key] = now_ts;
        batch.push_back(FileEvent{name, type, is_dir, new_name});
    };

    // flush staged file events in a single DB batch and single console write
    auto flush_batch = [&]() {
        if (batch.empty()) return;

        // when a burst of multiple file events arrives simultaneously in the
        // kernel buffer, schedule their dispatch order via OsTaskScheduler (SRTF/FCFS/SJF/RR)
        if (batch.size() > 1) {
            std::vector<SchedTask> event_tasks;
            event_tasks.reserve(batch.size());
            for (size_t i = 0; i < batch.size(); ++i) {
                const auto& ev = batch[i];
                // assign estimated burst time based on audit event complexity
                int est_burst = 3;
                if (ev.event_type == "RENAMED") {
                    est_burst = 6; // cookie-paired rename carries two paths
                } else if (ev.event_type == "DELETED") {
                    est_burst = 2; // short burst -> prioritized under SRTF / SJF
                }
                event_tasks.push_back(SchedTask{
                    static_cast<int>(i + 1),
                    ev.event_type + ":" + ev.file_name,
                    static_cast<int>(i), // staggered arrival order within buffer
                    est_burst, 0, -1, 0, 0, 0, 0, {}
                });
            }

            ScheduleResult sched_res = OsTaskScheduler::schedule_with(sched_policy, event_tasks, 2);
            std::vector<FileEvent> ordered_batch;
            ordered_batch.reserve(batch.size());
            for (size_t idx : sched_res.dispatch_order) {
                if (idx < batch.size()) ordered_batch.push_back(batch[idx]);
            }
            batch.swap(ordered_batch);
        }

        db.log_file_events_batch(batch);
        std::string out;
        out.reserve(batch.size() * 64);
        for (size_t i = 0; i < batch.size(); ++i) {
            if (i > 0) out += '\n';
            const auto& ev = batch[i];
            out += "[ALERT][INOTIFY] ";
            out += ev.event_type;
            out += ": ";
            out += (ev.is_dir ? "directory '" : "file '");
            out += ev.file_name;
            out += '\'';
            if (!ev.new_name.empty()) {
                out += " -> '";
                out += ev.new_name;
                out += '\'';
            }
        }
        say(out);
        batch.clear();
    };

    constexpr size_t BUF_LEN = 16384;
    // buffer sized for reading batched inotify events
    alignas(struct inotify_event) char buffer[BUF_LEN];

    // poll inotify file descriptor and shutdown eventfd (sleeps indefinitely with 0% CPU when idle)
    struct pollfd pfds[2]{};
    pfds[0].fd = inotify_fd;
    pfds[0].events = POLLIN;
    pfds[1].fd = g_stop_fd;
    pfds[1].events = POLLIN;
    const nfds_t nfds = (g_stop_fd >= 0) ? 2 : 1;

    while (g_running.load()) {
        auto now = std::chrono::steady_clock::now();

        // if primary watched directory vanished, try to recreate and re-arm watch
        if (primary_wd < 0 && now - last_rewatch_try >= 1s) {
            last_rewatch_try = now;
            if (add_watch_tree(watch_dir, 16, true)) {
                say("[INOTIFY] Watch re-armed on '" + watch_dir + "'");
            }
        }

        // calculate exact poll timeout: infinite (-1) when idle and eventfd is active, avoiding idle wakeups
        int timeout_ms = (g_stop_fd >= 0) ? -1 : 200;
        if (!pending_moves.empty()) {
            auto earliest_deadline = now + 300ms;
            for (const auto& [cookie, pm] : pending_moves) {
                auto dl = pm.when + 300ms;
                if (dl < earliest_deadline) earliest_deadline = dl;
            }
            auto rem = std::chrono::duration_cast<std::chrono::milliseconds>(earliest_deadline - now).count();
            timeout_ms = static_cast<int>(std::max<long long>(0, rem));
        } else if (primary_wd < 0) {
            auto rem = std::chrono::duration_cast<std::chrono::milliseconds>((last_rewatch_try + 1s) - now).count();
            timeout_ms = static_cast<int>(std::clamp<long long>(rem, 1, 1000));
        }

        int ret = poll(pfds, nfds, timeout_ms);
        if (ret < 0) {
            if (errno == EINTR) continue;  // signal interrupted us, retry
            say(std::string("[INOTIFY ERROR] poll failed: ") + std::strerror(errno), true);
            break;
        }
        if (!g_running.load()) break;

        if (ret > 0 && (pfds[0].revents & POLLIN)) {
            // process all events queued in the inotify buffer
            ssize_t num_read;
            while ((num_read = read(inotify_fd, buffer, BUF_LEN)) > 0) {
                const auto batch_time = std::chrono::steady_clock::now();
                ssize_t offset = 0;
                while (offset < num_read) {
                    auto* event = reinterpret_cast<struct inotify_event*>(&buffer[offset]);
                    std::string name = (event->len > 0) ? std::string(event->name) : std::string();
                    bool is_dir = (event->mask & IN_ISDIR) != 0;

                    auto wd_it = wd_to_path.find(event->wd);
                    fs::path dir_path = (wd_it != wd_to_path.end()) ? wd_it->second : fs::path(watch_dir);
                    std::error_code rel_ec;
                    bool in_primary = !primary_root.empty() &&
                                      (dir_path == primary_root ||
                                       (!fs::relative(dir_path, primary_root, rel_ec).empty() &&
                                        fs::relative(dir_path, primary_root, rel_ec).native().rfind("..", 0) != 0));

                    if (event->mask & IN_Q_OVERFLOW) {
                        stage_record("*", "QUEUE_OVERFLOW", false);
                    } else if (event->wd == trash_wd) {
                        // file or folder arrived in ~/.local/share/Trash/files (GUI / File Manager delete)
                        if ((event->mask & (IN_MOVED_TO | IN_CREATE)) && !name.empty() && name[0] != '.') {
                            auto pm_it = (event->cookie != 0) ? pending_moves.find(event->cookie) : pending_moves.end();
                            if (pm_it != pending_moves.end()) {
                                stage_record(pm_it->second.display_name, "DELETED", pm_it->second.is_dir);
                                pending_moves.erase(pm_it);
                            } else {
                                stage_record(name, "DELETED", is_dir);
                            }
                        }
                    } else if ((event->mask & IN_CREATE) && is_dir) {
                        // new subdirectory created inside a watched folder -> recursively watch it
                        if (in_primary || !should_skip_dir(name)) {
                            add_watch_tree(dir_path / name, in_primary ? 16 : 2, false);
                        }
                    } else if (event->mask & IN_DELETE) {
                        // file or directory deleted via unlink() / rmdir()
                        if (!is_ignored_artifact(name, in_primary)) {
                            stage_record(format_display_path(dir_path, name), "DELETED", is_dir);
                        }
                    } else if (event->mask & IN_MOVED_FROM) {
                        // file or directory moved away: track cookie to pair with rename or Trash move
                        if (!is_ignored_artifact(name, in_primary)) {
                            std::string disp = format_display_path(dir_path, name);
                            if (event->cookie != 0) {
                                pending_moves[event->cookie] = {
                                    event->wd, disp, name, is_dir, in_primary, batch_time
                                };
                            } else {
                                stage_record(disp, "MOVED_OUT", is_dir);
                            }
                        }
                    } else if (event->mask & IN_MOVED_TO) {
                        if (is_dir && (in_primary || !should_skip_dir(name))) {
                            add_watch_tree(dir_path / name, in_primary ? 16 : 2, false);
                        }
                        auto it = (event->cookie != 0) ? pending_moves.find(event->cookie) : pending_moves.end();
                        if (it != pending_moves.end()) {
                            if (!is_ignored_artifact(name, in_primary)) {
                                std::string new_disp = (it->second.wd == event->wd && in_primary)
                                                     ? name
                                                     : format_display_path(dir_path, name);
                                stage_record(it->second.display_name, "RENAMED", is_dir, new_disp);
                            }
                            pending_moves.erase(it);
                        }
                    } else if (event->mask & (IN_DELETE_SELF | IN_MOVE_SELF)) {
                        if (event->wd == primary_wd) {
                            stage_record(watch_dir, "WATCH_DIR_REMOVED", true);
                            inotify_rm_watch(inotify_fd, primary_wd);
                            path_to_wd.erase(dir_path.string());
                            wd_to_path.erase(primary_wd);
                            primary_wd = -1;
                        } else if (wd_it != wd_to_path.end()) {
                            path_to_wd.erase(wd_it->second.string());
                            wd_to_path.erase(wd_it);
                        }
                    } else if (event->mask & IN_IGNORED) {
                        if (event->wd == primary_wd) primary_wd = -1;
                        if (wd_it != wd_to_path.end()) {
                            path_to_wd.erase(wd_it->second.string());
                            wd_to_path.erase(wd_it);
                        }
                    }

                    offset += static_cast<ssize_t>(sizeof(struct inotify_event) + event->len);
                }
            }
        }

        // pending moves that received no matching destination within timeout are moves out / trashed
        now = std::chrono::steady_clock::now();
        for (auto it = pending_moves.begin(); it != pending_moves.end();) {
            if (now - it->second.when >= 300ms) {
                stage_record(it->second.display_name, "MOVED_OUT", it->second.is_dir);
                it = pending_moves.erase(it);
            } else {
                ++it;
            }
        }

        flush_batch();
    }

    // flush any remaining pending moves on shutdown
    for (auto& [cookie, p] : pending_moves) stage_record(p.display_name, "MOVED_OUT", p.is_dir);
    flush_batch();

    // detach all watches and close file descriptor
    for (const auto& [wd, p] : wd_to_path) {
        inotify_rm_watch(inotify_fd, wd);
    }
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

    // if --sched-demo was passed, run the standalone OS CPU scheduler comparison and exit
    if (cfg.sched_demo) {
        HardwareSensorReader reader;
        reader.set_sched_policy(cfg.sched_policy);
        run_scheduler_demo(reader);
        return 0;
    }

    // create shutdown eventfd before installing signal handlers so threads wake instantaneously
    g_stop_fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);

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

    // create sensor reader, set OS CPU scheduler policy, and log initial schedule & Gantt chart
    HardwareSensorReader reader;
    reader.set_sched_policy(cfg.sched_policy);
    reader.read_all();
    std::cout << "[INIT] CPU temperature source: " << reader.temp_source() << "\n";
    std::cout << "[INIT] Alerts: CPU >= " << cfg.cpu_alert << "%, RAM >= " << cfg.ram_alert
              << "%, Temp >= " << cfg.temp_alert << "°C (0 = disabled)\n";
    std::cout << "[INIT] OS CPU Scheduler active: " << sched_policy_name(cfg.sched_policy) << "\n";
    std::cout << OsTaskScheduler::format_report(reader.last_schedule());
    std::cout << "[INIT] Spawning concurrent worker threads...\n";

    // spawn worker threads and wait for termination signal
    std::thread sensor_th(sensor_worker, std::ref(db), std::ref(reader), std::cref(cfg));
    std::thread inotify_th(inotify_worker, std::ref(db), cfg.watch_dir, cfg.sched_policy);

    say("\n[INFO] Daemon running. Press Ctrl+C to terminate cleanly.");
    say("[INFO] Telemetry is being written to " + db.backend_name() + " in real-time.");
    say("[INFO] Run 'make ui' in another terminal to open the web dashboard.\n");

    // wait until threads complete their loops after signal
    if (sensor_th.joinable())  sensor_th.join();
    if (inotify_th.joinable()) inotify_th.join();

    std::cout << "[SHUTDOWN] Received signal " << g_signal.load() << ". Closing session...\n";

    // cleanly close session record, eventfd, and database handles before exit
    db.end_session();
    db.close();
    if (g_stop_fd >= 0) {
        close(g_stop_fd);
        g_stop_fd = -1;
    }

    std::cout << "[EXIT] System Blackbox shutdown complete.\n";
    return 0;
}
