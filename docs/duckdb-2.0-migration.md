# DuckDB 2.0 migration: PR validation and release limitations

## Scope

The `compat/duckdb-2.0` branch upgrades the development build from
DuckDB 1.5.6 to DuckDB **v2.0.0 (Cyanoptera)** at pinned commit
`d54cbbe3df17e8e5ce68312f483b4155836112aa`.
The extension version remains **3.10.0**. The ROOT file format, index
format (12), and public SQL functions are unchanged by this migration.

The change updates table-filter and expression access, writable vector
APIs, streaming SQL results, catalog/index metadata operations, and
CMake static-link dependencies for DuckDB 2.0.

A C API bootstrap remains in front of the ROOT-linked payload.
The bootstrap C API version in `cmake/RootBootstrap.cmake` is **1.5.6**;
that is the requested C API ABI version, not the DuckDB host version.
The independently downloaded DuckDB 2.0 CLI loaded the artifact.

## Validated on CERN lxplus

- Linux x86_64 / CERN LCG_110a
- GCC 14.3.0 and ROOT 6.40.04
- DuckDB 2.0.0 (`d54cbbe3df`)
- `./build-root4duckdb.sh --jobs 14`: build, payload/bootstrap link,
  extension LOAD, six public functions registered
- `./build-root4duckdb.sh --jobs 14 --tests`: two SQL tests passed
- `scripts/run-integration-test.sh`: **exit 0**
- Independent official DuckDB 2.0 CLI, same ROOT/GCC runtime:
  five SQL assertions passed at `threads=1` and five at `threads=4`

The integration suite covered direct and serialized ROOT reads,
nested collections, scalar/fixed-array fields, index construction,
Bloom/zonemap pruning, metadata catalogs and Iceberg, snapshots,
multi-connection scans, and object lifetime.

Concurrency regression:
- Eight independent databases: 48 validated reads.
- Shared database, eight connections: 576 validated reads.
- Shared database/index build: 504 validated reads.
- ROOT object construction/destruction: 1550 / 1550.
- Object-mode resident memory: 507556 KiB at 50 queries and
  508092 KiB at 1550 queries.

An `IN (12345.0, 23456.0)` basket-index regression was fixed.
The profiled query selected **0/3 files, 0/3 baskets**, opened no ROOT
files and decoded no values; runtime was 0.0876 s.

These are **functional validation fixtures**. They do not replace
production COMPASS mDST regressions or demonstrate performance scaling.

## Runtime portability: known blocker

A pristine environment test starts the official DuckDB 2.0 CLI with
`PATH=/usr/bin:/bin`, without ROOT/GCC environment variables and without
`LD_LIBRARY_PATH`. On the tested lxplus node, `/usr/bin/root-config`
reported ROOT 6.40.04, but the system `/lib64/libstdc++.so.6` lacked
`GLIBCXX_3.4.30`, which the compiled payload needs:

```text
cannot load /proc/self/fd/4: /lib64/libstdc++.so.6:
version `GLIBCXX_3.4.30' not found
```

This is a **failed pristine-load acceptance test**, not evidence of a
DuckDB 2.0 engine incompatibility. Independent SQL tests *passed* when
launched with the matched GCC/ROOT runtime. Do not claim that the current
binary works without environment setup on arbitrary Linux installations.
Do not ship copies of GCC runtime libraries without a packaging and
license review. Static `libstdc++` linking is an experimental CMake
option and is not validated as a portability solution.

## Before merging or publishing

1. Obtain a passing GitHub Actions build/test/upload run for the
   DuckDB 2.0 Linux AMD64 matrix; do not interpret a skipped matrix as
   a passed build. The latest branch workflows failed at
   **Generate matrix**, before compiling the extension.
2. Compare representative COMPASS mDST output and indexed pushdown
   against the existing DuckDB 1.5.6 baseline.
3. Validate `--package` metadata, exports, and dependencies for the
   final release artifact.
4. Resolve or explicitly scope the pristine-load C++ runtime limitation.
5. Publish only from a public source commit. Updating
   `duckdb/community-extensions` is a separate PR from merging
   `compat/duckdb-2.0` into this repository's `dev` branch.

## Reproduction

```bash
./build-root4duckdb.sh --jobs 14 --tests
ROOT4DUCKDB_INDEPENDENT_DUCKDB=.deps/vanilla-duckdb-2.0/duckdb \
  bash scripts/test-independent-duckdb.sh
ROOT4DUCKDB_INDEPENDENT_DUCKDB=.deps/vanilla-duckdb-2.0/duckdb \
  bash scripts/test-pristine-load.sh
```

Keep the build tree for incremental rebuilds; `--clean` is not needed
for ordinary source or CMake changes.
