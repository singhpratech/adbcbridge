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

//! Load the driver and run `SELECT 1` through the SQLite ODBC driver named by
//! `SQLITE_ODBC_DRIVER` (path or registered name).  Skipped with a message
//! when that variable is unset.

use std::error::Error;

use adbc_core::options::OptionDatabase;
use adbc_core::{Connection, Database, Driver, Statement};

fn sqlite_driver() -> Option<String> {
    match std::env::var("SQLITE_ODBC_DRIVER") {
        Ok(value) if !value.is_empty() => Some(value),
        _ => {
            eprintln!(
                "skipping: SQLITE_ODBC_DRIVER is not set (path or name of the SQLite ODBC driver)"
            );
            None
        }
    }
}

fn select_one(mut driver: adbcbridge::ManagedDriver, sqlite: &str) -> Result<(), Box<dyn Error>> {
    let uri = format!("Driver={sqlite};Database=:memory:;");
    let database = driver.new_database_with_opts([(OptionDatabase::Uri, uri.into())])?;
    let mut connection = database.new_connection()?;
    let mut statement = connection.new_statement()?;
    statement.set_sql_query("SELECT 1 AS one")?;
    let reader = statement.execute()?;
    let mut rows = 0;
    let mut columns = 0;
    for batch in reader {
        let batch = batch?;
        rows += batch.num_rows();
        columns = batch.num_columns();
        // Whatever integer width SQLite's ODBC driver reports, the value is 1.
        let cell = arrow_cell(batch.column(0));
        assert_eq!(
            cell.as_deref(),
            Some("1"),
            "column 0 = {:?}",
            batch.column(0)
        );
    }
    assert_eq!((rows, columns), (1, 1));
    Ok(())
}

/// The first value of `array`, rendered through arrow's Debug output so that
/// no arrow crate has to be a direct dependency of this test.
fn arrow_cell(array: &dyn std::fmt::Debug) -> Option<String> {
    // `RecordBatch::column` returns an `ArrayRef`; its Debug output looks like
    // "PrimitiveArray<Int64>\n[\n  1,\n]".  Pull out the first bracketed item.
    let text = format!("{array:?}");
    let body = text.split_once('[')?.1;
    Some(body.split([',', ']']).next()?.trim().to_string())
}

#[test]
fn select_one_through_driver_path() -> Result<(), Box<dyn Error>> {
    let Some(sqlite) = sqlite_driver() else {
        return Ok(());
    };
    let path = adbcbridge::driver_path()?;
    eprintln!("driver_path() = {}", path.display());
    assert!(path.is_absolute());
    assert!(path.is_file());
    select_one(adbcbridge::load()?, &sqlite)
}

/// The copy `build.rs` compiled, even when an environment variable points
/// `driver_path()` somewhere else.
#[cfg(feature = "bundled")]
#[test]
fn select_one_through_bundled_driver() -> Result<(), Box<dyn Error>> {
    let Some(sqlite) = sqlite_driver() else {
        return Ok(());
    };
    let bundled = std::path::Path::new(env!("ADBCBRIDGE_BUNDLED_DRIVER"));
    eprintln!("bundled driver = {}", bundled.display());
    assert!(bundled.is_file(), "{} was not built", bundled.display());
    select_one(adbcbridge::load_from(bundled)?, &sqlite)
}
