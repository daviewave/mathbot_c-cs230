#!/usr/bin/env bash
# A server that terminates lines with CRLF is answered and its flag captured all the same.
set -euo pipefail
source "$(dirname "${BASH_SOURCE[0]}")/lib.sh"

main() {
    start_mock --crlf --problems 350
    run_client "$IDENTIFICATION" "$MOCK_PORT" 127.0.0.1
    assert_flag_captured "$IDENTIFICATION"
}

main "$@"
