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
struct OdbcHandleRef* OdbcHandleRefNew(SQLHSTMT hstmt) {
  return NULL;
}
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

// A split of `units` values wide, as the ctid strategy would describe a heap of that
// many blocks: extent [0, units] and one automatic partition per AUTO_BLOCKS blocks.
static struct OdbcSplit CtidSplit(int64_t blocks) {
  struct OdbcSplit split;
  memset(&split, 0, sizeof(split));
  split.kind = ODBC_SPLIT_CTID;
  snprintf(split.expr, sizeof(split.expr), "ctid");
  split.lo = 0;
  split.hi = blocks;
  split.auto_partitions = blocks / ODBC_PARTITION_AUTO_BLOCKS;
  return split;
}

// A key-range split over the inclusive key extent [lo, hi], with no server row
// estimate -- so the automatic count comes from the key span, as ChooseSplit does.
static struct OdbcSplit KeySplit(int64_t lo, int64_t hi) {
  struct OdbcSplit split;
  memset(&split, 0, sizeof(split));
  split.kind = ODBC_SPLIT_KEY_RANGE;
  snprintf(split.expr, sizeof(split.expr), "\"id\"");
  split.lo = lo;
  split.hi = hi;
  const uint64_t span = (uint64_t)hi - (uint64_t)lo;
  const uint64_t values = span == UINT64_MAX ? UINT64_MAX : span + 1;
  const uint64_t want = values / ODBC_PARTITION_AUTO_ROWS;
  split.auto_partitions =
      want > (uint64_t)ODBC_PARTITION_AUTO_MAX ? ODBC_PARTITION_AUTO_MAX : (int64_t)want;
  return split;
}

static int64_t CountForCtid(int64_t requested, int64_t blocks) {
  struct OdbcSplit split = CtidSplit(blocks);
  return ResolvePartitionCount(requested, &split);
}

static int64_t CountForKey(int64_t requested, int64_t lo, int64_t hi) {
  struct OdbcSplit split = KeySplit(lo, hi);
  return ResolvePartitionCount(requested, &split);
}

static void TestPartitionCount(void) {
  // Automatic: one partition per ODBC_PARTITION_AUTO_BLOCKS blocks, capped.
  CHECK_I64(CountForCtid(0, 1), 1);
  CHECK_I64(CountForCtid(0, ODBC_PARTITION_AUTO_BLOCKS - 1), 1);
  CHECK_I64(CountForCtid(0, ODBC_PARTITION_AUTO_BLOCKS * 3), 3);
  CHECK_I64(CountForCtid(0, ODBC_PARTITION_AUTO_BLOCKS * 1000), ODBC_PARTITION_AUTO_MAX);

  // Explicit counts are honoured...
  CHECK_I64(CountForCtid(4, 100000), 4);
  CHECK_I64(CountForCtid(64, 100000), 64);
  // ... up to the hard ceiling ...
  CHECK_I64(CountForCtid(100000, 1000000), ODBC_PARTITION_MAX);
  // ... and never past one block per slice, which is the finest the ctid split goes.
  CHECK_I64(CountForCtid(8, 3), 3);
  CHECK_I64(CountForCtid(8, 1), 1);
  // A degenerate size still yields a usable count rather than zero or a negative.
  CHECK_I64(CountForCtid(0, 0), 1);
  CHECK_I64(CountForCtid(4, 0), 1);
  // A negative count means the same as 0: let the driver choose.  The option parser
  // never lets one through, so this is only about not misbehaving if one ever did.
  CHECK_I64(CountForCtid(-1, 100000), ODBC_PARTITION_AUTO_MAX);

  // The key-range split counts in key values instead, but obeys the same rules.
  CHECK_I64(CountForKey(0, 1, 1000), 1);  // too narrow to bother
  CHECK_I64(CountForKey(0, 1, 3 * ODBC_PARTITION_AUTO_ROWS), 3);
  CHECK_I64(CountForKey(0, 1, 1000 * ODBC_PARTITION_AUTO_ROWS), ODBC_PARTITION_AUTO_MAX);
  CHECK_I64(CountForKey(8, 1, 1000000), 8);
  // min == max: one key value, nothing to cut, however many were asked for.
  CHECK_I64(CountForKey(8, 42, 42), 1);
  // Fewer distinct key values than partitions: one value per slice, no empty slices.
  CHECK_I64(CountForKey(8, 10, 13), 3);
  // Negative keys, and a span that covers the entire int64 range, must not overflow
  // into a nonsense count.
  CHECK_I64(CountForKey(8, -1000000, 1000000), 8);
  CHECK_I64(CountForKey(8, INT64_MIN, INT64_MAX), 8);
  CHECK_I64(CountForKey(0, INT64_MIN, INT64_MAX), ODBC_PARTITION_AUTO_MAX);
}

// --- the boundaries cover the whole domain exactly ---------------------------

// The property the whole design rests on, for every strategy: for any extent and any
// slice count, the half-open [boundary k, boundary k+1) ranges the loop in
// OdbcStatementExecutePartitionsOdbc builds are contiguous, non-overlapping, and span
// the extent -- with slice 0 unbounded below and slice n-1 unbounded above, so the
// union is the whole domain of the expression whatever the extent was.
static void CheckBoundariesCover(int64_t lo, int64_t hi, int64_t n) {
  CHECK_TRUE(n >= 1);
  if (n == 1) return;  // one slice carries the query verbatim
  int64_t prev = SplitBoundary(lo, hi, n, 1);
  // Slice 0 is (-inf, prev) and must contain at least the lowest key.
  CHECK_TRUE(prev > lo);
  for (int64_t k = 2; k <= n - 1; k++) {
    const int64_t b = SplitBoundary(lo, hi, n, k);
    CHECK_TRUE(b > prev);  // strictly increasing: no gap, no overlap, no empty slice
    prev = b;
  }
  // Slice n-1 is [prev, +inf) and must contain at least the highest key.
  CHECK_TRUE(prev <= hi);
}

static void TestBoundariesCoverEveryBlock(void) {
  const int64_t sizes[] = {1, 2, 7, 8334, 83334, 1000003};
  const int64_t counts[] = {1, 2, 3, 4, 8, 16, 256};
  for (size_t si = 0; si < sizeof(sizes) / sizeof(sizes[0]); si++) {
    for (size_t ci = 0; ci < sizeof(counts) / sizeof(counts[0]); ci++) {
      const int64_t blocks = sizes[si];
      const int64_t n = CountForCtid(counts[ci], blocks);
      CHECK_TRUE(n >= 1 && n <= blocks);
      CheckBoundariesCover(0, blocks, n);
      // The ctid boundaries must still be exactly the block cuts they always were.
      for (int64_t k = 1; k < n; k++) { CHECK_I64(SplitBoundary(0, blocks, n, k), blocks * k / n); }
    }
  }
}

// The same property over key extents, including negative keys, a single key value, and
// the widest extent an int64 key can have -- which is where naive `(hi - lo) * k / n`
// arithmetic would overflow and hand back boundaries that are not even monotonic.
static void TestKeyBoundariesCoverEveryValue(void) {
  const int64_t extents[][2] = {
      {1, 1},
      {0, 1},
      {1, 1000000},
      {-500, 500},
      {-1000000, -1},
      {INT64_MIN, 0},
      {0, INT64_MAX},
      {INT64_MIN, INT64_MAX},
      {INT64_MIN, INT64_MIN + 3},
      {INT64_MAX - 3, INT64_MAX},
  };
  const int64_t counts[] = {1, 2, 3, 4, 8, 16, 256};
  for (size_t ei = 0; ei < sizeof(extents) / sizeof(extents[0]); ei++) {
    for (size_t ci = 0; ci < sizeof(counts) / sizeof(counts[0]); ci++) {
      const int64_t lo = extents[ei][0], hi = extents[ei][1];
      const int64_t n = CountForKey(counts[ci], lo, hi);
      CheckBoundariesCover(lo, hi, n);
    }
  }
}

// The YugabyteDB tablet-hash split is the key-range machinery over a fixed domain, so
// it only has to be checked to stay inside the uint16 the hash actually produces.
static void TestYbHashBoundaries(void) {
  const int64_t counts[] = {2, 3, 4, 8, 16, 256};
  for (size_t ci = 0; ci < sizeof(counts) / sizeof(counts[0]); ci++) {
    const int64_t n = counts[ci];
    CheckBoundariesCover(0, 65535, n);
    for (int64_t k = 1; k < n; k++) {
      const int64_t b = SplitBoundary(0, 65535, n, k);
      CHECK_TRUE(b > 0 && b <= 65535);
    }
  }
}

// --- the table reference splits into ODBC catalog arguments ------------------

static void CheckTableRef(const char* ref, const char* cat, const char* sch, const char* tbl) {
  struct OdbcTableRef out;
  if (!SplitTableRef(ref, strlen(ref), &out)) {
    fprintf(stderr, "FAIL %s:%d: expected to split table ref \"%s\"\n", __FILE__, __LINE__, ref);
    adbc_test_failures++;
    return;
  }
  CHECK_STR(out.catalog, strlen(out.catalog), cat);
  CHECK_STR(out.schema, strlen(out.schema), sch);
  CHECK_STR(out.table, strlen(out.table), tbl);
}

static void CheckBadTableRef(const char* ref) {
  struct OdbcTableRef out;
  if (SplitTableRef(ref, strlen(ref), &out)) {
    fprintf(stderr, "FAIL %s:%d: expected NOT to split table ref \"%s\"\n", __FILE__, __LINE__,
            ref);
    adbc_test_failures++;
  }
}

static void TestSplitTableRef(void) {
  // One, two or three parts, right-aligned onto table / schema.table / cat.schema.table.
  CheckTableRef("bench", "", "", "bench");
  CheckTableRef("public.bench", "", "public", "bench");
  CheckTableRef("adbc.public.bench", "adbc", "public", "bench");
  // Degenerate references have no catalog arguments to make, so they are refused
  // rather than turned into a lookup that could match the wrong table.
  CheckBadTableRef("");
  CheckBadTableRef(".");
  CheckBadTableRef("a.");
  CheckBadTableRef(".a");
  CheckBadTableRef("a..b");
  CheckBadTableRef("a.b.c.d");
}

// --- only names that need no escaping are ever put into SQL ------------------

static void TestSafeIdentifier(void) {
  CHECK_TRUE(IsSafeIdentifier("id"));
  CHECK_TRUE(IsSafeIdentifier("order_id"));
  CHECK_TRUE(IsSafeIdentifier("Id2$"));
  CHECK_TRUE(IsSafeIdentifier("\xc3\xa9t\xc3\xa9"));  // UTF-8 is left alone
  // Everything that could close a quoted identifier or a string literal, or comment
  // the rest of the statement out, must be refused -- the SQL is built with snprintf
  // and escapes nothing, so this predicate is what makes that safe.
  CHECK_TRUE(!IsSafeIdentifier(""));
  CHECK_TRUE(!IsSafeIdentifier("a\"b"));
  CHECK_TRUE(!IsSafeIdentifier("a'b"));
  CHECK_TRUE(!IsSafeIdentifier("a`b"));
  CHECK_TRUE(!IsSafeIdentifier("a\\b"));
  CHECK_TRUE(!IsSafeIdentifier("a b"));
  CHECK_TRUE(!IsSafeIdentifier("a-b"));
  CHECK_TRUE(!IsSafeIdentifier("a;b"));
  CHECK_TRUE(!IsSafeIdentifier("a.b"));
  CHECK_TRUE(!IsSafeIdentifier("a\nb"));
  CHECK_TRUE(!IsSafeIdentifier("a)b"));
}

// --- boundary literals are spelled for the strategy that made them -----------

static void TestWriteBound(void) {
  struct OdbcSplit ctid = CtidSplit(1000);
  struct OdbcSplit key = KeySplit(0, 1000);
  char buf[64];
  CHECK_TRUE(WriteBound(&ctid, buf, sizeof(buf), 42));
  CHECK_STR(buf, strlen(buf), "'(42,0)'::tid");
  CHECK_TRUE(WriteBound(&key, buf, sizeof(buf), 42));
  CHECK_STR(buf, strlen(buf), "42");
  CHECK_TRUE(WriteBound(&key, buf, sizeof(buf), INT64_MIN));
  CHECK_STR(buf, strlen(buf), "-9223372036854775808");
  // A buffer that cannot hold the literal is a refusal, never a truncated predicate.
  CHECK_TRUE(!WriteBound(&key, buf, 4, INT64_MIN));
}

// --- identifiers are quoted the way the driver asks for ----------------------

static void TestQuotedIdent(void) {
  char buf[32];
  CHECK_TRUE(WriteQuotedIdent(buf, sizeof(buf), '"', "id"));
  CHECK_STR(buf, strlen(buf), "\"id\"");
  CHECK_TRUE(WriteQuotedIdent(buf, sizeof(buf), '`', "id"));
  CHECK_STR(buf, strlen(buf), "`id`");
  // A driver with no identifier quoting gets the bare name.
  CHECK_TRUE(WriteQuotedIdent(buf, sizeof(buf), '\0', "id"));
  CHECK_STR(buf, strlen(buf), "id");
  CHECK_TRUE(!WriteQuotedIdent(buf, 3, '"', "id"));
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
  TestKeyBoundariesCoverEveryValue();
  TestYbHashBoundaries();
  TestSplitTableRef();
  TestSafeIdentifier();
  TestWriteBound();
  TestQuotedIdent();
  TestDescriptorRoundTrip();
  if (adbc_test_failures) {
    fprintf(stderr, "%d failure(s)\n", adbc_test_failures);
    return 1;
  }
  printf("test_partition: OK\n");
  return 0;
}
