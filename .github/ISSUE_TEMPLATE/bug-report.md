---
name: Bug report
about: adbcBridge itself does something wrong (wrong data, crash, error where the ODBC driver works)
title: "bug: <one line>"
labels: bug
---

**Version** (`pip show adbcbridge`, crate/NuGet/Maven version, or the commit):

**Driver and database** (name and version; driver manager and OS):

**Connection string** (secrets removed):

**Statement and the schema of the columns involved**:

**What happens** (the full ADBC error, or the wrong values, and what you expected):

**Does the same statement work through the driver directly** (pyodbc / isql)? Paste the result.

**Language and binding** (Python `adbc_driver_manager`, Rust crate, C#, Java, Go):
