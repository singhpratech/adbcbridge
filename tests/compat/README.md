# Compatibility matrix

`test_matrix.py` runs one identical ADBC workload — the 8-column typed table
(`i, f, s, b, d, ts, n, bo`), NULLs, Unicode including emoji, `?` parameters,
bulk ingest, a batch-crossing read, `GetObjects`/`GetTableSchema`, and the error
path — against every ODBC driver it can reach.

```sh
ADBC_ODBC_DRIVER=build/libadbc_driver_odbc.so \
  .venv/bin/python tests/compat/test_matrix.py [db ...]
```

Each database is enabled by an environment variable holding the path to its ODBC
driver; a database whose variable is unset prints `SKIP`. Servers come from
`docker-compose.yml` in this directory, and each connection string can be
overridden with `<NAME>_CONN`.

```sh
docker compose -f tests/compat/docker-compose.yml up -d
```

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
