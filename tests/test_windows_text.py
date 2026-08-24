# Copyright 2026 the adbcbridge authors
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
#
# SPDX-License-Identifier: Apache-2.0

"""Non-ASCII text through every path that carries it, against SQLite.

These pass on unixODBC, which hands narrow `char*` to the driver as UTF-8.  They
failed on Windows whenever the driver reached an ODBC entry point in its narrow
form: the Windows driver manager transcodes narrow text from the system ANSI
codepage (1252 on a Western install) to UTF-16, so UTF-8 bytes handed to
SQLExecDirect were read as if they were cp1252 and stored double-encoded.  The
fix (src/odbc_text.c) takes the W entry points on Windows; this file is what
found the bug, from the first Windows run, and what verifies the fix.

Bound parameters already go out as SQL_C_WCHAR via OdbcUtf8ToUtf16Into and are
correct on both platforms -- so each test below pairs the suspect path with the
parameter path, and the contrast is the diagnosis.

stdlib sqlite3 is the ground truth throughout: it never goes through ODBC, so
what it sees is what is really on disk.

    ADBC_ODBC_DRIVER=build/Release/libadbc_driver_odbc.dll \
    SQLITE_ODBC_DRIVER="SQLite3 ODBC Driver" python tests/test_windows_text.py
"""
import os
import pathlib
import sqlite3
import sys
import tempfile

import pyarrow as pa
import adbc_driver_manager.dbapi as dbapi

HERE = pathlib.Path(__file__).resolve().parent
DRIVER = os.environ.get(
    "ADBC_ODBC_DRIVER", str(HERE.parent / "build" / "libadbc_driver_odbc.so")
)
SQLITE_ODBC = os.environ.get("SQLITE_ODBC_DRIVER", "SQLite3")

LATIN = "héllo"        # every character exists in cp1252
CJK = "日本語"          # no character exists in cp1252: best-fit mapping loses them
EURO = "prix_€"        # U+20AC is cp1252 0x80, which is not valid UTF-8 on its own

failures = []


def check(name, ok, detail=""):
    print(f"  {'PASS' if ok else 'FAIL'}  {name}{'  ' + detail if detail else ''}")
    if not ok:
        failures.append(name)


def connect(db):
    uri = f"Driver={SQLITE_ODBC};Database={db};"
    return dbapi.connect(
        driver=DRIVER, db_kwargs={"uri": uri, "adbc.odbc.delegate": "never"}
    )


def fresh():
    return os.path.join(tempfile.mkdtemp(), "t.db")


def on_disk(db, table):
    """The bytes SQLite really holds, read without going through ODBC."""
    raw = sqlite3.connect(db)
    try:
        return raw.execute(f"SELECT CAST(s AS BLOB) FROM {table}").fetchone()[0]
    finally:
        raw.close()


def test_literal_vs_parameter():
    """A literal in statement text must store the same bytes as a bound parameter."""
    print("literal in statement text vs bound parameter")
    for label, text in (("latin1-representable", LATIN), ("outside cp1252", CJK)):
        db = fresh()
        with connect(db) as c:
            with c.cursor() as cur:
                cur.execute("CREATE TABLE lit (s TEXT)")
                cur.execute("CREATE TABLE par (s TEXT)")
                cur.execute(f"INSERT INTO lit VALUES ('{text}')")
                cur.execute("INSERT INTO par VALUES (?)", (text,))
            c.commit()
        want = text.encode("utf-8")
        got_par = on_disk(db, "par")
        got_lit = on_disk(db, "lit")
        check(f"bound parameter, {label}", got_par == want, got_par.hex())
        check(
            f"statement literal, {label}",
            got_lit == want,
            f"{got_lit.hex()} (want {want.hex()})",
        )


def test_ingest():
    """Arrow bulk ingest binds parameters, so it must be exact."""
    print("arrow adbc_ingest")
    db = fresh()
    with connect(db) as c:
        with c.cursor() as cur:
            cur.adbc_ingest("ing", pa.table({"s": pa.array([LATIN, CJK])}), mode="create")
        c.commit()
    raw = sqlite3.connect(db)
    got = [r[0] for r in raw.execute("SELECT s FROM ing")]
    raw.close()
    check("adbc_ingest round-trip", got == [LATIN, CJK], repr(got))


def test_predicate_matches():
    """A non-ASCII literal in a WHERE clause must match a correctly-stored row.

    This is the quiet one: it returns zero rows and raises nothing.
    """
    print("non-ASCII literal in a WHERE clause")
    db = fresh()
    with connect(db) as c:
        with c.cursor() as cur:
            cur.execute("CREATE TABLE t (s TEXT)")
            cur.execute("INSERT INTO t VALUES (?)", (LATIN,))
        c.commit()
    with connect(db) as c:
        with c.cursor() as cur:
            cur.execute(f"SELECT COUNT(*) AS n FROM t WHERE s = '{LATIN}'")
            n_lit = cur.fetch_arrow_table().to_pydict()["n"][0]
        with c.cursor() as cur:
            cur.execute("SELECT COUNT(*) AS n FROM t WHERE s = ?", (LATIN,))
            n_par = cur.fetch_arrow_table().to_pydict()["n"][0]
    check("predicate as bound parameter", n_par == 1, f"matched {n_par}")
    check("predicate as statement literal", n_lit == 1, f"matched {n_lit}")


def test_column_names():
    """Column names come back through SQLDescribeCol and land in the Arrow schema.

    Built with stdlib sqlite3 so the names on disk are known-good UTF-8; a narrow
    SQLDescribeCol returns them in the ANSI codepage, and cp1252 0x80 is not a
    valid UTF-8 start byte -- so the schema is not merely wrong, it is invalid.
    """
    print("non-ASCII column names")
    db = fresh()
    raw = sqlite3.connect(db)
    raw.execute(f'CREATE TABLE t ("{EURO}" TEXT, "naïve" TEXT)')
    raw.execute("INSERT INTO t VALUES ('a', 'b')")
    raw.commit()
    raw.close()
    with connect(db) as c:
        with c.cursor() as cur:
            cur.execute("SELECT * FROM t")
            try:
                names = cur.fetch_arrow_table().schema.names
            except UnicodeDecodeError as e:
                check("column names are valid UTF-8", False, f"{type(e).__name__}: {e}")
                return
    check("column names round-trip", names == [EURO, "naïve"], repr(names))


def test_value_read_path():
    """Values written by another client must read back byte-exact.

    Separated from the write path deliberately: on Windows this passed before the
    fix, which is what narrowed the bug to statement text and metadata rather
    than to fetching.
    """
    print("value read path (row written outside ODBC)")
    db = fresh()
    raw = sqlite3.connect(db)
    raw.execute("CREATE TABLE t (s TEXT)")
    raw.executemany("INSERT INTO t VALUES (?)", [(LATIN,), (CJK,)])
    raw.commit()
    raw.close()
    with connect(db) as c:
        with c.cursor() as cur:
            cur.execute("SELECT s FROM t ORDER BY rowid")
            got = cur.fetch_arrow_table().to_pydict()["s"]
    check("SELECT returns exact text", got == [LATIN, CJK], repr(got))


def test_ddl_identifier():
    """CREATE TABLE with a non-ASCII identifier must create that exact name."""
    print("non-ASCII identifier in DDL")
    db = fresh()
    name = "tabelle_ä"
    with connect(db) as c:
        with c.cursor() as cur:
            cur.execute(f'CREATE TABLE "{name}" (x INT)')
        c.commit()
    raw = sqlite3.connect(db)
    names = [r[0] for r in raw.execute("SELECT name FROM sqlite_master WHERE type='table'")]
    raw.close()
    check("identifier round-trip", names == [name], repr(names))


def test_error_text():
    """A diagnostic naming an object outside cp1252 must arrive as the same text.

    Inside cp1252 the two conversions of a narrow SQLGetDiagRec cancel out and the
    message looks right for the wrong reason; a name outside it does not cancel.
    """
    print("non-ASCII text in an error message")
    db = fresh()
    missing = f"tabelle_{CJK}_fehlt"
    with connect(db) as c:
        with c.cursor() as cur:
            try:
                cur.execute(f'SELECT * FROM "{missing}"')
                check("error names the missing table", False, "no error raised")
                return
            except Exception as e:  # noqa: BLE001 -- the message is the point
                text = str(e)
    check("error names the missing table", missing in text, text[:160])


def main():
    print(f"driver      {DRIVER}")
    print(f"odbc driver {SQLITE_ODBC}")
    if sys.platform == "win32":
        import ctypes

        print(f"ANSI codepage {ctypes.windll.kernel32.GetACP()}")
    print()
    for fn in (
        test_literal_vs_parameter,
        test_ingest,
        test_predicate_matches,
        test_column_names,
        test_value_read_path,
        test_ddl_identifier,
        test_error_text,
    ):
        fn()
        print()
    if failures:
        print(f"{len(failures)} failed:")
        for name in failures:
            print(f"  - {name}")
        raise SystemExit(1)
    print("all passed")


if __name__ == "__main__":
    main()
