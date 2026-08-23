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

"""
Driver quirks for adbcbridge (the ADBC-over-ODBC driver) talking to SQLite
through the SQLite ODBC driver.

The quirks object describes the driver + backend combination to the ADBC
Driver Foundry validation suite (https://github.com/adbc-drivers/validation).
"""

import functools
import os
import re
import typing
from pathlib import Path

import adbc_driver_manager

from adbc_drivers_validation import model

HERE = Path(__file__).resolve().parent
REPO_ROOT = HERE.parent.parent


def _sqlite_odbc_driver() -> str:
    """The SQLite ODBC driver shared object (or a registered DSN driver name)."""
    return os.environ.get("SQLITE_ODBC_DRIVER", "SQLite3")


def _database_path() -> str:
    """The SQLite database file the suite runs against."""
    return os.environ.get(
        "ADBCBRIDGE_VALIDATION_DB", "/tmp/adbcbridge-validation.sqlite"
    )


def _split_sql(statement: str) -> list[str]:
    """Split on top-level ``;``, stripping comments, preserving statement text."""
    out: list[str] = []
    buf: list[str] = []
    i = 0
    n = len(statement)
    while i < n:
        ch = statement[i]
        if ch in ("'", '"', "`"):
            # Copy the whole quoted run, honouring doubled-quote escapes.
            buf.append(ch)
            i += 1
            while i < n:
                if statement[i] == ch:
                    if i + 1 < n and statement[i + 1] == ch:
                        buf.append(ch * 2)
                        i += 2
                        continue
                    buf.append(ch)
                    i += 1
                    break
                buf.append(statement[i])
                i += 1
            continue
        if statement.startswith("--", i):
            while i < n and statement[i] != "\n":
                i += 1
            continue
        if statement.startswith("/*", i):
            end = statement.find("*/", i + 2)
            i = n if end < 0 else end + 2
            continue
        if ch == ";":
            piece = "".join(buf).strip()
            if piece:
                out.append(piece)
            buf = []
            i += 1
            continue
        buf.append(ch)
        i += 1

    piece = "".join(buf).strip()
    if piece:
        out.append(piece)
    return out or [statement]


class OdbcSqliteQuirks(model.DriverQuirks):
    """adbcbridge (ADBC -> ODBC) driving the SQLite ODBC driver."""

    name = "odbc_sqlite"
    driver = "adbc_driver_odbc"
    # Must match ADBC_ODBC_DRIVER_NAME in src/odbc_internal.h.
    driver_name = "ADBC ODBC Driver"
    vendor_name = "SQLite (via ODBC)"
    # SQLiteODBC reports the SQLite library version.
    vendor_version = re.compile(r"3\.\d+\.\d+.*")
    short_version = "3"

    features = model.DriverFeatures(
        # ConnectionGetTableSchema is implemented (SQLColumns-backed).
        connection_get_table_schema=True,
        # ConnectionGetStatistics is not wired into the driver vtable.
        connection_get_statistics=False,
        # No SQL_ATTR_CURRENT_CATALOG / schema support for SQLite.
        connection_set_current_catalog=False,
        connection_set_current_schema=False,
        # ConnectionCommit / ConnectionRollback are implemented.
        connection_transactions=True,
        # ConnectionGetObjects is implemented in src/odbc_objects.c.
        get_objects=True,
        # SQLPrimaryKeys / SQLForeignKeys are queried; no check/unique support.
        get_objects_constraints_check=False,
        get_objects_constraints_foreign=True,
        get_objects_constraints_primary=True,
        get_objects_constraints_unique=False,
        # No ODBC:type / xdbc field metadata is emitted by odbc_reader.c.
        metadata_type_name=False,
        # SQLBindParameter-backed binding (src/odbc_bind.c).
        statement_bind=True,
        statement_bulk_ingest=True,
        # SQLite has no catalogs or schemas to ingest into.
        statement_bulk_ingest_catalog=False,
        statement_bulk_ingest_schema=False,
        statement_bulk_ingest_temporary=True,
        # SQLite temp tables live in the `temp` database but are resolved by
        # bare name, so they share the namespace with ordinary tables.
        quirk_bulk_ingest_temporary_shares_namespace=True,
        # StatementExecuteSchema is implemented (prepare + SQLDescribeCol).
        statement_execute_schema=True,
        # StatementGetParameterSchema is NOT in the driver vtable.
        statement_get_parameter_schema=False,
        statement_prepare=True,
        statement_rows_affected=True,
        # SQLRowCount after DDL returns 0 from SQLiteODBC, and the driver
        # passes that through verbatim.
        statement_rows_affected_ddl=True,
        # SQLite exposes exactly one catalog/schema pair, both unnamed.
        current_catalog=None,
        current_schema=None,
        secondary_catalog=None,
        secondary_schema=None,
        secondary_catalog_schema=None,
        supported_xdbc_fields=[],
    )

    setup = model.DriverSetup(
        database={
            "uri": model.FromEnv(
                "ADBCBRIDGE_VALIDATION_URI",
            ),
        },
        connection={},
        statement={},
    )

    @property
    def queries_paths(self) -> tuple[Path]:
        return (HERE / "queries" / "odbc_sqlite",)

    def bind_parameter(self, index: int) -> str:
        # ODBC always uses positional `?` markers.
        return "?"

    def is_table_not_found(self, table_name: str | None, error: Exception) -> bool:
        text = str(error).lower()
        if "no such table" not in text and "not found" not in text:
            return False
        if table_name is None:
            return True
        return table_name.lower() in text

    def is_retryable(self, error: Exception) -> bool:
        text = str(error).lower()
        return "database is locked" in text or "database table is locked" in text

    def qualify_temp_table(
        self, _cursor: adbc_driver_manager.dbapi.Cursor, name: str
    ) -> str:
        # SQLite resolves `temp` tables by bare name.
        return self.quote_one_identifier(name)

    def quote_one_identifier(self, identifier: str) -> str:
        identifier = identifier.replace('"', '""')
        return f'"{identifier}"'

    def split_statement(self, statement: str) -> list[str]:
        # NOT quirks.split_statement(statement, dialect="sqlite"): passing a
        # dialect makes sqlglot *transpile* the SQL (SMALLINT -> INTEGER,
        # DOUBLE PRECISION -> REAL, TIMESTAMP 'x' -> CAST('x' AS TIMESTAMP)),
        # which silently changes which SQL type each type test exercises and
        # produces bogus PASS/FAIL results.
        #
        # NOT quirks.split_statement(statement) either: the line-based
        # fallback only breaks on lines that *end* with ";", so a statement
        # with a trailing "-- comment" gets glued to the next one, and
        # SQLiteODBC rejects multiple statements per SQLExecDirect.
        #
        # So: split on top-level ";", honouring quoting, and drop comments,
        # leaving the statement text itself untouched.
        return _split_sql(statement)

    @property
    def sample_ddl_constraints(self) -> list[str]:
        # Table and column names are dictated by
        # adbc_drivers_validation.tests.connection.TestConnection.
        return [
            'CREATE TABLE "constraint_check" ('
            " a INTEGER,"
            " CHECK (a > 0)"
            ")",
            'CREATE TABLE "constraint_unique" ('
            " a INTEGER UNIQUE,"
            " b INTEGER,"
            " c INTEGER,"
            " UNIQUE (c, b)"
            ")",
            'CREATE TABLE "constraint_primary" ('
            " a INTEGER NOT NULL PRIMARY KEY"
            ")",
            'CREATE TABLE "constraint_primary_multi" ('
            " a INTEGER NOT NULL,"
            " b INTEGER NOT NULL,"
            " PRIMARY KEY (b, a)"
            ")",
            'CREATE TABLE "constraint_primary_multi2" ('
            " a INTEGER NOT NULL,"
            " b INTEGER NOT NULL,"
            " PRIMARY KEY (a, b)"
            ")",
            'CREATE TABLE "constraint_foreign" ('
            " b INTEGER,"
            " FOREIGN KEY (b) REFERENCES constraint_primary (a)"
            ")",
            'CREATE TABLE "constraint_foreign_multi" ('
            " b INTEGER,"
            " c INTEGER,"
            " FOREIGN KEY (c, b) REFERENCES constraint_primary_multi2 (a, b)"
            ")",
        ]

    def drop_table(
        self,
        *,
        table_name: str,
        schema_name: str | None = None,
        catalog_name: str | None = None,
        if_exists: bool = True,
        temporary: bool = False,
    ) -> str:
        # SQLite drops temporary tables with the same statement as normal ones.
        name = self.quote_identifier(
            *[part for part in (catalog_name, schema_name, table_name) if part]
        )
        if if_exists:
            return f"DROP TABLE IF EXISTS {name}"
        return f"DROP TABLE {name}"

    def query_override(self, context: str, default: str) -> str:
        if context == "TestStatement.sample_table":
            return 'CREATE TABLE "sample_table" (id INTEGER, value TEXT)'
        return super().query_override(context, default)


@functools.cache
def get_quirks(_test_config: str = "odbc_sqlite") -> model.DriverQuirks:
    return OdbcSqliteQuirks()


def connection_uri() -> str:
    """The ODBC connection string used by the suite."""
    return f"Driver={_sqlite_odbc_driver()};Database={_database_path()};"


def driver_path() -> str:
    """Path to the built adbcbridge shared library."""
    explicit = os.environ.get("ADBC_ODBC_DRIVER")
    if explicit:
        return explicit
    return str(REPO_ROOT / "build" / "libadbc_driver_odbc.so")


def default_environment() -> dict[str, typing.Any]:
    """Environment defaults so the suite can run without manual exports."""
    return {"ADBCBRIDGE_VALIDATION_URI": connection_uri()}
