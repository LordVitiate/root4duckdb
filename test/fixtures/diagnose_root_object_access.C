// diagnose_root_object_access.C
// Usage:
// root -l -q 'diagnose_root_object_access.C("file.root","/path/libPhast.so","PaSetup","run",0)'

#include <TFile.h>
#include <TTree.h>
#include <TBranch.h>
#include <TBranchElement.h>
#include <TClass.h>
#include <TStreamerInfo.h>
#include <TStreamerElement.h>
#include <TObjArray.h>
#include <TSystem.h>

#include <iostream>
#include <memory>
#include <string>
#include <vector>
#include <algorithm>

namespace {

TBranch *FindObjectBranchExact(TTree *tree, const std::string &class_name) {
    if (!tree) return nullptr;
    auto *branches = tree->GetListOfBranches();
    for (int i = 0; i < branches->GetEntries(); ++i) {
        auto *branch = dynamic_cast<TBranch *>(branches->At(i));
        auto *element = dynamic_cast<TBranchElement *>(branch);
        if (element && element->GetClassName() && class_name == element->GetClassName()) {
            return branch;
        }
    }
    return nullptr;
}

void CollectBranches(TBranch *branch, std::vector<TBranch *> &out) {
    if (!branch) return;
    out.push_back(branch);
    auto *children = branch->GetListOfBranches();
    if (!children) return;
    for (int i = 0; i < children->GetEntries(); ++i) {
        CollectBranches(dynamic_cast<TBranch *>(children->At(i)), out);
    }
}

bool EndsWithToken(const std::string &name, const std::string &token) {
    if (name == token) return true;
    if (name.size() <= token.size()) return false;
    const size_t pos = name.size() - token.size();
    if (name.compare(pos, token.size(), token) != 0) return false;
    const char separator = name[pos - 1];
    return separator == '.' || separator == '/' || separator == '_';
}

TBranch *FindPhysicalLeafBranch(TBranch *object_branch, const std::string &field_name) {
    std::vector<TBranch *> all;
    CollectBranches(object_branch, all);
    TBranch *best = nullptr;
    int best_score = -1;
    for (auto *branch : all) {
        if (!branch) continue;
        const std::string name = branch->GetName();
        int score = -1;
        if (name == field_name) score = 100;
        else if (EndsWithToken(name, field_name)) score = 80;
        else if (name.find(field_name) != std::string::npos) score = 20;
        if (score > best_score) {
            best = branch;
            best_score = score;
        }
    }
    return best;
}

Long64_t ResolveDirectOffset(TClass *root_class, const std::string &field_name, std::string &type_name) {
    if (!root_class) return -1;
    auto *info = root_class->GetStreamerInfo();
    if (!info) return -1;
    auto *elements = info->GetElements();
    for (int i = 0; i < elements->GetEntries(); ++i) {
        auto *element = dynamic_cast<TStreamerElement *>(elements->At(i));
        if (!element || element->IsBase()) continue;
        if (field_name != element->GetName()) continue;
        type_name = element->GetTypeName();
        return info->GetElementOffset(i);
    }
    return -1;
}

void PrintBasketInfo(TBranch *branch) {
    if (!branch) return;
    std::cout << "physical branch: " << branch->GetName() << "\n";
    const int count = branch->GetWriteBasket() + 1;
    auto *entries = branch->GetBasketEntry();
    auto *bytes = branch->GetBasketBytes();
    std::cout << "basket_count: " << count << "\n";
    for (int i = 0; i < count; ++i) {
        const Long64_t begin = entries ? entries[i] : -1;
        const Long64_t seek = branch->GetBasketSeek(i);
        const int nbytes = bytes ? bytes[i] : -1;
        std::cout << "  basket " << i
                  << " entry_begin=" << begin
                  << " seek=" << seek
                  << " bytes=" << nbytes << "\n";
    }
}

} // namespace

void diagnose_root_object_access(
    const char *file_name,
    const char *dictionary,
    const char *tree_name = "PaSetup",
    const char *field_name = "run",
    Long64_t entry = 0) {

    const Long64_t load_result = gSystem->Load(dictionary);
    std::cout << "dictionary load result: " << load_result << "\n";
    if (load_result < 0) {
        std::cerr << "ERROR: dictionary load failed\n";
        return;
    }

    std::unique_ptr<TFile> file(TFile::Open(file_name, "READ"));
    if (!file || file->IsZombie()) {
        std::cerr << "ERROR: cannot open ROOT file\n";
        return;
    }

    TTree *tree = nullptr;
    file->GetObject(tree_name, tree);
    if (!tree) {
        std::cerr << "ERROR: tree not found: " << tree_name << "\n";
        return;
    }

    auto *root_class = TClass::GetClass(tree_name);
    if (!root_class || !root_class->HasDictionary()) {
        std::cerr << "ERROR: class/dictionary unavailable: " << tree_name << "\n";
        return;
    }

    auto *object_branch = FindObjectBranchExact(tree, tree_name);
    if (!object_branch) {
        std::cerr << "ERROR: exact object branch not found for class " << tree_name << "\n";
        return;
    }

    std::string type_name;
    const Long64_t offset = ResolveDirectOffset(root_class, field_name, type_name);
    if (offset < 0) {
        std::cerr << "ERROR: field not resolved: " << field_name << "\n";
        return;
    }

    std::cout << "tree: " << tree->GetName() << " entries=" << tree->GetEntries() << "\n";
    std::cout << "object branch: " << object_branch->GetName() << "\n";
    std::cout << "field: " << field_name << " type=" << type_name << " offset=" << offset << "\n";

    auto *physical_branch = FindPhysicalLeafBranch(object_branch, field_name);
    PrintBasketInfo(physical_branch);

    void *owned_object = root_class->New();
    void *address_slot = owned_object;
    if (!owned_object) {
        std::cerr << "ERROR: TClass::New failed\n";
        return;
    }

    object_branch->SetAutoDelete(kFALSE);
    object_branch->SetAddress(&address_slot);

    std::cout << "owned_object before: " << owned_object << "\n";
    std::cout << "address_slot before: " << address_slot << "\n";

    // This mirrors the successful universal_reader logic: materialize the tree,
    // then traverse the in-memory object using TStreamerInfo offsets.
    const Long64_t bytes_read = tree->GetEntry(entry);

    std::cout << "tree->GetEntry bytes: " << bytes_read << "\n";
    std::cout << "owned_object after:  " << owned_object << "\n";
    std::cout << "address_slot after:  " << address_slot << "\n";
    std::cout << "pointer_replaced: " << (address_slot != owned_object ? "YES" : "NO") << "\n";

    if (address_slot && (type_name == "int" || type_name == "Int_t")) {
        const int value = *reinterpret_cast<int *>(static_cast<char *>(address_slot) + offset);
        std::cout << "VALUE: " << value << "\n";
    } else {
        std::cout << "VALUE: diagnostic currently prints direct int/Int_t only\n";
    }

    object_branch->ResetAddress();

    // Critical ownership rule: destroy only what this diagnostic allocated.
    // Never destroy address_slot if ROOT replaced it with another pointer.
    root_class->Destructor(owned_object);
    owned_object = nullptr;
    address_slot = nullptr;

    std::cout << "cleanup: OK\n";
}
