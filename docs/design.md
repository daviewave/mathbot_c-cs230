# Math Bot client: design

The deliverable is one file, `src/client.c`, that must compile with a plain
`gcc client.c` on the course Vagrant box and with the strict flag set in
`docs/conventions.md`. Everything below is organised around that single file; the
Makefile, tests and mock server exist to prove it works, and are not submitted.

## 1. Command line and exit behaviour

```
./client <NetID@umass.edu> <port> <host IPv4>
```

| Argument | Validation | Why |
| --- | --- | --- |
| identification | non-empty NetID, ends with `@umass.edu`, no whitespace | the spec says "must be a UMass email address of the form NetID@umass.edu"; the server expects it verbatim after `cs230 HELLO ` |
| port | all digits, `strtol` with `endptr`, range 1..65535 | `htons` needs a 16-bit value; port 0 cannot be connected to |
| host | `inet_pton(AF_INET, …) == 1` | the spec names an IPv4 dotted quad; `inet_pton` is the function the spec hints at and it rejects malformed input, unlike `inet_addr` |

Wrong argument count or any failed validation prints a one-line usage to
`stderr` and exits `EXIT_FAILURE`. The flag is the only thing ever written to
`stdout`, so an autograder or a shell pipeline can capture it cleanly. Every
diagnostic goes to `stderr`.

Exit codes: `EXIT_SUCCESS` only after a `cs230 <FLAG> BYE` line was received
and the flag was printed. Everything else, including the server closing the
connection early (which is what it does on a wrong answer), is `EXIT_FAILURE`.

## 2. Data structures

```c
typedef struct {
    const char *identification;
    unsigned short port;
    struct in_addr host;      /* parsed once by inet_pton, reused by connect */
} ClientArguments;

typedef struct {
    char data[RECEIVE_BUFFER_SIZE];   /* bytes received but not yet consumed */
    size_t used;
} LineBuffer;

typedef struct {
    long long left;
    char operation;
    long long right;
} MathProblem;
```

`LineBuffer` is the whole answer to TCP framing. `recv` returns whatever bytes
the kernel has, which may be half a line or two lines at once. The buffer
accumulates bytes; `line_buffer_take_line` copies the first complete
`\n`-terminated line out (without the `\n`, and without one trailing `\r` so a
CRLF server parses like an LF one) and shifts the remainder to the front with
`memmove`. The network reader loops `take_line → recv → append`
until a line is available. Because the extraction is separate from `recv`,
it is unit-tested with hand-fed fragments, and the socket reader is
unit-tested over an `AF_UNIX` `socketpair`.

`RECEIVE_BUFFER_SIZE` is 4096. The longest legal message is the BYE line:
`cs230 ` + 64 hex characters + ` BYE\n` = 75 bytes; a STATUS line with two
20-digit operands is under 60. A line that fills the buffer without a newline
is a protocol error, not a reason to grow the buffer.

## 3. Protocol handling

Messages are classified by two parsers that both return `bool`:

- `parse_status(line, &problem)` accepts exactly
  `cs230 STATUS <num> <op> <num>` with single spaces, `<op>` one of `+ - * /`,
  operands parsed by `strtoll` with `endptr` and `errno == ERANGE` checks.
  Leading whitespace before an operand is rejected explicitly because
  `strtoll` would otherwise skip it; an explicit `+` or `-` sign is accepted
  because `strtoll` accepts it and nothing is gained by refusing `+5`; trailing
  text after the second operand is rejected. The spec says protocols are
  exact, so the parser is exact everywhere a leniency could change an answer.
- `parse_bye(line, flag, capacity)` accepts `cs230 <flag> BYE` where the
  flag is a non-empty token with no spaces. The flag is not required to be 64
  characters: the spec's example is 64 hex characters, but the autograder runs
  its own server and nothing is gained by rejecting a different length.

Any line that is neither is a protocol error: it is printed to `stderr` and
the client exits `EXIT_FAILURE`. The first message after HELLO is a STATUS per
the spec, so no special case is needed for it. Skipping unknown lines instead
was considered and rejected: a client that silently ignores what it does not
understand hides exactly the bugs this protocol's exactness is meant to expose.

Operands or results outside `long long` are also protocol errors. The server
could in principle send bigger numbers, but the spec gives no range, the
example operands are three digits, and answering with a wrong number is worse
than refusing.

### Arithmetic

`long long` throughout. C99 6.5.5 guarantees that `/` truncates toward zero,
which is exactly what the spec asks for (`200 / 3` → `66`). Division by zero
and `LLONG_MIN / -1` are undefined behaviour in C, so `evaluate` refuses them
and the client treats the problem as a protocol error. Addition, subtraction
and multiplication are guarded against overflow with the portable
pre-checks from CERT INT32-C (adapted to `long long`); the server's operands
are small in practice, but the guard costs nothing and keeps `-fanalyzer`
and a reviewer happy.

The answer is formatted with `snprintf("%s%lld\n", PROTOCOL_PREFIX, answer)`
(the prefix carries its own trailing space) into a fixed buffer and sent with `send_all`, which loops until every byte is
written because `send` on a stream socket may write fewer bytes than asked.

### Session loop

```
send HELLO
loop:
    line = receive_line()            -> EOF: "server closed the connection" -> FAILURE
    if parse_bye: print flag, SUCCESS
    if parse_status: evaluate, send answer, continue
    else: protocol error -> FAILURE
```

`receive_line` distinguishes three outcomes (`RECEIVE_LINE`, `RECEIVE_EOF`,
`RECEIVE_ERROR`). EOF before BYE is the server's way of saying the last
answer was wrong, so the message says so. One exception: if the bytes left in
the buffer at EOF form a BYE line that merely lacked its final newline, the
flag is on the wire and is printed; a buffered STATUS fragment at EOF is not
answered, because nobody is listening.

No receive timeout is set: the spec itself says a slow run "may not be wrong",
and a stalled server is visible with `MATHBOT_VERBOSE=1`.

## 4. Sockets

`socket(AF_INET, SOCK_STREAM, 0)`, a zeroed `struct sockaddr_in` with
`sin_family = AF_INET`, `sin_port = htons(port)`, `sin_addr` filled by
`inet_pton`, then `connect`. Every return value is checked; failures print
`strerror(errno)` to `stderr`. The socket is closed on every exit path from
`run_session`, which is why `main` owns the descriptor and `run_session`
only borrows it.

`SIGPIPE` is ignored with `signal(SIGPIPE, SIG_IGN)` at the start of `main`.
If the server drops the connection after a wrong answer and the client's next
`send` races it, the default disposition would kill the process silently
with no message and a signal exit status; ignoring it turns that into an
`EPIPE` error from `send` that the code reports. `signal` is in C99
`<signal.h>` and is portable, unlike the Linux-only `MSG_NOSIGNAL` flag.

`recv` with `EINTR` is retried; no signal handlers are installed, so this is
belt and braces.

## 5. Verbose tracing

The spec's first hint is to print what is sent and received. Setting
`MATHBOT_VERBOSE=1` in the environment makes the client echo each line
received (`<< …`) and each line sent (`>> …`) on `stderr`. It is off by
default so `stdout` carries only the flag.

## 6. Portability of the single file

The file is compiled two ways: with the strict flags
(`-std=c99 -D_POSIX_C_SOURCE=200809L …`) and with plain `gcc client.c`
(gnu17, no feature macros). The headers the spec lists plus `<errno.h>`,
`<limits.h>`, `<signal.h>`, `<stdbool.h>` are all available under both.
`inet_pton`, `htons`, `struct sockaddr_in`, `socket`, `connect`, `send`,
`recv`, `close` and `strtoll` are declared by `_POSIX_C_SOURCE=200809L`.
Nothing GNU-specific is used, so no `_GNU_SOURCE`. The file `#define`s nothing
itself so it behaves identically under both compiles. `make dist` proves both
builds.

## 7. Function inventory (all `static` except `main`)

| Function | Responsibility |
| --- | --- |
| `print_usage` | usage line on stderr |
| `contains_whitespace`, `equals_ignoring_case` | string helpers for validation |
| `is_valid_identification` | NetID@umass.edu check (domain case-insensitive, no whitespace, one `@`) |
| `parse_port` | string → 1..65535 |
| `parse_host` | dotted quad → `struct in_addr` via `inet_pton` |
| `parse_arguments` | argc/argv → `ClientArguments`, reason on stderr |
| `line_buffer_append` | add received bytes, false on overflow |
| `copy_line_out`, `line_buffer_drop` | copy a line out (dropping `\r`), shift the rest down |
| `line_buffer_take_line` | extract one complete line |
| `line_buffer_take_remainder` | the unterminated tail after EOF |
| `receive_line` | `recv` loop feeding the buffer; EINTR retried |
| `send_all` | `send` loop over short writes |
| `send_line` | send one line, with verbose echo |
| `send_hello` | format and send the HELLO line |
| `close_socket` | `close` with the failure reported |
| `connect_to_server` | socket + sockaddr_in + connect, socket closed on failure |
| `parse_operand`, `is_operator`, `parse_operator` | strict tokenising of a STATUS line |
| `parse_status` | STATUS line → `MathProblem` |
| `parse_bye` | BYE line → flag |
| `add_checked`, `subtract_checked`, `multiplication_fits`, `multiply_checked`, `divide_checked` | overflow-guarded arithmetic |
| `evaluate` | dispatch on the operator |
| `report_early_disconnect` | the EOF-before-BYE message |
| `handle_status` | evaluate and answer one problem |
| `print_flag` | flag on stdout, `printf` and `fflush` checked |
| `handle_line` | STATUS → answer, BYE → flag, else protocol error |
| `handle_eof` | accept a BYE tail at EOF, otherwise early disconnect |
| `run_session` | HELLO then the receive/answer loop |
| `is_verbose_enabled` | `MATHBOT_VERBOSE` |
| `main` | signal, arguments, connect, session, close |

## 8. Testing strategy

- `test/unit/test_client.c` includes `src/client.c` with `main` renamed, so
  every static function above is called directly: argument validation, line
  buffer behaviour with split and coalesced input, STATUS parsing including
  rejection of extra whitespace and trailing garbage, arithmetic including
  truncation toward zero for negative results and overflow refusal, BYE
  parsing, and `receive_line` over a `socketpair` fed byte by byte.
- `test/e2e/mock_server.py` (standard library only, run with `python3 -I`)
  speaks math speak on an ephemeral loopback port, writes the port to a file,
  sends a seeded random number of problems in 300..2000, can fragment writes
  with `TCP_NODELAY` and short pauses, can pipeline two STATUS lines in one
  write, drops the connection on a wrong answer, and ends with a 64-hex flag
  derived from `sha256(identification)`.
- e2e scripts cover: happy path (flag on stdout, exit 0), fragmented and
  pipelined messages (same flag), wrong-answer disconnect (server set to
  reject; exit non-zero, nothing on stdout, message on stderr), garbage line
  (protocol error), division by zero (refused), bad arguments (each argument
  wrong in turn), and connection refused (a closed port).

## 9. Deviations from the conventions

- The Makefile says `CC = gcc`, not `CC ?= gcc`: GNU make predefines `CC` as
  `cc`, so `?=` would never take effect and the gate would print `cc`. A
  command-line `make CC=clang` still overrides it, which is what the rule wants.
- `test/e2e/cases/` is empty and therefore absent from git: the client reads
  nothing from stdin, so there are no stdin scripts; the e2e scripts assert on
  the binary's stdout, stderr and exit status against the mock server instead.
- Diagnostics written with `fprintf(stderr, …)` are not return-checked. There
  is nothing to do when stderr is broken; the system calls and the stdout
  `printf`/`fflush` that carry the flag are all checked.
- `close` failing after a completed session is reported but does not change
  the exit status: the flag was captured, which is the rubric item.

The spec says no README or Makefile needs to be submitted; both are produced
anyway because the conventions ask for them and they cost nothing. `make dist`
runs `make -C dist`, which the conventions themselves prescribe as the proof
that the bundle builds; it is the one place a nested make appears.
