// Copyright (c) Wojciech Figat. All rights reserved.

#include "AuthoredAssetFacade.h"
#include "AssetSaveService.h"
#include "Engine/Engine/Globals.h"
#include "Engine/Platform/CriticalSection.h"
#include "Engine/Platform/FileSystem.h"
#include <algorithm>

namespace
{
    CriticalSection StateLocker;
    String LastError;

    String AssetPipelineLibraryFolder()
    {
#if USE_EDITOR
        return Globals::ProjectLibraryFolder;
#else
        return Globals::ProjectFolder / TEXT("Library");
#endif
    }

    AssetSaveService& GetService()
    {
        static AssetSaveService service(Globals::ProjectFolder, Globals::ProjectContentFolder,
            AssetPipelineLibraryFolder() / TEXT("AssetDatabase/MutationJournals"),
            AssetPipelineLibraryFolder() / TEXT("AssetDatabase/Recovery"));
        return service;
    }

    String ResolvePath(const StringView& path)
    {
        String result = FileSystem::IsRelative(path) ? Globals::ProjectFolder / String(path) : String(path);
        FileSystem::NormalizePath(result);
        return result;
    }

    String StableKey(const Guid& objectInstanceID)
    {
        return TEXT("authored:") + objectInstanceID.ToString(Guid::FormatType::N).ToLower();
    }

    void SetError(const StringView& value)
    {
        ScopeLock lock(StateLocker);
        LastError = value;
    }
}

Guid AuthoredAssetFacade::CreateAsset(const StringView& path, const Guid& objectInstanceID, const StringView& typeName,
    const StringView& name, const StringView& dataJson)
{
    if (!objectInstanceID.IsValid())
    {
        SetError(TEXT("Authored asset object instance has no valid identity."));
        return Guid::Empty;
    }
    AssetSaveResult result;
    if (GetService().CreateAsset(ResolvePath(path), StableKey(objectInstanceID), typeName, name, StringAnsi(dataJson), result))
    {
        SetError(result.Message);
        return Guid::Empty;
    }
    SetError(StringView::Empty);
    return result.AssetID;
}

int64 AuthoredAssetFacade::AddObjectToAsset(const StringView& path, const Guid& objectInstanceID, const StringView& typeName,
    const StringView& name, const StringView& dataJson)
{
    if (!objectInstanceID.IsValid())
    {
        SetError(TEXT("Authored sub-object instance has no valid identity."));
        return 0;
    }
    int64 localId = 0;
    AssetSaveResult result;
    if (GetService().AddObjectToAsset(ResolvePath(path), StableKey(objectInstanceID), typeName, name, StringAnsi(dataJson), localId, result))
    {
        SetError(result.Message);
        return 0;
    }
    SetError(StringView::Empty);
    return localId;
}

bool AuthoredAssetFacade::RemoveObjectFromAsset(const StringView& path, int64 localId)
{
    AssetSaveResult result;
    const bool failed = GetService().RemoveObjectFromAsset(ResolvePath(path), localId, result);
    SetError(failed ? result.Message : StringView::Empty);
    return failed;
}

bool AuthoredAssetFacade::SetMainObject(const StringView& path, int64 localId)
{
    AssetSaveResult result;
    const bool failed = GetService().SetMainObject(ResolvePath(path), localId, result);
    SetError(failed ? result.Message : StringView::Empty);
    return failed;
}

bool AuthoredAssetFacade::StageObjectData(const StringView& path, int64 localId, const StringView& dataJson, const StringView& reason)
{
    AssetSaveResult result;
    const bool failed = GetService().StageObjectData(ResolvePath(path), localId, StringAnsi(dataJson), reason, result);
    SetError(failed ? result.Message : StringView::Empty);
    return failed;
}

bool AuthoredAssetFacade::IsDirty(const StringView& path)
{
    return GetService().IsDirty(ResolvePath(path));
}

bool AuthoredAssetFacade::SaveAssetIfDirty(const StringView& path)
{
    AssetSaveResult result;
    const bool failed = GetService().SaveAssetIfDirty(ResolvePath(path), result);
    SetError(failed ? result.Message : StringView::Empty);
    return failed;
}

String AuthoredAssetFacade::GetDirtyPaths()
{
    Array<String> paths;
    GetService().GetDirtyPaths(paths);
    if (paths.Count() > 1)
        std::sort(paths.Get(), paths.Get() + paths.Count());
    String result;
    for (const String& path : paths)
    {
        if (result.HasChars())
            result += TEXT("\n");
        result += path;
    }
    return result;
}

bool AuthoredAssetFacade::ForceReserialize(const StringView& path, bool includeMetadata)
{
    AssetSaveResult result;
    const bool failed = GetService().ForceReserialize(ResolvePath(path), includeMetadata, result);
    SetError(failed ? result.Message : StringView::Empty);
    return failed;
}

String AuthoredAssetFacade::GetLastError()
{
    ScopeLock lock(StateLocker);
    return LastError;
}
