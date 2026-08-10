#!/usr/bin/env bash
set -euo pipefail

PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TEST_TMP="$(mktemp -d)"
trap 'rm -rf -- "$TEST_TMP"' EXIT

"${CXX:-c++}" -std=c++17 -O2 -Wall -Wextra -Werror \
    -I"$PROJECT_DIR/src/include" \
    "$PROJECT_DIR/src/root_serialized_codec.cpp" \
    "$PROJECT_DIR/src/root_serialized_nested_codec.cpp" \
    "$PROJECT_DIR/test/serialized_codec_test.cpp" \
    -o "$TEST_TMP/serialized_codec_test"

"$TEST_TMP/serialized_codec_test"
