#include "root4duckdb/direct/root_scan_internal.hpp"

namespace duckdb
{

void RootScanFileManager::Reset(RootScanGlobalState& gstate, RootScanLocalState& local_state, bool completed) const
{
    if (completed && local_state.file_active && gstate.file_scheduler)
    {
        const auto elapsed = std::chrono::steady_clock::now() - local_state.file_started;
        auto* file = local_state.root_file.GetTFile();
        const uint64_t read_calls =
            file ? static_cast<uint64_t>(std::max<Long64_t>(0, file->GetReadCalls())) : 0;
        const uint64_t read_bytes =
            file ? static_cast<uint64_t>(std::max<Long64_t>(0, file->GetBytesRead())) : 0;
        gstate.file_scheduler->RecordComplete(
            local_state.file_task,
            static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(elapsed).count()),
            read_calls, read_bytes);
    }
    local_state.serialized_columns.clear();
    local_state.cached_results.clear();
    local_state.has_cached_entry = false;
    local_state.current_entry = std::numeric_limits<uint64_t>::max();
    local_state.current_elem_idx = 0;
    local_state.root_readers.clear();
    local_state.direct_branch = nullptr;
    local_state.direct_leaf = nullptr;
    local_state.primitive_tree_branches.clear();
    local_state.primitive_tree_leaves.clear();
    local_state.primitive_tree_requires_read = false;
    local_state.root_file.Close();
    local_state.local_current_row = 0;
    local_state.local_end_row = 0;
    local_state.has_container_columns = false;
    local_state.file_active = false;
}

bool RootScanFileManager::EnsureReady(const RootScanBindData& bind_data, RootScanGlobalState& gstate,
                                      RootScanLocalState& local_state) const
{
    if (!gstate.file_scheduler)
    {
        throw InternalException("multi-file ROOT scheduler is unavailable");
    }
    while (true)
    {
        if (local_state.file_active &&
            (local_state.has_cached_entry || local_state.local_current_row < local_state.local_end_row))
        {
            return true;
        }
        if (local_state.file_active)
        {
            Reset(gstate, local_state, true);
        }
        rootlake::RootDirectFileTask task;
        if (!gstate.file_scheduler->Claim(task))
        {
            if (gstate.file_scheduler->AllFilesFinished())
            {
                const auto failures = gstate.file_scheduler->FailureSummary();
                if (!failures.empty())
                {
                    throw IOException(failures);
                }
            }
            return false;
        }
        local_state.file_task = std::move(task);
        local_state.file_started = std::chrono::steady_clock::now();
        try
        {
            Open(bind_data, gstate, local_state, local_state.file_task.path, false);
        }
        catch (const rootlake::RootFileUnavailableException& exception)
        {
            gstate.file_scheduler->RecordUnavailable(local_state.file_task, exception.Attempts(),
                                                     exception.ElapsedUs());
            Reset(gstate, local_state, false);
            throw IOException("Required ROOT input is unavailable: " + local_state.file_task.path + ": " +
                              exception.what());
        }
        catch (const std::exception& exception)
        {
            gstate.file_scheduler->RecordFailure(local_state.file_task, exception.what());
            Reset(gstate, local_state, false);
            throw IOException("Required ROOT input failed: " + local_state.file_task.path + ": " +
                              exception.what());
        }
        catch (...)
        {
            gstate.file_scheduler->RecordFailure(local_state.file_task, "unknown ROOT reader error");
            Reset(gstate, local_state, false);
            throw IOException("Required ROOT input failed: " + local_state.file_task.path +
                              ": unknown ROOT reader error");
        }
    }
}

} // namespace duckdb
