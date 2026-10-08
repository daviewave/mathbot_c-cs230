# Shared helpers for the e2e scripts. Sourced, never run: each script sets its own
# shell options and then does
#   source "$(dirname "${BASH_SOURCE[0]}")/lib.sh"
# relying on CLIENT_BIN, MOCK_SERVER and SERVER_BIN being exported by test/run_tests.sh.
# A script talks either to the single-connection mock (start_mock) or to the real server
# (start_server); assert_flag_captured confirms the flag with whichever one is running.

: "${CLIENT_BIN:?CLIENT_BIN must point at the built client}"
: "${MOCK_SERVER:?MOCK_SERVER must point at test/e2e/mock_server.py}"
: "${SERVER_BIN:?SERVER_BIN must point at the built mathbot_server}"

IDENTIFICATION="jdoe@umass.edu"
E2E_SCRATCH=$(mktemp -d "${TMPDIR:-/tmp}/mathbot-e2e.XXXXXX")
CLIENT_OUT="$E2E_SCRATCH/client.out"
CLIENT_ERR="$E2E_SCRATCH/client.err"
MOCK_ERR="$E2E_SCRATCH/mock.err"
CLIENT_STATUS=""
MOCK_STATUS=""
MOCK_PORT=""
MOCK_PID=""
SERVER_LOG="$E2E_SCRATCH/server.log"
SERVER_ERR="$E2E_SCRATCH/server.err"
SERVER_STATUS=""
SERVER_PORT=""
SERVER_PID=""
FLAGS_EXPECTED=0

# kill_if_running <pid>: terminates a background helper that is still alive and waits for it.
kill_if_running() {
    if [ -n "$1" ] && kill -0 "$1" 2>/dev/null; then
        kill "$1" 2>/dev/null || true
        wait "$1" 2>/dev/null || true
    fi
}

cleanup() {
    kill_if_running "$MOCK_PID"
    kill_if_running "$SERVER_PID"
    rm -rf "$E2E_SCRATCH"
}
trap cleanup EXIT

fail() {
    echo "FAIL: $*" >&2
    if [ -s "$CLIENT_ERR" ]; then
        sed 's/^/    client stderr: /' "$CLIENT_ERR" >&2
    fi
    if [ -s "$MOCK_ERR" ]; then
        sed 's/^/    mock stderr: /' "$MOCK_ERR" >&2
    fi
    if [ -s "$SERVER_LOG" ]; then
        sed 's/^/    server log: /' "$SERVER_LOG" >&2
    fi
    if [ -s "$SERVER_ERR" ]; then
        sed 's/^/    server stderr: /' "$SERVER_ERR" >&2
    fi
    exit 1
}

# start_mock <mock args...>: launches the mock (requiring HELLO to carry $IDENTIFICATION) in the
# background and exports MOCK_PORT.
start_mock() {
    local portfile="$E2E_SCRATCH/port"
    rm -f "$portfile"
    python3 -I "$MOCK_SERVER" --port-file "$portfile" --identification "$IDENTIFICATION" "$@" 2> "$MOCK_ERR" &
    MOCK_PID=$!
    local tries=0
    while [ ! -s "$portfile" ]; do
        if ! kill -0 "$MOCK_PID" 2>/dev/null; then
            fail "mock server exited before publishing a port"
        fi
        tries=$((tries + 1))
        if [ "$tries" -gt 50 ]; then
            fail "mock server did not publish a port within 5 s"
        fi
        sleep 0.1
    done
    MOCK_PORT=$(tr -d '[:space:]' < "$portfile")
    export MOCK_PORT
}

# start_server [VAR=value ...]: launches the real server on an ephemeral port with the given
# environment and exports SERVER_PORT once it has announced the port.
start_server() {
    : > "$SERVER_LOG"
    env "$@" "$SERVER_BIN" 0 > "$SERVER_LOG" 2> "$SERVER_ERR" &
    SERVER_PID=$!
    local tries=0
    while ! grep -q '^listening on port ' "$SERVER_LOG" 2>/dev/null; do
        if ! kill -0 "$SERVER_PID" 2>/dev/null; then
            fail "server exited before announcing a port"
        fi
        tries=$((tries + 1))
        if [ "$tries" -gt 50 ]; then
            fail "server did not announce a port within 5 s"
        fi
        sleep 0.1
    done
    SERVER_PORT=$(head -1 "$SERVER_LOG" | sed 's/^listening on port //')
    export SERVER_PORT
}

# stop_server: SIGTERMs the server, waits for it and records SERVER_STATUS.
stop_server() {
    SERVER_STATUS=0
    kill -TERM "$SERVER_PID" || fail "could not signal the server"
    wait "$SERVER_PID" || SERVER_STATUS=$?
    SERVER_PID=""
}

# assert_server_logged <count> <pattern>: waits up to 5 s for the server log to hold at least
# <count> lines matching <pattern> (the child logs its outcome just after the client exits).
assert_server_logged() {
    local tries=0
    while [ "$(grep -c -- "$2" "$SERVER_LOG")" -lt "$1" ]; do
        tries=$((tries + 1))
        if [ "$tries" -gt 50 ]; then
            fail "server log has fewer than $1 lines matching '$2'"
        fi
        sleep 0.1
    done
}

# wait_mock: waits for the mock, records MOCK_STATUS and returns it.
wait_mock() {
    MOCK_STATUS=0
    wait "$MOCK_PID" || MOCK_STATUS=$?
    MOCK_PID=""
    return "$MOCK_STATUS"
}

# run_client <args...>: runs the client under timeout, capturing output and status without aborting.
run_client() {
    CLIENT_STATUS=0
    timeout 60 "$CLIENT_BIN" "$@" > "$CLIENT_OUT" 2> "$CLIENT_ERR" || CLIENT_STATUS=$?
}

# expected_flag <id>: the flag the mock sends for <id>.
expected_flag() {
    python3 -I -c 'import hashlib, sys; print(hashlib.sha256(sys.argv[1].encode()).hexdigest())' "$1"
}

# assert_flag_printed <id>: exit 0, stdout exactly the flag, stderr empty.
assert_flag_printed() {
    local flag
    flag=$(expected_flag "$1")
    [ "$CLIENT_STATUS" -eq 0 ] || fail "client exited $CLIENT_STATUS, expected 0"
    [ "$(cat "$CLIENT_OUT")" = "$flag" ] || fail "stdout is '$(cat "$CLIENT_OUT")', expected '$flag'"
    [ ! -s "$CLIENT_ERR" ] || fail "stderr is not empty"
}

# assert_server_confirmed_flag: the mock exited 0, or the real server logged one more "flag sent".
assert_server_confirmed_flag() {
    if [ -n "$MOCK_PID" ]; then
        wait_mock || fail "mock server exited $MOCK_STATUS, expected 0 (flag not sent)"
    else
        FLAGS_EXPECTED=$((FLAGS_EXPECTED + 1))
        assert_server_logged "$FLAGS_EXPECTED" "flag sent"
    fi
}

# assert_flag_captured <id>: the client printed the flag and the server agrees it sent it.
assert_flag_captured() {
    assert_flag_printed "$1"
    assert_server_confirmed_flag
}

# assert_client_failed <stderr pattern>: non-zero exit, empty stdout, stderr matches the pattern.
assert_client_failed() {
    [ "$CLIENT_STATUS" -ne 0 ] || fail "client exited 0, expected failure"
    [ "$CLIENT_STATUS" -ne 124 ] || fail "client timed out instead of failing"
    [ ! -s "$CLIENT_OUT" ] || fail "stdout is not empty: '$(cat "$CLIENT_OUT")'"
    grep -qi -- "$1" "$CLIENT_ERR" || fail "stderr does not mention '$1'"
}
