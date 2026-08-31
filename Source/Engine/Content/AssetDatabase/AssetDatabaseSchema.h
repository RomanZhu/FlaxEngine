// Copyright (c) Wojciech Figat. All rights reserved.

#pragma once

#include "AssetDependency.h"
#include "Engine/Content/Artifacts/ArtifactKey.h"
#include "Engine/Content/AssetPipeline/AssetPipelineDiagnostics.h"

/// <summary>Current on-disk source asset database schema.</summary>
class FLAXENGINE_API AssetDatabaseSchema
{
public:
    static constexpr uint32 Version = 2;
};

/// <summary>Persisted singleton database state.</summary>
struct FLAXENGINE_API SourceAssetDatabaseStateRow
{
    uint32 SchemaVersion = AssetDatabaseSchema::Version;
    Guid ProjectId = Guid::Empty;
    uint64 CurrentRevision = 0;
    uint64 LastCompleteScanId = 0;
    uint64 ImporterRegistryGeneration = 0;
    bool CleanShutdown = true;
};

/// <summary>One canonical source and metadata pair.</summary>
struct FLAXENGINE_API SourceAssetRow
{
    Guid AssetGuid = Guid::Empty;
    String Path;
    String CanonicalPath;
    String MetaPath;
    String CanonicalMetaPath;
    bool IsFolder = false;
    ContentHash SourceHash;
    ContentHash MetaHash;
    uint64 MetaSemanticHash = 0;
    uint64 SourceSize = 0;
    int64 SourceMtimeHint = 0;
    String ImporterId;
    String PortabilityKey;
    AssetSourceKind SourceKind = AssetSourceKind::LegacyBinary;
    uint32 ImporterSettingsVersion = 0;
    ContentHash ImporterSettingsHash;
    ContentHash ImporterCodeHash;
    AssetRecordStatus Status = AssetRecordStatus::Ready;
    uint64 FirstSeenRevision = 0;
    uint64 LastSeenRevision = 0;
    uint64 LastModifiedRevision = 0;
};

/// <summary>One stable object inside a source asset.</summary>
struct FLAXENGINE_API SourceAssetObjectRow
{
    Guid AssetGuid = Guid::Empty;
    Guid ObjectGuid = Guid::Empty;
    int64 LocalFileId = 0;
    String StableIdentifier;
    String SubAssetKey;
    String TypeName;
    String DisplayName;
    bool IsMain = false;
    bool IsRemoved = false;
    AssetRecordStatus Status = AssetRecordStatus::Ready;
    String ObjectMetadata;
    uint64 FirstSeenRevision = 0;
    uint64 LastSeenRevision = 0;
    uint64 LastModifiedRevision = 0;
};

/// <summary>Normalized dependency edge. Reverse queries use this same authoritative table.</summary>
struct FLAXENGINE_API SourceAssetDependencyRow
{
    Guid OwnerAssetGuid = Guid::Empty;
    int64 OwnerLocalFileId = 0;
    String TargetId;
    AssetDependencyKind Kind = AssetDependencyKind::SourceFile;
    Guid TargetAssetGuid = Guid::Empty;
    int64 TargetLocalFileId = 0;
    String SourcePath;
    ArtifactKey ExactArtifact;
    String CustomDependency;
    ContentHash Content;
    uint32 Flags = 0;
    String OriginPath;
    int32 OriginLine = -1;
    int32 OriginColumn = -1;
};

/// <summary>Last published immutable artifact for an asset and target.</summary>
struct FLAXENGINE_API SourceAssetPublicationRow
{
    Guid AssetGuid = Guid::Empty;
    int64 LocalFileId = 0;
    String TargetId;
    ArtifactKey Artifact;
    ContentHash ManifestHash;
    ArtifactKey InputFingerprint;
    uint64 SourceRevision = 0;
    uint64 ImporterRegistryGeneration = 0;
    int64 PublishedUtcTicks = 0;
    bool IsLastKnownGood = false;
};

/// <summary>Persisted structured diagnostic with lifecycle information.</summary>
struct FLAXENGINE_API SourceAssetDiagnosticRow
{
    uint64 DiagnosticId = 0;
    Guid AssetGuid = Guid::Empty;
    int64 LocalFileId = 0;
    AssetPipelineDiagnostic Diagnostic;
    Guid AttemptId = Guid::Empty;
    uint64 CreatedRevision = 0;
    bool IsActive = true;
};

/// <summary>Complete normalized state stored by one atomic snapshot.</summary>
struct FLAXENGINE_API SourceAssetDatabaseState
{
    SourceAssetDatabaseStateRow Database;
    Array<SourceAssetRow> Sources;
    Array<SourceAssetObjectRow> Objects;
    Array<SourceAssetDependencyRow> Dependencies;
    Array<SourceAssetPublicationRow> Publications;
    Array<SourceAssetDiagnosticRow> Diagnostics;

    /// <summary>Validates uniqueness and referential/revision invariants. Returns true on failure.</summary>
    bool Validate(AssetPipelineDiagnostic& diagnostic) const;

    /// <summary>Serializes the schema payload deterministically.</summary>
    void Serialize(Array<byte>& output) const;

    /// <summary>Reads a complete schema payload. Returns true on failure.</summary>
    static bool Deserialize(const byte* data, uint32 length, SourceAssetDatabaseState& output, AssetPipelineDiagnostic& diagnostic);
};
