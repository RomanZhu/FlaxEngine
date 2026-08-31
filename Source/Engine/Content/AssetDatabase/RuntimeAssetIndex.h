// Copyright (c) Wojciech Figat. All rights reserved.

#pragma once

#include "Identity/AssetObjectId.h"
#include "Engine/Content/Artifacts/ArtifactKey.h"
#include "Engine/Content/AssetPipeline/AssetPipelineDiagnostics.h"
#include "Engine/Core/Collections/Array.h"
#include "Engine/Core/Types/String.h"

/// <summary>Runtime load-location flags emitted by the cooker.</summary>
enum class RuntimeAssetIndexFlags : uint32
{
    None = 0,
    ExactArtifact = 1,
    LegacyConverted = 2,
};

DECLARE_ENUM_OPERATORS(RuntimeAssetIndexFlags);

/// <summary>One deterministic proactive runtime preload request.</summary>
struct FLAXENGINE_API RuntimeAssetPreload
{
    AssetObjectId ID;
    uint32 Priority = 0;
    uint64 EstimatedBytes = 0;
    bool Required = false;
};

/// <summary>One packaged runtime object addressable by file GUID and local file ID without a project Library.</summary>
struct FLAXENGINE_API RuntimeAssetIndexEntry
{
    AssetObjectId ID;
    Guid BackingAssetID;
    String TypeName;
    String CanonicalPath;
    String PackagedPath;
    Guid PackageID;
    uint32 ChunkID = 0;
    uint64 Offset = 0;
    uint64 Size = 0;
    uint32 AssetFormatVersion = 0;
    RuntimeAssetIndexFlags Flags = RuntimeAssetIndexFlags::None;
    ArtifactKey ExactArtifact;
    Array<AssetObjectId> Dependencies;
    uint64 PreloadBudgetBytes = 0;
    Array<RuntimeAssetPreload> Preload;
};

/// <summary>Declared provider, toolchain, or environment fingerprint consumed by an artifact.</summary>
struct FLAXENGINE_API RuntimeBuildDependencyEvidence
{
    String Kind;
    String Identity;
    ContentHash Hash;
};

/// <summary>Exact artifact evidence retained for reproducing one cooked runtime object.</summary>
struct FLAXENGINE_API RuntimeBuildArtifactEvidence
{
    AssetObjectId ID;
    ArtifactKey Artifact;
    ArtifactKey InputFingerprint;
    ContentHash SettingsHash;
    String ProcessorID;
    uint32 ProcessorVersion = 0;
    Array<RuntimeBuildDependencyEvidence> Environment;
};

/// <summary>Content digest for one emitted runtime package.</summary>
struct FLAXENGINE_API RuntimeBuildPackageEvidence
{
    Guid PackageID;
    String Path;
    ContentHash Content;
    uint64 Size = 0;
};

/// <summary>Deterministic inputs and outputs needed to compare two cooked builds.</summary>
struct FLAXENGINE_API RuntimeBuildReproducibility
{
    int32 EngineBuild = 0;
    ArtifactKey TargetFingerprint;
    Array<AssetObjectId> Roots;
    Array<RuntimeBuildArtifactEvidence> Artifacts;
    Array<RuntimeBuildPackageEvidence> Packages;
    bool Deterministic = false;
};

/// <summary>Deterministic, hash-verified asset-object/package index used as the cooked runtime registry.</summary>
class FLAXENGINE_API RuntimeAssetIndex
{
public:
    static constexpr int32 FormatVersion = 5;
    static constexpr uint64 DefaultPreloadBudgetBytes = 64ull * 1024ull * 1024ull;

    static bool ContainsLibraryPath(const StringView& path);
    static bool WriteCanonicalJson(const Array<RuntimeAssetIndexEntry>& entries, StringAnsi& output, AssetPipelineDiagnostic& diagnostic);
    static bool Parse(const StringAnsiView& input, Array<RuntimeAssetIndexEntry>& entries, AssetPipelineDiagnostic& diagnostic);
    static bool Load(const StringView& path, Array<RuntimeAssetIndexEntry>& entries, AssetPipelineDiagnostic& diagnostic);
    static bool SaveAtomic(const StringView& path, const Array<RuntimeAssetIndexEntry>& entries, AssetPipelineDiagnostic& diagnostic);

    /// <summary>Writes a canonical, content-hashed cook reproducibility manifest.</summary>
    static bool WriteReproducibilityJson(const RuntimeBuildReproducibility& manifest, StringAnsi& output, AssetPipelineDiagnostic& diagnostic);
    static bool SaveReproducibilityAtomic(const StringView& path, const RuntimeBuildReproducibility& manifest, AssetPipelineDiagnostic& diagnostic);

    /// <summary>Compares two hash-verified cook manifests and distinguishes changed inputs from nondeterministic outputs.</summary>
    static bool CompareReproducibilityFiles(const StringView& baselinePath, const StringView& candidatePath,
        bool& sameInputs, bool& identical, String& difference, AssetPipelineDiagnostic& diagnostic);
};
