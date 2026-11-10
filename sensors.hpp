#pragma once

#include "scheduler.hpp"

#include <array>
#include <string>
#include <vector>

// header for pulling cpu, ram, temp, and fan stats out of /proc and /sys

// container for one snapshot of all four hardware vitals
struct SystemVitals {
    double cpu_usage{0.0};  // cpu usage percentage between 0.0 and 100.0
    double ram_usage{0.0};  // ram usage percentage between 0.0 and 100.0
    double cpu_temp{0.0};   // cpu core temperature in celsius
    int    fan_speed{0};    // cooling fan speed in rpm
};

// reads telemetry directly from linux virtual filesystems
class HardwareSensorReader {
public:
    // grabs initial cpu ticks for baseline and finds our cpu temp file
    HardwareSensorReader();

    // pulls all four metrics at once and returns a SystemVitals struct
    // (dispatched via the OS CPU Task Scheduler: SRTF / FCFS / SJF / RR)
    SystemVitals read_all();

    // parses /proc/stat deltas to compute current cpu utilization
    double read_cpu_usage();

    // parses /proc/meminfo to figure out memory usage percentage
    double read_ram_usage();

    // gets the cpu temperature in celsius from the detected sensor
    double read_cpu_temp();

    // scans hwmon sysfs entries and returns the max rpm across all fans
    int read_fan_speed();

    // name or path of the sensor we ended up using for temperature
    const std::string& temp_source() const { return temp_source_; }

    // configure or inspect the active OS CPU scheduling algorithm (default: SRTF)
    void set_sched_policy(SchedPolicy policy) { scheduler_.set_policy(policy); }
    SchedPolicy sched_policy() const { return scheduler_.policy(); }
    const ScheduleResult& last_schedule() const { return last_schedule_; }

    // build the 4 kernel telemetry probe tasks (PCBs) with current exponentially
    // averaged burst predictions (tau_n) for scheduling or side-by-side comparison
    std::vector<SchedTask> build_probe_tasks(SystemVitals* out = nullptr);

private:
    unsigned long long prev_idle_ticks_{0};   // saved idle ticks from last read for calculating delta
    unsigned long long prev_total_ticks_{0};  // saved total ticks from last read for calculating delta

    std::string temp_path_;    // sysfs path to read cpu temperature from
    std::string temp_source_;  // human readable name of the selected temperature sensor

    std::vector<std::string> fan_paths_;            // cached hwmon fan*_input paths discovered at startup
    std::vector<std::string> fallback_temp_paths_;  // cached thermal paths if primary cpu sensor is absent
    bool fallback_temps_probed_{false};

    // OS CPU Task Scheduler (SRTF by default) & exponential burst predictions (in us):
    //   [0] CPU_STAT (/proc/stat)
    //   [1] RAM_MEMINFO (/proc/meminfo)
    //   [2] THERMAL_SYS (/sys/hwmon)
    //   [3] FAN_HWMON (/sys/hwmon)
    OsTaskScheduler    scheduler_{SchedPolicy::SRTF, 4, 0.5};
    std::array<int, 4> predicted_burst_us_{{20, 26, 12, 8}};
    ScheduleResult     last_schedule_;

    // helper to parse raw tick counters out of /proc/stat
    bool read_proc_stat_ticks(unsigned long long& idle_ticks, unsigned long long& total_ticks);

    // probe sysfs and thermal zones to find the actual cpu temperature sensor
    void detect_cpu_temp_sensor();

    // probe sysfs once at startup to cache cooling fan sensor file paths
    void detect_fan_sensors();

    // cache fallback temperature inputs once so fallback mode avoids per-tick directory scans
    void detect_fallback_temp_sensors();

    // fallback that checks cached thermal sensors and picks the hottest reading
    double read_max_any_temp();
};
