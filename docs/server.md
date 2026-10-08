# The local math-speak server

The course's test server (128.119.243.147:27993) and the Gradescope autograder
no longer exist. `server/mathbot_server.c` is a local replacement that speaks
the protocol in `docs/spec.md` exactly as the spec describes the real server,
so `src/client.c` can be run, graded and demonstrated without either. It is
never submitted; the deliverable is still `src/client.c` alone.

## 1. What it does, next to the spec

| Spec says the server… | The local server |
| --- | --- |
| expects `cs230 HELLO <NETID>@umass.edu\n` first, "exactly as we describe" | first line must be byte-exactly `cs230 HELLO <id>` + `\n`; the id is one non-empty token without whitespace ending in `@umass.edu` (the domain compared case-insensitively). Anything else closes the connection without a reply. |
| sends `cs230 STATUS NUM OP NUM\n` | operands in -1000..1000, `OP` in `+ - * /`, a divisor never 0. The real server's range is unknown; the mock already used this one and the client is tested with it, so negative operands are a deliberate superset (section 4). |
| expects `cs230 <ANSWER>\n`, "will drop connection if it is wrong" | the reply must be byte-exactly `cs230 <answer>`; division truncates toward zero (`200 / 3` → `66`, `-7 / 2` → `-3`). A wrong, padded or `\r`-terminated answer closes the connection without BYE. |
| repeats "no less than 300, but no more than 2000" times | a per-connection random count in 300..2000, seeded from the time and the child's pid; `MATHBOT_PROBLEMS=n` fixes the count and `MATHBOT_SEED=n` the problems for deterministic runs |
| ends with `cs230 <FLAG> BYE\n`, a 64-byte flag "unique to your NetID" | `FLAG` = lowercase hex SHA-256 of `MATHBOT_SECRET` followed by the id. With the default empty secret this is `sha256(id)`, the same value `test/e2e/mock_server.py` and `expected_flag` in `test/e2e/lib.sh` compute. |
| serves many students at once | one forked child per connection, no limit; the parent only accepts |

Lines are framed on `\n` across `recv` boundaries with the same scheme as
the client's `LineBuffer`, so a client that fragments or pipelines its
replies is served correctly. The server is strict where the client is
lenient: it does not strip `\r`, it does not accept a `+` sign on an answer,
and a line longer than 4096 bytes or holding a NUL byte (invisible to a
string compare) closes the connection as `malformed line`.

## 2. Running it

Natively, from the repository root:

```
make server              # build/mathbot_server
make server-run          # serves on PORT (default 27993) until Ctrl-C
make server-run PORT=5000
./build/mathbot_server 0 # port 0: the kernel picks one, announced on stdout
```

Usage is `mathbot_server [PORT]`. The port comes from the argument, else the
`MATHBOT_PORT` variable, else 27993. The server binds `0.0.0.0`, so it is
reachable from other hosts and from outside a container. The first stdout
line is always `listening on port N`; after that there is one line when a
session connects and one when it ends, each prefixed with the peer address:

```
listening on port 27993
127.0.0.1:54952 connected
127.0.0.1:54952 flag sent
127.0.0.1:54968 connected
127.0.0.1:54968 closed: bad HELLO
shutting down
```

The outcomes are `flag sent`, `closed: bad HELLO`, `closed: wrong answer`,
`closed: client hung up`, `closed: timed out`, `closed: malformed line` and
`closed: socket error`. Only the last one is the server's own failure; it is
the only outcome for which the child exits 1 (the parent discards the status
either way).

Then run the client against it from another terminal:

```
./build/client you@umass.edu 27993 127.0.0.1
```

`SIGINT` or `SIGTERM` makes the server close its listening socket, print
`shutting down` and exit 0. Sessions already in progress run to completion in
their own processes.

### Environment variables

| Variable | Default | Effect |
| --- | --- | --- |
| `MATHBOT_PORT` | `27993` | port when no argument is given |
| `MATHBOT_SECRET` | empty | prepended to the id before hashing. Set it to anything non-empty and the flags stop being `sha256(id)`, so nobody can precompute them. The test suite relies on the default. |
| `MATHBOT_PROBLEMS` | unset | fixes the number of problems per session (1..100000) instead of drawing 300..2000 |
| `MATHBOT_SEED` | unset | seeds every session's problem generator with this value (1..4294967295), so every session asks the same problems in the same order; the conventions' `<PROJECT>_SEED` rule. Without it each session seeds from the time and its pid. |

```
MATHBOT_SECRET='something long' make server-run
```

### In Docker

The image was authored in a sandbox where the Docker daemon was not
reachable and podman could not start, so it has not been built or run here.
It is deliberately small (one source file, one gcc command, a non-root user
on `debian:bookworm-slim`) and `.dockerignore` at the repository root limits
the build context to `server/`.

```
docker build -f server/Dockerfile -t mathbot-server .
docker run --rm -p 27993:27993 mathbot-server
docker run --rm -p 27993:27993 -e MATHBOT_SECRET=changeme mathbot-server

docker compose -f server/compose.yaml up --build
MATHBOT_SECRET=changeme docker compose -f server/compose.yaml up --build
```

The client command does not change: `./build/client you@umass.edu 27993 127.0.0.1`.

## 3. `make autograde`: what Gradescope did

Gradescope compiled the submitted `client.c` with plain `gcc`, started "a
local server (the same as the public server)" and checked that the client
captured the flag. `make autograde` does exactly that: it runs `make dist`,
compiles `dist/client.c` with `gcc client.c -o a.out`, starts
`build/mathbot_server` on an ephemeral port with the default secret, runs
`./a.out AUTOGRADE_ID PORT 127.0.0.1` and requires stdout to be exactly the
flag that `python3 -I -c 'import hashlib,sys;print(hashlib.sha256(sys.argv[1].encode()).hexdigest())' AUTOGRADE_ID`
prints. It prints `PASS: …` or `FAIL` and exits accordingly, and always stops
the server (`test/autograde.sh`, trap on EXIT).

```
make autograde
make autograde AUTOGRADE_ID=you@umass.edu
```

## 4. Design decisions

- **One file, same rules as the client.** `-std=c99 -D_POSIX_C_SOURCE=200809L`
  with the full warning set as errors, clean under `-fanalyzer`, every
  function `static` but `main`, a header comment on each, every system call
  checked. The only global is `shutdown_requested`, the
  `volatile sig_atomic_t` the signal handler sets; everything else, including
  the three environment settings, travels in a `ServerConfig` passed down.
- **Fork per connection.** A session is sequential (send, wait, compare), so
  the simplest concurrency is a process per client: no shared state, a
  crashed or stalled session cannot hurt another, and the child's own
  `getpid()` seeds its generator. `SIGCHLD` is handled with a
  `waitpid(-1, WNOHANG)` loop (errno saved) so finished children never become
  zombies. Each session end still interrupts `accept` with `EINTR` (a socket
  with a timeout is never restarted, `SA_RESTART` or not), which the loop
  treats as transient and retries. The child drops the listening socket and
  restores default `SIGINT`/`SIGTERM`/`SIGCHLD` dispositions.
- **Clean shutdown without a race.** `SIGINT`/`SIGTERM` only set the flag.
  `accept` is interrupted by them (no `SA_RESTART`), and in case the signal
  lands between the flag check and the `accept` call the listening socket
  carries a one-second `SO_RCVTIMEO`, which Linux applies to `accept` (verified
  on this kernel: `accept` returns `EAGAIN` after the timeout), so the loop
  re-checks the flag at least once a second. The parent closes the listener
  and exits 0; it does not kill running children.
- **`SO_REUSEADDR`.** After a restart the previous connections sit in
  `TIME_WAIT` on the same port; without the option `bind` fails with
  `EADDRINUSE` for up to a minute.
- **30-second `SO_RCVTIMEO` per session.** A client that connects and never
  answers would otherwise pin a child forever. `recv` returns `EAGAIN` after
  the timeout, the child logs `closed: timed out` and exits. The mock server
  uses the same 30 s.
- **SHA-256 in the file.** The flag has to be a 64-hex "hash value unique to
  your NetID" and the mock already defines it as `sha256(id)`. Linking a
  crypto library would add a dependency to a one-file program; FIPS 180-4
  SHA-256 is about eighty lines and is unit-tested against the standard
  vectors (empty string, `abc`, the two-block vector) plus padding boundaries
  (55, 56, 64 bytes) and a 1000-byte incremental hash cross-checked with
  Python's `hashlib`. The round constants and initial hash live as
  `static const` locals, so the "no globals" rule holds literally.
- **`MATHBOT_SECRET` prefix.** `sha256(secret || id)` keeps the default
  (empty secret) identical to the mock and the test helpers, while a
  non-empty secret makes the flags unpredictable to anyone who does not know
  it. It is a prefix, not a suffix, so the id cannot be extended to forge
  another id's flag by appending.
- **xorshift32, not `rand`.** `rand` has hidden global state and `rand_r` is
  obsolescent in POSIX.1-2008; a three-line xorshift32 with its state in a
  local is deterministic, testable, and good enough for drawing arithmetic
  problems. The state is never zero (a zero seed is repaired). `MATHBOT_SEED`
  replaces the time/pid seed verbatim, so every session under it is
  identical and reproducible.
- **Strict where the spec is strict.** The spec says "any extra bytes will
  cause a failure in communication", so the HELLO line and every answer are
  compared byte for byte, exactly like the mock. The client's leniencies
  (CRLF, `+` operands, a BYE without a newline) are for talking to unknown
  servers; this server never needs them. The one leniency kept is the
  `@umass.edu` domain compared without regard to case: the client accepts
  `JDoe@UMass.EDU` on its command line and forwards it verbatim, and a server
  that then rejected it would turn a working client into a mystery failure.
- **Negative operands.** The spec's grammar is `NUM OP NUM` and its only
  example is `505 * 700`; what the real server drew is unknown. The mock used
  -1000..1000 from the start and the client is tested against it, so the
  server keeps that range: a client that survives `-5 - -12` survives
  anything the real server could have sent. A client written for unsigned
  operands only would fail here, and that is a finding, not a false alarm.
- **Logging to stdout, not checked.** Log lines are flushed after each write
  so a forked child never duplicates the parent's buffered output; their
  return values are ignored for the same reason `design.md` section 9 gives
  for stderr diagnostics.

## 5. Tests

- `test/unit/test_mathbot_server.c` includes the server with `main` renamed
  and covers SHA-256 vectors, hex encoding, flag derivation, the generator and
  its ranges, truncating division with negatives, HELLO parsing, byte-exact
  answer comparison, STATUS/BYE formatting, port and override parsing,
  configuration precedence, line framing over a `socketpair` (split,
  coalesced, over-long, closed early) and the `SO_RCVTIMEO` timeout.
- `test/e2e/real_server.sh` starts the server on an ephemeral port with
  `MATHBOT_PROBLEMS=400`, captures a flag with one client, then two at once;
  with `test/e2e/raw_peer.py` (`python3 -I`) it sends a wrong HELLO and a
  CRLF-terminated answer and checks nothing came back; it checks the parent
  has no children left once every session has logged its outcome (no
  zombies); it opens a session, `SIGTERM`s the parent, checks the exit is 0
  after `shutting down` and that the session still ends with the right BYE;
  then it starts a second server with `MATHBOT_SECRET` and the random
  300..2000 count, checks the flag changed to `sha256(secret || id)`, and
  checks `SIGINT` exits 0 too.
- `make autograde` is the whole-pipeline check (section 3).
- `test/e2e/mock_server.py` stays the fault-injection tool (fragmentation,
  pipelining, CRLF, rejection, injected lines); the real server is tested in
  addition to it, never instead.
