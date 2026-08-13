#pragma once

#include "root4duckdb/dataset/root_dataset_scan_internal.hpp"

namespace duckdb::rootlake {

/// Conservatively evaluates indexed metadata for one path predicate.
bool PredicateMetadataMayMatch(const PathPredicateBinding& predicate, const Value& min_value, const Value& max_value,
                               uint64_t nan_count, uint64_t pos_inf_count, uint64_t neg_inf_count,
                               const Value& bloom_value);

/// Reports whether DOUBLE metadata is lossless for a physical type.
bool UsesDoubleBackedValueMetadata(const LogicalType& type);

/// Parses and binds auxiliary semantic-path predicates.
void ParsePathPredicates(RootDatasetCatalog& catalog, DatasetBindData& bind, const std::string& raw_json);

/// Builds a conservative min/max SQL pruning clause.
std::string ZonemapClause(const TableFilter& filter, const std::string& min_expr, const std::string& max_expr,
                          const LogicalType& physical_type);

/// Finds the filter attached to a full output column.
optional_ptr<TableFilter> FilterForFullColumn(const DatasetGlobalState& global, column_t full_column);

/// Conservatively evaluates a Bloom payload against a filter.
bool BloomMayContainFilter(const TableFilter& filter, const string& bytes, const LogicalType& physical_type);

/// Reports whether a filter benefits from Bloom metadata.
bool FilterNeedsBloom(const TableFilter& filter);

/// Intersects an extracted event range with the global plan.
void MergeEventRangeIntoGlobal(DatasetGlobalState& global, const RootUnsignedFilterRange& range);

/// Builds an exact string predicate for catalog-side pruning.
std::string ExactStringFilterClause(const TableFilter& filter, const std::string& column_sql);

/// Detects filters that reject every materialized numeric row.
bool RejectsAllMaterializedRows(const TableFilter& filter);

/// Builds a quoted schema or column identifier list.
std::string IdListSQL(const std::vector<SchemaBinding>& schemas, bool column_ids);

/// Parses exact source and entry selection metadata.
void ParseEntrySelection(DatasetBindData& bind, const std::string& raw_json);

/// Builds the pre-decode predicate intersection.
void BuildPredicateIntersection(ClientContext& context, const DatasetBindData& bind, DatasetGlobalState& global);

} // namespace duckdb::rootlake
