<div align="center">

<img src="assets/images/IMG_20260806_215451.jpg" alt="ROOT to SQL" width="100%">

# root4duckdb

**Selective SQL over ROOT data — without conversion.**

<p>
  <a href="https://github.com/LordVitiate/root4duckdb/releases/latest">
    <img alt="Latest release" src="https://img.shields.io/github/v/release/LordVitiate/root4duckdb?sort=semver&display_name=release&style=flat">
  </a>
  <img alt="DuckDB" src="https://img.shields.io/badge/DuckDB-v1.4.5-FFF000">
  <img alt="CERN ROOT" src="https://img.shields.io/badge/CERN%20ROOT-6.40-2c6aa6">
  <img alt="Platform" src="https://img.shields.io/badge/platform-Linux%20x86__64-lightgrey">
  <img alt="License" src="https://img.shields.io/badge/license-MIT-green">
</p>

</div>

ROOT4DuckDB turns ROOT files into a **unified analytical data layer**.

The same SQL interface works across one file, remote file sets and indexed datasets. ROOT remains the source of truth: event data are neither converted nor duplicated.

> **Request the ROOT data you need; ROOT4DuckDB reads it using the most efficient safe path available.**

## Latest news 🔥

- **2026-08-13 — ROOT histograms in SQL.** `TH1`, `TH2`, `TH3` and profile objects are available as relational views.
- **2026-08-11 — 10.7 billion values on one node.** A direct query scanned 117 remote ROOT files in 10 min 44 s at 16.6 million values/s with approximately 1.3 GiB peak memory. [Methodology and results](docs/direct-multifile-scan.md).

## Quick start

The current binary release requires:

- Linux x86-64 with glibc 2.34 or newer;
- DuckDB 1.4.5;
- a compatible CERN ROOT 6.40 installation.

Download the extension:

```bash
curl -fL \
  https://github.com/LordVitiate/root4duckdb/releases/latest/download/root.duckdb_extension \
  -o root.duckdb_extension
```

The extension is not signed with an official DuckDB key:

```bash
duckdb -unsigned
```

Load it:

```sql
LOAD './root.duckdb_extension';
```

Inspect a ROOT file:

```sql
SELECT *
FROM read_root('events.root');
```

Read a primitive branch:

```sql
SELECT *
FROM read_root(
    'events.root',
    path_prefix := '/energy'
);
```

Read a nested experiment object using its ROOT dictionary:

```sql
SELECT *
FROM read_root(
    'events.root',
    dictionary := 'libExperiment.so',
    path_prefix := '/Event/tracks/momentum'
);
```

Query a ROOT histogram:

```sql
SELECT x_low, x_high, content, error
FROM read_root(
    'analysis.root',
    path_prefix := '/analysis/mass'
);
```

## Why root4duckdb?

ROOT efficiently stores complex scientific data. But an experiment is usually a dataset distributed across thousands of files, branches and baskets.

ROOT4DuckDB provides one logical SQL layer over that storage:

- one logical path across different ROOT layouts;
- automatic selection of the safest efficient reader;
- selective file, basket and entry reads;
- reusable statistics, Bloom filters and snapshots;
- direct integration with DuckDB queries, joins and aggregates;
- no conversion of the original event data.

**ROOT stores the data. ROOT4DuckDB makes it a queryable data platform.**

## Features

- primitive and deeply nested ROOT fields;
- fully split, partially split and unsplit layouts;
- direct, serialized and universal object readers;
- automatic correctness fallback;
- local, remote and parallel multi-file scans;
- projection and predicate pushdown;
- basket-aware indexes with statistics and Bloom filters;
- versioned metadata through Apache Iceberg;
- relational views for ROOT histograms.

Nested collections become ordinary rows:

```text
/Event/tracks/hits/energy

event_id | tracks_idx | hits_idx | energy
```

ROOT4DuckDB hides the physical traversal while preserving the logical structure through index columns.

## Performance

A measured direct scan decoded:

| Metric | Result |
|---|---:|
| Remote ROOT files | 117 |
| Values | 10,696,574,044 |
| Wall time | 10 min 44 s |
| Sustained rate | 16.6 million values/s |
| Peak memory | approximately 1.3 GiB |

An indexed validation query reduced 2,960 baskets to one basket, five entries and 25 decoded values.

These are measured workloads, not universal performance claims. See [Direct multi-file scans and performance](docs/direct-multifile-scan.md).

## Release files

For normal use, download:

```text
root.duckdb_extension
```

The release archive additionally contains licenses, checksums and build information for auditing and redistribution.

Apache Iceberg and the compiler runtime are embedded in the extension. Separate Iceberg or GCC runtime libraries are not required.

ROOT remains external because its I/O runtime and experiment dictionaries must match the environment that opens the data.

## Building

```bash
./build-iceberg.sh --clean --jobs $(nproc)
./build-root4duckdb.sh --clean --package --jobs $(nproc)
```

Release artifacts are written to `dist/`.

The build requires a C++23 compiler, CMake 3.28+, Python, Ninja and CERN ROOT.

See [Building from source](docs/building-from-source.md).

## Documentation

- [How ROOT4DuckDB reads a ROOT file](docs/root-reading-pipeline.md)
- [SQL reference](docs/sql-interface.md)
- [Building from source](docs/building-from-source.md)
- [Performance and validation](docs/direct-multifile-scan.md)

Generate the C++ API documentation with:

```bash
doxygen Doxyfile
```

## Status

ROOT4DuckDB is a research prototype.

Correctness takes priority over forcing an optimization. When a narrow reader cannot prove that it preserves the result, ROOT4DuckDB falls back to the universal ROOT reader or fails explicitly.

## License

ROOT4DuckDB is distributed under the [MIT License](LICENSE).

Developed by **Seraphim S.**  
[sserubin@jinr.ru](mailto:sserubin@jinr.ru)
