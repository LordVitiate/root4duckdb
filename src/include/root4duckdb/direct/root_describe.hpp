#pragma once

#include "duckdb/function/table_function.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/extension/extension_loader.hpp"

#include <string>
#include <vector>

namespace duckdb {

/// Registers metadata-only next-level semantic browsing.
void RegisterRootDescribe(ExtensionLoader& loader);

} // namespace duckdb
