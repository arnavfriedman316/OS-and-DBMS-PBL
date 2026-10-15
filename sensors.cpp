// implementation of hardware sensor collection from linux /proc and /sys filesystems

#include "sensors.hpp"

#include <iostream>     // for error logging
#include <fstream>      // for reading files
#include <sstream>      // for parsing string lines
#include <string>
#include <vector>
#include <filesystem>   // for scanning directories in sysfs
#include <algorithm>    // for std::max and std::clamp
#include <cmath>        // for std::round

namespace fs = std::filesystem;  // short alias so we don't have to keep writing std::filesystem

// reads a single line from a sysfs file and trims trailing junk
static std::string read_line(const fs::path& p) {
    std::ifstream ifs(p);
    std::string s;
    std::getline(ifs, s);
    return s;
}

// sysfs temperature files give values in millidegrees celsius, so divide by 1000
static double read_millideg(const fs::path& p) {
    std::ifstream ifs(p);
    double val = 0.0;
    if (!(ifs >> val)) return -1.0;
    double deg = val / 1000.0;
    return (deg > 0.0 && deg < 125.0) ? deg : -1.0;  // filter out bogus readings
}

// store initial tick snapshot so our very first cpu delta calculation has a baseline
HardwareSensorReader::HardwareSensorReader() {
    read_proc_stat_ticks(prev_idle_ticks_, prev_total_ticks_);
    detect_cpu_temp_sensor();
}

// pull raw jiffies/ticks from /proc/stat for aggregate cpu time
bool HardwareSensorReader::read_proc_stat_ticks(unsigned long long& idle_ticks, unsigned long long& total_ticks) {
    std::ifstream stat_file("/proc/stat");  // open the kernel statistics pseudo-file
    if (!stat_file.is_open()) {
        return false;
    }

    std::string line;
    if (std::getline(stat_file, line)) {  // grab the first line which has aggregate cpu ticks
        std::istringstream iss(line);
        std::string cpu_label;
        iss >> cpu_label;

        if (cpu_label == "cpu") {
            // parse all 10 tick counters reported by the linux kernel
            unsigned long long user = 0, nice = 0, system = 0, idle = 0;
            unsigned long long iowait = 0, irq = 0, softirq = 0, steal = 0;
            unsigned long long guest = 0, guest_nice = 0;

            iss >> user >> nice >> system >> idle >> iowait >> irq >> softirq >> steal >> guest >> guest_nice;

            idle_ticks = idle + iowait;  // idle ticks include regular idle and iowait time
            // sum up tick counters; guest time is already included in user/nice by the kernel
            total_ticks = user + nice + system + idle + iowait + irq + softirq + steal;
            return true;
        }
    }
    return false;
}

// calculate cpu utilization percentage by comparing current ticks against our last sample
double HardwareSensorReader::read_cpu_usage() {
    unsigned long long current_idle = 0;
    unsigned long long current_total = 0;

    if (!read_proc_stat_ticks(current_idle, current_total)) {
        return 0.0;  // couldn't read /proc/stat, return 0 as safe fallback
    }

    // measure how many ticks passed since the previous read
    unsigned long long delta_total = current_total - prev_total_ticks_;
    unsigned long long delta_idle = current_idle - prev_idle_ticks_;

    // update baseline for next cycle
    prev_total_ticks_ = current_total;
    prev_idle_ticks_ = current_idle;

    if (delta_total == 0) {
        return 0.0;  // avoid dividing by zero if called too quickly with no elapsed ticks
    }

    // time working divided by total time gives percentage
    double usage = 100.0 * static_cast<double>(delta_total - delta_idle) / static_cast<double>(delta_total);
    // round to one decimal place and clamp to 0.0 - 100.0 range
    return std::clamp(std::round(usage * 10.0) / 10.0, 0.0, 100.0);
}

// calculate memory usage percentage by parsing /proc/meminfo
double HardwareSensorReader::read_ram_usage() {
    std::ifstream meminfo_file("/proc/meminfo");
    if (!meminfo_file.is_open()) {
        return 0.0;
    }

    unsigned long long mem_total = 0;
    unsigned long long mem_available = 0;
    unsigned long long mem_free = 0;
    unsigned long long buffers = 0;
    unsigned long long cached = 0;
    std::string line;

    // parse line by line to locate total and available memory fields
    while (std::getline(meminfo_file, line)) {
        std::istringstream iss(line);
        std::string key;
        unsigned long long value;
        iss >> key >> value;

        if (key == "MemTotal:") {
            mem_total = value;
        } else if (key == "MemAvailable:") {
            mem_available = value;  // accounts for reclaimable cache, most accurate metric
        } else if (key == "MemFree:") {
            mem_free = value;
        } else if (key == "Buffers:") {
            buffers = value;
        } else if (key == "Cached:") {
            cached = value;
        }

        if (mem_total > 0 && mem_available > 0) {
            break;  // once we have both total and available memory we can stop reading
        }
    }

    // fallback for ancient kernels without MemAvailable
    if (mem_available == 0 && (mem_free > 0 || buffers > 0 || cached > 0)) {
        mem_available = mem_free + buffers + cached;
    }

    if (mem_total == 0) {
        return 0.0;  // sanity check against division by zero
    }

    // compute used percentage, round to one decimal place, and clamp
    double used = static_cast<double>(mem_total - mem_available);
    double pct = (used / static_cast<double>(mem_total)) * 100.0;
    return std::clamp(std::round(pct * 10.0) / 10.0, 0.0, 100.0);
}

// find the specific thermal sensor monitoring the cpu instead of wifi/gpu/nvme
void HardwareSensorReader::detect_cpu_temp_sensor() {
    std::error_code ec;
    const std::vector<std::string> cpu_drivers  = {"coretemp", "k10temp", "zenpower", "cpu_thermal"};
    const std::vector<std::string> pkg_labels   = {"Package id 0", "Tctl", "Tdie"};

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
}

// read cpu temperature in degrees celsius from the detected sensor path
double HardwareSensorReader::read_cpu_temp() {
    if (!temp_path_.empty()) {
        double deg = read_millideg(temp_path_);
        if (deg > 0.0) return std::round(deg * 10.0) / 10.0;
    }
    return read_max_any_temp();  // if sensor reading fails or path is missing, fall back
}

// fallback scan: check every temperature input across all hwmon and thermal zones
double HardwareSensorReader::read_max_any_temp() {
    double max_temp = 0.0;
    std::error_code ec;

    // scan hwmon devices for any temp*_input files
    for (const auto& entry : fs::directory_iterator("/sys/class/hwmon", ec)) {
        for (const auto& file : fs::directory_iterator(entry.path(), ec)) {
            std::string fname = file.path().filename().string();
            if (fname.rfind("temp", 0) == 0 && fname.ends_with("_input")) {
                max_temp = std::max(max_temp, read_millideg(file.path()));
            }
        }
    }
    // check generic acpi thermal zones
    for (const auto& entry : fs::directory_iterator("/sys/class/thermal", ec)) {
        if (entry.path().filename().string().rfind("thermal_zone", 0) == 0) {
            max_temp = std::max(max_temp, read_millideg(entry.path() / "temp"));
        }
    }
    return std::round(max_temp * 10.0) / 10.0;
}

// scan hwmon for cooling fan speeds and return the highest rpm
int HardwareSensorReader::read_fan_speed() {
    std::error_code ec;
    int max_rpm = 0;

    // laptops often have two fans where one might be idle at 0 rpm, so pick the fastest one
    for (const auto& entry : fs::directory_iterator("/sys/class/hwmon", ec)) {
        for (const auto& file : fs::directory_iterator(entry.path(), ec)) {
            std::string fname = file.path().filename().string();
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

// read all hardware vitals and pack them into a SystemVitals struct
SystemVitals HardwareSensorReader::read_all() {
    SystemVitals v;
    v.cpu_usage = read_cpu_usage();
    v.ram_usage = read_ram_usage();
    v.cpu_temp  = read_cpu_temp();
    v.fan_speed = read_fan_speed();
    return v;
}
