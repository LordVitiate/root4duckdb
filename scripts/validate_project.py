#!/usr/bin/env python3
from __future__ import annotations

import re
import subprocess
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
EXPECTED_VERSION = "3.8.0"
INDEX_VERSION = 12
errors: list[str] = []


def read(relative: str) -> str:
    path = ROOT / relative
    if not path.is_file():
        errors.append(f"missing: {relative}")
        return ""
    return path.read_text()


required_files = [
    "README.md",
    "BUILD_RU.md",
    "CHANGELOG.md",
    "RELEASE_NOTES.md",
    "BUILD_STATUS.md",
    "DELIVERY_3.8_RU.md",
    "MIGRATION_3.8_RU.md",
    "DELIVERY_3.7_RU.md",
    "MIGRATION_3.7_RU.md",
    "VERSION",
    "CMakeLists.txt",
    "cmake/RootDependencies.cmake",
    "cmake/RootIceberg.cmake",
    "build-iceberg.sh",
    "build-root4duckdb.sh",
    "setup-source-tree.sh",
    "run-duckdb.sh",
    "scripts/check-iceberg.sh",
    "scripts/detect-build-environment.sh",
    "scripts/run-integration-test.sh",
    "scripts/test-serialized-codec.sh",
    "src/root_scan.cpp",
    "src/root_branch_projection.cpp",
    "src/root_debug.cpp",
    "src/root_serialized_codec.cpp",
    "src/root_serialized_plan.cpp",
    "src/root_serialized_reader.cpp",
    "src/root_bloom.cpp",
    "src/root_filter.cpp",
    "src/root_index_metadata.cpp",
    "src/root_lake_index.cpp",
    "src/root_lake_scan.cpp",
    "src/include/root_branch_projection.hpp",
    "src/include/root_debug.hpp",
    "src/include/root_lake_common.hpp",
    "src/include/root_serialized_codec.hpp",
    "src/include/root_serialized_reader.hpp",
    "src/include/root_bloom.hpp",
    "src/include/root_filter.hpp",
    "src/include/root_index_metadata.hpp",
    "src/include/root_iceberg_catalog.hpp",
    "src/iceberg/root_iceberg_internal.hpp",
    "src/iceberg/root_iceberg_common.cpp",
    "src/iceberg/root_iceberg_publish.cpp",
    "src/iceberg/root_iceberg_scan.cpp",
    "scripts/production/plan_chunks.py",
    "scripts/production/index_chunk.sh",
    "scripts/production/validate_chunks.py",
    "scripts/production/commit_iceberg.sh",
    "test/production/test_control_plane.py",
]
for relative in required_files:
    if not (ROOT / relative).is_file():
        errors.append(f"missing: {relative}")

if read("VERSION").strip() != EXPECTED_VERSION:
    errors.append(f"VERSION is not {EXPECTED_VERSION}")

for obsolete in [
    "scripts/bootstrap-iceberg-cpp.sh",
    "scripts/ensure-iceberg-environment.sh",
    "src/root_iceberg_catalog.cpp",
    ".gitmodules",
]:
    if (ROOT / obsolete).exists():
        errors.append(f"obsolete file remains: {obsolete}")

top_cmake = read("CMakeLists.txt")
for source in [
    "src/root_branch_projection.cpp",
    "src/root_debug.cpp",
    "src/root_serialized_codec.cpp",
    "src/root_serialized_plan.cpp",
    "src/root_serialized_reader.cpp",
    "src/iceberg/root_iceberg_common.cpp",
    "src/iceberg/root_iceberg_publish.cpp",
    "src/iceberg/root_iceberg_scan.cpp",
]:
    if source not in top_cmake:
        errors.append(f"Iceberg source is not compiled: {source}")

iceberg_cmake = read("cmake/RootIceberg.cmake")
for token in [
    "ROOT4DUCKDB_ICEBERG_CORE",
    "ROOT4DUCKDB_ICEBERG_DATA",
    "ROOT4DUCKDB_ICEBERG_BUNDLE",
    "ROOT4DUCKDB_ICEBERG_SQL",
    "root4duckdb_use_shared_iceberg",
]:
    if token not in iceberg_cmake:
        errors.append(f"shared Iceberg CMake feature missing: {token}")
for forbidden in [
    "find_package(iceberg",
    "iceberg::iceberg_static",
    "iceberg::iceberg_bundle",
    "--exclude-libs",
]:
    if forbidden in iceberg_cmake:
        errors.append(f"transitive/static Iceberg linkage remains in RootIceberg.cmake: {forbidden}")

iceberg_build = read("build-iceberg.sh")
for token in [
    "-DICEBERG_BUILD_SHARED=ON",
    "-DICEBERG_BUILD_STATIC=OFF",
    "-DICEBERG_BUILD_SQL_CATALOG=ON",
    "-DICEBERG_SQL_SQLITE=ON",
    "CMAKE_INSTALL_RPATH='$ORIGIN'",
    "ICEBERG_COMMIT=\"0284683f7e1dd5742913d5f0a7c4b48e445df7a4\"",
]:
    if token not in iceberg_build:
        errors.append(f"separate Iceberg build feature missing: {token}")

root_build = read("build-root4duckdb.sh")
if '"$PROJECT_DIR/build-iceberg.sh"' in root_build:
    errors.append("ROOT4DuckDB build still invokes the Iceberg build implicitly")
for token in ["scripts/check-iceberg.sh", "libiceberg_bundle.so", "libiceberg_sql_catalog.so"]:
    if token not in root_build:
        errors.append(f"ROOT build shared-runtime check missing: {token}")

common = read("src/include/root_lake_common.hpp")
if f"ROOT_LAKE_INDEX_VERSION = {INDEX_VERSION}" not in common:
    errors.append(f"common reader index version is not {INDEX_VERSION}")
if "branch->GetEntry(static_cast<Long64_t>(entry))" in common:
    errors.append("semantic access must not materialize a physical leaf/ancestor branch")
for token in [
    "RootDictionaryCleanupMode",
    "RootDictionaryCleanupMode::RETAIN",
    "tree->GetEntry(static_cast<Long64_t>(entry))",
    "ResolvePhysicalBranch",
]:
    if token not in common:
        errors.append(f"universal reader invariant missing: {token}")

version_checks = {
    "scripts/production/plan_chunks.py": f"INDEX_VERSION = {INDEX_VERSION}",
    "scripts/production/index_chunk.sh": f'plan.get("index_version") != {INDEX_VERSION}',
    "scripts/production/validate_chunks.py": f'plan.get("index_version") != {INDEX_VERSION}',
    "scripts/production/commit_iceberg.sh": f"SELECT {INDEX_VERSION} AS index_version",
}
for relative, token in version_checks.items():
    if token not in read(relative):
        errors.append(f"index version mismatch in {relative}")

feature_checks: dict[str, list[str]] = {
    "src/root_scan.cpp": [
        "DirectDictionaryCleanupMode::RETAIN",
        "dictionary_cleanup",
        "root_scan.filter_pushdown = true",
    ],
    "src/root_debug.cpp": ["ROOT4DUCKDB_DEBUG", "ROOT4DUCKDB_DEBUG_VERBOSE"],
    "src/root_serialized_plan.cpp": [
        "BuildSerializedReadPlan",
        "WarnRootFallbackOnce",
    ],
    "src/root_serialized_reader.cpp": ["SerializedBasketReader::Decode", "SERIALIZED.BASKET"],
    "src/root_serialized_codec.cpp": [
        "DecodeSerializedVectorEntry",
        "EqualDecodedValues",
    ],
    "src/root_lake_index.cpp": [
        "root_build_dataset_index",
        "manifest_fingerprint",
        "dictionary_fingerprint",
        "object_context.Read(entry)",
        "catalog_mode",
        "RootIndexMetadataWriter",
    ],
    "src/root_lake_scan.cpp": [
        "root_dataset_stats",
        "entry_selection",
        "source_id",
        "entry_id",
        "metadata_count_only",
        "row_limit",
        "BuildFileTaskGroups",
        "Planning Time (us)",
    ],
    "src/root_bloom.cpp": ["BLOOM_MAGIC", "BLOOM_VERSION", "false_positive_rate", "Mix64"],
    "src/root_filter.cpp": ["RootFilterEvaluator", "ExtractRootUnsignedRange"],
    "src/root_index_metadata.cpp": ["CREATE TEMP TABLE", "FORMAT PARQUET", "DROP TABLE root_index_files"],
    "setup-source-tree.sh": [
        "\"$GIT_BIN\" -C \"$path\" fetch",
        "DUCKDB_COMMIT=\"f31be57c1845a8895169fd58142040be26d433cf\"",
        "CI_TOOLS_COMMIT=\"1f04702aae6f4e7ab45f38689fd79e765221b19e\"",
    ],
}
for relative, tokens in feature_checks.items():
    text = read(relative)
    for token in tokens:
        if token not in text:
            errors.append(f"feature missing in {relative}: {token}")

for source in (ROOT / "src/iceberg").glob("*.cpp"):
    line_count = len(source.read_text().splitlines())
    if line_count > 400:
        errors.append(f"Iceberg module is still too large ({line_count} lines): {source.name}")

for relative in (
    "src/root_bloom.cpp",
    "src/root_filter.cpp",
    "src/root_index_metadata.cpp",
    "src/root_branch_projection.cpp",
    "src/root_debug.cpp",
    "src/root_serialized_codec.cpp",
    "src/root_serialized_plan.cpp",
    "src/root_serialized_reader.cpp",
):
    if len(read(relative).splitlines()) > 400:
        errors.append(f"refactored responsibility grew beyond 400 lines: {relative}")

index_source = read("src/root_lake_index.cpp")
for forbidden in ("CsvWriters", "ConvertCsvToParquet", "root_baskets.csv"):
    if forbidden in index_source:
        errors.append(f"CSV index staging remains: {forbidden}")

production_source = read("scripts/production/pipeline_config.py") + read("scripts/production/plan_chunks.py")
for forbidden in ("30_000_000_000", "24_000_000_000", '"max_files": 8'):
    if forbidden in production_source:
        errors.append(f"hidden small production default remains: {forbidden}")
if "catalog_mode:='external'" not in read("scripts/production/index_chunk.sh"):
    errors.append("production worker does not use explicit external catalog staging")

integration_test = read("scripts/run-integration-test.sh")
for token in [
    "reader_mode := 'serialized'",
    "catalog_mode := 'sqlite'",
    "raw_validation_entries := 2",
]:
    if token not in integration_test:
        errors.append(f"serialized/Iceberg integration coverage missing: {token}")

source_files = list((ROOT / "src").rglob("*.cpp")) + list((ROOT / "src").rglob("*.hpp"))
for source in source_files:
    text = source.read_text()
    if re.search(r"/(?:home|Users)/", text):
        errors.append(f"machine-specific path: {source.relative_to(ROOT)}")

python_files = list((ROOT / "scripts").rglob("*.py")) + [
    ROOT / "orchestration/dagster/definitions.py",
    ROOT / "test/production/test_control_plane.py",
]
for script in python_files:
    if not script.is_file():
        continue
    try:
        compile(script.read_text(), str(script), "exec")
    except Exception as exc:  # pragma: no cover - diagnostic path
        errors.append(f"Python syntax failed: {script.relative_to(ROOT)}: {exc}")

shell_files = list(ROOT.glob("*.sh")) + list((ROOT / "scripts").rglob("*.sh"))
for script in shell_files:
    try:
        subprocess.run(["bash", "-n", str(script)], check=True, capture_output=True, text=True)
    except subprocess.CalledProcessError as exc:
        errors.append(
            f"shell syntax failed: {script.relative_to(ROOT)}: "
            f"{exc.stderr.strip() or exc.stdout.strip()}"
        )

for script in [
    ROOT / "build-iceberg.sh",
    ROOT / "build-root4duckdb.sh",
    ROOT / "setup-source-tree.sh",
    ROOT / "run-duckdb.sh",
    ROOT / "scripts/check-iceberg.sh",
]:
    if script.is_file() and not script.stat().st_mode & 0o111:
        errors.append(f"script is not executable: {script.relative_to(ROOT)}")

if errors:
    print("VALIDATION FAILED")
    for error in errors:
        print(f" - {error}")
    sys.exit(1)

print("VALIDATION OK")
print(f"Project: {ROOT}")
print(f"Version: {EXPECTED_VERSION}")
print(f"Index format: {INDEX_VERSION}")
print("Iceberg linkage: separate shared runtime (4 DSOs)")
