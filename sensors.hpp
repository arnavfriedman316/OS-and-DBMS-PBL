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

struct SystemVitals {
    double cpu_usage{0.0};  // CPU percentage [0.0 - 100.0]
    double ram_usage{0.0};  // RAM percentage [0.0 - 100.0]
    double cpu_temp{0.0};   // Temperature in Celsius
    int    fan_speed{0};    // Fan speed in RPM
};

class HardwareSensorReader {
public:
    HardwareSensorReader();

    /// Reads all current system vitals directly from the OS.
    SystemVitals read_all();

    /// Computes CPU utilization % using deltas between consecutive /proc/stat reads.
    double read_cpu_usage();

    /// Computes RAM utilization % from /proc/meminfo.
    double read_ram_usage();

    /// Discovers and reads CPU temperature in Celsius from /sys/class/hwmon or thermal zones.
    double read_cpu_temp();

    /// Discovers and reads Fan Speed in RPM from /sys/class/hwmon.
    int read_fan_speed();

private:
    unsigned long long prev_idle_ticks_{0};
    unsigned long long prev_total_ticks_{0};

    // Helper: read raw idle and total ticks from /proc/stat
    bool read_proc_stat_ticks(unsigned long long& idle_ticks, unsigned long long& total_ticks);
};
