// Copyright (c) Wojciech Figat. All rights reserved.

#if FLAX_TESTS
using System;
using System.Globalization;
using System.IO;
using FlaxEditor.Actions;
using FlaxEditor.Content;
using FlaxEditor.GUI.ContextMenu;
using FlaxEditor.Modules;
using FlaxEditor.Windows;
using NUnit.Framework;

namespace FlaxEngine.Tests
{
    /// <summary>
    /// Tests for <see cref="FlaxEditor.Utilities.Utils"/>.
    /// </summary>
    [TestFixture]
    public class TestEditorUtils
    {
        /// <summary>
        /// Test floating point values formatting to readable text.
        /// </summary>
        [Test]
        public void TestFormatFloat()
        {
            CultureInfo.CurrentCulture = CultureInfo.InvariantCulture;

            Assert.AreEqual("0", FlaxEditor.Utilities.Utils.FormatFloat(0.0f));
            Assert.AreEqual("0", FlaxEditor.Utilities.Utils.FormatFloat(0.0d));
            Assert.AreEqual("0.1234", FlaxEditor.Utilities.Utils.FormatFloat(0.1234f));
            Assert.AreEqual("0.1234", FlaxEditor.Utilities.Utils.FormatFloat(0.1234d));
            Assert.AreEqual("1234", FlaxEditor.Utilities.Utils.FormatFloat(1234.0f));
            Assert.AreEqual("1234", FlaxEditor.Utilities.Utils.FormatFloat(1234.0d));
            Assert.AreEqual("1234.123", FlaxEditor.Utilities.Utils.FormatFloat(1234.123f));
            Assert.AreEqual("1234.1234", FlaxEditor.Utilities.Utils.FormatFloat(1234.1234d));

            double[] values =
            {
                123450000000000000.0, 1.0 / 7, 10000000000.0 / 7, 100000000000000000.0 / 7, 0.001 / 7, 0.0001 / 7, 100000000000000000.0, 0.00000000001,
                1.23e-2, 1.234e-5, 1.2345E-10, 1.23456E-20, 5E-20, 1.23E+2, 1.234e5, 1.2345E10, -7.576E-05, 1.23456e20, 5e+20, 9.1093822E-31, 5.9736e24,
                double.Epsilon, Mathd.Epsilon, Mathf.Epsilon
            };
            foreach (int sign in new[] { 1, -1 })
            {
                foreach (double value in values)
                {
                    double value1 = sign * value;
                    string text = FlaxEditor.Utilities.Utils.FormatFloat(value1);
                    Assert.IsFalse(text.Contains("e", StringComparison.Ordinal));
                    Assert.IsFalse(text.Contains("E", StringComparison.Ordinal));
                    double value2 = double.Parse(text);
                    Assert.AreEqual(value2, value1);
                }
            }
            foreach (int sign in new[] { 1, -1 })
            {
                foreach (double value in values)
                {
                    float value1 = (float)(sign * value);
                    string text = FlaxEditor.Utilities.Utils.FormatFloat(value1);
                    Assert.IsFalse(text.Contains("e", StringComparison.Ordinal));
                    Assert.IsFalse(text.Contains("E", StringComparison.Ordinal));
                    float value2 = float.Parse(text);
                    Assert.AreEqual(value2, value1);
                }
            }
        }

        [Test]
        public void TestSceneItemsUseContentBackendForFileOperations()
        {
            var sceneItem = new SceneItem("C:/Project/Content/Scene.scene", Guid.NewGuid());
            var fileItem = new FileItem("C:/Project/Content/Notes.txt");

            Assert.IsTrue(ContentDatabaseModule.UseContentBackendForFileOperation(sceneItem));
            Assert.IsFalse(ContentDatabaseModule.UseContentBackendForFileOperation(fileItem));
            Assert.IsFalse(ContentDatabaseModule.UseContentBackendForFileOperation(null));
        }

        [Test]
        public void TestContentCopyRejectsFileCollisionWithoutChangingBytes()
        {
            var root = Path.Combine(Path.GetTempPath(), "FlaxContentCopyTests", Guid.NewGuid().ToString("N"));
            Directory.CreateDirectory(root);
            try
            {
                var sourcePath = Path.Combine(root, "Source.txt");
                var destinationPath = Path.Combine(root, "Destination.txt");
                File.WriteAllText(sourcePath, "source");
                File.WriteAllText(destinationPath, "destination");
                var database = new ContentDatabaseModule(null);

                var result = database.Copy(new FileItem(sourcePath), destinationPath);

                Assert.IsFalse(result.Succeeded);
                Assert.AreEqual(ContentMutationFailure.DestinationCollision, result.Failure);
                Assert.AreEqual("source", File.ReadAllText(sourcePath));
                Assert.AreEqual("destination", File.ReadAllText(destinationPath));
            }
            finally
            {
                Directory.Delete(root, true);
            }
        }

        [Test]
        public void TestContentCopyReturnsCreatedDestinationForPlainFile()
        {
            var root = Path.Combine(Path.GetTempPath(), "FlaxContentCopyTests", Guid.NewGuid().ToString("N"));
            Directory.CreateDirectory(root);
            try
            {
                var sourcePath = Path.Combine(root, "Source.txt");
                var destinationPath = Path.Combine(root, "Destination.txt");
                File.WriteAllText(sourcePath, "source");
                var database = new ContentDatabaseModule(null);

                var result = database.Copy(new FileItem(sourcePath), destinationPath);

                Assert.IsTrue(result.Succeeded);
                Assert.IsTrue(result.CreatedDestination);
                Assert.AreEqual("source", File.ReadAllText(destinationPath));
            }
            finally
            {
                Directory.Delete(root, true);
            }
        }

        [Test]
        public void TestContentCopyRejectsFolderAndCrossTypeCollisions()
        {
            var root = Path.Combine(Path.GetTempPath(), "FlaxContentCopyTests", Guid.NewGuid().ToString("N"));
            Directory.CreateDirectory(root);
            try
            {
                var sourceFolderPath = Path.Combine(root, "SourceFolder");
                var destinationFolderPath = Path.Combine(root, "DestinationFolder");
                var fileCollisionPath = Path.Combine(root, "FileCollision");
                Directory.CreateDirectory(sourceFolderPath);
                Directory.CreateDirectory(destinationFolderPath);
                File.WriteAllText(Path.Combine(sourceFolderPath, "Source.txt"), "source");
                File.WriteAllText(Path.Combine(destinationFolderPath, "Existing.txt"), "existing");
                File.WriteAllText(fileCollisionPath, "existing file");
                var sourceFolder = new ContentFolder(ContentFolderType.Content, sourceFolderPath, null);
                var database = new ContentDatabaseModule(null);

                var folderResult = database.Copy(sourceFolder, destinationFolderPath);
                var crossTypeResult = database.Copy(sourceFolder, fileCollisionPath);

                Assert.IsFalse(folderResult.Succeeded);
                Assert.IsFalse(crossTypeResult.Succeeded);
                Assert.AreEqual("existing", File.ReadAllText(Path.Combine(destinationFolderPath, "Existing.txt")));
                Assert.AreEqual("existing file", File.ReadAllText(fileCollisionPath));
            }
            finally
            {
                Directory.Delete(root, true);
            }
        }

        [Test]
        public void TestNewItemSurvivesContentRefreshBeforeFileExists()
        {
            var folder = new ContentFolder(ContentFolderType.Content, Path.Combine(Path.GetTempPath(), "FlaxContentFolder"), null);
            var proxy = new GenericJsonAssetProxy();
            var newItem = new NewItem(Path.Combine(folder.Path, "Json Asset.json"), proxy, null);
            var fileItem = new FileItem(Path.Combine(folder.Path, "Notes.txt"));

            Assert.IsFalse(ContentDatabaseModule.ShouldRemoveMissingContentItem(newItem));
            Assert.IsTrue(ContentDatabaseModule.ShouldRemoveMissingContentItem(fileItem));
            Assert.IsFalse(ContentDatabaseModule.ShouldRemoveMissingContentItem(null));
        }

        [Test]
        public void TestContentNamesRejectWhitespaceTrailingDotAndCrossTypeCollision()
        {
            var root = Path.Combine(Path.GetTempPath(), "FlaxContentNameTests", Guid.NewGuid().ToString("N"));
            Directory.CreateDirectory(root);
            try
            {
                var proxy = new GenericJsonAssetProxy();
                var item = new NewItem(Path.Combine(root, "Original.json"), proxy, null);
                var editing = new ContentEditingModule(null);
                Directory.CreateDirectory(Path.Combine(root, "Collision.json"));

                Assert.IsFalse(editing.IsValidAssetName(item, "   ", out _));
                Assert.IsFalse(editing.IsValidAssetName(item, "Trailing.", out _));
                Assert.IsFalse(editing.IsValidAssetName(item, "Trailing ", out _));
                Assert.IsFalse(editing.IsValidAssetName(item, "Collision", out _));
                if (OperatingSystem.IsWindows())
                    Assert.IsFalse(editing.IsValidAssetName(item, "CON", out _));
            }
            finally
            {
                Directory.Delete(root, true);
            }
        }

        [Test]
        public void TestNewItemContextMenuButtonDefersAutoClose()
        {
            var menu = new ContextMenu();
            var clicked = false;

            var button = ContentWindow.CreateDeferredNewItemButton(menu, "Json Asset", _ => clicked = true);

            Assert.IsFalse(button.CloseMenuOnClick);
            button.Click();
            Assert.IsTrue(clicked);
        }

        [Test]
        public void TestContentWorkspaceRefreshWaitsForRenamePopup()
        {
            Assert.IsFalse(ContentWindow.ShouldRefreshWorkspace(false, false));
            Assert.IsFalse(ContentWindow.ShouldRefreshWorkspace(true, true));
            Assert.IsTrue(ContentWindow.ShouldRefreshWorkspace(true, false));
        }

        [Test]
        public void TestContentRenameRejectsReentrantPopup()
        {
            Assert.IsTrue(ContentWindow.CanStartRename(false));
            Assert.IsFalse(ContentWindow.CanStartRename(true));
        }

        [Test]
        public void TestSceneActorsSidecarPathForStagedContentDelete()
        {
            var scenePath = Path.Combine(Globals.ProjectContentFolder, "Scenes", "Main.scene");
            var folderPath = Path.Combine(Globals.ProjectContentFolder, "Scenes");
            var filePath = Path.Combine(Globals.ProjectContentFolder, "Scenes", "Notes.txt");

            Assert.AreEqual(
                StringUtils.NormalizePath(Path.Combine(Globals.ProjectFolder, "SceneActors", "Scenes", "Main")),
                StringUtils.NormalizePath(ContentItemFilesystemAction.GetSceneActorsFolderPath(scenePath, false)));
            Assert.AreEqual(
                StringUtils.NormalizePath(Path.Combine(Globals.ProjectFolder, "SceneActors", "Scenes")),
                StringUtils.NormalizePath(ContentItemFilesystemAction.GetSceneActorsFolderPath(folderPath, true)));
            Assert.IsNull(ContentItemFilesystemAction.GetSceneActorsFolderPath(filePath, false));
        }

        [Test]
        public void TestExternalActorPathMapsToScenePath()
        {
            var actorId = Guid.NewGuid().ToString("N");
            var actorPath = Path.Combine(Globals.ProjectFolder, "SceneActors", "Scenes", "Main", "ExternalActors", actorId.Substring(0, 2), actorId + ".actor");

            Assert.IsTrue(SceneModule.TryGetScenePathFromExternalActorPath(actorPath, out var scenePath));
            Assert.AreEqual(
                StringUtils.NormalizePath(Path.Combine(Globals.ProjectContentFolder, "Scenes", "Main.scene")),
                StringUtils.NormalizePath(scenePath));
            Assert.IsFalse(SceneModule.TryGetScenePathFromExternalActorPath(Path.Combine(Globals.ProjectContentFolder, "Scenes", "Main.actor"), out _));
        }
    }
}
#endif
