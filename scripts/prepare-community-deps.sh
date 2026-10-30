#!/usr/bin/env bash
set -euo pipefail

project_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
deps_dir="$project_dir/.deps"
root_prefix="$deps_dir/community-root"
mamba_bin="$deps_dir/micromamba/bin/micromamba"
export MAMBA_ROOT_PREFIX="$deps_dir/mamba-root"

[[ "$(uname -s)" == Linux && "$(uname -m)" == x86_64 ]] || { echo "Community build supports Linux x86_64 only" >&2; exit 2; }
# The DuckDB manylinux CI image ships a working HTTPS curl. Keep it
# isolated from conda's libcurl: vcpkg invokes curl after LD_LIBRARY_PATH
# has been extended with the conda environment, which otherwise breaks
# HTTPS downloads with curl exit code 4.
system_curl=/usr/bin/curl
community_tools="$deps_dir/community-tools"
if [[ ! -x "$system_curl" ]]; then
    echo "[ERROR] Community build needs $system_curl for vcpkg downloads" >&2
    exit 1
fi
mkdir -p "$deps_dir/micromamba" "$community_tools"
cat > "$community_tools/curl" <<'EOF'
#!/bin/sh
exec /usr/bin/env -u LD_LIBRARY_PATH /usr/bin/curl "$@"
EOF
chmod 755 "$community_tools/curl"
if ! /usr/bin/env -u LD_LIBRARY_PATH "$system_curl" --version |
        grep -Eq '^Protocols:.*(^|[[:space:]])https([[:space:]]|$)'; then
    echo "[ERROR] System curl cannot handle HTTPS downloads" >&2
    exit 1
fi

if [[ ! -x "$mamba_bin" ]]; then
    curl -fsSL https://micro.mamba.pm/api/micromamba/linux-64/latest | tar -xj -C "$deps_dir/micromamba" bin/micromamba
fi

if [[ ! -x "$root_prefix/bin/root-config" || ! -x "$root_prefix/bin/x86_64-conda-linux-gnu-g++" ]]; then
    "$mamba_bin" create -y -p "$root_prefix" -c conda-forge --strict-channel-priority \
        'root=6.40.04' 'gcc_linux-64=14' 'gxx_linux-64=14' 'sysroot_linux-64=2.28' 'cmake>=3.28' ninja nlohmann_json sqlite pkg-config
elif ! "$mamba_bin" list -p "$root_prefix" | grep -Eq '^sysroot_linux-64[[:space:]]+2\.28([[:space:]]|$)'; then
    "$mamba_bin" install -y -p "$root_prefix" -c conda-forge --strict-channel-priority 'sysroot_linux-64=2.28'
fi

export ROOT4DUCKDB_DISABLE_PINNED_LCG=1
export ROOT4DUCKDB_ROOT_CONFIG="$root_prefix/bin/root-config"
export CC="$root_prefix/bin/x86_64-conda-linux-gnu-gcc"
export CXX="$root_prefix/bin/x86_64-conda-linux-gnu-g++"
export PATH="$community_tools:$root_prefix/bin:$PATH"
if [[ "$(command -v curl)" != "$community_tools/curl" ]]; then
    echo "[ERROR] vcpkg would not use the isolated HTTPS curl" >&2
    exit 1
fi
export CMAKE_PREFIX_PATH="$root_prefix${CMAKE_PREFIX_PATH:+:$CMAKE_PREFIX_PATH}"
export LD_LIBRARY_PATH="$root_prefix/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"

if ! printf '#include <string.h>\nint main(void) { char key[1] = {0}; explicit_bzero(key, sizeof(key)); return 0; }\n' |
        "$CC" -std=gnu11 -Werror=implicit-function-declaration -x c - -o /dev/null; then
    echo "[ERROR] DuckDB 2.0 requires a glibc sysroot providing explicit_bzero" >&2
    exit 1
fi

# Fail on bootstrap/sysroot C API incompatibilities before the expensive Iceberg build.
if [[ -f "$project_dir/duckdb/src/include/duckdb.h" ]]; then
    echo "[PRECHECK] Compile ROOT bootstrap against the Community toolchain"
    "$CC" -std=gnu11 -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion \
        -fsyntax-only \
        -I"$project_dir/duckdb/src/include" \
        -I"$project_dir/include" \
        "$project_dir/src/core/root_bootstrap.c"
fi

"$project_dir/build-iceberg.sh" --jobs "${ROOT4DUCKDB_COMMUNITY_JOBS:-2}"
