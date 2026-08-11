#pragma once

#include "duckdb/common/common.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <limits>
#include <mutex>
#include <string>
#include <unordered_set>
#include <vector>

namespace duckdb::rootlake {

struct RootDirectFileTask {
    idx_t source_id = 0;
    std::string path;
};

class RootDirectFileScheduler {
public:
    RootDirectFileScheduler(const std::vector<std::string> &files, idx_t worker_limit);

    bool Claim(RootDirectFileTask &task);
    void SetSourceRange(uint64_t lower, uint64_t upper);
    idx_t MaxThreads() const { return worker_limit_; }

    void RecordOpen(const RootDirectFileTask &task, uint32_t attempts, uint64_t elapsed_us);
    void RecordFailure(const RootDirectFileTask &task, const std::string &message);
    void RecordComplete(const RootDirectFileTask &task, uint64_t elapsed_us);
    void ObserveSchema(const std::string &fingerprint);
    void RecordFirstRow();

    uint64_t OpenedFiles() const { return opened_files_.load(); }
    uint64_t FailedFiles() const { return failed_files_.load(); }
    uint64_t RetriedOpens() const { return retried_opens_.load(); }
    uint64_t OpenTimeUs() const { return open_time_us_.load(); }
    uint64_t CompletedFiles() const { return completed_files_.load(); }
    uint64_t SkippedFiles() const { return skipped_files_.load(); }
    uint64_t SchemaVariants() const;
    uint64_t SchemaPlanReuses() const { return schema_plan_reuses_.load(); }
    uint64_t FirstRowUs() const { return first_row_us_.load(); }
    bool AllFilesFinished() const;
    std::string SlowestFile() const;
    uint64_t SlowestFileUs() const;
    std::string FailureSummary() const;

private:
    std::vector<std::string> files_;
    idx_t worker_limit_ = 1;
    std::atomic<idx_t> next_file_ {0};
    std::atomic<uint64_t> opened_files_ {0};
    std::atomic<uint64_t> failed_files_ {0};
    std::atomic<uint64_t> retried_opens_ {0};
    std::atomic<uint64_t> open_time_us_ {0};
    std::atomic<uint64_t> completed_files_ {0};
    std::atomic<uint64_t> skipped_files_ {0};
    std::atomic<uint64_t> schema_plan_reuses_ {0};
    std::atomic<uint64_t> first_row_us_ {0};
    uint64_t source_lower_ = 0;
    uint64_t source_upper_ = std::numeric_limits<uint64_t>::max();
    std::chrono::steady_clock::time_point started_;
    mutable std::mutex mutex_;
    std::unordered_set<std::string> schema_fingerprints_;
    std::vector<std::string> failures_;
    std::string slowest_file_;
    uint64_t slowest_file_us_ = 0;
};

} // namespace duckdb::rootlake
