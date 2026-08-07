#!/usr/bin/env bash
set -uo pipefail

PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DUCKDB_BIN="${DUCKDB_BIN:-$PROJECT_DIR/build/release/duckdb}"
OUT_DIR="${ROOT4DUCKDB_DEBUG_DIR:-$PROJECT_DIR/artifacts/debug_read_root}"

usage() {
    cat <<'USAGE'
Usage:
  ./scripts/debug-read-root.sh ROOT_FILE DICTIONARY SEMANTIC_PATH [MODE]

MODE:
  explain   bind only (default)
  describe  bind and print output schema
  select    execute and read up to 5 rows

Example:
  ./scripts/debug-read-root.sh \
    /path/file.root /path/libPhast.so /PaSetup/run explain
USAGE
}

if [[ $# -lt 3 || $# -gt 4 ]]; then
    usage >&2
    exit 2
fi

ROOT_FILE="$1"
DICTIONARY="$2"
SEMANTIC_PATH="$3"
MODE="${4:-explain}"

case "$MODE" in
    explain|describe|select) ;;
    *) echo "[ERROR] Invalid mode: $MODE" >&2; exit 2 ;;
esac

[[ -x "$DUCKDB_BIN" ]] || {
    echo "[ERROR] DuckDB binary not found: $DUCKDB_BIN" >&2
    exit 1
}
[[ -f "$ROOT_FILE" ]] || {
    echo "[ERROR] ROOT file not found: $ROOT_FILE" >&2
    exit 1
}
[[ -f "$DICTIONARY" ]] || {
    echo "[ERROR] Dictionary not found: $DICTIONARY" >&2
    exit 1
}

mkdir -p "$OUT_DIR"
STAMP="$(date +%Y%m%d_%H%M%S)"
LOG="$OUT_DIR/${MODE}_${STAMP}_$$.log"
SQL_FILE="$OUT_DIR/${MODE}_${STAMP}_$$.sql"

sql_escape() {
    printf '%s' "$1" | sed "s/'/''/g"
}

ROOT_SQL="$(sql_escape "$ROOT_FILE")"
DICT_SQL="$(sql_escape "$DICTIONARY")"
PATH_SQL="$(sql_escape "$SEMANTIC_PATH")"

case "$MODE" in
    explain)
        PREFIX="EXPLAIN"
        SUFFIX=";"
        ;;
    describe)
        PREFIX="DESCRIBE"
        SUFFIX=";"
        ;;
    select)
        PREFIX=""
        SUFFIX=$'\nLIMIT 5;'
        ;;
esac

cat > "$SQL_FILE" <<SQL
SET threads=1;
$PREFIX
SELECT *
FROM read_root(
    '$ROOT_SQL',
    dictionary := '$DICT_SQL',
    path_prefix := '$PATH_SQL'
)$SUFFIX
SQL

echo "[INFO] Debug log: $LOG"
echo "[INFO] SQL file:  $SQL_FILE"
echo "[INFO] ROOT4DUCKDB_DEBUG=1, MALLOC_CHECK_=3, threads=1"

set +e
env ROOT4DUCKDB_DEBUG=1 MALLOC_CHECK_=3 \
    "$DUCKDB_BIN" -csv -noheader :memory: < "$SQL_FILE" 2>&1 | tee "$LOG"
STATUS=${PIPESTATUS[0]}
set -e

{
    echo
    echo "[DEBUG-RUN] exit_status=$STATUS"
    echo "[DEBUG-RUN] last ROOT4DUCKDB markers:"
    grep '\[ROOT4DUCKDB\]' "$LOG" | tail -n 40 || true
} | tee -a "$LOG"

exit "$STATUS"
