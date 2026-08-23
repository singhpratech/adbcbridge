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

// The "Use from C#" snippets from the top-level README.md. They are never run
// - the paths in them are placeholders - but `dotnet test` compiles this file,
// so the snippets cannot silently rot.

using Apache.Arrow;
using Apache.Arrow.Adbc;
using Apache.Arrow.Adbc.C;
using Apache.Arrow.Ipc;
using Apache.Arrow.Types;

namespace AdbcBridge.SmokeTests;

internal static class ReadmeSnippet
{
    internal static async Task QuerySnippet()
    {
        using AdbcDriver driver = CAdbcDriverImporter.Load("/path/to/libadbc_driver_odbc.so");

        string uri = "Driver=/usr/lib/x86_64-linux-gnu/odbc/libsqlite3odbc.so;Database=my.db;";
        using AdbcDatabase database = driver.Open(new Dictionary<string, string> { ["uri"] = uri });
        using AdbcConnection connection = database.Connect(null);

        using AdbcStatement statement = connection.CreateStatement();
        statement.SqlQuery = "SELECT * FROM my_table";
        QueryResult result = statement.ExecuteQuery();

        IArrowArrayStream stream = result.Stream!;
        while (await stream.ReadNextRecordBatchAsync() is RecordBatch batch)
        {
            Console.WriteLine($"{batch.Length} rows");
        }
    }

    internal static void BindSnippet(AdbcConnection connection)
    {
        Schema parameters = new Schema.Builder()
            .Field(new Field("id", Int64Type.Default, nullable: true))
            .Field(new Field("name", StringType.Default, nullable: true))
            .Build();
        RecordBatch batch = new RecordBatch(
            parameters,
            new IArrowArray[]
            {
                new Int64Array.Builder().Append(1).Append(2).Build(),
                new StringArray.Builder().Append("ada").AppendNull().Build(),
            },
            length: 2);

        using AdbcStatement insert = connection.CreateStatement();
        insert.SqlQuery = "INSERT INTO my_table (id, name) VALUES (?, ?)";
        insert.Prepare();
        insert.Bind(batch, parameters);
        Console.WriteLine($"{insert.ExecuteUpdate().AffectedRows} rows inserted");
    }
}
