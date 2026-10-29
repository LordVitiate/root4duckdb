#!/usr/bin/env bash
set -euo pipefail

PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TEST_TMP="$(mktemp -d)"
trap 'rm -rf -- "$TEST_TMP"' EXIT

"${CXX:-c++}" -std=c++17 -O2 -Wall -Wextra -Werror \
    -I"$PROJECT_DIR/include" \
    "$PROJECT_DIR/src/reader/root_primitive_value.cpp" \
    "$PROJECT_DIR/src/serialized/root_serialized_codec_utils.cpp" \
    "$PROJECT_DIR/src/serialized/root_serialized_codec.cpp" \
    "$PROJECT_DIR/src/serialized/root_serialized_nested_codec.cpp" \
    "$PROJECT_DIR/test/serialized_codec_test.cpp" \
    -o "$TEST_TMP/serialized_codec_test"

"$TEST_TMP/serialized_codec_test"

# Opt-in runtime CERT/UB checks; leave ordinary project builds unchanged.
if [[ "${ROOT4DUCKDB_CODEC_UBSAN:-0}" == "1" ]]; then
    "${CXX:-c++}" -std=c++17 -O1 -g -Wall -Wextra -Werror \
        -fsanitize=undefined,float-cast-overflow -fno-sanitize-recover=all \
        -I"$PROJECT_DIR/include" \
        "$PROJECT_DIR/src/reader/root_primitive_value.cpp" \
        "$PROJECT_DIR/src/serialized/root_serialized_codec_utils.cpp" \
        "$PROJECT_DIR/src/serialized/root_serialized_codec.cpp" \
        "$PROJECT_DIR/src/serialized/root_serialized_nested_codec.cpp" \
        "$PROJECT_DIR/test/serialized_codec_test.cpp" \
        -o "$TEST_TMP/serialized_codec_sanitized"
    UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 "$TEST_TMP/serialized_codec_sanitized"
fi
