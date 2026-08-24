// Copyright 2026 the adbcbridge authors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//
// SPDX-License-Identifier: Apache-2.0

using Apache.Arrow;
using Apache.Arrow.Adbc;
using Apache.Arrow.Ipc;
using Apache.Arrow.Types;
using Xunit;

namespace AdbcBridge.Tests;

public class DriverTests
{
    /// <summary>
    /// The real thing: find the driver, load it, bridge to SQLite through the
    /// ODBC driver SQLITE_ODBC_DRIVER names, run SELECT 1 and read the batch.
    /// </summary>
    [SkippableFact]
    public void FindsLoadsAndQueriesSqlite()
    {
        string? sqlite = Environment.GetEnvironmentVariable("SQLITE_ODBC_DRIVER");
        Skip.If(string.IsNullOrEmpty(sqlite),
                "SQLITE_ODBC_DRIVER is not set: point it at libsqlite3odbc (a path, or a name from odbcinst.ini)");

        string path = Driver.Path();
        Assert.True(File.Exists(path), "Driver.Path() returned a missing file: " + path);
        Assert.True(Path.IsPathRooted(path), "Driver.Path() is not absolute: " + path);

        using (AdbcDriver driver = Driver.Load())
        {
            Assert.Equal(AdbcVersion.Version_1_1_0, driver.DriverVersion);
        }

        using AdbcConnection connection = Driver.Connect("Driver=" + sqlite + ";Database=:memory:;");
        using AdbcStatement statement = connection.CreateStatement();
        statement.SqlQuery = "SELECT 1 AS one";
        QueryResult result = statement.ExecuteQuery();
        IArrowArrayStream stream = Assert.IsAssignableFrom<IArrowArrayStream>(result.Stream);

        Assert.Single(stream.Schema.FieldsList);
        Assert.Equal("one", stream.Schema.GetFieldByIndex(0).Name);
        Assert.Equal(ArrowTypeId.Int32, stream.Schema.GetFieldByIndex(0).DataType.TypeId);

        RecordBatch? batch = stream.ReadNextRecordBatchAsync().AsTask().GetAwaiter().GetResult();
        Assert.NotNull(batch);
        Assert.Equal(1, batch!.Length);
        Int32Array column = Assert.IsType<Int32Array>(batch.Column(0));
        Assert.Equal(1, column.GetValue(0));

        RecordBatch? end = stream.ReadNextRecordBatchAsync().AsTask().GetAwaiter().GetResult();
        Assert.Null(end);
    }

    /// <summary>
    /// A variable that points nowhere is an error that names the variable and
    /// what was looked at, not a silent fall-through to some other copy.
    /// </summary>
    [Fact]
    public void ReportsWhereItLooked()
    {
        string missing = Path.Combine(Path.GetTempPath(), "adbcbridge-" + Guid.NewGuid().ToString("N"), "nope.so");
        string? previous = Environment.GetEnvironmentVariable(Driver.LibraryVariable);
        Environment.SetEnvironmentVariable(Driver.LibraryVariable, missing);
        try
        {
            DriverNotFoundException error = Assert.Throws<DriverNotFoundException>(() => Driver.Path());
            Assert.Contains(Driver.LibraryVariable, error.Message);
            Assert.Contains(missing, error.Message);
            Assert.Contains(error.Searched, place => place.Contains(missing));
        }
        finally
        {
            Environment.SetEnvironmentVariable(Driver.LibraryVariable, previous);
        }
    }
}
