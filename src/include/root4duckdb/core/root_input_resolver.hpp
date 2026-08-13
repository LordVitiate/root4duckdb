#pragma once

#include "duckdb/main/client_context.hpp"

#include <string>
#include <vector>

namespace duckdb::rootlake {

/// Expands file, directory, glob, JSON, comma, and @file specifications.
std::vector<std::string> ResolveRootInputs(ClientContext& context, const std::string& input);

/// Checks supported ROOT suffixes, including numbered parts.
bool IsRootFileName(const std::string& path);
/// Reports whether an input contains a filesystem glob.
bool HasRootGlob(const std::string& path);

} // namespace duckdb::rootlake
