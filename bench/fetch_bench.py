#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Read-path benchmark for the adbcbridge ODBC->Arrow driver.

Builds a SQLite table of N rows (int, double, 20-char text, date) and times
fetching the whole table three ways:

  (a) adbcbridge via adbc_driver_manager  -> cur.fetch_arrow_table()
  (b) pyodbc fetchall()                   -> pyarrow.Table
  (c) pyodbc fetchall()                   -> pandas.DataFrame   (if pandas present)

It also sweeps adbc.odbc.batch_size over 1024 / 8192 / 65536, and can measure a
raw-ODBC "floor" (bench/odbc_floor.c: SQLBindCol + SQLFetch, discarding data) so
the cost of our Arrow conversion can be separated from the cost of the
underlying ODBC driver.

Usage:
  SQLITE_ODBC_DRIVER=/path/to/libsqlite3odbc.so python bench/fetch_bench.py
  ... --rows 1000000 --reps 3 --json out.json --profile --per-column
"""

import argparse
import json
import os
import pathlib
import random
import shutil
import sqlite3
import statistics
import subprocess
import sys
import tempfile
import time

HERE = pathlib.Path(__file__).resolve().parent
ROOT = HERE.parent

DEFAULT_DRIVER = os.environ.get("ADBC_ODBC_DRIVER", str(ROOT / "build" / "libadbc_driver_odbc.so"))
SQLITE_ODBC = os.environ.get("SQLITE_ODBC_DRIVER", "SQLite3")

TABLE = "bench"
COLUMNS = ("id", "val", "txt", "dt")
QUERY = "SELECT id, val, txt, dt FROM %s" % TABLE
BATCH_SIZES = (1024, 8192, 65536)

ALPHABET = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789"


# --------------------------------------------------------------------------
# data generation


def build_db(path, rows, force=False):
    """Create `rows` rows of (int, double, 20-char text, date). Reuses an
    existing file whose row count already matches."""
    if os.path.exists(path) and not force:
        try:
            con = sqlite3.connect(path)
            n = con.execute("SELECT COUNT(*) FROM %s" % TABLE).fetchone()[0]
            con.close()
            if n == rows:
                print("reusing %s (%d rows, %.1f MiB)"
                      % (path, n, os.path.getsize(path) / 2**20))
                return
        except sqlite3.Error:
            pass
        os.remove(path)

    print("building %s with %d rows ..." % (path, rows), flush=True)
    t0 = time.perf_counter()
    con = sqlite3.connect(path)
    con.execute("PRAGMA journal_mode=OFF")
    con.execute("PRAGMA synchronous=OFF")
    # Declared types matter: the ODBC driver maps them to SQL types, which is
    # what decides whether our reader can bind the column for block fetches.
    con.execute("CREATE TABLE %s (id INTEGER, val DOUBLE, txt VARCHAR(20), dt DATE)" % TABLE)

    rnd = random.Random(20240822)
    chunk = 50000

    def gen():
        for i in range(rows):
            txt = "".join(rnd.choice(ALPHABET) for _ in range(20))
            dt = "%04d-%02d-%02d" % (2000 + (i % 25), (i % 12) + 1, (i % 28) + 1)
            yield (i, i * 1.5 + 0.25, txt, dt)

    it = gen()
    while True:
        batch = [row for _, row in zip(range(chunk), it)]
        if not batch:
            break
        con.executemany("INSERT INTO %s VALUES (?,?,?,?)" % TABLE, batch)
    con.commit()
    con.close()
    print("  built in %.1fs (%.1f MiB)"
          % (time.perf_counter() - t0, os.path.getsize(path) / 2**20), flush=True)


def conn_string(db):
    return "Driver=%s;Database=%s;" % (SQLITE_ODBC, db)


# --------------------------------------------------------------------------
# the three fetch paths


def fetch_adbc(db, batch_size, query=QUERY, max_bind_bytes=None):
    """(a) adbcbridge -> pyarrow.Table. Returns (seconds, nrows, nbatches)."""
    import adbc_driver_manager.dbapi as dbapi

    db_kwargs = {"uri": conn_string(db)}
    if batch_size is not None:
        db_kwargs["adbc.odbc.batch_size"] = str(batch_size)
    if max_bind_bytes is not None:
        db_kwargs["adbc.odbc.max_bind_bytes"] = str(max_bind_bytes)
    with dbapi.connect(driver=DEFAULT_DRIVER, db_kwargs=db_kwargs) as conn:
        with conn.cursor() as cur:
            t0 = time.perf_counter()
            cur.execute(query)
            tbl = cur.fetch_arrow_table()
            dt = time.perf_counter() - t0
            return dt, tbl.num_rows, tbl.column(0).num_chunks


def fetch_pyodbc_arrow(db, query=QUERY):
    """(b) pyodbc fetchall() -> pyarrow.Table.

    Returns (total_seconds, fetch_seconds, nrows)."""
    import pyarrow as pa
    import pyodbc

    with pyodbc.connect(conn_string(db)) as conn:
        cur = conn.cursor()
        t0 = time.perf_counter()
        cur.execute(query)
        rows = cur.fetchall()
        t_fetch = time.perf_counter() - t0
        cols = list(zip(*rows)) if rows else [() for _ in COLUMNS]
        tbl = pa.table({name: pa.array(col) for name, col in zip(COLUMNS, cols)})
        total = time.perf_counter() - t0
        return total, t_fetch, tbl.num_rows


def fetch_pyodbc_pandas(db, query=QUERY):
    """(c) pyodbc fetchall() -> pandas.DataFrame.

    Returns (total_seconds, fetch_seconds, nrows)."""
    import pandas as pd
    import pyodbc

    with pyodbc.connect(conn_string(db)) as conn:
        cur = conn.cursor()
        t0 = time.perf_counter()
        cur.execute(query)
        rows = cur.fetchall()
        t_fetch = time.perf_counter() - t0
        df = pd.DataFrame.from_records([tuple(r) for r in rows], columns=list(COLUMNS))
        total = time.perf_counter() - t0
        return total, t_fetch, len(df)


def fetch_sqlite3(db, query=QUERY):
    """Reference floor: the stdlib sqlite3 driver, no ODBC in the path."""
    con = sqlite3.connect(db)
    t0 = time.perf_counter()
    rows = con.execute(query).fetchall()
    dt = time.perf_counter() - t0
    con.close()
    return dt, len(rows)


# --------------------------------------------------------------------------
# raw-ODBC floor (optional, needs a C compiler)


def build_odbc_floor():
    """Compile bench/odbc_floor.c. Returns the exe path or None."""
    src = HERE / "odbc_floor.c"
    if not src.exists():
        return None
    cc = shutil.which("cc") or shutil.which("gcc")
    if not cc:
        return None
    exe = pathlib.Path(tempfile.gettempdir()) / "adbcbridge_odbc_floor"
    r = subprocess.run([cc, "-O2", "-o", str(exe), str(src), "-lodbc"],
                       capture_output=True, text=True)
    if r.returncode != 0:
        print("  (odbc_floor build failed: %s)" % r.stderr.strip()[:200])
        return None
    return exe


def run_odbc_floor(exe, db, batch_size, query=QUERY):
    r = subprocess.run([str(exe), conn_string(db), query, str(batch_size)],
                       capture_output=True, text=True)
    if r.returncode != 0:
        print("  (odbc_floor failed: %s)" % (r.stderr or r.stdout).strip()[:300])
        return None
    return json.loads(r.stdout)


# --------------------------------------------------------------------------
# harness


def repeat(fn, reps):
    """Run fn() reps times after one warmup; return (median, all_times, extra)."""
    fn()  # warmup: fills the OS page cache and any driver-side caches
    times = []
    extra = None
    for _ in range(reps):
        out = fn()
        times.append(out[0])
        extra = out[1:]
    return statistics.median(times), times, extra


def fmt_rate(rows, seconds):
    return "%.2f M rows/s" % (rows / seconds / 1e6) if seconds > 0 else "n/a"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--rows", type=int, default=1_000_000)
    ap.add_argument("--reps", type=int, default=3)
    ap.add_argument("--db", default=None, help="path to the SQLite file to build/reuse")
    ap.add_argument("--rebuild", action="store_true")
    ap.add_argument("--json", default=None, help="write raw results here")
    ap.add_argument("--profile", action="store_true", help="cProfile around the ADBC fetch")
    ap.add_argument("--per-column", action="store_true",
                    help="time each column separately to attribute conversion cost")
    ap.add_argument("--no-floor", action="store_true", help="skip the raw-ODBC floor")
    ap.add_argument("--unbound-penalty", action="store_true",
                    help="measure the cost of falling off the block-cursor path")
    ap.add_argument("--sweep", type=int, default=0, metavar="ROUNDS",
                    help="only sweep adbc.odbc.batch_size, interleaving the sizes over "
                         "ROUNDS rounds so machine drift cannot favour one size")
    args = ap.parse_args()

    if SQLITE_ODBC == "SQLite3":
        print("note: set SQLITE_ODBC_DRIVER to the SQLite ODBC .so path", file=sys.stderr)
    if not os.path.exists(DEFAULT_DRIVER):
        sys.exit("driver not built: %s" % DEFAULT_DRIVER)

    db = args.db or os.path.join(tempfile.gettempdir(), "adbcbridge_bench_%d.db" % args.rows)
    build_db(db, args.rows, force=args.rebuild)

    results = {
        "rows": args.rows,
        "reps": args.reps,
        "db_bytes": os.path.getsize(db),
        "adbc": {},
    }

    if args.sweep:
        # Interleaved: round 1 runs every batch size once, then round 2, ...
        # Machine drift (thermal, background load) then hits all sizes alike.
        print("\nbatch_size sweep: %d rows, %d interleaved rounds\n" % (args.rows, args.sweep))
        times = {bs: [] for bs in BATCH_SIZES}
        for bs in BATCH_SIZES:
            fetch_adbc(db, bs)  # warmup
        for rnd in range(args.sweep):
            for bs in BATCH_SIZES:
                dt, nrows, _ = fetch_adbc(db, bs)
                assert nrows == args.rows
                times[bs].append(dt)
            print("  round %d: %s" % (rnd + 1,
                  "  ".join("%d=%.3f" % (bs, times[bs][-1]) for bs in BATCH_SIZES)))
        print()
        base = statistics.median(times[BATCH_SIZES[0]])
        for bs in BATCH_SIZES:
            med = statistics.median(times[bs])
            results["adbc"][bs] = {"median": med, "times": times[bs],
                                   "min": min(times[bs]), "max": max(times[bs])}
            print("batch_size=%-6d median %.3f s  min %.3f  max %.3f  %-14s  %+.1f%% vs %d"
                  % (bs, med, min(times[bs]), max(times[bs]), fmt_rate(args.rows, med),
                     100.0 * (med - base) / base, BATCH_SIZES[0]))
        if args.json:
            with open(args.json, "w") as f:
                json.dump(results, f, indent=2)
            print("wrote %s" % args.json)
        return

    print("\n%d rows, median of %d runs (+1 warmup)\n" % (args.rows, args.reps))

    # (a) our driver, at each batch size
    for bs in BATCH_SIZES:
        med, times, extra = repeat(lambda bs=bs: fetch_adbc(db, bs), args.reps)
        nrows, nbatches = extra
        assert nrows == args.rows, "got %d rows, want %d" % (nrows, args.rows)
        results["adbc"][bs] = {"median": med, "times": times, "batches": nbatches}
        print("adbc  batch_size=%-6d %7.3f s  %-14s  (%d batches)  runs=%s"
              % (bs, med, fmt_rate(nrows, med), nbatches,
                 " ".join("%.3f" % t for t in times)))

    best_bs = min(results["adbc"], key=lambda k: results["adbc"][k]["median"])
    adbc_best = results["adbc"][best_bs]["median"]

    # (b) pyodbc -> pyarrow
    med, times, extra = repeat(lambda: fetch_pyodbc_arrow(db), args.reps)
    results["pyodbc_arrow"] = {"median": med, "times": times, "fetch_only": extra[0]}
    print("\npyodbc -> pyarrow.Table  %7.3f s  %-14s  (fetchall alone %.3f s)"
          % (med, fmt_rate(args.rows, med), extra[0]))

    # (c) pyodbc -> pandas
    try:
        med, times, extra = repeat(lambda: fetch_pyodbc_pandas(db), args.reps)
        results["pyodbc_pandas"] = {"median": med, "times": times, "fetch_only": extra[0]}
        print("pyodbc -> pandas.DataFrame %5.3f s  %-14s  (fetchall alone %.3f s)"
              % (med, fmt_rate(args.rows, med), extra[0]))
    except ImportError:
        print("pyodbc -> pandas: pandas not installed, skipped")

    # reference: no ODBC at all
    med, times, _ = repeat(lambda: fetch_sqlite3(db), args.reps)
    results["sqlite3"] = {"median": med, "times": times}
    print("sqlite3 (no ODBC)        %7.3f s  %s" % (med, fmt_rate(args.rows, med)))

    # raw-ODBC floor: how much of our time is the ODBC driver itself
    if not args.no_floor:
        exe = build_odbc_floor()
        if exe:
            results["odbc_floor"] = {}
            print()
            for bs in BATCH_SIZES:
                best = None
                for _ in range(args.reps + 1):
                    out = run_odbc_floor(exe, db, bs)
                    if out is None:
                        break
                    best = out if best is None else min(best, out, key=lambda o: o["seconds"])
                if best is None:
                    break
                results["odbc_floor"][bs] = best
                share = 100.0 * best["seconds"] / adbc_best
                print("raw ODBC floor batch=%-6d %7.3f s  %-14s  = %2.0f%% of our best"
                      "   [SQLExecDirect %.3f s + SQLFetch loop %.3f s, %d SQLFetch calls]"
                      % (bs, best["seconds"], fmt_rate(best["rows"], best["seconds"]), share,
                         best["exec_seconds"], best["fetch_seconds"], best["fetch_calls"]))

    if args.per_column:
        print("\nper-column (SELECT one column, %d rows, batch_size=%d):" % (args.rows, best_bs))
        results["per_column"] = {}
        for col in COLUMNS:
            q = "SELECT %s FROM %s" % (col, TABLE)
            med, _, _ = repeat(lambda q=q: fetch_adbc(db, best_bs, q), args.reps)
            results["per_column"][col] = med
            print("  %-4s %7.3f s  %s" % (col, med, fmt_rate(args.rows, med)))
        allc = results["adbc"][best_bs]["median"]
        summed = sum(results["per_column"].values())
        print("  all four together %.3f s (sum of singles %.3f s)" % (allc, summed))

    if args.unbound_penalty:
        # src/odbc_reader.c: ReaderBind() uses a block cursor only if EVERY column
        # could be bound; one oversized/unbounded column forces rows_per_fetch=1
        # and routes every value of every column through SQLGetData. Shrinking
        # max_bind_bytes below the text column's width triggers exactly that.
        print("\nunbound-column cliff (batch_size=%d):" % best_bs)
        med_lo, _, _ = repeat(
            lambda: fetch_adbc(db, best_bs, max_bind_bytes=8), max(2, args.reps // 2))
        med_hi = results["adbc"][best_bs]["median"]
        results["unbound_penalty"] = {"all_bound": med_hi, "one_unbound": med_lo}
        print("  all columns bound (block cursor) %7.3f s  %s" % (med_hi, fmt_rate(args.rows, med_hi)))
        print("  one column unbound (row-at-a-time) %5.3f s  %s" % (med_lo, fmt_rate(args.rows, med_lo)))
        print("  penalty: %.2fx slower" % (med_lo / med_hi))

    if args.profile:
        import cProfile
        import pstats
        import io as _io
        print("\ncProfile around fetch_arrow_table (batch_size=%d):" % best_bs)
        pr = cProfile.Profile()
        pr.enable()
        fetch_adbc(db, best_bs)
        pr.disable()
        s = _io.StringIO()
        pstats.Stats(pr, stream=s).sort_stats("cumulative").print_stats(12)
        print(s.getvalue())
        results["profile"] = s.getvalue()

    print("\nsummary: our driver is %.2fx pyodbc->pyarrow at batch_size=%d"
          % (results["pyodbc_arrow"]["median"] / adbc_best, best_bs))

    if args.json:
        with open(args.json, "w") as f:
            json.dump(results, f, indent=2)
        print("wrote %s" % args.json)


if __name__ == "__main__":
    main()
