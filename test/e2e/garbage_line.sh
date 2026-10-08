#!/usr/bin/env bash
# A line that is neither STATUS nor BYE is a protocol error.
set -euo pipefail
source "$(dirname "${BASH_SOURCE[0]}")/lib.sh"

main() {
    start_mock --inject "cs230 WHAT"
    run_client "$IDENTIFICATION" "$MOCK_PORT" 127.0.0.1
    assert_client_failed "protocol error"
}

main "$@"
