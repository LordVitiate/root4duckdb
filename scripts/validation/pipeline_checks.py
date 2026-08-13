"""Runtime pipeline and behavior-preservation checks."""

from __future__ import annotations

from validation.project_validator import ProjectValidator


def check_pipeline(validator: ProjectValidator) -> None:
    """Validate direct, indexed, serialized, and production contracts."""
    _check_semantic_reader(validator)
    _check_path_reader(validator)
    _check_feature_groups(validator)
    _check_module_sizes(validator)
    _check_shared_pipeline(validator)
    _check_production(validator)


def _check_semantic_reader(validator: ProjectValidator) -> None:
    """Preserve object-level GetEntry and offset traversal."""
    semantic = validator.source_group(
        "src/include/root4duckdb/reader/*.hpp",
        "src/reader/*.cpp",
    )
    if "branch->GetEntry(static_cast<Long64_t>(entry))" in semantic:
        validator.errors.append("semantic access must not materialize a physical leaf/ancestor branch")
    validator.require_tokens("semantic reader", semantic, (
        "class PathResolver",
        "class OffsetValueReader",
        "class RootObjectContext",
        "class RootObjectReader",
        "class RootEntryReader",
        "RootDictionaryCleanupMode",
        "RootDictionaryCleanupMode::RETAIN",
        "tree->GetEntry(static_cast<Long64_t>(entry))",
        "ResolvePhysicalBranch",
    ))


def _check_path_reader(validator: ProjectValidator) -> None:
    """Preserve serialized-first decoding with semantic fallback."""
    path_reader = validator.source_group(
        "src/include/root4duckdb/reader/root_path_reader.hpp",
        "src/reader/root_path_reader.cpp",
        "src/include/root4duckdb/serialized/*.hpp",
        "src/serialized/*.cpp",
    )
    validator.require_tokens("shared path reader", path_reader, (
        "class RootPathReader",
        "RootPathReader::Resolve",
        "RootPathReader::StartSerialized",
        "RootPathReader::TryReadSerialized",
        "RootPathReader::ActivateFallback",
        "EqualDecodedValues",
        "OffsetValueReader::CollectFlat",
    ))
    codec_utils = validator.read("src/include/root4duckdb/serialized/root_serialized_codec_utils.hpp")
    validator.require_tokens("serialized codec documentation", codec_utils, (
        "/// Reads an unsigned big-endian 16-bit integer.",
        "/// Returns the serialized width of a supported primitive.",
        "/// Decodes one primitive into the common numeric representation.",
    ))


def _check_feature_groups(validator: ProjectValidator) -> None:
    """Check capabilities in their refactored compilation groups."""
    groups = {
        "direct scan": (
            validator.source_group("src/direct/*", "src/include/root4duckdb/direct/*.hpp", "src/root_scan.cpp"),
            (
                "rootlake::RootDictionaryCleanupMode::RETAIN",
                "rootlake::PathResolver::TryResolve",
                "rootlake::RootObjectReader",
                "rootlake::RootPathReader",
                "RootScanBinder",
                "RootScanExecutor",
                "dictionary_cleanup",
                "root_scan.filter_pushdown = true",
            ),
        ),
        "index pipeline": (
            validator.source_group("src/index/*", "src/include/root4duckdb/index/*.hpp", "src/root_lake_index.cpp"),
            (
                "root_build_dataset_index",
                "manifest_fingerprint",
                "dictionary_fingerprint",
                "RootIndexFileBuilder",
                "RootIndexBinder",
                "RootIndexCoordinator",
                "RootIndexPublisher",
                "RootIndexResultWriter",
                "catalog_mode",
                "RootIndexMetadataWriter",
            ),
        ),
        "dataset scan": (
            validator.source_group("src/dataset/*", "src/include/root4duckdb/dataset/*.hpp", "src/root_lake_scan.cpp"),
            (
                "RootDatasetCatalog catalog",
                "DatasetScanBinder",
                "DatasetTaskPlanner",
                "DatasetScanExecutor",
                "RootObjectReader object_reader",
                "RootPathReader path_reader",
                "local.path_reader.TryReadSerialized",
                "local.path_reader.CollectFlat",
                "root_dataset_stats",
                "entry_selection",
                "source_id",
                "entry_id",
                "metadata_count_only",
                "row_limit",
                "BuildFileTaskGroups",
                "Planning Time (us)",
            ),
        ),
    }
    for label, (source, tokens) in groups.items():
        validator.require_tokens(label, source, tokens)

    file_checks = {
        "src/core/root_debug.cpp": ("ROOT4DUCKDB_DEBUG", "ROOT4DUCKDB_DEBUG_VERBOSE"),
        "src/serialized/root_serialized_plan.cpp": ("BuildSerializedReadPlan", "WarnRootFallbackOnce"),
        "src/serialized/root_serialized_reader.cpp": ("SerializedBasketReader::Decode", "SERIALIZED.BASKET"),
        "src/serialized/root_serialized_codec.cpp": ("DecodeSerializedVectorEntry", "EqualDecodedValues"),
        "src/serialized/root_serialized_nested_codec.cpp": ("DecodeSerializedNestedPrimitiveVectorColumn",),
        "src/index/root_index_builder.cpp": (
            "PathResolver::Resolve",
            "RootObjectReader object_reader",
            "RootEntryReader object_entry",
            "RootPathReader reader",
            "path.reader.TryReadSerialized",
            "path.reader.CollectTypedValues",
        ),
        "src/dataset/root_dataset_catalog.cpp": (
            "RootDatasetCatalog::ResolveSources",
            "RootDatasetCatalog::ResolveCommittedSnapshot",
            "RootDatasetCatalog::LoadSchemas",
            "RootDatasetCatalog::LoadPathSchemas",
            "LoadAccessPlan",
        ),
        "src/index/root_bloom.cpp": ("BLOOM_MAGIC", "BLOOM_VERSION", "false_positive_rate", "Mix64"),
        "src/index/root_filter.cpp": ("RootFilterEvaluator", "ExtractRootUnsignedRange"),
        "src/index/root_index_metadata.cpp": ("CREATE TEMP TABLE", "FORMAT PARQUET", "DROP TABLE root_index_files"),
        "setup-source-tree.sh": (
            '"$GIT_BIN" -C "$path" fetch',
            'DUCKDB_COMMIT="f31be57c1845a8895169fd58142040be26d433cf"',
            'CI_TOOLS_COMMIT="1f04702aae6f4e7ab45f38689fd79e765221b19e"',
        ),
    }
    for relative, tokens in file_checks.items():
        validator.require_tokens(relative, validator.read(relative), tokens)


def _check_module_sizes(validator: ProjectValidator) -> None:
    """Keep facades thin and implementation stages reviewable."""
    limits = {
        "src/root_scan.cpp": 300,
        "src/root_lake_scan.cpp": 300,
        "src/root_lake_index.cpp": 300,
        "src/histogram/root_histogram_reader.cpp": 300,
    }
    for relative, limit in limits.items():
        lines = len(validator.read(relative).splitlines())
        if lines > limit:
            validator.errors.append(f"registration facade exceeds {limit} lines ({lines}): {relative}")

    for source in (validator.root / "src").rglob("*.cpp"):
        lines = len(source.read_text().splitlines())
        if lines > 800:
            relative = source.relative_to(validator.root)
            validator.errors.append(
                f"compilation unit exceeds the pipeline-stage limit ({lines} lines): {relative}"
            )


def _check_shared_pipeline(validator: ProjectValidator) -> None:
    """Reject duplicated reader orchestration and obsolete staging."""
    direct = validator.source_group("src/direct/*", "src/include/root4duckdb/direct/*.hpp", "src/root_scan.cpp")
    validator.reject_tokens("direct scan", direct, ("class PathResolver", "class ObjectReader", "class ObjectContext"))
    validator.require_tokens("direct serialized-first contract", direct, (
        "rootlake::RootObjectReader",
        "rootlake::RootPathReader",
        "lstate.path_reader.TryReadSerialized",
        "lstate.path_reader.SerializedActive",
    ))

    consumers = {
        "direct scan": direct,
        "index builder": validator.read("src/index/root_index_builder.cpp"),
        "dataset scan": validator.source_group(
            "src/dataset/*", "src/include/root4duckdb/dataset/*.hpp", "src/root_lake_scan.cpp"
        ),
    }
    for label, source in consumers.items():
        if "RootPathReader" not in source:
            validator.errors.append(f"shared ROOT path reader is not used by {label}")
        if "SerializedBasketReader" in source:
            validator.errors.append(f"duplicated serialized reader orchestration remains in {label}")

    validator.reject_tokens("index registration facade", validator.read("src/root_lake_index.cpp"), (
        "CsvWriters",
        "ConvertCsvToParquet",
        "root_baskets.csv",
    ))
    all_sources = validator.source_group("src/**/*")
    validator.reject_tokens("source tree", all_sources, ("create_meta", "root_meta.hpp", "root_meta_generator.cpp"))


def _check_production(validator: ProjectValidator) -> None:
    """Preserve production sizing, catalog, and integration gates."""
    production = validator.read("scripts/production/pipeline_config.py") + validator.read(
        "scripts/production/plan_chunks.py"
    )
    validator.reject_tokens("production defaults", production, (
        "30_000_000_000",
        "24_000_000_000",
        '"max_files": 8',
    ))
    if "catalog_mode:='external'" not in validator.read("scripts/production/index_chunk.sh"):
        validator.errors.append("production worker does not use explicit external catalog staging")

    integration = validator.read("scripts/run-integration-test.sh")
    validator.require_tokens("integration coverage", integration, (
        "reader_mode := 'serialized'",
        "catalog_mode := 'sqlite'",
        "raw_validation_entries := 2",
    ))
