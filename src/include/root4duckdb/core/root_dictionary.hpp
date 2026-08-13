#pragma once

#include <string>

namespace duckdb {
class ClientContext;

namespace rootlake {

/// Loads a ROOT dictionary through DuckDB's file-system context.
bool LoadRootDictionary(ClientContext& context, const std::string& path);
/// Loads a local ROOT dictionary directly.
bool LoadRootDictionary(const std::string& path);

} // namespace rootlake
} // namespace duckdb
