-- ============================================================================
-- setup.sql — ONE-TIME MySQL / MariaDB setup for System Blackbox
-- ============================================================================
-- Run this ONCE as the database administrator:
--
--     sudo mariadb < sql/setup.sql          (MariaDB — Arch / CachyOS / Fedora)
--     sudo mysql   < sql/setup.sql          (MySQL 8 — Ubuntu / Debian)
--
-- It creates:
--   1. the database  `blackbox`
--   2. a WRITER user (used by the C++ engine)   -> can create tables and insert
--   3. a READER user (used by the web dashboard) -> can only SELECT
--
-- DBMS CONCEPT: "Principle of least privilege" (DCL = GRANT / REVOKE).
-- The dashboard is exposed to a browser, so it gets a login that physically
-- cannot change or delete data, even if the Python code had a bug.
--
-- !! If you change these passwords, change them in blackbox.conf too !!
-- ============================================================================

CREATE DATABASE IF NOT EXISTS blackbox
    CHARACTER SET utf8mb4
    COLLATE utf8mb4_unicode_ci;

-- Users are created for both 'localhost' (Unix socket) and '127.0.0.1' (TCP)
-- because MySQL treats those as two different hosts.
CREATE USER IF NOT EXISTS 'blackbox_writer'@'localhost' IDENTIFIED BY 'writer_pass';
CREATE USER IF NOT EXISTS 'blackbox_writer'@'127.0.0.1' IDENTIFIED BY 'writer_pass';
CREATE USER IF NOT EXISTS 'blackbox_reader'@'localhost' IDENTIFIED BY 'reader_pass';
CREATE USER IF NOT EXISTS 'blackbox_reader'@'127.0.0.1' IDENTIFIED BY 'reader_pass';

-- Writer: full control, but ONLY inside the blackbox database
GRANT ALL PRIVILEGES ON blackbox.* TO 'blackbox_writer'@'localhost';
GRANT ALL PRIVILEGES ON blackbox.* TO 'blackbox_writer'@'127.0.0.1';

-- Reader: read-only
GRANT SELECT ON blackbox.* TO 'blackbox_reader'@'localhost';
GRANT SELECT ON blackbox.* TO 'blackbox_reader'@'127.0.0.1';

FLUSH PRIVILEGES;

-- The tables themselves are created automatically by the C++ engine the
-- first time it starts (see db.cpp -> DatabaseManager::create_schema).
-- sql/schema.sql contains the exact same CREATE TABLE statements for reference.
SELECT 'System Blackbox database and users are ready.' AS status;
