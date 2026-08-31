// Copyright (c) Wojciech Figat. All rights reserved.

#include "AssetObjectLoader.h"
#include "Engine/Core/Collections/HashSet.h"

namespace
{
    bool Fail(AssetPipelineDiagnostic& diagnostic, AssetPipelineDiagnosticCode code, const AssetObjectId& object,
        const StringView& message)
    {
        diagnostic = AssetPipelineDiagnostic();
        diagnostic.Code = code;
        diagnostic.Stage = AssetPipelineDiagnosticStage::Resolution;
        diagnostic.AssetGuid = object.Asset.Value;
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

bool RuntimeCatalogAssetObjectResolver::ResolveCatalogObject(const AssetObjectId& object, AssetObjectLoadLocation& location,
    AssetPipelineDiagnostic& diagnostic)
{
    location = AssetObjectLoadLocation();
    location.Object = object;
    RuntimeAssetCatalogEntry entry;
    if (!_catalog.TryGet(object, entry) || _revision == 0)
        return Fail(diagnostic, AssetPipelineDiagnosticCode::ArtifactMissing, object,
            TEXT("Runtime catalog has no exact entry for the requested asset object."));
    location.StorageKind = AssetObjectStorageKind::RuntimePackage;
    location.TypeName = entry.TypeName;
    location.StorageName = entry.PackageName;
    location.Offset = entry.Offset;
    location.Size = entry.Size;
    location.Compression = static_cast<byte>(entry.Compression);
    location.Content = entry.Content;
    location.Revision = _revision;
    location.Dependencies = entry.Dependencies;
    diagnostic = AssetPipelineDiagnostic();
    return false;
}

bool AssetObjectLoader::Resolve(const AssetObjectId& object, AssetObjectLoadLocation& location,
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
    if (location.Object != object || location.Revision == 0 || location.TypeName.IsEmpty() || location.StorageName.IsEmpty() ||
        location.Size == 0 || location.Content.IsZero() || storageInvalid)
        return Fail(diagnostic, AssetPipelineDiagnosticCode::ArtifactInvalid, object,
            TEXT("Resolved object location has mismatched identity or incomplete immutable storage data."));
    HashSet<AssetObjectId> dependencies;
    for (const AssetObjectId& dependency : location.Dependencies)
    {
        if (!dependency.IsValid() || dependency == object || !dependencies.Add(dependency))
            return Fail(diagnostic, AssetPipelineDiagnosticCode::ArtifactInvalid, object,
                TEXT("Resolved object location has an invalid, self, or duplicate runtime dependency."));
    }
    return false;
}

bool AssetObjectLoader::Load(const AssetObjectId& object, AssetObjectLoadResult& result,
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
    Array<AssetObjectId> dependencies;
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

bool AssetObjectLoader::PrepareReplacement(const AssetObjectId& object, uint64 expectedRevision,
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
