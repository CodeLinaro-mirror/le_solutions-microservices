# Compiler
CC = gcc
CXX = g++

# Compiler flags
CFLAGS = -Iinclude -Wall -Werror
IDIR =
LDFLAGS =
LIBS = -lcjson

# Libraries
LIBS = -lhiredis -lcjson -lmosquitto

#Define libs for coverage
COVERAGE_LIBS = -lgcov

# AddressSanitizer flags
ASAN_FLAGS = -fsanitize=address -g

#Define coverage flag for conditional compilation with coverage
COVERAGE_FLAGS = --coverage -fprofile-arcs -ftest-coverage

#Define build folder
BUILD_DIR = build

# Source and object files
SRC = $(wildcard src/*.c)
OBJ = $(SRC:src/%.c=$(BUILD_DIR)/%.o)

# Executable name
EXEC = cloud_connect_mqtt

# Default target
all: $(EXEC)

# Link object files to create the executable
$(EXEC): $(OBJ)
	$(CC) $(OBJ) -o $@ $(IDIR) $(LDFLAGS) $(LIBS)

# Compile source files to object files
$(BUILD_DIR)/%.o: src/%.c
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS) $(IDIR) -c $< -o $@

# Clean up build files
clean:
	rm -rf $(BUILD_DIR) $(EXEC)

ifdef COVERAGE
    CXXFLAGS += -fprofile-arcs -ftest-coverage
    LDFLAGS += -fprofile-arcs -ftest-coverage
endif

# Phony targets
.PHONY: all clean

# AddressSanitizer target
sanitize: CFLAGS += $(ASAN_FLAGS)
sanitize: CXXFLAGS += $(ASAN_FLAGS)
sanitize: LDFLAGS += $(ASAN_FLAGS)
sanitize: clean all

coverage: CFLAGS += $(COVERAGE_FLAGS)
coverage: LIBS += $(COVERAGE_LIBS)
coverage: clean all
