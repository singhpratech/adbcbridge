# SPDX-License-Identifier: Apache-2.0
"""Per-database read benchmark: adbcbridge vs pyodbc on every database in the compat matrix.

    ADBC_ODBC_DRIVER=build/libadbc_driver_odbc.so <same *_ODBC_DRIVER env as tests/compat> \
        python bench/matrix_bench.py [--rows 200000] [--reps 3] [db ...]

Creates a 4-column table (id int, val double, txt varchar(20), dt date) through bulk ingest,
then times a full fetch of it: (a) adbcbridge cur.fetch_arrow_table(), (b) pyodbc fetchall()
-> pyarrow.Table. Writes bench/MATRIX_BENCHMARKS.md.
"""
import os, sys, time, statistics, datetime, pathlib, argparse
import pyarrow as pa
import adbc_driver_manager.dbapi as dbapi

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent.parent / "tests" / "compat"))
import test_matrix as m  # noqa: E402

ap = argparse.ArgumentParser()
ap.add_argument("--rows", type=int, default=200_000)
ap.add_argument("--reps", type=int, default=3)
ap.add_argument("dbs", nargs="*")
args = ap.parse_args()

TABLE = "adbc_bench"
TYPES = {  # per-database column types for CREATE TABLE
    "sqlite": "id INTEGER, val REAL, txt TEXT, dt DATE",
    "duckdb": "id INTEGER, val DOUBLE, txt VARCHAR, dt DATE",
    "postgres": "id INTEGER, val DOUBLE PRECISION, txt VARCHAR(20), dt DATE",
    "mariadb": "id INT, val DOUBLE, txt VARCHAR(20), dt DATE",
    "mysql": "id INT, val DOUBLE, txt VARCHAR(20), dt DATE",
    "oracle": "id NUMBER(10), val BINARY_DOUBLE, txt VARCHAR2(20), dt DATE",
    "clickhouse": "id Int32, val Float64, txt String, dt Date",
    "mssql": "id INT, val FLOAT, txt NVARCHAR(20), dt DATE",
}


def make_table(n):
    return pa.table({
        "id": pa.array(range(n), pa.int32()),
        "val": pa.array([i * 0.5 for i in range(n)], pa.float64()),
        "txt": pa.array(["row-%012d" % i for i in range(n)]),
        "dt": pa.array([i % 20000 for i in range(n)], pa.date32()),
    })


def bench(name, cfg):
    drv = os.environ.get(cfg["env"])
    if not drv:
        return None
    for kv in cfg.get("unicode_env", "").split():
        k, v = kv.split("=", 1); os.environ.setdefault(k, v)
    uri = os.environ.get(name.upper() + "_CONN", cfg["conn"]).format(drv=drv)
    ident = cfg.get("ident", lambda x: x)
    conn = dbapi.connect(driver=m.DRIVER, db_kwargs={"uri": uri}, autocommit=True)
    info = conn.adbc_get_info()
    vendor = "%s %s" % (info["vendor_name"], info["vendor_version"])
    n = args.rows if cfg.get("rowcount", True) or name == "clickhouse" else args.rows
    with conn.cursor() as cur:
        for sql in cfg.get("setup", []):
            cur.execute(sql)
        try:
            cur.execute("DROP TABLE " + ident(TABLE))
        except Exception:
            pass
        cols = TYPES.get(name, "id INTEGER, val DOUBLE PRECISION, txt VARCHAR(20), dt DATE")
        extra = " ENGINE = MergeTree ORDER BY id" if name == "clickhouse" else ""
        cur.execute("CREATE TABLE %s (%s)%s" % (ident(TABLE), cols, extra))
        tbl = make_table(n)
        t0 = time.perf_counter()
        cur.adbc_ingest(ident(TABLE), tbl, mode="append")
        t_ingest = time.perf_counter() - t0
    sql = "SELECT id, val, txt, dt FROM %s" % ident(TABLE)

    def run_adbc():
        with conn.cursor() as cur:
            cur.execute(sql)
            t = cur.fetch_arrow_table()
        assert t.num_rows == n, t.num_rows
    t_adbc = []
    run_adbc()
    for _ in range(args.reps):
        t0 = time.perf_counter(); run_adbc(); t_adbc.append(time.perf_counter() - t0)
    conn.close()

    t_pyodbc = []
    try:
        import pyodbc
        pc = pyodbc.connect(uri, autocommit=True)
        for sql_setup in cfg.get("setup", []):
            pc.execute(sql_setup)
        def run_pyodbc():
            rows = pc.execute(sql).fetchall()
            cols_ = list(zip(*rows)) if rows else [[], [], [], []]
            t = pa.table({"id": cols_[0], "val": cols_[1], "txt": cols_[2], "dt": cols_[3]})
            assert t.num_rows == n
        run_pyodbc()
        for _ in range(args.reps):
            t0 = time.perf_counter(); run_pyodbc(); t_pyodbc.append(time.perf_counter() - t0)
        pc.close()
    except Exception as e:  # noqa: BLE001
        t_pyodbc = None
        pyodbc_err = str(e).splitlines()[0][:80]
    return dict(vendor=vendor, rows=n, ingest=t_ingest, adbc=statistics.median(t_adbc),
                pyodbc=(statistics.median(t_pyodbc) if t_pyodbc else None),
                pyodbc_err=(None if t_pyodbc else pyodbc_err))


import json
cache = pathlib.Path(__file__).resolve().parent / ".matrix_bench.json"
results = json.loads(cache.read_text()) if cache.exists() else {}
for name in (args.dbs or list(m.DBS)):
    try:
        r = bench(name, m.DBS[name])
    except Exception as e:  # noqa: BLE001
        r = dict(error="%s: %s" % (type(e).__name__, str(e).splitlines()[0][:120]))
    if r is None:
        continue
    results[name] = r
    if "error" in r:
        print("%-10s ERROR %s" % (name, r["error"]))
    else:
        sp = ("%.2fx" % (r["pyodbc"] / r["adbc"])) if r["pyodbc"] else "n/a (%s)" % r["pyodbc_err"]
        print("%-10s %-28s rows=%d ingest=%.2fs adbc=%.3fs pyodbc=%s speedup=%s" % (
            name, r["vendor"], r["rows"], r["ingest"], r["adbc"],
            ("%.3fs" % r["pyodbc"]) if r["pyodbc"] else "n/a", sp))

cache.write_text(json.dumps(results, indent=1))
out = pathlib.Path(__file__).resolve().parent / "MATRIX_BENCHMARKS.md"
lines = ["<!-- SPDX-License-Identifier: Apache-2.0 -->", "# Per-database benchmarks", "",
         "Full fetch of %d rows `(int, double, varchar(20), date)`; median of %d runs after a warmup; "
         "`adbcbridge` = `cur.fetch_arrow_table()`, `pyodbc` = `fetchall()` -> `pyarrow.Table`. "
         "Generated by `bench/matrix_bench.py` on %s. Servers run locally in Docker, so numbers are "
         "dominated by the ODBC driver + database, not the network." % (args.rows, args.reps, datetime.date.today()),
         "", "| Database | Rows | Bulk ingest | adbcbridge fetch | pyodbc fetch | Speed-up | Rows/s (adbcbridge) |", "|---|---:|---:|---:|---:|---:|---:|"]
for name, r in results.items():
    if "error" in r:
        lines.append("| %s | — | — | — | — | error: %s | — |" % (name, r["error"])); continue
    lines.append("| %s (%s) | %d | %.2f s | **%.3f s** | %s | %s | %s |" % (
        name, r["vendor"], r["rows"], r["ingest"], r["adbc"],
        ("%.3f s" % r["pyodbc"]) if r["pyodbc"] else "n/a", 
        ("%.1f×" % (r["pyodbc"] / r["adbc"])) if r["pyodbc"] else "—",
        "{:,.0f}".format(r["rows"] / r["adbc"])))
out.write_text("\n".join(lines) + "\n")
print("wrote", out)
