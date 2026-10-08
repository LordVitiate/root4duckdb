#include "root4duckdb/core/root_headers.hpp"

#include "root4duckdb/core/root_dictionary.hpp"
#include "root4duckdb/core/root_debug.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/file_system.hpp"
#include "duckdb/main/client_context.hpp"

#include <dlfcn.h>
#include <link.h>
#include <mutex>
#include <set>

namespace duckdb::rootlake
{
namespace
{

std::mutex& DictionaryMutex()
{
    static std::mutex mutex;
    return mutex;
}

std::set<std::string>& LoadedDictionaryPaths()
{
    static std::set<std::string> paths;
    return paths;
}

int LocateRootCoreLibrary(dl_phdr_info* info, size_t, void* context)
{
    if (!info || !info->dlpi_name || !*info->dlpi_name)
    {
        return 0;
    }
    const std::string path(info->dlpi_name);
    const auto separator = path.find_last_of('/');
    const std::string name = path.substr(separator == std::string::npos ? 0 : separator + 1);
    if (name != "libCore.so" && name.rfind("libCore.so.", 0) != 0)
    {
        return 0;
    }
    *static_cast<std::string*>(context) = path;
    return 1;
}

bool LoadRootGeometryDependency(std::string& error)
{
    std::string root_library;
    dl_iterate_phdr(LocateRootCoreLibrary, &root_library);
    const auto separator = root_library.find_last_of('/');
    if (separator == std::string::npos)
    {
        error = "cannot resolve the directory of the loaded ROOT libCore.so";
        return false;
    }
    const std::string geometry_library = root_library.substr(0, separator + 1) + "libGeom.so";
    dlerror();
    if (!dlopen(geometry_library.c_str(), RTLD_NOW | RTLD_GLOBAL))
    {
        const char* message = dlerror();
        error = "cannot load " + geometry_library + ": " +
                (message ? std::string(message) : std::string("dynamic loader error"));
        return false;
    }
    return true;
}

bool Load(const std::string& path, bool detailed_debug)
{
    if (path.empty())
    {
        return false;
    }
    if (detailed_debug)
    {
        RootDebug("DICT.MUTEX_WAIT", "path=" + path);
    }
    const std::lock_guard<std::mutex> lock(DictionaryMutex());
    if (LoadedDictionaryPaths().find(path) != LoadedDictionaryPaths().end())
    {
        return true;
    }
    if (detailed_debug)
    {
        RootDebug("DICT.BEFORE_LOAD", "path=" + path);
    }
    // Loading a rootcling dictionary runs its module-registration initializer.
    // Keep the handle alive for the process lifetime: ROOT stores class
    // metadata and collection proxies that refer to code in this library.
    // Never dlclose a dictionary while ROOT may still reference its classes.
    // External experiment dictionaries may refer to optional reconstruction functions.
    // Resolve functions when used; data decoding needs their class registration, not
    // every external function in the experiment software. Data-symbol relocations
    // still resolve immediately and genuinely missing functions remain unusable.
    dlerror();
    void* handle = dlopen(path.c_str(), RTLD_LAZY | RTLD_GLOBAL);
    if (!handle)
    {
        const char* message = dlerror();
        const std::string failure = message ? message : "dynamic loader error";
        if (failure.find("undefined symbol: gGeoManager") != std::string::npos)
        {
            std::string dependency_error;
            if (!LoadRootGeometryDependency(dependency_error))
            {
                throw IOException("Failed to load ROOT dictionary " + path + ": " + failure +
                                  "; " + dependency_error);
            }
            dlerror();
            handle = dlopen(path.c_str(), RTLD_LAZY | RTLD_GLOBAL);
        }
        if (!handle)
        {
            message = dlerror();
            throw IOException("Failed to load ROOT dictionary " + path + ": " +
                              (message ? std::string(message) : failure));
        }
    }
    LoadedDictionaryPaths().insert(path);
    if (detailed_debug)
    {
        RootDebug("DICT.AFTER_LOAD", "path=" + path + " result=ok");
    }
    return true;
}

} // namespace

bool LoadRootDictionary(ClientContext& context, const std::string& path)
{
    if (path.empty())
    {
        return false;
    }
    RootDebug("DICT.REQUEST", "path=" + path);
    auto& file_system = FileSystem::GetFileSystem(context);
    const bool exists = file_system.FileExists(path);
    RootDebug("DICT.FILE_CHECK", "path=" + path + " exists=" + std::to_string(exists ? 1 : 0));
    if (!exists)
    {
        throw IOException("Dictionary file not found: " + path);
    }
    return Load(path, true);
}

bool LoadRootDictionary(const std::string& path)
{
    return Load(path, false);
}

} // namespace duckdb::rootlake
