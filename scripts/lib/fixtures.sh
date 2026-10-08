#!/usr/bin/env bash

build_root_fixture() {
    local fixture_dir="$1" build_dir="$2" name="$3" maker="$4" compiler="$5" root_config="${6:-root-config}"
    local -a root_cflags root_libs
    read -r -a root_cflags <<< "$("$root_config" --cflags)"
    read -r -a root_libs <<< "$("$root_config" --libs)"
    rootcling -f "$build_dir/${name}Dict.cpp" -c -I"$fixture_dir" \
        "$fixture_dir/$name.hpp" "$fixture_dir/${name}LinkDef.h"
    "$compiler" -std=c++17 -O2 -fPIC -shared "${root_cflags[@]}" -I"$fixture_dir" \
        "$fixture_dir/$name.cpp" "$build_dir/${name}Dict.cpp" \
        -o "$build_dir/lib$name.so" "${root_libs[@]}"
    "$compiler" -std=c++17 -O2 "${root_cflags[@]}" -I"$fixture_dir" \
        "$fixture_dir/$maker.cpp" -L"$build_dir" "-l$name" -Wl,-rpath,"$build_dir" \
        -o "$build_dir/$maker" "${root_libs[@]}"
}
