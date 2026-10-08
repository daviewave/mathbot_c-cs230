#!/usr/bin/env bash
# Gradescope stand-in, run by `make autograde`: compiles dist/client.c exactly as the autograder did
# (`gcc client.c -o a.out`), starts the local server on an ephemeral port, runs the compiled client
# with the given identification and requires stdout to be exactly sha256(identification).
# Environment: DIST (bundle directory), SERVER_BIN (built server), AUTOGRADE_ID (identification).
set -euo pipefail

: "${DIST:?DIST must point at the dist/ bundle}"
: "${SERVER_BIN:?SERVER_BIN must point at the built mathbot_server}"
: "${AUTOGRADE_ID:?AUTOGRADE_ID must be the identification to grade with}"

SCRATCH=$(mktemp -d "${TMPDIR:-/tmp}/mathbot-autograde.XXXXXX")
SERVER_LOG="$SCRATCH/server.log"
SERVER_PID=""

cleanup() {
    if [ -n "$SERVER_PID" ] && kill -0 "$SERVER_PID" 2>/dev/null; then
        kill -TERM "$SERVER_PID" 2>/dev/null || true
        wait "$SERVER_PID" 2>/dev/null || true
    fi
    rm -rf "$SCRATCH"
}
trap cleanup EXIT

fail() {
    echo "autograde: $*" >&2
    if [ -s "$SERVER_LOG" ]; then
        sed 's/^/    server: /' "$SERVER_LOG" >&2
    fi
    echo "FAIL"
    exit 1
}

compile_like_the_autograder() {
    (cd "$DIST" && gcc client.c -o a.out) || fail "gcc client.c -o a.out failed"
}

start_server() {
    MATHBOT_SECRET= "$SERVER_BIN" 0 > "$SERVER_LOG" 2>&1 &
    SERVER_PID=$!
    local tries=0
    while ! grep -q '^listening on port ' "$SERVER_LOG" 2>/dev/null; do
        kill -0 "$SERVER_PID" 2>/dev/null || fail "server exited before announcing a port"
        tries=$((tries + 1))
        [ "$tries" -le 50 ] || fail "server did not announce a port within 5 s"
        sleep 0.1
    done
    SERVER_PORT=$(head -1 "$SERVER_LOG" | sed 's/^listening on port //')
}

expected_flag() {
    python3 -I -c 'import hashlib, sys; print(hashlib.sha256(sys.argv[1].encode()).hexdigest())' "$AUTOGRADE_ID"
}

run_submission() {
    local status=0
    timeout 120 "$DIST/a.out" "$AUTOGRADE_ID" "$SERVER_PORT" 127.0.0.1 > "$SCRATCH/client.out" 2> "$SCRATCH/client.err" || status=$?
    [ "$status" -eq 0 ] || fail "client exited $status: $(cat "$SCRATCH/client.err")"
}

check_flag() {
    local printed expected
    printed=$(cat "$SCRATCH/client.out")
    expected=$(expected_flag)
    [ "$printed" = "$expected" ] || fail "stdout is '$printed', expected '$expected'"
}

main() {
    compile_like_the_autograder
    start_server
    run_submission
    check_flag
    rm -f "$DIST/a.out"
    echo "PASS: $AUTOGRADE_ID captured $(cat "$SCRATCH/client.out")"
}

main "$@"
