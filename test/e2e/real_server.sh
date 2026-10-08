#!/usr/bin/env bash
# The real client against the real server (build/mathbot_server): one session, two concurrent
# sessions, a rejected HELLO that gets no BYE, and a clean exit on SIGTERM.
set -euo pipefail
source "$(dirname "${BASH_SOURCE[0]}")/lib.sh"

SECOND_IDENTIFICATION="asmith@umass.edu"

one_session_captures_the_flag() {
    run_client "$IDENTIFICATION" "$SERVER_PORT" 127.0.0.1
    assert_flag_captured "$IDENTIFICATION"
}

two_sessions_at_once_both_capture_their_flags() {
    local first_out="$E2E_SCRATCH/first.out" first_err="$E2E_SCRATCH/first.err" first_pid
    timeout 60 "$CLIENT_BIN" "$IDENTIFICATION" "$SERVER_PORT" 127.0.0.1 > "$first_out" 2> "$first_err" &
    first_pid=$!
    run_client "$SECOND_IDENTIFICATION" "$SERVER_PORT" 127.0.0.1
    wait "$first_pid" || fail "concurrent client exited $?"
    assert_flag_captured "$SECOND_IDENTIFICATION"
    [ "$(cat "$first_out")" = "$(expected_flag "$IDENTIFICATION")" ] || fail "concurrent client printed '$(cat "$first_out")'"
    [ ! -s "$first_err" ] || fail "concurrent client wrote to stderr: $(cat "$first_err")"
    FLAGS_EXPECTED=$((FLAGS_EXPECTED + 1))
    assert_server_logged "$FLAGS_EXPECTED" "flag sent"
}

# Speaks a wrong HELLO and prints every byte the server sends before closing.
bytes_after_bad_hello() {
    timeout 10 python3 -I -c '
import socket, sys
connection = socket.create_connection(("127.0.0.1", int(sys.argv[1])))
connection.sendall(b"cs230 HELLO nobody@example.com\n")
received = b""
while True:
    chunk = connection.recv(4096)
    if not chunk:
        break
    received += chunk
connection.close()
sys.stdout.write(received.decode("ascii", errors="replace"))' "$SERVER_PORT"
}

bad_hello_is_closed_without_bye() {
    local received
    received=$(bytes_after_bad_hello)
    [ -z "$received" ] || fail "server sent '$received' after a bad HELLO"
    assert_server_logged 1 "closed: bad HELLO"
}

sigterm_exits_zero() {
    stop_server
    [ "$SERVER_STATUS" -eq 0 ] || fail "server exited $SERVER_STATUS on SIGTERM, expected 0"
    head -1 "$SERVER_LOG" | grep -Eq '^listening on port [0-9]+$' || fail "first server line is '$(head -1 "$SERVER_LOG")'"
    tail -1 "$SERVER_LOG" | grep -q '^shutting down$' || fail "last server line is '$(tail -1 "$SERVER_LOG")'"
    [ ! -s "$SERVER_ERR" ] || fail "server stderr is not empty"
}

main() {
    start_server MATHBOT_PROBLEMS=400
    one_session_captures_the_flag
    two_sessions_at_once_both_capture_their_flags
    bad_hello_is_closed_without_bye
    sigterm_exits_zero
}

main "$@"
