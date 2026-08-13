#include "root4duckdb/index/root_index_pipeline_utils.hpp"

namespace duckdb::rootlake {

bool LocalFileStat(const std::string& path, struct stat& status) {
    if (path.find("://") != std::string::npos) {
        return false;
    }
    return ::stat(path.c_str(), &status) == 0;
}

uint64_t LocalFileSize(const std::string& path) {
    struct stat status {};
    if (!LocalFileStat(path, status) || status.st_size < 0) {
        return 0;
    }
    return static_cast<uint64_t>(status.st_size);
}

int64_t LocalMtimeNS(const std::string& path) {
    struct stat status {};
    if (!LocalFileStat(path, status)) {
        return 0;
    }
#if defined(__APPLE__)
    return static_cast<int64_t>(status.st_mtimespec.tv_sec) * 1000000000LL +
           static_cast<int64_t>(status.st_mtimespec.tv_nsec);
#else
    return static_cast<int64_t>(status.st_mtim.tv_sec) * 1000000000LL + static_cast<int64_t>(status.st_mtim.tv_nsec);
#endif
}

std::string TimestampId() {
    const auto now = std::chrono::system_clock::now().time_since_epoch();
    const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(now).count();
    return std::to_string(ns);
}

std::vector<std::string> ParseLogicalPaths(const std::string& raw) {
    std::vector<std::string> paths;
    const auto first = raw.find_first_not_of(" \t\r\n");
    if (first != std::string::npos && raw[first] == '[') {
        auto json = nlohmann::json::parse(raw);
        if (!json.is_array()) {
            throw InvalidInputException("logical paths JSON must be an array");
        }
        for (const auto& item : json) {
            if (!item.is_string()) {
                throw InvalidInputException("logical paths JSON entries must be strings");
            }
            paths.push_back(NormalizePath(item.get<std::string>()));
        }
    } else {
        std::stringstream ss(raw);
        std::string path;
        while (std::getline(ss, path, ',')) {
            const auto begin = path.find_first_not_of(" \t\r\n");
            if (begin == std::string::npos) {
                continue;
            }
            const auto end = path.find_last_not_of(" \t\r\n");
            paths.push_back(NormalizePath(path.substr(begin, end - begin + 1)));
        }
    }
    std::sort(paths.begin(), paths.end());
    paths.erase(std::unique(paths.begin(), paths.end()), paths.end());
    if (paths.empty()) {
        throw InvalidInputException("At least one logical ROOT path is required");
    }
    const auto root_class = ParsePath(paths.front()).root_class;
    for (const auto& path : paths) {
        if (ParsePath(path).root_class != root_class) {
            throw InvalidInputException(
                "One root_build_index call can index multiple paths only from one top-level ROOT class");
        }
    }
    return paths;
}

std::string LogicalPathsJSON(const std::vector<std::string>& paths) {
    nlohmann::json json = paths;
    return json.dump();
}

std::string FileContentFingerprint(const std::string& path) {
    if (path.empty()) {
        return {};
    }
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        return Hex64(FNV1a64(path));
    }
    uint64_t hash = 14695981039346656037ULL;
    std::array<char, 1024 * 1024> buffer{};
    while (input) {
        input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const auto count = input.gcount();
        if (count > 0) {
            hash = FNV1a64(buffer.data(), static_cast<size_t>(count), hash);
        }
    }
    return Hex64(hash);
}

std::string ManifestFingerprint(const std::vector<std::string>& files) {
    uint64_t hash = FNV1a64(std::to_string(ROOT_LAKE_INDEX_VERSION));
    for (const auto& file : files) {
        const auto size = LocalFileSize(file);
        const auto mtime = LocalMtimeNS(file);
        hash = FNV1a64(file, hash);
        hash = FNV1a64(&size, sizeof(size), hash);
        hash = FNV1a64(&mtime, sizeof(mtime), hash);
    }
    return Hex64(hash);
}

void WriteFailureReport(const fs::path& output_dir, const std::string& snapshot_id,
                        const std::vector<RootIndexBuildStatus>& statuses) {
    fs::create_directories(output_dir);
    const auto report = output_dir / ("failed-" + snapshot_id + ".csv");
    std::ofstream out(report, std::ios::binary);
    out << "file_path,status,message\n";
    for (const auto& status : statuses) {
        if (status.status == "OK") {
            continue;
        }
        out << CsvEscape(status.file_path) << ',' << CsvEscape(status.status) << ',' << CsvEscape(status.message)
            << '\n';
    }
}

bool IsSafePublishTableName(const std::string& name) {
    if (name.empty()) {
        return false;
    }
    for (const char c : name) {
        if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '.')) {
            return false;
        }
    }
    return true;
}

} // namespace duckdb::rootlake
