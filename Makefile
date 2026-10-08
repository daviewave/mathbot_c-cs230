# Build driver for the Math Bot client. Section numbers refer to docs/conventions.md.
# The submitted deliverable is src/client.c alone; everything here exists to prove it.

# make predefines CC=cc, so ?= would never apply; = still lets `make CC=clang` override.
CC = gcc
# Language floor plus the feature macro that exposes POSIX sockets under -std=c99 (section 2).
STD = -std=c99 -D_POSIX_C_SOURCE=200809L
# Development warning set, treated as errors (section 2).
WARNINGS = -Wall -Wextra -Wpedantic -Wshadow -Wstrict-prototypes -Wmissing-prototypes \
	       -Wconversion -Wvla -Werror
OPTFLAGS ?= -O2
DEBUGFLAGS = -O0 -g3
# -MMD -MP writes header dependency files next to the objects so edits rebuild what they touch.
CFLAGS ?= $(OPTFLAGS) $(STD) $(WARNINGS) -MMD -MP
LDFLAGS ?=

BUILD = build
DIST = dist
SRC_DIR = src
SOURCES = $(wildcard $(SRC_DIR)/*.c)
OBJECTS = $(patsubst $(SRC_DIR)/%.c,$(BUILD)/%.o,$(SOURCES))
DEBUG_OBJECTS = $(patsubst $(SRC_DIR)/%.c,$(BUILD)/debug/%.o,$(SOURCES))
BIN = $(BUILD)/client
DEBUG_BIN = $(BUILD)/debug/client

# Defaults for `make run`, the course server; override on the command line: make run ID=me@umass.edu
ID ?= netid@umass.edu
PORT ?= 27993
HOST ?= 128.119.243.147

.PHONY: all debug test check run dist clean

# all: release binary into build/ (section 3).
all: $(BIN)

$(BIN): $(OBJECTS)
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)

$(BUILD)/%.o: $(SRC_DIR)/%.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

# debug: -O0 -g3 binary into build/debug/ so it never shadows the release objects.
debug: $(DEBUG_BIN)

$(DEBUG_BIN): $(DEBUG_OBJECTS)
	$(CC) $(DEBUGFLAGS) $(STD) $(WARNINGS) $^ -o $@ $(LDFLAGS)

$(BUILD)/debug/%.o: $(SRC_DIR)/%.c | $(BUILD)/debug
	$(CC) $(DEBUGFLAGS) $(STD) $(WARNINGS) -MMD -MP -c $< -o $@

$(BUILD) $(BUILD)/debug $(BUILD)/check:
	mkdir -p $@

# test: run_tests.sh compiles the unit tests with the same flags and hands the binary to the e2e scripts.
test: $(BIN)
	CC="$(CC)" CFLAGS="$(CFLAGS)" CLIENT_BIN="$(abspath $(BIN))" bash test/run_tests.sh

# check: -fanalyzer needs a real compile, so the object lands in build/check and is never linked.
check: | $(BUILD)/check
	$(CC) $(CFLAGS) -fanalyzer -c $(SOURCES) -o $(BUILD)/check/client.o

# run: the course server with a placeholder NetID; pass ID=... to use yours.
run: $(BIN)
	$(BIN) $(ID) $(PORT) $(HOST)

# dist: flat Gradescope bundle (section 7). Proves three builds: the generated flat Makefile with
# the strict flags, the course's `gcc -std=c99 -Wall`, and the autograder's plain `gcc client.c`.
dist:
	mkdir -p $(DIST)
	cp $(SRC_DIR)/client.c README.txt $(DIST)/
	printf 'CC = gcc\nCFLAGS ?= $(STD) $(WARNINGS)\n\nclient: client.c\n\t$$(CC) $$(CFLAGS) client.c -o client\n\n.PHONY: clean\nclean:\n\trm -f client a.out\n' > $(DIST)/Makefile
	$(MAKE) -C $(DIST)
	cd $(DIST) && $(CC) -std=c99 -Wall client.c -o a.out && rm -f a.out
	cd $(DIST) && $(CC) client.c && rm -f a.out

clean:
	rm -rf $(BUILD) $(DIST)

-include $(OBJECTS:.o=.d) $(DEBUG_OBJECTS:.o=.d)
