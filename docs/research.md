# Research notes for the Math Bot client

Each section records the sources consulted, what they established, the
practices this `client.c` adopts because of them, and the pitfalls the code
is written to avoid. Local findings were verified on gcc 16.2.1 / glibc
(Fedora); the course Vagrant box is older, but every construct below is
POSIX.1-2008 or ISO C99, so nothing depends on a recent toolchain.

## 1. The TCP client call sequence: socket, connect, send, recv, close

### Sources

- Beej's Guide, "A Simple Stream Client": https://beej.us/guide/bgnet/html/split/client-server-background.html
- Beej's Guide, "IP Addresses, structs, and Data Munging": https://beej.us/guide/bgnet/html/split/ip-addresses-structs-and-data-munging.html
- socket(2): https://man7.org/linux/man-pages/man2/socket.2.html
- connect(2): https://man7.org/linux/man-pages/man2/connect.2.html
- inet_pton(3): https://man7.org/linux/man-pages/man3/inet_pton.3.html
- close(2): https://man7.org/linux/man-pages/man2/close.2.html

### What was learned

- `int socket(int domain, int type, int protocol)` with `AF_INET, SOCK_STREAM, 0`
  returns a descriptor or -1 with `errno`.
- `struct sockaddr_in` is `{ sin_family; sin_port; struct in_addr sin_addr; sin_zero[8] }`.
  `sin_port` must be in network byte order (`htons`); Beej: `sin_zero` "should
  be set to all zeros with the function memset()". A `struct sockaddr_in *`
  is cast to `struct sockaddr *` for `connect`.
- `int inet_pton(int af, const char *restrict src, void *restrict dst)` returns
  1 on success, 0 if `src` is not a valid address, -1 with `EAFNOSUPPORT`.
  Only `== 1` is success. Verified locally: it rejects leading zeros
  (`010.1.1.1`), octets over 255, too few octets, and any leading, trailing
  or newline whitespace, so no pre-trimming or extra validation is needed.
- `connect()` returns 0 or -1; `ECONNREFUSED` means "found no one listening
  on the remote address". After a failed `connect` the socket state is
  unspecified; portable code closes it.
- Beej's example `exit(1)`s inside the connected section without `close()`.
  The kernel reclaims the descriptor, but this code does not rely on that.

### Practices adopted in this code

- One `connect_to_server(const char *ip, uint16_t port)` does `socket`,
  `memset` the address, `sin_family = AF_INET`, `sin_port = htons(port)`,
  `inet_pton(...) == 1`, `connect`. On any failure after `socket` succeeded
  it closes the descriptor and returns -1 with the failing call named on
  `stderr` via `strerror(errno)`.
- The IP string is validated with `inet_pton` during argument checking,
  before any socket exists, so a bad address is a usage error.
- `main` has one exit path after the session: `close(fd)` runs exactly once
  whether the session ended with BYE, EOF, or a protocol error. `close()`'s
  return value is checked and reported.

### Pitfalls avoided

- Treating `inet_pton` as boolean (`if (inet_pton(...))`), which accepts -1.
- Forgetting `htons`: port 27993 goes out byte-swapped as 27033.
- Reusing a socket after a failed `connect` (the program exits instead).

## 2. Partial reads and line framing over TCP

### Sources

- Beej's Guide, "Son of Data Encapsulation": https://beej.us/guide/bgnet/html/split/slightly-advanced-techniques.html
- recv(2): https://man7.org/linux/man-pages/man2/recv.2.html

### What was learned

- A stream socket has no message boundaries. Beej: "you might have read past
  the end of one packet and onto the next in a single recv() call", and a
  message can equally arrive in pieces. The remedy is a work buffer that is
  appended to after each `recv`, scanned for a complete message, consumed,
  and compacted (leftover moved to the front).
- `ssize_t recv(int sockfd, void *buf, size_t len, int flags)` "normally
  return[s] any data available, up to the requested amount" rather than
  waiting for the full amount. 0 means "a stream socket peer has performed an
  orderly shutdown (the traditional end-of-file return)"; -1 is an error with
  `errno`, where `EINTR` means "interrupted by delivery of a signal before
  any data was available".

### Practices adopted in this code

- `LineBuffer { char data[LINE_BUFFER_SIZE]; size_t used; }` with
  `LINE_BUFFER_SIZE = 4096`. The longest protocol line is about 75 bytes
  (`cs230 ` + 64 hex + ` BYE\n`); 4096 is a 50x margin and still one page.
- `read_line(fd, &buffer, line, line_cap)`: first `take_line` (memchr for
  `'\n'` in `data[0..used)`, copy out, NUL-terminate, `memmove` the
  remainder, decrement `used`); only when no complete line is present call
  `recv(fd, data + used, sizeof data - used, 0)`. This answers a STATUS line
  exactly once however it is fragmented or coalesced, the project's named
  review-focus failure mode.
- Return convention: 1 = line delivered, 0 = EOF with no complete line,
  -1 = error. `recv` returning 0 before BYE is reported as "server closed the
  connection" and the program exits non-zero; the spec says that is what a
  wrong answer looks like.
- `used == sizeof data` with no newline is a protocol error (line longer than
  the buffer): report and exit non-zero rather than truncate.
- `EINTR` on `recv` retries. No handlers are installed (section 3 ignores
  SIGPIPE rather than catching it), so this path is not expected to run.
- The buffer is zero-initialised (`= {{0}, 0}`); section 6 explains why.

### Pitfalls avoided

- One `recv` per message with `buf[n] = '\0'` and `strtok`: fails on the
  first fragmented STATUS and answers two coalesced problems with one reply,
  which the server treats as a wrong answer.
- `strchr` on a buffer that is not NUL-terminated; `memchr` with `used` is
  used instead.
- Passing `sizeof data` instead of `sizeof data - used` to `recv`.

## 3. Short sends and SIGPIPE

### Sources

- Beej's Guide, "Handling Partial send()s": https://beej.us/guide/bgnet/html/split/slightly-advanced-techniques.html
- send(2): https://man7.org/linux/man-pages/man2/send.2.html
- POSIX send(): https://pubs.opengroup.org/onlinepubs/9699919799/functions/send.html
- signal(7): https://man7.org/linux/man-pages/man7/signal.7.html
- glibc `bits/socket.h` on this machine (the `MSG_NOSIGNAL` enumerator)

### What was learned

- `send()` "return[s] the number of bytes sent", possibly fewer than asked;
  Beej's `sendall()` loops over the unsent remainder until done or -1. The
  spec forbids any stray byte, so an incomplete send is a protocol failure.
- Writing to a socket whose peer has closed raises `SIGPIPE`, default action
  "Term". send(2): "the process will also receive a SIGPIPE unless
  MSG_NOSIGNAL is set"; `EPIPE` is still returned with the flag.
- `MSG_NOSIGNAL` is Linux since 2.2 and was added to POSIX in Issue 7
  (POSIX.1-2008). In glibc it is an enumerator in `bits/socket.h` with no
  `__USE_GNU` guard, and it compiled here under
  `-std=c99 -D_POSIX_C_SOURCE=200809L -Wpedantic -Werror`. It does not
  exist on macOS (`SO_NOSIGPIPE` there), and the autograder's toolchain
  cannot be verified.
- `signal(SIGPIPE, SIG_IGN)` is ISO C (`<signal.h>`), compiles under plain
  `-std=c99` with no feature macro (verified), and with the disposition set
  to ignore no handler runs, so nothing is interrupted and `send` simply
  fails with `EPIPE`.

### Practices adopted in this code

- `signal(SIGPIPE, SIG_IGN)` once at the top of `main`; `send` uses
  `flags = 0`. Reasoning: the deliverable is one file compiled by an unknown
  `gcc` on the autograder with no flags of ours. A missing macro is a compile
  error and a zero; `signal()` cannot fail to compile anywhere. The program
  owns a single socket, so a process-wide disposition has no side effects,
  and for `SIG_IGN` `signal` is fully specified by C99 7.14.1.1.
- `send_all(fd, buf, len)`: loop on `send(fd, buf + sent, len - sent, 0)`,
  retry on `EINTR`, return -1 on any other error, succeed only when
  `sent == len`. `EPIPE`/`ECONNRESET` are reported as "server closed the
  connection", exit non-zero.
- Replies are built in a fixed `char reply[64]` with `snprintf`, the return
  value checked against the buffer size, then sent with one `send_all`.

### Pitfalls avoided

- `send(fd, msg, strlen(msg), 0)` without checking the count.
- Diagnostics on stdout mixed with the flag: everything but the flag,
  including the `MATHBOT_VERBOSE` trace, goes to `stderr`.
- Dying silently from `SIGPIPE` with exit status 141 and no message.

## 4. Integer arithmetic for the protocol

### Sources

- ISO C99 draft n1256, 6.5p5 and 6.5.5p5-6: https://www.open-std.org/jtc1/sc22/wg14/www/docs/n1256.pdf
- strtol(3)/strtoll(3): https://man7.org/linux/man-pages/man3/strtol.3.html
- CERT INT32-C: https://cmu-sei.github.io/secure-coding-standards/sei-cert-c-coding-standard/rules/integers-int/int32-c
- CERT INT33-C: https://cmu-sei.github.io/secure-coding-standards/sei-cert-c-coding-standard/rules/integers-int/int33-c

### What was learned

- C99 6.5.5p6: "When integers are divided, the result of the / operator is
  the algebraic quotient with any fractional part discarded", footnote 90:
  "This is often called 'truncation toward zero'". So `200 / 3 == 66` and
  `-7 / 2 == -3` are guaranteed by the language (verified), which is exactly
  the spec's "truncated" requirement; no `double` or `floor` is involved.
- 6.5.5p5: "if the value of the second operand is zero, the behavior is
  undefined." 6.5p5: a result "not in the range of representable values for
  its type" is undefined behaviour, covering signed overflow of `+ - *` and
  the one non-obvious division case `LLONG_MIN / -1`.
- `long long strtoll(const char *nptr, char **endptr, int base)`: skips
  leading whitespace, accepts an optional sign, stores the first unconverted
  character in `*endptr` (`*endptr == nptr` if no digits), returns
  `LLONG_MAX`/`LLONG_MIN` with `errno = ERANGE` on overflow, and "does not
  modify errno on success", so `errno = 0` must precede the call. Verified:
  `"12abc"` -> 12 with rest `"abc"`, `""` -> 0 consumed,
  `"9223372036854775808"` -> `ERANGE`.
- CERT INT32-C's portable multiplication precondition is a four-way sign
  split: `a > LLONG_MAX / b` (both positive), `b < LLONG_MIN / a` (`a` > 0,
  `b` <= 0), `a < LLONG_MIN / b` (`a` <= 0, `b` > 0), `a != 0 && b < LLONG_MAX / a`
  (both <= 0). Addition: `(b > 0 && a > LLONG_MAX - b) || (b < 0 && a < LLONG_MIN - b)`;
  subtraction mirrors it; division: `b == 0 || (a == LLONG_MIN && b == -1)`.

### Practices adopted in this code

- Operands and results are `long long`. The spec's `505 * 700` suggests
  operands fit in `int`, whose product always fits in 64 bits, so the guards
  are not expected to fire against the course server; they exist because an
  unguarded `/ 0` or overflow is undefined behaviour, not merely a wrong
  answer, and the mock server exercises them.
- `parse_operand(const char *token, long long *out)`: reject an empty token,
  `errno = 0`, `strtoll(token, &end, 10)`, fail on `end == token`,
  `*end != '\0'`, or `errno == ERANGE`.
- `evaluate(long long a, char op, long long b, long long *out)` returns -1
  for an unknown operator, division by zero, `LLONG_MIN / -1`, or overflow
  detected with the INT32-C preconditions (no `__builtin_*_overflow`, which
  an old `gcc` on the Vagrant box might lack). Failure is a protocol error,
  exit non-zero, never a wrong answer sent.
- STATUS parsing splits on single spaces into exactly five tokens
  `cs230 STATUS A OP B`, OP being one character from `+-*/`. The tokenizer
  hands `strtoll` a token that starts at a digit or `-`, so `strtoll`'s
  acceptance of leading whitespace cannot admit a malformed line.

### Pitfalls avoided

- `atoi`/`atol`: no error reporting, undefined on overflow.
- `sscanf("%lld %c %lld")`: accepts trailing garbage, undefined on overflow.
- Computing in `int`, or in `double` with rounding (`66.67` -> `67`).

## 5. Validating command-line arguments

### Sources

- strtol(3) and inet_pton(3) as above; the spec's "Program Requirements".

### What was learned

- `strtol` behaves like `strtoll`; checking `errno`, `end == arg`,
  `*end == '\0'` and the range is the whole recipe.
- Port 0 means "pick an ephemeral port" and is meaningless for `connect`;
  valid TCP ports are 1..65535 and `sin_port` is `uint16_t`.
- `inet_pton` is the only validation an IPv4 literal needs (section 1).

### Practices adopted in this code

- `argc != 4` -> usage line on `stderr`, `EXIT_FAILURE`. Argument order is
  id, port, host exactly as the spec demands.
- `parse_port(const char *s, uint16_t *out)`: first character must be a
  digit (so `" 80"` and `"-1"` fail before `strtol` could accept them),
  `errno = 0`, `strtol`, reject `ERANGE`, `*end != '\0'`, or a value outside
  1..65535; store `(uint16_t)value`.
- `parse_ip(const char *s, struct in_addr *out)`: `inet_pton(AF_INET, s, out) == 1`.
- `validate_id(const char *s)`: `strstr(s, "@umass.edu")` found at index
  greater than 0 (non-empty NetID) and no whitespace anywhere, because the
  id is spliced into a `\n`-terminated protocol line. The HELLO line is
  built with `snprintf` and its length checked, so an over-long id is a
  usage error, not a truncated message.

### Pitfalls avoided

- Accepting port `0`, `65536`, `27993abc` or `""`.
- An id with an embedded newline, which would send two lines.
- Validating after `socket()`/`connect()` and leaking the descriptor;
  validation runs first and owns no resources.

## 6. Headers and feature-test macros for one file under two compilers

### Sources

- feature_test_macros(7): https://man7.org/linux/man-pages/man7/feature_test_macros.7.html
- GCC warning options: https://gcc.gnu.org/onlinedocs/gcc/Warning-Options.html
- GCC static analyzer options: https://gcc.gnu.org/onlinedocs/gcc/Static-Analyzer-Options.html
- GCC bugzilla, analyzer fd-leak false positives on socket code: https://gcc.gnu.org/bugzilla/show_bug.cgi?id=108648
- Local probes with gcc 16.2.1 (below).

### What was learned

- `-std=c99` makes gcc define `__STRICT_ANSI__`; glibc then omits
  `_DEFAULT_SOURCE`, so ISO headers expose only ISO C. `-D_POSIX_C_SOURCE=200809L`
  exposes POSIX.1-2008 and implies C99. Plain `gcc` is a GNU dialect (gnu17
  on older releases, gnu23 on gcc 16 here) with `_DEFAULT_SOURCE` on, a
  strict superset.
- Verified: `struct sockaddr_in`, `htons`, `inet_pton`, `MSG_NOSIGNAL`,
  `ssize_t`, `send`, `recv`, `connect`, `close` all compile under
  `-std=c99 -D_POSIX_C_SOURCE=200809L -Wall -Wextra -Wpedantic -Wshadow
  -Wstrict-prototypes -Wmissing-prototypes -Wconversion -Wvla -Werror` and
  under plain `gcc file.c`. `ssize_t` comes from `<sys/types.h>` (also via
  `<sys/socket.h>`); `uint16_t` arrives via `<netinet/in.h>` but
  `<stdint.h>` is included explicitly.
- `-Wconversion` (which enables `-Wsign-conversion` in C) rejects
  `size_t n = recv(...)` ("may change the sign of the result") and
  `htons(int_port)` ("conversion from int to uint16_t may change value").
- `-fanalyzer` false positive, reproduced: with `char buf[4096]; size_t used = 0;`
  and `memchr(buf, '\n', used)` before the first `recv`, the analyzer reports
  "use of uninitialized value 'buf'" at the later `memcpy`, because it does
  not model that `memchr` with length 0 returns NULL. Zero-initialising the
  buffer (`char buf[N] = {0}` or `LineBuffer b = {{0}, 0}`) makes the run
  clean; reordering the loop to recv-first does not. Bugzilla 108648 shows
  the analyzer's descriptor-leak tracking is also imperfect for sockets held
  in structs, a reason to keep the descriptor in a plain local `int`.

### Practices adopted in this code

- Includes: `<stdio.h> <stdlib.h> <string.h> <errno.h> <limits.h> <signal.h>
  <stdint.h> <sys/types.h> <sys/socket.h> <netinet/in.h> <arpa/inet.h>
  <unistd.h>`; the spec's minimum list is a subset. No `#define
  _POSIX_C_SOURCE` in the source: the Makefile passes it, plain `gcc` does
  not need it.
- Every `send`/`recv` result is held in `ssize_t`, tested `< 0` and `== 0`
  first, then converted with an explicit `(size_t)n`.
- The port is a `uint16_t` from `parse_port` onward, so `htons` receives the
  type it is declared with.
- The receive buffer is a zero-initialised struct local to `main`; the
  socket is a local `int`. `make check` runs `-fanalyzer` with the full
  warning set and must be clean without pragmas.
- Every non-`main` function is `static` and defined with a prototype.

### Pitfalls avoided

- `#define _GNU_SOURCE` in the source to "fix" a missing declaration.
- Implicit `ssize_t`->`size_t` and `int`->`uint16_t` conversions.
- `#pragma GCC diagnostic` to silence the analyzer instead of removing the
  pattern that confuses it.

## 7. Testing a network client with a mock server

### Sources

- Python socketserver: https://docs.python.org/3/library/socketserver.html
- Python socket (`sendall`, `send`, `setsockopt`, `TCP_NODELAY`): https://docs.python.org/3/library/socket.html
- Python `-I` isolated mode: https://docs.python.org/3/using/cmdline.html

### What was learned

- `socketserver.TCPServer(("127.0.0.1", 0), Handler)` binds an ephemeral
  port ("Port 0 means to select an arbitrary unused port"); the chosen
  address is `server.server_address`. `StreamRequestHandler.rfile.readline()`
  loops on `recv` until a newline because "TCP is stream-based".
  `handle_request()` serves exactly one connection, which each e2e case needs.
- `socket.sendall` sends until everything is out; `socket.send` may send
  fewer bytes. Coalescing two messages is one `sendall(msg1 + msg2)`;
  fragmenting is `send(prefix)`, a short `sleep`, `send(rest)`.
- `TCP_NODELAY` (present in Python's `socket`, verified) disables Nagle,
  which would otherwise hold the second small fragment until the first is
  ACKed; cheap insurance over up to 2000 round trips.
- A split is visible to the client only if its `recv` runs between the two
  sends; the sleep makes that overwhelmingly likely, not certain. The
  deterministic proof is the unit test that feeds `take_line` byte by byte
  and in coalesced chunks; the mock is an integration check.
- `python3 -I` ignores `PYTHON*` variables and keeps the script directory
  and user site-packages off `sys.path`; the mock needs only the standard
  library.

### Practices adopted in this code

- `test/e2e/mock_server.py` binds `127.0.0.1:0`, prints the port on its
  first stdout line and flushes, then `handle_request()`s one client. The
  shell driver reads that line, runs `build/client id port 127.0.0.1` under
  `timeout`, and compares the printed flag with the one the mock derives
  from the id (`sha256(id)` hex, 64 characters, matching the spec's format).
- The problem count comes from `random.Random(MATHBOT_SEED)` in 300..2000;
  operands include negatives; a fixed fraction of STATUS lines are sent
  fragmented, another fraction coalesced with the following STATUS.
- Wrong answer: the mock closes the socket without BYE; the e2e case asserts
  the client exits non-zero with a "server closed" message. A wrong-answer
  mode is driven by an environment variable so the mock misbehaves, not the
  client.
- Refused connection: the driver obtains a closed port by binding and
  immediately closing a socket in Python, then runs the client against it.

### Pitfalls avoided

- A hard-coded port colliding with another process or a `TIME_WAIT` socket.
- A mock that `recv(1024)`s once and assumes a whole line; `readline` also
  tolerates a client that fragments.
- Tests whose correctness depends on wall-clock timing; timing only makes
  the fragmentation likely, correctness is asserted by the unit test.

## 8. SHA-256 for the flag (server)

### Sources

- FIPS PUB 180-4, Secure Hash Standard, sections 4.1.2 (functions), 4.2.2
  (constants), 5.1.1 (padding), 5.3.3 (initial hash value), 6.2 (SHA-256):
  https://nvlpubs.nist.gov/nistpubs/FIPS/NIST.FIPS.180-4.pdf
- NIST CAVP example vectors for SHA-256 (`abc`, the two-block message):
  https://csrc.nist.gov/projects/cryptographic-standards-and-guidelines/example-values
- Python `hashlib` (local cross-check of every non-standard vector):
  https://docs.python.org/3/library/hashlib.html

### What was learned

- The message is padded with one `0x80` byte, zeros, and the 64-bit
  big-endian bit length so the total is a multiple of 64 bytes; when fewer
  than 8 bytes remain after the `0x80`, a whole extra block is needed
  (lengths 56..63 mod 64). The 55-, 56- and 64-byte inputs sit exactly on
  those seams.
- Each block expands into a 64-word schedule; 64 rounds mix eight working
  variables with `Ch`, `Maj`, two big and two small sigma functions and the
  64 round constants (fractional parts of cube roots of the first 64 primes);
  the result is added into the running state. Everything is 32-bit modular
  arithmetic on `uint32_t`, so `-Wconversion` only needs casts at the
  byte/word boundaries.
- Standard results: `sha256("") = e3b0c442…b855`,
  `sha256("abc") = ba7816bf…15ad`,
  `sha256("abcdbcde…nopq") = 248d6a61…06c1`.

### Practices adopted in this code

- `Sha256 { state[8]; byte_length; block[64]; block_used; }` with
  `sha256_init`, `sha256_update` (compress on every full block),
  `sha256_final` (pad, length, final compress, big-endian output). The
  constants are `static const` locals inside the two functions that use them,
  keeping the file free of globals.
- The flag is `hex(sha256(secret || id))`; `MATHBOT_SECRET` empty by default
  so the test helpers' `sha256(id)` stays valid.
- Unit tests check the three standard vectors, the 55/56/64-byte padding
  seams and a 1000-byte input fed in uneven pieces, all against `hashlib`.

### Pitfalls avoided

- Forgetting the extra padding block when 56..63 bytes are pending (the
  classic off-by-one that passes `abc` and fails on longer input).
- Counting the length in bytes where the spec wants bits.
- Rotations written on `int` (undefined for the sign bit) instead of `uint32_t`.

## 9. A forking TCP server: fork, SIGCHLD, clean shutdown

### Sources

- Beej's Guide, "A Simple Stream Server" (fork per connection, `sigaction`
  for SIGCHLD, `SO_REUSEADDR`):
  https://beej.us/guide/bgnet/html/split/client-server-background.html
- fork(2): https://man7.org/linux/man-pages/man2/fork.2.html
- sigaction(2): https://man7.org/linux/man-pages/man2/sigaction.2.html
- waitpid(2): https://man7.org/linux/man-pages/man2/wait.2.html
- signal-safety(7): https://man7.org/linux/man-pages/man7/signal-safety.7.html
- accept(2): https://man7.org/linux/man-pages/man2/accept.2.html
- socket(7), `SO_RCVTIMEO`: https://man7.org/linux/man-pages/man7/socket.7.html

### What was learned

- After `fork` both processes hold the accepted socket and the listener; the
  child closes the listener, the parent closes the accepted socket, or the
  connection never reaches EOF for the client.
- A child that exits stays a zombie until waited for. Beej's handler
  `while (waitpid(-1, NULL, WNOHANG) > 0);` reaps every finished child in one
  go; `waitpid` is async-signal-safe and `errno` must be saved and restored
  in the handler because it is clobbered. `SA_NOCLDSTOP` keeps stopped
  children from raising the signal; `SA_RESTART` on the SIGCHLD action keeps
  `accept` from returning `EINTR` on every session end.
- `SIGINT`/`SIGTERM` handlers must only set a `volatile sig_atomic_t`.
  Without `SA_RESTART`, a blocking `accept` returns `EINTR` when they run, so
  the loop can check the flag. There is still a window between the check and
  the call in which a signal is missed; the classic cures are a self-pipe or
  `pselect`. socket(7) documents `SO_RCVTIMEO` for calls that "perform socket
  I/O", and Linux applies it to `accept` as well: verified locally on kernel
  7.2 with a 10-line program (`accept` returned `EAGAIN` after 1 s). A
  one-second timeout on the listener turns the window into a one-second delay.
- `SO_REUSEADDR` lets `bind` succeed while the previous instance's
  connections are in `TIME_WAIT` (Beej: "Address already in use").
- `fork` duplicates stdio buffers; unflushed output would be written twice.
  Flushing after every log line keeps the buffers empty at `fork`.

### Practices adopted in this code

- `install_handler` wraps `sigaction` (memset, `sigemptyset`, flags) and
  checks both calls; `install_signal_handlers` for the parent,
  `restore_default_signals` in the child.
- Parent: `accept` loop polling `shutdown_requested`, `EAGAIN`/`EINTR`/
  `ECONNABORTED` are transient, anything else is fatal and exits 1.
- Child: close listener, restore signals, set the 30 s `SO_RCVTIMEO`, serve,
  close, `exit(EXIT_SUCCESS)`.
- The server binds `0.0.0.0` so a published container port reaches it.

### Pitfalls avoided

- Calling `printf` or `malloc` in a signal handler.
- Reaping with `wait()` (blocks) or reaping one child per signal (signals
  coalesce, zombies accumulate).
- Keeping the listener open in the child, which would keep the port busy
  after the parent exits.
- Letting a stalled client hold a child forever: `SO_RCVTIMEO` on every session.
