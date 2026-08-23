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
