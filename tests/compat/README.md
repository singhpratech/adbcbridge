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
