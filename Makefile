# Plain Makefile for macOS and Linux. Windows: use CMake (see CMakeLists.txt).
#
#   make            build the demo and the tests into build/
#   make test       run the unit tests
#   make asan       run the unit tests built with -fsanitize=address,undefined
#   make cxx        compile the implementation as C++11 and use the API from C++
#   make interop    run tests/interop.sh (set RBL_FAKE_HOST, RBL_FAKE_GUEST, RBL_JAVA_CP for the other peers)
#   make clean

CC      ?= cc
CXX     ?= c++
OPT     ?= -O2 -g
WARN    := -Wall -Wextra -Wpedantic -Wshadow
CSTD    := -std=c99
BUILD   := build
LDLIBS  := -lm
THREADS := -pthread
SAN     := -fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer

all: $(BUILD)/demo $(BUILD)/test_link $(BUILD)/host_example $(BUILD)/guest_example

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/demo: examples/demo.c examples/impl.c rewrite_link.h | $(BUILD)
	$(CC) $(CSTD) $(WARN) $(OPT) examples/demo.c examples/impl.c -o $@ $(LDLIBS)

$(BUILD)/host_example: examples/host_example.c examples/game_stub.c rewrite_link.h | $(BUILD)
	$(CC) $(CSTD) $(WARN) $(OPT) examples/host_example.c examples/game_stub.c -o $@ $(LDLIBS)

$(BUILD)/guest_example: examples/guest_example.c examples/game_stub.c rewrite_link.h | $(BUILD)
	$(CC) $(CSTD) $(WARN) $(OPT) examples/guest_example.c examples/game_stub.c -o $@ $(LDLIBS)

$(BUILD)/test_link: tests/test_link.c rewrite_link.h | $(BUILD)
	$(CC) $(CSTD) $(WARN) $(OPT) $(THREADS) tests/test_link.c -o $@ $(LDLIBS)

$(BUILD)/test_link_asan: tests/test_link.c rewrite_link.h | $(BUILD)
	$(CC) $(CSTD) $(WARN) -O1 -g $(SAN) $(THREADS) tests/test_link.c -o $@ $(LDLIBS)

$(BUILD)/cxx_check: tests/cxx_check.cpp tests/cxx_other.cpp rewrite_link.h | $(BUILD)
	$(CXX) -std=c++11 $(WARN) $(OPT) tests/cxx_check.cpp tests/cxx_other.cpp -o $@

test: $(BUILD)/test_link
	$(BUILD)/test_link

asan: $(BUILD)/test_link_asan
	$(BUILD)/test_link_asan

cxx: $(BUILD)/cxx_check
	$(BUILD)/cxx_check

interop: $(BUILD)/demo
	RBL_DEMO=$(BUILD)/demo tests/interop.sh

clean:
	rm -rf $(BUILD)

.PHONY: all test asan cxx interop clean
