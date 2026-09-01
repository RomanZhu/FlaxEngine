// Copyright (c) Wojciech Figat. All rights reserved.

#pragma once

#include "Engine/Content/AssetInfo.h"
#include "Engine/Content/AssetPipeline/AssetPipelineDiagnostics.h"
#include "Engine/Core/Collections/Array.h"
#include "Engine/Core/Collections/Dictionary.h"

/// <summary>One immutable engine or plugin-owned asset object.</summary>
struct FLAXENGINE_API BuiltinAssetCatalogEntry
{
    AssetInfo Info;
    String Uri;
};

/// <summary>
/// Read-only loading authority for engine and referenced-plugin content.
/// Physical storage paths are implementation details; builtin:// URIs and GUIDs are stable query identities.
/// </summary>
class FLAXENGINE_API BuiltinAssetCatalog
{
private:
    Array<BuiltinAssetCatalogEntry> _entries;
    Array<String> _roots;
    Dictionary<Guid, int32> _byObject;
    Dictionary<String, int32> _byPath;
    Dictionary<String, int32> _byUri;
    int32 _prebuiltRoots = 0;
    int32 _generatedRoots = 0;

public:
    static BuiltinAssetCatalog& Get();

    /// <summary>Builds the immutable catalog from engine and referenced-plugin binary content.</summary>
    /// <returns>True on invalid or colliding built-in content.</returns>
    bool Initialize(AssetPipelineDiagnostic& diagnostic);

    void Dispose();

    bool TryGet(const Guid& runtimeId, AssetInfo& info) const;
    bool TryGetByPath(const StringView& pathOrUri, AssetInfo& info) const;
    bool IsReadOnlyPath(const StringView& pathOrUri) const;
    StringView GetStoragePath(const Guid& objectId) const;
    StringView GetUri(const Guid& objectId) const;
    void GetAll(Array<Guid>& result) const;
    void GetAllByTypeName(const StringView& typeName, Array<Guid>& result) const;

    FORCE_INLINE int32 Count() const
    {
        return _entries.Count();
    }

    /// <summary>Number of roots loaded without scanning asset storage files.</summary>
    FORCE_INLINE int32 PrebuiltRootsCount() const
    {
        return _prebuiltRoots;
    }

    /// <summary>Number of missing catalogs generated atomically during this initialization.</summary>
    FORCE_INLINE int32 GeneratedRootsCount() const
    {
        return _generatedRoots;
    }
};
