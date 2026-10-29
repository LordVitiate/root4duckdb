#!/usr/bin/env bash
set -euo pipefail

# Emits shell exports to stdout. Diagnostics go to stderr, so callers may use:
#   eval "$(./scripts/detect-build-environment.sh)"
# or:
#   ./scripts/detect-build-environment.sh --write .deps/build-env.sh

OUTPUT_FILE=""
RUNTIME_BASE="${ROOT4DUCKDB_RUNTIME_BASE:-/tmp/root4duckdb-$(id -u)}"
case "$RUNTIME_BASE" in
  /afs/*) echo "[FAIL] ROOT4DUCKDB_RUNTIME_BASE must not use AFS" >&2; exit 2 ;;
esac
TMP_ROOT="$RUNTIME_BASE/tmp"
CACHE_ROOT="$RUNTIME_BASE/cache"
CCACHE_ROOT="$CACHE_ROOT/ccache"
CCACHE_TMP="$TMP_ROOT/ccache"
mkdir -p "$TMP_ROOT" "$CACHE_ROOT" "$CCACHE_ROOT" "$CCACHE_TMP"
case "${1:-}" in
  --write)
    [[ $# -eq 2 ]] || { echo "[FAIL] --write requires a file path" >&2; exit 2; }
    OUTPUT_FILE="$2"
    ;;
  "") ;;
  *) echo "[FAIL] unknown argument: $1" >&2; exit 2 ;;
esac

# Never let a failed detection leave a stale environment file that can
# accidentally be sourced as if it belonged to the current invocation.
if [[ -n "$OUTPUT_FILE" ]]; then
  rm -f "$OUTPUT_FILE"
fi

quote() { printf '%q' "$1"; }

# ROOT4DUCKDB_PINNED_LCG_BEGIN
#
# On CERN hosts prefer one tested LCG view as a coherent compiler/ROOT/runtime
# stack. This avoids combining a system ROOT with an unrelated CVMFS libstdc++.
if [[ "${ROOT4DUCKDB_DISABLE_PINNED_LCG:-0}" != "1" ]]; then
  pinned_arch="$(uname -m 2>/dev/null || true)"
  pinned_os_major=""
  if [[ -r /etc/os-release ]]; then
    pinned_os_major="$(. /etc/os-release; printf '%s' "${VERSION_ID%%.*}")"
  fi

  pinned_default_view=""
  if [[ -n "$pinned_arch" && -n "$pinned_os_major" ]]; then
    pinned_default_view="/cvmfs/sft.cern.ch/lcg/views/LCG_110a/${pinned_arch}-el${pinned_os_major}-gcc14-opt"
  fi

  pinned_view="${ROOT4DUCKDB_LCG_VIEW:-$pinned_default_view}"
  pinned_root_version="${ROOT4DUCKDB_ROOT_VERSION:-6.40.04}"

  if [[ -n "$pinned_view" && -d "$pinned_view" ]]; then
    case "$pinned_view" in
      /afs/*)
        echo "[FAIL] ROOT4DUCKDB_LCG_VIEW must not use AFS: $pinned_view" >&2
        exit 2
        ;;
    esac

    pinned_root_config="$pinned_view/bin/root-config"
    pinned_setup="$pinned_view/setup.sh"

    [[ -x "$pinned_root_config" ]] || {
      echo "[FAIL] pinned LCG view has no root-config: $pinned_view" >&2
      exit 2
    }

    [[ -f "$pinned_setup" ]] || {
      echo "[FAIL] pinned LCG view has no setup.sh: $pinned_view" >&2
      exit 2
    }

    # Resolve the compiler selected by this exact LCG view without importing
    # the complete view environment into the current shell.
    pinned_cxx="$(
      env -i \
        HOME=/tmp \
        PATH=/usr/bin:/bin \
        bash --noprofile --norc -c '
          source "$1" >/dev/null 2>&1
          command -v g++
        ' _ "$pinned_setup"
    )"

    pinned_cc="$(
      env -i \
        HOME=/tmp \
        PATH=/usr/bin:/bin \
        bash --noprofile --norc -c '
          source "$1" >/dev/null 2>&1
          command -v gcc
        ' _ "$pinned_setup"
    )"

    [[ -x "$pinned_cxx" && -x "$pinned_cc" ]] || {
      echo "[FAIL] could not resolve GCC from pinned LCG view: $pinned_view" >&2
      echo "       CC=$pinned_cc" >&2
      echo "       CXX=$pinned_cxx" >&2
      exit 2
    }

    pinned_cxx="$(readlink -f "$pinned_cxx")"
    pinned_cc="$(readlink -f "$pinned_cc")"

    actual_root_version="$("$pinned_root_config" --version)"
    if [[ "$actual_root_version" != "$pinned_root_version" ]]; then
      echo "[FAIL] pinned ROOT version mismatch" >&2
      echo "       expected: $pinned_root_version" >&2
      echo "       actual:   $actual_root_version" >&2
      echo "       view:     $pinned_view" >&2
      exit 2
    fi

    pinned_root_prefix="$("$pinned_root_config" --prefix)"
    pinned_root_libdir="$("$pinned_root_config" --libdir)"

    pinned_libstdcpp="$("$pinned_cxx" -print-file-name=libstdc++.so.6)"
    [[ -f "$pinned_libstdcpp" ]] || {
      echo "[FAIL] GCC runtime libstdc++.so.6 was not found" >&2
      exit 2
    }

    pinned_libstdcpp="$(readlink -f "$pinned_libstdcpp")"
    pinned_runtime_dir="$(dirname "$pinned_libstdcpp")"
    pinned_compiler_bin="$(cd "$(dirname "$pinned_cxx")" && pwd -P)"

    runtime_base="${ROOT4DUCKDB_RUNTIME_BASE:-/tmp/root4duckdb-$(id -u)}"
    case "$runtime_base" in
      /afs/*)
        echo "[FAIL] ROOT4DUCKDB_RUNTIME_BASE must not use AFS" >&2
        exit 2
        ;;
    esac

    tmp_root="$runtime_base/tmp"
    cache_root="$runtime_base/cache"
    ccache_root="$cache_root/ccache"
    ccache_tmp="$tmp_root/ccache"

    mkdir -p "$tmp_root" "$cache_root" "$ccache_root" "$ccache_tmp"

    pinned_ld_dirs=()
    declare -A pinned_ld_seen=()

    for dir in \
      "$pinned_runtime_dir" \
      "$pinned_root_libdir" \
      "$pinned_view/lib" \
      "$pinned_view/lib64"; do

      [[ -d "$dir" ]] || continue
      case "$dir" in
        /afs/*)
          echo "[FAIL] AFS path entered pinned runtime: $dir" >&2
          exit 2
          ;;
      esac

      if [[ -z "${pinned_ld_seen[$dir]:-}" ]]; then
        pinned_ld_seen[$dir]=1
        pinned_ld_dirs+=("$dir")
      fi
    done

    pinned_ld_library_path="$(IFS=:; echo "${pinned_ld_dirs[*]}")"

    pinned_path="$pinned_view/bin:$pinned_compiler_bin:/usr/local/bin:/usr/bin:/bin"

    pinned_pkgconfig=()
    for dir in \
      "$pinned_view/lib/pkgconfig" \
      "$pinned_view/lib64/pkgconfig" \
      "$pinned_view/share/pkgconfig"; do
      [[ -d "$dir" ]] && pinned_pkgconfig+=("$dir")
    done
    pinned_pkgconfig_path="$(IFS=:; echo "${pinned_pkgconfig[*]}")"

    pinned_git="/usr/bin/git"
    [[ -x "$pinned_git" ]] || {
      echo "[FAIL] /usr/bin/git is unavailable" >&2
      exit 2
    }
    pinned_git_exec="$("$pinned_git" --exec-path)"

    cxx_line="$("$pinned_cxx" --version 2>/dev/null | head -n 1)"

    echo "[OK] pinned LCG view: $pinned_view" >&2
    echo "[OK] pinned ROOT: $actual_root_version ($pinned_root_config)" >&2
    echo "[OK] pinned CXX: $pinned_cxx ($cxx_line)" >&2
    echo "[OK] pinned libstdc++: $pinned_libstdcpp" >&2

    {
      printf 'export ROOT4DUCKDB_LCG_VIEW=%s\n' "$(quote "$pinned_view")"
      printf 'export ROOT4DUCKDB_ROOT_PREFIX=%s\n' "$(quote "$pinned_view")"
      printf 'export ROOT4DUCKDB_ROOT_CONFIG=%s\n' "$(quote "$pinned_root_config")"
      printf 'export ROOT4DUCKDB_ROOT_VERSION=%s\n' "$(quote "$pinned_root_version")"
      printf 'export ROOTSYS=%s\n' "$(quote "$pinned_root_prefix")"

      printf 'export CC=%s\n' "$(quote "$pinned_cc")"
      printf 'export CXX=%s\n' "$(quote "$pinned_cxx")"

      printf 'unset PYTHONHOME PYTHONPATH PYTHONPLATLIBDIR\n'
      printf 'export PATH=%s\n' "$(quote "$pinned_path")"
      printf 'export LD_LIBRARY_PATH=%s\n' "$(quote "$pinned_ld_library_path")"

      printf 'export CMAKE_PREFIX_PATH=%s\n' "$(quote "$pinned_view")"
      printf 'export PKG_CONFIG_PATH=%s\n' "$(quote "$pinned_pkgconfig_path")"

      printf 'unset LIBRARY_PATH CPATH CPLUS_INCLUDE_PATH\n'

      printf 'export TMPDIR=%s\n' "$(quote "$tmp_root")"
      printf 'export XDG_CACHE_HOME=%s\n' "$(quote "$cache_root")"
      printf 'export CCACHE_DIR=%s\n' "$(quote "$ccache_root")"
      printf 'export CCACHE_TEMPDIR=%s\n' "$(quote "$ccache_tmp")"
      printf 'mkdir -p -- "$TMPDIR" "$XDG_CACHE_HOME" "$CCACHE_DIR" "$CCACHE_TEMPDIR"\n'

      printf 'export GIT_BIN=%s\n' "$(quote "$pinned_git")"
      printf 'export GIT_EXEC_PATH=%s\n' "$(quote "$pinned_git_exec")"
    } > "${OUTPUT_FILE:-/dev/stdout}"

    if [[ -n "$OUTPUT_FILE" ]]; then
      chmod 0644 "$OUTPUT_FILE"
      echo "[OK] build environment written to $OUTPUT_FILE" >&2
    fi

    exit 0
  fi
fi
# ROOT4DUCKDB_PINNED_LCG_END


compiler_family() {
  local cxx="$1" text
  text="$($cxx --version 2>/dev/null | head -n 1 || true)"
  if grep -qi clang <<<"$text"; then
    printf 'clang\n'
  else
    printf 'gcc\n'
  fi
}

compiler_major() {
  local cxx="$1" family="$2" value
  if [[ "$family" == clang ]]; then
    value="$($cxx --version 2>/dev/null | sed -nE '1s/.*version[[:space:]]+([0-9]+).*/\1/p')"
  else
    value="$($cxx -dumpfullversion -dumpversion 2>/dev/null | cut -d. -f1)"
  fi
  printf '%s\n' "${value:-0}"
}

compiler_compatible() {
  local cxx="$1"
  [[ -x "$cxx" ]] || return 1
  local family major minimum
  family="$(compiler_family "$cxx")"
  major="$(compiler_major "$cxx" "$family")"
  minimum=14
  [[ "$family" == clang ]] && minimum=18
  [[ "$major" =~ ^[0-9]+$ ]] && (( major >= minimum ))
}

HOST_ARCH="$(uname -m 2>/dev/null || true)"
HOST_OS_MAJOR=""
if [[ -r /etc/os-release ]]; then
  HOST_OS_MAJOR="$(. /etc/os-release; printf '%s' "${VERSION_ID%%.*}")"
fi
HOST_PLATFORM=""
[[ -n "$HOST_ARCH" && -n "$HOST_OS_MAJOR" ]] && HOST_PLATFORM="${HOST_ARCH}-el${HOST_OS_MAJOR}"

candidate_score() {
  local cxx="$1" family major minor version score
  family="$(compiler_family "$cxx")"
  if [[ "$family" == clang ]]; then
    version="$($cxx --version 2>/dev/null | sed -nE '1s/.*version[[:space:]]+([0-9]+)(\.([0-9]+))?.*/\1 \3/p')"
  else
    version="$($cxx -dumpfullversion -dumpversion 2>/dev/null | awk -F. '{print $1, ($2==""?0:$2)}')"
  fi
  read -r major minor <<<"${version:-0 0}"
  major="${major:-0}"; minor="${minor:-0}"
  # Prefer GCC for ABI compatibility with common ROOT builds. For automatic
  # selection, prefer the oldest supported major line rather than the newest:
  # Iceberg C++ v0.3.0 is built and tested from GCC 14 upward, while future GCC
  # versions may introduce fresh optimizer warnings in third-party code. An
  # explicitly supplied CXX remains authoritative.
  if [[ "$family" == gcc ]]; then
    score=$(( 100000 - (major - 14) * 1000 + minor ))
  else
    score=$(( 50000 - (major - 18) * 1000 + minor ))
  fi
  # Prefer a toolchain built for the current host platform. This avoids choosing
  # an old CentOS 7 compiler over x86_64-el9 merely because both are GCC 14.3.
  if [[ -n "$HOST_PLATFORM" && "$cxx" == *"/$HOST_PLATFORM/"* ]]; then
    score=$(( score + 50000 ))
  elif [[ -n "$HOST_ARCH" && "$cxx" == *"/$HOST_ARCH-"* ]]; then
    score=$(( score + 1000 ))
  fi
  [[ "$cxx" == /usr/bin/* ]] && score=$(( score + 20000 ))
  printf '%s\n' "$score"
}

add_candidate() {
  local value="${1:-}"
  [[ -n "$value" ]] || return 0
  if command -v "$value" >/dev/null 2>&1; then
    value="$(command -v "$value")"
  fi
  [[ -x "$value" ]] || return 0
  CANDIDATES+=("$value")
}

CANDIDATES=()
add_candidate "${CXX:-}"
for name in g++-15 g++15 g++-14 g++14 g++ clang++-20 clang++20 clang++-19 clang++19 clang++-18 clang++18 clang++; do
  add_candidate "$name"
done

shopt -s nullglob
for path in \
  /cvmfs/sft.cern.ch/lcg/releases/gcc/*/*/bin/g++ \
  /cvmfs/sft.cern.ch/lcg/releases/clang/*/*/bin/clang++ \
  /opt/rh/gcc-toolset-*/root/usr/bin/g++ \
  /opt/rh/llvm-toolset-*/root/usr/bin/clang++; do
  add_candidate "$path"
done
shopt -u nullglob

BEST_CXX=""
BEST_SCORE=-1
# An explicitly supplied compatible CXX is authoritative. This matters for ABI
# compatibility with an already selected ROOT/PHAST toolchain.
if [[ -n "${CXX:-}" ]]; then
  explicit_cxx="$CXX"
  if command -v "$explicit_cxx" >/dev/null 2>&1; then
    explicit_cxx="$(command -v "$explicit_cxx")"
  fi
  if compiler_compatible "$explicit_cxx"; then
    BEST_CXX="$explicit_cxx"
  fi
fi

if [[ -z "$BEST_CXX" ]]; then
  # Deduplicate by resolved path without requiring readlink -f support on every platform.
  declare -A SEEN=()
  for candidate in "${CANDIDATES[@]}"; do
    key="$(cd "$(dirname "$candidate")" 2>/dev/null && pwd -P)/$(basename "$candidate")"
    [[ -n "${SEEN[$key]:-}" ]] && continue
    SEEN[$key]=1
    compiler_compatible "$candidate" || continue
    score="$(candidate_score "$candidate")"
    if (( score > BEST_SCORE )); then
      BEST_SCORE="$score"
      BEST_CXX="$candidate"
    fi
  done
fi

if [[ -z "$BEST_CXX" ]]; then
  cat >&2 <<'MSG'
[FAIL] No compatible C++23 compiler was found.
Apache Iceberg C++ v0.3.0 requires GCC 14+ or Clang 18+.
Set CXX explicitly, install a compatible compiler, or expose a CERN CVMFS toolchain.
MSG
  exit 2
fi

FAMILY="$(compiler_family "$BEST_CXX")"
BIN_DIR="$(cd "$(dirname "$BEST_CXX")" && pwd -P)"
TOOLCHAIN_ROOT="$(cd "$BIN_DIR/.." && pwd -P)"
CXX_BASE="$(basename "$BEST_CXX")"
if [[ "$FAMILY" == clang ]]; then
  CC_BASE="${CXX_BASE/clang++/clang}"
else
  CC_BASE="${CXX_BASE/g++/gcc}"
fi
BEST_CC="$BIN_DIR/$CC_BASE"
[[ -x "$BEST_CC" ]] || { echo "[FAIL] matching C compiler not found beside $BEST_CXX (expected $BEST_CC)" >&2; exit 2; }

LIB_PATHS=()
# Never prepend generic /usr/lib or /usr/lib64: that can override the host libc.
# A self-contained non-system toolchain (for example CERN CVMFS) needs its own
# libstdc++ runtime first, while distro compilers should use the system loader.
if [[ "$TOOLCHAIN_ROOT" != /usr && "$TOOLCHAIN_ROOT" != /usr/local ]]; then
  for dir in "$TOOLCHAIN_ROOT/lib64" "$TOOLCHAIN_ROOT/lib"; do
    [[ -d "$dir" ]] && LIB_PATHS+=("$dir")
  done
fi
TOOLCHAIN_LD=""
if ((${#LIB_PATHS[@]})); then
  TOOLCHAIN_LD="$(IFS=:; echo "${LIB_PATHS[*]}")"
fi

# A compiler switch can expose a Conda/CVMFS Git with an incomplete CA setup or
# exec-path. Prefer the host Git when available; dependency downloads do not
# need to come from the selected compiler prefix.
REQUESTED_GIT_BIN="${GIT_BIN:-}"
GIT_BIN=""
for candidate in "$REQUESTED_GIT_BIN" /usr/bin/git "$(command -v git || true)"; do
  [[ -x "$candidate" ]] || continue
  GIT_BIN="$candidate"
  break
done
[[ -n "$GIT_BIN" ]] || { echo "[FAIL] Git executable not found" >&2; exit 2; }
GIT_EXEC_CANDIDATES=()
[[ -n "${GIT_EXEC_PATH:-}" ]] && GIT_EXEC_CANDIDATES+=("$GIT_EXEC_PATH")
current_exec="$($GIT_BIN --exec-path 2>/dev/null || true)"
[[ -n "$current_exec" ]] && GIT_EXEC_CANDIDATES+=("$current_exec")
GIT_EXEC_CANDIDATES+=(
  /usr/libexec/git-core
  /usr/lib/git-core
  /usr/local/libexec/git-core
  /usr/local/lib/git-core
)
shopt -s nullglob
for dir in \
  /cvmfs/sft.cern.ch/lcg/views/LCG_*/x86_64-el9-*-opt/libexec/git-core \
  /cvmfs/sft.cern.ch/lcg/views/LCG_*/x86_64-el9-*-opt/lib/git-core \
  /cvmfs/sft.cern.ch/lcg/releases/git/*/*/libexec/git-core \
  /cvmfs/sft.cern.ch/lcg/releases/git/*/*/lib/git-core; do
  GIT_EXEC_CANDIDATES+=("$dir")
done
shopt -u nullglob
GIT_EXEC_PATH_FIXED=""
for dir in "${GIT_EXEC_CANDIDATES[@]}"; do
  if [[ -x "$dir/git-remote-https" || -x "$dir/git-remote-http" ]]; then
    GIT_EXEC_PATH_FIXED="$dir"
    break
  fi
done
if [[ -z "$GIT_EXEC_PATH_FIXED" ]]; then
  helper="$(find /usr -type f -name git-remote-https -print -quit 2>/dev/null || true)"
  [[ -n "$helper" ]] && GIT_EXEC_PATH_FIXED="$(dirname "$helper")"
fi
[[ -n "$GIT_EXEC_PATH_FIXED" ]] || {
  cat >&2 <<'MSG'
[FAIL] git-remote-https helper not found.
On LXPLUS, source an LCG view that contains Git or export GIT_EXEC_PATH to its
git-core directory, then rerun the command. Example:
  source /cvmfs/sft.cern.ch/lcg/views/LCG_110a/x86_64-el9-gcc14-opt/setup.sh
MSG
  exit 2
}

CXX_LINE="$($BEST_CXX --version 2>/dev/null | head -n 1)"
echo "[OK] selected CXX: $BEST_CXX ($CXX_LINE)" >&2
echo "[OK] selected CC:  $BEST_CC" >&2
echo "[OK] selected Git: $GIT_BIN" >&2
echo "[OK] Git HTTPS helper: $GIT_EXEC_PATH_FIXED" >&2

{
  printf 'export CC=%s\n' "$(quote "$BEST_CC")"
  printf 'export CXX=%s\n' "$(quote "$BEST_CXX")"
  printf 'unset PYTHONHOME PYTHONPATH PYTHONPLATLIBDIR\n'
  printf 'export PATH=%s:${PATH:-}\n' "$(quote "$BIN_DIR")"
  printf 'export TMPDIR=%s\n' "$(quote "$TMP_ROOT")"
  printf 'export XDG_CACHE_HOME=%s\n' "$(quote "$CACHE_ROOT")"
  printf 'export CCACHE_DIR=%s\n' "$(quote "$CCACHE_ROOT")"
  printf 'export CCACHE_TEMPDIR=%s\n' "$(quote "$CCACHE_TMP")"
  printf 'mkdir -p -- "$TMPDIR" "$XDG_CACHE_HOME" "$CCACHE_DIR" "$CCACHE_TEMPDIR"\n'
  if [[ -n "$TOOLCHAIN_LD" ]]; then
    printf 'export LD_LIBRARY_PATH=%s:${LD_LIBRARY_PATH:-}\n' "$(quote "$TOOLCHAIN_LD")"
  fi
  printf 'export GIT_BIN=%s\n' "$(quote "$GIT_BIN")"
  printf 'export GIT_EXEC_PATH=%s\n' "$(quote "$GIT_EXEC_PATH_FIXED")"
} > "${OUTPUT_FILE:-/dev/stdout}"

if [[ -n "$OUTPUT_FILE" ]]; then
  chmod 0644 "$OUTPUT_FILE"
  echo "[OK] build environment written to $OUTPUT_FILE" >&2
fi
