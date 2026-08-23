#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
#
# Run the Go benchmark against one or more databases and record the results in
# bench/LANGUAGE_BENCHMARKS.md.
#
#     export ADBC_ODBC_DRIVER=$PWD/build/libadbc_driver_odbc.so
#     export POSTGRES_ODBC_DRIVER=... SQLITE_ODBC_DRIVER=...   # as tests/compat
#     bench/go/run.sh sqlite postgres
#
# Each database's connection string comes from bench/rust/conn.py, which reads
# it out of tests/compat/test_matrix.py exactly as bench/matrix_bench.py does,
# so every language's benchmark talks to the same servers with the same
# settings. Set ADBC_MATRIX_SUFFIX to isolate concurrent runs sharing a server.
#
# Needs the Go toolchain (1.24+; go.mod pins the version) and unixODBC's
# headers, which github.com/alexbrainman/odbc compiles against for the
# database/sql comparison columns — the same sql.h the driver itself is built
# against. Point GOMODCACHE/GOCACHE somewhere writable if $HOME is not.
#
# Knobs: ROWS, FETCH_ROWS, REPS, PYTHON, GO.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"

ROWS="${ROWS:-10000}"
FETCH_ROWS="${FETCH_ROWS:-100000}"
REPS="${REPS:-3}"
PYTHON="${PYTHON:-python3}"
GO="${GO:-go}"

# The table this language ingests into; conn.py spells it for the database.
export ADBC_BENCH_TABLE="${ADBC_BENCH_TABLE:-adbc_bench_go}"

# shellcheck source=../langbench.sh
. "$ROOT/bench/langbench.sh"

if [ $# -eq 0 ]; then
    echo "usage: $0 <dbname> [dbname ...]" >&2
    exit 2
fi
: "${ADBC_ODBC_DRIVER:?set it to the built libadbc_driver_odbc.so}"

BIN="${BENCH_GO_BIN:-$HERE/bench_go}"
(cd "$HERE" && "$GO" build -o "$BIN" .)

lang_init

for db in "$@"; do
    echo "== $db" >&2
    eval "$("$PYTHON" "$ROOT/bench/rust/conn.py" "$db")"
    if ! output="$("$BIN" -rows "$ROWS" -fetch-rows "$FETCH_ROWS" -reps "$REPS" "$db")"; then
        echo "$db: bench_go failed" >&2
        continue
    fi
    echo "$output"
    lang_record go "$db" "$(printf '%s\n' "$output" | grep '^| ' | tail -1)"
done

echo "wrote $LANG_OUT" >&2
