#include "root4duckdb/core/root_runtime_settings.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/main/config.hpp"

#include <algorithm>
#include <limits>

namespace duckdb::rootlake {

static idx_t ReadUnsignedSetting(ClientContext& context, const string& name, idx_t fallback) {
    Value value;
    if (context.TryGetCurrentSetting(name, value)) {
        return value.GetValue<idx_t>();
    }
    return fallback;
}

static idx_t ReadMemorySetting(ClientContext& context, const string& name, idx_t fallback) {
    Value value;
    if (!context.TryGetCurrentSetting(name, value)) {
        return fallback;
    }
    const auto text = value.ToString();
    if (StringUtil::CIEquals(text, "auto")) {
        return fallback;
    }
    return DBConfig::ParseMemoryLimit(text);
}

RootRuntimeSettings RootRuntimeSettings::From(ClientContext& context, idx_t file_count, idx_t basket_count_hint,
                                                  idx_t work_unit_count) {
    RootRuntimeSettings out;

    const auto& config = DBConfig::GetConfig(context);
    const auto configured_threads =
        config.options.maximum_threads == DConstants::INVALID_INDEX ? idx_t(1) : config.options.maximum_threads;
    const auto concurrency_units =
        std::max<idx_t>(1, work_unit_count == 0 ? std::max<idx_t>(1, file_count) : work_unit_count);
    out.threads = std::max<idx_t>(1, std::min(configured_threads, concurrency_units));

    const auto explicit_in_flight = ReadUnsignedSetting(context, "root_max_in_flight_files", 0);
    out.max_in_flight_files =
        explicit_in_flight == 0 ? out.threads : std::max<idx_t>(1, std::min(explicit_in_flight, concurrency_units));

    const auto duckdb_memory = config.options.maximum_memory == DConstants::INVALID_INDEX
                                   ? idx_t(4ULL * 1024ULL * 1024ULL * 1024ULL)
                                   : config.options.maximum_memory;
    out.memory_limit_bytes = ReadMemorySetting(context, "root_memory_limit", duckdb_memory / 2);

    // Admission estimate is internal: 192 MiB base + 64 MiB for large basket
    // plans. Divide before comparing so the admission calculation itself cannot
    // overflow when a user configures a very large worker count.
    out.estimated_worker_bytes = 192ULL * 1024ULL * 1024ULL;
    if (basket_count_hint > 4096) {
        out.estimated_worker_bytes += 64ULL * 1024ULL * 1024ULL;
    }
    const auto memory_cap = out.estimated_worker_bytes == 0
                                ? idx_t(1)
                                : std::max<idx_t>(1, out.memory_limit_bytes / out.estimated_worker_bytes);
    out.max_in_flight_files = std::min(out.max_in_flight_files, memory_cap);
    out.threads = std::min(out.threads, out.max_in_flight_files);

    // This is a safety ceiling, not a fixed allocation. The v12 builder sizes
    // every basket from its actual value count and the requested false-positive
    // rate (1% by default). Set bloom_bytes := 0 when equality/IN pruning is not
    // worth the index space for a particular dataset.
    out.bloom_bytes = 64U * 1024U;
    return out;
}

void RegisterRootRuntimeSettings(ExtensionLoader& loader) {
    auto& config = DBConfig::GetConfig(loader.GetDatabaseInstance());
    config.AddExtensionOption("root_max_in_flight_files",
                              "Maximum concurrently open ROOT files; 0 derives the value from SET threads",
                              LogicalType::UBIGINT, Value::UBIGINT(0));
    config.AddExtensionOption("root_memory_limit",
                              "Memory admission budget for ROOT readers/indexers (e.g. '8GB' or 'auto')",
                              LogicalType::VARCHAR, Value("auto"));
}

} // namespace duckdb::rootlake
