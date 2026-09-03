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

// Partitioned result sets (AdbcStatementExecutePartitions /
// AdbcConnectionReadPartition) over ODBC.
//
// ODBC has no notion of a partitioned result, and no ODBC driver will split a query
// for us.  What ADBC's partition contract actually asks for, though, is only this:
// hand back N opaque descriptors such that reading every one of them -- on any
// connection, in any order, concurrently -- yields the same rows as the unpartitioned
// query, with the same schema.  A descriptor is therefore just *a query*: the original
// SELECT narrowed to a disjoint slice of the table, which N connections then run in
// parallel against a server that has N cores and one client that had one.
//
// That is the entire mechanism, and it is also its entire limitation: the split has to
// be one this driver can prove is exact, from SQL text alone plus one metadata query.
// Anything less than proof falls back to a single partition carrying the original
// query verbatim, which is always correct and never slower than not calling
// ExecutePartitions at all.
//
// Every strategy below produces the same shape of split: an *ordered expression* over
// the table's rows, and K-1 boundary values that cut its range into K half-open
// intervals.  Slice k is the original query with
//
//     WHERE <expr> >= <boundary k> AND <expr> < <boundary k+1>
//
// AND-ed on, the first slice left unbounded below and the last unbounded above.  Two
// properties make that exact for any expression and any boundaries: half-open
// intervals put a row that lands exactly on a boundary in one slice and only one, so
// duplicate values cannot be doubled or dropped; and the unbounded ends mean the union
// of the slices is the whole range of the expression, so a boundary list computed from
// stale metadata costs balance but never rows.  The one thing a strategy must supply
// is that `<expr>` is never NULL for a row of the table -- a NULL compares false
// against every bound and would vanish from all K slices.
//
// --- Strategy 1: the PostgreSQL heap split (`ctid`) ---------------------------------
//
// Every PostgreSQL heap tuple has a `ctid` of (block, offset), and `tid` compares
// lexicographically, so the table's blocks are a total order that partitions the heap
// with no index, no key column and no sort.  Slice k of K over a table of P blocks is
//
//     WHERE ctid >= '(lo,0)'::tid AND ctid < '(hi,0)'::tid
//
// Offsets are 1-based, so '(N,0)' names a point strictly below every real tuple in
// block N: the boundary lands *between* blocks and no tuple can fall on both sides or
// on neither.  PostgreSQL 14+ turns this into a TID Range Scan, so a slice reads only
// its own blocks off disk rather than filtering a full sequential scan.  `ctid` is
// never NULL.
//
// This is the best split where it exists: it needs no index, it is exactly balanced in
// bytes rather than in key values, and it reads sequentially.  It is also the least
// portable -- `ctid` is a heap detail, and the PostgreSQL-wire servers that do not
// store their tables in a heap do not have it.  CockroachDB answers `SELECT ctid` with
// 42703, YugabyteDB with 0A000.
//
// --- Strategy 2: the key-range split ------------------------------------------------
//
// Where there is no heap, there is nearly always a primary key, and a primary key is
// indexed, ordered and NOT NULL -- exactly the three things the split shape asks for.
// So: take the leading primary-key column, ask the server for its MIN and MAX, and cut
// [min, max] into K intervals.
//
//     WHERE "id" >= 125001 AND "id" < 250001
//
// The whole gate is built out of ODBC catalog calls (SQLPrimaryKeys, SQLColumns), not
// out of any one server's system tables, so it reaches every database this driver can
// reach and not only the PostgreSQL-wire ones.  What it demands before it will split:
//
//   * exactly one table behind the name, with a primary key;
//   * a leading (KEY_SEQ 1) primary-key column whose SQLColumns row says it is an
//     integer type and SQL_NO_NULLS.  The NOT NULL is load-bearing, not decorative --
//     see the note about NULL above -- so it is read from the catalog rather than
//     assumed from "it is a primary key";
//   * a name made only of identifier characters, so that quoting it is unambiguous.
//
// Anything else is a single partition.  The split is *correct* for any NOT NULL
// ordered column, indexed or not; the primary key is required because an unindexed
// range predicate makes each slice scan the whole table, which is K times the server
// work for one client's worth of rows.  That is the trap in a modulo-or-hash split
// (`WHERE mod(hash(k), K) = i`), which balances perfectly and is never index-usable,
// and it is why this driver does not offer one.  `LIMIT`/`OFFSET` is worse still: the
// server must produce and discard OFFSET rows per slice, so the total work is
// quadratic, and without a total order the pages are not even stable between slices.
//
// Balance comes from assuming the key is roughly uniform over [min, max].  A skewed
// key gives lopsided slices -- correct, but not much faster.  Getting better than that
// needs a quantile probe, which costs a scan of the index and is not worth it against
// the read it is trying to speed up.
//
// --- Strategy 3: YugabyteDB's tablet hash -------------------------------------------
//
// YugabyteDB's default `PRIMARY KEY (id)` is *hash*-partitioned, not ordered, and this
// matters more than it sounds: `WHERE id >= a AND id < b` against a hash-partitioned
// key is not an index condition at all, it is a `Storage Filter` over a `Seq Scan`.
// A key-range split there would be correct and K times slower than not splitting.
//
// So for that case the ordered expression is the tablet hash itself, which YugabyteDB
// exposes as `yb_hash_code(...)` -- an index condition on the primary index, over the
// fixed range [0, 65536):
//
//     WHERE yb_hash_code("id") >= 8192 AND yb_hash_code("id") < 16384
//
// It is used only when the *whole* hash-key set is the single column being hashed; on
// `PRIMARY KEY ((a, b) HASH)`, `yb_hash_code(a)` is not the tablet hash and would fall
// back to a filter.
//
// The discriminator for all of this is `pg_index.indoption`, where YugabyteDB records
// hash partitioning as bit 0x4 (stock PostgreSQL uses only 0x1 DESC and 0x2 NULLS
// FIRST).  On any PostgreSQL-wire server the driver reads that bit before it will emit
// a key-range predicate, and refuses to split at all if it cannot read it -- a wrong
// answer here costs a factor of K, so it is not guessed either.
//
// --- What is deliberately not attempted ---------------------------------------------
//
// The query must be a bare `SELECT <list> FROM <one table>`: no WHERE, no JOIN, no
// ORDER BY, no aggregate, not even a parenthesis (which rules out subqueries and
// function calls in one stroke).  A query with a WHERE clause could be split safely by
// AND-ing the ctid predicate on, but `SELECT ... FROM t WHERE ...` is not reliably
// distinguishable from the many shapes that cannot be split without re-implementing a
// SQL parser, and a wrong answer here is silent data loss rather than an error.  So the
// rule is the narrow one, and everything else gets one partition.
//
// Views and foreign tables need no special case: they have no heap of their own --
// `pg_relation_size` reports 0 blocks -- and no primary key either (PostgreSQL rejects
// one on a foreign table outright, 0A000), so SQLPrimaryKeys returns nothing and both
// strategies decline them.  A declaratively partitioned parent reports 0 blocks too, so
// the heap split declines it for the same reason -- but not for want of a key: a parent
// can carry a primary key of its own and SQLPrimaryKeys returns it, so when that key
// leads with an integer NOT NULL column the parent takes the key-range split; a parent
// with no primary key, or one whose key leads with the partitioning column (PRIMARY KEY
// (ts, id) on a time-ranged table, since PostgreSQL requires the partition key inside
// the primary key), still gets one partition.  It is the zero block count and not an
// error that rules the heap split out: `SELECT ctid` succeeds on a partitioned parent
// (partition-local ctids that repeat) and on a foreign table; only a view rejects the
// column, 42703.  A materialized view has a heap of its own and takes the ctid split.
//
// Nothing here ever *guesses* a partition column.  Every strategy either proves its
// expression is total and NOT NULL over the table from the catalog, or hands back one
// partition.  Returning the wrong rows quickly is much worse than the right rows
// slowly, and a single partition is exactly as fast as not calling ExecutePartitions.
//
// --- Snapshot semantics -------------------------------------------------------------
//
// Partitions are read on different connections and therefore under different
// snapshots.  Against a table being modified concurrently, the union of the slices is
// not a point-in-time view of the table -- that is inherent to ADBC's partition model,
// which has nowhere to carry a shared snapshot, and is why partitioning is opt-in.

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
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

#include "odbc_internal.h"
#include "utils.h"

// Descriptor wire format: a magic + version prefix, then the slice's SQL text.  Text
// rather than a packed struct because a descriptor may cross a process boundary and
// because a query is the only thing it has to carry; the magic is checked so that a
// descriptor from some other driver is rejected rather than executed.
#define ODBC_PARTITION_MAGIC "ADBCODBCPART\x01\n"
#define ODBC_PARTITION_MAGIC_LEN (sizeof(ODBC_PARTITION_MAGIC) - 1)

// Auto partition count: one partition per this many heap blocks (8 KiB each), i.e. one
// per 64 MiB of table.  Below that, a second connection costs more (connect, plan,
// snapshot) than the halved scan saves.
#define ODBC_PARTITION_AUTO_BLOCKS 8192
#define ODBC_PARTITION_AUTO_MAX 8
// The same 64 MiB, counted in rows instead, for the strategies that have a row
// estimate rather than a block count.  A four-column benchmark row is ~70 bytes on
// disk, so a million of them is about one auto partition's worth.
#define ODBC_PARTITION_AUTO_ROWS 1000000
// Refuse to hand out an unbounded number of connections' worth of work.
#define ODBC_PARTITION_MAX 256

// Longest identifier the split will carry.  ODBC's own catalog columns are 128 for
// ODBC 3.x; anything longer is simply not split.
#define ODBC_PARTITION_IDENT_MAX 128

// --- Tiny SQL scanner ---------------------------------------------------------------
// Only enough to recognise the one shape that can be split; see the header comment.

static bool IsIdentStart(char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}

static bool IsIdentChar(char c) {
  return IsIdentStart(c) || (c >= '0' && c <= '9') || c == '$';
}

static bool IsSpace(char c) {
  return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f';
}

static size_t SkipSpace(const char* s, size_t n, size_t i) {
  while (i < n && IsSpace(s[i])) i++;
  return i;
}

// Case-insensitive compare of the word at `i` against `word`, requiring a word boundary
// on both sides.  Returns the index just past the word, or 0 if it does not match.
static size_t MatchWord(const char* s, size_t n, size_t i, const char* word) {
  size_t k = 0;
  while (word[k]) {
    if (i + k >= n) return 0;
    char a = s[i + k];
    if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
    char b = word[k];
    if (b >= 'A' && b <= 'Z') b = (char)(b - 'A' + 'a');
    if (a != b) return 0;
    k++;
  }
  if (i + k < n && IsIdentChar(s[i + k])) return 0;
  return i + k;
}

// Words whose presence anywhere in the statement means the query is not the bare
// single-table SELECT that can be sliced.  `from` and `select` are handled separately.
static const char* const kUnsplittableWords[] = {
    "where",  "join",   "group",     "order",       "limit",    "offset", "union",
    "except", "having", "window",    "fetch",       "for",      "into",   "with",
    "values", "using",  "natural",   "cross",       "inner",    "outer",  "left",
    "right",  "full",   "lateral",   "only",        "distinct", "all",    "top",
    "as",     "on",     "intersect", "tablesample", NULL,
};

// Split the SQL into `SELECT <select_list> FROM <table_ref>`, rejecting anything else.
// On success the three spans point into `sql`.
static bool ParseSimpleSelect(const char* sql, size_t n, size_t* list_off, size_t* list_len,
                              size_t* tbl_off, size_t* tbl_len) {
  // Trailing semicolons and whitespace are the only trailer allowed.
  while (n > 0 && (IsSpace(sql[n - 1]) || sql[n - 1] == ';')) n--;
  size_t i = SkipSpace(sql, n, 0);
  size_t after_select = MatchWord(sql, n, i, "select");
  if (!after_select) return false;

  // Scan the whole statement once: no quotes of any kind, no parentheses, no comment
  // markers, no parameter markers, and exactly one FROM at the top level.  Rejecting
  // string and identifier quoting outright is what makes a scanner this small sound --
  // with no quoting to track, every word boundary found below is a real one.
  size_t from_start = 0, from_end = 0;
  int from_count = 0;
  for (size_t k = after_select; k < n; k++) {
    char c = sql[k];
    if (c == '\'' || c == '"' || c == '`' || c == '(' || c == ')' || c == ';' || c == '?' ||
        c == '[' || c == ']' || c == '{' || c == '}' || c == '@' || c == ':' || c == '\\') {
      return false;
    }
    if (c == '-' && k + 1 < n && sql[k + 1] == '-') return false;  // line comment
    if (c == '/' && k + 1 < n && sql[k + 1] == '*') return false;  // block comment
    if (!IsIdentStart(c)) continue;
    if (k > after_select && IsIdentChar(sql[k - 1])) continue;  // mid-word
    size_t end = MatchWord(sql, n, k, "from");
    if (end) {
      from_count++;
      from_start = k;
      from_end = end;
      k = end - 1;
      continue;
    }
    for (int w = 0; kUnsplittableWords[w]; w++) {
      if (MatchWord(sql, n, k, kUnsplittableWords[w])) return false;
    }
  }
  if (from_count != 1) return false;

  *list_off = after_select;
  *list_len = from_start - after_select;
  // `SELECT FROM t` is not a query; the span between the keywords must hold something.
  bool has_list = false;
  for (size_t k = 0; k < *list_len; k++) {
    if (!IsSpace(sql[after_select + k])) {
      has_list = true;
      break;
    }
  }
  if (!has_list) return false;

  // The table reference: 1..3 dot-separated bare identifiers, optionally followed by a
  // bare alias, and nothing else.  (`AS` was rejected above, so an alias here is the
  // bare form.)  A comma anywhere would be an implicit cross join.
  size_t t = SkipSpace(sql, n, from_end);
  size_t tstart = t;
  int parts = 0;
  while (t < n) {
    if (!IsIdentStart(sql[t])) return false;
    while (t < n && IsIdentChar(sql[t])) t++;
    parts++;
    if (t < n && sql[t] == '.') {
      t++;
      continue;
    }
    break;
  }
  if (parts < 1 || parts > 3) return false;
  size_t tend = t;
  t = SkipSpace(sql, n, t);
  if (t < n) {
    // A bare alias, and then nothing.
    if (!IsIdentStart(sql[t])) return false;
    while (t < n && IsIdentChar(sql[t])) t++;
    t = SkipSpace(sql, n, t);
    if (t < n) return false;
  }
  *tbl_off = tstart;
  *tbl_len = tend - tstart;
  return *tbl_len > 0;
}

// --- PostgreSQL block count ---------------------------------------------------------

// Ask the server for the table's heap size in blocks, and in the same round trip prove
// that `tid` exists and compares the way the split assumes.  Returns false -- with no
// error set -- for every "this is not a table we can slice" answer: not PostgreSQL, no
// such relation, a relation with no heap of its own (view, foreign table, partitioned
// parent), or a server that merely looks like PostgreSQL and rejects the query.
static bool PostgresHeapBlocks(SQLHDBC hdbc, const char* table, size_t table_len,
                               int64_t* out_blocks) {
  // The table reference came out of ParseSimpleSelect, so it is bare identifiers and
  // dots only -- no quote to escape, and nothing that could close the literal.
  for (size_t i = 0; i < table_len; i++) {
    if (table[i] != '.' && !IsIdentChar(table[i])) return false;
  }
  char sql[512];
  int wrote = snprintf(sql, sizeof(sql),
                       "SELECT (pg_relation_size(c.oid) / current_setting('block_size')::bigint)"
                       "::bigint FROM pg_class c WHERE c.oid = to_regclass('%.*s') "
                       "AND '(0,0)'::tid < '(1,0)'::tid",
                       (int)table_len, table);
  if (wrote <= 0 || (size_t)wrote >= sizeof(sql)) return false;

  SQLHSTMT hstmt = NULL;
  if (!SQL_SUCCEEDED(SQLAllocHandle(SQL_HANDLE_STMT, hdbc, &hstmt))) return false;
  bool ok = false;
  if (SQL_SUCCEEDED(OdbcExecDirectUtf8(hstmt, sql))) {
    SQLRETURN ret = SQLFetch(hstmt);
    if (SQL_SUCCEEDED(ret)) {
      SQLBIGINT blocks = 0;
      SQLLEN ind = 0;
      if (SQL_SUCCEEDED(SQLGetData(hstmt, 1, SQL_C_SBIGINT, &blocks, sizeof(blocks), &ind)) &&
          ind != SQL_NULL_DATA && blocks > 0) {
        *out_blocks = (int64_t)blocks;
        ok = true;
      }
    }
    SQLCloseCursor(hstmt);
  }
  SQLFreeHandle(SQL_HANDLE_STMT, hstmt);
  return ok;
}

// psqlodbc drives every PostgreSQL-wire server there is, and most of them have no
// `ctid`.  The behavioural probe above catches nearly all of those, but a server that
// answers it *wrongly* would be silent data loss, so the DBMS has to name itself
// PostgreSQL as well.
//
// Note what this does *not* say: CockroachDB and YugabyteDB both answer SQL_DBMS_NAME
// with "PostgreSQL", so this is a necessary condition for the heap split and never a
// sufficient one.  The key-range strategy uses it for the opposite purpose -- as the
// cue that `pg_index` is there to be read.
static bool DbmsIsPostgres(SQLHDBC hdbc) {
  SQLCHAR name[64] = {0};
  SQLSMALLINT len = 0;
  if (!SQL_SUCCEEDED(SQLGetInfo(hdbc, SQL_DBMS_NAME, name, sizeof(name), &len))) return false;
  return strncmp((const char*)name, "PostgreSQL", 10) == 0;
}

// --- Key-range split ----------------------------------------------------------------

// A name the driver is willing to put in a quoted identifier or a string literal
// without escaping anything: identifier characters and non-ASCII bytes (so that a
// UTF-8 column name still works), and nothing else.  That rules out the quote
// character, the backslash and every control byte in one test, which is what lets the
// SQL below be built with snprintf and still be safe.
static bool IsSafeIdentifier(const char* s) {
  if (!s || !*s) return false;
  for (const unsigned char* p = (const unsigned char*)s; *p; p++) {
    if (*p >= 0x80) continue;
    if (!IsIdentChar((char)*p)) return false;
  }
  return true;
}

// One catalog name each for the three levels of a table reference.  Absent levels stay
// empty, and an empty level is passed to ODBC as NULL ("the driver decides").
struct OdbcTableRef {
  char catalog[ODBC_PARTITION_IDENT_MAX];
  char schema[ODBC_PARTITION_IDENT_MAX];
  char table[ODBC_PARTITION_IDENT_MAX];
};

// Split `a`, `a.b` or `a.b.c` into (catalog, schema, table).  The span comes from
// ParseSimpleSelect, so it is identifier characters and dots only.
static bool SplitTableRef(const char* ref, size_t len, struct OdbcTableRef* out) {
  memset(out, 0, sizeof(*out));
  char* parts[3] = {out->catalog, out->schema, out->table};
  size_t starts[3], lens[3];
  int n = 0;
  size_t begin = 0;
  for (size_t i = 0; i <= len; i++) {
    if (i == len || ref[i] == '.') {
      if (n == 3 || i == begin) return false;
      starts[n] = begin;
      lens[n] = i - begin;
      if (lens[n] >= ODBC_PARTITION_IDENT_MAX) return false;
      n++;
      begin = i + 1;
    }
  }
  if (n < 1) return false;
  // Right-align: one part is the table, two are schema.table, three catalog.schema.table.
  for (int i = 0; i < n; i++) {
    char* dst = parts[3 - n + i];
    memcpy(dst, ref + starts[i], lens[i]);
    dst[lens[i]] = '\0';
  }
  return true;
}

// SQLGetData a character column into a fixed buffer; false if it is NULL, too long, or
// unreadable.
static bool GetDataString(SQLHSTMT hstmt, SQLUSMALLINT col, char* buf, size_t cap) {
  SQLLEN ind = 0;
  buf[0] = '\0';
  SQLRETURN ret = OdbcGetDataStrUtf8(hstmt, col, buf, cap, &ind, false);
  if (!SQL_SUCCEEDED(ret) || ind == SQL_NULL_DATA) return false;
  // SQL_SUCCESS_WITH_INFO here means truncation, which for an identifier means the
  // driver's name is longer than we are willing to carry.
  if (ret == SQL_SUCCESS_WITH_INFO) return false;
  buf[cap - 1] = '\0';
  return buf[0] != '\0';
}

static bool GetDataSmallInt(SQLHSTMT hstmt, SQLUSMALLINT col, SQLSMALLINT* out) {
  SQLLEN ind = 0;
  if (!SQL_SUCCEEDED(SQLGetData(hstmt, col, SQL_C_SSHORT, out, 0, &ind))) return false;
  return ind != SQL_NULL_DATA;
}

// The leading column of `ref`'s primary key, via ODBC's own catalog.  Also narrows
// `ref` to the catalog and schema the server actually resolved it in, so that the
// SQLColumns call that follows cannot land on a same-named table in another schema.
//
// Returns false when there is no primary key, when the name resolves to more than one
// table, or when the leading column's name is not one we are willing to quote.
static bool PrimaryKeyLeadColumn(SQLHDBC hdbc, struct OdbcTableRef* ref, char* out_col,
                                 size_t out_cap, int* out_key_columns) {
  SQLHSTMT hstmt = NULL;
  if (!SQL_SUCCEEDED(SQLAllocHandle(SQL_HANDLE_STMT, hdbc, &hstmt))) return false;
  bool ok = false;
  int columns = 0;
  char found_cat[ODBC_PARTITION_IDENT_MAX] = {0};
  char found_sch[ODBC_PARTITION_IDENT_MAX] = {0};
  char found_tbl[ODBC_PARTITION_IDENT_MAX] = {0};
  bool ambiguous = false;

  SQLCHAR* cat = ref->catalog[0] ? (SQLCHAR*)ref->catalog : NULL;
  SQLCHAR* sch = ref->schema[0] ? (SQLCHAR*)ref->schema : NULL;
  if (SQL_SUCCEEDED(OdbcPrimaryKeysUtf8(hstmt, (const char*)cat, cat ? SQL_NTS : 0, (const char*)sch,
                                        sch ? SQL_NTS : 0, ref->table, SQL_NTS))) {
    while (SQL_SUCCEEDED(SQLFetch(hstmt))) {
      char rcat[ODBC_PARTITION_IDENT_MAX] = {0};
      char rsch[ODBC_PARTITION_IDENT_MAX] = {0};
      char rtbl[ODBC_PARTITION_IDENT_MAX] = {0};
      char rcol[ODBC_PARTITION_IDENT_MAX] = {0};
      SQLSMALLINT seq = 0;
      // TABLE_CAT and TABLE_SCHEM are legitimately NULL on drivers with no such
      // concept; TABLE_NAME, COLUMN_NAME and KEY_SEQ are not optional.
      (void)GetDataString(hstmt, 1, rcat, sizeof(rcat));
      (void)GetDataString(hstmt, 2, rsch, sizeof(rsch));
      if (!GetDataString(hstmt, 3, rtbl, sizeof(rtbl)) ||
          !GetDataString(hstmt, 4, rcol, sizeof(rcol)) || !GetDataSmallInt(hstmt, 5, &seq)) {
        ambiguous = true;
        break;
      }
      if (columns == 0) {
        memcpy(found_cat, rcat, sizeof(found_cat));
        memcpy(found_sch, rsch, sizeof(found_sch));
        memcpy(found_tbl, rtbl, sizeof(found_tbl));
      } else if (strcmp(found_cat, rcat) != 0 || strcmp(found_sch, rsch) != 0 ||
                 strcmp(found_tbl, rtbl) != 0) {
        // The unqualified name matched a primary key on more than one table.
        ambiguous = true;
        break;
      }
      columns++;
      if (seq == 1) {
        if (strlen(rcol) >= out_cap) {
          ambiguous = true;
          break;
        }
        memcpy(out_col, rcol, strlen(rcol) + 1);
        ok = true;
      }
    }
    SQLCloseCursor(hstmt);
  }
  SQLFreeHandle(SQL_HANDLE_STMT, hstmt);
  if (ambiguous || !ok || columns < 1 || !IsSafeIdentifier(out_col)) return false;
  memcpy(ref->catalog, found_cat, sizeof(ref->catalog));
  memcpy(ref->schema, found_sch, sizeof(ref->schema));
  memcpy(ref->table, found_tbl, sizeof(ref->table));
  *out_key_columns = columns;
  return true;
}

// Is `column` of `ref` an integer column declared NOT NULL?
//
// Both halves matter.  Integer because a boundary has to be computed exactly, and
// integers are the one family where the arithmetic below cannot round a row onto the
// wrong side of a bound.  NOT NULL because a NULL key compares false against every
// boundary and would disappear from every slice -- a primary key is NOT NULL by
// definition everywhere it matters, but "by definition" is not a thing this driver is
// willing to lose rows on, so it is read from the catalog.
static bool ColumnIsSplittableInteger(SQLHDBC hdbc, const struct OdbcTableRef* ref,
                                      const char* column) {
  SQLHSTMT hstmt = NULL;
  if (!SQL_SUCCEEDED(SQLAllocHandle(SQL_HANDLE_STMT, hdbc, &hstmt))) return false;
  bool ok = false;

  SQLCHAR* cat = ref->catalog[0] ? (SQLCHAR*)ref->catalog : NULL;
  SQLCHAR* sch = ref->schema[0] ? (SQLCHAR*)ref->schema : NULL;
  // ColumnName is a *pattern* argument in ODBC, so `_` in a column name would match
  // more than itself.  Ask for every column instead and match the name exactly.
  if (SQL_SUCCEEDED(OdbcColumnsUtf8(hstmt, (const char*)cat, cat ? SQL_NTS : 0, (const char*)sch,
                                    sch ? SQL_NTS : 0, ref->table, SQL_NTS, NULL, 0))) {
    while (SQL_SUCCEEDED(SQLFetch(hstmt))) {
      char rcol[ODBC_PARTITION_IDENT_MAX] = {0};
      SQLSMALLINT type = 0, nullable = 0;
      if (!GetDataString(hstmt, 4, rcol, sizeof(rcol))) continue;
      if (strcmp(rcol, column) != 0) continue;
      if (!GetDataSmallInt(hstmt, 5, &type) || !GetDataSmallInt(hstmt, 11, &nullable)) break;
      ok = (type == SQL_TINYINT || type == SQL_SMALLINT || type == SQL_INTEGER ||
            type == SQL_BIGINT) &&
           nullable == SQL_NO_NULLS;
      break;
    }
    SQLCloseCursor(hstmt);
  }
  SQLFreeHandle(SQL_HANDLE_STMT, hstmt);
  return ok;
}

// The driver's identifier quote character, or '\0' when it has none.  ODBC says a
// driver with no quoting answers with a single space.
static char IdentifierQuote(SQLHDBC hdbc) {
  SQLCHAR q[8] = {0};
  SQLSMALLINT len = 0;
  if (!SQL_SUCCEEDED(SQLGetInfo(hdbc, SQL_IDENTIFIER_QUOTE_CHAR, q, sizeof(q), &len))) return '\0';
  if (q[0] == ' ' || q[0] == '\0') return '\0';
  return (char)q[0];
}

// Write `column` quoted for this driver.  `column` has already passed
// IsSafeIdentifier, so it cannot contain the quote character and needs no escaping.
static bool WriteQuotedIdent(char* buf, size_t cap, char quote, const char* column) {
  int wrote =
      quote ? snprintf(buf, cap, "%c%s%c", quote, column, quote) : snprintf(buf, cap, "%s", column);
  return wrote > 0 && (size_t)wrote < cap;
}

// MIN and MAX of `expr` over the table, as int64.  False when the table is empty (both
// NULL), when either value will not convert, or when the server rejects the query.
static bool ExpressionExtent(SQLHDBC hdbc, const char* table, size_t table_len, const char* expr,
                             int64_t* out_lo, int64_t* out_hi) {
  char sql[512];
  int wrote = snprintf(sql, sizeof(sql), "SELECT MIN(%s), MAX(%s) FROM %.*s", expr, expr,
                       (int)table_len, table);
  if (wrote <= 0 || (size_t)wrote >= sizeof(sql)) return false;

  SQLHSTMT hstmt = NULL;
  if (!SQL_SUCCEEDED(SQLAllocHandle(SQL_HANDLE_STMT, hdbc, &hstmt))) return false;
  bool ok = false;
  if (SQL_SUCCEEDED(OdbcExecDirectUtf8(hstmt, sql))) {
    if (SQL_SUCCEEDED(SQLFetch(hstmt))) {
      SQLBIGINT lo = 0, hi = 0;
      SQLLEN ind_lo = 0, ind_hi = 0;
      if (SQL_SUCCEEDED(SQLGetData(hstmt, 1, SQL_C_SBIGINT, &lo, sizeof(lo), &ind_lo)) &&
          SQL_SUCCEEDED(SQLGetData(hstmt, 2, SQL_C_SBIGINT, &hi, sizeof(hi), &ind_hi)) &&
          ind_lo != SQL_NULL_DATA && ind_hi != SQL_NULL_DATA && (int64_t)hi >= (int64_t)lo) {
        *out_lo = (int64_t)lo;
        *out_hi = (int64_t)hi;
        ok = true;
      }
    }
    SQLCloseCursor(hstmt);
  }
  SQLFreeHandle(SQL_HANDLE_STMT, hstmt);
  return ok;
}

// What a PostgreSQL-wire server says about the leading primary-key column's ordering.
struct OdbcPgIndexFacts {
  bool lead_is_hash;  // YugabyteDB hash partitioning (indoption bit 0x4)
  int hash_columns;   // how many of the key's columns are hash-partitioned
  bool has_yb_hash_code;
  int64_t est_rows;  // pg_class.reltuples, or -1 when the server has no estimate
};

// Read those facts in one round trip.  Returns false if the server will not answer,
// and on a PostgreSQL-wire server that is a refusal to key-range split at all: the
// difference between an ordered key and a hash-partitioned one is the difference
// between an index condition and K full table scans.
static bool PgIndexFacts(SQLHDBC hdbc, const char* table, size_t table_len, const char* column,
                         struct OdbcPgIndexFacts* out) {
  char sql[1024];
  int wrote =
      snprintf(sql, sizeof(sql),
               "SELECT (i.indoption[0] & 4)::int, "
               "(SELECT count(*) FROM generate_subscripts(i.indoption, 1) s "
               "WHERE (i.indoption[s] & 4) <> 0)::int, "
               "(SELECT count(*) FROM pg_proc p WHERE p.proname = 'yb_hash_code')::int, "
               "c.reltuples::bigint "
               "FROM pg_index i JOIN pg_class c ON c.oid = i.indrelid "
               "JOIN pg_attribute a ON a.attrelid = i.indrelid AND a.attnum = i.indkey[0] "
               "WHERE i.indrelid = to_regclass('%.*s') AND i.indisprimary AND a.attname = '%s'",
               (int)table_len, table, column);
  if (wrote <= 0 || (size_t)wrote >= sizeof(sql)) return false;

  SQLHSTMT hstmt = NULL;
  if (!SQL_SUCCEEDED(SQLAllocHandle(SQL_HANDLE_STMT, hdbc, &hstmt))) return false;
  bool ok = false;
  if (SQL_SUCCEEDED(OdbcExecDirectUtf8(hstmt, sql))) {
    if (SQL_SUCCEEDED(SQLFetch(hstmt))) {
      SQLINTEGER lead = 0, nhash = 0, ybhash = 0;
      SQLBIGINT rows = 0;
      SQLLEN i1 = 0, i2 = 0, i3 = 0, i4 = 0;
      if (SQL_SUCCEEDED(SQLGetData(hstmt, 1, SQL_C_SLONG, &lead, 0, &i1)) &&
          SQL_SUCCEEDED(SQLGetData(hstmt, 2, SQL_C_SLONG, &nhash, 0, &i2)) &&
          SQL_SUCCEEDED(SQLGetData(hstmt, 3, SQL_C_SLONG, &ybhash, 0, &i3)) &&
          SQL_SUCCEEDED(SQLGetData(hstmt, 4, SQL_C_SBIGINT, &rows, sizeof(rows), &i4)) &&
          i1 != SQL_NULL_DATA && i2 != SQL_NULL_DATA && i3 != SQL_NULL_DATA) {
        out->lead_is_hash = lead != 0;
        out->hash_columns = (int)nhash;
        out->has_yb_hash_code = ybhash > 0;
        // CockroachDB reports no reltuples at all; treat that as "no estimate".
        out->est_rows = (i4 == SQL_NULL_DATA) ? -1 : (int64_t)rows;
        ok = true;
      }
    }
    SQLCloseCursor(hstmt);
  }
  SQLFreeHandle(SQL_HANDLE_STMT, hstmt);
  return ok;
}

// --- The chosen split ---------------------------------------------------------------

enum OdbcSplitKind {
  ODBC_SPLIT_NONE = 0,
  ODBC_SPLIT_CTID,       // strategy 1: PostgreSQL heap blocks
  ODBC_SPLIT_KEY_RANGE,  // strategy 2: min/max of an ordered primary-key column
  ODBC_SPLIT_YB_HASH,    // strategy 3: YugabyteDB's tablet hash
};

struct OdbcSplit {
  enum OdbcSplitKind kind;
  // The ordered SQL expression the boundaries compare against, and the inclusive
  // extent of its values over the table.
  char expr[ODBC_PARTITION_IDENT_MAX + 32];
  int64_t lo, hi;
  // Partition count to use when the caller asked for "automatic", or -1 when this
  // strategy has no size estimate to base one on.
  int64_t auto_partitions;
};

// Write the SQL literal for boundary value `v` in this split's expression domain.
static bool WriteBound(const struct OdbcSplit* split, char* buf, size_t cap, int64_t v) {
  int wrote = (split->kind == ODBC_SPLIT_CTID) ? snprintf(buf, cap, "'(%" PRId64 ",0)'::tid", v)
                                               : snprintf(buf, cap, "%" PRId64, v);
  return wrote > 0 && (size_t)wrote < cap;
}

// Boundary k of n over [lo, hi], for 1 <= k <= n-1.  Computed in unsigned arithmetic so
// that an extent spanning the whole int64 range cannot overflow: `span` is at most
// UINT64_MAX, `span % n` is below n <= ODBC_PARTITION_MAX, and k is below n, so the
// remainder term is at most 255*255.  Callers guarantee span >= n, which makes
// `span / n` at least 1 and the boundaries therefore strictly increasing.
static int64_t SplitBoundary(int64_t lo, int64_t hi, int64_t n, int64_t k) {
  const uint64_t span = (uint64_t)hi - (uint64_t)lo;
  const uint64_t un = (uint64_t)n, uk = (uint64_t)k;
  const uint64_t offset = span / un * uk + (span % un) * uk / un;
  return (int64_t)((uint64_t)lo + offset);
}

// Try, in order, every strategy that could apply to `table`.  Leaves split->kind at
// ODBC_SPLIT_NONE when none does.
static void ChooseSplit(SQLHDBC hdbc, const char* table, size_t table_len,
                        struct OdbcSplit* split) {
  memset(split, 0, sizeof(*split));
  split->auto_partitions = -1;

  // Strategy 1: the PostgreSQL heap.  Best where it exists, and needs no key at all.
  const bool pg_wire = DbmsIsPostgres(hdbc);
  int64_t blocks = 0;
  if (pg_wire && PostgresHeapBlocks(hdbc, table, table_len, &blocks)) {
    split->kind = ODBC_SPLIT_CTID;
    snprintf(split->expr, sizeof(split->expr), "ctid");
    split->lo = 0;
    split->hi = blocks;
    split->auto_partitions = blocks / ODBC_PARTITION_AUTO_BLOCKS;
    return;
  }

  // Strategies 2 and 3 both start from the leading primary-key column.
  struct OdbcTableRef ref;
  char column[ODBC_PARTITION_IDENT_MAX];
  int key_columns = 0;
  if (!SplitTableRef(table, table_len, &ref)) return;
  if (!PrimaryKeyLeadColumn(hdbc, &ref, column, sizeof(column), &key_columns)) return;
  if (!ColumnIsSplittableInteger(hdbc, &ref, column)) return;

  char quoted[ODBC_PARTITION_IDENT_MAX + 4];
  if (!WriteQuotedIdent(quoted, sizeof(quoted), IdentifierQuote(hdbc), column)) return;

  struct OdbcPgIndexFacts facts = {false, 0, false, -1};
  if (pg_wire) {
    // On a PostgreSQL-wire server the ordering of the key is knowable, so it is not
    // assumed: no answer means no split.
    if (!PgIndexFacts(hdbc, table, table_len, column, &facts)) return;
    if (facts.est_rows > 0) split->auto_partitions = facts.est_rows / ODBC_PARTITION_AUTO_ROWS;
  }

  if (facts.lead_is_hash) {
    // Strategy 3.  Only when the hash key is exactly the column we are hashing, or
    // yb_hash_code() of it is not the tablet hash and the predicate is a filter.
    if (!facts.has_yb_hash_code || facts.hash_columns != 1) return;
    split->kind = ODBC_SPLIT_YB_HASH;
    if (snprintf(split->expr, sizeof(split->expr), "yb_hash_code(%s)", quoted) < 0) {
      split->kind = ODBC_SPLIT_NONE;
      return;
    }
    // yb_hash_code is a uint16 over the whole space, whatever the data looks like.
    split->lo = 0;
    split->hi = 65535;
    return;
  }

  // Strategy 2.
  int64_t lo = 0, hi = 0;
  if (!ExpressionExtent(hdbc, table, table_len, quoted, &lo, &hi)) return;
  split->kind = ODBC_SPLIT_KEY_RANGE;
  memcpy(split->expr, quoted, strlen(quoted) + 1);
  split->lo = lo;
  split->hi = hi;
  if (split->auto_partitions < 0) {
    // No server estimate: a dense integer key spans about one value per row, which is
    // the right order of magnitude for choosing a partition count and is never used
    // for anything else.  A caller who names a partition count never reaches this.
    const uint64_t span = (uint64_t)hi - (uint64_t)lo;
    const uint64_t values = span == UINT64_MAX ? UINT64_MAX : span + 1;
    const uint64_t want = values / ODBC_PARTITION_AUTO_ROWS;
    split->auto_partitions =
        want > (uint64_t)ODBC_PARTITION_AUTO_MAX ? ODBC_PARTITION_AUTO_MAX : (int64_t)want;
  }
}

// --- Descriptor allocation ----------------------------------------------------------

struct OdbcPartitionsState {
  size_t count;
  uint8_t** blobs;
  size_t* lengths;
  // The two arrays AdbcPartitions points at.
  const uint8_t** view;
};

static void PartitionsRelease(struct AdbcPartitions* partitions) {
  struct OdbcPartitionsState* st = (struct OdbcPartitionsState*)partitions->private_data;
  if (st) {
    for (size_t i = 0; i < st->count; i++) free(st->blobs[i]);
    free(st->blobs);
    free(st->lengths);
    free(st->view);
    free(st);
  }
  memset(partitions, 0, sizeof(*partitions));
}

static struct OdbcPartitionsState* PartitionsStateNew(size_t count) {
  struct OdbcPartitionsState* st = calloc(1, sizeof(*st));
  if (!st) return NULL;
  st->count = count;
  st->blobs = calloc(count ? count : 1, sizeof(uint8_t*));
  st->lengths = calloc(count ? count : 1, sizeof(size_t));
  st->view = calloc(count ? count : 1, sizeof(const uint8_t*));
  if (!st->blobs || !st->lengths || !st->view) {
    free(st->blobs);
    free(st->lengths);
    free(st->view);
    free(st);
    return NULL;
  }
  return st;
}

// Wrap one slice's SQL in a descriptor and store it at `slot`.
static bool PartitionsSet(struct OdbcPartitionsState* st, size_t slot, const char* sql,
                          size_t sql_len) {
  size_t total = ODBC_PARTITION_MAGIC_LEN + sql_len;
  uint8_t* blob = malloc(total ? total : 1);
  if (!blob) return false;
  memcpy(blob, ODBC_PARTITION_MAGIC, ODBC_PARTITION_MAGIC_LEN);
  memcpy(blob + ODBC_PARTITION_MAGIC_LEN, sql, sql_len);
  st->blobs[slot] = blob;
  st->lengths[slot] = total;
  st->view[slot] = blob;
  return true;
}

static void PartitionsPublish(struct OdbcPartitionsState* st, struct AdbcPartitions* partitions) {
  partitions->num_partitions = st->count;
  partitions->partitions = st->view;
  partitions->partition_lengths = st->lengths;
  partitions->private_data = st;
  partitions->release = PartitionsRelease;
}

// --- ExecutePartitions --------------------------------------------------------------

// How many slices the caller asked for, resolved against what the table can offer.
// Not inlined, deliberately.  What was measured, on a 32-bit Windows build with MSVC
// 19.44.35228 for x86 and NDEBUG held constant: at /Od, /O1 and /O2 /Ob0
// tests/c/test_partition.c passes; at /O2 /Ob2 -- inline expansion on -- this function,
// inlined into its caller, returned the key span (2,000,000) where 8 is correct, for
// the one input whose span fits in 32 bits while the operands do not
// (lo = -1,000,000, hi = 1,000,000).  Every way of observing the value (a print, a
// volatile, AddressSanitizer -- which also reports no memory error) makes it correct,
// and the failure could not be reproduced outside this translation unit, so the cause
// is left open rather than named.  Keeping the call out of line is the one fix that
// does not depend on what the optimiser decides, and the 32-bit CI job builds Release
// so it stays covered.
#if defined(_MSC_VER)
__declspec(noinline)
#elif defined(__GNUC__)
__attribute__((noinline))
#endif
static int64_t ResolvePartitionCount(int64_t requested, const struct OdbcSplit* split) {
  int64_t want = requested;
  if (want <= 0) {
    // Automatic: one partition per ODBC_PARTITION_AUTO_ROWS rows' worth of table.
    // Below that a second connection costs more (connect, plan, snapshot) than the
    // halved scan saves.  With no estimate at all, do not split.
    want = split->auto_partitions < 0 ? 1 : split->auto_partitions;
    if (want > ODBC_PARTITION_AUTO_MAX) want = ODBC_PARTITION_AUTO_MAX;
  }
  if (want > ODBC_PARTITION_MAX) want = ODBC_PARTITION_MAX;
  if (want < 1) want = 1;
  // Every boundary must be distinct, or a slice is a connection that reads nothing --
  // and SplitBoundary needs span >= n to keep the boundaries strictly increasing.
  const uint64_t span = (uint64_t)split->hi - (uint64_t)split->lo;
  if (span < (uint64_t)want) want = (int64_t)span;
  if (want < 1) want = 1;
  return want;
}

// The single-partition fallback: one descriptor carrying the query verbatim.  Always
// correct, whatever the query was.
static AdbcStatusCode OnePartition(const char* query, struct AdbcPartitions* partitions,
                                   struct AdbcError* error) {
  struct OdbcPartitionsState* st = PartitionsStateNew(1);
  if (!st || !PartitionsSet(st, 0, query, strlen(query))) {
    if (st) {
      struct AdbcPartitions tmp = {0};
      tmp.private_data = st;
      PartitionsRelease(&tmp);
    }
    InternalAdbcSetError(error, "out of memory");
    return ADBC_STATUS_INTERNAL;
  }
  PartitionsPublish(st, partitions);
  return ADBC_STATUS_OK;
}

AdbcStatusCode OdbcStatementExecutePartitionsOdbc(struct OdbcStatement* stmt,
                                                  struct ArrowSchema* schema,
                                                  struct AdbcPartitions* partitions,
                                                  int64_t* rows_affected, struct AdbcError* error) {
  if (!stmt->query) {
    InternalAdbcSetError(error, "Must call StatementSetSqlQuery first");
    return ADBC_STATUS_INVALID_STATE;
  }
  if (stmt->ingest_table) {
    InternalAdbcSetError(error, "Bulk ingest does not produce a partitioned result set");
    return ADBC_STATUS_INVALID_STATE;
  }
  if (stmt->has_bind) {
    // A partition descriptor carries SQL text and nothing else, so there is nowhere to
    // put bound parameters -- and re-binding them on someone else's connection is not
    // something this driver can promise.
    InternalAdbcSetError(error,
                         "Partitioned results are not supported for a statement with bound "
                         "parameters");
    return ADBC_STATUS_NOT_IMPLEMENTED;
  }
  if (rows_affected) *rows_affected = -1;

  // The schema has to be the unpartitioned query's schema: every slice adds only a
  // WHERE clause, so describing the original query answers for all of them.
  if (schema) {
    RAISE_ADBC(OdbcStatementEnsureHandle(stmt, error));
    ODBC_CHECK(OdbcPrepareSql(stmt->ref->hstmt, stmt->query, &stmt->reader_opts), SQL_HANDLE_STMT,
               stmt->ref->hstmt, "SQLPrepare", error);
    stmt->prepared = true;
    RAISE_ADBC(OdbcDescribeResultSchema(stmt->ref->hstmt, &stmt->reader_opts, schema, error));
  }

  const size_t qlen = strlen(stmt->query);
  size_t list_off = 0, list_len = 0, tbl_off = 0, tbl_len = 0;
  struct OdbcSplit split;
  memset(&split, 0, sizeof(split));
  if (stmt->partitions == 1 ||
      !ParseSimpleSelect(stmt->query, qlen, &list_off, &list_len, &tbl_off, &tbl_len)) {
    return OnePartition(stmt->query, partitions, error);
  }
  ChooseSplit(stmt->conn->hdbc, stmt->query + tbl_off, tbl_len, &split);
  if (split.kind == ODBC_SPLIT_NONE) return OnePartition(stmt->query, partitions, error);

  const int64_t nparts = ResolvePartitionCount(stmt->partitions, &split);
  if (nparts <= 1) return OnePartition(stmt->query, partitions, error);

  struct OdbcPartitionsState* st = PartitionsStateNew((size_t)nparts);
  if (!st) {
    InternalAdbcSetError(error, "out of memory");
    return ADBC_STATUS_INTERNAL;
  }
  // `SELECT <list> FROM <table>` plus the widest possible predicate: the expression
  // twice, two boundary literals, and the fixed ` WHERE  >=  AND  < ` around them.
  const size_t slice_cap = qlen + 2 * strlen(split.expr) + 128;
  char* slice = malloc(slice_cap);
  if (!slice) {
    struct AdbcPartitions tmp = {0};
    tmp.private_data = st;
    PartitionsRelease(&tmp);
    InternalAdbcSetError(error, "out of memory");
    return ADBC_STATUS_INTERNAL;
  }

  AdbcStatusCode status = ADBC_STATUS_OK;
  for (int64_t k = 0; k < nparts; k++) {
    // Half-open [boundary k, boundary k+1), with the first slice unbounded below and
    // the last unbounded above.  A row whose expression lands exactly on a boundary
    // belongs to the slice above it and to no other, so equal values are neither
    // doubled nor dropped; the unbounded ends mean the union is the whole domain of
    // the expression however stale the extent turns out to be.  For `ctid`, boundary
    // `b` is written '(b,0)', which lies strictly between block b-1's last tuple and
    // block b's first, because tuple offsets are 1-based.
    char lo_lit[64], hi_lit[64];
    int wrote;
    const int head = (int)(tbl_off + tbl_len);
    if (k == 0) {
      wrote =
          WriteBound(&split, hi_lit, sizeof(hi_lit), SplitBoundary(split.lo, split.hi, nparts, 1))
              ? snprintf(slice, slice_cap, "%.*s WHERE %s < %s", head, stmt->query, split.expr,
                         hi_lit)
              : -1;
    } else if (k == nparts - 1) {
      wrote =
          WriteBound(&split, lo_lit, sizeof(lo_lit), SplitBoundary(split.lo, split.hi, nparts, k))
              ? snprintf(slice, slice_cap, "%.*s WHERE %s >= %s", head, stmt->query, split.expr,
                         lo_lit)
              : -1;
    } else {
      wrote = (WriteBound(&split, lo_lit, sizeof(lo_lit),
                          SplitBoundary(split.lo, split.hi, nparts, k)) &&
               WriteBound(&split, hi_lit, sizeof(hi_lit),
                          SplitBoundary(split.lo, split.hi, nparts, k + 1)))
                  ? snprintf(slice, slice_cap, "%.*s WHERE %s >= %s AND %s < %s", head, stmt->query,
                             split.expr, lo_lit, split.expr, hi_lit)
                  : -1;
    }
    if (wrote <= 0 || (size_t)wrote >= slice_cap ||
        !PartitionsSet(st, (size_t)k, slice, (size_t)wrote)) {
      status = ADBC_STATUS_INTERNAL;
      InternalAdbcSetError(error, "failed to build partition %" PRId64, k);
      break;
    }
  }
  free(slice);
  if (status != ADBC_STATUS_OK) {
    struct AdbcPartitions tmp = {0};
    tmp.private_data = st;
    PartitionsRelease(&tmp);
    return status;
  }
  PartitionsPublish(st, partitions);
  return ADBC_STATUS_OK;
}

// --- ReadPartition ------------------------------------------------------------------

AdbcStatusCode OdbcConnectionReadPartitionOdbc(struct OdbcConnection* conn,
                                               const uint8_t* serialized_partition,
                                               size_t serialized_length,
                                               struct ArrowArrayStream* out,
                                               struct AdbcError* error) {
  if (!conn->connected) {
    InternalAdbcSetError(error, "Connection is not initialized");
    return ADBC_STATUS_INVALID_STATE;
  }
  if (!out) {
    InternalAdbcSetError(error, "ReadPartition requires an output stream");
    return ADBC_STATUS_INVALID_ARGUMENT;
  }
  if (serialized_length < ODBC_PARTITION_MAGIC_LEN ||
      memcmp(serialized_partition, ODBC_PARTITION_MAGIC, ODBC_PARTITION_MAGIC_LEN) != 0) {
    InternalAdbcSetError(error, "Not an adbcbridge partition descriptor");
    return ADBC_STATUS_INVALID_ARGUMENT;
  }
  const size_t sql_len = serialized_length - ODBC_PARTITION_MAGIC_LEN;
  char* sql = malloc(sql_len + 1);
  if (!sql) {
    InternalAdbcSetError(error, "out of memory");
    return ADBC_STATUS_INTERNAL;
  }
  memcpy(sql, serialized_partition + ODBC_PARTITION_MAGIC_LEN, sql_len);
  sql[sql_len] = '\0';

  // A partition's stream owns its own statement handle: nothing about it is tied to the
  // statement that produced the descriptor, which is what lets a descriptor be read on
  // a different connection, in a different process, at any later time.
  SQLHSTMT hstmt = NULL;
  if (!SQL_SUCCEEDED(SQLAllocHandle(SQL_HANDLE_STMT, conn->hdbc, &hstmt))) {
    free(sql);
    return OdbcSetError(SQL_HANDLE_DBC, conn->hdbc, "SQLAllocHandle(SQL_HANDLE_STMT)", error);
  }
  SQLRETURN ret = OdbcExecDirectSql(hstmt, sql, &conn->reader_opts);
  free(sql);
  if (!SQL_SUCCEEDED(ret) && ret != SQL_NO_DATA) {
    AdbcStatusCode status = OdbcSetError(SQL_HANDLE_STMT, hstmt, "SQLExecDirect", error);
    SQLFreeHandle(SQL_HANDLE_STMT, hstmt);
    return status;
  }
  struct OdbcHandleRef* ref = OdbcHandleRefNew(hstmt);
  if (!ref) {
    SQLFreeHandle(SQL_HANDLE_STMT, hstmt);
    InternalAdbcSetError(error, "out of memory");
    return ADBC_STATUS_INTERNAL;
  }
  AdbcStatusCode status = OdbcReaderInit(ref, &conn->reader_opts, out, error);
  // OdbcReaderInit took its own reference on success; drop ours either way.
  OdbcHandleRefRelease(ref);
  return status;
}
