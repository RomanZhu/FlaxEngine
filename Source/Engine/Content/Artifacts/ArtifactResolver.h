// Copyright (c) Wojciech Figat. All rights reserved.

#pragma once

#include "ArtifactManifest.h"
#include "ResolvedArtifact.h"
#include "Engine/Content/AssetDatabase/AssetDatabase.h"
#include "Engine/Content/Build/AssetBuildService.h"

enum class ArtifactResolvePolicy : byte
{
    Interactive,
    Exact,
    NoBuild,
    PublishedOnly,
};

struct FLAXENGINE_API ArtifactRequest
{
    AssetObjectId Object;
    ArtifactTarget Target;
    StringAnsi OutputKind;
    StringAnsi RequiredCompatibility;
    ArtifactResolvePolicy Policy = ArtifactResolvePolicy::Interactive;
    /// <summary>Optional caller cancellation queried while waiting for an exact build.</summary>
    Function<bool()> IsCancellationRequested;
};

struct FLAXENGINE_API ArtifactResolutionPlan
{
    ArtifactKey CurrentInputFingerprint;
    AssetBuildJobRequest BuildRequest;
};

using ArtifactResolutionPlanProvider = Function<bool(const AssetRecord&, const ArtifactRequest&, ArtifactResolutionPlan&, AssetPipelineDiagnostic&)>;

/// <summary>Inspects current manifests and applies exact, interactive-last-good, or no-build policy.</summary>
class FLAXENGINE_API ArtifactResolver
{
private:
    AssetDatabase* _database = nullptr;
    AssetBuildService* _buildService = nullptr;
    ArtifactResolutionPlanProvider _planProvider;
    String _libraryRoot;
    ArtifactTarget _defaultTarget;

public:
    static ArtifactResolver& Get();

    void Configure(AssetDatabase& database, AssetBuildService& buildService, const StringView& libraryRoot,
        const ArtifactTarget& defaultTarget, const ArtifactResolutionPlanProvider& planProvider);
    void Reset();

    bool IsConfigured() const;
    const ArtifactTarget& GetDefaultTarget() const
    {
        return _defaultTarget;
    }

    /// <summary>Resolves one physical output. Returns true on failure.</summary>
    bool Resolve(const ArtifactRequest& request, ResolvedArtifact& result, AssetPipelineDiagnostic& diagnostic);

    /// <summary>Resolves canonical identity and physical storage for Content/factory loading.</summary>
    bool ResolveLoadLocation(const ArtifactRequest& request, AssetLoadLocation& result, AssetPipelineDiagnostic& diagnostic);
};
