// Copyright (c) Wojciech Figat. All rights reserved.

#include "AssetBuildService.h"
#include "Engine/Content/Artifacts/ArtifactStore.h"
#include "Engine/Core/Types/DateTime.h"
#include "Engine/Platform/File.h"
#include "Engine/Platform/FileSystem.h"
#include "Engine/Platform/Platform.h"
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace
{
    bool IsTerminal(AssetBuildJobStatus status)
    {
        return status == AssetBuildJobStatus::Succeeded || status == AssetBuildJobStatus::Failed || status == AssetBuildJobStatus::Cancelled;
    }

    const char* StatusName(AssetBuildJobStatus status)
    {
        switch (status)
        {
        case AssetBuildJobStatus::Queued: return "queued";
        case AssetBuildJobStatus::Building: return "building";
        case AssetBuildJobStatus::Publishing: return "publishing";
        case AssetBuildJobStatus::Succeeded: return "succeeded";
        case AssetBuildJobStatus::Failed: return "failed";
        case AssetBuildJobStatus::Cancelled: return "cancelled";
        default: return "invalid";
        }
    }

    const char* PriorityName(AssetBuildJobPriority priority)
    {
        switch (priority)
        {
        case AssetBuildJobPriority::Background: return "background";
        case AssetBuildJobPriority::Foreground: return "foreground";
        default: return "normal";
        }
    }

    struct JobKeyHash
    {
        size_t operator()(const AssetBuildJobKey& key) const
        {
            return GetHash(key);
        }
    };

    std::string ProcessorKey(const StringView& value)
    {
        const StringAnsi utf8(value);
        return std::string(utf8.Get(), utf8.Length());
    }

    void AppendJsonEscaped(StringAnsi& output, const StringAnsiView& value)
    {
        for (int32 i = 0; i < value.Length(); i++)
        {
            const char c = value[i];
            if (c == '"') output += "\\\"";
            else if (c == '\\') output += "\\\\";
            else if (c == '\n') output += "\\n";
            else if (c == '\r') output += "\\r";
            else if (c == '\t') output += "\\t";
            else if (static_cast<byte>(c) >= 0x20) output += c;
        }
    }

    void AppendJsonEscaped(StringAnsi& output, const StringView& value)
    {
        const StringAnsi utf8(value);
        AppendJsonEscaped(output, utf8);
    }
}

struct AssetBuildSharedState
{
    AssetBuildJobRequest Request;
    Guid JobID = Guid::New();
    AssetCancellationSource Cancellation;
    std::atomic<AssetBuildJobStatus> Status { AssetBuildJobStatus::Queued };
    std::mutex CompletionMutex;
    std::condition_variable Completed;
    AssetBuildJobResult Result;
    std::unordered_set<uint64> Requesters;
    std::vector<std::shared_ptr<AssetBuildSharedState>> Dependencies;
    String LogPath;
    StringAnsi Log;
    std::chrono::steady_clock::time_point QueuedAt = std::chrono::steady_clock::now();
    std::chrono::steady_clock::time_point BuildStartedAt;
    std::chrono::steady_clock::time_point PublicationStartedAt;
};

class AssetBuildService::Impl
{
public:
    mutable std::mutex Mutex;
    std::mutex PublicationMutex;
    std::condition_variable Changed;
    std::unordered_map<AssetBuildJobKey, std::shared_ptr<AssetBuildSharedState>, JobKeyHash> Jobs;
    std::deque<std::shared_ptr<AssetBuildSharedState>> Queue;
    std::vector<std::thread> Workers;
    std::unordered_map<std::string, int32> ActiveProcessorClasses;
    std::unordered_set<std::string> ActiveSerialGroups;
    AssetBuildServiceLimits Limits;
    String LibraryRoot;
    uint64 NextRequester = 1;
    uint64 ActiveMemory = 0;
    int32 ActiveExternalTools = 0;
    int32 ActiveWorkers = 0;
    AssetBuildMetrics Metrics;
    bool Initialized = false;
    bool Stopping = false;

    void Worker();
    void FinishLocked(const std::shared_ptr<AssetBuildSharedState>& job, AssetBuildJobStatus status, const AssetPipelineDiagnostic& diagnostic);
    void EnqueueLocked(const std::shared_ptr<AssetBuildSharedState>& job);
    void WriteLogLocked(const std::shared_ptr<AssetBuildSharedState>& job, AssetBuildJobStatus status);
    bool CanRunLocked(const std::shared_ptr<AssetBuildSharedState>& job, AssetPipelineDiagnostic& dependencyFailure) const;
    void PruneLogsLocked();
};

bool AssetBuildRequestHandle::IsValid() const
{
    return _state != nullptr && _requester != 0;
}

AssetBuildJobStatus AssetBuildRequestHandle::GetStatus() const
{
    return _state ? _state->Status.load(std::memory_order_acquire) : AssetBuildJobStatus::Invalid;
}

bool AssetBuildRequestHandle::Wait(uint32 timeoutMilliseconds) const
{
    if (!_state)
        return false;
    std::unique_lock<std::mutex> lock(_state->CompletionMutex);
    if (timeoutMilliseconds == MAX_uint32)
    {
        _state->Completed.wait(lock, [this]() { return IsTerminal(_state->Status.load(std::memory_order_acquire)); });
        return true;
    }
    return _state->Completed.wait_for(lock, std::chrono::milliseconds(timeoutMilliseconds),
        [this]() { return IsTerminal(_state->Status.load(std::memory_order_acquire)); });
}

bool AssetBuildRequestHandle::TryGetResult(AssetBuildJobResult& result) const
{
    if (!_state || !IsTerminal(_state->Status.load(std::memory_order_acquire)))
        return false;
    std::lock_guard<std::mutex> lock(_state->CompletionMutex);
    result = _state->Result;
    return true;
}

AssetBuildService::AssetBuildService()
    : _impl(new Impl())
{
}

AssetBuildService::~AssetBuildService()
{
    Shutdown();
}

bool AssetBuildService::Initialize(const StringView& libraryRoot, const AssetBuildServiceLimits& limits, AssetPipelineDiagnostic& diagnostic)
{
    diagnostic = AssetPipelineDiagnostic();
    if (libraryRoot.IsEmpty() || limits.MaximumWorkers < 1 || limits.MaximumWorkers > 64 || limits.MaximumMemoryBytes == 0 ||
        limits.MaximumExternalTools < 0 || limits.MaximumLogFiles < 1)
    {
        diagnostic.Code = AssetPipelineDiagnosticCode::ResourceLimitExceeded;
        diagnostic.Stage = AssetPipelineDiagnosticStage::Configuration;
        diagnostic.SourcePath = libraryRoot;
        diagnostic.Message = TEXT("Asset build service limits or Library root are invalid.");
        return true;
    }
    if (ArtifactStore::EnsureLayout(libraryRoot, diagnostic))
        return true;
    std::lock_guard<std::mutex> lock(_impl->Mutex);
    if (_impl->Initialized)
    {
        diagnostic.Code = AssetPipelineDiagnosticCode::BuildFailed;
        diagnostic.Stage = AssetPipelineDiagnosticStage::Configuration;
        diagnostic.Message = TEXT("Asset build service is already initialized.");
        return true;
    }
    _impl->Limits = limits;
    _impl->LibraryRoot = libraryRoot;
    _impl->Stopping = false;
    _impl->Initialized = true;
    _impl->PruneLogsLocked();
    _impl->Workers.reserve(limits.MaximumWorkers);
    for (int32 i = 0; i < limits.MaximumWorkers; i++)
        _impl->Workers.emplace_back([impl = _impl.get()]() { impl->Worker(); });
    return false;
}

bool AssetBuildService::IsRunning() const
{
    std::lock_guard<std::mutex> lock(_impl->Mutex);
    return _impl->Initialized && !_impl->Stopping;
}

AssetBuildRequestHandle AssetBuildService::Request(const AssetBuildJobRequest& request)
{
    std::lock_guard<std::mutex> lock(_impl->Mutex);
    const uint64 requester = _impl->NextRequester++;
    auto fail = [&](AssetPipelineDiagnosticCode code, const StringView& message)
    {
        auto state = std::make_shared<AssetBuildSharedState>();
        state->Request = request;
        state->Requesters.insert(requester);
        state->Result.Key = request.Key;
        state->Result.AssetID = request.AssetID;
        state->Result.RefreshId = request.RefreshId;
        state->Result.Pass = request.Pass;
        state->Result.JobID = state->JobID;
        state->Result.Status = AssetBuildJobStatus::Failed;
        state->Result.Diagnostic.Code = code;
        state->Result.Diagnostic.Stage = AssetPipelineDiagnosticStage::Build;
        state->Result.Diagnostic.AssetGuid = request.AssetID;
        state->Result.Diagnostic.Message = message;
        state->Status.store(AssetBuildJobStatus::Failed, std::memory_order_release);
        return AssetBuildRequestHandle(state, requester);
    };
    if (!_impl->Initialized || _impl->Stopping)
        return fail(AssetPipelineDiagnosticCode::BuildCancelled, TEXT("Asset build service is not accepting requests."));
    _impl->Metrics.Requests++;
    if (request.RefreshId.IsValid() != (request.Pass != 0))
        return fail(AssetPipelineDiagnosticCode::InvalidSettingsCombination,
            TEXT("Asset build refresh context must provide both a refresh ID and a non-zero pass, or neither."));
    if (!request.Key.IsValid() || !request.AssetID.IsValid() || request.ProcessorClass.IsEmpty() || !request.Build.IsBinded() ||
        request.MemoryBytes > _impl->Limits.MaximumMemoryBytes || request.ExternalToolSlots < 0 || request.ExternalToolSlots > _impl->Limits.MaximumExternalTools ||
        request.ProcessorConcurrencyLimit < 1 || request.Priority > AssetBuildJobPriority::Foreground)
        return fail(AssetPipelineDiagnosticCode::ResourceLimitExceeded, TEXT("Asset build request identity, callback, or resource declaration is invalid."));

    auto existing = _impl->Jobs.find(request.Key);
    if (existing != _impl->Jobs.end())
    {
        const auto& state = existing->second;
        bool samePlan = state->Request.AssetID == request.AssetID && state->Request.ProcessorClass == request.ProcessorClass &&
            state->Request.MemoryBytes == request.MemoryBytes && state->Request.ExternalToolSlots == request.ExternalToolSlots &&
            state->Request.Dependencies.Count() == request.Dependencies.Count() && state->Request.ProcessorID == request.ProcessorID &&
            state->Request.SerialGroup == request.SerialGroup && state->Request.Target == request.Target && state->Request.OutputKinds == request.OutputKinds &&
            state->Request.KeyComponents.Count() == request.KeyComponents.Count();
        for (int32 i = 0; samePlan && i < request.Dependencies.Count(); i++)
            samePlan = state->Request.Dependencies[i] == request.Dependencies[i];
        for (int32 i = 0; samePlan && i < request.KeyComponents.Count(); i++)
            samePlan = state->Request.KeyComponents[i].Name == request.KeyComponents[i].Name &&
                state->Request.KeyComponents[i].Type == request.KeyComponents[i].Type && state->Request.KeyComponents[i].Value == request.KeyComponents[i].Value;
        if (!samePlan)
            return fail(AssetPipelineDiagnosticCode::BuildFailed, TEXT("An exact build key was reused for a different build plan."));
        if (!request.AllowTerminalDeduplication && IsTerminal(state->Status.load(std::memory_order_acquire)))
        {
            _impl->Jobs.erase(existing);
        }
        else
        {
            _impl->Metrics.DeduplicationHits++;
            state->Requesters.insert(requester);
            if (state->Status.load(std::memory_order_acquire) == AssetBuildJobStatus::Queued && request.Priority > state->Request.Priority)
            {
                const auto queued = std::find(_impl->Queue.begin(), _impl->Queue.end(), state);
                ASSERT(queued != _impl->Queue.end());
                if (queued != _impl->Queue.end())
                {
                    _impl->Queue.erase(queued);
                    state->Request.Priority = request.Priority;
                    _impl->EnqueueLocked(state);
                    _impl->Changed.notify_all();
                }
            }
            return AssetBuildRequestHandle(state, requester);
        }
    }

    auto state = std::make_shared<AssetBuildSharedState>();
    state->Request = request;
    state->Requesters.insert(requester);
    state->Result.Key = request.Key;
    state->Result.AssetID = request.AssetID;
    state->Result.RefreshId = request.RefreshId;
    state->Result.Pass = request.Pass;
    state->Result.JobID = state->JobID;
    state->Result.Status = AssetBuildJobStatus::Queued;
    for (const AssetBuildJobKey& dependency : request.Dependencies)
    {
        if (!dependency.IsValid() || dependency == request.Key)
            return fail(AssetPipelineDiagnosticCode::BuildCycle, TEXT("Asset build request contains an invalid self dependency."));
        const auto found = _impl->Jobs.find(dependency);
        if (found == _impl->Jobs.end())
            return fail(AssetPipelineDiagnosticCode::BuildFailed, TEXT("Asset build dependencies must be submitted in topological order."));
        state->Dependencies.push_back(found->second);
    }
    ArtifactStoragePath logPath;
    AssetPipelineDiagnostic ignored;
    const String jobId = state->JobID.ToString(Guid::FormatType::N);
    if (!ArtifactStore::TryGetJobLogPath(_impl->LibraryRoot, jobId, logPath, ignored))
        state->LogPath = logPath.Get();
    _impl->Jobs.emplace(request.Key, state);
    _impl->EnqueueLocked(state);
    _impl->WriteLogLocked(state, AssetBuildJobStatus::Queued);
    _impl->Changed.notify_all();
    return AssetBuildRequestHandle(state, requester);
}

void AssetBuildService::CancelRequester(const AssetBuildRequestHandle& handle)
{
    if (!handle.IsValid())
        return;
    std::lock_guard<std::mutex> lock(_impl->Mutex);
    const auto found = handle._state->Requesters.find(handle._requester);
    if (found == handle._state->Requesters.end())
        return;
    handle._state->Requesters.erase(found);
    if (handle._state->Requesters.empty() && !IsTerminal(handle._state->Status.load(std::memory_order_acquire)))
        handle._state->Cancellation.Cancel();
    _impl->Changed.notify_all();
}

void AssetBuildService::Impl::EnqueueLocked(const std::shared_ptr<AssetBuildSharedState>& job)
{
    const auto position = std::find_if(Queue.begin(), Queue.end(), [&job](const std::shared_ptr<AssetBuildSharedState>& queued)
    {
        return queued->Request.Priority < job->Request.Priority;
    });
    Queue.insert(position, job);
}

void AssetBuildService::Impl::WriteLogLocked(const std::shared_ptr<AssetBuildSharedState>& job, AssetBuildJobStatus status)
{
    if (job->LogPath.IsEmpty())
        return;
    StringAnsi line("{\"schemaVersion\":1,\"recordType\":\"jobEvent\",\"jobId\":\"");
    line += StringAnsi(job->JobID.ToString(Guid::FormatType::N));
    line += "\",\"assetId\":\"";
    line += StringAnsi(job->Request.AssetID.ToString(Guid::FormatType::N));
    line += "\",\"refreshId\":\"";
    line += job->Request.RefreshId.IsValid() ? StringAnsi(job->Request.RefreshId.ToString(Guid::FormatType::N)) : StringAnsi();
    line += "\",\"pass\":";
    line += StringAnsi::Format("{0}", job->Request.Pass);
    line += ",\"key\":\"";
    line += job->Request.Key.ExactPlan.ToString();
    line += "\",\"processorId\":\"";
    AppendJsonEscaped(line, job->Request.ProcessorID);
    line += "\",\"target\":\"";
    AppendJsonEscaped(line, job->Request.Target);
    line += "\",\"stage\":\"";
    line += StatusName(status);
    line += "\",\"priority\":\"";
    line += PriorityName(job->Request.Priority);
    line += "\",\"rebuildReason\":\"";
    AppendJsonEscaped(line, job->Request.RebuildReason);
    line += "\",\"outputKinds\":[";
    for (int32 i = 0; i < job->Request.OutputKinds.Count(); i++)
    {
        if (i != 0)
            line += ',';
        line += '"';
        AppendJsonEscaped(line, job->Request.OutputKinds[i]);
        line += '"';
    }
    line += ']';
    if (status == AssetBuildJobStatus::Queued && job->Request.KeyComponents.HasItems())
    {
        line += ",\"keyComponents\":[";
        for (int32 i = 0; i < job->Request.KeyComponents.Count(); i++)
        {
            if (i != 0)
                line += ',';
            const ArtifactKeyComponent& component = job->Request.KeyComponents[i];
            line += "{\"name\":\"";
            AppendJsonEscaped(line, component.Name);
            line += "\",\"type\":\"";
            AppendJsonEscaped(line, component.Type);
            line += "\",\"value\":\"";
            AppendJsonEscaped(line, AssetBuildDiagnostics::RedactAbsolutePath(component.Value));
            line += "\"}";
        }
        line += ']';
    }
    line += "}\n";
    job->Log += line;
    if (IsTerminal(status) && job->Result.Diagnostic.Code != AssetPipelineDiagnosticCode::None)
    {
        AssetPipelineDiagnostic persisted = job->Result.Diagnostic;
        const StringAnsi redactedSource = AssetBuildDiagnostics::RedactAbsolutePath(StringAnsi(persisted.SourcePath));
        const StringAnsi redactedFile = AssetBuildDiagnostics::RedactAbsolutePath(StringAnsi(persisted.Location.File));
        persisted.SourcePath = String(redactedSource);
        persisted.Location.File = String(redactedFile);
        StringAnsi diagnosticJson;
        AssetPipelineDiagnostic ignored;
        if (!AssetBuildDiagnostics::DiagnosticToJson(persisted, diagnosticJson, ignored))
        {
            job->Log += diagnosticJson;
            job->Log += '\n';
        }
    }
    File::WriteAllBytes(job->LogPath, job->Log.Get(), job->Log.Length());
}

void AssetBuildService::Impl::FinishLocked(const std::shared_ptr<AssetBuildSharedState>& job, AssetBuildJobStatus status, const AssetPipelineDiagnostic& diagnostic)
{
    AssetPipelineDiagnostic resultDiagnostic = diagnostic;
    if (status == AssetBuildJobStatus::Cancelled)
        resultDiagnostic.Severity = AssetPipelineDiagnosticSeverity::Info;
    if (status == AssetBuildJobStatus::Succeeded)
        Metrics.Succeeded++;
    else if (status == AssetBuildJobStatus::Failed)
        Metrics.Failed++;
    else if (status == AssetBuildJobStatus::Cancelled)
        Metrics.Cancelled++;
    {
        std::lock_guard<std::mutex> completionLock(job->CompletionMutex);
        job->Result.Status = status;
        job->Result.Diagnostic = MoveTemp(resultDiagnostic);
        job->Status.store(status, std::memory_order_release);
    }
    WriteLogLocked(job, status);
    job->Completed.notify_all();
    Changed.notify_all();
}

bool AssetBuildService::Impl::CanRunLocked(const std::shared_ptr<AssetBuildSharedState>& job, AssetPipelineDiagnostic& dependencyFailure) const
{
    dependencyFailure = AssetPipelineDiagnostic();
    if (job->Cancellation.GetToken().IsCancellationRequested())
    {
        dependencyFailure.Code = AssetPipelineDiagnosticCode::BuildCancelled;
        dependencyFailure.Stage = AssetPipelineDiagnosticStage::Build;
        dependencyFailure.AssetGuid = job->Request.AssetID;
        dependencyFailure.Message = TEXT("Asset build has no active requester or service shutdown was requested.");
        return false;
    }
    for (const auto& dependency : job->Dependencies)
    {
        const AssetBuildJobStatus status = dependency->Status.load(std::memory_order_acquire);
        if (status == AssetBuildJobStatus::Failed || status == AssetBuildJobStatus::Cancelled)
        {
            dependencyFailure.Code = status == AssetBuildJobStatus::Cancelled ? AssetPipelineDiagnosticCode::BuildCancelled : AssetPipelineDiagnosticCode::BuildFailed;
            dependencyFailure.Stage = AssetPipelineDiagnosticStage::Build;
            dependencyFailure.AssetGuid = job->Request.AssetID;
            dependencyFailure.Message = TEXT("An exact build dependency did not complete successfully.");
            return false;
        }
        if (status != AssetBuildJobStatus::Succeeded)
            return false;
    }
    if (ActiveMemory + job->Request.MemoryBytes > Limits.MaximumMemoryBytes ||
        ActiveExternalTools + job->Request.ExternalToolSlots > Limits.MaximumExternalTools)
        return false;
    if (job->Request.SerialGroup.HasChars() && ActiveSerialGroups.find(ProcessorKey(job->Request.SerialGroup)) != ActiveSerialGroups.end())
        return false;
    const std::string processor = ProcessorKey(job->Request.ProcessorClass);
    const auto active = ActiveProcessorClasses.find(processor);
    return active == ActiveProcessorClasses.end() || active->second < job->Request.ProcessorConcurrencyLimit;
}

void AssetBuildService::Impl::PruneLogsLocked()
{
    Array<String> files;
    const String logsPath = ArtifactStore::GetLogsPath(LibraryRoot);
    if (FileSystem::DirectoryGetFiles(files, logsPath, TEXT("*.jsonl"), DirectorySearchOption::TopDirectoryOnly) || files.Count() < Limits.MaximumLogFiles)
        return;
    std::sort(files.Get(), files.Get() + files.Count(), [](const String& a, const String& b)
    {
        const DateTime aTime = FileSystem::GetFileLastEditTime(a);
        const DateTime bTime = FileSystem::GetFileLastEditTime(b);
        if (aTime != bTime)
            return aTime < bTime;
        return a < b;
    });
    const int32 removeCount = files.Count() - Limits.MaximumLogFiles + 1;
    for (int32 i = 0; i < removeCount; i++)
        FileSystem::DeleteFile(files[i]);
}

AssetBuildMetrics AssetBuildService::GetMetrics() const
{
    std::lock_guard<std::mutex> lock(_impl->Mutex);
    return _impl->Metrics;
}

void AssetBuildService::GetJobs(Array<AssetBuildJobSummary>& jobs) const
{
    jobs.Clear();
    std::lock_guard<std::mutex> lock(_impl->Mutex);
    jobs.EnsureCapacity(static_cast<int32>(_impl->Jobs.size()));
    for (const auto& entry : _impl->Jobs)
    {
        const auto& state = entry.second;
        AssetBuildJobSummary summary;
        summary.JobID = state->JobID;
        summary.AssetID = state->Request.AssetID;
        summary.RefreshId = state->Request.RefreshId;
        summary.Pass = state->Request.Pass;
        summary.Key = state->Request.Key.ExactPlan;
        summary.ProcessorID = state->Request.ProcessorID;
        summary.Target = state->Request.Target;
        summary.RebuildReason = state->Request.RebuildReason;
        summary.Status = static_cast<byte>(state->Status.load(std::memory_order_acquire));
        summary.Diagnostic = state->Result.Diagnostic;
        jobs.Add(MoveTemp(summary));
    }
}

void AssetBuildService::Impl::Worker()
{
    // Use otherwise-idle CPU capacity without competing with the Editor interaction threads.
    Platform::SetThreadPriority(ThreadPriority::BelowNormal);
    for (;;)
    {
        std::shared_ptr<AssetBuildSharedState> job;
        {
            std::unique_lock<std::mutex> lock(Mutex);
            for (;;)
            {
                if (Stopping)
                    return;
                for (size_t i = 0; i < Queue.size();)
                {
                    AssetPipelineDiagnostic dependencyFailure;
                    if (CanRunLocked(Queue[i], dependencyFailure))
                    {
                        job = Queue[i];
                        Queue.erase(Queue.begin() + i);
                        break;
                    }
                    if (dependencyFailure.Code != AssetPipelineDiagnosticCode::None)
                    {
                        const auto rejected = Queue[i];
                        Queue.erase(Queue.begin() + i);
                        FinishLocked(rejected,
                            dependencyFailure.Code == AssetPipelineDiagnosticCode::BuildCancelled ? AssetBuildJobStatus::Cancelled : AssetBuildJobStatus::Failed,
                            dependencyFailure);
                        continue;
                    }
                    i++;
                }
                if (job)
                    break;
                Changed.wait(lock);
            }
            ActiveMemory += job->Request.MemoryBytes;
            ActiveExternalTools += job->Request.ExternalToolSlots;
            ActiveWorkers++;
            ActiveProcessorClasses[ProcessorKey(job->Request.ProcessorClass)]++;
            if (job->Request.SerialGroup.HasChars())
                ActiveSerialGroups.insert(ProcessorKey(job->Request.SerialGroup));
            job->BuildStartedAt = std::chrono::steady_clock::now();
            Metrics.QueueWaitMilliseconds += std::chrono::duration_cast<std::chrono::milliseconds>(job->BuildStartedAt - job->QueuedAt).count();
            Metrics.BuildsStarted++;
            Metrics.PeakMemoryBytes = Math::Max(Metrics.PeakMemoryBytes, ActiveMemory);
            Metrics.PeakWorkers = Math::Max(Metrics.PeakWorkers, ActiveWorkers);
            job->Status.store(AssetBuildJobStatus::Building, std::memory_order_release);
            job->Result.Status = AssetBuildJobStatus::Building;
            WriteLogLocked(job, AssetBuildJobStatus::Building);
        }

        AssetPipelineDiagnostic diagnostic;
        const AssetCancellationToken token = job->Cancellation.GetToken();
        bool failed = job->Request.Build(token, diagnostic);
        const uint64 buildElapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - job->BuildStartedAt).count();
        const bool cancelledAfterBuild = token.IsCancellationRequested();

        auto releaseResources = [&]()
        {
            ActiveMemory -= job->Request.MemoryBytes;
            ActiveExternalTools -= job->Request.ExternalToolSlots;
            ActiveWorkers--;
            const std::string processor = ProcessorKey(job->Request.ProcessorClass);
            const auto active = ActiveProcessorClasses.find(processor);
            ASSERT(active != ActiveProcessorClasses.end() && active->second > 0);
            if (--active->second == 0)
                ActiveProcessorClasses.erase(active);
            if (job->Request.SerialGroup.HasChars())
                ActiveSerialGroups.erase(ProcessorKey(job->Request.SerialGroup));
        };

        if (failed || cancelledAfterBuild)
        {
            std::lock_guard<std::mutex> lock(Mutex);
            Metrics.BuildMilliseconds += buildElapsed;
            releaseResources();
            if (cancelledAfterBuild)
            {
                diagnostic.Code = AssetPipelineDiagnosticCode::BuildCancelled;
                diagnostic.Stage = AssetPipelineDiagnosticStage::Build;
                diagnostic.AssetGuid = job->Request.AssetID;
                diagnostic.Message = TEXT("Asset build was cancelled before publication.");
                FinishLocked(job, AssetBuildJobStatus::Cancelled, diagnostic);
            }
            else
            {
                if (diagnostic.Code == AssetPipelineDiagnosticCode::None)
                {
                    diagnostic.Code = AssetPipelineDiagnosticCode::BuildFailed;
                    diagnostic.Stage = AssetPipelineDiagnosticStage::Build;
                    diagnostic.AssetGuid = job->Request.AssetID;
                    diagnostic.Message = TEXT("Asset build callback failed without a diagnostic.");
                }
                FinishLocked(job, AssetBuildJobStatus::Failed, diagnostic);
            }
            continue;
        }

        std::unique_lock<std::mutex> publicationLock(PublicationMutex);
        {
            std::lock_guard<std::mutex> lock(Mutex);
            Metrics.BuildMilliseconds += buildElapsed;
            if (Stopping || token.IsCancellationRequested())
            {
                releaseResources();
                diagnostic.Code = AssetPipelineDiagnosticCode::BuildCancelled;
                diagnostic.Stage = AssetPipelineDiagnosticStage::Publication;
                diagnostic.AssetGuid = job->Request.AssetID;
                diagnostic.Message = TEXT("Asset build publication was stopped safely.");
                FinishLocked(job, AssetBuildJobStatus::Cancelled, diagnostic);
                continue;
            }
            job->Status.store(AssetBuildJobStatus::Publishing, std::memory_order_release);
            job->Result.Status = AssetBuildJobStatus::Publishing;
            job->PublicationStartedAt = std::chrono::steady_clock::now();
            Metrics.PublicationsStarted++;
            WriteLogLocked(job, AssetBuildJobStatus::Publishing);
        }
        failed = job->Request.Publish.IsBinded() && job->Request.Publish(token, diagnostic);
        {
            std::lock_guard<std::mutex> lock(Mutex);
            Metrics.PublicationMilliseconds += std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - job->PublicationStartedAt).count();
            releaseResources();
            if (token.IsCancellationRequested())
            {
                diagnostic.Code = AssetPipelineDiagnosticCode::BuildCancelled;
                diagnostic.Stage = AssetPipelineDiagnosticStage::Publication;
                diagnostic.AssetGuid = job->Request.AssetID;
                diagnostic.Message = TEXT("Asset build publication was cancelled.");
                FinishLocked(job, AssetBuildJobStatus::Cancelled, diagnostic);
            }
            else if (failed)
            {
                if (diagnostic.Code == AssetPipelineDiagnosticCode::None)
                {
                    diagnostic.Code = AssetPipelineDiagnosticCode::BuildFailed;
                    diagnostic.Stage = AssetPipelineDiagnosticStage::Publication;
                    diagnostic.AssetGuid = job->Request.AssetID;
                    diagnostic.Message = TEXT("Asset publication callback failed without a diagnostic.");
                }
                FinishLocked(job, AssetBuildJobStatus::Failed, diagnostic);
            }
            else
            {
                FinishLocked(job, AssetBuildJobStatus::Succeeded, AssetPipelineDiagnostic());
            }
        }
    }
}

void AssetBuildService::Shutdown()
{
    {
        std::unique_lock<std::mutex> publicationLock(_impl->PublicationMutex);
        std::lock_guard<std::mutex> lock(_impl->Mutex);
        if (!_impl->Initialized || _impl->Stopping)
            return;
        _impl->Stopping = true;
        for (auto& entry : _impl->Jobs)
        {
            const auto& job = entry.second;
            const AssetBuildJobStatus status = job->Status.load(std::memory_order_acquire);
            if (!IsTerminal(status))
                job->Cancellation.Cancel();
            if (status == AssetBuildJobStatus::Queued)
            {
                AssetPipelineDiagnostic diagnostic;
                diagnostic.Code = AssetPipelineDiagnosticCode::BuildCancelled;
                diagnostic.Stage = AssetPipelineDiagnosticStage::Build;
                diagnostic.AssetGuid = job->Request.AssetID;
                diagnostic.Message = TEXT("Asset build was cancelled during service shutdown.");
                _impl->FinishLocked(job, AssetBuildJobStatus::Cancelled, diagnostic);
            }
        }
        _impl->Queue.clear();
    }
    _impl->Changed.notify_all();
    for (std::thread& worker : _impl->Workers)
    {
        if (worker.joinable())
            worker.join();
    }
    std::lock_guard<std::mutex> lock(_impl->Mutex);
    _impl->Workers.clear();
    _impl->Jobs.clear();
    _impl->ActiveProcessorClasses.clear();
    _impl->ActiveMemory = 0;
    _impl->ActiveExternalTools = 0;
    _impl->Initialized = false;
}
