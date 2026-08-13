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

/// One source claimed by a direct-scan worker.
struct RootDirectFileTask {
    idx_t source_id = 0;
    std::string path;
};

/// Coordinates file claims and direct-scan telemetry.
class RootDirectFileScheduler {
  public:
    RootDirectFileScheduler(const std::vector<std::string>& files, idx_t worker_limit);

    /// Claims the next source allowed by the current source range.
    bool Claim(RootDirectFileTask& task);
    /// Applies inclusive source-id pruning before file opens.
    void SetSourceRange(uint64_t lower, uint64_t upper);
    /// Returns the configured worker ceiling.
    idx_t MaxThreads() const;

    /// Records one successful file open and its retries.
    void RecordOpen(const RootDirectFileTask& task, uint32_t attempts, uint64_t elapsed_us);
    /// Records a file that exhausted its open retry budget.
    void RecordUnavailable(const RootDirectFileTask& task, uint32_t attempts, uint64_t elapsed_us);
    /// Records a file-level scan failure.
    void RecordFailure(const RootDirectFileTask& task, const std::string& message);
    /// Records successful completion and elapsed scan time.
    void RecordComplete(const RootDirectFileTask& task, uint64_t elapsed_us);
    /// Tracks schema reuse across source files.
    void ObserveSchema(const std::string& fingerprint);
    /// Captures time to the first materialized output row.
    void RecordFirstRow();

    uint64_t OpenedFiles() const;
    uint64_t FailedFiles() const;
    uint64_t UnavailableFiles() const;
    uint64_t RetriedOpens() const;
    uint64_t OpenTimeUs() const;
    uint64_t CompletedFiles() const;
    uint64_t SkippedFiles() const;
    uint64_t SchemaVariants() const;
    uint64_t SchemaPlanReuses() const;
    uint64_t FirstRowUs() const;
    bool AllFilesFinished() const;
    std::string SlowestFile() const;
    uint64_t SlowestFileUs() const;
    std::string FailureSummary() const;

  private:
    std::vector<std::string> files_;
    idx_t worker_limit_ = 1;
    std::atomic<idx_t> next_file_{0};
    std::atomic<uint64_t> opened_files_{0};
    std::atomic<uint64_t> failed_files_{0};
    std::atomic<uint64_t> unavailable_files_{0};
    std::atomic<uint64_t> retried_opens_{0};
    std::atomic<uint64_t> open_time_us_{0};
    std::atomic<uint64_t> completed_files_{0};
    std::atomic<uint64_t> skipped_files_{0};
    std::atomic<uint64_t> schema_plan_reuses_{0};
    std::atomic<uint64_t> first_row_us_{0};
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
