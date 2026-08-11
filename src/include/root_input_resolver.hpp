#pragma once

#include "duckdb/main/client_context.hpp"

#include <string>
#include <vector>

namespace duckdb::rootlake {

// Expands a direct file, a directory, a shell-style glob, a comma-separated
// list, a JSON string array, or an @file list into a stable input sequence.
// Glob matches are sorted; explicit list order is preserved.
std::vector<std::string> ResolveRootInputs(ClientContext &context, const std::string &input);

bool IsRootFileName(const std::string &path);
bool HasRootGlob(const std::string &path);

} // namespace duckdb::rootlake
