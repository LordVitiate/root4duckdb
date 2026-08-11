#include "TFile.h"
#include "TTree.h"
#include "TBranch.h"
#include "TBasket.h"
#include "TLeaf.h"
#include "TString.h"
#include "TSystem.h"

#include <TBranchElement.h>
#include <TClass.h>
#include <TStreamerInfo.h>
#include <TStreamerElement.h>
#include <TVirtualCollectionProxy.h>
#include <TKey.h>

#undef BIT

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cctype>
#include <chrono>
#include <fstream>
#include <functional>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <unordered_map>
#include <thread>
#include <vector>

#include "duckdb/main/extension/extension_loader.hpp"
#include "duckdb/parser/parsed_data/create_table_function_info.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/file_system.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckdb/common/types/data_chunk.hpp"
#include "duckdb/common/types/vector.hpp"
#include "duckdb/planner/table_filter.hpp"
#include "duckdb/planner/filter/constant_filter.hpp" 
#include "duckdb/planner/filter/conjunction_filter.hpp"

#include <nlohmann/json.hpp>
#include "include/root_meta.hpp"
#include "include/root_branch_projection.hpp"
#include "include/root_debug.hpp"
#include "include/root_direct_scheduler.hpp"
#include "include/root_file_opener.hpp"
#include "include/root_filter.hpp"
#include "include/root_input_resolver.hpp"
#include "include/root_lake_common.hpp"
#include "include/root_runtime_settings.hpp"
#include "include/root_serialized_reader.hpp"

namespace duckdb {

// ============================================================
// Структуры для простых веток
// ============================================================
struct SimpleBranchInfo {
    std::string name;
    std::string type_name;
    TBranch* branch = nullptr;
    TLeaf* leaf = nullptr;
};

// ============================================================
// ObjectContext (был RootObjectContext)
// ============================================================
enum class DirectDictionaryCleanupMode : uint8_t {
    FULL = 0,
    DESTRUCT_ONLY = 1,
    RETAIN = 2
};

struct ObjectContext
{
    TTree* tree = nullptr;
    TBranch* branch = nullptr;
    TClass* root_class = nullptr;
    void* owned_object = nullptr;
    void* address_slot = nullptr;
    std::string class_name;
    DirectDictionaryCleanupMode cleanup_mode = DirectDictionaryCleanupMode::FULL;

    ObjectContext() = default;
    ObjectContext(const ObjectContext&) = delete;
    ObjectContext& operator=(const ObjectContext&) = delete;

    ObjectContext(ObjectContext&& other) noexcept { MoveFrom(std::move(other)); }
    ObjectContext& operator=(ObjectContext&& other) noexcept {
        if (this != &other) {
            Reset();
            MoveFrom(std::move(other));
        }
        return *this;
    }
    ~ObjectContext() { Reset(); }

    void Bind(TTree* tree_p, TBranch* branch_p, TClass* class_p, std::string class_name_p,
              DirectDictionaryCleanupMode cleanup_mode_p = DirectDictionaryCleanupMode::FULL)
    {
        Reset();
        tree = tree_p;
        branch = branch_p;
        root_class = class_p;
        class_name = std::move(class_name_p);
        cleanup_mode = cleanup_mode_p;
        if (!tree || !branch || !root_class) {
            throw InvalidInputException("Cannot bind null ROOT tree/branch/class");
        }
        RootDebug("OBJECT.BEFORE_NEW", "class=" + class_name + " class_ptr=" + RootPointer(root_class));
        owned_object = root_class->New();
        RootDebug("OBJECT.AFTER_NEW", "class=" + class_name + " object_ptr=" + RootPointer(owned_object));
        if (!owned_object) throw IOException("TClass::New failed for " + class_name);
        address_slot = owned_object;
        branch->SetAutoDelete(kFALSE);
        RootDebug("OBJECT.BEFORE_SET_ADDRESS",
                  "class=" + class_name + " branch_ptr=" + RootPointer(branch) +
                  " address_slot_ptr=" + RootPointer(&address_slot) +
                  " object_ptr=" + RootPointer(address_slot));
        branch->SetAddress(&address_slot);
        RootDebug("OBJECT.AFTER_SET_ADDRESS", "class=" + class_name);
    }

    void* CurrentObject() const { return address_slot; }

private:
    void MoveFrom(ObjectContext&& other)
    {
        tree = other.tree;
        branch = other.branch;
        root_class = other.root_class;
        owned_object = other.owned_object;
        address_slot = other.address_slot;
        class_name = std::move(other.class_name);
        cleanup_mode = other.cleanup_mode;
        other.tree = nullptr;
        other.branch = nullptr;
        other.root_class = nullptr;
        other.owned_object = nullptr;
        other.address_slot = nullptr;
        other.cleanup_mode = DirectDictionaryCleanupMode::FULL;
        if (branch) branch->SetAddress(&address_slot);
    }

    void Reset()
    {
        if (branch) {
            RootDebug("OBJECT.BEFORE_RESET_ADDRESS",
                      "class=" + class_name + " branch_ptr=" + RootPointer(branch));
            branch->ResetAddress();
            RootDebug("OBJECT.AFTER_RESET_ADDRESS", "class=" + class_name);
        }
        if (owned_object && root_class) {
            RootDebug("OBJECT.BEFORE_DESTRUCTOR",
                      "class=" + class_name + " object_ptr=" + RootPointer(owned_object) +
                      " cleanup_mode=" + std::to_string(static_cast<int>(cleanup_mode)));
            if (cleanup_mode == DirectDictionaryCleanupMode::FULL) {
                root_class->Destructor(owned_object, kFALSE);
            } else if (cleanup_mode == DirectDictionaryCleanupMode::DESTRUCT_ONLY) {
                root_class->Destructor(owned_object, kTRUE);
            } else {
                RootDebug("OBJECT.RETAINED", "class=" + class_name);
            }
            RootDebug("OBJECT.AFTER_DESTRUCTOR", "class=" + class_name);
        }
        tree = nullptr;
        branch = nullptr;
        root_class = nullptr;
        owned_object = nullptr;
        address_slot = nullptr;
        class_name.clear();
        cleanup_mode = DirectDictionaryCleanupMode::FULL;
    }
};

// ============================================================
// PathLevel
// ============================================================
struct PathLevel
{
    std::string name;
    std::string type;
    Long64_t offset_in_parent;
    Long64_t cumulative_offset;
    bool is_primitive;
    bool is_string;
    bool is_pointer;
    bool is_container;
    bool is_fixed_array;
    uint64_t fixed_array_length;
    uint32_t element_size;
    std::vector<uint32_t> array_dimensions;
    TClass* klass;
    TClass* element_class;

    PathLevel()
        : offset_in_parent(-1),
          cumulative_offset(0),
          is_primitive(false),
          is_string(false),
          is_pointer(false),
          is_container(false),
          is_fixed_array(false),
          fixed_array_length(0),
          element_size(0),
          klass(nullptr),
          element_class(nullptr)
    {
    }
};

// ============================================================
// PathParser
// ============================================================
class PathParser
{
public:
    struct ParsedPath
    {
        std::string root_class;
        std::vector<std::string> fields;
        std::string original;
    };

    static ParsedPath Parse(const std::string& path)
    {
        ParsedPath result;
        result.original = path;
        std::string p = path;
        
        if (!p.empty() && p[0] == '/')
        {
            p = p.substr(1);
        }
        if (p.find("events/") == 0)
        {
            p = p.substr(8);
        }
        
        std::vector<std::string> parts;
        std::stringstream ss(p);
        std::string part;
        
        while (std::getline(ss, part, '/'))
        {
            if (!part.empty())
            {
                parts.push_back(part);
            }
        }
        
        if (parts.empty())
        {
            return result;
        }
        
        result.root_class = parts[0];
        result.fields.assign(parts.begin() + 1, parts.end());
        return result;
    }

    static std::vector<std::string> SplitPaths(const std::string& query)
    {
        std::vector<std::string> paths;
        std::stringstream ss(query);
        std::string path;
        
        while (std::getline(ss, path, ','))
        {
            size_t start = path.find_first_not_of(" \t");
            size_t end = path.find_last_not_of(" \t");
            if (start != std::string::npos)
            {
                paths.push_back(path.substr(start, end - start + 1));
            }
        }
        return paths;
    }
};

// ============================================================
// BuildIndexSignature
// ============================================================
static std::string BuildIndexSignature(const std::vector<PathLevel>& levels)
{
    std::vector<std::string> names;
    for (const auto& lvl : levels)
    {
        if (lvl.is_container)
        {
            names.push_back(lvl.name + "_idx");
        }
        if (lvl.is_fixed_array)
        {
            if (lvl.array_dimensions.size() <= 1)
            {
                names.push_back(lvl.name + "_idx");
            }
            else
            {
                for (size_t dim = 0; dim < lvl.array_dimensions.size(); ++dim)
                {
                    names.push_back(lvl.name + "_dim" + std::to_string(dim) + "_idx");
                }
            }
        }
    }
    std::string sig;
    for (size_t i = 0; i < names.size(); ++i)
    {
        if (i) sig += ",";
        sig += names[i];
    }
    return sig;
}

// ============================================================
// PathResolver
// ============================================================
class PathResolver
{
public:
    static std::string ExtractInnerType(const std::string& container_type)
    {
        size_t template_start = container_type.find('<');
        if (template_start == std::string::npos)
        {
            return "";
        }
        
        std::string inner = container_type.substr(template_start + 1);
        size_t depth = 1;
        size_t end_pos = 0;
        size_t first_comma = std::string::npos;
        
        for (size_t i = 0; i < inner.size(); ++i)
        {
            if (inner[i] == '<')
            {
                depth++;
            }
            else if (inner[i] == '>')
            {
                depth--;
                if (depth == 0)
                {
                    end_pos = i;
                    break;
                }
            }
            else if (inner[i] == ',' && depth == 1 && first_comma == std::string::npos)
            {
                first_comma = i;
            }
        }
        
        if (end_pos == 0)
        {
            return "";
        }
        
        if (first_comma != std::string::npos && first_comma < end_pos)
        {
            inner = inner.substr(0, first_comma);
        }
        else
        {
            inner = inner.substr(0, end_pos);
        }
        
        if (inner.find("std::") == 0)
        {
            inner = inner.substr(5);
        }
        
        size_t s = inner.find_first_not_of(" \t");
        size_t e = inner.find_last_not_of(" \t");
        if (s == std::string::npos)
        {
            return "";
        }
        
        return inner.substr(s, e - s + 1);
    }

    static std::string PrimaryTemplateName(std::string type)
    {
        size_t first = type.find_first_not_of(" \t\r\n");
        if (first == std::string::npos) return "";
        size_t last = type.find_last_not_of(" \t\r\n");
        type = type.substr(first, last - first + 1);
        if (type.find("std::") == 0) type = type.substr(5);
        const auto open = type.find('<');
        if (open != std::string::npos) type.resize(open);
        return type;
    }

    static std::vector<std::string> TemplateArgs(const std::string& type)
    {
        const auto open = type.find('<');
        if (open == std::string::npos) return {};
        std::vector<std::string> args;
        int depth = 0;
        size_t begin = open + 1;
        for (size_t i = open + 1; i < type.size(); ++i)
        {
            if (type[i] == '<') ++depth;
            else if (type[i] == '>')
            {
                if (depth == 0)
                {
                    auto arg = type.substr(begin, i - begin);
                    size_t a = arg.find_first_not_of(" \t");
                    size_t b = arg.find_last_not_of(" \t");
                    args.push_back(a == std::string::npos ? "" : arg.substr(a, b - a + 1));
                    return args;
                }
                --depth;
            }
            else if (type[i] == ',' && depth == 0)
            {
                auto arg = type.substr(begin, i - begin);
                size_t a = arg.find_first_not_of(" \t");
                size_t b = arg.find_last_not_of(" \t");
                args.push_back(a == std::string::npos ? "" : arg.substr(a, b - a + 1));
                begin = i + 1;
            }
        }
        return {};
    }

    static bool IsSTLContainer(const std::string& type)
    {
        const auto name = PrimaryTemplateName(type);
        return name == "vector" || name == "set" || name == "multiset" || name == "list" ||
               name == "deque" || name == "map" || name == "multimap" || name == "unordered_map" ||
               name == "unordered_multimap" || name == "unordered_set" || name == "unordered_multiset";
    }

    static bool IsMap(const std::string& type)
    {
        const auto name = PrimaryTemplateName(type);
        return name == "map" || name == "multimap" || name == "unordered_map" ||
               name == "unordered_multimap";
    }

    static bool IsContiguousVector(const std::string& type)
    {
        return PrimaryTemplateName(type) == "vector";
    }

    static bool IsPointerType(std::string type)
    {
        size_t end = type.find_last_not_of(" \t");
        return end != std::string::npos && type[end] == '*';
    }

    static bool IsPrimitive(const std::string& type)
    {
        std::string t = type;
        size_t first = t.find_first_not_of(" \t");
        if (first != std::string::npos) t = t.substr(first);
        if (t.find("const ") == 0) t = t.substr(6);
        if (t.find("std::") == 0) t = t.substr(5);
        const auto bracket = t.find('[');
        if (bracket != std::string::npos)
        {
            t = t.substr(0, bracket);
        }
        
        static const std::vector<std::string> prims = {
            "Bool_t", "bool", "Char_t", "char", "UChar_t", "unsigned char",
            "Short_t", "short", "UShort_t", "unsigned short",
            "Int_t", "int", "UInt_t", "unsigned int",
            "Long_t", "long", "ULong_t", "unsigned long",
            "Long64_t", "long long", "ULong64_t", "unsigned long long",
            "Float_t", "float", "Double_t", "double"
        };
        return std::find(prims.begin(), prims.end(), t) != prims.end();
    }

    static bool IsString(const std::string& type)
    {
        std::string t = type;
        if (t.find("std::") == 0)
        {
            t = t.substr(5);
        }
        return t == "string" || t == "TString";
    }

    struct FieldMatch
    {
        TStreamerElement* element = nullptr;
        Long64_t offset = 0;
    };

    static std::optional<FieldMatch> FindField(TClass* cls, const std::string& name,
                                               std::vector<std::string>& visited)
    {
        if (!cls)
        {
            return std::nullopt;
        }
        const std::string class_name = cls->GetName();
        RootDebug("PATH.FIND_FIELD",
                  "class=" + class_name + " field=" + name +
                  " class_ptr=" + RootPointer(cls));
        if (std::find(visited.begin(), visited.end(), class_name) != visited.end())
        {
            return std::nullopt;
        }
        visited.push_back(class_name);
        RootDebug("STREAMER.BEFORE",
                  "class=" + class_name + " calling GetStreamerInfo");
        auto* info = cls->GetStreamerInfo();
        RootDebug("STREAMER.AFTER",
                  "class=" + class_name + " info_ptr=" + RootPointer(info));
        auto* elements = info ? info->GetElements() : nullptr;
        RootDebug("STREAMER.ELEMENTS",
                  "class=" + class_name + " elements_ptr=" + RootPointer(elements) +
                  " count=" + std::to_string(elements ? elements->GetEntries() : 0));
        for (int i = 0; elements && i < elements->GetEntries(); ++i)
        {
            auto* elem = dynamic_cast<TStreamerElement*>(elements->At(i));
            if (!elem || elem->IsBase())
            {
                continue;
            }
            if (name == elem->GetName())
            {
                FieldMatch match;
                match.element = elem;
                match.offset = info->GetElementOffset(i);
                RootDebug("PATH.FIELD_FOUND",
                          "class=" + class_name + " field=" + name +
                          " type=" + std::string(elem->GetTypeName()) +
                          " offset=" + std::to_string(match.offset) +
                          " element_ptr=" + RootPointer(elem));
                visited.pop_back();
                return match;
            }
        }
        for (int i = 0; elements && i < elements->GetEntries(); ++i)
        {
            auto* base = dynamic_cast<TStreamerElement*>(elements->At(i));
            if (!base || !base->IsBase())
            {
                continue;
            }
            auto* base_class = base->GetClassPointer();
            if (!base_class)
            {
                base_class = TClass::GetClass(base->GetTypeName());
            }
            auto nested = FindField(base_class, name, visited);
            if (nested)
            {
                nested->offset += info->GetElementOffset(i);
                visited.pop_back();
                return nested;
            }
        }
        visited.pop_back();
        return std::nullopt;
    }

    static uint32_t PrimitiveSize(const std::string& type)
    {
        std::string t = type;
        size_t first = t.find_first_not_of(" \t");
        if (first != std::string::npos) t = t.substr(first);
        if (t.find("const ") == 0) t = t.substr(6);
        if (t.find("std::") == 0) t = t.substr(5);
        const auto bracket = t.find('[');
        if (bracket != std::string::npos)
        {
            t = t.substr(0, bracket);
        }
        if (t == "Bool_t" || t == "bool") return sizeof(bool);
        if (t == "Char_t" || t == "char") return sizeof(int8_t);
        if (t == "UChar_t" || t == "unsigned char") return sizeof(uint8_t);
        if (t == "Short_t" || t == "short") return sizeof(int16_t);
        if (t == "UShort_t" || t == "unsigned short") return sizeof(uint16_t);
        if (t == "Int_t" || t == "int") return sizeof(int32_t);
        if (t == "UInt_t" || t == "unsigned int") return sizeof(uint32_t);
        if (t == "Long_t" || t == "long") return sizeof(long);
        if (t == "ULong_t" || t == "unsigned long") return sizeof(unsigned long);
        if (t == "Long64_t" || t == "long long") return sizeof(int64_t);
        if (t == "ULong64_t" || t == "unsigned long long") return sizeof(uint64_t);
        if (t == "Float_t" || t == "float") return sizeof(float);
        if (t == "Double_t" || t == "double") return sizeof(double);
        return 0;
    }

    static std::vector<PathLevel> resolve(TClass* root_class, const std::vector<std::string>& fields)
    {
        std::vector<PathLevel> levels;
        if (!root_class || fields.empty())
        {
            return levels;
        }

        TClass* current_class = root_class;
        Long64_t cumulative = 0;
        RootDebug("PATH.RESOLVE_BEGIN",
                  "root_class=" + std::string(root_class->GetName()) +
                  " fields=" + JoinDebugFields(fields) +
                  " class_ptr=" + RootPointer(root_class));

        for (size_t fld_idx = 0; fld_idx < fields.size(); ++fld_idx)
        {
            const auto& name = fields[fld_idx];
            RootDebug("PATH.RESOLVE_LEVEL",
                      "index=" + std::to_string(fld_idx) +
                      " field=" + name +
                      " current_class=" +
                          std::string(current_class ? current_class->GetName() : "<null>") +
                      " class_ptr=" + RootPointer(current_class));

            if (name == "value" && !levels.empty() && levels.back().is_container &&
                !IsMap(levels.back().type))
            {
                const auto& parent = levels.back();
                std::string inner_type = parent.element_class ? parent.element_class->GetName()
                                                              : ExtractInnerType(parent.type);
                if (inner_type.empty()) return {};
                PathLevel lvl;
                lvl.name = "value";
                lvl.type = inner_type;
                lvl.offset_in_parent = 0;
                lvl.cumulative_offset = parent.cumulative_offset;
                lvl.klass = parent.element_class;
                if (!lvl.klass) lvl.klass = TClass::GetClass(inner_type.c_str());
                lvl.is_primitive = IsPrimitive(inner_type);
                lvl.is_string = IsString(inner_type);
                lvl.is_container = lvl.klass && lvl.klass->GetCollectionProxy();
                if (lvl.is_container) lvl.element_class = lvl.klass->GetCollectionProxy()->GetValueClass();
                lvl.element_size = lvl.is_primitive ? PrimitiveSize(inner_type)
                                                    : (lvl.klass ? static_cast<uint32_t>(lvl.klass->Size()) : 0);
                const bool terminal = fld_idx == fields.size() - 1;
                if (terminal && !lvl.is_primitive && !lvl.is_string) return {};
                if (!terminal && !lvl.klass) return {};
                levels.push_back(lvl);
                cumulative = lvl.is_container ? 0 : lvl.cumulative_offset;
                current_class = lvl.is_container ? lvl.element_class : lvl.klass;
                continue;
            }

            if (!current_class)
            {
                return {};
            }
            std::string streamer_name = name;
            if (!levels.empty() && levels.back().is_container && IsMap(levels.back().type))
            {
                if (name == "key") streamer_name = "first";
                else if (name == "value") streamer_name = "second";
            }
            std::vector<std::string> visited;
            auto match = FindField(current_class, streamer_name, visited);
            if (!match || !match->element)
            {
                return {};
            }
            auto* elem = match->element;
            RootDebug("PATH.ELEMENT",
                      "field=" + name + " type=" + std::string(elem->GetTypeName()) +
                      " pointer=" + std::to_string(elem->IsaPointer() ? 1 : 0) +
                      " class_ptr=" + RootPointer(elem->GetClassPointer()));
            PathLevel lvl;
            lvl.name = name;
            lvl.type = elem->GetTypeName();
            lvl.offset_in_parent = match->offset;
            lvl.cumulative_offset = cumulative + lvl.offset_in_parent;
            lvl.is_primitive = IsPrimitive(lvl.type);
            lvl.is_string = IsString(lvl.type);
            lvl.is_pointer = elem->IsaPointer();
            TClass* elem_class = elem->GetClassPointer();
            lvl.is_container = elem_class && elem_class->GetCollectionProxy();
            lvl.klass = elem_class;
            if (lvl.is_container)
            {
                lvl.element_class = elem_class->GetCollectionProxy()->GetValueClass();
            }

            const int rank = elem->GetArrayDim();
            uint64_t length = 1;
            for (int dim = 0; dim < rank; ++dim)
            {
                const int extent = elem->GetMaxIndex(dim);
                if (extent <= 0)
                {
                    length = 0;
                    lvl.array_dimensions.clear();
                    break;
                }
                lvl.array_dimensions.push_back(static_cast<uint32_t>(extent));
                length *= static_cast<uint64_t>(extent);
            }
            lvl.is_fixed_array = !lvl.array_dimensions.empty() && length > 0;
            lvl.fixed_array_length = lvl.is_fixed_array ? length : 0;
            lvl.element_size = lvl.is_primitive ? PrimitiveSize(lvl.type)
                                                : (elem_class ? static_cast<uint32_t>(elem_class->Size()) : 0);
            if (lvl.is_fixed_array && !lvl.element_size)
            {
                return {};
            }

            RootDebug("PATH.LEVEL_READY",
                      "field=" + lvl.name + " type=" + lvl.type +
                      " offset=" + std::to_string(lvl.offset_in_parent) +
                      " cumulative=" + std::to_string(lvl.cumulative_offset) +
                      " primitive=" + std::to_string(lvl.is_primitive ? 1 : 0) +
                      " string=" + std::to_string(lvl.is_string ? 1 : 0) +
                      " container=" + std::to_string(lvl.is_container ? 1 : 0) +
                      " fixed_array=" + std::to_string(lvl.is_fixed_array ? 1 : 0) +
                      " klass_ptr=" + RootPointer(lvl.klass) +
                      " element_class_ptr=" + RootPointer(lvl.element_class));
            levels.push_back(lvl);
            if (lvl.is_pointer)
            {
                cumulative = 0;
                current_class = elem_class;
            }
            else if (lvl.is_container)
            {
                cumulative = 0;
                current_class = lvl.element_class;
            }
            else if (lvl.is_fixed_array && elem_class)
            {
                cumulative = 0;
                current_class = elem_class;
            }
            else if (elem_class)
            {
                cumulative = lvl.cumulative_offset;
                current_class = elem_class;
            }
            else
            {
                current_class = nullptr;
            }
        }
        RootDebug("PATH.RESOLVE_END",
                  "levels=" + std::to_string(levels.size()) +
                  " fields=" + JoinDebugFields(fields));
        return levels;
    }

};

// ============================================================
// Direct semantic path selection
//
// Exact reads must never discover the complete ROOT schema.  The requested
// semantic path is resolved directly from TStreamerInfo.  When the requested
// path denotes an object/container, only its immediate children are inspected.
// ============================================================
static void CollectImmediateSemanticChildren(
    TClass* cls,
    const std::string& prefix,
    std::vector<std::string>& primitive_paths,
    std::set<std::string>& child_paths,
    std::set<std::string>& active_base_classes)
{
    if (!cls)
    {
        return;
    }

    const std::string class_name = cls->GetName();
    if (!active_base_classes.insert(class_name).second)
    {
        return;
    }

    auto* info = cls->GetStreamerInfo();
    auto* elements = info ? info->GetElements() : nullptr;
    if (!elements)
    {
        active_base_classes.erase(class_name);
        return;
    }

    // Base-class data members are semantically direct members of the object.
    for (int i = 0; i < elements->GetEntries(); ++i)
    {
        auto* base = dynamic_cast<TStreamerElement*>(elements->At(i));
        if (!base || !base->IsBase())
        {
            continue;
        }
        auto* base_class = base->GetClassPointer();
        if (!base_class)
        {
            base_class = TClass::GetClass(base->GetTypeName());
        }
        CollectImmediateSemanticChildren(
            base_class, prefix, primitive_paths, child_paths, active_base_classes);
    }

    for (int i = 0; i < elements->GetEntries(); ++i)
    {
        auto* elem = dynamic_cast<TStreamerElement*>(elements->At(i));
        if (!elem || elem->IsBase())
        {
            continue;
        }

        const std::string full = prefix + elem->GetName();
        const std::string type = elem->GetTypeName();
        if (PathResolver::IsPrimitive(type) || PathResolver::IsString(type))
        {
            primitive_paths.push_back(full);
            continue;
        }

        // Complex objects, pointers and containers are exposed as immediate
        // children.  They are not recursively explored during bind.
        auto* elem_class = elem->GetClassPointer();
        if (elem_class || elem->IsaPointer())
        {
            child_paths.insert(full + "/");
        }
    }

    active_base_classes.erase(class_name);
}

static bool SelectSemanticPathDirectly(
    TClass* root_class,
    const PathParser::ParsedPath& parsed,
    const std::string& path_prefix_raw,
    std::string& bind_prefix,
    std::vector<std::string>& primitive_paths,
    std::set<std::string>& child_paths)
{
    if (!root_class)
    {
        return false;
    }

    std::string canonical = path_prefix_raw;
    while (canonical.size() > 1 && canonical.back() == '/')
    {
        canonical.pop_back();
    }

    if (parsed.fields.empty())
    {
        bind_prefix = canonical + "/";
        std::set<std::string> active_bases;
        CollectImmediateSemanticChildren(
            root_class, bind_prefix, primitive_paths, child_paths, active_bases);
        return !primitive_paths.empty() || !child_paths.empty();
    }

    auto levels = PathResolver::resolve(root_class, parsed.fields);
    if (levels.empty())
    {
        return false;
    }

    const auto& terminal = levels.back();
    if (terminal.is_primitive || terminal.is_string)
    {
        bind_prefix = canonical;
        primitive_paths.push_back(canonical);
        return true;
    }

    TClass* target_class = nullptr;
    if (terminal.is_container)
    {
        if (PathResolver::IsMap(terminal.type))
        {
            bind_prefix = canonical + "/";
            primitive_paths.push_back(canonical + "/key");
            primitive_paths.push_back(canonical + "/value");
            return true;
        }

        std::string inner_type = terminal.element_class
            ? terminal.element_class->GetName()
            : PathResolver::ExtractInnerType(terminal.type);
        if (PathResolver::IsPrimitive(inner_type) || PathResolver::IsString(inner_type))
        {
            bind_prefix = canonical + "/";
            primitive_paths.push_back(canonical + "/value");
            return true;
        }
        target_class = terminal.element_class;
    }
    else
    {
        target_class = terminal.klass;
    }

    if (!target_class)
    {
        return false;
    }

    bind_prefix = canonical + "/";
    std::set<std::string> active_bases;
    CollectImmediateSemanticChildren(
        target_class, bind_prefix, primitive_paths, child_paths, active_bases);
    return !primitive_paths.empty() || !child_paths.empty();
}

// ============================================================
// Вспомогательные функции find_tree, find_branch
// ============================================================
static TTree* find_tree(TFile* file, const std::string& target_class)
{
    if (!file)
    {
        return nullptr;
    }
    TIter next(file->GetListOfKeys());
    TKey* key;
    TTree* first = nullptr;
    while ((key = dynamic_cast<TKey*>(next())))
    {
        if (std::string(key->GetClassName()) != "TTree")
        {
            continue;
        }
        auto* t = dynamic_cast<TTree*>(file->Get(key->GetName()));
        if (!t)
        {
            continue;
        }
        if (!first)
        {
            first = t;
        }
        if (target_class.empty())
        {
            return t;
        }
        auto* br = t->GetListOfBranches();
        for (int i = 0; i < br->GetEntries(); ++i)
        {
            auto* be = dynamic_cast<TBranchElement*>(br->At(i));
            if (be && be->GetClassName() && std::string(be->GetClassName()) == target_class)
            {
                return t;
            }
        }
    }
    return target_class.empty() ? first : nullptr;
}

static TBranch* find_branch(TTree* tree, const std::string& target_class)
{
    if (!tree)
    {
        return nullptr;
    }
    auto* br = tree->GetListOfBranches();
    for (int i = 0; i < br->GetEntries(); ++i)
    {
        auto* be = dynamic_cast<TBranchElement*>(br->At(i));
        if (be && be->GetClassName() && std::string(be->GetClassName()) == target_class)
        {
            return be;
        }
    }
    return target_class.empty() && br->GetEntries() ? dynamic_cast<TBranch*>(br->At(0)) : nullptr;
}

// ============================================================
// RootTypeToDuckDB (дополнена типами для листьев)
// ============================================================
static LogicalType RootTypeToDuckDB(const std::string& root_type, bool is_string, bool is_primitive)
{
    if (is_string || !is_primitive)
    {
        return LogicalType::VARCHAR;
    }

    std::string t = root_type;
    // Обработка коротких обозначений из TLeaf
    if (t == "I") return LogicalType::INTEGER;
    if (t == "F") return LogicalType::FLOAT;
    if (t == "D") return LogicalType::DOUBLE;
    if (t == "L") return LogicalType::BIGINT;
    if (t == "b") return LogicalType::TINYINT;   // signed char
    if (t == "B") return LogicalType::UTINYINT;  // unsigned char
    if (t == "O") return LogicalType::BOOLEAN;   // bool
    if (t == "S") return LogicalType::SMALLINT;
    if (t == "s") return LogicalType::USMALLINT;
    if (t == "i") return LogicalType::UINTEGER;
    if (t == "l") return LogicalType::UBIGINT;

    if (t == "Double_t" || t == "double") return LogicalType::DOUBLE;
    if (t == "Float_t" || t == "float") return LogicalType::FLOAT;
    if (t == "Int_t" || t == "int") return LogicalType::INTEGER;
    if (t == "Long64_t" || t == "long long") return LogicalType::BIGINT;
    if (t == "Char_t" || t == "char") return LogicalType::TINYINT;
    if (t == "UChar_t" || t == "unsigned char") return LogicalType::UTINYINT;    
    if (t == "Bool_t" || t == "bool") return LogicalType::BOOLEAN;
    if (t == "Short_t" || t == "short") return LogicalType::SMALLINT;
    if (t == "UShort_t" || t == "unsigned short") return LogicalType::USMALLINT;
    if (t == "UInt_t" || t == "unsigned int") return LogicalType::UINTEGER;
    if (t == "ULong_t" || t == "unsigned long" ||
        t == "ULong64_t" || t == "unsigned long long") return LogicalType::UBIGINT;

    return LogicalType::VARCHAR;
}

// ============================================================
// RootLakeColumnInfo
// Unique extension prefix is required: DuckDB itself defines duckdb::ColumnInfo.
// ============================================================
struct RootLakeColumnInfo
{
    std::string name;
    std::string logical_path;
    MetaColumnType type;
    std::string branch_name;
    std::string root_type;
    bool is_string = false;
    std::vector<PathLevel> levels;
    std::string index_signature;
    bool is_virtual_index = false;

    LogicalType ToDuckDBType() const
    {
        if (is_virtual_index)
        {
            return LogicalType(LogicalTypeId::INTEGER);
        }
        if (!root_type.empty())
        {
            return RootTypeToDuckDB(root_type, is_string, true);
        }
        switch (type)
        {
            case MetaColumnType::INT32: return LogicalType(LogicalTypeId::INTEGER);
            case MetaColumnType::FLOAT: return LogicalType(LogicalTypeId::FLOAT);
            case MetaColumnType::DOUBLE: return LogicalType(LogicalTypeId::DOUBLE);
            case MetaColumnType::INT64: return LogicalType(LogicalTypeId::BIGINT);
            default: return LogicalType(LogicalTypeId::VARCHAR);
        }
    }
};

// ============================================================
// BasketMeta (для индекса)
// ============================================================
struct BasketMeta
{
    uint16_t column_index;
    uint64_t start_row;
    uint32_t num_rows;
    double min_value;
    double max_value;
    uint32_t bloom_size;
    std::vector<uint8_t> bloom_filter;

    [[nodiscard]] bool ContainsRow(uint64_t global_row) const noexcept
    {
        return global_row >= start_row && global_row < start_row + num_rows;
    }

    [[nodiscard]] uint64_t EndRow() const noexcept
    {
        return start_row + num_rows;
    }
};

// ============================================================
// BloomFilter
// ============================================================
class BloomFilter
{
    std::vector<uint8_t> filter_data_;
    uint32_t bit_size_ = 0;

public:
    BloomFilter() = default;

    explicit BloomFilter(std::vector<uint8_t> data)
        : filter_data_(std::move(data)), bit_size_(filter_data_.size() * 8)
    {
    }

    [[nodiscard]] static uint64_t HashFNV1a(const void* data, size_t len)
    {
        const uint8_t* bytes = static_cast<const uint8_t*>(data);
        uint64_t hash = 14695981039346656037ULL;
        for (size_t i = 0; i < len; ++i)
        {
            hash ^= bytes[i];
            hash *= 1099511628211ULL;
        }
        return hash;
    }

    [[nodiscard]] static std::pair<uint32_t, uint32_t> ComputePositions(uint64_t h1, uint32_t bit_size)
    {
        uint64_t h2 = h1 ^ 0x9e3779b97f4a7c15ULL;
        return { static_cast<uint32_t>(h1 % bit_size), static_cast<uint32_t>(h2 % bit_size) };
    }

    [[nodiscard]] bool CheckBit(uint32_t bit_pos) const
    {
        if (bit_size_ == 0)
        {
            return true;
        }
        uint32_t byte_idx = bit_pos / 8;
        uint32_t bit_idx = bit_pos % 8;
        return (filter_data_[byte_idx] & (1u << bit_idx)) != 0;
    }

    template<typename T>
    [[nodiscard]] bool Contains(const T& value) const
    {
        if (filter_data_.empty())
        {
            return true;
        }

        uint64_t h1 = HashFNV1a(&value, sizeof(T));
        auto [pos1, pos2] = ComputePositions(h1, bit_size_);

        return CheckBit(pos1) && CheckBit(pos2);
    }

    [[nodiscard]] bool empty() const noexcept
    {
        return filter_data_.empty();
    }
};

// ============================================================
// BinaryMetadataReader
// ============================================================
class BinaryMetadataReader
{
    const std::vector<uint8_t>& buffer_;
    size_t offset_ = 0;

public:
    explicit BinaryMetadataReader(const std::vector<uint8_t>& buffer) : buffer_(buffer) {}

    template<typename T>
    T Read()
    {
        if (offset_ + sizeof(T) > buffer_.size())
        {
            throw IOException("Binary metadata EOF.");
        }
        T value;
        std::memcpy(&value, &buffer_[offset_], sizeof(T));
        offset_ += sizeof(T);
        return value;
    }

    std::vector<uint8_t> ReadBytes(uint32_t count)
    {
        if (offset_ + count > buffer_.size())
        {
            throw IOException("Binary metadata block EOF.");
        }
        std::vector<uint8_t> result(buffer_.begin() + offset_, buffer_.begin() + offset_ + count);
        offset_ += count;
        return result;
    }

    [[nodiscard]] bool HasMore() const
    {
        return offset_ < buffer_.size();
    }
};

// ============================================================
// MetadataLoader (загрузка индекса)
// ============================================================
class MetadataLoader
{
public:
    struct LoadResult
    {
        std::string tree_name;
        uint64_t total_rows = 0;
        std::vector<RootLakeColumnInfo> columns;
        std::vector<BasketMeta> baskets;
    };

private:
    static MetaColumnType ParseColumnType(const std::string& type_name)
    {
        if (type_name == "Int_t" || type_name == "I") return MetaColumnType::INT32;
        if (type_name == "Float_t" || type_name == "F") return MetaColumnType::FLOAT;
        if (type_name == "Double_t" || type_name == "D") return MetaColumnType::DOUBLE;
        if (type_name == "Long64_t" || type_name == "L") return MetaColumnType::INT64;
        return MetaColumnType::UNKNOWN;
    }

    static std::vector<BasketMeta> ParseBinaryTail(const std::vector<uint8_t>& buffer)
    {
        std::vector<BasketMeta> baskets;
        BinaryMetadataReader reader(buffer);

        while (reader.HasMore())
        {
            BasketMeta basket;
            basket.column_index = reader.Read<uint16_t>();
            basket.start_row = reader.Read<uint64_t>();
            basket.num_rows = reader.Read<uint32_t>();
            basket.min_value = reader.Read<double>();
            basket.max_value = reader.Read<double>();
            basket.bloom_size = reader.Read<uint32_t>();

            if (basket.bloom_size > 0)
            {
                basket.bloom_filter = reader.ReadBytes(basket.bloom_size);
            }
            baskets.push_back(std::move(basket));
        }
        return baskets;
    }

public:
    static LoadResult Load(const std::string& meta_path)
    {
        LoadResult result;

        std::ifstream in(meta_path, std::ios::binary);
        if (!in)
        {
            throw IOException("Index not found: " + meta_path);
        }

        std::string json_line;
        std::getline(in, json_line);
        auto root_meta_json = nlohmann::json::parse(json_line);

        result.tree_name = root_meta_json["tree_name"].get<std::string>();
        result.total_rows = root_meta_json["total_entries"].get<uint64_t>();

        for (const auto& col : root_meta_json["columns"])
        {
            RootLakeColumnInfo info;
            info.name = col["name"].get<std::string>();
            info.type = ParseColumnType(col["type"].get<std::string>());
            result.columns.push_back(std::move(info));
        }

        std::vector<uint8_t> binary_tail(
            (std::istreambuf_iterator<char>(in)),
            std::istreambuf_iterator<char>()
        );
        in.close();

        result.baskets = ParseBinaryTail(binary_tail);
        return result;
    }
};

// ============================================================
// TypedBufferPool (для чтения сложных объектов)
// ============================================================
class TypedBufferPool
{
    std::vector<int32_t> int32_buf_;
    std::vector<float> float_buf_;
    std::vector<double> double_buf_;
    std::vector<int64_t> int64_buf_;

public:
    void Resize(size_t column_count)
    {
        int32_buf_.assign(column_count, 0);
        float_buf_.assign(column_count, 0.0f);
        double_buf_.assign(column_count, 0.0);
        int64_buf_.assign(column_count, 0);
    }

    [[nodiscard]] void* GetBufferPtr(size_t col_idx, MetaColumnType type)
    {
        switch (type)
        {
            case MetaColumnType::INT32: return &int32_buf_[col_idx];
            case MetaColumnType::FLOAT: return &float_buf_[col_idx];
            case MetaColumnType::DOUBLE: return &double_buf_[col_idx];
            case MetaColumnType::INT64: return &int64_buf_[col_idx];
            default: return nullptr;
        }
    }

    [[nodiscard]] double GetValueAsDouble(size_t col_idx, MetaColumnType type) const
    {
        switch (type)
        {
            case MetaColumnType::INT32: return static_cast<double>(int32_buf_[col_idx]);
            case MetaColumnType::FLOAT: return static_cast<double>(float_buf_[col_idx]);
            case MetaColumnType::DOUBLE: return double_buf_[col_idx];
            case MetaColumnType::INT64: return static_cast<double>(int64_buf_[col_idx]);
            default: return 0.0;
        }
    }

    void CopyToOutput(size_t col_idx, MetaColumnType type, idx_t out_idx, Vector& output_vec) const
    {
        switch (type)
        {
            case MetaColumnType::INT32: FlatVector::GetData<int32_t>(output_vec)[out_idx] = int32_buf_[col_idx]; break;
            case MetaColumnType::FLOAT: FlatVector::GetData<float>(output_vec)[out_idx] = float_buf_[col_idx]; break;
            case MetaColumnType::DOUBLE: FlatVector::GetData<double>(output_vec)[out_idx] = double_buf_[col_idx]; break;
            case MetaColumnType::INT64: FlatVector::GetData<int64_t>(output_vec)[out_idx] = int64_buf_[col_idx]; break;
            default: break;
        }
    }
};

// ============================================================
// RootLakeFilterEngine (для pushdown фильтров)
// Do not call this duckdb::FilterEngine: DuckDB has an internal class with that name.
// ============================================================
class WorkScheduler
{
    uint64_t& next_row_;
    uint64_t total_rows_;
    std::mutex& mutex_;

public:
    struct WorkBatch
    {
        uint64_t start;
        uint64_t end;
        [[nodiscard]] bool HasWork() const { return start < end; }
        [[nodiscard]] uint64_t Size() const { return end - start; }
    };

    WorkScheduler(uint64_t& next_row, uint64_t total_rows, std::mutex& mtx)
        : next_row_(next_row), total_rows_(total_rows), mutex_(mtx) {}

    WorkBatch ClaimWork(uint64_t preferred_batch_size = 100000)
    {
        std::lock_guard<std::mutex> lock(mutex_);

        if (next_row_ >= total_rows_)
        {
            return {0, 0};
        }

        WorkBatch batch;
        batch.start = next_row_;
        batch.end = std::min(next_row_ + preferred_batch_size, total_rows_);

        next_row_ = batch.end;
        return batch;
    }

    [[nodiscard]] static idx_t EstimateOptimalThreads(uint64_t total_rows)
    {
        const uint64_t rows_per_thread = 500000;
        idx_t threads = static_cast<idx_t>(total_rows / rows_per_thread);
        return std::max<idx_t>(1, threads);
    }
};

// ============================================================
// ColumnGroup
// ============================================================
struct ColumnGroup
{
    std::string branch_name;
    std::string index_signature;
    std::vector<idx_t> column_indices;
    bool is_container() const { return !index_signature.empty(); }
};

// ============================================================
// FastRootBindData
// ============================================================
struct FastRootBindData : public TableFunctionData
{
    RootDebugLifetimeSentinel lifetime_sentinel {"FastRootBindData"};
    std::string root_path;
    std::string input_specification;
    std::vector<std::string> root_paths;
    idx_t representative_source_id = 0;
    uint64_t bind_open_us = 0;
    std::string meta_path;
    std::string tree_name;
    uint64_t total_rows = 0;
    std::vector<RootLakeColumnInfo> columns;
    std::vector<BasketMeta> baskets;
    std::vector<ColumnGroup> column_groups;

    bool is_browse_mode = false;
    bool is_direct_branch_mode = false;   // для простых веток без индекса
    bool is_empty_mode = false;           // safe zero-row result; never opens a TTree
    bool external_dictionary_loaded = false;
    DirectDictionaryCleanupMode dictionary_cleanup_mode = DirectDictionaryCleanupMode::FULL;
    std::vector<std::string> browse_children;
    SimpleBranchInfo direct_branch_info;
    rootlake::RootReaderMode reader_mode = rootlake::RootReaderMode::AUTO;
    uint32_t raw_validation_entries = 4;
    uint64_t raw_max_entry_bytes = 64ULL * 1024ULL * 1024ULL;
    uint64_t raw_max_values_per_entry = 10ULL * 1024ULL * 1024ULL;
    uint64_t tree_cache_bytes = 64ULL * 1024ULL * 1024ULL;
    idx_t source_id_column = DConstants::INVALID_INDEX;
    idx_t source_path_column = DConstants::INVALID_INDEX;

    bool IsMultiFile() const { return root_paths.size() > 1; }

    ~FastRootBindData()
    {
        RootDebug("BIND_DATA.DTOR_BODY",
                  "this=" + RootPointer(this) +
                  " root_path=" + root_path +
                  " columns=" + std::to_string(columns.size()));
    }
};

// ============================================================
// FastRootGlobalState
// ============================================================
struct FastRootGlobalState : public GlobalTableFunctionState
{
    uint64_t next_row = 0;
    uint64_t total_rows = 0;
    uint64_t scheduled_rows = 0;
    size_t browse_offset = 0;
    std::mutex coordination_mutex;
    unique_ptr<TableFilterSet> filters;
    std::vector<idx_t> scan_column_ids;
    std::vector<idx_t> output_column_ids;
    std::atomic<uint64_t> serialized_entries {0};
    std::atomic<uint64_t> serialized_values {0};
    std::atomic<uint64_t> serialized_baskets {0};
    std::atomic<uint64_t> serialized_compressed_bytes {0};
    std::atomic<uint64_t> serialized_entry_bytes {0};
    std::atomic<uint64_t> object_validation_entries {0};
    std::atomic<uint64_t> object_fallback_entries {0};
    unique_ptr<rootlake::RootDirectFileScheduler> file_scheduler;
    uint64_t event_lower = 0;
    uint64_t event_upper = std::numeric_limits<uint64_t>::max();
    bool event_range_impossible = false;

    idx_t MaxThreads() const override
    {
        if (file_scheduler) return file_scheduler->MaxThreads();
        return WorkScheduler::EstimateOptimalThreads(scheduled_rows);
    }
};

// ============================================================
// ReadResult
// ============================================================
struct ReadResult
{
    std::vector<std::string> strings;
    std::vector<double> numbers;
    std::vector<bool> is_string_flag;
    std::vector<Long64_t> event_ids;
    std::vector<std::vector<int>> vector_indices;
    std::vector<std::string> vector_names;
    std::string source_path;

    size_t size() const { return strings.size(); }
    bool empty() const { return strings.empty(); }
    
    void clear()
    {
        strings.clear(); numbers.clear(); is_string_flag.clear();
        event_ids.clear(); vector_indices.clear(); vector_names.clear();
        source_path.clear();
    }

    void add_string(const std::string& s, Long64_t evt_id,
                    const std::vector<int>& indices,
                    const std::vector<std::string>& vec_names)
    {
        strings.push_back(s);
        numbers.push_back(0);
        is_string_flag.push_back(true);
        event_ids.push_back(evt_id);
        vector_indices.push_back(indices);
        if (vector_names.empty()) vector_names = vec_names;
    }
    
    void add_number(double v, Long64_t evt_id,
                    const std::vector<int>& indices,
                    const std::vector<std::string>& vec_names)
    {
        strings.emplace_back();
        numbers.push_back(v);
        is_string_flag.push_back(false);
        event_ids.push_back(evt_id);
        vector_indices.push_back(indices);
        if (vector_names.empty()) vector_names = vec_names;
    }
};

// ============================================================
// FastRootLocalState
// ============================================================
struct FastRootLocalState : public LocalTableFunctionState
{
    rootlake::RootFileHandle root_file;
    TypedBufferPool buffer_pool;

    std::unordered_map<std::string, ObjectContext> root_contexts;

    uint64_t local_current_row = 0;
    uint64_t local_end_row = 0;

    uint64_t current_entry = std::numeric_limits<uint64_t>::max();
    size_t current_elem_idx = 0;
    std::vector<ReadResult> cached_results;
    bool has_cached_entry = false;
    
    bool has_container_columns = false;
    rootlake::RootFilterEvaluator filter_evaluator;

    rootlake::SerializedReadPlan serialized_plan;
    rootlake::SerializedBasketReader serialized_reader;
    bool serialized_active = false;
    idx_t serialized_column = DConstants::INVALID_INDEX;
    std::string serialized_context;
    uint32_t validation_remaining = 0;
    std::vector<double> serialized_values;
    std::vector<int32_t> serialized_indices;
    uint64_t reported_serialized_baskets = 0;
    uint64_t reported_serialized_compressed_bytes = 0;
    uint64_t reported_serialized_entry_bytes = 0;

    // Для режима прямой работы с веткой
    TBranch* direct_branch = nullptr;
    TLeaf* direct_leaf = nullptr;

    bool file_active = false;
    rootlake::RootDirectFileTask file_task;
    std::chrono::steady_clock::time_point file_started;

    ~FastRootLocalState() = default;
};

// ============================================================
// ObjectReader (аналог ValueReader)
// ============================================================
class ObjectReader
{
public:
    static void collect(void* root_ptr,
                        const std::vector<PathLevel>& levels,
                        Long64_t max_values,
                        Long64_t evt_id,
                        ReadResult& out)
    {
        std::vector<int> current_indices;
        std::vector<std::string> vec_names;
        collect_recursive(root_ptr, levels, 0, max_values, evt_id,
                          current_indices, vec_names, out);
    }

private:
    static void append_array_names(const PathLevel& lvl, size_t current_depth,
                                   std::vector<std::string>& vec_names)
    {
        const size_t rank = lvl.array_dimensions.size() <= 1 ? 1 : lvl.array_dimensions.size();
        const size_t target_size = current_depth + rank;
        if (lvl.array_dimensions.size() <= 1)
        {
            if (vec_names.size() < target_size) vec_names.push_back(lvl.name + "_idx");
            return;
        }
        for (size_t dim = 0; dim < lvl.array_dimensions.size(); ++dim)
        {
            if (vec_names.size() < target_size)
            {
                vec_names.push_back(lvl.name + "_dim" + std::to_string(dim) + "_idx");
            }
        }
    }

    static void push_array_coordinates(uint64_t flat, const std::vector<uint32_t>& dimensions,
                                       std::vector<int>& indices)
    {
        if (dimensions.empty())
        {
            indices.push_back(static_cast<int>(flat));
            return;
        }
        std::vector<int> coordinates(dimensions.size(), 0);
        for (size_t reverse = dimensions.size(); reverse > 0; --reverse)
        {
            const size_t dim = reverse - 1;
            coordinates[dim] = static_cast<int>(flat % dimensions[dim]);
            flat /= dimensions[dim];
        }
        indices.insert(indices.end(), coordinates.begin(), coordinates.end());
    }

    struct ContainerAccess
    {
        char* base = nullptr;
        uint32_t stride = 0;
        bool contiguous = false;
    };

    static ContainerAccess prepare_container_access(const PathLevel& lvl,
                                                    TVirtualCollectionProxy* proxy,
                                                    size_t n)
    {
        ContainerAccess access;
        const char* disable = std::getenv("ROOT4DUCKDB_DISABLE_CONTIGUOUS_VECTOR");
        if (disable && *disable && std::string(disable) != "0") return access;
        if (!proxy || n == 0 || !PathResolver::IsContiguousVector(lvl.type)) return access;
        std::string inner = PathResolver::ExtractInnerType(lvl.type);
        if (inner.find("std::") == 0) inner = inner.substr(5);
        if (inner == "bool" || inner == "Bool_t" || PathResolver::IsPointerType(inner)) return access;
        uint32_t stride = PathResolver::PrimitiveSize(inner);
        if (!stride && lvl.element_class) stride = static_cast<uint32_t>(lvl.element_class->Size());
        if (!stride) return access;
        auto* first = static_cast<char*>(proxy->At(0));
        if (!first) return access;
        if (n > 1)
        {
            auto* second = static_cast<char*>(proxy->At(1));
            if (!second || second != first + stride) return access;
        }
        access.base = first;
        access.stride = stride;
        access.contiguous = true;
        RootDebug("VECTOR.CONTIGUOUS", "type=" + lvl.type + " size=" + std::to_string(n) +
                  " stride=" + std::to_string(stride));
        return access;
    }

    static void collect_recursive(void* current_ptr,
                                  const std::vector<PathLevel>& levels,
                                  size_t level_idx,
                                  Long64_t max_values,
                                  Long64_t evt_id,
                                  std::vector<int>& current_indices,
                                  std::vector<std::string>& vec_names,
                                  ReadResult& out)
    {
        if (out.size() >= static_cast<size_t>(max_values) || level_idx >= levels.size() || !current_ptr)
        {
            return;
        }

        const auto& lvl = levels[level_idx];
        char* field_ptr = static_cast<char*>(current_ptr) + lvl.offset_in_parent;

        if (lvl.is_pointer && field_ptr)
        {
            field_ptr = *reinterpret_cast<char**>(field_ptr);
            if (!field_ptr)
            {
                return;
            }
        }

        bool is_last = (level_idx == levels.size() - 1);

        if (lvl.is_fixed_array)
        {
            append_array_names(lvl, current_indices.size(), vec_names);
            const size_t pushed = lvl.array_dimensions.empty() ? 1 : lvl.array_dimensions.size();
            for (uint64_t i = 0; i < lvl.fixed_array_length && out.size() < static_cast<size_t>(max_values); ++i)
            {
                char* elem = field_ptr + i * lvl.element_size;
                push_array_coordinates(i, lvl.array_dimensions, current_indices);
                if (is_last)
                {
                    if (lvl.is_primitive)
                    {
                        out.add_number(read_primitive(elem, lvl.type), evt_id, current_indices, vec_names);
                    }
                }
                else
                {
                    collect_recursive(elem, levels, level_idx + 1, max_values,
                                      evt_id, current_indices, vec_names, out);
                }
                current_indices.resize(current_indices.size() - pushed);
            }
            return;
        }

        if (is_last && lvl.is_container)
        {
            auto* proxy = lvl.klass ? lvl.klass->GetCollectionProxy() : nullptr;
            if (!proxy)
            {
                return;
            }
            TVirtualCollectionProxy::TPushPop guard(proxy, field_ptr);
            size_t n = proxy->Size();
            std::string idx_name = lvl.name + "_idx";
            if (vec_names.empty() || vec_names.size() < current_indices.size() + 1)
            {
                vec_names.push_back(idx_name);
            }
            
            std::string inner = PathResolver::ExtractInnerType(lvl.type);
            std::string inner_clean = inner;
            if (inner_clean.find("std::") == 0)
            {
                inner_clean = inner_clean.substr(5);
            }

            const auto access = prepare_container_access(lvl, proxy, n);
            for (size_t i = 0; i < n && out.size() < static_cast<size_t>(max_values); ++i)
            {
                void* elem = access.contiguous ? static_cast<void*>(access.base + i * access.stride)
                                               : proxy->At(i);
                if (!elem)
                {
                    continue;
                }
                current_indices.push_back(static_cast<int>(i));
                
                if (is_primitive_type(inner_clean))
                {
                    out.add_number(read_primitive(elem, inner_clean), evt_id, current_indices, vec_names);
                }
                else if (inner_clean == "TString")
                {
                    out.add_string(read_tstring(elem), evt_id, current_indices, vec_names);
                }
                else if (inner_clean == "string")
                {
                    out.add_string(read_string(elem), evt_id, current_indices, vec_names);
                }
                current_indices.pop_back();
            }
            return;
        }

        if (is_last)
        {
            if (lvl.is_primitive)
            {
                out.add_number(read_primitive(field_ptr, lvl.type), evt_id, current_indices, vec_names);
            }
            else if (lvl.is_string)
            {
                std::string type_clean = lvl.type;
                if (type_clean.find("std::") == 0)
                {
                    type_clean = type_clean.substr(5);
                }
                
                if (type_clean == "TString")
                {
                    out.add_string(read_tstring(field_ptr), evt_id, current_indices, vec_names);
                }
                else
                {
                    out.add_string(read_string(field_ptr), evt_id, current_indices, vec_names);
                }
            }
            return;
        }

        if (lvl.is_container)
        {
            if (!lvl.klass || !lvl.klass->HasDictionary())
            {
                return;
            }
            auto* proxy = lvl.klass->GetCollectionProxy();
            if (!proxy)
            {
                return;
            }
            TVirtualCollectionProxy::TPushPop guard(proxy, field_ptr);
            size_t n = proxy->Size();
            if (vec_names.empty() || vec_names.size() < current_indices.size() + 1)
            {
                vec_names.push_back(lvl.name + "_idx");
            }
            const auto access = prepare_container_access(lvl, proxy, n);
            for (size_t i = 0; i < n && out.size() < static_cast<size_t>(max_values); ++i)
            {
                void* elem = access.contiguous ? static_cast<void*>(access.base + i * access.stride)
                                               : proxy->At(i);
                if (!elem)
                {
                    continue;
                }
                current_indices.push_back(static_cast<int>(i));
                collect_recursive(elem, levels, level_idx + 1, max_values,
                                  evt_id, current_indices, vec_names, out);
                current_indices.pop_back();
            }
            return;
        }

        if (lvl.klass)
        {
            collect_recursive(field_ptr, levels, level_idx + 1, max_values,
                              evt_id, current_indices, vec_names, out);
        }
    }

    static double read_primitive(void* ptr, const std::string& type)
    {
        if (!ptr)
        {
            return 0;
        }
        
        std::string t = type;
        size_t first = t.find_first_not_of(" \t");
        if (first != std::string::npos) t = t.substr(first);
        if (t.find("const ") == 0) t = t.substr(6);
        if (t.find("std::") == 0) t = t.substr(5);
        const auto bracket = t.find('[');
        if (bracket != std::string::npos)
        {
            t = t.substr(0, bracket);
        }
        
        if (t == "Float_t" || t == "float") return *reinterpret_cast<float*>(ptr);
        if (t == "Double_t" || t == "double") return *reinterpret_cast<double*>(ptr);
        if (t == "Int_t" || t == "int") return *reinterpret_cast<int*>(ptr);
        if (t == "Long64_t" || t == "long long") return *reinterpret_cast<long long*>(ptr);
        if (t == "Bool_t" || t == "bool") return (*reinterpret_cast<bool*>(ptr)) ? 1.0 : 0.0;
        if (t == "Char_t" || t == "char") return static_cast<double>(*reinterpret_cast<char*>(ptr));
        if (t == "UChar_t" || t == "unsigned char") return static_cast<double>(*reinterpret_cast<unsigned char*>(ptr));
        if (t == "Short_t" || t == "short") return *reinterpret_cast<short*>(ptr);
        if (t == "UShort_t" || t == "unsigned short") return *reinterpret_cast<unsigned short*>(ptr);
        if (t == "UInt_t" || t == "unsigned int") return *reinterpret_cast<unsigned int*>(ptr);
        if (t == "ULong_t" || t == "unsigned long") return *reinterpret_cast<unsigned long*>(ptr);
        
        return 0;
    }

    static std::string read_string(void* ptr)
    {
        if (!ptr)
        {
            return "";
        }
        try
        {
            return *reinterpret_cast<std::string*>(ptr);
        }
        catch (...)
        {
            return "<string_error>";
        }
    }

    static std::string read_tstring(void* ptr)
    {
        if (!ptr)
        {
            return "";
        }
        try
        {
            TString* ts = static_cast<TString*>(ptr);
            return std::string(ts->Data(), ts->Length());
        }
        catch (...)
        {
            return "<tstring_error>";
        }
    }

    static bool is_primitive_type(const std::string& type)
    {
        std::string t = type;
        if (t.find("std::") == 0)
        {
            t = t.substr(5);
        }
        
        static const std::vector<std::string> prims = {
            "Bool_t", "bool", "Char_t", "char", "UChar_t", "unsigned char",
            "Short_t", "short", "UShort_t", "unsigned short",
            "Int_t", "int", "UInt_t", "unsigned int",
            "Long_t", "long", "ULong_t", "unsigned long",
            "Long64_t", "long long", "ULong64_t", "unsigned long long",
            "Float_t", "float", "Double_t", "double"
        };
        return std::find(prims.begin(), prims.end(), t) != prims.end();
    }
};

// ============================================================
// Функции привязки (Bind*)
// ============================================================
static void AddEventIdColumn(
    FastRootBindData& bind_data,
    std::vector<std::string>& return_names,
    std::vector<LogicalType>& return_types)
{
    return_names.emplace_back("event_id");
    return_types.emplace_back(LogicalType(LogicalTypeId::BIGINT));
    
    RootLakeColumnInfo col;
    col.name = "event_id";
    col.type = MetaColumnType::INT64;
    bind_data.columns.emplace_back(std::move(col));
}

static void BindDirectPrimitives(
    FastRootBindData& bind_data,
    const std::string& path_prefix,
    const std::vector<std::string>& matching_paths,
    std::vector<std::string>& return_names,
    std::vector<LogicalType>& return_types)
{
    RootDebug("BIND.PRIMITIVES_BEGIN",
              "prefix=" + path_prefix +
              " matching_paths=" + std::to_string(matching_paths.size()));
    struct ResolvedColumn
    {
        RootLakeColumnInfo column;
        LogicalType duckdb_type;
    };

    std::vector<ResolvedColumn> resolved_columns;
    std::set<std::string> seen_value_names;
    std::vector<std::string> ordered_index_names;
    std::set<std::string> seen_index_names;

    for (const auto& full_path : matching_paths)
    {
        RootDebug("BIND.PRIMITIVE_PATH", "path=" + full_path);
        std::string rest = full_path.size() < path_prefix.size()
            ? std::string()
            : full_path.substr(path_prefix.size());
        if (rest.find('/') != std::string::npos)
        {
            continue;
        }

        std::string flat_name = rest;
        if (flat_name.empty())
        {
            std::string clean_prefix = path_prefix;
            if (!clean_prefix.empty() && clean_prefix.back() == '/')
            {
                clean_prefix.pop_back();
            }
            if (clean_prefix.size() >= 6 && clean_prefix.substr(clean_prefix.size() - 6) == "/value")
            {
                clean_prefix.resize(clean_prefix.size() - 6);
            }
            const size_t last_slash = clean_prefix.find_last_of('/');
            flat_name = last_slash == std::string::npos
                ? clean_prefix
                : clean_prefix.substr(last_slash + 1);
        }
        for (char& c : flat_name)
        {
            if (c == '/') c = '_';
        }

        std::string lower_name = flat_name;
        std::transform(lower_name.begin(), lower_name.end(), lower_name.begin(), ::tolower);
        if (!seen_value_names.insert(lower_name).second)
        {
            continue;
        }

        const auto parsed = PathParser::Parse(full_path);
        if (parsed.fields.empty())
        {
            continue;
        }
        RootDebug("TCLASS.BEFORE",
                  "GetClass name=" + parsed.root_class + " source=" + full_path);
        auto* cls = TClass::GetClass(parsed.root_class.c_str());
        RootDebug("TCLASS.AFTER",
                  "name=" + parsed.root_class + " ptr=" + RootPointer(cls) +
                  " has_dictionary=" + std::to_string(cls && cls->HasDictionary() ? 1 : 0));
        if (!cls || !cls->HasDictionary())
        {
            continue;
        }
        auto levels = PathResolver::resolve(cls, parsed.fields);
        if (levels.empty())
        {
            continue;
        }
        const auto& leaf = levels.back();
        if (!leaf.is_primitive && !leaf.is_string)
        {
            continue;
        }

        RootLakeColumnInfo col;
        col.name = flat_name;
        col.logical_path = full_path;
        col.type = MetaColumnType::UNKNOWN;
        col.branch_name = parsed.root_class;
        col.root_type = leaf.type;
        col.is_string = leaf.is_string;
        col.levels = std::move(levels);
        col.index_signature = BuildIndexSignature(col.levels);

        if (!col.index_signature.empty())
        {
            std::stringstream signature(col.index_signature);
            std::string index_name;
            while (std::getline(signature, index_name, ','))
            {
                std::string lower_index = index_name;
                std::transform(lower_index.begin(), lower_index.end(), lower_index.begin(), ::tolower);
                if (seen_index_names.insert(lower_index).second)
                {
                    ordered_index_names.push_back(index_name);
                }
            }
        }

        ResolvedColumn resolved;
        resolved.duckdb_type = RootTypeToDuckDB(col.root_type, col.is_string, true);
        resolved.column = std::move(col);
        RootDebug("BIND.COLUMN_READY",
                  "name=" + resolved.column.name +
                  " root_type=" + resolved.column.root_type +
                  " signature=" + resolved.column.index_signature);
        resolved_columns.emplace_back(std::move(resolved));
    }

    AddEventIdColumn(bind_data, return_names, return_types);

    const std::string root_class_name = PathParser::Parse(path_prefix).root_class;
    for (const auto& index_name : ordered_index_names)
    {
        RootLakeColumnInfo index_column;
        index_column.name = index_name;
        index_column.type = MetaColumnType::INT32;
        index_column.is_virtual_index = true;
        index_column.branch_name = root_class_name;
        bind_data.columns.emplace_back(std::move(index_column));
        return_names.emplace_back(index_name);
        return_types.emplace_back(LogicalType(LogicalTypeId::INTEGER));
    }

    for (auto& resolved : resolved_columns)
    {
        return_names.emplace_back(resolved.column.name);
        return_types.emplace_back(resolved.duckdb_type);
        bind_data.columns.emplace_back(std::move(resolved.column));
    }

    RootDebug("BIND.PRIMITIVES_END",
              "columns=" + std::to_string(bind_data.columns.size()) +
              " return_names=" + std::to_string(return_names.size()));
}

static void BindBrowseMode(
    FastRootBindData& bind_data,
    const std::string& path_prefix,
    const std::set<std::string>& direct_children,
    std::vector<std::string>& return_names,
    std::vector<LogicalType>& return_types)
{
    bind_data.is_browse_mode = true;
    return_names.emplace_back("path");
    return_types.emplace_back(LogicalType(LogicalTypeId::VARCHAR));

    for (const auto& child_full_path : direct_children)
    {
        std::string folder_name = child_full_path.substr(path_prefix.size());
        if (!folder_name.empty() && folder_name.back() == '/')
        {
            folder_name.pop_back();
        }
        if (folder_name.empty())
        {
            continue;
        }
        bind_data.browse_children.push_back(folder_name);
    }
}

static void BindEmptyResult(
    FastRootBindData& bind_data,
    const std::string& path_prefix,
    std::vector<std::string>& return_names,
    std::vector<LogicalType>& return_types)
{
    bind_data.is_empty_mode = true;
    bind_data.total_rows = 0;
    AddEventIdColumn(bind_data, return_names, return_types);
    if (!path_prefix.empty())
    {
        std::string clean_prefix = path_prefix;
        if (clean_prefix.back() == '/')
        {
            clean_prefix.pop_back();
        }
        size_t last_slash = clean_prefix.find_last_of('/');
        std::string container_name = (last_slash == std::string::npos) ? clean_prefix : clean_prefix.substr(last_slash + 1);

        if (container_name.find("vec") == 0 || container_name.find("set") == 0)
        {
            std::string idx_name = container_name + "_idx";
            RootLakeColumnInfo idx_col;
            idx_col.name = idx_name;
            idx_col.type = MetaColumnType::INT32;
            idx_col.is_virtual_index = true;
            idx_col.branch_name = PathParser::Parse(path_prefix).root_class;
            bind_data.columns.emplace_back(std::move(idx_col));
            return_names.emplace_back(idx_name);
            return_types.emplace_back(LogicalType(LogicalTypeId::INTEGER));
        }
    }
}

// Load a user dictionary before *any* TFile/TClass inspection.  The old
// implementation loaded it only in the metadata-index branch, which meant
// browse/direct-path queries inspected PaEvent/PaSetup without their classes.
static bool LoadRequestedDictionary(ClientContext& context, TableFunctionBindInput& input)
{
    auto it = input.named_parameters.find("dictionary");
    if (it == input.named_parameters.end())
    {
        RootDebug("DICT.NONE", "dictionary parameter was not supplied");
        return false;
    }
    const std::string dict_path = it->second.ToString();
    if (dict_path.empty())
    {
        RootDebug("DICT.EMPTY", "dictionary parameter is empty");
        return false;
    }
    RootDebug("DICT.REQUEST", "path=" + dict_path);
    auto& fs = FileSystem::GetFileSystem(context);
    const bool exists = fs.FileExists(dict_path);
    RootDebug("DICT.FILE_CHECK",
              "path=" + dict_path + " exists=" + std::to_string(exists ? 1 : 0));
    if (!exists)
    {
        throw IOException("Dictionary file not found: " + dict_path);
    }
    static std::mutex dictionary_mutex;
    RootDebug("DICT.MUTEX_WAIT", "path=" + dict_path);
    std::lock_guard<std::mutex> lock(dictionary_mutex);
    RootDebug("DICT.BEFORE_LOAD",
              "path=" + dict_path + " gSystem_ptr=" + RootPointer(gSystem));
    const Long64_t result = gSystem->Load(dict_path.c_str());
    RootDebug("DICT.AFTER_LOAD",
              "path=" + dict_path + " result=" + std::to_string(result));
    if (result < 0)
    {
        throw IOException("Failed to load ROOT dictionary: " + dict_path);
    }
    return true;
}

static DirectDictionaryCleanupMode ResolveDictionaryCleanupMode(const TableFunctionBindInput& input,
                                                                       bool dictionary_loaded)
{
    auto it = input.named_parameters.find("dictionary_cleanup");
    std::string mode = it == input.named_parameters.end() ? "auto" : it->second.ToString();
    std::transform(mode.begin(), mode.end(), mode.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (mode.empty() || mode == "auto") {
        return dictionary_loaded ? DirectDictionaryCleanupMode::RETAIN
                                 : DirectDictionaryCleanupMode::FULL;
    }
    if (mode == "retain" || mode == "none" || mode == "skip") {
        return DirectDictionaryCleanupMode::RETAIN;
    }
    if (mode == "destruct_only" || mode == "dtor_only") {
        return DirectDictionaryCleanupMode::DESTRUCT_ONLY;
    }
    if (mode == "full" || mode == "strict" || mode == "delete") {
        return DirectDictionaryCleanupMode::FULL;
    }
    throw InvalidInputException("dictionary_cleanup must be one of: auto, retain, destruct_only, full");
}

static bool BindSemanticPathWithoutMetadata(
    FastRootBindData& bind_data,
    TFile* file,
    const std::string& path_prefix_raw,
    const std::string& normalized_prefix,
    std::vector<std::string>& return_names,
    std::vector<LogicalType>& return_types)
{
    (void)normalized_prefix;
    RootDebug("SEMANTIC.BEGIN",
              "path=" + path_prefix_raw + " file_ptr=" + RootPointer(file));

    const auto parsed = PathParser::Parse(path_prefix_raw);
    RootDebug("SEMANTIC.PARSED",
              "root_class=" + parsed.root_class +
              " fields=" + JoinDebugFields(parsed.fields));
    if (parsed.root_class.empty())
    {
        return false;
    }

    RootDebug("TCLASS.BEFORE", "GetClass name=" + parsed.root_class + " semantic_bind=1");
    auto* root_class = TClass::GetClass(parsed.root_class.c_str());
    RootDebug("TCLASS.AFTER",
              "name=" + parsed.root_class + " ptr=" + RootPointer(root_class) +
              " has_dictionary=" +
                  std::to_string(root_class && root_class->HasDictionary() ? 1 : 0));
    if (!root_class || !root_class->HasDictionary())
    {
        return false;
    }
    RootDebug("TREE.BEFORE_FIND", "class=" + parsed.root_class);
    auto* tree = find_tree(file, parsed.root_class);
    RootDebug("TREE.AFTER_FIND",
              "class=" + parsed.root_class + " tree_ptr=" + RootPointer(tree) +
              " tree_name=" + std::string(tree ? tree->GetName() : "<null>"));
    auto* branch = find_branch(tree, parsed.root_class);
    RootDebug("BRANCH.AFTER_FIND",
              "class=" + parsed.root_class + " branch_ptr=" + RootPointer(branch) +
              " branch_name=" + std::string(branch ? branch->GetName() : "<null>"));
    if (!tree || !branch)
    {
        return false;
    }

    std::string bind_prefix;
    std::vector<std::string> primitive_paths;
    std::set<std::string> direct_children;
    RootDebug("SEMANTIC.BEFORE_SELECT", "path=" + path_prefix_raw);
    if (!SelectSemanticPathDirectly(
            root_class, parsed, path_prefix_raw, bind_prefix,
            primitive_paths, direct_children))
    {
        return false;
    }

    RootDebug("SEMANTIC.AFTER_SELECT",
              "bind_prefix=" + bind_prefix +
              " primitive_paths=" + std::to_string(primitive_paths.size()) +
              " direct_children=" + std::to_string(direct_children.size()));
    bind_data.tree_name = tree->GetName();
    bind_data.total_rows = static_cast<uint64_t>(tree->GetEntries());

    if (!primitive_paths.empty())
    {
        BindDirectPrimitives(
            bind_data, bind_prefix, primitive_paths, return_names, return_types);
        if (bind_data.columns.size() > 1)
        {
            RootDebug("SEMANTIC.SUCCESS",
                      "mode=primitive columns=" + std::to_string(bind_data.columns.size()));
            return true;
        }

        // The exact semantic path existed but its terminal type is not yet a
        // readable SQL scalar.  Do not fall back to global schema discovery.
        bind_data.columns.clear();
        return_names.clear();
        return_types.clear();
        return false;
    }

    if (!direct_children.empty())
    {
        BindBrowseMode(
            bind_data, bind_prefix, direct_children, return_names, return_types);
        RootDebug("SEMANTIC.SUCCESS",
                  "mode=browse children=" + std::to_string(direct_children.size()));
        return true;
    }
    return false;
}

// ============================================================
// RootScanBind
// ============================================================
static std::unique_ptr<TFile> OpenRepresentativeFile(FastRootBindData& bind_data)
{
    std::vector<std::string> failures;
    for (idx_t source_id = 0; source_id < bind_data.root_paths.size(); ++source_id)
    {
        RootDebug("FILE.BEFORE_OPEN",
                  "mode=representative source_id=" + std::to_string(source_id) +
                  " path=" + bind_data.root_paths[source_id]);
        auto result = rootlake::OpenRootFile(bind_data.root_paths[source_id]);
        bind_data.bind_open_us += result.elapsed_us;
        RootDebug("FILE.AFTER_OPEN",
                  "mode=representative source_id=" + std::to_string(source_id) +
                  " attempts=" + std::to_string(result.attempts) +
                  " elapsed_us=" + std::to_string(result.elapsed_us) +
                  " zombie=" + std::to_string(result.file && result.file->IsZombie() ? 1 : 0));
        if (result)
        {
            bind_data.representative_source_id = source_id;
            bind_data.root_path = bind_data.root_paths[source_id];
            return std::move(result.file);
        }
        failures.push_back(bind_data.root_paths[source_id] + ": " + result.error);
    }
    std::ostringstream message;
    message << "Failed to open every ROOT input while selecting a representative file";
    const auto limit = std::min<size_t>(failures.size(), 4);
    for (size_t index = 0; index < limit; ++index) message << "; " << failures[index];
    if (failures.size() > limit) message << "; ...";
    throw IOException(message.str());
}

static void AddMultiFileIdentityColumns(FastRootBindData& bind_data,
                                        vector<string>& return_names,
                                        vector<LogicalType>& return_types)
{
    if (!bind_data.IsMultiFile() || bind_data.is_browse_mode ||
        bind_data.source_id_column != DConstants::INVALID_INDEX) return;

    bind_data.source_id_column = bind_data.columns.size();
    RootLakeColumnInfo source_id;
    source_id.name = "source_id";
    source_id.root_type = "ULong64_t";
    bind_data.columns.push_back(std::move(source_id));
    return_names.emplace_back("source_id");
    return_types.emplace_back(LogicalTypeId::UBIGINT);

    bind_data.source_path_column = bind_data.columns.size();
    RootLakeColumnInfo source_path;
    source_path.name = "source_path";
    source_path.root_type = "string";
    source_path.is_string = true;
    bind_data.columns.push_back(std::move(source_path));
    return_names.emplace_back("source_path");
    return_types.emplace_back(LogicalTypeId::VARCHAR);
}

unique_ptr<FunctionData> RootScanBind(
    ClientContext& context,
    TableFunctionBindInput& input,
    vector<LogicalType>& return_types,
    vector<string>& return_names)
{
    RootDebugOperationScope debug_operation("RootScanBind");
    auto bind_data = make_uniq<FastRootBindData>();

    bind_data->input_specification = input.inputs[0].ToString();
    bind_data->root_paths = rootlake::ResolveRootInputs(context, bind_data->input_specification);
    bind_data->root_path = bind_data->root_paths.front();
    auto reader_mode = input.named_parameters.find("reader_mode");
    if (reader_mode != input.named_parameters.end()) {
        bind_data->reader_mode = rootlake::ParseRootReaderMode(reader_mode->second.ToString());
    }
    auto raw_validation = input.named_parameters.find("raw_validation_entries");
    if (raw_validation != input.named_parameters.end()) {
        bind_data->raw_validation_entries = raw_validation->second.GetValue<uint32_t>();
    }
    auto raw_entry_limit = input.named_parameters.find("raw_max_entry_bytes");
    if (raw_entry_limit != input.named_parameters.end()) {
        bind_data->raw_max_entry_bytes = raw_entry_limit->second.GetValue<uint64_t>();
    }
    auto raw_value_limit = input.named_parameters.find("raw_max_values_per_entry");
    if (raw_value_limit != input.named_parameters.end()) {
        bind_data->raw_max_values_per_entry = raw_value_limit->second.GetValue<uint64_t>();
    }
    auto tree_cache = input.named_parameters.find("tree_cache_bytes");
    if (tree_cache != input.named_parameters.end()) {
        bind_data->tree_cache_bytes = tree_cache->second.GetValue<uint64_t>();
    }
    if (bind_data->raw_max_entry_bytes < 12) {
        throw InvalidInputException("raw_max_entry_bytes must be at least 12");
    }
    if (bind_data->raw_max_values_per_entry == 0) {
        throw InvalidInputException("raw_max_values_per_entry must be positive");
    }
    RootDebug("BIND.BEGIN",
              "root_input=" + bind_data->input_specification +
              " resolved_files=" + std::to_string(bind_data->root_paths.size()) +
              " inputs=" + std::to_string(input.inputs.size()) +
              " named_parameters=" + std::to_string(input.named_parameters.size()));
    bind_data->meta_path = bind_data->IsMultiFile() ? std::string() : bind_data->root_path + ".json";
    if (!bind_data->IsMultiFile() && input.inputs.size() > 1)
    {
        auto& fs = FileSystem::GetFileSystem(context);
        bind_data->meta_path = input.inputs[1].ToString() + "/" + fs.ExtractBaseName(bind_data->root_path) + ".json";
    }

    const bool dictionary_loaded = LoadRequestedDictionary(context, input);
    bind_data->external_dictionary_loaded = dictionary_loaded;
    bind_data->dictionary_cleanup_mode =
        ResolveDictionaryCleanupMode(input, dictionary_loaded);
    RootDebug("BIND.DICTIONARY_DONE",
              "loaded=" + std::to_string(dictionary_loaded ? 1 : 0));

    bool use_path_prefix = input.named_parameters.find("path_prefix") != input.named_parameters.end();
    if (!use_path_prefix)
    {
        // Без path_prefix — browse mode на корневом уровне (список веток)
        RootDebug("FILE.BEFORE_OPEN", "mode=root_browse path=" + bind_data->root_path);
        auto file = OpenRepresentativeFile(*bind_data);
        RootDebug("FILE.AFTER_OPEN",
                  "mode=root_browse file_ptr=" + RootPointer(file.get()) +
                  " zombie=" + std::to_string(file && file->IsZombie() ? 1 : 0));
        if (!file || file->IsZombie())
        {
            throw IOException("Failed to open ROOT file: " + bind_data->root_path);
        }
        TTree* tree = find_tree(file.get(), ""); // первое дерево
        if (!tree)
        {
            throw IOException("No TTree found in ROOT file.");
        }
        auto* branches = tree->GetListOfBranches();
        std::vector<std::string> root_paths;
        for (int i = 0; i < branches->GetEntries(); ++i)
        {
            auto* be = dynamic_cast<TBranchElement*>(branches->At(i));
            if (be && be->GetClassName())
            {
                root_paths.push_back("/" + std::string(be->GetClassName()));
            }
            else
            {
                auto* br = dynamic_cast<TBranch*>(branches->At(i));
                if (br)
                {
                    root_paths.push_back("/" + std::string(br->GetName()));
                }
            }
        }
        std::sort(root_paths.begin(), root_paths.end());
        root_paths.erase(std::unique(root_paths.begin(), root_paths.end()), root_paths.end());

        bind_data->is_browse_mode = true;
        bind_data->browse_children = root_paths;
        return_names.emplace_back("path");
        return_types.emplace_back(LogicalType(LogicalTypeId::VARCHAR));
        RootDebug("FILE.BEFORE_CLOSE", "mode=root_browse file_ptr=" + RootPointer(file.get()));
        file.reset();
        RootDebug("FILE.AFTER_CLOSE", "mode=root_browse");
        RootDebug("BIND.RETURN", "mode=root_browse");
        return std::move(bind_data);
    }

    // --- Далее обработка с path_prefix ---
    std::string path_prefix_raw = input.named_parameters["path_prefix"].ToString();
    RootDebug("BIND.PATH", "path_prefix=" + path_prefix_raw);
    const auto requested_path = PathParser::Parse(path_prefix_raw);
    if (!requested_path.fields.empty() && !dictionary_loaded)
    {
        RootDebug("BIND.NO_DICTIONARY",
                  "semantic_path=" + path_prefix_raw + " refusing unsafe emulated-class bind");
        throw InvalidInputException(
            "Semantic ROOT path '" + path_prefix_raw +
            "' requires dictionary := '/path/to/libDictionary.so'. "
            "Binding complex classes from embedded StreamerInfo without a runtime dictionary is disabled because ROOT may construct unsafe emulated classes.");
    }
    std::string path_prefix = path_prefix_raw;
    if (!path_prefix.empty() && path_prefix.back() == '/')
    {
        path_prefix.pop_back();
    }
    // Определяем, является ли путь контейнером или классом (добавляем /)
    size_t last_slash = path_prefix.find_last_of('/');
    std::string last_part = (last_slash == std::string::npos) 
        ? path_prefix : path_prefix.substr(last_slash + 1);
    if (last_part.find("vec") == 0 || 
        last_part.find("set") == 0 || 
        last_part.find("list") == 0 ||
        (!last_part.empty() && std::isupper(last_part[0])))
    {
        path_prefix += '/';
    }

    // Проверяем наличие индекса
    auto& fs = FileSystem::GetFileSystem(context);
    bool index_exists = !bind_data->meta_path.empty() && fs.FileExists(bind_data->meta_path);

    if (!index_exists)
    {
        // Работаем без индекса: ищем простую ветку по имени
        RootDebug("FILE.BEFORE_OPEN", "mode=path_bind path=" + bind_data->root_path);
        auto file = OpenRepresentativeFile(*bind_data);
        RootDebug("FILE.AFTER_OPEN",
                  "mode=path_bind file_ptr=" + RootPointer(file.get()) +
                  " zombie=" + std::to_string(file && file->IsZombie() ? 1 : 0));
        if (!file || file->IsZombie())
        {
            throw IOException("Failed to open ROOT file: " + bind_data->root_path);
        }
        TTree* tree = find_tree(file.get(), ""); 
        if (!tree)
        {
            throw IOException("No TTree found in ROOT file.");
        }

        // Complex semantic paths can be bound directly from StreamerInfo when
        // the user dictionary is available; no legacy .root.json is required.
        RootDebug("BIND.BEFORE_SEMANTIC", "path=" + path_prefix_raw);
        if (BindSemanticPathWithoutMetadata(*bind_data, file.get(), path_prefix_raw, path_prefix,
                                            return_names, return_types))
        {
            AddMultiFileIdentityColumns(*bind_data, return_names, return_types);
            RootDebug("BIND.AFTER_SEMANTIC", "result=success path=" + path_prefix_raw);
            RootDebug("FILE.BEFORE_CLOSE", "mode=semantic_success file_ptr=" + RootPointer(file.get()));
            file.reset();
            RootDebug("FILE.AFTER_CLOSE", "mode=semantic_success");
            RootDebug("BIND.RETURN", "mode=semantic path=" + path_prefix_raw);
            return std::move(bind_data);
        }
        RootDebug("BIND.AFTER_SEMANTIC", "result=not_bound path=" + path_prefix_raw);

        // Собираем все простые ветки
        std::vector<SimpleBranchInfo> simple_branches;
        auto* branches = tree->GetListOfBranches();
        for (int i = 0; i < branches->GetEntries(); ++i)
        {
            auto* br = dynamic_cast<TBranch*>(branches->At(i));
            if (!br) continue;
            if (dynamic_cast<TBranchElement*>(br)) continue;
            TLeaf* leaf = br->GetLeaf(br->GetName());
            if (!leaf) continue;
            SimpleBranchInfo info;
            info.name = br->GetName();
            info.type_name = leaf->GetTypeName();
            info.branch = br;
            info.leaf = leaf;
            simple_branches.push_back(std::move(info));
        }

        // Ищем совпадение с path_prefix (убираем ведущий /)
        std::string target_name = path_prefix_raw;
        if (!target_name.empty() && target_name[0] == '/') target_name = target_name.substr(1);
        // Также может быть путь вида /Events (имя дерева) — игнорируем, т.к. это дерево, а не ветка

        // Проверяем, не является ли target_name именем дерева
        bool is_tree_name = false;
        TIter next(file->GetListOfKeys());
        TKey* key;
        while ((key = dynamic_cast<TKey*>(next())))
        {
            if (std::string(key->GetClassName()) == "TTree" && std::string(key->GetName()) == target_name)
            {
                is_tree_name = true;
                break;
            }
        }
        if (is_tree_name)
        {
            // Это имя дерева — выдаём список его веток в browse mode
            bind_data->is_browse_mode = true;
            for (const auto& info : simple_branches)
            {
                bind_data->browse_children.push_back("/" + info.name);
            }
            return_names.emplace_back("path");
            return_types.emplace_back(LogicalType(LogicalTypeId::VARCHAR));
            RootDebug("FILE.BEFORE_CLOSE", "mode=tree_browse file_ptr=" + RootPointer(file.get()));
            file.reset();
            RootDebug("FILE.AFTER_CLOSE", "mode=tree_browse");
            RootDebug("BIND.RETURN", "mode=tree_browse");
            return std::move(bind_data);
        }

        // Ищем ветку с таким именем
        for (const auto& info : simple_branches)
        {
            if (info.name == target_name)
            {
                // Нашли прямую ветку — создаём колонку
                bind_data->is_direct_branch_mode = true;
                bind_data->direct_branch_info = info;
                bind_data->tree_name = tree->GetName();
                bind_data->total_rows = tree->GetEntries();

                // Добавляем event_id и саму колонку
                AddEventIdColumn(*bind_data, return_names, return_types);
                RootLakeColumnInfo col;
                col.name = info.name;
                col.type = MetaColumnType::UNKNOWN;
                col.branch_name = info.name;
                col.root_type = info.type_name;
                col.is_string = PathResolver::IsString(info.type_name);
                bind_data->columns.push_back(std::move(col));
                return_names.emplace_back(info.name);
                return_types.emplace_back(RootTypeToDuckDB(info.type_name, false, true));
                AddMultiFileIdentityColumns(*bind_data, return_names, return_types);
                RootDebug("FILE.BEFORE_CLOSE", "mode=direct_branch file_ptr=" + RootPointer(file.get()));
                file.reset();
                RootDebug("FILE.AFTER_CLOSE", "mode=direct_branch");
                RootDebug("BIND.RETURN", "mode=direct_branch name=" + info.name);
                return std::move(bind_data);
            }
        }

        // Если ничего не нашли — пустой результат
        BindEmptyResult(*bind_data, path_prefix_raw, return_names, return_types);
        AddMultiFileIdentityColumns(*bind_data, return_names, return_types);
        RootDebug("FILE.BEFORE_CLOSE", "mode=empty file_ptr=" + RootPointer(file.get()));
        file.reset();
        RootDebug("FILE.AFTER_CLOSE", "mode=empty");
        RootDebug("BIND.RETURN", "mode=empty path=" + path_prefix_raw);
        return std::move(bind_data);
    }

    // ---- Далее идёт стандартная логика с индексом ----
    auto meta = MetadataLoader::Load(bind_data->meta_path);
    bind_data->tree_name = std::move(meta.tree_name);
    bind_data->total_rows = meta.total_rows;
    bind_data->baskets = std::move(meta.baskets);

    // Indexed and non-indexed reads use the same direct TStreamerInfo path
    // resolver.  Basket metadata affects scheduling only; it never changes how
    // a semantic value is bound or materialized.
    {
        rootlake::RootFileHandle inspector;
        inspector.Open(bind_data->root_path, bind_data->tree_name, nullptr);
        if (inspector.IsValid() &&
            BindSemanticPathWithoutMetadata(
                *bind_data, inspector.GetTFile(), path_prefix_raw, path_prefix,
                return_names, return_types))
        {
            AddMultiFileIdentityColumns(*bind_data, return_names, return_types);
            return std::move(bind_data);
        }
    }

    BindEmptyResult(*bind_data, path_prefix_raw, return_names, return_types);
    AddMultiFileIdentityColumns(*bind_data, return_names, return_types);
    return std::move(bind_data);
}

// ============================================================
// RootScanInit
// ============================================================
unique_ptr<GlobalTableFunctionState> RootScanInit(ClientContext& context, TableFunctionInitInput& input)
{
    auto& bind_data = input.bind_data->Cast<FastRootBindData>();
    auto global_state = make_uniq<FastRootGlobalState>();

    global_state->browse_offset = 0;
    if (input.filters) global_state->filters = input.filters->Copy();
    global_state->scan_column_ids = input.column_ids;
    if (input.projection_ids.empty()) {
        global_state->output_column_ids = input.column_ids;
    } else {
        global_state->output_column_ids.reserve(input.projection_ids.size());
        for (const auto projection_id : input.projection_ids) {
            if (projection_id >= input.column_ids.size()) {
                throw InternalException("Invalid read_root projection id");
            }
            global_state->output_column_ids.push_back(input.column_ids[projection_id]);
        }
    }
    global_state->total_rows = bind_data.total_rows;
    global_state->next_row = 0;
    if (bind_data.IsMultiFile() && !bind_data.is_browse_mode && !bind_data.is_empty_mode) {
        const auto runtime = rootlake::RootRuntimeSettings::From(context, bind_data.root_paths.size());
        global_state->file_scheduler = make_uniq<rootlake::RootDirectFileScheduler>(
            bind_data.root_paths, runtime.threads);
    }
    if (!bind_data.is_browse_mode && global_state->filters) {
        for (const auto &entry : global_state->filters->filters) {
            if (entry.first >= global_state->scan_column_ids.size()) continue;
            const auto full_column = global_state->scan_column_ids[entry.first];
            if (full_column == bind_data.source_id_column && global_state->file_scheduler) {
                const auto source_range = rootlake::ExtractRootUnsignedRange(*entry.second);
                if (!source_range.known) continue;
                if (source_range.impossible) {
                    global_state->event_range_impossible = true;
                    break;
                }
                global_state->file_scheduler->SetSourceRange(source_range.lower, source_range.upper);
                continue;
            }
            if (full_column != 0 && full_column != COLUMN_IDENTIFIER_ROW_ID) continue;
            const auto range = rootlake::ExtractRootUnsignedRange(*entry.second);
            if (!range.known) continue;
            if (range.impossible) {
                global_state->event_range_impossible = true;
                if (!bind_data.IsMultiFile()) {
                    global_state->next_row = bind_data.total_rows;
                    global_state->total_rows = bind_data.total_rows;
                }
                break;
            }
            global_state->event_lower = std::max(global_state->event_lower, range.lower);
            if (range.upper != std::numeric_limits<uint64_t>::max()) {
                global_state->event_upper = std::min(global_state->event_upper, range.upper);
            }
            if (!bind_data.IsMultiFile()) {
                global_state->next_row = std::max(global_state->next_row, range.lower);
                if (range.upper != std::numeric_limits<uint64_t>::max()) {
                    global_state->total_rows = std::min(global_state->total_rows, range.upper + 1);
                }
            }
        }
    }
    global_state->scheduled_rows = global_state->total_rows >= global_state->next_row
        ? global_state->total_rows - global_state->next_row : 0;
    return std::move(global_state);
}

// ============================================================
// Per-file local binding. Multi-file scans call this lazily after a worker
// claims a file; the single-file fast path calls it once during local init.
// ============================================================
static void InitializeRootLocalFile(const FastRootBindData& bind_data,
                                    FastRootGlobalState& gstate,
                                    FastRootLocalState& target,
                                    const std::string& file_path,
                                    bool synchronize_open)
{
    RootDebugOperationScope debug_operation("RootScanInitLocal");
    RootDebug("INIT_LOCAL.BEGIN",
              "root_path=" + file_path +
              " tree=" + bind_data.tree_name +
              " columns=" + std::to_string(bind_data.columns.size()));

    auto* local_state = &target;
    auto* open_mutex = synchronize_open ? &gstate.coordination_mutex : nullptr;

    // Режим прямой ветки
    if (bind_data.is_direct_branch_mode)
    {
        const auto open_result = local_state->root_file.Open(file_path, bind_data.tree_name, open_mutex);
        if (gstate.file_scheduler) {
            gstate.file_scheduler->RecordOpen(local_state->file_task,
                                              open_result.attempts, open_result.elapsed_us);
        }
        auto* tree = local_state->root_file.GetTTree();
        if (tree)
        {
            local_state->direct_branch = tree->GetBranch(bind_data.direct_branch_info.name.c_str());
            if (local_state->direct_branch)
            {
                local_state->direct_leaf = local_state->direct_branch->GetLeaf(
                    local_state->direct_branch->GetName());
                std::vector<TBranch *> projected_branches {local_state->direct_branch};
                if (local_state->direct_leaf && local_state->direct_leaf->GetLeafCount() &&
                    local_state->direct_leaf->GetLeafCount()->GetBranch()) {
                    projected_branches.push_back(
                        local_state->direct_leaf->GetLeafCount()->GetBranch());
                }
                const auto projection = rootlake::ApplyBranchProjection(
                    tree, projected_branches, bind_data.tree_cache_bytes);
                if (!projection.applied) {
                    rootlake::EnableAllBranches(tree, bind_data.tree_cache_bytes);
                }
            }
        }
        if (!local_state->direct_branch || !local_state->direct_leaf) {
            throw IOException("ROOT schema mismatch in " + file_path +
                              ": primitive branch '" + bind_data.direct_branch_info.name + "' is absent");
        }
        if (gstate.file_scheduler) {
            const auto entries = static_cast<uint64_t>(std::max<Long64_t>(0, tree->GetEntries()));
            local_state->local_current_row = gstate.event_range_impossible
                ? entries : std::min(entries, gstate.event_lower);
            local_state->local_end_row = entries;
            if (gstate.event_upper != std::numeric_limits<uint64_t>::max()) {
                local_state->local_end_row = std::min(entries, gstate.event_upper + 1);
            }
            local_state->file_active = true;
            gstate.file_scheduler->ObserveSchema("primitive:" + bind_data.direct_branch_info.type_name);
        }
        return;
    }

    // Обычный режим с индексом
    const auto open_result = local_state->root_file.Open(file_path, bind_data.tree_name, open_mutex);
    if (gstate.file_scheduler) {
        gstate.file_scheduler->RecordOpen(local_state->file_task,
                                          open_result.attempts, open_result.elapsed_us);
    }
    local_state->buffer_pool.Resize(bind_data.columns.size());

    auto* file = local_state->root_file.GetTFile();
    if (!file || file->IsZombie())
    {
        throw IOException("Invalid ROOT file: " + file_path);
    }

    std::set<std::string> unique_root_classes;
    for (idx_t col_idx : gstate.scan_column_ids)
    {
        if (col_idx == COLUMN_IDENTIFIER_ROW_ID || col_idx >= bind_data.columns.size())
        {
            continue;
        }
        const auto& col = bind_data.columns[col_idx];
        if (!col.branch_name.empty())
        {
            unique_root_classes.insert(col.branch_name);
        }
    }

    if (unique_root_classes.empty())
    {
        for (const auto& col : bind_data.columns)
        {
            if (!col.branch_name.empty())
            {
                unique_root_classes.insert(col.branch_name);
            }
        }
    }

    for (const auto& root_class_name : unique_root_classes)
    {
        auto* tree = find_tree(file, root_class_name);
        if (!tree)
        {
            if (gstate.file_scheduler) {
                throw IOException("ROOT schema mismatch in " + file_path +
                                  ": tree for class '" + root_class_name + "' is absent");
            }
            continue;
        }

        auto* branch = find_branch(tree, root_class_name);
        if (!branch)
        {
            if (gstate.file_scheduler) {
                throw IOException("ROOT schema mismatch in " + file_path +
                                  ": object branch for class '" + root_class_name + "' is absent");
            }
            continue;
        }

        auto* rc = TClass::GetClass(root_class_name.c_str());
        if (!rc || !rc->HasDictionary())
        {
            if (gstate.file_scheduler) {
                throw IOException("ROOT dictionary is unavailable for class '" + root_class_name + "'");
            }
            continue;
        }

        ObjectContext ctx;
        ctx.Bind(tree, branch, rc, root_class_name,
                 bind_data.dictionary_cleanup_mode);
        local_state->root_contexts.emplace(root_class_name, std::move(ctx));
    }

    std::vector<idx_t> serialized_candidates;
    for (const auto col_idx : gstate.scan_column_ids)
    {
        if (col_idx == COLUMN_IDENTIFIER_ROW_ID || col_idx >= bind_data.columns.size()) continue;
        const auto& col = bind_data.columns[col_idx];
        if (col.is_virtual_index || col.is_string || col.levels.empty() || col.logical_path.empty()) continue;
        if (std::find(serialized_candidates.begin(), serialized_candidates.end(), col_idx) ==
            serialized_candidates.end()) {
            serialized_candidates.push_back(col_idx);
        }
    }
    if (serialized_candidates.empty())
    {
        for (idx_t col_idx = 0; col_idx < bind_data.columns.size(); ++col_idx)
        {
            const auto& col = bind_data.columns[col_idx];
            if (!col.is_virtual_index && !col.is_string && !col.levels.empty() && !col.logical_path.empty()) {
                serialized_candidates.push_back(col_idx);
            }
        }
    }

    if (serialized_candidates.size() == 1)
    {
        const auto col_idx = serialized_candidates.front();
        const auto& col = bind_data.columns[col_idx];
        auto context_it = local_state->root_contexts.find(col.branch_name);
        if (context_it != local_state->root_contexts.end())
        {
            auto& object_context = context_it->second;
            const auto parsed = rootlake::ParsePath(col.logical_path);
            const auto physical = rootlake::ResolvePhysicalBranch(object_context.branch, parsed.fields);
            local_state->serialized_plan = rootlake::BuildSerializedReadPlan(
                object_context.root_class, parsed, physical.branch);
            const auto projection = physical.mode == "ancestor"
                ? rootlake::ApplyBranchProjection(
                    object_context.tree, {physical.branch}, bind_data.tree_cache_bytes)
                : rootlake::BranchProjectionResult {};
            if (!projection.applied) {
                rootlake::EnableAllBranches(object_context.tree, bind_data.tree_cache_bytes);
            }
            if (bind_data.reader_mode != rootlake::RootReaderMode::OBJECT &&
                local_state->serialized_plan.supported)
            {
                local_state->serialized_reader.Bind(
                    physical.branch, local_state->serialized_plan,
                    bind_data.raw_max_entry_bytes, bind_data.raw_max_values_per_entry,
                    object_context.CurrentObject());
                local_state->serialized_active = true;
                local_state->serialized_column = col_idx;
                local_state->serialized_context = col.branch_name;
                local_state->validation_remaining = bind_data.raw_validation_entries;
            }
            else if (bind_data.reader_mode == rootlake::RootReaderMode::SERIALIZED)
            {
                throw InvalidInputException("reader_mode='serialized' cannot read " + col.logical_path +
                                            ": " + local_state->serialized_plan.reason);
            }
            else if (bind_data.reader_mode == rootlake::RootReaderMode::AUTO)
            {
                rootlake::WarnRootFallbackOnce(col.logical_path,
                    local_state->serialized_plan.schema_fingerprint,
                    local_state->serialized_plan.reason);
            }
        }
        else if (bind_data.reader_mode == rootlake::RootReaderMode::SERIALIZED)
        {
            throw InvalidInputException("reader_mode='serialized' cannot bind ROOT object context for " +
                                        col.logical_path);
        }
        else
        {
            rootlake::WarnRootFallbackOnce(col.logical_path, "unknown",
                                           "ROOT object context is unavailable");
        }
    }
    else if (bind_data.reader_mode == rootlake::RootReaderMode::SERIALIZED &&
             serialized_candidates.size() > 1)
    {
        throw InvalidInputException(
            "reader_mode='serialized' requires exactly one materialized logical ROOT value column");
    }

    for (const auto& col : bind_data.columns)
    {
        if (col.is_virtual_index)
        {
            local_state->has_container_columns = true;
            break;
        }
    }

    if (gstate.file_scheduler) {
        uint64_t entries = 0;
        if (!local_state->root_contexts.empty()) {
            entries = static_cast<uint64_t>(std::max<Long64_t>(
                0, local_state->root_contexts.begin()->second.tree->GetEntries()));
        }
        local_state->local_current_row = gstate.event_range_impossible
            ? entries : std::min(entries, gstate.event_lower);
        local_state->local_end_row = entries;
        if (gstate.event_upper != std::numeric_limits<uint64_t>::max()) {
            local_state->local_end_row = std::min(entries, gstate.event_upper + 1);
        }
        local_state->file_active = true;
        const auto fingerprint = local_state->serialized_plan.schema_fingerprint.empty()
            ? std::string("object:") + bind_data.tree_name
            : local_state->serialized_plan.schema_fingerprint;
        gstate.file_scheduler->ObserveSchema(fingerprint);
    }
}

unique_ptr<LocalTableFunctionState> RootScanInitLocal(ExecutionContext& context,
                                                      TableFunctionInitInput& input,
                                                      GlobalTableFunctionState* global_state_p)
{
    auto& bind_data = input.bind_data->Cast<FastRootBindData>();
    auto& gstate = global_state_p->Cast<FastRootGlobalState>();
    auto local_state = make_uniq<FastRootLocalState>();
    if (!bind_data.is_empty_mode && !bind_data.is_browse_mode && !bind_data.IsMultiFile()) {
        InitializeRootLocalFile(bind_data, gstate, *local_state, bind_data.root_path, true);
    }
    return std::move(local_state);
}

static void ResetRootLocalFile(FastRootGlobalState& gstate,
                               FastRootLocalState& local_state,
                               bool completed)
{
    if (completed && local_state.file_active && gstate.file_scheduler) {
        const auto elapsed = std::chrono::steady_clock::now() - local_state.file_started;
        gstate.file_scheduler->RecordComplete(
            local_state.file_task,
            static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::microseconds>(elapsed).count()));
    }
    local_state.serialized_reader.Bind(nullptr, rootlake::SerializedReadPlan {});
    local_state.serialized_active = false;
    local_state.serialized_column = DConstants::INVALID_INDEX;
    local_state.serialized_context.clear();
    local_state.serialized_plan = {};
    local_state.validation_remaining = 0;
    local_state.serialized_values.clear();
    local_state.serialized_indices.clear();
    local_state.reported_serialized_baskets = 0;
    local_state.reported_serialized_compressed_bytes = 0;
    local_state.reported_serialized_entry_bytes = 0;
    local_state.cached_results.clear();
    local_state.has_cached_entry = false;
    local_state.current_entry = std::numeric_limits<uint64_t>::max();
    local_state.current_elem_idx = 0;
    local_state.root_contexts.clear();
    local_state.direct_branch = nullptr;
    local_state.direct_leaf = nullptr;
    local_state.root_file.Close();
    local_state.local_current_row = 0;
    local_state.local_end_row = 0;
    local_state.has_container_columns = false;
    local_state.file_active = false;
}

static bool EnsureMultiFileReady(const FastRootBindData& bind_data,
                                 FastRootGlobalState& gstate,
                                 FastRootLocalState& local_state)
{
    if (!gstate.file_scheduler) throw InternalException("multi-file ROOT scheduler is unavailable");
    while (true) {
        if (local_state.file_active &&
            (local_state.has_cached_entry || local_state.local_current_row < local_state.local_end_row)) {
            return true;
        }
        if (local_state.file_active) ResetRootLocalFile(gstate, local_state, true);

        rootlake::RootDirectFileTask task;
        if (!gstate.file_scheduler->Claim(task)) {
            if (gstate.file_scheduler->AllFilesFinished()) {
                const auto failures = gstate.file_scheduler->FailureSummary();
                if (!failures.empty()) throw IOException(failures);
            }
            return false;
        }

        local_state.file_task = std::move(task);
        local_state.file_started = std::chrono::steady_clock::now();
        try {
            InitializeRootLocalFile(bind_data, gstate, local_state,
                                    local_state.file_task.path, false);
        } catch (const rootlake::RootFileUnavailableException &exception) {
            gstate.file_scheduler->RecordUnavailable(
                local_state.file_task, exception.attempts, exception.elapsed_us);
            ResetRootLocalFile(gstate, local_state, false);
            continue;
        } catch (const std::exception &exception) {
            gstate.file_scheduler->RecordFailure(local_state.file_task, exception.what());
            ResetRootLocalFile(gstate, local_state, false);
            continue;
        } catch (...) {
            gstate.file_scheduler->RecordFailure(local_state.file_task, "unknown ROOT reader error");
            ResetRootLocalFile(gstate, local_state, false);
            continue;
        }
    }
}

// ============================================================
// ProcessBrowseMode
// ============================================================
static void ProcessBrowseMode(
    ClientContext& context,
    const FastRootBindData& bind_data,
    FastRootGlobalState& gstate,
    FastRootLocalState& lstate,
    DataChunk& output)
{
    const auto& children = bind_data.browse_children;
    std::lock_guard<std::mutex> lock(gstate.coordination_mutex);
    size_t start = gstate.browse_offset;

    if (start >= children.size())
    {
        output.SetCardinality(0);
        return;
    }

    size_t count = 0;
    size_t i = start;
    for (; i < children.size() && count < STANDARD_VECTOR_SIZE; ++i)
    {
        bool passes = true;
        if (gstate.filters)
        {
            for (const auto& filter : gstate.filters->filters)
            {
                if (filter.first >= gstate.scan_column_ids.size()) continue;
                const auto column = gstate.scan_column_ids[filter.first];
                const auto actual = column == COLUMN_IDENTIFIER_ROW_ID
                    ? rootlake::RootScalarActual::Event(i)
                    : (column == 0 ? rootlake::RootScalarActual::String(children[i])
                                   : rootlake::RootScalarActual::Null(LogicalType::SQLNULL));
                if (!lstate.filter_evaluator.Evaluate(context, *filter.second, actual)) {
                    passes = false;
                    break;
                }
            }
        }
        if (!passes) continue;
        for (idx_t output_index = 0; output_index < gstate.output_column_ids.size(); ++output_index)
        {
            auto& vector = output.data[output_index];
            const auto column = gstate.output_column_ids[output_index];
            if (column == 0)
            {
                FlatVector::GetData<string_t>(vector)[count] = StringVector::AddString(vector, children[i]);
                FlatVector::Validity(vector).SetValid(count);
            }
            else if (column == COLUMN_IDENTIFIER_ROW_ID)
            {
                FlatVector::GetData<int64_t>(vector)[count] = static_cast<int64_t>(i);
                FlatVector::Validity(vector).SetValid(count);
            }
            else
            {
                FlatVector::Validity(vector).SetInvalid(count);
            }
        }
        ++count;
    }
    gstate.browse_offset = i;
    output.SetCardinality(count);
}

static bool WriteNumericValue(Vector& vector, idx_t row, double value)
{
    switch (vector.GetType().id())
    {
        case LogicalTypeId::TINYINT:
            FlatVector::GetData<int8_t>(vector)[row] = static_cast<int8_t>(value); break;
        case LogicalTypeId::UTINYINT:
            FlatVector::GetData<uint8_t>(vector)[row] = static_cast<uint8_t>(value); break;
        case LogicalTypeId::SMALLINT:
            FlatVector::GetData<int16_t>(vector)[row] = static_cast<int16_t>(value); break;
        case LogicalTypeId::USMALLINT:
            FlatVector::GetData<uint16_t>(vector)[row] = static_cast<uint16_t>(value); break;
        case LogicalTypeId::INTEGER:
            FlatVector::GetData<int32_t>(vector)[row] = static_cast<int32_t>(value); break;
        case LogicalTypeId::UINTEGER:
            FlatVector::GetData<uint32_t>(vector)[row] = static_cast<uint32_t>(value); break;
        case LogicalTypeId::BIGINT:
            FlatVector::GetData<int64_t>(vector)[row] = static_cast<int64_t>(value); break;
        case LogicalTypeId::UBIGINT:
            FlatVector::GetData<uint64_t>(vector)[row] = static_cast<uint64_t>(value); break;
        case LogicalTypeId::FLOAT:
            FlatVector::GetData<float>(vector)[row] = static_cast<float>(value); break;
        case LogicalTypeId::DOUBLE:
            FlatVector::GetData<double>(vector)[row] = value; break;
        case LogicalTypeId::BOOLEAN:
            FlatVector::GetData<bool>(vector)[row] = value != 0.0; break;
        default:
            FlatVector::Validity(vector).SetInvalid(row);
            return false;
    }
    FlatVector::Validity(vector).SetValid(row);
    return true;
}

static std::optional<int32_t> ResolveCachedIndexValue(const FastRootBindData& bind_data,
                                                       const FastRootLocalState& lstate,
                                                       idx_t col_idx, size_t elem_idx)
{
    if (col_idx >= bind_data.columns.size()) return std::nullopt;
    const auto& col = bind_data.columns[col_idx];
    std::string search = col.name;
    if (search.size() > 4 && search.substr(search.size() - 4) == "_idx") search.resize(search.size() - 4);
    for (idx_t candidate_index = 0; candidate_index < bind_data.columns.size(); ++candidate_index)
    {
        const auto& candidate = bind_data.columns[candidate_index];
        if (candidate.is_virtual_index || candidate.levels.empty() ||
            candidate.branch_name != col.branch_name || candidate_index >= lstate.cached_results.size()) continue;
        const auto& result = lstate.cached_results[candidate_index];
        if (elem_idx >= result.size() || elem_idx >= result.vector_indices.size()) continue;
        for (size_t name_index = 0; name_index < result.vector_names.size(); ++name_index)
        {
            std::string name = result.vector_names[name_index];
            if (name.size() > 4 && name.substr(name.size() - 4) == "_idx") name.resize(name.size() - 4);
            if (name == search && name_index < result.vector_indices[elem_idx].size())
            {
                return static_cast<int32_t>(result.vector_indices[elem_idx][name_index]);
            }
        }
    }
    return std::nullopt;
}

static rootlake::RootScalarActual CachedScalar(const FastRootBindData& bind_data,
                                                const FastRootLocalState& lstate,
                                                idx_t col_idx, uint64_t entry, size_t elem_idx)
{
    if (col_idx == COLUMN_IDENTIFIER_ROW_ID) return rootlake::RootScalarActual::Signed(entry);
    if (col_idx >= bind_data.columns.size()) return rootlake::RootScalarActual::Null(LogicalType::SQLNULL);
    const auto& column = bind_data.columns[col_idx];
    if (column.name == "event_id" && column.levels.empty()) {
        return rootlake::RootScalarActual::Signed(entry);
    }
    if (col_idx == bind_data.source_id_column) {
        return rootlake::RootScalarActual::Event(lstate.file_task.source_id);
    }
    if (col_idx == bind_data.source_path_column) {
        return rootlake::RootScalarActual::String(lstate.file_task.path);
    }
    if (column.is_virtual_index) return rootlake::RootScalarActual::Index(
        ResolveCachedIndexValue(bind_data, lstate, col_idx, elem_idx));
    const auto logical_type = RootTypeToDuckDB(column.root_type, column.is_string, true);
    if (col_idx >= lstate.cached_results.size()) return rootlake::RootScalarActual::Null(logical_type);
    const auto& result = lstate.cached_results[col_idx];
    if (elem_idx >= result.size()) return rootlake::RootScalarActual::Null(logical_type);
    if (result.is_string_flag[elem_idx]) return rootlake::RootScalarActual::String(result.strings[elem_idx]);
    return rootlake::RootScalarActual::Numeric(logical_type, result.numbers[elem_idx]);
}

static bool PassesCachedFilters(ClientContext& context, const FastRootBindData& bind_data,
                                const FastRootGlobalState& gstate, FastRootLocalState& lstate,
                                uint64_t entry, size_t elem_idx)
{
    if (!gstate.filters) return true;
    for (const auto& filter : gstate.filters->filters)
    {
        if (filter.first >= gstate.scan_column_ids.size()) continue;
        const auto actual = CachedScalar(bind_data, lstate, gstate.scan_column_ids[filter.first], entry, elem_idx);
        if (!lstate.filter_evaluator.Evaluate(context, *filter.second, actual)) return false;
    }
    return true;
}

static bool PassesDirectBranchFilters(ClientContext& context, const FastRootBindData& bind_data,
                                      const FastRootGlobalState& gstate, FastRootLocalState& lstate,
                                      uint64_t entry, double value)
{
    if (!gstate.filters) return true;
    const auto value_type = RootTypeToDuckDB(bind_data.direct_branch_info.type_name, false, true);
    for (const auto& filter : gstate.filters->filters)
    {
        if (filter.first >= gstate.scan_column_ids.size()) continue;
        const auto column = gstate.scan_column_ids[filter.first];
        auto actual = rootlake::RootScalarActual::Null(LogicalType::SQLNULL);
        if (column == 0 || column == COLUMN_IDENTIFIER_ROW_ID) {
            actual = rootlake::RootScalarActual::Signed(entry);
        } else if (column == 1) {
            actual = rootlake::RootScalarActual::Numeric(value_type, value);
        } else if (column == bind_data.source_id_column) {
            actual = rootlake::RootScalarActual::Event(lstate.file_task.source_id);
        } else if (column == bind_data.source_path_column) {
            actual = rootlake::RootScalarActual::String(lstate.file_task.path);
        } else {
            actual = rootlake::RootScalarActual::Null(LogicalType::SQLNULL);
        }
        if (!lstate.filter_evaluator.Evaluate(context, *filter.second, actual)) return false;
    }
    return true;
}

// ============================================================
// ProcessDirectBranch (чтение простой ветки)
// ============================================================
static void ProcessDirectBranch(
    ClientContext& context,
    const FastRootBindData& bind_data,
    FastRootGlobalState& gstate,
    FastRootLocalState& lstate,
    DataChunk& output)
{
    if (!lstate.direct_branch || !lstate.direct_leaf)
    {
        output.SetCardinality(0);
        return;
    }

    idx_t out_count = 0;
    // output имеет две колонки: event_id и значение ветки
    while (out_count < STANDARD_VECTOR_SIZE)
    {
        if (lstate.local_current_row >= lstate.local_end_row)
        {
            if (gstate.file_scheduler) break;
            WorkScheduler scheduler(gstate.next_row, gstate.total_rows, gstate.coordination_mutex);
            auto batch = scheduler.ClaimWork(100000);
            if (!batch.HasWork()) break;
            lstate.local_current_row = batch.start;
            lstate.local_end_row = batch.end;
        }
        Long64_t entry = lstate.local_current_row;
        // Bare primitive-branch compatibility only.  Even here the entry is loaded
        // through TTree so no physical branch becomes an alternative semantic reader.
        auto* direct_tree = lstate.root_file.GetTTree();
        if (!direct_tree || direct_tree->GetEntry(entry) < 0)
        {
            break;
        }

        const double val = lstate.direct_leaf->GetValue();
        ++lstate.local_current_row;
        if (!PassesDirectBranchFilters(context, bind_data, gstate, lstate, entry, val)) continue;

        for (idx_t output_index = 0; output_index < gstate.output_column_ids.size(); ++output_index)
        {
            const auto column = gstate.output_column_ids[output_index];
            auto& vector = output.data[output_index];
            if (column == 0 || column == COLUMN_IDENTIFIER_ROW_ID)
            {
                FlatVector::GetData<int64_t>(vector)[out_count] = static_cast<int64_t>(entry);
                FlatVector::Validity(vector).SetValid(out_count);
            }
            else if (column == 1)
            {
                WriteNumericValue(vector, out_count, val);
            }
            else if (column == bind_data.source_id_column)
            {
                FlatVector::GetData<uint64_t>(vector)[out_count] = lstate.file_task.source_id;
                FlatVector::Validity(vector).SetValid(out_count);
            }
            else if (column == bind_data.source_path_column)
            {
                FlatVector::GetData<string_t>(vector)[out_count] =
                    StringVector::AddString(vector, lstate.file_task.path);
                FlatVector::Validity(vector).SetValid(out_count);
            }
            else
            {
                FlatVector::Validity(vector).SetInvalid(out_count);
            }
        }

        out_count++;
    }
    output.SetCardinality(out_count);
}

// ============================================================
// ProcessCachedEntry (для сложных объектов)
// ============================================================
static void ProcessCachedEntry(
    ClientContext& context,
    const FastRootBindData& bind_data,
    FastRootGlobalState& gstate,
    FastRootLocalState& lstate,
    DataChunk& output,
    idx_t& out_count)
{
    uint64_t entry = lstate.current_entry;
    size_t max_elements = 0;
    for (const auto& res : lstate.cached_results)
    {
        if (!res.empty())
        {
            max_elements = std::max(max_elements, res.size());
        }
    }
    if (max_elements == 0)
    {
        max_elements = 1;
    }

    size_t start_elem = lstate.current_elem_idx;
    size_t next_elem = start_elem;

    for (size_t elem_idx = start_elem; elem_idx < max_elements && out_count < STANDARD_VECTOR_SIZE; ++elem_idx)
    {
        next_elem = elem_idx + 1;
        if (!PassesCachedFilters(context, bind_data, gstate, lstate, entry, elem_idx)) continue;
        for (idx_t out_idx = 0; out_idx < gstate.output_column_ids.size(); ++out_idx)
        {
            idx_t col_idx = gstate.output_column_ids[out_idx];
            auto& vec = output.data[out_idx];

            if (col_idx == COLUMN_IDENTIFIER_ROW_ID)
            {
                FlatVector::GetData<int64_t>(vec)[out_count] = static_cast<int64_t>(entry);
                FlatVector::Validity(vec).SetValid(out_count);
                continue;
            }

            if (col_idx >= bind_data.columns.size())
            {
                FlatVector::Validity(vec).SetInvalid(out_count);
                continue;
            }

            const auto& col = bind_data.columns[col_idx];

            if (col.name == "event_id" && col.levels.empty())
            {
                FlatVector::GetData<int64_t>(vec)[out_count] = static_cast<int64_t>(entry);
                FlatVector::Validity(vec).SetValid(out_count);
                continue;
            }

            if (col_idx == bind_data.source_id_column)
            {
                FlatVector::GetData<uint64_t>(vec)[out_count] = lstate.file_task.source_id;
                FlatVector::Validity(vec).SetValid(out_count);
                continue;
            }

            if (col_idx == bind_data.source_path_column)
            {
                FlatVector::GetData<string_t>(vec)[out_count] =
                    StringVector::AddString(vec, lstate.file_task.path);
                FlatVector::Validity(vec).SetValid(out_count);
                continue;
            }

            if (col.is_virtual_index)
            {
                // A lineage/index column must remain readable even when DuckDB
                // projects the terminal value column away, e.g.
                //   SELECT min(vecCaloClus_idx) FROM read_root(.../e)
                //
                // The old code searched only gstate.column_ids.  In an aggregate
                // query that list can contain event_id + the virtual index only,
                // so no materialized leaf was found and the validity mask was
                // incorrectly set to NULL for every index.  Search the cached
                // materialized results instead: ReadAndCacheEntry always builds at
                // least one representative leaf for container-only projections.
                std::string search = col.name;
                if (search.size() > 4 && search.substr(search.size() - 4) == "_idx")
                {
                    search = search.substr(0, search.size() - 4);
                }

                idx_t ref_col_idx = static_cast<idx_t>(-1);
                int idx_pos = -1;

                for (idx_t cand = 0; cand < bind_data.columns.size(); ++cand)
                {
                    const auto& candidate = bind_data.columns[cand];
                    if (candidate.is_virtual_index || candidate.levels.empty() ||
                        candidate.branch_name != col.branch_name ||
                        cand >= lstate.cached_results.size())
                    {
                        continue;
                    }

                    const auto& candidate_res = lstate.cached_results[cand];
                    if (candidate_res.empty())
                    {
                        continue;
                    }

                    for (size_t name_idx = 0; name_idx < candidate_res.vector_names.size(); ++name_idx)
                    {
                        std::string vn = candidate_res.vector_names[name_idx];
                        if (vn.size() > 4 && vn.substr(vn.size() - 4) == "_idx")
                        {
                            vn = vn.substr(0, vn.size() - 4);
                        }
                        if (vn == search)
                        {
                            ref_col_idx = cand;
                            idx_pos = static_cast<int>(name_idx);
                            break;
                        }
                    }

                    if (ref_col_idx != static_cast<idx_t>(-1))
                    {
                        break;
                    }
                }

                if (ref_col_idx == static_cast<idx_t>(-1) || idx_pos < 0)
                {
                    RootDebug("INDEX.NO_REFERENCE",
                              "column=" + col.name +
                              " branch=" + col.branch_name +
                              " projected_columns=" + std::to_string(gstate.output_column_ids.size()));
                    FlatVector::Validity(vec).SetInvalid(out_count);
                    continue;
                }

                const auto& ref_res = lstate.cached_results[ref_col_idx];
                if (elem_idx >= ref_res.size() ||
                    elem_idx >= ref_res.vector_indices.size() ||
                    static_cast<size_t>(idx_pos) >= ref_res.vector_indices[elem_idx].size())
                {
                    FlatVector::Validity(vec).SetInvalid(out_count);
                    continue;
                }

                FlatVector::GetData<int32_t>(vec)[out_count] =
                    ref_res.vector_indices[elem_idx][static_cast<size_t>(idx_pos)];
                FlatVector::Validity(vec).SetValid(out_count);
                continue;
            }

            if (col_idx >= lstate.cached_results.size())
            {
                FlatVector::Validity(vec).SetInvalid(out_count);
                continue;
            }

            const auto& res = lstate.cached_results[col_idx];
            if (elem_idx >= res.size())
            {
                FlatVector::Validity(vec).SetInvalid(out_count);
                continue;
            }

            if (res.is_string_flag[elem_idx])
            {
                if (vec.GetType().id() == LogicalTypeId::VARCHAR)
                {
                    FlatVector::GetData<string_t>(vec)[out_count] = StringVector::AddString(vec, res.strings[elem_idx]);
                    FlatVector::Validity(vec).SetValid(out_count);
                }
                else
                {
                    FlatVector::Validity(vec).SetInvalid(out_count);
                }
            }
            else
            {
                const double val = res.numbers[elem_idx];
                if (!WriteNumericValue(vec, out_count, val))
                {
                    continue;
                }
            }
        }
        out_count++;
    }

    lstate.current_elem_idx = next_elem;
    if (lstate.current_elem_idx >= max_elements)
    {
        lstate.has_cached_entry = false;
        lstate.cached_results.clear();
        lstate.local_current_row++;
    }
}

// ============================================================
// CacheResult
// ============================================================
enum class CacheResult
{
    CACHED,
    CONTINUE_LOOP,
    BREAK_LOOP
};

static std::vector<std::string> SplitIndexSignature(const std::string& signature)
{
    std::vector<std::string> names;
    std::stringstream stream(signature);
    std::string name;
    while (std::getline(stream, name, ',')) {
        if (!name.empty()) names.push_back(name);
    }
    return names;
}

static void MaterializeSerializedResult(const RootLakeColumnInfo& column, uint64_t entry,
                                        const rootlake::SerializedReadPlan& plan,
                                        const std::vector<double>& values,
                                        const std::vector<int32_t>& flat_indices,
                                        ReadResult& result)
{
    result.clear();
    const auto index_names = SplitIndexSignature(column.index_signature);
    if (index_names.size() != plan.index_depth ||
        flat_indices.size() != values.size() * plan.index_depth) {
        throw InternalException("serialized ROOT index shape differs from bound SQL schema");
    }
    std::vector<int> indices(plan.index_depth);
    for (idx_t value_index = 0; value_index < values.size(); ++value_index) {
        for (idx_t depth = 0; depth < plan.index_depth; ++depth) {
            indices[depth] = flat_indices[value_index * plan.index_depth + depth];
        }
        result.add_number(values[value_index], static_cast<Long64_t>(entry), indices, index_names);
    }
}

static void ReadResultAsSerializedVectors(const ReadResult& result,
                                          std::vector<double>& values,
                                          std::vector<int32_t>& flat_indices)
{
    values.clear();
    flat_indices.clear();
    values.reserve(result.numbers.size());
    for (idx_t i = 0; i < result.numbers.size(); ++i) {
        if (result.is_string_flag[i]) continue;
        values.push_back(result.numbers[i]);
        for (const auto index : result.vector_indices[i]) {
            flat_indices.push_back(static_cast<int32_t>(index));
        }
    }
}

// ============================================================
// ReadAndCacheEntry
// ============================================================
static CacheResult ReadAndCacheEntry(
    const FastRootBindData& bind_data,
    FastRootGlobalState& gstate,
    FastRootLocalState& lstate,
    DataChunk& output,
    idx_t& out_count)
{
    uint64_t entry = lstate.local_current_row;
    
    std::map<std::string, std::vector<idx_t>> branch_columns;
    for (idx_t out_idx = 0; out_idx < gstate.scan_column_ids.size(); ++out_idx)
    {
        idx_t col_idx = gstate.scan_column_ids[out_idx];
        if (col_idx == COLUMN_IDENTIFIER_ROW_ID || col_idx >= bind_data.columns.size())
        {
            continue;
        }
        const auto& col = bind_data.columns[col_idx];
        if (!col.levels.empty() && !col.is_virtual_index && col.name != "event_id")
        {
            branch_columns[col.branch_name].push_back(col_idx);
        }
    }

    lstate.cached_results.resize(bind_data.columns.size());
    bool has_data = false;

    if (lstate.serialized_active && lstate.serialized_column < bind_data.columns.size())
    {
        std::string failure_reason;
        const bool decoded = lstate.serialized_reader.Decode(
            entry, lstate.serialized_values, lstate.serialized_indices, failure_reason);
        const auto& serialized_counters = lstate.serialized_reader.Counters();
        gstate.serialized_baskets.fetch_add(
            serialized_counters.baskets - lstate.reported_serialized_baskets);
        gstate.serialized_compressed_bytes.fetch_add(
            serialized_counters.compressed_bytes - lstate.reported_serialized_compressed_bytes);
        gstate.serialized_entry_bytes.fetch_add(
            serialized_counters.serialized_bytes - lstate.reported_serialized_entry_bytes);
        lstate.reported_serialized_baskets = serialized_counters.baskets;
        lstate.reported_serialized_compressed_bytes = serialized_counters.compressed_bytes;
        lstate.reported_serialized_entry_bytes = serialized_counters.serialized_bytes;
        if (decoded)
        {
            gstate.serialized_entries.fetch_add(1);
            gstate.serialized_values.fetch_add(lstate.serialized_values.size());
            if (lstate.validation_remaining > 0)
            {
                auto context_it = lstate.root_contexts.find(lstate.serialized_context);
                ReadResult reference;
                bool reference_ok = false;
                if (context_it != lstate.root_contexts.end())
                {
                    auto& ctx = context_it->second;
                    const auto bytes = ctx.tree->GetEntry(static_cast<Long64_t>(entry));
                    gstate.object_validation_entries.fetch_add(1);
                    if (bytes >= 0 && ctx.CurrentObject())
                    {
                        const auto& column = bind_data.columns[lstate.serialized_column];
                        ObjectReader::collect(ctx.CurrentObject(), column.levels,
                                              std::numeric_limits<Long64_t>::max(),
                                              static_cast<Long64_t>(entry), reference);
                        std::vector<double> reference_values;
                        std::vector<int32_t> reference_indices;
                        ReadResultAsSerializedVectors(reference, reference_values, reference_indices);
                        reference_ok = rootlake::EqualDecodedValues(
                            lstate.serialized_values, lstate.serialized_indices,
                            reference_values, reference_indices);
                    }
                }
                if (!reference_ok) failure_reason = "serialized values differ from universal ROOT reader";
                else --lstate.validation_remaining;
            }
        }

        if (!decoded || !failure_reason.empty())
        {
            if (bind_data.reader_mode == rootlake::RootReaderMode::SERIALIZED) {
                throw IOException("reader_mode='serialized' failed for " +
                                  lstate.serialized_plan.logical_path + ": " + failure_reason);
            }
            rootlake::WarnRootFallbackOnce(lstate.serialized_plan.logical_path,
                                           lstate.serialized_plan.schema_fingerprint,
                                           failure_reason);
            auto context_it = lstate.root_contexts.find(lstate.serialized_context);
            if (context_it != lstate.root_contexts.end()) {
                rootlake::EnableAllBranches(context_it->second.tree, bind_data.tree_cache_bytes);
            }
            lstate.serialized_active = false;
        }
        else
        {
            const auto& column = bind_data.columns[lstate.serialized_column];
            MaterializeSerializedResult(column, entry, lstate.serialized_plan,
                                        lstate.serialized_values, lstate.serialized_indices,
                                        lstate.cached_results[lstate.serialized_column]);
            has_data = !lstate.cached_results[lstate.serialized_column].empty();
            if (!has_data) {
                lstate.local_current_row++;
                return CacheResult::CONTINUE_LOOP;
            }
            lstate.current_entry = entry;
            lstate.current_elem_idx = 0;
            lstate.has_cached_entry = true;
            return CacheResult::CACHED;
        }
    }

    if (branch_columns.empty() && lstate.has_container_columns)
    {
        idx_t sample_col_idx = static_cast<idx_t>(-1);
        std::string sample_branch;
        
        for (idx_t i = 0; i < bind_data.columns.size(); ++i)
        {
            const auto& col = bind_data.columns[i];
            if (!col.levels.empty() && !col.is_virtual_index && col.name != "event_id")
            {
                sample_col_idx = i;
                sample_branch = col.branch_name;
                break;
            }
        }

        if (sample_col_idx != static_cast<idx_t>(-1))
        {
            auto it = lstate.root_contexts.find(sample_branch);
            if (it != lstate.root_contexts.end())
            {
                auto& ctx = it->second;
                if (entry < static_cast<uint64_t>(ctx.tree->GetEntries()))
                {
                    RootDebug("READ.BEFORE_GET_ENTRY",
                              "mode=sample class=" + sample_branch +
                              " entry=" + std::to_string(entry) +
                              " tree_ptr=" + RootPointer(ctx.tree));
                    Long64_t bytes = ctx.tree->GetEntry(entry);
                    gstate.object_fallback_entries.fetch_add(1);
                    RootDebug("READ.AFTER_GET_ENTRY",
                              "mode=sample class=" + sample_branch +
                              " entry=" + std::to_string(entry) +
                              " bytes=" + std::to_string(bytes) +
                              " object_ptr=" + RootPointer(ctx.CurrentObject()));
                    if (bytes >= 0 && ctx.CurrentObject())
                    {
                        const auto& col = bind_data.columns[sample_col_idx];
                        lstate.cached_results[sample_col_idx].clear();
                        ObjectReader::collect(ctx.CurrentObject(), col.levels, std::numeric_limits<Long64_t>::max(), entry, lstate.cached_results[sample_col_idx]);
                        has_data = !lstate.cached_results[sample_col_idx].empty();
                    }
                }
            }
        }
    }
    else
    {
        for (const auto& [branch_name, col_indices] : branch_columns)
        {
            auto it = lstate.root_contexts.find(branch_name);
            if (it == lstate.root_contexts.end())
            {
                continue;
            }
            auto& ctx = it->second;
            if (entry >= static_cast<uint64_t>(ctx.tree->GetEntries()))
            {
                continue;
            }

            RootDebug("READ.BEFORE_GET_ENTRY",
                      "mode=group class=" + branch_name +
                      " entry=" + std::to_string(entry) +
                      " tree_ptr=" + RootPointer(ctx.tree));
            Long64_t bytes = ctx.tree->GetEntry(entry);
            gstate.object_fallback_entries.fetch_add(1);
            RootDebug("READ.AFTER_GET_ENTRY",
                      "mode=group class=" + branch_name +
                      " entry=" + std::to_string(entry) +
                      " bytes=" + std::to_string(bytes) +
                      " object_ptr=" + RootPointer(ctx.CurrentObject()));
            if (bytes < 0)
            {
                continue;
            }

            for (idx_t col_idx : col_indices)
            {
                const auto& col = bind_data.columns[col_idx];
                if (ctx.CurrentObject())
                {
                    lstate.cached_results[col_idx].clear();
                    ObjectReader::collect(ctx.CurrentObject(), col.levels, std::numeric_limits<Long64_t>::max(), entry, lstate.cached_results[col_idx]);
                    if (!lstate.cached_results[col_idx].empty())
                    {
                        has_data = true;
                    }
                }
            }
        }
    }

    if (!has_data)
    {
        if (lstate.has_container_columns)
        {
            lstate.local_current_row++;
            return CacheResult::CONTINUE_LOOP;
        }
        else
        {
            if (entry > 0)
            {
                lstate.local_current_row = lstate.local_end_row;
                return CacheResult::BREAK_LOOP;
            }
        }
    }

    lstate.current_entry = entry;
    lstate.current_elem_idx = 0;
    lstate.has_cached_entry = true;
    return CacheResult::CACHED;
}

// ============================================================
// RootScanFunction
// ============================================================
void RootScanFunction(ClientContext& context, TableFunctionInput& data_p, DataChunk& output)
{
    auto& bind_data = data_p.bind_data->Cast<FastRootBindData>();
    auto& gstate = data_p.global_state->Cast<FastRootGlobalState>();
    auto& lstate = data_p.local_state->Cast<FastRootLocalState>();

    if (bind_data.is_empty_mode)
    {
        output.SetCardinality(0);
        return;
    }
    if (gstate.event_range_impossible)
    {
        output.SetCardinality(0);
        return;
    }

    // 1. Browse mode
    if (bind_data.is_browse_mode)
    {
        ProcessBrowseMode(context, bind_data, gstate, lstate, output);
        return;
    }

    // 2. Режим прямой ветки (без индекса)
    if (bind_data.is_direct_branch_mode)
    {
        while (true) {
            if (gstate.file_scheduler && !EnsureMultiFileReady(bind_data, gstate, lstate)) {
                output.SetCardinality(0);
                return;
            }
            ProcessDirectBranch(context, bind_data, gstate, lstate, output);
            if (output.size() > 0) {
                if (gstate.file_scheduler) gstate.file_scheduler->RecordFirstRow();
                return;
            }
            if (!gstate.file_scheduler) return;
        }
    }

    // 3. Обычный режим с индексом (сложные объекты)
    while (true) {
        if (gstate.file_scheduler && !EnsureMultiFileReady(bind_data, gstate, lstate)) {
            output.SetCardinality(0);
            return;
        }

        idx_t out_count = 0;
        while (out_count < STANDARD_VECTOR_SIZE)
        {
            if (lstate.has_cached_entry)
            {
                ProcessCachedEntry(context, bind_data, gstate, lstate, output, out_count);
            }
            else
            {
                if (lstate.local_current_row >= lstate.local_end_row)
                {
                    if (gstate.file_scheduler) break;
                    WorkScheduler scheduler(gstate.next_row, gstate.total_rows, gstate.coordination_mutex);
                    auto batch = scheduler.ClaimWork(100000);
                    if (!batch.HasWork()) break;
                    lstate.local_current_row = batch.start;
                    lstate.local_end_row = batch.end;
                }
                CacheResult res = ReadAndCacheEntry(bind_data, gstate, lstate, output, out_count);
                if (res == CacheResult::CONTINUE_LOOP)
                {
                    continue;
                }
                if (res == CacheResult::BREAK_LOOP)
                {
                    lstate.local_current_row = lstate.local_end_row;
                    continue;
                }
            }
        }

        output.SetCardinality(out_count);
        if (out_count > 0) {
            if (gstate.file_scheduler) gstate.file_scheduler->RecordFirstRow();
            return;
        }
        if (!gstate.file_scheduler) return;
    }
}

// ============================================================
// RegisterRootScan
// ============================================================
static InsertionOrderPreservingMap<string> RootScanToString(TableFunctionToStringInput& input)
{
    InsertionOrderPreservingMap<string> result;
    if (!input.bind_data) return result;
    const auto& bind = input.bind_data->Cast<FastRootBindData>();
    result["ROOT Input"] = bind.input_specification;
    result["ROOT Files"] = std::to_string(bind.root_paths.size());
    result["ROOT Representative"] = bind.root_path;
    result["ROOT Bind Open Time (us)"] = std::to_string(bind.bind_open_us);
    result["ROOT Decode Mode"] = rootlake::RootReaderModeName(bind.reader_mode);
    result["Serialized Validation Entries"] = std::to_string(bind.raw_validation_entries);
    return result;
}

static InsertionOrderPreservingMap<string> RootScanDynamicToString(TableFunctionDynamicToStringInput& input)
{
    InsertionOrderPreservingMap<string> result;
    if (input.bind_data) {
        const auto& bind = input.bind_data->Cast<FastRootBindData>();
        result["ROOT Decode Mode"] = rootlake::RootReaderModeName(bind.reader_mode);
        result["ROOT Input Files"] = std::to_string(bind.root_paths.size());
        result["ROOT Representative"] = bind.root_path;
        result["ROOT Bind Open Time (us)"] = std::to_string(bind.bind_open_us);
    }
    if (!input.global_state) return result;
    const auto& global = input.global_state->Cast<FastRootGlobalState>();
    result["Serialized Entry Calls"] = std::to_string(global.serialized_entries.load());
    result["Serialized Values"] = std::to_string(global.serialized_values.load());
    result["Serialized Baskets"] = std::to_string(global.serialized_baskets.load());
    result["Serialized Basket Bytes"] = std::to_string(global.serialized_compressed_bytes.load());
    result["Serialized Entry Bytes"] = std::to_string(global.serialized_entry_bytes.load());
    result["Object Validation Entries"] = std::to_string(global.object_validation_entries.load());
    result["Object Fallback Entries"] = std::to_string(global.object_fallback_entries.load());
    if (global.file_scheduler) {
        result["ROOT Opened Files"] = std::to_string(global.file_scheduler->OpenedFiles());
        result["ROOT Completed Files"] = std::to_string(global.file_scheduler->CompletedFiles());
        result["ROOT Skipped Files"] = std::to_string(global.file_scheduler->SkippedFiles());
        result["ROOT Unavailable Files"] = std::to_string(global.file_scheduler->UnavailableFiles());
        result["ROOT Failed Files"] = std::to_string(global.file_scheduler->FailedFiles());
        result["ROOT Retried Opens"] = std::to_string(global.file_scheduler->RetriedOpens());
        result["ROOT Open Time (us)"] = std::to_string(global.file_scheduler->OpenTimeUs());
        result["ROOT Schema Variants"] = std::to_string(global.file_scheduler->SchemaVariants());
        result["ROOT Schema Plan Reuses"] = std::to_string(global.file_scheduler->SchemaPlanReuses());
        result["ROOT Time To First Row (us)"] = std::to_string(global.file_scheduler->FirstRowUs());
        result["ROOT Slowest File"] = global.file_scheduler->SlowestFile();
        result["ROOT Slowest File Time (us)"] = std::to_string(global.file_scheduler->SlowestFileUs());
    }
    return result;
}

void RegisterRootScan(ExtensionLoader &loader)
{
    TableFunction root_scan("read_root", {LogicalType::VARCHAR}, RootScanFunction, RootScanBind, RootScanInit);

    root_scan.named_parameters["dictionary"] = LogicalType::VARCHAR;
    root_scan.named_parameters["path_prefix"] = LogicalType::VARCHAR;
    root_scan.named_parameters["reader_mode"] = LogicalType::VARCHAR;
    root_scan.named_parameters["raw_validation_entries"] = LogicalType::UINTEGER;
    root_scan.named_parameters["raw_max_entry_bytes"] = LogicalType::UBIGINT;
    root_scan.named_parameters["raw_max_values_per_entry"] = LogicalType::UBIGINT;
    root_scan.named_parameters["tree_cache_bytes"] = LogicalType::UBIGINT;

    root_scan.filter_pushdown = true;
    root_scan.filter_prune = true;
    root_scan.projection_pushdown = true;
    root_scan.init_local = RootScanInitLocal;
    root_scan.to_string = RootScanToString;
    root_scan.dynamic_to_string = RootScanDynamicToString;
    loader.RegisterFunction(root_scan);
}

} // namespace duckdb 
