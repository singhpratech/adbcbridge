/* SPDX-License-Identifier: Apache-2.0 */
/*
 * ingres_ansi_shim.c -- expose the Ingres ODBC driver's ANSI entry points only.
 *
 * The Ingres 10.1 ODBC driver (libiiodbcdriver.1.so, built from the GPL
 * ingres-10.1.0-00 source kit) implements its wide entry points -- SQLConnectW,
 * SQLDriverConnectW, SQLExecDirectW and the rest -- against a *four-byte* SQLWCHAR
 * (the platform wchar_t, the DataDirect/iODBC convention).  unixODBC's SQLWCHAR is two
 * bytes (UCS-2LE), and it decides a driver is a Unicode driver purely by finding
 * SQLConnectW in it, after which every call an application makes is converted to UCS-2
 * and handed to those entry points.  The driver reads the first character and stops at
 * the following zero byte, so a connection string arrives as "D" and the connect fails
 *
 *     08004  786744  E_GC0138_GCN_NO_SERVER  User provided a server class as part of
 *            the database name (dbname/class), but no servers of that class ... are
 *            running in the target installation
 *
 * -- an empty vnode and database, leaving just the "/INGRES" server class.  The same
 * call made with a UTF-32 string connects, which is the proof of the width.
 *
 * The driver's *narrow* entry points are correct and complete.  This shim is a driver
 * library that re-exports only those, so unixODBC classifies it as an ANSI driver and
 * does the UCS-2 <-> UTF-8 conversion itself.  Each stub is a PC-relative indirect jump
 * to the real function, so it forwards any signature unchanged, arguments and return
 * value alike, and costs one jump.  x86-64 System V only.
 *
 * Build (see tests/compat/README.md):
 *     gcc -shared -fPIC -O2 -o libingres_ansi.so ingres_ansi_shim.c -ldl
 *
 * The real driver is found through INGRES_REAL_ODBC_DRIVER, or on the loader path as
 * libiiodbcdriver.1.so.  The symbol list is `nm -D` over the 10.1 driver, minus the
 * wide entry points.
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>

#define HIDDEN __attribute__((visibility("hidden")))

HIDDEN void *ii_SQLAllocConnect;
HIDDEN void *ii_SQLAllocEnv;
HIDDEN void *ii_SQLAllocHandle;
HIDDEN void *ii_SQLAllocStmt;
HIDDEN void *ii_SQLBindCol;
HIDDEN void *ii_SQLBindParameter;
HIDDEN void *ii_SQLBrowseConnect;
HIDDEN void *ii_SQLCancel;
HIDDEN void *ii_SQLCloseCursor;
HIDDEN void *ii_SQLColAttribute;
HIDDEN void *ii_SQLColumnPrivileges;
HIDDEN void *ii_SQLColumns;
HIDDEN void *ii_SQLConnect;
HIDDEN void *ii_SQLCopyDesc;
HIDDEN void *ii_SQLDescribeCol;
HIDDEN void *ii_SQLDescribeParam;
HIDDEN void *ii_SQLDisconnect;
HIDDEN void *ii_SQLDriverConnect;
HIDDEN void *ii_SQLEndTran;
HIDDEN void *ii_SQLError;
HIDDEN void *ii_SQLExecDirect;
HIDDEN void *ii_SQLExecute;
HIDDEN void *ii_SQLExtendedFetch;
HIDDEN void *ii_SQLFetch;
HIDDEN void *ii_SQLFetchScroll;
HIDDEN void *ii_SQLForeignKeys;
HIDDEN void *ii_SQLFreeConnect;
HIDDEN void *ii_SQLFreeEnv;
HIDDEN void *ii_SQLFreeHandle;
HIDDEN void *ii_SQLFreeStmt;
HIDDEN void *ii_SQLGetConnectAttr;
HIDDEN void *ii_SQLGetConnectOption;
HIDDEN void *ii_SQLGetCursorName;
HIDDEN void *ii_SQLGetData;
HIDDEN void *ii_SQLGetDescField;
HIDDEN void *ii_SQLGetDescRec;
HIDDEN void *ii_SQLGetDiagField;
HIDDEN void *ii_SQLGetDiagRec;
HIDDEN void *ii_SQLGetEnvAttr;
HIDDEN void *ii_SQLGetFunctions;
HIDDEN void *ii_SQLGetInfo;
HIDDEN void *ii_SQLGetStmtAttr;
HIDDEN void *ii_SQLGetStmtOption;
HIDDEN void *ii_SQLGetTypeInfo;
HIDDEN void *ii_SQLMoreResults;
HIDDEN void *ii_SQLNativeSql;
HIDDEN void *ii_SQLNumParams;
HIDDEN void *ii_SQLNumResultCols;
HIDDEN void *ii_SQLParamData;
HIDDEN void *ii_SQLParamOptions;
HIDDEN void *ii_SQLPrepare;
HIDDEN void *ii_SQLPrimaryKeys;
HIDDEN void *ii_SQLProcedureColumns;
HIDDEN void *ii_SQLProcedureColumns_Internal;
HIDDEN void *ii_SQLProcedures;
HIDDEN void *ii_SQLPutData;
HIDDEN void *ii_SQLRowCount;
HIDDEN void *ii_SQLSetConnectAttr;
HIDDEN void *ii_SQLSetConnectOption;
HIDDEN void *ii_SQLSetCursorName;
HIDDEN void *ii_SQLSetDescField;
HIDDEN void *ii_SQLSetDescRec;
HIDDEN void *ii_SQLSetEnvAttr;
HIDDEN void *ii_SQLSetPos;
HIDDEN void *ii_SQLSetScrollOptions;
HIDDEN void *ii_SQLSetStmtAttr;
HIDDEN void *ii_SQLSetStmtOption;
HIDDEN void *ii_SQLSpecialColumns;
HIDDEN void *ii_SQLStatistics;
HIDDEN void *ii_SQLTablePrivileges;
HIDDEN void *ii_SQLTables;
HIDDEN void *ii_SQLTransact;

__asm__(
".text\n"
".globl SQLAllocConnect\n.type SQLAllocConnect,@function\nSQLAllocConnect: jmp *ii_SQLAllocConnect(%rip)\n.size SQLAllocConnect,.-SQLAllocConnect\n"
".globl SQLAllocEnv\n.type SQLAllocEnv,@function\nSQLAllocEnv: jmp *ii_SQLAllocEnv(%rip)\n.size SQLAllocEnv,.-SQLAllocEnv\n"
".globl SQLAllocHandle\n.type SQLAllocHandle,@function\nSQLAllocHandle: jmp *ii_SQLAllocHandle(%rip)\n.size SQLAllocHandle,.-SQLAllocHandle\n"
".globl SQLAllocStmt\n.type SQLAllocStmt,@function\nSQLAllocStmt: jmp *ii_SQLAllocStmt(%rip)\n.size SQLAllocStmt,.-SQLAllocStmt\n"
".globl SQLBindCol\n.type SQLBindCol,@function\nSQLBindCol: jmp *ii_SQLBindCol(%rip)\n.size SQLBindCol,.-SQLBindCol\n"
".globl SQLBindParameter\n.type SQLBindParameter,@function\nSQLBindParameter: jmp *ii_SQLBindParameter(%rip)\n.size SQLBindParameter,.-SQLBindParameter\n"
".globl SQLBrowseConnect\n.type SQLBrowseConnect,@function\nSQLBrowseConnect: jmp *ii_SQLBrowseConnect(%rip)\n.size SQLBrowseConnect,.-SQLBrowseConnect\n"
".globl SQLCancel\n.type SQLCancel,@function\nSQLCancel: jmp *ii_SQLCancel(%rip)\n.size SQLCancel,.-SQLCancel\n"
".globl SQLCloseCursor\n.type SQLCloseCursor,@function\nSQLCloseCursor: jmp *ii_SQLCloseCursor(%rip)\n.size SQLCloseCursor,.-SQLCloseCursor\n"
".globl SQLColAttribute\n.type SQLColAttribute,@function\nSQLColAttribute: jmp *ii_SQLColAttribute(%rip)\n.size SQLColAttribute,.-SQLColAttribute\n"
".globl SQLColumnPrivileges\n.type SQLColumnPrivileges,@function\nSQLColumnPrivileges: jmp *ii_SQLColumnPrivileges(%rip)\n.size SQLColumnPrivileges,.-SQLColumnPrivileges\n"
".globl SQLColumns\n.type SQLColumns,@function\nSQLColumns: jmp *ii_SQLColumns(%rip)\n.size SQLColumns,.-SQLColumns\n"
".globl SQLConnect\n.type SQLConnect,@function\nSQLConnect: jmp *ii_SQLConnect(%rip)\n.size SQLConnect,.-SQLConnect\n"
".globl SQLCopyDesc\n.type SQLCopyDesc,@function\nSQLCopyDesc: jmp *ii_SQLCopyDesc(%rip)\n.size SQLCopyDesc,.-SQLCopyDesc\n"
".globl SQLDescribeCol\n.type SQLDescribeCol,@function\nSQLDescribeCol: jmp *ii_SQLDescribeCol(%rip)\n.size SQLDescribeCol,.-SQLDescribeCol\n"
".globl SQLDescribeParam\n.type SQLDescribeParam,@function\nSQLDescribeParam: jmp *ii_SQLDescribeParam(%rip)\n.size SQLDescribeParam,.-SQLDescribeParam\n"
".globl SQLDisconnect\n.type SQLDisconnect,@function\nSQLDisconnect: jmp *ii_SQLDisconnect(%rip)\n.size SQLDisconnect,.-SQLDisconnect\n"
".globl SQLDriverConnect\n.type SQLDriverConnect,@function\nSQLDriverConnect: jmp *ii_SQLDriverConnect(%rip)\n.size SQLDriverConnect,.-SQLDriverConnect\n"
".globl SQLEndTran\n.type SQLEndTran,@function\nSQLEndTran: jmp *ii_SQLEndTran(%rip)\n.size SQLEndTran,.-SQLEndTran\n"
".globl SQLError\n.type SQLError,@function\nSQLError: jmp *ii_SQLError(%rip)\n.size SQLError,.-SQLError\n"
".globl SQLExecDirect\n.type SQLExecDirect,@function\nSQLExecDirect: jmp *ii_SQLExecDirect(%rip)\n.size SQLExecDirect,.-SQLExecDirect\n"
".globl SQLExecute\n.type SQLExecute,@function\nSQLExecute: jmp *ii_SQLExecute(%rip)\n.size SQLExecute,.-SQLExecute\n"
".globl SQLExtendedFetch\n.type SQLExtendedFetch,@function\nSQLExtendedFetch: jmp *ii_SQLExtendedFetch(%rip)\n.size SQLExtendedFetch,.-SQLExtendedFetch\n"
".globl SQLFetch\n.type SQLFetch,@function\nSQLFetch: jmp *ii_SQLFetch(%rip)\n.size SQLFetch,.-SQLFetch\n"
".globl SQLFetchScroll\n.type SQLFetchScroll,@function\nSQLFetchScroll: jmp *ii_SQLFetchScroll(%rip)\n.size SQLFetchScroll,.-SQLFetchScroll\n"
".globl SQLForeignKeys\n.type SQLForeignKeys,@function\nSQLForeignKeys: jmp *ii_SQLForeignKeys(%rip)\n.size SQLForeignKeys,.-SQLForeignKeys\n"
".globl SQLFreeConnect\n.type SQLFreeConnect,@function\nSQLFreeConnect: jmp *ii_SQLFreeConnect(%rip)\n.size SQLFreeConnect,.-SQLFreeConnect\n"
".globl SQLFreeEnv\n.type SQLFreeEnv,@function\nSQLFreeEnv: jmp *ii_SQLFreeEnv(%rip)\n.size SQLFreeEnv,.-SQLFreeEnv\n"
".globl SQLFreeHandle\n.type SQLFreeHandle,@function\nSQLFreeHandle: jmp *ii_SQLFreeHandle(%rip)\n.size SQLFreeHandle,.-SQLFreeHandle\n"
".globl SQLFreeStmt\n.type SQLFreeStmt,@function\nSQLFreeStmt: jmp *ii_SQLFreeStmt(%rip)\n.size SQLFreeStmt,.-SQLFreeStmt\n"
".globl SQLGetConnectAttr\n.type SQLGetConnectAttr,@function\nSQLGetConnectAttr: jmp *ii_SQLGetConnectAttr(%rip)\n.size SQLGetConnectAttr,.-SQLGetConnectAttr\n"
".globl SQLGetConnectOption\n.type SQLGetConnectOption,@function\nSQLGetConnectOption: jmp *ii_SQLGetConnectOption(%rip)\n.size SQLGetConnectOption,.-SQLGetConnectOption\n"
".globl SQLGetCursorName\n.type SQLGetCursorName,@function\nSQLGetCursorName: jmp *ii_SQLGetCursorName(%rip)\n.size SQLGetCursorName,.-SQLGetCursorName\n"
".globl SQLGetData\n.type SQLGetData,@function\nSQLGetData: jmp *ii_SQLGetData(%rip)\n.size SQLGetData,.-SQLGetData\n"
".globl SQLGetDescField\n.type SQLGetDescField,@function\nSQLGetDescField: jmp *ii_SQLGetDescField(%rip)\n.size SQLGetDescField,.-SQLGetDescField\n"
".globl SQLGetDescRec\n.type SQLGetDescRec,@function\nSQLGetDescRec: jmp *ii_SQLGetDescRec(%rip)\n.size SQLGetDescRec,.-SQLGetDescRec\n"
".globl SQLGetDiagField\n.type SQLGetDiagField,@function\nSQLGetDiagField: jmp *ii_SQLGetDiagField(%rip)\n.size SQLGetDiagField,.-SQLGetDiagField\n"
".globl SQLGetDiagRec\n.type SQLGetDiagRec,@function\nSQLGetDiagRec: jmp *ii_SQLGetDiagRec(%rip)\n.size SQLGetDiagRec,.-SQLGetDiagRec\n"
".globl SQLGetEnvAttr\n.type SQLGetEnvAttr,@function\nSQLGetEnvAttr: jmp *ii_SQLGetEnvAttr(%rip)\n.size SQLGetEnvAttr,.-SQLGetEnvAttr\n"
".globl SQLGetFunctions\n.type SQLGetFunctions,@function\nSQLGetFunctions: jmp *ii_SQLGetFunctions(%rip)\n.size SQLGetFunctions,.-SQLGetFunctions\n"
".globl SQLGetInfo\n.type SQLGetInfo,@function\nSQLGetInfo: jmp *ii_SQLGetInfo(%rip)\n.size SQLGetInfo,.-SQLGetInfo\n"
".globl SQLGetStmtAttr\n.type SQLGetStmtAttr,@function\nSQLGetStmtAttr: jmp *ii_SQLGetStmtAttr(%rip)\n.size SQLGetStmtAttr,.-SQLGetStmtAttr\n"
".globl SQLGetStmtOption\n.type SQLGetStmtOption,@function\nSQLGetStmtOption: jmp *ii_SQLGetStmtOption(%rip)\n.size SQLGetStmtOption,.-SQLGetStmtOption\n"
".globl SQLGetTypeInfo\n.type SQLGetTypeInfo,@function\nSQLGetTypeInfo: jmp *ii_SQLGetTypeInfo(%rip)\n.size SQLGetTypeInfo,.-SQLGetTypeInfo\n"
".globl SQLMoreResults\n.type SQLMoreResults,@function\nSQLMoreResults: jmp *ii_SQLMoreResults(%rip)\n.size SQLMoreResults,.-SQLMoreResults\n"
".globl SQLNativeSql\n.type SQLNativeSql,@function\nSQLNativeSql: jmp *ii_SQLNativeSql(%rip)\n.size SQLNativeSql,.-SQLNativeSql\n"
".globl SQLNumParams\n.type SQLNumParams,@function\nSQLNumParams: jmp *ii_SQLNumParams(%rip)\n.size SQLNumParams,.-SQLNumParams\n"
".globl SQLNumResultCols\n.type SQLNumResultCols,@function\nSQLNumResultCols: jmp *ii_SQLNumResultCols(%rip)\n.size SQLNumResultCols,.-SQLNumResultCols\n"
".globl SQLParamData\n.type SQLParamData,@function\nSQLParamData: jmp *ii_SQLParamData(%rip)\n.size SQLParamData,.-SQLParamData\n"
".globl SQLParamOptions\n.type SQLParamOptions,@function\nSQLParamOptions: jmp *ii_SQLParamOptions(%rip)\n.size SQLParamOptions,.-SQLParamOptions\n"
".globl SQLPrepare\n.type SQLPrepare,@function\nSQLPrepare: jmp *ii_SQLPrepare(%rip)\n.size SQLPrepare,.-SQLPrepare\n"
".globl SQLPrimaryKeys\n.type SQLPrimaryKeys,@function\nSQLPrimaryKeys: jmp *ii_SQLPrimaryKeys(%rip)\n.size SQLPrimaryKeys,.-SQLPrimaryKeys\n"
".globl SQLProcedureColumns\n.type SQLProcedureColumns,@function\nSQLProcedureColumns: jmp *ii_SQLProcedureColumns(%rip)\n.size SQLProcedureColumns,.-SQLProcedureColumns\n"
".globl SQLProcedureColumns_Internal\n.type SQLProcedureColumns_Internal,@function\nSQLProcedureColumns_Internal: jmp *ii_SQLProcedureColumns_Internal(%rip)\n.size SQLProcedureColumns_Internal,.-SQLProcedureColumns_Internal\n"
".globl SQLProcedures\n.type SQLProcedures,@function\nSQLProcedures: jmp *ii_SQLProcedures(%rip)\n.size SQLProcedures,.-SQLProcedures\n"
".globl SQLPutData\n.type SQLPutData,@function\nSQLPutData: jmp *ii_SQLPutData(%rip)\n.size SQLPutData,.-SQLPutData\n"
".globl SQLRowCount\n.type SQLRowCount,@function\nSQLRowCount: jmp *ii_SQLRowCount(%rip)\n.size SQLRowCount,.-SQLRowCount\n"
".globl SQLSetConnectAttr\n.type SQLSetConnectAttr,@function\nSQLSetConnectAttr: jmp *ii_SQLSetConnectAttr(%rip)\n.size SQLSetConnectAttr,.-SQLSetConnectAttr\n"
".globl SQLSetConnectOption\n.type SQLSetConnectOption,@function\nSQLSetConnectOption: jmp *ii_SQLSetConnectOption(%rip)\n.size SQLSetConnectOption,.-SQLSetConnectOption\n"
".globl SQLSetCursorName\n.type SQLSetCursorName,@function\nSQLSetCursorName: jmp *ii_SQLSetCursorName(%rip)\n.size SQLSetCursorName,.-SQLSetCursorName\n"
".globl SQLSetDescField\n.type SQLSetDescField,@function\nSQLSetDescField: jmp *ii_SQLSetDescField(%rip)\n.size SQLSetDescField,.-SQLSetDescField\n"
".globl SQLSetDescRec\n.type SQLSetDescRec,@function\nSQLSetDescRec: jmp *ii_SQLSetDescRec(%rip)\n.size SQLSetDescRec,.-SQLSetDescRec\n"
".globl SQLSetEnvAttr\n.type SQLSetEnvAttr,@function\nSQLSetEnvAttr: jmp *ii_SQLSetEnvAttr(%rip)\n.size SQLSetEnvAttr,.-SQLSetEnvAttr\n"
".globl SQLSetPos\n.type SQLSetPos,@function\nSQLSetPos: jmp *ii_SQLSetPos(%rip)\n.size SQLSetPos,.-SQLSetPos\n"
".globl SQLSetScrollOptions\n.type SQLSetScrollOptions,@function\nSQLSetScrollOptions: jmp *ii_SQLSetScrollOptions(%rip)\n.size SQLSetScrollOptions,.-SQLSetScrollOptions\n"
".globl SQLSetStmtAttr\n.type SQLSetStmtAttr,@function\nSQLSetStmtAttr: jmp *ii_SQLSetStmtAttr(%rip)\n.size SQLSetStmtAttr,.-SQLSetStmtAttr\n"
".globl SQLSetStmtOption\n.type SQLSetStmtOption,@function\nSQLSetStmtOption: jmp *ii_SQLSetStmtOption(%rip)\n.size SQLSetStmtOption,.-SQLSetStmtOption\n"
".globl SQLSpecialColumns\n.type SQLSpecialColumns,@function\nSQLSpecialColumns: jmp *ii_SQLSpecialColumns(%rip)\n.size SQLSpecialColumns,.-SQLSpecialColumns\n"
".globl SQLStatistics\n.type SQLStatistics,@function\nSQLStatistics: jmp *ii_SQLStatistics(%rip)\n.size SQLStatistics,.-SQLStatistics\n"
".globl SQLTablePrivileges\n.type SQLTablePrivileges,@function\nSQLTablePrivileges: jmp *ii_SQLTablePrivileges(%rip)\n.size SQLTablePrivileges,.-SQLTablePrivileges\n"
".globl SQLTables\n.type SQLTables,@function\nSQLTables: jmp *ii_SQLTables(%rip)\n.size SQLTables,.-SQLTables\n"
".globl SQLTransact\n.type SQLTransact,@function\nSQLTransact: jmp *ii_SQLTransact(%rip)\n.size SQLTransact,.-SQLTransact\n"
);

__attribute__((constructor)) static void ingres_ansi_shim_init(void)
{
    const char *path = getenv("INGRES_REAL_ODBC_DRIVER");
    void *h;

    if (path == NULL || *path == '\0')
        path = "libiiodbcdriver.1.so";
    h = dlopen(path, RTLD_NOW | RTLD_GLOBAL);
    if (h == NULL) {
        fprintf(stderr, "ingres_ansi_shim: cannot load %s: %s\n", path, dlerror());
        return;
    }

    ii_SQLAllocConnect = dlsym(h, "SQLAllocConnect");
    ii_SQLAllocEnv = dlsym(h, "SQLAllocEnv");
    ii_SQLAllocHandle = dlsym(h, "SQLAllocHandle");
    ii_SQLAllocStmt = dlsym(h, "SQLAllocStmt");
    ii_SQLBindCol = dlsym(h, "SQLBindCol");
    ii_SQLBindParameter = dlsym(h, "SQLBindParameter");
    ii_SQLBrowseConnect = dlsym(h, "SQLBrowseConnect");
    ii_SQLCancel = dlsym(h, "SQLCancel");
    ii_SQLCloseCursor = dlsym(h, "SQLCloseCursor");
    ii_SQLColAttribute = dlsym(h, "SQLColAttribute");
    ii_SQLColumnPrivileges = dlsym(h, "SQLColumnPrivileges");
    ii_SQLColumns = dlsym(h, "SQLColumns");
    ii_SQLConnect = dlsym(h, "SQLConnect");
    ii_SQLCopyDesc = dlsym(h, "SQLCopyDesc");
    ii_SQLDescribeCol = dlsym(h, "SQLDescribeCol");
    ii_SQLDescribeParam = dlsym(h, "SQLDescribeParam");
    ii_SQLDisconnect = dlsym(h, "SQLDisconnect");
    ii_SQLDriverConnect = dlsym(h, "SQLDriverConnect");
    ii_SQLEndTran = dlsym(h, "SQLEndTran");
    ii_SQLError = dlsym(h, "SQLError");
    ii_SQLExecDirect = dlsym(h, "SQLExecDirect");
    ii_SQLExecute = dlsym(h, "SQLExecute");
    ii_SQLExtendedFetch = dlsym(h, "SQLExtendedFetch");
    ii_SQLFetch = dlsym(h, "SQLFetch");
    ii_SQLFetchScroll = dlsym(h, "SQLFetchScroll");
    ii_SQLForeignKeys = dlsym(h, "SQLForeignKeys");
    ii_SQLFreeConnect = dlsym(h, "SQLFreeConnect");
    ii_SQLFreeEnv = dlsym(h, "SQLFreeEnv");
    ii_SQLFreeHandle = dlsym(h, "SQLFreeHandle");
    ii_SQLFreeStmt = dlsym(h, "SQLFreeStmt");
    ii_SQLGetConnectAttr = dlsym(h, "SQLGetConnectAttr");
    ii_SQLGetConnectOption = dlsym(h, "SQLGetConnectOption");
    ii_SQLGetCursorName = dlsym(h, "SQLGetCursorName");
    ii_SQLGetData = dlsym(h, "SQLGetData");
    ii_SQLGetDescField = dlsym(h, "SQLGetDescField");
    ii_SQLGetDescRec = dlsym(h, "SQLGetDescRec");
    ii_SQLGetDiagField = dlsym(h, "SQLGetDiagField");
    ii_SQLGetDiagRec = dlsym(h, "SQLGetDiagRec");
    ii_SQLGetEnvAttr = dlsym(h, "SQLGetEnvAttr");
    ii_SQLGetFunctions = dlsym(h, "SQLGetFunctions");
    ii_SQLGetInfo = dlsym(h, "SQLGetInfo");
    ii_SQLGetStmtAttr = dlsym(h, "SQLGetStmtAttr");
    ii_SQLGetStmtOption = dlsym(h, "SQLGetStmtOption");
    ii_SQLGetTypeInfo = dlsym(h, "SQLGetTypeInfo");
    ii_SQLMoreResults = dlsym(h, "SQLMoreResults");
    ii_SQLNativeSql = dlsym(h, "SQLNativeSql");
    ii_SQLNumParams = dlsym(h, "SQLNumParams");
    ii_SQLNumResultCols = dlsym(h, "SQLNumResultCols");
    ii_SQLParamData = dlsym(h, "SQLParamData");
    ii_SQLParamOptions = dlsym(h, "SQLParamOptions");
    ii_SQLPrepare = dlsym(h, "SQLPrepare");
    ii_SQLPrimaryKeys = dlsym(h, "SQLPrimaryKeys");
    ii_SQLProcedureColumns = dlsym(h, "SQLProcedureColumns");
    ii_SQLProcedureColumns_Internal = dlsym(h, "SQLProcedureColumns_Internal");
    ii_SQLProcedures = dlsym(h, "SQLProcedures");
    ii_SQLPutData = dlsym(h, "SQLPutData");
    ii_SQLRowCount = dlsym(h, "SQLRowCount");
    ii_SQLSetConnectAttr = dlsym(h, "SQLSetConnectAttr");
    ii_SQLSetConnectOption = dlsym(h, "SQLSetConnectOption");
    ii_SQLSetCursorName = dlsym(h, "SQLSetCursorName");
    ii_SQLSetDescField = dlsym(h, "SQLSetDescField");
    ii_SQLSetDescRec = dlsym(h, "SQLSetDescRec");
    ii_SQLSetEnvAttr = dlsym(h, "SQLSetEnvAttr");
    ii_SQLSetPos = dlsym(h, "SQLSetPos");
    ii_SQLSetScrollOptions = dlsym(h, "SQLSetScrollOptions");
    ii_SQLSetStmtAttr = dlsym(h, "SQLSetStmtAttr");
    ii_SQLSetStmtOption = dlsym(h, "SQLSetStmtOption");
    ii_SQLSpecialColumns = dlsym(h, "SQLSpecialColumns");
    ii_SQLStatistics = dlsym(h, "SQLStatistics");
    ii_SQLTablePrivileges = dlsym(h, "SQLTablePrivileges");
    ii_SQLTables = dlsym(h, "SQLTables");
    ii_SQLTransact = dlsym(h, "SQLTransact");
}
