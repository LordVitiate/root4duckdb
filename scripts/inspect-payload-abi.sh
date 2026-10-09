#!/usr/bin/env bash
# Inspect binary ABI constraints for the *existing* root4duckdb payload.
# Use after normal or ROOT4DUCKDB_PAYLOAD_STATIC_LIBSTDCXX=ON build.
# This script never modifies build artifacts or system libraries.
set -euo pipefail

PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PAYLOAD="$PROJECT_DIR/build/release/extension/root/root_payload.duckdb_extension"
BOOTSTRAP="$PROJECT_DIR/build/release/extension/root/root.duckdb_extension"
HOST="$PROJECT_DIR/.deps/vanilla-duckdb-1.5.6/duckdb"
CACHE="$PROJECT_DIR/build/release/CMakeCache.txt"

for t in readelf awk sort; do
    command -v "$t" >/dev/null 2>&1 || { echo "[FAIL] Required tool: $t" >&2; exit 2; }
done
[[ -f "$PAYLOAD" && -f "$BOOTSTRAP" && -x "$HOST" ]] ||
    { echo '[FAIL] Missing payload, bootstrap, or independent DuckDB' >&2; exit 2; }

echo '=== STATIC LIBSTDC++ BUILD OPTION ==='
if [[ -f "$CACHE" ]]; then
    grep -F 'ROOT4DUCKDB_PAYLOAD_STATIC_LIBSTDCXX:BOOL=' "$CACHE" ||
        echo '[INFO] Option absent (older CMake configuration)'
fi

needed_versions() {
    local path="$1"
    # Read *version needs*, not version definitions exported by this ELF.
    readelf --version-info "$path" |
        awk '
          /Version needs section/ { in_need=1; next }
          /Version definition section/ { in_need=0 }
          in_need && /Name: GLIBCXX_/ {
              for (i=1; i<NF; i++)
                  if ($i == "Name:") print $(i+1)
          }
        ' | sort -Vu
}

inspect() {
    local label="$1"
    local path="$2"
    local versions maxv

    echo
    echo "=== $label ==="
    echo "FILE: $path"

    if [[ ! -f "$path" ]]; then
        echo '[MISSING]'
        return
    fi

    echo 'NEEDED:'
    readelf -d "$path" |
        awk '/NEEDED/ { print }'

    versions="$(needed_versions "$path")"
    maxv="$(printf '%s\n' "$versions" | awk 'NF' | tail -1)"

    if [[ -z "$maxv" ]]; then
        echo 'GLIBCXX required directly: NONE'
    else
        echo "GLIBCXX max required directly: $maxv"
        echo 'GLIBCXX version requirements:'
        printf '%s\n' "$versions"
    fi
}

inspect 'INDEPENDENT DUCKDB' "$HOST"
inspect 'PUBLIC C BOOTSTRAP' "$BOOTSTRAP"
inspect 'PRIVATE C++ PAYLOAD' "$PAYLOAD"

if command -v root-config >/dev/null 2>&1; then
    root_libdir="$(root-config --libdir)"
    echo
    echo '=== ROOT SELECTED BY UNMODIFIED PATH ==='
    echo "root-config: $(command -v root-config)"
    echo "version: $(root-config --version)"
    echo "libdir: $root_libdir"
    for name in libCore.so libRIO.so libTree.so libCling.so; do
        inspect "ROOT/$name" "$root_libdir/$name"
    done
else
    echo '[INFO] No root-config on current PATH; ROOT DSO inspection skipped'
fi

echo
echo '=== CLEAN RUNTIME LINKER VIEW (diagnostic only) ==='
env -i PATH=/usr/bin:/bin ldd "$PAYLOAD" || true

echo
echo '=== INTERPRETATION ==='
echo 'A payload with no direct GLIBCXX versions is not automatically portable.'
echo 'ROOT and other needed DSOs can still need newer libstdc++; ABI and'
echo 'cross-DSO object lifetime checks also remain mandatory.'
