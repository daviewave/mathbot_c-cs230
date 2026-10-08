# Math Bot Client Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A single-file C99 TCP client (`src/client.c`) that identifies to the CS230 math server, answers every `STATUS` problem exactly, prints the flag from the `BYE` message, and is proven by unit tests and an end-to-end suite against a local mock server.

**Architecture:** One source file with small `static` functions in four groups: argument validation, TCP line framing (`LineBuffer`), protocol parsing and checked `long long` arithmetic, and the socket session loop. Unit tests include the source with `main` renamed; e2e tests drive the built binary against `test/e2e/mock_server.py` on a loopback port.

**Tech Stack:** C99 + POSIX sockets (`-D_POSIX_C_SOURCE=200809L`), GNU make, bash, python3 standard library (mock server), `gcc -fanalyzer`.

**Spec:** `docs/spec.md` (course spec) and `docs/design.md` (decisions); conventions in `docs/conventions.md`.

## Global Constraints

- Deliverable: exactly `src/client.c`; it must compile with plain `gcc client.c` AND with `gcc -std=c99 -D_POSIX_C_SOURCE=200809L -Wall -Wextra -Wpedantic -Wshadow -Wstrict-prototypes -Wmissing-prototypes -Wconversion -Wvla -Werror`.
- Arguments in this order: identification (`NetID@umass.edu`), port, host IPv4 address.
- Messages sent are byte-exact: `cs230 HELLO <id>\n` and `cs230 <ANSWER>\n`; no extra bytes.
- Arithmetic in `long long`; `/` truncates toward zero (`200 / 3` → `66`).
- Line framing must survive messages split across or coalesced within `recv` results.
- On `cs230 <FLAG> BYE\n`: print the flag on stdout, exit `EXIT_SUCCESS`. Any other outcome: message on stderr, `EXIT_FAILURE`.
- Every system call return value is checked. Every function has a header comment; no narration inside bodies.
- `make clean && make && make check && make test && make dist` must pass.
- No `Co-Authored-By`, no mention of AI assistance anywhere. Never push.

## Review Focus

1. A STATUS line arriving in two `recv` fragments (even split inside a number) must be answered exactly once. Pinned by Task 3 (`test_take_line_split_fragments`) and Task 5 (`test_receive_line_byte_by_byte`), and the e2e `fragmented` case in Task 6.
2. Two STATUS lines arriving in one `recv` must both be answered, in order, with no bytes lost. Pinned by Task 3 (`test_take_line_coalesced`) and the e2e `pipelined` case in Task 6.
3. The server closing the connection after a wrong answer must produce a clear stderr message and a non-zero exit, never a hang or a `SIGPIPE` death. Pinned by Task 5 (`test_receive_line_eof`) and the e2e `wrong_answer` case.
4. Negative results of division must truncate toward zero (`-7 / 2` → `-3`), and `x / 0` must be refused instead of crashing. Pinned by Task 4 (`test_evaluate_division_truncates_toward_zero`, `test_evaluate_division_by_zero_refused`).
5. A malformed STATUS line (extra spaces, trailing text, unknown operator) must be reported as a protocol error, not silently mis-answered. Pinned by Task 4 (`test_parse_status_rejects_*`) and the e2e `garbage_line` case.

---

### Task 1: Repository scaffold

**Files:**
- Create: `.gitignore`, `Makefile`, `test/unit/check.h`, `test/run_tests.sh`, `README.txt`, `src/client.c` (compiling skeleton with `main` only)

**Interfaces:**
- Produces: `make all|debug|test|check|run|dist|clean`; `build/client`; `test/run_tests.sh` compiles every `test/unit/test_*.c` with `$CFLAGS` and runs every `test/e2e/*.sh` with `CLIENT_BIN` exported.

- [ ] **Step 1: `.gitignore`**

```
build/
dist/
*.o
*.d
core
core.*
*.swp
*~
.vscode/
.idea/
```

- [ ] **Step 2: `Makefile`** (see `docs/conventions.md` section 3; comments are expected here)

```make
# Build driver for the Math Bot client. Section numbers refer to docs/conventions.md.
CC      ?= gcc
# Language floor and the feature macro that exposes POSIX sockets under -std=c99 (section 2).
STD      = -std=c99 -D_POSIX_C_SOURCE=200809L
WARNINGS = -Wall -Wextra -Wpedantic -Wshadow -Wstrict-prototypes -Wmissing-prototypes \
           -Wconversion -Wvla -Werror
OPTFLAGS ?= -O2
DEBUGFLAGS = -O0 -g3
# -MMD -MP writes header dependency files next to the objects.
CFLAGS  ?= $(OPTFLAGS) $(STD) $(WARNINGS) -MMD -MP
LDFLAGS ?=

BUILD   = build
DIST    = dist
SRC_DIR = src
SOURCES = $(wildcard $(SRC_DIR)/*.c)
OBJECTS = $(patsubst $(SRC_DIR)/%.c,$(BUILD)/%.o,$(SOURCES))
DEBUG_OBJECTS = $(patsubst $(SRC_DIR)/%.c,$(BUILD)/debug/%.o,$(SOURCES))
BIN     = $(BUILD)/client
DEBUG_BIN = $(BUILD)/debug/client

# Defaults for `make run`; override on the command line: make run ID=me@umass.edu
ID   ?= netid@umass.edu
PORT ?= 27993
HOST ?= 128.119.243.147

.PHONY: all debug test check run dist clean

all: $(BIN)

$(BIN): $(OBJECTS)
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)

$(BUILD)/%.o: $(SRC_DIR)/%.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

debug: $(DEBUG_BIN)

$(DEBUG_BIN): $(DEBUG_OBJECTS)
	$(CC) $(DEBUGFLAGS) $(STD) $(WARNINGS) $^ -o $@ $(LDFLAGS)

$(BUILD)/debug/%.o: $(SRC_DIR)/%.c | $(BUILD)/debug
	$(CC) $(DEBUGFLAGS) $(STD) $(WARNINGS) -MMD -MP -c $< -o $@

$(BUILD) $(BUILD)/debug $(BUILD)/check $(DIST):
	mkdir -p $@

# Unit tests are compiled by run_tests.sh with the same flags; e2e scripts get the binary path.
test: $(BIN)
	CC="$(CC)" CFLAGS="$(CFLAGS)" CLIENT_BIN="$(abspath $(BIN))" bash test/run_tests.sh

# Static analysis: -fanalyzer needs a real compile, so the object goes to build/check and is discarded.
check: | $(BUILD)/check
	$(CC) $(CFLAGS) -fanalyzer -c $(SOURCES) -o $(BUILD)/check/client.o

run: $(BIN)
	$(BIN) $(ID) $(PORT) $(HOST)

# Flat Gradescope bundle: the spec-named source, README, and a flat Makefile. Proves
# the bundle builds both with the course's `gcc -std=c99 -Wall` and with plain `gcc client.c`.
dist: | $(DIST)
	cp $(SRC_DIR)/client.c README.txt $(DIST)/
	printf '%s\n' 'CC ?= gcc' 'CFLAGS ?= -std=c99 -Wall' 'client: client.c' \
		'	$$(CC) $$(CFLAGS) client.c -o client' '.PHONY: clean' 'clean:' '	rm -f client a.out' \
		> $(DIST)/Makefile
	$(MAKE) -C $(DIST)
	cd $(DIST) && $(CC) client.c && rm -f a.out

clean:
	rm -rf $(BUILD) $(DIST)

-include $(OBJECTS:.o=.d) $(DEBUG_OBJECTS:.o=.d)
```

- [ ] **Step 3: `test/unit/check.h`** — copy the harness verbatim from `docs/conventions.md` section 6.

- [ ] **Step 4: `test/run_tests.sh`**

```bash
#!/usr/bin/env bash
# Builds every unit test binary, runs it, then runs every e2e script. One PASS/FAIL line per test.
set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
BUILD="$ROOT/build"
CC="${CC:-gcc}"
CFLAGS="${CFLAGS:--O2 -std=c99 -D_POSIX_C_SOURCE=200809L -Wall -Wextra -Wpedantic -Wshadow -Wstrict-prototypes -Wmissing-prototypes -Wconversion -Wvla -Werror}"
export CLIENT_BIN="${CLIENT_BIN:-$BUILD/client}"
export MOCK_SERVER="$ROOT/test/e2e/mock_server.py"

mkdir -p "$BUILD/test"
total=0
failures=0

run_test() {
    local name=$1
    shift
    total=$((total + 1))
    if "$@" > "$BUILD/test/$name.log" 2>&1; then
        echo "PASS $name"
    else
        echo "FAIL $name"
        sed 's/^/    /' "$BUILD/test/$name.log"
        failures=$((failures + 1))
    fi
}

for source in "$ROOT"/test/unit/test_*.c; do
    [ -e "$source" ] || continue
    name=$(basename "$source" .c)
    # shellcheck disable=SC2086
    $CC $CFLAGS "$source" -o "$BUILD/test/$name"
    run_test "$name" "$BUILD/test/$name"
done

for script in "$ROOT"/test/e2e/*.sh; do
    [ -e "$script" ] || continue
    name="e2e_$(basename "$script" .sh)"
    run_test "$name" bash "$script"
done

echo "$total tests, $failures failures"
[ "$failures" -eq 0 ]
```

- [ ] **Step 5: `src/client.c` skeleton** — header comment, the spec's includes, and `int main(void) { return EXIT_FAILURE; }` so `make` and `make check` succeed.

- [ ] **Step 6: `README.txt` skeleton** — title, overview placeholder sentence, build/run commands, empty "Requirements map" heading, and `Video: not required for this project (spec: "There is no video for this last submission")`.

- [ ] **Step 7: Verify** — `make clean && make && make check && make test && make dist` all succeed (test prints `0 tests, 0 failures`).

- [ ] **Step 8: Commit** — `git add -A && git commit -m "build: scaffold Makefile, test harness and README skeleton"`.

---

### Task 2: Argument validation

**Files:**
- Modify: `src/client.c`
- Create: `test/unit/test_client.c`

**Interfaces:**
- Produces:
  - `typedef struct { const char *identification; unsigned short port; const char *host; } ClientArguments;`
  - `static bool is_valid_identification(const char *identification);`
  - `static bool parse_port(const char *text, unsigned short *port);`
  - `static bool is_valid_host(const char *host);`
  - `static bool parse_arguments(int argc, char **argv, ClientArguments *arguments);` (prints the specific reason on stderr; caller prints usage)
  - `static void print_usage(const char *program);`

- [ ] **Step 1: Write the failing tests** in `test/unit/test_client.c`

```c
#include <stdio.h>
#include <stdlib.h>

int program_main(int argc, char **argv);   /* -Wmissing-prototypes for the renamed main */
#define main program_main
#include "../../src/client.c"
#undef main
#include "check.h"

static void test_identification(void) {
    CHECK(is_valid_identification("jdoe@umass.edu"));
    CHECK(!is_valid_identification("@umass.edu"));
    CHECK(!is_valid_identification("jdoe@gmail.com"));
    CHECK(!is_valid_identification("jdoe@umass.edu "));
    CHECK(!is_valid_identification("j doe@umass.edu"));
    CHECK(!is_valid_identification("a@b@umass.edu"));
    CHECK(!is_valid_identification(""));
}

static void test_parse_port(void) {
    unsigned short port = 0;
    CHECK(parse_port("27993", &port));
    CHECK_EQ_INT(port, 27993);
    CHECK(parse_port("1", &port) && port == 1);
    CHECK(parse_port("65535", &port) && port == 65535);
    CHECK(!parse_port("0", &port));
    CHECK(!parse_port("65536", &port));
    CHECK(!parse_port("-1", &port));
    CHECK(!parse_port("+5", &port));
    CHECK(!parse_port("80x", &port));
    CHECK(!parse_port(" 80", &port));
    CHECK(!parse_port("", &port));
    CHECK(!parse_port("99999999999999999999", &port));
}

static void test_host(void) {
    CHECK(is_valid_host("128.119.243.147"));
    CHECK(is_valid_host("127.0.0.1"));
    CHECK(!is_valid_host("localhost"));
    CHECK(!is_valid_host("256.1.1.1"));
    CHECK(!is_valid_host("1.2.3"));
    CHECK(!is_valid_host(""));
}

static void test_parse_arguments(void) {
    char *good[] = { "client", "jdoe@umass.edu", "27993", "128.119.243.147", NULL };
    char *bad_count[] = { "client", "jdoe@umass.edu", "27993", NULL };
    char *bad_port[] = { "client", "jdoe@umass.edu", "port", "128.119.243.147", NULL };
    ClientArguments arguments;
    CHECK(parse_arguments(4, good, &arguments));
    CHECK_EQ_STR(arguments.identification, "jdoe@umass.edu");
    CHECK_EQ_INT(arguments.port, 27993);
    CHECK_EQ_STR(arguments.host, "128.119.243.147");
    CHECK(!parse_arguments(3, bad_count, &arguments));
    CHECK(!parse_arguments(4, bad_port, &arguments));
}

int main(void) {
    test_identification();
    test_parse_port();
    test_host();
    test_parse_arguments();
    CHECK_REPORT("test_client");
}
```

- [ ] **Step 2: Run** `make test` — expected: compile error, `is_valid_identification` undeclared.

- [ ] **Step 3: Implement** in `src/client.c` (constants `MIN_PORT`, `MAX_PORT`, `EXPECTED_ARGUMENT_COUNT`, `MAX_IDENTIFICATION_LENGTH = 254`, `IDENTIFICATION_DOMAIN "@umass.edu"`):

```c
/* True for a non-empty NetID followed by @umass.edu, with no whitespace or second '@'. */
static bool is_valid_identification(const char *identification) {
    size_t length = strlen(identification);
    size_t domain_length = strlen(IDENTIFICATION_DOMAIN);
    const char *domain_start;
    if (length <= domain_length || length > MAX_IDENTIFICATION_LENGTH) {
        return false;
    }
    domain_start = identification + length - domain_length;
    if (strcmp(domain_start, IDENTIFICATION_DOMAIN) != 0 || strchr(identification, '@') != domain_start) {
        return false;
    }
    return !contains_whitespace(identification);
}

/* Parses a decimal port in 1..65535; rejects signs, whitespace and trailing text. */
static bool parse_port(const char *text, unsigned short *port) {
    char *end;
    long value;
    if (!isdigit((unsigned char)text[0])) {
        return false;
    }
    errno = 0;
    value = strtol(text, &end, 10);
    if (errno != 0 || *end != '\0' || value < MIN_PORT || value > MAX_PORT) {
        return false;
    }
    *port = (unsigned short)value;
    return true;
}
```
plus `contains_whitespace`, `is_valid_host` (`inet_pton(AF_INET, host, &address) == 1`), `parse_arguments` (count check, then each validator with a specific stderr message), `print_usage`.

- [ ] **Step 4: Run** `make test` — expected `PASS test_client`.
- [ ] **Step 5: Commit** — `git commit -am "feat: validate identification, port and host arguments"` (add the new test file).

---

### Task 3: Line framing buffer

**Files:**
- Modify: `src/client.c`, `test/unit/test_client.c`

**Interfaces:**
- Produces:
  - `enum { RECEIVE_BUFFER_SIZE = 4096 };`
  - `typedef struct { char data[RECEIVE_BUFFER_SIZE]; size_t used; } LineBuffer;`
  - `static bool line_buffer_append(LineBuffer *buffer, const char *bytes, size_t count);` false when the bytes do not fit
  - `static bool line_buffer_take_line(LineBuffer *buffer, char *line, size_t capacity);` copies the first complete line without its `\n`, shifts the rest down; false when no complete line is buffered

- [ ] **Step 1: Failing tests**

```c
static void test_take_line_split_fragments(void) {
    LineBuffer buffer = { {0}, 0 };
    char line[RECEIVE_BUFFER_SIZE];
    CHECK(line_buffer_append(&buffer, "cs230 STATUS 1", 14));
    CHECK(!line_buffer_take_line(&buffer, line, sizeof line));
    CHECK(line_buffer_append(&buffer, "2 + 3\n", 6));
    CHECK(line_buffer_take_line(&buffer, line, sizeof line));
    CHECK_EQ_STR(line, "cs230 STATUS 12 + 3");
    CHECK(!line_buffer_take_line(&buffer, line, sizeof line));
    CHECK_EQ_INT(buffer.used, 0);
}

static void test_take_line_coalesced(void) {
    LineBuffer buffer = { {0}, 0 };
    char line[RECEIVE_BUFFER_SIZE];
    const char *two = "cs230 STATUS 1 + 1\ncs230 STATUS 2 * 2\ncs230 ST";
    CHECK(line_buffer_append(&buffer, two, strlen(two)));
    CHECK(line_buffer_take_line(&buffer, line, sizeof line));
    CHECK_EQ_STR(line, "cs230 STATUS 1 + 1");
    CHECK(line_buffer_take_line(&buffer, line, sizeof line));
    CHECK_EQ_STR(line, "cs230 STATUS 2 * 2");
    CHECK(!line_buffer_take_line(&buffer, line, sizeof line));
    CHECK_EQ_INT(buffer.used, 8);
    CHECK(line_buffer_append(&buffer, "ATUS 3 - 4\n", 11));
    CHECK(line_buffer_take_line(&buffer, line, sizeof line));
    CHECK_EQ_STR(line, "cs230 STATUS 3 - 4");
}

static void test_take_line_empty_line(void) {
    LineBuffer buffer = { {0}, 0 };
    char line[RECEIVE_BUFFER_SIZE];
    CHECK(line_buffer_append(&buffer, "\n", 1));
    CHECK(line_buffer_take_line(&buffer, line, sizeof line));
    CHECK_EQ_STR(line, "");
}

static void test_append_overflow(void) {
    LineBuffer buffer = { {0}, 0 };
    char filler[RECEIVE_BUFFER_SIZE];
    memset(filler, 'x', sizeof filler);
    CHECK(line_buffer_append(&buffer, filler, sizeof filler));
    CHECK(!line_buffer_append(&buffer, "y", 1));
    CHECK_EQ_INT(buffer.used, RECEIVE_BUFFER_SIZE);
}
```

- [ ] **Step 2: Run** `make test` — expected compile failure on `LineBuffer`.
- [ ] **Step 3: Implement** (`memchr` for the newline, `memcpy` out, `memmove` the remainder, `buffer->used -= length + 1`).
- [ ] **Step 4: Run** `make test` — PASS.
- [ ] **Step 5: Commit** — `git commit -am "feat: frame newline-terminated lines across recv boundaries"`.

---

### Task 4: STATUS and BYE parsing, checked arithmetic

**Files:**
- Modify: `src/client.c`, `test/unit/test_client.c`

**Interfaces:**
- Produces:
  - `typedef struct { long long left; char operation; long long right; } MathProblem;`
  - `static bool parse_operand(const char *text, const char **end, long long *value);`
  - `static bool parse_status(const char *line, MathProblem *problem);`
  - `static bool parse_bye(const char *line, char *flag, size_t capacity);`
  - `static bool evaluate(const MathProblem *problem, long long *result);` false on division by zero, `LLONG_MIN / -1`, overflow, unknown operator
  - `#define PROTOCOL_PREFIX "cs230 "`, `#define STATUS_PREFIX "cs230 STATUS "`, `#define BYE_SUFFIX " BYE"`

- [ ] **Step 1: Failing tests**

```c
static void test_parse_status_accepts_spec_example(void) {
    MathProblem problem;
    CHECK(parse_status("cs230 STATUS 505 * 700", &problem));
    CHECK_EQ_INT(problem.left, 505);
    CHECK_EQ_INT(problem.operation, '*');
    CHECK_EQ_INT(problem.right, 700);
}

static void test_parse_status_accepts_negatives(void) {
    MathProblem problem;
    CHECK(parse_status("cs230 STATUS -12 / -5", &problem));
    CHECK_EQ_INT(problem.left, -12);
    CHECK_EQ_INT(problem.right, -5);
}

static void test_parse_status_rejects_malformed(void) {
    MathProblem problem;
    CHECK(!parse_status("cs230 STATUS 1  + 2", &problem));      /* double space */
    CHECK(!parse_status("cs230 STATUS  1 + 2", &problem));      /* leading space before operand */
    CHECK(!parse_status("cs230 STATUS 1 + 2 ", &problem));      /* trailing space */
    CHECK(!parse_status("cs230 STATUS 1 + 2 3", &problem));     /* trailing token */
    CHECK(!parse_status("cs230 STATUS 1 % 2", &problem));       /* unknown operator */
    CHECK(!parse_status("cs230 STATUS 1 +", &problem));         /* missing operand */
    CHECK(!parse_status("cs230 STATUS a + 2", &problem));
    CHECK(!parse_status("cs230 STATUS +1 + 2", &problem));
    CHECK(!parse_status("cs230 STATUS 99999999999999999999 + 2", &problem));  /* ERANGE */
    CHECK(!parse_status("cs230 HELLO 1 + 2", &problem));
    CHECK(!parse_status("", &problem));
}

static void test_parse_bye(void) {
    char flag[FLAG_CAPACITY];
    CHECK(parse_bye("cs230 7c5ee45183d657f5148fd4bbabb6615128ec32699164980be7b8b451fd9ac0c3 BYE", flag, sizeof flag));
    CHECK_EQ_STR(flag, "7c5ee45183d657f5148fd4bbabb6615128ec32699164980be7b8b451fd9ac0c3");
    CHECK(!parse_bye("cs230  BYE", flag, sizeof flag));
    CHECK(!parse_bye("cs230 BYE", flag, sizeof flag));
    CHECK(!parse_bye("cs230 abc def BYE", flag, sizeof flag));
    CHECK(!parse_bye("cs230 abc BYE ", flag, sizeof flag));
    CHECK(!parse_bye("cs230 STATUS 1 + 2", flag, sizeof flag));
}

static void test_evaluate_basic(void) {
    long long result;
    MathProblem add = { 505, '+', 700 }, sub = { 5, '-', 9 }, mul = { 505, '*', 700 }, div = { 200, '/', 3 };
    CHECK(evaluate(&add, &result) && result == 1205);
    CHECK(evaluate(&sub, &result) && result == -4);
    CHECK(evaluate(&mul, &result) && result == 353500);
    CHECK(evaluate(&div, &result) && result == 66);
}

static void test_evaluate_division_truncates_toward_zero(void) {
    long long result;
    MathProblem a = { -7, '/', 2 }, b = { 7, '/', -2 }, c = { -7, '/', -2 }, d = { 1, '/', 3 };
    CHECK(evaluate(&a, &result) && result == -3);
    CHECK(evaluate(&b, &result) && result == -3);
    CHECK(evaluate(&c, &result) && result == 3);
    CHECK(evaluate(&d, &result) && result == 0);
}

static void test_evaluate_division_by_zero_refused(void) {
    long long result;
    MathProblem zero = { 5, '/', 0 }, min = { LLONG_MIN, '/', -1 };
    CHECK(!evaluate(&zero, &result));
    CHECK(!evaluate(&min, &result));
}

static void test_evaluate_overflow_refused(void) {
    long long result;
    MathProblem add = { LLONG_MAX, '+', 1 }, sub = { LLONG_MIN, '-', 1 };
    MathProblem mul = { LLONG_MAX, '*', 2 }, neg_mul = { LLONG_MIN, '*', -1 };
    MathProblem ok_mul = { -3037000499LL, '*', 3037000499LL }, unknown = { 1, '%', 1 };
    CHECK(!evaluate(&add, &result));
    CHECK(!evaluate(&sub, &result));
    CHECK(!evaluate(&mul, &result));
    CHECK(!evaluate(&neg_mul, &result));
    CHECK(evaluate(&ok_mul, &result) && result == -9223372030926249001LL);
    CHECK(!evaluate(&unknown, &result));
}
```

- [ ] **Step 2: Run** `make test` — compile failure on `MathProblem`.
- [ ] **Step 3: Implement**: `parse_operand` (first char must be `-` or a digit, `errno = 0; strtoll; reject ERANGE or no digits`), `is_operator`, `parse_status` (prefix check, operand, `" <op> "`, operand, must end at `'\0'`), `parse_bye` (prefix and suffix checks, flag token non-empty, no spaces, fits capacity), `add_checked`, `subtract_checked`, `multiply_checked` (CERT INT32-C pre-checks), `divide_checked`, `evaluate` as a `switch`.
- [ ] **Step 4: Run** `make test` — PASS.
- [ ] **Step 5: Commit** — `git commit -am "feat: parse STATUS and BYE messages and evaluate with overflow guards"`.

---

### Task 5: Socket session and main

**Files:**
- Modify: `src/client.c`, `test/unit/test_client.c`

**Interfaces:**
- Consumes: everything from Tasks 2–4.
- Produces:
  - `typedef enum { RECEIVE_LINE, RECEIVE_EOF, RECEIVE_ERROR } ReceiveResult;`
  - `static ReceiveResult receive_line(int socket_fd, LineBuffer *buffer, char *line, size_t capacity);`
  - `static bool send_all(int socket_fd, const char *bytes, size_t count);`
  - `static bool send_line(int socket_fd, const char *line, bool verbose);`
  - `static bool send_hello(int socket_fd, const char *identification, bool verbose);`
  - `static bool handle_status(int socket_fd, const MathProblem *problem, bool verbose);`
  - `static bool print_flag(const char *flag);`
  - `static int connect_to_server(const char *host, unsigned short port);`
  - `static int run_session(int socket_fd, const char *identification, bool verbose);`
  - `static bool is_verbose_enabled(void);` reads `MATHBOT_VERBOSE`
  - `int main(int argc, char **argv)`

- [ ] **Step 1: Failing tests** (use `socketpair(AF_UNIX, SOCK_STREAM, 0, fds)`; `write` to `fds[1]`, read with the functions under test on `fds[0]`)

```c
static void test_receive_line_byte_by_byte(void) {
    int fds[2];
    LineBuffer buffer = { {0}, 0 };
    char line[RECEIVE_BUFFER_SIZE];
    const char *message = "cs230 STATUS 12 + 34\n";
    size_t i;
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);
    for (i = 0; i < strlen(message); i++) {
        CHECK(write(fds[1], message + i, 1) == 1);
    }
    CHECK_EQ_INT(receive_line(fds[0], &buffer, line, sizeof line), RECEIVE_LINE);
    CHECK_EQ_STR(line, "cs230 STATUS 12 + 34");
    close(fds[0]);
    close(fds[1]);
}

static void test_receive_line_eof(void) {
    int fds[2];
    LineBuffer buffer = { {0}, 0 };
    char line[RECEIVE_BUFFER_SIZE];
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);
    CHECK(write(fds[1], "partial", 7) == 7);
    close(fds[1]);
    CHECK_EQ_INT(receive_line(fds[0], &buffer, line, sizeof line), RECEIVE_EOF);
    close(fds[0]);
}

static void test_receive_line_too_long(void) {
    int fds[2];
    LineBuffer buffer = { {0}, 0 };
    char line[RECEIVE_BUFFER_SIZE];
    char filler[RECEIVE_BUFFER_SIZE + 1];
    memset(filler, 'x', sizeof filler);
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);
    CHECK(write(fds[1], filler, sizeof filler) == (ssize_t)sizeof filler);
    CHECK_EQ_INT(receive_line(fds[0], &buffer, line, sizeof line), RECEIVE_ERROR);
    close(fds[0]);
    close(fds[1]);
}

static void test_send_hello_exact_bytes(void) {
    int fds[2];
    char received[64];
    ssize_t count;
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);
    CHECK(send_hello(fds[0], "jdoe@umass.edu", false));
    count = read(fds[1], received, sizeof received - 1);
    CHECK(count > 0);
    received[count > 0 ? count : 0] = '\0';
    CHECK_EQ_STR(received, "cs230 HELLO jdoe@umass.edu\n");
    close(fds[0]);
    close(fds[1]);
}

static void test_handle_status_exact_bytes(void) {
    int fds[2];
    char received[64];
    ssize_t count;
    MathProblem problem = { 505, '*', 700 }, bad = { 1, '/', 0 };
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);
    CHECK(handle_status(fds[0], &problem, false));
    count = read(fds[1], received, sizeof received - 1);
    CHECK(count > 0);
    received[count > 0 ? count : 0] = '\0';
    CHECK_EQ_STR(received, "cs230 353500\n");
    CHECK(!handle_status(fds[0], &bad, false));
    close(fds[0]);
    close(fds[1]);
}

static void test_connect_refused(void) {
    CHECK(connect_to_server("127.0.0.1", 1) < 0);   /* nothing listens on tcp/1 in the sandbox */
}
```

- [ ] **Step 2: Run** `make test` — compile failure on `receive_line`.
- [ ] **Step 3: Implement** per `docs/design.md` sections 3–5: `receive_line` loops `take_line → recv into a chunk → append`, retries `EINTR`, reports `strerror(errno)`; `send_all` loops over short sends; `connect_to_server` builds `sockaddr_in`, closes the socket on every failure; `run_session` sends HELLO then loops; `main` ignores `SIGPIPE` (`signal(SIGPIPE, SIG_IGN)`, checked against `SIG_ERR`), parses arguments, connects, runs, closes (checked).
- [ ] **Step 4: Run** `make test && make check` — PASS, analyzer clean.
- [ ] **Step 5: Commit** — `git commit -am "feat: connect, identify and answer problems until the flag arrives"`.

---

### Task 6: Mock server and e2e suite

**Files:**
- Create: `test/e2e/mock_server.py`, `test/e2e/lib.sh`, `test/e2e/happy_path.sh`, `test/e2e/fragmented.sh`, `test/e2e/pipelined.sh`, `test/e2e/wrong_answer.sh`, `test/e2e/garbage_line.sh`, `test/e2e/division_by_zero.sh`, `test/e2e/bad_arguments.sh`, `test/e2e/connection_refused.sh`

**Interfaces:**
- Consumes: `$CLIENT_BIN` (absolute path to the built client), `$MOCK_SERVER` (path to the mock), both exported by `test/run_tests.sh`; the client's contract: flag alone on stdout + exit 0 on success, nothing on stdout + non-zero otherwise.
- Produces: `python3 -I mock_server.py --port-file PATH [--seed N] [--problems N] [--fragment] [--pipeline] [--reject] [--inject LINE]` serving one connection then exiting; the flag is `sha256(identification).hexdigest()`.

- [ ] **Step 1: Mock server** (`socket` + `selectors`-free blocking code is fine; `setsockopt(TCP_NODELAY)`; bind `127.0.0.1:0`; write the chosen port to `--port-file` atomically (write to a temp name then `os.replace`); `listen`; accept one client; read lines with its own buffer; require `cs230 HELLO <id>` first; problem count `random.Random(seed).randint(300, 2000)` unless `--problems`; operands in `-1000..1000` (divisor never 0); expected answers computed with `int()`-truncation toward zero, i.e. `-(abs(a) // abs(b))` when signs differ, never Python's floor division; `--fragment` splits every 7th message at a random byte and sleeps 2 ms between the pieces; `--pipeline` sends every 5th problem together with the next one in one `sendall` and then reads two answers; `--reject` closes the socket after the first answer without BYE; `--inject LINE` sends LINE + `\n` instead of the first problem; a wrong answer closes the socket; the final message is `cs230 <flag> BYE\n`; exit 0 when the flag was sent, 1 otherwise.)
- [ ] **Step 2: `lib.sh`**: `start_mock <args…>` starts the server in the background with a `$TMPDIR` port file, waits (polling, 50 × 0.1 s) for the port file, exports `MOCK_PORT`; a `trap` kills the server on exit; `run_client <args…>` runs `timeout 60 "$CLIENT_BIN"` capturing stdout/stderr to files and recording the exit code.
- [ ] **Step 3: Scripts** assert with `grep`/`test`: happy path (exit 0, stdout is exactly the expected sha256 hex of the id, stderr empty); fragmented and pipelined (same, with `--fragment` / `--pipeline --problems 400`); wrong_answer (`--reject`: exit non-zero, empty stdout, stderr matches `closed the connection`); garbage_line (`--inject "cs230 WHAT"`: exit non-zero, stderr matches `protocol error`); division_by_zero (`--inject "cs230 STATUS 5 / 0"`: exit non-zero, stderr matches `protocol error`); bad_arguments (no args, 2 args, bad email, port 0, port 70000, host `localhost`: each exits non-zero with `usage` on stderr, nothing on stdout); connection_refused (port taken from a listening socket that is then closed, or simply port 1: exit non-zero, stderr matches `connect`).
- [ ] **Step 4: Run** `make test` — every `e2e_*` line PASS.
- [ ] **Step 5: Commit** — `git add test && git commit -m "test: add mock math server and end-to-end suite"`.

---

### Task 7: README, gate, review

- [ ] **Step 1:** Fill `README.txt`: overview, build/run, requirements map (every spec bullet → function), design notes, the sandbox note (real server unreachable; verified against the mock), no-video line.
- [ ] **Step 2:** `make clean && make && make check && make test && make dist` — record outputs.
- [ ] **Step 3:** Independent review against `docs/spec.md` and `docs/conventions.md`: unmet rubric items, unchecked return values, comment rule violations. Fix each, re-run the gate.
- [ ] **Step 4: Commit** — `git commit -am "docs: finish README requirements map"`.
