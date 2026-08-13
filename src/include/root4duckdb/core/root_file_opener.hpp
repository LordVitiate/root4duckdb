#pragma once

#include "root4duckdb/core/root_headers.hpp"

#include <cstdint>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <utility>

class TTree;

namespace duckdb::rootlake {

/// Result and retry telemetry for a ROOT file open.
struct RootFileOpenResult {
    std::unique_ptr<TFile> file;
    uint32_t attempts = 0;
    uint64_t elapsed_us = 0;
    std::string error;

    explicit operator bool() const;
};

/// Signals that a remote or local ROOT file exhausted its open budget.
class RootFileUnavailableException : public std::runtime_error {
  public:
    RootFileUnavailableException(std::string message, uint32_t attempts, uint64_t elapsed_us);

    uint32_t attempts;
    uint64_t elapsed_us;
};

/// Opens a file with a deliberately small transient retry budget.
RootFileOpenResult OpenRootFile(const std::string& path, uint32_t max_attempts = 2);

/// Owns one opened file and its selected tree.
class RootFileHandle {
  public:
    /// Replaces the current file and resolves the requested tree.
    RootFileOpenResult Open(const std::string& file_path, const std::string& tree_name,
                            std::mutex* sync_mutex = nullptr);
    /// Releases the current file and tree.
    void Close();

    TFile* GetTFile() const;
    TTree* GetTTree() const;
    bool IsValid() const;

  private:
    std::unique_ptr<TFile> file_;
    TTree* tree_ = nullptr;
    std::string tree_name_;
};

} // namespace duckdb::rootlake
