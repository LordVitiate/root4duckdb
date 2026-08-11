#pragma once

#include "TFile.h"

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>

class TTree;

namespace duckdb::rootlake {

struct RootFileOpenResult {
    std::unique_ptr<TFile> file;
    uint32_t attempts = 0;
    uint64_t elapsed_us = 0;
    std::string error;

    explicit operator bool() const { return file && !file->IsZombie(); }
};

// A deliberately small automatic retry budget protects transient remote
// opens without hiding a persistently unavailable input for minutes.
RootFileOpenResult OpenRootFile(const std::string &path, uint32_t max_attempts = 2);

class RootFileHandle {
public:
    RootFileOpenResult Open(const std::string &file_path, const std::string &tree_name,
                            std::mutex *sync_mutex = nullptr);
    void Close();

    TFile *GetTFile() const { return file_.get(); }
    TTree *GetTTree() const { return tree_; }
    bool IsValid() const;

private:
    std::unique_ptr<TFile> file_;
    TTree *tree_ = nullptr;
    std::string tree_name_;
};

} // namespace duckdb::rootlake
