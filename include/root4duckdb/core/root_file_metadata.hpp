#ifndef ROOT4DUCKDB_CORE_ROOT_FILE_METADATA_HPP
#define ROOT4DUCKDB_CORE_ROOT_FILE_METADATA_HPP

#include <cstdint>
#include <string>
#include <sys/stat.h>

namespace duckdb::rootlake
{

bool LocalFileStat(const std::string& path, struct stat& status);
uint64_t LocalFileSize(const std::string& path);
int64_t LocalMtimeNS(const std::string& path);

} // namespace duckdb::rootlake

#endif
