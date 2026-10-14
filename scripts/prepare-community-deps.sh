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
        'root=6.40.04' 'gcc_linux-64=14' 'gxx_linux-64=14' 'cmake>=3.28' ninja nlohmann_json sqlite pkg-config
fi

export ROOT4DUCKDB_DISABLE_PINNED_LCG=1
export ROOT4DUCKDB_ROOT_CONFIG="$root_prefix/bin/root-config"
export CC="$root_prefix/bin/x86_64-conda-linux-gnu-gcc"
export CXX="$root_prefix/bin/x86_64-conda-linux-gnu-g++"
export PATH="$root_prefix/bin:$PATH"
export CMAKE_PREFIX_PATH="$root_prefix${CMAKE_PREFIX_PATH:+:$CMAKE_PREFIX_PATH}"
export LD_LIBRARY_PATH="$root_prefix/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"

"$project_dir/build-iceberg.sh" --jobs "${ROOT4DUCKDB_COMMUNITY_JOBS:-2}"
