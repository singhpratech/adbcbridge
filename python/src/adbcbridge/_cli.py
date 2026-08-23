# Copyright 2026 the adbcbridge authors
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
#
# SPDX-License-Identifier: Apache-2.0

"""The ``adbcbridge`` command line tool."""

from __future__ import annotations

import argparse
import pathlib
import sys
from typing import List, Optional, Sequence

from . import __version__, connect
from ._locate import DriverNotFoundError, driver_path, odbc_drivers, odbcinst_ini


def _cmd_query(args: argparse.Namespace) -> int:
    params: Optional[List[str]] = args.param or None
    with connect(uri=args.connection_string, driver_path=args.driver_path,
                 autocommit=True) as conn:
        with conn.cursor() as cur:
            cur.execute(args.sql, parameters=params)
            if not cur.description:  # DDL/DML: no result set
                affected = cur.rowcount
                # ODBC reports -1 when a statement has no meaningful row count.
                print("OK" if affected is None or affected < 0
                      else "rows affected: %d" % affected)
                return 0
            table = cur.fetch_arrow_table()
    if args.limit is not None and table.num_rows > args.limit:
        table = table.slice(0, args.limit)
    if args.format == "csv":
        import pyarrow.csv

        pyarrow.csv.write_csv(table, sys.stdout.buffer)
    elif args.format == "schema":
        print(table.schema)
    else:
        print(table)
        print("%d rows x %d columns" % (table.num_rows, table.num_columns))
    return 0


def _cmd_drivers(args: argparse.Namespace) -> int:
    ini = odbcinst_ini()
    drivers = odbc_drivers()
    if not drivers:
        where = str(ini) if ini else "odbcinst -j / odbcinst.ini"
        print("no ODBC drivers registered (%s)" % where, file=sys.stderr)
        return 1
    print("# %s" % ini)
    width = max(len(d.name) for d in drivers)
    for driver in drivers:
        note = ""
        if driver.path and not pathlib.Path(driver.path).is_file():
            note = "  (missing)"
        print("%-*s  %s%s" % (width, driver.name, driver.path or "-", note))
        if args.verbose and driver.description:
            print("%-*s  %s" % (width, "", driver.description))
    return 0


def _cmd_driver_path(args: argparse.Namespace) -> int:
    print(driver_path())
    return 0


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog="adbcbridge",
        description="Query any ODBC data source through adbcbridge (ADBC/Arrow).",
    )
    parser.add_argument("--version", action="version", version="adbcbridge " + __version__)
    sub = parser.add_subparsers(dest="command", required=True)

    query = sub.add_parser(
        "query",
        help="run a SQL statement and print the resulting Arrow table",
        description="Run a SQL statement against an ODBC connection string.",
        epilog='example: adbcbridge query "Driver=SQLite3;Database=my.db;" '
               '"SELECT * FROM t"',
    )
    query.add_argument("connection_string", help="ODBC connection string (Driver=...;)")
    query.add_argument("sql", help="SQL to execute")
    query.add_argument("-p", "--param", action="append", metavar="VALUE",
                       help="value for a '?' parameter (repeat, in order); "
                            "always sent as a string")
    query.add_argument("--driver-path", metavar="PATH",
                       help="path to libadbc_driver_odbc.so (default: auto-detect)")
    query.add_argument("--limit", type=int, metavar="N", help="print at most N rows")
    query.add_argument("--format", choices=("table", "csv", "schema"), default="table",
                       help="output format (default: table)")
    query.set_defaults(func=_cmd_query)

    drivers = sub.add_parser(
        "drivers",
        help="list the ODBC drivers registered on this machine",
        description="List the ODBC drivers from odbcinst -j / odbcinst.ini.",
    )
    drivers.add_argument("-v", "--verbose", action="store_true",
                         help="also print each driver's description")
    drivers.set_defaults(func=_cmd_drivers)

    where = sub.add_parser(
        "driver-path",
        help="print the path of the adbcbridge shared library that would be used",
    )
    where.set_defaults(func=_cmd_driver_path)
    return parser


def main(argv: Optional[Sequence[str]] = None) -> int:
    args = build_parser().parse_args(argv)
    try:
        return args.func(args)
    except DriverNotFoundError as exc:
        print("adbcbridge: %s" % exc, file=sys.stderr)
        return 2
    except Exception as exc:  # database errors: a traceback helps nobody here
        print("adbcbridge: %s: %s" % (type(exc).__name__, exc), file=sys.stderr)
        return 1
