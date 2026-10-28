<!-- SPDX-License-Identifier: Apache-2.0 -->
# QuestDB over QWP: the native driver

Since QuestDB 10.0 the preferred way to talk to the server is QWP, QuestDB's own
columnar wire protocol over WebSocket; the PostgreSQL wire is kept for compatibility.
adbcBridge ships a second driver for it, **`adbc_driver_questdb`**: an ADBC 1.1.0 driver
that speaks QWP directly through QuestDB's official C client
([questdb/c-questdb-client](https://github.com/questdb/c-questdb-client), Apache-2.0).
No ODBC, no PostgreSQL wire: query results are the client's own Arrow batches passed
straight through, and bulk ingest goes out as Arrow batches the same way.

Use it when the server is QuestDB 10.0 or newer. For QuestDB 9 and older, or when you
want one driver for every database, the ODBC bridge over psqlodbc still works
(`docs/COMPATIBILITY.md`, row *questdb*).

## Build

The driver is its own CMake project under `questdb/`, separate from the ODBC driver
because the client is written in Rust: building needs **Rust 1.91 or newer**
([rustup](https://rustup.rs/)), CMake 3.16+ and a C11 compiler. The client source is
fetched as a hash-pinned tarball and linked statically, so the result is one file.

```bash
cmake -S questdb -B build-questdb -DCMAKE_BUILD_TYPE=Release
cmake --build build-questdb
# -> build-questdb/libadbc_driver_questdb.so  (.dylib on macOS)
```

The first build compiles the client and arrow-rs (a few minutes); rebuilds take seconds.

## Connect

Point any ADBC driver manager at the library. The only required option is `uri`.

```python
import adbc_driver_manager.dbapi as dbapi

conn = dbapi.connect(
    driver="/path/to/libadbc_driver_questdb.so",
    entrypoint="AdbcDriverInit",
    db_kwargs={"uri": "ws://localhost:9000"},
)
with conn.cursor() as cur:
    cur.execute("SELECT symbol, avg(price) FROM trades WHERE ts > $1 GROUP BY symbol",
                parameters=[datetime.datetime(2026, 1, 1)])
    table = cur.fetch_arrow_table()
```

`ws://host:port` is QuestDB's HTTP port (9000 by default); `wss://` for TLS.
`http://` and `https://` are accepted as aliases.

### Database options

| Option | Meaning |
|---|---|
| `uri` | `ws://host:port` or `wss://host:port`. Required. |
| `username`, `password` | HTTP basic authentication. |
| `adbc.questdb.token` | Static bearer token instead of a password. |
| `adbc.questdb.conf` | Extra client connect-string keys, appended verbatim: TLS roots (`tls_ca=os_roots;` or `tls_roots=/path/ca.pem;`), pool sizes (`query_pool_max=8;`), timeouts. See the [client's configuration reference](https://questdb.com/docs/connect/clients/c-and-cpp/). |

The database object owns one connection pool; each `ExecuteQuery` borrows a reader from it
for the life of its result stream, so several statements can stream at once, up to
`query_pool_max` (4 by default).

### Statement options

| Option | Default | Meaning |
|---|---|---|
| `adbc.questdb.query_timeout_ms` | 0 (none) | Per-query timeout. Needs a server that advertises the capability; QuestDB 10.1.0 does not, and the client then refuses the query rather than run it without the timeout. |
| `adbc.ingest.target_table`, `adbc.ingest.mode` | | Standard ADBC bulk ingest; see below. |
| `adbc.questdb.ingest.timestamp_column` | auto | The data column that becomes the designated timestamp. |
| `adbc.questdb.ingest.symbol_mode` | `auto` | `auto`: dictionary-encoded string columns become `SYMBOL`, plain strings `VARCHAR`. `symbol`: every string column becomes `SYMBOL`. `varchar`: none does. |
| `adbc.questdb.ingest.partition_by` | `DAY` | Partitioning of a table the ingest creates: `NONE`, `HOUR`, `DAY`, `WEEK`, `MONTH`, `YEAR`. `NONE` creates an unpartitioned, non-WAL table. |
| `adbc.questdb.ingest.wait_for_apply` | `true` | After the server acknowledges the batches, wait until the table's WAL is applied, so a `SELECT` that follows sees the rows. |
| `adbc.questdb.ingest.apply_timeout_ms` | 30000 | Bound on that wait; on expiry the ingest returns `TIMEOUT` with the rows acknowledged and still being applied. |

## Bulk ingest

`adbc_ingest` (Python) or `AdbcStatementBind` with `adbc.ingest.target_table` sends each
Arrow batch through the client's Arrow sender. When the table does not exist (modes
`create`, `create_append`, `replace`) the driver first issues a `CREATE TABLE` built from
the Arrow schema, so column names and types are exactly yours:

| Arrow | QuestDB |
|---|---|
| bool | BOOLEAN |
| int8 / int16 / int32 / int64 | BYTE / SHORT / INT / LONG |
| uint8 / uint16 / uint32 / uint64 | SHORT / INT / LONG / LONG (values above 2⁶³−1 are rejected) |
| float16, float32 / float64 | FLOAT / DOUBLE |
| utf8, large_utf8, utf8_view | VARCHAR (SYMBOL with `symbol_mode=symbol`) |
| dictionary<…, utf8> | SYMBOL (VARCHAR with `symbol_mode=varchar`) |
| binary, large_binary | BINARY |
| fixed_size_binary(16) | UUID |
| timestamp (s, ms, us) / timestamp (ns) | TIMESTAMP / TIMESTAMP_NS |
| date32, date64 | DATE |
| decimal128(p, s) | DECIMAL(p, s) |
| list<double> | DOUBLE[] |

The designated timestamp is, in order: the `timestamp_column` option; for an existing
table its own designated column when the data has it; for a new table the single
timestamp-typed column of the data. A new table with no timestamp column gets an extra
`timestamp TIMESTAMP` column that the server stamps with arrival time. Modes follow ADBC:
`create` fails if the table exists, `append` fails if it does not, `replace` drops and
recreates.

QuestDB acknowledges a frame before its rows are visible (WAL apply is asynchronous).
The driver therefore polls `wal_tables()` until `writerTxn` has caught up with
`sequencerTxn` before returning, unless `wait_for_apply=false`.

## Type mapping on read

Result schemas are the client's. GetTableSchema reports the same types, so the two agree:

| QuestDB | Arrow |
|---|---|
| BOOLEAN, BYTE, SHORT, INT, LONG | bool, int8, int16, int32, int64 |
| CHAR | uint16 (a UTF-16 code unit) |
| FLOAT, DOUBLE | float32, float64 |
| STRING, VARCHAR | utf8 |
| SYMBOL | dictionary<uint32, utf8> |
| DATE | timestamp[ms, UTC] |
| TIMESTAMP, TIMESTAMP_NS | timestamp[us, UTC], timestamp[ns, UTC] |
| UUID | fixed_size_binary(16), extension `arrow.uuid` |
| LONG256 | fixed_size_binary(32) |
| IPV4 | uint32 |
| GEOHASH | int64 (bits in field metadata `questdb.geohash_bits`) |
| DECIMAL(p ≤ 18, s) | decimal64(18, s) |
| DECIMAL(19–38, s) | decimal128(38, s) |
| DECIMAL(39–76, s) | decimal256(76, s) |
| DOUBLE[], DOUBLE[][] | list<double>, list<list<double>> |
| BINARY | binary |

Every field carries `questdb.column_type` metadata with the server's type name.
QuestDB's BYTE, SHORT, CHAR and BOOLEAN have no NULL; they read back as 0 / false.

## Catalog

QuestDB has one namespace; the driver reports it as catalog `qdb`, schema `public`.
`GetObjects` lists tables and materialized views from `tables()` and their columns from
`table_columns()`, with the designated timestamp and dedup keys noted in `remarks` and the
QuestDB type in `xdbc_type_name`. There are no constraints to report.

## What it does not do

- **Transactions.** QuestDB has none over QWP; switching autocommit off returns
  `NOT_IMPLEMENTED`, as the ADBC specification allows.
- **Cancel mid-batch.** `AdbcStatementCancel` takes effect between batches (the client's
  handles are single-threaded); a blocked fetch finishes its current batch first.
- **Parameter schemas.** QuestDB does not describe parameters; bind with typed Arrow
  arrays, or cast in SQL (`$1::int`).
- **Small decimals on read.** c-questdb-client 7.2.0's Arrow egress rejects `DECIMAL`
  columns with precision 9 or less (`QWP egress: unsupported column type DECIMAL(8,2)`);
  recorded in [UPSTREAM.md](../UPSTREAM.md).
- **Enterprise-only features** (durable acknowledgements, multi-endpoint failover, OIDC)
  are reachable through `adbc.questdb.conf` but not tested here.

## Testing

`questdb/tests/test_qwp.py` runs the driver through the ADBC driver manager against a
live server (connect, every column type, bind parameters, `executemany`, bulk ingest in
all four modes, catalog, errors, timeouts); `questdb/tests/c/test_qwp_driver.c` is the C
smoke test. Both are wired into CI (`questdb` job) against the `questdb/questdb` image.

```bash
docker run -d -p 9000:9000 questdb/questdb:10.1.0
QUESTDB_URI=ws://127.0.0.1:9000 ./build-questdb/adbc_questdb_test_driver build-questdb/libadbc_driver_questdb.so
QUESTDB_URI=ws://127.0.0.1:9000 ADBC_QUESTDB_DRIVER=$PWD/build-questdb/libadbc_driver_questdb.so \
  python -m pytest questdb/tests/test_qwp.py
```
