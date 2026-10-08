#include "root4duckdb/core/root_headers.hpp"

#include "root4duckdb/core/root_runtime_settings.hpp"
#include "root4duckdb/direct/root_describe.hpp"
#include "root4duckdb/iceberg/root_iceberg_catalog.hpp"
#define DUCKDB_EXTENSION_MAIN

#include "root4duckdb/core/root_extension.hpp"
#include "duckdb/main/extension/extension_loader.hpp"
#include "duckdb/main/capi/capi_internal.hpp"

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

    // ROOT installs process-wide signal handlers while initializing.
    // ROOT4DuckDB is an embedded library, so restore the handlers owned
    // by the host process (DuckDB CLI, JVM, Python, etc.).
    if (gSystem) {
        gSystem->ResetSignals();
    }
}

#ifndef ROOT4DUCKDB_PAYLOAD_ONLY
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

#endif // ROOT4DUCKDB_PAYLOAD_ONLY

} // namespace duckdb

extern "C" {

DUCKDB_CPP_EXTENSION_ENTRY(root, loader) {
    duckdb::LoadRootInternal(loader);
}


DUCKDB_EXTENSION_API bool root4duckdb_payload_init_c(
    duckdb_extension_info info,
    struct duckdb_extension_access *access) {

    if (!info || !access || !access->get_database || !access->set_error) {
        return false;
    }

    try {
        duckdb_database *database_handle = access->get_database(info);
        if (!database_handle) {
            access->set_error(
                info,
                "ROOT4DuckDB payload could not obtain the host DuckDB database");
            return false;
        }

        // This is the same DatabaseWrapper representation used by DuckDB's
        // own C API implementation. The payload is built for this exact
        // DuckDB ABI.
        auto *wrapper =
            reinterpret_cast<duckdb::DatabaseWrapper *>(database_handle);

        if (!wrapper ||
            !wrapper->database ||
            !wrapper->database->instance) {
            access->set_error(
                info,
                "ROOT4DuckDB payload received an invalid host DuckDB database");
            return false;
        }

        duckdb::ExtensionLoader loader(
            *wrapper->database->instance,
            "root");

        duckdb::LoadRootInternal(loader);
        return true;

    } catch (const std::exception &ex) {
        access->set_error(info, ex.what());
        return false;
    } catch (...) {
        access->set_error(
            info,
            "ROOT4DuckDB payload initialization failed with an unknown exception");
        return false;
    }
}
}
