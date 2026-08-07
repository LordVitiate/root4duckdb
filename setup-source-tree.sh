#!/usr/bin/env bash
set -euo pipefail

PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DEPS_DIR="$PROJECT_DIR/.deps"
BUILD_ENV="$DEPS_DIR/build-env.sh"

DUCKDB_URL="https://github.com/duckdb/duckdb.git"
DUCKDB_VERSION="v1.4.5"
DUCKDB_COMMIT="f31be57c1845a8895169fd58142040be26d433cf"
CI_TOOLS_URL="https://github.com/duckdb/extension-ci-tools.git"
CI_TOOLS_COMMIT="1f04702aae6f4e7ab45f38689fd79e765221b19e"

usage() {
    cat <<'USAGE'
Usage: ./setup-source-tree.sh

Fetches the exact DuckDB and extension-ci-tools commits required by this source
archive. It works both inside a Git checkout and after unzip; submodule metadata
is not required.
USAGE
}

case "${1:-}" in
    -h|--help) usage; exit 0 ;;
    "") ;;
    *) echo "[ERROR] Unknown argument: $1" >&2; usage >&2; exit 2 ;;
esac

mkdir -p "$DEPS_DIR"
if [[ -f "$BUILD_ENV" ]]; then
    # shellcheck disable=SC1090
    source "$BUILD_ENV"
else
    "$PROJECT_DIR/scripts/detect-build-environment.sh" --write "$BUILD_ENV"
    # shellcheck disable=SC1090
    source "$BUILD_ENV"
fi
hash -r

GIT_BIN="${GIT_BIN:-$(command -v git || true)}"
[[ -x "$GIT_BIN" ]] || {
    echo "[ERROR] A working Git executable was not found" >&2
    exit 1
}

for tool in cmake; do
    command -v "$tool" >/dev/null 2>&1 || {
        echo "[ERROR] Required tool not found: $tool" >&2
        exit 1
    }
done

prepare_repository() {
    local name="$1" url="$2" commit="$3"
    local path="$PROJECT_DIR/$name"
    local repository_root=""

    if [[ -d "$path" ]]; then
        repository_root="$("$GIT_BIN" -C "$path" rev-parse --show-toplevel 2>/dev/null || true)"
    fi

    if [[ -e "$path" && "$repository_root" != "$path" ]]; then
        echo "[WARN] Removing incomplete dependency tree: $path" >&2
        case "$path" in
            "$PROJECT_DIR/duckdb"|"$PROJECT_DIR/extension-ci-tools") ;;
            *) echo "[ERROR] Refusing to clean unexpected path: $path" >&2; exit 2 ;;
        esac
        cmake -E remove_directory "$path"
        repository_root=""
    fi

    if [[ "$repository_root" != "$path" ]]; then
        mkdir -p "$path"
        "$GIT_BIN" -C "$path" init --quiet
        "$GIT_BIN" -C "$path" remote add origin "$url"
    else
        if "$GIT_BIN" -C "$path" remote get-url origin >/dev/null 2>&1; then
            "$GIT_BIN" -C "$path" remote set-url origin "$url"
        else
            "$GIT_BIN" -C "$path" remote add origin "$url"
        fi
    fi

    "$GIT_BIN" -C "$path" fetch --force --depth 1 origin "$commit"
    "$GIT_BIN" -C "$path" checkout --detach FETCH_HEAD
    local actual
    actual="$("$GIT_BIN" -C "$path" rev-parse HEAD)"
    [[ "$actual" == "$commit" ]] || {
        echo "[ERROR] $name commit mismatch" >&2
        echo "        expected: $commit" >&2
        echo "        actual:   $actual" >&2
        exit 1
    }
    echo "[OK] $name: $actual"
}

prepare_repository duckdb "$DUCKDB_URL" "$DUCKDB_COMMIT"
prepare_repository extension-ci-tools "$CI_TOOLS_URL" "$CI_TOOLS_COMMIT"

echo "[OK] Source tree is ready for DuckDB $DUCKDB_VERSION"
