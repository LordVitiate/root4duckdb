#!/usr/bin/env bash
# Test the shipped C-bootstrap extension inside an independent DuckDB binary.
# This is a runtime/integration test, NOT a claim that a bare LOAD is portable.
set -euo pipefail

PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$PROJECT_DIR"

DUCKDB_BIN="${ROOT4DUCKDB_INDEPENDENT_DUCKDB:-$PROJECT_DIR/.deps/vanilla-duckdb-1.5.6/duckdb}"
EXT="$PROJECT_DIR/build/release/extension/root/root.duckdb_extension"
WORK_DIR="${ROOT4DUCKDB_INDEPENDENT_TEST_DIR:-$PROJECT_DIR/test/.tmp-independent-duckdb}"
FIXTURE_DIR="$PROJECT_DIR/test/fixtures"

fail() {
    printf '[FAIL] %s\n' "$*" >&2
    exit 1
}

[[ -x "$DUCKDB_BIN" ]] || fail "Independent DuckDB not found: $DUCKDB_BIN"
[[ -f "$EXT" ]] || fail "Extension not found: $EXT"
[[ -f "$PROJECT_DIR/.deps/iceberg-env.sh" ]] || fail "Missing .deps/iceberg-env.sh"
[[ -f "$PROJECT_DIR/.deps/build-env.sh" ]] || fail "Missing .deps/build-env.sh"

# Resolve the same GCC/ROOT environment previously used for building.
# Unlike the production bootstrap, the diagnostic test deliberately
# sets the runtime environment before starting the independent DuckDB.
# shellcheck disable=SC1091
source "$PROJECT_DIR/.deps/iceberg-env.sh"
# shellcheck disable=SC1091
source "$PROJECT_DIR/.deps/build-env.sh"

ROOT_CONFIG="${ROOT4DUCKDB_ROOT_CONFIG:-$(command -v root-config || true)}"
[[ -x "$ROOT_CONFIG" ]] || fail "root-config is not executable: $ROOT_CONFIG"
command -v rootcling >/dev/null 2>&1 || fail "rootcling is unavailable"
[[ -x "${CXX:-}" ]] || fail "Compatible C++ compiler was not found in the build environment"

ROOT_LIBDIR="$("$ROOT_CONFIG" --libdir)"
# Do not realpath root-config: the LCG view itself contains the TBB dependencies.
VIEW="$(dirname "$(dirname "$ROOT_CONFIG")")"
CXX_LIB="$("$CXX" -print-file-name=libstdc++.so.6)"
[[ -f "$CXX_LIB" ]] || fail "GCC libstdc++ not found: $CXX_LIB"
CXX_LIBDIR="$(dirname "$(readlink -f "$CXX_LIB")")"

LIB_PATH="$CXX_LIBDIR:$ROOT_LIBDIR"
for dir in "$VIEW/lib64" "$VIEW/lib"; do
    [[ -d "$dir" ]] && LIB_PATH+=":$dir"
done
[[ -f "$VIEW/lib64/libtbb.so.12" || -f "$VIEW/lib/libtbb.so.12" ]] || \
    fail "TBB is not present in inferred LCG view: $VIEW"

# GCC prints its actual include search paths; do not hardcode its version.
mapfile -t CPP_DIRS < <(
    "$CXX" -E -x c++ -v /dev/null 2>&1 |
        awk '
            /#include <\.\.\.> search starts here:/ { inside=1; next }
            /End of search list\./ { inside=0 }
            inside { sub(/^[[:space:]]+/, ""); if (index($0, "/include/c++/") > 0) print }
        '
)
(("${#CPP_DIRS[@]}" > 0)) || fail "Unable to discover GCC C++ include directories"
CPP_PATH="$(IFS=:; echo "${CPP_DIRS[*]}")"
export CPLUS_INCLUDE_PATH="$CPP_PATH${CPLUS_INCLUDE_PATH:+:$CPLUS_INCLUDE_PATH}"
export LD_LIBRARY_PATH="$LIB_PATH${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"

mkdir -p "$WORK_DIR/data" "$WORK_DIR/build"

echo '=== INDEPENDENT DUCKDB ==='
"$DUCKDB_BIN" --version
echo '=== ROOT ==='
"$ROOT_CONFIG" --version
echo '=== GCC ==='
"$CXX" --version | head -1
echo '=== TEST FIXTURE ==='
echo "$WORK_DIR/data/a.root"

# Use the project's existing reference fixture and ROOT dictionary,
# including split branches, nested vectors, and an ancestor ROOT file.
if [[ ! -s "$WORK_DIR/data/a.root" ||
      ! -s "$WORK_DIR/data/ancestor.root" ||
      ! -s "$WORK_DIR/build/libTestEvent.so" ]]; then
    echo '[INFO] Compiling ROOT fixture and dictionary with build GCC'
    # shellcheck source=lib/fixtures.sh
    source "$PROJECT_DIR/scripts/lib/fixtures.sh"
    build_root_fixture "$FIXTURE_DIR" "$WORK_DIR/build" TestEvent make_fixture "$CXX" "$ROOT_CONFIG"

    "$WORK_DIR/build/make_fixture" "$WORK_DIR/data"
fi

[[ -s "$WORK_DIR/data/a.root" ]] || fail "Fixture a.root was not generated"
[[ -s "$WORK_DIR/data/ancestor.root" ]] || fail "Fixture ancestor.root was not generated"

for workers in 1 4; do
    echo
    echo "=== REAL ROOT READ: independent DuckDB, threads=$workers ==="
    sql="$WORK_DIR/check-$workers.sql"
    log="$WORK_DIR/check-$workers.log"

    cat > "$sql" <<SQL
LOAD '$EXT';
SET threads=$workers;

SELECT count(*) = 3 AND sum(run) = 303 AS direct_scalar_ok
FROM read_root(
    '$WORK_DIR/data/a.root',
    dictionary := '$WORK_DIR/build/libTestEvent.so',
    path_prefix := '/TestEvent'
);

SELECT count(*) = 5 AND abs(sum(u) - 53.0) < 0.00001
       AND sum(vecHit_idx) = 6 AS direct_nested_ok
FROM read_root(
    '$WORK_DIR/data/a.root',
    dictionary := '$WORK_DIR/build/libTestEvent.so',
    path_prefix := '/TestEvent/vecHit'
);

SELECT count(*) = 3 AND abs(sum(u) - 6.0) < 0.00001
       AS serialized_nested_ok
FROM read_root(
    '$WORK_DIR/data/ancestor.root',
    dictionary := '$WORK_DIR/build/libTestEvent.so',
    path_prefix := '/TestEvent/vecHit',
    reader_mode := 'serialized',
    raw_validation_entries := 2
);

SELECT count(*) = 12 AND sum(value) = 78
       AS serialized_deeply_nested_ok
FROM read_root(
    '$WORK_DIR/data/ancestor.root',
    dictionary := '$WORK_DIR/build/libTestEvent.so',
    path_prefix := '/TestEvent/vecLayer/groups/hits/refs',
    reader_mode := 'serialized',
    raw_validation_entries := 2
);

SELECT count(*) >= 10 AS root_describe_ok
FROM root_describe(
    '$WORK_DIR/data/a.root', '/TestEvent',
    dictionary := '$WORK_DIR/build/libTestEvent.so'
);
SQL

    # tee displays ALL output live and also saves it.
    # pipefail guarantees that a crashing DuckDB fails this script.
    if ! env -i \
        HOME="$HOME" \
        PATH="$(dirname "$CXX"):/usr/bin:/bin" \
        LD_LIBRARY_PATH="$LIB_PATH" \
        CPLUS_INCLUDE_PATH="$CPP_PATH" \
        ROOT4DUCKDB_ROOT_CONFIG="$ROOT_CONFIG" \
        MALLOC_CHECK_=3 \
        timeout 120 \
        "$DUCKDB_BIN" -unsigned -csv -noheader :memory: \
        < "$sql" 2>&1 | tee "$log"; then
        fail "Independent DuckDB crashed or returned an error (see $log)"
    fi

    successes="$(grep -cx 'true' "$log" || true)"
    [[ "$successes" == 5 ]] || \
        fail "Expected five true results for threads=$workers, got $successes (see $log)"

    if grep -Eq "header 'format' not found|undefined symbol:" "$log"; then
        fail "Cling/compiler or linker errors occurred (see $log)"
    fi
    echo "[OK] All five SQL checks passed at threads=$workers"
done

echo
echo '[OK] Real ROOT reads work in independent DuckDB 1.5.6'
echo "[INFO] Full test logs and generated fixtures: $WORK_DIR"
