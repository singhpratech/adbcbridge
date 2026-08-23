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

# Validation results

## Run provenance

| | |
|---|---|
| Suite | `adbc-drivers/validation` @ `246e7f43f7276f3563c13f7438961e414e3c8422` (2026-08-21) |
| Driver | adbcbridge @ `8c5a47d`, Debug build, `build/libadbc_driver_odbc.so` |
| Backend | SQLite 3 via SQLiteODBC (`libsqlite3odbc.so`), file-backed database |
| Python | 3.13.12 · `adbc-driver-manager` 1.12.0 · `pyarrow` 25.0.1 · `pytest` 9.1.1 |
| Command | see [README.md](README.md) |

## Summary

| Status | Count |
|---|---|
| PASS | 192 |
| FAIL | 88 |
| SKIP | 46 |
| XFAIL | 1 |
| **Total** | **327** |

Of the 88 failures, **52 are driver defects** (findings `D1`–`D13`, fixable in
`src/`) and **36 are backend limitations** (findings `V1`–`V8`) that no change
to adbcbridge can address — they are properties of SQLite and of the SQLite
ODBC driver, and on a foundry submission they would be declared as query
overrides rather than fixed.

Per-finding counts:

| Finding | Kind | Tests | One-line summary |
|---|---|---:|---|
| [D1](#d1) | driver | 1 | `ADBC_INFO_DRIVER_NAME` is not a stable name |
| [D2](#d2) | driver | 1 | `ADBC_INFO_DRIVER_ARROW_VERSION` is not a version string |
| [D3](#d3) | driver | 1 | `GetObjects(depth=CATALOGS)` returns zero rows |
| [D4](#d4) | driver | 3 | `GetObjects` ignores `catalog`/`db_schema` filters |
| [D5](#d5) | driver | 1 | FK constraint usage reports `""` instead of NULL |
| [D6](#d6) | driver | 1 | Non-FK constraints report `[]` instead of NULL usage |
| [D7](#d7) | driver | 1 | Ingest over an existing table returns `UNKNOWN`, not `ALREADY_EXISTS` |
| [D8](#d8) | driver | 1 | Arrow `na` cannot be bound as a parameter |
| [D9](#d9) | driver | 1 | Multi-row bind is rejected for result-returning queries |
| [D10](#d10) | driver | 8 | Arrow `time32`/`time64` cannot be bound or ingested |
| [D11](#d11) | driver | 6 | View and dictionary Arrow types cannot be bound or ingested |
| [D12](#d12) | driver | 3 | `TIME` columns always read back as `time32[s]` |
| [D13](#d13) | driver | 24 | `TIMESTAMP` columns only ever map to `timestamp[us]`/`[ns]` |
| [D14](#d14) | driver | 1 (XFAIL) | `StatementGetParameterSchema` is not implemented |
| [V1](#v1) | backend | 4 | SQLite rejects typed literals / `from_hex` |
| [V2](#v2) | backend | 4 | SQLite `CAST` collapses to five storage classes |
| [V3](#v3) | backend | 5 | SQLiteODBC reports `DECIMAL` columns as `SQL_DOUBLE` |
| [V4](#v4) | backend | 7 | SQLite `REAL` is always 64-bit |
| [V5](#v5) | backend | 4 | Ingested decimals land in a `TEXT` column |
| [V6](#v6) | backend | 6 | Ingest normalizes every timestamp precision to microseconds |
| [V7](#v7) | backend | 1 | Ingest cannot preserve a timestamp timezone |
| [V8](#v8) | backend | 5 | SQLiteODBC loses precision on extreme doubles and sub-ms times |

The full per-test table is in the [appendix](#appendix-every-test).

---

## Driver defects

These are fixable in adbcbridge. Per the work item, no `src/` file was edited;
each entry names the file and function to change.

### D1

**`test_connection::test_get_info` — FAIL**

`ADBC_INFO_DRIVER_NAME` is reported as `ADBC ODBC Driver (sqlite3odbc.so)`. The
suite requires a stable driver identity; embedding the backing ODBC driver's
filename means the value changes per connection, so no quirks file can declare
it and downstream tooling cannot key off it.

*Fix:* `src/odbc_driver.c`, `OdbcConnectionGetInfo` — emit
`ADBC_ODBC_DRIVER_NAME` verbatim for `ADBC_INFO_DRIVER_NAME`. The underlying
`SQL_DRIVER_NAME` is genuinely useful for diagnostics, so expose it separately:
either as `ADBC_INFO_VENDOR_NAME`'s companion under a driver-specific info code
(≥ `ADBC_INFO_VENDOR_*` custom range) or as a read-only connection option
`adbc.odbc.driver_name` in `OdbcConnectionGetOption`.

### D2

**`test_connection::test_get_info_arrow_version` — FAIL**

`ADBC_INFO_DRIVER_ARROW_VERSION` is `nanoarrow 0.9.0`. The suite asserts the
value starts with `v`, i.e. that it is a bare version string.

*Fix:* `src/odbc_driver.c`, `OdbcConnectionGetInfo` — change the
`ADBC_INFO_DRIVER_ARROW_VERSION` case to emit `"v" NANOARROW_VERSION`.

### D3

**`test_connection::test_get_objects_catalog` — FAIL**

`GetObjects(depth=ADBC_OBJECT_DEPTH_CATALOGS)` returns an empty stream. Every
ADBC connection is in *some* catalog, even when the name is NULL, so the
catalog list must be non-empty and must contain the current catalog.

*Fix:* `src/odbc_objects.c`, `CollectTables` / `OdbcConnectionGetObjects` — at
`CATALOGS` depth, enumerate catalogs with
`SQLTables(hstmt, (SQLCHAR*)SQL_ALL_CATALOGS, SQL_NTS, "", 0, "", 0, "", 0)`,
and when the backend reports none (SQLite reports no catalogs), still emit a
single row with a NULL `catalog_name` so the current catalog is represented.
The same applies at `DB_SCHEMAS` depth via `SQL_ALL_SCHEMAS`.

### D4

**`test_connection::test_get_objects_schema` — FAIL**
**`test_connection::test_get_objects_table_invalid_catalog` — FAIL**
**`test_connection::test_get_objects_table_invalid_schema` — FAIL**

`GetObjects` accepts `catalog` and `db_schema` filter arguments but never
applies them: filtering on `catalog_filter="thiscatalogdoesnotexist"` still
returns the local `(NULL, NULL)` schema and all of its tables.

*Fix:* `src/odbc_objects.c`, `CollectTables` — the `catalog` and `schema`
parameters are threaded into the struct but not into the `SQLTables` call and
not post-filtered. Pass them as the catalog/schema pattern arguments to
`SQLTables`, and additionally drop any returned row whose `TABLE_CAT` /
`TABLE_SCHEM` does not match the requested pattern (ODBC drivers are permitted
to ignore patterns they do not support, so the post-filter is required for
correctness). Note the ADBC contract: these filters are SQL `LIKE` patterns,
and a NULL filter means "no filtering", which is distinct from an empty-string
filter meaning "the unnamed catalog/schema".

### D5

**`test_connection::test_get_objects_constraints_foreign` — FAIL**

For a foreign key, `constraint_column_usage` reports
`{'fk_catalog': '', 'fk_db_schema': '', ...}` where the spec (and the suite)
expects NULL for a backend with no catalogs or schemas.

*Fix:* `src/odbc_objects.c`, `AppendConstraints` — the `SQLForeignKeys`
`PKTABLE_CAT` (column 1) and `PKTABLE_SCHEM` (column 2) values are appended
with the plain string appender. Route them through the existing
`AppendStrOrNull` helper and treat a zero-length string as NULL, matching how
`TABLE_CAT`/`TABLE_SCHEM` are already handled in `CollectTables`.

### D6

**`test_connection::test_get_objects_constraints_primary` — FAIL**

For a `PRIMARY KEY` constraint, `constraint_column_usage` is an empty list. The
ADBC schema makes this field nullable specifically so that non-foreign-key
constraints can report NULL; an empty list means "a foreign key that references
nothing", which is not a thing.

*Fix:* `src/odbc_objects.c`, `AppendConstraints` — in the `SQLPrimaryKeys`
branch (and any future check/unique branch), append a null to the
`constraint_column_usage` list child via `ArrowArrayAppendNull` instead of
finishing an empty list element.

### D7

**`test_ingest::test_createappend_schema_mismatch[ingest/string]` — FAIL**

Bulk ingest with `mode=create` against a table that already exists surfaces
`ADBC_STATUS_UNKNOWN`; ADBC requires `ADBC_STATUS_ALREADY_EXISTS` so callers
can implement create-or-append themselves.

*Fix:* two places.
1. `src/odbc_reader.c`, `SqlStateToStatus` — map SQLSTATE `42S01`
   ("base table or view already exists") to `ADBC_STATUS_ALREADY_EXISTS`.
2. `src/odbc_bind.c`, `OdbcStatementIngest` — the `CREATE TABLE` is issued
   through `ExecSimple`, which collapses failures; propagate the status from
   `OdbcSetError` rather than returning a generic error, so the mapped status
   reaches the caller. SQLiteODBC returns SQLSTATE `HY000` with the message
   `table X already exists`, so a message-based fallback is also needed for
   backends that do not set `42S01`.

### D8

**`test_statement::test_parameter_null_typed` — FAIL**

Binding a parameter column of Arrow type `na` (all-null, untyped) fails with
`NOT_IMPLEMENTED: Unsupported Arrow type for parameter binding: na`. This is
the natural encoding for `INSERT INTO t VALUES (?, NULL)` from a DBAPI driver.

*Fix:* `src/odbc_bind.c`, `SlotFromArrow` — add a `NANOARROW_TYPE_NA` case that
sets `p->indicator = SQL_NULL_DATA`, `p->c_type = SQL_C_DEFAULT`,
`p->sql_type = SQL_VARCHAR`, `p->column_size = 1`, `p->buffer_length = 0`.

### D9

**`test_statement::test_parameter_execute` — FAIL**

Binding a 4-row parameter batch to `SELECT 1 + ?` fails with
`NOT_IMPLEMENTED: Cannot bind more than one row to a query that returns a
result set`. ADBC requires executing the statement once per parameter row and
exposing the concatenation of the result sets as a single stream.

*Fix:* `src/odbc_bind.c`, `ExecuteRows` / `OdbcStatementExecuteBound` — the
current code returns early when `out != NULL` and the batch has more than one
row. Replace that with a stream wrapper that drives the per-row loop lazily:
execute row *i*, hand its result batches to the consumer, then `SQLMoreResults`
/ re-execute for row *i+1*. All executions must produce a compatible schema
(check with `OdbcDescribeResultSchema` on the first execution and error with
`ADBC_STATUS_INVALID_STATE` on mismatch).

### D10

**`test_query::test_query[type/bind/time_s | time_ms | time_us | time_ns]` — FAIL**
**`test_ingest::test_create[ingest/time_s | time_ms | time_us | time_ns]` — FAIL**

Arrow `time32`/`time64` values cannot be bound as parameters or bulk-ingested:
`NOT_IMPLEMENTED: Unsupported Arrow type for ingest: time32` (and `time64`).
ODBC has had `SQL_C_TYPE_TIME` since 3.0, so this is a pure gap.

*Fix:* two places.
1. `src/odbc_bind.c`, `SlotFromArrow` — add `NANOARROW_TYPE_TIME32` and
   `NANOARROW_TYPE_TIME64` cases. Convert to `TIME_STRUCT` with
   `c_type = SQL_C_TYPE_TIME`, `sql_type = SQL_TYPE_TIME`. `TIME_STRUCT` has
   no fractional field, so for sub-second units either round-trip through
   `SQL_C_CHAR` with an `HH:MM:SS.ffffff` string (`sql_type = SQL_TYPE_TIME`,
   `decimal_digits` set from the Arrow unit) or accept truncation and document
   it. The string form is preferable and matches the existing decimal handling.
2. `src/odbc_bind.c`, `ColumnTypeSql` — map the same two Arrow types to the
   backend's `SQL_TYPE_TIME` name via the existing `TypeNameFor` lookup so
   `CREATE TABLE` during ingest emits a `TIME` column.

### D11

**`test_query::test_query[type/bind/string_view | binary_view]` — FAIL**
**`test_query::test_query_bind_dictionary[type/bind/string | large_string]` — FAIL**
**`test_ingest::test_create[ingest/string_view | binary_view]` — FAIL**

Arrow view layouts and dictionary-encoded arrays are rejected outright.
Dictionary encoding in particular is what a pandas categorical column becomes,
so this is a common shape for real callers.

*Fix:* `src/odbc_bind.c`, `SlotFromArrow` —
1. Add `NANOARROW_TYPE_STRING_VIEW` / `NANOARROW_TYPE_BINARY_VIEW` to the
   existing string/binary cases: `ArrowArrayViewGetStringUnsafe` and
   `ArrowArrayViewGetBytesUnsafe` already handle the view layouts in
   nanoarrow, so the cases can share the same bodies.
2. For `NANOARROW_TYPE_DICTIONARY`, resolve the index at `row` via
   `ArrowArrayViewGetIntUnsafe`, then recurse into `av->dictionary` with that
   index. The dictionary's `ArrowSchemaView` must be parsed once per bind
   rather than per row.

`src/odbc_bind.c`, `ColumnTypeSql` needs the same three types added so ingest
can pick a column type.

### D12

**`test_query::test_query[type/select/time]` — FAIL**
**`test_query::test_execute_schema[type/select/time]` — FAIL**
**`test_query::test_get_table_schema[type/select/time]` — FAIL**

A `TIME` column is always mapped to `time32[s]`; the suite expects `time64[us]`
and the stored fractional seconds are dropped.

*Fix:* `src/odbc_reader.c`, `ClassifyColumn` — the `SQL_TYPE_TIME` branch
unconditionally selects `SQL_C_TYPE_TIME` / `TIME_STRUCT`, which has
second resolution by construction. When `c->decimal_digits > 0`, fetch the
column as `SQL_C_CHAR` and parse `HH:MM:SS.fff...` (the driver already does
string-based parsing for decimals in `AppendDecimalString`), selecting
`time64[us]` for 1–6 digits and `time64[ns]` for 7–9. `BuildSchema` and
`AppendValue` need matching arms for the new fetch kind.

*Caveat:* SQLiteODBC reports `decimal_digits = 0` for `TIME` columns, so on
this backend the fix changes nothing observable — see [V2](#v2). It matters for
SQL Server, Oracle, and DB2, which report the declared precision.

### D13

**`test_query::{test_query,test_execute_schema,test_get_table_schema}[type/select/timestamp0..timestamp3, timestamp7..timestamp9]` — FAIL**
**`test_query::test_query[type/bind/timestamp_s | timestamp_ms | timestamp_ns]` — FAIL**

Every `TIMESTAMP` column becomes `timestamp[us]`, or `timestamp[ns]` when the
reported scale exceeds 6. Second- and millisecond-precision columns are never
produced, so a `TIMESTAMP(0)` or `TIMESTAMP(3)` column is silently widened.

*Fix:* `src/odbc_reader.c`, `ClassifyColumn` — the `SQL_TYPE_TIMESTAMP` branch
currently reads

```c
c->unit = c->decimal_digits > 6 ? NANOARROW_TIME_UNIT_NANO : NANOARROW_TIME_UNIT_MICRO;
```

Select all four units from the reported scale instead: `0` → `SECOND`, `1..3` →
`MILLI`, `4..6` → `MICRO`, `7..9` → `NANO`. `AppendValue` already computes from
a `TIMESTAMP_STRUCT`, so it needs the two extra divisors; `BuildSchema` already
passes `c->unit` through.

*Caveat:* SQLiteODBC reports `decimal_digits = 0` for *every* `TIMESTAMP`
column regardless of the declared precision (verified with `SQLDescribeCol` —
a `TIMESTAMP(3)` and a `TIMESTAMP(9)` column both report scale 0). With this
fix all of these cases would map to `timestamp[s]`, so `timestamp0` would start
passing and the rest would still fail, for a backend reason. The fix is
correct and necessary for honest ODBC backends; on SQLite the remaining
mismatch should be declared with query overrides.

### D14

**`test_statement::test_parameter_schema` — XFAIL (expected failure, correctly reported)**

`StatementGetParameterSchema` is not in the driver vtable, so
`quirks.py` declares `statement_get_parameter_schema=False` and the suite
xfails the test. Listed here only so the gap is visible: implementing it means
adding `driver->StatementGetParameterSchema` in `src/odbc_driver.c`
`AdbcDriverInit` backed by `SQLNumParams` + `SQLDescribeParam`.

---

## Backend limitations

No adbcbridge change can fix these; they are properties of SQLite or of the
SQLite ODBC driver. On a foundry submission each would be declared with a
`skip` or `hide` override under `queries/odbc_sqlite/`, exactly as the
`timestampNtz` cases already are.

### V1

**`test_query::test_query[type/literal/date | timestamp | timestamptz | binary]` — FAIL**

`[ODBC] SQLExecDirect failed … syntax error`. The cases use
`SELECT DATE '2023-05-15'`, `SELECT TIMESTAMP 'x'`,
`SELECT TIMESTAMP WITH TIME ZONE 'x'` and `SELECT from_hex('…')`. SQLite has no
typed literals and no `from_hex` function.

*Handling:* add `queries/odbc_sqlite/type/literal/<case>.toml` with
`skip = "SQLite has no typed datetime literals"` (and, for `binary`, a
`.sql` override using SQLite's native `X'…'` blob literal, which the driver
does read correctly — `type/select/binary` passes).

### V2

**`test_query::test_query[type/literal/int16 | int64 | boolean | time]` — FAIL**

`SELECT CAST(16384 AS SMALLINT)` yields `int32`, not `int16`;
`SELECT TRUE` yields `int32`, not `bool`; `CAST('13:45:31.123456' AS TIME)`
yields `int32`. SQLite's `CAST` only knows five storage classes (NULL, INTEGER,
REAL, TEXT, BLOB) and discards the requested SQL type, so `SQLDescribeCol` on
the *expression* has nothing to report but `SQL_INTEGER`. The equivalent
`type/select/*` cases, where the type is declared on a real column, all pass —
the driver's type mapping is correct.

*Handling:* `skip` overrides for the `type/literal` variants of these types.

### V3

**`test_query::{test_query,test_execute_schema,test_get_table_schema}[type/select/decimal]` — FAIL**
**`test_query::test_query[type/literal/decimal, type/bind/decimal]` — FAIL**

A `DECIMAL(10,2)` column reads back as `double` (and, when bound, as `string`).
SQLiteODBC reports `DECIMAL`/`NUMERIC` columns as `SQL_DOUBLE` or `SQL_VARCHAR`
depending on the declared width, never as `SQL_DECIMAL` with a usable scale.
The driver's `SQL_DECIMAL` handling is exercised and correct — it just never
sees that type code from this backend.

### V4

**`test_query::{test_query,test_execute_schema,test_get_table_schema}[type/select/float32]` — FAIL**
**`test_query::test_query[type/literal/float32, type/bind/float32, type/bind/float16]` — FAIL**
**`test_ingest::test_create[ingest/float32]` — FAIL**

`float` expected, `double` returned. SQLite has exactly one floating-point
storage class and it is IEEE-754 binary64; `REAL` is an alias, not a 32-bit
type, and SQLiteODBC reports `SQL_DOUBLE`.

### V5

**`test_ingest::test_create[ingest/decimal | decimal_scale_zero | decimal_scale_equals_precision | decimal_scale_negative]` — FAIL**

Ingesting an Arrow `decimal128` produces a column that reads back as `string`.
The driver writes the decimal as an exact `SQL_C_CHAR` string (correct — it
avoids the binary64 rounding that a numeric path would introduce), and
`ColumnTypeSql` asks the backend for a `DECIMAL` column type; SQLiteODBC's
`SQLGetTypeInfo` maps that to `VARCHAR`, so the round-trip is lossless in value
but not in type.

### V6

**`test_ingest::test_create[ingest/timestamp_s | timestamp_ms | timestamp_ns | timestamptz_s | timestamptz_ms | timestamptz_ns]` — FAIL**

Every ingested timestamp column reads back as `timestamp[us]`. This is
[D13](#d13) seen from the ingest side: the created column is a plain SQLite
`TIMESTAMP`, and SQLiteODBC reports scale 0 for it, so the reader cannot
recover the original Arrow unit. Fixing D13 alone will not fix this on SQLite.

### V7

**`test_ingest::test_create[ingest/timestamptz_us]` — FAIL**

`timestamp[us, tz=UTC]` in, `timestamp[us]` out. ODBC 3.x has no
timezone-aware SQL type code, so a driver can only preserve the timezone by
storing it out of band. The *values* round-trip correctly (Arrow tz-aware
timestamps are UTC epoch values and the driver binds them as such); only the
type annotation is lost.

*Possible mitigation, not a fix:* adbcbridge could remember the ingested
schema's timezone in a side table, or accept a statement option naming a
default output timezone. Both are out of scope of the ADBC spec, so the honest
answer here is a `skip`.

### V8

**`test_query::test_query[type/select/float64, type/bind/float64, type/bind/timestamp_us]` — FAIL**
**`test_ingest::test_create[ingest/float64, ingest/timestamp_us]` — FAIL**

Two distinct precision losses in the backend:

- `±1.7976931348623157e308` round-trips as `±inf` and
  `2.2250738585072014e-308` as `2.2250738585072e-308`. SQLiteODBC formats
  doubles through a fixed-width decimal string on the way in.
- `2023-05-15T13:45:30.123456` round-trips as `…30.123`. The driver binds a
  `TIMESTAMP_STRUCT` with `decimal_digits = 9` and a nanosecond `fraction`
  field (verified in `SlotFromArrow`), so the truncation to milliseconds
  happens inside SQLiteODBC.

---

## Appendix: every test

Statuses are as reported by pytest. `Finding` links each non-passing test to
the sections above.

<!-- BEGIN GENERATED TABLE -->
| Module | Test | Status | Finding | Detail |
|---|---|---|---|---|
| test_connection | `test_get_info` | FAIL | D1 | AssertionError: assert 'ADBC ODBC Dr...lite3odbc.so)' == 'ADBC ODBC Driver' - ADBC ODBC Driver + ADBC ODBC Driver (sqlite3odbc.so) |
| test_connection | `test_get_info_arrow_version` | FAIL | D2 | AssertionError: assert ('nanoarrow 0.9.0' and False) + where False = <built-in method startswith of str object at 0x7e2799f339f0>('v') + where <built- |
| test_connection | `test_get_objects_catalog` | FAIL | D3 | assert None in [] + where None = DriverFeatures(connection_get_table_schema=True, connection_get_statistics=False, connection_set_current_catalog=Fals |
| test_connection | `test_get_objects_constraints_foreign` | FAIL | D5 | AssertionError: assert {'constraint_..._name': 'a'}]} == {'constraint_..._name': 'a'}]} Omitting 2 identical items, use -vv to show Differing items: { |
| test_connection | `test_get_objects_constraints_primary` | FAIL | D6 | AssertionError: assert {'constraint_...mn_usage': []} == {'constraint_..._usage': None} Omitting 2 identical items, use -vv to show Differing items: { |
| test_connection | `test_get_objects_schema` | FAIL | D4 | assert [(None, None)] == [] Left contains one more item: (None, None) Use -v to get more diff |
| test_connection | `test_get_objects_table_invalid_catalog` | FAIL | D4 | AssertionError: assert (None, None, 'getobjects0d1aa8d564dc5287') not in [(None, None, 'constraint_check'), (None, None, 'constraint_foreign'), (None, |
| test_connection | `test_get_objects_table_invalid_schema` | FAIL | D4 | AssertionError: assert (None, None, 'getobjects0d1aa8d564dc5287') not in [(None, None, 'constraint_check'), (None, None, 'constraint_foreign'), (None, |
| test_ingest | `test_create[ingest/binary_view]` | FAIL | D11 | adbc_driver_manager.NotSupportedError: NOT_IMPLEMENTED: Unsupported Arrow type for ingest: binary_view |
| test_ingest | `test_create[ingest/decimal]` | FAIL | V5 | AssertionError: Field types do not match: expected value (decimal128(10, 2)) != actual value (string) assert Decimal128Type(decimal128(10, 2)) == Data |
| test_ingest | `test_create[ingest/decimal_scale_equals_precision]` | FAIL | V5 | AssertionError: Field types do not match: expected value (decimal128(2, 2)) != actual value (string) assert Decimal128Type(decimal128(2, 2)) == DataTy |
| test_ingest | `test_create[ingest/decimal_scale_negative]` | FAIL | V5 | AssertionError: Field types do not match: expected value (decimal128(2, -2)) != actual value (string) assert Decimal128Type(decimal128(2, -2)) == Data |
| test_ingest | `test_create[ingest/decimal_scale_zero]` | FAIL | V5 | AssertionError: Field types do not match: expected value (decimal128(10, 0)) != actual value (string) assert Decimal128Type(decimal128(10, 0)) == Data |
| test_ingest | `test_create[ingest/float32]` | FAIL | V4 | AssertionError: Field types do not match: expected value (float) != actual value (double) assert DataType(float) == DataType(double) + where DataType( |
| test_ingest | `test_create[ingest/float64]` | FAIL | V8 | AssertionError: Tables do not match! Diff: --- expected +++ actual @@ -3,8 +3,8 @@ {'idx': 0, 'value': 0.0} {'idx': 1, 'value': 3.14159265358979} {'id |
| test_ingest | `test_create[ingest/string_view]` | FAIL | D11 | adbc_driver_manager.NotSupportedError: NOT_IMPLEMENTED: Unsupported Arrow type for ingest: string_view |
| test_ingest | `test_create[ingest/time_ms]` | FAIL | D10 | adbc_driver_manager.NotSupportedError: NOT_IMPLEMENTED: Unsupported Arrow type for ingest: time32 |
| test_ingest | `test_create[ingest/time_ns]` | FAIL | D10 | adbc_driver_manager.NotSupportedError: NOT_IMPLEMENTED: Unsupported Arrow type for ingest: time64 |
| test_ingest | `test_create[ingest/time_s]` | FAIL | D10 | adbc_driver_manager.NotSupportedError: NOT_IMPLEMENTED: Unsupported Arrow type for ingest: time32 |
| test_ingest | `test_create[ingest/time_us]` | FAIL | D10 | adbc_driver_manager.NotSupportedError: NOT_IMPLEMENTED: Unsupported Arrow type for ingest: time64 |
| test_ingest | `test_create[ingest/timestamp_ms]` | FAIL | V6 | AssertionError: Field types do not match: expected value (timestamp[ms]) != actual value (timestamp[us]) assert TimestampType(timestamp[ms]) == Timest |
| test_ingest | `test_create[ingest/timestamp_ns]` | FAIL | V6 | AssertionError: Field types do not match: expected value (timestamp[ns]) != actual value (timestamp[us]) assert TimestampType(timestamp[ns]) == Timest |
| test_ingest | `test_create[ingest/timestamp_s]` | FAIL | V6 | AssertionError: Field types do not match: expected value (timestamp[s]) != actual value (timestamp[us]) assert TimestampType(timestamp[s]) == Timestam |
| test_ingest | `test_create[ingest/timestamp_us]` | FAIL | V8 | AssertionError: Tables do not match! Diff: --- expected +++ actual @@ -2,7 +2,7 @@ value: timestamp[us] {'idx': 0, 'value': None} {'idx': 1, 'value':  |
| test_ingest | `test_create[ingest/timestamptz_ms]` | FAIL | V6 | AssertionError: Field types do not match: expected value (timestamp[ms, tz=UTC]) != actual value (timestamp[us]) assert TimestampType(timestamp[ms, tz |
| test_ingest | `test_create[ingest/timestamptz_ns]` | FAIL | V6 | AssertionError: Field types do not match: expected value (timestamp[ns, tz=UTC]) != actual value (timestamp[us]) assert TimestampType(timestamp[ns, tz |
| test_ingest | `test_create[ingest/timestamptz_s]` | FAIL | V6 | AssertionError: Field types do not match: expected value (timestamp[s, tz=UTC]) != actual value (timestamp[us]) assert TimestampType(timestamp[s, tz=U |
| test_ingest | `test_create[ingest/timestamptz_us]` | FAIL | V7 | AssertionError: Field types do not match: expected value (timestamp[us, tz=UTC]) != actual value (timestamp[us]) assert TimestampType(timestamp[us, tz |
| test_ingest | `test_createappend_schema_mismatch[ingest/string]` | FAIL | D7 | AssertionError: assert <AdbcStatusCode.UNKNOWN: 1> == <AdbcStatusCode.ALREADY_EXISTS: 4> + where <AdbcStatusCode.UNKNOWN: 1> = OperationalError('UNKNO |
| test_query | `test_execute_schema[type/select/decimal]` | FAIL | V3 | AssertionError: Field types do not match: expected res (decimal128(10, 2)) != actual res (double) assert Decimal128Type(decimal128(10, 2)) == DataType |
| test_query | `test_execute_schema[type/select/float32]` | FAIL | V4 | AssertionError: Field types do not match: expected res (float) != actual res (double) assert DataType(float) == DataType(double) + where DataType(floa |
| test_query | `test_execute_schema[type/select/time]` | FAIL | D12 | AssertionError: Field types do not match: expected res (time64[us]) != actual res (time32[s]) assert Time64Type(time64[us]) == Time32Type(time32[s]) + |
| test_query | `test_execute_schema[type/select/timestamp0]` | FAIL | D13 | AssertionError: Field types do not match: expected res (timestamp[s]) != actual res (timestamp[us]) assert TimestampType(timestamp[s]) == TimestampTyp |
| test_query | `test_execute_schema[type/select/timestamp1]` | FAIL | D13 | AssertionError: Field types do not match: expected res (timestamp[ms]) != actual res (timestamp[us]) assert TimestampType(timestamp[ms]) == TimestampT |
| test_query | `test_execute_schema[type/select/timestamp2]` | FAIL | D13 | AssertionError: Field types do not match: expected res (timestamp[ms]) != actual res (timestamp[us]) assert TimestampType(timestamp[ms]) == TimestampT |
| test_query | `test_execute_schema[type/select/timestamp3]` | FAIL | D13 | AssertionError: Field types do not match: expected res (timestamp[ms]) != actual res (timestamp[us]) assert TimestampType(timestamp[ms]) == TimestampT |
| test_query | `test_execute_schema[type/select/timestamp7]` | FAIL | D13 | AssertionError: Field types do not match: expected res (timestamp[ns]) != actual res (timestamp[us]) assert TimestampType(timestamp[ns]) == TimestampT |
| test_query | `test_execute_schema[type/select/timestamp8]` | FAIL | D13 | AssertionError: Field types do not match: expected res (timestamp[ns]) != actual res (timestamp[us]) assert TimestampType(timestamp[ns]) == TimestampT |
| test_query | `test_execute_schema[type/select/timestamp9]` | FAIL | D13 | AssertionError: Field types do not match: expected res (timestamp[ns]) != actual res (timestamp[us]) assert TimestampType(timestamp[ns]) == TimestampT |
| test_query | `test_get_table_schema[type/select/decimal]` | FAIL | V3 | AssertionError: Field types do not match: expected res (decimal128(10, 2)) != actual res (double) assert Decimal128Type(decimal128(10, 2)) == DataType |
| test_query | `test_get_table_schema[type/select/float32]` | FAIL | V4 | AssertionError: Field types do not match: expected res (float) != actual res (double) assert DataType(float) == DataType(double) + where DataType(floa |
| test_query | `test_get_table_schema[type/select/time]` | FAIL | D12 | AssertionError: Field types do not match: expected res (time64[us]) != actual res (time32[s]) assert Time64Type(time64[us]) == Time32Type(time32[s]) + |
| test_query | `test_get_table_schema[type/select/timestamp0]` | FAIL | D13 | AssertionError: Field types do not match: expected res (timestamp[s]) != actual res (timestamp[us]) assert TimestampType(timestamp[s]) == TimestampTyp |
| test_query | `test_get_table_schema[type/select/timestamp1]` | FAIL | D13 | AssertionError: Field types do not match: expected res (timestamp[ms]) != actual res (timestamp[us]) assert TimestampType(timestamp[ms]) == TimestampT |
| test_query | `test_get_table_schema[type/select/timestamp2]` | FAIL | D13 | AssertionError: Field types do not match: expected res (timestamp[ms]) != actual res (timestamp[us]) assert TimestampType(timestamp[ms]) == TimestampT |
| test_query | `test_get_table_schema[type/select/timestamp3]` | FAIL | D13 | AssertionError: Field types do not match: expected res (timestamp[ms]) != actual res (timestamp[us]) assert TimestampType(timestamp[ms]) == TimestampT |
| test_query | `test_get_table_schema[type/select/timestamp7]` | FAIL | D13 | AssertionError: Field types do not match: expected res (timestamp[ns]) != actual res (timestamp[us]) assert TimestampType(timestamp[ns]) == TimestampT |
| test_query | `test_get_table_schema[type/select/timestamp8]` | FAIL | D13 | AssertionError: Field types do not match: expected res (timestamp[ns]) != actual res (timestamp[us]) assert TimestampType(timestamp[ns]) == TimestampT |
| test_query | `test_get_table_schema[type/select/timestamp9]` | FAIL | D13 | AssertionError: Field types do not match: expected res (timestamp[ns]) != actual res (timestamp[us]) assert TimestampType(timestamp[ns]) == TimestampT |
| test_query | `test_query[type/bind/binary_view]` | FAIL | D11 | adbc_driver_manager.NotSupportedError: NOT_IMPLEMENTED: Unsupported Arrow type for parameter binding: binary_view |
| test_query | `test_query[type/bind/decimal]` | FAIL | V3 | AssertionError: Field types do not match: expected res (decimal128(10, 2)) != actual res (string) assert Decimal128Type(decimal128(10, 2)) == DataType |
| test_query | `test_query[type/bind/float16]` | FAIL | V4 | AssertionError: Field types do not match: expected res (float) != actual res (double) assert DataType(float) == DataType(double) + where DataType(floa |
| test_query | `test_query[type/bind/float32]` | FAIL | V4 | AssertionError: Field types do not match: expected res (float) != actual res (double) assert DataType(float) == DataType(double) + where DataType(floa |
| test_query | `test_query[type/bind/float64]` | FAIL | V8 | AssertionError: Tables do not match! Diff: --- expected +++ actual @@ -1,7 +1,7 @@ res: double -{'res': -1.7976931348623157e+308} +{'res': -inf} {'res |
| test_query | `test_query[type/bind/string_view]` | FAIL | D11 | adbc_driver_manager.NotSupportedError: NOT_IMPLEMENTED: Unsupported Arrow type for parameter binding: string_view |
| test_query | `test_query[type/bind/time_ms]` | FAIL | D10 | adbc_driver_manager.NotSupportedError: NOT_IMPLEMENTED: Unsupported Arrow type for parameter binding: time32 |
| test_query | `test_query[type/bind/time_ns]` | FAIL | D10 | adbc_driver_manager.NotSupportedError: NOT_IMPLEMENTED: Unsupported Arrow type for parameter binding: time64 |
| test_query | `test_query[type/bind/time_s]` | FAIL | D10 | adbc_driver_manager.NotSupportedError: NOT_IMPLEMENTED: Unsupported Arrow type for parameter binding: time32 |
| test_query | `test_query[type/bind/time_us]` | FAIL | D10 | adbc_driver_manager.NotSupportedError: NOT_IMPLEMENTED: Unsupported Arrow type for parameter binding: time64 |
| test_query | `test_query[type/bind/timestamp_ms]` | FAIL | D13 | AssertionError: Field types do not match: expected res (timestamp[ms]) != actual res (timestamp[us]) assert TimestampType(timestamp[ms]) == TimestampT |
| test_query | `test_query[type/bind/timestamp_ns]` | FAIL | D13 | AssertionError: Field types do not match: expected res (timestamp[ns]) != actual res (timestamp[us]) assert TimestampType(timestamp[ns]) == TimestampT |
| test_query | `test_query[type/bind/timestamp_s]` | FAIL | D13 | AssertionError: Field types do not match: expected res (timestamp[s]) != actual res (timestamp[us]) assert TimestampType(timestamp[s]) == TimestampTyp |
| test_query | `test_query[type/bind/timestamp_us]` | FAIL | V8 | AssertionError: Tables do not match! Diff: --- expected +++ actual @@ -1,6 +1,6 @@ res: timestamp[us] {'res': None} {'res': '0001-01-01T00:00:00'} -{' |
| test_query | `test_query[type/literal/binary]` | FAIL | V1 | adbc_driver_manager.OperationalError: UNKNOWN: [ODBC] SQLExecDirect failed [HY000] (1) [SQLite]no such function: from_hex (1). Vendor code: 1. SQLSTAT |
| test_query | `test_query[type/literal/boolean]` | FAIL | V2 | AssertionError: Field types do not match: expected res (bool) != actual res (int32) assert DataType(bool) == DataType(int32) + where DataType(bool) =  |
| test_query | `test_query[type/literal/date]` | FAIL | V1 | adbc_driver_manager.OperationalError: UNKNOWN: [ODBC] SQLExecDirect failed [HY000] (1) [SQLite]near "AS": syntax error (1). Vendor code: 1. SQLSTATE:  |
| test_query | `test_query[type/literal/decimal]` | FAIL | V3 | AssertionError: Field types do not match: expected res (decimal128(10, 2)) != actual res (double) assert Decimal128Type(decimal128(10, 2)) == DataType |
| test_query | `test_query[type/literal/float32]` | FAIL | V4 | AssertionError: Field types do not match: expected res (float) != actual res (double) assert DataType(float) == DataType(double) + where DataType(floa |
| test_query | `test_query[type/literal/int16]` | FAIL | V2 | AssertionError: Field types do not match: expected res (int16) != actual res (int32) assert DataType(int16) == DataType(int32) + where DataType(int16) |
| test_query | `test_query[type/literal/int64]` | FAIL | V2 | AssertionError: Field types do not match: expected res (int64) != actual res (int32) assert DataType(int64) == DataType(int32) + where DataType(int64) |
| test_query | `test_query[type/literal/time]` | FAIL | V2 | AssertionError: Field types do not match: expected res (time64[us]) != actual res (int32) assert Time64Type(time64[us]) == DataType(int32) + where Tim |
| test_query | `test_query[type/literal/timestamp]` | FAIL | V1 | adbc_driver_manager.OperationalError: UNKNOWN: [ODBC] SQLExecDirect failed [HY000] (1) [SQLite]near "AS": syntax error (1). Vendor code: 1. SQLSTATE:  |
| test_query | `test_query[type/literal/timestamptz]` | FAIL | V1 | adbc_driver_manager.OperationalError: UNKNOWN: [ODBC] SQLExecDirect failed [HY000] (1) [SQLite]near "TIME": syntax error (1). Vendor code: 1. SQLSTATE |
| test_query | `test_query[type/select/decimal]` | FAIL | V3 | AssertionError: Field types do not match: expected res (decimal128(10, 2)) != actual res (double) assert Decimal128Type(decimal128(10, 2)) == DataType |
| test_query | `test_query[type/select/float32]` | FAIL | V4 | AssertionError: Field types do not match: expected res (float) != actual res (double) assert DataType(float) == DataType(double) + where DataType(floa |
| test_query | `test_query[type/select/float64]` | FAIL | V8 | AssertionError: Tables do not match! Diff: --- expected +++ actual @@ -1,7 +1,7 @@ res: double {'res': 3.14159265358979} {'res': 0.0} -{'res': -1.7976 |
| test_query | `test_query[type/select/time]` | FAIL | D12 | AssertionError: Field types do not match: expected res (time64[us]) != actual res (time32[s]) assert Time64Type(time64[us]) == Time32Type(time32[s]) + |
| test_query | `test_query[type/select/timestamp0]` | FAIL | D13 | AssertionError: Field types do not match: expected res (timestamp[s]) != actual res (timestamp[us]) assert TimestampType(timestamp[s]) == TimestampTyp |
| test_query | `test_query[type/select/timestamp1]` | FAIL | D13 | AssertionError: Field types do not match: expected res (timestamp[ms]) != actual res (timestamp[us]) assert TimestampType(timestamp[ms]) == TimestampT |
| test_query | `test_query[type/select/timestamp2]` | FAIL | D13 | AssertionError: Field types do not match: expected res (timestamp[ms]) != actual res (timestamp[us]) assert TimestampType(timestamp[ms]) == TimestampT |
| test_query | `test_query[type/select/timestamp3]` | FAIL | D13 | AssertionError: Field types do not match: expected res (timestamp[ms]) != actual res (timestamp[us]) assert TimestampType(timestamp[ms]) == TimestampT |
| test_query | `test_query[type/select/timestamp7]` | FAIL | D13 | AssertionError: Field types do not match: expected res (timestamp[ns]) != actual res (timestamp[us]) assert TimestampType(timestamp[ns]) == TimestampT |
| test_query | `test_query[type/select/timestamp8]` | FAIL | D13 | AssertionError: Field types do not match: expected res (timestamp[ns]) != actual res (timestamp[us]) assert TimestampType(timestamp[ns]) == TimestampT |
| test_query | `test_query[type/select/timestamp9]` | FAIL | D13 | AssertionError: Field types do not match: expected res (timestamp[ns]) != actual res (timestamp[us]) assert TimestampType(timestamp[ns]) == TimestampT |
| test_query | `test_query_bind_dictionary[type/bind/large_string]` | FAIL | D11 | adbc_driver_manager.NotSupportedError: NOT_IMPLEMENTED: Unsupported Arrow type for parameter binding: dictionary |
| test_query | `test_query_bind_dictionary[type/bind/string]` | FAIL | D11 | adbc_driver_manager.NotSupportedError: NOT_IMPLEMENTED: Unsupported Arrow type for parameter binding: dictionary |
| test_statement | `test_parameter_execute` | FAIL | D9 | adbc_driver_manager.NotSupportedError: NOT_IMPLEMENTED: Cannot bind more than one row to a query that returns a result set |
| test_statement | `test_parameter_null_typed` | FAIL | D8 | adbc_driver_manager.NotSupportedError: NOT_IMPLEMENTED: Unsupported Arrow type for parameter binding: na |
| test_statement | `test_parameter_schema` | XFAIL | D14 | StatementGetParameterSchema is not implemented |
| test_connection | `test_get_objects_constraints_check` | SKIP | S2 | not implemented |
| test_connection | `test_get_objects_constraints_unique` | SKIP | S2 | not implemented |
| test_connection | `test_get_statistics` | SKIP | S3 | connection_get_statistics not supported |
| test_connection | `test_get_table_schema_catalog` | SKIP | S4 | secondary_catalog not supported |
| test_connection | `test_get_table_schema_schema` | SKIP | S4 | secondary_schema not supported |
| test_connection | `test_set_current_catalog` | SKIP | S2 | not implemented |
| test_connection | `test_set_current_schema` | SKIP | S2 | not implemented |
| test_ingest | `test_catalog` | SKIP | S2 | not implemented |
| test_ingest | `test_not_null` | SKIP | S2 | not implemented |
| test_ingest | `test_replace_catalog[ingest/string]` | SKIP | S2 | not implemented |
| test_ingest | `test_replace_schema[ingest/string]` | SKIP | S2 | not implemented |
| test_ingest | `test_schema` | SKIP | S2 | not implemented |
| test_query | `test_execute_schema[type/select/timestamp0tz]` | SKIP | S1 | SQLite has no TIMESTAMP WITH TIME ZONE type, and ODBC 3.x has no timezone-aware SQL type code for the driver to map from |
| test_query | `test_execute_schema[type/select/timestamp1tz]` | SKIP | S1 | SQLite has no TIMESTAMP WITH TIME ZONE type, and ODBC 3.x has no timezone-aware SQL type code for the driver to map from |
| test_query | `test_execute_schema[type/select/timestamp2tz]` | SKIP | S1 | SQLite has no TIMESTAMP WITH TIME ZONE type, and ODBC 3.x has no timezone-aware SQL type code for the driver to map from |
| test_query | `test_execute_schema[type/select/timestamp3tz]` | SKIP | S1 | SQLite has no TIMESTAMP WITH TIME ZONE type, and ODBC 3.x has no timezone-aware SQL type code for the driver to map from |
| test_query | `test_execute_schema[type/select/timestamp4tz]` | SKIP | S1 | SQLite has no TIMESTAMP WITH TIME ZONE type, and ODBC 3.x has no timezone-aware SQL type code for the driver to map from |
| test_query | `test_execute_schema[type/select/timestamp5tz]` | SKIP | S1 | SQLite has no TIMESTAMP WITH TIME ZONE type, and ODBC 3.x has no timezone-aware SQL type code for the driver to map from |
| test_query | `test_execute_schema[type/select/timestamp6tz]` | SKIP | S1 | SQLite has no TIMESTAMP WITH TIME ZONE type, and ODBC 3.x has no timezone-aware SQL type code for the driver to map from |
| test_query | `test_execute_schema[type/select/timestamp7tz]` | SKIP | S1 | SQLite has no TIMESTAMP WITH TIME ZONE type, and ODBC 3.x has no timezone-aware SQL type code for the driver to map from |
| test_query | `test_execute_schema[type/select/timestamp8tz]` | SKIP | S1 | SQLite has no TIMESTAMP WITH TIME ZONE type, and ODBC 3.x has no timezone-aware SQL type code for the driver to map from |
| test_query | `test_execute_schema[type/select/timestamp9tz]` | SKIP | S1 | SQLite has no TIMESTAMP WITH TIME ZONE type, and ODBC 3.x has no timezone-aware SQL type code for the driver to map from |
| test_query | `test_get_table_schema[type/select/timestamp0tz]` | SKIP | S1 | SQLite has no TIMESTAMP WITH TIME ZONE type, and ODBC 3.x has no timezone-aware SQL type code for the driver to map from |
| test_query | `test_get_table_schema[type/select/timestamp1tz]` | SKIP | S1 | SQLite has no TIMESTAMP WITH TIME ZONE type, and ODBC 3.x has no timezone-aware SQL type code for the driver to map from |
| test_query | `test_get_table_schema[type/select/timestamp2tz]` | SKIP | S1 | SQLite has no TIMESTAMP WITH TIME ZONE type, and ODBC 3.x has no timezone-aware SQL type code for the driver to map from |
| test_query | `test_get_table_schema[type/select/timestamp3tz]` | SKIP | S1 | SQLite has no TIMESTAMP WITH TIME ZONE type, and ODBC 3.x has no timezone-aware SQL type code for the driver to map from |
| test_query | `test_get_table_schema[type/select/timestamp4tz]` | SKIP | S1 | SQLite has no TIMESTAMP WITH TIME ZONE type, and ODBC 3.x has no timezone-aware SQL type code for the driver to map from |
| test_query | `test_get_table_schema[type/select/timestamp5tz]` | SKIP | S1 | SQLite has no TIMESTAMP WITH TIME ZONE type, and ODBC 3.x has no timezone-aware SQL type code for the driver to map from |
| test_query | `test_get_table_schema[type/select/timestamp6tz]` | SKIP | S1 | SQLite has no TIMESTAMP WITH TIME ZONE type, and ODBC 3.x has no timezone-aware SQL type code for the driver to map from |
| test_query | `test_get_table_schema[type/select/timestamp7tz]` | SKIP | S1 | SQLite has no TIMESTAMP WITH TIME ZONE type, and ODBC 3.x has no timezone-aware SQL type code for the driver to map from |
| test_query | `test_get_table_schema[type/select/timestamp8tz]` | SKIP | S1 | SQLite has no TIMESTAMP WITH TIME ZONE type, and ODBC 3.x has no timezone-aware SQL type code for the driver to map from |
| test_query | `test_get_table_schema[type/select/timestamp9tz]` | SKIP | S1 | SQLite has no TIMESTAMP WITH TIME ZONE type, and ODBC 3.x has no timezone-aware SQL type code for the driver to map from |
| test_query | `test_query[type/bind/timestamptz_ms]` | SKIP | S1 | SQLite has no TIMESTAMP WITH TIME ZONE type, and ODBC 3.x has no timezone-aware SQL type code for the driver to map from |
| test_query | `test_query[type/bind/timestamptz_ns]` | SKIP | S1 | SQLite has no TIMESTAMP WITH TIME ZONE type, and ODBC 3.x has no timezone-aware SQL type code for the driver to map from |
| test_query | `test_query[type/bind/timestamptz_s]` | SKIP | S1 | SQLite has no TIMESTAMP WITH TIME ZONE type, and ODBC 3.x has no timezone-aware SQL type code for the driver to map from |
| test_query | `test_query[type/bind/timestamptz_us]` | SKIP | S1 | SQLite has no TIMESTAMP WITH TIME ZONE type, and ODBC 3.x has no timezone-aware SQL type code for the driver to map from |
| test_query | `test_query[type/select/timestamp0tz]` | SKIP | S1 | SQLite has no TIMESTAMP WITH TIME ZONE type, and ODBC 3.x has no timezone-aware SQL type code for the driver to map from |
| test_query | `test_query[type/select/timestamp1tz]` | SKIP | S1 | SQLite has no TIMESTAMP WITH TIME ZONE type, and ODBC 3.x has no timezone-aware SQL type code for the driver to map from |
| test_query | `test_query[type/select/timestamp2tz]` | SKIP | S1 | SQLite has no TIMESTAMP WITH TIME ZONE type, and ODBC 3.x has no timezone-aware SQL type code for the driver to map from |
| test_query | `test_query[type/select/timestamp3tz]` | SKIP | S1 | SQLite has no TIMESTAMP WITH TIME ZONE type, and ODBC 3.x has no timezone-aware SQL type code for the driver to map from |
| test_query | `test_query[type/select/timestamp4tz]` | SKIP | S1 | SQLite has no TIMESTAMP WITH TIME ZONE type, and ODBC 3.x has no timezone-aware SQL type code for the driver to map from |
| test_query | `test_query[type/select/timestamp5tz]` | SKIP | S1 | SQLite has no TIMESTAMP WITH TIME ZONE type, and ODBC 3.x has no timezone-aware SQL type code for the driver to map from |
| test_query | `test_query[type/select/timestamp6tz]` | SKIP | S1 | SQLite has no TIMESTAMP WITH TIME ZONE type, and ODBC 3.x has no timezone-aware SQL type code for the driver to map from |
| test_query | `test_query[type/select/timestamp7tz]` | SKIP | S1 | SQLite has no TIMESTAMP WITH TIME ZONE type, and ODBC 3.x has no timezone-aware SQL type code for the driver to map from |
| test_query | `test_query[type/select/timestamp8tz]` | SKIP | S1 | SQLite has no TIMESTAMP WITH TIME ZONE type, and ODBC 3.x has no timezone-aware SQL type code for the driver to map from |
| test_query | `test_query[type/select/timestamp9tz]` | SKIP | S1 | SQLite has no TIMESTAMP WITH TIME ZONE type, and ODBC 3.x has no timezone-aware SQL type code for the driver to map from |
| test_connection | `test_current_catalog` | PASS |  |  |
| test_connection | `test_current_db_schema` | PASS |  |  |
| test_connection | `test_get_objects_column_filter_catalog` | PASS |  |  |
| test_connection | `test_get_objects_column_filter_column_name` | PASS |  |  |
| test_connection | `test_get_objects_column_filter_schema` | PASS |  |  |
| test_connection | `test_get_objects_column_filter_table` | PASS |  |  |
| test_connection | `test_get_objects_column_filter_table_name` | PASS |  |  |
| test_connection | `test_get_objects_column_not_exist` | PASS |  |  |
| test_connection | `test_get_objects_column_present` | PASS |  |  |
| test_connection | `test_get_objects_column_xdbc` | PASS |  |  |
| test_connection | `test_get_objects_table_exact_table` | PASS |  |  |
| test_connection | `test_get_objects_table_invalid_table` | PASS |  |  |
| test_connection | `test_get_objects_table_not_exist` | PASS |  |  |
| test_connection | `test_get_objects_table_present` | PASS |  |  |
| test_connection | `test_get_table_schema_not_found` | PASS |  |  |
| test_connection | `test_unknown_option` | PASS |  |  |
| test_ingest | `test_append[ingest/string]` | PASS |  |  |
| test_ingest | `test_append_fail[ingest/string]` | PASS |  |  |
| test_ingest | `test_create[ingest/binary]` | PASS |  |  |
| test_ingest | `test_create[ingest/boolean]` | PASS |  |  |
| test_ingest | `test_create[ingest/date]` | PASS |  |  |
| test_ingest | `test_create[ingest/fixed_size_binary]` | PASS |  |  |
| test_ingest | `test_create[ingest/int16]` | PASS |  |  |
| test_ingest | `test_create[ingest/int32]` | PASS |  |  |
| test_ingest | `test_create[ingest/int64]` | PASS |  |  |
| test_ingest | `test_create[ingest/large_binary]` | PASS |  |  |
| test_ingest | `test_create[ingest/large_string]` | PASS |  |  |
| test_ingest | `test_create[ingest/string]` | PASS |  |  |
| test_ingest | `test_create_conflict[ingest/string]` | PASS |  |  |
| test_ingest | `test_create_large_batch[ingest/string]` | PASS |  |  |
| test_ingest | `test_create_long_values[ingest/binary]` | PASS |  |  |
| test_ingest | `test_create_long_values[ingest/string]` | PASS |  |  |
| test_ingest | `test_create_multiple_batches[ingest/string]` | PASS |  |  |
| test_ingest | `test_createappend[ingest/string]` | PASS |  |  |
| test_ingest | `test_ingest_no_parameters` | PASS |  |  |
| test_ingest | `test_ingest_then_query[ingest/string]` | PASS |  |  |
| test_ingest | `test_many_columns` | PASS |  |  |
| test_ingest | `test_replace[ingest/string]` | PASS |  |  |
| test_ingest | `test_replace_noop[ingest/string]` | PASS |  |  |
| test_ingest | `test_temporary[ingest/string]` | PASS |  |  |
| test_query | `test_execute_schema[type/select/binary]` | PASS |  |  |
| test_query | `test_execute_schema[type/select/boolean]` | PASS |  |  |
| test_query | `test_execute_schema[type/select/date]` | PASS |  |  |
| test_query | `test_execute_schema[type/select/float64]` | PASS |  |  |
| test_query | `test_execute_schema[type/select/int16]` | PASS |  |  |
| test_query | `test_execute_schema[type/select/int32]` | PASS |  |  |
| test_query | `test_execute_schema[type/select/int64]` | PASS |  |  |
| test_query | `test_execute_schema[type/select/string]` | PASS |  |  |
| test_query | `test_execute_schema[type/select/timestamp4]` | PASS |  |  |
| test_query | `test_execute_schema[type/select/timestamp5]` | PASS |  |  |
| test_query | `test_execute_schema[type/select/timestamp6]` | PASS |  |  |
| test_query | `test_get_table_schema[type/select/binary]` | PASS |  |  |
| test_query | `test_get_table_schema[type/select/boolean]` | PASS |  |  |
| test_query | `test_get_table_schema[type/select/date]` | PASS |  |  |
| test_query | `test_get_table_schema[type/select/float64]` | PASS |  |  |
| test_query | `test_get_table_schema[type/select/int16]` | PASS |  |  |
| test_query | `test_get_table_schema[type/select/int32]` | PASS |  |  |
| test_query | `test_get_table_schema[type/select/int64]` | PASS |  |  |
| test_query | `test_get_table_schema[type/select/string]` | PASS |  |  |
| test_query | `test_get_table_schema[type/select/timestamp4]` | PASS |  |  |
| test_query | `test_get_table_schema[type/select/timestamp5]` | PASS |  |  |
| test_query | `test_get_table_schema[type/select/timestamp6]` | PASS |  |  |
| test_query | `test_lint_query[ingest/binary]` | PASS |  |  |
| test_query | `test_lint_query[ingest/binary_view]` | PASS |  |  |
| test_query | `test_lint_query[ingest/boolean]` | PASS |  |  |
| test_query | `test_lint_query[ingest/date]` | PASS |  |  |
| test_query | `test_lint_query[ingest/decimal]` | PASS |  |  |
| test_query | `test_lint_query[ingest/decimal_scale_equals_precision]` | PASS |  |  |
| test_query | `test_lint_query[ingest/decimal_scale_negative]` | PASS |  |  |
| test_query | `test_lint_query[ingest/decimal_scale_zero]` | PASS |  |  |
| test_query | `test_lint_query[ingest/fixed_size_binary]` | PASS |  |  |
| test_query | `test_lint_query[ingest/float32]` | PASS |  |  |
| test_query | `test_lint_query[ingest/float64]` | PASS |  |  |
| test_query | `test_lint_query[ingest/int16]` | PASS |  |  |
| test_query | `test_lint_query[ingest/int32]` | PASS |  |  |
| test_query | `test_lint_query[ingest/int64]` | PASS |  |  |
| test_query | `test_lint_query[ingest/large_binary]` | PASS |  |  |
| test_query | `test_lint_query[ingest/large_string]` | PASS |  |  |
| test_query | `test_lint_query[ingest/string]` | PASS |  |  |
| test_query | `test_lint_query[ingest/string_view]` | PASS |  |  |
| test_query | `test_lint_query[ingest/time_ms]` | PASS |  |  |
| test_query | `test_lint_query[ingest/time_ns]` | PASS |  |  |
| test_query | `test_lint_query[ingest/time_s]` | PASS |  |  |
| test_query | `test_lint_query[ingest/time_us]` | PASS |  |  |
| test_query | `test_lint_query[ingest/timestamp_ms]` | PASS |  |  |
| test_query | `test_lint_query[ingest/timestamp_ns]` | PASS |  |  |
| test_query | `test_lint_query[ingest/timestamp_s]` | PASS |  |  |
| test_query | `test_lint_query[ingest/timestamp_us]` | PASS |  |  |
| test_query | `test_lint_query[ingest/timestamptz_ms]` | PASS |  |  |
| test_query | `test_lint_query[ingest/timestamptz_ns]` | PASS |  |  |
| test_query | `test_lint_query[ingest/timestamptz_s]` | PASS |  |  |
| test_query | `test_lint_query[ingest/timestamptz_us]` | PASS |  |  |
| test_query | `test_lint_query[type/bind/binary]` | PASS |  |  |
| test_query | `test_lint_query[type/bind/binary_view]` | PASS |  |  |
| test_query | `test_lint_query[type/bind/boolean]` | PASS |  |  |
| test_query | `test_lint_query[type/bind/date]` | PASS |  |  |
| test_query | `test_lint_query[type/bind/decimal]` | PASS |  |  |
| test_query | `test_lint_query[type/bind/fixed_size_binary]` | PASS |  |  |
| test_query | `test_lint_query[type/bind/float16]` | PASS |  |  |
| test_query | `test_lint_query[type/bind/float32]` | PASS |  |  |
| test_query | `test_lint_query[type/bind/float64]` | PASS |  |  |
| test_query | `test_lint_query[type/bind/int16]` | PASS |  |  |
| test_query | `test_lint_query[type/bind/int32]` | PASS |  |  |
| test_query | `test_lint_query[type/bind/int64]` | PASS |  |  |
| test_query | `test_lint_query[type/bind/large_binary]` | PASS |  |  |
| test_query | `test_lint_query[type/bind/large_string]` | PASS |  |  |
| test_query | `test_lint_query[type/bind/string]` | PASS |  |  |
| test_query | `test_lint_query[type/bind/string_view]` | PASS |  |  |
| test_query | `test_lint_query[type/bind/time_ms]` | PASS |  |  |
| test_query | `test_lint_query[type/bind/time_ns]` | PASS |  |  |
| test_query | `test_lint_query[type/bind/time_s]` | PASS |  |  |
| test_query | `test_lint_query[type/bind/time_us]` | PASS |  |  |
| test_query | `test_lint_query[type/bind/timestamp_ms]` | PASS |  |  |
| test_query | `test_lint_query[type/bind/timestamp_ns]` | PASS |  |  |
| test_query | `test_lint_query[type/bind/timestamp_s]` | PASS |  |  |
| test_query | `test_lint_query[type/bind/timestamp_us]` | PASS |  |  |
| test_query | `test_lint_query[type/bind/timestamptz_ms]` | PASS |  |  |
| test_query | `test_lint_query[type/bind/timestamptz_ns]` | PASS |  |  |
| test_query | `test_lint_query[type/bind/timestamptz_s]` | PASS |  |  |
| test_query | `test_lint_query[type/bind/timestamptz_us]` | PASS |  |  |
| test_query | `test_lint_query[type/literal/binary]` | PASS |  |  |
| test_query | `test_lint_query[type/literal/boolean]` | PASS |  |  |
| test_query | `test_lint_query[type/literal/date]` | PASS |  |  |
| test_query | `test_lint_query[type/literal/decimal]` | PASS |  |  |
| test_query | `test_lint_query[type/literal/float32]` | PASS |  |  |
| test_query | `test_lint_query[type/literal/float64]` | PASS |  |  |
| test_query | `test_lint_query[type/literal/int16]` | PASS |  |  |
| test_query | `test_lint_query[type/literal/int32]` | PASS |  |  |
| test_query | `test_lint_query[type/literal/int64]` | PASS |  |  |
| test_query | `test_lint_query[type/literal/string]` | PASS |  |  |
| test_query | `test_lint_query[type/literal/time]` | PASS |  |  |
| test_query | `test_lint_query[type/literal/timestamp]` | PASS |  |  |
| test_query | `test_lint_query[type/literal/timestamptz]` | PASS |  |  |
| test_query | `test_lint_query[type/select/binary]` | PASS |  |  |
| test_query | `test_lint_query[type/select/boolean]` | PASS |  |  |
| test_query | `test_lint_query[type/select/date]` | PASS |  |  |
| test_query | `test_lint_query[type/select/decimal]` | PASS |  |  |
| test_query | `test_lint_query[type/select/float32]` | PASS |  |  |
| test_query | `test_lint_query[type/select/float64]` | PASS |  |  |
| test_query | `test_lint_query[type/select/int16]` | PASS |  |  |
| test_query | `test_lint_query[type/select/int32]` | PASS |  |  |
| test_query | `test_lint_query[type/select/int64]` | PASS |  |  |
| test_query | `test_lint_query[type/select/string]` | PASS |  |  |
| test_query | `test_lint_query[type/select/time]` | PASS |  |  |
| test_query | `test_lint_query[type/select/timestamp0]` | PASS |  |  |
| test_query | `test_lint_query[type/select/timestamp0tz]` | PASS |  |  |
| test_query | `test_lint_query[type/select/timestamp1]` | PASS |  |  |
| test_query | `test_lint_query[type/select/timestamp1tz]` | PASS |  |  |
| test_query | `test_lint_query[type/select/timestamp2]` | PASS |  |  |
| test_query | `test_lint_query[type/select/timestamp2tz]` | PASS |  |  |
| test_query | `test_lint_query[type/select/timestamp3]` | PASS |  |  |
| test_query | `test_lint_query[type/select/timestamp3tz]` | PASS |  |  |
| test_query | `test_lint_query[type/select/timestamp4]` | PASS |  |  |
| test_query | `test_lint_query[type/select/timestamp4tz]` | PASS |  |  |
| test_query | `test_lint_query[type/select/timestamp5]` | PASS |  |  |
| test_query | `test_lint_query[type/select/timestamp5tz]` | PASS |  |  |
| test_query | `test_lint_query[type/select/timestamp6]` | PASS |  |  |
| test_query | `test_lint_query[type/select/timestamp6tz]` | PASS |  |  |
| test_query | `test_lint_query[type/select/timestamp7]` | PASS |  |  |
| test_query | `test_lint_query[type/select/timestamp7tz]` | PASS |  |  |
| test_query | `test_lint_query[type/select/timestamp8]` | PASS |  |  |
| test_query | `test_lint_query[type/select/timestamp8tz]` | PASS |  |  |
| test_query | `test_lint_query[type/select/timestamp9]` | PASS |  |  |
| test_query | `test_lint_query[type/select/timestamp9tz]` | PASS |  |  |
| test_query | `test_query[type/bind/binary]` | PASS |  |  |
| test_query | `test_query[type/bind/boolean]` | PASS |  |  |
| test_query | `test_query[type/bind/date]` | PASS |  |  |
| test_query | `test_query[type/bind/fixed_size_binary]` | PASS |  |  |
| test_query | `test_query[type/bind/int16]` | PASS |  |  |
| test_query | `test_query[type/bind/int32]` | PASS |  |  |
| test_query | `test_query[type/bind/int64]` | PASS |  |  |
| test_query | `test_query[type/bind/large_binary]` | PASS |  |  |
| test_query | `test_query[type/bind/large_string]` | PASS |  |  |
| test_query | `test_query[type/bind/string]` | PASS |  |  |
| test_query | `test_query[type/literal/float64]` | PASS |  |  |
| test_query | `test_query[type/literal/int32]` | PASS |  |  |
| test_query | `test_query[type/literal/string]` | PASS |  |  |
| test_query | `test_query[type/select/binary]` | PASS |  |  |
| test_query | `test_query[type/select/boolean]` | PASS |  |  |
| test_query | `test_query[type/select/date]` | PASS |  |  |
| test_query | `test_query[type/select/int16]` | PASS |  |  |
| test_query | `test_query[type/select/int32]` | PASS |  |  |
| test_query | `test_query[type/select/int64]` | PASS |  |  |
| test_query | `test_query[type/select/string]` | PASS |  |  |
| test_query | `test_query[type/select/timestamp4]` | PASS |  |  |
| test_query | `test_query[type/select/timestamp5]` | PASS |  |  |
| test_query | `test_query[type/select/timestamp6]` | PASS |  |  |
| test_statement | `test_execute_schema_noalias` | PASS |  |  |
| test_statement | `test_nonascii_queries` | PASS |  |  |
| test_statement | `test_prepare` | PASS |  |  |
| test_statement | `test_rows_affected` | PASS |  |  |
| test_statement | `test_transaction_toggle` | PASS |  |  |
