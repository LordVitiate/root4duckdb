#!/usr/bin/env bash
set -euo pipefail

PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
FIXTURE_DIR="$PROJECT_DIR/test/fixtures"
WORK_DIR="${ROOT4DUCKDB_TEST_DIR:-$PROJECT_DIR/test/.tmp-root-lakehouse}"
DUCKDB_BIN="${DUCKDB_BIN:-$PROJECT_DIR/build/release/duckdb}"

if [[ ! -x "$DUCKDB_BIN" ]]; then
    DISCOVERED_DUCKDB="$(find "$PROJECT_DIR/build/release" -type f -name duckdb -perm -111 2>/dev/null | head -n 1 || true)"
    [[ -n "$DISCOVERED_DUCKDB" ]] && DUCKDB_BIN="$DISCOVERED_DUCKDB"
fi
for executable in root-config rootcling; do
    if ! command -v "$executable" >/dev/null 2>&1; then
        echo "missing required executable: $executable" >&2
        exit 2
    fi
done
if [[ ! -x "$DUCKDB_BIN" ]]; then
    echo "DuckDB shell not found: $DUCKDB_BIN" >&2
    exit 2
fi

profile_metric() {
    local label="$1"
    python3 -c '
import re
import sys

label = sys.argv[1]
text = sys.stdin.read()
position = text.find(label)
if position < 0:
    raise SystemExit(2)
match = re.search(r"\d+", text[position + len(label):position + len(label) + 512])
if match is None:
    raise SystemExit(2)
print(match.group(0))
' "$label"
}

rm -rf "$WORK_DIR"
mkdir -p "$WORK_DIR/data" "$WORK_DIR/build"

ROOT_CFLAGS="$(root-config --cflags)"
ROOT_LIBS="$(root-config --libs)"
CXX_BIN="${CXX:-c++}"

rootcling \
    -f "$WORK_DIR/build/TestEventDict.cpp" \
    -c \
    -I"$FIXTURE_DIR" \
    "$FIXTURE_DIR/TestEvent.hpp" \
    "$FIXTURE_DIR/TestEventLinkDef.h"

# shellcheck disable=SC2086
"$CXX_BIN" -std=c++17 -O2 -fPIC -shared \
    $ROOT_CFLAGS \
    -I"$FIXTURE_DIR" \
    "$FIXTURE_DIR/TestEvent.cpp" \
    "$WORK_DIR/build/TestEventDict.cpp" \
    -o "$WORK_DIR/build/libTestEvent.so" \
    $ROOT_LIBS

# shellcheck disable=SC2086
"$CXX_BIN" -std=c++17 -O2 \
    $ROOT_CFLAGS \
    -I"$FIXTURE_DIR" \
    "$FIXTURE_DIR/make_fixture.cpp" \
    -L"$WORK_DIR/build" -lTestEvent \
    -Wl,-rpath,"$WORK_DIR/build" \
    -o "$WORK_DIR/build/make_fixture" \
    $ROOT_LIBS

"$WORK_DIR/build/make_fixture" "$WORK_DIR/data"
printf '%s\n' "$WORK_DIR/data/a.root" "$WORK_DIR/data/b.root" > "$WORK_DIR/data/two-files.list"

# Bind-only regression: EXPLAIN invokes table-function bind but must not
# materialize a ROOT entry or recursively discover the complete schema.
cat > "$WORK_DIR/direct-bind.sql" <<SQL
EXPLAIN
SELECT *
FROM read_root(
    '$WORK_DIR/data/a.root',
    dictionary := '$WORK_DIR/build/libTestEvent.so',
    path_prefix := '/TestEvent/run'
);
EXPLAIN
SELECT *
FROM read_root(
    '$WORK_DIR/data/a.root',
    dictionary := '$WORK_DIR/build/libTestEvent.so',
    path_prefix := '/TestEvent/vecHit/uv'
);
SQL
MALLOC_CHECK_=3 "$DUCKDB_BIN" -csv -noheader :memory: < "$WORK_DIR/direct-bind.sql" >/dev/null

cat > "$WORK_DIR/direct-read.sql" <<SQL
LOAD parquet;
SELECT
    count(*) = 5
    AND abs(sum(u) - 53.0) < 0.00001
    AND sum(event_id) = 2
    AND sum(vecHit_idx) = 6 AS direct_nested_read_ok
FROM read_root(
    '$WORK_DIR/data/a.root',
    dictionary := '$WORK_DIR/build/libTestEvent.so',
    path_prefix := '/TestEvent/vecHit/u'
);
SELECT count(*) = 5
       AND sum(run) = 704
       AND count(DISTINCT source_id) = 2
       AND count(DISTINCT source_path) = 2
       AND min(event_id) = 0 AND max(event_id) = 2 AS direct_glob_scalar_ok
FROM read_root(
    '$WORK_DIR/data/[ab].root',
    dictionary := '$WORK_DIR/build/libTestEvent.so',
    path_prefix := '/TestEvent/run'
);
SELECT count(*) = 9
       AND abs(sum(u) - 102.5) < 0.00001
       AND sum(event_id) = 4
       AND sum(source_id) = 4 AS direct_glob_nested_ok
FROM read_root(
    '$WORK_DIR/data/[ab].root',
    dictionary := '$WORK_DIR/build/libTestEvent.so',
    path_prefix := '/TestEvent/vecHit/u'
);
SELECT count(*) = 5
       AND sum(run) = 704
       AND count(DISTINCT source_id) = 2 AS direct_uri_list_ok
FROM read_root(
    '@$WORK_DIR/data/two-files.list',
    dictionary := '$WORK_DIR/build/libTestEvent.so',
    path_prefix := '/TestEvent/run'
);
SELECT count(*) = 2
       AND sum(run) = 401
       AND min(source_id) = 1 AND max(source_id) = 1 AS direct_source_pruning_ok
FROM read_root(
    '$WORK_DIR/data/[ab].root',
    dictionary := '$WORK_DIR/build/libTestEvent.so',
    path_prefix := '/TestEvent/run'
)
WHERE source_id = 1;
SELECT count(*) = 0 AS missing_path_is_safe
FROM read_root(
    '$WORK_DIR/data/a.root',
    dictionary := '$WORK_DIR/build/libTestEvent.so',
    path_prefix := '/TestEvent/does_not_exist'
);
SELECT count(*) = 3 AND sum(run) = 303 AS direct_scalar_read_ok
FROM read_root(
    '$WORK_DIR/data/a.root',
    dictionary := '$WORK_DIR/build/libTestEvent.so',
    path_prefix := '/TestEvent/run'
);
SELECT count(*) = 3 AND count(flags) = 3 AND min(flags) = 0 AND max(flags) = 2 AND sum(flags) = 3 AS direct_uchar_read_ok
FROM read_root(
    '$WORK_DIR/data/a.root',
    dictionary := '$WORK_DIR/build/libTestEvent.so',
    path_prefix := '/TestEvent/flags'
);
SELECT count(*) = 3 AND count(signed_code) = 3 AND min(signed_code) = -1 AND max(signed_code) = 1 AND sum(signed_code) = 0 AS direct_char_read_ok
FROM read_root(
    '$WORK_DIR/data/a.root',
    dictionary := '$WORK_DIR/build/libTestEvent.so',
    path_prefix := '/TestEvent/signed_code'
);
SELECT count(*) = 4 AND min(event_id) = 0 AND max(event_id) = 0 AS direct_filter_semantics_ok
FROM read_root(
    '$WORK_DIR/data/a.root',
    dictionary := '$WORK_DIR/build/libTestEvent.so',
    path_prefix := '/TestEvent/vecHit/u'
)
WHERE event_id < 1;
SELECT count(*) = 9 AND min(vertex_idx) = 0 AND max(vertex_idx) = 2 AND sum(vertex) = 99 AS direct_fixed_array_ok
FROM read_root(
    '$WORK_DIR/data/a.root',
    dictionary := '$WORK_DIR/build/libTestEvent.so',
    path_prefix := '/TestEvent/vertex'
);
SELECT count(*) = 18
       AND min(matrix_dim0_idx) = 0 AND max(matrix_dim0_idx) = 1
       AND min(matrix_dim1_idx) = 0 AND max(matrix_dim1_idx) = 2
       AND sum(matrix) = 225 AS direct_multidim_array_ok
FROM read_root(
    '$WORK_DIR/data/a.root',
    dictionary := '$WORK_DIR/build/libTestEvent.so',
    path_prefix := '/TestEvent/matrix'
);
SELECT count(*) = 3 AND min(inherited) = 9000 AND max(inherited) = 9002 AS direct_inherited_ok
FROM read_root(
    '$WORK_DIR/data/a.root',
    dictionary := '$WORK_DIR/build/libTestEvent.so',
    path_prefix := '/TestEvent/inherited'
);
SELECT count(*) = 6 AND abs(sum(key) - 9.0) < 0.00001 AS direct_map_key_ok
FROM read_root(
    '$WORK_DIR/data/a.root',
    dictionary := '$WORK_DIR/build/libTestEvent.so',
    path_prefix := '/TestEvent/mapScore/key'
);
SELECT count(*) = 6 AND abs(sum(mapScore) - 607.5) < 0.00001 AS direct_map_value_ok
FROM read_root(
    '$WORK_DIR/data/a.root',
    dictionary := '$WORK_DIR/build/libTestEvent.so',
    path_prefix := '/TestEvent/mapScore/value'
);
SELECT count(*) = 6 AND sum(setCode) = 36 AS direct_set_ok
FROM read_root(
    '$WORK_DIR/data/a.root',
    dictionary := '$WORK_DIR/build/libTestEvent.so',
    path_prefix := '/TestEvent/setCode/value'
);
SELECT count(*) = 9 AND abs(sum(first) - 31.5) < 0.00001 AS direct_nested_pair_first_ok
FROM read_root(
    '$WORK_DIR/data/a.root',
    dictionary := '$WORK_DIR/build/libTestEvent.so',
    path_prefix := '/TestEvent/nestedPairs/value/value/first'
);
SELECT count(*) = 9 AND sum(second) = 1809 AS direct_nested_pair_second_ok
FROM read_root(
    '$WORK_DIR/data/a.root',
    dictionary := '$WORK_DIR/build/libTestEvent.so',
    path_prefix := '/TestEvent/nestedPairs/value/value/second'
);
-- Filter columns are evaluated inside read_root even when COUNT(*) projects
-- none of them. This also validates exact event-range clipping.
SELECT count(*) = 1 AS direct_filter_pushdown_ok
FROM read_root(
    '$WORK_DIR/data/a.root',
    dictionary := '$WORK_DIR/build/libTestEvent.so',
    path_prefix := '/TestEvent/vecHit/u'
)
WHERE event_id = 2 AND u = 11;
SELECT count(*) = 3 AND abs(sum(u) - 6.0) < 0.00001 AS serialized_direct_reader_ok
FROM read_root(
    '$WORK_DIR/data/ancestor.root',
    dictionary := '$WORK_DIR/build/libTestEvent.so',
    path_prefix := '/TestEvent/vecHit/u',
    reader_mode := 'serialized',
    raw_validation_entries := 2
);
SELECT count(*) = 5
       AND sum(refs) = 30
       AND min(vecHit_idx) = 0 AND max(vecHit_idx) = 1
       AND min(refs_idx) = 0 AND max(refs_idx) = 1
       AS serialized_nested_primitive_vector_ok
FROM read_root(
    '$WORK_DIR/data/ancestor.root',
    dictionary := '$WORK_DIR/build/libTestEvent.so',
    path_prefix := '/TestEvent/vecHit/refs/value',
    reader_mode := 'serialized',
    raw_validation_entries := 2
);
SQL

DIRECT_RESULT="$(MALLOC_CHECK_=3 "$DUCKDB_BIN" -csv -noheader :memory: < "$WORK_DIR/direct-read.sql")"
if [[ "$DIRECT_RESULT" != $'true\ntrue\ntrue\ntrue\ntrue\ntrue\ntrue\ntrue\ntrue\ntrue\ntrue\ntrue\ntrue\ntrue\ntrue\ntrue\ntrue\ntrue\ntrue\ntrue\ntrue' ]]; then
    echo "unexpected direct semantic read result: $DIRECT_RESULT" >&2
    exit 1
fi

printf '%s\n' "$WORK_DIR/data/a.root" "$WORK_DIR/data/missing.root" > "$WORK_DIR/data/with-missing.list"
cat > "$WORK_DIR/direct-missing-input.sql" <<SQL
SELECT sum(run)
FROM read_root(
    '@$WORK_DIR/data/with-missing.list',
    dictionary := '$WORK_DIR/build/libTestEvent.so',
    path_prefix := '/TestEvent/run'
);
SQL
if ! MALLOC_CHECK_=3 "$DUCKDB_BIN" -csv -noheader :memory: \
    < "$WORK_DIR/direct-missing-input.sql" > "$WORK_DIR/direct-missing-input.log" 2>&1; then
    echo "multi-file direct scan failed instead of skipping an unavailable input" >&2
    cat "$WORK_DIR/direct-missing-input.log" >&2
    exit 1
fi
if ! grep -q "\[ROOT4DUCKDB\]\[WARN\]\[ROOT_FILE_UNAVAILABLE\].*missing.root" \
    "$WORK_DIR/direct-missing-input.log"; then
    echo "multi-file missing-input warning was not emitted" >&2
    cat "$WORK_DIR/direct-missing-input.log" >&2
    exit 1
fi
if ! grep -q '^303$' "$WORK_DIR/direct-missing-input.log"; then
    echo "multi-file missing-input scan did not preserve the readable-file result" >&2
    cat "$WORK_DIR/direct-missing-input.log" >&2
    exit 1
fi

# The normal scan must select the contiguous std::vector<T> path, and disabling
# it must produce byte-for-byte identical SQL output through the proxy fallback.
ROOT4DUCKDB_DEBUG=1 ROOT4DUCKDB_DEBUG_VERBOSE=1 MALLOC_CHECK_=3 "$DUCKDB_BIN" -csv -noheader :memory: \
    > "$WORK_DIR/contiguous.csv" 2> "$WORK_DIR/contiguous.log" <<SQL
SELECT event_id, vecHit_idx, u
FROM read_root(
    '$WORK_DIR/data/a.root',
    dictionary := '$WORK_DIR/build/libTestEvent.so',
    path_prefix := '/TestEvent/vecHit/u'
)
ORDER BY event_id, vecHit_idx;
SQL
if ! grep -q '\[VECTOR.CONTIGUOUS\].*vector<TestHit>' "$WORK_DIR/contiguous.log"; then
    echo "contiguous vector path was not selected" >&2
    cat "$WORK_DIR/contiguous.log" >&2
    exit 1
fi
ROOT4DUCKDB_DISABLE_CONTIGUOUS_VECTOR=1 MALLOC_CHECK_=3 "$DUCKDB_BIN" -csv -noheader :memory: \
    > "$WORK_DIR/proxy-fallback.csv" <<SQL
SELECT event_id, vecHit_idx, u
FROM read_root(
    '$WORK_DIR/data/a.root',
    dictionary := '$WORK_DIR/build/libTestEvent.so',
    path_prefix := '/TestEvent/vecHit/u'
)
ORDER BY event_id, vecHit_idx;
SQL
cmp "$WORK_DIR/contiguous.csv" "$WORK_DIR/proxy-fallback.csv"

cat > "$WORK_DIR/build-index.sql" <<SQL
LOAD parquet;
CREATE TEMP TABLE build_status AS
SELECT *
FROM root_build_dataset_index(
    '$WORK_DIR/data/[abc].root',
    'Events',
    '["/TestEvent/vecHit/u","/TestEvent/run"]',
    '$WORK_DIR/index',
    dictionary := '$WORK_DIR/build/libTestEvent.so',
    index_threads := 2,
    max_in_flight_files := 2,
    bloom_bytes := 64,
    allow_partial := false
);
SELECT count(*) AS indexed_files
FROM build_status
WHERE status = 'OK';
SQL

BUILD_RESULT="$(MALLOC_CHECK_=3 "$DUCKDB_BIN" -csv -noheader :memory: < "$WORK_DIR/build-index.sql")"
if [[ "$BUILD_RESULT" != "3" ]]; then
    echo "unexpected index build result: $BUILD_RESULT" >&2
    exit 1
fi

BLOOM_FORMAT_RESULT="$("$DUCKDB_BIN" -csv -noheader :memory: <<SQL
LOAD parquet;
SELECT count(*) > 0
       AND bool_and(substr(hex(bloom_filter), 1, 8) = '52344246')
       AND min(octet_length(bloom_filter)) >= 24
FROM read_parquet('$WORK_DIR/index/snapshots/*/root_baskets.parquet')
WHERE bloom_filter IS NOT NULL;
SQL
)"
if [[ "$BLOOM_FORMAT_RESULT" != "true" ]]; then
    echo "adaptive Bloom header/version check failed: $BLOOM_FORMAT_RESULT" >&2
    exit 1
fi

python3 "$PROJECT_DIR/scripts/production/discover_dataset.py" \
    --input "$WORK_DIR/data/[abc].root" \
    --output "$WORK_DIR/fixture-manifest.json" >/dev/null
EXPECTED_SOURCE_IDS="$(python3 - "$WORK_DIR/fixture-manifest.json" <<'PY2'
import json,sys
print("\n".join(sorted(row["source_id"] for row in json.load(open(sys.argv[1]))["files"])))
PY2
)"
ACTUAL_SOURCE_IDS="$("$DUCKDB_BIN" -csv -noheader :memory: <<SQL
LOAD parquet;
SELECT DISTINCT source_id
FROM read_root_dataset(
    '$WORK_DIR/index',
    '/TestEvent/vecHit/u',
    dictionary := '$WORK_DIR/build/libTestEvent.so'
)
ORDER BY source_id;
SQL
)"
if [[ "$ACTUAL_SOURCE_IDS" != "$EXPECTED_SOURCE_IDS" ]]; then
    echo "Python discovery source_id differs from C++ FileId" >&2
    diff -u <(printf '%s\n' "$EXPECTED_SOURCE_IDS") <(printf '%s\n' "$ACTUAL_SOURCE_IDS") >&2 || true
    exit 1
fi

cat > "$WORK_DIR/build-scalar-index.sql" <<SQL
LOAD parquet;
SELECT count(*)
FROM root_build_index(
    '$WORK_DIR/data/a.root',
    'Events',
    '/TestEvent/run',
    '$WORK_DIR/index_scalar',
    dictionary := '$WORK_DIR/build/libTestEvent.so',
    catalog_mode := 'sqlite',
    index_threads := 1,
    bloom_bytes := 64,
    allow_partial := false
)
WHERE status = 'OK';
CREATE TEMP MACRO current_root_snapshot() AS (
    SELECT arg_max(root_snapshot_id, committed_at_ns)
    FROM read_parquet('$WORK_DIR/index_scalar/warehouse/root_index/commits/data/*.parquet')
);
SELECT count(*) = 3 AND min(value) = 100 AND max(value) = 102 AND sum(value) = 303
FROM read_root_dataset(
    '$WORK_DIR/index_scalar',
    '/TestEvent/run',
    dictionary := '$WORK_DIR/build/libTestEvent.so'
);
SELECT count(*) = 1
       AND count(index_signature) = 1
       AND min(index_signature) = ''
FROM read_parquet('$WORK_DIR/index_scalar/warehouse/root_index/schemas/data/*.parquet', filename = true)
WHERE logical_path = '/TestEvent/run'
  AND filename LIKE '%' || current_root_snapshot() || '-%';
SELECT count(*) = 1
       AND min(min_value) = 100
       AND max(max_value) = 102
       AND min(physical_offset) > 0
       AND min(compressed_size) > 0
       AND min(uncompressed_size) > 0
FROM read_parquet('$WORK_DIR/index_scalar/warehouse/root_index/baskets/data/*.parquet', filename = true)
WHERE filename LIKE '%' || current_root_snapshot() || '-%';
SQL

SCALAR_RESULT="$(MALLOC_CHECK_=3 "$DUCKDB_BIN" -csv -noheader :memory: < "$WORK_DIR/build-scalar-index.sql")"
if [[ "$SCALAR_RESULT" != $'1\ntrue\ntrue\ntrue' ]]; then
    echo "unexpected scalar index result: $SCALAR_RESULT" >&2
    exit 1
fi

cat > "$WORK_DIR/query.sql" <<SQL
LOAD parquet;
WITH actual AS (
    SELECT event_fk, vecHit_idx, value
    FROM read_root_dataset(
        '$WORK_DIR/index',
        '/TestEvent/vecHit/u',
        dictionary := '$WORK_DIR/build/libTestEvent.so',
        coalesce_gap_bytes := 65536,
        prefetch_ranges := true
    )
    WHERE value > 10 AND NOT isnan(value)
), expected(event_fk, vecHit_idx, value) AS (
    VALUES
        (0::UBIGINT, 1::INTEGER, 12.0::DOUBLE),
        (0::UBIGINT, 3::INTEGER, 18.0::DOUBLE),
        (2::UBIGINT, 0::INTEGER, 11.0::DOUBLE),
        (3::UBIGINT, 1::INTEGER, 20.0::DOUBLE),
        (4::UBIGINT, 1::INTEGER, 10.5::DOUBLE)
), difference AS (
    (SELECT * FROM actual EXCEPT SELECT * FROM expected)
    UNION ALL
    (SELECT * FROM expected EXCEPT SELECT * FROM actual)
)
SELECT
    (SELECT count(*) FROM actual) AS rows_found,
    (SELECT sum(value) FROM actual) AS value_sum,
    (SELECT sum(event_fk) FROM actual) AS event_fk_sum,
    (SELECT sum(vecHit_idx) FROM actual) AS vec_idx_sum,
    (SELECT count(*) FROM difference) AS differences,
    (SELECT count(*)
       FROM read_root_dataset(
           '$WORK_DIR/index', '/TestEvent/vecHit/u',
           dictionary := '$WORK_DIR/build/libTestEvent.so'
       )
      WHERE value > 10 AND NOT isnan(value)) AS count_projection_pushdown,
    (SELECT sum(event_fk)
       FROM read_root_dataset(
           '$WORK_DIR/index', '/TestEvent/vecHit/u',
           dictionary := '$WORK_DIR/build/libTestEvent.so'
       )
      WHERE value > 10 AND NOT isnan(value)) AS hidden_filter_column,
    (SELECT count(*)
       FROM read_root_dataset(
           '$WORK_DIR/index', '/TestEvent/vecHit/u',
           dictionary := '$WORK_DIR/build/libTestEvent.so'
       )
      WHERE isnan(value)) AS nan_rows,
    (SELECT count(*)
       FROM read_root_dataset(
           '$WORK_DIR/index', '/TestEvent/vecHit/u',
           dictionary := '$WORK_DIR/build/libTestEvent.so'
       )
      WHERE value > 10) AS duckdb_nan_ordering,
    (SELECT count(*)
       FROM read_root_dataset(
           '$WORK_DIR/index', '/TestEvent/vecHit/u',
           dictionary := '$WORK_DIR/build/libTestEvent.so'
       )
      WHERE value = 0.1) AS float_literal_bloom;
SQL

QUERY_RESULT="$(MALLOC_CHECK_=3 "$DUCKDB_BIN" -csv -noheader :memory: < "$WORK_DIR/query.sql")"
if [[ "$QUERY_RESULT" != "5,71.5,9,6,0,5,9,1,6,1" ]]; then
    echo "unexpected scan result: $QUERY_RESULT" >&2
    exit 1
fi


cat > "$WORK_DIR/fixed-array.sql" <<SQL
LOAD parquet;
SELECT count(*)
FROM root_build_index(
    '$WORK_DIR/data/a.root',
    'Events',
    '/TestEvent/vertex',
    '$WORK_DIR/index_vertex',
    dictionary := '$WORK_DIR/build/libTestEvent.so',
    catalog_mode := 'sqlite',
    index_threads := 1,
    bloom_bytes := 64,
    allow_partial := false
)
WHERE status = 'OK';
CREATE TEMP MACRO current_root_snapshot() AS (
    SELECT arg_max(root_snapshot_id, committed_at_ns)
    FROM read_parquet('$WORK_DIR/index_vertex/warehouse/root_index/commits/data/*.parquet')
);
SELECT count(*) = 9
       AND count(DISTINCT event_fk) = 3
       AND min(vertex_idx) = 0
       AND max(vertex_idx) = 2
       AND sum(value) = 99
FROM read_root_dataset(
    '$WORK_DIR/index_vertex',
    '/TestEvent/vertex',
    dictionary := '$WORK_DIR/build/libTestEvent.so'
);
SELECT index_signature = 'vertex_idx'
       AND root_type = 'float'
FROM read_parquet('$WORK_DIR/index_vertex/warehouse/root_index/schemas/data/*.parquet', filename = true)
WHERE logical_path = '/TestEvent/vertex'
  AND filename LIKE '%' || current_root_snapshot() || '-%';
SELECT count(*) = 1
       AND bool_and(is_fixed_array)
       AND min(array_length) = 3
       AND min(array_dimensions) = '3'
FROM read_parquet('$WORK_DIR/index_vertex/warehouse/root_index/access/data/*.parquet', filename = true)
WHERE field_name = 'vertex'
  AND filename LIKE '%' || current_root_snapshot() || '-%';
SQL

FIXED_ARRAY_RESULT="$(MALLOC_CHECK_=3 "$DUCKDB_BIN" -csv -noheader :memory: < "$WORK_DIR/fixed-array.sql")"
if [[ "$FIXED_ARRAY_RESULT" != $'1\ntrue\ntrue\ntrue' ]]; then
    echo "unexpected fixed-array result: $FIXED_ARRAY_RESULT" >&2
    exit 1
fi

cat > "$WORK_DIR/multidim-array.sql" <<SQL
LOAD parquet;
SELECT count(*)
FROM root_build_index(
    '$WORK_DIR/data/a.root',
    'Events',
    '/TestEvent/matrix',
    '$WORK_DIR/index_matrix',
    dictionary := '$WORK_DIR/build/libTestEvent.so',
    catalog_mode := 'sqlite',
    index_threads := 1,
    bloom_bytes := 64,
    allow_partial := false
)
WHERE status = 'OK';
CREATE TEMP MACRO current_root_snapshot() AS (
    SELECT arg_max(root_snapshot_id, committed_at_ns)
    FROM read_parquet('$WORK_DIR/index_matrix/warehouse/root_index/commits/data/*.parquet')
);
SELECT count(*) = 18
       AND count(DISTINCT event_fk) = 3
       AND min(matrix_dim0_idx) = 0 AND max(matrix_dim0_idx) = 1
       AND min(matrix_dim1_idx) = 0 AND max(matrix_dim1_idx) = 2
       AND sum(value) = 225
FROM read_root_dataset(
    '$WORK_DIR/index_matrix',
    '/TestEvent/matrix',
    dictionary := '$WORK_DIR/build/libTestEvent.so'
);
SELECT index_signature = 'matrix_dim0_idx,matrix_dim1_idx'
FROM read_parquet('$WORK_DIR/index_matrix/warehouse/root_index/schemas/data/*.parquet', filename = true)
WHERE logical_path = '/TestEvent/matrix'
  AND filename LIKE '%' || current_root_snapshot() || '-%';
SELECT count(*) = 1
       AND bool_and(is_fixed_array)
       AND min(array_rank) = 2
       AND min(array_length) = 6
       AND min(array_dimensions) = '2x3'
FROM read_parquet('$WORK_DIR/index_matrix/warehouse/root_index/access/data/*.parquet', filename = true)
WHERE field_name = 'matrix'
  AND filename LIKE '%' || current_root_snapshot() || '-%';
SQL

MULTIDIM_RESULT="$(MALLOC_CHECK_=3 "$DUCKDB_BIN" -csv -noheader :memory: < "$WORK_DIR/multidim-array.sql")"
if [[ "$MULTIDIM_RESULT" != $'1\ntrue\ntrue\ntrue' ]]; then
    echo "unexpected multidimensional-array result: $MULTIDIM_RESULT" >&2
    exit 1
fi

cat > "$WORK_DIR/inherited.sql" <<SQL
LOAD parquet;
SELECT count(*)
FROM root_build_index(
    '$WORK_DIR/data/a.root',
    'Events',
    '/TestEvent/inherited',
    '$WORK_DIR/index_inherited',
    dictionary := '$WORK_DIR/build/libTestEvent.so',
    index_threads := 1,
    allow_partial := false
)
WHERE status = 'OK';
SELECT count(*) = 3 AND min(value) = 9000 AND max(value) = 9002
FROM read_root_dataset(
    '$WORK_DIR/index_inherited',
    '/TestEvent/inherited',
    dictionary := '$WORK_DIR/build/libTestEvent.so'
);
SQL

INHERITED_RESULT="$(MALLOC_CHECK_=3 "$DUCKDB_BIN" -csv -noheader :memory: < "$WORK_DIR/inherited.sql")"
if [[ "$INHERITED_RESULT" != $'1\ntrue' ]]; then
    echo "unexpected inherited-field result: $INHERITED_RESULT" >&2
    exit 1
fi

cat > "$WORK_DIR/ancestor.sql" <<SQL
LOAD parquet;
CREATE TEMP TABLE ancestor_status AS
SELECT *
FROM root_build_index(
    '$WORK_DIR/data/ancestor.root',
    'Events',
    '/TestEvent/vecHit/u',
    '$WORK_DIR/index_ancestor',
    dictionary := '$WORK_DIR/build/libTestEvent.so',
    catalog_mode := 'sqlite',
    reader_mode := 'serialized',
    raw_validation_entries := 2,
    index_threads := 1000,
    bloom_bytes := 64,
    allow_partial := false
);
SELECT count(*) = 1
       AND min(effective_threads) = 1
       AND min(requested_threads) = 1000
       AND min(flattened_values) = 3
FROM ancestor_status
WHERE status = 'OK';
CREATE TEMP MACRO current_root_snapshot() AS (
    SELECT arg_max(root_snapshot_id, committed_at_ns)
    FROM read_parquet('$WORK_DIR/index_ancestor/warehouse/root_index/commits/data/*.parquet')
);
SELECT count(*) = 3 AND sum(value) = 6
FROM read_root_dataset(
    '$WORK_DIR/index_ancestor',
    '/TestEvent/vecHit/u',
    dictionary := '$WORK_DIR/build/libTestEvent.so',
    reader_mode := 'serialized',
    raw_validation_entries := 2
);
SELECT count(DISTINCT column_id) = 1
       AND bool_and(basket_branch_mode = 'ancestor')
       AND min(length(basket_branch_name)) > 0
FROM read_parquet('$WORK_DIR/index_ancestor/warehouse/root_index/baskets/data/*.parquet', filename = true)
WHERE filename LIKE '%' || current_root_snapshot() || '-%';

CREATE TEMP TABLE ancestor_nested_status AS
SELECT *
FROM root_build_index(
    '$WORK_DIR/data/ancestor.root',
    'Events',
    '/TestEvent/vecHit/refs/value',
    '$WORK_DIR/index_ancestor_nested',
    dictionary := '$WORK_DIR/build/libTestEvent.so',
    catalog_mode := 'sqlite',
    reader_mode := 'serialized',
    raw_validation_entries := 2,
    index_threads := 1,
    bloom_bytes := 64,
    allow_partial := false
);
SELECT count(*) = 1 AND min(flattened_values) = 5
FROM ancestor_nested_status
WHERE status = 'OK';
SELECT count(*) = 5
       AND sum(value) = 30
       AND min(vecHit_idx) = 0 AND max(vecHit_idx) = 1
       AND min(refs_idx) = 0 AND max(refs_idx) = 1
FROM read_root_dataset(
    '$WORK_DIR/index_ancestor_nested',
    '/TestEvent/vecHit/refs/value',
    dictionary := '$WORK_DIR/build/libTestEvent.so',
    reader_mode := 'serialized',
    raw_validation_entries := 2
);

-- Fixed array read from the full TestEvent object; ancestor branch is basket metadata only.
SELECT count(*)
FROM root_build_index(
    '$WORK_DIR/data/ancestor.root',
    'Events',
    '/TestEvent/vecHit/uv',
    '$WORK_DIR/index_ancestor_uv',
    dictionary := '$WORK_DIR/build/libTestEvent.so',
    index_threads := 1,
    bloom_bytes := 64,
    allow_partial := false
)
WHERE status = 'OK';
SELECT count(*) = 6
       AND min(uv_idx) = 0 AND max(uv_idx) = 1
       AND abs(sum(value) - 13.5) < 0.00001
FROM read_root_dataset(
    '$WORK_DIR/index_ancestor_uv',
    '/TestEvent/vecHit/uv',
    dictionary := '$WORK_DIR/build/libTestEvent.so'
);
SQL

ANCESTOR_RESULT="$(MALLOC_CHECK_=3 "$DUCKDB_BIN" -csv -noheader :memory: < "$WORK_DIR/ancestor.sql")"
if [[ "$ANCESTOR_RESULT" != $'true\ntrue\ntrue\ntrue\ntrue\n1\ntrue' ]]; then
    echo "unexpected ancestor-fallback result: $ANCESTOR_RESULT" >&2
    exit 1
fi

cat > "$WORK_DIR/mixed-layout.sql" <<SQL
LOAD parquet;
SELECT count(*)
FROM root_build_index(
    '$WORK_DIR/data/*.root',
    'Events',
    '/TestEvent/vecHit/u',
    '$WORK_DIR/index_mixed_layout',
    dictionary := '$WORK_DIR/build/libTestEvent.so',
    catalog_mode := 'sqlite',
    index_threads := 8,
    bloom_bytes := 64,
    allow_partial := false
)
WHERE status = 'OK';
CREATE TEMP MACRO current_root_snapshot() AS (
    SELECT arg_max(root_snapshot_id, committed_at_ns)
    FROM read_parquet('$WORK_DIR/index_mixed_layout/warehouse/root_index/commits/data/*.parquet')
);
SELECT count(*) = 17 AND count(DISTINCT event_fk) = 9
FROM read_root_dataset(
    '$WORK_DIR/index_mixed_layout',
    '/TestEvent/vecHit/u',
    dictionary := '$WORK_DIR/build/libTestEvent.so'
);
SELECT (SELECT count(DISTINCT schema_id)
        FROM read_parquet('$WORK_DIR/index_mixed_layout/warehouse/root_index/schemas/data/*.parquet', filename = true)
        WHERE filename LIKE '%' || current_root_snapshot() || '-%') = 1
       AND (SELECT count(DISTINCT basket_branch_mode)
        FROM read_parquet('$WORK_DIR/index_mixed_layout/warehouse/root_index/baskets/data/*.parquet', filename = true)
        WHERE filename LIKE '%' || current_root_snapshot() || '-%') >= 2;
SQL

MIXED_LAYOUT_RESULT="$(MALLOC_CHECK_=3 "$DUCKDB_BIN" -csv -noheader :memory: < "$WORK_DIR/mixed-layout.sql")"
if [[ "$MIXED_LAYOUT_RESULT" != $'5\ntrue\ntrue' ]]; then
    echo "unexpected mixed-layout result: $MIXED_LAYOUT_RESULT" >&2
    exit 1
fi

cat > "$WORK_DIR/lineage-stats.sql" <<SQL
LOAD parquet;
SELECT count(*) = 11
       AND count(source_id) = 11
       AND count(entry_id) = 11
       AND count(DISTINCT source_id) = 3
       AND min(entry_id) = 0
FROM read_root_dataset(
    '$WORK_DIR/index',
    '/TestEvent/vecHit/u',
    dictionary := '$WORK_DIR/build/libTestEvent.so'
);
SELECT row_count = 11
       AND non_null_count = 11
       AND null_count = 0
       AND basket_count > 0
       AND compressed_bytes > 0
FROM root_dataset_stats('$WORK_DIR/index', '/TestEvent/vecHit/u');
SQL
LINEAGE_STATS_RESULT="$(MALLOC_CHECK_=3 "$DUCKDB_BIN" -csv -noheader :memory: < "$WORK_DIR/lineage-stats.sql")"
if [[ "$LINEAGE_STATS_RESULT" != $'true\ntrue' ]]; then
    echo "unexpected lineage/stats result: $LINEAGE_STATS_RESULT" >&2
    exit 1
fi

SOURCE_ID_A="$(python3 - "$WORK_DIR/fixture-manifest.json" "$WORK_DIR/data/a.root" <<'PY2'
import json,sys
manifest=json.load(open(sys.argv[1]))
print(next(row["source_id"] for row in manifest["files"] if row["uri"] == sys.argv[2]))
PY2
)"
if [[ -z "$SOURCE_ID_A" ]]; then
    echo "cannot resolve fixture source_id for a.root" >&2
    exit 1
fi
cat > "$WORK_DIR/entry-selection.sql" <<SQL
LOAD parquet;
SELECT count(*) = 5
       AND count(DISTINCT source_id) = 1
       AND min(source_id) = '$SOURCE_ID_A'
       AND count(DISTINCT entry_id) = 2
       AND min(entry_id) = 0
       AND max(entry_id) = 2
       AND abs(sum(value) - 53.0) < 0.00001
FROM read_root_dataset(
    '$WORK_DIR/index',
    '/TestEvent/vecHit/u',
    dictionary := '$WORK_DIR/build/libTestEvent.so',
    entry_selection := '{"$SOURCE_ID_A":{"ranges":[[0,1]],"entries":[2]}}'
);
EXPLAIN ANALYZE
SELECT *
FROM read_root_dataset(
    '$WORK_DIR/index',
    '/TestEvent/vecHit/u',
    dictionary := '$WORK_DIR/build/libTestEvent.so',
    entry_selection := '{"$SOURCE_ID_A":{"ranges":[[0,1]],"entries":[2]}}'
);
SQL
ENTRY_SELECTION_RESULT="$(MALLOC_CHECK_=3 "$DUCKDB_BIN" -csv -noheader :memory: < "$WORK_DIR/entry-selection.sql")"
ENTRY_SELECTION_METRIC="$(profile_metric 'Explicit Entry Selections' <<<"$ENTRY_SELECTION_RESULT" || true)"
if [[ "$(printf '%s\n' "$ENTRY_SELECTION_RESULT" | head -n 1)" != "true" ]] || \
   [[ "$ENTRY_SELECTION_METRIC" != "1" ]]; then
    echo "unexpected exact entry-selection result: $ENTRY_SELECTION_RESULT" >&2
    exit 1
fi

cat > "$WORK_DIR/entry-selection-delta.json" <<JSON
{"$SOURCE_ID_A":{"ranges":[[0,1]],"entries_delta":{"base":2,"deltas":[]}}}
JSON
cat > "$WORK_DIR/limits-and-aggregates.sql" <<SQL
LOAD parquet;
.read $PROJECT_DIR/sql/root_dataset_aggregates.sql
SELECT count(*) = 3
       AND max(entry_id) <= 2
FROM read_root_dataset(
    '$WORK_DIR/index',
    '/TestEvent/vecHit/u',
    dictionary := '$WORK_DIR/build/libTestEvent.so',
    entry_selection_file := '$WORK_DIR/entry-selection-delta.json',
    row_limit := 3
);
SELECT row_count = 11 AND non_null_count = 11 AND min_value IS NOT NULL AND max_value IS NOT NULL
FROM root_dataset_stats('$WORK_DIR/index', '/TestEvent/vecHit/u');
SELECT row_count = 11 FROM root_dataset_count('$WORK_DIR/index', '/TestEvent/vecHit/u');
SELECT non_null_count = 11 FROM root_dataset_non_null_count('$WORK_DIR/index', '/TestEvent/vecHit/u');
SQL
LIMITS_RESULT="$(MALLOC_CHECK_=3 "$DUCKDB_BIN" -csv -noheader :memory: < "$WORK_DIR/limits-and-aggregates.sql")"
if [[ "$LIMITS_RESULT" != $'true\ntrue\ntrue\ntrue' ]]; then
    echo "unexpected row-limit/aggregate result: $LIMITS_RESULT" >&2
    exit 1
fi

cat > "$WORK_DIR/event-pruning.sql" <<SQL
LOAD parquet;
EXPLAIN ANALYZE
SELECT *
FROM read_root_dataset(
    '$WORK_DIR/index',
    '/TestEvent/vecHit/u',
    dictionary := '$WORK_DIR/build/libTestEvent.so'
)
WHERE event_fk = 2;
SQL

EVENT_PRUNING_RESULT="$(MALLOC_CHECK_=3 "$DUCKDB_BIN" -csv -noheader :memory: < "$WORK_DIR/event-pruning.sql")"
if ! grep -q 'Skipped ROOT Entries' <<<"$EVENT_PRUNING_RESULT"; then
    echo "event-entry pruning metric is absent" >&2
    exit 1
fi

cat > "$WORK_DIR/null-pruning.sql" <<SQL
LOAD parquet;
SELECT count(*) = 0
FROM read_root_dataset(
    '$WORK_DIR/index',
    '/TestEvent/vecHit/u',
    dictionary := '$WORK_DIR/build/libTestEvent.so'
)
WHERE value IS NULL;
EXPLAIN ANALYZE
SELECT count(*)
FROM read_root_dataset(
    '$WORK_DIR/index',
    '/TestEvent/vecHit/u',
    dictionary := '$WORK_DIR/build/libTestEvent.so'
)
WHERE value IS NULL;
SQL
NULL_PRUNING_RESULT="$(MALLOC_CHECK_=3 "$DUCKDB_BIN" -csv -noheader :memory: < "$WORK_DIR/null-pruning.sql")"
if [[ "$(printf '%s\n' "$NULL_PRUNING_RESULT" | head -n 1)" != "true" ]]; then
    echo "unexpected IS NULL result: $NULL_PRUNING_RESULT" >&2
    exit 1
fi

cat > "$WORK_DIR/in-bloom.sql" <<SQL
LOAD parquet;
EXPLAIN ANALYZE
SELECT count(*)
FROM read_root_dataset(
    '$WORK_DIR/index',
    '/TestEvent/vecHit/u',
    dictionary := '$WORK_DIR/build/libTestEvent.so'
)
WHERE value IN (12345.0, 23456.0);
SQL
IN_BLOOM_RESULT="$(MALLOC_CHECK_=3 "$DUCKDB_BIN" -csv -noheader :memory: < "$WORK_DIR/in-bloom.sql")"
IN_SELECTED_FILES="$(profile_metric 'Selected ROOT Files' <<<"$IN_BLOOM_RESULT" || true)"
IN_DECODED_VALUES="$(profile_metric 'Decoded Values' <<<"$IN_BLOOM_RESULT" || true)"
if [[ "$IN_SELECTED_FILES" != "0" || "$IN_DECODED_VALUES" != "0" ]]; then
    echo "IN equality set should be rejected by basket Bloom filters" >&2
    exit 1
fi

cat > "$WORK_DIR/metadata-count.sql" <<SQL
LOAD parquet;
EXPLAIN ANALYZE
SELECT count(*)
FROM read_root_dataset(
    '$WORK_DIR/index',
    '/TestEvent/vecHit/u',
    dictionary := '$WORK_DIR/build/libTestEvent.so'
);
SQL
METADATA_COUNT_RESULT="$(MALLOC_CHECK_=3 "$DUCKDB_BIN" -csv -noheader :memory: < "$WORK_DIR/metadata-count.sql")"
METADATA_ONLY_ROWS="$(profile_metric 'Metadata-only Rows' <<<"$METADATA_COUNT_RESULT" || true)"
METADATA_DECODED_VALUES="$(profile_metric 'Decoded Values' <<<"$METADATA_COUNT_RESULT" || true)"
METADATA_TASKS="$(profile_metric 'Coalesced Read Tasks' <<<"$METADATA_COUNT_RESULT" || true)"
METADATA_OPENED="$(profile_metric 'Opened ROOT Files' <<<"$METADATA_COUNT_RESULT" || true)"
if [[ "$METADATA_ONLY_ROWS" != "11" || "$METADATA_DECODED_VALUES" != "0" ||
      "$METADATA_TASKS" != "0" || "$METADATA_OPENED" != "0" ]]; then
    echo "COUNT(*) should use file cardinalities without task planning or ROOT decoding" >&2
    exit 1
fi

cat > "$WORK_DIR/catalog-publication.sql" <<SQL
LOAD parquet;
CREATE TEMP TABLE publication_status AS
SELECT *
FROM root_build_index(
    '$WORK_DIR/data/a.root',
    'Events',
    '["/TestEvent/run","/TestEvent/vecHit/u","/TestEvent/mapScore/key"]',
    '',
    dictionary := '$WORK_DIR/build/libTestEvent.so',
    index_threads := 1,
    bloom_bytes := 64,
    catalog_prefix := 'main.published_root',
    publish_mode := 'replace',
    allow_partial := false
);
SELECT count(*) = 1 AND bool_and(published) AND min(publish_mode) = 'replace'
       AND contains(min(message), '3 logical paths')
FROM publication_status
WHERE status = 'OK';
SELECT (SELECT count(*) FROM main.published_root_files) = 3
       AND (SELECT count(*) FROM main.published_root_schemas) = 3
       AND (SELECT count(*) FROM main.published_root_access) >= 5
       AND (SELECT count(*) FROM main.published_root_baskets) >= 3
       AND (SELECT count(*) FROM main.published_root_snapshots WHERE state='COMMITTED') = 1;
SELECT count(*) = 5 AND abs(sum(value) - 53.0) < 0.00001
FROM read_root_dataset(
    'main.published_root',
    '/TestEvent/vecHit/u',
    dictionary := '$WORK_DIR/build/libTestEvent.so'
);
SELECT count(*) = 6 AND abs(sum(value) - 9.0) < 0.00001
FROM read_root_dataset(
    'main.published_root',
    '/TestEvent/mapScore/key',
    dictionary := '$WORK_DIR/build/libTestEvent.so'
);
SELECT count(*) = 1 AND min(event_fk) = 2 AND max(event_fk) = 2 AND sum(value) = 11
FROM read_root_dataset(
    'main.published_root',
    '/TestEvent/vecHit/u',
    dictionary := '$WORK_DIR/build/libTestEvent.so',
    path_predicates := '[{"path":"/TestEvent/run","op":"=","value":102},{"path":"/TestEvent/vecHit/u","op":">","value":10,"quantifier":"any"}]'
);
EXPLAIN ANALYZE
SELECT *
FROM read_root_dataset(
    'main.published_root',
    '/TestEvent/vecHit/u',
    dictionary := '$WORK_DIR/build/libTestEvent.so',
    path_predicates := '[{"path":"/TestEvent/run","op":">=","value":100},{"path":"/TestEvent/vecHit/u","op":">","value":10}]'
);
SQL

PUBLICATION_RESULT="$(MALLOC_CHECK_=3 "$DUCKDB_BIN" -csv -noheader :memory: < "$WORK_DIR/catalog-publication.sql")"
PUBLICATION_HEAD="$(printf '%s\n' "$PUBLICATION_RESULT" | head -n 5)"
if [[ "$PUBLICATION_HEAD" != $'true\ntrue\ntrue\ntrue\ntrue' ]]; then
    echo "unexpected catalog-publication result: $PUBLICATION_RESULT" >&2
    exit 1
fi
PATH_PREDICATE_INDEXES="$(profile_metric 'Path Predicate Indexes' <<<"$PUBLICATION_RESULT" || true)"
PREDICATE_INTERSECTIONS="$(profile_metric 'Predicate Intersections' <<<"$PUBLICATION_RESULT" || true)"
if [[ "$PATH_PREDICATE_INDEXES" != "2" || "$PREDICATE_INTERSECTIONS" != "1" ]]; then
    echo "multi-path predicate intersection metrics are absent: $PUBLICATION_RESULT" >&2
    exit 1
fi

echo "ROOT4DuckDB integration test passed: direct=$DIRECT_RESULT scalar=$SCALAR_RESULT lakehouse=$QUERY_RESULT fixed=$FIXED_ARRAY_RESULT multidim=$MULTIDIM_RESULT inherited=$INHERITED_RESULT ancestor=$ANCESTOR_RESULT mixed=$MIXED_LAYOUT_RESULT publication=$PUBLICATION_RESULT"
