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
/// Force the 32-bit-SQLLEN driver quirk on or off ("true"/"false").  Unset means
/// autodetect from SQL_DRIVER_NAME; see OdbcReaderOptions::sqllen_32bit.
#define ADBC_ODBC_OPTION_SQLLEN_32BIT "adbc.odbc.sqllen_32bit"
/// Read-only: SQL_DRIVER_NAME of the underlying ODBC driver.  ADBC_INFO_DRIVER_NAME
/// must be a stable identity for adbcbridge itself, so the backing driver's file
/// name is exposed here (and, as context, in ADBC_INFO_VENDOR_NAME) instead.
#define ADBC_ODBC_OPTION_DRIVER_NAME "adbc.odbc.driver_name"
/// Read-only: SQL_DRIVER_NAME of the underlying ODBC driver.  ADBC_INFO_DRIVER_NAME
/// must be a stable identity for adbcbridge itself, so the backing driver's file
/// name is exposed here (and, as context, in ADBC_INFO_VENDOR_NAME) instead.

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
  // Driver quirk: bind boolean parameters as integers (DuckDB rejects SQL_BIT params).
  bool bool_param_as_int;
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
  // Driver quirk: the names SQLGetTypeInfo reports are not names the server accepts in
  // DDL, so bulk ingest spells its CREATE TABLE with portable SQL type names (BIGINT,
  // DOUBLE PRECISION, BOOLEAN, ...) instead.  psqlodbc drives every PostgreSQL-wire
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
  // Driver quirk: the driver's SQLWCHAR is wchar_t (4 bytes on Linux) while unixODBC
  // passes UTF-16 (Firebird OdbcFb): a bound SQL_C_WCHAR parameter is truncated to
  // byte_length/4 characters ("héllo 🚀" stores as "héll") and fetched wide columns come
  // back as UTF-32. Use the narrow SQL_C_CHAR path instead, which is UTF-8 when the
  // connection is opened with CHARSET=UTF8.
  bool wchar_as_utf8;
  // Driver quirk: never call SQLDescribeParam (DuckDB aborts the process on it).
  bool no_describe_param;
  // Driver quirk: never call SQLColumns -- nothing usable comes back from it, and not in
  // a way the return code reveals.  The Arrow Flight SQL ODBC driver returns SQL_SUCCESS
  // from SQLColumns and describes all 18 result columns, then segfaults inside the first
  // SQLFetch on that cursor, with no bound columns at all.  psqlodbc against ArcadeDB
  // returns SQL_SUCCESS and an empty result set, because the pg_catalog query it builds
  // is one ArcadeDB's SQL parser rejects -- so every table would look like it has no
  // columns.  Neither leaves a return code to fall back on, so the call has to be
  // skipped: GetObjects describes "SELECT * FROM <table> WHERE 1=0" instead, which is
  // where GetTableSchema already gets a table's columns from.
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
  // Driver quirk: keep ODBC parameter arrays ahead of multi-row INSERT batching for bulk
  // ingest.  Multi-row INSERT is the default because it was faster on every server
  // measured (see ExecuteRows), including most of the ones whose arrays work; MariaDB
  // Connector/ODBC, which turns a bound array into one COM_STMT_BULK_EXECUTE, is the
  // exception and the only thing that sets this.
  bool prefer_param_arrays;
  // SQL_MAX_STATEMENT_LEN, in bytes; 0 when the driver will not say.
  int64_t max_statement_len;
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
  // Multi-row INSERT ingest batching, learned once per connection (see
  // ADBC_ODBC_OPTION_ROWS_PER_INSERT).  The probe is a two-row SQLPrepare against the
  // ingest target: whether the server takes the form at all is a property of the server,
  // not of the table, so the verdict is cached here rather than paid per statement.
  bool multirow_probed;      // the form probe has run
  bool multirow_unsupported; // ... and the server takes neither form: never try again
  bool multirow_insert_all;  // ... and the form it takes is Oracle's INSERT ALL
  // Largest parameter count an INSERT on this connection was seen to prepare, discovered
  // by halving; 0 until something has actually been refused.
  int64_t multirow_max_params;
  struct OdbcReaderOptions reader_opts;
};

struct OdbcStatement {
  struct OdbcConnection* conn;
  // Non-NULL when a native ADBC driver serves this statement (see OdbcConnection).
  struct OdbcProxyStatement* proxy;
  struct OdbcHandleRef* ref;
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

AdbcStatusCode OdbcStatementEnsureHandle(struct OdbcStatement* stmt, struct AdbcError* error);

/// Execute stmt->query once per bound row (parameters from bind_stream).
AdbcStatusCode OdbcStatementExecuteBound(struct OdbcStatement* stmt, struct ArrowArrayStream* out,
                                         int64_t* rows_affected, struct AdbcError* error);

/// Bulk ingest bind_stream into stmt->ingest_table.
AdbcStatusCode OdbcStatementIngest(struct OdbcStatement* stmt, int64_t* rows_affected,
                                   struct AdbcError* error);

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

/// Build an ArrowArrayStream that fetches from an executed statement.
/// Takes a reference on `ref`.
AdbcStatusCode OdbcReaderInit(struct OdbcHandleRef* ref, const struct OdbcReaderOptions* opts,
                              struct ArrowArrayStream* out, struct AdbcError* error);
