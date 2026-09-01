// Copyright (c) Wojciech Figat. All rights reserved.

#pragma once

#include "ArtifactOutputValidator.h"
#include "Engine/Content/Build/ArtifactBuildContext.h"

/// <summary>Testable failure points around immutable publication state transitions.</summary>
enum class ArtifactPublicationFailurePoint : byte
{
    None,
    AfterOutputClose,
    AfterOutputValidation,
    AfterFirstImmutableMove,
    AfterAllImmutableMoves,
    AfterManifestTempWrite,
    AfterManifestFlush,
    BeforeAtomicReplace,
    AfterAtomicReplaceBeforeNotification,
    DuringCleanup,
};

struct FLAXENGINE_API ArtifactPublicationOutputPlan
{
    StringAnsi Kind;
    ArtifactKey Key;
};

using ArtifactPublicationStateQuery = Function<void(uint64&, ArtifactKey&)>;
using ArtifactPublicationNotification = Function<void(const ArtifactManifest&)>;

/// <summary>Immutable inputs required to publish a closed processor result.</summary>
struct FLAXENGINE_API ArtifactPublicationRequest
{
    ArtifactTarget Target;
    Guid RefreshId = Guid::Empty;
    uint32 Pass = 0;
    String ProcessorID;
    uint32 ProcessorImplementationVersion = 0;
    String BuildID;
    String BuiltAtUtc;
    Array<ArtifactPublicationOutputPlan> Outputs;
    ArtifactPublicationStateQuery QueryCurrentState;
    ArtifactPublicationNotification Notify;
    ArtifactPublicationFailurePoint FailurePoint = ArtifactPublicationFailurePoint::None;
};

struct FLAXENGINE_API ArtifactPublicationResult
{
    Guid RefreshId = Guid::Empty;
    uint32 Pass = 0;
    bool WasSuperseded = false;
    bool NotificationSent = false;
    ArtifactManifest Manifest;
};

/// <summary>Validates, publishes immutable outputs, and atomically selects them with a manifest.</summary>
class FLAXENGINE_API ArtifactPublisher
{
public:
    static bool Publish(const StringView& libraryRoot, const PreparedAsset& prepared, ArtifactBuildContext& context,
        const ArtifactPublicationRequest& request, const ArtifactOutputValidatorRegistry& validators,
        ArtifactPublicationResult& result, AssetPipelineDiagnostic& diagnostic);
};
