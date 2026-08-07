#!/usr/bin/env bash
set -euo pipefail
PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
ROOT_FILE="${ROOT4DUCKDB_VERIFY_ROOT_FILE:-}"
DICTIONARY="${ROOT4DUCKDB_VERIFY_DICTIONARY:-}"
PATH_FLAGS="${ROOT4DUCKDB_VERIFY_FLAGS_PATH:-/PaEvent/vecParticle/flags}"
FIRST_ENTRIES="${ROOT4DUCKDB_VERIFY_FIRST_ENTRIES:-5000}"
EXPECTED_FIRST_ROWS="${ROOT4DUCKDB_VERIFY_EXPECTED_FIRST_ROWS:-}"
INDEX_PATH="${ROOT4DUCKDB_VERIFY_INDEX_PATH:-}"
REBUILD="${ROOT4DUCKDB_VERIFY_REBUILD:-0}"

python3 "$PROJECT_DIR/scripts/validate_project.py"
if [[ "$REBUILD" == "1" || ! -x "$PROJECT_DIR/build/release/duckdb" ]]; then
  "$PROJECT_DIR/build-root4duckdb.sh" --clean --tests
else
  echo "[INFO] using existing tested build; set ROOT4DUCKDB_VERIFY_REBUILD=1 to rebuild"
fi
DUCKDB="$PROJECT_DIR/run-duckdb.sh"
[[ -x "$DUCKDB" ]] || { echo "missing runtime wrapper: $DUCKDB" >&2; exit 1; }

if [[ -z "$ROOT_FILE" || -z "$DICTIONARY" ]]; then
  echo "[INFO] build/tests passed; set ROOT4DUCKDB_VERIFY_ROOT_FILE and ROOT4DUCKDB_VERIFY_DICTIONARY for PHAST runtime checks"
  exit 0
fi

sql_escape(){ printf '%s' "$1" | sed "s/'/''/g"; }
ROOT_SQL="$(sql_escape "$ROOT_FILE")"; DICT_SQL="$(sql_escape "$DICTIONARY")"; PATH_SQL="$(sql_escape "$PATH_FLAGS")"
RESULT="$($DUCKDB -csv -noheader :memory: -c "
SELECT count(*) AS rows_total,
       count(value) AS rows_non_null,
       count(DISTINCT value) AS distinct_values,
       min(event_id) AS min_event_id,
       max(event_id) AS max_event_id
FROM (
 SELECT event_id, flags AS value
 FROM read_root('$ROOT_SQL', dictionary:='$DICT_SQL', dictionary_cleanup:='retain', path_prefix:='$PATH_SQL')
 WHERE event_id < $FIRST_ENTRIES
);" 2>verify-phast.err)"
IFS=',' read -r ROWS NON_NULL DISTINCT MIN_EVENT MAX_EVENT <<<"$RESULT"
[[ "$ROWS" =~ ^[0-9]+$ && "$NON_NULL" == "$ROWS" && "$DISTINCT" -gt 0 ]] || {
  echo "[FAIL] primitive values are NULL/empty: $RESULT" >&2; cat verify-phast.err >&2; exit 1; }
[[ "$MAX_EVENT" -lt "$FIRST_ENTRIES" ]] || { echo "[FAIL] residual event filter is incorrect: $RESULT" >&2; exit 1; }
if [[ -n "$EXPECTED_FIRST_ROWS" && "$ROWS" != "$EXPECTED_FIRST_ROWS" ]]; then
  echo "[FAIL] expected $EXPECTED_FIRST_ROWS rows, got $ROWS" >&2; exit 1
fi
rm -f verify-phast.err
echo "[OK] PHAST direct read: rows=$ROWS non_null=$NON_NULL distinct=$DISTINCT event_range=$MIN_EVENT..$MAX_EVENT"
echo "[OK] retain cleanup exited normally (no double free)"

if [[ -n "$INDEX_PATH" ]]; then
  INDEX_SQL="$(sql_escape "$INDEX_PATH")"
  INDEX_RESULT="$($DUCKDB -csv -noheader :memory: -c "
SELECT count(*), count(source_id), count(entry_id)
FROM read_root_dataset('$INDEX_SQL', '$PATH_SQL', dictionary:='$DICT_SQL',
                       dictionary_cleanup:='retain', row_limit:=100);
" 2>verify-indexed.err)"
  if [[ "$INDEX_RESULT" != "100,100,100" ]]; then
    echo "[FAIL] indexed row_limit/lineage check: $INDEX_RESULT" >&2
    cat verify-indexed.err >&2
    exit 1
  fi
  rm -f verify-indexed.err
  echo "[OK] indexed row_limit=100 and canonical lineage"
fi
