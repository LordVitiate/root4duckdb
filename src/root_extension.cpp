#include "root4duckdb/core/root_headers.hpp"
#include "root4duckdb/core/root_debug.hpp"

#include "root4duckdb/core/root_runtime_settings.hpp"
#include "root4duckdb/direct/root_describe.hpp"
#include "root4duckdb/iceberg/root_iceberg_catalog.hpp"
#define DUCKDB_EXTENSION_MAIN

#include "root4duckdb/core/root_extension.hpp"
#include "duckdb/main/extension/extension_loader.hpp"
#include "duckdb/main/capi/capi_internal.hpp"

#include <mutex>

namespace duckdb
{

void RegisterRootScan(ExtensionLoader& loader);

namespace rootlake
{
void RegisterRootLakeIndex(ExtensionLoader& loader);
void RegisterRootLakeScan(ExtensionLoader& loader);
} // namespace rootlake

namespace
{

std::mutex& RootExtensionLoadMutex()
{
    static std::mutex mutex;
    return mutex;
}

void InitializeRootRuntime()
{
    // ROOT owns its runtime initialization. Do not reset process-wide signal
    // handlers from a DuckDB extension: other host threads may rely on them.
    ROOT::EnableThreadSafety();
}

} // namespace

void LoadRootInternal(ExtensionLoader& loader)
{
    // Multiple DuckDB databases can LOAD the same extension concurrently.
    // Register each database independently while serializing ROOT startup.
    const std::lock_guard<std::mutex> lock(RootExtensionLoadMutex());
    rootlake::RootDebug("LOAD.ENTER", "before ROOT::EnableThreadSafety");
    static std::once_flag root_runtime_once;
    std::call_once(root_runtime_once, InitializeRootRuntime);
    rootlake::RootDebug("LOAD.RUNTIME_READY", "before RegisterRootScan");
    RegisterRootScan(loader);
    rootlake::RootDebug("LOAD.SCAN_READY", "before RegisterRootDescribe");
    RegisterRootDescribe(loader);
    rootlake::RootDebug("LOAD.DESCRIBE_READY", "before runtime settings");
    // Versioned Parquet/Iceberg-backed deep index API.
    rootlake::RegisterRootRuntimeSettings(loader);
    rootlake::RootDebug("LOAD.SETTINGS_READY", "before index registration");
    rootlake::RegisterRootLakeIndex(loader);
    rootlake::RootDebug("LOAD.INDEX_READY", "before dataset scan registration");
    rootlake::RegisterRootLakeScan(loader);
    rootlake::RootDebug("LOAD.DATASET_READY", "before Iceberg registration");
    rootlake::RegisterRootIcebergCatalog(loader);
    rootlake::RootDebug("LOAD.COMPLETE", "all functions registered");
}

#ifndef ROOT4DUCKDB_PAYLOAD_ONLY
void RootExtension::Load(ExtensionLoader& loader)
{
    LoadRootInternal(loader);
}

std::string RootExtension::Name()
{
    return "root";
}

std::string RootExtension::Version() const
{
#ifdef EXT_VERSION_ROOT
    return EXT_VERSION_ROOT;
#else
    return "";
#endif
}

#endif // ROOT4DUCKDB_PAYLOAD_ONLY

} // namespace duckdb

extern "C"
{

    DUCKDB_CPP_EXTENSION_ENTRY(root, loader)
    {
        duckdb::LoadRootInternal(loader);
    }
    DUCKDB_EXTENSION_API bool root4duckdb_payload_init_c(duckdb_extension_info info,
                                                         struct duckdb_extension_access* access)
    {
        if (!info || !access || !access->get_database || !access->set_error)
        {
            return false;
        }
        try
        {
            duckdb_database* database_handle = access->get_database(info);
            if (!database_handle)
            {
                access->set_error(info, "ROOT4DuckDB payload could not obtain the host DuckDB database");
                return false;
            }
            // Match DuckDB C API DatabaseWrapper layout for the pinned ABI.
            duckdb::rootlake::RootDebug("PAYLOAD.DB_HANDLE", "before DuckDB wrapper inspection");
            auto* wrapper = reinterpret_cast<duckdb::DatabaseWrapper*>(database_handle);
            if (!wrapper || !wrapper->database || !wrapper->database->instance)
            {
                access->set_error(info, "ROOT4DuckDB payload received an invalid host DuckDB database");
                return false;
            }
            duckdb::rootlake::RootDebug("PAYLOAD.WRAPPER_READY", "before ExtensionLoader construction");
            duckdb::ExtensionLoader loader(*wrapper->database->instance, "root");
            duckdb::rootlake::RootDebug("PAYLOAD.LOADER_READY", "before function registration");
            duckdb::LoadRootInternal(loader);
            return true;
        }
        catch (const std::exception& ex)
        {
            access->set_error(info, ex.what());
            return false;
        }
        catch (...)
        {
            access->set_error(info, "ROOT4DuckDB payload initialization failed with an unknown exception");
            return false;
        }
    }
}
