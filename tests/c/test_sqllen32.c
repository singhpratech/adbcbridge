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
//
// Unit tests for the 32-bit-SQLLEN driver quirk accessors in odbc_internal.h.
//
// IBM Db2's clidriver libdb2.so is built with a 32-bit SQLLEN/SQLULEN on 64-bit
// Linux, so everything it writes through a SQLLEN* is four bytes wide: indicator
// arrays become int32[] with stride 4 and scalar out-parameters get only their low
// half.  These tests stand in for that driver by writing into the buffers exactly
// the way it does, with no database involved.

#include "odbc_internal.h"

#include "test_common.h"

// Write `value` the way a 32-bit-SQLLEN driver writes element `row` of an
// indicator array, i.e. int32 at byte offset row * 4.
static void DriverWriteInd32(void* base, size_t row, int32_t value) {
  memcpy((char*)base + row * sizeof(int32_t), &value, sizeof(value));
}

// Write `value` the way such a driver writes a scalar SQLLEN/SQLULEN
// out-parameter: four bytes at the variable's own address.
static void DriverWriteScalar32(void* p, uint32_t value) {
  memcpy(p, &value, sizeof(value));
}

static void TestScalarsQuirkOff(void) {
  SQLLEN v = -1;
  CHECK_I64(OdbcReadLen(&v, false), -1);
  v = 1234567890123LL;
  CHECK_I64(OdbcReadLen(&v, false), 1234567890123LL);

  SQLULEN u = 4000000000ULL;
  CHECK_I64((int64_t)OdbcReadULen(&u, false), 4000000000LL);
}

static void TestScalarsQuirkOn(void) {
  // The caller zeroes, the driver writes four bytes, we read the low half back.
  SQLLEN v = 0;
  DriverWriteScalar32(&v, (uint32_t)-1);  // SQL_NULL_DATA
  CHECK_I64(OdbcReadLen(&v, true), SQL_NULL_DATA);

  v = 0;
  DriverWriteScalar32(&v, (uint32_t)-4);  // SQL_NO_TOTAL
  CHECK_I64(OdbcReadLen(&v, true), SQL_NO_TOTAL);

  v = 0;
  DriverWriteScalar32(&v, 42);
  CHECK_I64(OdbcReadLen(&v, true), 42);

  // Garbage left in the high half must not leak into the result.
  v = (SQLLEN)0x1122334400000000LL;
  DriverWriteScalar32(&v, (uint32_t)-1);
  CHECK_I64(OdbcReadLen(&v, true), -1);

  SQLULEN u = 0;
  DriverWriteScalar32(&u, 20);  // e.g. SQLDescribeCol column size
  CHECK_I64((int64_t)OdbcReadULen(&u, true), 20);

  u = (SQLULEN)0xAAAAAAAA00000000ULL;
  DriverWriteScalar32(&u, 3);  // e.g. SQL_ATTR_ROWS_FETCHED_PTR
  CHECK_I64((int64_t)OdbcReadULen(&u, true), 3);
}

static void TestIndicatorArray(void) {
  enum { kRows = 5 };
  SQLLEN ind[kRows];

  // Quirk off: plain SQLLEN[] round-trip.
  for (size_t i = 0; i < kRows; i++) OdbcIndicatorSet(ind, i, (SQLLEN)(i * 100), false);
  OdbcIndicatorSet(ind, 2, SQL_NULL_DATA, false);
  for (size_t i = 0; i < kRows; i++) {
    CHECK_I64(OdbcIndicatorGet(ind, i, false),
              i == 2 ? (SQLLEN)SQL_NULL_DATA : (SQLLEN)(i * 100));
  }

  // Quirk on: our own writes must round-trip through our own reads...
  memset(ind, 0, sizeof(ind));
  for (size_t i = 0; i < kRows; i++) OdbcIndicatorSet(ind, i, (SQLLEN)(i * 100), true);
  OdbcIndicatorSet(ind, 3, SQL_NULL_DATA, true);
  for (size_t i = 0; i < kRows; i++) {
    CHECK_I64(OdbcIndicatorGet(ind, i, true),
              i == 3 ? (SQLLEN)SQL_NULL_DATA : (SQLLEN)(i * 100));
  }
  // ...and only the first kRows * 4 bytes of the allocation are touched, which is
  // why an 8-byte-per-row allocation is always large enough.
  const unsigned char* bytes = (const unsigned char*)ind;
  for (size_t b = kRows * sizeof(int32_t); b < sizeof(ind); b++) CHECK_I64(bytes[b], 0);

  // ...and a real 32-bit driver's writes are read back correctly: 'aaa', NULL, 'cccc'
  // is exactly what libdb2.so produces (0x03, 0xFFFFFFFF, 0x04 at stride 4).
  memset(ind, 0, sizeof(ind));
  DriverWriteInd32(ind, 0, 3);
  DriverWriteInd32(ind, 1, -1);
  DriverWriteInd32(ind, 2, 4);
  CHECK_I64(OdbcIndicatorGet(ind, 0, true), 3);
  CHECK_I64(OdbcIndicatorGet(ind, 1, true), SQL_NULL_DATA);
  CHECK_I64(OdbcIndicatorGet(ind, 2, true), 4);

  // Reading that same buffer without the quirk is what the bug looked like: the
  // NULL at row 1 disappears and row 1's length is nonsense.
  CHECK_TRUE(OdbcIndicatorGet(ind, 1, false) != SQL_NULL_DATA);
}

int main(void) {
  TestScalarsQuirkOff();
  TestScalarsQuirkOn();
  TestIndicatorArray();
  return TEST_MAIN_RESULT();
}
