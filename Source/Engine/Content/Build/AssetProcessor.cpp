// Copyright (c) Wojciech Figat. All rights reserved.

#include "AssetProcessor.h"

void AssetProcessorDescriptor::AppendVersionKey(ArtifactKeyBuilder& builder, const AssetProcessorOutputDescriptor& output) const
{
    builder.AddString(StringAnsiView("processor-id"), ID);
    builder.AddUInt32(StringAnsiView("processor-api"), EngineApiLevel);
    builder.AddUInt32(StringAnsiView("processor-settings-version"), SettingsSchemaVersion);
    builder.AddUInt32(StringAnsiView("processor-implementation-version"), ImplementationVersion);
    builder.AddUInt32(StringAnsiView("processor-interface-version"), InterfaceVersion);
    builder.AddString(StringAnsiView("output-kind"), output.Kind);
    builder.AddUInt32(StringAnsiView("output-format-version"), output.FormatVersion);
    if (ProviderKind == AssetProcessorProviderKind::ThirdParty)
        builder.AddHash(StringAnsiView("provider-semantic-identity"), ProviderSemanticIdentity);
}
