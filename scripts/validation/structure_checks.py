"""Project layout, source manifest, and build-boundary checks."""

from __future__ import annotations

import re

from validation.project_validator import ProjectValidator


EXPECTED_VERSION = "3.8.0"
INDEX_VERSION = 12


def check_structure(validator: ProjectValidator) -> None:
    """Validate files, CMake source coverage, and dependency boundaries."""
    validator.require_files(_required_files())
    if validator.read("VERSION").strip() != EXPECTED_VERSION:
        validator.errors.append(f"VERSION is not {EXPECTED_VERSION}")

    _check_obsolete_files(validator)
    _check_cmake_sources(validator)
    _check_iceberg_build(validator)
    _check_headers(validator)
    _check_index_versions(validator)


def _required_files() -> tuple[str, ...]:
    """Return structural anchors rather than duplicating the CMake manifest."""
    return (
        ".clang-format",
        ".clang-tidy",
        "VERSION",
        "CMakeLists.txt",
        "cmake/RootCompiler.cmake",
        "cmake/RootDependencies.cmake",
        "cmake/RootIceberg.cmake",
        "src/CMakeLists.txt",
        "src/include/root4duckdb/root4duckdb.hpp",
        "src/include/root4duckdb/root_pch.hpp",
        "src/include/root4duckdb/core/core.hpp",
        "src/include/root4duckdb/reader/reader.hpp",
        "src/include/root4duckdb/reader/root_semantic_reader.hpp",
        "src/include/root4duckdb/direct/root_scan_internal.hpp",
        "src/include/root4duckdb/dataset/root_dataset_scan_internal.hpp",
        "src/include/root4duckdb/index/root_index_pipeline.hpp",
        "src/include/root4duckdb/histogram/root_histogram_internal.hpp",
        "src/include/root4duckdb/serialized/root_serialized_codec_utils.hpp",
        "scripts/lib/sql.sh",
        "scripts/production/pipeline_io.py",
        "scripts/production/plan_chunks.py",
        "scripts/production/index_chunk.sh",
        "scripts/production/validate_chunks.py",
        "scripts/production/commit_iceberg.sh",
        "scripts/package-release.sh",
        "test/loadable_extension_smoke.cpp",
        "test/production/test_control_plane.py",
    )


def _check_obsolete_files(validator: ProjectValidator) -> None:
    """Reject retired generators and dependency bootstraps."""
    obsolete = (
        "scripts/bootstrap-iceberg-cpp.sh",
        "scripts/ensure-iceberg-environment.sh",
        "src/root_iceberg_catalog.cpp",
        "src/root_semantic_reader.cpp",
        "src/root_serialized_branch_resolver.cpp",
        "src/include/root_serialized_branch_resolver.hpp",
        "src/include/root_serialized_codec_internal.hpp",
        "src/include/root_meta.hpp",
        "src/root_meta_generator.cpp",
        "cmake/RootSources.cmake",
        ".gitmodules",
    )
    for relative in obsolete:
        if (validator.root / relative).exists():
            validator.errors.append(f"obsolete file remains: {relative}")


def _check_cmake_sources(validator: ProjectValidator) -> None:
    """Ensure each C++ implementation belongs to one build module."""
    top_cmake = validator.read("CMakeLists.txt")
    if "add_subdirectory(src)" not in top_cmake:
        validator.errors.append("top-level CMake does not load the source modules")

    compiled: list[str] = []
    module_cmake_files = sorted((validator.root / "src").rglob("CMakeLists.txt"))
    for cmake_file in module_cmake_files:
        relative_dir = cmake_file.parent.relative_to(validator.root)
        for filename in re.findall(r"\$\{CMAKE_CURRENT_SOURCE_DIR\}/([^\s)]+\.cpp)", cmake_file.read_text()):
            compiled.append((relative_dir / filename).as_posix())
    duplicates = sorted({source for source in compiled if compiled.count(source) > 1})
    for source in duplicates:
        validator.errors.append(f"C++ source is compiled more than once: {source}")
    compiled_set = set(compiled)
    for source in (validator.root / "src").rglob("*.cpp"):
        relative = source.relative_to(validator.root).as_posix()
        if relative not in compiled_set:
            validator.errors.append(f"uncompiled C++ source remains: {relative}")
    for relative in compiled_set:
        if not (validator.root / relative).is_file():
            validator.errors.append(f"CMake source is missing: {relative}")

    entrypoints = {"root_extension.cpp", "root_scan.cpp", "root_lake_index.cpp", "root_lake_scan.cpp"}
    root_sources = {path.name for path in (validator.root / "src").glob("*.cpp")}
    if root_sources != entrypoints:
        validator.errors.append(f"src root must contain only registration entrypoints: {sorted(root_sources)}")
    for module in ("core", "reader", "serialized", "direct", "index", "dataset", "histogram", "iceberg"):
        if not (validator.root / "src" / module / "CMakeLists.txt").is_file():
            validator.errors.append(f"module CMake unit is missing: src/{module}/CMakeLists.txt")
    include_root = validator.root / "src/include/root4duckdb"
    for header in (validator.root / "src").rglob("*.hpp"):
        if include_root not in header.parents and header != validator.root / "src/include/root_extension.hpp":
            validator.errors.append(f"header is outside the module include tree: {header.relative_to(validator.root)}")

    compiler = validator.read("cmake/RootCompiler.cmake")
    validator.require_tokens("compiler policy", compiler, (
        "target_precompile_headers",
        "root_pch.hpp",
        "cxx_std_17",
        "-Wall",
        "-Wextra",
        "-Wpedantic",
    ))
    if "cmake_minimum_required(VERSION 3.25)" not in top_cmake:
        validator.errors.append("CMake 3.25 minimum is required for the modular PCH build")
    validator.require_tokens("clang-format policy", validator.read(".clang-format"), (
        "ColumnLimit: 120",
        "InsertBraces: true",
        "AllowShortFunctionsOnASingleLine: None",
        "BinPackParameters: true",
    ))


def _check_iceberg_build(validator: ProjectValidator) -> None:
    """Preserve the split shared and portable-static Iceberg contract."""
    iceberg_cmake = validator.read("cmake/RootIceberg.cmake")
    validator.require_tokens("RootIceberg.cmake", iceberg_cmake, (
        "ROOT4DUCKDB_ICEBERG_CORE",
        "ROOT4DUCKDB_ICEBERG_DATA",
        "ROOT4DUCKDB_ICEBERG_BUNDLE",
        "ROOT4DUCKDB_ICEBERG_SQL",
        "root4duckdb_use_shared_iceberg",
        "root4duckdb_embed_static_iceberg",
        "iceberg::iceberg_bundle_static",
        "iceberg::iceberg_sql_catalog_static",
        "--exclude-libs,ALL",
        "SKIP_BUILD_RPATH",
        "SKIP_PRECOMPILE_HEADERS",
    ))
    validator.reject_tokens("RootIceberg.cmake", iceberg_cmake, (
        "iceberg::iceberg_shared",
        "iceberg::iceberg_bundle_shared",
    ))

    iceberg_build = validator.read("build-iceberg.sh")
    validator.require_tokens("build-iceberg.sh", iceberg_build, (
        "-DICEBERG_BUILD_SHARED=ON",
        "-DICEBERG_BUILD_STATIC=ON",
        "-DICEBERG_BUILD_SQL_CATALOG=ON",
        "-DICEBERG_SQL_SQLITE=ON",
        "CMAKE_INSTALL_RPATH='$ORIGIN'",
        'ICEBERG_COMMIT="0284683f7e1dd5742913d5f0a7c4b48e445df7a4"',
    ))

    root_build = validator.read("build-root4duckdb.sh")
    if '"$PROJECT_DIR/build-iceberg.sh"' in root_build:
        validator.errors.append("ROOT4DuckDB build still invokes the Iceberg build implicitly")
    validator.require_tokens("build-root4duckdb.sh", root_build, (
        "scripts/check-iceberg.sh",
        "scripts/package-release.sh",
        "libiceberg_bundle.so",
        "libiceberg_sql_catalog.so",
    ))


def _check_headers(validator: ProjectValidator) -> None:
    """Validate umbrella ownership and the ROOT include boundary."""
    common = validator.read("src/include/root4duckdb/core/root_lake_common.hpp")
    if f"ROOT_LAKE_INDEX_VERSION = {INDEX_VERSION}" not in common:
        validator.errors.append(f"common reader index version is not {INDEX_VERSION}")
    validator.reject_tokens("root_lake_common.hpp", common, (
        '#include "root_headers.hpp"',
        '#include "root_semantic_reader.hpp"',
        "class PathResolver",
        "class OffsetValueReader",
        "class RootObjectContext",
    ))

    root_headers = validator.read("src/include/root4duckdb/core/root_headers.hpp")
    validator.require_tokens("ROOT include boundary", root_headers, (
        '#include "Rtypes.h"',
        '#include "TBufferFile.h"',
        '#include "TBranchElement.h"',
        '#include "TLeaf.h"',
        '#include "TStreamerInfoActions.h"',
        '#include "TVirtualCollectionProxy.h"',
        "#undef BIT",
    ))
    validator.require_tokens("public umbrella", validator.read("src/include/root4duckdb/root4duckdb.hpp"), (
        '#include "root4duckdb/core/core.hpp"',
        '#include "root4duckdb/histogram/histogram.hpp"',
        '#include "root4duckdb/reader/reader.hpp"',
        '#include "root4duckdb/serialized/serialized.hpp"',
    ))


def _check_index_versions(validator: ProjectValidator) -> None:
    """Keep control-plane and C++ index formats aligned."""
    checks = {
        "scripts/production/plan_chunks.py": f"INDEX_VERSION = {INDEX_VERSION}",
        "scripts/production/index_chunk.sh": f'plan.get("index_version") != {INDEX_VERSION}',
        "scripts/production/validate_chunks.py": f'plan.get("index_version") != {INDEX_VERSION}',
        "scripts/production/commit_iceberg.sh": f"SELECT {INDEX_VERSION} AS index_version",
    }
    for relative, token in checks.items():
        if token not in validator.read(relative):
            validator.errors.append(f"index version mismatch in {relative}")
