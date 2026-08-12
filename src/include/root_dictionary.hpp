#pragma once

#include <string>

namespace duckdb {
class ClientContext;

namespace rootlake {

bool LoadRootDictionary(ClientContext &context, const std::string &path);
bool LoadRootDictionary(const std::string &path);

} // namespace rootlake
} // namespace duckdb
