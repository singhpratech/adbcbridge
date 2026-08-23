// SPDX-License-Identifier: Apache-2.0
//
// Unit tests for the SQL-type -> Arrow-type mapping and the textual date/time
// parsers in odbc_reader.c.  Most of these SQL types (SQL_SS_TIME2,
// SQL_SS_TIMESTAMPOFFSET, SQL_GUID, SQL_INTERVAL_*, SQL_DECIMAL) are never
// reported by the SQLite ODBC driver used in tests/test_sqlite.py, so they are
// covered here instead.  The reader's translation unit is included so that its
// internal helpers are visible.

#include "odbc_reader.c"

#include "test_common.h"

static const struct OdbcReaderOptions kDefaultOpts = {
    .batch_size = ADBC_ODBC_DEFAULT_BATCH_SIZE,
    .max_bind_bytes = ADBC_ODBC_DEFAULT_MAX_BIND_BYTES,
    .decimal_as_string = false,
};

static struct OdbcColumn Classify(SQLSMALLINT sql_type, SQLULEN column_size,
                                  SQLSMALLINT decimal_digits,
                                  const struct OdbcReaderOptions* opts) {
  struct OdbcColumn c;
  memset(&c, 0, sizeof(c));
  c.sql_type = sql_type;
  c.column_size = column_size;
  c.decimal_digits = decimal_digits;
  c.nullable = SQL_NULLABLE;
  // A NULL statement handle makes the SQLColAttribute probes fall back to
  // their defaults, which is what a driver that cannot answer them does too.
  ClassifyColumn(NULL, 1, &c, opts ? opts : &kDefaultOpts);
  return c;
}

static void TestClassifyTime(void) {
  // Whole seconds: TIME_STRUCT is enough.
  struct OdbcColumn c = Classify(SQL_TYPE_TIME, 8, 0, NULL);
  CHECK_I64(c.kind, FETCH_TIME);
  CHECK_I64(c.c_type, SQL_C_TYPE_TIME);
  CHECK_I64(c.elem_size, (SQLLEN)sizeof(TIME_STRUCT));
  CHECK_TRUE(c.bound);

  c = Classify(SQL_TIME, 8, 0, NULL);
  CHECK_I64(c.kind, FETCH_TIME);

  // Fractional seconds: TIME_STRUCT has no sub-second field, so the value has
  // to be fetched as text and parsed into time64.  1-6 digits -> time64[us].
  c = Classify(SQL_TYPE_TIME, 15, 6, NULL);
  CHECK_I64(c.kind, FETCH_TIME64);
  CHECK_I64(c.c_type, SQL_C_CHAR);
  CHECK_I64(c.unit, NANOARROW_TIME_UNIT_MICRO);
  CHECK_TRUE(c.bound);
  CHECK_TRUE(c.elem_size >= 40);

  c = Classify(SQL_TYPE_TIME, 12, 3, NULL);
  CHECK_I64(c.kind, FETCH_TIME64);
  CHECK_I64(c.unit, NANOARROW_TIME_UNIT_MICRO);

  // SQL Server's SQL_SS_TIME2 (-154) behaves the same way, and its TIME(7)
  // needs more resolution than microseconds -> time64[ns].
  c = Classify(SQL_SS_TIME2, 16, 7, NULL);
  CHECK_I64(c.kind, FETCH_TIME64);
  CHECK_I64(c.c_type, SQL_C_CHAR);
  CHECK_I64(c.unit, NANOARROW_TIME_UNIT_NANO);

  c = Classify(SQL_TYPE_TIME, 18, 9, NULL);
  CHECK_I64(c.unit, NANOARROW_TIME_UNIT_NANO);
  c = Classify(SQL_SS_TIME2, 8, 0, NULL);
  CHECK_I64(c.kind, FETCH_TIME);
  CHECK_I64(c.c_type, SQL_C_TYPE_TIME);

  // Arrow has no time-with-timezone type; keep the text form.
  c = Classify(SQL_TYPE_TIME_WITH_TIMEZONE, 14, 0, NULL);
  CHECK_I64(c.kind, FETCH_CHAR);
  CHECK_TRUE(c.bound);
}

static void TestClassifyTimestamp(void) {
  // The reported fractional-seconds scale picks the Arrow unit:
  // 1-3 -> [ms], 4-6 -> [us], 7-9 -> [ns].  A reported 0 is only believed when the
  // column size (20 + digits) confirms it; a bare 0/19 stays at the lossless
  // microsecond default (MySQL Connector/ODBC reports 0/19 for DATETIME(6)).
  struct OdbcColumn c = Classify(SQL_TYPE_TIMESTAMP, 19, 0, NULL);
  CHECK_I64(c.kind, FETCH_TIMESTAMP);
  CHECK_I64(c.c_type, SQL_C_TYPE_TIMESTAMP);
  CHECK_I64(c.unit, NANOARROW_TIME_UNIT_MICRO);

  c = Classify(SQL_TYPE_TIMESTAMP, 21, 1, NULL);
  CHECK_I64(c.unit, NANOARROW_TIME_UNIT_MILLI);

  c = Classify(SQL_TYPE_TIMESTAMP, 23, 3, NULL);
  CHECK_I64(c.kind, FETCH_TIMESTAMP);
  CHECK_I64(c.c_type, SQL_C_TYPE_TIMESTAMP);
  CHECK_I64(c.unit, NANOARROW_TIME_UNIT_MILLI);

  c = Classify(SQL_TYPE_TIMESTAMP, 24, 4, NULL);
  CHECK_I64(c.unit, NANOARROW_TIME_UNIT_MICRO);

  c = Classify(SQL_TYPE_TIMESTAMP, 26, 6, NULL);
  CHECK_I64(c.unit, NANOARROW_TIME_UNIT_MICRO);

  c = Classify(SQL_TYPE_TIMESTAMP, 27, 7, NULL);
  CHECK_I64(c.unit, NANOARROW_TIME_UNIT_NANO);

  c = Classify(SQL_TYPE_TIMESTAMP, 30, 9, NULL);
  CHECK_I64(c.unit, NANOARROW_TIME_UNIT_NANO);

  // SQL Server's SQL_SS_TIMESTAMPOFFSET (-155): text, parsed to UTC micros.
  c = Classify(SQL_SS_TIMESTAMPOFFSET, 34, 7, NULL);
  CHECK_I64(c.kind, FETCH_TIMESTAMP_TZ);
  CHECK_I64(c.c_type, SQL_C_CHAR);
  CHECK_I64(c.unit, NANOARROW_TIME_UNIT_MICRO);
  CHECK_TRUE(c.bound);
  CHECK_TRUE(c.elem_size >= 80);

  c = Classify(SQL_TYPE_TIMESTAMP_WITH_TIMEZONE, 35, 6, NULL);
  CHECK_I64(c.kind, FETCH_TIMESTAMP_TZ);
  CHECK_I64(c.c_type, SQL_C_CHAR);
}

static void TestClassifyStringy(void) {
  // A GUID is text, and drivers may report column_size 0 for it; the column
  // must still be bound with a buffer big enough for "{...}".
  struct OdbcColumn c = Classify(SQL_GUID, 0, 0, NULL);
  CHECK_I64(c.kind, FETCH_CHAR);
  CHECK_I64(c.c_type, SQL_C_CHAR);
  CHECK_TRUE(c.bound);
  CHECK_TRUE(c.elem_size >= 39);

  const SQLSMALLINT intervals[] = {
      SQL_INTERVAL_YEAR,          SQL_INTERVAL_MONTH,
      SQL_INTERVAL_DAY,           SQL_INTERVAL_HOUR,
      SQL_INTERVAL_MINUTE,        SQL_INTERVAL_SECOND,
      SQL_INTERVAL_YEAR_TO_MONTH, SQL_INTERVAL_DAY_TO_HOUR,
      SQL_INTERVAL_DAY_TO_MINUTE, SQL_INTERVAL_DAY_TO_SECOND,
      SQL_INTERVAL_HOUR_TO_MINUTE, SQL_INTERVAL_HOUR_TO_SECOND,
      SQL_INTERVAL_MINUTE_TO_SECOND};
  for (size_t i = 0; i < sizeof(intervals) / sizeof(intervals[0]); i++) {
    c = Classify(intervals[i], 0, 0, NULL);
    CHECK_I64(c.kind, FETCH_CHAR);
    CHECK_I64(c.c_type, SQL_C_CHAR);
    CHECK_TRUE(c.bound);
    CHECK_TRUE(c.elem_size >= 64);
  }

  // A text buffer that would blow the binding budget falls back to SQLGetData
  // instead of silently truncating the value.
  struct OdbcReaderOptions tight = kDefaultOpts;
  tight.max_bind_bytes = 16;
  c = Classify(SQL_GUID, 36, 0, &tight);
  CHECK_I64(c.kind, FETCH_CHAR);
  CHECK_TRUE(!c.bound);
  c = Classify(SQL_INTERVAL_DAY_TO_SECOND, 1 << 20, 0, NULL);
  CHECK_I64(c.kind, FETCH_CHAR);
  CHECK_TRUE(!c.bound);
}

static void TestClassifyDecimal(void) {
  // Representable as decimal128.
  struct OdbcColumn c = Classify(SQL_DECIMAL, 10, 3, NULL);
  CHECK_I64(c.kind, FETCH_DECIMAL);
  CHECK_I64(c.precision, 10);
  CHECK_I64(c.scale, 3);
  c = Classify(SQL_NUMERIC, 38, 0, NULL);
  CHECK_I64(c.kind, FETCH_DECIMAL);

  // Not representable as decimal128 -> string, losslessly.
  c = Classify(SQL_DECIMAL, 0, 0, NULL);
  CHECK_I64(c.kind, FETCH_CHAR);
  c = Classify(SQL_DECIMAL, 39, 2, NULL);
  CHECK_I64(c.kind, FETCH_CHAR);
  c = Classify(SQL_NUMERIC, 65, 30, NULL);
  CHECK_I64(c.kind, FETCH_CHAR);
  // Nonsensical scale.
  c = Classify(SQL_DECIMAL, 10, 12, NULL);
  CHECK_I64(c.kind, FETCH_CHAR);
  c = Classify(SQL_DECIMAL, 10, -2, NULL);
  CHECK_I64(c.kind, FETCH_CHAR);

  // ...or on request.
  struct OdbcReaderOptions as_string = kDefaultOpts;
  as_string.decimal_as_string = true;
  c = Classify(SQL_DECIMAL, 10, 3, &as_string);
  CHECK_I64(c.kind, FETCH_CHAR);
}

// The Arrow type each classification produces, end to end through BuildSchema.
static void CheckFormat(SQLSMALLINT sql_type, SQLULEN column_size, SQLSMALLINT decimal_digits,
                        const char* expected_format) {
  struct OdbcColumn c = Classify(sql_type, column_size, decimal_digits, NULL);
  c.name = strdup("v");
  struct ArrowSchema schema;
  struct AdbcError error = ADBC_ERROR_INIT;
  CHECK_I64(BuildSchema(&c, 1, &schema, &error), ADBC_STATUS_OK);
  CHECK_STR(schema.children[0]->format, strlen(schema.children[0]->format), expected_format);
  schema.release(&schema);
  if (error.release) error.release(&error);
  free(c.name);
}

static void TestSchemaFormats(void) {
  CheckFormat(SQL_TYPE_TIME, 8, 0, "tts");                     // time32[s]
  CheckFormat(SQL_TYPE_TIME, 15, 6, "ttu");                    // time64[us]
  CheckFormat(SQL_SS_TIME2, 16, 7, "ttn");                     // time64[ns]
  CheckFormat(SQL_TYPE_TIMESTAMP, 19, 0, "tsu:");              // timestamp[s]
  CheckFormat(SQL_TYPE_TIMESTAMP, 23, 3, "tsm:");              // timestamp[ms]
  CheckFormat(SQL_TYPE_TIMESTAMP, 26, 6, "tsu:");              // timestamp[us]
  CheckFormat(SQL_TYPE_TIMESTAMP, 30, 9, "tsn:");              // timestamp[ns]
  CheckFormat(SQL_SS_TIMESTAMPOFFSET, 34, 7, "tsu:UTC");       // timestamp[us, UTC]
  CheckFormat(SQL_TYPE_TIMESTAMP_WITH_TIMEZONE, 35, 6, "tsu:UTC");
  CheckFormat(SQL_TYPE_TIME_WITH_TIMEZONE, 14, 0, "u");        // string
  CheckFormat(SQL_GUID, 36, 0, "u");                           // string
  CheckFormat(SQL_INTERVAL_DAY_TO_SECOND, 0, 0, "u");          // string
  CheckFormat(SQL_DECIMAL, 10, 3, "d:10,3");                   // decimal128(10, 3)
  CheckFormat(SQL_DECIMAL, 39, 3, "u");                        // too wide -> string
  CheckFormat(SQL_DECIMAL, 0, 0, "u");                         // unknown -> string
}

static void CheckTime(const char* s, int64_t expected) {
  int64_t v = -1;
  if (!ParseTimeScaled(s, strlen(s), 6, &v)) {
    fprintf(stderr, "FAIL: could not parse time '%s'\n", s);
    adbc_test_failures++;
    return;
  }
  CHECK_I64(v, expected);
}

static void CheckTimeInvalid(const char* s) {
  int64_t v = 0;
  if (ParseTimeScaled(s, strlen(s), 6, &v)) {
    fprintf(stderr, "FAIL: accepted bad time '%s' (-> %lld)\n", s, (long long)v);
    adbc_test_failures++;
  }
}

static void TestParseTime(void) {
  CheckTime("00:00:00", 0);
  CheckTime("12:34:56", 45296000000LL);
  CheckTime("12:34:56.789012", 45296789012LL);
  // SQL Server renders time(7) with 7 digits; the extra digit is truncated.
  CheckTime("12:34:56.7890129", 45296789012LL);
  // Fewer digits than microseconds are right-padded, not left-padded.
  CheckTime("12:34:56.5", 45296500000LL);
  CheckTime("00:00:00.000001", 1);
  CheckTime("23:59:59.999999", 86399999999LL);
  CheckTime("12:34", 45240000000LL);
  CheckTime("  12:34:56  ", 45296000000LL);

  CheckTimeInvalid("");
  CheckTimeInvalid("nope");
  CheckTimeInvalid("12");
  CheckTimeInvalid("24:00:00");
  CheckTimeInvalid("12:60:00");
  CheckTimeInvalid("12:34:60");
  CheckTimeInvalid("12:34:56xyz");
}

static void CheckTimeNanos(const char* s, int64_t expected) {
  int64_t v = -1;
  if (!ParseTimeScaled(s, strlen(s), 9, &v)) {
    fprintf(stderr, "FAIL: could not parse time '%s' as nanos\n", s);
    adbc_test_failures++;
    return;
  }
  CHECK_I64(v, expected);
}

// A TIME(7..9) column is fetched as text and parsed straight into time64[ns];
// the microsecond scale would throw the extra digits away.
static void TestParseTimeNanos(void) {
  CheckTimeNanos("00:00:00", 0);
  CheckTimeNanos("12:34:56", 45296000000000LL);
  CheckTimeNanos("12:34:56.789012345", 45296789012345LL);
  // SQL Server renders time(7) with exactly 7 digits: the rest is zero-padded.
  CheckTimeNanos("12:34:56.7890123", 45296789012300LL);
  CheckTimeNanos("12:34:56.5", 45296500000000LL);
  CheckTimeNanos("00:00:00.000000001", 1);
  CheckTimeNanos("23:59:59.999999999", 86399999999999LL);
  // A tenth digit is beyond nanosecond resolution and is truncated.
  CheckTimeNanos("23:59:59.9999999991", 86399999999999LL);
}

static void CheckTimestamp(const char* s, int64_t expected) {
  int64_t v = -1;
  if (!ParseTimestampUtcMicros(s, strlen(s), &v)) {
    fprintf(stderr, "FAIL: could not parse timestamp '%s'\n", s);
    adbc_test_failures++;
    return;
  }
  CHECK_I64(v, expected);
}

static void CheckTimestampInvalid(const char* s) {
  int64_t v = 0;
  if (ParseTimestampUtcMicros(s, strlen(s), &v)) {
    fprintf(stderr, "FAIL: accepted bad timestamp '%s' (-> %lld)\n", s, (long long)v);
    adbc_test_failures++;
  }
}

static void TestParseTimestamp(void) {
  CheckTimestamp("1970-01-01 00:00:00", 0);
  CheckTimestamp("1970-01-01T00:00:00Z", 0);
  CheckTimestamp("1970-01-01", 0);
  CheckTimestamp("2024-02-29 13:45:10.123456", 1709214310123456LL);
  CheckTimestamp("2024-02-29T13:45:10.123456Z", 1709214310123456LL);
  // No offset means UTC; an offset is subtracted to get UTC.
  CheckTimestamp("2024-02-29 13:45:10.123456+02:00", 1709207110123456LL);
  CheckTimestamp("2024-02-29 13:45:10.123456-05:00", 1709232310123456LL);
  CheckTimestamp("2024-02-29 13:45:10.123456+0200", 1709207110123456LL);
  CheckTimestamp("2024-02-29 13:45:10.123456+02", 1709207110123456LL);
  // SQL Server's datetimeoffset text form: 7 fractional digits, blank before
  // the offset.
  CheckTimestamp("2024-02-29 13:45:10.1234567 -05:00", 1709232310123456LL);
  CheckTimestamp("2024-02-29", 1709164800000000LL);
  // Before the epoch, where the fraction must not be subtracted.
  CheckTimestamp("1899-12-31 23:59:59.999999", -2208988800000001LL);

  CheckTimestampInvalid("");
  CheckTimestampInvalid("not a timestamp");
  CheckTimestampInvalid("2024-02-29 25:00:00");
  CheckTimestampInvalid("2024-13-01 00:00:00");
  CheckTimestampInvalid("2024-00-01 00:00:00");
  CheckTimestampInvalid("2024-02-29 13:45:10 junk");
  CheckTimestampInvalid("2024-02-29 13:45:10+99:00");
  CheckTimestampInvalid("2024/02/29 13:45:10");
}

int main(void) {
  TestClassifyTime();
  TestClassifyTimestamp();
  TestClassifyStringy();
  TestClassifyDecimal();
  TestSchemaFormats();
  TestParseTime();
  TestParseTimeNanos();
  TestParseTimestamp();
  return TEST_MAIN_RESULT();
}
