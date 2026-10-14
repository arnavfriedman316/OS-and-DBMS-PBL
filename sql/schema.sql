-- ============================================================================
-- schema.sql — System Blackbox Table Definitions (DDL)
-- ============================================================================
-- Database: blackbox (MySQL / MariaDB & PostgreSQL compatible)
--
-- Tables:
--   1. sessions      : Tracks each run of the C++ daemon (PID, start/end, crash detection)
--   2. vitals_log    : Time-series CPU %, RAM %, Core Temp °C, Fan RPM
--   3. deleted_files : Forensic inotify audit trail (deletions, renames, moves)
--   4. alerts        : Threshold alert state changes (TRIGGERED / RESOLVED with hysteresis)
-- ============================================================================

-- Table 1: Sessions (Used for process crash detection)
CREATE TABLE IF NOT EXISTS sessions (
    id BIGINT AUTO_INCREMENT PRIMARY KEY,
    start_time VARCHAR(25) NOT NULL,
    end_time VARCHAR(25) NULL,
    pid INT NOT NULL,
    hostname VARCHAR(255) NULL,
    clean_exit TINYINT NULL -- 1 = clean shutdown (SIGINT/SIGTERM), 0 = crash/kill -9, NULL = running
) ENGINE=InnoDB;

-- Table 2: System Vitals (Time-series telemetry sampled every second)
CREATE TABLE IF NOT EXISTS vitals_log (
    id BIGINT AUTO_INCREMENT PRIMARY KEY,
    timestamp VARCHAR(25) NOT NULL,
    cpu_usage DOUBLE NOT NULL,
    ram_usage DOUBLE NOT NULL,
    cpu_temp DOUBLE DEFAULT 0.0,
    fan_speed INT DEFAULT 0,
    session_id BIGINT NULL,
    INDEX idx_vitals_ts (timestamp),
    INDEX idx_vitals_session (session_id)
) ENGINE=InnoDB;

-- Table 3: Forensic File Auditing (Kernel Inotify events)
CREATE TABLE IF NOT EXISTS deleted_files (
    id BIGINT AUTO_INCREMENT PRIMARY KEY,
    timestamp VARCHAR(25) NOT NULL,
    file_name VARCHAR(512) NOT NULL,
    event_type VARCHAR(32) DEFAULT 'DELETED',
    is_dir TINYINT DEFAULT 0,
    new_name VARCHAR(512) NULL,
    session_id BIGINT NULL,
    INDEX idx_files_ts (timestamp),
    INDEX idx_files_session (session_id)
) ENGINE=InnoDB;

-- Table 4: Threshold Alerts (Hysteresis monitoring log)
CREATE TABLE IF NOT EXISTS alerts (
    id BIGINT AUTO_INCREMENT PRIMARY KEY,
    timestamp VARCHAR(25) NOT NULL,
    metric VARCHAR(64) NOT NULL,
    value DOUBLE NOT NULL,
    threshold DOUBLE NOT NULL,
    state VARCHAR(32) NOT NULL, -- 'TRIGGERED' or 'RESOLVED'
    session_id BIGINT NULL,
    INDEX idx_alerts_ts (timestamp),
    INDEX idx_alerts_session (session_id)
) ENGINE=InnoDB;
