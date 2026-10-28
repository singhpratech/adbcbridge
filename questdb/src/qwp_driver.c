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

/// adbc_driver_questdb: ADBC 1.1.0 over QWP, the QuestDB wire protocol.
///
/// The driver is a thin shell around QuestDB's official C client.  The client
/// owns the WebSocket connections (one pool per AdbcDatabase), runs SQL with
/// positional bind parameters, and hands every result batch back through the
/// Arrow C Data Interface, so a query's ArrowArrayStream is the client's own
/// batches passed through untouched.  Bulk ingest goes the other way through
/// the client's Arrow batch sender.
///
/// Mapping:
///   AdbcDatabase    -> questdb_db (connection pool, lazily connected)
///   AdbcConnection  -> a handle on that pool; verified by borrowing a reader
///   AdbcStatement   -> per execution, a reader borrowed from the pool and a
///                      cursor; the result stream owns both and returns the
///                      reader when released, so several statements can stream
///                      at the same time (up to the pool's query_pool_max).
///
/// QuestDB has no transactions over QWP: autocommit cannot be switched off.

#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <time.h>
#endif

#include <arrow-adbc/adbc.h>
#include "nanoarrow/nanoarrow.h"
#include "utils.h"
#include "odbc_threads.h"

#include <questdb/client.h>
#include <questdb/egress/qwp_reader.h>
#include <questdb/ingress/qwp_sender.h>

#if defined(_WIN32)
#define QDB_EXPORT __declspec(dllexport)
#else
#define QDB_EXPORT __attribute__((visibility("default")))
#endif

#ifndef ADBC_QUESTDB_VERSION
#define ADBC_QUESTDB_VERSION "0.0.0"
#endif
#ifndef ADBC_QUESTDB_CLIENT_TAG
#define ADBC_QUESTDB_CLIENT_TAG "unknown"
#endif

// Option keys.  The generic ADBC ones come from adbc.h.
#define QDB_OPT_URI "uri"
#define QDB_OPT_USERNAME "username"
#define QDB_OPT_PASSWORD "password"
#define QDB_OPT_TOKEN "adbc.questdb.token"
#define QDB_OPT_CONF "adbc.questdb.conf"
#define QDB_OPT_QUERY_TIMEOUT_MS "adbc.questdb.query_timeout_ms"
#define QDB_OPT_INGEST_TS "adbc.questdb.ingest.timestamp_column"
#define QDB_OPT_INGEST_SYMBOL_MODE "adbc.questdb.ingest.symbol_mode"
#define QDB_OPT_INGEST_WAIT "adbc.questdb.ingest.wait_for_apply"
#define QDB_OPT_INGEST_APPLY_TIMEOUT_MS "adbc.questdb.ingest.apply_timeout_ms"
#define QDB_OPT_INGEST_PARTITION_BY "adbc.questdb.ingest.partition_by"

#define QDB_CATALOG "qdb"
#define QDB_SCHEMA "public"

// ---------------------------------------------------------------------------
// Small helpers

static char* QdbStrdup(const char* s) {
  if (!s) return NULL;
  size_t n = strlen(s);
  char* out = (char*)malloc(n + 1);
  if (out) memcpy(out, s, n + 1);
  return out;
}

static char* QdbStrndup(const char* s, size_t n) {
  char* out = (char*)malloc(n + 1);
  if (!out) return NULL;
  memcpy(out, s, n);
  out[n] = 0;
  return out;
}

static int64_t QdbNowMs(void) {
#if defined(_WIN32)
  return (int64_t)GetTickCount64();
#else
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
#endif
}

static void QdbSleepMs(int ms) {
#if defined(_WIN32)
  Sleep((DWORD)ms);
#else
  struct timespec ts = {ms / 1000, (long)(ms % 1000) * 1000000L};
  nanosleep(&ts, NULL);
#endif
}

/// Map a client error code onto an ADBC status.
static AdbcStatusCode QdbStatusFor(questdb_error_code code) {
  switch (code) {
    case line_sender_error_server_security_error:
    case line_sender_error_auth_error:
      return ADBC_STATUS_UNAUTHENTICATED;
    case line_sender_error_invalid_api_call:
    case line_sender_error_invalid_utf8:
    case line_sender_error_invalid_name:
    case line_sender_error_invalid_timestamp:
    case line_sender_error_invalid_decimal:
    case line_sender_error_invalid_bind:
    case line_sender_error_config_error:
    case line_sender_error_arrow_unsupported_column_kind:
    case line_sender_error_server_parse_error:
      return ADBC_STATUS_INVALID_ARGUMENT;
    case line_sender_error_query_timeout:
      return ADBC_STATUS_TIMEOUT;
    case line_sender_error_cancelled:
      return ADBC_STATUS_CANCELLED;
    case line_sender_error_unsupported_server:
    case line_sender_error_protocol_version_error:
      return ADBC_STATUS_NOT_IMPLEMENTED;
    case line_sender_error_server_internal_error:
      return ADBC_STATUS_INTERNAL;
    default:
      return ADBC_STATUS_IO;
  }
}

/// Convert and free a client error.  `what` names the operation.
static AdbcStatusCode QdbError(struct AdbcError* error, questdb_error* err, const char* what) {
  if (!err) {
    InternalAdbcSetError(error, "%s failed without a client error", what);
    return ADBC_STATUS_INTERNAL;
  }
  size_t len = 0;
  const char* msg = questdb_error_msg(err, &len);
  questdb_error_code code = questdb_error_get_code(err);
  AdbcStatusCode status = QdbStatusFor(code);
  InternalAdbcSetError(error, "[QuestDB] %s: %.*s", what, (int)len, msg ? msg : "");
  if (error) {
    error->vendor_code = (int32_t)code;
    if (status == ADBC_STATUS_UNAUTHENTICATED) memcpy(error->sqlstate, "28000", 5);
    else if (status == ADBC_STATUS_INVALID_ARGUMENT) memcpy(error->sqlstate, "42000", 5);
    else if (status == ADBC_STATUS_TIMEOUT) memcpy(error->sqlstate, "57014", 5);
    else if (status == ADBC_STATUS_IO) memcpy(error->sqlstate, "08000", 5);
    else memcpy(error->sqlstate, "HY000", 5);
  }
  questdb_error_free(err);
  return status;
}

/// Append `key=value;` to a connect string, doubling ';' inside the value as
/// the QuestDB configuration-string grammar requires.
static int QdbConfAppend(struct InternalAdbcStringBuilder* sb, const char* key, const char* value) {
  if (InternalAdbcStringBuilderAppend(sb, "%s=", key)) return 1;
  for (const char* p = value; *p; p++) {
    if (*p == ';') {
      if (InternalAdbcStringBuilderAppend(sb, ";;")) return 1;
    } else {
      if (InternalAdbcStringBuilderAppend(sb, "%c", *p)) return 1;
    }
  }
  return InternalAdbcStringBuilderAppend(sb, ";");
}

/// Quote an identifier for QuestDB SQL: "name" with embedded quotes doubled.
static char* QdbQuoteIdent(const char* name) {
  size_t n = strlen(name), extra = 0;
  for (size_t i = 0; i < n; i++) if (name[i] == '"') extra++;
  char* out = (char*)malloc(n + extra + 3);
  if (!out) return NULL;
  char* w = out;
  *w++ = '"';
  for (size_t i = 0; i < n; i++) {
    if (name[i] == '"') *w++ = '"';
    *w++ = name[i];
  }
  *w++ = '"';
  *w = 0;
  return out;
}

/// Quote a string literal: 'text' with embedded quotes doubled.
static char* QdbQuoteLiteral(const char* s) {
  size_t n = strlen(s), extra = 0;
  for (size_t i = 0; i < n; i++) if (s[i] == '\'') extra++;
  char* out = (char*)malloc(n + extra + 3);
  if (!out) return NULL;
  char* w = out;
  *w++ = '\'';
  for (size_t i = 0; i < n; i++) {
    if (s[i] == '\'') *w++ = '\'';
    *w++ = s[i];
  }
  *w++ = '\'';
  *w = 0;
  return out;
}

static bool QdbUtf8(line_sender_utf8* out, const char* s, struct AdbcError* error,
                    AdbcStatusCode* status) {
  questdb_error* err = NULL;
  if (!line_sender_utf8_init(out, strlen(s), s, &err)) {
    *status = QdbError(error, err, "UTF-8 validation");
    return false;
  }
  return true;
}

// ---------------------------------------------------------------------------
// Structures

struct QdbDatabase {
  char* uri;
  char* username;
  char* password;
  char* token;
  char* extra_conf;
  char* conf;         // the connect string handed to the client
  questdb_db* pool;   // non-NULL after DatabaseInit
};

struct QdbConnection {
  struct QdbDatabase* db;
  bool connected;
  char* server_version;  // "10.1.0", read lazily
};

/// Cancellation cell shared between a statement and its live result stream;
/// the stream can outlive the statement, so it is reference counted.
struct QdbCancel {
  OdbcMutex lock;
  int refs;
  bool cancelled;
};

static struct QdbCancel* QdbCancelNew(void) {
  struct QdbCancel* c = (struct QdbCancel*)calloc(1, sizeof(*c));
  if (!c) return NULL;
  OdbcMutexInit(&c->lock);
  c->refs = 1;
  return c;
}
static void QdbCancelRetain(struct QdbCancel* c) {
  OdbcMutexLock(&c->lock);
  c->refs++;
  OdbcMutexUnlock(&c->lock);
}
static void QdbCancelRelease(struct QdbCancel* c) {
  if (!c) return;
  OdbcMutexLock(&c->lock);
  int left = --c->refs;
  OdbcMutexUnlock(&c->lock);
  if (left == 0) {
    OdbcMutexDestroy(&c->lock);
    free(c);
  }
}
static bool QdbCancelled(struct QdbCancel* c) {
  OdbcMutexLock(&c->lock);
  bool v = c->cancelled;
  OdbcMutexUnlock(&c->lock);
  return v;
}

enum QdbIngestMode { QDB_INGEST_CREATE, QDB_INGEST_APPEND, QDB_INGEST_REPLACE, QDB_INGEST_CREATE_APPEND };
enum QdbSymbolMode { QDB_SYMBOL_AUTO, QDB_SYMBOL_SYMBOL, QDB_SYMBOL_NOT_SYMBOL };

struct QdbStatement {
  struct QdbConnection* conn;
  char* sql;
  struct ArrowArrayStream bind;
  bool has_bind;
  char* ingest_table;
  enum QdbIngestMode ingest_mode;
  char* ingest_ts;
  char* partition_by;  // DAY by default; NONE for an unpartitioned table
  enum QdbSymbolMode symbol_mode;
  bool wait_for_apply;
  int64_t apply_timeout_ms;
  int64_t query_timeout_ms;
  struct QdbCancel* cancel;
};

/// One execution sequence: the SQL text run once, or once per bound row.
/// Also the private_data of the exported ArrowArrayStream.
struct QdbRun {
  struct QdbDatabase* db;
  struct QdbCancel* cancel;
  char* sql;
  int64_t timeout_ms;

  qwp_reader* reader;
  qwp_reader_cursor* cursor;
  bool started;

  // Bound parameters, iterated one row per execution.
  struct ArrowArrayStream bind;
  bool has_bind;
  struct ArrowSchema bind_schema;
  bool bind_schema_valid;
  struct ArrowArray bind_batch;
  struct ArrowArrayView bind_view;
  bool bind_batch_valid;
  int64_t bind_row;

  // Result schema (from the first batch) and the batch that carried it.
  struct ArrowSchema schema;
  bool have_schema;
  struct ArrowArray first;
  bool have_first;

  // Totals across executions.
  int64_t rows_affected;
  bool rows_known;

  char errmsg[2048];
};

// ---------------------------------------------------------------------------
// Run: executing SQL and streaming Arrow batches

static void QdbRunFreeCursor(struct QdbRun* run) {
  if (run->cursor) {
    qwp_reader_cursor_free(run->cursor);
    run->cursor = NULL;
  }
}

static void QdbRunFree(struct QdbRun* run) {
  if (!run) return;
  QdbRunFreeCursor(run);
  if (run->reader) qwp_reader_close(run->reader);
  if (run->bind_batch_valid) {
    ArrowArrayViewReset(&run->bind_view);
    if (run->bind_batch.release) run->bind_batch.release(&run->bind_batch);
  }
  if (run->bind_schema_valid && run->bind_schema.release) run->bind_schema.release(&run->bind_schema);
  if (run->has_bind && run->bind.release) run->bind.release(&run->bind);
  if (run->have_schema && run->schema.release) run->schema.release(&run->schema);
  if (run->have_first && run->first.release) run->first.release(&run->first);
  QdbCancelRelease(run->cancel);
  free(run->sql);
  free(run);
}

/// Record the cursor's terminal (rows affected) before it is freed.
static void QdbRunAccountTerminal(struct QdbRun* run) {
  if (!run->cursor) return;
  uint8_t op = 0;
  uint64_t n = 0;
  if (qwp_reader_cursor_terminal_exec_done(run->cursor, &op, &n)) {
    if (n != UINT64_MAX) {
      run->rows_affected += (int64_t)n;
      run->rows_known = true;
    }
  }
}

/// Bind one Arrow value (row `row` of column `col` of the bound batch).
static AdbcStatusCode QdbBindValue(qwp_reader_query* q, struct ArrowSchemaView* sv,
                                   struct ArrowArrayView* col, int64_t row, int64_t index,
                                   struct AdbcError* error) {
  if (ArrowArrayViewIsNull(col, row)) {
    switch (sv->type) {
      case NANOARROW_TYPE_BOOL: qwp_reader_query_bind_null(q, qwp_reader_column_kind_boolean); break;
      case NANOARROW_TYPE_INT8: case NANOARROW_TYPE_UINT8: qwp_reader_query_bind_null(q, qwp_reader_column_kind_byte); break;
      case NANOARROW_TYPE_INT16: case NANOARROW_TYPE_UINT16: qwp_reader_query_bind_null(q, qwp_reader_column_kind_short); break;
      case NANOARROW_TYPE_INT32: case NANOARROW_TYPE_UINT32: qwp_reader_query_bind_null(q, qwp_reader_column_kind_int); break;
      case NANOARROW_TYPE_INT64: case NANOARROW_TYPE_UINT64: qwp_reader_query_bind_null(q, qwp_reader_column_kind_long); break;
      case NANOARROW_TYPE_FLOAT: qwp_reader_query_bind_null(q, qwp_reader_column_kind_float); break;
      case NANOARROW_TYPE_DOUBLE: qwp_reader_query_bind_null(q, qwp_reader_column_kind_double); break;
      case NANOARROW_TYPE_TIMESTAMP:
        qwp_reader_query_bind_null(q, sv->time_unit == NANOARROW_TIME_UNIT_NANO
                                          ? qwp_reader_column_kind_timestamp_nanos
                                          : qwp_reader_column_kind_timestamp);
        break;
      case NANOARROW_TYPE_DATE32: case NANOARROW_TYPE_DATE64: qwp_reader_query_bind_null(q, qwp_reader_column_kind_date); break;
      case NANOARROW_TYPE_BINARY: case NANOARROW_TYPE_LARGE_BINARY: qwp_reader_query_bind_null_binary(q); break;
      case NANOARROW_TYPE_FIXED_SIZE_BINARY:
        if (sv->fixed_size == 16) { qwp_reader_query_bind_null(q, qwp_reader_column_kind_uuid); break; }
        qwp_reader_query_bind_null_binary(q);
        break;
      case NANOARROW_TYPE_DECIMAL128: qwp_reader_query_bind_null_decimal128(q, (int8_t)sv->decimal_scale); break;
      default: qwp_reader_query_bind_null_varchar(q); break;
    }
    return ADBC_STATUS_OK;
  }
  switch (sv->type) {
    case NANOARROW_TYPE_BOOL:
      qwp_reader_query_bind_bool(q, ArrowArrayViewGetIntUnsafe(col, row) != 0);
      return ADBC_STATUS_OK;
    case NANOARROW_TYPE_INT8:
      qwp_reader_query_bind_i8(q, (int8_t)ArrowArrayViewGetIntUnsafe(col, row));
      return ADBC_STATUS_OK;
    case NANOARROW_TYPE_INT16:
    case NANOARROW_TYPE_UINT8:
      qwp_reader_query_bind_i16(q, (int16_t)ArrowArrayViewGetIntUnsafe(col, row));
      return ADBC_STATUS_OK;
    case NANOARROW_TYPE_INT32:
    case NANOARROW_TYPE_UINT16:
      qwp_reader_query_bind_i32(q, (int32_t)ArrowArrayViewGetIntUnsafe(col, row));
      return ADBC_STATUS_OK;
    case NANOARROW_TYPE_INT64:
    case NANOARROW_TYPE_UINT32:
      qwp_reader_query_bind_i64(q, ArrowArrayViewGetIntUnsafe(col, row));
      return ADBC_STATUS_OK;
    case NANOARROW_TYPE_UINT64: {
      uint64_t v = ArrowArrayViewGetUIntUnsafe(col, row);
      if (v > (uint64_t)INT64_MAX) {
        InternalAdbcSetError(error, "Parameter %" PRId64 " row %" PRId64 ": %" PRIu64
                             " does not fit a QuestDB LONG", index + 1, row, v);
        return ADBC_STATUS_INVALID_ARGUMENT;
      }
      qwp_reader_query_bind_i64(q, (int64_t)v);
      return ADBC_STATUS_OK;
    }
    case NANOARROW_TYPE_HALF_FLOAT:
    case NANOARROW_TYPE_FLOAT:
      qwp_reader_query_bind_f32(q, (float)ArrowArrayViewGetDoubleUnsafe(col, row));
      return ADBC_STATUS_OK;
    case NANOARROW_TYPE_DOUBLE:
      qwp_reader_query_bind_f64(q, ArrowArrayViewGetDoubleUnsafe(col, row));
      return ADBC_STATUS_OK;
    case NANOARROW_TYPE_STRING:
    case NANOARROW_TYPE_LARGE_STRING:
    case NANOARROW_TYPE_STRING_VIEW: {
      struct ArrowStringView s = ArrowArrayViewGetStringUnsafe(col, row);
      questdb_error* err = NULL;
      line_sender_utf8 u;
      if (!line_sender_utf8_init(&u, (size_t)s.size_bytes, s.data, &err))
        return QdbError(error, err, "bind parameter");
      qwp_reader_query_bind_varchar(q, u);
      return ADBC_STATUS_OK;
    }
    case NANOARROW_TYPE_BINARY:
    case NANOARROW_TYPE_LARGE_BINARY:
    case NANOARROW_TYPE_BINARY_VIEW: {
      struct ArrowBufferView b = ArrowArrayViewGetBytesUnsafe(col, row);
      qwp_reader_query_bind_binary(q, b.data.as_uint8, (size_t)b.size_bytes);
      return ADBC_STATUS_OK;
    }
    case NANOARROW_TYPE_FIXED_SIZE_BINARY: {
      struct ArrowBufferView b = ArrowArrayViewGetBytesUnsafe(col, row);
      if (sv->fixed_size == 16) {
        qwp_reader_query_bind_uuid(q, b.data.as_uint8);
      } else {
        qwp_reader_query_bind_binary(q, b.data.as_uint8, (size_t)b.size_bytes);
      }
      return ADBC_STATUS_OK;
    }
    case NANOARROW_TYPE_TIMESTAMP: {
      int64_t v = ArrowArrayViewGetIntUnsafe(col, row);
      switch (sv->time_unit) {
        case NANOARROW_TIME_UNIT_SECOND: qwp_reader_query_bind_timestamp_micros(q, v * 1000000); break;
        case NANOARROW_TIME_UNIT_MILLI: qwp_reader_query_bind_timestamp_micros(q, v * 1000); break;
        case NANOARROW_TIME_UNIT_MICRO: qwp_reader_query_bind_timestamp_micros(q, v); break;
        case NANOARROW_TIME_UNIT_NANO: qwp_reader_query_bind_timestamp_nanos(q, v); break;
      }
      return ADBC_STATUS_OK;
    }
    case NANOARROW_TYPE_DATE32:
      qwp_reader_query_bind_date_millis(q, ArrowArrayViewGetIntUnsafe(col, row) * 86400000LL);
      return ADBC_STATUS_OK;
    case NANOARROW_TYPE_DATE64:
      qwp_reader_query_bind_date_millis(q, ArrowArrayViewGetIntUnsafe(col, row));
      return ADBC_STATUS_OK;
    case NANOARROW_TYPE_DECIMAL128: {
      struct ArrowDecimal dec;
      ArrowDecimalInit(&dec, 128, sv->decimal_precision, sv->decimal_scale);
      ArrowArrayViewGetDecimalUnsafe(col, row, &dec);
      uint64_t lo = dec.words[dec.low_word_index];
      int64_t hi = (int64_t)dec.words[dec.high_word_index];
      qwp_reader_query_bind_decimal128(q, lo, hi, (int8_t)sv->decimal_scale);
      return ADBC_STATUS_OK;
    }
    case NANOARROW_TYPE_DICTIONARY: {
      // Dictionary-encoded strings (e.g. a SYMBOL column read back): bind the value.
      int64_t idx = ArrowArrayViewGetIntUnsafe(col, row);
      struct ArrowStringView s = ArrowArrayViewGetStringUnsafe(col->dictionary, idx);
      questdb_error* err = NULL;
      line_sender_utf8 u;
      if (!line_sender_utf8_init(&u, (size_t)s.size_bytes, s.data, &err))
        return QdbError(error, err, "bind parameter");
      qwp_reader_query_bind_varchar(q, u);
      return ADBC_STATUS_OK;
    }
    default:
      InternalAdbcSetError(error, "Parameter %" PRId64 ": Arrow type %s cannot be bound over QWP",
                           index + 1, ArrowTypeString(sv->type));
      return ADBC_STATUS_NOT_IMPLEMENTED;
  }
}

/// Move the bind iterator to the next row; false (and no error) when done.
static bool QdbRunNextBindRow(struct QdbRun* run, struct AdbcError* error, AdbcStatusCode* status) {
  *status = ADBC_STATUS_OK;
  for (;;) {
    if (run->bind_batch_valid && run->bind_row + 1 < run->bind_batch.length) {
      run->bind_row++;
      return true;
    }
    if (run->bind_batch_valid) {
      ArrowArrayViewReset(&run->bind_view);
      if (run->bind_batch.release) run->bind_batch.release(&run->bind_batch);
      run->bind_batch_valid = false;
    }
    if (!run->bind_schema_valid) {
      int rc = run->bind.get_schema(&run->bind, &run->bind_schema);
      if (rc != 0) {
        InternalAdbcSetError(error, "Bound parameter stream: get_schema failed: %s",
                             run->bind.get_last_error(&run->bind));
        *status = ADBC_STATUS_INVALID_ARGUMENT;
        return false;
      }
      run->bind_schema_valid = true;
    }
    int rc = run->bind.get_next(&run->bind, &run->bind_batch);
    if (rc != 0) {
      InternalAdbcSetError(error, "Bound parameter stream: get_next failed: %s",
                           run->bind.get_last_error(&run->bind));
      *status = ADBC_STATUS_INVALID_ARGUMENT;
      return false;
    }
    if (!run->bind_batch.release) return false;  // end of stream
    struct ArrowError na_error;
    if (ArrowArrayViewInitFromSchema(&run->bind_view, &run->bind_schema, &na_error) != NANOARROW_OK ||
        ArrowArrayViewSetArray(&run->bind_view, &run->bind_batch, &na_error) != NANOARROW_OK) {
      InternalAdbcSetError(error, "Bound parameters: %s", na_error.message);
      run->bind_batch.release(&run->bind_batch);
      *status = ADBC_STATUS_INVALID_ARGUMENT;
      return false;
    }
    run->bind_batch_valid = true;
    run->bind_row = -1;
    if (run->bind_batch.length == 0) continue;
  }
}

/// Start the next execution: the plain SQL once, or the SQL with the next
/// bound row.  Returns false with status OK when there is nothing left.
static bool QdbRunNextExecution(struct QdbRun* run, struct AdbcError* error, AdbcStatusCode* status) {
  *status = ADBC_STATUS_OK;
  QdbRunAccountTerminal(run);
  QdbRunFreeCursor(run);
  if (QdbCancelled(run->cancel)) {
    InternalAdbcSetError(error, "Statement cancelled");
    *status = ADBC_STATUS_CANCELLED;
    return false;
  }
  line_sender_utf8 sql;
  if (!QdbUtf8(&sql, run->sql, error, status)) return false;
  questdb_error* err = NULL;

  if (!run->has_bind) {
    if (run->started) return false;
    run->started = true;
    if (run->timeout_ms > 0) {
      qwp_reader_query* q = qwp_reader_prepare(run->reader, sql, &err);
      if (!q) { *status = QdbError(error, err, "prepare"); return false; }
      qwp_reader_query_set_timeout_ms(q, (uint64_t)run->timeout_ms);
      run->cursor = qwp_reader_query_execute(&q, &err);
    } else {
      run->cursor = qwp_reader_execute(run->reader, sql, &err);
    }
    if (!run->cursor) { *status = QdbError(error, err, "execute"); return false; }
    return true;
  }

  if (!QdbRunNextBindRow(run, error, status)) return false;
  run->started = true;
  qwp_reader_query* q = qwp_reader_prepare(run->reader, sql, &err);
  if (!q) { *status = QdbError(error, err, "prepare"); return false; }
  if (run->timeout_ms > 0) qwp_reader_query_set_timeout_ms(q, (uint64_t)run->timeout_ms);
  for (int64_t c = 0; c < run->bind_schema.n_children; c++) {
    struct ArrowSchemaView sv;
    struct ArrowError na_error;
    if (ArrowSchemaViewInit(&sv, run->bind_schema.children[c], &na_error) != NANOARROW_OK) {
      InternalAdbcSetError(error, "Bound parameter %" PRId64 ": %s", c + 1, na_error.message);
      qwp_reader_query_free(q);
      *status = ADBC_STATUS_INVALID_ARGUMENT;
      return false;
    }
    *status = QdbBindValue(q, &sv, run->bind_view.children[c], run->bind_row, c, error);
    if (*status != ADBC_STATUS_OK) {
      qwp_reader_query_free(q);
      return false;
    }
  }
  run->cursor = qwp_reader_query_execute(&q, &err);
  if (!run->cursor) { *status = QdbError(error, err, "execute"); return false; }
  return true;
}

/// Fetch the next Arrow batch across executions.  `out` is left released at
/// the end of the whole run.
static AdbcStatusCode QdbRunNextArrow(struct QdbRun* run, struct ArrowArray* out, struct AdbcError* error) {
  out->release = NULL;
  for (;;) {
    if (!run->cursor) {
      AdbcStatusCode status;
      if (!QdbRunNextExecution(run, error, &status)) return status;
    }
    if (QdbCancelled(run->cancel)) {
      questdb_error* err = NULL;
      qwp_reader_cursor_cancel(run->cursor, &err);
      if (err) questdb_error_free(err);
      InternalAdbcSetError(error, "Statement cancelled");
      return ADBC_STATUS_CANCELLED;
    }
    struct ArrowArray arr;
    struct ArrowSchema sch;
    memset(&arr, 0, sizeof(arr));
    memset(&sch, 0, sizeof(sch));
    questdb_error* err = NULL;
    qwp_reader_arrow_batch_result rc = qwp_reader_cursor_next_arrow_batch(run->cursor, &arr, &sch, &err);
    if (rc == qwp_reader_arrow_batch_ok) {
      if (!run->have_schema) {
        run->schema = sch;
        run->have_schema = true;
      } else if (sch.release) {
        sch.release(&sch);
      }
      *out = arr;
      return ADBC_STATUS_OK;
    }
    if (rc == qwp_reader_arrow_batch_error) return QdbError(error, err, "fetch");
    // End of this execution: account for it and move on.
    QdbRunAccountTerminal(run);
    {
      uint64_t seq = 0, total = 0;
      if (qwp_reader_cursor_terminal_end(run->cursor, &seq, &total)) {
        run->rows_affected += (int64_t)total;
        run->rows_known = true;
      }
    }
    QdbRunFreeCursor(run);
  }
}

/// Drain the whole run without decoding Arrow (ExecuteUpdate).
static AdbcStatusCode QdbRunDrain(struct QdbRun* run, struct AdbcError* error) {
  for (;;) {
    if (!run->cursor) {
      AdbcStatusCode status;
      if (!QdbRunNextExecution(run, error, &status)) return status;
    }
    questdb_error* err = NULL;
    const qwp_reader_batch* b;
    while ((b = qwp_reader_cursor_next_batch(run->cursor, &err)) != NULL) {
      if (QdbCancelled(run->cancel)) {
        questdb_error* cerr = NULL;
        qwp_reader_cursor_cancel(run->cursor, &cerr);
        if (cerr) questdb_error_free(cerr);
        InternalAdbcSetError(error, "Statement cancelled");
        return ADBC_STATUS_CANCELLED;
      }
    }
    if (err) return QdbError(error, err, "execute");
    QdbRunAccountTerminal(run);
    {
      uint64_t seq = 0, total = 0;
      if (qwp_reader_cursor_terminal_end(run->cursor, &seq, &total)) {
        run->rows_affected += (int64_t)total;
        run->rows_known = true;
      }
    }
    QdbRunFreeCursor(run);
  }
}

static struct QdbRun* QdbRunNew(struct QdbStatement* stmt, struct AdbcError* error, AdbcStatusCode* status) {
  struct QdbRun* run = (struct QdbRun*)calloc(1, sizeof(*run));
  if (!run) { *status = ADBC_STATUS_INTERNAL; return NULL; }
  run->db = stmt->conn->db;
  run->sql = QdbStrdup(stmt->sql);
  run->timeout_ms = stmt->query_timeout_ms;
  run->cancel = stmt->cancel;
  QdbCancelRetain(run->cancel);
  if (stmt->has_bind) {
    run->bind = stmt->bind;
    run->has_bind = true;
    memset(&stmt->bind, 0, sizeof(stmt->bind));
    stmt->has_bind = false;
  }
  questdb_error* err = NULL;
  run->reader = questdb_db_borrow_reader(run->db->pool, &err);
  if (!run->reader) {
    *status = QdbError(error, err, "borrow a QWP reader");
    QdbRunFree(run);
    return NULL;
  }
  *status = ADBC_STATUS_OK;
  return run;
}

// --- exported ArrowArrayStream ----------------------------------------------

static int QdbStreamGetSchema(struct ArrowArrayStream* stream, struct ArrowSchema* out) {
  struct QdbRun* run = (struct QdbRun*)stream->private_data;
  if (!run) return EINVAL;
  return ArrowSchemaDeepCopy(&run->schema, out);
}

static int QdbStreamGetNext(struct ArrowArrayStream* stream, struct ArrowArray* out) {
  struct QdbRun* run = (struct QdbRun*)stream->private_data;
  if (!run) return EINVAL;
  if (run->have_first) {
    *out = run->first;
    run->have_first = false;
    memset(&run->first, 0, sizeof(run->first));
    return 0;
  }
  struct AdbcError error;
  memset(&error, 0, sizeof(error));
  AdbcStatusCode status = QdbRunNextArrow(run, out, &error);
  if (status != ADBC_STATUS_OK) {
    snprintf(run->errmsg, sizeof(run->errmsg), "%s", error.message ? error.message : "fetch failed");
    if (error.release) error.release(&error);
    out->release = NULL;
    return InternalAdbcStatusCodeToErrno(status);
  }
  return 0;
}

static const char* QdbStreamGetLastError(struct ArrowArrayStream* stream) {
  struct QdbRun* run = (struct QdbRun*)stream->private_data;
  return run ? run->errmsg : "";
}

static void QdbStreamRelease(struct ArrowArrayStream* stream) {
  struct QdbRun* run = (struct QdbRun*)stream->private_data;
  if (run) {
    if (run->cursor) {
      questdb_error* err = NULL;
      qwp_reader_cursor_cancel(run->cursor, &err);
      if (err) questdb_error_free(err);
    }
    QdbRunFree(run);
  }
  stream->private_data = NULL;
  stream->release = NULL;
}

/// Prime the run (learn the schema from the first batch) and export it.
static AdbcStatusCode QdbRunExport(struct QdbRun* run, struct ArrowArrayStream* out,
                                   struct AdbcError* error) {
  AdbcStatusCode status = QdbRunNextArrow(run, &run->first, error);
  if (status != ADBC_STATUS_OK) {
    QdbRunFree(run);
    return status;
  }
  run->have_first = run->first.release != NULL;
  if (!run->have_schema) {
    // No batch at all: an empty result with no columns to describe.
    ArrowSchemaInit(&run->schema);
    CHECK_NA(INTERNAL, ArrowSchemaSetTypeStruct(&run->schema, 0), error);
    run->have_schema = true;
  }
  out->get_schema = QdbStreamGetSchema;
  out->get_next = QdbStreamGetNext;
  out->get_last_error = QdbStreamGetLastError;
  out->release = QdbStreamRelease;
  out->private_data = run;
  return ADBC_STATUS_OK;
}

// ---------------------------------------------------------------------------
// Internal queries (catalog, version): run SQL and visit rows

typedef AdbcStatusCode (*QdbRowFn)(const struct ArrowSchema* schema, struct ArrowArrayView* batch,
                                   int64_t row, void* ctx, struct AdbcError* error);

static AdbcStatusCode QdbQueryRows(struct QdbConnection* conn, const char* sql, QdbRowFn fn, void* ctx,
                                   struct AdbcError* error) {
  questdb_error* err = NULL;
  qwp_reader* reader = questdb_db_borrow_reader(conn->db->pool, &err);
  if (!reader) return QdbError(error, err, "borrow a QWP reader");
  AdbcStatusCode status = ADBC_STATUS_OK;
  line_sender_utf8 u;
  if (!QdbUtf8(&u, sql, error, &status)) { qwp_reader_close(reader); return status; }
  qwp_reader_cursor* cursor = qwp_reader_execute(reader, u, &err);
  if (!cursor) {
    status = QdbError(error, err, sql);
    qwp_reader_close(reader);
    return status;
  }
  struct ArrowSchema schema;
  bool have_schema = false;
  struct ArrowArrayView view;
  for (;;) {
    struct ArrowArray arr;
    struct ArrowSchema sch;
    memset(&arr, 0, sizeof(arr));
    memset(&sch, 0, sizeof(sch));
    qwp_reader_arrow_batch_result rc = qwp_reader_cursor_next_arrow_batch(cursor, &arr, &sch, &err);
    if (rc == qwp_reader_arrow_batch_end) break;
    if (rc == qwp_reader_arrow_batch_error) { status = QdbError(error, err, sql); break; }
    if (!have_schema) { schema = sch; have_schema = true; }
    else if (sch.release) sch.release(&sch);
    struct ArrowError na_error;
    if (ArrowArrayViewInitFromSchema(&view, &schema, &na_error) != NANOARROW_OK ||
        ArrowArrayViewSetArray(&view, &arr, &na_error) != NANOARROW_OK) {
      InternalAdbcSetError(error, "%s: %s", sql, na_error.message);
      status = ADBC_STATUS_INTERNAL;
      if (arr.release) arr.release(&arr);
      break;
    }
    for (int64_t r = 0; r < arr.length && status == ADBC_STATUS_OK; r++) {
      status = fn(&schema, &view, r, ctx, error);
    }
    ArrowArrayViewReset(&view);
    if (arr.release) arr.release(&arr);
    if (status != ADBC_STATUS_OK) break;
  }
  if (have_schema && schema.release) schema.release(&schema);
  if (status != ADBC_STATUS_OK) {
    questdb_error* cerr = NULL;
    qwp_reader_cursor_cancel(cursor, &cerr);
    if (cerr) questdb_error_free(cerr);
  }
  qwp_reader_cursor_free(cursor);
  qwp_reader_close(reader);
  return status;
}

static int64_t QdbColIndex(const struct ArrowSchema* schema, const char* name) {
  for (int64_t i = 0; i < schema->n_children; i++) {
    if (schema->children[i]->name && strcmp(schema->children[i]->name, name) == 0) return i;
  }
  return -1;
}

/// A string cell as a malloc'd C string (NULL for SQL NULL); handles
/// dictionary-encoded (SYMBOL) columns too.
static char* QdbCellString(struct ArrowArrayView* batch, int64_t col, int64_t row) {
  if (col < 0) return NULL;
  struct ArrowArrayView* c = batch->children[col];
  if (ArrowArrayViewIsNull(c, row)) return NULL;
  struct ArrowStringView s;
  if (c->dictionary) {
    int64_t idx = ArrowArrayViewGetIntUnsafe(c, row);
    s = ArrowArrayViewGetStringUnsafe(c->dictionary, idx);
  } else if (c->storage_type == NANOARROW_TYPE_STRING || c->storage_type == NANOARROW_TYPE_LARGE_STRING ||
             c->storage_type == NANOARROW_TYPE_STRING_VIEW) {
    s = ArrowArrayViewGetStringUnsafe(c, row);
  } else if (c->storage_type == NANOARROW_TYPE_BOOL) {
    return QdbStrdup(ArrowArrayViewGetIntUnsafe(c, row) ? "true" : "false");
  } else {
    char buf[64];
    snprintf(buf, sizeof(buf), "%" PRId64, ArrowArrayViewGetIntUnsafe(c, row));
    return QdbStrdup(buf);
  }
  return QdbStrndup(s.data, (size_t)s.size_bytes);
}

static bool QdbCellBool(struct ArrowArrayView* batch, int64_t col, int64_t row) {
  if (col < 0) return false;
  struct ArrowArrayView* c = batch->children[col];
  if (ArrowArrayViewIsNull(c, row)) return false;
  if (c->storage_type == NANOARROW_TYPE_BOOL) return ArrowArrayViewGetIntUnsafe(c, row) != 0;
  char* s = QdbCellString(batch, col, row);
  bool v = s && (strcmp(s, "true") == 0 || strcmp(s, "TRUE") == 0 || strcmp(s, "1") == 0);
  free(s);
  return v;
}

// --- server version ---------------------------------------------------------

static AdbcStatusCode QdbVersionRow(const struct ArrowSchema* schema, struct ArrowArrayView* batch,
                                    int64_t row, void* ctx, struct AdbcError* error) {
  char** out = (char**)ctx;
  if (*out) return ADBC_STATUS_OK;
  char* s = QdbCellString(batch, 0, row);
  if (!s) return ADBC_STATUS_OK;
  // "Build Information: QuestDB 10.1.0, JDK 25.0.2, Commit Hash ..."
  const char* p = strstr(s, "QuestDB ");
  if (p) {
    p += 8;
    size_t n = strcspn(p, ", ");
    *out = QdbStrndup(p, n);
  } else {
    *out = QdbStrdup(s);
  }
  free(s);
  return ADBC_STATUS_OK;
}

static const char* QdbServerVersion(struct QdbConnection* conn) {
  if (!conn->server_version) {
    struct AdbcError error;
    memset(&error, 0, sizeof(error));
    if (QdbQueryRows(conn, "SELECT build()", QdbVersionRow, &conn->server_version, &error) != ADBC_STATUS_OK) {
      if (error.release) error.release(&error);
    }
    if (!conn->server_version) conn->server_version = QdbStrdup("unknown");
  }
  return conn->server_version;
}

// ---------------------------------------------------------------------------
// Database

static AdbcStatusCode QdbDatabaseNew(struct AdbcDatabase* database, struct AdbcError* error) {
  struct QdbDatabase* db = (struct QdbDatabase*)calloc(1, sizeof(*db));
  if (!db) { InternalAdbcSetError(error, "Out of memory"); return ADBC_STATUS_INTERNAL; }
  database->private_data = db;
  return ADBC_STATUS_OK;
}

static AdbcStatusCode QdbDatabaseSetOption(struct AdbcDatabase* database, const char* key,
                                           const char* value, struct AdbcError* error) {
  struct QdbDatabase* db = (struct QdbDatabase*)database->private_data;
  if (!db) return ADBC_STATUS_INVALID_STATE;
  if (db->pool) {
    InternalAdbcSetError(error, "Database options cannot be changed after AdbcDatabaseInit");
    return ADBC_STATUS_INVALID_STATE;
  }
  char** slot = NULL;
  if (strcmp(key, QDB_OPT_URI) == 0 || strcmp(key, "adbc.questdb.uri") == 0) slot = &db->uri;
  else if (strcmp(key, QDB_OPT_USERNAME) == 0) slot = &db->username;
  else if (strcmp(key, QDB_OPT_PASSWORD) == 0) slot = &db->password;
  else if (strcmp(key, QDB_OPT_TOKEN) == 0) slot = &db->token;
  else if (strcmp(key, QDB_OPT_CONF) == 0) slot = &db->extra_conf;
  else {
    InternalAdbcSetError(error, "Unknown database option %s", key);
    return ADBC_STATUS_NOT_IMPLEMENTED;
  }
  free(*slot);
  *slot = value ? QdbStrdup(value) : NULL;
  return ADBC_STATUS_OK;
}

static AdbcStatusCode QdbGetStringOption(const char* value, char* out, size_t* length,
                                         struct AdbcError* error) {
  if (!value) return ADBC_STATUS_NOT_FOUND;
  size_t n = strlen(value) + 1;
  if (*length >= n && out) memcpy(out, value, n);
  *length = n;
  return ADBC_STATUS_OK;
}

static AdbcStatusCode QdbDatabaseGetOption(struct AdbcDatabase* database, const char* key, char* value,
                                           size_t* length, struct AdbcError* error) {
  struct QdbDatabase* db = (struct QdbDatabase*)database->private_data;
  if (!db) return ADBC_STATUS_INVALID_STATE;
  if (strcmp(key, QDB_OPT_URI) == 0) return QdbGetStringOption(db->uri, value, length, error);
  if (strcmp(key, QDB_OPT_USERNAME) == 0) return QdbGetStringOption(db->username, value, length, error);
  if (strcmp(key, QDB_OPT_CONF) == 0) return QdbGetStringOption(db->extra_conf, value, length, error);
  InternalAdbcSetError(error, "Unknown database option %s", key);
  return ADBC_STATUS_NOT_FOUND;
}

static AdbcStatusCode QdbNotImplementedOption(const char* kind, const char* key, struct AdbcError* error) {
  InternalAdbcSetError(error, "Unknown %s option %s", kind, key);
  return ADBC_STATUS_NOT_IMPLEMENTED;
}
static AdbcStatusCode QdbDatabaseSetOptionBytes(struct AdbcDatabase* d, const char* key, const uint8_t* v,
                                                size_t n, struct AdbcError* e) { return QdbNotImplementedOption("database", key, e); }
static AdbcStatusCode QdbDatabaseSetOptionInt(struct AdbcDatabase* d, const char* key, int64_t v,
                                              struct AdbcError* e) { return QdbNotImplementedOption("database", key, e); }
static AdbcStatusCode QdbDatabaseSetOptionDouble(struct AdbcDatabase* d, const char* key, double v,
                                                 struct AdbcError* e) { return QdbNotImplementedOption("database", key, e); }
static AdbcStatusCode QdbDatabaseGetOptionBytes(struct AdbcDatabase* d, const char* key, uint8_t* v,
                                                size_t* n, struct AdbcError* e) { return QdbNotImplementedOption("database", key, e); }
static AdbcStatusCode QdbDatabaseGetOptionInt(struct AdbcDatabase* d, const char* key, int64_t* v,
                                              struct AdbcError* e) { return QdbNotImplementedOption("database", key, e); }
static AdbcStatusCode QdbDatabaseGetOptionDouble(struct AdbcDatabase* d, const char* key, double* v,
                                                 struct AdbcError* e) { return QdbNotImplementedOption("database", key, e); }

/// Build the client connect string from the options.
static AdbcStatusCode QdbBuildConf(struct QdbDatabase* db, struct AdbcError* error) {
  if (!db->uri) {
    InternalAdbcSetError(error, "The \"uri\" option is required, e.g. ws://localhost:9000");
    return ADBC_STATUS_INVALID_ARGUMENT;
  }
  const char* scheme = "ws";
  const char* rest = db->uri;
  if (strncmp(rest, "ws://", 5) == 0) { rest += 5; }
  else if (strncmp(rest, "wss://", 6) == 0) { scheme = "wss"; rest += 6; }
  else if (strncmp(rest, "http://", 7) == 0) { rest += 7; }
  else if (strncmp(rest, "https://", 8) == 0) { scheme = "wss"; rest += 8; }
  else if (strncmp(rest, "questdb://", 10) == 0) { rest += 10; }
  else if (strstr(rest, "://")) {
    InternalAdbcSetError(error, "Unsupported URI scheme in %s: use ws://host:port or wss://host:port", db->uri);
    return ADBC_STATUS_INVALID_ARGUMENT;
  }
  size_t hostlen = strcspn(rest, "/?#");
  char* hostport = QdbStrndup(rest, hostlen);
  if (!hostport) return ADBC_STATUS_INTERNAL;
  if (hostlen == 0) { free(hostport); InternalAdbcSetError(error, "No host in URI %s", db->uri); return ADBC_STATUS_INVALID_ARGUMENT; }
  bool has_port = false;
  {
    const char* colon = strrchr(hostport, ':');
    const char* bracket = strrchr(hostport, ']');
    has_port = colon && (!bracket || colon > bracket);
  }
  struct InternalAdbcStringBuilder sb;
  if (InternalAdbcStringBuilderInit(&sb, 256)) { free(hostport); return ADBC_STATUS_INTERNAL; }
  int rc = InternalAdbcStringBuilderAppend(&sb, "%s::addr=%s%s;", scheme, hostport, has_port ? "" : ":9000");
  free(hostport);
  if (!rc && db->username) rc = QdbConfAppend(&sb, "username", db->username);
  if (!rc && db->password) rc = QdbConfAppend(&sb, "password", db->password);
  if (!rc && db->token) rc = QdbConfAppend(&sb, "token", db->token);
  // Readers connect on first borrow, so a bad address or credential surfaces
  // from AdbcConnectionInit (which borrows one) rather than from here.
  if (!rc) rc = InternalAdbcStringBuilderAppend(&sb, "lazy_connect=true;");
  if (!rc && db->extra_conf && *db->extra_conf) {
    size_t n = strlen(db->extra_conf);
    rc = InternalAdbcStringBuilderAppend(&sb, "%s%s", db->extra_conf, db->extra_conf[n - 1] == ';' ? "" : ";");
  }
  if (rc) { InternalAdbcStringBuilderReset(&sb); return ADBC_STATUS_INTERNAL; }
  free(db->conf);
  db->conf = QdbStrdup(sb.buffer);
  InternalAdbcStringBuilderReset(&sb);
  return db->conf ? ADBC_STATUS_OK : ADBC_STATUS_INTERNAL;
}

static AdbcStatusCode QdbDatabaseInit(struct AdbcDatabase* database, struct AdbcError* error) {
  struct QdbDatabase* db = (struct QdbDatabase*)database->private_data;
  if (!db) return ADBC_STATUS_INVALID_STATE;
  if (db->pool) return ADBC_STATUS_INVALID_STATE;
  RAISE_ADBC(QdbBuildConf(db, error));
  questdb_error* err = NULL;
  db->pool = questdb_db_connect(db->conf, strlen(db->conf), &err);
  if (!db->pool) return QdbError(error, err, "open the QWP connection pool");
  return ADBC_STATUS_OK;
}

static AdbcStatusCode QdbDatabaseRelease(struct AdbcDatabase* database, struct AdbcError* error) {
  struct QdbDatabase* db = (struct QdbDatabase*)database->private_data;
  if (!db) return ADBC_STATUS_INVALID_STATE;
  if (db->pool) questdb_db_close(db->pool);
  free(db->uri); free(db->username); free(db->password); free(db->token); free(db->extra_conf); free(db->conf);
  free(db);
  database->private_data = NULL;
  return ADBC_STATUS_OK;
}

// ---------------------------------------------------------------------------
// Connection

static AdbcStatusCode QdbConnectionNew(struct AdbcConnection* connection, struct AdbcError* error) {
  struct QdbConnection* conn = (struct QdbConnection*)calloc(1, sizeof(*conn));
  if (!conn) { InternalAdbcSetError(error, "Out of memory"); return ADBC_STATUS_INTERNAL; }
  connection->private_data = conn;
  return ADBC_STATUS_OK;
}

static AdbcStatusCode QdbConnectionSetOption(struct AdbcConnection* connection, const char* key,
                                             const char* value, struct AdbcError* error) {
  struct QdbConnection* conn = (struct QdbConnection*)connection->private_data;
  if (!conn) return ADBC_STATUS_INVALID_STATE;
  if (strcmp(key, ADBC_CONNECTION_OPTION_AUTOCOMMIT) == 0) {
    if (value && strcmp(value, ADBC_OPTION_VALUE_ENABLED) == 0) return ADBC_STATUS_OK;
    InternalAdbcSetError(error, "QuestDB has no transactions over QWP: autocommit cannot be disabled");
    return ADBC_STATUS_NOT_IMPLEMENTED;
  }
  if (strcmp(key, ADBC_CONNECTION_OPTION_CURRENT_CATALOG) == 0 ||
      strcmp(key, ADBC_CONNECTION_OPTION_CURRENT_DB_SCHEMA) == 0) {
    const char* want = strcmp(key, ADBC_CONNECTION_OPTION_CURRENT_CATALOG) == 0 ? QDB_CATALOG : QDB_SCHEMA;
    if (value && strcmp(value, want) == 0) return ADBC_STATUS_OK;
    InternalAdbcSetError(error, "QuestDB has a single namespace (%s.%s); cannot switch to %s", QDB_CATALOG,
                         QDB_SCHEMA, value ? value : "(null)");
    return ADBC_STATUS_NOT_IMPLEMENTED;
  }
  if (strcmp(key, ADBC_CONNECTION_OPTION_ISOLATION_LEVEL) == 0 ||
      strcmp(key, ADBC_CONNECTION_OPTION_READ_ONLY) == 0) {
    InternalAdbcSetError(error, "Connection option %s is not supported over QWP", key);
    return ADBC_STATUS_NOT_IMPLEMENTED;
  }
  InternalAdbcSetError(error, "Unknown connection option %s", key);
  return ADBC_STATUS_NOT_IMPLEMENTED;
}

static AdbcStatusCode QdbConnectionGetOption(struct AdbcConnection* connection, const char* key, char* value,
                                             size_t* length, struct AdbcError* error) {
  struct QdbConnection* conn = (struct QdbConnection*)connection->private_data;
  if (!conn) return ADBC_STATUS_INVALID_STATE;
  if (strcmp(key, ADBC_CONNECTION_OPTION_AUTOCOMMIT) == 0)
    return QdbGetStringOption(ADBC_OPTION_VALUE_ENABLED, value, length, error);
  if (strcmp(key, ADBC_CONNECTION_OPTION_CURRENT_CATALOG) == 0)
    return QdbGetStringOption(QDB_CATALOG, value, length, error);
  if (strcmp(key, ADBC_CONNECTION_OPTION_CURRENT_DB_SCHEMA) == 0)
    return QdbGetStringOption(QDB_SCHEMA, value, length, error);
  InternalAdbcSetError(error, "Unknown connection option %s", key);
  return ADBC_STATUS_NOT_FOUND;
}

static AdbcStatusCode QdbConnectionSetOptionBytes(struct AdbcConnection* c, const char* key, const uint8_t* v,
                                                  size_t n, struct AdbcError* e) { return QdbNotImplementedOption("connection", key, e); }
static AdbcStatusCode QdbConnectionSetOptionInt(struct AdbcConnection* c, const char* key, int64_t v,
                                                struct AdbcError* e) { return QdbNotImplementedOption("connection", key, e); }
static AdbcStatusCode QdbConnectionSetOptionDouble(struct AdbcConnection* c, const char* key, double v,
                                                   struct AdbcError* e) { return QdbNotImplementedOption("connection", key, e); }
static AdbcStatusCode QdbConnectionGetOptionBytes(struct AdbcConnection* c, const char* key, uint8_t* v,
                                                  size_t* n, struct AdbcError* e) { return QdbNotImplementedOption("connection", key, e); }
static AdbcStatusCode QdbConnectionGetOptionInt(struct AdbcConnection* c, const char* key, int64_t* v,
                                                struct AdbcError* e) { return QdbNotImplementedOption("connection", key, e); }
static AdbcStatusCode QdbConnectionGetOptionDouble(struct AdbcConnection* c, const char* key, double* v,
                                                   struct AdbcError* e) { return QdbNotImplementedOption("connection", key, e); }

static AdbcStatusCode QdbConnectionInit(struct AdbcConnection* connection, struct AdbcDatabase* database,
                                        struct AdbcError* error) {
  struct QdbConnection* conn = (struct QdbConnection*)connection->private_data;
  struct QdbDatabase* db = (struct QdbDatabase*)database->private_data;
  if (!conn || !db) return ADBC_STATUS_INVALID_STATE;
  if (conn->connected) return ADBC_STATUS_INVALID_STATE;
  if (!db->pool) {
    InternalAdbcSetError(error, "AdbcDatabaseInit has not been called");
    return ADBC_STATUS_INVALID_STATE;
  }
  // Borrow and return one reader: this is where the WebSocket is opened and
  // authenticated, so connection errors surface here.
  questdb_error* err = NULL;
  qwp_reader* reader = questdb_db_borrow_reader(db->pool, &err);
  if (!reader) return QdbError(error, err, "connect");
  qwp_reader_close(reader);
  conn->db = db;
  conn->connected = true;
  return ADBC_STATUS_OK;
}

static AdbcStatusCode QdbConnectionRelease(struct AdbcConnection* connection, struct AdbcError* error) {
  struct QdbConnection* conn = (struct QdbConnection*)connection->private_data;
  if (!conn) return ADBC_STATUS_INVALID_STATE;
  free(conn->server_version);
  free(conn);
  connection->private_data = NULL;
  return ADBC_STATUS_OK;
}

static AdbcStatusCode QdbConnectionCommit(struct AdbcConnection* connection, struct AdbcError* error) {
  InternalAdbcSetError(error, "No transaction is open: QuestDB over QWP is always in autocommit mode");
  return ADBC_STATUS_INVALID_STATE;
}
static AdbcStatusCode QdbConnectionRollback(struct AdbcConnection* connection, struct AdbcError* error) {
  return QdbConnectionCommit(connection, error);
}

static AdbcStatusCode QdbConnectionCancel(struct AdbcConnection* connection, struct AdbcError* error) {
  InternalAdbcSetError(error, "Cancel the statement instead: AdbcStatementCancel");
  return ADBC_STATUS_NOT_IMPLEMENTED;
}

static AdbcStatusCode QdbConnectionGetInfo(struct AdbcConnection* connection, const uint32_t* info_codes,
                                           size_t info_codes_length, struct ArrowArrayStream* out,
                                           struct AdbcError* error) {
  struct QdbConnection* conn = (struct QdbConnection*)connection->private_data;
  if (!conn || !conn->connected) return ADBC_STATUS_INVALID_STATE;
  static const uint32_t kAll[] = {
      ADBC_INFO_VENDOR_NAME,    ADBC_INFO_VENDOR_VERSION,       ADBC_INFO_VENDOR_ARROW_VERSION,
      ADBC_INFO_VENDOR_SQL,     ADBC_INFO_VENDOR_SUBSTRAIT,     ADBC_INFO_DRIVER_NAME,
      ADBC_INFO_DRIVER_VERSION, ADBC_INFO_DRIVER_ARROW_VERSION, ADBC_INFO_DRIVER_ADBC_VERSION,
  };
  if (!info_codes) {
    info_codes = kAll;
    info_codes_length = sizeof(kAll) / sizeof(kAll[0]);
  }
  struct ArrowSchema schema;
  struct ArrowArray array;
  RAISE_ADBC(InternalAdbcInitConnectionGetInfoSchema(&schema, &array, error));
  for (size_t i = 0; i < info_codes_length; i++) {
    switch (info_codes[i]) {
      case ADBC_INFO_VENDOR_NAME:
        RAISE_ADBC(InternalAdbcConnectionGetInfoAppendString(&array, info_codes[i], "QuestDB", error));
        break;
      case ADBC_INFO_VENDOR_VERSION:
        RAISE_ADBC(InternalAdbcConnectionGetInfoAppendString(&array, info_codes[i], QdbServerVersion(conn), error));
        break;
      case ADBC_INFO_VENDOR_ARROW_VERSION:
        RAISE_ADBC(InternalAdbcConnectionGetInfoAppendString(&array, info_codes[i], "c-questdb-client " ADBC_QUESTDB_CLIENT_TAG, error));
        break;
      case ADBC_INFO_VENDOR_SQL:
        RAISE_ADBC(InternalAdbcConnectionGetInfoAppendInt(&array, info_codes[i], 1, error));
        break;
      case ADBC_INFO_VENDOR_SUBSTRAIT:
        RAISE_ADBC(InternalAdbcConnectionGetInfoAppendInt(&array, info_codes[i], 0, error));
        break;
      case ADBC_INFO_DRIVER_NAME:
        RAISE_ADBC(InternalAdbcConnectionGetInfoAppendString(&array, info_codes[i], "adbcBridge QuestDB driver (QWP)", error));
        break;
      case ADBC_INFO_DRIVER_VERSION:
        RAISE_ADBC(InternalAdbcConnectionGetInfoAppendString(&array, info_codes[i], ADBC_QUESTDB_VERSION, error));
        break;
      case ADBC_INFO_DRIVER_ARROW_VERSION:
        RAISE_ADBC(InternalAdbcConnectionGetInfoAppendString(&array, info_codes[i], "nanoarrow " NANOARROW_VERSION, error));
        break;
      case ADBC_INFO_DRIVER_ADBC_VERSION:
        RAISE_ADBC(InternalAdbcConnectionGetInfoAppendInt(&array, info_codes[i], ADBC_VERSION_1_1_0, error));
        break;
      default:
        break;
    }
  }
  array.length = array.children[0]->length;
  struct ArrowError na_error;
  CHECK_NA_DETAIL(INTERNAL, ArrowArrayFinishBuildingDefault(&array, &na_error), &na_error, error);
  CHECK_NA(INTERNAL, ArrowBasicArrayStreamInit(out, &schema, 1), error);
  ArrowBasicArrayStreamSetArray(out, 0, &array);
  return ADBC_STATUS_OK;
}

static AdbcStatusCode QdbConnectionGetTableTypes(struct AdbcConnection* connection, struct ArrowArrayStream* out,
                                                 struct AdbcError* error) {
  struct QdbConnection* conn = (struct QdbConnection*)connection->private_data;
  if (!conn || !conn->connected) return ADBC_STATUS_INVALID_STATE;
  struct ArrowSchema schema;
  ArrowSchemaInit(&schema);
  CHECK_NA(INTERNAL, ArrowSchemaSetTypeStruct(&schema, 1), error);
  CHECK_NA(INTERNAL, ArrowSchemaSetType(schema.children[0], NANOARROW_TYPE_STRING), error);
  CHECK_NA(INTERNAL, ArrowSchemaSetName(schema.children[0], "table_type"), error);
  schema.children[0]->flags &= ~ARROW_FLAG_NULLABLE;
  struct ArrowArray array;
  CHECK_NA(INTERNAL, ArrowArrayInitFromSchema(&array, &schema, NULL), error);
  CHECK_NA(INTERNAL, ArrowArrayStartAppending(&array), error);
  CHECK_NA(INTERNAL, ArrowArrayAppendString(array.children[0], ArrowCharView("TABLE")), error);
  CHECK_NA(INTERNAL, ArrowArrayFinishElement(&array), error);
  CHECK_NA(INTERNAL, ArrowArrayAppendString(array.children[0], ArrowCharView("MATERIALIZED VIEW")), error);
  CHECK_NA(INTERNAL, ArrowArrayFinishElement(&array), error);
  struct ArrowError na_error;
  CHECK_NA_DETAIL(INTERNAL, ArrowArrayFinishBuildingDefault(&array, &na_error), &na_error, error);
  CHECK_NA(INTERNAL, ArrowBasicArrayStreamInit(out, &schema, 1), error);
  ArrowBasicArrayStreamSetArray(out, 0, &array);
  return ADBC_STATUS_OK;
}

// --- catalog: tables() and table_columns() ----------------------------------

struct QdbTableInfo {
  char* name;
  char* designated_ts;
  bool mat_view;
  bool wal;
};

struct QdbTableList {
  struct QdbTableInfo* items;
  size_t n, cap;
};

static void QdbTableListFree(struct QdbTableList* l) {
  for (size_t i = 0; i < l->n; i++) { free(l->items[i].name); free(l->items[i].designated_ts); }
  free(l->items);
  l->items = NULL; l->n = l->cap = 0;
}

static AdbcStatusCode QdbTablesRow(const struct ArrowSchema* schema, struct ArrowArrayView* batch, int64_t row,
                                   void* ctx, struct AdbcError* error) {
  struct QdbTableList* l = (struct QdbTableList*)ctx;
  if (l->n == l->cap) {
    size_t cap = l->cap ? l->cap * 2 : 16;
    struct QdbTableInfo* items = (struct QdbTableInfo*)realloc(l->items, cap * sizeof(*items));
    if (!items) return ADBC_STATUS_INTERNAL;
    l->items = items; l->cap = cap;
  }
  int64_t c_name = QdbColIndex(schema, "table_name");
  if (c_name < 0) c_name = QdbColIndex(schema, "name");
  struct QdbTableInfo* t = &l->items[l->n];
  t->name = QdbCellString(batch, c_name, row);
  t->designated_ts = QdbCellString(batch, QdbColIndex(schema, "designatedTimestamp"), row);
  t->mat_view = QdbCellBool(batch, QdbColIndex(schema, "matView"), row);
  t->wal = QdbCellBool(batch, QdbColIndex(schema, "walEnabled"), row);
  if (!t->name) t->name = QdbStrdup("");
  l->n++;
  return ADBC_STATUS_OK;
}

static int QdbTableCmp(const void* a, const void* b) {
  return strcmp(((const struct QdbTableInfo*)a)->name, ((const struct QdbTableInfo*)b)->name);
}

static AdbcStatusCode QdbListTables(struct QdbConnection* conn, struct QdbTableList* out, struct AdbcError* error) {
  memset(out, 0, sizeof(*out));
  AdbcStatusCode status = QdbQueryRows(conn, "SELECT * FROM tables()", QdbTablesRow, out, error);
  if (status != ADBC_STATUS_OK) { QdbTableListFree(out); return status; }
  qsort(out->items, out->n, sizeof(out->items[0]), QdbTableCmp);
  return ADBC_STATUS_OK;
}

static AdbcStatusCode QdbFindTable(struct QdbConnection* conn, const char* name, struct QdbTableInfo* out,
                                   bool* found, struct AdbcError* error) {
  struct QdbTableList l;
  RAISE_ADBC(QdbListTables(conn, &l, error));
  *found = false;
  for (size_t i = 0; i < l.n; i++) {
    if (strcmp(l.items[i].name, name) == 0) {
      *found = true;
      out->name = QdbStrdup(l.items[i].name);
      out->designated_ts = QdbStrdup(l.items[i].designated_ts);
      out->mat_view = l.items[i].mat_view;
      out->wal = l.items[i].wal;
      break;
    }
  }
  QdbTableListFree(&l);
  return ADBC_STATUS_OK;
}

struct QdbColumnInfo {
  char* name;
  char* type;
  bool designated;
  bool indexed;
  bool upsert_key;
};

struct QdbColumnList {
  struct QdbColumnInfo* items;
  size_t n, cap;
};

static void QdbColumnListFree(struct QdbColumnList* l) {
  for (size_t i = 0; i < l->n; i++) { free(l->items[i].name); free(l->items[i].type); }
  free(l->items);
  l->items = NULL; l->n = l->cap = 0;
}

static AdbcStatusCode QdbColumnsRow(const struct ArrowSchema* schema, struct ArrowArrayView* batch, int64_t row,
                                    void* ctx, struct AdbcError* error) {
  struct QdbColumnList* l = (struct QdbColumnList*)ctx;
  if (l->n == l->cap) {
    size_t cap = l->cap ? l->cap * 2 : 16;
    struct QdbColumnInfo* items = (struct QdbColumnInfo*)realloc(l->items, cap * sizeof(*items));
    if (!items) return ADBC_STATUS_INTERNAL;
    l->items = items; l->cap = cap;
  }
  struct QdbColumnInfo* c = &l->items[l->n];
  int64_t c_name = QdbColIndex(schema, "column");
  if (c_name < 0) c_name = QdbColIndex(schema, "column_name");
  c->name = QdbCellString(batch, c_name, row);
  c->type = QdbCellString(batch, QdbColIndex(schema, "type"), row);
  c->designated = QdbCellBool(batch, QdbColIndex(schema, "designated"), row);
  c->indexed = QdbCellBool(batch, QdbColIndex(schema, "indexed"), row);
  c->upsert_key = QdbCellBool(batch, QdbColIndex(schema, "upsertKey"), row);
  if (!c->name) c->name = QdbStrdup("");
  if (!c->type) c->type = QdbStrdup("");
  l->n++;
  return ADBC_STATUS_OK;
}

static AdbcStatusCode QdbListColumns(struct QdbConnection* conn, const char* table, struct QdbColumnList* out,
                                     struct AdbcError* error) {
  memset(out, 0, sizeof(*out));
  char* lit = QdbQuoteLiteral(table);
  if (!lit) return ADBC_STATUS_INTERNAL;
  size_t n = strlen(lit) + 40;
  char* sql = (char*)malloc(n);
  if (!sql) { free(lit); return ADBC_STATUS_INTERNAL; }
  snprintf(sql, n, "SELECT * FROM table_columns(%s)", lit);
  free(lit);
  AdbcStatusCode status = QdbQueryRows(conn, sql, QdbColumnsRow, out, error);
  free(sql);
  if (status != ADBC_STATUS_OK) QdbColumnListFree(out);
  return status;
}

/// SQL LIKE with % and _ (case-insensitive, as the ODBC driver does).
static char QdbLower(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c; }
static bool QdbLikeMatch(const char* pat, const char* s) {
  if (!pat) return true;
  while (*pat) {
    if (*pat == '%') {
      while (*pat == '%') pat++;
      if (!*pat) return true;
      for (const char* t = s; ; t++) {
        if (QdbLikeMatch(pat, t)) return true;
        if (!*t) return false;
      }
    }
    if (!*s) return false;
    if (*pat != '_' && QdbLower(*pat) != QdbLower(*s)) return false;
    pat++; s++;
  }
  return *s == 0;
}

static const char* QdbTableType(const struct QdbTableInfo* t) { return t->mat_view ? "MATERIALIZED VIEW" : "TABLE"; }

static bool QdbTypeAllowed(const char** table_type, const char* type) {
  if (!table_type) return true;
  for (const char** p = table_type; *p; p++) {
    if (strcmp(*p, type) == 0) return true;
  }
  return false;
}

/// Precision and scale out of "DECIMAL(p,s)"; false when absent.
static bool QdbDecimalParams(const char* type, int* precision, int* scale) {
  const char* p = strchr(type, '(');
  if (!p) return false;
  return sscanf(p, "(%d,%d)", precision, scale) == 2;
}

/// Tag a field the way the client tags result columns.
static AdbcStatusCode QdbSetFieldMetadata(struct ArrowSchema* field, const char* column_type, const char* ext,
                                          struct AdbcError* error) {
  struct ArrowBuffer meta;
  ArrowMetadataBuilderInit(&meta, NULL);
  if (ext) CHECK_NA(INTERNAL, ArrowMetadataBuilderAppend(&meta, ArrowCharView("ARROW:extension:name"), ArrowCharView(ext)), error);
  CHECK_NA(INTERNAL, ArrowMetadataBuilderAppend(&meta, ArrowCharView("questdb.column_type"), ArrowCharView(column_type)), error);
  ArrowErrorCode rc = ArrowSchemaSetMetadata(field, (const char*)meta.data);
  ArrowBufferReset(&meta);
  CHECK_NA(INTERNAL, rc, error);
  return ADBC_STATUS_OK;
}

/// The Arrow type of a QuestDB column type name, as the client's Arrow egress
/// emits it, so GetTableSchema and a result's schema agree.
static AdbcStatusCode QdbTypeToArrow(struct ArrowSchema* field, const char* qtype, struct AdbcError* error) {
  char base[32];
  size_t n = strcspn(qtype, "([");
  if (n >= sizeof(base)) n = sizeof(base) - 1;
  memcpy(base, qtype, n);
  base[n] = 0;
  for (char* p = base; *p; p++) *p = (char)((*p >= 'a' && *p <= 'z') ? *p - 'a' + 'A' : *p);
  int dims = 0;
  for (const char* p = qtype; *p; p++) if (*p == '[') dims++;
  if (dims > 0) {
    struct ArrowSchema* leaf = field;
    for (int i = 0; i < dims; i++) {
      CHECK_NA(INTERNAL, ArrowSchemaSetType(leaf, NANOARROW_TYPE_LIST), error);
      leaf = leaf->children[0];
    }
    RAISE_ADBC(QdbTypeToArrow(leaf, base, error));
    return QdbSetFieldMetadata(field, strcmp(base, "LONG") == 0 ? "long_array" : "double_array", NULL, error);
  }
  const char* tag = NULL;
  const char* ext = NULL;
  if (strcmp(base, "BOOLEAN") == 0) { CHECK_NA(INTERNAL, ArrowSchemaSetType(field, NANOARROW_TYPE_BOOL), error); tag = "boolean"; }
  else if (strcmp(base, "BYTE") == 0) { CHECK_NA(INTERNAL, ArrowSchemaSetType(field, NANOARROW_TYPE_INT8), error); tag = "byte"; }
  else if (strcmp(base, "SHORT") == 0) { CHECK_NA(INTERNAL, ArrowSchemaSetType(field, NANOARROW_TYPE_INT16), error); tag = "short"; }
  else if (strcmp(base, "CHAR") == 0) { CHECK_NA(INTERNAL, ArrowSchemaSetType(field, NANOARROW_TYPE_UINT16), error); tag = "char"; }
  else if (strcmp(base, "INT") == 0) { CHECK_NA(INTERNAL, ArrowSchemaSetType(field, NANOARROW_TYPE_INT32), error); tag = "int"; }
  else if (strcmp(base, "LONG") == 0) { CHECK_NA(INTERNAL, ArrowSchemaSetType(field, NANOARROW_TYPE_INT64), error); tag = "long"; }
  else if (strcmp(base, "FLOAT") == 0) { CHECK_NA(INTERNAL, ArrowSchemaSetType(field, NANOARROW_TYPE_FLOAT), error); tag = "float"; }
  else if (strcmp(base, "DOUBLE") == 0) { CHECK_NA(INTERNAL, ArrowSchemaSetType(field, NANOARROW_TYPE_DOUBLE), error); tag = "double"; }
  else if (strcmp(base, "TIMESTAMP") == 0) {
    CHECK_NA(INTERNAL, ArrowSchemaSetTypeDateTime(field, NANOARROW_TYPE_TIMESTAMP, NANOARROW_TIME_UNIT_MICRO, "UTC"), error);
    tag = "timestamp";
  } else if (strcmp(base, "TIMESTAMP_NS") == 0) {
    CHECK_NA(INTERNAL, ArrowSchemaSetTypeDateTime(field, NANOARROW_TYPE_TIMESTAMP, NANOARROW_TIME_UNIT_NANO, "UTC"), error);
    tag = "timestamp_nanos";
  } else if (strcmp(base, "DATE") == 0) {
    CHECK_NA(INTERNAL, ArrowSchemaSetTypeDateTime(field, NANOARROW_TYPE_TIMESTAMP, NANOARROW_TIME_UNIT_MILLI, "UTC"), error);
    tag = "date";
  } else if (strcmp(base, "BINARY") == 0) { CHECK_NA(INTERNAL, ArrowSchemaSetType(field, NANOARROW_TYPE_BINARY), error); tag = "binary"; }
  else if (strcmp(base, "UUID") == 0) {
    CHECK_NA(INTERNAL, ArrowSchemaSetTypeFixedSize(field, NANOARROW_TYPE_FIXED_SIZE_BINARY, 16), error);
    tag = "uuid"; ext = "arrow.uuid";
  } else if (strcmp(base, "LONG256") == 0) {
    CHECK_NA(INTERNAL, ArrowSchemaSetTypeFixedSize(field, NANOARROW_TYPE_FIXED_SIZE_BINARY, 32), error);
    tag = "long256";
  } else if (strcmp(base, "IPV4") == 0) { CHECK_NA(INTERNAL, ArrowSchemaSetType(field, NANOARROW_TYPE_UINT32), error); tag = "ipv4"; }
  else if (strcmp(base, "GEOHASH") == 0) { CHECK_NA(INTERNAL, ArrowSchemaSetType(field, NANOARROW_TYPE_INT64), error); tag = "geohash"; }
  else if (strcmp(base, "DECIMAL") == 0) {
    int precision = 38, scale = 0;
    QdbDecimalParams(qtype, &precision, &scale);
    // The client reports the storage class's precision, not the declared one:
    // DECIMAL64 is 18, DECIMAL128 is 38 and DECIMAL256 is 76.
    if (precision <= 18) {
      CHECK_NA(INTERNAL, ArrowSchemaSetTypeDecimal(field, NANOARROW_TYPE_DECIMAL64, 18, scale), error);
      tag = "decimal64";
    } else if (precision <= 38) {
      CHECK_NA(INTERNAL, ArrowSchemaSetTypeDecimal(field, NANOARROW_TYPE_DECIMAL128, 38, scale), error);
      tag = "decimal128";
    } else {
      CHECK_NA(INTERNAL, ArrowSchemaSetTypeDecimal(field, NANOARROW_TYPE_DECIMAL256, 76, scale), error);
      tag = "decimal256";
    }
  } else if (strcmp(base, "SYMBOL") == 0) {
    CHECK_NA(INTERNAL, ArrowSchemaSetType(field, NANOARROW_TYPE_UINT32), error);
    CHECK_NA(INTERNAL, ArrowSchemaAllocateDictionary(field), error);
    ArrowSchemaInit(field->dictionary);
    CHECK_NA(INTERNAL, ArrowSchemaSetType(field->dictionary, NANOARROW_TYPE_STRING), error);
    tag = "symbol";
  } else {
    // STRING, VARCHAR and anything new: text.
    CHECK_NA(INTERNAL, ArrowSchemaSetType(field, NANOARROW_TYPE_STRING), error);
    tag = "varchar";
  }
  return QdbSetFieldMetadata(field, tag, ext, error);
}

static AdbcStatusCode QdbAppendStrOrNull(struct ArrowArray* a, const char* s) {
  return (!s ? ArrowArrayAppendNull(a, 1) : ArrowArrayAppendString(a, ArrowCharView(s))) == NANOARROW_OK
             ? ADBC_STATUS_OK : ADBC_STATUS_INTERNAL;
}

static AdbcStatusCode QdbAppendColumns(struct QdbConnection* conn, struct ArrowArray* cols_list,
                                       const struct QdbTableInfo* t, const char* column_pattern,
                                       struct AdbcError* error) {
  struct QdbColumnList cols;
  RAISE_ADBC(QdbListColumns(conn, t->name, &cols, error));
  struct ArrowArray* c = cols_list->children[0];
  for (size_t i = 0; i < cols.n; i++) {
    const struct QdbColumnInfo* col = &cols.items[i];
    if (!QdbLikeMatch(column_pattern, col->name)) continue;
    RAISE_ADBC(QdbAppendStrOrNull(c->children[0], col->name));                         // column_name
    CHECK_NA(INTERNAL, ArrowArrayAppendInt(c->children[1], (int64_t)i + 1), error);   // ordinal_position
    const char* remarks = col->designated ? "designated timestamp" : (col->upsert_key ? "dedup key" : NULL);
    RAISE_ADBC(QdbAppendStrOrNull(c->children[2], remarks));                           // remarks
    CHECK_NA(INTERNAL, ArrowArrayAppendNull(c->children[3], 1), error);               // xdbc_data_type
    RAISE_ADBC(QdbAppendStrOrNull(c->children[4], col->type));                         // xdbc_type_name
    int precision = 0, scale = 0;
    if (QdbDecimalParams(col->type, &precision, &scale)) {
      CHECK_NA(INTERNAL, ArrowArrayAppendInt(c->children[5], precision), error);      // xdbc_column_size
      CHECK_NA(INTERNAL, ArrowArrayAppendInt(c->children[6], scale), error);          // xdbc_decimal_digits
      CHECK_NA(INTERNAL, ArrowArrayAppendInt(c->children[7], 10), error);             // xdbc_num_prec_radix
    } else {
      CHECK_NA(INTERNAL, ArrowArrayAppendNull(c->children[5], 1), error);
      CHECK_NA(INTERNAL, ArrowArrayAppendNull(c->children[6], 1), error);
      CHECK_NA(INTERNAL, ArrowArrayAppendNull(c->children[7], 1), error);
    }
    CHECK_NA(INTERNAL, ArrowArrayAppendInt(c->children[8], 1), error);                // xdbc_nullable (SQL_NULLABLE)
    CHECK_NA(INTERNAL, ArrowArrayAppendNull(c->children[9], 1), error);               // xdbc_column_def
    CHECK_NA(INTERNAL, ArrowArrayAppendNull(c->children[10], 1), error);              // xdbc_sql_data_type
    CHECK_NA(INTERNAL, ArrowArrayAppendNull(c->children[11], 1), error);              // xdbc_datetime_sub
    CHECK_NA(INTERNAL, ArrowArrayAppendNull(c->children[12], 1), error);              // xdbc_char_octet_length
    RAISE_ADBC(QdbAppendStrOrNull(c->children[13], "YES"));                            // xdbc_is_nullable
    for (int k = 14; k <= 18; k++) CHECK_NA(INTERNAL, ArrowArrayAppendNull(c->children[k], 1), error);
    CHECK_NA(INTERNAL, ArrowArrayFinishElement(c), error);
  }
  QdbColumnListFree(&cols);
  CHECK_NA(INTERNAL, ArrowArrayFinishElement(cols_list), error);
  return ADBC_STATUS_OK;
}

static AdbcStatusCode QdbConnectionGetObjects(struct AdbcConnection* connection, int depth, const char* catalog,
                                              const char* db_schema, const char* table_name,
                                              const char** table_type, const char* column_name,
                                              struct ArrowArrayStream* out, struct AdbcError* error) {
  struct QdbConnection* conn = (struct QdbConnection*)connection->private_data;
  if (!conn || !conn->connected) return ADBC_STATUS_INVALID_STATE;

  struct ArrowSchema schema;
  struct ArrowArray array;
  RAISE_ADBC(InternalAdbcInitConnectionObjectsSchema(&schema, error));
  CHECK_NA(INTERNAL, ArrowArrayInitFromSchema(&array, &schema, NULL), error);
  CHECK_NA(INTERNAL, ArrowArrayStartAppending(&array), error);

  struct ArrowArray* cat_name = array.children[0];
  struct ArrowArray* cat_schemas = array.children[1];
  struct ArrowArray* sch = cat_schemas->children[0];
  struct ArrowArray* sch_name = sch->children[0];
  struct ArrowArray* sch_tables = sch->children[1];
  struct ArrowArray* tbl = sch_tables->children[0];

  bool cat_ok = QdbLikeMatch(catalog, QDB_CATALOG);
  bool sch_ok = QdbLikeMatch(db_schema, QDB_SCHEMA);
  if (cat_ok) {
    RAISE_ADBC(QdbAppendStrOrNull(cat_name, QDB_CATALOG));
    if (depth == ADBC_OBJECT_DEPTH_CATALOGS) {
      CHECK_NA(INTERNAL, ArrowArrayAppendNull(cat_schemas, 1), error);
    } else {
      if (sch_ok) {
        RAISE_ADBC(QdbAppendStrOrNull(sch_name, QDB_SCHEMA));
        if (depth == ADBC_OBJECT_DEPTH_DB_SCHEMAS) {
          CHECK_NA(INTERNAL, ArrowArrayAppendNull(sch_tables, 1), error);
        } else {
          struct QdbTableList tables;
          AdbcStatusCode status = QdbListTables(conn, &tables, error);
          if (status != ADBC_STATUS_OK) { ArrowArrayRelease(&array); schema.release(&schema); return status; }
          for (size_t i = 0; i < tables.n; i++) {
            const struct QdbTableInfo* t = &tables.items[i];
            if (!QdbLikeMatch(table_name, t->name)) continue;
            if (!QdbTypeAllowed(table_type, QdbTableType(t))) continue;
            status = QdbAppendStrOrNull(tbl->children[0], t->name);
            if (status == ADBC_STATUS_OK) status = QdbAppendStrOrNull(tbl->children[1], QdbTableType(t));
            if (status == ADBC_STATUS_OK) {
              if (depth == ADBC_OBJECT_DEPTH_TABLES) {
                if (ArrowArrayAppendNull(tbl->children[2], 1) || ArrowArrayAppendNull(tbl->children[3], 1))
                  status = ADBC_STATUS_INTERNAL;
              } else {
                status = QdbAppendColumns(conn, tbl->children[2], t, column_name, error);
                // No constraints in QuestDB: an empty list.
                if (status == ADBC_STATUS_OK && ArrowArrayFinishElement(tbl->children[3])) status = ADBC_STATUS_INTERNAL;
              }
            }
            if (status == ADBC_STATUS_OK && ArrowArrayFinishElement(tbl)) status = ADBC_STATUS_INTERNAL;
            if (status != ADBC_STATUS_OK) {
              QdbTableListFree(&tables);
              ArrowArrayRelease(&array);
              schema.release(&schema);
              if (status == ADBC_STATUS_INTERNAL && (!error || !error->message))
                InternalAdbcSetError(error, "GetObjects: building the result failed");
              return status;
            }
          }
          QdbTableListFree(&tables);
          CHECK_NA(INTERNAL, ArrowArrayFinishElement(sch_tables), error);
        }
        CHECK_NA(INTERNAL, ArrowArrayFinishElement(sch), error);
      }
      CHECK_NA(INTERNAL, ArrowArrayFinishElement(cat_schemas), error);
    }
  }
  array.length = cat_name->length;
  struct ArrowError na_error;
  CHECK_NA_DETAIL(INTERNAL, ArrowArrayFinishBuildingDefault(&array, &na_error), &na_error, error);
  CHECK_NA(INTERNAL, ArrowBasicArrayStreamInit(out, &schema, 1), error);
  ArrowBasicArrayStreamSetArray(out, 0, &array);
  return ADBC_STATUS_OK;
}

static AdbcStatusCode QdbConnectionGetTableSchema(struct AdbcConnection* connection, const char* catalog,
                                                  const char* db_schema, const char* table_name,
                                                  struct ArrowSchema* schema, struct AdbcError* error) {
  struct QdbConnection* conn = (struct QdbConnection*)connection->private_data;
  if (!conn || !conn->connected) return ADBC_STATUS_INVALID_STATE;
  if (!table_name) { InternalAdbcSetError(error, "table_name is required"); return ADBC_STATUS_INVALID_ARGUMENT; }
  if ((catalog && *catalog && strcmp(catalog, QDB_CATALOG) != 0) ||
      (db_schema && *db_schema && strcmp(db_schema, QDB_SCHEMA) != 0)) {
    InternalAdbcSetError(error, "Table %s not found: QuestDB has a single namespace %s.%s", table_name,
                         QDB_CATALOG, QDB_SCHEMA);
    return ADBC_STATUS_NOT_FOUND;
  }
  struct QdbColumnList cols;
  AdbcStatusCode status = QdbListColumns(conn, table_name, &cols, error);
  if (status != ADBC_STATUS_OK) {
    if (status == ADBC_STATUS_INVALID_ARGUMENT || status == ADBC_STATUS_IO) {
      // table_columns() on a missing table is a server-side error.
      bool found = false;
      struct QdbTableInfo t = {0};
      struct AdbcError scratch;
      memset(&scratch, 0, sizeof(scratch));
      if (QdbFindTable(conn, table_name, &t, &found, &scratch) == ADBC_STATUS_OK && !found) {
        if (error && error->release) error->release(error);
        InternalAdbcSetError(error, "Table %s not found", table_name);
        status = ADBC_STATUS_NOT_FOUND;
      }
      if (scratch.release) scratch.release(&scratch);
      free(t.name); free(t.designated_ts);
    }
    return status;
  }
  ArrowSchemaInit(schema);
  CHECK_NA(INTERNAL, ArrowSchemaSetTypeStruct(schema, (int64_t)cols.n), error);
  for (size_t i = 0; i < cols.n; i++) {
    CHECK_NA(INTERNAL, ArrowSchemaSetName(schema->children[i], cols.items[i].name), error);
    status = QdbTypeToArrow(schema->children[i], cols.items[i].type, error);
    if (status != ADBC_STATUS_OK) { QdbColumnListFree(&cols); schema->release(schema); return status; }
  }
  QdbColumnListFree(&cols);
  return ADBC_STATUS_OK;
}

static AdbcStatusCode QdbConnectionGetStatistics(struct AdbcConnection* c, const char* catalog, const char* s,
                                                 const char* t, char approximate, struct ArrowArrayStream* out,
                                                 struct AdbcError* error) {
  InternalAdbcSetError(error, "GetStatistics is not supported");
  return ADBC_STATUS_NOT_IMPLEMENTED;
}
static AdbcStatusCode QdbConnectionGetStatisticNames(struct AdbcConnection* c, struct ArrowArrayStream* out,
                                                     struct AdbcError* error) {
  InternalAdbcSetError(error, "GetStatisticNames is not supported");
  return ADBC_STATUS_NOT_IMPLEMENTED;
}
static AdbcStatusCode QdbConnectionReadPartition(struct AdbcConnection* c, const uint8_t* p, size_t n,
                                                 struct ArrowArrayStream* out, struct AdbcError* error) {
  InternalAdbcSetError(error, "Partitioned results are not supported");
  return ADBC_STATUS_NOT_IMPLEMENTED;
}

// ---------------------------------------------------------------------------
// Statement

static AdbcStatusCode QdbStatementNew(struct AdbcConnection* connection, struct AdbcStatement* statement,
                                      struct AdbcError* error) {
  struct QdbConnection* conn = (struct QdbConnection*)connection->private_data;
  if (!conn || !conn->connected) return ADBC_STATUS_INVALID_STATE;
  struct QdbStatement* stmt = (struct QdbStatement*)calloc(1, sizeof(*stmt));
  if (!stmt) { InternalAdbcSetError(error, "Out of memory"); return ADBC_STATUS_INTERNAL; }
  stmt->conn = conn;
  stmt->ingest_mode = QDB_INGEST_CREATE;
  stmt->wait_for_apply = true;
  stmt->apply_timeout_ms = 30000;
  stmt->cancel = QdbCancelNew();
  if (!stmt->cancel) { free(stmt); return ADBC_STATUS_INTERNAL; }
  statement->private_data = stmt;
  return ADBC_STATUS_OK;
}

static void QdbStatementDropBind(struct QdbStatement* stmt) {
  if (stmt->has_bind && stmt->bind.release) stmt->bind.release(&stmt->bind);
  memset(&stmt->bind, 0, sizeof(stmt->bind));
  stmt->has_bind = false;
}

static AdbcStatusCode QdbStatementRelease(struct AdbcStatement* statement, struct AdbcError* error) {
  struct QdbStatement* stmt = (struct QdbStatement*)statement->private_data;
  if (!stmt) return ADBC_STATUS_INVALID_STATE;
  QdbStatementDropBind(stmt);
  QdbCancelRelease(stmt->cancel);
  free(stmt->sql); free(stmt->ingest_table); free(stmt->ingest_ts); free(stmt->partition_by);
  free(stmt);
  statement->private_data = NULL;
  return ADBC_STATUS_OK;
}

static AdbcStatusCode QdbStatementSetSqlQuery(struct AdbcStatement* statement, const char* query,
                                              struct AdbcError* error) {
  struct QdbStatement* stmt = (struct QdbStatement*)statement->private_data;
  if (!stmt) return ADBC_STATUS_INVALID_STATE;
  free(stmt->sql);
  stmt->sql = QdbStrdup(query);
  free(stmt->ingest_table);
  stmt->ingest_table = NULL;
  return ADBC_STATUS_OK;
}

static AdbcStatusCode QdbStatementSetSubstraitPlan(struct AdbcStatement* statement, const uint8_t* plan,
                                                   size_t length, struct AdbcError* error) {
  InternalAdbcSetError(error, "Substrait plans are not supported");
  return ADBC_STATUS_NOT_IMPLEMENTED;
}

static AdbcStatusCode QdbStatementPrepare(struct AdbcStatement* statement, struct AdbcError* error) {
  struct QdbStatement* stmt = (struct QdbStatement*)statement->private_data;
  if (!stmt) return ADBC_STATUS_INVALID_STATE;
  if (!stmt->sql) { InternalAdbcSetError(error, "No query has been set"); return ADBC_STATUS_INVALID_STATE; }
  // QWP prepares per execution; nothing to do ahead of time.
  return ADBC_STATUS_OK;
}

static AdbcStatusCode QdbStatementGetParameterSchema(struct AdbcStatement* statement, struct ArrowSchema* schema,
                                                     struct AdbcError* error) {
  InternalAdbcSetError(error, "QuestDB does not describe statement parameters");
  return ADBC_STATUS_NOT_IMPLEMENTED;
}

static AdbcStatusCode QdbStatementBind(struct AdbcStatement* statement, struct ArrowArray* values,
                                       struct ArrowSchema* schema, struct AdbcError* error) {
  struct QdbStatement* stmt = (struct QdbStatement*)statement->private_data;
  if (!stmt) return ADBC_STATUS_INVALID_STATE;
  QdbStatementDropBind(stmt);
  // The caller keeps its schema; the array is ours to release.
  struct ArrowSchema copy;
  CHECK_NA(INTERNAL, ArrowSchemaDeepCopy(schema, &copy), error);
  CHECK_NA(INTERNAL, ArrowBasicArrayStreamInit(&stmt->bind, &copy, 1), error);
  ArrowBasicArrayStreamSetArray(&stmt->bind, 0, values);
  stmt->has_bind = true;
  return ADBC_STATUS_OK;
}

static AdbcStatusCode QdbStatementBindStream(struct AdbcStatement* statement, struct ArrowArrayStream* stream,
                                             struct AdbcError* error) {
  struct QdbStatement* stmt = (struct QdbStatement*)statement->private_data;
  if (!stmt) return ADBC_STATUS_INVALID_STATE;
  QdbStatementDropBind(stmt);
  stmt->bind = *stream;
  memset(stream, 0, sizeof(*stream));
  stmt->has_bind = true;
  return ADBC_STATUS_OK;
}

static AdbcStatusCode QdbStatementSetOption(struct AdbcStatement* statement, const char* key, const char* value,
                                            struct AdbcError* error) {
  struct QdbStatement* stmt = (struct QdbStatement*)statement->private_data;
  if (!stmt) return ADBC_STATUS_INVALID_STATE;
  if (strcmp(key, ADBC_INGEST_OPTION_TARGET_TABLE) == 0) {
    free(stmt->ingest_table);
    stmt->ingest_table = QdbStrdup(value);
    free(stmt->sql);
    stmt->sql = NULL;
    return ADBC_STATUS_OK;
  }
  if (strcmp(key, ADBC_INGEST_OPTION_MODE) == 0) {
    if (strcmp(value, ADBC_INGEST_OPTION_MODE_CREATE) == 0) stmt->ingest_mode = QDB_INGEST_CREATE;
    else if (strcmp(value, ADBC_INGEST_OPTION_MODE_APPEND) == 0) stmt->ingest_mode = QDB_INGEST_APPEND;
    else if (strcmp(value, ADBC_INGEST_OPTION_MODE_REPLACE) == 0) stmt->ingest_mode = QDB_INGEST_REPLACE;
    else if (strcmp(value, ADBC_INGEST_OPTION_MODE_CREATE_APPEND) == 0) stmt->ingest_mode = QDB_INGEST_CREATE_APPEND;
    else { InternalAdbcSetError(error, "Unknown ingest mode %s", value); return ADBC_STATUS_INVALID_ARGUMENT; }
    return ADBC_STATUS_OK;
  }
  if (strcmp(key, ADBC_INGEST_OPTION_TARGET_CATALOG) == 0 || strcmp(key, ADBC_INGEST_OPTION_TARGET_DB_SCHEMA) == 0) {
    const char* want = strcmp(key, ADBC_INGEST_OPTION_TARGET_CATALOG) == 0 ? QDB_CATALOG : QDB_SCHEMA;
    if (!value || !*value || strcmp(value, want) == 0) return ADBC_STATUS_OK;
    InternalAdbcSetError(error, "QuestDB has a single namespace %s.%s; cannot ingest into %s", QDB_CATALOG, QDB_SCHEMA, value);
    return ADBC_STATUS_NOT_IMPLEMENTED;
  }
  if (strcmp(key, ADBC_INGEST_OPTION_TEMPORARY) == 0) {
    if (value && strcmp(value, ADBC_OPTION_VALUE_DISABLED) == 0) return ADBC_STATUS_OK;
    InternalAdbcSetError(error, "QuestDB has no temporary tables");
    return ADBC_STATUS_NOT_IMPLEMENTED;
  }
  if (strcmp(key, QDB_OPT_INGEST_TS) == 0) {
    free(stmt->ingest_ts);
    stmt->ingest_ts = (value && *value) ? QdbStrdup(value) : NULL;
    return ADBC_STATUS_OK;
  }
  if (strcmp(key, QDB_OPT_INGEST_PARTITION_BY) == 0) {
    static const char* const kUnits[] = {"NONE", "HOUR", "DAY", "WEEK", "MONTH", "YEAR", NULL};
    const char* match = NULL;
    for (const char* const* u = kUnits; *u; u++) {
      bool same = strlen(*u) == strlen(value);
      for (size_t i = 0; same && value[i]; i++) same = QdbLower(value[i]) == QdbLower((*u)[i]);
      if (same) match = *u;
    }
    if (!match) { InternalAdbcSetError(error, "%s: expected NONE, HOUR, DAY, WEEK, MONTH or YEAR, got %s", key, value); return ADBC_STATUS_INVALID_ARGUMENT; }
    free(stmt->partition_by);
    stmt->partition_by = QdbStrdup(match);
    return ADBC_STATUS_OK;
  }
  if (strcmp(key, QDB_OPT_INGEST_SYMBOL_MODE) == 0) {
    if (strcmp(value, "auto") == 0) stmt->symbol_mode = QDB_SYMBOL_AUTO;
    else if (strcmp(value, "symbol") == 0) stmt->symbol_mode = QDB_SYMBOL_SYMBOL;
    else if (strcmp(value, "not_symbol") == 0 || strcmp(value, "varchar") == 0) stmt->symbol_mode = QDB_SYMBOL_NOT_SYMBOL;
    else { InternalAdbcSetError(error, "%s: expected auto, symbol or varchar, got %s", key, value); return ADBC_STATUS_INVALID_ARGUMENT; }
    return ADBC_STATUS_OK;
  }
  if (strcmp(key, QDB_OPT_INGEST_WAIT) == 0) {
    if (strcmp(value, ADBC_OPTION_VALUE_ENABLED) == 0) stmt->wait_for_apply = true;
    else if (strcmp(value, ADBC_OPTION_VALUE_DISABLED) == 0) stmt->wait_for_apply = false;
    else { InternalAdbcSetError(error, "%s: expected true or false, got %s", key, value); return ADBC_STATUS_INVALID_ARGUMENT; }
    return ADBC_STATUS_OK;
  }
  if (strcmp(key, QDB_OPT_QUERY_TIMEOUT_MS) == 0 || strcmp(key, QDB_OPT_INGEST_APPLY_TIMEOUT_MS) == 0) {
    char* end = NULL;
    long long v = strtoll(value, &end, 10);
    if (!end || *end || v < 0) { InternalAdbcSetError(error, "%s: expected a non-negative integer, got %s", key, value); return ADBC_STATUS_INVALID_ARGUMENT; }
    if (strcmp(key, QDB_OPT_QUERY_TIMEOUT_MS) == 0) stmt->query_timeout_ms = (int64_t)v;
    else stmt->apply_timeout_ms = (int64_t)v;
    return ADBC_STATUS_OK;
  }
  InternalAdbcSetError(error, "Unknown statement option %s", key);
  return ADBC_STATUS_NOT_IMPLEMENTED;
}

static AdbcStatusCode QdbStatementSetOptionInt(struct AdbcStatement* statement, const char* key, int64_t value,
                                               struct AdbcError* error) {
  struct QdbStatement* stmt = (struct QdbStatement*)statement->private_data;
  if (!stmt) return ADBC_STATUS_INVALID_STATE;
  if (value < 0) { InternalAdbcSetError(error, "%s must be non-negative", key); return ADBC_STATUS_INVALID_ARGUMENT; }
  if (strcmp(key, QDB_OPT_QUERY_TIMEOUT_MS) == 0) { stmt->query_timeout_ms = value; return ADBC_STATUS_OK; }
  if (strcmp(key, QDB_OPT_INGEST_APPLY_TIMEOUT_MS) == 0) { stmt->apply_timeout_ms = value; return ADBC_STATUS_OK; }
  return QdbNotImplementedOption("statement", key, error);
}

static AdbcStatusCode QdbStatementGetOption(struct AdbcStatement* statement, const char* key, char* value,
                                            size_t* length, struct AdbcError* error) {
  struct QdbStatement* stmt = (struct QdbStatement*)statement->private_data;
  if (!stmt) return ADBC_STATUS_INVALID_STATE;
  if (strcmp(key, ADBC_INGEST_OPTION_TARGET_TABLE) == 0) return QdbGetStringOption(stmt->ingest_table, value, length, error);
  if (strcmp(key, QDB_OPT_INGEST_TS) == 0) return QdbGetStringOption(stmt->ingest_ts, value, length, error);
  if (strcmp(key, QDB_OPT_INGEST_PARTITION_BY) == 0)
    return QdbGetStringOption(stmt->partition_by ? stmt->partition_by : "DAY", value, length, error);
  if (strcmp(key, QDB_OPT_INGEST_WAIT) == 0)
    return QdbGetStringOption(stmt->wait_for_apply ? ADBC_OPTION_VALUE_ENABLED : ADBC_OPTION_VALUE_DISABLED, value, length, error);
  if (strcmp(key, QDB_OPT_INGEST_SYMBOL_MODE) == 0)
    return QdbGetStringOption(stmt->symbol_mode == QDB_SYMBOL_AUTO ? "auto" : stmt->symbol_mode == QDB_SYMBOL_SYMBOL ? "symbol" : "varchar",
                              value, length, error);
  InternalAdbcSetError(error, "Unknown statement option %s", key);
  return ADBC_STATUS_NOT_FOUND;
}

static AdbcStatusCode QdbStatementGetOptionInt(struct AdbcStatement* statement, const char* key, int64_t* value,
                                               struct AdbcError* error) {
  struct QdbStatement* stmt = (struct QdbStatement*)statement->private_data;
  if (!stmt) return ADBC_STATUS_INVALID_STATE;
  if (strcmp(key, QDB_OPT_QUERY_TIMEOUT_MS) == 0) { *value = stmt->query_timeout_ms; return ADBC_STATUS_OK; }
  if (strcmp(key, QDB_OPT_INGEST_APPLY_TIMEOUT_MS) == 0) { *value = stmt->apply_timeout_ms; return ADBC_STATUS_OK; }
  InternalAdbcSetError(error, "Unknown statement option %s", key);
  return ADBC_STATUS_NOT_FOUND;
}

static AdbcStatusCode QdbStatementSetOptionBytes(struct AdbcStatement* s, const char* key, const uint8_t* v,
                                                 size_t n, struct AdbcError* e) { return QdbNotImplementedOption("statement", key, e); }
static AdbcStatusCode QdbStatementSetOptionDouble(struct AdbcStatement* s, const char* key, double v,
                                                  struct AdbcError* e) { return QdbNotImplementedOption("statement", key, e); }
static AdbcStatusCode QdbStatementGetOptionBytes(struct AdbcStatement* s, const char* key, uint8_t* v,
                                                 size_t* n, struct AdbcError* e) { return QdbNotImplementedOption("statement", key, e); }
static AdbcStatusCode QdbStatementGetOptionDouble(struct AdbcStatement* s, const char* key, double* v,
                                                  struct AdbcError* e) { return QdbNotImplementedOption("statement", key, e); }

static AdbcStatusCode QdbStatementCancel(struct AdbcStatement* statement, struct AdbcError* error) {
  struct QdbStatement* stmt = (struct QdbStatement*)statement->private_data;
  if (!stmt) return ADBC_STATUS_INVALID_STATE;
  OdbcMutexLock(&stmt->cancel->lock);
  stmt->cancel->cancelled = true;
  OdbcMutexUnlock(&stmt->cancel->lock);
  return ADBC_STATUS_OK;
}

// --- bulk ingest --------------------------------------------------------------

struct QdbWalTxn { int64_t writer, sequencer; bool found; };

static AdbcStatusCode QdbWalRow(const struct ArrowSchema* schema, struct ArrowArrayView* batch, int64_t row,
                                void* ctx, struct AdbcError* error) {
  struct QdbWalTxn* w = (struct QdbWalTxn*)ctx;
  int64_t cw = QdbColIndex(schema, "writerTxn"), cs = QdbColIndex(schema, "sequencerTxn");
  if (cw < 0 || cs < 0) return ADBC_STATUS_OK;
  w->writer = ArrowArrayViewGetIntUnsafe(batch->children[cw], row);
  w->sequencer = ArrowArrayViewGetIntUnsafe(batch->children[cs], row);
  w->found = true;
  return ADBC_STATUS_OK;
}

/// Wait until the table's WAL has been applied, so a SELECT that follows the
/// ingest sees the rows (the server acknowledges a frame before it is visible).
static AdbcStatusCode QdbWaitForApply(struct QdbConnection* conn, const char* table, int64_t timeout_ms,
                                      struct AdbcError* error) {
  char* lit = QdbQuoteLiteral(table);
  if (!lit) return ADBC_STATUS_INTERNAL;
  size_t n = strlen(lit) + 80;
  char* sql = (char*)malloc(n);
  if (!sql) { free(lit); return ADBC_STATUS_INTERNAL; }
  snprintf(sql, n, "SELECT writerTxn, sequencerTxn FROM wal_tables() WHERE name = %s", lit);
  free(lit);
  int64_t deadline = QdbNowMs() + timeout_ms;
  AdbcStatusCode status = ADBC_STATUS_OK;
  for (;;) {
    struct QdbWalTxn w = {0, 0, false};
    status = QdbQueryRows(conn, sql, QdbWalRow, &w, error);
    if (status != ADBC_STATUS_OK) break;
    if (!w.found || w.writer >= w.sequencer) break;  // not a WAL table, or caught up
    if (QdbNowMs() >= deadline) {
      InternalAdbcSetError(error, "Ingest into %s was acknowledged but not yet applied after %" PRId64
                           " ms (writerTxn %" PRId64 " < sequencerTxn %" PRId64 "); the rows will appear once the WAL "
                           "is applied. Raise %s or set %s=false to skip this wait.",
                           table, timeout_ms, w.writer, w.sequencer, QDB_OPT_INGEST_APPLY_TIMEOUT_MS, QDB_OPT_INGEST_WAIT);
      status = ADBC_STATUS_TIMEOUT;
      break;
    }
    QdbSleepMs(10);
  }
  free(sql);
  return status;
}

static AdbcStatusCode QdbExecSimple(struct QdbConnection* conn, const char* sql, struct AdbcError* error) {
  questdb_error* err = NULL;
  qwp_reader* reader = questdb_db_borrow_reader(conn->db->pool, &err);
  if (!reader) return QdbError(error, err, "borrow a QWP reader");
  AdbcStatusCode status = ADBC_STATUS_OK;
  line_sender_utf8 u;
  if (!QdbUtf8(&u, sql, error, &status)) { qwp_reader_close(reader); return status; }
  qwp_reader_cursor* cursor = qwp_reader_execute(reader, u, &err);
  if (!cursor) { status = QdbError(error, err, sql); qwp_reader_close(reader); return status; }
  while (qwp_reader_cursor_next_batch(cursor, &err) != NULL) {}
  if (err) status = QdbError(error, err, sql);
  qwp_reader_cursor_free(cursor);
  qwp_reader_close(reader);
  return status;
}

static bool QdbSchemaIsTimestamp(const struct ArrowSchema* field) {
  struct ArrowSchemaView sv;
  if (ArrowSchemaViewInit(&sv, field, NULL) != NANOARROW_OK) return false;
  return sv.type == NANOARROW_TYPE_TIMESTAMP;
}
static bool QdbSchemaIsText(const struct ArrowSchema* field, bool* dictionary) {
  struct ArrowSchemaView sv;
  if (ArrowSchemaViewInit(&sv, field, NULL) != NANOARROW_OK) return false;
  *dictionary = sv.type == NANOARROW_TYPE_DICTIONARY;
  if (*dictionary) {
    struct ArrowSchemaView dv;
    if (!field->dictionary || ArrowSchemaViewInit(&dv, field->dictionary, NULL) != NANOARROW_OK) return false;
    return dv.type == NANOARROW_TYPE_STRING || dv.type == NANOARROW_TYPE_LARGE_STRING || dv.type == NANOARROW_TYPE_STRING_VIEW;
  }
  return sv.type == NANOARROW_TYPE_STRING || sv.type == NANOARROW_TYPE_LARGE_STRING || sv.type == NANOARROW_TYPE_STRING_VIEW;
}

/// The QuestDB column type for a field of the data to ingest; NULL when the
/// client cannot send that Arrow type.
static const char* QdbQuestTypeFor(const struct ArrowSchema* field, enum QdbSymbolMode mode, char* buf, size_t n) {
  struct ArrowSchemaView sv;
  if (ArrowSchemaViewInit(&sv, field, NULL) != NANOARROW_OK) return NULL;
  switch (sv.type) {
    case NANOARROW_TYPE_BOOL: return "BOOLEAN";
    case NANOARROW_TYPE_INT8: return "BYTE";
    case NANOARROW_TYPE_INT16: case NANOARROW_TYPE_UINT8: return "SHORT";
    case NANOARROW_TYPE_INT32: case NANOARROW_TYPE_UINT16: return "INT";
    case NANOARROW_TYPE_INT64: case NANOARROW_TYPE_UINT32: case NANOARROW_TYPE_UINT64: return "LONG";
    case NANOARROW_TYPE_HALF_FLOAT: case NANOARROW_TYPE_FLOAT: return "FLOAT";
    case NANOARROW_TYPE_DOUBLE: return "DOUBLE";
    case NANOARROW_TYPE_STRING: case NANOARROW_TYPE_LARGE_STRING: case NANOARROW_TYPE_STRING_VIEW:
      return mode == QDB_SYMBOL_SYMBOL ? "SYMBOL" : "VARCHAR";
    case NANOARROW_TYPE_DICTIONARY: {
      bool dict = false;
      if (!QdbSchemaIsText(field, &dict)) return NULL;
      return mode == QDB_SYMBOL_NOT_SYMBOL ? "VARCHAR" : "SYMBOL";
    }
    case NANOARROW_TYPE_BINARY: case NANOARROW_TYPE_LARGE_BINARY: case NANOARROW_TYPE_BINARY_VIEW: return "BINARY";
    case NANOARROW_TYPE_FIXED_SIZE_BINARY: return sv.fixed_size == 16 ? "UUID" : "BINARY";
    case NANOARROW_TYPE_TIMESTAMP: return sv.time_unit == NANOARROW_TIME_UNIT_NANO ? "TIMESTAMP_NS" : "TIMESTAMP";
    case NANOARROW_TYPE_DATE32: case NANOARROW_TYPE_DATE64: return "DATE";
    case NANOARROW_TYPE_DECIMAL128:
      snprintf(buf, n, "DECIMAL(%d,%d)", sv.decimal_precision, sv.decimal_scale);
      return buf;
    case NANOARROW_TYPE_LIST: case NANOARROW_TYPE_LARGE_LIST: case NANOARROW_TYPE_FIXED_SIZE_LIST: {
      struct ArrowSchemaView cv;
      if (field->n_children != 1 || ArrowSchemaViewInit(&cv, field->children[0], NULL) != NANOARROW_OK) return NULL;
      return cv.type == NANOARROW_TYPE_DOUBLE ? "DOUBLE[]" : NULL;
    }
    default: return NULL;
  }
}

/// CREATE TABLE for the data's schema.  `ts_name` is the designated timestamp
/// (a data column, or an extra `timestamp` column when `add_ts`).
static AdbcStatusCode QdbCreateTableFromSchema(struct QdbConnection* conn, const char* table,
                                               const struct ArrowSchema* schema, const char* ts_name, bool add_ts,
                                               enum QdbSymbolMode mode, const char* partition_by,
                                               struct AdbcError* error) {
  struct InternalAdbcStringBuilder sb;
  if (InternalAdbcStringBuilderInit(&sb, 512)) return ADBC_STATUS_INTERNAL;
  char* q = QdbQuoteIdent(table);
  int rc = InternalAdbcStringBuilderAppend(&sb, "CREATE TABLE %s (", q);
  free(q);
  for (int64_t i = 0; i < schema->n_children && !rc; i++) {
    char buf[48];
    const char* qt = QdbQuestTypeFor(schema->children[i], mode, buf, sizeof(buf));
    if (!qt) {
      struct ArrowSchemaView sv;
      const char* tn = ArrowSchemaViewInit(&sv, schema->children[i], NULL) == NANOARROW_OK ? ArrowTypeString(sv.type) : "?";
      InternalAdbcSetError(error, "Column %s: Arrow type %s has no QuestDB column type for ingest",
                           schema->children[i]->name ? schema->children[i]->name : "", tn);
      InternalAdbcStringBuilderReset(&sb);
      return ADBC_STATUS_NOT_IMPLEMENTED;
    }
    char* qc = QdbQuoteIdent(schema->children[i]->name ? schema->children[i]->name : "");
    rc = InternalAdbcStringBuilderAppend(&sb, "%s%s %s", i ? ", " : "", qc, qt);
    free(qc);
  }
  if (!rc && add_ts) rc = InternalAdbcStringBuilderAppend(&sb, "%s\"%s\" TIMESTAMP", schema->n_children ? ", " : "", ts_name);
  if (!rc) rc = InternalAdbcStringBuilderAppend(&sb, ")");
  if (!rc && ts_name) {
    char* qts = QdbQuoteIdent(ts_name);
    rc = InternalAdbcStringBuilderAppend(&sb, " TIMESTAMP(%s)", qts);
    free(qts);
    if (!rc && partition_by && strcmp(partition_by, "NONE") != 0)
      rc = InternalAdbcStringBuilderAppend(&sb, " PARTITION BY %s WAL", partition_by);
  }
  if (rc) { InternalAdbcStringBuilderReset(&sb); return ADBC_STATUS_INTERNAL; }
  AdbcStatusCode status = QdbExecSimple(conn, sb.buffer, error);
  InternalAdbcStringBuilderReset(&sb);
  return status;
}

static AdbcStatusCode QdbIngest(struct QdbStatement* stmt, int64_t* rows_affected, struct AdbcError* error) {
  struct QdbConnection* conn = stmt->conn;
  if (!stmt->has_bind) { InternalAdbcSetError(error, "Bulk ingest needs bound data (AdbcStatementBind)"); return ADBC_STATUS_INVALID_STATE; }
  questdb_error* err = NULL;
  line_sender_table_name table;
  if (!line_sender_table_name_init(&table, strlen(stmt->ingest_table), stmt->ingest_table, &err))
    return QdbError(error, err, "ingest target table name");

  // Existence and designated timestamp.
  struct QdbTableInfo existing = {0};
  bool exists = false;
  RAISE_ADBC(QdbFindTable(conn, stmt->ingest_table, &existing, &exists, error));
  AdbcStatusCode status = ADBC_STATUS_OK;
  switch (stmt->ingest_mode) {
    case QDB_INGEST_CREATE:
      if (exists) { InternalAdbcSetError(error, "Table %s already exists", stmt->ingest_table); status = ADBC_STATUS_ALREADY_EXISTS; }
      break;
    case QDB_INGEST_APPEND:
      if (!exists) { InternalAdbcSetError(error, "Table %s does not exist", stmt->ingest_table); status = ADBC_STATUS_NOT_FOUND; }
      break;
    case QDB_INGEST_REPLACE:
      if (exists) {
        char* q = QdbQuoteIdent(stmt->ingest_table);
        size_t n = strlen(q) + 32;
        char* sql = (char*)malloc(n);
        snprintf(sql, n, "DROP TABLE IF EXISTS %s", q);
        status = QdbExecSimple(conn, sql, error);
        free(sql); free(q);
        exists = false;
      }
      break;
    case QDB_INGEST_CREATE_APPEND:
      break;
  }
  if (status != ADBC_STATUS_OK) { free(existing.name); free(existing.designated_ts); return status; }

  // Schema of the data.
  struct ArrowSchema schema;
  if (stmt->bind.get_schema(&stmt->bind, &schema) != 0) {
    InternalAdbcSetError(error, "Bound data: get_schema failed: %s", stmt->bind.get_last_error(&stmt->bind));
    free(existing.name); free(existing.designated_ts);
    return ADBC_STATUS_INVALID_ARGUMENT;
  }
  // Designated timestamp: the option, else the table's own (if the batch has
  // it), else the single timestamp column of a new table, else server time.
  char* ts_name = stmt->ingest_ts ? QdbStrdup(stmt->ingest_ts) : NULL;
  if (!ts_name && exists && existing.designated_ts && *existing.designated_ts) {
    if (QdbColIndex(&schema, existing.designated_ts) >= 0) ts_name = QdbStrdup(existing.designated_ts);
  }
  bool add_ts = false;  // a new table gets a server-stamped `timestamp` column
  if (!ts_name && !exists) {
    int64_t only = -1, count = 0;
    for (int64_t i = 0; i < schema.n_children; i++) {
      if (QdbSchemaIsTimestamp(schema.children[i])) { only = i; count++; }
    }
    if (count == 1) ts_name = QdbStrdup(schema.children[only]->name);
  }
  if (ts_name && QdbColIndex(&schema, ts_name) < 0) {
    InternalAdbcSetError(error, "Designated timestamp column %s is not in the bound data", ts_name);
    free(ts_name); schema.release(&schema); free(existing.name); free(existing.designated_ts);
    return ADBC_STATUS_INVALID_ARGUMENT;
  }
  if (!exists) {
    const char* ts_for_create = ts_name;
    if (!ts_for_create) {
      if (QdbColIndex(&schema, "timestamp") >= 0) {
        InternalAdbcSetError(error, "No timestamp column to use as the designated timestamp, and the data already has a "
                             "column named \"timestamp\"; set %s", QDB_OPT_INGEST_TS);
        schema.release(&schema); free(existing.name); free(existing.designated_ts);
        return ADBC_STATUS_INVALID_ARGUMENT;
      }
      ts_for_create = "timestamp";
      add_ts = true;
    }
    status = QdbCreateTableFromSchema(conn, stmt->ingest_table, &schema, ts_for_create, add_ts, stmt->symbol_mode,
                                      stmt->partition_by ? stmt->partition_by : "DAY", error);
    if (status != ADBC_STATUS_OK) {
      free(ts_name); schema.release(&schema); free(existing.name); free(existing.designated_ts);
      return status;
    }
  }
  // Symbol overrides.
  qwp_arrow_override* overrides = NULL;
  size_t n_over = 0;
  if (stmt->symbol_mode != QDB_SYMBOL_AUTO) {
    overrides = (qwp_arrow_override*)calloc((size_t)schema.n_children, sizeof(*overrides));
    for (int64_t i = 0; i < schema.n_children; i++) {
      bool dict = false;
      if (!QdbSchemaIsText(schema.children[i], &dict)) continue;
      if (ts_name && strcmp(schema.children[i]->name, ts_name) == 0) continue;
      if (stmt->symbol_mode == QDB_SYMBOL_SYMBOL && !dict) {
        overrides[n_over].column = schema.children[i]->name;
        overrides[n_over].column_len = strlen(schema.children[i]->name);
        overrides[n_over].kind = qwp_arrow_override_symbol;
        n_over++;
      } else if (stmt->symbol_mode == QDB_SYMBOL_NOT_SYMBOL && dict) {
        overrides[n_over].column = schema.children[i]->name;
        overrides[n_over].column_len = strlen(schema.children[i]->name);
        overrides[n_over].kind = qwp_arrow_override_not_symbol;
        n_over++;
      }
    }
  }
  line_sender_column_name ts_col;
  memset(&ts_col, 0, sizeof(ts_col));
  if (ts_name && !line_sender_column_name_init(&ts_col, strlen(ts_name), ts_name, &err)) {
    status = QdbError(error, err, "designated timestamp column name");
    free(overrides); free(ts_name); schema.release(&schema); free(existing.name); free(existing.designated_ts);
    return status;
  }

  qwp_sender* sender = questdb_db_borrow_sender(conn->db->pool, &err);
  if (!sender) {
    status = QdbError(error, err, "borrow a QWP sender");
    free(overrides); free(ts_name); schema.release(&schema); free(existing.name); free(existing.designated_ts);
    return status;
  }
  int64_t rows = 0;
  for (;;) {
    struct ArrowArray batch;
    memset(&batch, 0, sizeof(batch));
    if (stmt->bind.get_next(&stmt->bind, &batch) != 0) {
      InternalAdbcSetError(error, "Bound data: get_next failed: %s", stmt->bind.get_last_error(&stmt->bind));
      status = ADBC_STATUS_INVALID_ARGUMENT;
      break;
    }
    if (!batch.release) break;
    if (batch.length == 0) { batch.release(&batch); continue; }
    if (QdbCancelled(stmt->cancel)) {
      batch.release(&batch);
      InternalAdbcSetError(error, "Ingest cancelled after %" PRId64 " rows", rows);
      status = ADBC_STATUS_CANCELLED;
      break;
    }
    int64_t n = batch.length;
    bool ok;
    if (ts_name) {
      ok = qwp_sender_flush_arrow_batch_at_column_and_wait(sender, table, &batch, &schema, ts_col, overrides, n_over,
                                                           qwpws_ack_level_ok, &err);
    } else {
      ok = qwp_sender_flush_arrow_batch_at_now_and_wait(sender, table, &batch, &schema, overrides, n_over,
                                                        qwpws_ack_level_ok, &err);
    }
    if (batch.release) batch.release(&batch);
    if (!ok) { status = QdbError(error, err, "ingest"); break; }
    rows += n;
  }
  questdb_db_return_sender(conn->db->pool, sender);
  free(overrides);
  free(ts_name);
  schema.release(&schema);
  free(existing.name);
  free(existing.designated_ts);
  if (status == ADBC_STATUS_OK && rows > 0 && stmt->wait_for_apply)
    status = QdbWaitForApply(conn, stmt->ingest_table, stmt->apply_timeout_ms, error);
  if (rows_affected) *rows_affected = rows;
  return status;
}

// --- execute ------------------------------------------------------------------

static AdbcStatusCode QdbStatementExecuteQuery(struct AdbcStatement* statement, struct ArrowArrayStream* out,
                                               int64_t* rows_affected, struct AdbcError* error) {
  struct QdbStatement* stmt = (struct QdbStatement*)statement->private_data;
  if (!stmt) return ADBC_STATUS_INVALID_STATE;
  if (rows_affected) *rows_affected = -1;
  OdbcMutexLock(&stmt->cancel->lock);
  stmt->cancel->cancelled = false;
  OdbcMutexUnlock(&stmt->cancel->lock);

  if (stmt->ingest_table) {
    int64_t n = 0;
    RAISE_ADBC(QdbIngest(stmt, &n, error));
    QdbStatementDropBind(stmt);
    if (rows_affected) *rows_affected = n;
    if (out) {
      struct ArrowSchema schema;
      ArrowSchemaInit(&schema);
      CHECK_NA(INTERNAL, ArrowSchemaSetTypeStruct(&schema, 0), error);
      CHECK_NA(INTERNAL, ArrowBasicArrayStreamInit(out, &schema, 0), error);
    }
    return ADBC_STATUS_OK;
  }
  if (!stmt->sql) { InternalAdbcSetError(error, "No query has been set"); return ADBC_STATUS_INVALID_STATE; }

  AdbcStatusCode status;
  struct QdbRun* run = QdbRunNew(stmt, error, &status);
  if (!run) return status;
  if (!out) {
    status = QdbRunDrain(run, error);
    if (status == ADBC_STATUS_OK && rows_affected) *rows_affected = run->rows_known ? run->rows_affected : -1;
    QdbRunFree(run);
    return status;
  }
  status = QdbRunExport(run, out, error);
  // A DML statement run through ExecuteQuery has no batches, only a terminal
  // with its row count; report it the way ExecuteUpdate would.
  if (status == ADBC_STATUS_OK && rows_affected && run->rows_known) *rows_affected = run->rows_affected;
  return status;
}

static AdbcStatusCode QdbStatementExecuteSchema(struct AdbcStatement* statement, struct ArrowSchema* schema,
                                                struct AdbcError* error) {
  struct QdbStatement* stmt = (struct QdbStatement*)statement->private_data;
  if (!stmt) return ADBC_STATUS_INVALID_STATE;
  if (!stmt->sql) { InternalAdbcSetError(error, "No query has been set"); return ADBC_STATUS_INVALID_STATE; }
  AdbcStatusCode status;
  struct QdbRun* run = QdbRunNew(stmt, error, &status);
  if (!run) return status;
  struct ArrowArrayStream stream;
  memset(&stream, 0, sizeof(stream));
  status = QdbRunExport(run, &stream, error);
  if (status != ADBC_STATUS_OK) return status;
  int rc = stream.get_schema(&stream, schema);
  stream.release(&stream);
  if (rc != 0) { InternalAdbcSetError(error, "ExecuteSchema: schema copy failed"); return ADBC_STATUS_INTERNAL; }
  return ADBC_STATUS_OK;
}

static AdbcStatusCode QdbStatementExecutePartitions(struct AdbcStatement* statement, struct ArrowSchema* schema,
                                                    struct AdbcPartitions* partitions, int64_t* rows_affected,
                                                    struct AdbcError* error) {
  InternalAdbcSetError(error, "Partitioned execution is not supported");
  return ADBC_STATUS_NOT_IMPLEMENTED;
}

// ---------------------------------------------------------------------------
// Driver init

static AdbcStatusCode QdbDriverRelease(struct AdbcDriver* driver, struct AdbcError* error) {
  driver->private_data = NULL;
  return ADBC_STATUS_OK;
}

static const struct AdbcError* QdbErrorFromArrayStream(struct ArrowArrayStream* stream, AdbcStatusCode* status) {
  return NULL;
}

QDB_EXPORT
AdbcStatusCode AdbcDriverQuestDBInit(int version, void* raw_driver, struct AdbcError* error) {
  if (version != ADBC_VERSION_1_0_0 && version != ADBC_VERSION_1_1_0) {
    InternalAdbcSetError(error, "Only ADBC 1.0.0 and 1.1.0 are supported");
    return ADBC_STATUS_NOT_IMPLEMENTED;
  }
  struct AdbcDriver* driver = (struct AdbcDriver*)raw_driver;
  memset(driver, 0, version == ADBC_VERSION_1_0_0 ? ADBC_DRIVER_1_0_0_SIZE : ADBC_DRIVER_1_1_0_SIZE);
  driver->private_data = (void*)(intptr_t)version;
  driver->release = QdbDriverRelease;

  driver->DatabaseInit = QdbDatabaseInit;
  driver->DatabaseNew = QdbDatabaseNew;
  driver->DatabaseRelease = QdbDatabaseRelease;
  driver->DatabaseSetOption = QdbDatabaseSetOption;

  driver->ConnectionCommit = QdbConnectionCommit;
  driver->ConnectionGetInfo = QdbConnectionGetInfo;
  driver->ConnectionGetObjects = QdbConnectionGetObjects;
  driver->ConnectionGetTableSchema = QdbConnectionGetTableSchema;
  driver->ConnectionGetTableTypes = QdbConnectionGetTableTypes;
  driver->ConnectionInit = QdbConnectionInit;
  driver->ConnectionNew = QdbConnectionNew;
  driver->ConnectionReadPartition = QdbConnectionReadPartition;
  driver->ConnectionRelease = QdbConnectionRelease;
  driver->ConnectionRollback = QdbConnectionRollback;
  driver->ConnectionSetOption = QdbConnectionSetOption;

  driver->StatementBind = QdbStatementBind;
  driver->StatementBindStream = QdbStatementBindStream;
  driver->StatementExecutePartitions = QdbStatementExecutePartitions;
  driver->StatementExecuteQuery = QdbStatementExecuteQuery;
  driver->StatementGetParameterSchema = QdbStatementGetParameterSchema;
  driver->StatementNew = QdbStatementNew;
  driver->StatementPrepare = QdbStatementPrepare;
  driver->StatementRelease = QdbStatementRelease;
  driver->StatementSetOption = QdbStatementSetOption;
  driver->StatementSetSqlQuery = QdbStatementSetSqlQuery;
  driver->StatementSetSubstraitPlan = QdbStatementSetSubstraitPlan;

  if (version >= ADBC_VERSION_1_1_0) {
    driver->ErrorGetDetailCount = InternalAdbcCommonErrorGetDetailCount;
    driver->ErrorGetDetail = InternalAdbcCommonErrorGetDetail;
    driver->ErrorFromArrayStream = QdbErrorFromArrayStream;
    driver->DatabaseGetOption = QdbDatabaseGetOption;
    driver->DatabaseGetOptionBytes = QdbDatabaseGetOptionBytes;
    driver->DatabaseGetOptionDouble = QdbDatabaseGetOptionDouble;
    driver->DatabaseGetOptionInt = QdbDatabaseGetOptionInt;
    driver->DatabaseSetOptionBytes = QdbDatabaseSetOptionBytes;
    driver->DatabaseSetOptionDouble = QdbDatabaseSetOptionDouble;
    driver->DatabaseSetOptionInt = QdbDatabaseSetOptionInt;
    driver->ConnectionCancel = QdbConnectionCancel;
    driver->ConnectionGetOption = QdbConnectionGetOption;
    driver->ConnectionGetOptionBytes = QdbConnectionGetOptionBytes;
    driver->ConnectionGetOptionDouble = QdbConnectionGetOptionDouble;
    driver->ConnectionGetOptionInt = QdbConnectionGetOptionInt;
    driver->ConnectionGetStatistics = QdbConnectionGetStatistics;
    driver->ConnectionGetStatisticNames = QdbConnectionGetStatisticNames;
    driver->ConnectionSetOptionBytes = QdbConnectionSetOptionBytes;
    driver->ConnectionSetOptionDouble = QdbConnectionSetOptionDouble;
    driver->ConnectionSetOptionInt = QdbConnectionSetOptionInt;
    driver->StatementCancel = QdbStatementCancel;
    driver->StatementExecuteSchema = QdbStatementExecuteSchema;
    driver->StatementGetOption = QdbStatementGetOption;
    driver->StatementGetOptionBytes = QdbStatementGetOptionBytes;
    driver->StatementGetOptionDouble = QdbStatementGetOptionDouble;
    driver->StatementGetOptionInt = QdbStatementGetOptionInt;
    driver->StatementSetOptionBytes = QdbStatementSetOptionBytes;
    driver->StatementSetOptionDouble = QdbStatementSetOptionDouble;
    driver->StatementSetOptionInt = QdbStatementSetOptionInt;
  }
  return ADBC_STATUS_OK;
}

QDB_EXPORT
AdbcStatusCode AdbcDriverInit(int version, void* driver, struct AdbcError* error) {
  return AdbcDriverQuestDBInit(version, driver, error);
}
