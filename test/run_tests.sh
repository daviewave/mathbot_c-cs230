#!/usr/bin/env bash
# Builds every unit test binary, runs it, then runs every e2e script. One PASS/FAIL line per test.
# Invoked by `make test`, which exports CC, CFLAGS and CLIENT_BIN; the defaults below match the Makefile.
set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
BUILD="$ROOT/build"
CC="${CC:-gcc}"
CFLAGS="${CFLAGS:--O2 -std=c99 -D_POSIX_C_SOURCE=200809L -Wall -Wextra -Wpedantic -Wshadow -Wstrict-prototypes -Wmissing-prototypes -Wconversion -Wvla -Werror}"
export CLIENT_BIN="${CLIENT_BIN:-$BUILD/client}"
export MOCK_SERVER="$ROOT/test/e2e/mock_server.py"

mkdir -p "$BUILD/test"
total=0
failures=0

run_test() {
    local name=$1
    shift
    total=$((total + 1))
    if "$@" > "$BUILD/test/$name.log" 2>&1; then
        echo "PASS $name"
    else
        echo "FAIL $name"
        sed 's/^/    /' "$BUILD/test/$name.log"
        failures=$((failures + 1))
    fi
}

for source in "$ROOT"/test/unit/test_*.c; do
    [ -e "$source" ] || continue
    name=$(basename "$source" .c)
    # shellcheck disable=SC2086
    $CC $CFLAGS "$source" -o "$BUILD/test/$name"
    run_test "$name" "$BUILD/test/$name"
done

for script in "$ROOT"/test/e2e/*.sh; do
    [ -e "$script" ] || continue
    name="e2e_$(basename "$script" .sh)"
    case "$name" in e2e_lib) continue ;; esac
    run_test "$name" bash "$script"
done

echo "$total tests, $failures failures"
[ "$failures" -eq 0 ]
