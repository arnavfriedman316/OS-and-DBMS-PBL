# ============================================================================
# System Blackbox — Build System
# ============================================================================
# University PBL: Operating Systems + Database Management Systems
#
# Key flags explained:
#   -std=c++23    : Use the latest C++ standard (GCC 16 supports it fully).
#   -Wall -Wextra : Enable comprehensive compiler warnings.
#   -g            : Embed debug symbols (for gdb / valgrind during dev).
#   -O2           : Optimise for speed without aggressive transformations.
#   -MMD -MP      : Auto-generate header dependency files (*.d).
#   -lmariadb     : Link against MySQL / MariaDB C client library.
#   -lsqlite3     : Link against the SQLite3 shared library (fallback engine).
#   -pthread      : Enable POSIX threading (required by <thread> and <mutex>).
# ============================================================================

CXX          := g++
MYSQL_CFLAGS := $(shell mysql_config --cflags 2>/dev/null || mariadb_config --cflags 2>/dev/null || echo "-I/usr/include/mysql")
MYSQL_LIBS   := $(shell mysql_config --libs 2>/dev/null || mariadb_config --libs 2>/dev/null || echo "-lmariadb")

CXXFLAGS     := -std=c++23 -Wall -Wextra -Wpedantic -O2 -g -MMD -MP $(MYSQL_CFLAGS)
LDFLAGS      := $(MYSQL_LIBS) -lsqlite3 -pthread

TARGET       := system_blackbox
SRCS         := main.cpp db.cpp sensors.cpp
OBJS         := $(SRCS:.cpp=.o)
DEPS         := $(SRCS:.cpp=.d)

# ── Default target ──────────────────────────────────────────────────────────
all: $(TARGET)

# ── Link object files into the final binary ─────────────────────────────────
$(TARGET): $(OBJS)
	$(CXX) $(OBJS) -o $@ $(LDFLAGS)

# ── Compile each .cpp → .o ──────────────────────────────────────────────────
%.o: %.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

# Pull in the auto-generated header dependencies (ignored if they don't exist yet)
-include $(DEPS)

# ── Utility targets ─────────────────────────────────────────────────────────

# Build and start the recording daemon
run: $(TARGET)
	./$(TARGET)

# Launch the live Web UI dashboard
ui:
	@python3 server.py

serve: ui

# One-time database setup (requires sudo access to MySQL/MariaDB)
setup-db:
	@echo "[setup-db] Running sql/setup.sql into MySQL/MariaDB..."
	@if command -v mariadb >/dev/null 2>&1; then \
		sudo mariadb < sql/setup.sql; \
	elif command -v mysql >/dev/null 2>&1; then \
		sudo mysql < sql/setup.sql; \
	else \
		echo "[setup-db ERROR] Neither mariadb nor mysql CLI found."; exit 1; \
	fi

# Remove build output only — your recorded data in DB is kept
clean:
	rm -f $(OBJS) $(DEPS) $(TARGET)

# Delete SQLite fallback files if testing clean starts
cleandb:
	@read -p "Delete blackbox.db and all recorded data? [y/N] " ans; \
	if [ "$$ans" = "y" ] || [ "$$ans" = "Y" ]; then \
		rm -f blackbox.db blackbox.db-wal blackbox.db-shm; echo "[cleandb] SQLite database deleted."; \
	else echo "[cleandb] cancelled."; fi

# Create the test directory that the inotify thread watches
setup:
	mkdir -p test_watch
	@echo "[setup] test_watch/ directory ready."

.PHONY: all run ui serve clean cleandb setup setup-db
