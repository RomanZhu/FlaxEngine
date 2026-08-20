// Copyright (c) Wojciech Figat. All rights reserved.

#pragma once

#include "ArtifactManifest.h"
#include "Engine/Core/Collections/Dictionary.h"
#include "Engine/Core/Delegate.h"

using ArtifactOutputValidator = Function<bool(const StringView&, const ArtifactManifestOutput&, AssetPipelineDiagnostic&)>;

/// <summary>Engine-owned validators selected by output kind and optional asset type.</summary>
class FLAXENGINE_API ArtifactOutputValidatorRegistry
{
public:
    bool Register(const StringAnsiView& outputKind, const StringView& assetType, const ArtifactOutputValidator& validator, AssetPipelineDiagnostic& diagnostic);
    bool Validate(const StringAnsiView& outputKind, const StringView& assetType, const StringView& path,
        const ArtifactManifestOutput& output, AssetPipelineDiagnostic& diagnostic) const;
    void Clear();

private:
    Dictionary<String, ArtifactOutputValidator> _validators;
    static String MakeKey(const StringAnsiView& outputKind, const StringView& assetType);
};
