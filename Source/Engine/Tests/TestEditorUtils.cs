// Copyright (c) Wojciech Figat. All rights reserved.

#if FLAX_TESTS
using System;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Linq;
using System.Runtime.InteropServices;
using System.Threading;
using FlaxEditor.Actions;
using FlaxEditor.Content;
using FlaxEditor.Content.Documents;
using FlaxEditor.Content.Import;
using FlaxEditor.GUI.ContextMenu;
using FlaxEditor.Modules;
using FlaxEditor.Surface;
using FlaxEditor.Windows;
using FlaxEditor.Windows.Assets;
using NUnit.Framework;
using Newtonsoft.Json.Linq;

namespace FlaxEngine.Tests
{
    /// <summary>
    /// Tests for <see cref="FlaxEditor.Utilities.Utils"/>.
    /// </summary>
    [TestFixture]
    public class TestEditorUtils
    {
        private sealed class ThumbnailOwner : IContentItemOwner
        {
            public void OnItemDeleted(ContentItem item) { }
            public void OnItemRenamed(ContentItem item) { }
            public void OnItemReimported(ContentItem item) { }
            public void OnItemDispose(ContentItem item) { }
        }

        private sealed class ParticleSurfaceOwner : IVisjectSurfaceOwner
        {
            public Asset SurfaceAsset => null;
            public string SurfaceName => "Particle Emitter test";
            public FlaxEditor.Undo Undo => null;
            public byte[] SurfaceData { get; set; }
            public VisjectSurfaceContext ParentContext => null;
            public void OnContextCreated(VisjectSurfaceContext context) { }
            public void OnSurfaceEditedChanged() { }
            public void OnSurfaceGraphEdited() { }
            public void OnSurfaceClose() { }
        }

        private static ContentFolder CreateCanonicalTextFolder(string folderPath, out string assetPath, out Guid assetId)
        {
            Directory.CreateDirectory(folderPath);
            assetPath = Path.Combine(folderPath, "Canonical.txt");
            var folder = new ContentFolder(ContentFolderType.Content, folderPath, null);
            CreateCanonicalTextItem(assetPath, folder, out assetId);
            return folder;
        }

        private static BinaryAssetItem CreateCanonicalTextItem(string assetPath, ContentFolder folder, out Guid assetId)
        {
            File.WriteAllText(assetPath, "canonical source");
            assetId = AssetOperationService.CreateImportedSourceMetadata(assetPath, typeof(RawDataAsset).FullName, "Flax.Text");
            Assert.AreNotEqual(Guid.Empty, assetId);
            Assert.IsTrue(AssetDatabaseQueryService.TryGetRecord(assetId, out var record));

            var item = new BinaryAssetItem(assetPath, ref assetId, typeof(RawDataAsset).FullName,
                typeof(RawDataAsset), ContentItemSearchFilter.Other);
            item.SetAssetDatabaseRecord(record);
            item.ParentFolder = folder;
            return item;
        }

        private static void CleanupCanonicalCopyAsset(string path)
        {
            if (!File.Exists(path))
                return;
            if (AssetDatabaseQueryService.TryGetMainRecordAtPath(path, out _))
                AssetOperationService.DeleteAsset(path);
            else
            {
                File.Delete(path);
                File.Delete(path + ".meta");
            }
        }

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
        public void TestContentImporterCoordinatorDoesNotPreemptEditorWork()
        {
            Assert.AreEqual(ThreadPriority.BelowNormal, ContentImportingModule.WorkerThreadPriority);
        }

        [Test]
        public void TestContentImporterOnlyOverlapsIndependentCanonicalSources()
        {
            Assert.AreEqual(2, ContentImportingModule.MaxConcurrentCanonicalImports);
            var firstRequest = new Request
            {
                InputPath = "C:/External/First.glb",
                OutputPath = "C:/Project/Content/First.glb",
                UseCanonicalSource = true,
            };
            var secondRequest = new Request
            {
                InputPath = "C:/External/Second.glb",
                OutputPath = "C:/Project/Content/Second.glb",
                UseCanonicalSource = true,
            };
            var first = new ModelImportEntry(ref firstRequest);
            var second = new ModelImportEntry(ref secondRequest);

            Assert.IsTrue(ContentImportingModule.CanExecuteCanonicalImportsConcurrently(first, second));

            secondRequest.InputPath = firstRequest.InputPath;
            second = new ModelImportEntry(ref secondRequest);
            Assert.IsFalse(ContentImportingModule.CanExecuteCanonicalImportsConcurrently(first, second));

            secondRequest.InputPath = "C:/External/Second.glb";
            secondRequest.AllowReplace = true;
            second = new ModelImportEntry(ref secondRequest);
            Assert.IsFalse(ContentImportingModule.CanExecuteCanonicalImportsConcurrently(first, second));
        }

        [Test]
        public void TestContentImporterExecutesCanonicalPairConcurrentlyAtLowPriority()
        {
            using var entered = new CountdownEvent(ContentImportingModule.MaxConcurrentCanonicalImports);
            var priorities = new ThreadPriority[ContentImportingModule.MaxConcurrentCanonicalImports];

            ContentImportingModule.ExecuteCanonicalImportPair(index =>
            {
                priorities[index] = Thread.CurrentThread.Priority;
                entered.Signal();
                Assert.IsTrue(entered.Wait(TimeSpan.FromSeconds(5)), "Canonical import work did not overlap.");
            });

            Assert.AreEqual(ThreadPriority.BelowNormal, priorities[1]);
        }

        public static int RunConcurrentCanonicalImportCoordinatorTests()
        {
            var tests = new TestEditorUtils();
            tests.TestContentImporterOnlyOverlapsIndependentCanonicalSources();
            tests.TestContentImporterExecutesCanonicalPairConcurrentlyAtLowPriority();
            return 0;
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
        public void TestSceneAndPrefabItemsExposeConcreteProjectTypes()
        {
            var sceneItem = new SceneItem("C:/Project/Content/Scene.scene", Guid.NewGuid());
            var prefabItem = new PrefabItem("C:/Project/Content/Actor.prefab", Guid.NewGuid());

            Assert.AreEqual(ContentItemType.Scene, sceneItem.ItemType);
            Assert.AreEqual(ContentItemSearchFilter.Scene, sceneItem.SearchFilter);
            Assert.AreEqual("Scene", sceneItem.TypeDescription);
            Assert.IsTrue(sceneItem.IsOfType(typeof(SceneAsset)));
            Assert.IsFalse(sceneItem.IsOfType(typeof(Prefab)));

            Assert.AreEqual(ContentItemType.Asset, prefabItem.ItemType);
            Assert.AreEqual(ContentItemSearchFilter.Prefab, prefabItem.SearchFilter);
            Assert.IsTrue(prefabItem.IsOfType(typeof(Prefab)));
            Assert.IsFalse(prefabItem.IsOfType(typeof(SceneAsset)));
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
        public void TestMultiCopyRoutesCanonicalSourcesThroughNativeBatch()
        {
            Assert.AreEqual(0, RunMultiCopyRoutesCanonicalSourcesThroughNativeBatch());
        }

        public static int RunMultiCopyRoutesCanonicalSourcesThroughNativeBatch()
        {
            var root = Path.Combine(Globals.ProjectContentFolder, "__CanonicalBatchCopy_" + Guid.NewGuid().ToString("N"));
            var firstSource = Path.Combine(root, "First.txt");
            var secondSource = Path.Combine(root, "Second.txt");
            var firstDestination = Path.Combine(root, "First Copy.txt");
            var secondDestination = Path.Combine(root, "Second Copy.txt");
            int nativeCopies = 0;
            try
            {
                Directory.CreateDirectory(root);
                var folder = new ContentFolder(ContentFolderType.Content, root, null);
                var first = CreateCanonicalTextItem(firstSource, folder, out _);
                var second = CreateCanonicalTextItem(secondSource, folder, out _);
                AssetWorkspaceModule.CanonicalCopyObserver = (_, _) => nativeCopies++;

                var result = new AssetWorkspaceModule(null).Copy(new[]
                {
                    (Item: (ContentItem)first, Destination: firstDestination),
                    (Item: (ContentItem)second, Destination: secondDestination),
                });

                if (!result.Succeeded)
                    return 1;
                if (nativeCopies != 2)
                    return 2;
                if (result.CompletedPaths.Length != 2)
                    return 3;
                if (!result.CompletedPaths.Any(x => string.Equals(Path.GetFullPath(x), Path.GetFullPath(firstDestination),
                        StringComparison.OrdinalIgnoreCase)) ||
                    !result.CompletedPaths.Any(x => string.Equals(Path.GetFullPath(x), Path.GetFullPath(secondDestination),
                        StringComparison.OrdinalIgnoreCase)))
                    return 4;
                return 0;
            }
            finally
            {
                AssetWorkspaceModule.CanonicalCopyObserver = null;
                CleanupCanonicalCopyAsset(firstDestination);
                CleanupCanonicalCopyAsset(secondDestination);
                CleanupCanonicalCopyAsset(firstSource);
                CleanupCanonicalCopyAsset(secondSource);
                if (Directory.Exists(root))
                    Directory.Delete(root, true);
                AssetPipelineService.RefreshSources(new[] { root });
            }
        }

        [Test]
        public void TestSingleCanonicalPairMutationRoutes()
        {
            var token = Guid.NewGuid().ToString("N");
            var sourcePath = Path.Combine(Globals.ProjectContentFolder, "__SinglePairSource_" + token + ".txt");
            var projectCopyPath = Path.Combine(Globals.ProjectContentFolder, "__SinglePairProjectCopy_" + token + ".txt");
            var projectMovePath = Path.Combine(Globals.ProjectContentFolder, "__SinglePairProjectMove_" + token + ".txt");
            var apiCopyPath = Path.Combine(Globals.ProjectContentFolder, "__SinglePairApiCopy_" + token + ".txt");
            var apiMovePath = Path.Combine(Globals.ProjectContentFolder, "__SinglePairApiMove_" + token + ".txt");
            ContentItemFilesystemAction deleteAction = null;
            int nativeProjectMoves = 0;
            var lifecycleStage = "setup";
            var lifecycleTimer = System.Diagnostics.Stopwatch.StartNew();
            try
            {
                var workspace = FlaxEditor.Editor.Instance.ContentDatabase;
                var contentFolder = workspace.Find(Globals.ProjectContentFolder) as ContentFolder;
                Assert.NotNull(contentFolder, "Project Content root was not indexed.");
                var sourceItem = CreateCanonicalTextItem(sourcePath, contentFolder, out var sourceId);

                lifecycleStage = "Project copy";
                var copyResult = workspace.Copy(sourceItem, projectCopyPath);
                Assert.IsTrue(copyResult.Succeeded, copyResult.Message);
                Assert.IsTrue(File.Exists(projectCopyPath), "Project copy source output is missing.");
                Assert.IsTrue(File.Exists(projectCopyPath + ".meta"), "Project copy metadata output is missing.");
                Assert.IsFalse(AssetPipelineService.RefreshSources(new[] { projectCopyPath }),
                    "Project copy refresh failed before database verification.");
                Assert.IsTrue(AssetDatabaseQueryService.TryGetMainRecordAtPath(projectCopyPath, out var projectCopyRecord),
                    "Project copy was not published to the asset database.");
                Assert.AreNotEqual(sourceId, projectCopyRecord.SourceAssetID);
                Assert.AreEqual(projectCopyRecord.SourceAssetID, AssetDatabaseQueryService.AssetPathToGUID(projectCopyPath),
                    "Project copy path did not resolve to its persistent source GUID.");

                lifecycleStage = "Project move";
                var projectCopyItem = workspace.FindAsset(projectCopyRecord.SourceAssetID);
                Assert.NotNull(projectCopyItem, "Project copy item was not presented in the workspace.");
                AssetWorkspaceModule.CanonicalMoveObserver = (source, destination) =>
                {
                    Assert.AreEqual(Path.GetFullPath(projectCopyPath), Path.GetFullPath(source));
                    Assert.AreEqual(Path.GetFullPath(projectMovePath), Path.GetFullPath(destination));
                    nativeProjectMoves++;
                };
                var moveResult = workspace.TryMove(new[] { (Item: (ContentItem)projectCopyItem, Destination: projectMovePath) });
                Assert.IsTrue(moveResult.Succeeded, moveResult.Message);
                Assert.AreEqual(1, nativeProjectMoves, "Project move did not enter native authority exactly once.");
                Assert.IsFalse(File.Exists(projectCopyPath), "Project move left its old source behind.");
                Assert.IsFalse(File.Exists(projectCopyPath + ".meta"), "Project move left its old metadata behind.");
                Assert.IsTrue(File.Exists(projectMovePath), "Project move source output is missing.");
                Assert.IsTrue(File.Exists(projectMovePath + ".meta"), "Project move metadata output is missing.");
                Assert.IsFalse(AssetPipelineService.RefreshSources(new[] { projectCopyPath, projectMovePath }),
                    "Project move refresh failed before database verification.");
                Assert.IsTrue(AssetDatabaseQueryService.TryGetMainRecordAtPath(projectMovePath, out var projectMoveRecord),
                    "Project move was not published to the asset database.");
                Assert.AreEqual(projectCopyRecord.SourceAssetID, projectMoveRecord.SourceAssetID);

                lifecycleStage = "Project case-only move";
                var projectCasePath = Path.Combine(Path.GetDirectoryName(projectMovePath),
                    Path.GetFileName(projectMovePath).ToLowerInvariant());
                AssetWorkspaceModule.CanonicalMoveObserver = (source, destination) =>
                {
                    Assert.AreEqual(Path.GetFullPath(projectMovePath), Path.GetFullPath(source));
                    Assert.AreEqual(Path.GetFullPath(projectCasePath), Path.GetFullPath(destination));
                };
                var caseMoveResult = workspace.TryMove(new[] { (Item: (ContentItem)projectCopyItem, Destination: projectCasePath) });
                Assert.IsTrue(caseMoveResult.Succeeded, caseMoveResult.Message);
                projectMovePath = projectCasePath;
                Assert.AreEqual(Path.GetFullPath(projectMovePath), Path.GetFullPath(projectCopyItem.Path));
                Assert.IsFalse(AssetPipelineService.RefreshSources(new[] { projectMovePath }),
                    "Project case-only move refresh failed before database verification.");
                Assert.IsTrue(AssetDatabaseQueryService.TryGetMainRecordAtPath(projectMovePath, out var caseMoveRecord));
                Assert.AreEqual(projectMoveRecord.SourceAssetID, caseMoveRecord.SourceAssetID);
                Assert.AreEqual(Path.GetFullPath(projectMovePath), Path.GetFullPath(caseMoveRecord.SourcePath));
                projectMoveRecord = caseMoveRecord;
                var contentWindow = FlaxEditor.Editor.Instance.Windows.ContentWin;
                Assert.AreEqual(1, contentWindow.Selection.Count);
                Assert.AreEqual(Path.GetFullPath(projectMovePath), Path.GetFullPath(contentWindow.Selection[0].Path));

                lifecycleStage = "Project delete";
                var projectMoveItem = workspace.FindAsset(projectMoveRecord.SourceAssetID);
                Assert.NotNull(projectMoveItem, "Project move item was not presented in the workspace.");
                deleteAction = ContentItemFilesystemAction.Delete(FlaxEditor.Editor.Instance,
                    new List<ContentItem> { projectMoveItem });
                Assert.NotNull(deleteAction, "Project delete did not create recoverable undo state.");
                Assert.IsFalse(File.Exists(projectMovePath), "Project delete left its source behind.");
                Assert.IsFalse(File.Exists(projectMovePath + ".meta"), "Project delete left its metadata behind.");
                Assert.IsFalse(AssetPipelineService.RefreshSources(new[] { projectMovePath }),
                    "Project delete refresh failed before restore.");
                lifecycleStage = "Project restore";
                Assert.IsTrue(deleteAction.TryUndo(), "Project delete restore failed.");
                Assert.IsTrue(File.Exists(projectMovePath), "Project restore source output is missing.");
                Assert.IsTrue(File.Exists(projectMovePath + ".meta"), "Project restore metadata output is missing.");
                Assert.IsFalse(AssetPipelineService.RefreshSources(new[] { projectMovePath }),
                    "Project restore refresh failed before database verification.");
                Assert.IsTrue(AssetDatabaseQueryService.TryGetMainRecordAtPath(projectMovePath, out var restoredProjectRecord),
                    "Project restore was not published to the asset database.");
                Assert.AreEqual(projectMoveRecord.SourceAssetID, restoredProjectRecord.SourceAssetID);

                lifecycleStage = "API copy";
                Assert.IsFalse(AssetOperationService.CopyAsset(sourcePath, apiCopyPath, out var apiCopyId),
                    string.Join(Environment.NewLine, AssetDatabaseQueryService.GetDiagnostics().Select(x => x.Code + ": " + x.Message)));
                Assert.AreNotEqual(Guid.Empty, apiCopyId);
                Assert.AreNotEqual(sourceId, apiCopyId);
                Assert.IsTrue(File.Exists(apiCopyPath));
                Assert.IsTrue(File.Exists(apiCopyPath + ".meta"));
                Assert.IsFalse(AssetPipelineService.RefreshSources(new[] { apiCopyPath }),
                    "API copy refresh failed before the dependent move.");
                lifecycleStage = "API move";
                Assert.IsFalse(AssetOperationService.MoveAsset(apiCopyPath, apiMovePath),
                    string.Join(Environment.NewLine, AssetDatabaseQueryService.GetDiagnostics().Select(x => x.Code + ": " + x.Message)));
                Assert.IsFalse(File.Exists(apiCopyPath));
                Assert.IsFalse(File.Exists(apiCopyPath + ".meta"));
                Assert.IsTrue(File.Exists(apiMovePath));
                Assert.IsTrue(File.Exists(apiMovePath + ".meta"));
                Assert.IsFalse(AssetPipelineService.RefreshSources(new[] { apiCopyPath, apiMovePath }),
                    "API move refresh failed before the dependent trash.");
                lifecycleStage = "API trash";
                Assert.IsFalse(AssetOperationService.TrashAsset(apiMovePath, out var trash),
                    string.Join(Environment.NewLine, AssetDatabaseQueryService.GetDiagnostics().Select(x => x.Code + ": " + x.Message)));
                Assert.AreEqual(apiCopyId, trash.AssetGuid);
                Assert.IsFalse(File.Exists(apiMovePath));
                Assert.IsFalse(File.Exists(apiMovePath + ".meta"));
                Assert.IsTrue(File.Exists(trash.TrashSourcePath));
                Assert.IsTrue(File.Exists(trash.TrashMetaPath));
                Assert.IsFalse(AssetPipelineService.RefreshSources(new[] { apiMovePath }),
                    "API trash refresh failed before restore.");
                lifecycleStage = "API restore";
                Assert.IsFalse(AssetOperationService.RestoreAsset(trash),
                    string.Join(Environment.NewLine, AssetDatabaseQueryService.GetDiagnostics().Select(x => x.Code + ": " + x.Message)));
                Assert.IsTrue(File.Exists(apiMovePath));
                Assert.IsTrue(File.Exists(apiMovePath + ".meta"));
                Assert.IsFalse(AssetPipelineService.RefreshSources(new[] { apiMovePath }),
                    "API restore refresh failed before database verification.");
                Assert.IsTrue(AssetDatabaseQueryService.TryGetMainRecordAtPath(apiMovePath, out var apiRestoredRecord));
                Assert.AreEqual(apiCopyId, apiRestoredRecord.SourceAssetID);

                lifecycleStage = "diagnostics";
                var lifecyclePaths = new[] { sourcePath, projectCopyPath, projectMovePath, apiCopyPath, apiMovePath };
                var diagnostics = AssetDatabaseQueryService.GetDiagnostics().Where(diagnostic =>
                    lifecyclePaths.Any(path => !string.IsNullOrEmpty(diagnostic.SourcePath) &&
                        ContentMutationPathUtils.Comparer.Equals(ContentMutationPathUtils.Normalize(path),
                            ContentMutationPathUtils.Normalize(diagnostic.SourcePath)))).ToArray();
                Assert.IsEmpty(diagnostics, string.Join(Environment.NewLine,
                    diagnostics.Select(diagnostic => diagnostic.Code + ": " + diagnostic.Message)));
                Assert.Less(lifecycleTimer.ElapsedMilliseconds, 5000,
                    "UI-backed single mutation lifecycle exceeded its responsiveness budget.");
            }
            catch (Exception ex)
            {
                throw new InvalidOperationException("Single canonical pair mutation failed during " + lifecycleStage + ". " + ex, ex);
            }
            finally
            {
                AssetWorkspaceModule.CanonicalMoveObserver = null;
                deleteAction?.Dispose();
                foreach (var path in new[] { projectCopyPath, projectMovePath, apiCopyPath, apiMovePath, sourcePath })
                    CleanupCanonicalCopyAsset(path);
                AssetPipelineService.RefreshSources(new[] { sourcePath, projectCopyPath, projectMovePath, apiCopyPath, apiMovePath });
            }
        }

        public static int RunSingleCanonicalPairMutationRoutes()
        {
            new TestEditorUtils().TestSingleCanonicalPairMutationRoutes();
            return 0;
        }

        [Test]
        public void TestMixedFolderCopyUsesOneOrderedNativeBatch()
        {
            var root = Path.Combine(Globals.ProjectContentFolder, "__MixedFolderCopy_" + Guid.NewGuid().ToString("N"));
            var sourceFolderPath = Path.Combine(root, "Source");
            var destinationFolderPath = Path.Combine(root, "Destination");
            var nestedFolderPath = Path.Combine(sourceFolderPath, "Nested");
            var sourceAssetPath = Path.Combine(sourceFolderPath, "Canonical.txt");
            var destinationAssetPath = Path.Combine(destinationFolderPath, "Canonical.txt");
            AssetCopyEntryRequest[] observedEntries = null;
            string[] presentedPaths = null;
            ContentItemFilesystemAction folderDeleteAction = null;
            var lifecycleTimer = System.Diagnostics.Stopwatch.StartNew();
            var lifecycleStage = "setup";
            var consoleDiagnostics = new List<string>();
            LogMessageDelegate captureConsoleDiagnostic = (level, message, stackTrace, threadId) =>
            {
                if (level == LogType.Warning || level == LogType.Error || level == LogType.Fatal)
                {
                    lock (consoleDiagnostics)
                        consoleDiagnostics.Add("[" + lifecycleStage + "] " + level + ": " + message);
                }
            };
            Debug.LogMessageReceived += captureConsoleDiagnostic;
            try
            {
                Directory.CreateDirectory(nestedFolderPath);
                var sourceFolder = new ContentFolder(ContentFolderType.Content, sourceFolderPath, null);
                CreateCanonicalTextItem(sourceAssetPath, sourceFolder, out var sourceAssetId);
                var plainPath = Path.Combine(sourceFolderPath, "Plain.txt");
                File.WriteAllText(plainPath, "plain");
                new FileItem(plainPath) { ParentFolder = sourceFolder };
                var nestedFolder = new ContentFolder(ContentFolderType.Content, nestedFolderPath, null);
                sourceFolder.Children.Add(nestedFolder);
                var nestedPath = Path.Combine(nestedFolderPath, "Nested.txt");
                File.WriteAllText(nestedPath, "nested");
                new FileItem(nestedPath) { ParentFolder = nestedFolder };
                AssetWorkspaceModule.NativeCopyBatchObserver = entries => observedEntries = entries.ToArray();
                AssetWorkspaceModule.NativePresentationObserver = (_, paths) => presentedPaths = paths.ToArray();

                var workspace = FlaxEditor.Editor.Instance.ContentDatabase;
                lifecycleStage = "copy";
                var result = workspace.Copy(sourceFolder, destinationFolderPath);

                Assert.IsTrue(result.Succeeded, result.Message);
                Assert.NotNull(observedEntries);
                CollectionAssert.AreEqual(new[]
                {
                    AssetCopyEntryKind.Directory,
                    AssetCopyEntryKind.CanonicalAsset,
                    AssetCopyEntryKind.Directory,
                    AssetCopyEntryKind.File,
                    AssetCopyEntryKind.File,
                }, observedEntries.Select(x => x.Kind).ToArray());
                Assert.AreEqual(Path.GetFullPath(destinationFolderPath), Path.GetFullPath(observedEntries[0].DestinationPath));
                Assert.AreEqual(Path.GetFullPath(Path.Combine(destinationFolderPath, "Nested")), Path.GetFullPath(observedEntries[2].DestinationPath));
                Assert.AreEqual(Path.GetFullPath(Path.Combine(destinationFolderPath, "Nested", "Nested.txt")), Path.GetFullPath(observedEntries[3].DestinationPath));
                Assert.AreEqual(1, result.CompletedPaths.Length);
                CollectionAssert.AreEqual(new[] { Path.GetFullPath(destinationFolderPath) },
                    presentedPaths.Select(Path.GetFullPath).ToArray());
                Assert.IsTrue(File.Exists(destinationAssetPath));
                Assert.IsTrue(File.Exists(destinationAssetPath + ".meta"));
                Assert.IsTrue(File.Exists(Path.Combine(destinationFolderPath, "Plain.txt")));
                Assert.IsTrue(File.Exists(Path.Combine(destinationFolderPath, "Nested", "Nested.txt")));
                var destinationRecords = AssetWorkspaceQuery.QueryAllRecords(new AssetDatabaseQuery
                {
                    PathPrefix = destinationFolderPath,
                    MainAssetsOnly = true,
                });
                var copiedRecord = destinationRecords.FirstOrDefault(x =>
                    ContentMutationPathUtils.Comparer.Equals(ContentMutationPathUtils.Normalize(x.SourcePath),
                        ContentMutationPathUtils.Normalize(destinationAssetPath)));
                Assert.AreNotEqual(Guid.Empty, copiedRecord.SourceAssetID);
                Assert.AreNotEqual(sourceAssetId, copiedRecord.SourceAssetID);
                Assert.AreEqual(Path.GetFullPath(destinationAssetPath),
                    Path.GetFullPath(AssetDatabaseQueryService.GUIDToAssetPath(copiedRecord.SourceAssetID)));

                var copiedItem = workspace.FindAsset(copiedRecord.SourceAssetID);
                Assert.NotNull(copiedItem);
                Assert.AreEqual(Path.GetFullPath(destinationAssetPath), Path.GetFullPath(copiedItem.Path));
                var contentWindow = FlaxEditor.Editor.Instance.Windows.ContentWin;
                Assert.AreEqual(1, contentWindow.Selection.Count);
                Assert.AreEqual(Path.GetFullPath(destinationFolderPath),
                    Path.GetFullPath(contentWindow.Selection[0].Path));
                contentWindow.Select(copiedItem, true);
                Assert.AreEqual(1, contentWindow.Selection.Count);
                Assert.AreEqual(Path.GetFullPath(destinationAssetPath),
                    Path.GetFullPath(contentWindow.Selection[0].Path));
                contentWindow.ClearSelection(false);

                var destinationFolder = workspace.Find(destinationFolderPath) as ContentFolder;
                Assert.NotNull(destinationFolder);
                contentWindow.Select(destinationFolder, true);
                var movedFolderPath = destinationFolderPath + " Moved";
                var movedAssetPath = Path.Combine(movedFolderPath, "Canonical.txt");
                lifecycleStage = "move";
                var moveResult = workspace.TryMove(new[] { (Item: (ContentItem)destinationFolder, Destination: movedFolderPath) });
                Assert.IsTrue(moveResult.Succeeded, moveResult.Message);
                Assert.IsFalse(Directory.Exists(destinationFolderPath));
                Assert.IsFalse(File.Exists(destinationAssetPath));
                Assert.IsFalse(File.Exists(destinationAssetPath + ".meta"));
                Assert.IsTrue(Directory.Exists(movedFolderPath));
                Assert.IsTrue(File.Exists(movedAssetPath));
                Assert.IsTrue(File.Exists(movedAssetPath + ".meta"));
                Assert.IsTrue(AssetDatabaseQueryService.TryGetMainRecordAtPath(movedAssetPath, out var movedRecord));
                Assert.AreEqual(copiedRecord.SourceAssetID, movedRecord.SourceAssetID);
                Assert.AreEqual(1, contentWindow.Selection.Count);
                Assert.AreEqual(Path.GetFullPath(movedFolderPath), Path.GetFullPath(contentWindow.Selection[0].Path));

                lifecycleStage = "delete";
                folderDeleteAction = ContentItemFilesystemAction.Delete(FlaxEditor.Editor.Instance,
                    new List<ContentItem> { destinationFolder });
                Assert.NotNull(folderDeleteAction);
                Assert.IsFalse(Directory.Exists(movedFolderPath));
                Assert.IsFalse(File.Exists(movedAssetPath));
                Assert.IsFalse(File.Exists(movedAssetPath + ".meta"));
                lifecycleStage = "restore";
                Assert.IsTrue(folderDeleteAction.TryUndo());
                Assert.IsTrue(Directory.Exists(movedFolderPath));
                Assert.IsTrue(File.Exists(movedAssetPath));
                Assert.IsTrue(File.Exists(movedAssetPath + ".meta"));
                Assert.IsFalse(AssetPipelineService.RefreshSources(new[] { movedFolderPath }));
                Assert.IsTrue(AssetDatabaseQueryService.TryGetMainRecordAtPath(movedAssetPath, out var restoredRecord));
                Assert.AreEqual(copiedRecord.SourceAssetID, restoredRecord.SourceAssetID);
                var lifecycleDiagnostics = AssetDatabaseQueryService.GetDiagnostics().Where(diagnostic =>
                    !string.IsNullOrEmpty(diagnostic.SourcePath) &&
                    ContentMutationPathUtils.IsWithinRoot(diagnostic.SourcePath, root, true)).ToArray();
                Assert.IsEmpty(lifecycleDiagnostics, string.Join(Environment.NewLine,
                    lifecycleDiagnostics.Select(diagnostic => diagnostic.Code + ": " + diagnostic.Message)));
                Assert.Less(lifecycleTimer.ElapsedMilliseconds, 5000,
                    "UI-backed mixed folder lifecycle exceeded its responsiveness budget.");
                Assert.IsEmpty(consoleDiagnostics, string.Join(Environment.NewLine, consoleDiagnostics));

                Assert.IsFalse(AssetDatabaseQueryService.TryGetMainRecordAtPath(destinationAssetPath, out _));
                Assert.IsTrue(File.Exists(sourceAssetPath));
                Assert.IsTrue(File.Exists(sourceAssetPath + ".meta"));
            }
            finally
            {
                AssetWorkspaceModule.NativeCopyBatchObserver = null;
                AssetWorkspaceModule.NativePresentationObserver = null;
                Debug.LogMessageReceived -= captureConsoleDiagnostic;
                folderDeleteAction?.Dispose();
                CleanupCanonicalCopyAsset(destinationAssetPath);
                CleanupCanonicalCopyAsset(sourceAssetPath);
                if (Directory.Exists(root))
                    Directory.Delete(root, true);
                AssetPipelineService.RefreshSources(new[] { root });
            }
        }

        public static int RunMixedFolderCopyUsesOneOrderedNativeBatch()
        {
            try
            {
                var tests = new TestEditorUtils();
                tests.TestMixedFolderCopyUsesOneOrderedNativeBatch();
                tests.TestNativePresentationExcludesRolledBackPartialPaths();
                tests.TestContentCopyRejectsFolderAndCrossTypeCollisions();
            }
            catch (Exception ex)
            {
                throw new InvalidOperationException(ex.ToString(), ex);
            }
            return 0;
        }

        [Test]
        public void TestNativePresentationExcludesRolledBackPartialPaths()
        {
            var first = Path.GetFullPath(Path.Combine(Globals.ProjectContentFolder, "First.txt"));
            var second = Path.GetFullPath(Path.Combine(Globals.ProjectContentFolder, "Second.txt"));
            var result = ContentMutationResult.Fail(ContentMutationFailure.CopyFailed, first, second,
                "Injected partial failure.", completedPaths: new[] { first, second }, rolledBackPaths: new[] { second });

            CollectionAssert.AreEqual(new[] { first }, AssetWorkspaceModule.GetRetainedMutationPaths(result));
        }

        [Test]
        public void TestFolderCopyRoutesCanonicalChildrenThroughNativeAuthority()
        {
            var root = Path.Combine(Globals.ProjectContentFolder, "__CanonicalFolderCopy_" + Guid.NewGuid().ToString("N"));
            var sourceFolderPath = Path.Combine(root, "Source");
            var destinationFolderPath = Path.Combine(root, "Destination");
            string sourceAssetPath = null;
            var destinationAssetPath = Path.Combine(destinationFolderPath, "Canonical.txt");
            int nativeCopies = 0;
            try
            {
                Directory.CreateDirectory(root);
                var sourceFolder = CreateCanonicalTextFolder(sourceFolderPath, out sourceAssetPath, out var sourceAssetId);
                AssetWorkspaceModule.CanonicalCopyObserver = (source, destination) =>
                {
                    Assert.AreEqual(Path.GetFullPath(sourceAssetPath), Path.GetFullPath(source));
                    Assert.AreEqual(Path.GetFullPath(destinationAssetPath), Path.GetFullPath(destination));
                    nativeCopies++;
                };

                var result = new AssetWorkspaceModule(null).Copy(sourceFolder, destinationFolderPath);

                Assert.IsTrue(result.Succeeded, result.Message);
                Assert.AreEqual(1, nativeCopies);
                Assert.IsTrue(File.Exists(destinationAssetPath));
                Assert.IsTrue(File.Exists(destinationAssetPath + ".meta"));
                Assert.IsTrue(AssetDatabaseQueryService.TryGetMainRecordAtPath(destinationAssetPath, out var destinationRecord));
                Assert.AreNotEqual(sourceAssetId, destinationRecord.SourceAssetID);
            }
            finally
            {
                AssetWorkspaceModule.CanonicalCopyObserver = null;
                CleanupCanonicalCopyAsset(destinationAssetPath);
                CleanupCanonicalCopyAsset(sourceAssetPath);
                if (Directory.Exists(root))
                    Directory.Delete(root, true);
                AssetPipelineService.RefreshSources(new[] { root });
            }
        }

        [Test]
        public void TestFolderCopyBypassesManagedRollbackForNativeBatch()
        {
            var root = Path.Combine(Globals.ProjectContentFolder, "__CanonicalFolderRollback_" + Guid.NewGuid().ToString("N"));
            var sourceFolderPath = Path.Combine(root, "Source");
            var destinationFolderPath = Path.Combine(root, "Destination");
            string sourceAssetPath = null;
            var destinationAssetPath = Path.Combine(destinationFolderPath, "Canonical.txt");
            int nativeCopies = 0;
            int nativeDeletes = 0;
            try
            {
                Directory.CreateDirectory(root);
                var sourceFolder = CreateCanonicalTextFolder(sourceFolderPath, out sourceAssetPath, out _);
                AssetWorkspaceModule.CanonicalCopyObserver = (_, _) => nativeCopies++;
                AssetWorkspaceModule.CanonicalDeleteObserver = path =>
                {
                    Assert.AreEqual(Path.GetFullPath(destinationAssetPath), Path.GetFullPath(path));
                    nativeDeletes++;
                };
                ContentMutationTransaction.FaultInjector = point =>
                    point == "after-copy" ? new IOException("Managed copy path should not execute") : null;

                var result = new AssetWorkspaceModule(null).Copy(sourceFolder, destinationFolderPath);

                Assert.IsTrue(result.Succeeded, result.Message);
                Assert.AreEqual(1, nativeCopies);
                Assert.AreEqual(0, nativeDeletes);
                Assert.IsTrue(File.Exists(sourceAssetPath));
                Assert.IsTrue(File.Exists(sourceAssetPath + ".meta"));
                Assert.IsTrue(Directory.Exists(destinationFolderPath));
                Assert.IsTrue(File.Exists(destinationAssetPath));
            }
            finally
            {
                ContentMutationTransaction.FaultInjector = null;
                AssetWorkspaceModule.CanonicalCopyObserver = null;
                AssetWorkspaceModule.CanonicalDeleteObserver = null;
                CleanupCanonicalCopyAsset(destinationAssetPath);
                CleanupCanonicalCopyAsset(sourceAssetPath);
                if (Directory.Exists(root))
                    Directory.Delete(root, true);
                AssetPipelineService.RefreshSources(new[] { root });
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
                var timer = System.Diagnostics.Stopwatch.StartNew();

                var folderResult = database.Copy(sourceFolder, destinationFolderPath);
                var crossTypeResult = database.Copy(sourceFolder, fileCollisionPath);

                Assert.IsFalse(folderResult.Succeeded);
                Assert.IsFalse(crossTypeResult.Succeeded);
                Assert.AreEqual(ContentMutationFailure.DestinationCollision, folderResult.Failure);
                Assert.AreEqual(ContentMutationFailure.DestinationCollision, crossTypeResult.Failure);
                Assert.AreEqual(Path.GetFullPath(sourceFolderPath), Path.GetFullPath(folderResult.SourcePath));
                Assert.AreEqual(Path.GetFullPath(destinationFolderPath), Path.GetFullPath(folderResult.DestinationPath));
                Assert.IsEmpty(folderResult.CompletedPaths);
                Assert.IsEmpty(folderResult.RolledBackPaths);
                Assert.AreEqual("existing", File.ReadAllText(Path.Combine(destinationFolderPath, "Existing.txt")));
                Assert.AreEqual("existing file", File.ReadAllText(fileCollisionPath));
                Assert.Less(timer.ElapsedMilliseconds, 1000,
                    "Structured Project-panel collision preflight exceeded its responsiveness budget.");
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
        public void TestContentTransactionInjectedFailureRollsBackImmediately()
        {
            var root = Path.Combine(Path.GetTempPath(), "FlaxContentTransactionTests", Guid.NewGuid().ToString("N"));
            var source = Path.Combine(root, "Source.txt");
            var destination = Path.Combine(root, "Destination.txt");
            Directory.CreateDirectory(root);
            File.WriteAllText(source, "source");
            try
            {
                var plan = new ContentMutationPlan(ContentMutationOperationKind.Move);
                plan.Entries.Add(new ContentMutationEntry(source, destination, ContentMutationPathRole.Main, false));
                var transaction = new ContentMutationTransaction(plan);
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
                Assert.IsTrue(File.Exists(source));
                Assert.IsFalse(File.Exists(destination));
                var normalizedDestination = ContentMutationPathUtils.Normalize(destination);
                CollectionAssert.AreEqual(new[] { normalizedDestination }, result.CompletedPaths);
                CollectionAssert.AreEqual(new[] { normalizedDestination }, result.RolledBackPaths);
                Assert.IsEmpty(AssetWorkspaceModule.GetRetainedMutationPaths(result));
            }
            finally
            {
                ContentMutationTransaction.FaultInjector = null;
                Directory.Delete(root, true);
            }
        }

        [Test]
        public void TestContentTransactionRollbackFailureIsStructured()
        {
            var root = Path.Combine(Path.GetTempPath(), "FlaxContentTransactionTests", Guid.NewGuid().ToString("N"));
            var source = Path.Combine(root, "Source.txt");
            var destination = Path.Combine(root, "Destination.txt");
            Directory.CreateDirectory(root);
            File.WriteAllText(source, "source");
            try
            {
                var plan = new ContentMutationPlan(ContentMutationOperationKind.Move);
                plan.Entries.Add(new ContentMutationEntry(source, destination, ContentMutationPathRole.Main, false));
                ContentMutationTransaction.FaultInjector = point => point == "after-main-move" ? new IOException("Injected failure") : null;

                var result = new ContentMutationTransaction(plan).Execute(new[]
                {
                    new ContentMutationStep("main-move", new[] { 0 }, () =>
                    {
                        File.Move(source, destination);
                        return ContentMutationResult.Success(source, destination);
                    }, () => false)
                });

                Assert.IsFalse(result.Succeeded);
                Assert.AreEqual(ContentMutationFailure.RollbackFailure, result.Failure);
                Assert.IsFalse(File.Exists(source));
                Assert.IsTrue(File.Exists(destination));
            }
            finally
            {
                ContentMutationTransaction.FaultInjector = null;
                Directory.Delete(root, true);
            }
        }

        public static int RunContentMutationRollbackTests()
        {
            var tests = new TestEditorUtils();
            tests.TestContentTransactionInjectedFailureRollsBackImmediately();
            tests.TestContentTransactionRollbackFailureIsStructured();
            return 0;
        }

        [Test]
        public void TestContentWorkspaceCreateReportsOrdinaryWriteFailuresWithoutJournals()
        {
            var root = Path.Combine(Path.GetTempPath(), "FlaxContentWriteFailureTests", Guid.NewGuid().ToString("N"));
            var successPath = Path.Combine(root, "Created.txt");
            var deniedPath = Path.Combine(root, "Denied.txt");
            var failedPath = Path.Combine(root, "Failed.txt");
            Directory.CreateDirectory(root);
            try
            {
                var workspace = new AssetWorkspaceModule(null);
                var success = workspace.CreatePath(successPath, false, () => File.WriteAllText(successPath, "created"));
                var denied = workspace.CreatePath(deniedPath, false, () => throw new UnauthorizedAccessException("Injected write denial."));
                var failed = workspace.CreatePath(failedPath, false, () => throw new IOException("Injected disk write failure."));

                Assert.IsTrue(success.Succeeded, success.Message);
                Assert.AreEqual(ContentMutationFailure.None, success.Failure);
                CollectionAssert.AreEqual(new[] { ContentMutationPathUtils.Normalize(successPath) }, success.CompletedPaths);
                Assert.AreEqual("created", File.ReadAllText(successPath));

                Assert.IsFalse(denied.Succeeded);
                Assert.AreEqual(ContentMutationFailure.PermissionDenied, denied.Failure);
                Assert.IsEmpty(denied.CompletedPaths);
                Assert.IsEmpty(AssetWorkspaceModule.GetRetainedMutationPaths(denied));
                Assert.IsFalse(File.Exists(deniedPath));
                Assert.IsFalse(File.Exists(deniedPath + ".meta"));

                Assert.IsFalse(failed.Succeeded);
                Assert.AreEqual(ContentMutationFailure.LockedStorage, failed.Failure);
                Assert.IsEmpty(failed.CompletedPaths);
                Assert.IsEmpty(AssetWorkspaceModule.GetRetainedMutationPaths(failed));
                Assert.IsFalse(File.Exists(failedPath));
                Assert.IsFalse(File.Exists(failedPath + ".meta"));

                var legacyJournalRoot = Path.Combine(Globals.ProjectCacheFolder, "ContentMutationRecovery");
                Assert.IsFalse(File.Exists(Path.Combine(legacyJournalRoot, success.TransactionId.ToString("N") + ".json")));
                Assert.IsFalse(File.Exists(Path.Combine(legacyJournalRoot, denied.TransactionId.ToString("N") + ".json")));
                Assert.IsFalse(File.Exists(Path.Combine(legacyJournalRoot, failed.TransactionId.ToString("N") + ".json")));
            }
            finally
            {
                Directory.Delete(root, true);
            }
        }

        public static int RunContentMutationWriteFailureTests()
        {
            new TestEditorUtils().TestContentWorkspaceCreateReportsOrdinaryWriteFailuresWithoutJournals();
            return 0;
        }

        [Test]
        public void TestContentTransactionRejectsSourceChangedAfterPreflight()
        {
            var root = Path.Combine(Path.GetTempPath(), "FlaxContentTransactionTests", Guid.NewGuid().ToString("N"));
            var source = Path.Combine(root, "Source.txt");
            var destination = Path.Combine(root, "Destination.txt");
            Directory.CreateDirectory(root);
            File.WriteAllText(source, "source");
            try
            {
                var plan = new ContentMutationPlan(ContentMutationOperationKind.Move);
                plan.Entries.Add(new ContentMutationEntry(source, destination, ContentMutationPathRole.Main, false));
                var transaction = new ContentMutationTransaction(plan);
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
        public void TestContentCreateTransactionDoesNotRequireSourcePath()
        {
            var root = Path.Combine(Path.GetTempPath(), "FlaxContentTransactionTests", Guid.NewGuid().ToString("N"));
            var destination = Path.Combine(root, "Created.txt");
            Directory.CreateDirectory(root);
            try
            {
                var plan = new ContentMutationPlan(ContentMutationOperationKind.Create);
                plan.Entries.Add(new ContentMutationEntry(destination, destination, ContentMutationPathRole.Main, false)
                {
                    SourceRequired = false,
                });
                var result = new ContentMutationTransaction(plan).Execute(new[]
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
                var result = new ContentMutationTransaction(plan).Execute(new[]
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
                        result = new ContentMutationTransaction(plan).Execute(new[]
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
                        result = new ContentMutationTransaction(plan).Execute(new[]
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
                        result = new ContentMutationTransaction(plan).Execute(new[]
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

        public static int RunProjectPanelShowsGameProjectRoot()
        {
            var game = FlaxEditor.Editor.Instance.ContentDatabase.Game;
            Assert.IsNotNull(game);
            Assert.AreEqual(FlaxEditor.Editor.Instance.GameProject.Name, game.Text);
            Assert.IsTrue(game.ShowHeader);
            Assert.IsTrue(game.IsSelectable);
            Assert.Greater(game.ChildrenIndent, 0.0f);
            Assert.AreSame(game, game.Content.ParentNode);
            Assert.AreSame(game, game.Source.ParentNode);
            var genericJson = new GenericJsonAssetProxy();
            Assert.IsTrue(genericJson.AcceptsAsset(typeof(JsonAsset).FullName, "ProjectSettingsIndex.settings"));
            Assert.IsFalse(genericJson.AcceptsAsset("Example.Settings", "GameSettings.settings"));
            return 0;
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

        private static AssetItem AssertAuthoredEditorRoute(string path, Guid id, string typeName, string processor,
            string itemType, string proxyType, Type windowType)
        {
            Assert.IsTrue(AssetDatabaseQueryService.TryGetRecord(id, out var record), "Missing database record for " + id);
            Assert.AreEqual(id, record.ID);
            StringAssert.AreEqualIgnoringCase(Path.GetFullPath(path), Path.GetFullPath(record.SourcePath));
            Assert.AreEqual(typeName, record.TypeName);
            Assert.AreEqual(processor, record.ProcessorID);
            Assert.AreEqual(AssetSourceKind.TextDocument, record.SourceKind);
            Assert.AreEqual(AssetRecordStatus.Ready, record.Status);

            var workspace = FlaxEditor.Editor.Instance.ContentDatabase;
            var item = workspace.FindAsset(id);
            Assert.NotNull(item, "Project item missing for " + path);
            Assert.AreEqual(itemType, item.GetType().Name);
            Assert.AreEqual(typeName, item.TypeName);
            Assert.IsFalse(item.IsOfType<RawDataAsset>(), "Authored asset was exposed as RawData for " + path);
            Assert.IsTrue(item.IsCanonicalSource, "Project item is not canonical for " + path);
            var proxy = workspace.GetProxy(item);
            Assert.NotNull(proxy, "Project proxy missing for " + path);
            Assert.AreEqual(proxyType, proxy.GetType().Name);
            Assert.IsFalse(proxy is FileProxy, "Authored asset was routed through FileProxy for " + path);

            var contentWindow = FlaxEditor.Editor.Instance.Windows.ContentWin;
            contentWindow.Select(item, true);
            Assert.AreSame(item, contentWindow.Selection.Single());
            contentWindow.ClearSelection(false);

            var editorWindow = FlaxEditor.Editor.Instance.ContentEditing.Open(item, true);
            try
            {
                Assert.NotNull(editorWindow, "Editor window missing for " + path);
                Assert.AreEqual(windowType, editorWindow.GetType());
            }
            finally
            {
                editorWindow?.Close();
            }
            return item;
        }

        private static void RoundTripAuthoredGraph<TAsset>(AssetItem item, string propertiesJson = null) where TAsset : Asset
        {
            var asset = AssetDocumentRegistry.OpenGraph<TAsset>(item, out var session);
            try
            {
                Assert.NotNull(asset, "Graph asset failed to open for " + item.Path);
                Assert.AreEqual(typeof(TAsset), asset.GetType());
                Assert.IsFalse(asset.WaitForLoaded());
                session.SetGraphSurface((byte[])session.GetGraphSurface().Clone());
                Assert.IsFalse(session.SaveGraph(item, propertiesJson));
                Assert.IsFalse(session.IsDirty);
                Assert.IsTrue(session.ReloadFromDisk(), item.Path + " did not reload from disk.");
                asset.Reload();
                Assert.IsFalse(asset.WaitForLoaded());
            }
            finally
            {
                AssetDocumentRegistry.Close(item, ref session);
            }
        }

        private static void AssertAuthoredRuntimeType<TAsset>(Guid id) where TAsset : Asset
        {
            var asset = FlaxEngine.Content.LoadAssetAsync<TAsset>(id);
            Assert.NotNull(asset, "Runtime asset failed to load for " + typeof(TAsset).FullName + " " + id);
            Assert.AreEqual(typeof(TAsset), asset.GetType());
            Assert.IsFalse(asset.WaitForLoaded());
            asset.Reload();
            Assert.IsFalse(asset.WaitForLoaded());
        }

        private static void AssertAuthoredGraphReopens<TAsset>(AssetItem item) where TAsset : Asset
        {
            var asset = AssetDocumentRegistry.OpenGraph<TAsset>(item, out var session);
            try
            {
                Assert.NotNull(asset, "Graph asset failed to reopen for " + item.Path);
                Assert.IsFalse(asset.WaitForLoaded());
                Assert.AreEqual(Path.GetFullPath(item.Path), Path.GetFullPath(session.SourcePath));
            }
            finally
            {
                AssetDocumentRegistry.Close(item, ref session);
            }
        }

        private static void AssertAuthoredText(string path, string typeName)
        {
            var bytes = File.ReadAllBytes(path);
            Assert.Greater(bytes.Length, 2);
            Assert.AreNotEqual((byte)'G', bytes[0], "Authored source was replaced by a binary Flax artifact.");
            var json = JObject.Parse(File.ReadAllText(path));
            Assert.AreEqual(typeName, (string)json["type"]);
        }

        private static void WaitForCurrentArtifact(Guid id)
        {
            var timer = System.Diagnostics.Stopwatch.StartNew();
            while (!AssetPipelineService.IsArtifactCurrent(id))
            {
                var status = AssetPipelineService.GetBuildStatus(id);
                Assert.IsFalse(status == "Failed" || status == "Cancelled",
                    "Artifact build ended as " + status + " for " + id + ": " + AssetPipelineService.GetBuildDiagnostic(id).Message);
                Assert.Less(timer.Elapsed, TimeSpan.FromMinutes(5), "Artifact build exceeded the five-minute ceiling for " + id);
                Thread.Sleep(1);
            }
        }

        private static byte[] MoveParticleGraphNode(byte[] data, Float2 delta)
        {
            var owner = new ParticleSurfaceOwner { SurfaceData = (byte[])data.Clone() };
            var surface = new ParticleEmitterSurface(owner, null, null);
            try
            {
                Assert.IsFalse(surface.Load(), "Particle test surface failed to load.");
                var node = surface.Nodes.FirstOrDefault(x => x != surface.RootNode);
                Assert.NotNull(node, "Particle template has no movable graph node.");
                node.Location += delta;
                Assert.IsFalse(surface.Save(), "Particle test surface failed to save after moving a node.");
                return owner.SurfaceData;
            }
            finally
            {
                surface.Dispose();
            }
        }

        private static byte[] MoveMaterialGraphNode(byte[] data, Float2 delta)
        {
            var owner = new ParticleSurfaceOwner { SurfaceData = (byte[])data.Clone() };
            var surface = new MaterialSurface(owner);
            try
            {
                Assert.IsFalse(surface.Load(), "Material test surface failed to load.");
                var node = surface.Nodes.FirstOrDefault();
                Assert.NotNull(node, "Material graph has no movable node.");
                node.Location += delta;
                Assert.IsFalse(surface.Save(), "Material test surface failed to save after moving a node.");
                return owner.SurfaceData;
            }
            finally
            {
                surface.Dispose();
            }
        }

        private static void ApplyPublishedArtifact(Guid objectId)
        {
            var expected = AssetDatabaseQueryService.GetCurrentRuntimeArtifactCacheID(objectId);
            Assert.AreNotEqual(Guid.Empty, expected, "Published artifact has no exact cache identity.");
            var timer = System.Diagnostics.Stopwatch.StartNew();
            while (true)
            {
                AssetPipelineService.DispatchArtifactUpdatesForTesting();
                var asset = AssetDatabaseQueryService.LoadAssetPreview(objectId);
                if (AssetDatabaseQueryService.GetLoadedRuntimeArtifactCacheID(asset) == expected)
                    break;
                Assert.Less(timer.Elapsed, TimeSpan.FromSeconds(30), "Artifact hot-swap did not finish.");
                Thread.Sleep(1);
            }
            FlaxEditor.Editor.Instance.ContentDatabase.OnUpdate();
        }

        [Test]
        public void TestParticleAndCollisionAuthoredTextLifecycle()
        {
            var root = Path.Combine(Globals.ProjectContentFolder, "__AuthoredTextLifecycle_" + Guid.NewGuid().ToString("N"));
            var emitterPath = Path.Combine(root, "Emitter.particleemitter");
            var systemPath = Path.Combine(root, "System.particlesystem");
            var collisionPath = Path.Combine(root, "Collision.collisiondata");
            var modelPath = Path.Combine(root, "Triangle.gltf");
            var consoleDiagnostics = new List<string>();
            var lifecycleStage = "setup";
            LogMessageDelegate captureConsoleDiagnostic = (level, message, stackTrace, threadId) =>
            {
                if (level == LogType.Warning || level == LogType.Error || level == LogType.Fatal)
                {
                    lock (consoleDiagnostics)
                        consoleDiagnostics.Add("[" + lifecycleStage + "] " + level + ": " + message);
                }
            };
            Debug.LogMessageReceived += captureConsoleDiagnostic;
            try
            {
                Directory.CreateDirectory(root);
                File.WriteAllText(modelPath, "{\"asset\":{\"version\":\"2.0\"},\"buffers\":[{\"byteLength\":44,\"uri\":\"data:application/octet-stream;base64,AAAAAAAAAAAAAAAAAACAPwAAAAAAAAAAAAAAAAAAgD8AAAAAAAABAAIAAAA=\"}],\"bufferViews\":[{\"buffer\":0,\"byteOffset\":0,\"byteLength\":36,\"target\":34962},{\"buffer\":0,\"byteOffset\":36,\"byteLength\":6,\"target\":34963}],\"accessors\":[{\"bufferView\":0,\"componentType\":5126,\"count\":3,\"type\":\"VEC3\",\"max\":[1,1,0],\"min\":[0,0,0]},{\"bufferView\":1,\"componentType\":5123,\"count\":3,\"type\":\"SCALAR\"}],\"meshes\":[{\"primitives\":[{\"attributes\":{\"POSITION\":0},\"indices\":1}]}],\"nodes\":[{\"mesh\":0}],\"scenes\":[{\"nodes\":[0]}],\"scene\":0}");
                lifecycleStage = "model metadata";
                var modelId = ModelImporterService.CreateDefaultMetadata(modelPath);
                lifecycleStage = "emitter create";
                var emitterId = AssetDocumentRegistry.CreateGraph(emitterPath, typeof(ParticleEmitter).FullName);
                lifecycleStage = "system create";
                var systemId = AuthoredAssetDocumentService.Create(systemPath, typeof(ParticleSystem).FullName);
                lifecycleStage = "collision create";
                var collisionId = AuthoredAssetDocumentService.Create(collisionPath, typeof(CollisionData).FullName);
                Assert.AreNotEqual(Guid.Empty, emitterId);
                Assert.AreNotEqual(Guid.Empty, systemId);
                Assert.AreNotEqual(Guid.Empty, collisionId);
                Assert.AreNotEqual(Guid.Empty, modelId, string.Join(Environment.NewLine,
                    AssetDatabaseQueryService.GetDiagnostics().Select(x => x.Code + ": " + x.Message)));
                lifecycleStage = "initial refresh";
                Assert.IsFalse(AssetPipelineService.RefreshSources(new[] { modelPath, emitterPath, systemPath, collisionPath }));
                lifecycleStage = "model foreground build";
                Assert.IsFalse(AssetPipelineService.BuildAssetForeground(modelId));

                lifecycleStage = "editor routes";
                var emitterItem = AssertAuthoredEditorRoute(emitterPath, emitterId, typeof(ParticleEmitter).FullName,
                    "Flax.GraphDocument", "BinaryAssetItem", "ParticleEmitterProxy", typeof(ParticleEmitterWindow));
                var systemItem = AssertAuthoredEditorRoute(systemPath, systemId, typeof(ParticleSystem).FullName,
                    "Flax.ParticleSystem", "ParticleSystemItem", "ParticleSystemProxy", typeof(ParticleSystemWindow));
                AssertAuthoredEditorRoute(collisionPath, collisionId, typeof(CollisionData).FullName,
                    "Flax.CollisionData", "CollisionDataItem", "CollisionDataProxy", typeof(CollisionDataWindow));

                lifecycleStage = "emitter edit";
                var thumbnailOwner = new ThumbnailOwner();
                var stableEmitterId = emitterItem.ObjectID;
                var emitter = AssetDocumentRegistry.OpenGraph<ParticleEmitter>(emitterItem, out var emitterSession);
                try
                {
                    Assert.NotNull(emitter);
                    Assert.IsFalse(emitter.WaitForLoaded());
                    var template = FlaxEngine.Content.LoadAsyncInternal<ParticleEmitter>("Editor/Particles/Constant Burst");
                    Assert.NotNull(template);
                    Assert.IsFalse(template.WaitForLoaded(), "Particle thumbnail template failed to load.");
                    emitterSession.SetGraphSurface(template.LoadSurface(false));
                    Assert.IsFalse(emitterSession.SaveGraph(emitterItem));
                    ApplyPublishedArtifact(emitterId);

                    emitterItem.Thumbnail = FlaxEditor.Editor.Instance.Icons.Document128;
                    emitterItem.AddReference(thumbnailOwner, true);
                    var movedSurface = MoveParticleGraphNode(emitterSession.GetGraphSurface(), new Float2(11, 7));
                    emitterSession.SetGraphSurface(movedSurface);
                    Assert.IsFalse(emitterSession.SaveGraph(emitterItem));
                    Assert.IsFalse(emitterItem.IsThumbnailRequestQueuedForTesting,
                        "Save queued a thumbnail before the published runtime artifact was hot-swapped.");
                    ApplyPublishedArtifact(emitterId);
                    Assert.IsTrue(emitterItem.IsThumbnailRequestQueuedForTesting,
                        "Published particle artifact did not renew visible thumbnail demand.");
                    Assert.IsTrue(emitterItem.Thumbnail.IsValid,
                        "Publication discarded the last-good thumbnail before its replacement was ready.");
                    Assert.IsFalse(emitterItem.ExpireThumbnailDemand(Engine.FrameCount + 2),
                        "The save-reconciliation regression requires a transient visibility lapse.");
                    Assert.IsFalse(emitterItem.IsThumbnailRequestQueuedForTesting,
                        "Cancelling the post-save request left its queued state stuck.");
                    emitterItem.RequestThumbnail(thumbnailOwner);
                    Assert.IsTrue(FlaxEditor.Editor.Instance.Thumbnails.HasDeferredPreviewForTesting(emitterItem),
                        "Restored Project-tree visibility did not requeue the cancelled post-save thumbnail.");
                    emitterItem.RemoveReference(thumbnailOwner);

                    emitterItem.Thumbnail = FlaxEditor.Editor.Instance.Icons.Document128;
                    emitterItem.AddReference(thumbnailOwner, true);
                    emitterSession.SetGraphSurface(MoveParticleGraphNode(emitterSession.GetGraphSurface(), new Float2(5, 3)));
                    Assert.IsFalse(emitterSession.SaveGraph(emitterItem));
                    Assert.IsFalse(emitterItem.IsThumbnailRequestQueuedForTesting,
                        "Repeated save queued a thumbnail before its runtime artifact was hot-swapped.");
                    ApplyPublishedArtifact(emitterId);
                    Assert.AreEqual(stableEmitterId, emitterItem.ObjectID,
                        "Repeated graph saves changed the emitter's persistent thumbnail identity.");
                    Assert.IsTrue(emitterItem.IsThumbnailRequestQueuedForTesting,
                        "Repeated publication did not renew visible thumbnail demand.");
                    Assert.IsFalse(emitterSession.IsDirty);
                    Assert.IsTrue(emitterSession.ReloadFromDisk(), "Emitter graph session did not reload from disk.");
                }
                finally
                {
                    AssetDocumentRegistry.Close(emitterItem, ref emitterSession);
                    emitterItem.RemoveReference(thumbnailOwner);
                }

                lifecycleStage = "timeline and collision save";
                var timeline = AuthoredAssetDocumentService.LoadParticleSystemTimeline(systemPath);
                Assert.IsFalse(FlaxEditor.Editor.Instance.ContentDatabase.SaveAsset(systemPath,
                    () => AuthoredAssetDocumentService.SaveParticleSystemTimeline(systemPath, timeline)));

                var collisionFailed = AuthoredAssetDocumentService.SaveCollisionData(collisionPath,
                    CollisionDataType.TriangleMesh, modelId, 0, uint.MaxValue, ConvexMeshGenerationFlags.None, 255);
                var collisionDiagnostics = AssetDatabaseQueryService.GetDiagnostics();
                lock (consoleDiagnostics)
                    Assert.IsFalse(collisionFailed, string.Join(Environment.NewLine,
                        consoleDiagnostics.Concat(collisionDiagnostics.Select(x => x.Code + ": " + x.Message))));

                lifecycleStage = "builds";
                foreach (var id in new[] { emitterId, systemId, collisionId })
                {
                    Assert.IsFalse(AssetPipelineService.BuildAssetForeground(id));
                    WaitForCurrentArtifact(id);
                }
                lifecycleStage = "cook emitter";
                var cookValidationTimer = System.Diagnostics.Stopwatch.StartNew();
                var cookValidationDeadline = TimeSpan.FromSeconds(30);
                CurrentFormatCookerValidation.AssertCooks(emitterId, "particle emitter", cookValidationTimer,
                    cookValidationDeadline);
                lifecycleStage = "cook system";
                CurrentFormatCookerValidation.AssertCooks(systemId, "particle system", cookValidationTimer,
                    cookValidationDeadline);
                lifecycleStage = "cook collision";
                CurrentFormatCookerValidation.AssertCooks(collisionId, "collision data", cookValidationTimer,
                    cookValidationDeadline);

                lifecycleStage = "collision dependency invalidation";
                File.AppendAllText(modelPath, " ");
                Assert.IsFalse(AssetPipelineService.RefreshSources(new[] { modelPath }));
                Assert.IsFalse(AssetPipelineService.BuildAssetForeground(modelId));
                Assert.IsFalse(AssetPipelineService.IsArtifactCurrent(collisionId),
                    "Collision artifact remained current after its source model changed.");
                Assert.IsFalse(AssetPipelineService.RebuildAsset(collisionId, true));
                Assert.IsTrue(AssetPipelineService.IsArtifactCurrent(collisionId),
                    "Collision artifact did not become current after dependency rebuild.");

                lifecycleStage = "runtime reload";
                var system = FlaxEngine.Content.LoadAssetAsync<ParticleSystem>(systemId);
                var collision = FlaxEngine.Content.LoadAssetAsync<CollisionData>(collisionId);
                Assert.NotNull(system);
                Assert.NotNull(collision);
                Assert.IsFalse(system.WaitForLoaded());
                Assert.IsFalse(collision.WaitForLoaded());
                system.Reload();
                collision.Reload();
                Assert.IsFalse(system.WaitForLoaded());
                Assert.IsFalse(collision.WaitForLoaded());

                lifecycleStage = "refresh routes";
                AssetPipelineService.RefreshSources(new[] { emitterPath, systemPath, collisionPath });
                emitterItem = AssertAuthoredEditorRoute(emitterPath, emitterId, typeof(ParticleEmitter).FullName,
                    "Flax.GraphDocument", "BinaryAssetItem", "ParticleEmitterProxy", typeof(ParticleEmitterWindow));
                AssertAuthoredEditorRoute(systemPath, systemId, typeof(ParticleSystem).FullName,
                    "Flax.ParticleSystem", "ParticleSystemItem", "ParticleSystemProxy", typeof(ParticleSystemWindow));
                AssertAuthoredEditorRoute(collisionPath, collisionId, typeof(CollisionData).FullName,
                    "Flax.CollisionData", "CollisionDataItem", "CollisionDataProxy", typeof(CollisionDataWindow));
                AssetDocumentRegistry.OpenGraph<ParticleEmitter>(emitterItem, out var restartedEmitterSession);
                Assert.AreEqual(Path.GetFullPath(emitterPath), Path.GetFullPath(restartedEmitterSession.SourcePath));
                AssetDocumentRegistry.Close(emitterItem, ref restartedEmitterSession);
                Assert.IsTrue(FlaxEditor.Editor.GetCollisionDataOptions(collisionPath, out var collisionType, out var sourceModel,
                    out _, out _, out _, out _), "Collision options were not readable after reload.");
                Assert.AreEqual(CollisionDataType.TriangleMesh, collisionType);
                Assert.AreEqual(modelId, sourceModel);

                AssertAuthoredText(emitterPath, typeof(ParticleEmitter).FullName);
                AssertAuthoredText(systemPath, typeof(ParticleSystem).FullName);
                AssertAuthoredText(collisionPath, typeof(CollisionData).FullName);
                var ids = new[] { emitterId, systemId, collisionId };
                var paths = new[] { emitterPath, systemPath, collisionPath };
                var diagnostics = AssetDatabaseQueryService.GetDiagnostics().Where(x =>
                    x.Severity != AssetPipelineDiagnosticSeverity.Info &&
                    (ids.Contains(x.AssetGuid) || paths.Any(path => string.Equals(path, x.SourcePath, StringComparison.OrdinalIgnoreCase)))).ToArray();
                Assert.IsEmpty(diagnostics, string.Join(Environment.NewLine, diagnostics.Select(x => x.Code + ": " + x.Message)));
                lock (consoleDiagnostics)
                    Assert.IsEmpty(consoleDiagnostics, "emitter=" + emitterId + ", system=" + systemId +
                        ", collision=" + collisionId + ", model=" + modelId + Environment.NewLine +
                        string.Join(Environment.NewLine, consoleDiagnostics));
            }
            finally
            {
                Debug.LogMessageReceived -= captureConsoleDiagnostic;
                foreach (var path in new[] { emitterPath, systemPath, collisionPath, modelPath })
                    CleanupCanonicalCopyAsset(path);
                if (Directory.Exists(root))
                    Directory.Delete(root, true);
                AssetPipelineService.RefreshSources(new[] { root });
            }
        }

        public static int RunParticleAndCollisionAuthoredTextLifecycle()
        {
            new TestEditorUtils().TestParticleAndCollisionAuthoredTextLifecycle();
            return 0;
        }

        private static void WriteCurrentJsonDocument(string path, Guid id, string typeName, string source)
        {
            File.WriteAllText(path, source);
            File.WriteAllText(path + ".meta",
                "{\n" +
                "  \"fileFormatVersion\": 2,\n" +
                "  \"guid\": \"" + id.ToString("N") + "\",\n" +
                "  \"folderAsset\": false,\n" +
                "  \"importer\": { \"id\": \"Flax.JsonDocument\", \"version\": 1, \"settings\": {} },\n" +
                "  \"objectIds\": { \"main\": { \"fileId\": 1, \"type\": \"" + typeName + "\" } },\n" +
                "  \"labels\": [],\n" +
                "  \"userData\": {}\n" +
                "}\n");
        }

        private static void WaitForCurrentArtifact(Guid id, string caseName,
            System.Diagnostics.Stopwatch aggregateTimer, TimeSpan aggregateDeadline)
        {
            while (!AssetPipelineService.IsArtifactCurrent(id))
            {
                var status = AssetPipelineService.GetBuildStatus(id);
                Assert.IsFalse(status == "Failed" || status == "Cancelled" || status == "NotBuilt",
                    caseName + " build ended as " + status + ": " +
                    AssetPipelineService.GetBuildDiagnostic(id).Message);
                if (aggregateTimer.Elapsed >= aggregateDeadline)
                {
                    AssetPipelineService.CancelBuild(id);
                    Assert.Fail(caseName + " exceeded the aggregate cooker-validation deadline with status " + status + ".");
                }
                Thread.Sleep(1);
            }
        }

        [Test]
        public void TestSceneAndPrefabCurrentFormatCookerPaths()
        {
            var aggregateTimer = System.Diagnostics.Stopwatch.StartNew();
            var aggregateDeadline = TimeSpan.FromSeconds(30);
            var root = Path.Combine(Globals.ProjectContentFolder,
                "__ScenePrefabCook_" + Guid.NewGuid().ToString("N"));
            var scenePath = Path.Combine(root, "Fixture.scene");
            var prefabPath = Path.Combine(root, "Fixture.prefab");
            var sceneId = Guid.NewGuid();
            var prefabId = Guid.NewGuid();
            var paths = new[] { scenePath, prefabPath };
            var ids = new[] { sceneId, prefabId };
            var stage = "setup";
            try
            {
                CurrentFormatCookerValidation.Stage("scene/prefab", "source write begin", aggregateTimer,
                    aggregateDeadline);
                Directory.CreateDirectory(root);
                WriteCurrentJsonDocument(scenePath, sceneId, typeof(SceneAsset).FullName,
                    "{\"sceneVersion\":4,\"objects\":[{\"fileId\":1,\"type\":\"FlaxEngine.Scene\"},{\"fileId\":2,\"type\":\"FlaxEngine.EmptyActor\",\"parentFileId\":1}]}");
                WriteCurrentJsonDocument(prefabPath, prefabId, typeof(Prefab).FullName,
                    "{\"prefabVersion\":4,\"objects\":[{\"fileId\":1,\"type\":\"FlaxEngine.EmptyActor\"}]}");
                CurrentFormatCookerValidation.Stage("scene/prefab", "source write end", aggregateTimer,
                    aggregateDeadline);
                stage = "registration";
                CurrentFormatCookerValidation.Stage("scene/prefab", "registration begin", aggregateTimer,
                    aggregateDeadline);
                Assert.IsFalse(AssetPipelineService.RefreshSources(paths, false));
                CurrentFormatCookerValidation.Stage("scene/prefab", "registration end", aggregateTimer,
                    aggregateDeadline);
                for (var i = 0; i < ids.Length; i++)
                {
                    var registrationDiagnostics = AssetDatabaseQueryService.GetDiagnostics().Where(x =>
                        paths.Any(path => string.Equals(path, x.SourcePath, StringComparison.OrdinalIgnoreCase))).ToArray();
                    Assert.IsTrue(AssetDatabaseQueryService.TryGetMainRecordAtPath(paths[i], out var record),
                        "Current-format JSON document was not registered: " + paths[i] + Environment.NewLine +
                        string.Join(Environment.NewLine, registrationDiagnostics.Select(x => x.Code + ": " + x.Message)));
                    Assert.AreEqual("Flax.JsonDocument", record.ProcessorID);
                    ids[i] = record.ID;
                }
                stage = "artifact builds";
                for (var i = 0; i < ids.Length; i++)
                {
                    var caseName = i == 0 ? "current-format scene" : "current-format prefab";
                    CurrentFormatCookerValidation.Stage("scene/prefab", caseName + " build request", aggregateTimer,
                        aggregateDeadline);
                    Assert.IsFalse(AssetPipelineService.BuildAssetForeground(ids[i]));
                    Assert.AreNotEqual("NotBuilt", AssetPipelineService.GetBuildStatus(ids[i]),
                        caseName + " foreground build was not dispatched to the registered JSON document processor: " +
                        AssetPipelineService.GetBuildDiagnostic(ids[i]).Message);
                    WaitForCurrentArtifact(ids[i], caseName, aggregateTimer, aggregateDeadline);
                    CurrentFormatCookerValidation.Stage("scene/prefab", caseName + " build ready", aggregateTimer,
                        aggregateDeadline);
                }

                stage = "scene cooker";
                CurrentFormatCookerValidation.AssertCooks(ids[0], "current-format scene", aggregateTimer,
                    aggregateDeadline);
                stage = "prefab cooker";
                CurrentFormatCookerValidation.AssertCooks(ids[1], "current-format prefab", aggregateTimer,
                    aggregateDeadline);

                Assert.IsTrue(AssetDatabaseQueryService.TryGetRecord(ids[0], out var sceneRecord));
                Assert.AreEqual(typeof(SceneAsset).FullName, sceneRecord.TypeName);
                Assert.IsTrue(AssetDatabaseQueryService.TryGetRecord(ids[1], out var prefabRecord));
                Assert.AreEqual(typeof(Prefab).FullName, prefabRecord.TypeName);
                var diagnostics = AssetDatabaseQueryService.GetDiagnostics().Where(x =>
                    x.Severity != AssetPipelineDiagnosticSeverity.Info &&
                    (ids.Contains(x.AssetGuid) || paths.Any(path =>
                        string.Equals(path, x.SourcePath, StringComparison.OrdinalIgnoreCase)))).ToArray();
                Assert.IsEmpty(diagnostics, string.Join(Environment.NewLine,
                    diagnostics.Select(x => x.Code + ": " + x.Message)));
            }
            catch (Exception ex)
            {
                throw new InvalidOperationException("Current-format scene/prefab cooker validation failed during " + stage + ".", ex);
            }
            finally
            {
                if (Directory.Exists(root))
                    Directory.Delete(root, true);
                AssetPipelineService.RefreshSources(new[] { root });
            }
        }

        public static int RunSceneAndPrefabCurrentFormatCookerPaths()
        {
            new TestEditorUtils().TestSceneAndPrefabCurrentFormatCookerPaths();
            return 0;
        }

        [Test]
        public void TestAdditionalAuthoredTextFamiliesLifecycle()
        {
            var root = Path.Combine(Globals.ProjectContentFolder, "__AdditionalAuthoredLifecycle_" + Guid.NewGuid().ToString("N"));
            var materialPath = Path.Combine(root, "Surface.material");
            var materialFunctionPath = Path.Combine(root, "MaterialFunction.materialfunction");
            var animationGraphPath = Path.Combine(root, "Animation.animgraph");
            var animationFunctionPath = Path.Combine(root, "AnimationFunction.animgraphfunction");
            var visualScriptPath = Path.Combine(root, "Logic.visualscript");
            var behaviorTreePath = Path.Combine(root, "Behavior.behaviortree");
            var particleFunctionPath = Path.Combine(root, "ParticleFunction.particlefunction");
            var materialInstancePath = Path.Combine(root, "SurfaceInstance.materialinstance");
            var skeletonMaskPath = Path.Combine(root, "Mask.skeletonmask");
            var sceneAnimationPath = Path.Combine(root, "Timeline.sceneanimation");
            var paths = new[]
            {
                materialPath, materialFunctionPath, animationGraphPath, animationFunctionPath, visualScriptPath,
                behaviorTreePath, particleFunctionPath, materialInstancePath, skeletonMaskPath, sceneAnimationPath,
            };
            var consoleDiagnostics = new List<string>();
            var lifecycleStage = "setup";
            LogMessageDelegate captureConsoleDiagnostic = (level, message, stackTrace, threadId) =>
            {
                if (level == LogType.Warning && message.Contains("Missing Base Model asset for the Animation Graph"))
                    return;
                if (level == LogType.Warning || level == LogType.Error || level == LogType.Fatal)
                {
                    lock (consoleDiagnostics)
                        consoleDiagnostics.Add("[" + lifecycleStage + "] " + level + ": " + message);
                }
            };
            Debug.LogMessageReceived += captureConsoleDiagnostic;
            try
            {
                Directory.CreateDirectory(root);
                lifecycleStage = "create";
                var materialId = AssetDocumentRegistry.CreateGraph(materialPath, typeof(Material).FullName);
                var materialFunctionId = AssetDocumentRegistry.CreateGraph(materialFunctionPath, typeof(MaterialFunction).FullName);
                var animationGraphId = AssetDocumentRegistry.CreateGraph(animationGraphPath, typeof(AnimationGraph).FullName);
                var animationFunctionId = AssetDocumentRegistry.CreateGraph(animationFunctionPath, typeof(AnimationGraphFunction).FullName);
                var visualScriptId = AssetDocumentRegistry.CreateGraph(visualScriptPath, typeof(VisualScript).FullName);
                var behaviorTreeId = AssetDocumentRegistry.CreateGraph(behaviorTreePath, typeof(BehaviorTree).FullName);
                var particleFunctionId = AssetDocumentRegistry.CreateGraph(particleFunctionPath, typeof(ParticleEmitterFunction).FullName);
                var materialInstanceId = AuthoredAssetDocumentService.Create(materialInstancePath, typeof(MaterialInstance).FullName);
                var skeletonMaskId = AuthoredAssetDocumentService.Create(skeletonMaskPath, typeof(SkeletonMask).FullName);
                var sceneAnimationId = AuthoredAssetDocumentService.Create(sceneAnimationPath, typeof(SceneAnimation).FullName);
                var ids = new[]
                {
                    materialId, materialFunctionId, animationGraphId, animationFunctionId, visualScriptId,
                    behaviorTreeId, particleFunctionId, materialInstanceId, skeletonMaskId, sceneAnimationId,
                };
                Assert.IsFalse(ids.Contains(Guid.Empty), string.Join(Environment.NewLine,
                    AssetDatabaseQueryService.GetDiagnostics().Select(x => x.Code + ": " + x.Message)));

                lifecycleStage = "initial refresh";
                Assert.IsFalse(AssetPipelineService.RefreshSources(paths));
                lifecycleStage = "editor routes";
                var materialItem = AssertAuthoredEditorRoute(materialPath, materialId, typeof(Material).FullName,
                    "Flax.GraphDocument", "BinaryAssetItem", "MaterialProxy", typeof(MaterialWindow));
                var materialFunctionItem = AssertAuthoredEditorRoute(materialFunctionPath, materialFunctionId, typeof(MaterialFunction).FullName,
                    "Flax.GraphDocument", "BinaryAssetItem", "MaterialFunctionProxy", typeof(MaterialFunctionWindow));
                var animationGraphItem = AssertAuthoredEditorRoute(animationGraphPath, animationGraphId, typeof(AnimationGraph).FullName,
                    "Flax.GraphDocument", "BinaryAssetItem", "AnimationGraphProxy", typeof(AnimationGraphWindow));
                var animationFunctionItem = AssertAuthoredEditorRoute(animationFunctionPath, animationFunctionId, typeof(AnimationGraphFunction).FullName,
                    "Flax.GraphDocument", "BinaryAssetItem", "AnimationGraphFunctionProxy", typeof(AnimationGraphFunctionWindow));
                var visualScriptItem = AssertAuthoredEditorRoute(visualScriptPath, visualScriptId, typeof(VisualScript).FullName,
                    "Flax.GraphDocument", "VisualScriptItem", "VisualScriptProxy", typeof(VisualScriptWindow));
                var behaviorTreeItem = AssertAuthoredEditorRoute(behaviorTreePath, behaviorTreeId, typeof(BehaviorTree).FullName,
                    "Flax.GraphDocument", "BinaryAssetItem", "BehaviorTreeProxy", typeof(BehaviorTreeWindow));
                var particleFunctionItem = AssertAuthoredEditorRoute(particleFunctionPath, particleFunctionId, typeof(ParticleEmitterFunction).FullName,
                    "Flax.GraphDocument", "BinaryAssetItem", "ParticleEmitterFunctionProxy", typeof(ParticleEmitterFunctionWindow));
                AssertAuthoredEditorRoute(materialInstancePath, materialInstanceId, typeof(MaterialInstance).FullName,
                    "Flax.MaterialInstance", "BinaryAssetItem", "MaterialInstanceProxy", typeof(MaterialInstanceWindow));
                AssertAuthoredEditorRoute(skeletonMaskPath, skeletonMaskId, typeof(SkeletonMask).FullName,
                    "Flax.SkeletonMask", "BinaryAssetItem", "SkeletonMaskProxy", typeof(SkeletonMaskWindow));
                AssertAuthoredEditorRoute(sceneAnimationPath, sceneAnimationId, typeof(SceneAnimation).FullName,
                    "Flax.SceneAnimation", "SceneAnimationItem", "SceneAnimationProxy", typeof(SceneAnimationWindow));

                lifecycleStage = "material thumbnail publication ordering";
                var materialThumbnailOwner = new ThumbnailOwner();
                materialItem.Thumbnail = FlaxEditor.Editor.Instance.Icons.Document128;
                materialItem.AddReference(materialThumbnailOwner, true);
                AssetDocumentRegistry.OpenGraph<Material>(materialItem, out var materialSession);
                try
                {
                    materialSession.SetGraphSurface(MoveMaterialGraphNode(materialSession.GetGraphSurface(), new Float2(9, 4)));
                    Assert.IsFalse(materialSession.SaveGraph(materialItem));
                    Assert.IsFalse(materialItem.IsThumbnailRequestQueuedForTesting,
                        "Material save queued a thumbnail before its runtime artifact was hot-swapped.");
                    ApplyPublishedArtifact(materialId);
                    Assert.IsTrue(materialItem.IsThumbnailRequestQueuedForTesting,
                        "Published material artifact did not renew visible thumbnail demand.");
                    Assert.IsTrue(materialItem.Thumbnail.IsValid,
                        "Material publication discarded the last-good thumbnail before replacement.");
                }
                finally
                {
                    AssetDocumentRegistry.Close(materialItem, ref materialSession);
                    materialItem.RemoveReference(materialThumbnailOwner);
                }

                lifecycleStage = "graph edit save reload";
                RoundTripAuthoredGraph<Material>(materialItem);
                RoundTripAuthoredGraph<MaterialFunction>(materialFunctionItem);
                RoundTripAuthoredGraph<AnimationGraph>(animationGraphItem);
                RoundTripAuthoredGraph<AnimationGraphFunction>(animationFunctionItem);
                RoundTripAuthoredGraph<VisualScript>(visualScriptItem);
                RoundTripAuthoredGraph<BehaviorTree>(behaviorTreeItem);
                RoundTripAuthoredGraph<ParticleEmitterFunction>(particleFunctionItem);

                lifecycleStage = "authored edit save";
                var material = FlaxEngine.Content.LoadAssetAsync<Material>(materialId);
                var materialInstance = FlaxEngine.Content.LoadAssetAsync<MaterialInstance>(materialInstanceId);
                Assert.NotNull(material);
                Assert.NotNull(materialInstance);
                Assert.IsFalse(material.WaitForLoaded());
                Assert.IsFalse(materialInstance.WaitForLoaded());
                materialInstance.BaseMaterial = material;
                Assert.IsFalse(FlaxEditor.Editor.Instance.ContentDatabase.SaveAsset(materialInstance));
                var mask = FlaxEngine.Content.LoadAssetAsync<SkeletonMask>(skeletonMaskId);
                Assert.NotNull(mask);
                Assert.IsFalse(mask.WaitForLoaded());
                Assert.IsFalse(FlaxEditor.Editor.Instance.ContentDatabase.SaveAsset(mask));
                var timeline = AuthoredAssetDocumentService.LoadSceneAnimationTimeline(sceneAnimationPath);
                Assert.IsFalse(FlaxEditor.Editor.Instance.ContentDatabase.SaveAsset(sceneAnimationPath,
                    () => AuthoredAssetDocumentService.SaveSceneAnimationTimeline(sceneAnimationPath, timeline)));

                lifecycleStage = "initial builds";
                foreach (var id in ids)
                {
                    Assert.IsFalse(AssetPipelineService.BuildAssetForeground(id));
                    WaitForCurrentArtifact(id);
                }
                Assert.IsTrue(AssetDatabaseQueryService.GetDependencies(materialInstanceId).Any(x => x.TargetObject == materialId),
                    "Material instance did not retain its persistent base-material dependency.");

                lifecycleStage = "runtime dependency stability";
                var materialJson = JObject.Parse(File.ReadAllText(materialPath));
                var materialProperties = (JObject)materialJson["properties"];
                Assert.NotNull(materialProperties);
                materialProperties["maskThreshold"] = ((float?)materialProperties["maskThreshold"] ?? 0.3f) + 0.01f;
                RoundTripAuthoredGraph<Material>(materialItem, materialProperties.ToString());
                Assert.IsFalse(AssetPipelineService.BuildAssetForeground(materialId));
                Assert.IsTrue(AssetPipelineService.IsArtifactCurrent(materialInstanceId),
                    "Runtime-only material reference unexpectedly invalidated its owner's build artifact.");

                lifecycleStage = "runtime reload";
                AssertAuthoredRuntimeType<Material>(materialId);
                AssertAuthoredRuntimeType<MaterialFunction>(materialFunctionId);
                AssertAuthoredRuntimeType<AnimationGraph>(animationGraphId);
                AssertAuthoredRuntimeType<AnimationGraphFunction>(animationFunctionId);
                AssertAuthoredRuntimeType<VisualScript>(visualScriptId);
                AssertAuthoredRuntimeType<BehaviorTree>(behaviorTreeId);
                AssertAuthoredRuntimeType<ParticleEmitterFunction>(particleFunctionId);
                AssertAuthoredRuntimeType<MaterialInstance>(materialInstanceId);
                AssertAuthoredRuntimeType<SkeletonMask>(skeletonMaskId);
                AssertAuthoredRuntimeType<SceneAnimation>(sceneAnimationId);

                lifecycleStage = "restart equivalent refresh";
                Assert.IsFalse(AssetPipelineService.RefreshSources(paths));
                materialItem = AssertAuthoredEditorRoute(materialPath, materialId, typeof(Material).FullName,
                    "Flax.GraphDocument", "BinaryAssetItem", "MaterialProxy", typeof(MaterialWindow));
                materialFunctionItem = AssertAuthoredEditorRoute(materialFunctionPath, materialFunctionId, typeof(MaterialFunction).FullName,
                    "Flax.GraphDocument", "BinaryAssetItem", "MaterialFunctionProxy", typeof(MaterialFunctionWindow));
                animationGraphItem = AssertAuthoredEditorRoute(animationGraphPath, animationGraphId, typeof(AnimationGraph).FullName,
                    "Flax.GraphDocument", "BinaryAssetItem", "AnimationGraphProxy", typeof(AnimationGraphWindow));
                animationFunctionItem = AssertAuthoredEditorRoute(animationFunctionPath, animationFunctionId, typeof(AnimationGraphFunction).FullName,
                    "Flax.GraphDocument", "BinaryAssetItem", "AnimationGraphFunctionProxy", typeof(AnimationGraphFunctionWindow));
                visualScriptItem = AssertAuthoredEditorRoute(visualScriptPath, visualScriptId, typeof(VisualScript).FullName,
                    "Flax.GraphDocument", "VisualScriptItem", "VisualScriptProxy", typeof(VisualScriptWindow));
                behaviorTreeItem = AssertAuthoredEditorRoute(behaviorTreePath, behaviorTreeId, typeof(BehaviorTree).FullName,
                    "Flax.GraphDocument", "BinaryAssetItem", "BehaviorTreeProxy", typeof(BehaviorTreeWindow));
                particleFunctionItem = AssertAuthoredEditorRoute(particleFunctionPath, particleFunctionId, typeof(ParticleEmitterFunction).FullName,
                    "Flax.GraphDocument", "BinaryAssetItem", "ParticleEmitterFunctionProxy", typeof(ParticleEmitterFunctionWindow));
                AssertAuthoredEditorRoute(materialInstancePath, materialInstanceId, typeof(MaterialInstance).FullName,
                    "Flax.MaterialInstance", "BinaryAssetItem", "MaterialInstanceProxy", typeof(MaterialInstanceWindow));
                AssertAuthoredEditorRoute(skeletonMaskPath, skeletonMaskId, typeof(SkeletonMask).FullName,
                    "Flax.SkeletonMask", "BinaryAssetItem", "SkeletonMaskProxy", typeof(SkeletonMaskWindow));
                AssertAuthoredEditorRoute(sceneAnimationPath, sceneAnimationId, typeof(SceneAnimation).FullName,
                    "Flax.SceneAnimation", "SceneAnimationItem", "SceneAnimationProxy", typeof(SceneAnimationWindow));
                AssertAuthoredGraphReopens<Material>(materialItem);
                AssertAuthoredGraphReopens<MaterialFunction>(materialFunctionItem);
                AssertAuthoredGraphReopens<AnimationGraph>(animationGraphItem);
                AssertAuthoredGraphReopens<AnimationGraphFunction>(animationFunctionItem);
                AssertAuthoredGraphReopens<VisualScript>(visualScriptItem);
                AssertAuthoredGraphReopens<BehaviorTree>(behaviorTreeItem);
                AssertAuthoredGraphReopens<ParticleEmitterFunction>(particleFunctionItem);

                for (var i = 0; i < paths.Length; i++)
                    AssertAuthoredText(paths[i], AssetDatabaseQueryService.TryGetRecord(ids[i], out var record) ? record.TypeName : string.Empty);
                var diagnostics = AssetDatabaseQueryService.GetDiagnostics().Where(x =>
                    x.Severity != AssetPipelineDiagnosticSeverity.Info &&
                    (ids.Contains(x.AssetGuid) || paths.Any(path => string.Equals(path, x.SourcePath, StringComparison.OrdinalIgnoreCase)))).ToArray();
                Assert.IsEmpty(diagnostics, string.Join(Environment.NewLine, diagnostics.Select(x => x.Code + ": " + x.Message)));
                lock (consoleDiagnostics)
                    Assert.IsEmpty(consoleDiagnostics, string.Join(Environment.NewLine, consoleDiagnostics));
            }
            finally
            {
                Debug.LogMessageReceived -= captureConsoleDiagnostic;
                foreach (var path in paths)
                    CleanupCanonicalCopyAsset(path);
                if (Directory.Exists(root))
                    Directory.Delete(root, true);
                AssetPipelineService.RefreshSources(new[] { root });
            }
        }

        public static int RunAdditionalAuthoredTextFamiliesLifecycle()
        {
            new TestEditorUtils().TestAdditionalAuthoredTextFamiliesLifecycle();
            return 0;
        }

        [Test]
        public void TestConcurrentImportThroughEditorFacingApis()
        {
            var root = Path.Combine(Globals.ProjectContentFolder, "__ConcurrentImportUi_" + Guid.NewGuid().ToString("N"));
            var paths = Enumerable.Range(0, 3).Select(i => Path.Combine(root, "Texture" + i + ".png"))
                .Concat(Enumerable.Range(0, 3).Select(i => Path.Combine(root, "Model" + i + ".glb"))).ToArray();
            var ids = new List<Guid>();
            var consoleDiagnostics = new List<string>();
            var stage = "setup";
            var total = System.Diagnostics.Stopwatch.StartNew();
            LogMessageDelegate capture = (level, message, stackTrace, threadId) =>
            {
                if (level == LogType.Warning || level == LogType.Error || level == LogType.Fatal)
                    lock (consoleDiagnostics)
                        consoleDiagnostics.Add("[" + stage + "] " + level + ": " + message);
            };
            Debug.LogMessageReceived += capture;
            try
            {
                Directory.CreateDirectory(root);
                var png = Convert.FromBase64String("iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mNk+A8AAQUBAScY42YAAAAASUVORK5CYII=");
                var triangle = Convert.FromBase64String("AAAAAAAAAAAAAAAAAACAPwAAAAAAAAAAAAAAAAAAgD8AAAAAAAABAAIAAAA=");
                var gltf = "{\"asset\":{\"version\":\"2.0\"},\"buffers\":[{\"byteLength\":44}],\"bufferViews\":[{\"buffer\":0,\"byteOffset\":0,\"byteLength\":36,\"target\":34962},{\"buffer\":0,\"byteOffset\":36,\"byteLength\":6,\"target\":34963}],\"accessors\":[{\"bufferView\":0,\"componentType\":5126,\"count\":3,\"type\":\"VEC3\",\"max\":[1,1,0],\"min\":[0,0,0]},{\"bufferView\":1,\"componentType\":5123,\"count\":3,\"type\":\"SCALAR\"}],\"meshes\":[{\"primitives\":[{\"attributes\":{\"POSITION\":0},\"indices\":1}]}],\"nodes\":[{\"mesh\":0}],\"scenes\":[{\"nodes\":[0]}],\"scene\":0}";
                var json = System.Text.Encoding.UTF8.GetBytes(gltf);
                var paddedJsonLength = (json.Length + 3) & ~3;
                byte[] glb;
                using (var stream = new MemoryStream())
                using (var writer = new BinaryWriter(stream))
                {
                    writer.Write(0x46546c67u);
                    writer.Write(2u);
                    writer.Write((uint)(12 + 8 + paddedJsonLength + 8 + triangle.Length));
                    writer.Write((uint)paddedJsonLength);
                    writer.Write(0x4e4f534au);
                    writer.Write(json);
                    for (var i = json.Length; i < paddedJsonLength; i++)
                        writer.Write((byte)' ');
                    writer.Write((uint)triangle.Length);
                    writer.Write(0x004e4942u);
                    writer.Write(triangle);
                    glb = stream.ToArray();
                }

                stage = "metadata";
                for (var i = 0; i < paths.Length; i++)
                {
                    File.WriteAllBytes(paths[i], i < 3 ? png : glb);
                    var id = i < 3
                        ? TextureImporterService.CreateMetadata(paths[i], FlaxEngine.Tools.TextureTool.Options.Default)
                        : ModelImporterService.CreateDefaultMetadata(paths[i]);
                    Assert.AreNotEqual(Guid.Empty, id);
                    ids.Add(id);
                }
                Assert.IsFalse(AssetPipelineService.RefreshSources(paths));

                stage = "overlapping builds";
                var overlapWitnessId = ids[1];
                var cancelledId = ids[2];
                AssetPipelineService.SetBuildPausedForTesting(overlapWitnessId, true);
                AssetPipelineService.SetBuildPausedForTesting(cancelledId, true);
                foreach (var id in ids)
                    Assert.IsFalse(AssetPipelineService.BuildAsset(id));
                while (AssetPipelineService.GetBuildStatus(overlapWitnessId) != "Building" ||
                       AssetPipelineService.GetBuildStatus(cancelledId) != "Building")
                {
                    Assert.Less(total.ElapsedMilliseconds, 5 * 60 * 1000, "Concurrent import witnesses did not start before the hard abort ceiling.");
                    Thread.Sleep(1);
                }
                var statusLatency = System.Diagnostics.Stopwatch.StartNew();
                var initialStatuses = ids.Select(AssetPipelineService.GetBuildStatus).ToArray();
                Assert.Less(statusLatency.ElapsedMilliseconds, 1000, "Human-facing build status queries stalled Editor interaction.");
                Assert.IsFalse(AssetPipelineService.CancelBuild(cancelledId));
                AssetPipelineService.SetBuildPausedForTesting(overlapWitnessId, false);
                AssetPipelineService.SetBuildPausedForTesting(cancelledId, false);
                var peakActive = initialStatuses.Count(status => status == "Queued" || status == "Building" || status == "Publishing");
                while (true)
                {
                    var active = ids.Count(id =>
                    {
                        var status = AssetPipelineService.GetBuildStatus(id);
                        return status == "Queued" || status == "Building" || status == "Publishing";
                    });
                    peakActive = Math.Max(peakActive, active);
                    if (active == 0)
                        break;
                    Assert.Less(total.ElapsedMilliseconds, 5 * 60 * 1000, "Concurrent importer cohort exceeded the hard abort ceiling.");
                    Thread.Sleep(2);
                }
                Assert.GreaterOrEqual(peakActive, 2, "The reduced cohort never exposed overlapping human-facing job states.");
                Assert.AreEqual("Cancelled", AssetPipelineService.GetBuildStatus(cancelledId));
                foreach (var id in ids.Where(id => id != cancelledId))
                    Assert.AreEqual("ReadyExact", AssetPipelineService.GetBuildStatus(id), AssetPipelineService.GetBuildDiagnostic(id).Message);

                stage = "selection";
                var selectionLatency = System.Diagnostics.Stopwatch.StartNew();
                var records = AssetWorkspaceQuery.QueryAllRecords(new AssetDatabaseQuery { PathPrefix = root, MainAssetsOnly = true });
                Assert.AreEqual(paths.Length, records.Length);
                var item = FlaxEditor.Editor.Instance.ContentDatabase.FindAsset(ids[0]);
                Assert.NotNull(item);
                var contentWindow = FlaxEditor.Editor.Instance.Windows.ContentWin;
                contentWindow.Select(item, true);
                Assert.AreSame(item, contentWindow.Selection.Single());
                contentWindow.ClearSelection(false);
                Assert.Less(selectionLatency.ElapsedMilliseconds, 1000, "Project selection stalled while import results were presented.");

                stage = "cached restart";
                AssetPipelineService.DrainArtifactPublications();
                Assert.IsFalse(AssetPipelineService.Shutdown());
                Assert.IsFalse(AssetPipelineService.Initialize());
                Assert.IsFalse(AssetPipelineService.LoadOrScan());
                Assert.IsTrue(AssetPipelineService.IsArtifactCurrent(ids[0]));
                Assert.IsFalse(AssetPipelineService.BuildAsset(ids[0]));
                Assert.IsEmpty(AssetPipelineService.DrainArtifactPublications(), "Cached restart unexpectedly imported again.");

                stage = "external remove and re-add";
                var retainedSource = File.ReadAllBytes(paths[0]);
                var retainedMeta = File.ReadAllBytes(paths[0] + ".meta");
                File.Delete(paths[0]);
                File.Delete(paths[0] + ".meta");
                var removal = System.Diagnostics.Stopwatch.StartNew();
                Assert.IsFalse(AssetPipelineService.RefreshSources(new[] { paths[0] }));
                Assert.Less(removal.ElapsedMilliseconds, 1000);
                Assert.IsFalse(AssetDatabaseQueryService.TryGetMainRecordAtPath(paths[0], out _));
                File.WriteAllBytes(paths[0], retainedSource);
                File.WriteAllBytes(paths[0] + ".meta", retainedMeta);
                Assert.IsFalse(AssetPipelineService.RefreshSources(new[] { paths[0] }));
                Assert.AreEqual(ids[0], AssetDatabaseQueryService.AssetPathToGUID(paths[0]));
                AssetPipelineService.DrainArtifactPublications();
                Assert.IsFalse(AssetPipelineService.BuildAsset(ids[0]));
                Assert.IsTrue(AssetPipelineService.IsArtifactCurrent(ids[0]));
                Assert.IsEmpty(AssetPipelineService.DrainArtifactPublications(), "Cached re-add unexpectedly imported again.");

                var diagnostics = AssetDatabaseQueryService.GetDiagnostics().Where(x =>
                    x.Severity != AssetPipelineDiagnosticSeverity.Info &&
                    (ids.Contains(x.AssetGuid) || paths.Contains(x.SourcePath, StringComparer.OrdinalIgnoreCase))).ToArray();
                Assert.IsEmpty(diagnostics, string.Join(Environment.NewLine, diagnostics.Select(x => x.Code + ": " + x.Message)));
                lock (consoleDiagnostics)
                    Assert.IsEmpty(consoleDiagnostics, string.Join(Environment.NewLine, consoleDiagnostics));
                Assert.Less(total.ElapsedMilliseconds, 5 * 60 * 1000);
            }
            finally
            {
                if (ids.Count > 2)
                {
                    AssetPipelineService.SetBuildPausedForTesting(ids[1], false);
                    AssetPipelineService.SetBuildPausedForTesting(ids[2], false);
                }
                Debug.LogMessageReceived -= capture;
                foreach (var path in paths)
                    CleanupCanonicalCopyAsset(path);
                if (Directory.Exists(root))
                    Directory.Delete(root, true);
                AssetPipelineService.RefreshSources(new[] { root });
            }
        }

        public static int RunConcurrentImportThroughEditorFacingApis()
        {
            new TestEditorUtils().TestConcurrentImportThroughEditorFacingApis();
            return 0;
        }

        [Test]
        public void TestProjectModelDropPersistsAcrossSaveReopenAndSceneCopy()
        {
            var token = Guid.NewGuid().ToString("N");
            var scenePath = Path.Combine(Globals.ProjectContentFolder, "__ModelDropScene_" + token + ".scene");
            var copyPath = Path.Combine(Globals.ProjectContentFolder, "__ModelDropSceneCopy_" + token + ".scene");
            var diagnostics = new List<string>();
            Scene sourceScene = null;
            Scene copiedScene = null;
            LogMessageDelegate capture = (level, message, stackTrace, threadId) =>
            {
                if (level == LogType.Warning || level == LogType.Error || level == LogType.Fatal)
                    diagnostics.Add(level + ": " + message);
            };
            Debug.LogMessageReceived += capture;
            try
            {
                FlaxEditor.CliAuthoringCommands.CreateScene(scenePath);
                Assert.IsFalse(AssetPipelineService.RefreshSources(new[] { scenePath }, false), "Created scene source refresh failed.");
                Assert.IsTrue(AssetDatabaseQueryService.TryGetMainRecordAtPath(scenePath, out var sourceRecord), "Created scene was not registered.");
                Assert.IsFalse(AssetPipelineService.RebuildAsset(sourceRecord.ID, true), "Created scene artifact build failed.");
                Assert.IsTrue(AssetPipelineService.IsArtifactCurrent(sourceRecord.ID), "Created scene artifact was not current.");
                Assert.IsFalse(Level.LoadScene(sourceRecord.ID), "Created scene failed to load.");
                sourceScene = Level.FindScene(sourceRecord.ID);
                Assert.NotNull(sourceScene);
                FlaxEditor.Editor.Instance.Scene.SetActiveScene(sourceScene);

                var model = FlaxEngine.Content.LoadAsyncInternal<Model>("Editor/Primitives/Cube");
                Assert.NotNull(model);
                Assert.IsFalse(model.WaitForLoaded());
                var modelId = model.ID;
                var modelItem = new ModelItem(model.Path, ref modelId, typeof(Model).FullName, typeof(Model));
                Assert.IsTrue(SceneTreeWindow.PlaceAssetItems(new AssetItem[] { modelItem }, null, out var placedNodes), "Project-panel drop logic rejected the model.");
                Assert.AreEqual(1, placedNodes.Count);
                var placed = (placedNodes[0] as FlaxEditor.SceneGraph.ActorNode)?.Actor as StaticModel;
                Assert.NotNull(placed);
                var placedName = placed.Name;
                Assert.AreSame(model, placed.Model);
                Assert.IsTrue(FlaxEditor.Editor.Instance.Scene.IsEdited(sourceScene));

                Assert.IsTrue(FlaxEditor.Editor.Instance.Scene.SaveSceneSynchronously(sourceScene), "Synchronous scene save failed or left the scene dirty.");
                Assert.IsFalse(FlaxEditor.Editor.Instance.Scene.IsEdited(sourceScene));
                Assert.IsTrue(AssetPipelineService.IsArtifactCurrent(sourceRecord.ID), "Scene save did not publish the current artifact.");
                var savedSource = File.ReadAllText(scenePath);
                var savedDocument = JObject.Parse(savedSource);
                var savedActor = (savedDocument["objects"] as JArray)?.OfType<JObject>().FirstOrDefault(x =>
                    x.Value<long>("fileId") == placed.LocalFileId &&
                    x.Value<string>("type") == typeof(StaticModel).FullName);
                Assert.NotNull(savedActor, "Saved scene source omitted the Project-panel actor's persistent local file ID.");
                var savedModelToken = savedActor.SelectToken("$..Model");
                var savedModelText = savedModelToken?.Type == JTokenType.String
                    ? savedModelToken.Value<string>()
                    : savedModelToken?["guid"]?.Value<string>();
                Assert.IsTrue(AssetGuid.TryParse(savedModelText, out var savedModelGuid), "Saved scene source omitted the dropped model reference.");
                Assert.AreEqual(modelId, savedModelGuid.Value, "Saved scene source changed the dropped model identity.");
                Assert.IsFalse(Level.UnloadScene(sourceScene));
                sourceScene = null;
                Scripting.FlushRemovedObjects();
                Assert.IsFalse(Level.LoadScene(sourceRecord.ID));
                sourceScene = Level.FindScene(sourceRecord.ID);
                Assert.NotNull(sourceScene, "Saved source scene was not loaded for reopen.");
                var reopenedActor = sourceScene.FindActor(placedName);
                Assert.NotNull(reopenedActor, "Project-panel actor name was missing from the reloaded scene hierarchy.");
                Assert.IsInstanceOf<StaticModel>(reopenedActor, "Reloaded Project-panel actor had the wrong concrete type.");
                var reopenedModel = (StaticModel)reopenedActor;
                Assert.AreEqual(modelId, reopenedModel.Model.ID, "Reopened actor did not retain the dropped model identity.");

                var sourceItem = FlaxEditor.Editor.Instance.ContentDatabase.FindAsset(sourceRecord.ID) as AssetItem;
                if (sourceItem == null)
                {
                    sourceItem = new SceneItem(scenePath, sourceRecord.ID);
                    sourceItem.SetAssetDatabaseRecord(sourceRecord);
                }
                var copyResult = FlaxEditor.Editor.Instance.ContentDatabase.Copy(sourceItem, copyPath);
                Assert.IsTrue(copyResult.Succeeded, copyResult.Message);
                Assert.IsTrue(AssetDatabaseQueryService.TryGetMainRecordAtPath(copyPath, out var copyRecord));
                Assert.AreNotEqual(sourceRecord.ID, copyRecord.ID);
                Assert.IsFalse(AssetPipelineService.RebuildAsset(copyRecord.ID, true));
                Assert.IsFalse(Level.LoadScene(copyRecord.ID));
                copiedScene = Level.FindScene(copyRecord.ID);
                Assert.NotNull(copiedScene?.FindActor<StaticModel>(placedName));
                var copiedSky = copiedScene.FindActor<Sky>("Sky");
                var copiedSun = copiedScene.FindActor<DirectionalLight>("Sun");
                Assert.NotNull(copiedSky);
                Assert.NotNull(copiedSun);
                Assert.AreSame(copiedSun, copiedSky.SunLight);
                Assert.AreNotSame(sourceScene.FindActor<DirectionalLight>("Sun"), copiedSky.SunLight);

                Assert.IsFalse(Level.UnloadScene(sourceScene));
                Assert.IsFalse(Level.UnloadScene(copiedScene));
                sourceScene = null;
                copiedScene = null;
                Scripting.FlushRemovedObjects();
                Assert.IsFalse(AssetPipelineService.Shutdown());
                Assert.IsFalse(AssetPipelineService.Initialize());
                Assert.IsFalse(AssetPipelineService.LoadOrScan());
                Assert.IsFalse(Level.LoadScene(copyRecord.ID));
                copiedScene = Level.FindScene(copyRecord.ID);
                Assert.NotNull(copiedScene?.FindActor<StaticModel>(placedName));
                Assert.AreSame(copiedScene.FindActor<DirectionalLight>("Sun"), copiedScene.FindActor<Sky>("Sky").SunLight);
                Assert.IsEmpty(diagnostics, string.Join(Environment.NewLine, diagnostics));
            }
            finally
            {
                Debug.LogMessageReceived -= capture;
                if (sourceScene != null)
                    Level.UnloadScene(sourceScene);
                if (copiedScene != null)
                    Level.UnloadScene(copiedScene);
                Scripting.FlushRemovedObjects();
                if (File.Exists(copyPath))
                    AssetOperationService.DeleteAsset(copyPath);
                if (File.Exists(scenePath))
                    AssetOperationService.DeleteAsset(scenePath);
                AssetPipelineService.RefreshSources(new[] { scenePath, copyPath }, false);
            }
        }

        public static int RunProjectModelDropPersistsAcrossSaveReopenAndSceneCopy()
        {
            new TestEditorUtils().TestProjectModelDropPersistsAcrossSaveReopenAndSceneCopy();
            return 0;
        }

        [Test]
        public void TestModelItemRejectsUnavailableExactArtifact()
        {
            var id = Guid.NewGuid();
            var item = new ModelItem(Path.Combine(Globals.ProjectContentFolder, "Missing.flax"), ref id,
                typeof(Model).FullName, typeof(Model));

            Assert.IsNull(item.OnEditorDrop(null));
        }

        [Test]
        public void TestModelItemPlacesResolvedModel()
        {
            var model = FlaxEngine.Content.LoadAsyncInternal<Model>("Editor/Primitives/Cube");
            Assert.IsNotNull(model);
            var id = model.ID;
            var item = new ModelItem(model.Path, ref id, typeof(Model).FullName, typeof(Model));
            var actor = item.OnEditorDrop(null) as StaticModel;
            try
            {
                Assert.IsNotNull(actor);
                Assert.AreSame(model, actor.Model);
            }
            finally
            {
                if (actor != null)
                    FlaxEngine.Object.Destroy(actor);
            }
        }

    }
}
#endif
