// ============================================================================
// alerts.hpp — Threshold Alert Detection (with hysteresis)
// ============================================================================
// System Blackbox — Phase 2
// University PBL: Operating Systems + Database Management Systems
//
// Watches one metric (e.g. CPU %) and decides when to raise an alert.
//
// Two rules stop it from spamming the database:
//   1. Sustain:    the value must stay >= threshold for `sustain` samples in a
//                  row before we say TRIGGERED (one short CPU spike is normal).
//   2. Hysteresis: once triggered, it only goes back to normal (RESOLVED)
//                  when the value drops below (threshold - margin). Without
//                  this, a value hovering around 90% would flip on/off every
//                  second.
// ============================================================================

#pragma once

#include <string>

class ThresholdMonitor {
public:
    enum class Change { None, Triggered, Resolved };

    ThresholdMonitor(std::string metric, double threshold, double margin, int sustain = 3)
        : metric_(std::move(metric)), threshold_(threshold), margin_(margin), sustain_(sustain) {}

    // Feed one new sample. Returns Triggered / Resolved only on the sample where the state changes.
    Change update(double value) {
        if (threshold_ <= 0.0) return Change::None;  // threshold 0 = alerting disabled

        if (!active_) {
            above_count_ = (value >= threshold_) ? above_count_ + 1 : 0;  // count consecutive high samples
            if (above_count_ >= sustain_) {
                active_ = true;
                above_count_ = 0;
                return Change::Triggered;
            }
        } else if (value < threshold_ - margin_) {   // dropped clearly below the limit
            active_ = false;
            return Change::Resolved;
        }
        return Change::None;
    }

    const std::string& metric() const { return metric_; }
    double threshold() const { return threshold_; }
    bool active() const { return active_; }

private:
    std::string metric_;     // column name in vitals_log, e.g. "cpu_usage"
    double threshold_;       // alert when value >= this
    double margin_;          // must drop below threshold - margin to resolve
    int    sustain_;         // consecutive samples needed to trigger
    int    above_count_{0};  // how many samples in a row were above the threshold
    bool   active_{false};   // currently in alert state?
};
