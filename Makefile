# toronto: build the runtime, the examples and the tests, and run the tests.
#
#   make                 build everything for this OS
#   make test            build everything, then run every test
#   make check-linux     run `make test` in Docker (gcc:14, epoll backend)
#   make POLLER=kqueue   force a poller backend
#   make SAN=            build without the sanitizers
#   make clean           remove build/
#
# Requires GNU make (3.81 or newer).

OS := $(shell uname -s | tr '[:upper:]' '[:lower:]')

ifeq ($(OS),linux)
POLLER ?= epoll
else
POLLER ?= kqueue
endif

ifeq ($(filter $(POLLER),epoll kqueue),)
$(error POLLER must be epoll or kqueue, not '$(POLLER)')
endif

# One build directory per OS and backend. The macOS build and the Docker build
# share this source directory, and they must not share object files.
BUILD := build/$(OS)-$(POLLER)

OPT  ?= -O1 -g
# UBSan only reports by default; -fno-sanitize-recover makes a report fail the test.
SAN  ?= -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer
WARN := -Wall -Wextra -Wpedantic -Werror

CFLAGS  := -std=c11 $(WARN) $(OPT) $(SAN) -I. -MMD -MP
LDFLAGS := $(SAN)

# The runtime is every file in runtime/, with only the selected poller backend.
RUNTIME_SRC := $(filter-out runtime/poller_%.c,$(wildcard runtime/*.c)) \
               $(wildcard runtime/poller_$(POLLER).c)
EXAMPLE_SRC := $(wildcard examples/*.c)
# One program per test. Exit code 0 means pass.
TEST_SRC    := $(wildcard tests/test_*.c)

RUNTIME_OBJ := $(RUNTIME_SRC:%.c=$(BUILD)/%.o)
EXAMPLE_BIN := $(EXAMPLE_SRC:%.c=$(BUILD)/%)
TEST_BIN    := $(TEST_SRC:%.c=$(BUILD)/%)
PROGRAMS    := $(EXAMPLE_BIN) $(TEST_BIN)
OBJ         := $(RUNTIME_OBJ) $(PROGRAMS:%=%.o)

.PHONY: all test check-linux clean

all: $(RUNTIME_OBJ) $(PROGRAMS)

$(OBJ): $(BUILD)/%.o: %.c Makefile
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

# Every example and every test is one source file linked with the runtime.
$(PROGRAMS): $(BUILD)/%: $(BUILD)/%.o $(RUNTIME_OBJ)
	$(CC) $(LDFLAGS) $^ -o $@

test: all
	@pass=0; fail=0; \
	for t in $(TEST_BIN); do \
		if ./$$t; then \
			pass=$$((pass + 1)); echo "PASS  $$t"; \
		else \
			fail=$$((fail + 1)); echo "FAIL  $$t"; \
		fi; \
	done; \
	echo "$(OS)-$(POLLER): $$pass passed, $$fail failed"; \
	[ $$fail -eq 0 ]

check-linux:
	docker run --rm -v "$(CURDIR)":/src -w /src gcc:14 make test

clean:
	rm -rf build

-include $(OBJ:.o=.d)
