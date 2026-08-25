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

namespace {

std::string TrimInput(std::string value) {
    const auto begin = value.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos) {
        return {};
    }
    const auto end = value.find_last_not_of(" \t\r\n");
    return value.substr(begin, end - begin + 1);
}

std::vector<std::string> ParseSpecifications(const std::string& raw) {
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

std::vector<std::string> ReadLocalList(const std::string& path) {
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

bool IsImplicitList(const std::string& path) {
    if (RootInputResolver::IsRemoteUri(path)) {
        return false;
    }
    const auto extension = fs::path(path).extension().string();
    if (extension != ".txt" && extension != ".list" && extension != ".manifest" && extension != ".uris") {
        return false;
    }
    std::error_code error;
    return fs::is_regular_file(fs::path(path), error) && !error;
}

std::string RemoteGlobFailure(const std::string& pattern, const std::string& detail) {
    std::string message = "Cannot expand remote ROOT glob '" + pattern + "'";
    if (!detail.empty()) {
        message += ": " + detail;
    }
    message += ". Exact remote URIs are opened by ROOT; wildcard expansion requires a DuckDB filesystem "
               "backend that can list that URI scheme. Use an explicit JSON/comma list or a local @list of URIs "
               "when listing is unavailable.";
    return message;
}

} // namespace

RootInputResolver::RootInputResolver(ClientContext& context) : context_(context) {
}

bool RootInputResolver::IsRootFileName(const std::string& path) {
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

bool RootInputResolver::HasGlob(const std::string& path) {
    return path.find_first_of("*?[") != std::string::npos;
}

bool RootInputResolver::IsRemoteUri(const std::string& path) {
    const auto scheme = path.find("://");
    return scheme != std::string::npos && scheme > 0;
}

bool RootInputResolver::IsS3Uri(const std::string& path) {
    return path.rfind("s3://", 0) == 0 || path.rfind("davix://", 0) == 0;
}

std::vector<std::string> RootInputResolver::Resolve(const std::string& raw) const {
    auto& file_system = FileSystem::GetFileSystem(context_);
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
            if (IsRemoteUri(list_path)) {
                throw InvalidInputException("ROOT @lists must be local files; their entries may contain remote URIs");
            }
        } else if (IsImplicitList(spec)) {
            list_path = spec;
        }
        if (!list_path.empty()) {
            auto nested = ReadLocalList(list_path);
            for (auto it = nested.rbegin(); it != nested.rend(); ++it) {
                pending.push_front(std::move(*it));
            }
            continue;
        }

        // Exact paths and URIs are intentionally not probed during resolution.
        // For s3:// and davix:// this keeps credentials in ROOT/.rootrc or the
        // process environment instead of copying secrets into SQL text/plans.
        if (!HasGlob(spec) && (RootInputResolver::IsRootFileName(spec) || IsRemoteUri(spec))) {
            append(spec);
            continue;
        }

        std::string pattern = spec;
        bool directory_input = false;
        if (!IsRemoteUri(spec)) {
            std::error_code error;
            if (fs::is_directory(fs::path(spec), error) && !error) {
                directory_input = true;
                pattern = (fs::path(spec) / "*.root*").string();
            }
        }

        std::vector<std::string> matched_files;
        try {
            auto matches = file_system.Glob(pattern);
            matched_files.reserve(matches.size());
            for (auto& entry : matches) {
                if (!directory_input || RootInputResolver::IsRootFileName(entry.path)) {
                    matched_files.push_back(std::move(entry.path));
                }
            }
        } catch (const std::exception& exception) {
            if (IsRemoteUri(pattern) && HasGlob(pattern)) {
                throw IOException(RemoteGlobFailure(pattern, exception.what()));
            }
            throw;
        }
        std::sort(matched_files.begin(), matched_files.end());
        for (const auto& path : matched_files) {
            append(path);
        }

        if (matched_files.empty() && IsRemoteUri(spec) && HasGlob(spec)) {
            throw IOException(RemoteGlobFailure(spec, "the filesystem backend returned no matches"));
        }

        // Missing exact local paths reach the common open stage so multi-file
        // scans report them through the same unavailable-source telemetry.
        if (matched_files.empty() && !HasGlob(spec)) {
            append(spec);
        }
    }

    if (files.empty()) {
        throw IOException("No ROOT files matched input specification: " + raw);
    }
    return files;
}

std::vector<std::string> ResolveRootInputs(ClientContext& context, const std::string& input) {
    return RootInputResolver(context).Resolve(input);
}

bool IsRootFileName(const std::string& path) {
    return RootInputResolver::IsRootFileName(path);
}

bool HasRootGlob(const std::string& path) {
    return RootInputResolver::HasGlob(path);
}

} // namespace duckdb::rootlake
