#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Differential check for column-wise array parameter binding.

Every case is ingested twice -- once with ``adbc.odbc.array_binding=true`` and
once with the row-at-a-time fallback -- and the data read back must be
byte-identical between the two modes.  Also exercises the batch-level fallbacks
(values wider than 32 KiB), the option's own validation, and the transaction
batching that both modes share: a multi-row execute commits once at the end, a
transaction the caller opened stays theirs to commit, and a failure rolls back.

Usage::

    ADBC_ODBC_DRIVER=build/libadbc_driver_odbc.so \\
    SQLITE_ODBC_DRIVER=/path/to/libsqlite3odbc.so \\
        python bench/verify_array_binding.py
"""

import os
import pathlib
import tempfile
import decimal

import pyarrow as pa
import adbc_driver_manager.dbapi as dbapi

HERE = pathlib.Path(__file__).resolve().parent
DRIVER = os.environ.get(
    "ADBC_ODBC_DRIVER", str(HERE.parent / "build" / "libadbc_driver_odbc.so")
)
SQLITE = os.environ.get("SQLITE_ODBC_DRIVER", "SQLite3")
OPT = "adbc.odbc.array_binding"


def conn_for(db, **kwargs):
    return dbapi.connect(driver=DRIVER, db_kwargs={"uri": f"Driver={SQLITE};Database={db};"},
                         **kwargs)


def tmpdb(name):
    return os.path.join(tempfile.mkdtemp(), name)


def ingest_roundtrip(tbl, arr):
    tmp = tempfile.mkdtemp()
    db = os.path.join(tmp, "v.db")
    with conn_for(db) as conn:
        with conn.cursor() as cur:
            cur._stmt.set_options(**{OPT: "true" if arr else "false"})
            n = cur.adbc_ingest("t", tbl, mode="create")
        with conn.cursor() as cur:
            cur.execute("SELECT * FROM t")
            back = cur.fetch_arrow_table()
    return n, back.to_pydict()


def check(name, tbl):
    na, da = ingest_roundtrip(tbl, True)
    nr, dr = ingest_roundtrip(tbl, False)
    assert na == nr == tbl.num_rows, (name, na, nr, tbl.num_rows)
    assert da == dr, (name, {k: (da[k][:5], dr[k][:5]) for k in da if da[k] != dr[k]})
    print(f"ok  {name}: {tbl.num_rows} rows, cols={list(tbl.column_names)}")


N = 5000
check("mixed", pa.table({
    "i8": pa.array([(i % 200) - 100 for i in range(N)], pa.int8()),
    "i16": pa.array([i % 30000 for i in range(N)], pa.int16()),
    "i32": pa.array([(i - N // 2) * 3 for i in range(N)], pa.int32()),
    "i64": pa.array([(i - N // 2) * 1000003 for i in range(N)], pa.int64()),
    "u8": pa.array([i % 256 for i in range(N)], pa.uint8()),
    "u16": pa.array([i % 65535 for i in range(N)], pa.uint16()),
    "u32": pa.array([i * 7 for i in range(N)], pa.uint32()),
    "u64": pa.array([i * 11 for i in range(N)], pa.uint64()),
    "f32": pa.array([(i - N // 2) * 0.5 for i in range(N)], pa.float32()),
    "f64": pa.array([i * -1.25 for i in range(N)], pa.float64()),
    "bo": pa.array([i % 3 == 0 for i in range(N)], pa.bool_()),
    "s": pa.array(["" if i % 5 == 0 else f"v{i}" for i in range(N)], pa.string()),
    "b": pa.array([b"" if i % 7 == 0 else bytes([i % 256]) * (i % 9) for i in range(N)], pa.binary()),
    "d": pa.array([i % 30000 for i in range(N)], pa.date32()),
    "ts": pa.array([1_700_000_000_000_000 + i for i in range(N)], pa.timestamp("us")),
}))

check("nulls-everywhere", pa.table({
    "i32": pa.array([None if i % 3 else i for i in range(N)], pa.int32()),
    "i64": pa.array([None if i % 2 else i for i in range(N)], pa.int64()),
    "f64": pa.array([None if i % 4 else float(i) for i in range(N)], pa.float64()),
    "s": pa.array([None if i % 5 else f"s{i}" for i in range(N)], pa.string()),
    "b": pa.array([None if i % 6 else b"\x00\xff" for i in range(N)], pa.binary()),
    "bo": pa.array([None if i % 7 else i % 2 == 0 for i in range(N)], pa.bool_()),
    "d": pa.array([None if i % 8 else i for i in range(N)], pa.date32()),
    "ts": pa.array([None if i % 9 else i * 1000 for i in range(N)], pa.timestamp("ms")),
}))

check("all-null", pa.table({
    "i64": pa.array([None] * N, pa.int64()),
    "s": pa.array([None] * N, pa.string()),
}))

# Wide strings: > 32 KiB triggers the row-at-a-time fallback for that batch.
check("wide-strings", pa.table({
    "i": pa.array(range(200), pa.int64()),
    "s": pa.array(["x" * (40000 if i == 100 else 10) for i in range(200)], pa.string()),
}))

check("decimal", pa.table({
    "n": pa.array([decimal.Decimal(f"{i}.{i % 100:02d}") for i in range(1000)],
                  pa.decimal128(12, 2)),
}))

check("timestamp-units", pa.table({
    "s": pa.array([1_700_000_000 + i for i in range(1000)], pa.timestamp("s")),
    "ms": pa.array([1_700_000_000_000 + i for i in range(1000)], pa.timestamp("ms")),
    "ns": pa.array([1_700_000_000_000_000_000 + i * 1000 for i in range(1000)],
                   pa.timestamp("ns")),
}))

# A stream whose first batch falls back (a value wider than the staging cap) and
# whose later batches use parameter arrays again.  Some drivers keep per-execute
# state that only survives this ordering if the parameters are reset in between --
# MariaDB Connector/ODBC segfaulted on it.
mixed_batches = pa.Table.from_batches([
    pa.record_batch({"i": pa.array(range(200), pa.int64()),
                     "s": pa.array(["x" * 40000] + [f"a{i}" for i in range(199)])}),
    pa.record_batch({"i": pa.array(range(200, 400), pa.int64()),
                     "s": pa.array([f"b{i}" for i in range(200)])}),
    pa.record_batch({"i": pa.array(range(400, 600), pa.int64()),
                     "s": pa.array([f"c{i}" for i in range(200)])}),
])
check("fallback-then-array", mixed_batches)

# Sliced (offset != 0) arrays exercise the direct-bind offset arithmetic.
big = pa.table({
    "i64": pa.array(range(N), pa.int64()),
    "i32": pa.array(range(N), pa.int32()),
    "s": pa.array([f"r{i}" for i in range(N)], pa.string()),
})
check("sliced", big.slice(1234, 2500).combine_chunks())

# executemany through the same path
tmp = tempfile.mkdtemp()
db = os.path.join(tmp, "em.db")
with conn_for(db) as conn:
    with conn.cursor() as cur:
        cur.execute("CREATE TABLE em (a INTEGER, b TEXT)")
        cur.executemany("INSERT INTO em VALUES (?, ?)",
                        [(i, None if i % 10 == 0 else f"n{i}") for i in range(3000)])
        print("executemany rowcount:", cur.rowcount)
        assert cur.rowcount == 3000, cur.rowcount
        cur.execute("SELECT COUNT(*), COUNT(b) FROM em")
        got = cur.fetchone()
        assert got == (3000, 2700), got
        # Parameterized SELECT still returns a result set (single row bound).
        cur.execute("SELECT b FROM em WHERE a = ?", (7,))
        assert cur.fetchone()[0] == "n7"
print("ok  executemany + parameterized select")


def dml_rowcounts(arr):
    """Run a fixed UPDATE/DELETE script and collect every reported rowcount."""
    db = os.path.join(tempfile.mkdtemp(), "dml.db")
    counts = []
    with conn_for(db) as conn:
        with conn.cursor() as cur:
            cur._stmt.set_options(**{OPT: "true" if arr else "false"})
            cur.execute("CREATE TABLE t (a INTEGER, b TEXT)")
            cur.executemany("INSERT INTO t VALUES (?, ?)",
                            [(i, "x") for i in range(1000)])
            counts.append(("insert", cur.rowcount))
            # No parameter set matches anything: rows affected is 0, not 500.
            cur.executemany("UPDATE t SET b = 'y' WHERE a = ?",
                            [(10_000 + i,) for i in range(500)])
            counts.append(("update-no-match", cur.rowcount))
            cur.executemany("UPDATE t SET b = 'y' WHERE a = ?",
                            [(i,) for i in range(200)])
            counts.append(("update-match", cur.rowcount))
            # Half the sets match, half do not.
            cur.executemany("UPDATE t SET b = 'z' WHERE a = ?",
                            [(i if i % 2 else 20_000 + i,) for i in range(400)])
            counts.append(("update-half", cur.rowcount))
            # One parameter value matches several rows each.
            cur.executemany("UPDATE t SET b = 'w' WHERE a % 100 = ?",
                            [(i,) for i in range(4)])
            counts.append(("update-fanout", cur.rowcount))
            cur.executemany("DELETE FROM t WHERE a = ?",
                            [(30_000 + i,) for i in range(500)])
            counts.append(("delete-no-match", cur.rowcount))
            cur.executemany("DELETE FROM t WHERE a = ?", [(i,) for i in range(300)])
            counts.append(("delete-match", cur.rowcount))
            cur.execute("SELECT COUNT(*) FROM t")
            counts.append(("remaining", cur.fetchone()[0]))
    return counts


ca, cr = dml_rowcounts(True), dml_rowcounts(False)
assert ca == cr, (ca, cr)
by_name = dict(ca)
assert by_name["insert"] == 1000, ca
assert by_name["update-no-match"] == 0, ca
assert by_name["update-match"] == 200, ca
assert by_name["delete-no-match"] == 0, ca
assert by_name["delete-match"] == 300, ca
assert by_name["remaining"] == 700, ca
print("ok  executemany DML rowcount parity:", ca)

# --- transaction batching --------------------------------------------------
# Multi-row executes turn autocommit off for the duration and commit once at the
# end.  A transaction the caller opened themselves must be left alone: their
# rollback still has to undo everything.
db = tmpdb("txn.db")
with conn_for(db, autocommit=True) as conn:
    with conn.cursor() as cur:
        cur.execute("CREATE TABLE t (a INTEGER)")
conn = conn_for(db, autocommit=False)
with conn.cursor() as cur:
    cur.executemany("INSERT INTO t VALUES (?)", [(i,) for i in range(500)])
    cur.execute("SELECT COUNT(*) FROM t")
    assert cur.fetchone()[0] == 500, "rows should be visible inside the transaction"
conn.rollback()
with conn.cursor() as cur:
    cur.execute("SELECT COUNT(*) FROM t")
    got = cur.fetchone()[0]
assert got == 0, f"the caller's rollback must undo the whole executemany, got {got}"
conn.close()
print("ok  caller's transaction respected (rollback undid executemany)")

# In autocommit the rows are committed by the time the execute returns, without
# the caller doing anything.
db = tmpdb("auto.db")
conn = conn_for(db, autocommit=True)
with conn.cursor() as cur:
    cur.execute("CREATE TABLE t (a INTEGER)")
    cur.executemany("INSERT INTO t VALUES (?)", [(i,) for i in range(500)])
conn.close()
with conn_for(db, autocommit=True) as conn:  # fresh connection: only committed rows
    with conn.cursor() as cur:
        cur.execute("SELECT COUNT(*) FROM t")
        got = cur.fetchone()[0]
assert got == 500, f"autocommit execute must commit, got {got}"
print("ok  autocommit executemany committed once at the end")

# A failure part-way through rolls the whole ingest back rather than leaving a
# half-filled table behind.
db = tmpdb("fail.db")
conn = conn_for(db, autocommit=True)
with conn.cursor() as cur:
    cur.execute("CREATE TABLE t (a INTEGER PRIMARY KEY)")
    try:
        cur.executemany("INSERT INTO t VALUES (?)", [(i % 400,) for i in range(800)])
        raise AssertionError("expected a duplicate-key error")
    except dbapi.Error as e:
        print("ok  duplicate key rejected:", type(e).__name__)
conn.close()
with conn_for(db, autocommit=True) as conn:
    with conn.cursor() as cur:
        cur.execute("SELECT COUNT(*) FROM t")
        got = cur.fetchone()[0]
assert got == 0, f"a failed executemany must roll back, got {got} rows"
print("ok  failed executemany rolled back")

# Explicitly disabling array binding must still work end to end.
tmp = tempfile.mkdtemp()
db = os.path.join(tmp, "off.db")
with conn_for(db) as conn:
    with conn.cursor() as cur:
        cur._stmt.set_options(**{OPT: "false"})
        cur.execute("CREATE TABLE off (a INTEGER)")
        cur.executemany("INSERT INTO off VALUES (?)", [(i,) for i in range(500)])
        cur.execute("SELECT COUNT(*) FROM off")
        assert cur.fetchone()[0] == 500
    with conn.cursor() as cur:
        try:
            cur._stmt.set_options(**{OPT: "maybe"})
            raise AssertionError("expected rejection")
        except Exception as e:
            print("ok  bad option value rejected:", type(e).__name__)
print("VERIFY OK")
