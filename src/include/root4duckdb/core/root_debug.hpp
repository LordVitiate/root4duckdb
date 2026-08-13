#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace duckdb {

/// Reports whether stage-level diagnostics are enabled.
bool RootDebugEnabled();
/// Reports whether per-entry diagnostics are enabled.
bool RootDebugVerboseEnabled();
/// Emits one structured diagnostic event when enabled.
void RootDebug(const char* stage, const std::string& message);
/// Joins semantic path components for diagnostics.
std::string JoinDebugFields(const std::vector<std::string>& fields);

/// Formats a pointer without exposing template implementation in the header.
std::string RootPointer(const void* pointer);

/// Associates nested diagnostics with one operation identifier.
class RootDebugOperationScope {
  public:
    explicit RootDebugOperationScope(std::string label);
    ~RootDebugOperationScope();

    uint64_t Id() const;

  private:
    uint64_t previous_id = 0;
    std::string label;
};

/// Records the destruction boundary of an owning state object.
struct RootDebugLifetimeSentinel {
    const char* label = "unknown";
    ~RootDebugLifetimeSentinel();
};

} // namespace duckdb
