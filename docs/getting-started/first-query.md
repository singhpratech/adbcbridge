<!-- SPDX-License-Identifier: Apache-2.0 -->
# Your first query

This page runs a complete session against SQLite through adbcBridge — connect,
query, insert with parameters, bulk-ingest a table — in Python, and then shows
the same connect-and-query program in Rust, C#, Java, Go and C. SQLite is used
because its ODBC driver is tiny and needs no server; swap the connection string
for any other database once this works.

If you have not installed adbcBridge yet, start with
[Installing on Linux](install-linux.md),
[macOS](install-macos.md) or [Windows](install-windows.md). All examples assume
the driver is discoverable by name (`driver="odbc"`), which an install gives you.

## Python: the full session

adbcBridge is loaded through the ADBC driver manager. Install that client and
pyarrow:

```sh
pip install adbc-driver-manager pyarrow
```

Then run this program. Each numbered step builds on the last.

```python
import adbc_driver_manager.dbapi as dbapi
import pyarrow as pa

# 1. Connect through the bridge to a SQLite file. `driver="odbc"` resolves the
#    bridge by name from the manifest an install wrote; `uri` is an ODBC
#    connection string.
conn = dbapi.connect(
    driver="odbc",
    db_kwargs={"uri": "Driver=SQLite3;Database=/tmp/first.db;"},
)

# 2. Run a query and get the result as a pyarrow.Table.
with conn.cursor() as cur:
    cur.execute("SELECT 42 AS answer, 'hello' AS greeting")
    table = cur.fetch_arrow_table()      # pyarrow.Table
    print(table)

# 3. Create a table and insert rows with parameters. ODBC uses `?` placeholders;
#    executemany() binds one row per tuple.
with conn.cursor() as cur:
    cur.execute("CREATE TABLE people (id INTEGER, name TEXT)")
    cur.executemany(
        "INSERT INTO people (id, name) VALUES (?, ?)",
        [(1, "Ada"), (2, "Grace")],
    )
conn.commit()                            # connections are transactional by default

# 4. Bulk-ingest a whole Arrow table in one call. mode="create" makes a new
#    table from the Arrow schema; use mode="append" to add to an existing one.
more = pa.table({"id": [3, 4], "name": ["Alan", "Edsger"]})
with conn.cursor() as cur:
    cur.adbc_ingest("more_people", more, mode="create")
conn.commit()

# 5. Read it all back.
with conn.cursor() as cur:
    cur.execute("SELECT id, name FROM people ORDER BY id")
    print(cur.fetch_arrow_table().to_pydict())

conn.close()
```

What each call does:

| Call | Role |
|---|---|
| `dbapi.connect(driver="odbc", db_kwargs={"uri": …})` | Load the bridge by name; the `uri` is the ODBC connection string. |
| `cur.execute(sql, parameters=…)` | Run one statement; parameters use `?`. |
| `cur.executemany(sql, rows)` | Run one parameterised statement once per row. |
| `cur.fetch_arrow_table()` | Return the current result set as a `pyarrow.Table`. |
| `cur.adbc_ingest(name, table, mode=…)` | Bulk-load an Arrow table; `mode` is `"create"`, `"append"`, or `"create_append"`. |
| `conn.commit()` | Commit the transaction (autocommit is off by default). |

> **Tip:** The Python package `adbcbridge` (from the wheel) wraps step 1 as
> `adbcbridge.connect(uri="Driver=SQLite3;Database=/tmp/first.db;")`, which also
> locates the bundled library for you and returns the same
> `adbc_driver_manager.dbapi.Connection`. Everything from step 2 on is
> identical. See [languages/python.md](../languages/python.md).

> **Troubleshooting:** If step 1 fails with `Can't open lib … : file not found`
> for a file that is there, or `cannot allocate memory in static TLS block`, see
> [TROUBLESHOOTING.md](../TROUBLESHOOTING.md) — those are ODBC-driver load
> issues (often a static-TLS conflict from importing pyarrow first), not bugs in
> your program.

## The same first query in every other language

Each block below connects to the same SQLite database and runs `SELECT 42 AS
answer`. The result is Arrow either way — a record batch or reader. Parameters
and bulk ingest work in every binding too; each language's page has the detail.

### Rust

```rust
use adbc_core::options::OptionDatabase;
use adbc_core::{Connection, Database, Driver, Statement};

fn main() -> Result<(), Box<dyn std::error::Error>> {
    let mut driver = adbcbridge::load()?;
    let uri = "Driver=SQLite3;Database=/tmp/first.db;";
    let database = driver.new_database_with_opts([(OptionDatabase::Uri, uri.into())])?;
    let mut connection = database.new_connection()?;
    let mut statement = connection.new_statement()?;
    statement.set_sql_query("SELECT 42 AS answer")?;
    for batch in statement.execute()? {
        println!("{} row(s)", batch?.num_rows());
    }
    Ok(())
}
```

Detail, including the `bundled` feature that compiles the driver from source:
[languages/rust.md](../languages/rust.md).

### C#

```csharp
using AdbcBridge;
using Apache.Arrow;
using Apache.Arrow.Adbc;
using Apache.Arrow.Ipc;

using AdbcConnection connection = Driver.Connect("Driver=SQLite3;Database=first.db;");
using AdbcStatement statement = connection.CreateStatement();
statement.SqlQuery = "SELECT 42 AS answer";
IArrowArrayStream stream = (IArrowArrayStream)statement.ExecuteQuery().Stream;
while (await stream.ReadNextRecordBatchAsync() is RecordBatch batch)
{
    Console.WriteLine(batch.Length);
}
```

Detail: [languages/csharp.md](../languages/csharp.md).

### Java

```java
import org.adbcbridge.AdbcBridge;
import org.apache.arrow.adbc.core.*;
import org.apache.arrow.memory.RootAllocator;
import org.apache.arrow.vector.VectorSchemaRoot;
import org.apache.arrow.vector.ipc.ArrowReader;

try (RootAllocator allocator = new RootAllocator();
     AdbcDatabase database =
         AdbcBridge.open(allocator, "Driver=SQLite3;Database=first.db;", null);
     AdbcConnection connection = database.connect();
     AdbcStatement statement = connection.createStatement()) {
  statement.setSqlQuery("SELECT 42 AS answer");
  try (AdbcStatement.QueryResult result = statement.executeQuery()) {
    ArrowReader reader = result.getReader();
    while (reader.loadNextBatch()) {
      VectorSchemaRoot batch = reader.getVectorSchemaRoot();
      System.out.println(batch.contentToTSVString());
    }
  }
}
```

On JDK 17 and later, start the JVM with
`--add-opens=java.base/java.nio=ALL-UNNAMED` or the first allocation fails.
Detail: [languages/java.md](../languages/java.md).

### Go

```go
package main

import (
	"context"
	"fmt"

	adbcbridge "github.com/singhpratech/adbcbridge/go"
	"github.com/apache/arrow-go/v18/arrow/memory"
)

func main() {
	ctx := context.Background()

	db, err := adbcbridge.Open(ctx, memory.DefaultAllocator,
		"Driver=SQLite3;Database=first.db;", nil)
	if err != nil {
		panic(err)
	}
	defer db.Close()

	cnxn, err := db.Open(ctx)
	if err != nil {
		panic(err)
	}
	defer cnxn.Close()

	stmt, err := cnxn.NewStatement()
	if err != nil {
		panic(err)
	}
	defer stmt.Close()
	if err := stmt.SetSqlQuery("SELECT 42 AS answer"); err != nil {
		panic(err)
	}
	rdr, _, err := stmt.ExecuteQuery(ctx)
	if err != nil {
		panic(err)
	}
	defer rdr.Release()
	for rdr.Next() {
		fmt.Println(rdr.RecordBatch())
	}
}
```

Building needs a C compiler (cgo). Detail: [languages/go.md](../languages/go.md).

### C

The C path uses the ADBC driver manager's flat C API directly and links against
the ADBC driver manager library; `"driver"` set to `"odbc"` resolves the bridge
by name, exactly as the higher-level bindings do under the hood.

```c
#include <arrow-adbc/adbc.h>
#include <stdio.h>

int main(void) {
  struct AdbcError error = ADBC_ERROR_INIT;
  struct AdbcDatabase database;
  struct AdbcConnection connection;
  struct AdbcStatement statement;
  struct ArrowArrayStream stream;
  int64_t rows = 0;

  AdbcDatabaseNew(&database, &error);
  AdbcDatabaseSetOption(&database, "driver", "odbc", &error);
  AdbcDatabaseSetOption(&database, "uri", "Driver=SQLite3;Database=first.db;", &error);
  AdbcDatabaseInit(&database, &error);

  AdbcConnectionNew(&connection, &error);
  AdbcConnectionInit(&connection, &database, &error);

  AdbcStatementNew(&connection, &statement, &error);
  AdbcStatementSetSqlQuery(&statement, "SELECT 42 AS answer", &error);
  AdbcStatementExecuteQuery(&statement, &stream, &rows, &error);

  /* Read Arrow batches from `stream` (get_schema / get_next), then: */
  stream.release(&stream);

  AdbcStatementRelease(&statement, &error);
  AdbcConnectionRelease(&connection, &error);
  AdbcDatabaseRelease(&database, &error);
  return 0;
}
```

Detail, including how to consume the `ArrowArrayStream` and check every
`AdbcStatusCode`: [languages/c.md](../languages/c.md).

## Next steps

- Change the connection string to reach a real database:
  [Connection strings](../reference/connection-strings.md).
- Tune batch size, decimals, prefetch and ingest:
  [Options](../reference/options.md).
- See what type each SQL column becomes in Arrow:
  [Types](../reference/types.md).
- Check whether your database is verified and what quirks it needs:
  [COMPATIBILITY.md](../COMPATIBILITY.md).
