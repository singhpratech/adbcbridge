// SPDX-License-Identifier: Apache-2.0
// Load the `mongodbbi` matrix entry's two collections into a running MongoDB.
//
//     docker exec -i adbcbridge-mongodbbi mongosh --quiet < tests/compat/fixtures/load_mongodbbi.js
//
// The MongoDB BI Connector (mongosqld) is a query engine only -- it has no DDL and no
// DML -- so the entry's data cannot be created over the ODBC connection the way every
// other entry's is; it is written into MongoDB itself here, and mongodbbi.drdl beside
// this file is the schema that maps these two collections to SQL tables.
//
//   adbc_t    the workload's two rows.  The BI Connector has no binary type at all
//             (bson.Binary cannot even be named in a DRDL schema), so `b` holds the two
//             bytes as text -- which is what the entry reads back.  A BSON date is
//             milliseconds, so `ts` carries 123 of the workload's 123456 microseconds,
//             and `d`, having no DATE type to land in, is a midnight timestamp.
//   adbc_big  100,000 documents of (a, b, c, d, e) -- what check_big() reads and what
//             bench/matrix_bench.py times a fetch of.
//
// Re-running this is idempotent: each collection is dropped first.
db = db.getSiblingDB('adbc');

db.adbc_t.drop();
db.adbc_t.insertMany([
  {i: 1, f: 1.5, s: "héllo 🚀", b: "\u0001\u0002",
   d: ISODate("2024-02-29T00:00:00Z"), ts: ISODate("2024-02-29T13:45:10.123Z"),
   n: NumberDecimal("12.345"), bo: true},
  {i: 2, f: null, s: null, b: null, d: null, ts: null, n: null, bo: null},
]);

db.adbc_big.drop();
var N = 100000, batch = [];
for (var i = 0; i < N; i++) {
  batch.push({a: i, b: "r" + i, c: i * 1.0, d: ISODate("2024-01-01T00:00:00Z"), e: i % 2 == 0});
  if (batch.length == 10000) { db.adbc_big.insertMany(batch, {ordered: false}); batch = []; }
}
if (batch.length) db.adbc_big.insertMany(batch, {ordered: false});

print("adbc_t=" + db.adbc_t.countDocuments({}) + " adbc_big=" + db.adbc_big.countDocuments({}));
