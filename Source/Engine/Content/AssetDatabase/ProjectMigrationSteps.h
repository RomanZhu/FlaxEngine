// Copyright (c) Wojciech Figat. All rights reserved.

#pragma once

#include "Engine/Content/AssetPipeline/AssetPipelineDiagnostics.h"

/// <summary>Idempotent filesystem conversion steps used by the project migration coordinator.</summary>
class FLAXENGINE_API ProjectMigrationSteps
{
public:
    static bool EstablishCanonicalSettings(const StringView& projectDescriptorPath, const StringView& contentRoot,
        Array<AssetPipelineDiagnostic>& diagnostics);
    static bool ClassifyAndConvertLegacyAssets(const StringView& contentRoot, const StringView& quarantineRoot,
        Array<AssetPipelineDiagnostic>& diagnostics);
    static bool CompleteAndUpgradeMetadata(const StringView& projectRoot, const StringView& contentRoot,
        Array<AssetPipelineDiagnostic>& diagnostics);
    static bool RewriteAndVerifySerializedReferences(const StringView& contentRoot, const StringView& legacyPreimageRoot,
        StringAnsi& report, Array<AssetPipelineDiagnostic>& diagnostics);
    static bool CanonicalizeAndVerifyAuthoredSources(const StringView& contentRoot, bool writeChanges,
        StringAnsi& report, Array<AssetPipelineDiagnostic>& diagnostics);
    static bool VerifyImportedDatabase(const StringView& contentRoot, StringAnsi& report,
        Array<AssetPipelineDiagnostic>& diagnostics);
    static bool WriteCandidateProjectMarker(const StringView& projectDescriptorPath, const StringView& outputPath,
        String& fingerprint, AssetPipelineDiagnostic& diagnostic);
};
