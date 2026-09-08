# ============================================================================
# System Blackbox — Phase 1 Build System
# ============================================================================
# University PBL: Operating Systems + Database Management Systems
#
# Key flags explained:
#   -std=c++23    : Use the latest C++ standard (GCC 16 supports it fully).
#   -Wall -Wextra : Enable comprehensive compiler warnings.
#   -g            : Embed debug symbols (for gdb / valgrind during dev).
#   -O2           : Optimise for speed without aggressive transformations.
#   -lsqlite3     : Link against the SQLite3 shared library.
#   -pthread      : Enable POSIX threading (required by <thread> and <mutex>).
# ============================================================================

CXX      := g++
CXXFLAGS := -std=c++23 -Wall -Wextra -Wpedantic -O2 -g
LDFLAGS  := -lsqlite3 -pthread

TARGET   := system_blackbox
SRCS     := main.cpp db.cpp sensors.cpp
OBJS     := $(SRCS:.cpp=.o)

# ── Default target ──────────────────────────────────────────────────────────
all: $(TARGET)

# ── Link object files into the final binary ─────────────────────────────────
$(TARGET): $(OBJS)
	$(CXX) $(OBJS) -o $@ $(LDFLAGS)

# ── Compile each .cpp → .o ──────────────────────────────────────────────────
%.o: %.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

# ── Dependency hints (header changes trigger recompilation) ──────────────────
main.o:    db.hpp sensors.hpp
db.o:      db.hpp
sensors.o: sensors.hpp

# ── Utility targets ─────────────────────────────────────────────────────────
clean:
	rm -f $(OBJS) $(TARGET) blackbox.db blackbox.db-wal blackbox.db-shm

# Create the test directory that the inotify thread will watch later
setup:
	mkdir -p test_watch
	@echo "[setup] test_watch/ directory ready."

# Launch the live Web UI dashboard
ui:
	@python3 server.py

serve: ui

.PHONY: all clean setup ui serve

