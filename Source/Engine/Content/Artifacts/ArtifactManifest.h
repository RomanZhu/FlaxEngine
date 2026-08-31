// Copyright (c) Wojciech Figat. All rights reserved.

#pragma once

#include "ArtifactTarget.h"
#include "Engine/Content/AssetDatabase/AssetDependency.h"

/// <summary>Generated manifest dependency provenance.</summary>
struct FLAXENGINE_API ArtifactManifestDependency
{
    AssetDependencyKind Kind = AssetDependencyKind::SourceFile;
    AssetDependencyState State = AssetDependencyState::Present;
    String Identity;
    AssetObjectId ObjectID;
    Guid AssetID = Guid::Empty;
    ContentHash Hash;
    ContentHash MetadataHash;
    ArtifactKey ExactArtifact;
    ContentHash InterfaceHash;
    uint32 InterfaceVersion = 0;
    String Origin;
};

/// <summary>One immutable output selected by a successful manifest.</summary>
struct FLAXENGINE_API ArtifactManifestOutput
{
    StringAnsi Kind;
    uint32 FormatVersion = 1;
    ArtifactKey Key;
    String RelativePath;
    ContentHash Content;
    uint64 Size = 0;
    StringAnsi Compatibility;
};

/// <summary>One persistent object address materialized by this artifact.</summary>
struct FLAXENGINE_API ArtifactManifestObject
{
    AssetObjectId ObjectID;
    Guid BackingAssetID = Guid::Empty;
    String TypeName;
    String Name;
    String StableKey;
    bool IsMainObject = false;
};

/// <summary>Canonical generated record selecting one coherent successful output set.</summary>
struct FLAXENGINE_API ArtifactManifest
{
    static constexpr int32 CurrentVersion = 3;

    int32 ManifestVersion = CurrentVersion;
    Guid AssetID = Guid::Empty;
    uint64 DatabaseRevision = 0;
    String ProcessorID;
    uint32 ProcessorImplementationVersion = 0;
    ArtifactTarget Target;
    ArtifactKey InputFingerprint;
    ContentHash SourceHash;
    ContentHash SettingsHash;
    Array<ArtifactManifestDependency> Dependencies;
    Array<ArtifactManifestObject> Objects;
    Array<AssetImportReasonNode> ImportReasons;
    Array<ArtifactManifestOutput> Outputs;
    String BuildID;
    String BuiltAtUtc;
    ArtifactKey PreviousSuccessfulInputFingerprint;
    Array<ArtifactKeyComponent> KeyComponents;

    /// <summary>Parses and validates a generated manifest. Returns true on failure.</summary>
    static bool Parse(const StringAnsiView& json, const StringView& path, ArtifactManifest& result, AssetPipelineDiagnostic& diagnostic);

    /// <summary>Writes deterministic UTF-8 canonical JSON. Returns true on failure.</summary>
    bool ToJson(StringAnsi& json, AssetPipelineDiagnostic& diagnostic) const;

    /// <summary>Validates semantic invariants independent of the JSON codec.</summary>
    bool Validate(const StringView& path, AssetPipelineDiagnostic& diagnostic) const;
};
