#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 1 ]]; then
    echo "usage: $0 /absolute/path/to/libDictionary.so" >&2
    exit 2
fi

DICTIONARY="$1"
if [[ ! -f "$DICTIONARY" ]]; then
    echo "dictionary not found: $DICTIONARY" >&2
    exit 2
fi
if ! command -v ldd >/dev/null 2>&1; then
    echo "ldd is required" >&2
    exit 2
fi

DEPS="$(ldd "$DICTIONARY" 2>&1 || true)"
printf '%s\n' "$DEPS"

if grep -q 'not found' <<<"$DEPS"; then
    echo >&2
    echo "ERROR: the ROOT dictionary has unresolved shared-library dependencies." >&2
    exit 1
fi

if grep -Eiq '(^|[[:space:]/])libduckdb([^[:space:]]*)\.so' <<<"$DEPS"; then
    echo >&2
    echo "ERROR: this ROOT dictionary dependency tree contains libduckdb.so." >&2
    echo "A dictionary loaded into the ROOT4DUCKDB DuckDB process must be built without DuckDB." >&2
    echo "Rebuild a clean PHAST dictionary and verify it with this script before using dictionary := ..." >&2
    exit 1
fi

echo >&2
echo "OK: no libduckdb.so dependency and no unresolved libraries were found." >&2
