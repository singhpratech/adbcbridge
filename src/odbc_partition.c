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
// --- The PostgreSQL split -----------------------------------------------------------
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
// its own blocks off disk rather than filtering a full sequential scan.
//
// The first slice is left unbounded below and the last unbounded above.  That matters
// because the block count is a *snapshot* -- `pg_relation_size` is current as of the
// metadata query, but the table may have grown by the time the partitions are read --
// and an unbounded last slice absorbs whatever appeared past the end.  Coverage is
// then complete no matter how stale the size was; only the balance of the slices
// suffers.
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
// Views, foreign tables and declarative-partitioned parents need no special case: they
// have no heap of their own, `pg_relation_size` reports 0 blocks for them, and a
// zero-block table takes the single-partition path already.
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
// Refuse to hand out an unbounded number of connections' worth of work.
#define ODBC_PARTITION_MAX 256

// --- Tiny SQL scanner ---------------------------------------------------------------
// Only enough to recognise the one shape that can be split; see the header comment.

static bool IsIdentStart(char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}

static bool IsIdentChar(char c) {
  return IsIdentStart(c) || (c >= '0' && c <= '9') || c == '$';
}

static bool IsSpace(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f'; }

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
    "where",  "join",   "group",     "order",  "limit",  "offset", "union",
    "except", "having", "window",    "fetch",  "for",    "into",   "with",
    "values", "using",  "natural",   "cross",  "inner",  "outer",  "left",
    "right",  "full",   "lateral",   "only",   "distinct", "all",  "top",
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
    if (!IsSpace(sql[after_select + k])) { has_list = true; break; }
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
  if (SQL_SUCCEEDED(SQLExecDirect(hstmt, (SQLCHAR*)sql, SQL_NTS))) {
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
static bool DbmsIsPostgres(SQLHDBC hdbc) {
  SQLCHAR name[64] = {0};
  SQLSMALLINT len = 0;
  if (!SQL_SUCCEEDED(SQLGetInfo(hdbc, SQL_DBMS_NAME, name, sizeof(name), &len))) return false;
  return strncmp((const char*)name, "PostgreSQL", 10) == 0;
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

static void PartitionsPublish(struct OdbcPartitionsState* st,
                              struct AdbcPartitions* partitions) {
  partitions->num_partitions = st->count;
  partitions->partitions = st->view;
  partitions->partition_lengths = st->lengths;
  partitions->private_data = st;
  partitions->release = PartitionsRelease;
}

// --- ExecutePartitions --------------------------------------------------------------

// How many slices the caller asked for, resolved against what the table can offer.
static int64_t ResolvePartitionCount(int64_t requested, int64_t blocks) {
  int64_t want = requested;
  if (want <= 0) {
    want = blocks / ODBC_PARTITION_AUTO_BLOCKS;
    if (want > ODBC_PARTITION_AUTO_MAX) want = ODBC_PARTITION_AUTO_MAX;
  }
  if (want > ODBC_PARTITION_MAX) want = ODBC_PARTITION_MAX;
  if (want < 1) want = 1;
  // A slice must own at least one block, or it is a connection that reads nothing.
  if (want > blocks) want = blocks;
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
                                                  int64_t* rows_affected,
                                                  struct AdbcError* error) {
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
    ODBC_CHECK(SQLPrepare(stmt->ref->hstmt, (SQLCHAR*)stmt->query, SQL_NTS), SQL_HANDLE_STMT,
               stmt->ref->hstmt, "SQLPrepare", error);
    stmt->prepared = true;
    RAISE_ADBC(OdbcDescribeResultSchema(stmt->ref->hstmt, &stmt->reader_opts, schema, error));
  }

  const size_t qlen = strlen(stmt->query);
  size_t list_off = 0, list_len = 0, tbl_off = 0, tbl_len = 0;
  int64_t blocks = 0;
  if (stmt->partitions == 1 ||
      !ParseSimpleSelect(stmt->query, qlen, &list_off, &list_len, &tbl_off, &tbl_len) ||
      !DbmsIsPostgres(stmt->conn->hdbc) ||
      !PostgresHeapBlocks(stmt->conn->hdbc, stmt->query + tbl_off, tbl_len, &blocks)) {
    return OnePartition(stmt->query, partitions, error);
  }

  const int64_t nparts = ResolvePartitionCount(stmt->partitions, blocks);
  if (nparts <= 1) return OnePartition(stmt->query, partitions, error);

  struct OdbcPartitionsState* st = PartitionsStateNew((size_t)nparts);
  if (!st) {
    InternalAdbcSetError(error, "out of memory");
    return ADBC_STATUS_INTERNAL;
  }
  // `SELECT <list> FROM <table>` plus the widest possible ctid predicate.
  const size_t slice_cap = qlen + 160;
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
    // Boundaries at block b are written '(b,0)': tuple offsets are 1-based, so this
    // point lies strictly between block b-1's last tuple and block b's first.  The
    // first slice has no lower bound and the last no upper one, so the slices cover
    // the whole heap however stale `blocks` turns out to be.
    const int64_t lo = blocks * k / nparts;
    const int64_t hi = blocks * (k + 1) / nparts;
    int wrote;
    const int head = (int)(tbl_off + tbl_len);
    if (k == 0) {
      wrote = snprintf(slice, slice_cap, "%.*s WHERE ctid < '(%" PRId64 ",0)'::tid", head,
                       stmt->query, hi);
    } else if (k == nparts - 1) {
      wrote = snprintf(slice, slice_cap, "%.*s WHERE ctid >= '(%" PRId64 ",0)'::tid", head,
                       stmt->query, lo);
    } else {
      wrote = snprintf(slice, slice_cap,
                       "%.*s WHERE ctid >= '(%" PRId64 ",0)'::tid AND ctid < '(%" PRId64
                       ",0)'::tid",
                       head, stmt->query, lo, hi);
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
  SQLRETURN ret = SQLExecDirect(hstmt, (SQLCHAR*)sql, SQL_NTS);
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
