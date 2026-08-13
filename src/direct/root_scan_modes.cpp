#include "root4duckdb/direct/root_scan_internal.hpp"

namespace duckdb {

bool RootScanExecutor::WriteNumericValue(Vector& vector, idx_t row, const rootlake::RootPrimitiveValue& value) {
    switch (vector.GetType().id()) {
    case LogicalTypeId::TINYINT:
        FlatVector::GetData<int8_t>(vector)[row] = static_cast<int8_t>(value.AsSigned());
        break;

    case LogicalTypeId::UTINYINT:
        FlatVector::GetData<uint8_t>(vector)[row] = static_cast<uint8_t>(value.AsUnsigned());
        break;

    case LogicalTypeId::SMALLINT:
        FlatVector::GetData<int16_t>(vector)[row] = static_cast<int16_t>(value.AsSigned());
        break;

    case LogicalTypeId::USMALLINT:
        FlatVector::GetData<uint16_t>(vector)[row] = static_cast<uint16_t>(value.AsUnsigned());
        break;

    case LogicalTypeId::INTEGER:
        FlatVector::GetData<int32_t>(vector)[row] = static_cast<int32_t>(value.AsSigned());
        break;

    case LogicalTypeId::UINTEGER:
        FlatVector::GetData<uint32_t>(vector)[row] = static_cast<uint32_t>(value.AsUnsigned());
        break;

    case LogicalTypeId::BIGINT:
        FlatVector::GetData<int64_t>(vector)[row] = value.AsSigned();
        break;

    case LogicalTypeId::UBIGINT:
        FlatVector::GetData<uint64_t>(vector)[row] = value.AsUnsigned();
        break;

    case LogicalTypeId::FLOAT:
        FlatVector::GetData<float>(vector)[row] = static_cast<float>(value.AsDouble());
        break;

    case LogicalTypeId::DOUBLE:
        FlatVector::GetData<double>(vector)[row] = value.AsDouble();
        break;

    case LogicalTypeId::BOOLEAN:
        FlatVector::GetData<bool>(vector)[row] = value.AsBool();
        break;

    default:
        FlatVector::Validity(vector).SetInvalid(row);
        return false;
    }

    FlatVector::Validity(vector).SetValid(row);
    return true;
}

std::optional<int32_t> RootScanExecutor::ResolveCachedIndexValue(const RootScanBindData& bind_data,
                                                                 const RootScanLocalState& lstate, idx_t col_idx,
                                                                 size_t elem_idx) {
    if (col_idx >= bind_data.columns.size()) {
        return std::nullopt;
    }
    const auto& col = bind_data.columns[col_idx];
    std::string search = col.name;
    if (search.size() > 4 && search.substr(search.size() - 4) == "_idx") {
        search.resize(search.size() - 4);
    }
    for (idx_t candidate_index = 0; candidate_index < bind_data.columns.size(); ++candidate_index) {
        const auto& candidate = bind_data.columns[candidate_index];
        if (candidate.is_virtual_index || candidate.levels.empty() || candidate.branch_name != col.branch_name ||
            candidate_index >= lstate.cached_results.size()) {
            continue;
        }
        const auto& result = lstate.cached_results[candidate_index];
        if (elem_idx >= result.size() || elem_idx >= result.vector_indices.size()) {
            continue;
        }
        for (size_t name_index = 0; name_index < result.vector_names.size(); ++name_index) {
            std::string name = result.vector_names[name_index];
            if (name.size() > 4 && name.substr(name.size() - 4) == "_idx") {
                name.resize(name.size() - 4);
            }
            if (name == search && name_index < result.vector_indices[elem_idx].size()) {
                return static_cast<int32_t>(result.vector_indices[elem_idx][name_index]);
            }
        }
    }
    return std::nullopt;
}

rootlake::RootScalarActual RootScanExecutor::CachedScalar(const RootScanBindData& bind_data,
                                                          const RootScanLocalState& lstate, idx_t col_idx,
                                                          uint64_t entry, size_t elem_idx) {
    if (col_idx == COLUMN_IDENTIFIER_ROW_ID) {
        return rootlake::RootScalarActual::Signed(static_cast<int64_t>(entry));
    }
    if (col_idx >= bind_data.columns.size()) {
        return rootlake::RootScalarActual::Null(LogicalType::SQLNULL);
    }
    const auto& column = bind_data.columns[col_idx];
    if (column.name == "event_id" && column.levels.empty()) {
        return rootlake::RootScalarActual::Signed(static_cast<int64_t>(entry));
    }
    if (col_idx == bind_data.source_id_column) {
        return rootlake::RootScalarActual::Event(lstate.file_task.source_id);
    }
    if (col_idx == bind_data.source_path_column) {
        return rootlake::RootScalarActual::String(lstate.file_task.path);
    }
    if (column.is_virtual_index) {
        return rootlake::RootScalarActual::Index(ResolveCachedIndexValue(bind_data, lstate, col_idx, elem_idx));
    }
    const auto logical_type = rootlake::RootTypeToScanLogicalType(column.root_type, column.is_string, true);
    if (col_idx >= lstate.cached_results.size()) {
        return rootlake::RootScalarActual::Null(logical_type);
    }
    const auto& result = lstate.cached_results[col_idx];
    if (elem_idx >= result.size()) {
        return rootlake::RootScalarActual::Null(logical_type);
    }
    if (result.is_string_flag[elem_idx]) {
        return rootlake::RootScalarActual::String(result.strings[elem_idx]);
    }
    return PrimitiveScalarActual(logical_type, result.numbers[elem_idx]);
}

bool RootScanExecutor::PassesCachedFilters(ClientContext& context, const RootScanBindData& bind_data,
                                           const RootScanGlobalState& gstate, RootScanLocalState& lstate,
                                           uint64_t entry, size_t elem_idx) {
    if (!gstate.filters) {
        return true;
    }
    for (const auto& filter : gstate.filters->filters) {
        if (filter.first >= gstate.scan_column_ids.size()) {
            continue;
        }
        const auto actual = CachedScalar(bind_data, lstate, gstate.scan_column_ids[filter.first], entry, elem_idx);
        if (!lstate.filter_evaluator.Evaluate(context, *filter.second, actual)) {
            return false;
        }
    }
    return true;
}

bool RootScanExecutor::PassesPrimitiveTreeFilters(ClientContext& context, const RootScanBindData& bind_data,
                                                  const RootScanGlobalState& gstate, RootScanLocalState& lstate,
                                                  uint64_t entry) {
    if (!gstate.filters) {
        return true;
    }

    for (const auto& filter : gstate.filters->filters) {
        if (filter.first >= gstate.scan_column_ids.size()) {
            continue;
        }

        const auto column_index = gstate.scan_column_ids[filter.first];

        auto actual = rootlake::RootScalarActual::Null(LogicalType::SQLNULL);

        if (column_index == 0 || column_index == COLUMN_IDENTIFIER_ROW_ID) {

            actual = rootlake::RootScalarActual::Signed(static_cast<int64_t>(entry));

        } else if (column_index == bind_data.source_id_column) {

            actual = rootlake::RootScalarActual::Event(lstate.file_task.source_id);

        } else if (column_index == bind_data.source_path_column) {

            actual = rootlake::RootScalarActual::String(lstate.file_task.path);

        } else if (column_index < bind_data.columns.size() && column_index < lstate.primitive_tree_leaves.size()) {

            const auto& column = bind_data.columns[column_index];

            auto* leaf = lstate.primitive_tree_leaves[column_index];

            if (leaf && leaf->GetValuePointer()) {

                const auto value =
                    rootlake::RootPrimitiveValue ::FromPointer(leaf->GetValuePointer(), column.root_type);

                const auto logical_type = rootlake ::RootTypeToScanLogicalType(column.root_type, false, true);

                switch (value.kind) {
                case rootlake ::RootPrimitiveKind ::SIGNED:
                    actual = rootlake ::RootScalarActual ::Signed(value.signed_value, logical_type);
                    break;

                case rootlake ::RootPrimitiveKind ::UNSIGNED:
                    actual = rootlake ::RootScalarActual ::Unsigned(value.unsigned_value, logical_type);
                    break;

                case rootlake ::RootPrimitiveKind ::FLOATING:
                    actual = rootlake ::RootScalarActual ::Numeric(logical_type, value.floating_value);
                    break;
                }
            }
        }

        if (!lstate.filter_evaluator.Evaluate(context, *filter.second, actual)) {
            return false;
        }
    }

    return true;
}

void RootScanExecutor::ProcessPrimitiveTree(ClientContext& context, const RootScanBindData& bind_data,
                                            RootScanGlobalState& gstate, RootScanLocalState& lstate,
                                            DataChunk& output) {
    auto* tree = lstate.root_file.GetTTree();

    if (!tree) {
        output.SetCardinality(0);
        return;
    }

    idx_t output_count = 0;

    while (output_count < STANDARD_VECTOR_SIZE) {
        if (lstate.local_current_row >= lstate.local_end_row) {
            if (gstate.file_scheduler) {
                break;
            }

            RootEntryScheduler scheduler(gstate.next_row, gstate.total_rows, gstate.coordination_mutex);

            const auto batch = scheduler.ClaimWork(100000);

            if (!batch.HasWork()) {
                break;
            }

            lstate.local_current_row = batch.start;
            lstate.local_end_row = batch.end;
        }

        const auto entry = lstate.local_current_row++;

        if (lstate.primitive_tree_requires_read) {
            if (tree->GetEntry(static_cast<Long64_t>(entry)) < 0) {
                break;
            }
        }

        if (!PassesPrimitiveTreeFilters(context, bind_data, gstate, lstate, entry)) {
            continue;
        }

        for (idx_t output_index = 0; output_index < gstate.output_column_ids.size(); ++output_index) {
            const auto column_index = gstate.output_column_ids[output_index];

            auto& vector = output.data[output_index];

            if (column_index == 0 || column_index == COLUMN_IDENTIFIER_ROW_ID) {
                FlatVector::GetData<int64_t>(vector)[output_count] = static_cast<int64_t>(entry);

                FlatVector::Validity(vector).SetValid(output_count);

                continue;
            }

            if (column_index == bind_data.source_id_column) {
                FlatVector::GetData<uint64_t>(vector)[output_count] = lstate.file_task.source_id;

                FlatVector::Validity(vector).SetValid(output_count);

                continue;
            }

            if (column_index == bind_data.source_path_column) {
                FlatVector::GetData<string_t>(vector)[output_count] =
                    StringVector::AddString(vector, lstate.file_task.path);

                FlatVector::Validity(vector).SetValid(output_count);

                continue;
            }

            if (column_index >= bind_data.columns.size() || column_index >= lstate.primitive_tree_leaves.size()) {
                FlatVector::Validity(vector).SetInvalid(output_count);
                continue;
            }

            auto* leaf = lstate.primitive_tree_leaves[column_index];

            const auto& column = bind_data.columns[column_index];

            if (!leaf || !leaf->GetValuePointer()) {
                FlatVector::Validity(vector).SetInvalid(output_count);
                continue;
            }

            const auto value = rootlake::RootPrimitiveValue ::FromPointer(leaf->GetValuePointer(), column.root_type);

            WriteNumericValue(vector, output_count, value);
        }

        ++output_count;
    }

    output.SetCardinality(output_count);
}

bool RootScanExecutor::PassesDirectBranchFilters(ClientContext& context, const RootScanBindData& bind_data,
                                                 const RootScanGlobalState& gstate, RootScanLocalState& lstate,
                                                 uint64_t entry, const rootlake::RootPrimitiveValue& value) {
    if (!gstate.filters) {
        return true;
    }
    const auto value_type = rootlake::RootTypeToScanLogicalType(bind_data.direct_branch_info.type_name, false, true);
    for (const auto& filter : gstate.filters->filters) {
        if (filter.first >= gstate.scan_column_ids.size()) {
            continue;
        }
        const auto column = gstate.scan_column_ids[filter.first];
        auto actual = rootlake::RootScalarActual::Null(LogicalType::SQLNULL);
        if (column == 0 || column == COLUMN_IDENTIFIER_ROW_ID) {
            actual = rootlake::RootScalarActual::Signed(static_cast<int64_t>(entry));
        } else if (column == 1) {
            actual = PrimitiveScalarActual(value_type, value);
        } else if (column == bind_data.source_id_column) {
            actual = rootlake::RootScalarActual::Event(lstate.file_task.source_id);
        } else if (column == bind_data.source_path_column) {
            actual = rootlake::RootScalarActual::String(lstate.file_task.path);
        } else {
            actual = rootlake::RootScalarActual::Null(LogicalType::SQLNULL);
        }
        if (!lstate.filter_evaluator.Evaluate(context, *filter.second, actual)) {
            return false;
        }
    }
    return true;
}

void RootScanExecutor::ProcessDirectBranch(ClientContext& context, const RootScanBindData& bind_data,
                                           RootScanGlobalState& gstate, RootScanLocalState& lstate, DataChunk& output) {
    if (!lstate.direct_branch || !lstate.direct_leaf) {
        output.SetCardinality(0);
        return;
    }

    idx_t out_count = 0;
    while (out_count < STANDARD_VECTOR_SIZE) {
        if (lstate.local_current_row >= lstate.local_end_row) {
            if (gstate.file_scheduler) {
                break;
            }
            RootEntryScheduler scheduler(gstate.next_row, gstate.total_rows, gstate.coordination_mutex);
            auto batch = scheduler.ClaimWork(100000);
            if (!batch.HasWork()) {
                break;
            }
            lstate.local_current_row = batch.start;
            lstate.local_end_row = batch.end;
        }
        const auto entry = lstate.local_current_row;
        // Bare primitive-branch compatibility only.  Even here the entry is loaded
        // through TTree so no physical branch becomes an alternative semantic reader.
        auto* direct_tree = lstate.root_file.GetTTree();
        if (!direct_tree || direct_tree->GetEntry(static_cast<Long64_t>(entry)) < 0) {
            break;
        }

        const auto val = rootlake::RootPrimitiveValue::FromPointer(lstate.direct_leaf->GetValuePointer(),
                                                                   bind_data.direct_branch_info.type_name);
        ++lstate.local_current_row;
        if (!PassesDirectBranchFilters(context, bind_data, gstate, lstate, entry, val)) {
            continue;
        }

        for (idx_t output_index = 0; output_index < gstate.output_column_ids.size(); ++output_index) {
            const auto column = gstate.output_column_ids[output_index];
            auto& vector = output.data[output_index];
            if (column == 0 || column == COLUMN_IDENTIFIER_ROW_ID) {
                FlatVector::GetData<int64_t>(vector)[out_count] = static_cast<int64_t>(entry);
                FlatVector::Validity(vector).SetValid(out_count);
            } else if (column == 1) {
                WriteNumericValue(vector, out_count, val);
            } else if (column == bind_data.source_id_column) {
                FlatVector::GetData<uint64_t>(vector)[out_count] = lstate.file_task.source_id;
                FlatVector::Validity(vector).SetValid(out_count);
            } else if (column == bind_data.source_path_column) {
                FlatVector::GetData<string_t>(vector)[out_count] =
                    StringVector::AddString(vector, lstate.file_task.path);
                FlatVector::Validity(vector).SetValid(out_count);
            } else {
                FlatVector::Validity(vector).SetInvalid(out_count);
            }
        }

        out_count++;
    }
    output.SetCardinality(out_count);
}

void RootScanExecutor::ProcessCachedEntry(ClientContext& context, const RootScanBindData& bind_data,
                                          RootScanGlobalState& gstate, RootScanLocalState& lstate, DataChunk& output,
                                          idx_t& out_count) {
    uint64_t entry = lstate.current_entry;
    size_t max_elements = 0;
    for (const auto& res : lstate.cached_results) {
        if (!res.empty()) {
            max_elements = std::max(max_elements, res.size());
        }
    }
    if (max_elements == 0) {
        max_elements = 1;
    }

    size_t start_elem = lstate.current_elem_idx;
    size_t next_elem = start_elem;

    for (size_t elem_idx = start_elem; elem_idx < max_elements && out_count < STANDARD_VECTOR_SIZE; ++elem_idx) {
        next_elem = elem_idx + 1;
        if (!PassesCachedFilters(context, bind_data, gstate, lstate, entry, elem_idx)) {
            continue;
        }
        for (idx_t out_idx = 0; out_idx < gstate.output_column_ids.size(); ++out_idx) {
            idx_t col_idx = gstate.output_column_ids[out_idx];
            auto& vec = output.data[out_idx];

            if (col_idx == COLUMN_IDENTIFIER_ROW_ID) {
                FlatVector::GetData<int64_t>(vec)[out_count] = static_cast<int64_t>(entry);
                FlatVector::Validity(vec).SetValid(out_count);
                continue;
            }

            if (col_idx >= bind_data.columns.size()) {
                FlatVector::Validity(vec).SetInvalid(out_count);
                continue;
            }

            const auto& col = bind_data.columns[col_idx];

            if (col.name == "event_id" && col.levels.empty()) {
                FlatVector::GetData<int64_t>(vec)[out_count] = static_cast<int64_t>(entry);
                FlatVector::Validity(vec).SetValid(out_count);
                continue;
            }

            if (col_idx == bind_data.source_id_column) {
                FlatVector::GetData<uint64_t>(vec)[out_count] = lstate.file_task.source_id;
                FlatVector::Validity(vec).SetValid(out_count);
                continue;
            }

            if (col_idx == bind_data.source_path_column) {
                FlatVector::GetData<string_t>(vec)[out_count] = StringVector::AddString(vec, lstate.file_task.path);
                FlatVector::Validity(vec).SetValid(out_count);
                continue;
            }

            if (col.is_virtual_index) {
                std::string search = col.name;
                if (search.size() > 4 && search.substr(search.size() - 4) == "_idx") {
                    search = search.substr(0, search.size() - 4);
                }

                idx_t ref_col_idx = static_cast<idx_t>(-1);
                int idx_pos = -1;

                for (idx_t cand = 0; cand < bind_data.columns.size(); ++cand) {
                    const auto& candidate = bind_data.columns[cand];
                    if (candidate.is_virtual_index || candidate.levels.empty() ||
                        candidate.branch_name != col.branch_name || cand >= lstate.cached_results.size()) {
                        continue;
                    }

                    const auto& candidate_res = lstate.cached_results[cand];
                    if (candidate_res.empty()) {
                        continue;
                    }

                    for (size_t name_idx = 0; name_idx < candidate_res.vector_names.size(); ++name_idx) {
                        std::string vn = candidate_res.vector_names[name_idx];
                        if (vn.size() > 4 && vn.substr(vn.size() - 4) == "_idx") {
                            vn = vn.substr(0, vn.size() - 4);
                        }
                        if (vn == search) {
                            ref_col_idx = cand;
                            idx_pos = static_cast<int>(name_idx);
                            break;
                        }
                    }

                    if (ref_col_idx != static_cast<idx_t>(-1)) {
                        break;
                    }
                }

                if (ref_col_idx == static_cast<idx_t>(-1) || idx_pos < 0) {
                    RootDebug("INDEX.NO_REFERENCE",
                              "column=" + col.name + " branch=" + col.branch_name +
                                  " projected_columns=" + std::to_string(gstate.output_column_ids.size()));
                    FlatVector::Validity(vec).SetInvalid(out_count);
                    continue;
                }

                const auto& ref_res = lstate.cached_results[ref_col_idx];
                if (elem_idx >= ref_res.size() || elem_idx >= ref_res.vector_indices.size() ||
                    static_cast<size_t>(idx_pos) >= ref_res.vector_indices[elem_idx].size()) {
                    FlatVector::Validity(vec).SetInvalid(out_count);
                    continue;
                }

                FlatVector::GetData<int32_t>(vec)[out_count] =
                    ref_res.vector_indices[elem_idx][static_cast<size_t>(idx_pos)];
                FlatVector::Validity(vec).SetValid(out_count);
                continue;
            }

            if (col_idx >= lstate.cached_results.size()) {
                FlatVector::Validity(vec).SetInvalid(out_count);
                continue;
            }

            const auto& res = lstate.cached_results[col_idx];
            if (elem_idx >= res.size()) {
                FlatVector::Validity(vec).SetInvalid(out_count);
                continue;
            }

            if (res.is_string_flag[elem_idx]) {
                if (vec.GetType().id() == LogicalTypeId::VARCHAR) {
                    FlatVector::GetData<string_t>(vec)[out_count] = StringVector::AddString(vec, res.strings[elem_idx]);
                    FlatVector::Validity(vec).SetValid(out_count);
                } else {
                    FlatVector::Validity(vec).SetInvalid(out_count);
                }
            } else {
                const auto& val = res.numbers[elem_idx];
                if (!WriteNumericValue(vec, out_count, val)) {
                    continue;
                }
            }
        }
        out_count++;
    }

    lstate.current_elem_idx = next_elem;
    if (lstate.current_elem_idx >= max_elements) {
        lstate.has_cached_entry = false;
        lstate.cached_results.clear();
        lstate.local_current_row++;
    }
}

} // namespace duckdb
