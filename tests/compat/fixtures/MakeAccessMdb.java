// Generates tests/compat/fixtures/access.mdb, the read-only fixture behind the
// Microsoft Access entry of the compatibility matrix.
//
// MDB Tools -- the only free ODBC driver for Access on Linux -- is read-only:
// it executes no DDL and no DML, so the fixture has to be produced out of band.
// It is *generated* here with Jackcess (Apache-2.0) rather than copied from
// anywhere, so the checked-in .mdb carries no third-party licence.
//
//   java -cp jackcess.jar:commons-lang3.jar:commons-logging.jar \
//        MakeAccessMdb.java tests/compat/fixtures/access.mdb
//
// See tests/compat/README.md for the exact download commands.
import com.healthmarketscience.jackcess.ColumnBuilder;
import com.healthmarketscience.jackcess.DataType;
import com.healthmarketscience.jackcess.Database;
import com.healthmarketscience.jackcess.DatabaseBuilder;
import com.healthmarketscience.jackcess.DateTimeType;
import com.healthmarketscience.jackcess.Table;
import com.healthmarketscience.jackcess.TableBuilder;

import java.io.File;
import java.math.BigDecimal;
import java.time.LocalDate;
import java.time.LocalDateTime;

public class MakeAccessMdb {
  public static void main(String[] args) throws Exception {
    File f = new File(args.length > 0 ? args[0] : "access.mdb");
    if (f.exists() && !f.delete()) throw new IllegalStateException("cannot delete " + f);
    // V2000 == Jet 4, the .mdb generation MDB Tools reads best.
    Database db = DatabaseBuilder.create(Database.FileFormat.V2000, f);
    db.setDateTimeType(DateTimeType.LOCAL_DATE_TIME);

    Table t = new TableBuilder("adbc_t")
        .addColumn(new ColumnBuilder("i", DataType.LONG))
        .addColumn(new ColumnBuilder("f", DataType.DOUBLE))
        .addColumn(new ColumnBuilder("s", DataType.TEXT).setLength(100))
        .addColumn(new ColumnBuilder("b", DataType.OLE))
        .addColumn(new ColumnBuilder("d", DataType.SHORT_DATE_TIME))
        .addColumn(new ColumnBuilder("ts", DataType.SHORT_DATE_TIME))
        .addColumn(new ColumnBuilder("n", DataType.NUMERIC).setPrecision(10).setScale(3))
        .addColumn(new ColumnBuilder("bo", DataType.BOOLEAN))
        .toTable(db);

    // The same ROW1/ROW2 the matrix INSERTs into every writable database.
    // Access DATETIME has no sub-second component, so ts loses the 123456 us.
    // Access YESNO cannot be NULL, so row 2's bo is FALSE rather than NULL.
    t.addRow(1, 1.5, "héllo 🚀", new byte[] {1, 2},
             LocalDate.of(2024, 2, 29).atStartOfDay(),
             LocalDateTime.of(2024, 2, 29, 13, 45, 10),
             new BigDecimal("12.345"), Boolean.TRUE);
    t.addRow(2, null, null, null, null, null, null, Boolean.FALSE);

    // A longer table so the read crosses the reader's 1024-row batch boundary, the way
    // the bulk-ingest step does for the writable databases. Kept to the matrix entry's
    // big_rows so the checked-in fixture stays small.
    int n = args.length > 1 ? Integer.parseInt(args[1]) : 3000;
    Table big = new TableBuilder("adbc_big")
        .addColumn(new ColumnBuilder("a", DataType.LONG))
        .addColumn(new ColumnBuilder("b", DataType.TEXT).setLength(50))
        .toTable(db);
    for (int i = 0; i < n; i++) big.addRow(i, "r" + i);

    db.close();
    System.out.println("wrote " + f + " (" + f.length() + " bytes)");
  }
}
