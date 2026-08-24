// SPDX-License-Identifier: Apache-2.0
//
// Unit tests for the SQL_C_WCHAR (UTF-16 -> UTF-8) conversion path in
// odbc_reader.c, plus NaN/Inf passthrough for the float columns.
//
// The SQLite ODBC driver used by tests/test_sqlite.py reports SQL_VARCHAR for
// every text column, so the FETCH_WCHAR path is unreachable from there; this
// test exercises it directly.  The reader's translation unit is included so
// that its internal helpers are visible.

#include "odbc_reader.c"

#include <math.h>

#include "test_common.h"

// Convert UTF-16 code units to UTF-8 through AppendUtf16 and compare.
// The inputs are written as UTF-16 units.  When this test is built with a four-byte
// SQLWCHAR (unixODBC's SQL_WCHART_CONVERT, the width iODBC always has) the same
// text is fed as one code point per unit, which is what such a driver manager
// carries; a lone surrogate stays a lone unit so the U+FFFD cases hold either way.
static void CheckUtf16Raw(const SQLWCHAR* w, size_t n, const char* expected);

static void CheckUtf16(const uint16_t* units, size_t n, const char* expected) {
  SQLWCHAR w[64];

  CHECK_TRUE(n <= sizeof(w) / sizeof(w[0]));
  if (sizeof(SQLWCHAR) >= 4) {
    // First as a driver that puts UTF-16 units into four-byte slots would send it
    // (MySQL Connector/ODBC on iODBC): the reader combines the pairs regardless.
    for (size_t i = 0; i < n; i++) w[i] = (SQLWCHAR)units[i];
    CheckUtf16Raw(w, n, expected);
  }
  if (sizeof(SQLWCHAR) < 4) {
    for (size_t i = 0; i < n; i++) w[i] = (SQLWCHAR)units[i];
  } else {
    size_t k = 0;
    for (size_t i = 0; i < n; i++) {
      uint32_t cp = units[i];
      if (cp >= 0xD800 && cp <= 0xDBFF && i + 1 < n && units[i + 1] >= 0xDC00 && units[i + 1] <= 0xDFFF) {
        cp = 0x10000 + ((cp - 0xD800) << 10) + (units[i + 1] - 0xDC00);
        i++;
      }
      w[k++] = (SQLWCHAR)cp;
    }
    n = k;
  }
  CheckUtf16Raw(w, n, expected);
}

// Feed n SQLWCHAR units to AppendUtf16 and compare the UTF-8 that comes out.
static void CheckUtf16Raw(const SQLWCHAR* w, size_t n, const char* expected) {
  struct ArrowArray arr;
  struct ArrowArrayView view;
  struct ArrowBuffer scratch;
  struct ArrowError na_error;

  CHECK_I64(ArrowArrayInitFromType(&arr, NANOARROW_TYPE_STRING), NANOARROW_OK);
  CHECK_I64(ArrowArrayStartAppending(&arr), NANOARROW_OK);
  ArrowBufferInit(&scratch);
  CHECK_I64(AppendUtf16(&arr, w, n, &scratch), NANOARROW_OK);
  ArrowBufferReset(&scratch);
  // FULL validation checks that the data buffer really is valid UTF-8.
  CHECK_I64(ArrowArrayFinishBuilding(&arr, NANOARROW_VALIDATION_LEVEL_FULL, &na_error),
            NANOARROW_OK);

  ArrowArrayViewInitFromType(&view, NANOARROW_TYPE_STRING);
  CHECK_I64(ArrowArrayViewSetArray(&view, &arr, &na_error), NANOARROW_OK);
  CHECK_I64(view.length, 1);
  struct ArrowStringView sv = ArrowArrayViewGetStringUnsafe(&view, 0);
  CHECK_STR(sv.data, sv.size_bytes, expected);
  ArrowArrayViewReset(&view);
  ArrowArrayRelease(&arr);
}

static void TestUtf16(void) {
  // Two-byte SQLWCHAR (unixODBC, Windows) carries UTF-16; four-byte (iODBC, or
  // unixODBC with SQL_WCHART_CONVERT) carries one code point per unit.
  CHECK_TRUE(sizeof(SQLWCHAR) == 2 || sizeof(SQLWCHAR) == 4);

  const uint16_t empty[1] = {0};
  CheckUtf16(empty, 0, "");

  const uint16_t ascii[] = {'h', 'i', '!'};
  CheckUtf16(ascii, 3, "hi!");

  // 2-byte and 3-byte sequences: U+00E9 e-acute, U+20AC euro sign.
  const uint16_t bmp[] = {0x00E9, 0x20AC};
  CheckUtf16(bmp, 2, "\xc3\xa9\xe2\x82\xac");

  // Non-BMP: U+1F600 GRINNING FACE, encoded as the surrogate pair D83D DE00.
  const uint16_t emoji[] = {0xD83D, 0xDE00};
  CheckUtf16(emoji, 2, "\xf0\x9f\x98\x80");

  // Non-BMP surrounded by ASCII, and two consecutive astral characters.
  // U+1F1E6 U+1F1F8 (regional indicators) => "\xf0\x9f\x87\xa6\xf0\x9f\x87\xb8".
  const uint16_t mixed[] = {'a', 0xD83D, 0xDE00, 'b', 0xD83C, 0xDDE6, 0xD83C, 0xDDF8};
  CheckUtf16(mixed, 8, "a\xf0\x9f\x98\x80" "b\xf0\x9f\x87\xa6\xf0\x9f\x87\xb8");

  // U+10FFFF, the highest code point.
  const uint16_t max_cp[] = {0xDBFF, 0xDFFF};
  CheckUtf16(max_cp, 2, "\xf4\x8f\xbf\xbf");

  // Malformed input must still produce valid UTF-8: unpaired surrogates
  // become U+FFFD (EF BF BD) rather than an invalid 3-byte encoding.
  const uint16_t lone_high[] = {'x', 0xD83D};
  CheckUtf16(lone_high, 2, "x\xef\xbf\xbd");
  const uint16_t lone_low[] = {0xDE00, 'y'};
  CheckUtf16(lone_low, 2, "\xef\xbf\xbd" "y");
  const uint16_t high_then_bmp[] = {0xD83D, 'z'};
  CheckUtf16(high_then_bmp, 2, "\xef\xbf\xbd" "z");
}

// NaN and the infinities are legal Arrow float values and must survive the
// append path bit-for-bit rather than becoming nulls or errors.
static void TestFloatSpecialValues(void) {
  const double values[] = {NAN, INFINITY, -INFINITY, 0.0, -0.0};
  const size_t n = sizeof(values) / sizeof(values[0]);

  struct ArrowArray f64;
  struct ArrowArray f32;
  struct ArrowError na_error;
  CHECK_I64(ArrowArrayInitFromType(&f64, NANOARROW_TYPE_DOUBLE), NANOARROW_OK);
  CHECK_I64(ArrowArrayInitFromType(&f32, NANOARROW_TYPE_FLOAT), NANOARROW_OK);
  CHECK_I64(ArrowArrayStartAppending(&f64), NANOARROW_OK);
  CHECK_I64(ArrowArrayStartAppending(&f32), NANOARROW_OK);
  for (size_t i = 0; i < n; i++) {
    CHECK_I64(ArrowArrayAppendDouble(&f64, values[i]), NANOARROW_OK);
    CHECK_I64(ArrowArrayAppendDouble(&f32, values[i]), NANOARROW_OK);
  }
  CHECK_I64(ArrowArrayFinishBuilding(&f64, NANOARROW_VALIDATION_LEVEL_FULL, &na_error),
            NANOARROW_OK);
  CHECK_I64(ArrowArrayFinishBuilding(&f32, NANOARROW_VALIDATION_LEVEL_FULL, &na_error),
            NANOARROW_OK);

  const double* d = (const double*)f64.buffers[1];
  const float* f = (const float*)f32.buffers[1];
  CHECK_I64(f64.length, (int64_t)n);
  CHECK_I64(f64.null_count, 0);
  CHECK_TRUE(isnan(d[0]) && isnan(f[0]));
  CHECK_TRUE(isinf(d[1]) && d[1] > 0 && isinf(f[1]) && f[1] > 0);
  CHECK_TRUE(isinf(d[2]) && d[2] < 0 && isinf(f[2]) && f[2] < 0);
  CHECK_TRUE(d[3] == 0.0 && !signbit(d[3]));
  CHECK_TRUE(d[4] == 0.0 && signbit(d[4]));
  ArrowArrayRelease(&f64);
  ArrowArrayRelease(&f32);
}

int main(void) {
  TestUtf16();
  TestFloatSpecialValues();
  return TEST_MAIN_RESULT();
}
