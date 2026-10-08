#include "root4duckdb/core/root_headers.hpp"

#include "root4duckdb/core/root_dictionary.hpp"
#include "root4duckdb/core/root_debug.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/file_system.hpp"
#include "duckdb/main/client_context.hpp"

#include <dlfcn.h>
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
    dlerror();
    void* handle = dlopen(path.c_str(), RTLD_NOW | RTLD_GLOBAL);
    if (!handle)
    {
        const char* message = dlerror();
        throw IOException("Failed to load ROOT dictionary " + path + ": " +
                          (message ? std::string(message) : std::string("dynamic loader error")));
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
