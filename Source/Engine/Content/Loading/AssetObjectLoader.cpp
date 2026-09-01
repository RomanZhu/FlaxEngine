// Copyright (c) Wojciech Figat. All rights reserved.

#include "AssetObjectLoader.h"
#include "Engine/Core/Collections/HashSet.h"
#if USE_EDITOR
#include "Engine/Content/Artifacts/ArtifactResolver.h"
#include "Engine/Content/AssetDatabase/AssetDatabase.h"
#endif

namespace
{
    bool Fail(AssetPipelineDiagnostic& diagnostic, AssetPipelineDiagnosticCode code, const Guid& object,
        const StringView& message)
    {
        diagnostic = AssetPipelineDiagnostic();
        diagnostic.Code = code;
        diagnostic.Stage = AssetPipelineDiagnosticStage::Resolution;
        diagnostic.AssetGuid = object;
        diagnostic.Message = message;
        return true;
    }

    void CopyResult(const LoadedAssetRecord& record, AssetObjectLoadResult& result)
    {
        result.Object = record.Object;
        result.State = record.State;
        result.Instance = record.Instance;
        result.Revision = record.Revision;
    }
}

#if USE_EDITOR
bool EditorArtifactAssetObjectResolver::ResolveArtifactObject(const Guid& object,
    AssetObjectLoadLocation& location, AssetPipelineDiagnostic& diagnostic)
{
    location = AssetObjectLoadLocation();
    location.Object = object;
    AssetRecord record;
    if (!AssetDatabase::Get().TryGetRecord(object, record))
        return Fail(diagnostic, AssetPipelineDiagnosticCode::SourceMissing, object,
            TEXT("Asset database has no exact record for the requested asset object."));
    if (!ArtifactResolver::Get().IsConfigured())
        return Fail(diagnostic, AssetPipelineDiagnosticCode::ArtifactMissing, object,
            TEXT("Editor artifact resolver is not configured."));

    const AssetObjectId storageObject(AssetGuid(record.SourceAssetID), record.LocalId);
    ArtifactRequest request;
    request.Object = storageObject;
    request.Target = ArtifactResolver::Get().GetDefaultTarget();
    request.OutputKind = "runtime";
    request.Policy = ArtifactResolvePolicy::Exact;
    AssetLoadLocation resolved;
    if (ArtifactResolver::Get().ResolveLoadLocation(request, resolved, diagnostic))
        return true;
    if (resolved.Info.ObjectID != record.ID || !resolved.Artifact.IsExact || resolved.Artifact.IsLastGood ||
        resolved.Artifact.StorageKind != ArtifactStorageKind::Generated)
        return Fail(diagnostic, AssetPipelineDiagnosticCode::ArtifactInvalid, object,
            TEXT("Editor artifact resolution did not return the exact requested object publication."));

    ArtifactKey artifact;
    if (ArtifactKey::Parse(resolved.Artifact.Key, artifact))
        return Fail(diagnostic, AssetPipelineDiagnosticCode::ArtifactInvalid, object,
            TEXT("Editor artifact resolution returned an invalid output key."));
    location.StorageKind = AssetObjectStorageKind::EditorArtifact;
    location.StorageObject = storageObject;
    location.InstanceID = record.ID;
    location.TypeName = StringAnsi(record.TypeName);
    location.SourceName = record.CanonicalPath.Get();
    location.StorageName = resolved.Artifact.StoragePath.Get();
    location.Size = resolved.Artifact.Size;
    location.Content = resolved.Artifact.Content;
    location.Artifact = artifact;
    location.Revision = record.DatabaseRevision;
    for (const AssetObjectId& dependency : record.RuntimeReferences)
    {
        AssetRecord dependencyRecord;
        Guid dependencyId;
        if (AssetDatabase::Get().TryGetRecord(dependency, dependencyRecord))
            dependencyId = dependencyRecord.ID;
        else if (dependency.IsMainObject())
            dependencyId = dependency.Asset.Value;
        else
            return Fail(diagnostic, AssetPipelineDiagnosticCode::ArtifactInvalid, object,
                TEXT("Asset record has a runtime dependency without an exact persistent GUID."));
        location.Dependencies.Add(dependencyId);
    }
    diagnostic = AssetPipelineDiagnostic();
    return false;
}
#endif

bool RuntimeCatalogAssetObjectResolver::ResolveCatalogObject(const Guid& object, AssetObjectLoadLocation& location,
    AssetPipelineDiagnostic& diagnostic)
{
    location = AssetObjectLoadLocation();
    location.Object = object;
    RuntimeAssetCatalogEntry entry;
    if (!_catalog.TryGet(object, entry) || _revision == 0)
        return Fail(diagnostic, AssetPipelineDiagnosticCode::ArtifactMissing, object,
            TEXT("Runtime catalog has no exact entry for the requested asset object."));
    location.InstanceID = entry.Object;
    location.StorageObject = AssetObjectId::Main(AssetGuid(entry.Object));
    location.StorageKind = AssetObjectStorageKind::RuntimePackage;
    location.TypeName = entry.TypeName;
    location.StorageName = String(entry.PackageName);
    location.SourceName = location.StorageName;
    location.Offset = entry.Offset;
    location.Size = entry.Size;
    location.Compression = static_cast<byte>(entry.Compression);
    location.Content = entry.Content;
    location.Revision = _revision;
    location.Dependencies.EnsureCapacity(entry.Dependencies.Count());
    for (const Guid& dependency : entry.Dependencies)
        location.Dependencies.Add(dependency);
    diagnostic = AssetPipelineDiagnostic();
    return false;
}

bool AssetObjectLoader::Resolve(const Guid& object, AssetObjectLoadLocation& location,
    AssetPipelineDiagnostic& diagnostic)
{
    const bool failed = _mode == AssetObjectLoadMode::Cooked
        ? _runtimeResolver->ResolveCatalogObject(object, location, diagnostic)
        : _editorResolver->ResolveArtifactObject(object, location, diagnostic);
    if (failed)
    {
        if (diagnostic.Code == AssetPipelineDiagnosticCode::None)
            Fail(diagnostic, AssetPipelineDiagnosticCode::ArtifactMissing, object,
                TEXT("No exact object location could be resolved."));
        return true;
    }
    const bool storageInvalid = _mode == AssetObjectLoadMode::Cooked
        ? location.StorageKind != AssetObjectStorageKind::RuntimePackage
        : location.StorageKind != AssetObjectStorageKind::EditorArtifact || location.Artifact.IsZero();
    const bool instanceInvalid = _mode == AssetObjectLoadMode::Editor && !location.InstanceID.IsValid();
    if (location.Object != object || !location.StorageObject.IsValid() || instanceInvalid || location.Revision == 0 || location.TypeName.IsEmpty() || location.StorageName.IsEmpty() ||
        location.Size == 0 || location.Content.IsZero() || storageInvalid)
        return Fail(diagnostic, AssetPipelineDiagnosticCode::ArtifactInvalid, object,
            TEXT("Resolved object location has mismatched identity or incomplete immutable storage data."));
    HashSet<Guid> dependencies;
    for (const Guid& dependency : location.Dependencies)
    {
        if (!dependency.IsValid() || dependency == object || !dependencies.Add(dependency))
            return Fail(diagnostic, AssetPipelineDiagnosticCode::ArtifactInvalid, object,
                TEXT("Resolved object location has an invalid, self, or duplicate runtime dependency."));
    }
    return false;
}

bool AssetObjectLoader::Load(const Guid& object, AssetObjectLoadResult& result,
    AssetPipelineDiagnostic& diagnostic)
{
    result = AssetObjectLoadResult();
    result.Object = object;
    LoadedAssetLoadTicket ticket;
    LoadedAssetRecord record;
    const LoadedAssetAcquireResult acquire = _registry.AcquireLoad(object, ticket, record, diagnostic);
    if (acquire == LoadedAssetAcquireResult::Invalid)
        return true;
    if (acquire != LoadedAssetAcquireResult::Owner)
    {
        CopyResult(record, result);
        return record.State != LoadedAssetState::Loaded;
    }

    AssetObjectLoadLocation location;
    void* instance = nullptr;
    AssetPipelineDiagnostic loadDiagnostic;
    if (!Resolve(object, location, loadDiagnostic))
    {
        const bool createFailed = _factory.CreateObject(location, instance, loadDiagnostic);
        if ((createFailed || !instance) && loadDiagnostic.Code == AssetPipelineDiagnosticCode::None)
            Fail(loadDiagnostic, AssetPipelineDiagnosticCode::ArtifactInvalid, object,
                TEXT("Object factory could not materialize the resolved object."));
    }
    if (loadDiagnostic.Code != AssetPipelineDiagnosticCode::None || !instance)
    {
        if (instance)
            _factory.DestroyObject(instance);
        instance = nullptr;
    }

    const uint64 revision = instance ? location.Revision : 0;
    Array<Guid> dependencies;
    if (instance)
        dependencies = location.Dependencies;
    AssetPipelineDiagnostic completionDiagnostic;
    if (_registry.CompleteLoad(ticket, instance, revision, dependencies, loadDiagnostic, record, completionDiagnostic))
    {
        if (instance)
            _factory.DestroyObject(instance);
        diagnostic = completionDiagnostic;
        return true;
    }
    CopyResult(record, result);
    diagnostic = record.Diagnostic;
    return record.State != LoadedAssetState::Loaded;
}

bool AssetObjectLoader::PrepareReplacement(const Guid& object, uint64 expectedRevision,
    LoadedAssetReplacement& replacement, AssetPipelineDiagnostic& diagnostic)
{
    replacement = LoadedAssetReplacement();
    replacement.Object = object;
    AssetObjectLoadLocation location;
    if (Resolve(object, location, diagnostic))
        return true;
    if (expectedRevision == 0 || location.Revision != expectedRevision)
        return Fail(diagnostic, AssetPipelineDiagnosticCode::PrepareInvalidated, object,
            TEXT("Resolved replacement does not match the requested database revision."));
    if (_factory.CreateObject(location, replacement.Instance, diagnostic) || !replacement.Instance ||
        diagnostic.Code != AssetPipelineDiagnosticCode::None)
    {
        if (replacement.Instance)
            _factory.DestroyObject(replacement.Instance);
        replacement.Instance = nullptr;
        if (diagnostic.Code == AssetPipelineDiagnosticCode::None)
            Fail(diagnostic, AssetPipelineDiagnosticCode::ArtifactInvalid, object,
                TEXT("Object factory could not materialize the replacement."));
        return true;
    }
    replacement.Revision = location.Revision;
    replacement.Dependencies = location.Dependencies;
    diagnostic = AssetPipelineDiagnostic();
    return false;
}

void AssetObjectLoader::DiscardInstance(void* instance)
{
    if (instance)
        _factory.DestroyObject(instance);
}
