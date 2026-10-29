#!/usr/bin/env bash
set -euo pipefail

PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

JOBS=14
CLEAN=0
RUN_TESTS=0
PACKAGE_RELEASE=0
ROOT_PREFIX_ARG=""
DUCKDB_VERSION="${DUCKDB_VERSION:-v2.0.0}"

usage() {
    cat <<'USAGE'
Usage: ./build-root4duckdb.sh [options]

Builds ROOT4DuckDB against the Iceberg installation created by
./build-iceberg.sh. The bundled CLI uses shared Iceberg libraries; the
publishable loadable extension embeds the static archives.

Options:
  -j, --jobs N       Parallel build jobs (default: 14)
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


ICEBERG_ENV="$PROJECT_DIR/.deps/iceberg-env.sh"
if [[ ! -f "$ICEBERG_ENV" ]]; then
    echo "[ERROR] Iceberg is not prepared." >&2
    echo "        Run: ./build-iceberg.sh --jobs $JOBS" >&2
    exit 1
fi

BUILD_ENV="$PROJECT_DIR/.deps/build-env.sh"

# Resolve a deterministic compiler/ROOT/runtime stack on every invocation.
# iceberg-env.sh sources this same file, so the login-shell environment
# cannot silently choose another ROOT or libstdc++.
"$PROJECT_DIR/scripts/detect-build-environment.sh" --write "$BUILD_ENV"

# shellcheck disable=SC1090
source "$ICEBERG_ENV"
hash -r

# Unless the user explicitly passed --root, use the ROOT selected together
# with the compiler/runtime by detect-build-environment.sh.
if [[ -z "$ROOT_PREFIX_ARG" && -n "${ROOT4DUCKDB_ROOT_PREFIX:-}" ]]; then
    ROOT_PREFIX_ARG="$ROOT4DUCKDB_ROOT_PREFIX"
fi

assert_no_afs_lookup_paths() {
    local variable
    local value
    local part
    local bad=0
    local -a parts

    for variable in \
        PATH \
        LD_LIBRARY_PATH \
        LIBRARY_PATH \
        CPATH \
        CPLUS_INCLUDE_PATH \
        CMAKE_PREFIX_PATH \
        PKG_CONFIG_PATH; do

        value="${!variable-}"
        [[ -n "$value" ]] || continue

        IFS=':' read -r -a parts <<< "$value"
        for part in "${parts[@]}"; do
            [[ -n "$part" ]] || continue
            if [[ "$part" == /afs/* ]]; then
                echo "[ERROR] AFS path in $variable: $part" >&2
                bad=1
            fi
        done
    done

    ((bad == 0)) || {
        echo "[ERROR] AFS is forbidden in ROOT4DuckDB build/runtime lookup paths." >&2
        exit 1
    }

    echo "[OK] AFS absent from build/runtime lookup paths"
}

assert_no_afs_lookup_paths

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

    local compiler_runtime
    local cling_library=""
    local root_runtime=""
    local compiler_glibcxx=""
    local root_glibcxx=""
    local root_probe=""

    compiler_runtime="$(readlink -f "$libstdcpp")"

    for candidate in \
        "$ROOT_LIBDIR/libCling.so" \
        "$ROOT_LIBDIR"/libCling.so.*; do
        if [[ -e "$candidate" ]]; then
            cling_library="$candidate"
            break
        fi
    done

    if [[ -n "$cling_library" ]]; then
        root_runtime="$(
            ldd "$cling_library" 2>/dev/null |
                awk '/libstdc\+\+\.so\.6/ && !found {
                    if ($2 == "=>") {
                        value = $3
                    } else {
                        value = $1
                    }
                    found = 1
                }
                END {
                    if (found) {
                        print value
                    }
                }'
        )"

        if [[ -n "$root_runtime" && -e "$root_runtime" ]]; then
            root_runtime="$(readlink -f "$root_runtime")"

            if [[ "$root_runtime" != "$compiler_runtime" ]]; then
                echo "[ERROR] ROOT and compiler resolve different libstdc++ runtimes." >&2
                echo "        ROOT:     $root_runtime" >&2
                echo "        compiler: $compiler_runtime" >&2
                exit 1
            fi
        fi
    fi

    local compiler_macros
    local compiler_release=""
    local compiler_cxx11_abi=""
    local root_release=""
    local root_cxx11_abi=""

    compiler_macros="$(
        printf '#include <bits/c++config.h>\n' |
            "$CXX" -dM -E -x c++ -
    )"
    compiler_glibcxx="$(
        awk '$2 == "__GLIBCXX__" { print $3 }' <<<"$compiler_macros"
    )"
    compiler_release="$(
        awk '$2 == "_GLIBCXX_RELEASE" { print $3 }' <<<"$compiler_macros"
    )"
    compiler_cxx11_abi="$(
        awk '$2 == "_GLIBCXX_USE_CXX11_ABI" { print $3 }' <<<"$compiler_macros"
    )"

    if command -v root >/dev/null 2>&1; then
        # Evaluate the actual libstdc++ ABI settings used by ROOT/Cling.
        # __GLIBCXX__ alone is a source datestamp, not an ABI identifier.
        if root_probe="$(
            root -l -b -q \
                -e 'std::cout << "__ROOT4DUCKDB_GLIBCXX__=" << __GLIBCXX__ << std::endl; std::cout << "__ROOT4DUCKDB_GLIBCXX_RELEASE__=" << _GLIBCXX_RELEASE << std::endl; std::cout << "__ROOT4DUCKDB_CXX11_ABI__=" << _GLIBCXX_USE_CXX11_ABI << std::endl; std::exit(0);' \
                2>&1
        )"; then
            :
        else
            root_status=$?
            echo "[ERROR] ROOT ABI probe failed with status $root_status" >&2
            printf '%s\n' "$root_probe" >&2
            exit 1
        fi

        root_glibcxx="$(
            sed -nE \
                's/.*__ROOT4DUCKDB_GLIBCXX__=([0-9]+).*/\1/p' \
                <<<"$root_probe"
        )"
        root_release="$(
            sed -nE \
                's/.*__ROOT4DUCKDB_GLIBCXX_RELEASE__=([0-9]+).*/\1/p' \
                <<<"$root_probe"
        )"
        root_cxx11_abi="$(
            sed -nE \
                's/.*__ROOT4DUCKDB_CXX11_ABI__=([01]).*/\1/p' \
                <<<"$root_probe"
        )"

        if [[ -z "$root_glibcxx" || -z "$compiler_glibcxx" ||
              -z "$root_release" || -z "$compiler_release" ||
              -z "$root_cxx11_abi" || -z "$compiler_cxx11_abi" ]]; then
            echo "[ERROR] Could not determine ROOT/compiler libstdc++ ABI settings." >&2
            printf '%s\n' "$root_probe" >&2
            exit 1
        fi

        # _GLIBCXX_RELEASE identifies the header major version, not a binary
        # ABI version. ROOT can use GCC 13 headers while an extension uses
        # GCC 14 headers, provided the dual ABI, symbol versions and linked
        # libstdc++ runtime are compatible. Do not silently ignore mismatches.
        if [[ "$root_cxx11_abi" != "$compiler_cxx11_abi" ]]; then
            echo "[ERROR] ROOT/Cling and compiler use different C++11 string ABIs." >&2
            echo "        ROOT CXX11_ABI=$root_cxx11_abi; compiler CXX11_ABI=$compiler_cxx11_abi" >&2
            exit 1
        fi

        if [[ "$root_release" != "$compiler_release" ]]; then
            echo "[WARN] ROOT/Cling and compiler use different libstdc++ header major versions." >&2
            echo "       ROOT _GLIBCXX_RELEASE=$root_release; compiler _GLIBCXX_RELEASE=$compiler_release" >&2
            echo "       CXX11_ABI matches; required symbols and runtime were checked." >&2
        fi

        if [[ "$root_glibcxx" != "$compiler_glibcxx" ]]; then
            echo "[WARN] ROOT/Cling and compiler have different libstdc++ header datestamps." >&2
            echo "       ROOT __GLIBCXX__=$root_glibcxx; compiler __GLIBCXX__=$compiler_glibcxx" >&2
        fi

        if grep -Eq \
            'CheckABICompatibility|Possible C\+\+ standard library mismatch' \
            <<<"$root_probe"; then
            echo "[ERROR] ROOT/Cling reported a C++ standard-library mismatch." >&2
            printf '%s\n' "$root_probe" >&2
            exit 1
        fi
    fi

    echo "[OK] ROOT/libstdc++ ABI preflight passed"
    echo "[OK] ROOT/compiler CXX11_ABI=$compiler_cxx11_abi (ROOT headers=$root_release compiler headers=$compiler_release)"
    echo "[OK] ROOT/compiler runtime=$compiler_runtime"
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

ROOT_CONFIG_REAL="$(readlink -f "$(command -v root-config)")"
ROOT_INCDIR="$("$ROOT_CONFIG_REAL" --incdir)"
ROOT_LIBDIR="$("$ROOT_CONFIG_REAL" --libdir)"
ROOT_VERSION="$("$ROOT_CONFIG_REAL" --version)"

export ROOT4DUCKDB_ROOT_CONFIG="$ROOT_CONFIG_REAL"

ICEBERG_LIBRARY_PATH="$ROOT4DUCKDB_ICEBERG_PREFIX/lib"
ICEBERG_LIBRARY_PATH+=":$ROOT4DUCKDB_ICEBERG_PREFIX/lib64"

export LD_LIBRARY_PATH="$ROOT_LIBDIR:$ICEBERG_LIBRARY_PATH${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"

CC_REAL="$(readlink -f "$CC")"
CXX_REAL="$(readlink -f "$CXX")"

CMAKE_TOOLCHAIN_FLAGS="-DCMAKE_C_COMPILER=$CC_REAL"
CMAKE_TOOLCHAIN_FLAGS+=" -DCMAKE_CXX_COMPILER=$CXX_REAL"
CMAKE_DEPENDENCY_FLAGS="-DROOT4DUCKDB_ICEBERG_PREFIX=$ROOT4DUCKDB_ICEBERG_PREFIX"
CMAKE_DEPENDENCY_FLAGS+=" -DROOT4DUCKDB_ROOT_CONFIG_EXECUTABLE=$ROOT_CONFIG_REAL"
CMAKE_DEPENDENCY_FLAGS+=" -DROOT4DUCKDB_RUNTIME_BOOTSTRAP=ON"

export EXT_RELEASE_FLAGS="${EXT_RELEASE_FLAGS:-} $CMAKE_TOOLCHAIN_FLAGS $CMAKE_DEPENDENCY_FLAGS"
export EXT_DEBUG_FLAGS="${EXT_DEBUG_FLAGS:-} $CMAKE_TOOLCHAIN_FLAGS $CMAKE_DEPENDENCY_FLAGS"

echo "[0/6] Validating source package"
python3 "$PROJECT_DIR/scripts/validate_project.py"
"$PROJECT_DIR/scripts/test-serialized-codec.sh"
check_root_abi

# The dependency directories survive Git branch switches. Their mere presence
# does not mean that the sources match this branch's DuckDB/API version.
EXPECTED_DUCKDB_COMMIT="d54cbbe3df17e8e5ce68312f483b4155836112aa"
EXPECTED_CI_TOOLS_COMMIT="b8d325c37e82160886579880707493bd358c0e62"

source_tree_matches_pins() {
    [[ -f "$PROJECT_DIR/duckdb/CMakeLists.txt" &&
       -f "$PROJECT_DIR/duckdb/src/include/duckdb/planner/table_filter_set.hpp" &&
       -f "$PROJECT_DIR/extension-ci-tools/makefiles/duckdb_extension.Makefile" ]] || return 1

    [[ "$("$GIT_BIN" -C "$PROJECT_DIR/duckdb" rev-parse HEAD 2>/dev/null)" == "$EXPECTED_DUCKDB_COMMIT" &&
       "$("$GIT_BIN" -C "$PROJECT_DIR/extension-ci-tools" rev-parse HEAD 2>/dev/null)" == "$EXPECTED_CI_TOOLS_COMMIT" ]]
}

SOURCE_TREE_CHANGED=0
if ! source_tree_matches_pins; then
    echo "[1/6] Preparing pinned DuckDB sources (missing or mismatched checkout)"
    "$PROJECT_DIR/setup-source-tree.sh"
    source_tree_matches_pins || {
        echo "[ERROR] Dependency sources do not match the pinned revisions after setup." >&2
        exit 1
    }
    SOURCE_TREE_CHANGED=1
else
    echo "[1/6] Pinned DuckDB sources already present and verified"
fi

BUILD_DIR="$PROJECT_DIR/build/release"

# A CMake build directory belongs to one DuckDB source revision. Do not reuse
# compiled objects or cached API settings after changing pinned dependencies.
if (( SOURCE_TREE_CHANGED && !CLEAN )); then
    echo "[INFO] DuckDB dependencies changed; resetting the stale CMake build directory."
    CLEAN=1
fi

# An older DuckDB checkout can leave CMAKE_CXX_STANDARD=11 in the cache.
# DuckDB 2.0 rejects this setting before its own default C++17 policy applies.
if (( !CLEAN )) && [[ -f "$BUILD_DIR/CMakeCache.txt" ]]; then
    cached_cxx_standard="$(
        sed -nE 's/^CMAKE_CXX_STANDARD:[^=]*=([0-9]+)$/\1/p' "$BUILD_DIR/CMakeCache.txt"
    )"
    if [[ "$cached_cxx_standard" =~ ^[0-9]+$ ]] && (( cached_cxx_standard < 17 )); then
        echo "[INFO] Cached C++ standard $cached_cxx_standard is incompatible with DuckDB 2.0."
        echo "[INFO] Resetting build/release to apply DuckDB's C++17 default."
        CLEAN=1
    fi
fi

if ((CLEAN == 0)) && [[ -f "$BUILD_DIR/CMakeCache.txt" ]]; then
    cached_root_config="$(
        sed -n             's/^ROOT_CONFIG_EXECUTABLE:FILEPATH=//p'             "$BUILD_DIR/CMakeCache.txt"
    )"

    if [[ -n "$cached_root_config" ]]; then
        cached_root_config_real="$cached_root_config"
        if [[ -e "$cached_root_config" ]]; then
            cached_root_config_real="$(readlink -f "$cached_root_config")"
        fi

        if [[ "$cached_root_config_real" != "$ROOT_CONFIG_REAL" ]]; then
            echo "[INFO] ROOT changed since the previous CMake configure."
            echo "       cached:   $cached_root_config_real"
            echo "       selected: $ROOT_CONFIG_REAL"
            echo "[INFO] Resetting build/release to prevent mixed ROOT runtimes."
            cmake -E remove_directory "$BUILD_DIR"
        fi
    fi
fi

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

        expected_stdlib="$(
            readlink -f "$("$CXX_REAL" -print-file-name=libstdc++.so.6)"
        )"

        actual_stdlib="$(
            awk '/libstdc\+\+\.so\.6/ && !found {
                if ($2 == "=>") {
                    value = $3
                } else {
                    value = $1
                }
                found = 1
            }
            END {
                if (found) {
                    print value
                }
            }' <<<"$dependencies"
        )"

        if [[ -n "$actual_stdlib" && -e "$actual_stdlib" ]]; then
            actual_stdlib="$(readlink -f "$actual_stdlib")"

            if [[ "$actual_stdlib" != "$expected_stdlib" ]]; then
                echo "[ERROR] Wrong libstdc++ runtime for $artifact" >&2
                echo "        expected: $expected_stdlib" >&2
                echo "        actual:   $actual_stdlib" >&2
                exit 1
            fi
        fi
    done

    # ROOT is built with DONT_LINK in bootstrap mode. The DuckDB host
    # must not carry another ROOT/Iceberg implementation: the loadable
    # extension embeds Iceberg and discovers ROOT at load time.
    if command -v readelf >/dev/null 2>&1; then
        for artifact in "$DUCKDB_BIN" "$LOADABLE_EXT"; do
            if readelf -d "$artifact" |
                grep -E '\(NEEDED\)' |
                grep -Eq 'libiceberg(_data|_bundle|_sql_catalog)?\.so|lib(Core|RIO|Tree|Hist|Cling)\.so'; then
                echo "[ERROR] Unexpected direct ROOT/Iceberg dependency in $artifact" >&2
                exit 1
            fi
        done
    fi
fi

echo "[5/6] Smoke testing loadable ROOT extension"
"$DUCKDB_BIN" -c "SELECT 42 AS root4duckdb_build_ok;"

# DONT_LINK deliberately leaves the bundled CLI without ROOT functions.
# Explicitly LOAD the built extension and require all public functions.
ROOT_EXTENSION_SQL="$(printf '%s' "$LOADABLE_EXT" | sed "s/'/''/g")"
registered="$(
    "$DUCKDB_BIN" -unsigned -bail -cmd "LOAD '$ROOT_EXTENSION_SQL';" \
        -csv -noheader :memory: -c "
            SELECT count(DISTINCT function_name)
            FROM duckdb_functions()
            WHERE function_name IN (
                'read_root',
                'root_build_index',
                'root_build_dataset_index',
                'read_root_dataset',
                'root_dataset_stats',
                'root_iceberg_catalog'
            );"
)"
if [[ "$registered" != "6" ]]; then
    echo "[ERROR] ROOT loadable extension registered $registered/6 public functions" >&2
    exit 1
fi
echo "[OK] ROOT extension: 6/6 public functions registered"

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
