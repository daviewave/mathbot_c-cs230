# Math Bot local server: implementation plan

> The course server (128.119.243.147:27993) and the Gradescope autograder no
> longer exist. This plan adds local replacements so the client can be exercised
> end to end exactly as the spec describes. Tasks run in order; each is one
> commit with its tests.

**Goal:** A persistent, concurrent math-speak server in C99 (`server/mathbot_server.c`)
that behaves like the spec's server, an autograder stand-in (`make autograde`), a
container image for it, and docs. The client in `src/client.c` does not change.

**Architecture:** One source file, all functions `static` except `main`, zero
globals except the `volatile sig_atomic_t` shutdown flag. Four groups: SHA-256
(FIPS 180-4) and the flag, the problem generator and math-speak formatting, the
per-connection session (line framing identical to the client's), and the
listener with one forked child per connection. Unit tests use the include trick;
an e2e script drives the real client against the real server; `make autograde`
reproduces what Gradescope did.

**Tech stack:** C99 + POSIX.1-2008 (`fork`, `sigaction`, `waitpid`, sockets),
GNU make, bash, python3 standard library (only in tests), Docker (multi-stage,
`debian:bookworm-slim`; authored here, not buildable in this sandbox).

**Spec:** `docs/spec.md` describes the server's observable behaviour; the mock
in `test/e2e/mock_server.py` is the reference for operand ranges and the flag
derivation (`sha256(id)`), which the server reproduces with an empty secret.

## Constraints

- Same strict flags as the client, clean under `-fanalyzer`.
- Every system call's return value checked; header comment on every function; no
  narration inside bodies.
- `test/e2e/mock_server.py` stays as the fault-injection tool; the real server is
  tested in addition, never instead.
- Deterministic runs: `MATHBOT_PROBLEMS` overrides the per-connection random
  problem count; `MATHBOT_SECRET` (default empty) is prepended to the id before
  hashing, so the default flag equals the mock's and `lib.sh`'s `expected_flag`.
- No pushes, no assistant attribution anywhere, work only in the repo and `$TMPDIR`.

## Tasks

### Task 1: Plan (this file)
- [x] `docs/plan-server.md`, committed on its own.

### Task 2: SHA-256, hex encoding, flag derivation
Files: create `server/mathbot_server.c` (hash group only), `test/unit/test_mathbot_server.c`.
- [ ] Tests first: empty string -> `e3b0c442…b855`, `"abc"` -> `ba7816bf…15ad`, the
  two-block `"abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"` vector,
  a 1000-byte input that crosses several blocks (compared with a python hashlib
  run recorded in the test), hex encoding, `derive_flag` with empty and non-empty
  secret.
- [ ] Implement `Sha256` (init/update/final/digest), `hex_encode`, `derive_flag`.
  Round constants and initial hash live as `static const` locals, not globals.

### Task 3: Problems, arithmetic and protocol lines
Files: extend both files.
- [ ] Tests first: xorshift generator is deterministic and never returns 0;
  `problem_count` in 300..2000 over many seeds; `make_problem` operands in
  -1000..1000, operator in `+-*/`, divisor never 0 over 10000 draws; `evaluate`
  truncates toward zero (`200/3 = 66`, `-7/2 = -3`, `7/-2 = -3`, `-7/-2 = 3`) and
  handles negatives for `+ - *`; `parse_hello` accepts exactly
  `cs230 HELLO <id>` with a valid id and rejects empty ids, spaces, wrong domain,
  missing prefix, trailing `\r`; `is_correct_answer` is byte-exact
  (`cs230 66` yes, `cs230  66`, `cs230 66 `, `66` no); `format_status` and
  `format_bye` produce the spec's lines.
- [ ] Implement them.

### Task 4: Listener, fork-per-connection session, main
Files: finish `server/mathbot_server.c`; Makefile targets `server`, `server-run`;
`check` covers the server; `test` builds the server; `.gitignore` unchanged
(`build/` already ignored).
- [ ] Config: `mathbot_server [PORT]`, PORT from argv, else `MATHBOT_PORT`, else
  27993; port 0 means ephemeral; `MATHBOT_PROBLEMS` and `MATHBOT_SECRET` read once
  at startup into a `ServerConfig` passed down (no globals).
- [ ] Listener: `socket`, `SO_REUSEADDR`, bind `0.0.0.0`, `listen`, `getsockname`,
  first stdout line `listening on port N`, one-second `SO_RCVTIMEO` on the
  listener so the accept loop polls the shutdown flag.
- [ ] Signals via `sigaction`: `SIGCHLD` reaps with `waitpid(-1, WNOHANG)` in a loop
  (errno saved), `SIGINT`/`SIGTERM` set the flag, `SIGPIPE` ignored; children
  restore the defaults.
- [ ] Session: 30 s `SO_RCVTIMEO`, line framing across `recv` boundaries, HELLO
  check, seeded problems (`time ^ pid`), byte-exact answer check, close without
  BYE on any deviation, `cs230 <flag> BYE` after the last answer. One log line on
  connect and one on outcome, both with the peer address.
- [ ] Smoke: `make server && build/mathbot_server 0` plus `build/client` against it.

### Task 5: e2e against the real server
Files: `test/e2e/real_server.sh`, `test/e2e/lib.sh` (`start_server`, server-aware
`assert_flag_captured`), `test/run_tests.sh` (export `SERVER_BIN`).
- [ ] One client with `MATHBOT_PROBLEMS=400`, flag asserted; two clients at once,
  both flags asserted; a wrong HELLO over `python3 -I` sees the connection close
  without BYE; `SIGTERM` makes the server exit 0.

### Task 6: `make autograde`
Files: `test/autograde.sh`, Makefile target.
- [ ] `dist`, `gcc client.c -o a.out` in `dist/`, server on an ephemeral port,
  client run with `AUTOGRADE_ID` (default `autograde@umass.edu`), stdout compared
  with `python3 -I` hashlib, PASS/FAIL, server always stopped.

### Task 7: Container image
Files: `server/Dockerfile`, `server/compose.yaml`, `.dockerignore`.
- [ ] Multi-stage on `debian:bookworm-slim`, strict flags, non-root user,
  `EXPOSE 27993`, `ENV MATHBOT_PORT=27993`, `ENTRYPOINT`. Not buildable in this
  sandbox (Docker socket denied, podman cannot start); recorded in the docs.

### Task 8: Documentation
Files: `docs/server.md`, `README.txt`, `docs/design.md` section 8,
`docs/research.md` new sections (SHA-256, fork/SIGCHLD, SO_REUSEADDR/SO_RCVTIMEO).

### Task 9: Independent review and fixes
- [ ] A reviewer reads the spec, the server and the Dockerfile and lists defects;
  fix them; final gate:
  `make clean && make && make server && make check && make test && make dist && make autograde`.
