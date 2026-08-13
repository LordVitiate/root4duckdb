<div align="center">

<img src="assets/images/IMG_20260806_215451.jpg" alt="ROOT to SQL" width="100%">

# root4duckdb

**Selective SQL over large ROOT datasets — without converting the event data.**

<p>
  <img alt="Release" src="https://img.shields.io/badge/release-3.8.0-2f6fdb">
  <img alt="Index format" src="https://img.shields.io/badge/index%20format-12-6f42c1">
  <img alt="DuckDB" src="https://img.shields.io/badge/DuckDB-v1.4.5-FFF000">
  <img alt="CERN ROOT" src="https://img.shields.io/badge/CERN%20ROOT-required-2c6aa6">
  <img alt="Core language" src="https://img.shields.io/badge/core-C%2B%2B17-00599C">
  <img alt="Iceberg adapter" src="https://img.shields.io/badge/Iceberg%20adapter-C%2B%2B23-00599C">
  <img alt="Platform" src="https://img.shields.io/badge/platform-Linux%20x86__64-lightgrey">
  <img alt="License" src="https://img.shields.io/badge/license-MIT-green">
</p>

<p>
  <a href="#quick-start">Quick start</a> ·
  <a href="#how-root4duckdb-reads-a-root-file">How it works</a> ·
  <a href="#sql-interface">SQL interface</a> ·
  <a href="docs/direct-multifile-scan.md">Performance</a> ·
  <a href="#project-status-and-roadmap">Roadmap</a>
</p>

</div>

> *“I owe much of my work to my own laziness. I disliked writing programs, so when I was working on the IBM 701, I began developing a system that would make programs easier to write.”*  
> — **John Backus**

<a id="latest-news"></a>

## Latest news 🔥

<!-- SECTION:latest_news -->
- **2026-08-11 — 10.7 billion nested values scanned on one node.** A direct SQL query decoded **10,696,574,044 values from 117 remote ROOT files in 10 min 44 s**, sustaining **16.6 million values/s** with approximately **1.3 GiB peak resident memory**. Three transient remote-open timeouts were recovered automatically and all 117 files contributed to the final checksum. See the [methodology, reproducible query and interpretation](docs/direct-multifile-scan.md#measured-performance).
<!-- /SECTION:latest_news -->

## Quick start

Build the project using the instructions in [Building from source](#building-from-source), then start the bundled DuckDB CLI directly:

```bash
./build/release/duckdb
```

The bundled CLI already contains root4duckdb. To use the loadable extension from another ABI-compatible DuckDB build instead:

```sql
LOAD './build/release/extension/root/root.duckdb_extension';
```

On CERN lxplus, `./run-duckdb.sh` is a convenience workaround that restores the CVMFS compiler, ROOT and shared Iceberg runtime paths before starting the same bundled CLI.

One SQL call can scan a file, a glob or an explicit file list:

```sql
SET threads = 10;

SELECT
    count(*) AS decoded_values,
    sum(CAST(value AS HUGEINT)) AS checksum
FROM read_root(
    '/data/run*.root*',
    dictionary := '/data/libExperiment.so',
    path_prefix := '/EventRecord/records/value'
);
```

ROOT files remain the source of truth. The query returns ordinary DuckDB rows with the ROOT entry number, nested collection indices and the selected primitive value.

## Contents

- [Latest news](#latest-news)
- [Quick start](#quick-start)
- [Data-reading and execution optimizations](#data-reading-and-execution-optimizations)
- [How root4duckdb reads a ROOT file](#how-root4duckdb-reads-a-root-file)
  - [Fully split layout](#fully-split-layout)
  - [Partially split layout](#partially-split-layout)
  - [Unsplit object](#unsplit-object)
- [Validation on real experimental data](#validation-on-real-experimental-data)
- [Direct multi-file scans and performance](docs/direct-multifile-scan.md)
- [SQL interface](#sql-interface)
  - [read_root(...)](#read-root)
  - [root_build_index(...)](#root-build-index)
  - [read_root_dataset(...)](#read-root-dataset)
  - [root_dataset_stats(...)](#root-dataset-stats)
  - [root_iceberg_catalog(...)](#root-iceberg-catalog)
- [Building from source](#building-from-source)
- [Project status and roadmap](#project-status-and-roadmap)
- [Developer and contributing](#developer-and-contributing)

ROOT TTree is an early columnar storage format. Split branches and compressed baskets already provide the physical basis for selective reading.

What ROOT lacks is a **dataset-level query-planning layer**.

root4duckdb adds this layer without replacing ROOT:

| Layer | Role |
|---|---|
| **ROOT** | Columnar event storage |
| **DuckDB** | SQL execution |
| **root4duckdb** | Query statistics and pruning |
| **Apache Iceberg** | Dataset planning and snapshots |

ROOT remains the authoritative event store. root4duckdb maps logical object fields onto ROOT's physical layout and uses sidecar metadata to eliminate irrelevant files, baskets and entry ranges before decoding.

In this sense, root4duckdb brings **Parquet-like query planning** to existing ROOT datasets without converting the event data.

## Data-reading and execution optimizations

| Capability | ROOT alone | root4duckdb | Parquet + Iceberg |
|---|:---:|:---:|:---:|
| Projection pushdown | Manual | ✓ | ✓ |
| Decode only requested logical values | Layout-dependent* | ✓** | ✓ |
| File pruning | — | ✓ | ✓ |
| Basket / row-group pruning | Limited* | ✓ | ✓ |
| Entry-range pruning | Manual | ✓ | ✓ |
| Min/max pruning | — | ✓ | ✓ |
| Bloom-filter pruning | — | ✓ | ✓ |
| Counts from metadata | — | ✓ | ✓ |
| Early stop after row limit | Manual | ✓ | ✓ |
| Parallel file scanning | Manual | ✓ | ✓ |
| Dataset versioning | — | ✓*** | ✓ |
| SQL execution | — | DuckDB | SQL engines |

\* ROOT supports selective branch and basket reading, but the result depends on the physical split layout and must normally be orchestrated by application code.

\** For supported serialized layouts, root4duckdb extracts only the requested logical values without reconstructing complete C++ event objects. Unsupported layouts automatically fall back to the universal object reader.

\*** Provided through Apache Iceberg.

## How root4duckdb reads a ROOT file

The same logical value may be stored using different ROOT layouts. root4duckdb always selects the narrowest safe read path allowed by the file.

<a id="fully-split-layout"></a>

### 🟨⬜⬜ Fully split: physical branch to SQL column

This is the simplest case.

ROOT has already decomposed the object into a physical primitive branch:

```text
EventRecord
└── tracks
    └── quality
```

The branch contains one variable-length sequence of `quality` values for every `TTree` entry.

Each event already has a stable ROOT entry number:

```cpp
GetEntry(entry_id)
```

Each value inside `tracks` also has a natural position in that event:

```text
entry_id = 929
tracks_idx = 2
```

root4duckdb maps these components directly into a relational result:

| ROOT representation | SQL column |
|---|---|
| `TTree` entry number | `entry_id` |
| Position inside `tracks` | `tracks_idx` |
| `quality` branch value | `quality` |

The physical hierarchy therefore becomes an ordinary SQL relation:

```text
entry_id | tracks_idx | quality
---------|--------------|---------
929      | 0            | 1.84
929      | 1            | 3.12
929      | 2            | 0.97
930      | 0            | 2.41
```

One physical primitive branch becomes one SQL value column. The ROOT entry number and collection position become SQL index columns.

For deeper collections, root4duckdb adds one index column at every variable-size level:

```text
entry_id | track_idx | hit_idx | value
```

The hierarchy is preserved without normalized join tables:

```text
ROOT entry
    └── collection index
        └── nested collection index
            └── primitive value
```

becomes:

```text
entry_id | outer_idx | inner_idx | value
```

> **A split ROOT branch already contains the column. root4duckdb exposes its values and natural indexes directly as SQL columns.**

### Adding query-planning metadata

The event values remain in the ROOT file.

root4duckdb writes only external sidecar metadata describing where useful values may be found:

- entry ranges;
- basket locations and sizes;
- value counts;
- min/max statistics;
- Bloom filters.

For example:

```text
ROOT file
└── basket 42
    ├── entries: 900–949
    ├── values: 287
    ├── min: 0.02
    └── max: 18.74
```

These metadata allow entire files, baskets and entry ranges to be rejected before ROOT decoding begins.

The ROOT data are not copied or converted:

```text
ROOT file
    └── authoritative event data

sidecar
    └── statistics and physical mappings
```

### Publishing the dataset through Iceberg

The sidecars are then published as part of an Apache Iceberg dataset.

Iceberg records:

- which ROOT files belong to the dataset;
- which sidecar metadata describe them;
- which schema is active;
- which immutable snapshot represents the current dataset version.

```text
Iceberg snapshot
├── ROOT file A
│   └── sidecar A
├── ROOT file B
│   └── sidecar B
└── ROOT file C
    └── sidecar C
```

Iceberg does not replace ROOT and does not store the event values. It provides the dataset-wide catalog, manifests and version history required to plan queries across many files.

### Reading the value back

Consider a query requesting one track value:

```sql
SELECT quality
FROM read_root_dataset(...)
WHERE source_id = 'file-A'
  AND entry_id = 929
  AND tracks_idx = 2;
```

For a fully split ROOT file, the read plan is almost direct:

```text
SQL predicate
    ↓
resolve the Iceberg snapshot
    ↓
select file-A
    ↓
locate the basket containing entry 929
    ↓
read only the quality branch
    ↓
GetEntry(929)
    ↓
select tracks_idx = 2
    ↓
return the SQL value
```

No unrelated ROOT files are opened.

No unrelated branches are materialized.

No complete `EventRecord` or `TrackRecord` object needs to be reconstructed.

No join is required to recover the track identity.

### Equivalent native ROOT code

Without root4duckdb, the same lookup can be written manually in ROOT C++:

```cpp
#include <TFile.h>
#include <TTree.h>
#include <TTreeReader.h>
#include <TTreeReaderArray.h>

#include <cstddef>
#include <iostream>
#include <stdexcept>

int main() {
    TFile file("file-A.root", "READ");
    if (file.IsZombie()) {
        throw std::runtime_error("Cannot open ROOT file");
    }

    auto *tree = file.Get<TTree>("Events");
    if (!tree) {
        throw std::runtime_error("Cannot find TTree");
    }

    tree->SetBranchStatus("*", false);
    tree->SetBranchStatus("EventRecord.tracks.quality", true);

    TTreeReader reader(tree);
    TTreeReaderArray<float> quality(
        reader,
        "EventRecord.tracks.quality"
    );

    constexpr Long64_t entry_id = 929;
    constexpr std::size_t tracks_idx = 2;

    if (reader.SetEntry(entry_id) != TTreeReader::kEntryValid) {
        throw std::runtime_error("Cannot read requested entry");
    }

    if (tracks_idx >= quality.GetSize()) {
        throw std::out_of_range("Track index is outside the collection");
    }

    const float value = quality[tracks_idx];
    std::cout << value << '\n';
}
```

ROOT already provides the essential physical operation:

```cpp
reader.SetEntry(929);
value = quality[2];
```

root4duckdb does not replace that mechanism. It adds everything required to derive the same operation automatically from SQL across an entire dataset.

| Step | Native ROOT C++ | root4duckdb |
|---|---|---|
| Select dataset version | Application logic | Iceberg snapshot |
| Locate the file | Application logic | File pruning |
| Select the branch | `SetBranchStatus()` | Projection pushdown |
| Locate the basket | ROOT or custom logic | Sidecar planning |
| Select the event | `SetEntry(929)` | `entry_id = 929` |
| Select the track | `quality[2]` | `tracks_idx = 2` |
| Skip impossible regions | Custom implementation | Min/max and Bloom pruning |
| Return the value | C++ code | SQL result |

> **For a fully split file, ROOT already provides efficient column access. root4duckdb turns that local mechanism into automatic, dataset-wide SQL planning.**

<a id="partially-split-layout"></a>

### 🟨🟨⬜ Partially split: extract a logical field from its physical parent

In the second case, the requested primitive value exists in the ROOT object model but does not have its own physical branch.

For example:

```text
EventRecord
└── tracks                 physical branch
    ├── quality              logical field
    ├── ndf                  logical field
    └── momentum             logical field
```

ROOT stores and reads `tracks` as one physical unit. The logical field `quality` is described by the class dictionary and `TStreamerInfo`, but it cannot be selected as an independent branch.

The physical and logical read units are therefore different:

```text
physical read unit
    = tracks branch

requested logical value
    = tracks[i].quality
```

root4duckdb discovers the member path through the ROOT dictionary and exposes it through exactly the same SQL relation as a fully split branch:

```text
entry_id | tracks_idx | quality
---------|--------------|---------
929      | 0            | 1.84
929      | 1            | 3.12
929      | 2            | 0.97
930      | 0            | 2.41
```

The SQL representation does not depend on how deeply ROOT physically split the object:

| ROOT representation | SQL column |
|---|---|
| `TTree` entry number | `entry_id` |
| Position inside `tracks` | `tracks_idx` |
| `TrackRecord::quality` member | `quality` |

The difference appears only in the physical read plan.

For a fully split file:

```text
quality SQL column
        ↓
quality physical branch
```

For a partially split file:

```text
quality SQL column
        ↓
TrackRecord::quality logical member
        ↓
tracks physical branch
```

> **The SQL column remains the same. Only the smallest physical ROOT object required to recover it changes.**

### Logical metadata over a physical parent branch

root4duckdb builds statistics for the requested logical field, even when ROOT stores that field inside a wider physical branch.

A sidecar entry may therefore describe both:

```text
logical path
    /EventRecord/tracks/quality

physical branch
    EventRecord.tracks
```

For each physical basket, the sidecar can record statistics for the extracted logical values:

```text
tracks basket 42
├── entries: 900–949
├── quality values: 287
├── quality min: 0.02
├── quality max: 18.74
└── quality Bloom filter
```

The basket still belongs to the physical `tracks` branch. The min/max and Bloom metadata describe only the logical `quality` values contained inside it.

This allows root4duckdb to prune using the logical SQL predicate before reading the wider physical parent:

```sql
WHERE quality > 100
```

```text
quality predicate
        ↓
logical min/max statistics
        ↓
reject impossible tracks baskets
        ↓
read only surviving physical baskets
```

The event data remain exclusively in ROOT:

```text
ROOT file
└── tracks baskets
    └── serialized TrackRecord objects

sidecar
└── quality statistics
    └── mappings to tracks baskets
```

### Publishing through Iceberg

The sidecars are published through the same Iceberg dataset model:

```text
Iceberg snapshot
├── ROOT file A
│   ├── physical tracks branch
│   └── quality sidecar metadata
├── ROOT file B
│   ├── physical tracks branch
│   └── quality sidecar metadata
└── ROOT file C
    ├── physical tracks branch
    └── quality sidecar metadata
```

Iceberg identifies the active dataset version and its files. The sidecars provide logical statistics and physical basket mappings. ROOT remains responsible for storing the event objects.

### Reading the value back

Consider the same SQL lookup:

```sql
SELECT quality
FROM read_root_dataset(...)
WHERE source_id = 'file-A'
  AND entry_id = 929
  AND tracks_idx = 2;
```

For a partially split file, the reverse read plan becomes:

```text
SQL predicate
    ↓
resolve the Iceberg snapshot
    ↓
select file-A
    ↓
resolve /EventRecord/tracks/quality
    ↓
find the physical ancestor: EventRecord.tracks
    ↓
locate the basket containing entry 929
    ↓
read the tracks basket
    ↓
GetEntry(929)
    ↓
select tracks[2]
    ↓
extract quality
    ↓
return one SQL value
```

No unrelated ROOT files are opened.

No unrelated physical branches are read.

The `tracks` parent must still be read because the ROOT layout stores `quality` inside it. However, only the requested logical value is emitted and materialized as an SQL column.

> **root4duckdb does not claim to read four isolated bytes when ROOT stores a complete object. It reads the smallest self-contained physical ancestor and returns only the requested logical value.**

### Two ways to extract the member

After the physical basket has been selected, root4duckdb can recover the logical member in two ways.

```text
Object extraction
    tracks object
        ↓
    TrackRecord element
        ↓
    quality member
```

This uses ROOT dictionaries, `TStreamerInfo`, collection proxies and normal object reconstruction.

For supported and validated layouts, root4duckdb may instead extract the primitive member directly from the serialized entry payload:

```text
Serialized tracks entry
        ↓
validated member layout
        ↓
quality value
```

The serialized reader also supports validated nested collection plans such as:

```text
/EventRecord/particles/vertex_ids/value
/EventRecord/hits/digits/samples/value
```

For these layouts, streamer actions skip preceding bases, objects and variable-width members; each nested primitive vector is decoded from its framed length and dense payload. One SQL index column is emitted for every collection level. The implementation follows the streamer schema rather than hard-coding class or member names.

Both methods produce the same SQL rows and indexes.

If selective serialized extraction is not supported for the current layout, root4duckdb automatically uses object reconstruction.

### Equivalent native ROOT code

Without root4duckdb, the application must read the physical parent branch and extract the member manually.

The exact class and accessor names depend on the experiment dictionary, but the operation is approximately:

```cpp
#include <TFile.h>
#include <TTree.h>

#include <cstddef>
#include <iostream>
#include <stdexcept>
#include <vector>

#include "TrackRecord.h"

int main() {
    TFile file("file-A.root", "READ");
    if (file.IsZombie()) {
        throw std::runtime_error("Cannot open ROOT file");
    }

    auto *tree = file.Get<TTree>("Events");
    if (!tree) {
        throw std::runtime_error("Cannot find TTree");
    }

    std::vector<TrackRecord> *tracks = nullptr;

    tree->SetBranchStatus("*", false);
    tree->SetBranchStatus("EventRecord.tracks", true);
    tree->SetBranchAddress("EventRecord.tracks", &tracks);

    constexpr Long64_t entry_id = 929;
    constexpr std::size_t tracks_idx = 2;

    if (tree->GetEntry(entry_id) <= 0) {
        throw std::runtime_error("Cannot read requested entry");
    }

    if (!tracks || tracks_idx >= tracks->size()) {
        throw std::out_of_range("Track index is outside the collection");
    }

    const TrackRecord &track = tracks->at(tracks_idx);
    const float value = track.quality;

    std::cout << value << '\n';
}
```

The essential operation is:

```cpp
tree->GetEntry(929);
value = tracks->at(2).quality;
```

The application must already know:

- which physical branch contains the logical member;
- which dictionary defines the class;
- how to address the collection;
- how to extract the requested field;
- which files and baskets might satisfy the predicate.

root4duckdb derives these operations from the logical SQL path and its sidecar metadata.

| Step | Native ROOT C++ | root4duckdb |
|---|---|---|
| Resolve the logical field | Application knowledge | ROOT dictionary traversal |
| Find the physical branch | Application knowledge | Nearest physical ancestor |
| Select the file | Application logic | Iceberg and file pruning |
| Select the basket | ROOT or custom logic | Sidecar planning |
| Read the parent object | `GetEntry()` | Automatic |
| Select the collection element | `tracks.at(i)` | `tracks_idx = i` |
| Extract the primitive member | `track.quality` | SQL value column |
| Prune by member statistics | Custom implementation | Min/max and Bloom pruning |
| Handle unsupported layouts | Custom implementation | Automatic object fallback |

> **For a partially split file, root4duckdb separates the logical SQL column from its wider physical ROOT container, adds statistics for that logical value, and reads the smallest physical ancestor required to recover it.**

<a id="unsplit-object"></a>

### 🟨🟨🟨 Unsplit object: one SQL model, two decoding paths

In the third case, ROOT stores the requested value inside a complete self-contained object.

```text
EventRecord                         physical branch
└── tracks                    object member
    └── quality                 requested logical value
```

Neither `tracks` nor `quality` has an independent physical branch. The smallest self-contained physical unit is the complete `EventRecord` entry.

```text
physical read unit
    = EventRecord

requested logical value
    = EventRecord.tracks[i].quality
```

ROOT therefore cannot select `quality` as an independent physical column.

### Mapping the object to SQL

The physical layout changes, but the SQL representation remains the same:

| ROOT object model | SQL column |
|---|---|
| `TTree` entry number | `entry_id` |
| Position inside `tracks` | `tracks_idx` |
| `TrackRecord::quality` | `quality` |

```text
entry_id | tracks_idx | quality
---------|--------------|---------
929      | 0            | 1.84
929      | 1            | 3.12
929      | 2            | 0.97
930      | 0            | 2.41
```

The logical identity of every value is still:

```text
(entry_id, tracks_idx)
```

For deeper collections, one SQL index column is added for every variable-size level:

```text
entry_id | outer_idx | inner_idx | value
```

> **The ROOT serialization layout may change, but the SQL schema and relational identity remain stable.**

### Adding logical statistics

The event data remain inside the ROOT file.

root4duckdb builds sidecar metadata for the requested logical field and maps those statistics back to the physical `EventRecord` baskets that contain it.

```text
logical path
    /EventRecord/tracks/quality

physical ancestor
    EventRecord
```

A sidecar may describe a physical basket like this:

```text
EventRecord basket 42
├── entries: 900–949
├── quality values: 287
├── quality min: 0.02
├── quality max: 18.74
└── quality Bloom filter
```

The basket belongs to the physical `EventRecord` branch. The statistics describe only the logical `quality` values found inside it.

This allows a predicate such as:

```sql
WHERE quality > 100
```

to reject impossible physical regions before any `EventRecord` object is decoded:

```text
quality predicate
        ↓
logical min/max statistics
        ↓
reject impossible EventRecord baskets
        ↓
decode only surviving entries
```

> **Planning is performed using the logical SQL value, even when ROOT stores that value inside a complete object.**

### Publishing the dataset through Iceberg

The ROOT files and their sidecars are published as one versioned dataset:

```text
Iceberg snapshot
├── ROOT file A
│   ├── EventRecord baskets
│   └── quality sidecar
├── ROOT file B
│   ├── EventRecord baskets
│   └── quality sidecar
└── ROOT file C
    ├── EventRecord baskets
    └── quality sidecar
```

Iceberg provides:

- the active dataset version;
- the list of ROOT files;
- the corresponding sidecars;
- manifests and immutable snapshots.

ROOT remains the authoritative event store. Iceberg provides dataset-wide visibility and versioning.

### Reading the value back

Consider the same SQL request:

```sql
SELECT quality
FROM read_root_dataset(...)
WHERE source_id = 'file-A'
  AND entry_id = 929
  AND tracks_idx = 2;
```

The common part of the read plan is:

```text
SQL predicate
    ↓
resolve the Iceberg snapshot
    ↓
select file-A
    ↓
resolve /EventRecord/tracks/quality
    ↓
identify EventRecord as the physical ancestor
    ↓
locate the basket containing entry 929
    ↓
read the selected EventRecord entry
```

At this point, root4duckdb has two possible decoding paths.

---

#### Serialized decoding

For a supported and validated layout, root4duckdb extracts the requested values directly from the serialized entry payload.

```text
selected EventRecord basket
        ↓
decompress the basket
        ↓
locate serialized entry 929
        ↓
follow the validated layout plan
        ↓
locate tracks element 2
        ↓
decode quality
        ↓
emit one SQL value
```

Complete `EventRecord` and `TrackRecord` C++ objects are not constructed.

```text
physical input
    = complete serialized EventRecord entry

decoded output
    = requested quality values
```

root4duckdb still reads the physical bytes required by the ROOT layout, but it avoids full object reconstruction and emits only the requested logical SQL columns.

> **Read the required physical payload, decode only the requested logical values.**

The complete reverse plan is:

```text
SQL predicate
    ↓
Iceberg snapshot
    ↓
file pruning
    ↓
basket pruning
    ↓
entry-range selection
    ↓
serialized EventRecord payload
    ↓
tracks_idx
    ↓
quality
    ↓
SQL result
```

A simplified representation of this operation is:

```cpp
// Conceptual pseudocode, not a public ROOT API example.

auto payload = read_serialized_entry(
    basket,
    entry_id
);

auto tracks = locate_collection(
    payload,
    vec_track_plan
);

float value = decode_primitive<float>(
    tracks[tracks_idx],
    quality_plan
);
```

The real decoder uses a plan derived from ROOT streamer metadata and accepts only layouts that pass its safety checks.

---

#### Object reconstruction

If direct serialized decoding is unsupported or fails validation, root4duckdb uses ROOT's universal object mechanism.

```text
selected EventRecord basket
        ↓
TTree::GetEntry(929)
        ↓
ROOT reconstructs EventRecord
        ↓
traverse tracks
        ↓
select element 2
        ↓
read quality
        ↓
emit one SQL value
```

This path uses:

- ROOT dictionaries;
- `TClass`;
- `TStreamerInfo`;
- stored member offsets;
- `TVirtualCollectionProxy`;
- normal ROOT object reconstruction.

The operation is conceptually equivalent to:

```cpp
tree->GetEntry(entry_id);

const auto &track =
    event->tracks.at(tracks_idx);

const float value =
    track.quality;
```

The actual universal reader does not hard-code `EventRecord`, `tracks` or `quality`. It builds the traversal dynamically from the loaded ROOT dictionary.

```text
TClass
    +
TStreamerInfo
    +
member offsets
    +
collection proxies
```

Object reconstruction is more expensive because ROOT may materialize complete C++ objects. However, it provides the semantic correctness reference for layouts rejected by the serialized decoder.

> **When selective decoding cannot be proven safe, ROOT reconstructs the object and root4duckdb traverses it semantically.**

### Planning remains the same

The two paths differ only after files, baskets and entry ranges have already been selected.

```text
Iceberg planning
        ↓
file pruning
        ↓
basket pruning
        ↓
entry-range pruning
        ↓
final decoding strategy
        ↓
same SQL rows
```

Object fallback does not disable the sidecar index. It changes only the final method used to recover values from the surviving ROOT entries.

### Comparing the two paths

| Step | Serialized decoding | Object reconstruction |
|---|---|---|
| Iceberg snapshot resolution | Same | Same |
| File pruning | Same | Same |
| Basket pruning | Same | Same |
| Entry-range pruning | Same | Same |
| Physical ROOT input | `EventRecord` payload | `EventRecord` payload |
| Complete `EventRecord` construction | No | Yes |
| Complete `TrackRecord` construction | No | Usually yes |
| Requested SQL columns emitted | Only requested | Only requested |
| Layout support | Conservative | Universal reference |
| Correctness role | Validated optimization | Semantic reference |
| Automatic fallback | To object reader | — |

In automatic mode, root4duckdb uses serialized decoding only after validating the layout and sampled results against the universal object reader.

Unsupported or mismatching layouts fall back automatically.

> **For unsplit ROOT objects, root4duckdb preserves the same SQL schema and the same dataset-level planning. Only the final decoding strategy changes: selective serialized decoding when proven safe, complete ROOT object reconstruction when necessary.**

## Validation on real experimental data

root4duckdb was validated on experimental ROOT data.

### Where the speed comes from

The main optimization is not reading the same data slightly faster. It is avoiding unnecessary work before decoding begins.

| Source of speed | Measured effect |
|---|---|
| **Sidecar metadata** | Counted **15,496,396 values in 0.082 s** without opening ROOT data |
| **File, basket and entry pruning** | Returned **25 values from 1 of 2,960 baskets**, reading only **5 entries** |
| **Projection pushdown** | Only the branch or physical ancestor required by the query was selected |
| **Selective decoding** | The same **66,506 rows** took **1.708 s** with ROOT object reconstruction and **0.539 s** with serialized decoding |
| **Min/max and Bloom pruning** | An equality lookup reached **1 matching row after selecting 26 of 2,960 baskets** |
| **Vectorized SQL execution** | A complete aggregate over **15,496,396 values** finished in **25.7 s** |

Traditional ROOT object path

```text
open ROOT data
    ↓
reconstruct C++ objects
    ↓
traverse collections
    ↓
evaluate the predicate
    ↓
return selected values
```
...root4duckdb indexed path

```text
inspect sidecar metadata
    ↓
prune files and baskets
    ↓
select entry ranges and columns
    ↓
decode requested values
    ↓
execute in DuckDB
```

### Measured reader comparison

Both readers produced exactly the same relational result:

```text
ROOT object reconstruction    66,506 rows    1.708 s
Serialized decoding           66,506 rows    0.539 s
Difference                            0 rows
Measured speedup                     3.17×
```

The larger gains appear on selective queries, where metadata prevents most ROOT data from being read at all:

```text
15,496,396 indexed values
          ↓
1 selected basket
          ↓
5 selected entries
          ↓
25 decoded values
```

### Large direct multi-file scan

The native multi-file RootScan was also exercised against a homogeneous set of remote experimental ROOT files. The query performed a complete checksum aggregate over one deeply nested primitive column.

| Metric | Measured result |
|---|---:|
| Remote ROOT files | **117 / 117** |
| Decoded primitive values | **10,696,574,044** |
| Wall time | **644.478 s** |
| Sustained value rate | **16.6 million values/s** |
| Observed remote-read throughput | **412–471 MiB/s** |
| Average CPU consumption | **4.49 cores** |
| Peak resident memory | **approximately 1.3 GiB** |
| Transient remote-open timeouts | **3, all recovered** |
| Final checksum | `2784253092475411033` |

The scan ran on one shared compute node with `threads = 14` and an 8 GiB DuckDB memory limit. It opened files lazily, reused one compatible schema plan, and returned one aggregate row; no intermediate event dataset was produced.

This is one measured scan, not a fixed speed for every ROOT file. Different layouts, compression and remote-storage load can change the result, and paths that require object fallback may be slower. The complete query and correctness checks are documented in [Direct multi-file scans and performance](docs/direct-multifile-scan.md#measured-performance).

The sidecar layer is small compared with the event data it describes. It does not duplicate ROOT payloads; it stores compact ranges, counts, statistics and optional Bloom filters. Even under a deliberately pessimistic estimate, a **230 TB** dataset would require only **hundreds of gigabytes of metadata**—well below **1%** of the original data volume—and most of that worst-case footprint would come from Bloom filters. Without aggressive Bloom indexing, the core file, basket, range and min/max metadata are considerably smaller.

> **Speed comes first from pruning, then from reading only the required physical data, and finally from avoiding unnecessary object reconstruction.**

The integration suite also covers split and unsplit objects, nested collections, arrays, inheritance, mixed layouts, automatic object fallback and Iceberg publication.

Timings describe the measured validation workload and environment; performance on other systems may differ.

<a id="sql-interface"></a>

## SQL interface

<a id="read-root"></a>

### `read_root(...)`

`read_root(...)` exposes one ROOT file or an ad-hoc group of ROOT files as a DuckDB relation.

It reads logical fields through ROOT dictionaries and `TStreamerInfo`. A prebuilt sidecar index or Iceberg catalog is not required.

```sql
read_root(
    root_inputs,
    dictionary := '...',
    path_prefix := '...'
)
```

The common call needs no reader-policy arguments. Optional safety and diagnostic controls are listed below for validation and development work.

### Interface

| Argument | Default | Effect |
|---|---:|---|
| `root_inputs` | required | ROOT file, directory, shell-style glob, comma list, JSON string array or `@file` list |
| `dictionary` | — | Loads the C++ dictionaries required for semantic object traversal |
| `path_prefix` | — | Selects the logical object, collection or primitive field |
| `reader_mode` | `auto` | Chooses serialized decoding or universal object reconstruction |
| `raw_validation_entries` | `4` | Number of initial entries compared with the universal object reader |
| `raw_max_entry_bytes` | `64 MiB` | Maximum serialized entry size accepted by the fast reader |
| `raw_max_values_per_entry` | `10,485,760` | Maximum number of values decoded from one entry |
| `tree_cache_bytes` | `64 MiB` | ROOT branch cache and read-ahead size |

Complex semantic paths require a compatible ROOT dictionary:

```sql
dictionary := '/data/libExperiment.so'
```

### Path behaviour

| Request | Result |
|---|---|
| No `path_prefix` | Lists top-level ROOT paths in a `path` column |
| Object or collection path | Lists its immediate logical children |
| Primitive field path | Returns `event_id`, nested collection indices and the requested value |

For multiple inputs, the schema is bound once from the first readable representative file. Files are then opened lazily by a bounded worker pool; one slow or unavailable input does not serialize all other opens. Compatible files reuse the bound access plan, while an actual per-file schema or decoder mismatch remains an execution error.

Multi-file results add two identity columns:

| Column | Type | Meaning |
|---|---|---|
| `source_id` | `UBIGINT` | Stable zero-based position after input expansion |
| `source_path` | `VARCHAR` | Resolved ROOT input path |

`event_id` remains the entry number inside one source, so the stable row identity is `(source_id, event_id, nested indices...)`. A predicate on `source_id` is pushed into the scheduler and prevents unrelated files from being opened.

```sql
SELECT count(*), sum(value)
FROM read_root(
    '/data/run*.root*',
    dictionary := '/data/libExperiment.so',
    path_prefix := '/EventRecord/records/value'
);
```

The same call also accepts `'/data/run1.root'`, `'@/data/input.list'`, a directory, a comma-separated list, or a JSON string array. No multi-file policy flags are required: `SET threads`, DuckDB's memory limit and the extension's automatic admission control determine safe concurrency.

Remote opens use a bounded retry budget. If a multi-file input is still unavailable, root4duckdb emits an explicit `ROOT_FILE_UNAVAILABLE` warning, records the file in profiling counters, and continues with the remaining sources. A single exact input remains strict and fails the query when it cannot be opened. This exception applies only to file availability; schema and decoding failures are never silently ignored.

See [Direct multi-file scans and performance](docs/direct-multifile-scan.md) for the execution model, failure semantics, profiling fields, regression procedure and measured benchmark.

For example:

```text
/EventRecord/tracks/quality
```

becomes:

```text
event_id | tracks_idx | quality
```

### Reader modes

| Mode | Behaviour |
|---|---|
| `auto` | Uses serialized decoding when supported, validates it against the object reader, and falls back automatically |
| `serialized` | Requires the serialized fast path and fails if the layout is unsupported or validation fails |
| `object` | Always reconstructs the ROOT object and traverses it through `TStreamerInfo` |

The serialized mode currently requires exactly one materialized logical value column. Identity and collection-index columns may still be returned with it.

### Example

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

Projection pushdown prevents unused logical columns from being materialized and applies physical branch projection when the ROOT layout permits it. An `event_id` range also limits the entries scheduled for reading.

Value predicates are evaluated during the direct scan, but `read_root(...)` has no dataset sidecar statistics for file or basket pruning.

> **Use `read_root(...)` for direct file/glob access, schema exploration and reader validation. Use `read_root_dataset(...)` when reusable metadata-driven pruning is required.**

<a id="root-build-index"></a>

## `root_build_index(...)`

`root_build_index(...)` scans ROOT files and builds reusable sidecar metadata for one or more logical paths.

It does **not** copy or convert event data. ROOT files remain the source of truth.

```sql
root_build_index(
    inputs,
    tree_name,
    logical_paths,
    output_dir,
    dictionary := '...'
)
```

`root_build_dataset_index(...)` is an alias with the same interface.

### Interface

| Argument | Default | Effect |
|---|---:|---|
| `inputs` | required | ROOT file, directory, glob, comma-separated list, JSON array or `@file` list |
| `tree_name` | required | Selects the `TTree` |
| `logical_paths` | required | One path, comma-separated paths or a JSON array of paths |
| `output_dir` | required* | Stores the generated sidecars and catalog |
| `dictionary` | — | Loads dictionaries required for semantic object traversal |
| `reader_mode` | `auto` | Chooses serialized decoding or object reconstruction during indexing |
| `bloom_bytes` | runtime setting | Controls Bloom-filter size; `0` disables Bloom filters |
| `bloom_false_positive_rate` | `0.01` | Controls the Bloom-filter size–precision trade-off |
| `index_threads` | runtime setting | Controls parallel file indexing |
| `memory_budget_bytes` | runtime setting | Limits indexing concurrency and metadata buffering |
| `overwrite` | `false` | Allows replacement of an existing local snapshot |
| `allow_partial` | `false` | Allows publication when some files fail |
| `catalog_mode` | `local` | Selects `local`, `sqlite`, `external` or `tables` publication |

\* `output_dir` may be empty only with `catalog_mode := 'tables'`.

Multiple logical paths may be indexed in one call, provided that they belong to the same top-level ROOT class. The top-level object is materialized only once per entry and reused for all requested paths.

### Catalog modes

| Mode | Effect |
|---|---|
| `local` | Publishes a local Parquet sidecar snapshot |
| `sqlite` | Publishes the snapshot through the embedded Iceberg SQLite catalog |
| `external` | Produces immutable staging metadata for an external committer |
| `tables` | Writes directly into explicitly named metadata tables |

### Example

```sql
SELECT
    file_path,
    entries,
    flattened_values,
    baskets,
    status,
    snapshot_id
FROM root_build_index(
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

The function returns one status row per ROOT file:

```text
file_path | entries | flattened_values | baskets | status | snapshot_id
```

The generated metadata contains logical schemas, object traversal plans, file statistics, basket ranges, value counts, min/max statistics and optional Bloom filters.

> **`root_build_index(...)` performs the expensive ROOT traversal once so that later queries can eliminate irrelevant files and baskets before decoding begins.**

<a id="read-root-dataset"></a>

## `read_root_dataset(...)`

`read_root_dataset(...)` reads a logical field from an indexed ROOT dataset.

It resolves the committed snapshot, uses sidecar metadata to prune files, baskets and entry ranges, and decodes only the surviving ROOT data.

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
| `dictionary` | — | Loads the ROOT dictionaries required for object fallback |
| `snapshot_id` | latest committed | Reads a specific immutable dataset version |
| `reader_mode` | `auto` | Chooses serialized decoding or universal object reconstruction |
| `require_fresh_index` | `true` | Rejects ROOT files changed after indexing |
| `row_limit` | unlimited | Stops the reader after the requested number of emitted rows |
| `max_open_files` | automatic | Limits concurrent ROOT readers |
| `memory_budget_bytes` | automatic | Limits reader concurrency by available memory |
| `path_predicates` | — | Uses statistics from other indexed logical paths to restrict candidate entries |
| `entry_selection` | — | Restricts reading to explicit source IDs, entry ranges or individual entries |

Additional controls configure ROOT caching, range coalescing, prefetching and serialized-reader safety limits.

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

The canonical row identity is:

```text
source_id + entry_id + nested indices
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
resolve snapshot
    ↓
prune ROOT files
    ↓
prune baskets with min/max and Bloom metadata
    ↓
restrict entry ranges
    ↓
decode the requested logical value
    ↓
evaluate the exact predicate in DuckDB
```

Projection pushdown avoids materializing unused output columns. An unfiltered `COUNT(*)` can be answered directly from metadata without opening ROOT files.

> **`read_root_dataset(...)` is the primary analytical reader: SQL predicates determine not only which rows are returned, but which ROOT data are read at all.**

<a id="root-dataset-stats"></a>

## `root_dataset_stats(...)`

`root_dataset_stats(...)` returns exact summary statistics for an indexed logical field.

The result is calculated entirely from sidecar metadata. ROOT files are not opened and event values are not decoded.

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
| `snapshot_id` | latest committed | Reads statistics from a specific dataset snapshot |
| Catalog table parameters | default table names | Allows custom catalog layouts |

### Returned values

| Column | Meaning |
|---|---|
| `row_count` | Total number of flattened values |
| `non_null_count` | Number of non-null values |
| `null_count` | Number of null values |
| `nan_count` | Number of NaN values |
| `pos_inf_count` / `neg_inf_count` | Number of positive and negative infinities |
| `min_value` / `max_value` | Dataset-wide numeric range |
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

> **Use `root_dataset_stats(...)` when the answer already exists in metadata and reading ROOT data would be unnecessary.**

---

<a id="root-iceberg-catalog"></a>

## `root_iceberg_catalog(...)`

`root_iceberg_catalog(...)` inspects a local root4duckdb Iceberg catalog.

The catalog directory must contain the embedded SQLite catalog created during Iceberg publication.

```sql
root_iceberg_catalog(
    catalog_directory
)
```

### Interface

| Argument | Effect |
|---|---|
| `catalog_directory` | Opens the Iceberg SQLite catalog stored in that directory |

The function reports the active Iceberg state of the metadata tables:

```text
root_index.files
root_index.schemas
root_index.access
root_index.baskets
root_index.snapshots
root_index.commits
```

Only tables that exist in the catalog are returned.

### Returned values

| Column | Meaning |
|---|---|
| `table_name` | Metadata table name |
| `iceberg_snapshot_id` | Current Iceberg snapshot identifier |
| `metadata_location` | Current Iceberg metadata file |
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

> **`root_iceberg_catalog(...)` shows which versioned metadata tables and Iceberg snapshots currently define the dataset.**

## Building from source

root4duckdb is built in two stages:

1. Apache Iceberg C++ is compiled once as a separate shared runtime.
2. DuckDB and the root4duckdb extension are built against that runtime.

```text
build-iceberg.sh
    ↓
shared Iceberg libraries

build-root4duckdb.sh
    ↓
DuckDB CLI + root4duckdb extension
```

### Requirements

- Linux x86_64
- CERN ROOT with a working `root-config`
- GCC 14+ or Clang 18+
- CMake 3.25+
- Git, Python 3, Make and Ninja
- SQLite development headers
- `nlohmann/json.hpp`

The first build requires network access to fetch pinned versions of Apache Iceberg C++, DuckDB and DuckDB extension tools.

### Build

```bash
unzip root4duckdb-v3.8.0-extension.zip
cd root4duckdb-v3.8.0-extension

chmod +x \
  build-iceberg.sh \
  build-root4duckdb.sh \
  setup-source-tree.sh \
  run-duckdb.sh \
  scripts/*.sh \
  scripts/production/*
```

Make ROOT available when `root-config` is not already in `PATH`:

```bash
source /path/to/root/bin/thisroot.sh
```

Build the shared Iceberg runtime:

```bash
./build-iceberg.sh --jobs 4
```

Then build root4duckdb and run the test suite:

```bash
./build-root4duckdb.sh --clean --tests --jobs 4
```

The scripts automatically:

- detect a compatible C++23 compiler;
- search CERN CVMFS toolchains when available;
- configure `CC`, `CXX`, `PATH` and `LD_LIBRARY_PATH`;
- restore a working Git HTTPS helper after a toolchain switch;
- fetch the exact pinned dependency commits;
- validate the source package and serialized codec;
- verify ROOT and `libstdc++` ABI compatibility;
- check the final dynamic-library boundary;
- run SQL and native integration tests.

### Build outputs

```text
.deps/iceberg-cpp/
    shared Apache Iceberg C++ runtime

build/release/duckdb
    DuckDB CLI with the statically registered root4duckdb extension

build/release/extension/root/root.duckdb_extension
    loadable DuckDB extension
```

### Run

Run the bundled CLI directly:

```bash
./build/release/duckdb
```

The root4duckdb extension is already registered in this binary. To load the extension into another ABI-compatible DuckDB build, use the produced loadable artifact:

```sql
LOAD './build/release/extension/root/root.duckdb_extension';
```

On CERN lxplus, the compiler and shared-library paths may disappear between shell sessions. In that environment, use:

```bash
./run-duckdb.sh
```

This wrapper is an lxplus/CVMFS environment fix. It restores the compiler, ROOT and shared Iceberg runtime paths, then executes the same `build/release/duckdb` binary.

### Rebuild after source changes

Iceberg does not need to be rebuilt when only root4duckdb source files change:

```bash
./scripts/rebuild-extension.sh --jobs 4
```

For a complete clean rebuild of root4duckdb:

```bash
./build-root4duckdb.sh --clean --tests --jobs 4
```

To select a compiler explicitly:

```bash
CC=/path/to/gcc CXX=/path/to/g++ \
  ./build-iceberg.sh --clean --jobs 4

./build-root4duckdb.sh --clean --tests --jobs 4
```

> **Apache Iceberg C++ is built once and remains outside the root4duckdb binary. Subsequent extension rebuilds reuse the same shared runtime.**

## Project status and roadmap

root4duckdb is currently a **research prototype**, not a production-ready data platform.

The main architectural ideas are implemented and validated: universal ROOT object traversal, relational flattening, selective serialized decoding, native multi-file execution, reusable basket metadata, SQL execution and Iceberg-backed dataset snapshots. However, the project still requires substantial engineering before it can be considered safe for general production use.

The [direct multi-file performance notes](docs/direct-multifile-scan.md#development-directions) connect these directions to the bottlenecks observed in the measured scan.

## Developer and contributing

root4duckdb is developed by **Seraphim S.**

**Contact:** [sserubin@jinr.ru](mailto:sserubin@jinr.ru)

root4duckdb is still an early research project, and contributions are welcome. Help is especially valuable in ROOT layout support, DuckDB optimization, Apache Iceberg integration, testing, packaging and distributed execution.

> **Help bring the core ideas to completion and turn the prototype into a reliable platform for querying large ROOT datasets.**
