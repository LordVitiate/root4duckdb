#include "root4duckdb/core/root_headers.hpp"

#include "root4duckdb/core/root_runtime_settings.hpp"
#include "root4duckdb/direct/root_describe.hpp"
#include "root4duckdb/iceberg/root_iceberg_catalog.hpp"
#define DUCKDB_EXTENSION_MAIN

#include "root4duckdb/core/root_extension.hpp"
#include "duckdb/main/extension/extension_loader.hpp"

namespace duckdb {

void RegisterRootScan(ExtensionLoader& loader);

namespace rootlake {
void RegisterRootLakeIndex(ExtensionLoader& loader);
void RegisterRootLakeScan(ExtensionLoader& loader);
} // namespace rootlake

void LoadRootInternal(ExtensionLoader& loader) {
    ROOT::EnableThreadSafety();

    RegisterRootScan(loader);
    RegisterRootDescribe(loader);

    // Versioned Parquet/Iceberg-backed deep index API.
    rootlake::RegisterRootRuntimeSettings(loader);
    rootlake::RegisterRootLakeIndex(loader);
    rootlake::RegisterRootLakeScan(loader);
    rootlake::RegisterRootIcebergCatalog(loader);
}

void RootExtension::Load(ExtensionLoader& loader) {
    LoadRootInternal(loader);
}

std::string RootExtension::Name() {
    return "root";
}

std::string RootExtension::Version() const {
#ifdef EXT_VERSION_ROOT
    return EXT_VERSION_ROOT;
#else
    return "";
#endif
}

} // namespace duckdb

extern "C" {

DUCKDB_CPP_EXTENSION_ENTRY(root, loader) {
    duckdb::LoadRootInternal(loader);
}
}
