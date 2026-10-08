#ifndef ROOT4DUCKDB_CORE_ROOT_EXTENSION_HPP
#define ROOT4DUCKDB_CORE_ROOT_EXTENSION_HPP

#include "duckdb.hpp"
#include "duckdb/main/extension/extension_loader.hpp"

namespace duckdb
{

/// DuckDB entry point for the ROOT extension.
class RootExtension : public Extension
{
  public:
    void Load(ExtensionLoader& loader) override;
    std::string Name() override;
    std::string Version() const override;
};

} // namespace duckdb

#endif
