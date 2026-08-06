#!/usr/bin/env bash
set -euo pipefail

PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${ROOT4DUCKDB_BUILD_DIR:-$PROJECT_DIR/build/release}"
JOBS="${ROOT4DUCKDB_JOBS:-4}"
RUN_SMOKE=1
ICEBERG_ENV="$PROJECT_DIR/.deps/iceberg-env.sh"

usage() {
    cat <<'USAGE'
Usage: ./scripts/rebuild-extension.sh [--jobs N] [--no-smoke]

Incrementally rebuilds only the ROOT extension objects, the loadable extension,
and the DuckDB CLI relink that contains the static extension. It does not remove
build/release, download DuckDB, or rebuild unchanged DuckDB objects.
USAGE
}

while (($#)); do
    case "$1" in
        -j|--jobs)
            [[ $# -ge 2 ]] || { echo "[ERROR] Missing jobs value" >&2; exit 2; }
            JOBS="$2"; shift 2 ;;
        --no-smoke)
            RUN_SMOKE=0; shift ;;
        -h|--help)
            usage; exit 0 ;;
        *)
            echo "[ERROR] Unknown argument: $1" >&2
            usage >&2
            exit 2 ;;
    esac
done

[[ "$JOBS" =~ ^[1-9][0-9]*$ ]] || {
    echo "[ERROR] jobs must be a positive integer" >&2
    exit 2
}

if [[ ! -f "$BUILD_DIR/CMakeCache.txt" ]]; then
    echo "[ERROR] Existing configured build tree not found: $BUILD_DIR" >&2
    echo "        Run the full build once. After that, use this script." >&2
    exit 1
fi

[[ -f "$ICEBERG_ENV" ]] || {
    echo "[ERROR] Shared Iceberg environment is missing; run ./build-iceberg.sh" >&2
    exit 1
}
# shellcheck disable=SC1090
source "$ICEBERG_ENV"
"$PROJECT_DIR/scripts/check-iceberg.sh" --prefix "$ROOT4DUCKDB_ICEBERG_PREFIX" --quiet

if command -v root-config >/dev/null 2>&1; then
    ROOT_LIBDIR="$(root-config --libdir)"
    export LD_LIBRARY_PATH="$ROOT_LIBDIR${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
fi

for tool in cmake; do
    command -v "$tool" >/dev/null 2>&1 || {
        echo "[ERROR] Required tool not found: $tool" >&2
        exit 1
    }
done

printf '[INFO] Project: %s\n' "$PROJECT_DIR"
printf '[INFO] Existing build tree: %s\n' "$BUILD_DIR"
printf '[INFO] Parallel jobs: %s\n' "$JOBS"
printf '[INFO] Rebuilding changed extension sources only\n'

# CMake/Ninja tracks dependencies. If only src/root_scan.cpp changed, only that
# translation unit is compiled, then root.duckdb_extension and duckdb are relinked.
cmake --build "$BUILD_DIR" \
    --target root_extension root_loadable_extension duckdb \
    --parallel "$JOBS"

DUCKDB_BIN="$BUILD_DIR/duckdb"
LOADABLE_EXT="$BUILD_DIR/extension/root/root.duckdb_extension"

[[ -x "$DUCKDB_BIN" ]] || {
    echo "[ERROR] DuckDB executable missing after incremental build: $DUCKDB_BIN" >&2
    exit 1
}
[[ -f "$LOADABLE_EXT" ]] || {
    echo "[ERROR] Loadable extension missing after incremental build: $LOADABLE_EXT" >&2
    exit 1
}

if ((RUN_SMOKE)); then
    "$DUCKDB_BIN" -c "SELECT count(*) FROM duckdb_functions() WHERE function_name = 'read_root';" >/dev/null
    echo "[OK] Smoke test passed"
fi

echo "[OK] Incremental extension rebuild completed"
echo "DuckDB: $DUCKDB_BIN"
echo "Extension: $LOADABLE_EXT"
