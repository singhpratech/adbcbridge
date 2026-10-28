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

// adbcBridge from TypeScript: the library is loaded by the Node.js ADBC driver
// manager (@apache-arrow/adbc-driver-manager) exactly as any other ADBC driver,
// and the ODBC connection string travels as a database option.
//
//   ADBC_ODBC_DRIVER     path to libadbc_driver_odbc.so / .dylib / .dll
//   SQLITE_ODBC_DRIVER   path to the SQLite ODBC driver (libsqlite3odbc.so)
//
// Runs with Node 22.18+ directly (`node --test`): no compile step, the types
// are stripped at load time.

import { test, before, after } from 'node:test'
import assert from 'node:assert/strict'
import { AdbcDatabase, AdbcError, IngestMode } from '@apache-arrow/adbc-driver-manager'
import type { AdbcConnection } from '@apache-arrow/adbc-driver-manager'
import { tableFromArrays, Table } from 'apache-arrow'

const DRIVER = process.env.ADBC_ODBC_DRIVER
const SQLITE = process.env.SQLITE_ODBC_DRIVER
const skip = !DRIVER || !SQLITE ? 'set ADBC_ODBC_DRIVER and SQLITE_ODBC_DRIVER' : false

let db: AdbcDatabase
let conn: AdbcConnection

before(async () => {
  if (skip) return
  db = new AdbcDatabase({
    driver: DRIVER!,
    databaseOptions: {
      'adbc.odbc.connection_string': `Driver=${SQLITE};Database=:memory:`,
      'adbc.odbc.delegate': 'never',
    },
  })
  conn = await db.connect()
})

after(async () => {
  await conn?.close()
  await db?.close()
})

test('a query comes back as an Arrow Table with typed columns', { skip }, async () => {
  const t = await conn.query("SELECT 1 AS n, 2.5 AS d, 'text' AS s, NULL AS missing")
  assert.equal(t.numRows, 1)
  assert.deepEqual(t.schema.fields.map((f) => f.name), ['n', 'd', 's', 'missing'])
  const row = t.get(0)!.toJSON()
  assert.equal(Number(row.n), 1)
  assert.equal(row.d, 2.5)
  assert.equal(row.s, 'text')
  assert.equal(row.missing, null)
})

test('GetInfo names the driver and the vendor', { skip }, async () => {
  const info = await conn.getInfo()
  // info_value is a dense union; read it through the column, not row.toJSON().
  const codes = info.getChild('info_name')!
  const values = info.getChild('info_value')!
  const byCode = new Map<number, unknown>()
  for (let i = 0; i < info.numRows; i++) byCode.set(Number(codes.get(i)), values.get(i))
  assert.match(String(byCode.get(0)), /SQLite/)        // ADBC_INFO_VENDOR_NAME
  assert.match(String(byCode.get(100)), /ODBC/)        // ADBC_INFO_DRIVER_NAME
  assert.equal(Number(byCode.get(103)), 1001000)       // ADBC_INFO_DRIVER_ADBC_VERSION = 1.1.0
})

test('execute returns the affected-row count, and the table is then visible', { skip }, async () => {
  await conn.execute('CREATE TABLE ts_rows (id INTEGER PRIMARY KEY, name TEXT)')
  const n = await conn.execute("INSERT INTO ts_rows (id, name) VALUES (1, 'a'), (2, 'b'), (3, 'c')")
  assert.equal(n, 3)
  const t = await conn.query('SELECT count(*) AS c FROM ts_rows')
  assert.equal(Number(t.get(0)!.toJSON().c), 3)
})

test('parameters bind from an Arrow Table, one execution per row', { skip }, async () => {
  const params = tableFromArrays({ id: new Int32Array([2, 3]) })
  const t = await conn.query('SELECT name FROM ts_rows WHERE id = ? ORDER BY name', params)
  // The driver runs the statement once per bound row and concatenates the results.
  assert.deepEqual(t.toArray().map((r) => r.toJSON().name), ['b', 'c'])
})

test('bulk ingest creates the table from the Arrow schema and reads back', { skip }, async () => {
  const data = tableFromArrays({
    id: new Int32Array([1, 2, 3, 4]),
    price: new Float64Array([1.5, 2.5, 3.5, 4.5]),
    label: ['w', 'x', 'y', 'z'],
  })
  const n = await conn.ingest('ts_ingest', data, { mode: IngestMode.Create })
  assert.equal(n, 4)
  const t = await conn.query('SELECT id, price, label FROM ts_ingest ORDER BY id')
  assert.equal(t.numRows, 4)
  assert.deepEqual(t.toArray().map((r) => r.toJSON().label), ['w', 'x', 'y', 'z'])
  assert.equal(t.toArray().map((r) => Number(r.toJSON().price)).reduce((a, b) => a + b, 0), 12)

  const more = await conn.ingest('ts_ingest', data, { mode: IngestMode.Append })
  assert.equal(more, 4)
  const c = await conn.query('SELECT count(*) AS c FROM ts_ingest')
  assert.equal(Number(c.get(0)!.toJSON().c), 8)

  await assert.rejects(conn.ingest('ts_ingest', data, { mode: IngestMode.Create }), (e: unknown) => {
    assert.ok(e instanceof AdbcError)
    return true
  })
})

test('streaming reads deliver record batches', { skip }, async () => {
  const reader = await conn.queryStream('SELECT id FROM ts_ingest')
  let rows = 0
  let batches = 0
  for await (const batch of reader) {
    rows += batch.numRows
    batches += 1
  }
  assert.equal(rows, 8)
  assert.ok(batches >= 1)
})

test('catalog: table types, objects and a table schema', { skip }, async () => {
  const types = await conn.getTableTypes()
  const names = types.toArray().map((r) => r.toJSON().table_type)
  assert.ok(names.includes('TABLE'), `table types: ${names}`)

  const schema = await conn.getTableSchema({ tableName: 'ts_ingest' })
  assert.deepEqual(schema.fields.map((f) => f.name), ['id', 'price', 'label'])

  const objects = await conn.getObjects({ tableName: 'ts_%' })
  assert.equal(objects.numRows, 1)
  const catalog = objects.get(0)!.toJSON() as { catalog_db_schemas: unknown }
  const text = JSON.stringify(catalog, (_k, v) => (typeof v === 'bigint' ? Number(v) : v))
  assert.match(text, /ts_ingest/)
  assert.match(text, /ts_rows/)
})

test('an SQL error is an AdbcError with the ODBC SQLSTATE', { skip }, async () => {
  await assert.rejects(conn.query('SELECT * FROM no_such_table_ts'), (e: unknown) => {
    assert.ok(e instanceof AdbcError)
    assert.match(e.message, /no_such_table_ts/)
    assert.ok(e.sqlState, 'sqlState is set')
    return true
  })
})

test('a statement handle: prepare once, bind, execute', { skip }, async () => {
  const stmt = await conn.createStatement()
  try {
    await stmt.setSqlQuery('INSERT INTO ts_rows (id, name) VALUES (?, ?)')
    await stmt.bind(tableFromArrays({ id: new Int32Array([10, 11]), name: ['ten', 'eleven'] }))
    const n = await stmt.executeUpdate()
    assert.equal(n, 2)
  } finally {
    await stmt.close()
  }
  const t: Table = await conn.query('SELECT name FROM ts_rows WHERE id >= 10 ORDER BY id')
  assert.deepEqual(t.toArray().map((r) => r.toJSON().name), ['ten', 'eleven'])
})
