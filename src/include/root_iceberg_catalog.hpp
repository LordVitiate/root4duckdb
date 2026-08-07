#pragma once

#include "duckdb/main/client_context.hpp"
#include "duckdb/main/extension/extension_loader.hpp"

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace duckdb::rootlake {

struct RootIcebergTableCommit {
    std::string table_name;
    int64_t iceberg_snapshot_id = 0;
    std::string metadata_location;
    std::string manifest_list;
    uint64_t record_count = 0;
};

struct RootIcebergCommitResult {
    std::string catalog_root;
    std::string root_snapshot_id;
    std::vector<RootIcebergTableCommit> tables;
};

// Publish the Parquet metadata parts produced by the ROOT indexer into six real
// Apache Iceberg tables backed by SqlCatalog + SQLite. This function is
// deliberately fail-closed: it never writes current.json/_SUCCESS.json and
// never falls back to the legacy local Parquet catalog.
RootIcebergCommitResult PublishRootIndexStagingToIceberg(
    ClientContext &context,
    const std::string &catalog_root,
    const std::string &root_snapshot_id,
    const std::string &staging_dir,
    const std::string &manifest_fingerprint,
    const std::string &dictionary_fingerprint);

bool IsRootIcebergCatalog(const std::string &catalog_root);
std::unordered_map<std::string, RootIcebergTableCommit>
LoadRootIcebergCatalogState(const std::string &catalog_root);

// Uses the separately built shared Apache Iceberg C++ runtime to resolve the
// current snapshot and returns a DuckDB read_parquet([...]) relation over only
// the active Iceberg data files. No DuckDB Iceberg extension is required for
// this local SQLite catalog path.
std::string RootIcebergRelation(const std::string &catalog_root,
                                const std::string &table_name);

// Compatibility relation for the legacy ROOT dataset reader. The old reader
// calls its visibility log "snapshots", while the real Iceberg layout stores
// the ROOT commit boundary in root_index.commits and reserves
// root_index.snapshots for per-table Iceberg snapshot metadata.
std::string RootIcebergCommittedSnapshotsRelation(const std::string &catalog_root,
                                                  uint32_t index_version);

void RegisterRootIcebergCatalog(ExtensionLoader &loader);

} // namespace duckdb::rootlake
