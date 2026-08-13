#!/usr/bin/env bash
set -euo pipefail

PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${ROOT4DUCKDB_BUILD_DIR:-$PROJECT_DIR/build/release-package}"
OUTPUT_DIR="${ROOT4DUCKDB_RELEASE_DIR:-$PROJECT_DIR/dist}"

EXTENSION="$BUILD_DIR/extension/root/root.duckdb_extension"
LOADABLE_SMOKE="$BUILD_DIR/extension/root/root_loadable_extension_smoke"
PLATFORM_FILE="$BUILD_DIR/duckdb_platform_out"
EXPECTED_DUCKDB_VERSION="${DUCKDB_VERSION:-v1.4.5}"

fail() {
    echo "[ERROR] $*" >&2
    exit 1
}

for tool in python3 readelf nm ldd sha256sum tar; do
    command -v "$tool" >/dev/null 2>&1 ||
        fail "Required tool not found: $tool"
done

[[ -f "$EXTENSION" ]] ||
    fail "Extension not found: $EXTENSION"

[[ -x "$LOADABLE_SMOKE" ]] ||
    fail "Loadable-extension smoke test not found: $LOADABLE_SMOKE"

[[ -s "$PLATFORM_FILE" ]] ||
    fail "DuckDB platform file not found: $PLATFORM_FILE"

command -v root-config >/dev/null 2>&1 ||
    fail "root-config is unavailable"

[[ "$(uname -s)" == "Linux" &&
   "$(uname -m)" == "x86_64" ]] || {
    fail "The current release profile supports Linux x86_64 only"
}

PROJECT_VERSION="$(
    tr -d '[:space:]' < "$PROJECT_DIR/VERSION"
)"

[[ "$PROJECT_VERSION" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]] ||
    fail "Invalid VERSION: $PROJECT_VERSION"

EXPECTED_EXTENSION_VERSION="v$PROJECT_VERSION"
EXPECTED_PLATFORM="$(tr -d '\r\n' < "$PLATFORM_FILE")"

metadata="$({
    python3 - "$EXTENSION" <<'PY'
from pathlib import Path
import sys

path = Path(sys.argv[1])
data = path.read_bytes()

if len(data) < 512:
    raise SystemExit(
        "extension is smaller than the DuckDB metadata footer"
    )

footer = data[-512:]
fields = [
    footer[offset:offset + 32]
    .rstrip(b"\0")
    .decode("ascii")
    for offset in range(0, 256, 32)
]
fields.reverse()

magic, platform, duckdb_version, extension_version, abi_type = fields[:5]

if magic != "4":
    raise SystemExit("invalid DuckDB extension metadata magic")

print(
    "\t".join(
        (
            platform,
            duckdb_version,
            extension_version,
            abi_type,
        )
    )
)
PY
} 2>&1)" ||
    fail "Cannot parse extension metadata: $metadata"

IFS=$'\t' read -r \
    platform \
    duckdb_version \
    extension_version \
    abi_type <<<"$metadata"

[[ "$platform" == "$EXPECTED_PLATFORM" ]] || {
    fail "Platform metadata mismatch: expected $EXPECTED_PLATFORM, found $platform"
}

[[ "$duckdb_version" == "$EXPECTED_DUCKDB_VERSION" ]] || {
    fail "DuckDB metadata mismatch: expected $EXPECTED_DUCKDB_VERSION, found $duckdb_version"
}

[[ "$extension_version" == "$EXPECTED_EXTENSION_VERSION" ]] || {
    fail "Extension metadata mismatch: expected $EXPECTED_EXTENSION_VERSION, found $extension_version"
}

[[ "$abi_type" == "CPP" ]] ||
    fail "Unexpected extension ABI: $abi_type"

dynamic_section="$(readelf -d "$EXTENSION")"

if grep -Eq \
    'libiceberg(_data|_bundle|_sql_catalog)?\.so' \
    <<<"$dynamic_section"; then
    fail "Release extension still depends on shared Iceberg libraries"
fi

if grep -Eq '\((RPATH|RUNPATH)\)' <<<"$dynamic_section"; then
    fail "Release extension contains RPATH or RUNPATH"
fi

mapfile -t exports < <(
    nm -D --defined-only --format=posix "$EXTENSION" |
        awk '{sub(/@.*/, "", $1); print $1}' |
        sort -u
)

if ((${#exports[@]} != 1)) ||
   [[ "${exports[0]:-}" != "root_duckdb_cpp_init" ]]; then
    printf '[ERROR] Unexpected exported symbols:\n' >&2
    printf '        %s\n' "${exports[@]:-<none>}" >&2
    exit 1
fi

dependencies="$(ldd "$EXTENSION")"
missing="$(sed -n '/not found/p' <<<"$dependencies")"

[[ -z "$missing" ]] || {
    echo "[ERROR] Release extension has unresolved runtime dependencies:" >&2
    printf '%s\n' "$missing" >&2
    exit 1
}

smoke_dependencies="$(ldd "$LOADABLE_SMOKE")"

if grep -Eq \
    'libiceberg|libCore\.so' \
    <<<"$smoke_dependencies"; then
    fail "Loadable-extension smoke host is not isolated from ROOT4DuckDB dependencies"
fi

"$LOADABLE_SMOKE" "$EXTENSION"

ROOT_VERSION="$(root-config --version)"

COMPILER="$(
    sed -n \
        's/^CMAKE_CXX_COMPILER:[^=]*=//p' \
        "$BUILD_DIR/CMakeCache.txt" |
        head -1
)"

[[ -x "$COMPILER" ]] ||
    fail "Cannot resolve the release compiler from CMakeCache.txt"

COMPILER_VERSION="$("$COMPILER" --version | sed -n '1p')"

if git -C "$PROJECT_DIR" \
    rev-parse --is-inside-work-tree >/dev/null 2>&1; then
    source_changes="$(
        git -C "$PROJECT_DIR" \
            status --porcelain --untracked-files=normal
    )"

    [[ -z "$source_changes" ]] ||
        fail "Tracked release sources contain uncommitted changes"

    SOURCE_COMMIT="$(
        git -C "$PROJECT_DIR" rev-parse HEAD
    )"
else
    SOURCE_COMMIT="source-archive"
fi

safe_platform="$(
    tr -c 'A-Za-z0-9._-' '_' <<<"$platform" |
        sed 's/_$//'
)"

safe_root="$(
    tr -c 'A-Za-z0-9._-' '_' <<<"$ROOT_VERSION" |
        sed 's/_$//'
)"

bundle_name="root4duckdb-${EXPECTED_EXTENSION_VERSION}"
bundle_name+="-duckdb-${duckdb_version}"
bundle_name+="-${safe_platform}"
bundle_name+="-root-${safe_root}"

stage_root="$(
    mktemp -d "${TMPDIR:-/tmp}/root4duckdb-release.XXXXXX"
)"
stage="$stage_root/$bundle_name"

mkdir -p "$stage" "$OUTPUT_DIR"
trap 'rm -rf "$stage_root"' EXIT

cp "$EXTENSION" "$stage/root.duckdb_extension"
cp "$PROJECT_DIR/LICENSE" "$stage/LICENSE"

iceberg_notice="$PROJECT_DIR/.deps/iceberg-cpp/share/doc/iceberg/NOTICE"
iceberg_license="$PROJECT_DIR/.deps/iceberg-cpp/share/doc/iceberg/LICENSE"

[[ -f "$iceberg_notice" ]] ||
    fail "Apache Iceberg NOTICE is missing: $iceberg_notice"

[[ -f "$iceberg_license" ]] ||
    fail "Apache Iceberg LICENSE is missing: $iceberg_license"

mkdir -p "$stage/licenses/apache-iceberg"
cp "$iceberg_notice" "$stage/licenses/apache-iceberg/NOTICE"
cp "$iceberg_license" "$stage/licenses/apache-iceberg/LICENSE"

third_party_sources="$PROJECT_DIR/.deps/iceberg-cpp-build/_deps"

if [[ -d "$third_party_sources" ]]; then
    while IFS= read -r -d '' notice; do
        relative="${notice#"$third_party_sources"/}"
        destination="$stage/licenses/third-party/$relative"

        mkdir -p "$(dirname "$destination")"
        cp "$notice" "$destination"
    done < <(
        find "$third_party_sources" \
            -maxdepth 4 \
            -type f \
            \( \
                -iname 'LICENSE' -o \
                -iname 'LICENSE.*' -o \
                -iname 'NOTICE' -o \
                -iname 'NOTICE.*' -o \
                -iname 'COPYING' -o \
                -iname 'COPYING.*' \
            \) \
            -print0
    )
fi

{
    printf 'ROOT4DUCKDB_VERSION=%s\n' \
        "$EXPECTED_EXTENSION_VERSION"
    printf 'DUCKDB_VERSION=%s\n' \
        "$duckdb_version"
    printf 'DUCKDB_PLATFORM=%s\n' \
        "$platform"
    printf 'DUCKDB_EXTENSION_ABI=%s\n' \
        "$abi_type"
    printf 'ROOT_VERSION=%s\n' \
        "$ROOT_VERSION"
    printf 'ICEBERG_VERSION=v0.3.0\n'
    printf 'ICEBERG_LINKAGE=static\n'
    printf 'SOURCE_COMMIT=%s\n' \
        "$SOURCE_COMMIT"
    printf 'COMPILER=%s\n' \
        "$COMPILER"
    printf 'COMPILER_VERSION=%s\n' \
        "$COMPILER_VERSION"

    printf '\nDYNAMIC_DEPENDENCIES\n%s\n' \
        "$dependencies"

    printf '\nREQUIRED_SYMBOL_VERSIONS\n'
    readelf --version-info "$EXTENSION" |
        sed -n '/Version needs section/,$p'
} > "$stage/BUILD-INFO.txt"

(
    cd "$stage"

    find . \
        -type f \
        ! -name SHA256SUMS \
        -print0 |
        sort -z |
        xargs -0 sha256sum > SHA256SUMS
)

raw_asset="$OUTPUT_DIR/root.duckdb_extension"
bundle_asset="$OUTPUT_DIR/$bundle_name.tar.gz"

cp "$stage/root.duckdb_extension" "$raw_asset"
tar -C "$stage_root" -czf "$bundle_asset" "$bundle_name"

(
    cd "$OUTPUT_DIR"

    sha256sum \
        "$(basename "$raw_asset")" \
        > root.duckdb_extension.sha256

    sha256sum \
        "$(basename "$bundle_asset")" \
        > "$bundle_name.tar.gz.sha256"
)

echo "[OK] Publishable ROOT4DuckDB assets created"
echo "Extension: $raw_asset"
echo "Bundle:    $bundle_asset"
echo "Platform:  $platform"
echo "ROOT ABI:  $ROOT_VERSION"
