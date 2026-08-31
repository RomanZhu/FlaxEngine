// Copyright (c) Wojciech Figat. All rights reserved.

#include "Types.h"

#if COMPILE_WITH_ASSETS_IMPORTER

#include "Engine/Core/Log.h"
#include "Engine/Content/Storage/FlaxStorage.h"
#include "Engine/Profiler/ProfilerMemory.h"

CreateAssetContext::CreateAssetContext(const StringView& inputPath, const StringView& outputPath, const Guid& id, void* arg, bool artifactStagingMode, const StringView& intendedTypeName)
    : _artifactStagingMode(artifactStagingMode)
    , _intendedAssetID(id)
    , _intendedTypeName(intendedTypeName)
    , _artifactOutputPath(outputPath)
{
    ASSERT(artifactStagingMode);
    InputPath = inputPath;
    OutputPath = outputPath;
    TargetAssetPath = outputPath;
    CustomArg = arg;
    Data.Header.ID = id;
    SkipMetadata = true;
}

CreateAssetResult CreateAssetContext::Run(const CreateAssetFunction& callback)
{
    ASSERT(callback.IsBinded());
    ASSERT(_artifactStagingMode);

    CreateAssetResult result = callback(*this);
    if (result != CreateAssetResult::Ok)
        return result;

    OutputPath = _artifactOutputPath;
    Data.Header.ID = _intendedAssetID;
    if (!OutputPath.EndsWith(ASSET_FILES_EXTENSION) || Data.Header.TypeName.IsEmpty() ||
        (!_intendedTypeName.IsEmpty() && Data.Header.TypeName != _intendedTypeName))
        return CreateAssetResult::InvalidTypeID;
    Data.Metadata.Release();
    return FlaxStorage::Create(OutputPath, Data) ? CreateAssetResult::CannotSaveFile : CreateAssetResult::Ok;
}

bool CreateAssetContext::AllocateChunk(int32 index)
{
    if (index < 0 || index >= ASSET_FILE_DATA_CHUNKS)
    {
        LOG(Warning, "Invalid asset chunk index {0}.", index);
        return true;
    }
    if (Data.Header.Chunks[index] != nullptr)
    {
        LOG(Warning, "Asset chunk {0} has already been allocated.", index);
        return true;
    }

    PROFILE_MEM(ContentFiles);
    Data.Header.Chunks[index] = New<FlaxChunk>();
    return false;
}

#endif
