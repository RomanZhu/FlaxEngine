// Copyright (c) Wojciech Figat. All rights reserved.

#include "ConvertedTypePolicy.h"
#include "Engine/Platform/FileSystem.h"

bool ConvertedTypePolicy::IsConvertedGraphType(const StringView& typeName)
{
    return typeName == TEXT("FlaxEngine.Material") ||
        typeName == TEXT("FlaxEngine.MaterialFunction") ||
        typeName == TEXT("FlaxEngine.AnimationGraph") ||
        typeName == TEXT("FlaxEngine.AnimationGraphFunction") ||
        typeName == TEXT("FlaxEngine.VisualScript") ||
        typeName == TEXT("FlaxEngine.BehaviorTree") ||
        typeName == TEXT("FlaxEngine.ParticleEmitterFunction");
}

bool ConvertedTypePolicy::IsLegacyExceptionType(const StringView& typeName)
{
    return typeName == TEXT("FlaxEngine.Scene") ||
        typeName == TEXT("FlaxEngine.Prefab") ||
        typeName == TEXT("FlaxEngine.ParticleEmitter") ||
        typeName == TEXT("FlaxEngine.ParticleSystem") ||
        typeName == TEXT("FlaxEngine.SceneAnimation") ||
        typeName == TEXT("FlaxEngine.MaterialInstance") ||
        typeName == TEXT("FlaxEngine.Shader") ||
        typeName == TEXT("FlaxEngine.SkeletonMask") ||
        typeName == TEXT("FlaxEngine.RawDataAsset");
}

bool ConvertedTypePolicy::AllowsLegacyBinaryAuthoring(const AssetPipelineSettings& settings, const StringView& typeName, const StringView& path)
{
    if (!settings.LockConvertedTypeAuthoring || !IsConvertedGraphType(typeName))
        return true;
    const String extension = FileSystem::GetExtension(path).ToLower();
    return extension != TEXT("flax");
}

bool ConvertedTypePolicy::AllowsLegacyBinaryAuthoring(const StringView& typeName, const StringView& path)
{
    const AssetPipelineSettings* settings = AssetPipelineSettings::Get();
    if (settings == nullptr)
        return true;
    return AllowsLegacyBinaryAuthoring(*settings, typeName, path);
}
