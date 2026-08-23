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
