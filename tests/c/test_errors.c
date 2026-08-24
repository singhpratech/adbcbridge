// SPDX-License-Identifier: Apache-2.0
//
// Unit tests for the diagnostic-collection path in odbc_reader.c.
//
// SQLGetDiagRec fills at most BufferLength-1 bytes but reports the *untruncated*
// message length, so a backend message longer than the buffer (MSSQL RAISERROR,
// an Oracle error stack) used to send both the "already exists" substring search
// and the "%.*s" formatting past the end of the on-stack buffer.  The fake below
// reproduces exactly that; ASAN builds catch the over-read, and the length
// assertions catch it everywhere else.
//
// The translation unit is included so that OdbcSetError's fake SQLGetDiagRec
// (defined here, and therefore preferred over libodbc's) is the one it calls.

#include "odbc_reader.c"

#include "test_common.h"

// ---------------------------------------------------------------------------
// Fake diagnostics

static const char* g_fake_sqlstate = "HY000";
static const char* g_fake_message = "";
// Untruncated length the fake reports, whatever it actually managed to write.
static SQLSMALLINT g_fake_reported_len = 0;
static int g_fake_records = 1;
static int g_fake_calls = 0;
static bool g_fake_refetch_fails = false;

SQLRETURN SQL_API SQLGetDiagRec(SQLSMALLINT handle_type, SQLHANDLE handle, SQLSMALLINT rec,
                                SQLCHAR* sqlstate, SQLINTEGER* native, SQLCHAR* msg,
                                SQLSMALLINT buflen, SQLSMALLINT* msg_len) {
  (void)handle_type;
  (void)handle;
  if (rec > g_fake_records) return SQL_NO_DATA;
  g_fake_calls++;
  // Some drivers cannot answer a second time for the same record; make sure the
  // fallback path is exercised too.
  if (g_fake_refetch_fails && buflen > (SQLSMALLINT)SQL_MAX_MESSAGE_LENGTH) return SQL_ERROR;
  memcpy(sqlstate, g_fake_sqlstate, 6);
  if (native) *native = 4711;
  size_t full = strlen(g_fake_message);
  size_t room = buflen > 0 ? (size_t)buflen - 1 : 0;
  size_t written = full < room ? full : room;
  memcpy(msg, g_fake_message, written);
  msg[written] = '\0';
  // The point of the fake: report the length the driver *would* have written.
  if (msg_len) *msg_len = g_fake_reported_len ? g_fake_reported_len : (SQLSMALLINT)full;
  return written < full ? SQL_SUCCESS_WITH_INFO : SQL_SUCCESS;
}

// ---------------------------------------------------------------------------

static void TestOverlongMessage(void) {
  // 1800 'y's: longer than the SQL_MAX_MESSAGE_LENGTH buffer OdbcSetError uses.
  char big[1801];
  memset(big, 'y', sizeof(big) - 1);
  big[sizeof(big) - 1] = '\0';
  g_fake_sqlstate = "42000";
  g_fake_message = big;
  g_fake_reported_len = (SQLSMALLINT)(sizeof(big) - 1);
  g_fake_records = 1;
  g_fake_refetch_fails = false;
  g_fake_calls = 0;

  struct AdbcError error = ADBC_ERROR_INIT;
  AdbcStatusCode status = OdbcSetError(SQL_HANDLE_STMT, (SQLHANDLE)&error, "SQLExecDirect", &error);
  CHECK_I64(status, ADBC_STATUS_INVALID_ARGUMENT);  // 42000
  CHECK_TRUE(error.message != NULL);
  if (error.message) {
    CHECK_TRUE(strstr(error.message, "SQLExecDirect") != NULL);
    // The message is re-fetched into a buffer that holds it, so more than the
    // first SQL_MAX_MESSAGE_LENGTH bytes survive -- and, crucially, every byte
    // reported came from a buffer big enough to hold it.  (The ADBC error
    // message itself is capped at 1 KiB by InternalAdbcSetError.)
    const char* first_y = strchr(error.message, 'y');
    CHECK_TRUE(first_y != NULL);
    if (first_y) {
      size_t run = strspn(first_y, "y");
      CHECK_TRUE(run > (size_t)SQL_MAX_MESSAGE_LENGTH - 1);
      CHECK_TRUE(run <= 1024);
    }
  }
  CHECK_TRUE(memcmp(error.sqlstate, "42000", 5) == 0);
  if (error.release) error.release(&error);

  // When the driver will not answer a second time, only the bytes it really
  // wrote are reported: never the untruncated length it claimed.
  g_fake_refetch_fails = true;
  g_fake_calls = 0;
  error = ADBC_ERROR_INIT;
  status = OdbcSetError(SQL_HANDLE_STMT, (SQLHANDLE)&error, "SQLExecDirect", &error);
  CHECK_I64(status, ADBC_STATUS_INVALID_ARGUMENT);
  CHECK_TRUE(g_fake_calls >= 2);
  if (error.message) {
    const char* first_y = strchr(error.message, 'y');
    CHECK_TRUE(first_y != NULL);
    if (first_y) CHECK_I64(strspn(first_y, "y"), SQL_MAX_MESSAGE_LENGTH - 1);
  }
  if (error.release) error.release(&error);
  g_fake_refetch_fails = false;
}

static void TestAlreadyExistsFallback(void) {
  // A backend that reports "already exists" without SQLSTATE 42S01 (SQLiteODBC).
  g_fake_sqlstate = "HY000";
  g_fake_message = "table ing already exists";
  g_fake_reported_len = 0;
  g_fake_records = 1;

  struct AdbcError error = ADBC_ERROR_INIT;
  AdbcStatusCode status = OdbcSetError(SQL_HANDLE_STMT, (SQLHANDLE)&error, "SQLExecDirect", &error);
  CHECK_I64(status, ADBC_STATUS_ALREADY_EXISTS);
  CHECK_TRUE(error.message && strstr(error.message, "already exists") != NULL);
  if (error.release) error.release(&error);
}

static void TestSqlStateMapping(void) {
  g_fake_sqlstate = "42S01";
  g_fake_message = "There is already an object named 'ing' in the database.";
  g_fake_reported_len = 0;
  g_fake_records = 1;
  struct AdbcError error = ADBC_ERROR_INIT;
  CHECK_I64(OdbcSetError(SQL_HANDLE_STMT, (SQLHANDLE)&error, "SQLExecDirect", &error),
            ADBC_STATUS_ALREADY_EXISTS);
  if (error.release) error.release(&error);

  g_fake_sqlstate = "42S02";
  g_fake_message = "no such table: nope";
  error = ADBC_ERROR_INIT;
  CHECK_I64(OdbcSetError(SQL_HANDLE_STMT, (SQLHANDLE)&error, "SQLExecDirect", &error),
            ADBC_STATUS_NOT_FOUND);
  if (error.release) error.release(&error);
}

int main(void) {
  TestOverlongMessage();
  TestAlreadyExistsFallback();
  TestSqlStateMapping();
  return TEST_MAIN_RESULT();
}
