#pragma once

#include <string>

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

private:
    unsigned long long prev_idle_ticks_{0};   // saved idle ticks from last read for calculating delta
    unsigned long long prev_total_ticks_{0};  // saved total ticks from last read for calculating delta

    std::string temp_path_;    // sysfs path to read cpu temperature from
    std::string temp_source_;  // human readable name of the selected temperature sensor

    // helper to parse raw tick counters out of /proc/stat
    bool read_proc_stat_ticks(unsigned long long& idle_ticks, unsigned long long& total_ticks);

    // probe sysfs and thermal zones to find the actual cpu temperature sensor
    void detect_cpu_temp_sensor();

    // fallback that scans every thermal zone and picks the hottest reading
    double read_max_any_temp();
};
