<!-- SPDX-License-Identifier: Apache-2.0 -->
# Type mapping

adbcBridge is an ADBC (Arrow Database Connectivity) driver that speaks to a
database through an ODBC (Open Database Connectivity) driver. This page is the
reference for how types cross that boundary in both directions:

- **Read** — an ODBC `SQL_*` type, as the backing driver describes each result
  column, becomes an Arrow type in the schema adbcBridge returns.
- **Bind** — an Arrow type, as it appears in a batch of bound parameters or in a
  bulk-ingest stream, becomes an ODBC C type (`SQL_C_*`) and SQL type (`SQL_*`)
  that adbcBridge hands to `SQLBindParameter`.

Two terms recur below. A driver's **C type** (`SQL_C_CHAR`, `SQL_C_SBIGINT`, …)
is the in-memory shape adbcBridge asks the driver to read or write; the driver's
**SQL type** (`SQL_VARCHAR`, `SQL_BIGINT`, …) is how the value is described to
the server. **SQLWCHAR** is the ODBC wide-character unit: two bytes (UTF-16) with
unixODBC and on Windows, four bytes with iODBC.

Everything here is driven from `src/odbc_reader.c` (`ClassifyColumn`, the
read side) and `src/odbc_bind.c` (`SlotFromArrowValue` and `ArrayParamPlan`, the
bind side). Where a driver quirk changes a mapping, the quirk is named; its full
definition is in [Driver quirks](quirks.md).

---

## Read: ODBC SQL type to Arrow type

adbcBridge calls `SQLDescribeCol` for every result column and classifies it by
its reported SQL type, its column size (precision, or declared width in
characters) and its decimal digits (scale, or fractional-second precision).

### Exact and numeric types

| ODBC SQL type | Condition | Arrow type | ODBC C type read | Notes |
|---|---|---|---|---|
| `SQL_BIT` | — | `bool` | `SQL_C_BIT` | |
| `SQL_TINYINT` | signed | `int8` | `SQL_C_STINYINT` | signedness from `SQL_DESC_UNSIGNED` |
| `SQL_TINYINT` | unsigned | `uint8` | `SQL_C_UTINYINT` | |
| `SQL_SMALLINT` | signed / unsigned | `int16` / `uint16` | `SQL_C_SSHORT` / `SQL_C_USHORT` | |
| `SQL_INTEGER` | signed / unsigned | `int32` / `uint32` | `SQL_C_SLONG` / `SQL_C_ULONG` | |
| `SQL_BIGINT` | signed / unsigned | `int64` / `uint64` | `SQL_C_SBIGINT` / `SQL_C_UBIGINT` | |
| `SQL_REAL` | — | `float32` | `SQL_C_FLOAT` | |
| `SQL_FLOAT`, `SQL_DOUBLE` | — | `float64` | `SQL_C_DOUBLE` | |
| `SQL_DECIMAL`, `SQL_NUMERIC` | `1 ≤ precision ≤ 38`, `0 ≤ scale ≤ precision`, and `adbc.odbc.decimal_as_string` off | `decimal128(precision, scale)` | `SQL_C_CHAR` | value read as text and parsed exactly into a 128-bit decimal |
| `SQL_DECIMAL`, `SQL_NUMERIC` | otherwise (out of `decimal128` range, or `adbc.odbc.decimal_as_string=true`) | `string` (utf8) | `SQL_C_CHAR` | the driver's textual rendering, lossless |

A `SQL_CHAR`/`SQL_VARCHAR`/`SQL_WCHAR`/`SQL_WVARCHAR` column of width ≤ 8 whose
driver type name is a boolean (for example PostgreSQL `bool`, which psqlodbc
reports as a short character column) is detected by name and mapped to Arrow
`bool`, read as text.

### Date and time types

| ODBC SQL type | Condition | Arrow type | Read as | Notes |
|---|---|---|---|---|
| `SQL_TYPE_DATE`, `SQL_DATE` | — | `date32` (days) | `SQL_C_TYPE_DATE` (`DATE_STRUCT`) | |
| `SQL_TYPE_TIME`, `SQL_TIME`, `SQL_SS_TIME2` | scale 0 | `time32[s]` | `SQL_C_TYPE_TIME` (`TIME_STRUCT`) | `TIME_STRUCT` has no sub-second field |
| `SQL_TYPE_TIME`, `SQL_TIME`, `SQL_SS_TIME2` | scale 1–6 | `time64[us]` | text | parsed from the driver's text rendering |
| `SQL_TYPE_TIME`, `SQL_TIME`, `SQL_SS_TIME2` | scale 7–9 | `time64[ns]` | text | |
| `SQL_TYPE_TIME_WITH_TIMEZONE` | — | `string` (utf8) | text | Arrow has no time-with-timezone type; the driver's text form is kept |
| `SQL_TYPE_TIMESTAMP`, `SQL_TIMESTAMP` | naive | `timestamp[unit]` (no zone) | `SQL_C_TYPE_TIMESTAMP` (`TIMESTAMP_STRUCT`) | unit chosen from precision, see below |
| `SQL_TYPE_TIMESTAMP`, `SQL_TIMESTAMP` | driver reports a zone (`IsTimestampWithTimezone`) | `timestamp[us, UTC]` | text | parsed to UTC |
| `SQL_TYPE_TIMESTAMP`, `SQL_TIMESTAMP` | driver has no `TIMESTAMP_STRUCT` for it (quirk `timestamp_as_text`, TDengine) | `timestamp[us]` (no zone) | text | value stays local, no zone attached |
| `SQL_SS_TIMESTAMPOFFSET`, `SQL_TYPE_TIMESTAMP_WITH_TIMEZONE` | — | `timestamp[us, UTC]` | text | SQL Server `datetimeoffset` and ODBC 4.0 timestamp-with-zone |

**Timestamp precision** (`TimestampUnitForColumn`): the Arrow time unit follows
the column's fractional-second precision — scale 1–3 gives `ms`, 4–6 gives `us`,
7–9 gives `ns`. A reported scale of 0 is not trusted on its own: when the column
size is 21–29 (a driver that encodes precision in the width, e.g.
`column_size − 20`), that width is used instead; with any other size (MySQL
Connector/ODBC reports scale 0 / size 19 for `DATETIME(6)`, SQLiteODBC scale 0 /
size 32 for every `TIMESTAMP`) the unit falls back to microseconds, which cannot
silently drop sub-second digits the way whole seconds would. `timestamp[s]` is
therefore never produced. Timestamps carrying a zone are always `us`.

### Other types

| ODBC SQL type | Arrow type | Read as | Notes |
|---|---|---|---|
| `SQL_GUID` | `string` (utf8) | text | 36 characters, or 38 with the braces some drivers add |
| `SQL_INTERVAL_*` (all 13 qualifiers) | `string` (utf8) | text | Arrow intervals cannot express every ODBC qualifier (year-to-month, day-to-second with leading precision, …); the driver's text form is lossless |
| `SQL_BINARY`, `SQL_VARBINARY`, `SQL_LONGVARBINARY`, IBM BLOB | `binary` | `SQL_C_BINARY` | |
| `SQL_CHAR`, `SQL_VARCHAR`, `SQL_LONGVARCHAR` | `string` (utf8) | `SQL_C_CHAR` (POSIX) / `SQL_C_WCHAR` (Windows) | see Unicode below |
| `SQL_WCHAR`, `SQL_WVARCHAR`, `SQL_WLONGVARCHAR` | `string` (utf8) | `SQL_C_WCHAR` | transcoded from UTF-16 (or 4-byte SQLWCHAR) to UTF-8 |
| anything else / unknown | `string` (utf8) | `SQL_C_CHAR` | the fallback: ask the driver for a string representation |

Character columns whose Arrow type ends up `string` always produce **UTF-8**
Arrow data regardless of the C type used to read them.

---

## Unicode on the read path

How a character column is read depends on the platform and on driver quirks,
because the ODBC driver manager transcodes narrow (`SQL_C_CHAR`) and wide
(`SQL_C_WCHAR`) buffers differently on each:

- **unixODBC / iODBC (POSIX):** narrow `SQL_C_CHAR` bytes are passed through
  untouched, so a UTF-8 database is read as UTF-8 over the narrow path. `SQL_CHAR`
  columns take that path; `SQL_WCHAR` columns are read `SQL_C_WCHAR` and
  transcoded from SQLWCHAR to UTF-8. On iODBC, SQLWCHAR is four bytes, and
  adbcBridge's codecs handle both one-code-point-per-unit and surrogate-pair forms.
- **Windows:** the driver manager transcodes a `SQL_C_CHAR` buffer through the
  process ANSI code page (so `"héllo"` corrupts and anything outside the code page
  becomes `?`). To avoid that, **every** character column on Windows — `SQL_CHAR`
  included — is read as `SQL_C_WCHAR` and converted in adbcBridge.

Two Windows-only quirks override that default for specific drivers:

- **`text_as_binary`** (Arrow Flight SQL ODBC driver): its `SQL_C_WCHAR`
  conversion keeps only the low 16 bits of a non-BMP code point and its
  `SQL_C_CHAR` conversion is the ANSI code page, but `SQL_C_BINARY` on a text
  column hands the server's native UTF-8 through byte-exact. The column is read
  `SQL_C_BINARY` and the bytes are taken as UTF-8.
- **`wchar_as_utf8`** (Apache Ignite): an ANSI-only driver whose narrow path is
  already UTF-8. The column is read `SQL_C_CHAR`; the driver manager leaves a
  `SQL_C_CHAR` buffer alone, so the bytes arrive as written. On POSIX this same
  quirk is set for Firebird's OdbcFb, Virtuoso's ANSI driver (on 2-byte-SQLWCHAR
  builds only, i.e. unixODBC, not the iODBC/macOS build, where its wide path is
  the correct one), Informix, and MySQL Connector/ODBC built for iODBC — drivers
  whose SQLWCHAR handling is not UTF-16.

---

## Bind: Arrow type to ODBC parameter

When a batch is bound as parameters (`AdbcStatementBind`/`BindStream`) or fed to
bulk ingest, adbcBridge chooses a C type and SQL type per Arrow column. Two code
paths exist and are kept in agreement: a **row-at-a-time** path
(`SlotFromArrowValue`) and a **column-array** path (`ArrayParamPlan`, used when
array binding is on and the driver supports it). Both send identical wire values.

| Arrow type | ODBC C type | ODBC SQL type | Notes |
|---|---|---|---|
| `null` | `SQL_C_CHAR` | `SQL_VARCHAR` | bound as a NULL varchar |
| `bool` | `SQL_C_BIT` | `SQL_BIT` | see boolean quirks below |
| `int8`/`int16`/`int32` and unsigned that fit in int32 | `SQL_C_SLONG` | `SQL_INTEGER` | `SQL_C_SLONG` is the most widely supported integer binding; narrower types are widened into a staging buffer |
| `int64` (out of int32 range) | `SQL_C_SBIGINT` | `SQL_BIGINT` | |
| `uint64` (out of int32 range) | `SQL_C_UBIGINT` | `SQL_BIGINT` | |
| any wide integer, quirk `bigint_param_as_string` (Oracle, Virtuoso) | `SQL_C_CHAR` | `SQL_NUMERIC` | 64-bit ints sent as numeric text |
| `float16`/`float32`/`float64` | `SQL_C_DOUBLE` (row path) / `SQL_C_FLOAT`+`SQL_REAL` for `float32` (array path) | `SQL_DOUBLE` / `SQL_REAL` | half-float is widened to double |
| `string`, `large_string`, `string_view` | `SQL_C_WCHAR` | `SQL_WVARCHAR` (≤ 4000) / `SQL_WLONGVARCHAR` (> 4000) | sent as UTF-16 by default |
| `string` under quirk `wchar_as_utf8` / `narrow_params` | `SQL_C_CHAR` | `SQL_VARCHAR` / `SQL_LONGVARCHAR` | UTF-8 bytes for drivers whose SQLWCHAR is not UTF-16 |
| `binary`, `large_binary`, `fixed_size_binary`, `binary_view` | `SQL_C_BINARY` | `SQL_VARBINARY` (≤ 4000) / `SQL_LONGVARBINARY` (> 4000) | |
| `date32` | `SQL_C_TYPE_DATE` | `SQL_TYPE_DATE` | |
| `time32[s]` | `SQL_C_TYPE_TIME` | `SQL_TYPE_TIME` | |
| `time32`/`time64` sub-second | `SQL_C_CHAR` | `SQL_VARCHAR` | `"HH:MM:SS.ffffff"`; `TIME_STRUCT` has no fractional field, and several drivers reject `SQL_C_CHAR → SQL_TYPE_TIME` |
| `timestamp` (naive or zoned) | `SQL_C_TYPE_TIMESTAMP` | `SQL_TYPE_TIMESTAMP` | rendered from `TIMESTAMP_STRUCT`; the zone is not sent to the driver |
| `timestamp` under quirk `timestamp_as_text` (TDengine) | `SQL_C_CHAR` | `SQL_VARCHAR` | ISO-8601 text |
| `decimal128`, `decimal256` | `SQL_C_CHAR` | `SQL_DECIMAL` | rendered to exact decimal text, described with the Arrow precision and scale |
| `decimal128`/`decimal256` under quirk `decimal_param_as_varchar` (DuckDB) | `SQL_C_CHAR` | `SQL_VARCHAR` | DuckDB mis-scales `SQL_DECIMAL` parameters |
| `dictionary` | (its value type) | (its value type) | decoded per row and bound as the value it encodes — a pandas categorical binds like the plain column |
| anything else | — | — | `ADBC_STATUS_NOT_IMPLEMENTED`, naming the Arrow type |

Strings longer than 4000 units are described as the `LONG` variant of their SQL
type; the `column_size` sent is the value's own length. Half-float is always
widened to double.

### Boolean binding quirks

Arrow `bool` normally binds as `SQL_C_BIT` / `SQL_BIT`, but three quirks reroute it:

- **`bool_param_as_varchar`** (QuestDB): the words `"true"`/`"false"` in a
  `SQL_VARCHAR` — QuestDB parses a boolean only from those words.
- **`bool_param_as_int`** (DuckDB, Informix): an integer described as
  `SQL_INTEGER` — those drivers reject `SQL_BIT` parameters.
- **`bool_param_as_tinyint`** (TDengine): an integer described as `SQL_TINYINT`,
  the one boolean-parameter route taos-odbc implements.

### Temporal / binary text fallback

Under quirk **`temporal_binary_param_as_varchar`** (MySQL Connector/ODBC fronting
a non-MySQL server with server-side prepares off), date, timestamp and binary
parameters are bound as `SQL_VARCHAR` text so the server's own literal parsing
coerces them, instead of the driver emitting MySQL charset-introducer literals
(`_binary'...'`) that other servers cannot parse.

---

## NULL handling

- A NULL Arrow value sets the ODBC length/indicator to `SQL_NULL_DATA`. The C and
  SQL type are still chosen from the Arrow column type, so a typed NULL is sent.
- Arrow `null` (untyped, e.g. Python `None`) is sent as a NULL `SQL_VARCHAR`.
- Under quirk **`null_param_as_varchar`** (clickhouse-odbc, which cannot encode a
  typed NULL for a numeric parameter) every NULL is bound as a NULL `SQL_VARCHAR`
  whatever the column's Arrow type. This is a per-value decision, so a batch with
  NULLs in such a column cannot use array binding and falls back to row-at-a-time.
- On read, a column the driver reports as `SQL_NO_NULLS` clears the Arrow
  `NULLABLE` flag on its field; otherwise the field is nullable.

---

## Unknown and unsupported types

- **Read:** any SQL type not listed above falls through to the default arm and is
  read as text (`SQL_C_CHAR`, or `SQL_C_WCHAR` on Windows) and delivered as an
  Arrow `string`. Nothing is dropped; the worst case is that a value arrives as
  its textual rendering rather than a native Arrow type.
- **Bind:** an Arrow type not listed above (for example a list, struct, map, or
  union) fails the bind with `ADBC_STATUS_NOT_IMPLEMENTED`, and the error names
  the offending Arrow type.
