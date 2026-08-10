#pragma once

#include <string>

class TBranch;
class TBranchElement;
class TClass;
class TStreamerElement;
class TStreamerInfo;

namespace duckdb::rootlake {

struct SerializedPhysicalBranchMatch {
    TBranch *payload_branch = nullptr;
    TBranchElement *count_branch = nullptr;
    bool outer_vector = false;
    bool split_collection_member = false;
    std::string reason;
};

// Resolve a serialized payload from ROOT structural metadata rather than branch
// naming conventions. For nested vector members, streamer element identity and
// GetBranchCount() are authoritative; names are used only in diagnostics.
SerializedPhysicalBranchMatch ResolveSerializedPhysicalBranch(
    TBranch *hint_branch,
    TClass *outer_element_class,
    TStreamerInfo *root_streamer,
    TStreamerElement *outer_element,
    int outer_index,
    TStreamerInfo *outer_streamer,
    TStreamerElement *target_element,
    int target_index,
    const std::string &logical_path,
    bool nested_primitive_vector);

} // namespace duckdb::rootlake
