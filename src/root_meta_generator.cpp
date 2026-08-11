#include "root_lake_common.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/file_system.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckdb/main/extension/extension_loader.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace duckdb {
namespace {

template <class T>
void AppendScalar(std::vector<uint8_t> &buffer, const T &value) {
    const auto *bytes = reinterpret_cast<const uint8_t *>(&value);
    buffer.insert(buffer.end(), bytes, bytes + sizeof(T));
}

class PrimitiveBranchBuffer {
public:
    explicit PrimitiveBranchBuffer(std::string type_name_p)
        : type_name(std::move(type_name_p)) {
    }

    void *Data() { return storage.data(); }

    size_t ValueSize() const {
        if (type_name == "Double_t" || type_name == "D" ||
            type_name == "Long64_t" || type_name == "L") {
            return 8;
        }
        return 4;
    }

    double AsDouble() const {
        if (type_name == "Int_t" || type_name == "I") {
            return Read<int32_t>();
        }
        if (type_name == "Float_t" || type_name == "F") {
            return Read<float>();
        }
        if (type_name == "Double_t" || type_name == "D") {
            return Read<double>();
        }
        if (type_name == "Long64_t" || type_name == "L") {
            return static_cast<double>(Read<int64_t>());
        }
        return 0;
    }

private:
    template <class T>
    T Read() const {
        T value {};
        std::memcpy(&value, storage.data(), sizeof(T));
        return value;
    }

    std::string type_name;
    alignas(uint64_t) std::array<uint8_t, 8> storage;
};

struct CreateMetaBindData final : public TableFunctionData {
    std::vector<std::string> files;
    std::string tree_name;
    std::string output_dir;
};

struct CreateMetaGlobalState final : public GlobalTableFunctionState {
    idx_t current_file = 0;
};

class LegacyMetadataWriter {
public:
    void Write(const std::string &root_path, const std::string &tree_name,
               const std::string &meta_path) const {
        std::unique_ptr<TFile> file(TFile::Open(root_path.c_str(), "READ"));
        if (!file || file->IsZombie()) throw std::runtime_error("Zombie file");

        TTree *tree = nullptr;
        file->GetObject(tree_name.c_str(), tree);
        if (!tree) throw std::runtime_error("TTree not found");

        std::string schema = "{";
        schema += "\"tree_name\":\"" + tree_name + "\",";
        schema += "\"total_entries\":" +
                  std::to_string(static_cast<uint64_t>(tree->GetEntries())) +
                  ",";
        schema += "\"columns\":[";

        std::vector<uint8_t> basket_index;
        auto *branches = tree->GetListOfBranches();
        const auto column_count = static_cast<uint32_t>(branches->GetEntries());
        for (uint16_t column = 0; column < column_count; ++column) {
            auto *branch = static_cast<TBranch *>(branches->At(column));
            const uint32_t basket_count = branch->GetWriteBasket();
            auto *basket_entries = branch->GetBasketEntry();
            auto *leaf = branch->GetLeaf(branch->GetName());
            const std::string type_name = leaf ? leaf->GetTypeName() : "Float_t";

            if (column > 0) schema += ",";
            schema += "{\"name\":\"" + std::string(branch->GetName()) +
                      "\",\"type\":\"" + type_name +
                      "\",\"num_baskets\":" +
                      std::to_string(basket_count) + "}";

            PrimitiveBranchBuffer value(type_name);
            tree->SetBranchAddress(branch->GetName(), value.Data());
            for (uint32_t basket = 0; basket < basket_count; ++basket) {
                const uint64_t start_row = basket_entries[basket];
                const uint32_t row_count = static_cast<uint32_t>(
                    basket_entries[basket + 1] - basket_entries[basket]);
                if (!row_count) continue;
                AppendBasket(tree, value, column, start_row, row_count,
                             basket_index);
            }
            tree->SetBranchAddress(branch->GetName(), nullptr);
        }

        schema += "]}";
        std::ofstream output(meta_path, std::ios::binary);
        if (!output) throw std::runtime_error("Cannot create output .json file");
        output << schema << '\n';
        if (!basket_index.empty()) {
            output.write(reinterpret_cast<const char *>(basket_index.data()),
                         static_cast<std::streamsize>(basket_index.size()));
        }
    }

private:
    static void AppendBasket(TTree *tree, PrimitiveBranchBuffer &value,
                             uint16_t column, uint64_t start_row,
                             uint32_t row_count,
                             std::vector<uint8_t> &output) {
        double minimum = std::numeric_limits<double>::max();
        double maximum = -std::numeric_limits<double>::max();
        constexpr uint32_t bloom_bytes = 64;
        constexpr uint32_t bloom_bits = bloom_bytes * 8;
        std::array<uint8_t, bloom_bytes> bloom {};

        for (uint64_t row = start_row; row < start_row + row_count; ++row) {
            // Legacy metadata covers bare primitive branches, but entry loading
            // still goes through TTree to preserve the reader invariant.
            tree->GetEntry(static_cast<Long64_t>(row));
            const double current = value.AsDouble();
            minimum = std::min(minimum, current);
            maximum = std::max(maximum, current);
            const uint64_t first_hash =
                rootlake::FNV1a64(value.Data(), value.ValueSize());
            const uint64_t second_hash = first_hash ^ 0x9e3779b97f4a7c15ULL;
            bloom[(first_hash % bloom_bits) / 8] |=
                static_cast<uint8_t>(1U << ((first_hash % bloom_bits) % 8));
            bloom[(second_hash % bloom_bits) / 8] |=
                static_cast<uint8_t>(1U << ((second_hash % bloom_bits) % 8));
        }

        AppendScalar(output, column);
        AppendScalar(output, start_row);
        AppendScalar(output, row_count);
        AppendScalar(output, minimum);
        AppendScalar(output, maximum);
        AppendScalar(output, bloom_bytes);
        output.insert(output.end(), bloom.begin(), bloom.end());
    }
};

unique_ptr<FunctionData> CreateMetaBind(
    ClientContext &context, TableFunctionBindInput &input,
    vector<LogicalType> &return_types, vector<string> &return_names) {
    auto result = make_uniq<CreateMetaBindData>();
    const auto file_pattern = input.inputs[0].ToString();
    result->tree_name = input.inputs[1].ToString();
    if (input.inputs.size() > 2) result->output_dir = input.inputs[2].ToString();

    auto &file_system = FileSystem::GetFileSystem(context);
    for (auto &file : file_system.Glob(file_pattern)) {
        result->files.push_back(file.path);
    }
    if (result->files.empty()) {
        throw IOException("По маске '" + file_pattern +
                          "' не найдено ни одного файла.");
    }

    return_names = {"file_path", "meta_path", "status"};
    return_types = {LogicalType::VARCHAR, LogicalType::VARCHAR,
                    LogicalType::VARCHAR};
    return std::move(result);
}

unique_ptr<GlobalTableFunctionState> CreateMetaInit(
    ClientContext &, TableFunctionInitInput &) {
    return make_uniq<CreateMetaGlobalState>();
}

void CreateMetaFunction(ClientContext &context, TableFunctionInput &input,
                        DataChunk &output) {
    auto &bind = input.bind_data->Cast<CreateMetaBindData>();
    auto &state = input.global_state->Cast<CreateMetaGlobalState>();
    const idx_t file_index = state.current_file++;
    if (file_index >= bind.files.size()) {
        output.SetCardinality(0);
        return;
    }

    auto &file_system = FileSystem::GetFileSystem(context);
    const auto &root_path = bind.files[file_index];
    const std::string meta_path = bind.output_dir.empty()
                                      ? root_path + ".json"
                                      : bind.output_dir + "/" +
                                            file_system.ExtractBaseName(root_path) +
                                            ".json";
    try {
        LegacyMetadataWriter().Write(root_path, bind.tree_name, meta_path);
        output.SetValue(2, 0, Value("SUCCESS"));
    } catch (const std::exception &exception) {
        output.SetValue(2, 0,
                        Value("ERROR: " + std::string(exception.what())));
    }
    output.SetValue(0, 0, Value(root_path));
    output.SetValue(1, 0, Value(meta_path));
    output.SetCardinality(1);
}

} // namespace

void RegisterRootMetaGenerator(ExtensionLoader &loader) {
    TableFunction default_output(
        "create_meta", {LogicalType::VARCHAR, LogicalType::VARCHAR},
        CreateMetaFunction, CreateMetaBind, CreateMetaInit);
    loader.RegisterFunction(default_output);

    TableFunction explicit_output(
        "create_meta",
        {LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR},
        CreateMetaFunction, CreateMetaBind, CreateMetaInit);
    loader.RegisterFunction(explicit_output);
}

} // namespace duckdb
