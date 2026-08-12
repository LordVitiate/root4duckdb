#pragma once

#include "root_index_metadata.hpp"
#include "root_semantic_reader.hpp"
#include "root_serialized_reader.hpp"

#include <cstdint>
#include <set>
#include <string>
#include <vector>

namespace duckdb::rootlake {

struct RootIndexBuildOptions {
    std::string tree_name;
    std::vector<std::string> logical_paths;
    uint32_t bloom_bytes = 0;
    double bloom_false_positive_rate = 0.01;
    uint64_t tree_cache_bytes = 64ULL * 1024ULL * 1024ULL;
    RootReaderMode reader_mode = RootReaderMode::AUTO;
    uint32_t raw_validation_entries = 4;
    uint64_t raw_max_entry_bytes = 64ULL * 1024ULL * 1024ULL;
    uint64_t raw_max_values_per_entry = 10ULL * 1024ULL * 1024ULL;
    RootDictionaryCleanupMode dictionary_cleanup_mode =
        RootDictionaryCleanupMode::FULL;
};

struct RootIndexBuildStatus {
    std::string file_path;
    std::string file_id;
    std::string schema_id;
    uint64_t entries = 0;
    uint64_t flattened_values = 0;
    uint64_t baskets = 0;
    std::string status;
    std::string message;
};

struct RootIndexFilePlan {
    std::string path;
    uint64_t entries = 0;
    uint64_t event_base = 0;
    std::string error;
};

RootIndexFilePlan InspectRootIndexFile(const RootIndexBuildOptions &options,
                                       const std::string &path);

class RootIndexFileBuilder {
public:
    RootIndexFileBuilder(const RootIndexBuildOptions &options,
                         std::string dataset_id, std::string snapshot_id);

    RootIndexBuildStatus Build(const std::string &root_path, uint64_t event_base,
                               RootFileIndexMetadata &metadata,
                               std::set<std::string> &written_columns) const;

private:
    const RootIndexBuildOptions &options;
    std::string dataset_id;
    std::string snapshot_id;
};

} // namespace duckdb::rootlake
