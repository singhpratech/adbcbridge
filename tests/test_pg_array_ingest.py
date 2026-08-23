#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Licensed to the Apache Software Foundation (ASF) under one
# or more contributor license agreements.  See the NOTICE file
# distributed with this work for additional information
# regarding copyright ownership.  The ASF licenses this file
# to you under the Apache License, Version 2.0 (the
# "License"); you may not use this file except in compliance
# with the License.  You may obtain a copy of the License at
#
#   http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing,
# software distributed under the License is distributed on an
# "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
# KIND, either express or implied.  See the License for the
# specific language governing permissions and limitations
# under the License.
"""PostgreSQL array ingest.

Against PostgreSQL the driver sends a bulk ingest as one array parameter per
column and lets the server expand it --

    INSERT INTO t ("a", "b") SELECT * FROM unnest(?::bigint[], ?::text[])

-- instead of binding K*ncols separate cells.  The values travel as PostgreSQL
array literals, which is exactly where a quoting bug would corrupt data, so the
tests here are adversarial by construction: every value that could be read back
as something else (the word NULL, an empty element, a brace, a comma, a quote, a
backslash, a newline, non-ASCII) is in the fixtures, and the whole thing is
checked *differentially* against the row-at-a-time path on randomised data.

Needs a PostgreSQL server (PG_URI) and psqlodbc (POSTGRES_ODBC_DRIVER); skips
otherwise.  Run under pytest, or directly:

    PG_URI=postgresql://... POSTGRES_ODBC_DRIVER=/path/psqlodbcw.so \\
    ADBC_ODBC_DRIVER=build/libadbc_driver_odbc.so pytest tests/test_pg_array_ingest.py
"""

import datetime
import decimal
import os
import pathlib
import random
import urllib.parse

import pytest

import adbc_driver_manager as adm
import pyarrow

HERE = pathlib.Path(__file__).resolve().parent
DRIVER = os.environ.get("ADBC_ODBC_DRIVER", str(HERE.parent / "build" / "libadbc_driver_odbc.so"))
PSQLODBC = os.environ.get("POSTGRES_ODBC_DRIVER")
PG_CONN = os.environ.get("PG_URI")

pytestmark = pytest.mark.skipif(
    not (PSQLODBC and PG_CONN and os.path.exists(DRIVER)),
    reason="needs PG_URI, POSTGRES_ODBC_DRIVER and a built adbcbridge",
)

ROWS_PER_INSERT = "adbc.odbc.rows_per_insert"
INGEST_CONNECTIONS = "adbc.odbc.ingest_connections"


def connstr():
    u = urllib.parse.urlparse(PG_CONN)
    return "DRIVER=%s;SERVER=%s;PORT=%d;DATABASE=%s;UID=%s;PWD=%s;" % (
        PSQLODBC, u.hostname or "127.0.0.1", u.port or 5432,
        (u.path or "/postgres").lstrip("/"), u.username or "", u.password or "",
    )


def connect():
    db = adm.AdbcDatabase(
        driver=DRIVER, entrypoint="AdbcDriverInit",
        **{"adbc.odbc.connection_string": connstr(), "adbc.odbc.delegate": "never"},
    )
    return db, adm.AdbcConnection(db)


def sql(*statements):
    """Run statements that return nothing."""
    db, conn = connect()
    try:
        for statement in statements:
            stmt = adm.AdbcStatement(conn)
            stmt.set_sql_query(statement)
            stmt.execute_update()
            stmt.close()
    finally:
        conn.close()
        db.close()


def query(text):
    """Run a query and return its rows as a list of tuples."""
    db, conn = connect()
    try:
        stmt = adm.AdbcStatement(conn)
        stmt.set_sql_query(text)
        handle, _ = stmt.execute_query()
        table = pyarrow.RecordBatchReader._import_from_c(handle.address).read_all()
        stmt.close()
    finally:
        conn.close()
        db.close()
    return [tuple(row) for row in zip(*[c.to_pylist() for c in table.columns])]


def ingest(table, target, *, mode="create", rows_per_insert=None, connections=None):
    """Ingest `table`, returning the reported affected-row count."""
    db, conn = connect()
    try:
        if connections is not None:
            conn.set_autocommit(True)
        stmt = adm.AdbcStatement(conn)
        opts = {"adbc.ingest.target_table": target, "adbc.ingest.mode": "adbc.ingest.mode." + mode}
        if rows_per_insert is not None:
            opts[ROWS_PER_INSERT] = str(rows_per_insert)
        if connections is not None:
            opts[INGEST_CONNECTIONS] = str(connections)
        stmt.set_options(**opts)
        stmt.bind_stream(table.__arrow_c_stream__())
        affected = stmt.execute_update()
        stmt.close()
    finally:
        conn.close()
        db.close()
    return affected


def naive(value):
    """Drop a timestamp's tzinfo.

    The reader hands a PostgreSQL ``timestamp`` back as a UTC-stamped Arrow
    timestamp, which is not what the naive datetimes in the fixtures compare
    equal to; the wall-clock value is what these tests are about.
    """
    if isinstance(value, datetime.datetime):
        return value.replace(tzinfo=None)
    return value


def read_back(target, columns):
    """Every row of `target`, ordered by its first column, as a list of tuples."""
    rows = query('SELECT %s FROM %s ORDER BY 1'
                 % (", ".join('"%s"' % c for c in columns), target))
    return [tuple(naive(v) for v in row) for row in rows]


# ---------------------------------------------------------------------------
# fixtures


# Every string here is one the PostgreSQL array-literal parser could get wrong.
NASTY_STRINGS = [
    "",                     # empty element: only "" is legal, a bare one is a parse error
    "NULL",                 # the word NULL unquoted *is* a NULL
    "null",
    "  NULL  ",             # unquoted elements are whitespace-trimmed
    " leading and trailing ",
    "{braced}",
    "}",
    "{",
    "a,b",                  # the element separator
    ',',
    '"',                    # the quote character
    '""',
    "\\",                   # the escape character
    "\\\\",
    '\\"',
    'x"y\\z',
    "line1\nline2",
    "tab\there",
    "carriage\rreturn",
    "vertical\vtab",
    "form\ffeed",
    "héllo wörld",          # non-ASCII UTF-8
    "🚀 emoji ✨",
    "日本語テキスト",
    " nbsp ",
    "{NULL,NULL}",          # a whole array literal, as a value
    '{"a","b"}',
    "e'escaped'",
    "%s %d %%",             # format-string shapes, in case anything printf()s a value
    "'; DROP TABLE x; --",
    "x" * 5000,             # longer than any per-value scratch buffer
]


def nasty_table():
    """A table whose every column carries the awkward values for its type."""
    n = len(NASTY_STRINGS) + 8
    strings = list(NASTY_STRINGS) + [None] * 8
    ids = list(range(n))
    ints = [
        0, 1, -1, 2 ** 62, -(2 ** 62), 2 ** 63 - 1, -(2 ** 63), 127, -128,
    ]
    ints = (ints * n)[:n]
    ints[3] = None
    floats = [
        0.0, -0.0, 1.5, -1.5, 1e308, -1e308, 5e-324, 3.141592653589793,
        float("inf"), float("-inf"), float("nan"),
    ]
    floats = (floats * n)[:n]
    floats[2] = None
    dates = [
        datetime.date(1970, 1, 1), datetime.date(1, 1, 1), datetime.date(9999, 12, 31),
        datetime.date(2000, 2, 29), datetime.date(1969, 12, 31), datetime.date(2038, 1, 20),
    ]
    dates = (dates * n)[:n]
    dates[1] = None
    stamps = [
        datetime.datetime(1970, 1, 1, 0, 0, 0),
        datetime.datetime(1, 1, 1, 0, 0, 0),
        datetime.datetime(9999, 12, 31, 23, 59, 59, 999999),
        datetime.datetime(2000, 2, 29, 12, 34, 56, 789012),
        datetime.datetime(1969, 12, 31, 23, 59, 59, 1),
    ]
    stamps = (stamps * n)[:n]
    stamps[4] = None
    bools = ([True, False, None] * n)[:n]
    decs = [
        decimal.Decimal("0"), decimal.Decimal("-0.000001"),
        decimal.Decimal("12345678901234567890.123456"),
        decimal.Decimal("-99999999999999999999.999999"),
    ]
    decs = (decs * n)[:n]
    decs[1] = None
    return pyarrow.table({
        "id": pyarrow.array(ids, pyarrow.int64()),
        "txt": pyarrow.array(strings, pyarrow.string()),
        "i": pyarrow.array(ints, pyarrow.int64()),
        "f": pyarrow.array(floats, pyarrow.float64()),
        "d": pyarrow.array(dates, pyarrow.date32()),
        "ts": pyarrow.array(stamps, pyarrow.timestamp("us")),
        "b": pyarrow.array(bools, pyarrow.bool_()),
        "n": pyarrow.array(decs, pyarrow.decimal128(38, 6)),
    })


def random_table(seed, rows):
    """Randomised data over the same column types, with NULLs everywhere."""
    rng = random.Random(seed)
    alphabet = list('abc,{}"\\\n\t é日') + ["NULL", "", "\U0001f680"]

    def rnd_string():
        if rng.random() < 0.12:
            return None
        return "".join(rng.choice(alphabet) for _ in range(rng.randrange(0, 12)))

    def maybe(value):
        return None if rng.random() < 0.12 else value

    return pyarrow.table({
        "id": pyarrow.array(list(range(rows)), pyarrow.int64()),
        "txt": pyarrow.array([rnd_string() for _ in range(rows)], pyarrow.string()),
        "i": pyarrow.array([maybe(rng.randint(-(2 ** 63), 2 ** 63 - 1)) for _ in range(rows)],
                           pyarrow.int64()),
        "f": pyarrow.array([maybe(rng.uniform(-1e12, 1e12)) for _ in range(rows)],
                           pyarrow.float64()),
        "d": pyarrow.array([maybe(rng.randrange(-40000, 40000)) for _ in range(rows)],
                           pyarrow.date32()),
        "ts": pyarrow.array([maybe(rng.randrange(-2 * 10 ** 15, 2 * 10 ** 15))
                             for _ in range(rows)], pyarrow.timestamp("us")),
        "b": pyarrow.array([maybe(rng.random() < 0.5) for _ in range(rows)], pyarrow.bool_()),
        "n": pyarrow.array([maybe(decimal.Decimal(rng.randrange(-10 ** 20, 10 ** 20))
                                  .scaleb(-6)) for _ in range(rows)],
                           pyarrow.decimal128(38, 6)),
    })


# ---------------------------------------------------------------------------
# tests


def statements_for(columns_ddl, table):
    """Ingest `table` into a table of `columns_ddl` and return the INSERTs that ran.

    A statement-level trigger records current_query() for every INSERT that
    reaches the table, which is the only way from outside the driver to see
    which statement shape the ingest chose.
    """
    sql(
        "DROP TABLE IF EXISTS adbc_arr_seen",
        "DROP TABLE IF EXISTS adbc_arr_log",
        "CREATE TABLE adbc_arr_log (q text)",
        "CREATE TABLE adbc_arr_seen (%s)" % columns_ddl,
        "CREATE OR REPLACE FUNCTION adbc_arr_note() RETURNS trigger AS $$"
        " BEGIN INSERT INTO adbc_arr_log VALUES (current_query()); RETURN NULL; END $$"
        " LANGUAGE plpgsql",
        "CREATE TRIGGER adbc_arr_trg AFTER INSERT ON adbc_arr_seen"
        " FOR EACH STATEMENT EXECUTE FUNCTION adbc_arr_note()",
    )
    assert ingest(table, "adbc_arr_seen", mode="append") == table.num_rows
    return [row[0] for row in query("SELECT q FROM adbc_arr_log")]


def test_the_array_form_is_what_runs():
    """The fast path is actually taken, and it is the unnest form."""
    table = pyarrow.table({"id": pyarrow.array([1, 2, 3], pyarrow.int64()),
                           "txt": pyarrow.array(["a", "b", "c"], pyarrow.string())})
    seen = statements_for('"id" bigint, "txt" text', table)
    assert len(seen) == 1, seen
    assert "unnest(" in seen[0], seen[0]
    assert read_back("adbc_arr_seen", ["id", "txt"]) == [(1, "a"), (2, "b"), (3, "c")]
    sql("DROP TABLE adbc_arr_seen", "DROP TABLE adbc_arr_log",
        "DROP FUNCTION adbc_arr_note()")


def test_every_fixture_type_takes_the_array_form():
    """The adversarial fixture's whole schema is on the fast path, not just part of it."""
    seen = statements_for(
        '"id" bigint, "txt" text, "i" bigint, "f" double precision, "d" date,'
        ' "ts" timestamp, "b" boolean, "n" numeric(38, 6)', nasty_table())
    assert seen, "no INSERT reached the table"
    assert all("unnest(" in q for q in seen), seen
    sql("DROP TABLE adbc_arr_seen", "DROP TABLE adbc_arr_log",
        "DROP FUNCTION adbc_arr_note()")


def test_a_binary_column_keeps_the_ordinary_form():
    """The guard is real: an Arrow type with no array spelling here is not sent as one."""
    table = pyarrow.table({
        "id": pyarrow.array([1, 2], pyarrow.int64()),
        "bin": pyarrow.array([b"\x00\x01", None], pyarrow.binary()),
    })
    seen = statements_for('"id" bigint, "bin" bytea', table)
    assert seen, "no INSERT reached the table"
    assert not any("unnest(" in q for q in seen), seen
    sql("DROP TABLE adbc_arr_seen", "DROP TABLE adbc_arr_log",
        "DROP FUNCTION adbc_arr_note()")


def test_nasty_values_round_trip():
    """Every awkward value comes back exactly as it went in."""
    table = nasty_table()
    sql("DROP TABLE IF EXISTS adbc_arr_nasty")
    assert ingest(table, "adbc_arr_nasty") == table.num_rows
    got = read_back("adbc_arr_nasty", table.column_names)
    want = [tuple(row) for row in zip(*[c.to_pylist() for c in table.columns])]
    assert len(got) == len(want)
    for g, w in zip(got, want):
        for i, (a, b) in enumerate(zip(g, w)):
            if isinstance(b, float) and b != b:  # NaN
                assert isinstance(a, float) and a != a, (g, w)
                continue
            assert a == b, (table.column_names[i], repr(a), repr(b))
    sql("DROP TABLE adbc_arr_nasty")


@pytest.mark.parametrize("seed", [1, 2, 3])
def test_differential_against_the_row_at_a_time_path(seed):
    """Randomised data ingests identically with and without the array form.

    ``adbc.odbc.rows_per_insert=1`` turns off both batching rewrites, leaving the
    plain one-execute-per-row path -- the reference the array form has to match.
    """
    table = random_table(seed, 400)
    sql("DROP TABLE IF EXISTS adbc_arr_fast", "DROP TABLE IF EXISTS adbc_arr_ref")
    assert ingest(table, "adbc_arr_fast") == table.num_rows
    assert ingest(table, "adbc_arr_ref", rows_per_insert=1) == table.num_rows
    fast = read_back("adbc_arr_fast", table.column_names)
    ref = read_back("adbc_arr_ref", table.column_names)
    assert fast == ref
    want = [tuple(row) for row in zip(*[c.to_pylist() for c in table.columns])]
    assert fast == sorted(want, key=lambda r: r[0])
    sql("DROP TABLE adbc_arr_fast", "DROP TABLE adbc_arr_ref")


def test_nasty_values_match_the_row_at_a_time_path():
    """The adversarial fixture, differentially: fast path against reference path."""
    table = nasty_table()
    sql("DROP TABLE IF EXISTS adbc_arr_n1", "DROP TABLE IF EXISTS adbc_arr_n2")
    ingest(table, "adbc_arr_n1")
    ingest(table, "adbc_arr_n2", rows_per_insert=1)
    a = read_back("adbc_arr_n1", table.column_names)
    b = read_back("adbc_arr_n2", table.column_names)
    # NaN != NaN, so compare the float column by its repr and the rest by value.
    assert [(r[:3], repr(r[3]), r[4:]) for r in a] == [(r[:3], repr(r[3]), r[4:]) for r in b]
    sql("DROP TABLE adbc_arr_n1", "DROP TABLE adbc_arr_n2")


def test_unsupported_column_type_falls_back_whole():
    """A binary column has no array spelling here; the ingest still lands, intact."""
    payloads = [b"", b"\x00\x01\x02", b"\\x", b"{,}", bytes(range(256)), None]
    table = pyarrow.table({
        "id": pyarrow.array(list(range(len(payloads))), pyarrow.int64()),
        "bin": pyarrow.array(payloads, pyarrow.binary()),
        "txt": pyarrow.array(["a,b", "{", None, '"', "\\", ""], pyarrow.string()),
    })
    sql("DROP TABLE IF EXISTS adbc_arr_bin")
    assert ingest(table, "adbc_arr_bin") == table.num_rows
    got = read_back("adbc_arr_bin", ["id", "bin", "txt"])
    assert got == [tuple(row) for row in zip(*[c.to_pylist() for c in table.columns])]
    sql("DROP TABLE adbc_arr_bin")


def test_dates_outside_the_rendered_year_range_still_land():
    """A date the array form will not spell falls back mid-batch, exactly once."""
    dates = [
        datetime.date(2020, 1, 1),
        datetime.date(2021, 1, 1),
        None,
        # 10000-01-01 and 0001-01-01 - 1 day are outside what the array renderer
        # spells; PostgreSQL itself holds both.
        datetime.date(9999, 12, 31),
        datetime.date(2022, 1, 1),
    ]
    far = pyarrow.array([0, 1, None, 2932896, 3], pyarrow.date32())  # 2932896 = 10000-01-01
    table = pyarrow.table({
        "id": pyarrow.array(list(range(5)), pyarrow.int64()),
        "d": pyarrow.array([d.toordinal() - datetime.date(1970, 1, 1).toordinal()
                            if d else None for d in dates], pyarrow.date32()),
        "far": far,
    })
    sql("DROP TABLE IF EXISTS adbc_arr_dates", "DROP TABLE IF EXISTS adbc_arr_dates_ref")
    assert ingest(table, "adbc_arr_dates") == table.num_rows
    assert ingest(table, "adbc_arr_dates_ref", rows_per_insert=1) == table.num_rows
    assert (read_back("adbc_arr_dates", ["id", "d", "far"])
            == read_back("adbc_arr_dates_ref", ["id", "d", "far"]))
    assert query("SELECT count(*) FROM adbc_arr_dates")[0][0] == 5
    sql("DROP TABLE adbc_arr_dates", "DROP TABLE adbc_arr_dates_ref")


def test_all_null_column():
    """A column that is NULL from end to end, and an empty-string column beside it."""
    n = 50
    table = pyarrow.table({
        "id": pyarrow.array(list(range(n)), pyarrow.int64()),
        "allnull": pyarrow.array([None] * n, pyarrow.string()),
        "allempty": pyarrow.array([""] * n, pyarrow.string()),
        "nullint": pyarrow.array([None] * n, pyarrow.int64()),
    })
    sql("DROP TABLE IF EXISTS adbc_arr_null")
    assert ingest(table, "adbc_arr_null") == n
    got = read_back("adbc_arr_null", table.column_names)
    assert got == [(i, None, "", None) for i in range(n)]
    sql("DROP TABLE adbc_arr_null")


def test_single_row_and_empty_batches():
    """One row, and no rows at all, still behave."""
    sql("DROP TABLE IF EXISTS adbc_arr_one", "DROP TABLE IF EXISTS adbc_arr_zero")
    one = pyarrow.table({"id": pyarrow.array([7], pyarrow.int64()),
                         "txt": pyarrow.array(["NULL"], pyarrow.string())})
    assert ingest(one, "adbc_arr_one") == 1
    assert read_back("adbc_arr_one", ["id", "txt"]) == [(7, "NULL")]
    zero = pyarrow.table({"id": pyarrow.array([], pyarrow.int64()),
                          "txt": pyarrow.array([], pyarrow.string())})
    assert ingest(zero, "adbc_arr_zero") == 0
    assert query("SELECT count(*) FROM adbc_arr_zero")[0][0] == 0
    sql("DROP TABLE adbc_arr_one", "DROP TABLE adbc_arr_zero")


def test_more_rows_than_one_statement_carries():
    """Several array statements' worth of rows, so the chunking is exercised."""
    n = 25_000  # ADBC_ODBC_ARRAY_INGEST_ROWS is 10,000
    table = pyarrow.table({
        "id": pyarrow.array(list(range(n)), pyarrow.int64()),
        "txt": pyarrow.array(["v,%d{}" % i if i % 7 else None for i in range(n)],
                             pyarrow.string()),
    })
    sql("DROP TABLE IF EXISTS adbc_arr_many")
    assert ingest(table, "adbc_arr_many") == n
    assert query("SELECT count(*), count(txt), sum(id) FROM adbc_arr_many")[0] == (
        n, n - len(range(0, n, 7)), n * (n - 1) // 2)
    assert query("SELECT txt FROM adbc_arr_many WHERE id = 1")[0][0] == "v,1{}"
    sql("DROP TABLE adbc_arr_many")


def test_single_column_table():
    """One column means single-argument unnest, which PostgreSQL plans differently."""
    values = ["a,b", None, "", "{}", '"', "\\", "NULL"] * 40
    table = pyarrow.table({"txt": pyarrow.array(values, pyarrow.string())})
    sql("DROP TABLE IF EXISTS adbc_arr_one_col")
    assert ingest(table, "adbc_arr_one_col") == len(values)
    got = sorted(row[0] for row in query('SELECT "txt" FROM adbc_arr_one_col')
                 if row[0] is not None)
    assert got == sorted(v for v in values if v is not None)
    assert query("SELECT count(*) FROM adbc_arr_one_col WHERE txt IS NULL")[0][0] == 40
    sql("DROP TABLE adbc_arr_one_col")


def test_values_wider_than_one_statement_carries():
    """Strings big enough to hit the per-parameter byte ceiling, so a chunk is narrowed.

    ADBC_ODBC_ARRAY_INGEST_MAX_BYTES is 32 MB and these rows are about 38 MB, so
    the renderer has to stop short of the row count it started with and the rest
    of the batch goes in the statement after it.
    """
    n = 300
    big = "x,{}\"\\" * 25_600  # ~128 kB each, all of it needing quoting
    table = pyarrow.table({
        "id": pyarrow.array(list(range(n)), pyarrow.int64()),
        "txt": pyarrow.array([big + str(i) for i in range(n)], pyarrow.string()),
    })
    sql("DROP TABLE IF EXISTS adbc_arr_wide")
    assert ingest(table, "adbc_arr_wide") == n
    assert query("SELECT count(*), count(DISTINCT txt) FROM adbc_arr_wide")[0] == (n, n)
    assert query('SELECT "txt" FROM adbc_arr_wide WHERE id = 7')[0][0] == big + "7"
    assert query('SELECT "txt" FROM adbc_arr_wide WHERE id = %d' % (n - 1))[0][0] == \
        big + str(n - 1)
    sql("DROP TABLE adbc_arr_wide")


def test_composes_with_parallel_ingest():
    """The array form and adbc.odbc.ingest_connections are not rivals."""
    n = 5000
    table = pyarrow.table({
        "id": pyarrow.array(list(range(n)), pyarrow.int64()),
        "txt": pyarrow.array(['"%d,{}\\' % i for i in range(n)], pyarrow.string()),
    })
    sql("DROP TABLE IF EXISTS adbc_arr_par")
    assert ingest(table, "adbc_arr_par", connections=4) == n
    assert query("SELECT count(*), count(DISTINCT id), sum(id) FROM adbc_arr_par")[0] == (
        n, n, n * (n - 1) // 2)
    assert query("SELECT txt FROM adbc_arr_par WHERE id = 42")[0][0] == '"42,{}\\'
    sql("DROP TABLE adbc_arr_par")


def test_append_into_a_wider_typed_table():
    """PostgreSQL assignment casts do the narrowing the array form relies on."""
    sql("DROP TABLE IF EXISTS adbc_arr_types",
        'CREATE TABLE adbc_arr_types ("id" integer, "s" varchar(20), "r" real,'
        ' "n" numeric(10, 2))')
    table = pyarrow.table({
        "id": pyarrow.array([1, 2, 3], pyarrow.int64()),
        "s": pyarrow.array(["a,b", None, "{}"], pyarrow.string()),
        "r": pyarrow.array([1.5, -2.25, None], pyarrow.float64()),
        "n": pyarrow.array([decimal.Decimal("1.23"), None, decimal.Decimal("-4.56")],
                           pyarrow.decimal128(10, 2)),
    })
    assert ingest(table, "adbc_arr_types", mode="append") == 3
    assert read_back("adbc_arr_types", ["id", "s", "r", "n"]) == [
        (1, "a,b", 1.5, decimal.Decimal("1.23")),
        (2, None, -2.25, None),
        (3, "{}", None, decimal.Decimal("-4.56")),
    ]
    sql("DROP TABLE adbc_arr_types")


def test_no_cast_to_the_target_column_falls_back():
    """A column PostgreSQL will not assignment-cast to keeps the ordinary path.

    An Arrow date against a text column: the array form's ``?::date[]`` yields a
    date, and there is no assignment cast from date to text, so the prepared
    statement is refused and the ingest falls back with nothing applied.
    """
    sql("DROP TABLE IF EXISTS adbc_arr_nocast",
        'CREATE TABLE adbc_arr_nocast ("id" bigint, "d" text)')
    table = pyarrow.table({
        "id": pyarrow.array([1, 2], pyarrow.int64()),
        "d": pyarrow.array([0, 19000], pyarrow.date32()),
    })
    assert ingest(table, "adbc_arr_nocast", mode="append") == 2
    assert read_back("adbc_arr_nocast", ["id", "d"]) == [(1, "1970-01-01"), (2, "2022-01-08")]
    sql("DROP TABLE adbc_arr_nocast")
