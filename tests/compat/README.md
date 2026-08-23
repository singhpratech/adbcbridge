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
(e.g. `MYSQL_CONN=...`). Inside it, `{drv}` expands to the driver library and `{drvdir}`
to the directory holding it. The file-based entries need no server at all: `sqlite` and
`duckdb` create their database in a temp dir, and `access` reads a checked-in `.mdb`
fixture.

The script opens the driver libraries of the entries it is about to run *before* it
imports pyarrow. MySQL Connector/ODBC cannot be loaded at all once pyarrow has been
imported, so without that the eleven MySQL-wire entries would fail with unixODBC's
`Can't open lib ... : file not found`; see
[`docs/TROUBLESHOOTING.md`](../../docs/TROUBLESHOOTING.md).

Services under the `extra` compose profile are not started by a plain `up -d`; name the
profile to bring one up:

```sh
docker compose -f tests/compat/docker-compose.yml --profile extra up -d dolt
```

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

## Dolt

[Dolt](https://github.com/dolthub/dolt) is a version-controlled SQL database — a Git-like
commit graph over tables — that serves the MySQL wire protocol, so the same MySQL
Connector/ODBC drives it (see the MySQL section above for the driver download and the
`LD_PRELOAD` pyarrow needs).  It is behind the `extra` compose profile, so a plain
`up -d` does not start it.

### Start the server

```sh
docker compose -f tests/compat/docker-compose.yml --profile extra up -d dolt
# or, standalone:
docker run -d --name adbcbridge-dolt --memory=2g -e DOLT_ROOT_HOST=% \
  -p 127.0.0.1:13310:3306 dolthub/dolt-sql-server
```

`DOLT_ROOT_HOST=%` is not optional. The image creates `root@localhost` by default, and a
client arriving through the published port is not `localhost` to the server, so without it
every connection fails with `Access denied for user 'root'`.

Then create the database — the image has no `MYSQL_DATABASE`-style env var, and `dolt sql`
inside the container talks to the running server:

```sh
docker exec adbcbridge-dolt dolt sql -q "CREATE DATABASE IF NOT EXISTS adbc"
```

### Run the entry

```sh
export DOLT_ODBC_DRIVER=/tmp/dbs/mysql/mysql-connector-odbc-9.4.0-linux-glibc2.28-x86-64bit/lib/libmyodbc9w.so
LD_PRELOAD=/lib/x86_64-linux-gnu/libstdc++.so.6 \
ADBC_ODBC_DRIVER=$PWD/build/libadbc_driver_odbc.so \
python tests/compat/test_matrix.py dolt
# dolt      PASS  (MySQL (via ODBC) 8.0.33)
```

### The `PLUGIN_DIR` in the connection string

Dolt authenticates with `mysql_native_password` and offers nothing else — asked for
`caching_sha2_password` it accepts the `ALTER USER` but then closes the handshake with
`No authentication methods available for authentication`. MySQL Connector/ODBC 9.x, in
turn, no longer links `mysql_native_password` in; it ships as a loadable plugin, and the
compiled-in search path is `/usr/local/mysql/lib/plugin`, which an unpacked (non-root)
tarball does not populate. The result is:

```
Authentication plugin 'mysql_native_password' cannot be loaded:
/usr/local/mysql/lib/plugin/mysql_native_password.so: cannot open shared object file
```

The tarball does ship the plugin, in `lib/plugin/` beside the driver library, so the entry
points `PLUGIN_DIR` at it. To keep a machine-specific path out of the repo, the connection
string uses `{drvdir}` — a second placeholder alongside `{drv}`, expanding to the directory
holding the driver library — so the entry reads
`…;User=root;PLUGIN_DIR={drvdir}/plugin;`. A driver installed somewhere whose plugins live
elsewhere can override the whole string with `DOLT_CONN`.

This is a packaging mismatch between the two, not a bug in either, and nothing in `src/`
works around it: it is a connection option.

### Notes

Dolt needed no driver quirk and no tolerance flag beyond the two the MySQL entry already
carries (`bool_type="int8"` for `TINYINT(1)`, and `ANSI_QUOTES` in `sql_mode` for the
double-quoted identifiers `adbc_ingest` emits). `VARBINARY(10)`, `DATE`, `DATETIME(6)`,
`DECIMAL(10,3)`, the emoji round-trip, NULL parameters, affected-row counts and the
5000-row batched read all pass unchanged.

One thing worth recording because it is easy to misread as a driver bug: under **pyodbc**,
a `datetime` bound as a parameter reaches a Dolt `DATETIME(6)` column truncated to one
fractional digit (`13:45:10.123456` → `13:45:10.1`), while the same value inserted as a SQL
literal round-trips exactly. Dolt describes every `DATETIME` column as scale 0 (`SQLColumns`
returns a NULL `DECIMAL_DIGITS`), and pyodbc's own parameter binding follows that. adbcbridge
binds timestamps with an explicit scale, so it stores and reads back the full microseconds
and the matrix assertion passes — the benchmark's pyodbc column is the only place the
truncation is visible.


## Databend

Databend is a column-store analytic warehouse that speaks the **MySQL wire protocol**, so
MySQL Connector/ODBC drives it and no new driver download is needed -- point
`DATABEND_ODBC_DRIVER` at the same `libmyodbc9w.so` the `mysql` entry uses (see
[MySQL 8](#mysql-8) for where to unpack it).

Server (or the `databend` service in `docker-compose.yml`, which is in the `extra`
compose profile so a plain `up -d` does not start it):

```sh
docker run -d --name adbcbridge-databend --memory=2g -p 127.0.0.1:13311:3307 \
  -e QUERY_DEFAULT_USER=root -e QUERY_DEFAULT_PASSWORD=adbc datafuselabs/databend
```

It is ready in a few seconds; `docker logs` prints `MySQL listened at 0.0.0.0:3307`.

Run the entry:

```sh
export DATABEND_ODBC_DRIVER=$MYSQL_ODBC_DRIVER
LD_PRELOAD=/lib/x86_64-linux-gnu/libstdc++.so.6 \
ADBC_ODBC_DRIVER=$PWD/build/libadbc_driver_odbc.so \
python tests/compat/test_matrix.py databend
# databend  PASS  (MySQL 8.0.90-v1.2.881-...)
```

The `LD_PRELOAD` is the same static-TLS workaround the `mysql` entry needs; see that
section for why.

### Connection string: `NO_SSPS=1` and `PLUGIN_DIR`

Two settings in the entry's connection string are load-bearing.

`NO_SSPS=1` is what makes the entry work at all. Databend's MySQL handler refuses
`COM_STMT_PREPARE` outright:

```
Prepare is not support in Databend. (1105) (SQLPrepare)
```

so every parameterised statement fails at `SQLPrepare`. With `NO_SSPS=1` the connector
stops using the server-side prepare protocol and substitutes bound parameters into the
SQL text itself, sending each statement as a plain query.

`PLUGIN_DIR` is needed only for the unpacked (non-root) tarball. Databend authenticates
with `mysql_native_password`, which is no longer built into Connector/ODBC 9 -- it is a
loadable plugin, shipped in `lib/plugin` beside the driver, and the driver's compiled-in
default path (`/usr/local/mysql/lib/plugin`) does not exist in a tarball install:

```
Authentication plugin 'mysql_native_password' cannot be loaded:
/usr/local/mysql/lib/plugin/mysql_native_password.so: cannot open shared object file
```

The entry writes `{plugin}` in its connection string rather than a fixed path;
`test_matrix.py` expands that to `PLUGIN_DIR=<dir of the driver>/plugin` when such a
directory exists and to nothing when it does not, so a packaged root install (whose
default plugin path is correct) is unaffected.

### Driver quirks: MySQL syntax against a server that is not MySQL

Both quirks come from the same root cause -- MySQL Connector/ODBC assumes the server on
the other end is a MySQL -- and both are keyed in `OdbcDetectQuirks` on the driver being
`myodbc` *and* the server reporting `SQL_TXN_CAPABLE = SQL_TC_NONE`. MySQL and MariaDB are
both transactional, so that pair identifies a MySQL-wire warehouse without keying on a
version string.

**`_binary` charset introducers** (`temporal_binary_param_as_varchar`). In `NO_SSPS` mode
the connector renders every parameter whose SQL type is not character or numeric as a
MySQL charset-introducer literal -- `_binary'2024-02-29'` for a `DATE`, likewise for
`TIMESTAMP` and `VARBINARY`. Introducers are MySQL/MariaDB syntax, so Databend's parser
rejects them:

```
1 | (_binary'2024-02-29'
  |         ^^ unexpected `'2024-02-29'`
```

A standalone C probe confirms this is the ODBC driver's own rendering and not anything
adbcbridge does: binding the identical value as `SQL_C_CHAR -> SQL_VARCHAR` succeeds where
`SQL_C_TYPE_DATE -> SQL_TYPE_DATE`, `SQL_C_CHAR -> SQL_TYPE_DATE` and
`SQL_C_BINARY -> SQL_VARBINARY` all fail -- it is the *SQL* type that decides. The quirk
therefore binds dates, timestamps and binaries as plain `SQL_VARCHAR` text and lets the
server's own literal parsing coerce them, the same trick the sub-second `TIME` path
already uses for every driver. The column-wise parameter-array path defers those columns
to the row-at-a-time path, exactly as `null_param_as_varchar` does; nothing is lost,
because a driver that substitutes parameters into the SQL text sends one statement per
parameter set either way.

**MySQL type names in ingest DDL** (`ansi_ddl_type_names`). `SQLGetTypeInfo` answers
with MySQL's type system whatever the server is -- `bit` for `SQL_BIT`, `long varchar` for
`SQL_LONGVARCHAR`, `long varbinary`, `datetime` -- and Databend rejects most of those
names:

```
CREATE TABLE `adbc_ing` (`a` bigint, `b` long varchar, `c` double, `d` date, `e` bit)
                                         --- ^^^^ unexpected `long`
```

The quirk skips `SQLGetTypeInfo` and uses the portable ISO names `ColumnTypeSql()` already
carries as its per-type fallbacks (`BOOLEAN`, `BIGINT`, `DOUBLE PRECISION`, `TEXT`,
`BLOB`, `DATE`, `TIMESTAMP`, `DECIMAL(p,s)`), every one of which Databend accepts.

### Entry notes

Databend needs no `sql_mode` `setup` step: its default dialect is PostgreSQL, so the
double-quoted identifiers `adbc_ingest` emits already parse.

Three things the entry has to tolerate, all of them Databend's own MySQL-wire metadata
rather than driver bugs:

* **`decimal_as_string`.** Databend describes every `DECIMAL` column with scale 0 --
  `DECIMAL(10,3)` arrives as precision 9, scale 0 -- while sending the digits themselves
  in full. Taken at face value that scale rounds `12.345` to `12`, so the entry sets the
  `adbc.odbc.decimal_as_string` database option (via the entry's `db_kwargs`) and reads
  decimals as their exact text.
* **`b` is `VARCHAR`, not `BINARY`** -- the same choice the ClickHouse entry makes with
  `String`. A Databend `BINARY` column is rendered as *hex text* on the MySQL wire, so
  `b"\x01\x02"` reads back as `b"0102"` and no byte string ever round-trips. A character
  column carries the two bytes through unchanged.
* **`bool_type="int16"`.** Databend sends `BOOLEAN` as a `SMALLINT`; MySQL's own `BOOLEAN`
  is `TINYINT(1)`, which the same driver reports as `SQL_TINYINT` -> `int8`.

Everything else in the workload passes unchanged: the emoji round-trip, microsecond
timestamps, NULL parameters, `GetObjects`, error mapping and affected-row counts (Databend
answers `SQLRowCount` for its own `INSERT`s even though it reports `SQL_TC_NONE` for
transactions).

The entry sets `big_rows=2000` rather than the default 5000. Databend commits a fresh
immutable data block per `INSERT`, and the connector in `NO_SSPS` mode sends one statement
per parameter set -- a parameter array is not turned into a multi-row `INSERT` -- so
single-row ingest runs at a few tens of rows per second (35-44 rows/s on the benchmark's
narrower row). 2000 rows still crosses the reader's 1024-row batch boundary, which is what
that step exists to test. Reads are not affected: the same server streams 2000 rows back
at ~78,000 rows/s.

### Clean up

```sh
docker rm -f adbcbridge-databend
```

## Percona Server 8.4

Percona Server is a drop-in replacement for MySQL: same wire protocol, same client
libraries, same SQL, so the MySQL Connector/ODBC build used for the `mysql` entry drives
it unchanged and no new driver has to be downloaded.

Server (or use the `percona` service in `docker-compose.yml`, which is in the `extra`
profile so a plain `up -d` leaves it alone):

```sh
docker run -d --name adbcbridge-percona --memory=2g -p 127.0.0.1:13312:3306 \
  -e MYSQL_ROOT_PASSWORD=adbc -e MYSQL_DATABASE=adbc \
  -e MYSQL_USER=adbc -e MYSQL_PASSWORD=adbc percona/percona-server:8.4
```

It is ready when `docker logs adbcbridge-percona` reports `ready for connections` on
port 3306 (a few seconds; the entrypoint initialises the data directory on first start).

Run the entry — same driver, same `LD_PRELOAD` as MySQL (see the MySQL 8 section above
for why `libstdc++` has to be preloaded under pyarrow):

```sh
export PERCONA_ODBC_DRIVER=$MYSQL_ODBC_DRIVER  # the MySQL Connector/ODBC libmyodbc9w.so
LD_PRELOAD=/lib/x86_64-linux-gnu/libstdc++.so.6 \
ADBC_ODBC_DRIVER=$PWD/build/libadbc_driver_odbc.so \
python tests/compat/test_matrix.py percona
# percona   PASS  (MySQL (via ODBC) 8.4.11-11)
```

Entry notes: identical to the `mysql` entry apart from the port. `ANSI_QUOTES` goes into
`sql_mode` (the entry's `setup`) for the double-quoted identifiers `adbc_ingest` emits,
and `BOOLEAN` is `TINYINT(1)`, reported as `SQL_TINYINT`, so `bo` is expected as `int8`.
No tolerance flags beyond that and no driver quirk in `src/`: the emoji round-trip,
microsecond `DATETIME(6)`, `DECIMAL(10,3)`, `VARBINARY`, NULL parameters, affected-row
counts, the 5000-row batched read and `GetObjects` all pass as they do on MySQL. Percona
reports itself to `SQLGetInfo` as `MySQL` (the version string `8.4.11-11` is what
identifies the fork), so `adbc_get_info` names MySQL, not Percona.

## MonetDB

Server (Dec2025-SP3, 11.55.x) on `127.0.0.1:15000`, database `adbc`, user `monetdb`:

```sh
docker run -d --name adbcbridge-monetdb \
  -e MDB_DB_ADMIN_PASS=adbc -e MDB_CREATE_DBS=adbc \
  -p 127.0.0.1:15000:50000 monetdb/monetdb:latest
```

`MDB_CREATE_DBS` names the databases to create and `MDB_DB_ADMIN_PASS` sets the password
of their `monetdb` admin user — the entrypoint refuses to start if one is given without
the other.

The ODBC driver is not in the Debian/Ubuntu archives (MonetDB was removed from them);
get it from MonetDB's own apt repository without root. `libMonetODBC.so` needs libmapi,
libstream and libmutils from the same release, so unpack all four packages into one tree
and point `LD_LIBRARY_PATH` at it:

```sh
mkdir -p /tmp/monetdb && cd /tmp/monetdb
base=https://www.monetdb.org/downloads/deb/pool/noble/monetdb/m/monetdb
for p in libmonetdb-client-odbc libmonetdb-client28 libmonetdb-stream28 libmonetdb-mutils; do
  curl -sLO $base/${p}_11.55.7_amd64.deb
  dpkg-deb -x ${p}_11.55.7_amd64.deb ex
done
```

(Replace `noble` with your distribution's codename — the repository also carries
`jammy`, `focal`, `bookworm`, `trixie` and others — and bump `11.55.7` to whatever
version that codename currently ships; `dists/<codename>/monetdb/binary-amd64/Packages`
lists them.)

Run the entry:

```sh
export LD_LIBRARY_PATH=/tmp/monetdb/ex/usr/lib/x86_64-linux-gnu
MONETDB_ODBC_DRIVER=/tmp/monetdb/ex/usr/lib/x86_64-linux-gnu/libMonetODBC.so \
ADBC_ODBC_DRIVER=$PWD/build/libadbc_driver_odbc.so \
  python tests/compat/test_matrix.py monetdb
# monetdb   PASS  (MonetDB 11.55.0007)
```

The entry needs no tolerance flags and the driver needs no quirk: MonetDBODBClib handles
typed parameters, NULL parameters, row counts, 5000-row batched reads and the standard
catalog calls as they are. Identifiers fold to lower case, like the unquoted names the
workload uses, so no `ident` mapping is needed either.
## CockroachDB

CockroachDB speaks the PostgreSQL wire protocol, so it needs no ODBC driver of
its own — the same `psqlodbc` build used for the `postgres` entry drives it.

### Get the ODBC driver without root

```sh
mkdir -p /tmp/adbc-drivers && cd /tmp/adbc-drivers
apt-get download odbc-postgresql
dpkg-deb -x odbc-postgresql_*.deb pgodbc
# the driver and the shared libraries it links against:
export COCKROACH_ODBC_DRIVER=$PWD/pgodbc/usr/lib/x86_64-linux-gnu/odbc/psqlodbcw.so
export LD_LIBRARY_PATH=$PWD/pgodbc/usr/lib/x86_64-linux-gnu:$LD_LIBRARY_PATH
```

### Start the server

```sh
docker compose -f tests/compat/docker-compose.yml up -d cockroachdb
# or standalone:
docker run -d --name adbcbridge-cockroach \
  -p 127.0.0.1:16257:26257 -p 127.0.0.1:18080:8080 \
  cockroachdb/cockroach:latest \
  start-single-node --insecure --cache=128MiB --max-sql-memory=128MiB
```

A single insecure node comes up on `127.0.0.1:16257` (SQL, user `root`, no
password, database `defaultdb`) with the DB Console on
<http://127.0.0.1:18080>. It is ready when this succeeds:

```sh
docker exec adbcbridge-cockroach ./cockroach sql --insecure -e 'SELECT version()'
```

### Run the entry

```sh
ADBC_ODBC_DRIVER=build/libadbc_driver_odbc.so \
  .venv/bin/python tests/compat/test_matrix.py cockroachdb
# cockroachdb PASS  (PostgreSQL 18.0.0)
```

### Notes

The entry needs **no tolerance flags and no driver quirk** — CockroachDB passes
the full workload through the unmodified PostgreSQL code path. `GetInfo` reports
`PostgreSQL 18.0.0` because CockroachDB advertises a PostgreSQL server version
over the wire; `SELECT version()` is the only way to tell the two apart, and
`SQL_DRIVER_NAME` is `psqlodbcw.so` for both. That is exactly why no quirk keyed
on the driver name would be correct here: it would also fire on real PostgreSQL.

Type-name differences worth knowing for the ingest path:

| Standard / PostgreSQL | CockroachDB | note |
|---|---|---|
| `INTEGER` | `INT8` | `INTEGER` is **64-bit** in CockroachDB, not 32-bit; it reports column size 19 and reads back as `int64` |
| `DOUBLE PRECISION` | `FLOAT8` | same semantics |
| `BYTEA` | `BYTES` | `BYTEA` and `BLOB` are both accepted as synonyms |
| `NUMERIC(p,s)` | `DECIMAL(p,s)` | both spellings accepted |
| `TEXT` | `STRING` | `TEXT` and `VARCHAR` are accepted as synonyms |
| `CLOB` | — | **not** accepted (`ERROR: type "clob" does not exist`); use `STRING`/`TEXT`. `BLOB` *is* accepted and canonicalises to `BYTES` |
| `TIMESTAMP` | `TIMESTAMP` | stored in UTC with microsecond precision; `TIMESTAMP` is *without* time zone, so no offset is applied on the way in or out and the round-trip is exact |

None of this needs special handling on the ingest path: `adbc_ingest` negotiates
its `CREATE TABLE` type names through `SQLGetTypeInfo` on the psqlodbc side, and
every name that driver hands back is one CockroachDB already accepts. An ingest
of `int64/string/double/date32/bool` lands as `INT8 / STRING / FLOAT8 / DATE /
BOOL`.

The DDL declares `i INTEGER PRIMARY KEY`, and the primary key is load-bearing
rather than decorative. A CockroachDB table created without a primary key gets a
synthesised hidden column:

```
rowid INT8 NOT VISIBLE NOT NULL DEFAULT unique_rowid(),
CONSTRAINT adbc_t_pkey PRIMARY KEY (rowid ASC)
```

`SELECT *` never returns `rowid`, so the read path is unaffected — but
`information_schema.columns` lists it, and `psqlodbc` builds `SQLColumns` from
that view without filtering on `is_hidden`. `GetObjects` and `GetTableSchema`
therefore report a 9th column `rowid`. Declaring an explicit primary key is the
supported fix (and is what CockroachDB's own documentation recommends for every
table). Tables created by `adbc_ingest` get the hidden `rowid` too; it is
harmless there because ingest names its columns explicitly.

### Clean up

```sh
docker compose -f tests/compat/docker-compose.yml down cockroachdb
# or, if started standalone:
docker stop adbcbridge-cockroach && docker rm adbcbridge-cockroach
```
## YugabyteDB

YugabyteDB's YSQL layer is the PostgreSQL 15 query engine running on top of a
distributed (Raft-replicated, sharded) storage layer, so it speaks the
PostgreSQL wire protocol and the same `psqlodbc` build used for the `postgres`
entry drives it -- no ODBC driver of its own.

### Get the ODBC driver without root

```sh
mkdir -p /tmp/adbc-drivers && cd /tmp/adbc-drivers
apt-get download odbc-postgresql
dpkg-deb -x odbc-postgresql_*.deb pgodbc
# the driver and the shared libraries it links against:
export YUGABYTE_ODBC_DRIVER=$PWD/pgodbc/usr/lib/x86_64-linux-gnu/odbc/psqlodbcw.so
export LD_LIBRARY_PATH=$PWD/pgodbc/usr/lib/x86_64-linux-gnu:$LD_LIBRARY_PATH
```

### Start the server

```sh
docker compose -f tests/compat/docker-compose.yml up -d yugabyte
# or standalone:
docker run -d --name adbcbridge-yugabyte -p 127.0.0.1:15433:5433 \
  yugabytedb/yugabyte:latest bin/yugabyted start --background=false \
  --tserver_flags=memory_limit_hard_bytes=805306368,use_memory_defaults_optimized_for_ysql=false \
  --master_flags=memory_limit_hard_bytes=402653184,use_memory_defaults_optimized_for_ysql=false
```

`yugabyted` starts a master and a tserver in the one container, and left alone
they size their heaps as a fraction of *total host RAM*. The two
`memory_limit_hard_bytes` caps above hold the whole node under 1 GiB resident
(measured: ~800 MiB steady state), which is what makes it cheap enough to run
next to the rest of the matrix. `use_memory_defaults_optimized_for_ysql=false`
goes with them: it keeps the pre-2024 memory-division defaults instead of the
YSQL-optimised ones, which are tuned for nodes with at least 2 GiB.

The single node comes up on `127.0.0.1:15433` (YSQL, user `yugabyte`, **no
password**, database `yugabyte`). The image is a ~1.65 GB pull; once it is local
the node is ready about half a minute after `up -d`, when this succeeds:

```sh
docker exec adbcbridge-yugabyte bin/ysqlsh -h "$(docker exec adbcbridge-yugabyte hostname -i)" \
  -U yugabyte -d yugabyte -c 'SELECT version()'
```

`ysqlsh` cannot use `-h 127.0.0.1` *inside* the container: `yugabyted` binds YSQL
to the container's own address, not to loopback. From the host the published
port works normally.

### Run the entry

```sh
ADBC_ODBC_DRIVER=build/libadbc_driver_odbc.so \
  .venv/bin/python tests/compat/test_matrix.py yugabyte
# yugabyte  PASS  (PostgreSQL (via ODBC) 15.0.12)
```

### Notes

The entry needs **no tolerance flags and no driver quirk**: YugabyteDB passes the
full workload through the unmodified PostgreSQL code path, with the same DDL the
`postgres` entry uses. `GetInfo` reports `PostgreSQL 15.0.12` because YSQL
advertises a PostgreSQL server version over the wire (`SQL_DBMS_NAME` is
`PostgreSQL`, `SQL_DRIVER_NAME` is `psqlodbcw.so`) -- exactly as CockroachDB
does. `SELECT version()` is the only way to tell them apart:

```
PostgreSQL 15.12-YB-2026.1.1.1-b0 on x86_64-pc-linux-gnu, ...
```

Any future YugabyteDB-specific quirk would therefore have to key on that
`-YB-` marker in `SQL_DBMS_VER`/`version()`, never on the DBMS or driver name,
which real PostgreSQL shares.

Unlike CockroachDB, the DDL needs **no explicit `PRIMARY KEY`**. A YSQL table
declared without one still gets an internal row identifier, but it is a *system*
column (`ybctid`, `attnum` -7) rather than a user column with a hidden flag, so
`information_schema.columns` never lists it and `SQLColumns`/`GetObjects`
/`GetTableSchema` see exactly the eight declared columns.

Types behave as in PostgreSQL 15 and need no translation:

| Type in the DDL | YSQL behaviour |
|---|---|
| `INTEGER` | 32-bit, as in PostgreSQL (**not** 64-bit as in CockroachDB) -> `int32` |
| `DOUBLE PRECISION`, `BYTEA`, `DATE`, `BOOLEAN` | identical to PostgreSQL |
| `TIMESTAMP` | microsecond precision, without time zone; the round-trip is exact |
| `NUMERIC(10,3)` | reported with the declared precision -> `decimal128(10, 3)` |

An `adbc_ingest` of `int64/string/double/date32/bool` lands as `bigint / text /
double precision / date / boolean`, the same mapping `psqlodbc` negotiates for
PostgreSQL through `SQLGetTypeInfo`. The 5000-row batched ingest and read-back
complete in about two seconds on a single local node.

### Clean up

```sh
docker compose -f tests/compat/docker-compose.yml stop yugabyte
docker compose -f tests/compat/docker-compose.yml rm -f yugabyte
# or, if started standalone:
docker stop adbcbridge-yugabyte && docker rm adbcbridge-yugabyte
```
## Firebird 5

Server (port 13050, database `/var/lib/firebird/data/adbc.fdb`, user `adbc`/`adbc`):

```sh
docker compose -f tests/compat/docker-compose.yml up -d firebird
# or, standalone:
docker run -d --name adbcbridge-firebird -p 127.0.0.1:13050:3050 \
  -e FIREBIRD_ROOT_PASSWORD=adbc -e FIREBIRD_USER=adbc -e FIREBIRD_PASSWORD=adbc \
  -e FIREBIRD_DATABASE=adbc.fdb -e FIREBIRD_DATABASE_DEFAULT_CHARSET=UTF8 \
  firebirdsql/firebird:5
```

There is no `firebird-odbc` Debian/Ubuntu package; take the official release tarball. It
contains a single `libOdbcFb.so` which needs no installation — point
`FIREBIRD_ODBC_DRIVER` straight at it:

```sh
FB=$PWD/fbodbc && mkdir -p $FB && cd $FB
curl -sLO https://github.com/FirebirdSQL/firebird-odbc-driver/releases/download/v3.5.0-rc1/firebird-odbc-driver-3.5.0-rc1-linux-x64.tar.gz
tar xzf firebird-odbc-driver-3.5.0-rc1-linux-x64.tar.gz   # -> libOdbcFb.so
```

`libOdbcFb.so` does not link `libfbclient` — it `dlopen`s it, so `libfbclient.so.2` has to
be on `LD_LIBRARY_PATH` (or named with `CLIENT=` in the connection string). Copying it out
of the server image guarantees the client matches the server; `libfbclient` in turn needs
`libtommath`, which `apt-get download` provides without installing anything:

```sh
mkdir -p $FB/fbclient && cd $FB/fbclient
docker cp adbcbridge-firebird:/opt/firebird/lib/libfbclient.so.5.0.4 .
ln -sf libfbclient.so.5.0.4 libfbclient.so.2
apt-get download libtommath1 && dpkg-deb -x libtommath1_*.deb ex
cp -a ex/usr/lib/x86_64-linux-gnu/libtommath.so.1* .
```

Run the entry:

```sh
export LD_LIBRARY_PATH=$FB/fbclient:$LD_LIBRARY_PATH
export FIREBIRD_ODBC_DRIVER=$FB/libOdbcFb.so
export ADBC_ODBC_DRIVER=$PWD/build/libadbc_driver_odbc.so
python tests/compat/test_matrix.py firebird
# firebird  PASS  (Firebird 06.03.1812 LI-V Firebird 5.0)
```

Notes on this driver/server pair, all visible in the matrix entry:

* Identifiers are folded to upper case unless quoted (`ident=str.upper`), like Oracle.
* The connection string uses Firebird's URL form,
  `DBNAME=inet://127.0.0.1:13050//var/lib/firebird/data/adbc.fdb`, plus `CHARSET=UTF8`
  (without it the connection is `NONE` and non-ASCII text does not round-trip).
* `b` is `BLOB SUB_TYPE BINARY`: Firebird's `VARBINARY` is `CHAR CHARACTER SET OCTETS`,
  which OdbcFb describes as `SQL_VARCHAR`, and binding a `SQL_C_BINARY` parameter to it
  stores an empty value.
* `decimal_type`: `NUMERIC(10,3)` is stored as a scaled `BIGINT` and OdbcFb reports the
  storage precision, so the column arrives as `decimal128(18, 3)`.
* `ts_us`: Firebird's `TIMESTAMP` resolution is 1/10000 s, so `13:45:10.123456` reads back
  as `13:45:10.1234`.
* OdbcFb sizes `SQL_C_WCHAR` buffers in 4-byte `wchar_t` while unixODBC passes UTF-16.
  adbcbridge detects the driver (`SQL_DRIVER_NAME` = `OdbcFb`) and stays on the narrow
  UTF-8 path — see `wchar_as_utf8` in `src/odbc_internal.h`.
* Bulk ingest batches through a third `INSERT` form here. Firebird's dialect has no
  multi-row `VALUES` (`-104 Token unknown` at the second row-group's comma) and no
  Oracle-style `INSERT ALL`, so the probe falls through to
  `INSERT INTO t (cols) SELECT CAST(? AS <type>), … FROM RDB$DATABASE UNION ALL SELECT …`
  (`multirow_union_from`). A bare `?` alone in a select list has no type Firebird can
  infer, hence the `CAST`; the type it names is the one bulk ingest would have *created*
  for that column, so it holds the bound value exactly and the target column's own limits
  are still enforced by the `INSERT` — a string too long for it raises `string right
  truncation` here, as a one-row `INSERT` does. The engine allows 256 relation contexts
  per statement, so K settles at ~250 row-groups.
* A NULL bound with `SQL_C_DEFAULT` on a `SQL_BIGINT` parameter used to poison it: the
  ODBC default C type for `SQL_BIGINT` is `SQL_C_CHAR`, OdbcFb retypes the parameter and
  never re-derives it, so every 64-bit integer after the first NULL in that column was
  written as NULL without a diagnostic. NULLs of that type now go as `SQL_C_SBIGINT`
  (`NullParamCType` in `src/odbc_bind.c`).

## TimescaleDB (PostgreSQL 16 + timescaledb)

TimescaleDB is the `timescaledb` extension on top of stock PostgreSQL, so it speaks the
PostgreSQL wire protocol and the same `psqlodbc` build used for the `postgres` entry
drives it — there is no TimescaleDB ODBC driver.

### Get the ODBC driver without root

```sh
mkdir -p /tmp/adbc-drivers && cd /tmp/adbc-drivers
apt-get download odbc-postgresql
dpkg-deb -x odbc-postgresql_*.deb pgodbc
export TIMESCALE_ODBC_DRIVER=$PWD/pgodbc/usr/lib/x86_64-linux-gnu/odbc/psqlodbcw.so
export LD_LIBRARY_PATH=$PWD/pgodbc/usr/lib/x86_64-linux-gnu:$LD_LIBRARY_PATH
```

### Start the server

```sh
docker compose -f tests/compat/docker-compose.yml up -d timescaledb
# or standalone:
docker run -d --name adbcbridge-timescale -p 127.0.0.1:15434:5432 \
  -e POSTGRES_USER=adbc -e POSTGRES_PASSWORD=adbc -e POSTGRES_DB=adbc \
  timescale/timescaledb:latest-pg16
```

The port is `15434` so the entry can run alongside the plain `postgres` entry on
`15432`. The image installs the extension into the `POSTGRES_DB` database itself
(`shared_preload_libraries` is already set for it); the entry still runs
`CREATE EXTENSION IF NOT EXISTS timescaledb` in `setup`, which makes it work on a plain
PostgreSQL image that merely has the extension available. It is ready when this
succeeds:

```sh
docker exec adbcbridge-timescale psql -U adbc -d adbc \
  -c "SELECT extversion FROM pg_extension WHERE extname = 'timescaledb'"
```

### Run the entry

```sh
ADBC_ODBC_DRIVER=$PWD/build/libadbc_driver_odbc.so \
  .venv/bin/python tests/compat/test_matrix.py timescaledb
# timescaledb PASS  (PostgreSQL (via ODBC) 16.0.15)
```

### What the entry tests beyond `postgres`

The standard workload would make this a duplicate of the `postgres` entry, so the entry
also exercises a **hypertable** — the partitioned time-series table that is the reason to
run TimescaleDB at all. Its `extra` steps (see below) create a table, turn it into a
hypertable partitioned by a `DATE` column, bulk-ingest into it through `adbc_ingest`, and
read it back:

```sql
CREATE TABLE "adbc_ht" ("a" BIGINT, "b" VARCHAR(20), "c" DOUBLE PRECISION,
                        "d" DATE NOT NULL, "e" BOOLEAN);
SELECT (create_hypertable('adbc_ht', by_range('d', INTERVAL '7 days'))).created;
-- ingest 4 rows spanning Jan/Feb/Mar 2024, then:
SELECT count(*) FROM timescaledb_information.chunks WHERE hypertable_name = 'adbc_ht';
SELECT count(DISTINCT time_bucket(INTERVAL '7 days', "d")) FROM "adbc_ht";
```

Both counts are `3`: the four rows land in three chunks (the 2024-01-01 and 2024-01-02
rows share one 7-day chunk), which confirms the ingest really went through the
partitioning machinery rather than into a plain table. `time_bucket()` is TimescaleDB's own
bucketing function, so the last query also proves the read path works on the result of a
Timescale-provided function.

Two things are load-bearing in that DDL:

* **`"d" DATE NOT NULL`.** `create_hypertable()` requires the partitioning column to be
  `NOT NULL` (it adds the constraint itself when it can). This is why the entry ingests
  its own `EXTRA_ROWS` payload rather than the matrix's standard ingest table — that one
  carries a NULL in the date column, which a hypertable rejects.
* **The main `adbc_t` table stays a plain table.** Its `ts`/`d` columns are NULL in the
  second row the workload inserts, so it cannot be partitioned on either of them, and
  the standard checks (`GetObjects`, `GetTableSchema`, batched reads) are meant to run
  against an ordinary table anyway.

Chunks live in the internal `_timescaledb_internal` schema under generated names
(`_hyper_1_1_chunk`, …), so they never collide with the `adbc_t`/`adbc_ing` names the
standard workload filters on in `GetObjects`.

### The `extra` key

`extra` is a generic per-database hook in `test_matrix.py`, not a Timescale special
case: a list of `(step, expected)` pairs run after the standard workload, where `step` is
either SQL or a `(table, arrow table)` pair to bulk-ingest with `mode="append"`, and
`expected` is the first result row (or, for an ingest, the row count) to assert — `None`
runs the step without checking it. `{sfx}` in a step expands to `ADBC_MATRIX_SUFFIX`, so
extra tables are isolated per run exactly like `adbc_t` is. Any other database can use it
for its own dialect-specific feature.

### Notes

The entry needs **no tolerance flags and no driver quirk**: everything the plain
PostgreSQL entry passes, TimescaleDB passes identically, and the hypertable steps need no
special handling on the ADBC side either — `adbc_ingest` appends into a hypertable with
the same `INSERT` path it uses for a plain table, and Timescale routes the rows to chunks
transparently. `GetInfo` reports the PostgreSQL server version (`16.0.15` for
`timescale/timescaledb:latest-pg16`, which runs PostgreSQL 16.15 with timescaledb 2.29.2);
the extension version is only visible via `pg_extension`, exactly as for a real
PostgreSQL server, so no quirk keyed on the driver or server name could tell the two
apart — nor should it.

The entry is re-runnable: its first `extra` step is `DROP TABLE IF EXISTS`, which drops
the hypertable and all of its chunks.

### Clean up

```sh
docker compose -f tests/compat/docker-compose.yml down timescaledb
# or, if started standalone:
docker stop adbcbridge-timescale && docker rm adbcbridge-timescale
```

## Citus 14 (PostgreSQL 18 + citus)

Citus is the `citus` extension on top of stock PostgreSQL — it shards a table across a
cluster and plans queries over the shards — so it speaks the PostgreSQL wire protocol and
the same `psqlodbc` build used for the `postgres` entry drives it. There is no Citus ODBC
driver.

### Get the ODBC driver without root

```sh
mkdir -p /tmp/adbc-drivers && cd /tmp/adbc-drivers
apt-get download odbc-postgresql
dpkg-deb -x odbc-postgresql_*.deb pgodbc
export CITUS_ODBC_DRIVER=$PWD/pgodbc/usr/lib/x86_64-linux-gnu/odbc/psqlodbcw.so
export LD_LIBRARY_PATH=$PWD/pgodbc/usr/lib/x86_64-linux-gnu:$LD_LIBRARY_PATH
```

### Start the server

```sh
docker compose -f tests/compat/docker-compose.yml --profile extra up -d citus
# or standalone:
docker run -d --name adbcbridge-citus --memory=2g -p 127.0.0.1:15436:5432 \
  -e POSTGRES_USER=adbc -e POSTGRES_PASSWORD=adbc -e POSTGRES_DB=adbc \
  citusdata/citus:latest
```

The port is `15436` so the entry can run alongside the other PostgreSQL-wire entries.
The image already sets `shared_preload_libraries = citus` and creates the extension in
`POSTGRES_DB`. It is ready when this succeeds:

```sh
docker exec adbcbridge-citus psql -U adbc -d adbc \
  -c "SELECT extversion FROM pg_extension WHERE extname = 'citus'"
```

### One container, a one-node cluster

A fresh `citusdata/citus` container has an **empty `pg_dist_node`**: it is a PostgreSQL
server with the extension loaded, not yet a cluster, and `create_distributed_table()`
fails on it with

```
ERROR:  replication_factor (1) exceeds number of worker nodes (0)
HINT:  Add more worker nodes or try again with a lower replication factor.
```

Two idempotent calls turn the single container into a one-node cluster that is its own
worker, and the entry's `setup` makes them:

```sql
SELECT citus_set_coordinator_host('localhost', 5432);                     -- register in pg_dist_node
SELECT citus_set_node_property('localhost', 5432, 'shouldhaveshards', true);
```

The second one is the load-bearing half: `citus_set_coordinator_host()` alone adds the
node with `shouldhaveshards = false`, so the cluster still has nowhere to put a shard.
They have to be idempotent because `bench/matrix_bench.py` replays `setup` on every
connection it opens.

### Run the entry

```sh
ADBC_ODBC_DRIVER=$PWD/build/libadbc_driver_odbc.so \
  .venv/bin/python tests/compat/test_matrix.py citus
# citus     PASS  (PostgreSQL (via ODBC) 18.0.4)
```

### What the entry tests beyond `postgres`

The standard workload alone would make this a duplicate of the `postgres` entry, so the
entry's `extra` steps (see the TimescaleDB section for what `extra` is) exercise the
reason to run Citus at all — a **hash-distributed table**:

```sql
CREATE TABLE "adbc_dist" ("a" BIGINT NOT NULL, "b" VARCHAR(20), "c" DOUBLE PRECISION,
                          "d" DATE, "e" BOOLEAN);
SELECT create_distributed_table('adbc_dist', 'a');
SELECT partmethod FROM pg_dist_partition WHERE logicalrelid = 'adbc_dist'::regclass;  -- 'h'
-- ingest 4 rows through adbc_ingest(mode="append"), then:
SELECT count(DISTINCT get_shard_id_for_distribution_column('adbc_dist', "a")) > 1
  FROM "adbc_dist";                                   -- true: the rows are spread over shards
SELECT count(*), sum("a") FROM "adbc_dist";           -- (4, 10), merged from the shards
```

`create_distributed_table()` splits the table into 32 shards (`citus.shard_count`), each
a real table of its own; `adbc_ingest` appends into the distributed table with the same
`INSERT` path it uses for a plain table, and Citus routes every row to the shard its
distribution column hashes to. The `> 1` check is what proves the rows went through that
machinery rather than into one local table, and the aggregate proves the read path works
on a plan that fans out to the shards and merges on the coordinator.

Two things are load-bearing:

* **`"a" BIGINT NOT NULL`.** `"a"` is the distribution column, and Citus will not accept
  a NULL there.
* **The main `adbc_t` table stays a plain table.** Its second row is NULL in every column
  but `i`, so it has no column that could serve as a distribution column, and the standard
  checks (`GetObjects`, `GetTableSchema`, batched reads) are meant to run against an
  ordinary table anyway.

Shards are named after the table with the shard id appended (`adbc_dist_102040`, …), so
they never collide with the `adbc_t`/`adbc_ing` names the standard workload filters on in
`GetObjects`.

### Notes

The entry needs **no tolerance flags and no driver quirk**: everything the plain
PostgreSQL entry passes, Citus passes identically. `GetInfo` reports the PostgreSQL server
version (`18.0.4` for `citusdata/citus:latest`, which runs PostgreSQL 18.4 with citus
14.1); the extension version is only visible via `pg_extension`, exactly as for
TimescaleDB.

The entry is re-runnable: its first `extra` step is `DROP TABLE IF EXISTS`, which drops
the distributed table and all of its shards.

### Clean up

```sh
docker compose -f tests/compat/docker-compose.yml --profile extra down citus
# or, if started standalone:
docker rm -f adbcbridge-citus
```

## CrateDB 6

CrateDB is a distributed SQL database on top of Lucene. It speaks the PostgreSQL wire
protocol (announcing itself as PostgreSQL 14), so the same `psqlodbc` build used for the
`postgres` entry drives it — there is no CrateDB ODBC driver.

### Get the ODBC driver without root

```sh
mkdir -p /tmp/adbc-drivers && cd /tmp/adbc-drivers
apt-get download odbc-postgresql
dpkg-deb -x odbc-postgresql_*.deb pgodbc
export CRATEDB_ODBC_DRIVER=$PWD/pgodbc/usr/lib/x86_64-linux-gnu/odbc/psqlodbcw.so
export LD_LIBRARY_PATH=$PWD/pgodbc/usr/lib/x86_64-linux-gnu:$LD_LIBRARY_PATH
```

### Start the server

```sh
docker compose -f tests/compat/docker-compose.yml --profile extra up -d cratedb
# or standalone:
docker run -d --name adbcbridge-cratedb --memory=2g -p 127.0.0.1:15440:5432 \
  -e CRATE_HEAP_SIZE=512m crate crate -Cdiscovery.type=single-node
```

`-Cdiscovery.type=single-node` skips the cluster bootstrap, and `CRATE_HEAP_SIZE` caps
the JVM heap (unset, it takes a quarter of host RAM). The compose service sits in the
`extra` profile so a plain `up -d` does not start it. It takes a few seconds and is
ready when this prints a version:

```sh
docker exec adbcbridge-cratedb crash -c "SELECT version['number'] FROM sys.nodes"
```

### Run the entry

```sh
ADBC_ODBC_DRIVER=$PWD/build/libadbc_driver_odbc.so \
  .venv/bin/python tests/compat/test_matrix.py cratedb
# cratedb   PASS  (PostgreSQL (via ODBC) 14.0.0)
```

### Notes

CrateDB is the first entry that is PostgreSQL on the wire but not in its type system,
and the first that is eventually consistent. What that costs the entry:

* **`refresh`.** A row is queryable only after the table's shards refresh, which happens
  once a second (`refresh_interval`), so a `SELECT` issued immediately after an `INSERT`
  or an `adbc_ingest` legitimately returns nothing. The entry sets
  `refresh='REFRESH TABLE "{}"'` and `test_matrix.py` runs it after each write. This is
  a property of the database, not of the driver: `REFRESH TABLE` is CrateDB's own
  read-your-writes statement.
* **No binary type.** CrateDB has no `BYTEA`/`BLOB` column type at all — blobs live in
  separate blob tables addressed over HTTP, outside SQL. `b` is therefore `TEXT`, and
  the bytes of a `SQL_C_BINARY` parameter arrive as whatever the ODBC driver encoded
  them to, for psqlodbc PostgreSQL's bytea hex escape (`binary_text="\x0102"`).
* **No `DATE` column type.** `CREATE TABLE t (d DATE)` fails with ``Type `date` does not
  support storage``; only `TIMESTAMP` can hold a date, so `d` is `TIMESTAMP` in the DDL.
* **`ingest_types`.** The DDL `adbc_ingest(mode="create")` generates takes its type
  names from the driver's `SQLGetTypeInfo`, which for psqlodbc is PostgreSQL's: `date`
  (above) and `bool` (CrateDB spells it `BOOLEAN` and accepts no alias). The entry maps
  `date32 -> timestamp[us]` and `bool -> int8` so the ingest is still exercised in full.
  A CrateDB user hitting this in their own code has the same two substitutions to make;
  nothing in the ODBC metadata identifies the server as CrateDB rather than PostgreSQL,
  so the driver cannot make them for you.
* **`decimal_type`.** CrateDB does not report a `NUMERIC` column's precision and scale
  over the wire, so psqlodbc falls back to its own default and the declared
  `NUMERIC(10,3)` arrives as `decimal128(28, 6)`. The value itself is exact.
* **`ts_us`.** CrateDB timestamps are millisecond-precision: `13:45:10.123456` reads
  back as `13:45:10.123`.

Benchmark (`bench/matrix_bench.py --rows 10000 --fetch-rows 100000`): ingest 626 rows/s
row-at-a-time, 511 rows/s with parameter arrays (pyodbc `executemany`: 162 rows/s), fetch
767k rows/s (pyodbc 552k). Ingest is slow in absolute terms because every `INSERT` is a
distributed write into Lucene; parameter arrays do not help, since psqlodbc sends each
set as its own bind/execute over the wire either way.

### Driver fix: a row count the driver never wrote

The first run reported `0` rows ingested for an ingest that had in fact inserted every
row. Root cause, from a standalone ODBC probe (no adbcbridge involved): with a parameter
array of three sets, psqlodbc against CrateDB behaves differently depending on whether a
transaction is open —

```
direct, autocommit           ret=0 processed=3 first RowCount=1 total=3
prepared, autocommit         ret=0 processed=3 first RowCount=1 total=3
direct, in transaction       ret=0 processed=3 first RowCount=-99 total=0
prepared, in transaction     ret=0 processed=3 first RowCount=-99 total=0
```

`-99` is the value the probe pre-filled: inside a transaction `SQLRowCount` returns
`SQL_SUCCESS` **without writing the out-parameter at all**. adbcbridge batches a
multi-row execute into one transaction, and its `OdbcRowCount()` zeroed the variable
first (for the 32-bit-`SQLLEN` quirk), so "not written" read as "0 rows affected".

The fix is in `OdbcRowCount()` (`src/odbc_reader.c`) and is generic, not keyed on any
driver: pre-fill the variable with all-ones, which reads as `-1` at either `SQLLEN`
width, so a driver that answers without writing says *unknown* rather than *none*. `-1`
is what ODBC and ADBC already use for an unavailable row count, and every driver in the
matrix that does write the out-parameter is unaffected (checked with the same probe:
sqliteodbc writes `0` after DDL, `1` after a single-row `INSERT`).

### Clean up

```sh
docker compose -f tests/compat/docker-compose.yml --profile extra down cratedb
# or, if started standalone:
docker rm -f adbcbridge-cratedb
```

## QuestDB 10

QuestDB is a column-oriented time-series database that speaks the PostgreSQL wire
protocol, so the same `psqlodbc` build used for the `postgres` entry drives it — there is
no QuestDB ODBC driver. Only the *wire protocol* is PostgreSQL's, though: the type
system, the DDL parser and the catalog are QuestDB's own, and that is where every quirk
below comes from.

### Get the ODBC driver without root

```sh
mkdir -p /tmp/adbc-drivers && cd /tmp/adbc-drivers
apt-get download odbc-postgresql
dpkg-deb -x odbc-postgresql_*.deb pgodbc
export QUESTDB_ODBC_DRIVER=$PWD/pgodbc/usr/lib/x86_64-linux-gnu/odbc/psqlodbcw.so
export LD_LIBRARY_PATH=$PWD/pgodbc/usr/lib/x86_64-linux-gnu:$LD_LIBRARY_PATH
```

### Start the server

```sh
docker compose -f tests/compat/docker-compose.yml --profile extra up -d questdb
# or standalone:
docker run -d --name adbcbridge-questdb --memory=2g \
  -e JAVA_OPTS="-Xms256m -Xmx768m" \
  -p 127.0.0.1:18812:8812 -p 127.0.0.1:19000:9000 questdb/questdb
```

PostgreSQL wire is on `18812` (user `admin`, password `quest`, database `qdb`); `19000`
is the HTTP console and REST API, which is the easiest way to look at the server
independently of ODBC. It comes up in a few seconds and is ready when this succeeds:

```sh
curl -sG http://127.0.0.1:19000/exec --data-urlencode "query=SELECT 1"
```

### Run the entry

```sh
ADBC_ODBC_DRIVER=$PWD/build/libadbc_driver_odbc.so \
  .venv/bin/python tests/compat/test_matrix.py questdb
# questdb   PASS  (PostgreSQL (via ODBC) 11.0.3)
```

`GetInfo` reports `PostgreSQL 11.0.3` because that is the server version QuestDB
advertises over the wire, and `SQL_DRIVER_NAME` is `psqlodbcw.so` — the same pair real
PostgreSQL gives. `SELECT version()` is what tells them apart:

```
PostgreSQL 12.3, compiled by Visual C++ build 1914, 64-bit, QuestDB
```

### Types

QuestDB's DDL takes its own type names, and psqlodbc's `SQLGetTypeInfo` answers with
PostgreSQL's internal ones, which QuestDB rejects outright:

| Workload column | QuestDB type | note |
|---|---|---|
| `i` | `INT` | `INTEGER` also accepted; `int4` is **not** |
| `f` | `DOUBLE` | `DOUBLE PRECISION` also accepted; `float8` is not |
| `s` | `STRING` | `TEXT` and a bare `VARCHAR` work; `VARCHAR(50)` is a **syntax error** — QuestDB's VARCHAR takes no length |
| `b` | `BINARY` | `BLOB`/`BYTEA`/`VARBINARY(n)` are not QuestDB types |
| `d` | `DATE` | millisecond resolution; reads back as a timestamp, not a date |
| `ts` | `TIMESTAMP` | microsecond resolution, no time zone — the round-trip is exact |
| `n` | `DECIMAL(10,3)` | `NUMERIC` does not exist here |
| `bo` | `BOOLEAN` | **no NULL state**: a NULL boolean is stored as false |

`bool`, `int8`, `float8`, `numeric` and `bpchar` all fail with
`unsupported column type`, while `BIGINT`, `DOUBLE PRECISION`, `BOOLEAN`, `TEXT`, `DATE`,
`TIMESTAMP` and `DECIMAL(p,s)` — the portable spellings — are all accepted. That is why
the driver detects QuestDB and spells its ingest DDL in standard SQL types
(`ansi_ddl_type_names`, below) instead of the names the ODBC driver hands it.

### Two psqlodbc settings in the connection string

Both are the driver's, not the server's:

* **`BoolsAsChar=0`.** By default psqlodbc reports a PostgreSQL `bool` column as a
  `VARCHAR(5)` holding `"1"`/`"0"`, which would make the workload's `bo` column an Arrow
  string. With it off the column is `SQL_BIT` → Arrow `bool`, as for every other entry.
* **`Protocol=7.4-0`.** The trailing digit is psqlodbc's "level of rollback on errors";
  `2` (the default) wraps each execute of a prepared statement in `SAVEPOINT`. QuestDB
  has no `SAVEPOINT` statement — it fails the whole insert with
  `internal SAVEPOINT failed` — and `0` turns that off.

### Driver quirks

Three quirks are keyed on the server, not on the driver name: `psqlodbcw.so` also drives
real PostgreSQL, CockroachDB, YugabyteDB and TimescaleDB, so a name-keyed quirk would
fire on all of them. `OdbcDetectQuirks` asks `SELECT version()` once per connection —
only for psqlodbc — and looks for `questdb` in the answer.

* **`ansi_ddl_type_names`.** Bulk ingest normally asks `SQLGetTypeInfo` for the driver's
  name of each SQL type; against QuestDB those names (`int8`, `float8`, `bool`) are not
  DDL QuestDB accepts. The flag makes `ColumnTypeSql` skip the `SQLGetTypeInfo` chain and
  use the portable fallback name it already carries for every Arrow type, so an ingest of
  `int64/string/double/date32/bool` creates `BIGINT / TEXT / DOUBLE PRECISION / DATE /
  BOOLEAN`.
* **`bool_param_as_varchar`.** QuestDB parses a boolean parameter only from the words
  `true`/`false`. psqlodbc sends an `SQL_BIT` parameter as `"1"`/`"0"`, which QuestDB
  stores as **false without any diagnostic** — every `True` silently became `False`. The
  flag binds booleans as a `VARCHAR` holding those two words instead.
* **`no_param_arrays`.** psqlodbc executes a column-wise parameter array by inlining the
  values into a single `BEGIN;INSERT ...;INSERT ...` string, where every non-numeric
  value becomes a string literal. PostgreSQL types such a literal from the target column;
  QuestDB does not convert it at all, so a bound `b"\x01\x02"` fails with
  `inconvertible types: STRING -> BINARY [from='\x0102']`. One execute per row instead,
  which psqlodbc sends as a typed `PQexecPrepared` — and QuestDB takes the bytes. The
  cost is small here: 22.3k rows/s row-at-a-time against 23.9k with arrays forced on.

### `GetObjects` falls back to `SQLDescribeCol`

`SQLColumns` **fails** against QuestDB, with

```
HY000 Unrecognized return value from copy_and_convert_field. (8) (SQLColumns)
```

This one is a psqlodbc bug that QuestDB merely triggers. psqlodbc builds `SQLColumns`
from a `pg_catalog` query and, for a server that reports itself as PostgreSQL 10 or
newer, selects `pg_attribute.attidentity` — which it binds with a **NULL
`StrLen_or_IndPtr`**, so a NULL in that column is a hard error rather than a NULL value.
Real PostgreSQL stores `''` there; QuestDB's `pg_attribute` emulation has a `CHAR`
column, and QuestDB sends an empty `CHAR` as NULL. Nothing on the ADBC side can make that
call succeed.

`AppendColumns` therefore falls back, when `SQLColumns` fails outright, to describing the
result set of `SELECT * FROM <table> WHERE 1=0` with `SQLDescribeCol` — which is what
`GetTableSchema` has always done. The fallback is not keyed on any driver: it only ever
runs where the primary path already returned an error. It knows less than `SQLColumns`
(no remarks, no column default, no radix), and leaves those fields NULL rather than
guessing. It also drops one qualifier at a time from the table name, because QuestDB
rejects the catalog its own `SQLTables` reports:
`SELECT * FROM "qdb"."public"."adbc_t"` is an error, `"public"."adbc_t"` is fine.

### Notes

The entry needs two tolerance flags:

* `decimal_type="decimal128(28, 3)"` — QuestDB does not send the declared precision of a
  `DECIMAL` over the wire, so psqlodbc describes the column at its own maximum precision
  (28) with the column's scale.
* `not_null=("bo",)` — QuestDB's `BOOLEAN` has no NULL state, exactly like Access
  `YESNO`: the workload's all-NULL second row reads back with `bo` false.

Everything else passes unchanged: typed and NULL parameters, the emoji (QuestDB is
UTF-8 throughout), microsecond timestamps, `BINARY` round-trips, affected-row counts,
5000-row batched reads, and the "table does not exist [table=adbc_no_such_table]" error
text.

### Clean up

```sh
docker compose -f tests/compat/docker-compose.yml --profile extra down questdb
# or, if started standalone:
docker stop adbcbridge-questdb && docker rm adbcbridge-questdb
```

## Azure SQL Edge 16.0

Azure SQL Edge is the SQL Server 2022 engine shipped as a smaller container image for
edge deployments, so Microsoft's `msodbcsql 18` — the driver the `mssql` entry uses —
drives it unchanged. It is a separate entry rather than a note on `mssql` because it is
a separately built engine with a trimmed feature set; running the workload against it
directly is what shows the ODBC path is unaffected.

### Get the ODBC driver without root

The same driver as the `mssql` entry. If `msodbcsql18` is already installed system-wide
just point at it; otherwise unpack the Debian package anywhere:

```sh
mkdir -p /tmp/adbc-drivers && cd /tmp/adbc-drivers
# msodbcsql18_*.deb from packages.microsoft.com (ACCEPT_EULA=Y is baked into the .deb)
dpkg-deb -x msodbcsql18_*.deb msodbc
export AZURESQLEDGE_ODBC_DRIVER=$PWD/msodbc/opt/microsoft/msodbcsql18/lib64/libmsodbcsql-18.6.so.2.1
```

### Start the server

```sh
docker compose -f tests/compat/docker-compose.yml --profile extra up -d azuresqledge
# or standalone:
docker run -d --name adbcbridge-azuresqledge --memory=2g \
  -e ACCEPT_EULA=1 -e MSSQL_SA_PASSWORD="Adbc!Bridge2026" \
  -p 127.0.0.1:14332:1433 mcr.microsoft.com/azure-sql-edge:latest
```

Two differences from the `mssql` image worth noting: this one wants `ACCEPT_EULA=1`
(not `Y`), and TDS is published on `14332` so it can run alongside the `mssql` service
on `14331`. It takes about half a minute to come up and is ready when the log says so:

```sh
docker logs adbcbridge-azuresqledge 2>&1 | grep -q "ready for client connections"
```

### Run the entry

```sh
ADBC_ODBC_DRIVER=$PWD/build/libadbc_driver_odbc.so \
  .venv/bin/python tests/compat/test_matrix.py azuresqledge
# azuresqledge PASS  (Microsoft SQL Server (via ODBC) 16.00.5100)
```

### Quirks: none

The whole workload passes with no tolerance flags and no `OdbcDetectQuirks` entry: `INT`,
`FLOAT`, `NVARCHAR(50)`, `VARBINARY(10)`, `DATE`, `DATETIME2(6)`, `DECIMAL(10,3)` and
`BIT` all round-trip, including the astral-plane emoji, microsecond timestamps and NULL
parameters, and bulk ingest reports its row counts.

Note that Azure SQL Edge is indistinguishable from SQL Server through ODBC: it reports
`SQL_DBMS_NAME` "Microsoft SQL Server" and `SQL_DBMS_VER` `16.00.5100`, the same pair a
real SQL Server 2022 gives, so no quirk could be keyed on it even if one were needed.
`SELECT @@VERSION` is what tells them apart — it names "Microsoft Azure SQL Edge
Developer (RTM)".

## RisingWave 3

RisingWave is a streaming database (materialised views kept up to date incrementally over
streaming input). It speaks the PostgreSQL wire protocol and announces itself as
PostgreSQL 13, so the same `psqlodbc` build used for the `postgres` entry drives it —
there is no RisingWave ODBC driver.

### Get the ODBC driver without root

```sh
mkdir -p /tmp/adbc-drivers && cd /tmp/adbc-drivers
apt-get download odbc-postgresql
dpkg-deb -x odbc-postgresql_*.deb pgodbc
export RISINGWAVE_ODBC_DRIVER=$PWD/pgodbc/usr/lib/x86_64-linux-gnu/odbc/psqlodbcw.so
export LD_LIBRARY_PATH=$PWD/pgodbc/usr/lib/x86_64-linux-gnu:$LD_LIBRARY_PATH
```

### Start the server

```sh
docker compose -f tests/compat/docker-compose.yml --profile extra up -d risingwave
# or standalone:
docker run -d --name adbcbridge-risingwave --memory=2g -p 127.0.0.1:14566:4566 \
  -v $PWD/tests/compat/risingwave.toml:/risingwave.toml:ro \
  risingwavelabs/risingwave:latest single_node --config-path /risingwave.toml
```

`single_node` runs the meta, compute, frontend and compactor components in one process,
with an embedded SQLite meta store and the local filesystem as its object store — no
external MinIO or etcd. The compose service sits in the `extra` profile so a plain
`up -d` does not start it. It is ready in about half a minute, when the log line
`pgwire::pg_server: server started addr="0.0.0.0:4566"` appears:

```sh
docker logs adbcbridge-risingwave 2>&1 | grep "server started"
```

**The mounted config file is not optional.** `single_node` sizes each component from the
memory available to the process and gives the compactor an eighth of it (of which 80% is
usable, less a fixed 128 MB metadata cache); in a container small enough to run beside the
rest of the matrix that share lands below the compactor's own minimum, and the process
aborts at startup on

```
thread 'rw-standalone-compactor' panicked at src/storage/compactor/src/server.rs:124:9:
assertion failed: compactor_memory_limit_bytes > min_compactor_memory_limit_bytes as usize * 2
```

(exit 139, and it takes the whole standalone process with it, not just the compactor).
The minimum is twice one SST — the same log line reports `sstable_size_bytes 268435456`,
so ~537 MB — and it is a constant, so raising `--memory` is the wrong lever: 2 GB gives
the compactor 215 MB and 3 GB gives it 322 MB, both far short, and it would take a
container of roughly 7 GB for the derived share to clear the check.
`tests/compat/risingwave.toml` pins `storage.compactor_memory_limit_mb = 1024` instead. It is a cap on what compaction
may use, not an allocation — the container settles around 300 MB running the matrix.

### Run the entry

```sh
ADBC_ODBC_DRIVER=$PWD/build/libadbc_driver_odbc.so \
  .venv/bin/python tests/compat/test_matrix.py risingwave
# risingwave PASS  (PostgreSQL (via ODBC) 13.14.0)
```

### Notes

No driver quirk was needed: psqlodbc drives RisingWave the way it drives PostgreSQL, and
every part of the workload — typed parameters, NULLs, `BYTEA`, emoji, microsecond
timestamps, parameter arrays on bulk ingest, batched reads, `GetObjects`,
`GetTableSchema`, error text — works unchanged. Two things about the *server* shape the
entry:

* **`refresh`.** A row becomes visible to a scan only once the next barrier commits it,
  so a `SELECT` issued immediately after an `INSERT` or an `adbc_ingest` legitimately
  returns nothing — with the `refresh` key removed the entry fails at the first read,
  with zero rows, every time. The entry sets `refresh="FLUSH"` and `test_matrix.py` runs
  it after each write; `FLUSH` is RisingWave's own read-your-writes statement and waits
  for that barrier. It takes no table name, so the `"{}"` the other entries use for one
  simply goes unused. (`SET RW_IMPLICIT_FLUSH = true` is the session-level equivalent.)
* **No type modifiers.** RisingWave's parser accepts no precision, length or scale on a
  column type: `VARCHAR(50)` and `TIMESTAMP(6)` fail to parse (``expected ',' or ')'
  after column definition, found: (``) and `NUMERIC(10,3)` parses but is rejected
  (``unsupported data type: NUMERIC(10,3)``). The entry's DDL therefore declares `s` as
  `VARCHAR` and `n` as `NUMERIC`. This costs the *entry* nothing else: psqlodbc's own
  type names, which the generated ingest DDL uses (`int8`, `float8`, `bool`, `varchar`,
  `numeric`, `date`, `bytea`, `timestamp`), are all accepted unqualified, so unlike
  QuestDB this entry needs no `ansi_ddl_type_names` and no `ingest_types`.
* **`decimal_type`.** A `NUMERIC` with no declared precision or scale has none to report,
  so psqlodbc falls back to its own maximum precision (28) and RisingWave reports the
  scale of the values actually in the result set — 3 for the `12.345` the entry stores,
  1 for a `7.1`, and psqlodbc's default 6 for an empty result. The column arrives as
  `decimal128(28, 3)`; the value itself is exact.

Benchmark (`bench/matrix_bench.py --rows 10000 --fetch-rows 100000`): ingest 983 rows/s
row-at-a-time, 1,707 rows/s with parameter arrays (pyodbc `executemany`: 913 rows/s),
fetch 991k rows/s (pyodbc 476k). Ingest is slow in absolute terms because RisingWave is
built for streaming input rather than for `INSERT`, and each statement here waits for a
barrier; parameter arrays nearly double it, which is the largest relative gain of any
PostgreSQL-wire entry.

### Clean up

```sh
docker compose -f tests/compat/docker-compose.yml --profile extra down risingwave
# or, if started standalone:
docker rm -f adbcbridge-risingwave
```

## Microsoft Access (MDB Tools)

No server: MDB Tools reads an Access `.mdb`/`.accdb` file directly. Getting the driver
without root is two commands — download the Debian/Ubuntu packages and unpack them into
a directory of your own:

```sh
mkdir -p /tmp/adbc-access && cd /tmp/adbc-access
apt-get download odbc-mdbtools libmdb3t64 libmdbsql3t64 mdbtools
for f in *.deb; do dpkg-deb -x "$f" ex; done
```

`libmdbodbc*.so` needs `libmdb.so.3`/`libmdbsql.so.3` from the same set, so put their
directory on the loader path:

```sh
export LD_LIBRARY_PATH=/tmp/adbc-access/ex/usr/lib/x86_64-linux-gnu:$LD_LIBRARY_PATH
export ACCESS_ODBC_DRIVER=/tmp/adbc-access/ex/usr/lib/x86_64-linux-gnu/odbc/libmdbodbcW.so
```

Then, from the repository root:

```sh
ADBC_ODBC_DRIVER=$PWD/build/libadbc_driver_odbc.so \
  python tests/compat/test_matrix.py access
# access    PASS  (MDBTOOLS (via ODBC) 1.0.0)
```

Verified with `odbc-mdbtools` 1.0.0+dfsg-1.2ubuntu1 on Ubuntu 24.04. Both driver flavours
in the package pass: `libmdbodbcW.so` (Unicode entry points, the default above) and
`libmdbodbc.so` (ANSI). The `mdbtools` package is not required by the matrix — it only
brings the `mdb-*` command-line tools, which are handy for inspecting a fixture
(`mdb-tables`, `mdb-schema`, `mdb-export`).

### The fixture

MDB Tools is **read-only**: it executes no DDL and no DML whatsoever, and it cannot
create a database file. The matrix therefore runs the `access` entry with
`read_only=True` against `fixtures/access.mdb`, a Jet 4 (Access 2000) file *generated*
by `fixtures/MakeAccessMdb.java` — it is not copied from anywhere, so no third-party
licence attaches to it. `test_matrix.py` copies it into a temporary directory before
connecting, so a run never touches the checked-in file.

To regenerate it you need a JDK (17+, for the single-file source launcher) and
[Jackcess](https://jackcess.sourceforge.io/) (Apache-2.0), which is the only free
library that can *write* an Access file:

```sh
cd /tmp/adbc-access
for j in com/healthmarketscience/jackcess/jackcess/4.0.5/jackcess-4.0.5 \
         org/apache/commons/commons-lang3/3.14.0/commons-lang3-3.14.0 \
         commons-logging/commons-logging/1.3.0/commons-logging-1.3.0; do
  curl -sSLO "https://repo1.maven.org/maven2/$j.jar"
done
cd /path/to/adbcbridge
java -cp '/tmp/adbc-access/jackcess-4.0.5.jar:/tmp/adbc-access/commons-lang3-3.14.0.jar:/tmp/adbc-access/commons-logging-1.3.0.jar' \
     tests/compat/fixtures/MakeAccessMdb.java tests/compat/fixtures/access.mdb 3000
```

The trailing argument is the row count of the batch-crossing `adbc_big` table; it must
match the entry's `big_rows`.

Changing `V2000` to `V2016` in that file produces an `.accdb` (Access 2007+) instead.
MDB Tools reads those too, and the entry passes against one when pointed at it with
`ACCESS_CONN`:

```sh
sed 's/V2000/V2016/; s/MakeAccessMdb/MakeAccdb/g' \
    tests/compat/fixtures/MakeAccessMdb.java > /tmp/adbc-access/MakeAccdb.java
java -cp "$JACKCESS_CP" /tmp/adbc-access/MakeAccdb.java /tmp/adbc-access/access.accdb 3000
ACCESS_CONN='Driver={drv};DBQ=/tmp/adbc-access/access.accdb;' \
  python tests/compat/test_matrix.py access
```

The checked-in fixture stays `.mdb`: Jet 4 is the format MDB Tools supports most
completely, and the `.accdb` holding the same rows is more than twice the size
(456 kB against 196 kB).

### What works, and what MDB Tools cannot do

Working through adbcbridge, exactly as for the server-backed databases:

| | |
|---|---|
| types | `LONG`→`int32`, `DOUBLE`→`double`, `TEXT`→`string`, `LONGBINARY`→`binary`, `DATETIME`→`timestamp[us]`, `DECIMAL(10,3)`→`string`, `YESNO`→`bool` |
| NULLs | every nullable column round-trips as NULL |
| Unicode | UCS-2 → UTF-8, including accented Latin |
| `SELECT ... WHERE` | literal predicates, quoted identifiers, `COUNT(*)` |
| batched reads | 3000 rows across the reader's 1024-row batch boundary |
| metadata | `GetInfo`, `GetObjects` (filtered to the requested table), `GetTableSchema` |
| errors | surfaced as ADBC errors carrying the driver's message and native code |

Rough edges the matrix does not assert on: MDB Tools answers `SQLGetInfo` for
`SQL_DBMS_NAME` but not `SQL_DRIVER_NAME`, and its `SQLTables` ignores the special
"list the table types" call, so `GetTableTypes` returns one row per table
(`SYSTEM TABLE` five times, `TABLE` twice) rather than the distinct types. Errors reach
ADBC as `ADBC_STATUS_UNKNOWN` because the driver reports SQLSTATE `0000` for everything,
including a missing table.

The tolerances the entry sets, each for a limitation of MDB Tools or of the Access file
format itself:

| flag | why |
|---|---|
| `read_only=True` | no `CREATE`/`INSERT`/`UPDATE`/`DELETE` at all — its SQL parser rejects them, so the table and rows come from the fixture and the bulk-ingest steps are skipped. Its parser also has no `ORDER BY`, so the read-only path sorts client-side. |
| `params=False` | neither `SQLPrepare` nor `SQLBindParameter` is implemented ("Driver does not support this function"); the parameterised query runs with a literal instead. |
| `error_text=False` | an unknown table produces a bare `Couldn't parse SQL` naming neither the table nor the problem. |
| `astral=False` | the iconv conversion out of Jet's UCS-2 turns the emoji's surrogate pair into `??`. Latin-1 accents survive. |
| `ts_precision="s"` | Access `DATETIME` is a day fraction with one-second resolution; the 123456 µs of the fixture timestamp cannot be stored. |
| `not_null=("bo",)` | Access `YESNO` has no NULL state — the all-NULL row reads back `False`, not `None`. |

Three fixes in `src/` were needed to reach `PASS`; two of them reuse machinery that was
already there for other drivers.

- `SQLSetStmtAttr(SQL_ATTR_ROWS_FETCHED_PTR)` fails, so the driver never reported how
  many rows `SQLFetch` produced and every result set read as empty. The reader now
  notices that return code, drops to one row per fetch and counts one row per successful
  `SQLFetch`. Not keyed on a driver name — it is decided by what the driver answered.
- MDB Tools writes bound-column indicators four bytes wide, leaving the high half of our
  `SQLLEN` untouched: after `SQLFetch` a NULL column reads `0x……ffffffff`, so every NULL
  looked like a 4 GB value. That is the same shape as the Db2 CLI driver's 32-bit
  `SQLLEN`, so the existing `sqllen_32bit` quirk covers it — MDB Tools simply turns it
  on. (`adbc.odbc.sqllen_32bit` can still pin it either way by hand.)
- Its `SQLTables` ignores the `TableName` argument, so `GetObjects(table_name_filter=…)`
  returned the columns of every table in the file, `MSys*` system tables included.
  `GetObjects` already re-applied the catalog and schema patterns client-side for drivers
  that ignore those (SQLiteODBC); the table-name pattern is now enforced in the same
  place, for every driver.

The quirk lives in `OdbcDetectQuirks` (`src/odbc_driver.c`). Because MDB Tools does not
implement `SQL_DRIVER_NAME`, that function now falls back to `SQL_DBMS_NAME`
(`MDBTOOLS`) when `SQL_DRIVER_NAME` is unavailable; drivers that answer it are still
keyed on it. For the same reason the read-only option `adbc.odbc.driver_name` returns an
error on this driver rather than a name.

## OpenLink Virtuoso 7.2

Virtuoso is ODBC-native: port 1111 carries its own binary wire protocol and `virtodbc.so`
speaks it directly, so there is no separate client library and no `Database=` keyword —
`HOST` is `host:port` and the `dba` user lands in `DB.DBA`.

Server:

```sh
docker run -d --name adbcbridge-virtuoso --memory=2g \
  -e DBA_PASSWORD=adbc -p 127.0.0.1:11111:1111 openlink/virtuoso-opensource-7
```

(or `docker compose -f tests/compat/docker-compose.yml --profile extra up -d virtuoso`;
it is in the `extra` profile, so a plain `up -d` leaves it alone). It is ready in about
ten seconds — `Server online at 1111` in `docker logs`.

Driver — `libvirtodbc0` from the Ubuntu archive, unpacked without root:

```sh
mkdir -p /tmp/dbs/virtuoso && cd /tmp/dbs/virtuoso
curl -sLO http://archive.ubuntu.com/ubuntu/pool/universe/v/virtuoso-opensource/libvirtodbc0_7.2.12+dfsg-1build1_amd64.deb
dpkg-deb -x libvirtodbc0_7.2.12+dfsg-1build1_amd64.deb ex
# -> ex/usr/lib/x86_64-linux-gnu/odbc/virtodbc.so    (ANSI, the one to use)
#    ex/usr/lib/x86_64-linux-gnu/odbc/virtodbcu.so   (Unicode -- see below, do not use)
#    ...and the _r (reentrant) variants of both.
```

Pick the revision built against your distribution's OpenSSL: `-1build1` is the noble
(24.04) build and needs `libssl.so.3`; the current `-4ubuntu2` links `libssl.so.4`
(OpenSSL 3.5) and will not load on an OpenSSL 3.0 host. The pool index at that URL lists
what is available.

Run the entry:

```sh
export VIRTUOSO_ODBC_DRIVER=/tmp/dbs/virtuoso/ex/usr/lib/x86_64-linux-gnu/odbc/virtodbc.so
ADBC_ODBC_DRIVER=$PWD/build/libadbc_driver_odbc.so \
  python tests/compat/test_matrix.py virtuoso
# virtuoso  PASS  (OpenLink Virtuoso (via ODBC) 07.20.3243)
```

Entry notes: Virtuoso keeps unquoted identifiers in the case they were written (no
`ident` mapping), and `SQLColumns` matches a table name case-insensitively. Its type
names are its own — `NVARCHAR` is the wide string type (a plain `VARCHAR` is a byte
string), `DATETIME` is the microsecond timestamp, and `TIMESTAMP` is an unrelated "row
timestamp" the driver describes as a binary column. There is no `BOOLEAN`: Virtuoso uses
`SMALLINT` for one, so the entry expects `int16` for `bo`. No tolerance flags are needed:
the emoji, `DECIMAL(10,3)`, `VARBINARY`, NULL parameters, affected-row counts and the
5000-row batched read all round-trip.

`BIGINT` reads back as `decimal128(19, 0)` — Virtuoso describes it as `SQL_DECIMAL` with
precision 19 — which is what the bulk-ingest table's `a` column comes back as.

### Driver quirk 1: no `SQL_C_WCHAR`

`virtodbc.so` is a pure ANSI driver (it exports no `…W` entry points at all). It accepts
an `SQL_C_WCHAR` parameter and then reads the buffer as if it were narrow, so
`"héllo 🚀"` stores as its first byte pair, `"0\0"`, with no diagnostic. Its narrow path
is exact for UTF-8: Virtuoso's own charsets (`DB.DBA.SYS_CHARSETS`) are all single-byte
and an unqualified connection passes narrow bytes straight through, so UTF-8 text —
astral-plane emoji included — round-trips byte for byte through both `NVARCHAR` and
`VARCHAR`. adbcbridge detects the driver from `SQL_DRIVER_NAME` (`virtodbc.so`) and sets
the existing `wchar_as_utf8` reader option, the same one Firebird's OdbcFb needs.

Do **not** add `Charset=UTF-8` to the connection string. `UTF-8` is not one of Virtuoso's
charsets, so the server falls back to its single-byte default and the same UTF-8 bytes
then arrive as mojibake (`hÃ©llo ð…`); `Charset=UTF8` makes the driver abort the process
with `GPF: Dkbox.c:638 Double free`.

### Driver quirk 2: `SQL_C_SBIGINT` parameters store 0

A 64-bit integer parameter is stored as `0`, silently — the driver's conversion table has
no `SQL_C_SBIGINT`. The entry needs nothing for this: adbcbridge sets the existing
`bigint_param_as_string` option (Oracle's Instant Client ODBC needs the same one), which
sends the value as numeric text; the conversion is exact.

### Driver quirk 3: `SQL_C_TYPE_DATE` parameter arrays repeat row 0

`virtodbc.so` accepts `SQL_ATTR_PARAMSET_SIZE`, executes every set and reports the right
affected-row count, but binds a `SQL_C_TYPE_DATE` parameter from the *first* set only:
every row of the array is inserted with row 0's date. Other types (`SQL_C_CHAR`,
`SQL_C_DOUBLE`, `SQL_C_SLONG`) walk the array correctly, so the damage is invisible
unless a date column is part of the batch. adbcbridge sets `no_param_arrays`, executing
one row at a time as it does for DuckDB, clickhouse-odbc, OdbcFb and MonetDBODBClib.

### Why not the Unicode driver, `virtodbcu.so`

`virtodbcu.so` does support wide strings, and needs `WideAsUTF16=Y` in the connection
string to read `SQLWCHAR` as UTF-16 rather than as `wchar_t` — but it cannot be reached
through unixODBC's ANSI entry points, which is what adbcbridge (and any ANSI ODBC
application) calls. The first statement that *fails* aborts the process:

```
*** stack smashing detected ***: terminated
```

The crash is inside `libodbc.so.2`, in the ANSI→Unicode translation that follows a
failed `SQLExecDirect` — not in adbcbridge. This 40-line standalone probe reproduces it
with no ADBC in the picture (`cc -o vprobe vprobe.c -lodbc`):

```c
SQLDriverConnect(dbc, NULL, (SQLCHAR*)"Driver={…/virtodbcu.so};HOST=127.0.0.1:11111;"
                 "UID=dba;PWD=adbc;WideAsUTF16=Y;", SQL_NTS, NULL, 0, NULL, SQL_DRIVER_NOPROMPT);
SQLExecDirect(stmt, (SQLCHAR*)"DROP TABLE no_such_table_probe", SQL_NTS);  /* SIGABRT */
```

The same probe against `virtodbc.so` returns `SQL_ERROR` and the diagnostic
(`42S02 … SR268: No table in drop table.`) as it should, and pyodbc — which calls the
`…W` entry points directly, so unixODBC does not translate — drives `virtodbcu.so`
without trouble. So the fault is in that driver's Unicode diagnostic path meeting
unixODBC's translation layer, there is no connection handle or `reader_opts` flag that
could avoid it, and the ANSI driver has no such problem and loses nothing: use it.

Note that both builds report `SQL_DRIVER_NAME` as `virtodbc.so`, so the quirks above are
keyed on a name that matches either.

## openGauss 6.0

openGauss is Huawei's fork of PostgreSQL 9.2, so it speaks the PostgreSQL wire protocol
and the same `psqlodbc` build the `postgres` entry uses drives it unchanged — there is no
openGauss ODBC driver to fetch. It reports itself as `PostgreSQL` 9.2.4 over the wire.
Everything unusual about this entry is the *server*, not the driver.

### Get the ODBC driver without root

The same `psqlodbc` as `postgres`; if you already have `POSTGRES_ODBC_DRIVER` set, just
point the openGauss variable at it:

```sh
export OPENGAUSS_ODBC_DRIVER=$POSTGRES_ODBC_DRIVER
# or, from scratch:
mkdir -p /tmp/adbc-drivers && cd /tmp/adbc-drivers
apt-get download odbc-postgresql
dpkg-deb -x odbc-postgresql_*.deb pgodbc
export OPENGAUSS_ODBC_DRIVER=$PWD/pgodbc/usr/lib/x86_64-linux-gnu/odbc/psqlodbcw.so
export LD_LIBRARY_PATH=$PWD/pgodbc/usr/lib/x86_64-linux-gnu:$LD_LIBRARY_PATH
```

### Start the server

```sh
docker compose -f tests/compat/docker-compose.yml --profile extra up -d opengauss
# or standalone:
docker run -d --name adbcbridge-opengauss --memory=3g --shm-size=1g --cap-add=SYS_NICE \
  -p 127.0.0.1:15438:5432 -e GS_PASSWORD='Adbc@2026' enmotech/opengauss:latest \
  -c shared_buffers=256MB -c max_process_memory=2GB -c max_connections=50
```

Two flags are not optional:

* **`--cap-add=SYS_NICE`**. openGauss's MOT (memory-optimized table) engine allocates its
  recovery-manager arena with `mbind()`, which Docker's default seccomp profile rejects
  with `EPERM` unless the container holds `CAP_SYS_NICE`. Without it the postmaster dies
  a second into start-up and the container exits 1; `docker logs` shows only
  `gs_ctl: could not start server`, and the reason is in
  `/var/lib/opengauss/data/pg_log/postgresql-*.log`:
  `[Memory] mbind: Operation not permitted` → `Failed to allocate memory of 104857600
  size` → `[System] Failed to Initialize the Recovery Manager`.
* **the `-c` overrides and `--memory=3g`**. `gs_initdb` sizes the instance from *host*
  RAM: on a 31 GB box it writes `shared_buffers=1024MB` and takes `max_process_memory`
  to 12 GB, which a small container cannot back. `max_process_memory` has a hard lower
  bound of 2 GB (`1536000 is outside the valid range for parameter "max_process_memory"
  (2097152 .. 2147483647)`), so 3 GB is the smallest workable `--memory`; the server
  actually resides in about 550 MB.

Then create the role and database the matrix connects as — openGauss **refuses a remote
login for the initial user** (`gaussdb`), so `GS_PASSWORD` alone gets you nothing over
TCP:

```sh
docker exec -u omm adbcbridge-opengauss bash -lc "
  export GAUSSHOME=/usr/local/opengauss PATH=/usr/local/opengauss/bin:\$PATH
  export LD_LIBRARY_PATH=/usr/local/opengauss/lib:\$LD_LIBRARY_PATH
  gsql -d postgres -p 5432 -c \"CREATE USER adbc WITH SYSADMIN PASSWORD 'Adbc@2026';\"
  gsql -d postgres -p 5432 -c \"CREATE DATABASE adbc OWNER adbc DBCOMPATIBILITY 'PG';\"
"
```

* The password must satisfy openGauss's complexity rule (8+ characters from at least
  three of upper/lower/digit/special), hence `Adbc@2026`.
* `DBCOMPATIBILITY 'PG'` pins the SQL dialect. It is this image's default
  (`SHOW sql_compatibility` says `PG`) but worth spelling out, because openGauss's other
  mode, `A` (Oracle), redefines `DATE` as `timestamp(0) without time zone` — the entry's
  `d` column would then come back as a timestamp rather than a date.
* Authentication works because this image ships `password_encryption_type = 1`, which
  stores an MD5 verifier beside openGauss's own SHA-256 one, and its `pg_hba.conf` asks
  remote clients for `md5` — psqlodbc speaks MD5 and PostgreSQL's SCRAM, neither of
  which is openGauss's native SHA-256 scheme. A server set to
  `password_encryption_type = 2` cannot be reached by psqlodbc at all.

### Run the entry

```sh
export OPENGAUSS_ODBC_DRIVER=$POSTGRES_ODBC_DRIVER
ADBC_ODBC_DRIVER=$PWD/build/libadbc_driver_odbc.so \
python tests/compat/test_matrix.py opengauss
# opengauss PASS  (PostgreSQL (via ODBC) 9.2.4)
```

### Quirks

None. No driver quirk, no tolerance flag, and no `db_kwargs`: the entry is the `postgres`
entry with a different port and login. `INTEGER`, `DOUBLE PRECISION`, `VARCHAR`, `BYTEA`,
`DATE`, `TIMESTAMP`, `NUMERIC(10,3)` and `BOOLEAN` all round-trip, including the emoji,
the all-NULL row, typed parameters, microsecond timestamps, affected-row counts, 5000-row
batched reads, `GetObjects`/`GetTableSchema`, and the error text (`relation
"adbc_no_such_table" does not exist on gaussdb`). Parameter arrays work, so `adbc.odbc.array_binding=true` is roughly 4x faster on
ingest (see `bench/MATRIX_BENCHMARKS.md`).

### Clean up

```sh
docker compose -f tests/compat/docker-compose.yml --profile extra down opengauss
# or, if started standalone:
docker rm -f adbcbridge-opengauss
```

## Arrow Flight SQL (sqlflite 1.5.5 / DuckDB 1.1.1)

Arrow Flight SQL is a wire protocol, not a database: a server answers `CommandStatement*`
RPCs and hands back Arrow record batches. The one ODBC driver for it is the Arrow Flight
SQL ODBC driver Dremio publishes (`github.com/dremio/flightsql-odbc`, shipped as
`arrow-flight-sql-odbc-driver`), so everything the `flightsql` entry works around is that
driver's, not any one server's. The server used here is `voltrondata/sqlflite`, which
puts DuckDB behind a Flight SQL service.

Server:

```sh
docker run -d --name adbcbridge-flightsql --memory=2g \
  -e SQLFLITE_PASSWORD=adbc -e TLS_ENABLED=0 -e PRINT_QUERIES=0 \
  -p 127.0.0.1:31337:31337 voltrondata/sqlflite:latest
```

(or `docker compose -f tests/compat/docker-compose.yml --profile extra up -d flightsql`;
it is in the `extra` profile, so a plain `up -d` leaves it alone). It is ready in about a
second — `SQLFlite server - started` in `docker logs`. `SQLFLITE_PASSWORD` is mandatory:
without one the server exits at startup. The user name is sqlflite's own default,
`sqlflite_username`. The image ships a DuckDB database holding TPC-H at scale factor
0.01, which the entry's `extra` steps read.

Driver — the `.rpm` from Dremio's download site, unpacked without root (there is no
`.deb`; `7z` turns the RPM into a cpio archive, which `cpio` extracts into a prefix of
your choosing):

```sh
mkdir -p /tmp/dbs/flightsql && cd /tmp/dbs/flightsql
curl -sLO https://download.dremio.com/arrow-flight-sql-odbc-driver/arrow-flight-sql-odbc-driver-LATEST.x86_64.rpm
7z x -y arrow-flight-sql-odbc-driver-LATEST.x86_64.rpm     # -> *.cpio
mkdir -p ex && cd ex && cpio -idmu --no-absolute-filenames < ../*.cpio
# -> ex/opt/arrow-flight-sql-odbc-driver/lib64/libarrow-odbc.so.0.9.7.479
```

The library needs nothing beyond a stock glibc host (`ldd` reports no missing
dependencies — Arrow, gRPC and Protobuf are all linked in), so no `LD_LIBRARY_PATH` entry
is needed for it.

Run the entry:

```sh
export FLIGHTSQL_ODBC_DRIVER=/tmp/dbs/flightsql/ex/opt/arrow-flight-sql-odbc-driver/lib64/libarrow-odbc.so.0.9.7.479
ADBC_ODBC_DRIVER=$PWD/build/libadbc_driver_odbc.so \
  python tests/compat/test_matrix.py flightsql
# flightsql PASS  (sqlflite (via ODBC) 00.00.0000.duckdb v1.1.1)
```

Entry notes: the read side of the workload is exact. Every column type round-trips —
`BLOB` as bytes, `DATE`, microsecond `TIMESTAMP`, `BOOLEAN` as `bool`, and `"héllo 🚀"`
with its astral-plane emoji intact — and `SELECT`, the 100,000-row batched read,
`GetObjects`, `GetTableSchema` and the error path all behave. The write side does not
exist at all; see quirk 1.

### Driver quirk 1: no `SQLBindParameter`, so the entry is `read_only`

The driver answers `SQLBindParameter` with `HYC00 "Unsupported function"` on a *virgin*
statement handle, before any SQL has been seen:

```c
SQLAllocHandle(SQL_HANDLE_STMT, dbc, &stmt);
SQLINTEGER v = 42; SQLLEN ind = 4;
SQLBindParameter(stmt, 1, SQL_PARAM_INPUT, SQL_C_SLONG, SQL_INTEGER, 0, 0, &v, 0, &ind);
/* SQL_ERROR, HYC00 [Apache Arrow][Flight SQL] (100) Unsupported function. */
```

So nothing that binds a parameter can run: not the parameterised `INSERT` the other
entries load `adbc_t` with, and not `adbc_ingest`. `SQLPrepare` succeeds on a statement
containing `?`, but `SQLNumParams` then reports 0 markers and there is no way to supply a
value. (`SQLRowCount` also returns -1 after a literal `INSERT`, so there would be no
affected-row count to check either.)

This is not something adbcbridge can work around — parameter binding *is* the write path
— so the entry uses the existing `read_only` tolerance flag, as the `access` entry does
for MDB Tools. `SQLExecDirect` of literal SQL works fine, so the entry's `setup` builds
`adbc_t` (the same eight columns and the same two rows, spelled as literals) and
`adbc_big` (100,000 rows from DuckDB's `range()`), and the whole read side of the
workload then runs unchanged against them. `setup` is replayed on every connection —
`bench/matrix_bench.py` opens several — so each statement is `CREATE OR REPLACE`.

### Driver quirk 2: `SQLColumns` segfaults on the first `SQLFetch`

`SQLColumns` returns `SQL_SUCCESS` and describes all 18 result columns, and then the
first `SQLFetch` on that cursor segfaults inside the driver — with **no** bound columns at
all, so this is not a binding-width problem and there is nothing a caller can do
differently:

```c
SQLColumns(stmt, NULL, 0, NULL, 0, (SQLCHAR*)"adbc_t", SQL_NTS, NULL, 0);  /* SQL_SUCCESS */
SQLNumResultCols(stmt, &n);                                               /* n == 18 */
SQLFetch(stmt);                                                           /* SIGSEGV */
```

It happens for any table, including the server's own TPC-H tables, and whether or not the
catalog and schema are given (passing a catalog that matches nothing returns an empty
result set, which fetches without crashing — the crash is in producing the first row).
`SQLTables` and `SQLPrimaryKeys` are fine, and `SQLStatistics` is honestly reported as
`HYC00 Unsupported function`.

A crash leaves no return code to fall back on, so the call has to be skipped outright.
adbcbridge already has the fallback this needs: `AppendColumnsViaDescribe`
(`src/odbc_objects.c`) reads a table's columns off the result-set metadata of
`SELECT * FROM <table> WHERE 1=0`, which is where `GetTableSchema` gets them from anyway.
Until now it only ran after `SQLColumns` *failed*; the new `no_sql_columns` reader option
makes `AppendColumns` take it unconditionally. `OdbcDetectQuirks` (`src/odbc_driver.c`)
sets it from `SQL_DRIVER_NAME`, which this driver reports as
`Arrow Flight ODBC Driver`.

### Driver quirk 3: every `DECIMAL` is described as `(19, 0)`

The driver describes any `SQL_DECIMAL` column with precision 19 and scale 0 whatever was
declared — `DECIMAL(10,3)` and a `12.345::DECIMAL(10,3)` literal both come back that way
— while sending the digits themselves in full. Taken at face value that scale rounds
`12.345` to `12`, so the entry sets `adbc.odbc.decimal_as_string` and expects `n` as a
string, exactly as the `databend` entry does for the same reason.

### A trap that is not a quirk here: `SQL_C_WCHAR` and astral-plane characters

The driver's wide path throws on anything outside the BMP —
`SQLGetData(..., SQL_C_WCHAR, ...)` on a value containing `🚀` fails with
`HY000 wstring_convert::from_bytes` — while its narrow path is correct UTF-8. It costs
nothing here, because the driver describes DuckDB's `VARCHAR` as `SQL_VARCHAR` (not
`SQL_WVARCHAR`), so adbcbridge's reader is on the narrow path already and the emoji
round-trips. Should a Flight SQL server ever cause a column to be described as
`SQL_WVARCHAR`, the existing `wchar_as_utf8` option (Firebird's OdbcFb and Virtuoso both
need it) is the fix, added to the same `arrow flight` block in `OdbcDetectQuirks`.

## H2 (PostgreSQL mode) — does not work with psqlodbc

H2 has no ODBC driver of its own. Its server mode can speak the PostgreSQL v3 wire
protocol (`org.h2.tools.Server -pg`), so on paper the same `psqlodbc` build that drives
the `postgres`, `cockroachdb`, `yugabyte` and `timescaledb` entries should drive it too.
It does not: **psqlodbc cannot complete `SQLDriverConnect` against H2**, so there is no
`h2` entry in `test_matrix.py`. The details are below so the result can be re-checked
when either side changes.

### Start the server (no Docker, no root)

```sh
mkdir -p /tmp/dbs/h2/data && cd /tmp/dbs/h2
curl -LO https://repo1.maven.org/maven2/com/h2database/h2/2.4.240/h2-2.4.240.jar
java -Xmx1g -cp h2-2.4.240.jar org.h2.tools.Server \
  -pg -pgPort 15435 -ifNotExists -baseDir /tmp/dbs/h2/data
# PG server running at pg://127.0.0.1:15435 (only local connections)
```

`-ifNotExists` lets the first connection create the database named in the startup packet
(`adbc` below) under `-baseDir`. Without `-pgAllowOthers` H2 refuses non-loopback
clients, which is what the matrix wants. Port 15435 keeps it clear of the `postgres`
(15432), `yugabyte` (15433) and `timescaledb` (15434) entries.

### What fails

```sh
python - <<'EOF'
import os, pyodbc
pyodbc.connect("Driver=%s;Server=127.0.0.1;Port=15435;Database=adbc;Uid=adbc;Pwd=adbc;"
               % os.environ["POSTGRES_ODBC_DRIVER"])
EOF
# pyodbc.ProgrammingError: ('42001', '[42001] ERROR: Syntax error in SQL statement
#   "SET [*]extra_float_digits = 2" ... (110) (SQLDriverConnect)')
```

psqlodbc 16 sends one fixed batch as part of its connect handshake, before
`SQLDriverConnect` returns (the literal is in the driver binary):

```
SET DateStyle = 'ISO';SET extra_float_digits = 2;show transaction_isolation
```

H2's SQL parser accepts the first statement and rejects the other two — in **every** H2
`MODE`, PostgreSQL included:

| Statement | H2 2.4.240 |
|---|---|
| `SET DateStyle = 'ISO'` | OK |
| `SET extra_float_digits = 2` | `42001` syntax error — `SET` takes only H2's own setting names |
| `SHOW transaction_isolation` | `42000` syntax error — H2 2.x has no `SHOW` |

Nothing intercepts them: `PgServerThread.getSQL()` rewrites exactly two statements,
`show max_identifier_length` (to `CALL 63`) and `set client_encoding to …` (to
`set DATESTYLE ISO`); everything else goes straight to the H2 parser. And the failure is
fatal on the driver side — psqlodbc accepts only a successful result from that batch and
otherwise takes the error path out of the connect routine.

The server itself is fine, which is the point worth recording. A ~90-line raw
PostgreSQL v3 client (startup packet, cleartext auth, simple `Q` messages) talks to it
happily and reproduces the two errors in isolation:

```
OK   'SELECT 1'                      [['1']]
OK   'SELECT H2VERSION()'             [['2.4.240']]
FAIL 'SET extra_float_digits = 2'     42001 Syntax error in SQL statement …
FAIL 'SHOW transaction_isolation'     42000 Syntax error in SQL statement …
```

### Why there is no quirk for it

The repo's usual answer to a misbehaving driver is a keyed entry in `OdbcDetectQuirks`
(`src/odbc_driver.c`). That cannot apply here: the handshake runs *inside*
`SQLDriverConnect`, so adbcbridge never gets a connection handle to detect anything on,
and no `reader_opts` flag is reachable. Nor is it tunable from either end:

- psqlodbc has no option to skip or alter that batch. `Protocol=6.2/6.4/7.4`,
  `pqopt={options='-c extra_float_digits=2'}`, `ConnSettings`, `UseServerSidePrepare=0`
  and `Ksqo=0` were all tried and all fail identically. H2 announces `server_version`
  `8.2.23`, so psqlodbc always takes its ≥ 7.4 path.
- H2's PG server has only `-pgPort`, `-pgAllowOthers` and `-pgVirtualThreads`; there is
  no "ignore unknown settings" switch (`IGNORE_UNKNOWN_SETTINGS` does not exist in
  H2 2.x, and `MODE=PostgreSQL` does not change `SET`/`SHOW` parsing).

So this is a genuine H2-plus-psqlodbc incompatibility, not an adbcbridge gap. It becomes
testable if H2 teaches `PgServerThread.getSQL()` to swallow unknown `SET`/`SHOW` GUCs, or
if psqlodbc learns to tolerate a failure of that batch. Tested with H2 2.4.240 and
2.3.232 (identical `getSQL`) against psqlodbc 16.00.0000.

## TiDB 7.5

TiDB is MySQL-wire-protocol compatible, so it needs no ODBC driver of its own — the same
MySQL Connector/ODBC build used for the `mysql` entry drives it (see [MySQL 8](#mysql-8)
above for the root-free tarball, and for the `LD_PRELOAD` that `import pyarrow` makes
necessary).

### Start the server

```sh
docker compose -f tests/compat/docker-compose.yml --profile extra up -d tidb
# or standalone:
docker run -d --name adbcbridge-tidb --memory=2g \
  -p 127.0.0.1:14000:4000 pingcap/tidb:latest
```

The image runs a single `tidb-server` with its embedded **unistore** engine, so there is
no PD or TiKV to start and the whole cluster is one ~400 MB container (~530 MiB resident).
It is ready in a couple of seconds, when the log says `server is running MySQL protocol`:

```sh
docker logs adbcbridge-tidb 2>&1 | grep -m1 'running MySQL protocol'
```

SQL is on `127.0.0.1:14000`, user `root` with **no password**. The image creates no
`adbc` database and no non-root account, so the entry uses the built-in `test` database.

### Run the entry

```sh
export TIDB_ODBC_DRIVER=$MYSQL_ODBC_DRIVER   # the Connector/ODBC tarball's libmyodbc9w.so
LD_PRELOAD=/lib/x86_64-linux-gnu/libstdc++.so.6 \
ADBC_ODBC_DRIVER=$PWD/build/libadbc_driver_odbc.so \
  .venv/bin/python tests/compat/test_matrix.py tidb
# tidb      PASS  (MySQL (via ODBC) 8.0.11-TiDB-v7.5.1)
```

### Connector/ODBC needs `PLUGIN_DIR` here, but not for MySQL 8

TiDB creates `root` with **`mysql_native_password`**, while MySQL 8.4 defaults to
`caching_sha2_password`. `caching_sha2_password` is built into libmysqlclient;
`mysql_native_password` is a *loadable client-side plugin*, which Connector/ODBC 9 looks
for in the directory it was compiled with — `/usr/local/mysql/lib/plugin`, the generic
tarball's install prefix. Unpacked anywhere else, the connection fails before it starts:

```
[08004] [MySQL][ODBC 9.4(w) Driver]Authentication plugin 'mysql_native_password' cannot
be loaded: /usr/local/mysql/lib/plugin/mysql_native_password.so: cannot open shared
object file: No such file or directory (2059)
```

The tarball does ship the plugin, next to the driver in `lib/plugin/`. So the entry's
connection string ends in `{plugin_dir}`, which `conn_uri()` in `test_matrix.py` expands
to `PLUGIN_DIR=<dir of the driver>/plugin;` when that directory exists and to nothing when
it does not — a packaged (rpm/deb) install has no `plugin/` beside the driver and its
compiled-in default is already right. This is a packaging artefact of running the driver
from an unpacked tarball, not a driver bug and not something adbcbridge can detect: it
happens inside `SQLDriverConnect`.

### Notes

The entry needs **no tolerance flags and no driver quirk**. It is the `mysql` entry's DDL
and settings unchanged: `BOOLEAN` is `TINYINT(1)`, which the driver reports as
`SQL_TINYINT` (`bool_type="int8"`), and `adbc_ingest`'s double-quoted identifiers need
`ANSI_QUOTES` in `sql_mode` (the entry's `setup`). `VARBINARY(10)`, `DATETIME(6)`,
`DECIMAL(10,3)` and `DATE` behave as in MySQL, so the emoji round-trip, the microsecond
timestamp, NULL parameters, affected-row counts and the 5000-row batched ingest and read
all pass on the generic path.

TiDB is indistinguishable from MySQL by the identifiers a quirk could key on —
`SQL_DRIVER_NAME` is `libmyodbc9w.so` and `SQL_DBMS_NAME` is `MySQL`, exactly as for
MySQL 8 — and `SQL_DBMS_VER` is `8.0.11-TiDB-v7.5.1`: the MySQL version TiDB claims
compatibility with, followed by its own. Any future TiDB-specific quirk would have to key
on that `-TiDB-` marker (or on `SELECT tidb_version()`), never on the driver or DBMS name,
which real MySQL shares. This is the same situation as CockroachDB behind `psqlodbc`.

Two TiDB behaviours are worth knowing even though neither affects the workload: its
default `sql_mode` already contains `STRICT_TRANS_TABLES` and friends but not
`ANSI_QUOTES` (hence the same `setup` line as MySQL), and its DDL is *online* —
asynchronous schema change rather than a table lock — which the matrix exercises on every
run by dropping and immediately recreating the same table names, with no wait needed.

### Clean up

```sh
docker compose -f tests/compat/docker-compose.yml --profile extra down tidb
# or, if started standalone:
docker rm -f adbcbridge-tidb
```

## Materialize 26

Materialize is a streaming warehouse: a `MATERIALIZED VIEW` there is not a snapshot you
refresh but a dataflow kept incrementally up to date as its inputs change. It speaks the
PostgreSQL wire protocol, so the same `psqlodbc` build used for the `postgres` entry
drives it — there is no Materialize ODBC driver. Unlike QuestDB or CrateDB, the SQL layer
really is PostgreSQL's: the workload's DDL, the type names psqlodbc's `SQLGetTypeInfo`
puts in the generated ingest DDL, and the catalog queries all work unchanged.

### Get the ODBC driver without root

The same `psqlodbc` as the `postgres` entry; if you already exported
`POSTGRES_ODBC_DRIVER`, just point the Materialize variable at it:

```sh
export MATERIALIZE_ODBC_DRIVER=$POSTGRES_ODBC_DRIVER
```

Otherwise unpack it first (no root needed):

```sh
mkdir -p /tmp/adbc-drivers && cd /tmp/adbc-drivers
apt-get download odbc-postgresql
dpkg-deb -x odbc-postgresql_*.deb pgodbc
export MATERIALIZE_ODBC_DRIVER=$PWD/pgodbc/usr/lib/x86_64-linux-gnu/odbc/psqlodbcw.so
export LD_LIBRARY_PATH=$PWD/pgodbc/usr/lib/x86_64-linux-gnu:$LD_LIBRARY_PATH
```

### Start the server

```sh
docker compose -f tests/compat/docker-compose.yml --profile extra up -d materialize
# or standalone:
docker run -d --name adbcbridge-materialize --memory=2g \
  -p 127.0.0.1:16875:6875 materialize/materialized:latest
```

The single-node image needs no configuration: PostgreSQL wire is on `16875` with one
`materialize` superuser, no authentication and a `materialize` database. It takes about
half a minute to come up (it bootstraps its catalog and starts the `quickstart` cluster),
and settles at roughly 1.3 GB resident — the `--memory=2g` cap is comfortable. It is
ready when the last startup phase has logged:

```sh
until docker logs adbcbridge-materialize 2>&1 |
        grep -q 'envd serve: postamble complete'; do sleep 1; done
```

### Run the entry

```sh
MATERIALIZE_ODBC_DRIVER=$POSTGRES_ODBC_DRIVER \
ADBC_ODBC_DRIVER=$PWD/build/libadbc_driver_odbc.so \
  .venv/bin/python tests/compat/test_matrix.py materialize
# materialize PASS  (PostgreSQL (via ODBC) 9.5.0)
```

`GetInfo` reports `PostgreSQL 9.5.0`, because 9.5 is the wire version Materialize
advertises, and `SQL_DRIVER_NAME` is `psqlodbcw.so` — the same pair real PostgreSQL
gives. `SELECT version()` is what tells them apart:

```
PostgreSQL 9.5 on x86_64-unknown-linux-gnu (Materialize 26.38.1)
```

### Quirks

**`Protocol=7.4-0` in the connection string (required for large ingests).** This is a
psqlodbc setting, not a Materialize one. Once psqlodbc is inside a transaction it wraps
each further execute in a `SAVEPOINT` so it can roll back that one statement; Materialize
implements no `SAVEPOINT`, and the whole batch fails with

```
ERROR: Expected a keyword at the beginning of a statement, found identifier "savepoint" (42601)
```

psqlodbc splits a parameter array into a second batch once the statement text it inlines
the values into grows past its internal limit, so where that happens depends on how wide
the rows are: the matrix workload's 5000 narrow rows still go as one batch and pass
without the setting, while `bench/matrix_bench.py`'s wider rows split at about 4000 and do
not. The `questdb` entry sets `Protocol=7.4-0` for exactly the same reason.

**`NUMERIC` reads back as a string.** Materialize has a single arbitrary-precision
`numeric`: `NUMERIC(10,3)` keeps the scale but not the precision, and psqlodbc describes
the column at the type's own maximum of 39 digits. An Arrow `decimal128` tops out at 38,
so the reader falls back to the exact decimal string (`decimal_type="string"` in the
entry). No precision is lost — this is the same fallback the `sqlite` entry uses.

Everything else is stock PostgreSQL behaviour: `BYTEA`, `DATE`, microsecond `TIMESTAMP`,
`BOOLEAN`, astral-plane Unicode, parameter arrays, row counts and `GetObjects` all behave
as they do against `postgres`.

### What the entry checks beyond the standard workload

The plain workload would just be a slower duplicate of `postgres`, so the entry's `extra`
steps exercise the reason to run Materialize at all: they create a table and a
`MATERIALIZED VIEW` aggregating it, bulk-ingest through ADBC into the view's *input*, and
read the aggregate straight back. That passes only if the write really did flow through
the maintained dataflow — no refresh step anywhere.

### Clean up

```sh
docker compose -f tests/compat/docker-compose.yml --profile extra down materialize
# or, if started standalone:
docker rm -f adbcbridge-materialize
```

## MariaDB ColumnStore 23.02 (MariaDB 11.1)

MariaDB ColumnStore is a columnar storage engine *inside* an ordinary MariaDB server, so
the MariaDB Connector/ODBC build used for the `mariadb` entry drives it unchanged and no
new driver has to be downloaded. The engine is what the entry exercises: `adbc_t` is
declared `ENGINE=Columnstore`, and the entry's `setup` sets
`default_storage_engine = Columnstore` so every table the driver's generated ingest DDL
creates is columnar too. Without both, the workload would run against a plain InnoDB
MariaDB and never touch the engine.

Server (or use the `columnstore` service in `docker-compose.yml`, which is in the `extra`
profile so a plain `up -d` leaves it alone). `--hostname` must match `PM1`: the node
registers itself in the cluster under that name.

```sh
docker run -d --name adbcbridge-columnstore --hostname mcs1 \
  -e PM1=mcs1 -e MARIADB_ROOT_PASSWORD=adbc --memory=2g \
  -v $PWD/tests/compat/columnstore.cnf:/etc/my.cnf.d/zz-adbc.cnf:ro \
  -p 127.0.0.1:13313:3306 mariadb/columnstore:latest
```

The entrypoint starts `mariadbd` and the CMAPI server but **not** the ColumnStore backend
processes (`controllernode`, `PrimProc`, `DMLProc`, ...), and MariaDB answers on 3306
long before they exist. Until they are started every ColumnStore DDL fails with
`Internal error: Cannot execute the statement. DBRM is read only!` and the error log
fills with `connect() error: Connection refused ... port: 8616`. The image's `provision`
script is what starts them — it sets the CMAPI key, adds this node to the cluster and
restarts it (`mcs cluster node add` on its own answers
`Starting transaction isn't successful.` because the key has not been set):

```sh
docker exec adbcbridge-columnstore provision
# Adding PM(s) To Cluster ... done
# Restarting Cluster ... done
# Validating ColumnStore Engine ... done
```

The image honours `MARIADB_ROOT_PASSWORD` but not `MARIADB_DATABASE`/`MARIADB_USER`, and
the account it creates is `root@localhost`, which a client coming in through the
published port cannot match. Create the database and a `%` user by hand. The password
has to get past `cracklib_password_check`, which this image loads: `adbc` is rejected
with `Your password does not satisfy the current policy requirements`.

```sh
docker exec adbcbridge-columnstore mariadb -uroot -padbc -e "
  CREATE DATABASE adbc;
  CREATE USER 'adbc'@'%' IDENTIFIED BY 'Adbc!Bridge2026';
  GRANT ALL ON *.* TO 'adbc'@'%';"
```

`tests/compat/columnstore.cnf` (mounted above) carries two server settings the entry
depends on, both of which have to be set at startup:

* `character_set_server = utf8mb4`. The image ships `utf8mb3`, which cannot hold the
  astral-plane emoji the matrix writes — `INSERT` fails with
  `Incorrect string value: '\xF0\x9F\x9A\x80' for column 's'`. The `adbc` database
  inherits it, and so do `adbc_t` and every ingest table.
* `columnstore_cache_inserts = ON`. ColumnStore's bulk-load path is `cpimport`; an
  `INSERT` carrying *bound parameters* goes to `DMLProc` a row at a time, each row
  committing its own extent. Measured through plain pyodbc, with no adbcbridge involved,
  that is **~2 rows/s** — a 2000-row ingest takes a quarter of an hour, whether the rows
  go as an ODBC parameter array (MariaDB's `COM_STMT_BULK_EXECUTE`) or one execute per
  row. The same rows as one literal `INSERT ... VALUES (...),(...)` run at ~350 rows/s,
  which is the batch path. `columnstore_cache_inserts` buffers the parameterised rows in
  the UM and writes them out in one batch, and the whole matrix workload then runs in
  three seconds. The variable is read-only at run time (`Variable 'columnstore_cache_inserts'
  is a read only variable`), so it cannot go in the entry's `SET SESSION` setup.

Run the entry — same driver library as `mariadb`:

```sh
export COLUMNSTORE_ODBC_DRIVER=$MARIADB_ODBC_DRIVER  # MariaDB Connector/ODBC libmaodbc.so
ADBC_ODBC_DRIVER=$PWD/build/libadbc_driver_odbc.so \
python tests/compat/test_matrix.py columnstore
# columnstore PASS  (MariaDB (via ODBC) 11.01.000001)
```

Entry notes. The wire is MariaDB's, so the `mariadb` entry's tolerances carry over:
`ANSI_QUOTES` in `sql_mode` for the double-quoted identifiers `adbc_ingest` emits, and
`BOOLEAN` stored as `TINYINT(1)` and reported as `SQL_TINYINT`, so `bo` is expected as
`int8`. The type system is not MariaDB's, though:

* **No `VARBINARY`.** `CREATE TABLE` is refused outright with `Varbinary is currently not
  supported by Columnstore`, so `b` is `BLOB`, which round-trips the two bytes fine. No
  tolerance flag is needed — it is a DDL choice, not a lost capability.
* **`CHAR`/`VARCHAR` cap at 8000 bytes** (`char, varchar and varbinary length may not
  exceed 8000 bytes`); `TEXT`/`MEDIUMTEXT` are the types above that.

Everything else in the workload passes as it does on MariaDB: the emoji round-trip,
microsecond `DATETIME(6)`, `DECIMAL(10,3)`, NULL parameters, affected-row counts, the
5000-row batched read, `GetObjects` and the error path.

One driver quirk was needed, in `OdbcDetectQuirks` (`src/odbc_driver.c`), keyed on
`maodbc` **plus** the server actually having the engine:

```sql
SELECT COUNT(*) FROM information_schema.engines
 WHERE engine = 'Columnstore' AND support IN ('YES', 'DEFAULT')
```

ColumnStore's DDL parser accepts only its own list of type names, and two of the names
`maodbc`'s `SQLGetTypeInfo` answers with are not on it: `SQL_LONGVARCHAR` is
`LONG VARCHAR` and `SQL_BIT` is `BIT`. Both are refused with `The syntax or the data
type(s) is not supported by Columnstore`, even though the underlying types (`MEDIUMTEXT`,
`TINYINT`) do exist there — so the ingest DDL the driver generates,
`CREATE TABLE "adbc_ing" ("a" BIGINT, "b" LONG VARCHAR, "c" DOUBLE, "d" DATE, "e" BIT)`,
fails on the string and boolean columns. The standard spellings `TEXT` and `BOOLEAN` are
accepted, which is exactly what the existing `ansi_ddl_type_names` flag (added for
QuestDB) produces, so that is what the quirk sets. The driver name alone could not carry
it: `maodbc` also drives plain MariaDB, where `LONG VARCHAR` and `BIT` are fine — hence
the extra question to the server, in the same shape as the `version()` probe psqlodbc
gets. Turning ANSI type names on for an InnoDB table on such a server costs nothing;
MariaDB accepts `TEXT` and `BOOLEAN` just as readily.

Benchmarks (`bench/matrix_bench.py --rows 10000 --fetch-rows 100000`): fetch 1.41M
rows/s (pyodbc 452k/s), ingest 14.9k rows/s and 54.6k rows/s with
`adbc.odbc.array_binding=true` (pyodbc `fast_executemany` 28.8k/s) — all of that with
`columnstore_cache_inserts = ON`; without it ingest is ~2 rows/s.

## libSQL server (sqld) — no PostgreSQL wire protocol, so no ODBC route

libSQL is Turso's fork of SQLite, and `sqld` (the `ghcr.io/tursodatabase/libsql-server`
image) is its server. It was queued for the matrix on the assumption that it still
exposed a PostgreSQL wire listener — `SQLD_PG_LISTEN_ADDR` / `--pg-listen-addr` — which
`psqlodbc` could drive the way it drives the `cockroachdb`, `yugabyte`, `timescaledb`,
`cratedb` and `questdb` entries. **That listener no longer exists**, so there is no
`libsql` entry in `test_matrix.py` and no service in `docker-compose.yml`.

sqld speaks only its own HTTP/JSON protocol (Hrana) and gRPC. There is no libSQL ODBC
driver, and the SQLite ODBC driver used by the `sqlite` entry opens local files through
`libsqlite3` — it has no client for a remote sqld.

### Reproducing the check

```sh
docker run -d --name adbcbridge-libsql --memory=2g \
  -e SQLD_PG_LISTEN_ADDR=0.0.0.0:5432 -e SQLD_HTTP_LISTEN_ADDR=0.0.0.0:8080 \
  -p 127.0.0.1:15437:5432 -p 127.0.0.1:18086:8080 \
  ghcr.io/tursodatabase/libsql-server:latest
```

The env var is accepted and silently ignored — the startup banner lists only the two
listeners it does have, and `SQLD_PG_LISTEN_ADDR` appears nowhere in the config it
prints:

```
config:
	- mode: primary (0.0.0.0:5001)
	- database path: iku.db
	- listening for HTTP requests on: 0.0.0.0:8080
INFO sqld: listening for incoming user HTTP connection on 0.0.0.0:8080
INFO sqld: listening for incoming gRPC connection on 0.0.0.0:5001
```

Nothing inside the container listens on 5432 (only 5001 and 8080), so the published host
port is a `docker-proxy` socket with no server behind it: a PostgreSQL v3 startup packet
sent to `127.0.0.1:15437` gets `ECONNRESET` rather than an authentication request. There
is no point running `psqlodbc` against it — `SQLDriverConnect` cannot get further than
that reset.

The server itself is healthy, which is what makes this a missing feature rather than a
broken image. Its own protocol answers on 8080:

```sh
curl -s -X POST http://127.0.0.1:18086/v2/pipeline -H 'Content-Type: application/json' \
  -d '{"requests":[{"type":"execute","stmt":{"sql":"SELECT sqlite_version()"}},{"type":"close"}]}'
# {"results":[{"type":"ok","response":{"type":"execute","result":{"cols":[{"name":"sqlite_version()"...
#   "rows":[[{"type":"text","value":"3.47.0"}]] ...
```

### It is not a matter of picking another tag

`sqld --help` lists no `--pg-listen-addr` (or any other `pg`/`postgres` option), and the
binary contains no such string at all, while a flag it does have is there for comparison:

```sh
docker run --rm --entrypoint sh ghcr.io/tursodatabase/libsql-server:latest -c '
  grep -a -c "SQLD_HRANA_LISTEN_ADDR" /bin/sqld           # 1  (control)
  grep -a -c -i -E "pg_listen|pg-listen|pgwire" /bin/sqld # 0'
```

Every tag published in that registry is the same in this respect: the oldest one there,
`v0.22.0` (sqld 0.22.0, 2023-11-08), already has no PG wire either — the code was
dropped upstream before the first tag the registry still carries. Tested with sqld
0.24.33 (`latest`) and 0.22.0.

This row moves back out of "Known not to work" if sqld ever restores a PostgreSQL wire
listener, or if a libSQL ODBC driver appears; the entry would then look like the other
PG-wire entries, with SQLite's type affinity driving the tolerance flags (the `sqlite`
entry's `decimal_type="string"` is the likely starting point).

## MatrixOne 4.2

MatrixOne is a hyper-converged (HTAP) database that speaks the MySQL wire protocol — it
announces itself as `8.0.30-MatrixOne-v4.2.0` — so it needs no ODBC driver of its own:
the same MySQL Connector/ODBC build used for the `mysql` entry drives it (see
[MySQL 8](#mysql-8) above for the root-free tarball, and for the `LD_PRELOAD` that
`import pyarrow` makes necessary).

### Start the server

```sh
docker compose -f tests/compat/docker-compose.yml --profile extra up -d matrixone
# or standalone:
docker run -d --name adbcbridge-matrixone --memory=2g \
  -p 127.0.0.1:16001:6001 matrixorigin/matrixone:latest
```

One container holds the whole cluster (log service, TN and CN in one process); it is
ready in about 20 s and settles at ~800 MiB resident under the 2 GB cap. Its log has no
single "ready" line, so wait for the MySQL handshake on the port instead:

```sh
until timeout 1 bash -c 'exec 3<>/dev/tcp/127.0.0.1/16001; head -c 40 <&3' \
        | grep -qa MatrixOne; do sleep 2; done
# [<NUL>8.0.30-MatrixOne-v4.2.0 ...
```

SQL is on `127.0.0.1:16001`, with the image's built-in `dump` / `111` account. There is
no user database at all (only `mo_catalog`, `system`, `mysql`, …), so the entry's `setup`
runs `CREATE DATABASE IF NOT EXISTS adbc` and `USE adbc` — both idempotent, which matters
because `bench/matrix_bench.py` replays `setup` on every connection it opens — and the
connection string names no database.

### Run the entry

```sh
export MATRIXONE_ODBC_DRIVER=$MYSQL_ODBC_DRIVER   # the Connector/ODBC tarball's libmyodbc9w.so
LD_PRELOAD=/lib/x86_64-linux-gnu/libstdc++.so.6 \
ADBC_ODBC_DRIVER=$PWD/build/libadbc_driver_odbc.so \
  .venv/bin/python tests/compat/test_matrix.py matrixone
# matrixone PASS  (MySQL (via ODBC) 8.0.30-MatrixOne-v4.2.0)
```

`PLUGIN_DIR` is needed here for the same reason as for TiDB and Dolt: MatrixOne offers
only `mysql_native_password`, whose *client-side* plugin Connector/ODBC 9 loads at run
time from its compiled-in `/usr/local/mysql/lib/plugin`. The entry's connection string
ends in `{plugin_dir}`, which `conn_uri()` expands to the tarball's own `lib/plugin`
when that directory exists — see [TiDB](#tidb-75) for the full story.

### Quirks

Everything else in the standard workload runs on the generic path: the emoji round-trip,
`VARBINARY(10)`, `DATETIME(6)` microseconds, `DECIMAL(10,3)`, NULL parameters,
affected-row counts and the 5000-row batched ingest and read. `BOOLEAN` is `TINYINT(1)`
as in MySQL (`bool_type="int8"`) and `adbc_ingest`'s double-quoted identifiers need
`ANSI_QUOTES` in `sql_mode`, exactly as for MySQL. Three things are MatrixOne's own.

**A table with no PRIMARY KEY gets a hidden column.** MatrixOne synthesises
`__mo_fake_pk_col` (a `BIGINT UNSIGNED` auto-increment) for a table declared without a
key, and `information_schema.columns` — hence `SQLColumns`, hence `GetObjects` and
`GetTableSchema` — reports it as a 9th column, although `SELECT *` never returns it. The
entry declares `i INT PRIMARY KEY`, the same fix the `cockroachdb` entry uses for the
synthesised `rowid` there.

**A parameter bound into a `BIT` column takes the server down.** This is not a driver
problem: it reproduces with plain pyodbc, and the failure is a *server abort*, not an
error return.

```
malloc(): unaligned fastbin chunk detected
SIGABRT: abort
... github.com/matrixorigin/matrixone/pkg/common/malloc._Cfunc_calloc
```

Against `CREATE TABLE t (v bit)`, an inserted literal (`VALUES (1)`, `VALUES (NULL)`)
is stored and read back correctly, but over the binary protocol a bound integer `1` fails
with `data out of range: data type bit(1), value 1 (1690)`, a bound `NULL` is silently
stored as *false*, and a mixed batch of the three corrupts the server's heap and kills the
process — every connection drops with `Lost connection to MySQL server during query`.
Connector/ODBC's `SQLGetTypeInfo` names `BIT` for a boolean, so the ingest DDL asks for
exactly that column type. The entry therefore sends the boolean column as `int8` →
`TINYINT`, which MatrixOne handles correctly, with the same `ingest_types` mechanism the
`cratedb` entry uses for its missing `DATE`:

```python
ingest_types={pa.bool_(): pa.int8()},
```

That keeps create/append/replace ingest under test. MatrixOne's own `BOOL` column type is
unaffected — `adbc_t`'s `bo BOOLEAN` round-trips fine; it is `BIT` specifically that is
broken.

**It describes a TEXT column as five characters** (`SQLDescribeCol` reports
`SQL_WLONGVARCHAR`, column size 5, octet length 0) no matter how long its values are —
while a `VARCHAR(50)` result column comes back the other way round, with the type maximum
4,294,967,295. The over-large half was already handled (`long_bind_bytes`); the too-small
half was not, and it is much more expensive: bound at five characters, *every* row of a
`long varchar` column comes back truncated and has to be re-read with `SQLGetData`, which
read 100,000 rows at **3.2k rows/s** — slower than not binding at all. So
`ApplyBindWidth()` in `src/odbc_reader.c` now treats a small width on a no-declared-length
column (`SQL_LONGVARCHAR` / `SQL_WLONGVARCHAR` / `SQL_LONGVARBINARY`) the same way it
already treated an implausibly large one: the driver's number is a guess either way, so
the column is bound at `long_bind_bytes` and only values that outgrow *that* are re-read.
The same read then runs at **2.05M rows/s**. This is a generic reader fix, keyed on the
SQL type rather than on a driver name; it cannot make any other database's binding
narrower, since it only ever raises a width.

### Benchmark

```
ADBC_MATRIX_SUFFIX=_matrixone .venv/bin/python bench/matrix_bench.py \
  --rows 10000 --fetch-rows 100000 matrixone
# fetch=2,045,021/s (pyodbc 900,455/s)  ingest=4,115/s array=4,356/s pyodbc=4,213/s
```

Ingest is slow in absolute terms and identical for all three clients (~4.2k rows/s), so it
is the server, not the binding path: MatrixOne commits a transaction per `INSERT` into its
log service, and parameter arrays do not become a multi-row `INSERT` on this wire.

### Clean up

```sh
docker compose -f tests/compat/docker-compose.yml --profile extra down matrixone
# or, if started standalone:
docker rm -f adbcbridge-matrixone
```

## IBM Informix 15 (developer edition)

Informix answers DRDA — the wire protocol Db2 speaks — on a second listener, the
`<server>_dr` alias the image puts on port 9089. So the driver is the one the `db2`
entry already uses, IBM's freely downloadable **clidriver** `libdb2.so`: no new download,
just point `INFORMIX_ODBC_DRIVER` at it. (Informix's own CSDK ODBC driver is a licensed
SDK; this entry does not need it.)

Server:

```sh
docker run -d --name adbcbridge-informix --hostname informix --memory=2g \
  -e LICENSE=accept -e GL_USEGLU=1 -e DELIMIDENT=y \
  -p 127.0.0.1:19089:9089 icr.io/informix/informix-developer-database:latest
```

or `docker compose -f tests/compat/docker-compose.yml --profile extra up -d informix`.

Three things about that command are load-bearing:

* **No `--privileged`.** IBM's quickstart runs this image privileged; do not. In a
  privileged container `sudo` inside it fails PAM account management
  (`sudo: PAM account management error: Authentication service cannot retrieve
  authentication info`), and the entrypoint routes *every* setup step through `sudo`
  (`RUNAS`/`SED` in `/opt/ibm/scripts/informix_functions.sh`). The `$ONCONFIG` and
  `sqlhosts` edits are then silently skipped, `oninit` dies with `Bad DBSERVERNAME`,
  and the log loops on `Waiting for sysadmin` forever. Unprivileged, the same image
  initialises in about a minute.
* **`GL_USEGLU=1`** switches the server's Unicode handling to ICU. Without it a
  four-byte UTF-8 character — the matrix stores `"héllo 🚀"` — is rejected on the way in
  (`-202 An illegal character has been found in the statement` on the narrow path,
  `-415 Data conversion error` on the wide one). Two- and three-byte characters work
  either way, so this only shows up on the emoji.
* **`DELIMIDENT=y`** makes the server read `"..."` as a delimited identifier instead of
  a string literal. `adbc_ingest` quotes the names it generates, and without this the
  generated `CREATE TABLE "adbc_ing" ("a" BIGINT, ...)` is `-201 A syntax error has
  occurred`. On a DRDA session this has to be set in the **server's** environment: the
  client-side `DELIMIDENT` belongs to Informix's own CSDK, and Informix has no
  `SET ENVIRONMENT` for it the way MySQL has `sql_mode`.

Then create the database. The image ships no user database, and the locale it would
default to is `en_US.819` (ISO-8859-1), which cannot hold the workload's emoji:

```sh
docker exec adbcbridge-informix bash -lc '
  . /opt/ibm/scripts/informix_inf.env
  export DB_LOCALE=en_us.utf8 CLIENT_LOCALE=en_us.utf8
  printf "CREATE DATABASE adbc WITH LOG;\n" > /tmp/c.sql && dbaccess - /tmp/c.sql'
```

`WITH LOG` is required: an unlogged Informix database cannot be reached over DRDA at all.

Run the entry (the same `LD_LIBRARY_PATH` the `db2` entry needs — `libdb2.so` loads its
siblings from there):

```sh
export INFORMIX_ODBC_DRIVER=/tmp/dbs/db2/clidriver/lib/libdb2.so
LD_LIBRARY_PATH=/tmp/dbs/db2/clidriver/lib:$LD_LIBRARY_PATH \
ADBC_ODBC_DRIVER=$PWD/build/libadbc_driver_odbc.so \
python tests/compat/test_matrix.py informix
# informix  PASS  (IDS/UNIX64 (via ODBC) 12.10.0000)
```

`12.10.0000` is the DRDA compatibility level the listener reports, not the server
version — the image is Informix 15.0.1.

### Quirks

`libdb2.so` reports `SQL_DRIVER_NAME` "libdb2.a" whether it is talking to Db2 or to
Informix, so the Informix workarounds cannot be keyed on the driver name; the driver asks
`SQL_DBMS_NAME` instead, which is `IDS/UNIX64` here and `DB2/LINUXX8664` for Db2. Two
things then differ from Db2:

* **`SQL_C_WCHAR` parameters.** Informix converts UTF-16 in the server and gives up on a
  surrogate pair: the emoji fails the `INSERT` with `-415 Data conversion error`. The
  driver sends character parameters on the narrow (UTF-8) path instead
  (`reader_opts.wchar_as_utf8`, as for Firebird and Virtuoso).
* **`SQL_C_BIT` parameters.** Binding one against an Informix `BOOLEAN` corrupts the DRDA
  conversation itself — `SQL30020N`, "syntax error in the communication data stream" —
  and the connection is unusable afterwards. Booleans go as integers instead
  (`reader_opts.bool_param_as_int`).

The 32-bit `SQLLEN` of the clidriver applies here exactly as it does for Db2, and is
detected the same way (`adbc.odbc.sqllen_32bit`).

Two things are the server's, not the driver's, and are handled by the entry's tolerance
flags rather than a quirk:

* Informix's finest timestamp is `DATETIME YEAR TO FRACTION(5)` — five fractional
  digits — so `13:45:10.123456` is stored as `.12345` (`ts_us=(123450,)`).
* Informix `BOOLEAN` has no DRDA counterpart and is described as `SMALLINT`, so the
  column reads back as `int16` with `1` for true (`bool_type="int16"`).

For binary the entry declares `BYTE`, Informix's byte-string type (there is no
`VARBINARY`; the sibling `BLOB` is a smart large object and needs an sbspace the image
does not configure). The clidriver describes it with IBM's own `SQL_BLOB` type code
`-98`, which the reader now treats as a binary column — left unrecognised it fell through
to the text default, where the driver hands the bytes back hex-encoded (`"0102"`).

## GreptimeDB 1.1

[GreptimeDB](https://github.com/GreptimeTeam/greptimedb) is a time-series database that
serves **both** wire protocols the matrix already drives: the MySQL one on 4002 and the
PostgreSQL one on 4003. Only the MySQL one is usable — psqlodbc cannot complete
`SQLDriverConnect` against the PostgreSQL port at all (see below) — so the entry is
driven by MySQL Connector/ODBC, the same driver as `mysql`, `tidb`, `dolt`, `databend`
and `matrixone`.

### Start the server

It is behind the `extra` compose profile, so a plain `up -d` does not start it:

```sh
docker compose -f tests/compat/docker-compose.yml --profile extra up -d greptimedb
```

or by hand:

```sh
docker run -d --name adbcbridge-greptimedb --memory=1g \
  -p 127.0.0.1:14002:4002 -p 127.0.0.1:14003:4003 \
  greptime/greptimedb:latest standalone start \
  --rpc-bind-addr 0.0.0.0:4001 --http-addr 0.0.0.0:4000 \
  --mysql-addr 0.0.0.0:4002 --postgres-addr 0.0.0.0:4003
```

The image needs no setup: it is ready in a second or two with a `greptime` catalog, a
`public` schema, one passwordless `greptime` user and no authentication. 1 GB is
comfortable for this workload.

### Run the entry

The driver is the same MySQL Connector/ODBC tarball as the `mysql` entry (see that
section for the download and for the `LD_PRELOAD` that `pyarrow` forces on it), so there
is no new driver to extract:

```sh
export GREPTIMEDB_ODBC_DRIVER=/tmp/dbs/mysql/mysql-connector-odbc-9.4.0-linux-glibc2.28-x86-64bit/lib/libmyodbc9w.so
LD_PRELOAD=/lib/x86_64-linux-gnu/libstdc++.so.6 \
ADBC_ODBC_DRIVER=$PWD/build/libadbc_driver_odbc.so \
python tests/compat/test_matrix.py greptimedb
# greptimedb PASS  (MySQL (via ODBC) 8.4.2)
```

It announces itself as `8.4.2-GreptimeDB-1.1.4` over the MySQL wire and, like Databend,
reports `SQL_TXN_CAPABLE` = `SQL_TC_NONE`, so the driver's existing "MySQL
Connector/ODBC in front of a non-MySQL" quirks (`temporal_binary_param_as_varchar`,
`ansi_ddl_type_names`) already apply to it unchanged.

### The PostgreSQL wire does not work with psqlodbc

The same shape of failure as H2 (below), and for the same reason — psqlodbc's connect
handshake, not anything adbcbridge does:

```sh
python - <<'EOF'
import os, pyodbc
pyodbc.connect("Driver=%s;Server=127.0.0.1;Port=14003;Database=public;Uid=greptime;"
               % os.environ["POSTGRES_ODBC_DRIVER"])
EOF
# pyodbc.DataError: ('22023', '[22023] ERROR: Unsupported show variable:
#   TRANSACTION_ISOLATION (110) (SQLDriverConnect)')
```

psqlodbc 16 sends one fixed batch inside `SQLDriverConnect` (the literal is in the driver
binary): `SET DateStyle = 'ISO';SET extra_float_digits = 2;show transaction_isolation`.
A short raw PostgreSQL v3 client shows GreptimeDB takes the first two statements and
refuses the third:

| Statement | GreptimeDB 1.1.4 |
|---|---|
| `SET DateStyle = 'ISO'` | OK |
| `SET extra_float_digits = 2` | OK |
| `show transaction_isolation` | `22023` `Unsupported show variable: TRANSACTION_ISOLATION` |
| `SHOW TRANSACTION ISOLATION LEVEL` | OK — `read committed` |

So the value exists; GreptimeDB simply does not implement the `transaction_isolation`
GUC spelling that psqlodbc asks for (nor `max_identifier_length`, nor `server_version`).
The failure is fatal on the driver side, and neither end has a knob for it:
`Protocol=6.2/6.3/6.4/7.4-0/7.4-1/7.4-2`, `pqopt={options='…'}`, `ConnSettings=`,
`Ksqo=0`, `UseDeclareFetch=0` and `UseServerSidePrepare=0` were all tried and all fail
identically, and the server's only PostgreSQL-side switch is the listen address. It
becomes testable if GreptimeDB adds the GUC or psqlodbc tolerates a failure of that
batch. As with H2, no quirk in `OdbcDetectQuirks` could help: the handshake runs *inside*
`SQLDriverConnect`, so adbcbridge never gets a connection handle.

### Connection string: `NO_SSPS=1` and `PLUGIN_DIR`

`NO_SSPS=1` is what makes the entry work at all. GreptimeDB *does* answer
`COM_STMT_PREPARE`, unlike Databend, but its prepared-statement metadata is unusable: it
describes **every** parameter as `VAR_STRING` whatever the target column is, and then
type-checks the value it receives against the column and rejects the string it asked
for. A standalone unixODBC probe against `(i INT, …)` shows both halves:

```
SQLNumParams = 5
  param 1: SQL type 12 (SQL_VARCHAR), size 255, decdigits 0, nullable 2
  param 2..5: SQL type 12 (SQL_VARCHAR), size 255            <- every one of them
execute, bound SQL_C_SLONG -> SQL_INTEGER: FAILED
    [HY000] (1210) (InvalidArguments): Expected type: Int32(Int32Type),
                                       actual: MYSQL_TYPE_STRING
execute, bound SQL_C_SLONG -> SQL_VARCHAR: FAILED
    [HY000] (1210) (InvalidArguments): Expected type: Int32(Int32Type),
                                       actual: MYSQL_TYPE_BLOB
```

No ODBC-side binding satisfies both halves, so the server-side prepare protocol is a dead
end here. With `NO_SSPS=1` the connector substitutes bound parameters into the SQL text
and every statement goes as a plain query, which GreptimeDB parses and types normally.
The one thing it then needs is the driver quirk Databend already installs: in that mode
Connector/ODBC writes dates, timestamps and binaries as MySQL charset-introducer literals
(`_binary'2024-02-29 13:45:10.123456'`), which GreptimeDB's parser rejects with
`Unsupported ast node in sqltorel: Prefixed { prefix: Ident { value: "_binary" … } }`.
`temporal_binary_param_as_varchar` sends them as ordinary quoted text instead.

`PLUGIN_DIR` (the `{plugin_dir}` key in the entry's connection string) is the same story
as TiDB, Dolt and MatrixOne: GreptimeDB offers `mysql_native_password`, whose client-side
plugin Connector/ODBC 9 loads at run time from the directory it was *built* with, so an
unpacked tarball has to be pointed at its own copy.

### Driver quirk: every table needs a `TIME INDEX`

This is the one thing about GreptimeDB that needed new code rather than a tolerance flag.
Every GreptimeDB table must declare exactly one `TIME INDEX` column:

```
CREATE TABLE t (i INT, s VARCHAR(50))
-- (InvalidSyntax): Missing time index constraint
```

and that column has to be a `TIMESTAMP` (`time index column data type should be
timestamp` for a `DATE`) and `NOT NULL` (`time index column can't be null`). An ingest
payload need not carry a timestamp at all, so `adbc_ingest`'s generated
CREATE TABLE for it is rejected outright and no per-entry tolerance can reach it — that
DDL is built inside the driver.

`reader_opts.ddl_extra_column` and `reader_opts.ddl_table_options` (`src/odbc_internal.h`)
are two strings appended to a generated `CREATE TABLE`:

```
CREATE TABLE t (<ingested columns>, <ddl_extra_column>) <ddl_table_options>
```

Both are `NULL` for every other server, so nothing else changes. They are set in
`OdbcDetectQuirks` inside the existing "Connector/ODBC in front of a non-transactional
server" branch, keyed on `SELECT version()` containing `greptimedb` — the same pattern
psqlodbc uses to tell QuestDB from PostgreSQL, and the extra query is paid only by a
MySQL-wire server that already reported `SQL_TC_NONE`:

```c
conn->reader_opts.ddl_extra_column =
    "greptime_timestamp TIMESTAMP(3) TIME INDEX DEFAULT CURRENT_TIMESTAMP";
conn->reader_opts.ddl_table_options = "WITH ('append_mode'='true')";
```

`DEFAULT CURRENT_TIMESTAMP` is what keeps the ingested columns untouched: the `INSERT`
names only the payload's own columns and the server fills the time index in itself. The
table option is not decoration — outside append mode GreptimeDB *merges* rows that share
a time index, so rows ingested inside the same millisecond collapse:

```sql
CREATE TABLE q3 (a BIGINT, ts TIMESTAMP(3) TIME INDEX DEFAULT CURRENT_TIMESTAMP);
-- five INSERTs, one row each
SELECT count(*) FROM q3;   -- 2
```

With `WITH ('append_mode'='true')` the same five inserts read back as five rows.

### Entry notes

Four things are the server's own and live in the entry's tolerance flags:

* **`ts` is the `TIME INDEX`.** `adbc_t` declares
  `ts TIMESTAMP(6) TIME INDEX … WITH ('append_mode'='true')`, and because a time index
  cannot be NULL the all-NULL second row carries row 1's timestamp instead
  (`row2_fill=("ts",)`, a new flag) with the read-back assertion skipping that column
  (`not_null=("ts",)`). Append mode is needed on this table too: its two rows then share
  a timestamp, and without it they would merge into one.
* **Identifiers are backtick-quoted** (the new `quote` flag). GreptimeDB answers
  `SELECT @@sql_mode` with `0` and has no `ANSI_QUOTES` to switch to, so a double-quoted
  name in a column position is a *string literal*: `SELECT "a", "b" FROM t` returns the
  constants `('a','b')` for every row and `WHERE "a" = 2` matches nothing — silently,
  with no error. Connector/ODBC reports the backtick as `SQL_IDENTIFIER_QUOTE_CHAR`
  against this server, so `adbc_ingest` already quotes correctly; it is
  `test_matrix.py`'s own SQL that needed telling.
* **No `DOUBLE PRECISION`.** GreptimeDB has `DOUBLE` and `FLOAT` but neither ISO
  spelling: `SQL data type not supported yet: DoublePrecision` (and `: Real`). That is
  the one portable name in the generated ingest DDL it rejects — `BIGINT`, `TEXT`,
  `DATE`, `BOOLEAN`, `DECIMAL(p,s)`, `TIMESTAMP`, `BLOB`, `SMALLINT` and `INTEGER` are
  all accepted — so the entry sends that column as a decimal instead
  (`ingest_types={pa.float64(): pa.decimal128(12, 3)}`), which keeps create/append/replace
  under test. `adbc_t`'s own `f DOUBLE` column is unaffected.
* **`BOOLEAN` is `TINYINT(1)` on the wire** → `SQL_TINYINT` → `int8` (`bool_type="int8"`),
  exactly as for MySQL itself.

Everything else passes unchanged: the emoji round-trip, microsecond timestamps,
`DECIMAL(10,3)`, `VARBINARY` bytes, NULL parameters, affected-row counts, the 5000-row
batched read, `GetObjects`/`GetTableSchema` and the error text for an unknown table
(`Table not found: greptime.public.adbc_no_such_table`).

### Benchmark

```
greptimedb  MySQL (via ODBC) 8.4.2
            fetch 926,622 rows/s (pyodbc 306,078/s)
            ingest 5,054 rows/s, array 6,766 rows/s
```

`bench/matrix_bench.py`'s pyodbc ingest column is empty for this entry, and that is a
pyodbc limitation rather than a server or a driver one: pyodbc has no equivalent of
`temporal_binary_param_as_varchar`, so its `DATE` parameter goes out as the same
`_binary'…'` literal GreptimeDB's parser refuses.

### Clean up

```sh
docker rm -f adbcbridge-greptimedb
# or: docker compose -f tests/compat/docker-compose.yml --profile extra rm -sf greptimedb
```

## ArcadeDB 26.9

ArcadeDB is a multi-model database — the same records are reachable as documents, as a
property graph, as key-value pairs — with a PostgreSQL wire listener bolted on by a
plugin. That listener is why it is in this matrix: psqlodbc, the driver the `postgres`
entry uses, drives it unchanged. Nothing above the wire is PostgreSQL's, and the entry is
`read_only` because of it.

Server:

```sh
docker run -d --name adbcbridge-arcadedb --memory=1g -p 127.0.0.1:15441:5432 \
  -e JAVA_OPTS="-Darcadedb.server.rootPassword=Adbc2026 \
    -Darcadedb.server.defaultDatabases=adbc[adbc:adbc] \
    -Darcadedb.postgres.port=5432 \
    -Darcadedb.server.plugins=Postgres:com.arcadedb.postgres.PostgresProtocolPlugin" \
  arcadedata/arcadedb:latest
```

(or `docker compose -f tests/compat/docker-compose.yml --profile extra up -d arcadedb`;
it is in the `extra` profile, so a plain `up -d` leaves it alone). It is ready in about
ten seconds — `ArcadeDB Server started` in `docker logs`, preceded by
`Listening for incoming connections on 0.0.0.0:5432`. Three things about that command are
not optional:

* `arcadedb.server.plugins=Postgres:...` — the PostgreSQL protocol plugin is **not**
  loaded by default. Without it 5432 is closed and there is nothing to connect to.
* `arcadedb.server.rootPassword` — with no password set the server tries to prompt for
  one and exits.
* `arcadedb.server.defaultDatabases=adbc[adbc:adbc]` — the image ships no database at
  all. The syntax is `name[user:password]`; the matrix connects as `root`, but the
  database has to exist.

Host port 15441, not 15433: that one is yugabyte's.

No driver to fetch — ArcadeDB reuses the psqlodbc build the `postgres` entry uses (see
the PostgreSQL section for the root-free `.deb` extraction):

```sh
export ARCADEDB_ODBC_DRIVER=$POSTGRES_ODBC_DRIVER
ADBC_ODBC_DRIVER=$PWD/build/libadbc_driver_odbc.so ADBC_ODBC_DELEGATE=never \
  python tests/compat/test_matrix.py arcadedb
# arcadedb  PASS  (PostgreSQL (via ODBC) 12.0.0)
```

`SELECT version()` answers `PostgreSQL 12.0 (ArcadeDB 26.9.1-SNAPSHOT)` — the `arcadedb`
in there is what the driver keys its one quirk on, since `SQL_DBMS_NAME` is a bare
`PostgreSQL`.

### Why the entry is `read_only`

ArcadeDB has no `CREATE TABLE`:

```
CREATE TABLE probe1 (i INTEGER, s VARCHAR(50))
-> 42601 SQL syntax error at line 1, column 7: no viable alternative at input 'CREATE TABLE'
```

Its equivalent is a *type* plus one *property* per column, which is 1 + N statements:

```sql
CREATE DOCUMENT TYPE adbc_t;      -- or VERTEX TYPE / EDGE TYPE
CREATE PROPERTY adbc_t.i INTEGER;
CREATE PROPERTY adbc_t.s STRING;
```

`adbc_ingest` generates a `CREATE TABLE` for its target, so `mode="create"` (and
`"replace"`) cannot be spelled for ArcadeDB at all. That is a shape mismatch, not a type
name an `ansi_ddl_type_names`-style quirk could patch up, so the entry declares
`read_only=True` and builds `adbc_t` and `adbc_big` in `setup` with ArcadeDB's own DDL —
the same route the `flightsql` entry takes for a different reason. Everything else runs:
`SELECT`, a parameterised `SELECT`, the 100,000-row batched read, `GetObjects`,
`GetTableSchema`, the error path, and the `extra` graph steps, all through ODBC.

Ordinary DML is fine; only the generated DDL is not. `setup` drops and recreates both
types every time, because ArcadeDB has `CREATE DOCUMENT TYPE x IF NOT EXISTS` but no
`IF NOT EXISTS` for a property — and `setup` has to be idempotent, since
`bench/matrix_bench.py` replays it on every connection it opens.

### Driver quirk: `SQLColumns` returns an empty result set

psqlodbc builds `SQLColumns` as one pg_catalog query, and ArcadeDB emulates enough of
pg_catalog that each *table* in it works on its own (`pg_class`, `pg_namespace`,
`pg_attribute` and a two-table join over them all answer). The query as psqlodbc writes
it does not:

```
select n.nspname, c.relname, a.attname, ..., pg_get_expr(d.adbin, d.adrelid), ...
from (((pg_catalog.pg_class c inner join pg_catalog.pg_namespace n on ...)
       inner join pg_catalog.pg_attribute a on ...)
      inner join pg_catalog.pg_type t on ...)
left outer join pg_attrdef d on ...
```

Two things in it are past ArcadeDB's parser: the parenthesised join nesting, which it
does not recognise as a query at all, and `pg_get_expr()`, which it does not have
(`Unknown function name 'pg_get_expr'`). Run verbatim it answers "not a query"; run
through `SQLColumns` it produces `SQL_SUCCESS` and **zero rows**, so every table looks
like it has no columns:

```python
cur.columns(table="adbc_t").fetchall()   # -> []
```

There is no return code to fall back on, which is exactly the situation
`reader_opts.no_sql_columns` exists for (the Arrow Flight SQL driver reaches it by
segfaulting instead). With it set, `GetObjects` describes `SELECT * FROM <table> WHERE
1=0` and reads the columns off `SQLDescribeCol`, which ArcadeDB answers correctly and in
declaration order. The quirk is keyed on `SELECT version()` containing `arcadedb`, inside
the psqlodbc block — never on the driver name, which says nothing about the server.

### Driver fallback: `SQLTables(SQL_ALL_TABLE_TYPES)`

`GetTableTypes` asks the driver to enumerate table types, which psqlodbc answers with a
query of its own making:

```sql
select NULL, NULL, relkind from
  (select 'r' as relkind union select 'v' union select 'm' union select 'f' union select 'p') as a
```

ArcadeDB rejects it, and `SQLTables` fails — while an ordinary table listing through the
same call works perfectly. Rather than a per-server quirk, `OdbcConnectionGetTableTypes`
now falls back to a plain `SQLTables` listing and returns the distinct `TABLE_TYPE`
values the server's own tables have (`TABLE` here). Nothing else in the matrix reaches
that path, since every other driver answers the enumeration.

### Server behaviour handled by the entry's tolerance flags

* **`@rid`, `@type`, `@cat`** (`pseudo_columns`). Every `SELECT *` carries ArcadeDB's
  record metadata — record id, type name, and category (`d`ocument / `v`ertex / `e`dge) —
  so a describe of an eight-property type reports eleven columns. They are not columns of
  the table: the entry lists them in `pseudo_columns`, which drops them from the
  all-NULL row check and from the `GetObjects`/`GetTableSchema` comparisons.
* **`BoolsAsChar=0`** in the connection string. Without it psqlodbc reports every
  `BOOLEAN` as a `VARCHAR(5)` holding `"1"`/`"0"` instead of `SQL_BIT`. Same setting, same
  reason, as the `questdb` entry.
* **`not_null=("bo",)`.** With `BoolsAsChar=0` a boolean property has no NULL state on
  the wire: row 2's `bo` goes in as `NULL` and reads back `False`.
* **`decimal_type="decimal128(28, 3)"`.** ArcadeDB reports no declared precision for a
  `DECIMAL` property, so psqlodbc falls back to its own maximum (28) with the scale of the
  values in the result set — as it does for RisingWave's unqualified `NUMERIC`.
* **Timestamp literals need the ISO-8601 `T`.** `'2024-02-29 13:45:10.123456'` into a
  `DATETIME_MICROS` property is stored as `NULL`, silently;
  `'2024-02-29T13:45:10.123456'` round-trips to the microsecond. (A bound
  `SQL_TYPE_TIMESTAMP` parameter goes as the space form, so it hits the same silent NULL —
  a reason to write timestamps as ISO text against this server.)
* **No binary transport.** A bound `bytea` parameter is refused by the protocol layer
  outright (`Error on parsing bind message: Type with code 0 not supported for
  deserializing`), and a `BINARY` property fed a string hands the string straight back.
  A `\x0102` literal does not even lex (`token recognition error at: '\x'`), so the entry
  writes ROW1's two bytes as the `\uXXXX` escapes ArcadeDB does accept — which is what
  they read back as, and what the workload's `"\x01\x02"` branch already allows.
* **32 kB statements.** The PostgreSQL plugin refuses anything longer
  (`String content (294205) too long (>32768)`), so `setup` loads `adbc_big` in ~30 kB
  chunks. `arcadedb_insert()` in `test_matrix.py` does the splitting.

A boolean *parameter* is also refused — psqlodbc sends `SQL_BIT` as `"1"`, and ArcadeDB
answers `Cannot convert type 'class java.lang.String' to 'BOOLEAN'`, while the word
`'true'` is accepted. That is the same shape as QuestDB's `bool_param_as_varchar` quirk,
but nothing in this `read_only` entry binds a boolean, so no quirk is set for it here; it
is recorded as a known limitation instead.

### What the entry checks beyond the standard workload

ArcadeDB is a graph database as much as a document one, and none of the standard workload
touches that, so the `extra` steps build a small graph through the ODBC path and traverse
it: three vertices, two edges (`CREATE EDGE ... FROM (SELECT ...) TO (SELECT ...)`), then
one- and two-hop traversals with `expand(out('adbc_ge'))`. `MATCH` would be the other way
to write the traversal, but its `{...}` pattern syntax collides with ODBC escape
sequences and the driver manager rejects it (`ODBC escape convert error`) before the
server sees it.

### Benchmark

```sh
ADBC_MATRIX_SUFFIX=_arcadedb python bench/matrix_bench.py \
  --rows 10000 --fetch-rows 100000 --pyodbc-timeout 300 arcadedb
# arcadedb  PostgreSQL (via ODBC) 12.0.0   fetch=331,629/s
```

A `read_only` entry has no ingest numbers, and `--fetch-rows` does not apply: the
benchmark reads whatever `adbc_big` holds, which `setup` sizes at 100,000 rows.

### Clean up

```sh
docker rm -f adbcbridge-arcadedb
```

## InfluxDB 3 Core (Arrow Flight SQL)

InfluxDB 3 Core is a time-series database whose query interface is Arrow Flight SQL
(DataFusion behind it), so the ODBC route is the same Arrow Flight SQL ODBC driver the
[`flightsql`](#arrow-flight-sql-sqlflite-155--duckdb-111) entry uses — read that section
first: everything it documents about the driver (no `SQLBindParameter`, `SQLColumns`
segfaults on the first `SQLFetch`, decimals described as `(19, 0)`) is the driver's and
therefore true here too. This entry exists to run the workload against a *second*, very
different Flight SQL server: `flightsql` is DuckDB behind the protocol, this is a
time-series engine with its own column model.

Server:

```sh
docker run -d --name adbcbridge-influxdb3 --memory=2g -p 127.0.0.1:18181:8181 \
  influxdb:3-core influxdb3 serve --node-id=n1 --object-store=memory --without-auth
```

(or `docker compose -f tests/compat/docker-compose.yml --profile extra up -d influxdb3`;
it is in the `extra` profile, so a plain `up -d` leaves it alone). It is ready in about
three seconds — `curl -s http://127.0.0.1:18181/health` answers `OK`. `--without-auth`
runs it with no token, which is what lets the ODBC connection go through without one;
`--object-store=memory` keeps the whole database in RAM, so removing the container
removes the data. Port 8181 serves the HTTP API and the Flight SQL endpoint at once.

Driver: none to install — this is the same library the `flightsql` entry extracts, so
point a second variable at it:

```sh
export INFLUXDB3_ODBC_DRIVER=$FLIGHTSQL_ODBC_DRIVER
```

### Loading the data (the entry cannot)

InfluxDB 3's SQL is query-only: `CREATE TABLE`, `CREATE VIEW` and `INSERT` all come back
as `Error during planning: DDL not supported`, and a table exists only once line protocol
has been written to it. Together with the driver's missing `SQLBindParameter` that leaves
no way at all to load a table over the ODBC connection, so the entry is `read_only=True`
and its two tables are written over the HTTP API first:

```sh
python3 tests/compat/fixtures/load_influxdb3.py            # http://127.0.0.1:18181, db "adbc"
# wrote 100002 points to adbc (HTTP 204)
```

That script (stdlib only) writes `adbc_t` — the workload's two rows — and `adbc_big`,
100,000 `(a, b)` points one nanosecond apart. It takes about a second and is idempotent:
a point replaces the one with the same table, tag set and timestamp.

Run the entry:

```sh
ADBC_ODBC_DRIVER=$PWD/build/libadbc_driver_odbc.so \
  python tests/compat/test_matrix.py influxdb3
# influxdb3 PASS  (InfluxDB IOx (via ODBC) 02.00.0000)
```

### The database name is a gRPC header, not a Flight SQL concept

Flight SQL has no "connect to database X" step, so InfluxDB reads the database from a
gRPC metadata header (`database`). The driver forwards every connection property it does
not recognise as a header of exactly that name, so nothing more than this is needed:

```
Driver={drv};Host=127.0.0.1;Port=18181;useEncryption=false;database=adbc;
```

`useEncryption=false` matches the server's plain-gRPC listener (it is only TLS when
`--tls-cert`/`--tls-key` are given). No `UID`/`PWD`: the server was started
`--without-auth`.

### What works

The whole read side of the workload, exactly as for `flightsql`: `int64`, `double`,
`string` (including `"héllo 🚀"` — the driver describes InfluxDB's strings as
`SQL_VARCHAR`, so the reader is on its correct narrow UTF-8 path and the astral-plane
emoji survives), `bool`, `DATE`, `TIMESTAMP`, the all-NULL row, the 100,000-row batched
read, `GetObjects`, `GetTableSchema` and the error text (`table
'public.iox.adbc_no_such_table' not found`). `GetObjects` works because the existing
`no_sql_columns` quirk already covers this driver. **No driver change was needed for
InfluxDB.** The entry also runs two things only a time-series engine does: a `date_bin`
1-hour bucketed aggregate, and a range scan on the `time` column InfluxDB partitions by.

Fetch: **1.03M rows/s** over the 100,000-row `adbc_big` (`bench/matrix_bench.py`). There
is no ingest number — nothing can be written through this driver.

### The entry's tolerances are all InfluxDB's column model

An InfluxDB table is *tags* (strings), *fields* (float, integer, unsigned, string or
boolean — that is the whole list) and `time`, a nanosecond timestamp that every point
carries and that is always spelled `time`. Four of the workload's eight columns therefore
cannot exist as declared, and since the server allows no view to paper over it, the
entry's `select` does the aliasing and casting in the query itself:

```sql
SELECT i, f, s, b, CAST(d AS DATE) AS d, time AS ts, n, bo FROM adbc_t ORDER BY i
```

| flag | why |
|---|---|
| `read_only=True` | no DDL and no DML: InfluxDB's SQL is query-only, and the driver has no `SQLBindParameter` — two independent reasons, either one enough. The tables come from `fixtures/load_influxdb3.py`. |
| `params=False` | the driver answers `SQLBindParameter` with `HYC00 "Unsupported function"` on a virgin statement handle; the parameterised query runs with a literal. |
| `select=...` | `ts` is the table's own `time` column (so the entry reads the server's real timestamp rather than a stand-in), and `d` is a text field cast back to `DATE` — InfluxDB has no date type. |
| `catalog_cols=("b", "bo", "d", "f", "i", "n", "s", "time")` | what `GetObjects`/`GetTableSchema` really report: the same eight columns with `time` in place of `ts`, in InfluxDB's own alphabetical order. |
| `not_null=("ts",)` | `time` has no NULL state — every point has one — so the all-NULL row reads back with its timestamp, as Access `YESNO` reads back `False`. |
| `binary_text="\\x0102"` | no binary type at all, so the two bytes are stored as text, exactly as for CrateDB. |
| `decimal_type="string"` | no `DECIMAL` either; `n` is a string field read back as its exact digits — the same place the `flightsql` entry ends up for a different reason (see its quirk 3). |

Two further things are worth knowing and cost the entry nothing:

* The driver describes **every** `SQL_TYPE_TIMESTAMP` column as scale 3, whatever the
  Arrow unit behind it — `Timestamp(ns)`, `Timestamp(µs)` and `Timestamp(ms)` all come
  back `size=23 scale=3` — while `SQLGetData` hands over the full digits
  (`2024-02-29 13:45:10.123456000`). The reader sizes the Arrow column from the described
  scale, so timestamps arrive as `timestamp[ms]`: `.123456` reads back `.123000`, which
  the workload's `ts_us` default already allows. This is not InfluxDB-specific — it is
  the same for sqlflite — so it is left alone rather than special-cased.
* Writing points dated 2024 into a server whose clock says 2026 is accepted: InfluxDB 3
  Core applies no ingest-time window to a past timestamp.

### Clean up

```sh
docker compose -f tests/compat/docker-compose.yml --profile extra down influxdb3
# or, if started standalone:
docker rm -f adbcbridge-influxdb3
```

## StarRocks 4.1

[StarRocks](https://www.starrocks.io/) is an MPP columnar warehouse that serves the MySQL
wire protocol on 9030 — its FE announces itself as `8.0.33` and `version()` returns that
too — so it needs no ODBC driver of its own: the same MySQL Connector/ODBC build used for
the `mysql` entry drives it (see [MySQL 8](#mysql-8) above for the root-free tarball, and
for the `LD_PRELOAD` that `import pyarrow` makes necessary). Only the wire protocol is
MySQL's; the SQL dialect, the type names and the transaction model are StarRocks' own,
and that is where all of this entry's work is.

### Start the server

```sh
docker compose -f tests/compat/docker-compose.yml --profile extra up -d starrocks
# or standalone:
docker run -d --name adbcbridge-starrocks --memory=5g \
  -p 127.0.0.1:19030:9030 starrocks/allin1-ubuntu:latest
```

`allin1-ubuntu` runs the FE (frontend/coordinator, a JVM) and one BE (backend) in the one
container. It is ready in well under a minute; wait for the BE to register rather than
for the port, because the FE accepts connections a few seconds before it has a backend to
plan against:

```sh
until docker exec adbcbridge-starrocks \
        mysql -h127.0.0.1 -P9030 -uroot -e 'SHOW BACKENDS\G' 2>/dev/null \
      | grep -q 'Alive: true'; do sleep 5; done
# Version: 4.1.4-...   Alive: true   MemLimit: 4.050GB
```

The BE sizes its own memory limit from the container's, so `--memory` is the only knob
needed. SQL is on `127.0.0.1:19030` with the built-in passwordless `root`. There is no
user database, so the entry's `setup` runs `CREATE DATABASE IF NOT EXISTS adbc` and
`USE adbc` — both idempotent, which matters because `bench/matrix_bench.py` replays
`setup` on every connection it opens — and the connection string names no database.

`CREATE TABLE` needs no `DISTRIBUTED BY` and no `replication_num`: StarRocks 3.1 and later
pick a random bucket distribution for a table that does not ask for one, and the single-BE
image defaults the replication factor to 1.

### Run the entry

```sh
export STARROCKS_ODBC_DRIVER=$MYSQL_ODBC_DRIVER   # the Connector/ODBC tarball's libmyodbc9w.so
LD_PRELOAD=/lib/x86_64-linux-gnu/libstdc++.so.6 \
ADBC_ODBC_DRIVER=$PWD/build/libadbc_driver_odbc.so \
  .venv/bin/python tests/compat/test_matrix.py starrocks
# starrocks PASS  (MySQL (via ODBC) 8.0.33)
```

`PLUGIN_DIR` is needed here for the same reason as for TiDB, Dolt and MatrixOne:
StarRocks offers only `mysql_native_password`, whose *client-side* plugin Connector/ODBC 9
loads at run time from its compiled-in `/usr/local/mysql/lib/plugin`. The entry's
connection string ends in `{plugin_dir}`, which `conn_uri()` expands to the tarball's own
`lib/plugin` when that directory exists — see [TiDB](#tidb-75) for the full story.

### `NO_SSPS=1`: no prepared statements but `SELECT`

StarRocks answers `COM_STMT_PREPARE` for anything other than a `SELECT` with

```
1295: Getting analyzing error. Detail message:
      This command is not supported in the prepared statement protocol yet.
```

which reaches adbcbridge as `SQLPrepare failed` on the parameterised `INSERT`. It is not a
matter of switching the feature on — `enable_prepare_stmt` is already `true`, and
`PREPARE s FROM 'INSERT INTO t VALUES (?,?)'` fails the same way in the `mysql` client, on
a duplicate-key table and on a `PRIMARY KEY` table alike, while `PREPARE` of a `SELECT`
succeeds. So the entry sets `NO_SSPS=1`, exactly as `databend` does: Connector/ODBC then
stops using the server-side prepare protocol and substitutes bound parameters into the SQL
text, and every statement goes as a plain query.

### Driver quirk: `_binary` literals (`temporal_binary_param_as_varchar`)

In `NO_SSPS` mode Connector/ODBC writes every parameter whose *SQL* type is not character
or numeric as a MySQL charset-introducer literal — `_binary'2024-02-29'` for a
`SQL_TYPE_DATE`, `_binary'…'` for a `SQL_TYPE_TIMESTAMP` or a `SQL_VARBINARY`. Introducers
are MySQL/MariaDB syntax, and StarRocks' parser rejects them:

```
1064: Getting syntax error at line 1, column 33.
      Detail message: Unexpected input ''2024-02-29''
```

`temporal_binary_param_as_varchar` — the existing `OdbcDetectQuirks` flag for MySQL
Connector/ODBC in front of a server that reports `SQL_TXN_CAPABLE = SQL_TC_NONE`, which is
how a non-MySQL MySQL-wire warehouse identifies itself — binds those three as
`SQL_VARCHAR` text instead, so the driver emits an ordinary quoted literal. StarRocks
coerces all three from one: a plain `'…'` literal holding the raw bytes lands in a
`VARBINARY` column as those bytes (`hex(b)` → `0102`).

That flag was already declared, documented and set for this exact case, but its
implementation had been dropped in a bad merge conflict resolution (`552622b`, which was
meant only to fold `ignore_driver_type_names` into `ansi_ddl_type_names`) — it was dead
code on `main`, and Databend, the server it was written for, was relying on it. Restoring
it is `src/odbc_bind.c`: the `SQL_C_CHAR` paths in `SlotFromArrowValue()` for
binary/`DATE32`/`TIMESTAMP`, the `TimestampTextFromStruct()` helper they render through,
and the three `*supported = false` returns in `ArrayParamPlan()` that send such a batch
row-at-a-time (a driver that substitutes parameters into the SQL text sends one statement
per set either way, so nothing is lost).

### Driver quirk: MySQL type names in ingest DDL (`ansi_ddl_type_names`)

The same `SQL_TC_NONE` branch sets `ansi_ddl_type_names`, and StarRocks needs it for the
same two names MariaDB ColumnStore did: Connector/ODBC's `SQLGetTypeInfo` answers with
MySQL's type system whatever the server is, so `SQL_LONGVARCHAR` is `long varchar` and
`SQL_BIT` is `bit`, and StarRocks rejects both (`Unexpected input 'VARCHAR'`,
`Unexpected input ')'`). The portable names `TEXT` and `BOOLEAN` are accepted — `TEXT`
becomes `varchar(65533)`.

One portable name had to change for StarRocks, in `ColumnTypeSql()`: the fallback for a
double was the ISO spelling `DOUBLE PRECISION`, which StarRocks does not parse at all
(`CREATE TABLE t (c DOUBLE PRECISION)` → `Unexpected input ','`). It is now `DOUBLE`. This
fallback is only ever reached when the driver's own type names are unusable, which in
practice means an analytic engine behind someone else's wire protocol — and MySQL/MariaDB
(hence ColumnStore), QuestDB and Databend all *name* the type `DOUBLE` and accept
`DOUBLE PRECISION` only as an alias, so the one-word spelling is strictly the safer of the
two here. The `questdb` entry, the other user of `ansi_ddl_type_names`, was re-run to
confirm it.

### Entry notes: no `ANSI_QUOTES`, and a wider `DECIMAL`

**StarRocks has no double-quoted-identifier mode.** Every other MySQL-wire entry puts
`ANSI_QUOTES` in `sql_mode` so that the double-quoted identifiers `adbc_ingest` emits
parse. StarRocks accepts the *value* — `SET SESSION sql_mode = CONCAT(@@sql_mode,
',ANSI_QUOTES')` succeeds and `@@sql_mode` reads back `ANSI_QUOTES,ONLY_FULL_GROUP_BY` —
but its parser ignores it: `SELECT "i" FROM t` still returns the string `i`, and
`CREATE TABLE "q" ("a" BIGINT)` is a syntax error. Nothing in `src/` had to change, because
nothing there hard-codes the quote: `OdbcQuoteChar()` asks the driver for
`SQL_IDENTIFIER_QUOTE_CHAR`, and Connector/ODBC correctly answers the backtick when the
session has no `ANSI_QUOTES`, so ingest already quotes the way the server wants. What did
have to change is `tests/compat/test_matrix.py`, which wrote its own reads of the ingested
table with literal `"`; those now go through a `qi()` helper and the entry's
``quote="`"``. Every other entry keeps the default `"`.

**`DECIMAL(10,3)` reads back as `decimal128(12, 3)`.** StarRocks describes a decimal
column at the *display* width MySQL uses on the wire — 12, the ten digits plus the sign and
the decimal point — where a real MySQL reports the declared precision, and Connector/ODBC
passes that through as the `SQL_DECIMAL` precision. No digits are lost (the scale is right
and `12.345` round-trips exactly), so this is one tolerance flag,
`decimal_type="decimal128(12, 3)"`, not a driver fix.

Everything else in the standard workload runs on the generic path: the emoji round-trip,
`VARBINARY(10)`, `DATE`, `DATETIME` microseconds, NULL parameters, affected-row counts,
`GetObjects`/`GetTableSchema`, and the batched ingest and read. `BOOLEAN` is stored as a
`TINYINT` and described as `SQL_TINYINT`, so `bool_type="int8"` as for MySQL.

### Ingest is ~10 rows/s, and that is the server

Every StarRocks `INSERT` is its own load transaction whose version has to publish before
the statement returns, and that costs a flat ~100 ms whatever the statement holds. It is
not the driver and not adbcbridge — the `mysql` client inside the container measures the
same thing:

```sh
# 100 single-row INSERTs, straight from the container's own client
docker exec adbcbridge-starrocks mysql -h127.0.0.1 -P9030 -uroot -e "$SQL"
# 100 single-row inserts: 10.09s
```

Since `NO_SSPS` makes the connector send one statement per parameter set, and a parameter
array is not a multi-row `INSERT` on this wire, single-row ingest runs at that rate for
every client. The entry therefore sets `big_rows=2000` — still across the reader's
1024-row batch boundary, which is what that step is for, without spending eight minutes on
it — and the benchmark below uses `--rows 300`. Reads are unaffected and fast.

### Benchmark

```
ADBC_MATRIX_SUFFIX=_starrocks .venv/bin/python bench/matrix_bench.py \
  --rows 300 --fetch-rows 2000 --pyodbc-timeout 300 starrocks
# fetch=406,335/s (pyodbc 305,290/s)  ingest=10/s array=10/s pyodbc=failed
```

Reads are ordinary: 406k rows/s against pyodbc's 305k on the same 2000-row table. Ingest
is the ~100 ms per `INSERT` above, identical with and without array binding because the
connector sends one statement per set either way.

The pyodbc ingest column is not a timeout — it is the `_binary` literal, which pyodbc has
no way around:

```
ProgrammingError: ('42000', '[42000] [MySQL][ODBC 9.4(w) Driver][mysqld-8.0.33]
  Getting syntax error at line 1, column ...')
```

pyodbc binds a `datetime` parameter as `SQL_TYPE_TIMESTAMP`, Connector/ODBC renders that
as `_binary'...'` under `NO_SSPS`, and StarRocks' parser rejects it. adbcbridge writes the
same table because `temporal_binary_param_as_varchar` sends those parameters as text
instead.

### Clean up

```sh
docker compose -f tests/compat/docker-compose.yml --profile extra down starrocks
# or, if started standalone:
docker rm -f adbcbridge-starrocks
```

## Apache Ignite 2.17

Apache Ignite is a distributed in-memory key-value grid with a SQL engine on top of it.
Every SQL table *is* a cache — the `PRIMARY KEY` of a `CREATE TABLE` is the cache key —
and that one fact shapes the whole entry. Ignite is also the only server in this matrix
whose ODBC driver has to be built first: Apache ships `libignite-odbc.so` for Windows
only (two `.msi` installers), and the Linux binary release carries the C++ *sources* it
would be built from.

### Start the server

```sh
docker run -d --name adbcbridge-ignite --memory=2g \
  -e JVM_OPTS="-Xms512m -Xmx1200m" \
  -p 127.0.0.1:11800:10800 apacheignite/ignite:latest
```

(or `docker compose -f tests/compat/docker-compose.yml --profile extra up -d ignite`; it
is in the `extra` profile, so a plain `up -d` leaves it alone). It is ready in about
fifteen seconds — `Topology snapshot [ver=1, ..., state=ACTIVE]` in `docker logs`. 10800
is Ignite's thin-client port, which is also the ODBC one; there is nothing else to
configure, the node comes up with a `PUBLIC` schema and authentication off.

`JVM_OPTS` is not decoration: Ignite sizes its heap from the *host's* RAM, not from the
container's limit, so on a large box the default heap is far past `--memory` and the JVM
is killed by the OOM killer partway through the first big statement. Do not pass
`IGNITE_QUIET=false` either — the node then logs every page-lock acquisition, which is
megabytes per second under load.

### Build the ODBC driver without root

The image carries the driver's sources under
`/opt/ignite/apache-ignite/platforms/cpp` (`odbc/`, plus the `common`, `binary` and
`network` modules it links against). Copy them out of the container and build them
against the system unixODBC and OpenSSL headers — no Ignite, no JVM and no JNI is
involved, the ODBC driver is a plain thin client that speaks Ignite's binary protocol:

```sh
mkdir -p /tmp/dbs/ignite
docker cp adbcbridge-ignite:/opt/ignite/apache-ignite/platforms/cpp /tmp/dbs/ignite/cpp
cmake -S /tmp/dbs/ignite/cpp -B /tmp/dbs/ignite/build -DCMAKE_BUILD_TYPE=Release \
      -DWITH_ODBC=ON -DWITH_CORE=OFF -DCMAKE_INSTALL_PREFIX=/tmp/dbs/ignite/inst
cmake --build /tmp/dbs/ignite/build -j4      # ~2 minutes
cmake --install /tmp/dbs/ignite/build
# -> /tmp/dbs/ignite/inst/lib/libignite-odbc.so
```

`WITH_CORE=OFF` is what keeps the build root-free and short: the Ignite C++ *core* module
embeds a JVM and needs `JAVA_HOME` and the Ignite jars, while the ODBC driver needs
neither. Requires `unixodbc-dev` and `libssl-dev`, which the matrix needs anyway. The
installed library has an `RPATH` pointing at its own `lib` directory, so it finds
`libignite-common/binary/network.so` on its own — no `LD_LIBRARY_PATH` entry.

### Run the entry

```sh
export IGNITE_ODBC_DRIVER=/tmp/dbs/ignite/inst/lib/libignite-odbc.so
ADBC_ODBC_DRIVER=$PWD/build/libadbc_driver_odbc.so ADBC_ODBC_DELEGATE=never \
  ADBC_MATRIX_SUFFIX=_ignite python tests/compat/test_matrix.py ignite
# ignite    PASS  (Apache Ignite (via ODBC) 02.04.0000)
```

The connection string is `ADDRESS=host:port` (`SERVER=`/`PORT=` also work) plus
`SCHEMA=PUBLIC`. `02.04.0000` is the *protocol* version the driver reports as
`SQL_DBMS_VER`, not the server's — Ignite's ODBC driver has no other version to give.

### Driver quirk 1: no wide SQL type

`SQLBindParameter` refuses `SQL_WVARCHAR` outright, before it looks at any value:

```
HYC00  Data type is not supported. [typeId=-9]
```

`type_traits.cpp`'s `IsSqlTypeSupported()` lists `SQL_CHAR`/`SQL_VARCHAR`/
`SQL_LONGVARCHAR` and refuses `SQL_WCHAR`/`SQL_WVARCHAR`/`SQL_WLONGVARCHAR`. The C type
`SQL_C_WCHAR` *is* accepted, but the driver's wide buffers are `wchar_t`-sized
(`PutStrToStrBuffer<wchar_t>`, 4 bytes on Linux) where unixODBC passes UTF-16 — the same
mistake Firebird's OdbcFb makes. Its narrow path is UTF-8 (Ignite stores strings as
UTF-8 and the driver copies the bytes through), so the driver sets the existing
`wchar_as_utf8` quirk on `SQL_DRIVER_NAME` "Apache Ignite" and `héllo 🚀` round-trips.

### Driver quirk 2: parameter arrays read the NULL indicator from row 0

Ignite's driver implements column-wise parameter arrays and reports both
`SQL_ATTR_PARAMS_PROCESSED_PTR` and the parameter-status array, and a two-column,
three-row array inserts three correct rows. Add a NULL anywhere below the first row and
the *connection* dies:

```
SQLExecute -> HY000, then the next statement:
08001  Failed to establish connection with any provided hosts.
```

with the server logging `java.lang.OutOfMemoryError: Java heap space` and halting the JVM.
`odbc/src/app/parameter.cpp` is why:

```cpp
void Parameter::Write(BinaryWriterImpl& writer, int offset, SqlUlen idx) const
{
    if (buffer.GetInputSize() == SQL_NULL_DATA) { writer.WriteNull(); return; }

    ApplicationDataBuffer buf(buffer);   // the copy...
    buf.SetByteOffset(offset);
    buf.SetElementOffset(idx);           // ...is where the row offset is applied
```

`GetInputSize()` reads `*GetResLen()`, and `GetResLen()` applies the buffer's *own*
element offset — which is still 0. So the NULL test always inspects the indicator of row
0 and every row of the chunk inherits row 0's NULL-ness. A NULL in a later row is
therefore sent as whatever the value buffer holds at that row, and for a character or
binary column that means a length the server cannot parse: it reads a bogus array size
and tries to allocate it. Hence `no_param_arrays` for this driver, the same flag DuckDB,
clickhouse-odbc, OdbcFb and MonetDBODBClib carry. One execute per row is correct because
there the element offset is 0 for real.

Standalone repro of the working two-column case, and of the row-0 indicator bug, without
adbcbridge: bind `SQL_ATTR_PARAMSET_SIZE=3` on `INSERT INTO t (a, b) VALUES (?, ?)` with
`ind[] = {SQL_NTS, SQL_NULL_DATA, SQL_NTS}` — all three rows arrive non-NULL, and with
`ind[0] = SQL_NULL_DATA` all three arrive NULL.

### Why the entry sets `ingest_create=False`

Ignite refuses any table that does not declare a key:

```
CREATE TABLE nopk (a BIGINT, b VARCHAR)
-> 42000  No PRIMARY KEY defined for CREATE TABLE
```

`adbc_ingest`'s generated `CREATE TABLE` has no notion of a key, and no column of an
ingest payload can generally be one — the matrix's own is `a = [1, 2, NULL]`, and Ignite
allows neither a NULL key nor the duplicate keys the `append` step would then insert
(`23000 Failed to INSERT some keys because they are already in cache`). Nor can the key
be a column the server fills in itself, the way GreptimeDB's `ddl_extra_column` is: a
`DEFAULT` must be a constant (`Non-constant DEFAULT expressions are not supported` for
`RANDOM_UUID()`), and an `INSERT` that omits the key column fails with `Failed to prepare
update plan` whatever its default. So `mode="create"` cannot work here, and this is a
property of Ignite rather than of its driver — no quirk can fix it.

The entry therefore sets `ingest_create=False`, which makes the workload read the big
table `setup` built (exactly as a `read_only` entry does) instead of creating one, and
covers bulk ingest in its `extra` steps in the only shape Ignite has for it: an
`adbc_ingest(mode="append")` into a table that declares its own `PRIMARY KEY`. Everything
else runs unchanged — the typed-parameter `INSERT` of all eight columns including the
all-NULL row, the reads, the parameterised `SELECT`, the 100,000-row batched read,
`GetObjects`, `GetTableSchema` and the error path.

### Entry notes

| Setting | Why |
|---|---|
| `ident=str.upper` | Ignite folds an unquoted identifier to upper case. |
| `quote=""` | its driver answers `SQL_IDENTIFIER_QUOTE_CHAR` with an empty string, so `adbc_ingest` quotes nothing and the names it emits are folded; this file's own SQL has to leave them unquoted too, or a quoted `"a"` would be a different column from the `A` ingest just created. |
| `ddl ... i INT PRIMARY KEY` | see above — there is no table without one. `b BINARY` is Ignite's byte-string type; `VARCHAR` takes an optional length that it ignores. |
| `setup=[... SYSTEM_RANGE ...]` | `adbc_big` is filled server-side by `INSERT INTO adbc_big (a, b) SELECT X, 'r' \|\| X FROM SYSTEM_RANGE(0, 99999)` — `SYSTEM_RANGE` is the H2 table function Ignite's SQL engine inherits. 100,000 rows in about three seconds, which is what `setup` costs on every connection opened. |

Nothing else needed a tolerance: `DECIMAL(10,3)` is described with its declared precision
and scale, `TIMESTAMP` keeps all six fractional digits through a bound parameter,
`BOOLEAN` is a real `SQL_BIT` with a NULL state, and `GetObjects` reports the eight
columns in order.

One thing to know about the driver's own metadata, which costs the entry nothing but
explains the shape of a read: it describes every `VARCHAR` and `BINARY` column as
2,147,483,647 units wide (Ignite's SQL types carry no length), and it can recover neither
kind of truncated value — `SQL_GETDATA_EXTENSIONS` is `SQL_GD_ANY_COLUMN | SQL_GD_ANY_ORDER
| SQL_GD_BLOCK` with **no** `SQL_GD_BOUND`, and `FetchScroll()` refuses every orientation
but `SQL_FETCH_NEXT` ("Only SQL_FETCH_NEXT FetchOrientation type is supported"). Both
reports are honest — there is no `SQLSetPos` behind them — so such a column stays unbound
and the result set is read a row at a time with `SQLGetData` rather than through a block
cursor. It is fast regardless (930k rows/s below): the driver has already paged the whole
answer into client memory, so a per-row `SQLGetData` is a local copy.

### Benchmark

```sh
ADBC_MATRIX_SUFFIX=_ignite python bench/matrix_bench.py \
  --rows 10000 --fetch-rows 100000 --pyodbc-timeout 300 ignite
```

Like the `read_only` entries, this measures the fetch only — 930k rows/s over `adbc_big`'s
100,000 rows. There is no create-ingest to measure; an `adbc_ingest(mode="append")` of
10,000 rows into a pre-created `PRIMARY KEY` table runs at about 95k rows/s (median of
five), on the driver's multi-row `INSERT` path, since parameter arrays are off here.

### Clean up

```sh
docker compose -f tests/compat/docker-compose.yml --profile extra down ignite
# or, if started standalone:
docker rm -f adbcbridge-ignite
```

## OpenSearch 3.8 (SQL plugin)

OpenSearch is a search engine, not a SQL database, but every distribution bundles the
**SQL plugin**: a `/_plugins/_sql` REST endpoint that answers `SELECT`/`SHOW`/`DESCRIBE`
over indices. The OpenSearch project also publishes an ODBC driver for that endpoint,
which is what this entry drives. It is a read-only driver, and the project ships it for
**Windows and macOS only** — so the Linux build below is the interesting part of this
section.

Server:

```sh
docker run -d --name adbcbridge-opensearch --memory=3g -p 127.0.0.1:19200:9200 \
  -e discovery.type=single-node -e DISABLE_SECURITY_PLUGIN=true \
  -e OPENSEARCH_JAVA_OPTS="-Xms512m -Xmx512m" \
  opensearchproject/opensearch:latest
```

(or `docker compose -f tests/compat/docker-compose.yml --profile extra up -d opensearch`;
it is in the `extra` profile, so a plain `up -d` leaves it alone). It is ready in under a
minute — `curl -s http://127.0.0.1:19200` answers with the version document.
`DISABLE_SECURITY_PLUGIN=true` turns off TLS and authentication, which is what lets the
ODBC connection through with `auth=NONE;useSSL=0`; `OPENSEARCH_JAVA_OPTS` pins a 512 MB
heap, since the default is a quarter of the host's RAM.

### Getting the driver: there is no Linux build to download

`opensearch-project/sql-odbc` has exactly one release (1.5.0.0, July 2023) and its single
asset, `artifacts.tar.gz`, holds one macOS `.pkg` and two Windows `.msi` files — no
`.so`. The README says as much ("a read-only ODBC driver for Windows and Mac"), and the
source tree carries `build_mac_*.sh` and `build_win_*.ps1` and no Linux script.

It does build on Linux, though: `src/CMakeLists.txt` has a `WITH_UNIXODBC` branch and
`src/sqlodbc/CMakeLists.txt` links `odbc odbcinst` for `UNIX`. That branch has plainly
never been compiled — five things stand in the way, three of them real bugs — but once
they are fixed the driver works. Build it root-free (no `vcpkg` needed; the only
dependency is the AWS SDK for C++ `core` component, which the driver uses as its HTTP
client, and it builds against the system curl/openssl/zlib):

```sh
mkdir -p /tmp/dbs/opensearch && cd /tmp/dbs/opensearch
git clone --depth 1 https://github.com/opensearch-project/sql-odbc.git src-repo

# 1. aws-cpp-sdk-core.  1.8.x is the last series with no aws-crt-cpp dependency, so
#    BUILD_ONLY=core is a few minutes rather than half an hour.
curl -sSLO https://github.com/aws/aws-sdk-cpp/archive/refs/tags/1.8.186.tar.gz
tar xzf 1.8.186.tar.gz
# Its aws-c-common / aws-checksums / aws-c-event-stream ExternalProjects build their test
# suites with -Werror and do not compile clean under gcc 13, and the SDK forwards no
# flags to them: add -DBUILD_TESTING=OFF to the three CMAKE_ARGS lists, and drop -Werror
# from the SDK's own warning list.
sed -i 's|-DCMAKE_INSTALL_PREFIX=${AWS_DEPS_INSTALL_DIR}|&\n        -DBUILD_TESTING=OFF|' \
  aws-sdk-cpp-1.8.186/third-party/cmake/BuildAws{CCommon,Checksums,EventStream}.cmake
sed -i 's/"-Wall" "-Werror"/"-Wall"/' aws-sdk-cpp-1.8.186/cmake/compiler_settings.cmake
cmake -S aws-sdk-cpp-1.8.186 -B aws-build -DCMAKE_BUILD_TYPE=Release -DBUILD_ONLY=core \
  -DENABLE_TESTING=OFF -DCUSTOM_MEMORY_MANAGEMENT=OFF -DENABLE_RTTI=OFF \
  -DBUILD_SHARED_LIBS=ON -DCMAKE_INSTALL_PREFIX=$PWD/aws-prefix
cmake --build aws-build -j6 && cmake --install aws-build
```

Then the driver tree. Three of the five fixes are one-line `sed`s; two are edits:

```sh
cd /tmp/dbs/opensearch/src-repo
# (a) -Werror, on a 2023 code base under gcc 13
sed -i 's/ -Werror//' src/CMakeLists.txt
# (b) a vestigial `#include "linux/kconfig.h"` -- a *kernel* header, which is also why
#     the CMakeLists puts /usr/src/linux-headers-5.0.0-27/include on the include path
sed -i '/#include "linux\/kconfig.h"/d' src/sqlodbc/opensearch_odbc.h
# (e) sem_init() is given the semaphore's *capacity* as its initial count, where WIN32
#     (CreateSemaphore(NULL, initial, capacity, ...)) and __APPLE__
#     (dispatch_semaphore_create(initial)) both use `initial`.  See below: this one
#     crashes at run time rather than at compile time.
sed -i 's/sem_init(&m_semaphore, 0, capacity);/sem_init(\&m_semaphore, 0, initial);/' \
  src/sqlodbc/opensearch_semaphore.cpp
```

* **(c) rapidjson.** `src/CMakeLists.txt` sets `RAPIDJSON_SRC` inside an `if(WIN32)`
  (macOS gets rapidjson from vcpkg) although the vendored copy is right there in
  `libraries/rapidjson/include`. Drop that one guard so the `set(RAPIDJSON_SRC ...)`
  runs unconditionally.
* **(d) `src/sqlodbc/opensearch_semaphore.cpp` does not compile off Windows/macOS at
  all.** Its constructor's member-initialiser list is empty on any other platform, so a
  bare `:` is left before the constructor body; and `try_lock_for` calls
  `sem_timedwait(&m_semaphore & ts)` where it means `sem_timedwait(&m_semaphore, &ts)`.
  Guard the `:` with `#if defined(WIN32) || defined(__APPLE__)` and pass `&ts` as the
  second argument (normalising `ts.tv_nsec` past a second into `tv_sec` while you are
  there, or `sem_timedwait` answers `EINVAL`).

Then build:

```sh
cd /tmp/dbs/opensearch
# SIZEOF_VOID_P / SIZEOF_LONG / HAVE_SSIZE_T are hard-coded for __APPLE__ in
# opensearch_odbc.h and #error out otherwise; the LP64 values are the Linux ones.
DEFS="-DSIZEOF_VOID_P=8 -DSIZEOF_LONG=8 -DHAVE_SSIZE_T -DHAVE_LONG_LONG"
cmake -S src-repo/src -B odbc-build -DCMAKE_BUILD_TYPE=Release -DBUILD_WITH_TESTS=OFF \
  -DCMAKE_C_FLAGS="$DEFS" -DCMAKE_CXX_FLAGS="$DEFS" -DCMAKE_PREFIX_PATH=$PWD/aws-prefix
cmake --build odbc-build -j6
# -> src-repo/build/odbc/lib/libsqlodbc.so
```

Keep the driver and the AWS shared objects together and put that directory on
`LD_LIBRARY_PATH`:

```sh
mkdir -p /tmp/dbs/opensearch/lib
cp /tmp/dbs/opensearch/src-repo/build/odbc/lib/libsqlodbc.so /tmp/dbs/opensearch/lib/
cp /tmp/dbs/opensearch/aws-prefix/lib/*.so* /tmp/dbs/opensearch/lib/
export OPENSEARCH_ODBC_DRIVER=/tmp/dbs/opensearch/lib/libsqlodbc.so
export LD_LIBRARY_PATH=/tmp/dbs/opensearch/lib:$LD_LIBRARY_PATH
```

The driver reports `SQL_DRIVER_NAME` as `libsqlodbc.dylib` whatever it was built as — a
compiled-in constant, not the file it is loaded from.

### Loading the data (the entry cannot)

The SQL plugin is a query interface: `SELECT`, `SHOW` and `DESCRIBE` are the whole
grammar — there is no `CREATE TABLE` and no `INSERT`, an index comes into existence when
a document is written to it over the REST API — and the driver answers `SQLBindParameter`
with `OpenSearch does not support parameters`. Either alone would make the entry
`read_only=True`; together they leave no way at all to load a table over ODBC, so the
three indices are written first:

```sh
python3 tests/compat/fixtures/load_opensearch.py        # http://127.0.0.1:19200
# wrote adbc_t (2 docs), adbc_big (100000 docs) and adbc_search (3 docs) to ...
```

That script (stdlib only) creates each index with an explicit mapping — field types would
otherwise be guessed from the first document, and a `null` in it maps nothing — and
`_bulk`-writes `adbc_t` (the workload's two rows), `adbc_big` (100,000 `(a, b, c, d, e)`
documents) and `adbc_search` (three documents with an analysed `text` field, for the
full-text `extra` steps). It takes about two seconds and is idempotent: each document
carries its own `_id`, so a second run replaces the same documents.

It also raises one **cluster setting**, which is not about storage but about what SQL may
return: `plugins.query.size_limit` (default 10,000) caps a result set whatever its
`LIMIT` says, and the rows past it are dropped without a word — a 100,000-document
`adbc_big` would otherwise read back as 10,000 rows and `check_big()` would fail on the
count with nothing to explain it.

Run the entry:

```sh
ADBC_ODBC_DRIVER=$PWD/build/libadbc_driver_odbc.so \
  python tests/compat/test_matrix.py opensearch
# opensearch PASS  (OpenSearch (via ODBC) 3.8.0)
```

### Driver quirk: the ANSI `SQLDriverConnect` cannot connect at all

adbcbridge connects with the narrow `SQLDriverConnect`, which unixODBC hands straight to
a driver that exports one. Against this driver that returned `SQL_ERROR` **with no
diagnostic record at all**, while pyodbc — which calls `SQLDriverConnectW` — connected to
the same server with the same connection string.

The reason is in `CC_connect()` (`src/sqlodbc/opensearch_connection.cpp`):

```c
CC_determine_locale_encoding(self);      // hard-codes "SQL_ASCII" -- a TODO in the file
if (CC_is_in_unicode_driver(self)) {     // set only by SQLDriverConnectW
    if (!SQL_SUCCEEDED(CC_send_client_encoding(self, "UTF8"))) return 0;
} else {
    if (!SQL_SUCCEEDED(CC_send_client_encoding(self, self->locale_encoding))) return 0;
}
```

`SetClientEncoding` accepts only the encodings in `m_supported_client_encodings`, and
`SQL_ASCII` is not one of them — so on the ANSI path the connect always fails, and it
fails through `CheckRetVal`'s `"Error from CC_Connect"`, which goes to `CC_log_error`
rather than `CC_set_error`: hence `SQL_ERROR` with an empty diagnostic queue. A `dlopen`
of the driver calling its own `SQLDriverConnect` directly reproduces it with no driver
manager in the picture, which is what pinned it on the driver.

No entry in `OdbcDetectQuirks` can fix this: a quirk is keyed off a *live connection*,
and this is what fails to make one. adbcbridge therefore **retries a connect that failed
with an empty diagnostic queue through `SQLDriverConnectW`** (`OdbcDriverConnectWide` in
`src/odbc_driver.c`). The empty queue is the guard as much as the symptom: a connect that
failed *and said why* — bad credentials, no such host — is a real answer and is left
alone rather than attempted a second time, which against a locking-out server would cost
a second bad login. Everything after the connect — `SQLExecDirect`, `SQLDescribeCol`,
`SQLGetData`, `SQLColumns` — goes through the driver's ANSI entry points as before and
works, emoji included.

### Driver fix: `sem_init()` with the wrong initial count

With the connection working, the first `conn.close()` segfaulted inside
`OpenSearchResultQueue::clear()`. `opensearch_semaphore`'s POSIX branch does

```c
sem_init(&m_semaphore, 0, capacity);   // WIN32: CreateSemaphore(NULL, initial, capacity, ...)
                                       // APPLE: dispatch_semaphore_create(initial)
```

— `capacity` where the other two platforms use `initial`. The result queue constructs
`m_pop_semaphore(0, capacity)`, so on Linux the *pop* semaphore starts full instead of
empty: `pop()` succeeds on an empty queue, `std::queue::pop()` on an empty queue corrupts
it, and the next `clear()` deletes a garbage pointer. Fixing the argument (step (e)
above) fixes the crash. It is a bug in the driver's Linux path rather than anything
adbcbridge can work around, so it is a patch to the source you build.

### Types: what the SQL plugin and the driver between them can express

| flag | why |
|---|---|
| `read_only=True` | no DDL and no DML: the SQL plugin is query-only, and the driver has no `SQLBindParameter` — two independent reasons. The indices come from `fixtures/load_opensearch.py`. |
| `params=False` | `SQLBindParameter` answers `OpenSearch does not support parameters`; the parameterised query runs with a literal. |
| ``quote="`"`` | OpenSearch SQL quotes identifiers with the backtick. A `"..."` is not an identifier at all: `SELECT "a" FROM "adbc_big"` fails with ``no such index ["adbc_big"]``. Same fact as the `greptimedb` entry. |
| `ts_text=True` | `date` is OpenSearch's one temporal field type; the SQL plugin types it `DATE` when the format is date-only and `TIMESTAMP` when it carries a time. The driver's `type_to_oid_map` (`opensearch_parse_result.cpp`) maps `date` — to `SQL_TYPE_TIMESTAMP` — but has no entry for `timestamp` at all, a type name the plugin grew after the driver's last release, and anything unmapped falls back to VARCHAR. So `d` arrives as a timestamp and `ts` as the text `2024-02-29 13:45:10.123`; the workload parses it and checks the value as usual. |
| `binary_text="\\x0102"` | no binary type at all, so the two bytes are stored as text, exactly as for CrateDB and InfluxDB 3. |
| `decimal_type="string"` | no `DECIMAL` either; `n` is a `keyword` field read back as its exact digits. |
| `column_order=False` | `SQLColumns` reports an index's fields in mapping order, which is neither the workload's order nor alphabetical, so the catalog columns are compared as a set. |

Everything else in the workload runs unchanged: `int64`, `double`, `string` (including
`"héllo 🚀"` — the astral-plane emoji survives the round trip), `bool`, `DATE`, the
all-NULL row (a field a document does not have simply is not there, which is how a NULL
is spelled), the 100,000-row batched read, `GetObjects`, `GetTableSchema` and the error
text (`no such index [adbc_no_such_table]`). The timestamp is millisecond-resolution —
OpenSearch stores a date as epoch milliseconds — which the workload's `ts_us` default
already allows.

The `extra` steps run the thing only a search engine does: `MATCH` and `MATCH_QUERY`
full-text predicates over `adbc_search`'s analysed `text` field, plus two aggregations
pushed down to the engine over `adbc_big`.

Fetch: **~120k rows/s** over the 100,000-document `adbc_big` (`bench/matrix_bench.py`).
There is no ingest number — nothing can be written through this driver.

### Clean up

```sh
docker compose -f tests/compat/docker-compose.yml --profile extra down opensearch
# or, if started standalone:
docker rm -f adbcbridge-opensearch
```

## Apache Doris 2.1

Doris is an MPP analytic warehouse that serves the MySQL wire protocol on 9030, so it
needs no ODBC driver of its own: the same MySQL Connector/ODBC build used for the `mysql`
entry drives it (see [MySQL 8](#mysql-8) above for the root-free tarball, and for the
`LD_PRELOAD` that `import pyarrow` makes necessary).

```sh
export DORIS_ODBC_DRIVER=$MYSQL_ODBC_DRIVER
```

### Start the server

```sh
docker compose -f tests/compat/docker-compose.yml --profile extra up -d doris
# or standalone:
docker run -d --name adbcbridge-doris --memory=6g \
  -p 127.0.0.1:19031:9030 apache/doris:doris-all-in-one-2.1.0
```

The all-in-one image runs the frontend (FE) and one backend (BE) in a single container.
The image is large (7 GB) and takes a few minutes to pull; the server itself settles at
~2.5 GB resident under the 6 GB cap.

**It is not ready when the port opens.** The FE accepts connections almost immediately,
but the BE registers with it a little later, and until it has, every `CREATE TABLE` fails.
The condition to wait for is `Alive: true` in `SHOW BACKENDS`:

```sh
until docker exec adbcbridge-doris \
        mysql -uroot -P9030 -h127.0.0.1 -e 'SHOW BACKENDS\G' 2>/dev/null \
      | grep -q 'Alive: true'; do sleep 10; done
```

SQL is on `127.0.0.1:19031`, with the image's passwordless `root` account. There is no
user database, so the entry's `setup` runs `CREATE DATABASE IF NOT EXISTS adbc` and
`USE adbc` — both idempotent, which matters because `bench/matrix_bench.py` replays
`setup` on every connection it opens — and the connection string names no database.

`setup` also runs

```sql
ADMIN SET FRONTEND CONFIG ('force_olap_table_replication_num' = '1')
```

because a Doris table defaults to three replicas and this cluster has one backend:
without it every `CREATE TABLE` is refused with *"replication num should be less than the
number of available backends. replication num is 3, available backend num is 1"*. That is
a fact of a one-BE deployment, not of the workload, so it is set once on the FE rather
than written into each table's DDL. (`default_replication_num` does not exist as an FE
config in 2.1; `force_olap_table_replication_num` is the mutable knob that works.)

### Run the entry

```sh
LD_PRELOAD=/lib/x86_64-linux-gnu/libstdc++.so.6 \
ADBC_ODBC_DRIVER=$PWD/build/libadbc_driver_odbc.so \
  .venv/bin/python tests/compat/test_matrix.py doris
# doris     PASS  (MySQL (via ODBC) 5.7.99)
```

### Connection string: `NO_SSPS=1` and `PLUGIN_DIR`

`NO_SSPS=1` is what makes the entry work at all. Doris answers `COM_STMT_PREPARE` only
for a point `SELECT`:

```
SQLPrepare: errCode = 2, detailMessage = Only support prepare SelectStmt point query now
```

and an `INSERT` that reaches the server-side prepare path dies inside the FE with a bare

```
SQLExecDirectW: NullPointerException, msg: null (1105)
```

With `NO_SSPS=1` the connector stops using the server-side prepare protocol and
substitutes bound parameters into the SQL text, so every statement goes as a plain query —
the same setting `databend` and `greptimedb` need, for the same shape of reason.

`PLUGIN_DIR` is needed here as it is for TiDB, Dolt and MatrixOne: Doris offers
`mysql_native_password`, whose *client-side* plugin Connector/ODBC 9 loads at run time from
its compiled-in `/usr/local/mysql/lib/plugin`. The entry's connection string ends in
`{plugin_dir}`, which `conn_uri()` expands to the tarball's own `lib/plugin` when that
directory exists — see [TiDB](#tidb-75) for the full story.

### Already covered: the Databend quirk

Doris reports `SQL_TXN_CAPABLE` = `SQL_TC_NONE`, so the existing *"Connector/ODBC in front
of a server that is not a MySQL"* quirk in `src/odbc_driver.c` fires unchanged. It is
exactly what Doris needs. Under `NO_SSPS`, the connector writes date, timestamp and binary
parameters as MySQL charset-introducer literals, which Doris' parser rejects:

```
INSERT INTO pc VALUES (1, _binary'2024-02-29')
  errCode = 2, detailMessage = Syntax error in line 1
```

`temporal_binary_param_as_varchar` sends them as ordinary quoted text instead, and
`ansi_ddl_type_names` stops generated ingest DDL from using the MySQL type names
Connector/ODBC's `SQLGetTypeInfo` answers with whatever the server is (`long varchar` and
`bit`, neither of which Doris has). This is also the reason plain **pyodbc cannot ingest
into Doris at all** — the benchmark's pyodbc ingest column is empty because its `dt`
parameter goes as `_binary'...'` with nothing to rewrite it.

### Driver quirk: every table needs a distribution clause

Doris is an MPP store, so a table has to say how its rows are spread over the backends:

```
CREATE TABLE t (a BIGINT, b TEXT)
  errCode = 2, detailMessage = Create olap table should contain distribution desc
```

`adbc_ingest` builds its target table from the payload's columns and has no notion of
distribution, so this is a *server requirement the generated DDL cannot express* — the
case `ddl_table_options` exists for (GreptimeDB's `append_mode` is the other one). Keyed on
Doris in `OdbcDetectQuirks`, the generated DDL becomes

```sql
CREATE TABLE t (<ingested columns>)
  DISTRIBUTED BY RANDOM BUCKETS AUTO
  PROPERTIES ("enable_duplicate_without_keys_by_default" = "true")
```

Random distribution with an automatic bucket count is the neutral choice for a table whose
columns come from whatever the caller ingests. The property is not decoration: given no
key clause, Doris makes a duplicate-key table out of the *leading* columns, and then
refuses the table if the first of them is a string, float or double —

```
CREATE TABLE t (b TEXT, a BIGINT) DISTRIBUTED BY RANDOM BUCKETS AUTO
  errCode = 2, detailMessage = The olap table first column could not be float, double,
  string or array, struct, map, please use decimal or varchar instead.
```

— so an ingest payload whose first column happens to be text would fail on the `CREATE`.
`enable_duplicate_without_keys_by_default` asks for a duplicate table with no key columns
at all, which takes any column order and any column type.

**Detecting Doris is the awkward part.** It answers `SELECT version()` with a bare
`5.7.99` — it presents itself as a MySQL 5.7 and `SQL_DBMS_NAME` is `MySQL` — so the
version string that identifies GreptimeDB says nothing here. The one variable that names
it is `@@version_comment`:

```
mysql> SELECT @@version_comment;
Doris version doris-2.1.0-rc11-91efb6a43d
```

so the quirk asks for that, and only for a connection where `version()` did not already
identify the server, so GreptimeDB still pays for one query and MySQL, MariaDB, TiDB, Dolt,
Percona and MatrixOne (all transactional, so outside this branch entirely) pay for none.

### Entry notes

| flag | why |
|---|---|
| `ddl` `b VARCHAR(50)` | Doris has **no binary column type at all** — its parser does not know the word (`no viable alternative at input 'VARBINARY'`). A character column carries `b"\x01\x02"` through unchanged, as for ClickHouse and Databend. |
| `ddl … DISTRIBUTED BY RANDOM BUCKETS AUTO` | the same server requirement as above, for the one table this file writes itself. |
| `bool_type="int8"` | `BOOLEAN` goes over the MySQL wire as `TINYINT(1)` → `SQL_TINYINT`, exactly as MySQL's own does. |
| ``quote="`"`` | Doris **accepts** `ANSI_QUOTES` in `sql_mode` and then ignores it: with the mode set, `SELECT "a" FROM t` still returns the constant `'a'` for every row rather than the column, silently. Its identifiers are backtick-quoted, and since no `ANSI_QUOTES` is set Connector/ODBC reports the backtick as `SQL_IDENTIFIER_QUOTE_CHAR`, so `adbc_ingest` quotes correctly on its own — it is this file's SQL that has to be told. Same shape as `greptimedb`. |
| `ingest_types={pa.float64(): pa.decimal128(12, 3)}` | Doris has `DOUBLE` but neither `DOUBLE PRECISION` nor `REAL` (`extraneous input 'PRECISION'`), and `DOUBLE PRECISION` is what the portable ingest DDL asks for. Sending that column as a decimal — a type Doris names the same way — keeps create/append/replace ingest under test. `adbc_t`'s own `f DOUBLE` column is unaffected. Same fix as `greptimedb`. |
| `big_rows=2000` | ingest runs at ~2.2k rows/s (below), so 5000 rows would spend a couple of seconds for nothing; 2000 still crosses the reader's 1024-row batch boundary, which is what the step is for. |

Everything else runs on the generic path: the emoji round-trip, `DATE`, `DATETIME(6)`
microseconds, `DECIMAL(10,3)`, NULL parameters, affected-row counts, `GetObjects` /
`GetTableSchema` and the error path.

### Benchmark

```
ADBC_MATRIX_SUFFIX=_doris .venv/bin/python bench/matrix_bench.py \
  --rows 10000 --fetch-rows 100000 --pyodbc-timeout 300 doris
# fetch=1,401,474/s (pyodbc 362,998/s)  ingest=2,249/s array=2,342/s pyodbc=—/s
```

Reads are fast and ingest is not, which is what a warehouse of this shape looks like from
a row-at-a-time client: every `INSERT` is a separate load transaction the FE publishes
across the backend, so array binding buys nothing (2,342 against 2,249 rows/s). Bulk loads
into Doris are meant to go through Stream Load or `INSERT INTO ... SELECT`, neither of
which is an ODBC path. The pyodbc ingest column is empty because pyodbc cannot write a
date into Doris at all — see the `_binary` quirk above.

### Clean up

```sh
docker compose -f tests/compat/docker-compose.yml --profile extra down doris
# or, if started standalone:
docker rm -f adbcbridge-doris
```

## Apache Cloudberry 2.1.0-incubating (Greenplum fork)

Apache Cloudberry is the Greenplum fork: a **massively parallel (MPP)** cluster of
PostgreSQL segments behind a single coordinator. Each segment is a PostgreSQL 14
backend, the coordinator plans a query across them and merges the result, and every
table is hash-distributed (or replicated, or randomly distributed) across the segments.
It speaks the PostgreSQL wire protocol, so the same `psqlodbc` build used for the
`postgres` entry drives it — there is no Cloudberry ODBC driver.

### Get the ODBC driver without root

```sh
mkdir -p /tmp/adbc-drivers && cd /tmp/adbc-drivers
apt-get download odbc-postgresql
dpkg-deb -x odbc-postgresql_*.deb pgodbc
export CLOUDBERRY_ODBC_DRIVER=$PWD/pgodbc/usr/lib/x86_64-linux-gnu/odbc/psqlodbcw.so
export LD_LIBRARY_PATH=$PWD/pgodbc/usr/lib/x86_64-linux-gnu:$LD_LIBRARY_PATH
```

### Which image

There is **no Apache-published runnable server image**. The `apache/incubator-cloudberry`
repository on Docker Hub holds only CI toolchain tags — `cbdb-build-*` (compiler and
build dependencies) and `cbdb-test-*` (regression-test harnesses) — none of which start a
database, and `apache/cloudberry-db` does not exist at all (`pull access denied`). The
image used here is the community one from
<https://github.com/woblerr/docker-cloudberry>, which packages the released Cloudberry
2.1.0-incubating and initialises a cluster on first start.

### Start the server

```sh
docker compose -f tests/compat/docker-compose.yml --profile extra up -d cloudberry
# or standalone:
docker run -d --name adbcbridge-cloudberry \
  -p 127.0.0.1:15443:5432 --memory=3g --shm-size=1g \
  -e CLOUDBERRY_PASSWORD=adbc -e CLOUDBERRY_DATABASE_NAME=adbc \
  woblerr/cloudberry:2.1.0-incubating
```

The port is `15443` so the entry can run alongside the other PostgreSQL-wire entries.
`CLOUDBERRY_DEPLOYMENT` defaults to `singlenode`, which puts a coordinator **and two
primary segments** in the one container — that is what makes the cluster a real MPP
cluster rather than a single PostgreSQL server, and it is what the entry's `extra` steps
check.

`--shm-size=1g` is **not optional**: the segments communicate through POSIX shared
memory, and `gpinitsystem` fails against Docker's 64 MB default.

First start runs `gpinitsystem` (which builds the coordinator and both segments, then
restarts the cluster) and takes roughly one to two minutes. The container logs tail the
coordinator log, so it is ready when this succeeds:

```sh
docker exec -u gpadmin adbcbridge-cloudberry bash -lc \
  'source /usr/local/cloudberry-db/cloudberry-env.sh; psql -d adbc -tAc "SELECT version()"'
# PostgreSQL 14.4 (Apache Cloudberry 2.1.0-incubating build dev) on x86_64-pc-linux-gnu...
```

Note the `source` — the Cloudberry environment (`PATH`, `COORDINATOR_DATA_DIRECTORY`) is
set from `cloudberry-env.sh`, so `psql` is not on the default `PATH` of a non-login
shell.

The matrix connects as the superuser `gpadmin` with the password given as
`CLOUDBERRY_PASSWORD`; the image appends `host all all 0.0.0.0/0 md5` to `pg_hba.conf`
during initialisation, so a remote login works without further configuration.

### Run the entry

```sh
ADBC_ODBC_DRIVER=build/libadbc_driver_odbc.so \
  .venv/bin/python tests/compat/test_matrix.py cloudberry
# cloudberry PASS  (PostgreSQL (via ODBC) 14.0.4)
```

### Notes

The entry needs **no tolerance flags and no driver quirk** — Cloudberry passes the whole
workload through the unmodified PostgreSQL code path, and the DDL is the `postgres`
entry's unchanged: `INTEGER` is 32-bit, `DOUBLE PRECISION`, `VARCHAR(50)`, `BYTEA`,
`DATE`, `TIMESTAMP`, `NUMERIC(10,3)` and `BOOLEAN` all behave exactly as they do on stock
PostgreSQL, and no `PRIMARY KEY` is needed (unlike CockroachDB, a table without one gets
no synthesised extra column, so `SQLColumns` reports the eight declared columns).

`GetInfo` reports `PostgreSQL 14.0.4` because Cloudberry advertises a PostgreSQL server
version over the wire; `SELECT version()` is the only way to tell it apart, and
`SQL_DRIVER_NAME` is `psqlodbcw.so` for both. That is exactly why **no quirk keyed on
the driver name would be correct here** — it would also fire on real PostgreSQL,
CockroachDB and every other entry sharing this driver.

Because of that, the standard workload alone would be indistinguishable from `postgres`.
It does already run on the MPP engine — the 5000 rows the ingest step writes really do
hash-distribute across both segments:

```sh
docker exec -u gpadmin adbcbridge-cloudberry bash -lc \
  'source /usr/local/cloudberry-db/cloudberry-env.sh;
   psql -d adbc -c "SELECT gp_segment_id, count(*) FROM adbc_ing GROUP BY 1 ORDER BY 1"'
#  gp_segment_id | count
# ---------------+-------
#              0 |  2454
#              1 |  2546
```

— but nothing in it would *fail* on a single-node server. So the entry adds `extra`
steps covering the two things that make Cloudberry Cloudberry:

* a table declared `DISTRIBUTED BY ("a")`, bulk-ingested through `adbc_ingest` and then
  checked to occupy more than one segment (`count(DISTINCT gp_segment_id) > 1`), with an
  aggregate planned across the segments and merged on the coordinator;
* **append-optimized, column-oriented storage** — `WITH (appendonly=true,
  orientation=column)` — the Greenplum-family storage format stock PostgreSQL has no
  equivalent of. The ingest, the point read and the aggregate all run against it.

One version detail matters for that last check. Greenplum 6 and earlier recorded the
storage format in a `relstorage` column on `pg_class`; Cloudberry 2.x is PostgreSQL
14-based, where storage format is a **table access method**, so `relstorage` no longer
exists and the format is read from `pg_am` instead:

```sql
SELECT a.amname FROM pg_am a JOIN pg_class c ON c.relam = a.oid
 WHERE c.relname = 'adbc_ao';   -- ao_column
```

`adbc_t` itself stays a plain heap table, so the standard workload is unaffected.

### Clean up

```sh
docker compose -f tests/compat/docker-compose.yml --profile extra down cloudberry
# or, if started standalone:
docker rm -f adbcbridge-cloudberry
```

## YDB (PostgreSQL wire protocol)

YDB is a distributed HTAP database whose native interface is gRPC with its own query
language, YQL. It also serves a **PostgreSQL-compatible wire protocol**, and that is
what this entry drives, with the same `psqlodbc` build the `postgres` entry uses — there
is no YDB ODBC driver. Only the wire protocol and a thin `pg_catalog` emulation are
PostgreSQL's; the storage engine, the DDL rules and the catalog underneath are YDB's own.

### Get the ODBC driver without root

```sh
mkdir -p /tmp/adbc-drivers && cd /tmp/adbc-drivers
apt-get download odbc-postgresql
dpkg-deb -x odbc-postgresql_*.deb pgodbc
export YDB_ODBC_DRIVER=$PWD/pgodbc/usr/lib/x86_64-linux-gnu/odbc/psqlodbcw.so
export LD_LIBRARY_PATH=$PWD/pgodbc/usr/lib/x86_64-linux-gnu:$LD_LIBRARY_PATH
```

(or, if the `postgres` entry is already set up,
`export YDB_ODBC_DRIVER=$POSTGRES_ODBC_DRIVER` — it is the same library.)

### Start the server

```sh
docker compose -f tests/compat/docker-compose.yml --profile extra up -d ydb
# or standalone:
docker run -d --name adbcbridge-ydb -h localhost --memory=3g \
  -p 127.0.0.1:15444:5432 ydbplatform/local-ydb:latest \
  --enable-feature-flag enable_pg_syntax \
  --enable-feature-flag enable_table_pg_types \
  --enable-feature-flag enable_temp_tables
```

It takes about a minute to bring up its single-node cluster, and settles at roughly
500 MB resident. It is ready when the container reports healthy, or equivalently when
this succeeds:

```sh
docker exec adbcbridge-ydb /health_check
```

#### The feature flags, and why they are arguments rather than environment variables

The PG listener on 5432 is open from the first start, but the SQL behind it is refused —
`SQLDriverConnect` fails with `Status: GENERIC_ERROR ... Error: PG syntax is disabled` —
until `enable_pg_syntax` is on. YDB's own documentation sets these through
`YDB_FEATURE_FLAGS` (alongside `POSTGRES_PORT`, `YDB_USE_IN_MEMORY_PDISKS` and friends),
but **this image's entrypoint reads none of those variables**: `/initialize_local_ydb`
passes its own arguments straight through to `local_ydb deploy`, whose flag option is
`--enable-feature-flag`. The port is fixed at 5432 by the `--fixed-ports` the entrypoint
always passes, so publishing `15444:5432` is all the port mapping needed.

The names must be the **snake_case protobuf field names** of
`NKikimrConfig.TFeatureFlags`, which is what the generated `config.yaml` uses throughout.
A name in the CamelCase spelling the binary's own strings show (`EnablePgSyntax`) is
written into that config verbatim and then rejected by `ydbd` at startup:

```
Caught exception: .../json2proto.cpp:594: unknown field "EnablePgSyntax"
  for "NKikimrConfig.TFeatureFlags"
```

`ydbd` exits, `local_ydb deploy` never returns, and the container sits `unhealthy` with
nothing in `docker logs` but `[ydb|init] Starting YDB...`. The message is only in
`/ydb_data/cluster/node_1/stderr` inside the container — worth knowing, because that is
the shape of *any* bad flag name here.

#### Create the role the matrix connects as

The image ships **no users at all** (`ALTER USER root ...` answers `User not found`), and
login authentication is on, so the PG wire answers `FATAL: UNAUTHORIZED` for every
name/password pair and `fe_sendauth: no password supplied` when the password is empty —
`libpq` refuses to send an empty one. Create a role once the container is up (the
`docker exec` lines below name the standalone container; under compose it is
`compat-ydb-1`):

```sh
docker exec adbcbridge-ydb /ydb --endpoint grpc://localhost:2136 --database /local \
  --no-discovery sql -s "CREATE USER adbcuser PASSWORD 'Ydb!Bridge2026'"
docker exec adbcbridge-ydb /ydb --endpoint grpc://localhost:2136 --database /local \
  --no-discovery sql -s 'GRANT ALL ON `/local` TO adbcuser'
```

Both are needed: without the `GRANT` the role logs in and queries, but every
`CREATE TABLE` comes back `UNAUTHORIZED ... Access denied for scheme request`. The
password may not contain the user name (`Password must not contain user name`), which
rules out the `adbc`/`adbc` pair the other entries use.

### Run the entry

```sh
ADBC_ODBC_DRIVER=$PWD/build/libadbc_driver_odbc.so \
  .venv/bin/python tests/compat/test_matrix.py ydb
# ydb       PASS  (PostgreSQL (via ODBC) 14.0.5)
```

### The connection string: two psqlodbc settings

```
Driver={drv};Server=127.0.0.1;Port=15444;Database=local;Uid=adbcuser;
Pwd=Ydb!Bridge2026;BoolsAsChar=0;UseServerSidePrepare=0;
```

`Database=local` is the database the single-node image creates (YDB path `/local`).

**`BoolsAsChar=0`** — without it psqlodbc reports every `BOOLEAN` column as a
`VARCHAR(5)` holding `"1"`/`"0"` instead of `SQL_BIT`, so `bo` would read back as a
string. The `questdb` and `arcadedb` entries need the same setting for the same reason.

**`UseServerSidePrepare=0`** is what makes any NULL reachable at all — see below.

### Server limitation: no NULL bind parameter

YDB's PG wire does not implement a NULL parameter. In the extended query protocol a
`Bind` message gives each parameter a length, with **-1** meaning NULL; YDB reads that as
a zero-length *value* instead. Through ODBC that surfaces as a different failure per
column type, none of which mentions NULL:

| column type | what a bound NULL does |
|---|---|
| `TEXT`, `VARCHAR` | stored as `''` — **silently**, no diagnostic |
| `BYTEA` | stored as `b''` — silently |
| `INTEGER`, `DATE`, `TIMESTAMP`, `NUMERIC`, `BOOLEAN` | `ERROR: invalid input syntax for type <t>: ""` |
| a row with several NULLs | `Fatal: mkql_terminator.cpp:45: ERROR: invalid byte sequence for encoding "UTF8": 0xff`, and the connection is dropped |

This is the server, not psqlodbc and not adbcbridge, and a ~70-line stdlib PostgreSQL v3
frontend with no ODBC anywhere in the path shows it: connect, create
`(i int4 PRIMARY KEY, v int4, s text)`, then

* `INSERT INTO pgw VALUES (1, NULL, NULL)` over the **simple** query protocol → `ok`;
* the same insert as `Parse`/`Bind`/`Execute` with parameter lengths `-1` → the server
  closes the socket without even an `ErrorResponse`.

A literal `NULL` in the SQL text is handled correctly, which is what the fix uses:
psqlodbc's **`UseServerSidePrepare=0`** stops it using the extended protocol at all and
substitutes bound values into the statement text, where a NULL becomes the literal
`NULL`. With that one setting the whole workload — the all-NULL second row, the NULLs in
the ingest payload — goes through unchanged, and the entry needs no `null_params=False`.

### Driver quirks: two, both on existing flags

Both are keyed on the server, not on the driver name — `psqlodbcw.so` drives ten servers
in this matrix and a name-keyed quirk would fire on real PostgreSQL. See
`OdbcDetectQuirks` in `src/odbc_driver.c`.

#### Identifying YDB at all

YDB is the only PostgreSQL-wire server here that `SELECT version()` does **not** name:

```
PostgreSQL 16.10 on x86_64-pc-linux-gnu, compiled by clang version 20.1.8, 64-bit
```

It does name itself in the `server_version` **ParameterStatus** of the startup handshake
— `14.5 (ydb stable-23-4)` — but psqlodbc keeps that to itself and reports the bare
`14.0.5` as `SQL_DBMS_VER`, so nothing reachable through ODBC carries the string "ydb".
What YDB *does* do differently is map the `server_version` **setting** to `version()`
itself, so `SHOW server_version` hands back the whole banner where every other server on
this wire answers with a bare version number:

| server | `SELECT version()` | `SHOW server_version` |
|---|---|---|
| PostgreSQL 16 | `PostgreSQL 16.15 (Debian ...) on x86_64-...` | `16.15 (Debian 16.15-1.pgdg13+2)` |
| YDB | `PostgreSQL 16.10 on x86_64-...` | `PostgreSQL 16.10 on x86_64-...` |

So the quirk asks for both and fires only when they are the same string. That extra query
runs only for `psqlodbc` connections whose `version()` matched no other marker.

#### Quirk 1: every table needs a PRIMARY KEY (`ddl_extra_column`)

YDB refuses any `CREATE TABLE` that does not declare one:

```
Error: Pre type annotation, code: 1020
  <main>:1:1: Error: Primary key is required for ydb tables.
```

`adbc_t` can simply declare `i INTEGER PRIMARY KEY`, the way the `cockroachdb` and
`matrixone` entries do. The **generated ingest DDL** cannot: `adbc_ingest` builds
`CREATE TABLE t (<the payload's columns>)` and no ingested column is a candidate — a YDB
primary key is implicitly `NOT NULL` (`Tried to insert NULL value into NOT NULL column`)
and any ingested column may be NULL. So the driver appends a key the server fills in
itself, exactly as GreptimeDB's mandatory `TIME INDEX` column is appended:

```c
conn->reader_opts.ddl_extra_column = "adbc_pk SERIAL PRIMARY KEY";
```

The ingested columns are untouched, and `SERIAL` numbers the rows as they arrive
(verified through a multi-row `INSERT` and through `mode="replace"`).

#### Quirk 2: `pg_attribute` is empty (`no_sql_columns`)

YDB lists its tables in `pg_catalog.pg_class` — `SQLTables` works — but
`pg_catalog.pg_attribute` and `pg_catalog.pg_attrdef` are both **empty**
(`SELECT count(*)` → 0). psqlodbc's `SQLColumns` joins `pg_class` to `pg_attribute`, so
it returns `SQL_SUCCESS` with a zero-row result set and every table looks like it has no
columns. Same shape as ArcadeDB (where that query fails on the server instead), and the
same existing flag:

```c
conn->reader_opts.no_sql_columns = true;
```

`GetObjects` and `GetTableSchema` then describe `SELECT * FROM <table> WHERE 1=0`, which
is where `GetTableSchema` already got a table's columns from. `pg_settings` is empty too;
nothing here needs it.

### Entry notes

The rest of the workload is plain PostgreSQL and needs a single tolerance flag:

| flag | why |
|---|---|
| `decimal_type="decimal128(28, 3)"` | YDB does not report the declared precision of a `NUMERIC` over the wire, so psqlodbc falls back to its own maximum (28) with the column's scale — as it does for QuestDB's `DECIMAL` and RisingWave's unqualified `NUMERIC` |

Everything else round-trips exactly: `INTEGER`, `DOUBLE PRECISION`, `VARCHAR(50)`
(including `"héllo 🚀"` — the astral-plane emoji survives), `BYTEA` (`b"\x01\x02"`),
`DATE`, `TIMESTAMP` at full microsecond precision (`.123456`, not rounded), `BOOLEAN`,
the all-NULL second row, the parameterised `SELECT`, bulk ingest in all three modes, the
5,000-row batched read, `GetObjects`, `GetTableSchema` and the error text
(`Cannot find table 'db.[/local/adbc_no_such_table]'`).

### Benchmark

```sh
ADBC_MATRIX_SUFFIX=_ydb python bench/matrix_bench.py \
  --rows 10000 --fetch-rows 100000 --pyodbc-timeout 300 ydb
```

### Clean up

```sh
docker compose -f tests/compat/docker-compose.yml --profile extra down ydb
# or, if started standalone:
docker rm -f adbcbridge-ydb
```

## Vertica 25.3

Vertica is a columnar analytics warehouse (OpenText Analytics Database since the
acquisition). It is in this matrix because it is one of the few remaining databases with
a first-party ODBC driver of its own: `libverticaodbc.so` speaks Vertica's native
protocol on 5433 — despite the port and despite Vertica's PostgreSQL ancestry, this is
**not** a PostgreSQL wire and psqlodbc cannot drive it.

It is the cleanest entry in the matrix. No tolerance flags, no `ingest_types`, no
`setup` — every one of the workload's eight types is a native Vertica type that
round-trips exactly:

```python
i int64, f double, s string, b binary, d date32[day],
ts timestamp[us], n decimal128(10, 3), bo bool
```

(`i` is `int64` because Vertica has exactly one integer type: `INT`, `INTEGER`,
`BIGINT`, `SMALLINT` and `TINYINT` are all 64-bit aliases of it.)

### Get the ODBC driver without root

The Linux client package is a plain tarball on vertica.com — no account, no
registration, and it unpacks anywhere:

```sh
mkdir -p /tmp/dbs/vertica && cd /tmp/dbs/vertica
curl -sSLO https://www.vertica.com/client_drivers/25.1.x/25.1.0-0/vertica-client-25.1.0-0.x86_64.tar.gz
tar xzf vertica-client-25.1.0-0.x86_64.tar.gz     # -> opt/vertica/{lib64,bin,en-US,include}
```

25.1 is the newest client published at that path; it drives the 25.3 server below
without complaint (and the tarball also carries `vsql`, which is handy for poking at the
server directly). The driver links only against the system `libstdc++`/`libcrypt`, so it
needs no `LD_LIBRARY_PATH` entry.

Unlike every other driver here, it will not load until it finds a **`vertica.ini` of its
own**, located by the `VERTICAINI` environment variable. It is what tells the driver
which driver manager it is running under and where its message catalogue is:

```sh
cat > /tmp/dbs/vertica/vertica.ini <<EOF
[Driver]
ErrorMessagesPath = /tmp/dbs/vertica/opt/vertica
ODBCInstLib = /usr/lib/x86_64-linux-gnu/libodbcinst.so
DriverManagerEncoding = UTF-16
EOF
export VERTICAINI=/tmp/dbs/vertica/vertica.ini
```

`DriverManagerEncoding = UTF-16` is the one line that matters for correctness: unixODBC
is built with a 2-byte `SQLWCHAR`, and the driver defaults to UTF-32 (the DataDirect
convention). Get it wrong and every wide string comes back mangled — with UTF-16 set,
`héllo 🚀` round-trips including the astral-plane emoji. `ErrorMessagesPath` points at
the directory *containing* `en-US/`, not at `en-US` itself.

### Start the server

There is no `vertica/vertica-ce` image any more. The `vertica` Docker Hub namespace is
empty, and the `opentext` one that replaced it publishes no CE image — what it does
publish is `opentext/vertica-k8s`, the full server built for the VerticaDB Kubernetes
operator. That is what this entry uses, with the Community Edition licence the image
still ships in `/opt/vertica/config/licensing`.

```sh
docker compose -f tests/compat/docker-compose.yml --profile extra up -d vertica
tests/compat/fixtures/setup_vertica.sh
# compat-vertica-1: creating database VMart on 172.21.0.3 (a few minutes)
# ...
# Vertica Analytic Database v25.3.0-8
# compat-vertica-1: ready
```

The second command is not optional and is the whole awkwardness of this entry: the image
ships a server with **no database and no way to make one**. The operator normally creates
it from outside the pod, so there is nothing to hang off an entrypoint. The script does
the four steps by hand and takes about half a minute; its header comments carry the
detail, but in short:

1. **Recreate `dbadmin`.** The image's `/etc/passwd` has no such account, yet every file
   it ships is owned by uid 997 / gid 995 and the server refuses to run as anyone else.
   This is why the compose service runs as `user: "0"` — root is the only user that can
   add one — and why the server itself is then started as `dbadmin`.
2. **Generate TLS certificates.** The node management agent (the local agent `vcluster`
   drives, in place of the admintools-over-SSH of the pre-24.x images) will not start
   without a key pair and a CA to chain it to, which the operator would mount in. A
   self-signed set generated in place is all it wants; nothing outside the container ever
   presents them and the SQL port does not use them.
3. **`vcluster create_db`**, which builds the catalog and starts the node.
4. **Wait on the SQL port ourselves** — see below.

Two settings on the compose service are load-bearing:

* `ulimits: nofile: 65536`. Vertica's start-up check refuses to run under Docker's
  default 1024: *"Host does not meet minimum requirements: Not enough open file handles
  allowed (1024 available/32768 required)"*, and the catalog read fails.
* `image: opentext/vertica-k8s:25.3.0-8-minimal`, pinned rather than `latest`. **Vertica
  26.1 dropped Community Edition**, and a 26.x server refuses the licence its own image
  ships:

  ```
  Community Edition (CE) license is deprecated and no longer supported in
  OpenText Analytics Database (Vertica) 26.1 and later
  ```

  That kills the catalog bootstrap outright, so 25.x is the ceiling for a
  licence-free run.

Host port 15433 is also the `yugabyte` entry's, but neither is in the default compose
profile and they are never up together.

#### `create_db` reports failure on a database that is running

The last thing `create_db` does is poll the server's **HTTPS** service (8443) to confirm
the node is up, and that service has no certificate: the image's `httpstls.json` ships
with an empty `key` and `certificate`. So the poll fails the TLS handshake and the
command ends with

```
✘ Wait for 1 node(s) to come up: failed
Error during execution: execute HTTPSPollNodeStateOp failed, ... reached polling timeout
```

while `vertica.log` shows the node perfectly healthy (`Task 'RebalanceCluster' enabled`,
transactions committing) and `vsql` answers on 5433. Only the health check is broken, not
the database. The script therefore passes `--startup-timeout 30` to stop `create_db`
spending its default 300 seconds on a poll that cannot succeed, and then waits on the SQL
port — the only one the matrix uses — itself.

### Run the entry

```sh
export VERTICA_ODBC_DRIVER=/tmp/dbs/vertica/opt/vertica/lib64/libverticaodbc.so
export VERTICAINI=/tmp/dbs/vertica/vertica.ini
ADBC_ODBC_DRIVER=$PWD/build/libadbc_driver_odbc.so ADBC_ODBC_DELEGATE=never \
  ADBC_MATRIX_SUFFIX=_vertica python tests/compat/test_matrix.py vertica
# vertica   PASS  (Vertica Database (via ODBC) 25.03.0000)
```

The driver answers `SQL_DRIVER_NAME` `verticaodbcw.so` and `SQL_DBMS_NAME`
`Vertica Database`; `"verticaodbc"` is what the driver's one quirk keys on.

### Driver quirk: parameter arrays beat multi-row INSERT

adbcbridge's default bulk-ingest path packs K rows into one
`INSERT ... VALUES (...),(...)`, because that is faster than ODBC parameter arrays on
every other server measured. Vertica is the second exception after MariaDB
Connector/ODBC, and by a much wider margin: its driver turns a bound array into a single
native bulk load, while a multi-row INSERT stays one row-store insert per statement —
which is the worst case for a column store. Timing 10,000-row ingests directly:

| path | rows/s |
| --- | --- |
| `adbc.odbc.array_binding=false` (multi-row INSERT) | 17,122 |
| `adbc.odbc.array_binding=true` (parameter arrays) | 138,720 |
| driver default, with the quirk | 131,773 |

So `OdbcDetectQuirks` sets `prefer_param_arrays` for `verticaodbc`, which is the existing
flag `maodbc` already uses — an 8× improvement for a caller that sets no options. Note
that `bench/matrix_bench.py` pins `array_binding` explicitly in both of its ingest
columns, so neither of them shows this: the quirk only moves what happens when nothing is
set.

### Notes

* **No transaction quirk needed.** Vertica reports `SQL_TXN_CAPABLE` = 3 and honours
  commit/rollback normally.
* **`GETDATA_EXTENSIONS` = 15** (`SQL_GD_ANY_COLUMN | SQL_GD_ANY_ORDER | SQL_GD_BLOCK |
  SQL_GD_BOUND`), so the reader binds wide columns and repairs truncated values in place;
  no `refetch_repair` fallback is involved.
* **`SQL_MAX_STATEMENT_LEN` = 0** (the driver will not say), so the multi-row INSERT
  batch size is settled by probing, as it is for most drivers.
* The generated ingest DDL comes out as Vertica's own `SQLGetTypeInfo` names —
  `int`, `long varchar(1048576)`, `float`, `date`, `boolean` — all of which it accepts, so
  the entry needs neither `ansi_ddl_type_names` nor `ingest_types`.

### Benchmark

10,000-row ingest, 100,000-row fetch:

```
fetch=948,103/s (pyodbc 436,509/s)  ingest=15,949/s array=151,356/s pyodbc=131,357/s
```

The fetch is the second-fastest in the matrix — a column store handing over a wide block
cursor is exactly what the reader's bound-block path is good at — and 2.2× pyodbc.

### Clean up

```sh
docker compose -f tests/compat/docker-compose.yml --profile extra down vertica
```

The database lives in the container's own filesystem (`/home/dbadmin/data`), so removing
the container removes it; `setup_vertica.sh` starts over from scratch on the next one.

## OceanBase CE 4.4.2 (MySQL 5.7.25 wire)

[OceanBase](https://github.com/oceanbase/oceanbase) is a distributed, multi-tenant HTAP
database. Its MySQL mode serves the MySQL wire protocol — it announces itself as
`5.7.25-OceanBase_CE-v4.4.2.1` — so it needs no ODBC driver of its own: the same MySQL
Connector/ODBC build used for the `mysql` entry drives it (see [MySQL 8](#mysql-8) above
for the root-free tarball, and for the `LD_PRELOAD` that `import pyarrow` makes
necessary).

```sh
export OCEANBASE_ODBC_DRIVER=$MYSQL_ODBC_DRIVER   # the tarball's libmyodbc9w.so
```

### Start the server

```sh
docker compose -f tests/compat/docker-compose.yml --profile extra up -d oceanbase
# or standalone:
docker run -d --name adbcbridge-oceanbase --memory=6g \
  --ulimit nofile=20000:20000 --ulimit stack=-1:-1 \
  -p 127.0.0.1:12881:2881 \
  -e MODE=SLIM -e OB_TENANT_NAME=test -e OB_TENANT_PASSWORD=adbc -e OB_DATABASE=adbc \
  oceanbase/oceanbase-ce:latest
```

Wait for the log line, not for the port — the MySQL listener answers minutes before the
tenant behind it does:

```sh
until docker logs adbcbridge-oceanbase 2>&1 | grep -q "boot success"; do sleep 5; done
```

That takes about a minute from an already-pulled image, and the container settles at
~2.6 GiB, peaking at ~4.0 GiB during the benchmark — comfortably inside the 6 GB cap.
The image itself is ~4 GB, so the first `docker pull` is the long part.

Three things about the container are worth spelling out, because two of them are the
difference between a server and a failed boot.

**`--ulimit nofile=20000`.** `obd`, the deployer the image drives everything through,
refuses to work below 20,000 open files:

```
[ERROR] OBD-1007: The value of the ulimit parameter "open files" must not be less
        than 20000 (Current value: 1024)
```

Docker's default is 1024, so without the flag the boot stops there. It is settable
per-container and needs no root on the host.

**`MODE=SLIM`, and why not the default `MODE=MINI`.** This is not a size knob. The
entrypoint has two paths: `MINI` (the default) has `obd` deploy a cluster and then
*create* the `test` tenant, while `SLIM` unpacks a **prebuilt** single-node cluster
(squashfs images of the data and clog directories, `/root/demo/store.img` and
`etc.img`) and starts that. On this box the `MINI` path does not finish. Tenant creation
ends in

```
ALTER SYSTEM LOAD MODULE DATA module = timezone tenant = 'test' infile = 'etc/'
```

which loads the 118,610 rows of `etc/timezone_V1.log` into `mysql.time_zone_transition`.
Measured, that ran at ~4 rows/s — hours of work against the 1000-second
`ob_query_timeout` `obd` sets for it, so the tenant create times out, `deploy_failed`
runs and the container exits. `SLIM` never runs it: the prebuilt store already holds the
tenant *and* its populated timezone tables. (`MINI` also needs its memory and log-disk
sizes overridden as a set — `OB_MEMORY_LIMIT` defaults to 6 GB, which does not fit under
a 6 GB cap, and the MINI `OB_LOG_DISK_SIZE` leaves the tenant with "not enough log_disk.
(Available: 0, Need: ...)". None of that is needed on the SLIM path, whose sizes are
baked into the prebuilt config: `memory_limit` 6144M, `system_memory` 1024M.)

**`OB_TENANT_NAME` / `OB_TENANT_PASSWORD` / `OB_DATABASE` are honoured on the SLIM path
too.** Once the cluster is up the entrypoint waits for the tenant to answer, runs
`CREATE DATABASE IF NOT EXISTS adbc` in it, and then `ALTER USER root IDENTIFIED BY
'adbc'` — the last three lines of the log before `boot success`.

### Run the entry

```sh
LD_PRELOAD=/lib/x86_64-linux-gnu/libstdc++.so.6 \
ADBC_ODBC_DRIVER=$PWD/build/libadbc_driver_odbc.so \
ADBC_MATRIX_SUFFIX=_oceanbase ADBC_ODBC_DELEGATE=never \
  .venv/bin/python tests/compat/test_matrix.py oceanbase
# oceanbase PASS  (MySQL (via ODBC) 5.7.25)
```

### The user name carries the tenant: `root@test`

OceanBase is multi-tenant, and a login name is `user@tenant`. The connection string
therefore says `User=root@test`, not `User=root`. `test` is the business tenant — the one
the image creates (`OB_TENANT_NAME`) and the one `OB_DATABASE=adbc` creates the database
in. `root@sys` is the *cluster's* administrative tenant: it is where `ALTER SYSTEM` and
the `gv$` views live, not where user databases belong, and its `oceanbase` schema is not
what this workload wants to be pointed at. The `@` means nothing to Connector/ODBC —
it passes the whole string through as the MySQL user name — so no escaping is involved.

`PLUGIN_DIR` is needed here for the same reason as for TiDB, Dolt and MatrixOne:
OceanBase offers only `mysql_native_password`, whose *client-side* plugin Connector/ODBC 9
loads at run time from its compiled-in `/usr/local/mysql/lib/plugin`. The entry's
connection string ends in `{plugin_dir}`, which `conn_uri()` expands to the tarball's own
`lib/plugin` when that directory exists — see [TiDB](#tidb-75) for the full story.

### Quirks: none

There is no `oceanbase` key in `OdbcDetectQuirks`, and the entry needs no tolerance flag
that the `mysql` entry does not. The whole standard workload runs on the generic path on
the first try: the emoji round-trip, `VARBINARY(10)`, `DATETIME(6)` microseconds,
`DECIMAL(10,3)`, NULL parameters, affected-row counts, `GetObjects`/`GetTableSchema`, and
the 5000-row batched ingest and read. The entry is the `mysql` entry's DDL with the
`mysql` entry's two tolerances, for the same two reasons:

* `bool_type="int8"` — `BOOLEAN` is `TINYINT(1)`, which the driver reports as
  `SQL_TINYINT`.
* `setup=["SET SESSION sql_mode = CONCAT(@@sql_mode, ',ANSI_QUOTES')"]` — the
  double-quoted identifiers `adbc_ingest` emits. OceanBase's MySQL mode implements
  `ANSI_QUOTES` exactly as MySQL does; its stock `sql_mode` is
  `STRICT_ALL_TABLES,NO_ZERO_IN_DATE,NO_AUTO_CREATE_USER`.

Unlike MatrixOne, a table declared without a `PRIMARY KEY` grows no hidden column here,
so `adbc_t` needs no key and `GetObjects` reports exactly the eight columns.

### One noise line that is not a problem

Every connection prints this to stderr, once, before it succeeds:

```
Character set '45' is not a compiled character set and is not specified in the
'/usr/local/mysql/share/charsets/Index.xml' file
```

It is `libmysqlclient` inside Connector/ODBC, not adbcbridge, and it is cosmetic. 45 is
`utf8mb4_general_ci`, OceanBase's default collation; the client library has no compiled-in
entry for that id and looks for the charset index at the path the *generic tarball* was
built with, which an unpacked-elsewhere tarball does not have. The connection is
negotiated as UTF-8 regardless — checked directly rather than assumed:

```
SELECT @@character_set_client, @@character_set_connection, @@character_set_results,
       @@collation_connection
-- ('utf8mb4', 'utf8mb4', 'utf8mb4', 'utf8mb4_general_ci')
SELECT HEX('🚀')   -- F09F9A80
```

so the emoji round-trips byte for byte and the matrix's `astral` assertion passes. MySQL 8
does not print it because its default collation (`utf8mb4_0900_ai_ci`, 255) *is* compiled
in.

### Benchmark

```
ADBC_MATRIX_SUFFIX=_oceanbase .venv/bin/python bench/matrix_bench.py \
  --rows 10000 --fetch-rows 100000 --pyodbc-timeout 300 oceanbase
# fetch=1,217,853/s (pyodbc 689,517/s)  ingest=81,912/s array=66,921/s pyodbc=16,328/s
```

Ingest is the fastest of the MySQL-wire entries by a wide margin: 82k rows/s, against
MySQL 8.4's own 8.5k and MatrixOne's 4.4k through the same driver. The multi-row `INSERT`
the ingest path builds is what earns it — the same payload through pyodbc `executemany`,
which sends one statement per row, runs at 16.3k. Parameter arrays are *slower* than the
multi-row form here (67k rows/s), because Connector/ODBC executes a bound array row by
row, so the default is already the right choice and `prefer_param_arrays` is not wanted.

### Clean up

```sh
docker compose -f tests/compat/docker-compose.yml --profile extra down oceanbase
# or, if started standalone:
docker rm -f adbcbridge-oceanbase
```

## Dremio 26 (OSS, Arrow Flight SQL)

Dremio is a lakehouse query engine, and Arrow Flight SQL is its *native* client
protocol — port 32010, alongside the web UI and REST API on 9047. The ODBC route is
therefore the same Arrow Flight SQL ODBC driver the
[`flightsql`](#arrow-flight-sql-sqlflite-155--duckdb-111) and
[`influxdb3`](#influxdb-3-core-arrow-flight-sql) entries use — read the `flightsql`
section first: it is Dremio's own driver, and everything documented there (no
`SQLBindParameter`, `SQLColumns` segfaults on the first `SQLFetch`, decimals described
with precision 19) is the driver's and therefore true here too. What this entry adds is
the driver run against the engine it was written for.

Server:

```sh
docker run -d --name adbcbridge-dremio --memory=5g \
  -p 127.0.0.1:9047:9047 -p 127.0.0.1:32010:32010 dremio/dremio-oss:latest
```

(or `docker compose -f tests/compat/docker-compose.yml --profile extra up -d dremio`;
it is in the `extra` profile, so a plain `up -d` leaves it alone). It is ready in about
15 seconds — `Dremio Daemon Started as master` in `docker logs`, and
`curl -s http://127.0.0.1:9047/apiv2/server_status` answers `"OK"`. The image is 1.4 GB;
the JVM settles at about 1.5 GB resident with the image's own heap settings, so
`--memory=5g` leaves plenty of room.

### First start: there are no users, and every login fails until you make one

A fresh Dremio has an empty user store and refuses all authentication — including the
Flight SQL handshake — until the first admin exists. It is created over one unauthenticated
REST call (`_dremionull` is the literal placeholder token that endpoint expects):

```sh
curl -s -X PUT http://127.0.0.1:9047/apiv2/bootstrap/firstuser \
  -H 'Authorization: _dremionull' -H 'Content-Type: application/json' \
  -d '{"userName":"adbc","firstName":"adbc","lastName":"bridge",
       "email":"adbc@example.com","createdAt":1700000000000,
       "password":"Adbc2026pass"}'
# {"resourcePath":"/user/adbc","userName":"adbc", ...}
```

The password has to be at least 8 characters with a letter and a digit -- `"short"`
comes back `400`. Run it once per container; a second call answers `First user can only
be created when no user is already registered`.

Driver: none to install — this is the same library the `flightsql` entry extracts, so
point a third variable at it:

```sh
export DREMIO_ODBC_DRIVER=$FLIGHTSQL_ODBC_DRIVER
```

Run the entry:

```sh
ADBC_ODBC_DRIVER=$PWD/build/libadbc_driver_odbc.so \
  python tests/compat/test_matrix.py dremio
# dremio    PASS  (Dremio Server (via ODBC) 26.00.0005-202509091642240013-f5051a07)
```

### The query context is a connection property, not a `USE`

Flight SQL has no "connect to database X" step. Dremio reads the default context from a
gRPC header named `schema`, and the driver forwards every connection property it does not
recognise as a header of exactly that name — the same mechanism the `influxdb3` entry uses
for `database`:

```
Driver={drv};Host=127.0.0.1;Port=32010;UID=adbc;PWD=Adbc2026pass;useEncryption=false;schema=$scratch;
```

`useEncryption=false` matches the container's plain-gRPC listener: the image's
`conf/dremio.conf` sets nothing under `services.flight` but `use_session_service`, so the
Flight service comes up without TLS (`Flight Service started at ... on port 32010` in the
log). `UID`/`PWD` are the admin created above; the driver turns them into Dremio's bearer
token.

### Loading the data: `$scratch`, and why the entry is still read-only

`$scratch` is the one writable source a stock dremio-oss has — a filesystem source under
`/opt/dremio/data/pdfs/scratch`. `CREATE TABLE ... AS SELECT`, `DROP TABLE` and, on a
table created there with an explicit column list (that makes it an Iceberg table),
`INSERT` all work through the ODBC connection. **Dremio is not the reason this entry is
read-only.** A CTAS table is the one thing that cannot take DML:

```
INSERT INTO adbc_t VALUES (3, ...)
-- Table ["$scratch".adbc_t] is not configured to support DML operations
CREATE TABLE adbc_ice (i INT); INSERT INTO adbc_ice VALUES (1), (2)   -- both fine
```

The driver is. It answers `SQLBindParameter` with `HYC00 "Unsupported function"` on a
virgin statement handle, and after a `SQLPrepare` that *succeeds* on
`INSERT INTO adbc_ice (i) VALUES (?)` it reports `SQLNumParams` = 0 and refuses the bind
again — exactly as the `flightsql` section documents, and confirmed here against Dremio:

```
virgin SQLBindParameter rc -1 (HYC00, 100, [Apache Arrow][Flight SQL] Unsupported function.)
prepare rc 0    SQLNumParams n = 0    bind rc -1 (HYC00, ...)
execute rc -1 (HY000, ... Illegal use of dynamic parameter)
```

So no parameter can reach a server that would happily take one, which rules out both the
parameterised `INSERT` the other entries load `adbc_t` with and `adbc_ingest`, and the
entry is `read_only=True`. `SQLExecDirect` of literal SQL works fine, so `setup` builds
both tables itself and the read side of the workload then runs unchanged. `setup` is
replayed on every connection (`bench/matrix_bench.py` opens several), and Dremio has no
`CREATE OR REPLACE TABLE`, so each statement is `CREATE TABLE IF NOT EXISTS ... AS SELECT`
— a no-op costing about half a second once the table exists, and one statement per table
rather than a `CREATE` plus a literal `INSERT`.

Two Dremio spellings the literals need:

* **`_UTF8` in front of a string literal with an astral-plane character.** Dremio's
  parser encodes an unprefixed literal in ISO-8859-1, so `SELECT '🚀'` fails with
  `Error during planning the query` before the query runs at all, while
  `SELECT _UTF8'héllo 🚀'` returns the emoji intact. (`é` alone is inside ISO-8859-1 and
  needs no prefix — only the surrogate-pair characters do.)
* **`BINARY_STRING('\x01\x02')`** for a `VARBINARY` literal. `X'0102'` is rejected
  outright ("Unable to convert the value of `X'0102':BINARY(2)` ... to a Dremio constant
  expression"), and `CAST(... AS VARBINARY)` only reinterprets a string's own bytes —
  `BINARY_STRING` is the one that reads `\xNN` escapes.

`adbc_big` needs 100,000 rows and Dremio has no `range()` or `generate_series()`, so it
comes from five ten-row `VALUES` lists cross joined, with the row number computed from
their digits. Dremio writes the Parquet file in about a third of a second.

### What works

The whole read side of the workload: `int32`, `double`, `string` (including
`"héllo 🚀"` — the driver describes Dremio's `VARCHAR` as `SQL_VARCHAR`, so adbcbridge's
reader is on its correct narrow UTF-8 path and the emoji survives), `VARBINARY` as bytes,
`DATE`, `TIMESTAMP`, `BOOLEAN`, the all-NULL row, the 100,000-row batched read,
`GetObjects`, `GetTableSchema` and the error text (`Object 'adbc_no_such_table' not
found. Please check that it exists in the selected context.`). `GetObjects` works because
the existing `no_sql_columns` quirk already covers this driver. **No driver change was
needed for Dremio** — the third server behind this driver, and the second to need nothing
new.

Fetch: **1.1M rows/s** over the 100,000-row `adbc_big` (`bench/matrix_bench.py --rows
10000 --fetch-rows 100000`; three runs on a loaded box gave 1.03M, 1.13M and 1.39M, so
treat it as the same order as the other two Flight SQL entries rather than a ranking).
There is no ingest number — nothing can be written through this driver.

The entry's `extra` steps run Dremio's own catalog (`INFORMATION_SCHEMA."COLUMNS"` over a
CTAS table, which is a real dataset in the source) and a filtered columnar scan of the
100,000-row Parquet table.

### The entry's tolerances

| flag | why |
|---|---|
| `read_only=True` | the driver has no `SQLBindParameter`, so nothing that binds a value can run — not the parameterised `INSERT`, not `adbc_ingest`. Dremio itself writes fine; see above. `setup` builds `adbc_t` and `adbc_big` with literal SQL instead. |
| `params=False` | the driver answers `SQLBindParameter` with `HYC00 "Unsupported function"` on a virgin statement handle; the parameterised query runs with a literal. |
| `decimal_type="decimal128(19, 3)"` | the driver reports every `SQL_DECIMAL` as precision 19 whatever was declared, so `DECIMAL(10,3)` is described `(19, 3)`. Unlike the sqlflite case in the `flightsql` entry the *scale* is right, so nothing is lost — `12.345` arrives exact, just in a wider `decimal128`, and no `decimal_as_string` is needed. |

One thing worth knowing that costs the entry nothing: Dremio's `TIMESTAMP` is
millisecond-precision, and the driver describes it as scale 3, so `13:45:10.123456`
would read back `.123000` — the workload's `ts_us` default already allows that, and the
entry stores `.123` to begin with.

### Clean up

```sh
docker compose -f tests/compat/docker-compose.yml --profile extra down dremio
# or, if started standalone:
docker rm -f adbcbridge-dremio
```

## TDengine 3.3.6

TDengine is a time-series database with its own ODBC driver, **taos-odbc**
(`taosdata/taos-connector-odbc`, MIT). There is no Linux binary release of it, so it is
built from source below — against the client libraries that ship inside the server
image, which is what makes the whole thing root-free.

The entry is `read_only`, and unusually the reason is the *server*, not the driver: see
[Why the entry is read-only](#why-the-entry-is-read-only).

### Get the driver without root

Copy the client libraries and headers out of the server image (they are the ones the
driver links against, so their version matches the server exactly), and give them the
`.so`/`.so.1` symlinks a linker needs:

```sh
mkdir -p /tmp/dbs/tdengine/lib /tmp/dbs/tdengine/include /tmp/dbs/tdengine/log
docker create --name taosc tdengine/tdengine:latest
docker cp taosc:/usr/local/taos/driver/. /tmp/dbs/tdengine/lib/
docker cp taosc:/usr/local/taos/include/. /tmp/dbs/tdengine/include/
docker rm taosc
for f in libtaos libtaosnative libtaosws; do
  ln -sf $f.so.3.3.6.13 /tmp/dbs/tdengine/lib/$f.so
  ln -sf $f.so.3.3.6.13 /tmp/dbs/tdengine/lib/$f.so.1
done
```

Then build the connector. Take the **`3.3.6` branch**, not `main`: `main` calls
`taos_connect_with()`, which is not in the 3.3.6 client's `taos.h`, and the build stops
at `implicit declaration of function 'taos_connect_with'`. Requirements are cmake, flex,
bison, gcc and `unixodbc-dev` (all already needed to build adbcbridge itself, except flex
and bison); the build fetches cJSON itself:

```sh
git clone --depth 1 -b 3.3.6 https://github.com/taosdata/taos-connector-odbc.git
cd taos-connector-odbc
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_C_FLAGS=-I/tmp/dbs/tdengine/include \
  -DCMAKE_LIBRARY_PATH=/tmp/dbs/tdengine/lib \
  -DCMAKE_SHARED_LINKER_FLAGS="-L/tmp/dbs/tdengine/lib -Wl,-rpath,/tmp/dbs/tdengine/lib"
cmake --build build -j4 --target taos_odbc
cp -a build/src/libtaos_odbc.so* /tmp/dbs/tdengine/
```

Two environment variables are needed at run time, both root-free:

* `LD_LIBRARY_PATH=/tmp/dbs/tdengine/lib` — `libtaos.so` `dlopen()`s `libtaosnative.so`
  by name, and a `dlopen` does not go through the `RPATH` baked into the ODBC driver.
  Without it `SQLAllocHandle(SQL_HANDLE_ENV)` fails with
  `failed to load libtaosnative.so since No such file or directory`.
* `TAOS_LOG_DIR=/tmp/dbs/tdengine/log` — the TDengine *client* insists on a writable log
  directory and defaults to `/var/log/taos`, which needs root; failing to create it fails
  the environment handle too. (TDengine reads its client configuration from environment
  variables named `TAOS_<setting>`, so this needs no `taos.cfg`.)

### Start the server

```sh
docker run -d --name adbcbridge-tdengine --memory=2g \
  --hostname localhost -e TAOS_FQDN=localhost -e TAOS_SERVER_PORT=16030 \
  -p 127.0.0.1:16030:16030 tdengine/tdengine:latest
```

or `docker compose -f tests/compat/docker-compose.yml --profile extra up -d tdengine`.

The port arithmetic is the fiddly part. A native TDengine client does not keep talking to
the address it was given: it asks the cluster for its dnode list and connects to the
endpoint advertised there, which is `TAOS_FQDN:TAOS_SERVER_PORT` (`SHOW DNODES` prints
it). The stock `-p 127.0.0.1:16030:6030` therefore fails *after* connecting, on the first
query, with `rpc network error` — the client is dialling `localhost:6030`, which is
inside the container. Moving the server itself to 16030 and publishing the same number
makes the advertised `localhost:16030` correct on both sides.

### Run the entry

```sh
export TDENGINE_ODBC_DRIVER=/tmp/dbs/tdengine/libtaos_odbc.so
LD_LIBRARY_PATH=/tmp/dbs/tdengine/lib:$LD_LIBRARY_PATH \
TAOS_LOG_DIR=/tmp/dbs/tdengine/log \
ADBC_ODBC_DRIVER=$PWD/build/libadbc_driver_odbc.so \
python tests/compat/test_matrix.py tdengine
# tdengine  PASS  (tdengine (via ODBC) 03.03.0613 ...)
```

The entry's `setup` creates the database (`PRECISION 'us'`, so the workload's
microseconds survive) and both tables, so a fresh container needs no preparation. Every
statement in it is idempotent: the tables are `CREATE TABLE IF NOT EXISTS` and each row
carries a fixed primary-key timestamp, which TDengine overwrites in place rather than
appending a duplicate — replaying the whole thing on every connection (which
`bench/matrix_bench.py` does) leaves exactly the same rows.

### Why the entry is read-only

Nothing about the driver stops a write — the `extra` steps below bulk ingest through it.
The standard *write* side of the matrix workload simply cannot be expressed against a
TDengine table:

* **Every table starts with a TIMESTAMP primary key.** `CREATE TABLE t (i INT, ...)` is
  refused with `First column must be timestamp` (0x80002641). The workload's `INSERT`
  fills columns positionally from `i`, so the first bound value would be `1`, and
  `adbc_ingest`'s generated DDL declares no timestamp column at all.
* **That key is range-checked.** It has to be non-NULL, distinct per row and inside the
  database's retention window, so even feeding it the workload's integers fails:
  `INSERT INTO t VALUES (1, ...)` is `Timestamp data out of range` (0x8000060B) — 1 µs
  after the epoch is far outside the default `KEEP` of 3650 days. The same rules out
  `ingest_types` casting the ingest payload's `a` column to a timestamp: its values are
  `0..n-1` and one of them is NULL.

So `setup` builds `adbc_t` and `adbc_big` with literal SQL, exactly as the `flightsql`
entry does, and the whole read side of the workload — types, NULLs, the emoji,
parameters, batched reads across the 1024-row boundary, `GetObjects`, error mapping —
runs unchanged. The `extra` steps then cover the write path where TDengine's shape allows
one: they create a table whose first column *is* a timestamp and `adbc_ingest(...,
mode="append")` into it, through the driver's real parameter binding.

### Connection string

```
Driver={drv};SERVER=127.0.0.1:16030;UID=root;PWD=taosdata;TIMESTAMP_AS_IS=1;
```

`TIMESTAMP_AS_IS=1` is taos-odbc's own setting and the entry does not work without it:
by default the driver describes every TIMESTAMP column as an `SQL_WVARCHAR` holding the
formatted text, so `d` and `ts` would read back as strings rather than as Arrow
timestamps. There is no `DB=` — `setup` issues `CREATE DATABASE IF NOT EXISTS adbc` and
`USE adbc`, which is what lets a fresh container work.

### Driver quirks

Both are keyed on `SQL_DRIVER_NAME` (`libtaos_odbc.so`) in `OdbcDetectQuirks`. They have
the same root: taos-odbc looks a conversion up by the exact `(C type, SQL type, TDengine
type)` triple and its table is sparse.

* **No `SQL_C_TYPE_TIMESTAMP`, in either direction** (`reader_opts.timestamp_as_text`).
  `SQLBindCol` on a timestamp column fails the whole result set with
  `#1 Column converstion to 'SQL_C_TYPE_TIMESTAMP[0x5d/93]' not implemented yet`, and
  `SQLBindParameter` fails the same way for a timestamp parameter. Its bound conversions
  are the numeric C types, `SQL_C_CHAR`, `SQL_C_WCHAR` and `SQL_C_BINARY` — so the
  reader takes the column as text and parses it with the ISO-8601 parser the
  timestamp-with-timezone columns already use (the Arrow type stays a naive
  `timestamp[us]`, and the column keeps its place in the block cursor instead of dropping
  the result set to one-row `SQLGetData`), and a timestamp parameter goes across as
  `"YYYY-MM-DD HH:MM:SS.ffffff"` `SQL_VARCHAR` text, which is what the sub-second `TIME`
  path already does for every driver.
* **A boolean parameter only as `SQL_TINYINT`** (`reader_opts.bool_param_as_tinyint`).
  `SQL_C_BIT` → `SQL_BIT` is "not implemented yet"; the one route into a TDengine `BOOL`
  column is `SQL_C_SBIGINT` described as `SQL_TINYINT`. The existing
  `bool_param_as_int` sends the same integer as `SQL_INTEGER`, which is not in the
  driver's table, and `bool_param_as_varchar`'s `"true"`/`"false"` is parsed there with
  `strtoll` and refused, so this is its own flag.

Ingest needs no `ansi_ddl_type_names`-style help: adbcbridge quotes generated identifiers
with whatever `SQL_IDENTIFIER_QUOTE_CHAR` reports, and taos-odbc answers with a backtick,
which is the only identifier quote TDengine's parser has.

### Entry notes

* **`quote`**, which the entry sets to a backtick — the same point on the *test
  harness* side. The workload's own generated
  SQL (`SELECT "a", "b" FROM "adbc_big"`) is ANSI-quoted, and TDengine reads `"..."` as a
  string literal: `FROM "adbc_big"` is `syntax error near '"adbc_big"'`. The entry's
  `quote` key is what the harness spells its identifiers with.
* `n` is a `VARCHAR(20)` holding `12.345`, not a `DECIMAL(10,3)`. TDengine 3.3.6 does
  have `DECIMAL`, but taos-odbc does not implement it: a single such column fails the
  whole `SELECT` with `'UNKNOWN'[21] not supported yet` (21 is `TSDB_DATA_TYPE_DECIMAL64`,
  and the 128-bit `TSDB_DATA_TYPE_DECIMAL` is missing from its type table too). The
  column reads back as its exact text, which is what the `databend` and `flightsql`
  entries do for decimals their drivers misdescribe.
* `not_null=("ts",)` — row 2 of the workload is NULL in every column but `i`; the
  primary-key timestamp cannot be one.
* `column_order=False` — that primary key has to be declared first, so the columns are
  not in the order the workload lists them; `GetObjects` compares them as a set.
* `d` is a second `TIMESTAMP`: TDengine has no `DATE` type. `s` is `NCHAR` (the wide
  string type — the emoji round-trips), `b` is `VARBINARY`, `bo` is `BOOL`.
* The `extra` ingest table declares `a` as `INT` rather than `BIGINT`, and that is the
  driver again: an int64 payload whose values all fit in 32 bits is bound — here as
  everywhere else — as `SQL_C_SLONG`/`SQL_INTEGER`, and the driver's table maps that only
  to a TDengine `INT`. Values past 2³¹ go as `SQL_C_SBIGINT`/`SQL_BIGINT`, which does
  reach a `BIGINT` column.
* pyodbc drives this driver only on its narrow path: pyodbc sends statement text as
  UTF-16 by default, and taos-odbc fails any statement carrying a non-ASCII character
  with `conversion for 'UTF-8' to 'UTF-8' not found` (`SQLExecDirectW`). Add
  `conn.setencoding(encoding="utf-8", ctype=pyodbc.SQL_CHAR)` when probing with pyodbc.
  adbcbridge is unaffected: it calls the narrow `SQLExecDirect`/`SQLPrepare` throughout.

### Benchmark

`read_only`, so `bench/matrix_bench.py` times only the read: **403k rows/s** for the
20,000-row `adbc_big` (three columns, one of them a text-parsed timestamp).

### Clean up

```sh
docker rm -f adbcbridge-tdengine
```

## Google Cloud Spanner (emulator + PGAdapter 0.55.2)

Cloud Spanner has no ODBC driver and no PostgreSQL wire protocol of its own. What it has
is **PGAdapter**, the proxy Google ships for it: a PostgreSQL-wire front end that
translates to Spanner's gRPC API, which is how `psql`, JDBC and — here — `psqlodbc` reach
a PostgreSQL-dialect Spanner database. So the `spanner` entry uses the same `psqlodbc`
build as the `postgres` entry, with two containers behind it: the **Cloud Spanner
emulator** (Spanner itself, in memory, no GCP project and no credentials) and PGAdapter
in front of it.

Only the wire protocol is PostgreSQL's. Behind it is Spanner's schema and type system: no
32-bit integer, no `TIMESTAMP WITHOUT TIME ZONE`, no type modifier on `NUMERIC`, and a
mandatory primary key on every table.

### Get the ODBC driver without root

The same `psqlodbc` as the `postgres` entry; if you already exported
`POSTGRES_ODBC_DRIVER`, just point the Spanner variable at it:

```sh
export SPANNER_ODBC_DRIVER=$POSTGRES_ODBC_DRIVER
```

Otherwise unpack it first (no root needed):

```sh
mkdir -p /tmp/adbc-drivers && cd /tmp/adbc-drivers
apt-get download odbc-postgresql
dpkg-deb -x odbc-postgresql_*.deb pgodbc
export SPANNER_ODBC_DRIVER=$PWD/pgodbc/usr/lib/x86_64-linux-gnu/odbc/psqlodbcw.so
export LD_LIBRARY_PATH=$PWD/pgodbc/usr/lib/x86_64-linux-gnu:$LD_LIBRARY_PATH
```

### Start the servers

```sh
docker compose -f tests/compat/docker-compose.yml --profile extra up -d spanner-pg
```

or standalone — two containers on one user-defined network, so PGAdapter can resolve the
emulator by name:

```sh
docker network create adbcbridge-spanner-net
docker run -d --name adbcbridge-spanner --network adbcbridge-spanner-net --memory=2g \
  gcr.io/cloud-spanner-emulator/emulator
docker run -d --name adbcbridge-spanner-pg --network adbcbridge-spanner-net --memory=2g \
  -p 127.0.0.1:15442:5432 -e SPANNER_EMULATOR_HOST=adbcbridge-spanner:9010 \
  gcr.io/cloud-spanner-pg-adapter/pgadapter \
  -p test-project -i test-instance -d test-database -x -r autoConfigEmulator=true
```

Both images are public and free. The emulator serves Spanner's gRPC API on 9010 and a
REST gateway on 9020; neither needs publishing, since only PGAdapter talks to it.
PGAdapter's flags are what make this a two-command server:

* `-p/-i/-d` name the project, instance and database. They do not have to exist:
  `-r autoConfigEmulator=true` (a JDBC connection property, passed through with `-r`)
  makes PGAdapter create the instance and the database on the emulator at start-up, and
  puts the connection in the "emulator" mode that skips credentials entirely. Without it
  you would have to create both first, which the emulator's REST gateway can do with no
  auth and no `gcloud` (publish it with `-p 127.0.0.1:19020:9020` first):

  ```sh
  curl -s -X POST http://127.0.0.1:19020/v1/projects/test-project/instances \
    -d '{"instanceId":"test-instance","instance":{"config":"projects/test-project/instanceConfigs/emulator-config","displayName":"test","nodeCount":1}}'
  curl -s -X POST http://127.0.0.1:19020/v1/projects/test-project/instances/test-instance/databases \
    -d '{"createStatement":"CREATE DATABASE \"test-database\"","databaseDialect":"POSTGRESQL"}'
  ```
* `-x` lets PGAdapter accept connections that do not appear to come from localhost —
  inside a container the Docker host does not. Publishing on `127.0.0.1:15442` only is
  what keeps the server private.

They are ready when PGAdapter has logged its port (about ten seconds, most of it JVM
start-up):

```sh
until docker logs adbcbridge-spanner-pg 2>&1 | grep -q 'Server started on port'; do sleep 1; done
```

Together they settle at about 800 MB resident (the emulator ~340 MB, the JVM ~460 MB).

### Run the entry

```sh
SPANNER_ODBC_DRIVER=$POSTGRES_ODBC_DRIVER \
ADBC_ODBC_DRIVER=$PWD/build/libadbc_driver_odbc.so \
  .venv/bin/python tests/compat/test_matrix.py spanner
# spanner   PASS  (PostgreSQL (via ODBC) 14.0.1)
```

`GetInfo` reports `PostgreSQL 14.0.1` and `SELECT version()` answers `PostgreSQL 14.1`,
because that is the wire version PGAdapter claims (and `-v` can make it claim anything
else) — nothing in either says "Spanner". That matters for the driver quirk below.

### The entry's DDL is Spanner's, not PostgreSQL's

```sql
CREATE TABLE adbc_t (i bigint PRIMARY KEY, f double precision, s varchar(50),
                     b bytea, d date, ts timestamptz, n numeric, bo bool)
```

* **`PRIMARY KEY` is mandatory.** Any table without one is refused outright: `Primary key
  must be defined for table "adbc_t"`.
* **`timestamptz`, never `timestamp`.** Spanner has one timestamp type, UTC-based;
  `ts timestamp` fails with `Type <timestamp> is not supported`.
* **`numeric` takes no modifier.** `NUMERIC(10,3)` fails with `Type modifier is not
  supported for type <numeric>`; Spanner's `numeric` is a single fixed type.
* **no 32-bit integer.** `int4` fails with `Type <int4> is not supported; use bigint or
  int8 instead`. (`integer` is accepted as a spelling and silently widened to `bigint`.)

`bytea`, `date`, `varchar(n)`, `bool` and `double precision` are all Spanner types and
behave as on PostgreSQL, astral-plane Unicode included.

### Driver quirks

Both are keyed on the server, not on `psqlodbc` — the same driver library drives real
PostgreSQL. Since `version()` is PGAdapter's own claim, the driver asks for a setting
only PGAdapter has:

```sql
SELECT current_setting('spanner.ddl_transaction_mode', true)
```

On a real PostgreSQL the second argument (`missing_ok`) makes that answer `NULL` instead
of raising, so the probe costs one scalar query and never an error.

**1. A parameter array carrying a timestamp fails the whole batch
(`reader_opts.no_timestamp_param_arrays`).** psqlodbc executes a parameter array by
inlining the values into one statement, and it writes a bound `SQL_TYPE_TIMESTAMP` as a
*cast* literal:

```sql
INSERT INTO t VALUES (2, '2024-02-29 13:45:10.123456'::timestamp, '2024-02-29'::date, '1.5')
```

(that is one parameter set of a two-row array, as PGAdapter logged it with
`-log_grpc_messages`; `::date` is fine, Spanner has that type)

`::timestamp` is the type Spanner does not have, so the batch fails with `The Postgres
Type is not supported: timestamp without time zone`. Sent one row at a time the same
value goes as a typed parameter (`TIMESTAMP` over gRPC, which is what Spanner stores), so
the quirk turns off parameter arrays *only for a batch that binds a timestamp* — every
other batch, a bulk ingest of ordinary columns included, keeps its array. The narrower
flag is worth having: bulk ingest through the array path runs about 20% faster here than
row-at-a-time, and the workload's timestamps are two rows.

**2. Generated ingest DDL has to declare a primary key
(`reader_opts.ingest_key_column`).** `adbc_ingest(mode="create")` builds its own
`CREATE TABLE` from the Arrow schema, and on Spanner a table without a key is refused. No
ingested column can be the key — an ingest payload may repeat a value or hold NULL in any
column, and the matrix's own payload does both — so the flag appends a surrogate key
Spanner fills in itself:

```sql
CREATE TABLE "adbc_ing" ("a" int8, "b" text, "c" float8, "d" date, "e" bool,
                         "adbc_ingest_key" bigint GENERATED BY DEFAULT AS IDENTITY PRIMARY KEY)
```

The generated `INSERT` names the ingested columns explicitly, so the extra column is never
written to and every ingest mode (`create`, `append`, `replace`) works unchanged. It is
spelled without a leading underscore on purpose: Spanner rejects `_adbc_key` with `Column
name not valid`.

Nothing else needed a quirk. `bytea`, `date`, `numeric`, `timestamptz`, `float8` and
`text` — psqlodbc's own `SQLGetTypeInfo` names — are all names Spanner accepts, so
`ansi_ddl_type_names` (which QuestDB needs) would make ingest DDL *worse* here: its
portable fallbacks are `BLOB`, `TIMESTAMP` and `DECIMAL(p,s)`, none of which Spanner has.

### Tolerances in the entry (the server, not the driver)

* `ingest_types={pa.int32(): pa.int64()}` — Spanner has no 32-bit integer, so an int32
  column is ingested as int64, which is what Spanner would store anyway.
* `decimal_type="decimal128(28, 3)"` — Spanner does not report a `numeric` column's
  precision over the wire, so psqlodbc falls back to its own maximum (28) with the scale
  of the values in the result set.
* The `ts` column reads back as a *time-zone-aware* `timestamp[us, tz=UTC]`, because
  Spanner's only timestamp type carries a zone. psqlodbc renders it in the session's zone
  — the wall clock that was stored — so `test_matrix.py` compares the naive value.

### What the entry checks beyond the standard workload

The plain workload would look much like `postgres`, so the `extra` steps exercise the
reason to run Spanner: an **interleaved** table, where child rows are stored physically
inside the parent's key range instead of in a table of their own.

```sql
CREATE TABLE "adbc_child" (..., PRIMARY KEY ("a", "b"))
  INTERLEAVE IN PARENT "adbc_parent" ON DELETE CASCADE
```

The steps bulk-ingest through ADBC into the parent, then into the child, and read back
across a join. The child ingest passes only if the parent ingest really landed: Spanner
refuses an interleaved row whose parent row does not exist.

### Benchmark

Spanner writes are the slow part, so this entry is benchmarked at the small size:

```sh
ADBC_MATRIX_SUFFIX=_spanner SPANNER_ODBC_DRIVER=$POSTGRES_ODBC_DRIVER \
ADBC_ODBC_DRIVER=$PWD/build/libadbc_driver_odbc.so \
  .venv/bin/python bench/matrix_bench.py --rows 300 --fetch-rows 2000 spanner
# ingest 233 rows/s, 287 rows/s with array binding (pyodbc 222 rows/s)
# fetch  36.0k rows/s (pyodbc 35.9k rows/s)
```

Ingest gets *slower* the bigger the batch — 190 rows/s for 1000 rows, 168 for 2000, 128
for 5000 — because every row is its own DML statement inside one Spanner read-write
transaction, and the transaction's buffered mutations grow with it. `--rows 10000
--fetch-rows 100000`, the size the other entries are benchmarked at, does not finish in
any reasonable time against the emulator: the 100,000-row load was still running after
half an hour. This is the emulator's single-process implementation as much as Spanner's
design, so treat these numbers as a floor rather than a measure of Cloud Spanner itself.

Reads are ordinary: the ODBC path is as fast as pyodbc against the same server, and about
3x faster than delegating to the native `adbc_driver_postgresql` (12.2k rows/s), which
also connects to PGAdapter happily.

### DDL cannot run inside a transaction

Spanner has no transactional DDL, and PGAdapter's default `spanner.ddl_transaction_mode`
is `Batch`, which refuses DDL in an explicit transaction:

```
25000 ERROR: DDL statements are not allowed in mixed batches or transactions.
```

Nothing in the matrix or the benchmark trips over that — both ingest on an autocommit
connection, where adbcbridge opens its own transaction *after* the `CREATE TABLE` — but
`adbc_ingest(mode="create")` on a connection with `autocommit=False` does. One session
setting lifts it, and is worth knowing about rather than working around in the driver:

```sql
SET spanner.ddl_transaction_mode = 'AutocommitExplicitTransaction'
```

(PGAdapter then commits the open transaction when it meets a DDL statement.)

### One schema change at a time

Spanner serialises DDL: a second `CREATE`/`DROP` issued while another is still running is
rejected outright with `Schema change operation rejected because a concurrent schema
change operation or read-write transaction is already in progress`. That is worth knowing
if you run the entry and `bench/matrix_bench.py` against the same database at once (or two
matrix runs with different `ADBC_MATRIX_SUFFIX` values) — the isolated table names are not
enough here, the DDL itself has to be serialised.

### Clean up

```sh
docker compose -f tests/compat/docker-compose.yml --profile extra down spanner-pg spanner
# or, if started standalone:
docker rm -f adbcbridge-spanner-pg adbcbridge-spanner
docker network rm adbcbridge-spanner-net
```

## MongoDB 7 + BI Connector 2.14 (mongosqld)

MongoDB has no SQL wire protocol of its own. The [MongoDB BI
Connector](https://www.mongodb.com/docs/bi-connector/current/) is MongoDB's own answer to
that: `mongosqld` sits in front of a MongoDB instance, presents each collection as a
relational table and speaks the **MySQL wire protocol** to clients, announcing itself as
`5.7.12 mongosqld v2.14.22`. So the ODBC route is MySQL Connector/ODBC — the same
`libmyodbc9w.so` the [`mysql`](#mysql-8) entry uses (read that section for the download
and for the `LD_PRELOAD` pyarrow needs), pointed at a second variable:

```sh
export MONGODBBI_ODBC_DRIVER=$MYSQL_ODBC_DRIVER
```

### The BI Connector tarball (free download, no account, no root)

`mongosqld` is not in any image; it ships as a tarball MongoDB serves without a login:

```sh
mkdir -p /tmp/dbs/mongodbbi && cd /tmp/dbs/mongodbbi
curl -sSLO https://info-mongodb-com.s3.amazonaws.com/mongodb-bi/v2/mongodb-bi-linux-x86_64-ubuntu2004-v2.14.22.tgz
tar xzf mongodb-bi-linux-x86_64-ubuntu2004-v2.14.22.tgz
# -> mongodb-bi-linux-x86_64-ubuntu2004-v2.14.22/bin/{mongosqld,mongodrdl,mongotranslate}
```

The binaries are dynamically linked against **OpenSSL 1.1**, which `mongo:7` (Ubuntu
22.04, OpenSSL 3) does not have — `mongosqld` refuses to start with `error while loading
shared libraries: libssl.so.1.1`. Unpack the two libraries from the Ubuntu 20.04 package
(no root: `dpkg-deb -x` writes wherever it is told) and put them next to the binary, which
is the directory the container mounts:

```sh
mkdir -p bi && cp mongodb-bi-linux-x86_64-ubuntu2004-v2.14.22/bin/mongosqld bi/
curl -sSLO http://archive.ubuntu.com/ubuntu/pool/main/o/openssl/libssl1.1_1.1.1f-1ubuntu2.24_amd64.deb
dpkg-deb -x libssl1.1_1.1.1f-1ubuntu2.24_amd64.deb ssl
cp ssl/usr/lib/x86_64-linux-gnu/lib{ssl,crypto}.so.1.1 bi/
export MONGODB_BI_DIR=/tmp/dbs/mongodbbi/bi
```

### Server

One container runs both halves: `mongod` (the image's own entrypoint) and, on top of it,
`mongosqld` reading it over localhost and serving the MySQL wire on 3307.

```sh
docker run -d --name adbcbridge-mongodbbi --memory=2g -p 127.0.0.1:13315:3307 \
  -e LD_LIBRARY_PATH=/opt/mongobi -v $MONGODB_BI_DIR:/opt/mongobi:ro \
  -v $PWD/tests/compat/fixtures/mongodbbi.drdl:/etc/mongodbbi.drdl:ro mongo:7
```

(or `MONGODB_BI_DIR=… docker compose -f tests/compat/docker-compose.yml --profile extra up
-d mongodbbi`; it is in the `extra` profile, so a plain `up -d` leaves it alone.) MongoDB
is ready in a few seconds — `docker exec adbcbridge-mongodbbi mongosh --quiet --eval
'db.runCommand({ping:1})'` answers `{ ok: 1 }`.

### Loading the data (the entry cannot)

`mongosqld` is a **query engine only**: it has no `CREATE TABLE` and no `INSERT`, and a
"table" there is a MongoDB collection plus a column mapping in its schema. So the entry is
`read_only=True` and its two collections are written into MongoDB directly, the way the
`influxdb3` entry's tables are written over the HTTP API:

```sh
docker exec -i adbcbridge-mongodbbi mongosh --quiet < tests/compat/fixtures/load_mongodbbi.js
# adbc_t=2 adbc_big=100000
```

Then start `mongosqld` on top of the loaded collections, with the schema that maps them:

```sh
docker exec -d adbcbridge-mongodbbi /opt/mongobi/mongosqld --addr 0.0.0.0:3307 \
  --mongo-uri mongodb://127.0.0.1:27017 --schema /etc/mongodbbi.drdl \
  --logPath /tmp/mongosqld.log --logAppend
docker exec adbcbridge-mongodbbi tail -2 /tmp/mongosqld.log
# ... [initandlisten] waiting for connections at [::]:3307
```

`fixtures/mongodbbi.drdl` is a DRDL schema — the BI Connector's own mapping format, of
which `mongodrdl` (in the same tarball) generates a first draft by sampling. The matrix
uses a written one because sampling types a field from the values it happens to see:
`adbc_big.c` comes out `int` because its first values are whole, and a `binData` field
comes out `varchar` and then reads back NULL. Anything the schema does not list is not a
table, which is also why `mongosqld` needs restarting (or `--schemaRefreshIntervalSecs`)
after the collections change.

### Run the entry

```sh
export MONGODBBI_ODBC_DRIVER=$MYSQL_ODBC_DRIVER
LD_PRELOAD=/lib/x86_64-linux-gnu/libstdc++.so.6 \
ADBC_ODBC_DRIVER=$PWD/build/libadbc_driver_odbc.so \
  python tests/compat/test_matrix.py mongodbbi
# mongodbbi PASS  (MySQL (via ODBC) 5.7.12 mongosqld v2.14.22)
```

### What works

The whole read side of the workload: `int64`, `double`, `string` (including `"héllo 🚀"`
— the emoji survives the round trip), `bool` as `int8`, timestamps, the all-NULL row, the
parameterised `SELECT`, the 100,000-row batched read, `GetObjects`, `GetTableSchema` and
the error text. On the SQL side `mongosqld` translates into MongoDB aggregation pipelines,
so the entry's `extra` steps run a filtered count, a `GROUP BY` on a boolean field and a
`COUNT(DISTINCT _id)` over the `ObjectId` every document carries.

Fetch: **128k rows/s** over the 100,000-row `adbc_big` (`bench/matrix_bench.py`). There is
no ingest number — nothing can be written through `mongosqld`.

### Two Connector/ODBC crashes, and the one that needed a driver quirk

Both were found with a standalone C program against `libmyodbc9w.so` (no adbcbridge in the
picture), and both are segfaults inside the connector, not error returns.

**1. The handshake, without `PLUGIN_DIR`.** `mongosqld` offers only
`mysql_native_password`, whose client-side plugin Connector/ODBC 9 no longer links in — it
ships as a loadable `.so` beside the driver, and the compiled-in search path of the generic
tarball is `/usr/local/mysql/lib/plugin`. Where [Dolt](#dolt) reports that as a clean
`Authentication plugin ... cannot be loaded`, here `SQLDriverConnect` **segfaults**.
Pointing `PLUGIN_DIR` at the tarball's own plugin directory — which `conn_uri()`'s
`{plugin_dir}` already does for TiDB, Dolt, Databend and MatrixOne — avoids it entirely, so
this one costs the entry nothing but a connection property.

**2. `SQLColumns` on any table with a `DECIMAL` column.** This is the one the driver had to
work around:

```
#0  __GI_____strtol_l_internal (nptr=0x0, ...)
#1  get_buffer_length (..., sqltype=3 /* SQL_DECIMAL */, col_size=0) at driver/catalog.cc:626
#2  columns_i_s (..., table="adbc_t", ...) at driver/catalog.cc:962
#3  MySQLColumns (...) / SQLColumnsW (...)
```

`mongosqld`'s `information_schema.columns` reports `NULL` for `NUMERIC_PRECISION`,
`NUMERIC_SCALE` and `CHARACTER_OCTET_LENGTH` on every column, and Connector/ODBC 9 builds
`SQLColumns` entirely from `information_schema` (the older `SHOW`-based path is gone; there
is no `NO_I_S` option left to switch to). For a `DECIMAL` column it runs `strtol()` on that
NULL pointer and the process dies. `adbc_big`, which has no decimal column, comes back
fine — so nothing in the return code separates the two cases:

```sh
./probe_c "Driver=…libmyodbc9w.so;…;PLUGIN_DIR=…;" columns adbc_big   # SQLColumns rc=0, six columns
./probe_c "Driver=…libmyodbc9w.so;…;PLUGIN_DIR=…;" columns adbc_t     # Segmentation fault
```

The fix is one keyed entry in `OdbcDetectQuirks` setting the **existing**
`no_sql_columns` flag — the same one the Arrow Flight SQL driver and psqlodbc-against-
ArcadeDB use — after which `GetObjects` describes `SELECT * FROM <table> WHERE 1=0` instead,
which is where `GetTableSchema` already gets a table's columns from. It is keyed on
`SQL_DBMS_VER` (`"5.7.12 mongosqld v2.14.22"`, straight out of the handshake) rather than on
`SELECT version()`, which answers a bare `5.7.12` here and names nothing.

### The entry's tolerances are the BI Connector's type system

| flag | why |
|---|---|
| `read_only=True` | `mongosqld` has no DDL and no DML at all, so neither the entry's `ddl` nor `adbc_ingest` has anywhere to go. The two collections come from `fixtures/load_mongodbbi.js`. |
| `NO_SSPS=1` (in `conn`) | `COM_STMT_PREPARE` comes back as error 1295, `This command is not supported in the prepared statement protocol yet` — the same answer Databend's MySQL handler gives. With `NO_SSPS` the connector substitutes bound parameters into the SQL text, and the parameterised `SELECT` works. |
| `{plugin_dir}` (in `conn`) | `mysql_native_password`, as above — and here its absence is a segfault, not a diagnostic. |
| `pseudo_columns=("_id",)` | every MongoDB document carries an `_id` and the BI Connector maps it as an ordinary column, so `SELECT *` returns a ninth, always-populated column — the same shape as ArcadeDB's `@rid`. |
| `catalog_cols=(...)` | `mongosqld` reports a table's columns in its own alphabetical order, in the catalog and in `SELECT *` alike. |
| `quote` set to a backtick | a MySQL dialect with no `sql_mode` to set (there is no `SET SESSION` at all), so `"…"` is a string literal and identifiers are backtick-quoted, as for GreptimeDB. |
| `bool_type="int8"` | MongoDB's boolean goes over the MySQL wire as `TINYINT(1)`, which Connector/ODBC reports as `SQL_TINYINT`, exactly as MySQL's own `BOOLEAN` does. |
| `decimal_type="string"` | `mongosqld` describes every decimal as `DECIMAL(65,20)` whatever is in it; 65 digits is past what an Arrow `decimal128` holds, so the column arrives as its exact text (`"12.345"`), as for `databend` and `flightsql`. |
| `big_rows=100000` | `adbc_big` is what `check_big()` reads and what `bench/matrix_bench.py` times a fetch of on a read-only entry. |

Two more facts about the mapping, both of them the connector's and neither costing a flag:

* **No binary type exists.** `bson.Binary` is rejected as a DRDL Mongo type (`unsupported
  Mongo type: "bson.Binary"`) and `varbinary`/`binary`/`bindata` are all rejected as DRDL
  SQL types; a sampled `binData` field is mapped to `varchar` and then reads back NULL. So
  the workload's two bytes are stored as *text* and read back as text, which the assertion
  already allows — the same place CrateDB and InfluxDB 3 end up.
* **A BSON date is milliseconds.** `ts` keeps 123 of the workload's 123456 microseconds
  (within the default `ts_us` tolerance), and `d` — there being no DATE type — is a
  midnight timestamp, which the workload also already allows.

### Clean up

```sh
docker compose -f tests/compat/docker-compose.yml --profile extra down mongodbbi
# or, if started standalone:
docker rm -f adbcbridge-mongodbbi
```
