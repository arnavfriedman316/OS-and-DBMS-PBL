// ============================================================================
// sensors.hpp — Linux OS Hardware Telemetry Interface
// ============================================================================
// System Blackbox — Phase 2
// University PBL: Operating Systems + Database Management Systems
//
// PURPOSE:
//   Provides low-level C++ routines for interrogating the Linux virtual
//   filesystems (/proc and /sys).
//
// OS CONCEPTS DEMONSTRATED:
//   • /proc filesystem   — kernel data structures exposed as pseudo-files.
//   • /proc/stat         — kernel jiffies/ticks for CPU utilization calculation.
//   • /proc/meminfo      — dynamic virtual memory manager accounting.
//   • /sys filesystem    — sysfs unified device model for hardware sensors (hwmon).
// ============================================================================

#pragma once

#include <string>

// Struct to store the 4 hardware metrics collected from the system
struct SystemVitals {
    double cpu_usage{0.0};  // CPU utilization percentage (0.0 to 100.0)
    double ram_usage{0.0};  // RAM utilization percentage (0.0 to 100.0)
    double cpu_temp{0.0};   // CPU temperature in degrees Celsius
    int    fan_speed{0};    // Cooling fan speed in RPM (revolutions per minute)
};

// Class responsible for reading hardware metrics from Linux kernel files
class HardwareSensorReader {
public:
    // Constructor: captures the initial CPU tick counts as a baseline
    // and finds which sensor is the real CPU temperature sensor
    HardwareSensorReader();

    // Reads all 4 metrics (CPU, RAM, Temp, Fan) and returns them in a struct
    SystemVitals read_all();

    // Calculates CPU usage % by comparing delta ticks between reads in /proc/stat
    double read_cpu_usage();

    // Calculates RAM usage % using MemTotal and MemAvailable from /proc/meminfo
    double read_ram_usage();

    // Reads the CPU temperature (°C) from the sensor found at startup
    double read_cpu_temp();

    // Scans /sys/class/hwmon to read cooling fan speed in RPM (fastest fan)
    int read_fan_speed();

    // Human-readable description of which temperature sensor is being used
    // e.g. "coretemp (Package id 0)" — printed at startup
    const std::string& temp_source() const { return temp_source_; }

private:
    unsigned long long prev_idle_ticks_{0};   // Stores previous idle CPU ticks for delta calculation
    unsigned long long prev_total_ticks_{0};  // Stores previous total CPU ticks for delta calculation

    std::string temp_path_;    // path of the chosen CPU temp file (empty = fallback scan)
    std::string temp_source_;  // description of the chosen sensor

    // Helper: parses /proc/stat to extract idle ticks and total ticks
    bool read_proc_stat_ticks(unsigned long long& idle_ticks, unsigned long long& total_ticks);

    // Helper: looks through hwmon / thermal zones for a CPU-specific sensor
    void detect_cpu_temp_sensor();

    // Helper: old behaviour — highest reading of any temperature sensor
    double read_max_any_temp();
};
