<!-- SPDX-License-Identifier: Apache-2.0 -->
# Connection keywords set for you

What `adbc.odbc.tune` does: the ODBC connection keywords adbcBridge adds for a driver it recognises, and the ones it deliberately leaves alone.

Some ODBC drivers have connection keywords whose good value depends on how the
application reads a result set — something the driver cannot know and you should
not have to. Where adbcBridge recognises the target driver it fills those in
while it assembles the connection string, under three rules: a keyword **you**
set (in the connection string or in the DSN) is never overridden, nothing that
changes what a query returns is ever set, and `adbc.odbc.tune=false` turns the
whole thing off.

The complete list in v0.1.0 is one keyword:

| driver | condition | what is added | why |
|---|---|---|---|
| psqlodbc (PostgreSQL and the thirteen other PostgreSQL-wire servers in the matrix) | you set `UseDeclareFetch=1` and no `Fetch` | `Fetch=8192` (`8 × adbc.odbc.batch_size`, clamped to 8192…65536) | `UseDeclareFetch=1` asks psqlodbc to stream the result set through a server-side cursor instead of buffering all of it client-side. Each `FETCH` then brings back `max(Fetch, rowset)` rows, so psqlodbc's default `Fetch=100` is inert — our rowset always wins it — and the cursor round-trips once per rowset. 1M rows of `(int4, float8, varchar(20), date)` at `batch_size` 1024: **0.72 s** at the default `Fetch` against **0.57 s** with this, which is exactly what the same read costs *not* streaming. Peak process RSS for that read is 158 MB streaming against 422 MB buffered |

psqlodbc's other keywords were swept and are deliberately **not** set:
`ByteaAsLongVarBinary`, `TextAsLongVarchar`, `MaxVarcharSize` and `UnknownSizes`
change the SQL types and widths the driver reports, and so the Arrow schema and
the DDL bulk ingest generates; `TrueIsMinus1` and `LFConversion` rewrite values;
`UseDeclareFetch` and `Protocol` are transaction semantics (a server-side cursor
and per-statement `SAVEPOINT`s, neither of which every PostgreSQL-wire server
behind psqlodbc has). Everything else measured flat, within ±4% of the default
on a 1M-row read.

If you do turn `UseDeclareFetch=1` on, note that it is genuinely a different
mode, not just a buffer size: the read becomes `O(Fetch)` in client memory
instead of `O(result set)`, it needs a server that implements `DECLARE … CURSOR
WITH HOLD` and `FETCH n IN …`, and rolling back a transaction with the cursor
still open leaves the statement handle needing a fresh cursor.
