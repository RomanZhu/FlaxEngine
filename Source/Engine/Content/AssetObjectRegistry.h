// Copyright (c) Wojciech Figat. All rights reserved.

#pragma once

#include "AssetInfo.h"
#include "Config.h"
#include "Engine/Core/Collections/Dictionary.h"
#include "Engine/Platform/CriticalSection.h"
#if !USE_EDITOR
#include "Engine/Content/Build/RuntimeAssetCatalog.h"
#endif

struct AssetHeader;
struct FlaxStorageReference;
class FlaxStorage;

/// <summary>
/// Resolves exact persistent asset objects to their current load packages.
/// In the editor this contains only transient, editor-private packages. Project assets are
/// resolved by AssetDatabase. Cooked games resolve immutable entries from RuntimeAssetCatalog.
/// </summary>
class FLAXENGINE_API AssetObjectRegistry
{
public:
    struct Entry
    {
        AssetInfo Info;

        Entry()
        {
        }

        Entry(const Guid& runtimeId, const AssetObjectId& objectId, const StringView& typeName, const StringView& path)
            : Info(runtimeId, objectId, typeName, path)
        {
        }
    };

private:
#if USE_EDITOR
    CriticalSection _locker;
    Dictionary<AssetObjectId, Entry> _objects;
    Dictionary<Guid, AssetObjectId> _runtimeObjects;
#else
    RuntimeAssetCatalog _runtimeCatalog;
#endif

public:
    int32 Size() const;
    AssetObjectId GetGameSettingsObject() const;
    void Init();

    /// <summary>Finds one object by its canonical composite identity.</summary>
    bool FindObject(const AssetObjectId& objectId, AssetInfo& info);

    /// <summary>Finds one object by a transient/runtime scripting object identifier.</summary>
    bool FindRuntimeObject(const Guid& runtimeId, AssetInfo& info);

    /// <summary>Finds an object by a registered transient package path or cooked path alias.</summary>
    bool FindObject(const StringView& path, AssetInfo& info);

    /// <summary>Gets an editor-private package path for one exact object.</summary>
    StringView GetEditorObjectPath(const AssetObjectId& objectId) const;

    void GetAllRuntimeIds(Array<Guid, HeapAllocation>& result) const;
    void GetAllRuntimeIdsByTypeName(const StringView& typeName, Array<Guid, HeapAllocation>& result) const;

#if USE_EDITOR
    /// <summary>Registers every object stored in an editor-private package.</summary>
    void RegisterTransientPackage(FlaxStorage* storage);
    void RegisterTransientPackage(const FlaxStorageReference& storage);

    /// <summary>Registers one editor-private runtime object as the main object of its transient package.</summary>
    void RegisterTransientObject(const AssetHeader& header, const StringView& path);
    void RegisterTransientObject(const Guid& runtimeId, const StringView& typeName, const StringView& path);

    bool RemoveTransientObject(const AssetObjectId& objectId, AssetInfo* info);
    bool RemoveTransientRuntimeObject(const Guid& runtimeId, AssetInfo* info);
    bool RemoveTransientPackage(const StringView& path, AssetInfo* info);
    bool RenameTransientPackage(const StringView& oldPath, const StringView& newPath);
    void RenameTransientFolder(const StringView& oldPath, const StringView& newPath);
#endif
};
