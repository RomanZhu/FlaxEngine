// Copyright (c) Wojciech Figat. All rights reserved.

#include "ModelArtifactValidator.h"

#if COMPILE_WITH_MODEL_TOOL && USE_EDITOR

#include "ModelProcessor.h"
#include "Engine/Content/Assets/Animation.h"
#include "Engine/Content/Assets/Material.h"
#include "Engine/Content/Assets/Model.h"
#include "Engine/Content/Assets/SkinnedModel.h"
#include "Engine/Content/Assets/Texture.h"
#include "Engine/Content/Storage/ContentStorageManager.h"
#include "Engine/Platform/FileSystem.h"

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

    bool ValidateRuntime(const StringView& path, const ArtifactManifestOutput& output, const Guid& expectedID,
        const StringView& expectedType, AssetPipelineDiagnostic& diagnostic)
    {
        if (output.FormatVersion != ModelProcessor::RuntimeFormatVersion || output.Compatibility != "flax-model-runtime-v1" ||
            output.Size == 0 || output.Size != FileSystem::GetFileSize(path))
            return Invalid(diagnostic, TEXT("Model runtime artifact format metadata or size is invalid."));
        auto storage = ContentStorageManager::GetStorage(path);
        if (!storage)
            return Invalid(diagnostic, TEXT("Model runtime artifact is not a readable Flax storage file."));
        Array<FlaxStorage::Entry> entries;
        storage->GetEntries(entries);
        if (entries.Count() != 1 || entries[0].ID != expectedID || entries[0].TypeName != expectedType)
            return Invalid(diagnostic, TEXT("Model runtime artifact identity or type does not match the requested logical asset."));
        AssetInitData data;
        if (storage->LoadAssetHeader(expectedID, data))
            return Invalid(diagnostic, TEXT("Model runtime artifact header cannot be loaded."));
        const int32 expectedVersion = expectedType == Model::TypeName ? Model::SerializedVersion :
            expectedType == SkinnedModel::TypeName ? SkinnedModel::SerializedVersion :
            expectedType == Animation::TypeName ? Animation::SerializedVersion :
            expectedType == Material::TypeName ? Material::SerializedVersion :
            expectedType == Texture::TypeName ? Texture::SerializedVersion : -1;
        if (expectedVersion < 0 || data.SerializedVersion != expectedVersion)
            return Invalid(diagnostic, TEXT("Model runtime artifact serialized version is incompatible."));
#if USE_EDITOR
        if (data.Metadata.IsValid())
            return Invalid(diagnostic, TEXT("Model runtime artifact contains authoritative legacy import metadata."));
#endif
        diagnostic = AssetPipelineDiagnostic();
        return false;
    }

    bool ValidateDerived(const StringView& path, const ArtifactManifestOutput& output, const StringAnsiView& kind,
        uint32 formatVersion, const StringAnsiView& compatibility, AssetPipelineDiagnostic& diagnostic)
    {
        if (output.Kind != kind || output.FormatVersion != formatVersion || output.Compatibility != compatibility ||
            output.Size == 0 || output.Size > 4ull * 1024ull * 1024ull * 1024ull || output.Size != FileSystem::GetFileSize(path))
            return Invalid(diagnostic, TEXT("Model derived artifact metadata or bounded size is invalid."));
        diagnostic = AssetPipelineDiagnostic();
        return false;
    }
}

bool ModelArtifactValidator::Register(ArtifactOutputValidatorRegistry& registry, const PreparedAsset& prepared, AssetPipelineDiagnostic& diagnostic)
{
    for (const DeclaredArtifactOutput& output : prepared.Outputs)
    {
        ArtifactOutputValidator validator;
        if (output.Kind == StringAnsiView("runtime"))
        {
            validator = [expectedID = prepared.AssetID, expectedType = prepared.OutputType](const StringView& path,
                const ArtifactManifestOutput& manifestOutput, AssetPipelineDiagnostic& result)
            {
                return ValidateRuntime(path, manifestOutput, expectedID, expectedType, result);
            };
        }
        else
        {
            const StringAnsi kind = output.Kind;
            const uint32 version = output.FormatVersion;
            const StringAnsi compatibility = output.CompatibilityTag;
            validator = [kind, version, compatibility](const StringView& path, const ArtifactManifestOutput& manifestOutput,
                AssetPipelineDiagnostic& result)
            {
                return ValidateDerived(path, manifestOutput, kind, version, compatibility, result);
            };
        }
        if (registry.Register(output.Kind, prepared.OutputType, validator, diagnostic))
            return true;
    }
    return false;
}

#endif
