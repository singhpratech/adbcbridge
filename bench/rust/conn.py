# SPDX-License-Identifier: Apache-2.0
"""Resolve one database's ODBC connection string the way `bench/matrix_bench.py` does.

    eval "$(python3 bench/rust/conn.py postgres)"

Prints shell `export` lines for the Rust benchmark (`bench/rust/src/main.rs`), which
knows nothing about the compat matrix and reads everything it needs out of the
environment:

    <DB>_CONN   the full ODBC connection string, built by `test_matrix.conn_uri`, so
                `{drv}`, `{drvdir}` and `{plugin}`/`{plugin_dir}` expand exactly as they
                do for the compat matrix -- the MySQL-wire entries (Databend, Doris,
                StarRocks, TiDB, ...) need the PLUGIN_DIR one to authenticate at all.
                An existing <DB>_CONN in the environment wins, as it does in
                matrix_bench.py.
    <DB>_TABLE  the benchmark table named the way SQL has to spell it -- the compat
                matrix's `ident` hook, which upper-cases for Oracle and Db2
    <DB>_SETUP  per-connection setup statements, one per line (empty for most
                databases; MariaDB/MySQL need ANSI_QUOTES)

plus any `unicode_env` the database needs (Oracle's NLS_LANG).  The file-based
entries (sqlite) get a fresh temporary directory from `test_matrix`, so each
invocation names a database file of its own.

The table is named `$ADBC_BENCH_TABLE$ADBC_MATRIX_SUFFIX`, defaulting to
`adbc_bench_rs`; the C#, Java and Go benchmarks under `bench/` set
`ADBC_BENCH_TABLE` to a name of their own so they can share a server without
tripping over each other's table.

Exits 1 with a message on stderr if the database's `*_ODBC_DRIVER` variable is unset.
"""
import os
import pathlib
import shutil, pathlib, shlex, sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[2] / "tests" / "compat"))
import test_matrix as m  # noqa: E402

pa = m.pa  # pyarrow, already imported the way test_matrix has to import it

TABLE = os.environ.get("ADBC_BENCH_TABLE", "adbc_bench_rs") + os.environ.get("ADBC_MATRIX_SUFFIX", "")
# The table a read-only entry (ArcadeDB, TDengine) is measured against.
READONLY_TABLE = "adbc_big"


def setup_env(cfg):
    """The entry's per-connection statements, minus any that are really a fixture load.

    A read-only entry's `setup` mixes the session statements every connection needs
    (`USE adbc`, a `CREATE DATABASE IF NOT EXISTS`) with the INSERTs that build its
    fixture -- ArcadeDB's and TDengine's run to hundreds of kilobytes, past what an
    environment can carry.  Statements over 2 KB are the fixture; they are dropped, and
    tests/compat/test_matrix.py is what loads them.
    """
    keep, dropped = [], 0
    for stmt in cfg.get("setup", []):
        if len(stmt) > 2048:
            dropped += 1
        else:
            keep.append(stmt)
    if dropped and READONLY_TABLE:
        # The short statements around the fixture load are its DROP TYPE / CREATE TYPE /
        # CREATE PROPERTY; applied without the INSERTs they recreate the read-only
        # table empty, and every harness then reads 0 rows.  Leave the table alone.
        table = READONLY_TABLE.lower()
        rebuilt = [s for s in keep if table in s.lower()]
        keep = [s for s in keep if table not in s.lower()]
        dropped += len(rebuilt)
    if dropped:
        sys.stderr.write("conn.py: %d fixture-load statement(s) left out of %s's setup -- run "
                         "tests/compat/test_matrix.py first\n" % (dropped, cfg.get("env", "?")))
    return "\n".join(keep)


def main(name):
    cfg = m.DBS.get(name)
    if cfg is None:
        sys.exit("conn.py: unknown database %r (known: %s)" % (name, ", ".join(m.DBS)))
    drv = os.environ.get(cfg["env"])
    if not drv:
        sys.exit("conn.py: %s is unset, so %s cannot be reached" % (cfg["env"], name))
    prefix = name.upper()
    # test_matrix.conn_uri() applies the <NAME>_CONN override and every placeholder the
    # entries use -- {drv}, {drvdir} and the {plugin}/{plugin_dir} PLUGIN_DIR= setting
    # MySQL Connector/ODBC needs for a server still on mysql_native_password.  Formatting
    # {drv} alone here raised KeyError on those entries (dolt, matrixone, greptimedb,
    # tidb, databend, ...), so no language benchmark could reach them.
    # A file-based entry reads a checked-in fixture (Access); put it where the
    # connection string names it, as test_matrix.py and matrix_bench.py do.
    if cfg.get("fixture"):
        shutil.copy(pathlib.Path(m.__file__).parent / "fixtures" / cfg["fixture"],
                    os.path.join(m.TMP, cfg["fixture"]))
    uri = m.conn_uri(name, cfg, drv)
    ident = cfg.get("ident", lambda x: x)
    out = [
        (prefix + "_CONN", uri),
        (prefix + "_TABLE", ident(TABLE)),
        # A read-only entry's `setup` is the fixture load itself (ArcadeDB: 100,000
        # INSERTs), far past what an environment can carry -- and the harnesses only
        # read the fixture, which tests/compat/test_matrix.py has already loaded.
        # Per-connection settings are short; anything long is the fixture, and is left
        # to the compat run.
        (prefix + "_SETUP", setup_env(cfg)),
        # The entry's `ingest_types`, for the two payload columns a server can refuse:
        # int32 (Spanner has no int4) and date32 (CrateDB has no date storage type).
        # The bench has no boolean column, and its float64 column takes the driver's
        # own float8 DDL everywhere, so the other remaps entries carry are not sent.
        (prefix + "_INGEST_TYPES", ",".join(
            "%s=%s" % (f, t) for f, t, sent in (
                ("int32", "int64", cfg.get("ingest_types", {}).get(pa.int32()) == pa.int64()),
                ("date32", "timestamp_us",
                 cfg.get("ingest_types", {}).get(pa.date32()) == pa.timestamp("us")),
            ) if sent)),
        # The statement that makes a write visible to the next scan ("{}" = table).
        (prefix + "_REFRESH", cfg.get("refresh", "")),
        # A read-only entry: nothing to ingest, read the fixture's largest table.
        (prefix + "_READONLY_TABLE",
         READONLY_TABLE if cfg.get("read_only") or not cfg.get("ingest_create", True) else ""),
    ]
    for kv in cfg.get("unicode_env", "").split():
        out.append(tuple(kv.split("=", 1)))
    for key, value in out:
        print("export %s=%s" % (key, shlex.quote(value)))


if __name__ == "__main__":
    if len(sys.argv) != 2:
        sys.exit("usage: conn.py <dbname>")
    main(sys.argv[1])
