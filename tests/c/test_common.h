// SPDX-License-Identifier: Apache-2.0
// Minimal assertion helpers shared by the adbcbridge C unit tests.
#pragma once

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int adbc_test_failures = 0;

#define CHECK_TRUE(COND)                                                     \
  do {                                                                       \
    if (!(COND)) {                                                           \
      fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #COND);        \
      adbc_test_failures++;                                                  \
    }                                                                        \
  } while (0)

#define CHECK_I64(ACTUAL, EXPECTED)                                          \
  do {                                                                       \
    long long actual_ = (long long)(ACTUAL);                                 \
    long long expected_ = (long long)(EXPECTED);                             \
    if (actual_ != expected_) {                                              \
      fprintf(stderr, "FAIL %s:%d: %s = %lld, expected %lld\n", __FILE__,    \
              __LINE__, #ACTUAL, actual_, expected_);                        \
      adbc_test_failures++;                                                  \
    }                                                                        \
  } while (0)

#define CHECK_STR(ACTUAL, ACTUAL_LEN, EXPECTED)                              \
  do {                                                                       \
    size_t alen_ = (size_t)(ACTUAL_LEN);                                     \
    const char* astr_ = (const char*)(ACTUAL);                               \
    const char* estr_ = (EXPECTED);                                          \
    if (alen_ != strlen(estr_) || memcmp(astr_, estr_, alen_) != 0) {        \
      fprintf(stderr, "FAIL %s:%d: got '%.*s' (%zu bytes), expected '%s'\n", \
              __FILE__, __LINE__, (int)alen_, astr_, alen_, estr_);          \
      adbc_test_failures++;                                                  \
    }                                                                        \
  } while (0)

#define TEST_MAIN_RESULT()                                                   \
  (adbc_test_failures == 0                                                   \
       ? (printf("%s: OK\n", __FILE__), 0)                                   \
       : (fprintf(stderr, "%s: %d failure(s)\n", __FILE__,                   \
                  adbc_test_failures),                                       \
          1))
