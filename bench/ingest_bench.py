#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Bulk-ingest benchmark for the adbc-odbc driver.

Ingests a table of mixed Arrow types into SQLite through the driver twice --
once with column-wise array parameter binding (``adbc.odbc.array_binding=true``,
the default) and once with the row-at-a-time fallback -- and prints rows/sec for
each.

Usage::

    SQLITE_ODBC_DRIVER=/path/to/libsqlite3odbc.so \\
        python bench/ingest_bench.py [--rows 200000] [--batch-size 8192]
"""

import argparse
import os
import pathlib
import sys
import tempfile
import time

import pyarrow as pa
import adbc_driver_manager.dbapi as dbapi

HERE = pathlib.Path(__file__).resolve().parent
DRIVER = os.environ.get(
    "ADBC_ODBC_DRIVER", str(HERE.parent / "build" / "libadbc_driver_odbc.so")
)
SQLITE_ODBC = os.environ.get("SQLITE_ODBC_DRIVER", "SQLite3")

ARRAY_BINDING = "adbc.odbc.array_binding"


def make_table(rows: int, batch_size: int) -> pa.Table:
    """A mixed-type table: ints, float, string, blob, date, timestamp, bool."""
    i64 = pa.array(range(rows), pa.int64())
    i32 = pa.array([(i * 7) % 100000 for i in range(rows)], pa.int32())
    f64 = pa.array([i * 1.5 for i in range(rows)], pa.float64())
    # ~20 byte strings, with a null every 97th row
    s = pa.array(
        [None if i % 97 == 0 else f"row-{i:012d}" for i in range(rows)], pa.string()
    )
    b = pa.array([(i % 251).to_bytes(1, "little") * 8 for i in range(rows)], pa.binary())
    d = pa.array([i % 20000 for i in range(rows)], pa.date32())
    ts = pa.array([1_700_000_000_000_000 + i * 1000 for i in range(rows)],
                  pa.timestamp("us"))
    flag = pa.array([i % 2 == 0 for i in range(rows)], pa.bool_())
    tbl = pa.table(
        {"i64": i64, "i32": i32, "f64": f64, "s": s, "b": b, "d": d, "ts": ts,
         "flag": flag}
    )
    # Hand the driver batches of exactly `batch_size` rows.
    return pa.Table.from_batches(tbl.to_batches(max_chunksize=batch_size), tbl.schema)


def connect(path: str):
    uri = f"Driver={SQLITE_ODBC};Database={path};"
    return dbapi.connect(driver=DRIVER, db_kwargs={"uri": uri})


def run(tbl: pa.Table, array_binding: bool) -> tuple[float, int]:
    tmp = tempfile.mkdtemp()
    db = os.path.join(tmp, "bench.db")
    try:
        with connect(db) as conn:
            conn.adbc_connection.set_autocommit(False)
            with conn.cursor() as cur:
                cur._stmt.set_options(**{ARRAY_BINDING: "true" if array_binding else "false"})
                start = time.perf_counter()
                n = cur.adbc_ingest("bench", tbl, mode="create")
                conn.commit()
                elapsed = time.perf_counter() - start
            with conn.cursor() as cur:
                cur.execute("SELECT COUNT(*) FROM bench")
                got = cur.fetchone()[0]
        if got != tbl.num_rows:
            raise SystemExit(f"row count mismatch: inserted {got}, expected {tbl.num_rows}")
        if n not in (tbl.num_rows, -1):
            print(f"  warning: driver reported {n} rows affected, expected {tbl.num_rows}")
        return elapsed, got
    finally:
        try:
            os.remove(db)
        except OSError:
            pass


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--rows", type=int, default=200_000)
    ap.add_argument("--batch-size", type=int, default=8192,
                    help="Arrow record-batch size handed to the driver")
    ap.add_argument("--repeat", type=int, default=1)
    args = ap.parse_args(argv)

    print(f"driver:      {DRIVER}")
    print(f"odbc driver: {SQLITE_ODBC}")
    print(f"rows:        {args.rows}  batch size: {args.batch_size}")
    tbl = make_table(args.rows, args.batch_size)
    print(f"schema:      {', '.join(f.name + ':' + str(f.type) for f in tbl.schema)}")
    print()

    results = {}
    for label, array_binding in (("array binding ", True), ("row-at-a-time ", False)):
        best = None
        for _ in range(args.repeat):
            elapsed, rows = run(tbl, array_binding)
            best = elapsed if best is None else min(best, elapsed)
        results[label] = best
        print(f"{label}: {best:8.3f} s   {rows / best:12,.0f} rows/sec")

    fast, slow = results["array binding "], results["row-at-a-time "]
    print()
    print(f"speedup:        {slow / fast:.2f}x")
    print(f"saved:          {(slow - fast) / args.rows * 1e6:.2f} us/row")
    print()
    print("Both runs ingest inside a single transaction, so this measures binding "
          "and\nper-execute overhead only.  How much array binding buys depends on the "
          "ODBC\ndriver: SQLite's driver walks the parameter array itself in-process, "
          "so the\nsaving is just driver-manager dispatch, whereas a client/server "
          "driver turns\none SQLExecute into one round trip instead of N.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
