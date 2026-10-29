#!/usr/bin/env bash
# Run the repository's authoritative clang-format policy on tracked C++ files.
set -euo pipefail

if (( $# < 1 )) || [[ "$1" != --check && "$1" != --fix ]]; then
    echo 'Usage: scripts/cpp-style.sh --check|--fix [path ...]' >&2
    exit 2
fi
mode="$1"
shift
project_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)"
cd "$project_dir"
formatter="${CLANG_FORMAT:-clang-format}"
if ! command -v "$formatter" >/dev/null 2>&1; then
    echo "clang-format not found: $formatter" >&2
    exit 2
fi
files=()
if (( $# > 0 )); then
    for path in "$@"; do
        case "$path" in
            src/*.cpp|src/*.hpp|src/*.tpp|include/*.hpp|include/*.tpp|test/*.cpp|test/*.hpp|examples/*.cpp|examples/*.hpp)
                [[ -f "$path" ]] || { echo "file not found: $path" >&2; exit 2; }
                files+=("$path")
                ;;
            *) echo "not an eligible project C++ file: $path" >&2; exit 2 ;;
        esac
    done
else
    while IFS= read -r -d '' path; do
        case "$path" in
            src/*.cpp|src/*.hpp|src/*.tpp|include/*.hpp|include/*.tpp|test/*.cpp|test/*.hpp|examples/*.cpp|examples/*.hpp)
                [[ -f "$path" ]] && files+=("$path") ;;
        esac
    done < <(git ls-files -z -- '*.cpp' '*.hpp' '*.tpp')
fi
if (( ${#files[@]} == 0 )); then
    echo 'no project-owned C++ files selected' >&2
    exit 2
fi
if [[ "$mode" == --check ]]; then
    "$formatter" --dry-run --Werror -- "${files[@]}"
else
    "$formatter" -i -- "${files[@]}"
fi
printf '[OK] %s: %d C++ file(s)\n' "$mode" "${#files[@]}"
