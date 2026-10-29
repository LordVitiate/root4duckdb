#!/usr/bin/env bash
# End-to-end acceptance test: real ROOT reads by independent DuckDB with
# NO sourced CERN LCG runtime and NO LD_LIBRARY_PATH or ROOT_* overrides.
# Build the fixture and dictionary with the system ROOT / system GCC so that
# the test does not accidentally depend on an LCG-built fixture dictionary.
set -euo pipefail

PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DUCKDB="${ROOT4DUCKDB_INDEPENDENT_DUCKDB:-$PROJECT_DIR/.deps/vanilla-duckdb-2.0/duckdb}"
EXT="$PROJECT_DIR/build/release/extension/root/root.duckdb_extension"
FIXTURES="$PROJECT_DIR/test/fixtures"
WORK_DIR="${ROOT4DUCKDB_PRISTINE_TEST_DIR:-$(mktemp -d /tmp/root4duckdb-pristine.XXXXXX)}"
LOG_DIR="$WORK_DIR/logs"

fail() {
    printf '[FAIL] %s\n' "$*" >&2
    exit 1
}

clean() {
    env -i HOME="$HOME" PATH=/usr/bin:/bin "$@"
}

for executable in "$DUCKDB" /usr/bin/root-config /usr/bin/rootcling /usr/bin/g++; do
    [[ -x "$executable" ]] || fail "Missing executable: $executable"
done
[[ -f "$EXT" ]] || fail "Missing extension: $EXT"
mkdir -p "$WORK_DIR/build" "$WORK_DIR/data" "$LOG_DIR"

echo '=== CLEAN SYSTEM ROOT ==='
clean /usr/bin/root-config --version
ROOT_LIBDIR="$(clean /usr/bin/root-config --libdir)"
ROOT_CFLAGS_STRING="$(clean /usr/bin/root-config --cflags)"
ROOT_LIBS_STRING="$(clean /usr/bin/root-config --libs)"
read -r -a ROOT_CFLAGS <<< "$ROOT_CFLAGS_STRING"
read -r -a ROOT_LIBS <<< "$ROOT_LIBS_STRING"
echo "ROOT libraries: $ROOT_LIBDIR"
echo '=== INDEPENDENT DUCKDB ==='
clean "$DUCKDB" --version

echo '=== BUILD DICTIONARY USING SYSTEM ROOT (NO LCG) ==='
clean /usr/bin/rootcling \
    -f "$WORK_DIR/build/TestEventDict.cpp" \
    -c \
    -I"$FIXTURES" \
    "$FIXTURES/TestEvent.hpp" \
    "$FIXTURES/TestEventLinkDef.h"

clean /usr/bin/g++ -std=c++17 -O2 -fPIC -shared \
    "${ROOT_CFLAGS[@]}" -I"$FIXTURES" \
    "$FIXTURES/TestEvent.cpp" \
    "$WORK_DIR/build/TestEventDict.cpp" \
    -Wl,-rpath,"$ROOT_LIBDIR" \
    -o "$WORK_DIR/build/libTestEvent.so" \
    "${ROOT_LIBS[@]}"

clean /usr/bin/g++ -std=c++17 -O2 \
    "${ROOT_CFLAGS[@]}" -I"$FIXTURES" \
    "$FIXTURES/make_fixture.cpp" \
    -L"$WORK_DIR/build" -lTestEvent \
    -Wl,-rpath,"$WORK_DIR/build" \
    -Wl,-rpath,"$ROOT_LIBDIR" \
    -o "$WORK_DIR/build/make_fixture" \
    "${ROOT_LIBS[@]}"

echo '=== GENERATE REAL ROOT FILES USING SYSTEM ROOT ==='
clean "$WORK_DIR/build/make_fixture" "$WORK_DIR/data"
[[ -s "$WORK_DIR/data/a.root" && -s "$WORK_DIR/data/ancestor.root" ]] ||
    fail "Missing ROOT test fixtures"

# Use the same 5 semantic checks as test-independent-duckdb.sh, but no
# ROOT environment override of any kind is passed to the SQL process.
for workers in 1 4; do
    echo
    echo "=== PRISTINE REAL ROOT READ: threads=$workers ==="
    sql="$WORK_DIR/check-$workers.sql"
    log="$LOG_DIR/threads-$workers.log"

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

    if clean timeout 120 "$DUCKDB" -unsigned -csv -noheader :memory: \
        < "$sql" 2>&1 | tee "$log"; then
        :
    else
        fail "Clean ROOT SQL failed with threads=$workers (see $log)"
    fi

    successes="$(grep -cx 'true' "$log" || true)"
    [[ "$successes" == 5 ]] ||
        fail "Expected 5 true results for threads=$workers, got $successes"

    if grep -Eq 'fatal error:|Fatal in <|undefined symbol:|GLIBCXX_|CheckABICompatibility|Possible C\+\+ standard library mismatch' "$log"; then
        fail "ROOT/Cling/ABI diagnostic emitted with threads=$workers"
    fi

    echo "[OK] All five ROOT SQL checks passed without environment setup at threads=$workers"
done

echo
echo '[OK] Pristine DuckDB + system ROOT: 10/10 real ROOT SQL checks passed'
echo "[INFO] Fixtures and logs: $WORK_DIR"
