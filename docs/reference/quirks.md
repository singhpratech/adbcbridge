<!-- SPDX-License-Identifier: Apache-2.0 -->
# Driver quirks

No two ODBC (Open Database Connectivity) drivers behave identically. Some report
a capability they do not honour; some need a non-standard spelling of a type or a
statement; some encode wide characters in a way the ODBC standard does not pin
down. adbcBridge keeps these differences out of the rest of the code by detecting
the backing driver at connect time and setting a set of **quirk flags** that the
reader and the parameter binder consult.

This page documents the detection mechanism, every driver adbcBridge recognises
and the quirks it sets, and the meaning of each quirk flag. It is drawn from
`OdbcDetectQuirks` in `src/odbc_driver.c` and the `OdbcReaderOptions` struct in
`src/odbc_internal.h`. None of this is configuration a user normally touches;
two quirks can be forced by option (`adbc.odbc.sqllen_32bit`,
`adbc.odbc.array_binding` — see [Options](options.md)), and the rest are
automatic.

---

## How detection works

`OdbcDetectQuirks` runs once, immediately after a connection is opened. It works
in three passes:

1. **Self-reported capabilities.** `SQLGetInfo` is queried for capabilities the
   driver is willing to describe about itself, and quirk flags are set from the
   answers:
   - `SQL_PARAM_ARRAY_ROW_COUNTS` → `param_array_row_counts`
   - `SQL_TXN_CAPABLE` → `txn_capable` (false when the driver answers `SQL_TC_NONE`)
   - `SQL_MAX_STATEMENT_LEN` → `max_statement_len`
   - `SQL_GETDATA_EXTENSIONS` → `getdata_repair` (needs
     `SQL_GD_BLOCK | SQL_GD_BOUND | SQL_GD_ANY_ORDER`) and `getdata_bound`
   - `SQL_FORWARD_ONLY_CURSOR_ATTRIBUTES1` → `refetch_repair` (from
     `SQL_CA1_ABSOLUTE`)

2. **Driver identity.** `SQLGetInfo(SQL_DRIVER_NAME)` is read, lower-cased, and
   matched with substring tests (`strstr`). MDB Tools (Microsoft Access) does not
   implement `SQL_DRIVER_NAME`, so detection falls back to
   `SQLGetInfo(SQL_DBMS_NAME)` for it. Each matched driver sets the quirks in the
   table below.

3. **Connect-time server probes.** One ODBC driver can front many different
   servers — psqlodbc drives every PostgreSQL-wire database, MySQL Connector/ODBC
   drives every MySQL-wire one — and reports the same driver name for all of them.
   No quirk may be keyed on such a driver's name alone, or it would fire on the
   real database too. For those drivers adbcBridge asks the *server* who it is with
   a small query (`SELECT version()`, `SHOW server_version`,
   `SELECT @@version_comment`, `current_setting('spanner.ddl_transaction_mode')`,
   an `information_schema.engines` count, `SQL_DBMS_VER`), and sets quirks from the
   answer. Errors from these probes are swallowed: a server that does not
   understand the query simply is not the one being looked for.

Some quirks are guarded by `#if defined(_WIN32)` and apply only on Windows,
because the difference they compensate for is a property of the Windows driver
manager (its narrow/wide transcoding) or of a Windows-only build of the driver.
Those are marked **(Windows only)** below.

---

## Driver quirk table

The **match** column is the lower-cased substring tested against the driver name
(or, where noted, a server probe). One database can appear under two matches — a
MySQL-wire warehouse matches `myodbc` and then a server probe narrows it further.

| Driver (match) | Database(s) | Quirks set | Reason (from the source) |
|---|---|---|---|
| `duckdb` | DuckDB | `min_buffer_rows=2048`, `getdata_repair=false`, `getdata_bound=false`, `bool_param_as_int`, `decimal_param_as_varchar`, `no_describe_param`, `no_param_arrays` | Writes a full 2048-row vector into bound buffers regardless of array size; rejects `SQLSetPos`, so truncated values are unrecoverable; rejects `SQL_BIT` params; mis-scales `SQL_DECIMAL` params; `SQLDescribeParam` aborts the process; ignores the indicator array of a column-wise param array |
| `sqlite3odbc` | SQLite | `refetch_repair=true` | Its result set is materialised in memory and `SQLFetchScroll(SQL_FETCH_ABSOLUTE)` re-reads any row correctly, so the 65,536-char width it claims for every TEXT column can be bound |
| `clickhouse` | ClickHouse | `null_param_as_varchar`, `nullable_type_format="Nullable(%s)"`, `no_param_arrays`, `fractional_time_type_format="Time64(%d)"`, `fractional_time_max_digits=9` | Cannot encode typed NULLs for numeric params; needs `Nullable(...)` DDL wrapper; applies only the first sets of a param array; reports whole-second TIME only |
| `maodbc` | MariaDB Connector/ODBC (MariaDB, ColumnStore) | `prefer_param_arrays` (driver < 3.2); `prefer_param_arrays=false`+`no_param_arrays` (driver ≥ 3.2); `getdata_repair=false`; `fractional_time_type_format="TIME(%d)"`, `fractional_time_max_digits=6` | Its bound arrays become one `COM_STMT_BULK_EXECUTE` and beat multi-row INSERT (< 3.2), but 3.2+ over Connector/C segfaults on a NULL DATE in an array; ignores `SQLSetPos`; reports whole-second TIME |
| `maodbc` or `myodbc` + engine probe | MariaDB ColumnStore | `ansi_ddl_type_names` | ColumnStore's DDL parser rejects `LONG VARCHAR` and `BIT`; the standard spellings `TEXT`/`BOOLEAN` are accepted |
| `odbcfb` / `firebirdodbc` | Firebird | `wchar_as_utf8`, `no_param_arrays`, `multirow_union_from="RDB$DATABASE"`, `ddl_string_type_name="VARCHAR(8191)"` | Sizes `SQL_C_WCHAR` in 4-byte wchar_t; executes only the first param set; has no multi-row VALUES (uses `UNION ALL` over the one-row system table); its `BLOB SUB_TYPE TEXT` reads ~100× slower than VARCHAR |
| `ignite` | Apache Ignite | `wchar_as_utf8`, `narrow_params`, `narrow_sql` (Windows only), `no_param_arrays` | Has no wide SQL type at all (`SQL_WVARCHAR` → HYC00); narrow path is UTF-8; reads a param array's NULL indicator from the wrong row |
| `virtodbc` | OpenLink Virtuoso | `wchar_as_utf8` (2-byte SQLWCHAR only), `bigint_param_as_string`, `no_param_arrays`; **(Windows only)** `getdata_repair=false`, `ind_stride_32bit` | ANSI driver has no real `SQL_C_WCHAR`; reads `SQL_C_SBIGINT` as 0; binds `SQL_C_TYPE_DATE` from the first set only; on Win64 strides the bound-column indicator array by 4 bytes |
| `monetdb` | MonetDB | `no_param_arrays` | Executes only the first parameter set of a bound array, silently |
| `verticaodbc` | Vertica | `prefer_param_arrays` | Its client driver turns a bound array into one native bulk load, far ahead of multi-row INSERT on a column store |
| `psqlodbc` + `version()` = `postgresql …` and no fork marker | PostgreSQL (incl. TimescaleDB, Citus) | `pg_array_ingest=true` | Only real PostgreSQL is owed the multi-argument `unnest` array-ingest form; forks (Yugabyte, Cockroach, Greenplum/Cloudberry, openGauss, RisingWave, CrateDB, GreptimeDB, Materialize, YDB, …) are excluded by name |
| `psqlodbc` + `version()` contains `questdb` | QuestDB | `ansi_ddl_type_names`, `bool_param_as_varchar`, `no_param_arrays` | Rejects PostgreSQL's internal type names; parses booleans only from `true`/`false`; cannot convert psqlodbc's inlined array literals |
| `psqlodbc` + `version()` contains `arcadedb` | ArcadeDB | `no_sql_columns` | Its SQL parser rejects psqlodbc's `SQLColumns` query but still answers `SQL_SUCCESS` with an empty result |
| `psqlodbc` + `SHOW server_version` self-identifies | YDB | `ddl_extra_column="adbc_pk SERIAL PRIMARY KEY"`, `pg_array_ingest=false`, `no_sql_columns` | Every table needs a PRIMARY KEY no ingested column can be; leaves `pg_attribute` empty so `SQLColumns` returns nothing |
| `psqlodbc` + `spanner.ddl_transaction_mode` setting exists | Google Cloud Spanner (via PGAdapter) | `pg_array_ingest=false`, `no_timestamp_param_arrays`, `ddl_extra_column="\"adbc_ingest_key\" bigint GENERATED BY DEFAULT AS IDENTITY PRIMARY KEY"`, `max_statement_params=950` | Not PostgreSQL; has no `TIMESTAMP WITHOUT TIME ZONE`; needs a PRIMARY KEY; drops the connection above 950 params instead of failing prepare |
| `myodbc` + `SQL_TC_NONE` | MySQL-wire warehouses (Databend, …) | `temporal_binary_param_as_varchar`, `ansi_ddl_type_names` | Non-transactional MySQL-wire servers run with `NO_SSPS=1` and emit MySQL introducer literals other servers reject; report MySQL's type names whatever the server |
| … + `SQL_DBMS_VER` contains `mongosqld` | MongoDB BI Connector | `no_sql_columns` | `SQLColumns` segfaults on a NULL `NUMERIC_PRECISION` for a DECIMAL column |
| … + `@@version_comment` contains `doris` | Apache Doris | `ddl_table_options="DISTRIBUTED BY RANDOM BUCKETS AUTO PROPERTIES (…)"` | An MPP warehouse refuses a CREATE TABLE with no distribution clause |
| … + `version()` contains `greptimedb` | GreptimeDB (MySQL wire) | `ddl_extra_column="greptime_timestamp TIMESTAMP(3) TIME INDEX DEFAULT CURRENT_TIMESTAMP"`, `ddl_table_options="WITH ('append_mode'='true')"` | Every table needs one NOT NULL TIMESTAMP time index; without append mode rows sharing a time index merge |
| `arrow flight` | Arrow Flight SQL servers (Dremio, sqlflite, …) | `no_sql_columns`; **(Windows only)** `text_as_binary` | `SQLColumns` segfaults inside the first `SQLFetch`; on Windows only `SQL_C_BINARY` reads its text byte-exact |
| `taos_odbc` | TDengine | `timestamp_as_text`, `bool_param_as_tinyint` | No `TIMESTAMP_STRUCT` conversion; the only route into a BOOL column is an integer described as `SQL_TINYINT` |
| `sqora` | Oracle | `bigint_param_as_string`, `multirow_insert_all`, `fixed_rowset`, `getdata_repair=false` | Rejects `SQL_C_SBIGINT`; has no multi-row VALUES (uses `INSERT ALL`); segfaults if the rowset size changes mid-cursor; cannot re-read a truncated LOB value |
| `msodbcsql` | Microsoft SQL Server | `ddl_string_type_name="NVARCHAR(MAX)"` | Its `SQL_LONGVARCHAR` is the deprecated `TEXT`, which cannot be sorted, grouped, de-duplicated or compared |
| `db2` + `SQL_DBMS_NAME` = `IDS…` | IBM Informix | `wchar_as_utf8`, `narrow_params`, `bool_param_as_int` | Gives up on a UTF-16 surrogate pair (-415); a `SQL_C_BIT` param breaks the DRDA stream |
| `db2` + `SQL_DBMS_NAME` not `IDS…` | IBM Db2 | `ddl_string_as_max_varchar` | Its `SQL_LONGVARCHAR` is `LONG VARCHAR`, ~700× slower to bulk-insert than VARCHAR |
| `myodbc` + 4-byte SQLWCHAR | MySQL Connector/ODBC on iODBC (macOS) | `wide_utf16_pairs`, `wchar_as_utf8` | Writes UTF-16 units into 4-byte slots but mangles wide params under `NO_SSPS`; its narrow path is clean UTF-8 |

### Autodetected `sqllen_32bit`

Unless pinned by `adbc.odbc.sqllen_32bit`, the 32-bit-SQLLEN quirk is turned on
when the driver name contains `db2` (but not `libdb2o`/`db2o.`) or `mdbtools`.
IBM's freely downloadable Db2 CLI driver ships a 32-bit-`SQLLEN` `libdb2.so` even
on 64-bit Linux; MDB Tools writes bound-column NULL indicators the same
truncated way.

### Windows final adjustment

On Windows, `wchar_as_utf8` is forced back off for every matched driver **except**
Apache Ignite. The quirk's premise — "the narrow path is UTF-8" — does not hold
on Windows, where a Unicode driver's narrow path is the ANSI code page; Ignite is
the one ANSI-only driver whose own narrow path is genuinely UTF-8.

---

## Quirk flag glossary

Every field of `OdbcReaderOptions` that acts as a quirk, with its exact meaning.

### Buffer sizing and truncation repair

| Flag | Type | Meaning |
|---|---|---|
| `min_buffer_rows` | int64 | Allocate at least this many rows in bound buffers, for a driver (DuckDB) that writes a whole internal chunk regardless of `SQL_ATTR_ROW_ARRAY_SIZE`. |
| `getdata_repair` | bool | `SQLGetData` can re-read a bound column of any row of a block cursor in any order (`SQL_GD_BLOCK|SQL_GD_BOUND|SQL_GD_ANY_ORDER`). Lets a long column be bound and only the truncated values re-read. |
| `getdata_bound` | bool | `SQLGetData` can re-read a bound column (`SQL_GD_BOUND`); enough on its own when the cursor holds one row. |
| `refetch_repair` | bool | `SQLFetchScroll(SQL_FETCH_ABSOLUTE)` re-reads an earlier row and a plain `SQLFetch` resumes after it; another way to recover a truncated bound value. |
| `fixed_rowset` | bool | `SQL_ATTR_ROW_ARRAY_SIZE` is fixed for the life of a cursor (Oracle's SQORA segfaults if it changes); the rowset is chosen before the first fetch and never touched again. |

### Parameter binding

| Flag | Type | Meaning |
|---|---|---|
| `bool_param_as_int` | bool | Bind booleans as an integer described `SQL_INTEGER` (DuckDB, Informix reject `SQL_BIT`). |
| `bool_param_as_tinyint` | bool | Bind booleans as an integer described `SQL_TINYINT` (TDengine's one BOOL route). |
| `bool_param_as_varchar` | bool | Bind booleans as the words `"true"`/`"false"` in a VARCHAR (QuestDB). |
| `bigint_param_as_string` | bool | Send 64-bit integers as numeric text (Oracle, Virtuoso have no `SQL_C_SBIGINT`). |
| `decimal_param_as_varchar` | bool | Bind decimals as `SQL_VARCHAR` text, not `SQL_DECIMAL` (DuckDB mis-scales). |
| `null_param_as_varchar` | bool | Bind NULL parameters as a NULL VARCHAR whatever the type (clickhouse-odbc). |
| `temporal_binary_param_as_varchar` | bool | Bind date/timestamp/binary parameters as VARCHAR text so the server parses the literal (MySQL Connector/ODBC fronting a non-MySQL server). |
| `timestamp_as_text` | bool | The driver has no `TIMESTAMP_STRUCT` conversion; read timestamp columns as text and bind timestamp parameters as text (TDengine). |
| `no_param_arrays` | bool | Column-wise parameter arrays are accepted but only partly executed; bind one execute per row instead. |
| `no_timestamp_param_arrays` | bool | As `no_param_arrays`, but only for a batch that binds a timestamp (Spanner). |
| `prefer_param_arrays` | bool | Keep ODBC parameter arrays ahead of multi-row INSERT for ingest (MariaDB Connector/ODBC, Vertica). |
| `param_array_row_counts` | int | `SQL_PARAM_ARRAY_ROW_COUNTS`: `SQL_PARC_BATCH` (default, one row count per set) or `SQL_PARC_NO_BATCH`. |

### Unicode and statement text

| Flag | Type | Meaning |
|---|---|---|
| `wchar_as_utf8` | bool | The driver's SQLWCHAR is not UTF-16; use the narrow `SQL_C_CHAR` (UTF-8) path for strings both ways. |
| `wide_utf16_pairs` | bool | (4-byte SQLWCHAR only) The driver puts UTF-16 code units in its wchar_t slots — a non-BMP char is a surrogate pair (MySQL Connector/ODBC on iODBC). |
| `narrow_params` | bool | Bind string parameters as `SQL_C_CHAR` (UTF-8) even where the wide path is default, including on Windows (Apache Ignite, Informix). |
| `text_as_binary` | bool | **(Windows)** Read character columns as `SQL_C_BINARY` and take the bytes as UTF-8 (Arrow Flight SQL driver). |
| `narrow_sql` | bool | **(Windows)** Send caller statement text through the narrow `SQLExecDirect`/`SQLPrepare` as UTF-8 (Apache Ignite). |
| `ind_stride_32bit` | bool | **(Windows)** Read side only: the driver strides the bound-column indicator array by 4 bytes per row on a block cursor (Virtuoso on Win64). |
| `sqllen_32bit` | bool | The driver was compiled with a 32-bit `SQLLEN`/`SQLULEN` while the driver manager uses 64-bit (IBM Db2 CLI, MDB Tools). Every `SQLLEN` the driver writes is read at 4-byte width. |
| `sqllen_32bit_forced` | bool | `sqllen_32bit` was pinned by option; suppresses autodetection. |

### DDL and ingest shaping

| Flag | Type | Meaning |
|---|---|---|
| `nullable_type_format` | string | DDL wrapper for a nullable column, e.g. `"Nullable(%s)"` (ClickHouse). |
| `ansi_ddl_type_names` | bool | The names `SQLGetTypeInfo` reports are not accepted in DDL; spell CREATE TABLE with portable type names (QuestDB, MySQL-wire warehouses). |
| `ddl_extra_column` | string | An extra column the server fills in itself, appended to ingest DDL (GreptimeDB time index, YDB / Spanner surrogate primary key). |
| `ddl_table_options` | string | A trailing table-options clause every ingest CREATE TABLE needs (Doris distribution, GreptimeDB append mode). |
| `ddl_string_as_max_varchar` | bool | Spell an unbounded Arrow string as the widest VARCHAR, not `SQL_LONGVARCHAR` (Db2 `LONG VARCHAR`). |
| `ddl_string_type_name` | string | A literal DDL type for an unbounded Arrow string (`NVARCHAR(MAX)` for SQL Server, `VARCHAR(8191)` for Firebird). |
| `fractional_time_type_format` | string | DDL type for a sub-second TIME column, e.g. `"Time64(%d)"`, `"TIME(%d)"`. |
| `fractional_time_max_digits` | int | Largest fractional-digit count that format accepts (0 = no limit). |

### Multi-row INSERT and array ingest

| Flag | Type | Meaning |
|---|---|---|
| `multirow_insert_all` | bool | The server has no multi-row `VALUES` but has Oracle's `INSERT ALL … SELECT 1 FROM dual`. |
| `multirow_union_from` | string | The server has neither; use `INSERT … SELECT <typed>, … FROM <this> UNION ALL …` over this one-row table (Firebird's `RDB$DATABASE`). |
| `pg_array_ingest` | bool | Send a whole batch as one array parameter per column via multi-argument `unnest` (real PostgreSQL only; verified once per connection before use). |
| `max_statement_len` | int64 | `SQL_MAX_STATEMENT_LEN` in bytes (0 = unknown). |
| `max_statement_params` | int64 | A hard parameter ceiling the halving probe cannot find (Spanner's 950; 0 = probe it). |
| `txn_capable` | bool | `SQL_TXN_CAPABLE` was not `SQL_TC_NONE`, so wrapping a multi-row execute in a transaction is worth trying. |

### Metadata and describe

| Flag | Type | Meaning |
|---|---|---|
| `no_describe_param` | bool | Never call `SQLDescribeParam` (DuckDB aborts the process on it). |
| `no_sql_columns` | bool | Never call `SQLColumns` (it segfaults or returns nothing usable with no return code to reveal it); describe `SELECT * FROM <table> WHERE 1=0` instead. |

The four batching constants that bound these behaviours — the starting parameter
ceiling, the VALUES row cap, the array-ingest row count and their byte budgets —
are compile-time limits in `src/odbc_internal.h`, not per-driver quirks.
