<!-- SPDX-License-Identifier: Apache-2.0 -->
# adbc_driver_questdb

An ADBC 1.1.0 driver for QuestDB 10+ over QWP, the QuestDB wire protocol, built on
QuestDB's official C client ([questdb/c-questdb-client](https://github.com/questdb/c-questdb-client)).
Query results are the client's Arrow batches passed through; bulk ingest sends Arrow
batches the other way.

```bash
cmake -S questdb -B build-questdb -DCMAKE_BUILD_TYPE=Release   # needs Rust 1.91+
cmake --build build-questdb
```

Documentation: [`docs/getting-started/questdb-qwp.md`](../docs/getting-started/questdb-qwp.md).

Layout:

- `CMakeLists.txt`: fetches the pinned client (tag `7.2.0`, by commit and SHA-256), builds
  it with the Arrow C Data Interface enabled and links it statically into
  `libadbc_driver_questdb`.
- `src/qwp_driver.c`: the driver. Reuses `src/utils.c` and the vendored nanoarrow from the
  repository root for the Arrow results it builds itself (GetInfo, GetObjects, GetTableSchema).
- `tests/test_qwp.py`: the suite, through `adbc_driver_manager`, against a live server.
- `tests/c/test_qwp_driver.c`: C smoke test (`ctest --test-dir build-questdb`).
