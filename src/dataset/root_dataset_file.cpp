#include "root4duckdb/dataset/root_dataset_scan_internal.hpp"

#include "root4duckdb/core/root_file_opener.hpp"
#include "root4duckdb/serialized/root_serialized_basket_cache.hpp"

#include <memory>
#include <unordered_map>
#include <utility>

namespace duckdb::rootlake
{

void DatasetScanExecutor::OpenTaskFile(const DatasetBindData& bind, DatasetGlobalState& global,
                                       DatasetLocalState& local, const RootBasketTask& task) const
{
    if (local.open_uri == task.root_uri && local.open_schema == task.schema_id && local.file)
    {
        global.reused_open_files.fetch_add(1);
        return;
    }
    SyncSerializedCounters(local, global);
    local.path_reader.EndObjectEntry();
    local.path_reader.Reset();
    // Detach all predicate ROOT bindings before the source TFile is released.
    local.predicate_readers.clear();
    local.object_reader.Reset();
    local.reported_serialized_baskets = 0;
    local.reported_serialized_compressed_bytes = 0;
    local.reported_serialized_entry_bytes = 0;
    local.reported_nested_fixed_direct = 0;
    local.reported_nested_fixed_scratch = 0;
    auto opened = OpenRootFile(task.root_uri);
    global.opened_files.fetch_add(1);
    if (!opened)
    {
        throw IOException("Failed to open ROOT file: " + task.root_uri + ": " + opened.error);
    }
    local.file = std::move(opened.file);
    auto schema_it = bind.schema_lookup.find(task.schema_id);
    if (schema_it == bind.schema_lookup.end())
    {
        throw IOException("Unknown schema_id in task: " + task.schema_id);
    }
    const auto& schema = bind.schemas[schema_it->second];
    local.object_reader.Bind(local.file.get(), task.tree_name, schema.root_class,
                             bind.root_access.dictionary_cleanup_mode);
    auto parsed = ParsePath(bind.logical_path);
    auto* root_class = local.object_reader.RootClass();
    auto* tree = local.object_reader.Tree();
    auto* object_branch = local.object_reader.ObjectBranch();
    auto full_levels = PathResolver::Resolve(root_class, parsed.fields);
    ValidateAccessPlan(schema, full_levels);
    local.predicate_levels.clear();
    local.predicate_levels.reserve(bind.path_predicates.size());
    local.predicate_readers.clear();
    local.predicate_readers.reserve(bind.path_predicates.size());
    for (const auto& predicate : bind.path_predicates)
    {
        const auto predicate_path = ParsePath(predicate.path);
        if (predicate_path.root_class != schema.root_class)
        {
            throw IOException("Predicate path root class differs from target path root class: " + predicate.path);
        }
        auto predicate_levels = PathResolver::Resolve(root_class, predicate_path.fields);
        const auto predicate_schema_id = SchemaFingerprint(predicate_path.root_class, predicate_levels);
        const auto predicate_schema_it = predicate.schema_lookup.find(predicate_schema_id);
        if (predicate_schema_it == predicate.schema_lookup.end())
        {
            throw IOException("ROOT file streamer schema is absent from predicate index for " + predicate.path);
        }
        ValidateAccessPlan(predicate.schemas[predicate_schema_it->second], predicate_levels);
        RootPathReader predicate_reader;
        predicate_reader.Resolve(tree, object_branch, root_class, predicate_path, predicate_levels);
        local.predicate_readers.push_back(std::move(predicate_reader));
        local.predicate_levels.push_back(std::move(predicate_levels));
    }
    local.path_reader.Resolve(tree, object_branch, root_class, std::move(parsed), std::move(full_levels));
    // Shared basket bytes for predicate and returned-value projections.
    std::unordered_map<TBranch*, std::shared_ptr<SerializedBasketCache>> caches;
    local.path_reader.AttachSharedBasketCache(caches);
    for (auto& reader : local.predicate_readers)
    {
        reader.AttachSharedBasketCache(caches);
    }
    bool projection_applied = false;
    if (bind.path_predicates.empty() && local.path_reader.PhysicalMode() == "ancestor")
    {
        const auto projection =
            ApplyBranchProjection(tree, {local.path_reader.PhysicalBranch()}, local.file_cache_bytes);
        projection_applied = projection.applied;
        if (projection_applied)
        {
            global.projected_files.fetch_add(1);
        }
    }
    if (!projection_applied)
    {
        EnableAllBranches(tree, local.file_cache_bytes);
    }
    auto reader_options = bind.root_access;
    reader_options.tree_cache_bytes = local.file_cache_bytes;
    reader_options.enable_all_branches_on_fallback = true;
    const auto started = local.path_reader.StartSerialized(reader_options);
    for (auto& predicate_reader : local.predicate_readers)
    {
        predicate_reader.StartSerialized(reader_options);
    }
    if (started.FallbackActivated())
    {
        global.fallback_files.fetch_add(1);
    }
    const auto runtime_index_depth = local.path_reader.IndexDepth();
    if (runtime_index_depth != bind.index_names.size())
    {
        throw IOException("ROOT container depth differs from the catalog output schema for " + bind.logical_path);
    }
    local.open_uri = task.root_uri;
    local.open_schema = task.schema_id;
}

} // namespace duckdb::rootlake
