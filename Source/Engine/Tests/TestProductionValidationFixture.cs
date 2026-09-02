// Copyright (c) Wojciech Figat. All rights reserved.

#if FLAX_TESTS
using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Text;
using System.Threading;
using FlaxEditor.Content;
using NUnit.Framework;

namespace FlaxEngine.Tests
{
    [TestFixture]
    public class TestProductionValidationFixture
    {
        [Test]
        public void TestStagesReducedCanonicalSourceCohortWithoutImporting()
        {
            string root;
            string externalActors;
            using (var fixture = ProductionValidationFixture.Create())
            {
                root = fixture.RootPath;
                externalActors = fixture.ExternalActorsPath;
                Assert.AreEqual(TimeSpan.FromMinutes(5), ProductionValidationFixture.ImportTimeout);
                Assert.AreEqual(7, fixture.SourcePaths.Count);
                Assert.AreEqual(fixture.SourcePaths.Count, fixture.AssetIds.Count);
                foreach (var path in fixture.SourcePaths)
                {
                    Assert.IsTrue(File.Exists(path), path);
                    Assert.IsTrue(File.Exists(path + ".meta"), path + ".meta");
                }

                var glb = File.ReadAllBytes(fixture.ModelPath);
                Assert.Greater(glb.Length, 20);
                Assert.AreEqual(0x46546c67u, BitConverter.ToUInt32(glb, 0));
                Assert.AreEqual(2u, BitConverter.ToUInt32(glb, 4));
                Assert.AreEqual((uint)glb.Length, BitConverter.ToUInt32(glb, 8));

                var indexPath = Path.Combine(externalActors, "scene-fragments.index");
                var fragmentPath = Path.Combine(externalActors, "00", "2.sceneactor");
                Assert.IsTrue(File.Exists(indexPath));
                Assert.IsTrue(File.Exists(fragmentPath));
                StringAssert.Contains(fixture.ExternalSceneId.ToString("N"), File.ReadAllText(indexPath));
                StringAssert.Contains("\"rootActorLocalId\": 2", File.ReadAllText(fragmentPath));
            }

            Assert.IsFalse(Directory.Exists(root));
            Assert.IsFalse(Directory.Exists(externalActors));
        }

        [Test]
        public void TestRepresentativeModelFamilyLifecycleAndCook()
        {
            var timer = System.Diagnostics.Stopwatch.StartNew();
            string root = null;
            ProductionValidationFixture fixture = null;
            var stage = "initial registration";
            try
            {
                fixture = ProductionValidationFixture.Create();
                root = fixture.RootPath;
                stage = "placeholder removal";
                File.Delete(fixture.ModelPath);
                File.Delete(fixture.ModelPath + ".meta");
                Assert.IsFalse(AssetPipelineService.RefreshSources(new[] { fixture.ModelPath }, false));
                stage = "expanded family import";
                WriteTwoMeshGlb(fixture.ModelPath, false);
                var modelId = CreateAuthoredModelMetadata(fixture);
                stage = "authored family refresh";
                Assert.IsFalse(AssetPipelineService.RefreshSources(new[] { fixture.ModelPath }, false));
                Assert.IsTrue(AssetDatabaseQueryService.TryGetRecord(modelId, out var authoredMain),
                    "Authored model root was not indexed: " + string.Join(" | ",
                        AssetDatabaseQueryService.GetDiagnostics().Select(x => x.Code + ": " + x.Message)));
                Assert.IsTrue(string.Equals(Path.GetFullPath(fixture.ModelPath), Path.GetFullPath(authoredMain.SourcePath),
                    StringComparison.OrdinalIgnoreCase));
                stage = "authored family build request";
                Assert.IsFalse(AssetPipelineService.BuildAsset(modelId));
                stage = "authored family build completion";
                ProductionValidationFixture.WaitForImports(new[] { modelId });
                Assert.Less(timer.Elapsed, ProductionValidationFixture.ImportTimeout);

                var records = GetModelRecords(fixture);
                Assert.GreaterOrEqual(records.Length, 5, "The representative GLB did not expose its model family: " +
                    string.Join(", ", records.Select(x => x.SubAssetKey + "=" + x.TypeName)));
                var main = records.Single(x => x.IsMain);
                var children = records.Where(x => !x.IsMain).ToArray();
                Assert.AreEqual(modelId, main.ID);
                Assert.IsTrue(IsModelType(main.TypeName), "Unexpected model root type: " + main.TypeName);
                Assert.IsTrue(children.All(x => x.ID != Guid.Empty && x.SourceAssetID == modelId));
                Assert.IsTrue(children.Any(x => IsModelType(x.TypeName)), "Mesh subasset type was missing.");
                Assert.IsTrue(children.Any(x => x.TypeName == typeof(Material).FullName), "Material subasset type was missing.");
                Assert.IsTrue(children.Any(x => x.TypeName == typeof(Animation).FullName), "Animation subasset type was missing.");
                Assert.IsTrue(records.All(x => x.TypeName != typeof(RawDataAsset).FullName), "Model family fell back to RawData.");
                CollectionAssert.AllItemsAreUnique(records.Select(x => x.ID).ToArray());
                CollectionAssert.AllItemsAreUnique(records.Select(x => x.LocalId).ToArray());
                var sourceDependency = AssetDatabaseQueryService.GetDependencies(main.ID)
                    .Single(x => x.Kind == "SourceFile");
                Assert.IsNotEmpty(sourceDependency.ContentHash,
                    "Model publication did not retain its canonical source dependency hash.");
                var initialSourceHash = sourceDependency.ContentHash;

                var stableChildren = children.ToDictionary(x => x.SubAssetKey, x => x.ID, StringComparer.Ordinal);
                Assert.GreaterOrEqual(stableChildren.Count, 2);
                foreach (var record in records)
                    AssertConcreteProjectType(record);
                AssertModelProjectRoute(main, true);
                AssertModelProjectRoute(children.First(x => IsModelType(x.TypeName)), false);

                stage = "runtime load and cook";
                var model = FlaxEngine.Content.LoadAssetAsync<ModelBase>(main.ID);
                Assert.NotNull(model);
                Assert.IsFalse(model.WaitForLoaded());
                Assert.Greater(model.LODsCount, 0);
                model.Reload();
                Assert.IsFalse(model.WaitForLoaded());
                var cookValidationTimer = System.Diagnostics.Stopwatch.StartNew();
                var cookValidationDeadline = TimeSpan.FromSeconds(30);
                CurrentFormatCookerValidation.AssertCooks(main.ID, "model root", cookValidationTimer,
                    cookValidationDeadline);
                var meshSubAsset = children.First(x => IsModelType(x.TypeName));
                CurrentFormatCookerValidation.AssertCooks(meshSubAsset.ID, "model mesh subasset",
                    cookValidationTimer, cookValidationDeadline);
                var animationSubAsset = children.First(x => x.TypeName == typeof(Animation).FullName);
                CurrentFormatCookerValidation.AssertCooks(animationSubAsset.ID, "model animation subasset",
                    cookValidationTimer, cookValidationDeadline);

                stage = "source reorder invalidation";
                WriteTwoMeshGlb(fixture.ModelPath, true);
                Assert.IsFalse(AssetPipelineService.RefreshSources(new[] { fixture.ModelPath }));
                Assert.IsFalse(AssetPipelineService.IsArtifactCurrent(main.ID), "Source change did not invalidate the model artifact.");
                Assert.IsTrue(children.Any(x => !AssetPipelineService.IsArtifactCurrent(x.ID)),
                    "Source change did not invalidate any model subasset artifact.");
                var reordered = GetModelRecords(fixture);
                foreach (var child in reordered.Where(x => !x.IsMain))
                    Assert.AreEqual(stableChildren[child.SubAssetKey], child.ID, "Mesh reorder changed a persistent subasset GUID.");
                Assert.IsFalse(AssetPipelineService.BuildAsset(modelId));
                ProductionValidationFixture.WaitForImports(new[] { modelId });
                var reorderedSourceDependency = AssetDatabaseQueryService.GetDependencies(main.ID)
                    .Single(x => x.Kind == "SourceFile");
                Assert.AreNotEqual(initialSourceHash, reorderedSourceDependency.ContentHash,
                    "Model source rewrite did not update its dependency hash.");

                stage = "pipeline restart";
                FlaxEditor.Editor.Instance.Windows.ContentWin.ClearSelection(false);
                FlaxEngine.Content.UnloadAsset(model);
                model = null;
                AssetPipelineService.DrainArtifactPublications();
                Assert.IsFalse(AssetPipelineService.Shutdown());
                Assert.IsFalse(AssetPipelineService.Initialize());
                Assert.IsFalse(AssetPipelineService.LoadOrScan());
                var restarted = GetModelRecords(fixture);
                CollectionAssert.AreEquivalent(reordered.Select(x => x.ID).ToArray(), restarted.Select(x => x.ID).ToArray());
                Assert.IsTrue(restarted.All(x => x.TypeName != typeof(RawDataAsset).FullName));
                Assert.IsTrue(AssetPipelineService.IsArtifactCurrent(restarted.Single(x => x.IsMain).ID));
                Assert.IsTrue(restarted.All(x => AssetPipelineService.GetBuildStatus(x.ID) == "ReadyExact"),
                    "Restart left non-ready model-family objects: " + string.Join(", ", restarted
                        .Select(x => x.SubAssetKey + "=" + AssetPipelineService.GetBuildStatus(x.ID))));
                Assert.IsFalse(AssetPipelineService.BuildAsset(modelId));
                Assert.IsEmpty(AssetPipelineService.DrainArtifactPublications(), "Restart unexpectedly reimported the unchanged GLB family.");
                AssertModelProjectRoute(restarted.Single(x => x.IsMain), true);
                Assert.Less(timer.Elapsed, ProductionValidationFixture.ImportTimeout);
            }
            catch (Exception ex)
            {
                throw new InvalidOperationException("Representative model lifecycle failed during " + stage + ".", ex);
            }
            finally
            {
                FlaxEditor.Editor.Instance.Windows.ContentWin.ClearSelection(false);
                fixture?.Dispose();
                if (root != null)
                    AssetPipelineService.RefreshSources(new[] { root });
            }
        }

        private static AssetDatabaseRecordInfo[] GetModelRecords(ProductionValidationFixture fixture)
        {
            return AssetDatabaseQueryService.QueryRecords(new AssetDatabaseQuery
            {
                PathPrefix = fixture.RootPath,
                Limit = 64,
            }).Where(x => x.ProcessorID == "Flax.Model").ToArray();
        }

        private static void AssertModelProjectRoute(AssetDatabaseRecordInfo record, bool expectImportProxy)
        {
            var item = FlaxEditor.Editor.Instance.ContentDatabase.FindAsset(record.ID);
            Assert.NotNull(item, "Project did not expose model object " + record.ID);
            Assert.IsTrue(item is ModelItem || item is SkinnedModeItem,
                "Project exposed an unexpected model item type: " + item.GetType().FullName);
            Assert.IsFalse(item.IsOfType<RawDataAsset>());
            FlaxEditor.Editor.Instance.Windows.ContentWin.Select(item, true);
            Assert.AreSame(item, FlaxEditor.Editor.Instance.Windows.ContentWin.Selection.Single());
            var inspected = FlaxEditor.Editor.Instance.Windows.PropertiesWin.Presenter.Selection;
            Assert.AreEqual(1, inspected.Count, "Inspector did not bind the selected model object.");
            if (expectImportProxy)
                StringAssert.Contains("ModelImportAssetPropertiesProxy", inspected[0].GetType().Name);
            else
                Assert.IsInstanceOf<ModelBase>(inspected[0]);
        }

        private static void AssertConcreteProjectType(AssetDatabaseRecordInfo record)
        {
            var item = FlaxEditor.Editor.Instance.ContentDatabase.FindAsset(record.ID);
            Assert.NotNull(item, "Project did not expose model-family object " + record.ID);
            Assert.IsInstanceOf<BinaryAssetItem>(item);
            Assert.IsFalse(item.IsOfType<RawDataAsset>());
            if (IsModelType(record.TypeName))
            {
                Assert.IsTrue(item is ModelItem || item is SkinnedModeItem);
                Assert.AreEqual(ContentItemSearchFilter.Model, item.SearchFilter);
            }
            else if (record.TypeName == typeof(Material).FullName)
            {
                Assert.IsTrue(item.IsOfType<Material>());
                Assert.AreEqual(ContentItemSearchFilter.Material, item.SearchFilter);
            }
            else if (record.TypeName == typeof(Animation).FullName)
            {
                Assert.IsTrue(item.IsOfType<Animation>());
                Assert.AreEqual(ContentItemSearchFilter.Animation, item.SearchFilter);
            }
        }

        private static bool IsModelType(string typeName)
        {
            return typeName == typeof(Model).FullName || typeName == typeof(SkinnedModel).FullName;
        }

        private static Guid CreateAuthoredModelMetadata(ProductionValidationFixture fixture)
        {
            File.Delete(fixture.ModelPath + ".meta");
            var modelId = ModelImporterService.CreateDefaultMetadata(fixture.ModelPath);
            Assert.AreNotEqual(Guid.Empty, modelId, "Model metadata creation did not author the complete family.");
            return modelId;
        }

        private static void WriteTwoMeshGlb(string path, bool reversed)
        {
            var meshes = reversed
                ? "{\"name\":\"Beta\",\"primitives\":[{\"attributes\":{\"POSITION\":0},\"indices\":1,\"material\":0}]},{\"name\":\"Alpha\",\"primitives\":[{\"attributes\":{\"POSITION\":0},\"indices\":1,\"material\":0}]}"
                : "{\"name\":\"Alpha\",\"primitives\":[{\"attributes\":{\"POSITION\":0},\"indices\":1,\"material\":0}]},{\"name\":\"Beta\",\"primitives\":[{\"attributes\":{\"POSITION\":0},\"indices\":1,\"material\":0}]}";
            var nodes = reversed
                ? "{\"name\":\"Beta\",\"mesh\":0},{\"name\":\"Alpha\",\"mesh\":1}"
                : "{\"name\":\"Alpha\",\"mesh\":0},{\"name\":\"Beta\",\"mesh\":1}";
            var json = "{\"asset\":{\"version\":\"2.0\"},\"buffers\":[{\"byteLength\":76}]," +
                       "\"bufferViews\":[{\"buffer\":0,\"byteOffset\":0,\"byteLength\":36,\"target\":34962},{\"buffer\":0,\"byteOffset\":36,\"byteLength\":6,\"target\":34963},{\"buffer\":0,\"byteOffset\":44,\"byteLength\":8},{\"buffer\":0,\"byteOffset\":52,\"byteLength\":24}]," +
                       "\"accessors\":[{\"bufferView\":0,\"componentType\":5126,\"count\":3,\"type\":\"VEC3\",\"max\":[1,1,0],\"min\":[0,0,0]},{\"bufferView\":1,\"componentType\":5123,\"count\":3,\"type\":\"SCALAR\"},{\"bufferView\":2,\"componentType\":5126,\"count\":2,\"type\":\"SCALAR\",\"max\":[1],\"min\":[0]},{\"bufferView\":3,\"componentType\":5126,\"count\":2,\"type\":\"VEC3\"}]," +
                       "\"materials\":[{\"name\":\"FixtureMaterial\",\"pbrMetallicRoughness\":{\"baseColorFactor\":[1,0,0,1]}}],\"meshes\":[" + meshes + "],\"nodes\":[" + nodes + "]," +
                       "\"animations\":[{\"name\":\"Idle\",\"samplers\":[{\"input\":2,\"output\":3,\"interpolation\":\"LINEAR\"}],\"channels\":[{\"sampler\":0,\"target\":{\"node\":0,\"path\":\"translation\"}}]}],\"scenes\":[{\"nodes\":[0,1]}],\"scene\":0}";
            var jsonBytes = new UTF8Encoding(false).GetBytes(json);
            var paddedJsonLength = (jsonBytes.Length + 3) & ~3;
            using var stream = File.Create(path);
            using var writer = new BinaryWriter(stream, Encoding.UTF8);
            writer.Write(0x46546c67u);
            writer.Write(2u);
            writer.Write((uint)(12 + 8 + paddedJsonLength + 8 + 76));
            writer.Write((uint)paddedJsonLength);
            writer.Write(0x4e4f534au);
            writer.Write(jsonBytes);
            for (var i = jsonBytes.Length; i < paddedJsonLength; i++)
                writer.Write((byte)' ');
            writer.Write(76u);
            writer.Write(0x004e4942u);
            foreach (var value in new[] { 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f })
                writer.Write(value);
            writer.Write((ushort)0);
            writer.Write((ushort)1);
            writer.Write((ushort)2);
            writer.Write((ushort)0);
            writer.Write(0.0f);
            writer.Write(1.0f);
            writer.Write(0.0f);
            writer.Write(0.0f);
            writer.Write(0.0f);
            writer.Write(1.0f);
            writer.Write(0.0f);
            writer.Write(0.0f);
        }

        public static int RunStagesReducedCanonicalSourceCohortWithoutImporting()
        {
            new TestProductionValidationFixture().TestStagesReducedCanonicalSourceCohortWithoutImporting();
            return 0;
        }

        public static int RunRepresentativeModelFamilyLifecycleAndCook()
        {
            new TestProductionValidationFixture().TestRepresentativeModelFamilyLifecycleAndCook();
            return 0;
        }
    }
}
#endif
