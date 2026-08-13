#include "root4duckdb/core/root_input_resolver.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/file_system.hpp"

#include <algorithm>
#include <cctype>
#include <deque>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <unordered_set>

#include <nlohmann/json.hpp>

namespace duckdb::rootlake {
namespace fs = std::filesystem;

static std::string TrimInput(std::string value) {
    const auto begin = value.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos) {
        return {};
    }
    const auto end = value.find_last_not_of(" \t\r\n");
    return value.substr(begin, end - begin + 1);
}

bool IsRootFileName(const std::string& path) {
    const auto slash = path.find_last_of("/\\");
    const auto name = path.substr(slash == std::string::npos ? 0 : slash + 1);
    const auto root = name.rfind(".root");
    if (root == std::string::npos) {
        return false;
    }
    const auto suffix = name.substr(root + 5);
    if (suffix.empty()) {
        return true;
    }
    if (suffix.front() != '.' || suffix.size() == 1) {
        return false;
    }
    return std::all_of(suffix.begin() + 1, suffix.end(), [](unsigned char value) { return std::isdigit(value); });
}

bool HasRootGlob(const std::string& path) {
    return path.find_first_of("*?[") != std::string::npos;
}

static std::vector<std::string> ParseSpecifications(const std::string& raw) {
    const auto input = TrimInput(raw);
    if (input.empty()) {
        return {};
    }
    if (input.front() == '[' && input.back() == ']') {
        const auto json = nlohmann::json::parse(input);
        if (!json.is_array()) {
            throw InvalidInputException("ROOT inputs JSON must be an array");
        }
        std::vector<std::string> result;
        result.reserve(json.size());
        for (const auto& item : json) {
            if (!item.is_string()) {
                throw InvalidInputException("ROOT inputs JSON entries must be strings");
            }
            auto value = TrimInput(item.get<std::string>());
            if (!value.empty()) {
                result.push_back(std::move(value));
            }
        }
        return result;
    }

    std::vector<std::string> result;
    std::stringstream stream(input);
    std::string item;
    while (std::getline(stream, item, ',')) {
        item = TrimInput(std::move(item));
        if (!item.empty()) {
            result.push_back(std::move(item));
        }
    }
    return result;
}

static std::vector<std::string> ReadList(const std::string& path) {
    std::ifstream input(path);
    if (!input) {
        throw IOException("Cannot open ROOT URI list: " + path);
    }
    std::vector<std::string> result;
    std::string line;
    while (std::getline(input, line)) {
        line = TrimInput(std::move(line));
        if (!line.empty() && line.front() != '#') {
            result.push_back(std::move(line));
        }
    }
    return result;
}

static bool IsImplicitList(const std::string& path) {
    const auto extension = fs::path(path).extension().string();
    if (extension != ".txt" && extension != ".list" && extension != ".manifest" && extension != ".uris") {
        return false;
    }
    std::error_code error;
    if (!fs::is_regular_file(fs::path(path), error)) {
        return false;
    }
    return true;
}

std::vector<std::string> ResolveRootInputs(ClientContext& context, const std::string& raw) {
    auto& file_system = FileSystem::GetFileSystem(context);
    std::deque<std::string> pending;
    for (auto& item : ParseSpecifications(raw)) {
        pending.push_back(std::move(item));
    }

    std::vector<std::string> files;
    std::unordered_set<std::string> seen;
    idx_t expansions = 0;
    const auto append = [&](const std::string& path) {
        if (seen.insert(path).second) {
            files.push_back(path);
        }
    };

    while (!pending.empty()) {
        if (++expansions > 1000000) {
            throw InvalidInputException("ROOT input expansion is unreasonably large");
        }
        auto spec = TrimInput(std::move(pending.front()));
        pending.pop_front();
        if (spec.empty()) {
            continue;
        }

        std::string list_path;
        if (spec.front() == '@') {
            list_path = spec.substr(1);
        } else if (IsImplicitList(spec)) {
            list_path = spec;
        }
        if (!list_path.empty()) {
            auto nested = ReadList(list_path);
            for (auto it = nested.rbegin(); it != nested.rend(); ++it) {
                pending.push_front(std::move(*it));
            }
            continue;
        }

        // The overwhelmingly common explicit input needs no existence probe.
        // This is essential for remote lists: bind must not perform one stat or
        // open per URI before DuckDB's workers can start.
        if (!HasRootGlob(spec) && (IsRootFileName(spec) || spec.find("://") != std::string::npos)) {
            append(spec);
            continue;
        }

        std::string pattern = spec;
        bool directory_input = false;
        std::error_code error;
        if (fs::is_directory(fs::path(spec), error)) {
            directory_input = true;
            pattern = (fs::path(spec) / "*.root*").string();
        }

        auto matches = file_system.Glob(pattern);
        std::vector<std::string> matched_files;
        matched_files.reserve(matches.size());
        for (auto& entry : matches) {
            // An explicit user glob is authoritative. Directory shorthand is
            // the only expansion that applies the conventional ROOT suffix.
            if (!directory_input || IsRootFileName(entry.path)) {
                matched_files.push_back(std::move(entry.path));
            }
        }
        std::sort(matched_files.begin(), matched_files.end());
        for (const auto& path : matched_files) {
            append(path);
        }

        // Exact remote paths and missing exact local paths must reach the open
        // stage: silently dropping them would make a multi-file result partial.
        if (matched_files.empty() && !HasRootGlob(spec)) {
            append(spec);
        }
    }

    if (files.empty()) {
        throw IOException("No ROOT files matched input specification: " + raw);
    }
    return files;
}

} // namespace duckdb::rootlake
