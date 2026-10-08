#ifndef ROOT4DUCKDB_CORE_ROOT_INPUT_RESOLVER_HPP
#define ROOT4DUCKDB_CORE_ROOT_INPUT_RESOLVER_HPP

#include "duckdb/main/client_context.hpp"

#include <string>
#include <vector>

namespace duckdb::rootlake
{

/// Resolve input syntax without moving credentials into query plans.
class RootInputResolver final
{
  public:
    explicit RootInputResolver(ClientContext& context);

    std::vector<std::string> Resolve(const std::string& input) const;

    static bool IsRootFileName(const std::string& path);
    static bool HasGlob(const std::string& path);
    static bool IsRemoteUri(const std::string& path);
    static bool IsS3Uri(const std::string& path);

  private:
    ClientContext& context_;
};

/// Compatibility wrappers for existing call sites. New code should prefer the
/// explicit RootInputResolver object so source policy has one owner.
std::vector<std::string> ResolveRootInputs(ClientContext& context, const std::string& input);
bool IsRootFileName(const std::string& path);
bool HasRootGlob(const std::string& path);

} // namespace duckdb::rootlake

#endif
