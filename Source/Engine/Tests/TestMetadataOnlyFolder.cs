// Copyright (c) Wojciech Figat. All rights reserved.

#if FLAX_TESTS
using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using FlaxEditor;
using FlaxEditor.Content;
using FlaxEditor.Actions;
using NUnit.Framework;

namespace FlaxEngine.Tests
{
    [TestFixture]
    public class TestMetadataOnlyFolder
    {
        [Test]
        public void ProjectPanelLifecycle()
        {
            var folderPath = Path.Combine(Globals.ProjectContentFolder, "__MetadataOnlyFolder_" + Guid.NewGuid().ToString("N"));
            var metadataPath = folderPath + ".meta";
            var folderId = Guid.NewGuid();
            ContentItemFilesystemAction action = null;
            try
            {
                File.WriteAllText(metadataPath,
                    "{\n" +
                    "  \"fileFormatVersion\": 2,\n" +
                    "  \"guid\": \"" + folderId.ToString("N") + "\",\n" +
                    "  \"folderAsset\": true,\n" +
                    "  \"importer\": { \"id\": \"Flax.Folder\", \"version\": 1, \"settings\": {} },\n" +
                    "  \"labels\": [],\n" +
                    "  \"userData\": {}\n" +
                    "}\n");
                Assert.IsFalse(Directory.Exists(folderPath));

                Assert.IsFalse(AssetPipelineService.RefreshSources(new[] { folderPath }));
                Assert.IsTrue(Directory.Exists(folderPath));
                Assert.IsTrue(AssetDatabaseQueryService.TryGetRecord(folderId, out var record));
                Assert.AreEqual(AssetRecordStatus.Ready, record.Status);
                Assert.AreEqual(AssetSourceKind.Folder, record.SourceKind);

                var workspace = Editor.Instance.ContentDatabase;
                workspace.RefreshFolder(workspace.Game.Content.Folder, false);
                var item = workspace.Find(folderPath);
                Assert.IsInstanceOf<ContentFolder>(item);

                action = ContentItemFilesystemAction.Delete(Editor.Instance, new List<ContentItem> { item });
                Assert.IsNotNull(action, "Project-panel deletion rejected the reconstructed folder.");
                Assert.IsFalse(Directory.Exists(folderPath));
                Assert.IsFalse(File.Exists(metadataPath));
                Assert.IsNull(workspace.Find(folderPath));
                Assert.IsFalse(AssetDatabaseQueryService.TryGetRecord(folderId, out _));

                var diagnostics = AssetDatabaseQueryService.GetDiagnostics().Where(x =>
                    !string.IsNullOrEmpty(x.SourcePath) && ContentMutationPathUtils.Comparer.Equals(
                        ContentMutationPathUtils.Normalize(folderPath), ContentMutationPathUtils.Normalize(x.SourcePath))).ToArray();
                Assert.IsEmpty(diagnostics, string.Join(Environment.NewLine, diagnostics.Select(x => x.Code + ": " + x.Message)));
            }
            finally
            {
                action?.Dispose();
                if (Directory.Exists(folderPath))
                    Directory.Delete(folderPath, true);
                if (File.Exists(metadataPath))
                    File.Delete(metadataPath);
                AssetPipelineService.RefreshSources(new[] { folderPath }, false);
                Editor.Instance.ContentDatabase.RefreshFolder(Editor.Instance.ContentDatabase.Game.Content.Folder, false);
            }
        }

        public static int RunProjectPanelLifecycle()
        {
            new TestMetadataOnlyFolder().ProjectPanelLifecycle();
            return 0;
        }
    }
}
#endif
