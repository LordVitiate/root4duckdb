#include "root4duckdb/reader/root_object_reader.hpp"

#include "root4duckdb/core/root_headers.hpp"
#include "root4duckdb/core/root_lake_common.hpp"

#include <algorithm>

namespace duckdb::rootlake {

TTree* FindTree(TFile* file, const std::string& tree_name, const std::string& root_class) {
    if (!file) {
        return nullptr;
    }
    if (!tree_name.empty()) {
        TTree* tree = nullptr;
        file->GetObject(tree_name.c_str(), tree);
        if (!tree) {
            throw InvalidInputException("TTree not found: " + tree_name);
        }
        return tree;
    }
    TTree* first = nullptr;
    TIter next(file->GetListOfKeys());
    while (auto* key = dynamic_cast<TKey*>(next())) {
        if (std::string(key->GetClassName()) != "TTree") {
            continue;
        }
        auto* tree = dynamic_cast<TTree*>(file->Get(key->GetName()));
        if (!tree) {
            continue;
        }
        if (!first) {
            first = tree;
        }
        if (root_class.empty()) {
            return tree;
        }
        auto* branches = tree->GetListOfBranches();
        for (int i = 0; branches && i < branches->GetEntries(); ++i) {
            auto* branch = dynamic_cast<TBranchElement*>(branches->At(i));
            if (branch && branch->GetClassName() && root_class == branch->GetClassName()) {
                return tree;
            }
        }
    }
    return root_class.empty() ? first : nullptr;
}

TBranch* FindObjectBranch(TTree* tree, const std::string& root_class) {
    if (!tree) {
        return nullptr;
    }
    auto* branches = tree->GetListOfBranches();
    for (int i = 0; branches && i < branches->GetEntries(); ++i) {
        auto* branch = dynamic_cast<TBranchElement*>(branches->At(i));
        if (branch && branch->GetClassName() && root_class == branch->GetClassName()) {
            return branch;
        }
    }
    auto* named = tree->GetBranch(root_class.c_str());
    if (named) {
        return named;
    }
    return root_class.empty() && branches && branches->GetEntries() ? dynamic_cast<TBranch*>(branches->At(0)) : nullptr;
}

void CollectBranchTree(TBranch* branch, std::vector<TBranch*>& out) {
    if (!branch) {
        return;
    }
    out.push_back(branch);
    auto* children = branch->GetListOfBranches();
    for (int i = 0; children && i < children->GetEntries(); ++i) {
        CollectBranchTree(dynamic_cast<TBranch*>(children->At(i)), out);
    }
}

bool BranchNameEndsWithToken(const std::string& name, const std::string& token) {
    if (name == token) {
        return true;
    }
    if (name.size() <= token.size()) {
        return false;
    }
    const auto position = name.size() - token.size();
    if (name.compare(position, token.size(), token) != 0) {
        return false;
    }
    const char separator = name[position - 1];
    return separator == '.' || separator == '/' || separator == '_';
}

bool ContainsTokensInOrder(const std::string& name, const std::vector<std::string>& tokens) {
    size_t position = 0;
    for (const auto& token : tokens) {
        const auto found = name.find(token, position);
        if (found == std::string::npos) {
            return false;
        }
        position = found + token.size();
    }
    return true;
}

TBranch* FindPhysicalBranch(TBranch* object_branch, const std::vector<std::string>& fields) {
    if (!object_branch || fields.empty()) {
        return nullptr;
    }
    std::vector<TBranch*> all;
    CollectBranchTree(object_branch, all);
    const auto& leaf = fields.back();
    const auto dotted = JoinStrings(fields, ".");
    const bool allow_terminal_only = fields.size() == 1;
    TBranch* best = nullptr;
    int best_score = -1;
    for (auto* candidate : all) {
        if (!candidate) {
            continue;
        }
        const std::string name = candidate->GetName();
        int score = -1;
        if (name == dotted) {
            score = 500;
        } else if (BranchNameEndsWithToken(name, dotted)) {
            score = 450;
        } else if (ContainsTokensInOrder(name, fields)) {
            score = 350;
        } else if (allow_terminal_only && name == leaf) {
            score = 250;
        } else if (allow_terminal_only && BranchNameEndsWithToken(name, leaf)) {
            score = 200;
        }
        if (score < 0 || !HasPersistentBaskets(candidate)) {
            continue;
        }
        if (candidate->GetListOfBranches() && candidate->GetListOfBranches()->GetEntries() == 0) {
            score += 25;
        }
        if (candidate->GetBasketSeek(0) > 0) {
            score += 20;
        }
        if (score > best_score) {
            best = candidate;
            best_score = score;
        }
    }
    return best;
}

bool HasPersistentBaskets(TBranch* branch) {
    if (!branch) {
        return false;
    }
    const int basket_count = branch->GetWriteBasket() + 1;
    auto* entries = branch->GetBasketEntry();
    if (basket_count <= 0 || !entries) {
        return false;
    }
    auto* bytes = branch->GetBasketBytes();
    for (int basket = 0; basket < basket_count; ++basket) {
        if (branch->GetBasketSeek(basket) > 0 || (bytes && bytes[basket] > 0) || branch->GetBasket(basket)) {
            return true;
        }
    }
    return false;
}

PhysicalBranchResolution ResolvePhysicalBranch(TBranch* object_branch, const std::vector<std::string>& fields) {
    if (!object_branch) {
        return {};
    }
    if (auto* exact = FindPhysicalBranch(object_branch, fields)) {
        return {exact, "exact"};
    }
    auto streamer_fields = fields;
    bool has_alias = false;
    for (auto& field : streamer_fields) {
        if (field == "key") {
            field = "first";
            has_alias = true;
        }
    }
    if (!streamer_fields.empty() && streamer_fields.back() == "value") {
        streamer_fields.back() = "second";
        has_alias = true;
    }
    if (has_alias) {
        if (auto* exact = FindPhysicalBranch(object_branch, streamer_fields)) {
            return {exact, "exact_alias"};
        }
    }
    for (idx_t prefix_size = fields.size(); prefix_size > 0; --prefix_size) {
        const std::vector<std::string> prefix(fields.begin(), fields.begin() + prefix_size);
        if (auto* ancestor = FindPhysicalBranch(object_branch, prefix)) {
            return {ancestor, prefix_size == fields.size() ? "exact" : "ancestor"};
        }
        if (has_alias) {
            const std::vector<std::string> streamer_prefix(streamer_fields.begin(),
                                                           streamer_fields.begin() + prefix_size);
            if (auto* ancestor = FindPhysicalBranch(object_branch, streamer_prefix)) {
                return {ancestor, prefix_size == fields.size() ? "exact_alias" : "ancestor_alias"};
            }
        }
    }
    if (HasPersistentBaskets(object_branch)) {
        return {object_branch, "object"};
    }

    std::vector<TBranch*> all;
    CollectBranchTree(object_branch, all);
    TBranch* best = nullptr;
    int best_score = -1;
    for (auto* candidate : all) {
        if (candidate == object_branch || !HasPersistentBaskets(candidate)) {
            continue;
        }
        const std::string name = candidate->GetName();
        int score = 0;
        for (idx_t i = 0; i < fields.size(); ++i) {
            if (BranchNameEndsWithToken(name, fields[i])) {
                score += static_cast<int>(100 - std::min<idx_t>(99, i));
            }
        }
        if (score > best_score) {
            best = candidate;
            best_score = score;
        }
    }
    return {best, best ? "proxy" : ""};
}

std::string SchemaFingerprint(const std::string& root_class, const std::vector<PathLevel>& levels) {
    uint64_t hash = FNV1a64(root_class);
    for (const auto& level : levels) {
        hash = FNV1a64(level.name, hash);
        hash = FNV1a64(level.type, hash);
        hash = FNV1a64(&level.offset_in_parent, sizeof(level.offset_in_parent), hash);
        const uint8_t flags = static_cast<uint8_t>((level.is_pointer ? 1 : 0) | (level.is_container ? 2 : 0) |
                                                   (level.is_primitive ? 4 : 0) | (level.is_string ? 8 : 0) |
                                                   (level.is_fixed_array ? 16 : 0));
        hash = FNV1a64(&flags, sizeof(flags), hash);
        hash = FNV1a64(&level.fixed_array_length, sizeof(level.fixed_array_length), hash);
        for (const auto dimension : level.array_dimensions) {
            hash = FNV1a64(&dimension, sizeof(dimension), hash);
        }
    }
    return Hex64(hash);
}

} // namespace duckdb::rootlake
