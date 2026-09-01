// Copyright (c) Wojciech Figat. All rights reserved.

#include "ArtifactPublisher.h"
#include "ArtifactLock.h"
#include "ArtifactStore.h"
#include "Engine/Content/AssetDatabase/AssetDatabase.h"
#include "Engine/Core/Types/DateTime.h"
#include "Engine/Core/ScopeExit.h"
#include "Engine/Platform/File.h"
#include "Engine/Platform/FileSystem.h"
#include <algorithm>
#include <memory>
#if PLATFORM_WINDOWS
#include "Engine/Platform/Win32/IncludeWindowsHeaders.h"
#endif

namespace
{
#if PLATFORM_WINDOWS
    String ToNativePath(const StringView& path)
    {
        String value(path);
        value.Replace(TEXT('/'), TEXT('\\'));
        if (value.StartsWith(TEXT("\\\\?\\")))
            return value;
        if (value.StartsWith(TEXT("\\\\")))
            return TEXT("\\\\?\\UNC\\") + value.Substring(2);
        return TEXT("\\\\?\\") + value;
    }

    bool NativeFileExists(const StringView& path)
    {
        const String nativePath = ToNativePath(path);
        const DWORD attributes = GetFileAttributesW(*nativePath);
        return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
    }

    bool NativeDirectoryExists(const StringView& path)
    {
        const String nativePath = ToNativePath(path);
        const DWORD attributes = GetFileAttributesW(*nativePath);
        return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    }

    bool EnsureNativeDirectory(const StringView& path)
    {
        if (NativeDirectoryExists(path))
            return false;
        String value(path);
        value.Replace(TEXT('/'), TEXT('\\'));
        const int32 separator = value.FindLast('\\');
        if (separator > 2 && EnsureNativeDirectory(value.Substring(0, separator)))
            return true;
        const String nativePath = ToNativePath(value);
        if (CreateDirectoryW(*nativePath, nullptr) != 0)
            return false;
        return GetLastError() != ERROR_ALREADY_EXISTS || !NativeDirectoryExists(value);
    }
#else
    bool NativeFileExists(const StringView& path)
    {
        return FileSystem::FileExists(path);
    }

    bool EnsureNativeDirectory(const StringView& path)
    {
        return !FileSystem::DirectoryExists(path) && FileSystem::CreateDirectory(path);
    }
#endif

    bool PublicationFail(AssetPipelineDiagnostic& diagnostic, AssetPipelineDiagnosticCode code, const PreparedAsset& prepared,
        const StringView& path, const StringView& message)
    {
        diagnostic = AssetPipelineDiagnostic();
        diagnostic.Code = code;
        diagnostic.Stage = AssetPipelineDiagnosticStage::Publication;
        diagnostic.AssetGuid = prepared.AssetID;
        diagnostic.SourcePath = path;
        diagnostic.Message = message;
        return true;
    }

    bool Inject(ArtifactPublicationFailurePoint configured, ArtifactPublicationFailurePoint current,
        const PreparedAsset& prepared, AssetPipelineDiagnostic& diagnostic)
    {
        if (configured != current)
            return false;
        return PublicationFail(diagnostic, AssetPipelineDiagnosticCode::BuildFailed, prepared, StringView::Empty,
            TEXT("Injected artifact publication failure."));
    }

    bool ReadAndHash(const StringView& path, uint64& size, ContentHash& hash)
    {
        Array<byte> bytes;
#if PLATFORM_WINDOWS
        const String nativePath = ToNativePath(path);
        if (File::ReadAllBytes(nativePath, bytes))
#else
        if (File::ReadAllBytes(path, bytes))
#endif
            return true;
        size = bytes.Count();
        hash = ContentHash::Compute(bytes.Get(), bytes.Count());
        return false;
    }

    bool PersistPublication(const StringView& libraryRoot, const ArtifactManifest& manifest, const StringAnsiView& manifestJson,
        AssetPipelineDiagnostic& diagnostic)
    {
        AssetDatabase& database = AssetDatabase::Get();
        if (!database.IsUsingLibrary(libraryRoot))
            return false;

        const String targetId(manifest.Target.BuildKey(ArtifactTargetDimension::All).ToString());
        SourceAssetPublicationRow publication;
        publication.AssetGuid = manifest.ObjectID.Asset.Value;
        publication.LocalFileId = manifest.ObjectID.LocalId;
        publication.TargetId = targetId;
        publication.ManifestHash = ContentHash::Compute(manifestJson.Get(), manifestJson.Length());
        const ArtifactManifestOutput* primaryOutput = nullptr;
        for (const ArtifactManifestOutput& output : manifest.Outputs)
        {
            if (!primaryOutput || output.Kind == StringAnsiView("runtime"))
                primaryOutput = &output;
            if (output.Kind == StringAnsiView("runtime"))
                break;
        }
        if (!primaryOutput || primaryOutput->Key.IsZero())
        {
            diagnostic = AssetPipelineDiagnostic();
            diagnostic.Code = AssetPipelineDiagnosticCode::ArtifactInvalid;
            diagnostic.Stage = AssetPipelineDiagnosticStage::Publication;
            diagnostic.AssetGuid = manifest.AssetID;
            diagnostic.Message = TEXT("Artifact publication has no deterministic primary output key.");
            return true;
        }
        publication.Artifact = primaryOutput->Key;
        publication.InputFingerprint = manifest.InputFingerprint;
        publication.SourceRevision = manifest.DatabaseRevision;
        publication.ImporterRegistryGeneration = database.GetDurableSnapshot().GetState().Database.ImporterRegistryGeneration;
        publication.PublishedUtcTicks = DateTime::NowUTC().Ticks;
        publication.IsLastKnownGood = true;

        Array<SourceAssetDependencyRow> dependencies;
        dependencies.EnsureCapacity(manifest.Dependencies.Count());
        for (const ArtifactManifestDependency& source : manifest.Dependencies)
        {
            SourceAssetDependencyRow dependency;
            dependency.OwnerAssetGuid = publication.AssetGuid;
            dependency.OwnerLocalFileId = publication.LocalFileId;
            dependency.TargetId = targetId;
            dependency.Kind = source.Kind;
            dependency.ExactArtifact = source.ExactArtifact;
            dependency.Content = source.Hash;
            dependency.OriginPath = source.Origin;
            if (source.Kind == AssetDependencyKind::SourceFile)
                dependency.SourcePath = source.Identity;
            else if (source.Kind == AssetDependencyKind::Toolchain)
                dependency.CustomDependency = source.Identity;
            else
            {
                if (!source.ObjectID.IsValid())
                {
                    diagnostic = AssetPipelineDiagnostic();
                    diagnostic.Code = AssetPipelineDiagnosticCode::ArtifactMissing;
                    diagnostic.Stage = AssetPipelineDiagnosticStage::Publication;
                    diagnostic.AssetGuid = manifest.AssetID;
                    diagnostic.SourcePath = source.Identity;
                    diagnostic.Message = TEXT("Cannot persist an artifact dependency that has no asset-object database record.");
                    return true;
                }
                dependency.TargetAssetGuid = source.ObjectID.Asset.Value;
                dependency.TargetLocalFileId = source.ObjectID.LocalId;
            }
            dependencies.Add(MoveTemp(dependency));
        }
        return database.RecordPublication(publication, dependencies, diagnostic);
    }

    String DescribeDifference(const StringView& existingPath, const StringView& stagedPath)
    {
        Array<byte> existingBytes;
        Array<byte> stagedBytes;
#if PLATFORM_WINDOWS
        if (File::ReadAllBytes(ToNativePath(existingPath), existingBytes) || File::ReadAllBytes(ToNativePath(stagedPath), stagedBytes))
#else
        if (File::ReadAllBytes(existingPath, existingBytes) || File::ReadAllBytes(stagedPath, stagedBytes))
#endif
            return TEXT("Existing immutable artifact key contains different or corrupt bytes.");
        const int32 commonLength = Math::Min(existingBytes.Count(), stagedBytes.Count());
        int32 firstDifference = 0;
        while (firstDifference < commonLength && existingBytes[firstDifference] == stagedBytes[firstDifference])
            firstDifference++;
        const int32 existingValue = firstDifference < existingBytes.Count() ? existingBytes[firstDifference] : -1;
        const int32 stagedValue = firstDifference < stagedBytes.Count() ? stagedBytes[firstDifference] : -1;
        return String::Format(TEXT("Existing immutable artifact key contains different or corrupt bytes (existing {0} bytes, staged {1} bytes, first difference at byte {2}: existing {3}, staged {4})."),
            existingBytes.Count(), stagedBytes.Count(), firstDifference, existingValue, stagedValue);
    }

    bool FlushFile(const StringView& path)
    {
#if PLATFORM_WINDOWS
        const String value = ToNativePath(path);
        HANDLE handle = CreateFileW(*value, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (handle == INVALID_HANDLE_VALUE)
            return true;
        const bool failed = FlushFileBuffers(handle) == 0;
        CloseHandle(handle);
        return failed;
#else
        return false;
#endif
    }

    bool AtomicReplace(const StringView& destination, const StringView& staging)
    {
#if PLATFORM_WINDOWS
        const String destinationPath = ToNativePath(destination);
        const String stagingPath = ToNativePath(staging);
        return MoveFileExW(*stagingPath, *destinationPath, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) == 0;
#else
        return FileSystem::MoveFile(destination, staging, true);
#endif
    }

    bool MoveNoReplace(const StringView& destination, const StringView& staging)
    {
#if PLATFORM_WINDOWS
        const String destinationPath = ToNativePath(destination);
        const String stagingPath = ToNativePath(staging);
        return MoveFileExW(*stagingPath, *destinationPath, MOVEFILE_WRITE_THROUGH) == 0;
#else
        return FileSystem::MoveFile(destination, staging, false);
#endif
    }

    ContentHash CalculateSourceHash(const PreparedAsset& prepared)
    {
        Array<ContentHash> hashes;
        for (const AssetDependency& dependency : prepared.Dependencies)
        {
            if (dependency.Kind == AssetDependencyKind::SourceFile)
                hashes.Add(dependency.Content);
        }
        if (hashes.Count() == 1)
            return hashes[0];
        ArtifactKeyBuilder builder(StringAnsiView("flax-manifest-source-set-v1"));
        for (int32 i = 0; i < hashes.Count(); i++)
            builder.AddHash(StringAnsi::Format("source-{0}", i), hashes[i]);
        return builder.Finalize().Digest;
    }

    String DescribeOrigin(const AssetDependencyOrigin& origin)
    {
        String value = origin.Path;
        if (!origin.GraphNode.IsEmpty())
            value += TEXT(" node ") + origin.GraphNode;
        if (!origin.GraphPin.IsEmpty())
            value += TEXT(" pin ") + origin.GraphPin;
        return value;
    }
}

bool ArtifactPublisher::Publish(const StringView& libraryRoot, const PreparedAsset& prepared, ArtifactBuildContext& context,
    const ArtifactPublicationRequest& request, const ArtifactOutputValidatorRegistry& validators,
    ArtifactPublicationResult& result, AssetPipelineDiagnostic& diagnostic)
{
    result = ArtifactPublicationResult();
    diagnostic = AssetPipelineDiagnostic();
    bool cleanupRequired = context.IsInitialized();
    SCOPE_EXIT
    {
        if (cleanupRequired)
            context.Cancel();
    };
    if (!context.IsInitialized() || context.IsClosed() || prepared.Outputs.IsEmpty() || request.Outputs.Count() != prepared.Outputs.Count() ||
        request.ProcessorID.IsEmpty() || request.ProcessorImplementationVersion < 1 || request.BuildID.IsEmpty() || !request.QueryCurrentState.IsBinded())
        return PublicationFail(diagnostic, AssetPipelineDiagnosticCode::BuildFailed, prepared, context.GetStagingPath(), TEXT("Artifact publication request is incomplete or context is not active."));
    if (context.Close(diagnostic))
        return true;
    if (Inject(request.FailurePoint, ArtifactPublicationFailurePoint::AfterOutputClose, prepared, diagnostic))
        return true;

    Array<ArtifactManifestOutput> publishedOutputs;
    publishedOutputs.EnsureCapacity(prepared.Outputs.Count());
    for (const DeclaredArtifactOutput& declared : prepared.Outputs)
    {
        const ArtifactPublicationOutputPlan* plan = nullptr;
        for (const ArtifactPublicationOutputPlan& candidate : request.Outputs)
        {
            if (candidate.Kind == declared.Kind)
            {
                if (plan)
                    return PublicationFail(diagnostic, AssetPipelineDiagnosticCode::BuildFailed, prepared, context.GetStagingPath(), TEXT("Artifact publication plan duplicates an output kind."));
                plan = &candidate;
            }
        }
        if (!plan || plan->Key.IsZero())
            return PublicationFail(diagnostic, AssetPipelineDiagnosticCode::BuildFailed, prepared, context.GetStagingPath(), TEXT("Artifact publication plan is missing a declared output key."));
        const StagedArtifactFile* staged = nullptr;
        for (const StagedArtifactFile& file : context.GetFiles())
        {
            if (file.OutputKind == declared.Kind)
            {
                if (staged)
                    return PublicationFail(diagnostic, AssetPipelineDiagnosticCode::BuildFailed, prepared, context.GetStagingPath(), TEXT("Current manifest format requires one staged file per output kind."));
                staged = &file;
            }
        }
        if (!staged)
            return PublicationFail(diagnostic, AssetPipelineDiagnosticCode::ArtifactMissing, prepared, context.GetStagingPath(), TEXT("Declared artifact output has no staged file."));
        if (FlushFile(staged->AbsolutePath))
            return PublicationFail(diagnostic, AssetPipelineDiagnosticCode::ArtifactInvalid, prepared, staged->AbsolutePath, TEXT("Cannot flush staged artifact output."));
        uint64 size;
        ContentHash contentHash;
        if (ReadAndHash(staged->AbsolutePath, size, contentHash) || size != staged->Size || contentHash != staged->Hash)
            return PublicationFail(diagnostic, AssetPipelineDiagnosticCode::ArtifactInvalid, prepared, staged->AbsolutePath, TEXT("Staged artifact output changed before publication."));
        ArtifactStoragePath finalPath;
        if (ArtifactStore::TryGetArtifactPath(libraryRoot, request.Target, declared.TargetDimensions, declared.EffectiveAssetID,
            declared.Kind, plan->Key, declared.Extension, finalPath, diagnostic))
            return true;
        String relativePath;
        if (ArtifactStore::TryMakeLibraryRelative(libraryRoot, finalPath.Get(), relativePath, diagnostic))
            return true;
        ArtifactManifestOutput output;
        output.Kind = declared.Kind;
        output.FormatVersion = declared.FormatVersion;
        output.Key = plan->Key;
        output.RelativePath = relativePath;
        output.Content = contentHash;
        output.Size = size;
        output.Compatibility = declared.CompatibilityTag;
        if (validators.Validate(output.Kind, prepared.OutputType, staged->AbsolutePath, output, diagnostic))
            return true;
        publishedOutputs.Add(MoveTemp(output));
    }
    if (Inject(request.FailurePoint, ArtifactPublicationFailurePoint::AfterOutputValidation, prepared, diagnostic))
        return true;

    Array<ArtifactKey> lockKeys;
    for (const ArtifactManifestOutput& output : publishedOutputs)
    {
        if (!lockKeys.Contains(output.Key))
            lockKeys.Add(output.Key);
    }
    std::sort(lockKeys.Get(), lockKeys.Get() + lockKeys.Count(), [](const ArtifactKey& a, const ArtifactKey& b)
    {
        return a.ToString() < b.ToString();
    });
    Array<std::unique_ptr<ArtifactLock>> locks;
    locks.EnsureCapacity(lockKeys.Count());
    for (const ArtifactKey& key : lockKeys)
    {
        auto lock = std::make_unique<ArtifactLock>();
        if (lock->Acquire(libraryRoot, key, context.GetJobID(), context.GetCancellation(), diagnostic))
            return true;
        locks.Add(MoveTemp(lock));
    }

    for (int32 i = 0; i < publishedOutputs.Count(); i++)
    {
        ArtifactManifestOutput& output = publishedOutputs[i];
        ArtifactStoragePath destination;
        if (ArtifactStore::TryResolveLibraryRelative(libraryRoot, output.RelativePath, destination, diagnostic))
            return true;
        const String directory = StringUtils::GetDirectoryName(destination.Get());
        if (EnsureNativeDirectory(directory))
            return PublicationFail(diagnostic, AssetPipelineDiagnosticCode::LibraryCreationFailed, prepared, directory, TEXT("Cannot create immutable artifact output directory."));
        const StagedArtifactFile* staged = nullptr;
        for (const StagedArtifactFile& file : context.GetFiles())
        {
            if (file.OutputKind == output.Kind)
            {
                staged = &file;
                break;
            }
        }
        if (NativeFileExists(destination.Get()))
        {
            uint64 existingSize;
            ContentHash existingHash;
            if (ReadAndHash(destination.Get(), existingSize, existingHash) || existingSize != output.Size || existingHash != output.Content)
                return PublicationFail(diagnostic, AssetPipelineDiagnosticCode::ArtifactInvalid, prepared, destination.Get(), DescribeDifference(destination.Get(), staged->AbsolutePath));
        }
        else if (MoveNoReplace(destination.Get(), staged->AbsolutePath))
        {
            uint64 existingSize;
            ContentHash existingHash;
            if (!NativeFileExists(destination.Get()) || ReadAndHash(destination.Get(), existingSize, existingHash) || existingSize != output.Size || existingHash != output.Content)
                return PublicationFail(diagnostic, AssetPipelineDiagnosticCode::ArtifactInvalid, prepared, destination.Get(), TEXT("Immutable artifact move failed and no identical winning output exists."));
        }
        if (i == 0 && Inject(request.FailurePoint, ArtifactPublicationFailurePoint::AfterFirstImmutableMove, prepared, diagnostic))
            return true;
    }
    if (Inject(request.FailurePoint, ArtifactPublicationFailurePoint::AfterAllImmutableMoves, prepared, diagnostic))
        return true;

    // Output kinds may be built independently. Serialize manifest updates per asset so
    // concurrent output publications can merge without dropping each other's entries.
    ArtifactKeyBuilder manifestLockBuilder(StringAnsiView("flax-artifact-manifest-lock-v1"));
    manifestLockBuilder.AddGuid(StringAnsiView("asset"), prepared.AssetID);
    manifestLockBuilder.AddKey(StringAnsiView("target"), request.Target.BuildKey(ArtifactTargetDimension::All));
    ArtifactLock manifestLock;
    if (manifestLock.Acquire(libraryRoot, manifestLockBuilder.Finalize(), context.GetJobID(), context.GetCancellation(), diagnostic))
        return true;

    uint64 currentRevision = 0;
    ArtifactKey currentFingerprint;
    request.QueryCurrentState(currentRevision, currentFingerprint);
    if (currentRevision != prepared.DatabaseRevision || currentFingerprint != prepared.InputFingerprint)
    {
        result.WasSuperseded = true;
        context.Cancel();
        cleanupRequired = false;
        return false;
    }

    ArtifactManifest manifest;
    manifest.ObjectID = prepared.ObjectID;
    manifest.AssetID = prepared.AssetID;
    manifest.DatabaseRevision = prepared.DatabaseRevision;
    manifest.ProcessorID = request.ProcessorID;
    manifest.ProcessorImplementationVersion = request.ProcessorImplementationVersion;
    manifest.Target = request.Target;
    manifest.InputFingerprint = prepared.InputFingerprint;
    manifest.SourceHash = CalculateSourceHash(prepared);
    manifest.SettingsHash = prepared.SettingsHash;
    manifest.Outputs = publishedOutputs;
    manifest.BuildID = request.BuildID;
    manifest.BuiltAtUtc = request.BuiltAtUtc;
    for (const AssetDependency& source : prepared.Dependencies)
    {
        ArtifactManifestDependency dependency;
        dependency.Kind = source.Kind;
        dependency.Identity = source.StableIdentity;
        dependency.ObjectID = source.ObjectID;
        if (source.Kind == AssetDependencyKind::BuildInput || source.Kind == AssetDependencyKind::RuntimeReference)
        {
            dependency.AssetID = dependency.ObjectID.ToRuntimeObjectGuid();
        }
        dependency.Hash = source.Content;
        dependency.ExactArtifact = source.ExactArtifact;
        dependency.InterfaceHash = source.SemanticInterface;
        dependency.InterfaceVersion = source.InterfaceVersion;
        dependency.Origin = DescribeOrigin(source.Origin);
        manifest.Dependencies.Add(MoveTemp(dependency));
    }
    ArtifactKeyComponent fingerprintComponent;
    fingerprintComponent.Name = "prepared-input";
    fingerprintComponent.Type = "artifact-key";
    fingerprintComponent.Value = prepared.InputFingerprint.ToString();
    manifest.KeyComponents.Add(MoveTemp(fingerprintComponent));

    ArtifactStoragePath manifestPath;
    if (ArtifactStore::TryGetManifestPath(libraryRoot, request.Target, prepared.AssetID, manifestPath, diagnostic))
        return true;
    if (NativeFileExists(manifestPath.Get()))
    {
        StringAnsi oldJson;
        ArtifactManifest oldManifest;
        AssetPipelineDiagnostic ignored;
#if PLATFORM_WINDOWS
        const String readableManifestPath = ToNativePath(manifestPath.Get());
#else
        const String readableManifestPath = manifestPath.Get();
#endif
        if (!File::ReadAllText(readableManifestPath, oldJson) && !ArtifactManifest::Parse(oldJson, manifestPath.Get(), oldManifest, ignored) && oldManifest.ObjectID == prepared.ObjectID)
        {
            const bool sameBuildInputs = oldManifest.DatabaseRevision == manifest.DatabaseRevision &&
                oldManifest.ProcessorID == manifest.ProcessorID &&
                oldManifest.ProcessorImplementationVersion == manifest.ProcessorImplementationVersion &&
                oldManifest.Target.BuildKey(ArtifactTargetDimension::All) == manifest.Target.BuildKey(ArtifactTargetDimension::All) &&
                oldManifest.InputFingerprint == manifest.InputFingerprint &&
                oldManifest.SourceHash == manifest.SourceHash && oldManifest.SettingsHash == manifest.SettingsHash;
            if (sameBuildInputs)
            {
                for (const ArtifactManifestOutput& oldOutput : oldManifest.Outputs)
                {
                    bool replaced = false;
                    for (const ArtifactManifestOutput& output : manifest.Outputs)
                    {
                        if (output.Kind == oldOutput.Kind)
                        {
                            replaced = true;
                            break;
                        }
                    }
                    if (!replaced)
                        manifest.Outputs.Add(oldOutput);
                }
                manifest.PreviousSuccessfulInputFingerprint = oldManifest.PreviousSuccessfulInputFingerprint;
            }
            else
            {
                manifest.PreviousSuccessfulInputFingerprint = oldManifest.InputFingerprint;
            }
        }
    }
    StringAnsi manifestJson;
    if (manifest.ToJson(manifestJson, diagnostic))
        return true;
    const String manifestDirectory = StringUtils::GetDirectoryName(manifestPath.Get());
    if (EnsureNativeDirectory(manifestDirectory))
        return PublicationFail(diagnostic, AssetPipelineDiagnosticCode::LibraryCreationFailed, prepared, manifestDirectory, TEXT("Cannot create artifact manifest directory."));
    const String stagingManifest = manifestPath.Get() + TEXT(".stage-") + Guid::New().ToString(Guid::FormatType::N);
#if PLATFORM_WINDOWS
    const String stagingManifestIo = ToNativePath(stagingManifest);
#else
    const String stagingManifestIo = stagingManifest;
#endif
    SCOPE_EXIT { FileSystem::DeleteFile(stagingManifestIo); };
    if (File::WriteAllBytes(stagingManifestIo, manifestJson.Get(), manifestJson.Length()))
        return PublicationFail(diagnostic, AssetPipelineDiagnosticCode::ArtifactInvalid, prepared, stagingManifest, TEXT("Cannot write artifact manifest staging file."));
    if (Inject(request.FailurePoint, ArtifactPublicationFailurePoint::AfterManifestTempWrite, prepared, diagnostic))
        return true;
    if (FlushFile(stagingManifest))
        return PublicationFail(diagnostic, AssetPipelineDiagnosticCode::ArtifactInvalid, prepared, stagingManifest, TEXT("Cannot flush artifact manifest staging file."));
    if (Inject(request.FailurePoint, ArtifactPublicationFailurePoint::AfterManifestFlush, prepared, diagnostic))
        return true;
    StringAnsi reparsedJson;
    ArtifactManifest reparsed;
    if (File::ReadAllText(stagingManifestIo, reparsedJson) || ArtifactManifest::Parse(reparsedJson, stagingManifest, reparsed, diagnostic) ||
        reparsed.ObjectID != manifest.ObjectID || reparsed.InputFingerprint != manifest.InputFingerprint || reparsed.Outputs.Count() != manifest.Outputs.Count())
        return PublicationFail(diagnostic, AssetPipelineDiagnosticCode::ArtifactInvalid, prepared, stagingManifest, TEXT("Artifact manifest staging validation failed."));
    request.QueryCurrentState(currentRevision, currentFingerprint);
    if (currentRevision != prepared.DatabaseRevision || currentFingerprint != prepared.InputFingerprint)
    {
        result.WasSuperseded = true;
        context.Cancel();
        cleanupRequired = false;
        return false;
    }
    if (Inject(request.FailurePoint, ArtifactPublicationFailurePoint::BeforeAtomicReplace, prepared, diagnostic))
        return true;
    if (AtomicReplace(manifestPath.Get(), stagingManifest))
        return PublicationFail(diagnostic, AssetPipelineDiagnosticCode::ArtifactInvalid, prepared, manifestPath.Get(), TEXT("Cannot atomically replace current artifact manifest."));
    if (PersistPublication(libraryRoot, manifest, manifestJson, diagnostic))
        return true;
    result.Manifest = manifest;
    if (Inject(request.FailurePoint, ArtifactPublicationFailurePoint::AfterAtomicReplaceBeforeNotification, prepared, diagnostic))
        return true;
    if (request.Notify.IsBinded())
    {
        request.Notify(result.Manifest);
        result.NotificationSent = true;
    }
    if (Inject(request.FailurePoint, ArtifactPublicationFailurePoint::DuringCleanup, prepared, diagnostic))
        return true;
    context.Cancel();
    cleanupRequired = false;
    return false;
}
