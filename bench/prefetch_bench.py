#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Measure `adbc.odbc.prefetch` -- overlapping SQLFetch with the Arrow conversion.

Reads the same query at several prefetch depths and reports the median wall clock, so
that the value of hiding the fetch behind the conversion can be read off per driver.
Depth 0 is the default synchronous reader; depth 1 is double buffering.

The runs are interleaved and the result set is checksummed at every depth against the
depth-0 read, so a depth that is faster but wrong cannot be reported as a win.

Usage:
  ADBC_ODBC_DRIVER=/path/libadbc_driver_odbc.so python bench/prefetch_bench.py \\
      --connstr 'DRIVER=/path/libsqlite3odbc.so;Database=/tmp/bench.db;' \\
      --query 'SELECT id, val, txt, dt FROM bench' --depths 0,1,2 --reps 5
"""

import argparse
import json
import os
import pathlib
import statistics
import time

import adbc_driver_manager as adm
import pyarrow
import pyarrow.compute as pc

ROOT = pathlib.Path(__file__).resolve().parent.parent
ODBC_DRIVER = os.environ.get("ADBC_ODBC_DRIVER", str(ROOT / "build" / "libadbc_driver_odbc.so"))


def read_once(connstr, query, depth, batch_size=None):
    """Full end-to-end read: connect, execute, fetch, build the Arrow table."""
    opts = {
        "adbc.odbc.connection_string": connstr,
        "adbc.odbc.delegate": "never",
        "adbc.odbc.prefetch": str(depth),
    }
    if batch_size:
        opts["adbc.odbc.batch_size"] = str(batch_size)
    t0 = time.perf_counter()
    db = adm.AdbcDatabase(driver=ODBC_DRIVER, entrypoint="AdbcDriverInit", **opts)
    conn = adm.AdbcConnection(db)
    try:
        stmt = adm.AdbcStatement(conn)
        stmt.set_sql_query(query)
        handle, _ = stmt.execute_query()
        table = pyarrow.RecordBatchReader._import_from_c(handle.address).read_all()
        stmt.close()
    finally:
        conn.close()
        db.close()
    return table, time.perf_counter() - t0


def checksum(table):
    out = {"rows": table.num_rows}
    for name in table.column_names:
        col = table.column(name)
        if pyarrow.types.is_integer(col.type) or pyarrow.types.is_floating(col.type):
            out[name] = pc.sum(col).as_py()
        elif pyarrow.types.is_date(col.type):
            out[name] = pc.sum(col.cast(pyarrow.int32(), safe=False)
                               .cast(pyarrow.int64())).as_py()
        elif pyarrow.types.is_timestamp(col.type):
            out[name] = pc.sum(col.cast(pyarrow.int64(), safe=False)).as_py()
        else:
            out[name] = pc.sum(pc.binary_length(col)).as_py()
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--connstr", required=True)
    ap.add_argument("--query", required=True)
    ap.add_argument("--depths", default="0,1,2")
    ap.add_argument("--reps", type=int, default=5)
    ap.add_argument("--batch-size", type=int)
    ap.add_argument("--label", default="")
    ap.add_argument("--json")
    args = ap.parse_args()

    depths = [int(d) for d in args.depths.split(",")]
    timings = {}
    reference = None

    read_once(args.connstr, args.query, 0, args.batch_size)  # warm-up, discarded
    for _ in range(args.reps):
        for d in depths:
            table, sec = read_once(args.connstr, args.query, d, args.batch_size)
            sig = checksum(table)
            if reference is None:
                reference = sig
            elif sig != reference:
                raise SystemExit("CHECKSUM MISMATCH at prefetch=%d: %r vs %r"
                                 % (d, sig, reference))
            timings.setdefault(d, []).append(sec)

    rows = reference["rows"]
    print("%s (%d rows)" % (args.label or args.query, rows))
    print("%-10s %9s %9s %9s %10s %8s" % ("prefetch", "median s", "min s", "max s",
                                          "Mrow/s", "vs 0"))
    base = statistics.median(timings[depths[0]])
    for d in depths:
        vals = timings[d]
        m = statistics.median(vals)
        print("%-10d %9.3f %9.3f %9.3f %10.2f %7.2fx"
              % (d, m, min(vals), max(vals), rows / m / 1e6, base / m))

    if args.json:
        pathlib.Path(args.json).write_text(json.dumps(
            {"query": args.query, "rows": rows,
             "timings": {str(k): v for k, v in timings.items()}}, indent=2))


if __name__ == "__main__":
    main()
