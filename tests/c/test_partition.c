// SPDX-License-Identifier: Apache-2.0
//
// Unit tests for the two pure decisions behind partitioned results (odbc_partition.c):
// whether a query is the bare single-table SELECT that can be sliced at all, and how
// many slices a table of a given size gets.
//
// These are worth testing on their own because getting either wrong is *silent*: a
// query wrongly accepted for slicing does not fail, it returns the wrong rows.  Every
// case below that must not be split is therefore a case where the answer has to be "no"
// for a reason the scanner can actually see in the text.
//
// The partition translation unit is included so its internal helpers are visible; the
// handful of symbols it takes from the other translation units are stubbed below.

#include "odbc_partition.c"

#include "test_common.h"

// --- stubs for the symbols odbc_partition.c takes from the rest of the driver ----
AdbcStatusCode OdbcSetError(SQLSMALLINT handle_type, SQLHANDLE handle, const char* context,
                            struct AdbcError* error) {
  InternalAdbcSetError(error, "%s failed", context);
  return ADBC_STATUS_IO;
}
struct OdbcHandleRef* OdbcHandleRefNew(SQLHSTMT hstmt) { return NULL; }
void OdbcHandleRefRelease(struct OdbcHandleRef* ref) {}
AdbcStatusCode OdbcStatementEnsureHandle(struct OdbcStatement* stmt, struct AdbcError* error) {
  return ADBC_STATUS_INVALID_STATE;
}
AdbcStatusCode OdbcReaderInit(struct OdbcHandleRef* ref, const struct OdbcReaderOptions* opts,
                              struct ArrowArrayStream* out, struct AdbcError* error) {
  return ADBC_STATUS_NOT_IMPLEMENTED;
}
AdbcStatusCode OdbcDescribeResultSchema(SQLHSTMT hstmt, const struct OdbcReaderOptions* opts,
                                        struct ArrowSchema* out, struct AdbcError* error) {
  return ADBC_STATUS_NOT_IMPLEMENTED;
}

// --- helpers ----------------------------------------------------------------

// Parse `sql` and assert it was accepted, with the table reference spelled `table`.
static void CheckSplittable(const char* sql, const char* table) {
  size_t lo = 0, ll = 0, to = 0, tl = 0;
  if (!ParseSimpleSelect(sql, strlen(sql), &lo, &ll, &to, &tl)) {
    fprintf(stderr, "FAIL %s:%d: expected to split \"%s\"\n", __FILE__, __LINE__, sql);
    adbc_test_failures++;
    return;
  }
  CHECK_STR(sql + to, tl, table);
}

// Parse `sql` and assert it was refused, so that the caller gets one partition.
static void CheckNotSplittable(const char* sql) {
  size_t lo = 0, ll = 0, to = 0, tl = 0;
  if (ParseSimpleSelect(sql, strlen(sql), &lo, &ll, &to, &tl)) {
    fprintf(stderr, "FAIL %s:%d: expected NOT to split \"%s\"\n", __FILE__, __LINE__, sql);
    adbc_test_failures++;
  }
}

// --- the shapes that can be sliced ------------------------------------------

static void TestSplittable(void) {
  CheckSplittable("SELECT id, val, txt, dt FROM bench", "bench");
  CheckSplittable("select * from bench", "bench");
  CheckSplittable("SELECT * FROM public.bench", "public.bench");
  CheckSplittable("SELECT * FROM mydb.public.bench", "mydb.public.bench");
  // A trailing semicolon and surrounding whitespace are not part of the statement.
  CheckSplittable("  SELECT id FROM bench ;  ", "bench");
  CheckSplittable("SELECT\n  id,\n  val\nFROM\n  bench\n", "bench");
  // A bare table alias is harmless: with exactly one table in the FROM clause, the
  // unqualified `ctid` the predicate uses is unambiguous either way.
  CheckSplittable("SELECT id FROM bench b", "bench");
  // A column that merely *contains* a keyword is not that keyword.
  CheckSplittable("SELECT ordered, groups, wherewithal FROM bench", "bench");
  CheckSplittable("SELECT id FROM order_items", "order_items");
}

// --- the shapes that must not be, each for a reason the scanner can see -----

static void TestNotSplittable(void) {
  // A predicate the slice would have to preserve.
  CheckNotSplittable("SELECT id FROM bench WHERE id > 5");
  // Ordering: the ADBC contract says a sorted result set gets one partition anyway.
  CheckNotSplittable("SELECT id FROM bench ORDER BY id");
  // A row limit is not distributable at all: N slices of LIMIT 10 are 10*N rows.
  CheckNotSplittable("SELECT id FROM bench LIMIT 10");
  CheckNotSplittable("SELECT id FROM bench OFFSET 10");
  // Aggregates: slicing would return one partial aggregate per slice.
  CheckNotSplittable("SELECT count(*) FROM bench");
  CheckNotSplittable("SELECT max(id) FROM bench GROUP BY dt");
  // DISTINCT is an aggregate over the whole result set by another name.
  CheckNotSplittable("SELECT DISTINCT dt FROM bench");
  // More than one table: `ctid` would be ambiguous, and the join is not per-block.
  CheckNotSplittable("SELECT a.id FROM a JOIN b ON a.id = b.id");
  CheckNotSplittable("SELECT id FROM a, b");
  CheckNotSplittable("SELECT id FROM a CROSS JOIN b");
  // Set operations have two FROMs, and the ctid predicate would land on only one.
  CheckNotSplittable("SELECT id FROM a UNION SELECT id FROM b");
  CheckNotSplittable("SELECT id FROM a EXCEPT SELECT id FROM b");
  // Subqueries and CTEs: the FROM found by the scanner is not the outer one.
  CheckNotSplittable("SELECT id FROM (SELECT id FROM bench) t");
  CheckNotSplittable("WITH t AS (SELECT id FROM bench) SELECT id FROM t");
  // Anything with quoting: the scanner does not track quotes, so it must refuse rather
  // than mistake a keyword inside a literal for a keyword.
  CheckNotSplittable("SELECT 'from' FROM bench");
  CheckNotSplittable("SELECT \"id\" FROM bench");
  // Comments could hide any of the above.
  CheckNotSplittable("SELECT id FROM bench -- WHERE id > 5");
  CheckNotSplittable("SELECT id /* WHERE */ FROM bench");
  // Parameter markers have nowhere to be bound on the reading connection.
  CheckNotSplittable("SELECT id FROM bench WHERE id = ?");
  // Not a SELECT at all.
  CheckNotSplittable("INSERT INTO bench VALUES (1)");
  CheckNotSplittable("UPDATE bench SET id = 1");
  CheckNotSplittable("SELECT 1");           // no FROM
  CheckNotSplittable("SELECT FROM bench");  // no select list
  CheckNotSplittable("");
  // Two statements in one string.
  CheckNotSplittable("SELECT id FROM bench; SELECT id FROM other");
  // SELECT INTO writes a table rather than returning rows.
  CheckNotSplittable("SELECT id INTO copy FROM bench");
  // Locking clauses are per-row state, not something to spread over connections.
  CheckNotSplittable("SELECT id FROM bench FOR UPDATE");
}

// --- how many slices a table gets -------------------------------------------

static void TestPartitionCount(void) {
  // Automatic: one partition per ODBC_PARTITION_AUTO_BLOCKS blocks, capped.
  CHECK_I64(ResolvePartitionCount(0, 1), 1);
  CHECK_I64(ResolvePartitionCount(0, ODBC_PARTITION_AUTO_BLOCKS - 1), 1);
  CHECK_I64(ResolvePartitionCount(0, ODBC_PARTITION_AUTO_BLOCKS * 3), 3);
  CHECK_I64(ResolvePartitionCount(0, ODBC_PARTITION_AUTO_BLOCKS * 1000),
            ODBC_PARTITION_AUTO_MAX);

  // Explicit counts are honoured...
  CHECK_I64(ResolvePartitionCount(4, 100000), 4);
  CHECK_I64(ResolvePartitionCount(64, 100000), 64);
  // ... up to the hard ceiling ...
  CHECK_I64(ResolvePartitionCount(100000, 1000000), ODBC_PARTITION_MAX);
  // ... and never past one block per slice, which is the finest the ctid split goes.
  CHECK_I64(ResolvePartitionCount(8, 3), 3);
  CHECK_I64(ResolvePartitionCount(8, 1), 1);
  // A degenerate size still yields a usable count rather than zero or a negative.
  CHECK_I64(ResolvePartitionCount(0, 0), 1);
  CHECK_I64(ResolvePartitionCount(4, 0), 1);
  // A negative count means the same as 0: let the driver choose.  The option parser
  // never lets one through, so this is only about not misbehaving if one ever did.
  CHECK_I64(ResolvePartitionCount(-1, 100000), ODBC_PARTITION_AUTO_MAX);
}

// --- the ctid boundaries cover the heap exactly ------------------------------

// The property the whole design rests on: for any block count and any slice count, the
// half-open [lo, hi) ranges the loop in OdbcStatementExecutePartitionsOdbc builds are
// contiguous, non-overlapping, and span every block.  Recomputed here with the same
// arithmetic so a change to that expression has to change this too.
static void TestBoundariesCoverEveryBlock(void) {
  const int64_t sizes[] = {1, 2, 7, 8334, 83334, 1000003};
  const int64_t counts[] = {1, 2, 3, 4, 8, 16, 256};
  for (size_t si = 0; si < sizeof(sizes) / sizeof(sizes[0]); si++) {
    for (size_t ci = 0; ci < sizeof(counts) / sizeof(counts[0]); ci++) {
      const int64_t blocks = sizes[si];
      const int64_t n = ResolvePartitionCount(counts[ci], blocks);
      CHECK_TRUE(n >= 1 && n <= blocks);
      int64_t prev_hi = 0;
      for (int64_t k = 0; k < n; k++) {
        const int64_t lo = blocks * k / n;
        const int64_t hi = blocks * (k + 1) / n;
        CHECK_I64(lo, prev_hi);   // no gap, no overlap with the previous slice
        CHECK_TRUE(lo < hi);      // and every slice owns at least one block
        prev_hi = hi;
      }
      CHECK_I64(prev_hi, blocks);  // ... and together they reach the end of the heap
    }
  }
}

// --- descriptors round-trip through their wire format ------------------------

static void TestDescriptorRoundTrip(void) {
  struct AdbcPartitions parts = {0};
  struct AdbcError error = {0};
  const char* query = "SELECT id FROM bench";
  CHECK_I64(OnePartition(query, &parts, &error), ADBC_STATUS_OK);
  CHECK_I64(parts.num_partitions, 1);
  CHECK_TRUE(parts.release != NULL);
  // The descriptor is the magic followed by the query verbatim, which is what makes the
  // single-partition fallback exactly the original read.
  CHECK_I64(parts.partition_lengths[0], ODBC_PARTITION_MAGIC_LEN + strlen(query));
  CHECK_TRUE(memcmp(parts.partitions[0], ODBC_PARTITION_MAGIC, ODBC_PARTITION_MAGIC_LEN) == 0);
  CHECK_STR(parts.partitions[0] + ODBC_PARTITION_MAGIC_LEN,
            parts.partition_lengths[0] - ODBC_PARTITION_MAGIC_LEN, query);
  parts.release(&parts);
  CHECK_TRUE(parts.private_data == NULL);
  CHECK_TRUE(parts.release == NULL);
  if (error.release) error.release(&error);
}

int main(void) {
  TestSplittable();
  TestNotSplittable();
  TestPartitionCount();
  TestBoundariesCoverEveryBlock();
  TestDescriptorRoundTrip();
  if (adbc_test_failures) {
    fprintf(stderr, "%d failure(s)\n", adbc_test_failures);
    return 1;
  }
  printf("test_partition: OK\n");
  return 0;
}
