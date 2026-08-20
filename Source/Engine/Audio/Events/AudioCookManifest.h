// Copyright (c) Wojciech Figat. All rights reserved.

#pragma once

#include "Engine/Core/Types/Guid.h"
#include "Engine/Core/Types/String.h"
#include "Engine/Core/Collections/Array.h"

/// <summary>
/// One bank entry in the deterministic cooked-audio manifest.
/// </summary>
API_STRUCT() struct FLAXENGINE_API AudioCookManifestBank
{
    DECLARE_SCRIPTING_TYPE_MINIMAL(AudioCookManifestBank);

    API_FIELD() Guid ID = Guid::Empty;
    API_FIELD() String File;
    API_FIELD() String Hash;
    API_FIELD() uint64 Size = 0;
};

/// <summary>
/// Runtime description of the exact audio bank set selected by a cook.
/// </summary>
API_STRUCT() struct FLAXENGINE_API AudioCookManifest
{
    DECLARE_SCRIPTING_TYPE_MINIMAL(AudioCookManifest);

    API_FIELD() int32 Schema = 2;
    API_FIELD() String MetadataRevision;
    API_FIELD() String Platform;
    API_FIELD() String Locale = TEXT("default");
    API_FIELD() Array<AudioCookManifestBank> Banks;
};
