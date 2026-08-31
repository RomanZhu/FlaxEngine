// Copyright (c) Wojciech Figat. All rights reserved.

#include "AssetObjectRegistry.h"
#include "Engine/Core/Log.h"
#include "Engine/Core/Types/Stopwatch.h"
#include "Engine/Engine/Globals.h"
#include "Engine/Platform/FileSystem.h"
#include "Engine/Profiler/ProfilerCPU.h"
#if USE_EDITOR
#include "Engine/Content/Storage/ContentStorageManager.h"
#include "Engine/Content/Storage/FlaxStorage.h"
#endif

namespace
{
    String NormalizeObjectPath(const StringView& path)
    {
        String result(path);
        FileSystem::NormalizePath(result);
        return result;
    }

#if !USE_EDITOR
    void MakeRuntimeAssetInfo(const RuntimeAssetCatalogEntry& entry, AssetInfo& info)
    {
        const Guid runtimeId = entry.Object.ToRuntimeObjectGuid();
        info = AssetInfo(runtimeId, entry.Object, String(entry.TypeName), Globals::ProjectContentFolder / String(entry.PackageName));
        FileSystem::NormalizePath(info.Path);
    }
#endif
}

int32 AssetObjectRegistry::Size() const
{
#if USE_EDITOR
    ScopeLock lock(_locker);
    return _objects.Count();
#else
    return _runtimeCatalog.GetEntries().Count();
#endif
}

AssetObjectId AssetObjectRegistry::GetGameSettingsObject() const
{
#if USE_EDITOR
    return AssetObjectId();
#else
    return _runtimeCatalog.GetGameSettingsObject();
#endif
}

void AssetObjectRegistry::Init()
{
    PROFILE_CPU();
#if !USE_EDITOR
    const String path = Globals::ProjectContentFolder / TEXT("RuntimeAssetCatalog.bin");
    AssetPipelineDiagnostic diagnostic;
    if (RuntimeAssetCatalog::Load(path, _runtimeCatalog, diagnostic))
    {
        LOG(Error, "Cannot load runtime asset catalog: {0}", diagnostic.Message);
        return;
    }

    Stopwatch stopwatch;
    int32 missingCount = 0;
    for (const RuntimeAssetCatalogEntry& entry : _runtimeCatalog.GetEntries())
    {
        AssetInfo info;
        MakeRuntimeAssetInfo(entry, info);
        if (!FileSystem::FileExists(info.Path))
            missingCount++;
    }
    stopwatch.Stop();
    if (missingCount != 0)
        LOG(Error, "Runtime asset catalog references {0} missing package objects.", missingCount);
    LOG(Info, "Runtime asset catalog loaded {0} objects and {1} path aliases in {2}ms ({3} missing)",
        _runtimeCatalog.GetEntries().Count(), _runtimeCatalog.GetAliases().Count(), stopwatch.GetMilliseconds(), missingCount);
#endif
}

bool AssetObjectRegistry::FindObject(const AssetObjectId& objectId, AssetInfo& info)
{
    PROFILE_CPU();
    if (!objectId.IsValid())
        return false;
#if USE_EDITOR
    ScopeLock lock(_locker);
    Entry* entry = _objects.TryGet(objectId);
    if (entry == nullptr)
        return false;
    if (!FileSystem::FileExists(entry->Info.Path))
    {
        _runtimeObjects.Remove(entry->Info.ID);
        _objects.Remove(objectId);
        return false;
    }
    info = entry->Info;
    return true;
#else
    RuntimeAssetCatalogEntry entry;
    if (!_runtimeCatalog.TryGet(objectId, entry))
        return false;
    MakeRuntimeAssetInfo(entry, info);
    return FileSystem::FileExists(info.Path);
#endif
}

bool AssetObjectRegistry::FindRuntimeObject(const Guid& runtimeId, AssetInfo& info)
{
    PROFILE_CPU();
    if (!runtimeId.IsValid())
        return false;
#if USE_EDITOR
    AssetObjectId objectId;
    {
        ScopeLock lock(_locker);
        if (!_runtimeObjects.TryGet(runtimeId, objectId))
            return false;
    }
    return FindObject(objectId, info);
#else
    for (const RuntimeAssetCatalogEntry& entry : _runtimeCatalog.GetEntries())
    {
        if (entry.Object.ToRuntimeObjectGuid() == runtimeId)
            return FindObject(entry.Object, info);
    }
    return false;
#endif
}

bool AssetObjectRegistry::FindObject(const StringView& path, AssetInfo& info)
{
    PROFILE_CPU();
#if USE_EDITOR
    const String normalizedPath = NormalizeObjectPath(path);
    AssetObjectId foundObject;
    {
        ScopeLock lock(_locker);
        for (auto i = _objects.Begin(); i.IsNotEnd(); ++i)
        {
            if (i->Value.Info.Path == normalizedPath)
            {
                foundObject = i->Key;
                break;
            }
        }
    }
    return foundObject.IsValid() && FindObject(foundObject, info);
#else
    String aliasPath = NormalizeObjectPath(path);
    const String startupPath = NormalizeObjectPath(Globals::StartupFolder);
    if (aliasPath.StartsWith(startupPath) && aliasPath.Length() > startupPath.Length() && aliasPath[startupPath.Length()] == '/')
        aliasPath = aliasPath.Right(aliasPath.Length() - startupPath.Length() - 1);
    ContentHash aliasHash;
    AssetObjectId objectId;
    return !RuntimeAssetCatalog::HashPathAlias(aliasPath, aliasHash) &&
        _runtimeCatalog.TryGetByPathHash(aliasHash, objectId) && FindObject(objectId, info);
#endif
}

StringView AssetObjectRegistry::GetEditorObjectPath(const AssetObjectId& objectId) const
{
#if USE_EDITOR
    ScopeLock lock(_locker);
    const Entry* entry = _objects.TryGet(objectId);
    return entry ? entry->Info.Path : String::Empty;
#else
    return String::Empty;
#endif
}

void AssetObjectRegistry::GetAllRuntimeIds(Array<Guid>& result) const
{
#if USE_EDITOR
    ScopeLock lock(_locker);
    _runtimeObjects.GetKeys(result);
#else
    result.EnsureCapacity(result.Count() + _runtimeCatalog.GetEntries().Count());
    for (const RuntimeAssetCatalogEntry& entry : _runtimeCatalog.GetEntries())
        result.Add(entry.Object.ToRuntimeObjectGuid());
#endif
}

void AssetObjectRegistry::GetAllRuntimeIdsByTypeName(const StringView& typeName, Array<Guid>& result) const
{
#if USE_EDITOR
    ScopeLock lock(_locker);
    for (auto i = _objects.Begin(); i.IsNotEnd(); ++i)
    {
        if (i->Value.Info.TypeName == typeName)
            result.Add(i->Value.Info.ID);
    }
#else
    for (const RuntimeAssetCatalogEntry& entry : _runtimeCatalog.GetEntries())
    {
        if (String(entry.TypeName) == typeName)
            result.Add(entry.Object.ToRuntimeObjectGuid());
    }
#endif
}

#if USE_EDITOR

void AssetObjectRegistry::RegisterTransientPackage(FlaxStorage* storage)
{
    PROFILE_CPU();
    ASSERT(storage);
    Array<FlaxStorage::Entry> entries;
    storage->GetEntries(entries);
    const String path = NormalizeObjectPath(storage->GetPath());
    ScopeLock lock(_locker);

    for (auto i = _objects.Begin(); i.IsNotEnd(); ++i)
    {
        if (i->Value.Info.Path == path)
        {
            _runtimeObjects.Remove(i->Value.Info.ID);
            _objects.Remove(i);
        }
    }

    for (const FlaxStorage::Entry& storageEntry : entries)
    {
        const AssetObjectId objectId = AssetObjectId::Main(AssetGuid(storageEntry.ID));
        AssetObjectId existingObject;
        if (_runtimeObjects.TryGet(storageEntry.ID, existingObject) && existingObject != objectId)
        {
            LOG(Error, "Transient runtime object id {0} collides between {1} and {2}.", storageEntry.ID, existingObject, objectId);
            continue;
        }
        _objects[objectId] = Entry(storageEntry.ID, objectId, storageEntry.TypeName, path);
        _runtimeObjects[storageEntry.ID] = objectId;
    }
}

void AssetObjectRegistry::RegisterTransientPackage(const FlaxStorageReference& storage)
{
    RegisterTransientPackage(storage.Get());
}

void AssetObjectRegistry::RegisterTransientObject(const AssetHeader& header, const StringView& path)
{
    RegisterTransientObject(header.ID, header.TypeName, path);
}

void AssetObjectRegistry::RegisterTransientObject(const Guid& runtimeId, const StringView& typeName, const StringView& path)
{
    PROFILE_CPU();
    if (!runtimeId.IsValid())
        return;
    const AssetObjectId objectId = AssetObjectId::Main(AssetGuid(runtimeId));
    const String normalizedPath = NormalizeObjectPath(path);
    ScopeLock lock(_locker);

    AssetObjectId oldObject;
    if (_runtimeObjects.TryGet(runtimeId, oldObject) && oldObject != objectId)
        _objects.Remove(oldObject);
    for (auto i = _objects.Begin(); i.IsNotEnd(); ++i)
    {
        if (i->Key != objectId && i->Value.Info.Path == normalizedPath)
        {
            _runtimeObjects.Remove(i->Value.Info.ID);
            _objects.Remove(i);
        }
    }
    _objects[objectId] = Entry(runtimeId, objectId, typeName, normalizedPath);
    _runtimeObjects[runtimeId] = objectId;
}

bool AssetObjectRegistry::RemoveTransientObject(const AssetObjectId& objectId, AssetInfo* info)
{
    ScopeLock lock(_locker);
    Entry* entry = _objects.TryGet(objectId);
    if (entry == nullptr)
        return false;
    if (info)
        *info = entry->Info;
    _runtimeObjects.Remove(entry->Info.ID);
    _objects.Remove(objectId);
    return true;
}

bool AssetObjectRegistry::RemoveTransientRuntimeObject(const Guid& runtimeId, AssetInfo* info)
{
    AssetObjectId objectId;
    {
        ScopeLock lock(_locker);
        if (!_runtimeObjects.TryGet(runtimeId, objectId))
            return false;
    }
    return RemoveTransientObject(objectId, info);
}

bool AssetObjectRegistry::RemoveTransientPackage(const StringView& path, AssetInfo* info)
{
    const String normalizedPath = NormalizeObjectPath(path);
    ScopeLock lock(_locker);
    bool removed = false;
    for (auto i = _objects.Begin(); i.IsNotEnd(); ++i)
    {
        if (i->Value.Info.Path == normalizedPath)
        {
            if (info && !removed)
                *info = i->Value.Info;
            _runtimeObjects.Remove(i->Value.Info.ID);
            _objects.Remove(i);
            removed = true;
        }
    }
    return removed;
}

bool AssetObjectRegistry::RenameTransientPackage(const StringView& oldPath, const StringView& newPath)
{
    const String normalizedOldPath = NormalizeObjectPath(oldPath);
    const String normalizedNewPath = NormalizeObjectPath(newPath);
    ScopeLock lock(_locker);
    bool renamed = false;
    for (auto i = _objects.Begin(); i.IsNotEnd(); ++i)
    {
        if (i->Value.Info.Path == normalizedOldPath)
        {
            i->Value.Info.Path = normalizedNewPath;
            renamed = true;
        }
    }
    return renamed;
}

void AssetObjectRegistry::RenameTransientFolder(const StringView& oldPath, const StringView& newPath)
{
    const String normalizedOldPath = NormalizeObjectPath(oldPath);
    const String normalizedNewPath = NormalizeObjectPath(newPath);
    ScopeLock lock(_locker);
    for (auto i = _objects.Begin(); i.IsNotEnd(); ++i)
    {
        const String& path = i->Value.Info.Path;
        if (path.Length() <= normalizedOldPath.Length() || !path.StartsWith(normalizedOldPath, StringSearchCase::IgnoreCase))
            continue;
        const Char separator = path[normalizedOldPath.Length()];
        if (separator == '/' || separator == '\\')
            i->Value.Info.Path = normalizedNewPath + path.Substring(normalizedOldPath.Length());
    }
}

#endif
