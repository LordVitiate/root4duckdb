#!/usr/bin/env bash
# Launch an independent DuckDB with the runtime used to build root4duckdb.
# No shell setup is required from the caller. This does not make an already
# running arbitrary DuckDB process ABI-compatible with GCC14/ROOT.
set -euo pipefail

PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_ENV="$PROJECT_DIR/.deps/build-env.sh"
DUCKDB_BIN="${ROOT4DUCKDB_INDEPENDENT_DUCKDB:-$PROJECT_DIR/.deps/vanilla-duckdb-2.0/duckdb}"

fail() {
    printf '[FAIL] %s\n' "$*" >&2
    exit 2
}

[[ -f "$BUILD_ENV" ]] || fail "Missing $BUILD_ENV; build root4duckdb first"
[[ -x "$DUCKDB_BIN" ]] || fail "Independent DuckDB not executable: $DUCKDB_BIN"

# This file was generated at build time. Its prefix choices are validated
# immediately below. Sourcing happens ONLY in this script's process.
# shellcheck source=/dev/null
source "$BUILD_ENV"

ROOT_CONFIG="${ROOT4DUCKDB_ROOT_CONFIG:-}"
[[ -n "$ROOT_CONFIG" && -x "$ROOT_CONFIG" ]] ||
    fail "Build environment does not identify an executable root-config"

[[ -n "${CXX:-}" && -x "$CXX" ]] ||
    fail "Build environment does not identify an executable C++ compiler"

CXX_LIB="$("$CXX" -print-file-name=libstdc++.so.6)"
[[ -f "$CXX_LIB" ]] || fail "Compiler libstdc++ is unavailable: $CXX_LIB"
CXX_LIB="$(readlink -f "$CXX_LIB")"
CXX_LIBDIR="$(dirname "$CXX_LIB")"

ROOT_LIBDIR="$("$ROOT_CONFIG" --libdir)"
ROOT_VERSION="$("$ROOT_CONFIG" --version)"
[[ -d "$ROOT_LIBDIR" ]] || fail "ROOT library directory is unavailable: $ROOT_LIBDIR"

if [[ -n "${ROOT4DUCKDB_ROOT_VERSION:-}" &&
      "$ROOT_VERSION" != "$ROOT4DUCKDB_ROOT_VERSION" ]]; then
    fail "ROOT version differs from build: wanted $ROOT4DUCKDB_ROOT_VERSION, found $ROOT_VERSION"
fi

# GCC's own include search list, rather than an assumed include/c++/14.3.0.
# Temporarily disable pipefail because GCC's verbose probe is expected to
# return compiler diagnostics in stderr.
CPP_PATH="$(
    "$CXX" -E -x c++ -v /dev/null 2>&1 |
        awk '
            /#include <\.\.\.> search starts here:/ { printing=1; next }
            /End of search list\./ { printing=0 }
            printing && /\/include\/c\+\+\// {
                sub(/^[[:space:]]+/, "")
                if (list != "") list = list ":"
                list = list $0
            }
            END { print list }
        '
)" || fail "C++ standard include path discovery failed"
[[ -n "$CPP_PATH" ]] || fail "GCC did not report any standard C++ include paths"

has_format=0
IFS=: read -ra cpp_dirs <<< "$CPP_PATH"
for dir in "${cpp_dirs[@]}"; do
    if [[ -f "$dir/format" ]]; then has_format=1; break; fi
done
((has_format)) || fail "GCC std::format header is absent from compiler includes"

# build-env.sh already contains its selected runtime library directories.
# Guarantee that GCC's modern libstdc++ is first in the search path.
RUNTIME_LD="$CXX_LIBDIR:$ROOT_LIBDIR${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
[[ -n "$RUNTIME_LD" ]] || fail "Runtime library path is empty"

echo "[RUNTIME] DuckDB:  $DUCKDB_BIN" >&2
echo "[RUNTIME] ROOT:    $ROOT_VERSION ($ROOT_CONFIG)" >&2
echo "[RUNTIME] GCC:     $CXX" >&2
echo "[RUNTIME] C++ ABI: $CXX_LIB" >&2

if [[ "${1:-}" == "--check-runtime" ]]; then
    echo "[RUNTIME] LD_LIBRARY_PATH=$RUNTIME_LD" >&2
    echo "[RUNTIME] CPLUS_INCLUDE_PATH=$CPP_PATH" >&2
    exit 0
fi

# Set these BEFORE exec(), so the dynamic linker sees the correct runtime
# while loading DuckDB itself, not only after an extension LOAD.
exec env \
    LD_LIBRARY_PATH="$RUNTIME_LD" \
    CPLUS_INCLUDE_PATH="$CPP_PATH" \
    ROOT4DUCKDB_ROOT_CONFIG="$ROOT_CONFIG" \
    ROOTSYS="$("$ROOT_CONFIG" --prefix)" \
    "$DUCKDB_BIN" "$@"
