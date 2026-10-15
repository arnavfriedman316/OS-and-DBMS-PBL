# System Blackbox — Complete Operating Systems (OS) Architecture & Code Guide

> **PBL Course:** Operating Systems (OS) & Database Management Systems (DBMS)  
> **Topic:** Low-Level Linux Kernel Interfaces, Virtual Filesystems (`/proc` & `/sys`), Syscalls (`inotify`, `poll`), Multithreading, Signal Safety & Process Lifecycle  
> **Audience:** Written in simple, clear English so any teammate can understand every single function and easily explain it to a professor during a viva or presentation.

---

## Table of Contents
1. [The Big Picture: What is the OS Component?](#1-the-big-picture-what-is-the-os-component)
2. [Why Modern C++23 for the OS Layer?](#2-why-modern-c23-for-the-os-layer)
3. [Linux Virtual Filesystems Deep Dive (`/proc` and `/sys`)](#3-linux-virtual-filesystems-deep-dive-proc-and-sys)
   - [CPU Ticks & Jiffies (`/proc/stat`)](#31-cpu-ticks--jiffies-procstat)
   - [Virtual Memory Accounting (`/proc/meminfo`)](#32-virtual-memory-accounting-procmeminfo)
   - [Hardware Monitoring Subsystem (`/sys/class/hwmon`)](#33-hardware-monitoring-subsystem-sysclasshwmon)
4. [Kernel Inotify Subsystem (Zero-Polling File Auditing)](#4-kernel-inotify-subsystem-zero-polling-file-auditing)
   - [Why Event-Driven Syscalls Beat While Loops](#41-why-event-driven-syscalls-beat-while-loops)
   - [Syscalls Used: `inotify_init1`, `inotify_add_watch`, `poll`, `read`](#42-syscalls-used-in-detail)
   - [Cookie-Based Rename Pairing Algorithm](#43-cookie-based-rename-pairing-algorithm)
   - [Self-Healing Directory Watchdog](#44-self-healing-directory-watchdog)
5. [POSIX Multithreading & Synchronization](#5-posix-multithreading--synchronization)
   - [Thread 1 vs Thread 2 Responsibilities](#51-thread-1-vs-thread-2-responsibilities)
   - [Drift-Free Scheduling Loop](#52-drift-free-scheduling-loop)
   - [Thread Safety via `std::mutex` and RAII](#53-thread-safety-via-stdmutex-and-raii)
6. [POSIX Signal Safety & Process Lifecycle](#6-posix-signal-safety--process-lifecycle)
   - [Why `sigaction()` Over Deprecated `signal()`](#61-why-sigaction-over-deprecated-signal)
   - [Async-Signal-Safe Atomics (`std::atomic<bool>`)](#62-async-signal-safe-atomics)
   - [Process Accounting & Single-Instance Check (`kill(pid, 0)`)](#63-process-accounting--single-instance-check)
7. [Alert Engine: Sustain Counters & Hysteresis (`alerts.hpp`)](#7-alert-engine-sustain-counters--hysteresis-alertshpp)
8. [Complete File-by-File & Function-by-Function Breakdown](#8-complete-file-by-file--function-by-function-breakdown)
   - [File 1: `sensors.hpp`](#file-1-sensorshpp-hardware-telemetry-interface)
   - [File 2: `sensors.cpp`](#file-2-sensorscpp-hardware-telemetry-implementation---function-by-function)
   - [File 3: `alerts.hpp`](#file-3-alertshpp-threshold-monitor-with-hysteresis)
   - [File 4: `main.cpp` (OS Engine)](#file-4-maincpp-multithreaded-daemon--inotify-auditor)
   - [File 5: `Makefile` (Compiler & OS Flags)](#file-5-makefile-compiler-flags--posix-linking)
9. [Viva / Presentation Cheat Sheet (Questions Teammates Will Be Asked)](#9-viva--presentation-cheat-sheet-questions-teammates-will-be-asked)

---

## 1. The Big Picture: What is the OS Component?

Imagine an **airplane's electronic flight instrumentation system**:
- A flight computer doesn't guess how fast the plane is flying or wait for a user to click a button.
- It directly asks hardware probes for engine temperature, fuel pressure, and wind speed.
- It constantly monitors the cargo bay doors to alert the cockpit if a door swings open.

In our project:
- The **Operating System (OS) component** is that exact low-level flight computer.
- It runs natively on Linux as a multithreaded daemon (`system_blackbox`).
- Every second, it interrogates the Linux kernel's internal accounting files to measure **CPU usage %, RAM usage %, processor temperature (°C), and cooling fan speed (RPM)**.
- At the exact same time, it uses the Linux kernel's `inotify` subsystem to catch anyone **deleting, moving, or secretly renaming** files in a watched forensic folder (`test_watch/`).

---

## 2. Why Modern C++23 for the OS Layer?

If a professor asks: *"Why did you write the OS engine in C++ instead of Python or Node.js?"*, here are the 3 key reasons:

1. **Direct System Call Access:** High-level languages like Python run inside an interpreter with a Global Interpreter Lock (GIL). C++ compiles directly to native x86_64 machine code, giving us direct access to POSIX syscalls (`inotify_init1`, `poll`, `sigaction`, `readlink`, `kill`) with **zero overhead**.
2. **True Native Multithreading:** In C++, `std::thread` creates real kernel-level threads scheduled independently by the Linux CPU scheduler across multiple CPU cores.
3. **Deterministic Memory & RAII (Resource Acquisition Is Initialization):** When a file descriptor or database connection goes out of scope, C++ destructors close it immediately. There is no garbage collector pausing the system or delaying telemetry.

---

## 3. Linux Virtual Filesystems Deep Dive (`/proc` and `/sys`)

In Linux, there is a famous design philosophy: **"Everything is a file."**

The Linux kernel exposes its internal runtime variables and hardware drivers as **Virtual Filesystems** mounted at `/proc` and `/sys`:
- These files **do not exist on your SSD or hard drive**.
- If you open `/proc/stat`, the Linux kernel intercepts the `read()` system call and generates the text dynamically in RAM!

```
                    ┌─────────────────────────────────────────┐
                    │          Linux Kernel Space             │
                    │   • CPU Scheduler ticks (jiffies)       │
                    │   • Page Cache & Buddy Allocator memory │
                    │   • Driver telemetry (hwmon)            │
                    └────────────────────┬────────────────────┘
                                         │
                         Exposed as pseudo-filesystems:
                                         │
                    ┌────────────────────┴────────────────────┐
                    ▼                                         ▼
            /proc filesystem                          /sys filesystem
       ┌────────────────────────┐                ┌────────────────────────┐
       │ /proc/stat   (CPU)     │                │ /sys/class/hwmon/      │
       │ /proc/meminfo (RAM)    │                │ (Hardware thermal &    │
       │ /proc/<pid>/comm (PID) │                │  fan sensors)          │
       └────────────────────────┘                └────────────────────────┘
                    ▲                                         ▲
                    │                                         │
                    └────────────────────┬────────────────────┘
                                         │
                         Read by sensors.cpp via C++ streams
                                         │
                    ┌────────────────────┴────────────────────┐
                    │       C++ Engine (system_blackbox)      │
                    └─────────────────────────────────────────┘
```

---

### 3.1. CPU Ticks & Jiffies (`/proc/stat`)

Linux does not store CPU usage as a neat percentage like `25.4%`. Instead, the kernel scheduler maintains tick counters called **Jiffies** (typically 1/100th of a second).

When you inspect `/proc/stat`, the first line looks like this:
```
cpu  4705 67 1524 815997 749 180 23 0 0 0
```

These 10 numbers represent the total ticks the CPU has spent in various states since the computer booted:
1. **`user`**: Time spent running normal user-space programs.
2. **`nice`**: Time spent running low-priority user processes.
3. **`system`**: Time spent running inside the Linux kernel (handling syscalls and interrupts).
4. **`idle`**: Time spent doing completely nothing.
5. **`iowait`**: Time CPU was idle waiting for an SSD/HDD disk I/O request to complete.
6. **`irq`**: Time servicing hardware interrupts.
7. **`softirq`**: Time servicing software interrupts.
8. **`steal`**: In a virtual machine (VM), time stolen by the hypervisor for other VMs.
9. **`guest`**: Time spent running a virtual CPU for guest operating systems.
10. **`guest_nice`**: Time spent running a nice guest.

#### How we calculate CPU Usage % (The Delta Formula):
Because tick counters only go up, a single reading is meaningless. We must take **two readings across time** ($T_1$ and $T_2$):

1. **Calculate Total Idle Time:**
   $$\text{Idle Ticks} = \text{idle} + \text{iowait}$$
2. **Calculate Total CPU Time:**
   $$\text{Total Ticks} = \text{user} + \text{nice} + \text{system} + \text{idle} + \text{iowait} + \text{irq} + \text{softirq} + \text{steal}$$
   > **Important Viva Note:** In Linux kernel implementation, `guest` and `guest_nice` are already included inside `user` and `nice`. In [`sensors.cpp`](file:///home/mjfriedrice/Documents/OS%20AND%20DBMS%20PBL/sensors.cpp#L68-L72), we deliberately do not add them again to avoid double-counting virtualized CPU time!
3. **Calculate the Delta:**
   $$\Delta \text{Total} = \text{Total}_2 - \text{Total}_1$$
   $$\Delta \text{Idle} = \text{Idle}_2 - \text{Idle}_1$$
4. **Calculate Final CPU Usage %:**
   $$\text{CPU \%} = \frac{\Delta \text{Total} - \Delta \text{Idle}}{\Delta \text{Total}} \times 100$$

---

### 3.2. Virtual Memory Accounting (`/proc/meminfo`)

When beginners write memory monitors, they often make a big mistake:
```cpp
// INCORRECT NAIVE FORMULA:
RAM_Usage = (MemTotal - MemFree) / MemTotal * 100;
```
**Why is this wrong?**  
Linux follows the principle: *"Unused RAM is wasted RAM."* The Linux kernel uses almost all free memory to cache recently opened files, disk blocks, and program libraries (Page Cache and Buffers). If a user opens a new heavy app, the kernel instantly frees up this cache!

Therefore, `MemFree` is often tiny (e.g. only 400 MB on a 16 GB laptop), even though the computer has 12 GB available!

#### The Proper OS Formula used in [`sensors.cpp`](file:///home/mjfriedrice/Documents/OS%20AND%20DBMS%20PBL/sensors.cpp#L108-L157):
```
MemTotal:       16142340 kB
MemFree:          845120 kB
MemAvailable:   11204850 kB
```
- **`MemAvailable`** was introduced in Linux Kernel 3.14. It is the kernel's own estimate of how much memory can be allocated without causing the system to swap.
- Our code uses:
  $$\text{Used Memory} = \text{MemTotal} - \text{MemAvailable}$$
  $$\text{RAM \%} = \frac{\text{MemTotal} - \text{MemAvailable}}{\text{MemTotal}} \times 100$$
- If running on an ancient kernel where `MemAvailable` is absent, our code falls back to:
  $$\text{MemAvailable} \approx \text{MemFree} + \text{Buffers} + \text{Cached}$$

---

### 3.3. Hardware Monitoring Subsystem (`/sys/class/hwmon`)

Hardware sensors are exposed through the kernel's `sysfs` tree under `/sys/class/hwmon/hwmon[0..N]/`.

#### The Detection Challenge:
On modern laptops and desktops, `/sys/class/hwmon` contains dozens of sensors:
- `hwmon0`: NVMe SSD controller temp
- `hwmon1`: Realtek Wi-Fi card temp
- `hwmon2`: NVIDIA GPU temp
- `hwmon3`: Intel CPU (`coretemp`) or AMD CPU (`k10temp`)

If our code blindly read the first sensor, it might report that your CPU is 28°C when your actual processor is boiling at 95°C!

#### Our Autodetection Algorithm ([`sensors.cpp:165–208`](file:///home/mjfriedrice/Documents/OS%20AND%20DBMS%20PBL/sensors.cpp#L165-L208)):
1. **Driver Identification:** It iterates through `/sys/class/hwmon/hwmon*` reading the `name` file, specifically searching for recognized CPU driver names:
   - `coretemp` (Intel x86 CPUs)
   - `k10temp` or `zenpower` (AMD Ryzen CPUs)
   - `cpu_thermal` (ARM / Raspberry Pi)
2. **Package Label Matching:** Inside the matched driver directory, it looks for labels matching `Package id 0`, `Tctl`, or `Tdie` (which report the overall processor package temperature rather than individual cores).
3. **Thermal Zone Fallback:** If `hwmon` lacks CPU drivers, it scans `/sys/class/thermal/thermal_zone*/type` looking for `x86_pkg_temp` or `cpu-thermal`.
4. **Millidegree Conversion:** Kernel drivers store temperatures in **millidegrees Celsius** (integers without decimal points to avoid floating-point math inside kernel space). A reading of `54000` means $54.0^\circ\text{C}$. Our code divides by `1000.0`.
5. **Fan RPM:** It scans for `fan1_input`, `fan2_input` and reports the fastest spinning fan in Revolutions Per Minute.

---

## 4. Kernel Inotify Subsystem (Zero-Polling File Auditing)

How does a system monitor detect if a file is deleted or renamed?

### 4.1. Why Event-Driven Syscalls Beat While Loops

#### The Bad Way: Polling (Busy-Waiting)
```cpp
// NAIVE POLLING: Burns 100% CPU and misses fast events
while (true) {
    scan_entire_directory(); // High disk I/O!
    sleep(1);                // Misses a file created and deleted in 200ms!
}
```

#### The Good Way: Kernel `inotify` (Event-Driven)
The Linux kernel includes an event subsystem called **`inotify`** (Inode Notify).
- We tell the kernel: *"Please watch directory `test_watch/` for deletions and moves."*
- Our thread immediately goes to sleep (`blocked` state). **It consumes 0.0% CPU!**
- The moment a process executes an `unlink()` or `rename()` syscall inside `test_watch/`, the Linux Virtual File System (VFS) wakes our thread up and delivers an event struct.

---

### 4.2. Syscalls Used in Detail

[`main.cpp:344–469`](file:///home/mjfriedrice/Documents/OS%20AND%20DBMS%20PBL/main.cpp#L344-L469) implements the inotify worker using 4 fundamental POSIX syscalls:

1. **`inotify_init1(IN_NONBLOCK | IN_CLOEXEC)`**:
   - Creates a kernel inotify instance and returns a file descriptor (`inotify_fd`).
   - `IN_NONBLOCK`: File descriptor is non-blocking so `read()` won't hang indefinitely if empty.
   - `IN_CLOEXEC`: Close-on-exec flag. If our process ever spawns a child process via `fork()`/`exec()`, the kernel automatically closes this file descriptor so the child doesn't inherit it.
2. **`inotify_add_watch(inotify_fd, watch_dir, mask)`**:
   - Registers a directory with the kernel.
   - Mask used:
     - `IN_DELETE`: File or folder deleted.
     - `IN_MOVED_FROM`: File moved away or renamed from this name.
     - `IN_MOVED_TO`: File moved into folder or arrived as the new name in a rename.
     - `IN_DELETE_SELF` & `IN_MOVE_SELF`: The watched folder itself was deleted or moved!
3. **`poll(&pfd, 1, 200)` (I/O Multiplexing)**:
   - Suspends the thread and waits up to 200 milliseconds for data to become readable on `inotify_fd`.
   - By timing out every 200ms, the thread wakes up cleanly to check if the user pressed `Ctrl+C` (`g_running == false`), allowing instant, lag-free termination.
4. **`read(inotify_fd, buffer, BUF_LEN)`**:
   - Drains raw `struct inotify_event` records from the kernel into our aligned buffer.

---

### 4.3. Cookie-Based Rename Pairing Algorithm

When a user renames a file (e.g. `mv test_watch/a.txt test_watch/b.txt`), what does the Linux kernel do?

The Linux kernel does **not** have an `IN_RENAME` event. Instead, it generates two separate events:
1. `IN_MOVED_FROM` (for `a.txt`)
2. `IN_MOVED_TO` (for `b.txt`)

**How does our program know whether `a.txt` was renamed to `b.txt`, or if `a.txt` was stolen and moved out to `/tmp/`?**

The Linux kernel gives both events the **exact same 32-bit `cookie` integer**!

```
       Step 1: Kernel generates IN_MOVED_FROM (cookie = 4821, name = "secret.txt")
               │
               ▼
       Our C++ engine stores it in std::unordered_map:
       pending_moves[4821] = { name: "secret.txt", time: NOW }
               │
               ├──────────────────────────────────────────┐
               │                                          │
       Within 300ms:                              After 300ms:
       IN_MOVED_TO arrives                        No IN_MOVED_TO arrives
       (cookie = 4821, name = "new.txt")          (Cookie timed out)
               │                                          │
               ▼                                          ▼
       MATCH FOUND!                               NO MATCH!
       It was a RENAMED event!                    It was a MOVED_OUT event!
       "secret.txt" -> "new.txt"                  File moved outside folder!
```

---

### 4.4. Self-Healing Directory Watchdog

What happens if an intruder tries to break our audit system by deleting the entire `test_watch/` directory?
1. The kernel fires `IN_DELETE_SELF` or `IN_MOVE_SELF`.
2. Our code detects this:
   ```cpp
   record(watch_dir, "WATCH_DIR_REMOVED", true);
   watch_desc = -1;
   ```
3. Instead of crashing, the worker thread enters a self-healing loop: it automatically calls `fs::create_directories(watch_dir)` and re-arms the watch using `inotify_add_watch()`! The audit trail never stops.

---

## 5. POSIX Multithreading & Synchronization

### 5.1. Thread 1 vs Thread 2 Responsibilities

System Blackbox runs two concurrent POSIX worker threads managed via C++23 `std::thread`:

| Feature | Thread 1: Hardware Telemetry Worker | Thread 2: Kernel Inotify Auditor |
| :--- | :--- | :--- |
| **Source File** | [`main.cpp:252–327`](file:///home/mjfriedrice/Documents/OS%20AND%20DBMS%20PBL/main.cpp#L252-L327) | [`main.cpp:344–469`](file:///home/mjfriedrice/Documents/OS%20AND%20DBMS%20PBL/main.cpp#L344-L469) |
| **Data Source** | Linux `/proc` and `/sys` virtual files | Linux kernel `inotify` subsystem |
| **Execution Style** | Periodic polling (every 1 second) | Event-driven (woken up by kernel on file change) |
| **Database Target** | `vitals_log` & `alerts` tables | `deleted_files` table |
| **CPU Overhead** | Extremely low (< 0.2%) | Zero until a file event happens |

---

### 5.2. Drift-Free Scheduling Loop

Most beginner programs do this for periodic loops:
```cpp
// NAIVE SLEEP LOOP (Suffers from Clock Drift):
while (running) {
    read_sensors(); // takes 5 milliseconds
    sleep(1);       // sleeps 1000 milliseconds
} // Total loop cycle = 1005 ms. After 200 seconds, you have lost a full second!
```

#### Our Drift-Free Implementation ([`main.cpp:266–284`](file:///home/mjfriedrice/Documents/OS%20AND%20DBMS%20PBL/main.cpp#L266-L284)):
```cpp
const auto interval = 1s;
auto next_tick = std::chrono::steady_clock::now() + interval;

while (g_running.load()) {
    // 1. Sleep in 100ms slices until next_tick is reached
    while (g_running.load() && std::chrono::steady_clock::now() < next_tick) {
        auto left = next_tick - std::chrono::steady_clock::now();
        std::this_thread::sleep_for(std::min(left, 100ms));
    }
    
    // 2. Advance next_tick from the PREVIOUS tick, NOT from now()!
    next_tick += interval;
    
    read_and_log_sensors();
}
```
**Why this matters:** Even if disk writes take 20ms on one cycle, the next sample is still scheduled at the exact planned second. Telemetry never drifts!

---

### 5.3. Thread Safety via `std::mutex` and RAII

Because Thread 1 and Thread 2 run simultaneously:
1. **Console Output:** Both threads print logs. Without protection, characters from Thread 1 and Thread 2 would interleave and create garbled terminal text. We use `std::mutex g_out_mutex` inside `say()`.
2. **Database Socket:** Both threads write to the database. We use `std::mutex db_mutex_` inside `DatabaseManager`.
3. **RAII Lock Guards:** We lock mutexes using `std::lock_guard<std::mutex> lock(m)`. When the function finishes, C++ **automatically releases the lock**, even if an exception or early return occurs.

---

## 6. POSIX Signal Safety & Process Lifecycle

### 6.1. Why `sigaction()` Over Deprecated `signal()`

In C and C++, many developers use `signal(SIGINT, handler)`. In modern OS engineering, `signal()` is deprecated because:
- Its behavior varies wildly between Linux, BSD, and UNIX.
- On some systems, the signal handler is reset to default after the first signal.

We use the POSIX standard **`sigaction()`** system call ([`main.cpp:489–494`](file:///home/mjfriedrice/Documents/OS%20AND%20DBMS%20PBL/main.cpp#L489-L494)):
```cpp
struct sigaction sa{};
sa.sa_handler = signal_handler;
sigemptyset(&sa.sa_mask);
sigaction(SIGINT,  &sa, nullptr); // Catch Ctrl+C
sigaction(SIGTERM, &sa, nullptr); // Catch kill command
```
Notice we **do NOT set `SA_RESTART`**: This allows blocking system calls like `poll()` in Thread 2 to wake up immediately with error `EINTR`, allowing the thread to exit cleanly within milliseconds of pressing `Ctrl+C`.

---

### 6.2. Async-Signal-Safe Atomics (`std::atomic<bool>`)

**Critical OS Concept:** A signal can interrupt CPU execution at **literally any instruction** — even in the middle of a `printf()` or `malloc()` call. If your signal handler also calls `printf()`, it can cause a dead lock or heap corruption!

Therefore, our signal handler does only one thing:
```cpp
static void signal_handler(int signum) {
    g_signal.store(signum);
    g_running.store(false); // Lock-free atomic write
}
```
`std::atomic<bool>` uses CPU memory fences and atomic test-and-set instructions, making it 100% **Async-Signal-Safe**.

---

### 6.3. Process Accounting & Single-Instance Check

How do we make sure two instances of `system_blackbox` don't record to the database at the same time?

#### The Magic Syscall: `kill(pid, 0)` ([`main.cpp:234–241`](file:///home/mjfriedrice/Documents/OS%20AND%20DBMS%20PBL/main.cpp#L234-L241)):
- In Linux, calling `kill(pid, 0)` does **not** terminate the process! Signal `0` is a special probe signal.
- The kernel only checks: *"Does this process ID exist, and do we have permission to see it?"*
- If `kill(pid, 0) == 0`, the process is alive.

#### Handling PID Recycling:
Linux reuses PIDs when old programs terminate. What if an old `system_blackbox` crashed, and its PID was later reassigned to Spotify or Chrome?
- Our code doesn't just check `kill(pid, 0)`.
- It reads the Linux kernel file **`/proc/<pid>/comm`**, which contains the executable name.
- Only if the name is literally `"system_blackbox"` does it confirm that another instance is running!

---

## 7. Alert Engine: Sustain Counters & Hysteresis (`alerts.hpp`)

Why does [`alerts.hpp`](file:///home/mjfriedrice/Documents/OS%20AND%20DBMS%20PBL/alerts.hpp) exist?

If you set an alert at `90% CPU`:
- When opening a browser, the CPU spikes to `92%` for 100ms, then drops back to `4%`. This is normal OS behavior, not a server emergency.
- If CPU usage hovers at `89.9% -> 90.1% -> 89.9% -> 90.1%`, a naive system sends 20 email alerts in 20 seconds (**Alert Flapping / Alert Fatigue**).

```
   CPU %
    100 │                 ╭───╮
        │                 │   │   <- Spike for 1 sample (IGNORED by Sustain)
     90 ┼── TRIGGER ──────┼───┼───────────╭─────────╮────────────────────────
        │   THRESHOLD     │   │           │         │
        │                 │   │           │         │
     80 ┼── RESOLVE ──────┼───┼───────────┼─────────┼───────────╭────────────
        │   THRESHOLD     │   │           │         ╰───────────╯
        │   (Margin = 10%)│   │           │          (Keeps TRIGGERED state
        │                 │   │           │           until dropping below 80%)
      0 └─────────────────┴───┴───────────┴──────────────────────────────────
                                          ^ Sustained >= 90% for 3 samples
                                            -> ALERT TRIGGERED!
```

#### Our Dual Solution:
1. **Sustain Counter (`sustain = 3`):** CPU must stay $\ge 90\%$ for **3 consecutive samples** before firing `TRIGGERED`.
2. **Hysteresis Margin (`margin = 10.0`):** Once triggered, the alert does **not** resolve at 89.9%. The temperature/CPU must drop all the way below $90.0 - 10.0 = \mathbf{80.0\%}$ to fire `RESOLVED`.

---

## 8. Complete File-by-File & Function-by-Function Breakdown

---

### File 1: `sensors.hpp` (Hardware Telemetry Interface)
[`sensors.hpp`](file:///home/mjfriedrice/Documents/OS%20AND%20DBMS%20PBL/sensors.hpp) declares the data structures and class interface for reading Linux hardware metrics.

- **`struct SystemVitals`**: Plain Old Data (POD) struct containing 4 fields:
  - `double cpu_usage`: CPU utilization percentage (0.0 to 100.0).
  - `double ram_usage`: RAM utilization percentage (0.0 to 100.0).
  - `double cpu_temp`: Processor temperature in degrees Celsius.
  - `int fan_speed`: Cooling fan speed in RPM.
- **`class HardwareSensorReader`**:
  - Encapsulates baseline tick counters and sensor file paths.

---

### File 2: `sensors.cpp` (Hardware Telemetry Implementation - Function by Function)
[`sensors.cpp`](file:///home/mjfriedrice/Documents/OS%20AND%20DBMS%20PBL/sensors.cpp) contains the low-level parsing routines for `/proc` and `/sys`.

#### 1. `read_line(const fs::path& p)` (Lines 22–27)
- Opens a sysfs file using `std::ifstream`.
- Reads the first line, strips trailing newline characters, and returns it as a string.

#### 2. `read_millideg(const fs::path& p)` (Lines 30–36)
- Reads an integer value from a sysfs temperature file (e.g. `53400`).
- Divides by `1000.0` to convert to degrees Celsius (`53.4°C`).
- Validates that the reading is between $0^\circ\text{C}$ and $125^\circ\text{C}$ (filtering out disconnected sensor errors).

#### 3. `HardwareSensorReader::HardwareSensorReader()` (Lines 39–42)
- Constructor called at program startup.
- Takes an initial reading of `/proc/stat` ticks to establish a baseline.
- Calls `detect_cpu_temp_sensor()` to discover which sysfs file belongs to the real CPU.

#### 4. `read_proc_stat_ticks(idle_ticks, total_ticks)` (Lines 46–76)
- Opens the virtual file `/proc/stat`.
- Reads the first line starting with `"cpu"`.
- Extracts the 10 tick counters: `user`, `nice`, `system`, `idle`, `iowait`, `irq`, `softirq`, `steal`, `guest`, `guest_nice`.
- Sets `idle_ticks = idle + iowait`.
- Computes `total_ticks` as the sum of all primary counters (excluding `guest`/`guest_nice` to prevent double-counting).
- Returns `true` on success.

#### 5. `read_cpu_usage()` (Lines 80–104)
- Calls `read_proc_stat_ticks()` to get current ticks.
- Computes $\Delta \text{total} = \text{current\_total} - \text{prev\_total}$ and $\Delta \text{idle} = \text{current\_idle} - \text{prev\_idle}$.
- Updates stored baseline values for the next call.
- Applies formula: $\text{usage} = 100.0 \times \frac{\Delta \text{total} - \Delta \text{idle}}{\Delta \text{total}}$.
- Clamps the result between $0.0$ and $100.0$ and rounds to 1 decimal place.

#### 6. `read_ram_usage()` (Lines 108–157)
- Opens `/proc/meminfo`.
- Loops through lines searching for `MemTotal:`, `MemAvailable:`, `MemFree:`, `Buffers:`, and `Cached:`.
- Breaks early as soon as `MemTotal` and `MemAvailable` are found.
- If `MemAvailable` is missing (ancient kernels), calculates fallback: `MemFree + Buffers + Cached`.
- Computes percentage: $\frac{\text{MemTotal} - \text{MemAvailable}}{\text{MemTotal}} \times 100$.

#### 7. `detect_cpu_temp_sensor()` (Lines 165–208)
- Iterates over `/sys/class/hwmon/hwmon*`.
- Checks the `name` file for drivers: `coretemp` (Intel), `k10temp`/`zenpower` (AMD), `cpu_thermal` (ARM).
- Checks labels `Package id 0`, `Tctl`, `Tdie`.
- If found, saves the path into `temp_path_` and description into `temp_source_`.
- If no driver matches, checks `/sys/class/thermal/thermal_zone*/type`.
- If all fails, sets fallback mode to read the hottest sensor found.

#### 8. `read_cpu_temp()` (Lines 211–217)
- Reads the cached `temp_path_` using `read_millideg()`.
- If invalid or empty, delegates to `read_max_any_temp()`.

#### 9. `read_max_any_temp()` (Lines 220–239)
- Universal fallback: scans all `temp*_input` files in `/sys/class/hwmon/` and all `thermal_zone*` directories.
- Returns the highest recorded temperature.

#### 10. `read_fan_speed()` (Lines 244–262)
- Scans `/sys/class/hwmon/hwmon*/fan*_input`.
- Reads RPM values from all fans and returns the maximum RPM found.
- Returns `0` if the device is passively cooled (e.g. tablet or fanless VM).

#### 11. `read_all()` (Lines 266–273)
- Aggregates all 4 sensor readings (`cpu_usage`, `ram_usage`, `cpu_temp`, `fan_speed`) into a single `SystemVitals` struct.

---

### File 3: `alerts.hpp` (Threshold Monitor with Hysteresis)
[`alerts.hpp`](file:///home/mjfriedrice/Documents/OS%20AND%20DBMS%20PBL/alerts.hpp) implements stateful threshold tracking.

- **`update(double value)`**:
  - If alert is currently inactive: checks if `value >= threshold_`. If true, increments `above_count_`. Once `above_count_ >= sustain_` (3 samples), transitions to active and returns `Change::Triggered`.
  - If alert is currently active: checks if `value < threshold_ - margin_`. If true, resets active to false and returns `Change::Resolved`.
  - Otherwise returns `Change::None`.

---

### File 4: `main.cpp` (Multithreaded Daemon & Inotify Auditor)
[`main.cpp`](file:///home/mjfriedrice/Documents/OS%20AND%20DBMS%20PBL/main.cpp) orchestrates OS resources, worker threads, and signal handling.

#### 1. `say(const string& line, bool to_stderr)` (Lines 59–62)
- Thread-safe console logging wrapper protected by `std::mutex g_out_mutex`.

#### 2. `signal_handler(int signum)` (Lines 67–70)
- POSIX async-signal-safe callback. Atomically sets `g_running = false` and records `g_signal`.

#### 3. `exe_dir()` (Lines 117–123)
- Uses the Linux system call `readlink("/proc/self/exe", buf, sizeof(buf))` to discover the absolute directory where the current binary is located.

#### 4. `process_is_blackbox(int pid)` (Lines 234–241)
- Calls `kill(pid, 0)` to check process existence.
- Reads `/proc/<pid>/comm` to verify if the executable name is `"system_blackbox"`.

#### 5. `sensor_worker(db, reader, cfg)` (Lines 252–327)
- **Thread 1 body:** Executes the drift-free 1-second telemetry loop.
- Calls `reader.read_all()`, logs to database, evaluates `ThresholdMonitor` alerts, and invokes hourly retention purging.

#### 6. `inotify_worker(db, watch_dir)` (Lines 344–469)
- **Thread 2 body:** Initializes `inotify_init1()`, arms directory watch, and enters `poll()` event loop.
- Parses variable-length `inotify_event` buffers.
- Tracks rename cookies in `pending_moves` hashmap to differentiate internal renames from external moves.
- Self-heals the watch if the target directory is deleted.

#### 7. `main(argc, argv)` (Lines 474–576)
- Parses CLI arguments and `blackbox.conf`.
- Installs `sigaction` signal handlers for `SIGINT` and `SIGTERM`.
- Initializes database and performs crash forensic recovery.
- Spawns `sensor_worker` and `inotify_worker` via `std::thread`.
- Calls `.join()` to wait for threads on shutdown.
- Marks session cleanly ended and closes database handles.

---

### File 5: `Makefile` (Compiler Flags & POSIX Linking)
[`Makefile`](file:///home/mjfriedrice/Documents/OS%20AND%20DBMS%20PBL/Makefile) drives the native build system:

- **`-std=c++23`**: Enables modern C++ features (e.g. `std::string_view`, `<filesystem>`, extended constexpr).
- **`-pthread`**: Configures compiler and linker for POSIX threads (sets preprocessor definitions and links `libpthread`).
- **`-Wall -Wextra -Wpedantic`**: Enforces strict compiler warnings for safety.
- **`-O2`**: Enables GCC level-2 compiler optimizations.
- **`-MMD -MP`**: Auto-generates header dependency files (`*.d`) so recompilation happens only when modified headers change.

---

## 9. Viva / Presentation Cheat Sheet (Questions Teammates Will Be Asked)

| Viva Question | Simple, Impressive Answer to Give |
| :--- | :--- |
| **"What are `/proc` and `/sys`? Where are they stored on disk?"** | They are Virtual Filesystems (pseudo-filesystems) generated dynamically in RAM by the Linux kernel. They do not exist on physical storage. When our C++ code reads `/proc/stat`, the kernel intercepts the read syscall and populates CPU tick counters directly from kernel memory. |
| **"Why is `MemFree` misleading in Linux?"** | Linux uses unused RAM for disk buffers and page cache to speed up the system. Even if `MemFree` is low, that cached memory is instantly reclaimable. We use `MemAvailable` from `/proc/meminfo`, which reflects the actual memory available for new processes. |
| **"How did you calculate CPU % from tick counters?"** | CPU utilization cannot be measured in a single instant. We record total ticks and idle ticks from `/proc/stat`, wait an interval, read them again, and calculate the delta: $(\Delta \text{Total} - \Delta \text{Idle}) / \Delta \text{Total} \times 100$. |
| **"What is `inotify` and why is it better than a while loop?"** | A polling loop continuously scans the directory, consuming high CPU cycles and potentially missing quick create-and-delete events. `inotify` is an event-driven Linux kernel mechanism where our thread sleeps with 0% CPU until the kernel's Virtual Filesystem notifies us of a file change. |
| **"How do you tell the difference between a rename and a move out?"** | The Linux kernel pairs internal directory renames by assigning the same 32-bit `cookie` integer to both `IN_MOVED_FROM` and `IN_MOVED_TO`. We store moved-from events in a hashmap; if a moved-to event arrives with the same cookie within 300ms, it was a rename; otherwise, it was moved outside. |
| **"Why did you use `sigaction` instead of `signal`?"** | `signal()` is deprecated, non-portable, and resets on some UNIX systems. `sigaction()` provides well-defined POSIX behavior and allows us to omit `SA_RESTART`, ensuring blocking syscalls like `poll()` wake up immediately when `Ctrl+C` is pressed. |
| **"What is Async-Signal Safety?"** | When a signal interrupts a process, it can pause execution inside non-reentrant functions like `malloc` or `printf`. Calling them inside a signal handler causes deadlocks. Our signal handler is 100% async-signal-safe because it only writes to atomic variables (`std::atomic<bool>`). |
| **"What does `kill(pid, 0)` do?"** | Signal `0` sends no actual signal to the target process; it only performs an error check to verify whether that process ID is currently alive in the operating system. |
| **"What is Hysteresis in your alert engine?"** | It prevents alert flapping. If a threshold is 90%, the alert triggers at 90%, but only resolves when the value drops well below 80% (with a 10% margin). This prevents spamming when a metric hovers right around the boundary. |
