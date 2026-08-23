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

// Go port of bench/rust/src/main.rs, one database per run.
//
// Four measurements over the same 4-column table
// `(id int32, val float64, txt utf8, dt date32)`, each the median of -reps
// timings after one warmup:
//
//  1. adbc ingest  - libadbc_driver_odbc.so through the Go ADBC driver manager
//     (github.com/apache/arrow-adbc/go/adbc/drivermgr): bulk ingest in `create`
//     mode, autocommit off, one commit at the end, row count verified after.
//  2. adbc fetch   - `SELECT id, val, txt, dt` drained into arrow-go Records
//     through the same driver.
//  3. odbc ingest  - the same rows through database/sql and the
//     github.com/alexbrainman/odbc driver: a prepared
//     `INSERT ... VALUES (?, ?, ?, ?)` executed row by row inside one
//     transaction. The table is created by an empty ADBC ingest first, so both
//     ingest paths write into identical DDL.
//  4. odbc fetch   - the same SELECT read row by row through database/sql, no
//     Arrow: the plain-Go ODBC floor for this read.
//
// Usage:
//
//	ADBC_ODBC_DRIVER=build/libadbc_driver_odbc.so POSTGRES_CONN='Driver=...;' \
//	    bench_go -rows 10000 -fetch-rows 100000 postgres
//
// The database name is only a label and an environment-variable prefix; see
// run.sh, which resolves the connection strings out of tests/compat.
package main

import (
	"context"
	"database/sql"
	"encoding/json"
	"flag"
	"fmt"
	"os"
	"sort"
	"strconv"
	"strings"
	"time"

	"github.com/apache/arrow-adbc/go/adbc"
	"github.com/apache/arrow-adbc/go/adbc/drivermgr"
	"github.com/apache/arrow-go/v18/arrow"
	"github.com/apache/arrow-go/v18/arrow/array"
	"github.com/apache/arrow-go/v18/arrow/memory"

	_ "github.com/alexbrainman/odbc"
)

var ctx = context.Background()

// ---------------------------------------------------------------- the payload

// benchSchema is the benchmark table: the same shape and values
// bench/matrix_bench.py and bench/rust use.
var benchSchema = arrow.NewSchema([]arrow.Field{
	{Name: "id", Type: arrow.PrimitiveTypes.Int32, Nullable: true},
	{Name: "val", Type: arrow.PrimitiveTypes.Float64, Nullable: true},
	{Name: "txt", Type: arrow.BinaryTypes.String, Nullable: true},
	{Name: "dt", Type: arrow.FixedWidthTypes.Date32, Nullable: true},
}, nil)

// makeRecord builds n rows of the benchmark payload. The caller releases it.
func makeRecord(n int) arrow.Record {
	bld := array.NewRecordBuilder(memory.DefaultAllocator, benchSchema)
	defer bld.Release()
	ids := bld.Field(0).(*array.Int32Builder)
	vals := bld.Field(1).(*array.Float64Builder)
	txts := bld.Field(2).(*array.StringBuilder)
	dts := bld.Field(3).(*array.Date32Builder)
	ids.Reserve(n)
	vals.Reserve(n)
	txts.Reserve(n)
	dts.Reserve(n)
	for i := 0; i < n; i++ {
		ids.Append(int32(i))
		vals.Append(float64(i) * 0.5)
		txts.Append(fmt.Sprintf("row-%012d", i))
		dts.Append(arrow.Date32(i % 20000))
	}
	return bld.NewRecord()
}

// -------------------------------------------------------------------- ADBC

// adbcConnect opens a connection through libadbc_driver_odbc.so.
//
// adbc.odbc.delegate=never keeps the bridge on its own ODBC path instead of
// handing the connection to a database's native ADBC driver, so the numbers
// describe this driver.
func adbcConnect(driver, uri string, setup []string, autocommit bool) (adbc.Database, adbc.Connection, error) {
	var drv drivermgr.Driver
	db, err := drv.NewDatabase(map[string]string{
		"driver":             driver,
		"uri":                uri,
		"adbc.odbc.delegate": "never",
	})
	if err != nil {
		return nil, nil, err
	}
	cnxn, err := db.Open(ctx)
	if err != nil {
		db.Close()
		return nil, nil, err
	}
	for _, s := range setup {
		if err := adbcExec(cnxn, s); err != nil {
			cnxn.Close()
			db.Close()
			return nil, nil, err
		}
	}
	if !autocommit {
		opts, ok := cnxn.(adbc.PostInitOptions)
		if !ok {
			cnxn.Close()
			db.Close()
			return nil, nil, fmt.Errorf("the connection does not take options")
		}
		if err := opts.SetOption(adbc.OptionKeyAutoCommit, adbc.OptionValueDisabled); err != nil {
			cnxn.Close()
			db.Close()
			return nil, nil, err
		}
	}
	return db, cnxn, nil
}

// adbcExec runs a statement for its side effect, discarding any row count.
func adbcExec(cnxn adbc.Connection, sql string) error {
	stmt, err := cnxn.NewStatement()
	if err != nil {
		return err
	}
	defer stmt.Close()
	if err := stmt.SetSqlQuery(sql); err != nil {
		return err
	}
	_, err = stmt.ExecuteUpdate(ctx)
	return err
}

// dropTable drops every spelling of the name, ignoring failures: the table
// usually does not exist yet, and a case-folding database answers to only one
// of the spellings.
func dropTable(cnxn adbc.Connection, names []string, autocommit bool) {
	for _, name := range names {
		_ = adbcExec(cnxn, "DROP TABLE "+name)
		if !autocommit {
			// A failed DROP leaves e.g. PostgreSQL in an aborted transaction;
			// ending it here keeps the next statement from inheriting that.
			_ = cnxn.Commit(ctx)
		}
	}
}

// adbcIngest bulk-ingests rec into table in `create` mode, commits once, and
// returns the seconds that took (DDL + rows + commit, as matrix_bench.py).
func adbcIngest(cnxn adbc.Connection, table string, rec arrow.Record) (float64, error) {
	start := time.Now()
	stmt, err := cnxn.NewStatement()
	if err != nil {
		return 0, err
	}
	if err := stmt.SetOption(adbc.OptionKeyIngestTargetTable, table); err != nil {
		stmt.Close()
		return 0, err
	}
	if err := stmt.SetOption(adbc.OptionKeyIngestMode, adbc.OptionValueIngestModeCreate); err != nil {
		stmt.Close()
		return 0, err
	}
	if err := stmt.Bind(ctx, rec); err != nil {
		stmt.Close()
		return 0, err
	}
	if _, err := stmt.ExecuteUpdate(ctx); err != nil {
		stmt.Close()
		return 0, err
	}
	stmt.Close()
	if err := cnxn.Commit(ctx); err != nil {
		return 0, err
	}
	return time.Since(start).Seconds(), nil
}

// adbcFetch drains sql into Arrow records through the bridge and counts rows.
func adbcFetch(cnxn adbc.Connection, query string) (int, error) {
	stmt, err := cnxn.NewStatement()
	if err != nil {
		return 0, err
	}
	defer stmt.Close()
	if err := stmt.SetSqlQuery(query); err != nil {
		return 0, err
	}
	rdr, _, err := stmt.ExecuteQuery(ctx)
	if err != nil {
		return 0, err
	}
	defer rdr.Release()
	rows := 0
	for rdr.Next() {
		rows += int(rdr.Record().NumRows())
	}
	return rows, rdr.Err()
}

// adbcCount runs SELECT COUNT(*), tolerant of whatever integral Arrow type the
// database reports the count as.
func adbcCount(cnxn adbc.Connection, ident string) (int64, error) {
	stmt, err := cnxn.NewStatement()
	if err != nil {
		return 0, err
	}
	defer stmt.Close()
	if err := stmt.SetSqlQuery("SELECT COUNT(*) FROM " + ident); err != nil {
		return 0, err
	}
	rdr, _, err := stmt.ExecuteQuery(ctx)
	if err != nil {
		return 0, err
	}
	defer rdr.Release()
	for rdr.Next() {
		rec := rdr.Record()
		if rec.NumRows() == 0 {
			continue
		}
		switch col := rec.Column(0).(type) {
		case *array.Int64:
			return col.Value(0), nil
		case *array.Int32:
			return int64(col.Value(0)), nil
		case *array.Float64:
			return int64(col.Value(0)), nil
		case *array.String:
			return strconv.ParseInt(strings.TrimSpace(col.Value(0)), 10, 64)
		case *array.Decimal128:
			return col.Value(0).BigInt().Int64(), nil
		default:
			return 0, fmt.Errorf("unexpected COUNT(*) type %s", rec.Column(0).DataType())
		}
	}
	if rdr.Err() != nil {
		return 0, rdr.Err()
	}
	return 0, fmt.Errorf("COUNT(*) returned no rows")
}

// ------------------------------------------------------------- database/sql

// odbcConnect opens the same ODBC connection string through database/sql.
func odbcConnect(uri string, setup []string) (*sql.DB, error) {
	db, err := sql.Open("odbc", uri)
	if err != nil {
		return nil, err
	}
	// One connection: a pool would hide the per-statement cost being measured
	// and would leave the ingest's transaction on a different connection.
	db.SetMaxOpenConns(1)
	db.SetMaxIdleConns(1)
	if err := db.Ping(); err != nil {
		db.Close()
		return nil, err
	}
	for _, s := range setup {
		if _, err := db.Exec(s); err != nil {
			db.Close()
			return nil, err
		}
	}
	return db, nil
}

// odbcFetch reads the SELECT row by row through database/sql and counts rows.
// No Arrow: this is the plain-Go ODBC cost of the same read.
func odbcFetch(db *sql.DB, query string) (int, error) {
	rows, err := db.Query(query)
	if err != nil {
		return 0, err
	}
	defer rows.Close()
	var (
		id  sql.NullInt64
		val sql.NullFloat64
		txt sql.NullString
		dt  any
		n   int
	)
	for rows.Next() {
		if err := rows.Scan(&id, &val, &txt, &dt); err != nil {
			return 0, err
		}
		n++
	}
	return n, rows.Err()
}

// odbcIngest sends rows rows to ident with a prepared statement inside one
// transaction and returns the seconds that took; the table must already exist.
func odbcIngest(db *sql.DB, ident string, rows int) (float64, error) {
	epoch := time.Date(1970, 1, 1, 0, 0, 0, 0, time.UTC)
	start := time.Now()
	tx, err := db.Begin()
	if err != nil {
		return 0, err
	}
	stmt, err := tx.Prepare("INSERT INTO " + ident + " VALUES (?, ?, ?, ?)")
	if err != nil {
		tx.Rollback()
		return 0, err
	}
	for i := 0; i < rows; i++ {
		_, err := stmt.Exec(
			int32(i),
			float64(i)*0.5,
			fmt.Sprintf("row-%012d", i),
			epoch.AddDate(0, 0, i%20000),
		)
		if err != nil {
			stmt.Close()
			tx.Rollback()
			return 0, err
		}
	}
	stmt.Close()
	if err := tx.Commit(); err != nil {
		return 0, err
	}
	return time.Since(start).Seconds(), nil
}

// ------------------------------------------------------------- measurement

// repeat returns the median of reps timings after one warmup.
func repeat(reps int, once func() (float64, error)) (float64, error) {
	if _, err := once(); err != nil {
		return 0, err
	}
	times := make([]float64, 0, reps)
	for i := 0; i < reps; i++ {
		t, err := once()
		if err != nil {
			return 0, err
		}
		times = append(times, t)
	}
	sort.Float64s(times)
	return times[len(times)/2], nil
}

// timed times one call of body, which must report the row count it saw.
func timed(expected int, body func() (int, error)) (float64, error) {
	start := time.Now()
	got, err := body()
	secs := time.Since(start).Seconds()
	if err != nil {
		return 0, err
	}
	if got != expected {
		return 0, fmt.Errorf("read %d rows, expected %d", got, expected)
	}
	return secs, nil
}

// step is one measurement: its median seconds, or why it did not finish.
type step struct {
	secs float64
	err  string
}

// attempt keeps a failure - a panic included - from ending the run: every step
// reports independently.
func attempt(body func() (float64, error)) (out step) {
	defer func() {
		if r := recover(); r != nil {
			out = step{err: trim(fmt.Sprintf("panicked: %v", r))}
		}
	}()
	secs, err := body()
	if err != nil {
		return step{err: trim(err.Error())}
	}
	return step{secs: secs}
}

func trim(message string) string {
	line := strings.TrimSpace(strings.SplitN(message, "\n", 2)[0])
	if len(line) > 160 {
		line = line[:160]
	}
	return line
}

// ------------------------------------------------------------------ output

func rate(rows int, s step) (float64, bool) {
	if s.err == "" && s.secs > 0 {
		return float64(rows) / s.secs, true
	}
	return 0, false
}

// thousands renders 1234567.8 as "1,234,568".
func thousands(v float64) string {
	digits := strconv.FormatFloat(v, 'f', 0, 64)
	var out strings.Builder
	for i, c := range digits {
		if i > 0 && (len(digits)-i)%3 == 0 {
			out.WriteByte(',')
		}
		out.WriteRune(c)
	}
	return out.String()
}

func cell(rows int, s step) string {
	if v, ok := rate(rows, s); ok {
		return thousands(v)
	}
	return "—"
}

func jsonStep(rows int, s step) map[string]any {
	if s.err != "" {
		return map[string]any{"error": s.err}
	}
	return map[string]any{"secs": s.secs, "rate": float64(rows) / s.secs}
}

// -------------------------------------------------------------------- main

func die(format string, a ...any) {
	fmt.Fprintf(os.Stderr, "bench_go: "+format+"\n", a...)
	os.Exit(2)
}

func main() {
	rows := flag.Int("rows", 10000, "rows to ingest")
	fetchRows := flag.Int("fetch-rows", 100000, "rows to read back")
	reps := flag.Int("reps", 3, "timings to take the median of")
	flag.Parse()
	if flag.NArg() != 1 {
		die("expected a database name")
	}
	dbName := flag.Arg(0)
	prefix := strings.ToUpper(dbName)

	driver := os.Getenv("ADBC_ODBC_DRIVER")
	if driver == "" {
		die("ADBC_ODBC_DRIVER must name libadbc_driver_odbc.so")
	}
	uri := os.Getenv(prefix + "_CONN")
	if uri == "" {
		die("%s_CONN must hold the ODBC connection string", prefix)
	}

	// The bare table name handed to the ADBC ingest.
	table := envOr("ADBC_BENCH_TABLE", "adbc_bench_go") + os.Getenv("ADBC_MATRIX_SUFFIX")
	var setup []string
	for _, line := range strings.Split(os.Getenv(prefix+"_SETUP"), "\n") {
		if strings.TrimSpace(line) != "" {
			setup = append(setup, line)
		}
	}

	// Every spelling of that name later SQL might have to use. The bridge's
	// ingest quotes the table name with the database's own quote character, so
	// the quoted spelling is what usually answers; a case-folding database
	// (Oracle, Db2) can also answer to the upper-cased one, which conn.py
	// passes in <DB>_TABLE from the compat matrix's `ident` hook.
	candidates := dedup([]string{`"` + table + `"`, envOr(prefix+"_TABLE", table), table})

	// Ingest a single row and find out which spelling of the table and column
	// names reaches what that produced, so nothing downstream has to guess.
	ident, query := candidates[0], "SELECT id, val, txt, dt FROM "+candidates[0]
	probe := attempt(func() (float64, error) {
		db, cnxn, err := adbcConnect(driver, uri, setup, false)
		if err != nil {
			return 0, err
		}
		defer db.Close()
		defer cnxn.Close()
		dropTable(cnxn, candidates, false)
		rec := makeRecord(1)
		defer rec.Release()
		if _, err := adbcIngest(cnxn, table, rec); err != nil {
			return 0, err
		}
		found := ""
		for _, c := range candidates {
			if _, err := adbcCount(cnxn, c); err == nil {
				found = c
				break
			}
		}
		if found == "" {
			return 0, fmt.Errorf("ingested %s but no spelling of the name selects from it", table)
		}
		ident = found
		for _, sql := range []string{
			`SELECT "id", "val", "txt", "dt" FROM ` + found,
			"SELECT id, val, txt, dt FROM " + found,
		} {
			if _, err := adbcFetch(cnxn, sql); err == nil {
				query = sql
				return 0, nil
			}
		}
		return 0, fmt.Errorf("the benchmark table has no readable id/val/txt/dt columns")
	})
	if probe.err != "" {
		fmt.Fprintf(os.Stderr, "%s: %s\n", dbName, probe.err)
	}

	// 1. adbc ingest: drop, ingest -rows rows, commit, verify the count.
	adbcIngestStep := attempt(func() (float64, error) {
		db, cnxn, err := adbcConnect(driver, uri, setup, false)
		if err != nil {
			return 0, err
		}
		defer db.Close()
		defer cnxn.Close()
		secs, err := repeat(*reps, func() (float64, error) {
			dropTable(cnxn, candidates, false)
			rec := makeRecord(*rows)
			defer rec.Release()
			return adbcIngest(cnxn, table, rec)
		})
		if err != nil {
			return 0, err
		}
		return secs, verify(cnxn, ident, *rows)
	})

	// 2. database/sql ingest of the same rows into the table ADBC's DDL created.
	odbcIngestStep := attempt(func() (float64, error) {
		adbcDB, cnxn, err := adbcConnect(driver, uri, setup, false)
		if err != nil {
			return 0, err
		}
		defer adbcDB.Close()
		defer cnxn.Close()
		sqlDB, err := odbcConnect(uri, setup)
		if err != nil {
			return 0, err
		}
		defer sqlDB.Close()
		secs, err := repeat(*reps, func() (float64, error) {
			dropTable(cnxn, candidates, false)
			empty := makeRecord(0)
			defer empty.Release()
			if _, err := adbcIngest(cnxn, table, empty); err != nil {
				return 0, err
			}
			return odbcIngest(sqlDB, ident, *rows)
		})
		if err != nil {
			return 0, err
		}
		return secs, verify(cnxn, ident, *rows)
	})

	// Load the bigger table the two fetch steps read back.
	loaded := attempt(func() (float64, error) {
		db, cnxn, err := adbcConnect(driver, uri, setup, false)
		if err != nil {
			return 0, err
		}
		defer db.Close()
		defer cnxn.Close()
		dropTable(cnxn, candidates, false)
		rec := makeRecord(*fetchRows)
		defer rec.Release()
		if _, err := adbcIngest(cnxn, table, rec); err != nil {
			return 0, err
		}
		return 0, verify(cnxn, ident, *fetchRows)
	})

	adbcFetchStep, odbcFetchStep := step{err: loaded.err}, step{err: loaded.err}
	if loaded.err == "" {
		adbcFetchStep = attempt(func() (float64, error) {
			db, cnxn, err := adbcConnect(driver, uri, setup, true)
			if err != nil {
				return 0, err
			}
			defer db.Close()
			defer cnxn.Close()
			return repeat(*reps, func() (float64, error) {
				return timed(*fetchRows, func() (int, error) { return adbcFetch(cnxn, query) })
			})
		})
		odbcFetchStep = attempt(func() (float64, error) {
			sqlDB, err := odbcConnect(uri, setup)
			if err != nil {
				return 0, err
			}
			defer sqlDB.Close()
			return repeat(*reps, func() (float64, error) {
				return timed(*fetchRows, func() (int, error) { return odbcFetch(sqlDB, query) })
			})
		})
	}

	// Leave nothing of ours behind on a shared server.
	if db, cnxn, err := adbcConnect(driver, uri, setup, true); err == nil {
		dropTable(cnxn, candidates, true)
		cnxn.Close()
		db.Close()
	}

	line, _ := json.Marshal(map[string]any{
		"lang":        "go",
		"db":          dbName,
		"rows":        *rows,
		"fetch_rows":  *fetchRows,
		"reps":        *reps,
		"adbc_ingest": jsonStep(*rows, adbcIngestStep),
		"adbc_fetch":  jsonStep(*fetchRows, adbcFetchStep),
		"odbc_ingest": jsonStep(*rows, odbcIngestStep),
		"odbc_fetch":  jsonStep(*fetchRows, odbcFetchStep),
	})
	fmt.Println(string(line))
	fmt.Printf("| go | %s | %s | %s | %s | %s |\n",
		dbName,
		cell(*rows, adbcIngestStep),
		cell(*fetchRows, adbcFetchStep),
		cell(*rows, odbcIngestStep),
		cell(*fetchRows, odbcFetchStep))

	for _, s := range []struct {
		name string
		step step
	}{
		{"adbc ingest", adbcIngestStep},
		{"adbc fetch", adbcFetchStep},
		{"odbc ingest", odbcIngestStep},
		{"odbc fetch", odbcFetchStep},
	} {
		if s.step.err != "" {
			fmt.Fprintf(os.Stderr, "%s: %s: %s\n", dbName, s.name, s.step.err)
		}
	}
}

func verify(cnxn adbc.Connection, ident string, want int) error {
	got, err := adbcCount(cnxn, ident)
	if err != nil {
		return err
	}
	if got != int64(want) {
		return fmt.Errorf("wrong row count %d != %d", got, want)
	}
	return nil
}

func envOr(key, fallback string) string {
	if v := os.Getenv(key); v != "" {
		return v
	}
	return fallback
}

func dedup(in []string) []string {
	var out []string
	seen := map[string]bool{}
	for _, s := range in {
		if !seen[s] {
			seen[s] = true
			out = append(out, s)
		}
	}
	return out
}
