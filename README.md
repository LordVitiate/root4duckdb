# ROOT4DuckDB 3.8.0

ROOT4DuckDB is a DuckDB extension for reading nested ROOT objects through
`TStreamerInfo`, building a basket-aware deep index, and publishing that index
to Apache Iceberg.

The build has two explicit stages:

1. `build-iceberg.sh` builds Apache Iceberg C++ once as a private shared runtime.
2. `build-root4duckdb.sh` builds DuckDB and the ROOT extension against those
   four shared Iceberg libraries.

Iceberg is never bootstrapped as a side effect of building ROOT4DuckDB. The
extension also bypasses Iceberg's exported transitive CMake graph, so vendored
Arrow/Parquet/Avro static archives are not absorbed into DuckDB or into
`root.duckdb_extension`.

## Fast build

```bash
unzip root4duckdb-v3.8.0-extension.zip
cd root4duckdb-v3.8.0-extension
chmod +x build-iceberg.sh build-root4duckdb.sh setup-source-tree.sh run-duckdb.sh scripts/*.sh

# If ROOT is not already active:
source /path/to/root/bin/thisroot.sh

./build-iceberg.sh --jobs 4
./build-root4duckdb.sh --jobs 4 --clean --tests
./run-duckdb.sh
```

The first command downloads and builds pinned Apache Iceberg C++ v0.3.0. Later
ROOT4DuckDB rebuilds reuse it. `build-root4duckdb.sh` fetches the pinned DuckDB
v1.4.5 source automatically when the source archive has just been unzipped.

See [BUILD_RU.md](BUILD_RU.md) for requirements, LXPLUS details, incremental
rebuilds, and dynamic-link verification.

## Main SQL API

- `read_root(...)` — direct universal ROOT reader.
- `root_build_index(...)` / `root_build_dataset_index(...)` — deep physical
  index over logical semantic paths.
- `read_root_dataset(...)` — indexed dataset reader with projection and
  predicate pruning.
- `root_dataset_stats(...)` — metadata aggregates.
- `root_iceberg_catalog(...)` — inspection of the local Iceberg SQL catalog.

The canonical row identity is `source_id + entry_id + nested indices`. Version
3.8 adds a guarded serialized-basket reader for supported
`std::vector<object>/primitive-member` layouts. It reads only the physical
ancestor branch and emits DuckDB vectors without constructing every C++ object.
Every file is schema-planned and cross-checked against the universal reader;
unsupported or mismatching layouts automatically fall back with a warning.

Version 3.8 keeps index format 12. Its metadata path is typed Parquet (no
per-file CSV staging), its adaptive Bloom is used only for equality/`IN`
planning, and scans keep all tasks for one ROOT file on one worker. Existing
format-11 indexes must be rebuilt. Existing 3.7 format-12 indexes remain
readable; see [MIGRATION_3.8_RU.md](MIGRATION_3.8_RU.md).

Reader selection is explicit and testable: `reader_mode := 'auto'` is the
default, `serialized` requires the fast layout, and `object` forces the old
universal path. See [docs/SERIALIZED_READER_RU.md](docs/SERIALIZED_READER_RU.md).

For a local snapshot use `catalog_mode := 'local'`. Production chunk workers
use `catalog_mode := 'external'` and do not commit the shared Iceberg catalog.
The embedded SQLite catalog is explicitly selected with
`catalog_mode := 'sqlite'`.
