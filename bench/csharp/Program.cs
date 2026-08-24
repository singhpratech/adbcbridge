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
//
// C# port of bench/rust/src/main.rs, one database per run.
//
// Four measurements over the same 4-column table
// `(id int32, val float64, txt utf8, dt date32)`, each the median of --reps
// timings after one warmup:
//
//   1. adbc ingest - libadbc_driver_odbc.so through Apache.Arrow.Adbc's native
//      driver importer: bulk ingest in `create` mode, autocommit off, one
//      commit at the end, row count verified afterwards.
//   2. adbc fetch  - `SELECT id, val, txt, dt` drained into Apache.Arrow
//      RecordBatches through the same driver.
//   3. odbc ingest - the same rows through System.Data.Odbc: a prepared
//      `INSERT ... VALUES (?, ?, ?, ?)` executed row by row inside one
//      transaction. The table is created by an empty ADBC ingest first, so both
//      ingest paths write into identical DDL.
//   4. odbc fetch  - the same SELECT read row by row with an OdbcDataReader, no
//      Arrow: the plain-.NET ODBC floor for this read.
//
// Usage:
//
//   ADBC_ODBC_DRIVER=build/libadbc_driver_odbc.so POSTGRES_CONN='Driver=...;' \
//       bench_cs --rows 10000 --fetch-rows 100000 postgres
//
// The database name is only a label and an environment-variable prefix; see
// run.sh, which resolves the connection strings out of tests/compat.

using System.Data;
using System.Data.Odbc;
using System.Diagnostics;
using System.Globalization;
using System.Text;
using System.Text.Json;
using Apache.Arrow;
using Apache.Arrow.Adbc;
using Apache.Arrow.Adbc.C;
using Apache.Arrow.Ipc;
using Apache.Arrow.Types;

namespace AdbcBridge.Bench;

/// <summary>One measurement: its median seconds, or why it did not finish.</summary>
internal sealed record Step(double Seconds, string? Error)
{
    public static Step Ok(double seconds) => new(seconds, null);

    public static Step Failed(string message) => new(0, Bench.Trim(message));

    public double? Rate(int rows) =>
        Error is null && Seconds > 0 ? rows / Seconds : null;
}

internal static class Bench
{

    private static string DriverPath = "";
    private static string ConnUri = "";
    private static string[] Setup = System.Array.Empty<string>();

    private static int Main(string[] arguments)
    {
        int rows = 10_000, fetchRows = 100_000, reps = 3;
        string? database = null;
        for (int i = 0; i < arguments.Length; i++)
        {
            switch (arguments[i])
            {
                case "--rows": rows = Value(arguments, ref i); break;
                case "--fetch-rows": fetchRows = Value(arguments, ref i); break;
                case "--reps": reps = Value(arguments, ref i); break;
                case "-h" or "--help":
                    Console.WriteLine("bench_cs [--rows N] [--fetch-rows N] [--reps N] <dbname>");
                    return 0;
                default:
                    if (arguments[i].StartsWith('-'))
                    {
                        return Die($"unknown option {arguments[i]}");
                    }

                    database = arguments[i];
                    break;
            }
        }

        if (database is null)
        {
            return Die("expected a database name");
        }

        string prefix = database.ToUpperInvariant();
        DriverPath = Environment.GetEnvironmentVariable("ADBC_ODBC_DRIVER") ?? "";
        if (DriverPath.Length == 0)
        {
            return Die("ADBC_ODBC_DRIVER must name libadbc_driver_odbc.so");
        }

        ConnUri = Environment.GetEnvironmentVariable(prefix + "_CONN") ?? "";
        if (ConnUri.Length == 0)
        {
            return Die($"{prefix}_CONN must hold the ODBC connection string");
        }

        // The bare table name handed to the ADBC ingest.
        string table = (Environment.GetEnvironmentVariable("ADBC_BENCH_TABLE") ?? "adbc_bench_cs")
                       + (Environment.GetEnvironmentVariable("ADBC_MATRIX_SUFFIX") ?? "");
        Setup = (Environment.GetEnvironmentVariable(prefix + "_SETUP") ?? "")
            .Split('\n', StringSplitOptions.RemoveEmptyEntries | StringSplitOptions.TrimEntries);

        // Every spelling of that name later SQL might have to use. The bridge's
        // ingest quotes the table name with the database's own quote character,
        // so the quoted spelling is what usually answers; a case-folding
        // database (Oracle, Db2) can also answer to the upper-cased one, which
        // conn.py passes in <DB>_TABLE from the compat matrix's `ident` hook.
        string[] candidates = new[]
        {
            "\"" + table + "\"",
            Environment.GetEnvironmentVariable(prefix + "_TABLE") ?? table,
            table,
        }.Distinct().ToArray();

        // Ingest a single row and find out which spelling of the table and
        // column names reaches what that produced, so nothing downstream has to
        // guess: the ingest quotes every identifier, which on a case-folding
        // database makes the lower-cased spelling the only one that resolves.
        string ident = candidates[0];
        string select = "SELECT id, val, txt, dt FROM " + candidates[0];
        Step probe = Attempt(() =>
        {
            using Session session = Session.Open(autoCommit: false);
            DropTable(session, candidates, autoCommit: false);
            using RecordBatch one = MakeBatch(1);
            AdbcIngest(session, table, one);
            string? found = candidates.FirstOrDefault(candidate =>
            {
                try
                {
                    AdbcCount(session, candidate);
                    return true;
                }
                catch (Exception)
                {
                    return false;
                }
            });
            if (found is null)
            {
                throw new InvalidOperationException(
                    $"ingested {table} but no spelling of the name selects from it");
            }

            ident = found;
            foreach (string sql in new[]
                     {
                         $"SELECT \"id\", \"val\", \"txt\", \"dt\" FROM {found}",
                         $"SELECT id, val, txt, dt FROM {found}",
                     })
            {
                try
                {
                    AdbcFetch(session, sql);
                    select = sql;
                    return 0;
                }
                catch (Exception)
                {
                    // Try the next spelling.
                }
            }

            throw new InvalidOperationException(
                "the benchmark table has no readable id/val/txt/dt columns");
        });
        if (probe.Error is not null)
        {
            Console.Error.WriteLine($"{database}: {probe.Error}");
        }

        // 1. adbc ingest: drop, ingest --rows rows, commit, verify the count.
        Step adbcIngest = Attempt(() =>
        {
            using Session session = Session.Open(autoCommit: false);
            double seconds = Repeat(reps, () =>
            {
                DropTable(session, candidates, autoCommit: false);
                using RecordBatch batch = MakeBatch(rows);
                return AdbcIngest(session, table, batch);
            });
            Verify(session, ident, rows);
            return seconds;
        });

        // ADBC_BENCH_NO_NATIVE: skip the System.Data.Odbc comparison entirely and
        // leave its columns empty. Some ODBC drivers end the whole process from the
        // plain ODBC path -- DuckDB's throws a C++ exception out of SQLExecute, which
        // std::terminate turns into an abort no catch block sees -- and that would
        // lose the ADBC numbers too. With this set the ADBC columns still land.
        bool noNative = !string.IsNullOrEmpty(
            Environment.GetEnvironmentVariable("ADBC_BENCH_NO_NATIVE"));
        Step skipped = Step.Failed("skipped: ADBC_BENCH_NO_NATIVE");

        // 2. System.Data.Odbc ingest into the table ADBC's DDL created.
        Step odbcIngest = noNative ? skipped : Attempt(() =>
        {
            using Session session = Session.Open(autoCommit: false);
            using OdbcConnection odbc = OdbcConnect();
            double seconds = Repeat(reps, () =>
            {
                DropTable(session, candidates, autoCommit: false);
                using RecordBatch empty = MakeBatch(0);
                AdbcIngest(session, table, empty);
                return OdbcIngest(odbc, ident, rows);
            });
            Verify(session, ident, rows);
            return seconds;
        });

        // Load the bigger table the two fetch steps read back.
        Step loaded = Attempt(() =>
        {
            using Session session = Session.Open(autoCommit: false);
            DropTable(session, candidates, autoCommit: false);
            using RecordBatch batch = MakeBatch(fetchRows);
            AdbcIngest(session, table, batch);
            Verify(session, ident, fetchRows);
            return 0;
        });

        Step adbcFetch = loaded, odbcFetch = loaded;
        if (loaded.Error is null)
        {
            adbcFetch = Attempt(() =>
            {
                using Session session = Session.Open(autoCommit: true);
                return Repeat(reps, () => Timed(fetchRows, () => AdbcFetch(session, select)));
            });
            odbcFetch = noNative ? skipped : Attempt(() =>
            {
                using OdbcConnection odbc = OdbcConnect();
                return Repeat(reps, () => Timed(fetchRows, () => OdbcFetch(odbc, select)));
            });
        }

        // Leave nothing of ours behind on a shared server.
        try
        {
            using Session session = Session.Open(autoCommit: true);
            DropTable(session, candidates, autoCommit: true);
        }
        catch (Exception)
        {
            // Best effort.
        }

        Console.WriteLine(JsonSerializer.Serialize(new Dictionary<string, object>
        {
            ["lang"] = "csharp",
            ["db"] = database,
            ["rows"] = rows,
            ["fetch_rows"] = fetchRows,
            ["reps"] = reps,
            ["adbc_ingest"] = JsonStep(rows, adbcIngest),
            ["adbc_fetch"] = JsonStep(fetchRows, adbcFetch),
            ["odbc_ingest"] = JsonStep(rows, odbcIngest),
            ["odbc_fetch"] = JsonStep(fetchRows, odbcFetch),
        }));
        Console.WriteLine(
            $"| csharp | {database} | {Cell(rows, adbcIngest)} | {Cell(fetchRows, adbcFetch)} " +
            $"| {Cell(rows, odbcIngest)} | {Cell(fetchRows, odbcFetch)} |");

        foreach ((string name, Step step) in new[]
                 {
                     ("adbc ingest", adbcIngest), ("adbc fetch", adbcFetch),
                     ("odbc ingest", odbcIngest), ("odbc fetch", odbcFetch),
                 })
        {
            if (step.Error is not null)
            {
                Console.Error.WriteLine($"{database}: {name}: {step.Error}");
            }
        }

        return 0;
    }

    // ------------------------------------------------------------- the payload

    /// <summary>
    /// The benchmark table: `(id int32, val float64, txt utf8, dt date32)`, the
    /// same shape and values bench/matrix_bench.py and bench/rust use.
    /// </summary>
    private static RecordBatch MakeBatch(int n)
    {
        Int32Array.Builder ids = new Int32Array.Builder().Reserve(n);
        DoubleArray.Builder values = new DoubleArray.Builder().Reserve(n);
        StringArray.Builder texts = new StringArray.Builder().Reserve(n);
        ArrowBuffer.Builder<int> dates = new ArrowBuffer.Builder<int>(Math.Max(n, 1));
        for (int i = 0; i < n; i++)
        {
            ids.Append(i);
            values.Append(i * 0.5);
            texts.Append("row-" + i.ToString("D12", CultureInfo.InvariantCulture));
            dates.Append(i % 20_000);
        }

        Schema schema = new Schema.Builder()
            .Field(new Field("id", Int32Type.Default, true))
            .Field(new Field("val", DoubleType.Default, true))
            .Field(new Field("txt", StringType.Default, true))
            .Field(new Field("dt", Date32Type.Default, true))
            .Build();
        // Date32 straight from day counts, so the values match the other ports
        // byte for byte rather than going through a calendar type.
        Date32Array dt = new Date32Array(dates.Build(), ArrowBuffer.Empty, n, 0, 0);
        return new RecordBatch(
            schema,
            new IArrowArray[] { ids.Build(), values.Build(), texts.Build(), dt },
            n);
    }

    // -------------------------------------------------------------------- ADBC

    /// <summary>
    /// A driver, database and connection opened through
    /// libadbc_driver_odbc.so, disposed together.
    /// </summary>
    private sealed class Session : IDisposable
    {
        private readonly AdbcDriver _driver;
        private readonly AdbcDatabase _database;

        public AdbcConnection Connection { get; }

        private Session(AdbcDriver driver, AdbcDatabase database, AdbcConnection connection)
        {
            _driver = driver;
            _database = database;
            Connection = connection;
        }

        /// <summary>
        /// Open a connection through the driver under test.
        /// <c>adbc.odbc.delegate=never</c> keeps the bridge on its own ODBC path
        /// instead of handing the connection to a database's native ADBC driver,
        /// so the numbers describe this driver.
        /// </summary>
        /// <summary>
        /// ADBC_BENCH_AUTOCOMMIT: keep autocommit on even for the ingest steps.
        /// MonetDBODBClib's SQLEndTran is a no-op, so a connection with autocommit
        /// off never commits anything and the rows are gone by the time the fetch
        /// step opens its own connection; on such a driver the only way to measure
        /// the ingest at all is to let the bridge batch the stream itself, which is
        /// what bench/matrix_bench.py does everywhere.
        /// </summary>
        private static readonly bool ForceAutoCommit =
            !string.IsNullOrEmpty(Environment.GetEnvironmentVariable("ADBC_BENCH_AUTOCOMMIT"));

        internal static bool ForceAutoCommitting => ForceAutoCommit;

        public static Session Open(bool autoCommit)
        {
            autoCommit = autoCommit || ForceAutoCommit;
            AdbcDriver driver = CAdbcDriverImporter.Load(DriverPath);
            AdbcDatabase database = driver.Open(new Dictionary<string, string>
            {
                ["uri"] = ConnUri,
                ["adbc.odbc.delegate"] = "never",
            });
            AdbcConnection connection = database.Connect(null);
            Session session = new Session(driver, database, connection);
            foreach (string sql in Setup)
            {
                Execute(session, sql);
            }

            if (!autoCommit)
            {
                connection.AutoCommit = false;
            }

            return session;
        }

        public void Dispose()
        {
            Connection.Dispose();
            _database.Dispose();
            _driver.Dispose();
        }
    }

    /// <summary>Run a statement for its side effect, discarding any row count.</summary>
    private static void Execute(Session session, string sql)
    {
        using AdbcStatement statement = session.Connection.CreateStatement();
        statement.SqlQuery = sql;
        statement.ExecuteUpdate();
    }

    /// <summary>
    /// DROP TABLE under every spelling of the name, ignoring failures: the table
    /// usually does not exist yet, and a case-folding database answers to only
    /// one of the spellings.
    /// </summary>
    private static void DropTable(Session session, string[] names, bool autoCommit)
    {
        autoCommit = autoCommit || Session.ForceAutoCommitting;
        foreach (string name in names)
        {
            bool dropped = true;
            try
            {
                Execute(session, "DROP TABLE " + name);
            }
            catch (Exception)
            {
                // Expected when the table is not there.
                dropped = false;
            }

            if (!autoCommit)
            {
                try
                {
                    // A failed statement leaves the transaction aborted on
                    // PostgreSQL and on MonetDB -- and MonetDB refuses to end that
                    // with a COMMIT, insisting on a ROLLBACK ("Current transaction
                    // is aborted (please ROLLBACK)"). Commit the spelling that
                    // dropped, roll back the ones that did not, so the ingest's
                    // CREATE TABLE starts from a clean transaction either way.
                    if (dropped)
                    {
                        session.Connection.Commit();
                    }
                    else
                    {
                        session.Connection.Rollback();
                        // MonetDBODBClib's SQLEndTran does not clear an aborted
                        // transaction -- the next statement still fails with
                        // "Current transaction is aborted (please ROLLBACK)". A
                        // literal ROLLBACK does clear it, and is harmless where the
                        // driver manager already ended the transaction properly.
                        try
                        {
                            Execute(session, "ROLLBACK");
                        }
                        catch (Exception)
                        {
                            // Best effort.
                        }
                    }
                }
                catch (Exception)
                {
                    // Best effort.
                }
            }
        }
    }

    /// <summary>
    /// Bulk-ingest <paramref name="batch"/> into <paramref name="table"/> in
    /// `create` mode, commit once, and return the seconds that took (DDL + rows
    /// + commit, as matrix_bench.py).
    /// </summary>
    private static double AdbcIngest(Session session, string table, RecordBatch batch)
    {
        long start = Stopwatch.GetTimestamp();
        using (AdbcStatement statement = session.Connection.CreateStatement())
        {
            statement.SetOption(AdbcOptions.Ingest.TargetTable, table);
            statement.SetOption(AdbcOptions.Ingest.Mode, AdbcOptions.IngestMode.Create);
            statement.Bind(batch, batch.Schema);
            statement.ExecuteUpdate();
        }

        // Nothing to commit when ADBC_BENCH_AUTOCOMMIT put the connection in autocommit.
        if (!Session.ForceAutoCommitting)
        {
            session.Connection.Commit();
        }

        return Stopwatch.GetElapsedTime(start).TotalSeconds;
    }

    /// <summary>Drain a query into Arrow batches through the bridge and count the rows.</summary>
    private static int AdbcFetch(Session session, string sql)
    {
        using AdbcStatement statement = session.Connection.CreateStatement();
        statement.SqlQuery = sql;
        QueryResult result = statement.ExecuteQuery();
        // `using`: the stream must be released on this thread, before the statement.
        // Left to the GC, ArrowArrayStreamExporter's release callback runs on the .NET
        // finalizer thread, and an ODBC driver whose client library is thread-affine
        // (MySQL/MariaDB Connector/ODBC, whose libmysqlclient/libmariadb keeps a
        // per-thread MEM_ROOT) segfaults inside SQLCloseCursor when it does.
        using IArrowArrayStream stream = result.Stream
                                   ?? throw new InvalidOperationException("no result stream");
        int rows = 0;
        while (true)
        {
            using RecordBatch? batch =
                stream.ReadNextRecordBatchAsync().AsTask().GetAwaiter().GetResult();
            if (batch is null)
            {
                break;
            }

            rows += batch.Length;
        }

        return rows;
    }

    /// <summary>
    /// SELECT COUNT(*), tolerant of whatever integral Arrow type the database
    /// reports the count as.
    /// </summary>
    private static long AdbcCount(Session session, string ident)
    {
        using AdbcStatement statement = session.Connection.CreateStatement();
        statement.SqlQuery = "SELECT COUNT(*) FROM " + ident;
        QueryResult result = statement.ExecuteQuery();
        // `using`: the stream must be released on this thread, before the statement.
        // Left to the GC, ArrowArrayStreamExporter's release callback runs on the .NET
        // finalizer thread, and an ODBC driver whose client library is thread-affine
        // (MySQL/MariaDB Connector/ODBC, whose libmysqlclient/libmariadb keeps a
        // per-thread MEM_ROOT) segfaults inside SQLCloseCursor when it does.
        using IArrowArrayStream stream = result.Stream
                                   ?? throw new InvalidOperationException("no result stream");
        while (true)
        {
            using RecordBatch? batch =
                stream.ReadNextRecordBatchAsync().AsTask().GetAwaiter().GetResult();
            if (batch is null)
            {
                break;
            }

            if (batch.Length == 0)
            {
                continue;
            }

            return batch.Column(0) switch
            {
                Int64Array a => a.GetValue(0) ?? 0,
                Int32Array a => a.GetValue(0) ?? 0,
                // ClickHouse reports COUNT(*) as UInt64; the count never approaches
                // long.MaxValue, so these casts are safe.
                UInt64Array a => (long)(a.GetValue(0) ?? 0),
                UInt32Array a => a.GetValue(0) ?? 0,
                UInt16Array a => a.GetValue(0) ?? 0,
                UInt8Array a => a.GetValue(0) ?? 0,
                Int16Array a => a.GetValue(0) ?? 0,
                DoubleArray a => (long)(a.GetValue(0) ?? 0),
                Decimal128Array a => (long)a.GetValue(0)!,
                StringArray a => long.Parse(a.GetString(0)!.Trim(), CultureInfo.InvariantCulture),
                var other => throw new InvalidOperationException(
                    $"unexpected COUNT(*) type {other.Data.DataType}"),
            };
        }

        throw new InvalidOperationException("COUNT(*) returned no rows");
    }

    private static void Verify(Session session, string ident, int want)
    {
        long got = AdbcCount(session, ident);
        if (got != want)
        {
            throw new InvalidOperationException($"wrong row count {got} != {want}");
        }
    }

    // ------------------------------------------------------------ System.Data.Odbc

    private static OdbcConnection OdbcConnect()
    {
        OdbcConnection connection = new OdbcConnection(ConnUri);
        connection.Open();
        foreach (string sql in Setup)
        {
            using OdbcCommand command = new OdbcCommand(sql, connection);
            command.ExecuteNonQuery();
        }

        return connection;
    }

    /// <summary>
    /// Read the SELECT row by row with an OdbcDataReader and count the rows. No
    /// Arrow: this is the plain-.NET ODBC cost of the same read.
    /// </summary>
    private static int OdbcFetch(OdbcConnection connection, string sql)
    {
        using OdbcCommand command = new OdbcCommand(sql, connection);
        using OdbcDataReader reader = command.ExecuteReader();
        int rows = 0;
        while (reader.Read())
        {
            // Touch every column, as the Arrow path must.
            _ = reader.IsDBNull(0) ? 0 : reader.GetInt32(0);
            _ = reader.IsDBNull(1) ? 0 : reader.GetDouble(1);
            _ = reader.IsDBNull(2) ? null : reader.GetString(2);
            _ = reader.IsDBNull(3) ? default : reader.GetDate(3);
            rows++;
        }

        return rows;
    }

    /// <summary>
    /// Send <paramref name="rows"/> rows to <paramref name="ident"/> with a
    /// prepared statement inside one transaction and return the seconds that
    /// took; the table must already exist.
    /// </summary>
    private static double OdbcIngest(OdbcConnection connection, string ident, int rows)
    {
        DateTime epoch = new DateTime(1970, 1, 1, 0, 0, 0, DateTimeKind.Unspecified);
        long start = Stopwatch.GetTimestamp();
        using OdbcTransaction transaction = connection.BeginTransaction();
        using (OdbcCommand command =
               new OdbcCommand($"INSERT INTO {ident} VALUES (?, ?, ?, ?)", connection, transaction))
        {
            OdbcParameter id = command.Parameters.Add("id", OdbcType.Int);
            OdbcParameter value = command.Parameters.Add("val", OdbcType.Double);
            OdbcParameter text = command.Parameters.Add("txt", OdbcType.VarChar, 32);
            OdbcParameter date = command.Parameters.Add("dt", OdbcType.Date);
            command.Prepare();
            for (int i = 0; i < rows; i++)
            {
                id.Value = i;
                value.Value = i * 0.5;
                text.Value = "row-" + i.ToString("D12", CultureInfo.InvariantCulture);
                date.Value = epoch.AddDays(i % 20_000);
                command.ExecuteNonQuery();
            }
        }

        transaction.Commit();
        return Stopwatch.GetElapsedTime(start).TotalSeconds;
    }

    // -------------------------------------------------------------- measurement

    /// <summary>Median of <paramref name="reps"/> timings after one warmup.</summary>
    private static double Repeat(int reps, Func<double> once)
    {
        once();
        List<double> times = new List<double>(reps);
        for (int i = 0; i < reps; i++)
        {
            times.Add(once());
        }

        times.Sort();
        return times[times.Count / 2];
    }

    /// <summary>Time one call of <paramref name="body"/>, which reports the rows it saw.</summary>
    private static double Timed(int expected, Func<int> body)
    {
        long start = Stopwatch.GetTimestamp();
        int got = body();
        double seconds = Stopwatch.GetElapsedTime(start).TotalSeconds;
        if (got != expected)
        {
            throw new InvalidOperationException($"read {got} rows, expected {expected}");
        }

        return seconds;
    }

    /// <summary>Keep a failure from ending the run: every step reports independently.</summary>
    private static Step Attempt(Func<double> body)
    {
        try
        {
            return Step.Ok(body());
        }
        catch (Exception error)
        {
            return Step.Failed(error.Message);
        }
    }

    // ------------------------------------------------------------------- output

    internal static string Trim(string message)
    {
        string line = message.Split('\n', 2)[0].Trim();
        return line.Length > 160 ? line[..160] : line;
    }

    /// <summary>1234567.8 -> "1,234,568".</summary>
    private static string Thousands(double value)
    {
        string digits = value.ToString("F0", CultureInfo.InvariantCulture);
        StringBuilder builder = new StringBuilder(digits.Length + digits.Length / 3);
        for (int i = 0; i < digits.Length; i++)
        {
            if (i > 0 && (digits.Length - i) % 3 == 0)
            {
                builder.Append(',');
            }

            builder.Append(digits[i]);
        }

        return builder.ToString();
    }

    private static string Cell(int rows, Step step)
    {
        double? rate = step.Rate(rows);
        return rate is null ? "—" : Thousands(rate.Value);
    }

    private static Dictionary<string, object> JsonStep(int rows, Step step) =>
        step.Error is not null
            ? new Dictionary<string, object> { ["error"] = step.Error }
            : new Dictionary<string, object>
            {
                ["secs"] = step.Seconds,
                ["rate"] = rows / step.Seconds,
            };

    // ---------------------------------------------------------------- arguments

    private static int Value(string[] arguments, ref int i)
    {
        if (i + 1 >= arguments.Length)
        {
            Environment.Exit(Die("missing value for an option"));
        }

        return int.Parse(arguments[++i], CultureInfo.InvariantCulture);
    }

    private static int Die(string message)
    {
        Console.Error.WriteLine("bench_cs: " + message);
        Environment.Exit(2);
        return 2;
    }
}
