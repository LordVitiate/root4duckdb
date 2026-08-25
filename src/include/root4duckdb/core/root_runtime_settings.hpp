#pragma once

#include "duckdb/common/types/value.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/extension/extension_loader.hpp"

#include <cstdint>

namespace duckdb::rootlake {

struct RootRuntimeSettings {
    idx_t threads = 1;
    idx_t max_in_flight_files = 1;
    idx_t memory_limit_bytes = 0;
    idx_t estimated_worker_bytes = 0;
    uint32_t bloom_bytes = 0;

    /// Resolves explicit settings and resource-aware defaults.
    static RootRuntimeSettings From(ClientContext& context, idx_t file_count, idx_t basket_count_hint = 0,
                                    idx_t work_unit_count = 0);
};

/// Registers extension-wide ROOT execution settings.
void RegisterRootRuntimeSettings(ExtensionLoader& loader);

} // namespace duckdb::rootlake
