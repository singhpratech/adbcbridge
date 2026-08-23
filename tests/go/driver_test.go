// Smoke test: load libadbc_driver_odbc.so through the Go ADBC driver manager.
//
//   ADBC_ODBC_DRIVER=/abs/path/libadbc_driver_odbc.so \
//   SQLITE_ODBC_DRIVER=/abs/path/libsqlite3odbc.so go test ./...
package adbcbridge_test

import (
	"context"
	"os"
	"path/filepath"
	"strings"
	"testing"

	"github.com/apache/arrow-adbc/go/adbc"
	"github.com/apache/arrow-adbc/go/adbc/drivermgr"
	"github.com/apache/arrow-go/v18/arrow/array"
	"github.com/apache/arrow-go/v18/arrow/memory"
)

func TestSelectAndIngest(t *testing.T) {
	driverPath := os.Getenv("ADBC_ODBC_DRIVER")
	sqliteDriver := os.Getenv("SQLITE_ODBC_DRIVER")
	if driverPath == "" || sqliteDriver == "" {
		t.Skip("set ADBC_ODBC_DRIVER and SQLITE_ODBC_DRIVER")
	}
	ctx := context.Background()
	dbPath := filepath.Join(t.TempDir(), "go.db")
	var drv drivermgr.Driver
	db, err := drv.NewDatabase(map[string]string{
		"driver": driverPath,
		"uri":    "Driver=" + sqliteDriver + ";Database=" + dbPath + ";",
	})
	if err != nil {
		t.Fatal(err)
	}
	defer db.Close()
	cnxn, err := db.Open(ctx)
	if err != nil {
		t.Fatal(err)
	}
	defer cnxn.Close()

	stmt, err := cnxn.NewStatement()
	if err != nil {
		t.Fatal(err)
	}
	defer stmt.Close()

	exec := func(sql string) {
		if err := stmt.SetSqlQuery(sql); err != nil {
			t.Fatal(err)
		}
		if _, err := stmt.ExecuteUpdate(ctx); err != nil {
			t.Fatalf("%s: %v", sql, err)
		}
	}
	exec("CREATE TABLE t (i INTEGER, s TEXT)")
	exec("INSERT INTO t VALUES (1, 'héllo'), (2, NULL), (3, 'go')")

	if err := stmt.SetSqlQuery("SELECT i, s FROM t ORDER BY i"); err != nil {
		t.Fatal(err)
	}
	rdr, n, err := stmt.ExecuteQuery(ctx)
	if err != nil {
		t.Fatal(err)
	}
	defer rdr.Release()
	if n != -1 {
		t.Fatalf("rows affected should be -1 for a query, got %d", n)
	}
	total := int64(0)
	var seen []string
	for rdr.Next() {
		rec := rdr.Record()
		total += rec.NumRows()
		col := rec.Column(1).(*array.String)
		for i := 0; i < col.Len(); i++ {
			if col.IsNull(i) {
				seen = append(seen, "<null>")
			} else {
				// Value() aliases the C buffer; copy before the record is released.
				seen = append(seen, strings.Clone(col.Value(i)))
			}
		}
	}
	if rdr.Err() != nil {
		t.Fatal(rdr.Err())
	}
	if total != 3 || len(seen) != 3 || seen[0] != "héllo" || seen[1] != "<null>" || seen[2] != "go" {
		t.Fatalf("unexpected result: rows=%d values=%v", total, seen)
	}

	// Bulk ingest via Bind + ingest options.
	mem := memory.DefaultAllocator
	bld := array.NewInt64Builder(mem)
	defer bld.Release()
	bld.AppendValues([]int64{10, 20, 30}, nil)
	arr := bld.NewArray()
	defer arr.Release()
	schema := rdr.Schema() // reuse is fine: just need an arrow schema object type
	_ = schema
	rec := array.NewRecord(arrowSchema("x"), []arrow_Array{arr}, 3)
	defer rec.Release()
	ing, err := cnxn.NewStatement()
	if err != nil {
		t.Fatal(err)
	}
	defer ing.Close()
	if err := ing.SetOption(adbc.OptionKeyIngestTargetTable, "ingested"); err != nil {
		t.Fatal(err)
	}
	if err := ing.Bind(ctx, rec); err != nil {
		t.Fatal(err)
	}
	affected, err := ing.ExecuteUpdate(ctx)
	if err != nil {
		t.Fatal(err)
	}
	if affected != 3 {
		t.Fatalf("expected 3 ingested rows, got %d", affected)
	}
}
