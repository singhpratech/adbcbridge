<!-- SPDX-License-Identifier: Apache-2.0 -->
# Partitioned reads (`ExecutePartitions`)

One connection reading a large table is one CPU decoding it. ADBC's partition contract
exists for exactly that: `AdbcStatementExecutePartitions` hands back N opaque
descriptors, and `AdbcConnectionReadPartition` turns any one of them back into a stream
— on any connection, in any order, in any process. adbcBridge implements both, so N
connections can read one query in parallel and the pieces concatenate.

```python
import concurrent.futures, pyarrow
import adbc_driver_manager.dbapi as dbapi

def open_conn():
    return dbapi.connect(driver="libadbc_driver_odbc.so", db_kwargs={
        "adbc.odbc.connection_string": "Driver=PostgreSQL Unicode;Server=...;"})

with open_conn() as conn, conn.cursor() as cur:
    cur.adbc_statement.set_options(**{"adbc.odbc.partitions": "8"})
    cur.adbc_statement.set_sql_query("SELECT id, val, txt, dt FROM bench")
    descriptors, schema, _ = cur.adbc_statement.execute_partitions()

def read(descriptor):                      # one connection per partition
    with open_conn() as conn:
        stream = conn.adbc_connection.read_partition(descriptor)
        return pyarrow.RecordBatchReader._import_from_c(stream.address).read_all()

with concurrent.futures.ThreadPoolExecutor(len(descriptors)) as pool:
    table = pyarrow.concat_tables(list(pool.map(read, descriptors)))
```

`bench/partition_bench.py` is that client, benchmarked against the native
`adbc_driver_postgresql`; the numbers are in
[`bench/BENCHMARKS.md`](../../bench/BENCHMARKS.md).

**How the split is made.** Every strategy produces the same shape: an ordered SQL
expression that is never NULL for a row of the table, and N-1 boundary values cutting it
into N half-open intervals, with the first slice unbounded below and the last unbounded
above. Half-open intervals put a row that lands exactly on a boundary in one slice and
only one, so repeated values are neither doubled nor dropped; the unbounded ends make
the union of the slices the whole domain of the expression, so boundaries computed from
stale metadata cost balance but never rows.

Three strategies, tried in that order:

| # | expression | applies when | boundaries from |
|---|---|---|---|
| 1 | `ctid` | PostgreSQL, and the relation has a heap | `pg_relation_size` (block count) |
| 2 | the leading primary-key column | there is a single-table primary key whose leading column is an integer, `NOT NULL`, and ordered | one `SELECT MIN(k), MAX(k)` |
| 3 | `yb_hash_code(k)` | YugabyteDB, where the primary key is hash-partitioned | fixed: the hash is a `uint16` |

**1 — the PostgreSQL heap.** Every heap tuple has a `ctid` of `(block, offset)` and `tid`
compares lexicographically, so the table's blocks are a total order that slices the heap
with no index, no key column and no sort:

```sql
SELECT … FROM t WHERE ctid >= '(lo,0)'::tid AND ctid < '(hi,0)'::tid
```

Tuple offsets are 1-based, so `'(N,0)'` is a point strictly *between* blocks: no tuple
can land on both sides of a boundary or on neither. PostgreSQL 14+ executes each slice as
a TID Range Scan, so a slice reads only its own blocks rather than filtering a full
sequential scan. This is the best split where it exists — no index needed, balanced in
bytes rather than key values, and read sequentially — so it stays first.

**2 — the key range.** `ctid` is a heap detail, and several PostgreSQL-wire servers do
not store tables in a heap: CockroachDB answers `SELECT ctid` with `42703`, YugabyteDB
with `0A000`. What they do have is a primary key, which is indexed, ordered and
`NOT NULL` — the three things the split shape needs:

```sql
SELECT … FROM t WHERE "id" >= 125001 AND "id" < 250001
```

The gate is built out of ODBC's own catalog calls — `SQLPrimaryKeys` for the leading key
column, `SQLColumns` for its type and nullability — so it is not tied to any one server's
system tables. It also reaches tables PostgreSQL itself could not split before: a
declaratively partitioned parent has no heap of its own, and now gets N partitions where
it used to get one.

The primary key is required, not merely preferred: a range predicate on an *unindexed*
column makes every slice scan the whole table, so N slices would be N times the server
work. That is also why there is no modulo-or-hash split (`WHERE mod(hash(k), N) = i`),
which balances perfectly and is never index-usable, and no `LIMIT`/`OFFSET` split, where
the server must produce and discard `OFFSET` rows per slice.

**3 — YugabyteDB's tablet hash.** YugabyteDB's default `PRIMARY KEY (id)` is
*hash*-partitioned, and against a hash-partitioned key `WHERE id >= a AND id < b` is not
an index condition at all — it is a `Storage Filter` over a `Seq Scan`. A key-range split
there would be correct and N times slower than not splitting. So the ordered expression
becomes the tablet hash itself, over the fixed range `[0, 65536)`:

```sql
SELECT … FROM t WHERE yb_hash_code("id") >= 8192 AND yb_hash_code("id") < 16384
```

On any PostgreSQL-wire server the driver reads `pg_index.indoption` — where YugabyteDB
records hash partitioning as bit `0x4` — before it will emit a key-range predicate, and
declines to split at all if it cannot read it.

**When it falls back to one partition.** A wrong split is silent data loss rather than an
error, so the driver splits only what it can prove, and returns a single descriptor
carrying the original query — always correct, never slower than not calling
`ExecutePartitions` — for everything else:

| case | why |
|---|---|
| anything but a bare `SELECT <list> FROM <one table>` | a `WHERE`, `JOIN`, `ORDER BY`, `GROUP BY`, `LIMIT`, `DISTINCT`, `UNION`, subquery, CTE, aggregate or any parenthesis at all |
| any quoting or comment in the SQL | the scanner does not track quotes, so it refuses rather than mistake a keyword inside a literal for a keyword |
| a statement with bound parameters | a descriptor carries SQL text; there is nowhere to put them |
| no heap *and* no usable primary key | a view or foreign table; a table with no primary key, or one whose leading key column is text, numeric, a date, or nullable |
| a unique index that is not the primary key | choosing between several is not a choice the catalog makes for us, so the driver does not make it either |
| a PostgreSQL-wire server that will not say how its key is ordered | the difference between an index condition and N full scans is not something to guess |
| an empty table, or a key with one distinct value | nothing to slice |
| `adbc.odbc.partitions=1` | the explicit off switch |

Nothing here ever *guesses* a partition column: each strategy either proves its
expression is total and `NOT NULL` over the table from the catalog, or hands back one
partition.

**Snapshot semantics.** Partitions are read on separate connections and therefore under
separate snapshots. Against a table being written concurrently, the union of the slices
is not a point-in-time view — that is inherent to ADBC's partition model, which has
nowhere to carry a shared snapshot, and is why partitioning is opt-in. Against a table
that is not being written, the slices reproduce the unpartitioned read exactly: same
rows, same schema, no duplicates, no gaps, whatever order they are read in.

When the connection is [delegated to a native ADBC driver](delegation.md), both
entry points forward to that driver and its own partitioning (or its own refusal)
applies.
