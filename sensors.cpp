// ============================================================================
// sensors.cpp — Linux OS Hardware Telemetry Implementation
// ============================================================================
// System Blackbox — Phase 2
// University PBL: Operating Systems + Database Management Systems
// ============================================================================

#include "sensors.hpp"

#include <iostream>     // for std::cerr (error output)
#include <fstream>      // for std::ifstream (reading files)
#include <sstream>      // for std::istringstream (parsing strings)
#include <string>
#include <vector>
#include <filesystem>   // for scanning /sys/class/hwmon directories
#include <algorithm>    // for std::max, std::clamp
#include <cmath>        // for std::round

namespace fs = std::filesystem;  // shorthand so we don't type std::filesystem everywhere

// Reads the first line of a small sysfs file and strips the trailing newline
static std::string read_line(const fs::path& p) {
    std::ifstream ifs(p);
    std::string s;
    std::getline(ifs, s);
    return s;
}

// Reads a millidegree value from a sysfs temp file; returns degrees or -1 on failure
static double read_millideg(const fs::path& p) {
    std::ifstream ifs(p);
    double val = 0.0;
    if (!(ifs >> val)) return -1.0;
    double deg = val / 1000.0;
    return (deg > 0.0 && deg < 125.0) ? deg : -1.0;  // filter out impossible readings
}

// Constructor: takes an initial reading of CPU ticks so we have a baseline to compare against
HardwareSensorReader::HardwareSensorReader() {
    read_proc_stat_ticks(prev_idle_ticks_, prev_total_ticks_);
    detect_cpu_temp_sensor();
}

// Reads raw CPU tick counts from /proc/stat (a virtual file provided by the kernel)
// Returns idle ticks and total ticks via reference parameters
bool HardwareSensorReader::read_proc_stat_ticks(unsigned long long& idle_ticks, unsigned long long& total_ticks) {
    std::ifstream stat_file("/proc/stat");  // open the kernel's CPU stats file
    if (!stat_file.is_open()) {
        return false;  // can't open /proc/stat
    }

    std::string line;
    if (std::getline(stat_file, line)) {  // read the first line (aggregate CPU stats)
        std::istringstream iss(line);
        std::string cpu_label;
        iss >> cpu_label;  // read the "cpu" label at the start of the line

        if (cpu_label == "cpu") {
            // these are the 10 tick counters from /proc/stat
            unsigned long long user = 0, nice = 0, system = 0, idle = 0;
            unsigned long long iowait = 0, irq = 0, softirq = 0, steal = 0;
            unsigned long long guest = 0, guest_nice = 0;

            // parse all 10 values from the line
            iss >> user >> nice >> system >> idle >> iowait >> irq >> softirq >> steal >> guest >> guest_nice;

            idle_ticks = idle + iowait;  // time CPU spent doing nothing
            // Total CPU time: sum of all primary tick counters. In the Linux kernel (/proc/stat),
            // 'guest' and 'guest_nice' are already accounted for within 'user' and 'nice'.
            // Adding them again would double-count virtualized CPU time.
            total_ticks = user + nice + system + idle + iowait + irq + softirq + steal;
            return true;
        }
    }
    return false;
}

// Calculates CPU usage percentage by comparing current ticks with previous ticks
// Formula: CPU% = (working_time / total_time) * 100
double HardwareSensorReader::read_cpu_usage() {
    unsigned long long current_idle = 0;
    unsigned long long current_total = 0;

    if (!read_proc_stat_ticks(current_idle, current_total)) {
        return 0.0;  // couldn't read /proc/stat
    }

    // compute the difference since last reading
    unsigned long long delta_total = current_total - prev_total_ticks_;
    unsigned long long delta_idle = current_idle - prev_idle_ticks_;

    // save current values as the new baseline for next call
    prev_total_ticks_ = current_total;
    prev_idle_ticks_ = current_idle;

    if (delta_total == 0) {
        return 0.0;  // no time has passed, avoid division by zero
    }

    // (total - idle) = time spent working; divide by total to get percentage
    double usage = 100.0 * static_cast<double>(delta_total - delta_idle) / static_cast<double>(delta_total);
    // round to 1 decimal place and clamp between 0-100
    return std::clamp(std::round(usage * 10.0) / 10.0, 0.0, 100.0);
}

// Reads RAM usage percentage from /proc/meminfo
// Formula: RAM% = (MemTotal - MemAvailable) / MemTotal * 100
double HardwareSensorReader::read_ram_usage() {
    std::ifstream meminfo_file("/proc/meminfo");  // open kernel memory info file
    if (!meminfo_file.is_open()) {
        return 0.0;
    }

    unsigned long long mem_total = 0;
    unsigned long long mem_available = 0;
    unsigned long long mem_free = 0;
    unsigned long long buffers = 0;
    unsigned long long cached = 0;
    std::string line;

    // read lines to find memory statistics
    while (std::getline(meminfo_file, line)) {
        std::istringstream iss(line);
        std::string key;
        unsigned long long value;
        iss >> key >> value;

        if (key == "MemTotal:") {
            mem_total = value;
        } else if (key == "MemAvailable:") {
            mem_available = value;  // includes reclaimable page cache, best metric
        } else if (key == "MemFree:") {
            mem_free = value;
        } else if (key == "Buffers:") {
            buffers = value;
        } else if (key == "Cached:") {
            cached = value;
        }

        if (mem_total > 0 && mem_available > 0) {
            break;  // got both values, no need to read more
        }
    }

    // Fallback if MemAvailable wasn't provided by the kernel
    if (mem_available == 0 && (mem_free > 0 || buffers > 0 || cached > 0)) {
        mem_available = mem_free + buffers + cached;
    }

    if (mem_total == 0) {
        return 0.0;  // avoid division by zero
    }

    double used = static_cast<double>(mem_total - mem_available);
    double pct = (used / static_cast<double>(mem_total)) * 100.0;
    return std::clamp(std::round(pct * 10.0) / 10.0, 0.0, 100.0);  // round and clamp
}

// Finds the sensor that actually measures the CPU (not the SSD, WiFi card, etc.)
// Search order:
//   1. hwmon driver named coretemp (Intel), k10temp / zenpower (AMD), cpu_thermal (ARM)
//      -> prefer the "Package id 0" / "Tctl" / "Tdie" label, else its first temp input
//   2. thermal zone of type x86_pkg_temp / cpu-thermal / cpu_thermal / soc_thermal
//   3. nothing found -> read_cpu_temp() falls back to the hottest sensor of any kind
void HardwareSensorReader::detect_cpu_temp_sensor() {
    std::error_code ec;
    const std::vector<std::string> cpu_drivers  = {"coretemp", "k10temp", "zenpower", "cpu_thermal"};
    const std::vector<std::string> pkg_labels   = {"Package id 0", "Tctl", "Tdie"};

    // Step 1: hwmon drivers
    for (const auto& entry : fs::directory_iterator("/sys/class/hwmon", ec)) {
        std::string name = read_line(entry.path() / "name");
        if (std::find(cpu_drivers.begin(), cpu_drivers.end(), name) == cpu_drivers.end()) continue;

        std::string first_input;  // remember the first tempN_input in case no label matches
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

    // Step 2: thermal zones
    const std::vector<std::string> cpu_zones = {"x86_pkg_temp", "cpu-thermal", "cpu_thermal", "soc_thermal"};
    for (const auto& entry : fs::directory_iterator("/sys/class/thermal", ec)) {
        std::string type = read_line(entry.path() / "type");
        if (std::find(cpu_zones.begin(), cpu_zones.end(), type) != cpu_zones.end()) {
            temp_path_   = (entry.path() / "temp").string();
            temp_source_ = "thermal zone (" + type + ")";
            return;
        }
    }

    // Step 3: no CPU sensor found
    temp_source_ = "fallback: hottest sensor";
}

// Reads CPU temperature (°C). Values in sysfs are in millidegrees (53000 = 53.0°C)
double HardwareSensorReader::read_cpu_temp() {
    if (!temp_path_.empty()) {
        double deg = read_millideg(temp_path_);
        if (deg > 0.0) return std::round(deg * 10.0) / 10.0;  // round to 1 decimal
    }
    return read_max_any_temp();  // sensor missing or unreadable
}

// Fallback: highest reading across every hwmon temp*_input and thermal zone
double HardwareSensorReader::read_max_any_temp() {
    double max_temp = 0.0;
    std::error_code ec;  // used to avoid exceptions from filesystem ops

    for (const auto& entry : fs::directory_iterator("/sys/class/hwmon", ec)) {  // hwmon0, hwmon1...
        for (const auto& file : fs::directory_iterator(entry.path(), ec)) {
            std::string fname = file.path().filename().string();
            // look for files like temp1_input, temp2_input, etc.
            if (fname.rfind("temp", 0) == 0 && fname.ends_with("_input")) {
                max_temp = std::max(max_temp, read_millideg(file.path()));
            }
        }
    }
    for (const auto& entry : fs::directory_iterator("/sys/class/thermal", ec)) {
        if (entry.path().filename().string().rfind("thermal_zone", 0) == 0) {
            max_temp = std::max(max_temp, read_millideg(entry.path() / "temp"));
        }
    }
    return std::round(max_temp * 10.0) / 10.0;
}

// Reads fan speed (RPM) from /sys/class/hwmon/hwmon*/fan*_input
// Returns the fastest fan (a laptop can have an idle fan reading 0 and a spinning one)
// Returns 0 if no fan sensor exists (common on passively cooled systems / VMs)
int HardwareSensorReader::read_fan_speed() {
    std::error_code ec;
    int max_rpm = 0;

    for (const auto& entry : fs::directory_iterator("/sys/class/hwmon", ec)) {
        for (const auto& file : fs::directory_iterator(entry.path(), ec)) {
            std::string fname = file.path().filename().string();
            // look for files like fan1_input, fan2_input
            if (fname.rfind("fan", 0) == 0 && fname.ends_with("_input")) {
                std::ifstream ifs(file.path());
                int rpm = 0;
                if (ifs >> rpm && rpm > max_rpm) {
                    max_rpm = rpm;
                }
            }
        }
    }
    return max_rpm;
}

// Reads all four metrics and packs them into a single struct
// This is what main.cpp calls every second
SystemVitals HardwareSensorReader::read_all() {
    SystemVitals v;
    v.cpu_usage = read_cpu_usage();
    v.ram_usage = read_ram_usage();
    v.cpu_temp  = read_cpu_temp();
    v.fan_speed = read_fan_speed();
    return v;
}
