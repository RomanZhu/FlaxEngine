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

bool AssetProcessorSettingsSchema::PreviewUpgrade(const AssetMeta& current, bool mayStageTrackedChanges, AssetMeta& staged, AssetPipelineDiagnostic& diagnostic) const
{
    staged = current;
    diagnostic = AssetPipelineDiagnostic();
    if (current.Processor.ID != ProcessorID || CurrentVersion < 1 || current.Processor.SettingsVersion > CurrentVersion)
        return Fail(diagnostic, AssetPipelineDiagnosticCode::InvalidMeta, TEXT("Processor settings schema does not match metadata."));
    if (current.Processor.SettingsVersion == CurrentVersion)
        return false;
    if (!mayStageTrackedChanges)
        return Fail(diagnostic, AssetPipelineDiagnosticCode::MetaUpgradeRequired, TEXT("Tracked processor settings require an explicit metadata upgrade."));
    if (!Upgrade)
        return Fail(diagnostic, AssetPipelineDiagnosticCode::MetaUpgradeRequired, TEXT("Processor settings require an upgrade but no migration callback is registered."));

    CanonicalJsonError currentError;
    StringAnsi currentSettings;
    if (CanonicalJsonWriter::Canonicalize(staged.Processor.SettingsJson, currentSettings, currentError))
        return Fail(diagnostic, AssetPipelineDiagnosticCode::InvalidMeta, TEXT("Processor settings contain invalid JSON."));
    staged.Processor.SettingsJson = MoveTemp(currentSettings);

    while (staged.Processor.SettingsVersion < CurrentVersion)
    {
        StringAnsi upgraded;
        if (Upgrade(staged.Processor.SettingsVersion, staged.Processor.SettingsJson, upgraded, diagnostic))
            return true;
        CanonicalJsonError error;
        StringAnsi canonical;
        if (CanonicalJsonWriter::Canonicalize(upgraded, canonical, error))
            return Fail(diagnostic, AssetPipelineDiagnosticCode::InvalidMeta, TEXT("Processor settings migration produced invalid JSON."));
        staged.Processor.SettingsJson = MoveTemp(canonical);
        staged.Processor.SettingsVersion++;
    }
    return false;
}
