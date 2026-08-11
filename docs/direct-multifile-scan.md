# Direct multi-file scans

`read_root(...)` accepts a file, directory, shell-style glob, comma-separated list, JSON string array, or `@file` list. Multi-file execution is selected automatically when expansion produces more than one unique input.

```sql
SET threads = 10;

SELECT count(*), sum(value)
FROM read_root(
    '/data/run*.root*',
    dictionary := '/data/libExperiment.so',
    path_prefix := '/Event/records/value'
);
```

No scan-policy parameter is required. The worker limit is derived from `SET threads`, DuckDB's memory limit, the number of files, and the extension's existing memory admission control.

## Execution model

1. Input expansion preserves explicit list order and sorts the matches produced by each glob. Duplicate paths are removed.
2. Exact ROOT paths from a list are not probed or opened during expansion.
3. Bind opens only the first readable representative file and derives the SQL schema once.
4. Execution workers claim whole files from a shared queue. Each worker opens its claimed file lazily, keeps file affinity until it is exhausted, closes the object context safely, and then claims another file.
5. Per-file serialized plans are prepared against the already-bound logical path. Compatible fingerprints are counted as plan reuse. Object reconstruction remains the automatic fallback in `reader_mode := 'auto'`.
6. A failed open is retried automatically with a small bounded budget. Other workers continue independently. If an input remains unavailable or is incompatible, the query ends with a consolidated failure instead of returning a silently partial result.

The first SQL column remains `event_id`, which is the entry number inside one ROOT file. Multi-file results additionally expose:

| Column | Type | Meaning |
|---|---|---|
| `source_id` | `UBIGINT` | Stable zero-based position after input expansion |
| `source_path` | `VARCHAR` | Resolved file path |

This makes `(source_id, event_id, nested indices...)` a stable row identity. A predicate on `source_id` is pushed into the file queue and prevents unrelated files from being opened.

## Profiling

`EXPLAIN ANALYZE` reports the representative file and bind-open time together with opened, completed, skipped, failed and retried files; total execution open time; schema variants and plan reuses; time to first row; serialized/object counters; and the slowest completed file.

```sql
EXPLAIN ANALYZE
SELECT count(*), sum(value)
FROM read_root(
    '@/data/inputs.list',
    dictionary := '/data/libExperiment.so',
    path_prefix := '/Event/records/value'
);
```

For one-off scans this path avoids generating a large `UNION ALL` plan and allows remote opens and decoding to overlap. `read_root_dataset(...)` remains the preferred production interface when persistent basket statistics and file pruning are worth building in advance.

## Regression checks

The integration suite compares a two-file glob and an `@list` with their known count and checksum, verifies source identity and source pruning, exercises a nested logical path, and checks that a missing list entry produces a consolidated error. Run it after rebuilding the extension:

```bash
./scripts/rebuild-extension.sh --jobs 14
./scripts/run-integration-test.sh
```
