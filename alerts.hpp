#pragma once

#include <string>

// simple hysteresis threshold checker so we don't spam alerts on temporary spikes
// tracks a single metric and only triggers when it stays hot for a few ticks
class ThresholdMonitor {
public:
    // tells caller whether state flipped this tick
    enum class Change { None, Triggered, Resolved };

    // initialize thresholds and require sustain ticks before firing
    ThresholdMonitor(std::string metric, double threshold, double margin, int sustain = 3)
        : metric_(std::move(metric)), threshold_(threshold), margin_(margin), sustain_(sustain) {}

    // feed in latest reading, returns Change::Triggered or Change::Resolved on edge transition
    Change update(double value) {
        if (threshold_ <= 0.0) return Change::None;  // 0 or negative threshold means alert disabled

        if (!active_) {
            above_count_ = (value >= threshold_) ? above_count_ + 1 : 0;  // count consecutive high samples
            if (above_count_ >= sustain_) {
                active_ = true;
                above_count_ = 0;
                return Change::Triggered;  // tripped the limit for long enough
            }
        } else if (value < threshold_ - margin_) {   // dipped clearly below threshold minus margin
            active_ = false;
            return Change::Resolved;  // back to normal
        }
        return Change::None;
    }

    // which metric we are watching
    const std::string& metric() const { return metric_; }
    // current threshold limit
    double threshold() const { return threshold_; }
    // whether we are currently in alert state
    bool active() const { return active_; }

private:
    std::string metric_;     // metric column name like cpu_usage
    double threshold_;       // trigger boundary
    double margin_;          // hysteresis cushion so reading doesn't oscillate on/off
    int    sustain_;         // consecutive samples needed before firing
    int    above_count_{0};  // running tally of consecutive high samples
    bool   active_{false};   // flag for active alert state
};
