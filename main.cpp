// ============================================================================
// main.cpp — System Blackbox Engine (OS Telemetry & Kernel File Monitor)
// ============================================================================
// University PBL: Operating Systems + Database Management Systems
//
// ARCHITECTURE:
//   • Thread 1 (Sensor Thread):
//       Reads /proc/stat, /proc/meminfo, and /sys/class/hwmon every 1 second.
//       Writes vitals into SQLite (vitals_log table).
//
//   • Thread 2 (Inotify Thread):
//       Monitors directory 'test_watch' for file deletion events via Linux inotify.
//       Writes forensic logs into SQLite (deleted_files table).
//
//   • Database Layer (Thread-Safe WAL Mode):
//       Guards SQLite connection using std::mutex; readers (Python server)
//       never block writers (C++ daemon) thanks to Write-Ahead Logging (WAL).
//
//   • Signal Handling & RAII:
//       Graceful SIGINT/SIGTERM shutdown, clean thread joins, and auto-cleanup.
// ============================================================================

#include "db.hpp"
#include "sensors.hpp"

#include <iostream>
#include <iomanip>
#include <thread>
#include <atomic>
#include <chrono>
#include <csignal>
#include <filesystem>
#include <vector>
#include <cstring>
#include <cerrno>

#include <sys/inotify.h>
#include <poll.h>
#include <unistd.h>

namespace fs = std::filesystem;

// Global flag to signal all worker threads to stop on SIGINT/SIGTERM
static std::atomic<bool> g_running{true};

static void signal_handler(int signum) {
    if (signum == SIGINT || signum == SIGTERM) {
        std::cout << "\n[SHUTDOWN] Received signal " << signum << ". Stopping Blackbox daemon...\n";
        g_running.store(false);
    }
}

// ────────────────────────────────────────────────────────────────────────────
// Thread 1: Hardware Vitals Monitoring (/proc & /sys)
// ────────────────────────────────────────────────────────────────────────────
void sensor_worker(DatabaseManager& db) {
    HardwareSensorReader reader;
    std::cout << "[THREAD 1] Hardware Sensor thread started. Polling every 1s...\n";

    while (g_running.load()) {
        SystemVitals v = reader.read_all();

        // Write directly to SQLite via thread-safe DatabaseManager
        db.log_vitals(v.cpu_usage, v.ram_usage, v.cpu_temp, v.fan_speed);

        std::cout << "[SENSOR] CPU: " << std::setw(5) << std::fixed << std::setprecision(1) << v.cpu_usage << "% | "
                  << "RAM: " << std::setw(5) << v.ram_usage << "% | "
                  << "Temp: " << std::setw(4) << v.cpu_temp << "°C | "
                  << "Fan: " << std::setw(4) << v.fan_speed << " RPM "
                  << "-> Logged to blackbox.db\n";

        // Sleep in 100ms slices for fast reaction to shutdown signal
        for (int i = 0; i < 10 && g_running.load(); ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }

    std::cout << "[THREAD 1] Hardware Sensor thread exiting.\n";
}

// ────────────────────────────────────────────────────────────────────────────
// Thread 2: Linux Kernel inotify File Deletion Monitor
// ────────────────────────────────────────────────────────────────────────────
void inotify_worker(DatabaseManager& db, const std::string& watch_dir) {
    std::cout << "[THREAD 2] Inotify thread started. Watching directory: '" << watch_dir << "'\n";

    // 1. Initialize inotify instance in non-blocking mode
    int inotify_fd = inotify_init1(IN_NONBLOCK);
    if (inotify_fd < 0) {
        std::cerr << "[INOTIFY ERROR] Failed to initialize inotify: " << std::strerror(errno) << "\n";
        return;
    }

    // 2. Watch for file deletions or moves out of the directory
    int watch_desc = inotify_add_watch(inotify_fd, watch_dir.c_str(), IN_DELETE | IN_MOVED_FROM);
    if (watch_desc < 0) {
        std::cerr << "[INOTIFY ERROR] Cannot watch '" << watch_dir << "': " << std::strerror(errno) << "\n";
        close(inotify_fd);
        return;
    }

    constexpr size_t BUF_LEN = 4096;
    char buffer[BUF_LEN] __attribute__((aligned(__alignof__(struct inotify_event))));

    struct pollfd pfd;
    pfd.fd = inotify_fd;
    pfd.events = POLLIN;

    while (g_running.load()) {
        // Poll with 300ms timeout so we can check g_running periodically
        int ret = poll(&pfd, 1, 300);
        if (ret < 0) {
            if (errno == EINTR) continue; // interrupted by signal
            std::cerr << "[INOTIFY ERROR] poll failed: " << std::strerror(errno) << "\n";
            break;
        }

        if (ret > 0 && (pfd.revents & POLLIN)) {
            ssize_t num_read = read(inotify_fd, buffer, BUF_LEN);
            if (num_read <= 0) continue;

            ssize_t offset = 0;
            while (offset < num_read) {
                auto* event = reinterpret_cast<struct inotify_event*>(&buffer[offset]);

                if (event->len > 0) {
                    std::string filename(event->name);
                    if (event->mask & (IN_DELETE | IN_MOVED_FROM)) {
                        std::cout << "\n[ALERT][INOTIFY] Kernel detected deletion: '" 
                                  << filename << "' in " << watch_dir << "!\n";
                        
                        // Log forensic deletion event to SQLite
                        db.log_deleted_file(filename);
                        std::cout << "[DBMS] Deletion event stored in blackbox.db\n\n";
                    }
                }
                offset += sizeof(struct inotify_event) + event->len;
            }
        }
    }

    inotify_rm_watch(inotify_fd, watch_desc);
    close(inotify_fd);
    std::cout << "[THREAD 2] Inotify thread exiting.\n";
}

// ────────────────────────────────────────────────────────────────────────────
// Main Application Entrypoint
// ────────────────────────────────────────────────────────────────────────────
int main() {
    std::cout << "╔══════════════════════════════════════════════════════════════╗\n";
    std::cout << "║    System Blackbox — Native OS Telemetry & DBMS Engine       ║\n";
    std::cout << "║       Operating Systems & Database Management Systems        ║\n";
    std::cout << "╚══════════════════════════════════════════════════════════════╝\n\n";

    // Register signal handlers for clean exit
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);

    const std::string watch_dir = "test_watch";
    std::error_code ec;
    if (!fs::exists(watch_dir, ec)) {
        fs::create_directories(watch_dir, ec);
        std::cout << "[INIT] Created watch directory: ./" << watch_dir << "/\n";
    }

    // 1. Initialize SQLite database manager
    DatabaseManager db;
    if (!db.init("blackbox.db")) {
        std::cerr << "[FATAL] Failed to initialize database blackbox.db. Exiting.\n";
        return 1;
    }

    std::cout << "[INIT] Spawning concurrent worker threads...\n";

    // 2. Spawn concurrent OS worker threads
    std::thread sensor_th(sensor_worker, std::ref(db));
    std::thread inotify_th(inotify_worker, std::ref(db), watch_dir);

    std::cout << "\n[INFO] Daemon running. Press Ctrl+C to terminate cleanly.\n";
    std::cout << "[INFO] Telemetry is being written to blackbox.db in real-time.\n";
    std::cout << "[INFO] You can run 'python3 server.py' in another terminal to view web UI.\n\n";

    // 3. Wait for threads to join upon shutdown signal
    if (sensor_th.joinable()) {
        sensor_th.join();
    }
    if (inotify_th.joinable()) {
        inotify_th.join();
    }

    // 4. Close database connection cleanly (RAII also ensures this)
    db.close();

    std::cout << "[EXIT] System Blackbox shutdown complete.\n";
    return 0;
}
