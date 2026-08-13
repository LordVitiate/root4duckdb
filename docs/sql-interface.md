# SQL interface

root4duckdb exposes direct ROOT access, persistent indexing, dataset-scale reads, metadata statistics and Iceberg catalog inspection through DuckDB table functions.

| Function | Purpose |
|---|---|
| `read_root(...)` | Reads ROOT trees, logical object paths and histogram objects directly |
| `root_build_index(...)` | Builds reusable sidecar metadata and publishes one indexed snapshot |
| `root_build_dataset_index(...)` | Production-facing alias of `root_build_index(...)` |
| `read_root_dataset(...)` | Reads an indexed ROOT dataset with file, basket and entry pruning |
| `root_dataset_stats(...)` | Returns dataset statistics without opening ROOT files |
| `root_iceberg_catalog(...)` | Inspects the active Iceberg metadata snapshots |

## `read_root(...)`

`read_root(...)` exposes one ROOT file or an ad-hoc group of ROOT files as a DuckDB relation.

It supports:

- schema and path discovery;
- primitive `TTree` branches;
- deeply nested logical fields;
- nested variable-size collections;
- selective serialized decoding;
- universal ROOT object reconstruction;
- canonical SQL views of ROOT histograms and profiles;
- direct multi-file and glob scans.

A prebuilt sidecar index or Iceberg catalog is not required.

```sql
read_root(
    root_inputs,
    dictionary := '...',
    path_prefix := '...'
)
```

### Interface

| Argument | Default | Effect |
|---|---:|---|
| `root_inputs` | required | ROOT file, directory, shell-style glob, comma list, JSON string array or `@file` list |
| `dictionary` | — | Loads the C++ dictionaries required for semantic object traversal |
| `path_prefix` | — | Selects a logical object, collection, primitive field or histogram view |
| `reader_mode` | `auto` | Chooses serialized decoding or universal object reconstruction |
| `raw_validation_entries` | `4` | Number of initial entries compared with the universal object reader |
| `raw_max_entry_bytes` | `64 MiB` | Maximum serialized entry size accepted by the fast reader |
| `raw_max_values_per_entry` | `10,485,760` | Maximum number of values decoded from one entry |
| `tree_cache_bytes` | `64 MiB` | ROOT branch cache and read-ahead size |

Complex semantic paths require a compatible ROOT dictionary:

```sql
dictionary := '/data/libExperiment.so'
```

Primitive trees and ROOT histogram objects do not require an experiment dictionary when their stored types are already known to ROOT.

### Path discovery

Without `path_prefix`, `read_root(...)` returns the top-level readable paths:

```sql
SELECT *
FROM read_root('/data/events.root');
```

```text
path
-------------------------
/EventRecord
/analysis
/monitoring
```

An object or collection path lists its immediate logical children:

```sql
SELECT *
FROM read_root(
    '/data/events.root',
    dictionary := '/data/libExperiment.so',
    path_prefix := '/EventRecord/tracks'
);
```

A primitive path returns one relational row for every flattened value:

```sql
SELECT *
FROM read_root(
    '/data/events.root',
    dictionary := '/data/libExperiment.so',
    path_prefix := '/EventRecord/tracks/quality'
);
```

```text
event_id | tracks_idx | quality
---------|------------|--------
929      | 0          | 1.84
929      | 1          | 3.12
929      | 2          | 0.97
930      | 0          | 2.41
```

One SQL index column is added for every variable-size collection level:

```text
event_id | particles_idx | vertices_idx | value
```

### Reader modes

| Mode | Behaviour |
|---|---|
| `auto` | Uses serialized decoding when supported, validates it against the object reader and falls back automatically |
| `serialized` | Requires the serialized fast path and fails if the layout is unsupported or validation fails |
| `object` | Always reconstructs the ROOT object and traverses it through `TStreamerInfo` |

The serialized reader follows a plan derived from ROOT streamer metadata. It supports validated nested primitive collection layouts and emits one index column per collection level.

In automatic mode, unsupported or mismatching layouts fall back to the universal object reader. Schema and decoding mismatches are never silently ignored.

### Selective read example

```sql
SELECT
    event_id,
    tracks_idx,
    quality
FROM read_root(
    '/data/events.root',
    dictionary := '/data/libExperiment.so',
    path_prefix := '/EventRecord/tracks/quality'
)
WHERE event_id >= 929
  AND event_id < 934;
```

Projection pushdown prevents unused logical columns from being materialized. An `event_id` range limits the entries scheduled for reading.

For a compatible serialized layout, an execution profile may show:

```text
ROOT Input Files:            1
Serialized Entry Calls:      5
Serialized Values:           25
Serialized Baskets:          1
Serialized Basket Bytes:     9657
Object Validation Entries:   4
Object Fallback Entries:     0
```

Value predicates are evaluated during the direct scan. Because `read_root(...)` has no persistent sidecar statistics, it does not perform dataset-level file or basket pruning by value.

### Primitive trees

A primitive `TTree` can be read directly without semantic object reconstruction:

```sql
SELECT *
FROM read_root(
    '/data/primitive-tree.root',
    path_prefix := '/Events/value'
)
WHERE event_id < 100;
```

Primitive values preserve their signedness and width in the DuckDB schema where a direct mapping exists.

### Multiple ROOT files

`root_inputs` accepts:

- one exact file;
- a numbered ROOT part such as `.root.001`;
- a directory;
- a shell-style glob;
- a comma-separated list;
- a JSON string array;
- an `@file` containing one input per line.

```sql
SELECT count(*), sum(value)
FROM read_root(
    '/data/run*.root*',
    dictionary := '/data/libExperiment.so',
    path_prefix := '/EventRecord/records/value'
);
```

For multiple inputs, the schema is bound once from the first readable representative file. Compatible files reuse the bound access plan and are opened lazily by a bounded worker pool.

Multi-file results add two identity columns:

| Column | Type | Meaning |
|---|---|---|
| `source_id` | `UBIGINT` | Stable zero-based position after input expansion |
| `source_path` | `VARCHAR` | Resolved ROOT input path |

`event_id` remains the entry number inside one source. The stable identity of a flattened value is therefore:

```text
source_id + event_id + nested collection indices
```

A predicate on `source_id` is pushed into the scheduler and prevents unrelated files from being opened.

Remote opens use a bounded retry budget. If one input in a multi-file request remains unavailable, root4duckdb emits an explicit warning, records the failure in profiling counters and continues with the remaining sources.

A single exact input remains strict and fails when it cannot be opened. Schema and decoding failures are always strict.

### ROOT histograms and profiles

`read_root(...)` exposes known ROOT histogram classes through canonical relational schemas built from their public ROOT APIs.

Supported families include:

- `TH1`, `TH2` and `TH3` subclasses;
- `TProfile`;
- `TProfile2D`;
- `TProfile3D`.

`TH2Poly` is not flattened as a rectangular histogram because polygon bins require a separate geometry adapter.

Histogram mode currently accepts one ROOT file per `read_root(...)` call.

Three views are available:

| Path | Result |
|---|---|
| `/analysis/mass` | Bin view; equivalent to `/analysis/mass/bins` |
| `/analysis/mass/bins` | One row per global histogram cell |
| `/analysis/mass/axes` | One row per active axis |
| `/analysis/mass/meta` | One histogram-level metadata row |

#### Bin view

```sql
SELECT
    global_bin,
    x_bin,
    x_low,
    x_high,
    content,
    error
FROM read_root(
    '/data/analysis.root',
    path_prefix := '/analysis/mass/bins'
)
ORDER BY global_bin;
```

The bin view includes:

- object path, name, title and ROOT class;
- histogram dimension;
- global and per-axis bin numbers;
- underflow and overflow flags;
- bin edges, centers, widths and labels;
- content;
- symmetric and asymmetric ROOT errors;
- raw `Sumw2` values;
- profile entries and effective entries;
- histogram-wide entries, effective entries and sum of weights.

Underflow and overflow cells are preserved. They are not silently discarded.

#### Axis view

```sql
SELECT
    axis_name,
    title,
    nbins,
    min,
    max,
    variable_bins,
    labels_present
FROM read_root(
    '/data/analysis.root',
    path_prefix := '/analysis/mass/axes'
)
ORDER BY axis_idx;
```

The axis view contains geometry, active ranges, time-display settings, labels and presentation metadata for each active axis.

#### Metadata view

```sql
SELECT *
FROM read_root(
    '/data/analysis.root',
    path_prefix := '/analysis/mass/meta'
);
```

The metadata view contains one row with:

- class and dimensionality;
- profile status;
- entries, effective entries and sum of weights;
- number of histogram cells;
- `Sumw2` state;
- stored and calculated ranges;
- means, errors, standard deviations, skewness and kurtosis;
- overflow-statistics policy;
- style and drawing properties;
- attached function metadata;
- profile error option;
- bin counts for each active axis.

Histogram predicates and projections are evaluated through the same DuckDB table-function pipeline as other direct reads.

> **Use `read_root(...)` for direct files, globs, schema exploration, histogram access and reader validation. Use `read_root_dataset(...)` when persistent metadata-driven pruning is required.**

---

## `root_build_index(...)`

`root_build_index(...)` scans ROOT files and builds reusable sidecar metadata for one or more logical paths.

It does not copy or convert event data. ROOT files remain the source of truth.

```sql
root_build_index(
    inputs,
    tree_name,
    logical_paths,
    output_dir,
    dictionary := '...'
)
```

`root_build_dataset_index(...)` is the production-facing alias with the same SQL interface and execution pipeline.

### Interface

| Argument | Default | Effect |
|---|---:|---|
| `inputs` | required | ROOT file, directory, glob, comma-separated list, JSON array or `@file` list |
| `tree_name` | required | Selects the `TTree` |
| `logical_paths` | required | One path, comma-separated paths or a JSON array of paths |
| `output_dir` | required* | Stores generated sidecars, staging data or a local catalog |
| `dictionary` | — | Loads dictionaries required for semantic object traversal |
| `dictionary_cleanup` | runtime policy | Controls dictionary-owned object cleanup |
| `reader_mode` | `auto` | Chooses serialized decoding or object reconstruction during indexing |
| `raw_validation_entries` | runtime default | Controls serialized/object validation |
| `raw_max_entry_bytes` | runtime default | Limits serialized entry size |
| `raw_max_values_per_entry` | runtime default | Limits decoded values per entry |
| `tree_cache_bytes` | runtime default | Configures ROOT caching |
| `bloom_bytes` | runtime setting | Controls Bloom-filter size; `0` disables Bloom filters |
| `bloom_false_positive_rate` | `0.01` | Controls the Bloom size–precision trade-off |
| `index_threads` | runtime setting | Controls parallel file indexing |
| `max_in_flight_files` | runtime setting | Caps concurrently active file workers |
| `memory_budget_bytes` | runtime setting | Limits indexing concurrency and metadata buffering |
| `estimated_worker_bytes` | runtime setting | Supplies the planner’s worker-memory estimate |
| `metadata_flush_bytes` | runtime setting | Controls bounded metadata flushing |
| `overwrite` | `false` | Allows replacement of an existing local snapshot |
| `allow_partial` | `false` | Allows publication when some input files fail |
| `catalog_mode` | `local` | Selects `local`, `sqlite`, `external` or `tables` publication |
| `publish_mode` | `none` | Selects `none`, `append` or `replace` for explicit metadata tables |
| `chunk_id` | — | Identifies one production indexing chunk |
| `manifest_fingerprint` | — | Carries the validated input-manifest identity |
| `dictionary_fingerprint` | — | Carries the validated dictionary identity |

\* `output_dir` may be empty only with `catalog_mode := 'tables'`.

Multiple logical paths may be indexed in one call when they belong to the same top-level ROOT class. The top-level object is loaded once per entry and reused for all requested paths.

### Catalog modes

| Mode | Effect |
|---|---|
| `local` | Publishes a local Parquet sidecar snapshot |
| `sqlite` | Publishes six metadata tables through an embedded Apache Iceberg SQLite catalog |
| `external` | Produces immutable staging metadata for an external committer |
| `tables` | Writes into explicitly supplied DuckDB metadata tables |

The six canonical metadata relations are:

```text
files
schemas
access
baskets
snapshots
commits
```

### Example

```sql
SELECT
    file_path,
    entries,
    flattened_values,
    baskets,
    status,
    snapshot_id,
    effective_threads,
    published,
    publish_mode
FROM root_build_dataset_index(
    '/data/events/*.root',
    'Events',
    '[
        "/Event/tracks/chi2",
        "/Event/tracks/nHits"
    ]',
    '/data/root-index',
    dictionary := '/data/libExperiment.so',
    catalog_mode := 'sqlite'
);
```

The function returns one status row per ROOT file and includes publication identity and execution counters.

The generated metadata contain:

- source-file identity and freshness data;
- logical schemas;
- object traversal plans;
- nearest physical ancestor mappings;
- file and basket entry ranges;
- flattened value counts;
- numeric statistics;
- adaptive Bloom filters;
- snapshot and commit identity.

> **Indexing performs the expensive semantic traversal once so later queries can eliminate irrelevant files, baskets and entries before decoding begins.**

---

## `read_root_dataset(...)`

`read_root_dataset(...)` reads one logical field from an indexed ROOT dataset.

It resolves the committed snapshot, uses sidecar metadata to prune physical work and decodes only the surviving ROOT data.

```sql
read_root_dataset(
    index_or_catalog,
    logical_path,
    dictionary := '...'
)
```

### Interface

| Argument | Default | Effect |
|---|---:|---|
| `index_or_catalog` | required | Selects a local sidecar snapshot or published catalog |
| `logical_path` | required | Selects the indexed logical field |
| `dictionary` | — | Loads ROOT dictionaries required for semantic access or fallback |
| `dictionary_cleanup` | runtime policy | Controls dictionary-owned object cleanup |
| `snapshot_id` | latest committed | Selects a specific immutable dataset version |
| `reader_mode` | `auto` | Chooses serialized decoding or universal object reconstruction |
| `require_fresh_index` | `true` | Rejects ROOT files changed after indexing |
| `row_limit` | unlimited | Stops planning and execution after the requested emitted-row budget |
| `max_open_files` | automatic | Caps concurrent ROOT readers |
| `memory_budget_bytes` | automatic | Limits reader concurrency by available memory |
| `estimated_reader_bytes` | automatic | Supplies the planner’s per-reader memory estimate |
| `tree_cache_bytes` | runtime default | Configures ROOT cache size |
| `coalesce_gap_bytes` | runtime default | Coalesces nearby physical ranges |
| `prefetch_depth` | runtime default | Controls bounded range prefetch |
| `prefetch_ranges` | runtime default | Enables or disables physical-range prefetch |
| `path_predicates` | — | Uses other indexed logical paths to restrict candidate events |
| `entry_selection` | — | Restricts reading to explicit source IDs, ranges or entries |
| `entry_selection_file` | — | Loads the explicit entry selection from a file |

Catalog table parameters may override the default metadata relation names.

### Returned relation

```text
event_fk | nested indices | value | source_id | entry_id
```

| Column | Meaning |
|---|---|
| `event_fk` | Snapshot-local dataset event identifier |
| Nested index columns | Position at each variable-size collection level |
| `value` | Requested logical value |
| `source_id` | Stable identifier of the source ROOT file |
| `entry_id` | Entry number inside that ROOT file |

The canonical physical row identity is:

```text
source_id + entry_id + nested collection indices
```

### Example

```sql
SELECT
    source_id,
    entry_id,
    tracks_idx,
    value AS chi2
FROM read_root_dataset(
    '/data/root-index',
    '/Event/tracks/chi2',
    dictionary := '/data/libExperiment.so'
)
WHERE event_fk >= 929
  AND event_fk < 934
  AND value < 10;
```

The planner applies the query in stages:

```text
resolve committed snapshot
    ↓
prune ROOT files
    ↓
prune baskets with min/max and Bloom metadata
    ↓
restrict entry ranges
    ↓
decode the requested logical value
    ↓
evaluate the exact predicate
```

Projection pushdown avoids materializing unused output columns.

An unfiltered `COUNT(*)` can be answered from metadata without opening any ROOT files:

```sql
SELECT count(*)
FROM read_root_dataset(
    '/data/root-index',
    '/Event/tracks/chi2'
);
```

### Cross-path predicates

`path_predicates` allows statistics from indexed companion paths to reduce the candidate entry set before the requested value is read.

```sql
SELECT *
FROM read_root_dataset(
    '/data/root-index',
    '/Event/tracks/chi2',
    dictionary := '/data/libExperiment.so',
    path_predicates := '[
        {
            "path": "/Event/header/run",
            "op": "=",
            "value": 297667
        }
    ]'
);
```

Exact predicates are still evaluated during execution. Sidecar statistics are used only for conservative rejection.

### Explicit entry selection

A prior stage may pass explicit source-local entry ranges:

```sql
SELECT *
FROM read_root_dataset(
    '/data/root-index',
    '/Event/tracks/chi2',
    dictionary := '/data/libExperiment.so',
    entry_selection := '{
        "file-A": {
            "ranges": [[929, 934]],
            "entries": [1001, 1007]
        }
    }'
);
```

Selections may contain ranges, individual entries or delta-encoded entry lists.

> **`read_root_dataset(...)` is the primary indexed analytical reader: SQL predicates determine not only which rows are returned, but which ROOT files, baskets and entries are read at all.**

---

## `root_dataset_stats(...)`

`root_dataset_stats(...)` returns exact summary statistics for an indexed logical field.

The result is calculated from sidecar metadata. ROOT files are not opened and event values are not decoded.

```sql
root_dataset_stats(
    index_or_catalog,
    logical_path
)
```

### Interface

| Argument | Default | Effect |
|---|---:|---|
| `index_or_catalog` | required | Selects the local index or published catalog |
| `logical_path` | required | Selects the indexed logical field |
| `snapshot_id` | latest committed | Reads statistics from a specific snapshot |
| Catalog table parameters | default table names | Supports custom catalog layouts |

### Returned values

| Column | Meaning |
|---|---|
| `row_count` | Total number of flattened values |
| `non_null_count` | Number of non-null values |
| `null_count` | Number of null values |
| `nan_count` | Number of NaN values |
| `pos_inf_count` | Number of positive infinities |
| `neg_inf_count` | Number of negative infinities |
| `min_value` | Dataset-wide numeric minimum |
| `max_value` | Dataset-wide numeric maximum |
| `basket_count` | Number of indexed ROOT baskets |
| `compressed_bytes` | Total compressed size of those baskets |
| `snapshot_id` | Snapshot used for the result |

### Example

```sql
SELECT *
FROM root_dataset_stats(
    '/data/root-index',
    '/Event/tracks/chi2'
);
```

```text
row_count          15,496,396
non_null_count     15,496,396
min_value          8.18e-09
max_value          818,426.5
basket_count       2,960
compressed_bytes   4,825,018,773
```

> **Use `root_dataset_stats(...)` when the answer already exists in metadata and opening ROOT files would perform unnecessary work.**

---

## `root_iceberg_catalog(...)`

`root_iceberg_catalog(...)` inspects a local root4duckdb Apache Iceberg catalog.

The catalog directory contains the embedded SQLite catalog and warehouse created by `catalog_mode := 'sqlite'`.

```sql
root_iceberg_catalog(
    catalog_directory
)
```

### Interface

| Argument | Effect |
|---|---|
| `catalog_directory` | Opens the local Iceberg SQLite catalog |

The function reports the active Iceberg state of the metadata tables:

```text
files
schemas
access
baskets
snapshots
commits
```

Only tables present in the catalog are returned.

### Returned values

| Column | Meaning |
|---|---|
| `table_name` | Metadata table name |
| `iceberg_snapshot_id` | Current Iceberg snapshot identifier |
| `metadata_location` | Active Iceberg metadata file |
| `manifest_list` | Manifest list referenced by the snapshot |

### Example

```sql
SELECT *
FROM root_iceberg_catalog(
    '/data/root-index'
)
ORDER BY table_name;
```

```text
table_name | iceberg_snapshot_id | metadata_location | manifest_list
-----------|---------------------|-------------------|--------------
files      | ...                 | ...               | ...
schemas    | ...                 | ...               | ...
access     | ...                 | ...               | ...
baskets    | ...                 | ...               | ...
snapshots  | ...                 | ...               | ...
commits    | ...                 | ...               | ...
```

All six tables for one ROOT snapshot are committed as one coordinated publication boundary. Readers resolve committed state rather than consuming incomplete staging data.

> **`root_iceberg_catalog(...)` shows which versioned metadata tables and Iceberg snapshots currently define the indexed ROOT dataset.**
