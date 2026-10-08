#!/usr/bin/env bash
# Two STATUS lines in one write must both be answered, in order.
set -euo pipefail
source "$(dirname "${BASH_SOURCE[0]}")/lib.sh"

main() {
    start_mock --pipeline --problems 400
    run_client "$IDENTIFICATION" "$MOCK_PORT" 127.0.0.1
    assert_flag_captured "$IDENTIFICATION"
}

main "$@"
