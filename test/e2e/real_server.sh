#!/usr/bin/env bash
# The real client against the real server (build/mathbot_server): one session, two concurrent
# sessions, a rejected HELLO and a CRLF answer that get no BYE, no zombies left behind, a session
# that outlives the parent after SIGTERM, and a second server with a secret and the random problem
# count that exits 0 on SIGINT.
set -euo pipefail
source "$(dirname "${BASH_SOURCE[0]}")/lib.sh"

SECOND_IDENTIFICATION="asmith@umass.edu"
RAW_PEER="$(dirname "${BASH_SOURCE[0]}")/raw_peer.py"

# raw_peer <args...>: runs the raw peer against the current server under a timeout.
raw_peer() {
    timeout 30 python3 -I "$RAW_PEER" "$SERVER_PORT" "$@"
}

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

bad_hello_is_closed_without_bye() {
    local received
    received=$(raw_peer --bad-hello)
    [ -z "$received" ] || fail "server sent '$received' after a bad HELLO"
    assert_server_logged 1 "closed: bad HELLO"
}

crlf_answer_is_closed_without_bye() {
    local received
    received=$(raw_peer --cr-answer)
    [ -z "$received" ] || fail "server sent '$received' after a CRLF answer"
    assert_server_logged 1 "closed: wrong answer"
}

# Finished children must be reaped: once every session has logged its outcome, the parent has no children.
no_zombies_remain() {
    local tries=0
    while [ -n "$(ps -o pid= --ppid "$SERVER_PID")" ]; do
        tries=$((tries + 1))
        [ "$tries" -le 50 ] || fail "server still has children: $(ps -o pid=,stat= --ppid "$SERVER_PID")"
        sleep 0.1
    done
}

# A session in progress keeps running after the parent got SIGTERM, and still ends with BYE.
session_outlives_sigterm() {
    local bye_file="$E2E_SCRATCH/bye.out" peer_pid sessions_before
    sessions_before=$(grep -c " connected" "$SERVER_LOG")
    raw_peer --finish --pause 1 > "$bye_file" &
    peer_pid=$!
    assert_server_logged $((sessions_before + 1)) " connected"
    stop_server
    [ "$SERVER_STATUS" -eq 0 ] || fail "server exited $SERVER_STATUS on SIGTERM, expected 0"
    wait "$peer_pid" || fail "raw peer exited $?"
    [ "$(cat "$bye_file")" = "cs230 $(expected_flag "$IDENTIFICATION") BYE" ] || fail "late session got '$(cat "$bye_file")'"
    head -1 "$SERVER_LOG" | grep -Eq '^listening on port [0-9]+$' || fail "first server line is '$(head -1 "$SERVER_LOG")'"
    grep -q '^shutting down$' "$SERVER_LOG" || fail "server never logged 'shutting down'"
    [ ! -s "$SERVER_ERR" ] || fail "server stderr is not empty"
}

# A fresh server with a secret and no problem override: the random 300..2000 count, a different flag.
secret_changes_the_flag_and_sigint_exits_zero() {
    local flag
    start_server MATHBOT_SECRET=s3cret
    run_client "$IDENTIFICATION" "$SERVER_PORT" 127.0.0.1
    flag=$(expected_flag "s3cret$IDENTIFICATION")
    [ "$CLIENT_STATUS" -eq 0 ] || fail "client exited $CLIENT_STATUS against the secret server"
    [ "$(cat "$CLIENT_OUT")" = "$flag" ] || fail "stdout is '$(cat "$CLIENT_OUT")', expected '$flag'"
    [ "$(cat "$CLIENT_OUT")" != "$(expected_flag "$IDENTIFICATION")" ] || fail "secret did not change the flag"
    assert_server_logged 1 "flag sent"
    kill -INT "$SERVER_PID" || fail "could not signal the server"
    SERVER_STATUS=0
    wait "$SERVER_PID" || SERVER_STATUS=$?
    SERVER_PID=""
    [ "$SERVER_STATUS" -eq 0 ] || fail "server exited $SERVER_STATUS on SIGINT, expected 0"
}

main() {
    start_server MATHBOT_PROBLEMS=400
    one_session_captures_the_flag
    two_sessions_at_once_both_capture_their_flags
    bad_hello_is_closed_without_bye
    crlf_answer_is_closed_without_bye
    no_zombies_remain
    session_outlives_sigterm
    secret_changes_the_flag_and_sigint_exits_zero
}

main "$@"
