// Copyright (c) Wojciech Figat. All rights reserved.

#if FLAX_TESTS
using System;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Linq;
using System.Runtime.InteropServices;
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

            Assert.IsTrue(AssetWorkspaceModule.UseContentBackendForFileOperation(sceneItem));
            Assert.IsFalse(AssetWorkspaceModule.UseContentBackendForFileOperation(fileItem));
            Assert.IsFalse(AssetWorkspaceModule.UseContentBackendForFileOperation(null));
        }

        [Test]
        public void TestCanonicalSourceCopyBackendClassification()
        {
            var textureId = Guid.NewGuid();
            var textureItem = new BinaryAssetItem("C:/Project/Content/Texture.jpg", ref textureId,
                typeof(Texture).FullName, typeof(Texture), ContentItemSearchFilter.Texture);
            textureItem.SetAssetDatabaseRecord(new AssetDatabaseRecordInfo
            {
                ID = textureId,
                SourceAssetID = textureId,
                SourcePath = textureItem.Path,
                MetaPath = textureItem.Path + ".meta",
                ProcessorID = "Flax.Texture",
                SourceKind = AssetSourceKind.ImportedSource,
                IsMain = true,
            });

            var prefabId = Guid.NewGuid();
            var prefabItem = new BinaryAssetItem("C:/Project/Content/Prefab.prefab", ref prefabId,
                typeof(Prefab).FullName, typeof(Prefab), ContentItemSearchFilter.Prefab);
            prefabItem.SetAssetDatabaseRecord(new AssetDatabaseRecordInfo
            {
                ID = prefabId,
                SourceAssetID = prefabId,
                SourcePath = prefabItem.Path,
                MetaPath = prefabItem.Path + ".meta",
                ProcessorID = "Flax.JsonDocument",
                SourceKind = AssetSourceKind.TextDocument,
                IsMain = true,
            });

            Assert.IsFalse(AssetWorkspaceModule.UseContentBackendForCopy(textureItem));
            Assert.IsFalse(AssetWorkspaceModule.UseContentBackendForCopy(prefabItem));
        }

        [Test]
        public void TestCanonicalSubAssetIsReferenceableButNotIndependentlyMutable()
        {
            var root = Path.Combine(Path.GetTempPath(), "FlaxCanonicalSubAssetTests", Guid.NewGuid().ToString("N"));
            Directory.CreateDirectory(root);
            try
            {
                var sourcePath = Path.Combine(root, "Character.gltf");
                var metadataPath = sourcePath + ".meta";
                File.WriteAllText(sourcePath, "{}");
                File.WriteAllText(metadataPath, "{}");
                var id = Guid.NewGuid();
                var item = new BinaryAssetItem(sourcePath + "." + id.ToString("N") + ".subasset", ref id,
                    typeof(Animation).FullName, typeof(Animation), ContentItemSearchFilter.Animation);
                item.SetAssetDatabaseRecord(new AssetDatabaseRecordInfo
                {
                    ID = id,
                    SourceAssetID = Guid.NewGuid(),
                    TypeName = typeof(Animation).FullName,
                    SourcePath = sourcePath,
                    MetaPath = metadataPath,
                    SubAssetKey = "animation:Walk",
                    ProcessorID = "Flax.Model",
                    SourceKind = AssetSourceKind.ImportedSource,
                    Status = AssetRecordStatus.Ready,
                    IsMain = false,
                });

                Assert.IsTrue(item.Exists);
                Assert.IsTrue(item.CanDrag);
                Assert.IsFalse(item.CanRename);
                Assert.IsTrue(item.IsCanonicalSubAsset);
                var result = new AssetWorkspaceModule(null).Copy(item, Path.Combine(root, "Walk.flax"));
                Assert.IsFalse(result.Succeeded);
                Assert.AreEqual(ContentMutationFailure.InvalidSource, result.Failure);
            }
            finally
            {
                Directory.Delete(root, true);
            }
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
                var database = new AssetWorkspaceModule(null);

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
                var database = new AssetWorkspaceModule(null);

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
        public void TestCanonicalMetadataGuidReaderUsesClonedRootIdentity()
        {
            var root = Path.Combine(Path.GetTempPath(), "FlaxCanonicalCopyTests", Guid.NewGuid().ToString("N"));
            Directory.CreateDirectory(root);
            try
            {
                var expected = Guid.NewGuid();
                var serializedId = FlaxEngine.Json.JsonSerializer.GetStringID(expected);
                var metadataPath = Path.Combine(root, "Artifact.prefab.meta");
                File.WriteAllText(metadataPath, "{\"metaVersion\":1,\"guid\":\"" + serializedId + "\"}");

                Assert.AreEqual(expected, AssetWorkspaceModule.ReadCanonicalMetadataGuid(metadataPath));
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
                var database = new AssetWorkspaceModule(null);

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
            var database = new AssetWorkspaceModule(null);

            Assert.IsFalse(database.ShouldRemoveMissingContentItem(newItem));
            Assert.IsTrue(database.ShouldRemoveMissingContentItem(fileItem));
            Assert.IsFalse(database.ShouldRemoveMissingContentItem(null));
        }

        [Test]
        public void TestContentFolderFindChildUsesCanonicalPathIdentity()
        {
            var root = Path.Combine(Path.GetTempPath(), "FlaxContentFolderTests", Guid.NewGuid().ToString("N"));
            var folder = new ContentFolder(ContentFolderType.Content, root, null);
            var child = new FileItem(Path.Combine(root, "Materials", "Asset.material"))
            {
                ParentFolder = folder,
            };
            var equivalentPath = Path.Combine(root, "Materials", ".", "Asset.material");
            if (RuntimeInformation.IsOSPlatform(OSPlatform.Windows))
                equivalentPath = equivalentPath.Replace('\\', '/').ToUpperInvariant();

            Assert.AreSame(child, folder.FindChild(equivalentPath));
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
        public void TestNestedAssetSaveRemainsActiveUntilOutermostScopeEnds()
        {
            var path = Path.Combine(Path.GetTempPath(), "FlaxContentSaveTests", Guid.NewGuid().ToString("N"), "Asset.flax");
            Directory.CreateDirectory(Path.GetDirectoryName(path));
            File.WriteAllText(path, "asset");
            try
            {
                var database = new AssetWorkspaceModule(null);

                database.BeginAssetSave(path);
                database.BeginAssetSave(path);
                Assert.IsTrue(database.IsAssetSaveInProgress(path));

                database.EndAssetSave(path, true);
                Assert.IsTrue(database.IsAssetSaveInProgress(path));

                database.EndAssetSave(path, true);
                Assert.IsFalse(database.IsAssetSaveInProgress(path));
            }
            finally
            {
                Directory.Delete(Path.GetDirectoryName(path), true);
            }
        }

        [Test]
        public void TestNestedAssetSaveFailurePreventsSuccessfulOuterCompletionFromMaskingIt()
        {
            var path = Path.Combine(Path.GetTempPath(), "FlaxContentSaveTests", Guid.NewGuid().ToString("N"), "Asset.flax");
            Directory.CreateDirectory(Path.GetDirectoryName(path));
            File.WriteAllText(path, "asset");
            try
            {
                var database = new AssetWorkspaceModule(null);

                database.BeginAssetSave(path);
                database.BeginAssetSave(path);
                database.EndAssetSave(path, false);
                Assert.IsTrue(database.IsAssetSaveInProgress(path));

                database.EndAssetSave(path, true);
                Assert.IsFalse(database.IsAssetSaveInProgress(path));
            }
            finally
            {
                Directory.Delete(Path.GetDirectoryName(path), true);
            }
        }

        [Test]
        public void TestContentMutationRootContainmentIsBoundaryAware()
        {
            var root = Path.Combine(Path.GetTempPath(), "Project", "Content");

            Assert.IsTrue(ContentMutationPathUtils.IsWithinRoot(Path.Combine(root, "Folder", "Asset.flax"), root));
            Assert.IsFalse(ContentMutationPathUtils.IsWithinRoot(Path.Combine(Path.GetDirectoryName(root), "ContentBackup", "Asset.flax"), root));
        }

        [Test]
        public void TestContentMutationPlanRejectsCycleBeforeMutation()
        {
            var root = Path.Combine(Path.GetTempPath(), "FlaxContentTransactionTests", Guid.NewGuid().ToString("N"));
            var source = Path.Combine(root, "Source");
            Directory.CreateDirectory(source);
            try
            {
                var destination = Path.Combine(source, "Nested");
                var plan = new ContentMutationPlan(ContentMutationOperationKind.Move);
                plan.Entries.Add(new ContentMutationEntry(source, destination, ContentMutationPathRole.Main, true));

                var result = plan.Preflight();

                Assert.IsFalse(result.Succeeded);
                Assert.AreEqual(ContentMutationFailure.PathCycle, result.Failure);
                Assert.IsTrue(Directory.Exists(source));
                Assert.IsFalse(Directory.Exists(destination));
            }
            finally
            {
                Directory.Delete(root, true);
            }
        }

        [Test]
        public void TestContentTransactionInjectedFailureRollsBackAndRemovesJournal()
        {
            var root = Path.Combine(Path.GetTempPath(), "FlaxContentTransactionTests", Guid.NewGuid().ToString("N"));
            var journalRoot = Path.Combine(root, "Journal");
            var source = Path.Combine(root, "Source.txt");
            var destination = Path.Combine(root, "Destination.txt");
            Directory.CreateDirectory(root);
            File.WriteAllText(source, "source");
            try
            {
                var plan = new ContentMutationPlan(ContentMutationOperationKind.Move);
                plan.Entries.Add(new ContentMutationEntry(source, destination, ContentMutationPathRole.Main, false));
                var transaction = new ContentMutationTransaction(plan, journalRoot);
                ContentMutationTransaction.FaultInjector = point => point == "after-main-move" ? new IOException("Injected failure") : null;

                var result = transaction.Execute(new[]
                {
                    new ContentMutationStep(
                        "main-move",
                        new[] { 0 },
                        () =>
                        {
                            File.Move(source, destination);
                            return ContentMutationResult.Success(source, destination);
                        },
                        () =>
                        {
                            if (File.Exists(destination) && !File.Exists(source))
                                File.Move(destination, source);
                            return File.Exists(source) && !File.Exists(destination);
                        })
                });

                Assert.IsFalse(result.Succeeded);
                Assert.IsFalse(result.RequiresRecovery);
                Assert.IsTrue(File.Exists(source));
                Assert.IsFalse(File.Exists(destination));
                Assert.IsFalse(Directory.Exists(journalRoot) && Directory.EnumerateFiles(journalRoot, "*.json").Any());
            }
            finally
            {
                ContentMutationTransaction.FaultInjector = null;
                Directory.Delete(root, true);
            }
        }

        [Test]
        public void TestContentTransactionStartupRecoveryRestoresInterruptedMove()
        {
            var root = Path.Combine(Path.GetTempPath(), "FlaxContentTransactionTests", Guid.NewGuid().ToString("N"));
            var journalRoot = Path.Combine(root, "Journal");
            var source = Path.Combine(root, "Source.txt");
            var destination = Path.Combine(root, "Destination.txt");
            Directory.CreateDirectory(root);
            File.WriteAllText(source, "source");
            try
            {
                var plan = new ContentMutationPlan(ContentMutationOperationKind.Move);
                plan.Entries.Add(new ContentMutationEntry(source, destination, ContentMutationPathRole.Main, false));
                var transaction = new ContentMutationTransaction(plan, journalRoot);
                ContentMutationTransaction.FaultInjector = point => point == "after-main-move" ? new IOException("Injected process interruption") : null;

                var result = transaction.Execute(new[]
                {
                    new ContentMutationStep(
                        "main-move",
                        new[] { 0 },
                        () =>
                        {
                            File.Move(source, destination);
                            return ContentMutationResult.Success(source, destination);
                        },
                        () => false)
                });

                Assert.IsFalse(result.Succeeded);
                Assert.IsTrue(result.RequiresRecovery);
                Assert.IsFalse(File.Exists(source));
                Assert.IsTrue(File.Exists(destination));
                Assert.IsTrue(Directory.EnumerateFiles(journalRoot, "*.json").Any());

                ContentMutationTransaction.FaultInjector = null;
                Assert.AreEqual(0, ContentMutationTransaction.RecoverPendingTransactions(journalRoot));
                Assert.IsTrue(File.Exists(source));
                Assert.IsFalse(File.Exists(destination));
                Assert.IsFalse(Directory.EnumerateFiles(journalRoot, "*.json").Any());
            }
            finally
            {
                ContentMutationTransaction.FaultInjector = null;
                Directory.Delete(root, true);
            }
        }

        [Test]
        public void TestContentTransactionRejectsSourceChangedAfterPreflight()
        {
            var root = Path.Combine(Path.GetTempPath(), "FlaxContentTransactionTests", Guid.NewGuid().ToString("N"));
            var journalRoot = Path.Combine(root, "Journal");
            var source = Path.Combine(root, "Source.txt");
            var destination = Path.Combine(root, "Destination.txt");
            Directory.CreateDirectory(root);
            File.WriteAllText(source, "source");
            try
            {
                var plan = new ContentMutationPlan(ContentMutationOperationKind.Move);
                plan.Entries.Add(new ContentMutationEntry(source, destination, ContentMutationPathRole.Main, false));
                var transaction = new ContentMutationTransaction(plan, journalRoot);
                ContentMutationTransaction.FaultInjector = point =>
                {
                    if (point == "before-main-move")
                        File.WriteAllText(source, "source changed after preflight");
                    return null;
                };

                var result = transaction.Execute(new[]
                {
                    new ContentMutationStep(
                        "main-move",
                        new[] { 0 },
                        () =>
                        {
                            File.Move(source, destination);
                            return ContentMutationResult.Success(source, destination);
                        },
                        () => File.Exists(source) && !File.Exists(destination))
                });

                Assert.IsFalse(result.Succeeded);
                Assert.AreEqual(ContentMutationFailure.VerificationFailure, result.Failure);
                Assert.AreEqual("source changed after preflight", File.ReadAllText(source));
                Assert.IsFalse(File.Exists(destination));
            }
            finally
            {
                ContentMutationTransaction.FaultInjector = null;
                Directory.Delete(root, true);
            }
        }

        [Test]
        public void TestContentTransactionStartupRecoverySkipsFolderDescendantEntries()
        {
            var root = Path.Combine(Path.GetTempPath(), "FlaxContentTransactionTests", Guid.NewGuid().ToString("N"));
            var journalRoot = Path.Combine(root, "Journal");
            var source = Path.Combine(root, "Source");
            var destination = Path.Combine(root, "Destination");
            var sourceChild = Path.Combine(source, "Child.txt");
            var destinationChild = Path.Combine(destination, "Child.txt");
            Directory.CreateDirectory(source);
            File.WriteAllText(sourceChild, "child");
            try
            {
                var plan = new ContentMutationPlan(ContentMutationOperationKind.Move);
                plan.Entries.Add(new ContentMutationEntry(source, destination, ContentMutationPathRole.Main, true));
                plan.Entries.Add(new ContentMutationEntry(sourceChild, destinationChild, ContentMutationPathRole.Descendant, false)
                {
                    DestinationParentProducedByTransaction = true,
                });
                var transaction = new ContentMutationTransaction(plan, journalRoot);
                ContentMutationTransaction.FaultInjector = point => point == "after-folder-move" ? new IOException("Injected process interruption") : null;

                var result = transaction.Execute(new[]
                {
                    new ContentMutationStep(
                        "folder-move",
                        new[] { 0, 1 },
                        () =>
                        {
                            Directory.Move(source, destination);
                            return ContentMutationResult.Success(source, destination);
                        },
                        () => false)
                });

                Assert.IsFalse(result.Succeeded);
                Assert.IsTrue(result.RequiresRecovery);
                ContentMutationTransaction.FaultInjector = null;
                Assert.AreEqual(0, ContentMutationTransaction.RecoverPendingTransactions(journalRoot));
                Assert.IsTrue(File.Exists(sourceChild));
                Assert.IsFalse(Directory.Exists(destination));
            }
            finally
            {
                ContentMutationTransaction.FaultInjector = null;
                Directory.Delete(root, true);
            }
        }

        [Test]
        public void TestContentTransactionStartupRecoveryRemovesInterruptedCopy()
        {
            var root = Path.Combine(Path.GetTempPath(), "FlaxContentTransactionTests", Guid.NewGuid().ToString("N"));
            var journalRoot = Path.Combine(root, "Journal");
            var source = Path.Combine(root, "Source.txt");
            var destination = Path.Combine(root, "Destination.txt");
            Directory.CreateDirectory(root);
            File.WriteAllText(source, "source");
            try
            {
                var plan = new ContentMutationPlan(ContentMutationOperationKind.Copy);
                plan.Entries.Add(new ContentMutationEntry(source, destination, ContentMutationPathRole.Main, false));
                var transaction = new ContentMutationTransaction(plan, journalRoot);
                ContentMutationTransaction.FaultInjector = point => point == "after-copy" ? new IOException("Injected process interruption") : null;

                var result = transaction.Execute(new[]
                {
                    new ContentMutationStep(
                        "copy",
                        new[] { 0 },
                        () =>
                        {
                            File.Copy(source, destination);
                            return ContentMutationResult.Success(source, destination);
                        },
                        () => false)
                });

                Assert.IsTrue(result.RequiresRecovery);
                ContentMutationTransaction.FaultInjector = null;
                Assert.AreEqual(0, ContentMutationTransaction.RecoverPendingTransactions(journalRoot));
                Assert.AreEqual("source", File.ReadAllText(source));
                Assert.IsFalse(File.Exists(destination));
            }
            finally
            {
                ContentMutationTransaction.FaultInjector = null;
                Directory.Delete(root, true);
            }
        }

        [Test]
        public void TestContentTransactionStartupRecoveryRestoresInterruptedReplacement()
        {
            var root = Path.Combine(Path.GetTempPath(), "FlaxContentTransactionTests", Guid.NewGuid().ToString("N"));
            var journalRoot = Path.Combine(root, "Journal");
            var backupRoot = Path.Combine(root, "Backups");
            var source = Path.Combine(root, "Import.txt");
            var destination = Path.Combine(root, "Asset.txt");
            var backup = Path.Combine(backupRoot, "Asset.txt");
            Directory.CreateDirectory(root);
            Directory.CreateDirectory(backupRoot);
            File.WriteAllText(source, "replacement");
            File.WriteAllText(destination, "original");
            try
            {
                var plan = new ContentMutationPlan(ContentMutationOperationKind.ImportOutput);
                plan.Entries.Add(new ContentMutationEntry(destination, backup, ContentMutationPathRole.ReplacementBackup, false));
                plan.Entries.Add(new ContentMutationEntry(source, destination, ContentMutationPathRole.Main, false)
                {
                    AllowExistingDestination = true,
                });
                var transaction = new ContentMutationTransaction(plan, journalRoot);
                ContentMutationTransaction.FaultInjector = point => point == "after-import-output" ? new IOException("Injected process interruption") : null;

                var result = transaction.Execute(new[]
                {
                    new ContentMutationStep(
                        "import-backup",
                        new[] { 0 },
                        () =>
                        {
                            File.Copy(destination, backup);
                            return ContentMutationResult.Success(destination, backup);
                        },
                        () => false),
                    new ContentMutationStep(
                        "import-output",
                        new[] { 1 },
                        () =>
                        {
                            File.Copy(source, destination, true);
                            return ContentMutationResult.Success(source, destination);
                        },
                        () => false)
                });

                Assert.IsTrue(result.RequiresRecovery);
                Assert.AreEqual("replacement", File.ReadAllText(destination));
                ContentMutationTransaction.FaultInjector = null;
                Assert.AreEqual(0, ContentMutationTransaction.RecoverPendingTransactions(journalRoot));
                Assert.AreEqual("original", File.ReadAllText(destination));
                Assert.IsFalse(File.Exists(backup));
            }
            finally
            {
                ContentMutationTransaction.FaultInjector = null;
                Directory.Delete(root, true);
            }
        }

        [Test]
        public void TestContentTransactionStartupRecoveryRebuildsMetadataOnlyImport()
        {
            var root = Path.Combine(Path.GetTempPath(), "FlaxContentTransactionTests", Guid.NewGuid().ToString("N"));
            var journalRoot = Path.Combine(root, "Journal");
            var source = Path.Combine(root, "Model.glb");
            var metadata = source + ".meta";
            Directory.CreateDirectory(root);
            File.WriteAllText(source, "source");
            try
            {
                var plan = new ContentMutationPlan(ContentMutationOperationKind.ImportOutput);
                plan.Entries.Add(new ContentMutationEntry(source, metadata, ContentMutationPathRole.MetadataSidecar, false));
                ContentMutationTransaction.FaultInjector = point => point == "after-register-in-place" ? new IOException("Injected process interruption") : null;
                var result = new ContentMutationTransaction(plan, journalRoot).Execute(new[]
                {
                    new ContentMutationStep("register-in-place", new[] { 0 }, () =>
                    {
                        File.WriteAllText(metadata, "metadata");
                        return ContentMutationResult.Success(source, metadata);
                    }, () => false, () => File.Exists(metadata))
                });

                Assert.IsTrue(result.RequiresRecovery);
                ContentMutationTransaction.FaultInjector = null;
                var recoveredSources = new List<string>();
                Assert.AreEqual(0, ContentMutationTransaction.RecoverPendingTransactions(recoveredSources, journalRoot));
                Assert.AreEqual(new[] { StringUtils.NormalizePath(source) }, recoveredSources);
                Assert.AreEqual("source", File.ReadAllText(source));
                Assert.AreEqual("metadata", File.ReadAllText(metadata));
                Assert.IsFalse(Directory.EnumerateFiles(journalRoot, "*.json").Any());
            }
            finally
            {
                ContentMutationTransaction.FaultInjector = null;
                Directory.Delete(root, true);
            }
        }

        [Test]
        public void TestContentTransactionStartupRecoveryCleansPartialMetadataBatch()
        {
            var root = Path.Combine(Path.GetTempPath(), "FlaxContentTransactionTests", Guid.NewGuid().ToString("N"));
            var journalRoot = Path.Combine(root, "Journal");
            var sourceA = Path.Combine(root, "A.glb");
            var sourceB = Path.Combine(root, "B.glb");
            var stagedA = Path.Combine(root, "A.staged.meta");
            var stagedB = Path.Combine(root, "B.staged.meta");
            Directory.CreateDirectory(root);
            File.WriteAllText(sourceA, "a");
            File.WriteAllText(sourceB, "b");
            File.WriteAllText(stagedA, "meta-a");
            File.WriteAllText(stagedB, "meta-b");
            try
            {
                var plan = new ContentMutationPlan(ContentMutationOperationKind.ImportOutput);
                plan.Entries.Add(new ContentMutationEntry(stagedA, sourceA + ".meta", ContentMutationPathRole.MetadataSidecar, false));
                plan.Entries.Add(new ContentMutationEntry(stagedB, sourceB + ".meta", ContentMutationPathRole.MetadataSidecar, false));
                ContentMutationTransaction.FaultInjector = point => point == "after-commit-a" ? new IOException("Injected process interruption") : null;
                var result = new ContentMutationTransaction(plan, journalRoot).Execute(new[]
                {
                    new ContentMutationStep("commit-a", new[] { 0 }, () =>
                    {
                        File.Move(stagedA, sourceA + ".meta");
                        return ContentMutationResult.Success(stagedA, sourceA + ".meta");
                    }, () => false),
                    new ContentMutationStep("commit-b", new[] { 1 }, () => ContentMutationResult.Success(stagedB, sourceB + ".meta"), () => false)
                });

                Assert.IsTrue(result.RequiresRecovery);
                ContentMutationTransaction.FaultInjector = null;
                var recoveredSources = new List<string>();
                Assert.AreEqual(0, ContentMutationTransaction.RecoverPendingTransactions(recoveredSources, journalRoot));
                Assert.AreEqual(new[] { StringUtils.NormalizePath(sourceA) }, recoveredSources);
                Assert.IsTrue(File.Exists(sourceA + ".meta"));
                Assert.IsFalse(File.Exists(stagedA));
                Assert.IsFalse(File.Exists(stagedB));
            }
            finally
            {
                ContentMutationTransaction.FaultInjector = null;
                Directory.Delete(root, true);
            }
        }

        [Test]
        public void TestContentTransactionStartupRecoveryRemovesInterruptedImportSidecar()
        {
            var root = Path.Combine(Path.GetTempPath(), "FlaxContentTransactionTests", Guid.NewGuid().ToString("N"));
            var journalRoot = Path.Combine(root, "Journal");
            var source = Path.Combine(root, "Source.glb");
            var output = Path.Combine(root, "Imported.glb");
            var metadata = output + ".meta";
            Directory.CreateDirectory(root);
            File.WriteAllText(source, "source");
            try
            {
                var plan = new ContentMutationPlan(ContentMutationOperationKind.ImportOutput);
                plan.Entries.Add(new ContentMutationEntry(source, output, ContentMutationPathRole.Main, false));
                plan.Entries.Add(new ContentMutationEntry(output, metadata, ContentMutationPathRole.MetadataSidecar, false) { SourceProducedByTransaction = true });
                ContentMutationTransaction.FaultInjector = point => point == "after-metadata" ? new IOException("Injected process interruption") : null;
                var result = new ContentMutationTransaction(plan, journalRoot).Execute(new[]
                {
                    new ContentMutationStep("output", new[] { 0 }, () =>
                    {
                        File.Copy(source, output);
                        return ContentMutationResult.Success(source, output);
                    }, () => false),
                    new ContentMutationStep("metadata", new[] { 1 }, () =>
                    {
                        File.WriteAllText(metadata, "metadata");
                        return ContentMutationResult.Success(output, metadata);
                    }, () => false)
                });

                Assert.IsTrue(result.RequiresRecovery);
                ContentMutationTransaction.FaultInjector = null;
                Assert.AreEqual(0, ContentMutationTransaction.RecoverPendingTransactions(journalRoot));
                Assert.AreEqual("source", File.ReadAllText(source));
                Assert.IsFalse(File.Exists(output));
                Assert.IsFalse(File.Exists(metadata));
            }
            finally
            {
                ContentMutationTransaction.FaultInjector = null;
                Directory.Delete(root, true);
            }
        }

        [Test]
        public void TestContentTransactionStartupRecoveryCompletesCleanupJournal()
        {
            var root = Path.Combine(Path.GetTempPath(), "FlaxContentTransactionTests", Guid.NewGuid().ToString("N"));
            var journalRoot = Path.Combine(root, "Journal");
            var staged = Path.Combine(root, "Trash", "Asset.flax");
            var original = Path.Combine(root, "Content", "Asset.flax");
            Directory.CreateDirectory(Path.GetDirectoryName(staged));
            Directory.CreateDirectory(Path.GetDirectoryName(original));
            File.WriteAllText(staged, "staged");
            File.WriteAllText(original, "current");
            try
            {
                var plan = new ContentMutationPlan(ContentMutationOperationKind.Cleanup);
                plan.Entries.Add(new ContentMutationEntry(staged, original, ContentMutationPathRole.UndoTrash, false));
                Assert.IsNotNull(ContentMutationTransaction.PreserveRecoveryRecord(plan, "Injected cleanup failure.", journalRoot));

                Assert.AreEqual(0, ContentMutationTransaction.RecoverPendingTransactions(journalRoot));
                Assert.IsFalse(File.Exists(staged));
                Assert.AreEqual("current", File.ReadAllText(original));
                Assert.IsFalse(Directory.EnumerateFiles(journalRoot, "*.json").Any());
            }
            finally
            {
                Directory.Delete(root, true);
            }
        }

        [Test]
        public void TestContentCreateTransactionDoesNotRequireSourcePath()
        {
            var root = Path.Combine(Path.GetTempPath(), "FlaxContentTransactionTests", Guid.NewGuid().ToString("N"));
            var journalRoot = Path.Combine(root, "Journal");
            var destination = Path.Combine(root, "Created.txt");
            Directory.CreateDirectory(root);
            try
            {
                var plan = new ContentMutationPlan(ContentMutationOperationKind.Create);
                plan.Entries.Add(new ContentMutationEntry(destination, destination, ContentMutationPathRole.Main, false)
                {
                    SourceRequired = false,
                });
                var result = new ContentMutationTransaction(plan, journalRoot).Execute(new[]
                {
                    new ContentMutationStep("create", new[] { 0 }, () =>
                    {
                        File.WriteAllText(destination, "created");
                        return ContentMutationResult.Success(null, destination);
                    }, () =>
                    {
                        if (File.Exists(destination)) File.Delete(destination);
                        return !File.Exists(destination);
                    }, () => File.Exists(destination))
                });

                Assert.IsTrue(result.Succeeded);
                Assert.AreEqual("created", File.ReadAllText(destination));
                Assert.IsFalse(Directory.Exists(journalRoot) && Directory.EnumerateFiles(journalRoot, "*.json").Any());
            }
            finally
            {
                Directory.Delete(root, true);
            }
        }

        [Test]
        public void TestContentTransactionFragmentFailureRollsBackMainPath()
        {
            var root = Path.Combine(Path.GetTempPath(), "FlaxContentTransactionTests", Guid.NewGuid().ToString("N"));
            var journalRoot = Path.Combine(root, "Journal");
            var source = Path.Combine(root, "Scene.scene");
            var destination = Path.Combine(root, "Moved.scene");
            var sourceSidecar = Path.Combine(root, "ExternalActors", Guid.NewGuid().ToString("N"));
            var destinationSidecar = Path.Combine(root, "Trash", "ExternalActors", Path.GetFileName(sourceSidecar));
            Directory.CreateDirectory(sourceSidecar);
            File.WriteAllText(source, "scene");
            File.WriteAllText(Path.Combine(sourceSidecar, "Actor.sceneactor"), "actor");
            try
            {
                var plan = new ContentMutationPlan(ContentMutationOperationKind.Move);
                plan.Entries.Add(new ContentMutationEntry(source, destination, ContentMutationPathRole.Main, false));
                plan.Entries.Add(new ContentMutationEntry(sourceSidecar, destinationSidecar, ContentMutationPathRole.SceneFragments, true));
                var result = new ContentMutationTransaction(plan, journalRoot).Execute(new[]
                {
                    new ContentMutationStep("main", new[] { 0 }, () =>
                    {
                        File.Move(source, destination);
                        return ContentMutationResult.Success(source, destination);
                    }, () =>
                    {
                        if (File.Exists(destination) && !File.Exists(source)) File.Move(destination, source);
                        return File.Exists(source) && !File.Exists(destination);
                    }),
                    new ContentMutationStep("fragments", new[] { 1 },
                        () => ContentMutationResult.Fail(ContentMutationFailure.MoveFailed, sourceSidecar, destinationSidecar, "Injected sidecar failure."),
                        () => Directory.Exists(sourceSidecar) && !Directory.Exists(destinationSidecar))
                });

                Assert.IsFalse(result.Succeeded);
                Assert.IsFalse(result.RequiresRecovery);
                Assert.IsTrue(File.Exists(source));
                Assert.IsFalse(File.Exists(destination));
                Assert.IsTrue(File.Exists(Path.Combine(sourceSidecar, "Actor.sceneactor")));
                Assert.IsFalse(Directory.Exists(destinationSidecar));
            }
            finally
            {
                Directory.Delete(root, true);
            }
        }

        [Test]
        public void TestContentMutationRejectsReservedWindowsDestinationName()
        {
            if (!RuntimeInformation.IsOSPlatform(OSPlatform.Windows))
                Assert.Pass();
            var root = Path.Combine(Path.GetTempPath(), "FlaxContentTransactionTests", Guid.NewGuid().ToString("N"));
            var source = Path.Combine(root, "Source.txt");
            Directory.CreateDirectory(root);
            File.WriteAllText(source, "source");
            try
            {
                var plan = new ContentMutationPlan(ContentMutationOperationKind.Copy);
                plan.Entries.Add(new ContentMutationEntry(source, Path.Combine(root, "CON.txt"), ContentMutationPathRole.Main, false));
                var result = plan.Preflight();
                Assert.IsFalse(result.Succeeded);
                Assert.AreEqual(ContentMutationFailure.InvalidDestination, result.Failure);
            }
            finally
            {
                Directory.Delete(root, true);
            }
        }

        [Test]
        public void TestContentMutationRandomizedFilesystemModel()
        {
            const int seed = 0x51ab1e;
            const int operationCount = 300;
            var random = new Random(seed);
            var root = Path.Combine(Path.GetTempPath(), "FlaxContentModelTests", Guid.NewGuid().ToString("N"));
            var contentRoot = Path.Combine(root, "Content");
            var trashRoot = Path.Combine(root, "Trash");
            var journalRoot = Path.Combine(root, "Journal");
            Directory.CreateDirectory(contentRoot);
            Directory.CreateDirectory(trashRoot);
            var model = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);
            try
            {
                for (int i = 0; i < 4; i++)
                {
                    var name = $"Asset{i}.txt";
                    var data = $"seed-{seed}-asset-{i}";
                    File.WriteAllText(Path.Combine(contentRoot, name), data);
                    model.Add(name, data);
                }

                for (int operation = 0; operation < operationCount; operation++)
                {
                    var keys = model.Keys.OrderBy(x => x, StringComparer.OrdinalIgnoreCase).ToArray();
                    var kind = keys.Length < 2 ? 0 : random.Next(3);
                    var sourceName = keys[random.Next(keys.Length)];
                    var source = Path.Combine(contentRoot, sourceName);
                    var destinationName = $"Asset{operation + 100}.txt";
                    var destination = Path.Combine(contentRoot, destinationName);
                    var injectFailure = operation % 19 == 7;
                    ContentMutationResult result;

                    if (kind == 0)
                    {
                        var plan = new ContentMutationPlan(ContentMutationOperationKind.Copy);
                        plan.Entries.Add(new ContentMutationEntry(source, destination, ContentMutationPathRole.Main, false));
                        ContentMutationTransaction.FaultInjector = point => injectFailure && point == "after-copy" ? new IOException("seeded fault") : null;
                        result = new ContentMutationTransaction(plan, journalRoot).Execute(new[]
                        {
                            new ContentMutationStep("copy", new[] { 0 }, () =>
                            {
                                File.Copy(source, destination);
                                return ContentMutationResult.Success(source, destination);
                            }, () =>
                            {
                                if (File.Exists(destination)) File.Delete(destination);
                                return !File.Exists(destination);
                            })
                        });
                        if (result.Succeeded)
                            model.Add(destinationName, model[sourceName]);
                    }
                    else if (kind == 1)
                    {
                        var plan = new ContentMutationPlan(ContentMutationOperationKind.Move);
                        plan.Entries.Add(new ContentMutationEntry(source, destination, ContentMutationPathRole.Main, false));
                        ContentMutationTransaction.FaultInjector = point => injectFailure && point == "after-move" ? new IOException("seeded fault") : null;
                        result = new ContentMutationTransaction(plan, journalRoot).Execute(new[]
                        {
                            new ContentMutationStep("move", new[] { 0 }, () =>
                            {
                                File.Move(source, destination);
                                return ContentMutationResult.Success(source, destination);
                            }, () =>
                            {
                                if (File.Exists(destination) && !File.Exists(source)) File.Move(destination, source);
                                return File.Exists(source) && !File.Exists(destination);
                            })
                        });
                        if (result.Succeeded)
                        {
                            var data = model[sourceName];
                            model.Remove(sourceName);
                            model.Add(destinationName, data);
                        }
                    }
                    else
                    {
                        var trash = Path.Combine(trashRoot, $"{operation}-{sourceName}");
                        var plan = new ContentMutationPlan(ContentMutationOperationKind.Delete);
                        plan.Entries.Add(new ContentMutationEntry(source, trash, ContentMutationPathRole.UndoTrash, false));
                        ContentMutationTransaction.FaultInjector = point => injectFailure && point == "after-delete-stage" ? new IOException("seeded fault") : null;
                        result = new ContentMutationTransaction(plan, journalRoot).Execute(new[]
                        {
                            new ContentMutationStep("delete-stage", new[] { 0 }, () =>
                            {
                                File.Move(source, trash);
                                return ContentMutationResult.Success(source, trash);
                            }, () =>
                            {
                                if (File.Exists(trash) && !File.Exists(source)) File.Move(trash, source);
                                return File.Exists(source) && !File.Exists(trash);
                            })
                        });
                        if (result.Succeeded)
                        {
                            model.Remove(sourceName);
                            File.Delete(trash);
                        }
                    }

                    ContentMutationTransaction.FaultInjector = null;
                    Assert.AreEqual(0, Directory.Exists(journalRoot) ? Directory.EnumerateFiles(journalRoot, "*.json").Count() : 0, $"Journal leak at seed {seed}, operation {operation}.");
                    var actual = Directory.EnumerateFiles(contentRoot, "*.txt").ToDictionary(Path.GetFileName, File.ReadAllText, StringComparer.OrdinalIgnoreCase);
                    Assert.AreEqual(model.Count, actual.Count, $"Path count divergence at seed {seed}, operation {operation}.");
                    foreach (var pair in model)
                        Assert.AreEqual(pair.Value, actual[pair.Key], $"Data divergence at seed {seed}, operation {operation}, path {pair.Key}.");
                }
            }
            finally
            {
                ContentMutationTransaction.FaultInjector = null;
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
        public void TestRenamePopupIgnoresPostDisposalUpdateAndKeepsTextSnapshot()
        {
            Assert.AreEqual("CommittedName", FlaxEditor.GUI.RenamePopup.ResolveText(null, "CommittedName"));
            Assert.IsFalse(FlaxEditor.GUI.RenamePopup.ShouldProcessUpdate(false, false));
            Assert.IsFalse(FlaxEditor.GUI.RenamePopup.ShouldProcessUpdate(true, true));
            Assert.IsTrue(FlaxEditor.GUI.RenamePopup.ShouldProcessUpdate(false, true));
        }

        [Test]
        public void TestSceneFragmentsPathForStagedContentDelete()
        {
            var sceneId = Guid.NewGuid();

            Assert.AreEqual(
                StringUtils.NormalizePath(Path.Combine(Globals.ProjectFolder, "ExternalActors", sceneId.ToString("N").ToLowerInvariant())),
                StringUtils.NormalizePath(ContentItemFilesystemAction.GetSceneFragmentsFolderPath(sceneId)));
            Assert.IsNull(ContentItemFilesystemAction.GetSceneFragmentsFolderPath(Guid.Empty));
        }

        [Test]
        public void TestMetadataSidecarPathForStagedContentDelete()
        {
            var filePath = Path.Combine(Globals.ProjectContentFolder, "Notes.txt");

            Assert.AreEqual(
                StringUtils.NormalizePath(filePath + ".meta"),
                ContentItemFilesystemAction.GetMetadataSidecarPath(filePath, false));
            Assert.IsNull(ContentItemFilesystemAction.GetMetadataSidecarPath(filePath, true));
        }

        [Test]
        public void TestCanonicalTextUsesFileProxy()
        {
            var id = Guid.NewGuid();
            var path = Path.Combine(Globals.ProjectContentFolder, "Notes.txt");
            var item = new BinaryAssetItem(path, ref id, typeof(RawDataAsset).FullName, typeof(RawDataAsset), ContentItemSearchFilter.Other);
            item.SetAssetDatabaseRecord(new AssetDatabaseRecordInfo
            {
                ID = id,
                SourceAssetID = id,
                TypeName = typeof(RawDataAsset).FullName,
                SourcePath = path,
                MetaPath = path + ".meta",
                ProcessorID = "Flax.Text",
                SourceKind = AssetSourceKind.TextDocument,
                Status = AssetRecordStatus.Ready,
                IsMain = true,
            });

            Assert.IsTrue(new FileProxy().IsProxyFor(item));
        }

    }
}
#endif
