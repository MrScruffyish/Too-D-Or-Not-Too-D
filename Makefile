# Convenience wrapper around the CMake build described in README.md.

CMAKE        ?= cmake
GENERATOR    ?= Ninja
BUILD_DIR    ?= build
JOBS         ?= $(shell nproc 2>/dev/null || echo 2)
SELFTEST_DIR ?= /tmp/tood-selftest

.PHONY: all configure build run selftest clean

all: build

configure:
	$(CMAKE) -S . -B $(BUILD_DIR) -G $(GENERATOR)

build: configure
	$(CMAKE) --build $(BUILD_DIR) -j$(JOBS)

run: build
	./$(BUILD_DIR)/tood

selftest: build
	mkdir -p $(SELFTEST_DIR)
	./$(BUILD_DIR)/tood --selftest $(SELFTEST_DIR)

clean:
	rm -rf $(BUILD_DIR)
