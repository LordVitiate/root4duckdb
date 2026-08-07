#include "root_runtime_settings.hpp"
#include "root_iceberg_catalog.hpp"
#define DUCKDB_EXTENSION_MAIN

#include "TROOT.h"

#ifdef BIT
#undef BIT
#endif

#include "include/root_extension.hpp"
#include "duckdb/main/extension/extension_loader.hpp"

namespace duckdb {

void RegisterRootMetaGenerator(ExtensionLoader &loader);
void RegisterRootScan(ExtensionLoader &loader);

namespace rootlake {
void RegisterRootLakeIndex(ExtensionLoader &loader);
void RegisterRootLakeScan(ExtensionLoader &loader);
} // namespace rootlake

void LoadRootInternal(ExtensionLoader &loader) {
    ROOT::EnableThreadSafety();

    // Backward-compatible single-file API.
    RegisterRootMetaGenerator(loader);
    RegisterRootScan(loader);

    // Versioned Parquet/Iceberg-backed deep index API.
    rootlake::RegisterRootRuntimeSettings(loader);
    rootlake::RegisterRootLakeIndex(loader);
    rootlake::RegisterRootLakeScan(loader);
    rootlake::RegisterRootIcebergCatalog(loader);
}

void RootExtension::Load(ExtensionLoader &loader) {
    LoadRootInternal(loader);
}

} // namespace duckdb

extern "C" {

DUCKDB_CPP_EXTENSION_ENTRY(root, loader) {
    duckdb::LoadRootInternal(loader);
}

}
