#!/usr/bin/env bash
# Division by zero is refused as a protocol error instead of crashing.
set -euo pipefail
source "$(dirname "${BASH_SOURCE[0]}")/lib.sh"

main() {
    start_mock --inject "cs230 STATUS 5 / 0"
    run_client "$IDENTIFICATION" "$MOCK_PORT" 127.0.0.1
    assert_client_failed "protocol error"
}

main "$@"
