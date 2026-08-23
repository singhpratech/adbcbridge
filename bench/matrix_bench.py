# SPDX-License-Identifier: Apache-2.0
"""Per-database benchmark: read and bulk ingest, adbcbridge vs pyodbc, on every database in
the compat matrix.

    ADBC_ODBC_DRIVER=build/libadbc_driver_odbc.so <same *_ODBC_DRIVER env as tests/compat> \
        python bench/matrix_bench.py [--rows 20000] [--fetch-rows 200000] [--reps 3] [db ...]

Workload: a 4-column table (id int32, val double, txt varchar(20), dt date).

* ingest  - `cur.adbc_ingest(mode="create")` of --rows rows on a dbapi connection with the
            default autocommit=False, timed end to end (DDL + data + commit), once with the
            driver default and once with `adbc.odbc.array_binding=true`;
            the row count is verified afterwards.  pyodbc comparison: `executemany` with
            `fast_executemany=True` where the ODBC driver supports it.
* fetch   - full `SELECT` of --fetch-rows rows: `cur.fetch_arrow_table()` vs pyodbc
            `fetchall()` -> `pyarrow.Table`; median of --reps after a warmup.

Results are cached in bench/.matrix_bench.json so databases can be run one at a time;
bench/MATRIX_BENCHMARKS.md is regenerated from the cache on every run.
"""
import argparse, datetime, json, os, pathlib, statistics, sys, time

import pyarrow as pa
import adbc_driver_manager.dbapi as dbapi

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent.parent / "tests" / "compat"))
import test_matrix as m  # noqa: E402

ap = argparse.ArgumentParser()
ap.add_argument("--rows", type=int, default=20_000, help="rows to ingest")
ap.add_argument("--fetch-rows", type=int, default=200_000, help="rows to read back")
ap.add_argument("--reps", type=int, default=3)
ap.add_argument("--render", action="store_true", help="only rewrite MATRIX_BENCHMARKS.md from the cache")
ap.add_argument("--no-pyodbc", action="store_true")
ap.add_argument("--pyodbc-timeout", type=int, default=600, help="seconds per pyodbc step")
ap.add_argument("--_child", nargs=2, metavar=("FN", "DB"), help=argparse.SUPPRESS)
ap.add_argument("dbs", nargs="*")
args = ap.parse_args()

HERE = pathlib.Path(__file__).resolve().parent
TABLE = "adbc_bench" + os.environ.get("ADBC_MATRIX_SUFFIX", "")
COLS = ["id", "val", "txt", "dt"]


def make_table(n, cfg):
    # ingest_payload applies the entry's `ingest_types`, for a server with no column
    # type the generated DDL could name for one of these (CrateDB has no DATE).
    return m.ingest_payload(cfg, {
        "id": pa.array(range(n), pa.int32()),
        "val": pa.array([i * 0.5 for i in range(n)], pa.float64()),
        "txt": pa.array(["row-%012d" % i for i in range(n)]),
        "dt": pa.array([i % 20000 for i in range(n)], pa.date32()),
    })


def connect(uri, cfg, autocommit=True, **db_kwargs):
    # cfg["db_kwargs"]: options the entry needs to read the server correctly at all
    # (see test_matrix.py); an explicit keyword here still wins.
    kwargs = {"uri": uri, **cfg.get("db_kwargs", {}), **db_kwargs}
    conn = dbapi.connect(driver=m.DRIVER, db_kwargs=kwargs, autocommit=autocommit)
    with conn.cursor() as cur:
        for sql in cfg.get("setup", []):
            cur.execute(sql)
    return conn


def drop(cur, ident, reset=None):
    for t in (ident(TABLE), '"%s"' % TABLE, TABLE):
        try:
            cur.execute("DROP TABLE " + t)
            return
        except Exception:
            if reset:  # a failed statement aborts the transaction on MonetDB/Postgres
                reset()


def quiet(fn):
    """Call fn, ignoring errors such as DuckDB's 'no transaction is active' on rollback."""
    try:
        fn()
    except Exception:
        pass


def table_ref(conn, ident):
    """The spelling of TABLE that resolves: ingest quotes the name, so it is case-exact
    (lower-case on Oracle/Db2, which upper-case an unquoted name); MySQL quotes with
    backticks. Try the candidates and keep the first that SELECTs."""
    last = None
    for t in ('"%s"' % TABLE, ident(TABLE), "`%s`" % TABLE, TABLE):
        try:
            cur = conn.cursor()
            cur.execute("SELECT COUNT(*) FROM " + t)
            cur.fetchone()
            cur.close()
            return t
        except Exception as e:  # noqa: BLE001
            last = e
    raise last


def count(conn, ident):
    with conn.cursor() as cur:
        cur.execute("SELECT COUNT(*) FROM " + table_ref(conn, ident))
        return int(cur.fetchone()[0])


def time_ingest(uri, cfg, ident, tbl, array_binding, autocommit=True):
    conn = connect(uri, cfg, autocommit=autocommit, **{"adbc.odbc.delegate": "never"})
    try:
        with conn.cursor() as cur:
            drop(cur, ident)
            if not autocommit:
                # A failed DROP aborts the transaction on MonetDB/Postgres; start clean.
                conn.rollback()
            if array_binding is not None:
                cur.adbc_statement.set_options(**{"adbc.odbc.array_binding": array_binding})
            t0 = time.perf_counter()
            cur.adbc_ingest(TABLE, tbl, mode="create")
            if not autocommit:
                conn.commit()
            dt = time.perf_counter() - t0
            # Outside the timing: making the rows visible to a scan is a read-side step
            # on an eventually consistent store, not part of writing them.
            m.refresh(cur, cfg, TABLE)
        got = count(conn, ident)
        if got != tbl.num_rows:
            return dict(error="wrong row count %d != %d" % (got, tbl.num_rows))
        return dict(secs=dt)
    finally:
        conn.close()


def time_pyodbc_ingest(uri, cfg, ident, tbl):
    import pyodbc
    pc = pyodbc.connect(uri, autocommit=True)  # DDL first; some drivers' SQLEndTran is a no-op
    try:
        for sql in cfg.get("setup", []):
            pc.execute(sql)
        cur = pc.cursor()
        drop(cur, ident)
        # Let adbcbridge create the table so the DDL is identical, then time only the rows.
        conn = connect(uri, cfg, **{"adbc.odbc.delegate": "never"})
        with conn.cursor() as c:
            c.adbc_ingest(TABLE, tbl.slice(0, 0), mode="create")
        conn.close()
        rows = list(zip(*[tbl.column(c).to_pylist() for c in COLS]))
        sql = "INSERT INTO %s VALUES (?, ?, ?, ?)" % table_ref(pc, ident)
        pc.autocommit = bool(os.environ.get("ADBC_BENCH_PYODBC_AUTOCOMMIT"))
        fast = not os.environ.get("ADBC_BENCH_PYODBC_NO_FAST")
        try:
            cur.fast_executemany = fast
        except Exception:
            pass
        t0 = time.perf_counter()
        try:
            cur.executemany(sql, rows)
        except Exception:
            cur.fast_executemany = False
            quiet(pc.rollback)
            t0 = time.perf_counter()
            cur.executemany(sql, rows)
        if not pc.autocommit:
            pc.commit()
        dt = time.perf_counter() - t0
        m.refresh(cur, cfg, TABLE)
        conn = connect(uri, cfg, **{"adbc.odbc.delegate": "never"})
        try:
            got = count(conn, ident)
        finally:
            conn.close()
        if got != tbl.num_rows:
            return dict(error="wrong row count %d != %d" % (got, tbl.num_rows))
        return dict(secs=dt, fast=bool(cur.fast_executemany), autocommit=pc.autocommit)
    finally:
        pc.close()


def time_fetch(uri, cfg, ident, n, table=None, **db_kwargs):
    conn = connect(uri, cfg, **db_kwargs)
    try:
        delegated = ""
        try:
            delegated = conn.adbc_connection.get_option("adbc.odbc.delegated_to")
        except Exception:
            pass
        sql = "SELECT * FROM " + (table or table_ref(conn, ident))

        def run():
            with conn.cursor() as cur:
                cur.execute(sql)
                t = cur.fetch_arrow_table()
            assert t.num_rows == n, (t.num_rows, n)
        run()
        ts = []
        for _ in range(args.reps):
            t0 = time.perf_counter(); run(); ts.append(time.perf_counter() - t0)
        return dict(secs=statistics.median(ts), delegated=delegated)
    finally:
        conn.close()


def time_pyodbc_fetch(uri, cfg, ident, n):
    import pyodbc
    pc = pyodbc.connect(uri, autocommit=True)
    try:
        for sql in cfg.get("setup", []):
            pc.execute(sql)
        sql = "SELECT * FROM " + table_ref(pc, ident)

        def run():
            rows = pc.execute(sql).fetchall()
            cols = list(zip(*rows)) if rows else [[]] * 4
            t = pa.table(dict(zip(COLS, cols)))
            assert t.num_rows == n
        run()
        ts = []
        for _ in range(args.reps):
            t0 = time.perf_counter(); run(); ts.append(time.perf_counter() - t0)
        return dict(secs=statistics.median(ts))
    finally:
        pc.close()


def in_child(fn_name, name, uri):
    """Run a pyodbc step in a subprocess so a hung ODBC driver cannot stall the sweep."""
    import subprocess
    os.environ[name.upper() + "_CONN"] = uri  # the matrix's temp dir is per-process
    cmd = [sys.executable, __file__, "--rows", str(args.rows), "--fetch-rows", str(args.fetch_rows),
           "--reps", str(args.reps), "--_child", fn_name, name]
    # pyodbc's fast_executemany (its own parameter arrays) segfaults against sqliteodbc
    # and the Db2 clidriver and trips MonetDB's driver, and MonetDB's SQLEndTran is a
    # no-op under pyodbc: fall back to plain executemany, then to autocommit per row,
    # so the column shows pyodbc as it actually works against that driver.
    stages = [{}] if fn_name != "ingest" else [
        {}, {"ADBC_BENCH_PYODBC_NO_FAST": "1"},
        {"ADBC_BENCH_PYODBC_NO_FAST": "1", "ADBC_BENCH_PYODBC_AUTOCOMMIT": "1"}]
    res = None
    for extra in stages:
        env = dict(os.environ, **extra)
        try:
            p = subprocess.run(cmd, capture_output=True, text=True, timeout=args.pyodbc_timeout, env=env)
        except subprocess.TimeoutExpired:
            return dict(error="pyodbc timed out after %ds" % args.pyodbc_timeout)
        if p.returncode != 0:
            res = dict(error="pyodbc child exited %d: %s" % (p.returncode, (p.stderr.strip().splitlines() or ["?"])[-1][:100]))
            continue
        res = json.loads(p.stdout.strip().splitlines()[-1])
        if "error" not in res:
            return res
    return res


def attempt(fn, *a, **kw):
    try:
        return fn(*a, **kw)
    except Exception as e:  # noqa: BLE001
        return dict(error="%s: %s" % (type(e).__name__, str(e).splitlines()[0][:100]))


def bench(name, cfg):
    drv = os.environ.get(cfg["env"])
    if not drv:
        return None
    for kv in cfg.get("unicode_env", "").split():
        k, v = kv.split("=", 1); os.environ.setdefault(k, v)
    if cfg.get("fixture"):  # file-based database: work on a private copy
        import shutil
        shutil.copy(pathlib.Path(m.__file__).parent / "fixtures" / cfg["fixture"],
                    os.path.join(m.TMP, cfg["fixture"]))
    uri = m.conn_uri(name, cfg, drv)
    ident = cfg.get("ident", lambda x: x)
    conn = connect(uri, cfg, **{"adbc.odbc.delegate": "never"})
    info = conn.adbc_get_info()
    r = dict(vendor="%s %s" % (info["vendor_name"], info["vendor_version"]), rows=args.rows,
             fetch_rows=args.fetch_rows)
    if cfg.get("read_only"):
        # No DDL/DML through this driver: read the fixture's largest table instead.
        r["read_only"] = True
        with conn.cursor() as cur:
            cur.execute("SELECT COUNT(*) FROM adbc_big")
            r["fetch_rows"] = int(cur.fetchone()[0])
        conn.close()
        r["fetch"] = attempt(time_fetch, uri, cfg, ident, r["fetch_rows"], table="adbc_big",
                             **{"adbc.odbc.delegate": "never"})
        return r
    conn.close()
    tbl = make_table(args.rows, cfg)
    # Autocommit on: the driver batches the whole stream into one transaction itself.
    # (Firebird also cannot INSERT into a table created in the same open transaction.)
    # *array* is the driver default: parameter arrays unless the driver is known to
    # mishandle them (DuckDB, clickhouse-odbc, OdbcFb, MonetDB), in which case the
    # default already is one row at a time and both columns measure the same path.
    r["ingest"] = attempt(time_ingest, uri, cfg, ident, tbl, "false")
    r["ingest_array"] = attempt(time_ingest, uri, cfg, ident, tbl, None)
    if not args.no_pyodbc:
        r["ingest_pyodbc"] = in_child("ingest", name, uri)
    big = make_table(args.fetch_rows, cfg)
    # Load the fetch table with whichever ingest path works; fall back to the slow one.
    loaded = attempt(time_ingest, uri, cfg, ident, big, None)
    if "error" in loaded:
        r["fetch"] = loaded
        return r
    r["fetch"] = attempt(time_fetch, uri, cfg, ident, args.fetch_rows, **{"adbc.odbc.delegate": "never"})
    native = attempt(time_fetch, uri, cfg, ident, args.fetch_rows, **{"adbc.odbc.delegate": "auto"})
    if "error" not in native and native.get("delegated") not in ("", "odbc"):
        r["fetch_native"] = native
    if not args.no_pyodbc:
        r["fetch_pyodbc"] = in_child("fetch", name, uri)
    return r


def rate(rows, x):
    if not x or "error" in x:
        return None
    return rows / x["secs"]


def fmt_rate(v):
    return "{:,.0f}".format(v) if v else "—"


if args._child:
    fn_name, name = args._child
    cfg = m.DBS[name]
    for kv in cfg.get("unicode_env", "").split():
        k, v = kv.split("=", 1); os.environ.setdefault(k, v)
    uri = m.conn_uri(name, cfg, drv)
    # conn_uri() takes the driver library explicitly; without it every pyodbc child dies
    # with "conn_uri() missing 1 required positional argument: 'drv'" and the pyodbc
    # columns of the table come out empty for every database.
    uri = m.conn_uri(name, cfg, drv)
    ident = cfg.get("ident", lambda x: x)
    if fn_name == "ingest":
        res = attempt(time_pyodbc_ingest, uri, cfg, ident, make_table(args.rows, cfg))
    else:
        res = attempt(time_pyodbc_fetch, uri, cfg, ident, args.fetch_rows)
    print(json.dumps(res))
    sys.exit(0)

cache = HERE / ".matrix_bench.json"
results = json.loads(cache.read_text()) if cache.exists() else {}
for name in ([] if args.render else (args.dbs or list(m.DBS))):
    r = attempt(bench, name, m.DBS[name])
    if r is None:
        continue
    results[name] = r
    cache.write_text(json.dumps(results, indent=1))
    if "error" in r:
        print("%-11s ERROR %s" % (name, r["error"]))
        continue
    print("%-11s %-30s fetch=%s/s (pyodbc %s/s, native %s/s)  ingest=%s/s array=%s/s pyodbc=%s/s" % (
        name, r["vendor"],
        fmt_rate(rate(r["fetch_rows"], r.get("fetch"))), fmt_rate(rate(r["fetch_rows"], r.get("fetch_pyodbc"))),
        fmt_rate(rate(r["fetch_rows"], r.get("fetch_native"))),
        fmt_rate(rate(r["rows"], r.get("ingest"))), fmt_rate(rate(r["rows"], r.get("ingest_array"))),
        fmt_rate(rate(r["rows"], r.get("ingest_pyodbc")))))
    for k in ("ingest", "ingest_array", "ingest_pyodbc", "fetch", "fetch_pyodbc"):
        if isinstance(r.get(k), dict) and "error" in r[k]:
            print("            %s: %s" % (k, r[k]["error"]))

out = HERE / "MATRIX_BENCHMARKS.md"
lines = [
    "<!-- SPDX-License-Identifier: Apache-2.0 -->",
    "# Per-database benchmarks",
    "",
    "Generated by `bench/matrix_bench.py` on %s. Table `(id int32, val double, txt varchar(20), dt date)`. "
    "**Ingest** = `adbc_ingest(mode=\"create\")` of the row count in *Rows* on an autocommit connection (the driver batches the stream "
    "into one transaction itself), DDL + data + commit, row count verified; row-at-a-time (`adbc.odbc.array_binding=false`) "
    "vs *array* = the driver default, parameter arrays where the ODBC driver handles them; pyodbc = `executemany` (`fast_executemany` where the driver allows). "
    "**Fetch** = full read of the second *Rows* count, median of %d after a warmup; `fetch_arrow_table()` vs pyodbc "
    "`fetchall()` -> `pyarrow.Table`; *native* = the same read with [native delegation](../README.md#native-delegation) "
    "handing the connection to the database's own ADBC driver. All rates are rows/s; higher is better. "
    "Servers run locally in Docker, so numbers reflect the ODBC driver + database, not the network."
    % (datetime.date.today(), args.reps),
    "",
    "| Database | Rows | Ingest | Ingest (array) | Ingest pyodbc | Fetch | Fetch pyodbc | Fetch vs pyodbc | Fetch native |",
    "|---|---:|---:|---:|---:|---:|---:|---:|---:|",
]
notes = []
for name, r in results.items():
    if "error" in r:
        lines.append("| %s | | error | | | | | | |" % name)
        notes.append("* **%s**: %s" % (name, r["error"]))
        continue
    label = "%s (%s)" % (name, r["vendor"])
    if r.get("read_only"):
        lines.append("| %s | — / %s | read-only driver | — | — | %s | — | — | — |" % (
            label, "{:,}".format(r["fetch_rows"]), fmt_rate(rate(r["fetch_rows"], r.get("fetch")))))
        continue
    f, fp = rate(r["fetch_rows"], r.get("fetch")), rate(r["fetch_rows"], r.get("fetch_pyodbc"))
    lines.append("| %s | %s / %s | %s | %s | %s | %s | %s | %s | %s |" % (
        label, "{:,}".format(r["rows"]), "{:,}".format(r["fetch_rows"]), fmt_rate(rate(r["rows"], r.get("ingest"))), fmt_rate(rate(r["rows"], r.get("ingest_array"))),
        fmt_rate(rate(r["rows"], r.get("ingest_pyodbc"))), fmt_rate(f), fmt_rate(fp),
        ("%.1f×" % (f / fp)) if f and fp else "—", fmt_rate(rate(r["fetch_rows"], r.get("fetch_native")))))
    for k in ("ingest", "ingest_array", "ingest_pyodbc", "fetch", "fetch_pyodbc"):
        if isinstance(r.get(k), dict) and "error" in r[k]:
            notes.append("* **%s** %s: %s" % (name, k, r[k]["error"]))
if notes:
    lines += ["", "Failures:", ""] + notes
out.write_text("\n".join(lines) + "\n")
print("wrote", out)
