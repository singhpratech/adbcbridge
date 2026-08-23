# Compatibility matrix

`test_matrix.py` runs one identical ADBC workload (all types, NULLs, Unicode incl. emoji,
parameters, bulk ingest, batched reads, GetObjects, error mapping) against every ODBC
driver it can reach. Each database is enabled by an environment variable holding the path
to its ODBC driver, so a driver you have not installed is reported as `SKIP`, never a
failure:

```sh
export ADBC_ODBC_DRIVER=$PWD/build/libadbc_driver_odbc.so
python tests/compat/test_matrix.py            # everything that is configured
python tests/compat/test_matrix.py sqlite     # one database
```

Servers come from `docker-compose.yml` in this directory:

```sh
docker compose -f tests/compat/docker-compose.yml up -d
```

The sections below record how to obtain each ODBC driver **without root**, and the exact
commands used to run that database's entry.

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
