#!/usr/bin/env bash
set -euo pipefail
DUCKDB_BIN="${1:-./run-duckdb.sh}"
if [[ -n "${2:-}" ]]; then
  CATALOG="$2"
  [[ ! -e "$CATALOG" ]] || {
    echo "[FAIL] Refusing to overwrite existing catalog: $CATALOG" >&2
    exit 2
  }
else
  CATALOG="$(mktemp -d /tmp/root4duckdb-real-iceberg.XXXXXX)"
fi
ROOT_FILE="${ROOT4DUCKDB_VERIFY_ROOT_FILE:-}"
DICTIONARY="${ROOT4DUCKDB_VERIFY_DICTIONARY:-}"
PATH_VALUE="${ROOT4DUCKDB_VERIFY_PATH:-/PaEvent/vecTrack/chi2tot}"
TREE="${ROOT4DUCKDB_VERIFY_TREE:-PaEvent}"

[[ -x "${DUCKDB_BIN}" ]] || { echo "[FAIL] DuckDB binary not found: ${DUCKDB_BIN}" >&2; exit 2; }
[[ -n "${ROOT_FILE}" && -n "${DICTIONARY}" ]] || {
  echo "[FAIL] Set ROOT4DUCKDB_VERIFY_ROOT_FILE and ROOT4DUCKDB_VERIFY_DICTIONARY" >&2
  exit 2
}
PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
REGISTERED="$("${DUCKDB_BIN}" -csv -noheader :memory: -c "SELECT count(*) > 0 FROM duckdb_functions() WHERE function_name='root_build_dataset_index';")"
DUCKDB_ARGS=(-bail)
if [[ "$REGISTERED" == "false" ]]; then
  ROOT_EXTENSION="$PROJECT_DIR/build/release/extension/root/root.duckdb_extension"
  [[ -f "$ROOT_EXTENSION" ]] || { echo "[FAIL] ROOT extension not found: $ROOT_EXTENSION" >&2; exit 2; }
  ROOT_EXTENSION_SQL="${ROOT_EXTENSION//\'/\'\'}"
  DUCKDB_ARGS=(-unsigned -bail -cmd "LOAD '$ROOT_EXTENSION_SQL';")
elif [[ "$REGISTERED" != "true" ]]; then
  echo "[FAIL] Cannot determine ROOT extension registration: $REGISTERED" >&2
  exit 2
fi
"${DUCKDB_BIN}" "${DUCKDB_ARGS[@]}" :memory: <<SQL
SET threads=1;
SET root_max_in_flight_files=1;
SET root_memory_limit='2GB';
SELECT * FROM root_build_dataset_index(
  '${ROOT_FILE}', '${TREE}', '${PATH_VALUE}', '${CATALOG}',
  dictionary := '${DICTIONARY}',
  catalog_mode := 'sqlite'
);
SELECT * FROM root_iceberg_catalog('${CATALOG}') ORDER BY table_name;
SELECT count(*) > 0 AS readable
FROM read_root_dataset('${CATALOG}', '${PATH_VALUE}', dictionary := '${DICTIONARY}');
SQL

test -s "${CATALOG}/catalog.sqlite"
META_COUNT="$(find "${CATALOG}/warehouse" -type f -path '*/metadata/*.metadata.json' | wc -l)"
MANIFEST_COUNT="$(find "${CATALOG}/warehouse" -type f \( -name '*.avro' -o -name '*manifest*' \) | wc -l)"
[[ "${META_COUNT}" -ge 6 ]] || { echo "[FAIL] expected >=6 Iceberg metadata JSON files, got ${META_COUNT}" >&2; exit 1; }
[[ "${MANIFEST_COUNT}" -ge 6 ]] || { echo "[FAIL] expected Iceberg manifests/manifest lists, got ${MANIFEST_COUNT}" >&2; exit 1; }
[[ ! -e "${CATALOG}/current.json" && ! -e "${CATALOG}/_SUCCESS.json" ]] || {
  echo "[FAIL] legacy catalog markers were created" >&2; exit 1;
}
echo "[OK] real local Iceberg catalog: ${CATALOG}"
