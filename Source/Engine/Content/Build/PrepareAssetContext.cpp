// Copyright (c) Wojciech Figat. All rights reserved.

#include "PrepareAssetContext.h"
#include "Engine/Content/AssetDatabase/AssetPath.h"
#include "Engine/Platform/File.h"
#include <algorithm>

namespace
{
    void SetPrepareFailure(AssetPipelineDiagnostic& diagnostic, AssetPipelineDiagnosticCode code, const AssetRecord& record, const AssetProcessorDescriptor& descriptor, const StringView& message)
    {
        diagnostic = AssetPipelineDiagnostic();
        diagnostic.Code = code;
        diagnostic.Stage = AssetPipelineDiagnosticStage::Prepare;
        diagnostic.AssetGuid = record.ID;
        diagnostic.SourcePath = record.SourcePath.Get();
        diagnostic.ProcessorId = descriptor.ID;
        diagnostic.Message = message;
    }

    bool SameDependencyValue(const AssetDependency& a, const AssetDependency& b)
    {
        return a.Kind == b.Kind && a.StableIdentity == b.StableIdentity && a.AssetID == b.AssetID &&
               a.Content == b.Content && a.ExactArtifact == b.ExactArtifact &&
               a.SemanticInterface == b.SemanticInterface && a.InterfaceVersion == b.InterfaceVersion;
    }
}

PrepareAssetContext::PrepareAssetContext(const StringView& projectRoot, const StringView& contentRoot, const StringView& libraryRoot,
    const AssetRecord& record, const AssetProcessorDescriptor& descriptor, const StringAnsiView& normalizedSettings,
    SourceHashCache& hashCache, const AssetCancellationToken& cancellation, uint64 maximumSourceBytes, int32 maximumInputs)
    : _projectRoot(projectRoot)
    , _contentRoot(contentRoot)
    , _libraryRoot(libraryRoot)
    , _record(record)
    , _descriptor(descriptor)
    , _settings(normalizedSettings)
    , _hashCache(&hashCache)
    , _cancellation(cancellation)
    , _maximumSourceBytes(maximumSourceBytes)
    , _maximumInputs(maximumInputs)
{
}

bool PrepareAssetContext::CheckCancellation(AssetPipelineDiagnostic& diagnostic) const
{
    if (!_cancellation.IsCancellationRequested())
        return false;
    SetPrepareFailure(diagnostic, AssetPipelineDiagnosticCode::BuildCancelled, _record, _descriptor, TEXT("Asset preparation was cancelled."));
    return true;
}

bool PrepareAssetContext::AddDependency(AssetDependency dependency, AssetPipelineDiagnostic& diagnostic)
{
    for (const AssetDependency& existing : _declaredDependencies)
    {
        if (existing.Kind != dependency.Kind || existing.StableIdentity != dependency.StableIdentity || existing.AssetID != dependency.AssetID)
            continue;
        if (SameDependencyValue(existing, dependency))
            return false;
        SetPrepareFailure(diagnostic, AssetPipelineDiagnosticCode::UndeclaredInput, _record, _descriptor, TEXT("A dependency identity was declared with conflicting semantic content."));
        diagnostic.Location.File = dependency.Origin.Path;
        diagnostic.Related.Add(existing.Origin.Path);
        return true;
    }
    if (_declaredDependencies.Count() >= _maximumInputs)
    {
        SetPrepareFailure(diagnostic, AssetPipelineDiagnosticCode::ResourceLimitExceeded, _record, _descriptor, TEXT("Asset preparation exceeded the declared input-count limit."));
        return true;
    }
    _declaredDependencies.Add(MoveTemp(dependency));
    return false;
}

bool PrepareAssetContext::ReadSourceFile(const StringView& path, Array<byte>& data, ContentHash& hash, const AssetDependencyOrigin& origin, AssetPipelineDiagnostic& diagnostic)
{
    data.Clear();
    hash = ContentHash();
    if (CheckCancellation(diagnostic))
        return true;

    AssetPathPolicy::ProjectPath normalized;
    if (AssetPathPolicy::TryNormalizeProjectPath(_projectRoot, _contentRoot, _libraryRoot, path, normalized, diagnostic))
    {
        diagnostic.Stage = AssetPipelineDiagnosticStage::Prepare;
        diagnostic.AssetGuid = _record.ID;
        diagnostic.ProcessorId = _descriptor.ID;
        return true;
    }

    SourceHashFileState state;
    if (_hashCache->HashFile(normalized.AbsolutePath, hash, state, diagnostic))
    {
        diagnostic.AssetGuid = _record.ID;
        diagnostic.ProcessorId = _descriptor.ID;
        return true;
    }
    if (state.Size > _maximumSourceBytes || _sourceBytesRead > _maximumSourceBytes - state.Size)
    {
        SetPrepareFailure(diagnostic, AssetPipelineDiagnosticCode::ResourceLimitExceeded, _record, _descriptor, TEXT("Asset preparation exceeded the source-byte limit."));
        diagnostic.SourcePath = normalized.AbsolutePath;
        return true;
    }
    if (File::ReadAllBytes(normalized.AbsolutePath, data))
    {
        SetPrepareFailure(diagnostic, AssetPipelineDiagnosticCode::SourceBusy, _record, _descriptor, TEXT("Cannot read declared source bytes."));
        diagnostic.SourcePath = normalized.AbsolutePath;
        return true;
    }
    const ContentHash bytesHash = ContentHash::Compute(data.Get(), data.Count());
    if (bytesHash != hash)
    {
        data.Clear();
        SetPrepareFailure(diagnostic, AssetPipelineDiagnosticCode::SourceBusy, _record, _descriptor, TEXT("Source changed between hashing and the controlled read."));
        diagnostic.SourcePath = normalized.AbsolutePath;
        return true;
    }

    AssetDependency dependency;
    dependency.Kind = AssetDependencyKind::SourceFile;
    dependency.StableIdentity = normalized.ProjectRelativePath;
    dependency.Content = hash;
    dependency.Origin = origin;
    if (dependency.Origin.Path.IsEmpty())
        dependency.Origin.Path = normalized.ProjectRelativePath;
    if (AddDependency(MoveTemp(dependency), diagnostic))
    {
        data.Clear();
        return true;
    }
    _sourceBytesRead += state.Size;
    return false;
}

bool PrepareAssetContext::DeclareBuildInput(const StringView& stableIdentity, const Guid& id, const ArtifactKey& exactArtifact, const AssetSemanticInterface& semanticInterface, const AssetDependencyOrigin& origin, AssetPipelineDiagnostic& diagnostic)
{
    if (CheckCancellation(diagnostic))
        return true;
    AssetDependency dependency;
    dependency.Kind = AssetDependencyKind::BuildInput;
    dependency.StableIdentity = stableIdentity;
    dependency.AssetID = id;
    dependency.ExactArtifact = exactArtifact;
    dependency.SemanticInterface = semanticInterface.Hash;
    dependency.InterfaceVersion = semanticInterface.Version;
    dependency.Origin = origin;
    return AddDependency(MoveTemp(dependency), diagnostic);
}

bool PrepareAssetContext::DeclareRuntimeReference(const StringView& stableIdentity, const Guid& id, const AssetDependencyOrigin& origin, AssetPipelineDiagnostic& diagnostic)
{
    if (CheckCancellation(diagnostic))
        return true;
    AssetDependency dependency;
    dependency.Kind = AssetDependencyKind::RuntimeReference;
    dependency.StableIdentity = stableIdentity;
    dependency.AssetID = id;
    dependency.Origin = origin;
    return AddDependency(MoveTemp(dependency), diagnostic);
}

bool PrepareAssetContext::DeclareToolchain(const StringView& stableIdentity, const ContentHash& semanticIdentity, const AssetDependencyOrigin& origin, AssetPipelineDiagnostic& diagnostic)
{
    if (CheckCancellation(diagnostic))
        return true;
    AssetDependency dependency;
    dependency.Kind = AssetDependencyKind::Toolchain;
    dependency.StableIdentity = stableIdentity;
    dependency.Content = semanticIdentity;
    dependency.Origin = origin;
    return AddDependency(MoveTemp(dependency), diagnostic);
}

bool PrepareAssetContext::DeclareOutput(const StringAnsiView& kind, const Guid& effectiveAssetId, AssetPipelineDiagnostic& diagnostic)
{
    if (CheckCancellation(diagnostic))
        return true;
    const AssetProcessorOutputDescriptor* selected = nullptr;
    for (const AssetProcessorOutputDescriptor& output : _descriptor.Outputs)
    {
        if (output.Kind == kind)
        {
            selected = &output;
            break;
        }
    }
    if (!selected)
    {
        SetPrepareFailure(diagnostic, AssetPipelineDiagnosticCode::UndeclaredInput, _record, _descriptor, TEXT("Processor attempted to produce an output kind absent from its descriptor."));
        diagnostic.OutputKind = String(kind);
        return true;
    }
    for (const DeclaredArtifactOutput& output : _declaredOutputs)
    {
        if (output.Kind == kind)
        {
            SetPrepareFailure(diagnostic, AssetPipelineDiagnosticCode::InvalidMeta, _record, _descriptor, TEXT("Processor declared the same output kind more than once."));
            diagnostic.OutputKind = String(kind);
            return true;
        }
    }
    DeclaredArtifactOutput output;
    output.Kind = selected->Kind;
    output.Extension = selected->Extension;
    output.FormatVersion = selected->FormatVersion;
    output.TargetDimensions = selected->TargetDimensions;
    output.CompatibilityTag = selected->CompatibilityTag;
    output.EffectiveAssetID = effectiveAssetId.IsValid() ? effectiveAssetId : _record.ID;
    _declaredOutputs.Add(MoveTemp(output));
    return false;
}

void PrepareAssetContext::ReportDiagnostic(const AssetPipelineDiagnostic& diagnostic)
{
    _diagnostics.Add(diagnostic);
}

bool PrepareAssetContext::Finalize(uint64 currentDatabaseRevision, PreparedAsset& prepared, AssetPipelineDiagnostic& diagnostic)
{
    if (CheckCancellation(diagnostic))
        return true;
    if (currentDatabaseRevision != _record.DatabaseRevision)
    {
        SetPrepareFailure(diagnostic, AssetPipelineDiagnosticCode::PrepareInvalidated, _record, _descriptor, TEXT("The asset database changed while preparation was in progress."));
        return true;
    }
    if (_declaredOutputs.IsEmpty())
    {
        SetPrepareFailure(diagnostic, AssetPipelineDiagnosticCode::InvalidMeta, _record, _descriptor, TEXT("Processor preparation did not declare any artifact output."));
        return true;
    }

    Array<AssetDependency> dependencies(_declaredDependencies);
    if (AssetDependency::NormalizeAndSort(dependencies, diagnostic))
    {
        diagnostic.AssetGuid = _record.ID;
        diagnostic.ProcessorId = _descriptor.ID;
        return true;
    }
    Array<SubAssetCandidate> subAssets(prepared.SubAssets);
    for (SubAssetCandidate& candidate : subAssets)
    {
        candidate.StableKey = SubAssetPolicy::NormalizeKey(candidate.StableKey);
        if (!SubAssetPolicy::IsKeyValid(candidate.StableKey))
        {
            SetPrepareFailure(diagnostic, AssetPipelineDiagnosticCode::SubAssetReconcileRequired, _record, _descriptor, TEXT("Processor preparation returned an invalid subasset stable key."));
            return true;
        }
        for (String& previous : candidate.PreviousKeys)
            previous = SubAssetPolicy::NormalizeKey(previous);
        std::sort(candidate.PreviousKeys.Get(), candidate.PreviousKeys.Get() + candidate.PreviousKeys.Count());
    }
    std::sort(subAssets.Get(), subAssets.Get() + subAssets.Count(), [](const SubAssetCandidate& a, const SubAssetCandidate& b)
    {
        return a.StableKey < b.StableKey;
    });
    for (int32 i = 1; i < subAssets.Count(); i++)
    {
        if (subAssets[i - 1].StableKey == subAssets[i].StableKey)
        {
            SetPrepareFailure(diagnostic, AssetPipelineDiagnosticCode::SubAssetReconcileRequired, _record, _descriptor, TEXT("Processor preparation returned duplicate subasset stable keys."));
            return true;
        }
    }

    const uint64 payloadBytes = prepared.Payload ? prepared.Payload->GetMemoryUsage() : 0;
    const uint64 memoryEstimate = Math::Max(prepared.MemoryEstimate, payloadBytes);
    if (memoryEstimate > _maximumSourceBytes)
    {
        SetPrepareFailure(diagnostic, AssetPipelineDiagnosticCode::ResourceLimitExceeded, _record, _descriptor, TEXT("Prepared processor state exceeded the configured memory limit."));
        return true;
    }

    prepared.ObjectID = AssetObjectId(AssetGuid(_record.SourceAssetID), _record.LocalId);
    prepared.AssetID = _record.ID;
    prepared.OutputType = _record.TypeName.IsEmpty() ? _descriptor.MainOutputType : _record.TypeName;
    prepared.DatabaseRevision = _record.DatabaseRevision;
    prepared.SettingsHash = ContentHash::Compute(_settings.Get(), _settings.Length());
    prepared.Outputs = _declaredOutputs;
    std::sort(prepared.Outputs.Get(), prepared.Outputs.Get() + prepared.Outputs.Count(), [](const DeclaredArtifactOutput& a, const DeclaredArtifactOutput& b)
    {
        return a.Kind < b.Kind;
    });
    prepared.Dependencies = MoveTemp(dependencies);
    prepared.SubAssets = MoveTemp(subAssets);
    prepared.MemoryEstimate = memoryEstimate;

    ArtifactKeyBuilder builder(StringAnsiView("flax-prepared-input-v1"));
    builder.AddGuid(StringAnsiView("asset-guid"), prepared.ObjectID.Asset.Value);
    builder.AddUInt64(StringAnsiView("asset-file-id"), static_cast<uint64>(prepared.ObjectID.LocalId));
    builder.AddString(StringAnsiView("processor-id"), _descriptor.ID);
    builder.AddUInt32(StringAnsiView("processor-api"), _descriptor.EngineApiLevel);
    builder.AddUInt32(StringAnsiView("settings-schema-version"), _descriptor.SettingsSchemaVersion);
    builder.AddUInt32(StringAnsiView("implementation-version"), _descriptor.ImplementationVersion);
    builder.AddHash(StringAnsiView("settings"), prepared.SettingsHash);
    if (_descriptor.ProviderKind == AssetProcessorProviderKind::ThirdParty)
        builder.AddHash(StringAnsiView("provider-semantic-identity"), _descriptor.ProviderSemanticIdentity);
    int32 dependencyIndex = 0;
    for (const AssetDependency& dependency : prepared.Dependencies)
    {
        if (dependency.AffectsBuildKey())
            dependency.AppendKeyComponents(builder, dependencyIndex++);
    }
    builder.AddUInt32(StringAnsiView("output-count"), prepared.Outputs.Count());
    for (int32 i = 0; i < prepared.Outputs.Count(); i++)
    {
        const DeclaredArtifactOutput& output = prepared.Outputs[i];
        const StringAnsi prefix = StringAnsi::Format("output-{0}-", i);
        builder.AddString(prefix + "kind", output.Kind);
        builder.AddUInt32(prefix + "format-version", output.FormatVersion);
        builder.AddUInt32(prefix + "target-dimensions", static_cast<uint32>(output.TargetDimensions));
        builder.AddString(prefix + "compatibility", output.CompatibilityTag);
        builder.AddGuid(prefix + "asset-id", output.EffectiveAssetID);
    }
    builder.AddUInt32(StringAnsiView("subasset-count"), prepared.SubAssets.Count());
    for (int32 i = 0; i < prepared.SubAssets.Count(); i++)
    {
        const SubAssetCandidate& candidate = prepared.SubAssets[i];
        const StringAnsi prefix = StringAnsi::Format("subasset-{0}-", i);
        builder.AddString(prefix + "stable-key", candidate.StableKey);
        builder.AddString(prefix + "type", candidate.TypeName);
        builder.AddString(prefix + "display-name", candidate.DisplayName);
        builder.AddBool(prefix + "rename-evidence", candidate.RenameEvidenceReliable);
        Array<StringAnsi> previousKeys;
        for (const String& previous : candidate.PreviousKeys)
            previousKeys.Add(StringAnsi(previous));
        builder.AddSortedStrings(prefix + "previous-keys", previousKeys);
    }
    prepared.InputFingerprint = builder.Finalize();
    diagnostic = AssetPipelineDiagnostic();
    return false;
}
