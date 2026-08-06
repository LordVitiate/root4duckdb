#include "root_debug.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <utility>

namespace duckdb {

namespace {

bool EnvironmentDebugFlag(const char *name) {
    const char *raw = std::getenv(name);
    if (!raw || !*raw) return false;
    std::string value(raw);
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value != "0" && value != "false" && value != "off" && value != "no";
}

bool VerboseStage(const char *stage) {
    if (!stage) return false;
    const std::string name(stage);
    return name == "READ.BEFORE_GET_ENTRY" || name == "READ.AFTER_GET_ENTRY" ||
           name == "VECTOR.CONTIGUOUS" || name == "VECTOR.PROXY" ||
           name == "SERIALIZED.BASKET";
}

std::atomic<uint64_t> debug_sequence {0};
std::atomic<uint64_t> operation_sequence {0};
thread_local uint64_t operation_id = 0;

} // namespace

bool RootDebugEnabled() {
    static const bool enabled = EnvironmentDebugFlag("ROOT4DUCKDB_DEBUG");
    return enabled;
}

bool RootDebugVerboseEnabled() {
    static const bool enabled = EnvironmentDebugFlag("ROOT4DUCKDB_DEBUG_VERBOSE");
    return enabled;
}

void RootDebug(const char *stage, const std::string &message) {
    if (!RootDebugEnabled() || (VerboseStage(stage) && !RootDebugVerboseEnabled())) return;
    const auto sequence = debug_sequence.fetch_add(1, std::memory_order_relaxed) + 1;
    std::ostringstream thread;
    thread << std::this_thread::get_id();
    std::fprintf(stderr,
                 "[ROOT4DUCKDB][seq=%06llu][op=%04llu][thread=%s][%s] %s\n",
                 static_cast<unsigned long long>(sequence),
                 static_cast<unsigned long long>(operation_id),
                 thread.str().c_str(), stage, message.c_str());
    std::fflush(stderr);
}

std::string JoinDebugFields(const std::vector<std::string> &fields) {
    std::ostringstream out;
    for (size_t i = 0; i < fields.size(); ++i) {
        if (i) out << '/';
        out << fields[i];
    }
    return out.str();
}

RootDebugOperationScope::RootDebugOperationScope(std::string label_p)
    : previous_id(operation_id), label(std::move(label_p)) {
    operation_id = operation_sequence.fetch_add(1, std::memory_order_relaxed) + 1;
    RootDebug("OP.ENTER", label);
}

RootDebugOperationScope::~RootDebugOperationScope() {
    RootDebug("OP.LEAVE", label);
    operation_id = previous_id;
}

uint64_t RootDebugOperationScope::Id() const {
    return operation_id;
}

RootDebugLifetimeSentinel::~RootDebugLifetimeSentinel() {
    RootDebug("LIFETIME.MEMBERS_DESTROYED", label);
}

} // namespace duckdb
