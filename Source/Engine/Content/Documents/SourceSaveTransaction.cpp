// Copyright (c) Wojciech Figat. All rights reserved.

#include "SourceSaveTransaction.h"
#include "Engine/Content/AssetDatabase/AssetDatabase.h"
#include "Engine/Content/AssetDatabase/AssetPath.h"
#include "Engine/Core/ScopeExit.h"
#include "Engine/Engine/Globals.h"
#include "Engine/Platform/CriticalSection.h"
#include "Engine/Platform/File.h"
#include "Engine/Platform/FileSystem.h"
#include "Engine/Platform/StringUtils.h"
#include "Engine/Threading/Threading.h"

namespace
{
    CriticalSection Reservation;

    bool Fail(AssetPipelineDiagnostic& diagnostic, AssetPipelineDiagnosticCode code,
        const StringView& path, const StringView& message)
    {
        diagnostic = AssetPipelineDiagnostic();
        diagnostic.Code = code;
        diagnostic.Stage = AssetPipelineDiagnosticStage::Prepare;
        diagnostic.SourcePath = path;
        diagnostic.Message = message;
        return true;
    }

    String Normalize(const StringView& path)
    {
        String result(path);
        FileSystem::NormalizePath(result);
        return result;
    }

    bool IsAuthoredSourcePath(const StringView& path)
    {
        const String contentRoot = Normalize(Globals::ProjectContentFolder);
        if (contentRoot.HasChars() && AssetPathPolicy::IsSameOrChild(path, contentRoot))
            return true;
        if (Globals::ProjectFolder.HasChars())
        {
            const String externalActors = Normalize(Globals::ProjectFolder / TEXT("ExternalActors"));
            if (AssetPathPolicy::IsSameOrChild(path, externalActors))
                return true;
        }
        return false;
    }

    class AssetDatabaseSourceSaveRevisionProvider final : public ISourceSaveRevisionProvider
    {
    public:
        SourceSaveRevisionLookup LookupTrackedSource(const StringView& path, SourceSaveRevision& result,
            AssetPipelineDiagnostic& diagnostic) const override
        {
            const AssetDatabaseReadSnapshot snapshot = AssetDatabase::Get().GetDurableSnapshot();
            if (!snapshot.IsValid())
            {
                Fail(diagnostic, AssetPipelineDiagnosticCode::SnapshotInvalid, path,
                    TEXT("Durable asset database is unavailable for authored source lookup."));
                return SourceSaveRevisionLookup::Failed;
            }
            for (const SourceAssetRow& source : snapshot.GetState().Sources)
            {
                if (FileSystem::AreFilePathsEquivalent(source.Path, path))
                {
                    result = SourceSaveRevision();
                    result.SourceAssetID = source.AssetGuid;
                    result.SourcePath = source.Path;
                    result.SourceRevision = source.LastModifiedRevision;
                    result.DurableSourceHash = source.SourceHash;
                    result.IsTracked = true;
                    diagnostic = AssetPipelineDiagnostic();
                    return SourceSaveRevisionLookup::Found;
                }
            }
            diagnostic = AssetPipelineDiagnostic();
            return SourceSaveRevisionLookup::NotFound;
        }
    } DefaultRevisionProvider;

    bool CaptureState(const ISourceSaveRevisionProvider& provider, const StringView& path,
        SourceSaveRegistrationMode registrationMode,
        SourceSaveConflictPolicy conflictPolicy,
        SourceSaveRevision& result, Array<byte>& bytes, bool& conflict,
        AssetPipelineDiagnostic& diagnostic)
    {
        result = SourceSaveRevision();
        bytes.Clear();
        conflict = false;
        const String normalized = Normalize(path);
        if (normalized.IsEmpty())
            return Fail(diagnostic, AssetPipelineDiagnosticCode::InvalidMeta, path,
                TEXT("Authored source save path is empty."));
        if (registrationMode == SourceSaveRegistrationMode::UntrackedLocal && IsAuthoredSourcePath(normalized))
            return Fail(diagnostic, AssetPipelineDiagnosticCode::InvalidMeta, normalized,
                TEXT("Editor-local saves cannot target Content or ExternalActors."));

        SourceSaveRevision tracked;
        ContentHash durableSourceHash;
        SourceSaveRevisionLookup lookup = SourceSaveRevisionLookup::NotFound;
        if (registrationMode != SourceSaveRegistrationMode::UntrackedLocal)
            lookup = provider.LookupTrackedSource(normalized, tracked, diagnostic);
        if (lookup == SourceSaveRevisionLookup::Failed)
        {
            if (diagnostic.Code == AssetPipelineDiagnosticCode::None)
                Fail(diagnostic, AssetPipelineDiagnosticCode::SnapshotInvalid, normalized,
                    TEXT("Durable authored source lookup failed."));
            return true;
        }
        if (lookup == SourceSaveRevisionLookup::Found)
        {
            if (!tracked.SourceAssetID.IsValid() || tracked.SourcePath.IsEmpty() ||
                !FileSystem::AreFilePathsEquivalent(tracked.SourcePath, normalized))
            {
                return Fail(diagnostic, AssetPipelineDiagnosticCode::SnapshotInvalid, normalized,
                    TEXT("Tracked authored source identity is invalid."));
            }
            result.SourceAssetID = tracked.SourceAssetID;
            result.SourcePath = Normalize(tracked.SourcePath);
            result.SourceRevision = tracked.SourceRevision;
            result.DurableSourceHash = tracked.DurableSourceHash;
            result.IsTracked = true;
            durableSourceHash = tracked.DurableSourceHash;
        }
        else
        {
            result.SourcePath = normalized;
            diagnostic = AssetPipelineDiagnostic();
        }

        result.Exists = FileSystem::FileExists(result.SourcePath);
        if (result.Exists)
        {
            if (File::ReadAllBytes(result.SourcePath, bytes))
            {
                if (!FileSystem::FileExists(result.SourcePath))
                {
                    bytes.Clear();
                    result.Exists = false;
                    diagnostic = AssetPipelineDiagnostic();
                    return false;
                }
                return Fail(diagnostic, AssetPipelineDiagnosticCode::SourceBusy, result.SourcePath,
                    TEXT("Cannot read the exact authored source revision."));
            }
            result.SourceHash = ContentHash::Compute(bytes.Get(), bytes.Count());
            if (result.IsTracked && result.SourceHash != durableSourceHash &&
                conflictPolicy == SourceSaveConflictPolicy::Strict)
            {
                conflict = true;
                return Fail(diagnostic, AssetPipelineDiagnosticCode::SourceBusy, result.SourcePath,
                    TEXT("Authored source bytes do not match the durable database hash."));
            }
        }
        diagnostic = AssetPipelineDiagnostic();
        return false;
    }

    bool MatchesExpected(const SourceSaveRequest& request, const SourceSaveRevision& current)
    {
        const SourceSaveRevision& expected = request.Expected;
        if (!FileSystem::AreFilePathsEquivalent(expected.SourcePath, current.SourcePath) ||
            expected.IsTracked != current.IsTracked || expected.Exists != current.Exists ||
            expected.SourceHash != current.SourceHash ||
            expected.DurableSourceHash != current.DurableSourceHash)
        {
            return false;
        }
        if (expected.IsTracked &&
            (expected.SourceAssetID != current.SourceAssetID || expected.SourceRevision != current.SourceRevision))
        {
            return false;
        }
        return true;
    }

    bool IsSameBytes(const Array<byte>& current, const StringAnsiView& desired)
    {
        return current.Count() == desired.Length() &&
            (current.IsEmpty() || Platform::MemoryCompare(current.Get(), desired.Get(), current.Count()) == 0);
    }

    bool IsInjected(ISourceSaveFailureInjector* injector, SourceSaveFailurePoint point)
    {
        return injector && injector->ShouldFail(point);
    }
}

SourceSaveTransaction::SourceSaveTransaction(const ISourceSaveRevisionProvider* revisionProvider)
    : _revisionProvider(revisionProvider ? revisionProvider : &DefaultRevisionProvider)
{
}

bool SourceSaveTransaction::Capture(const StringView& path, SourceSaveRegistrationMode registrationMode,
    SourceSaveRevision& result, AssetPipelineDiagnostic& diagnostic,
    SourceSaveConflictPolicy conflictPolicy) const
{
    Array<byte> bytes;
    bool conflict;
    ScopeLock lock(Reservation);
    if (CaptureState(*_revisionProvider, path, registrationMode, conflictPolicy,
        result, bytes, conflict, diagnostic))
        return true;
    if (registrationMode == SourceSaveRegistrationMode::RequireTracked && !result.IsTracked)
        return Fail(diagnostic, AssetPipelineDiagnosticCode::SourceMissing, result.SourcePath,
            TEXT("Authored source is not registered in the durable asset database."));
    if (result.IsTracked && !result.Exists)
        return Fail(diagnostic, AssetPipelineDiagnosticCode::SourceMissing, result.SourcePath,
            TEXT("Registered authored source is missing."));
    diagnostic = AssetPipelineDiagnostic();
    return false;
}

bool SourceSaveTransaction::Commit(const SourceSaveRequest& request, SourceSaveResult& result,
    AssetPipelineDiagnostic& diagnostic, ISourceSaveCallback* callback,
    ISourceSaveFailureInjector* failureInjector) const
{
    result = SourceSaveResult();
    result.TransactionID = Guid::New();
    if (request.Expected.SourcePath.IsEmpty() ||
        (request.RegistrationMode == SourceSaveRegistrationMode::RequireTracked && !request.Expected.IsTracked) ||
        (request.RegistrationMode == SourceSaveRegistrationMode::UntrackedLocal && request.Expected.IsTracked))
    {
        result.Outcome = SourceSaveOutcome::Rejected;
        return Fail(diagnostic, AssetPipelineDiagnosticCode::InvalidMeta, request.Expected.SourcePath,
            TEXT("Authored source save request has no valid tracked revision."));
    }

    Array<byte> currentBytes;
    {
        bool captureConflict;
        ScopeLock lock(Reservation);
        if (CaptureState(*_revisionProvider, request.Expected.SourcePath, request.RegistrationMode, request.ConflictPolicy,
            result.Current, currentBytes, captureConflict, diagnostic))
        {
            result.Outcome = captureConflict ? SourceSaveOutcome::Conflict : SourceSaveOutcome::Failed;
            return true;
        }
        if (!MatchesExpected(request, result.Current))
        {
            result.Outcome = SourceSaveOutcome::Conflict;
            Fail(diagnostic, AssetPipelineDiagnosticCode::SourceBusy, request.Expected.SourcePath,
                TEXT("Authored source identity, revision, or content changed before save."));
            diagnostic.AssetGuid = request.Expected.SourceAssetID;
            return true;
        }
        if (IsSameBytes(currentBytes, request.CanonicalBytes))
        {
            result.Outcome = SourceSaveOutcome::Unchanged;
            diagnostic = AssetPipelineDiagnostic();
            return false;
        }
    }

    if (callback && callback->BeforeCommit(request, diagnostic))
    {
        result.Outcome = SourceSaveOutcome::Rejected;
        if (diagnostic.Code == AssetPipelineDiagnosticCode::None)
            Fail(diagnostic, AssetPipelineDiagnosticCode::PrepareInvalidated, request.Expected.SourcePath,
                TEXT("Authored source save was rejected before commit."));
        return true;
    }

    ScopeLock lock(Reservation);
    currentBytes.Clear();
    bool captureConflict;
    if (CaptureState(*_revisionProvider, request.Expected.SourcePath, request.RegistrationMode, request.ConflictPolicy,
        result.Current, currentBytes, captureConflict, diagnostic))
    {
        result.Outcome = captureConflict ? SourceSaveOutcome::Conflict : SourceSaveOutcome::Failed;
        return true;
    }
    if (!MatchesExpected(request, result.Current))
    {
        result.Outcome = SourceSaveOutcome::Conflict;
        Fail(diagnostic, AssetPipelineDiagnosticCode::SourceBusy, request.Expected.SourcePath,
            TEXT("Authored source changed while save authorization was running."));
        diagnostic.AssetGuid = request.Expected.SourceAssetID;
        return true;
    }
    if (IsSameBytes(currentBytes, request.CanonicalBytes))
    {
        result.Outcome = SourceSaveOutcome::Unchanged;
        diagnostic = AssetPipelineDiagnostic();
        return false;
    }
    if (result.Current.Exists && FileSystem::IsReadOnly(result.Current.SourcePath))
    {
        result.Outcome = SourceSaveOutcome::Failed;
        return Fail(diagnostic, AssetPipelineDiagnosticCode::InvalidMeta, result.Current.SourcePath,
            TEXT("Authored source is read-only."));
    }

    const String parent = StringUtils::GetDirectoryName(result.Current.SourcePath);
    if (FileSystem::CreateDirectory(parent))
    {
        result.Outcome = SourceSaveOutcome::Failed;
        return Fail(diagnostic, AssetPipelineDiagnosticCode::LibraryCreationFailed, parent,
            TEXT("Cannot create the authored source parent directory."));
    }
    if (IsInjected(failureInjector, SourceSaveFailurePoint::BeforeStagingWrite))
    {
        result.Outcome = SourceSaveOutcome::Failed;
        return Fail(diagnostic, AssetPipelineDiagnosticCode::PrepareInvalidated, result.Current.SourcePath,
            TEXT("Injected authored source failure before staging."));
    }

    const String staging = result.Current.SourcePath + TEXT(".stage-") + result.TransactionID.ToString(Guid::FormatType::N);
    SCOPE_EXIT { FileSystem::DeleteFile(staging); };
    if (File::WriteAllBytes(staging, request.CanonicalBytes.Get(), request.CanonicalBytes.Length()))
    {
        result.Outcome = SourceSaveOutcome::Failed;
        return Fail(diagnostic, AssetPipelineDiagnosticCode::InvalidMeta, result.Current.SourcePath,
            TEXT("Cannot write authored source staging bytes."));
    }
    if (IsInjected(failureInjector, SourceSaveFailurePoint::AfterStagingWrite))
    {
        result.Outcome = SourceSaveOutcome::Failed;
        return Fail(diagnostic, AssetPipelineDiagnosticCode::PrepareInvalidated, result.Current.SourcePath,
            TEXT("Injected authored source failure after staging."));
    }
    if (IsInjected(failureInjector, SourceSaveFailurePoint::BeforeReplace))
    {
        result.Outcome = SourceSaveOutcome::Failed;
        return Fail(diagnostic, AssetPipelineDiagnosticCode::PrepareInvalidated, result.Current.SourcePath,
            TEXT("Injected authored source failure before replace."));
    }

    currentBytes.Clear();
    if (CaptureState(*_revisionProvider, request.Expected.SourcePath, request.RegistrationMode, request.ConflictPolicy,
        result.Current, currentBytes, captureConflict, diagnostic))
    {
        result.Outcome = captureConflict ? SourceSaveOutcome::Conflict : SourceSaveOutcome::Failed;
        return true;
    }
    if (!MatchesExpected(request, result.Current))
    {
        result.Outcome = SourceSaveOutcome::Conflict;
        Fail(diagnostic, AssetPipelineDiagnosticCode::SourceBusy, request.Expected.SourcePath,
            TEXT("Authored source changed after staging and will not be replaced."));
        diagnostic.AssetGuid = request.Expected.SourceAssetID;
        return true;
    }
    if (result.Current.Exists && FileSystem::IsReadOnly(result.Current.SourcePath))
    {
        result.Outcome = SourceSaveOutcome::Failed;
        return Fail(diagnostic, AssetPipelineDiagnosticCode::InvalidMeta, result.Current.SourcePath,
            TEXT("Authored source became read-only before replace."));
    }
    if (FileSystem::MoveFile(result.Current.SourcePath, staging, true))
    {
        result.Outcome = SourceSaveOutcome::Failed;
        return Fail(diagnostic, AssetPipelineDiagnosticCode::InvalidMeta, result.Current.SourcePath,
            TEXT("Cannot atomically replace authored source."));
    }

    result.Outcome = SourceSaveOutcome::Committed;
    result.Current.Exists = true;
    result.Current.SourceHash = ContentHash::Compute(request.CanonicalBytes.Get(), request.CanonicalBytes.Length());
    result.SelfWrite.TransactionID = result.TransactionID;
    result.SelfWrite.Path = result.Current.SourcePath;
    result.SelfWrite.Content = result.Current.SourceHash;
    diagnostic = AssetPipelineDiagnostic();
    return false;
}
