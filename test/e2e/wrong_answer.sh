#!/usr/bin/env bash
# The server hanging up after an answer (what it does on a wrong one) is reported, not a hang or a crash.
set -euo pipefail
source "$(dirname "${BASH_SOURCE[0]}")/lib.sh"

main() {
    start_mock --reject
    run_client "$IDENTIFICATION" "$MOCK_PORT" 127.0.0.1
    assert_client_failed "closed the connection"
}

main "$@"
