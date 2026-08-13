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
---------|------------|--------
929      | 0          | 1.84
929      | 1          | 3.12
929      | 2          | 0.97
930      | 0          | 2.41
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
    ├── quality            logical field
    ├── ndf                logical field
    └── momentum           logical field
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
---------|------------|--------
929      | 0          | 1.84
929      | 1          | 3.12
929      | 2          | 0.97
930      | 0          | 2.41
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
EventRecord                       physical branch
└── tracks                        object member
    └── quality                   requested logical value
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
---------|------------|--------
929      | 0          | 1.84
929      | 1          | 3.12
929      | 2          | 0.97
930      | 0          | 2.41
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
