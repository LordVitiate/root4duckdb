#include "root4duckdb/direct/root_direct_scheduler.hpp"
#include "root4duckdb/core/root_text.hpp"

#include <algorithm>
#include <chrono>
#include <sstream>

namespace duckdb::rootlake
{

RootDirectFileScheduler::RootDirectFileScheduler(const std::vector<std::string>& files, idx_t worker_limit)
    : files_(files), worker_limit_(std::max<idx_t>(1, std::min<idx_t>(worker_limit, files.size()))),
      started_(std::chrono::steady_clock::now())
{
}

idx_t RootDirectFileScheduler::MaxThreads() const
{
    return worker_limit_;
}

uint64_t RootDirectFileScheduler::OpenedFiles() const
{
    return opened_files_.load();
}

uint64_t RootDirectFileScheduler::FailedFiles() const
{
    return failed_files_.load();
}

uint64_t RootDirectFileScheduler::UnavailableFiles() const
{
    return unavailable_files_.load();
}

uint64_t RootDirectFileScheduler::RetriedOpens() const
{
    return retried_opens_.load();
}

uint64_t RootDirectFileScheduler::OpenTimeUs() const
{
    return open_time_us_.load();
}

uint64_t RootDirectFileScheduler::CompletedFiles() const
{
    return completed_files_.load();
}

uint64_t RootDirectFileScheduler::SkippedFiles() const
{
    return skipped_files_.load();
}

uint64_t RootDirectFileScheduler::SchemaPlanReuses() const
{
    return schema_plan_reuses_.load();
}

uint64_t RootDirectFileScheduler::FirstRowUs() const
{
    return first_row_us_.load();
}

bool RootDirectFileScheduler::Claim(RootDirectFileTask& task)
{
    while (true)
    {
        const auto index = next_file_.fetch_add(1);
        if (index >= files_.size())
        {
            return false;
        }
        if (index < source_lower_ || index > source_upper_)
        {
            skipped_files_.fetch_add(1);
            continue;
        }
        task.source_id = index;
        task.path = files_[index];
        return true;
    }
}

void RootDirectFileScheduler::SetSourceRange(uint64_t lower, uint64_t upper)
{
    source_lower_ = lower;
    source_upper_ = upper;
}

void RootDirectFileScheduler::RecordOpen(const RootDirectFileTask&, uint32_t attempts, uint64_t elapsed_us)
{
    opened_files_.fetch_add(1);
    open_time_us_.fetch_add(elapsed_us);
    if (attempts > 1)
    {
        retried_opens_.fetch_add(attempts - 1);
    }
}

void RootDirectFileScheduler::RecordUnavailable(const RootDirectFileTask&, uint32_t attempts, uint64_t elapsed_us)
{
    unavailable_files_.fetch_add(1);
    open_time_us_.fetch_add(elapsed_us);
    if (attempts > 1)
    {
        retried_opens_.fetch_add(attempts - 1);
    }
}

void RootDirectFileScheduler::RecordFailure(const RootDirectFileTask& task, const std::string& message)
{
    failed_files_.fetch_add(1);
    const std::lock_guard<std::mutex> lock(mutex_);
    failures_.push_back(task.path + ": " + message);
}

void RootDirectFileScheduler::RecordComplete(const RootDirectFileTask& task, uint64_t elapsed_us)
{
    completed_files_.fetch_add(1);
    const std::lock_guard<std::mutex> lock(mutex_);
    if (elapsed_us > slowest_file_us_)
    {
        slowest_file_us_ = elapsed_us;
        slowest_file_ = task.path;
    }
}

void RootDirectFileScheduler::ObserveSchema(const std::string& fingerprint)
{
    if (fingerprint.empty())
    {
        return;
    }
    const std::lock_guard<std::mutex> lock(mutex_);
    if (!schema_fingerprints_.insert(fingerprint).second)
    {
        schema_plan_reuses_.fetch_add(1);
    }
}

void RootDirectFileScheduler::RecordFirstRow()
{
    auto expected = uint64_t(0);
    const auto elapsed = std::chrono::steady_clock::now() - started_;
    auto value = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(elapsed).count());
    if (value == 0)
    {
        value = 1;
    }
    first_row_us_.compare_exchange_strong(expected, value);
}

bool RootDirectFileScheduler::AllFilesFinished() const
{
    return completed_files_.load() + failed_files_.load() + unavailable_files_.load() + skipped_files_.load() >=
           files_.size();
}

uint64_t RootDirectFileScheduler::SchemaVariants() const
{
    const std::lock_guard<std::mutex> lock(mutex_);
    return schema_fingerprints_.size();
}

std::string RootDirectFileScheduler::SlowestFile() const
{
    const std::lock_guard<std::mutex> lock(mutex_);
    return slowest_file_;
}

uint64_t RootDirectFileScheduler::SlowestFileUs() const
{
    const std::lock_guard<std::mutex> lock(mutex_);
    return slowest_file_us_;
}

std::string RootDirectFileScheduler::FailureSummary() const
{
    const std::lock_guard<std::mutex> lock(mutex_);
    if (failures_.empty())
    {
        return {};
    }
    std::ostringstream result;
    result << failures_.size() << " ROOT input(s) failed";
    return RootOpenFailureMessage(result.str(), failures_);
}

} // namespace duckdb::rootlake
