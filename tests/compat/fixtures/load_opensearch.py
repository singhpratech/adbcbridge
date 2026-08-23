# SPDX-License-Identifier: Apache-2.0
"""Load the `opensearch` matrix entry's two indices into a running OpenSearch.

    python3 tests/compat/fixtures/load_opensearch.py [http://127.0.0.1:19200] [rows]

OpenSearch's SQL plugin is a *query* interface over indices -- there is no CREATE TABLE
and no INSERT (`SELECT` and `SHOW`/`DESCRIBE` are the whole grammar), and the ODBC driver
answers SQLBindParameter with "OpenSearch does not support parameters" -- so the entry's
data cannot be created over the ODBC connection the way every other entry's is.  This
writes it over the REST API instead (stdlib only, no client library):

  adbc_t    the workload's two rows.  OpenSearch has no DECIMAL and no binary type, so
            `n` and `b` are `keyword` fields read back as text; `d` is a date-only field,
            which the SQL plugin types DATE, and `ts` a date-and-time one, which it types
            TIMESTAMP.  Row 2 carries only `i`: a field a document does not have simply
            is not there, which is how the all-NULL row is spelled.
  adbc_big  `rows` documents of (a, b, c, d, e) -- what check_big() reads and what
            bench/matrix_bench.py times a fetch of.
  adbc_search  three documents with an analysed `text` field, for the entry's `extra`
            steps: MATCH over a full-text index is the thing only a search engine does,
            and a `keyword` field (which is not analysed) cannot show it.

Both indices are given an explicit mapping first: field types would otherwise be guessed
from the first document, and a `null` in it maps nothing at all.

One cluster setting goes with them, because it caps what SQL can return rather than what
is stored: `plugins.query.size_limit` (default 10000) is the largest result set a SQL
query may produce, whatever its LIMIT says -- past it rows are dropped silently.

Re-running this is idempotent: each document is written under its own _id, so a second
run replaces the same documents.
"""
import json
import sys
import urllib.request

URL = (sys.argv[1] if len(sys.argv) > 1 else "http://127.0.0.1:19200").rstrip("/")
BIG_ROWS = int(sys.argv[2]) if len(sys.argv) > 2 else 100000


def request(method, path, body=None, ctype="application/json"):
    data = body.encode("utf-8") if isinstance(body, str) else body
    req = urllib.request.Request(URL + path, data=data, method=method)
    if data is not None:
        req.add_header("Content-Type", ctype)
    with urllib.request.urlopen(req) as resp:
        return json.loads(resp.read().decode("utf-8"))


def recreate(index, properties):
    try:
        request("DELETE", "/" + index)
    except urllib.error.HTTPError as e:  # 404: it was not there
        if e.code != 404:
            raise
    request("PUT", "/" + index, json.dumps({"mappings": {"properties": properties}}))


def bulk(index, docs):
    """Write `docs` (a list of (id, dict)) in _bulk batches."""
    lines, n = [], 0
    for doc_id, doc in docs:
        lines.append(json.dumps({"index": {"_index": index, "_id": doc_id}}))
        lines.append(json.dumps(doc))
        if len(lines) >= 20000:
            n += flush(lines)
            lines = []
    if lines:
        n += flush(lines)
    request("POST", "/%s/_refresh" % index)  # make the writes visible to a search
    return n


def flush(lines):
    out = request("POST", "/_bulk", "\n".join(lines) + "\n", "application/x-ndjson")
    if out.get("errors"):
        first = next(i for i in out["items"] if "error" in i.get("index", {}))
        raise SystemExit("bulk write failed: %s" % first)
    return len(out["items"])


# A SQL query returns at most plugins.query.size_limit rows whatever its LIMIT says, and
# the rest are dropped without a word -- the default 10000 would silently truncate a
# 100,000-document adbc_big to a tenth of itself.  (The other half of paging, the cursor
# fetch size, is a per-request value, not a cluster setting: it is the driver's own
# `fetchSize` connection option.)
request("PUT", "/_cluster/settings", json.dumps({"persistent": {
    "plugins.query.size_limit": max(BIG_ROWS, 10000),
}}))

# `date` is one OpenSearch field type; the SQL plugin types it DATE when the format is
# date-only and TIMESTAMP when it carries a time.
recreate("adbc_t", {
    "i": {"type": "long"}, "f": {"type": "double"}, "s": {"type": "keyword"},
    "b": {"type": "keyword"}, "d": {"type": "date", "format": "strict_date"},
    "ts": {"type": "date", "format": "strict_date_hour_minute_second_millis"},
    "n": {"type": "keyword"}, "bo": {"type": "boolean"},
})
# b holds the six characters \x0102 -- the bytes the other entries store, spelled the way
# a server with no binary type has to (see the entry's `binary_text`).  Row 2 has only
# `i`: every other field is absent, which is OpenSearch's NULL.
bulk("adbc_t", [
    ("1", {"i": 1, "f": 1.5, "s": "héllo \U0001F680", "b": "\\x0102",
           "d": "2024-02-29", "ts": "2024-02-29T13:45:10.123", "n": "12.345",
           "bo": True}),
    ("2", {"i": 2}),
])

recreate("adbc_big", {
    "a": {"type": "long"}, "b": {"type": "keyword"}, "c": {"type": "double"},
    "d": {"type": "date", "format": "strict_date"}, "e": {"type": "boolean"},
})
n = bulk("adbc_big", [(str(i), {"a": i, "b": "r%d" % i, "c": float(i),
                                "d": "2024-01-01", "e": i % 2 == 0})
                      for i in range(BIG_ROWS)])

# `text` (analysed into terms), not `keyword` (stored whole): MATCH only matches terms,
# so the entry's full-text `extra` steps need this field to be analysed.
recreate("adbc_search", {"id": {"type": "long"}, "body": {"type": "text"}})
bulk("adbc_search", [
    ("1", {"id": 1, "body": "OpenSearch is a distributed search engine"}),
    ("2", {"id": 2, "body": "Arrow Database Connectivity bridges ODBC drivers"}),
    ("3", {"id": 3, "body": "the search plugin answers SQL over indices"}),
])
print("wrote adbc_t (2 docs), adbc_big (%d docs) and adbc_search (3 docs) to %s"
      % (n, URL))
