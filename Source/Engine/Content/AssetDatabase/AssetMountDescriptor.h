// Copyright (c) Wojciech Figat. All rights reserved.

#pragma once

#include "AssetMount.h"
#include "Engine/Core/Config/Settings.h"
#include "Engine/Core/ISerializable.h"

/// <summary>Portable authored descriptor for one read-only content mount.</summary>
API_STRUCT() struct FLAXENGINE_API AssetMountSourceDescriptor : ISerializable
{
    DECLARE_SCRIPTING_TYPE_MINIMAL(AssetMountSourceDescriptor);
    API_AUTO_SERIALIZATION();

    API_FIELD() Guid MountId = Guid::Empty;
    API_FIELD() String LogicalPrefix;
    API_FIELD() String Root;
    API_FIELD() AssetMountKind Kind = AssetMountKind::ExternalReadOnlyContent;
    API_FIELD() bool AllowLinkedRoot = false;
};

/// <summary>Authored explicit engine, plugin, and external content-mount declarations.</summary>
API_CLASS(sealed, Namespace="FlaxEditor.Content.Settings") class FLAXENGINE_API AssetMountSettings : public SettingsBase
{
    DECLARE_SCRIPTING_TYPE_MINIMAL(AssetMountSettings);
    API_AUTO_SERIALIZATION();

public:
    API_FIELD() Array<AssetMountSourceDescriptor> Mounts;
};

/// <summary>Built-in parser and validator for the authored content-mount settings role.</summary>
class FLAXENGINE_API AssetMountDescriptorCodec
{
public:
    static const Char* TypeName;
    static bool Parse(const StringAnsiView& source, const StringView& sourcePath, const StringView& projectRoot,
        const StringView& engineRoot, Array<AssetMount>& mounts, AssetPipelineDiagnostic& diagnostic);
    static bool Write(const Guid& sourceGuid, const Array<AssetMountSourceDescriptor>& descriptors, StringAnsi& source,
        AssetPipelineDiagnostic& diagnostic);
    static bool ResolveRoot(const StringView& expression, const StringView& projectRoot, const StringView& engineRoot,
        String& physicalRoot, AssetPipelineDiagnostic& diagnostic);
};
