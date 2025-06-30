# Compiler settings
CXX = g++
CXXFLAGS = -I../include -Wall -Werror -std=c++11 -D_GNU_SOURCE
LDFLAGS = -lCppUTest -lgcov -lcjson

#Define coverage flag for conditional compilation with coverage
COVERAGE_FLAGS = --coverage -fprofile-arcs -ftest-coverage

# AddressSanitizer flags
ASAN_FLAGS = -g -fsanitize=address

# Directories
SRC_DIR = ../src
TEST_DIR = .
INCLUDE_DIR = ../include
BUILD_DIR = ../build_ut
CONFIG_FILE_NAME = config.json
CONFIG_FILE = ../$(CONFIG_FILE_NAME)

# Source files
C_SOURCES = $(filter-out $(SRC_DIR)/cloud_connect_mqtt_main.c, $(wildcard $(SRC_DIR)/*.c)) $(wildcard $(TEST_DIR)/*.c)
#C_SOURCES = $(wildcard $(SRC_DIR)/*.c)  $(wildcard $(TEST_DIR)/*.c)
CPP_SOURCES = $(wildcard $(TEST_DIR)/*.cpp)
#OBJECTS = $(filter-out $(BUILD_DIR)/cloud_connect_kafka_main.o, $(C_SOURCES:$(SRC_DIR)/%.c=$(BUILD_DIR)/%.o))
#TEST_OBJECTS = $(CPP_SOURCES:$(TEST_DIR)/%.cpp=$(BUILD_DIR)/%.o)

OBJECTS = $(patsubst $(SRC_DIR)/%.c, $(BUILD_DIR)/%.o, $(filter $(SRC_DIR)/%.c, $(C_SOURCES))) \
            $(patsubst $(TEST_DIR)/%.c, $(BUILD_DIR)/%.o, $(filter $(TEST_DIR)/%.c, $(C_SOURCES)))
TEST_OBJECTS = $(patsubst $(TEST_DIR)/%.cpp, $(BUILD_DIR)/%.o, $(CPP_SOURCES))


# Target
TEST_TARGET = run_tests

#Check if coverage is set to 1
ifdef COVERAGE
	CPPFLAGS += $(COVERAGE_FLAGS)
	CFLAGS += $(COVERAGE_FLAGS)
	CXXFLAGS += $(COVERAGE_FLAGS)
	LDFLAGS += $(COVERAGE_FLAGS)
endif

# Rules
all: $(TEST_TARGET) copy_config

$(BUILD_DIR)/%.o: $(SRC_DIR)/%.c
	@mkdir -p $(BUILD_DIR)
	$(CC) -I$(INCLUDE_DIR) $(CFLAGS) -DUNIT_TEST -D_GNU_SOURCE -Wall -Werror -std=c11 -c $< -o $@

$(BUILD_DIR)/%.o: $(TEST_DIR)/%.c
	@mkdir -p $(BUILD_DIR)
	$(CC) -I$(INCLUDE_DIR) $(CFLAGS) -DUNIT_TEST -D_GNU_SOURCE -Wall -Werror -std=c11 -c $< -o $@

$(BUILD_DIR)/%.o: $(TEST_DIR)/%.cpp
	@mkdir -p $(BUILD_DIR)
	$(CXX) $(CPPFLAGS) -DUNIT_TEST -c $< -o $@

$(TEST_TARGET): $(TEST_OBJECTS) $(OBJECTS)
	$(CXX) $(TEST_OBJECTS) $(OBJECTS) -o $@ $(LDFLAGS)

# Add a target to clean coverage data
clean_coverage:
	rm -f *.gcda *.gcno *.gcov

# Add a target to generate coverage report
coverage: clean_coverage
	$(MAKE) COVERAGE=1
# ./run_tests
# lcov --capture --directory ../build --output-file coverage.info
# lcov --remove coverage.info '*/test/*' --output-file coverage_filtered.info
# genhtml coverage_filtered.info --output-directory ../coverage

clean:
	rm -rf $(BUILD_DIR) $(TEST_TARGET) $(CONFIG_FILE_NAME) *.gcda *.gcno
	rm -f ../src/*.gcno ../src/*.gcda

.PHONY: all clean copy_config

# AddressSanitizer target
sanitize: CFLAGS += $(ASAN_FLAGS) -fno-omit-frame-pointer
sanitize: CXXFLAGS += $(ASAN_FLAGS) -fno-omit-frame-pointer
sanitize: CPPFLAGS += $(ASAN_FLAGS) -fno-omit-frame-pointer
sanitize: LDFLAGS += $(ASAN_FLAGS)
sanitize: CPPFLAGS += $(ASAN_FLAGS)
sanitize: clean all
