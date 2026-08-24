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
# Windows installs no `python3`; fall back to `python` when it is missing.
PYTHON="${PYTHON:-$(command -v python3 >/dev/null 2>&1 && echo python3 || echo python)}"
MVN="${MVN:-mvn}"
JAVA="${JAVA:-java}"   # JAVA_HOME alone is not enough: java must be on PATH or JAVA set

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
# Maven writes target/classpath.txt in the JVM's own spelling (on Windows: C:\ paths
# joined with ';'), so only the prefix has to be spelled the same way -- a Git-Bash
# /c/... path with a ':' separator gives ClassNotFoundException, which looks like a
# build failure and is not.
case "$(uname -s)" in
    MINGW*|MSYS*|CYGWIN*) CLASSPATH="$(cygpath -w "$HERE/target/classes");$(cat "$HERE/target/classpath.txt")" ;;
    *) CLASSPATH="$HERE/target/classes:$(cat "$HERE/target/classpath.txt")" ;;
esac

# Arrow's off-heap allocator needs this on JDK 17+, as tests/java documents.
# On Windows the JVM's default encoding is the ANSI code page (Cp1252), and the
# em dash the bench prints for a missing JDBC column reached the file as one byte
# (0xE3) -- the row was no longer UTF-8. Both properties pin it everywhere.
run() {
    "$JAVA" --add-opens=java.base/java.nio=ALL-UNNAMED \
        -Dstdout.encoding=UTF-8 -Dfile.encoding=UTF-8 -cp "$CLASSPATH" \
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
