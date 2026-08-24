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

//! Rust port of `bench/matrix_bench.py`, one database per run.
//!
//! Four measurements over the same 4-column table
//! `(id int32, val float64, txt utf8, dt date32)`, each the median of `--reps`
//! timings after one warmup:
//!
//! 1. **adbc ingest** — `libadbc_driver_odbc.so` through the Rust ADBC driver
//!    manager: bulk ingest in `create` mode, autocommit off, one commit at the
//!    end, row count verified afterwards.
//! 2. **adbc fetch** — `SELECT id, val, txt, dt` drained into Arrow
//!    `RecordBatch`es through the same driver.
//! 3. **odbc-api fetch / arrow-odbc fetch** — the same `SELECT` read straight
//!    from the ODBC driver by the [`odbc_api`] crate into a column-wise row-set
//!    buffer, and by the [`arrow_odbc`] crate into Arrow `RecordBatch`es.
//! 4. **odbc-api ingest** — the same rows sent by [`odbc_api`]'s
//!    [`ColumnarBulkInserter`](odbc_api::ColumnarBulkInserter): a prepared
//!    `INSERT ... VALUES (?, ?, ?, ?)` with array-bound parameters, autocommit
//!    off, one commit at the end. The table itself is created by an empty ADBC
//!    ingest first, so both ingest paths write into identical DDL.
//!
//! Usage:
//!
//! ```text
//! ADBC_ODBC_DRIVER=build/libadbc_driver_odbc.so POSTGRES_CONN='Driver=...;' \
//!     bench_rs --rows 10000 --fetch-rows 100000 postgres
//! ```
//!
//! The database name is only a label and an environment-variable prefix; see
//! `run.sh`, which resolves the connection strings out of `tests/compat`.

use std::env;
use std::error::Error;
use std::sync::Arc;
use std::time::Instant;

use adbc_core::options::{
    AdbcVersion, IngestMode, OptionConnection, OptionDatabase, OptionStatement,
};
use adbc_core::{Connection, Database, Driver, Optionable, Statement};
use adbc_driver_manager::{ManagedConnection, ManagedDriver};
use arrow_array::{Array, Date32Array, Float64Array, Int32Array, RecordBatch, StringArray};
use arrow_schema::{DataType, Field, Schema};
use odbc_api::buffers::{BufferDesc, ColumnarDynBuffer};
use odbc_api::BindParamDesc;
use odbc_api::{sys::Date, ConnectionOptions, Cursor, Environment, Nullability, ResultSetMetadata};

type Res<T> = Result<T, Box<dyn Error>>;

/// Rows per fetched row set, and per array-bound `INSERT`.
const ROWSET: usize = 8192;

/// Widest text cell the odbc-api / arrow-odbc readers will allocate room for.
/// See [`clamp_text`]; 1024 is wide enough for any `VARCHAR` in this workload
/// and narrow enough that a row set stays a few megabytes.
const MAX_TEXT: usize = 1024;

// ---------------------------------------------------------------- arguments

struct Args {
    rows: usize,
    fetch_rows: usize,
    reps: usize,
    db: String,
}

fn parse_args() -> Args {
    let mut rows = 10_000usize;
    let mut fetch_rows = 100_000usize;
    let mut reps = 3usize;
    let mut db = None;
    let mut it = env::args().skip(1);
    while let Some(arg) = it.next() {
        let mut value = || {
            it.next()
                .unwrap_or_else(|| die("missing value for an option"))
                .parse::<usize>()
                .unwrap_or_else(|e| die(&format!("bad number: {e}")))
        };
        match arg.as_str() {
            "--rows" => rows = value(),
            "--fetch-rows" => fetch_rows = value(),
            "--reps" => reps = value(),
            "-h" | "--help" => {
                println!("bench_rs [--rows N] [--fetch-rows N] [--reps N] <dbname>");
                std::process::exit(0);
            }
            other if other.starts_with('-') => die(&format!("unknown option {other}")),
            other => db = Some(other.to_string()),
        }
    }
    Args {
        rows,
        fetch_rows,
        reps,
        db: db.unwrap_or_else(|| die("expected a database name")),
    }
}

fn die(message: &str) -> ! {
    eprintln!("bench_rs: {message}");
    std::process::exit(2);
}

// ------------------------------------------------------------- the payload

/// The benchmark table: `(id int32, val float64, txt utf8, dt date32)`, the
/// same shape and values `bench/matrix_bench.py` uses.
fn make_batch(n: usize) -> RecordBatch {
    let schema = Arc::new(Schema::new(vec![
        Field::new("id", DataType::Int32, true),
        Field::new("val", DataType::Float64, true),
        Field::new("txt", DataType::Utf8, true),
        Field::new("dt", DataType::Date32, true),
    ]));
    let id = Int32Array::from_iter_values((0..n).map(|i| i as i32));
    let val = Float64Array::from_iter_values((0..n).map(|i| i as f64 * 0.5));
    let txt = StringArray::from_iter_values((0..n).map(|i| format!("row-{i:012}")));
    let dt = Date32Array::from_iter_values((0..n).map(|i| (i % 20_000) as i32));
    RecordBatch::try_new(
        schema,
        vec![
            Arc::new(id) as Arc<dyn Array>,
            Arc::new(val),
            Arc::new(txt),
            Arc::new(dt),
        ],
    )
    .expect("build the benchmark batch")
}

/// Days since 1970-01-01 to a calendar date, so `date32` values can go into an
/// ODBC `SQL_DATE_STRUCT`. Howard Hinnant's `civil_from_days`.
fn civil_from_days(days: i32) -> Date {
    let z = days + 719_468;
    let era = if z >= 0 { z } else { z - 146_096 } / 146_097;
    let doe = (z - era * 146_097) as u32; // [0, 146096]
    let yoe = (doe - doe / 1460 + doe / 36_524 - doe / 146_096) / 365; // [0, 399]
    let doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    let mp = (5 * doy + 2) / 153;
    let d = doy - (153 * mp + 2) / 5 + 1;
    let m = if mp < 10 { mp + 3 } else { mp - 9 };
    let y = yoe as i32 + era * 400 + i32::from(m <= 2);
    Date {
        year: y as i16,
        month: m as u16,
        day: d as u16,
    }
}

// -------------------------------------------------------------------- ADBC

/// Open a connection through `libadbc_driver_odbc.so`.
///
/// `adbc.odbc.delegate=never` keeps the bridge on its own ODBC path instead of
/// handing the connection to a database's native ADBC driver, so the numbers
/// describe this driver.
fn adbc_connect(
    driver: &str,
    uri: &str,
    setup: &[String],
    autocommit: bool,
) -> Res<ManagedConnection> {
    // ADBC_BENCH_AUTOCOMMIT: keep autocommit on even for the ingest steps.
    // MonetDBODBClib's SQLEndTran is a no-op, so a connection with autocommit off
    // never commits anything and the rows are gone by the time the fetch step opens
    // its own connection; on such a driver the only way to measure the ingest at
    // all is to let the bridge batch the stream itself, which is what
    // bench/matrix_bench.py does everywhere.
    let autocommit = autocommit || env::var_os("ADBC_BENCH_AUTOCOMMIT").is_some();
    let mut driver = ManagedDriver::load_dynamic_from_filename(
        driver,
        Some(b"AdbcDriverInit"),
        AdbcVersion::V110,
    )?;
    let database = driver.new_database_with_opts([
        (OptionDatabase::Uri, uri.into()),
        (
            OptionDatabase::Other("adbc.odbc.delegate".into()),
            "never".into(),
        ),
    ])?;
    let mut connection = database.new_connection()?;
    for sql in setup {
        exec(&mut connection, sql)?;
    }
    if !autocommit {
        connection.set_option(OptionConnection::AutoCommit, "false".into())?;
    }
    Ok(connection)
}

/// Run a statement for its side effect, discarding any row count.
fn exec(connection: &mut ManagedConnection, sql: &str) -> Res<()> {
    let mut statement = connection.new_statement()?;
    statement.set_sql_query(sql)?;
    statement.execute_update()?;
    Ok(())
}

/// `DROP TABLE` under every spelling of the name, ignoring failures: the table
/// usually does not exist yet, and a case-folding database answers to only one
/// of the spellings.
fn drop_table(connection: &mut ManagedConnection, names: &[String], autocommit: bool) {
    for name in names {
        let dropped = exec(connection, &format!("DROP TABLE {name}")).is_ok();
        if !autocommit {
            // A failed statement leaves the transaction aborted on PostgreSQL and on
            // MonetDB -- and MonetDB refuses to end that with a COMMIT, insisting on a
            // ROLLBACK ("Current transaction is aborted (please ROLLBACK)"). Commit the
            // spelling that dropped, roll back the ones that did not, so the ingest's
            // CREATE TABLE starts from a clean transaction either way.
            if dropped {
                let _ = connection.commit();
            } else {
                let _ = connection.rollback();
                // MonetDBODBClib's SQLEndTran does not clear an aborted transaction
                // -- the next statement still fails with "Current transaction is
                // aborted (please ROLLBACK)". A literal ROLLBACK does clear it, and
                // is harmless where the driver manager already ended the
                // transaction properly.
                let _ = exec(connection, "ROLLBACK");
            }
        }
    }
}

/// Bulk ingest `batch` into `table` in `create` mode, committing once, and
/// return the seconds that took (DDL + rows + commit, as `matrix_bench.py`).
fn adbc_ingest(connection: &mut ManagedConnection, table: &str, batch: RecordBatch) -> Res<f64> {
    let start = Instant::now();
    let mut statement = connection.new_statement()?;
    statement.set_option(OptionStatement::TargetTable, table.into())?;
    statement.set_option(OptionStatement::IngestMode, IngestMode::Create.into())?;
    statement.bind(batch)?;
    statement.execute_update()?;
    drop(statement);
    // Nothing to commit when ADBC_BENCH_AUTOCOMMIT put the connection in autocommit.
    if env::var_os("ADBC_BENCH_AUTOCOMMIT").is_none() {
        connection.commit()?;
    }
    Ok(start.elapsed().as_secs_f64())
}

/// Drain `sql` into Arrow batches through the bridge and count the rows.
fn adbc_fetch(connection: &mut ManagedConnection, sql: &str) -> Res<usize> {
    let mut statement = connection.new_statement()?;
    statement.set_sql_query(sql)?;
    let reader = statement.execute()?;
    let mut rows = 0;
    for batch in reader {
        rows += batch?.num_rows();
    }
    Ok(rows)
}

/// `SELECT COUNT(*)`, tolerant of whatever integral Arrow type the database
/// reports the count as.
fn adbc_count(connection: &mut ManagedConnection, ident: &str) -> Res<i64> {
    let mut statement = connection.new_statement()?;
    statement.set_sql_query(format!("SELECT COUNT(*) FROM {ident}"))?;
    let reader = statement.execute()?;
    for batch in reader {
        let batch = batch?;
        if batch.num_rows() == 0 {
            continue;
        }
        let column = batch.column(0);
        return match column.data_type() {
            DataType::Int64 => Ok(as_prim::<arrow_array::types::Int64Type>(column)),
            DataType::Int32 => Ok(as_prim::<arrow_array::types::Int32Type>(column) as i64),
            DataType::Float64 => Ok(as_prim::<arrow_array::types::Float64Type>(column) as i64),
            DataType::Decimal128(_, 0) => {
                Ok(as_prim::<arrow_array::types::Decimal128Type>(column) as i64)
            }
            DataType::Utf8 => Ok(arrow_array::cast::AsArray::as_string::<i32>(column)
                .value(0)
                .trim()
                .parse()?),
            other => Err(format!("unexpected COUNT(*) type {other:?}").into()),
        };
    }
    Err("COUNT(*) returned no rows".into())
}

fn as_prim<T: arrow_array::types::ArrowPrimitiveType>(column: &Arc<dyn Array>) -> T::Native {
    arrow_array::cast::AsArray::as_primitive::<T>(column).value(0)
}

// ---------------------------------------------------------------- odbc-api

fn odbc_connect<'e>(
    environment: &'e Environment,
    uri: &str,
    setup: &[String],
    autocommit: bool,
) -> Res<odbc_api::Connection<'e>> {
    let connection =
        environment.connect_with_connection_string(uri, ConnectionOptions::default())?;
    for sql in setup {
        connection.execute(sql, (), None)?;
    }
    if !autocommit {
        connection.set_autocommit(false)?;
    }
    Ok(connection)
}

/// Cap the width of a text buffer at [`MAX_TEXT`].
///
/// Several ODBC drivers describe a text column by the widest value the *type*
/// could hold rather than the widest one the table holds: sqliteodbc reports
/// 65,536 characters for the `VARCHAR(20)` here, the Oracle driver reports
/// 2,147,483,647. A row set of 8192 such cells is a buffer nobody wants to
/// allocate, let alone stride through, and leaving it uncapped makes the read
/// look an order of magnitude slower than it is. Both `odbc-api` and
/// `arrow-odbc` tell callers to pick their own upper bound; this is ours. The
/// bridge caps the same way with `adbc.odbc.max_bind_bytes`, so both sides of
/// the comparison are bounded.
fn clamp_text(desc: BufferDesc) -> BufferDesc {
    match desc {
        BufferDesc::Text { max_str_len } => BufferDesc::Text {
            max_str_len: max_str_len.clamp(1, MAX_TEXT),
        },
        BufferDesc::WText { max_str_len } => BufferDesc::WText {
            max_str_len: max_str_len.clamp(1, MAX_TEXT),
        },
        other => other,
    }
}

/// Read `sql` into a column-wise row-set buffer, `ROWSET` rows at a time, and
/// count the rows. No Arrow: this is the raw ODBC cost of the same read.
fn odbc_api_fetch(connection: &odbc_api::Connection<'_>, sql: &str) -> Res<usize> {
    let mut cursor = connection
        .execute(sql, (), None)?
        .ok_or("the SELECT produced no result set")?;
    let mut description = Default::default();
    let mut descs = Vec::new();
    for index in 0..cursor.num_result_cols()? {
        cursor.describe_col(index as u16 + 1, &mut description)?;
        let nullable = matches!(
            description.nullability,
            Nullability::Unknown | Nullability::Nullable
        );
        let desc = BufferDesc::from_data_type(description.data_type, nullable)
            .unwrap_or(BufferDesc::Text { max_str_len: 255 });
        if env::var_os("BENCH_RS_DEBUG").is_some() {
            eprintln!(
                "column {}: {:?} -> {desc:?}",
                index + 1,
                description.data_type
            );
        }
        descs.push(clamp_text(desc));
    }
    if env::var_os("BENCH_RS_DEBUG").is_some() {
        eprintln!("odbc-api buffer descriptions: {descs:?}");
    }
    let mut block = cursor.bind_buffer(ColumnarDynBuffer::from_descs(ROWSET, descs))?;
    let mut rows = 0;
    while let Some(batch) = block.fetch()? {
        rows += batch.num_rows();
    }
    Ok(rows)
}

/// Read `sql` into Arrow `RecordBatch`es with `arrow-odbc` and count the rows.
fn arrow_odbc_fetch(connection: &odbc_api::Connection<'_>, sql: &str) -> Res<usize> {
    let cursor = connection
        .execute(sql, (), None)?
        .ok_or("the SELECT produced no result set")?;
    let reader = arrow_odbc::OdbcReaderBuilder::new()
        .with_max_num_rows_per_batch(ROWSET)
        .with_max_text_size(MAX_TEXT)
        .build(cursor)?;
    let mut rows = 0;
    for batch in reader {
        rows += batch?.num_rows();
    }
    Ok(rows)
}

/// Send `rows` rows to `ident` with array-bound parameters and commit once.
/// Returns the seconds that took; the table must already exist.
fn odbc_api_ingest(connection: &odbc_api::Connection<'_>, ident: &str, rows: usize) -> Res<f64> {
    let sql = format!("INSERT INTO {ident} VALUES (?, ?, ?, ?)");
    let capacity = rows.clamp(1, ROWSET);
    let start = Instant::now();
    let prepared = connection.prepare(&sql)?;
    let mut inserter = prepared.into_column_inserter(
        capacity,
        [
            BindParamDesc::i32(false),
            BindParamDesc::f64(false),
            BindParamDesc::text(20),
            BindParamDesc::date(false),
        ],
    )?;
    let mut offset = 0;
    while offset < rows {
        let n = capacity.min(rows - offset);
        inserter.set_num_rows(n);
        {
            let column = inserter
                .column_mut(0)
                .as_slice::<i32>()
                .ok_or("id is not i32")?;
            for (slot, i) in column.iter_mut().zip(offset..offset + n) {
                *slot = i as i32;
            }
        }
        {
            let column = inserter
                .column_mut(1)
                .as_slice::<f64>()
                .ok_or("val is not f64")?;
            for (slot, i) in column.iter_mut().zip(offset..offset + n) {
                *slot = i as f64 * 0.5;
            }
        }
        {
            let mut column = inserter.column_mut(2).as_text().ok_or("txt is not text")?;
            for i in 0..n {
                column.set_cell(i, Some(format!("row-{:012}", offset + i).as_bytes()));
            }
        }
        {
            let column = inserter
                .column_mut(3)
                .as_slice::<Date>()
                .ok_or("dt is not a date")?;
            for (slot, i) in column.iter_mut().zip(offset..offset + n) {
                *slot = civil_from_days((i % 20_000) as i32);
            }
        }
        let _ = inserter.execute()?;
        offset += n;
    }
    drop(inserter);
    if env::var_os("ADBC_BENCH_AUTOCOMMIT").is_none() {
        connection.commit()?;
    }
    Ok(start.elapsed().as_secs_f64())
}

// ------------------------------------------------------------- measurement

/// Median of `reps` timings after one warmup.
fn repeat(reps: usize, mut once: impl FnMut() -> Res<f64>) -> Res<f64> {
    once()?;
    let mut times = Vec::with_capacity(reps);
    for _ in 0..reps {
        times.push(once()?);
    }
    times.sort_by(|a, b| a.partial_cmp(b).expect("no NaN timings"));
    Ok(times[times.len() / 2])
}

/// Time one call of `body`, which must report the row count it saw.
fn timed(expected: usize, body: impl FnOnce() -> Res<usize>) -> Res<f64> {
    let start = Instant::now();
    let got = body()?;
    let secs = start.elapsed().as_secs_f64();
    if got != expected {
        return Err(format!("read {got} rows, expected {expected}").into());
    }
    Ok(secs)
}

/// Keep a failure from ending the run: every step reports independently.
///
/// A panic counts as a failure too: `odbc-api` panics rather than returning an
/// error when a driver breaks the ODBC contract (the Db2 CLI driver's 32-bit
/// `SQLLEN` is the case in point), and one such step should not take the other
/// four down with it.
fn attempt<T>(what: impl FnOnce() -> Res<T>) -> Result<T, String> {
    let caught = std::panic::catch_unwind(std::panic::AssertUnwindSafe(what));
    let outcome = caught.unwrap_or_else(|payload| {
        let message = payload
            .downcast_ref::<&str>()
            .map(|s| (*s).to_string())
            .or_else(|| payload.downcast_ref::<String>().cloned())
            .unwrap_or_else(|| "panicked".into());
        Err(format!("panicked: {message}").into())
    });
    outcome.map_err(|e| {
        let text = e.to_string();
        if env::var_os("BENCH_RS_DEBUG").is_some() {
            eprintln!("--- {text}");
        }
        let line = text.lines().next().unwrap_or("error").trim();
        line.chars().take(160).collect()
    })
}

// ------------------------------------------------------------------ output

type Step = Result<f64, String>;

fn rate(rows: usize, step: &Step) -> Option<f64> {
    match step {
        Ok(secs) if *secs > 0.0 => Some(rows as f64 / secs),
        _ => None,
    }
}

/// `1234567.8` -> `1,234,568`.
fn thousands(value: f64) -> String {
    let digits = format!("{:.0}", value);
    let mut out = String::new();
    for (i, c) in digits.chars().enumerate() {
        if i > 0 && (digits.len() - i) % 3 == 0 {
            out.push(',');
        }
        out.push(c);
    }
    out
}

fn cell(value: Option<f64>) -> String {
    value.map(thousands).unwrap_or_else(|| "—".into())
}

fn ratio(a: Option<f64>, b: Option<f64>) -> String {
    match (a, b) {
        (Some(a), Some(b)) if b > 0.0 => format!("{:.1}×", a / b),
        _ => "—".into(),
    }
}

fn json_escape(s: &str) -> String {
    let mut out = String::with_capacity(s.len());
    for c in s.chars() {
        match c {
            '"' => out.push_str("\\\""),
            '\\' => out.push_str("\\\\"),
            '\n' | '\r' | '\t' => out.push(' '),
            c if (c as u32) < 0x20 => out.push(' '),
            c => out.push(c),
        }
    }
    out
}

fn json_step(name: &str, rows: usize, step: &Step) -> String {
    match step {
        Ok(secs) => format!(
            "\"{name}\": {{\"secs\": {:.6}, \"rate\": {:.1}}}",
            secs,
            rows as f64 / secs
        ),
        Err(message) => format!("\"{name}\": {{\"error\": \"{}\"}}", json_escape(message)),
    }
}

// -------------------------------------------------------------------- main

fn main() {
    let args = parse_args();
    let prefix = args.db.to_uppercase();
    let driver = env::var("ADBC_ODBC_DRIVER")
        .unwrap_or_else(|_| die("ADBC_ODBC_DRIVER must name libadbc_driver_odbc.so"));
    let uri = env::var(format!("{prefix}_CONN")).unwrap_or_else(|_| {
        die(&format!(
            "{prefix}_CONN must hold the ODBC connection string"
        ))
    });

    // The bare table name handed to the ADBC ingest.
    let table = format!(
        "adbc_bench_rs{}",
        env::var("ADBC_MATRIX_SUFFIX").unwrap_or_default()
    );
    let setup: Vec<String> = env::var(format!("{prefix}_SETUP"))
        .unwrap_or_default()
        .lines()
        .filter(|s| !s.trim().is_empty())
        .map(|s| s.to_string())
        .collect();
    // Every spelling of that name later SQL might have to use, and every one
    // worth attempting a DROP on. The bridge's ingest quotes the table name
    // with the database's own quote character, so the quoted spelling is what
    // usually answers; a case-folding database (Oracle, Db2) can also answer to
    // the upper-cased one, which `conn.py` passes in `<DB>_TABLE` from the
    // compat matrix's `ident` hook.
    let mut candidates = vec![
        format!("\"{table}\""),
        env::var(format!("{prefix}_TABLE")).unwrap_or_else(|_| table.clone()),
        table.clone(),
    ];
    candidates.dedup();
    let drops = candidates.clone();

    let environment = Environment::new().unwrap_or_else(|e| die(&format!("ODBC environment: {e}")));

    let vendor = attempt(|| {
        let connection = odbc_connect(&environment, &uri, &[], true)?;
        Ok(connection.database_management_system_name()?)
    })
    .unwrap_or_else(|_| args.db.clone());

    // Ingest a single row and find out which spelling of the table and column
    // names reaches what that produced, so nothing downstream has to guess: the
    // ingest quotes every identifier, which on a case-folding database (Oracle,
    // Db2) makes the lower-cased spelling the only one that resolves.
    let (ident, select) = attempt(|| {
        let mut connection = adbc_connect(&driver, &uri, &setup, false)?;
        drop_table(&mut connection, &drops, false);
        adbc_ingest(&mut connection, &table, make_batch(1))?;
        let ident = candidates
            .iter()
            .find(|candidate| adbc_count(&mut connection, candidate).is_ok())
            .ok_or_else(|| format!("ingested {table} but no spelling of the name selects from it"))?
            .clone();
        let select = [
            format!(r#"SELECT "id", "val", "txt", "dt" FROM {ident}"#),
            format!("SELECT id, val, txt, dt FROM {ident}"),
        ]
        .into_iter()
        .find(|sql| adbc_fetch(&mut connection, sql).is_ok())
        .ok_or("the benchmark table has no readable id/val/txt/dt columns")?;
        Ok((ident, select))
    })
    .unwrap_or_else(|message| {
        eprintln!("{}: {message}", args.db);
        let ident = candidates[0].clone();
        let select = format!("SELECT id, val, txt, dt FROM {ident}");
        (ident, select)
    });

    // 1. adbc ingest: drop, ingest --rows rows, commit, verify the count.
    let adbc_ingest_step: Step = attempt(|| {
        let mut connection = adbc_connect(&driver, &uri, &setup, false)?;
        let secs = repeat(args.reps, || {
            drop_table(&mut connection, &drops, false);
            adbc_ingest(&mut connection, &table, make_batch(args.rows))
        })?;
        let got = adbc_count(&mut connection, &ident)?;
        if got != args.rows as i64 {
            return Err(format!("wrong row count {got} != {}", args.rows).into());
        }
        Ok(secs)
    });

    // ADBC_BENCH_NO_NATIVE: skip the odbc-api comparison entirely and leave its
    // columns empty. Some ODBC drivers abort the whole process from the plain ODBC
    // path -- DuckDB's throws a C++ exception out of SQLExecute, which Rust cannot
    // catch -- and that would take the ADBC numbers down with it. With this set the
    // ADBC columns are still measured.
    let no_native = env::var_os("ADBC_BENCH_NO_NATIVE").is_some();
    let skipped = || -> Step { Err("skipped: ADBC_BENCH_NO_NATIVE".to_string()) };

    // 2. odbc-api ingest of the same rows into the table ADBC's DDL created.
    let odbc_ingest_step: Step = if no_native { skipped() } else { attempt(|| {
        let mut adbc = adbc_connect(&driver, &uri, &setup, false)?;
        let odbc = odbc_connect(&environment, &uri, &setup, false)?;
        let secs = repeat(args.reps, || {
            odbc.rollback()?;
            drop_table(&mut adbc, &drops, false);
            adbc_ingest(&mut adbc, &table, make_batch(0))?;
            odbc_api_ingest(&odbc, &ident, args.rows)
        })?;
        let got = adbc_count(&mut adbc, &ident)?;
        if got != args.rows as i64 {
            return Err(format!("wrong row count {got} != {}", args.rows).into());
        }
        Ok(secs)
    })};

    // Load the bigger table the three fetch steps read back.
    let loaded = attempt(|| {
        let mut connection = adbc_connect(&driver, &uri, &setup, false)?;
        drop_table(&mut connection, &drops, false);
        adbc_ingest(&mut connection, &table, make_batch(args.fetch_rows))?;
        let got = adbc_count(&mut connection, &ident)?;
        if got != args.fetch_rows as i64 {
            return Err(format!("wrong row count {got} != {}", args.fetch_rows).into());
        }
        Ok(())
    });

    let (adbc_fetch_step, odbc_fetch_step, arrow_fetch_step): (Step, Step, Step) = match &loaded {
        Err(e) => (Err(e.clone()), Err(e.clone()), Err(e.clone())),
        Ok(()) => (
            attempt(|| {
                let mut connection = adbc_connect(&driver, &uri, &setup, true)?;
                repeat(args.reps, || {
                    timed(args.fetch_rows, || adbc_fetch(&mut connection, &select))
                })
            }),
            if no_native { skipped() } else { attempt(|| {
                let connection = odbc_connect(&environment, &uri, &setup, true)?;
                repeat(args.reps, || {
                    timed(args.fetch_rows, || odbc_api_fetch(&connection, &select))
                })
            })},
            if no_native { skipped() } else { attempt(|| {
                let connection = odbc_connect(&environment, &uri, &setup, true)?;
                repeat(args.reps, || {
                    timed(args.fetch_rows, || arrow_odbc_fetch(&connection, &select))
                })
            })},
        ),
    };

    // Leave nothing of ours behind on a shared server.
    if let Ok(mut connection) = adbc_connect(&driver, &uri, &setup, true) {
        drop_table(&mut connection, &drops, true);
    }

    println!(
        "{{\"db\": \"{}\", \"vendor\": \"{}\", \"rows\": {}, \"fetch_rows\": {}, \"reps\": {}, {}, {}, {}, {}, {}}}",
        json_escape(&args.db),
        json_escape(&vendor),
        args.rows,
        args.fetch_rows,
        args.reps,
        json_step("adbc_ingest", args.rows, &adbc_ingest_step),
        json_step("odbc_api_ingest", args.rows, &odbc_ingest_step),
        json_step("adbc_fetch", args.fetch_rows, &adbc_fetch_step),
        json_step("odbc_api_fetch", args.fetch_rows, &odbc_fetch_step),
        json_step("arrow_odbc_fetch", args.fetch_rows, &arrow_fetch_step),
    );

    let ingest = rate(args.rows, &adbc_ingest_step);
    let ingest_odbc = rate(args.rows, &odbc_ingest_step);
    let fetch = rate(args.fetch_rows, &adbc_fetch_step);
    let fetch_odbc = rate(args.fetch_rows, &odbc_fetch_step);
    let fetch_arrow = rate(args.fetch_rows, &arrow_fetch_step);
    println!(
        "| {} ({}) | {} | {} | {} | {} | {} | {} | {} |",
        args.db,
        vendor,
        cell(ingest),
        cell(ingest_odbc),
        ratio(ingest, ingest_odbc),
        cell(fetch),
        cell(fetch_odbc),
        cell(fetch_arrow),
        ratio(fetch, fetch_odbc),
    );

    for (name, step) in [
        ("adbc ingest", &adbc_ingest_step),
        ("odbc-api ingest", &odbc_ingest_step),
        ("adbc fetch", &adbc_fetch_step),
        ("odbc-api fetch", &odbc_fetch_step),
        ("arrow-odbc fetch", &arrow_fetch_step),
    ] {
        if let Err(message) = step {
            eprintln!("{}: {name}: {message}", args.db);
        }
    }
}
