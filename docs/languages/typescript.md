<!-- SPDX-License-Identifier: Apache-2.0 -->
# TypeScript and JavaScript (Node.js)

adbcBridge is a plain-C ADBC (Arrow Database Connectivity) driver that reaches
any data source with an ODBC (Open Database Connectivity) driver. This page
covers using it from TypeScript or JavaScript on Node.js, through the Node.js
ADBC driver manager that the Apache Arrow project publishes as
[`@apache-arrow/adbc-driver-manager`](https://www.npmjs.com/package/@apache-arrow/adbc-driver-manager).

There is no adbcBridge npm package and none is needed: the driver manager loads
the shared library you built or installed, by path or by its manifest name, and
hands you Arrow Tables from the [`apache-arrow`](https://www.npmjs.com/package/apache-arrow)
package. adbcBridge adds no query API of its own, only the ODBC connection
string and a handful of `adbc.odbc.*` options. The same manager loads the
QuestDB QWP driver, `adbc_driver_questdb`, the same way (see the end of this page).

> **Status.** The Node.js driver manager is new (0.25.0, October 2026) and its
> authors call it alpha: the API may change. It needs **Node.js 22 or newer**,
> runs on Linux, macOS and Windows (x64 and arm64), and does not run in a
> browser, Deno, or (officially) Bun. adbcBridge's own TypeScript suite
> (`tests/typescript/`) runs in CI against the SQLite ODBC driver and the
> QuestDB QWP driver.

## Requirements

- Node.js 22.18 or newer. TypeScript files run directly (`node file.ts`); no
  compiler step is needed for the examples here.
- The adbcBridge library: `libadbc_driver_odbc.so` (`.dylib`, `.dll`), from a
  [release](https://github.com/singhpratech/adbcbridge/releases) or built from
  source ([Install on Linux](../getting-started/install-linux.md), macOS, Windows).
- An ODBC driver manager (unixODBC on Linux and macOS; built in on Windows)
  and the ODBC driver for your database, as on every other page.

```bash
npm install @apache-arrow/adbc-driver-manager apache-arrow
```

## Loading the driver

Pass either the library's path or its manifest name as `driver`; the ODBC
connection string goes in `databaseOptions`.

```typescript
import { AdbcDatabase } from '@apache-arrow/adbc-driver-manager'

const db = new AdbcDatabase({
  driver: '/usr/local/lib/libadbc_driver_odbc.so',
  databaseOptions: {
    'adbc.odbc.connection_string': 'Driver=PostgreSQL Unicode;Server=localhost;Database=app;Uid=app;Pwd=secret',
  },
})
const conn = await db.connect()
```

`install.sh` writes the ADBC driver manifest `odbc.toml` into the user's
manifest directory (`~/.config/adbc/drivers` on Linux), and the Node.js driver
manager searches the same directories as the Python and Go managers, so after
an install the short name works:

```typescript
const db = new AdbcDatabase({ driver: 'odbc', databaseOptions: { /* … */ } })
```

Set `ADBC_DRIVER_PATH` to add a directory to that search.

## Queries

`query` returns an `apache-arrow` `Table`; `queryStream` returns a
`RecordBatchReader` for results too large to hold at once.

```typescript
const table = await conn.query('SELECT id, price, placed_at FROM orders WHERE total > 100')
console.log(table.numRows, table.schema.fields.map((f) => `${f.name}: ${f.type}`))
for (const row of table) {
  console.log(row.toJSON())
}

const reader = await conn.queryStream('SELECT * FROM events')
for await (const batch of reader) {
  console.log(batch.numRows)
}
```

Integers that do not fit a JavaScript number arrive as `bigint`
(`Int64` columns), the way `apache-arrow` represents them; `Number(value)` converts
when the range allows.

## Parameters

Parameters are an Arrow `Table` with one column per placeholder, in order. The
driver runs the statement once per row of that table and concatenates the
results, so a two-row parameter table is two executions.

```typescript
import { tableFromArrays } from 'apache-arrow'

const params = tableFromArrays({ id: new Int32Array([42]) })
const one = await conn.query('SELECT name FROM customers WHERE id = ?', params)
```

Placeholders are whatever the ODBC driver accepts: `?` for most drivers, `$1`
for the QuestDB QWP driver.

## Statements: `execute` and bulk ingest

```typescript
const affected = await conn.execute("UPDATE orders SET status = 'shipped' WHERE id = 7")

import { IngestMode } from '@apache-arrow/adbc-driver-manager'
const rows = await conn.ingest('orders_2026', table, { mode: IngestMode.Create })
```

Ingest modes are the ADBC ones: `Create` (fails if the table exists), `Append`,
`CreateAppend`, `Replace`. The table is created from the Arrow schema with the
type mapping on [Type mapping](../reference/types.md), and the rows go through
adbcBridge's batched, parameter-array ingest path, with the per-driver fallbacks
recorded in [Driver quirks](../reference/quirks.md). For a stream of batches use
`ingestStream` with a `RecordBatchReader`.

For a prepared statement reused across calls, take a statement handle:

```typescript
const stmt = await conn.createStatement()
try {
  await stmt.setSqlQuery('INSERT INTO log (level, msg) VALUES (?, ?)')
  await stmt.bind(tableFromArrays({ level: ['info', 'warn'], msg: ['started', 'disk 90%'] }))
  const n = await stmt.executeUpdate()   // 2
} finally {
  await stmt.close()
}
```

## Metadata

```typescript
const info = await conn.getInfo()                      // vendor and driver name/version
const types = await conn.getTableTypes()               // TABLE, VIEW, …
const schema = await conn.getTableSchema({ tableName: 'orders' })
const objects = await conn.getObjects({ tableName: 'ord%' })   // catalogs → schemas → tables → columns
```

`getObjects` returns the nested ADBC objects table (`catalog_name`,
`catalog_db_schemas[].db_schema_tables[].table_columns[]`), the same shape every
ADBC binding sees.

## Transactions

Connections start in autocommit. `conn.setAutoCommit(false)` opens a
transaction that `commit()` or `rollback()` closes, on drivers that support
them (the QuestDB QWP driver does not; the call fails with `NOT_IMPLEMENTED`).

## Options

Every adbcBridge option on [Options](../reference/options.md) is set the same
way: database options in the `databaseOptions` map, connection options with
`conn.setOption(key, value)`, statement options with `stmt.setOption`. For
example, to keep a connection on the ODBC path even when a native ADBC driver
is installed for the same database:

```typescript
databaseOptions: { 'adbc.odbc.connection_string': '…', 'adbc.odbc.delegate': 'never' }
```

## Errors

Failures reject with an `AdbcError` carrying `message`, `code` (the ADBC status
name, such as `IO` or `InvalidArguments`), `vendorCode` and, for ODBC errors, the
five-character `sqlState` the driver reported.

```typescript
import { AdbcError } from '@apache-arrow/adbc-driver-manager'
try {
  await conn.query('SELECT * FROM missing')
} catch (e) {
  if (e instanceof AdbcError) console.error(e.code, e.sqlState, e.message)
}
```

## The QuestDB QWP driver from Node.js

The native QuestDB driver ([QuestDB over QWP](../getting-started/questdb-qwp.md))
loads the same way; its only required option is `uri`.

```typescript
const q = new AdbcDatabase({
  driver: '/path/to/libadbc_driver_questdb.so',
  databaseOptions: { uri: 'ws://localhost:9000' },
})
const qc = await q.connect()
const trades = await qc.query('SELECT symbol, avg(price) FROM trades WHERE ts > $1 GROUP BY symbol',
  tableFromArrays({ since: [new Date('2026-01-01T00:00:00Z')] }))
```

Measured on one machine against QuestDB 10.1.0: one million rows of three typed
columns into an `apache-arrow` Table in 83 ms end to end, including the
conversion from the driver's batches to JavaScript-side Arrow.

## The test suite

`tests/typescript/` holds the suite CI runs (`npm ci && npm test` with
`ADBC_ODBC_DRIVER` and `SQLITE_ODBC_DRIVER` set; the QuestDB file also wants
`ADBC_QUESTDB_DRIVER` and `QUESTDB_URI`): typed results, `GetInfo`, row counts,
parameters, ingest in create and append modes, streaming, catalog calls, errors
with SQLSTATE, and a prepared statement with a bound batch.
