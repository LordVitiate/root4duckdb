#include "root4duckdb/iceberg/root_iceberg_internal.hpp"

#include "duckdb/common/types/data_chunk.hpp"
#include "duckdb/function/table_function.hpp"

#include "iceberg/manifest/manifest_entry.h"
#include "iceberg/table.h"
#include "iceberg/table_scan.h"

#include <filesystem>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace duckdb::rootlake {
namespace fs = std::filesystem;

namespace {

using iceberg::Namespace;
using iceberg::TableIdentifier;
using iceberg_internal::OpenCatalog;
using iceberg_internal::SqlCatalog;
using iceberg_internal::SqlLiteral;
using iceberg_internal::Take;

std::unordered_map<std::string, RootIcebergTableCommit>
ReadStateFromCatalog(const std::shared_ptr<SqlCatalog>& catalog) {
    std::unordered_map<std::string, RootIcebergTableCommit> out;
    for (const auto& name : {"files", "schemas", "access", "baskets", "snapshots", "commits"}) {
        const TableIdentifier identifier{Namespace{{"root_index"}}, name};
        const auto exists = Take(catalog->TableExists(identifier), "check Iceberg table " + identifier.ToString());
        if (!exists) {
            continue;
        }

        auto table = Take(catalog->LoadTable(identifier), "load Iceberg table " + identifier.ToString());
        RootIcebergTableCommit state;
        state.table_name = name;
        state.metadata_location = std::string(table->metadata_file_location());
        auto snapshot_result = table->current_snapshot();
        if (snapshot_result) {
            state.iceberg_snapshot_id = snapshot_result.value()->snapshot_id;
            state.manifest_list = snapshot_result.value()->manifest_list;
        }
        out.emplace(name, std::move(state));
    }
    return out;
}

struct CatalogInspectBindData final : public TableFunctionData {
    std::vector<RootIcebergTableCommit> rows;
};

struct CatalogInspectState final : public GlobalTableFunctionState {
    idx_t offset = 0;
    idx_t MaxThreads() const override {
        return 1;
    }
};

unique_ptr<FunctionData> CatalogInspectBind(ClientContext&, TableFunctionBindInput& input, vector<LogicalType>& types,
                                            vector<string>& names) {
    auto data = make_uniq<CatalogInspectBindData>();
    auto state = LoadRootIcebergCatalogState(input.inputs[0].ToString());
    for (const auto& name : {"files", "schemas", "access", "baskets", "snapshots", "commits"}) {
        auto it = state.find(name);
        if (it != state.end()) {
            data->rows.push_back(it->second);
        }
    }
    names = {"table_name", "iceberg_snapshot_id", "metadata_location", "manifest_list"};
    types = {LogicalType::VARCHAR, LogicalType::BIGINT, LogicalType::VARCHAR, LogicalType::VARCHAR};
    return std::move(data);
}

unique_ptr<GlobalTableFunctionState> CatalogInspectInit(ClientContext&, TableFunctionInitInput&) {
    return make_uniq<CatalogInspectState>();
}

void CatalogInspectScan(ClientContext&, TableFunctionInput& input, DataChunk& output) {
    auto& bind = input.bind_data->Cast<CatalogInspectBindData>();
    auto& state = input.global_state->Cast<CatalogInspectState>();
    idx_t count = 0;
    while (state.offset < bind.rows.size() && count < STANDARD_VECTOR_SIZE) {
        const auto& row = bind.rows[state.offset++];
        output.SetValue(0, count, Value(row.table_name));
        output.SetValue(1, count, Value::BIGINT(row.iceberg_snapshot_id));
        output.SetValue(2, count, Value(row.metadata_location));
        output.SetValue(3, count, Value(row.manifest_list));
        ++count;
    }
    output.SetCardinality(count);
}

} // namespace

bool IsRootIcebergCatalog(const std::string& catalog_root) {
    std::error_code error;
    return fs::is_regular_file(fs::path(catalog_root) / "catalog.sqlite", error) && !error;
}

std::unordered_map<std::string, RootIcebergTableCommit> LoadRootIcebergCatalogState(const std::string& catalog_root) {
    if (!IsRootIcebergCatalog(catalog_root)) {
        throw IOException("Not a ROOT4DuckDB Iceberg SqlCatalog: " + catalog_root);
    }
    return ReadStateFromCatalog(OpenCatalog(catalog_root));
}

std::string RootIcebergRelation(const std::string& catalog_root, const std::string& table_name) {
    auto catalog = OpenCatalog(catalog_root);
    const TableIdentifier identifier{Namespace{{"root_index"}}, table_name};
    const auto exists = Take(catalog->TableExists(identifier), "check Iceberg table " + identifier.ToString());
    if (!exists) {
        throw IOException("Missing root_index." + table_name + " in Iceberg catalog " + catalog_root);
    }

    auto table = Take(catalog->LoadTable(identifier), "load Iceberg table " + identifier.ToString());
    auto scan_builder = Take(table->NewScan(), "create Iceberg scan for " + identifier.ToString());
    auto scan = Take(scan_builder->Build(), "build Iceberg scan for " + identifier.ToString());
    auto tasks = Take(scan->PlanFiles(), "plan Iceberg files for " + identifier.ToString());
    if (tasks.empty()) {
        throw IOException("Iceberg table has no active data files: " + identifier.ToString());
    }

    std::ostringstream relation;
    relation << "read_parquet([";
    bool first = true;
    for (const auto& task : tasks) {
        if (!task || !task->data_file()) {
            throw IOException("Iceberg scan returned an invalid file task for " + identifier.ToString());
        }
        std::string path = task->data_file()->file_path;
        constexpr std::string_view file_prefix = "file://";
        if (path.rfind(file_prefix, 0) == 0) {
            path.erase(0, file_prefix.size());
        }
        if (!first) {
            relation << ',';
        }
        relation << SqlLiteral(path);
        first = false;
    }
    relation << "], union_by_name=true)";
    return relation.str();
}

std::string RootIcebergCommittedSnapshotsRelation(const std::string& catalog_root, uint32_t index_version) {
    const auto commits = RootIcebergRelation(catalog_root, "commits");
    return "(SELECT root_snapshot_id AS snapshot_id, root_snapshot_id, state, " + std::to_string(index_version) +
           "::INTEGER AS index_version, committed_at_ns AS created_at_ns, "
           "committed_at_ns, manifest_fingerprint, dictionary_fingerprint FROM " +
           commits + ")";
}

void RegisterRootIcebergCatalog(ExtensionLoader& loader) {
    TableFunction function("root_iceberg_catalog", {LogicalType::VARCHAR}, CatalogInspectScan, CatalogInspectBind,
                           CatalogInspectInit);
    loader.RegisterFunction(function);
}

} // namespace duckdb::rootlake
