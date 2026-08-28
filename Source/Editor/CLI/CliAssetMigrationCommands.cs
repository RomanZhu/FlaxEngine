// Copyright (c) Wojciech Figat. All rights reserved.

using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using FlaxEditor.Content;
using FlaxEngine;
using FlaxEngine.Tools;

namespace FlaxEditor
{
    /// <summary>Project-wide legacy source extraction commands.</summary>
    public static class CliAssetMigrationCommands
    {
        /// <summary>Extracts eligible legacy project assets into canonical sources while preserving identities.</summary>
        [CliCommand("assets.migration.convert-project", Description = "Convert eligible legacy project assets to text, PNG/DDS, and GLB sources.", Access = CliCommandAccess.MutatesProject)]
        public static CliCommandOperation ConvertProject()
        {
            return new ProjectMigrationOperation();
        }

        private sealed class ProjectMigrationOperation : CliCommandOperation
        {
            private enum Phase
            {
                Discover,
                StageImported,
                ConvertText,
                Publish,
                WaitForBuilds,
                Validate,
                Finalize,
                Completed,
            }

            private sealed class Entry
            {
                public string LegacyPath;
                public string DestinationPath;
                public string BackupPath;
                public string ExtractedPath;
                public Guid ID;
                public bool IsModel;
                public string TypeName;
                public int Width;
                public int Height;
                public int Meshes;
                public int Triangles;
                public string[] MaterialSlots;
                public Guid[] Materials;
            }

            private readonly List<string> _text = new();
            private readonly List<Entry> _imported = new();
            private readonly List<string> _skipped = new();
            private Phase _phase;
            private int _index;
            private CliCommandResult _result;

            public override bool IsCompleted => _phase == Phase.Completed;
            public override CliCommandResult Result => _result;

            public override void Update(TimeSpan timeBudget)
            {
                try
                {
                    switch (_phase)
                    {
                    case Phase.Discover:
                        Discover();
                        _phase = Phase.StageImported;
                        break;
                    case Phase.StageImported:
                        if (_index < _imported.Count)
                            Stage(_imported[_index++]);
                        else
                        {
                            _index = 0;
                            _phase = Phase.ConvertText;
                        }
                        break;
                    case Phase.ConvertText:
                        if (_index < _text.Count)
                        {
                            var path = _text[_index++];
                            if (AssetDatabaseFacade.MigrateLegacyAsset(path))
                                throw new InvalidOperationException($"Text conversion failed for '{path}'.");
                        }
                        else
                        {
                            _index = 0;
                            _phase = Phase.Publish;
                        }
                        break;
                    case Phase.Publish:
                        Publish();
                        _phase = Phase.WaitForBuilds;
                        break;
                    case Phase.WaitForBuilds:
                        if (CheckBuilds())
                        {
                            _index = 0;
                            _phase = Phase.Validate;
                        }
                        break;
                    case Phase.Validate:
                        if (_index < _imported.Count)
                            Validate(_imported[_index++]);
                        else
                        {
                            _index = 0;
                            _phase = Phase.Finalize;
                        }
                        break;
                    case Phase.Finalize:
                        if (_index < _imported.Count)
                        {
                            var entry = _imported[_index++];
                            if (AssetDatabaseFacade.FinalizeLegacyImportedMigration(entry.BackupPath))
                                throw new IOException($"Could not remove verified migration backup '{entry.BackupPath}'.");
                        }
                        else
                            Complete();
                        break;
                    }
                }
                catch (Exception ex)
                {
                    var diagnostics = AssetDatabaseFacade.GetDiagnostics();
                    RollbackImported();
                    _result = CliCommandResult.Failure("FLX-ASSET-PROJECT-MIGRATION-0006", ex.Message, diagnostics);
                    _phase = Phase.Completed;
                }
            }

            public override void Cancel()
            {
                RollbackImported();
                _result = CliCommandResult.Failure("FLX-ASSET-PROJECT-MIGRATION-CANCELLED", "Project asset migration was cancelled.");
                _phase = Phase.Completed;
            }

            private void Discover()
            {
                if (AssetDatabaseFacade.LoadOrScan(false))
                    throw new InvalidOperationException("Asset database scan failed before migration.");
                foreach (var path in Directory.EnumerateFiles(Globals.ProjectContentFolder, "*.flax", SearchOption.AllDirectories).OrderBy(x => x, StringComparer.OrdinalIgnoreCase))
                {
                    if (!FlaxEngine.Content.GetAssetInfo(path, out var info))
                    {
                        _skipped.Add(path);
                        continue;
                    }
                    var filename = Path.GetFileNameWithoutExtension(path);
                    var generatedCsg = filename.StartsWith("CSG_", StringComparison.OrdinalIgnoreCase);
                    var terrain = path.Contains($"{Path.DirectorySeparatorChar}SceneData{Path.DirectorySeparatorChar}", StringComparison.OrdinalIgnoreCase) &&
                                  path.Contains($"{Path.DirectorySeparatorChar}Terrain{Path.DirectorySeparatorChar}", StringComparison.OrdinalIgnoreCase);
                    if (generatedCsg || terrain || info.TypeName == typeof(RawDataAsset).FullName)
                    {
                        _skipped.Add(path);
                        continue;
                    }
                    if (IsTexture(info.TypeName) || info.TypeName == typeof(Model).FullName)
                    {
                        _imported.Add(new Entry { LegacyPath = path, ID = info.ID, IsModel = info.TypeName == typeof(Model).FullName, TypeName = info.TypeName });
                        continue;
                    }
                    if (IsTextType(info.TypeName))
                    {
                        _text.Add(path);
                        continue;
                    }
                    _skipped.Add(path);
                }
            }

            private void Stage(Entry entry)
            {
                var asset = FlaxEngine.Content.LoadAsync<Asset>(entry.ID);
                if (asset == null || asset.WaitForLoaded())
                    throw new InvalidOperationException($"Legacy asset failed to load: '{entry.LegacyPath}'.");
                CaptureBaseline(entry, asset);

                var temporaryFolder = Path.Combine(Globals.ProjectLibraryFolder, "Temp", "SourceMigrationExports", entry.ID.ToString("N"));
                Directory.CreateDirectory(temporaryFolder);
                if (Editor.Export(entry.LegacyPath, temporaryFolder))
                    throw new InvalidOperationException($"Legacy asset export failed: '{entry.LegacyPath}'.");
                var extension = entry.IsModel ? ".glb" : entry.TypeName == typeof(CubeTexture).FullName ? ".dds" : ".png";
                entry.ExtractedPath = Path.Combine(temporaryFolder, Path.GetFileNameWithoutExtension(entry.LegacyPath) + extension);
                if (!File.Exists(entry.ExtractedPath))
                    throw new FileNotFoundException("Asset exporter did not produce the expected source file.", entry.ExtractedPath);
                entry.DestinationPath = Path.ChangeExtension(entry.LegacyPath, extension);
                entry.BackupPath = Path.Combine(Globals.ProjectLibraryFolder, "MigrationBackups", entry.ID.ToString("N") + ".flax");
                FlaxEngine.Content.UnloadAsset(asset);
                FlaxEngine.Scripting.FlushRemovedObjects();

                Guid staged;
                if (entry.IsModel)
                {
                    var options = new ModelTool.Options();
                    Editor.TryRestoreImportOptions(ref options, entry.LegacyPath);
                    Normalize(ref options);
                    staged = AssetDatabaseFacade.StageLegacyModelMigration(entry.LegacyPath, entry.ExtractedPath, entry.DestinationPath, entry.BackupPath, options);
                }
                else
                {
                    var options = TextureTool.Options.Default;
                    if (!Editor.TryRestoreImportOptions(ref options, entry.LegacyPath))
                        throw new InvalidOperationException($"Legacy texture settings could not be restored: '{entry.LegacyPath}'.");
                    Normalize(ref options);
                    staged = AssetDatabaseFacade.StageLegacyTextureMigration(entry.LegacyPath, entry.ExtractedPath, entry.DestinationPath, entry.BackupPath, options);
                }
                if (staged != entry.ID)
                    throw new InvalidOperationException($"GUID-preserving staging failed for '{entry.LegacyPath}' (expected {entry.ID:N}, staged {staged:N}).");
            }

            private void Publish()
            {
                if (AssetDatabaseFacade.Scan(false))
                    throw new InvalidOperationException("Canonical asset database publication failed.");
                foreach (var entry in _imported.Where(x => x.IsModel))
                {
                    if (AssetDatabaseFacade.ReconcileModel(entry.ID))
                        throw new InvalidOperationException($"Model subasset reconciliation failed for '{entry.DestinationPath}'.");
                }
                foreach (var entry in _imported)
                {
                    var failed = entry.IsModel ? AssetDatabaseFacade.BuildModel(entry.ID) : AssetDatabaseFacade.BuildTexture(entry.ID);
                    if (failed)
                        throw new InvalidOperationException($"Canonical asset build request failed for '{entry.DestinationPath}'.");
                }
            }

            private bool CheckBuilds()
            {
                var complete = true;
                foreach (var entry in _imported)
                {
                    var status = entry.IsModel ? AssetDatabaseFacade.GetModelBuildStatus(entry.ID) : AssetDatabaseFacade.GetTextureBuildStatus(entry.ID);
                    if (status == "Failed" || status == "Cancelled" || status == "NotBuilt")
                        throw new InvalidOperationException($"Canonical asset build {status} for '{entry.DestinationPath}'.");
                    complete &= status == "ReadyExact";
                }
                return complete;
            }

            private static void CaptureBaseline(Entry entry, Asset asset)
            {
                if (asset is Texture texture)
                {
                    entry.Width = texture.Width;
                    entry.Height = texture.Height;
                }
                else if (asset is Model model)
                {
                    entry.Meshes = model.LODs[0].Meshes.Length;
                    entry.Triangles = model.LODs[0].Meshes.Sum(x => x.TriangleCount);
                    entry.MaterialSlots = model.MaterialSlots.Select(x => x.Name).ToArray();
                    entry.Materials = model.MaterialSlots.Select(x => x.Material?.ID ?? Guid.Empty).ToArray();
                }
            }

            private static void Validate(Entry entry)
            {
                var asset = FlaxEngine.Content.LoadAsync<Asset>(entry.ID);
                if (asset == null || asset.WaitForLoaded())
                    throw new InvalidOperationException($"Migrated asset failed to load by preserved GUID: '{entry.DestinationPath}'.");
                if (asset.GetType().FullName != entry.TypeName)
                    throw new InvalidOperationException($"Migrated asset type changed for '{entry.DestinationPath}'.");
                if (asset is Texture texture && (texture.Width != entry.Width || texture.Height != entry.Height))
                    throw new InvalidOperationException($"Migrated texture dimensions changed for '{entry.DestinationPath}'.");
                if (asset is Model model)
                {
                    if (model.LODs.Length == 0 || model.LODs[0].Meshes.Length != entry.Meshes || model.LODs[0].Meshes.Sum(x => x.TriangleCount) != entry.Triangles)
                        throw new InvalidOperationException($"Migrated model LOD0 topology changed for '{entry.DestinationPath}'.");
                    var slots = model.MaterialSlots.Select(x => x.Name).ToArray();
                    var materials = model.MaterialSlots.Select(x => x.Material?.ID ?? Guid.Empty).ToArray();
                    if (!slots.SequenceEqual(entry.MaterialSlots) || !materials.SequenceEqual(entry.Materials))
                        throw new InvalidOperationException($"Migrated model material slots changed for '{entry.DestinationPath}'.");
                }
            }

            private void RollbackImported()
            {
                foreach (var entry in _imported.Where(x => !string.IsNullOrEmpty(x.BackupPath) && File.Exists(x.BackupPath)).Reverse())
                    AssetDatabaseFacade.RollbackLegacyImportedMigration(entry.LegacyPath, entry.DestinationPath, entry.BackupPath);
            }

            private void Complete()
            {
                AssetDatabaseFacade.Scan(false);
                _result = CliCommandResult.Success(new
                {
                    importedSources = _imported.Count,
                    textures = _imported.Count(x => !x.IsModel),
                    models = _imported.Count(x => x.IsModel),
                    textDocuments = _text.Count,
                    retainedBinaryAssets = _skipped.Count,
                    retained = _skipped,
                });
                _phase = Phase.Completed;
            }

            private static void Normalize(ref TextureTool.Options options)
            {
                options.FlipX = false;
                options.FlipY = false;
                options.InvertRedChannel = false;
                options.InvertGreenChannel = false;
                options.InvertBlueChannel = false;
                options.InvertAlphaChannel = false;
                options.Resize = false;
                options.Scale = 1.0f;
            }

            private static void Normalize(ref ModelTool.Options options)
            {
                options.Type = ModelTool.ModelType.Model;
                options.Scale = 1.0f;
                options.Rotation = Quaternion.Identity;
                options.Translation = Float3.Zero;
                options.UseLocalOrigin = false;
                options.CenterGeometry = false;
                options.FlipNormals = false;
                options.ReverseWindingOrder = false;
                options.CalculateNormals = false;
                options.CalculateTangents = true;
                options.OptimizeMeshes = false;
                options.MergeMeshes = false;
                options.ImportLODs = false;
                options.GenerateLODs = false;
                options.BaseLOD = 0;
                options.LODCount = 1;
                options.TriangleReduction = 0.5f;
                options.LODTargetError = 0.05f;
                options.LODPreserveUVsWeight = 0.01f;
                options.ImportMaterials = true;
                options.CreateEmptyMaterialSlots = false;
                options.ImportTextures = false;
                options.ObjectIndex = -1;
                if (options.SDFResolution < 0.0001f)
                    options.SDFResolution = 1.0f;
            }

            private static bool IsTexture(string typeName)
            {
                return typeName == typeof(Texture).FullName || typeName == typeof(SpriteAtlas).FullName || typeName == typeof(CubeTexture).FullName;
            }

            private static bool IsTextType(string typeName)
            {
                return typeName == typeof(Material).FullName || typeName == typeof(MaterialInstance).FullName || typeName == typeof(CollisionData).FullName ||
                       typeName == typeof(SkeletonMask).FullName || typeName == typeof(SceneAnimation).FullName || typeName == typeof(ParticleSystem).FullName ||
                       typeName == typeof(ParticleEmitter).FullName || typeName == typeof(VisualScript).FullName || typeName == typeof(Shader).FullName;
            }
        }
    }
}
