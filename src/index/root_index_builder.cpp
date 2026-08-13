#include "root4duckdb/index/root_index_builder.hpp"

#include "root4duckdb/index/root_bloom.hpp"
#include "root4duckdb/reader/root_branch_projection.hpp"
#include "root4duckdb/core/root_lake_common.hpp"
#include "root4duckdb/reader/root_path_reader.hpp"
#include "root4duckdb/core/root_headers.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <memory>
#include <sys/stat.h>
#include <unordered_map>
#include <utility>

namespace duckdb::rootlake {
namespace {

uint64_t LocalFileSize(const std::string& path) {
    if (path.find("://") != std::string::npos) {
        return 0;
    }
    struct stat status {};
    if (::stat(path.c_str(), &status) != 0 || status.st_size < 0) {
        return 0;
    }
    return static_cast<uint64_t>(status.st_size);
}

int64_t LocalMtimeNS(const std::string& path) {
    if (path.find("://") != std::string::npos) {
        return 0;
    }
    struct stat status {};
    if (::stat(path.c_str(), &status) != 0) {
        return 0;
    }
#if defined(__APPLE__)
    return static_cast<int64_t>(status.st_mtimespec.tv_sec) * 1000000000LL +
           static_cast<int64_t>(status.st_mtimespec.tv_nsec);
#else
    return static_cast<int64_t>(status.st_mtim.tv_sec) * 1000000000LL + static_cast<int64_t>(status.st_mtim.tv_nsec);
#endif
}

void WriteSchemaRows(RootFileIndexMetadata& metadata, const std::string& schema_id, const std::string& column_id,
                     const std::string& logical_path, const ParsedPath& path, const std::vector<PathLevel>& levels) {
    const std::string access_plan_id = Hex64(FNV1a64(logical_path, FNV1a64(schema_id)) ^ 0x9f6abc31ULL);
    const auto leaf_type = PrimitiveBaseType(levels.back().type);

    RootSchemaMetadataRow schema;
    schema.index_version = ROOT_LAKE_INDEX_VERSION;
    schema.schema_id = schema_id;
    schema.column_id = column_id;
    schema.logical_path = logical_path;
    schema.root_class = path.root_class;
    schema.root_type = leaf_type;
    schema.duckdb_type = RootTypeToLogicalType(leaf_type).ToString();
    schema.access_plan_id = access_plan_id;
    schema.index_signature = IndexSignature(levels);
    schema.container_depth = static_cast<uint32_t>(IndexDepth(levels));
    metadata.schemas.push_back(std::move(schema));

    for (idx_t i = 0; i < levels.size(); ++i) {
        const auto& level = levels[i];
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
        access.element_size = level.element_size;
        metadata.access.push_back(std::move(access));
    }
}

struct BasketAccumulator {
    BasketAccumulator(uint32_t bloom_bytes, double bloom_fpr, uint32_t id)
        : basket_id(id), bloom(bloom_bytes, bloom_fpr) {
    }

    void Add(const RootPrimitiveValue& value, bool statistics_enabled) {
        ++value_count;

        if (!statistics_enabled) {
            return;
        }

        const double number = value.AsDouble();

        if (std::isnan(number)) {
            ++nan_count;
            return;
        }

        if (std::isinf(number)) {
            if (number > 0) {
                ++pos_inf_count;
            } else {
                ++neg_inf_count;
            }
            return;
        }

        min_value = std::min(min_value, number);
        max_value = std::max(max_value, number);
        bloom.Add(number);
        ++finite_count;
    }

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
};

struct IndexedPathPlan {
    std::string logical_path;
    RootPathReader reader;
    std::string schema_id;
    std::string column_id;
    std::vector<BasketAccumulator> baskets;
    idx_t active_basket = 0;
    bool statistics_double_safe = true;
};

void ApplyRootBasketMetadata(BasketAccumulator& basket, TBasket* root_basket) {
    if (!root_basket) {
        return;
    }
    basket.key_length = static_cast<uint32_t>(std::max(0, root_basket->GetKeylen()));
    basket.uncompressed_size = static_cast<uint32_t>(std::max(0, root_basket->GetObjlen()));
    if (!basket.compressed_size) {
        basket.compressed_size = static_cast<uint32_t>(std::max(0, root_basket->GetNbytes()));
    }
    if (!basket.physical_offset) {
        basket.physical_offset = static_cast<uint64_t>(std::max<Long64_t>(0, root_basket->GetSeekKey()));
    }
}

void ApplySerializedBasketMetadata(BasketAccumulator& basket, const SerializedBasketInfo& info) {
    if (info.basket_number != static_cast<int32_t>(basket.basket_id)) {
        return;
    }
    basket.key_length = info.key_length;
    basket.uncompressed_size = info.uncompressed_size;
    if (!basket.compressed_size) {
        basket.compressed_size = info.compressed_size;
    }
    if (!basket.physical_offset) {
        basket.physical_offset = info.physical_offset;
    }
}

IndexedPathPlan PreparePath(const RootIndexBuildOptions& options, TBranch* object_branch, TClass* root_class,
                            uint64_t total_entries, const std::string& logical_path) {
    IndexedPathPlan plan;
    plan.logical_path = logical_path;
    auto parsed = ParsePath(logical_path);
    auto levels = PathResolver::Resolve(root_class, parsed.fields);
    if (!levels.back().is_primitive) {
        throw NotImplementedException("Indexed ROOT paths require a primitive numeric leaf; "
                                      "unsupported leaf type for " +
                                      logical_path + ": " + levels.back().type);
    }

    plan.statistics_double_safe = IsLosslessDoubleBackedType(levels.back().type);
    plan.schema_id = SchemaFingerprint(parsed.root_class, levels);
    plan.column_id = ColumnId(plan.schema_id, logical_path);
    plan.reader.Resolve(nullptr, object_branch, root_class, std::move(parsed), std::move(levels));
    auto* physical_branch = plan.reader.PhysicalBranch();
    if (!physical_branch) {
        throw InvalidInputException("No persistent branch can provide entry ranges for " + logical_path);
    }

    const int basket_count = physical_branch->GetWriteBasket() + 1;
    auto* basket_entries = physical_branch->GetBasketEntry();
    auto* basket_bytes = physical_branch->GetBasketBytes();
    if (basket_count <= 0 || !basket_entries) {
        throw InvalidInputException("Physical branch has no persistent baskets: " +
                                    std::string(physical_branch->GetName()));
    }
    plan.baskets.reserve(static_cast<idx_t>(basket_count));
    for (int basket_id = 0; basket_id < basket_count; ++basket_id) {
        const uint64_t entry_begin = static_cast<uint64_t>(basket_entries[basket_id]);
        uint64_t entry_end =
            basket_id + 1 < basket_count ? static_cast<uint64_t>(basket_entries[basket_id + 1]) : total_entries;
        entry_end = std::min(entry_end, total_entries);
        if (entry_begin >= entry_end) {
            continue;
        }
        plan.baskets.emplace_back(options.bloom_bytes, options.bloom_false_positive_rate,
                                  static_cast<uint32_t>(basket_id));
        auto& basket = plan.baskets.back();
        basket.entry_begin = entry_begin;
        basket.entry_end = entry_end;
        const auto basket_seek = physical_branch->GetBasketSeek(basket_id);
        basket.physical_offset = basket_seek > 0 ? static_cast<uint64_t>(basket_seek) : 0;
        basket.compressed_size = basket_bytes ? static_cast<uint32_t>(std::max(0, basket_bytes[basket_id])) : 0;
        const bool serialized_metadata =
            options.reader_mode != RootReaderMode::OBJECT && plan.reader.SerializedPlan().supported;
        if (!serialized_metadata) {
            ApplyRootBasketMetadata(basket, physical_branch->GetBasket(basket_id));
            physical_branch->DropBaskets();
        }
    }
    return plan;
}

} // namespace

RootIndexFilePlan InspectRootIndexFile(const RootIndexBuildOptions& options, const std::string& path) {
    RootIndexFilePlan plan;
    plan.path = path;
    try {
        std::unique_ptr<TFile> file(TFile::Open(path.c_str(), "READ"));
        if (!file || file->IsZombie()) {
            throw IOException("ROOT file is zombie");
        }
        const auto parsed = ParsePath(options.logical_paths.front());
        auto* tree = FindTree(file.get(), options.tree_name, parsed.root_class);
        if (!tree) {
            throw InvalidInputException("No TTree found");
        }
        plan.entries = static_cast<uint64_t>(tree->GetEntries());
    } catch (const std::exception& exception) {
        plan.error = exception.what();
    }
    return plan;
}

RootIndexFileBuilder::RootIndexFileBuilder(const RootIndexBuildOptions& options_p, std::string dataset_id_p,
                                           std::string snapshot_id_p)
    : options(options_p), dataset_id(std::move(dataset_id_p)), snapshot_id(std::move(snapshot_id_p)) {
}

RootIndexBuildStatus RootIndexFileBuilder::Build(const std::string& root_path, uint64_t event_base,
                                                 RootFileIndexMetadata& metadata,
                                                 std::set<std::string>& written_columns) const {
    RootIndexBuildStatus status;
    status.file_path = root_path;
    try {
        std::unique_ptr<TFile> file(TFile::Open(root_path.c_str(), "READ"));
        if (!file || file->IsZombie()) {
            throw IOException("ROOT file is zombie");
        }

        const auto root_path_spec = ParsePath(options.logical_paths.front());
        RootObjectReader object_reader;
        object_reader.Bind(file.get(), options.tree_name, root_path_spec.root_class, options.dictionary_cleanup_mode);
        auto* tree = object_reader.Tree();
        auto* object_branch = object_reader.ObjectBranch();
        auto* root_class = object_reader.RootClass();

        const uint64_t total_entries = static_cast<uint64_t>(tree->GetEntries());
        status.entries = total_entries;
        const uint64_t file_size = LocalFileSize(root_path);
        const int64_t mtime_ns = LocalMtimeNS(root_path);
        const auto file_id = FileId(root_path, file_size, mtime_ns);
        status.file_id = file_id;

        std::vector<IndexedPathPlan> paths;
        paths.reserve(options.logical_paths.size());
        for (const auto& logical_path : options.logical_paths) {
            paths.push_back(PreparePath(options, object_branch, root_class, total_entries, logical_path));
        }

        std::vector<TBranch*> projected_branches;
        bool projection_safe = true;
        for (const auto& path : paths) {
            projected_branches.push_back(path.reader.PhysicalBranch());
            projection_safe = projection_safe && path.reader.PhysicalMode() == "ancestor";
        }
        const auto projection = projection_safe
                                    ? ApplyBranchProjection(tree, projected_branches, options.tree_cache_bytes)
                                    : BranchProjectionResult{};
        if (!projection.applied) {
            EnableAllBranches(tree, options.tree_cache_bytes);
        }

        uint64_t serialized_path_count = 0;
        uint64_t fallback_path_count = 0;
        std::unordered_map<TBranch*, idx_t> paths_per_branch;
        for (const auto& path : paths) {
            ++paths_per_branch[path.reader.PhysicalBranch()];
        }
        for (auto& path : paths) {
            std::string rejection;
            if (paths_per_branch[path.reader.PhysicalBranch()] > 1) {
                rejection = "multiple requested paths share one physical ancestor; universal one-pass read is cheaper";
            }
            RootPathReaderOptions reader_options;
            reader_options.reader_mode = options.reader_mode;
            reader_options.validation_entries = options.raw_validation_entries;
            reader_options.max_entry_bytes = options.raw_max_entry_bytes;
            reader_options.max_values_per_entry = options.raw_max_values_per_entry;
            reader_options.tree_cache_bytes = options.tree_cache_bytes;
            reader_options.operation = "index";
            const auto started = path.reader.StartSerialized(object_reader.CurrentObject(), std::move(reader_options),
                                                             std::move(rejection));
            if (started.serialized_active) {
                ++serialized_path_count;
            }
            if (started.fallback_activated) {
                ++fallback_path_count;
            }
        }

        std::vector<RootPrimitiveValue> values;
        std::vector<int32_t> indices;
        RootEntryReader object_entry(object_reader);
        for (uint64_t entry = 0; entry < total_entries; ++entry) {
            bool need_object = options.reader_mode == RootReaderMode::OBJECT;
            for (const auto& path : paths) {
                if (!path.reader.SerializedActive() || path.reader.ValidationRemaining() > 0) {
                    need_object = true;
                    break;
                }
            }
            object_entry.Begin(entry);
            if (need_object) {
                object_entry.Read();
            }

            for (auto& path : paths) {
                while (path.active_basket < path.baskets.size() &&
                       entry >= path.baskets[path.active_basket].entry_end) {
                    ++path.active_basket;
                }
                if (path.active_basket >= path.baskets.size()) {
                    continue;
                }
                auto& basket = path.baskets[path.active_basket];
                if (entry < basket.entry_begin || entry >= basket.entry_end) {
                    continue;
                }

                values.clear();
                indices.clear();
                if (path.reader.SerializedActive()) {
                    const auto read = path.reader.TryReadSerialized(entry, object_entry, values, indices,
                                                                    path.reader.ValidationRemaining() > 0);
                    if (read.fallback_activated) {
                        ++fallback_path_count;
                    }
                    SerializedBasketInfo basket_info;
                    if (path.reader.CurrentBasketInfo(basket_info)) {
                        ApplySerializedBasketMetadata(basket, basket_info);
                    }
                }

                if (!path.reader.SerializedActive()) {
                    values.clear();
                    void* object = object_entry.Read();
                    if (!object) {
                        ++basket.null_count;
                        continue;
                    }
                    path.reader.CollectTypedValues(object, values);
                }
                for (const auto& value : values) {
                    basket.Add(value, path.statistics_double_safe);
                }
            }
        }

        for (auto& path : paths) {
            for (auto& basket : path.baskets) {
                if (basket.key_length && basket.uncompressed_size) {
                    continue;
                }
                auto* physical_branch = path.reader.PhysicalBranch();
                ApplyRootBasketMetadata(basket, physical_branch->GetBasket(static_cast<int>(basket.basket_id)));
                physical_branch->DropBaskets();
            }
        }

        std::vector<std::string> schema_ids;
        for (auto& path : paths) {
            schema_ids.push_back(path.schema_id);
            if (written_columns.insert(path.column_id).second) {
                WriteSchemaRows(metadata, path.schema_id, path.column_id, path.logical_path, path.reader.Path(),
                                path.reader.Levels());
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

            for (auto& basket : path.baskets) {
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
                row.basket_branch_name = path.reader.PhysicalBranch()->GetName();
                row.basket_branch_mode = path.reader.PhysicalMode();
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
                row.bloom_filter = path.statistics_double_safe ? basket.bloom.SerializeAndRelease() : std::string{};
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
                         ", object_reads=" + std::to_string(object_entry.LoadCount());
    } catch (const std::exception& exception) {
        status.status = "ERROR";
        status.message = exception.what();
    }
    return status;
}

} // namespace duckdb::rootlake
