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

"""Native delegation: adbcbridge hands the driver over to a native ADBC driver.

Run under pytest, or directly:

    ADBC_ODBC_DRIVER=build/libadbc_driver_odbc.so python tests/test_delegate.py

Each test skips when its prerequisites (an ODBC driver, a server, a native ADBC
driver package) are missing.
"""

import os
import pathlib
import statistics
import tempfile
import time
import traceback

import adbc_driver_manager.dbapi as dbapi

try:
    import pytest
except ImportError:  # running as a plain script
    pytest = None

HERE = pathlib.Path(__file__).resolve().parent
DRIVER = os.environ.get(
    "ADBC_ODBC_DRIVER", str(HERE.parent / "build" / "libadbc_driver_odbc.so")
)
PG_URI = os.environ.get("PG_URI", "postgresql://adbc:adbc@127.0.0.1:15432/adbc")
ROWS = int(os.environ.get("DELEGATE_ROWS", "1000000"))


class Skipped(Exception):
    pass


def skip(reason):
    if pytest is not None:
        pytest.skip(reason)
    raise Skipped(reason)


def bridge(uri, **options):
    """Connect through adbcbridge."""
    db_kwargs = {"uri": uri}
    db_kwargs.update(options)
    return dbapi.connect(driver=DRIVER, db_kwargs=db_kwargs)


def driver_name(conn):
    return conn.adbc_get_info().get("driver_name", "")


def delegated_to(conn):
    """Who is serving this connection?

    Delegation replaces adbcbridge's whole function table, so the option only
    answers on the ODBC path; once the native driver has taken over it does not
    know the key.
    """
    try:
        return conn.adbc_connection.get_option("adbc.odbc.delegated_to")
    except Exception:
        return None


def pg_odbc_connection_string():
    drv = os.environ.get("POSTGRES_ODBC_DRIVER") or os.environ.get("PSQL_ODBC_DRIVER")
    if not drv:
        skip("set POSTGRES_ODBC_DRIVER to the psqlodbc shared object")
    return (
        f"Driver={drv};Server=127.0.0.1;Port=15432;Database=adbc;Uid=adbc;Pwd=adbc;"
    )


def require_postgres():
    try:
        import adbc_driver_postgresql.dbapi as pgn
    except ImportError:
        skip("adbc_driver_postgresql is not installed")
    try:
        with pgn.connect(PG_URI) as conn, conn.cursor() as cur:
            cur.execute("SELECT 1")
            cur.fetchall()
    except Exception as e:
        skip(f"no PostgreSQL server at {PG_URI}: {e}")
    return pgn


def require_sqlite_odbc():
    drv = os.environ.get("SQLITE_ODBC_DRIVER", "SQLite3")
    try:
        import adbc_driver_sqlite  # noqa: F401
    except ImportError:
        skip("adbc_driver_sqlite is not installed")
    return drv


# --- (a) a native URI is served by the native driver ------------------------


def test_postgres_uri_delegates():
    require_postgres()
    with bridge(PG_URI) as conn:
        name = driver_name(conn)
        assert "PostgreSQL" in name and "ODBC" not in name, name
        with conn.cursor() as cur:
            cur.execute("SELECT 1 AS one")
            assert cur.fetch_arrow_table().to_pydict() == {"one": [1]}
    print(f"postgres uri, delegate=auto -> {name}")


def test_postgres_delegated_fetch_is_native_speed():
    pgn = require_postgres()
    query = (
        f"SELECT i::int AS id, (i*0.5)::float8 AS val, 'row_'||i AS txt, "
        f"DATE '2024-01-01' + (i % 365) AS dt FROM generate_series(1,{ROWS}) i"
    )

    def timed(connect):
        def once():
            with connect() as conn, conn.cursor() as cur:
                start = time.perf_counter()
                cur.execute(query)
                table = cur.fetch_arrow_table()
                assert table.num_rows == ROWS
                return time.perf_counter() - start

        once()  # warm the server's cache
        return statistics.median(once() for _ in range(3))

    native = timed(lambda: pgn.connect(PG_URI))
    delegated = timed(lambda: bridge(PG_URI))
    odbc_conn = pg_odbc_connection_string()
    over_odbc = timed(lambda: bridge(odbc_conn, **{"adbc.odbc.delegate": "never"}))
    print(
        f"{ROWS} rows: native {native:.2f}s, bridge (delegated) {delegated:.2f}s, "
        f"bridge (ODBC) {over_odbc:.2f}s"
    )
    # Delegation is a hand-over, not a wrapper: the bridge should cost nothing
    # measurable on top of the native driver.
    assert delegated < native * 1.5 + 0.25, (native, delegated)
    assert delegated < over_odbc, (delegated, over_odbc)


# --- (b) an ODBC connection string is recognized too ------------------------


def test_postgres_odbc_connection_string_delegates():
    require_postgres()
    conn_str = pg_odbc_connection_string()
    with bridge(conn_str) as conn:
        name = driver_name(conn)
        assert "PostgreSQL" in name and "ODBC" not in name, name
        with conn.cursor() as cur:
            cur.execute("SELECT 1 AS one")
            assert cur.fetch_arrow_table().to_pydict() == {"one": [1]}
    print(f"postgres ODBC connection string, delegate=auto -> {name}")


def test_postgres_delegate_never_uses_odbc():
    conn_str = pg_odbc_connection_string()
    with bridge(conn_str, **{"adbc.odbc.delegate": "never"}) as conn:
        name = driver_name(conn)
        assert "ADBC ODBC Driver" in name, name
        assert delegated_to(conn) == "odbc"
        with conn.cursor() as cur:
            cur.execute("SELECT 1 AS one")
            assert cur.fetch_arrow_table().to_pydict() == {"one": [1]}
    print(f"postgres ODBC connection string, delegate=never -> {name}")


def write_dsn(name, entries):
    """Point unixODBC at a throwaway odbc.ini holding one DSN."""
    ini = os.path.join(tempfile.mkdtemp(), "odbc.ini")
    with open(ini, "w") as f:
        f.write(f"[{name}]\n")
        for key, value in entries.items():
            f.write(f"{key} = {value}\n")
    return ini


class odbcini:
    """Context manager for ODBCINI (unixODBC's user odbc.ini)."""

    def __init__(self, path):
        self.path = path

    def __enter__(self):
        self.old = os.environ.get("ODBCINI")
        os.environ["ODBCINI"] = self.path

    def __exit__(self, *exc):
        if self.old is None:
            del os.environ["ODBCINI"]
        else:
            os.environ["ODBCINI"] = self.old


def test_postgres_dsn_delegates():
    require_postgres()
    drv = os.environ.get("POSTGRES_ODBC_DRIVER") or os.environ.get("PSQL_ODBC_DRIVER")
    if not drv:
        skip("set POSTGRES_ODBC_DRIVER to the psqlodbc shared object")
    ini = write_dsn(
        "adbc_pg",
        {
            "Driver": drv,
            "Servername": "127.0.0.1",
            "Port": "15432",
            "Database": "adbc",
            "Username": "adbc",
            "Password": "adbc",
        },
    )
    with odbcini(ini):
        # The native URI is rebuilt out of the odbc.ini section.
        with dbapi.connect(driver=DRIVER, db_kwargs={"dsn": "adbc_pg"}) as conn:
            name = driver_name(conn)
            assert "PostgreSQL" in name and "ODBC" not in name, name
            with conn.cursor() as cur:
                cur.execute("SELECT 1 AS one")
                assert cur.fetch_arrow_table().to_pydict() == {"one": [1]}
    print(f"postgres DSN, delegate=auto -> {name}")


# --- (c) SQLite ------------------------------------------------------------


def test_sqlite_file_delegates():
    sqlite_odbc = require_sqlite_odbc()
    path = os.path.join(tempfile.mkdtemp(), "delegate.db")

    conn_str = f"Driver={sqlite_odbc};Database={path};"
    with bridge(conn_str) as conn:
        name = driver_name(conn)
        assert "SQLite" in name and "ODBC" not in name, name
        with conn.cursor() as cur:
            cur.execute("CREATE TABLE t (i INTEGER)")
            cur.execute("INSERT INTO t VALUES (42)")
        conn.commit()
    print(f"sqlite ODBC connection string, delegate=auto -> {name}")

    # The same file, read back over ODBC: delegation is transparent.
    with bridge(conn_str, **{"adbc.odbc.delegate": "never"}) as conn:
        assert delegated_to(conn) == "odbc"
        with conn.cursor() as cur:
            cur.execute("SELECT i FROM t")
            assert cur.fetch_arrow_table().to_pydict() == {"i": [42]}

    # A native sqlite: URI works as well.
    with bridge(f"sqlite:{path}") as conn:
        assert "SQLite" in driver_name(conn)
        with conn.cursor() as cur:
            cur.execute("SELECT i FROM t")
            assert cur.fetch_arrow_table().to_pydict() == {"i": [42]}


# --- (d) no native driver for this target -> ODBC ---------------------------


def test_dsn_without_native_driver_falls_back_to_odbc():
    drv = os.environ.get("MARIADB_ODBC_DRIVER")
    if not drv:
        skip("set MARIADB_ODBC_DRIVER to the MariaDB Connector/ODBC shared object")
    ini = write_dsn(
        "adbc_mariadb",
        {
            "Driver": drv,
            "Server": "127.0.0.1",
            "Port": "13306",
            "Database": "adbc",
            "User": "adbc",
            "Password": "adbc",
        },
    )
    with odbcini(ini):
        with dbapi.connect(driver=DRIVER, db_kwargs={"dsn": "adbc_mariadb"}) as conn:
            name = driver_name(conn)
            assert "ADBC ODBC Driver" in name, name
            assert delegated_to(conn) == "odbc"
            last_error = conn.adbc_database.get_option("adbc.odbc.delegate.last_error")
            assert "no native ADBC driver" in last_error, last_error
            with conn.cursor() as cur:
                cur.execute("SELECT 1 AS one")
                assert cur.fetch_arrow_table().to_pydict() == {"one": [1]}
    print(f"MariaDB DSN, delegate=auto -> {name} ({last_error})")


# --- policy ----------------------------------------------------------------


def test_delegate_always_errors_without_native_driver():
    try:
        with bridge("Driver=NoSuchDriver;Database=x;", **{"adbc.odbc.delegate": "always"}):
            raise AssertionError("expected delegation to fail")
    except dbapi.Error as e:
        assert "always" in str(e), e
        assert "no native ADBC driver" in str(e), e
    print("delegate=always without a native driver -> error")


def test_env_var_disables_delegation():
    sqlite_odbc = require_sqlite_odbc()
    path = os.path.join(tempfile.mkdtemp(), "env.db")
    old = os.environ.get("ADBC_ODBC_DELEGATE")
    os.environ["ADBC_ODBC_DELEGATE"] = "never"
    try:
        with bridge(f"Driver={sqlite_odbc};Database={path};") as conn:
            assert delegated_to(conn) == "odbc"
            assert "ADBC ODBC Driver" in driver_name(conn)
    finally:
        if old is None:
            del os.environ["ADBC_ODBC_DELEGATE"]
        else:
            os.environ["ADBC_ODBC_DELEGATE"] = old
    print("ADBC_ODBC_DELEGATE=never -> ODBC")


def test_forced_driver_that_cannot_be_loaded_falls_back():
    sqlite_odbc = os.environ.get("SQLITE_ODBC_DRIVER", "SQLite3")
    path = os.path.join(tempfile.mkdtemp(), "fallback.db")
    with bridge(
        f"Driver={sqlite_odbc};Database={path};",
        **{"adbc.odbc.delegate.driver": "/nonexistent/libadbc_driver_sqlite.so"},
    ) as conn:
        assert delegated_to(conn) == "odbc"
        last_error = conn.adbc_database.get_option("adbc.odbc.delegate.last_error")
        assert last_error, "expected a diagnostic"
        with conn.cursor() as cur:
            cur.execute("SELECT 1 AS one")
            assert cur.fetch_arrow_table().to_pydict() == {"one": [1]}
    print(f"unloadable adbc.odbc.delegate.driver -> ODBC ({last_error})")


def main():
    failures = 0
    for name, fn in sorted(globals().items()):
        if not name.startswith("test_") or not callable(fn):
            continue
        try:
            fn()
            print(f"PASS {name}")
        except Skipped as e:
            print(f"SKIP {name}: {e}")
        except Exception:
            failures += 1
            print(f"FAIL {name}")
            traceback.print_exc()
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
