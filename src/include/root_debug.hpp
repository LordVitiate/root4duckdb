#pragma once

#include <cstdint>
#include <sstream>
#include <string>
#include <vector>

namespace duckdb {

bool RootDebugEnabled();
bool RootDebugVerboseEnabled();
void RootDebug(const char *stage, const std::string &message);
std::string JoinDebugFields(const std::vector<std::string> &fields);

template <class T>
std::string RootPointer(T *pointer) {
    std::ostringstream out;
    out << static_cast<const void *>(pointer);
    return out.str();
}

class RootDebugOperationScope {
public:
    explicit RootDebugOperationScope(std::string label);
    ~RootDebugOperationScope();

    uint64_t Id() const;

private:
    uint64_t previous_id = 0;
    std::string label;
};

struct RootDebugLifetimeSentinel {
    const char *label = "unknown";
    ~RootDebugLifetimeSentinel();
};

} // namespace duckdb
