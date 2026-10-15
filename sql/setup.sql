-- creates the blackbox database and separate reader/writer accounts

CREATE DATABASE IF NOT EXISTS blackbox
    CHARACTER SET utf8mb4
    COLLATE utf8mb4_unicode_ci;

-- make users for localhost socket and 127.0.0.1 tcp since mysql considers them different hosts
CREATE USER IF NOT EXISTS 'blackbox_writer'@'localhost' IDENTIFIED BY 'writer_pass';
CREATE USER IF NOT EXISTS 'blackbox_writer'@'127.0.0.1' IDENTIFIED BY 'writer_pass';
CREATE USER IF NOT EXISTS 'blackbox_reader'@'localhost' IDENTIFIED BY 'reader_pass';
CREATE USER IF NOT EXISTS 'blackbox_reader'@'127.0.0.1' IDENTIFIED BY 'reader_pass';

-- daemon needs insert and schema rights on blackbox database
GRANT ALL PRIVILEGES ON blackbox.* TO 'blackbox_writer'@'localhost';
GRANT ALL PRIVILEGES ON blackbox.* TO 'blackbox_writer'@'127.0.0.1';

-- web dashboard only needs read-only select privileges
GRANT SELECT ON blackbox.* TO 'blackbox_reader'@'localhost';
GRANT SELECT ON blackbox.* TO 'blackbox_reader'@'127.0.0.1';

-- reload grant tables to activate changes
FLUSH PRIVILEGES;

-- tables are auto-created by the daemon on first launch or via sql/schema.sql
SELECT 'System Blackbox database and users are ready.' AS status;
