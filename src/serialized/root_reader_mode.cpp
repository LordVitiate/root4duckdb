#include "root4duckdb/serialized/root_serialized_reader.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <set>

namespace duckdb::rootlake
{
namespace
{
bool EnvironmentFlagEnabled(const char* name, bool default_value)
{
    const char* raw = std::getenv(name);
    if (!raw || !*raw)
    {
        return default_value;
    }
    std::string value(raw);
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c)
                   {
                       return static_cast<char>(std::tolower(c));
                   });
    return value != "0" && value != "false" && value != "off" && value != "no";
}

} // namespace

RootReaderMode ParseRootReaderMode(std::string mode)
{
    std::transform(mode.begin(), mode.end(), mode.begin(),
                   [](unsigned char c)
                   {
                       return static_cast<char>(std::tolower(c));
                   });
    if (mode.empty() || mode == "auto")
    {
        return RootReaderMode::AUTO;
    }
    if (mode == "serialized" || mode == "raw" || mode == "vectorized")
    {
        return RootReaderMode::SERIALIZED;
    }
    if (mode == "object" || mode == "fallback" || mode == "universal")
    {
        return RootReaderMode::OBJECT;
    }
    throw InvalidInputException("reader_mode must be one of: auto, serialized, object");
}

const char* RootReaderModeName(RootReaderMode mode)
{
    switch (mode)
    {
    case RootReaderMode::AUTO:
        return "auto";
    case RootReaderMode::SERIALIZED:
        return "serialized";
    case RootReaderMode::OBJECT:
        return "object";
    }
    return "unknown";
}

void WarnRootFallbackOnce(const std::string& logical_path, const std::string& schema_fingerprint,
                          const std::string& reason)
{
    if (!EnvironmentFlagEnabled("ROOT4DUCKDB_FALLBACK_WARNINGS", true))
    {
        return;
    }
    static std::mutex mutex;
    static std::set<std::string> emitted;
    const std::string key = logical_path + "\x1f" + schema_fingerprint + "\x1f" + reason;
    const std::lock_guard<std::mutex> guard(mutex);
    if (!emitted.insert(key).second)
    {
        return;
    }
    std::fprintf(stderr, "[ROOT4DUCKDB][WARN][ROOT_OBJECT_FALLBACK] path=%s schema=%s reason=%s\n",
                 logical_path.c_str(), schema_fingerprint.empty() ? "unknown" : schema_fingerprint.c_str(),
                 reason.c_str());
    std::fflush(stderr);
}

} // namespace duckdb::rootlake
