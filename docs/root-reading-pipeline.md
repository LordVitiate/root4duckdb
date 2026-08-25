# ROOT reading pipeline

This document describes the current ROOT4DuckDB read pipeline from input resolution to SQL rows.

It is intentionally narrower than the general architecture documentation. The focus here is the execution path used by `read_root(...)`, how object and serialized decoding cooperate, how single-file and multi-file parallelism are scheduled, and where indexed dataset execution differs.

For user-facing API details, see [SQL interface](sql-interface.md). For ROOT layout examples, see [Reading ROOT files](reading-root-files.md). For large direct scans and measurements, see [Direct multi-file scans](direct-multifile-scan.md).

---

## 1. Pipeline at a glance

A direct query has five distinct stages:

```text
input specification
        |
        v
RootInputResolver
        |
        v
representative ROOT file
        |
        v
semantic relation binding
        |
        +----> root_describe(...) for metadata-only browsing
        |
        v
projection + filter planning
        |
        v
work scheduling
        |
        +----> single file: entry ranges
        |
        +----> multiple files: file-affine workers
        |
        v
per-worker ROOT state
        |
        +----> object reader
        |
        +----> serialized reader
        |          |
        |          +----> shared basket cache per physical branch
        |
        v
DuckDB vectors
        |
        v
SQL filters / aggregates / joins
```

The main design rule is:

> **The SQL relation is selected semantically; physical ROOT branches and serialized offsets are implementation details.**

The public API therefore starts from objects and collections rather than primitive leaf addresses.

---

## 2. Input resolution

All ROOT entry points use the common `RootInputResolver` source layer.

The resolver accepts:

- one exact ROOT file;
- a directory;
- a local or filesystem-supported glob;
- comma-separated inputs;
- a JSON string array;
- a local `@file` containing one input URI per line.

Examples:

```sql
FROM read_root('/data/run001.root', ...)

FROM read_root('/data/run*.root*', ...)

FROM read_root(
    '["/data/run001.root","/data/run002.root"]',
    ...
)

FROM read_root('@/data/inputs.list', ...)
```

Input expansion is deterministic:

1. explicit list order is preserved;
2. matches produced by a glob are sorted;
3. duplicate paths are removed;
4. exact paths are not synchronously opened merely to test their existence.

The last point matters for remote production scans. A list containing thousands of remote files must not turn bind into thousands of serial network probes.

### Exact remote URIs

An exact remote URI is passed to the common ROOT opener without rewriting it.

This includes schemes handled by the installed ROOT backend, for example:

```text
root://...
s3://...
davix://...
```

ROOT4DuckDB does not accept storage credentials as SQL arguments. Authentication remains the responsibility of the ROOT/Davix environment, for example `.rootrc`, process environment variables, or site configuration.

This keeps secrets out of:

- SQL text;
- prepared plans;
- debug logs;
- query history.

### Remote wildcard expansion

Opening one remote object and listing a remote namespace are different operations.

For example:

```text
s3://bucket/data/run001.root
```

can be opened directly by a suitable ROOT backend, while:

```text
s3://bucket/data/run*.root
```

requires a filesystem implementation capable of listing that URI scheme.

If wildcard listing is unavailable, use an explicit list instead:

```text
@inputs.list
```

where each line is an exact remote URI.

---

## 3. Representative-file binding

`read_root(...)` binds one SQL schema before execution.

For multiple inputs, bind opens the first readable representative file. The representative file is used to determine:

- the ROOT class;
- the semantic path;
- immediate SQL value columns;
- collection index columns;
- candidate physical branches;
- serialized-layout information;
- the SQL types returned to DuckDB.

Other files are checked against this bound relation when workers open them.

This keeps the DuckDB schema stable across the query while still allowing compatible ROOT layout variants to be handled at execution time.

A file that cannot be opened is not silently treated as an empty file. Direct multi-file execution reports unavailable inputs explicitly; schema or decoding incompatibility remains an error.

---

## 4. Public semantic path contract

The current public contract is deliberately object-oriented.

`path_prefix` in `read_root(...)` selects a **relation**, not a primitive scalar leaf.

For example, use:

```sql
SELECT entry_id, vecTrack_idx, chi2tot, nmeas
FROM read_root(
    'events.root',
    dictionary := 'libExperiment.so',
    path_prefix := '/PaEvent/vecTrack'
);
```

not:

```text
/PaEvent/vecTrack/chi2tot
```

The selected object or collection becomes one SQL relation.

Its immediate primitive and string members become SQL value columns when they share the selected collection geometry.

Conceptually:

```text
ROOT
/PaEvent
└── vecTrack : vector<PaTrack>
    ├── chi2tot : float
    ├── nmeas   : int
    ├── qop     : double
    └── vecTPar : vector<PaTPar>
```

becomes:

```text
entry_id | vecTrack_idx | chi2tot | nmeas | qop
---------+--------------+---------+-------+----
...
```

`vecTPar` is not recursively flattened into the same relation. It is a nested child relation that can be selected separately.

This rule prevents one `read_root(...)` call from accidentally exploding an arbitrarily deep object graph.

### Primitive leaves are not public relation selectors

A primitive leaf such as:

```text
/PaEvent/vecTrack/chi2tot
```

remains meaningful internally as a logical value address, but direct `read_root(...)` rejects it as a public relation selector.

Select the parent relation and project the SQL column:

```sql
SELECT entry_id, vecTrack_idx, chi2tot
FROM read_root(
    'events.root',
    dictionary := 'libExperiment.so',
    path_prefix := '/PaEvent/vecTrack'
);
```

### Why leaf paths still exist internally

Leaf addresses are still useful where metadata belongs to an individual logical value.

In particular, index metadata and serialized plans may refer to concrete logical leaves because:

- min/max statistics describe one logical value;
- Bloom filters describe one logical value;
- physical basket mappings are attached to a logical value;
- serialized offsets terminate at a concrete value.

So the public direct-read relation model and the internal/index value-address model intentionally coexist.

---

## 5. Browsing one semantic level with `root_describe`

`root_describe(...)` is the metadata companion to `read_root(...)`.

It describes **exactly one logical level** below a selected semantic path.

Example:

```sql
SELECT *
FROM root_describe(
    'events.root',
    '/PaEvent/vecTrack',
    dictionary := 'libExperiment.so'
);
```

The result exposes:

```text
path
name
kind
root_type
is_primitive
is_string
is_container
is_fixed_array
is_pointer
```

Typical `kind` values include:

```text
PRIMITIVE
STRING
OBJECT
CONTAINER
FIXED_ARRAY
```

The purpose is navigation, not data materialization.

For example:

```text
/PaSetup/rich/ddetRot
```

can be inspected without recursively reading all descendants. Primitive children are candidates for the current SQL relation; nested objects and collections indicate paths that can be explored in the next step.

Associative containers expose synthetic `key` and `value` children. Primitive containers expose a synthetic `value` child in metadata.

The intended workflow is therefore:

```text
root_describe(parent)
        |
        +--> primitive/string child --> SQL column of parent relation
        |
        +--> object/container child  --> candidate next relation
```

---

## 6. Row identity

For direct scans, the public ROOT entry identifier is:

```text
entry_id
```

It is the zero-based `TTree` entry number within one ROOT source.

Nested collections add one index column per variable-size level.

For example:

```text
entry_id | vecTrack_idx | chi2tot
```

or:

```text
entry_id | vecParticle_idx | vecVertex_idx | value
```

For multi-file direct scans, source identity is added:

```text
source_id
source_path
```

The stable direct row address is therefore:

```text
(source_id, entry_id, nested indices...)
```

for multi-file input, and:

```text
(entry_id, nested indices...)
```

for one file.

`entry_id` intentionally describes ROOT storage identity. It must not be confused with an experiment-level event number stored as physics data inside the event object.

Indexed dataset relations retain their own dataset-facing identity columns, including `event_fk`, because they represent persisted index metadata rather than the public direct-scan row contract.

---

## 7. Projection planning

DuckDB tells the table function which columns are required by the query.

ROOT4DuckDB uses this information before decoding.

For example:

```sql
SELECT chi2tot
FROM read_root(
    'events.root',
    dictionary := 'libExperiment.so',
    path_prefix := '/PaEvent/vecTrack'
)
WHERE entry_id < 100000;
```

does not require `nmeas`, `qop`, or unrelated nested members to be emitted.

The planner separates three concepts:

```text
semantic relation
        |
        v
requested SQL columns
        |
        v
physical ROOT inputs required to produce them
```

This distinction is important because several logical columns may live in one physical ROOT branch.

A projection can therefore be narrow logically even when the smallest safe ROOT read unit is wider physically.

---

## 8. Entry filter planning

Filters on storage identity are converted into scheduling bounds before decoding.

For one file:

```sql
WHERE entry_id >= 100000
  AND entry_id < 200000
```

limits the entry range claimed by workers.

For multiple files:

```sql
WHERE source_id = 7
```

prunes other sources before workers open them.

Impossible storage-identity predicates terminate without decoding.

Value predicates are still evaluated against the produced relation. Direct `read_root(...)` has no persistent dataset statistics, so arbitrary value predicates normally cannot eliminate files or baskets before ROOT access.

That is the role of the indexed pipeline described later.

---

## 9. Single-file parallel execution

Reader mode does **not** determine worker count.

A single sufficiently large ROOT file is divided into entry work units. The current default work-unit size is:

```text
65,536 entries
```

Conceptually:

```text
one ROOT file
     |
     +--> entries      0 ..  65,535 --> worker A
     +--> entries 65,536 .. 131,071 --> worker B
     +--> entries 131,072 .. 196,607 --> worker C
     +--> ...
```

The number of active workers is bounded by:

- DuckDB `SET threads`;
- the number of available work units;
- ROOT4DuckDB runtime admission limits.

Each worker owns its own mutable ROOT state.

It does **not** share a `TFile`, `TTree`, `TBranch` cursor, object reader, or serialized cursor with another worker.

The ownership rule is:

```text
DuckDB worker
    |
    └── RootScanLocalState
         ├── TFile
         ├── TTree
         ├── TBranch objects
         ├── object reader state
         └── serialized reader state
```

This is what allows object mode and serialized mode to use multiple CPUs safely on one large file.

---

## 10. Multi-file parallel execution

When input expansion produces several sources, direct execution uses file-affine scheduling.

Conceptually:

```text
shared source queue
     |
     +--> worker A --> source 0 --> source 4 --> ...
     +--> worker B --> source 1 --> source 5 --> ...
     +--> worker C --> source 2 --> source 6 --> ...
     +--> worker D --> source 3 --> source 7 --> ...
```

A worker:

1. claims a source;
2. opens it lazily;
3. prepares its local reader state;
4. keeps affinity to that file until the selected range is complete;
5. closes the local ROOT state;
6. claims another source.

This avoids constructing a huge SQL `UNION ALL` plan and allows remote opens, basket reads, decompression and decoding from independent files to overlap.

`root_max_in_flight_files` and the ROOT memory admission budget limit the number of expensive simultaneous ROOT contexts. They are resource controls, not a request to serialize decoding.

### Current granularity

For multi-file direct scans the scheduling unit is primarily the file.

For one large file the scheduling unit is an entry range.

This distinction matters when a workload contains only a few extremely large files: single-file entry scheduling can expose more parallel work than file-only multi-file scheduling. A future scheduler can unify these into `(source, entry-range)` tasks if measurements show that the additional complexity is justified.

---

## 11. Two decoding paths

After semantic binding and scheduling, ROOT4DuckDB can obtain values using two compatible decoding paths.

```text
selected source + selected entry
              |
              v
      decoding decision
         /          \
        /            \
 serialized          object
    reader            reader
```

Both must produce the same SQL relation.

### Object reader

The object reader is the universal correctness path.

It asks ROOT to reconstruct the object and then traverses it using dictionary and streamer metadata.

Conceptually:

```text
TTree::GetEntry(entry_id)
        |
        v
ROOT reconstructs object
        |
        v
TStreamerInfo / collection traversal
        |
        v
requested primitive members
```

It handles layouts for which direct serialized extraction is unavailable or cannot be proven safe.

The object reader is not a single-thread fallback. Each DuckDB worker has its own ROOT object state, so object mode participates in the same parallel scheduling model.

### Serialized reader

For supported layouts, the serialized reader extracts requested values from the serialized ROOT entry payload without reconstructing the complete C++ object graph.

Conceptually:

```text
compressed ROOT basket
        |
        v
decompressed entry payload
        |
        v
validated serialized layout plan
        |
        v
requested logical member
```

Supported paths include primitive members and validated nested container layouts. Unsupported cases fall back to the object reader in `reader_mode := 'auto'`.

Serialized mode changes how values are decoded. It does not change source resolution or worker scheduling.

---

## 12. `reader_mode`

The main modes are:

```text
auto
object
serialized
```

### `auto`

`auto` is the normal safety-oriented mode.

It uses serialized decoding only where the layout is supported and validated. Unsupported columns or layouts use object reconstruction.

For multi-column relations this decision is not required to be all-or-nothing. Supported logical columns can use serialized decoding while another required value uses the object fallback, subject to consistent row geometry.

### `object`

`object` forces ROOT object reconstruction.

Use it for:

- correctness reference measurements;
- unsupported serialized layouts;
- debugging dictionary/streamer behavior;
- direct comparison against serialized output.

### `serialized`

`serialized` forces the serialized path for supported requested values.

It is appropriate after the relevant file generation and logical relation have passed correctness comparison against object mode.

`raw_validation_entries` controls validation sampling where applicable. Setting it to zero removes that runtime check and should be reserved for already validated production workloads.

---

## 13. Multi-column serialized relations

A relation can expose several sibling primitive columns from the same selected object or collection.

Example:

```sql
SELECT
    entry_id,
    vecTrack_idx,
    chi2tot,
    nmeas
FROM read_root(
    'events.root',
    dictionary := 'libExperiment.so',
    path_prefix := '/PaEvent/vecTrack',
    reader_mode := 'serialized'
);
```

The serialized pipeline maintains per-column decode state while enforcing common row geometry.

This is necessary because the SQL relation is row-oriented:

```text
(entry_id, vecTrack_idx) -> {chi2tot, nmeas, ...}
```

while ROOT may store members in a member-wise physical representation.

Sibling readers therefore agree on the collection shape before values are emitted. A geometry mismatch is treated as a correctness problem rather than silently zipping unrelated sequences.

---

## 14. Shared serialized basket cache

Multi-column serialized reading introduced an important physical optimization.

Several requested logical columns can come from the **same physical `TBranch`**.

Without coordination, each column reader could independently:

1. locate the same basket;
2. load it;
3. decompress it;
4. parse the same entry envelope.

That duplicates the most expensive common work.

ROOT4DuckDB now creates a `SerializedBasketCache` per physical branch inside one worker-local file context.

Conceptually:

```text
worker-local TBranch
        |
        v
SerializedBasketCache
      /      \
     /        \
chi2tot     nmeas
 reader      reader
```

The cache shares basket loading and decompression state across sibling serialized column readers.

Important ownership rule:

> **The basket cache is shared only inside one worker's ROOT context. ROOT objects are not shared between worker threads.**

The cache holds non-owning references to the worker-local branch/basket state; lifetime remains owned by `RootScanLocalState` and its ROOT file handle.

This preserves the thread-safety boundary while removing redundant basket work.

### Why decoding is still column-linear

ROOT member-wise storage is often effectively column-like inside the serialized payload:

```text
chi2tot[0..N]
nmeas[0..N]
...
```

For that layout, alternating fields element-by-element would worsen locality.

The current design therefore shares common basket/entry preparation but lets each logical column decode its contiguous region linearly.

Experiments with more aggressively fused sibling decoding did not show a useful gain and were not retained.

---

## 15. Object fallback in a mixed projection

A query in `auto` mode can request several values where only some are supported by the serialized reader.

The intended execution is:

```text
requested columns
      |
      +--> serialized-supported columns
      |          |
      |          └--> shared basket cache
      |
      +--> unsupported column(s)
                 |
                 └--> object reconstruction fallback
```

Fallback is coordinated at the physical-branch/object level where possible so one unsupported column does not imply repeatedly reconstructing the same object independently for every sibling field.

Correctness takes priority over retaining serialized mode for every projected column.

---

## 16. ROOT object lifetime and thread safety

ROOT4DuckDB enables ROOT thread safety, but it does not rely on concurrent mutation of one ROOT reader object.

The execution boundary is explicit:

```text
global state
    |
    +--> immutable/shared scheduling information
    +--> counters
    +--> source queue / entry cursor
    |
    +--> worker-local state A
    |      └--> independent ROOT file/readers
    |
    +--> worker-local state B
    |      └--> independent ROOT file/readers
    |
    +--> worker-local state C
           └--> independent ROOT file/readers
```

Shared state contains coordination, counters and immutable plans.

Mutable ROOT I/O state stays local to one DuckDB worker.

This rule applies equally to:

- object decoding;
- serialized decoding;
- basket caches;
- validation readers.

---

## 17. Primitive and string conversion

ROOT primitive values are converted through the common typed primitive layer rather than being universally coerced through `double`.

This preserves integer width and signedness where the SQL type allows it.

Examples include:

```text
Char_t / char
UChar_t / unsigned char
signed and unsigned 64-bit integers
compressed floating-point ROOT types
named enum values
```

The conversion layer is shared by object and serialized paths so that changing the physical decoder does not change SQL value semantics.

---

## 18. Containers and indexes

Collection structure becomes relational identity.

A variable-size container contributes an index column:

```text
vector<Track>
    |
    +--> vecTrack_idx
```

Nested containers contribute additional indices:

```text
vector<Particle>
    └── vector<short> vertex_ids
```

becomes:

```text
entry_id
vecParticle_idx
vertex_ids_idx
value
```

Fixed arrays also preserve positional identity when flattened.

Associative containers expose key/value semantics. Primitive containers expose `value`.

The relation boundary remains explicit: immediate members that share one collection geometry can become sibling SQL columns; a deeper nested collection is normally selected as a separate relation.

---

## 19. Direct multi-file failures and retries

Remote file opening uses the common ROOT opener.

The direct multi-file path can retry bounded transient open failures. If a source remains unavailable, the query reports that fact rather than silently pretending the file contained zero rows.

Single exact-file failure remains an error.

Schema incompatibility and decoding errors remain errors rather than file-availability skips.

This distinction is deliberate:

```text
temporary source availability problem
        !=
data/schema correctness problem
```

`EXPLAIN ANALYZE` exposes ROOT-specific counters so partial I/O behavior is visible.

---

## 20. From direct scan to DuckDB vectors

After an entry is decoded, ROOT4DuckDB writes values directly into DuckDB output vectors.

Conceptually:

```text
ROOT entry
   |
   v
row identity
   |
   +--> entry_id
   +--> collection indexes
   +--> source identity, if multi-file
   |
   v
requested primitive/string values
   |
   v
DuckDB DataChunk
```

DuckDB then continues normal relational execution:

```text
filters
aggregates
joins
window functions
sorting
materialization
```

ROOT4DuckDB is therefore a table-function execution layer, not a separate query engine.

---

## 21. Indexed dataset pipeline

`read_root_dataset(...)` adds a planning stage before ROOT decoding.

The direct path is:

```text
input
  -> bind
  -> schedule
  -> decode ROOT
  -> SQL
```

The indexed path is:

```text
Iceberg / sidecar metadata
        |
        v
snapshot resolution
        |
        v
file pruning
        |
        v
basket pruning
        |
        v
entry selection
        |
        v
ROOT decode
        |
        v
SQL
```

The event payload remains in ROOT.

The sidecar stores compact metadata such as:

- source identity;
- logical path;
- schema fingerprint;
- physical branch/ancestor mapping;
- entry ranges;
- basket ranges and compressed sizes;
- flattened value counts;
- null/special-value counts;
- min/max statistics;
- optional Bloom filters.

This lets queries reject irrelevant physical data before opening or decoding it.

### Direct relation paths versus indexed logical leaves

The two layers have different jobs.

`read_root(...)` is relation-oriented:

```text
path_prefix := '/PaEvent/vecTrack'
SELECT chi2tot, nmeas
```

Index construction and sidecar statistics may retain concrete logical value paths such as:

```text
/PaEvent/vecTrack/chi2tot
```

because statistics belong to values, not to an entire heterogeneous object.

Do not remove internal leaf addressing merely because direct SQL now binds object/collection relations.

---

## 22. Common source and open layer

Direct scans, dataset scans and index construction use the same source/opening policy.

The architectural boundary is:

```text
RootInputResolver
        |
        v
resolved source URI
        |
        v
OpenRootFile
        |
        +--> direct read
        +--> serialized validation
        +--> index build
        +--> indexed dataset execution
        +--> root_describe
```

This prevents each subsystem from developing different behavior for:

- `.root` and numbered `.root.NNN` files;
- globs;
- URI lists;
- remote errors;
- S3;
- ROOT/Davix opening.

Source syntax and ROOT I/O policy therefore have one owner.

---

## 23. Current parallelism validation

The current implementation has been checked on real nested ROOT data with both object and serialized decoding.

The following numbers are representative validation measurements, not universal performance claims.

### One large file

For a two-column nested relation with identical row checksum:

| Mode | Threads | Rows | Wall time |
|---|---:|---:|---:|
| object | 1 | 3,130,035 | 14.446 s |
| object | 8 | 3,130,035 | 2.745 s |
| serialized | 1 | 3,130,035 | 14.596 s |
| serialized | 8 | 3,130,035 | 2.930 s |

All four runs produced the same checksum:

```text
1008965967005258526
```

The important result is correctness plus real parallel execution:

```text
object:      14.446 s -> 2.745 s
serialized:  14.596 s -> 2.930 s
```

Object mode is therefore not restricted to one CPU.

### Eleven-file mask

A mask resolving to 11 sources also produced identical object/serialized results.

Small smoke scan:

```text
rows      = 137431
sources   = 11
checksum  = 12929826357376929484
```

Larger serialized scan:

| Threads | Rows | Sources | Wall time |
|---:|---:|---:|---:|
| 1 | 2,148,913 | 11 | 18.826 s |
| 8 | 2,148,913 | 11 | 4.019 s |

Checksum:

```text
11451661629936212168
```

These measurements show that both single-file entry scheduling and multi-file scheduling are active.

They do **not** imply that serialized decoding is always faster than object reconstruction. When both modes must load and decompress the same physical basket, I/O and common materialization costs can dominate. Serialized decoding has its largest advantage when it avoids reconstruction of substantial unused object payload.

---

## 24. Correctness contract

Performance changes are accepted only if row identity and values remain stable.

A useful direct comparison checks:

- row count;
- `(entry_id, indices...)`;
- all projected values;
- both directions of `EXCEPT ALL`, or an equivalent deterministic row checksum.

For multi-file input, include `source_id`.

Example pattern:

```sql
WITH object_rows AS (
    SELECT entry_id, vecTrack_idx, chi2tot, nmeas
    FROM read_root(
        getvariable('root_file'),
        dictionary := getvariable('root_dictionary'),
        path_prefix := '/PaEvent/vecTrack',
        reader_mode := 'object'
    )
),
serialized_rows AS (
    SELECT entry_id, vecTrack_idx, chi2tot, nmeas
    FROM read_root(
        getvariable('root_file'),
        dictionary := getvariable('root_dictionary'),
        path_prefix := '/PaEvent/vecTrack',
        reader_mode := 'serialized'
    )
)
SELECT count(*)
FROM (
    SELECT * FROM object_rows
    EXCEPT ALL
    SELECT * FROM serialized_rows
);
```

The reverse `EXCEPT ALL` must also be zero.

Checksums are useful for large benchmarks, but fixture tests should still compare exact rows where practical.

---

## 25. Integration-test coverage

The integration suite covers the main semantic and physical cases, including:

- root-level scalar members;
- split and unsplit objects;
- inline objects;
- variable-size object collections;
- primitive vectors;
- fixed arrays;
- maps, sets and pairs;
- inheritance;
- signed and unsigned primitive types;
- large integer values;
- compressed floating-point types;
- named enums;
- nested collection geometries;
- object and serialized modes;
- automatic fallback;
- multiple projected sibling columns;
- multi-file input forms;
- unavailable input handling;
- `entry_id` filtering;
- `source_id` filtering;
- `root_describe`;
- dataset index construction and reads.

The integration runner derives its expected number of direct checks from the generated SQL instead of relying on a hard-coded test count. Adding a new `SELECT` test therefore does not require manually updating an unrelated literal.

---

## 26. Observability

`EXPLAIN ANALYZE` is the primary way to inspect physical work.

Depending on the execution path, ROOT4DuckDB reports counters such as:

```text
resolved ROOT inputs
selected ROOT files
opened ROOT files
unavailable ROOT files
selected baskets
selected compressed bytes
skipped ROOT entries
serialized entry calls
decoded values
emitted rows
metadata-only rows
object validation entries
object fallback entries
```

These counters matter more than wall time alone.

For example, a faster query can result from:

- fewer files opened;
- fewer baskets loaded;
- fewer compressed bytes read;
- fewer entries decoded;
- less object reconstruction;
- more parallel overlap.

When object and serialized wall times converge, the next useful diagnostic is to compare physical work rather than adding more decoding abstractions blindly.

---

## 27. Choosing the right path

Use `root_describe(...)` when the question is:

```text
"What is immediately below this ROOT object/collection?"
```

Use direct `read_root(...)` when the question is:

```text
"Read this relation now from one file, a glob, or an explicit source list."
```

Use `reader_mode := 'object'` when the question is:

```text
"What is the universal ROOT/dictionary result?"
```

Use `reader_mode := 'serialized'` when:

```text
"This layout is already validated and selective serialized decoding is useful."
```

Use `read_root_dataset(...)` when:

```text
"I have reusable metadata and want file/basket/entry pruning before decoding."
```

The layers are complementary:

```text
root_describe
    -> semantic discovery

read_root
    -> direct relation access

object / serialized
    -> alternative decoding mechanisms

root_build_index
    -> persistent logical/physical metadata

read_root_dataset
    -> metadata-driven production scan
```

---

## 28. Design invariants

The current implementation is built around the following invariants.

1. **ROOT files remain the source of truth.** Event payload is not converted merely to become queryable.
2. **Public direct paths select relations.** Users select objects or collections and project SQL columns.
3. **Primitive leaf addresses remain internal where value-level metadata requires them.**
4. **`entry_id` is ROOT storage identity, not a physics event number.**
5. **Multi-file identity includes `source_id`.**
6. **Reader mode does not control parallelism.**
7. **Mutable ROOT state is worker-local.**
8. **Serialized decoding is an optimization, not a second semantic model.**
9. **Object reconstruction is the universal fallback.**
10. **Sibling serialized readers share physical basket work when they use the same branch.**
11. **One semantic relation has one consistent row geometry.**
12. **Source resolution and file opening have common owners across direct, indexed and metadata paths.**
13. **Exact remote URI opening and remote wildcard listing are separate capabilities.**
14. **Credentials never belong in SQL parameters.**
15. **Correctness equivalence is required before performance conclusions.**

---

## 29. End-to-end example

A typical exploration flow now looks like this.

First inspect the event object:

```sql
SELECT *
FROM root_describe(
    'events.root',
    '/PaEvent',
    dictionary := 'libPhast.so'
);
```

Then inspect one child collection:

```sql
SELECT *
FROM root_describe(
    'events.root',
    '/PaEvent/vecTrack',
    dictionary := 'libPhast.so'
);
```

Then query several sibling fields as one relation:

```sql
SET threads = 8;

SELECT
    entry_id,
    vecTrack_idx,
    chi2tot,
    nmeas
FROM read_root(
    'events.root',
    dictionary := 'libPhast.so',
    path_prefix := '/PaEvent/vecTrack',
    reader_mode := 'auto'
)
WHERE entry_id < 500000;
```

For a validated production scan over several files:

```sql
SET threads = 8;

SELECT
    count(*) AS rows,
    count(DISTINCT source_id) AS sources
FROM read_root(
    '/data/run*.root*',
    dictionary := 'libPhast.so',
    path_prefix := '/PaEvent/vecTrack',
    reader_mode := 'serialized',
    raw_validation_entries := 0
)
WHERE entry_id < 500000;
```

If repeated selective queries justify persistent planning metadata, build value-level indexes and use `read_root_dataset(...)` so file and basket pruning happen before ROOT decoding.

---

## 30. Summary

The current ROOT4DuckDB read path separates four concerns that were historically easy to mix together:

```text
semantic relation
physical ROOT storage
parallel execution
decoding strategy
```

The user selects an object or collection.

`root_describe(...)` exposes one semantic level.

DuckDB projection determines which immediate values are needed.

The common resolver determines which files or URIs participate.

The scheduler determines which worker owns each file or entry range.

Each worker owns independent ROOT state.

The object reader provides the universal correctness path.

The serialized reader avoids unnecessary object reconstruction where the physical layout is understood and validated.

Sibling serialized columns share one worker-local basket cache when they come from the same physical branch.

Finally, DuckDB receives ordinary vectors with stable ROOT identity columns and executes the rest of the SQL plan normally.

That separation is the core of the current pipeline.
