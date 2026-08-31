// Copyright (c) Wojciech Figat. All rights reserved.

#pragma once

#include "Engine/Core/Types/String.h"
#include "Engine/Content/AssetDatabase/Identity/AssetObjectId.h"

/// <summary>
/// Contains short information about an asset.
/// </summary>
API_STRUCT() struct AssetInfo
{
    DECLARE_SCRIPTING_TYPE_MINIMAL(AssetInfo);

    /// <summary>
    /// Unique ID.
    /// </summary>
    API_FIELD() Guid ID;

    /// <summary>
    /// Persistent source and local file identity. This is independent from the runtime scripting object ID.
    /// </summary>
    API_FIELD() AssetObjectId ObjectID;

    /// <summary>Database/artifact revision represented by this load record.</summary>
    API_FIELD() uint64 Revision = 0;

    /// <summary>
    /// The stored data full typename. Used to recognize asset type.
    /// </summary>
    API_FIELD() String TypeName;

    /// <summary>
    /// Canonical source or document path. Generated physical storage is carried by AssetLoadLocation.
    /// </summary>
    API_FIELD() String Path;

public:
    /// <summary>
    /// Initializes a new instance of the <see cref="AssetInfo"/> struct.
    /// </summary>
    AssetInfo()
    {
        ID = Guid::Empty;
    }

    /// <summary>
    /// Initializes a new instance of the <see cref="AssetInfo"/> struct.
    /// </summary>
    /// <param name="id">The identifier.</param>
    /// <param name="typeName">The typename identifier.</param>
    /// <param name="path">The path.</param>
    AssetInfo(const Guid& id, const StringView& typeName, const StringView& path)
        : ID(id)
        , ObjectID(AssetObjectId::Main(AssetGuid(id)))
        , TypeName(typeName)
        , Path(path)
    {
    }

    AssetInfo(const Guid& instanceId, const AssetObjectId& objectId, const StringView& typeName, const StringView& path, uint64 revision = 0)
        : ID(instanceId)
        , ObjectID(objectId)
        , Revision(revision)
        , TypeName(typeName)
        , Path(path)
    {
    }

public:
    /// <summary>
    /// Gets the string.
    /// </summary>
    /// <returns>The string.</returns>
    String ToString() const;
};
