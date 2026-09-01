// Copyright (c) Wojciech Figat. All rights reserved.

#pragma once

#include "Engine/Core/Types/Guid.h"
#include "Engine/Core/Types/String.h"

/// <summary>Stable source-root category.</summary>
enum class AssetSourceRootKind : byte
{
    ProjectContent,
    SceneFragments,
    EngineContent,
    PluginContent,
    ExternalReadOnlyContent,
};

/// <summary>Source-root visibility in ordinary asset browsing and search.</summary>
enum class AssetSourceRootVisibility : byte
{
    Public,
    Private,
};

/// <summary>Capabilities granted to callers operating on a source root.</summary>
enum class AssetSourceRootPermission : uint32
{
    None = 0,
    Read = 1 << 0,
    Write = 1 << 1,
    Scan = 1 << 2,
    GenericMutation = 1 << 3,
};

/// <summary>One permission-aware physical source mount and its stable logical prefix.</summary>
struct FLAXENGINE_API AssetSourceRoot
{
    Guid RootId;
    String Name;
    String OwnerPath;
    String PhysicalPath;
    String LogicalPrefix;
    AssetSourceRootKind Kind = AssetSourceRootKind::ProjectContent;
    AssetSourceRootVisibility Visibility = AssetSourceRootVisibility::Public;
    AssetSourceRootPermission Permissions = AssetSourceRootPermission::None;
    bool PublicAssetNamespace = false;
    bool BrowserVisible = false;
    bool RequiresAdjacentMeta = false;

    bool HasPermission(AssetSourceRootPermission permission) const
    {
        return (static_cast<uint32>(Permissions) & static_cast<uint32>(permission)) == static_cast<uint32>(permission);
    }

    bool IsPublic() const
    {
        return Visibility == AssetSourceRootVisibility::Public;
    }
};
