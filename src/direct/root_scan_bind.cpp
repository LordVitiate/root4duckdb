#include "root4duckdb/direct/root_scan_internal.hpp"

namespace duckdb {

void RootScanBinder::AddEventIdColumn(RootScanBindData& bind_data, std::vector<std::string>& return_names,
                                      std::vector<LogicalType>& return_types) {
    return_names.emplace_back("event_id");
    return_types.emplace_back(LogicalType(LogicalTypeId::BIGINT));

    RootScanColumn col;
    col.name = "event_id";
    bind_data.columns.emplace_back(std::move(col));
}

void RootScanBinder::BindDirectPrimitives(RootScanBindData& bind_data, const std::string& path_prefix,
                                          const std::vector<std::string>& matching_paths,
                                          std::vector<std::string>& return_names,
                                          std::vector<LogicalType>& return_types) {
    RootDebug("BIND.PRIMITIVES_BEGIN",
              "prefix=" + path_prefix + " matching_paths=" + std::to_string(matching_paths.size()));
    struct ResolvedColumn {
        RootScanColumn column;
        LogicalType duckdb_type;
    };

    std::vector<ResolvedColumn> resolved_columns;
    std::set<std::string> seen_value_names;
    std::vector<std::string> ordered_index_names;
    std::set<std::string> seen_index_names;

    for (const auto& full_path : matching_paths) {
        RootDebug("BIND.PRIMITIVE_PATH", "path=" + full_path);
        std::string rest = full_path.size() < path_prefix.size() ? std::string() : full_path.substr(path_prefix.size());
        if (rest.find('/') != std::string::npos) {
            continue;
        }

        std::string flat_name = rest;
        if (flat_name.empty()) {
            std::string clean_prefix = path_prefix;
            if (!clean_prefix.empty() && clean_prefix.back() == '/') {
                clean_prefix.pop_back();
            }
            if (clean_prefix.size() >= 6 && clean_prefix.substr(clean_prefix.size() - 6) == "/value") {
                clean_prefix.resize(clean_prefix.size() - 6);
            }
            const size_t last_slash = clean_prefix.find_last_of('/');
            flat_name = last_slash == std::string::npos ? clean_prefix : clean_prefix.substr(last_slash + 1);
        }
        for (char& c : flat_name) {
            if (c == '/') {
                c = '_';
            }
        }

        std::string lower_name = flat_name;
        std::transform(lower_name.begin(), lower_name.end(), lower_name.begin(), ::tolower);
        if (!seen_value_names.insert(lower_name).second) {
            continue;
        }

        const auto parsed = rootlake::ParsePathPrefix(full_path);
        if (parsed.fields.empty()) {
            continue;
        }
        RootDebug("TCLASS.BEFORE", "GetClass name=" + parsed.root_class + " source=" + full_path);
        auto* cls = TClass::GetClass(parsed.root_class.c_str());
        RootDebug("TCLASS.AFTER", "name=" + parsed.root_class + " ptr=" + RootPointer(cls) +
                                      " has_dictionary=" + std::to_string(cls && cls->HasDictionary() ? 1 : 0));
        if (!cls || !cls->HasDictionary()) {
            continue;
        }
        auto levels = rootlake::PathResolver::TryResolve(cls, parsed.fields);
        if (levels.empty()) {
            continue;
        }
        const auto& leaf = levels.back();
        if (!leaf.is_primitive && !leaf.is_string) {
            continue;
        }

        RootScanColumn col;
        col.name = flat_name;
        col.logical_path = full_path;
        col.branch_name = parsed.root_class;
        col.root_type = leaf.type;
        col.is_string = leaf.is_string;
        col.levels = std::move(levels);
        col.index_signature = rootlake::IndexSignature(col.levels);

        if (!col.index_signature.empty()) {
            std::stringstream signature(col.index_signature);
            std::string index_name;
            while (std::getline(signature, index_name, ',')) {
                std::string lower_index = index_name;
                std::transform(lower_index.begin(), lower_index.end(), lower_index.begin(), ::tolower);
                if (seen_index_names.insert(lower_index).second) {
                    ordered_index_names.push_back(index_name);
                }
            }
        }

        ResolvedColumn resolved;
        resolved.duckdb_type = rootlake::RootTypeToScanLogicalType(col.root_type, col.is_string, true);
        resolved.column = std::move(col);
        RootDebug("BIND.COLUMN_READY", "name=" + resolved.column.name + " root_type=" + resolved.column.root_type +
                                           " signature=" + resolved.column.index_signature);
        resolved_columns.emplace_back(std::move(resolved));
    }

    AddEventIdColumn(bind_data, return_names, return_types);

    const std::string root_class_name = rootlake::ParsePathPrefix(path_prefix).root_class;
    for (const auto& index_name : ordered_index_names) {
        RootScanColumn index_column;
        index_column.name = index_name;
        index_column.is_virtual_index = true;
        index_column.branch_name = root_class_name;
        bind_data.columns.emplace_back(std::move(index_column));
        return_names.emplace_back(index_name);
        return_types.emplace_back(LogicalType(LogicalTypeId::INTEGER));
    }

    for (auto& resolved : resolved_columns) {
        return_names.emplace_back(resolved.column.name);
        return_types.emplace_back(resolved.duckdb_type);
        bind_data.columns.emplace_back(std::move(resolved.column));
    }

    RootDebug("BIND.PRIMITIVES_END", "columns=" + std::to_string(bind_data.columns.size()) +
                                         " return_names=" + std::to_string(return_names.size()));
}

void RootScanBinder::BindBrowseMode(RootScanBindData& bind_data, const std::string& path_prefix,
                                    const std::set<std::string>& direct_children,
                                    std::vector<std::string>& return_names, std::vector<LogicalType>& return_types) {
    std::vector<std::string> children;
    return_names.emplace_back("path");
    return_types.emplace_back(LogicalType(LogicalTypeId::VARCHAR));

    for (const auto& child_full_path : direct_children) {
        std::string folder_name = child_full_path.substr(path_prefix.size());
        if (!folder_name.empty() && folder_name.back() == '/') {
            folder_name.pop_back();
        }
        if (folder_name.empty()) {
            continue;
        }
        children.push_back(std::move(folder_name));
    }
    bind_data.SelectBrowseMode(std::move(children));
}

void RootScanBinder::BindEmptyResult(RootScanBindData& bind_data, const std::string& path_prefix,
                                     std::vector<std::string>& return_names, std::vector<LogicalType>& return_types) {
    bind_data.SelectEmptyMode();
    bind_data.total_rows = 0;
    AddEventIdColumn(bind_data, return_names, return_types);
    if (!path_prefix.empty()) {
        std::string clean_prefix = path_prefix;
        if (clean_prefix.back() == '/') {
            clean_prefix.pop_back();
        }
        size_t last_slash = clean_prefix.find_last_of('/');
        std::string container_name =
            (last_slash == std::string::npos) ? clean_prefix : clean_prefix.substr(last_slash + 1);

        if (container_name.find("vec") == 0 || container_name.find("set") == 0) {
            std::string idx_name = container_name + "_idx";
            RootScanColumn idx_col;
            idx_col.name = idx_name;
            idx_col.is_virtual_index = true;
            idx_col.branch_name = rootlake::ParsePathPrefix(path_prefix).root_class;
            bind_data.columns.emplace_back(std::move(idx_col));
            return_names.emplace_back(idx_name);
            return_types.emplace_back(LogicalType(LogicalTypeId::INTEGER));
        }
    }
}

bool RootScanBinder::LoadRequestedDictionary(ClientContext& context, TableFunctionBindInput& input) {
    auto it = input.named_parameters.find("dictionary");
    if (it == input.named_parameters.end()) {
        RootDebug("DICT.NONE", "dictionary parameter was not supplied");
        return false;
    }
    const std::string dict_path = it->second.ToString();
    if (dict_path.empty()) {
        RootDebug("DICT.EMPTY", "dictionary parameter is empty");
        return false;
    }
    return rootlake::LoadRootDictionary(context, dict_path);
}

bool RootScanBinder::BindSemanticPath(RootScanBindData& bind_data, TFile* file, const std::string& path_prefix_raw,
                                      std::vector<std::string>& return_names, std::vector<LogicalType>& return_types) {
    RootDebug("SEMANTIC.BEGIN", "path=" + path_prefix_raw + " file_ptr=" + RootPointer(file));

    const auto parsed = rootlake::ParsePathPrefix(path_prefix_raw);
    RootDebug("SEMANTIC.PARSED", "root_class=" + parsed.root_class + " fields=" + JoinDebugFields(parsed.fields));
    if (parsed.root_class.empty()) {
        return false;
    }

    RootDebug("TCLASS.BEFORE", "GetClass name=" + parsed.root_class + " semantic_bind=1");
    auto* root_class = TClass::GetClass(parsed.root_class.c_str());
    RootDebug("TCLASS.AFTER", "name=" + parsed.root_class + " ptr=" + RootPointer(root_class) + " has_dictionary=" +
                                  std::to_string(root_class && root_class->HasDictionary() ? 1 : 0));
    if (!root_class || !root_class->HasDictionary()) {
        return false;
    }
    RootDebug("TREE.BEFORE_FIND", "class=" + parsed.root_class);
    auto* tree = rootlake::FindTree(file, "", parsed.root_class);
    RootDebug("TREE.AFTER_FIND", "class=" + parsed.root_class + " tree_ptr=" + RootPointer(tree) +
                                     " tree_name=" + std::string(tree ? tree->GetName() : "<null>"));
    auto* branch = rootlake::FindObjectBranch(tree, parsed.root_class);
    RootDebug("BRANCH.AFTER_FIND", "class=" + parsed.root_class + " branch_ptr=" + RootPointer(branch) +
                                       " branch_name=" + std::string(branch ? branch->GetName() : "<null>"));
    if (!tree || !branch) {
        return false;
    }

    rootlake::SemanticPathSelection selection;
    RootDebug("SEMANTIC.BEFORE_SELECT", "path=" + path_prefix_raw);
    if (!rootlake::SelectSemanticPath(root_class, parsed, path_prefix_raw, selection)) {
        return false;
    }

    RootDebug("SEMANTIC.AFTER_SELECT", "bind_prefix=" + selection.bind_prefix +
                                           " primitive_paths=" + std::to_string(selection.primitive_paths.size()) +
                                           " direct_children=" + std::to_string(selection.child_paths.size()));
    bind_data.tree_name = tree->GetName();
    bind_data.total_rows = static_cast<uint64_t>(tree->GetEntries());

    if (!selection.primitive_paths.empty()) {
        BindDirectPrimitives(bind_data, selection.bind_prefix, selection.primitive_paths, return_names, return_types);
        if (bind_data.columns.size() > 1) {
            RootDebug("SEMANTIC.SUCCESS", "mode=primitive columns=" + std::to_string(bind_data.columns.size()));
            return true;
        }

        // An exact non-scalar path must not trigger unrelated schema discovery.
        bind_data.columns.clear();
        return_names.clear();
        return_types.clear();
        return false;
    }

    if (!selection.child_paths.empty()) {
        BindBrowseMode(bind_data, selection.bind_prefix, selection.child_paths, return_names, return_types);
        RootDebug("SEMANTIC.SUCCESS", "mode=browse children=" + std::to_string(selection.child_paths.size()));
        return true;
    }
    return false;
}

std::unique_ptr<TFile> RootScanBinder::OpenRepresentativeFile(RootScanBindData& bind_data) {
    std::vector<std::string> failures;
    for (idx_t source_id = 0; source_id < bind_data.root_paths.size(); ++source_id) {
        RootDebug("FILE.BEFORE_OPEN", "mode=representative source_id=" + std::to_string(source_id) +
                                          " path=" + bind_data.root_paths[source_id]);
        auto result = rootlake::OpenRootFile(bind_data.root_paths[source_id]);
        bind_data.bind_open_us += result.elapsed_us;
        RootDebug("FILE.AFTER_OPEN", "mode=representative source_id=" + std::to_string(source_id) +
                                         " attempts=" + std::to_string(result.attempts) +
                                         " elapsed_us=" + std::to_string(result.elapsed_us) +
                                         " zombie=" + std::to_string(result.file && result.file->IsZombie() ? 1 : 0));
        if (result) {
            bind_data.representative_source_id = source_id;
            bind_data.root_path = bind_data.root_paths[source_id];
            return std::move(result.file);
        }
        failures.push_back(bind_data.root_paths[source_id] + ": " + result.error);
    }
    std::ostringstream message;
    message << "Failed to open every ROOT input while selecting a representative file";
    const auto limit = std::min<size_t>(failures.size(), 4);
    for (size_t index = 0; index < limit; ++index) {
        message << "; " << failures[index];
    }
    if (failures.size() > limit) {
        message << "; ...";
    }
    throw IOException(message.str());
}

void RootScanBinder::AddMultiFileIdentityColumns(RootScanBindData& bind_data, std::vector<std::string>& return_names,
                                                 std::vector<LogicalType>& return_types) {
    if (!bind_data.IsMultiFile() || bind_data.IsBrowseMode() ||
        bind_data.source_id_column != DConstants::INVALID_INDEX) {
        return;
    }

    bind_data.source_id_column = bind_data.columns.size();
    RootScanColumn source_id;
    source_id.name = "source_id";
    source_id.root_type = "ULong64_t";
    bind_data.columns.push_back(std::move(source_id));
    return_names.emplace_back("source_id");
    return_types.emplace_back(LogicalTypeId::UBIGINT);

    bind_data.source_path_column = bind_data.columns.size();
    RootScanColumn source_path;
    source_path.name = "source_path";
    source_path.root_type = "string";
    source_path.is_string = true;
    bind_data.columns.push_back(std::move(source_path));
    return_names.emplace_back("source_path");
    return_types.emplace_back(LogicalTypeId::VARCHAR);
}

unique_ptr<FunctionData> RootScanBinder::Bind(ClientContext& context, TableFunctionBindInput& input,
                                              vector<LogicalType>& return_types, vector<string>& return_names) {
    RootDebugOperationScope debug_operation("RootScanBind");
    auto bind_data = make_uniq<RootScanBindData>();
    ConfigureOptions(*bind_data, context, input);

    const bool dictionary_loaded = LoadRequestedDictionary(context, input);
    auto cleanup_parameter = input.named_parameters.find("dictionary_cleanup");
    const std::string cleanup_mode =
        cleanup_parameter == input.named_parameters.end() ? "auto" : cleanup_parameter->second.ToString();
    bind_data->root_access.dictionary_cleanup_mode = rootlake::ParseDictionaryCleanupMode(
        cleanup_mode,
        dictionary_loaded ? rootlake::RootDictionaryCleanupMode::RETAIN : rootlake::RootDictionaryCleanupMode::FULL);

    if (input.named_parameters.find("path_prefix") == input.named_parameters.end()) {
        BindRootBrowse(*bind_data, return_names, return_types);
    } else {
        BindRequestedPath(*bind_data, input, dictionary_loaded, return_names, return_types);
    }
    return std::move(bind_data);
}

void RootScanBinder::ConfigureOptions(RootScanBindData& bind_data, ClientContext& context,
                                      TableFunctionBindInput& input) {
    bind_data.input_specification = input.inputs[0].ToString();
    bind_data.root_paths = rootlake::ResolveRootInputs(context, bind_data.input_specification);
    bind_data.root_path = bind_data.root_paths.front();
    auto reader_mode = input.named_parameters.find("reader_mode");
    if (reader_mode != input.named_parameters.end()) {
        bind_data.root_access.reader_mode = rootlake::ParseRootReaderMode(reader_mode->second.ToString());
    }
    auto raw_validation = input.named_parameters.find("raw_validation_entries");
    if (raw_validation != input.named_parameters.end()) {
        bind_data.root_access.validation_entries = raw_validation->second.GetValue<uint32_t>();
    }
    auto raw_entry_limit = input.named_parameters.find("raw_max_entry_bytes");
    if (raw_entry_limit != input.named_parameters.end()) {
        bind_data.root_access.max_entry_bytes = raw_entry_limit->second.GetValue<uint64_t>();
    }
    auto raw_value_limit = input.named_parameters.find("raw_max_values_per_entry");
    if (raw_value_limit != input.named_parameters.end()) {
        bind_data.root_access.max_values_per_entry = raw_value_limit->second.GetValue<uint64_t>();
    }
    auto tree_cache = input.named_parameters.find("tree_cache_bytes");
    if (tree_cache != input.named_parameters.end()) {
        bind_data.root_access.tree_cache_bytes = tree_cache->second.GetValue<uint64_t>();
    }
    bind_data.root_access.Validate();
    RootDebug("BIND.BEGIN", "root_input=" + bind_data.input_specification +
                                " resolved_files=" + std::to_string(bind_data.root_paths.size()) +
                                " inputs=" + std::to_string(input.inputs.size()) +
                                " named_parameters=" + std::to_string(input.named_parameters.size()));
}

void RootScanBinder::BindRootBrowse(RootScanBindData& bind_data, std::vector<std::string>& return_names,
                                    std::vector<LogicalType>& return_types) {
    auto file = OpenRepresentativeFile(bind_data);

    std::set<std::string> children;

    TIter next_key(file->GetListOfKeys());

    while (auto* key = dynamic_cast<TKey*>(next_key())) {
        const std::string class_name = key->GetClassName();

        if (class_name != "TTree") {
            children.insert("/" + std::string(key->GetName()));
        }
    }

    TTree* tree = rootlake::FindTree(file.get(), "", "");

    if (tree) {
        auto* branches = tree->GetListOfBranches();

        for (int index = 0; branches && index < branches->GetEntries(); ++index) {

            auto* element = dynamic_cast<TBranchElement*>(branches->At(index));

            if (element && element->GetClassName()) {
                children.insert("/" + std::string(element->GetClassName()));
                continue;
            }

            auto* branch = dynamic_cast<TBranch*>(branches->At(index));

            if (branch) {
                children.insert("/" + std::string(branch->GetName()));
            }
        }
    }

    if (children.empty()) {
        throw IOException("No readable ROOT objects were found in file");
    }

    bind_data.SelectBrowseMode(std::vector<std::string>(children.begin(), children.end()));

    return_names.emplace_back("path");

    return_types.emplace_back(LogicalType(LogicalTypeId::VARCHAR));
}

void RootScanBinder::BindRequestedPath(RootScanBindData& bind_data, TableFunctionBindInput& input,
                                       bool dictionary_loaded, std::vector<std::string>& return_names,
                                       std::vector<LogicalType>& return_types) {
    const std::string path_prefix_raw = input.named_parameters["path_prefix"].ToString();

    RootDebug("BIND.PATH", "path_prefix=" + path_prefix_raw);

    auto file = OpenRepresentativeFile(bind_data);

    rootlake::RootHistogramBinding histogram_binding;
    std::unique_ptr<TH1> histogram_object;
    if (rootlake::TryBindRootHistogram(*file, path_prefix_raw, histogram_binding, histogram_object)) {

        if (bind_data.IsMultiFile()) {
            throw NotImplementedException("ROOT histogram object mode currently "
                                          "accepts one ROOT file per read_root() call");
        }

        bind_data.total_rows = histogram_binding.row_count;

        const auto& schema = histogram_binding.schema;

        return_names = schema.names;
        return_types = schema.types;

        RootDebug("BIND.HISTOGRAM", "path=" + histogram_binding.object_path +
                                        " class=" + histogram_binding.class_name +
                                        " view=" + rootlake::RootHistogramViewName(histogram_binding.view) +
                                        " rows=" + std::to_string(histogram_binding.row_count));

        bind_data.SelectHistogramMode(std::move(histogram_binding), std::move(histogram_object));

        return;
    }

    const auto requested_path = rootlake::ParsePathPrefix(path_prefix_raw);

    if (!requested_path.fields.empty() && !dictionary_loaded) {
        throw InvalidInputException("Semantic ROOT path '" + path_prefix_raw +
                                    "' requires dictionary := "
                                    "'/path/to/libDictionary.so'. "
                                    "Binding complex classes from embedded "
                                    "StreamerInfo without a runtime dictionary "
                                    "is disabled because ROOT may construct "
                                    "unsafe emulated classes.");
    }

    TTree* tree = rootlake::FindTree(file.get(), "", "");

    if (!tree) {
        throw IOException("No TTree found in ROOT file and requested "
                          "path is not a supported ROOT analysis object.");
    }

    if (!BindSemanticPath(bind_data, file.get(), path_prefix_raw, return_names, return_types)) {

        BindPrimitiveCompatibility(bind_data, *file, *tree, path_prefix_raw, return_names, return_types);
    }

    AddMultiFileIdentityColumns(bind_data, return_names, return_types);
}

std::vector<RootPrimitiveBranch> RootScanBinder::CollectPrimitiveBranches(TTree& tree) {
    std::vector<RootPrimitiveBranch> result;
    auto* branches = tree.GetListOfBranches();
    for (int index = 0; branches && index < branches->GetEntries(); ++index) {
        auto* branch = dynamic_cast<TBranch*>(branches->At(index));
        if (!branch || dynamic_cast<TBranchElement*>(branch)) {
            continue;
        }
        TLeaf* leaf = branch->GetLeaf(branch->GetName());
        if (!leaf) {
            continue;
        }
        result.push_back({branch->GetName(), leaf->GetTypeName(), branch, leaf});
    }
    return result;
}

bool RootScanBinder::IsTreeName(TFile& file, const std::string& name) {
    TIter next(file.GetListOfKeys());
    while (auto* key = dynamic_cast<TKey*>(next())) {
        if (std::string(key->GetClassName()) == "TTree" && std::string(key->GetName()) == name) {
            return true;
        }
    }
    return false;
}

void RootScanBinder::BindPrimitiveCompatibility(RootScanBindData& bind_data, TFile& file, TTree& tree,
                                                const std::string& path_prefix, std::vector<std::string>& return_names,
                                                std::vector<LogicalType>& return_types) {
    const auto branches = CollectPrimitiveBranches(tree);
    const std::string target_name =
        !path_prefix.empty() && path_prefix.front() == '/' ? path_prefix.substr(1) : path_prefix;

    if (IsTreeName(file, target_name)) {
        auto* target_tree = dynamic_cast<TTree*>(file.Get(target_name.c_str()));

        if (!target_tree && std::string(tree.GetName()) == target_name) {
            target_tree = &tree;
        }

        if (!target_tree) {
            throw IOException("ROOT TTree '" + target_name + "' was found in file keys but could not be opened");
        }

        const auto tree_primitives = CollectPrimitiveBranches(*target_tree);

        auto* tree_branches = target_tree->GetListOfBranches();

        bool primitive_only = tree_branches && tree_branches->GetEntries() > 0 &&
                              static_cast<idx_t>(tree_branches->GetEntries()) == tree_primitives.size();

        if (primitive_only) {
            for (const auto& branch : tree_primitives) {
                if (!branch.leaf || branch.leaf->GetLeafCount() || branch.leaf->GetLenStatic() != 1) {
                    primitive_only = false;
                    break;
                }
            }
        }

        if (primitive_only) {
            bind_data.SelectPrimitiveTreeMode();
            bind_data.tree_name = target_tree->GetName();
            bind_data.total_rows = static_cast<uint64_t>(std::max<Long64_t>(0, target_tree->GetEntries()));

            AddEventIdColumn(bind_data, return_names, return_types);

            for (const auto& branch : tree_primitives) {
                RootScanColumn column;
                column.name = branch.name;
                column.branch_name = branch.name;
                column.root_type = branch.type_name;

                bind_data.columns.push_back(std::move(column));

                return_names.emplace_back(branch.name);

                return_types.emplace_back(rootlake::RootTypeToScanLogicalType(branch.type_name, false, true));
            }

            RootDebug("BIND.PRIMITIVE_TREE",
                      "tree=" + bind_data.tree_name + " columns=" + std::to_string(tree_primitives.size()));

            return;
        }

        std::vector<std::string> children;
        children.reserve(tree_primitives.size());
        for (const auto& branch : tree_primitives) {
            children.push_back("/" + branch.name);
        }

        bind_data.SelectBrowseMode(std::move(children));

        return_names.emplace_back("path");
        return_types.emplace_back(LogicalType(LogicalTypeId::VARCHAR));
        return;
    }

    for (const auto& branch : branches) {
        if (branch.name != target_name) {
            continue;
        }
        bind_data.SelectDirectBranchMode(branch);
        bind_data.tree_name = tree.GetName();
        bind_data.total_rows = tree.GetEntries();
        AddEventIdColumn(bind_data, return_names, return_types);

        RootScanColumn column;
        column.name = branch.name;
        column.branch_name = branch.name;
        column.root_type = branch.type_name;
        column.is_string = rootlake::IsStringType(branch.type_name);
        bind_data.columns.push_back(std::move(column));
        return_names.emplace_back(branch.name);
        return_types.emplace_back(rootlake::RootTypeToScanLogicalType(branch.type_name, false, true));
        return;
    }

    BindEmptyResult(bind_data, path_prefix, return_names, return_types);
}

} // namespace duckdb
