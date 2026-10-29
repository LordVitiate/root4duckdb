#ifndef ROOT4DUCKDB_CORE_ROOT_SCAN_CONTRACT_HPP
#define ROOT4DUCKDB_CORE_ROOT_SCAN_CONTRACT_HPP

#include "duckdb/function/table_function.hpp"

namespace duckdb::rootlake
{

void ConfigureRootRelationScan(TableFunction& function);

} // namespace duckdb::rootlake

#endif
