#!/usr/bin/env bash
# Messages split across two writes (every 7th and the BYE line) must still be answered exactly once.
set -euo pipefail
source "$(dirname "${BASH_SOURCE[0]}")/lib.sh"

main() {
    start_mock --fragment --problems 400
    run_client "$IDENTIFICATION" "$MOCK_PORT" 127.0.0.1
    assert_flag_captured "$IDENTIFICATION"
}

main "$@"
