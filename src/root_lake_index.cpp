#include "root_runtime_settings.hpp"
#include "root_iceberg_catalog.hpp"
#include "include/root_bloom.hpp"
#include "include/root_branch_projection.hpp"
#include "include/root_index_metadata.hpp"
#include "include/root_lake_common.hpp"
#include "include/root_serialized_reader.hpp"
#include "duckdb/common/types/data_chunk.hpp"
#include "duckdb/common/types/vector.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckdb/main/connection.hpp"
#include "duckdb/main/extension/extension_loader.hpp"

#include <atomic>
#include <array>
#include <cerrno>
#include <chrono>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <deque>
#include <mutex>
#include <set>
#include <sys/stat.h>
#include <thread>
#include <tuple>

#include <nlohmann/json.hpp>

namespace duckdb::rootlake {

namespace fs = std::filesystem;

struct BuildStatus {
    std::string file_path;
    std::string file_id;
    std::string schema_id;
    uint64_t entries = 0;
    uint64_t flattened_values = 0;
    uint64_t baskets = 0;
    std::string status;
    std::string message;
};

struct BuildIndexBindData final : public TableFunctionData {
    unique_ptr<FunctionData> Copy() const override {
        auto result = make_uniq<BuildIndexBindData>(*this);
        return std::move(result);
    }
    bool Equals(const FunctionData &) const override {
        return false;
    }
    bool SupportStatementCache() const override {
        return false;
    }

    std::string root_glob;
    std::string tree_name;
    std::string logical_path;
    std::vector<std::string> logical_paths;
    std::string output_dir;
    std::string catalog_prefix;
    std::string dictionary;
    uint32_t bloom_bytes = 0;
    double bloom_false_positive_rate = 0.01;
    uint32_t index_threads = 0;
    uint32_t max_in_flight_files = 0;
    uint64_t memory_budget_bytes = 0;
    uint64_t estimated_worker_bytes = 512ULL * 1024ULL * 1024ULL;
    uint64_t metadata_flush_bytes = 128ULL * 1024ULL * 1024ULL;
    uint64_t tree_cache_bytes = 64ULL * 1024ULL * 1024ULL;
    RootReaderMode reader_mode = RootReaderMode::AUTO;
    uint32_t raw_validation_entries = 4;
    uint64_t raw_max_entry_bytes = 64ULL * 1024ULL * 1024ULL;
    uint64_t raw_max_values_per_entry = 10ULL * 1024ULL * 1024ULL;
    bool has_bloom_bytes = false;
    bool has_index_threads = false;
    bool has_max_in_flight_files = false;
    bool has_memory_budget_bytes = false;
    bool has_estimated_worker_bytes = false;
    bool has_metadata_flush_bytes = false;
    RootDictionaryCleanupMode dictionary_cleanup_mode = RootDictionaryCleanupMode::FULL;
    std::string chunk_id;
    std::string manifest_fingerprint;
    std::string dictionary_fingerprint;
    bool overwrite = false;
    bool allow_partial = false;
    std::string files_table;
    std::string schemas_table;
    std::string access_table;
    std::string baskets_table;
    std::string snapshots_table;
    std::string publish_mode = "none";
    std::string catalog_mode = "local";
    bool has_catalog_mode = false;
};

struct BuildIndexGlobalState final : public GlobalTableFunctionState {
    std::vector<BuildStatus> statuses;
    idx_t offset = 0;
    std::string snapshot_id;
    std::string snapshot_dir;
    uint32_t requested_threads = 0;
    uint32_t effective_threads = 1;
    bool published = false;
    std::string publish_mode = "none";
    std::string chunk_id;
    std::string manifest_fingerprint;
    std::string dictionary_fingerprint;

    idx_t MaxThreads() const override { return 1; }
};

static bool LocalFileStat(const std::string &path, struct stat &status) {
    if (path.find("://") != std::string::npos) return false;
    return ::stat(path.c_str(), &status) == 0;
}

static uint64_t LocalFileSize(const std::string &path) {
    struct stat status {};
    if (!LocalFileStat(path, status) || status.st_size < 0) return 0;
    return static_cast<uint64_t>(status.st_size);
}

static int64_t LocalMtimeNS(const std::string &path) {
    struct stat status {};
    if (!LocalFileStat(path, status)) return 0;
#if defined(__APPLE__)
    return static_cast<int64_t>(status.st_mtimespec.tv_sec) * 1000000000LL +
           static_cast<int64_t>(status.st_mtimespec.tv_nsec);
#else
    return static_cast<int64_t>(status.st_mtim.tv_sec) * 1000000000LL +
           static_cast<int64_t>(status.st_mtim.tv_nsec);
#endif
}

static std::string TimestampId() {
    const auto now = std::chrono::system_clock::now().time_since_epoch();
    const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(now).count();
    return std::to_string(ns);
}

static std::vector<std::string> ParseLogicalPaths(const std::string &raw) {
    std::vector<std::string> paths;
    const auto first = raw.find_first_not_of(" \t\r\n");
    if (first != std::string::npos && raw[first] == '[') {
        auto json = nlohmann::json::parse(raw);
        if (!json.is_array()) throw InvalidInputException("logical paths JSON must be an array");
        for (const auto &item : json) {
            if (!item.is_string()) throw InvalidInputException("logical paths JSON entries must be strings");
            paths.push_back(NormalizePath(item.get<std::string>()));
        }
    } else {
        std::stringstream ss(raw);
        std::string path;
        while (std::getline(ss, path, ',')) {
            const auto begin = path.find_first_not_of(" \t\r\n");
            if (begin == std::string::npos) continue;
            const auto end = path.find_last_not_of(" \t\r\n");
            paths.push_back(NormalizePath(path.substr(begin, end - begin + 1)));
        }
    }
    std::sort(paths.begin(), paths.end());
    paths.erase(std::unique(paths.begin(), paths.end()), paths.end());
    if (paths.empty()) throw InvalidInputException("At least one logical ROOT path is required");
    const auto root_class = ParsePath(paths.front()).root_class;
    for (const auto &path : paths) {
        if (ParsePath(path).root_class != root_class) {
            throw InvalidInputException("One root_build_index call can index multiple paths only from one top-level ROOT class");
        }
    }
    return paths;
}

static std::string LogicalPathsJSON(const std::vector<std::string> &paths) {
    nlohmann::json json = paths;
    return json.dump();
}

static std::string Trim(std::string value) {
    const auto begin = value.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos) return {};
    const auto end = value.find_last_not_of(" \t\r\n");
    return value.substr(begin, end - begin + 1);
}

static bool LooksLikeRootInput(const std::string &path) {
    const auto slash = path.find_last_of("/\\");
    const auto name = path.substr(slash == std::string::npos ? 0 : slash + 1);
    const auto root = name.rfind(".root");
    if (root == std::string::npos) return false;
    const auto suffix = name.substr(root + 5);
    if (suffix.empty()) return true;
    if (suffix.front() != '.' || suffix.size() == 1) return false;
    return std::all_of(suffix.begin() + 1, suffix.end(),
                       [](unsigned char c) { return std::isdigit(c); });
}

static std::vector<std::string> ReadInputList(const std::string &path) {
    std::ifstream input(path);
    if (!input) throw IOException("Cannot open ROOT URI list: " + path);
    std::vector<std::string> result;
    std::string line;
    while (std::getline(input, line)) {
        line = Trim(line);
        if (!line.empty() && line.front() != '#') result.push_back(line);
    }
    return result;
}

static std::vector<std::string> ParseInputSpecifications(const std::string &raw) {
    const auto trimmed = Trim(raw);
    if (trimmed.empty()) return {};
    if (trimmed.front() == '[') {
        auto json = nlohmann::json::parse(trimmed);
        if (!json.is_array()) throw InvalidInputException("ROOT inputs JSON must be an array");
        std::vector<std::string> result;
        for (const auto &item : json) {
            if (!item.is_string()) throw InvalidInputException("ROOT inputs JSON entries must be strings");
            result.push_back(Trim(item.get<std::string>()));
        }
        return result;
    }
    std::vector<std::string> result;
    std::stringstream stream(trimmed);
    std::string item;
    while (std::getline(stream, item, ',')) {
        item = Trim(item);
        if (!item.empty()) result.push_back(item);
    }
    return result;
}

static std::vector<std::string> ExpandRootInputs(ClientContext &context, const std::string &raw) {
    auto &file_system = FileSystem::GetFileSystem(context);
    std::deque<std::string> pending;
    for (const auto &spec : ParseInputSpecifications(raw)) pending.push_back(spec);
    std::vector<std::string> files;
    idx_t expansions = 0;
    while (!pending.empty()) {
        if (++expansions > 1000000) throw InvalidInputException("ROOT input expansion is unreasonably large");
        auto spec = Trim(std::move(pending.front()));
        pending.pop_front();
        if (spec.empty()) continue;

        std::string list_path;
        if (spec.front() == '@') list_path = spec.substr(1);
        else {
            const fs::path local(spec);
            const auto extension = local.extension().string();
            if (fs::is_regular_file(local) &&
                (extension == ".txt" || extension == ".list" || extension == ".manifest" || extension == ".uris")) {
                list_path = spec;
            }
        }
        if (!list_path.empty()) {
            const auto nested = ReadInputList(list_path);
            for (auto it = nested.rbegin(); it != nested.rend(); ++it) pending.push_front(*it);
            continue;
        }

        std::string pattern = spec;
        std::error_code ec;
        if (fs::is_directory(fs::path(spec), ec)) {
            pattern = (fs::path(spec) / "*.root*").string();
        }
        auto matches = file_system.Glob(pattern);
        for (auto &entry : matches) {
            if (LooksLikeRootInput(entry.path)) files.push_back(entry.path);
        }
    }
    std::sort(files.begin(), files.end());
    files.erase(std::unique(files.begin(), files.end()), files.end());
    if (files.empty()) throw IOException("No ROOT files matched input specification: " + raw);
    return files;
}

static std::string FileContentFingerprint(const std::string &path) {
    if (path.empty()) return {};
    std::ifstream input(path, std::ios::binary);
    if (!input) return Hex64(FNV1a64(path));
    uint64_t hash = 14695981039346656037ULL;
    std::array<char, 1024 * 1024> buffer {};
    while (input) {
        input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const auto count = input.gcount();
        if (count > 0) hash = FNV1a64(buffer.data(), static_cast<size_t>(count), hash);
    }
    return Hex64(hash);
}

static std::string ManifestFingerprint(const std::vector<std::string> &files) {
    uint64_t hash = FNV1a64(std::to_string(ROOT_LAKE_INDEX_VERSION));
    for (const auto &file : files) {
        const auto size = LocalFileSize(file);
        const auto mtime = LocalMtimeNS(file);
        hash = FNV1a64(file, hash);
        hash = FNV1a64(&size, sizeof(size), hash);
        hash = FNV1a64(&mtime, sizeof(mtime), hash);
    }
    return Hex64(hash);
}

static void LoadDictionary(const std::string &dictionary) {
    if (dictionary.empty()) return;
    if (gSystem->Load(dictionary.c_str()) < 0) {
        throw IOException("Failed to load ROOT dictionary: " + dictionary);
    }
}

struct FilePlan {
    std::string path;
    uint64_t entries = 0;
    uint64_t event_base = 0;
    std::string error;
};

static FilePlan InspectOneFile(const BuildIndexBindData &bind, const std::string &path) {
    FilePlan plan;
    plan.path = path;
    try {
        std::unique_ptr<TFile> file(TFile::Open(path.c_str(), "READ"));
        if (!file || file->IsZombie()) throw IOException("ROOT file is zombie");
        const auto parsed = ParsePath(bind.logical_paths.front());
        auto *tree = FindTree(file.get(), bind.tree_name, parsed.root_class);
        if (!tree) throw InvalidInputException("No TTree found");
        plan.entries = static_cast<uint64_t>(tree->GetEntries());
    } catch (std::exception &ex) {
        plan.error = ex.what();
    }
    return plan;
}

static void WriteFailureReport(const fs::path &output_dir, const std::string &snapshot_id,
                               const std::vector<BuildStatus> &statuses) {
    fs::create_directories(output_dir);
    const auto report = output_dir / ("failed-" + snapshot_id + ".csv");
    std::ofstream out(report, std::ios::binary);
    out << "file_path,status,message\n";
    for (const auto &status : statuses) {
        if (status.status == "OK") continue;
        out << CsvEscape(status.file_path) << ',' << CsvEscape(status.status) << ',' << CsvEscape(status.message)
            << '\n';
    }
}

static void WriteSchemaRows(RootFileIndexMetadata &metadata, const std::string &schema_id, const std::string &column_id,
                            const std::string &logical_path, const ParsedPath &parsed,
                            const std::vector<PathLevel> &levels) {
    const std::string access_plan_id = Hex64(FNV1a64(logical_path, FNV1a64(schema_id)) ^ 0x9f6abc31ULL);
    const auto leaf_type = PrimitiveBaseType(levels.back().type);
    const auto duck_type = RootTypeToLogicalType(leaf_type).ToString();
    const idx_t index_depth = IndexDepth(levels);

    RootSchemaMetadataRow schema;
    schema.index_version = ROOT_LAKE_INDEX_VERSION;
    schema.schema_id = schema_id;
    schema.column_id = column_id;
    schema.logical_path = logical_path;
    schema.root_class = parsed.root_class;
    schema.root_type = leaf_type;
    schema.duckdb_type = duck_type;
    schema.access_plan_id = access_plan_id;
    schema.index_signature = IndexSignature(levels);
    schema.container_depth = static_cast<uint32_t>(index_depth);
    metadata.schemas.push_back(std::move(schema));

    for (idx_t i = 0; i < levels.size(); ++i) {
        const auto &level = levels[i];
        RootAccessMetadataRow access;
        access.index_version = ROOT_LAKE_INDEX_VERSION;
        access.access_plan_id = access_plan_id;
        access.level_no = static_cast<uint32_t>(i);
        access.field_name = level.name;
        access.root_type = level.type;
        access.offset_in_parent = level.offset_in_parent;
        access.cumulative_offset = level.cumulative_offset;
        access.is_pointer = level.is_pointer;
        access.is_container = level.is_container;
        access.is_primitive = level.is_primitive;
        access.is_string = level.is_string;
        access.is_fixed_array = level.is_fixed_array;
        access.array_rank = static_cast<uint32_t>(level.array_dimensions.size());
        access.array_length = level.fixed_array_length;
        access.array_dimensions = ArrayDimensionsText(level.array_dimensions);
        access.element_size = static_cast<uint32_t>(level.element_size);
        metadata.access.push_back(std::move(access));
    }
}

struct BasketAccumulator {
    uint32_t basket_id = 0;
    uint64_t entry_begin = 0;
    uint64_t entry_end = 0;
    uint64_t physical_offset = 0;
    uint32_t key_length = 0;
    uint32_t compressed_size = 0;
    uint32_t uncompressed_size = 0;
    uint64_t value_count = 0;
    uint64_t null_count = 0;
    uint64_t nan_count = 0;
    uint64_t pos_inf_count = 0;
    uint64_t neg_inf_count = 0;
    uint64_t finite_count = 0;
    double min_value = std::numeric_limits<double>::infinity();
    double max_value = -std::numeric_limits<double>::infinity();
    RootBloomBuilder bloom;

    BasketAccumulator(uint32_t bloom_bytes, double bloom_fpr, uint32_t id)
        : basket_id(id), bloom(bloom_bytes, bloom_fpr) {
    }

    void Add(double value) {
        ++value_count;
        if (std::isnan(value)) {
            ++nan_count;
        } else if (std::isinf(value)) {
            if (value > 0) ++pos_inf_count;
            else ++neg_inf_count;
        } else {
            min_value = std::min(min_value, value);
            max_value = std::max(max_value, value);
            bloom.Add(value);
            ++finite_count;
        }
    }
};

struct IndexedPathPlan {
    std::string logical_path;
    ParsedPath parsed;
    std::vector<PathLevel> levels;
    TBranch *physical_branch = nullptr;
    std::string physical_mode;
    std::string schema_id;
    std::string column_id;
    std::vector<BasketAccumulator> baskets;
    idx_t active_basket = 0;
    SerializedReadPlan serialized_plan;
    SerializedBasketReader serialized_reader;
    bool serialized_active = false;
    uint32_t validation_remaining = 0;
    bool fallback_recorded = false;
};

static void ApplyRootBasketMetadata(BasketAccumulator &basket, TBasket *root_basket) {
    if (!root_basket) return;
    basket.key_length = static_cast<uint32_t>(std::max(0, root_basket->GetKeylen()));
    basket.uncompressed_size = static_cast<uint32_t>(std::max(0, root_basket->GetObjlen()));
    if (!basket.compressed_size) {
        basket.compressed_size = static_cast<uint32_t>(std::max(0, root_basket->GetNbytes()));
    }
    if (!basket.physical_offset) {
        basket.physical_offset = static_cast<uint64_t>(
            std::max<Long64_t>(0, root_basket->GetSeekKey()));
    }
}

static void ApplySerializedBasketMetadata(BasketAccumulator &basket,
                                          const SerializedBasketInfo &info) {
    if (info.basket_number != static_cast<int32_t>(basket.basket_id)) return;
    basket.key_length = info.key_length;
    basket.uncompressed_size = info.uncompressed_size;
    if (!basket.compressed_size) basket.compressed_size = info.compressed_size;
    if (!basket.physical_offset) basket.physical_offset = info.physical_offset;
}

static IndexedPathPlan PrepareIndexedPath(const BuildIndexBindData &bind, TBranch *object_branch,
                                          TClass *root_class, TFile *file, uint64_t total_entries,
                                          const std::string &logical_path) {
    IndexedPathPlan plan;
    plan.logical_path = logical_path;
    plan.parsed = ParsePath(logical_path);
    plan.levels = PathResolver::Resolve(root_class, plan.parsed.fields);
    if (!plan.levels.back().is_primitive || !IsLosslessDoubleBackedType(plan.levels.back().type)) {
        throw NotImplementedException(
            "Indexed ROOT paths currently require a numeric leaf up to 32-bit integer plus FLOAT/DOUBLE; "
            "unsupported leaf type for " + logical_path + ": " + plan.levels.back().type);
    }
    const auto physical = ResolvePhysicalBranch(object_branch, plan.parsed.fields);
    plan.physical_branch = physical.branch;
    plan.physical_mode = physical.mode;
    if (!plan.physical_branch) {
        throw InvalidInputException("No persistent branch can provide entry ranges for " + logical_path);
    }
    plan.schema_id = SchemaFingerprint(plan.parsed.root_class, plan.levels);
    plan.column_id = ColumnId(plan.schema_id, logical_path);
    plan.serialized_plan = BuildSerializedReadPlan(root_class, plan.parsed, plan.physical_branch);

    const int basket_count = plan.physical_branch->GetWriteBasket() + 1;
    auto *basket_entries = plan.physical_branch->GetBasketEntry();
    auto *basket_bytes = plan.physical_branch->GetBasketBytes();
    if (basket_count <= 0 || !basket_entries) {
        throw InvalidInputException("Physical branch has no persistent baskets: " +
                                    std::string(plan.physical_branch->GetName()));
    }
    plan.baskets.reserve(static_cast<idx_t>(basket_count));
    for (int basket_id = 0; basket_id < basket_count; ++basket_id) {
        const uint64_t entry_begin = static_cast<uint64_t>(basket_entries[basket_id]);
        uint64_t entry_end = basket_id + 1 < basket_count
                                 ? static_cast<uint64_t>(basket_entries[basket_id + 1])
                                 : total_entries;
        entry_end = std::min(entry_end, total_entries);
        if (entry_begin >= entry_end) continue;
        plan.baskets.emplace_back(bind.bloom_bytes, bind.bloom_false_positive_rate,
                                  static_cast<uint32_t>(basket_id));
        auto &basket = plan.baskets.back();
        basket.entry_begin = entry_begin;
        basket.entry_end = entry_end;
        const auto basket_seek = plan.physical_branch->GetBasketSeek(basket_id);
        basket.physical_offset = basket_seek > 0 ? static_cast<uint64_t>(basket_seek) : 0;
        basket.compressed_size = basket_bytes ? static_cast<uint32_t>(std::max(0, basket_bytes[basket_id])) : 0;
        const bool decode_will_supply_metadata =
            bind.reader_mode != RootReaderMode::OBJECT && plan.serialized_plan.supported;
        if (!decode_will_supply_metadata) {
            auto *root_basket = plan.physical_branch->GetBasket(basket_id);
            ApplyRootBasketMetadata(basket, root_basket);
            // Metadata inspection must not retain every decompressed basket.
            plan.physical_branch->DropBaskets();
        }
    }
    return plan;
}

static BuildStatus IndexOneFile(const BuildIndexBindData &bind, const std::string &dataset_id,
                                const std::string &snapshot_id, const std::string &root_path, uint64_t event_base,
                                RootFileIndexMetadata &metadata, std::set<std::string> &written_columns) {
    BuildStatus status;
    status.file_path = root_path;
    try {
        std::unique_ptr<TFile> file(TFile::Open(root_path.c_str(), "READ"));
        if (!file || file->IsZombie()) throw IOException("ROOT file is zombie");

        const auto root_parsed = ParsePath(bind.logical_paths.front());
        auto *root_class = TClass::GetClass(root_parsed.root_class.c_str());
        if (!root_class || !root_class->HasDictionary()) {
            throw InvalidInputException("ROOT dictionary is unavailable for class " + root_parsed.root_class);
        }
        auto *tree = FindTree(file.get(), bind.tree_name, root_parsed.root_class);
        if (!tree) throw InvalidInputException("No TTree found");
        auto *object_branch = FindObjectBranch(tree, root_parsed.root_class);
        if (!object_branch) throw InvalidInputException("No object branch for class " + root_parsed.root_class);

        const uint64_t total_entries = static_cast<uint64_t>(tree->GetEntries());
        status.entries = total_entries;
        const uint64_t file_size = LocalFileSize(root_path);
        const int64_t mtime_ns = LocalMtimeNS(root_path);
        const auto file_id = FileId(root_path, file_size, mtime_ns);
        status.file_id = file_id;

        std::vector<IndexedPathPlan> paths;
        paths.reserve(bind.logical_paths.size());
        for (const auto &logical_path : bind.logical_paths) {
            paths.push_back(PrepareIndexedPath(bind, object_branch, root_class, file.get(), total_entries,
                                               logical_path));
        }

        RootObjectContext object_context;
        object_context.Bind(tree, object_branch, root_class, bind.dictionary_cleanup_mode);
        std::vector<TBranch *> projected_branches;
        bool projection_safe = true;
        for (auto &path : paths) {
            projected_branches.push_back(path.physical_branch);
            projection_safe = projection_safe && path.physical_mode == "ancestor";
        }
        const auto projection = projection_safe
                                    ? ApplyBranchProjection(tree, projected_branches, bind.tree_cache_bytes)
                                    : BranchProjectionResult {};
        if (!projection.applied) EnableAllBranches(tree, bind.tree_cache_bytes);

        uint64_t serialized_path_count = 0;
        uint64_t fallback_path_count = 0;
        std::unordered_map<TBranch *, idx_t> paths_per_physical_branch;
        for (const auto &path : paths) ++paths_per_physical_branch[path.physical_branch];
        for (auto &path : paths) {
            if (bind.reader_mode == RootReaderMode::OBJECT) continue;
            if (!path.serialized_plan.supported) {
                if (bind.reader_mode == RootReaderMode::SERIALIZED) {
                    throw InvalidInputException("reader_mode='serialized' cannot index " +
                                                path.logical_path + ": " + path.serialized_plan.reason);
                }
                path.fallback_recorded = true;
                ++fallback_path_count;
                WarnRootFallbackOnce(path.logical_path, path.schema_id, path.serialized_plan.reason);
                continue;
            }
            if (paths_per_physical_branch[path.physical_branch] > 1) {
                const std::string reason =
                    "multiple requested paths share one physical ancestor; universal one-pass read is cheaper";
                if (bind.reader_mode == RootReaderMode::SERIALIZED) {
                    throw InvalidInputException("reader_mode='serialized' cannot index " +
                                                path.logical_path + ": " + reason);
                }
                path.fallback_recorded = true;
                ++fallback_path_count;
                WarnRootFallbackOnce(path.logical_path, path.schema_id, reason);
                continue;
            }
            path.serialized_reader.Bind(path.physical_branch, path.serialized_plan,
                                        bind.raw_max_entry_bytes, bind.raw_max_values_per_entry);
            path.serialized_active = true;
            path.validation_remaining = bind.raw_validation_entries;
            ++serialized_path_count;
        }

        auto fallback_path = [&](IndexedPathPlan &path, const std::string &reason) {
            if (bind.reader_mode == RootReaderMode::SERIALIZED) {
                throw IOException("reader_mode='serialized' failed for " + path.logical_path + ": " + reason);
            }
            path.serialized_active = false;
            if (!path.fallback_recorded) {
                path.fallback_recorded = true;
                ++fallback_path_count;
            }
            WarnRootFallbackOnce(path.logical_path, path.schema_id, reason);
        };

        std::vector<double> values;
        std::vector<int32_t> indices;
        std::vector<double> reference_values;
        std::vector<int32_t> reference_indices;
        uint64_t object_reads = 0;
        for (uint64_t entry = 0; entry < total_entries; ++entry) {
            bool need_object = bind.reader_mode == RootReaderMode::OBJECT;
            for (const auto &path : paths) {
                if (!path.serialized_active || path.validation_remaining > 0) {
                    need_object = true;
                    break;
                }
            }
            void *object = nullptr;
            bool object_loaded = false;
            auto ensure_object = [&]() {
                if (!object_loaded) {
                    object = object_context.Read(entry);
                    object_loaded = true;
                    ++object_reads;
                }
                return object;
            };
            if (need_object) ensure_object();

            for (auto &path : paths) {
                while (path.active_basket < path.baskets.size() &&
                       entry >= path.baskets[path.active_basket].entry_end) {
                    ++path.active_basket;
                }
                if (path.active_basket >= path.baskets.size()) continue;
                auto &basket = path.baskets[path.active_basket];
                if (entry < basket.entry_begin || entry >= basket.entry_end) continue;

                values.clear();
                indices.clear();
                if (path.serialized_active) {
                    std::string failure_reason;
                    const bool collect_indices = path.validation_remaining > 0;
                    const bool decoded = path.serialized_reader.Decode(
                        entry, values, indices, failure_reason, collect_indices);
                    SerializedBasketInfo basket_info;
                    if (path.serialized_reader.CurrentBasketInfo(basket_info)) {
                        ApplySerializedBasketMetadata(basket, basket_info);
                    }
                    if (!decoded) {
                        fallback_path(path, failure_reason);
                    } else if (path.validation_remaining > 0) {
                        reference_values.clear();
                        reference_indices.clear();
                        if (ensure_object()) {
                            OffsetValueReader::CollectFlat(object, path.levels, IndexDepth(path.levels),
                                                           reference_values, reference_indices);
                        }
                        if (!object || !EqualDecodedValues(values, indices,
                                                           reference_values, reference_indices)) {
                            fallback_path(path, "serialized values differ from universal ROOT reader");
                        } else {
                            --path.validation_remaining;
                        }
                    }
                }

                if (!path.serialized_active) {
                    values.clear();
                    if (!ensure_object()) {
                        ++basket.null_count;
                        continue;
                    }
                    OffsetValueReader::CollectValues(object, path.levels, values);
                }
                for (double value : values) basket.Add(value);
            }
        }

        // A runtime fallback may leave a suffix of basket headers unseen by the
        // serialized reader. Fill only those gaps; the normal fast path performs
        // no second decompression pass.
        for (auto &path : paths) {
            for (auto &basket : path.baskets) {
                if (basket.key_length && basket.uncompressed_size) continue;
                auto *root_basket = path.physical_branch->GetBasket(
                    static_cast<int>(basket.basket_id));
                ApplyRootBasketMetadata(basket, root_basket);
                path.physical_branch->DropBaskets();
            }
        }

        std::vector<std::string> schema_ids;
        for (auto &path : paths) {
            schema_ids.push_back(path.schema_id);
            if (written_columns.insert(path.column_id).second) {
                WriteSchemaRows(metadata, path.schema_id, path.column_id, path.logical_path, path.parsed, path.levels);
            }

            uint64_t flat_value_begin = 0;
            uint64_t total_values = 0;
            uint64_t file_null_count = 0;
            uint64_t file_nan_count = 0;
            uint64_t file_pos_inf_count = 0;
            uint64_t file_neg_inf_count = 0;
            uint64_t actual_baskets = 0;
            uint64_t file_finite_count = 0;
            double file_min = std::numeric_limits<double>::infinity();
            double file_max = -std::numeric_limits<double>::infinity();

            for (auto &basket : path.baskets) {
                const bool has_minmax = basket.finite_count > 0;
                if (has_minmax) {
                    file_min = std::min(file_min, basket.min_value);
                    file_max = std::max(file_max, basket.max_value);
                    file_finite_count += basket.finite_count;
                }
                RootBasketMetadataRow row;
                row.index_version = ROOT_LAKE_INDEX_VERSION;
                row.snapshot_id = snapshot_id;
                row.file_id = file_id;
                row.column_id = path.column_id;
                row.basket_id = basket.basket_id;
                row.basket_branch_name = path.physical_branch->GetName();
                row.basket_branch_mode = path.physical_mode;
                row.entry_begin = basket.entry_begin;
                row.entry_end = basket.entry_end;
                row.event_base = event_base;
                row.flat_value_begin = flat_value_begin;
                row.value_count = basket.value_count;
                row.physical_offset = basket.physical_offset;
                row.key_length = basket.key_length;
                row.compressed_size = basket.compressed_size;
                row.uncompressed_size = basket.uncompressed_size;
                row.compression = static_cast<uint32_t>(file->GetCompressionAlgorithm());
                row.has_minmax = has_minmax;
                row.min_value = basket.min_value;
                row.max_value = basket.max_value;
                row.null_count = basket.null_count;
                row.nan_count = basket.nan_count;
                row.pos_inf_count = basket.pos_inf_count;
                row.neg_inf_count = basket.neg_inf_count;
                row.bloom_filter = basket.bloom.SerializeAndRelease();
                metadata.baskets.push_back(std::move(row));
                flat_value_begin += basket.value_count;
                total_values += basket.value_count;
                file_null_count += basket.null_count;
                file_nan_count += basket.nan_count;
                file_pos_inf_count += basket.pos_inf_count;
                file_neg_inf_count += basket.neg_inf_count;
                ++actual_baskets;
            }

            RootFileMetadataRow file_row;
            file_row.index_version = ROOT_LAKE_INDEX_VERSION;
            file_row.dataset_id = dataset_id;
            file_row.snapshot_id = snapshot_id;
            file_row.file_id = file_id;
            file_row.root_uri = root_path;
            file_row.tree_name = tree->GetName();
            file_row.schema_id = path.schema_id;
            file_row.column_id = path.column_id;
            file_row.event_base = event_base;
            file_row.total_entries = total_entries;
            file_row.file_size = file_size;
            file_row.mtime_ns = mtime_ns;
            file_row.has_minmax = file_finite_count != 0;
            file_row.min_value = file_min;
            file_row.max_value = file_max;
            file_row.value_count = total_values;
            file_row.null_count = file_null_count;
            file_row.nan_count = file_nan_count;
            file_row.pos_inf_count = file_pos_inf_count;
            file_row.neg_inf_count = file_neg_inf_count;
            file_row.basket_count = actual_baskets;
            metadata.files.push_back(std::move(file_row));
            status.flattened_values += total_values;
            status.baskets += actual_baskets;
        }
        status.schema_id = JoinStrings(schema_ids, ",");
        status.status = "OK";
        status.message = "indexed " + std::to_string(paths.size()) +
                         " logical paths; serialized=" + std::to_string(serialized_path_count) +
                         ", object_fallback=" + std::to_string(fallback_path_count) +
                         ", object_reads=" + std::to_string(object_reads);
    } catch (const std::exception &ex) {
        status.status = "ERROR";
        status.message = ex.what();
    }
    return status;
}

static bool IsSafePublishTableName(const std::string &name) {
    if (name.empty()) return false;
    for (const char c : name) {
        if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '.')) return false;
    }
    return true;
}

static void PublishMetadataTables(DatabaseInstance &db, const fs::path &staging,
                                  const BuildIndexBindData &bind, const BuildIndexGlobalState &state,
                                  const std::string &dataset_id, const std::string &snapshot_id) {
    if (bind.publish_mode == "none") return;
    const std::vector<std::pair<std::string, fs::path>> tables = {
        {bind.files_table, staging / "root_files.parquet"},
        {bind.schemas_table, staging / "root_schemas.parquet"},
        {bind.access_table, staging / "root_access_levels.parquet"},
        {bind.baskets_table, staging / "root_baskets.parquet"},
    };
    Connection connection(db);
    auto begin = connection.Query("BEGIN TRANSACTION");
    EnsureQueryOK(*begin, "begin metadata catalog publication");
    try {
        if (bind.publish_mode == "replace") {
            auto drop_snapshots = connection.Query("DROP TABLE IF EXISTS " + bind.snapshots_table);
            EnsureQueryOK(*drop_snapshots, "drop old ROOT dataset snapshots table");
        }
        for (const auto &entry : tables) {
            const auto relation = entry.first;
            const auto parquet = SqlLiteral(entry.second.string());
            std::string sql;
            if (bind.publish_mode == "replace") {
                auto drop = connection.Query("DROP TABLE IF EXISTS " + relation);
                EnsureQueryOK(*drop, "drop old metadata catalog table " + relation);
                sql = "CREATE TABLE " + relation + " AS SELECT * FROM read_parquet(" + parquet + ")";
            } else if (bind.publish_mode == "append") {
                auto create = connection.Query("CREATE TABLE IF NOT EXISTS " + relation +
                                               " AS SELECT * FROM read_parquet(" + parquet + ") WHERE false");
                EnsureQueryOK(*create, "create metadata catalog table " + relation);
                sql = "INSERT INTO " + relation + " SELECT * FROM read_parquet(" + parquet + ")";
            } else {
                throw InvalidInputException("publish_mode must be one of: none, replace, append");
            }
            auto result = connection.Query(sql);
            EnsureQueryOK(*result, "publish metadata table " + relation);
        }

        // The snapshots relation is the commit log.  It is written last so a
        // reader never observes a partially published four-table generation.
        auto create_snapshots = connection.Query(
            "CREATE TABLE IF NOT EXISTS " + bind.snapshots_table + " ("
            "index_version UINTEGER, dataset_id VARCHAR, snapshot_id VARCHAR, parent_snapshot_id VARCHAR, "
            "created_at_ns UBIGINT, state VARCHAR, root_glob VARCHAR, tree_name VARCHAR, logical_paths VARCHAR, "
            "files_table VARCHAR, schemas_table VARCHAR, access_table VARCHAR, baskets_table VARCHAR, "
            "chunk_id VARCHAR, manifest_fingerprint VARCHAR, dictionary_fingerprint VARCHAR)");
        EnsureQueryOK(*create_snapshots, "create ROOT dataset snapshots table");

        if (!state.manifest_fingerprint.empty()) {
            auto duplicate = connection.Query(
                "SELECT count(*) FROM " + bind.snapshots_table + " WHERE state='COMMITTED' AND "
                "manifest_fingerprint=" + SqlLiteral(state.manifest_fingerprint) +
                (state.chunk_id.empty() ? std::string() : " AND chunk_id=" + SqlLiteral(state.chunk_id)));
            EnsureQueryOK(*duplicate, "check duplicate ROOT dataset snapshot");
            if (duplicate->GetValue(0, 0).GetValue<uint64_t>() != 0) {
                throw InvalidInputException("This manifest/chunk generation is already committed");
            }
        }

        std::string parent_snapshot;
        auto parent = connection.Query(
            "SELECT snapshot_id FROM " + bind.snapshots_table + " WHERE dataset_id=" + SqlLiteral(dataset_id) +
            " AND state='COMMITTED' ORDER BY created_at_ns DESC LIMIT 1");
        EnsureQueryOK(*parent, "resolve parent ROOT dataset snapshot");
        if (parent->RowCount()) parent_snapshot = parent->GetValue(0, 0).ToString();

        uint64_t created_at_ns = 0;
        try {
            created_at_ns = static_cast<uint64_t>(std::stoull(snapshot_id));
        } catch (...) {
            created_at_ns = 0;
        }
        const auto insert_snapshot =
            "INSERT INTO " + bind.snapshots_table + " VALUES (" +
            std::to_string(ROOT_LAKE_INDEX_VERSION) + "," + SqlLiteral(dataset_id) + "," +
            SqlLiteral(snapshot_id) + "," + (parent_snapshot.empty() ? "NULL" : SqlLiteral(parent_snapshot)) + "," +
            std::to_string(created_at_ns) + ",'COMMITTED'," + SqlLiteral(bind.root_glob) + "," +
            SqlLiteral(bind.tree_name) + "," + SqlLiteral(LogicalPathsJSON(bind.logical_paths)) + "," +
            SqlLiteral(bind.files_table) + "," + SqlLiteral(bind.schemas_table) + "," +
            SqlLiteral(bind.access_table) + "," + SqlLiteral(bind.baskets_table) + "," +
            (state.chunk_id.empty() ? "NULL" : SqlLiteral(state.chunk_id)) + "," +
            (state.manifest_fingerprint.empty() ? "NULL" : SqlLiteral(state.manifest_fingerprint)) + "," +
            (state.dictionary_fingerprint.empty() ? "NULL" : SqlLiteral(state.dictionary_fingerprint)) + ")";
        auto snapshot_result = connection.Query(insert_snapshot);
        EnsureQueryOK(*snapshot_result, "commit ROOT dataset snapshot");

        auto commit = connection.Query("COMMIT");
        EnsureQueryOK(*commit, "commit metadata catalog publication");
    } catch (...) {
        auto rollback = connection.Query("ROLLBACK");
        (void)rollback;
        throw;
    }
}

static void CommitSnapshot(const BuildIndexBindData &bind, const std::string &dataset_id,
                           BuildIndexGlobalState &state, const fs::path &staging) {
    const fs::path root(bind.output_dir);
    const fs::path snapshots = root / "snapshots";
    fs::create_directories(snapshots);
    const fs::path final_snapshot = snapshots / state.snapshot_id;
    if (fs::exists(final_snapshot)) {
        if (!bind.overwrite) throw IOException("Snapshot already exists: " + final_snapshot.string());
        fs::remove_all(final_snapshot);
    }
    fs::rename(staging, final_snapshot);
    state.snapshot_dir = final_snapshot.string();

    nlohmann::json snapshot;
    snapshot["format"] = "root4duckdb-lakehouse";
    snapshot["index_version"] = ROOT_LAKE_INDEX_VERSION;
    snapshot["dataset_id"] = dataset_id;
    snapshot["snapshot_id"] = state.snapshot_id;
    snapshot["logical_paths"] = bind.logical_paths;
    snapshot["tree_name"] = bind.tree_name;
    snapshot["chunk_id"] = state.chunk_id;
    snapshot["manifest_fingerprint"] = state.manifest_fingerprint;
    snapshot["dictionary_fingerprint"] = state.dictionary_fingerprint;
    snapshot["state"] = "COMMITTED";
    const auto relative_snapshot = fs::path("snapshots") / state.snapshot_id;
    snapshot["snapshot_dir"] = relative_snapshot.string();
    snapshot["tables"] = {
        {"files", (relative_snapshot / "root_files.parquet").string()},
        {"schemas", (relative_snapshot / "root_schemas.parquet").string()},
        {"access_levels", (relative_snapshot / "root_access_levels.parquet").string()},
        {"baskets", (relative_snapshot / "root_baskets.parquet").string()}
    };

    const fs::path temp_current = root / "current.json.tmp";
    const fs::path current = root / "current.json";
    {
        std::ofstream out(temp_current, std::ios::binary);
        out << snapshot.dump(2) << '\n';
    }
    if (std::rename(temp_current.string().c_str(), current.string().c_str()) != 0) {
        const auto message = std::string(std::strerror(errno));
        std::error_code cleanup_ec;
        fs::remove(temp_current, cleanup_ec);
        throw IOException("Cannot atomically publish ROOT index manifest: " + message);
    }
    nlohmann::json success = snapshot;
    success["format"] = "root4duckdb-index-success-v1";
    success["source_file_count"] = state.statuses.size();
    const fs::path success_tmp = root / "_SUCCESS.json.tmp";
    const fs::path success_path = root / "_SUCCESS.json";
    {
        std::ofstream out(success_tmp, std::ios::binary);
        out << success.dump(2) << '\n';
    }
    if (std::rename(success_tmp.string().c_str(), success_path.string().c_str()) != 0) {
        throw IOException("Cannot atomically publish ROOT index success marker");
    }
}

static unique_ptr<FunctionData> BuildIndexBind(ClientContext &, TableFunctionBindInput &input,
                                                vector<LogicalType> &return_types, vector<string> &return_names) {
    auto result = make_uniq<BuildIndexBindData>();
    result->root_glob = input.inputs[0].ToString();
    result->tree_name = input.inputs[1].ToString();
    result->logical_path = input.inputs[2].ToString();
    result->logical_paths = ParseLogicalPaths(result->logical_path);
    result->logical_path = JoinStrings(result->logical_paths, ",");
    result->output_dir = input.inputs[3].ToString();

    auto it = input.named_parameters.find("dictionary");
    if (it != input.named_parameters.end()) result->dictionary = it->second.ToString();
    it = input.named_parameters.find("reader_mode");
    if (it != input.named_parameters.end()) result->reader_mode = ParseRootReaderMode(it->second.ToString());
    it = input.named_parameters.find("raw_validation_entries");
    if (it != input.named_parameters.end()) result->raw_validation_entries = it->second.GetValue<uint32_t>();
    it = input.named_parameters.find("raw_max_entry_bytes");
    if (it != input.named_parameters.end()) result->raw_max_entry_bytes = it->second.GetValue<uint64_t>();
    it = input.named_parameters.find("raw_max_values_per_entry");
    if (it != input.named_parameters.end()) result->raw_max_values_per_entry = it->second.GetValue<uint64_t>();
    it = input.named_parameters.find("tree_cache_bytes");
    if (it != input.named_parameters.end()) result->tree_cache_bytes = it->second.GetValue<uint64_t>();
    it = input.named_parameters.find("bloom_bytes");
    if (it != input.named_parameters.end()) {
        result->bloom_bytes = it->second.GetValue<uint32_t>();
        result->has_bloom_bytes = true;
    }
    it = input.named_parameters.find("bloom_false_positive_rate");
    if (it != input.named_parameters.end()) {
        result->bloom_false_positive_rate = it->second.GetValue<double>();
    }
    it = input.named_parameters.find("index_threads");
    if (it != input.named_parameters.end()) {
        result->index_threads = it->second.GetValue<uint32_t>();
        result->has_index_threads = true;
    }
    it = input.named_parameters.find("max_in_flight_files");
    if (it != input.named_parameters.end()) {
        result->max_in_flight_files = it->second.GetValue<uint32_t>();
        result->has_max_in_flight_files = true;
    }
    it = input.named_parameters.find("memory_budget_bytes");
    if (it != input.named_parameters.end()) {
        result->memory_budget_bytes = it->second.GetValue<uint64_t>();
        result->has_memory_budget_bytes = true;
    }
    it = input.named_parameters.find("estimated_worker_bytes");
    if (it != input.named_parameters.end()) {
        result->estimated_worker_bytes = it->second.GetValue<uint64_t>();
        result->has_estimated_worker_bytes = true;
    }
    it = input.named_parameters.find("metadata_flush_bytes");
    if (it != input.named_parameters.end()) {
        result->metadata_flush_bytes = it->second.GetValue<uint64_t>();
        result->has_metadata_flush_bytes = true;
    }
    it = input.named_parameters.find("chunk_id");
    if (it != input.named_parameters.end()) result->chunk_id = it->second.ToString();
    it = input.named_parameters.find("manifest_fingerprint");
    if (it != input.named_parameters.end()) result->manifest_fingerprint = it->second.ToString();
    it = input.named_parameters.find("dictionary_fingerprint");
    if (it != input.named_parameters.end()) result->dictionary_fingerprint = it->second.ToString();
    std::string dictionary_cleanup;
    it = input.named_parameters.find("dictionary_cleanup");
    if (it != input.named_parameters.end()) dictionary_cleanup = it->second.ToString();
    result->dictionary_cleanup_mode =
        ParseDictionaryCleanupMode(dictionary_cleanup, !result->dictionary.empty());
    it = input.named_parameters.find("overwrite");
    if (it != input.named_parameters.end()) result->overwrite = it->second.GetValue<bool>();
    it = input.named_parameters.find("allow_partial");
    if (it != input.named_parameters.end()) result->allow_partial = it->second.GetValue<bool>();
    it = input.named_parameters.find("catalog_prefix");
    if (it != input.named_parameters.end()) result->catalog_prefix = it->second.ToString();
    it = input.named_parameters.find("files_table");
    if (it != input.named_parameters.end()) result->files_table = it->second.ToString();
    it = input.named_parameters.find("schemas_table");
    if (it != input.named_parameters.end()) result->schemas_table = it->second.ToString();
    it = input.named_parameters.find("access_table");
    if (it != input.named_parameters.end()) result->access_table = it->second.ToString();
    it = input.named_parameters.find("baskets_table");
    if (it != input.named_parameters.end()) result->baskets_table = it->second.ToString();
    it = input.named_parameters.find("snapshots_table");
    if (it != input.named_parameters.end()) result->snapshots_table = it->second.ToString();
    it = input.named_parameters.find("publish_mode");
    if (it != input.named_parameters.end()) result->publish_mode = it->second.ToString();
    it = input.named_parameters.find("catalog_mode");
    if (it != input.named_parameters.end()) {
        result->catalog_mode = it->second.ToString();
        result->has_catalog_mode = true;
    }
    if (!result->catalog_prefix.empty()) {
        if (!IsSafePublishTableName(result->catalog_prefix)) {
            throw InvalidInputException("Unsafe catalog_prefix: " + result->catalog_prefix);
        }
        result->files_table = result->catalog_prefix + "_files";
        result->schemas_table = result->catalog_prefix + "_schemas";
        result->access_table = result->catalog_prefix + "_access";
        result->baskets_table = result->catalog_prefix + "_baskets";
        result->snapshots_table = result->catalog_prefix + "_snapshots";
        if (result->publish_mode == "none") result->publish_mode = "append";
    }
    const bool any_publish_table = !result->files_table.empty() || !result->schemas_table.empty() ||
                                   !result->access_table.empty() || !result->baskets_table.empty() ||
                                   !result->snapshots_table.empty();
    const bool all_publish_tables = !result->files_table.empty() && !result->schemas_table.empty() &&
                                    !result->access_table.empty() && !result->baskets_table.empty() &&
                                    !result->snapshots_table.empty();
    if (any_publish_table && !all_publish_tables) {
        throw InvalidInputException("files_table, schemas_table, access_table, baskets_table and snapshots_table must be supplied together");
    }
    if (all_publish_tables && result->publish_mode == "none") result->publish_mode = "append";
    if (result->publish_mode != "none" && result->publish_mode != "replace" && result->publish_mode != "append") {
        throw InvalidInputException("publish_mode must be one of: none, replace, append");
    }
    if (!result->has_catalog_mode && all_publish_tables) result->catalog_mode = "tables";
    if (result->catalog_mode != "local" && result->catalog_mode != "external" &&
        result->catalog_mode != "sqlite" && result->catalog_mode != "tables") {
        throw InvalidInputException("catalog_mode must be one of: local, external, sqlite, tables");
    }
    if (result->catalog_mode == "tables" && !all_publish_tables) {
        throw InvalidInputException("catalog_mode='tables' requires catalog_prefix or all five metadata tables");
    }
    if (result->catalog_mode != "tables" && result->output_dir.empty()) {
        throw InvalidInputException("catalog_mode='local', 'external' and 'sqlite' require output_dir");
    }
    if (result->catalog_mode != "tables" && result->publish_mode != "none") {
        throw InvalidInputException("publish_mode applies only to catalog_mode='tables'");
    }
    if (result->publish_mode != "none") {
        for (const auto &table : {result->files_table, result->schemas_table, result->access_table,
                                  result->baskets_table, result->snapshots_table}) {
            if (!IsSafePublishTableName(table)) throw InvalidInputException("Unsafe metadata table name: " + table);
        }
    }
    if (result->bloom_bytes > 16U * 1024U * 1024U) {
        throw InvalidInputException("bloom_bytes exceeds the 16 MiB per-basket safety limit");
    }
    if (!(result->bloom_false_positive_rate > 0.0 && result->bloom_false_positive_rate < 1.0)) {
        throw InvalidInputException("bloom_false_positive_rate must be between 0 and 1");
    }
    if (result->has_estimated_worker_bytes && result->estimated_worker_bytes == 0) {
        throw InvalidInputException("estimated_worker_bytes must be positive");
    }
    if (result->has_metadata_flush_bytes && result->metadata_flush_bytes < 16ULL * 1024ULL * 1024ULL) {
        throw InvalidInputException("metadata_flush_bytes must be at least 16 MiB");
    }
    if (result->raw_max_entry_bytes < 12) {
        throw InvalidInputException("raw_max_entry_bytes must be at least 12");
    }
    if (result->raw_max_values_per_entry == 0) {
        throw InvalidInputException("raw_max_values_per_entry must be positive");
    }

    return_names = {"file_path", "file_id", "schema_id", "entries", "flattened_values", "baskets", "status",
                    "message", "snapshot_id", "snapshot_dir", "requested_threads", "effective_threads",
                    "published", "publish_mode", "chunk_id", "manifest_fingerprint", "dictionary_fingerprint"};
    return_types = {LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::UBIGINT,
                    LogicalType::UBIGINT, LogicalType::UBIGINT, LogicalType::VARCHAR, LogicalType::VARCHAR,
                    LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::UINTEGER, LogicalType::UINTEGER,
                    LogicalType::BOOLEAN, LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR,
                    LogicalType::VARCHAR};
    return std::move(result);
}

static unique_ptr<GlobalTableFunctionState> BuildIndexInit(ClientContext &context, TableFunctionInitInput &input) {
    auto &bind = const_cast<BuildIndexBindData &>(
        input.bind_data->Cast<BuildIndexBindData>());
    auto state = make_uniq<BuildIndexGlobalState>();
    state->snapshot_id = TimestampId();
    state->publish_mode = bind.catalog_mode == "tables" ? bind.publish_mode : bind.catalog_mode;
    state->chunk_id = bind.chunk_id;
    state->manifest_fingerprint = bind.manifest_fingerprint;
    state->dictionary_fingerprint = bind.dictionary_fingerprint;

    LoadDictionary(bind.dictionary);
    const auto files = ExpandRootInputs(context, bind.root_glob);
    const auto root_runtime = RootRuntimeSettings::From(context, files.size());
    if (!bind.has_index_threads) {
        bind.index_threads = static_cast<uint32_t>(root_runtime.threads);
    }
    if (!bind.has_max_in_flight_files) {
        bind.max_in_flight_files = static_cast<uint32_t>(root_runtime.max_in_flight_files);
    }
    if (!bind.has_memory_budget_bytes) {
        bind.memory_budget_bytes = root_runtime.memory_limit_bytes;
    }
    if (!bind.has_estimated_worker_bytes) {
        bind.estimated_worker_bytes = root_runtime.estimated_worker_bytes;
    }
    if (!bind.has_bloom_bytes) {
        bind.bloom_bytes = root_runtime.bloom_bytes;
    }
    if (state->manifest_fingerprint.empty()) state->manifest_fingerprint = ManifestFingerprint(files);
    if (state->dictionary_fingerprint.empty()) {
        state->dictionary_fingerprint = FileContentFingerprint(bind.dictionary);
    }

    const auto root_class = ParsePath(bind.logical_paths.front()).root_class;
    uint64_t dataset_hash = FNV1a64(root_class, FNV1a64(bind.tree_name));
    for (const auto &file : files) dataset_hash = FNV1a64(file, dataset_hash);
    const std::string dataset_id = Hex64(dataset_hash);
    fs::path staging_root;
    if (!bind.output_dir.empty()) {
        fs::create_directories(bind.output_dir);
        staging_root = fs::path(bind.output_dir);
    } else {
        staging_root = fs::temp_directory_path() / "root4duckdb-catalog-staging";
        fs::create_directories(staging_root);
    }
    const fs::path staging = staging_root / (".staging-" + state->snapshot_id);
    if (fs::exists(staging)) fs::remove_all(staging);
    fs::create_directories(staging / "parts");

    try {
        std::vector<FilePlan> plans(files.size());
        const uint32_t hardware_threads = std::max<uint32_t>(1, std::thread::hardware_concurrency());
        state->requested_threads = bind.index_threads;
        const uint32_t requested_threads = bind.index_threads ? bind.index_threads : hardware_threads;
        const uint32_t safe_worker_cap = hardware_threads;
        const uint32_t in_flight_cap = bind.max_in_flight_files ? bind.max_in_flight_files : safe_worker_cap;
        const uint32_t memory_cap = bind.memory_budget_bytes
            ? std::max<uint32_t>(1, static_cast<uint32_t>(bind.memory_budget_bytes / bind.estimated_worker_bytes))
            : safe_worker_cap;
        const idx_t thread_count = std::max<idx_t>(
            1, std::min<idx_t>(files.size(),
                std::min<uint32_t>(requested_threads,
                    std::min<uint32_t>(safe_worker_cap, std::min<uint32_t>(in_flight_cap, memory_cap)))));
        state->effective_threads = static_cast<uint32_t>(thread_count);
        if (!bind.has_metadata_flush_bytes) {
            const auto per_worker = bind.memory_budget_bytes
                ? bind.memory_budget_bytes / std::max<uint64_t>(1, static_cast<uint64_t>(thread_count) * 8ULL)
                : 128ULL * 1024ULL * 1024ULL;
            bind.metadata_flush_bytes = std::clamp<uint64_t>(per_worker,
                16ULL * 1024ULL * 1024ULL, 256ULL * 1024ULL * 1024ULL);
        }
        std::atomic<idx_t> inspect_next {0};
        std::vector<std::thread> workers;
        workers.reserve(thread_count);
        for (idx_t worker = 0; worker < thread_count; ++worker) {
            workers.emplace_back([&]() {
                while (true) {
                    const auto index = inspect_next.fetch_add(1);
                    if (index >= files.size()) break;
                    plans[index] = InspectOneFile(bind, files[index]);
                }
            });
        }
        for (auto &worker : workers) worker.join();

        uint64_t event_base = 0;
        state->statuses.resize(files.size());
        bool inspect_failed = false;
        for (idx_t i = 0; i < plans.size(); ++i) {
            plans[i].event_base = event_base;
            event_base += plans[i].entries;
            if (!plans[i].error.empty()) {
                inspect_failed = true;
                state->statuses[i].file_path = plans[i].path;
                state->statuses[i].entries = plans[i].entries;
                state->statuses[i].status = "ERROR";
                state->statuses[i].message = plans[i].error;
            }
        }
        if (inspect_failed && !bind.allow_partial) {
            WriteFailureReport(staging_root, state->snapshot_id, state->statuses);
            throw IOException("ROOT index preflight failed; see failed-" + state->snapshot_id + ".csv");
        }

        std::atomic<idx_t> index_next {0};
        workers.clear();
        for (idx_t worker = 0; worker < thread_count; ++worker) {
            workers.emplace_back([&, worker]() {
                const auto worker_part = staging / "parts" / ("worker-" + std::to_string(worker));
                std::vector<idx_t> staged_indices;
                std::set<std::string> written_columns;
                bool writer_failed = false;
                std::string writer_error;
                std::unique_ptr<RootIndexMetadataWriter> writer;
                try {
                    writer = std::make_unique<RootIndexMetadataWriter>(
                        *context.db, (staging / "parts").string(), worker, bind.metadata_flush_bytes);
                } catch (const std::exception &ex) {
                    writer_failed = true;
                    writer_error = ex.what();
                } catch (...) {
                    writer_failed = true;
                    writer_error = "unknown typed Parquet writer initialization failure";
                }
                while (true) {
                    if (writer_failed) break;
                    const auto index = index_next.fetch_add(1);
                    if (index >= plans.size()) break;
                    if (!plans[index].error.empty()) continue;
                    try {
                        RootFileIndexMetadata metadata;
                        auto next_written_columns = written_columns;
                        auto status = IndexOneFile(bind, dataset_id, state->snapshot_id, plans[index].path,
                                                   plans[index].event_base, metadata, next_written_columns);
                        if (status.status == "OK") {
                            writer->Append(metadata);
                            written_columns = std::move(next_written_columns);
                            staged_indices.push_back(index);
                        }
                        state->statuses[index] = std::move(status);
                    } catch (const std::exception &ex) {
                        state->statuses[index].file_path = plans[index].path;
                        state->statuses[index].entries = plans[index].entries;
                        state->statuses[index].status = "ERROR";
                        state->statuses[index].message = std::string("typed Parquet writer failed: ") + ex.what();
                        writer_failed = true;
                        writer_error = ex.what();
                        break;
                    } catch (...) {
                        state->statuses[index].file_path = plans[index].path;
                        state->statuses[index].entries = plans[index].entries;
                        state->statuses[index].status = "ERROR";
                        state->statuses[index].message = "unknown typed Parquet writer failure";
                        writer_failed = true;
                        writer_error = "unknown failure";
                        break;
                    }
                }
                if (!writer_failed) {
                    try {
                        writer->Finish();
                    } catch (const std::exception &ex) {
                        writer_failed = true;
                        writer_error = ex.what();
                    }
                }
                if (writer_failed) {
                    std::error_code ec;
                    fs::remove_all(worker_part, ec);
                    for (const auto index : staged_indices) {
                        state->statuses[index].status = "ERROR";
                        state->statuses[index].message = "worker Parquet batch discarded: " + writer_error;
                    }
                }
            });
        }
        for (auto &worker : workers) worker.join();

        bool indexing_failed = false;
        idx_t success_count = 0;
        for (const auto &status : state->statuses) {
            indexing_failed |= status.status != "OK";
            success_count += status.status == "OK" ? 1 : 0;
        }
        if (success_count == 0) {
            WriteFailureReport(staging_root, state->snapshot_id, state->statuses);
            throw IOException("No ROOT file was indexed successfully; see failed-" + state->snapshot_id + ".csv");
        }
        if (indexing_failed && !bind.allow_partial) {
            WriteFailureReport(staging_root, state->snapshot_id, state->statuses);
            throw IOException("ROOT indexing failed; see failed-" + state->snapshot_id + ".csv");
        }

        CompactRootIndexParquet(*context.db, staging.string());
        fs::remove_all(staging / "parts");
        if (bind.catalog_mode == "tables") {
            PublishMetadataTables(*context.db, staging, bind, *state, dataset_id, state->snapshot_id);
            state->published = true;
            state->publish_mode = bind.publish_mode;
            state->snapshot_dir = "tables:" + (bind.catalog_prefix.empty() ? bind.snapshots_table : bind.catalog_prefix);
            fs::remove_all(staging);
        } else if (bind.catalog_mode == "sqlite") {
            // The embedded SQLite catalog consumes the typed Parquet staging
            // directly. Do not publish current.json first: a failed Iceberg
            // commit must never leave a legacy local snapshot visible.
            const auto iceberg_commit = PublishRootIndexStagingToIceberg(
                context, bind.output_dir, state->snapshot_id, staging.string(),
                state->manifest_fingerprint, state->dictionary_fingerprint);
            (void)iceberg_commit;
            fs::remove_all(staging);
            state->snapshot_dir = bind.output_dir;
            state->published = true;
            state->publish_mode = "sqlite-local";
        } else {
            CommitSnapshot(bind, dataset_id, *state, staging);
            if (bind.catalog_mode == "local") {
                state->published = true;
                state->publish_mode = "local-parquet";
            } else if (bind.catalog_mode == "external") {
                state->published = false;
                state->publish_mode = "external-staging";
            }
        }
    } catch (...) {
        std::error_code ec;
        if (!bind.output_dir.empty() && fs::exists(staging, ec)) {
            const auto failed_root = staging_root / "failed";
            fs::create_directories(failed_root, ec);
            const auto failed = failed_root / (state->snapshot_id + "-" + TimestampId());
            ec.clear();
            fs::rename(staging, failed, ec);
            // If the atomic rename is unavailable, intentionally leave the
            // original .staging directory in place.  A failed worker must not
            // destroy file-local Parquet parts that are useful for diagnosis or
            // a targeted retry; cleanup_orphans.py owns later reclamation.
        } else {
            ec.clear();
            fs::remove_all(staging, ec);
        }
        throw;
    }
    return std::move(state);
}

static void BuildIndexFunction(ClientContext &, TableFunctionInput &input, DataChunk &output) {
    auto &state = input.global_state->Cast<BuildIndexGlobalState>();
    idx_t count = 0;
    while (state.offset < state.statuses.size() && count < STANDARD_VECTOR_SIZE) {
        const auto &row = state.statuses[state.offset++];
        output.SetValue(0, count, Value(row.file_path));
        output.SetValue(1, count, Value(row.file_id));
        output.SetValue(2, count, Value(row.schema_id));
        output.SetValue(3, count, Value::UBIGINT(row.entries));
        output.SetValue(4, count, Value::UBIGINT(row.flattened_values));
        output.SetValue(5, count, Value::UBIGINT(row.baskets));
        output.SetValue(6, count, Value(row.status));
        output.SetValue(7, count, Value(row.message));
        output.SetValue(8, count, Value(state.snapshot_id));
        output.SetValue(9, count, Value(state.snapshot_dir));
        output.SetValue(10, count, Value::UINTEGER(state.requested_threads));
        output.SetValue(11, count, Value::UINTEGER(state.effective_threads));
        output.SetValue(12, count, Value::BOOLEAN(state.published));
        output.SetValue(13, count, Value(state.publish_mode));
        output.SetValue(14, count, Value(state.chunk_id));
        output.SetValue(15, count, Value(state.manifest_fingerprint));
        output.SetValue(16, count, Value(state.dictionary_fingerprint));
        ++count;
    }
    output.SetCardinality(count);
}

static TableFunction MakeBuildIndexFunction(const std::string &name) {
    TableFunction function(name,
                           {LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR},
                           BuildIndexFunction, BuildIndexBind, BuildIndexInit);
    function.named_parameters["dictionary"] = LogicalType::VARCHAR;
    function.named_parameters["dictionary_cleanup"] = LogicalType::VARCHAR;
    function.named_parameters["reader_mode"] = LogicalType::VARCHAR;
    function.named_parameters["raw_validation_entries"] = LogicalType::UINTEGER;
    function.named_parameters["raw_max_entry_bytes"] = LogicalType::UBIGINT;
    function.named_parameters["raw_max_values_per_entry"] = LogicalType::UBIGINT;
    function.named_parameters["tree_cache_bytes"] = LogicalType::UBIGINT;
    function.named_parameters["bloom_bytes"] = LogicalType::UINTEGER;
    function.named_parameters["bloom_false_positive_rate"] = LogicalType::DOUBLE;
    function.named_parameters["index_threads"] = LogicalType::UINTEGER;
    function.named_parameters["max_in_flight_files"] = LogicalType::UINTEGER;
    function.named_parameters["memory_budget_bytes"] = LogicalType::UBIGINT;
    function.named_parameters["estimated_worker_bytes"] = LogicalType::UBIGINT;
    function.named_parameters["metadata_flush_bytes"] = LogicalType::UBIGINT;
    function.named_parameters["chunk_id"] = LogicalType::VARCHAR;
    function.named_parameters["manifest_fingerprint"] = LogicalType::VARCHAR;
    function.named_parameters["dictionary_fingerprint"] = LogicalType::VARCHAR;
    function.named_parameters["overwrite"] = LogicalType::BOOLEAN;
    function.named_parameters["allow_partial"] = LogicalType::BOOLEAN;
    function.named_parameters["catalog_prefix"] = LogicalType::VARCHAR;
    function.named_parameters["files_table"] = LogicalType::VARCHAR;
    function.named_parameters["schemas_table"] = LogicalType::VARCHAR;
    function.named_parameters["access_table"] = LogicalType::VARCHAR;
    function.named_parameters["baskets_table"] = LogicalType::VARCHAR;
    function.named_parameters["snapshots_table"] = LogicalType::VARCHAR;
    function.named_parameters["publish_mode"] = LogicalType::VARCHAR;
    function.named_parameters["catalog_mode"] = LogicalType::VARCHAR;
    return function;
}

void RegisterRootLakeIndex(ExtensionLoader &loader) {
    auto single_or_multi = MakeBuildIndexFunction("root_build_index");
    loader.RegisterFunction(single_or_multi);
    // Production-facing alias: accepts file, directory, glob, JSON array,
    // comma-separated masks, or @URI-list and indexes all logical paths in one
    // top-level object read per entry.
    auto dataset = MakeBuildIndexFunction("root_build_dataset_index");
    loader.RegisterFunction(dataset);
}

} // namespace duckdb::rootlake
