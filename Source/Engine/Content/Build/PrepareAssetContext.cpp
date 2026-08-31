// Copyright (c) Wojciech Figat. All rights reserved.

#include "PrepareAssetContext.h"
#include "Engine/Content/AssetDatabase/AssetDatabase.h"
#include "Engine/Content/AssetDatabase/AssetPath.h"
#include "Engine/Content/AssetDatabase/SubAsset.h"
#include "Engine/Platform/File.h"
#include "Engine/Platform/StringUtils.h"
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
        return a.Kind == b.Kind && a.State == b.State && a.StableIdentity == b.StableIdentity && a.ObjectID == b.ObjectID && a.AssetID == b.AssetID &&
               a.Content == b.Content && a.Metadata == b.Metadata && a.ExactArtifact == b.ExactArtifact &&
               a.SemanticInterface == b.SemanticInterface && a.InterfaceVersion == b.InterfaceVersion;
    }

    ContentHash EmptySetHash()
    {
        static const char Empty[] = "[]";
        return ContentHash::Compute(Empty, ARRAY_COUNT(Empty) - 1);
    }

    AssetObjectId ResolveObjectId(const Guid& backingId)
    {
        AssetRecord record;
        return backingId.IsValid() && AssetDatabase::Get().TryGetRecord(backingId, record) ? record.GetObjectId() : AssetObjectId();
    }

    const char* ReasonCode(AssetDependencyKind kind)
    {
        switch (kind)
        {
        case AssetDependencyKind::ExactSourceFile:
        case AssetDependencyKind::SourceAsset: return "SourceDependencyChanged";
        case AssetDependencyKind::Artifact: return "ArtifactDependencyChanged";
        case AssetDependencyKind::Custom: return "CustomDependencyChanged";
        case AssetDependencyKind::Global:
        case AssetDependencyKind::Target: return "TargetChanged";
        case AssetDependencyKind::ImporterProvider: return "ImporterVersionChanged";
        case AssetDependencyKind::Toolchain: return "ToolchainChanged";
        case AssetDependencyKind::Environment: return "EnvironmentChanged";
        case AssetDependencyKind::LogicalPath: return "LogicalPathChanged";
        case AssetDependencyKind::RuntimeReference: return "RuntimeReferenceObserved";
        default: return "DependencyChanged";
        }
    }
}

PrepareAssetContext::PrepareAssetContext(const StringView& projectRoot, const StringView& contentRoot, const StringView& libraryRoot,
    const AssetRecord& record, const AssetProcessorDescriptor& descriptor, const StringAnsiView& normalizedSettings,
    const ArtifactTarget& target, SourceHashCache& hashCache, const AssetCancellationToken& cancellation, uint64 maximumSourceBytes, int32 maximumInputs)
    : _projectRoot(projectRoot)
    , _contentRoot(contentRoot)
    , _libraryRoot(libraryRoot)
    , _record(record)
    , _descriptor(descriptor)
    , _target(target)
    , _settings(normalizedSettings)
    , _externalRemapsHash(EmptySetHash())
    , _postprocessorHash(EmptySetHash())
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
        if (existing.Kind != dependency.Kind || existing.StableIdentity != dependency.StableIdentity ||
            existing.ObjectID != dependency.ObjectID || existing.AssetID != dependency.AssetID)
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

    AssetPathPolicy::ProjectPath canonicalSource;
    if (AssetPathPolicy::TryNormalizeProjectPath(_projectRoot, _contentRoot, _libraryRoot, _record.SourcePath.Get(), canonicalSource, diagnostic))
    {
        data.Clear();
        diagnostic.Stage = AssetPipelineDiagnosticStage::Prepare;
        diagnostic.AssetGuid = _record.ID;
        diagnostic.ProcessorId = _descriptor.ID;
        return true;
    }
    bool declarationFailed = false;
    if (canonicalSource.ProjectRelativePath == normalized.ProjectRelativePath)
    {
        ArtifactKeyBuilder metadataBuilder(StringAnsiView("flax-source-metadata-fingerprint-v1"));
        metadataBuilder.AddUInt64(StringAnsiView("semantic"), _record.MetaSemanticHash);
        declarationFailed = DeclareSourceAssetByGuid(_record.SourceAssetID.IsValid() ? _record.SourceAssetID : _record.ID,
            hash, metadataBuilder.Finalize().Digest, false, origin, diagnostic);
    }
    else
    {
        declarationFailed = DeclareExactSourceFile(normalized.ProjectRelativePath, hash, false, origin, diagnostic);
    }
    if (declarationFailed)
    {
        data.Clear();
        return true;
    }
    _sourceBytesRead += state.Size;
    return false;
}

bool PrepareAssetContext::DeclareExactSourceFile(const StringView& path, const ContentHash& content, bool missing,
    const AssetDependencyOrigin& origin, AssetPipelineDiagnostic& diagnostic)
{
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
    AssetDependency dependency;
    dependency.Kind = AssetDependencyKind::ExactSourceFile;
    dependency.State = missing ? AssetDependencyState::Missing : AssetDependencyState::Present;
    dependency.StableIdentity = normalized.ProjectRelativePath;
    dependency.Content = content;
    dependency.Origin = origin;
    if (dependency.Origin.Path.IsEmpty())
        dependency.Origin.Path = normalized.ProjectRelativePath;
    return AddDependency(MoveTemp(dependency), diagnostic);
}

bool PrepareAssetContext::DeclareSourceAssetByGuid(const Guid& id, const ContentHash& content, const ContentHash& metadata,
    bool missing, const AssetDependencyOrigin& origin, AssetPipelineDiagnostic& diagnostic)
{
    if (CheckCancellation(diagnostic))
        return true;
    AssetDependency dependency;
    dependency.Kind = AssetDependencyKind::SourceAsset;
    dependency.State = missing ? AssetDependencyState::Missing : AssetDependencyState::Present;
    dependency.StableIdentity = TEXT("guid:") + id.ToString(Guid::FormatType::N).ToLower();
    dependency.ObjectID = ResolveObjectId(id);
    dependency.AssetID = id;
    dependency.Content = content;
    dependency.Metadata = metadata;
    dependency.Origin = origin;
    return AddDependency(MoveTemp(dependency), diagnostic);
}

bool PrepareAssetContext::DeclareSourceAssetByPath(const StringView& path, const ContentHash& content, const ContentHash& metadata,
    bool missing, const AssetDependencyOrigin& origin, AssetPipelineDiagnostic& diagnostic)
{
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
    AssetDependency dependency;
    dependency.Kind = AssetDependencyKind::SourceAsset;
    dependency.State = missing ? AssetDependencyState::Missing : AssetDependencyState::Present;
    dependency.StableIdentity = normalized.ProjectRelativePath;
    dependency.Content = content;
    dependency.Metadata = metadata;
    dependency.Origin = origin;
    return AddDependency(MoveTemp(dependency), diagnostic);
}

bool PrepareAssetContext::DeclareArtifactDependency(const StringView& stableIdentity, const Guid& id, AssetDependencyState selection,
    const ArtifactKey& selectedArtifact, const AssetSemanticInterface& semanticInterface, const AssetDependencyOrigin& origin,
    AssetPipelineDiagnostic& diagnostic, const AssetObjectId& objectId)
{
    if (CheckCancellation(diagnostic))
        return true;
    AssetDependency dependency;
    dependency.Kind = AssetDependencyKind::Artifact;
    dependency.State = selection;
    dependency.StableIdentity = stableIdentity.HasChars() ? String(stableIdentity) : TEXT("guid:") + id.ToString(Guid::FormatType::N).ToLower();
    dependency.ObjectID = objectId.IsValid() ? objectId : ResolveObjectId(id);
    dependency.AssetID = id;
    dependency.ExactArtifact = selectedArtifact;
    dependency.SemanticInterface = semanticInterface.Hash;
    dependency.InterfaceVersion = semanticInterface.Version;
    dependency.Origin = origin;
    return AddDependency(MoveTemp(dependency), diagnostic);
}

bool PrepareAssetContext::DeclareBuildInput(const StringView& stableIdentity, const Guid& id, const ArtifactKey& exactArtifact, const AssetSemanticInterface& semanticInterface, const AssetDependencyOrigin& origin, AssetPipelineDiagnostic& diagnostic)
{
    return DeclareArtifactDependency(stableIdentity, id,
        exactArtifact.IsZero() ? AssetDependencyState::CurrentArtifact : AssetDependencyState::ExactArtifact,
        exactArtifact, semanticInterface, origin, diagnostic);
}

bool PrepareAssetContext::DeclareBuildInput(const StringView& stableIdentity, const AssetObjectId& id, const ArtifactKey& exactArtifact, const AssetSemanticInterface& semanticInterface, const AssetDependencyOrigin& origin, AssetPipelineDiagnostic& diagnostic)
{
    return DeclareArtifactDependency(stableIdentity, SubAssetPolicy::GetBackingAssetId(id.Guid, id.LocalId),
        exactArtifact.IsZero() ? AssetDependencyState::CurrentArtifact : AssetDependencyState::ExactArtifact,
        exactArtifact, semanticInterface, origin, diagnostic, id);
}

bool PrepareAssetContext::DeclareRuntimeReference(const StringView& stableIdentity, const Guid& id, const AssetDependencyOrigin& origin, AssetPipelineDiagnostic& diagnostic)
{
    if (CheckCancellation(diagnostic))
        return true;
    AssetDependency dependency;
    dependency.Kind = AssetDependencyKind::RuntimeReference;
    dependency.StableIdentity = stableIdentity;
    dependency.ObjectID = ResolveObjectId(id);
    dependency.AssetID = id;
    dependency.Origin = origin;
    return AddDependency(MoveTemp(dependency), diagnostic);
}

bool PrepareAssetContext::DeclareRuntimeReference(const StringView& stableIdentity, const AssetObjectId& id, const AssetDependencyOrigin& origin, AssetPipelineDiagnostic& diagnostic)
{
    if (CheckCancellation(diagnostic))
        return true;
    AssetDependency dependency;
    dependency.Kind = AssetDependencyKind::RuntimeReference;
    dependency.StableIdentity = stableIdentity;
    dependency.ObjectID = id;
    dependency.AssetID = SubAssetPolicy::GetBackingAssetId(id.Guid, id.LocalId);
    dependency.Origin = origin;
    return AddDependency(MoveTemp(dependency), diagnostic);
}

bool PrepareAssetContext::DeclareCustomDependency(const StringView& name, const ContentHash& value,
    const AssetDependencyOrigin& origin, AssetPipelineDiagnostic& diagnostic)
{
    if (CheckCancellation(diagnostic))
        return true;
    AssetDependency dependency;
    dependency.Kind = AssetDependencyKind::Custom;
    dependency.StableIdentity = name;
    dependency.Content = value;
    dependency.Origin = origin;
    return AddDependency(MoveTemp(dependency), diagnostic);
}

bool PrepareAssetContext::DeclareGlobalDependency(const StringView& key, const ContentHash& value,
    const AssetDependencyOrigin& origin, AssetPipelineDiagnostic& diagnostic)
{
    if (CheckCancellation(diagnostic))
        return true;
    AssetDependency dependency;
    dependency.Kind = AssetDependencyKind::Global;
    dependency.StableIdentity = key;
    dependency.Content = value;
    dependency.Origin = origin;
    return AddDependency(MoveTemp(dependency), diagnostic);
}

bool PrepareAssetContext::DeclareEnvironmentDependency(const StringView& key, const ContentHash& normalizedValue,
    const AssetDependencyOrigin& origin, AssetPipelineDiagnostic& diagnostic)
{
    if (CheckCancellation(diagnostic))
        return true;
    AssetDependency dependency;
    dependency.Kind = AssetDependencyKind::Environment;
    dependency.StableIdentity = key;
    dependency.Content = normalizedValue;
    dependency.Origin = origin;
    return AddDependency(MoveTemp(dependency), diagnostic);
}

bool PrepareAssetContext::DeclareTargetDependency(ArtifactTargetDimension dimensions, const AssetDependencyOrigin& origin,
    AssetPipelineDiagnostic& diagnostic)
{
    if (CheckCancellation(diagnostic))
        return true;
    AssetDependency dependency;
    dependency.Kind = AssetDependencyKind::Target;
    dependency.StableIdentity = String::Format(TEXT("dimensions:{0}"), static_cast<uint32>(dimensions));
    dependency.Content = _target.BuildKey(dimensions).Digest;
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

bool PrepareAssetContext::DependsOnLogicalPath(const AssetDependencyOrigin& origin, AssetPipelineDiagnostic& diagnostic)
{
    if (CheckCancellation(diagnostic))
        return true;
    AssetPathPolicy::ProjectPath normalized;
    if (AssetPathPolicy::TryNormalizeProjectPath(_projectRoot, _contentRoot, _libraryRoot, _record.SourcePath.Get(), normalized, diagnostic))
    {
        diagnostic.Stage = AssetPipelineDiagnosticStage::Prepare;
        diagnostic.AssetGuid = _record.ID;
        diagnostic.ProcessorId = _descriptor.ID;
        return true;
    }
    AssetDependency dependency;
    dependency.Kind = AssetDependencyKind::LogicalPath;
    dependency.StableIdentity = normalized.ProjectRelativePath;
    dependency.Origin = origin;
    return AddDependency(MoveTemp(dependency), diagnostic);
}

void PrepareAssetContext::SetExternalObjectRemapsFingerprint(const ContentHash& value)
{
    _externalRemapsHash = value;
}

void PrepareAssetContext::SetPostprocessorFingerprint(const ContentHash& value)
{
    _postprocessorHash = value;
}

void PrepareAssetContext::SetSourceSerializerVersion(uint32 value)
{
    _sourceSerializerVersion = value;
}

bool PrepareAssetContext::AddImportReason(AssetImportReasonNode reason, AssetPipelineDiagnostic& diagnostic)
{
    if (reason.Code.IsEmpty() || reason.Parent < -1 || reason.Parent >= _importReasons.Count())
    {
        SetPrepareFailure(diagnostic, AssetPipelineDiagnosticCode::InvalidMeta, _record, _descriptor,
            TEXT("Import reason has an invalid code or parent node."));
        return true;
    }
    _importReasons.Add(MoveTemp(reason));
    return false;
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

    AssetDependencyOrigin automaticOrigin;
    automaticOrigin.Path = _record.SourcePath.Get();
    AssetDependency provider;
    provider.Kind = AssetDependencyKind::ImporterProvider;
    provider.StableIdentity = _descriptor.ProviderID + TEXT("/") + _descriptor.ID;
    provider.Content = _descriptor.ProviderSemanticIdentity;
    provider.Origin = automaticOrigin;
    if (AddDependency(MoveTemp(provider), diagnostic) || DeclareTargetDependency(ArtifactTargetDimension::All, automaticOrigin, diagnostic))
        return true;

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

    prepared.AssetID = _record.ID;
    prepared.ObjectID = _record.GetObjectId();
    prepared.OutputType = _record.TypeName.IsEmpty() ? _descriptor.MainOutputType : _record.TypeName;
    prepared.DatabaseRevision = _record.DatabaseRevision;
    prepared.AssetName = StringUtils::GetFileNameWithoutExtension(_record.SourcePath.Get());
    prepared.StableObjectKey = _record.SubAsset.Get();
    prepared.IsMainObject = _record.IsMainAsset();
    prepared.SourcePath = _record.SourcePath.Get();
    for (const AssetDependency& dependency : dependencies)
    {
        if (dependency.Kind == AssetDependencyKind::SourceAsset && dependency.State == AssetDependencyState::Present)
        {
            prepared.SourceHash = dependency.Content;
            break;
        }
    }
    prepared.SettingsHash = ContentHash::Compute(_settings.Get(), _settings.Length());
    prepared.ExternalRemapsHash = _externalRemapsHash;
    prepared.PostprocessorHash = _postprocessorHash;
    prepared.ProviderCodeHash = _descriptor.ProviderSemanticIdentity;
    prepared.TargetFingerprint = _target.BuildKey(ArtifactTargetDimension::All);
    prepared.Outputs = _declaredOutputs;
    std::sort(prepared.Outputs.Get(), prepared.Outputs.Get() + prepared.Outputs.Count(), [](const DeclaredArtifactOutput& a, const DeclaredArtifactOutput& b)
    {
        return a.Kind < b.Kind;
    });
    prepared.Dependencies = MoveTemp(dependencies);
    prepared.SubAssets = MoveTemp(subAssets);
    prepared.MemoryEstimate = memoryEstimate;

    ArtifactKeyBuilder builder(StringAnsiView("FlaxAssetImportInput/v3"));
    builder.AddGuid(StringAnsiView("asset-id"), prepared.AssetID);
    builder.AddGuid(StringAnsiView("source-file-guid"), _record.SourceAssetID.IsValid() ? _record.SourceAssetID : _record.ID);
    builder.AddHash(StringAnsiView("source-content"), prepared.SourceHash);
    builder.AddString(StringAnsiView("asset-name"), prepared.AssetName);
    builder.AddString(StringAnsiView("asset-basename"), StringUtils::GetFileName(_record.SourcePath.Get()));
    builder.AddString(StringAnsiView("processor-id"), _descriptor.ID);
    builder.AddUInt32(StringAnsiView("engine-import-abi"), _descriptor.EngineApiLevel);
    builder.AddUInt32(StringAnsiView("asset-import-contract"), 3);
    builder.AddUInt32(StringAnsiView("settings-schema-version"), _descriptor.SettingsSchemaVersion);
    builder.AddUInt32(StringAnsiView("implementation-version"), _descriptor.ImplementationVersion);
    builder.AddHash(StringAnsiView("provider-code-hash"), prepared.ProviderCodeHash);
    builder.AddString(StringAnsiView("normalized-importer-settings"), _settings);
    builder.AddHash(StringAnsiView("external-object-remaps"), prepared.ExternalRemapsHash);
    builder.AddHash(StringAnsiView("postprocessor-set-order"), prepared.PostprocessorHash);
    builder.AddKey(StringAnsiView("target-fingerprint"), prepared.TargetFingerprint);
    builder.AddUInt32(StringAnsiView("source-serializer-version"), _sourceSerializerVersion);
    builder.AddUInt64(StringAnsiView("source-meta-semantic"), _record.MetaSemanticHash);
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
    prepared.ImportReasons = _importReasons;
    if (prepared.ImportReasons.IsEmpty())
    {
        AssetImportReasonNode root;
        root.Code = "DesiredInputKeyChanged";
        root.Identity = _record.SourcePath.Get();
        root.CurrentFingerprint = prepared.InputFingerprint.ToString();
        root.Explanation = TEXT("Canonical importer inputs produced a new desired input key.");
        prepared.ImportReasons.Add(MoveTemp(root));
        for (const AssetDependency& dependency : prepared.Dependencies)
        {
            if (!dependency.AffectsBuildKey())
                continue;
            AssetImportReasonNode child;
            child.Parent = 0;
            child.Code = ReasonCode(dependency.Kind);
            child.Identity = dependency.StableIdentity;
            child.CurrentFingerprint = dependency.DescribeFingerprint();
            child.Explanation = dependency.State == AssetDependencyState::Missing
                ? TEXT("The declared dependency is missing; appearance will invalidate this result.")
                : TEXT("The observed dependency fingerprint participates in the desired input key.");
            prepared.ImportReasons.Add(MoveTemp(child));
        }
    }
    diagnostic = AssetPipelineDiagnostic();
    return false;
}
