// Copyright (c) Wojciech Figat. All rights reserved.

#include "LegacyImporterAdapter.h"

#if COMPILE_WITH_ASSETS_IMPORTER

#include "Engine/Core/ScopeExit.h"
#include "Engine/Platform/File.h"
#include "Engine/Platform/FileSystem.h"

bool LegacyImporterAdapter::Build(const CreateAssetFunction& callback, ArtifactBuildContext& buildContext,
    const StringView& inputIdentity, const StringAnsiView& outputKind, const StringView& outputFileName,
    const Guid& intendedAssetId, const StringView& intendedTypeName, void* customArgument,
    AssetPipelineDiagnostic& diagnostic)
{
    diagnostic = AssetPipelineDiagnostic();
    if (!callback.IsBinded() || !intendedAssetId.IsValid() || intendedTypeName.IsEmpty())
    {
        diagnostic.Code = AssetPipelineDiagnosticCode::BuildFailed;
        diagnostic.Stage = AssetPipelineDiagnosticStage::Build;
        diagnostic.AssetGuid = intendedAssetId;
        diagnostic.Message = TEXT("Legacy importer adapter received an invalid callback, identity, or type contract.");
        return true;
    }

    // Validate current bytes through the controlled input before handing its declared path to trusted legacy code.
    Array<byte> verifiedInput;
    ContentHash verifiedHash;
    if (buildContext.ReadInput(inputIdentity, verifiedInput, verifiedHash, diagnostic))
        return true;
    String inputPath;
    if (buildContext.TryGetInputPath(inputIdentity, inputPath, diagnostic))
        return true;
    String scratchPath;
    if (buildContext.CreateScratchFilePath(TEXT(".flax"), scratchPath, diagnostic))
        return true;
    SCOPE_EXIT
    {
        if (FileSystem::FileExists(scratchPath))
            FileSystem::DeleteFile(scratchPath);
    };

    CreateAssetContext legacyContext(inputPath, scratchPath, intendedAssetId, customArgument, true, intendedTypeName);
    const CreateAssetResult result = legacyContext.Run(callback);
    if (result != CreateAssetResult::Ok)
    {
        diagnostic.Code = result == CreateAssetResult::Abort ? AssetPipelineDiagnosticCode::BuildCancelled : AssetPipelineDiagnosticCode::BuildFailed;
        diagnostic.Stage = AssetPipelineDiagnosticStage::Build;
        diagnostic.AssetGuid = intendedAssetId;
        diagnostic.SourcePath = inputPath;
        diagnostic.OutputKind = String(outputKind);
        diagnostic.Message = TEXT("Legacy importer callback failed while producing a staged artifact.");
        diagnostic.Related.Add(::ToString(result));
        return true;
    }
    Array<byte> artifactBytes;
    if (File::ReadAllBytes(scratchPath, artifactBytes))
    {
        diagnostic.Code = AssetPipelineDiagnosticCode::ArtifactInvalid;
        diagnostic.Stage = AssetPipelineDiagnosticStage::Build;
        diagnostic.AssetGuid = intendedAssetId;
        diagnostic.SourcePath = scratchPath;
        diagnostic.OutputKind = String(outputKind);
        diagnostic.Message = TEXT("Legacy importer did not produce a readable staged artifact.");
        return true;
    }
    ArtifactWriter writer;
    if (buildContext.OpenOutput(outputKind, writer, diagnostic))
        return true;
    return writer.WriteFile(outputFileName, artifactBytes.Get(), artifactBytes.Count(), diagnostic);
}

#endif
