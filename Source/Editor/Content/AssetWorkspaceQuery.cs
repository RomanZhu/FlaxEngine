// Copyright (c) Wojciech Figat. All rights reserved.

using System;
using System.IO;
using FlaxEngine;

namespace FlaxEditor.Content
{
    /// <summary>Immutable database-backed browser/search result.</summary>
    public sealed class AssetWorkspaceEntry
    {
        public Guid ObjectID { get; internal set; }
        public string TypeName { get; internal set; }
        public string SourcePath { get; internal set; }
        public string MetadataPath { get; internal set; }
        public string StableIdentifier { get; internal set; }
        public string DisplayName { get; internal set; }
        public string ImporterID { get; internal set; }
        public AssetRecordStatus Status { get; internal set; }
        public ulong Revision { get; internal set; }
        public bool IsMain { get; internal set; }
    }

    /// <summary>Database-backed asset workspace queries. No filesystem enumeration is performed.</summary>
    public static class AssetWorkspaceQuery
    {
        public static AssetWorkspaceEntry[] Query(string pathPrefix = null, string typeName = null, AssetRecordStatus? status = null,
            string name = null, string importerId = null, string label = null, Guid referencedAsset = default,
            Guid usedByAsset = default, bool mainAssetsOnly = false)
        {
            if (!string.IsNullOrEmpty(pathPrefix) && !pathPrefix.StartsWith("builtin://", StringComparison.OrdinalIgnoreCase))
            {
                pathPrefix = pathPrefix.Replace('/', Path.DirectorySeparatorChar);
                if (!Path.IsPathRooted(pathPrefix))
                    pathPrefix = Path.GetFullPath(Path.Combine(Globals.ProjectFolder, pathPrefix));
            }
            var records = AssetDatabaseQueryService.QueryRecords(new AssetDatabaseQuery
            {
                PathPrefix = pathPrefix,
                TypeName = typeName,
                Status = status.GetValueOrDefault(),
                HasStatus = status.HasValue,
                Name = name,
                ImporterID = importerId,
                Label = label,
                ReferencedAsset = referencedAsset,
                UsedByAsset = usedByAsset,
                MainAssetsOnly = mainAssetsOnly,
            });
            var result = new AssetWorkspaceEntry[records.Length];
            for (var i = 0; i < records.Length; i++)
                result[i] = ToEntry(records[i]);
            return result;
        }

        public static bool TryGet(Guid id, out AssetWorkspaceEntry entry)
        {
            if (AssetDatabaseQueryService.TryGetRecord(id, out var record))
            {
                entry = ToEntry(record);
                return true;
            }
            entry = null;
            return false;
        }

        public static bool TryGetMainAtPath(string path, out AssetWorkspaceEntry entry)
        {
            if (AssetDatabaseQueryService.TryGetMainRecordAtPath(path, out var record))
            {
                entry = ToEntry(record);
                return true;
            }
            entry = null;
            return false;
        }

        private static AssetWorkspaceEntry ToEntry(AssetDatabaseRecordInfo record)
        {
            return new AssetWorkspaceEntry
            {
                ObjectID = record.ID,
                TypeName = record.TypeName,
                SourcePath = record.SourcePath,
                MetadataPath = record.MetaPath,
                StableIdentifier = record.SubAssetKey,
                DisplayName = record.DisplayName,
                ImporterID = record.ProcessorID,
                Status = record.Status,
                Revision = record.Revision,
                IsMain = record.IsMain,
            };
        }
    }
}
