#!/usr/bin/env bash
set -euo pipefail

PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PREFIX="${ROOT4DUCKDB_ICEBERG_PREFIX:-$PROJECT_DIR/.deps/iceberg-cpp}"
QUIET=0

while (($#)); do
    case "$1" in
        --prefix)
            [[ $# -ge 2 ]] || { echo "[ERROR] Missing prefix" >&2; exit 2; }
            PREFIX="$2"; shift 2 ;;
        --quiet)
            QUIET=1; shift ;;
        -h|--help)
            echo "Usage: scripts/check-iceberg.sh [--prefix PATH] [--quiet]"
            exit 0 ;;
        *)
            echo "[ERROR] Unknown argument: $1" >&2
            exit 2 ;;
    esac
done

[[ -f "$PREFIX/.root4duckdb-shared-install" ]] || {
    echo "[ERROR] Missing shared Iceberg install marker below $PREFIX" >&2
    exit 1
}
[[ -f "$PREFIX/include/iceberg/table.h" ]] || {
    echo "[ERROR] Missing Iceberg headers below $PREFIX" >&2
    exit 1
}

library_dir=""
for candidate in "$PREFIX/lib" "$PREFIX/lib64"; do
    if [[ -e "$candidate/libiceberg.so" || -e "$candidate/libiceberg.dylib" ]]; then
        library_dir="$candidate"
        break
    fi
done
[[ -n "$library_dir" ]] || {
    echo "[ERROR] Shared libiceberg was not found below $PREFIX" >&2
    exit 1
}

libraries=()
for name in iceberg iceberg_data iceberg_bundle iceberg_sql_catalog; do
    library=""
    for suffix in so dylib; do
        if [[ -e "$library_dir/lib${name}.${suffix}" ]]; then
            library="$library_dir/lib${name}.${suffix}"
            break
        fi
    done
    [[ -n "$library" ]] || {
        echo "[ERROR] Missing shared Iceberg component: $name" >&2
        exit 1
    }
    libraries+=("$library")
done

if command -v ldd >/dev/null 2>&1; then
    for library in "${libraries[@]}"; do
        missing="$(LD_LIBRARY_PATH="$PREFIX/lib:$PREFIX/lib64:${LD_LIBRARY_PATH:-}" \
                   ldd "$library" 2>/dev/null | sed -n '/not found/p')"
        [[ -z "$missing" ]] || {
            echo "[ERROR] Unresolved runtime dependency for $library:" >&2
            printf '%s\n' "$missing" >&2
            exit 1
        }
    done
fi

if ((QUIET == 0)); then
    echo "[OK] Shared Iceberg runtime: $PREFIX"
    printf '     %s\n' "${libraries[@]}"
fi
