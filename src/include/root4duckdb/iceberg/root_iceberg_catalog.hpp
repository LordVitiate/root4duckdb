#pragma once

#include "duckdb/main/client_context.hpp"
#include "duckdb/main/extension/extension_loader.hpp"

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace duckdb::rootlake {

/// One Iceberg table committed at the ROOT snapshot boundary.
struct RootIcebergTableCommit {
    std::string table_name;
    int64_t iceberg_snapshot_id = 0;
    std::string metadata_location;
    std::string manifest_list;
    uint64_t record_count = 0;
};

/// Atomic publication result for all index metadata tables.
struct RootIcebergCommitResult {
    std::string catalog_root;
    std::string root_snapshot_id;
    std::vector<RootIcebergTableCommit> tables;
};

/// Publishes staged metadata into six Iceberg tables and fails closed.
RootIcebergCommitResult PublishRootIndexStagingToIceberg(ClientContext& context, const std::string& catalog_root,
                                                         const std::string& root_snapshot_id,
                                                         const std::string& staging_dir,
                                                         const std::string& manifest_fingerprint,
                                                         const std::string& dictionary_fingerprint);

/// Detects the local SQLite-backed Iceberg catalog layout.
bool IsRootIcebergCatalog(const std::string& catalog_root);
/// Loads the active table snapshots for one ROOT commit.
std::unordered_map<std::string, RootIcebergTableCommit> LoadRootIcebergCatalogState(const std::string& catalog_root);

/// Builds a relation over active Iceberg data files.
std::string RootIcebergRelation(const std::string& catalog_root, const std::string& table_name);

/// Adapts the Iceberg commit log to the reader's snapshot relation.
std::string RootIcebergCommittedSnapshotsRelation(const std::string& catalog_root, uint32_t index_version);

/// Registers the Iceberg catalog table functions.
void RegisterRootIcebergCatalog(ExtensionLoader& loader);

} // namespace duckdb::rootlake
