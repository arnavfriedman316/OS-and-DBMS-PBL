-- database schema definitions for mysql and mariadb backends

-- tracks daemon lifecycles so we can detect ungraceful terminations
CREATE TABLE IF NOT EXISTS sessions (
    id BIGINT AUTO_INCREMENT PRIMARY KEY,
    start_time VARCHAR(25) NOT NULL,
    end_time VARCHAR(25) NULL,
    pid INT NOT NULL,
    hostname VARCHAR(255) NULL,
    clean_exit TINYINT NULL -- clean_exit: 1 = clean exit on SIGINT or SIGTERM, 0 = crashed, NULL = currently active
) ENGINE=InnoDB;

-- stores time-series hardware readings sampled every second with secondary indexes
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

-- forensic audit log for file operations caught by inotify
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

-- history of threshold breaches and resolutions
CREATE TABLE IF NOT EXISTS alerts (
    id BIGINT AUTO_INCREMENT PRIMARY KEY,
    timestamp VARCHAR(25) NOT NULL,
    metric VARCHAR(64) NOT NULL,
    value DOUBLE NOT NULL,
    threshold DOUBLE NOT NULL,
    state VARCHAR(32) NOT NULL,
    session_id BIGINT NULL,
    INDEX idx_alerts_ts (timestamp),
    INDEX idx_alerts_session (session_id)
) ENGINE=InnoDB;
