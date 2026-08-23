#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
#
# Run the Java benchmark against one or more databases and record the results in
# bench/LANGUAGE_BENCHMARKS.md.
#
#     export ADBC_ODBC_DRIVER=$PWD/build/libadbc_driver_odbc.so
#     export POSTGRES_ODBC_DRIVER=... SQLITE_ODBC_DRIVER=...   # as tests/compat
#     bench/java/run.sh sqlite postgres
#
# Each database's connection string comes from bench/rust/conn.py, which reads
# it out of tests/compat/test_matrix.py exactly as bench/matrix_bench.py does,
# so every language's benchmark talks to the same servers with the same
# settings. Set ADBC_MATRIX_SUFFIX to isolate concurrent runs sharing a server.
#
# Needs a JDK 17+ and Maven. Set MAVEN_OPTS=-Dmaven.repo.local=... if $HOME is
# not writable. The JDBC comparison columns use the SQLite and PostgreSQL JDBC
# drivers the pom pulls in; a <DB>_JDBC environment variable overrides the URL
# derived from the ODBC connection string, and without one those columns read
# `—`.
#
# Knobs: ROWS, FETCH_ROWS, REPS, PYTHON, MVN, JAVA.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"

ROWS="${ROWS:-10000}"
FETCH_ROWS="${FETCH_ROWS:-100000}"
REPS="${REPS:-3}"
PYTHON="${PYTHON:-python3}"
MVN="${MVN:-mvn}"
JAVA="${JAVA:-java}"

# The table this language ingests into; conn.py spells it for the database.
export ADBC_BENCH_TABLE="${ADBC_BENCH_TABLE:-adbc_bench_java}"

# shellcheck source=../langbench.sh
. "$ROOT/bench/langbench.sh"

if [ $# -eq 0 ]; then
    echo "usage: $0 <dbname> [dbname ...]" >&2
    exit 2
fi
: "${ADBC_ODBC_DRIVER:?set it to the built libadbc_driver_odbc.so}"

"$MVN" -B -q -f "$HERE/pom.xml" package -DskipTests
CLASSPATH="$HERE/target/classes:$(cat "$HERE/target/classpath.txt")"

# Arrow's off-heap allocator needs this on JDK 17+, as tests/java documents.
run() {
    "$JAVA" --add-opens=java.base/java.nio=ALL-UNNAMED -cp "$CLASSPATH" \
        org.adbcbridge.bench.Bench "$@"
}

lang_init

for db in "$@"; do
    echo "== $db" >&2
    eval "$("$PYTHON" "$ROOT/bench/rust/conn.py" "$db")"
    if ! output="$(run --rows "$ROWS" --fetch-rows "$FETCH_ROWS" --reps "$REPS" "$db")"; then
        echo "$db: bench_java failed" >&2
        continue
    fi
    echo "$output"
    lang_record java "$db" "$(printf '%s\n' "$output" | grep '^| ' | tail -1)"
done

echo "wrote $LANG_OUT" >&2
