CS230 Project 5: Math Bot client
================================

Overview
--------
client.c is a TCP client for the course "math speak" protocol. It validates its
three arguments (NetID@umass.edu, port, IPv4 host), opens a stream socket,
connects, sends "cs230 HELLO <id>\n", then loops: it reads one newline-terminated
line at a time from a receive buffer that survives messages split across or
coalesced within recv() calls, solves each "cs230 STATUS A OP B" problem in
long long with C's truncate-toward-zero division, replies "cs230 <ANSWER>\n"
with no extra bytes, and stops at "cs230 <FLAG> BYE", printing the flag alone
on stdout and exiting 0. Every other outcome (bad arguments, connection
failure, the server closing early after a wrong answer, a malformed line,
division by zero) prints a message on stderr and exits 1. The only submitted
file is src/client.c; it compiles with a plain `gcc client.c` as the autograder
does and with the strict warning set in docs/conventions.md.

Build and run
-------------
    make                 release build into build/client
    make debug           -O0 -g3 build into build/debug/client
    make server          the local math-speak server into build/mathbot_server
    make check           gcc -fanalyzer over src/ and server/
    make test            unit tests plus end-to-end tests against the mock and the real server
    make dist            flat Gradescope bundle in dist/, proven to build three ways
    make autograde       what Gradescope did: dist/client.c with plain gcc against the local server
    make clean           removes build/ and dist/

The course server (128.119.243.147:27993) and the Gradescope autograder no
longer exist. server/mathbot_server.c replaces both locally (docs/server.md):
it speaks the spec's protocol, forks a process per connection, sends 300..2000
problems and ends with a 64-hex SHA-256 flag derived from the identification.

    make server && make server-run          # terminal 1: serves on 27993 until Ctrl-C
    ./build/client netid@umass.edu 27993 127.0.0.1    # terminal 2: prints the flag

    make server-run PORT=5000               # another port
    MATHBOT_SECRET=changeme make server-run # flags no longer equal sha256(id)
    MATHBOT_VERBOSE=1 ./build/client ...    # echoes every line sent (>>) and received (<<) on stderr

    make autograde                          # PASS/FAIL like the autograder, exit status to match
    make autograde AUTOGRADE_ID=netid@umass.edu

The server can also run in Docker (server/Dockerfile, server/compose.yaml):
    docker build -f server/Dockerfile -t mathbot-server . && docker run --rm -p 27993:27993 mathbot-server

Requirements map
----------------
Spec section / rubric item                         Where it is satisfied (src/client.c)
--------------------------------------------------  ---------------------------------------------
Single file client.c, compiles with plain gcc       src/client.c; `make dist` runs `gcc client.c`,
                                                    `gcc -std=c99 -Wall client.c` and the strict set
Arguments in order: identification, port, host IP   parse_arguments
Identification is NetID@umass.edu                   is_valid_identification
Port as the socket API defines it (1..65535)        parse_port
Host IP as the socket API defines it (IPv4)         is_valid_host, inet_pton in connect_to_server
Step 1: open a TCP stream socket                    connect_to_server: socket(AF_INET, SOCK_STREAM, 0)
Step 2: connect on the given port                   connect_to_server: sockaddr_in, htons, inet_pton, connect
Step 3: send "cs230 HELLO <NETID>@umass.edu\n"      send_hello (byte-exact; unit test test_send_hello_exact_bytes)
Step 4: receive a math problem                      receive_line + line_buffer_take_line, parse_status
Step 5: send the correct "cs230 <ANSWER>\n"         evaluate, handle_status (unit test test_handle_status_exact_bytes)
Step 6: repeat 300..2000 times                      run_session loop; e2e happy_path uses the mock's seeded count
Step 7: receive "cs230 <FLAG> BYE\n", capture flag  parse_bye, print_flag (flag alone on stdout, exit 0)
Division truncated (200 / 3 -> 66)                  divide_checked (C99 6.5.5), test_evaluate_division_truncates_toward_zero
No extra bytes in messages                          send_line sends exactly the formatted line; verified by unit tests
Check system-call return values                     socket, connect, send, recv, close, signal, inet_pton, snprintf and the
                                                    stdout printf/fflush are checked; recv/send retry EINTR; send loops
                                                    over short writes (send_all); stderr diagnostics are fire-and-forget
Server drops connection on a wrong answer           receive_line returns RECEIVE_EOF -> report_early_disconnect, exit 1
Messages split across / coalesced within recv()     LineBuffer, line_buffer_append, line_buffer_take_line;
                                                    unit tests test_take_line_*, test_receive_line_*; e2e fragmented, pipelined
Hint: print messages for debugging                  MATHBOT_VERBOSE=1 (is_verbose_enabled, send_line, run_session)
Hint: identify when done                            parse_bye -> SESSION_DONE
Hint: print the final message / flag                print_flag
Robustness: malformed line, unknown operator,       parse_status (exact grammar), evaluate -> protocol error, exit 1
  overflow, division by zero
Wrong argument count or values                      parse_arguments + print_usage on stderr, exit 1

Design notes
------------
- Line framing: a 4096-byte LineBuffer accumulates recv() output; take_line
  extracts the first complete line and memmoves the rest down. A line that
  fills the buffer without a newline is a protocol error (the longest legal
  line is 75 bytes).
- Arithmetic: long long with overflow pre-checks (CERT INT32-C pattern) and a
  guard for x / 0 and LLONG_MIN / -1, which are undefined in C. A problem the
  client cannot answer correctly is reported rather than answered wrongly.
- SIGPIPE is ignored so a send() racing the server's disconnect reports EPIPE
  instead of killing the process silently.
- Tests: test/unit/test_client.c includes client.c with main renamed and
  exercises every function (223 checks); test/e2e/*.sh drive the binary against
  test/e2e/mock_server.py (python3 standard library) for the happy path,
  fragmented, pipelined and CRLF-terminated messages, wrong-answer disconnect,
  garbage line, division by zero, bad arguments and connection refused, and
  against the real server (test/e2e/real_server.sh: one session, two at once,
  a rejected HELLO, clean SIGTERM). test/unit/test_mathbot_server.c covers the
  server the same way, SHA-256 against the FIPS vectors included.
- Leniencies on input: a trailing '\r' is stripped, a '+'-prefixed operand is
  accepted, a BYE line without a final newline before the server closes still
  yields the flag, and the @umass.edu domain is matched case-insensitively.
  Everything else is byte-exact: extra spaces, unknown lines and operands
  outside long long are protocol errors (see docs/design.md).
- Rationale for every decision: docs/design.md; sources: docs/research.md.

Video: not required for this project (spec: "There is no video for this last submission").
