#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Threshold benchmark: adbcbridge against the *native* ADBC driver.

Measures two axes -- fetch and ingest -- and prints PASS/FAIL against a speedup
threshold (default 1.2x, i.e. adbcbridge must be at least 1.2 times faster than
the native ADBC driver for the same database).

Everything a caller waits for is inside the clock: opening the connection(s),
executing, and materialising the Arrow table for a fetch; opening the
connection(s), consuming an already-built in-memory Arrow table, executing and
committing for an ingest.  Building or reading the *source* table happens before
the clock starts.

Every timed run happens in a **fresh process**: this script re-executes itself as
``--worker KIND`` for each measurement, so that no warmed-up allocator, cached
statement or already-loaded shared library can carry a measurement.  The four
kinds are interleaved (A,B,A,B,...) so that a drift in machine load lands on both
sides rather than on whichever ran first.

A run is only a PASS if it is also *correct*: after a fetch the resulting Arrow
table is fingerprinted in Python with an order-independent checksum (a
partitioned read hands the rows back in a different order), and after an ingest
the target table is checksummed **in SQL on the server**, so that the
verification does not depend on the driver under test.  Any mismatch fails the
axis regardless of the timings.

Usage::

    POSTGRES_ODBC_DRIVER=/path/psqlodbcw.so \\
    ADBC_ODBC_DRIVER=/path/libadbc_driver_odbc.so \\
        python bench/native_threshold.py --database postgres \\
        --rows 1000000 --runs 3 --partitions 8

    SQLITE_ODBC_DRIVER=/path/libsqlite3odbc.so \\
    ADBC_ODBC_DRIVER=/path/libadbc_driver_odbc.so \\
        python bench/native_threshold.py --database sqlite --rows 200000

Exit status is 0 when both axes pass, 1 otherwise.
"""

import argparse
import concurrent.futures
import json
import os
import pathlib
import statistics
import subprocess
import sys
import tempfile
import time
import urllib.parse

import pyarrow
import pyarrow.compute as pc

HERE = pathlib.Path(__file__).resolve().parent
ROOT = HERE.parent

ODBC_DRIVER = os.environ.get(
    "ADBC_ODBC_DRIVER", str(ROOT / "build" / "libadbc_driver_odbc.so")
)
PSQLODBC = os.environ.get("POSTGRES_ODBC_DRIVER", "PostgreSQL Unicode")
SQLITEODBC = os.environ.get("SQLITE_ODBC_DRIVER", "SQLite3")

DEFAULT_PG = os.environ.get("PG_URI", "postgresql://adbc:adbc@127.0.0.1:15482/adbc")

COLUMNS = ("id", "val", "txt", "dt")
KINDS = ("bridge_fetch", "native_fetch", "bridge_ingest", "native_ingest")
AXES = (("fetch", "bridge_fetch", "native_fetch"),
        ("ingest", "bridge_ingest", "native_ingest"))
TARGETS = {"bridge_ingest": "tgt_thr_bridge", "native_ingest": "tgt_thr_native"}

INGEST_CONNECTIONS = "adbc.odbc.ingest_connections"


# --------------------------------------------------------------------------
# connecting


def odbc_connstr(args):
    """ODBC connection string for the database under test."""
    if args.database == "postgres":
        u = urllib.parse.urlparse(args.pg)
        return ("DRIVER=%s;SERVER=%s;PORT=%d;DATABASE=%s;UID=%s;PWD=%s;"
                % (PSQLODBC, u.hostname or "127.0.0.1", u.port or 5432,
                   (u.path or "/postgres").lstrip("/"),
                   u.username or "", u.password or ""))
    return "Driver=%s;Database=%s;" % (SQLITEODBC, args.sqlite_path)


def native_connect(args):
    """A DBAPI connection through the *native* ADBC driver for this database."""
    if args.database == "postgres":
        import adbc_driver_postgresql.dbapi as pgdbapi

        return pgdbapi.connect(args.pg)
    import adbc_driver_sqlite.dbapi as sqdbapi

    return sqdbapi.connect(args.sqlite_path)


def bridge_open(connstr):
    """An adbcbridge database + connection pair."""
    import adbc_driver_manager as adm

    opts = {"adbc.odbc.connection_string": connstr, "adbc.odbc.delegate": "never"}
    db = adm.AdbcDatabase(driver=ODBC_DRIVER, entrypoint="AdbcDriverInit", **opts)
    return db, adm.AdbcConnection(db)


# --------------------------------------------------------------------------
# checksums


def _byte_sum(col):
    """Sum of every character byte in a string/binary column, or None.

    Order independent -- each row contributes exactly its own bytes -- and
    allocation free: the values buffer is reduced in place with numpy.
    """
    import numpy

    total = 0
    for chunk in col.chunks:
        if len(chunk) == 0:
            continue
        bufs = chunk.buffers()
        if len(bufs) < 3 or bufs[1] is None or bufs[2] is None:
            return None  # unexpected layout (string_view, all null)
        wide = (pyarrow.types.is_large_string(chunk.type)
                or pyarrow.types.is_large_binary(chunk.type))
        dtype, width = (numpy.int64, 8) if wide else (numpy.int32, 4)
        offs = numpy.frombuffer(bufs[1], dtype=dtype,
                                offset=chunk.offset * width, count=len(chunk) + 1)
        data = numpy.frombuffer(bufs[2], dtype=numpy.uint8)
        total += int(numpy.add.reduce(data[int(offs[0]):int(offs[-1])],
                                      dtype=numpy.int64))
    return total


def table_checksum(table):
    """Order-independent fingerprint of a fetched Arrow table.

    Order independent because a partitioned read hands the rows back in a
    different order than a single-connection read, and width tolerant because the
    ODBC path may report a narrower integer than the native driver for the same
    column (two columns of the same values sum alike whatever the width).
    """
    out = {"rows": table.num_rows}
    for name in table.column_names:
        col = table.column(name)
        typ = col.type
        if pyarrow.types.is_integer(typ):
            out[name] = pc.sum(col).as_py()
        elif pyarrow.types.is_floating(typ) or pyarrow.types.is_decimal(typ):
            # Floating-point addition is not associative, so a partitioned read
            # would not reproduce a single-connection sum bit for bit.  Scale to
            # an exact integer first; that sum is order independent.
            scaled = pc.cast(
                pc.round(pc.multiply(pc.cast(col, pyarrow.float64()), 1000.0)),
                pyarrow.int64(), safe=False)
            out[name] = pc.sum(scaled).as_py()
        elif pyarrow.types.is_date(typ) or pyarrow.types.is_timestamp(typ):
            width = pyarrow.int32() if typ == pyarrow.date32() else pyarrow.int64()
            out[name] = pc.sum(
                pc.cast(pc.cast(col, width, safe=False), pyarrow.int64())).as_py()
        elif pyarrow.types.is_boolean(typ):
            out[name] = pc.sum(pc.cast(col, pyarrow.int64())).as_py()
        else:
            extent = pc.min_max(col).as_py()
            out[name] = [pc.sum(pc.binary_length(col)).as_py(), _byte_sum(col),
                         str(extent.get("min")), str(extent.get("max"))]
    return json.dumps(out, sort_keys=True)


def sql_checksum(conn, args, table):
    """Server-side, order-independent checksum of a whole table.

    Computed in SQL so that it is independent of the driver under test, and out
    of exact (integer / numeric) aggregates so that it does not depend on the
    physical row order either.
    """
    if args.database == "postgres":
        query = (
            "SELECT COUNT(*), SUM(id), SUM(hashtext(txt)::bigint), "
            "SUM(EXTRACT(EPOCH FROM dt)::bigint), SUM(ROUND(val::numeric, 6)) "
            "FROM %s" % table
        )
    else:
        query = (
            "SELECT COUNT(*), SUM(id), SUM(LENGTH(txt)), "
            "SUM(CAST(julianday(dt) - 2440587.5 AS INTEGER)), "
            "SUM(CAST(ROUND(val * 1000) AS INTEGER)) "
            "FROM %s" % table
        )
    with conn.cursor() as cur:
        cur.execute(query)
        row = cur.fetchone()
    return json.dumps([str(v) for v in row])


# --------------------------------------------------------------------------
# the four timed kinds -- one whole worker process each


def _stream_to_table(handle):
    return pyarrow.RecordBatchReader._import_from_c(handle.address).read_all()


def _read_partition(connstr, descriptor):
    """One partition, on its own database + connection, in its own thread."""
    db, conn = bridge_open(connstr)
    try:
        return _stream_to_table(conn.read_partition(descriptor))
    finally:
        conn.close()
        db.close()


def bridge_fetch(args, query):
    import adbc_driver_manager as adm

    connstr = odbc_connstr(args)
    t0 = time.perf_counter()
    db, conn = bridge_open(connstr)
    try:
        stmt = adm.AdbcStatement(conn)
        stmt.set_options(**{"adbc.odbc.partitions": str(args.partitions)})
        stmt.set_sql_query(query)
        descriptors, _schema, _rows = stmt.execute_partitions()
        descriptors = [bytes(d) for d in descriptors]
        stmt.close()
        if len(descriptors) == 1:
            # sqlite (and any single-partition plan) comes back whole; read it
            # on the connection we already have.
            handle = conn.read_partition(descriptors[0])
            tables = [_stream_to_table(handle)]
        else:
            with concurrent.futures.ThreadPoolExecutor(
                    max_workers=len(descriptors)) as pool:
                futures = [pool.submit(_read_partition, connstr, d)
                           for d in descriptors]
                tables = [f.result() for f in futures]
        table = pyarrow.concat_tables(tables)
    finally:
        conn.close()
        db.close()
    return time.perf_counter() - t0, table, {"descriptors": len(descriptors)}


def native_fetch(args, query):
    t0 = time.perf_counter()
    conn = native_connect(args)
    try:
        with conn.cursor() as cur:
            cur.execute(query)
            table = cur.fetch_arrow_table()
    finally:
        conn.close()
    return time.perf_counter() - t0, table, {}


def _unknown_option(exc):
    text = str(exc)
    return "Unknown statement option" in text or "NOT_IMPLEMENTED" in text


def bridge_ingest(args, source, target):
    import adbc_driver_manager as adm

    connstr = odbc_connstr(args)
    t0 = time.perf_counter()
    db, conn = bridge_open(connstr)
    # Fanning the ingest out over several connections needs the CREATE TABLE to be
    # committed, or the worker connections cannot see the table they are inserting into.
    # So N > 1 is measured in autocommit (the driver still wraps each worker's share in a
    # transaction of its own); N = 1 is measured in the caller-owned transaction that is
    # the atomic, default way to ingest.  Either way the commit is inside the clock.
    explicit_txn = args.ingest_connections == 1
    try:
        if explicit_txn:
            conn.set_autocommit(False)
        stmt = adm.AdbcStatement(conn)
        accepted = False
        try:
            stmt.set_options(**{INGEST_CONNECTIONS: str(args.ingest_connections)})
            accepted = True
        except Exception as exc:
            # The option is new; a driver build that predates it must still be
            # measurable at the default of one connection.
            if args.ingest_connections != 1 or not _unknown_option(exc):
                raise SystemExit("failed to set %s=%d: %s"
                                 % (INGEST_CONNECTIONS, args.ingest_connections, exc))
        stmt.set_options(**{
            "adbc.ingest.target_table": target,
            "adbc.ingest.mode": "adbc.ingest.mode.create",
        })
        stmt.bind_stream(source.__arrow_c_stream__())
        affected = stmt.execute_update()
        stmt.close()
        if explicit_txn:
            conn.commit()
    finally:
        conn.close()
        db.close()
    return (time.perf_counter() - t0,
            {"affected": affected, "ingest_connections_option": accepted})


def native_ingest(args, source, target):
    t0 = time.perf_counter()
    conn = native_connect(args)
    try:
        with conn.cursor() as cur:
            affected = cur.adbc_ingest(target, source, mode="create")
        conn.commit()
    finally:
        conn.close()
    return time.perf_counter() - t0, {"affected": affected}


# --------------------------------------------------------------------------
# worker: exactly one timed run, one line of JSON on stdout


def run_worker(args):
    query = "SELECT %s FROM %s" % (", ".join(COLUMNS), args.srctable)
    kind = args.worker

    if kind.endswith("_fetch"):
        runner = bridge_fetch if kind == "bridge_fetch" else native_fetch
        elapsed, table, detail = runner(args, query)
        result = {"elapsed": elapsed, "rows": table.num_rows,
                  "checksum": table_checksum(table), "detail": detail}
    else:
        target = TARGETS[kind]
        # Reading the source, dropping the stale target and verifying afterwards
        # all happen outside the clock.
        conn = native_connect(args)
        try:
            with conn.cursor() as cur:
                cur.execute(query)
                source = cur.fetch_arrow_table()
            with conn.cursor() as cur:
                cur.execute("DROP TABLE IF EXISTS %s" % target)
            conn.commit()
        finally:
            conn.close()

        runner = bridge_ingest if kind == "bridge_ingest" else native_ingest
        elapsed, detail = runner(args, source, target)

        conn = native_connect(args)
        try:
            checksum = sql_checksum(conn, args, target)
            with conn.cursor() as cur:
                cur.execute("SELECT COUNT(*) FROM %s" % target)
                rows = cur.fetchone()[0]
        finally:
            conn.close()
        detail["source_rows"] = source.num_rows
        result = {"elapsed": elapsed, "rows": rows,
                  "checksum": checksum, "detail": detail}

    sys.stdout.write(json.dumps(result) + "\n")
    sys.stdout.flush()
    return 0


# --------------------------------------------------------------------------
# parent: source data, worker dispatch, report


def generate(rows):
    """The canonical source table: (id bigint, val double, txt text, dt date)."""
    ids = range(1, rows + 1)
    # The dates stay inside 2000..2013 on purpose: adbc_driver_sqlite rounds a
    # date32 through a 32-bit count of seconds, so anything past 2038-01-19 is
    # written back mangled and would make the source data itself untrustworthy.
    return pyarrow.table({
        "id": pyarrow.array(ids, pyarrow.int64()),
        "val": pyarrow.array([i * 1.5 for i in ids], pyarrow.float64()),
        "txt": pyarrow.array(["row-%012d" % i for i in ids], pyarrow.string()),
        "dt": pyarrow.array([10957 + (i % 5000) for i in ids], pyarrow.date32()),
    })


def table_rows(conn, name):
    """Row count, or None when the table is not there."""
    try:
        with conn.cursor() as cur:
            cur.execute("SELECT COUNT(*) FROM %s" % name)
            return cur.fetchone()[0]
    except Exception:
        # PostgreSQL aborts the whole transaction when the table is missing, so
        # the connection has to be cleared before it can be used to build it.
        try:
            conn.rollback()
        except Exception:
            pass
        return None


def prepare_source(args):
    """Pick or build the source table; returns its name.

    PostgreSQL prefers the standing src1m / src10m tables when the row count
    matches one of them; anything else is a src<rows> table this script creates
    once and then reuses.
    """
    preset = {1_000_000: "src1m", 10_000_000: "src10m"}.get(args.rows)
    name = preset if (preset and args.database == "postgres") else "src%d" % args.rows

    conn = native_connect(args)
    try:
        have = table_rows(conn, name)
        if have == args.rows:
            return name
        if have is not None:
            print("note: %s holds %r rows, rebuilding it for %d"
                  % (name, have, args.rows))
            with conn.cursor() as cur:
                cur.execute("DROP TABLE IF EXISTS %s" % name)
            conn.commit()
        print("preparing source table %s (%d rows) ..." % (name, args.rows))
        with conn.cursor() as cur:
            cur.adbc_ingest(name, generate(args.rows), mode="create")
        conn.commit()
    finally:
        conn.close()
    return name


def reference_checksums(args):
    """Reference fingerprints, computed by the parent, not by either side.

    The fetch reference is a plain single-connection read through the native
    driver; the ingest reference is the source table's own SQL checksum.
    """
    query = "SELECT %s FROM %s" % (", ".join(COLUMNS), args.srctable)
    conn = native_connect(args)
    try:
        with conn.cursor() as cur:
            cur.execute(query)
            table = cur.fetch_arrow_table()
        fetch_ref = table_checksum(table)
        ingest_ref = sql_checksum(conn, args, args.srctable)
    finally:
        conn.close()
    return fetch_ref, ingest_ref, table.num_rows


def last_json(text):
    """The last line of `text` that parses as a JSON object; ignores other noise."""
    for line in reversed(text.splitlines()):
        line = line.strip()
        if not line:
            continue
        try:
            value = json.loads(line)
        except ValueError:
            continue
        if isinstance(value, dict):
            return value
    return None


def run_one(args, kind):
    """One timed run, in a brand new process."""
    argv = [sys.executable, str(pathlib.Path(__file__).resolve()),
            "--worker", kind,
            "--database", args.database,
            "--rows", str(args.rows),
            "--srctable", args.srctable,
            "--partitions", str(args.partitions),
            "--ingest-connections", str(args.ingest_connections),
            "--pg", args.pg]
    if args.sqlite_path:
        argv += ["--sqlite-path", args.sqlite_path]
    proc = subprocess.run(argv, capture_output=True, text=True)
    if proc.returncode != 0:
        sys.stderr.write("\n=== worker %s FAILED (exit %d) ===\ncommand: %s\n"
                         % (kind, proc.returncode, " ".join(argv)))
        sys.stderr.write("--- worker stdout ---\n%s\n--- worker stderr ---\n%s\n"
                         % (proc.stdout, proc.stderr))
        raise SystemExit("worker %s failed" % kind)
    result = last_json(proc.stdout)
    if result is None or "elapsed" not in result:
        sys.stderr.write("--- worker stdout ---\n%s\n--- worker stderr ---\n%s\n"
                         % (proc.stdout, proc.stderr))
        raise SystemExit("worker %s printed no JSON result" % kind)
    if proc.stderr.strip():
        sys.stderr.write("worker %s wrote to stderr:\n%s\n" % (kind, proc.stderr))
    return result


def summarise(values):
    return {"mean": statistics.fmean(values), "median": statistics.median(values),
            "min": min(values), "max": max(values)}


def report(args, results, fetch_ref, ingest_ref, ref_rows):
    print()
    print("per-run wall clock, seconds (fresh process per run, interleaved):")
    for kind in KINDS:
        times = " ".join("%8.3f" % r["elapsed"] for r in results[kind])
        print("  %-14s %s   mean %7.3f"
              % (kind, times, statistics.fmean(r["elapsed"] for r in results[kind])))

    print()
    print("  bridge_fetch partitions   : %s returned for %d requested"
          % (results["bridge_fetch"][0]["detail"].get("descriptors"), args.partitions))
    accepted = results["bridge_ingest"][0]["detail"].get("ingest_connections_option")
    print("  %s : %s"
          % (INGEST_CONNECTIONS,
             "accepted, set to %d" % args.ingest_connections if accepted
             else "not supported by this driver build; driver default used"))

    verdicts = []
    print()
    header = ("%-7s %-7s %8s %8s %8s %8s %10s  %s"
              % ("axis", "side", "mean", "median", "min", "max", "speedup", "verdict"))
    print(header)
    print("-" * len(header))
    for axis, ours_kind, native_kind in AXES:
        ours = summarise([r["elapsed"] for r in results[ours_kind]])
        native = summarise([r["elapsed"] for r in results[native_kind]])
        speedup = native["mean"] / ours["mean"] if ours["mean"] else float("inf")

        expected = fetch_ref if axis == "fetch" else ingest_ref
        problems = []
        for kind in (ours_kind, native_kind):
            for i, r in enumerate(results[kind]):
                if r["checksum"] != expected:
                    problems.append("%s run %d checksum %s != reference %s"
                                    % (kind, i + 1, r["checksum"], expected))
                if r["rows"] != ref_rows:
                    problems.append("%s run %d saw %d rows, expected %d"
                                    % (kind, i + 1, r["rows"], ref_rows))
        ok = not problems and speedup >= args.threshold
        verdicts.append((axis, ok, speedup, problems))

        print("%-7s %-7s %8.3f %8.3f %8.3f %8.3f %10s  %s"
              % (axis, "ours", ours["mean"], ours["median"], ours["min"],
                 ours["max"], "%.3fx" % speedup, "PASS" if ok else "FAIL"))
        print("%-7s %-7s %8.3f %8.3f %8.3f %8.3f %10s  %s"
              % ("", "native", native["mean"], native["median"], native["min"],
                 native["max"], "", ""))

    print()
    print("(speedup is native mean / ours mean: ours is that many times faster)")
    for axis, ok, speedup, problems in verdicts:
        if ok:
            print("%s: PASS -- ours is %.3fx faster than native "
                  "(threshold %.2fx) and every checksum matches the reference"
                  % (axis, speedup, args.threshold))
            continue
        print("%s: FAIL" % axis)
        if speedup < args.threshold:
            print("    SPEED: ours is %.3fx faster than native, below the "
                  "%.2fx threshold" % (speedup, args.threshold))
        for p in problems:
            print("    CORRECTNESS: %s" % p)
    return all(ok for _, ok, _, _ in verdicts)


def main(argv=None):
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--database", choices=("postgres", "sqlite"), required=True)
    ap.add_argument("--rows", type=int, default=1_000_000)
    ap.add_argument("--runs", type=int, default=3)
    ap.add_argument("--threshold", type=float, default=1.2,
                    help="ours must be at least this many times faster than native")
    ap.add_argument("--partitions", type=int, default=8,
                    help="adbc.odbc.partitions for the adbcbridge fetch")
    ap.add_argument("--ingest-connections", type=int, default=1,
                    help="%s for the adbcbridge ingest" % INGEST_CONNECTIONS)
    ap.add_argument("--pg", default=DEFAULT_PG,
                    help="postgresql:// URL for the native driver and psqlodbc")
    ap.add_argument("--sqlite-path", default=None,
                    help="sqlite file holding the source table "
                         "(default: one under the system temp directory)")
    # Worker-only plumbing: one timed run, then one line of JSON on stdout.
    ap.add_argument("--worker", choices=KINDS, help=argparse.SUPPRESS)
    ap.add_argument("--srctable", help=argparse.SUPPRESS)
    args = ap.parse_args(argv)

    if args.database == "sqlite" and not args.sqlite_path:
        args.sqlite_path = os.path.join(
            tempfile.gettempdir(), "adbc_native_threshold_%d.sqlite" % args.rows)

    if args.worker:
        if not args.srctable:
            ap.error("--worker requires --srctable")
        return run_worker(args)

    print("=" * 78)
    print("adbcbridge vs the native ADBC driver -- %s, threshold %.2fx"
          % (args.database, args.threshold))
    print("=" * 78)
    print("rows requested     : %d" % args.rows)
    print("runs               : %d (interleaved, one fresh process per run)" % args.runs)
    print("partitions (fetch) : %d" % args.partitions)
    print("ingest connections : %d" % args.ingest_connections)
    print("adbcbridge driver  : %s" % ODBC_DRIVER)
    if args.database == "postgres":
        print("odbc driver        : %s" % PSQLODBC)
        print("native driver      : adbc_driver_postgresql")
        print("postgres           : %s" % args.pg)
    else:
        print("odbc driver        : %s" % SQLITEODBC)
        print("native driver      : adbc_driver_sqlite")
        print("sqlite file        : %s" % args.sqlite_path)
    print("python             : %s" % sys.executable)
    print("load average start : %.2f %.2f %.2f" % os.getloadavg())
    print()

    args.srctable = prepare_source(args)
    fetch_ref, ingest_ref, ref_rows = reference_checksums(args)
    print("source table       : %s (%d rows)" % (args.srctable, ref_rows))
    print("fetch reference    : %s" % fetch_ref)
    print("ingest reference   : %s" % ingest_ref)
    print()

    results = {kind: [] for kind in KINDS}
    for run in range(args.runs):
        for kind in KINDS:
            r = run_one(args, kind)
            results[kind].append(r)
            print("  run %d/%d  %-14s %8.3f s  %d rows"
                  % (run + 1, args.runs, kind, r["elapsed"], r["rows"]))

    ok = report(args, results, fetch_ref, ingest_ref, ref_rows)
    print()
    print("load average end   : %.2f %.2f %.2f" % os.getloadavg())
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
