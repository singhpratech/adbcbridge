#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Partitioned-read benchmark: adbcbridge vs the native PostgreSQL ADBC driver.

Times a full read of the same query three ways, wall clock, end to end:

  (a) adbcbridge over psqlodbc, split with AdbcStatementExecutePartitions into N
      partitions read on N threads over N connections and concatenated
  (b) adbc_driver_postgresql (libpq, the native ADBC driver) reading the whole query
  (c) bench/odbc_floor.c -- SQLBindCol + SQLFetch over psqlodbc, converting nothing,
      which is the cost our driver cannot avoid

Every timing is end to end: opening the connections, executing, fetching and building
the Arrow table are all inside the clock, because that is what a caller waits for.  The
A/B/C variants are interleaved and repeated, and the median is reported, so that a spike
in machine load lands on all of them rather than on whichever ran first.

Usage:
  POSTGRES_ODBC_DRIVER=/path/psqlodbcw.so \\
  ADBC_ODBC_DRIVER=/path/libadbc_driver_odbc.so \\
      python bench/partition_bench.py \\
      --pg 'postgresql://user:pw@127.0.0.1:5432/db' --table bench1m --reps 5
"""

import argparse
import concurrent.futures
import json
import os
import pathlib
import statistics
import subprocess
import sys
import time
import urllib.parse

import adbc_driver_manager as adm
import pyarrow

HERE = pathlib.Path(__file__).resolve().parent
ROOT = HERE.parent

ODBC_DRIVER = os.environ.get("ADBC_ODBC_DRIVER", str(ROOT / "build" / "libadbc_driver_odbc.so"))
PSQLODBC = os.environ.get("POSTGRES_ODBC_DRIVER", "PostgreSQL Unicode")

COLUMNS = ("id", "val", "txt", "dt")


def odbc_connstr(pg_url, extra=""):
    """psqlodbc connection string from a postgresql:// URL."""
    u = urllib.parse.urlparse(pg_url)
    return (
        "DRIVER=%s;SERVER=%s;PORT=%d;DATABASE=%s;UID=%s;PWD=%s;%s"
        % (PSQLODBC, u.hostname or "127.0.0.1", u.port or 5432,
           (u.path or "/postgres").lstrip("/"), u.username or "", u.password or "", extra)
    )


# --------------------------------------------------------------------------
# (a) adbcbridge, partitioned


def _open(connstr, options=None):
    opts = {"adbc.odbc.connection_string": connstr, "adbc.odbc.delegate": "never"}
    opts.update(options or {})
    db = adm.AdbcDatabase(driver=ODBC_DRIVER, entrypoint="AdbcDriverInit", **opts)
    return db, adm.AdbcConnection(db)


def _stream_to_table(handle):
    return pyarrow.RecordBatchReader._import_from_c(handle.address).read_all()


def read_partition(connstr, descriptor, options=None):
    """One worker: its own database, connection and statement handle."""
    db, conn = _open(connstr, options)
    try:
        return _stream_to_table(conn.read_partition(descriptor))
    finally:
        conn.close()
        db.close()


def run_bridge(connstr, query, nparts, options=None, pool=None):
    """Split into `nparts`, read them concurrently, concatenate.  Wall clock."""
    t0 = time.perf_counter()
    db, conn = _open(connstr, options)
    try:
        stmt = adm.AdbcStatement(conn)
        stmt.set_options(**{"adbc.odbc.partitions": str(nparts)})
        stmt.set_sql_query(query)
        descriptors, _schema, _rows = stmt.execute_partitions()
        descriptors = [bytes(d) for d in descriptors]
        stmt.close()
        if len(descriptors) == 1:
            tables = [_stream_to_table(conn.read_partition(descriptors[0]))]
        else:
            futures = [pool.submit(read_partition, connstr, d, options) for d in descriptors]
            tables = [f.result() for f in futures]
    finally:
        conn.close()
        db.close()
    table = pyarrow.concat_tables(tables)
    return table, time.perf_counter() - t0, len(descriptors)


def run_bridge_plain(connstr, query, options=None):
    """adbcbridge with no partitioning at all -- ExecuteQuery on one connection."""
    t0 = time.perf_counter()
    db, conn = _open(connstr, options)
    try:
        stmt = adm.AdbcStatement(conn)
        stmt.set_sql_query(query)
        handle, _ = stmt.execute_query()
        table = _stream_to_table(handle)
        stmt.close()
    finally:
        conn.close()
        db.close()
    return table, time.perf_counter() - t0


# --------------------------------------------------------------------------
# (b) native adbc_driver_postgresql


def run_native(pg_url, query):
    import adbc_driver_postgresql.dbapi as pgdbapi

    t0 = time.perf_counter()
    with pgdbapi.connect(pg_url) as conn:
        with conn.cursor() as cur:
            cur.execute(query)
            table = cur.fetch_arrow_table()
    return table, time.perf_counter() - t0


# --------------------------------------------------------------------------
# (c) raw ODBC floor


def build_floor():
    exe = ROOT / "build" / "odbc_floor"
    src = HERE / "odbc_floor.c"
    if exe.exists() and exe.stat().st_mtime >= src.stat().st_mtime:
        return str(exe)
    exe.parent.mkdir(parents=True, exist_ok=True)
    try:
        subprocess.run(["cc", "-O2", "-o", str(exe), str(src), "-lodbc"], check=True)
    except (subprocess.CalledProcessError, FileNotFoundError):
        return None
    return str(exe)


def run_floor(exe, connstr, query, batch=1024):
    t0 = time.perf_counter()
    out = subprocess.run([exe, connstr, query, str(batch)], capture_output=True, text=True)
    wall = time.perf_counter() - t0
    if out.returncode != 0:
        return None, None
    try:
        return json.loads(out.stdout), wall
    except json.JSONDecodeError:
        return None, wall


# --------------------------------------------------------------------------
# checksum: the equivalence proof, not just a row count


def checksum(table):
    """Order-independent fingerprint of a whole result set."""
    import pyarrow.compute as pc

    out = {"rows": table.num_rows}
    for name in table.column_names:
        col = table.column(name)
        if pyarrow.types.is_integer(col.type) or pyarrow.types.is_floating(col.type):
            out[name] = pc.sum(col).as_py()
        elif pyarrow.types.is_date(col.type) or pyarrow.types.is_timestamp(col.type):
            # date32 has no direct int64 cast; go through the storage width.
            width = pyarrow.int32() if col.type == pyarrow.date32() else pyarrow.int64()
            out[name] = pc.sum(col.cast(width, safe=False).cast(pyarrow.int64())).as_py()
        else:
            # Sum of per-value lengths plus a cheap order-independent hash of the bytes.
            lengths = pc.sum(pc.binary_length(col)).as_py()
            out[name] = lengths
    return out


# --------------------------------------------------------------------------


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--pg", default=os.environ.get("PG_URI"),
                    help="postgresql:// URL for the native driver and psqlodbc")
    ap.add_argument("--table", default="bench1m")
    ap.add_argument("--parts", default="1,2,4,8",
                    help="comma-separated partition counts to measure")
    ap.add_argument("--reps", type=int, default=5)
    ap.add_argument("--prefetch", type=int, default=0,
                    help="adbc.odbc.prefetch for the adbcbridge runs")
    ap.add_argument("--declare-fetch", action="store_true",
                    help="add UseDeclareFetch=1 to the psqlodbc connection string")
    ap.add_argument("--json", help="write the raw timings here")
    ap.add_argument("--skip-native", action="store_true")
    ap.add_argument("--skip-floor", action="store_true")
    args = ap.parse_args()
    if not args.pg:
        ap.error("--pg (or PG_URI) is required")

    query = "SELECT %s FROM %s" % (", ".join(COLUMNS), args.table)
    extra = "UseDeclareFetch=1;Fetch=20000;" if args.declare_fetch else ""
    connstr = odbc_connstr(args.pg, extra)
    parts = [int(p) for p in args.parts.split(",")]
    options = {"adbc.odbc.prefetch": str(args.prefetch)} if args.prefetch else None

    floor_exe = None if args.skip_floor else build_floor()

    print("query : %s" % query)
    print("reps  : %d (medians reported)" % args.reps)
    print("psqlodbc extra: %r" % (extra or None))
    print("prefetch: %d" % args.prefetch)
    print()

    timings = {}
    reference = None

    def record(label, seconds):
        timings.setdefault(label, []).append(seconds)

    pool = concurrent.futures.ThreadPoolExecutor(max_workers=max(parts))
    try:
        # One warm-up round, discarded: it pays for page cache and for the ODBC driver's
        # first-connection work, neither of which the steady state repeats.
        for n in parts:
            run_bridge(connstr, query, n, options, pool)
        if not args.skip_native:
            run_native(args.pg, query)

        for rep in range(args.reps):
            # Interleave the variants so that load drifts across the run land on all of
            # them, not on whichever went first.
            for n in parts:
                table, sec, actual = run_bridge(connstr, query, n, options, pool)
                record("bridge N=%d" % n, sec)
                sig = checksum(table)
                if reference is None:
                    reference = sig
                elif sig != reference:
                    raise SystemExit("CHECKSUM MISMATCH at N=%d: %r vs %r" % (n, sig, reference))
                if rep == 0:
                    print("  N=%d -> %d partitions, %d rows" % (n, actual, table.num_rows))
            if not args.skip_native:
                table, sec = run_native(args.pg, query)
                record("native pg", sec)
                sig = checksum(table)
                if sig != reference:
                    print("  note: native checksum differs: %r vs %r" % (sig, reference))
            if floor_exe:
                _, sec = run_floor(floor_exe, connstr, query)
                if sec:
                    record("odbc floor", sec)
    finally:
        pool.shutdown()

    rows = reference["rows"]
    print()
    print("%-14s %9s %9s %9s %12s" % ("variant", "median s", "min s", "max s", "Mrow/s"))
    print("-" * 58)
    order = ["bridge N=%d" % n for n in parts] + ["native pg", "odbc floor"]
    med = {}
    for label in order:
        if label not in timings:
            continue
        vals = timings[label]
        m = statistics.median(vals)
        med[label] = m
        print("%-14s %9.3f %9.3f %9.3f %12.2f"
              % (label, m, min(vals), max(vals), rows / m / 1e6))
    print()
    if "native pg" in med:
        for n in parts:
            k = "bridge N=%d" % n
            if k in med:
                print("N=%d vs native: %.2fx" % (n, med["native pg"] / med[k]))

    if args.json:
        pathlib.Path(args.json).write_text(json.dumps(
            {"query": query, "rows": rows, "timings": timings, "checksum": reference}, indent=2))


if __name__ == "__main__":
    main()
