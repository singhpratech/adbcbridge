<!-- SPDX-License-Identifier: Apache-2.0 -->
# R

adbcBridge from R through the `adbcdrivermanager` package. R has a smoke test in the repository ([`tests/r/`](../../tests/r/README.md)) and no benchmark row yet.

```r
install.packages(c("adbcdrivermanager", "nanoarrow"))
```

```r
library(adbcdrivermanager)

drv <- adbc_driver("/path/to/libadbc_driver_odbc.so", entrypoint = "AdbcDriverInit")
db <- adbc_database_init(
  drv,
  uri = "Driver=/usr/lib/x86_64-linux-gnu/odbc/libsqlite3odbc.so;Database=my.db;"
)
con <- adbc_connection_init(db)

# read_adbc() returns a nanoarrow array stream; as.data.frame() materialises it.
df <- as.data.frame(read_adbc(con, "SELECT * FROM my_table"))

# Parameters are bound as a data frame: a column per '?', a row per execution.
execute_adbc(con, "INSERT INTO my_table (id, name) VALUES (?, ?)",
             bind = data.frame(id = 1:2, name = c("ada", "grace")))

# Bulk ingest: create (or append to) a table from a data frame.
write_adbc(df, con, "my_copy")

adbc_connection_release(con)
adbc_database_release(db)
```

`read_adbc()` and `execute_adbc()` accept the database object directly if you
do not need an explicit connection. Results are nanoarrow array streams, so a
large one can be pulled a batch at a time — `s <- read_adbc(con, "SELECT ...")`
then `s$get_next()` until it returns `NULL` — instead of materialised with
`as.data.frame()`. See `tests/r/` for a runnable example and the docker command
that runs it.
