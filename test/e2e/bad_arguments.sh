#!/usr/bin/env bash
# Every malformed command line exits non-zero with a usage line on stderr and nothing on stdout.
set -euo pipefail
source "$(dirname "${BASH_SOURCE[0]}")/lib.sh"

check_rejected_with_usage() {
    run_client "$@"
    [ "$CLIENT_STATUS" -ne 0 ] || fail "arguments '$*' were accepted"
    [ ! -s "$CLIENT_OUT" ] || fail "arguments '$*' printed on stdout"
    grep -qi usage "$CLIENT_ERR" || fail "arguments '$*' did not print usage on stderr"
}

main() {
    check_rejected_with_usage
    check_rejected_with_usage jdoe@umass.edu 27993
    check_rejected_with_usage jdoe@gmail.com 27993 127.0.0.1
    check_rejected_with_usage jdoe@umass.edu 0 127.0.0.1
    check_rejected_with_usage jdoe@umass.edu 70000 127.0.0.1
    check_rejected_with_usage jdoe@umass.edu 27993 localhost
}

main "$@"
