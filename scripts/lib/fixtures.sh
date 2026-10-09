#!/usr/bin/env bash

build_root_fixture() {
    local fixture_dir="$1" build_dir="$2" name="$3" maker="$4" compiler="$5" root_config="${6:-root-config}"
    local -a root_cflags root_libs
    local root_bindir root_libdir rootcling_bin
    root_bindir="$("$root_config" --bindir)"
    root_libdir="$("$root_config" --libdir)"
    rootcling_bin="$root_bindir/rootcling"
    if [[ ! -x "$rootcling_bin" || ! -f "$root_libdir/libCore.so" ]]; then
        echo "incomplete ROOT installation: $root_bindir / $root_libdir" >&2
        return 2
    fi
    read -r -a root_cflags <<< "$("$root_config" --cflags)"
    read -r -a root_libs <<< "$("$root_config" --libs)"
    "$rootcling_bin" -f "$build_dir/${name}Dict.cpp" -s "$build_dir/lib$name.so" \
        -c -I"$fixture_dir" "$fixture_dir/$name.hpp" "$fixture_dir/${name}LinkDef.h"
    if [[ ! -f "$build_dir/lib${name}_rdict.pcm" ]]; then
        echo "ROOT dictionary PCM was not generated for lib$name.so" >&2
        return 2
    fi
    "$compiler" -std=c++17 -O2 -fPIC -shared "${root_cflags[@]}" -I"$fixture_dir" \
        "$fixture_dir/$name.cpp" "$build_dir/${name}Dict.cpp" \
        -Wl,-rpath,"$root_libdir" -o "$build_dir/lib$name.so" "${root_libs[@]}"
    "$compiler" -std=c++17 -O2 "${root_cflags[@]}" -I"$fixture_dir" \
        "$fixture_dir/$maker.cpp" -L"$build_dir" "-l$name" \
        -Wl,-rpath,"$build_dir" -Wl,-rpath,"$root_libdir" \
        -o "$build_dir/$maker" "${root_libs[@]}"
}
