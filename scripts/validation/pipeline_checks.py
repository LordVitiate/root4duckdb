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
        "include/root4duckdb/reader/*.hpp",
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
        "include/root4duckdb/reader/root_path_reader.hpp",
        "src/reader/root_path_reader.cpp",
        "include/root4duckdb/serialized/*.hpp",
        "src/serialized/*.cpp",
    )
    validator.require_tokens("shared path reader", path_reader, (
        "class RootPathReader",
        "RootPathReader::Resolve",
        "RootPathReader::StartSerialized",
        "RootPathReader::TryReadSerialized",
        "RootPathReader::ActivateFallback",
        "EqualDecodedValues",
        "OffsetValueReader::CollectDirect",
    ))
    nested_codec = validator.read("src/serialized/root_serialized_nested_codec.cpp")
    validator.reject_tokens("standalone nested codec", nested_codec, (
        "root_serialized_reader.hpp",
        "root_headers.hpp",
        "TBufferFile",
        "SerializedReadPlan",
    ))
    nested_fixed = validator.read("src/serialized/root_serialized_nested_fixed.cpp")
    validator.require_tokens("ROOT-backed nested decoder", nested_fixed, (
        "DecodeSerializedNestedFixedColumn",
        "DecodeNestedMemberWise",
    ))

    codec_utils = validator.read("include/root4duckdb/serialized/root_serialized_codec_utils.hpp")
    validator.require_tokens("serialized codec documentation", codec_utils, (
        "/// Reads an unsigned big-endian 16-bit integer.",
        "/// Returns the serialized width of a supported primitive.",
        "/// Decodes one primitive into the common numeric representation.",
    ))


def _check_feature_groups(validator: ProjectValidator) -> None:
    """Check capabilities in their refactored compilation groups."""
    groups = {
        "direct scan": (
            validator.source_group("src/direct/*", "include/root4duckdb/direct/*.hpp", "src/root_scan.cpp"),
            (
                "rootlake::RootDictionaryCleanupMode::FULL",
                "rootlake::PathResolver::TryResolve",
                "rootlake::RootObjectReader",
                "rootlake::RootPathReader",
                "RootScanBinder",
                "RootScanExecutor",
                "dictionary_cleanup",
                "rootlake::ConfigureRootRelationScan(root_scan)",
            ),
        ),
        "index pipeline": (
            validator.source_group("src/index/*", "include/root4duckdb/index/*.hpp", "src/root_lake_index.cpp"),
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
            validator.source_group("src/dataset/*", "include/root4duckdb/dataset/*.hpp", "src/root_lake_scan.cpp"),
            (
                "RootDatasetCatalog catalog",
                "DatasetScanBinder",
                "DatasetTaskPlanner",
                "DatasetScanExecutor",
                "RootObjectReader object_reader",
                "RootPathReader path_reader",
                "local.path_reader.BeginNumericEntry",
                "local.path_reader.ReadNumericPage",
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

    # Bounded object traversal must be wired into every consumer rather than
    # merely declared. Reject regressions to entry-sized staging buffers.
    validator.require_tokens("resumable ROOT object cursor", validator.source_group(
        "include/root4duckdb/reader/root_offset_reader.hpp",
        "src/reader/root_offset_reader.cpp",
    ), (
        "class OffsetValueCursor final",
        "OffsetValueCursor::ReadPage",
        "OffsetValueCursor::Reset",
        "CursorPageSink",
        "sink.StartIndex(level, indices)",
    ))
    validator.require_tokens("direct ROOT cursor integration", validator.read(
        "src/direct/root_scan_execute.cpp"
    ), (
        "RootScanExecutor::ReadCursorPage",
        "lstate.cursor_entry_active",
        "lstate.path_readers",
    ))
    validator.require_tokens("dataset ROOT cursor integration", validator.read(
        "src/dataset/root_dataset_execute.cpp"
    ), (
        "local.path_reader.BeginNumericEntry",
        "local.path_reader.ReadNumericPage",
        "local.path_reader.EndNumericPage",
    ))
    validator.require_tokens("index ROOT cursor integration", validator.read(
        "src/index/root_index_builder.cpp"
    ), (
        "path.reader.BeginNumericEntry",
        "path.reader.ReadNumericPage",
        "path.reader.EndNumericPage",
    ))
    validator.require_tokens("serialized-first shared path ownership", validator.source_group(
        "include/root4duckdb/reader/root_path_reader.hpp",
        "src/reader/root_path_reader.cpp",
        "src/serialized/root_serialized_plan.cpp",
    ), (
        "RootPathReader::ReadObjectPage",
        "RootPathReader::BindObjectPath",
        "RootPathReader::BeginObjectEntry",
        "ConfigureNestedFixedProjection",
    ))
    validator.reject_tokens("deleted nested fixed opt-in", validator.read(
        "src/serialized/root_serialized_plan.cpp"
    ), ("ROOT4DUCKDB_NESTED_FIXED_OFFSET",))
    validator.require_tokens("DuckDB 2.0 constant comparison classification", validator.source_group(
        "src/index/root_filter.cpp", "src/direct/root_scan_state.cpp",
        "include/root4duckdb/index/root_filter.hpp"
    ), ("RootIsSimpleConstantComparisonFilter", "TableFilterType::EXPRESSION_FILTER",
        "ExpressionClass::BOUND_CONSTANT"))
    validator.reject_tokens("legacy filter type is not a DuckDB 2.0 scan predicate", validator.read(
        "src/direct/root_scan_state.cpp"
    ), ("TableFilterType::LEGACY_CONSTANT_COMPARISON",))
    validator.require_tokens("entry-level direct ROOT filter partition", validator.source_group(
        "src/direct/root_scan_state.cpp",
        "src/direct/root_scan_modes.cpp",
        "include/root4duckdb/direct/root_scan_internal.hpp"
    ), ("rootlake::PartitionRootRowFilters", "entry_filters", "element_filters", "entry_filters_pass",
        "PassesCachedFilters(context, bind_data, gstate, lstate, entry, 0, true)"))

    scan_contract = validator.source_group(
        "include/root4duckdb/core/root_scan_contract.hpp",
        "src/core/root_lake_common.cpp",
    )
    validator.require_tokens("shared ROOT scan registration", scan_contract, (
        "ConfigureRootRelationScan", "filter_pushdown = true", "filter_prune = true",
        "projection_pushdown = true", "OrderPreservationType::NO_ORDER",
    ))
    validator.require_tokens("indexed ROOT scan registration", validator.read("src/root_lake_scan.cpp"),
                             ("ConfigureRootRelationScan(function)",))
    validator.require_tokens("canonical ROOT filter evaluation", validator.source_group(
        "include/root4duckdb/index/root_filter.hpp",
        "src/direct/root_scan_modes.cpp",
        "src/dataset/root_dataset_filters.cpp",
    ), ("PartitionRootRowFilters", "EvaluateRootRowFilters"))
    validator.require_tokens("local ROOT event coordinates", validator.source_group(
        "src/dataset/root_dataset_plan.cpp", "src/dataset/root_dataset_execute.cpp",
    ), ("global.local_entry_range", "PassesDatasetEntryFilters", "global.event_lower"))
    validator.require_tokens("indexed entry filter declaration dependency",
                             validator.read("src/dataset/root_dataset_execute.cpp"),
                             ('#include "root4duckdb/dataset/root_dataset_pruning.hpp"',))
    validator.reject_tokens("per-task ROOT TTreeCache range reconfiguration",
        validator.read("src/dataset/root_dataset_execute.cpp"), ("SetCacheEntryRange(",))
    validator.require_tokens("ROOT TTree cache on/off tuple regression",
        validator.read("scripts/run-integration-test.sh"),
        ("tree-cache-parity.sql", "tree-no-cache-parity.csv", "tree-cache-parity.csv"))
    for consumer in ("src/direct/root_scan_state.cpp", "src/dataset/root_dataset_file.cpp",
                     "src/index/root_index_builder.cpp"):
        validator.require_tokens("shared serialized TBasket cache binding: " + consumer,
                                 validator.read(consumer), ("AttachSharedBasketCache(",))
    validator.require_tokens("shared numeric object-page reuse and exact tuple regression",
        validator.source_group("src/reader/root_path_reader.cpp", "scripts/run-integration-test.sh"),
        ("numeric_page", "values.swap(page.numbers)", "OBJECT_NUMERIC_PAGE_PARITY"))
    validator.require_tokens("direct one-column shared fixed-member numeric pager",
        validator.read("src/direct/root_scan_execute.cpp"),
        ("BeginNumericEntry(", "ReadNumericPage(STANDARD_VECTOR_SIZE",
         "RootNumericEntryRoute::SERIALIZED_PAGE", "Serialized Page Calls"))
    validator.require_tokens("direct shared pager integration acceptance",
        validator.read("scripts/run-integration-test.sh"),
        ("DIRECT_PAGED_CALLS", "DIRECT_PAGED_OBJECT", "direct single-field FIXED_MEMBER"))
    validator.require_tokens("direct synchronized multi-column page alignment and deterministic fallback",
        validator.read("src/direct/root_scan_execute.cpp"),
        ("multi_numeric_page_state_indices", "state.indices != reference->indices",
         "multi_numeric_shape_mismatch", "EndEntryCursors(lstate)",
         "Serialized Multi-Column Pages"))
    validator.require_tokens("multi-column 4099-row exact tuple and page count regression",
        validator.read("scripts/run-integration-test.sh"),
        ("DIRECT_MULTI_PAGED_PARITY", "DIRECT_MULTI_PAGES",
         "direct two-column ROOT relation streams 4099 exact indexed tuples"))
    for path in ("src/direct/root_scan_execute.cpp", "src/direct/root_scan_modes.cpp",
                 "src/direct/root_scan_special_modes.cpp", "src/direct/root_describe.cpp",
                 "src/dataset/root_dataset_execute.cpp", "src/root_lake_index.cpp",
                 "src/root_lake_scan.cpp", "src/iceberg/root_iceberg_scan.cpp"):
        validator.reject_tokens("deprecated DuckDB DataChunk cardinality: " + path,
                                validator.read(path), (".SetCardinality(",))
    validator.reject_tokens("incorrect global pushdown of file-local entry_id",
        validator.read("src/dataset/root_dataset_plan.cpp"),
        ("MergeEventRangeIntoGlobal(global, ExtractRootUnsignedRange(*entry_filter))",
         'ZonemapClause(*entry_filter, "f.event_base"',
         'ZonemapClause(*entry_filter, "f.event_base + b.entry_begin"'))
    validator.reject_tokens("obsolete unpaged index reader",
        validator.read("src/index/root_index_builder.cpp"),
        ("path.reader.TryReadSerialized(", "paths_per_branch["))
    validator.require_tokens("atomic index segment publication and digests",
        validator.read("src/index/root_index_metadata.cpp"),
        ("pending", "fs::rename(pending, committed)", "checkpoint.json",
         "FileContentFingerprint", '"segment-*"'))
    validator.require_tokens("crash-safe checkpoint restoration and catalog lock",
        validator.read("src/index/root_index_coordinator.cpp"),
        ("IndexBuildLock", "ReadMatchingRequest", "RestoreCheckpoints",
         "QuarantineIncompleteCheckpoint", "FileContentFingerprint(filename.string())",
         "ROOT4DUCKDB_TEST_FAIL_AFTER_INDEX"))
    validator.require_tokens("explicit index resume SQL option",
        validator.source_group("src/root_lake_index.cpp", "src/index/root_index_bind.cpp"),
        ('options.Add("resume", LogicalType::BOOLEAN)', 'result->resume ='))
    validator.require_tokens("interrupted index snapshot regression",
        validator.read("scripts/run-integration-test.sh"),
        ("ROOT4DUCKDB_TEST_FAIL_AFTER_INDEX=1", "resume := true",
         "recovered validated Parquet checkpoint", "index checkpoint resumed"))
    validator.require_tokens("production chunk checkpoint preservation",
        validator.read("scripts/production/index_chunk.sh"),
        ('resume:=true', 'candidate/"chunk.json"', 'item.rename(target)'))

    validator.require_tokens("direct cached virtual indexes for projections and filters", validator.source_group(
        "src/direct/root_scan_modes.cpp", "include/root4duckdb/direct/root_scan_internal.hpp"
    ), ("cached_index_sources", "resolve_index(binding.column_id)",
        "FindCachedIndexSource(bind_data, lstate, col_idx, elem_idx)"))
    validator.reject_tokens("legacy per-row virtual index search", validator.read(
        "src/direct/root_scan_modes.cpp"
    ), ("std::string search = col.name;", "for (idx_t cand = 0; cand < bind_data.columns.size(); ++cand)"))
    validator.reject_tokens("retired unused direct entry parameters", validator.read(
        "src/direct/root_scan_execute.cpp"
    ), ("(void)output;", "(void)out_count;", "ReadAndCacheEntry(bind_data, gstate, lstate, output"))
    validator.reject_tokens("deleted duplicated direct cursors", validator.source_group(
        "src/direct/*", "include/root4duckdb/direct/*.hpp"
    ), ("object_cursors", "serialized.path_reader"))
    validator.reject_tokens("deleted dataset cursor", validator.source_group(
        "src/dataset/*", "include/root4duckdb/dataset/*.hpp"
    ), ("local.object_cursor",))
    validator.reject_tokens("removed direct leaf strategy", validator.source_group(
        "src/direct/*", "include/root4duckdb/direct/*.hpp", "src/root_scan.cpp"
    ), (
        "RootDirectBranchScanMode", "IsDirectBranchMode", "SelectDirectBranchMode",
        "ProcessDirectBranch", "PassesDirectBranchFilters", "direct_leaf",
    ))
    validator.reject_tokens("removed numeric object sinks", validator.source_group(
        "src/reader/*", "include/root4duckdb/reader/*.hpp"
    ), ("NumericOnlySink", "FlatNumericSink", "OffsetValueReader::CollectFlat",
        "OffsetValueReader::CollectValues", "FallbackRecorded()"))
    validator.require_tokens("executed nested fixed decoder counters", validator.source_group(
        "src/serialized/root_serialized_reader.cpp",
        "src/direct/root_scan_execute.cpp",
        "src/dataset/root_dataset_execute.cpp",
    ), (
        "nested_fixed_direct_entries", "nested_fixed_scratch_entries",
        "Nested Fixed Direct Entries", "Nested Fixed Scratch Entries",
    ))
    validator.require_tokens("no implicit nested ROOT joins", validator.source_group(
        "src/reader/root_path_resolver.cpp", "src/direct/root_scan_execute.cpp"
    ), (
        "IndexSignature(child_levels) == target_signature",
        "result.vector_indices != sibling->vector_indices",
    ))
    validator.require_tokens("unified indexed numeric entry start", validator.source_group(
        "src/reader/root_path_reader.cpp", "include/root4duckdb/reader/root_path_reader.hpp",
        "src/dataset/root_dataset_execute.cpp"
    ), ("RootNumericEntryStartResult", "BeginNumericEntry", "RootNumericEntryRoute",
        "cannot begin another ROOT entry with unread numeric pages", "page.numbers.size() != page.size()"))
    validator.reject_tokens("dataset bypasses unified entry routing", validator.read(
        "src/dataset/root_dataset_execute.cpp"
    ), ("local.path_reader.BeginSerializedPage(", "local.path_reader.BeginObjectEntry(",
        "local.path_reader.TryReadSerialized(entry, object_entry, local.typed_values"))
    validator.require_tokens("serialized page cursor", validator.source_group(
        "src/serialized/*", "include/root4duckdb/serialized/*.hpp",
        "src/reader/root_path_reader.cpp", "src/dataset/root_dataset_execute.cpp"
    ), (
        "SerializedVectorPageCursor", "BeginSerializedPage",
        "ReadNumericPage", "HasNumericPage", "EndNumericPage", "Serialized Page Calls",
    ))
    validator.reject_tokens("retired serialized-only page API", validator.source_group(
        "src/reader/*", "include/root4duckdb/reader/*.hpp", "src/dataset/*"
    ), ("ReadSerializedPage", "HasSerializedPage", "EndSerializedPage"))
    validator.require_tokens("semantic relation and physical branch identities", validator.source_group(
        "src/direct/*", "include/root4duckdb/direct/*.hpp"
    ), ("projected_relation_columns", "root_class_name", "primitive_branch_name",
        "serialized_basket_caches", "result.vector_indices != sibling->vector_indices"))
    validator.reject_tokens("retired physical branch relation grouping", validator.source_group(
        "src/direct/*", "include/root4duckdb/direct/*.hpp"
    ), ("projected_branch_columns", "std::string branch_name;"))
    validator.require_tokens("paged fixed-member integration", validator.read(
        "scripts/run-integration-test.sh"
    ), ("serialized-fixed-pager.sql", "raw_max_values_per_entry := 1"))

    validator.require_tokens("integration uses the shared runtime launcher", validator.read(
        "scripts/run-integration-test.sh"
    ), ('DUCKDB_BIN="${DUCKDB_BIN:-$PROJECT_DIR/run-duckdb.sh}"',))
    validator.reject_tokens("integration must not discover arbitrary DuckDB binaries", validator.read(
        "scripts/run-integration-test.sh"
    ), ("DISCOVERED_DUCKDB",))
    validator.require_tokens("physical decoder and strict sibling regressions", validator.read(
        "scripts/run-integration-test.sh"
    ), ("SERIALIZED.NESTED_FIXED_OFFSET", "serialized-siblings.sql",
        "serialized=2, object_fallback=0"))

    validator.require_tokens("shared primitive type-safe conversion", validator.source_group(
        "include/root4duckdb/reader/root_primitive_value.hpp",
        "src/reader/root_primitive_value.cpp",
        "src/direct/root_scan_modes.cpp",
        "src/dataset/root_dataset_execute.cpp"
    ), ("AsInteger()", "AsFloat()", "std::isfinite(value)",
        "value.AsInteger<int8_t>()", "RootPrimitiveValue::Floating(value).AsFloat()"))
    validator.reject_tokens("duplicated dataset primitive narrowing policy", validator.read(
        "src/dataset/root_dataset_execute.cpp"
    ), ("CheckedDatasetInteger",))
    validator.reject_tokens("unchecked dataset float-to-integer casts", validator.read(
        "src/dataset/root_dataset_execute.cpp"
    ), ("static_cast<int8_t>(value)", "static_cast<uint8_t>(value)",
        "static_cast<int16_t>(value)", "static_cast<uint16_t>(value)",
        "static_cast<int32_t>(value)", "static_cast<uint32_t>(value)",
        "static_cast<int64_t>(value)", "static_cast<uint64_t>(value)"))
    validator.require_tokens("unaligned-safe ROOT object scalar loads", validator.read(
        "src/reader/root_primitive_conversion.cpp"
    ), ("LoadRootPrimitive", "std::memcpy(&value, pointer, sizeof(value))"))
    validator.reject_tokens("ROOT object scalar type punning", validator.read(
        "src/reader/root_primitive_conversion.cpp"
    ), ("reinterpret_cast<int64_t*>(pointer)", "reinterpret_cast<uint64_t*>(pointer)",
        "reinterpret_cast<double*>(pointer)", "reinterpret_cast<bool*>(pointer)"))
    validator.require_tokens("single checked numeric conversion policy", validator.read(
        "src/reader/root_primitive_conversion.cpp"
    ), ("Floating(value).AsUnsigned()", "Floating(value).AsSigned()"))
    validator.reject_tokens("unchecked numeric narrowing in ROOT adapters", validator.read(
        "src/reader/root_primitive_conversion.cpp"
    ), ("Unsigned(static_cast<uint64_t>(value))", "Signed(static_cast<int64_t>(value))"))
    validator.require_tokens("portable nested signed count check", validator.read(
        "src/serialized/root_serialized_nested_codec.cpp"
    ), ("encoded_count > static_cast<uint32_t>(std::numeric_limits<int32_t>::max())",))
    validator.reject_tokens("implementation-dependent nested count narrowing", validator.read(
        "src/serialized/root_serialized_nested_codec.cpp"
    ), ("static_cast<int32_t>(encoded_count)",))
    validator.require_tokens("CERT-safe split leaf scratch and counters", validator.read(
        "src/serialized/root_serialized_leaf_reader.cpp"
    ), ("CheckedPrimitiveCount", "std::isfinite", "std::trunc", "std::memcpy",
        "ReadLeafScratch", "primitive branch index dimensions exceed the host capacity"))
    validator.reject_tokens("retired split leaf type-punned scratch", validator.read(
        "src/serialized/root_serialized_leaf_reader.cpp"
    ), ("reinterpret_cast<const bool*>(scratch)", "reinterpret_cast<const int64_t*>(scratch)",
        "reinterpret_cast<const double*>(scratch)", "object_count = static_cast<int>(counted);"))
    validator.require_tokens("overflow-safe scratch rounding", validator.read(
        "src/serialized/root_serialized_reader.cpp"
    ), ("byte_count / alignment + (byte_count % alignment != 0)",))
    validator.require_tokens("serialized overflow sentinel", validator.read(
        "src/serialized/root_serialized_deep.cpp"
    ), (
        "sentinel_limit",
        "values.size() > max_values_per_entry",
    ))
    validator.require_tokens("cursor integration regression", validator.read(
        "scripts/run-integration-test.sh"
    ), (
        "cursor-regression.sql",
        "ROOT cursor regression: 99/100/101",
    ))

    # Direct scans own dictionary-created objects. Do not silently restore
    # the former RETAIN default, which leaked one object per reader lifetime.
    direct_bind = validator.read("src/direct/root_scan_bind.cpp")
    validator.require_tokens("direct ROOT object ownership", direct_bind, (
        "bind_data->root_access.dictionary_cleanup_mode = rootlake::RootDictionaryCleanupMode::FULL;",
    ))
    if "dictionary_loaded ? rootlake::RootDictionaryCleanupMode::RETAIN" in direct_bind:
        validator.errors.append("direct scan must reclaim dictionary-created ROOT objects")

    file_checks = {
        "src/core/root_debug.cpp": ("ROOT4DUCKDB_DEBUG", "ROOT4DUCKDB_DEBUG_VERBOSE"),
        "src/serialized/root_serialized_plan.cpp": ("BuildSerializedReadPlan",),
        "src/serialized/root_reader_mode.cpp": ("ParseRootReaderMode", "RootReaderModeName", "WarnRootFallbackOnce"),
        "src/serialized/root_serialized_reader.cpp": ("SerializedBasketReader::Decode", "SERIALIZED.BASKET"),
        "src/serialized/root_serialized_codec.cpp": ("DecodeSerializedVectorEntry", "EqualDecodedValues"),
        "src/serialized/root_serialized_nested_codec.cpp": ("DecodeSerializedNestedPrimitiveVectorColumn",),
        "src/index/root_index_builder.cpp": (
            "PathResolver::Resolve",
            "RootObjectReader object_reader",
            "RootEntryReader object_entry",
            "RootPathReader reader",
            "path.reader.BeginNumericEntry",
            "path.reader.ReadNumericPage",
            "path.reader.EndNumericPage",
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
            'DUCKDB_COMMIT="d54cbbe3df17e8e5ce68312f483b4155836112aa"',
            'CI_TOOLS_COMMIT="b8d325c37e82160886579880707493bd358c0e62"',
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
    direct = validator.source_group("src/direct/*", "include/root4duckdb/direct/*.hpp", "src/root_scan.cpp")
    validator.reject_tokens("direct scan", direct, ("class PathResolver", "class ObjectReader", "class ObjectContext"))
    validator.require_tokens("direct serialized-first contract", direct, (
        "rootlake::RootObjectReader",
        "rootlake::RootPathReader",
        "lstate.path_readers[serialized.column_id].TryReadSerialized",
        "lstate.path_readers[serialized.column_id].SerializedActive",
        "serialized_columns",
    ))

    consumers = {
        "direct scan": direct,
        "index builder": validator.read("src/index/root_index_builder.cpp"),
        "dataset scan": validator.source_group(
            "src/dataset/*", "include/root4duckdb/dataset/*.hpp", "src/root_lake_scan.cpp"
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

    build_script = validator.read("build-root4duckdb.sh")
    validator.require_tokens("bootstrap build smoke test", build_script, (
        "Unexpected direct ROOT/Iceberg dependency",
        "ROOT extension: 6/6 public functions registered",
        '"$DUCKDB_BIN" -unsigned -bail -cmd',
    ))
    validator.reject_tokens("obsolete shared Iceberg host requirement", build_script, (
        "Bundled DuckDB is not linked to",
    ))

    validator.require_tokens("ROOT C++ ABI compatibility gate", build_script, (
        'compiler_release=',
        'compiler_cxx11_abi=',
        'root_release=',
        'root_cxx11_abi=',
        '_GLIBCXX_USE_CXX11_ABI',
        '_GLIBCXX_RELEASE',
        '[ERROR] ROOT/Cling and compiler use different C++11 string ABIs.',
        '[WARN] ROOT/Cling and compiler use different libstdc++ header major versions.',
        'CXX11_ABI matches; required symbols and runtime were checked.',
    ))
    validator.reject_tokens("obsolete ROOT header version ABI gate", build_script, (
        '[ERROR] ROOT/Cling and compiler use different C++ headers.',
        '[ERROR] ROOT/Cling and compiler use incompatible libstdc++ ABI settings.',
    ))

    package = validator.read("scripts/package-release.sh")
    validator.require_tokens("C API release packaging", package, (
        "EXPECTED_CAPI_VERSION",
        '"C_STRUCT"',
        '"root_init_c_api"',
        "root.duckdb_extension",
    ))
    validator.reject_tokens("retired C++ release ABI", package, (
        "root_duckdb_cpp_init",
        "v1.4.5",
    ))

    smoke_host = validator.read("test/loadable_extension_smoke.cpp")
    validator.require_tokens("standalone loadable C API smoke", smoke_host, (
        '#include "duckdb.h"',
        'duckdb_set_config(config, "allow_unsigned_extensions", "true")',
        "duckdb_open_ext(",
        "duckdb_query(",
        "duckdb_value_int64(",
    ))
    validator.reject_tokens("removed C++ DuckDB config member", smoke_host, (
        "config.options.allow_unsigned_extensions",
    ))

    cmake = validator.read("CMakeLists.txt")
    validator.require_tokens("standalone smoke host linkage", cmake, (
        "target_link_libraries(root_loadable_extension_smoke PRIVATE",
        "duckdb_static",
        "dummy_static_extension_loader",
    ))

    native_concurrency = validator.read("test/fixtures/concurrent_root_load.cpp")
    validator.require_tokens("concurrent ROOT load isolation", native_concurrency, (
        "ROOT4DUCKDB_TEST_EXTENSION_PATH",
        "RootLoadSQL()",
        "QuoteSQL(extension_path)",
        'duckdb_set_config(config, "allow_unsigned_extensions", "true")',
        "duckdb_open_ext(",
    ))
    validator.require_tokens("concurrent ROOT load invocation", validator.read("scripts/run-integration-test.sh"), (
        'ROOT4DUCKDB_TEST_EXTENSION_PATH="${ROOT_EXTENSION:-}"',
    ))

    integration = validator.read("scripts/run-integration-test.sh")
    validator.require_tokens("integration coverage", integration, (
        "reader_mode := 'serialized'",
        "catalog_mode := 'sqlite'",
        "raw_validation_entries := 2",
    ))
