<!--
  Licensed to the Apache Software Foundation (ASF) under one
  or more contributor license agreements.  See the NOTICE file
  distributed with this work for additional information
  regarding copyright ownership.  The ASF licenses this file
  to you under the Apache License, Version 2.0 (the
  "License"); you may not use this file except in compliance
  with the License.  You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

  Unless required by applicable law or agreed to in writing,
  software distributed under the License is distributed on an
  "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
  KIND, either express or implied.  See the License for the
  specific language governing permissions and limitations
  under the License.
-->

# ADBC Driver Foundry validation suite

This directory wires adbcbridge into the
[ADBC Driver Foundry validation suite](https://github.com/adbc-drivers/validation),
running it against SQLite through the SQLite ODBC driver.

Latest results: [RESULTS.md](RESULTS.md).

## Requirements

**Python 3.13 or newer.** The validation suite declares
`requires-python = ">=3.13"` and genuinely needs it — `adbc_drivers_validation`
uses PEP 696 defaulted type parameters (`typing.Generator[None]`), which raise
`TypeError` at import time on 3.12. The repo's main `.venv` is 3.12, so the
suite gets its own virtualenv; see below.

Also required: the SQLite ODBC driver shared object and unixODBC. No server,
container, or DSN registration is needed — the driver is loaded by path.

## One-time setup

```shell
# from the repo root
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug && cmake --build build -j

git clone https://github.com/adbc-drivers/validation tests/validation-upstream

python3.13 -m venv .venv-validation
.venv-validation/bin/pip install -e tests/validation-upstream
```

Both `tests/validation-upstream/` and `.venv-validation/` are gitignored.

## Running the suite

```shell
cd tests/validation && \
  SQLITE_ODBC_DRIVER=/path/to/libsqlite3odbc.so \
  ADBCBRIDGE_VALIDATION_DB=/tmp/adbcbridge-validation.sqlite \
  ../../.venv-validation/bin/python -m pytest -q -rA
```

The exact command used to produce [RESULTS.md](RESULTS.md):

```shell
cd <repo>/tests/validation && \
  SQLITE_ODBC_DRIVER=/tmp/scratch/deb/ex/usr/lib/x86_64-linux-gnu/odbc/libsqlite3odbc.so \
  ADBCBRIDGE_VALIDATION_DB=/tmp/adbcbridge-validation.sqlite \
  ../../.venv-validation/bin/python -m pytest -q -rA --junitxml=/tmp/junit.xml
```

Delete the database file between runs for a clean slate; the suite drops and
recreates its own tables, so reusing it is fine.

### Environment variables

| Variable | Default | Meaning |
|---|---|---|
| `SQLITE_ODBC_DRIVER` | `SQLite3` | Path to `libsqlite3odbc.so` (or a registered driver name). |
| `ADBCBRIDGE_VALIDATION_DB` | `/tmp/adbcbridge-validation.sqlite` | SQLite database file the suite runs against. |
| `ADBC_ODBC_DRIVER` | `<repo>/build/libadbc_driver_odbc.so` | Path to the built adbcbridge shared library. |
| `ADBCBRIDGE_VALIDATION_URI` | derived from the two above | Full ODBC connection string. Set it to override completely. |

### Useful flags

```shell
# one test module
../../.venv-validation/bin/python -m pytest -q test_connection.py

# one case, with the SQL the suite runs
../../.venv-validation/bin/python -m pytest -q -k timestamp4 -vv

# list the queries the suite would run, without running them
../../.venv-validation/bin/python -m pytest --show-queries
```

## Layout

| Path | Purpose |
|---|---|
| `quirks.py` | `OdbcSqliteQuirks` — describes the driver + backend to the suite: declared features, connection setup, identifier quoting, statement splitting, constraint DDL. |
| `conftest.py` | Supplies the `driver` and `driver_path` fixtures the shared suite requires; re-exports the upstream fixtures. |
| `test_query.py`, `test_connection.py`, `test_statement.py`, `test_ingest.py` | Thin wrappers that parameterize the shared `adbc_drivers_validation.tests.*` classes with our quirks. |
| `queries/odbc_sqlite/` | Driver-specific overrides of individual base query cases (see below). |
| `pytest.ini` | Suite configuration; mirrors the upstream one plus the warning filters our driver needs. |

## Query overrides

The suite ships a base corpus of type/ingest cases written in standard SQL.
Where SQLite cannot express a case, a file at the same relative path under
`queries/odbc_sqlite/` overrides just that part of the case. Two mechanisms are
used here:

- `queries/odbc_sqlite/type/select/<case>.setup.sql` replaces only the setup
  DDL/DML. Used for `date`, `time`, and `timestamp0`–`timestamp9`: SQLite has
  no typed datetime literals (`DATE 'x'`, `TIMESTAMP 'x'`), so the same values
  are inserted as plain string literals into a column whose *declared* type is
  unchanged — which is what SQLiteODBC reports through `SQLDescribeCol`.
- `queries/odbc_sqlite/type/**/<case>.toml` with `skip = "reason"` marks a case
  unsupported. Used for the `timestampNtz` and `timestamptz_*` cases: SQLite
  has no `TIMESTAMP WITH TIME ZONE` type.

Use `hide = true` instead of `skip` to drop a case from the generated
documentation entirely.

## Note on statement splitting

`OdbcSqliteQuirks.split_statement` deliberately uses neither of the two
splitters the suite offers:

- `quirks.split_statement(sql, dialect="sqlite")` runs the setup SQL through
  sqlglot's *transpiler*, which rewrites `SMALLINT` to `INTEGER`,
  `DOUBLE PRECISION` to `REAL`, and `TIMESTAMP 'x'` to `CAST('x' AS TIMESTAMP)`.
  That silently changes which SQL type each type test exercises and produces
  results that look like driver bugs but are not.
- `quirks.split_statement(sql)` (the line-based fallback) only breaks on lines
  that *end* with `;`, so a statement with a trailing `-- comment` is glued to
  the next one, and SQLiteODBC rejects multiple statements per `SQLExecDirect`.

`_split_sql` in `quirks.py` splits on top-level `;` while honouring quoting and
dropping comments, leaving the statement text itself untouched.
