// Copyright (c) Wojciech Figat. All rights reserved.

using System.IO;
using FlaxEngine;

namespace FlaxEditor.Content.Import
{
    /// <summary>
    /// Folder import entry.
    /// </summary>
    public class FolderImportEntry : ImportFileEntry
    {
        /// <summary>
        /// Flag used to skip showing import settings dialog to used. Can be used for importing assets from code by plugins.
        /// </summary>
        public bool SkipSettingsDialog;

        /// <inheritdoc />
        public FolderImportEntry(ref Request request)
        : base(ref request)
        {
            SkipSettingsDialog = request.SkipSettingsDialog;
        }

        /// <inheritdoc />
        public override bool Import()
        {
            if (Directory.Exists(ResultUrl) || File.Exists(ResultUrl))
            {
                Editor.LogWarning("Cannot import folder because the destination already exists: " + ResultUrl);
                return true;
            }
            var parentPath = Path.GetDirectoryName(ResultUrl);
            var parent = Editor.Instance.ContentDatabase.Find(parentPath);
            if (parent == null)
            {
                Editor.LogWarning("Failed to find the parent folder for the imported directory.");
                return true;
            }
            var createResult = Editor.Instance.ContentDatabase.CreatePath(ResultUrl, true, () => Directory.CreateDirectory(ResultUrl));
            if (!createResult.Succeeded)
            {
                Editor.LogWarning(createResult.Message ?? "Failed to create the imported directory.");
                return true;
            }
            if (AssetPipelineService.RefreshSources(new[] { ResultUrl }))
            {
                foreach (var diagnostic in AssetDatabaseQueryService.GetDiagnostics())
                    Editor.LogWarning($"Failed to register imported folder {diagnostic.SourcePath}: {diagnostic.Message}");
                return true;
            }
            Editor.Instance.ContentDatabase.RefreshFolder(parent, true);
            var target = (ContentFolder)Editor.Instance.ContentDatabase.Find(ResultUrl);
            if (target == null)
            {
                Editor.LogWarning("Failed to index the imported directory: " + ResultUrl);
                return true;
            }

            // Import all sub elements
            var files = Directory.GetFiles(SourceUrl);
            if (files.Length != 0)
                Editor.Instance.ContentImporting.Import(files, target, SkipSettingsDialog);

            // Import all sub dirs
            var folders = Directory.GetDirectories(SourceUrl);
            if (folders.Length != 0)
                Editor.Instance.ContentImporting.Import(folders, target, SkipSettingsDialog);

            return false;
        }
    }
}
