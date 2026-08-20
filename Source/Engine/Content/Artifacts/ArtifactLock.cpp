// Copyright (c) Wojciech Figat. All rights reserved.

#include "ArtifactLock.h"
#include "Engine/Content/Documents/CanonicalJsonWriter.h"
#include "Engine/Core/Types/DateTime.h"
#include "Engine/Platform/File.h"
#include "Engine/Platform/FileSystem.h"
#include "Engine/Platform/Platform.h"
#include <chrono>
#if PLATFORM_WINDOWS
#include "Engine/Platform/Win32/IncludeWindowsHeaders.h"
#endif

namespace
{
    typedef rapidjson_flax::Document JsonDocument;
    typedef rapidjson_flax::Value JsonValue;

    bool LockFail(AssetPipelineDiagnostic& diagnostic, AssetPipelineDiagnosticCode code, const StringView& path, const StringView& message)
    {
        diagnostic = AssetPipelineDiagnostic();
        diagnostic.Code = code;
        diagnostic.Stage = AssetPipelineDiagnosticStage::Publication;
        diagnostic.SourcePath = path;
        diagnostic.Message = message;
        return true;
    }

    void AddString(JsonValue& object, const char* name, const StringView& value, JsonDocument::AllocatorType& allocator)
    {
        const StringAnsi utf8(value);
        object.AddMember(JsonValue(name, allocator).Move(), JsonValue(utf8.Get(), utf8.Length(), allocator).Move(), allocator);
    }

    bool SameOwner(const ArtifactLockRecord& a, const ArtifactLockRecord& b)
    {
        return a.Key == b.Key && a.ProcessID == b.ProcessID && a.ProcessStartIdentity == b.ProcessStartIdentity &&
            a.HostIdentity == b.HostIdentity && a.JobID == b.JobID;
    }

    bool ReadRecord(const StringView& path, ArtifactLockRecord& record, AssetPipelineDiagnostic& diagnostic)
    {
        StringAnsi json;
        if (File::ReadAllText(path, json))
            return LockFail(diagnostic, AssetPipelineDiagnosticCode::ArtifactLockBusy, path, TEXT("Artifact lock record is being created or cannot be read."));
        return ArtifactLockRecord::Parse(json, path, record, diagnostic);
    }
}

bool ArtifactLockRecord::ToJson(StringAnsi& json, AssetPipelineDiagnostic& diagnostic) const
{
    json.Clear();
    if (SchemaVersion != CurrentSchemaVersion)
        return LockFail(diagnostic, AssetPipelineDiagnosticCode::ArtifactInvalid, StringView::Empty, TEXT("Artifact lock schema version is invalid."));
    if (Key.IsZero())
        return LockFail(diagnostic, AssetPipelineDiagnosticCode::ArtifactInvalid, StringView::Empty, TEXT("Artifact lock key is empty."));
    if (ProcessID == 0)
        return LockFail(diagnostic, AssetPipelineDiagnosticCode::ArtifactInvalid, StringView::Empty, TEXT("Artifact lock process ID is empty."));
    if (ProcessStartIdentity == 0)
        return LockFail(diagnostic, AssetPipelineDiagnosticCode::ArtifactInvalid, StringView::Empty, TEXT("Artifact lock process start identity is empty."));
    if (HostIdentity.IsEmpty())
        return LockFail(diagnostic, AssetPipelineDiagnosticCode::ArtifactInvalid, StringView::Empty, TEXT("Artifact lock host identity is empty."));
    if (CreatedUtcTicks <= 0 || HeartbeatUtcTicks < CreatedUtcTicks)
        return LockFail(diagnostic, AssetPipelineDiagnosticCode::ArtifactInvalid, StringView::Empty, TEXT("Artifact lock timestamps are invalid."));
    if (!JobID.IsValid())
        return LockFail(diagnostic, AssetPipelineDiagnosticCode::ArtifactInvalid, StringView::Empty, TEXT("Artifact lock job identity is empty."));
    JsonDocument document(rapidjson::kObjectType);
    auto& allocator = document.GetAllocator();
    document.AddMember("schemaVersion", SchemaVersion, allocator);
    const StringAnsi key = Key.ToString();
    document.AddMember("artifactKey", JsonValue(key.Get(), key.Length(), allocator).Move(), allocator);
    document.AddMember("processId", ProcessID, allocator);
    document.AddMember("processStartIdentity", ProcessStartIdentity, allocator);
    AddString(document, "hostIdentity", HostIdentity, allocator);
    document.AddMember("createdUtcTicks", CreatedUtcTicks, allocator);
    document.AddMember("heartbeatUtcTicks", HeartbeatUtcTicks, allocator);
    AddString(document, "jobId", JobID.ToString(Guid::FormatType::N), allocator);
    Array<StringAnsi> order;
    order.Add("schemaVersion");
    order.Add("artifactKey");
    order.Add("processId");
    order.Add("processStartIdentity");
    order.Add("hostIdentity");
    order.Add("createdUtcTicks");
    order.Add("heartbeatUtcTicks");
    order.Add("jobId");
    CanonicalJsonError error;
    if (CanonicalJsonWriter::Write(document, json, error, &order))
        return LockFail(diagnostic, AssetPipelineDiagnosticCode::ArtifactInvalid, StringView::Empty, error.Message);
    diagnostic = AssetPipelineDiagnostic();
    return false;
}

bool ArtifactLockRecord::Parse(const StringAnsiView& json, const StringView& path, ArtifactLockRecord& record, AssetPipelineDiagnostic& diagnostic)
{
    record = ArtifactLockRecord();
    JsonDocument document;
    document.Parse(json.Get(), json.Length());
    CanonicalJsonError canonicalError;
    if (document.HasParseError() || !document.IsObject() || CanonicalJsonWriter::Validate(document, canonicalError) || document.MemberCount() != 8)
        return LockFail(diagnostic, AssetPipelineDiagnosticCode::ArtifactInvalid, path, TEXT("Artifact lock JSON is malformed or non-canonical."));
    const auto schema = document.FindMember("schemaVersion");
    const auto key = document.FindMember("artifactKey");
    const auto processId = document.FindMember("processId");
    const auto processStart = document.FindMember("processStartIdentity");
    const auto host = document.FindMember("hostIdentity");
    const auto created = document.FindMember("createdUtcTicks");
    const auto heartbeat = document.FindMember("heartbeatUtcTicks");
    const auto job = document.FindMember("jobId");
    if (schema == document.MemberEnd() || !schema->value.IsUint() || key == document.MemberEnd() || !key->value.IsString() ||
        processId == document.MemberEnd() || !processId->value.IsUint64() || processStart == document.MemberEnd() || !processStart->value.IsUint64() ||
        host == document.MemberEnd() || !host->value.IsString() || created == document.MemberEnd() || !created->value.IsInt64() ||
        heartbeat == document.MemberEnd() || !heartbeat->value.IsInt64() || job == document.MemberEnd() || !job->value.IsString())
        return LockFail(diagnostic, AssetPipelineDiagnosticCode::ArtifactInvalid, path, TEXT("Artifact lock record is missing a required field or field type."));
    record.SchemaVersion = schema->value.GetUint();
    if (ArtifactKey::Parse(StringAnsiView(key->value.GetString(), key->value.GetStringLength()), record.Key) ||
        Guid::Parse(StringAnsiView(job->value.GetString(), job->value.GetStringLength()), record.JobID))
        return LockFail(diagnostic, AssetPipelineDiagnosticCode::ArtifactInvalid, path, TEXT("Artifact lock key or job identity is invalid."));
    record.ProcessID = processId->value.GetUint64();
    record.ProcessStartIdentity = processStart->value.GetUint64();
    record.HostIdentity = String(StringAnsiView(host->value.GetString(), host->value.GetStringLength()));
    record.CreatedUtcTicks = created->value.GetInt64();
    record.HeartbeatUtcTicks = heartbeat->value.GetInt64();
    if (record.SchemaVersion != CurrentSchemaVersion || record.Key.IsZero() || record.ProcessID == 0 || record.ProcessStartIdentity == 0 ||
        record.HostIdentity.IsEmpty() || record.CreatedUtcTicks <= 0 || record.HeartbeatUtcTicks < record.CreatedUtcTicks || !record.JobID.IsValid())
        return LockFail(diagnostic, AssetPipelineDiagnosticCode::ArtifactInvalid, path, TEXT("Artifact lock record contains invalid identity or time values."));
    diagnostic = AssetPipelineDiagnostic();
    return false;
}

uint64 ArtifactLock::GetCurrentProcessStartIdentity()
{
#if PLATFORM_WINDOWS
    FILETIME created, exited, kernel, user;
    if (!GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user))
        return 0;
    ULARGE_INTEGER value;
    value.LowPart = created.dwLowDateTime;
    value.HighPart = created.dwHighDateTime;
    return value.QuadPart;
#else
    static const uint64 identity = static_cast<uint64>(DateTime::NowUTC().Ticks);
    return identity;
#endif
}

ArtifactLockProcessState ArtifactLock::ProbeLocalProcess(const ArtifactLockRecord& record)
{
    if (record.HostIdentity != Platform::GetComputerName())
        return ArtifactLockProcessState::Unknown;
#if PLATFORM_WINDOWS
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, static_cast<DWORD>(record.ProcessID));
    if (!process)
        return GetLastError() == ERROR_INVALID_PARAMETER ? ArtifactLockProcessState::RecordedProcessDead : ArtifactLockProcessState::Unknown;
    if (WaitForSingleObject(process, 0) == WAIT_OBJECT_0)
    {
        CloseHandle(process);
        return ArtifactLockProcessState::RecordedProcessDead;
    }
    FILETIME created, exited, kernel, user;
    if (!GetProcessTimes(process, &created, &exited, &kernel, &user))
    {
        CloseHandle(process);
        return ArtifactLockProcessState::Unknown;
    }
    CloseHandle(process);
    ULARGE_INTEGER value;
    value.LowPart = created.dwLowDateTime;
    value.HighPart = created.dwHighDateTime;
    return value.QuadPart == record.ProcessStartIdentity ? ArtifactLockProcessState::SameProcessAlive : ArtifactLockProcessState::PidReused;
#else
    if (record.ProcessID == Platform::GetCurrentProcessId())
        return record.ProcessStartIdentity == GetCurrentProcessStartIdentity() ? ArtifactLockProcessState::SameProcessAlive : ArtifactLockProcessState::PidReused;
    return ArtifactLockProcessState::Unknown;
#endif
}

namespace
{
    bool TryCreateLockFile(const StringView& path, const StringAnsiView& json, bool& alreadyExists, AssetPipelineDiagnostic& diagnostic)
    {
        alreadyExists = FileSystem::FileExists(path);
        if (alreadyExists)
            return false;
        File* file = File::Open(path, FileMode::CreateNew, FileAccess::Write, FileShare::None);
        if (!file)
        {
            alreadyExists = FileSystem::FileExists(path);
            return alreadyExists ? false : LockFail(diagnostic, AssetPipelineDiagnosticCode::LibraryCreationFailed, path, TEXT("Cannot create artifact lock file."));
        }
        uint32 written = 0;
        const bool failed = file->Write(json.Get(), json.Length(), &written) || written != static_cast<uint32>(json.Length());
        Delete(file);
        if (failed)
        {
            FileSystem::DeleteFile(path);
            return LockFail(diagnostic, AssetPipelineDiagnosticCode::LibraryCreationFailed, path, TEXT("Cannot write complete artifact lock record."));
        }
        return false;
    }

    bool RecoverRecord(const StringView& libraryRoot, const StringView& lockPath, const ArtifactLockRecord& expected,
        bool& recovered, AssetPipelineDiagnostic& diagnostic)
    {
        recovered = false;
        const String claimed = String(lockPath) + TEXT(".stale-") + Guid::New().ToString(Guid::FormatType::N);
        if (FileSystem::MoveFile(claimed, lockPath, false))
            return false;
        ArtifactLockRecord claimedRecord;
        if (ReadRecord(claimed, claimedRecord, diagnostic) || !SameOwner(expected, claimedRecord))
            return LockFail(diagnostic, AssetPipelineDiagnosticCode::ArtifactInvalid, claimed, TEXT("Recovered artifact lock identity changed during takeover."));
        ArtifactStoragePath stagingPath;
        AssetPipelineDiagnostic ignored;
        if (!ArtifactStore::TryGetJobStagingPath(libraryRoot, claimedRecord.JobID, stagingPath, ignored) && FileSystem::DirectoryExists(stagingPath.Get()))
        {
            if (FileSystem::DeleteDirectory(stagingPath.Get(), true))
                return LockFail(diagnostic, AssetPipelineDiagnosticCode::ArtifactInvalid, stagingPath.Get(), TEXT("Cannot remove staging owned by a disproven process identity."));
        }
        if (FileSystem::DeleteFile(claimed))
            return LockFail(diagnostic, AssetPipelineDiagnosticCode::ArtifactInvalid, claimed, TEXT("Cannot remove recovered artifact lock record."));
        recovered = true;
        diagnostic = AssetPipelineDiagnostic();
        return false;
    }
}

ArtifactLock::ArtifactLock(ArtifactLock&& other) noexcept
    : _path(MoveTemp(other._path))
    , _record(MoveTemp(other._record))
    , _ownsLock(other._ownsLock)
{
    other._ownsLock = false;
}

ArtifactLock& ArtifactLock::operator=(ArtifactLock&& other) noexcept
{
    if (this != &other)
    {
        Release();
        _path = MoveTemp(other._path);
        _record = MoveTemp(other._record);
        _ownsLock = other._ownsLock;
        other._ownsLock = false;
    }
    return *this;
}

ArtifactLock::~ArtifactLock()
{
    Release();
}

bool ArtifactLock::Acquire(const StringView& libraryRoot, const ArtifactKey& key, const Guid& jobId, const AssetCancellationToken& cancellation,
    AssetPipelineDiagnostic& diagnostic, uint32 timeoutMilliseconds, const ArtifactLockLivenessProbe& probe)
{
    Release();
    diagnostic = AssetPipelineDiagnostic();
    if (key.IsZero() || !jobId.IsValid() || ArtifactStore::TryGetLockPath(libraryRoot, key, _path, diagnostic))
        return true;
    _record = ArtifactLockRecord();
    _record.Key = key;
    _record.ProcessID = Platform::GetCurrentProcessId();
    _record.ProcessStartIdentity = GetCurrentProcessStartIdentity();
    _record.HostIdentity = Platform::GetComputerName();
    _record.CreatedUtcTicks = DateTime::NowUTC().Ticks;
    _record.HeartbeatUtcTicks = _record.CreatedUtcTicks;
    _record.JobID = jobId;
    StringAnsi json;
    if (_record.ToJson(json, diagnostic))
        return true;
    const auto started = std::chrono::steady_clock::now();
    for (;;)
    {
        bool alreadyExists = false;
        if (TryCreateLockFile(_path.Get(), json, alreadyExists, diagnostic))
            return true;
        if (!alreadyExists)
        {
            _ownsLock = true;
            diagnostic = AssetPipelineDiagnostic();
            return false;
        }
        if (cancellation.IsCancellationRequested())
            return LockFail(diagnostic, AssetPipelineDiagnosticCode::BuildCancelled, _path.Get(), TEXT("Artifact lock wait was cancelled."));

        ArtifactLockRecord existing;
        AssetPipelineDiagnostic readDiagnostic;
        if (!ReadRecord(_path.Get(), existing, readDiagnostic))
        {
            if (existing.Key != key)
                return LockFail(diagnostic, AssetPipelineDiagnosticCode::ArtifactInvalid, _path.Get(), TEXT("Artifact lock path contains a different exact key."));
            const ArtifactLockProcessState state = probe.IsBinded() ? probe(existing) : ProbeLocalProcess(existing);
            if (state == ArtifactLockProcessState::RecordedProcessDead || state == ArtifactLockProcessState::PidReused)
            {
                bool recovered = false;
                if (RecoverRecord(libraryRoot, _path.Get(), existing, recovered, diagnostic))
                    return true;
                if (recovered)
                    continue;
            }
        }
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count();
        if (elapsed >= timeoutMilliseconds)
            return LockFail(diagnostic, AssetPipelineDiagnosticCode::ArtifactLockBusy, _path.Get(), TEXT("Artifact key is locked by a live or unverified process identity."));
        Platform::Sleep(static_cast<int32>(Math::Min<int64>(10, timeoutMilliseconds - elapsed)));
    }
}

void ArtifactLock::Release()
{
    if (!_ownsLock)
        return;
    ArtifactLockRecord current;
    AssetPipelineDiagnostic ignored;
    if (!ReadRecord(_path.Get(), current, ignored) && SameOwner(_record, current))
        FileSystem::DeleteFile(_path.Get());
    _ownsLock = false;
    _path = ArtifactStoragePath();
    _record = ArtifactLockRecord();
}

bool ArtifactLock::RecoverAbandoned(const StringView& libraryRoot, const ArtifactLockLivenessProbe& probe, int32& recoveredCount, AssetPipelineDiagnostic& diagnostic)
{
    recoveredCount = 0;
    diagnostic = AssetPipelineDiagnostic();
    const String locksPath = ArtifactStore::GetLocksPath(libraryRoot);
    Array<String> files;
    if (FileSystem::DirectoryGetFiles(files, locksPath, TEXT("*.lock"), DirectorySearchOption::TopDirectoryOnly))
        return LockFail(diagnostic, AssetPipelineDiagnosticCode::ArtifactInvalid, locksPath, TEXT("Cannot enumerate artifact lock records for recovery."));
    for (const String& path : files)
    {
        ArtifactLockRecord record;
        AssetPipelineDiagnostic ignored;
        if (ReadRecord(path, record, ignored))
            continue;
        const ArtifactLockProcessState state = probe.IsBinded() ? probe(record) : ProbeLocalProcess(record);
        if (state != ArtifactLockProcessState::RecordedProcessDead && state != ArtifactLockProcessState::PidReused)
            continue;
        bool recovered = false;
        if (RecoverRecord(libraryRoot, path, record, recovered, diagnostic))
            return true;
        recoveredCount += recovered ? 1 : 0;
    }
    return false;
}
