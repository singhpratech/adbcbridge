# Copyright 2026 the adbcbridge authors
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
#
# SPDX-License-Identifier: Apache-2.0

# The "Use from R" snippet from docs/languages/r.md, kept here so it cannot
# silently rot.
#
# Everything below the marker line is the snippet verbatim. smoke.R reads this
# file, swaps the two placeholder paths and the database file name for real
# ones, and runs it -- so the snippet is executed, not just eyeballed. Keep it
# and the docs block byte-identical.

# --- README SNIPPET BELOW THIS LINE ---
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
