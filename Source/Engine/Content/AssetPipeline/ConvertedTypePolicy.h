// Copyright (c) Wojciech Figat. All rights reserved.

#pragma once

#include "AssetPipelineDiagnostics.h"
#include "AssetPipelineSettings.h"
#include "Engine/Scripting/ScriptingType.h"

/// <summary>Converted-type lockout and remaining legacy-authoritative exceptions.</summary>
API_CLASS(Static) class FLAXENGINE_API ConvertedTypePolicy
{
    DECLARE_SCRIPTING_TYPE_NO_SPAWN(ConvertedTypePolicy);

public:
    API_FUNCTION() static bool IsConvertedGraphType(const StringView& typeName);
    API_FUNCTION() static bool IsConvertedImportedType(const StringView& typeName);
    API_FUNCTION() static bool IsConvertedAssetType(const StringView& typeName);
    API_FUNCTION() static bool IsLegacyExceptionType(const StringView& typeName);
    static bool AllowsLegacyBinaryAuthoring(const AssetPipelineSettings& settings, const StringView& typeName, const StringView& path);
    API_FUNCTION() static bool AllowsLegacyBinaryAuthoring(const StringView& typeName, const StringView& path);
};
