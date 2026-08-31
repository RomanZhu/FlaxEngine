// Copyright (c) Wojciech Figat. All rights reserved.

using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Security.Cryptography;
using System.Text;
using FlaxEditor.Content;
using FlaxEngine;

namespace FlaxEditor
{
    /// <summary>
    /// Read-only semantic asset inspection commands for automation and characterization tests.
    /// </summary>
    public static class CliAssetInspectionCommands
    {
        /// <summary>Strictly validates canonical sidecars without writing tracked project files.</summary>
        [CliCommand("assets.validate-metadata", Description = "Scan and strictly validate canonical asset metadata without tracked writes.", Access = CliCommandAccess.ReadOnly)]
        public static CliCommandResult ValidateMetadata()
        {
            var before = HashContentTree();
            var scanFailed = AssetPipelineService.Scan(true);
            var diagnostics = AssetDatabaseQueryService.GetDiagnostics();
            var records = AssetDatabaseQueryService.GetRecords();
            var after = HashContentTree();
            if (!before.SequenceEqual(after))
                return CliCommandResult.Failure("FLX-ASSET-METADATA-NOWRITE-0006", "Metadata validation changed tracked Content files.");

            var blockingStatuses = new HashSet<AssetRecordStatus>
            {
                AssetRecordStatus.DuplicateGuid,
                AssetRecordStatus.UnsupportedProcessor,
                AssetRecordStatus.MetaUpgradeRequired,
                AssetRecordStatus.SubAssetReconciliationRequired,
                AssetRecordStatus.MissingMeta,
                AssetRecordStatus.MalformedMeta,
                AssetRecordStatus.OrphanMeta,
                AssetRecordStatus.PathCollision,
            };
            var report = new
            {
                schemaVersion = 1,
                revision = AssetDatabaseQueryService.Revision,
                records = records.Length,
                blockingRecords = records.Where(x => blockingStatuses.Contains(x.Status)).Select(x => new
                {
                    guid = x.ID,
                    path = x.SourcePath,
                    status = x.Status.ToString(),
                }).ToArray(),
                diagnostics = diagnostics.Select(x => new
                {
                    code = GetDiagnosticToken(x.Code),
                    severity = x.Severity.ToString(),
                    stage = x.Stage.ToString(),
                    guid = x.AssetGuid,
                    path = x.SourcePath,
                    processor = x.ProcessorId,
                    line = x.Location.Line,
                    column = x.Location.Column,
                    message = x.Message,
                    remediation = x.Remediation,
                }).ToArray(),
                contentHash = Convert.ToHexString(after).ToLowerInvariant(),
            };
            if (scanFailed)
                return CliCommandResult.Failure("FLX-ASSET-METADATA-SCAN-0006", "Canonical metadata scan infrastructure failed.", report);
            if (diagnostics.Any(x => x.Severity == AssetPipelineDiagnosticSeverity.Error) || records.Any(x => blockingStatuses.Contains(x.Status)))
                return CliCommandResult.Failure("FLX-ASSET-METADATA-0004", "Canonical asset metadata validation failed.", report);
            return CliCommandResult.Success(report);
        }

        private static byte[] HashContentTree()
        {
            using var hash = IncrementalHash.CreateHash(HashAlgorithmName.SHA256);
            foreach (var path in Directory.EnumerateFiles(Globals.ProjectContentFolder, "*", SearchOption.AllDirectories).OrderBy(x => x, StringComparer.Ordinal))
            {
                var relative = Path.GetRelativePath(Globals.ProjectContentFolder, path).Replace(Path.DirectorySeparatorChar, '/');
                hash.AppendData(Encoding.UTF8.GetBytes(relative));
                using var stream = File.OpenRead(path);
                var buffer = new byte[64 * 1024];
                int read;
                while ((read = stream.Read(buffer, 0, buffer.Length)) != 0)
                    hash.AppendData(buffer, 0, read);
            }
            return hash.GetHashAndReset();
        }

        /// <summary>Safely clears generated Project Library data without touching Content.</summary>
        [CliCommand("assets.clean-library", Description = "Clear and recreate the validated Project Library root.", Access = CliCommandAccess.MutatesProject)]
        public static CliCommandResult CleanLibrary()
        {
            if (AssetPipelineService.CleanLibrary())
                return CliCommandResult.Failure("FLX-ASSET-LIBRARY-CLEAN-0006", "Project Library cleanup failed.", AssetDatabaseQueryService.GetDiagnostics());
            return CliCommandResult.Success(new { path = Globals.ProjectLibraryFolder });
        }

        /// <summary>Refreshes canonical sources and waits for requested artifact builds to complete.</summary>
        [CliCommand("assets.refresh", Description = "Refresh canonical assets and synchronously publish changed artifacts.", Access = CliCommandAccess.MutatesProject)]
        public static CliCommandResult RefreshAssets(
            [CliOption("force", Description = "Revalidate and rebuild assets even when their inputs are unchanged.")] bool force = false)
        {
            var options = ImportAssetOptions.ImportRecursive | ImportAssetOptions.ForceSynchronousImport;
            if (force)
                options |= ImportAssetOptions.ForceUpdate;
            if (AssetPipelineService.Refresh(options))
                return CliCommandResult.Failure("FLX-ASSET-REFRESH-0006", "Canonical asset refresh failed.", AssetDatabaseQueryService.GetDiagnostics());
            return CliCommandResult.Success(new
            {
                revision = AssetDatabaseQueryService.Revision,
                records = AssetDatabaseQueryService.GetRecords().Length,
                forced = force,
            });
        }

        private static string GetDiagnosticToken(AssetPipelineDiagnosticCode code)
        {
            return code switch
            {
                AssetPipelineDiagnosticCode.None => "ASSET_NONE",
                AssetPipelineDiagnosticCode.InvalidSettingsCombination => "ASSET_INVALID_SETTINGS_COMBINATION",
                AssetPipelineDiagnosticCode.DuplicateGuid => "ASSET_DUPLICATE_GUID",
                AssetPipelineDiagnosticCode.MissingMeta => "ASSET_META_MISSING",
                AssetPipelineDiagnosticCode.MetaParseError => "ASSET_META_PARSE_ERROR",
                AssetPipelineDiagnosticCode.MetaUpgradeRequired => "ASSET_META_UPGRADE_REQUIRED",
                AssetPipelineDiagnosticCode.InvalidMeta => "ASSET_INVALID_META",
                AssetPipelineDiagnosticCode.PathCollision => "ASSET_PATH_COLLISION",
                AssetPipelineDiagnosticCode.SourceMissing => "ASSET_SOURCE_MISSING",
                AssetPipelineDiagnosticCode.SourceBusy => "ASSET_SOURCE_BUSY",
                AssetPipelineDiagnosticCode.ProcessorMissing => "ASSET_PROCESSOR_MISSING",
                AssetPipelineDiagnosticCode.SubAssetReconcileRequired => "ASSET_SUBASSET_RECONCILE_REQUIRED",
                AssetPipelineDiagnosticCode.LibraryPathInvalid => "ASSET_LIBRARY_PATH_INVALID",
                AssetPipelineDiagnosticCode.LibraryCreationFailed => "ASSET_LIBRARY_CREATION_FAILED",
                AssetPipelineDiagnosticCode.BuildFailed => "ASSET_BUILD_FAILED",
                AssetPipelineDiagnosticCode.BuildCycle => "ASSET_BUILD_CYCLE",
                AssetPipelineDiagnosticCode.ArtifactMissing => "ASSET_ARTIFACT_MISSING",
                AssetPipelineDiagnosticCode.ArtifactInvalid => "ASSET_ARTIFACT_INVALID",
                AssetPipelineDiagnosticCode.ArtifactRebuildRequired => "ASSET_ARTIFACT_REBUILD_REQUIRED",
                AssetPipelineDiagnosticCode.SnapshotInvalid => "ASSET_SNAPSHOT_INVALID",
                AssetPipelineDiagnosticCode.MigrationFailed => "ASSET_MIGRATION_FAILED",
                _ => "ASSET_UNKNOWN_DIAGNOSTIC",
            };
        }

        /// <summary>
        /// Loads assets and returns stable, type-specific semantic summaries.
        /// </summary>
        [CliCommand("assets.inspect-many", Description = "Load assets and return stable semantic summaries.", Access = CliCommandAccess.ReadOnly)]
        public static object[] InspectMany(
            [CliOption("assets", Required = true)] string[] assets,
            [CliOption("reload", Description = "Reload Content items before inspection.")] bool reload = false)
        {
            if (assets == null || assets.Length == 0)
                throw new ArgumentException("At least one asset is required.", nameof(assets));
            return assets.Select(x => Inspect(x, reload)).ToArray();
        }

        /// <summary>Builds the exact host texture artifact without changing tracked settings.</summary>
        [CliCommand("assets.texture.build", Description = "Build an exact canonical texture artifact.", Access = CliCommandAccess.MutatesProject)]
        public static CliCommandOperation BuildTexture(
            [CliOption("asset", Required = true)] string asset,
            [CliOption("force", Description = "Revalidate and republish even when the key is unchanged.")] bool force = false)
        {
            return new TextureBuildOperation(ResolveAssetItem(asset), force);
        }

        private sealed class TextureBuildOperation : CliCommandOperation
        {
            private readonly AssetItem _item;
            private readonly bool _force;
            private CliCommandResult _result;
            private bool _requested;

            public TextureBuildOperation(AssetItem item, bool force)
            {
                _item = item;
                _force = force;
            }

            public override bool IsCompleted => _result != null;
            public override CliCommandResult Result => _result;

            public override void Update(TimeSpan timeBudget)
            {
                if (!_requested)
                {
                    _requested = true;
                    var failed = _force ? AssetPipelineService.RebuildAsset(_item.ID) : AssetPipelineService.BuildAsset(_item.ID);
                    if (failed)
                    {
                        _result = CliCommandResult.Failure("FLX-ASSET-TEXTURE-BUILD-0004", "Texture build request failed.", AssetPipelineService.GetBuildDiagnostic(_item.ID));
                        return;
                    }
                }
                var status = AssetPipelineService.GetBuildStatus(_item.ID);
                if (status == "ReadyExact")
                    _result = CliCommandResult.Success(new { id = _item.ID, path = _item.Path, status, forced = _force });
                else if (status == "Failed" || status == "Cancelled")
                    _result = CliCommandResult.Failure("FLX-ASSET-TEXTURE-BUILD-0004", "Texture build failed.", AssetPipelineService.GetBuildDiagnostic(_item.ID));
            }
        }

        /// <summary>Explicitly updates tracked stable child GUID mappings for a canonical model source.</summary>
        [CliCommand("assets.model.reconcile", Description = "Reconcile canonical model subasset GUID mappings.", Access = CliCommandAccess.MutatesProject)]
        public static CliCommandResult ReconcileModel([CliOption("asset", Required = true)] string asset)
        {
            var item = ResolveAssetItem(asset);
            if (ModelImporterService.ReconcileSubAssets(item.ID))
                return CliCommandResult.Failure("FLX-ASSET-MODEL-RECONCILE-0004", "Model subasset reconciliation failed.", AssetDatabaseQueryService.GetDiagnostics());
            var records = AssetDatabaseQueryService.GetRecords().Where(x => x.SourceAssetID == item.ID).OrderBy(x => x.SubAssetKey).Select(x => new
            {
                id = x.ID,
                sourceId = x.SourceAssetID,
                key = x.SubAssetKey,
                type = x.TypeName,
                removed = x.Status == AssetRecordStatus.MissingSource,
            }).ToArray();
            return CliCommandResult.Success(new { id = item.ID, path = item.Path, records });
        }

        /// <summary>Builds an exact host model or GUID-addressed model-owned child artifact.</summary>
        [CliCommand("assets.model.build", Description = "Build an exact canonical model artifact.", Access = CliCommandAccess.MutatesProject)]
        public static CliCommandOperation BuildModel(
            [CliOption("asset", Required = true)] string asset,
            [CliOption("force", Description = "Revalidate and republish even when the key is unchanged.")] bool force = false)
        {
            Guid id;
            string path;
            if (Guid.TryParse(asset, out id))
            {
                if (!FlaxEngine.Content.GetRuntimeAssetInfo(id, out var info))
                    throw new FileNotFoundException($"Asset '{asset}' was not found in the Content database.");
                path = info.Path;
            }
            else
            {
                var item = ResolveAssetItem(asset);
                id = item.ID;
                path = item.Path;
            }
            return new ModelBuildOperation(id, path, force);
        }

        private sealed class ModelBuildOperation : CliCommandOperation
        {
            private readonly Guid _id;
            private readonly string _path;
            private readonly bool _force;
            private CliCommandResult _result;
            private bool _requested;

            public ModelBuildOperation(Guid id, string path, bool force)
            {
                _id = id;
                _path = path;
                _force = force;
            }

            public override bool IsCompleted => _result != null;
            public override CliCommandResult Result => _result;

            public override void Update(TimeSpan timeBudget)
            {
                if (!_requested)
                {
                    _requested = true;
                    var failed = _force ? AssetPipelineService.RebuildAsset(_id) : AssetPipelineService.BuildAsset(_id);
                    if (failed)
                    {
                        _result = CliCommandResult.Failure("FLX-ASSET-MODEL-BUILD-0004", "Model build request failed.", AssetPipelineService.GetBuildDiagnostic(_id));
                        return;
                    }
                }
                var status = AssetPipelineService.GetBuildStatus(_id);
                if (status == "ReadyExact")
                    _result = CliCommandResult.Success(new { id = _id, path = _path, status, forced = _force });
                else if (status == "Failed" || status == "Cancelled")
                    _result = CliCommandResult.Failure("FLX-ASSET-MODEL-BUILD-0004", "Model build failed.", AssetPipelineService.GetBuildDiagnostic(_id));
            }
        }

        /// <summary>Extracts a model-owned material artifact as an independent legacy material with a new GUID.</summary>
        [CliCommand("assets.model.material.extract", Description = "Extract a model-owned material to an independent asset.", Access = CliCommandAccess.MutatesProject)]
        public static CliCommandOperation ExtractModelMaterial(
            [CliOption("asset", Required = true)] string asset,
            [CliOption("target", Required = true)] string target)
        {
            if (!Guid.TryParse(asset, out var id))
                id = ResolveAssetItem(asset).ID;
            var record = AssetDatabaseQueryService.GetRecords().FirstOrDefault(x => x.ID == id);
            if (record.ID == Guid.Empty || record.ProcessorID != "Flax.Model" || record.TypeName != typeof(Material).FullName || record.IsMain)
                throw new InvalidOperationException("Extract Material requires a live model-owned material child GUID.");
            var outputPath = Path.IsPathRooted(target) ? Path.GetFullPath(target) : Path.GetFullPath(Path.Combine(Globals.ProjectContentFolder, target));
            var relativeOutput = Path.GetRelativePath(Globals.ProjectContentFolder, outputPath);
            if (relativeOutput == ".." || relativeOutput.StartsWith(".." + Path.DirectorySeparatorChar, StringComparison.Ordinal))
                throw new InvalidOperationException("Extracted materials must be created inside the project Content directory.");
            if (!outputPath.EndsWith(".flax", StringComparison.OrdinalIgnoreCase))
                throw new InvalidOperationException("Extracted compatibility materials must use the .flax extension until P08 material documents are available.");
            if (File.Exists(outputPath))
                throw new IOException($"Extraction target '{outputPath}' already exists.");
            return new ExtractModelMaterialOperation(id, outputPath);
        }

        private sealed class ExtractModelMaterialOperation : CliCommandOperation
        {
            private readonly Guid _sourceId;
            private readonly string _targetPath;
            private readonly Guid _outputId = Guid.NewGuid();
            private CliCommandResult _result;
            private bool _requested;

            public ExtractModelMaterialOperation(Guid sourceId, string targetPath)
            {
                _sourceId = sourceId;
                _targetPath = targetPath;
            }

            public override bool IsCompleted => _result != null;
            public override CliCommandResult Result => _result;

            public override void Update(TimeSpan timeBudget)
            {
                if (!_requested)
                {
                    _requested = true;
                    if (AssetPipelineService.BuildAsset(_sourceId))
                    {
                        _result = CliCommandResult.Failure("FLX-ASSET-MODEL-EXTRACT-0004", "Model-owned material build request failed.", AssetPipelineService.GetBuildDiagnostic(_sourceId));
                        return;
                    }
                }
                var status = AssetPipelineService.GetBuildStatus(_sourceId);
                if (status == "Failed" || status == "Cancelled")
                {
                    _result = CliCommandResult.Failure("FLX-ASSET-MODEL-EXTRACT-0004", "Model-owned material build failed.", AssetPipelineService.GetBuildDiagnostic(_sourceId));
                    return;
                }
                if (status != "ReadyExact")
                    return;
                var material = FlaxEngine.Content.LoadRuntimeObjectAsync<Material>(_sourceId);
                if (material == null || material.WaitForLoaded())
                {
                    _result = CliCommandResult.Failure("FLX-ASSET-MODEL-EXTRACT-0004", "The exact model-owned material could not be loaded.");
                    return;
                }
                if (Editor.Instance.ContentEditing.CloneAssetFile(material.StoragePath, _targetPath, _outputId))
                {
                    _result = CliCommandResult.Failure("FLX-ASSET-MODEL-EXTRACT-0004", "The independent material could not be created.");
                    return;
                }
                _result = CliCommandResult.Success(new { sourceId = _sourceId, id = _outputId, path = _targetPath, ownership = "independent" });
            }
        }

        /// <summary>
        /// Loads an asset and returns a stable, type-specific semantic summary.
        /// </summary>
        [CliCommand("assets.inspect", Description = "Load an asset and return a stable semantic summary.", Access = CliCommandAccess.ReadOnly)]
        public static object Inspect(
            [CliOption("asset", Required = true)] string asset,
            [CliOption("reload", Description = "Reload the Content item before inspection.")] bool reload = false)
        {
            if (AssetObjectId.TryParse(asset, out var requestedObject) && FlaxEngine.Content.GetAssetInfo(requestedObject, out var objectInfo))
            {
                var direct = FlaxEngine.Content.LoadAssetAsync(requestedObject);
                if (direct == null || direct.WaitForLoaded())
                    throw new InvalidOperationException($"Asset object '{requestedObject}' failed to load.");
                return DescribeLoaded(direct.ID, objectInfo.Path, direct, true, requestedObject);
            }
            if (Guid.TryParse(asset, out var requestedId) && FlaxEngine.Content.GetRuntimeAssetInfo(requestedId, out var requestedInfo))
            {
                var directItem = Editor.Instance.ContentDatabase.FindAsset(requestedId);
                if (directItem == null || directItem.ID != requestedId)
                {
                    var direct = FlaxEngine.Content.LoadRuntimeObjectAsync<Asset>(requestedId);
                    if (direct == null || direct.WaitForLoaded())
                        throw new InvalidOperationException($"Asset '{requestedId}' failed to load.");
                    return DescribeLoaded(requestedId, requestedInfo.Path, direct, false);
                }
            }
            var item = ResolveAssetItem(asset);
            if (reload)
                item.Reload();
            var loaded = item.LoadAsync();
            if (loaded == null || loaded.WaitForLoaded())
                throw new InvalidOperationException($"Asset '{item.Path}' failed to load.");
            return DescribeLoaded(item.ID, item.Path, loaded, item.IsCanonicalSource);
        }

        private static object DescribeLoaded(Guid id, string path, Asset loaded, bool canonical, AssetObjectId? objectId = null)
        {
            return new
            {
                id,
                objectId = objectId ?? loaded.PersistentObjectId,
                path,
                type = loaded.GetType().FullName,
                sourcePath = loaded.SourcePath,
                storagePath = loaded is BinaryAsset storage ? storage.StoragePath : loaded.Path,
                artifactKey = loaded is BinaryAsset artifact ? artifact.ArtifactKey : null,
                exactArtifact = loaded is BinaryAsset exact && exact.IsUsingExactArtifact,
                importPath = loaded is BinaryAsset binary ? binary.ImportPath : null,
                memoryUsage = loaded.MemoryUsage,
                references = loaded.GetReferences().Where(x => x != Guid.Empty).Distinct().OrderBy(x => x).ToArray(),
                semantics = Describe(loaded),
            };
        }

        private static AssetItem ResolveAssetItem(string value)
        {
            AssetItem item = null;
            if (Guid.TryParse(value, out var id))
            {
                item = Editor.Instance.ContentDatabase.FindAsset(id);
                if (item == null && FlaxEngine.Content.GetRuntimeAssetInfo(id, out var info) && !string.IsNullOrEmpty(info.Path))
                    item = FindAssetItem(info.Path);
            }
            else
            {
                var normalized = value.Replace(Path.AltDirectorySeparatorChar, Path.DirectorySeparatorChar);
                var path = Path.IsPathRooted(normalized)
                    ? Path.GetFullPath(normalized)
                    : normalized.Equals("Content", StringComparison.OrdinalIgnoreCase) || normalized.StartsWith("Content" + Path.DirectorySeparatorChar, StringComparison.OrdinalIgnoreCase)
                        ? Path.GetFullPath(normalized, Globals.ProjectFolder)
                        : Path.GetFullPath(normalized, Globals.ProjectContentFolder);
                item = FindAssetItem(path);
            }
            return item ?? throw new FileNotFoundException($"Asset '{value}' was not found in the Content database.");
        }

        private static AssetItem FindAssetItem(string path)
        {
            var database = Editor.Instance.ContentDatabase;
            var item = database.Find(path) as AssetItem;
            if (item != null)
                return item;
            var current = Path.GetDirectoryName(path);
            while (!string.IsNullOrEmpty(current))
            {
                var parent = database.Find(current);
                if (parent != null)
                {
                    database.RefreshFolder(parent, true);
                    break;
                }
                current = Path.GetDirectoryName(current);
            }
            return database.Find(path) as AssetItem;
        }

        private static object Describe(Asset asset)
        {
            switch (asset)
            {
            case Texture texture:
                return new
                {
                    format = texture.Format.ToString(),
                    texture.Width,
                    texture.Height,
                    texture.ArraySize,
                    texture.MipLevels,
                    texture.ResidentMipLevels,
                    texture.IsNormalMap,
                    texture.TotalMemoryUsage,
                };
            case Model model:
                return new
                {
                    lods = model.LODs.Select(DescribeModelLod).ToArray(),
                    materialSlots = DescribeMaterialSlots(model),
                    sdf = new
                    {
                        present = model.SDF.Texture != null,
                        model.SDF.LOD,
                        model.SDF.ResolutionScale,
                        model.SDF.WorldUnitsPerVoxel,
                        model.SDF.MaxDistance,
                        localBoundsMin = Describe(model.SDF.LocalBoundsMin),
                        localBoundsMax = Describe(model.SDF.LocalBoundsMax),
                    },
                };
            case SkinnedModel model:
                return new
                {
                    lods = model.LODs.Select(DescribeSkinnedModelLod).ToArray(),
                    materialSlots = DescribeMaterialSlots(model),
                    nodes = model.Nodes.Select(x => new { x.Name, x.ParentIndex }).ToArray(),
                    bones = model.Bones.Select(x => new { x.NodeIndex, x.ParentIndex }).ToArray(),
                    blendShapes = model.BlendShapes,
                };
            case Animation animation:
                return new
                {
                    animation.Length,
                    animation.Duration,
                    animation.FramesPerSecond,
                    info = animation.Info,
                };
            case AudioClip audio:
                return new
                {
                    format = audio.Format.ToString(),
                    audio.Length,
                    audio.Is3D,
                    audio.IsStreamable,
                    info = audio.Info,
                };
            case Material material:
                return new
                {
                    surfaceBytes = material.LoadSurface(false)?.Length ?? 0,
                    info = material.Info,
                    parameters = material.Parameters.Select(x => new { x.ParameterID, x.Name, type = x.ParameterType.ToString(), x.IsPublic }).ToArray(),
                };
            case MaterialFunction function:
                function.GetSignature(out var materialTypes, out var materialNames);
                return new { surfaceBytes = function.LoadSurface()?.Length ?? 0, signature = materialTypes.Zip(materialNames, (type, name) => new { type, name }).ToArray() };
            case AnimationGraph graph:
                return new { surfaceBytes = graph.LoadSurface()?.Length ?? 0, baseModel = graph.BaseModel?.ID ?? Guid.Empty };
            case AnimationGraphFunction function:
                function.GetSignature(out var animationTypes, out var animationNames);
                return new { surfaceBytes = function.LoadSurface()?.Length ?? 0, signature = animationTypes.Zip(animationNames, (type, name) => new { type, name }).ToArray() };
            case ParticleEmitter emitter:
                return new { surfaceBytes = emitter.LoadSurface(true)?.Length ?? 0 };
            case ParticleSystem system:
                return new { system.FramesPerSecond, system.DurationFrames, timelineBytes = system.LoadTimeline()?.Length ?? 0 };
            case CollisionData collision:
            {
                var options = collision.Options;
                return new
                {
                    type = options.Type.ToString(),
                    options.Model,
                    options.ModelLodIndex,
                    convexFlags = options.ConvexFlags.ToString(),
                    options.ConvexVertexLimit,
                    options.MaterialSlotsMask,
                    boundsMin = Describe(options.Box.Minimum),
                    boundsMax = Describe(options.Box.Maximum),
                };
            }
            case VisualScript script:
                return new
                {
                    surfaceBytes = script.LoadSurface()?.Length ?? 0,
                    script.ScriptTypeName,
                    script.Meta.BaseTypename,
                    methods = Enumerable.Range(0, script.GetMethodsCount()).Select(index => DescribeVisualScriptMethod(script, index)).ToArray(),
                };
            default:
                return new { loaded = true };
            }
        }

        private static object DescribeModelLod(ModelLOD lod)
        {
            return new
            {
                lod.LODIndex,
                lod.ScreenSize,
                lod.VertexCount,
                meshes = lod.Meshes.Select(x => new { x.Index, x.VertexCount, x.TriangleCount, x.MaterialSlotIndex }).ToArray(),
            };
        }

        private static object DescribeSkinnedModelLod(SkinnedModelLOD lod)
        {
            return new
            {
                lod.LODIndex,
                lod.ScreenSize,
                meshes = lod.Meshes.Select(x => new { x.Index, x.VertexCount, x.TriangleCount, x.MaterialSlotIndex }).ToArray(),
            };
        }

        private static object[] DescribeMaterialSlots(ModelBase model)
        {
            return model.MaterialSlots.Select(x => new { x.Name, material = x.Material?.ID ?? Guid.Empty, shadowsMode = x.ShadowsMode.ToString() }).ToArray<object>();
        }

        private static object DescribeVisualScriptMethod(VisualScript script, int index)
        {
            script.GetMethodSignature(index, out var name, out var flags, out var returnType, out var parameterNames, out var parameterTypes, out var parameterOuts);
            return new
            {
                index,
                name,
                flags,
                returnType,
                parameters = parameterNames.Select((parameterName, parameterIndex) => new
                {
                    name = parameterName,
                    type = parameterTypes[parameterIndex],
                    isOut = parameterOuts[parameterIndex],
                }).ToArray(),
            };
        }

        private static object Describe(Float3 value)
        {
            return new { value.X, value.Y, value.Z };
        }
    }
}
