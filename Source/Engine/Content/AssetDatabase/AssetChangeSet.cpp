// Copyright (c) Wojciech Figat. All rights reserved.

#include "AssetChangeSet.h"
#include "AssetDatabaseBinary.h"

using namespace SourceAssetDatabaseBinary;

namespace
{
    constexpr uint32 ChangeSetMagic = 0x53434146; // FACS
    constexpr uint32 ChangeSetVersion = 1;
}

bool AssetChangeSet::IsEmpty() const
{
    return !RefreshId.IsValid() && Pass == 0 && Added.IsEmpty() && Removed.IsEmpty() && Moved.IsEmpty() && SourceChanged.IsEmpty() &&
        MetadataChanged.IsEmpty() && Imported.IsEmpty() && ObjectsChanged.IsEmpty() &&
        StatusChanged.IsEmpty() && DiagnosticsChanged.IsEmpty();
}

void AssetChangeSet::Serialize(Array<byte>& output) const
{
    Writer writer;
    writer.Write(ChangeSetMagic);
    writer.Write(ChangeSetVersion);
    writer.Write(Revision);
    writer.Write(RefreshId);
    writer.Write(Pass);
    writer.Write((uint32)Added.Count());
    for (const AssetAddedChange& value : Added)
    {
        writer.Write(value.AssetGuid);
        writer.WriteString(value.Path);
    }
    writer.Write((uint32)Removed.Count());
    for (const AssetRemovedChange& value : Removed)
    {
        writer.Write(value.AssetGuid);
        writer.WriteString(value.PreviousPath);
    }
    writer.Write((uint32)Moved.Count());
    for (const AssetMovedChange& value : Moved)
    {
        writer.Write(value.AssetGuid);
        writer.WriteString(value.PreviousPath);
        writer.WriteString(value.Path);
    }
    writer.Write((uint32)SourceChanged.Count());
    for (const AssetSourceChangedChange& value : SourceChanged)
    {
        writer.Write(value.AssetGuid);
        writer.Write(value.PreviousHash);
        writer.Write(value.Hash);
    }
    writer.Write((uint32)MetadataChanged.Count());
    for (const AssetMetadataChangedChange& value : MetadataChanged)
    {
        writer.Write(value.AssetGuid);
        writer.Write(value.PreviousHash);
        writer.Write(value.Hash);
    }
    writer.Write((uint32)Imported.Count());
    for (const AssetImportedChange& value : Imported)
    {
        writer.Write(value.AssetGuid);
        writer.Write(value.LocalFileId);
        writer.WriteString(value.TargetId);
        writer.Write(value.Artifact);
    }
    writer.Write((uint32)ObjectsChanged.Count());
    for (const AssetObjectsChangedChange& value : ObjectsChanged)
    {
        writer.Write(value.AssetGuid);
        writer.Write((uint32)value.LocalFileIds.Count());
        for (const int64 localFileId : value.LocalFileIds)
            writer.Write(localFileId);
    }
    writer.Write((uint32)StatusChanged.Count());
    for (const AssetStatusChangedChange& value : StatusChanged)
    {
        writer.Write(value.AssetGuid);
        writer.Write((byte)value.Previous);
        writer.Write((byte)value.Status);
    }
    writer.Write((uint32)DiagnosticsChanged.Count());
    for (const AssetDiagnosticsChangedChange& value : DiagnosticsChanged)
    {
        writer.Write(value.AssetGuid);
        writer.Write(value.ActiveCount);
    }
    writer.Finish(output);
}

bool AssetChangeSet::Deserialize(const byte* data, uint32 length, AssetChangeSet& output)
{
    Reader reader(data, length);
    AssetChangeSet value;
    uint32 magic, version;
    uint32 count;
    if (reader.Read(magic) || reader.Read(version) || magic != ChangeSetMagic || version != ChangeSetVersion ||
        reader.Read(value.Revision) || reader.Read(value.RefreshId) || reader.Read(value.Pass) || reader.ReadCount(count))
        return true;
    if (value.Pass != 0 && !value.RefreshId.IsValid())
        return true;
    value.Added.Resize(count, false);
    for (AssetAddedChange& item : value.Added)
        if (reader.Read(item.AssetGuid) || reader.ReadString(item.Path))
            return true;

    if (reader.ReadCount(count))
        return true;
    value.Removed.Resize(count, false);
    for (AssetRemovedChange& item : value.Removed)
        if (reader.Read(item.AssetGuid) || reader.ReadString(item.PreviousPath))
            return true;

    if (reader.ReadCount(count))
        return true;
    value.Moved.Resize(count, false);
    for (AssetMovedChange& item : value.Moved)
        if (reader.Read(item.AssetGuid) || reader.ReadString(item.PreviousPath) || reader.ReadString(item.Path))
            return true;

    if (reader.ReadCount(count))
        return true;
    value.SourceChanged.Resize(count, false);
    for (AssetSourceChangedChange& item : value.SourceChanged)
        if (reader.Read(item.AssetGuid) || reader.Read(item.PreviousHash) || reader.Read(item.Hash))
            return true;

    if (reader.ReadCount(count))
        return true;
    value.MetadataChanged.Resize(count, false);
    for (AssetMetadataChangedChange& item : value.MetadataChanged)
        if (reader.Read(item.AssetGuid) || reader.Read(item.PreviousHash) || reader.Read(item.Hash))
            return true;

    if (reader.ReadCount(count))
        return true;
    value.Imported.Resize(count, false);
    for (AssetImportedChange& item : value.Imported)
        if (reader.Read(item.AssetGuid) || reader.Read(item.LocalFileId) || reader.ReadString(item.TargetId) || reader.Read(item.Artifact))
            return true;

    if (reader.ReadCount(count))
        return true;
    value.ObjectsChanged.Resize(count, false);
    for (AssetObjectsChangedChange& item : value.ObjectsChanged)
    {
        uint32 localCount;
        if (reader.Read(item.AssetGuid) || reader.ReadCount(localCount))
            return true;
        item.LocalFileIds.Resize(localCount, false);
        for (int64& localFileId : item.LocalFileIds)
            if (reader.Read(localFileId))
                return true;
    }

    if (reader.ReadCount(count))
        return true;
    value.StatusChanged.Resize(count, false);
    for (AssetStatusChangedChange& item : value.StatusChanged)
    {
        byte previous, status;
        if (reader.Read(item.AssetGuid) || reader.Read(previous) || reader.Read(status) ||
            previous > (byte)AssetRecordStatus::PathCollision || status > (byte)AssetRecordStatus::PathCollision)
            return true;
        item.Previous = (AssetRecordStatus)previous;
        item.Status = (AssetRecordStatus)status;
    }

    if (reader.ReadCount(count))
        return true;
    value.DiagnosticsChanged.Resize(count, false);
    for (AssetDiagnosticsChangedChange& item : value.DiagnosticsChanged)
        if (reader.Read(item.AssetGuid) || reader.Read(item.ActiveCount))
            return true;

    if (!reader.AtEnd())
        return true;
    output = MoveTemp(value);
    return false;
}
