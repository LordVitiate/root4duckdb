#include "root_serialized_reader.hpp"

#include "TBranch.h"
#include "TClass.h"
#include "TStreamerElement.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <mutex>
#include <set>
#include <utility>

namespace duckdb::rootlake {

namespace {

bool EnvironmentFlagEnabled(const char *name, bool default_value) {
    const char *raw = std::getenv(name);
    if (!raw || !*raw) return default_value;
    std::string value(raw);
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value != "0" && value != "false" && value != "off" && value != "no";
}

uint64_t FixedArrayLength(TStreamerElement *element,
                          std::vector<uint32_t> *dimensions = nullptr) {
    if (!element) return 0;
    const int rank = element->GetArrayDim();
    if (rank <= 0) return 1;
    uint64_t length = 1;
    for (int dim = 0; dim < rank; ++dim) {
        const int extent = element->GetMaxIndex(dim);
        if (extent <= 0 ||
            length > std::numeric_limits<uint64_t>::max() / static_cast<uint64_t>(extent)) {
            return 0;
        }
        if (dimensions) dimensions->push_back(static_cast<uint32_t>(extent));
        length *= static_cast<uint64_t>(extent);
    }
    return length;
}

bool FixedPrimitiveWidth(TStreamerElement *element, uint32_t &width,
                         std::string &reason) {
    if (!element) {
        reason = "null streamer element";
        return false;
    }
    if (element->IsaPointer()) {
        reason = "pointer before projected member";
        return false;
    }
    const std::string type = element->GetTypeName() ? element->GetTypeName() : "";
    if (type == "Float16_t" || type == "Double32_t") {
        reason = "compressed floating streamer element before projected member";
        return false;
    }
    const auto primitive = PrimitiveTypeSize(type);
    const auto length = FixedArrayLength(element);
    if (!primitive || !length ||
        length > std::numeric_limits<uint32_t>::max() / primitive) {
        reason = "variable-width or unsupported streamer element before projected member: " +
                 std::string(element->GetName());
        return false;
    }
    width = primitive * static_cast<uint32_t>(length);
    return true;
}

} // namespace

RootReaderMode ParseRootReaderMode(std::string mode) {
    std::transform(mode.begin(), mode.end(), mode.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (mode.empty() || mode == "auto") return RootReaderMode::AUTO;
    if (mode == "serialized" || mode == "raw" || mode == "vectorized") {
        return RootReaderMode::SERIALIZED;
    }
    if (mode == "object" || mode == "fallback" || mode == "universal") {
        return RootReaderMode::OBJECT;
    }
    throw InvalidInputException("reader_mode must be one of: auto, serialized, object");
}

const char *RootReaderModeName(RootReaderMode mode) {
    switch (mode) {
    case RootReaderMode::AUTO: return "auto";
    case RootReaderMode::SERIALIZED: return "serialized";
    case RootReaderMode::OBJECT: return "object";
    }
    return "unknown";
}

SerializedReadPlan BuildSerializedReadPlan(TClass *root_class, const ParsedPath &path,
                                           TBranch *physical_branch) {
    SerializedReadPlan plan;
    plan.logical_path = "/" + path.root_class + "/" + JoinStrings(path.fields, "/");
    plan.root_class = path.root_class;
    plan.physical_branch_name = physical_branch && physical_branch->GetName()
                                    ? physical_branch->GetName() : "";
    auto reject = [&](std::string reason) {
        plan.supported = false;
        plan.reason = std::move(reason);
        return plan;
    };

    if (!root_class || !physical_branch) return reject("missing ROOT class or physical branch");
    if (physical_branch->GetListOfBranches() &&
        physical_branch->GetListOfBranches()->GetEntries() > 0) {
        return reject("physical ancestor is split into persistent child branches");
    }
    if (path.fields.size() != 2) {
        return reject("serialized reader currently requires vector<object>/primitive-member");
    }

    std::vector<PathLevel> levels;
    try {
        levels = PathResolver::Resolve(root_class, path.fields);
    } catch (const std::exception &ex) {
        return reject(std::string("cannot resolve serialized path: ") + ex.what());
    }
    if (levels.size() != 2 || !levels[0].is_container ||
        !IsContiguousVectorType(levels[0].type) || levels[0].is_pointer ||
        !levels[0].element_class) {
        return reject("outer field is not std::vector<object>");
    }
    if (!levels[1].is_primitive || levels[1].is_pointer || levels[1].is_container) {
        return reject("terminal field is not a fixed primitive member");
    }
    if (!BranchNameEndsWithToken(plan.physical_branch_name, levels[0].name)) {
        return reject("physical branch is not the vector ancestor");
    }

    auto *element_class = levels[0].element_class;
    auto *streamer = element_class->GetStreamerInfo();
    if (!streamer) return reject("vector element class has no TStreamerInfo");
    auto *elements = streamer->GetElements();
    if (!elements) return reject("vector element streamer has no elements");

    uint64_t prefix_width = 0;
    TStreamerElement *target = nullptr;
    for (int i = 0; i < elements->GetEntries(); ++i) {
        auto *element = dynamic_cast<TStreamerElement *>(elements->At(i));
        if (!element) return reject("unexpected non-streamer element");
        if (!element->IsBase() && path.fields[1] == element->GetName()) {
            target = element;
            break;
        }
        if (element->IsBase()) {
            const std::string base_type = TrimType(
                element->GetTypeName() ? element->GetTypeName() : "");
            // TObject contributes its version marker, unique id and bits in
            // the observed member-wise representation. Per-file validation is
            // mandatory before this constant is trusted.
            if (base_type == "TObject") {
                prefix_width += 10;
                continue;
            }
            return reject("unsupported base class before projected member: " + base_type);
        }
        uint32_t element_width = 0;
        std::string reason;
        if (!FixedPrimitiveWidth(element, element_width, reason)) return reject(reason);
        prefix_width += element_width;
        if (prefix_width > std::numeric_limits<uint32_t>::max()) {
            return reject("serialized member prefix exceeds 32-bit offset");
        }
    }
    if (!target) return reject("terminal member is inherited or absent from element streamer");

    uint32_t target_width = 0;
    std::string target_reason;
    if (!FixedPrimitiveWidth(target, target_width, target_reason)) return reject(target_reason);
    std::vector<uint32_t> dimensions;
    const auto array_length = FixedArrayLength(target, &dimensions);
    const auto scalar_width = PrimitiveTypeSize(target->GetTypeName());
    if (!array_length || !scalar_width || target_width != array_length * scalar_width) {
        return reject("terminal member has unsupported persistent width");
    }

    plan.supported = true;
    plan.reason.clear();
    plan.container_name = levels[0].name;
    plan.element_class = element_class->GetName();
    plan.value_type = PrimitiveBaseType(target->GetTypeName());
    plan.schema_fingerprint = SchemaFingerprint(path.root_class, levels);
    plan.streamer_version = static_cast<uint32_t>(
        std::max(0, static_cast<int>(element_class->GetClassVersion())));
    plan.bytes_before_value_per_element = static_cast<uint32_t>(prefix_width);
    plan.value_bytes = scalar_width;
    plan.fixed_array_length = array_length;
    plan.array_dimensions = std::move(dimensions);
    plan.index_depth = 1 +
        (array_length > 1 ? std::max<idx_t>(1, plan.array_dimensions.size()) : 0);
    return plan;
}

void WarnRootFallbackOnce(const std::string &logical_path,
                          const std::string &schema_fingerprint,
                          const std::string &reason) {
    if (!EnvironmentFlagEnabled("ROOT4DUCKDB_FALLBACK_WARNINGS", true)) return;
    static std::mutex mutex;
    static std::set<std::string> emitted;
    const std::string key = logical_path + "\x1f" + schema_fingerprint + "\x1f" + reason;
    std::lock_guard<std::mutex> guard(mutex);
    if (!emitted.insert(key).second) return;
    std::fprintf(stderr,
                 "[ROOT4DUCKDB][WARN][ROOT_OBJECT_FALLBACK] path=%s schema=%s reason=%s\n",
                 logical_path.c_str(),
                 schema_fingerprint.empty() ? "unknown" : schema_fingerprint.c_str(),
                 reason.c_str());
    std::fflush(stderr);
}

} // namespace duckdb::rootlake
