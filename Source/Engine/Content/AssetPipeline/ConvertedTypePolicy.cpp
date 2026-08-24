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
        typeName == TEXT("FlaxEngine.ParticleEmitterFunction") ||
        typeName == TEXT("FlaxEngine.ParticleEmitter") ||
        typeName == TEXT("FlaxEngine.ParticleSystem") ||
        typeName == TEXT("FlaxEngine.CollisionData") ||
        typeName == TEXT("FlaxEngine.MaterialInstance") ||
        typeName == TEXT("FlaxEngine.SkeletonMask") ||
        typeName == TEXT("FlaxEngine.SceneAnimation");
}

bool ConvertedTypePolicy::IsLegacyExceptionType(const StringView& typeName)
{
    return typeName == TEXT("FlaxEngine.RawDataAsset");
}

bool ConvertedTypePolicy::IsConvertedImportedType(const StringView& typeName)
{
    return typeName == TEXT("FlaxEngine.Texture") ||
        typeName == TEXT("FlaxEngine.CubeTexture") ||
        typeName == TEXT("FlaxEngine.SpriteAtlas") ||
        typeName == TEXT("FlaxEngine.Model") ||
        typeName == TEXT("FlaxEngine.SkinnedModel") ||
        typeName == TEXT("FlaxEngine.Animation") ||
        typeName == TEXT("FlaxEngine.AudioClip") ||
        typeName == TEXT("FlaxEngine.FontAsset") ||
        typeName == TEXT("FlaxEngine.Shader");
}

bool ConvertedTypePolicy::IsConvertedAssetType(const StringView& typeName)
{
    return IsConvertedGraphType(typeName) || IsConvertedImportedType(typeName);
}

bool ConvertedTypePolicy::AllowsLegacyBinaryAuthoring(const AssetPipelineSettings&, const StringView& typeName, const StringView& path)
{
    if (!IsConvertedAssetType(typeName))
        return true;
    const String extension = FileSystem::GetExtension(path).ToLower();
    return extension != TEXT("flax");
}

bool ConvertedTypePolicy::AllowsLegacyBinaryAuthoring(const StringView& typeName, const StringView& path)
{
    AssetPipelineSettings settings;
    return AllowsLegacyBinaryAuthoring(settings, typeName, path);
}
