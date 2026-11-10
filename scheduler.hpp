#pragma once
// ═══════════════════════════════════════════════════════════════════════════
//  scheduler.hpp — OS Layer: Classic CPU Scheduling Algorithms
//
//  Implements university OS syllabus CPU scheduling algorithms:
//    1. SRTF (Shortest Remaining Time First — Preemptive SJF)
//    2. FCFS (First-Come, First-Served — Non-Preemptive FIFO)
//    3. SJF  (Shortest Job First — Non-Preemptive)
//    4. RR   (Round Robin — Preemptive Time-Quantum Scheduling)
//
//  Includes textbook exponential burst averaging:
//    tau_{n+1} = alpha * t_n + (1 - alpha) * tau_n
//  and computes per-process Completion Time (CT), Turnaround Time (TAT),
//  Waiting Time (WT), Response Time (RT), Context Switches, and Gantt charts.
// ═══════════════════════════════════════════════════════════════════════════

#include <algorithm>
#include <cctype>
#include <cmath>
#include <functional>
#include <iomanip>
#include <limits>
#include <queue>
#include <sstream>
#include <string>
#include <vector>

// ─── Supported University Syllabus CPU Scheduling Policies ──────────────────
enum class SchedPolicy {
    SRTF,  // Shortest Remaining Time First (Preemptive SJF)
    FCFS,  // First-Come, First-Served (Non-Preemptive FIFO)
    SJF,   // Shortest Job First (Non-Preemptive)
    RR     // Round Robin (Preemptive Time-Quantum)
};

// ─── Single Schedulable OS Task / Process Control Block (PCB) ───────────────
struct SchedTask {
    int         pid{0};              // Logical Task / Probe ID
    std::string name;                // Task label (e.g. "CPU_STAT (/proc/stat)")
    int         arrival_time{0};     // Arrival Time (AT) in microseconds
    int         burst_time{1};       // Estimated CPU Burst Time (BT) in microseconds
    int         remaining_time{1};   // Remaining Burst Time (RT) for preemptive SRTF/RR

    // Computed scheduling metrics after simulation
    int         start_time{-1};      // First dispatch timestamp (for Response Time)
    int         completion_time{0};  // Completion Time (CT)
    int         turnaround_time{0};  // Turnaround Time (TAT = CT - AT)
    int         waiting_time{0};     // Waiting Time (WT = TAT - BT)
    int         response_time{0};    // Response Time (RT = ST - AT)

    std::function<void()> work;      // Callback executed when probe is dispatched
};

// ─── Single Slice in a CPU Scheduling Gantt Chart ───────────────────────────
struct GanttSlice {
    int         pid{0};
    std::string name;
    int         start_time{0};
    int         end_time{0};
};

// ─── Aggregate Scheduling Output (Metrics, Dispatch Order & Gantt Chart) ────
struct ScheduleResult {
    SchedPolicy             policy{SchedPolicy::SRTF};
    std::vector<SchedTask>  tasks;
    std::vector<GanttSlice> gantt;
    std::vector<size_t>     dispatch_order;  // Order in which tasks are dispatched
    double                  avg_waiting_time{0.0};
    double                  avg_turnaround_time{0.0};
    double                  avg_response_time{0.0};
    int                     context_switches{0};
    int                     total_elapsed_time{0};
};

// ─── OS Task Scheduler ──────────────────────────────────────────────────────
class OsTaskScheduler {
public:
    explicit OsTaskScheduler(SchedPolicy policy = SchedPolicy::SRTF, int rr_quantum = 4, double alpha = 0.5)
        : policy_(policy), rr_quantum_(std::max(1, rr_quantum)), alpha_(std::clamp(alpha, 0.1, 0.9)) {}

    void set_policy(SchedPolicy p) { policy_ = p; }
    SchedPolicy policy() const { return policy_; }

    void set_quantum(int q) { rr_quantum_ = std::max(1, q); }
    int quantum() const { return rr_quantum_; }

    static const char* policy_name(SchedPolicy p) {
        switch (p) {
            case SchedPolicy::SRTF: return "SRTF (Shortest Remaining Time First — Preemptive)";
            case SchedPolicy::FCFS: return "FCFS (First-Come, First-Served — Non-Preemptive)";
            case SchedPolicy::SJF:  return "SJF (Shortest Job First — Non-Preemptive)";
            case SchedPolicy::RR:   return "RR (Round Robin — Preemptive Time-Quantum)";
        }
        return "SRTF";
    }

    static const char* policy_short_name(SchedPolicy p) {
        switch (p) {
            case SchedPolicy::SRTF: return "SRTF";
            case SchedPolicy::FCFS: return "FCFS";
            case SchedPolicy::SJF:  return "SJF";
            case SchedPolicy::RR:   return "RR";
        }
        return "SRTF";
    }

    static SchedPolicy parse_policy(const std::string& s) {
        std::string lower;
        lower.reserve(s.size());
        for (char c : s) lower.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
        if (lower == "fcfs" || lower == "fifo") return SchedPolicy::FCFS;
        if (lower == "sjf")                     return SchedPolicy::SJF;
        if (lower == "rr" || lower == "roundrobin") return SchedPolicy::RR;
        return SchedPolicy::SRTF;
    }

    // Textbook exponential averaging formula to predict next CPU burst:
    //   tau_{n+1} = alpha * t_n + (1 - alpha) * tau_n
    int predict_next_burst(int prev_predicted_us, int actual_measured_us) const {
        double next = alpha_ * static_cast<double>(std::max(1, actual_measured_us))
                    + (1.0 - alpha_) * static_cast<double>(std::max(1, prev_predicted_us));
        return std::max(1, static_cast<int>(std::lround(next)));
    }

    // Run the configured scheduling algorithm on a set of tasks
    ScheduleResult schedule(std::vector<SchedTask> tasks) const {
        return schedule_with(policy_, std::move(tasks), rr_quantum_);
    }

    // Run a specific scheduling policy (SRTF, FCFS, SJF, or RR) and compute
    // CT, TAT, WT, RT, dispatch order, and Gantt chart
    static ScheduleResult schedule_with(SchedPolicy policy, std::vector<SchedTask> tasks, int quantum = 4) {
        ScheduleResult res;
        res.policy = policy;
        if (tasks.empty()) return res;

        for (auto& t : tasks) {
            t.burst_time      = std::max(1, t.burst_time);
            t.remaining_time  = t.burst_time;
            t.start_time      = -1;
            t.completion_time = 0;
            t.turnaround_time = 0;
            t.waiting_time    = 0;
            t.response_time   = 0;
        }

        switch (policy) {
            case SchedPolicy::FCFS: run_fcfs(tasks, res); break;
            case SchedPolicy::SJF:  run_sjf(tasks, res);  break;
            case SchedPolicy::SRTF: run_srtf(tasks, res); break;
            case SchedPolicy::RR:   run_rr(tasks, res, std::max(1, quantum)); break;
        }

        // Calculate Turnaround Time (TAT = CT - AT),
        // Waiting Time (WT = TAT - BT), and Response Time (RT = ST - AT)
        double total_wt  = 0.0;
        double total_tat = 0.0;
        double total_rt  = 0.0;
        for (auto& t : tasks) {
            t.turnaround_time = std::max(0, t.completion_time - t.arrival_time);
            t.waiting_time    = std::max(0, t.turnaround_time - t.burst_time);
            t.response_time   = std::max(0, t.start_time - t.arrival_time);
            total_wt  += t.waiting_time;
            total_tat += t.turnaround_time;
            total_rt  += t.response_time;
        }

        const double n = static_cast<double>(tasks.size());
        res.avg_waiting_time    = total_wt / n;
        res.avg_turnaround_time = total_tat / n;
        res.avg_response_time   = total_rt / n;
        res.context_switches    = std::max(0, static_cast<int>(res.gantt.size()) - 1);
        res.total_elapsed_time  = res.gantt.empty() ? 0 : res.gantt.back().end_time;
        res.tasks               = std::move(tasks);
        return res;
    }

    // Format a Gantt chart and PCB metrics table for terminal / evaluator inspection
    static std::string format_report(const ScheduleResult& r) {
        std::ostringstream os;
        os << "  ┌─────┬────────────────────────────┬────────┬────────┬────────┬─────────┬─────────┐\n";
        os << "  │ PID │ Kernel Probe / OS Task     │ AT(us) │ BT(us) │ CT(us) │ TAT(us) │ WT(us)  │\n";
        os << "  ├─────┼────────────────────────────┼────────┼────────┼────────┼─────────┼─────────┤\n";
        for (const auto& t : r.tasks) {
            std::string label = t.name;
            if (label.size() > 26) label = label.substr(0, 26);
            os << "  │ P" << std::left << std::setw(3) << t.pid
               << "│ " << std::left << std::setw(26) << label << " "
               << "│ " << std::right << std::setw(6) << t.arrival_time << " "
               << "│ " << std::right << std::setw(6) << t.burst_time << " "
               << "│ " << std::right << std::setw(6) << t.completion_time << " "
               << "│ " << std::right << std::setw(7) << t.turnaround_time << " "
               << "│ " << std::right << std::setw(7) << t.waiting_time << " │\n";
        }
        os << "  ├─────┴────────────────────────────┴────────┴────────┴────────┴─────────┴─────────┤\n";

        std::ostringstream gantt_line;
        for (size_t i = 0; i < r.gantt.size(); ++i) {
            const auto& g = r.gantt[i];
            if (i == 0) gantt_line << g.start_time;
            gantt_line << " -> [P" << g.pid << "] -> " << g.end_time;
        }
        std::string gs = "Gantt (us): " + gantt_line.str();
        if (gs.size() > 80) gs = gs.substr(0, 77) + "...";
        os << "  │ " << std::left << std::setw(80) << gs << "│\n";

        std::ostringstream stats;
        stats << std::fixed << std::setprecision(2)
              << "Avg WT: " << r.avg_waiting_time << " us | "
              << "Avg TAT: " << r.avg_turnaround_time << " us | "
              << "Avg RT: " << r.avg_response_time << " us | "
              << "Context Switches: " << r.context_switches;
        os << "  │ " << std::left << std::setw(80) << stats.str() << "│\n";
        os << "  └─────────────────────────────────────────────────────────────────────────────────┘\n";
        return os.str();
    }

private:
    SchedPolicy policy_{SchedPolicy::SRTF};
    int         rr_quantum_{4};
    double      alpha_{0.5};

    static void append_gantt(ScheduleResult& res, int pid, const std::string& name, int start, int end) {
        if (end <= start) return;
        if (!res.gantt.empty() && res.gantt.back().pid == pid && res.gantt.back().end_time == start) {
            res.gantt.back().end_time = end;
        } else {
            res.gantt.push_back(GanttSlice{pid, name, start, end});
        }
    }

    // 1. FCFS (First-Come, First-Served): non-preemptive scheduling by arrival order
    static void run_fcfs(std::vector<SchedTask>& tasks, ScheduleResult& res) {
        std::vector<size_t> order(tasks.size());
        for (size_t i = 0; i < tasks.size(); ++i) order[i] = i;
        std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) {
            if (tasks[a].arrival_time != tasks[b].arrival_time)
                return tasks[a].arrival_time < tasks[b].arrival_time;
            return tasks[a].pid < tasks[b].pid;
        });

        int current_time = 0;
        for (size_t idx : order) {
            auto& t = tasks[idx];
            if (current_time < t.arrival_time) current_time = t.arrival_time;
            t.start_time = current_time;
            res.dispatch_order.push_back(idx);
            append_gantt(res, t.pid, t.name, current_time, current_time + t.burst_time);
            current_time += t.burst_time;
            t.remaining_time  = 0;
            t.completion_time = current_time;
        }
    }

    // 2. SJF (Shortest Job First): non-preemptive selection of shortest burst among arrived tasks
    static void run_sjf(std::vector<SchedTask>& tasks, ScheduleResult& res) {
        const size_t n = tasks.size();
        size_t completed = 0;
        int current_time = 0;

        while (completed < n) {
            size_t best = n;
            for (size_t i = 0; i < n; ++i) {
                if (tasks[i].remaining_time == 0 || tasks[i].arrival_time > current_time) continue;
                if (best == n ||
                    tasks[i].burst_time < tasks[best].burst_time ||
                    (tasks[i].burst_time == tasks[best].burst_time &&
                     tasks[i].arrival_time < tasks[best].arrival_time)) {
                    best = i;
                }
            }

            if (best == n) {
                int next_arrival = std::numeric_limits<int>::max();
                for (size_t i = 0; i < n; ++i) {
                    if (tasks[i].remaining_time > 0) {
                        next_arrival = std::min(next_arrival, tasks[i].arrival_time);
                    }
                }
                current_time = std::max(current_time + 1, next_arrival);
                continue;
            }

            auto& t = tasks[best];
            t.start_time = current_time;
            res.dispatch_order.push_back(best);
            append_gantt(res, t.pid, t.name, current_time, current_time + t.burst_time);
            current_time += t.burst_time;
            t.remaining_time  = 0;
            t.completion_time = current_time;
            ++completed;
        }
    }

    // 3. SRTF (Shortest Remaining Time First): preemptive SJF checking at every arrival/completion boundary
    static void run_srtf(std::vector<SchedTask>& tasks, ScheduleResult& res) {
        const size_t n = tasks.size();
        size_t completed = 0;
        int current_time = std::numeric_limits<int>::max();
        for (const auto& t : tasks) current_time = std::min(current_time, t.arrival_time);

        while (completed < n) {
            size_t best = n;
            for (size_t i = 0; i < n; ++i) {
                if (tasks[i].remaining_time <= 0 || tasks[i].arrival_time > current_time) continue;
                if (best == n ||
                    tasks[i].remaining_time < tasks[best].remaining_time ||
                    (tasks[i].remaining_time == tasks[best].remaining_time &&
                     tasks[i].arrival_time < tasks[best].arrival_time)) {
                    best = i;
                }
            }

            if (best == n) {
                int next_arrival = std::numeric_limits<int>::max();
                for (size_t i = 0; i < n; ++i) {
                    if (tasks[i].remaining_time > 0) {
                        next_arrival = std::min(next_arrival, tasks[i].arrival_time);
                    }
                }
                current_time = std::max(current_time + 1, next_arrival);
                continue;
            }

            auto& t = tasks[best];
            if (t.start_time < 0) {
                t.start_time = current_time;
                res.dispatch_order.push_back(best);
            }

            // Advance to either task completion or next arrival that could preempt current task
            int step = t.remaining_time;
            for (size_t i = 0; i < n; ++i) {
                if (tasks[i].remaining_time > 0 && tasks[i].arrival_time > current_time) {
                    int until_arrival = tasks[i].arrival_time - current_time;
                    if (until_arrival < step) step = until_arrival;
                }
            }
            step = std::max(1, step);

            append_gantt(res, t.pid, t.name, current_time, current_time + step);
            current_time     += step;
            t.remaining_time -= step;

            if (t.remaining_time == 0) {
                t.completion_time = current_time;
                ++completed;
            }
        }
    }

    // 4. Round Robin (RR): preemptive time-quantum scheduling with FIFO ready queue
    static void run_rr(std::vector<SchedTask>& tasks, ScheduleResult& res, int quantum) {
        const size_t n = tasks.size();
        std::vector<size_t> order(n);
        for (size_t i = 0; i < n; ++i) order[i] = i;
        std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) {
            if (tasks[a].arrival_time != tasks[b].arrival_time)
                return tasks[a].arrival_time < tasks[b].arrival_time;
            return tasks[a].pid < tasks[b].pid;
        });

        std::queue<size_t> ready_q;
        std::vector<bool> in_queue(n, false);
        size_t completed = 0;
        size_t next_idx = 0;
        int current_time = tasks[order[0]].arrival_time;

        auto enqueue_arrived = [&](int up_to_time) {
            while (next_idx < n && tasks[order[next_idx]].arrival_time <= up_to_time) {
                size_t idx = order[next_idx++];
                if (!in_queue[idx] && tasks[idx].remaining_time > 0) {
                    ready_q.push(idx);
                    in_queue[idx] = true;
                }
            }
        };

        enqueue_arrived(current_time);

        while (completed < n) {
            if (ready_q.empty()) {
                if (next_idx < n) {
                    current_time = std::max(current_time + 1, tasks[order[next_idx]].arrival_time);
                    enqueue_arrived(current_time);
                }
                continue;
            }

            size_t idx = ready_q.front();
            ready_q.pop();
            auto& t = tasks[idx];

            if (t.start_time < 0) {
                t.start_time = current_time;
                res.dispatch_order.push_back(idx);
            }

            int slice = std::min(quantum, t.remaining_time);
            append_gantt(res, t.pid, t.name, current_time, current_time + slice);
            current_time     += slice;
            t.remaining_time -= slice;

            // Arrivals during the time slice enter the ready queue before the preempted task re-queues
            enqueue_arrived(current_time);

            if (t.remaining_time == 0) {
                t.completion_time = current_time;
                ++completed;
            } else {
                ready_q.push(idx);
            }
        }
    }
};

// ─── Free Convenience Wrappers ──────────────────────────────────────────────
inline const char* sched_policy_name(SchedPolicy p) {
    return OsTaskScheduler::policy_name(p);
}

inline const char* sched_policy_short(SchedPolicy p) {
    return OsTaskScheduler::policy_short_name(p);
}

inline SchedPolicy parse_sched_policy(const std::string& s) {
    return OsTaskScheduler::parse_policy(s);
}
