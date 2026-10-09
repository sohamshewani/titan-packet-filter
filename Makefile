CXX := g++
CXXFLAGS := -std=c++17 -O3 -Wall -Wextra -MMD -MP
LDFLAGS := 

SRC_DIR := src
BUILD_DIR := build
TEST_DIR := tests
BENCH_DIR := benchmark

TARGET := titan-packet-filter
TEST_TARGET := test-runner
BENCH_TARGET := bench-runner

SRCS := $(wildcard $(SRC_DIR)/*.cpp)
OBJS := $(patsubst $(SRC_DIR)/%.cpp, $(BUILD_DIR)/%.o, $(SRCS))
DEPS := $(OBJS:.o=.d)

TEST_SRCS := $(wildcard $(TEST_DIR)/*.cpp)
BENCH_SRCS := $(wildcard $(BENCH_DIR)/*.cpp)

.PHONY: all build test clean benchmark

all: build

build: $(BUILD_DIR)/$(TARGET)

$(BUILD_DIR)/$(TARGET): $(OBJS) | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) $^ -o $@ $(LDFLAGS)

$(BUILD_DIR)/%.o: $(SRC_DIR)/%.cpp | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(BUILD_DIR):
	@mkdir -p $(BUILD_DIR)

test: build | $(BUILD_DIR)
	@if [ -d "$(TEST_DIR)" ] && [ -n "$(TEST_SRCS)" ]; then \
		$(CXX) $(CXXFLAGS) $(TEST_SRCS) $(filter-out $(BUILD_DIR)/main.o, $(OBJS)) -o $(BUILD_DIR)/$(TEST_TARGET) $(LDFLAGS); \
		$(BUILD_DIR)/$(TEST_TARGET); \
	else \
		echo "No tests found in $(TEST_DIR). Skipping test target."; \
	fi

benchmark: build | $(BUILD_DIR)
	@if [ -d "$(BENCH_DIR)" ] && [ -n "$(BENCH_SRCS)" ]; then \
		$(CXX) $(CXXFLAGS) $(BENCH_SRCS) $(filter-out $(BUILD_DIR)/main.o, $(OBJS)) -o $(BUILD_DIR)/$(BENCH_TARGET) $(LDFLAGS); \
		$(BUILD_DIR)/$(BENCH_TARGET); \
	else \
		echo "No benchmarks found in $(BENCH_DIR). Skipping benchmark target."; \
	fi

clean:
	@rm -rf $(BUILD_DIR)

-include $(DEPS)