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
import os, pathlib, shlex, sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[2] / "tests" / "compat"))
import test_matrix as m  # noqa: E402

TABLE = os.environ.get("ADBC_BENCH_TABLE", "adbc_bench_rs") + os.environ.get("ADBC_MATRIX_SUFFIX", "")


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
    uri = m.conn_uri(name, cfg, drv)
    ident = cfg.get("ident", lambda x: x)
    out = [
        (prefix + "_CONN", uri),
        (prefix + "_TABLE", ident(TABLE)),
        (prefix + "_SETUP", "\n".join(cfg.get("setup", []))),
    ]
    for kv in cfg.get("unicode_env", "").split():
        out.append(tuple(kv.split("=", 1)))
    for key, value in out:
        print("export %s=%s" % (key, shlex.quote(value)))


if __name__ == "__main__":
    if len(sys.argv) != 2:
        sys.exit("usage: conn.py <dbname>")
    main(sys.argv[1])
