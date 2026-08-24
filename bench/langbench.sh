# SPDX-License-Identifier: Apache-2.0
#
# Shared bits of bench/{csharp,java,go}/run.sh: the header of
# bench/LANGUAGE_BENCHMARKS.md and the upsert that keeps one row per
# (language, database) pair in it.
#
# Source it, then call `lang_init` once and `lang_record <lang> <db> <row>` per
# result. The row is the `| lang | db | ... |` line the benchmark binary prints
# on its last line. Rows for languages this script did not run -- the Python and
# Rust ones -- are left alone, so they survive a re-run of any single language.
#
# Not executable on its own.

LANG_OUT="${LANG_OUT:-$ROOT/bench/LANGUAGE_BENCHMARKS.md}"

# The explanatory header and the empty table, written once.
lang_header() {
    cat <<EOF
<!-- SPDX-License-Identifier: Apache-2.0 -->
# The same benchmark, from every language

One workload, run through five ADBC bindings against the same
\`libadbc_driver_odbc.so\`, so a reader can see that the driver behaves the same
whichever language loads it. The driver is a plain C shared library: every
binding \`dlopen\`s it and calls \`AdbcDriverInit\`, so the differences below are
the bindings' own, not the driver's.

Table \`(id int32, val float64, txt "row-%012d" utf8, dt date32)\`, one database
per run, every number the median of $REPS timings after a warmup. All rates are
rows/s; higher is better.

| Column | What it measures |
|---|---|
| **ADBC ingest** | $(printf "%'d" "$ROWS") rows through \`libadbc_driver_odbc.so\` and that language's ADBC driver manager: bulk ingest in \`create\` mode (\`adbc.ingest.target_table\` + \`adbc.ingest.mode\`), autocommit off, one commit, DDL + data + commit timed together and the row count verified afterwards. Building the Arrow batch is outside the timer. |
| **ADBC fetch** | \`SELECT id, val, txt, dt\` of $(printf "%'d" "$FETCH_ROWS") rows drained into that language's own Arrow record batches through the same driver. |
| **Native ingest / fetch** | the same two steps through the language's ordinary ODBC or JDBC client, no Arrow — the floor that binding could reach without this driver. |

The two ADBC columns are the ones to compare *across* rows: they run identical
work over identical DDL. The native columns are **not** comparable across
languages, because each one uses the bulk API its client actually offers:

| Language | ADBC binding | Native comparison client |
|---|---|---|
| python | \`adbc_driver_manager.dbapi\` | pyodbc \`executemany\` / \`fetchall()\` (see [\`MATRIX_BENCHMARKS.md\`](MATRIX_BENCHMARKS.md)) |
| rust | \`adbc_driver_manager\` crate | [\`odbc-api\`](https://crates.io/crates/odbc-api): array-bound \`ColumnarBulkInserter\`, \`ColumnarDynBuffer\` reads (see [\`RUST_BENCHMARKS.md\`](RUST_BENCHMARKS.md)) |
| csharp | \`Apache.Arrow.Adbc\`'s \`CAdbcDriverImporter\` | \`System.Data.Odbc\`: a prepared \`INSERT\` executed row by row in one transaction, and an \`OdbcDataReader\` |
| java | \`adbc-driver-jni\` | JDBC (\`sqlite-jdbc\` / \`postgresql\`): a prepared \`INSERT\` with \`addBatch\`/\`executeBatch\`, and a \`ResultSet\` |
| go | \`go/adbc/drivermgr\` | \`database/sql\` + [\`alexbrainman/odbc\`](https://github.com/alexbrainman/odbc): a prepared \`INSERT\` executed row by row in one transaction |

Native delegation is switched off (\`adbc.odbc.delegate=never\`), so every row
really travels over ODBC. Servers run locally, so the numbers reflect the ODBC
driver plus the database, not a network — and they move with whatever else is
running on the host, so read them as ratios rather than absolutes.

## Reproducing

\`\`\`sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j
export ADBC_ODBC_DRIVER=\$PWD/build/libadbc_driver_odbc.so
export SQLITE_ODBC_DRIVER=... POSTGRES_ODBC_DRIVER=...   # as tests/compat

bench/csharp/run.sh sqlite postgres     # .NET 8 SDK
bench/java/run.sh   sqlite postgres     # JDK 17+ and Maven
bench/go/run.sh     sqlite postgres     # Go 1.24+ and unixODBC's headers
\`\`\`

Each \`run.sh\` resolves its connection string with \`bench/rust/conn.py\`, which
reads it out of \`tests/compat/test_matrix.py\` exactly as \`bench/matrix_bench.py\`
does, and rewrites its own rows in the table below. Set \`ADBC_MATRIX_SUFFIX\` to
isolate concurrent runs sharing a server; \`ROWS\`, \`FETCH_ROWS\` and \`REPS\`
override the workload.

The Python and Rust rows are the same workload measured by the two benchmarks
that already existed, and are pasted in by hand:

\`\`\`sh
python bench/matrix_bench.py --rows $ROWS --fetch-rows $FETCH_ROWS --reps $REPS sqlite postgres
bench/rust/run.sh sqlite postgres
\`\`\`

For Python, *ADBC ingest* is \`matrix_bench.py\`'s **Ingest (array)** column — the
driver default, the same setting every other language here runs with — not its
**Ingest** column, which forces \`adbc.odbc.array_binding=false\`.

## What to look for

- **The same driver, the same behaviour.** On PostgreSQL, where the server does
  the bulk of the work, all five ADBC ingest rates land inside a 1.15× band. The
  binding barely shows.
- **On SQLite the binding is the whole story.** With the per-row database cost
  near zero, the ADBC ingest column spreads about 2.5× across languages. That
  spread is each driver manager's own per-batch overhead — C data interface
  copies, JNI or P/Invoke transitions — not the driver's.
- **Java pays the most.** \`adbc-driver-jni\` is the slowest binding in both ADBC
  columns, most visibly on SQLite; the gap closes on PostgreSQL ingest, where the
  server dominates, and persists on fetch.
- **ADBC ingest beats a row-at-a-time client comfortably.** Against the
  \`System.Data.Odbc\` and \`database/sql\` paths, which have no array-binding API,
  the bulk ingest is several times faster: the driver binds the whole batch as
  parameter arrays. Against clients that *do* batch — Rust's
  \`ColumnarBulkInserter\`, JDBC's \`executeBatch\` — it is in the same league.
- **A native-protocol client can still win a read.** pgjdbc reads PostgreSQL
  over its own wire protocol rather than ODBC, so its fetch beats the bridge's;
  that is a protocol difference, not driver overhead. Where the comparison stays
  inside ODBC (\`odbc-api\`, \`OdbcDataReader\`), the Arrow path is at or above the
  raw row-by-row one.

## Results

| Language | Database | ADBC ingest | ADBC fetch | Native ingest | Native fetch |
|---|---|---:|---:|---:|---:|
EOF
}

# lang_record <lang> <db> <row>: replace this pair's row, or append it.
lang_record() {
    local lang="$1" db="$2" row="$3" tmp
    [ -n "$row" ] || return 0
    tmp="$(mktemp)"
    # Replace the pair's row where it stands; a new pair goes after the last row of
    # the results table, not at the end of the file -- the file carries prose after
    # the table, and a row appended there is outside it.
    awk -v lang="$lang" -v db="$db" -v row="$row" '
        BEGIN { key = "| " lang " | " db " |" }
        FNR == NR {
            if (index($0, key) == 1) { have = 1 }
            else if ($0 ~ /^\| [a-z]+ \| [a-z0-9]+ \|/ && !seen_heading) { last = FNR }
            # A file with no rows yet: the results table header separator is
            # the line to insert after.
            if (!last && sep && $0 ~ /^\|---/) { last = FNR; sep = 0 }
            if ($0 ~ /^\| Language \| Database \|/) { sep = 1 }
            if (last && $0 ~ /^## /) { seen_heading = 1 }
            next
        }
        index($0, key) == 1 { print row; next }
        { print }
        !have && FNR == last { print row; have = 1 }
        END { if (!have) print row }
    ' "$LANG_OUT" "$LANG_OUT" > "$tmp"
    mv "$tmp" "$LANG_OUT"
}

# Create the file if it is missing or has lost its table header.
lang_init() {
    if [ ! -s "$LANG_OUT" ] || ! grep -q '^| Language | Database |' "$LANG_OUT"; then
        lang_header > "$LANG_OUT"
    fi
}
