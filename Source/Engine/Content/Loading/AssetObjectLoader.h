// Copyright (c) Wojciech Figat. All rights reserved.

#pragma once

#include "LoadedAssetRegistry.h"
#include "Engine/Content/Artifacts/ArtifactKey.h"
#include "Engine/Content/Build/RuntimeAssetCatalog.h"

enum class AssetObjectLoadMode : byte
{
    Editor,
    Cooked,
};

enum class AssetObjectStorageKind : byte
{
    EditorArtifact,
    RuntimePackage,
};

/// <summary>Resolved physical bytes and recorded dependencies for one exact object.</summary>
struct FLAXENGINE_API AssetObjectLoadLocation
{
    AssetObjectId Object;
    Guid InstanceID = Guid::Empty;
    AssetObjectStorageKind StorageKind = AssetObjectStorageKind::EditorArtifact;
    StringAnsi TypeName;
    String SourceName;
    String StorageName;
    uint64 Offset = 0;
    uint64 Size = 0;
    byte Compression = 0;
    ContentHash Content;
    ArtifactKey Artifact;
    uint64 Revision = 0;
    Array<AssetObjectId> Dependencies;
};

/// <summary>Injectable editor bridge to object entries in the artifact resolver.</summary>
class FLAXENGINE_API IEditorAssetObjectResolver
{
public:
    virtual ~IEditorAssetObjectResolver() = default;
    virtual bool ResolveArtifactObject(const AssetObjectId& object, AssetObjectLoadLocation& location,
        AssetPipelineDiagnostic& diagnostic) = 0;
};

/// <summary>Injectable cooked bridge to the immutable runtime object catalog.</summary>
class FLAXENGINE_API IRuntimeAssetObjectResolver
{
public:
    virtual ~IRuntimeAssetObjectResolver() = default;
    virtual bool ResolveCatalogObject(const AssetObjectId& object, AssetObjectLoadLocation& location,
        AssetPipelineDiagnostic& diagnostic) = 0;
};

#if USE_EDITOR
/// <summary>Production editor resolver backed by exact, build-on-demand artifact publication.</summary>
class FLAXENGINE_API EditorArtifactAssetObjectResolver : public IEditorAssetObjectResolver
{
public:
    bool ResolveArtifactObject(const AssetObjectId& object, AssetObjectLoadLocation& location,
        AssetPipelineDiagnostic& diagnostic) override;
};
#endif

/// <summary>Factory bridge that materializes resolved object bytes without constraining the concrete Asset type.</summary>
class FLAXENGINE_API IAssetObjectFactory
{
public:
    virtual ~IAssetObjectFactory() = default;
    virtual bool CreateObject(const AssetObjectLoadLocation& location, void*& instance,
        AssetPipelineDiagnostic& diagnostic) = 0;
    virtual void DestroyObject(void* instance) = 0;
};

/// <summary>Default cooked resolver backed directly by RuntimeAssetCatalog composite lookup.</summary>
class FLAXENGINE_API RuntimeCatalogAssetObjectResolver : public IRuntimeAssetObjectResolver
{
private:
    const RuntimeAssetCatalog& _catalog;
    uint64 _revision;

public:
    RuntimeCatalogAssetObjectResolver(const RuntimeAssetCatalog& catalog, uint64 revision)
        : _catalog(catalog)
        , _revision(revision)
    {
    }

    bool ResolveCatalogObject(const AssetObjectId& object, AssetObjectLoadLocation& location,
        AssetPipelineDiagnostic& diagnostic) override;
};

/// <summary>Object-level load result that retains its requested identity even when unresolved.</summary>
struct FLAXENGINE_API AssetObjectLoadResult
{
    AssetObjectId Object;
    LoadedAssetState State = LoadedAssetState::Unresolved;
    void* Instance = nullptr;
    uint64 Revision = 0;
};

/// <summary>Routes editor and cooked object resolution and deduplicates concurrent materialization.</summary>
class FLAXENGINE_API AssetObjectLoader : public NonCopyable
{
private:
    LoadedAssetRegistry& _registry;
    IEditorAssetObjectResolver* _editorResolver = nullptr;
    IRuntimeAssetObjectResolver* _runtimeResolver = nullptr;
    IAssetObjectFactory& _factory;
    AssetObjectLoadMode _mode;

    bool Resolve(const AssetObjectId& object, AssetObjectLoadLocation& location,
        AssetPipelineDiagnostic& diagnostic);

public:
    AssetObjectLoader(LoadedAssetRegistry& registry, IEditorAssetObjectResolver& resolver, IAssetObjectFactory& factory)
        : _registry(registry)
        , _editorResolver(&resolver)
        , _factory(factory)
        , _mode(AssetObjectLoadMode::Editor)
    {
    }

    AssetObjectLoader(LoadedAssetRegistry& registry, IRuntimeAssetObjectResolver& resolver, IAssetObjectFactory& factory)
        : _registry(registry)
        , _runtimeResolver(&resolver)
        , _factory(factory)
        , _mode(AssetObjectLoadMode::Cooked)
    {
    }

    /// <summary>Loads or joins the in-flight request for an exact object ID. Returns true on failure.</summary>
    bool Load(const AssetObjectId& object, AssetObjectLoadResult& result, AssetPipelineDiagnostic& diagnostic);

    /// <summary>Prepares an exact replacement without publishing it to the loaded registry.</summary>
    bool PrepareReplacement(const AssetObjectId& object, uint64 expectedRevision, LoadedAssetReplacement& replacement,
        AssetPipelineDiagnostic& diagnostic);

    void DiscardInstance(void* instance);
};
