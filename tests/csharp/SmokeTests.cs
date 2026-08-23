// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.

// C# smoke test for libadbc_driver_odbc.so.
//
// Loads the driver with Apache.Arrow.Adbc's native driver importer
// (CAdbcDriverImporter.Load), points it at the SQLite ODBC driver over a
// throwaway database file, and asserts on the Arrow RecordBatches that come
// back.
//
// Requires SQLITE_ODBC_DRIVER to name the SQLite ODBC driver (a path to
// libsqlite3odbc.so, or a driver name registered in odbcinst.ini).

using Apache.Arrow;
using Apache.Arrow.Adbc;
using Apache.Arrow.Adbc.C;
using Apache.Arrow.Ipc;
using Apache.Arrow.Types;
using Xunit;

namespace AdbcBridge.SmokeTests;

/// <summary>
/// One SQLite database file plus an open ADBC connection to it. Disposing the
/// fixture releases the connection, database and driver and deletes the file.
/// </summary>
internal sealed class Fixture : IDisposable
{
    private readonly string _directory;
    private readonly AdbcDriver _driver;
    private readonly AdbcDatabase _database;

    public AdbcConnection Connection { get; }

    /// <summary>
    /// The driver under test: build/libadbc_driver_odbc.so at the repo root,
    /// overridable with ADBC_ODBC_DRIVER.
    /// </summary>
    public static string DriverPath()
    {
        string? fromEnvironment = Environment.GetEnvironmentVariable("ADBC_ODBC_DRIVER");
        if (!string.IsNullOrEmpty(fromEnvironment))
        {
            return fromEnvironment;
        }

        // AppContext.BaseDirectory is tests/csharp/bin/<config>/net8.0/.
        return Path.GetFullPath(
            Path.Combine(AppContext.BaseDirectory, "..", "..", "..", "..", "..",
                         "build", "libadbc_driver_odbc.so"));
    }

    /// <summary>
    /// The ODBC driver we bridge to. A bare name is looked up in odbcinst.ini.
    /// </summary>
    private static string SqliteOdbcDriver()
    {
        string? fromEnvironment = Environment.GetEnvironmentVariable("SQLITE_ODBC_DRIVER");
        return string.IsNullOrEmpty(fromEnvironment) ? "SQLite3" : fromEnvironment;
    }

    public Fixture()
    {
        _directory = Path.Combine(Path.GetTempPath(),
                                  "adbcbridge-csharp-" + Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(_directory);

        string databaseFile = Path.Combine(_directory, "smoke.db");
        string uri = "Driver=" + SqliteOdbcDriver() + ";Database=" + databaseFile + ";";

        _driver = CAdbcDriverImporter.Load(DriverPath());
        _database = _driver.Open(new Dictionary<string, string> { ["uri"] = uri });
        Connection = _database.Connect(null);
    }

    /// <summary>Run <paramref name="sql"/> and drain the whole result set.</summary>
    public (Schema Schema, List<RecordBatch> Batches) Query(string sql)
    {
        using AdbcStatement statement = Connection.CreateStatement();
        statement.SqlQuery = sql;
        return Drain(statement.ExecuteQuery());
    }

    /// <summary>Run <paramref name="sql"/> as DML and return the reported row count.</summary>
    public long ExecuteUpdate(string sql)
    {
        using AdbcStatement statement = Connection.CreateStatement();
        statement.SqlQuery = sql;
        return statement.ExecuteUpdate().AffectedRows;
    }

    /// <summary>Read every batch out of a <see cref="QueryResult"/>.</summary>
    public static (Schema Schema, List<RecordBatch> Batches) Drain(QueryResult result)
    {
        IArrowArrayStream stream = Assert.IsAssignableFrom<IArrowArrayStream>(result.Stream);
        List<RecordBatch> batches = new List<RecordBatch>();
        while (true)
        {
            RecordBatch? batch =
                stream.ReadNextRecordBatchAsync().AsTask().GetAwaiter().GetResult();
            if (batch is null)
            {
                break;
            }

            batches.Add(batch);
        }

        return (stream.Schema, batches);
    }

    public void Dispose()
    {
        Connection.Dispose();
        _database.Dispose();
        _driver.Dispose();
        try
        {
            Directory.Delete(_directory, recursive: true);
        }
        catch (IOException)
        {
            // Best effort: the test already passed or failed on its own terms.
        }
    }
}

/// <summary>Column readers that concatenate across batches and keep nulls.</summary>
internal static class Columns
{
    public static List<int?> Int32(IEnumerable<RecordBatch> batches, int index)
    {
        return Read<int?, Int32Array>(batches, index, (array, row) => array.GetValue(row));
    }

    public static List<string?> String(IEnumerable<RecordBatch> batches, int index)
    {
        return Read<string?, StringArray>(
            batches, index, (array, row) => array.IsNull(row) ? null : array.GetString(row));
    }

    private static List<TValue> Read<TValue, TArray>(
        IEnumerable<RecordBatch> batches, int index, Func<TArray, int, TValue> get)
        where TArray : IArrowArray
    {
        List<TValue> values = new List<TValue>();
        foreach (RecordBatch batch in batches)
        {
            TArray array = Assert.IsType<TArray>(batch.Column(index));
            for (int row = 0; row < batch.Length; row++)
            {
                values.Add(get(array, row));
            }
        }

        return values;
    }
}

public class SmokeTests
{
    [Fact]
    public void LoadsTheDriverAndNegotiatesAdbc110()
    {
        using AdbcDriver driver = CAdbcDriverImporter.Load(Fixture.DriverPath());

        // The bridge implements the 1.1.0 ABI; the importer falls back to
        // 1.0.0 only if AdbcDriverInit rejects the newer version.
        Assert.Equal(AdbcVersion.Version_1_1_0, driver.DriverVersion);

        // Load defaults to the AdbcDriverInit export; the driver-manager
        // convention name AdbcDriverOdbcInit is exported too and works the same.
        using AdbcDriver alias =
            CAdbcDriverImporter.Load(Fixture.DriverPath(), "AdbcDriverOdbcInit");
        Assert.Equal(AdbcVersion.Version_1_1_0, alias.DriverVersion);
    }

    [Fact]
    public void SelectOne()
    {
        using Fixture fixture = new Fixture();
        (Schema schema, List<RecordBatch> batches) = fixture.Query("SELECT 1 AS one");

        Assert.Single(schema.FieldsList);
        Assert.Equal("one", schema.GetFieldByIndex(0).Name);

        // SQLite's ODBC driver describes the literal as SQL_INTEGER, which the
        // bridge maps to Arrow int32.
        Assert.Equal(ArrowTypeId.Int32, schema.GetFieldByIndex(0).DataType.TypeId);
        Assert.Equal(new List<int?> { 1 }, Columns.Int32(batches, 0));
    }

    [Fact]
    public void CreateInsertSelectWithNullAndUtf8()
    {
        using Fixture fixture = new Fixture();

        fixture.ExecuteUpdate("CREATE TABLE people (id INTEGER PRIMARY KEY, name TEXT)");
        long inserted = fixture.ExecuteUpdate(
            "INSERT INTO people (id, name) VALUES " +
            "(1, 'ada'), (2, 'ΑΘΗΝΑ ✈ 日本語'), (3, NULL)");
        Assert.Equal(3, inserted);

        (Schema schema, List<RecordBatch> batches) =
            fixture.Query("SELECT id, name FROM people ORDER BY id");

        Assert.Equal(new[] { "id", "name" },
                     schema.FieldsList.Select(field => field.Name).ToArray());
        Assert.Equal(new List<int?> { 1, 2, 3 }, Columns.Int32(batches, 0));

        // Non-ASCII round-trips as UTF-8 (the bridge asks the ODBC driver for
        // wide characters and transcodes), and the SQL NULL stays null rather
        // than turning into an empty string.
        Assert.Equal(new List<string?> { "ada", "ΑΘΗΝΑ ✈ 日本語", null },
                     Columns.String(batches, 1));

        RecordBatch last = batches[batches.Count - 1];
        Assert.True(last.Column(1).IsNull(last.Length - 1));
    }

    [Fact]
    public void ParameterisedInsertAndSelect()
    {
        using Fixture fixture = new Fixture();
        fixture.ExecuteUpdate("CREATE TABLE people (id INTEGER PRIMARY KEY, name TEXT)");

        // Parameterised INSERT: one bound RecordBatch, one column per '?' and
        // one row per execution.
        Schema parameters = new Schema.Builder()
            .Field(new Field("id", Int64Type.Default, nullable: true))
            .Field(new Field("name", StringType.Default, nullable: true))
            .Build();
        RecordBatch batch = new RecordBatch(
            parameters,
            new IArrowArray[]
            {
                new Int64Array.Builder().Append(1).Append(2).Append(3).Build(),
                new StringArray.Builder().Append("ada").Append("Ωmega ✈").AppendNull().Build(),
            },
            length: 3);

        using (AdbcStatement insert = fixture.Connection.CreateStatement())
        {
            insert.SqlQuery = "INSERT INTO people (id, name) VALUES (?, ?)";
            insert.Prepare();
            insert.Bind(batch, parameters);
            Assert.Equal(3, insert.ExecuteUpdate().AffectedRows);
        }

        (_, List<RecordBatch> rows) = fixture.Query("SELECT id, name FROM people ORDER BY id");
        Assert.Equal(new List<int?> { 1, 2, 3 }, Columns.Int32(rows, 0));
        Assert.Equal(new List<string?> { "ada", "Ωmega ✈", null }, Columns.String(rows, 1));

        // Parameterised SELECT: bind a single-row batch as the WHERE value.
        Schema argument = new Schema.Builder()
            .Field(new Field("id", Int64Type.Default, nullable: true))
            .Build();
        RecordBatch argumentBatch = new RecordBatch(
            argument,
            new IArrowArray[] { new Int64Array.Builder().Append(2).Build() },
            length: 1);

        using AdbcStatement select = fixture.Connection.CreateStatement();
        select.SqlQuery = "SELECT name FROM people WHERE id = ?";
        select.Prepare();
        select.Bind(argumentBatch, argument);
        (_, List<RecordBatch> matched) = Fixture.Drain(select.ExecuteQuery());

        Assert.Equal(new List<string?> { "Ωmega ✈" }, Columns.String(matched, 0));
    }

    [Fact]
    public void ErrorCarriesAMessage()
    {
        using Fixture fixture = new Fixture();
        using AdbcStatement statement = fixture.Connection.CreateStatement();
        statement.SqlQuery = "SELECT * FROM no_such_table";

        // The importer raises its own internal AdbcException subclass, so match
        // on the base type rather than the exact one.
        Exception? thrown = Record.Exception(() => statement.ExecuteQuery());
        AdbcException error = Assert.IsAssignableFrom<AdbcException>(thrown);
        Assert.Contains("no_such_table", error.Message, StringComparison.OrdinalIgnoreCase);
    }
}
