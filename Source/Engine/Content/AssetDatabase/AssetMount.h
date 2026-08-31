// Copyright (c) Wojciech Figat. All rights reserved.

#pragma once

#include "Engine/Content/AssetPipeline/AssetPipelineDiagnostics.h"
#include "Engine/Core/Collections/Array.h"
#include "Engine/Scripting/ScriptingType.h"
#include "Engine/Threading/Threading.h"

/// <summary>Explicit source mount ownership and permission kind.</summary>
API_ENUM() enum class AssetMountKind : byte
{
    ProjectContent,
    EngineContent,
    PluginContent,
    ExternalReadOnlyContent,
};

/// <summary>One explicit source mount. Logical prefixes are globally unique.</summary>
API_STRUCT() struct FLAXENGINE_API AssetMount
{
    DECLARE_SCRIPTING_TYPE_MINIMAL(AssetMount);

    API_FIELD() Guid MountId = Guid::Empty;
    API_FIELD() String LogicalPrefix;
    API_FIELD() String PhysicalRoot;
    API_FIELD() AssetMountKind Kind = AssetMountKind::ProjectContent;
    API_FIELD() bool Writable = false;
    API_FIELD() bool AllowLinkedRoot = false;
};

/// <summary>Resolved logical and physical identity inside one explicit mount.</summary>
API_STRUCT() struct FLAXENGINE_API AssetMountResolution
{
    DECLARE_SCRIPTING_TYPE_MINIMAL(AssetMountResolution);

    API_FIELD() bool Found = false;
    API_FIELD() AssetMount Mount;
    API_FIELD() String LogicalPath;
    API_FIELD() String RelativePath;
    API_FIELD() String PhysicalPath;
};

/// <summary>Thread-safe explicit mount table with unambiguous bidirectional resolution.</summary>
class FLAXENGINE_API AssetMountTable
{
public:
    bool InitializeProject(const StringView& projectRoot, const StringView& contentRoot, AssetPipelineDiagnostic& diagnostic);
    bool Register(AssetMount mount, AssetPipelineDiagnostic& diagnostic);
    /// <summary>Validates a complete mount snapshot, then replaces the active table atomically.</summary>
    bool ReplaceAll(const Array<AssetMount>& mounts, AssetPipelineDiagnostic& diagnostic);
    void Clear();
    Array<AssetMount> GetMounts() const;
    bool ResolveLogical(const StringView& logicalPath, AssetMountResolution& result, AssetPipelineDiagnostic& diagnostic) const;
    bool ResolvePhysical(const StringView& physicalPath, AssetMountResolution& result, AssetPipelineDiagnostic& diagnostic) const;
    bool IsWritable(const StringView& logicalPath) const;

private:
    mutable CriticalSection _locker;
    Array<AssetMount> _mounts;
};

/// <summary>Process mount registry used by the database, importers, editor, and command line.</summary>
API_CLASS(Static) class FLAXENGINE_API AssetMountRegistry
{
    DECLARE_SCRIPTING_TYPE_NO_SPAWN(AssetMountRegistry);

public:
    static AssetMountTable& Get();
    API_FUNCTION() static Array<AssetMount> GetMounts();
    API_FUNCTION() static bool TryResolveLogical(const StringView& logicalPath, API_PARAM(Out) AssetMountResolution& result);
    API_FUNCTION() static bool TryResolvePhysical(const StringView& physicalPath, API_PARAM(Out) AssetMountResolution& result);
    API_FUNCTION() static bool IsWritable(const StringView& logicalPath);
};
