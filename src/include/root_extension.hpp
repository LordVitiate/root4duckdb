#pragma once

#include "duckdb.hpp"
#include "duckdb/main/extension/extension_loader.hpp"

namespace duckdb {

class RootExtension : public Extension {
public:
    void Load(ExtensionLoader &loader) override;
    std::string Name() override { return "root"; }
    std::string Version() const override {
#ifdef EXT_VERSION_ROOT
        return EXT_VERSION_ROOT;
#else
        return "";
#endif
    }
};

} // namespace duckdb
