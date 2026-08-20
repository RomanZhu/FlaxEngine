// Copyright (c) Wojciech Figat. All rights reserved.

#include "ArtifactOutputValidator.h"

String ArtifactOutputValidatorRegistry::MakeKey(const StringAnsiView& outputKind, const StringView& assetType)
{
    return String(outputKind).ToLower() + TEXT("|") + String(assetType);
}

bool ArtifactOutputValidatorRegistry::Register(const StringAnsiView& outputKind, const StringView& assetType, const ArtifactOutputValidator& validator, AssetPipelineDiagnostic& diagnostic)
{
    diagnostic = AssetPipelineDiagnostic();
    const String key = MakeKey(outputKind, assetType);
    if (outputKind.IsEmpty() || !validator.IsBinded() || _validators.ContainsKey(key))
    {
        diagnostic.Code = AssetPipelineDiagnosticCode::InvalidSettingsCombination;
        diagnostic.Stage = AssetPipelineDiagnosticStage::Configuration;
        diagnostic.OutputKind = String(outputKind);
        diagnostic.Message = TEXT("Artifact output validator registration is invalid or duplicated.");
        return true;
    }
    _validators.Add(key, validator);
    return false;
}

bool ArtifactOutputValidatorRegistry::Validate(const StringAnsiView& outputKind, const StringView& assetType, const StringView& path,
    const ArtifactManifestOutput& output, AssetPipelineDiagnostic& diagnostic) const
{
    const ArtifactOutputValidator* validator = _validators.TryGet(MakeKey(outputKind, assetType));
    if (!validator)
        validator = _validators.TryGet(MakeKey(outputKind, StringView::Empty));
    if (!validator)
    {
        diagnostic = AssetPipelineDiagnostic();
        diagnostic.Code = AssetPipelineDiagnosticCode::ArtifactInvalid;
        diagnostic.Stage = AssetPipelineDiagnosticStage::Publication;
        diagnostic.SourcePath = path;
        diagnostic.OutputKind = String(outputKind);
        diagnostic.Message = TEXT("No validator is registered for the artifact output kind and asset type.");
        return true;
    }
    if ((*validator)(path, output, diagnostic))
    {
        diagnostic.Stage = AssetPipelineDiagnosticStage::Publication;
        diagnostic.SourcePath = path;
        diagnostic.OutputKind = String(outputKind);
        if (diagnostic.Code == AssetPipelineDiagnosticCode::None)
            diagnostic.Code = AssetPipelineDiagnosticCode::ArtifactInvalid;
        if (diagnostic.Message.IsEmpty())
            diagnostic.Message = TEXT("Artifact output validator rejected the staged file.");
        return true;
    }
    return false;
}

void ArtifactOutputValidatorRegistry::Clear()
{
    _validators.Clear();
}
