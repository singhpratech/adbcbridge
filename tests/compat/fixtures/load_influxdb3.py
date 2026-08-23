# SPDX-License-Identifier: Apache-2.0
"""Load the `influxdb3` matrix entry's two tables into a running InfluxDB 3 Core.

    python3 tests/compat/fixtures/load_influxdb3.py [http://127.0.0.1:18181] [adbc]

InfluxDB 3's SQL is query-only -- it answers any DDL with "DDL not supported" -- and
tables come into existence when line protocol is written to them, so the entry's data
cannot be created over the ODBC connection the way every other entry's is.  This writes
it over the HTTP write API instead (stdlib only, no client library):

  adbc_t    the workload's two rows.  InfluxDB has no DATE, DECIMAL or binary type, so
            `d`, `n` and `b` are string fields, which the entry's SELECT casts (`d`) or
            reads as text (`n`, `b`); `ts` is the table's own `time` column, which the
            entry's SELECT aliases.  Row 2 carries only `i`: a field that is not written
            for a point simply is not there, which is how the all-NULL row is spelled.
  adbc_big  100,000 points of (a, b) -- what check_big() reads and what
            bench/matrix_bench.py times a fetch of -- one nanosecond apart, so they all
            land in one 1-hour date_bin bucket.

Writing the same points again is harmless: a point replaces the one with the same table,
tag set and timestamp, so re-running this is idempotent.
"""
import sys
import urllib.request

URL = (sys.argv[1] if len(sys.argv) > 1 else "http://127.0.0.1:18181").rstrip("/")
DB = sys.argv[2] if len(sys.argv) > 2 else "adbc"

# 2024-02-29T13:45:10.123456Z, the timestamp the whole matrix uses, in nanoseconds.
TS = 1709214310123456000
BIG_TS = 1709214310000000000
BIG_ROWS = 100000

lines = [
    # b holds the six characters \x0102 -- the bytes the other entries store, spelled the
    # way a server with no binary type has to (see the entry's `binary_text`).  In line
    # protocol a backslash inside a string field is escaped, hence \\x0102 on the wire.
    'adbc_t i=1i,f=1.5,s="héllo \U0001F680",b="\\\\x0102",d="2024-02-29",'
    'n="12.345",bo=true %d' % TS,
    'adbc_t i=2i %d' % (TS + 1),
]
lines += ['adbc_big a=%di,b="r%d" %d' % (i, i, BIG_TS + i) for i in range(BIG_ROWS)]

body = ("\n".join(lines) + "\n").encode("utf-8")
req = urllib.request.Request(
    "%s/api/v3/write_lp?db=%s&precision=nanosecond" % (URL, DB), data=body, method="POST")
with urllib.request.urlopen(req) as resp:
    print("wrote %d points to %s (HTTP %d)" % (len(lines), DB, resp.status))
