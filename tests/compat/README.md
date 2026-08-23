# Compatibility matrix

`test_matrix.py` runs one identical ADBC workload (types, NULLs, Unicode incl. emoji,
parameters, bulk ingest, batched reads, GetObjects, error mapping) against every ODBC
driver it can reach. Each database is enabled by an environment variable pointing at its
ODBC driver library; databases whose variable is unset are reported as `SKIP`.

```sh
docker compose -f tests/compat/docker-compose.yml up -d
ADBC_ODBC_DRIVER=$PWD/build/libadbc_driver_odbc.so python tests/compat/test_matrix.py [db ...]
```

The connection string of an entry can be overridden with `<NAME>_CONN`
(e.g. `MYSQL_CONN=...`).

## IBM Db2 12.1

Server (Db2 Community Edition):

```sh
docker run -d --name adbcbridge-db2 --privileged -p 127.0.0.1:50000:50000 \
  -e LICENSE=accept -e DB2INST1_PASSWORD=Adbc2026 -e DBNAME=adbc \
  icr.io/db2_community/db2
```

Driver — IBM's freely downloadable ODBC/CLI driver package (no account needed for the
"clidriver" tarball; unpack it anywhere):

```sh
mkdir -p /tmp/dbs/db2 && cd /tmp/dbs/db2
# linuxx64_odbc_cli.tar.gz from IBM's Db2 client packages download page
tar xzf linuxx64_odbc_cli.tar.gz
# -> clidriver/lib/libdb2.so
```

Run the entry (`clidriver/lib` must be on `LD_LIBRARY_PATH`; the driver loads siblings
such as `libdb2clixml4c.so` from there):

```sh
export DB2_ODBC_DRIVER=/tmp/dbs/db2/clidriver/lib/libdb2.so
LD_LIBRARY_PATH=/tmp/dbs/db2/clidriver/lib:$LD_LIBRARY_PATH \
ADBC_ODBC_DRIVER=$PWD/build/libadbc_driver_odbc.so \
python tests/compat/test_matrix.py db2
# db2       PASS  (DB2/LINUXX8664 12.01.0500)
```

Entry notes: Db2 folds unquoted identifiers to upper case (`ident=str.upper`) and supports
`BOOLEAN`, `VARBINARY(n)`, `TIMESTAMP(6)` and `DECIMAL(10,3)` directly, so no tolerance
flags are needed.

### Driver quirk: 32-bit `SQLLEN`

The `clidriver` package's `libdb2.so` is built with **32-bit `SQLLEN`/`SQLULEN` even on
64-bit Linux** (IBM's 64-bit-`SQLLEN` build is the separate `libdb2o.so`, shipped only in
the full server / data-server-driver packages). unixODBC, pyodbc and this driver all use an
8-byte `SQLLEN`, so every `SQLLEN`/`SQLULEN` the driver *writes* is half as wide as the
caller's storage:

- indicator/length arrays bound with `SQLBindCol` come back as `int32[]` with **stride 4**,
  so NULLs go undetected and string lengths are garbage past the first row (the classic
  symptom is row 2's string being row 1's text followed by NUL bytes, and NULL doubles
  reading as `0.0` or a stale value);
- scalar out-parameters — `SQLRowCount`, `SQLDescribeCol`'s column size, `SQLColAttribute`'s
  numeric attribute, `SQLGetData`'s `StrLen_or_Ind`, `SQL_ATTR_ROWS_FETCHED_PTR`,
  `SQL_ATTR_PARAMS_PROCESSED_PTR` — get only their low four bytes, so negative sentinels
  (`SQL_NULL_DATA` = -1, `SQL_NO_TOTAL` = -4) surface as `4294967295` / `4294967292`.

Reading a single column with `batch_size=1` mostly looks correct, which is why simple
probes pass and the damage only shows up with several columns or several rows per fetch.

adbcbridge detects this from `SQL_DRIVER_NAME` (Db2's CLI driver reports `libdb2.a`;
`libdb2o.a` is excluded) and sets the `sqllen_32bit` reader option, which makes every
driver-written `SQLLEN`/`SQLULEN` be read back through `OdbcReadLen()` / `OdbcReadULen()` /
`OdbcIndicatorGet()` in `src/odbc_internal.h` — as a sign-extended `int32`, and as
`int32[]` with stride 4 for indicator arrays. All such out-variables are zero-initialised
before the call. The fast path is unchanged when the quirk is off.

Force it either way with the `adbc.odbc.sqllen_32bit` option (`true`/`false`, settable on
the database, connection or statement) — useful for any other ODBC driver built with a
32-bit `SQLLEN`, or to confirm the quirk is what is fixing a result set:

```python
conn = dbapi.connect(driver=..., db_kwargs={"uri": ..., "adbc.odbc.sqllen_32bit": "false"})
```

## MySQL 8

Server (or use the `mysql` service in `docker-compose.yml`):

```sh
docker run -d --name adbcbridge-mysql -p 127.0.0.1:13307:3306 \
  -e MYSQL_ROOT_PASSWORD=adbc -e MYSQL_DATABASE=adbc \
  -e MYSQL_USER=adbc -e MYSQL_PASSWORD=adbc mysql:8
```

Driver — the official MySQL Connector/ODBC, from the Linux *generic* tarball (no root, no
login; just unpack it):

```sh
mkdir -p /tmp/dbs/mysql && cd /tmp/dbs/mysql
curl -sSLO https://dev.mysql.com/get/Downloads/Connector-ODBC/9.4/mysql-connector-odbc-9.4.0-linux-glibc2.28-x86-64bit.tar.gz
tar xzf mysql-connector-odbc-9.4.0-linux-glibc2.28-x86-64bit.tar.gz
# -> mysql-connector-odbc-9.4.0-linux-glibc2.28-x86-64bit/lib/libmyodbc9w.so  (Unicode)
#    mysql-connector-odbc-9.4.0-linux-glibc2.28-x86-64bit/lib/libmyodbc9a.so  (ANSI)
```

Run the entry:

```sh
export MYSQL_ODBC_DRIVER=/tmp/dbs/mysql/mysql-connector-odbc-9.4.0-linux-glibc2.28-x86-64bit/lib/libmyodbc9w.so
LD_PRELOAD=/lib/x86_64-linux-gnu/libstdc++.so.6 \
ADBC_ODBC_DRIVER=$PWD/build/libadbc_driver_odbc.so \
python tests/compat/test_matrix.py mysql
# mysql     PASS  (MySQL 8.4.11)
```

`LD_PRELOAD=…/libstdc++.so.6` is a host/glibc workaround, not a driver bug: `import
pyarrow` loads enough shared libraries to exhaust glibc's *surplus static TLS*, after
which `dlopen()` of the C++ Connector/ODBC fails with
`libstdc++.so.6: cannot allocate memory in static TLS block`, which unixODBC surfaces as
`Can't open lib '…/libmyodbc9w.so' : file not found`. Preloading `libstdc++` (so it is
part of the initial link map) fixes it; nothing else in the matrix needs it, and
non-pyarrow ODBC applications are unaffected.

Cross-check with the MariaDB Connector/ODBC against the same MySQL 8 server — it works,
but MySQL 8.4 accounts default to `caching_sha2_password`, whose client-side plugin the
connector loads from its compiled-in plugin directory, so an unpacked (non-root)
libmariadb3 needs `PLUGIN_DIR=`:

```sh
MYSQL_ODBC_DRIVER=/path/to/odbc/libmaodbc.so \
MYSQL_CONN='Driver={drv};Server=127.0.0.1;Port=13307;Database=adbc;User=adbc;Password=adbc;PLUGIN_DIR=/path/to/libmariadb3/plugin;' \
ADBC_ODBC_DRIVER=$PWD/build/libadbc_driver_odbc.so \
python tests/compat/test_matrix.py mysql
# mysql     PASS  (MySQL 08.04.000011)
```

Entry notes: MySQL needs `ANSI_QUOTES` in `sql_mode` for the double-quoted identifiers
that `adbc_ingest` emits (set by the entry's `setup`), and `BOOLEAN` is `TINYINT(1)`, which
the drivers report as `SQL_TINYINT`, so the entry expects `int8` for the `bo` column. No
other tolerance flags are needed and no driver quirk is required in `src/`: both drivers
pass every assertion, including the emoji round-trip, microsecond timestamps,
`DECIMAL(10,3)`, NULL parameters, affected-row counts and the 5000-row batched read.
