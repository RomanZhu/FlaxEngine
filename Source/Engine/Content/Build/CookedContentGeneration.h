// Copyright (c) Wojciech Figat. All rights reserved.

#pragma once

#include "Engine/Content/Artifacts/ArtifactKey.h"
#include "Engine/Content/AssetPipeline/AssetPipelineDiagnostics.h"
#include "Engine/Core/Delegate.h"
#include "Engine/Core/Types/DateTime.h"

/// <summary>Fault-injection points for atomic cooked-content generation publication.</summary>
enum class CookedContentPublicationFailurePoint : byte
{
    None,
    BeforeGenerationMove,
    AfterGenerationMove,
    BeforePointerReplace,
};

/// <summary>State for one exclusively owned cooked-output deployment transaction.</summary>
struct CookedContentDeploymentState
{
    ContentHash NewGeneration;
    ContentHash PreviousGeneration;
    String RollbackRoot;
    bool HadPreviousGeneration = false;
    bool ActivationChanged = false;
    bool PreviousGenerationMoved = false;
};

/// <summary>
/// Publishes cooked head, catalog, packages, and streaming files as one immutable generation selected by one atomic pointer.
/// Deployment requires exclusive ownership of the output path. Before recursive platform packaging, inactive generations and
/// abandoned staging are removed from the package view while the previous active generation is held outside that view for rollback.
/// </summary>
class FLAXENGINE_API CookedContentGeneration
{
public:
    static String GetGenerationsPath(const StringView& contentRoot);
    static String GetCurrentGenerationPath(const StringView& contentRoot);

    /// <summary>Creates an isolated data-output root beneath the generation store. Returns true on failure.</summary>
    static bool CreateStaging(const StringView& contentRoot, const Guid& jobID, String& stagingDataRoot,
        AssetPipelineDiagnostic& diagnostic);

    /// <summary>Returns whether a streaming file must replace its published copy.</summary>
    static bool ShouldCopyStreamingFile(bool destinationExists, const DateTime& sourceModified, const DateTime& destinationModified);

    /// <summary>Publishes one complete staged data root and atomically selects it. Returns true on failure.</summary>
    static bool Publish(const StringView& contentRoot, const StringView& stagingDataRoot, ContentHash& generation,
        AssetPipelineDiagnostic& diagnostic, const Function<bool()>& isCancellationRequested = Function<bool()>(),
        CookedContentPublicationFailurePoint failurePoint = CookedContentPublicationFailurePoint::None);

    /// <summary>
    /// Publishes and activates a staged generation, then reduces the recursively packaged generation store to the active generation.
    /// The caller must exclusively own the output path and provide a new rollback directory outside the packaged output.
    /// Returns true on failure.
    /// </summary>
    static bool BeginDeployment(const StringView& contentRoot, const StringView& stagingDataRoot, const StringView& rollbackRoot,
        CookedContentDeploymentState& state, AssetPipelineDiagnostic& diagnostic,
        const Function<bool()>& isCancellationRequested = Function<bool()>());

    /// <summary>Commits a successfully packaged deployment by deleting its outside rollback data. Returns true on failure.</summary>
    static bool CommitDeployment(CookedContentDeploymentState& state, AssetPipelineDiagnostic& diagnostic);

    /// <summary>Restores the activation that preceded a reported packaging failure. Returns true on failure.</summary>
    static bool RollbackDeployment(const StringView& contentRoot, CookedContentDeploymentState& state,
        AssetPipelineDiagnostic& diagnostic);

    /// <summary>
    /// Resolves the immutable content directory selected by the atomic pointer and validates its completion marker,
    /// catalog, product header, and package presence. Startup does not rehash all generation bytes.
    /// Returns true on failure.
    /// </summary>
    static bool Resolve(const StringView& contentRoot, String& activeContentPath, ContentHash& generation,
        AssetPipelineDiagnostic& diagnostic);
};
