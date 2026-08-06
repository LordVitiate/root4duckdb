#!/usr/bin/env bash
set -euo pipefail

PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ICEBERG_ENV="$PROJECT_DIR/.deps/iceberg-env.sh"
CACHE="$PROJECT_DIR/build/release/CMakeCache.txt"
DUCKDB="$PROJECT_DIR/build/release/duckdb"

[[ -f "$ICEBERG_ENV" ]] || {
    echo "[ERROR] Missing $ICEBERG_ENV; run ./build-iceberg.sh first" >&2
    exit 1
}
[[ -f "$CACHE" && -x "$DUCKDB" ]] || {
    echo "[ERROR] ROOT4DuckDB is not built; run ./build-root4duckdb.sh" >&2
    exit 1
}

# shellcheck disable=SC1090
source "$ICEBERG_ENV"

CXX="$(sed -n 's/^CMAKE_CXX_COMPILER:[^=]*=//p' "$CACHE" | head -1)"
[[ -x "$CXX" ]] || { echo "[ERROR] Invalid cached compiler: $CXX" >&2; exit 1; }

LIBSTDCPP="$($CXX -print-file-name=libstdc++.so.6)"
LIBGCC="$($CXX -print-file-name=libgcc_s.so.1)"
RUNTIME_PATH="$(dirname "$LIBSTDCPP"):$(dirname "$LIBGCC")"
if command -v root-config >/dev/null 2>&1; then
    RUNTIME_PATH="$(root-config --libdir):$RUNTIME_PATH"
fi
export LD_LIBRARY_PATH="$RUNTIME_PATH:$ROOT4DUCKDB_ICEBERG_PREFIX/lib:$ROOT4DUCKDB_ICEBERG_PREFIX/lib64${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"

exec "$DUCKDB" "$@"
