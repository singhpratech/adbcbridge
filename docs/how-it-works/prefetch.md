<!-- SPDX-License-Identifier: Apache-2.0 -->
# Prefetch

`SQLFetch` blocks on the socket while the CPU is idle, and the conversion into Arrow then
runs while the socket is idle. `adbc.odbc.prefetch=1` overlaps them: a background thread
fetches the next rowset into a second set of bound buffers while the calling thread
converts the current one. Higher values keep more rowsets in flight, up to 8.

```python
conn = dbapi.connect(driver="libadbc_driver_odbc.so", db_kwargs={
    "adbc.odbc.connection_string": "...",
    "adbc.odbc.prefetch": "1"})
```

It is **off by default**, because whether it is safe is a property of the ODBC driver
underneath and no driver can be asked. It is also absent from the Windows build: the
pipeline is pthreads and is compiled out on `_WIN32` until the Win32 thread shim lands
([roadmap](../ROADMAP.md)); `adbc.odbc.prefetch` is accepted there and reads run
unpipelined. Two things make it safe where it does engage:

* The statement handle is owned by exactly one thread at a time. The fetch thread owns it
  from `pthread_create` to `pthread_join` and the calling thread touches it only outside
  that window, so no two ODBC calls on one handle are ever concurrent.
* It engages only when every column is bound at a width that cannot truncate. Repairing a
  truncated value means `SQLGetData` or `SQLFetchScroll` on a row the fetch thread has
  already read past, so a column bound narrower than its declared width (a `TEXT` or
  `NVARCHAR(MAX)`; see `adbc.odbc.long_bind_bytes`) turns prefetch off for that query
  rather than racing for the cursor. So does a column the driver cannot bind at all, and
  a driver that fetches one row at a time.

Falling back is silent and changes nothing about the rows. Errors are unchanged too: a
failure part way through a result set arrives at the same row it would have without
prefetch, and releasing a stream early stops the fetch thread before the handle is freed.

**It is worth very little in practice**, and the reason is worth knowing: most ODBC
drivers have already buffered the whole result set client-side by the time `SQLFetch` is
called, so there is no socket wait left to hide. Measured on 1,000,000 rows (medians of
5, `bench/prefetch_bench.py`):

| driver | prefetch=1 | prefetch=2 |
|---|---:|---:|
| psqlodbc, default | 1.00× | 1.01× |
| psqlodbc, `UseDeclareFetch=1` | 1.06× | 1.10× |
| sqliteodbc | 1.01× | 1.01× |
| MariaDB Connector/ODBC | 1.04× | 0.93× |

Only psqlodbc's cursor mode leaves a real socket wait inside `SQLFetch`, and even there
it buys 6–10%. For a big PostgreSQL read, [partitioning](partitioned-reads.md)
is the mechanism that pays.
