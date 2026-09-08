// ============================================================================
// sensors.cpp — Linux OS Hardware Telemetry Implementation
// ============================================================================
// System Blackbox — Phase 2
// University PBL: Operating Systems + Database Management Systems
// ============================================================================

#include "sensors.hpp"

#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <filesystem>
#include <algorithm>
#include <cmath>

namespace fs = std::filesystem;

HardwareSensorReader::HardwareSensorReader() {
    // Initialise tick counters so first read has baseline
    read_proc_stat_ticks(prev_idle_ticks_, prev_total_ticks_);
}

bool HardwareSensorReader::read_proc_stat_ticks(unsigned long long& idle_ticks, unsigned long long& total_ticks) {
    std::ifstream stat_file("/proc/stat");
    if (!stat_file.is_open()) {
        return false;
    }

    std::string line;
    if (std::getline(stat_file, line)) {
        std::istringstream iss(line);
        std::string cpu_label;
        iss >> cpu_label;

        if (cpu_label == "cpu") {
            unsigned long long user = 0, nice = 0, system = 0, idle = 0;
            unsigned long long iowait = 0, irq = 0, softirq = 0, steal = 0;
            unsigned long long guest = 0, guest_nice = 0;

            iss >> user >> nice >> system >> idle >> iowait >> irq >> softirq >> steal >> guest >> guest_nice;

            idle_ticks = idle + iowait;
            total_ticks = user + nice + system + idle + iowait + irq + softirq + steal + guest + guest_nice;
            return true;
        }
    }
    return false;
}

double HardwareSensorReader::read_cpu_usage() {
    unsigned long long current_idle = 0;
    unsigned long long current_total = 0;

    if (!read_proc_stat_ticks(current_idle, current_total)) {
        return 0.0;
    }

    unsigned long long delta_total = current_total - prev_total_ticks_;
    unsigned long long delta_idle = current_idle - prev_idle_ticks_;

    prev_total_ticks_ = current_total;
    prev_idle_ticks_ = current_idle;

    if (delta_total == 0) {
        return 0.0;
    }

    double usage = 100.0 * static_cast<double>(delta_total - delta_idle) / static_cast<double>(delta_total);
    return std::clamp(std::round(usage * 10.0) / 10.0, 0.0, 100.0);
}

double HardwareSensorReader::read_ram_usage() {
    std::ifstream meminfo_file("/proc/meminfo");
    if (!meminfo_file.is_open()) {
        return 0.0;
    }

    unsigned long long mem_total = 0;
    unsigned long long mem_available = 0;
    std::string line;

    while (std::getline(meminfo_file, line)) {
        std::istringstream iss(line);
        std::string key;
        unsigned long long value;
        iss >> key >> value;

        if (key == "MemTotal:") {
            mem_total = value;
        } else if (key == "MemAvailable:") {
            mem_available = value;
        }

        if (mem_total > 0 && mem_available > 0) {
            break;
        }
    }

    if (mem_total == 0) {
        return 0.0;
    }

    double used = static_cast<double>(mem_total - mem_available);
    double pct = (used / static_cast<double>(mem_total)) * 100.0;
    return std::clamp(std::round(pct * 10.0) / 10.0, 0.0, 100.0);
}

double HardwareSensorReader::read_cpu_temp() {
    double max_temp = 0.0;

    // 1. Check hwmon paths (/sys/class/hwmon/hwmon*/temp*_input)
    std::error_code ec;
    if (fs::exists("/sys/class/hwmon", ec)) {
        for (const auto& entry : fs::directory_iterator("/sys/class/hwmon", ec)) {
            if (!entry.is_directory(ec)) continue;
            for (const auto& file : fs::directory_iterator(entry.path(), ec)) {
                std::string fname = file.path().filename().string();
                if (fname.rfind("temp", 0) == 0 && fname.find("_input") != std::string::npos) {
                    std::ifstream ifs(file.path());
                    double val = 0.0;
                    if (ifs >> val) {
                        double deg = val / 1000.0;
                        if (deg > 0.0 && deg < 125.0) {
                            max_temp = std::max(max_temp, deg);
                        }
                    }
                }
            }
        }
    }

    if (max_temp > 0.0) {
        return std::round(max_temp * 10.0) / 10.0;
    }

    // 2. Fallback: thermal zones (/sys/class/thermal/thermal_zone*/temp)
    if (fs::exists("/sys/class/thermal", ec)) {
        for (const auto& entry : fs::directory_iterator("/sys/class/thermal", ec)) {
            std::string dname = entry.path().filename().string();
            if (dname.rfind("thermal_zone", 0) == 0) {
                fs::path temp_file = entry.path() / "temp";
                std::ifstream ifs(temp_file);
                double val = 0.0;
                if (ifs >> val) {
                    double deg = val / 1000.0;
                    if (deg > 0.0 && deg < 125.0) {
                        max_temp = std::max(max_temp, deg);
                    }
                }
            }
        }
    }

    return std::round(max_temp * 10.0) / 10.0;
}

int HardwareSensorReader::read_fan_speed() {
    std::error_code ec;
    if (!fs::exists("/sys/class/hwmon", ec)) {
        return 0;
    }

    for (const auto& entry : fs::directory_iterator("/sys/class/hwmon", ec)) {
        if (!entry.is_directory(ec)) continue;
        for (const auto& file : fs::directory_iterator(entry.path(), ec)) {
            std::string fname = file.path().filename().string();
            if (fname.rfind("fan", 0) == 0 && fname.find("_input") != std::string::npos) {
                std::ifstream ifs(file.path());
                int rpm = 0;
                if (ifs >> rpm && rpm >= 0) {
                    return rpm;
                }
            }
        }
    }

    return 0;
}

SystemVitals HardwareSensorReader::read_all() {
    SystemVitals v;
    v.cpu_usage = read_cpu_usage();
    v.ram_usage = read_ram_usage();
    v.cpu_temp  = read_cpu_temp();
    v.fan_speed = read_fan_speed();
    return v;
}
