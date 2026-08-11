#include "include/root_file_opener.hpp"

#include "TTree.h"

#include "duckdb/common/exception.hpp"

#include <algorithm>
#include <chrono>
#include <exception>
#include <thread>

namespace duckdb::rootlake {

RootFileOpenResult OpenRootFile(const std::string &path, uint32_t max_attempts) {
    RootFileOpenResult result;
    max_attempts = std::max<uint32_t>(1, max_attempts);
    const auto started = std::chrono::steady_clock::now();
    for (uint32_t attempt = 1; attempt <= max_attempts; ++attempt) {
        result.attempts = attempt;
        try {
            result.file.reset(TFile::Open(path.c_str(), "READ"));
            if (result.file && !result.file->IsZombie()) break;
            result.file.reset();
            result.error = "TFile::Open returned no readable file";
        } catch (const std::exception &exception) {
            result.file.reset();
            result.error = exception.what();
        } catch (...) {
            result.file.reset();
            result.error = "unknown exception from TFile::Open";
        }
        if (attempt < max_attempts) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }
    const auto elapsed = std::chrono::steady_clock::now() - started;
    result.elapsed_us = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(elapsed).count());
    if (result) result.error = "ok";
    return result;
}

RootFileOpenResult RootFileHandle::Open(const std::string &file_path,
                                        const std::string &tree_name,
                                        std::mutex *sync_mutex) {
    tree_name_ = tree_name;
    RootFileOpenResult result;
    if (sync_mutex) {
        std::lock_guard<std::mutex> lock(*sync_mutex);
        result = OpenRootFile(file_path);
    } else {
        result = OpenRootFile(file_path);
    }
    file_ = std::move(result.file);
    if (!file_ || file_->IsZombie()) {
        throw IOException("Failed to open ROOT file " + file_path + ": " + result.error);
    }
    file_->GetObject(tree_name_.c_str(), tree_);
    if (!tree_) throw IOException("TTree not found: " + tree_name_ + " in " + file_path);
    return result;
}

void RootFileHandle::Close() {
    tree_ = nullptr;
    file_.reset();
    tree_name_.clear();
}

bool RootFileHandle::IsValid() const {
    return file_ && tree_ && !file_->IsZombie();
}

} // namespace duckdb::rootlake
