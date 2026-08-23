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

# R smoke test for libadbc_driver_odbc.so.
#
# Loads the driver through the adbcdrivermanager CRAN package, points it at the
# SQLite ODBC driver over a throwaway database file, and asserts on the data
# frames nanoarrow hands back. See README.md in this directory for the docker
# command that runs it.
#
#   ADBC_ODBC_DRIVER    the driver under test (default ../../build/...)
#   SQLITE_ODBC_DRIVER  the SQLite ODBC driver to bridge to (path or a name
#                       registered in odbcinst.ini; default "SQLite3")

library(adbcdrivermanager)
library(nanoarrow)

# ---------------------------------------------------------------- harness ---

checks <- 0L

check <- function(what, ok) {
  if (!isTRUE(ok)) stop("FAILED: ", what, call. = FALSE)
  checks <<- checks + 1L
  cat("  ok  ", what, "\n", sep = "")
}

check_equal <- function(what, actual, expected) {
  check(what, isTRUE(all.equal(actual, expected)))
}

# ------------------------------------------------------------------ setup ---

# Directory holding this script, so it can find its siblings however it is run.
this_dir <- {
  file_arg <- grep("^--file=", commandArgs(trailingOnly = FALSE), value = TRUE)
  if (length(file_arg) == 1L) dirname(sub("^--file=", "", file_arg)) else getwd()
}

driver_path <- Sys.getenv("ADBC_ODBC_DRIVER", unset = NA)
if (is.na(driver_path)) {
  driver_path <- normalizePath(
    file.path(this_dir, "..", "..", "build", "libadbc_driver_odbc.so"),
    mustWork = FALSE
  )
}
if (!file.exists(driver_path)) {
  stop(
    "driver not found at ", driver_path,
    " - build it first (cmake --build build) or set ADBC_ODBC_DRIVER",
    call. = FALSE
  )
}

sqlite_odbc <- Sys.getenv("SQLITE_ODBC_DRIVER", unset = "SQLite3")
db_file <- file.path(tempdir(), "smoke.db")
unlink(db_file)

cat("driver:      ", driver_path, "\n", sep = "")
cat("sqlite odbc: ", sqlite_odbc, "\n", sep = "")

# libadbc_driver_odbc.so also exports AdbcDriverOdbcInit, which is the name the
# driver manager derives from the file name; naming AdbcDriverInit is explicit.
drv <- adbc_driver(driver_path, entrypoint = "AdbcDriverInit")

# The ADBC "uri" option is passed through to SQLDriverConnect verbatim.
uri <- sprintf("Driver=%s;Database=%s;", sqlite_odbc, db_file)
db <- adbc_database_init(drv, uri = uri)
con <- adbc_connection_init(db)

# The UTF-8 payload is written with \u escapes so the test does not depend on
# the encoding R reads this source file in: R produces UTF-8 for these at parse
# time in any locale.
utf8_text <- "héllo \U0001f680"

# ---------------------------------------------------- 1. SELECT a literal ---

cat("\n[1] SELECT a literal\n")

one <- as.data.frame(read_adbc(con, "SELECT 1 AS one"))
check_equal("SELECT 1 AS one returns one column named 'one'", names(one), "one")
check_equal("... holding the value 1", one$one, 1L)

# ------------------------------------------------- 2. NULLs and UTF-8 text ---

cat("\n[2] NULLs and UTF-8 text\n")

execute_adbc(con, "CREATE TABLE t (i INTEGER, s TEXT)")
execute_adbc(con, sprintf("INSERT INTO t VALUES (1, '%s')", utf8_text))
execute_adbc(con, "INSERT INTO t VALUES (2, NULL)")
execute_adbc(con, "INSERT INTO t VALUES (NULL, '')")

t <- as.data.frame(read_adbc(con, "SELECT i, s FROM t ORDER BY rowid"))
check_equal("three rows back", nrow(t), 3L)
check_equal("SQL NULL in an INTEGER column becomes NA", t$i, c(1L, 2L, NA))
check_equal("SQL NULL in a TEXT column becomes NA, '' stays ''",
            t$s, c(utf8_text, NA, ""))
# An NA must not be confused with the empty string, in either direction.
check("NA and '' are distinct", is.na(t$s[2]) && !is.na(t$s[3]))
# Prove the text survived as real UTF-8 bytes rather than mojibake or a
# question-mark transliteration: e-acute is C3 A9 and U+1F680 is F0 9F 9A 80.
check_equal("... with the exact UTF-8 bytes preserved",
            as.integer(charToRaw(t$s[1])),
            c(0x68, 0xc3, 0xa9, 0x6c, 0x6c, 0x6f, 0x20, 0xf0, 0x9f, 0x9a, 0x80))
check_equal("... and is declared UTF-8 to R", Encoding(t$s[1]), "UTF-8")

# ------------------------------------------------ 3. parameterised queries ---

cat("\n[3] parameterised queries (adbc_statement_bind)\n")

execute_adbc(con, "CREATE TABLE people (id INTEGER, name TEXT)")

# One bound batch, three rows, one execution: a column per '?' and a row per
# parameter set. The NULL name goes in as NA.
params <- data.frame(
  id = c(1L, 2L, 3L),
  name = c("ada", "grace", NA_character_),
  stringsAsFactors = FALSE
)

stmt <- adbc_statement_init(con)
adbc_statement_set_sql_query(stmt, "INSERT INTO people (id, name) VALUES (?, ?)")
adbc_statement_prepare(stmt)
adbc_statement_bind(stmt, params)
inserted <- adbc_statement_execute_query(stmt)
adbc_statement_release(stmt)
check_equal("bound INSERT reports three rows affected", inserted, 3)

people <- as.data.frame(read_adbc(con, "SELECT id, name FROM people ORDER BY id"))
check_equal("all three rows landed, in order", people$id, c(1L, 2L, 3L))
check_equal("... with the bound NA preserved as NULL",
            people$name, c("ada", "grace", NA))

# A bound parameter in a SELECT: one row of arguments, filtered result.
stmt <- adbc_statement_init(con)
adbc_statement_set_sql_query(stmt, "SELECT name FROM people WHERE id = ?")
adbc_statement_prepare(stmt)
adbc_statement_bind(stmt, data.frame(id = 2L))
stream <- nanoarrow_allocate_array_stream()
# Returns rows-affected, which is -1 ("unknown") for a SELECT; the rows
# themselves arrive through `stream`.
invisible(adbc_statement_execute_query(stmt, stream))
picked <- as.data.frame(stream)
# The stream is a child of the statement: release it before its parent, or
# adbc_statement_release() refuses with "has 1 unreleased child object".
nanoarrow_pointer_release(stream)
adbc_statement_release(stmt)
check_equal("bound SELECT returns only the matching row", picked$name, "grace")

# read_adbc()'s own bind= argument is the same thing one layer up.
picked2 <- as.data.frame(
  read_adbc(con, "SELECT name FROM people WHERE id = ?", bind = data.frame(id = 3L))
)
check("read_adbc(bind=) filters too, NULL name and all", is.na(picked2$name))
check_equal("... and returns exactly one row", nrow(picked2), 1L)

# ----------------------------------------------- 4. write_adbc bulk ingest ---

cat("\n[4] write_adbc() bulk ingest round trip\n")

# More rows than one Arrow batch (the bridge's default batch_size is 1024) so
# the ingest path is exercised across batch boundaries.
n <- 2500L
out <- data.frame(
  i = seq_len(n),
  f = seq_len(n) + 0.5,
  s = c(utf8_text, NA_character_, sprintf("row%d", 3:n)),
  stringsAsFactors = FALSE
)

write_adbc(out, con, "ingested")

back <- as.data.frame(read_adbc(con, 'SELECT i, f, s FROM "ingested" ORDER BY i'))
check_equal("every ingested row comes back", nrow(back), n)
check_equal("integer column round trips", back$i, out$i)
check_equal("double column round trips", back$f, out$f)
check_equal("text column round trips, UTF-8 and NA included", back$s, out$s)

# Appending to the table it just created must add to it, not replace it.
write_adbc(data.frame(i = n + 1L, f = 0.25, s = "appended"), con, "ingested",
           mode = "append")
total <- as.data.frame(read_adbc(con, 'SELECT COUNT(*) AS n FROM "ingested"'))
check_equal("append mode adds a row", as.integer(total$n), n + 1L)

# --------------------------------------------------- 5. the README snippet ---

cat("\n[5] the \"Use from R\" README snippet\n")

# readme_snippet.R holds the top-level README's snippet verbatim. Run it for
# real -- with the placeholder paths swapped for this test's -- so the snippet
# in the docs cannot rot into something that no longer works.
execute_adbc(con, "CREATE TABLE my_table (id INTEGER, name TEXT)")

snippet_file <- file.path(this_dir, "readme_snippet.R")
snippet <- readLines(snippet_file, encoding = "UTF-8")
marker <- grep("^# --- README SNIPPET BELOW THIS LINE ---$", snippet)
check_equal("readme_snippet.R has exactly one snippet marker", length(marker), 1L)

snippet <- snippet[(marker + 1L):length(snippet)]
# Drop trailing blank lines so the comparison below is not thrown by them.
while (length(snippet) && !nzchar(trimws(tail(snippet, 1L)))) {
  snippet <- head(snippet, -1L)
}

# The top-level README must carry this snippet verbatim. Find the fenced r
# block that starts with library(adbcdrivermanager) and compare it line for
# line, so editing one of the two without the other fails here.
readme_file <- file.path(this_dir, "..", "..", "README.md")
if (file.exists(readme_file)) {
  readme <- readLines(readme_file, encoding = "UTF-8")
  fences <- grep("^```", readme)
  opens <- fences[readme[fences] == "```r" &
                  readme[pmin(fences + 1L, length(readme))] ==
                    "library(adbcdrivermanager)"]
  check_equal("README.md has exactly one 'Use from R' code block",
              length(opens), 1L)
  closing <- fences[fences > opens[1]][1]
  block <- readme[(opens[1] + 1L):(closing - 1L)]
  check_equal("... byte-identical to readme_snippet.R", block, snippet)
} else {
  cat("  skip  README.md not mounted\n")
}

snippet <- gsub("/path/to/libadbc_driver_odbc.so", driver_path, snippet, fixed = TRUE)
snippet <- gsub("/usr/lib/x86_64-linux-gnu/odbc/libsqlite3odbc.so", sqlite_odbc,
                snippet, fixed = TRUE)
snippet <- gsub("Database=my.db;", paste0("Database=", db_file, ";"), snippet,
                fixed = TRUE)

# A fresh environment, so the snippet cannot lean on anything defined above.
snippet_env <- new.env(parent = globalenv())
eval(parse(text = paste(snippet, collapse = "\n")), envir = snippet_env)
check("the README snippet runs end to end", TRUE)

# The snippet's own effects: two bound rows inserted, and my_copy created by
# write_adbc() from a data frame that was empty.
snippet_rows <- as.data.frame(
  read_adbc(con, "SELECT id, name FROM my_table ORDER BY id")
)
check_equal("... inserting its two bound rows", snippet_rows$name, c("ada", "grace"))
check_equal("... and creating my_copy via write_adbc()",
            nrow(as.data.frame(read_adbc(con, 'SELECT * FROM "my_copy"'))), 0L)

# ---------------------------------------------------------------- teardown ---

adbc_connection_release(con)
adbc_database_release(db)

cat(sprintf("\nR SMOKE OK (%d checks)\n", checks))
