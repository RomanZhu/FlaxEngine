// Copyright (c) Wojciech Figat. All rights reserved.

#include "GeneratedAssetBuilder.h"

#if COMPILE_WITH_ASSETS_IMPORTER

#include "Engine/Content/AssetObjectRegistry.h"
#include "Engine/Content/Content.h"
#include "Engine/Content/Storage/ContentStorageManager.h"
#include "Engine/Core/Log.h"
#include "Engine/Core/ScopeExit.h"
#include "Engine/Core/Utilities.h"
#include "Engine/Engine/Globals.h"
#include "Engine/Platform/FileSystem.h"
#include "Engine/Platform/Platform.h"

bool GeneratedAssetBuilder::UseRelativeSourcePaths = false;

bool GeneratedAssetBuilder::Build(const CreateAssetFunction& callback, const StringView& outputPath, const StringView& expectedType,
    Guid& assetID, void* argument)
{
    return BuildInternal(callback, StringView::Empty, outputPath, expectedType, assetID, argument);
}

bool GeneratedAssetBuilder::BuildFromSource(const CreateAssetFunction& callback, const StringView& sourcePath, const StringView& outputPath,
    const StringView& expectedType, Guid& assetID, void* argument)
{
    if (!FileSystem::FileExists(sourcePath))
    {
        LOG(Error, "Missing generated-asset source '{0}'.", sourcePath);
        return true;
    }
    return BuildInternal(callback, sourcePath, outputPath, expectedType, assetID, argument);
}

bool GeneratedAssetBuilder::BuildFromSourceIfModified(const CreateAssetFunction& callback, const StringView& sourcePath, const StringView& outputPath,
    const StringView& expectedType, Guid& assetID, void* argument)
{
    if (FileSystem::FileExists(outputPath) && FileSystem::GetFileLastEditTime(sourcePath) <= FileSystem::GetFileLastEditTime(outputPath))
    {
        if (!assetID.IsValid())
        {
            AssetInfo info;
            if (Content::GetAssetInfo(outputPath, info))
                assetID = info.ID;
        }
        return false;
    }
    return BuildFromSource(callback, sourcePath, outputPath, expectedType, assetID, argument);
}

String GeneratedAssetBuilder::GetSourceReference(const String& path)
{
    if (UseRelativeSourcePaths && !FileSystem::IsRelative(path)
#if PLATFORM_WINDOWS
        && path.Length() > 2 && Globals::ProjectFolder.Length() > 2 && path[0] == Globals::ProjectFolder[0]
#endif
    )
        return FileSystem::ConvertAbsolutePathToRelative(Globals::ProjectFolder, path);
    return path;
}

bool GeneratedAssetBuilder::BuildInternal(const CreateAssetFunction& callback, const StringView& sourcePath, const StringView& outputPath,
    const StringView& expectedType, Guid& assetID, void* argument)
{
    if (!callback.IsBinded() || !outputPath.EndsWith(ASSET_FILES_EXTENSION))
        return true;

    AssetInfo existing;
    if (Content::GetAssetInfo(outputPath, existing))
        assetID = existing.ID;
    else if (!assetID.IsValid())
        assetID = Guid::New();

    const String directory = StringUtils::GetDirectoryName(outputPath);
    if (FileSystem::CreateDirectory(directory))
    {
        LOG(Error, "Cannot create generated asset directory '{0}'.", directory);
        return true;
    }

    const String stagingPath = Content::CreateTemporaryAssetPath();
    SCOPE_EXIT
    {
        ContentStorageManager::EnsureAccess(stagingPath);
        FileSystem::DeleteFile(stagingPath);
    };

    const double startTime = Platform::GetTimeSeconds();
    CreateAssetContext context(sourcePath, stagingPath, assetID, argument, true, expectedType);
    context.TargetAssetPath = outputPath;
    const CreateAssetResult result = context.Run(callback);
    if (result != CreateAssetResult::Ok)
    {
        if (result != CreateAssetResult::Abort && result != CreateAssetResult::Skip)
            LOG(Error, "Cannot build generated asset '{0}'. Result: {1}", outputPath, ::ToString(result));
        return true;
    }

    auto storage = ContentStorageManager::TryGetStorage(outputPath);
    if (storage && storage->IsLoaded() && storage->CloseFileHandles())
    {
        LOG(Error, "Cannot close generated asset storage '{0}'.", outputPath);
        return true;
    }
    if (FileSystem::MoveFile(outputPath, stagingPath, true))
    {
        LOG(Error, "Cannot publish generated asset '{0}'.", outputPath);
        return true;
    }
    if (storage)
        storage->Reload();
    Content::GetObjectRegistry()->RegisterTransientObject(context.Data.Header, outputPath);
    LOG(Info, "Generated asset '{0}' built in {1}s.", outputPath,
        Utilities::RoundTo2DecimalPlaces(Platform::GetTimeSeconds() - startTime));
    return false;
}

#endif
