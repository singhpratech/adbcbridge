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
sides rather than on whichever ran first.  Both drivers' Python modules are
imported *before* any clock starts (see ``warm_imports``), because importing them
does not cost the same on the two sides and is not part of reading a table.

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

    # A PostgreSQL-wire server with no `ctid`: the source table gets a primary key
    # (which is what the key-range split needs), and only the fetch axis is measured
    # because the native driver cannot write to these servers either.
    POSTGRES_ODBC_DRIVER=/path/psqlodbcw.so \\
    ADBC_ODBC_DRIVER=/path/libadbc_driver_odbc.so \\
        python bench/native_threshold.py --database cockroachdb \\
        --rows 1000000 --runs 3 --partitions 8 --axes fetch

Exit status is 0 when every measured axis passes, 1 otherwise.
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

# The PostgreSQL-wire servers this script can drive.  All three are read by psqlodbc on
# the adbcbridge side and by adbc_driver_postgresql on the native side, so they are the
# same measurement with a different URL -- except that CockroachDB and YugabyteDB have
# no `ctid`, which is the whole reason for measuring them.
#
# `primary_key` says whether the source table is built with one.  It is the default for
# the two heapless servers because without an index there is nothing for a key-range
# split to use, and a real table on either of them has a primary key; PostgreSQL's
# source table deliberately stays without one, so that its ctid numbers keep meaning
# what they meant before.
PG_WIRE = {
    "postgres": {"url": DEFAULT_PG, "primary_key": False},
    "cockroachdb": {"url": "postgresql://root@127.0.0.1:16257/defaultdb?sslmode=disable",
                    "primary_key": True},
    "yugabyte": {"url": "postgresql://yugabyte:yugabyte@127.0.0.1:15433/yugabyte",
                 "primary_key": True},
}
DATABASES = tuple(PG_WIRE) + ("sqlite",)

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
    if args.database in PG_WIRE:
        u = urllib.parse.urlparse(args.pg)
        return ("DRIVER=%s;SERVER=%s;PORT=%d;DATABASE=%s;UID=%s;PWD=%s;"
                % (PSQLODBC, u.hostname or "127.0.0.1", u.port or 5432,
                   (u.path or "/postgres").lstrip("/"),
                   u.username or "", u.password or ""))
    return "Driver=%s;Database=%s;" % (SQLITEODBC, args.sqlite_path)


def native_connect(args):
    """A DBAPI connection through the *native* ADBC driver for this database."""
    if args.database in PG_WIRE:
        import adbc_driver_postgresql.dbapi as pgdbapi

        return pgdbapi.connect(args.pg)
    import adbc_driver_sqlite.dbapi as sqdbapi

    return sqdbapi.connect(args.sqlite_path)


def bridge_dbapi(args):
    """A DBAPI connection through *adbcbridge*, for the housekeeping around a run.

    Needed because the native driver cannot always do the housekeeping: CockroachDB
    does not implement binary COPY, so adbc_driver_postgresql cannot read a single row
    from it and so cannot build or checksum the source table either.
    """
    import adbc_driver_manager.dbapi as mgr

    return mgr.connect(driver=ODBC_DRIVER, entrypoint="AdbcDriverInit",
                       db_kwargs={"adbc.odbc.connection_string": odbc_connstr(args),
                                  "adbc.odbc.delegate": "never"})


def native_can_read(args):
    """Can the native ADBC driver read one row from this server at all?"""
    if args.database not in PG_WIRE:
        return True
    try:
        conn = native_connect(args)
    except Exception:
        return False
    try:
        with conn.cursor() as cur:
            cur.execute("SELECT 1")
            cur.fetch_arrow_table()
        return True
    except Exception:
        return False
    finally:
        conn.close()


def admin_connect(args):
    """A connection for everything that is *not* a timed run.

    The native driver wherever it works, so that the reference the timings are checked
    against does not come from the driver under test; adbcbridge only where the native
    driver cannot read the server at all.
    """
    if args.native_ok is None:
        args.native_ok = native_can_read(args)
    if args.native_ok:
        return native_connect(args)
    return bridge_dbapi(args)


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
    if args.database in PG_WIRE:
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


def warm_imports(args):
    """Import both drivers before any clock starts.

    A fresh process per timed run is the point of this script, but it turns Python's
    module import into a per-run cost -- and an asymmetric one, because the two sides
    do not cost the same to import: `adbc_driver_manager` is about 2 ms, while
    `adbc_driver_postgresql.dbapi` is 130-150 ms of shared-library loading.  Left where
    each side happened to put it, that landed *inside* the native side's clock and
    outside ours, which is most of a sub-second measurement handed to one side.  So
    both are imported here, before anything is timed, and what the clock covers is the
    connection and the query.
    """
    import adbc_driver_manager  # noqa: F401
    import adbc_driver_manager.dbapi  # noqa: F401

    try:
        if args.database in PG_WIRE:
            import adbc_driver_postgresql.dbapi  # noqa: F401
        else:
            import adbc_driver_sqlite.dbapi  # noqa: F401
    except ImportError:
        pass


def run_worker(args):
    warm_imports(args)
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
        conn = admin_connect(args)
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

        conn = admin_connect(args)
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


def generate(rows, lo=None, hi=None):
    """The canonical source table: (id bigint, val double, txt text, dt date).

    `lo`/`hi` cut out one half-open slice of the ids, for building the table in
    chunks; the values for a given id do not depend on which chunk it arrives in.
    """
    ids = range(1, rows + 1) if lo is None else range(lo, hi)
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
    once and then reuses.  A table asked for with a primary key gets its own name,
    so that the two shapes can stand side by side on the same server and be
    compared -- a ctid split against a key-range split over the same rows.
    """
    preset = {1_000_000: "src1m", 10_000_000: "src10m"}.get(args.rows)
    name = preset if (preset and args.database == "postgres") else "src%d" % args.rows
    if args.primary_key:
        name += "pk"

    conn = admin_connect(args)
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
        print("preparing source table %s (%d rows%s) ..."
              % (name, args.rows, ", primary key on id" if args.primary_key else ""))
        if args.primary_key:
            # adbc_ingest creates a bare table, so the shape is declared first and the
            # rows appended into it.  In chunks, because a heapless server holds an
            # append in memory until it commits.
            with conn.cursor() as cur:
                cur.execute("CREATE TABLE %s (id bigint, val double precision, "
                            "txt varchar(64), dt date, PRIMARY KEY (id))" % name)
            conn.commit()
            for lo in range(1, args.rows + 1, 100_000):
                hi = min(lo + 100_000, args.rows + 1)
                with conn.cursor() as cur:
                    cur.adbc_ingest(name, generate(args.rows, lo, hi), mode="append")
                conn.commit()
        else:
            with conn.cursor() as cur:
                cur.adbc_ingest(name, generate(args.rows), mode="create")
            conn.commit()
        # Give the planner a row estimate; the automatic partition count asks for one.
        if args.database in PG_WIRE:
            try:
                with conn.cursor() as cur:
                    cur.execute("ANALYZE %s" % name)
                conn.commit()
            except Exception:
                pass
    finally:
        conn.close()
    return name


def reference_checksums(args):
    """Reference fingerprints, computed by the parent, not by either side.

    The fetch reference is a plain single-connection read through the native
    driver; the ingest reference is the source table's own SQL checksum.  Where the
    native driver cannot read the server at all it is a single-partition adbcbridge
    read instead, which is a weaker reference and is called out in the report.
    """
    query = "SELECT %s FROM %s" % (", ".join(COLUMNS), args.srctable)
    conn = admin_connect(args)
    try:
        with conn.cursor() as cur:
            cur.execute(query)
            table = cur.fetch_arrow_table()
        fetch_ref = table_checksum(table)
        ingest_ref = None
        if "ingest" in args.axes:
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
            "--axes", "both" if len(args.axes) == 2 else args.axes[0],
            "--pg", args.pg]
    if args.primary_key:
        argv += ["--primary-key"]
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
    axes = [a for a in AXES if a[0] in args.axes]
    print()
    print("per-run wall clock, seconds (fresh process per run, interleaved):")
    for kind in KINDS:
        if kind not in results:
            continue
        times = " ".join("%8.3f" % r["elapsed"] for r in results[kind])
        print("  %-14s %s   mean %7.3f"
              % (kind, times, statistics.fmean(r["elapsed"] for r in results[kind])))

    print()
    if "bridge_fetch" in results:
        print("  bridge_fetch partitions   : %s returned for %d requested"
              % (results["bridge_fetch"][0]["detail"].get("descriptors"), args.partitions))
    if "bridge_ingest" in results:
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
    for axis, ours_kind, native_kind in axes:
        ours = summarise([r["elapsed"] for r in results[ours_kind]])
        expected = fetch_ref if axis == "fetch" else ingest_ref
        problems = []
        if native_kind not in results:
            # No native ADBC driver can read this server, so there is no ratio to
            # report.  Correctness is still checked, and still decides the verdict.
            for i, r in enumerate(results[ours_kind]):
                if r["checksum"] != expected:
                    problems.append("%s run %d checksum %s != reference %s"
                                    % (ours_kind, i + 1, r["checksum"], expected))
                if r["rows"] != ref_rows:
                    problems.append("%s run %d saw %d rows, expected %d"
                                    % (ours_kind, i + 1, r["rows"], ref_rows))
            verdicts.append((axis, not problems, None, problems))
            print("%-7s %-7s %8.3f %8.3f %8.3f %8.3f %10s  %s"
                  % (axis, "ours", ours["mean"], ours["median"], ours["min"],
                     ours["max"], "n/a", "PASS" if not problems else "FAIL"))
            print("%-7s %-7s %8s %8s %8s %8s %10s  %s"
                  % ("", "native", "-", "-", "-", "-", "",
                     "native ADBC driver cannot read this server"))
            continue
        native = summarise([r["elapsed"] for r in results[native_kind]])
        speedup = native["mean"] / ours["mean"] if ours["mean"] else float("inf")

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
        if ok and speedup is None:
            print("%s: correct, but not comparable -- no native ADBC driver can read "
                  "this server, so there is no ratio to hold to a threshold" % axis)
            continue
        if ok:
            print("%s: PASS -- ours is %.3fx faster than native "
                  "(threshold %.2fx) and every checksum matches the reference"
                  % (axis, speedup, args.threshold))
            continue
        print("%s: FAIL" % axis)
        if speedup is not None and speedup < args.threshold:
            print("    SPEED: ours is %.3fx faster than native, below the "
                  "%.2fx threshold" % (speedup, args.threshold))
        for p in problems:
            print("    CORRECTNESS: %s" % p)
    return all(ok for _, ok, _, _ in verdicts)


def main(argv=None):
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--database", choices=DATABASES, required=True)
    ap.add_argument("--rows", type=int, default=1_000_000)
    ap.add_argument("--runs", type=int, default=3)
    ap.add_argument("--axes", choices=("both", "fetch", "ingest"), default="both",
                    help="which axes to measure; `fetch` is the only one available "
                         "against a server the native driver cannot write to")
    ap.add_argument("--primary-key", action="store_true", default=None,
                    help="build the source table with PRIMARY KEY (id), which is what "
                         "the key-range split needs (default: on for the servers with "
                         "no ctid, off for PostgreSQL)")
    ap.add_argument("--threshold", type=float, default=1.2,
                    help="ours must be at least this many times faster than native")
    ap.add_argument("--partitions", type=int, default=8,
                    help="adbc.odbc.partitions for the adbcbridge fetch")
    ap.add_argument("--ingest-connections", type=int, default=1,
                    help="%s for the adbcbridge ingest" % INGEST_CONNECTIONS)
    ap.add_argument("--pg", default=None,
                    help="postgresql:// URL for the native driver and psqlodbc "
                         "(default: the one this script knows for --database)")
    ap.add_argument("--sqlite-path", default=None,
                    help="sqlite file holding the source table "
                         "(default: one under the system temp directory)")
    ap.add_argument("--srctable",
                    help="measure this existing table instead of building one; for "
                         "table shapes this script does not know how to create, such "
                         "as a declaratively partitioned parent")
    # Worker-only plumbing: one timed run, then one line of JSON on stdout.
    ap.add_argument("--worker", choices=KINDS, help=argparse.SUPPRESS)
    args = ap.parse_args(argv)

    if args.database == "sqlite" and not args.sqlite_path:
        args.sqlite_path = os.path.join(
            tempfile.gettempdir(), "adbc_native_threshold_%d.sqlite" % args.rows)
    if args.pg is None:
        args.pg = PG_WIRE.get(args.database, {}).get("url", DEFAULT_PG)
    if args.primary_key is None:
        args.primary_key = PG_WIRE.get(args.database, {}).get("primary_key", False)
    args.axes = ("fetch", "ingest") if args.axes == "both" else (args.axes,)
    args.native_ok = None  # resolved on first use; see admin_connect
    kinds = tuple(k for k in KINDS if k.split("_")[1] in args.axes)

    if args.worker:
        if not args.srctable:
            ap.error("--worker requires --srctable")
        return run_worker(args)

    args.native_ok = native_can_read(args)
    if not args.native_ok:
        # Nothing to time on the native side, and nothing to compare against.
        kinds = tuple(k for k in kinds if not k.startswith("native_"))

    print("=" * 78)
    print("adbcbridge vs the native ADBC driver -- %s, threshold %.2fx"
          % (args.database, args.threshold))
    print("=" * 78)
    print("rows requested     : %d" % args.rows)
    print("runs               : %d (interleaved, one fresh process per run)" % args.runs)
    print("axes               : %s" % ", ".join(args.axes))
    print("partitions (fetch) : %d" % args.partitions)
    print("ingest connections : %d" % args.ingest_connections)
    print("source primary key : %s" % ("PRIMARY KEY (id)" if args.primary_key else "none"))
    print("adbcbridge driver  : %s" % ODBC_DRIVER)
    if args.database in PG_WIRE:
        print("odbc driver        : %s" % PSQLODBC)
        print("native driver      : adbc_driver_postgresql")
        print("server             : %s" % args.pg)
    else:
        print("odbc driver        : %s" % SQLITEODBC)
        print("native driver      : adbc_driver_sqlite")
        print("sqlite file        : %s" % args.sqlite_path)
    print("python             : %s" % sys.executable)
    print("load average start : %.2f %.2f %.2f" % os.getloadavg())
    if not args.native_ok:
        print()
        print("NOTE: the native ADBC driver cannot read this server at all, so there is")
        print("      no native side to compare against and the reference checksum is a")
        print("      single-partition adbcbridge read rather than an independent one.")
    print()

    if not args.srctable:
        args.srctable = prepare_source(args)
    fetch_ref, ingest_ref, ref_rows = reference_checksums(args)
    print("source table       : %s (%d rows)" % (args.srctable, ref_rows))
    print("fetch reference    : %s" % fetch_ref)
    if ingest_ref is not None:
        print("ingest reference   : %s" % ingest_ref)
    print()

    results = {kind: [] for kind in kinds}
    for run in range(args.runs):
        for kind in kinds:
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
