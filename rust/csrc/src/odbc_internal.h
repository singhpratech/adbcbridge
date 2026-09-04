// Copyright 2026 the adbcbridge authors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//
// SPDX-License-Identifier: Apache-2.0

// adbcbridge: an ADBC driver that bridges any ODBC driver to Apache Arrow.
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

// The Windows SDK's sqltypes.h is written against windows.h (SQLLEN is INT64,
// SQLHWND is HWND) and does not include it itself; unixODBC and iODBC are
// self-contained, which is why only the Windows build ever noticed.
#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif
#include <sql.h>
#include <sqlext.h>

#include <arrow-adbc/adbc.h>
#include "nanoarrow/nanoarrow.h"
#include "utils.h"

// --- SQL type codes not present in every driver-manager header ---------------
// Microsoft SQL Server extended types (msodbcsql.h).
#ifndef SQL_SS_TIME2
#define SQL_SS_TIME2 (-154)
#endif
#ifndef SQL_SS_TIMESTAMPOFFSET
#define SQL_SS_TIMESTAMPOFFSET (-155)
#endif
// ODBC 4.0 datetime-with-timezone types.
#ifndef SQL_TYPE_TIME_WITH_TIMEZONE
#define SQL_TYPE_TIME_WITH_TIMEZONE 94
#endif
#ifndef SQL_TYPE_TIMESTAMP_WITH_TIMEZONE
#define SQL_TYPE_TIMESTAMP_WITH_TIMEZONE 95
#endif

#if defined(_WIN32)
#define ADBC_ODBC_EXPORT __declspec(dllexport)
#else
#define ADBC_ODBC_EXPORT __attribute__((visibility("default")))
#endif

#define ADBC_ODBC_DRIVER_NAME "ADBC ODBC Driver"
#define ADBC_ODBC_DRIVER_VERSION "0.1.0"

// Driver-specific options
#define ADBC_ODBC_OPTION_DSN "dsn"
#define ADBC_ODBC_OPTION_CONNECTION_STRING "adbc.odbc.connection_string"
#define ADBC_ODBC_OPTION_BATCH_SIZE "adbc.odbc.batch_size"
#define ADBC_ODBC_OPTION_MAX_BIND_BYTES "adbc.odbc.max_bind_bytes"
/// Width, in bytes, at which a column whose declared width is past
/// `adbc.odbc.max_bind_bytes` is bound anyway.  See OdbcReaderOptions::long_bind_bytes.
#define ADBC_ODBC_OPTION_LONG_BIND_BYTES "adbc.odbc.long_bind_bytes"
/// Total bytes of bound rowset buffers a reader may allocate.  The rowset holds
/// `adbc.odbc.batch_size` rows unless that would exceed this budget, in which case it
/// holds as many rows as fit (at least one).  See OdbcReaderOptions::rowset_bytes.
#define ADBC_ODBC_OPTION_ROWSET_BYTES "adbc.odbc.rowset_bytes"
#define ADBC_ODBC_OPTION_DECIMAL_AS_STRING "adbc.odbc.decimal_as_string"
/// Bind Arrow batches as ODBC parameter arrays (one execute per batch) when the
/// driver supports it.  "true"/"false"; default true.
#define ADBC_ODBC_OPTION_ARRAY_BINDING "adbc.odbc.array_binding"
/// Rows per INSERT statement for bulk ingest ("adbc.ingest.*"): the driver ingests a
/// batch with `INSERT INTO t VALUES (?,?),(?,?),...` carrying this many row-groups of
/// parameters per execute, which collapses one round trip per row into one per group.
/// 0 (the default) picks a group size automatically, 1 disables the rewrite and leaves
/// the ingest on one INSERT per row (or on parameter arrays, where the driver has
/// usable ones).  Only bulk ingest is rewritten; a caller's own SQL is never touched.
/// See OdbcConnection::multirow_* for the probe that decides whether the server takes
/// the multi-row form at all.
#define ADBC_ODBC_OPTION_ROWS_PER_INSERT "adbc.odbc.rows_per_insert"
/// Number of connections a bulk ingest ("adbc.ingest.*") may spread its work over.  1
/// (the default) keeps the ingest on the caller's own connection, in one transaction,
/// exactly as it has always behaved.  N > 1 opens N-1 further connections to the same
/// database, hands each a share of the bound stream's batches, and lets each run the
/// ordinary multi-row INSERT path into the same table.
///
/// This TRADES ATOMICITY FOR SPEED and is opt-in for that reason: N connections are N
/// independent transactions, so the ingest as a whole is no longer atomic.  A worker
/// that fails makes the whole call fail, and every worker still running rolls its own
/// share back -- but a worker that had already finished has already committed, and
/// those rows stay in the table.  The caller sees an error and a table holding some
/// unspecified subset of the stream.  Only use N > 1 where a partially populated table
/// on failure is acceptable (or where the caller drops and retries).
///
/// Fan-out needs the target table to be visible to the worker connections, so it is
/// skipped -- silently falling back to one connection -- when the caller is inside its
/// own transaction (autocommit off), because the CREATE TABLE would then be uncommitted
/// and invisible to them.
#define ADBC_ODBC_OPTION_INGEST_CONNECTIONS "adbc.odbc.ingest_connections"
/// Number of partitions AdbcStatementExecutePartitions should try to split the query
/// into.  0 (the default) lets the driver choose from the table's size -- one partition
/// per 64 MiB of heap, capped at 8; 1 disables splitting and always returns the original
/// query as a single partition.  A query the driver cannot prove it can slice exactly
/// falls back to one partition whatever this says.  See src/odbc_partition.c.
#define ADBC_ODBC_OPTION_PARTITIONS "adbc.odbc.partitions"
/// Rowsets to keep in flight on a background fetch thread, so that SQLFetch for the next
/// rowset overlaps the Arrow conversion of the current one.  0 (the default) is off; 1 is
/// double buffering.  See OdbcReaderOptions::prefetch.
#define ADBC_ODBC_OPTION_PREFETCH "adbc.odbc.prefetch"
/// Force the 32-bit-SQLLEN driver quirk on or off ("true"/"false").  Unset means
/// autodetect from SQL_DRIVER_NAME; see OdbcReaderOptions::sqllen_32bit.
#define ADBC_ODBC_OPTION_SQLLEN_32BIT "adbc.odbc.sqllen_32bit"
/// Let adbcbridge add ODBC connection keywords that suit how it reads a result set,
/// where it recognises the target driver ("true"/"false"; default true).  A keyword the
/// caller set -- in the connection string or in the DSN -- is never overridden, and
/// nothing that changes what a query returns is ever set.  See OdbcTuneConnectionString
/// for the complete list of what this adds.
#define ADBC_ODBC_OPTION_TUNE "adbc.odbc.tune"
/// Read-only: SQL_DRIVER_NAME of the underlying ODBC driver.  ADBC_INFO_DRIVER_NAME
/// must be a stable identity for adbcbridge itself, so the backing driver's file
/// name is exposed here (and, as context, in ADBC_INFO_VENDOR_NAME) instead.
#define ADBC_ODBC_OPTION_DRIVER_NAME "adbc.odbc.driver_name"
/// Read-only: SQL_DRIVER_NAME of the underlying ODBC driver.  ADBC_INFO_DRIVER_NAME
/// must be a stable identity for adbcbridge itself, so the backing driver's file
/// name is exposed here (and, as context, in ADBC_INFO_VENDOR_NAME) instead.

// Ceiling on `adbc.odbc.partitions`: a partition is a connection's worth of work, and
// no useful split needs more of them than a large machine has cores.
#define ADBC_ODBC_MAX_PARTITIONS 256
// Ceiling on `adbc.odbc.prefetch`.  Each rowset in flight is another full set of bound
// buffers (`adbc.odbc.rowset_bytes` of them), and one is already enough to hide the
// fetch behind the conversion; more only helps when the two are very unevenly matched.
#define ADBC_ODBC_MAX_PREFETCH 8

#define ADBC_ODBC_DEFAULT_BATCH_SIZE 1024
#define ADBC_ODBC_DEFAULT_MAX_BIND_BYTES 32768
// 2 KiB per cell holds any ordinary text value while keeping a 1024-row rowset inside a
// few megabytes -- and, on a driver that null-fills a bound buffer (sqliteodbc does), it
// is 2 KiB written per row instead of the 256 KiB its declared width would ask for.
// Reading 100,000 rows of (int, double, text, date) out of SQLite takes 0.061 s at 2 KiB,
// 0.069 s at 4 KiB, 0.101 s at 16 KiB and 0.59 s at the declared 256 KiB.
#define ADBC_ODBC_DEFAULT_LONG_BIND_BYTES 2048
#define ADBC_ODBC_DEFAULT_ROWSET_BYTES (8 * 1024 * 1024)

// --- Multi-row INSERT ingest batching ---------------------------------------
// ODBC has no SQLGetInfo for "how many parameters may one statement carry": the closest
// is SQL_MAX_STATEMENT_LEN (bytes of SQL text), and most drivers answer 0 = unknown for
// even that.  So the parameter ceiling starts at a value every tested backend accepts and
// is *probed* downwards -- SQLPrepare of the K-row statement, halving K on failure -- with
// the answer remembered on the connection.  2000 clears SQL Server's 2100-parameter limit
// and, at four columns, its 1000-row VALUES limit; SQLite (999 or 32766 depending on the
// build) and anything else smaller is found by halving.
#define ADBC_ODBC_MULTIROW_MAX_PARAMS 2000
// SQL Server refuses a VALUES clause with more than 1000 row constructors.
#define ADBC_ODBC_MULTIROW_MAX_ROWS 1000
// SQL text budget when the driver answers 0 for SQL_MAX_STATEMENT_LEN.  Db2's real limit
// is ~2 MB and MySQL's max_allowed_packet defaults to 64 MB; 1 MB is under both and is
// never the binding constraint at the parameter counts above (a four-column row group is
// about fifteen bytes of text).
#define ADBC_ODBC_MULTIROW_MAX_SQL_BYTES (1024 * 1024)
// Ceiling on the per-row parameter scratch one multi-row execute may allocate.
#define ADBC_ODBC_MULTIROW_MAX_SLOT_BYTES (16 * 1024 * 1024)

// --- Array ingest (PostgreSQL) ----------------------------------------------
// Rows carried by one `INSERT INTO t SELECT * FROM unnest(?::t1[], ?::t2[], ...)`.
// The statement takes one parameter per *column* however many rows it carries, so the
// row count is bounded by how much text one parameter should hold rather than by any
// parameter ceiling.  10,000 rows of the four-column benchmark shape is about 600 kB
// spread over four parameters; measured against PostgreSQL 16 it is within noise of
// 50,000 and clearly ahead of 1,000 (see bench/BENCHMARKS.md).
#define ADBC_ODBC_ARRAY_INGEST_ROWS 10000
// Ceiling on the array literal built for one column of one such statement.  A batch that
// would exceed it is split, so a table of long strings does not build a parameter the
// server would refuse (PostgreSQL's own limit is 1 GB per value).
#define ADBC_ODBC_ARRAY_INGEST_MAX_BYTES (32 * 1024 * 1024)

// --- Parallel bulk ingest (ADBC_ODBC_OPTION_INGEST_CONNECTIONS) --------------
// Ceiling on `adbc.odbc.ingest_connections`.  A worker is a connection's worth of work,
// and servers cap concurrent connections far below this anyway.
#define ADBC_ODBC_MAX_INGEST_CONNECTIONS 64
// A stream that hands the driver one enormous batch would otherwise pin the whole ingest
// to a single worker, so batches longer than this are sliced into pieces of this many
// rows before they are queued.  16384 keeps every worker fed without making the queue
// itself the cost: at the default group size it is a few tens of executes per slice.
#define ADBC_ODBC_INGEST_SLICE_ROWS 16384

/// The spelling of "one INSERT carrying K rows" a server takes.  Probed in that order:
/// the standard form first, the two quirk forms only after it has been refused.
enum OdbcMultiRowForm {
  /// `INSERT INTO t (cols) VALUES (?, ?), (?, ?)` -- the standard, and what nearly
  /// every server takes.
  ODBC_MULTIROW_VALUES = 0,
  /// `INSERT ALL INTO t (cols) VALUES (?, ?) INTO t (cols) VALUES (?, ?) SELECT 1 FROM dual`
  /// -- Oracle, which has no multi-row VALUES (OdbcReaderOptions::multirow_insert_all).
  ODBC_MULTIROW_INSERT_ALL = 1,
  /// `INSERT INTO t (cols) SELECT CAST(? AS <type>), ... FROM <one-row table>
  /// UNION ALL SELECT ...` -- Firebird, which has neither of the above
  /// (OdbcReaderOptions::multirow_union_from).
  ODBC_MULTIROW_UNION = 2,
};

/// A refcounted ODBC statement handle shared between an AdbcStatement and
/// the ArrowArrayStream it produced.
struct OdbcHandleRef {
  SQLHSTMT hstmt;
  int refcount;
};

struct OdbcHandleRef* OdbcHandleRefNew(SQLHSTMT hstmt);
void OdbcHandleRefRelease(struct OdbcHandleRef* ref);

/// Populate an AdbcError from ODBC diagnostics and return a mapped status code.
AdbcStatusCode OdbcSetError(SQLSMALLINT handle_type, SQLHANDLE handle, const char* context,
                            struct AdbcError* error);

/// Check an ODBC return code; on failure set error and return.
#define ODBC_CHECK(RET, HTYPE, HANDLE, CONTEXT, ERROR)                    \
  do {                                                                    \
    SQLRETURN odbc_ret_ = (RET);                                          \
    if (!SQL_SUCCEEDED(odbc_ret_)) {                                      \
      return OdbcSetError((HTYPE), (HANDLE), (CONTEXT), (ERROR));         \
    }                                                                     \
  } while (0)

struct OdbcReaderOptions {
  int64_t batch_size;
  int64_t max_bind_bytes;
  // Width to bind a column whose declared width is past max_bind_bytes at, rather than
  // leaving it unbound.  Drivers describe a text or binary column by what its type could
  // hold -- sqliteodbc says 65,536 characters for every TEXT column, MySQL 16,777,215,
  // SQL Server 2,147,483,647 for NVARCHAR(MAX) -- so honouring those widths would mean
  // either a gigabyte of rowset or, as before, no binding at all and a one-row rowset for
  // the whole result set.  Binding narrow and re-reading the values that overflow costs
  // nothing on the values that do not.  Only used when a clipped value can be recovered;
  // see TruncationRepairable().
  int64_t long_bind_bytes;
  // Ceiling on the bound rowset buffers of one reader, in bytes.  A wide text column
  // makes a row expensive (sqliteodbc describes a TEXT column as 65,536 characters), so
  // sizing the rowset in rows alone would allocate hundreds of megabytes; the reader
  // fetches min(batch_size, rowset_bytes / row_width) rows at a time instead.
  int64_t rowset_bytes;
  bool decimal_as_string;
  // Driver quirk: the precision and scale SQLDescribeCol reports for a DECIMAL/NUMERIC
  // column are not the column's, so use these instead.  Set only for a driver whose
  // server has exactly one decimal type, since that is the only case where a fixed pair
  // can be right: Kinetica's is DECIMAL(18, 4) -- every DECIMAL(p, s) in DDL is stored
  // as that one -- while its driver describes such a column as precision 38 scale 0
  // (and SQLColumns answers 18, 18).  Believing scale 0 turns the "12.3450" the driver
  // hands over into a decimal128(38, 0) holding 12: silent truncation of every
  // fractional digit, which is why this is corrected rather than tolerated.
  // Zero (the default) leaves the described values alone.
  int32_t decimal_fixed_precision;
  int32_t decimal_fixed_scale;
  // Driver quirk: some drivers (DuckDB) write a whole internal chunk into bound
  // buffers regardless of SQL_ATTR_ROW_ARRAY_SIZE; allocate at least this many rows.
  int64_t min_buffer_rows;
  // Driver capability: SQLGetData can re-read a bound column of any row of a block
  // cursor, in any order (SQL_GD_BLOCK | SQL_GD_BOUND | SQL_GD_ANY_ORDER).  That lets
  // us bind a long column at its declared width and fall back to SQLGetData only for
  // the values that come back truncated, instead of refusing to bind it at all and
  // collapsing the whole result set to a one-row rowset.
  bool getdata_repair;
  // Driver capability: SQLGetData can re-read a column that is bound (SQL_GD_BOUND).
  // Enough on its own when the cursor holds a single row, which is what makes both
  // getdata_repair and refetch_repair work.
  bool getdata_bound;
  // Driver capability: SQLFetchScroll(SQL_FETCH_ABSOLUTE) re-reads an earlier row of
  // this cursor and a plain SQLFetch then resumes after it.  That makes a truncated
  // bound value recoverable without SQLGetData on a block cursor: the reader re-reads
  // the whole rowset one row at a time (where SQLGetData is always legal) and carries
  // on.  It is what lets a "long" column be bound at all on a driver that cannot
  // SQLGetData from a block cursor.
  bool refetch_repair;
  // Driver quirk: SQL_ATTR_ROW_ARRAY_SIZE is fixed for the life of a cursor -- whatever
  // it was when the statement was executed is the only size that cursor can be fetched
  // at.  Oracle's SQORA sizes its per-rowset fetch state once and then indexes it with
  // the current array size: raising the size mid-cursor walks off the end of that state
  // and segfaults inside the driver (see OdbcDetectQuirks), and lowering it silently
  // ends the result set early.  The reader picks the rowset before the first fetch and
  // never touches the attribute again on such a driver.
  bool fixed_rowset;
  // Driver quirk: bind boolean parameters as integers (DuckDB rejects SQL_BIT params).
  bool bool_param_as_int;
  // Driver quirk: bind boolean parameters as an integer described as SQL_TINYINT, not
  // SQL_INTEGER.  TDengine's taos-odbc looks a parameter conversion up by the exact
  // (C type, SQL type, column type) triple and lists exactly one route into a BOOL
  // column, SQL_C_SBIGINT described as SQL_TINYINT: SQL_BIT and SQL_INTEGER are both
  // "not implemented yet", and its VARCHAR route parses the text with strtoll, so the
  // "true"/"false" of bool_param_as_varchar is refused too.
  bool bool_param_as_tinyint;
  // Driver quirk: bind boolean parameters as the words "true"/"false" in a VARCHAR.
  // QuestDB's PostgreSQL wire protocol parses a boolean parameter only from those two
  // words, while psqlodbc sends an SQL_BIT parameter as "1"/"0" -- which QuestDB stores
  // as false without a diagnostic, so every true silently becomes false.
  bool bool_param_as_varchar;
  // Driver quirk: no SQL_C_SBIGINT parameter support (Oracle); send 64-bit ints as numeric text.
  bool bigint_param_as_string;
  // Driver quirk: bind decimal parameters as VARCHAR text instead of SQL_DECIMAL (DuckDB).
  bool decimal_param_as_varchar;
  // Driver quirk: bind NULL parameters as a NULL VARCHAR regardless of Arrow type
  // (clickhouse-odbc cannot encode typed NULLs for numeric parameters).
  bool null_param_as_varchar;
  // Driver quirk: DDL type wrapper for nullable columns, e.g. "Nullable(%s)" (ClickHouse).
  const char* nullable_type_format;
  // Server quirk: how to spell "return no rows" in the zero-row SELECT that
  // GetTableSchema -- and GetObjects' describe fallback -- reads a table's columns off.
  // NULL means the default, "WHERE 1=0".  Kinetica's planner constant-folds a provably
  // false predicate and answers the query from its empty pseudo-table SYSTEM.ITER, which
  // cannot carry a BYTES column: "Invalid attribute: BYTES(null) for table: SYSTEM.ITER
  // ... Unknown function: BYTES", so GetTableSchema fails on any table holding binary.
  // "LIMIT 0" is not folded that way and describes the real table.
  const char* zero_row_suffix;
  // Driver quirk: the names SQLGetTypeInfo reports are not names the server accepts in
  // DDL, so bulk ingest spells its CREATE TABLE with portable SQL type names (BIGINT,
  // DOUBLE, BOOLEAN, ...) instead.  psqlodbc drives every PostgreSQL-wire
  // server but answers SQLGetTypeInfo with PostgreSQL's own internal names ("int8",
  // "float8", "bool", "numeric"); QuestDB, which has its own type system behind that
  // wire protocol, rejects those with "unsupported column type" while accepting the
  // standard spellings.
  // Also set for MySQL Connector/ODBC fronting a non-MySQL server (Databend): it always
  // answers SQLGetTypeInfo with MySQL's names ("bit", "long varchar", "datetime"),
  // which such servers reject.
  bool ansi_ddl_type_names;
  // Server requirement (not a driver bug): this server refuses any CREATE TABLE that
  // does not declare something the generic ingest DDL has no notion of, so a bulk-ingest
  // table is created as
  //     CREATE TABLE t (<ingested columns>, <ddl_extra_column>) <ddl_table_options>
  // Both are NULL for every other server, which leaves the DDL exactly as it was.
  //
  // GreptimeDB needs both.  Every GreptimeDB table must carry exactly one TIME INDEX
  // column, and it has to be a NOT NULL TIMESTAMP -- an ingest payload need not have any
  // timestamp column at all, so the extra column is one the server fills in itself
  // (DEFAULT CURRENT_TIMESTAMP), which leaves the ingested columns untouched.  The table
  // option matters just as much: outside append mode GreptimeDB *merges* rows that share
  // a time index, so rows ingested within the same millisecond would silently collapse
  // into one.
  //
  // YDB and Google Cloud Spanner need only the column, for the same reason as each
  // other: every table must have a PRIMARY KEY ("Primary key is required for ydb
  // tables", "Primary key must be defined for table"), and none of the ingested columns
  // can serve as one -- an ingest payload may repeat a value, or hold NULL, in any
  // column.  Both get a surrogate key the server fills in itself ("adbc_pk SERIAL
  // PRIMARY KEY"; Spanner's PostgreSQL dialect spells it "adbc_ingest_key" bigint
  // GENERATED BY DEFAULT AS IDENTITY PRIMARY KEY).  The extra column is never written
  // to -- the generated INSERT names the ingested columns explicitly.
  //
  // Apache Doris needs only the table option: an MPP warehouse refuses a CREATE TABLE
  // with no distribution clause ("Create olap table should contain distribution desc"),
  // and -- without a key clause -- also refuses one whose leading column is a string,
  // float or double, since those are the columns it would otherwise make the duplicate
  // key out of.  "DISTRIBUTED BY RANDOM BUCKETS AUTO PROPERTIES
  // ("enable_duplicate_without_keys_by_default" = "true")" answers both.
  const char* ddl_extra_column;
  const char* ddl_table_options;
  // Driver quirk: bind date, timestamp and binary parameters as SQL_VARCHAR text and
  // let the server's own literal parsing coerce them, instead of using SQL_TYPE_DATE /
  // SQL_TYPE_TIMESTAMP / SQL_VARBINARY.
  //
  // MySQL Connector/ODBC substitutes bound parameters into the SQL text itself whenever
  // server-side prepared statements are off (NO_SSPS=1, which is the only way to reach a
  // MySQL-wire server that has no prepare support at all -- Databend answers
  // COM_STMT_PREPARE with "Prepare is not support in Databend").  In that mode it writes
  // every parameter whose *SQL* type is not character or numeric as a MySQL
  // charset-introducer literal: `_binary'2024-02-29'` for a DATE, `_binary'...'` for a
  // TIMESTAMP or a VARBINARY.  Introducers are MySQL/MariaDB syntax, so on any other
  // server behind that driver the statement fails to parse.  Binding the same values as
  // plain SQL_VARCHAR makes the driver emit an ordinary quoted literal, which such
  // servers accept and coerce -- the same trick the sub-second TIME path in
  // SlotFromArrowValue() already uses for every driver.
  bool temporal_binary_param_as_varchar;
  // Driver quirk: bind binary parameters as SQL_VARCHAR text -- the binary half of
  // temporal_binary_param_as_varchar above, without touching dates and timestamps.
  //
  // Exasol has no binary column type at all (BLOB is 0A000 "Feature not supported: data
  // type BLOB", and VARBINARY/BINARY/RAW are not words its parser knows), and its ODBC
  // driver refuses the C type outright: SQLBindParameter with SQL_C_BINARY answers
  // HY003, "Invalid application buffer type: SQL_C_BINARY", whatever the target column
  // is.  So an Arrow binary column cannot reach that server by the ordinary route at
  // all.  Sent as SQL_C_CHAR into a VARCHAR the same bytes store and read back byte for
  // byte, which is what a server with no binary type can offer.  Its dates, timestamps
  // and VARBINARY-less type system are otherwise fine, so nothing else changes.
  bool binary_param_as_varchar;
  // Driver quirk: a NULL parameter whose SQL type is SQL_DECIMAL or SQL_NUMERIC cannot be
  // bound with SQL_C_DEFAULT; name SQL_C_CHAR for it instead.  See NullParamCType.
  //
  // Exasol's driver answers SQLExecute with SI002, "C-Type not supported", followed by
  // HY010, "Error creating prepared statement header", for exactly that pair -- so the
  // whole statement fails, not just the parameter.  It matters more here than the name
  // suggests: Exasol has no narrow integer type, INT/INTEGER/BIGINT are all aliases of
  // DECIMAL, and SQLDescribeParam (which the NULL path asks first, and believes) reports
  // SQL_DECIMAL for every one of them.  So a NULL in any numeric column takes this
  // route.  Binding the same NULL as SQL_C_CHAR with a NULL data pointer is accepted.
  bool null_decimal_param_as_char;
  // Driver quirk: the ODBC driver was compiled with a 32-bit SQLLEN/SQLULEN while the
  // driver manager and this driver use 64-bit ones.  IBM's freely downloadable Db2
  // "clidriver" ships exactly such a libdb2.so on 64-bit Linux (the 64-bit-SQLLEN build
  // is the separate libdb2o.so).  Every SQLLEN/SQLULEN the driver *writes* is then four
  // bytes wide: indicator arrays are int32 with stride 4, and scalar out-parameters
  // (SQLRowCount, SQLDescribeCol's column size, SQLColAttribute's numeric attribute,
  // SQLGetData's StrLen_or_Ind, the rows-fetched / params-processed pointers) get only
  // their low four bytes.  See OdbcReadLen()/OdbcReadULen()/OdbcIndicator*() below.
  // MDB Tools (Microsoft Access) writes bound-column indicators the same way: after
  // SQLFetch a NULL column's four low bytes are 0xffffffff and the high half is
  // whatever was there before, so an unrepaired NULL reads as a 4 GB length.
  bool sqllen_32bit;
  // True once a user option pinned sqllen_32bit; suppresses autodetection.
  bool sqllen_32bit_forced;
  // Driver quirk, read side only: the driver advances the *bound-column indicator*
  // array by four bytes per row on a block cursor while striding data buffers correctly
  // (OpenLink Virtuoso's virtodbc.dll 7.2 on Win64).  Row k+1 overwrites only the high
  // half of row k's 8-byte SQLLEN, so the low four bytes of every row survive at offset
  // 4k -- exactly what OdbcReadLen(sqllen_32bit=true) reads.  Unlike sqllen_32bit this is
  // read-only: parameter indicators (OdbcIndicatorSet) stay 8-byte, which this driver
  // reads correctly.
  bool ind_stride_32bit;
  // Driver quirk: the driver's SQLWCHAR width differs from the driver manager's this
  // library was compiled against -- e.g. a driver built with wchar_t (4-byte) SQLWCHAR
  // loaded through unixODBC (Firebird OdbcFb). (A bridge compiled against iODBC uses
  // 4-byte units throughout, so an iODBC-built vendor driver needs no quirk there.)
  // Symptom: a bound SQL_C_WCHAR parameter is truncated to
  // byte_length/4 characters ("héllo 🚀" stores as "héll") and fetched wide columns come
  // back as UTF-32. Use the narrow SQL_C_CHAR path instead, which is UTF-8 when the
  // connection is opened with CHARSET=UTF8.
  bool wchar_as_utf8;
  // Driver quirk, four-byte SQLWCHAR (iODBC) only: the driver puts UTF-16 code units
  // into its wchar_t slots -- a non-BMP character travels as a surrogate pair of two
  // units -- rather than one code point per unit (MySQL Connector/ODBC 26.7 for macOS).
  // Bound wide parameters are then encoded that way; the reader accepts both forms
  // regardless, since a surrogate is never a valid code point on its own.
  bool wide_utf16_pairs;
  // Driver quirk: bind string parameters as SQL_C_CHAR (UTF-8 bytes) even where the wide
  // path is the default -- for a driver that has no wide SQL type at all (Apache Ignite:
  // SQLBindParameter answers HYC00 for SQL_WVARCHAR) and whose narrow path hands the
  // bytes through.  Unlike wchar_as_utf8 this holds on Windows too: the driver manager
  // transcodes narrow *statement text*, never a bound SQL_C_CHAR buffer, so the bytes
  // reach such a driver intact while fetched text still comes back wide.
  bool narrow_params;
  // Driver quirk, Windows: read character columns as SQL_C_BINARY and take the bytes as
  // UTF-8.  For a driver whose SQL_C_WCHAR conversion is lossy and whose SQL_C_CHAR
  // conversion goes through the ANSI code page, but whose binary conversion of a text
  // column hands its native UTF-8 through untouched (the Arrow Flight SQL ODBC driver:
  // U+1F680 arrives as U+F680 wide -- the low 16 bits -- and as '?' narrow, byte-exact as
  // binary; measured on Windows with pyodbc).  Binary is the one C type neither the
  // driver manager nor such a driver transcodes.
  bool text_as_binary;
  // Driver quirk, Windows: send statement text through the narrow SQLExecDirect /
  // SQLPrepare entry points as UTF-8 bytes instead of the W ones.  For an ANSI-only
  // driver whose narrow path is UTF-8 (Apache Ignite): the Windows driver manager maps
  // a W call onto such a driver by transcoding the text through the ANSI code page, so
  // a literal 'héllo' reaches it as cp1252 bytes and matches nothing, while a narrow
  // call is handed over untouched (measured on Windows: the statement-literal step of
  // the compat workload fails wide and passes narrow).  Only the calls that carry
  // caller text take this route; the bridge's own ASCII probes stay wide.
  bool narrow_sql;
  // Driver quirk, the mirror image of narrow_sql and not Windows-only: send statement
  // text through the W entry points (SQLExecDirectW / SQLPrepareW) everywhere, because
  // this driver decodes narrow statement text as Latin-1 rather than UTF-8.
  //
  // Set for SAP HANA's libodbcHDB.so.  unixODBC hands a narrow `char*` to the driver as
  // the bytes it is, so on Linux every other driver here reads the bridge's UTF-8
  // statement text correctly -- but HANA's client takes each byte for one Latin-1
  // character and re-encodes it, and no connection property (CHAR_AS_UTF8, CHAR_SET,
  // charset) or locale (LC_ALL=C, en_US.UTF-8, C.UTF-8) changes it.  The UTF-8 bytes of
  // 'héllo 🚀' (68 c3a9 6c6c6f 20 f09f9a80) are stored as the eleven Latin-1 characters
  // those bytes spell, so a literal in statement text lands double-encoded and does not
  // match the same value sent as a bound parameter -- the exact corruption the Windows
  // driver manager causes for every driver, here caused by one driver on every platform.
  // Through SQLExecDirectW the same statement stores 'héllo 🚀' exactly.
  bool wide_sql;
  // Driver quirk: never call SQLDescribeParam (DuckDB aborts the process on it).
  bool no_describe_param;
  // Driver quirk: the driver describes a column as SQL_TYPE_TIMESTAMP but has no
  // TIMESTAMP_STRUCT conversion for it -- TDengine's taos-odbc fails SQLBindCol with
  // "Column converstion to `SQL_C_TYPE_TIMESTAMP` not implemented yet" (its bound
  // conversions are the numeric C types, SQL_C_CHAR, SQL_C_WCHAR and SQL_C_BINARY).
  // Read such a column as text and parse it, as the timestamp-with-timezone columns are
  // already read: the Arrow type stays a naive timestamp[us], and the column keeps its
  // place in the block cursor instead of dropping the whole result set to SQLGetData.
  //   The same driver property applies to parameters -- taos-odbc has no
  // SQL_C_TYPE_TIMESTAMP parameter conversion either -- so a timestamp parameter is
  // bound as "YYYY-MM-DD HH:MM:SS[.ffffff]" SQL_VARCHAR text instead, which is what the
  // sub-second TIME path already does for every driver.
  bool timestamp_as_text;
  // Driver quirk: never call SQLColumns -- nothing usable comes back from it, and not in
  // a way the return code reveals.  The Arrow Flight SQL ODBC driver returns SQL_SUCCESS
  // from SQLColumns and describes all 18 result columns, then segfaults inside the first
  // SQLFetch on that cursor -- with no bound columns at all -- whenever the request spans
  // a table whose Flight SQL schema has no per-field key-value metadata: every table
  // tried against sqlflite, and InfluxDB 3's information_schema and most system tables
  // (its iox tables fetch cleanly, and Dremio 26 does not crash at all).  Nothing in the
  // return code separates the cases.  psqlodbc against ArcadeDB
  // returns SQL_SUCCESS and an empty result set, because the pg_catalog query it builds
  // is one ArcadeDB's SQL parser rejects -- so every table would look like it has no
  // columns.  MySQL Connector/ODBC against the MongoDB BI Connector segfaults inside
  // SQLColumns itself, on a NULL NUMERIC_PRECISION that mongosqld's information_schema
  // reports for a DECIMAL column.  None of them leaves a return code to fall back on, so
  // the call has to be skipped: GetObjects describes "SELECT * FROM <table> WHERE 1=0"
  // instead, which is where GetTableSchema already gets a table's columns from.
  bool no_sql_columns;
  // Driver quirk: DDL type for a TIME column with fractional seconds, e.g. "Time64(%d)";
  // the %d takes the fractional digit count.  Used for drivers whose SQLGetTypeInfo TIME
  // type is whole-second and takes no CREATE_PARAMS, so nothing in the ODBC metadata can
  // ask for a sub-second column.  NULL means "use SQLGetTypeInfo".
  const char* fractional_time_type_format;
  // Largest fractional digit count fractional_time_type_format accepts (0 = no limit).
  int fractional_time_max_digits;
  // Driver quirk: column-wise parameter arrays (SQL_ATTR_PARAMSET_SIZE) are accepted
  // but only partly executed, so every bound row gets its own execute instead
  // (DuckDB, clickhouse-odbc, Firebird's OdbcFb).
  bool no_param_arrays;
  // Driver quirk: as no_param_arrays, but only for a batch that binds a timestamp --
  // every other batch keeps its parameter array.  psqlodbc executes a parameter array by
  // inlining the values into one statement, where a SQL_TYPE_TIMESTAMP parameter becomes
  // '2024-02-29 13:45:10.123456'::timestamp; a server whose only timestamp type carries a
  // zone (Google Cloud Spanner, whose PostgreSQL dialect has no TIMESTAMP WITHOUT TIME
  // ZONE) refuses that cast and fails the whole batch.  Sent one row at a time the same
  // value goes as a typed parameter, which such a server converts to its own timestamp.
  bool no_timestamp_param_arrays;
  // SQL_PARAM_ARRAY_ROW_COUNTS: SQL_PARC_NO_BATCH means one cumulative SQLRowCount for
  // the whole parameter array; SQL_PARC_BATCH means one row count per parameter set,
  // walked with SQLMoreResults.  Defaults to SQL_PARC_BATCH, which is safe either way.
  int param_array_row_counts;
  // SQL_TXN_CAPABLE said something other than SQL_TC_NONE, so turning autocommit off
  // around a multi-row execute is worth trying.
  bool txn_capable;
  // Driver quirk: the server has no `INSERT INTO t VALUES (...),(...)`, but does have
  // Oracle's `INSERT ALL INTO t VALUES (...) INTO t VALUES (...) SELECT 1 FROM dual`.
  // Only consulted after the standard multi-row form has actually been refused, so it
  // costs a server that takes the standard form nothing.
  bool multirow_insert_all;
  // Driver quirk: the server has neither of the two forms above, but does take
  // `INSERT INTO t (cols) SELECT <typed>, ... FROM <this> UNION ALL SELECT ...` --
  // where <this> is the one-row table named here (Firebird's `RDB$DATABASE`).  The
  // select list has to give every parameter a type, so the form is only offered when
  // every bound column has an exact SQL type name (see MultiRowCastTypes).  Only
  // consulted after the two forms above have been refused.
  const char* multirow_union_from;
  // Driver quirk: keep ODBC parameter arrays ahead of multi-row INSERT batching for bulk
  // ingest.  Multi-row INSERT is the default because it was faster on every server
  // measured (see ExecuteRows), including most of the ones whose arrays work.  Three
  // drivers are the exception and are all that set this: MariaDB Connector/ODBC, which
  // turns a bound array into one COM_STMT_BULK_EXECUTE, Vertica's own client driver,
  // which turns one into a native bulk load, and Altibase's, which applies the array in
  // a single round trip (30k rows/s multi-row against ~800k as an array).
  bool prefer_param_arrays;
  // Server quirk: bulk ingest may send a whole batch as one array parameter per column
  // and let the server expand it --
  //   INSERT INTO t ("a", "b") SELECT * FROM unnest(?::bigint[], ?::text[])
  // -- instead of K*ncols separately bound cells.  Set only for PostgreSQL itself, from
  // the psqlodbc block of OdbcDetectQuirks: several servers speak the PostgreSQL wire
  // protocol without being PostgreSQL, and multi-argument unnest with those casts is not
  // theirs to promise.  Before the form is used on a connection it is also *verified*
  // there, by a single statement whose answer pins down NULLs, empty strings and
  // separator quoting (ArrayIngestServerOk); anything that answers differently keeps the
  // multi-row INSERT path.
  bool pg_array_ingest;
  // Driver quirk: spell an unbounded Arrow string column as the driver's widest VARCHAR
  // in generated ingest DDL, rather than as its SQL_LONGVARCHAR type.
  //
  // Set for IBM's CLI driver, whose SQL_LONGVARCHAR is Db2's LONG VARCHAR -- a type IBM
  // deprecated in Db2 9 and never gave a bulk-insert path.  Writing 20,000 rows of
  // (INTEGER, DOUBLE, <string>, DATE) into one, medians of 3 straight through the ODBC
  // API with no adbcbridge in the way: 737 rows/s as LONG VARCHAR against 516,459 as
  // VARCHAR(32672), 429,865 as VARCHAR(20) and 402,356 as CLOB(1M).  Every other string
  // type Db2 has is within 25% of the fastest; LONG VARCHAR alone is 700x off it, and
  // it is what SQLGetTypeInfo(SQL_LONGVARCHAR) names, so the generated DDL picked it.
  //
  // The width is the maximum the driver reports for SQL_VARCHAR (32,672 bytes on Db2),
  // so the column still holds all but the last 28 bytes of what LONG VARCHAR could, and
  // -- unlike LONG VARCHAR, which Db2 bars from indexes, ORDER BY, GROUP BY and
  // DISTINCT -- an ordinary VARCHAR column can be used for all of them.
  bool ddl_string_as_max_varchar;
  // Driver quirk: the literal DDL type to give an unbounded Arrow string column, for a
  // server whose SQL_LONGVARCHAR type is one it barely supports and whose replacement
  // cannot be spelled from SQLGetTypeInfo (its width is the word "MAX", not a number).
  //
  // Set for msodbcsql, whose SQL_LONGVARCHAR is SQL Server's TEXT.  Microsoft deprecated
  // TEXT, NTEXT and IMAGE in SQL Server 2005 and documents NVARCHAR(MAX) as the
  // replacement, and a TEXT column is close to unusable: ORDER BY and GROUP BY on one
  // are error 306, SELECT DISTINCT is 421, and even `WHERE s = 'a'` is 402, "the data
  // types text and varchar are incompatible".  So a table adbcbridge created could not
  // be sorted, grouped, de-duplicated or filtered on its own string column.
  // NVARCHAR(MAX) holds the same 2 GB, is Unicode rather than the database's code page,
  // and is faster both ways: 20,000-row ingest 172,081 rows/s as TEXT against 233,303 as
  // NVARCHAR(MAX), and a 100,000-row read 859,215 rows/s against 3,172,747 (medians of
  // 5, interleaved).
  const char* ddl_string_type_name;
  // Driver quirk: the literal DDL type to give an Arrow timestamp column, for a driver
  // whose SQLGetTypeInfo(SQL_TYPE_TIMESTAMP) names a whole-second type in its first row
  // -- the row generated ingest DDL reads -- and its sub-second type only further down.
  //
  // Set for SAP HANA's libodbcHDB.so, whose first row is SECONDDATE (COLUMN_SIZE 19,
  // MAXIMUM_SCALE 0, no CREATE_PARAMS) while TIMESTAMP, which holds 7 fractional
  // digits, is the second and third.  Nothing in the ODBC metadata distinguishes them:
  // SECONDDATE takes no CREATE_PARAMS, so fractional_time_type_format's route -- ask
  // for a scale -- has nothing to ask, and HANA's TIMESTAMP takes no precision argument
  // either ("TIMESTAMP(6)" is 42000/257, "incorrect syntax near ("), so the replacement
  // is a fixed name rather than a format.  Without it an Arrow timestamp[us] column
  // became SECONDDATE and every microsecond was silently dropped on ingest.
  const char* ddl_timestamp_type_name;
  // SQL_MAX_STATEMENT_LEN, in bytes; 0 when the driver will not say.
  int64_t max_statement_len;
  // Server quirk: a hard ceiling on the number of parameters one statement may carry.
  // 0 (the default) means "not known", and multi-row INSERT batching then finds the
  // ceiling by halving K until SQLPrepare stops refusing -- which is how every ODBC
  // driver is asked, there being no SQL_MAX_PARAMETERS to read.  Set this only for a
  // server whose ceiling that search cannot find: Cloud Spanner accepts the oversized
  // statement at prepare and, at execute, *drops the connection* (SQLSTATE 08S01), so
  // by the time the halving would run there is nothing left to halve on.
  int64_t max_statement_params;
  // Rowsets kept in flight on a background fetch thread (0 = off, the default).  See
  // ADBC_ODBC_OPTION_PREFETCH and the prefetch section of src/odbc_reader.c.
  int64_t prefetch;
};

// --- 32-bit-SQLLEN driver quirk accessors -----------------------------------
// All of these are exact no-ops (a plain load/store) when the quirk is off, so the
// normal path costs nothing beyond a predictable branch.

/// Read a scalar SQLLEN out-parameter the driver wrote.  The driver writes four bytes
/// at the variable's address regardless of endianness, so the low half is at offset 0.
/// The caller must zero the variable before the ODBC call.
static inline SQLLEN OdbcReadLen(const SQLLEN* p, bool sqllen_32bit) {
  if (!sqllen_32bit) return *p;
  int32_t v;
  memcpy(&v, p, sizeof(v));
  return (SQLLEN)v;  // sign-extends SQL_NULL_DATA (-1), SQL_NO_TOTAL (-4), ...
}

/// Read a scalar SQLULEN out-parameter the driver wrote (column size, rows fetched,
/// parameter sets processed).  The caller must zero the variable before the ODBC call.
static inline SQLULEN OdbcReadULen(const SQLULEN* p, bool sqllen_32bit) {
  if (!sqllen_32bit) return *p;
  uint32_t v;
  memcpy(&v, p, sizeof(v));
  return (SQLULEN)v;
}

/// Read element `row` of an indicator/length array the driver wrote through SQLBindCol.
/// A 32-bit-SQLLEN driver fills it as int32[] with stride 4.
static inline SQLLEN OdbcIndicatorGet(const SQLLEN* base, size_t row, bool sqllen_32bit) {
  if (!sqllen_32bit) return base[row];
  int32_t v;
  memcpy((char*)&v, (const char*)base + row * sizeof(int32_t), sizeof(v));
  return (SQLLEN)v;
}

/// Write element `row` of an indicator/length array the driver will read through
/// SQLBindParameter with SQL_ATTR_PARAMSET_SIZE > 1 (same stride rule).
static inline void OdbcIndicatorSet(SQLLEN* base, size_t row, SQLLEN value, bool sqllen_32bit) {
  if (!sqllen_32bit) {
    base[row] = value;
    return;
  }
  int32_t v = (int32_t)value;
  memcpy((char*)base + row * sizeof(int32_t), &v, sizeof(v));
}

/// SQLRowCount, honouring the quirk; -1 when the driver cannot answer.
SQLLEN OdbcRowCount(SQLHSTMT hstmt, bool sqllen_32bit);

/// SQLGetData with a quirk-aware StrLen_or_Ind out-parameter.
// SQLGetData with the indicator read at the driver's SQLLEN width.  static inline so
// the unit tests, which include a single translation unit, can use it too.
static inline SQLRETURN OdbcGetData(SQLHSTMT hstmt, SQLUSMALLINT col, SQLSMALLINT c_type,
                                    SQLPOINTER buf, SQLLEN buf_len, SQLLEN* indicator,
                                    bool sqllen_32bit) {
  SQLLEN ind = 0;
  SQLRETURN ret = SQLGetData(hstmt, col, c_type, buf, buf_len, &ind);
  if (indicator) *indicator = OdbcReadLen(&ind, sqllen_32bit);
  return ret;
}

struct OdbcDatabase;
struct OdbcPreOption;  // odbc_delegate.h
struct OdbcProxyConnection;
struct OdbcProxyStatement;

struct OdbcConnection {
  struct OdbcDatabase* db;
  // Non-NULL when a native ADBC driver serves this connection: every call is
  // forwarded to it and none of the ODBC state below is used.
  struct OdbcProxyConnection* proxy;
  // Options set before AdbcConnectionInit, kept so that they can be replayed on
  // the native connection when the database turns out to be delegated.
  struct OdbcPreOption* pre;
  size_t pre_count;
  // The first of those options that the ODBC path itself did not understand: it
  // is only an error once the connection is known to be served by ODBC.
  char* held_option;
  SQLHDBC hdbc;
  bool connected;
  bool autocommit;
  // Bumped by every rollback.  A statement whose handle was last used before one has to
  // take a fresh handle: see OdbcStatementEnsureHandle.
  uint64_t rollback_epoch;
  // Multi-row INSERT ingest batching, learned once per connection (see
  // ADBC_ODBC_OPTION_ROWS_PER_INSERT).  The probe is a two-row SQLPrepare against the
  // ingest target: whether the server takes the form at all is a property of the server,
  // not of the table, so the verdict is cached here rather than paid per statement.
  bool multirow_probed;      // the form probe has run
  bool multirow_unsupported; // ... and the server takes no form at all: never try again
  int multirow_form;         // ... and this is the form it took (enum OdbcMultiRowForm)
  // Largest parameter count an INSERT on this connection was seen to prepare, discovered
  // by halving; 0 until something has actually been refused.
  int64_t multirow_max_params;
  // Array ingest (reader_opts.pg_array_ingest), verified once per connection: whether
  // this server really expands a multi-argument unnest of array literals the way
  // PostgreSQL does.  The check is one small statement, run on the first bulk ingest
  // rather than on connect, so a connection that never ingests never pays for it.
  bool array_ingest_probed;      // the semantic check has run
  bool array_ingest_unsupported; // ... and the form is not usable here: never try again
  struct OdbcReaderOptions reader_opts;
};

struct OdbcStatement {
  struct OdbcConnection* conn;
  // Non-NULL when a native ADBC driver serves this statement (see OdbcConnection).
  struct OdbcProxyStatement* proxy;
  struct OdbcHandleRef* ref;
  // OdbcConnection::rollback_epoch as of the last time `ref` was made ready.
  uint64_t rollback_epoch;
  char* query;
  bool prepared;           // SQLPrepare has been issued for `query` on `ref->hstmt`
  bool prepare_requested;  // AdbcStatementPrepare was called; SQLPrepare is deferred
  bool executed;           // `query` has been executed at least once since it was set
  struct OdbcReaderOptions reader_opts;
  // Bound parameters (Bind / BindStream)
  struct ArrowArrayStream bind_stream;
  bool has_bind;
  /// Try column-wise parameter arrays before falling back to row-at-a-time.
  bool array_binding;
  /// Rows of parameters per INSERT for bulk ingest; 0 = automatic, 1 = disabled.
  int64_t rows_per_insert;
  /// Connections a bulk ingest may spread itself over; 1 (the default) = the caller's
  /// own connection alone, and the only value that keeps the ingest atomic.  See
  /// ADBC_ODBC_OPTION_INGEST_CONNECTIONS.
  int64_t ingest_connections;
  /// Partitions ExecutePartitions should aim for; 0 = automatic, 1 = never split.
  int64_t partitions;
  // Bulk ingest
  char* ingest_table;
  char* ingest_catalog;
  char* ingest_schema;
  char* ingest_mode;  // ADBC_INGEST_OPTION_MODE_* value
  bool ingest_temporary;
  // `"schema"."table" ("a", "b")` -- the part of the generated INSERT that a multi-row
  // form repeats.  Set by OdbcStatementIngest only, so the multi-row rewrite can never
  // reach a query the caller wrote themselves.
  char* ingest_into;
};

/// Fetch the identifier quote character ('"' default, '\0' if none) into out[8].
void OdbcQuoteChar(SQLHDBC hdbc, char* out);

/// UTF-8 -> UTF-16 into `o`, NUL-terminating it; returns the units written.  A UTF-8
/// byte never produces more than one UTF-16 unit (a 4-byte sequence becomes a two-unit
/// surrogate pair), so `n + 1` units is always enough room.  Malformed input becomes
/// U+FFFD.  Used for the wide parameter path and for the wide connect retry.
// `utf16_pairs`: with a four-byte SQLWCHAR, still write a non-BMP character as a UTF-16
// surrogate pair (two units) instead of one code point -- what some drivers built for
// iODBC expect (see OdbcReaderOptions::wide_utf16_pairs).  Ignored when SQLWCHAR is two
// bytes, where pairs are the only encoding.
int64_t OdbcUtf8ToUtf16Into(SQLWCHAR* o, const char* s, int64_t n, bool utf16_pairs);

// Text-carrying ODBC calls in UTF-8 (src/odbc_text.c).  The narrow entry points on
// unixODBC/iODBC, the W entry points with conversion on Windows, whose driver manager
// would otherwise read narrow text as the ANSI code page.  A NULL name argument is
// passed through as NULL; a length is SQL_NTS or a byte count.
SQLRETURN OdbcExecDirectUtf8(SQLHSTMT hstmt, const char* sql);
SQLRETURN OdbcPrepareUtf8(SQLHSTMT hstmt, const char* sql);
// The same two with the per-connection choice of entry point: `narrow` is
// OdbcReaderOptions::narrow_sql, and only means something on Windows (the narrow
// calls are the only ones elsewhere).  Statement text that came from the caller goes
// through these; the two-argument forms above are for the bridge's own ASCII probes.
// Statement text from the caller, routed by this connection's narrow_sql / wide_sql
// quirks; `opts` may be NULL, which means the platform default.
SQLRETURN OdbcExecDirectSql(SQLHSTMT hstmt, const char* sql, const struct OdbcReaderOptions* opts);
SQLRETURN OdbcPrepareSql(SQLHSTMT hstmt, const char* sql, const struct OdbcReaderOptions* opts);
SQLRETURN OdbcDescribeColUtf8(SQLHSTMT hstmt, SQLUSMALLINT col, char* name, SQLSMALLINT name_cap,
                              SQLSMALLINT* name_len, SQLSMALLINT* type, SQLULEN* size,
                              SQLSMALLINT* digits, SQLSMALLINT* nullable);
SQLRETURN OdbcColAttributeStrUtf8(SQLHSTMT hstmt, SQLUSMALLINT col, SQLUSMALLINT field,
                                  char* buf, SQLSMALLINT cap, SQLSMALLINT* len);
SQLRETURN OdbcGetDiagRecUtf8(SQLSMALLINT handle_type, SQLHANDLE handle, SQLSMALLINT rec,
                             SQLCHAR* state, SQLINTEGER* native, char* msg, SQLSMALLINT cap,
                             SQLSMALLINT* len);
SQLRETURN OdbcTablesUtf8(SQLHSTMT hstmt, const char* cat, SQLSMALLINT cat_len, const char* sch,
                         SQLSMALLINT sch_len, const char* tbl, SQLSMALLINT tbl_len,
                         const char* type, SQLSMALLINT type_len);
SQLRETURN OdbcColumnsUtf8(SQLHSTMT hstmt, const char* cat, SQLSMALLINT cat_len, const char* sch,
                          SQLSMALLINT sch_len, const char* tbl, SQLSMALLINT tbl_len,
                          const char* col, SQLSMALLINT col_len);
SQLRETURN OdbcPrimaryKeysUtf8(SQLHSTMT hstmt, const char* cat, SQLSMALLINT cat_len,
                              const char* sch, SQLSMALLINT sch_len, const char* tbl,
                              SQLSMALLINT tbl_len);
// A character column read with SQLGetData, as UTF-8: SQL_C_CHAR on unixODBC/iODBC,
// SQL_C_WCHAR converted on Windows.  `ind` gets SQL_NULL_DATA or the UTF-8 byte length.
SQLRETURN OdbcGetDataStrUtf8(SQLHSTMT hstmt, SQLUSMALLINT col, char* buf, size_t cap,
                             SQLLEN* ind, bool sqllen_32bit);
SQLRETURN OdbcForeignKeysUtf8(SQLHSTMT hstmt, const char* fcat, SQLSMALLINT fcat_len,
                              const char* fsch, SQLSMALLINT fsch_len, const char* ftbl,
                              SQLSMALLINT ftbl_len);

AdbcStatusCode OdbcStatementEnsureHandle(struct OdbcStatement* stmt, struct AdbcError* error);

/// Execute stmt->query once per bound row (parameters from bind_stream).
AdbcStatusCode OdbcStatementExecuteBound(struct OdbcStatement* stmt, struct ArrowArrayStream* out,
                                         int64_t* rows_affected, struct AdbcError* error);

/// Bulk ingest bind_stream into stmt->ingest_table.
AdbcStatusCode OdbcStatementIngest(struct OdbcStatement* stmt, int64_t* rows_affected,
                                   struct AdbcError* error);

/// Open one more ODBC connection to the database `db` already describes, with the same
/// connection string every other connection to it uses.  The caller owns the handle and
/// must SQLDisconnect + SQLFreeHandle it.  Used by parallel bulk ingest, which needs a
/// connection per worker; OdbcConnectionInit is the other caller.
AdbcStatusCode OdbcOpenHdbc(struct OdbcDatabase* db, SQLHDBC* out, struct AdbcError* error);

AdbcStatusCode OdbcConnectionGetObjects(struct AdbcConnection* connection, int depth,
                                        const char* catalog, const char* db_schema,
                                        const char* table_name, const char** table_type,
                                        const char* column_name, struct ArrowArrayStream* out,
                                        struct AdbcError* error);

/// Describe the result set of an executed/prepared statement as an Arrow schema.
AdbcStatusCode OdbcDescribeResultSchema(SQLHSTMT hstmt, const struct OdbcReaderOptions* opts,
                                        struct ArrowSchema* out, struct AdbcError* error);

/// Describe the parameters of a prepared statement as an Arrow struct schema
/// (SQLNumParams + SQLDescribeParam).  Drivers that cannot describe parameters get
/// N nullable utf8 fields named "0".."N-1".
AdbcStatusCode OdbcDescribeParameterSchema(SQLHSTMT hstmt, const struct OdbcReaderOptions* opts,
                                           struct ArrowSchema* out, struct AdbcError* error);

/// Split stmt->query into partition descriptors (src/odbc_partition.c).
AdbcStatusCode OdbcStatementExecutePartitionsOdbc(struct OdbcStatement* stmt,
                                                  struct ArrowSchema* schema,
                                                  struct AdbcPartitions* partitions,
                                                  int64_t* rows_affected,
                                                  struct AdbcError* error);

/// Execute one partition descriptor on `conn` and stream its rows.
AdbcStatusCode OdbcConnectionReadPartitionOdbc(struct OdbcConnection* conn,
                                               const uint8_t* serialized_partition,
                                               size_t serialized_length,
                                               struct ArrowArrayStream* out,
                                               struct AdbcError* error);

/// Build an ArrowArrayStream that fetches from an executed statement.
/// Takes a reference on `ref`.
AdbcStatusCode OdbcReaderInit(struct OdbcHandleRef* ref, const struct OdbcReaderOptions* opts,
                              struct ArrowArrayStream* out, struct AdbcError* error);
