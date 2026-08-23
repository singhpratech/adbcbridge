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

**MySQL type names in ingest DDL** (`ignore_driver_type_names`). `SQLGetTypeInfo` answers
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
