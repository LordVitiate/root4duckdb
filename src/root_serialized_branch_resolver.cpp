#include "root_serialized_branch_resolver.hpp"

#include "root_debug.hpp"
#include "root_lake_common.hpp"

#include "TBranch.h"
#include "TBranchElement.h"
#include "TClass.h"
#include "TStreamerElement.h"
#include "TStreamerInfo.h"
#include "TVirtualCollectionProxy.h"

#include <string>
#include <vector>

namespace duckdb::rootlake {
namespace {

bool BranchMemberMatches(TBranchElement *candidate,
                         TStreamerInfo *outer_streamer,
                         TStreamerElement *target_element,
                         int target_index) {
    if (!candidate || !target_element || !HasPersistentBaskets(candidate)) return false;
    auto *info = candidate->GetInfo();
    if (!info) return false;
    auto *elements = info->GetElements();
    const int id = candidate->GetID();
    if (!elements || id < 0 || id >= elements->GetEntries()) return false;
    auto *element = dynamic_cast<TStreamerElement *>(elements->At(id));
    if (!element || element->IsBase()) return false;

    const std::string wanted_name = target_element->GetName() ? target_element->GetName() : "";
    const std::string wanted_type = TrimType(
        target_element->GetTypeName() ? target_element->GetTypeName() : "");
    const std::string actual_name = element->GetName() ? element->GetName() : "";
    const std::string actual_type = TrimType(element->GetTypeName() ? element->GetTypeName() : "");
    if (actual_name != wanted_name || actual_type != wanted_type) return false;
    if (info == outer_streamer && id != target_index) return false;
    return true;
}

TStreamerElement *BranchStreamerElement(TBranchElement *branch) {
    if (!branch) return nullptr;
    auto *info = branch->GetInfo();
    auto *elements = info ? info->GetElements() : nullptr;
    const int id = branch->GetID();
    if (!elements || id < 0 || id >= elements->GetEntries()) return nullptr;
    return dynamic_cast<TStreamerElement *>(elements->At(id));
}

TClass *StreamerCollectionValueClass(TStreamerElement *element) {
    if (!element) return nullptr;
    auto *collection_class = element->GetClassPointer();
    auto *proxy = collection_class ? collection_class->GetCollectionProxy() : nullptr;
    return proxy ? proxy->GetValueClass() : nullptr;
}

bool StreamerCollectionMatches(TStreamerElement *element, TClass *outer_element_class) {
    if (!element || !outer_element_class) return false;
    auto *value_class = StreamerCollectionValueClass(element);
    return value_class && value_class->GetName() && outer_element_class->GetName() &&
           std::string(value_class->GetName()) == std::string(outer_element_class->GetName());
}

bool CountBranchMatchesOuter(TBranchElement *count, TClass *outer_element_class) {
    if (!count || !outer_element_class || !HasPersistentBaskets(count)) return false;
    // For split object members ROOT may expose the TBranchElement itself as type=0
    // with no branch-level collection proxy.  The authoritative STL type lives in
    // the streamer's member slot selected by GetInfo()+GetID().
    return StreamerCollectionMatches(BranchStreamerElement(count), outer_element_class);
}

bool IsOuterVectorBranch(TBranch *branch, TClass *outer_element_class) {
    auto *element = dynamic_cast<TBranchElement *>(branch);
    if (!element || !outer_element_class || !HasPersistentBaskets(element)) return false;
    return element->GetBranchCount() == nullptr &&
           StreamerCollectionMatches(BranchStreamerElement(element), outer_element_class);
}

bool OuterBranchMatches(TBranchElement *candidate,
                        TClass *outer_element_class,
                        TStreamerInfo *root_streamer,
                        TStreamerElement *outer_element,
                        int outer_index) {
    if (!candidate || !outer_element_class || !outer_element) return false;
    if (!IsOuterVectorBranch(candidate, outer_element_class)) return false;

    auto *info = candidate->GetInfo();
    auto *elements = info ? info->GetElements() : nullptr;
    const int id = candidate->GetID();
    if (!elements || id < 0 || id >= elements->GetEntries()) return false;
    auto *actual = dynamic_cast<TStreamerElement *>(elements->At(id));
    if (!actual || actual->IsBase()) return false;

    const std::string wanted_name = outer_element->GetName() ? outer_element->GetName() : "";
    const std::string wanted_type = TrimType(
        outer_element->GetTypeName() ? outer_element->GetTypeName() : "");
    const std::string actual_name = actual->GetName() ? actual->GetName() : "";
    const std::string actual_type = TrimType(actual->GetTypeName() ? actual->GetTypeName() : "");
    if (actual_name != wanted_name || actual_type != wanted_type) return false;

    // When ROOT preserved the root-class StreamerInfo identity, require the exact
    // member slot too. Older/on-file StreamerInfo instances may be distinct
    // pointers while describing the same member; name+type remains the fallback.
    if (root_streamer && info == root_streamer && id != outer_index) return false;
    return true;
}

void DebugBranch(const std::string &logical_path,
                 TBranchElement *candidate,
                 int target_index,
                 const std::string &target_name) {
    if (!candidate) return;
    auto *count = candidate->GetBranchCount();
    auto *info = candidate->GetInfo();
    TStreamerElement *element = nullptr;
    if (info && info->GetElements() && candidate->GetID() >= 0 &&
        candidate->GetID() < info->GetElements()->GetEntries()) {
        element = dynamic_cast<TStreamerElement *>(info->GetElements()->At(candidate->GetID()));
    }
    const std::string member = element && element->GetName() ? element->GetName() : "";
    if (!count && candidate->GetID() != target_index && member != target_name) return;

    auto *proxy = candidate->GetCollectionProxy();
    auto *value_class = proxy ? proxy->GetValueClass() : nullptr;
    auto *stream_value_class = StreamerCollectionValueClass(element);
    RootDebug("SERIALIZED.PLAN_BRANCH",
              "path=" + logical_path +
              " branch=" + (candidate->GetName() ? std::string(candidate->GetName()) : "") +
              " full=" + PersistentBranchIdentity(candidate) +
              " persistent=" + (HasPersistentBaskets(candidate) ? "true" : "false") +
              " type=" + std::to_string(candidate->GetType()) +
              " id=" + std::to_string(candidate->GetID()) +
              " member=" + member +
              " member_type=" + (element && element->GetTypeName()
                                      ? std::string(element->GetTypeName()) : "") +
              " count=" + (count ? PersistentBranchIdentity(count) : "none") +
              " value_class=" + (value_class && value_class->GetName()
                                      ? std::string(value_class->GetName()) : "none") +
              " streamer_value_class=" +
                  (stream_value_class && stream_value_class->GetName()
                       ? std::string(stream_value_class->GetName()) : "none"));
}

} // namespace

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
    bool nested_primitive_vector) {
    SerializedPhysicalBranchMatch result;
    if (!hint_branch || !outer_element_class || !root_streamer || !outer_element ||
        !outer_streamer || !target_element) {
        result.reason = "missing ROOT structural metadata for serialized branch resolution";
        return result;
    }

    result.outer_vector = OuterBranchMatches(
        dynamic_cast<TBranchElement *>(hint_branch), outer_element_class,
        root_streamer, outer_element, outer_index);
    if (!nested_primitive_vector && result.outer_vector) {
        result.payload_branch = hint_branch;
        return result;
    }

    auto *selected = dynamic_cast<TBranchElement *>(hint_branch);
    if (BranchMemberMatches(selected, outer_streamer, target_element, target_index) &&
        CountBranchMatchesOuter(selected->GetBranchCount(), outer_element_class)) {
        result.payload_branch = selected;
        result.count_branch = selected->GetBranchCount();
        result.split_collection_member = true;
        return result;
    }

    TBranch *mother = hint_branch->GetMother();
    if (!mother) mother = hint_branch;
    std::vector<TBranch *> all;
    CollectBranchTree(mother, all);

    // Resolve the semantic outer vector independently from the heuristic hint.
    // FindPhysicalBranch may legally choose an unrelated same-named sibling
    // (e.g. PaEvent::vecVertex vs PaParticle::vecVertex); the serialized planner
    // must recover PaEvent::vecParticle from StreamerInfo + collection type.
    std::vector<TBranchElement *> outer_matches;
    for (auto *branch : all) {
        auto *candidate = dynamic_cast<TBranchElement *>(branch);
        if (!OuterBranchMatches(candidate, outer_element_class, root_streamer,
                                outer_element, outer_index)) {
            continue;
        }
        outer_matches.push_back(candidate);
        RootDebug("SERIALIZED.PLAN_OUTER_CANDIDATE",
                  "path=" + logical_path +
                  " branch=" + PersistentBranchIdentity(candidate) +
                  " type=" + std::to_string(candidate->GetType()) +
                  " id=" + std::to_string(candidate->GetID()));
    }

    TBranchElement *resolved_outer = nullptr;
    if (outer_matches.size() == 1) {
        resolved_outer = outer_matches.front();
        result.outer_vector = true;
    } else if (outer_matches.size() > 1) {
        result.reason = "multiple physical branches match the requested outer vector member";
        return result;
    } else if (result.outer_vector) {
        resolved_outer = dynamic_cast<TBranchElement *>(hint_branch);
    }

    if (!nested_primitive_vector) {
        if (resolved_outer) result.payload_branch = resolved_outer;
        else result.reason = "no physical branch matches the requested outer vector member";
        return result;
    }

    std::vector<TBranchElement *> matches;
    const std::string target_name = target_element->GetName() ? target_element->GetName() : "";
    for (auto *branch : all) {
        auto *candidate = dynamic_cast<TBranchElement *>(branch);
        if (!candidate) continue;
        DebugBranch(logical_path, candidate, target_index, target_name);
        if (!BranchMemberMatches(candidate, outer_streamer, target_element, target_index)) continue;
        if (!CountBranchMatchesOuter(candidate->GetBranchCount(), outer_element_class)) continue;
        if (resolved_outer && candidate->GetBranchCount() != resolved_outer) continue;
        matches.push_back(candidate);
        RootDebug("SERIALIZED.PLAN_CANDIDATE",
                  "path=" + logical_path +
                  " branch=" + PersistentBranchIdentity(candidate) +
                  " type=" + std::to_string(candidate->GetType()) +
                  " id=" + std::to_string(candidate->GetID()) +
                  " count=" + PersistentBranchIdentity(candidate->GetBranchCount()));
    }

    if (matches.size() == 1) {
        result.payload_branch = matches.front();
        result.count_branch = matches.front()->GetBranchCount();
        result.split_collection_member = true;
        return result;
    }
    if (matches.size() > 1) {
        result.reason = "multiple split collection branches match the requested streamer member";
        return result;
    }

    if (resolved_outer) {
        result.payload_branch = resolved_outer;
        result.outer_vector = true;
        return result;
    }

    auto *hint_element = dynamic_cast<TBranchElement *>(hint_branch);
    RootDebug("SERIALIZED.PLAN_REJECT",
              "path=" + logical_path +
              " selected=" + PersistentBranchIdentity(hint_branch) +
              " selected_type=" + std::to_string(hint_element ? hint_element->GetType() : -1) +
              " selected_id=" + std::to_string(hint_element ? hint_element->GetID() : -1) +
              " selected_count=" +
                  (hint_element && hint_element->GetBranchCount()
                       ? PersistentBranchIdentity(hint_element->GetBranchCount()) : "none"));
    result.reason = "no physical branch is structurally linked to the requested vector member";
    return result;
}

} // namespace duckdb::rootlake
