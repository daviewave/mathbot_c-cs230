#!/usr/bin/env bash
# Nothing listens on the port: connect fails, the error is reported and nothing reaches stdout.
set -euo pipefail
source "$(dirname "${BASH_SOURCE[0]}")/lib.sh"

free_port() {
    python3 -I -c 'import socket
listener = socket.socket()
listener.bind(("127.0.0.1", 0))
print(listener.getsockname()[1])
listener.close()'
}

main() {
    run_client "$IDENTIFICATION" "$(free_port)" 127.0.0.1
    assert_client_failed "connect"
}

main "$@"
