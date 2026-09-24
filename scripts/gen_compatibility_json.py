#!/usr/bin/env python3
"""Emit docs/compatibility.json: the compatibility matrix as machine-readable data.

Everything here is derived, never hand-written.  Two sources:

* ``docs/COMPATIBILITY.md`` -- the per-OS result table (``entry | Linux | macOS arm64 |
  Windows x64``) and the human table above it (``Database | Wire / driver | Matrix |
  Notes``).
* ``tests/compat/test_matrix.py`` -- the ``DBS`` dict, which is what the harness actually
  runs, so the quirks and tolerances come from the code rather than from prose.

The script asserts the counts it publishes against the two tables, and fails rather than
emit a number that disagrees with its source.  That is the point of it: the per-OS totals
have been wrong in hand-written copy before.
"""
from __future__ import annotations

import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
MD = ROOT / "docs" / "COMPATIBILITY.md"
OUT = ROOT / "docs" / "compatibility.json"
OS_HEADER = "| entry | Linux | macOS arm64 | Windows x64 |"
HUMAN_HEADER = "| Database | Wire / driver | Matrix | Notes |"

#: entry id -> its row index in the "Database | Wire / driver | Matrix | Notes" table.
#:
#: Written out in full and deliberately, because neither position nor name matching is
#: safe here.  The two tables are in different orders, and a prefix match silently pairs
#: "db2" with "Db2 for i" and "ibmi" with "IBM Informix".  The script asserts this is a
#: bijection over the rows, so a row added to one table without the other fails the build.
ROW_OF: dict[str, int] = {
    "sqlite": 0,
    "duckdb": 1,
    "postgres": 2,
    "mariadb": 3,
    "columnstore": 4,
    "oracle": 8,
    "clickhouse": 11,
    "mssql": 7,
    "azuresqledge": 24,
    "mysql": 5,
    "tidb": 21,
    "dolt": 6,
    "databend": 23,
    "percona": 13,
    "matrixone": 37,
    "doris": 40,
    "oceanbase": 44,
    "greptimedb": 29,
    "starrocks": 38,
    "mongodbbi": 47,
    "db2": 9,
    "informix": 27,
    "monetdb": 20,
    "vertica": 43,
    "cockroachdb": 12,
    "yugabyte": 14,
    "timescaledb": 15,
    "citus": 16,
    "cloudberry": 41,
    "materialize": 34,
    "opengauss": 26,
    "cratedb": 17,
    "questdb": 18,
    "risingwave": 19,
    "spanner": 36,
    "firebird": 22,
    "virtuoso": 25,
    "flightsql": 28,
    "arcadedb": 33,
    "influxdb3": 30,
    "ignite": 35,
    "opensearch": 39,
    "ydb": 42,
    "dremio": 31,
    "tdengine": 46,
    "access": 32,
    "singlestore": 48,
    "hana": 45,
    "exasol": 49,
    "altibase": 50,
    "kinetica": 51,
    "ingres": 52,
    "ibmi": 10,
}


def tables(text: str) -> dict[str, list[list[str]]]:
    """Every markdown table in the file, keyed by its header line."""
    out: dict[str, list[list[str]]] = {}
    lines = text.splitlines()
    for i, line in enumerate(lines):
        if not line.startswith("| ") or i + 1 >= len(lines):
            continue
        if not lines[i + 1].startswith("|---"):
            continue
        rows = []
        for row in lines[i + 2:]:
            if not row.startswith("| "):
                break
            rows.append([c.strip() for c in row.strip().strip("|").split(" | ")])
        out[line] = rows
    return out


def verdict(cell: str) -> tuple[str, str | None]:
    """Classify a result cell and keep whatever else it says as the detail.

    The cells are prose as much as verdicts: "PASS (PostgreSQL 15.15)", "PASS (MariaDB
    11.8 arm64) -- after the maodbc quirk ...", "PASS with MySQL Connector/ODBC 26.7.1
    through a bridge built against iODBC", "driver unavailable: ... ships Linux and
    Windows assets only", "server not runnable here: ...".  Anything opening with PASS is
    a pass whatever follows it; the two "unavailable" forms are why an entry has no result
    on that platform rather than a failure of the driver, and are reported as such.
    """
    text = (cell or "").strip()
    bare = text.lstrip("*").strip()
    low = bare.lower()
    if re.match(r"pass\b", low):
        detail = bare[4:].strip().lstrip("(").strip()
        return ("pass", detail.rstrip(")").strip() or None)
    if re.match(r"fail\b", low):
        return ("fail", bare[4:].strip() or None)
    if low.startswith("driver unavailable"):
        return ("driver-unavailable", bare.split(":", 1)[-1].strip() or None)
    if low.startswith("server not runnable"):
        return ("server-unavailable", bare.split(":", 1)[-1].strip() or None)
    if low in ("", "-", "\u2014", "n/a"):
        return ("not-run", None)
    return ("other", bare or None)


def read_dbs() -> dict[str, dict]:
    """The harness's DBS dict, read with ast rather than imported.

    tests/compat/test_matrix.py imports pyarrow, which the version-agreement CI job does
    not have and should not need; parsing also avoids running a test module for its data.
    Entries are ``dict(k=v, ...)`` calls whose values are literals, so each keyword is
    literal_eval'd and anything that is not a literal is kept as its source text.
    """
    import ast

    tree = ast.parse((ROOT / "tests" / "compat" / "test_matrix.py").read_text(encoding="utf-8"))
    node = None
    for stmt in tree.body:
        targets = getattr(stmt, "targets", [])
        if targets and isinstance(targets[0], ast.Name) and targets[0].id == "DBS":
            node = stmt.value
            break
    if not isinstance(node, ast.Dict):
        sys.exit("tests/compat/test_matrix.py: could not find a DBS = {...} assignment")

    def value(v):
        try:
            return ast.literal_eval(v)
        except Exception:
            return ast.unparse(v)

    def entry(v) -> dict:
        if isinstance(v, ast.Dict):
            return {k.value: value(val) for k, val in zip(v.keys, v.values)
                    if isinstance(k, ast.Constant)}
        if isinstance(v, ast.Call) and isinstance(v.func, ast.Name) and v.func.id == "dict":
            return {kw.arg: value(kw.value) for kw in v.keywords if kw.arg}
        sys.exit(f"DBS holds an entry this script cannot read: {ast.unparse(v)[:60]}")

    out = {}
    for k, v in zip(node.keys, node.values):
        if not isinstance(k, ast.Constant):
            sys.exit("DBS has a non-literal key")
        out[k.value] = entry(v)
    return out


def main() -> int:
    text = MD.read_text(encoding="utf-8")
    tbl = tables(text)
    for header in (OS_HEADER, HUMAN_HEADER):
        if header not in tbl:
            sys.exit(f"{MD.name}: table not found: {header}")
    os_rows, human_rows = tbl[OS_HEADER], tbl[HUMAN_HEADER]
    if len(os_rows) != len(human_rows):
        sys.exit(f"the two tables disagree: {len(os_rows)} per-OS rows, {len(human_rows)} human rows")

    dbs = read_dbs()
    unknown = [r[0] for r in os_rows if r[0] not in dbs]
    if unknown:
        sys.exit(f"entries in the table that the harness does not define: {', '.join(unknown)}")
    if len(dbs) != len(os_rows):
        sys.exit(f"harness defines {len(dbs)} entries, the table lists {len(os_rows)}")

    # The two tables are NOT in the same order, so they are joined through ROW_OF rather
    # than by position or by name.  Getting this wrong is silent: zipping them published
    # QuestDB's results under Microsoft Access's name, and a prefix match paired "db2"
    # with "Db2 for i" and "ibmi" with "IBM Informix".
    if len(ROW_OF) != len(os_rows):
        sys.exit(f"ROW_OF maps {len(ROW_OF)} entries, the table lists {len(os_rows)}")
    if sorted(ROW_OF.values()) != list(range(len(human_rows))):
        sys.exit("ROW_OF is not a bijection over the human table's rows")
    for e in (r[0] for r in os_rows):
        if e not in ROW_OF:
            sys.exit(f"entry {e!r} has no ROW_OF mapping")

    # The harness carries values that are not JSON (tuples, callables); keep the ones that
    # describe behaviour and render them as data.  The connection plumbing is dropped: it
    # is local fixture detail, and a published connection string invites copy-paste.
    skip_keys = {"env", "conn", "db_kwargs", "fixture", "setup", "refresh", "extra"}

    def quirks(entry: dict) -> dict:
        out = {}
        for k, v in sorted(entry.items()):
            if k in skip_keys:
                continue
            if isinstance(v, (str, int, float, bool)) or v is None:
                out[k] = v
            elif isinstance(v, (tuple, list, set)):
                out[k] = sorted(str(x) for x in v)
            else:
                out[k] = str(v)
        return out

    databases = []
    for os_row in os_rows:
        entry = os_row[0]
        human_row = human_rows[ROW_OF[entry]]
        linux, linux_detail = verdict(os_row[1])
        mac, mac_detail = verdict(os_row[2])
        win, win_detail = verdict(os_row[3])
        databases.append({
            "entry": entry,
            "name": human_row[0],
            "driver": human_row[1],
            "results": {
                "linux": {"status": linux, "detail": linux_detail},
                "macos_arm64": {"status": mac, "detail": mac_detail},
                "windows_x64": {"status": win, "detail": win_detail},
            },
            "quirks": quirks(dbs[entry]),
            "notes": human_row[3] or None,
        })

    counts = {
        os_key: sum(1 for d in databases if d["results"][os_key]["status"] == "pass")
        for os_key in ("linux", "macos_arm64", "windows_x64")
    }
    counts["databases"] = len(databases)

    # The guard: the README and the site quote these, and they have drifted before.
    expected = {"databases": 53, "linux": 53, "macos_arm64": 45, "windows_x64": 48}
    if counts != {**counts, **expected} or any(counts[k] != v for k, v in expected.items()):
        sys.exit(
            "counted %s but this script expects %s -- if the matrix really changed, update "
            "the expectation here AND every place that quotes it (README.md, docs/index.md, "
            "the landing page)" % (counts, expected)
        )

    doc = {
        "$schema": "https://adbcbridge.org/compatibility.schema.json",
        "about": "Which databases adbcBridge is verified against, per operating system, with the "
                 "driver quirks each entry needs. Generated from docs/COMPATIBILITY.md and the "
                 "harness in tests/compat/test_matrix.py; do not edit by hand. Deliberately carries no\n                  generation date or commit: CI regenerates it and diffs, so the output has to be\n                  reproducible, and git records when it changed.",
        "counts": counts,
        "status_values": ["pass", "fail", "driver-unavailable", "server-unavailable", "not-run", "other"],
        "databases": databases,
    }
    OUT.write_text(json.dumps(doc, indent=2, sort_keys=False) + "\n", encoding="utf-8")
    print(f"wrote {OUT.relative_to(ROOT)}: {counts['databases']} databases, "
          f"linux {counts['linux']}, macOS {counts['macos_arm64']}, windows {counts['windows_x64']}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
