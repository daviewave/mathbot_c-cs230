#!/usr/bin/env bash
# Happy path: the seeded random problem count (300..2000), flag alone on stdout, exit 0.
set -euo pipefail
source "$(dirname "${BASH_SOURCE[0]}")/lib.sh"

main() {
    start_mock --seed 7
    run_client "$IDENTIFICATION" "$MOCK_PORT" 127.0.0.1
    assert_flag_captured "$IDENTIFICATION"
}

main "$@"
