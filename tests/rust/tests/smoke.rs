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

//! Rust smoke test for `libadbc_driver_odbc.so`.
//!
//! Loads the driver through the ADBC driver manager, points it at the SQLite
//! ODBC driver over a throwaway database file, and asserts on the Arrow
//! `RecordBatch`es that come back.
//!
//! Requires `SQLITE_ODBC_DRIVER` to name the SQLite ODBC driver (a path to
//! `libsqlite3odbc.so`, or a driver name registered in `odbcinst.ini`).

use std::path::PathBuf;
use std::sync::Arc;

use adbc_core::options::{AdbcVersion, OptionDatabase};
use adbc_core::{Connection, Database, Driver, Statement};
use adbc_driver_manager::{ManagedConnection, ManagedDriver};
use arrow_array::cast::AsArray;
use arrow_array::types::Int32Type;
use arrow_array::{Array, Int64Array, RecordBatch, RecordBatchReader, StringArray};
use arrow_schema::{DataType, Field, Schema};
use tempfile::TempDir;

/// Path to the driver under test: `build/libadbc_driver_odbc.so` at the repo
/// root, overridable with `ADBC_ODBC_DRIVER`.
fn driver_path() -> PathBuf {
    match std::env::var_os("ADBC_ODBC_DRIVER") {
        Some(p) => PathBuf::from(p),
        None => PathBuf::from(env!("CARGO_MANIFEST_DIR"))
            .join("../../build/libadbc_driver_odbc.so")
            .canonicalize()
            .expect(
                "build/libadbc_driver_odbc.so not found - run \
                 `cmake -S . -B build && cmake --build build` first, \
                 or set ADBC_ODBC_DRIVER",
            ),
    }
}

/// The ODBC driver we bridge to. A bare name is looked up in `odbcinst.ini`.
fn sqlite_odbc_driver() -> String {
    std::env::var("SQLITE_ODBC_DRIVER").unwrap_or_else(|_| "SQLite3".to_string())
}

/// A connection to a fresh SQLite database file.
///
/// The [`TempDir`] is returned alongside so the caller keeps the database file
/// alive for as long as the connection.
fn connect() -> (ManagedConnection, TempDir) {
    let tempdir = tempfile::tempdir().expect("create temp dir");
    let db_file = tempdir.path().join("smoke.db");

    let mut driver = ManagedDriver::load_dynamic_from_filename(
        driver_path(),
        Some(b"AdbcDriverInit"),
        AdbcVersion::V110,
    )
    .expect("load libadbc_driver_odbc.so");

    let uri = format!(
        "Driver={};Database={};",
        sqlite_odbc_driver(),
        db_file.display()
    );
    let database = driver
        .new_database_with_opts([(OptionDatabase::Uri, uri.as_str().into())])
        .expect("open database");

    let connection = database.new_connection().expect("open connection");
    (connection, tempdir)
}

/// Run `sql` and drain the result into a single vector of batches.
fn query(connection: &mut ManagedConnection, sql: &str) -> (Arc<Schema>, Vec<RecordBatch>) {
    let mut statement = connection.new_statement().expect("new statement");
    statement.set_sql_query(sql).expect("set query");
    let reader = statement.execute().expect("execute");
    let schema = reader.schema();
    let batches = reader
        .collect::<Result<Vec<_>, _>>()
        .expect("read record batches");
    (schema, batches)
}

/// Run `sql` as DML and return the reported row count.
fn exec_update(connection: &mut ManagedConnection, sql: &str) -> Option<i64> {
    let mut statement = connection.new_statement().expect("new statement");
    statement.set_sql_query(sql).expect("set query");
    statement.execute_update().expect("execute_update")
}

/// Concatenate the rows of the `index`th column, which must be `Int32`.
fn int32_column(batches: &[RecordBatch], index: usize) -> Vec<Option<i32>> {
    batches
        .iter()
        .flat_map(|batch| {
            batch
                .column(index)
                .as_primitive::<Int32Type>()
                .iter()
                .collect::<Vec<_>>()
        })
        .collect()
}

/// Concatenate the rows of the `index`th column, which must be `Utf8`.
fn string_column(batches: &[RecordBatch], index: usize) -> Vec<Option<String>> {
    batches
        .iter()
        .flat_map(|batch| {
            batch
                .column(index)
                .as_string::<i32>()
                .iter()
                .map(|v| v.map(str::to_string))
                .collect::<Vec<_>>()
        })
        .collect()
}

#[test]
fn select_one() {
    let (mut connection, _tempdir) = connect();
    let (schema, batches) = query(&mut connection, "SELECT 1 AS one");

    assert_eq!(schema.fields().len(), 1, "one column, got {schema:?}");
    assert_eq!(schema.field(0).name(), "one");
    assert_eq!(
        schema.field(0).data_type(),
        &DataType::Int32,
        "SQLite's ODBC driver reports the literal as SQL_INTEGER"
    );

    let rows: i64 = batches.iter().map(|b| b.num_rows() as i64).sum();
    assert_eq!(rows, 1, "exactly one row");
    assert_eq!(int32_column(&batches, 0), vec![Some(1)]);
}

#[test]
fn parameterised_insert_and_select() {
    let (mut connection, _tempdir) = connect();

    exec_update(
        &mut connection,
        "CREATE TABLE people (id INTEGER PRIMARY KEY, name TEXT)",
    );

    // Parameterised INSERT: one bound RecordBatch, three rows, executed once.
    let params_schema = Arc::new(Schema::new(vec![
        Field::new("id", DataType::Int64, true),
        Field::new("name", DataType::Utf8, true),
    ]));
    let params = RecordBatch::try_new(
        params_schema,
        vec![
            Arc::new(Int64Array::from(vec![Some(1), Some(2), Some(3)])) as Arc<dyn Array>,
            Arc::new(StringArray::from(vec![
                Some("ada"),
                Some("grace"),
                None::<&str>,
            ])) as Arc<dyn Array>,
        ],
    )
    .expect("build parameter batch");

    let mut insert = connection.new_statement().expect("new statement");
    insert
        .set_sql_query("INSERT INTO people (id, name) VALUES (?, ?)")
        .expect("set query");
    insert.prepare().expect("prepare");
    insert.bind(params).expect("bind parameters");
    let inserted = insert.execute_update().expect("execute_update");
    drop(insert);
    assert_eq!(inserted, Some(3), "three rows inserted");

    // All three rows made it in, in order, with the NULL preserved.
    let (schema, batches) = query(&mut connection, "SELECT id, name FROM people ORDER BY id");
    assert_eq!(
        schema
            .fields()
            .iter()
            .map(|f| f.name().as_str())
            .collect::<Vec<_>>(),
        vec!["id", "name"]
    );
    assert_eq!(int32_column(&batches, 0), vec![Some(1), Some(2), Some(3)]);
    assert_eq!(
        string_column(&batches, 1),
        vec![Some("ada".to_string()), Some("grace".to_string()), None]
    );

    // Parameterised SELECT: bind a single-row batch as the WHERE value.
    let arg_schema = Arc::new(Schema::new(vec![Field::new("id", DataType::Int64, true)]));
    let arg = RecordBatch::try_new(
        arg_schema,
        vec![Arc::new(Int64Array::from(vec![Some(2)])) as Arc<dyn Array>],
    )
    .expect("build argument batch");

    let mut select = connection.new_statement().expect("new statement");
    select
        .set_sql_query("SELECT name FROM people WHERE id = ?")
        .expect("set query");
    select.prepare().expect("prepare");
    select.bind(arg).expect("bind parameter");
    let reader = select.execute().expect("execute");
    let batches = reader
        .collect::<Result<Vec<_>, _>>()
        .expect("read record batches");

    assert_eq!(
        string_column(&batches, 0),
        vec![Some("grace".to_string())],
        "only the row with id = 2"
    );
}

#[test]
fn error_carries_a_message() {
    let (mut connection, _tempdir) = connect();
    let mut statement = connection.new_statement().expect("new statement");
    statement
        .set_sql_query("SELECT * FROM no_such_table")
        .expect("set query");

    let error = statement
        .execute()
        .err()
        .expect("querying a missing table must fail");
    assert!(
        error.message.to_lowercase().contains("no_such_table"),
        "error should name the missing table, got: {}",
        error.message
    );
}
