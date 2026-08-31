// Copyright (c) Wojciech Figat. All rights reserved.

using System;
using System.Collections.Generic;
using System.IO;
using FlaxEngine;

namespace FlaxEditor.Content
{
    /// <summary>Immutable database-backed browser/search result.</summary>
    public sealed class AssetWorkspaceEntry
    {
        public AssetObjectId ObjectID { get; internal set; }
        public Guid RuntimeID { get; internal set; }
        public string TypeName { get; internal set; }
        public string SourcePath { get; internal set; }
        public string MetadataPath { get; internal set; }
        public string StableIdentifier { get; internal set; }
        public string ImporterID { get; internal set; }
        public AssetRecordStatus Status { get; internal set; }
        public ulong Revision { get; internal set; }
        public bool IsMain { get; internal set; }
    }

    /// <summary>Database-backed asset workspace queries. No filesystem enumeration is performed.</summary>
    public static class AssetWorkspaceQuery
    {
        public static AssetWorkspaceEntry[] Query(string pathPrefix = null, string typeName = null, AssetRecordStatus? status = null)
        {
            if (!string.IsNullOrEmpty(pathPrefix) && !pathPrefix.StartsWith("builtin://", StringComparison.OrdinalIgnoreCase))
            {
                pathPrefix = pathPrefix.Replace('/', Path.DirectorySeparatorChar);
                if (!Path.IsPathRooted(pathPrefix))
                    pathPrefix = Path.GetFullPath(Path.Combine(Globals.ProjectFolder, pathPrefix));
            }
            var records = AssetDatabaseFacade.GetRecords();
            var result = new List<AssetWorkspaceEntry>(records.Length);
            for (var i = 0; i < records.Length; i++)
            {
                var record = records[i];
                if (pathPrefix != null && !record.SourcePath.Replace('/', Path.DirectorySeparatorChar).StartsWith(pathPrefix, StringComparison.OrdinalIgnoreCase))
                    continue;
                if (typeName != null && !string.Equals(record.TypeName, typeName, StringComparison.Ordinal))
                    continue;
                if (status.HasValue && record.Status != status.Value)
                    continue;
                result.Add(ToEntry(record));
            }
            result.Sort((a, b) =>
            {
                int path = string.Compare(a.SourcePath, b.SourcePath, StringComparison.OrdinalIgnoreCase);
                return path != 0 ? path : a.ObjectID.LocalId.CompareTo(b.ObjectID.LocalId);
            });
            return result.ToArray();
        }

        public static bool TryGet(AssetObjectId id, out AssetWorkspaceEntry entry)
        {
            var records = AssetDatabaseFacade.GetRecords();
            for (var i = 0; i < records.Length; i++)
            {
                var record = records[i];
                if (record.SourceAssetID == id.Asset.Value && record.LocalId == id.LocalId)
                {
                    entry = ToEntry(record);
                    return true;
                }
            }
            entry = null;
            return false;
        }

        private static AssetWorkspaceEntry ToEntry(AssetDatabaseRecordInfo record)
        {
            return new AssetWorkspaceEntry
            {
                ObjectID = new AssetObjectId(new AssetGuid(record.SourceAssetID), record.LocalId),
                RuntimeID = record.ID,
                TypeName = record.TypeName,
                SourcePath = record.SourcePath,
                MetadataPath = record.MetaPath,
                StableIdentifier = record.SubAssetKey,
                ImporterID = record.ProcessorID,
                Status = record.Status,
                Revision = record.Revision,
                IsMain = record.IsMain,
            };
        }
    }
}
