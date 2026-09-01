// Copyright (c) Wojciech Figat. All rights reserved.

#include "AssetProcessorSettings.h"
#include "Engine/Content/Documents/CanonicalJsonWriter.h"

namespace
{
    bool Fail(AssetPipelineDiagnostic& diagnostic, AssetPipelineDiagnosticCode code, const StringView& message)
    {
        diagnostic = AssetPipelineDiagnostic();
        diagnostic.Code = code;
        diagnostic.Stage = AssetPipelineDiagnosticStage::Prepare;
        diagnostic.Message = message;
        return true;
    }
}

bool AssetProcessorSettingsSchema::InitializeNewMeta(AssetMeta& meta, AssetPipelineDiagnostic& diagnostic) const
{
    CanonicalJsonError error;
    StringAnsi canonicalDefaults;
    if (ProcessorID.IsEmpty() || CurrentVersion < 1 || CanonicalJsonWriter::Canonicalize(NormalizedDefaults, canonicalDefaults, error))
        return Fail(diagnostic, AssetPipelineDiagnosticCode::InvalidMeta, TEXT("Processor settings schema or defaults are invalid."));
    meta.Processor.ID = ProcessorID;
    meta.Processor.SettingsVersion = CurrentVersion;
    meta.Processor.SettingsJson = MoveTemp(canonicalDefaults);
    diagnostic = AssetPipelineDiagnostic();
    return false;
}
