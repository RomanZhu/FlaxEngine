// Copyright (c) Wojciech Figat. All rights reserved.

using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Threading;
using FlaxEditor.Content;
using FlaxEditor.Content.Create;
using FlaxEditor.Content.Import;
using FlaxEngine;
using FlaxEngine.Tools;

namespace FlaxEditor.Modules
{
    /// <summary>
    /// Imports assets and other resources to the project. Provides per asset import settings editing.
    /// </summary>
    /// <seealso cref="FlaxEditor.Modules.EditorModule" />
    public sealed class ContentImportingModule : EditorModule
    {
        private enum CanonicalBuildKind
        {
            None,
            Texture,
            Model,
            Graph,
        }

        private sealed class InPlaceCanonicalImportEntry : IFileEntryAction
        {
            public string SourceUrl { get; }
            public string ResultUrl => SourceUrl;
            public bool ReplaceForeignMetadata { get; }
            public Guid BuildAssetID { get; private set; }
            public CanonicalBuildKind BuildKind { get; private set; }

            public InPlaceCanonicalImportEntry(string sourceUrl, bool replaceForeignMetadata)
            {
                SourceUrl = sourceUrl;
                ReplaceForeignMetadata = replaceForeignMetadata;
            }

            public bool Execute()
            {
                var failed = CreateDefaultCanonicalMetadata(SourceUrl, out var buildAssetId, out var buildKind);
                if (failed)
                    return true;
                BuildAssetID = buildAssetId;
                BuildKind = buildKind;
                return false;
            }

            public void SetPreparedBuild(Guid assetID)
            {
                BuildAssetID = assetID;
                BuildKind = GetCanonicalBuildKind(SourceUrl);
            }
        }

        private readonly struct PendingCanonicalBuild
        {
            public readonly Guid AssetID;
            public readonly CanonicalBuildKind BuildKind;
            public readonly string SourcePath;

            public PendingCanonicalBuild(Guid assetId, CanonicalBuildKind buildKind, string sourcePath)
            {
                AssetID = assetId;
                BuildKind = buildKind;
                SourcePath = sourcePath;
            }
        }

        // Amount of requests done/total used to calculate importing progress

        private int _importBatchDone;
        private int _importBatchSize;

        // Firstly service is collecting import requests and then performs actual importing in the background.

        private readonly Queue<IFileEntryAction> _importingQueue = new Queue<IFileEntryAction>();
        private readonly List<Request> _requests = new List<Request>();

        private long _workerEndFlag;
        private Thread _workerThread;

        /// <summary>
        /// Gets a value indicating whether this instance is importing assets.
        /// </summary>
        public bool IsImporting
        {
            get
            {
                lock (_requests)
                    return _importBatchSize > 0;
            }
        }

        /// <summary>
        /// Gets the importing assets progress.
        /// </summary>
        public float ImportingProgress
        {
            get
            {
                lock (_requests)
                    return _importBatchSize > 0 ? (float)_importBatchDone / _importBatchSize : 1.0f;
            }
        }

        /// <summary>
        /// Gets the amount of files done in the current import batch.
        /// </summary>
        public float ImportBatchDone
        {
            get
            {
                lock (_requests)
                    return _importBatchDone;
            }
        }

        /// <summary>
        /// Gets the size of the current import batch (imported files + files to import left).
        /// </summary>
        public int ImportBatchSize
        {
            get
            {
                lock (_requests)
                    return _importBatchSize;
            }
        }

        /// <summary>
        /// Occurs when assets importing starts.
        /// </summary>
        public event Action ImportingQueueBegin;

        /// <summary>
        /// Occurs when file is being imported. Can be called on non-main thread.
        /// </summary>
        public event Action<IFileEntryAction> ImportFileBegin;

        /// <summary>
        /// Import file end delegate.
        /// </summary>
        /// <param name="entry">The imported file entry.</param>
        /// <param name="failed">if set to <c>true</c> if importing failed, otherwise false.</param>
        public delegate void ImportFileEndDelegate(IFileEntryAction entry, bool failed);

        /// <summary>
        /// Occurs when file importing end. Can be called on non-main thread.
        /// </summary>
        public event ImportFileEndDelegate ImportFileEnd;

        /// <summary>
        /// Occurs when assets importing ends. Can be called on non-main thread.
        /// </summary>
        public event Action ImportingQueueEnd;

        /// <inheritdoc />
        internal ContentImportingModule(Editor editor)
        : base(editor)
        {
        }

        /// <summary>
        /// Creates the specified file entry (can show create settings dialog if needed).
        /// </summary>
        /// <param name="entry">The entry.</param>
        public void Create(CreateFileEntry entry)
        {
            if (entry.HasSettings)
            {
                // Use settings dialog
                var dialog = new CreateFilesDialog(entry);
                dialog.Show(Editor.Windows.MainWindow);
            }
            else
            {
                // Use direct creation
                LetThemBeCreatedxD(entry);
            }
        }

        /// <summary>
        /// Shows the dialog for selecting files to import.
        /// </summary>
        /// <param name="targetLocation">The target location.</param>
        public void ShowImportFileDialog(ContentFolder targetLocation)
        {
            // Ask user to select files to import
            if (FileSystem.ShowOpenFileDialog(Editor.Windows.MainWindow, null, "All files (*.*)\0*.*\0", true, "Select files to import", out var files))
                return;
            if (files != null && files.Length > 0)
            {
                Import(files, targetLocation);
            }
        }

        /// <summary>
        /// Reimports the specified <see cref="BinaryAssetItem"/> item.
        /// </summary>
        /// <param name="item">The item.</param>
        /// <param name="settings">The import settings to override.</param>
        /// <param name="skipSettingsDialog">True if skip any popup dialogs showing for import options adjusting. Can be used when importing files from code.</param>
        public void Reimport(BinaryAssetItem item, object settings = null, bool skipSettingsDialog = false)
        {
            if (item != null && !item.GetImportPath(out string importPath))
                Reimport(item, importPath, settings, skipSettingsDialog);
        }

        internal void Reimport(BinaryAssetItem item, string importPath, object settings = null, bool skipSettingsDialog = false)
        {
            if (item == null)
                throw new ArgumentNullException(nameof(item));
            importPath = StringUtils.NormalizePath(importPath);
            if (GetReimportPath(item.ShortName, ref importPath, skipSettingsDialog))
                return;
            Import(importPath, item.Path, true, skipSettingsDialog, settings, true);
        }

        internal bool GetReimportPath(string contextName, ref string importPath, bool skipSettingsDialog = false)
        {
            // Check if input file is missing
            if (!System.IO.File.Exists(importPath))
            {
                Editor.LogWarning(string.Format("Cannot reimport asset \'{0}\'. File \'{1}\' does not exist.", contextName, importPath));
                if (skipSettingsDialog)
                    return true;

                // Ask user to select new file location
                var title = string.Format("Please find missing \'{0}\' file for asset \'{1}\'", importPath, contextName);
                if (FileSystem.ShowOpenFileDialog(Editor.Windows.MainWindow, null, "All files (*.*)\0*.*\0", false, title, out var files))
                    return true;
                if (files != null && files.Length > 0)
                    importPath = files[0];

                // Validate file path again
                if (!System.IO.File.Exists(importPath))
                    return true;
            }
            return false;
        }

        /// <summary>
        /// Imports the specified files.
        /// </summary>
        /// <param name="files">The files.</param>
        /// <param name="targetLocation">The target location.</param>
        /// <param name="skipSettingsDialog">True if skip any popup dialogs showing for import options adjusting. Can be used when importing files from code.</param>
        public void Import(IEnumerable<string> files, ContentFolder targetLocation, bool skipSettingsDialog = false)
        {
            Import(files, targetLocation, skipSettingsDialog, null);
        }

        internal void Import(IEnumerable<string> files, ContentFolder targetLocation, bool skipSettingsDialog, object settings)
        {
            if (targetLocation == null)
                throw new ArgumentNullException();
            if (files == null)
                return;

            var filesArray = files as string[] ?? files.ToArray();
            var preflight = PreflightImport(filesArray, targetLocation);
            if (!preflight.Succeeded)
            {
                Editor.LogWarning(preflight.Message);
                ContentMutationDiagnostics.Log("mutation.import.rejected", $"target='{targetLocation.Path}'; failure={preflight.Failure}; message='{ContentMutationDiagnostics.Sanitize(preflight.Message)}'");
                return;
            }

            lock (_requests)
            {
                bool skipDialog = skipSettingsDialog;
                foreach (var file in filesArray)
                {
                    Import(file, targetLocation, skipSettingsDialog, settings, ref skipDialog);
                }
            }
        }

        /// <summary>
        /// Imports the specified file.
        /// </summary>
        /// <param name="file">The file.</param>
        /// <param name="targetLocation">The target location.</param>
        /// <param name="skipSettingsDialog">True if skip any popup dialogs showing for import options adjusting. Can be used when importing files from code.</param>
        /// <param name="settings">Import settings to override. Use null to skip this value.</param>
        public void Import(string file, ContentFolder targetLocation, bool skipSettingsDialog = false, object settings = null)
        {
            var preflight = PreflightImport(new[] { file }, targetLocation);
            if (!preflight.Succeeded)
            {
                Editor.LogWarning(preflight.Message);
                return;
            }
            bool skipDialog = skipSettingsDialog;
            Import(file, targetLocation, skipSettingsDialog, settings, ref skipDialog);
        }

        internal ContentMutationResult PreflightImport(IEnumerable<string> files, ContentFolder targetLocation)
        {
            if (targetLocation == null)
                return ContentMutationResult.Fail(ContentMutationFailure.InvalidDestination, null, null, "The import target folder is missing.");
            if (files == null)
                return ContentMutationResult.Fail(ContentMutationFailure.InvalidSource, null, targetLocation.Path, "No import sources were provided.");

            var destinations = new HashSet<string>(ContentMutationPathUtils.Comparer);
            string firstSource = null;
            string firstDestination = null;
            foreach (var input in files)
            {
                var inputPath = ContentMutationPathUtils.Normalize(input);
                firstSource ??= inputPath;
                if (!ContentMutationPathUtils.Exists(inputPath))
                    return ContentMutationResult.Fail(ContentMutationFailure.MissingSource, inputPath, targetLocation.Path, $"Import source '{inputPath}' does not exist.");
                var isDirectory = Directory.Exists(inputPath);
                if (ContentMutationPathUtils.ContainsReparsePoint(inputPath, isDirectory))
                    return ContentMutationResult.Fail(ContentMutationFailure.UnsupportedLink, inputPath, targetLocation.Path, $"Import source '{inputPath}' contains an unsupported filesystem link.");

                string outputName;
                if (isDirectory)
                {
                    outputName = Path.GetFileName(inputPath.TrimEnd(Path.DirectorySeparatorChar, Path.AltDirectorySeparatorChar));
                }
                else
                {
                    var extension = Path.GetExtension(inputPath) ?? string.Empty;
                    if (string.Equals(extension, ".flax", StringComparison.OrdinalIgnoreCase))
                        return ContentMutationResult.Fail(ContentMutationFailure.InvalidSource, inputPath, targetLocation.Path, "Legacy .flax assets cannot be imported into an asset-system-v2 project.");
                    var isCanonicalSource = IsCanonicalSourceImport(extension);
                    string outputExtension = null;
                    var isBuilt = !isCanonicalSource && Editor.CanImport(extension, out outputExtension);
                    if (isCanonicalSource && !targetLocation.CanHaveAssets)
                        return ContentMutationResult.Fail(ContentMutationFailure.InvalidDestination, inputPath, targetLocation.Path, "The target folder cannot contain imported assets.");
                    if (isBuilt)
                    {
                        if (!targetLocation.CanHaveAssets)
                            return ContentMutationResult.Fail(ContentMutationFailure.InvalidDestination, inputPath, targetLocation.Path, "The target folder cannot contain imported assets.");
                        extension = "." + outputExtension;
                    }
                    else if (!targetLocation.CanHaveScripts && (extension == ".cs" || extension == ".cpp" || extension == ".h" || extension == ".c" || extension == ".hpp"))
                    {
                        return ContentMutationResult.Fail(ContentMutationFailure.InvalidDestination, inputPath, targetLocation.Path, "The target folder cannot contain source files.");
                    }
                    outputName = Path.GetFileNameWithoutExtension(inputPath) + extension;
                }

                var destination = ContentMutationPathUtils.Normalize(Path.Combine(targetLocation.Path, outputName));
                firstDestination ??= destination;
                if (!destinations.Add(destination) || ContentMutationPathUtils.Exists(destination))
                    return ContentMutationResult.Fail(ContentMutationFailure.DestinationCollision, inputPath, destination, $"Import destination '{destination}' already exists or is duplicated in the batch.");
                if (!isDirectory && IsCanonicalSourceImport(Path.GetExtension(inputPath)))
                {
                    var metaDestination = destination + ".meta";
                    if (!destinations.Add(metaDestination) || ContentMutationPathUtils.Exists(metaDestination))
                        return ContentMutationResult.Fail(ContentMutationFailure.DestinationCollision, inputPath, metaDestination, $"Canonical metadata destination '{metaDestination}' already exists or is duplicated in the batch.");
                }
            }
            return destinations.Count == 0
                ? ContentMutationResult.Fail(ContentMutationFailure.InvalidSource, null, targetLocation.Path, "No import sources were provided.")
                : ContentMutationResult.Prepared(firstSource, firstDestination);
        }

        private void Import(string inputPath, ContentFolder targetLocation, bool skipSettingsDialog, object settings, ref bool skipDialog)
        {
            if (targetLocation == null)
                throw new ArgumentNullException();

            var extension = System.IO.Path.GetExtension(inputPath) ?? string.Empty;

            if (string.Equals(extension, ".flax", StringComparison.OrdinalIgnoreCase))
            {
                Editor.LogWarning($"Legacy .flax assets cannot be imported into an asset-system-v2 project: {inputPath}");
                return;
            }

            // Check if given file extension is a binary asset (.flax files) and can be imported by the engine
            bool useCanonicalSource = IsCanonicalSourceImport(extension);
            string outputExtension = null;
            bool isBuilt = !useCanonicalSource && Editor.CanImport(extension, out outputExtension);
            if (useCanonicalSource)
            {
                outputExtension = extension;
                if (!targetLocation.CanHaveAssets)
                {
                    Editor.LogWarning(string.Format("Cannot import '{0}' to '{1}'. The target directory cannot have assets.", inputPath, targetLocation.Node.Path));
                    return;
                }
            }
            if (isBuilt)
            {
                outputExtension = '.' + outputExtension;

                if (!targetLocation.CanHaveAssets)
                {
                    // Error
                    Editor.LogWarning(string.Format("Cannot import \'{0}\' to \'{1}\'. The target directory cannot have assets.", inputPath, targetLocation.Node.Path));
                    if (!skipDialog)
                    {
                        skipDialog = true;
                        MessageBox.Show("Target location cannot have assets. Use Content folder for your game assets.", "Cannot import assets", MessageBoxButtons.OK, MessageBoxIcon.Error);
                    }
                    return;
                }
            }
            else
            {
                // Preserve file extension (will copy file to the import location)
                outputExtension = extension;

                // Check if can place source files here
                if (!targetLocation.CanHaveScripts && (extension == ".cs" || extension == ".cpp" || extension == ".h" || extension == ".c" || extension == ".hpp"))
                {
                    // Error
                    Editor.LogWarning(string.Format("Cannot import \'{0}\' to \'{1}\'. The target directory cannot have scripts.", inputPath, targetLocation.Node.Path));
                    if (!skipDialog)
                    {
                        skipDialog = true;
                        MessageBox.Show("Target location cannot have scripts. Use Source folder for your game source code.", "Cannot import assets", MessageBoxButtons.OK, MessageBoxIcon.Error);
                    }
                    return;
                }
            }

            var shortName = System.IO.Path.GetFileNameWithoutExtension(inputPath);
            var outputPath = System.IO.Path.Combine(targetLocation.Path, shortName + outputExtension);

            Import(inputPath, outputPath, isBuilt, skipSettingsDialog, settings, false, useCanonicalSource);
        }

        /// <summary>
        /// Imports the specified file to the target destination.
        /// Actual importing is done later after gathering settings from the user via <see cref="ImportFilesDialog"/>.
        /// </summary>
        /// <param name="inputPath">The input path.</param>
        /// <param name="outputPath">The output path.</param>
        /// <param name="isInBuilt">True if use in-built importer (engine backend).</param>
        /// <param name="skipSettingsDialog">True if skip any popup dialogs showing for import options adjusting. Can be used when importing files from code.</param>
        /// <param name="settings">Import settings to override. Use null to skip this value.</param>
        /// <param name="allowReplace">True only for an explicit reimport that may replace the destination.</param>
        /// <param name="useCanonicalSource">True to preserve the imported source and create adjacent metadata.</param>
        private void Import(string inputPath, string outputPath, bool isInBuilt, bool skipSettingsDialog = false, object settings = null, bool allowReplace = false, bool useCanonicalSource = false)
        {
            inputPath = StringUtils.NormalizePath(inputPath);
            outputPath = StringUtils.NormalizePath(outputPath);
            if (string.Equals(Path.GetExtension(inputPath), ".flax", StringComparison.OrdinalIgnoreCase) ||
                string.Equals(Path.GetExtension(outputPath), ".flax", StringComparison.OrdinalIgnoreCase))
            {
                Editor.LogWarning($"Legacy .flax assets cannot be imported or reimported in an asset-system-v2 project: {inputPath}");
                return;
            }
            lock (_requests)
            {
                _requests.Add(new Request
                {
                    InputPath = inputPath,
                    OutputPath = outputPath,
                    IsInBuilt = isInBuilt,
                    SkipSettingsDialog = skipSettingsDialog,
                    AllowReplace = allowReplace,
                    UseCanonicalSource = useCanonicalSource,
                    Settings = settings,
                });
            }
        }

        private static bool IsCanonicalSourceImport(string extension)
        {
            extension = extension?.ToLowerInvariant();
            switch (extension)
            {
            case ".png":
            case ".tga":
            case ".exr":
            case ".bmp":
            case ".gif":
            case ".tiff":
            case ".tif":
            case ".jpeg":
            case ".jpg":
            case ".dds":
            case ".hdr":
            case ".raw":
            case ".ies":
            case ".fbx":
            case ".obj":
            case ".x":
            case ".dae":
            case ".gltf":
            case ".glb":
            case ".blend":
            case ".bvh":
            case ".ase":
            case ".ply":
            case ".dxf":
            case ".ifc":
            case ".nff":
            case ".smd":
            case ".vta":
            case ".mdl":
            case ".md2":
            case ".md3":
            case ".md5mesh":
            case ".q3o":
            case ".q3s":
            case ".ac":
            case ".stl":
            case ".lwo":
            case ".lws":
            case ".lxo":
            case ".wav":
            case ".mp3":
            case ".ogg":
            case ".ttf":
            case ".otf":
            case ".shader":
            case ".mp4":
            case ".webm":
            case ".mov":
            case ".mkv":
            case ".txt":
                return true;
            default:
                return false;
            }
        }

        internal void RegisterInPlaceCanonicalSources(IEnumerable<string> sourcePaths)
        {
            var entries = new List<IFileEntryAction>();
            var uniquePaths = new HashSet<string>(ContentMutationPathUtils.Comparer);
            foreach (var sourcePath in sourcePaths)
            {
                var path = ContentMutationPathUtils.Normalize(sourcePath);
                var metadataPath = path + ".meta";
                var replaceForeignMetadata = File.Exists(metadataPath) && IsUnityMetadata(metadataPath);
                if (!uniquePaths.Add(path) || !File.Exists(path) ||
                    (File.Exists(metadataPath) && !replaceForeignMetadata) ||
                    !IsCanonicalSourceImport(Path.GetExtension(path)))
                    continue;
                entries.Add(new InPlaceCanonicalImportEntry(path, replaceForeignMetadata));
            }
            if (entries.Count == 0)
                return;

            lock (_requests)
            {
                _importBatchSize += entries.Count;
                for (int i = 0; i < entries.Count; i++)
                    _importingQueue.Enqueue(entries[i]);
            }
            StartWorker();
        }

        private static bool IsUnityMetadata(string path)
        {
            try
            {
                using var stream = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.ReadWrite | FileShare.Delete);
                using var reader = new StreamReader(stream);
                var version = reader.ReadLine();
                var guid = reader.ReadLine();
                return version?.StartsWith("fileFormatVersion:", StringComparison.Ordinal) == true &&
                       guid?.StartsWith("guid:", StringComparison.Ordinal) == true;
            }
            catch
            {
                return false;
            }
        }

        private static CanonicalBuildKind GetCanonicalBuildKind(string sourcePath)
        {
            switch (Path.GetExtension(sourcePath)?.ToLowerInvariant())
            {
            case ".png": case ".tga": case ".exr": case ".bmp": case ".gif": case ".tiff": case ".tif":
            case ".jpeg": case ".jpg": case ".dds": case ".hdr": case ".raw":
                return CanonicalBuildKind.Texture;
            case ".obj": case ".fbx": case ".x": case ".dae": case ".gltf": case ".glb": case ".blend":
            case ".bvh": case ".ase": case ".ply": case ".dxf": case ".ifc": case ".nff": case ".smd":
            case ".vta": case ".mdl": case ".md2": case ".md3": case ".md5mesh": case ".q3o": case ".q3s":
            case ".ac": case ".stl": case ".lwo": case ".lws": case ".lxo":
                return CanonicalBuildKind.Model;
            case ".wav": case ".mp3": case ".ogg": case ".ttf": case ".otf": case ".shader":
            case ".mp4": case ".webm": case ".mov": case ".mkv": case ".txt":
                return CanonicalBuildKind.Graph;
            default:
                return CanonicalBuildKind.None;
            }
        }

        private static bool CreateDefaultCanonicalMetadata(string sourcePath, out Guid buildAssetId, out CanonicalBuildKind buildKind)
        {
            buildAssetId = Guid.Empty;
            buildKind = GetCanonicalBuildKind(sourcePath);
            switch (Path.GetExtension(sourcePath)?.ToLowerInvariant())
            {
            case ".png":
            case ".tga":
            case ".exr":
            case ".bmp":
            case ".gif":
            case ".tiff":
            case ".tif":
            case ".jpeg":
            case ".jpg":
            case ".dds":
            case ".hdr":
            case ".raw":
            {
                buildAssetId = AssetDatabaseFacade.CreateTextureMetadata(sourcePath, TextureTool.Options.Default);
                return buildAssetId == Guid.Empty;
            }
            case ".obj":
            case ".fbx":
            case ".x":
            case ".dae":
            case ".gltf":
            case ".glb":
            case ".blend":
            case ".bvh":
            case ".ase":
            case ".ply":
            case ".dxf":
            case ".ifc":
            case ".nff":
            case ".smd":
            case ".vta":
            case ".mdl":
            case ".md2":
            case ".md3":
            case ".md5mesh":
            case ".q3o":
            case ".q3s":
            case ".ac":
            case ".stl":
            case ".lwo":
            case ".lws":
            case ".lxo":
            {
                buildAssetId = AssetDatabaseFacade.CreateDefaultModelMetadata(sourcePath);
                return buildAssetId == Guid.Empty;
            }
            case ".wav":
            case ".mp3":
            case ".ogg":
                buildAssetId = AssetDatabaseFacade.CreateAudioMetadata(sourcePath, AudioTool.Options.Default);
                return buildAssetId == Guid.Empty;
            default:
                buildAssetId = CreateImportedSourceMetadataID(sourcePath);
                return buildAssetId == Guid.Empty;
            }
        }

        private static void WaitForCanonicalBuilds(List<PendingCanonicalBuild> builds)
        {
            while (builds.Count != 0)
            {
                for (int i = builds.Count - 1; i >= 0; i--)
                {
                    var build = builds[i];
                    var status = build.BuildKind == CanonicalBuildKind.Model
                        ? AssetDatabaseFacade.GetModelBuildStatus(build.AssetID)
                        : build.BuildKind == CanonicalBuildKind.Texture
                            ? AssetDatabaseFacade.GetTextureBuildStatus(build.AssetID)
                            : AssetDatabaseFacade.GetGraphBuildStatus(build.AssetID);
                    switch (status)
                    {
                    case "ReadyExact":
                        builds.RemoveAt(i);
                        break;
                    case "Failed":
                    case "Cancelled":
                    case "NotBuilt":
                        var diagnostic = build.BuildKind == CanonicalBuildKind.Model
                            ? AssetDatabaseFacade.GetModelBuildDiagnostic(build.AssetID)
                            : build.BuildKind == CanonicalBuildKind.Texture
                                ? AssetDatabaseFacade.GetTextureBuildDiagnostic(build.AssetID)
                                : AssetDatabaseFacade.GetGraphBuildDiagnostic(build.AssetID);
                        Editor.LogWarning($"Canonical build for {build.SourcePath} ({build.AssetID:N}) ended as {status}: {diagnostic.Message}");
                        builds.RemoveAt(i);
                        break;
                    }
                }
                if (builds.Count != 0)
                    Thread.Sleep(25);
            }
        }

        private bool ExecutePreparedInPlaceRegistrationBatch(List<InPlaceCanonicalImportEntry> entries, string[] stagingPaths, Guid[] assetIds, List<int> validIndices)
        {
            var plan = new ContentMutationPlan(ContentMutationOperationKind.ImportOutput);
            var steps = new List<ContentMutationStep>();
            var backupRoot = StringUtils.CombinePaths(Globals.ProjectCacheFolder, "ContentMutationBackups", plan.Id.ToString("N"));
            var backupPaths = new Dictionary<int, string>();

            // This runs last during rollback, after metadata paths have been restored. Only the
            // batch's own sources moved, so reindexing those is enough and avoids a full scan.
            var rollbackPaths = validIndices.Select(index => entries[index].SourceUrl).ToArray();
            steps.Add(new ContentMutationStep(
                "restore-database-after-batch-rollback",
                Array.Empty<int>(),
                () => ContentMutationResult.Success(null, null),
                () => rollbackPaths.Length == 0 || !AssetDatabaseFacade.RefreshSources(rollbackPaths),
                () => true));

            for (int position = 0; position < validIndices.Count; position++)
            {
                var index = validIndices[position];
                var entry = entries[index];
                var metadataPath = entry.SourceUrl + ".meta";
                if (entry.ReplaceForeignMetadata)
                {
                    Directory.CreateDirectory(backupRoot);
                    var backupPath = StringUtils.CombinePaths(backupRoot, index + "_" + Path.GetFileName(metadataPath));
                    backupPaths.Add(index, backupPath);
                    var backupEntry = plan.Entries.Count;
                    plan.Entries.Add(new ContentMutationEntry(metadataPath, backupPath, ContentMutationPathRole.ReplacementBackup, false));
                    steps.Add(new ContentMutationStep(
                        "backup-foreign-metadata-" + index,
                        new[] { backupEntry },
                        () =>
                        {
                            File.Move(metadataPath, backupPath);
                            return ContentMutationResult.Success(metadataPath, backupPath);
                        },
                        () =>
                        {
                            if (File.Exists(backupPath) && !File.Exists(metadataPath))
                                File.Move(backupPath, metadataPath);
                            return File.Exists(metadataPath) && !File.Exists(backupPath);
                        },
                        () => !File.Exists(metadataPath) && File.Exists(backupPath)));
                }

                var stagedPath = stagingPaths[index];
                var metadataEntry = plan.Entries.Count;
                plan.Entries.Add(new ContentMutationEntry(stagedPath, metadataPath, ContentMutationPathRole.MetadataSidecar, false)
                {
                    DestinationReleasedByTransaction = entry.ReplaceForeignMetadata,
                    SourceRequired = true,
                });
                steps.Add(new ContentMutationStep(
                    "commit-staged-metadata-" + index,
                    new[] { metadataEntry },
                    () =>
                    {
                        File.Move(stagedPath, metadataPath);
                        return ContentMutationResult.Success(stagedPath, metadataPath);
                    },
                    () =>
                    {
                        if (File.Exists(metadataPath) && !File.Exists(stagedPath))
                            File.Move(metadataPath, stagedPath);
                        return File.Exists(stagedPath) && !File.Exists(metadataPath);
                    },
                    () => File.Exists(metadataPath) && !File.Exists(stagedPath)));
            }

            var publishIds = validIndices.Select(index => assetIds[index]).ToArray();
            var publishPaths = validIndices.Select(index => entries[index].SourceUrl).ToArray();
            steps.Add(new ContentMutationStep(
                "publish-canonical-metadata-batch",
                Array.Empty<int>(),
                () => AssetDatabaseFacade.PublishDefaultCanonicalMetadataBatch(publishIds, publishPaths)
                    ? ContentMutationResult.Fail(ContentMutationFailure.VerificationFailure, null, null, "Canonical metadata batch database publication failed.")
                    : ContentMutationResult.Success(null, null),
                () => true,
                () => validIndices.All(index => FlaxEngine.Content.GetAssetInfo(entries[index].SourceUrl, out var info) && info.ID == assetIds[index])));

            var result = new ContentMutationTransaction(plan).Execute(steps);
            if (!result.Succeeded)
                Editor.LogWarning($"Canonical metadata batch transaction {plan.Id:N} failed: {result.Failure}: {result.Message}");
            if (result.Succeeded)
            {
                foreach (var backupPath in backupPaths.Values)
                    DeleteImportPath(backupPath);
                if (Directory.Exists(backupRoot) && !Directory.EnumerateFileSystemEntries(backupRoot).Any())
                    Directory.Delete(backupRoot, false);
            }
            ContentMutationDiagnostics.Log(result.Succeeded ? "mutation.register-in-place-batch.committed" : "mutation.register-in-place-batch.failed",
                $"transaction={plan.Id:N}; sources={validIndices.Count}; failure={result.Failure}; recovery={result.RequiresRecovery}");
            return !result.Succeeded;
        }

        private void ProcessInPlaceCanonicalBatch(List<InPlaceCanonicalImportEntry> entries, List<PendingCanonicalBuild> pendingBuilds)
        {
            var failed = new bool[entries.Count];
            var stagingPaths = new string[entries.Count];
            var assetIds = new Guid[entries.Count];
            var activeIndices = new List<int>(entries.Count);
            var stagingRoot = Path.Combine(Globals.ProjectLibraryFolder, "Temp", "MetadataBatches", Guid.NewGuid().ToString("N"));
            Directory.CreateDirectory(stagingRoot);

            for (int i = 0; i < entries.Count; i++)
            {
                var metadataPath = entries[i].SourceUrl + ".meta";
                stagingPaths[i] = StringUtils.CombinePaths(stagingRoot, i + ".meta");
                Editor.ContentDatabase.BeginAssetSave(metadataPath);
                try
                {
                    ImportFileBegin?.Invoke(entries[i]);
                    activeIndices.Add(i);
                }
                catch (Exception ex)
                {
                    failed[i] = true;
                    Editor.LogWarning(ex);
                }
            }

            try
            {
                if (activeIndices.Count != 0)
                {
                    var sourcePaths = activeIndices.Select(index => entries[index].SourceUrl).ToArray();
                    var activeStagingPaths = activeIndices.Select(index => stagingPaths[index]).ToArray();
                    var stagedIds = AssetDatabaseFacade.StageDefaultCanonicalMetadataBatch(sourcePaths, activeStagingPaths);
                    var validIndices = new List<int>(activeIndices.Count);
                    for (int i = 0; i < activeIndices.Count; i++)
                    {
                        var index = activeIndices[i];
                        if (i >= stagedIds.Length || stagedIds[i] == Guid.Empty)
                        {
                            failed[index] = true;
                            continue;
                        }
                        assetIds[index] = stagedIds[i];
                        validIndices.Add(index);
                    }
                    if (validIndices.Count != activeIndices.Count)
                    {
                        foreach (var diagnostic in AssetDatabaseFacade.GetDiagnostics())
                            Editor.LogWarning($"Canonical metadata staging failed for {diagnostic.SourcePath}: {diagnostic.Message}");
                    }
                    if (validIndices.Count != 0 && ExecutePreparedInPlaceRegistrationBatch(entries, stagingPaths, assetIds, validIndices))
                    {
                        for (int i = 0; i < validIndices.Count; i++)
                            failed[validIndices[i]] = true;
                    }
                }
            }
            catch (Exception ex)
            {
                Editor.LogWarning(ex);
                for (int i = 0; i < entries.Count; i++)
                    failed[i] = true;
            }
            finally
            {
                for (int i = 0; i < entries.Count; i++)
                {
                    if (File.Exists(stagingPaths[i]))
                        DeleteImportPath(stagingPaths[i]);
                    Editor.ContentDatabase.EndAssetSave(entries[i].SourceUrl + ".meta", !failed[i]);
                    if (!failed[i])
                    {
                        entries[i].SetPreparedBuild(assetIds[i]);
                        pendingBuilds.Add(new PendingCanonicalBuild(assetIds[i], entries[i].BuildKind, entries[i].SourceUrl));
                    }
                    else
                    {
                        Editor.LogWarning("Failed to import " + entries[i].SourceUrl + " to " + entries[i].ResultUrl);
                    }
                    lock (_requests)
                        _importBatchDone++;
                    Profiler.BeginEvent("ImportFileEnd");
                    ImportFileEnd?.Invoke(entries[i], failed[i]);
                    Profiler.EndEvent();
                }
                if (Directory.Exists(stagingRoot) && !Directory.EnumerateFileSystemEntries(stagingRoot).Any())
                    Directory.Delete(stagingRoot, false);
            }
        }

        private void WorkerMain()
        {
            IFileEntryAction entry;
            bool wasLastTickWorking = false;
            var pendingCanonicalBuilds = new List<PendingCanonicalBuild>();

            while (Interlocked.Read(ref _workerEndFlag) == 0)
            {
                // Try to get entry to process
                lock (_requests)
                {
                    if (_importingQueue.Count > 0)
                        entry = _importingQueue.Dequeue();
                    else
                        entry = null;
                }

                // Check if has any no job
                bool inThisTickWork = entry != null;
                if (inThisTickWork)
                {
                    // Check if begin importing
                    if (!wasLastTickWorking)
                    {
                        lock (_requests)
                            _importBatchDone = 0;
                        ImportingQueueBegin?.Invoke();
                    }

                    if (entry is InPlaceCanonicalImportEntry canonicalEntry)
                    {
                        var canonicalBatch = new List<InPlaceCanonicalImportEntry> { canonicalEntry };
                        lock (_requests)
                        {
                            while (_importingQueue.Count != 0 && _importingQueue.Peek() is InPlaceCanonicalImportEntry)
                                canonicalBatch.Add((InPlaceCanonicalImportEntry)_importingQueue.Dequeue());
                        }
                        ProcessInPlaceCanonicalBatch(canonicalBatch, pendingCanonicalBuilds);
                        wasLastTickWorking = true;
                        continue;
                    }

                    // Import file
                    bool failed = true;
                    // The importer refreshes content items through ImportFileEnd. Mark the
                    // output as editor-authored so its filesystem notification does not
                    // trigger a second, delayed asset reload after the import batch ends.
                    // A folder import can produce many output paths, so leave it to the
                    // regular content database refresh path.
                    string trackedWritePath = null;
                    if (entry is InPlaceCanonicalImportEntry)
                        trackedWritePath = entry.ResultUrl + ".meta";
                    else if (!System.IO.Directory.Exists(entry.SourceUrl))
                        trackedWritePath = entry.ResultUrl;
                    if (trackedWritePath != null)
                        Editor.ContentDatabase.BeginAssetSave(trackedWritePath);
                    try
                    {
                        ImportFileBegin?.Invoke(entry);
                        failed = ExecuteImportTransaction(entry);
                    }
                    catch (Exception ex)
                    {
                        Editor.LogWarning(ex);
                    }
                    finally
                    {
                        if (trackedWritePath != null)
                            Editor.ContentDatabase.EndAssetSave(trackedWritePath, !failed);

                        if (failed)
                        {
                            Editor.LogWarning("Failed to import " + entry.SourceUrl + " to " + entry.ResultUrl);
                        }

                        lock (_requests)
                            _importBatchDone++;
                        Profiler.BeginEvent("ImportFileEnd");
                        ImportFileEnd?.Invoke(entry, failed);
                        Profiler.EndEvent();
                    }
                }
                else
                {
                    // Check if end importing
                    if (wasLastTickWorking)
                    {
                        WaitForCanonicalBuilds(pendingCanonicalBuilds);
                        lock (_requests)
                        {
                            if (_importingQueue.Count != 0)
                                continue;
                            _importBatchDone = _importBatchSize = 0;
                        }
                        ImportingQueueEnd?.Invoke();
                    }

                    // Wait some time
                    Thread.Sleep(100);
                }

                wasLastTickWorking = inThisTickWork;
            }
        }

        private bool ExecuteImportTransaction(IFileEntryAction entry)
        {
            if (entry is InPlaceCanonicalImportEntry inPlaceEntry)
                return ExecuteInPlaceRegistrationTransaction(inPlaceEntry);

            // Folder imports enqueue their descendants and do not have a synchronous commit boundary.
            if (Directory.Exists(entry.SourceUrl))
                return entry.Execute();

            var destinationPath = ContentMutationPathUtils.Normalize(entry.ResultUrl);
            var isCreate = entry is CreateFileEntry;
            var sourcePath = isCreate ? destinationPath : ContentMutationPathUtils.Normalize(entry.SourceUrl);
            var destinationExisted = File.Exists(destinationPath);
            var allowReplace = entry is ImportFileEntry importEntry && importEntry.AllowReplace;
            if (destinationExisted && !allowReplace)
            {
                Editor.LogWarning("Cannot import because the destination already exists: " + destinationPath);
                return true;
            }

            var plan = new ContentMutationPlan(isCreate ? ContentMutationOperationKind.Create : ContentMutationOperationKind.ImportOutput);
            var steps = new List<ContentMutationStep>();
            var backupRoot = StringUtils.CombinePaths(Globals.ProjectCacheFolder, "ContentMutationBackups", plan.Id.ToString("N"));
            var backupPath = destinationExisted ? StringUtils.CombinePaths(backupRoot, Path.GetFileName(destinationPath)) : null;
            var existingItem = destinationExisted ? Editor.ContentDatabase.Find(destinationPath) : null;
            var existingAssetId = existingItem is AssetItem assetItem ? assetItem.ID : Guid.Empty;

            if (destinationExisted)
            {
                Directory.CreateDirectory(backupRoot);
                var backupEntryIndex = plan.Entries.Count;
                plan.Entries.Add(new ContentMutationEntry(destinationPath, backupPath, ContentMutationPathRole.ReplacementBackup, false));
                steps.Add(new ContentMutationStep(
                    "import-backup",
                    new[] { backupEntryIndex },
                    () =>
                    {
                        try
                        {
                            File.Copy(destinationPath, backupPath, false);
                            return ContentMutationResult.Success(destinationPath, backupPath);
                        }
                        catch (Exception ex)
                        {
                            return ContentMutationResult.Fail(ContentMutationFailure.CopyFailed, destinationPath, backupPath, ex.Message);
                        }
                    },
                    () => DeleteImportPath(backupPath),
                    () => File.Exists(backupPath) && new FileInfo(backupPath).Length == new FileInfo(destinationPath).Length));
            }

            var importEntryIndex = plan.Entries.Count;
            plan.Entries.Add(new ContentMutationEntry(sourcePath, destinationPath, ContentMutationPathRole.Main, false)
            {
                AllowExistingDestination = destinationExisted,
                SourceRequired = !isCreate,
            });
            steps.Add(new ContentMutationStep(
                "import-output",
                new[] { importEntryIndex },
                () => entry.Execute()
                    ? ContentMutationResult.Fail(ContentMutationFailure.VerificationFailure, sourcePath, destinationPath, "The importer reported a failure.")
                    : ContentMutationResult.Success(sourcePath, destinationPath),
                () => destinationExisted
                    ? RestoreImportBackup(backupPath, destinationPath, existingAssetId, existingItem?.ItemType == ContentItemType.Scene)
                    : DeleteImportedOutput(destinationPath),
                () => File.Exists(destinationPath) && new FileInfo(destinationPath).Length > 0));

            if (entry is TextureImportEntry { IsCanonicalSource: true } textureEntry)
            {
                var metadataPath = ContentMutationPathUtils.Normalize(textureEntry.MetadataPath);
                var metadataEntryIndex = plan.Entries.Count;
                plan.Entries.Add(new ContentMutationEntry(destinationPath, metadataPath, ContentMutationPathRole.MetadataSidecar, false)
                {
                    SourceProducedByTransaction = true,
                });
                steps.Add(new ContentMutationStep(
                    "texture-metadata",
                    new[] { metadataEntryIndex },
                    () => textureEntry.CreateMetadata()
                        ? ContentMutationResult.Fail(ContentMutationFailure.VerificationFailure, destinationPath, metadataPath, "Texture metadata creation or database registration failed.")
                        : ContentMutationResult.Success(destinationPath, metadataPath),
                    () => DeleteCanonicalMetadata(metadataPath),
                    () => File.Exists(metadataPath) && FlaxEngine.Content.GetAssetInfo(destinationPath, out var info) && info.ID != Guid.Empty));
            }
            else if (entry is ModelImportEntry { IsCanonicalSource: true } modelEntry)
            {
                AddCanonicalMetadataStep(plan, steps, destinationPath, modelEntry.MetadataPath, modelEntry.CreateMetadata, "model-metadata");
            }
            else if (entry is AudioImportEntry { IsCanonicalSource: true } audioEntry)
            {
                AddCanonicalMetadataStep(plan, steps, destinationPath, audioEntry.MetadataPath, audioEntry.CreateMetadata, "audio-metadata");
            }
            else if (entry is ImportFileEntry { IsCanonicalSource: true })
            {
                var metadataPath = destinationPath + ".meta";
                AddCanonicalMetadataStep(plan, steps, destinationPath, metadataPath, () => CreateImportedSourceMetadata(destinationPath), "imported-metadata");
            }

            var result = new ContentMutationTransaction(plan).Execute(steps);
            if (result.Succeeded && backupPath != null)
            {
                if (!DeleteImportPath(backupPath))
                {
                    var cleanupPlan = new ContentMutationPlan(ContentMutationOperationKind.Cleanup);
                    cleanupPlan.Entries.Add(new ContentMutationEntry(backupPath, destinationPath, ContentMutationPathRole.ReplacementBackup, false));
                    ContentMutationTransaction.PreserveRecoveryRecord(cleanupPlan, "A committed import left a replacement backup that could not be removed.");
                }
            }
            try
            {
                if (Directory.Exists(backupRoot) && !Directory.EnumerateFileSystemEntries(backupRoot).Any())
                    Directory.Delete(backupRoot, false);
            }
            catch (Exception ex)
            {
                Editor.LogWarning("Failed to clean import transaction backup folder: " + ex.Message);
            }
            ContentMutationDiagnostics.Log(result.Succeeded ? "mutation.import.committed" : "mutation.import.failed", $"transaction={plan.Id:N}; source='{sourcePath}'; destination='{destinationPath}'; replaced={destinationExisted}; failure={result.Failure}; recovery={result.RequiresRecovery}");
            return !result.Succeeded;
        }

        private static bool ExecuteInPlaceRegistrationTransaction(InPlaceCanonicalImportEntry entry)
        {
            var sourcePath = ContentMutationPathUtils.Normalize(entry.SourceUrl);
            var metadataPath = sourcePath + ".meta";
            var plan = new ContentMutationPlan(ContentMutationOperationKind.ImportOutput);
            var steps = new List<ContentMutationStep>();
            string backupRoot = null;
            string backupPath = null;
            if (entry.ReplaceForeignMetadata)
            {
                backupRoot = StringUtils.CombinePaths(Globals.ProjectCacheFolder, "ContentMutationBackups", plan.Id.ToString("N"));
                backupPath = StringUtils.CombinePaths(backupRoot, Path.GetFileName(metadataPath));
                Directory.CreateDirectory(backupRoot);
                var backupEntry = plan.Entries.Count;
                plan.Entries.Add(new ContentMutationEntry(metadataPath, backupPath, ContentMutationPathRole.ReplacementBackup, false));
                steps.Add(new ContentMutationStep(
                    "backup-foreign-metadata",
                    new[] { backupEntry },
                    () =>
                    {
                        File.Move(metadataPath, backupPath);
                        return ContentMutationResult.Success(metadataPath, backupPath);
                    },
                    () =>
                    {
                        if (File.Exists(backupPath) && !File.Exists(metadataPath))
                            File.Move(backupPath, metadataPath);
                        return File.Exists(metadataPath) && !File.Exists(backupPath);
                    },
                    () => !File.Exists(metadataPath) && File.Exists(backupPath)));
            }
            var metadataEntry = plan.Entries.Count;
            plan.Entries.Add(new ContentMutationEntry(sourcePath, metadataPath, ContentMutationPathRole.MetadataSidecar, false)
            {
                DestinationReleasedByTransaction = entry.ReplaceForeignMetadata,
                SourceRequired = true,
            });
            steps.Add(new ContentMutationStep(
                "register-in-place-source",
                new[] { metadataEntry },
                () => entry.Execute()
                    ? ContentMutationResult.Fail(ContentMutationFailure.VerificationFailure, sourcePath, metadataPath, "Canonical metadata creation or database registration failed.")
                    : ContentMutationResult.Success(sourcePath, metadataPath),
                () => DeleteCanonicalMetadata(metadataPath),
                () => File.Exists(metadataPath) && FlaxEngine.Content.GetAssetInfo(sourcePath, out var info) && info.ID != Guid.Empty));
            var result = new ContentMutationTransaction(plan).Execute(steps);
            if (result.Succeeded && backupPath != null)
            {
                DeleteImportPath(backupPath);
                if (Directory.Exists(backupRoot) && !Directory.EnumerateFileSystemEntries(backupRoot).Any())
                    Directory.Delete(backupRoot, false);
            }
            ContentMutationDiagnostics.Log(result.Succeeded ? "mutation.register-in-place.committed" : "mutation.register-in-place.failed",
                $"transaction={plan.Id:N}; source='{sourcePath}'; failure={result.Failure}; recovery={result.RequiresRecovery}");
            return !result.Succeeded;
        }

        private static bool RestoreImportBackup(string backupPath, string destinationPath, Guid assetId, bool isScene)
        {
            try
            {
                if (assetId != Guid.Empty && !isScene)
                    return !Editor.Instance.ContentEditing.CloneAssetFile(backupPath, destinationPath, assetId, true);

                var temporaryPath = ContentMutationPathUtils.CreateTemporarySibling(destinationPath, "flax-import-restore");
                File.Copy(backupPath, temporaryPath, false);
                if (File.Exists(destinationPath))
                    File.Replace(temporaryPath, destinationPath, null);
                else
                    File.Move(temporaryPath, destinationPath);
                return File.Exists(destinationPath) && new FileInfo(destinationPath).Length == new FileInfo(backupPath).Length;
            }
            catch (Exception ex)
            {
                Editor.LogWarning("Failed to restore import backup: " + ex.Message);
                return false;
            }
        }

        private static bool DeleteImportedOutput(string path)
        {
            try
            {
                if (FlaxEngine.Content.GetAssetInfo(path, out _))
                    FlaxEngine.Content.DeleteAsset(path);
                if (File.Exists(path))
                    File.Delete(path);
                return !File.Exists(path);
            }
            catch (Exception ex)
            {
                Editor.LogWarning("Failed to remove partial import output: " + ex.Message);
                return false;
            }
        }

        private static bool DeleteImportPath(string path)
        {
            try
            {
                if (File.Exists(path))
                    File.Delete(path);
                return !File.Exists(path);
            }
            catch (Exception ex)
            {
                Editor.LogWarning("Failed to remove import transaction path: " + ex.Message);
                return false;
            }
        }

        private static void AddCanonicalMetadataStep(ContentMutationPlan plan, List<ContentMutationStep> steps, string destinationPath, string metadataPath, Func<bool> createMetadata, string stepName)
        {
            metadataPath = ContentMutationPathUtils.Normalize(metadataPath);
            var metadataEntryIndex = plan.Entries.Count;
            plan.Entries.Add(new ContentMutationEntry(destinationPath, metadataPath, ContentMutationPathRole.MetadataSidecar, false)
            {
                SourceProducedByTransaction = true,
            });
            steps.Add(new ContentMutationStep(
                stepName,
                new[] { metadataEntryIndex },
                () => createMetadata()
                    ? ContentMutationResult.Fail(ContentMutationFailure.VerificationFailure, destinationPath, metadataPath, "Canonical metadata creation or database registration failed.")
                    : ContentMutationResult.Success(destinationPath, metadataPath),
                () => DeleteCanonicalMetadata(metadataPath),
                () => File.Exists(metadataPath)));
        }

        private static bool CreateImportedSourceMetadata(string destinationPath)
        {
            return CreateImportedSourceMetadataID(destinationPath) == Guid.Empty;
        }

        private static Guid CreateImportedSourceMetadataID(string destinationPath)
        {
            var extension = Path.GetExtension(destinationPath)?.ToLowerInvariant();
            string typeName;
            string processorId;
            switch (extension)
            {
            case ".ies":
                typeName = "FlaxEngine.IESProfile";
                processorId = "Flax.IES";
                break;
            case ".ttf":
            case ".otf":
                typeName = typeof(FontAsset).FullName;
                processorId = "Flax.Font";
                break;
            case ".shader":
                typeName = typeof(Shader).FullName;
                processorId = "Flax.ShaderSource";
                break;
            case ".mp4":
            case ".webm":
            case ".mov":
            case ".mkv":
                typeName = "FlaxEngine.Video";
                processorId = "Flax.Video";
                break;
            case ".txt":
                typeName = typeof(RawDataAsset).FullName;
                processorId = "Flax.Text";
                break;
            default:
                return Guid.Empty;
            }
            return AssetDatabaseFacade.CreateImportedSourceMetadata(destinationPath, typeName, processorId);
        }

        private static bool DeleteCanonicalMetadata(string path)
        {
            var deleted = DeleteImportPath(path);
            if (deleted)
            {
                var sourcePath = path.EndsWith(".meta", StringComparison.OrdinalIgnoreCase)
                    ? path.Substring(0, path.Length - 5)
                    : path;
                AssetDatabaseFacade.RefreshSources(new[] { sourcePath });
            }
            return deleted;
        }

        internal void LetThemBeImportedxD(List<ImportFileEntry> entries)
        {
            int count = entries.Count;
            if (count > 0)
            {
                lock (_requests)
                {
                    _importBatchSize += count;
                    for (int i = 0; i < count; i++)
                        _importingQueue.Enqueue(entries[i]);
                }

                StartWorker();
            }
        }

        internal void LetThemBeCreatedxD(CreateFileEntry entry)
        {
            lock (_requests)
            {
                _importBatchSize += 1;
                _importingQueue.Enqueue(entry);
            }

            StartWorker();
        }

        private void StartWorker()
        {
            if (_workerThread != null)
                return;

            _workerEndFlag = 0;
            _workerThread = new Thread(WorkerMain)
            {
                Name = "Content Importer",
                Priority = ThreadPriority.Highest
            };
            _workerThread.Start();
        }

        private void EndWorker()
        {
            if (_workerThread == null)
                return;

            Interlocked.Increment(ref _workerEndFlag);
            Thread.Sleep(0);

            _workerThread.Join(1000);
#if !USE_NETCORE
            _workerThread.Abort(); // Deprecated in .NET 7
#endif
            _workerThread = null;
        }

        /// <inheritdoc />
        public override void OnInit()
        {
            ImportFileEntry.RegisterDefaultTypes();
            ScriptedImporterRegistry.Initialize();
            ScriptsBuilder.ScriptsReloadBegin += OnScriptsReloadBegin;
        }

        private void OnScriptsReloadBegin()
        {
            // Remove import file types from scripting assemblies
            List<string> removeFileTypes = new List<string>();
            foreach (var pair in ImportFileEntry.FileTypes)
            {
                if (pair.Value.Method.IsCollectible || (pair.Value.Target != null && pair.Value.Target.GetType().IsCollectible))
                    removeFileTypes.Add(pair.Key);
            }
            foreach (var fileType in removeFileTypes)
                ImportFileEntry.FileTypes.Remove(fileType);
        }

        /// <inheritdoc />
        public override void OnUpdate()
        {
            lock (_requests)
            {
                // Check if has no requests to process
                if (_requests.Count == 0)
                    return;
                try
                {
                    // Get entries
                    List<ImportFileEntry> entries = new List<ImportFileEntry>(_requests.Count);
                    bool needSettingsDialog = false;
                    for (int i = 0; i < _requests.Count; i++)
                    {
                        var request = _requests[i];
                        var entry = ImportFileEntry.CreateEntry(ref request);
                        if (entry != null)
                        {
                            if (request.Settings != null && entry.TryOverrideSettings(request.Settings))
                            {
                                // Use overridden settings
                            }
                            else if (!request.SkipSettingsDialog)
                            {
                                needSettingsDialog |= entry.HasSettings;
                            }

                            entries.Add(entry);
                        }
                    }
                    _requests.Clear();

                    // Check if need to show importing dialog or can just pass requests
                    if (needSettingsDialog)
                    {
                        var dialog = new ImportFilesDialog(entries);
                        dialog.Show(Editor.Windows.MainWindow);
                        dialog.Focus();
                    }
                    else
                    {
                        LetThemBeImportedxD(entries);
                    }
                }
                catch (Exception ex)
                {
                    // Error
                    Editor.LogWarning(ex);
                    Editor.LogError("Failed to process files import request.");
                }
            }
        }

        /// <inheritdoc />
        public override void OnExit()
        {
            ScriptsBuilder.ScriptsReloadBegin -= OnScriptsReloadBegin;
            ScriptedImporterRegistry.Shutdown();
            EndWorker();
        }
    }
}
