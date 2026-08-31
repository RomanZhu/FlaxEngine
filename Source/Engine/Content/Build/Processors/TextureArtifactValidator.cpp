// Copyright (c) Wojciech Figat. All rights reserved.

#include "TextureArtifactValidator.h"
#include "TextureProcessor.h"
#include "Engine/Content/Assets/Texture.h"
#include "Engine/Content/Assets/CubeTexture.h"
#include "Engine/Content/Storage/ContentStorageManager.h"
#include "Engine/Render2D/SpriteAtlas.h"
#include "Engine/Graphics/PixelFormatExtensions.h"
#include "Engine/Platform/FileSystem.h"

#if COMPILE_WITH_TEXTURE_TOOL

namespace
{
    bool Invalid(AssetPipelineDiagnostic& diagnostic, const StringView& message)
    {
        diagnostic = AssetPipelineDiagnostic();
        diagnostic.Code = AssetPipelineDiagnosticCode::ArtifactInvalid;
        diagnostic.Stage = AssetPipelineDiagnosticStage::Publication;
        diagnostic.Message = message;
        return true;
    }
}

bool TextureArtifactValidator::Register(ArtifactOutputValidatorRegistry& registry, const Guid& expectedAssetID, const StringView& expectedType, AssetPipelineDiagnostic& diagnostic)
{
    ArtifactOutputValidator runtime = [expectedAssetID, expectedType = String(expectedType)](const StringView& path, const ArtifactManifestOutput& output, AssetPipelineDiagnostic& result)
    {
        return ValidateRuntime(path, output, expectedAssetID, expectedType, result);
    };
    return registry.Register(StringAnsiView("runtime"), expectedType, runtime, diagnostic);
}

bool TextureArtifactValidator::ValidateRuntime(const StringView& path, const ArtifactManifestOutput& output, const Guid& expectedAssetID, const StringView& expectedType, AssetPipelineDiagnostic& diagnostic)
{
    if (output.FormatVersion != TextureProcessor::RuntimeFormatVersion || output.Compatibility != "flax-texture-v4" ||
        output.Size == 0 || output.Size != FileSystem::GetFileSize(path))
        return Invalid(diagnostic, TEXT("Texture runtime artifact format metadata or size is invalid."));

    auto storage = ContentStorageManager::GetStorage(path);
    if (!storage)
        return Invalid(diagnostic, TEXT("Texture runtime artifact is not a readable Flax storage file."));
    Array<FlaxStorage::Entry> entries;
    storage->GetEntries(entries);
    if (entries.Count() != 1 || entries[0].ID != expectedAssetID || entries[0].TypeName != expectedType)
        return Invalid(diagnostic, TEXT("Texture runtime artifact identity or type does not match the requested asset."));

    AssetInitData data;
    const bool validType = expectedType == Texture::TypeName || expectedType == CubeTexture::TypeName || expectedType == SpriteAtlas::TypeName;
    if (!validType || storage->LoadAssetHeader(expectedAssetID, data) || data.SerializedVersion != Texture::SerializedVersion ||
        data.CustomData.Length() != sizeof(TextureHeader))
        return Invalid(diagnostic, TEXT("Texture runtime artifact header version or metadata is invalid."));
#if USE_EDITOR
    if (data.Metadata.IsValid())
        return Invalid(diagnostic, TEXT("Texture runtime artifact contains authoritative editor metadata."));
#endif
    TextureHeader header;
    Platform::MemoryCopy(&header, data.CustomData.Get(), sizeof(TextureHeader));
    if (header.Width < 1 || header.Height < 1 || header.Depth < 0 ||
        header.Width > TextureProcessor::MaximumDimension || header.Height > TextureProcessor::MaximumDimension ||
        header.MipLevels < 1 || header.MipLevels > GPU_MAX_TEXTURE_MIP_LEVELS ||
        !PixelFormatExtensions::IsValid(header.Format) || header.Type > TextureFormatType::HdrRGB)
        return Invalid(diagnostic, TEXT("Texture runtime artifact dimensions, format, or mip count are invalid."));
    for (int32 mip = 0; mip < header.MipLevels; mip++)
    {
        const FlaxChunk* chunk = data.Header.Chunks[mip];
        if (!chunk || !chunk->ExistsInFile() || static_cast<uint64>(chunk->LocationInFile.Address) + chunk->LocationInFile.Size > output.Size)
            return Invalid(diagnostic, TEXT("Texture runtime artifact has a missing or out-of-bounds mip chunk."));
    }
    diagnostic = AssetPipelineDiagnostic();
    return false;
}

#endif
