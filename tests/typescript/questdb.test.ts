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

// The QuestDB QWP driver (questdb/) from TypeScript, through the same Node.js
// ADBC driver manager. Skips unless both are set:
//
//   ADBC_QUESTDB_DRIVER  path to libadbc_driver_questdb.so
//   QUESTDB_URI          ws://host:port of a QuestDB 10 server

import { test, before, after } from 'node:test'
import assert from 'node:assert/strict'
import { AdbcDatabase, IngestMode } from '@apache-arrow/adbc-driver-manager'
import type { AdbcConnection } from '@apache-arrow/adbc-driver-manager'
import { tableFromArrays } from 'apache-arrow'

const DRIVER = process.env.ADBC_QUESTDB_DRIVER
const URI = process.env.QUESTDB_URI
const skip = !DRIVER || !URI ? 'set ADBC_QUESTDB_DRIVER and QUESTDB_URI' : false

let db: AdbcDatabase
let conn: AdbcConnection

before(async () => {
  if (skip) return
  db = new AdbcDatabase({ driver: DRIVER!, databaseOptions: { uri: URI! } })
  conn = await db.connect()
})

after(async () => {
  await conn?.close()
  await db?.close()
})

test('a million rows stream into an Arrow Table over QWP', { skip }, async () => {
  const t = await conn.query(
    'SELECT x AS id, x * 1.5 AS price, cast(x * 1000000 as timestamp) AS ts FROM long_sequence(1000000)',
  )
  assert.equal(t.numRows, 1_000_000)
  assert.deepEqual(t.schema.fields.map((f) => f.name), ['id', 'price', 'ts'])
  assert.match(String(t.schema.fields[2].type), /Timestamp/)
})

test('bind parameters and row counts over QWP', { skip }, async () => {
  const t = await conn.query('SELECT $1::long * x AS v FROM long_sequence(3)', tableFromArrays({ p: new Int32Array([10]) }))
  assert.deepEqual(t.toArray().map((r) => Number(r.toJSON().v)), [10, 20, 30])
  await conn.execute('DROP TABLE IF EXISTS ts_qwp_rows')
  await conn.execute('CREATE TABLE ts_qwp_rows (id LONG, ts TIMESTAMP) TIMESTAMP(ts) PARTITION BY DAY WAL')
  const n = await conn.execute('INSERT INTO ts_qwp_rows SELECT x, timestamp_sequence(0, 1000000) FROM long_sequence(5)')
  assert.equal(n, 5)
  await conn.execute('DROP TABLE IF EXISTS ts_qwp_rows')
})

test('bulk ingest over QWP, applied before the count that follows', { skip }, async () => {
  await conn.execute('DROP TABLE IF EXISTS ts_qwp_ingest')
  const base = Date.UTC(2024, 1, 29, 8, 0, 0)
  const n = 10_000
  const data = tableFromArrays({
    sym: Array.from({ length: n }, (_, i) => `s${i % 7}`),
    qty: Float64Array.from({ length: n }, (_, i) => i),
    ts: Array.from({ length: n }, (_, i) => new Date(base + i * 1000)),
  })
  const rows = await conn.ingest('ts_qwp_ingest', data, { mode: IngestMode.Create })
  assert.equal(rows, n)
  const c = await conn.query('SELECT count() AS c, sum(qty) AS q FROM ts_qwp_ingest')
  const row = c.get(0)!.toJSON()
  assert.equal(Number(row.c), n)
  assert.equal(Number(row.q), (n * (n - 1)) / 2)
  await conn.execute('DROP TABLE IF EXISTS ts_qwp_ingest')
})
