// universal_reader.C
// Запуск: 
//   Один путь:   root -l -q 'universal_reader.C("file.root","/PaEvent/vecHeader",50)'
//   Мульти:      root -l -q 'universal_reader.C("file.root","/PaEvent/vecHeader,/PaEvent/vecScaler",50)'
//   Список:      root -l -q 'universal_reader.C("file.root","",0)'

// ═══════════════════════════════════════════════════════════════
// 🆕 НОВОЕ: Мульти-запросы с умным объединением по индексам
// • Пути с одинаковой структурой индексов → одна таблица
// • Пути с разной структурой → отдельные таблицы
// • Ядро чтения — БЕЗ ИЗМЕНЕНИЙ
// ═══════════════════════════════════════════════════════════════

#include <TFile.h>
#include <TTree.h>
#include <TBranch.h>
#include <TBranchElement.h>
#include <TClass.h>
#include <TStreamerInfo.h>
#include <TStreamerElement.h>
#include <TVirtualCollectionProxy.h>
#include <TObjArray.h>
#include <TList.h>
#include <TSystem.h>
#include <TKey.h>

#include <iostream>
#include <vector>
#include <string>
#include <sstream>
#include <cstdio>
#include <algorithm>
#include <memory>
#include <set>
#include <map>
#include <tuple>

// ───────────────────────────────────────────────────────────────
struct PathLevel {
    std::string name;
    std::string type;
    Long64_t offset_in_parent;
    Long64_t cumulative_offset;
    bool is_primitive;
    bool is_string;
    bool is_pointer;
    bool is_container;
    TClass* klass;
    TClass* element_class;
    
    PathLevel() : offset_in_parent(-1), cumulative_offset(0),
                  is_primitive(false), is_string(false),
                  is_pointer(false), is_container(false),
                  klass(nullptr), element_class(nullptr) {}
};

// ───────────────────────────────────────────────────────────────
struct ReadResult {
    std::vector<std::string> strings;
    std::vector<double> numbers;
    std::vector<bool> is_string_flag;
    std::vector<Long64_t> event_ids;
    std::vector<std::vector<int>> vector_indices;
    std::vector<std::string> vector_names;
    std::string source_path;  // 🔧 Для отладки и заголовков
    
    size_t size() const { return strings.size(); }
    bool empty() const { return strings.empty(); }
    void clear() {
        strings.clear(); numbers.clear(); is_string_flag.clear();
        event_ids.clear(); vector_indices.clear(); vector_names.clear();
        source_path.clear();
    }
    
    // 🔧 Сигнатура для группировки: "vecTrack_idx" или "vecTrack_idx,vecTPar_idx"
    std::string index_signature() const {
        std::string sig;
        for (size_t i = 0; i < vector_names.size(); ++i) {
            if (i > 0) sig += ",";
            sig += vector_names[i];
        }
        return sig;
    }
    
    void add_string(const std::string& s, Long64_t evt_id,
                    const std::vector<int>& indices,
                    const std::vector<std::string>& vec_names) {
        strings.push_back(s);
        numbers.push_back(0);
        is_string_flag.push_back(true);
        event_ids.push_back(evt_id);
        vector_indices.push_back(indices);
        if (vector_names.empty()) vector_names = vec_names;
    }
    void add_number(double v, Long64_t evt_id,
                    const std::vector<int>& indices,
                    const std::vector<std::string>& vec_names) {
        strings.emplace_back();
        numbers.push_back(v);
        is_string_flag.push_back(false);
        event_ids.push_back(evt_id);
        vector_indices.push_back(indices);
        if (vector_names.empty()) vector_names = vec_names;
    }
};

// ───────────────────────────────────────────────────────────────
class PathParser {
public:
    struct ParsedPath {
        std::string root_class;
        std::vector<std::string> fields;
        std::string original;
    };
    
    static ParsedPath parse(const std::string& path) {
        ParsedPath result;
        result.original = path;
        std::string p = path;
        if (!p.empty() && p[0] == '/') p = p.substr(1);
        if (p.find("events/") == 0) p = p.substr(8);
        std::vector<std::string> parts;
        std::stringstream ss(p);
        std::string part;
        while (std::getline(ss, part, '/')) {
            if (!part.empty()) parts.push_back(part);
        }
        if (parts.empty()) return result;
        result.root_class = parts[0];
        result.fields.assign(parts.begin() + 1, parts.end());
        return result;
    }
    
    // 🔧 Парсинг списка путей через запятую
    static std::vector<std::string> split_paths(const std::string& query) {
        std::vector<std::string> paths;
        std::stringstream ss(query);
        std::string path;
        while (std::getline(ss, path, ',')) {
            // Trim whitespace
            size_t start = path.find_first_not_of(" \t");
            size_t end = path.find_last_not_of(" \t");
            if (start != std::string::npos) {
                paths.push_back(path.substr(start, end - start + 1));
            }
        }
        return paths;
    }
};

// ───────────────────────────────────────────────────────────────
class PathResolver {
public:
    static std::vector<PathLevel> resolve(TClass* root_class,
                                          const std::vector<std::string>& fields) {
        std::vector<PathLevel> levels;
        if (!root_class || fields.empty()) return levels;
        
        TClass* current_class = root_class;
        auto* current_info = current_class->GetStreamerInfo();
        Long64_t cumulative = 0;
        
        for (size_t fld_idx = 0; fld_idx < fields.size(); ++fld_idx) {
            const auto& name = fields[fld_idx];
            auto* elements = current_info->GetElements();
            bool found = false;
            
            for (int i = 0; i < elements->GetEntries(); ++i) {
                auto* elem = dynamic_cast<TStreamerElement*>(elements->At(i));
                if (!elem || elem->IsBase()) continue;
                if (elem->GetName() != name) continue;
                
                PathLevel lvl;
                lvl.name = name;
                lvl.type = elem->GetTypeName();
                lvl.offset_in_parent = current_info->GetElementOffset(i);
                lvl.cumulative_offset = cumulative + lvl.offset_in_parent;
                
                lvl.is_primitive = is_primitive_type(lvl.type);
                lvl.is_string = (lvl.type == "std::string" || lvl.type == "string");
                lvl.is_pointer = elem->IsaPointer();
                
                TClass* elem_class = elem->GetClassPointer();
                lvl.is_container = (elem_class && elem_class->GetCollectionProxy());
                lvl.klass = elem_class;
                
                if (lvl.is_container && elem_class->GetCollectionProxy()) {
                    lvl.element_class = elem_class->GetCollectionProxy()->GetValueClass();
                }
                
                levels.push_back(lvl);
                
                if (lvl.is_pointer) {
                    cumulative = 0;
                    current_class = elem_class;
                } else if (lvl.is_container) {
                    cumulative = 0;
                    current_class = lvl.element_class;
                } else if (elem_class) {
                    cumulative = lvl.cumulative_offset;
                    current_class = elem_class;
                } else {
                    current_class = nullptr;
                }
                
                if (current_class) {
                    current_info = current_class->GetStreamerInfo();
                }
                found = true;
                break;
            }
            if (!found) return {};
        }
        return levels;
    }
    
    static void print_debug(const std::vector<PathLevel>& levels) {
        std::cout << "✅ Resolved (" << levels.size() << " levels):\n";
        for (size_t i = 0; i < levels.size(); ++i) {
            const auto& lvl = levels[i];
            std::cout << " [" << i << "] " << lvl.name << " : " << lvl.type
                      << " @+" << lvl.offset_in_parent << " (cum: +" << lvl.cumulative_offset << ")";
            if (lvl.is_container) std::cout << " [CONTAINER]";
            if (lvl.is_pointer) std::cout << " [PTR]";
            if (lvl.is_primitive) std::cout << " [PRIM]";
            if (lvl.is_string) std::cout << " [STRING]";
            std::cout << "\n";
        }
    }
    
private:
    static bool is_primitive_type(const std::string& type) {
        static const std::vector<std::string> prims = {
            "Bool_t","bool","Char_t","char","UChar_t","unsigned char",
            "Short_t","short","UShort_t","unsigned short",
            "Int_t","int","UInt_t","unsigned int",
            "Long_t","long","ULong_t","unsigned long",
            "Long64_t","long long","ULong64_t","unsigned long long",
            "Float_t","float","Double_t","double"
        };
        return std::find(prims.begin(), prims.end(), type) != prims.end();
    }
};

// ───────────────────────────────────────────────────────────────
class ValueReader {
public:
    static void collect(void* root_ptr,
                        const std::vector<PathLevel>& levels,
                        Long64_t max_values,
                        Long64_t evt_id,
                        ReadResult& out) {
        std::vector<int> current_indices;
        std::vector<std::string> vec_names;
        collect_recursive(root_ptr, levels, 0, max_values, evt_id,
                          current_indices, vec_names, out);
    }
    
private:
    static void collect_recursive(void* current_ptr,
                                  const std::vector<PathLevel>& levels,
                                  size_t level_idx,
                                  Long64_t max_values,
                                  Long64_t evt_id,
                                  std::vector<int>& current_indices,
                                  std::vector<std::string>& vec_names,
                                  ReadResult& out) {
        if (out.size() >= static_cast<size_t>(max_values) || level_idx >= levels.size() || !current_ptr) return;
        
        const auto& lvl = levels[level_idx];
        char* field_ptr = static_cast<char*>(current_ptr) + lvl.offset_in_parent;
        
        if (lvl.is_pointer && field_ptr) {
            field_ptr = *reinterpret_cast<char**>(field_ptr);
            if (!field_ptr) return;
        }
        
        bool is_last = (level_idx == levels.size() - 1);
        
        // 1. Контейнер примитивов на последнем уровне → разворачиваем
        if (is_last && lvl.is_container) {
            auto* proxy = lvl.klass ? lvl.klass->GetCollectionProxy() : nullptr;
            if (!proxy) return;
            TVirtualCollectionProxy::TPushPop guard(proxy, field_ptr);
            size_t n = proxy->Size();
            std::string idx_name = lvl.name + "_idx";
            if (vec_names.empty() || vec_names.size() < current_indices.size() + 1) {
                vec_names.push_back(idx_name);
            }
            std::string inner = lvl.type;
            if (inner.find("vector<") == 0) inner = inner.substr(7, inner.size()-8);
            else if (inner.find("set<") == 0) inner = inner.substr(4, inner.size()-6);
            else if (inner.find("list<") == 0) inner = inner.substr(5, inner.size()-6);
            
            for (size_t i = 0; i < n && out.size() < static_cast<size_t>(max_values); ++i) {
                void* elem = proxy->At(i);
                if (!elem) continue;
                current_indices.push_back(static_cast<int>(i));
                if (is_primitive_type(inner)) {
                    out.add_number(read_primitive(elem, inner), evt_id, current_indices, vec_names);
                } else if (inner == "std::string" || inner == "string") {
                    out.add_string(read_string(elem), evt_id, current_indices, vec_names);
                }
                current_indices.pop_back();
            }
            return;
        }
        
        // 2. Обычный лист
        if (is_last) {
            if (lvl.is_primitive) {
                out.add_number(read_primitive(field_ptr, lvl.type), evt_id, current_indices, vec_names);
            } else if (lvl.is_string) {
                out.add_string(read_string(field_ptr), evt_id, current_indices, vec_names);
            }
            return;
        }
        
        // 3. Контейнер объектов → рекурсия
        if (lvl.is_container) {
            if (!lvl.klass || !lvl.klass->HasDictionary()) return;
            auto* proxy = lvl.klass->GetCollectionProxy();
            if (!proxy) return;
            TVirtualCollectionProxy::TPushPop guard(proxy, field_ptr);
            size_t n = proxy->Size();
            if (vec_names.empty() || vec_names.size() < current_indices.size() + 1) {
                vec_names.push_back(lvl.name + "_idx");
            }
            for (size_t i = 0; i < n && out.size() < static_cast<size_t>(max_values); ++i) {
                void* elem = proxy->At(i);
                if (!elem) continue;
                current_indices.push_back(static_cast<int>(i));
                collect_recursive(elem, levels, level_idx + 1, max_values,
                                  evt_id, current_indices, vec_names, out);
                current_indices.pop_back();
            }
            return;
        }
        
        // 4. Вложенный объект
        if (lvl.klass) {
            collect_recursive(field_ptr, levels, level_idx + 1, max_values,
                              evt_id, current_indices, vec_names, out);
        }
    }
    
    static double read_primitive(void* ptr, const std::string& type) {
        if (!ptr) return 0;
        if (type == "Float_t" || type == "float") return *reinterpret_cast<float*>(ptr);
        if (type == "Double_t" || type == "double") return *reinterpret_cast<double*>(ptr);
        if (type == "Int_t" || type == "int") return *reinterpret_cast<int*>(ptr);
        if (type == "Long64_t" || type == "long long") return *reinterpret_cast<long long*>(ptr);
        if (type == "Bool_t" || type == "bool") return (*reinterpret_cast<bool*>(ptr)) ? 1.0 : 0.0;
        if (type == "Short_t" || type == "short") return *reinterpret_cast<short*>(ptr);
        if (type == "UShort_t" || type == "unsigned short") return *reinterpret_cast<unsigned short*>(ptr);
        if (type == "UInt_t" || type == "unsigned int") return *reinterpret_cast<unsigned int*>(ptr);
        if (type == "ULong_t" || type == "unsigned long") return *reinterpret_cast<unsigned long*>(ptr);
        return 0;
    }
    static std::string read_string(void* ptr) {
        if (!ptr) return "";
        try { return *reinterpret_cast<std::string*>(ptr); }
        catch (...) { return "<string_error>"; }
    }
    static bool is_primitive_type(const std::string& type) {
        static const std::vector<std::string> prims = {
            "Bool_t","bool","Char_t","char","UChar_t","unsigned char",
            "Short_t","short","UShort_t","unsigned short",
            "Int_t","int","UInt_t","unsigned int",
            "Long_t","long","ULong_t","unsigned long",
            "Long64_t","long long","ULong64_t","unsigned long long",
            "Float_t","float","Double_t","double"
        };
        return std::find(prims.begin(), prims.end(), type) != prims.end();
    }
};

// ───────────────────────────────────────────────────────────────
// 🔧 НОВОЕ: Мерджер результатов с одинаковой сигнатурой индексов
// ───────────────────────────────────────────────────────────────
// ───────────────────────────────────────────────────────────────
// 🔧 ИСПРАВЛЕННЫЙ ResultMerger
// ───────────────────────────────────────────────────────────────
class ResultMerger {
public:
    struct MergedRow {
        Long64_t event_id;
        std::vector<int> indices;
        std::vector<std::string> index_names;  // 🔧 ДОБАВЛЕНО
        std::vector<std::pair<std::string, std::string>> values; // (column_name, value_str)
    };
    
    static std::map<std::string, std::vector<ReadResult>> 
    group_by_signature(const std::vector<ReadResult>& results) {
        std::map<std::string, std::vector<ReadResult>> groups;
        for (const auto& r : results) {
            if (!r.empty()) groups[r.index_signature()].push_back(r);
        }
        return groups;
    }
    
    static std::vector<MergedRow> merge(const std::vector<ReadResult>& results) {
        if (results.empty()) return {};
        
        // Берём имена индексов из первого результата
        std::vector<std::string> index_names;
        if (!results[0].vector_names.empty()) index_names = results[0].vector_names;
        
        // Ключ: (event_id, "idx1:idx2:...") → строка
        std::map<std::pair<Long64_t, std::string>, MergedRow> rows;
        
        for (const auto& res : results) {
            std::string col_name = extract_leaf_name(res.source_path);
            for (size_t i = 0; i < res.size(); ++i) {
                // Ключ по индексам
                std::string idx_key;
                for (int idx : res.vector_indices[i]) idx_key += std::to_string(idx) + ":";
                auto key = std::make_pair(res.event_ids[i], idx_key);
                
                // Создаём строку если нет
                if (rows.find(key) == rows.end()) {
                    rows[key] = { res.event_ids[i], res.vector_indices[i], index_names, {} };
                }
                
                // Форматируем значение
                std::string val = res.is_string_flag[i] ? 
                    ("\"" + res.strings[i] + "\"") : 
                    (res.numbers[i] == static_cast<long long>(res.numbers[i]) ?
                        std::to_string(static_cast<long long>(res.numbers[i])) :
                        format_double(res.numbers[i]));
                rows[key].values.emplace_back(col_name, val);
            }
        }
        
        // Собираем выход и сортируем колонки
        std::vector<MergedRow> out;
        for (auto& [_, row] : rows) {
            std::sort(row.values.begin(), row.values.end(),
                      [](auto& a, auto& b) { return a.first < b.first; });
            out.push_back(std::move(row));
        }
        return out;
    }
    
    static void print_table(const std::string& signature,
                            const std::vector<MergedRow>& rows,
                            Long64_t max_display) {
        if (rows.empty()) return;
        
        std::cout << "\n📋 Merged table (signature: \"" << signature << "\"):\n";
        
        // Заголовок
        std::cout << "| event_id";
        for (const auto& in : rows[0].index_names) std::cout << " | " << in;
        for (const auto& [col, _] : rows[0].values) std::cout << " | " << col;
        std::cout << " |\n";
        
        // Разделитель
        std::cout << "| :---";
        for (size_t i = 0; i < rows[0].index_names.size(); ++i) std::cout << " | :---";
        for (size_t i = 0; i < rows[0].values.size(); ++i) std::cout << " | ---:";
        std::cout << " |\n";
        
        // Данные
        Long64_t printed = 0;
        for (const auto& row : rows) {
            if (printed++ >= max_display) break;
            std::cout << "| " << row.event_id;
            for (int idx : row.indices) std::cout << " | " << idx;
            for (const auto& [_, val] : row.values) std::cout << " | " << val;
            std::cout << " |\n";
        }
        std::cout << "\n";
    }
    
private:
    static std::string extract_leaf_name(const std::string& path) {
        size_t pos = path.find_last_of('/');
        return (pos != std::string::npos) ? path.substr(pos+1) : path;
    }
    
    static std::string format_double(double v) {
        char buf[64];
        if (v == static_cast<long long>(v)) {
            snprintf(buf, sizeof(buf), "%lld", static_cast<long long>(v));
        } else {
            snprintf(buf, sizeof(buf), "%.6g", v);
        }
        return std::string(buf);
    }
};
// ───────────────────────────────────────────────────────────────
class TablePrinter {
public:
    static void print(const std::string& target_field,
                      Long64_t max_values,
                      const ReadResult& result) {
        std::cout << "📋 First " << max_values << " values of '" << target_field << "':\n";
        std::cout << "| event_id";
        for (const auto& vname : result.vector_names) std::cout << " | " << vname;
        std::cout << " | value |\n";
        std::cout << "| :---";
        for (size_t i = 0; i < result.vector_names.size(); ++i) std::cout << " | :---";
        std::cout << " | ---: |\n";
        for (size_t i = 0; i < result.size() && i < static_cast<size_t>(max_values); ++i) {
            print_row(result.event_ids[i], result.vector_indices[i],
                      result.is_string_flag[i],
                      result.is_string_flag[i] ? result.strings[i] : "",
                      result.is_string_flag[i] ? 0 : result.numbers[i]);
        }
        std::cout << "\n";
    }
private:
    static void print_row(Long64_t evt_id, const std::vector<int>& indices,
                          bool is_str, const std::string& str_val, double num_val) {
        std::cout << "| " << evt_id;
        for (int idx : indices) std::cout << " | " << idx;
        if (is_str) {
            std::string d = str_val;
            if (d.size() > 30) d = d.substr(0, 27) + "...";
            std::cout << " | \"" << d << "\" |\n";
        } else {
            if (num_val == static_cast<long long>(num_val))
                std::cout << " | " << static_cast<long long>(num_val) << " |\n";
            else
                printf(" | %8.6g |\n", num_val);
        }
    }
};

// ───────────────────────────────────────────────────────────────
class PathExplorer {
public:
    static void print_all_paths(TFile* file) {
        if (!file || file->IsZombie()) return;
        std::cout << "\n🔍 Available paths in file:\n══════════════════════════════════════════════\n";
        TIter next(file->GetListOfKeys()); TKey* key;
        while ((key = dynamic_cast<TKey*>(next()))) {
            if (std::string(key->GetClassName()) != "TTree") continue;
            auto* tree = dynamic_cast<TTree*>(file->Get(key->GetName()));
            if (!tree) continue;
            std::cout << "📂 TTree: " << tree->GetName() << " (" << tree->GetEntries() << " entries)\n";
            auto* branches = tree->GetListOfBranches();
            for (int i = 0; i < branches->GetEntries(); ++i) {
                auto* be = dynamic_cast<TBranchElement*>(branches->At(i));
                if (!be || !be->GetClassName()) continue;
                TClass* cls = TClass::GetClass(be->GetClassName());
                if (!cls) continue;
                explore_class(cls, "/" + std::string(be->GetClassName()), "");
            }
        }
        std::cout << "══════════════════════════════════════════════\n";
    }
private:
    static void explore_class(TClass* cls, const std::string& prefix, const std::string& indent) {
        if (!cls) return;
        auto* si = cls->GetStreamerInfo(); if (!si) return;
        auto* elems = si->GetElements();
        for (int i = 0; i < elems->GetEntries(); ++i) {
            auto* elem = dynamic_cast<TStreamerElement*>(elems->At(i));
            if (!elem || elem->IsBase()) continue;
            std::string full = prefix.empty() ? elem->GetName() : prefix + "/" + elem->GetName();
            std::string type = elem->GetTypeName();
            bool is_prim = is_primitive(type);
            bool is_str = (type == "std::string" || type == "string");
            if (is_prim || is_str) { std::cout << indent << "📄 " << full << " [" << type << "]\n"; continue; }
            TClass* ec = elem->GetClassPointer();
            bool is_cont = (ec && ec->GetCollectionProxy());
            if (is_cont) {
                std::cout << indent << "📦 " << full << " [" << type << "]\n";
                TClass* vc = ec->GetCollectionProxy()->GetValueClass();
                if (vc && !is_primitive(vc->GetName()) && vc->GetName() != std::string("std::string"))
                    explore_class(vc, full, indent + "  ");
            } else if (ec && !elem->IsaPointer()) {
                std::cout << indent << "🔹 " << full << " [" << type << "]\n";
                explore_class(ec, full, indent + "  ");
            } else {
                std::cout << indent << "🔗 " << full << " [" << type << "]\n";
            }
        }
    }
    static bool is_primitive(const std::string& type) {
        static const std::vector<std::string> prims = {
            "Bool_t","bool","Char_t","char","UChar_t","unsigned char","Short_t","short",
            "UShort_t","unsigned short","Int_t","int","UInt_t","unsigned int","Long_t","long",
            "ULong_t","unsigned long","Long64_t","long long","ULong64_t","unsigned long long",
            "Float_t","float","Double_t","double"
        };
        return std::find(prims.begin(), prims.end(), type) != prims.end();
    }
};

// ═══════════════════════════════════════════════════════════════
class UniversalReader {
public:
    static void run(const char* fname, const char* query_paths, Long64_t max_values) {
        UniversalReader reader(fname, query_paths, max_values);
        reader.execute();
    }
    
private:
    std::string fname_; std::string query_paths_; Long64_t max_values_;
    UniversalReader(const char* f, const char* p, Long64_t m) : fname_(f), query_paths_(p), max_values_(m) {}

    TTree* find_tree(TFile* file, const std::string& target) {
        if (!file) return nullptr;
        TIter next(file->GetListOfKeys()); TKey* key; TTree* first = nullptr;
        while ((key = dynamic_cast<TKey*>(next()))) {
            if (std::string(key->GetClassName()) != "TTree") continue;
            auto* t = dynamic_cast<TTree*>(file->Get(key->GetName()));
            if (!t) continue;
            if (!first) first = t;
            if (target.empty()) return t;
            auto* br = t->GetListOfBranches();
            for (int i=0; i<br->GetEntries(); ++i) {
                auto* be = dynamic_cast<TBranchElement*>(br->At(i));
                if (be && be->GetClassName() && std::string(be->GetClassName()) == target) return t;
            }
        }
        return first;
    }
    TBranch* find_branch(TTree* tree, const std::string& cls) {
        if (!tree) return nullptr;
        auto* br = tree->GetListOfBranches();
        for (int i=0; i<br->GetEntries(); ++i) {
            auto* be = dynamic_cast<TBranchElement*>(br->At(i));
            if (be && be->GetClassName() && std::string(be->GetClassName()) == cls) return be;
        }
        return br->GetEntries() ? dynamic_cast<TBranch*>(br->At(0)) : nullptr;
    }

    void execute() {
        auto file = open_file(); if (!file) return;
        
        // 🔧 Список путей или листинг
        if (query_paths_.empty() || query_paths_ == "*") {
            PathExplorer::print_all_paths(file.get());
            return;
        }
        
        auto paths = PathParser::split_paths(query_paths_);
        if (paths.empty()) { std::cerr << "❌ No paths specified\n"; return; }
        
        std::cout << "🔍 Universal Reader: " << fname_ << "\n";
        std::cout << " Paths: " << query_paths_ << "\n";
        std::cout << "══════════════════════════════════════════════\n";
        
        // 🔧 Читаем каждый путь
        std::vector<ReadResult> all_results;
        for (const auto& qp : paths) {
            auto parsed = PathParser::parse(qp);
            if (parsed.fields.empty()) { std::cerr << "❌ Invalid path: " << qp << "\n"; continue; }
            
            auto tree = find_tree(file.get(), parsed.root_class);
            if (!tree) { std::cerr << "❌ No TTree for '" << parsed.root_class << "'\n"; continue; }
            auto branch = find_branch(tree, parsed.root_class);
            if (!branch) { std::cerr << "❌ Branch not found\n"; continue; }
            
            auto* root_class = TClass::GetClass(parsed.root_class.c_str());
            if (!root_class || !root_class->HasDictionary()) {
                std::cerr << "❌ Dictionary missing for '" << parsed.root_class << "'\n";
                continue;
            }
            
            auto levels = PathResolver::resolve(root_class, parsed.fields);
            if (levels.empty()) { std::cerr << "❌ Could not resolve: " << qp << "\n"; continue; }
            
            std::cout << "✅ Resolved: " << qp << "\n";
            PathResolver::print_debug(levels);
            
            ReadResult res;
            res.source_path = qp;
            auto result = read_values(tree, branch, root_class, levels, max_values_);
            result.source_path = qp;
            all_results.push_back(result);
        }
        
        if (all_results.empty()) { std::cerr << "❌ No data collected\n"; return; }
        
        // 🔧 Группируем и мерджим по сигнатуре индексов
        auto groups = ResultMerger::group_by_signature(all_results);
        for (auto& [sig, results] : groups) {
            if (results.size() == 1) {
                // Один путь → обычная печать
                TablePrinter::print(PathParser::parse(results[0].source_path).fields.back(),
                                    max_values_, results[0]);
            } else {
                // Несколько путей с одинаковой сигнатурой → мердж
                auto merged = ResultMerger::merge(results);
                ResultMerger::print_table(sig, merged, max_values_);
            }
        }
        
        std::cout << "✅ Done.\n";
    }

    std::unique_ptr<TFile> open_file() {
        auto* f = TFile::Open(fname_.c_str(), "READ");
        if (!f || f->IsZombie()) { std::cerr << "❌ Cannot open " << fname_ << "\n"; return nullptr; }
        return std::unique_ptr<TFile>(f);
    }
    
    ReadResult read_values(TTree* tree, TBranch* branch, TClass* rc,
                           const std::vector<PathLevel>& lvls, Long64_t max_vals) {
        auto* be = dynamic_cast<TBranchElement*>(branch);
        void* obj = rc->New(); be->SetAddress(&obj);
        ReadResult res;
        Long64_t tot = tree->GetEntries();
        Long64_t step = std::max(tot / 20, (Long64_t)5000);
        for (Long64_t ev=0; ev<tot && res.size()<static_cast<size_t>(max_vals); ++ev) {
            if (ev>0 && ev%step==0) { fprintf(stderr, "\r⏳ %lld/%lld (%.1f%%)", ev, tot, 100.*ev/tot); fflush(stderr); }
            tree->GetEntry(ev);
            ValueReader::collect(obj, lvls, max_vals, ev, res);
        }
        if (step>0) fprintf(stderr, "\r⏳ %lld/%lld (100%%)\n", tot, tot);
        rc->Destructor(obj);
        return res;
    }
};

// ═══════════════════════════════════════════════════════════════
void universal_reader(const char* fname = "simple.root",
                      const char* query_paths = "/Event/evt_id",
                      Long64_t max_values = 5) {
    gSystem->Load("./phast.8.044/lib/libPhast.so");
    UniversalReader::run(fname, query_paths, max_values);
}