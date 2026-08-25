#include "root4duckdb/direct/root_describe.hpp"

#include "root4duckdb/core/root_dictionary.hpp"
#include "root4duckdb/core/root_file_opener.hpp"
#include "root4duckdb/core/root_input_resolver.hpp"
#include "root4duckdb/core/root_lake_common.hpp"
#include "root4duckdb/reader/root_object_reader.hpp"
#include "root4duckdb/reader/root_offset_reader.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/types/data_chunk.hpp"
#include "duckdb/common/types/vector.hpp"

#include <algorithm>
#include <memory>
#include <sstream>
#include <utility>
#include <vector>

namespace duckdb {
namespace {

struct RootDescribeRow {
    std::string path;
    std::string name;
    std::string kind;
    std::string root_type;
    bool is_primitive = false;
    bool is_string = false;
    bool is_container = false;
    bool is_fixed_array = false;
    bool is_pointer = false;
};

struct RootDescribeBindData final : public TableFunctionData {
    std::vector<RootDescribeRow> rows;
};

struct RootDescribeGlobalState final : public GlobalTableFunctionState {
    idx_t offset = 0;

    idx_t MaxThreads() const override {
        return 1;
    }
};

std::unique_ptr<TFile> OpenDescribeRepresentative(ClientContext& context, const std::string& input) {
    const auto sources = rootlake::RootInputResolver(context).Resolve(input);
    std::vector<std::string> failures;
    for (const auto& source : sources) {
        auto opened = rootlake::OpenRootFile(source);
        if (opened) {
            return std::move(opened.file);
        }
        failures.push_back(source + ": " + opened.error);
    }

    std::ostringstream message;
    message << "root_describe could not open a representative ROOT input";
    const auto limit = std::min<size_t>(failures.size(), 4);
    for (size_t index = 0; index < limit; ++index) {
        message << "; " << failures[index];
    }
    if (failures.size() > limit) {
        message << "; ...";
    }
    throw IOException(message.str());
}

unique_ptr<FunctionData> RootDescribeBind(ClientContext& context, TableFunctionBindInput& input,
                                          vector<LogicalType>& return_types, vector<string>& return_names) {
    if (input.inputs.size() != 2) {
        throw InvalidInputException("root_describe requires (input, path)");
    }
    const auto input_spec = input.inputs[0].ToString();
    const auto raw_path = input.inputs[1].ToString();
    if (raw_path.empty()) {
        throw InvalidInputException("root_describe path must not be empty");
    }

    auto dictionary = input.named_parameters.find("dictionary");
    if (dictionary != input.named_parameters.end() && !dictionary->second.ToString().empty()) {
        rootlake::LoadRootDictionary(context, dictionary->second.ToString());
    }

    auto file = OpenDescribeRepresentative(context, input_spec);
    const auto parsed = rootlake::ParsePathPrefix(raw_path);
    if (parsed.root_class.empty()) {
        throw InvalidInputException("root_describe path must select a ROOT class/object");
    }
    auto* root_class = TClass::GetClass(parsed.root_class.c_str());
    if (!root_class || !root_class->HasDictionary()) {
        throw InvalidInputException("root_describe semantic path '" + rootlake::NormalizePath(raw_path) +
                                    "' requires a compatible ROOT dictionary");
    }
    auto* tree = rootlake::FindTree(file.get(), "", parsed.root_class);
    if (!tree || !rootlake::FindObjectBranch(tree, parsed.root_class)) {
        throw IOException("root_describe cannot find ROOT object branch for " + parsed.root_class);
    }

    std::vector<rootlake::SemanticPathChild> children;
    if (!rootlake::DescribeSemanticPath(root_class, parsed, raw_path, children)) {
        throw InvalidInputException("root_describe cannot resolve semantic path: " + rootlake::NormalizePath(raw_path));
    }

    auto result = make_uniq<RootDescribeBindData>();
    result->rows.reserve(children.size());
    for (auto& child : children) {
        RootDescribeRow row;
        row.path = std::move(child.path);
        row.name = std::move(child.name);
        row.kind = std::move(child.kind);
        row.root_type = std::move(child.root_type);
        row.is_primitive = child.is_primitive;
        row.is_string = child.is_string;
        row.is_container = child.is_container;
        row.is_fixed_array = child.is_fixed_array;
        row.is_pointer = child.is_pointer;
        result->rows.push_back(std::move(row));
    }
    std::sort(result->rows.begin(), result->rows.end(),
              [](const RootDescribeRow& left, const RootDescribeRow& right) { return left.path < right.path; });

    return_names = {"path", "name", "kind", "root_type", "is_primitive", "is_string", "is_container",
                    "is_fixed_array", "is_pointer"};
    return_types = {LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR,
                    LogicalType::BOOLEAN, LogicalType::BOOLEAN, LogicalType::BOOLEAN, LogicalType::BOOLEAN,
                    LogicalType::BOOLEAN};
    return result;
}

unique_ptr<GlobalTableFunctionState> RootDescribeInit(ClientContext&, TableFunctionInitInput&) {
    return make_uniq<RootDescribeGlobalState>();
}

void RootDescribeFunction(ClientContext&, TableFunctionInput& input, DataChunk& output) {
    auto& bind = input.bind_data->Cast<RootDescribeBindData>();
    auto& global = input.global_state->Cast<RootDescribeGlobalState>();
    idx_t count = 0;
    while (global.offset < bind.rows.size() && count < STANDARD_VECTOR_SIZE) {
        const auto& row = bind.rows[global.offset++];
        FlatVector::GetData<string_t>(output.data[0])[count] = StringVector::AddString(output.data[0], row.path);
        FlatVector::GetData<string_t>(output.data[1])[count] = StringVector::AddString(output.data[1], row.name);
        FlatVector::GetData<string_t>(output.data[2])[count] = StringVector::AddString(output.data[2], row.kind);
        FlatVector::GetData<string_t>(output.data[3])[count] = StringVector::AddString(output.data[3], row.root_type);
        FlatVector::GetData<bool>(output.data[4])[count] = row.is_primitive;
        FlatVector::GetData<bool>(output.data[5])[count] = row.is_string;
        FlatVector::GetData<bool>(output.data[6])[count] = row.is_container;
        FlatVector::GetData<bool>(output.data[7])[count] = row.is_fixed_array;
        FlatVector::GetData<bool>(output.data[8])[count] = row.is_pointer;
        ++count;
    }
    output.SetCardinality(count);
}

} // namespace

void RegisterRootDescribe(ExtensionLoader& loader) {
    TableFunction function("root_describe", {LogicalType::VARCHAR, LogicalType::VARCHAR}, RootDescribeFunction,
                           RootDescribeBind, RootDescribeInit);
    function.named_parameters["dictionary"] = LogicalType::VARCHAR;
    loader.RegisterFunction(function);
}

} // namespace duckdb
