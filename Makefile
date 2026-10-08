# Build driver for the Math Bot client. Section numbers refer to docs/conventions.md.
# The submitted deliverable is src/client.c alone; everything here exists to prove it,
# including server/mathbot_server.c, the local stand-in for the course server (docs/server.md).

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
# The server is built from its own directory with the client's flags; it is never submitted.
SERVER_DIR = server
SERVER_SOURCES = $(wildcard $(SERVER_DIR)/*.c)
SERVER_OBJECTS = $(patsubst $(SERVER_DIR)/%.c,$(BUILD)/server/%.o,$(SERVER_SOURCES))
SERVER_BIN = $(BUILD)/mathbot_server

# Defaults for `make run` and `make server-run`: the local server on the course's port.
# The course server (128.119.243.147) is gone; override on the command line: make run ID=me@umass.edu
ID ?= netid@umass.edu
PORT ?= 27993
HOST ?= 127.0.0.1
# The identification `make autograde` grades with; any NetID@umass.edu works since the flag derives from it.
AUTOGRADE_ID ?= autograde@umass.edu

.PHONY: all debug test check run dist clean server server-run autograde

# all: release binary into build/ (section 3).
all: $(BIN)

$(BIN): $(OBJECTS)
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)

$(BUILD)/%.o: $(SRC_DIR)/%.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

# server: the local math-speak server, same flags; its objects live in build/server/ so a
# src/ file of the same name could never shadow them.
server: $(SERVER_BIN)

$(SERVER_BIN): $(SERVER_OBJECTS)
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)

$(BUILD)/server/%.o: $(SERVER_DIR)/%.c | $(BUILD)/server
	$(CC) $(CFLAGS) -c $< -o $@

# server-run: serve on PORT (default 27993) until Ctrl-C; the client then targets 127.0.0.1.
server-run: $(SERVER_BIN)
	$(SERVER_BIN) $(PORT)

# debug: -O0 -g3 binary into build/debug/ so it never shadows the release objects.
debug: $(DEBUG_BIN)

$(DEBUG_BIN): $(DEBUG_OBJECTS)
	$(CC) $(DEBUGFLAGS) $(STD) $(WARNINGS) $^ -o $@ $(LDFLAGS)

$(BUILD)/debug/%.o: $(SRC_DIR)/%.c | $(BUILD)/debug
	$(CC) $(DEBUGFLAGS) $(STD) $(WARNINGS) -MMD -MP -c $< -o $@

$(BUILD) $(BUILD)/debug $(BUILD)/check $(BUILD)/server:
	mkdir -p $@

# test: run_tests.sh compiles the unit tests with the same flags and hands both binaries to the e2e scripts.
test: $(BIN) $(SERVER_BIN)
	CC="$(CC)" CFLAGS="$(CFLAGS)" CLIENT_BIN="$(abspath $(BIN))" SERVER_BIN="$(abspath $(SERVER_BIN))" \
		bash test/run_tests.sh

# check: -fanalyzer needs a real compile, so the objects land in build/check and are never linked.
check: | $(BUILD)/check
	$(CC) $(CFLAGS) -fanalyzer -c $(SOURCES) -o $(BUILD)/check/client.o
	$(CC) $(CFLAGS) -fanalyzer -c $(SERVER_SOURCES) -o $(BUILD)/check/mathbot_server.o

# run: the client against HOST:PORT (default: a local server) with a placeholder NetID; pass ID=... to use yours.
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

# autograde: what Gradescope did (docs/server.md): the dist bundle compiled with a plain `gcc client.c -o a.out`,
# run against the local server with AUTOGRADE_ID, stdout compared with the flag python computes. PASS/FAIL, exit status.
autograde: dist $(SERVER_BIN)
	DIST="$(abspath $(DIST))" SERVER_BIN="$(abspath $(SERVER_BIN))" AUTOGRADE_ID="$(AUTOGRADE_ID)" bash test/autograde.sh

clean:
	rm -rf $(BUILD) $(DIST)

-include $(OBJECTS:.o=.d) $(DEBUG_OBJECTS:.o=.d) $(SERVER_OBJECTS:.o=.d)
