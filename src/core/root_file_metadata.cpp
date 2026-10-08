#include "root4duckdb/core/root_file_metadata.hpp"

namespace duckdb::rootlake
{

bool LocalFileStat(const std::string& path, struct stat& status)
{
    if (path.find("://") != std::string::npos)
    {
        return false;
    }
    return ::stat(path.c_str(), &status) == 0;
}

uint64_t LocalFileSize(const std::string& path)
{
    struct stat status{};
    if (!LocalFileStat(path, status) || status.st_size < 0)
    {
        return 0;
    }
    return static_cast<uint64_t>(status.st_size);
}

int64_t LocalMtimeNS(const std::string& path)
{
    struct stat status{};
    if (!LocalFileStat(path, status))
    {
        return 0;
    }
#if defined(__APPLE__)
    return static_cast<int64_t>(status.st_mtimespec.tv_sec) * 1000000000LL +
           static_cast<int64_t>(status.st_mtimespec.tv_nsec);
#else
    return static_cast<int64_t>(status.st_mtim.tv_sec) * 1000000000LL + static_cast<int64_t>(status.st_mtim.tv_nsec);
#endif
}

} // namespace duckdb::rootlake
