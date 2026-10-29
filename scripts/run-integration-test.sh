#!/usr/bin/env bash
set -euo pipefail

PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
FIXTURE_DIR="$PROJECT_DIR/test/fixtures"
WORK_DIR="${ROOT4DUCKDB_TEST_DIR:-$(mktemp -d /tmp/root4duckdb-it.XXXXXX)}"
# Use the environment-aware launcher so Iceberg and compiler runtimes
# match the successful build. Explicit DUCKDB_BIN overrides remain supported.
DUCKDB_BIN="${DUCKDB_BIN:-$PROJECT_DIR/run-duckdb.sh}"
CMAKE_CACHE="$PROJECT_DIR/build/release/CMakeCache.txt"
CACHED_ROOT_CONFIG=""
if [[ -r "$CMAKE_CACHE" ]]; then
    CACHED_ROOT_CONFIG="$(sed -n 's/^ROOT4DUCKDB_ROOT_CONFIG_EXECUTABLE:[^=]*=//p' "$CMAKE_CACHE" | head -n 1)"
fi
ROOT_CONFIG_BIN="${ROOT4DUCKDB_ROOT_CONFIG:-${CACHED_ROOT_CONFIG:-}}"
if [[ -z "$ROOT_CONFIG_BIN" ]]; then
    ROOT_CONFIG_BIN="$(command -v root-config || true)"
fi
if [[ -n "$CACHED_ROOT_CONFIG" && -n "${ROOT4DUCKDB_ROOT_CONFIG:-}" ]]; then
    compiled_root="$("$CACHED_ROOT_CONFIG" --libdir)"
    requested_root="$("$ROOT4DUCKDB_ROOT_CONFIG" --libdir)"
    if [[ "$(readlink -f "$compiled_root/libCore.so")" != "$(readlink -f "$requested_root/libCore.so")" ]]; then
        echo "integration test ROOT differs from the ROOT used by CMake" >&2
        echo "CMake ROOT: $CACHED_ROOT_CONFIG" >&2
        echo "Environment ROOT: $ROOT4DUCKDB_ROOT_CONFIG" >&2
        exit 2
    fi
fi
if [[ -z "$ROOT_CONFIG_BIN" || ! -x "$ROOT_CONFIG_BIN" ]]; then
    echo "missing required ROOT configuration executable: $ROOT_CONFIG_BIN" >&2
    exit 2
fi
ROOT_CONFIG_BIN="$(readlink -f "$ROOT_CONFIG_BIN")"
ROOT_LIBDIR="$(readlink -f "$("$ROOT_CONFIG_BIN" --libdir)")"
ROOT_BINDIR="$("$ROOT_CONFIG_BIN" --bindir)"
ROOT_INCDIR="$("$ROOT_CONFIG_BIN" --incdir)"
if [[ ! -x "$ROOT_BINDIR/rootcling" || ! -f "$ROOT_LIBDIR/libCore.so" ]]; then
    echo "ROOT tools or libraries are missing from $ROOT_BINDIR / $ROOT_LIBDIR" >&2
    exit 2
fi

root_library_directory_compatible() {
    local candidate="$1" library
    for library in libCore.so libRIO.so libTree.so libHist.so libCling.so libMathCore.so; do
        if [[ -e "$candidate/$library" &&
              "$(readlink -f "$candidate/$library")" != "$(readlink -f "$ROOT_LIBDIR/$library")" ]]; then
            return 1
        fi
    done
    return 0
}

ROOT_RUNTIME_PATH="$ROOT_LIBDIR"
IFS=: read -r -a existing_runtime_paths <<< "${LD_LIBRARY_PATH:-}"
for runtime_path in "${existing_runtime_paths[@]}"; do
    [[ -n "$runtime_path" && "$runtime_path" != "$ROOT_LIBDIR" ]] || continue
    if root_library_directory_compatible "$runtime_path"; then
        ROOT_RUNTIME_PATH+=":$runtime_path"
    else
        echo "[ROOT] ignoring conflicting library directory: $runtime_path" >&2
    fi
done
export ROOTSYS="$("$ROOT_CONFIG_BIN" --prefix)"
export ROOT4DUCKDB_ROOT_CONFIG="$ROOT_CONFIG_BIN"
export PATH="$ROOT_BINDIR:$PATH"
export LD_LIBRARY_PATH="$ROOT_RUNTIME_PATH"
export ROOT_LIBRARY_PATH="$WORK_DIR/build:$ROOT_LIBDIR"
export ROOT_INCLUDE_PATH="$ROOT_INCDIR:$FIXTURE_DIR"
ROOT_DEPENDENCY_STATUS=0
ROOT_DEPENDENCY_REPORT="$(ldd -v "$ROOT_LIBDIR/libCore.so" 2>&1)" || ROOT_DEPENDENCY_STATUS=$?
if ((ROOT_DEPENDENCY_STATUS != 0)) || grep -Eq 'not found|no version information available' <<< "$ROOT_DEPENDENCY_REPORT"; then
    echo "[ROOT] incompatible shared-library dependencies for $ROOT_LIBDIR/libCore.so" >&2
    grep -E 'not found|no version information available' <<< "$ROOT_DEPENDENCY_REPORT" >&2 || true
    echo "[ROOT] use a matching LCG view (LCG_110a for ROOT 6.40.04), not LCG_110" >&2
    exit 2
fi
echo "[ROOT] version=$("$ROOT_CONFIG_BIN" --version) config=$ROOT_CONFIG_BIN" >&2
echo "[ROOT] bin=$ROOT_BINDIR lib=$ROOT_LIBDIR" >&2

validate_fixture_runtime() {
    local executable="$1" library actual expected
    for library in libCore.so libRIO.so libTree.so; do
        actual="$(ldd "$executable" | awk -v library="$library" '$1 == library {print $3; exit}')"
        expected="$(readlink -f "$ROOT_LIBDIR/$library")"
        if [[ -z "$actual" || ! -e "$actual" || "$(readlink -f "$actual")" != "$expected" ]]; then
            echo "ROOT runtime mismatch for $executable: $library" >&2
            echo "Expected: $expected" >&2
            echo "Resolved: ${actual:-missing}" >&2
            return 2
        fi
    done
}
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

if [[ -e "$WORK_DIR" && ! -d "$WORK_DIR" ]] ||
   [[ -d "$WORK_DIR" && -n "$(find "$WORK_DIR" -mindepth 1 -maxdepth 1 -print -quit)" ]]; then
    echo "integration test directory must be empty: $WORK_DIR" >&2
    exit 2
fi
mkdir -p "$WORK_DIR/data" "$WORK_DIR/build" "$WORK_DIR/legacy"

ROOT_REGISTERED_STATUS=0
ROOT_REGISTERED="$("$DUCKDB_BIN" -csv -noheader :memory: \
    -c "SELECT count(*) > 0 FROM duckdb_functions() WHERE function_name = 'read_root';" 2>&1)" ||
    ROOT_REGISTERED_STATUS=$?
if ((ROOT_REGISTERED_STATUS != 0)); then
    echo "[ERROR] DuckDB CLI registration probe failed (exit=$ROOT_REGISTERED_STATUS): $DUCKDB_BIN" >&2
    echo "$ROOT_REGISTERED" >&2
    exit "$ROOT_REGISTERED_STATUS"
fi
DUCKDB_ARGS=(-bail)
if [[ "$ROOT_REGISTERED" == "false" ]]; then
    ROOT_EXTENSION="$PROJECT_DIR/build/release/extension/root/root.duckdb_extension"
    if [[ ! -f "$ROOT_EXTENSION" ]]; then
        echo "ROOT extension is not built: $ROOT_EXTENSION" >&2
        exit 2
    fi
    DUCKDB_ARGS=(-unsigned -bail -cmd "LOAD '$ROOT_EXTENSION';")
elif [[ "$ROOT_REGISTERED" != "true" ]]; then
    echo "unable to determine ROOT registration in $DUCKDB_BIN: $ROOT_REGISTERED" >&2
    exit 2
fi
LOADED_REGISTERED_STATUS=0
LOADED_REGISTERED="$("$DUCKDB_BIN" "${DUCKDB_ARGS[@]}" -csv -noheader :memory: \
    -c "SELECT count(*) > 0 FROM duckdb_functions() WHERE function_name = 'read_root';" 2>&1)" ||
    LOADED_REGISTERED_STATUS=$?
if ((LOADED_REGISTERED_STATUS != 0)); then
    echo "[ERROR] DuckDB extension LOAD/registration probe failed (exit=$LOADED_REGISTERED_STATUS)" >&2
    echo "$LOADED_REGISTERED" >&2
    exit "$LOADED_REGISTERED_STATUS"
fi
if [[ "$LOADED_REGISTERED" != "true" ]]; then
    echo "read_root was not registered in DuckDB CLI: $LOADED_REGISTERED" >&2
    exit 2
fi

# shellcheck source=lib/fixtures.sh
source "$PROJECT_DIR/scripts/lib/fixtures.sh"
CXX_BIN="${CXX:-c++}"
build_root_fixture "$FIXTURE_DIR" "$WORK_DIR/build" TestEvent make_fixture "$CXX_BIN" "$ROOT_CONFIG_BIN"
validate_fixture_runtime "$WORK_DIR/build/make_fixture"
echo "[ROOT] fixture dictionary=$WORK_DIR/build/libTestEvent_rdict.pcm" >&2
"$WORK_DIR/build/make_fixture" "$WORK_DIR/data"

# Stress fixtures must not contaminate the original '*.root' glob used
# by the mixed-layout regression (a, b, c, packed, ancestor).
mv "$WORK_DIR/data/cursor.root" "$WORK_DIR/cursor.root"
mv "$WORK_DIR/data/serialized_page.root" "$WORK_DIR/serialized_page.root"
if [[ "$(find "$WORK_DIR/data" -maxdepth 1 -type f -name '*.root' | wc -l)" -ne 5 ]]; then
    echo "unexpected general ROOT fixture count after cursor isolation" >&2
    find "$WORK_DIR/data" -maxdepth 1 -type f -name '*.root' -print >&2
    exit 1
fi

# End-to-end regression: 99, 100, 101 and 4099 values per ROOT entry.
# A 100-value serialized limit must never yield successful truncated scans.
cat > "$WORK_DIR/cursor-regression.sql" <<SQL
SELECT count(*) = 4399 AND sum(value) = 8413702 AND sum(header_idx) = 8413702
FROM read_root(
    '$WORK_DIR/cursor.root',
    dictionary := '$WORK_DIR/build/libTestEvent.so',
    path_prefix := '/TestEvent/header',
    reader_mode := 'object'
);
SELECT count(*) = 4099 AND min(header_idx) = 0 AND max(header_idx) = 4098
       AND count(DISTINCT header_idx) = 4099
FROM read_root(
    '$WORK_DIR/cursor.root',
    dictionary := '$WORK_DIR/build/libTestEvent.so',
    path_prefix := '/TestEvent/header',
    reader_mode := 'object'
) WHERE entry_id = 3;
SELECT count(*) = 4399 AND sum(value) = 8413702
       AND min(vecHit_idx) = 0 AND max(vecHit_idx) = 0
       AND max(weights_idx) = 4098
FROM read_root(
    '$WORK_DIR/cursor.root',
    dictionary := '$WORK_DIR/build/libTestEvent.so',
    path_prefix := '/TestEvent/vecHit/weights',
    reader_mode := 'object'
);
SELECT count(*) = 4399 AND sum(value) = 8413702
FROM read_root(
    '$WORK_DIR/cursor.root',
    dictionary := '$WORK_DIR/build/libTestEvent.so',
    path_prefix := '/TestEvent/vecHit/weights',
    reader_mode := 'auto',
    raw_max_values_per_entry := 100
);
SELECT count(*) = 100 AND min(header_idx) = 0 AND max(header_idx) = 99
FROM read_root(
    '$WORK_DIR/cursor.root',
    dictionary := '$WORK_DIR/build/libTestEvent.so',
    path_prefix := '/TestEvent/header',
    reader_mode := 'serialized',
    raw_max_values_per_entry := 100
) WHERE entry_id = 1;
LOAD parquet;
SELECT count(*) = 1 AND min(flattened_values) = 4399
FROM root_build_index(
    '$WORK_DIR/cursor.root',
    'Events',
    '/TestEvent/vecHit/weights/value',
    '$WORK_DIR/index_cursor',
    dictionary := '$WORK_DIR/build/libTestEvent.so',
    reader_mode := 'object',
    catalog_mode := 'sqlite',
    index_threads := 1,
    bloom_bytes := 64,
    allow_partial := false
) WHERE status = 'OK';
SELECT count(*) = 4399 AND sum(value) = 8413702 AND max(weights_idx) = 4098
FROM read_root_dataset(
    '$WORK_DIR/index_cursor',
    '/TestEvent/vecHit/weights/value',
    dictionary := '$WORK_DIR/build/libTestEvent.so',
    reader_mode := 'object'
);
SELECT count(*) = 8 AND sum(value) = 32756
FROM read_root_dataset(
    '$WORK_DIR/index_cursor',
    '/TestEvent/vecHit/weights/value',
    dictionary := '$WORK_DIR/build/libTestEvent.so',
    reader_mode := 'object'
) WHERE value > 4090;
SQL
CURSOR_RESULT="$(MALLOC_CHECK_=3 "$DUCKDB_BIN" "${DUCKDB_ARGS[@]}" -csv -noheader :memory: \
    < "$WORK_DIR/cursor-regression.sql")"
if [[ "$(printf '%s\n' "$CURSOR_RESULT" | grep -xc true)" -ne 8 ]] ||
    printf '%s\n' "$CURSOR_RESULT" | grep -qv '^true$'; then
    echo "ROOT cursor regression failed: $CURSOR_RESULT" >&2
    exit 1
fi
echo "[OK] ROOT cursor regression: 99/100/101, 4099-row resume, dataset and index" >&2

# In-process concurrency regression: independent DuckDB databases load ROOT
# and scan real dictionary-backed ROOT events from eight native threads.
"$CXX_BIN" -std=c++20 -O2 -pthread \
    -I"$PROJECT_DIR/duckdb/src/include" \
    "$FIXTURE_DIR/concurrent_root_load.cpp" \
    -L"$PROJECT_DIR/build/release/src" \
    -Wl,-rpath,"$PROJECT_DIR/build/release/src" \
    -lduckdb -ldl -o "$WORK_DIR/build/concurrent_root_load"
ROOT4DUCKDB_TEST_EXTENSION_PATH="${ROOT_EXTENSION:-}" \
    timeout -k 5s 180s "$WORK_DIR/build/concurrent_root_load" \
    "$WORK_DIR/data/a.root" "$WORK_DIR/build/libTestEvent.so" \
    "$WORK_DIR/build/concurrent-index"

build_root_fixture "$FIXTURE_DIR" "$WORK_DIR/build" LegacyTestEvent make_legacy_fixture "$CXX_BIN" "$ROOT_CONFIG_BIN"
validate_fixture_runtime "$WORK_DIR/build/make_legacy_fixture"

"$WORK_DIR/build/make_legacy_fixture" "$WORK_DIR/legacy/legacy.root"
printf '%s\n' "$WORK_DIR/data/a.root" "$WORK_DIR/data/b.root" > "$WORK_DIR/data/two-files.list"

# Bind-only regression: EXPLAIN invokes table-function bind but must not
# materialize a ROOT entry or recursively discover the complete schema.
cat > "$WORK_DIR/direct-bind.sql" <<SQL
EXPLAIN
SELECT *
FROM read_root(
    '$WORK_DIR/data/a.root',
    dictionary := '$WORK_DIR/build/libTestEvent.so',
    path_prefix := '/TestEvent'
);
EXPLAIN
SELECT *
FROM read_root(
    '$WORK_DIR/data/a.root',
    dictionary := '$WORK_DIR/build/libTestEvent.so',
    path_prefix := '/TestEvent/vecHit/uv'
);
SQL
MALLOC_CHECK_=3 "$DUCKDB_BIN" "${DUCKDB_ARGS[@]}" -csv -noheader :memory: < "$WORK_DIR/direct-bind.sql" >/dev/null

# TestBase inherits TObject: its bookkeeping fields must not leak into
# physics-facing semantic discovery or into the automatic SELECT * schema.
cat > "$WORK_DIR/tobject-discovery.sql" <<SQL
LOAD parquet;
SELECT count(*) = 0
FROM root_describe(
    '$WORK_DIR/data/a.root',
    '/TestEvent',
    dictionary := '$WORK_DIR/build/libTestEvent.so'
)
WHERE path IN ('/TestEvent/fUniqueID', '/TestEvent/fBits');
DESCRIBE SELECT *
FROM read_root(
    '$WORK_DIR/data/a.root',
    dictionary := '$WORK_DIR/build/libTestEvent.so',
    path_prefix := '/TestEvent'
);
SQL
if ! MALLOC_CHECK_=3 "$DUCKDB_BIN" "${DUCKDB_ARGS[@]}" -csv -noheader :memory: \
    < "$WORK_DIR/tobject-discovery.sql" > "$WORK_DIR/tobject-discovery.log" 2>&1; then
    echo "TObject semantic discovery failed" >&2
    cat "$WORK_DIR/tobject-discovery.log" >&2
    exit 1
fi
if [[ "$(head -n 1 "$WORK_DIR/tobject-discovery.log")" != "true" ]] ||
   grep -Eq '(^|,)(fUniqueID|fBits)(,|$)' "$WORK_DIR/tobject-discovery.log" ||
   ! grep -q '^run,' "$WORK_DIR/tobject-discovery.log"; then
    echo "TObject semantic discovery schema is incorrect" >&2
    cat "$WORK_DIR/tobject-discovery.log" >&2
    exit 1
fi

# A scalar relation projected only as entry_id still contains every event.
# An empty physical projection is not the end of the ROOT tree.
if ! ID_ONLY_RESULT="$("$DUCKDB_BIN" "${DUCKDB_ARGS[@]}" -csv -noheader :memory: 2>"$WORK_DIR/entry-id-only.log" <<SQL
SELECT count(*) = 3 AND sum(entry_id) = 3
FROM read_root(
    '$WORK_DIR/data/a.root',
    dictionary := '$WORK_DIR/build/libTestEvent.so',
    path_prefix := '/TestEvent'
);
SELECT count(*) = 5 AND sum(entry_id) = 4
FROM read_root(
    '$WORK_DIR/data/[ab].root',
    dictionary := '$WORK_DIR/build/libTestEvent.so',
    path_prefix := '/TestEvent'
);
SQL
)"; then
    echo "entry_id-only ROOT scan failed" >&2
    cat "$WORK_DIR/entry-id-only.log" >&2
    exit 1
fi
if [[ "$(printf '%s\n' "$ID_ONLY_RESULT" | grep -xc true)" != 2 ]]; then
    echo "scalar entry_id-only projection lost ROOT events: $ID_ONLY_RESULT" >&2
    exit 1
fi

# Exercise repeated object reads using automatic caller-owned cleanup
# for objects created through a loaded ROOT dictionary.
cat > "$WORK_DIR/object-mode-repeat.sql" <<SQL
SELECT count(*) = 3 AND sum(run) = 303
FROM read_root(
    '$WORK_DIR/data/a.root',
    dictionary := '$WORK_DIR/build/libTestEvent.so',
    reader_mode := 'object',
    path_prefix := '/TestEvent'
);
SQL
for iteration in 1 2 3 4 5 6 7 8 9 10; do
    if ! "$DUCKDB_BIN" "${DUCKDB_ARGS[@]}" -csv -noheader :memory: < "$WORK_DIR/object-mode-repeat.sql" \
        > "$WORK_DIR/object-mode-repeat.log" 2>&1; then
        echo "Repeated ROOT object-mode scan failed on iteration $iteration" >&2
        cat "$WORK_DIR/object-mode-repeat.log" >&2
        exit 1
    fi
    if ! grep -qx true "$WORK_DIR/object-mode-repeat.log"; then
        echo "Repeated ROOT object-mode scan returned unexpected data on iteration $iteration" >&2
        cat "$WORK_DIR/object-mode-repeat.log" >&2
        exit 1
    fi
done

cat > "$WORK_DIR/direct-read.sql" <<SQL
LOAD parquet;
SELECT count(*) >= 10
       AND sum(CASE WHEN path = '/TestEvent/run' AND kind = 'PRIMITIVE' THEN 1 ELSE 0 END) = 1
       AND sum(CASE WHEN path = '/TestEvent/vecHit' AND kind = 'CONTAINER' THEN 1 ELSE 0 END) = 1
       AND sum(CASE WHEN path = '/TestEvent/point' AND kind = 'OBJECT' THEN 1 ELSE 0 END) = 1
       AS root_describe_next_level_ok
FROM root_describe(
    '$WORK_DIR/data/a.root',
    '/TestEvent',
    dictionary := '$WORK_DIR/build/libTestEvent.so'
);
SELECT
    count(*) = 5
    AND abs(sum(u) - 53.0) < 0.00001
    AND sum(entry_id) = 2
    AND sum(vecHit_idx) = 6 AS direct_nested_read_ok
FROM read_root(
    '$WORK_DIR/data/a.root',
    dictionary := '$WORK_DIR/build/libTestEvent.so',
    path_prefix := '/TestEvent/vecHit'
);
SELECT count(*) = 5
       AND sum(run) = 704
       AND count(DISTINCT source_id) = 2
       AND count(DISTINCT source_path) = 2
       AND min(entry_id) = 0 AND max(entry_id) = 2 AS direct_glob_scalar_ok
FROM read_root(
    '$WORK_DIR/data/[ab].root',
    dictionary := '$WORK_DIR/build/libTestEvent.so',
    path_prefix := '/TestEvent'
);
SELECT count(*) = 9
       AND abs(sum(u) - 102.5) < 0.00001
       AND sum(entry_id) = 4
       AND sum(source_id) = 4 AS direct_glob_nested_ok
FROM read_root(
    '$WORK_DIR/data/[ab].root',
    dictionary := '$WORK_DIR/build/libTestEvent.so',
    path_prefix := '/TestEvent/vecHit'
);
SELECT count(*) = 5
       AND sum(run) = 704
       AND count(DISTINCT source_id) = 2 AS direct_uri_list_ok
FROM read_root(
    '@$WORK_DIR/data/two-files.list',
    dictionary := '$WORK_DIR/build/libTestEvent.so',
    path_prefix := '/TestEvent'
);
SELECT count(*) = 2
       AND sum(run) = 401
       AND min(source_id) = 1 AND max(source_id) = 1 AS direct_source_pruning_ok
FROM read_root(
    '$WORK_DIR/data/[ab].root',
    dictionary := '$WORK_DIR/build/libTestEvent.so',
    path_prefix := '/TestEvent'
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
    path_prefix := '/TestEvent'
);
SELECT count(*) = 3 AND count(flags) = 3 AND min(flags) = 0 AND max(flags) = 2 AND sum(flags) = 3 AS direct_uchar_read_ok
FROM read_root(
    '$WORK_DIR/data/a.root',
    dictionary := '$WORK_DIR/build/libTestEvent.so',
    path_prefix := '/TestEvent'
);
SELECT count(*) = 3 AND count(signed_code) = 3 AND min(signed_code) = -1 AND max(signed_code) = 1 AND sum(signed_code) = 0 AS direct_char_read_ok
FROM read_root(
    '$WORK_DIR/data/a.root',
    dictionary := '$WORK_DIR/build/libTestEvent.so',
    path_prefix := '/TestEvent'
);
SELECT count(*) = 4 AND min(entry_id) = 0 AND max(entry_id) = 0 AS direct_filter_semantics_ok
FROM read_root(
    '$WORK_DIR/data/a.root',
    dictionary := '$WORK_DIR/build/libTestEvent.so',
    path_prefix := '/TestEvent/vecHit'
)
WHERE entry_id < 1;
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
    path_prefix := '/TestEvent'
);
SELECT count(*) = 6 AND abs(sum(key) - 9.0) < 0.00001 AS direct_map_key_ok
FROM read_root(
    '$WORK_DIR/data/a.root',
    dictionary := '$WORK_DIR/build/libTestEvent.so',
    path_prefix := '/TestEvent/mapScore'
);
SELECT count(*) = 6 AND abs(sum(value) - 607.5) < 0.00001 AS direct_map_value_ok
FROM read_root(
    '$WORK_DIR/data/a.root',
    dictionary := '$WORK_DIR/build/libTestEvent.so',
    path_prefix := '/TestEvent/mapScore'
);
SELECT count(*) = 6 AND sum(value) = 36 AS direct_set_ok
FROM read_root(
    '$WORK_DIR/data/a.root',
    dictionary := '$WORK_DIR/build/libTestEvent.so',
    path_prefix := '/TestEvent/setCode'
);
SELECT count(*) = 9 AND abs(sum(first) - 31.5) < 0.00001 AS direct_nested_pair_first_ok
FROM read_root(
    '$WORK_DIR/data/a.root',
    dictionary := '$WORK_DIR/build/libTestEvent.so',
    path_prefix := '/TestEvent/nestedPairs/value/value'
);
SELECT count(*) = 9 AND sum(second) = 1809 AS direct_nested_pair_second_ok
FROM read_root(
    '$WORK_DIR/data/a.root',
    dictionary := '$WORK_DIR/build/libTestEvent.so',
    path_prefix := '/TestEvent/nestedPairs/value/value'
);
-- Filter columns are evaluated inside read_root even when COUNT(*) projects
-- none of them. This also validates exact event-range clipping.
SELECT count(*) = 1 AS direct_filter_pushdown_ok
FROM read_root(
    '$WORK_DIR/data/a.root',
    dictionary := '$WORK_DIR/build/libTestEvent.so',
    path_prefix := '/TestEvent/vecHit'
)
WHERE entry_id = 2 AND u = 11;
SELECT count(*) = 3 AND abs(sum(u) - 6.0) < 0.00001 AS serialized_direct_reader_ok
FROM read_root(
    '$WORK_DIR/data/ancestor.root',
    dictionary := '$WORK_DIR/build/libTestEvent.so',
    path_prefix := '/TestEvent/vecHit',
    reader_mode := 'serialized',
    raw_validation_entries := 2
);
SELECT count(*) = 3
       AND abs(sum(u) - 6.0) < 0.00001
       AND sum(kind) = 1
       AND min(vecHit_idx) = 0 AND max(vecHit_idx) = 1
       AS serialized_multi_column_relation_ok
FROM read_root(
    '$WORK_DIR/data/ancestor.root',
    dictionary := '$WORK_DIR/build/libTestEvent.so',
    path_prefix := '/TestEvent/vecHit',
    reader_mode := 'serialized',
    raw_validation_entries := 2
);
SELECT count(*) = 3 AND abs(sum(u) - 6.0) < 0.00001 AS serialized_versioned_fixed_member_ok
FROM read_root(
    '$WORK_DIR/legacy/legacy.root',
    dictionary := '$WORK_DIR/build/libTestEvent.so',
    path_prefix := '/TestEvent/vecHit',
    reader_mode := 'serialized',
    raw_validation_entries := 2
);
WITH
uncached AS MATERIALIZED (
    SELECT entry_id, vecHit_idx, u
    FROM read_root(
        '$WORK_DIR/data/ancestor.root',
        dictionary := '$WORK_DIR/build/libTestEvent.so',
        path_prefix := '/TestEvent/vecHit',
        reader_mode := 'serialized',
        tree_cache_bytes := 0,
        raw_validation_entries := 0
    )
    WHERE entry_id < 2
),
cached AS MATERIALIZED (
    SELECT entry_id, vecHit_idx, u
    FROM read_root(
        '$WORK_DIR/data/ancestor.root',
        dictionary := '$WORK_DIR/build/libTestEvent.so',
        path_prefix := '/TestEvent/vecHit',
        reader_mode := 'serialized',
        tree_cache_bytes := 67108864,
        raw_validation_entries := 0
    )
    WHERE entry_id < 2
)
SELECT (SELECT count(*) FROM uncached) > 0
       AND NOT EXISTS (SELECT * FROM uncached EXCEPT ALL SELECT * FROM cached)
       AND NOT EXISTS (SELECT * FROM cached EXCEPT ALL SELECT * FROM uncached)
       AS serialized_cache_range_equivalence_ok;
SELECT count(*) = 6
       AND abs(sum(uv) - 13.5) < 0.00001
       AND min(vecHit_idx) = 0 AND max(vecHit_idx) = 1
       AND min(uv_idx) = 0 AND max(uv_idx) = 1
       AS serialized_versioned_fixed_array_ok
FROM read_root(
    '$WORK_DIR/legacy/legacy.root',
    dictionary := '$WORK_DIR/build/libTestEvent.so',
    path_prefix := '/TestEvent/vecHit/uv',
    reader_mode := 'serialized',
    raw_validation_entries := 2
);
SELECT count(*) = 5
       AND sum(value) = 30
       AND min(vecHit_idx) = 0 AND max(vecHit_idx) = 1
       AND min(refs_idx) = 0 AND max(refs_idx) = 1
       AS serialized_nested_primitive_vector_ok
FROM read_root(
    '$WORK_DIR/data/ancestor.root',
    dictionary := '$WORK_DIR/build/libTestEvent.so',
    path_prefix := '/TestEvent/vecHit/refs',
    reader_mode := 'serialized',
    raw_validation_entries := 2
);
SELECT count(*) = 6
       AND sum(value) = 312
       AND min(vecHit_idx) = 0 AND max(vecHit_idx) = 1
       AND min(labels_idx) = 0 AND max(labels_idx) = 1
       AS serialized_nested_set_ok
FROM read_root(
    '$WORK_DIR/data/ancestor.root',
    dictionary := '$WORK_DIR/build/libTestEvent.so',
    path_prefix := '/TestEvent/vecHit/labels',
    reader_mode := 'serialized',
    raw_validation_entries := 2
);
SELECT count(*) = 3 AND sum(kind) = 1 AS serialized_named_enum_ok
FROM read_root(
    '$WORK_DIR/data/ancestor.root',
    dictionary := '$WORK_DIR/build/libTestEvent.so',
    path_prefix := '/TestEvent/vecHit',
    reader_mode := 'serialized',
    raw_validation_entries := 2
);
SELECT count(*) = 12
       AND sum(value) = 78
       AND min(vecLayer_idx) = 0 AND max(vecLayer_idx) = 1
       AND min(groups_idx) = 0 AND max(groups_idx) = 1
       AND min(hits_idx) = 0 AND max(hits_idx) = 0
       AND min(refs_idx) = 0 AND max(refs_idx) = 1
       AS serialized_arbitrary_depth_ok
FROM read_root(
    '$WORK_DIR/data/ancestor.root',
    dictionary := '$WORK_DIR/build/libTestEvent.so',
    path_prefix := '/TestEvent/vecLayer/groups/hits/refs',
    reader_mode := 'serialized',
    raw_validation_entries := 2
);
SELECT count(*) = 3 AND sum(run) = 303 AS serialized_split_scalar_ok
FROM read_root(
    '$WORK_DIR/data/a.root',
    dictionary := '$WORK_DIR/build/libTestEvent.so',
    path_prefix := '/TestEvent',
    reader_mode := 'serialized',
    raw_validation_entries := 2
);
SELECT count(*) = 18 AND abs(sum(matrix) - 225.0) < 0.00001 AS serialized_root_fixed_array_ok
FROM read_root(
    '$WORK_DIR/data/a.root',
    dictionary := '$WORK_DIR/build/libTestEvent.so',
    path_prefix := '/TestEvent/matrix',
    reader_mode := 'serialized',
    raw_validation_entries := 2
);
SELECT count(*) = 3 AND abs(sum(x) - 3003.0) < 0.00001 AS serialized_split_inline_object_ok
FROM read_root(
    '$WORK_DIR/data/a.root',
    dictionary := '$WORK_DIR/build/libTestEvent.so',
    path_prefix := '/TestEvent/point',
    reader_mode := 'serialized',
    raw_validation_entries := 2
);
SELECT count(*) = 2 AND abs(sum(x) - 2001.0) < 0.00001 AS serialized_member_inline_object_ok
FROM read_root(
    '$WORK_DIR/data/ancestor.root',
    dictionary := '$WORK_DIR/build/libTestEvent.so',
    path_prefix := '/TestEvent/point',
    reader_mode := 'serialized',
    raw_validation_entries := 2
);
SELECT count(*) = 5 AND abs(sum(u) - 53.0) < 0.00001 AS serialized_split_vector_leaf_ok
FROM read_root(
    '$WORK_DIR/data/a.root',
    dictionary := '$WORK_DIR/build/libTestEvent.so',
    path_prefix := '/TestEvent/vecHit',
    reader_mode := 'serialized',
    raw_validation_entries := 2
);
SELECT count(*) = 3 AND sum(big_signed) = 27021597764222982::HUGEINT AS serialized_int64_exact_ok
FROM read_root(
    '$WORK_DIR/data/a.root',
    dictionary := '$WORK_DIR/build/libTestEvent.so',
    path_prefix := '/TestEvent',
    reader_mode := 'serialized',
    raw_validation_entries := 2
);
SELECT count(*) = 3 AND sum(big_unsigned) = 27021597764225982::HUGEINT AS serialized_uint64_exact_ok
FROM read_root(
    '$WORK_DIR/data/a.root',
    dictionary := '$WORK_DIR/build/libTestEvent.so',
    path_prefix := '/TestEvent',
    reader_mode := 'serialized',
    raw_validation_entries := 2
);
SELECT count(*) = 3 AND abs(sum(compressed_float) - 6.75) < 0.01 AS serialized_float16_ok
FROM read_root(
    '$WORK_DIR/data/a.root',
    dictionary := '$WORK_DIR/build/libTestEvent.so',
    path_prefix := '/TestEvent',
    reader_mode := 'serialized',
    raw_validation_entries := 2
);
SELECT count(*) = 3 AND abs(sum(compressed_double) - 303.375) < 0.01 AS serialized_double32_ok
FROM read_root(
    '$WORK_DIR/data/a.root',
    dictionary := '$WORK_DIR/build/libTestEvent.so',
    path_prefix := '/TestEvent',
    reader_mode := 'serialized',
    raw_validation_entries := 2
);
SELECT count(*) = 2 AND sum(run) = 801 AS serialized_unsplit_root_scalar_ok
FROM read_root(
    '$WORK_DIR/data/packed.root',
    dictionary := '$WORK_DIR/build/libTestEvent.so',
    path_prefix := '/TestEvent',
    reader_mode := 'serialized',
    raw_validation_entries := 2
);
SELECT count(*) = 2 AND abs(sum(x) - 2001.0) < 0.00001 AS serialized_unsplit_inline_object_ok
FROM read_root(
    '$WORK_DIR/data/packed.root',
    dictionary := '$WORK_DIR/build/libTestEvent.so',
    path_prefix := '/TestEvent/point',
    reader_mode := 'serialized',
    raw_validation_entries := 2
);
SELECT count(*) = 12 AND abs(sum(matrix) - 90.0) < 0.00001 AS serialized_unsplit_fixed_array_ok
FROM read_root(
    '$WORK_DIR/data/packed.root',
    dictionary := '$WORK_DIR/build/libTestEvent.so',
    path_prefix := '/TestEvent/matrix',
    reader_mode := 'serialized',
    raw_validation_entries := 2
);
SELECT count(*) = 4 AND abs(sum(key) - 4.0) < 0.00001 AS serialized_map_key_ok
FROM read_root(
    '$WORK_DIR/data/packed.root',
    dictionary := '$WORK_DIR/build/libTestEvent.so',
    path_prefix := '/TestEvent/mapScore',
    reader_mode := 'serialized',
    raw_validation_entries := 2
);
SELECT count(*) = 4 AND abs(sum(value) - 1603.0) < 0.00001 AS serialized_map_value_ok
FROM read_root(
    '$WORK_DIR/data/packed.root',
    dictionary := '$WORK_DIR/build/libTestEvent.so',
    path_prefix := '/TestEvent/mapScore',
    reader_mode := 'serialized',
    raw_validation_entries := 2
);
SELECT count(*) = 4 AND sum(value) = 22 AS serialized_set_ok
FROM read_root(
    '$WORK_DIR/data/packed.root',
    dictionary := '$WORK_DIR/build/libTestEvent.so',
    path_prefix := '/TestEvent/setCode',
    reader_mode := 'serialized',
    raw_validation_entries := 2
);
SELECT count(*) = 6 AND abs(sum(first) - 18.0) < 0.00001 AS serialized_pair_first_ok
FROM read_root(
    '$WORK_DIR/data/packed.root',
    dictionary := '$WORK_DIR/build/libTestEvent.so',
    path_prefix := '/TestEvent/nestedPairs/value/value',
    reader_mode := 'serialized',
    raw_validation_entries := 2
);
SELECT count(*) = 6 AND sum(second) = 1203 AS serialized_pair_second_ok
FROM read_root(
    '$WORK_DIR/data/packed.root',
    dictionary := '$WORK_DIR/build/libTestEvent.so',
    path_prefix := '/TestEvent/nestedPairs/value/value',
    reader_mode := 'serialized',
    raw_validation_entries := 2
);
SELECT count(*) = 2 AND abs(sum(score) - 6002.0) < 0.00001 AS serialized_custom_member_ok
FROM read_root(
    '$WORK_DIR/data/packed.root',
    dictionary := '$WORK_DIR/build/libTestEvent.so',
    path_prefix := '/TestEvent/custom',
    reader_mode := 'serialized',
    raw_validation_entries := 2
);
SELECT count(*) = 3 AND sum(exact_id) = 27021597764222980::HUGEINT AS serialized_nested_int64_exact_ok
FROM read_root(
    '$WORK_DIR/data/ancestor.root',
    dictionary := '$WORK_DIR/build/libTestEvent.so',
    path_prefix := '/TestEvent/vecHit',
    reader_mode := 'serialized',
    raw_validation_entries := 2
);
SELECT count(*) = 3 AND sum(exact_uid) = 27021597764225980::HUGEINT AS serialized_nested_uint64_exact_ok
FROM read_root(
    '$WORK_DIR/data/ancestor.root',
    dictionary := '$WORK_DIR/build/libTestEvent.so',
    path_prefix := '/TestEvent/vecHit',
    reader_mode := 'serialized',
    raw_validation_entries := 2
);
SELECT count(*) = 3 AND abs(sum(compressed) - 6.0) < 0.01 AS serialized_nested_float16_ok
FROM read_root(
    '$WORK_DIR/data/ancestor.root',
    dictionary := '$WORK_DIR/build/libTestEvent.so',
    path_prefix := '/TestEvent/vecHit',
    reader_mode := 'serialized',
    raw_validation_entries := 2
);
SELECT count(*) = 3 AND abs(sum(compressed_wide) - 6.375) < 0.01 AS serialized_nested_double32_ok
FROM read_root(
    '$WORK_DIR/data/ancestor.root',
    dictionary := '$WORK_DIR/build/libTestEvent.so',
    path_prefix := '/TestEvent/vecHit',
    reader_mode := 'serialized',
    raw_validation_entries := 2
);
SELECT count(*) = 6 AND abs(sum(key) - 9.0) < 0.00001 AS serialized_split_map_key_ok
FROM read_root(
    '$WORK_DIR/data/a.root',
    dictionary := '$WORK_DIR/build/libTestEvent.so',
    path_prefix := '/TestEvent/mapScore',
    reader_mode := 'serialized',
    raw_validation_entries := 2
);
SELECT count(*) = 6 AND abs(sum(value) - 607.5) < 0.00001 AS serialized_split_map_value_ok
FROM read_root(
    '$WORK_DIR/data/a.root',
    dictionary := '$WORK_DIR/build/libTestEvent.so',
    path_prefix := '/TestEvent/mapScore',
    reader_mode := 'serialized',
    raw_validation_entries := 2
);
SELECT count(*) = 6 AND sum(value) = 36 AS serialized_split_set_ok
FROM read_root(
    '$WORK_DIR/data/a.root',
    dictionary := '$WORK_DIR/build/libTestEvent.so',
    path_prefix := '/TestEvent/setCode',
    reader_mode := 'serialized',
    raw_validation_entries := 2
);
SELECT count(*) = 9
       AND sum(value) = 3309
       AND min(header_idx) = 0
       AND max(header_idx) = 2
       AS serialized_top_level_primitive_vector_ok
FROM read_root(
    '$WORK_DIR/data/a.root',
    dictionary := '$WORK_DIR/build/libTestEvent.so',
    path_prefix := '/TestEvent/header',
    reader_mode := 'serialized',
    raw_validation_entries := 2
);
-- Real nested vector<object>/vector<object>/fixed-array semantics. The
-- serialized decoder must produce the same flattened values and three indexes
-- as ROOT object traversal on an unsplit physical vecHit ancestor.
SELECT count(*) = 9 AND sum(Cov) = 27 AND max(Cov_idx) = 2
       AND min(params_idx) = 0 AND max(params_idx) = 0
       AS nested_fixed_serialized_default_ok
FROM read_root(
    '$WORK_DIR/data/ancestor.root',
    dictionary := '$WORK_DIR/build/libTestEvent.so',
    path_prefix := '/TestEvent/vecHit/params/Cov',
    reader_mode := 'serialized',
    raw_validation_entries := 2
);
SELECT count(*) = 9 AND sum(Cov) = 27 AND max(Cov_idx) = 2
       AS nested_fixed_object_oracle_ok
FROM read_root(
    '$WORK_DIR/data/ancestor.root',
    dictionary := '$WORK_DIR/build/libTestEvent.so',
    path_prefix := '/TestEvent/vecHit/params/Cov',
    reader_mode := 'object'
);
SQL

# Behavioral result alone cannot identify a selected-subtree fallback. Assert
# the byte-offset decoder actually executes for the nested fixed-array fixture.
ROOT4DUCKDB_DEBUG=1 MALLOC_CHECK_=3 "$DUCKDB_BIN" "${DUCKDB_ARGS[@]}" -csv -noheader :memory: \
    > "$WORK_DIR/nested-fixed-path.csv" 2> "$WORK_DIR/nested-fixed-path.log" <<SQL
LOAD '$ROOT_EXTENSION';
SELECT sum(Cov)::BIGINT
FROM read_root(
    '$WORK_DIR/data/ancestor.root',
    dictionary := '$WORK_DIR/build/libTestEvent.so',
    path_prefix := '/TestEvent/vecHit/params/Cov',
    reader_mode := 'serialized',
    raw_validation_entries := 2
);
SQL
if ! grep -qx '27' "$WORK_DIR/nested-fixed-path.csv"; then
    echo "nested fixed decoder failed its SQL value regression" >&2
    cat "$WORK_DIR/nested-fixed-path.csv" >&2
    exit 1
fi
if ! grep -Fq '[SERIALIZED.NESTED_FIXED_OFFSET] path=/TestEvent/vecHit/params/Cov' \
        "$WORK_DIR/nested-fixed-path.log"; then
    echo "nested fixed SQL passed but did not use the direct serialized offset decoder" >&2
    grep -E 'SERIALIZED\\.|ROOT_OBJECT_FALLBACK' "$WORK_DIR/nested-fixed-path.log" >&2 || true
    exit 1
fi
echo "[OK] nested fixed decoder selected byte-offset path, not ROOT scratch" >&2

# Dense serialized Cov indexes must retain the same full coordinates as the
# ROOT object oracle, including value and nested array index positions.
cat > "$WORK_DIR/nested-cov-dense-indices.sql" <<SQL
LOAD '$ROOT_EXTENSION';
WITH
object_rows AS MATERIALIZED (
    SELECT entry_id, vecHit_idx, params_idx, Cov_idx, Cov
    FROM read_root(
        '$WORK_DIR/data/ancestor.root',
        dictionary := '$WORK_DIR/build/libTestEvent.so',
        path_prefix := '/TestEvent/vecHit/params/Cov',
        reader_mode := 'object'
    )
),
serialized_rows AS MATERIALIZED (
    SELECT entry_id, vecHit_idx, params_idx, Cov_idx, Cov
    FROM read_root(
        '$WORK_DIR/data/ancestor.root',
        dictionary := '$WORK_DIR/build/libTestEvent.so',
        path_prefix := '/TestEvent/vecHit/params/Cov',
        reader_mode := 'serialized',
        raw_validation_entries := 0
    )
)
SELECT (SELECT count(*) FROM serialized_rows) = 9
       AND NOT EXISTS (SELECT * FROM object_rows EXCEPT ALL SELECT * FROM serialized_rows)
       AND NOT EXISTS (SELECT * FROM serialized_rows EXCEPT ALL SELECT * FROM object_rows);
SQL
NESTED_COV_DENSE_RESULT="$(MALLOC_CHECK_=3 "$DUCKDB_BIN" "${DUCKDB_ARGS[@]}" -csv -noheader :memory: \
    < "$WORK_DIR/nested-cov-dense-indices.sql")"
if [[ "$NESTED_COV_DENSE_RESULT" != "true" ]]; then
    echo "dense serialized nested Cov indexes differ from ROOT object oracle: $NESTED_COV_DENSE_RESULT" >&2
    exit 1
fi
echo "[OK] dense nested Cov indexes match object oracle" >&2

DIRECT_RESULT="$(MALLOC_CHECK_=3 "$DUCKDB_BIN" "${DUCKDB_ARGS[@]}" -csv -noheader :memory: < "$WORK_DIR/direct-read.sql")"
DIRECT_EXPECTED="$(grep -Ec '^SELECT([[:space:]]|$)' "$WORK_DIR/direct-read.sql")"
DIRECT_PASSED="$(printf '%s\n' "$DIRECT_RESULT" | grep -c '^true$' || true)"
if [[ "$DIRECT_PASSED" -ne "$DIRECT_EXPECTED" ]] || \
        printf '%s\n' "$DIRECT_RESULT" | grep -qv '^true$'; then
    echo "unexpected direct semantic read result: passed=$DIRECT_PASSED expected=$DIRECT_EXPECTED" >&2
    printf '%s\n' "$DIRECT_RESULT" >&2
    exit 1
fi

cat > "$WORK_DIR/direct-leaf-selector.sql" <<SQL
SELECT *
FROM read_root(
    '$WORK_DIR/data/a.root',
    dictionary := '$WORK_DIR/build/libTestEvent.so',
    path_prefix := '/TestEvent/run'
);
SQL
if MALLOC_CHECK_=3 "$DUCKDB_BIN" "${DUCKDB_ARGS[@]}" -csv -noheader :memory: \
    < "$WORK_DIR/direct-leaf-selector.sql" > "$WORK_DIR/direct-leaf-selector.log" 2>&1; then
    echo "primitive leaf unexpectedly remained a read_root relation selector" >&2
    exit 1
fi
if ! grep -q "must select an object or collection" "$WORK_DIR/direct-leaf-selector.log"; then
    echo "leaf-selector failure did not explain the object/collection contract" >&2
    cat "$WORK_DIR/direct-leaf-selector.log" >&2
    exit 1
fi

printf '%s\n' "$WORK_DIR/data/a.root" "$WORK_DIR/data/missing.root" > "$WORK_DIR/data/with-missing.list"
cat > "$WORK_DIR/direct-missing-input.sql" <<SQL
SELECT sum(run)
FROM read_root(
    '@$WORK_DIR/data/with-missing.list',
    dictionary := '$WORK_DIR/build/libTestEvent.so',
    path_prefix := '/TestEvent'
);
SQL
if MALLOC_CHECK_=3 "$DUCKDB_BIN" "${DUCKDB_ARGS[@]}" -csv -noheader :memory: \
    < "$WORK_DIR/direct-missing-input.sql" > "$WORK_DIR/direct-missing-input.log" 2>&1; then
    echo "multi-file direct scan silently accepted an unavailable required input" >&2
    cat "$WORK_DIR/direct-missing-input.log" >&2
    exit 1
fi
if ! grep -q 'Required ROOT input is unavailable:.*missing.root' \
    "$WORK_DIR/direct-missing-input.log"; then
    echo "multi-file missing-input failure was not propagated" >&2
    cat "$WORK_DIR/direct-missing-input.log" >&2
    exit 1
fi

# Fail closed at bind time too: do not skip a missing first input while
# selecting a representative file for schema discovery.
printf '%s\n' "$WORK_DIR/data/missing.root" "$WORK_DIR/data/a.root" > "$WORK_DIR/data/first-missing.list"
cat > "$WORK_DIR/direct-first-missing.sql" <<SQL
SELECT sum(run)
FROM read_root(
    '@$WORK_DIR/data/first-missing.list',
    dictionary := '$WORK_DIR/build/libTestEvent.so',
    path_prefix := '/TestEvent'
);
SQL
if MALLOC_CHECK_=3 "$DUCKDB_BIN" "${DUCKDB_ARGS[@]}" -csv -noheader :memory: \
    < "$WORK_DIR/direct-first-missing.sql" > "$WORK_DIR/direct-first-missing.log" 2>&1; then
    echo "representative-file bind silently skipped a required missing ROOT input" >&2
    exit 1
fi
if ! grep -q 'Failed to open a required ROOT input.*' "$WORK_DIR/direct-first-missing.log"; then
    echo "representative-file bind error did not identify the missing required file" >&2
    cat "$WORK_DIR/direct-first-missing.log" >&2
    exit 1
fi

# The normal scan must select the contiguous std::vector<T> path, and disabling
# it must produce byte-for-byte identical SQL output through the proxy fallback.
ROOT4DUCKDB_DEBUG=1 ROOT4DUCKDB_DEBUG_VERBOSE=1 MALLOC_CHECK_=3 "$DUCKDB_BIN" "${DUCKDB_ARGS[@]}" -csv -noheader :memory: \
    > "$WORK_DIR/contiguous.csv" 2> "$WORK_DIR/contiguous.log" <<SQL
SELECT entry_id, vecHit_idx, u
FROM read_root(
    '$WORK_DIR/data/a.root',
    dictionary := '$WORK_DIR/build/libTestEvent.so',
    path_prefix := '/TestEvent/vecHit',
    reader_mode := 'object'
)
ORDER BY entry_id, vecHit_idx;
SQL
if ! grep -q '\[VECTOR.CONTIGUOUS\].*vector<TestHit>' "$WORK_DIR/contiguous.log"; then
    echo "contiguous vector path was not selected" >&2
    cat "$WORK_DIR/contiguous.log" >&2
    exit 1
fi
ROOT4DUCKDB_DISABLE_CONTIGUOUS_VECTOR=1 MALLOC_CHECK_=3 "$DUCKDB_BIN" "${DUCKDB_ARGS[@]}" -csv -noheader :memory: \
    > "$WORK_DIR/proxy-fallback.csv" <<SQL
SELECT entry_id, vecHit_idx, u
FROM read_root(
    '$WORK_DIR/data/a.root',
    dictionary := '$WORK_DIR/build/libTestEvent.so',
    path_prefix := '/TestEvent/vecHit',
    reader_mode := 'object'
)
ORDER BY entry_id, vecHit_idx;
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

BUILD_RESULT="$(MALLOC_CHECK_=3 "$DUCKDB_BIN" "${DUCKDB_ARGS[@]}" -csv -noheader :memory: < "$WORK_DIR/build-index.sql")"
if [[ "$BUILD_RESULT" != "3" ]]; then
    echo "unexpected index build result: $BUILD_RESULT" >&2
    exit 1
fi

BLOOM_FORMAT_RESULT="$("$DUCKDB_BIN" "${DUCKDB_ARGS[@]}" -csv -noheader :memory: <<SQL
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
ACTUAL_SOURCE_IDS="$("$DUCKDB_BIN" "${DUCKDB_ARGS[@]}" -csv -noheader :memory: <<SQL
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

SCALAR_RESULT="$(MALLOC_CHECK_=3 "$DUCKDB_BIN" "${DUCKDB_ARGS[@]}" -csv -noheader :memory: < "$WORK_DIR/build-scalar-index.sql")"
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

QUERY_RESULT="$(MALLOC_CHECK_=3 "$DUCKDB_BIN" "${DUCKDB_ARGS[@]}" -csv -noheader :memory: < "$WORK_DIR/query.sql")"
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

FIXED_ARRAY_RESULT="$(MALLOC_CHECK_=3 "$DUCKDB_BIN" "${DUCKDB_ARGS[@]}" -csv -noheader :memory: < "$WORK_DIR/fixed-array.sql")"
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

MULTIDIM_RESULT="$(MALLOC_CHECK_=3 "$DUCKDB_BIN" "${DUCKDB_ARGS[@]}" -csv -noheader :memory: < "$WORK_DIR/multidim-array.sql")"
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

INHERITED_RESULT="$(MALLOC_CHECK_=3 "$DUCKDB_BIN" "${DUCKDB_ARGS[@]}" -csv -noheader :memory: < "$WORK_DIR/inherited.sql")"
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

ANCESTOR_RESULT="$(MALLOC_CHECK_=3 "$DUCKDB_BIN" "${DUCKDB_ARGS[@]}" -csv -noheader :memory: < "$WORK_DIR/ancestor.sql")"
if [[ "$ANCESTOR_RESULT" != $'true\ntrue\ntrue\ntrue\ntrue\n1\ntrue' ]]; then
    echo "unexpected ancestor-fallback result: $ANCESTOR_RESULT" >&2
    exit 1
fi

# Explicit serialized mode must not be rejected merely because two indexed
# columns share the same physical ROOT ancestor branch.
cat > "$WORK_DIR/serialized-siblings.sql" <<SQL
LOAD parquet;
SELECT count(*) = 1
       AND min(flattened_values) = 9
       AND bool_and(contains(message, 'serialized=2, object_fallback=0'))
       AND bool_and(CAST(split_part(message, 'shared_basket_loads=', 2) AS UBIGINT) > 0)
       AND bool_and(CAST(split_part(message, 'shared_basket_loads=', 2) AS UBIGINT) <= baskets / 2)
FROM root_build_index(
    '$WORK_DIR/data/ancestor.root',
    'Events',
    '/TestEvent/vecHit/u,/TestEvent/vecHit/uv',
    '$WORK_DIR/index_serialized_siblings',
    dictionary := '$WORK_DIR/build/libTestEvent.so',
    reader_mode := 'serialized',
    raw_validation_entries := 2,
    index_threads := 1,
    bloom_bytes := 64,
    allow_partial := false
)
WHERE status = 'OK';
SQL
SERIALIZED_SIBLINGS_RESULT="$(MALLOC_CHECK_=3 "$DUCKDB_BIN" "${DUCKDB_ARGS[@]}" -csv -noheader :memory: \
    < "$WORK_DIR/serialized-siblings.sql")"
if [[ "$SERIALIZED_SIBLINGS_RESULT" != "true" ]]; then
    echo "explicit serialized sibling paths unexpectedly fell back: $SERIALIZED_SIBLINGS_RESULT" >&2
    exit 1
fi

# AUTO and SERIALIZED must share ROOT basket access where supported.
cat > "$WORK_DIR/auto-serialized-siblings.sql" <<SQL
LOAD parquet;
SELECT count(*) = 1 AND min(flattened_values) = 9
       AND bool_and(contains(message, 'serialized=2, object_fallback=0'))
       AND bool_and(CAST(split_part(message, 'shared_basket_loads=', 2) AS UBIGINT) > 0)
FROM root_build_index(
    '$WORK_DIR/data/ancestor.root', 'Events',
    '/TestEvent/vecHit/u,/TestEvent/vecHit/uv',
    '$WORK_DIR/index_auto_serialized_siblings',
    dictionary := '$WORK_DIR/build/libTestEvent.so',
    reader_mode := 'auto', raw_validation_entries := 2,
    index_threads := 1, bloom_bytes := 64, allow_partial := false
)
WHERE status = 'OK';
SQL
AUTO_SIBLINGS_RESULT="$(MALLOC_CHECK_=3 "$DUCKDB_BIN" "${DUCKDB_ARGS[@]}" -csv -noheader :memory: < "$WORK_DIR/auto-serialized-siblings.sql")"
if [[ "$AUTO_SIBLINGS_RESULT" != "true" ]]; then
    echo "automatic multi-path indexing unexpectedly fell back: $AUTO_SIBLINGS_RESULT" >&2
    exit 1
fi

# One semantic relation may project multiple sibling columns irrespective of
# the physical ROOT branch layout. The object oracle and strict serialized
# route must return identical full row tuples, not just equal row counts.
cat > "$WORK_DIR/semantic-relation-columns.sql" <<SQL
WITH
object_rows AS MATERIALIZED (
    SELECT entry_id, vecHit_idx, u, kind, exact_id
    FROM read_root(
        '$WORK_DIR/data/ancestor.root',
        dictionary := '$WORK_DIR/build/libTestEvent.so',
        path_prefix := '/TestEvent/vecHit',
        reader_mode := 'object'
    )
),
serialized_rows AS MATERIALIZED (
    SELECT entry_id, vecHit_idx, u, kind, exact_id
    FROM read_root(
        '$WORK_DIR/data/ancestor.root',
        dictionary := '$WORK_DIR/build/libTestEvent.so',
        path_prefix := '/TestEvent/vecHit',
        reader_mode := 'serialized',
        raw_validation_entries := 0
    )
)
SELECT (SELECT count(*) FROM object_rows) = 3
       AND NOT EXISTS (SELECT * FROM object_rows EXCEPT ALL SELECT * FROM serialized_rows)
       AND NOT EXISTS (SELECT * FROM serialized_rows EXCEPT ALL SELECT * FROM object_rows);
SQL
SEMANTIC_RELATION_RESULT="$(MALLOC_CHECK_=3 "$DUCKDB_BIN" "${DUCKDB_ARGS[@]}" -csv -noheader :memory: \
    < "$WORK_DIR/semantic-relation-columns.sql")"
if [[ "$SEMANTIC_RELATION_RESULT" != "true" ]]; then
    echo "semantic relation columns diverged between object and serialized ROOT: $SEMANTIC_RELATION_RESULT" >&2
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

MIXED_LAYOUT_RESULT="$(MALLOC_CHECK_=3 "$DUCKDB_BIN" "${DUCKDB_ARGS[@]}" -csv -noheader :memory: < "$WORK_DIR/mixed-layout.sql")"
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
LINEAGE_STATS_RESULT="$(MALLOC_CHECK_=3 "$DUCKDB_BIN" "${DUCKDB_ARGS[@]}" -csv -noheader :memory: < "$WORK_DIR/lineage-stats.sql")"
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
ENTRY_SELECTION_RESULT="$(MALLOC_CHECK_=3 "$DUCKDB_BIN" "${DUCKDB_ARGS[@]}" -csv -noheader :memory: < "$WORK_DIR/entry-selection.sql")"
ENTRY_SELECTION_METRIC="$(profile_metric 'Explicit Entry Selections' <<<"$ENTRY_SELECTION_RESULT" || true)"
if [[ "$(printf '%s\n' "$ENTRY_SELECTION_RESULT" | head -n 1)" != "true" ]] || \
   [[ "$ENTRY_SELECTION_METRIC" != "1" ]]; then
    echo "unexpected exact entry-selection result: $ENTRY_SELECTION_RESULT" >&2
    exit 1
fi

# The ROOT TTree cache is configured per file, never reset to a basket task range.
# Compare complete indexed tuples with the cache enabled and disabled.
cat > "$WORK_DIR/tree-cache-parity.sql" <<SQL
LOAD parquet;
SELECT source_id, entry_id, vecHit_idx, value
FROM read_root_dataset(
    '$WORK_DIR/index',
    '/TestEvent/vecHit/u',
    dictionary := '$WORK_DIR/build/libTestEvent.so',
    reader_mode := 'auto',
    tree_cache_bytes := 67108864
)
WHERE entry_id IN (0, 2)
ORDER BY source_id, entry_id, vecHit_idx, value;
SQL
sed 's/tree_cache_bytes := 67108864/tree_cache_bytes := 0/' \
    "$WORK_DIR/tree-cache-parity.sql" > "$WORK_DIR/tree-no-cache-parity.sql"

MALLOC_CHECK_=3 "$DUCKDB_BIN" "${DUCKDB_ARGS[@]}" -csv -noheader :memory: \
    < "$WORK_DIR/tree-cache-parity.sql" > "$WORK_DIR/tree-cache-parity.csv" \
    2> "$WORK_DIR/tree-cache-parity.err"
MALLOC_CHECK_=3 "$DUCKDB_BIN" "${DUCKDB_ARGS[@]}" -csv -noheader :memory: \
    < "$WORK_DIR/tree-no-cache-parity.sql" > "$WORK_DIR/tree-no-cache-parity.csv" \
    2> "$WORK_DIR/tree-no-cache-parity.err"
if grep -q 'TTreeCache::FillBuffer.*Inconsistency' "$WORK_DIR/tree-cache-parity.err"; then
    echo "ROOT TTreeCache cluster state inconsistent in indexed task scan" >&2
    cat "$WORK_DIR/tree-cache-parity.err" >&2
    exit 1
fi
if ! cmp -s "$WORK_DIR/tree-cache-parity.csv" "$WORK_DIR/tree-no-cache-parity.csv"; then
    echo "indexed tuple values differ with ROOT TTree cache enabled versus disabled" >&2
    diff -u "$WORK_DIR/tree-cache-parity.csv" "$WORK_DIR/tree-no-cache-parity.csv" >&2 || true
    exit 1
fi
echo "[OK] indexed ROOT tree-cache on/off complete tuple parity" >&2


# Adaptive cache must not let a sparse indexed query trigger a full-tree
# TTreeCache read-ahead. Dense scans retain the native ROOT cache, while
# explicit cache values override automatic admission in both directions.
cat > "$WORK_DIR/tree-cache-adaptive.sql" <<SQL
LOAD parquet;
EXPLAIN ANALYZE
SELECT sum(value)
FROM read_root_dataset(
    '$WORK_DIR/index', '/TestEvent/vecHit/u',
    dictionary := '$WORK_DIR/build/libTestEvent.so'
);
EXPLAIN ANALYZE
SELECT sum(value)
FROM read_root_dataset(
    '$WORK_DIR/index', '/TestEvent/vecHit/u',
    dictionary := '$WORK_DIR/build/libTestEvent.so'
)
WHERE entry_id < 1;
EXPLAIN ANALYZE
SELECT sum(value)
FROM read_root_dataset(
    '$WORK_DIR/index', '/TestEvent/vecHit/u',
    dictionary := '$WORK_DIR/build/libTestEvent.so',
    tree_cache_bytes := 67108864
)
WHERE entry_id < 1;
EXPLAIN ANALYZE
SELECT sum(value)
FROM read_root_dataset(
    '$WORK_DIR/index', '/TestEvent/vecHit/u',
    dictionary := '$WORK_DIR/build/libTestEvent.so',
    tree_cache_bytes := 0
);
SQL
CACHE_POLICY_RESULT="$(MALLOC_CHECK_=3 "$DUCKDB_BIN" "${DUCKDB_ARGS[@]}" -csv -noheader :memory: < "$WORK_DIR/tree-cache-adaptive.sql")"
if ! python3 -c '
import re
import sys

profile = sys.stdin.read()
enabled = [int(v) for v in re.findall(r"ROOT Cache Enabled Files:\s*(\d+)", profile)]
disabled = [int(v) for v in re.findall(r"ROOT Cache Disabled Files:\s*(\d+)", profile)]
if len(enabled) != 4 or len(disabled) != 4:
    raise SystemExit("missing adaptive ROOT cache metrics")
if not (enabled[0] > 0 and disabled[0] == 0):
    raise SystemExit("dense default scan did not retain the ROOT cache")
if not (enabled[1] == 0 and disabled[1] > 0):
    raise SystemExit("sparse default scan did not disable the ROOT cache")
if not (enabled[2] > 0 and disabled[2] == 0):
    raise SystemExit("explicit ROOT cache size was not honored")
if not (enabled[3] == 0 and disabled[3] > 0):
    raise SystemExit("explicit ROOT cache disable was not honored")
' <<< "$CACHE_POLICY_RESULT"; then
    echo "unexpected indexed ROOT cache admission: $CACHE_POLICY_RESULT" >&2
    exit 1
fi
echo "[OK] indexed ROOT adaptive cache admission and explicit overrides" >&2

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
LIMITS_RESULT="$(MALLOC_CHECK_=3 "$DUCKDB_BIN" "${DUCKDB_ARGS[@]}" -csv -noheader :memory: < "$WORK_DIR/limits-and-aggregates.sql")"
if [[ "$LIMITS_RESULT" != $'true\ntrue\ntrue\ntrue' ]]; then
    echo "unexpected row-limit/aggregate result: $LIMITS_RESULT" >&2
    exit 1
fi

# File-local entry_id and global event_fk must remain distinct in indexed ROOT scans.
cat > "$WORK_DIR/local-entry-pruning.sql" <<SQL
LOAD parquet;
WITH reference AS MATERIALIZED (
    SELECT source_id, entry_id, vecHit_idx, value
    FROM read_root_dataset('$WORK_DIR/index', '/TestEvent/vecHit/u', dictionary := '$WORK_DIR/build/libTestEvent.so')
), filtered AS MATERIALIZED (
    SELECT source_id, entry_id, vecHit_idx, value
    FROM read_root_dataset('$WORK_DIR/index', '/TestEvent/vecHit/u', dictionary := '$WORK_DIR/build/libTestEvent.so')
    WHERE entry_id = 0
), expected AS MATERIALIZED (SELECT * FROM reference WHERE entry_id = 0)
SELECT (SELECT count(*) FROM expected) > 0
       AND (SELECT count(DISTINCT source_id) FROM expected) > 1
       AND NOT EXISTS (SELECT * FROM expected EXCEPT ALL SELECT * FROM filtered)
       AND NOT EXISTS (SELECT * FROM filtered EXCEPT ALL SELECT * FROM expected);
WITH reference AS MATERIALIZED (
    SELECT source_id, event_fk, entry_id, vecHit_idx, value
    FROM read_root_dataset('$WORK_DIR/index', '/TestEvent/vecHit/u', dictionary := '$WORK_DIR/build/libTestEvent.so')
), filtered AS MATERIALIZED (
    SELECT source_id, event_fk, entry_id, vecHit_idx, value
    FROM read_root_dataset('$WORK_DIR/index', '/TestEvent/vecHit/u', dictionary := '$WORK_DIR/build/libTestEvent.so')
    WHERE entry_id = 0 AND event_fk >= 3 AND value > 0
), expected AS MATERIALIZED (SELECT * FROM reference WHERE entry_id = 0 AND event_fk >= 3 AND value > 0)
SELECT (SELECT count(*) FROM expected) > 0
       AND NOT EXISTS (SELECT * FROM expected EXCEPT ALL SELECT * FROM filtered)
       AND NOT EXISTS (SELECT * FROM filtered EXCEPT ALL SELECT * FROM expected);
SQL
LOCAL_ENTRY_RESULT="$(MALLOC_CHECK_=3 "$DUCKDB_BIN" "${DUCKDB_ARGS[@]}" -csv -noheader :memory: < "$WORK_DIR/local-entry-pruning.sql")"
if [[ "$LOCAL_ENTRY_RESULT" != $'true\ntrue' ]]; then
    echo "file-local entry_id pruning differs from its materialized oracle: $LOCAL_ENTRY_RESULT" >&2
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

EVENT_PRUNING_RESULT="$(MALLOC_CHECK_=3 "$DUCKDB_BIN" "${DUCKDB_ARGS[@]}" -csv -noheader :memory: < "$WORK_DIR/event-pruning.sql")"
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
NULL_PRUNING_RESULT="$(MALLOC_CHECK_=3 "$DUCKDB_BIN" "${DUCKDB_ARGS[@]}" -csv -noheader :memory: < "$WORK_DIR/null-pruning.sql")"
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
IN_BLOOM_RESULT="$(MALLOC_CHECK_=3 "$DUCKDB_BIN" "${DUCKDB_ARGS[@]}" -csv -noheader :memory: < "$WORK_DIR/in-bloom.sql")"
IN_SELECTED_FILES="$(profile_metric 'Selected ROOT Files' <<<"$IN_BLOOM_RESULT" || true)"
IN_DECODED_VALUES="$(profile_metric 'Decoded Values' <<<"$IN_BLOOM_RESULT" || true)"
if [[ "$IN_SELECTED_FILES" != "0" || "$IN_DECODED_VALUES" != "0" ]]; then
    echo "IN equality set should be rejected by basket Bloom filters" >&2
    echo "[IN_BLOOM] selected_files=${IN_SELECTED_FILES:-missing} decoded_values=${IN_DECODED_VALUES:-missing}" >&2
    printf '%s\n' "$IN_BLOOM_RESULT" >&2
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
METADATA_COUNT_RESULT="$(MALLOC_CHECK_=3 "$DUCKDB_BIN" "${DUCKDB_ARGS[@]}" -csv -noheader :memory: < "$WORK_DIR/metadata-count.sql")"
METADATA_ONLY_ROWS="$(profile_metric 'Metadata-only Rows' <<<"$METADATA_COUNT_RESULT" || true)"
METADATA_DECODED_VALUES="$(profile_metric 'Decoded Values' <<<"$METADATA_COUNT_RESULT" || true)"
METADATA_TASKS="$(profile_metric 'Coalesced Read Tasks' <<<"$METADATA_COUNT_RESULT" || true)"
METADATA_OPENED="$(profile_metric 'Opened ROOT Files' <<<"$METADATA_COUNT_RESULT" || true)"
if [[ "$METADATA_ONLY_ROWS" != "11" || "$METADATA_DECODED_VALUES" != "0" ||
      "$METADATA_TASKS" != "0" || "$METADATA_OPENED" != "0" ]]; then
    echo "COUNT(*) should use file cardinalities without task planning or ROOT decoding" >&2
    exit 1
fi

# Physical ancestor's fixed member must stream even with a small value limit.
cat > "$WORK_DIR/serialized-fixed-pager.sql" <<SQL
LOAD parquet;
CREATE TEMP TABLE pager_index_status AS
SELECT *
FROM root_build_index(
    '$WORK_DIR/serialized_page.root', 'Events', '/TestEvent/vecHit/u',
    '$WORK_DIR/serialized_pager_index',
    dictionary := '$WORK_DIR/build/libTestEvent.so',
    reader_mode := 'serialized', raw_max_values_per_entry := 1, index_threads := 1,
    bloom_bytes := 64, allow_partial := false
);
SELECT count(*) = 1 FROM pager_index_status WHERE status = 'OK';
SELECT count(*) = 4099 AND min(value) = 0 AND max(value) = 4098
       AND abs(sum(value) - 8398851.0) < 0.00001
FROM read_root_dataset(
    '$WORK_DIR/serialized_pager_index', '/TestEvent/vecHit/u',
    dictionary := '$WORK_DIR/build/libTestEvent.so',
    reader_mode := 'serialized', raw_max_values_per_entry := 1
);
EXPLAIN ANALYZE SELECT sum(value)
FROM read_root_dataset(
    '$WORK_DIR/serialized_pager_index', '/TestEvent/vecHit/u',
    dictionary := '$WORK_DIR/build/libTestEvent.so',
    reader_mode := 'serialized', raw_max_values_per_entry := 1
);
SQL
PAGER_RESULT="$(MALLOC_CHECK_=3 "$DUCKDB_BIN" "${DUCKDB_ARGS[@]}" -csv -noheader :memory: < "$WORK_DIR/serialized-fixed-pager.sql")"
if [[ "$(printf '%s\n' "$PAGER_RESULT" | grep -c '^true$')" != "2" ]]; then
    echo "bounded serialized cursor regression failed: $PAGER_RESULT" >&2
    exit 1
fi
PAGER_PAGES="$(profile_metric 'Serialized Page Calls' <<<"$PAGER_RESULT" || true)"
PAGER_OBJECT="$(profile_metric 'GetEntry Calls' <<<"$PAGER_RESULT" || true)"
if [[ -z "$PAGER_PAGES" || "$PAGER_PAGES" -lt 3 || "$PAGER_OBJECT" != "0" ]]; then
    echo "serialized pager did not emit bounded pages without ROOT object reads" >&2
    printf '%s\n' "$PAGER_RESULT" >&2
    exit 1
fi
echo "[OK] serialized fixed-member cursor bypassed entry-wide value staging" >&2
# A controlled interruption after completed checkpoint segments must not force
# the indexed file to be processed again when resume := true is requested.
cat > "$WORK_DIR/checkpoint-interrupt.sql" <<SQL
LOAD parquet;
SELECT count(*) FROM root_build_index(
    '$WORK_DIR/serialized_page.root', 'Events', '/TestEvent/vecHit/u',
    '$WORK_DIR/checkpoint-resume-index',
    dictionary := '$WORK_DIR/build/libTestEvent.so',
    reader_mode := 'serialized', raw_validation_entries := 0,
    index_threads := 1, bloom_bytes := 64, allow_partial := false
);
SQL
if ROOT4DUCKDB_TEST_FAIL_AFTER_INDEX=1 MALLOC_CHECK_=3 "$DUCKDB_BIN" "${DUCKDB_ARGS[@]}" \
    -csv -noheader :memory: < "$WORK_DIR/checkpoint-interrupt.sql" \
    >"$WORK_DIR/checkpoint-interrupt.out" 2>"$WORK_DIR/checkpoint-interrupt.err"; then
    echo "interrupted index build unexpectedly succeeded" >&2
    exit 1
fi
if ! grep -q 'ROOT index test interruption after durable checkpoints' "$WORK_DIR/checkpoint-interrupt.err" ||
    [[ -z "$(find "$WORK_DIR/checkpoint-resume-index/failed" -name checkpoint.json -print -quit)" ]]; then
    echo "failed to preserve complete ROOT index checkpoint" >&2
    exit 1
fi
RESUME_RESULT="$(MALLOC_CHECK_=3 "$DUCKDB_BIN" "${DUCKDB_ARGS[@]}" -csv -noheader :memory: <<SQL
LOAD parquet;
WITH status AS MATERIALIZED (
 SELECT * FROM root_build_index(
    '$WORK_DIR/serialized_page.root', 'Events', '/TestEvent/vecHit/u',
    '$WORK_DIR/checkpoint-resume-index',
    dictionary := '$WORK_DIR/build/libTestEvent.so',
    reader_mode := 'serialized', raw_validation_entries := 0,
    index_threads := 1, bloom_bytes := 64,
    allow_partial := false, resume := true
 )
)
SELECT count(*) = 1 AND bool_and(status = 'OK') AND
       bool_and(message = 'recovered validated Parquet checkpoint') FROM status;
SELECT count(*) = 4099 AND min(value) = 0 AND max(value) = 4098
FROM read_root_dataset(
    '$WORK_DIR/checkpoint-resume-index', '/TestEvent/vecHit/u',
    dictionary := '$WORK_DIR/build/libTestEvent.so',
    reader_mode := 'serialized'
);
SQL
)"
if [[ "$(printf '%s\n' "$RESUME_RESULT" | grep -c '^true$')" != "2" ]]; then
    echo "ROOT index checkpoint recovery failed: $RESUME_RESULT" >&2
    exit 1
fi
echo "[OK] index checkpoint resumed without reindexing the completed source file" >&2
# Corrupt a published checkpoint *before* recovery: checksum validation
# must quarantine the segment and rebuild the original ROOT file.
sed 's@checkpoint-resume-index@checkpoint-corrupt-index@g' \
    "$WORK_DIR/checkpoint-interrupt.sql" > "$WORK_DIR/checkpoint-corrupt.sql"
if ROOT4DUCKDB_TEST_FAIL_AFTER_INDEX=1 MALLOC_CHECK_=3 "$DUCKDB_BIN" "${DUCKDB_ARGS[@]}" \
    -csv -noheader :memory: < "$WORK_DIR/checkpoint-corrupt.sql" \
    >"$WORK_DIR/checkpoint-corrupt.out" 2>"$WORK_DIR/checkpoint-corrupt.err"; then
    echo "controlled corrupted checkpoint interruption unexpectedly succeeded" >&2
    exit 1
fi
CORRUPT_PARQUET="$(find "$WORK_DIR/checkpoint-corrupt-index/failed" -name root_baskets.parquet -print -quit)"
if [[ -z "$CORRUPT_PARQUET" ]]; then
    echo "missing committed checkpoint for corruption acceptance" >&2
    exit 1
fi
printf 'truncated-by-interruption' > "$CORRUPT_PARQUET"
CORRUPT_RESULT="$(MALLOC_CHECK_=3 "$DUCKDB_BIN" "${DUCKDB_ARGS[@]}" -csv -noheader :memory: <<SQL
LOAD parquet;
WITH rebuilt AS MATERIALIZED (
 SELECT * FROM root_build_index(
    '$WORK_DIR/serialized_page.root', 'Events', '/TestEvent/vecHit/u',
    '$WORK_DIR/checkpoint-corrupt-index',
    dictionary := '$WORK_DIR/build/libTestEvent.so',
    reader_mode := 'serialized', raw_validation_entries := 0,
    index_threads := 1, bloom_bytes := 64,
    allow_partial := false, resume := true
 )
)
SELECT count(*) = 1 AND bool_and(status = 'OK') AND
       bool_and(message != 'recovered validated Parquet checkpoint') FROM rebuilt;
SELECT count(*) = 4099 FROM read_root_dataset(
    '$WORK_DIR/checkpoint-corrupt-index', '/TestEvent/vecHit/u',
    dictionary := '$WORK_DIR/build/libTestEvent.so', reader_mode := 'serialized'
);
SQL
)"
if [[ "$(printf '%s\n' "$CORRUPT_RESULT" | grep -c '^true$')" != "2" ]]; then
    echo "corrupt checkpoint was reused or not rebuilt: $CORRUPT_RESULT" >&2
    exit 1
fi
echo "[OK] incomplete/corrupted Parquet checkpoint quarantined and reindexed" >&2

EVO_BASE="$("$DUCKDB_BIN" "${DUCKDB_ARGS[@]}" -csv -noheader :memory: <<SQL
LOAD parquet;
SELECT count(*) FROM root_build_index(
    '$WORK_DIR/data/a.root', 'Events', '/TestEvent/run', '$WORK_DIR/index_evolve',
    dictionary := '$WORK_DIR/build/libTestEvent.so', catalog_mode := 'sqlite',
    index_threads := 1, allow_partial := false
) WHERE status='OK';
SELECT root_snapshot_id FROM read_parquet(
    '$WORK_DIR/index_evolve/warehouse/root_index/commits/data/*.parquet'
) WHERE state='COMMITTED' ORDER BY committed_at_ns DESC LIMIT 1;
SQL
)"
EVO_FIRST="$(printf '%s\n' "$EVO_BASE" | head -n 1)"
EVO_SNAPSHOT="$(printf '%s\n' "$EVO_BASE" | tail -n 1)"
if [[ "$EVO_FIRST" != "1" || ! "$EVO_SNAPSHOT" =~ ^[0-9]+$ ]]; then
    echo "invalid initial Iceberg index: $EVO_BASE" >&2
    exit 1
fi
cat > "$WORK_DIR/index-evolve.sql" <<SQL
LOAD parquet;
SELECT count(*) FROM root_build_index(
    '$WORK_DIR/data/a.root', 'Events', '/TestEvent/vecHit/u', '$WORK_DIR/index_evolve',
    dictionary := '$WORK_DIR/build/libTestEvent.so', catalog_mode := 'sqlite',
    update_mode := 'extend', resume := true, index_threads := 1, allow_partial := false
) WHERE status='OK';
SQL
if ROOT4DUCKDB_TEST_FAIL_AFTER_INDEX=1 "$DUCKDB_BIN" "${DUCKDB_ARGS[@]}" \
    -csv -noheader :memory: < "$WORK_DIR/index-evolve.sql" \
    >"$WORK_DIR/index-evolve-interrupted.log" 2>&1; then
    echo "interrupted ROOT path extension unexpectedly succeeded" >&2
    exit 1
fi
EVO_OLD="$("$DUCKDB_BIN" "${DUCKDB_ARGS[@]}" -csv -noheader :memory: <<SQL
LOAD parquet;
SELECT count(*)=3 AND sum(value)=303 FROM read_root_dataset(
    '$WORK_DIR/index_evolve', '/TestEvent/run', dictionary := '$WORK_DIR/build/libTestEvent.so'
);
SQL
)"
if [[ "$EVO_OLD" != "true" ]]; then
    echo "interrupted extension damaged original ROOT path" >&2
    exit 1
fi
EVO_RESULT="$("$DUCKDB_BIN" "${DUCKDB_ARGS[@]}" -csv -noheader :memory: <<SQL
LOAD parquet;
WITH built AS MATERIALIZED (
    SELECT * FROM root_build_index(
        '$WORK_DIR/data/a.root', 'Events', '/TestEvent/vecHit/u', '$WORK_DIR/index_evolve',
        dictionary := '$WORK_DIR/build/libTestEvent.so', catalog_mode := 'sqlite',
        update_mode := 'extend', resume := true, index_threads := 1, allow_partial := false
    )
)
SELECT count(*)=1 AND bool_and(status='OK')
    AND bool_and(message='recovered validated Parquet checkpoint') FROM built;
SELECT count(*)=3 AND sum(value)=303 FROM read_root_dataset(
    '$WORK_DIR/index_evolve', '/TestEvent/run', dictionary := '$WORK_DIR/build/libTestEvent.so'
);
SELECT count(*)>0 FROM read_root_dataset(
    '$WORK_DIR/index_evolve', '/TestEvent/vecHit/u', dictionary := '$WORK_DIR/build/libTestEvent.so'
);
SELECT count(*)=3 AND sum(value)=303 FROM read_root_dataset(
    '$WORK_DIR/index_evolve', '/TestEvent/run',
    dictionary := '$WORK_DIR/build/libTestEvent.so', snapshot_id := '$EVO_SNAPSHOT'
);
SELECT count(DISTINCT root_snapshot_id)=2 FROM read_parquet(
    '$WORK_DIR/index_evolve/warehouse/root_index/commits/data/*.parquet'
) WHERE state='COMMITTED';
SQL
)"
if [[ "$(printf '%s\n' "$EVO_RESULT" | grep -c '^true$')" != "5" ]]; then
    echo "ROOT index extension/recovery failed: $EVO_RESULT" >&2
    exit 1
fi
if "$DUCKDB_BIN" "${DUCKDB_ARGS[@]}" -csv -noheader :memory: \
    < "$WORK_DIR/index-evolve.sql" >"$WORK_DIR/index-evolve-duplicate.log" 2>&1; then
    echo "duplicate indexed ROOT path unexpectedly succeeded" >&2
    exit 1
fi
cat > "$WORK_DIR/index-evolve-publish.sql" <<SQL
LOAD parquet;
SELECT count(*) FROM root_build_index(
    '$WORK_DIR/data/a.root', 'Events', '/TestEvent/flags', '$WORK_DIR/index_evolve',
    dictionary := '$WORK_DIR/build/libTestEvent.so', catalog_mode := 'sqlite',
    update_mode := 'extend', resume := true, index_threads := 1, allow_partial := false
) WHERE status='OK';
SQL
if ROOT4DUCKDB_TEST_FAIL_AFTER_ICEBERG_TABLE=baskets "$DUCKDB_BIN" "${DUCKDB_ARGS[@]}" \
    -csv -noheader :memory: < "$WORK_DIR/index-evolve-publish.sql" \
    >"$WORK_DIR/index-evolve-publish-interrupted.log" 2>&1; then
    echo "partial Iceberg publication unexpectedly succeeded" >&2
    exit 1
fi
EVO_PARTIAL="$("$DUCKDB_BIN" "${DUCKDB_ARGS[@]}" -csv -noheader :memory: <<SQL
LOAD parquet;
SELECT count(*)=3 AND sum(value)=303 FROM read_root_dataset(
    '$WORK_DIR/index_evolve', '/TestEvent/run', dictionary := '$WORK_DIR/build/libTestEvent.so'
);
SELECT count(DISTINCT root_snapshot_id)=2 FROM read_parquet(
    '$WORK_DIR/index_evolve/warehouse/root_index/commits/data/*.parquet'
) WHERE state='COMMITTED';
SQL
)"
if [[ "$(printf '%s\n' "$EVO_PARTIAL" | grep -c '^true$')" != "2" ]]; then
    echo "uncommitted Iceberg generation became visible: $EVO_PARTIAL" >&2
    exit 1
fi
EVO_RECOVER="$("$DUCKDB_BIN" "${DUCKDB_ARGS[@]}" -csv -noheader :memory: <<SQL
LOAD parquet;
WITH built AS MATERIALIZED (
    SELECT * FROM root_build_index(
        '$WORK_DIR/data/a.root', 'Events', '/TestEvent/flags', '$WORK_DIR/index_evolve',
        dictionary := '$WORK_DIR/build/libTestEvent.so', catalog_mode := 'sqlite',
        update_mode := 'extend', resume := true, index_threads := 1, allow_partial := false
    )
)
SELECT count(*)=1 AND bool_and(status='OK')
    AND bool_and(message='recovered validated Parquet checkpoint') FROM built;
SELECT count(*)=3 FROM read_root_dataset(
    '$WORK_DIR/index_evolve', '/TestEvent/flags', dictionary := '$WORK_DIR/build/libTestEvent.so'
);
SELECT count(*)=3 AND sum(value)=303 FROM read_root_dataset(
    '$WORK_DIR/index_evolve', '/TestEvent/run', dictionary := '$WORK_DIR/build/libTestEvent.so'
);
SELECT count(DISTINCT root_snapshot_id)=3 FROM read_parquet(
    '$WORK_DIR/index_evolve/warehouse/root_index/commits/data/*.parquet'
) WHERE state='COMMITTED';
SQL
)"
if [[ "$(printf '%s\n' "$EVO_RECOVER" | grep -c '^true$')" != "4" ]]; then
    echo "Iceberg multi-table commit replay failed: $EVO_RECOVER" >&2
    exit 1
fi
echo "[OK] Iceberg path evolution, previous snapshots and interrupted publication replay" >&2


# The common RootPathReader object page uses reusable numeric scratch buffers.
# Compare every indexed tuple across multiple 2048-row pages against serialized.
OBJECT_NUMERIC_PAGE_PARITY="$(MALLOC_CHECK_=3 "$DUCKDB_BIN" "${DUCKDB_ARGS[@]}" -csv -noheader :memory: <<SQL
LOAD parquet;
WITH object_rows AS MATERIALIZED (
    SELECT event_fk, entry_id, vecHit_idx, value
    FROM read_root_dataset(
        '$WORK_DIR/serialized_pager_index', '/TestEvent/vecHit/u',
        dictionary := '$WORK_DIR/build/libTestEvent.so',
        reader_mode := 'object'
    )
), serialized_rows AS MATERIALIZED (
    SELECT event_fk, entry_id, vecHit_idx, value
    FROM read_root_dataset(
        '$WORK_DIR/serialized_pager_index', '/TestEvent/vecHit/u',
        dictionary := '$WORK_DIR/build/libTestEvent.so',
        reader_mode := 'serialized', raw_max_values_per_entry := 1
    )
)
SELECT (SELECT count(*) FROM object_rows) = 4099
       AND NOT EXISTS (SELECT * FROM object_rows EXCEPT ALL SELECT * FROM serialized_rows)
       AND NOT EXISTS (SELECT * FROM serialized_rows EXCEPT ALL SELECT * FROM object_rows);
SQL
)"
if [[ "$OBJECT_NUMERIC_PAGE_PARITY" != "true" ]]; then
    echo "common ROOT numeric object page differs from serialized tuple oracle: $OBJECT_NUMERIC_PAGE_PARITY" >&2
    exit 1
fi
echo "[OK] common ROOT object numeric page matches 4099 serialized indexed tuples" >&2

# Regression: virtual index columns must remain correct after a 4099-value
# entry crosses multiple SQL chunks; compare strict serialized and object rows.
cat > "$WORK_DIR/direct-large-index-projection.sql" <<SQL
WITH
object_rows AS MATERIALIZED (
    SELECT entry_id, vecHit_idx, u
    FROM read_root(
        '$WORK_DIR/serialized_page.root',
        dictionary := '$WORK_DIR/build/libTestEvent.so',
        path_prefix := '/TestEvent/vecHit',
        reader_mode := 'object'
    )
),
serialized_rows AS MATERIALIZED (
    SELECT entry_id, vecHit_idx, u
    FROM read_root(
        '$WORK_DIR/serialized_page.root',
        dictionary := '$WORK_DIR/build/libTestEvent.so',
        path_prefix := '/TestEvent/vecHit',
        reader_mode := 'serialized',
        raw_validation_entries := 0
    )
)
SELECT (SELECT count(*) FROM serialized_rows) = 4099
       AND (SELECT sum(vecHit_idx) FROM serialized_rows) = 8398851
       AND (SELECT sum(u) FROM serialized_rows) = 8398851
       AND NOT EXISTS (SELECT * FROM object_rows EXCEPT ALL SELECT * FROM serialized_rows)
       AND NOT EXISTS (SELECT * FROM serialized_rows EXCEPT ALL SELECT * FROM object_rows);
SQL
LARGE_INDEX_RESULT="$(MALLOC_CHECK_=3 "$DUCKDB_BIN" "${DUCKDB_ARGS[@]}" -csv -noheader :memory: \
    < "$WORK_DIR/direct-large-index-projection.sql")"
if [[ "$LARGE_INDEX_RESULT" != "true" ]]; then
    echo "direct cached index lookup diverged across ROOT object/serialized chunks: $LARGE_INDEX_RESULT" >&2
    exit 1
fi
echo "[OK] 4099-row direct virtual indexes match ROOT object and serialized readers" >&2

# Single projected fixed-member relation must stream from the shared numeric
# reader in at least three bounded pages, without ROOT object fallback.
DIRECT_PAGED_PROFILE="$(MALLOC_CHECK_=3 "$DUCKDB_BIN" "${DUCKDB_ARGS[@]}" -csv -noheader :memory: <<SQL
LOAD '$ROOT_EXTENSION';
EXPLAIN ANALYZE
SELECT sum(u) FROM read_root(
    '$WORK_DIR/serialized_page.root',
    dictionary := '$WORK_DIR/build/libTestEvent.so',
    path_prefix := '/TestEvent/vecHit',
    reader_mode := 'serialized', raw_validation_entries := 0,
    raw_max_values_per_entry := 1
) WHERE entry_id = 0;
SQL
)"
DIRECT_PAGED_CALLS="$(profile_metric 'Serialized Page Calls' <<<"$DIRECT_PAGED_PROFILE" || true)"
DIRECT_PAGED_OBJECT="$(profile_metric 'Object Fallback Entries' <<<"$DIRECT_PAGED_PROFILE" || true)"
if [[ -z "$DIRECT_PAGED_CALLS" || "$DIRECT_PAGED_CALLS" -lt 3 ||
      "$DIRECT_PAGED_OBJECT" != "0" ]]; then
    echo "direct relation bypassed shared serialized numeric pages: $DIRECT_PAGED_PROFILE" >&2
    exit 1
fi
echo "[OK] direct single-field FIXED_MEMBER uses shared bounded numeric pages" >&2

# The selected semantic relation has two scalar siblings with identical
# vecHit_idx coordinates. Both must stream in lockstep across 4099 values,
# preserving complete rows across SQL vector boundaries.
DIRECT_MULTI_PAGED_PARITY="$(MALLOC_CHECK_=3 "$DUCKDB_BIN" "${DUCKDB_ARGS[@]}" -csv -noheader :memory: <<SQL
WITH object_rows AS MATERIALIZED (
    SELECT entry_id, vecHit_idx, u, exact_id
    FROM read_root(
        '$WORK_DIR/serialized_page.root',
        dictionary := '$WORK_DIR/build/libTestEvent.so',
        path_prefix := '/TestEvent/vecHit', reader_mode := 'object'
    )
), serialized_rows AS MATERIALIZED (
    SELECT entry_id, vecHit_idx, u, exact_id
    FROM read_root(
        '$WORK_DIR/serialized_page.root',
        dictionary := '$WORK_DIR/build/libTestEvent.so',
        path_prefix := '/TestEvent/vecHit',
        reader_mode := 'serialized', raw_validation_entries := 0
    )
), auto_rows AS MATERIALIZED (
    SELECT entry_id, vecHit_idx, u, exact_id
    FROM read_root(
        '$WORK_DIR/serialized_page.root',
        dictionary := '$WORK_DIR/build/libTestEvent.so',
        path_prefix := '/TestEvent/vecHit',
        reader_mode := 'auto', raw_validation_entries := 0
    )
)
SELECT (SELECT count(*) FROM serialized_rows) = 4099
       AND (SELECT count(*) FROM auto_rows) = 4099
       AND NOT EXISTS (SELECT * FROM object_rows EXCEPT ALL SELECT * FROM serialized_rows)
       AND NOT EXISTS (SELECT * FROM serialized_rows EXCEPT ALL SELECT * FROM object_rows)
       AND NOT EXISTS (SELECT * FROM object_rows EXCEPT ALL SELECT * FROM auto_rows)
       AND NOT EXISTS (SELECT * FROM auto_rows EXCEPT ALL SELECT * FROM object_rows);
SQL
)"
if [[ "$DIRECT_MULTI_PAGED_PARITY" != "true" ]]; then
    echo "paged multi-column ROOT relation differs from object oracle: $DIRECT_MULTI_PAGED_PARITY" >&2
    exit 1
fi
DIRECT_MULTI_PAGED_PROFILE="$(MALLOC_CHECK_=3 "$DUCKDB_BIN" "${DUCKDB_ARGS[@]}" -csv -noheader :memory: <<SQL
EXPLAIN ANALYZE
SELECT sum(u), min(exact_id)
FROM read_root(
    '$WORK_DIR/serialized_page.root',
    dictionary := '$WORK_DIR/build/libTestEvent.so',
    path_prefix := '/TestEvent/vecHit',
    reader_mode := 'serialized', raw_validation_entries := 0
)
WHERE entry_id = 0;
SQL
)"
DIRECT_MULTI_PAGES="$(profile_metric 'Serialized Multi-Column Pages' <<<"$DIRECT_MULTI_PAGED_PROFILE" || true)"
DIRECT_MULTI_OBJECT="$(profile_metric 'Object Fallback Entries' <<<"$DIRECT_MULTI_PAGED_PROFILE" || true)"
if [[ -z "$DIRECT_MULTI_PAGES" || "$DIRECT_MULTI_PAGES" -lt 3 ||
      "$DIRECT_MULTI_OBJECT" != "0" ]]; then
    echo "direct scalar siblings did not stream synchronized pages: $DIRECT_MULTI_PAGED_PROFILE" >&2
    exit 1
fi
echo "[OK] direct two-column ROOT relation streams 4099 exact indexed tuples in common pages" >&2

# Evaluate event-only predicates once per ROOT page; retain nested row semantics.
cat > "$WORK_DIR/direct-entry-filter-partition.sql" <<SQL
LOAD '$ROOT_EXTENSION';
WITH oracle AS MATERIALIZED (
    SELECT entry_id, vecHit_idx, params_idx, Cov_idx, Cov
    FROM read_root(
        '$WORK_DIR/data/ancestor.root',
        dictionary := '$WORK_DIR/build/libTestEvent.so',
        path_prefix := '/TestEvent/vecHit/params/Cov',
        reader_mode := 'object'
    )
    WHERE entry_id < 2 AND Cov_idx = 1 AND Cov > 0
), actual AS MATERIALIZED (
    SELECT entry_id, vecHit_idx, params_idx, Cov_idx, Cov
    FROM read_root(
        '$WORK_DIR/data/ancestor.root',
        dictionary := '$WORK_DIR/build/libTestEvent.so',
        path_prefix := '/TestEvent/vecHit/params/Cov',
        reader_mode := 'serialized', raw_validation_entries := 0
    )
    WHERE entry_id < 2 AND Cov_idx = 1 AND Cov > 0
)
SELECT (SELECT count(*) FROM actual) > 0
       AND NOT EXISTS (SELECT * FROM oracle EXCEPT ALL SELECT * FROM actual)
       AND NOT EXISTS (SELECT * FROM actual EXCEPT ALL SELECT * FROM oracle);
WITH oracle AS MATERIALIZED (
    SELECT entry_id, vecHit_idx, u
    FROM read_root(
        '$WORK_DIR/serialized_page.root',
        dictionary := '$WORK_DIR/build/libTestEvent.so',
        path_prefix := '/TestEvent/vecHit', reader_mode := 'object'
    )
    WHERE entry_id = 0 AND vecHit_idx BETWEEN 1023 AND 3073 AND u > 1024
), actual AS MATERIALIZED (
    SELECT entry_id, vecHit_idx, u
    FROM read_root(
        '$WORK_DIR/serialized_page.root',
        dictionary := '$WORK_DIR/build/libTestEvent.so',
        path_prefix := '/TestEvent/vecHit',
        reader_mode := 'serialized', raw_validation_entries := 0
    )
    WHERE entry_id = 0 AND vecHit_idx BETWEEN 1023 AND 3073 AND u > 1024
)
SELECT (SELECT count(*) FROM actual) = 2049
       AND NOT EXISTS (SELECT * FROM oracle EXCEPT ALL SELECT * FROM actual)
       AND NOT EXISTS (SELECT * FROM actual EXCEPT ALL SELECT * FROM oracle);
-- Filtering a virtual index must work even when the index is not projected.
WITH oracle AS MATERIALIZED (
    SELECT u
    FROM read_root(
        '$WORK_DIR/serialized_page.root',
        dictionary := '$WORK_DIR/build/libTestEvent.so',
        path_prefix := '/TestEvent/vecHit', reader_mode := 'object'
    )
    WHERE entry_id = 0 AND vecHit_idx BETWEEN 1023 AND 3073 AND u > 1024
), actual AS MATERIALIZED (
    SELECT u
    FROM read_root(
        '$WORK_DIR/serialized_page.root',
        dictionary := '$WORK_DIR/build/libTestEvent.so',
        path_prefix := '/TestEvent/vecHit',
        reader_mode := 'serialized', raw_validation_entries := 0
    )
    WHERE entry_id = 0 AND vecHit_idx BETWEEN 1023 AND 3073 AND u > 1024
)
SELECT (SELECT count(*) FROM actual) = 2049
       AND NOT EXISTS (SELECT * FROM oracle EXCEPT ALL SELECT * FROM actual)
       AND NOT EXISTS (SELECT * FROM actual EXCEPT ALL SELECT * FROM oracle);
SQL
DIRECT_ENTRY_FILTER_RESULT="$(MALLOC_CHECK_=3 "$DUCKDB_BIN" "${DUCKDB_ARGS[@]}" -csv -noheader :memory: \
    < "$WORK_DIR/direct-entry-filter-partition.sql")"
if [[ $(grep -c '^true$' <<< "$DIRECT_ENTRY_FILTER_RESULT") -ne 3 ]]; then
    echo "direct event/value filter parity failed: $DIRECT_ENTRY_FILTER_RESULT" >&2
    exit 1
fi
echo "[OK] direct event-only and per-value filters preserve exact tuples" >&2

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
-- Predicate and selected-value projections may share a physical ROOT basket.
-- Strict serialized mode must preserve event-level ANY semantics without GetEntry.
SELECT count(*) = 1 AND min(event_fk) = 2 AND max(event_fk) = 2 AND sum(value) = 11
FROM read_root_dataset(
    'main.published_root',
    '/TestEvent/vecHit/u',
    dictionary := '$WORK_DIR/build/libTestEvent.so',
    reader_mode := 'serialized',
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

PUBLICATION_RESULT="$(MALLOC_CHECK_=3 "$DUCKDB_BIN" "${DUCKDB_ARGS[@]}" -csv -noheader :memory: < "$WORK_DIR/catalog-publication.sql")"
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

# An explicitly requested commit ledger must be present and queryable.
# Ordinary SQL catalogs use their _snapshots table as the commit log.
cat > "$WORK_DIR/missing-commit-ledger.sql" <<SQL
CREATE TABLE main.broken_root_snapshots (
    snapshot_id VARCHAR,
    state VARCHAR,
    index_version INTEGER,
    created_at_ns BIGINT
);
INSERT INTO main.broken_root_snapshots
VALUES ('unverified', 'COMMITTED', 12, 1);
SELECT count(*)
FROM read_root_dataset(
    'main.broken_root',
    '/TestEvent/run',
    commits_table := 'main.broken_root_commits'
);
SQL
if "$DUCKDB_BIN" "${DUCKDB_ARGS[@]}" -csv -noheader :memory: \
    < "$WORK_DIR/missing-commit-ledger.sql" > "$WORK_DIR/missing-commit-ledger.log" 2>&1; then
    echo "ROOT catalog accepted a snapshot without its required commit ledger" >&2
    exit 1
fi
if ! grep -q 'broken_root_commits' "$WORK_DIR/missing-commit-ledger.log"; then
    echo "ROOT catalog did not report the missing commit ledger" >&2
    cat "$WORK_DIR/missing-commit-ledger.log" >&2
    exit 1
fi

# Freshness must be checked before pruning (including Bloom pruning) and
# before the metadata-only COUNT(*) fast path. Modify only a generated fixture
# after all ordinary index tests have completed.
touch -m -d '2030-01-01 00:00:00 UTC' "$WORK_DIR/data/a.root"

cat > "$WORK_DIR/stale-index-count.sql" <<SQL
LOAD parquet;
SELECT count(*)
FROM read_root_dataset(
    '$WORK_DIR/index',
    '/TestEvent/vecHit/u',
    dictionary := '$WORK_DIR/build/libTestEvent.so'
);
SQL
if MALLOC_CHECK_=3 "$DUCKDB_BIN" "${DUCKDB_ARGS[@]}" -csv -noheader :memory: \
    < "$WORK_DIR/stale-index-count.sql" > "$WORK_DIR/stale-index-count.log" 2>&1; then
    echo "COUNT(*) accepted stale ROOT index metadata" >&2
    exit 1
fi
if ! grep -q 'ROOT file mtime changed after indexing' "$WORK_DIR/stale-index-count.log"; then
    echo "stale COUNT(*) failed for an unexpected reason" >&2
    cat "$WORK_DIR/stale-index-count.log" >&2
    exit 1
fi

cat > "$WORK_DIR/stale-index-pruning.sql" <<SQL
LOAD parquet;
SELECT count(*)
FROM read_root_dataset(
    '$WORK_DIR/index',
    '/TestEvent/vecHit/u',
    dictionary := '$WORK_DIR/build/libTestEvent.so'
)
WHERE value IN (-999999.0);
SQL
if MALLOC_CHECK_=3 "$DUCKDB_BIN" "${DUCKDB_ARGS[@]}" -csv -noheader :memory: \
    < "$WORK_DIR/stale-index-pruning.sql" > "$WORK_DIR/stale-index-pruning.log" 2>&1; then
    echo "pruned dataset scan accepted stale ROOT index metadata" >&2
    exit 1
fi
if ! grep -q 'ROOT file mtime changed after indexing' "$WORK_DIR/stale-index-pruning.log"; then
    echo "stale pruning scan failed for an unexpected reason" >&2
    cat "$WORK_DIR/stale-index-pruning.log" >&2
    exit 1
fi

# Explicit opt-out remains available for intentional use of stored metadata.
STALE_OVERRIDE_RESULT="$("$DUCKDB_BIN" "${DUCKDB_ARGS[@]}" -csv -noheader :memory: <<SQL
LOAD parquet;
SELECT count(*) = 11
FROM read_root_dataset(
    '$WORK_DIR/index',
    '/TestEvent/vecHit/u',
    dictionary := '$WORK_DIR/build/libTestEvent.so',
    require_fresh_index := false
);
SQL
)"
if [[ "$STALE_OVERRIDE_RESULT" != "true" ]]; then
    echo "explicit require_fresh_index := false did not preserve metadata COUNT(*)" >&2
    exit 1
fi

echo "ROOT4DuckDB integration test passed: direct=$DIRECT_RESULT scalar=$SCALAR_RESULT lakehouse=$QUERY_RESULT fixed=$FIXED_ARRAY_RESULT multidim=$MULTIDIM_RESULT inherited=$INHERITED_RESULT ancestor=$ANCESTOR_RESULT mixed=$MIXED_LAYOUT_RESULT publication=$PUBLICATION_RESULT"
