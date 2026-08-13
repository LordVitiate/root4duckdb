# Direct scans, synthetic validation and measured performance

ROOT4DuckDB is tested at two different scales:

1. controlled synthetic ROOT files exercise semantic correctness across difficult C++ and ROOT layouts;
2. real remote experimental data measures sustained decoding, resource use and recovery behavior.

These tests answer different questions. The synthetic fixture checks whether SQL rows preserve ROOT semantics. The multi-file scan checks whether the same reader pipeline remains practical at billion-value scale.

## Measured performance

### 10.7 billion nested values on one node

On 2026-08-11, the native multi-file reader scanned a homogeneous collection of remote experimental ROOT files.

The query selected one deeply nested primitive field and computed a complete count and checksum:

```sql
SET threads = 14;
SET memory_limit = '8GB';

SELECT
    count(*) AS decoded_values,
    sum(CAST(value AS HUGEINT)) AS checksum
FROM read_root(
    '/data/run*.root*',
    dictionary := '/data/libExperiment.so',
    path_prefix := '/EventRecord/records/value'
);
```

The paths above show the measured query shape. The original files and experiment dictionary are not distributed with ROOT4DuckDB.

### Results

| Metric | Measured result |
|---|---:|
| Compute nodes | **1** |
| DuckDB worker threads | **14** |
| DuckDB memory limit | **8 GiB** |
| Remote ROOT files requested | **117** |
| Remote ROOT files contributing | **117** |
| Decoded primitive values | **10,696,574,044** |
| Wall time | **644.478 s** |
| Sustained value rate | **16.6 million values/s** |
| Observed remote-read throughput | **412–471 MiB/s** |
| Average CPU consumption | **4.49 cores** |
| Peak resident memory | **approximately 1.3 GiB** |
| Transient remote-open timeouts | **3** |
| Unrecovered files | **0** |
| Final checksum | `2784253092475411033` |

The complete scan finished in approximately **10 min 44 s**.

No intermediate event table was written. DuckDB consumed the decoded values directly and produced one aggregate row.

### What happened during the scan

```text
117 remote ROOT files
        ↓
lazy file opening
        ↓
compatible schema reuse
        ↓
nested collection decoding
        ↓
DuckDB vectorized aggregation
        ↓
count and checksum
```

Files were opened lazily instead of keeping all 117 files resident at once. Compatible inputs reused the same logical schema plan.

Three remote-open operations timed out transiently. The scheduler retried them, and all files eventually contributed to the final count and checksum.

### Interpretation

This measurement demonstrates that:

- the direct reader can scan more than ten billion nested values without materializing an intermediate dataset;
- memory use remains bounded far below the configured DuckDB limit;
- remote-open failures do not necessarily invalidate a long scan;
- a single schema-compatible plan can be reused across many homogeneous ROOT files;
- DuckDB can aggregate decoded ROOT values as they arrive.

It does **not** establish a universal ROOT4DuckDB speed.

Performance depends on:

- ROOT split level and physical branch layout;
- collection depth;
- compression algorithm and compression ratio;
- basket sizes;
- requested projections;
- serialized-reader eligibility;
- object fallback frequency;
- network and remote-storage load;
- file-open latency;
- available CPU and memory bandwidth.

The result should therefore be read as one measured workload, not as a fixed product-wide throughput guarantee.

## Synthetic ROOT semantic validation

Large production scans are useful performance tests, but they are poor debugging fixtures. Real experiment dictionaries contain many classes, historical schema versions and framework-specific behavior.

A separate deterministic fixture, `make_r4d_stress.C`, was used on 2026-08-12 to exercise the reader against known values and deliberately difficult layouts.

### Generated files

The fixture generated equivalent event models using several ROOT storage layouts:

| File | Split level | Purpose |
|---|---:|---|
| `r4d_stress_split.root` | `99` | Fully split object layout |
| persistent-ancestor variant | `1` | Members stored below a persistent physical ancestor |
| `r4d_stress_unsplit.root` | `0` | Complete object payload stored unsplit |

Each primary fixture contained **12 deterministic events**.

The persistent-ancestor variant covers layouts in which a logical field exists in `TStreamerInfo`, but no independent physical leaf branch exists for that field.

### Data model coverage

The synthetic model intentionally combined:

- class inheritance;
- nested objects;
- vectors of objects;
- nested vectors;
- fixed-size arrays;
- multidimensional values;
- `std::string`;
- `std::vector<bool>`;
- `std::map`;
- nullable pointers;
- signed and unsigned ROOT integer types;
- values close to the exact-integer boundary of IEEE-754 `double`.

This is not intended to imitate one experiment class. It isolates the storage patterns that a universal ROOT reader must handle.

### Observed query results

The validation queries covered independent parts of the model rather than checking only one convenient scalar.

| Probe | Observed result |
|---|---:|
| Nested track scalar | **24 rows** |
| Nested hit detector field | **12 rows** |
| Split fixed-array probe | **15 rows** |
| Unsplit fixed-array probe over the first three events | **36 rows** |
| Helix-like nested object probe | **6 rows** |
| Nested residual collection | **18 rows** |
| Group collection | **11 rows** |
| Map keys | **6 rows** |
| Map values | **6 rows** |
| String field probe | **6 rows** |

These counts are fixture-specific expected results. Their purpose is to detect dropped elements, duplicated rows, incorrect collection indices and accidental changes in flattening behavior.

They are not performance measurements.

### Nullable pointer behavior

The fixture mixed null and non-null object pointers.

The reader preserved the distinction:

- a null pointer became SQL `NULL`;
- non-null pointed objects produced their stored values;
- the checked non-null quality values included `901` and `902`.

A null object was not converted into a default-constructed value.

### Exact 64-bit integer transport

The fixture included unsigned values close to \(2^{53}\), where transporting integers through `double` can silently lose precision.

The observed unsigned values were preserved exactly:

```text
9007199254740990
9007199254740991
9007199254740992
9007199254740993
9007199254740994
```

The corresponding signed values were also preserved exactly:

```text
-9007199254740990
-9007199254740991
-9007199254740992
-9007199254740993
-9007199254740994
```

This test specifically guards against an implementation that internally normalizes every ROOT numeric value through floating-point transport.

The SQL type, signedness and integer payload must survive independently.

## Reader-mode contract

ROOT4DuckDB exposes three reader policies:

| Mode | Contract |
|---|---|
| `object` | Use ROOT object reconstruction as the semantic reference |
| `serialized` | Require direct serialized decoding and reject unsupported layouts |
| `auto` | Use serialized decoding only when the layout is supported; otherwise fall back to object reconstruction |

The synthetic tests exercise this distinction explicitly.

### Object mode

Object reconstruction is the universal reference path. It relies on ROOT dictionaries, `TClass` and `TStreamerInfo` to recover the requested logical value.

It is expected to handle both split and unsplit objects, although unsplit inputs can require reconstruction of a larger parent object.

### Serialized mode

Forced serialized mode is intentionally strict.

If the physical layout cannot be decoded safely, the query must fail with a clear unsupported-layout error. It must not:

- return a default value;
- reinterpret an unsigned value as signed;
- approximate a 64-bit integer through `double`;
- silently switch to object reconstruction;
- emit a partially decoded relation.

A rejection in forced serialized mode is therefore an expected validation result for an unsupported layout.

### Automatic mode

Automatic mode preserves SQL correctness while allowing optimized decoding where it is proven safe.

For the unsigned 64-bit fixture, automatic mode preserved the exact values through object fallback when the serialized path could not guarantee correct typed decoding.

```text
logical SQL request
        ↓
inspect physical ROOT layout
        ↓
serialized layout supported?
   ├── yes → selective serialized decoding
   └── no  → ROOT object reconstruction
        ↓
same logical SQL type and value
```

Fallback changes the final decoding mechanism. It does not change the requested logical path or permit lossy numeric conversion.

## Differential validation

Where both readers support the same layout, correctness is checked relationally.

A row count alone is insufficient: the same count can still contain different values or duplicated collection indices.

The comparison therefore uses both directions of `EXCEPT ALL`:

```sql
WITH
object_rows AS (
    SELECT *
    FROM read_root(
        'fixture.root',
        path_prefix := '/R4DEvent/field',
        reader_mode := 'object'
    )
),
serialized_rows AS (
    SELECT *
    FROM read_root(
        'fixture.root',
        path_prefix := '/R4DEvent/field',
        reader_mode := 'serialized'
    )
),
object_only AS (
    SELECT * FROM object_rows
    EXCEPT ALL
    SELECT * FROM serialized_rows
),
serialized_only AS (
    SELECT * FROM serialized_rows
    EXCEPT ALL
    SELECT * FROM object_rows
)
SELECT
    (SELECT count(*) FROM object_rows) AS object_rows,
    (SELECT count(*) FROM serialized_rows) AS serialized_rows,
    (SELECT count(*) FROM object_only) AS object_only,
    (SELECT count(*) FROM serialized_only) AS serialized_only;
```

A successful differential check requires:

```text
object_rows = serialized_rows
object_only = 0
serialized_only = 0
```

`EXCEPT ALL` is important because it preserves duplicate multiplicity. Plain `EXCEPT` could hide a duplicated or missing nested element.

## Measured reader comparison

A separate experimental-data comparison measured both decoding paths on the same relational result:

| Reader | Rows | Wall time |
|---|---:|---:|
| ROOT object reconstruction | **66,506** | **1.708 s** |
| Serialized decoding | **66,506** | **0.539 s** |
| Bidirectional row difference | **0** | — |

The measured serialized-reader speedup was:

\[
\frac{1.708}{0.539} \approx 3.17
\]

This is an end-to-end measurement for that selected field and layout. It is not used as a universal serialized-reader multiplier.

The important correctness condition is the zero relational difference. The timing is meaningful only after that condition is satisfied.

## Indexed selective-read measurements

The sidecar-index pipeline was also measured independently of the complete direct scan.

The indexed dataset contained:

```text
15,496,396 primitive values
2,960 ROOT baskets
```

### Metadata-only count

A count resolved from sidecar metadata completed without opening ROOT payload data:

| Metric | Result |
|---|---:|
| Logical values | **15,496,396** |
| Wall time | **0.082 s** |
| Selected ROOT files | **0** |

This is metadata evaluation, not a 15-million-value decode benchmark.

### Entry-range selection

A range predicate selected:

| Metric | Result |
|---|---:|
| Available baskets | **2,960** |
| Selected baskets | **1** |
| Selected entries | **5** |
| Decoded values | **25** |
| Selected basket bytes | **9,657** |

The optimization reduced the work before primitive decoding began:

```text
15,496,396 indexed values
        ↓
1 of 2,960 baskets
        ↓
5 ROOT entries
        ↓
25 decoded values
```

### Equality lookup

A tested equality predicate produced one matching row after metadata pruning selected **26 of 2,960 baskets**.

Min/max statistics and Bloom filters reduced the candidate set. DuckDB still evaluated the final predicate over decoded candidate values; metadata was not treated as proof that every value in a selected basket matched.

### Complete aggregate

A full aggregate over all **15,496,396** values completed in **25.7 s** in the measured environment.

Unlike the metadata-only count, this query decoded the primitive values and executed the aggregate in DuckDB.

## What the combined tests establish

The test groups cover complementary parts of the pipeline:

| Test | Primary question |
|---|---|
| Synthetic split/unsplit fixture | Are ROOT semantics preserved? |
| Integer boundary fixture | Are signedness and 64-bit values lossless? |
| Object-versus-serialized comparison | Does optimization return the same relation? |
| Indexed selective query | Is unnecessary ROOT input pruned before decoding? |
| Full indexed aggregate | Does the complete value path remain operational? |
| 117-file remote scan | Does direct decoding scale on one node? |
| Timeout recovery | Can transient remote failures be recovered? |

Together they support a stronger claim than a single throughput number:

> ROOT4DuckDB preserves a universal object-based correctness path, applies selective serialized decoding only where supported, prunes indexed input before reading, and can scan more than ten billion nested primitive values on one node without materializing an intermediate event dataset.

## What remains outside these measurements

These tests do not yet constitute:

- a comparison across all ROOT compression algorithms;
- a multi-node distributed benchmark;
- a controlled comparison with RDataFrame or Uproot on identical hardware;
- a benchmark of every supported ROOT class;
- a malformed-file or fuzzing campaign;
- a guarantee that every unsplit class is eligible for serialized decoding;
- a fixed throughput expectation for remote storage;
- a replacement for experiment-specific physics validation.

The synthetic fixture is deliberately small because its purpose is deterministic semantic coverage. The remote scan is deliberately large because its purpose is operational scale.

Neither test should be used for the other purpose.

## Reproducing the measurements

For a meaningful reproduction, record at least:

- ROOT4DuckDB commit;
- DuckDB version;
- ROOT version;
- compiler and standard-library version;
- file count;
- logical path;
- reader mode;
- split level;
- compression algorithm;
- total compressed input bytes;
- worker thread count;
- DuckDB memory limit;
- local or remote storage;
- wall time;
- CPU time;
- peak resident memory;
- decoded value count;
- checksum or differential result;
- retry and skipped-file counters.

A performance result without a value count and correctness invariant is incomplete.

For complete scans, use an aggregate checksum so that values must actually pass through the reader:

```sql
SELECT
    count(*) AS decoded_values,
    sum(CAST(value AS HUGEINT)) AS checksum
FROM read_root(
    'input*.root',
    dictionary := 'libExperiment.so',
    path_prefix := '/EventRecord/records/value'
);
```

For optimized reader validation, compare the complete relations in both directions with `EXCEPT ALL`.

For indexed queries, inspect `EXPLAIN ANALYZE` and record:

- selected ROOT files;
- selected baskets;
- selected entries;
- selected bytes;
- decoded values;
- object fallback entries.

This separates metadata pruning, physical I/O and final decoding instead of collapsing all three stages into one timing.
