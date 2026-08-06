![Footer](assets/images/IMG_20260806_215451.jpg)
<div align="center">

# root4duckdb

**A query-planning and SQL execution layer for large-scale ROOT datasets.**

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

</div>

> *“I owe much of my work to my own laziness. I disliked writing programs, so when I was working on the IBM 701, I began developing a system that would make programs easier to write.”*  
> — **John Backus**

## Contents

- [Data-reading and execution optimizations](#data-reading-and-execution-optimizations)
- [How root4duckdb reads a ROOT file](#how-root4duckdb-reads-a-root-file)
  - [Fully split layout](#fully-split-layout)
  - [Partially split layout](#partially-split-layout)
  - [Unsplit object](#unsplit-object)
- [Validation on real experimental data](#validation-on-real-experimental-data)
- [SQL interface](#sql-interface)
  - [read_root(...)](#read-root)
  - [root_build_index(...)](#root-build-index)
  - [read_root_dataset(...)](#read-root-dataset)
  - [root_dataset_stats(...)](#root-dataset-stats)
  - [root_iceberg_catalog(...)](#root-iceberg-catalog)
- [Building from source](#building-from-source)
- [Project status and roadmap](#project-status-and-roadmap)
- [Developer and contributing](#developer-and-contributing)

To keep these links stable, place the following anchors immediately before the corresponding headings:

<a id="fully-split-layout"></a>
<a id="partially-split-layout"></a>
<a id="unsplit-object"></a>
<a id="sql-interface"></a>
<a id="read-root"></a>
<a id="root-build-index"></a>
<a id="read-root-dataset"></a>
<a id="root-dataset-stats"></a>
<a id="root-iceberg-catalog"></a>

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

### 🟨⬜⬜ Fully split: physical branch to SQL column

This is the simplest case.

ROOT has already decomposed the object into a physical primitive branch:

```text
PaEvent
└── vecTrack
    └── chi2tot
```

The branch contains one variable-length sequence of `chi2tot` values for every `TTree` entry.

Each event already has a stable ROOT entry number:

```cpp
GetEntry(entry_id)
```

Each value inside `vecTrack` also has a natural position in that event:

```text
entry_id = 929
vecTrack_idx = 2
```

root4duckdb maps these components directly into a relational result:

| ROOT representation | SQL column |
|---|---|
| `TTree` entry number | `entry_id` |
| Position inside `vecTrack` | `vecTrack_idx` |
| `chi2tot` branch value | `chi2tot` |

The physical hierarchy therefore becomes an ordinary SQL relation:

```text
entry_id | vecTrack_idx | chi2tot
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
SELECT chi2tot
FROM read_root_dataset(...)
WHERE source_id = 'file-A'
  AND entry_id = 929
  AND vecTrack_idx = 2;
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
read only the chi2tot branch
    ↓
GetEntry(929)
    ↓
select vecTrack_idx = 2
    ↓
return the SQL value
```

No unrelated ROOT files are opened.

No unrelated branches are materialized.

No complete `PaEvent` or `PaTrack` object needs to be reconstructed.

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
    tree->SetBranchStatus("PaEvent.vecTrack.chi2tot", true);

    TTreeReader reader(tree);
    TTreeReaderArray<float> chi2tot(
        reader,
        "PaEvent.vecTrack.chi2tot"
    );

    constexpr Long64_t entry_id = 929;
    constexpr std::size_t vecTrack_idx = 2;

    if (reader.SetEntry(entry_id) != TTreeReader::kEntryValid) {
        throw std::runtime_error("Cannot read requested entry");
    }

    if (vecTrack_idx >= chi2tot.GetSize()) {
        throw std::out_of_range("Track index is outside the collection");
    }

    const float value = chi2tot[vecTrack_idx];
    std::cout << value << '\n';
}
```

ROOT already provides the essential physical operation:

```cpp
reader.SetEntry(929);
value = chi2tot[2];
```

root4duckdb does not replace that mechanism. It adds everything required to derive the same operation automatically from SQL across an entire dataset.

| Step | Native ROOT C++ | root4duckdb |
|---|---|---|
| Select dataset version | Application logic | Iceberg snapshot |
| Locate the file | Application logic | File pruning |
| Select the branch | `SetBranchStatus()` | Projection pushdown |
| Locate the basket | ROOT or custom logic | Sidecar planning |
| Select the event | `SetEntry(929)` | `entry_id = 929` |
| Select the track | `chi2tot[2]` | `vecTrack_idx = 2` |
| Skip impossible regions | Custom implementation | Min/max and Bloom pruning |
| Return the value | C++ code | SQL result |

> **For a fully split file, ROOT already provides efficient column access. root4duckdb turns that local mechanism into automatic, dataset-wide SQL planning.**

### 🟨🟨⬜ Partially split: extract a logical field from its physical parent

In the second case, the requested primitive value exists in the ROOT object model but does not have its own physical branch.

For example:

```text
PaEvent
└── vecTrack                 physical branch
    ├── chi2tot              logical field
    ├── ndf                  logical field
    └── momentum             logical field
```

ROOT stores and reads `vecTrack` as one physical unit. The logical field `chi2tot` is described by the class dictionary and `TStreamerInfo`, but it cannot be selected as an independent branch.

The physical and logical read units are therefore different:

```text
physical read unit
    = vecTrack branch

requested logical value
    = vecTrack[i].chi2tot
```

root4duckdb discovers the member path through the ROOT dictionary and exposes it through exactly the same SQL relation as a fully split branch:

```text
entry_id | vecTrack_idx | chi2tot
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
| Position inside `vecTrack` | `vecTrack_idx` |
| `PaTrack::chi2tot` member | `chi2tot` |

The difference appears only in the physical read plan.

For a fully split file:

```text
chi2tot SQL column
        ↓
chi2tot physical branch
```

For a partially split file:

```text
chi2tot SQL column
        ↓
PaTrack::chi2tot logical member
        ↓
vecTrack physical branch
```

> **The SQL column remains the same. Only the smallest physical ROOT object required to recover it changes.**

### Logical metadata over a physical parent branch

root4duckdb builds statistics for the requested logical field, even when ROOT stores that field inside a wider physical branch.

A sidecar entry may therefore describe both:

```text
logical path
    /PaEvent/vecTrack/chi2tot

physical branch
    PaEvent.vecTrack
```

For each physical basket, the sidecar can record statistics for the extracted logical values:

```text
vecTrack basket 42
├── entries: 900–949
├── chi2tot values: 287
├── chi2tot min: 0.02
├── chi2tot max: 18.74
└── chi2tot Bloom filter
```

The basket still belongs to the physical `vecTrack` branch. The min/max and Bloom metadata describe only the logical `chi2tot` values contained inside it.

This allows root4duckdb to prune using the logical SQL predicate before reading the wider physical parent:

```sql
WHERE chi2tot > 100
```

```text
chi2tot predicate
        ↓
logical min/max statistics
        ↓
reject impossible vecTrack baskets
        ↓
read only surviving physical baskets
```

The event data remain exclusively in ROOT:

```text
ROOT file
└── vecTrack baskets
    └── serialized PaTrack objects

sidecar
└── chi2tot statistics
    └── mappings to vecTrack baskets
```

### Publishing through Iceberg

The sidecars are published through the same Iceberg dataset model:

```text
Iceberg snapshot
├── ROOT file A
│   ├── physical vecTrack branch
│   └── chi2tot sidecar metadata
├── ROOT file B
│   ├── physical vecTrack branch
│   └── chi2tot sidecar metadata
└── ROOT file C
    ├── physical vecTrack branch
    └── chi2tot sidecar metadata
```

Iceberg identifies the active dataset version and its files. The sidecars provide logical statistics and physical basket mappings. ROOT remains responsible for storing the event objects.

### Reading the value back

Consider the same SQL lookup:

```sql
SELECT chi2tot
FROM read_root_dataset(...)
WHERE source_id = 'file-A'
  AND entry_id = 929
  AND vecTrack_idx = 2;
```

For a partially split file, the reverse read plan becomes:

```text
SQL predicate
    ↓
resolve the Iceberg snapshot
    ↓
select file-A
    ↓
resolve /PaEvent/vecTrack/chi2tot
    ↓
find the physical ancestor: PaEvent.vecTrack
    ↓
locate the basket containing entry 929
    ↓
read the vecTrack basket
    ↓
GetEntry(929)
    ↓
select vecTrack[2]
    ↓
extract chi2tot
    ↓
return one SQL value
```

No unrelated ROOT files are opened.

No unrelated physical branches are read.

The `vecTrack` parent must still be read because the ROOT layout stores `chi2tot` inside it. However, only the requested logical value is emitted and materialized as an SQL column.

> **root4duckdb does not claim to read four isolated bytes when ROOT stores a complete object. It reads the smallest self-contained physical ancestor and returns only the requested logical value.**

### Two ways to extract the member

After the physical basket has been selected, root4duckdb can recover the logical member in two ways.

```text
Object extraction
    vecTrack object
        ↓
    PaTrack element
        ↓
    chi2tot member
```

This uses ROOT dictionaries, `TStreamerInfo`, collection proxies and normal object reconstruction.

For supported and validated layouts, root4duckdb may instead extract the primitive member directly from the serialized entry payload:

```text
Serialized vecTrack entry
        ↓
validated member layout
        ↓
chi2tot value
```

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

#include "PaTrack.h"

int main() {
    TFile file("file-A.root", "READ");
    if (file.IsZombie()) {
        throw std::runtime_error("Cannot open ROOT file");
    }

    auto *tree = file.Get<TTree>("Events");
    if (!tree) {
        throw std::runtime_error("Cannot find TTree");
    }

    std::vector<PaTrack> *vecTrack = nullptr;

    tree->SetBranchStatus("*", false);
    tree->SetBranchStatus("PaEvent.vecTrack", true);
    tree->SetBranchAddress("PaEvent.vecTrack", &vecTrack);

    constexpr Long64_t entry_id = 929;
    constexpr std::size_t vecTrack_idx = 2;

    if (tree->GetEntry(entry_id) <= 0) {
        throw std::runtime_error("Cannot read requested entry");
    }

    if (!vecTrack || vecTrack_idx >= vecTrack->size()) {
        throw std::out_of_range("Track index is outside the collection");
    }

    const PaTrack &track = vecTrack->at(vecTrack_idx);
    const float value = track.chi2tot;

    std::cout << value << '\n';
}
```

The essential operation is:

```cpp
tree->GetEntry(929);
value = vecTrack->at(2).chi2tot;
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
| Select the collection element | `vecTrack.at(i)` | `vecTrack_idx = i` |
| Extract the primitive member | `track.chi2tot` | SQL value column |
| Prune by member statistics | Custom implementation | Min/max and Bloom pruning |
| Handle unsupported layouts | Custom implementation | Automatic object fallback |

> **For a partially split file, root4duckdb separates the logical SQL column from its wider physical ROOT container, adds statistics for that logical value, and reads the smallest physical ancestor required to recover it.**

### 🟨🟨🟨 Unsplit object: one SQL model, two decoding paths

In the third case, ROOT stores the requested value inside a complete self-contained object.

```text
PaEvent                         physical branch
└── vecTrack                    object member
    └── chi2tot                 requested logical value
```

Neither `vecTrack` nor `chi2tot` has an independent physical branch. The smallest self-contained physical unit is the complete `PaEvent` entry.

```text
physical read unit
    = PaEvent

requested logical value
    = PaEvent.vecTrack[i].chi2tot
```

ROOT therefore cannot select `chi2tot` as an independent physical column.

### Mapping the object to SQL

The physical layout changes, but the SQL representation remains the same:

| ROOT object model | SQL column |
|---|---|
| `TTree` entry number | `entry_id` |
| Position inside `vecTrack` | `vecTrack_idx` |
| `PaTrack::chi2tot` | `chi2tot` |

```text
entry_id | vecTrack_idx | chi2tot
---------|--------------|---------
929      | 0            | 1.84
929      | 1            | 3.12
929      | 2            | 0.97
930      | 0            | 2.41
```

The logical identity of every value is still:

```text
(entry_id, vecTrack_idx)
```

For deeper collections, one SQL index column is added for every variable-size level:

```text
entry_id | outer_idx | inner_idx | value
```

> **The ROOT serialization layout may change, but the SQL schema and relational identity remain stable.**

### Adding logical statistics

The event data remain inside the ROOT file.

root4duckdb builds sidecar metadata for the requested logical field and maps those statistics back to the physical `PaEvent` baskets that contain it.

```text
logical path
    /PaEvent/vecTrack/chi2tot

physical ancestor
    PaEvent
```

A sidecar may describe a physical basket like this:

```text
PaEvent basket 42
├── entries: 900–949
├── chi2tot values: 287
├── chi2tot min: 0.02
├── chi2tot max: 18.74
└── chi2tot Bloom filter
```

The basket belongs to the physical `PaEvent` branch. The statistics describe only the logical `chi2tot` values found inside it.

This allows a predicate such as:

```sql
WHERE chi2tot > 100
```

to reject impossible physical regions before any `PaEvent` object is decoded:

```text
chi2tot predicate
        ↓
logical min/max statistics
        ↓
reject impossible PaEvent baskets
        ↓
decode only surviving entries
```

> **Planning is performed using the logical SQL value, even when ROOT stores that value inside a complete object.**

### Publishing the dataset through Iceberg

The ROOT files and their sidecars are published as one versioned dataset:

```text
Iceberg snapshot
├── ROOT file A
│   ├── PaEvent baskets
│   └── chi2tot sidecar
├── ROOT file B
│   ├── PaEvent baskets
│   └── chi2tot sidecar
└── ROOT file C
    ├── PaEvent baskets
    └── chi2tot sidecar
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
SELECT chi2tot
FROM read_root_dataset(...)
WHERE source_id = 'file-A'
  AND entry_id = 929
  AND vecTrack_idx = 2;
```

The common part of the read plan is:

```text
SQL predicate
    ↓
resolve the Iceberg snapshot
    ↓
select file-A
    ↓
resolve /PaEvent/vecTrack/chi2tot
    ↓
identify PaEvent as the physical ancestor
    ↓
locate the basket containing entry 929
    ↓
read the selected PaEvent entry
```

At this point, root4duckdb has two possible decoding paths.

---

#### Serialized decoding

For a supported and validated layout, root4duckdb extracts the requested values directly from the serialized entry payload.

```text
selected PaEvent basket
        ↓
decompress the basket
        ↓
locate serialized entry 929
        ↓
follow the validated layout plan
        ↓
locate vecTrack element 2
        ↓
decode chi2tot
        ↓
emit one SQL value
```

Complete `PaEvent` and `PaTrack` C++ objects are not constructed.

```text
physical input
    = complete serialized PaEvent entry

decoded output
    = requested chi2tot values
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
serialized PaEvent payload
    ↓
vecTrack_idx
    ↓
chi2tot
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
    tracks[vecTrack_idx],
    chi2tot_plan
);
```

The real decoder uses a plan derived from ROOT streamer metadata and accepts only layouts that pass its safety checks.

---

#### Object reconstruction

If direct serialized decoding is unsupported or fails validation, root4duckdb uses ROOT's universal object mechanism.

```text
selected PaEvent basket
        ↓
TTree::GetEntry(929)
        ↓
ROOT reconstructs PaEvent
        ↓
traverse vecTrack
        ↓
select element 2
        ↓
read chi2tot
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
    event->vecTrack.at(vecTrack_idx);

const float value =
    track.chi2tot;
```

The actual universal reader does not hard-code `PaEvent`, `vecTrack` or `chi2tot`. It builds the traversal dynamically from the loaded ROOT dictionary.

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
| Physical ROOT input | `PaEvent` payload | `PaEvent` payload |
| Complete `PaEvent` construction | No | Yes |
| Complete `PaTrack` construction | No | Usually yes |
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

The sidecar layer is small compared with the event data it describes. It does not duplicate ROOT payloads; it stores compact ranges, counts, statistics and optional Bloom filters. Even under a deliberately pessimistic estimate, a **230 TB** dataset would require only **hundreds of gigabytes of metadata**—well below **1%** of the original data volume—and most of that worst-case footprint would come from Bloom filters. Without aggressive Bloom indexing, the core file, basket, range and min/max metadata are considerably smaller.

> **Speed comes first from pruning, then from reading only the required physical data, and finally from avoiding unnecessary object reconstruction.**

The integration suite also covers split and unsplit objects, nested collections, arrays, inheritance, mixed layouts, automatic object fallback and Iceberg publication.

Timings describe the measured validation workload and environment; performance on other systems may differ.

### `read_root(...)`

`read_root(...)` exposes one ROOT file as a DuckDB relation.

It reads logical fields through ROOT dictionaries and `TStreamerInfo`. A prebuilt sidecar index or Iceberg catalog is not required.

```sql
read_root(
    root_file,
    dictionary := '...',
    path_prefix := '...',
    reader_mode := 'auto'
)
```

### Interface

| Argument | Default | Effect |
|---|---:|---|
| `root_file` | required | ROOT file to open |
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

For example:

```text
/Event/vecTrack/chi2
```

becomes:

```text
event_id | vecTrack_idx | chi2
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
    vecTrack_idx,
    chi2
FROM read_root(
    '/data/events.root',
    dictionary := '/data/libExperiment.so',
    path_prefix := '/Event/vecTrack/chi2',
    reader_mode := 'auto'
)
WHERE event_id >= 929
  AND event_id < 934;
```

Projection pushdown prevents unused logical columns from being materialized and applies physical branch projection when the ROOT layout permits it. An `event_id` range also limits the entries scheduled for reading.

Value predicates are evaluated during the direct scan, but `read_root(...)` has no dataset sidecar statistics for file or basket pruning.

> **Use `read_root(...)` for direct single-file access, schema exploration and reader validation. Use `read_root_dataset(...)` when reusable metadata-driven pruning is required.**

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
        "/Event/vecTrack/chi2",
        "/Event/vecTrack/nHits"
    ]',
    '/data/root-index',
    dictionary := '/data/libExperiment.so',
    reader_mode := 'auto',
    index_threads := 4,
    catalog_mode := 'sqlite',
    allow_partial := false
);
```

The function returns one status row per ROOT file:

```text
file_path | entries | flattened_values | baskets | status | snapshot_id
```

The generated metadata contains logical schemas, object traversal plans, file statistics, basket ranges, value counts, min/max statistics and optional Bloom filters.

> **`root_build_index(...)` performs the expensive ROOT traversal once so that later queries can eliminate irrelevant files and baskets before decoding begins.**

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
    vecTrack_idx,
    value AS chi2
FROM read_root_dataset(
    '/data/root-index',
    '/Event/vecTrack/chi2',
    dictionary := '/data/libExperiment.so',
    reader_mode := 'auto'
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
    '/Event/vecTrack/chi2'
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

```bash
./run-duckdb.sh
```

Use the wrapper instead of launching `build/release/duckdb` directly. It restores the compiler, ROOT and shared Iceberg runtime paths required by the built binary.

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

The main architectural ideas are implemented and validated: universal ROOT object traversal, relational flattening, reusable basket metadata, selective decoding, SQL execution and Iceberg-backed dataset snapshots. However, the project still requires substantial engineering before it can be considered safe for general production use.

### Priorities

- [ ] **Simplify the build**  
  Reduce the compiler, ROOT, DuckDB and Iceberg integration complexity and provide reproducible binary packages.

- [ ] **Validate the full ROOT type system**  
  Extend integration tests across primitive types, arrays, inheritance, pointers, nested STL containers, split levels and schema variations.

- [ ] **Refactor the reader and planner**  
  Separate semantic traversal, physical layout discovery, serialized decoding, indexing and query planning into stable internal components.

- [ ] **Complete query optimizations**  
  Improve projection pushdown, multi-column predicate planning, Bloom-filter selection, basket scheduling, aggregate pushdown and early termination.

- [ ] **Support additional ROOT structures**  
  Add first-class SQL access to objects such as `TH1`, `TH2`, `TGraph`, `RNTuple` and other common ROOT data structures.

- [ ] **Improve catalog integration**  
  Harden Iceberg publication, snapshot updates, schema evolution, recovery and interoperability with external catalogs.

- [ ] **Add distributed execution**  
  Distribute indexing and query tasks across multiple machines while preserving deterministic snapshots, bounded memory usage and exact row identity.

- [ ] **Run larger production-scale validation**  
  Test complete experimental datasets, heterogeneous file generations, remote storage, failures, retries and concurrent readers.

- [ ] **Build a DAG-based execution layer**  
  Represent indexing and query work as an explicit dependency graph that can be distributed across multiple nodes, resumed after failures and inspected at every stage. The DAG must preserve deterministic snapshots, exact lineage, bounded resource usage, retries and validation before publication.

Contributions are particularly valuable in ROOT layout coverage, DuckDB optimization, Iceberg interoperability, packaging and distributed execution.

> **The current release demonstrates the architecture and its performance potential. The next stage is turning that working prototype into a maintainable production system.**

## Developer and contributing

root4duckdb is developed by **Seraphim S.**

**Contact:** [sserubin@jinr.ru](mailto:sserubin@jinr.ru)

root4duckdb is still an early research project, and contributions are welcome. Help is especially valuable in ROOT layout support, DuckDB optimization, Apache Iceberg integration, testing, packaging and distributed execution.

> **Help bring the core ideas to completion and turn the prototype into a reliable platform for querying large ROOT datasets.**
