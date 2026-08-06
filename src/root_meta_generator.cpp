#include "duckdb/main/extension/extension_loader.hpp"
#include "duckdb/parser/parsed_data/create_table_function_info.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/file_system.hpp"
#include "Rtypes.h"
#include "TFile.h"
#include "TTree.h"
#include "TBranch.h"
#include "TLeaf.h"

#include "include/root_meta.hpp"
#include <fstream>
#include <algorithm>
#include <cmath>
#include <limits>
#include "nlohmann/json.hpp" 
#include "TBasket.h" 
namespace duckdb {

// --- Перегрузки для записи в бинарный буфер ---
inline void SafeAppendToBuffer(std::vector<uint8_t>& buffer, uint32_t value) {
    const uint8_t* ptr = reinterpret_cast<const uint8_t*>(&value);
    buffer.insert(buffer.end(), ptr, ptr + sizeof(uint32_t));
}

inline void SafeAppendToBuffer(std::vector<uint8_t>& buffer, uint16_t value) {
    const uint8_t* ptr = reinterpret_cast<const uint8_t*>(&value);
    buffer.insert(buffer.end(), ptr, ptr + sizeof(uint16_t));
}

inline void SafeAppendToBuffer(std::vector<uint8_t>& buffer, uint64_t value) {
    const uint8_t* ptr = reinterpret_cast<const uint8_t*>(&value);
    buffer.insert(buffer.end(), ptr, ptr + sizeof(uint64_t));
}

inline void SafeAppendToBuffer(std::vector<uint8_t>& buffer, uint8_t value) {
    buffer.push_back(value);
}

inline void SafeAppendToBuffer(std::vector<uint8_t>& buffer, double value) {
    const uint8_t* ptr = reinterpret_cast<const uint8_t*>(&value);
    buffer.insert(buffer.end(), ptr, ptr + sizeof(double));
}

inline void SafeAppendToBuffer(std::vector<uint8_t>& buffer, int64_t value) {
    const uint8_t* ptr = reinterpret_cast<const uint8_t*>(&value);
    buffer.insert(buffer.end(), ptr, ptr + sizeof(int64_t));
}


// Быстрый хэш FNV-1a для наполнения Блум-фильтра корзины
inline uint64_t FnvHash64(const void* data, size_t length) {
    const uint8_t* bytes = static_cast<const uint8_t*>(data);
    uint64_t hash = 14695981039346656037ULL;
    for (size_t i = 0; i < length; ++i) {
        hash ^= bytes[i];
        hash *= 1099511628211ULL;
    }
    return hash;
}

struct CreateMetaBindData : public TableFunctionData {
    std::vector<std::string> files; 
    std::string tree_name;
    std::string output_dir;        
};

struct CreateMetaGlobalState : public GlobalTableFunctionState {
    idx_t current_file_idx = 0;
};

unique_ptr<FunctionData> CreateMetaBind(ClientContext &context, TableFunctionBindInput &input,
                                        vector<LogicalType> &return_types, vector<string> &return_names) {
    auto result = make_uniq<CreateMetaBindData>();
    
    auto file_pattern = input.inputs[0].ToString();
    result->tree_name = input.inputs[1].ToString();
    
    if (input.inputs.size() > 2) {
        result->output_dir = input.inputs[2].ToString();
    }

    auto &fs = FileSystem::GetFileSystem(context);
    auto glob_res = fs.Glob(file_pattern);
    for (auto &file_info : glob_res) {
        result->files.push_back(file_info.path);
    }

    if (result->files.empty()) {
        throw IOException("По маске '" + file_pattern + "' не найдено ни одного файла.");
    }

    return_names.push_back("file_path");
    return_types.push_back(LogicalType::VARCHAR);

    return_names.push_back("meta_path");
    return_types.push_back(LogicalType::VARCHAR);

    return_names.push_back("status");
    return_types.push_back(LogicalType::VARCHAR);

    return std::move(result);
}

unique_ptr<GlobalTableFunctionState> CreateMetaInit(ClientContext &context, TableFunctionInitInput &input) {
    return make_uniq<CreateMetaGlobalState>();
}
void CreateMetaFunction(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) {
    auto &bind_data = data_p.bind_data->Cast<CreateMetaBindData>();
    auto &gstate = data_p.global_state->Cast<CreateMetaGlobalState>();

    idx_t file_idx = gstate.current_file_idx++;
    if (file_idx >= bind_data.files.size()) {
        output.SetCardinality(0); 
        return;
    }

    auto &fs = FileSystem::GetFileSystem(context);
    std::string root_path = bind_data.files[file_idx];
    std::string meta_path = bind_data.output_dir.empty() 
        ? root_path + ".json" 
        : bind_data.output_dir + "/" + fs.ExtractBaseName(root_path) + ".json";

    idx_t count = 0;

    try {
        std::unique_ptr<TFile> file(TFile::Open(root_path.c_str(), "READ"));
        if (!file || file->IsZombie()) {
            throw std::runtime_error("Zombie file");
        }

        TTree* tree = nullptr;
        file->GetObject(bind_data.tree_name.c_str(), tree);
        if (!tree) {
            throw std::runtime_error("TTree not found");
        }

        TObjArray* branches = tree->GetListOfBranches();
        uint32_t num_columns = branches->GetEntries();

        // Сборка JSON схемы (без внешних библиотек)
        std::string manual_json = "{";
        manual_json += "\"tree_name\":\"" + bind_data.tree_name + "\",";
        manual_json += "\"total_entries\":" + std::to_string((uint64_t)tree->GetEntries()) + ",";
        manual_json += "\"columns\":[";

        std::vector<uint8_t> baskets_index_buffer;

        for (uint16_t col_idx = 0; col_idx < num_columns; ++col_idx) {
            TBranch* branch = (TBranch*)branches->At(col_idx);
            uint32_t num_baskets = branch->GetWriteBasket();
            Long64_t* basket_entry = branch->GetBasketEntry();

            TLeaf* leaf = branch->GetLeaf(branch->GetName());
            std::string type_name = leaf ? leaf->GetTypeName() : "Float_t";

            if (col_idx > 0) manual_json += ",";
            manual_json += "{";
            manual_json += "\"name\":\"" + std::string(branch->GetName()) + "\",";
            manual_json += "\"type\":\"" + type_name + "\",";
            manual_json += "\"num_baskets\":" + std::to_string(num_baskets);
            manual_json += "}";

            // Выделяем память строго под нативный тип для ROOT
            void* eval_buffer_ptr = nullptr;
            if (type_name == "Int_t" || type_name == "I") {
                eval_buffer_ptr = malloc(sizeof(int32_t));
            } else if (type_name == "Float_t" || type_name == "F") {
                eval_buffer_ptr = malloc(sizeof(float));
            } else if (type_name == "Double_t" || type_name == "D") {
                eval_buffer_ptr = malloc(sizeof(double));
            } else if (type_name == "Long64_t" || type_name == "L") {
                eval_buffer_ptr = malloc(sizeof(int64_t));
            } else {
                eval_buffer_ptr = malloc(8); // Фолбэк
            }
            
            // Регистрируем корректный типизированный указатель в ROOT
            tree->SetBranchAddress(branch->GetName(), eval_buffer_ptr);

            // 2. Фулл-скан по корзинам
            for (uint32_t b_idx = 0; b_idx < num_baskets; ++b_idx) {
                uint64_t start_row = basket_entry[b_idx];
                uint32_t num_rows = basket_entry[b_idx + 1] - basket_entry[b_idx];
                
                if (num_rows == 0) continue;

                double min_val = std::numeric_limits<double>::max();
                double max_val = -std::numeric_limits<double>::max();

                uint32_t bloom_bytes_size = 64;
                uint32_t bloom_bits_size = bloom_bytes_size * 8;
                std::vector<uint8_t> bloom_filter(bloom_bytes_size, 0);

                size_t element_size = 4;

                for (uint64_t row = start_row; row < start_row + num_rows; ++row) {
                    // Load the tree entry, never a physical branch entry.  This legacy
                    // metadata function indexes bare primitive branches only; semantic
                    // object columns are handled by root_build_index through StreamerInfo offsets.
                    tree->GetEntry(static_cast<Long64_t>(row));
                    double current_val = 0.0;
                    
                    if (type_name == "Int_t" || type_name == "I") {
                        current_val = *reinterpret_cast<int32_t*>(eval_buffer_ptr);
                        element_size = 4;
                    } else if (type_name == "Float_t" || type_name == "F") {
                        current_val = *reinterpret_cast<float*>(eval_buffer_ptr);
                        element_size = 4;
                    } else if (type_name == "Double_t" || type_name == "D") {
                        current_val = *reinterpret_cast<double*>(eval_buffer_ptr);
                        element_size = 8;
                    } else if (type_name == "Long64_t" || type_name == "L") {
                        current_val = *reinterpret_cast<int64_t*>(eval_buffer_ptr);
                        element_size = 8;
                    }

                    if (current_val < min_val) min_val = current_val;
                    if (current_val > max_val) max_val = current_val;

                    // Хэшируем непосредственно нативное значение из памяти
                    uint64_t h1 = FnvHash64(eval_buffer_ptr, element_size);
                    uint64_t h2 = h1 ^ 0x9e3779b97f4a7c15ULL;

                    bloom_filter[(h1 % bloom_bits_size) / 8] |= (1 << ((h1 % bloom_bits_size) % 8));
                    bloom_filter[(h2 % bloom_bits_size) / 8] |= (1 << ((h2 % bloom_bits_size) % 8));
                }

                // Упаковываем данные корзины в бинарный индекс (без изменений)
                SafeAppendToBuffer(baskets_index_buffer, (uint16_t)col_idx);
                SafeAppendToBuffer(baskets_index_buffer, (uint64_t)start_row);
                SafeAppendToBuffer(baskets_index_buffer, (uint32_t)num_rows);
                SafeAppendToBuffer(baskets_index_buffer, (double)min_val);
                SafeAppendToBuffer(baskets_index_buffer, (double)max_val);
                SafeAppendToBuffer(baskets_index_buffer, (uint32_t)bloom_bytes_size);
                
                if (bloom_bytes_size > 0) {
                    baskets_index_buffer.insert(baskets_index_buffer.end(), bloom_filter.begin(), bloom_filter.end());
                }
            }
            
            // Освобождаем ресурсы для текущей ветки
            tree->SetBranchAddress(branch->GetName(), nullptr);
            free(eval_buffer_ptr);
        }

        manual_json += "]}"; // Закрываем JSON схему
        // Запись метаданных на диск
        std::ofstream out(meta_path, std::ios::binary);
        if (!out) {
            throw std::runtime_error("Cannot create output .json file");
        }
        
        out << manual_json << "\n"; 
        
        if (!baskets_index_buffer.empty()) {
            out.write(reinterpret_cast<const char*>(baskets_index_buffer.data()), baskets_index_buffer.size());
        }
        out.close();

        output.SetValue(0, count, Value(root_path));
        output.SetValue(1, count, Value(meta_path));
        output.SetValue(2, count, Value("SUCCESS"));
        count++;

    } catch (std::exception &e) {
        output.SetValue(0, count, Value(root_path));
        output.SetValue(1, count, Value(meta_path));
        output.SetValue(2, count, Value("ERROR: " + std::string(e.what())));
        count++;
    }

    output.SetCardinality(count);
}


// ============================================================================
// ШАГ 4: СИСТЕМНАЯ РЕГИСТРАЦИЯ ФУНКЦИИ В КАТАЛОГЕ DUCKDB
// ============================================================================
void RegisterRootMetaGenerator(ExtensionLoader &loader) {
    TableFunction create_meta_glob("create_meta", {LogicalType::VARCHAR, LogicalType::VARCHAR}, 
                                   CreateMetaFunction, CreateMetaBind, CreateMetaInit);
    loader.RegisterFunction(create_meta_glob);

    TableFunction create_meta_glob_dir("create_meta", {LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR}, 
                                       CreateMetaFunction, CreateMetaBind, CreateMetaInit);
    loader.RegisterFunction(create_meta_glob_dir);
}

} // namespace duckdb
