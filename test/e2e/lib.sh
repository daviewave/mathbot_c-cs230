# Shared helpers for the e2e scripts. Sourced, never run: each script sets its own
# shell options and then does
#   source "$(dirname "${BASH_SOURCE[0]}")/lib.sh"
# relying on CLIENT_BIN and MOCK_SERVER being exported by test/run_tests.sh.

: "${CLIENT_BIN:?CLIENT_BIN must point at the built client}"
: "${MOCK_SERVER:?MOCK_SERVER must point at test/e2e/mock_server.py}"

IDENTIFICATION="jdoe@umass.edu"
E2E_SCRATCH=$(mktemp -d "${TMPDIR:-/tmp}/mathbot-e2e.XXXXXX")
CLIENT_OUT="$E2E_SCRATCH/client.out"
CLIENT_ERR="$E2E_SCRATCH/client.err"
MOCK_ERR="$E2E_SCRATCH/mock.err"
CLIENT_STATUS=""
MOCK_STATUS=""
MOCK_PORT=""
MOCK_PID=""

cleanup() {
    if [ -n "$MOCK_PID" ] && kill -0 "$MOCK_PID" 2>/dev/null; then
        kill "$MOCK_PID" 2>/dev/null || true
        wait "$MOCK_PID" 2>/dev/null || true
    fi
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

# assert_flag_captured <id>: exit 0, stdout exactly the flag, stderr empty, mock exited 0.
assert_flag_captured() {
    local flag
    flag=$(expected_flag "$1")
    [ "$CLIENT_STATUS" -eq 0 ] || fail "client exited $CLIENT_STATUS, expected 0"
    [ "$(cat "$CLIENT_OUT")" = "$flag" ] || fail "stdout is '$(cat "$CLIENT_OUT")', expected '$flag'"
    [ ! -s "$CLIENT_ERR" ] || fail "stderr is not empty"
    wait_mock || fail "mock server exited $MOCK_STATUS, expected 0 (flag not sent)"
}

# assert_client_failed <stderr pattern>: non-zero exit, empty stdout, stderr matches the pattern.
assert_client_failed() {
    [ "$CLIENT_STATUS" -ne 0 ] || fail "client exited 0, expected failure"
    [ "$CLIENT_STATUS" -ne 124 ] || fail "client timed out instead of failing"
    [ ! -s "$CLIENT_OUT" ] || fail "stdout is not empty: '$(cat "$CLIENT_OUT")'"
    grep -qi -- "$1" "$CLIENT_ERR" || fail "stderr does not mention '$1'"
}
