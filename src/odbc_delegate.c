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
#include <unistd.h>
#endif
#if defined(__linux__) || defined(__FreeBSD__)
#include <link.h>
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

/// A deduplicating list of owned strings.
struct StrList {
  char** items;
  size_t count;
  size_t capacity;
};

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

static void StrListFree(struct StrList* list) {
  for (size_t i = 0; i < list->count; i++) free(list->items[i]);
  free(list->items);
  memset(list, 0, sizeof(*list));
}

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

// ---------------------------------------------------------------------------
// ODBC connection string / odbc.ini keyword lookup

/// Fetch `key` from an ODBC connection string ("Driver={x};Server=y;").
/// Values may be brace-quoted.  Returns a malloc'd string or NULL.
static char* ConnStringGet(const char* conn, const char* key) {
  if (!conn || !key) return NULL;
  const char* p = conn;
  size_t keylen = strlen(key);
  while (*p) {
    while (*p == ';' || *p == ' ' || *p == '\t') p++;
    if (!*p) break;
    const char* eq = strchr(p, '=');
    if (!eq) break;
    // Trim trailing space in the keyword.
    const char* kend = eq;
    while (kend > p && (kend[-1] == ' ' || kend[-1] == '\t')) kend--;
    bool match = ((size_t)(kend - p) == keylen);
    if (match) {
      for (size_t i = 0; i < keylen; i++) {
        if (tolower((unsigned char)p[i]) != tolower((unsigned char)key[i])) {
          match = false;
          break;
        }
      }
    }
    const char* v = eq + 1;
    const char* vend;
    if (*v == '{') {
      v++;
      vend = strchr(v, '}');
      if (!vend) vend = v + strlen(v);
      p = *vend ? vend + 1 : vend;
    } else {
      vend = strchr(v, ';');
      if (!vend) vend = v + strlen(v);
      p = vend;
    }
    while (*p && *p != ';') p++;
    if (match) {
      size_t len = (size_t)(vend - v);
      char* out = malloc(len + 1);
      if (!out) return NULL;
      memcpy(out, v, len);
      out[len] = '\0';
      return out;
    }
  }
  return NULL;
}

#if !defined(_WIN32)
typedef int (*SQLGetPrivateProfileStringFn)(const char* section, const char* entry, const char* def,
                                            char* buffer, int buflen, const char* filename);

/// unixODBC/iODBC's odbc.ini reader, resolved lazily so that we do not have to
/// link libodbcinst.
static SQLGetPrivateProfileStringFn ProfileStringFn(void) {
  static SQLGetPrivateProfileStringFn cached = NULL;
  static bool tried = false;
  if (tried) return cached;
  tried = true;
  cached = (SQLGetPrivateProfileStringFn)dlsym(RTLD_DEFAULT, "SQLGetPrivateProfileString");
  if (cached) return cached;
  static const char* kLibs[] = {"libodbcinst.so.2", "libodbcinst.so", "libodbcinst.dylib",
                                "libiodbcinst.so.2", "libiodbcinst.so"};
  for (size_t i = 0; i < sizeof(kLibs) / sizeof(kLibs[0]); i++) {
    void* handle = dlopen(kLibs[i], RTLD_LAZY | RTLD_LOCAL);
    if (!handle) continue;
    cached = (SQLGetPrivateProfileStringFn)dlsym(handle, "SQLGetPrivateProfileString");
    if (cached) return cached;
    dlclose(handle);
  }
  return cached;
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

/// Percent-encode a URI userinfo component.
static void AppendUserInfo(struct InternalAdbcStringBuilder* sb, const char* value) {
  static const char kHex[] = "0123456789ABCDEF";
  for (const unsigned char* p = (const unsigned char*)value; *p; p++) {
    if (isalnum(*p) || *p == '-' || *p == '.' || *p == '_' || *p == '~') {
      InternalAdbcStringBuilderAppend(sb, "%c", (char)*p);
    } else {
      InternalAdbcStringBuilderAppend(sb, "%%%c%c", kHex[*p >> 4], kHex[*p & 0x0F]);
    }
  }
}

/// A file path taken from a URI-ish string: strip "//" but keep "file:" URIs.
static char* PathFromUriRest(const char* rest) {
  if (!rest) return NULL;
  if (strncmp(rest, "//", 2) == 0) rest += 2;
  return DupString(rest);
}

/// Build the native connection URI for `family` from the ODBC keywords.
/// Returns NULL when the ODBC options do not carry enough information.
static char* BuildNativeUri(enum OdbcNativeFamily family, const char* conn, const char* dsn,
                            const char* username, const char* password) {
  static const char* kHostKeys[] = {"Server", "Servername", "Host", "Hostname", NULL};
  static const char* kPortKeys[] = {"Port", NULL};
  static const char* kDatabaseKeys[] = {"Database", "DB", "Data Source", "Dbname", NULL};
  static const char* kUserKeys[] = {"Uid", "UID", "User", "Username", "UserName", NULL};
  static const char* kPasswordKeys[] = {"Pwd", "PWD", "Password", NULL};

  char* database = KeywordGet(conn, dsn, kDatabaseKeys);
  char* uri = NULL;

  switch (family) {
    case FAMILY_SQLITE:
    case FAMILY_DUCKDB: {
      // The native SQLite/DuckDB drivers take the database file directly.
      if (database && *database) uri = DupString(database);
      break;
    }
    case FAMILY_POSTGRESQL: {
      char* host = KeywordGet(conn, dsn, kHostKeys);
      char* port = KeywordGet(conn, dsn, kPortKeys);
      char* user = username ? DupString(username) : KeywordGet(conn, dsn, kUserKeys);
      char* pass = password ? DupString(password) : KeywordGet(conn, dsn, kPasswordKeys);
      struct InternalAdbcStringBuilder sb;
      InternalAdbcStringBuilderInit(&sb, 128);
      InternalAdbcStringBuilderAppend(&sb, "postgresql://");
      if (user && *user) {
        AppendUserInfo(&sb, user);
        if (pass && *pass) {
          InternalAdbcStringBuilderAppend(&sb, ":");
          AppendUserInfo(&sb, pass);
        }
        InternalAdbcStringBuilderAppend(&sb, "@");
      }
      InternalAdbcStringBuilderAppend(&sb, "%s", (host && *host) ? host : "localhost");
      if (port && *port) InternalAdbcStringBuilderAppend(&sb, ":%s", port);
      InternalAdbcStringBuilderAppend(&sb, "/%s", database ? database : "");
      uri = DupString(sb.buffer);
      InternalAdbcStringBuilderReset(&sb);
      free(host);
      free(port);
      free(user);
      free(pass);
      break;
    }
    default:
      // Snowflake/BigQuery/Flight SQL URIs cannot be reconstructed from ODBC
      // keywords; the caller must pass a native URI.
      break;
  }
  free(database);
  return uri;
}

/// Work out which native driver fits, and the URI to hand it.
/// Returns false (with *why set) when nothing fits.
static bool DetectNative(const struct OdbcDelegateTarget* target,
                         const struct OdbcDelegateOptions* opts, char** out_driver, char** out_uri,
                         bool* out_uri_verbatim, char** why) {
  const char* rest = NULL;
  enum OdbcNativeFamily family = FamilyFromUri(target->connection_string, &rest);
  char* uri = NULL;
  // A DSN reaches us either as the "dsn" option or as DSN= in the connection string.
  char* dsn = (target->dsn && *target->dsn) ? DupString(target->dsn)
                                            : ConnStringGet(target->connection_string, "DSN");

  *out_uri_verbatim = family != FAMILY_NONE;
  if (family != FAMILY_NONE) {
    // A native-looking URI was passed straight through.
    if (family == FAMILY_SQLITE || family == FAMILY_DUCKDB) {
      uri = PathFromUriRest(rest);
    } else {
      uri = DupString(target->connection_string);
    }
  }

  if (opts->driver && *opts->driver) {
    // The native driver is forced; only the URI still has to be worked out.
    enum OdbcNativeFamily forced = FamilyFromDriverName(opts->driver);
    if (family == FAMILY_NONE) {
      family = forced;
      if (family == FAMILY_NONE) {
        free(uri);
        free(dsn);
        *why = DupString(
            "adbc.odbc.delegate.driver does not name a known database family; "
            "pass a native URI as \"uri\"");
        return false;
      }
      uri = BuildNativeUri(family, target->connection_string, dsn, target->username,
                           target->password);
    }
    free(dsn);
    if (!uri) {
      *why = DupString("could not build a native connection URI from the ODBC options");
      return false;
    }
    *out_driver = DupString(opts->driver);
    *out_uri = uri;
    return true;
  }

  if (family == FAMILY_NONE) {
    // An ODBC connection string or a DSN: look at Driver= / the DSN section.
    static const char* kDriverKeys[] = {"Driver", NULL};
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
    uri =
        BuildNativeUri(family, target->connection_string, dsn, target->username, target->password);
    free(dsn);
    dsn = NULL;
    if (!uri) {
      struct InternalAdbcStringBuilder sb;
      InternalAdbcStringBuilderInit(&sb, 128);
      InternalAdbcStringBuilderAppend(&sb, "could not build a native %s URI from the ODBC options",
                                      FamilyDriverName(family));
      *why = DupString(sb.buffer);
      InternalAdbcStringBuilderReset(&sb);
      return false;
    }
  }

  free(dsn);
  *out_driver = DupString(FamilyDriverName(family));
  *out_uri = uri;
  return true;
}

// ---------------------------------------------------------------------------
// Loading the native driver through the ADBC driver manager

typedef AdbcStatusCode (*AdbcLoadDriverFn)(const char* driver_name, const char* entrypoint,
                                           int version, void* driver, struct AdbcError* error);
typedef AdbcStatusCode (*AdbcFindLoadDriverFn)(const char* driver_name, const char* entrypoint,
                                               int version, uint32_t load_options,
                                               const char* additional_search_path_list,
                                               void* driver, struct AdbcError* error);

#define ADBC_ODBC_LOAD_FLAG_DEFAULT 15u  // env | user | system | relative paths

struct OdbcDriverManagerApi {
  AdbcLoadDriverFn load;
  AdbcFindLoadDriverFn find_load;
};

#if !defined(_WIN32)
/// Directories of every already-loaded shared object whose path mentions ADBC,
/// plus their parents (site-packages, lib/, ...), and the driver manager itself.
struct LoadedObjectScan {
  struct StrList dirs;
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
    StrListAdd(&scan->dirs, parent);
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
#endif  // !_WIN32

/// Resolve AdbcLoadDriver/AdbcFindLoadDriver from whichever driver manager is
/// already in the process.  We never link against the driver manager (that would
/// be a circular dependency), so this is all dlsym.
static bool DriverManagerApi(const struct StrList* managers, struct OdbcDriverManagerApi* api) {
#if defined(_WIN32)
  (void)managers;
  (void)api;
  return false;
#else
  memset(api, 0, sizeof(*api));
  api->load = (AdbcLoadDriverFn)dlsym(RTLD_DEFAULT, "AdbcLoadDriver");
  api->find_load = (AdbcFindLoadDriverFn)dlsym(RTLD_DEFAULT, "AdbcFindLoadDriver");
  if (api->load) return true;

  struct StrList candidates = {0};
  for (size_t i = 0; i < managers->count; i++) {
    StrListAdd(&candidates, managers->items[i]);
    char* dir = DirName(managers->items[i]);
    if (dir) {
      char buf[4096];
      snprintf(buf, sizeof(buf), "%s/libadbc_driver_manager.so", dir);
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
    api->load = (AdbcLoadDriverFn)dlsym(handle, "AdbcLoadDriver");
    api->find_load = (AdbcFindLoadDriverFn)dlsym(handle, "AdbcFindLoadDriver");
    if (api->load) break;
    dlclose(handle);
  }
  StrListFree(&candidates);
  return api->load != NULL;
#endif
}

/// Try every plausible location for the native driver called `name`.
/// On success `driver` holds a fully populated (manager-wrapped) function table.
static bool LoadNativeDriver(const struct OdbcDriverManagerApi* api, const char* name,
                             const struct StrList* dirs, const char* search_path, int version,
                             struct AdbcDriver* driver, char** why) {
  struct AdbcError error = ADBC_ERROR_INIT;
  char last[512] = {0};

#define TRY_LOAD(EXPR)                                   \
  do {                                                   \
    memset(driver, 0, sizeof(*driver));                  \
    if ((EXPR) == ADBC_STATUS_OK) return true;           \
    if (error.message) {                                 \
      snprintf(last, sizeof(last), "%s", error.message); \
      if (error.release) error.release(&error);          \
    }                                                    \
    error = (struct AdbcError)ADBC_ERROR_INIT;           \
  } while (0)

  // A name that already is a path or a manifest is used exactly as given; the
  // "libadbc_driver_<name>" guesses below only make sense for bare names.
  bool bare_name = strpbrk(name, "/\\.") == NULL;

  // 1. The driver manager's own resolution: manifests, ADBC_DRIVER_PATH, the
  //    system/user driver directories, and plain shared-library names.
  if (api->find_load) {
    TRY_LOAD(api->find_load(name, NULL, version, ADBC_ODBC_LOAD_FLAG_DEFAULT, search_path, driver,
                            &error));
  }
  TRY_LOAD(api->load(name, NULL, version, driver, &error));

  // 2. Python-wheel style layouts next to whatever is already loaded, plus any
  //    directory the caller nominated.
  for (size_t i = 0; bare_name && i < dirs->count; i++) {
    char path[4096];
    snprintf(path, sizeof(path), "%s/adbc_driver_%s/libadbc_driver_%s.so", dirs->items[i], name,
             name);
    if (FileExists(path)) TRY_LOAD(api->load(path, NULL, version, driver, &error));
    snprintf(path, sizeof(path), "%s/adbc_driver_%s/libadbc_driver_%s.dylib", dirs->items[i], name,
             name);
    if (FileExists(path)) TRY_LOAD(api->load(path, NULL, version, driver, &error));
    snprintf(path, sizeof(path), "%s/libadbc_driver_%s.so", dirs->items[i], name);
    if (FileExists(path)) TRY_LOAD(api->load(path, NULL, version, driver, &error));
    snprintf(path, sizeof(path), "%s/libadbc_driver_%s.dylib", dirs->items[i], name);
    if (FileExists(path)) TRY_LOAD(api->load(path, NULL, version, driver, &error));
  }

  // 3. The dynamic loader's own search path.
  if (bare_name) {
    char soname[256];
    snprintf(soname, sizeof(soname), "libadbc_driver_%s.so", name);
    TRY_LOAD(api->load(soname, NULL, version, driver, &error));
  }

#undef TRY_LOAD

  memset(driver, 0, sizeof(*driver));
  struct InternalAdbcStringBuilder sb;
  InternalAdbcStringBuilderInit(&sb, 128);
  InternalAdbcStringBuilderAppend(&sb, "no native \"%s\" ADBC driver could be loaded", name);
  if (last[0]) InternalAdbcStringBuilderAppend(&sb, " (%s)", last);
  *why = DupString(sb.buffer);
  InternalAdbcStringBuilderReset(&sb);
  return false;
}

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

void OdbcDelegateOptionsInit(struct OdbcDelegateOptions* opts) {
  memset(opts, 0, sizeof(*opts));
  // A deployment-wide off switch, for hosts that cannot pass options through.
  const char* env = getenv(ADBC_ODBC_DELEGATE_ENV);
  if (env && *env && !ParseMode(env, &opts->mode)) { opts->mode = ODBC_DELEGATE_AUTO; }
}

void OdbcDelegateOptionsRelease(struct OdbcDelegateOptions* opts) {
  free(opts->driver);
  free(opts->search_path);
  free(opts->last_error);
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

bool OdbcDelegateSetOption(struct OdbcDelegateOptions* opts, const char* key, const char* value,
                           AdbcStatusCode* status, struct AdbcError* error) {
  if (strcmp(key, ADBC_ODBC_OPTION_DELEGATE) == 0) {
    if (!ParseMode(value, &opts->mode)) {
      InternalAdbcSetError(error, "Invalid value \"%s\" for %s (auto/never/always)", value, key);
      *status = ADBC_STATUS_INVALID_ARGUMENT;
      return true;
    }
    *status = ADBC_STATUS_OK;
    return true;
  }
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
  if (strcmp(key, ADBC_ODBC_OPTION_DELEGATE_LAST_ERROR) == 0 ||
      strcmp(key, ADBC_ODBC_OPTION_DELEGATED_TO) == 0) {
    InternalAdbcSetError(error, "%s is read-only", key);
    *status = ADBC_STATUS_INVALID_ARGUMENT;
    return true;
  }
  if (IsPassThroughKey(key)) {
    // Held for the native driver; ignored when we end up on the ODBC path.
    *status = AddPassThrough(opts, key, value ? value : "", error);
    return true;
  }
  return false;
}

bool OdbcDelegateGetOption(const struct OdbcDelegateOptions* opts, const char* key,
                           const char** out) {
  if (strcmp(key, ADBC_ODBC_OPTION_DELEGATED_TO) == 0) {
    // Once delegation happens this driver is out of the picture, so anything
    // that still reaches us is running on ODBC.
    *out = ADBC_ODBC_DELEGATED_TO_ODBC;
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
  if (strcmp(key, ADBC_ODBC_OPTION_DELEGATE_LAST_ERROR) == 0) {
    *out = opts->last_error ? opts->last_error : "";
    return true;
  }
  return false;
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
                                   struct OdbcDelegateOptions* opts, bool* delegated,
                                   struct AdbcError* error) {
  *delegated = false;
  if (opts->mode == ODBC_DELEGATE_NEVER) return ADBC_STATUS_OK;

#if defined(_WIN32)
  return DelegateGiveUp(opts, DupString("delegation is not implemented on Windows"),
                        ADBC_STATUS_NOT_IMPLEMENTED, error);
#else
  // Delegation replaces the function table the caller dispatches through, so it
  // only works when we were loaded by the ADBC driver manager.
  struct AdbcDriver* table = database->private_driver;
  if (!table || table->DatabaseInit != self_database_init) {
    return DelegateGiveUp(
        opts,
        DupString("the driver was not loaded through the ADBC driver manager, so the "
                  "native driver cannot be swapped in"),
        ADBC_STATUS_NOT_IMPLEMENTED, error);
  }
  int version = (int)(intptr_t)table->private_data;
  if (version != ADBC_VERSION_1_0_0 && version != ADBC_VERSION_1_1_0) {
    return DelegateGiveUp(opts, DupString("unknown ADBC version in the driver table"),
                          ADBC_STATUS_INTERNAL, error);
  }
  size_t table_size =
      version == ADBC_VERSION_1_0_0 ? ADBC_DRIVER_1_0_0_SIZE : ADBC_DRIVER_1_1_0_SIZE;

  char* name = NULL;
  char* uri = NULL;
  char* why = NULL;
  bool uri_verbatim = false;
  if (!DetectNative(target, opts, &name, &uri, &uri_verbatim, &why)) {
    return DelegateGiveUp(opts, why, ADBC_STATUS_NOT_FOUND, error);
  }

  struct LoadedObjectScan scan = {0};
  ScanLoadedObjects(&scan);
  StrListAddPathList(&scan.dirs, opts->search_path);
  StrListAddPathList(&scan.dirs, getenv(ADBC_ODBC_DELEGATE_PATH_ENV));

  struct OdbcDriverManagerApi api;
  if (!DriverManagerApi(&scan.managers, &api)) {
    free(name);
    free(uri);
    StrListFree(&scan.dirs);
    StrListFree(&scan.managers);
    return DelegateGiveUp(
        opts, DupString("the ADBC driver manager's loader (AdbcLoadDriver) was not found"),
        ADBC_STATUS_NOT_FOUND, error);
  }

  struct AdbcDriver* native = calloc(1, sizeof(struct AdbcDriver));
  if (!native) {
    free(name);
    free(uri);
    StrListFree(&scan.dirs);
    StrListFree(&scan.managers);
    InternalAdbcSetError(error, "out of memory");
    return ADBC_STATUS_INTERNAL;
  }

  bool loaded = LoadNativeDriver(&api, name, &scan.dirs, opts->search_path, version, native, &why);
  StrListFree(&scan.dirs);
  StrListFree(&scan.managers);
  if (!loaded) {
    free(native);
    free(name);
    free(uri);
    return DelegateGiveUp(opts, why, ADBC_STATUS_NOT_FOUND, error);
  }

  // Stand a native database up with the translated options.  Nothing is swapped
  // until it is fully initialized, so a failure here is a clean fallback.
  void* our_private_data = database->private_data;
  struct AdbcError native_error = ADBC_ERROR_INIT;
  database->private_data = NULL;
  AdbcStatusCode status = native->DatabaseNew(database, &native_error);
  if (status == ADBC_STATUS_OK) {
    status = native->DatabaseSetOption(database, ADBC_OPTION_URI, uri, &native_error);
  }
  // Credentials are folded into a URI we built ourselves; pass them separately
  // only when the caller handed us a native URI verbatim.
  if (status == ADBC_STATUS_OK && uri_verbatim && target->username) {
    status =
        native->DatabaseSetOption(database, ADBC_OPTION_USERNAME, target->username, &native_error);
    if (status == ADBC_STATUS_OK && target->password) {
      status = native->DatabaseSetOption(database, ADBC_OPTION_PASSWORD, target->password,
                                         &native_error);
    }
  }
  for (size_t i = 0; status == ADBC_STATUS_OK && i < opts->pass_count; i++) {
    status = native->DatabaseSetOption(database, opts->pass_keys[i], opts->pass_values[i],
                                       &native_error);
  }
  if (status == ADBC_STATUS_OK) { status = native->DatabaseInit(database, &native_error); }

  if (status != ADBC_STATUS_OK) {
    struct InternalAdbcStringBuilder sb;
    InternalAdbcStringBuilderInit(&sb, 128);
    InternalAdbcStringBuilderAppend(&sb, "the native \"%s\" driver rejected the target", name);
    if (native_error.message) {
      InternalAdbcStringBuilderAppend(&sb, ": %s", native_error.message);
    }
    why = DupString(sb.buffer);
    InternalAdbcStringBuilderReset(&sb);
    if (native_error.release) native_error.release(&native_error);
    if (database->private_data) { (void)native->DatabaseRelease(database, NULL); }
    if (native->release) (void)native->release(native, NULL);
    free(native);
    free(name);
    free(uri);
    database->private_data = our_private_data;
    return DelegateGiveUp(opts, why, status, error);
  }
  if (native_error.release) native_error.release(&native_error);

  // Hand over: overwrite the function table in place, so that the pointer the
  // driver manager holds (and hands to AdbcError) stays valid and every later
  // call lands on the native driver.  Our own function table is never used
  // again; the driver manager releases the native one on AdbcDatabaseRelease.
  memcpy(table, native, table_size);
  free(native);
  free(name);
  free(uri);
  (void)our_private_data;  // the caller owns and frees its own state
  *delegated = true;
  return ADBC_STATUS_OK;
#endif
}
