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

// Native delegation.  See odbc_delegate.h for the contract.

#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE
#endif

#include "odbc_delegate.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if !defined(_WIN32)
#include <dlfcn.h>
#include <pthread.h>
#include <sys/stat.h>
#include <unistd.h>
#endif
#if defined(__linux__) || defined(__FreeBSD__)
#include <link.h>
#endif
#if defined(__linux__) && defined(__GLIBC__)
#include <sys/auxv.h>
#endif
#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

#include "utils.h"

#if defined(_WIN32)
#define ADBC_ODBC_PATH_SEP ';'
#else
#define ADBC_ODBC_PATH_SEP ':'
#endif

// ---------------------------------------------------------------------------
// Small string helpers

static char* DupString(const char* s) {
  return s ? strdup(s) : NULL;
}

static void ReplaceString(char** dst, const char* value) {
  char* copy = DupString(value);
  free(*dst);
  *dst = copy;
}

/// Case-insensitive substring search.
static bool ContainsNoCase(const char* haystack, const char* needle) {
  if (!haystack || !needle) return false;
  size_t n = strlen(needle);
  if (n == 0) return true;
  for (const char* p = haystack; *p; p++) {
    size_t i = 0;
    while (i < n && p[i] && tolower((unsigned char)p[i]) == tolower((unsigned char)needle[i])) {
      i++;
    }
    if (i == n) return true;
  }
  return false;
}

static bool EqualsNoCase(const char* a, const char* b) {
  if (!a || !b) return false;
  while (*a && *b) {
    if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return false;
    a++;
    b++;
  }
  return *a == *b;
}

/// Is `value` in a NULL-terminated list of names (case-insensitively)?
static bool InNameList(const char* const* names, const char* value) {
  for (size_t i = 0; names[i]; i++) {
    if (EqualsNoCase(names[i], value)) return true;
  }
  return false;
}

/// A deduplicating list of owned strings.
struct StrList {
  char** items;
  size_t count;
  size_t capacity;
};

// Nothing fills a StrList on Windows: neither the driver search nor the
// odbcinst.ini enumeration behind it exists there.
#if !defined(_WIN32)
static void StrListAdd(struct StrList* list, const char* value) {
  if (!value || !*value) return;
  for (size_t i = 0; i < list->count; i++) {
    if (strcmp(list->items[i], value) == 0) return;
  }
  if (list->count == list->capacity) {
    size_t capacity = list->capacity ? list->capacity * 2 : 8;
    char** items = realloc(list->items, capacity * sizeof(char*));
    if (!items) return;
    list->items = items;
    list->capacity = capacity;
  }
  char* copy = strdup(value);
  if (!copy) return;
  list->items[list->count++] = copy;
}
#endif  // !_WIN32

static void StrListFree(struct StrList* list) {
  for (size_t i = 0; i < list->count; i++) free(list->items[i]);
  free(list->items);
  memset(list, 0, sizeof(*list));
}

// Everything from here to LooksLikePath is used only when a native driver can
// actually be loaded, which is the POSIX path: on Windows OdbcDelegateTryInit
// gives up before the search begins (see the note there).
#if !defined(_WIN32)

/// Split a path list ("a:b:c") into a StrList.
static void StrListAddPathList(struct StrList* list, const char* path_list) {
  if (!path_list) return;
  const char* p = path_list;
  while (*p) {
    const char* end = strchr(p, ADBC_ODBC_PATH_SEP);
    size_t len = end ? (size_t)(end - p) : strlen(p);
    if (len > 0 && len < 4096) {
      char buf[4096];
      memcpy(buf, p, len);
      buf[len] = '\0';
      StrListAdd(list, buf);
    }
    if (!end) break;
    p = end + 1;
  }
}

/// Directory part of a path (without the trailing separator), or NULL.
static char* DirName(const char* path) {
  const char* slash = strrchr(path, '/');
#if defined(_WIN32)
  const char* back = strrchr(path, '\\');
  if (back && (!slash || back > slash)) slash = back;
#endif
  if (!slash || slash == path) return NULL;
  size_t len = (size_t)(slash - path);
  char* out = malloc(len + 1);
  if (!out) return NULL;
  memcpy(out, path, len);
  out[len] = '\0';
  return out;
}

static bool FileExists(const char* path) {
#if defined(_WIN32)
  FILE* f = fopen(path, "rb");
  if (!f) return false;
  fclose(f);
  return true;
#else
  return access(path, R_OK) == 0;
#endif
}

#endif  // !_WIN32

/// Does this option value name a filesystem path or a manifest rather than a
/// bare driver name?  Bare names are alphanumerics, '_' and '-' only.
static bool LooksLikePath(const char* value) {
  if (!value || !*value) return false;
  for (const char* p = value; *p; p++) {
    if (!(isalnum((unsigned char)*p) || *p == '_' || *p == '-')) return true;
  }
  return false;
}

#if !defined(_WIN32)
/// Would loading this file be loading somebody else's code?  Used for the
/// candidates adbcbridge *derives* itself (the directories of already-loaded
/// ADBC objects): those are not something the caller asked for by name, so a
/// library that a third party could have put there is skipped rather than
/// dlopen()ed.  A path the caller named explicitly is their decision.
static bool PathIsTrusted(const char* path) {
#if defined(_WIN32)
  (void)path;
  return true;
#else
  uid_t me = geteuid();
  struct stat st;
  if (stat(path, &st) != 0) return false;
  if ((st.st_mode & S_IWOTH) != 0) return false;
  if (st.st_uid != 0 && st.st_uid != me) return false;
  char* dir = DirName(path);
  if (!dir) return true;
  bool ok = stat(dir, &st) == 0 && (st.st_mode & S_IWOTH) == 0 &&
            (st.st_uid == 0 || st.st_uid == me);
  free(dir);
  return ok;
#endif
}

#endif  // !_WIN32

/// True for a process that must not trust the environment (setuid/setgid).
static bool IsSecureExec(void) {
#if defined(_WIN32)
  return false;
#elif defined(__linux__) && defined(__GLIBC__)
  return getauxval(AT_SECURE) != 0;
#else
  return geteuid() != getuid() || getegid() != getgid();
#endif
}

// ---------------------------------------------------------------------------
// ODBC connection string / odbc.ini keyword lookup

/// An iterator over "Key=Value;Key2={Va;lue};" pairs.
struct ConnStringIter {
  const char* p;
};

/// Next keyword/value pair; both malloc'd.  Returns false at the end.
/// Handles brace-quoted values including the "}}" escape for a literal '}'.
static bool ConnStringNext(struct ConnStringIter* it, char** out_key, char** out_value) {
  const char* p = it->p;
  if (!p) return false;
  while (*p) {
    while (*p == ';' || *p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
    if (!*p) break;
    const char* eq = strchr(p, '=');
    if (!eq) break;
    const char* kend = eq;
    while (kend > p && (kend[-1] == ' ' || kend[-1] == '\t')) kend--;
    size_t keylen = (size_t)(kend - p);
    char* key = malloc(keylen + 1);
    if (!key) break;
    memcpy(key, p, keylen);
    key[keylen] = '\0';

    const char* v = eq + 1;
    char* value = NULL;
    if (*v == '{') {
      v++;
      // Copy up to the closing brace, turning "}}" into a literal '}'.
      size_t capacity = strlen(v) + 1;
      value = malloc(capacity);
      if (!value) {
        free(key);
        break;
      }
      size_t n = 0;
      while (*v) {
        if (*v == '}') {
          if (v[1] == '}') {
            value[n++] = '}';
            v += 2;
            continue;
          }
          v++;
          break;
        }
        value[n++] = *v++;
      }
      value[n] = '\0';
      // The rest of this attribute (up to the next ';') is not part of the value.
      while (*v && *v != ';') v++;
      p = v;
    } else {
      const char* vend = strchr(v, ';');
      if (!vend) vend = v + strlen(v);
      size_t len = (size_t)(vend - v);
      value = malloc(len + 1);
      if (!value) {
        free(key);
        break;
      }
      memcpy(value, v, len);
      value[len] = '\0';
      p = vend;
    }
    it->p = p;
    *out_key = key;
    *out_value = value;
    return true;
  }
  it->p = p;
  return false;
}

/// Fetch `key` from an ODBC connection string.  Returns a malloc'd string or NULL.
static char* ConnStringGet(const char* conn, const char* key) {
  if (!conn || !key) return NULL;
  struct ConnStringIter it = {conn};
  char* k = NULL;
  char* v = NULL;
  while (ConnStringNext(&it, &k, &v)) {
    if (EqualsNoCase(k, key)) {
      free(k);
      return v;
    }
    free(k);
    free(v);
  }
  return NULL;
}

#if !defined(_WIN32)
typedef int (*SQLGetPrivateProfileStringFn)(const char* section, const char* entry, const char* def,
                                            char* buffer, int buflen, const char* filename);

static SQLGetPrivateProfileStringFn g_profile_fn = NULL;
static pthread_once_t g_profile_once = PTHREAD_ONCE_INIT;

static void ProfileStringResolve(void) {
  SQLGetPrivateProfileStringFn fn =
      (SQLGetPrivateProfileStringFn)dlsym(RTLD_DEFAULT, "SQLGetPrivateProfileString");
  if (fn) {
    g_profile_fn = fn;
    return;
  }
  // unixODBC and iODBC, including the Homebrew/MacPorts prefixes that a bare
  // dlopen() does not search on macOS.
  static const char* kLibs[] = {
      "libodbcinst.so.2",
      "libodbcinst.so",
      "libodbcinst.2.dylib",
      "libodbcinst.dylib",
      "libiodbcinst.2.dylib",
      "libiodbcinst.dylib",
      "libiodbcinst.so.2",
      "libiodbcinst.so",
      "/opt/homebrew/lib/libodbcinst.2.dylib",
      "/opt/homebrew/lib/libodbcinst.dylib",
      "/usr/local/lib/libodbcinst.2.dylib",
      "/usr/local/lib/libodbcinst.dylib",
      "/opt/homebrew/lib/libiodbcinst.2.dylib",
      "/opt/homebrew/lib/libiodbcinst.dylib",
      "/usr/local/lib/libiodbcinst.2.dylib",
      "/usr/local/lib/libiodbcinst.dylib",
      "/usr/lib/libiodbcinst.dylib",
  };
  for (size_t i = 0; i < sizeof(kLibs) / sizeof(kLibs[0]); i++) {
    void* handle = dlopen(kLibs[i], RTLD_LAZY | RTLD_LOCAL);
    if (!handle) continue;
    fn = (SQLGetPrivateProfileStringFn)dlsym(handle, "SQLGetPrivateProfileString");
    if (fn) {
      // Deliberately kept open: the pointer stays valid for the process.
      g_profile_fn = fn;
      return;
    }
    dlclose(handle);
  }
}

/// unixODBC/iODBC's odbc.ini reader, resolved lazily (and exactly once) so that
/// we do not have to link libodbcinst.
static SQLGetPrivateProfileStringFn ProfileStringFn(void) {
  pthread_once(&g_profile_once, ProfileStringResolve);
  return g_profile_fn;
}
#endif

/// Fetch `key` from the [dsn] section of odbc.ini.  Returns malloc'd or NULL.
static char* DsnGet(const char* dsn, const char* key) {
#if defined(_WIN32)
  (void)dsn;
  (void)key;
  return NULL;
#else
  if (!dsn || !*dsn) return NULL;
  SQLGetPrivateProfileStringFn fn = ProfileStringFn();
  if (!fn) return NULL;
  char buf[1024] = {0};
  int n = fn(dsn, key, "", buf, (int)sizeof(buf), "odbc.ini");
  if (n <= 0 || !buf[0]) return NULL;
  return strdup(buf);
#endif
}

/// The entry names of the [dsn] section of odbc.ini (or the section names of
/// odbcinst.ini when `section` is NULL).  False when the driver manager cannot
/// enumerate them, which is the one case where we cannot vouch for a DSN.
static bool ProfileNames(const char* section, const char* filename, struct StrList* out) {
#if defined(_WIN32)
  (void)section;
  (void)filename;
  (void)out;
  return false;
#else
  SQLGetPrivateProfileStringFn fn = ProfileStringFn();
  if (!fn) return false;
  char buf[16384];
  memset(buf, 0, sizeof(buf));
  int n = fn(section, NULL, "", buf, (int)sizeof(buf), filename);
  if (n <= 0) return false;
  // A run of NUL-terminated names, ending with an empty one.
  const char* p = buf;
  const char* end = buf + sizeof(buf);
  while (p < end && *p) {
    StrListAdd(out, p);
    p += strlen(p) + 1;
  }
  return true;
#endif
}

/// Look a keyword up in the connection string first, then in the DSN section.
static char* KeywordGet(const char* conn, const char* dsn, const char* const* aliases) {
  for (size_t i = 0; aliases[i]; i++) {
    char* v = ConnStringGet(conn, aliases[i]);
    if (v && *v) return v;
    free(v);
  }
  for (size_t i = 0; aliases[i]; i++) {
    char* v = DsnGet(dsn, aliases[i]);
    if (v && *v) return v;
    free(v);
  }
  return NULL;
}

// ---------------------------------------------------------------------------
// Target detection

enum OdbcNativeFamily {
  FAMILY_NONE = 0,
  FAMILY_POSTGRESQL,
  FAMILY_SQLITE,
  FAMILY_DUCKDB,
  FAMILY_SNOWFLAKE,
  FAMILY_BIGQUERY,
  FAMILY_FLIGHTSQL,
};

static const char* FamilyDriverName(enum OdbcNativeFamily family) {
  switch (family) {
    case FAMILY_POSTGRESQL: return "postgresql";
    case FAMILY_SQLITE: return "sqlite";
    case FAMILY_DUCKDB: return "duckdb";
    case FAMILY_SNOWFLAKE: return "snowflake";
    case FAMILY_BIGQUERY: return "bigquery";
    case FAMILY_FLIGHTSQL: return "flightsql";
    default: return NULL;
  }
}

/// Guess the family from a native driver name/path ("postgresql",
/// "/opt/libadbc_driver_postgresql.so", ...).
static enum OdbcNativeFamily FamilyFromDriverName(const char* name) {
  if (ContainsNoCase(name, "postgres")) return FAMILY_POSTGRESQL;
  if (ContainsNoCase(name, "sqlite")) return FAMILY_SQLITE;
  if (ContainsNoCase(name, "duckdb")) return FAMILY_DUCKDB;
  if (ContainsNoCase(name, "snowflake")) return FAMILY_SNOWFLAKE;
  if (ContainsNoCase(name, "bigquery")) return FAMILY_BIGQUERY;
  if (ContainsNoCase(name, "flightsql") || ContainsNoCase(name, "flight_sql")) {
    return FAMILY_FLIGHTSQL;
  }
  return FAMILY_NONE;
}

/// Map the ODBC `Driver=` keyword (a filename or a name from odbcinst.ini).
static enum OdbcNativeFamily FamilyFromOdbcDriver(const char* driver) {
  if (!driver || !*driver) return FAMILY_NONE;
  if (ContainsNoCase(driver, "psqlodbc") || ContainsNoCase(driver, "postgres")) {
    return FAMILY_POSTGRESQL;
  }
  if (ContainsNoCase(driver, "sqlite")) return FAMILY_SQLITE;
  if (ContainsNoCase(driver, "duckdb")) return FAMILY_DUCKDB;
  if (ContainsNoCase(driver, "snowflake")) return FAMILY_SNOWFLAKE;
  if (ContainsNoCase(driver, "bigquery")) return FAMILY_BIGQUERY;
  return FAMILY_NONE;
}

/// Split "scheme:rest".  Returns the family and sets *rest past the scheme when
/// the value looks like a native ADBC URI rather than an ODBC connection string.
static enum OdbcNativeFamily FamilyFromUri(const char* uri, const char** rest) {
  if (!uri) return FAMILY_NONE;
  const char* p = uri;
  while (*p && (isalnum((unsigned char)*p) || *p == '+' || *p == '.' || *p == '-')) p++;
  if (*p != ':' || p == uri) return FAMILY_NONE;
  size_t len = (size_t)(p - uri);
  if (len >= 32) return FAMILY_NONE;
  char scheme[32];
  memcpy(scheme, uri, len);
  scheme[len] = '\0';
  *rest = p + 1;
  if (EqualsNoCase(scheme, "postgresql") || EqualsNoCase(scheme, "postgres")) {
    return FAMILY_POSTGRESQL;
  }
  if (EqualsNoCase(scheme, "sqlite") || EqualsNoCase(scheme, "sqlite3")) { return FAMILY_SQLITE; }
  if (EqualsNoCase(scheme, "duckdb")) return FAMILY_DUCKDB;
  if (EqualsNoCase(scheme, "snowflake")) return FAMILY_SNOWFLAKE;
  if (EqualsNoCase(scheme, "bigquery")) return FAMILY_BIGQUERY;
  if (EqualsNoCase(scheme, "grpc") || EqualsNoCase(scheme, "grpc+tls") ||
      EqualsNoCase(scheme, "grpc+tcp") || EqualsNoCase(scheme, "grpc+unix")) {
    return FAMILY_FLIGHTSQL;
  }
  return FAMILY_NONE;
}

bool OdbcDelegateIsNativeUri(const char* value) {
  const char* rest = NULL;
  return FamilyFromUri(value, &rest) != FAMILY_NONE;
}

// ---------------------------------------------------------------------------
// Percent-encoding

static void AppendEncoded(struct InternalAdbcStringBuilder* sb, const char* value) {
  static const char kHex[] = "0123456789ABCDEF";
  for (const unsigned char* p = (const unsigned char*)value; *p; p++) {
    if (isalnum(*p) || *p == '-' || *p == '.' || *p == '_' || *p == '~') {
      InternalAdbcStringBuilderAppend(sb, "%c", (char)*p);
    } else {
      InternalAdbcStringBuilderAppend(sb, "%%%c%c", kHex[*p >> 4], kHex[*p & 0x0F]);
    }
  }
}

static int HexValue(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

/// Percent-decode `len` bytes.  Returns a malloc'd string.
static char* PercentDecode(const char* value, size_t len) {
  char* out = malloc(len + 1);
  if (!out) return NULL;
  size_t n = 0;
  for (size_t i = 0; i < len; i++) {
    if (value[i] == '%' && i + 2 < len) {
      int hi = HexValue(value[i + 1]);
      int lo = HexValue(value[i + 2]);
      if (hi >= 0 && lo >= 0) {
        out[n++] = (char)((hi << 4) | lo);
        i += 2;
        continue;
      }
    }
    out[n++] = value[i];
  }
  out[n] = '\0';
  return out;
}

// ---------------------------------------------------------------------------
// Which ODBC keywords may be dropped, translated, or must stop delegation
//
// Rebuilding a native URI from an ODBC connection string silently changes what
// the connection *is* unless every keyword is accounted for.  Dropping
// "SSLmode=verify-full" would downgrade a verified TLS session to libpq's
// default (sslmode=prefer, no certificate check), so anything that is not
// either consumed by the URI builder, forwarded to the native driver, or known
// to be cosmetic stops delegation instead.

enum KeywordClass {
  KW_UNKNOWN = 0,  // we cannot represent it: do not delegate
  KW_CONSUMED,     // the URI builder reads it (Server, Database, Uid, ...)
  KW_IGNORED,      // no effect on the session (client-side cosmetics, DSN bookkeeping)
  KW_FORWARD,      // passed to the native driver as a URI parameter
};

/// Keywords the URI builder reads, for every family.
static const char* const kConsumedKeys[] = {"Driver",   "DSN",      "Server",   "Servername",
                                            "Host",     "Hostname", "Port",     "Database",
                                            "DB",       "Dbname",   "Data Source", "Uid",
                                            "User",     "Username", "Pwd",      "Password",
                                            NULL};

/// Bookkeeping written into odbc.ini/odbcinst.ini by the driver manager and its
/// setup dialogs; none of it reaches the server.
static const char* const kIgnoredKeys[] = {"Description", "Trace",      "TraceFile", "TraceLib",
                                           "Setup",       "UsageCount", "Threading", "FileUsage",
                                           "CPTimeout",   "CPReuse",    "DriverODBCVer",
                                           "SaveFile",    "Driver_Name", NULL};

/// psqlodbc keywords that only steer client-side behaviour of the ODBC driver
/// itself (result buffering, type reporting, logging).  Everything else --
/// TLS/GSS settings, ReadOnly, ConnSettings, ... -- is either forwarded or
/// stops delegation.
static const char* const kPgIgnoredKeys[] = {
    "UseDeclareFetch", "Fetch", "Protocol", "BoolsAsChar", "Debug", "CommLog", "Optimizer",
    "Ksqo", "UniqueIndex", "UnknownSizes", "TextAsLongVarchar", "UnknownsAsLongVarchar",
    "MaxVarcharSize", "MaxLongVarcharSize", "ExtraSysTablePrefixes", "LFConversion",
    "UpdatableCursors", "TrueIsMinus1", "ByteaAsLongVarBinary", "UseServerSidePrepare",
    "LowerCaseIdentifier", "RowVersioning", "ShowSystemTables", "ShowOidColumn",
    "FakeOidIndex", "DisallowPremature", "Parse", "CancelAsFreeStmt", "XaOpt",
    NULL};

/// psqlodbc keywords that carry a libpq setting: forwarded verbatim as URI
/// query parameters, under the same name libpq uses.
static const char* const kPgForwardKeys[] = {
    "sslmode", "sslrootcert", "sslcert", "sslkey", "sslcrl", "sslcompression", "sslsni",
    "gssencmode", "channel_binding", "krbsrvname", "pgkrbsrvname", "connect_timeout",
    "application_name", "options", "target_session_attrs", "require_auth", "load_balance_hosts",
    NULL};

/// libpq keywords accepted inside psqlodbc's `pqopt={...}` block.
static const char* const kPqoptKeys[] = {
    "sslmode", "sslrootcert", "sslcert", "sslkey", "sslcrl", "sslcompression", "sslsni",
    "sslpassword", "gssencmode", "channel_binding", "krbsrvname", "connect_timeout",
    "application_name", "options", "target_session_attrs", "require_auth", "keepalives",
    "keepalives_idle", "keepalives_interval", "keepalives_count", "load_balance_hosts",
    "client_encoding", "fallback_application_name", "hostaddr", "service", "passfile",
    NULL};

/// sqliteodbc keywords that only affect how the ODBC driver reports things.
static const char* const kSqliteIgnoredKeys[] = {"StepAPI",  "ShortNames", "LongNames", "NoWCHAR",
                                                 "BigInt",   "OEMCP",      NULL};

/// The DuckDB ODBC driver takes only the database and a few client-side knobs.
static const char* const kDuckdbIgnoredKeys[] = {"custom_user_agent", NULL};

/// Classify one keyword.  `uri_param` receives the native parameter name for
/// KW_FORWARD.
static enum KeywordClass ClassifyKeyword(enum OdbcNativeFamily family, const char* key,
                                         const char** uri_param) {
  if (uri_param) *uri_param = NULL;
  if (!key || !*key) return KW_IGNORED;
  if (InNameList(kConsumedKeys, key)) return KW_CONSUMED;
  if (InNameList(kIgnoredKeys, key)) return KW_IGNORED;
  switch (family) {
    case FAMILY_POSTGRESQL:
      if (InNameList(kPgIgnoredKeys, key)) return KW_IGNORED;
      for (size_t i = 0; kPgForwardKeys[i]; i++) {
        if (EqualsNoCase(kPgForwardKeys[i], key)) {
          // psqlodbc spells libpq's krbsrvname "pgkrbsrvname".
          if (uri_param) {
            *uri_param =
                EqualsNoCase(key, "pgkrbsrvname") ? "krbsrvname" : kPgForwardKeys[i];
          }
          return KW_FORWARD;
        }
      }
      if (EqualsNoCase(key, "pqopt")) return KW_FORWARD;  // handled specially
      return KW_UNKNOWN;
    case FAMILY_SQLITE:
      return InNameList(kSqliteIgnoredKeys, key) ? KW_IGNORED : KW_UNKNOWN;
    case FAMILY_DUCKDB:
      return InNameList(kDuckdbIgnoredKeys, key) ? KW_IGNORED : KW_UNKNOWN;
    default:
      return KW_UNKNOWN;
  }
}

// ---------------------------------------------------------------------------
// Building the native connection URI

/// An ordered key/value list (unlike StrList, values may repeat).
struct KeyValueList {
  char** keys;
  char** values;
  size_t count;
};

static void KvAdd(struct KeyValueList* kv, const char* key, const char* value) {
  for (size_t i = 0; i < kv->count; i++) {
    if (EqualsNoCase(kv->keys[i], key)) {
      ReplaceString(&kv->values[i], value);
      return;
    }
  }
  char** keys = realloc(kv->keys, (kv->count + 1) * sizeof(char*));
  if (!keys) return;
  kv->keys = keys;
  char** values = realloc(kv->values, (kv->count + 1) * sizeof(char*));
  if (!values) return;
  kv->values = values;
  kv->keys[kv->count] = DupString(key);
  kv->values[kv->count] = DupString(value);
  if (kv->keys[kv->count] && kv->values[kv->count]) kv->count++;
}

static void KvFree(struct KeyValueList* kv) {
  for (size_t i = 0; i < kv->count; i++) {
    free(kv->keys[i]);
    free(kv->values[i]);
  }
  free(kv->keys);
  free(kv->values);
  memset(kv, 0, sizeof(*kv));
}

/// Parse psqlodbc's `pqopt={key=value key2='value 2'}` (a libpq conninfo string)
/// into `params`.  Returns false (with *why) on an unrepresentable keyword.
static bool ParsePqopt(const char* value, struct KeyValueList* params, char** why) {
  const char* p = value;
  while (*p) {
    while (*p == ' ' || *p == '\t') p++;
    if (!*p) break;
    const char* key = p;
    while (*p && *p != '=' && *p != ' ' && *p != '\t') p++;
    size_t keylen = (size_t)(p - key);
    if (keylen == 0 || keylen >= 64) {
      *why = DupString("could not parse the psqlodbc pqopt={...} block");
      return false;
    }
    char name[64];
    memcpy(name, key, keylen);
    name[keylen] = '\0';
    while (*p == ' ' || *p == '\t') p++;
    if (*p != '=') {
      *why = DupString("could not parse the psqlodbc pqopt={...} block");
      return false;
    }
    p++;
    while (*p == ' ' || *p == '\t') p++;
    struct InternalAdbcStringBuilder sb;
    InternalAdbcStringBuilderInit(&sb, 32);
    if (*p == '\'') {
      p++;
      while (*p && *p != '\'') {
        if (*p == '\\' && p[1]) p++;
        InternalAdbcStringBuilderAppend(&sb, "%c", *p++);
      }
      if (*p == '\'') p++;
    } else {
      while (*p && *p != ' ' && *p != '\t') {
        if (*p == '\\' && p[1]) p++;
        InternalAdbcStringBuilderAppend(&sb, "%c", *p++);
      }
    }
    if (!InNameList(kPqoptKeys, name)) {
      struct InternalAdbcStringBuilder msg;
      InternalAdbcStringBuilderInit(&msg, 64);
      InternalAdbcStringBuilderAppend(
          &msg, "the libpq option \"%s\" from pqopt={...} cannot be handed to the native driver",
          name);
      *why = DupString(msg.buffer);
      InternalAdbcStringBuilderReset(&msg);
      InternalAdbcStringBuilderReset(&sb);
      return false;
    }
    KvAdd(params, name, sb.buffer ? sb.buffer : "");
    InternalAdbcStringBuilderReset(&sb);
  }
  return true;
}

/// Classify one keyword and, when it carries a setting, record it in `params`.
static bool AcceptKeyword(enum OdbcNativeFamily family, const char* key, const char* value,
                          struct KeyValueList* params, char** why) {
  const char* uri_param = NULL;
  switch (ClassifyKeyword(family, key, &uri_param)) {
    case KW_CONSUMED:
    case KW_IGNORED:
      return true;
    case KW_FORWARD:
      if (!uri_param) {  // pqopt={...}
        return ParsePqopt(value ? value : "", params, why);
      }
      if (value && *value) KvAdd(params, uri_param, value);
      return true;
    case KW_UNKNOWN:
    default: {
      struct InternalAdbcStringBuilder sb;
      InternalAdbcStringBuilderInit(&sb, 64);
      InternalAdbcStringBuilderAppend(
          &sb,
          "the ODBC keyword \"%s\" has no equivalent in the native %s driver, so delegating "
          "would silently change the connection",
          key, FamilyDriverName(family) ? FamilyDriverName(family) : "native");
      *why = DupString(sb.buffer);
      InternalAdbcStringBuilderReset(&sb);
      return false;
    }
  }
}

/// Walk every keyword of the DSN section and then of the connection string
/// (which wins), collecting the ones that must reach the native driver.
static bool CollectKeywords(enum OdbcNativeFamily family, const char* conn, const char* dsn,
                            struct KeyValueList* params, char** why) {
  if (dsn && *dsn) {
    struct StrList entries = {0};
    if (!ProfileNames(dsn, "odbc.ini", &entries)) {
      StrListFree(&entries);
      struct InternalAdbcStringBuilder sb;
      InternalAdbcStringBuilderInit(&sb, 64);
      InternalAdbcStringBuilderAppend(
          &sb, "the keywords of DSN \"%s\" cannot be enumerated, so they cannot be checked", dsn);
      *why = DupString(sb.buffer);
      InternalAdbcStringBuilderReset(&sb);
      return false;
    }
    for (size_t i = 0; i < entries.count; i++) {
      char* value = DsnGet(dsn, entries.items[i]);
      bool ok = AcceptKeyword(family, entries.items[i], value, params, why);
      free(value);
      if (!ok) {
        StrListFree(&entries);
        return false;
      }
    }
    StrListFree(&entries);
  }
  struct ConnStringIter it = {conn};
  char* key = NULL;
  char* value = NULL;
  while (ConnStringNext(&it, &key, &value)) {
    bool ok = AcceptKeyword(family, key, value, params, why);
    free(key);
    free(value);
    if (!ok) return false;
  }
  return true;
}

/// A file path taken from a URI-ish string: strip "//" but keep "file:" URIs.
static char* PathFromUriRest(const char* rest) {
  if (!rest) return NULL;
  if (strncmp(rest, "//", 2) == 0) rest += 2;
  return DupString(rest);
}

static bool AllDigits(const char* s) {
  if (!s || !*s) return false;
  for (const char* p = s; *p; p++) {
    if (!isdigit((unsigned char)*p)) return false;
  }
  return true;
}

/// Build "postgresql://user:pass@host:port/db?params" with every component
/// percent-encoded.  Unix-socket directories and IPv6 literals get the spelling
/// libpq expects; anything that cannot be expressed stops delegation.
static char* BuildPostgresUri(const char* host, const char* port, const char* database,
                              const char* user, const char* password,
                              const struct KeyValueList* params, char** why) {
  if (port && *port && !AllDigits(port)) {
    *why = DupString("the Port keyword is not a number");
    return NULL;
  }
  struct InternalAdbcStringBuilder sb;
  InternalAdbcStringBuilderInit(&sb, 160);
  InternalAdbcStringBuilderAppend(&sb, "postgresql://");
  if (user && *user) {
    AppendEncoded(&sb, user);
    if (password && *password) {
      InternalAdbcStringBuilderAppend(&sb, ":");
      AppendEncoded(&sb, password);
    }
    InternalAdbcStringBuilderAppend(&sb, "@");
  }
  // A host that is a directory is a Unix-domain socket: libpq only accepts it
  // as the "host" query parameter, never in the authority.
  bool socket_host = host && (host[0] == '/' || host[0] == '@');
  if (!socket_host) {
    const char* h = (host && *host) ? host : "localhost";
    char unbracketed[256];
    size_t hlen = strlen(h);
    if (h[0] == '[' && hlen >= 2 && h[hlen - 1] == ']' && hlen - 2 < sizeof(unbracketed)) {
      memcpy(unbracketed, h + 1, hlen - 2);
      unbracketed[hlen - 2] = '\0';
      h = unbracketed;
    }
    if (strchr(h, ':')) {
      // An IPv6 literal: it goes in brackets and must not be percent-encoded,
      // and anything else with a colon in it is not a host we can express.
      for (const char* p = h; *p; p++) {
        if (!isxdigit((unsigned char)*p) && *p != ':' && *p != '.') {
          InternalAdbcStringBuilderReset(&sb);
          *why = DupString("the Server keyword is neither a host name nor an IPv6 address");
          return NULL;
        }
      }
      InternalAdbcStringBuilderAppend(&sb, "[%s]", h);
    } else {
      AppendEncoded(&sb, h);
    }
    if (port && *port) InternalAdbcStringBuilderAppend(&sb, ":%s", port);
  }
  InternalAdbcStringBuilderAppend(&sb, "/");
  if (database && *database) AppendEncoded(&sb, database);

  const char* sep = "?";
  if (socket_host) {
    InternalAdbcStringBuilderAppend(&sb, "%shost=", sep);
    AppendEncoded(&sb, host);
    sep = "&";
    if (port && *port) {
      InternalAdbcStringBuilderAppend(&sb, "%sport=%s", sep, port);
      sep = "&";
    }
  }
  for (size_t i = 0; params && i < params->count; i++) {
    InternalAdbcStringBuilderAppend(&sb, "%s", sep);
    AppendEncoded(&sb, params->keys[i]);
    InternalAdbcStringBuilderAppend(&sb, "=");
    AppendEncoded(&sb, params->values[i]);
    sep = "&";
  }
  char* uri = DupString(sb.buffer);
  InternalAdbcStringBuilderReset(&sb);
  return uri;
}

static const char* const kHostKeys[] = {"Server", "Servername", "Host", "Hostname", NULL};
static const char* const kPortKeys[] = {"Port", NULL};
static const char* const kDatabaseKeys[] = {"Database", "DB", "Data Source", "Dbname", NULL};
static const char* const kUserKeys[] = {"Uid", "User", "Username", NULL};
static const char* const kPasswordKeys[] = {"Pwd", "Password", NULL};

/// Build the native connection URI for `family` from the ODBC keywords.
/// Returns NULL (with *why) when the ODBC options cannot be represented.
static char* BuildNativeUri(enum OdbcNativeFamily family, const char* conn, const char* dsn,
                            const char* username, const char* password, char** why) {
  struct KeyValueList params = {0};
  if (!CollectKeywords(family, conn, dsn, &params, why)) {
    KvFree(&params);
    return NULL;
  }
  char* database = KeywordGet(conn, dsn, kDatabaseKeys);
  char* uri = NULL;

  switch (family) {
    case FAMILY_SQLITE:
    case FAMILY_DUCKDB: {
      // The native SQLite/DuckDB drivers take the database file directly.
      if (database && *database) {
        uri = DupString(database);
      } else {
        *why = DupString("no Database keyword to hand to the native driver");
      }
      break;
    }
    case FAMILY_POSTGRESQL: {
      char* host = KeywordGet(conn, dsn, kHostKeys);
      char* port = KeywordGet(conn, dsn, kPortKeys);
      char* user = username ? DupString(username) : KeywordGet(conn, dsn, kUserKeys);
      char* pass = password ? DupString(password) : KeywordGet(conn, dsn, kPasswordKeys);
      uri = BuildPostgresUri(host, port, database, user, pass, &params, why);
      free(host);
      free(port);
      free(user);
      free(pass);
      break;
    }
    default:
      // Snowflake/BigQuery/Flight SQL URIs cannot be reconstructed from ODBC
      // keywords; the caller must pass a native URI.
      *why = DupString("a native URI is required for this database family");
      break;
  }
  free(database);
  KvFree(&params);
  return uri;
}

#if !defined(_WIN32)
/// Append `key=value` to a native URI's query string.  Only the hand-over to a
/// native driver needs it, which is why it is not compiled on Windows.
static char* UriWithParams(const char* uri, const struct KeyValueList* params) {
  struct InternalAdbcStringBuilder sb;
  InternalAdbcStringBuilderInit(&sb, strlen(uri) + 64);
  InternalAdbcStringBuilderAppend(&sb, "%s", uri);
  const char* sep = strchr(uri, '?') ? "&" : "?";
  for (size_t i = 0; i < params->count; i++) {
    InternalAdbcStringBuilderAppend(&sb, "%s", sep);
    AppendEncoded(&sb, params->keys[i]);
    InternalAdbcStringBuilderAppend(&sb, "=");
    AppendEncoded(&sb, params->values[i]);
    sep = "&";
  }
  char* out = DupString(sb.buffer);
  InternalAdbcStringBuilderReset(&sb);
  return out;
}
#endif  // !_WIN32

// ---------------------------------------------------------------------------
// The other direction: a native URI on the ODBC fallback path
//
// unixODBC cannot do anything with "postgresql://..." (it answers IM002 "data
// source not found", which tells the user nothing), so when delegation does not
// happen for a native URI we translate it into an ODBC connection string --
// or say clearly that we cannot.

/// The pieces of a "scheme://user:pass@host:port/path?query" URI, decoded.
struct ParsedUri {
  char* user;
  char* password;
  char* host;
  char* port;
  char* path;
  struct KeyValueList params;
};

static void ParsedUriFree(struct ParsedUri* uri) {
  free(uri->user);
  free(uri->password);
  free(uri->host);
  free(uri->port);
  free(uri->path);
  KvFree(&uri->params);
  memset(uri, 0, sizeof(*uri));
}

static void ParseUriQuery(const char* query, struct KeyValueList* params) {
  const char* p = query;
  while (p && *p) {
    const char* amp = strchr(p, '&');
    const char* end = amp ? amp : p + strlen(p);
    const char* eq = memchr(p, '=', (size_t)(end - p));
    if (eq) {
      char* key = PercentDecode(p, (size_t)(eq - p));
      char* value = PercentDecode(eq + 1, (size_t)(end - eq - 1));
      if (key && value) KvAdd(params, key, value);
      free(key);
      free(value);
    }
    if (!amp) break;
    p = amp + 1;
  }
}

/// Parse the "//authority/path?query" part of a native URI.
static void ParseUriRest(const char* rest, struct ParsedUri* out) {
  memset(out, 0, sizeof(*out));
  if (strncmp(rest, "//", 2) == 0) {
    rest += 2;
    const char* authority_end = rest + strcspn(rest, "/?#");
    const char* at = NULL;
    for (const char* p = rest; p < authority_end; p++) {
      if (*p == '@') at = p;
    }
    const char* hostpart = rest;
    if (at) {
      const char* colon = memchr(rest, ':', (size_t)(at - rest));
      out->user = PercentDecode(rest, (size_t)((colon ? colon : at) - rest));
      if (colon) out->password = PercentDecode(colon + 1, (size_t)(at - colon - 1));
      hostpart = at + 1;
    }
    if (hostpart < authority_end && *hostpart == '[') {
      const char* close = memchr(hostpart, ']', (size_t)(authority_end - hostpart));
      if (close) {
        out->host = PercentDecode(hostpart + 1, (size_t)(close - hostpart - 1));
        if (close + 1 < authority_end && close[1] == ':') {
          out->port = PercentDecode(close + 2, (size_t)(authority_end - close - 2));
        }
        hostpart = authority_end;
      }
    }
    if (!out->host && hostpart < authority_end) {
      const char* colon = memchr(hostpart, ':', (size_t)(authority_end - hostpart));
      out->host = PercentDecode(hostpart, (size_t)((colon ? colon : authority_end) - hostpart));
      if (colon) out->port = PercentDecode(colon + 1, (size_t)(authority_end - colon - 1));
    }
    rest = authority_end;
  }
  const char* query = strchr(rest, '?');
  const char* path_end = query ? query : rest + strlen(rest);
  if (*rest == '/') rest++;
  if (path_end > rest) out->path = PercentDecode(rest, (size_t)(path_end - rest));
  if (query) ParseUriQuery(query + 1, &out->params);
}

/// The name of an installed ODBC driver that can serve `family`, from
/// odbcinst.ini.  Returns malloc'd or NULL.
static char* FindOdbcDriverForFamily(enum OdbcNativeFamily family) {
  struct StrList drivers = {0};
  char* found = NULL;
  if (ProfileNames(NULL, "odbcinst.ini", &drivers)) {
    for (size_t i = 0; i < drivers.count && !found; i++) {
      if (EqualsNoCase(drivers.items[i], "ODBC Drivers")) continue;
      if (FamilyFromOdbcDriver(drivers.items[i]) == family) found = DupString(drivers.items[i]);
    }
  }
  StrListFree(&drivers);
  return found;
}

static void AppendOdbcValue(struct InternalAdbcStringBuilder* sb, const char* value) {
  // Brace-quote and escape per the ODBC connection-string grammar.
  InternalAdbcStringBuilderAppend(sb, "{");
  for (const char* p = value; *p; p++) {
    if (*p == '}') InternalAdbcStringBuilderAppend(sb, "}");
    InternalAdbcStringBuilderAppend(sb, "%c", *p);
  }
  InternalAdbcStringBuilderAppend(sb, "}");
}

AdbcStatusCode OdbcDelegateNativeUriToOdbc(const char* uri, const char* last_error, char** out,
                                           struct AdbcError* error) {
  const char* rest = NULL;
  enum OdbcNativeFamily family = FamilyFromUri(uri, &rest);
  *out = NULL;
  if (family == FAMILY_NONE) return ADBC_STATUS_OK;  // an ordinary ODBC string

  char* driver = FindOdbcDriverForFamily(family);
  if (!driver) {
    InternalAdbcSetError(
        error,
        "\"%s\" is a native ADBC URI: no native \"%s\" driver took the connection (%s), and no "
        "ODBC driver for %s is installed to fall back to. Install the native driver, or pass an "
        "ODBC connection string (\"Driver=...;Server=...\").",
        uri, FamilyDriverName(family), (last_error && *last_error) ? last_error : "no reason given",
        FamilyDriverName(family));
    return ADBC_STATUS_INVALID_ARGUMENT;
  }

  struct InternalAdbcStringBuilder sb;
  InternalAdbcStringBuilderInit(&sb, 128);
  InternalAdbcStringBuilderAppend(&sb, "Driver=");
  AppendOdbcValue(&sb, driver);
  InternalAdbcStringBuilderAppend(&sb, ";");
  AdbcStatusCode status = ADBC_STATUS_OK;

  if (family == FAMILY_SQLITE || family == FAMILY_DUCKDB) {
    char* path = PathFromUriRest(rest);
    InternalAdbcStringBuilderAppend(&sb, "Database=");
    AppendOdbcValue(&sb, path ? path : "");
    InternalAdbcStringBuilderAppend(&sb, ";");
    free(path);
  } else if (family == FAMILY_POSTGRESQL) {
    struct ParsedUri parsed;
    ParseUriRest(rest, &parsed);
    InternalAdbcStringBuilderAppend(&sb, "Server=");
    AppendOdbcValue(&sb, parsed.host ? parsed.host : "localhost");
    InternalAdbcStringBuilderAppend(&sb, ";");
    if (parsed.port) {
      InternalAdbcStringBuilderAppend(&sb, "Port=");
      AppendOdbcValue(&sb, parsed.port);
      InternalAdbcStringBuilderAppend(&sb, ";");
    }
    if (parsed.path) {
      InternalAdbcStringBuilderAppend(&sb, "Database=");
      AppendOdbcValue(&sb, parsed.path);
      InternalAdbcStringBuilderAppend(&sb, ";");
    }
    if (parsed.user) {
      InternalAdbcStringBuilderAppend(&sb, "Uid=");
      AppendOdbcValue(&sb, parsed.user);
      InternalAdbcStringBuilderAppend(&sb, ";");
    }
    if (parsed.password) {
      InternalAdbcStringBuilderAppend(&sb, "Pwd=");
      AppendOdbcValue(&sb, parsed.password);
      InternalAdbcStringBuilderAppend(&sb, ";");
    }
    for (size_t i = 0; i < parsed.params.count; i++) {
      // Only settings psqlodbc understands under the same name survive; anything
      // else would be dropped, which is exactly what we refuse to do silently.
      if (EqualsNoCase(parsed.params.keys[i], "sslmode")) {
        InternalAdbcStringBuilderAppend(&sb, "SSLmode=");
        AppendOdbcValue(&sb, parsed.params.values[i]);
        InternalAdbcStringBuilderAppend(&sb, ";");
      } else if (EqualsNoCase(parsed.params.keys[i], "host")) {
        InternalAdbcStringBuilderAppend(&sb, "Server=");
        AppendOdbcValue(&sb, parsed.params.values[i]);
        InternalAdbcStringBuilderAppend(&sb, ";");
      } else if (EqualsNoCase(parsed.params.keys[i], "port")) {
        InternalAdbcStringBuilderAppend(&sb, "Port=");
        AppendOdbcValue(&sb, parsed.params.values[i]);
        InternalAdbcStringBuilderAppend(&sb, ";");
      } else if (EqualsNoCase(parsed.params.keys[i], "user")) {
        InternalAdbcStringBuilderAppend(&sb, "Uid=");
        AppendOdbcValue(&sb, parsed.params.values[i]);
        InternalAdbcStringBuilderAppend(&sb, ";");
      } else if (EqualsNoCase(parsed.params.keys[i], "password")) {
        InternalAdbcStringBuilderAppend(&sb, "Pwd=");
        AppendOdbcValue(&sb, parsed.params.values[i]);
        InternalAdbcStringBuilderAppend(&sb, ";");
      } else {
        InternalAdbcSetError(error,
                             "\"%s\" is a native ADBC URI that no native driver took (%s), and its "
                             "parameter \"%s\" cannot be expressed for the ODBC driver \"%s\"",
                             uri, (last_error && *last_error) ? last_error : "no reason given",
                             parsed.params.keys[i], driver);
        status = ADBC_STATUS_INVALID_ARGUMENT;
        break;
      }
    }
    ParsedUriFree(&parsed);
  } else {
    InternalAdbcSetError(error,
                         "\"%s\" is a native ADBC URI that no native driver took (%s), and it "
                         "cannot be translated into an ODBC connection string",
                         uri, (last_error && *last_error) ? last_error : "no reason given");
    status = ADBC_STATUS_INVALID_ARGUMENT;
  }

  if (status == ADBC_STATUS_OK) *out = DupString(sb.buffer);
  InternalAdbcStringBuilderReset(&sb);
  free(driver);
  return status;
}

// ---------------------------------------------------------------------------
// Target detection

/// Which native driver fits, and what to hand it.
struct NativeTarget {
  char* driver_name;  // "postgresql", or the value of adbc.odbc.delegate.driver
  char* uri;
  bool uri_verbatim;  // the caller passed a native URI; ODBC cannot serve it
  enum OdbcNativeFamily family;
};

static void NativeTargetFree(struct NativeTarget* t) {
  free(t->driver_name);
  free(t->uri);
  memset(t, 0, sizeof(*t));
}

/// Work out which native driver fits, and the URI to hand it.
/// Returns false (with *why set) when nothing fits.
static bool DetectNative(const struct OdbcDelegateTarget* target,
                         const struct OdbcDelegateOptions* opts, struct NativeTarget* out,
                         char** why) {
  memset(out, 0, sizeof(*out));
  const char* rest = NULL;
  enum OdbcNativeFamily family = FamilyFromUri(target->connection_string, &rest);
  char* uri = NULL;
  // A DSN reaches us either as the "dsn" option or as DSN= in the connection string.
  char* dsn = (target->dsn && *target->dsn) ? DupString(target->dsn)
                                            : ConnStringGet(target->connection_string, "DSN");

  out->uri_verbatim = family != FAMILY_NONE;
  if (family != FAMILY_NONE) {
    // A native-looking URI was passed straight through.
    uri = (family == FAMILY_SQLITE || family == FAMILY_DUCKDB)
              ? PathFromUriRest(rest)
              : DupString(target->connection_string);
  }

  if (opts->driver && *opts->driver) {
    // The native driver is forced; only the URI still has to be worked out.
    if (family == FAMILY_NONE) {
      family = FamilyFromDriverName(opts->driver);
      if (family == FAMILY_NONE) {
        free(uri);
        free(dsn);
        *why = DupString(
            "adbc.odbc.delegate.driver does not name a known database family; "
            "pass a native URI as \"uri\"");
        return false;
      }
      uri = BuildNativeUri(family, target->connection_string, dsn, target->username,
                           target->password, why);
    }
    free(dsn);
    if (!uri) {
      if (!*why) *why = DupString("could not build a native connection URI from the ODBC options");
      return false;
    }
    out->driver_name = DupString(opts->driver);
    out->uri = uri;
    out->family = family;
    return true;
  }

  if (family == FAMILY_NONE) {
    // An ODBC connection string or a DSN: look at Driver= / the DSN section.
    static const char* const kDriverKeys[] = {"Driver", NULL};
    char* odbc_driver = KeywordGet(target->connection_string, dsn, kDriverKeys);
    family = FamilyFromOdbcDriver(odbc_driver);
    if (family == FAMILY_NONE) {
      struct InternalAdbcStringBuilder sb;
      InternalAdbcStringBuilderInit(&sb, 128);
      InternalAdbcStringBuilderAppend(&sb, "no native ADBC driver is known for ");
      if (odbc_driver && *odbc_driver) {
        InternalAdbcStringBuilderAppend(&sb, "ODBC driver \"%s\"", odbc_driver);
      } else if (dsn && *dsn) {
        InternalAdbcStringBuilderAppend(&sb, "DSN \"%s\"", dsn);
      } else {
        InternalAdbcStringBuilderAppend(&sb, "this connection string");
      }
      *why = DupString(sb.buffer);
      InternalAdbcStringBuilderReset(&sb);
      free(odbc_driver);
      free(dsn);
      return false;
    }
    free(odbc_driver);
    uri = BuildNativeUri(family, target->connection_string, dsn, target->username,
                         target->password, why);
    free(dsn);
    dsn = NULL;
    if (!uri) {
      if (!*why) {
        struct InternalAdbcStringBuilder sb;
        InternalAdbcStringBuilderInit(&sb, 128);
        InternalAdbcStringBuilderAppend(
            &sb, "could not build a native %s URI from the ODBC options", FamilyDriverName(family));
        *why = DupString(sb.buffer);
        InternalAdbcStringBuilderReset(&sb);
      }
      return false;
    }
  }

  free(dsn);
  out->driver_name = DupString(FamilyDriverName(family));
  out->uri = uri;
  out->family = family;
  return true;
}

// ---------------------------------------------------------------------------
// Loading the native driver through the ADBC driver manager
//
// Only AdbcLoadDriver is used: AdbcFindLoadDriver changed its signature between
// driver manager releases (1.7.0 has six parameters, 1.8.0 seven), and calling
// it with the wrong one corrupts the arguments.  AdbcLoadDriver has had the
// same five parameters since 1.0.0, so the manifest lookup that the newer
// AdbcFindLoadDriver would have done is done here instead.

typedef AdbcStatusCode (*AdbcLoadDriverFn)(const char* driver_name, const char* entrypoint,
                                           int version, void* driver, struct AdbcError* error);

/// The platform tuple used as the key in a driver manifest's [Driver.shared]
/// table, spelled exactly as the ADBC driver manager spells it.
#if defined(_WIN32)
#define ADBC_ODBC_MANIFEST_OS "windows"
#elif defined(__APPLE__)
#define ADBC_ODBC_MANIFEST_OS "macos"
#elif defined(__FreeBSD__)
#define ADBC_ODBC_MANIFEST_OS "freebsd"
#elif defined(__OpenBSD__)
#define ADBC_ODBC_MANIFEST_OS "openbsd"
#else
#define ADBC_ODBC_MANIFEST_OS "linux"
#endif

#if defined(__x86_64__) || defined(_M_X64)
#define ADBC_ODBC_MANIFEST_ARCH "amd64"
#elif defined(__aarch64__) || defined(_M_ARM64)
#define ADBC_ODBC_MANIFEST_ARCH "arm64"
#elif defined(__i386__) || defined(_M_IX86)
#define ADBC_ODBC_MANIFEST_ARCH "x86"
#elif defined(__arm__) || defined(_M_ARM)
#define ADBC_ODBC_MANIFEST_ARCH "arm"
#elif defined(__powerpc64__) && defined(__LITTLE_ENDIAN__)
#define ADBC_ODBC_MANIFEST_ARCH "powerpc64le"
#elif defined(__powerpc64__)
#define ADBC_ODBC_MANIFEST_ARCH "powerpc64"
#elif defined(__powerpc__)
#define ADBC_ODBC_MANIFEST_ARCH "powerpc"
#elif defined(__riscv) && __riscv_xlen == 64
#define ADBC_ODBC_MANIFEST_ARCH "riscv64"
#elif defined(__s390x__)
#define ADBC_ODBC_MANIFEST_ARCH "s390x"
#else
#define ADBC_ODBC_MANIFEST_ARCH "unknown"
#endif

#if defined(__MINGW32__)
#define ADBC_ODBC_MANIFEST_ENV "_mingw"
#elif defined(__linux__) && !defined(__GLIBC__)
#define ADBC_ODBC_MANIFEST_ENV "_musl"
#else
#define ADBC_ODBC_MANIFEST_ENV ""
#endif

#define ADBC_ODBC_MANIFEST_PLATFORM \
  ADBC_ODBC_MANIFEST_OS "_" ADBC_ODBC_MANIFEST_ARCH ADBC_ODBC_MANIFEST_ENV

#if !defined(_WIN32)
/// Read the shared-library path out of an ADBC driver manifest (a TOML file
/// whose [Driver.shared] table maps platform tuples to paths).
static char* ManifestLibraryPath(const char* manifest_path) {
  FILE* f = fopen(manifest_path, "rb");
  if (!f) return NULL;
  char line[2048];
  char* found = NULL;
  while (!found && fgets(line, sizeof(line), f)) {
    char* p = line;
    while (*p == ' ' || *p == '\t') p++;
    if (*p == '#' || *p == '[') continue;
    char* eq = strchr(p, '=');
    if (!eq) continue;
    char* kend = eq;
    while (kend > p && (kend[-1] == ' ' || kend[-1] == '\t')) kend--;
    size_t keylen = (size_t)(kend - p);
    char key[128];
    if (keylen == 0 || keylen >= sizeof(key)) continue;
    memcpy(key, p, keylen);
    key[keylen] = '\0';
    if (!EqualsNoCase(key, ADBC_ODBC_MANIFEST_PLATFORM) && !EqualsNoCase(key, "shared")) continue;
    char* v = eq + 1;
    while (*v == ' ' || *v == '\t') v++;
    char quote = *v;
    if (quote != '"' && quote != '\'') continue;
    v++;
    char* end = strchr(v, quote);
    if (!end) continue;
    *end = '\0';
    if (*v) found = DupString(v);
  }
  fclose(f);
  return found;
}

/// Directories of every already-loaded shared object whose path mentions ADBC,
/// plus their parents (site-packages, lib/, ...), and the driver manager itself.
struct LoadedObjectScan {
  struct StrList dirs;     // the objects' own directories
  struct StrList parents;  // one level up: a Python wheel's site-packages
  struct StrList managers;
};

static void ScanObjectPath(struct LoadedObjectScan* scan, const char* path) {
  if (!path || !*path) return;
  if (!ContainsNoCase(path, "adbc")) return;
  if (ContainsNoCase(path, "adbc_driver_manager")) StrListAdd(&scan->managers, path);
  char* dir = DirName(path);
  if (!dir) return;
  StrListAdd(&scan->dirs, dir);
  char* parent = DirName(dir);
  if (parent) {
    StrListAdd(&scan->parents, parent);
    free(parent);
  }
  free(dir);
}

#if defined(__linux__) || defined(__FreeBSD__)
static int ScanPhdrCallback(struct dl_phdr_info* info, size_t size, void* data) {
  (void)size;
  ScanObjectPath((struct LoadedObjectScan*)data, info->dlpi_name);
  return 0;
}
#endif

static void ScanLoadedObjects(struct LoadedObjectScan* scan) {
#if defined(__linux__) || defined(__FreeBSD__)
  dl_iterate_phdr(ScanPhdrCallback, scan);
#elif defined(__APPLE__)
  uint32_t count = _dyld_image_count();
  for (uint32_t i = 0; i < count; i++) ScanObjectPath(scan, _dyld_get_image_name(i));
#else
  (void)scan;
#endif
}

static void LoadedObjectScanFree(struct LoadedObjectScan* scan) {
  StrListFree(&scan->dirs);
  StrListFree(&scan->parents);
  StrListFree(&scan->managers);
}

static AdbcLoadDriverFn g_load_driver = NULL;
static pthread_mutex_t g_load_driver_mutex = PTHREAD_MUTEX_INITIALIZER;

/// Resolve AdbcLoadDriver from whichever driver manager is already in the
/// process.  We never link against the driver manager (that would be a circular
/// dependency), so this is all dlsym; the handle is kept for the process.
static AdbcLoadDriverFn DriverManagerLoadFn(const struct StrList* managers) {
  pthread_mutex_lock(&g_load_driver_mutex);
  if (g_load_driver) {
    AdbcLoadDriverFn fn = g_load_driver;
    pthread_mutex_unlock(&g_load_driver_mutex);
    return fn;
  }
  g_load_driver = (AdbcLoadDriverFn)dlsym(RTLD_DEFAULT, "AdbcLoadDriver");
  if (!g_load_driver) {
    struct StrList candidates = {0};
    for (size_t i = 0; i < managers->count; i++) {
      StrListAdd(&candidates, managers->items[i]);
      char* dir = DirName(managers->items[i]);
      if (dir) {
        char buf[4096];
        snprintf(buf, sizeof(buf), "%s/libadbc_driver_manager.so", dir);
        StrListAdd(&candidates, buf);
        snprintf(buf, sizeof(buf), "%s/libadbc_driver_manager.dylib", dir);
        StrListAdd(&candidates, buf);
        free(dir);
      }
    }
    StrListAdd(&candidates, "libadbc_driver_manager.so");
    StrListAdd(&candidates, "libadbc_driver_manager.so.0");
    StrListAdd(&candidates, "libadbc_driver_manager.dylib");
    for (size_t i = 0; i < candidates.count; i++) {
      void* handle = dlopen(candidates.items[i], RTLD_LAZY | RTLD_LOCAL);
      if (!handle) continue;
      g_load_driver = (AdbcLoadDriverFn)dlsym(handle, "AdbcLoadDriver");
      if (g_load_driver) break;
      dlclose(handle);
    }
    StrListFree(&candidates);
  }
  AdbcLoadDriverFn fn = g_load_driver;
  pthread_mutex_unlock(&g_load_driver_mutex);
  return fn;
}

/// The directories the driver manager looks in for `<name>.toml` manifests.
static void ManifestDirs(struct StrList* out, const struct StrList* extra) {
  if (!IsSecureExec()) StrListAddPathList(out, getenv("ADBC_DRIVER_PATH"));
  char buf[4096];
  const char* xdg = getenv("XDG_CONFIG_HOME");
  const char* home = getenv("HOME");
  if (xdg && *xdg) {
    snprintf(buf, sizeof(buf), "%s/adbc/drivers", xdg);
    StrListAdd(out, buf);
  } else if (home && *home) {
    snprintf(buf, sizeof(buf), "%s/.config/adbc/drivers", home);
    StrListAdd(out, buf);
  }
  StrListAdd(out, "/etc/adbc/drivers");
  for (size_t i = 0; extra && i < extra->count; i++) StrListAdd(out, extra->items[i]);
}

/// Every place a native driver called `name` might live, in priority order.
static void NativeDriverCandidates(const char* name, bool is_path,
                                   const struct LoadedObjectScan* scan,
                                   const struct StrList* extra_dirs, struct StrList* out) {
  char path[4096];
  if (is_path) {
    size_t len = strlen(name);
    if (len > 5 && strcmp(name + len - 5, ".toml") == 0) {
      char* resolved = ManifestLibraryPath(name);
      if (resolved) {
        StrListAdd(out, resolved);
        free(resolved);
      }
    }
    StrListAdd(out, name);
    return;
  }

  // 1. Driver manifests, the way the driver manager resolves a bare name.
  struct StrList manifest_dirs = {0};
  ManifestDirs(&manifest_dirs, extra_dirs);
  for (size_t i = 0; i < manifest_dirs.count; i++) {
    snprintf(path, sizeof(path), "%s/%s.toml", manifest_dirs.items[i], name);
    if (!FileExists(path)) continue;
    char* resolved = ManifestLibraryPath(path);
    if (resolved) {
      StrListAdd(out, resolved);
      free(resolved);
    }
  }
  StrListFree(&manifest_dirs);

  // 2. Python-wheel layouts (adbc_driver_postgresql/libadbc_driver_postgresql.so)
  //    next to whatever ADBC object is already loaded, and one level up, which
  //    is where site-packages is.  Only this exact layout is accepted there: a
  //    bare libadbc_driver_<name>.so dropped into a shared parent directory
  //    must not become loadable code.
  for (int pass = 0; pass < 2; pass++) {
    const struct StrList* dirs = pass == 0 ? &scan->dirs : &scan->parents;
    for (size_t i = 0; i < dirs->count; i++) {
      snprintf(path, sizeof(path), "%s/adbc_driver_%s/libadbc_driver_%s.so", dirs->items[i], name,
               name);
      if (FileExists(path) && PathIsTrusted(path)) StrListAdd(out, path);
      snprintf(path, sizeof(path), "%s/adbc_driver_%s/libadbc_driver_%s.dylib", dirs->items[i],
               name, name);
      if (FileExists(path) && PathIsTrusted(path)) StrListAdd(out, path);
    }
  }

  // 3. Directories the caller nominated explicitly (adbc.odbc.delegate.search_path
  //    or ADBC_ODBC_DELEGATE_PATH), and the ADBC objects' own directories.
  for (int pass = 0; pass < 2; pass++) {
    const struct StrList* dirs = pass == 0 ? extra_dirs : &scan->dirs;
    bool derived = pass == 1;
    for (size_t i = 0; dirs && i < dirs->count; i++) {
      snprintf(path, sizeof(path), "%s/libadbc_driver_%s.so", dirs->items[i], name);
      if (FileExists(path) && (!derived || PathIsTrusted(path))) StrListAdd(out, path);
      snprintf(path, sizeof(path), "%s/libadbc_driver_%s.dylib", dirs->items[i], name);
      if (FileExists(path) && (!derived || PathIsTrusted(path))) StrListAdd(out, path);
      snprintf(path, sizeof(path), "%s/adbc_driver_%s/libadbc_driver_%s.so", dirs->items[i], name,
               name);
      if (FileExists(path) && (!derived || PathIsTrusted(path))) StrListAdd(out, path);
    }
  }

  // 4. The dynamic loader's own search path, and finally the driver manager's
  //    own interpretation of the name.
  snprintf(path, sizeof(path), "libadbc_driver_%s.so", name);
  StrListAdd(out, path);
  snprintf(path, sizeof(path), "libadbc_driver_%s.dylib", name);
  StrListAdd(out, path);
  StrListAdd(out, name);
}

/// Try every plausible location for the native driver called `name`.
/// On success `driver` holds a fully populated (manager-wrapped) function table.
static bool LoadNativeDriver(AdbcLoadDriverFn load, const char* name, bool is_path,
                             const struct LoadedObjectScan* scan, const struct StrList* extra_dirs,
                             int* version, struct AdbcDriver* driver, char** why) {
  struct StrList candidates = {0};
  NativeDriverCandidates(name, is_path, scan, extra_dirs, &candidates);

  char last[512] = {0};
  bool loaded = false;
  // ADBC 1.1.0 first: a 1.0.0-only driver must reject it, and then we ask again
  // for the older table.  The proxy only exposes what the table it got has.
  static const int kVersions[] = {ADBC_VERSION_1_1_0, ADBC_VERSION_1_0_0};
  for (size_t v = 0; !loaded && v < sizeof(kVersions) / sizeof(kVersions[0]); v++) {
    for (size_t i = 0; i < candidates.count; i++) {
      struct AdbcError error = ADBC_ERROR_INIT;
      memset(driver, 0, sizeof(*driver));
      if (load(candidates.items[i], NULL, kVersions[v], driver, &error) == ADBC_STATUS_OK) {
        *version = kVersions[v];
        loaded = true;
        break;
      }
      if (error.message) snprintf(last, sizeof(last), "%s", error.message);
      if (error.release) error.release(&error);
    }
  }
  StrListFree(&candidates);
  if (loaded) return true;

  memset(driver, 0, sizeof(*driver));
  struct InternalAdbcStringBuilder sb;
  InternalAdbcStringBuilderInit(&sb, 128);
  InternalAdbcStringBuilderAppend(&sb, "no native \"%s\" ADBC driver could be loaded", name);
  if (last[0]) InternalAdbcStringBuilderAppend(&sb, " (%s)", last);
  *why = DupString(sb.buffer);
  InternalAdbcStringBuilderReset(&sb);
  return false;
}
#endif  // !_WIN32

// ---------------------------------------------------------------------------
// Options

/// Parse an "auto"/"never"/"always" value.  Returns false if it is none of them.
static bool ParseMode(const char* value, enum OdbcDelegateMode* mode) {
  if (!value || strcmp(value, "auto") == 0) {
    *mode = ODBC_DELEGATE_AUTO;
  } else if (strcmp(value, "never") == 0) {
    *mode = ODBC_DELEGATE_NEVER;
  } else if (strcmp(value, "always") == 0) {
    *mode = ODBC_DELEGATE_ALWAYS;
  } else {
    return false;
  }
  return true;
}

static bool ParseBool(const char* value, bool* out) {
  if (!value) return false;
  if (strcmp(value, "true") == 0 || strcmp(value, "1") == 0) {
    *out = true;
    return true;
  }
  if (strcmp(value, "false") == 0 || strcmp(value, "0") == 0) {
    *out = false;
    return true;
  }
  return false;
}

void OdbcDelegateOptionsInit(struct OdbcDelegateOptions* opts) {
  memset(opts, 0, sizeof(*opts));
  // A deployment-wide off switch, for hosts that cannot pass options through.
  const char* env = getenv(ADBC_ODBC_DELEGATE_ENV);
  if (env && *env && !ParseMode(env, &opts->mode)) { opts->mode = ODBC_DELEGATE_AUTO; }
  // Loading a native driver from a path named by a *database option* is opt-in:
  // many hosts forward caller-supplied options verbatim, and dlopen()ing what
  // they name would turn this driver into an arbitrary code loader.
  const char* allow = getenv(ADBC_ODBC_DELEGATE_ALLOW_PATHS_ENV);
  if (allow && !IsSecureExec()) (void)ParseBool(allow, &opts->allow_paths);
}

void OdbcDelegateOptionsRelease(struct OdbcDelegateOptions* opts) {
  free(opts->driver);
  free(opts->search_path);
  free(opts->last_error);
  free(opts->delegated_to);
  for (size_t i = 0; i < opts->pass_count; i++) {
    free(opts->pass_keys[i]);
    free(opts->pass_values[i]);
  }
  free(opts->pass_keys);
  free(opts->pass_values);
  memset(opts, 0, sizeof(*opts));
}

/// Is this an option meant for the native driver rather than for us?
static bool IsPassThroughKey(const char* key) {
  if (strncmp(key, "adbc.", 5) != 0) return false;
  if (strncmp(key, "adbc.odbc.", 10) == 0) return false;
  return true;
}

static AdbcStatusCode AddPassThrough(struct OdbcDelegateOptions* opts, const char* key,
                                     const char* value, struct AdbcError* error) {
  for (size_t i = 0; i < opts->pass_count; i++) {
    if (strcmp(opts->pass_keys[i], key) == 0) {
      ReplaceString(&opts->pass_values[i], value);
      return ADBC_STATUS_OK;
    }
  }
  char** keys = realloc(opts->pass_keys, (opts->pass_count + 1) * sizeof(char*));
  if (!keys) goto oom;
  opts->pass_keys = keys;
  char** values = realloc(opts->pass_values, (opts->pass_count + 1) * sizeof(char*));
  if (!values) goto oom;
  opts->pass_values = values;
  opts->pass_keys[opts->pass_count] = DupString(key);
  opts->pass_values[opts->pass_count] = DupString(value);
  if (!opts->pass_keys[opts->pass_count]) goto oom;
  opts->pass_count++;
  return ADBC_STATUS_OK;
oom:
  InternalAdbcSetError(error, "out of memory");
  return ADBC_STATUS_INTERNAL;
}

const char* OdbcDelegateHeldOption(const struct OdbcDelegateOptions* opts) {
  return opts->pass_count ? opts->pass_keys[0] : NULL;
}

bool OdbcDelegateSetOption(struct OdbcDelegateOptions* opts, const char* key, const char* value,
                           AdbcStatusCode* status, struct AdbcError* error) {
  bool ours = strcmp(key, ADBC_ODBC_OPTION_DELEGATE) == 0 ||
              strcmp(key, ADBC_ODBC_OPTION_DELEGATE_DRIVER) == 0 ||
              strcmp(key, ADBC_ODBC_OPTION_DELEGATE_SEARCH_PATH) == 0 ||
              strcmp(key, ADBC_ODBC_OPTION_DELEGATE_ALLOW_PATHS) == 0 ||
              strcmp(key, ADBC_ODBC_OPTION_DELEGATE_LAST_ERROR) == 0 ||
              strcmp(key, ADBC_ODBC_OPTION_DELEGATED_TO) == 0 || IsPassThroughKey(key);
  if (!ours) return false;

  if (strcmp(key, ADBC_ODBC_OPTION_DELEGATE_LAST_ERROR) == 0 ||
      strcmp(key, ADBC_ODBC_OPTION_DELEGATED_TO) == 0) {
    InternalAdbcSetError(error, "%s is read-only", key);
    *status = ADBC_STATUS_INVALID_ARGUMENT;
    return true;
  }
  if (opts->initialized) {
    // Whether to delegate was decided in AdbcDatabaseInit; changing it now would
    // silently do nothing.
    InternalAdbcSetError(error, "%s cannot be set after AdbcDatabaseInit", key);
    *status = ADBC_STATUS_INVALID_STATE;
    return true;
  }

  if (strcmp(key, ADBC_ODBC_OPTION_DELEGATE) == 0) {
    if (!ParseMode(value, &opts->mode)) {
      InternalAdbcSetError(error, "Invalid value \"%s\" for %s (auto/never/always)", value, key);
      *status = ADBC_STATUS_INVALID_ARGUMENT;
      return true;
    }
    *status = ADBC_STATUS_OK;
    return true;
  }
  if (strcmp(key, ADBC_ODBC_OPTION_DELEGATE_ALLOW_PATHS) == 0) {
    if (!ParseBool(value, &opts->allow_paths)) {
      InternalAdbcSetError(error, "Invalid value \"%s\" for %s (true/false)", value ? value : "",
                           key);
      *status = ADBC_STATUS_INVALID_ARGUMENT;
      return true;
    }
    *status = ADBC_STATUS_OK;
    return true;
  }
  // Whether a path is allowed is checked in AdbcDatabaseInit, not here: the
  // driver manager buffers database options and applies them in an unspecified
  // order, so allow_paths may well arrive after the key it guards.
  if (strcmp(key, ADBC_ODBC_OPTION_DELEGATE_DRIVER) == 0) {
    ReplaceString(&opts->driver, value);
    *status = ADBC_STATUS_OK;
    return true;
  }
  if (strcmp(key, ADBC_ODBC_OPTION_DELEGATE_SEARCH_PATH) == 0) {
    ReplaceString(&opts->search_path, value);
    *status = ADBC_STATUS_OK;
    return true;
  }
  // A native-driver option.  With delegation off it can never reach a native
  // driver, so say so now; otherwise it is held until AdbcDatabaseInit has
  // decided, and reported there if delegation did not happen after all.
  if (opts->mode == ODBC_DELEGATE_NEVER) return false;
  *status = AddPassThrough(opts, key, value ? value : "", error);
  return true;
}

bool OdbcDelegateGetOption(const struct OdbcDelegateOptions* opts, const char* key,
                           const char** out) {
  if (strcmp(key, ADBC_ODBC_OPTION_DELEGATED_TO) == 0) {
    // Empty until AdbcDatabaseInit has decided.
    *out = opts->delegated_to ? opts->delegated_to : "";
    return true;
  }
  if (strcmp(key, ADBC_ODBC_OPTION_DELEGATE) == 0) {
    *out = opts->mode == ODBC_DELEGATE_NEVER
               ? "never"
               : (opts->mode == ODBC_DELEGATE_ALWAYS ? "always" : "auto");
    return true;
  }
  if (strcmp(key, ADBC_ODBC_OPTION_DELEGATE_DRIVER) == 0) {
    *out = opts->driver ? opts->driver : "";
    return true;
  }
  if (strcmp(key, ADBC_ODBC_OPTION_DELEGATE_SEARCH_PATH) == 0) {
    *out = opts->search_path ? opts->search_path : "";
    return true;
  }
  if (strcmp(key, ADBC_ODBC_OPTION_DELEGATE_ALLOW_PATHS) == 0) {
    *out = opts->allow_paths ? "true" : "false";
    return true;
  }
  if (strcmp(key, ADBC_ODBC_OPTION_DELEGATE_LAST_ERROR) == 0) {
    *out = opts->last_error ? opts->last_error : "";
    return true;
  }
  return false;
}

// ---------------------------------------------------------------------------
// The native driver behind a delegated database

struct OdbcDelegateProxy {
  struct AdbcDriver* native;  // the loaded (manager-wrapped) function table
  struct AdbcDatabase db;     // the native driver's own database handle
  int version;                // ADBC version the table was initialized with
  char* name;                 // for adbc.odbc.delegated_to
};

struct OdbcProxyConnection {
  struct OdbcDelegateProxy* proxy;
  struct AdbcConnection conn;
};

struct OdbcProxyStatement {
  struct OdbcDelegateProxy* proxy;
  struct AdbcStatement stmt;
};

/// Copy a native driver's error into the caller's, then release it.  The native
/// error's private_data belongs to the native driver's error vtable, which the
/// caller (dispatching through *our* table) would never call, so details are
/// copied across rather than handed over.
static AdbcStatusCode ProxyFinish(struct OdbcDelegateProxy* proxy, AdbcStatusCode status,
                                  struct AdbcError* native_error, struct AdbcError* error) {
  if (status != ADBC_STATUS_OK && error) {
    if (native_error->message) {
      InternalAdbcSetError(error, "%s", native_error->message);
    } else if (status == ADBC_STATUS_NOT_IMPLEMENTED) {
      // A driver is free to decline an entry point without saying anything about it.
      InternalAdbcSetError(error, "the native \"%s\" driver does not implement this operation",
                           proxy->name);
    } else {
      InternalAdbcSetError(error, "the native \"%s\" driver failed", proxy->name);
    }
    memcpy(error->sqlstate, native_error->sqlstate, sizeof(error->sqlstate));
    // ADBC_ERROR_INIT sets vendor_code to ADBC_ERROR_VENDOR_CODE_PRIVATE_DATA before the
    // call: that is the *caller* opting in to extended errors, not the driver promising
    // to have written one.  A driver that returns a status without touching the error at
    // all -- which is exactly what adbc_driver_postgresql does for the entry points it
    // has not implemented -- leaves that value in place with private_data still NULL, and
    // asking such a driver for the error's details makes it dereference the state it
    // never created.  A populated error always has a release; an untouched one never
    // does, so that is what says whether there is anything to ask about.
    if (native_error->vendor_code != ADBC_ERROR_VENDOR_CODE_PRIVATE_DATA) {
      error->vendor_code = native_error->vendor_code;
    } else if (native_error->release && proxy->version >= ADBC_VERSION_1_1_0 &&
               proxy->native->ErrorGetDetailCount && proxy->native->ErrorGetDetail) {
      int count = proxy->native->ErrorGetDetailCount(native_error);
      for (int i = 0; i < count; i++) {
        struct AdbcErrorDetail detail = proxy->native->ErrorGetDetail(native_error, i);
        if (detail.key) {
          InternalAdbcAppendErrorDetail(error, detail.key, detail.value, detail.value_length);
        }
      }
    }
  }
  if (native_error->release) native_error->release(native_error);
  return status;
}

/// The native driver does not implement this 1.1.0 entry point.
static AdbcStatusCode ProxyMissing(const struct OdbcDelegateProxy* proxy, const char* what,
                                   struct AdbcError* error) {
  InternalAdbcSetError(error, "the native \"%s\" driver does not implement %s", proxy->name, what);
  return ADBC_STATUS_NOT_IMPLEMENTED;
}

void OdbcDelegateProxyRelease(struct OdbcDelegateProxy* proxy) {
  if (!proxy) return;
  if (proxy->native) {
    if (proxy->db.private_data) (void)proxy->native->DatabaseRelease(&proxy->db, NULL);
    // Releases the driver manager's own per-driver state and its dlopen handle.
    if (proxy->native->release) (void)proxy->native->release(proxy->native, NULL);
    free(proxy->native);
  }
  free(proxy->name);
  free(proxy);
}

// ---------------------------------------------------------------------------
// The hand-over

/// Give up on delegation: record why, and only fail hard in "always" mode.
static AdbcStatusCode DelegateGiveUp(struct OdbcDelegateOptions* opts, char* why,
                                     AdbcStatusCode code, struct AdbcError* error) {
  if (why) {
    ReplaceString(&opts->last_error, why);
    free(why);
  }
  ReplaceString(&opts->delegated_to, ADBC_ODBC_DELEGATED_TO_ODBC);
  if (opts->mode == ODBC_DELEGATE_ALWAYS) {
    InternalAdbcSetError(error, "%s=always but %s", ADBC_ODBC_OPTION_DELEGATE,
                         opts->last_error ? opts->last_error : "delegation failed");
    return code;
  }
  return ADBC_STATUS_OK;
}

AdbcStatusCode OdbcDelegateTryInit(struct AdbcDatabase* database,
                                   AdbcStatusCode (*self_database_init)(struct AdbcDatabase*,
                                                                        struct AdbcError*),
                                   const struct OdbcDelegateTarget* target,
                                   struct OdbcDelegateOptions* opts,
                                   struct OdbcDelegateProxy** out_proxy, struct AdbcError* error) {
  (void)database;
  *out_proxy = NULL;
  // Whatever happens below, the decision is made now: later option changes are
  // rejected rather than silently ignored.
  opts->initialized = true;
  if (opts->mode == ODBC_DELEGATE_NEVER) {
    ReplaceString(&opts->delegated_to, ADBC_ODBC_DELEGATED_TO_ODBC);
    ReplaceString(&opts->last_error, "delegation is off (" ADBC_ODBC_OPTION_DELEGATE "=never)");
    return ADBC_STATUS_OK;
  }
  // Loading a shared library from a path a *database option* named would turn
  // this driver into an arbitrary code loader for every host that forwards
  // caller-supplied options, so it takes an explicit opt-in.  A misconfiguration
  // is reported rather than quietly ignored, in every mode.
  if (!opts->allow_paths) {
    const char* offender = NULL;
    if (opts->driver && LooksLikePath(opts->driver)) {
      offender = ADBC_ODBC_OPTION_DELEGATE_DRIVER;
    } else if (opts->search_path && *opts->search_path) {
      offender = ADBC_ODBC_OPTION_DELEGATE_SEARCH_PATH;
    }
    if (offender) {
      InternalAdbcSetError(
          error,
          "%s names a filesystem path; only a bare driver name (letters, digits, '_' and '-') "
          "such as \"postgresql\" is accepted by default. Loading a shared library by path is "
          "opt-in: set %s=true or %s=1.",
          offender, ADBC_ODBC_OPTION_DELEGATE_ALLOW_PATHS, ADBC_ODBC_DELEGATE_ALLOW_PATHS_ENV);
      ReplaceString(&opts->delegated_to, ADBC_ODBC_DELEGATED_TO_ODBC);
      ReplaceString(&opts->last_error, "delegation by path is not allowed");
      return ADBC_STATUS_INVALID_ARGUMENT;
    }
  }

  struct NativeTarget target_info;
  char* why = NULL;
  if (!DetectNative(target, opts, &target_info, &why)) {
    return DelegateGiveUp(opts, why, ADBC_STATUS_NOT_FOUND, error);
  }

#if defined(_WIN32)
  (void)self_database_init;
  NativeTargetFree(&target_info);
  return DelegateGiveUp(
      opts,
      DupString("native delegation is not implemented on Windows: the ADBC driver manager's "
                "loader is only resolved through dlopen/dlsym"),
      ADBC_STATUS_NOT_IMPLEMENTED, error);
#else
  struct LoadedObjectScan scan = {0};
  ScanLoadedObjects(&scan);
  struct StrList extra_dirs = {0};
  if (opts->allow_paths) StrListAddPathList(&extra_dirs, opts->search_path);
  // Unlike LD_LIBRARY_PATH, an environment variable of ours is not dropped by
  // the dynamic loader for setuid/setcap processes, so drop it here.
  if (!IsSecureExec()) StrListAddPathList(&extra_dirs, getenv(ADBC_ODBC_DELEGATE_PATH_ENV));

  AdbcLoadDriverFn load = DriverManagerLoadFn(&scan.managers);
  if (!load) {
    NativeTargetFree(&target_info);
    LoadedObjectScanFree(&scan);
    StrListFree(&extra_dirs);
    return DelegateGiveUp(
        opts, DupString("the ADBC driver manager's loader (AdbcLoadDriver) was not found"),
        ADBC_STATUS_NOT_FOUND, error);
  }

  struct AdbcDriver* native = calloc(1, sizeof(struct AdbcDriver));
  if (!native) {
    NativeTargetFree(&target_info);
    LoadedObjectScanFree(&scan);
    StrListFree(&extra_dirs);
    InternalAdbcSetError(error, "out of memory");
    return ADBC_STATUS_INTERNAL;
  }

  int version = ADBC_VERSION_1_1_0;
  bool loaded = LoadNativeDriver(load, target_info.driver_name,
                                 opts->allow_paths && LooksLikePath(target_info.driver_name),
                                 &scan, &extra_dirs, &version, native, &why);
  LoadedObjectScanFree(&scan);
  StrListFree(&extra_dirs);
  if (!loaded) {
    free(native);
    NativeTargetFree(&target_info);
    return DelegateGiveUp(opts, why, ADBC_STATUS_NOT_FOUND, error);
  }
  if (native->DatabaseInit == self_database_init) {
    // adbcbridge itself (by path, or by the "odbc" manifest name).  Handing the
    // database to ourselves would recurse and leave two drivers owning it.
    if (native->release) (void)native->release(native, NULL);
    free(native);
    NativeTargetFree(&target_info);
    return DelegateGiveUp(opts,
                          DupString("adbc.odbc.delegate.driver resolves to adbcbridge itself; "
                                    "refusing to delegate to this driver"),
                          ADBC_STATUS_INVALID_ARGUMENT, error);
  }

  struct OdbcDelegateProxy* proxy = calloc(1, sizeof(struct OdbcDelegateProxy));
  if (!proxy) {
    if (native->release) (void)native->release(native, NULL);
    free(native);
    NativeTargetFree(&target_info);
    InternalAdbcSetError(error, "out of memory");
    return ADBC_STATUS_INTERNAL;
  }
  proxy->native = native;
  proxy->version = version;
  proxy->name = target_info.driver_name;
  target_info.driver_name = NULL;

  // Stand a native database up with the translated options.  Nothing is handed
  // over until it is fully initialized, so a failure here is a clean fallback.
  struct AdbcError ne = ADBC_ERROR_INIT;
  AdbcStatusCode status = native->DatabaseNew(&proxy->db, &ne);
  if (status == ADBC_STATUS_OK) {
    status = native->DatabaseSetOption(&proxy->db, ADBC_OPTION_URI, target_info.uri, &ne);
  }
  // Credentials are folded into a URI we built ourselves; they only have to be
  // passed separately when the caller handed us a native URI verbatim.
  if (status == ADBC_STATUS_OK && target_info.uri_verbatim &&
      (target->username || target->password)) {
    if (target->username) {
      status = native->DatabaseSetOption(&proxy->db, ADBC_OPTION_USERNAME, target->username, &ne);
    }
    if (status == ADBC_STATUS_OK && target->password) {
      status = native->DatabaseSetOption(&proxy->db, ADBC_OPTION_PASSWORD, target->password, &ne);
    }
    if (status != ADBC_STATUS_OK) {
      // adbc_driver_postgresql and adbc_driver_sqlite only take "uri".  For a
      // libpq URI the credentials belong in it; for a file-backed database they
      // mean nothing at all, and quietly dropping them is not an option.
      if (ne.release) ne.release(&ne);
      ne = (struct AdbcError)ADBC_ERROR_INIT;
      if (target_info.family == FAMILY_POSTGRESQL) {
        struct KeyValueList creds = {0};
        if (target->username) KvAdd(&creds, "user", target->username);
        if (target->password) KvAdd(&creds, "password", target->password);
        char* merged = UriWithParams(target_info.uri, &creds);
        KvFree(&creds);
        status = merged ? native->DatabaseSetOption(&proxy->db, ADBC_OPTION_URI, merged, &ne)
                        : ADBC_STATUS_INTERNAL;
        free(merged);
      } else {
        struct InternalAdbcStringBuilder sb;
        InternalAdbcStringBuilderInit(&sb, 128);
        InternalAdbcStringBuilderAppend(
            &sb,
            "the native \"%s\" driver does not accept a separate username/password; put the "
            "credentials in the URI",
            proxy->name);
        why = DupString(sb.buffer);
        InternalAdbcStringBuilderReset(&sb);
      }
    }
  }
  for (size_t i = 0; status == ADBC_STATUS_OK && i < opts->pass_count; i++) {
    status = native->DatabaseSetOption(&proxy->db, opts->pass_keys[i], opts->pass_values[i], &ne);
  }
  if (status == ADBC_STATUS_OK && !why) { status = native->DatabaseInit(&proxy->db, &ne); }

  if (status != ADBC_STATUS_OK || why) {
    struct InternalAdbcStringBuilder sb;
    InternalAdbcStringBuilderInit(&sb, 128);
    if (why) {
      InternalAdbcStringBuilderAppend(&sb, "%s", why);
      free(why);
      why = NULL;
      if (status == ADBC_STATUS_OK) status = ADBC_STATUS_INVALID_ARGUMENT;
    } else {
      InternalAdbcStringBuilderAppend(&sb, "the native \"%s\" driver rejected the target",
                                      proxy->name);
      if (ne.message) InternalAdbcStringBuilderAppend(&sb, ": %s", ne.message);
    }
    why = DupString(sb.buffer);
    InternalAdbcStringBuilderReset(&sb);
    bool verbatim = target_info.uri_verbatim;
    int32_t sqlstate[5];
    memcpy(sqlstate, ne.sqlstate, sizeof(sqlstate));
    if (ne.release) ne.release(&ne);
    OdbcDelegateProxyRelease(proxy);
    NativeTargetFree(&target_info);
    if (verbatim) {
      // The caller asked for this URI by name and the native driver that owns
      // it answered.  ODBC cannot parse "postgresql://..." at all, so falling
      // back would replace a real diagnostic ("password authentication failed")
      // with unixODBC's "[IM002] Data source name not found".
      ReplaceString(&opts->last_error, why);
      ReplaceString(&opts->delegated_to, ADBC_ODBC_DELEGATED_TO_ODBC);
      InternalAdbcSetError(error, "%s", why);
      if (error) memcpy(error->sqlstate, sqlstate, sizeof(sqlstate));
      free(why);
      return status;
    }
    return DelegateGiveUp(opts, why, status, error);
  }
  if (ne.release) ne.release(&ne);
  NativeTargetFree(&target_info);
  ReplaceString(&opts->delegated_to, proxy->name);
  *out_proxy = proxy;
  return ADBC_STATUS_OK;
#endif
}

// ---------------------------------------------------------------------------
// Forwarding
//
// One indirection per ADBC call and nothing per row: result sets are the native
// driver's own ArrowArrayStream, handed to the caller untouched.

#define PROXY_ERROR struct AdbcError ne = ADBC_ERROR_INIT

AdbcStatusCode OdbcProxyDatabaseSetOption(struct OdbcDelegateProxy* p, const char* key,
                                          const char* value, struct AdbcError* error) {
  PROXY_ERROR;
  return ProxyFinish(p, p->native->DatabaseSetOption(&p->db, key, value, &ne), &ne, error);
}

AdbcStatusCode OdbcProxyDatabaseSetOptionInt(struct OdbcDelegateProxy* p, const char* key,
                                             int64_t value, struct AdbcError* error) {
  PROXY_ERROR;
  if (p->version < ADBC_VERSION_1_1_0 || !p->native->DatabaseSetOptionInt) {
    return ProxyMissing(p, "DatabaseSetOptionInt", error);
  }
  return ProxyFinish(p, p->native->DatabaseSetOptionInt(&p->db, key, value, &ne), &ne, error);
}

AdbcStatusCode OdbcProxyDatabaseSetOptionDouble(struct OdbcDelegateProxy* p, const char* key,
                                                double value, struct AdbcError* error) {
  PROXY_ERROR;
  if (p->version < ADBC_VERSION_1_1_0 || !p->native->DatabaseSetOptionDouble) {
    return ProxyMissing(p, "DatabaseSetOptionDouble", error);
  }
  return ProxyFinish(p, p->native->DatabaseSetOptionDouble(&p->db, key, value, &ne), &ne, error);
}

AdbcStatusCode OdbcProxyDatabaseSetOptionBytes(struct OdbcDelegateProxy* p, const char* key,
                                               const uint8_t* value, size_t length,
                                               struct AdbcError* error) {
  PROXY_ERROR;
  if (p->version < ADBC_VERSION_1_1_0 || !p->native->DatabaseSetOptionBytes) {
    return ProxyMissing(p, "DatabaseSetOptionBytes", error);
  }
  return ProxyFinish(p, p->native->DatabaseSetOptionBytes(&p->db, key, value, length, &ne), &ne,
                     error);
}

AdbcStatusCode OdbcProxyDatabaseGetOption(struct OdbcDelegateProxy* p, const char* key,
                                          char* value, size_t* length, struct AdbcError* error) {
  PROXY_ERROR;
  if (p->version < ADBC_VERSION_1_1_0 || !p->native->DatabaseGetOption) {
    return ProxyMissing(p, "DatabaseGetOption", error);
  }
  return ProxyFinish(p, p->native->DatabaseGetOption(&p->db, key, value, length, &ne), &ne, error);
}

AdbcStatusCode OdbcProxyDatabaseGetOptionInt(struct OdbcDelegateProxy* p, const char* key,
                                             int64_t* value, struct AdbcError* error) {
  PROXY_ERROR;
  if (p->version < ADBC_VERSION_1_1_0 || !p->native->DatabaseGetOptionInt) {
    return ProxyMissing(p, "DatabaseGetOptionInt", error);
  }
  return ProxyFinish(p, p->native->DatabaseGetOptionInt(&p->db, key, value, &ne), &ne, error);
}

AdbcStatusCode OdbcProxyDatabaseGetOptionDouble(struct OdbcDelegateProxy* p, const char* key,
                                                double* value, struct AdbcError* error) {
  PROXY_ERROR;
  if (p->version < ADBC_VERSION_1_1_0 || !p->native->DatabaseGetOptionDouble) {
    return ProxyMissing(p, "DatabaseGetOptionDouble", error);
  }
  return ProxyFinish(p, p->native->DatabaseGetOptionDouble(&p->db, key, value, &ne), &ne, error);
}

AdbcStatusCode OdbcProxyDatabaseGetOptionBytes(struct OdbcDelegateProxy* p, const char* key,
                                               uint8_t* value, size_t* length,
                                               struct AdbcError* error) {
  PROXY_ERROR;
  if (p->version < ADBC_VERSION_1_1_0 || !p->native->DatabaseGetOptionBytes) {
    return ProxyMissing(p, "DatabaseGetOptionBytes", error);
  }
  return ProxyFinish(p, p->native->DatabaseGetOptionBytes(&p->db, key, value, length, &ne), &ne,
                     error);
}

/// Replay one held connection option on the native connection.  A typed setter
/// the native driver does not have is reported as such: dropping the option
/// silently would be worse.
static AdbcStatusCode ProxyReplayPreOption(struct OdbcDelegateProxy* p,
                                           struct AdbcConnection* conn,
                                           const struct OdbcPreOption* opt,
                                           struct AdbcError* ne, const char** missing) {
  switch (opt->type) {
    case ODBC_PRE_OPTION_INT:
      if (p->version < ADBC_VERSION_1_1_0 || !p->native->ConnectionSetOptionInt) break;
      return p->native->ConnectionSetOptionInt(conn, opt->key, opt->number, ne);
    case ODBC_PRE_OPTION_DOUBLE:
      if (p->version < ADBC_VERSION_1_1_0 || !p->native->ConnectionSetOptionDouble) break;
      return p->native->ConnectionSetOptionDouble(conn, opt->key, opt->real, ne);
    case ODBC_PRE_OPTION_BYTES:
      if (p->version < ADBC_VERSION_1_1_0 || !p->native->ConnectionSetOptionBytes) break;
      return p->native->ConnectionSetOptionBytes(conn, opt->key, opt->bytes, opt->length, ne);
    case ODBC_PRE_OPTION_STRING:
    default:
      return p->native->ConnectionSetOption(conn, opt->key, opt->value, ne);
  }
  *missing = opt->type == ODBC_PRE_OPTION_INT      ? "ConnectionSetOptionInt"
             : opt->type == ODBC_PRE_OPTION_DOUBLE ? "ConnectionSetOptionDouble"
                                                   : "ConnectionSetOptionBytes";
  return ADBC_STATUS_NOT_IMPLEMENTED;
}

AdbcStatusCode OdbcProxyConnectionInit(struct OdbcDelegateProxy* p,
                                       const struct OdbcPreOption* pre, size_t pre_count,
                                       struct OdbcProxyConnection** out,
                                       struct AdbcError* error) {
  PROXY_ERROR;
  struct OdbcProxyConnection* conn = calloc(1, sizeof(struct OdbcProxyConnection));
  if (!conn) {
    InternalAdbcSetError(error, "out of memory");
    return ADBC_STATUS_INTERNAL;
  }
  conn->proxy = p;
  const char* missing = NULL;
  AdbcStatusCode status = p->native->ConnectionNew(&conn->conn, &ne);
  for (size_t i = 0; status == ADBC_STATUS_OK && i < pre_count; i++) {
    status = ProxyReplayPreOption(p, &conn->conn, &pre[i], &ne, &missing);
  }
  if (status == ADBC_STATUS_OK) {
    status = p->native->ConnectionInit(&conn->conn, &p->db, &ne);
  }
  if (status != ADBC_STATUS_OK) {
    if (conn->conn.private_data) (void)p->native->ConnectionRelease(&conn->conn, NULL);
    free(conn);
    if (missing) {
      if (ne.release) ne.release(&ne);
      return ProxyMissing(p, missing, error);
    }
    return ProxyFinish(p, status, &ne, error);
  }
  if (ne.release) ne.release(&ne);
  *out = conn;
  return ADBC_STATUS_OK;
}

const char* OdbcProxyConnectionName(const struct OdbcProxyConnection* conn) {
  return conn->proxy->name;
}

AdbcStatusCode OdbcProxyConnectionRelease(struct OdbcProxyConnection* conn,
                                          struct AdbcError* error) {
  struct OdbcDelegateProxy* p = conn->proxy;
  PROXY_ERROR;
  AdbcStatusCode status = ADBC_STATUS_OK;
  if (conn->conn.private_data) status = p->native->ConnectionRelease(&conn->conn, &ne);
  free(conn);
  return ProxyFinish(p, status, &ne, error);
}

AdbcStatusCode OdbcProxyConnectionCommit(struct OdbcProxyConnection* conn,
                                         struct AdbcError* error) {
  struct OdbcDelegateProxy* p = conn->proxy;
  PROXY_ERROR;
  return ProxyFinish(p, p->native->ConnectionCommit(&conn->conn, &ne), &ne, error);
}

AdbcStatusCode OdbcProxyConnectionRollback(struct OdbcProxyConnection* conn,
                                           struct AdbcError* error) {
  struct OdbcDelegateProxy* p = conn->proxy;
  PROXY_ERROR;
  return ProxyFinish(p, p->native->ConnectionRollback(&conn->conn, &ne), &ne, error);
}

AdbcStatusCode OdbcProxyConnectionCancel(struct OdbcProxyConnection* conn,
                                         struct AdbcError* error) {
  struct OdbcDelegateProxy* p = conn->proxy;
  PROXY_ERROR;
  if (p->version < ADBC_VERSION_1_1_0 || !p->native->ConnectionCancel) {
    return ProxyMissing(p, "ConnectionCancel", error);
  }
  return ProxyFinish(p, p->native->ConnectionCancel(&conn->conn, &ne), &ne, error);
}

AdbcStatusCode OdbcProxyConnectionGetInfo(struct OdbcProxyConnection* conn,
                                          const uint32_t* info_codes, size_t info_codes_length,
                                          struct ArrowArrayStream* out, struct AdbcError* error) {
  struct OdbcDelegateProxy* p = conn->proxy;
  PROXY_ERROR;
  return ProxyFinish(
      p, p->native->ConnectionGetInfo(&conn->conn, info_codes, info_codes_length, out, &ne), &ne,
      error);
}

AdbcStatusCode OdbcProxyConnectionGetObjects(struct OdbcProxyConnection* conn, int depth,
                                             const char* catalog, const char* db_schema,
                                             const char* table_name, const char** table_type,
                                             const char* column_name,
                                             struct ArrowArrayStream* out,
                                             struct AdbcError* error) {
  struct OdbcDelegateProxy* p = conn->proxy;
  PROXY_ERROR;
  return ProxyFinish(p,
                     p->native->ConnectionGetObjects(&conn->conn, depth, catalog, db_schema,
                                                     table_name, table_type, column_name, out, &ne),
                     &ne, error);
}

AdbcStatusCode OdbcProxyConnectionGetTableSchema(struct OdbcProxyConnection* conn,
                                                 const char* catalog, const char* db_schema,
                                                 const char* table_name,
                                                 struct ArrowSchema* schema,
                                                 struct AdbcError* error) {
  struct OdbcDelegateProxy* p = conn->proxy;
  PROXY_ERROR;
  return ProxyFinish(p,
                     p->native->ConnectionGetTableSchema(&conn->conn, catalog, db_schema,
                                                         table_name, schema, &ne),
                     &ne, error);
}

AdbcStatusCode OdbcProxyConnectionGetTableTypes(struct OdbcProxyConnection* conn,
                                                struct ArrowArrayStream* out,
                                                struct AdbcError* error) {
  struct OdbcDelegateProxy* p = conn->proxy;
  PROXY_ERROR;
  return ProxyFinish(p, p->native->ConnectionGetTableTypes(&conn->conn, out, &ne), &ne, error);
}

AdbcStatusCode OdbcProxyConnectionGetStatistics(struct OdbcProxyConnection* conn,
                                                const char* catalog, const char* db_schema,
                                                const char* table_name, char approximate,
                                                struct ArrowArrayStream* out,
                                                struct AdbcError* error) {
  struct OdbcDelegateProxy* p = conn->proxy;
  PROXY_ERROR;
  if (p->version < ADBC_VERSION_1_1_0 || !p->native->ConnectionGetStatistics) {
    return ProxyMissing(p, "ConnectionGetStatistics", error);
  }
  return ProxyFinish(p,
                     p->native->ConnectionGetStatistics(&conn->conn, catalog, db_schema,
                                                        table_name, approximate, out, &ne),
                     &ne, error);
}

AdbcStatusCode OdbcProxyConnectionGetStatisticNames(struct OdbcProxyConnection* conn,
                                                    struct ArrowArrayStream* out,
                                                    struct AdbcError* error) {
  struct OdbcDelegateProxy* p = conn->proxy;
  PROXY_ERROR;
  if (p->version < ADBC_VERSION_1_1_0 || !p->native->ConnectionGetStatisticNames) {
    return ProxyMissing(p, "ConnectionGetStatisticNames", error);
  }
  return ProxyFinish(p, p->native->ConnectionGetStatisticNames(&conn->conn, out, &ne), &ne, error);
}

AdbcStatusCode OdbcProxyConnectionReadPartition(struct OdbcProxyConnection* conn,
                                                const uint8_t* serialized_partition,
                                                size_t serialized_length,
                                                struct ArrowArrayStream* out,
                                                struct AdbcError* error) {
  struct OdbcDelegateProxy* p = conn->proxy;
  PROXY_ERROR;
  if (!p->native->ConnectionReadPartition) {
    return ProxyMissing(p, "ConnectionReadPartition", error);
  }
  return ProxyFinish(p,
                     p->native->ConnectionReadPartition(&conn->conn, serialized_partition,
                                                        serialized_length, out, &ne),
                     &ne, error);
}

AdbcStatusCode OdbcProxyConnectionSetOption(struct OdbcProxyConnection* conn, const char* key,
                                            const char* value, struct AdbcError* error) {
  struct OdbcDelegateProxy* p = conn->proxy;
  PROXY_ERROR;
  return ProxyFinish(p, p->native->ConnectionSetOption(&conn->conn, key, value, &ne), &ne, error);
}

AdbcStatusCode OdbcProxyConnectionSetOptionInt(struct OdbcProxyConnection* conn, const char* key,
                                               int64_t value, struct AdbcError* error) {
  struct OdbcDelegateProxy* p = conn->proxy;
  PROXY_ERROR;
  if (p->version < ADBC_VERSION_1_1_0 || !p->native->ConnectionSetOptionInt) {
    return ProxyMissing(p, "ConnectionSetOptionInt", error);
  }
  return ProxyFinish(p, p->native->ConnectionSetOptionInt(&conn->conn, key, value, &ne), &ne,
                     error);
}

AdbcStatusCode OdbcProxyConnectionSetOptionDouble(struct OdbcProxyConnection* conn,
                                                  const char* key, double value,
                                                  struct AdbcError* error) {
  struct OdbcDelegateProxy* p = conn->proxy;
  PROXY_ERROR;
  if (p->version < ADBC_VERSION_1_1_0 || !p->native->ConnectionSetOptionDouble) {
    return ProxyMissing(p, "ConnectionSetOptionDouble", error);
  }
  return ProxyFinish(p, p->native->ConnectionSetOptionDouble(&conn->conn, key, value, &ne), &ne,
                     error);
}

AdbcStatusCode OdbcProxyConnectionSetOptionBytes(struct OdbcProxyConnection* conn, const char* key,
                                                 const uint8_t* value, size_t length,
                                                 struct AdbcError* error) {
  struct OdbcDelegateProxy* p = conn->proxy;
  PROXY_ERROR;
  if (p->version < ADBC_VERSION_1_1_0 || !p->native->ConnectionSetOptionBytes) {
    return ProxyMissing(p, "ConnectionSetOptionBytes", error);
  }
  return ProxyFinish(p, p->native->ConnectionSetOptionBytes(&conn->conn, key, value, length, &ne),
                     &ne, error);
}

AdbcStatusCode OdbcProxyConnectionGetOption(struct OdbcProxyConnection* conn, const char* key,
                                            char* value, size_t* length,
                                            struct AdbcError* error) {
  struct OdbcDelegateProxy* p = conn->proxy;
  PROXY_ERROR;
  if (p->version < ADBC_VERSION_1_1_0 || !p->native->ConnectionGetOption) {
    return ProxyMissing(p, "ConnectionGetOption", error);
  }
  return ProxyFinish(p, p->native->ConnectionGetOption(&conn->conn, key, value, length, &ne), &ne,
                     error);
}

AdbcStatusCode OdbcProxyConnectionGetOptionInt(struct OdbcProxyConnection* conn, const char* key,
                                               int64_t* value, struct AdbcError* error) {
  struct OdbcDelegateProxy* p = conn->proxy;
  PROXY_ERROR;
  if (p->version < ADBC_VERSION_1_1_0 || !p->native->ConnectionGetOptionInt) {
    return ProxyMissing(p, "ConnectionGetOptionInt", error);
  }
  return ProxyFinish(p, p->native->ConnectionGetOptionInt(&conn->conn, key, value, &ne), &ne,
                     error);
}

AdbcStatusCode OdbcProxyConnectionGetOptionDouble(struct OdbcProxyConnection* conn,
                                                  const char* key, double* value,
                                                  struct AdbcError* error) {
  struct OdbcDelegateProxy* p = conn->proxy;
  PROXY_ERROR;
  if (p->version < ADBC_VERSION_1_1_0 || !p->native->ConnectionGetOptionDouble) {
    return ProxyMissing(p, "ConnectionGetOptionDouble", error);
  }
  return ProxyFinish(p, p->native->ConnectionGetOptionDouble(&conn->conn, key, value, &ne), &ne,
                     error);
}

AdbcStatusCode OdbcProxyConnectionGetOptionBytes(struct OdbcProxyConnection* conn, const char* key,
                                                 uint8_t* value, size_t* length,
                                                 struct AdbcError* error) {
  struct OdbcDelegateProxy* p = conn->proxy;
  PROXY_ERROR;
  if (p->version < ADBC_VERSION_1_1_0 || !p->native->ConnectionGetOptionBytes) {
    return ProxyMissing(p, "ConnectionGetOptionBytes", error);
  }
  return ProxyFinish(p, p->native->ConnectionGetOptionBytes(&conn->conn, key, value, length, &ne),
                     &ne, error);
}

AdbcStatusCode OdbcProxyStatementNew(struct OdbcProxyConnection* conn,
                                     struct OdbcProxyStatement** out, struct AdbcError* error) {
  struct OdbcDelegateProxy* p = conn->proxy;
  PROXY_ERROR;
  struct OdbcProxyStatement* stmt = calloc(1, sizeof(struct OdbcProxyStatement));
  if (!stmt) {
    InternalAdbcSetError(error, "out of memory");
    return ADBC_STATUS_INTERNAL;
  }
  stmt->proxy = p;
  AdbcStatusCode status = p->native->StatementNew(&conn->conn, &stmt->stmt, &ne);
  if (status != ADBC_STATUS_OK) {
    free(stmt);
    return ProxyFinish(p, status, &ne, error);
  }
  if (ne.release) ne.release(&ne);
  *out = stmt;
  return ADBC_STATUS_OK;
}

AdbcStatusCode OdbcProxyStatementRelease(struct OdbcProxyStatement* stmt,
                                         struct AdbcError* error) {
  struct OdbcDelegateProxy* p = stmt->proxy;
  PROXY_ERROR;
  AdbcStatusCode status = ADBC_STATUS_OK;
  if (stmt->stmt.private_data) status = p->native->StatementRelease(&stmt->stmt, &ne);
  free(stmt);
  return ProxyFinish(p, status, &ne, error);
}

AdbcStatusCode OdbcProxyStatementBind(struct OdbcProxyStatement* stmt, struct ArrowArray* values,
                                      struct ArrowSchema* schema, struct AdbcError* error) {
  struct OdbcDelegateProxy* p = stmt->proxy;
  PROXY_ERROR;
  return ProxyFinish(p, p->native->StatementBind(&stmt->stmt, values, schema, &ne), &ne, error);
}

AdbcStatusCode OdbcProxyStatementBindStream(struct OdbcProxyStatement* stmt,
                                            struct ArrowArrayStream* stream,
                                            struct AdbcError* error) {
  struct OdbcDelegateProxy* p = stmt->proxy;
  PROXY_ERROR;
  return ProxyFinish(p, p->native->StatementBindStream(&stmt->stmt, stream, &ne), &ne, error);
}

AdbcStatusCode OdbcProxyStatementExecuteQuery(struct OdbcProxyStatement* stmt,
                                              struct ArrowArrayStream* out,
                                              int64_t* rows_affected, struct AdbcError* error) {
  struct OdbcDelegateProxy* p = stmt->proxy;
  PROXY_ERROR;
  return ProxyFinish(p, p->native->StatementExecuteQuery(&stmt->stmt, out, rows_affected, &ne),
                     &ne, error);
}

AdbcStatusCode OdbcProxyStatementExecutePartitions(struct OdbcProxyStatement* stmt,
                                                   struct ArrowSchema* schema,
                                                   struct AdbcPartitions* partitions,
                                                   int64_t* rows_affected,
                                                   struct AdbcError* error) {
  struct OdbcDelegateProxy* p = stmt->proxy;
  PROXY_ERROR;
  if (!p->native->StatementExecutePartitions) {
    return ProxyMissing(p, "StatementExecutePartitions", error);
  }
  return ProxyFinish(
      p, p->native->StatementExecutePartitions(&stmt->stmt, schema, partitions, rows_affected, &ne),
      &ne, error);
}

AdbcStatusCode OdbcProxyStatementExecuteSchema(struct OdbcProxyStatement* stmt,
                                               struct ArrowSchema* schema,
                                               struct AdbcError* error) {
  struct OdbcDelegateProxy* p = stmt->proxy;
  PROXY_ERROR;
  if (p->version < ADBC_VERSION_1_1_0 || !p->native->StatementExecuteSchema) {
    return ProxyMissing(p, "StatementExecuteSchema", error);
  }
  return ProxyFinish(p, p->native->StatementExecuteSchema(&stmt->stmt, schema, &ne), &ne, error);
}

AdbcStatusCode OdbcProxyStatementGetParameterSchema(struct OdbcProxyStatement* stmt,
                                                    struct ArrowSchema* schema,
                                                    struct AdbcError* error) {
  struct OdbcDelegateProxy* p = stmt->proxy;
  PROXY_ERROR;
  if (!p->native->StatementGetParameterSchema) {
    return ProxyMissing(p, "StatementGetParameterSchema", error);
  }
  return ProxyFinish(p, p->native->StatementGetParameterSchema(&stmt->stmt, schema, &ne), &ne,
                     error);
}

AdbcStatusCode OdbcProxyStatementPrepare(struct OdbcProxyStatement* stmt,
                                         struct AdbcError* error) {
  struct OdbcDelegateProxy* p = stmt->proxy;
  PROXY_ERROR;
  return ProxyFinish(p, p->native->StatementPrepare(&stmt->stmt, &ne), &ne, error);
}

AdbcStatusCode OdbcProxyStatementCancel(struct OdbcProxyStatement* stmt, struct AdbcError* error) {
  struct OdbcDelegateProxy* p = stmt->proxy;
  PROXY_ERROR;
  if (p->version < ADBC_VERSION_1_1_0 || !p->native->StatementCancel) {
    return ProxyMissing(p, "StatementCancel", error);
  }
  return ProxyFinish(p, p->native->StatementCancel(&stmt->stmt, &ne), &ne, error);
}

AdbcStatusCode OdbcProxyStatementSetSqlQuery(struct OdbcProxyStatement* stmt, const char* query,
                                             struct AdbcError* error) {
  struct OdbcDelegateProxy* p = stmt->proxy;
  PROXY_ERROR;
  return ProxyFinish(p, p->native->StatementSetSqlQuery(&stmt->stmt, query, &ne), &ne, error);
}

AdbcStatusCode OdbcProxyStatementSetSubstraitPlan(struct OdbcProxyStatement* stmt,
                                                  const uint8_t* plan, size_t length,
                                                  struct AdbcError* error) {
  struct OdbcDelegateProxy* p = stmt->proxy;
  PROXY_ERROR;
  if (!p->native->StatementSetSubstraitPlan) {
    return ProxyMissing(p, "StatementSetSubstraitPlan", error);
  }
  return ProxyFinish(p, p->native->StatementSetSubstraitPlan(&stmt->stmt, plan, length, &ne), &ne,
                     error);
}

AdbcStatusCode OdbcProxyStatementSetOption(struct OdbcProxyStatement* stmt, const char* key,
                                           const char* value, struct AdbcError* error) {
  struct OdbcDelegateProxy* p = stmt->proxy;
  PROXY_ERROR;
  return ProxyFinish(p, p->native->StatementSetOption(&stmt->stmt, key, value, &ne), &ne, error);
}

AdbcStatusCode OdbcProxyStatementSetOptionInt(struct OdbcProxyStatement* stmt, const char* key,
                                              int64_t value, struct AdbcError* error) {
  struct OdbcDelegateProxy* p = stmt->proxy;
  PROXY_ERROR;
  if (p->version < ADBC_VERSION_1_1_0 || !p->native->StatementSetOptionInt) {
    return ProxyMissing(p, "StatementSetOptionInt", error);
  }
  return ProxyFinish(p, p->native->StatementSetOptionInt(&stmt->stmt, key, value, &ne), &ne, error);
}

AdbcStatusCode OdbcProxyStatementSetOptionDouble(struct OdbcProxyStatement* stmt, const char* key,
                                                 double value, struct AdbcError* error) {
  struct OdbcDelegateProxy* p = stmt->proxy;
  PROXY_ERROR;
  if (p->version < ADBC_VERSION_1_1_0 || !p->native->StatementSetOptionDouble) {
    return ProxyMissing(p, "StatementSetOptionDouble", error);
  }
  return ProxyFinish(p, p->native->StatementSetOptionDouble(&stmt->stmt, key, value, &ne), &ne,
                     error);
}

AdbcStatusCode OdbcProxyStatementSetOptionBytes(struct OdbcProxyStatement* stmt, const char* key,
                                                const uint8_t* value, size_t length,
                                                struct AdbcError* error) {
  struct OdbcDelegateProxy* p = stmt->proxy;
  PROXY_ERROR;
  if (p->version < ADBC_VERSION_1_1_0 || !p->native->StatementSetOptionBytes) {
    return ProxyMissing(p, "StatementSetOptionBytes", error);
  }
  return ProxyFinish(p, p->native->StatementSetOptionBytes(&stmt->stmt, key, value, length, &ne),
                     &ne, error);
}

AdbcStatusCode OdbcProxyStatementGetOption(struct OdbcProxyStatement* stmt, const char* key,
                                           char* value, size_t* length, struct AdbcError* error) {
  struct OdbcDelegateProxy* p = stmt->proxy;
  PROXY_ERROR;
  if (p->version < ADBC_VERSION_1_1_0 || !p->native->StatementGetOption) {
    return ProxyMissing(p, "StatementGetOption", error);
  }
  return ProxyFinish(p, p->native->StatementGetOption(&stmt->stmt, key, value, length, &ne), &ne,
                     error);
}

AdbcStatusCode OdbcProxyStatementGetOptionInt(struct OdbcProxyStatement* stmt, const char* key,
                                              int64_t* value, struct AdbcError* error) {
  struct OdbcDelegateProxy* p = stmt->proxy;
  PROXY_ERROR;
  if (p->version < ADBC_VERSION_1_1_0 || !p->native->StatementGetOptionInt) {
    return ProxyMissing(p, "StatementGetOptionInt", error);
  }
  return ProxyFinish(p, p->native->StatementGetOptionInt(&stmt->stmt, key, value, &ne), &ne, error);
}

AdbcStatusCode OdbcProxyStatementGetOptionDouble(struct OdbcProxyStatement* stmt, const char* key,
                                                 double* value, struct AdbcError* error) {
  struct OdbcDelegateProxy* p = stmt->proxy;
  PROXY_ERROR;
  if (p->version < ADBC_VERSION_1_1_0 || !p->native->StatementGetOptionDouble) {
    return ProxyMissing(p, "StatementGetOptionDouble", error);
  }
  return ProxyFinish(p, p->native->StatementGetOptionDouble(&stmt->stmt, key, value, &ne), &ne,
                     error);
}

AdbcStatusCode OdbcProxyStatementGetOptionBytes(struct OdbcProxyStatement* stmt, const char* key,
                                                uint8_t* value, size_t* length,
                                                struct AdbcError* error) {
  struct OdbcDelegateProxy* p = stmt->proxy;
  PROXY_ERROR;
  if (p->version < ADBC_VERSION_1_1_0 || !p->native->StatementGetOptionBytes) {
    return ProxyMissing(p, "StatementGetOptionBytes", error);
  }
  return ProxyFinish(p, p->native->StatementGetOptionBytes(&stmt->stmt, key, value, length, &ne),
                     &ne, error);
}

#undef PROXY_ERROR
