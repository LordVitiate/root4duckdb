#include "root_headers.hpp"

#include "root_dictionary.hpp"
#include "root_debug.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/file_system.hpp"
#include "duckdb/main/client_context.hpp"

#include <mutex>

namespace duckdb::rootlake {
namespace {

std::mutex &DictionaryMutex() {
    static std::mutex mutex;
    return mutex;
}

bool Load(const std::string &path, bool detailed_debug) {
    if (path.empty()) return false;
    if (detailed_debug) RootDebug("DICT.MUTEX_WAIT", "path=" + path);
    std::lock_guard<std::mutex> lock(DictionaryMutex());
    if (detailed_debug) {
        RootDebug("DICT.BEFORE_LOAD",
                  "path=" + path + " gSystem_ptr=" + RootPointer(gSystem));
    }
    const Long64_t result = gSystem->Load(path.c_str());
    if (detailed_debug) {
        RootDebug("DICT.AFTER_LOAD",
                  "path=" + path + " result=" + std::to_string(result));
    }
    if (result < 0) throw IOException("Failed to load ROOT dictionary: " + path);
    return true;
}

} // namespace

bool LoadRootDictionary(ClientContext &context, const std::string &path) {
    if (path.empty()) return false;
    RootDebug("DICT.REQUEST", "path=" + path);
    auto &file_system = FileSystem::GetFileSystem(context);
    const bool exists = file_system.FileExists(path);
    RootDebug("DICT.FILE_CHECK",
              "path=" + path + " exists=" +
                  std::to_string(exists ? 1 : 0));
    if (!exists) {
        throw IOException("Dictionary file not found: " + path);
    }
    return Load(path, true);
}

bool LoadRootDictionary(const std::string &path) {
    return Load(path, false);
}

} // namespace duckdb::rootlake
