#!/usr/bin/env bash
set -euo pipefail

PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

JOBS=4
CLEAN=0
RUN_TESTS=0
PACKAGE_RELEASE=0
ROOT_PREFIX_ARG=""
DUCKDB_VERSION="${DUCKDB_VERSION:-v1.4.5}"

usage() {
    cat <<'USAGE'
Usage: ./build-root4duckdb.sh [options]

Builds ROOT4DuckDB against the Iceberg installation created by
./build-iceberg.sh. The bundled CLI uses shared Iceberg libraries; the
publishable loadable extension embeds the static archives.

Options:
  -j, --jobs N       Parallel build jobs (default: 4)
  --root PREFIX      ROOT installation prefix
  --clean            Remove build/release before building
  --tests            Run SQL and native integration tests
  --package          Test and package publishable release assets
  -h, --help         Show this help
USAGE
}

while (($#)); do
    case "$1" in
        -j|--jobs)
            [[ $# -ge 2 ]] || {
                echo "[ERROR] Missing jobs value" >&2
                exit 2
            }
            JOBS="$2"
            shift 2
            ;;
        --root)
            [[ $# -ge 2 ]] || {
                echo "[ERROR] Missing ROOT prefix" >&2
                exit 2
            }
            ROOT_PREFIX_ARG="$2"
            shift 2
            ;;
        --clean)
            CLEAN=1
            shift
            ;;
        --tests)
            RUN_TESTS=1
            shift
            ;;
        --package)
            PACKAGE_RELEASE=1
            RUN_TESTS=1
            shift
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            echo "[ERROR] Unknown argument: $1" >&2
            usage >&2
            exit 2
            ;;
    esac
done

[[ "$JOBS" =~ ^[1-9][0-9]*$ ]] || {
    echo "[ERROR] --jobs must be a positive integer" >&2
    exit 2
}

if ((JOBS > 8)); then
    echo "[WARN] --jobs=$JOBS can exceed lxplus memory limits; 4 is the safe default."
fi

ICEBERG_ENV="$PROJECT_DIR/.deps/iceberg-env.sh"
if [[ ! -f "$ICEBERG_ENV" ]]; then
    echo "[ERROR] Iceberg is not prepared." >&2
    echo "        Run: ./build-iceberg.sh --jobs $JOBS" >&2
    exit 1
fi

# shellcheck disable=SC1090
source "$ICEBERG_ENV"
hash -r

"$PROJECT_DIR/scripts/check-iceberg.sh" \
    --prefix "$ROOT4DUCKDB_ICEBERG_PREFIX" \
    --quiet

source_root() {
    local prefix
    local candidate

    if [[ -n "$ROOT_PREFIX_ARG" ]]; then
        prefix="$(cd "$ROOT_PREFIX_ARG" && pwd -P)"

        if [[ -x "$prefix/bin/root-config" ]]; then
            export PATH="$prefix/bin:$PATH"
            return
        fi

        candidate="$prefix/bin/thisroot.sh"
        if [[ -f "$candidate" ]]; then
            echo "[INFO] Sourcing ROOT: $candidate"
            # shellcheck disable=SC1090
            source "$candidate"
            return
        fi

        echo "[ERROR] ROOT was not found below prefix: $prefix" >&2
        exit 1
    fi

    if command -v root-config >/dev/null 2>&1; then
        return
    fi

    for candidate in \
        "$PROJECT_DIR/../root/bin/thisroot.sh" \
        "${ROOTSYS:-}/bin/thisroot.sh"; do
        if [[ -n "$candidate" && -f "$candidate" ]]; then
            # shellcheck disable=SC1090
            source "$candidate"
            return
        fi
    done

    echo "[ERROR] root-config is unavailable." >&2
    echo "        Source thisroot.sh or pass --root PREFIX." >&2
    exit 1
}

find_ninja() {
    if command -v ninja >/dev/null 2>&1; then
        return
    fi

    if command -v ninja-build >/dev/null 2>&1; then
        local shim="$PROJECT_DIR/.build-tools"
        mkdir -p "$shim"
        ln -sf "$(command -v ninja-build)" "$shim/ninja"
        export PATH="$shim:$PATH"
        return
    fi

    echo "[ERROR] Required tool not found: ninja (or ninja-build)" >&2
    exit 1
}

check_root_abi() {
    command -v readelf >/dev/null 2>&1 || return
    command -v strings >/dev/null 2>&1 || return

    local libstdcpp
    local required
    local available
    local missing
    local token
    local name
    local library
    local tag

    libstdcpp="$("$CXX" -print-file-name=libstdc++.so.6)"
    [[ -f "$libstdcpp" ]] || return

    required="$(
        for token in $(root-config --libs); do
            [[ "$token" == -l* ]] || continue

            name="${token#-l}"
            library=""

            if [[ -e "$ROOT_LIBDIR/lib${name}.so" ]]; then
                library="$ROOT_LIBDIR/lib${name}.so"
            else
                library="$(
                    find "$ROOT_LIBDIR" \
                        -maxdepth 1 \
                        -name "lib${name}.so.*" \
                        -print \
                        -quit 2>/dev/null || true
                )"
            fi

            [[ -n "$library" ]] || continue

            readelf --version-info "$library" 2>/dev/null |
                sed -nE \
                    's/.*((GLIBCXX|CXXABI)_[0-9]+(\.[0-9]+)+).*/\1/p' ||
                true
        done |
            sort -Vu
    )"

    [[ -n "$required" ]] || return

    available="$(
        strings "$libstdcpp" |
            sed -nE \
                '/^(GLIBCXX|CXXABI)_[0-9]+(\.[0-9]+)+$/p' |
            sort -Vu
    )"

    missing=""
    while IFS= read -r tag; do
        [[ -n "$tag" ]] || continue
        grep -Fxq "$tag" <<<"$available" ||
            missing+="$tag"$'\n'
    done <<<"$required"

    if [[ -n "$missing" ]]; then
        echo "[ERROR] The Iceberg compiler uses an older libstdc++ than ROOT requires." >&2
        echo "[ERROR] CXX=$CXX; libstdc++=$libstdcpp" >&2
        printf '%s' "$missing" >&2
        echo "[HINT] Rebuild Iceberg with the ROOT-compatible compiler:" >&2
        echo "       CC=/path/gcc CXX=/path/g++ ./build-iceberg.sh --clean" >&2
        exit 1
    fi

    echo "[OK] ROOT/libstdc++ ABI preflight passed"
}

source_root
find_ninja

for tool in cmake make python3 root-config; do
    command -v "$tool" >/dev/null 2>&1 || {
        echo "[ERROR] Required tool not found: $tool" >&2
        exit 1
    }
done

[[ -x "${CC:-}" && -x "${CXX:-}" ]] || {
    echo "[ERROR] Iceberg environment does not contain usable CC/CXX" >&2
    exit 1
}

"$CXX" -std=c++23 -x c++ -fsyntax-only - <<'CPP'
#include <expected>
#include <ranges>
#include <string>

int main() {
    auto text =
        std::views::iota(0, 1) |
        std::ranges::to<std::string>();

    return text.empty();
}
CPP

ROOT_INCDIR="$(root-config --incdir)"
ROOT_LIBDIR="$(root-config --libdir)"
ROOT_VERSION="$(root-config --version)"

ICEBERG_LIBRARY_PATH="$ROOT4DUCKDB_ICEBERG_PREFIX/lib"
ICEBERG_LIBRARY_PATH+=":$ROOT4DUCKDB_ICEBERG_PREFIX/lib64"

export LD_LIBRARY_PATH="$ROOT_LIBDIR:$ICEBERG_LIBRARY_PATH${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"

CC_REAL="$(readlink -f "$CC")"
CXX_REAL="$(readlink -f "$CXX")"

CMAKE_TOOLCHAIN_FLAGS="-DCMAKE_C_COMPILER=$CC_REAL"
CMAKE_TOOLCHAIN_FLAGS+=" -DCMAKE_CXX_COMPILER=$CXX_REAL"
CMAKE_DEPENDENCY_FLAGS="-DROOT4DUCKDB_ICEBERG_PREFIX=$ROOT4DUCKDB_ICEBERG_PREFIX"

export EXT_RELEASE_FLAGS="${EXT_RELEASE_FLAGS:-} $CMAKE_TOOLCHAIN_FLAGS $CMAKE_DEPENDENCY_FLAGS"
export EXT_DEBUG_FLAGS="${EXT_DEBUG_FLAGS:-} $CMAKE_TOOLCHAIN_FLAGS $CMAKE_DEPENDENCY_FLAGS"

echo "[0/6] Validating source package"
python3 "$PROJECT_DIR/scripts/validate_project.py"
"$PROJECT_DIR/scripts/test-serialized-codec.sh"
check_root_abi

if [[ ! -f "$PROJECT_DIR/duckdb/CMakeLists.txt" ||
      ! -f "$PROJECT_DIR/extension-ci-tools/makefiles/duckdb_extension.Makefile" ]]; then
    echo "[1/6] Preparing pinned DuckDB sources"
    "$PROJECT_DIR/setup-source-tree.sh"
else
    echo "[1/6] Pinned DuckDB sources already present"
fi

BUILD_DIR="$PROJECT_DIR/build/release"

if ((CLEAN)); then
    echo "[2/6] Removing build/release"
    cmake -E remove_directory "$BUILD_DIR"
else
    echo "[2/6] Keeping incremental build tree"
fi

export GEN=ninja
export CMAKE_BUILD_PARALLEL_LEVEL="$JOBS"
export NINJA_STATUS='[%f/%t running=%r elapsed=%e] '

echo "[3/6] Building ROOT4DuckDB"
echo "[INFO] ROOT=$ROOT_VERSION"
echo "[INFO] CXX=$CXX_REAL ($("$CXX_REAL" --version | head -1))"
echo "[INFO] Iceberg=shared bundled runtime ($ROOT4DUCKDB_ICEBERG_PREFIX)"

make -C "$PROJECT_DIR" \
    release \
    OVERRIDE_GIT_DESCRIBE="$DUCKDB_VERSION"

DUCKDB_BIN="$BUILD_DIR/duckdb"
LOADABLE_EXT="$BUILD_DIR/extension/root/root.duckdb_extension"

[[ -x "$DUCKDB_BIN" ]] || {
    echo "[ERROR] Missing $DUCKDB_BIN" >&2
    exit 1
}

[[ -f "$LOADABLE_EXT" ]] || {
    echo "[ERROR] Missing $LOADABLE_EXT" >&2
    exit 1
}

echo "[4/6] Checking dynamic dependencies"

if command -v ldd >/dev/null 2>&1; then
    for artifact in "$DUCKDB_BIN" "$LOADABLE_EXT"; do
        dependencies="$(ldd "$artifact")"
        missing="$(sed -n '/not found/p' <<<"$dependencies")"

        [[ -z "$missing" ]] || {
            echo "[ERROR] Missing runtime libraries for $artifact:" >&2
            printf '%s\n' "$missing" >&2
            exit 1
        }
    done

    duckdb_dependencies="$(ldd "$DUCKDB_BIN")"

    for component in \
        libiceberg.so \
        libiceberg_data.so \
        libiceberg_bundle.so \
        libiceberg_sql_catalog.so; do
        grep -Fq "$component" <<<"$duckdb_dependencies" || {
            echo "[ERROR] Bundled DuckDB is not linked to $component" >&2
            exit 1
        }
    done
fi

echo "[5/6] Smoke testing registered functions"
"$DUCKDB_BIN" -c "SELECT 42 AS root4duckdb_build_ok;"

"$DUCKDB_BIN" <<'SQL'
SELECT DISTINCT function_name
FROM duckdb_functions()
WHERE function_name IN (
    'read_root',
    'root_build_index',
    'root_build_dataset_index',
    'read_root_dataset',
    'root_dataset_stats',
    'root_iceberg_catalog'
)
ORDER BY function_name;
SQL

if ((RUN_TESTS)); then
    echo "[6/6] Running SQL and native integration tests"

    make -C "$PROJECT_DIR" \
        test_release \
        OVERRIDE_GIT_DESCRIBE="$DUCKDB_VERSION"

    DUCKDB_BIN="$DUCKDB_BIN" \
        "$PROJECT_DIR/scripts/run-integration-test.sh"
else
    echo "[6/6] Tests skipped (use --tests)"
fi

if ((PACKAGE_RELEASE)); then
    echo "[release] Building isolated loadable extension"

    make -C "$PROJECT_DIR" \
        release-package \
        OVERRIDE_GIT_DESCRIBE="$DUCKDB_VERSION"

    ROOT4DUCKDB_BUILD_DIR="$PROJECT_DIR/build/release-package" \
        bash "$PROJECT_DIR/scripts/package-release.sh"
fi

echo "[OK] ROOT4DuckDB build completed"
echo "DuckDB: $DUCKDB_BIN"

if ((PACKAGE_RELEASE)); then
    echo "Release assets: $PROJECT_DIR/dist"
else
    echo "Extension: $LOADABLE_EXT"
fi
