# build recipe for the c++ daemon linking against mariadb and sqlite3

CXX          := g++
MYSQL_CFLAGS := $(shell mysql_config --cflags 2>/dev/null || mariadb_config --cflags 2>/dev/null || echo "-I/usr/include/mysql")
MYSQL_LIBS   := $(shell mysql_config --libs 2>/dev/null || mariadb_config --libs 2>/dev/null || echo "-lmariadb")

CXXFLAGS     := -std=c++23 -Wall -Wextra -Wpedantic -O2 -g -MMD -MP $(MYSQL_CFLAGS)
LDFLAGS      := $(MYSQL_LIBS) -lsqlite3 -pthread

TARGET       := system_blackbox
SRCS         := main.cpp db.cpp sensors.cpp
OBJS         := $(SRCS:.cpp=.o)
DEPS         := $(SRCS:.cpp=.d)

# default build target
all: $(TARGET)

# link compiled objects into executable
$(TARGET): $(OBJS)
	$(CXX) $(OBJS) -o $@ $(LDFLAGS)

# compile translation units with dependency tracking flags
%.o: %.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

# include auto-generated header dependency rules
-include $(DEPS)

# build and run daemon in the terminal
run: $(TARGET)
	./$(TARGET)

# launch python web dashboard
ui:
	@python3 server.py

serve: ui

# execute setup sql into mariadb or mysql cli
setup-db:
	@echo "[setup-db] Running sql/setup.sql into MySQL/MariaDB..."
	@if command -v mariadb >/dev/null 2>&1; then \
		sudo mariadb < sql/setup.sql; \
	elif command -v mysql >/dev/null 2>&1; then \
		sudo mysql < sql/setup.sql; \
	else \
		echo "[setup-db ERROR] Neither mariadb nor mysql CLI found."; exit 1; \
	fi

# remove compiled objects and binary
clean:
	rm -f $(OBJS) $(DEPS) $(TARGET)

# purge local sqlite database files for a clean slate
cleandb:
	@read -p "Delete blackbox.db and all recorded data? [y/N] " ans; \
	if [ "$$ans" = "y" ] || [ "$$ans" = "Y" ]; then \
		rm -f blackbox.db blackbox.db-wal blackbox.db-shm; echo "[cleandb] SQLite database deleted."; \
	else echo "[cleandb] cancelled."; fi

# create watch directory monitored by inotify
setup:
	mkdir -p test_watch
	@echo "[setup] test_watch/ directory ready."

.PHONY: all run ui serve clean cleandb setup setup-db
