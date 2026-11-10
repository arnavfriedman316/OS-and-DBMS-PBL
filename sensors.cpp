// implementation of hardware sensor collection from linux /proc and /sys filesystems

#include "sensors.hpp"

#include <cstdio>       // for fast low-overhead FILE* reading of pseudo-files
#include <cstring>      // for std::strncmp
#include <fstream>      // for reading sysfs files during discovery
#include <string>
#include <vector>
#include <filesystem>   // for scanning directories in sysfs
#include <algorithm>    // for std::max and std::clamp
#include <cmath>        // for std::round
#include <chrono>       // for measuring actual microsecond burst times (t_n) of kernel probes

namespace fs = std::filesystem;  // short alias so we don't have to keep writing std::filesystem

// reads a single line from a sysfs file and trims trailing junk
static std::string read_line(const fs::path& p) {
    std::ifstream ifs(p);
    std::string s;
    std::getline(ifs, s);
    while (!s.empty() && (s.back() == '\r' || s.back() == ' ' || s.back() == '\t')) {
        s.pop_back();
    }
    return s;
}

// sysfs temperature files give values in millidegrees celsius, so divide by 1000
static double read_millideg(const std::string& path) {
    FILE* fp = std::fopen(path.c_str(), "r");
    if (!fp) return -1.0;
    double val = 0.0;
    int matched = std::fscanf(fp, "%lf", &val);
    std::fclose(fp);
    if (matched != 1) return -1.0;
    double deg = val / 1000.0;
    return (deg > 0.0 && deg < 125.0) ? deg : -1.0;  // filter out bogus readings
}

// store initial tick snapshot so our very first cpu delta calculation has a baseline
HardwareSensorReader::HardwareSensorReader() {
    read_proc_stat_ticks(prev_idle_ticks_, prev_total_ticks_);
    detect_cpu_temp_sensor();
    detect_fan_sensors();
}

// pull raw jiffies/ticks from /proc/stat for aggregate cpu time
bool HardwareSensorReader::read_proc_stat_ticks(unsigned long long& idle_ticks, unsigned long long& total_ticks) {
    FILE* stat_file = std::fopen("/proc/stat", "r");  // open the kernel statistics pseudo-file
    if (!stat_file) {
        return false;
    }

    char line[512];
    bool ok = false;
    if (std::fgets(line, sizeof(line), stat_file)) {  // grab the first line which has aggregate cpu ticks
        // parse the 8 primary tick counters reported by the linux kernel
        // (guest and guest_nice are omitted because they are already included in user/nice by the kernel)
        unsigned long long user = 0, nice = 0, system = 0, idle = 0;
        unsigned long long iowait = 0, irq = 0, softirq = 0, steal = 0;

        if (std::sscanf(line, "cpu %llu %llu %llu %llu %llu %llu %llu %llu",
                        &user, &nice, &system, &idle, &iowait, &irq, &softirq, &steal) >= 4) {
            idle_ticks  = idle + iowait;  // idle ticks include regular idle and iowait time
            total_ticks = user + nice + system + idle + iowait + irq + softirq + steal;
            ok = true;
        }
    }
    std::fclose(stat_file);
    return ok;
}

// calculate cpu utilization percentage by comparing current ticks against our last sample
double HardwareSensorReader::read_cpu_usage() {
    unsigned long long current_idle = 0;
    unsigned long long current_total = 0;

    if (!read_proc_stat_ticks(current_idle, current_total)) {
        return 0.0;  // couldn't read /proc/stat, return 0 as safe fallback
    }

    if (current_total <= prev_total_ticks_) {
        prev_total_ticks_ = current_total;
        prev_idle_ticks_  = current_idle;
        return 0.0;  // avoid dividing by zero or unsigned wraparound if called with no elapsed ticks
    }

    // measure how many ticks passed since the previous read (guarding SMP NOHZ idle counter jitter)
    unsigned long long delta_total = current_total - prev_total_ticks_;
    unsigned long long delta_idle  = (current_idle >= prev_idle_ticks_)
                                   ? (current_idle - prev_idle_ticks_)
                                   : 0ULL;

    // update baseline for next cycle
    prev_total_ticks_ = current_total;
    prev_idle_ticks_  = current_idle;

    if (delta_idle >= delta_total) {
        return 0.0;
    }

    // time working divided by total time gives percentage
    double usage = 100.0 * static_cast<double>(delta_total - delta_idle) / static_cast<double>(delta_total);
    // round to one decimal place and clamp to 0.0 - 100.0 range
    return std::clamp(std::round(usage * 10.0) / 10.0, 0.0, 100.0);
}

// calculate memory usage percentage by parsing /proc/meminfo
double HardwareSensorReader::read_ram_usage() {
    FILE* meminfo_file = std::fopen("/proc/meminfo", "r");
    if (!meminfo_file) {
        return 0.0;
    }

    unsigned long long mem_total = 0;
    unsigned long long mem_available = 0;
    unsigned long long mem_free = 0;
    unsigned long long buffers = 0;
    unsigned long long cached = 0;
    bool saw_mem_available = false;
    char line[256];

    // parse line by line using stack buffer to locate total and available memory fields
    while (std::fgets(line, sizeof(line), meminfo_file)) {
        unsigned long long val = 0;
        if (std::strncmp(line, "MemTotal:", 9) == 0) {
            if (std::sscanf(line + 9, "%llu", &val) == 1) mem_total = val;
        } else if (std::strncmp(line, "MemAvailable:", 13) == 0) {
            if (std::sscanf(line + 13, "%llu", &val) == 1) {
                mem_available = val;  // accounts for reclaimable cache, most accurate metric
                saw_mem_available = true;
            }
        } else if (std::strncmp(line, "MemFree:", 8) == 0) {
            if (std::sscanf(line + 8, "%llu", &val) == 1) mem_free = val;
        } else if (std::strncmp(line, "Buffers:", 8) == 0) {
            if (std::sscanf(line + 8, "%llu", &val) == 1) buffers = val;
        } else if (std::strncmp(line, "Cached:", 7) == 0) {
            if (std::sscanf(line + 7, "%llu", &val) == 1) cached = val;
        }

        if (mem_total > 0 && saw_mem_available) {
            break;  // once we have both total and available memory we can stop reading
        }
    }
    std::fclose(meminfo_file);

    // fallback for ancient kernels without MemAvailable
    if (!saw_mem_available && (mem_free > 0 || buffers > 0 || cached > 0)) {
        mem_available = mem_free + buffers + cached;
    }

    if (mem_total == 0 || mem_available >= mem_total) {
        return 0.0;  // sanity check against division by zero or unsigned underflow
    }

    // compute used percentage, round to one decimal place, and clamp
    double used = static_cast<double>(mem_total - mem_available);
    double pct = (used / static_cast<double>(mem_total)) * 100.0;
    return std::clamp(std::round(pct * 10.0) / 10.0, 0.0, 100.0);
}

// find the specific thermal sensor monitoring the cpu instead of wifi/gpu/nvme
void HardwareSensorReader::detect_cpu_temp_sensor() {
    std::error_code ec;
    const std::vector<std::string> cpu_drivers = {"coretemp", "k10temp", "zenpower", "zenergy", "cpu_thermal"};
    const std::vector<std::string> pkg_labels  = {"Package id 0", "Tctl", "Tdie", "CPU"};

    // 1. check hwmon drivers for known cpu thermal module names in cpu_drivers
    for (const auto& entry : fs::directory_iterator("/sys/class/hwmon", ec)) {
        std::string name = read_line(entry.path() / "name");
        if (std::find(cpu_drivers.begin(), cpu_drivers.end(), name) == cpu_drivers.end()) continue;

        std::string first_input;  // remember first temp input in case no specific label matches
        for (int i = 1; i <= 64; ++i) {
            fs::path input = entry.path() / ("temp" + std::to_string(i) + "_input");
            if (!fs::exists(input, ec)) continue;
            if (first_input.empty()) first_input = input.string();

            std::string label = read_line(entry.path() / ("temp" + std::to_string(i) + "_label"));
            if (std::find(pkg_labels.begin(), pkg_labels.end(), label) != pkg_labels.end()) {
                temp_path_   = input.string();
                temp_source_ = name + " (" + label + ")";
                return;
            }
        }
        if (!first_input.empty()) {
            temp_path_   = first_input;
            temp_source_ = name;
            return;
        }
    }

    // 2. check thermal zones if hwmon didn't match a known cpu driver
    const std::vector<std::string> cpu_zones = {"x86_pkg_temp", "cpu-thermal", "cpu_thermal", "soc_thermal"};
    for (const auto& entry : fs::directory_iterator("/sys/class/thermal", ec)) {
        std::string type = read_line(entry.path() / "type");
        if (std::find(cpu_zones.begin(), cpu_zones.end(), type) != cpu_zones.end()) {
            temp_path_   = (entry.path() / "temp").string();
            temp_source_ = "thermal zone (" + type + ")";
            return;
        }
    }

    // 3. no direct cpu sensor matched, fallback to hottest sensor in system
    temp_source_ = "fallback: hottest sensor";
    detect_fallback_temp_sensors();
}

// discover and cache cooling fan sysfs inputs once at startup to avoid per-tick directory scans
void HardwareSensorReader::detect_fan_sensors() {
    fan_paths_.clear();
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator("/sys/class/hwmon", ec)) {
        for (const auto& file : fs::directory_iterator(entry.path(), ec)) {
            std::string fname = file.path().filename().string();
            if (fname.rfind("fan", 0) == 0 && fname.ends_with("_input")) {
                fan_paths_.push_back(file.path().string());
            }
        }
    }
}

// discover fallback thermal inputs once, skipping slow wireless or sleeping dgpu sensors when possible
void HardwareSensorReader::detect_fallback_temp_sensors() {
    if (fallback_temps_probed_) return;
    fallback_temps_probed_ = true;
    fallback_temp_paths_.clear();

    std::error_code ec;
    std::vector<std::string> deferred_slow;

    for (const auto& entry : fs::directory_iterator("/sys/class/hwmon", ec)) {
        std::string drv = read_line(entry.path() / "name");
        // iwlwifi / nouveau / amdgpu PCIe reads can stall 15ms-500ms waking hardware
        bool is_slow_peripheral = (drv.rfind("iwlwifi", 0) == 0 || drv == "nouveau" || drv == "amdgpu");
        for (const auto& file : fs::directory_iterator(entry.path(), ec)) {
            std::string fname = file.path().filename().string();
            if (fname.rfind("temp", 0) == 0 && fname.ends_with("_input")) {
                if (is_slow_peripheral) {
                    deferred_slow.push_back(file.path().string());
                } else {
                    fallback_temp_paths_.push_back(file.path().string());
                }
            }
        }
    }
    for (const auto& entry : fs::directory_iterator("/sys/class/thermal", ec)) {
        if (entry.path().filename().string().rfind("thermal_zone", 0) == 0) {
            fs::path tp = entry.path() / "temp";
            if (fs::exists(tp, ec)) {
                fallback_temp_paths_.push_back(tp.string());
            }
        }
    }
    if (fallback_temp_paths_.empty()) {
        fallback_temp_paths_ = std::move(deferred_slow);
    }
}

// read cpu temperature in degrees celsius from the detected sensor path
double HardwareSensorReader::read_cpu_temp() {
    if (!temp_path_.empty()) {
        double deg = read_millideg(temp_path_);
        if (deg > 0.0) return std::round(deg * 10.0) / 10.0;
    }
    return read_max_any_temp();  // if sensor reading fails or path is missing, fall back
}

// fallback scan: check cached temperature inputs without walking directories every second
double HardwareSensorReader::read_max_any_temp() {
    if (!fallback_temps_probed_) {
        detect_fallback_temp_sensors();
    }
    double max_temp = 0.0;
    for (const auto& path : fallback_temp_paths_) {
        max_temp = std::max(max_temp, read_millideg(path));
    }
    return std::round(max_temp * 10.0) / 10.0;
}

// read cached cooling fan speed inputs and return the highest rpm
int HardwareSensorReader::read_fan_speed() {
    int max_rpm = 0;

    // laptops often have two fans where one might be idle at 0 rpm, so pick the fastest one
    for (const auto& path : fan_paths_) {
        FILE* fp = std::fopen(path.c_str(), "r");
        if (!fp) continue;
        int rpm = 0;
        if (std::fscanf(fp, "%d", &rpm) == 1 && rpm > max_rpm) {
            max_rpm = rpm;
        }
        std::fclose(fp);
    }
    return max_rpm;
}

// build the 4 kernel telemetry probe tasks (PCBs) with staggered arrival times
// (0, 2, 4, 6 us) and exponentially-averaged predicted burst times (tau_n)
std::vector<SchedTask> HardwareSensorReader::build_probe_tasks(SystemVitals* out) {
    std::vector<SchedTask> tasks;
    tasks.reserve(4);

    tasks.push_back(SchedTask{
        1, "CPU_STAT (/proc/stat)",
        0, std::max(1, predicted_burst_us_[0]), 0,
        -1, 0, 0, 0, 0,
        out ? std::function<void()>([this, out]() {
            out->cpu_usage = read_cpu_usage();
        }) : std::function<void()>{}
    });

    tasks.push_back(SchedTask{
        2, "RAM_INFO (/proc/meminfo)",
        2, std::max(1, predicted_burst_us_[1]), 0,
        -1, 0, 0, 0, 0,
        out ? std::function<void()>([this, out]() {
            out->ram_usage = read_ram_usage();
        }) : std::function<void()>{}
    });

    tasks.push_back(SchedTask{
        3, "THERMAL_SYS (/sys/hwmon)",
        4, std::max(1, predicted_burst_us_[2]), 0,
        -1, 0, 0, 0, 0,
        out ? std::function<void()>([this, out]() {
            out->cpu_temp = read_cpu_temp();
        }) : std::function<void()>{}
    });

    tasks.push_back(SchedTask{
        4, "FAN_HWMON (/sys/hwmon)",
        6, std::max(1, predicted_burst_us_[3]), 0,
        -1, 0, 0, 0, 0,
        out ? std::function<void()>([this, out]() {
            out->fan_speed = read_fan_speed();
        }) : std::function<void()>{}
    });

    return tasks;
}

// read all hardware vitals by scheduling the 4 kernel probes via OsTaskScheduler
// (SRTF by default, or FCFS / SJF / Round Robin) and updating each probe's
// predicted burst time via textbook exponential averaging:
//   tau_{n+1} = alpha * t_n + (1 - alpha) * tau_n
SystemVitals HardwareSensorReader::read_all() {
    SystemVitals v;
    auto tasks = build_probe_tasks(&v);

    // compute schedule using configured OS CPU scheduling algorithm
    last_schedule_ = scheduler_.schedule(tasks);

    // dispatch kernel probes in scheduler-selected order and measure actual burst t_n
    for (size_t idx : last_schedule_.dispatch_order) {
        if (idx >= tasks.size() || !tasks[idx].work) continue;

        auto t0 = std::chrono::steady_clock::now();
        tasks[idx].work();
        auto t1 = std::chrono::steady_clock::now();

        int actual_us = static_cast<int>(
            std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count()
        );
        // clamp microsecond reading to [2, 200] us to guard against OS preemption outliers
        actual_us = std::clamp(actual_us, 2, 200);

        predicted_burst_us_[idx] = scheduler_.predict_next_burst(
            predicted_burst_us_[idx], actual_us
        );
    }

    return v;
}
