// Copyright (c) Wojciech Figat. All rights reserved.

#pragma once

#include "AssetPipelineDiagnostics.h"
#include "Engine/Content/AssetDatabase/AssetMount.h"
#include "Engine/Core/Collections/Array.h"
#include "Engine/Scripting/ScriptingType.h"

/// <summary>Built-in bootstrap-critical authored settings roles.</summary>
API_ENUM() enum class AssetSettingsRole : byte
{
    Project,
    Editor,
    Build,
    AssetPipeline,
};

/// <summary>Resolved unique owner of one mandatory settings role.</summary>
API_STRUCT() struct FLAXENGINE_API AssetSettingsRoleInfo
{
    DECLARE_SCRIPTING_TYPE_MINIMAL(AssetSettingsRoleInfo);

    API_FIELD() AssetSettingsRole Role = AssetSettingsRole::Project;
    API_FIELD() String TypeName;
    API_FIELD() String SourcePath;
    API_FIELD() Guid FileGuid = Guid::Empty;
};

/// <summary>Validated bootstrap state read only from the project marker and canonical settings source.</summary>
API_STRUCT() struct FLAXENGINE_API AssetPipelineBootstrapSnapshot
{
    DECLARE_SCRIPTING_TYPE_MINIMAL(AssetPipelineBootstrapSnapshot);

    API_FIELD() bool Valid = false;
    API_FIELD() bool RequiresMigration = false;
    API_FIELD() bool ReadOnly = false;
    API_FIELD() int32 AssetSystemVersion = 0;
    API_FIELD() Guid ProjectId = Guid::Empty;
    API_FIELD() String SourceRoot;
    API_FIELD() String IdentityModel;
    API_FIELD() int32 ArtifactLayoutVersion = 0;
    API_FIELD() int32 SourceDocumentVersion = 0;
    API_FIELD() String SettingsPath;
    API_FIELD() Guid SettingsGuid = Guid::Empty;
    API_FIELD() String SettingsFingerprint;
    API_FIELD() int32 DiskQuotaGigabytes = 100;
    API_FIELD() int32 MinimumFreeSpaceGigabytes = 5;
    API_FIELD() int32 GarbageCollectionGracePeriodHours = 24;
    API_FIELD() int32 RetainedLastGoodCount = 1;
    API_FIELD() int32 LogRetentionDays = 14;
    API_FIELD() int32 WorkerLimit = 0;
    API_FIELD() int32 MemoryLimitMegabytes = 4096;
    API_FIELD() Array<AssetSettingsRoleInfo> MandatoryRoles;
    API_FIELD() Array<AssetMount> Mounts;
    API_FIELD() Array<AssetPipelineDiagnostic> Diagnostics;
};

/// <summary>Built-in bootstrap reader for the one-way project marker and mandatory pipeline settings role.</summary>
API_CLASS(Static) class FLAXENGINE_API AssetPipelineBootstrap
{
    DECLARE_SCRIPTING_TYPE_NO_SPAWN(AssetPipelineBootstrap);

public:
    static constexpr int32 CurrentAssetSystemVersion = 3;
    static constexpr int32 CurrentArtifactLayoutVersion = 2;
    static constexpr int32 CurrentSourceDocumentVersion = 1;

    /// <summary>Validates bootstrap inputs without writing the project, Content, metadata, or Library.</summary>
    API_FUNCTION() static AssetPipelineBootstrapSnapshot Validate(const StringView& projectDescriptorPath, const StringView& contentRoot);
};
