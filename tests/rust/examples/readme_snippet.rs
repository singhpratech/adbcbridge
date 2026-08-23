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

//! The "Use from Rust" snippet from the top-level `README.md`, kept here so
//! `cargo test` fails if the snippet ever stops compiling.
//!
//! It is never run: the paths in it are placeholders.

use adbc_core::options::{AdbcVersion, OptionDatabase};
use adbc_core::{Connection, Database, Driver, Statement};
use adbc_driver_manager::ManagedDriver;

fn main() -> Result<(), Box<dyn std::error::Error>> {
    let mut driver = ManagedDriver::load_dynamic_from_filename(
        "/path/to/libadbc_driver_odbc.so",
        Some(b"AdbcDriverInit"),
        AdbcVersion::V110,
    )?;

    let uri = "Driver=/usr/lib/x86_64-linux-gnu/odbc/libsqlite3odbc.so;Database=my.db;";
    let database = driver.new_database_with_opts([(OptionDatabase::Uri, uri.into())])?;
    let mut connection = database.new_connection()?;

    let mut statement = connection.new_statement()?;
    statement.set_sql_query("SELECT * FROM my_table")?;
    for batch in statement.execute()? {
        let batch = batch?; // arrow_array::RecordBatch
        println!("{} rows", batch.num_rows());
    }
    Ok(())
}
