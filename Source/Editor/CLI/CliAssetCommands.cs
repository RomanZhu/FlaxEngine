// Copyright (c) Wojciech Figat. All rights reserved.

using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Reflection;
using FlaxEditor.Actions;
using FlaxEditor.Content;
using FlaxEditor.Content.Documents;
using FlaxEditor.Content.Import;
using FlaxEditor.Content.Settings;
using FlaxEditor.Windows.Assets;
using FlaxEngine;
using FlaxEngine.Tools;
using Newtonsoft.Json;
using Newtonsoft.Json.Linq;
using FlaxJsonSerializer = FlaxEngine.Json.JsonSerializer;

namespace FlaxEditor
{
    internal enum CliAssetVerificationStep
    {
        RequestBuild,
        WaitForBuild,
        Load,
        Fail,
    }

    internal static class CliAssetPersistence
    {
        public static void PrepareForSave(Asset asset, string propertyPath)
        {
            if (asset is not JsonAsset jsonAsset || string.IsNullOrWhiteSpace(propertyPath))
                return;

            var separator = propertyPath.IndexOf('.');
            var rootMember = separator == -1 ? propertyPath : propertyPath.Substring(0, separator);
            if (string.Equals(rootMember, nameof(JsonAsset.Instance), StringComparison.OrdinalIgnoreCase))
                jsonAsset.SetInstance(jsonAsset.Instance);
        }

        public static void PersistTags(IEnumerable<string> tagNames)
        {
            var names = tagNames?
                .Where(x => !string.IsNullOrWhiteSpace(x))
                .SelectMany(EnumerateTagHierarchy)
                .Distinct(StringComparer.Ordinal)
                .ToArray() ?? Array.Empty<string>();
            if (names.Length == 0)
                return;

            var settingsAsset = GameSettings.LoadAsset<LayersAndTagsSettings>();
            if (!settingsAsset || settingsAsset.WaitForLoaded())
                throw new InvalidOperationException("Failed to load the Layers and Tags settings asset.");

            // Preserve pending edits owned by an open settings window before updating the same asset.
            var settingsItem = Editor.Instance.ContentDatabase.FindAsset(settingsAsset.ID) as JsonAssetItem;
            var assetWindow = Editor.Instance.Windows.FindEditor(settingsItem) as JsonAssetWindow;
            assetWindow?.Save();

            var settings = (LayersAndTagsSettings)settingsAsset.Instance;
            var missing = names.Where(x => !settings.Tags.Contains(x)).ToArray();
            if (missing.Length == 0)
                return;
            foreach (var name in missing)
            {
                if (!settings.Tags.Contains(name))
                    settings.Tags.Add(name);
            }
            settings.Tags.Sort(StringComparer.Ordinal);
            settingsAsset.SetInstance(settings);
            if (Editor.Instance.ContentDatabase.SaveAsset(settingsAsset))
                throw new InvalidOperationException("Failed to save the Layers and Tags settings asset.");
            assetWindow?.RefreshAsset();
        }

        private static IEnumerable<string> EnumerateTagHierarchy(string tagName)
        {
            tagName = tagName.Trim();
            for (int i = 0; i < tagName.Length; i++)
            {
                if (tagName[i] == '.' && i != 0)
                    yield return tagName.Substring(0, i);
            }
            yield return tagName;
        }
    }

    /// <summary>
    /// Describes one asset operation for the typed CLI asset commands.
    /// </summary>
    public sealed class CliAssetOperationOptions
    {
        /// <summary>
        /// Gets or sets the operation name.
        /// </summary>
        [JsonProperty("action")]
        public string Action { get; set; }

        /// <summary>
        /// Gets or sets the primary Content path.
        /// </summary>
        [JsonProperty("path")]
        public string Path { get; set; }

        /// <summary>
        /// Gets or sets the destination path.
        /// </summary>
        [JsonProperty("destination")]
        public string Destination { get; set; }

        /// <summary>
        /// Gets or sets import source paths.
        /// </summary>
        [JsonProperty("sources")]
        public string[] Sources { get; set; }

        /// <summary>
        /// Gets or sets the asset type used for creation.
        /// </summary>
        [JsonProperty("assetType")]
        public string AssetType { get; set; }

        /// <summary>
        /// Gets or sets typed importer options.
        /// </summary>
        [JsonProperty("importOptions")]
        public JObject ImportOptions { get; set; }

        /// <summary>
        /// Gets or sets a public property path.
        /// </summary>
        [JsonProperty("propertyPath")]
        public string PropertyPath { get; set; }

        /// <summary>
        /// Gets or sets a property value.
        /// </summary>
        [JsonProperty("value")]
        public JToken Value { get; set; }

        /// <summary>
        /// Gets or sets the base material path for a material-instance operation.
        /// </summary>
        [JsonProperty("baseMaterial")]
        public string BaseMaterial { get; set; }

        /// <summary>
        /// Gets or sets material parameter overrides.
        /// </summary>
        [JsonProperty("parameters")]
        public JObject Parameters { get; set; }

        /// <summary>
        /// Gets or sets the existing-output policy: error, skip, or update.
        /// </summary>
        [JsonProperty("ifExists")]
        public string IfExists { get; set; } = "error";

        /// <summary>
        /// Gets or sets whether folder traversal is recursive.
        /// </summary>
        [JsonProperty("recursive")]
        public bool Recursive { get; set; }

        /// <summary>
        /// Gets or sets whether a destructive action was confirmed.
        /// </summary>
        [JsonProperty("force")]
        public bool Force { get; set; }

        /// <summary>
        /// Gets or sets whether the modified asset is saved.
        /// </summary>
        [JsonProperty("save")]
        public bool Save { get; set; } = true;
    }

    /// <summary>
    /// Built-in typed commands for single and batched asset operations.
    /// </summary>
    public static class CliAssetCommands
    {
        internal static CliAssetVerificationStep GetVerificationStep(bool requiresArtifactBuild, bool buildRequested, bool artifactCurrent, string buildStatus)
        {
            if (!requiresArtifactBuild || artifactCurrent)
                return CliAssetVerificationStep.Load;
            if (string.Equals(buildStatus, "Failed", StringComparison.Ordinal) ||
                string.Equals(buildStatus, "Cancelled", StringComparison.Ordinal))
                return CliAssetVerificationStep.Fail;
            if (!buildRequested)
                return CliAssetVerificationStep.RequestBuild;
            if (string.Equals(buildStatus, "NotBuilt", StringComparison.Ordinal) ||
                string.Equals(buildStatus, "Queued", StringComparison.Ordinal) ||
                string.Equals(buildStatus, "Building", StringComparison.Ordinal) ||
                string.Equals(buildStatus, "Publishing", StringComparison.Ordinal) ||
                string.Equals(buildStatus, "ReadyExact", StringComparison.Ordinal))
                return CliAssetVerificationStep.WaitForBuild;
            return CliAssetVerificationStep.Fail;
        }

        internal static bool ShouldForceVerificationReload(bool requiresArtifactBuild)
        {
            // Canonical publication already hot-swaps loaded assets. Reloading the same generated
            // storage again can race that swap and does not add persistence coverage.
            return !requiresArtifactBuild;
        }

        /// <summary>
        /// Executes one Editor-owned asset operation.
        /// </summary>
        [CliCommand("assets.execute", Description = "Execute one Editor-owned asset operation.", Access = CliCommandAccess.MutatesProject)]
        public static CliCommandOperation Execute(
            CliCommandContext context,
            [CliOption("operation", Description = "The asset operation payload.", Required = true)] CliAssetOperationOptions operation,
            [CliOption("verify-reload", Description = "Reload changed assets before reporting success.")] bool verifyReload = false)
        {
            return new AssetBatchOperation(context, new[] { operation }, false, verifyReload, true);
        }

        /// <summary>
        /// Executes many Editor-owned asset operations in one Editor session.
        /// </summary>
        [CliCommand("assets.batch", Description = "Execute a resumable batch of Editor-owned asset operations.", Access = CliCommandAccess.MutatesProject)]
        public static CliCommandOperation Batch(
            CliCommandContext context,
            [CliOption("operations", Description = "Ordered asset operations.", Required = true)] CliAssetOperationOptions[] operations,
            [CliOption("continue-on-error", Description = "Continue after an item fails.")] bool continueOnError = false,
            [CliOption("verify-reload", Description = "Reload changed assets before reporting success.")] bool verifyReload = false)
        {
            return new AssetBatchOperation(context, operations, continueOnError, verifyReload, false);
        }

        private sealed class AssetBatchOperation : CliCommandOperation
        {
            private sealed class AssetVerification
            {
                public string Path;
                public string PropertyPath;
                public JToken ExpectedValue;
            }

            private readonly CliCommandContext _context;
            private readonly CliAssetOperationOptions[] _operations;
            private readonly bool _continueOnError;
            private readonly bool _verifyReload;
            private readonly bool _single;
            private readonly List<object> _results = new List<object>();
            private readonly List<AssetVerification> _verifications = new List<AssetVerification>();
            private readonly object _importLocker = new object();
            private readonly List<object> _importedAssets = new List<object>();
            private CliCommandResult _result;
            private int _index;
            private int _succeeded;
            private int _failed;
            private int _verifyIndex;
            private bool _waitingForImport;
            private bool _importCompleted;
            private bool _importFailed;
            private bool _cancelled;
            private AssetVerification _pendingVerification;
            private AssetItem _pendingVerificationItem;
            private Asset _pendingVerificationAsset;
            private bool _pendingVerificationIsAction;
            private bool _pendingVerificationBuildRequested;

            public AssetBatchOperation(CliCommandContext context, CliAssetOperationOptions[] operations, bool continueOnError, bool verifyReload, bool single)
            {
                _context = context ?? throw new ArgumentNullException(nameof(context));
                _operations = operations ?? throw new ArgumentNullException(nameof(operations));
                if (_operations.Length == 0)
                    throw new ArgumentException("At least one asset operation is required.", nameof(operations));
                if (_operations.Any(x => x == null))
                    throw new ArgumentException("Asset operations cannot contain null entries.", nameof(operations));
                _continueOnError = continueOnError;
                _verifyReload = verifyReload;
                _single = single;
            }

            public override bool IsCompleted => _result != null;

            public override CliCommandResult Result => _result;

            public override void Update(TimeSpan timeBudget)
            {
                if (_result != null)
                    return;
                if (_cancelled || _context.CancellationToken.IsCancellationRequested)
                {
                    Cancel();
                    _result = CliCommandResult.Failure("FLX-ASSET-CANCELLED-0006", "The asset operation was cancelled.");
                    return;
                }

                if (_waitingForImport)
                {
                    CompleteImportIfReady();
                    return;
                }

                if (_pendingVerification != null)
                {
                    CompleteVerificationIfReady();
                    return;
                }

                if (_index < _operations.Length)
                {
                    ExecuteNext();
                    return;
                }

                if (_verifyReload && _verifyIndex < _verifications.Count)
                {
                    VerifyNext();
                    return;
                }

                Complete();
            }

            public override void Cancel()
            {
                if (_cancelled)
                    return;
                _cancelled = true;
                DetachImportEvents();
                ClearPendingVerification();
            }

            private void ExecuteNext()
            {
                var operation = _operations[_index];
                var action = (operation.Action ?? string.Empty).Trim().ToLowerInvariant();
                _context.ReportProgress($"Asset {_index + 1}/{_operations.Length}: {action}", (float)_index / _operations.Length);
                try
                {
                    object data;
                    switch (action)
                    {
                    case "list": data = ListAssets(operation); break;
                    case "types": data = ListAssetTypes(operation); break;
                    case "info": data = DescribeAsset(RequireItem(operation.Path)); break;
                    case "select": data = SelectAsset(operation); break;
                    case "open": data = OpenAssetEditor(operation); break;
                    case "create": data = CreateAsset(operation); break;
                    case "mkdir": data = CreateFolder(operation); break;
                    case "import": BeginImport(operation, false); return;
                    case "duplicate": data = DuplicateAsset(operation); break;
                    case "move": data = MoveAsset(operation); break;
                    case "delete": data = DeleteAsset(operation); break;
                    case "reimport": BeginImport(operation, true); return;
                    case "export": data = ExportAsset(operation); break;
                    case "get": data = GetAssetProperty(operation); break;
                    case "set": data = SetAssetProperty(operation); break;
                    case "save": data = SaveAsset(operation); break;
                    case "refresh": data = RefreshContent(operation); break;
                    case "verify": BeginVerification(new AssetVerification { Path = operation.Path }, true); return;
                    case "material-instance": data = ConfigureMaterialInstance(operation); break;
                    default: throw new InvalidOperationException($"Unsupported asset action '{operation.Action}'.");
                    }
                    RecordSuccess(action, data);
                }
                catch (Exception ex)
                {
                    RecordFailure(action, ex);
                }
            }

            private void RecordSuccess(string action, object data)
            {
                _results.Add(new { index = _index, action, success = true, data });
                _succeeded++;
                _index++;
            }

            private void RecordFailure(string action, Exception exception)
            {
                _results.Add(new { index = _index, action, success = false, error = new { code = "FLX-ASSET-BATCH-0006", message = exception.Message } });
                _failed++;
                _index++;
                if (!_continueOnError)
                    Complete();
            }

            private void Complete()
            {
                DetachImportEvents();
                _context.ReportProgress("Asset batch complete", 1.0f);
                var summary = new
                {
                    total = _operations.Length,
                    succeeded = _succeeded,
                    failed = _failed,
                    verified = _verifyReload ? _verifyIndex : 0,
                    results = _results.ToArray(),
                };
                if (_failed != 0)
                {
                    _result = CliCommandResult.Failure("FLX-ASSET-BATCH-0006", $"{_failed} of {_operations.Length} asset operations failed.", summary);
                    return;
                }
                if (_single)
                {
                    var token = JToken.FromObject(_results[0]);
                    _result = CliCommandResult.Success(token["data"]);
                    return;
                }
                _result = CliCommandResult.Success(summary);
            }

            private object ListAssets(CliAssetOperationOptions options)
            {
                var item = RequireItem(string.IsNullOrWhiteSpace(options.Path) ? Globals.ProjectContentFolder : options.Path);
                IEnumerable<ContentItem> items = item is ContentFolder folder
                    ? options.Recursive ? EnumerateChildren(folder) : folder.Children
                    : new[] { item };
                return items.Select(DescribeAsset).ToArray();
            }

            private object ListAssetTypes(CliAssetOperationOptions options)
            {
                var folder = RequireFolder(string.IsNullOrWhiteSpace(options.Path) ? Globals.ProjectContentFolder : options.Path);
                return Editor.Instance.ContentDatabase.Proxy
                    .Where(x => x.IsAsset && x.CanCreate(folder))
                    .Select(x => new { name = x.Name, extension = x.FileExtension, defaultName = x.NewItemName })
                    .OrderBy(x => x.name, StringComparer.OrdinalIgnoreCase)
                    .ToArray();
            }

            private static object SelectAsset(CliAssetOperationOptions options)
            {
                var item = RequireItem(options.Path) as AssetItem
                           ?? throw new InvalidOperationException($"Content item '{options.Path}' is not an asset.");
                Editor.Instance.Windows.ContentWin.Select(item, true);
                return DescribeAsset(item);
            }

            private static object OpenAssetEditor(CliAssetOperationOptions options)
            {
                var item = RequireItem(options.Path) as AssetItem
                           ?? throw new InvalidOperationException($"Content item '{options.Path}' is not an asset.");
                var proxy = Editor.Instance.ContentDatabase.GetProxy(item)
                            ?? throw new InvalidOperationException($"Asset '{item.Path}' has no Project-panel proxy.");
                var window = Editor.Instance.ContentEditing.Open(item)
                             ?? throw new InvalidOperationException($"Asset '{item.Path}' has no compatible editor factory.");
                return new
                {
                    path = item.Path,
                    type = item.TypeName,
                    proxy = proxy.GetType().FullName,
                    editor = window.GetType().FullName,
                    canonical = item.IsCanonicalSource,
                    processor = item.ProcessorID,
                };
            }

            private object CreateAsset(CliAssetOperationOptions options)
            {
                if (string.IsNullOrWhiteSpace(options.AssetType))
                    throw new InvalidOperationException("Asset creation requires an asset type.");
                var requestedPath = RequireProjectContentPath(options.Path);
                var parent = RequireExistingParent(requestedPath);
                var proxy = Editor.Instance.ContentDatabase.Proxy.FirstOrDefault(x =>
                    x.IsAsset && x.CanCreate(parent) && IsAssetTypeMatch(x, options.AssetType));
                if (proxy == null)
                    throw new InvalidOperationException($"Asset type '{options.AssetType}' is not available in '{parent.Path}'.");
                var path = EnsureAssetExtension(requestedPath, proxy.FileExtension);
                var existing = FindExisting(path);
                if (existing != null)
                    return HandleExisting(existing, options.IfExists);
                path = RequireNewPath(path);
                var createResult = Editor.Instance.ContentDatabase.CreatePath(path, false, () => CreateAssetFile(proxy, path));
                if (!createResult.Succeeded)
                    throw new InvalidOperationException(createResult.Message ?? $"Failed to create {options.AssetType} asset '{path}'.");
                RefreshPath(path, false);
                RequireItem(path);
                TrackVerification(path);
                return DescribePath(path, options.AssetType);
            }

            private static void CreateAssetFile(ContentProxy proxy, string path)
            {
                if (proxy is VisualScriptProxy)
                {
                    if (Editor.CreateVisualScript(path, typeof(Script).FullName))
                        throw new IOException($"Failed to create Visual Script asset '{path}'.");
                    return;
                }
                if (proxy is ParticleEmitterProxy && System.IO.Path.GetExtension(path).Equals(".particleemitter", StringComparison.OrdinalIgnoreCase))
                {
                    if (AssetDocumentRegistry.CreateGraph(path, typeof(ParticleEmitter).FullName) == Guid.Empty)
                        throw new IOException($"Failed to create Particle Emitter asset '{path}'.");
                    return;
                }
                proxy.Create(path, null);
            }

            private static bool IsAssetTypeMatch(ContentProxy proxy, string requestedType)
            {
                if (string.Equals(proxy.Name, requestedType, StringComparison.OrdinalIgnoreCase))
                    return true;
                var proxyName = new string(proxy.Name.Where(char.IsLetterOrDigit).ToArray());
                var requestedName = new string(requestedType.Where(char.IsLetterOrDigit).ToArray());
                return string.Equals(proxyName, requestedName, StringComparison.OrdinalIgnoreCase);
            }

            private object CreateFolder(CliAssetOperationOptions options)
            {
                var existing = FindExisting(options.Path);
                if (existing != null)
                    return HandleExisting(existing, options.IfExists);
                var path = RequireNewPath(options.Path);
                var parent = RequireExistingParent(path);
                var createResult = Editor.Instance.ContentDatabase.CreatePath(path, true, () => Directory.CreateDirectory(path));
                if (!createResult.Succeeded)
                    throw new InvalidOperationException(createResult.Message ?? $"Failed to create folder '{path}'.");
                AssetDatabase.ImportAsset(path, ImportAssetOptions.ImportRecursive);
                Editor.Instance.ContentDatabase.RefreshFolder(parent, true);
                return DescribeAsset(RequireFolder(path));
            }

            private void BeginImport(CliAssetOperationOptions options, bool reimport)
            {
                lock (_importLocker)
                {
                    _importedAssets.Clear();
                    _importCompleted = false;
                    _importFailed = false;
                    _waitingForImport = true;
                }
                var importing = Editor.Instance.ContentImporting;
                importing.ImportFileEnd += OnAssetImportFileEnd;
                importing.ImportingQueueEnd += OnAssetImportQueueEnd;
                try
                {
                    if (reimport)
                    {
                        var item = RequireItem(options.Path) as BinaryAssetItem
                                   ?? throw new InvalidOperationException("Only binary assets with import metadata can be reimported.");
                        var proxy = Editor.Instance.ContentDatabase.GetProxy(item);
                        if (proxy == null || !proxy.CanReimport(item))
                            throw new InvalidOperationException($"Asset '{item.Path}' does not support reimport.");
                        if (item.GetImportPath(out var importPath) || !File.Exists(importPath))
                            throw new FileNotFoundException($"The import source for asset '{item.Path}' does not exist.", importPath);
                        importing.Reimport(item, CreateImportSettings(options.AssetType, options.ImportOptions), true);
                    }
                    else
                    {
                        if (options.Sources == null || options.Sources.Length == 0)
                            throw new InvalidOperationException("Asset import requires at least one source path.");
                        var target = RequireFolder(options.Destination);
                        var sources = options.Sources.Select(System.IO.Path.GetFullPath).ToArray();
                        foreach (var source in sources)
                        {
                            if (!File.Exists(source) && !Directory.Exists(source))
                                throw new FileNotFoundException($"Import source '{source}' does not exist.", source);
                        }
                        var preflight = importing.PreflightImport(sources, target);
                        if (!preflight.Succeeded)
                            throw new InvalidOperationException(preflight.Message ?? $"Asset import preflight failed ({preflight.Failure}).");
                        importing.Import(sources, target, true, CreateImportSettings(options.AssetType, options.ImportOptions));
                    }
                }
                catch
                {
                    lock (_importLocker)
                        _waitingForImport = false;
                    DetachImportEvents();
                    throw;
                }
            }

            private static object CreateImportSettings(string assetType, JObject importOptions)
            {
                if (string.IsNullOrWhiteSpace(assetType) && importOptions == null)
                    return null;
                var modelOptions = importOptions == null
                    ? ModelTool.Options.Default
                    : JsonConvert.DeserializeObject<ModelTool.Options>(importOptions.ToString(Formatting.None), FlaxJsonSerializer.Settings);
                if (!string.IsNullOrWhiteSpace(assetType))
                {
                    if (!Enum.TryParse(assetType, true, out ModelTool.ModelType modelType))
                        throw new InvalidOperationException($"Unsupported import asset type '{assetType}'.");
                    modelOptions.Type = modelType;
                }
                return new ModelImportSettings { Settings = modelOptions };
            }

            private void OnAssetImportFileEnd(FlaxEditor.Content.IFileEntryAction entry, bool failed)
            {
                lock (_importLocker)
                {
                    _importFailed |= failed;
                    _importedAssets.Add(new { source = entry.SourceUrl, path = entry.ResultUrl, success = !failed });
                    if (!failed)
                        TrackVerification(entry.ResultUrl);
                }
            }

            private void OnAssetImportQueueEnd()
            {
                lock (_importLocker)
                    _importCompleted = true;
            }

            private void CompleteImportIfReady()
            {
                object[] imported;
                bool failed;
                lock (_importLocker)
                {
                    if (!_importCompleted)
                        return;
                    imported = _importedAssets.ToArray();
                    failed = _importFailed;
                    _waitingForImport = false;
                }
                DetachImportEvents();
                var action = (_operations[_index].Action ?? string.Empty).Trim().ToLowerInvariant();
                if (failed)
                    RecordFailure(action, new InvalidOperationException("One or more assets failed to import."));
                else
                    RecordSuccess(action, imported);
            }

            private void DetachImportEvents()
            {
                var editor = Editor.Instance;
                if (editor == null)
                    return;
                var importing = editor.ContentImporting;
                importing.ImportFileEnd -= OnAssetImportFileEnd;
                importing.ImportingQueueEnd -= OnAssetImportQueueEnd;
            }

            private object DuplicateAsset(CliAssetOperationOptions options)
            {
                var item = RequireItem(options.Path);
                var existing = FindExisting(options.Destination);
                if (existing != null)
                    return HandleExisting(existing, options.IfExists);
                var destination = RequireNewPath(options.Destination);
                RequireExistingParent(destination);
                var copyResult = Editor.Instance.ContentDatabase.Copy(item, destination);
                if (!copyResult.Succeeded)
                    throw new InvalidOperationException(copyResult.Message ?? $"Failed to duplicate asset to '{destination}'.");
                RefreshPath(destination, false);
                TrackVerification(destination);
                return DescribeAsset(RequireItem(destination));
            }

            private object MoveAsset(CliAssetOperationOptions options)
            {
                var item = RequireItem(options.Path);
                var destination = RequireMoveDestination(item, options.Destination);
                RequireExistingParent(destination);
                var moveResult = Editor.Instance.ContentDatabase.TryMove(new[] { (item, destination) });
                if (!moveResult.Succeeded)
                    throw new InvalidOperationException(moveResult.Message ?? $"Failed to move asset to '{destination}' ({moveResult.Failure}).");
                RefreshPath(destination, false);
                TrackVerification(destination);
                return DescribeAsset(RequireItem(destination));
            }

            private object DeleteAsset(CliAssetOperationOptions options)
            {
                if (!options.Force)
                    throw new InvalidOperationException("Asset deletion requires explicit confirmation.");
                var item = RequireItem(options.Path);
                var path = item.Path;
                if (PathEquals(path, Globals.ProjectContentFolder))
                    throw new InvalidOperationException("The project Content root cannot be deleted.");
                if (item is AssetItem { IsCanonicalSource: true })
                {
                    Editor.Instance.ContentDatabase.Delete(item, true);
                }
                else
                {
                    var action = ContentItemFilesystemAction.Delete(Editor.Instance, new List<ContentItem> { item });
                    if (action == null)
                        throw new InvalidOperationException($"Failed to stage asset deletion for '{path}'.");
                    action.Dispose();
                }
                if (File.Exists(path) || Directory.Exists(path))
                    throw new InvalidOperationException($"Failed to delete asset '{path}'.");
                if (File.Exists(path + ".meta"))
                    throw new InvalidOperationException($"Failed to delete asset metadata '{path}.meta'.");
                return new { path, deleted = true };
            }

            private object ExportAsset(CliAssetOperationOptions options)
            {
                var item = RequireItem(options.Path);
                var destination = System.IO.Path.GetFullPath(options.Destination ?? throw new InvalidOperationException("Asset export requires a destination folder."));
                Directory.CreateDirectory(destination);
                var proxy = Editor.Instance.ContentDatabase.GetProxy(item);
                if (proxy != null && proxy.CanExport)
                    proxy.Export(item, destination);
                else if (item.IsAsset && Editor.CanExport(item.Path))
                {
                    if (Editor.Export(item.Path, destination))
                        throw new InvalidOperationException($"Failed to export asset '{item.Path}'.");
                }
                else
                    throw new InvalidOperationException($"Asset '{item.Path}' does not support export.");
                return new { path = item.Path, destination };
            }

            private object GetAssetProperty(CliAssetOperationOptions options)
            {
                var asset = LoadAsset(options.Path);
                var value = GetMemberPathValue(asset, options.PropertyPath);
                return new
                {
                    path = RequireItem(options.Path).Path,
                    property = options.PropertyPath,
                    value = value == null ? JValue.CreateNull() : JToken.Parse(FlaxJsonSerializer.Serialize(value)),
                    valueType = value?.GetType().FullName,
                };
            }

            private object SetAssetProperty(CliAssetOperationOptions options)
            {
                if (options.Value == null)
                    throw new InvalidOperationException("Asset property assignment requires a JSON value.");
                var asset = LoadAsset(options.Path);
                var member = SetMemberPathValue(asset, options.PropertyPath, options.Value);
                var assignedValue = GetMemberPathValue(asset, options.PropertyPath);
                if (options.Save)
                {
                    if (assignedValue is Tag tag)
                        CliAssetPersistence.PersistTags(new[] { tag.ToString() });
                    CliAssetPersistence.PrepareForSave(asset, options.PropertyPath);
                    if (Editor.Instance.ContentDatabase.SaveAsset(asset))
                        throw new InvalidOperationException($"Failed to save asset '{options.Path}'.");
                    TrackVerification(options.Path, options.PropertyPath, assignedValue);
                }
                return new { path = RequireItem(options.Path).Path, property = options.PropertyPath, value = options.Value, valueType = GetMemberType(member).FullName, saved = options.Save };
            }

            private object SaveAsset(CliAssetOperationOptions options)
            {
                var asset = LoadAsset(options.Path);
                if (Editor.Instance.ContentDatabase.SaveAsset(asset))
                    throw new InvalidOperationException($"Failed to save asset '{options.Path}'.");
                TrackVerification(options.Path);
                return new { path = RequireItem(options.Path).Path, saved = true };
            }

            private object RefreshContent(CliAssetOperationOptions options)
            {
                var path = RequireProjectContentPath(string.IsNullOrWhiteSpace(options.Path) ? Globals.ProjectContentFolder : options.Path);
                RefreshPath(path, options.Recursive);
                return new { path, recursive = options.Recursive, registered = Editor.Instance.ContentDatabase.Find(path) != null };
            }

            private object ConfigureMaterialInstance(CliAssetOperationOptions options)
            {
                var path = EnsureAssetExtension(options.Path, "materialinstance");
                var existing = FindExisting(path);
                if (existing == null)
                {
                    path = RequireNewPath(path);
                    RequireExistingParent(path);
                    Editor.Instance.ContentDatabase.GetProxy<MaterialInstance>().Create(path, null);
                    RefreshPath(path, false);
                }
                else if (NormalizeIfExists(options.IfExists) == "error")
                {
                    throw new IOException($"The destination '{path}' already exists.");
                }
                else if (NormalizeIfExists(options.IfExists) == "skip")
                {
                    return HandleExisting(existing, "skip");
                }

                var instance = LoadAsset(path) as MaterialInstance
                               ?? throw new InvalidOperationException($"Asset '{path}' is not a MaterialInstance.");
                if (!string.IsNullOrWhiteSpace(options.BaseMaterial))
                    instance.BaseMaterial = LoadAsset(options.BaseMaterial) as MaterialBase
                                            ?? throw new InvalidOperationException($"Asset '{options.BaseMaterial}' is not a material.");
                if (instance.BaseMaterial == null)
                    throw new InvalidOperationException("A material-instance operation requires baseMaterial or an existing base material.");

                var parameters = options.Parameters ?? new JObject();
                foreach (var property in parameters.Properties())
                {
                    var parameter = instance.Parameters.FirstOrDefault(x => string.Equals(x.Name, property.Name, StringComparison.OrdinalIgnoreCase));
                    if (parameter == null)
                        throw new InvalidOperationException($"Material parameter '{property.Name}' was not found on '{path}'.");
                    instance.SetParameterValue(parameter.Name, ConvertMaterialParameter(parameter, property.Value));
                }
                if (options.Save && Editor.Instance.ContentDatabase.SaveAsset(instance))
                    throw new InvalidOperationException($"Failed to save material instance '{path}'.");
                if (options.Save)
                    TrackVerification(path);
                return new
                {
                    path,
                    baseMaterial = instance.BaseMaterial.Path,
                    parameters = parameters.Properties().Select(x => x.Name).ToArray(),
                    saved = options.Save,
                };
            }

            private object ConvertMaterialParameter(MaterialParameter parameter, JToken token)
            {
                if (token == null || token.Type == JTokenType.Null)
                    return null;
                if (token.Type == JTokenType.String)
                {
                    var value = token.Value<string>();
                    switch (parameter.ParameterType)
                    {
                    case MaterialParameterType.Texture:
                    case MaterialParameterType.NormalMap:
                        return LoadAsset(value) as Texture ?? throw new InvalidOperationException($"Asset '{value}' is not a Texture.");
                    case MaterialParameterType.CubeTexture:
                        return LoadAsset(value) as CubeTexture ?? throw new InvalidOperationException($"Asset '{value}' is not a CubeTexture.");
                    }
                }
                var current = parameter.Value;
                var targetType = current?.GetType() ?? typeof(object);
                return JsonConvert.DeserializeObject(token.ToString(Formatting.None), targetType, FlaxJsonSerializer.Settings);
            }

            private void VerifyNext()
            {
                var verification = _verifications[_verifyIndex];
                try
                {
                    BeginVerification(verification, false);
                }
                catch (Exception ex)
                {
                    RecordVerificationFailure(verification, ex);
                }
            }

            private void BeginVerification(AssetVerification verification, bool isAction)
            {
                var path = RequireProjectContentPath(verification.Path);
                RefreshPath(path, false);
                var item = RequireItem(path) as AssetItem
                           ?? throw new InvalidOperationException($"Content item '{path}' is not an asset.");
                if (!File.Exists(item.Path))
                    throw new FileNotFoundException($"Asset '{item.Path}' was not persisted.", item.Path);
                _pendingVerification = verification;
                _pendingVerificationItem = item;
                _pendingVerificationAsset = null;
                _pendingVerificationIsAction = isAction;
                _pendingVerificationBuildRequested = false;
            }

            private void CompleteVerificationIfReady()
            {
                var verification = _pendingVerification;
                var item = _pendingVerificationItem;
                var isAction = _pendingVerificationIsAction;
                try
                {
                    var asset = _pendingVerificationAsset;
                    if (asset == null)
                    {
                        var hasRecord = AssetDatabaseQueryService.TryGetRecord(item.ObjectID, out var record);
                        if (item.IsCanonicalSource && !hasRecord)
                            return;
                        var requiresBuild = item.IsCanonicalSource && record.SourceKind != AssetSourceKind.Folder;
                        var artifactCurrent = !requiresBuild || AssetPipelineService.IsArtifactCurrent(item.ID);
                        var buildStatus = requiresBuild ? AssetPipelineService.GetBuildStatus(item.ID) : "ReadyExact";
                        switch (GetVerificationStep(requiresBuild, _pendingVerificationBuildRequested, artifactCurrent, buildStatus))
                        {
                        case CliAssetVerificationStep.RequestBuild:
                            if (AssetPipelineService.BuildAssetForeground(item.ID))
                                throw new InvalidOperationException($"Asset '{item.Path}' failed to request an asynchronous artifact build.");
                            _pendingVerificationBuildRequested = true;
                            return;
                        case CliAssetVerificationStep.WaitForBuild:
                            return;
                        case CliAssetVerificationStep.Fail:
                            throw new InvalidOperationException($"Asset '{item.Path}' artifact build ended with status '{buildStatus}'.");
                        }

                        if (ShouldForceVerificationReload(requiresBuild))
                            item.Reload();
                        asset = item.LoadAsync();
                        if (asset == null)
                            throw new InvalidOperationException($"Asset '{item.Path}' failed to reload from disk.");
                        _pendingVerificationAsset = asset;
                    }
                    if (!asset.IsLoaded && !asset.LastLoadFailed)
                        return;
                    if (asset.LastLoadFailed)
                        throw new InvalidOperationException($"Asset '{item.Path}' failed to reload from disk.");
                    if (verification.PropertyPath != null)
                    {
                        var value = GetMemberPathValue(asset, verification.PropertyPath);
                        var actual = value == null ? JValue.CreateNull() : JToken.Parse(FlaxJsonSerializer.Serialize(value));
                        if (!JToken.DeepEquals(actual, verification.ExpectedValue))
                            throw new InvalidOperationException($"Asset property '{verification.PropertyPath}' did not persist. Expected {verification.ExpectedValue}, reloaded {actual}.");
                    }
                    var data = new { path = item.Path, id = item.ID, type = item.TypeName, persisted = true, reloaded = true };
                    ClearPendingVerification();
                    if (isAction)
                    {
                        RecordSuccess("verify", data);
                    }
                    else
                    {
                        _verifyIndex++;
                        _context.ReportProgress($"Verified {_verifyIndex}/{_verifications.Count} assets", _operations.Length == 0 ? 1.0f : 0.9f + 0.1f * _verifyIndex / _verifications.Count);
                    }
                }
                catch (Exception ex)
                {
                    ClearPendingVerification();
                    if (isAction)
                        RecordFailure("verify", ex);
                    else
                        RecordVerificationFailure(verification, ex);
                }
            }

            private void ClearPendingVerification()
            {
                _pendingVerification = null;
                _pendingVerificationItem = null;
                _pendingVerificationAsset = null;
                _pendingVerificationIsAction = false;
                _pendingVerificationBuildRequested = false;
            }

            private void RecordVerificationFailure(AssetVerification verification, Exception exception)
            {
                _results.Add(new { index = _operations.Length + _verifyIndex, action = "verify", success = false, error = new { code = "FLX-ASSET-VERIFY-0006", message = exception.Message }, path = verification.Path, property = verification.PropertyPath });
                _failed++;
                _verifyIndex++;
                if (!_continueOnError)
                    Complete();
            }

            private void TrackVerification(string path)
            {
                path = System.IO.Path.GetFullPath(path);
                lock (_importLocker)
                {
                    if (!_verifications.Any(x => x.PropertyPath == null && PathEquals(x.Path, path)))
                        _verifications.Add(new AssetVerification { Path = path });
                }
            }

            private void TrackVerification(string path, string propertyPath, object expectedValue)
            {
                path = System.IO.Path.GetFullPath(path);
                var expected = expectedValue == null ? JValue.CreateNull() : JToken.Parse(FlaxJsonSerializer.Serialize(expectedValue));
                lock (_importLocker)
                {
                    var existing = _verifications.FirstOrDefault(x =>
                        string.Equals(x.PropertyPath, propertyPath, StringComparison.OrdinalIgnoreCase) && PathEquals(x.Path, path));
                    if (existing != null)
                        existing.ExpectedValue = expected;
                    else
                        _verifications.Add(new AssetVerification { Path = path, PropertyPath = propertyPath, ExpectedValue = expected });
                }
            }

            private static FlaxEngine.Asset LoadAsset(string path)
            {
                var item = RequireItem(path) as AssetItem
                           ?? throw new InvalidOperationException($"Content item '{path}' is not an asset.");
                var asset = item.LoadAsync();
                if (asset == null || asset.WaitForLoaded())
                    throw new InvalidOperationException($"Failed to load asset '{item.Path}'.");
                return asset;
            }

            private static ContentItem FindExisting(string path)
            {
                if (string.IsNullOrWhiteSpace(path))
                    return null;
                path = RequireProjectContentPath(path);
                var database = Editor.Instance.ContentDatabase;
                var item = database.Find(path);
                if (item == null && (File.Exists(path) || Directory.Exists(path)))
                {
                    RefreshPath(path, true);
                    item = database.Find(path);
                }
                return item;
            }

            private static ContentItem RequireItem(string path)
            {
                path = RequireProjectContentPath(path);
                var item = FindExisting(path);
                return item ?? throw new FileNotFoundException($"Content item '{path}' was not found.", path);
            }

            private static ContentFolder RequireFolder(string path)
            {
                return RequireItem(path) as ContentFolder
                       ?? throw new InvalidOperationException($"Content item '{path}' is not a folder.");
            }

            private static ContentFolder RequireExistingParent(string path)
            {
                return RequireFolder(System.IO.Path.GetDirectoryName(path));
            }

            private static string RequireNewPath(string path)
            {
                path = RequireProjectContentPath(path);
                if (PathEquals(path, Globals.ProjectContentFolder))
                    throw new InvalidOperationException("The project Content root cannot be replaced.");
                if (File.Exists(path) || Directory.Exists(path) || Editor.Instance.ContentDatabase.Find(path) != null)
                    throw new IOException($"The destination '{path}' already exists.");
                // ContentDatabase stores Flax-normalized paths (drive prefix
                // followed by forward separators). Passing a Windows-normalized
                // path into Move/Rename can make the watcher register the same
                // physical file twice and rewrite the asset ID.
                return StringUtils.NormalizePath(path);
            }

            private static string RequireMoveDestination(ContentItem item, string path)
            {
                var destination = StringUtils.NormalizePath(RequireProjectContentPath(path));
                var source = StringUtils.NormalizePath(item.Path);
                if (string.Equals(source, destination, StringComparison.Ordinal))
                    throw new InvalidOperationException($"Content item '{source}' is already at the requested destination.");
                return ContentMutationPathUtils.IsCaseOnlyRename(source, destination)
                    ? destination
                    : RequireNewPath(destination);
            }

            private static string EnsureAssetExtension(string path, string extension = "flax")
            {
                if (string.IsNullOrWhiteSpace(path))
                    throw new InvalidOperationException("An asset path is required.");
                if (string.IsNullOrEmpty(System.IO.Path.GetExtension(path)))
                    path += "." + extension.TrimStart('.');
                return RequireProjectContentPath(path);
            }

            private static string RequireProjectContentPath(string path)
            {
                if (string.IsNullOrWhiteSpace(path))
                    throw new InvalidOperationException("An asset path is required.");
                if (!System.IO.Path.IsPathRooted(path))
                {
                    var normalized = path.Replace(System.IO.Path.AltDirectorySeparatorChar, System.IO.Path.DirectorySeparatorChar);
                    path = normalized.Equals("Content", StringComparison.OrdinalIgnoreCase) || normalized.StartsWith("Content" + System.IO.Path.DirectorySeparatorChar, StringComparison.OrdinalIgnoreCase)
                        ? System.IO.Path.GetFullPath(normalized, Globals.ProjectFolder)
                        : System.IO.Path.GetFullPath(normalized, Globals.ProjectContentFolder);
                }
                else
                {
                    path = System.IO.Path.GetFullPath(path);
                }
                var root = System.IO.Path.GetFullPath(Globals.ProjectContentFolder).TrimEnd(System.IO.Path.DirectorySeparatorChar, System.IO.Path.AltDirectorySeparatorChar);
                var comparison = System.IO.Path.DirectorySeparatorChar == '\\' ? StringComparison.OrdinalIgnoreCase : StringComparison.Ordinal;
                if (!string.Equals(path.TrimEnd(System.IO.Path.DirectorySeparatorChar, System.IO.Path.AltDirectorySeparatorChar), root, comparison) &&
                    !path.StartsWith(root + System.IO.Path.DirectorySeparatorChar, comparison))
                    throw new InvalidOperationException($"Asset path '{path}' is outside the project Content folder '{root}'.");
                return path;
            }

            private static void RefreshPath(string path, bool recursive)
            {
                path = RequireProjectContentPath(path);
                var database = Editor.Instance.ContentDatabase;
                var item = database.Find(path);
                if (item != null)
                {
                    database.RefreshFolder(item, recursive);
                    return;
                }
                var current = Directory.Exists(path) ? path : System.IO.Path.GetDirectoryName(path);
                while (!string.IsNullOrEmpty(current))
                {
                    item = database.Find(current);
                    if (item != null)
                    {
                        database.RefreshFolder(item, true);
                        return;
                    }
                    if (PathEquals(current, Globals.ProjectContentFolder))
                        break;
                    current = System.IO.Path.GetDirectoryName(current);
                }
                var root = database.Find(Globals.ProjectContentFolder)
                           ?? throw new InvalidOperationException("The project Content folder is not available in the content database.");
                database.RefreshFolder(root, true);
            }

            private static object HandleExisting(ContentItem item, string policy)
            {
                policy = NormalizeIfExists(policy);
                if (policy == "error")
                    throw new IOException($"The destination '{item.Path}' already exists.");
                return new { asset = DescribeAsset(item), skipped = policy == "skip", updated = policy == "update" };
            }

            private static string NormalizeIfExists(string value)
            {
                value = string.IsNullOrWhiteSpace(value) ? "error" : value.Trim().ToLowerInvariant();
                if (value is not ("error" or "skip" or "update"))
                    throw new InvalidOperationException("ifExists must be error, skip, or update.");
                return value;
            }

            private static bool PathEquals(string a, string b)
            {
                var comparison = System.IO.Path.DirectorySeparatorChar == '\\' ? StringComparison.OrdinalIgnoreCase : StringComparison.Ordinal;
                return string.Equals(System.IO.Path.GetFullPath(a).TrimEnd(System.IO.Path.DirectorySeparatorChar, System.IO.Path.AltDirectorySeparatorChar),
                                     System.IO.Path.GetFullPath(b).TrimEnd(System.IO.Path.DirectorySeparatorChar, System.IO.Path.AltDirectorySeparatorChar), comparison);
            }

            private static IEnumerable<ContentItem> EnumerateChildren(ContentFolder folder)
            {
                foreach (var child in folder.Children)
                {
                    yield return child;
                    if (child is ContentFolder childFolder)
                    {
                        foreach (var descendant in EnumerateChildren(childFolder))
                            yield return descendant;
                    }
                }
            }

            private static object DescribeAsset(ContentItem item)
            {
                var info = new FileInfo(item.Path);
                var asset = item as AssetItem;
                string importPath = null;
                if (item is BinaryAssetItem binaryAsset)
                    binaryAsset.GetImportPath(out importPath);
                return new
                {
                    path = item.Path,
                    name = item.ShortName,
                    kind = item.IsFolder ? "folder" : item.IsAsset ? "asset" : "file",
                    type = asset?.TypeName ?? item.TypeDescription,
                    id = asset?.ID,
                    size = item.IsFolder || !info.Exists ? (long?)null : info.Length,
                    importPath,
                };
            }

            private static object DescribePath(string path, string type)
            {
                return new { path, name = System.IO.Path.GetFileNameWithoutExtension(path), kind = Directory.Exists(path) ? "folder" : "asset", type };
            }

            private static object GetMemberPathValue(object target, string path)
            {
                var names = SplitMemberPath(path);
                for (int i = 0; i < names.Length; i++)
                {
                    var member = FindMember(target.GetType(), names[i], false);
                    target = GetMemberValue(target, member);
                    if (target == null && i != names.Length - 1)
                        throw new InvalidOperationException($"Property path '{path}' contains a null value at '{names[i]}'.");
                }
                return target;
            }

            private static MemberInfo SetMemberPathValue(object target, string path, JToken token)
            {
                var names = SplitMemberPath(path);
                var parents = new List<(object Owner, MemberInfo Member, bool ValueType)>();
                for (int i = 0; i < names.Length - 1; i++)
                {
                    var member = FindMember(target.GetType(), names[i], false);
                    var child = GetMemberValue(target, member);
                    if (child == null)
                        throw new InvalidOperationException($"Property path '{path}' contains a null value at '{names[i]}'.");
                    parents.Add((target, member, GetMemberType(member).IsValueType));
                    target = child;
                }
                var leaf = FindMember(target.GetType(), names[names.Length - 1], true);
                var value = JsonConvert.DeserializeObject(token.ToString(Formatting.None), GetMemberType(leaf), FlaxJsonSerializer.Settings);
                SetMemberValue(target, leaf, value);
                for (int i = parents.Count - 1; i >= 0; i--)
                {
                    if (parents[i].ValueType)
                        SetMemberValue(parents[i].Owner, parents[i].Member, target);
                    target = parents[i].Owner;
                }
                return leaf;
            }

            private static string[] SplitMemberPath(string path)
            {
                if (string.IsNullOrWhiteSpace(path))
                    throw new InvalidOperationException("A public asset property path is required.");
                var names = path.Split('.');
                if (names.Any(string.IsNullOrWhiteSpace))
                    throw new InvalidOperationException($"Invalid asset property path '{path}'.");
                return names;
            }

            private static MemberInfo FindMember(Type type, string name, bool requireWritable)
            {
                const BindingFlags flags = BindingFlags.Instance | BindingFlags.Public;
                var property = type.GetProperty(name, flags) ?? type.GetProperties(flags).FirstOrDefault(x => string.Equals(x.Name, name, StringComparison.OrdinalIgnoreCase));
                if (property != null && property.GetIndexParameters().Length == 0 && property.CanRead && (!requireWritable || property.CanWrite))
                    return property;
                var field = type.GetField(name, flags) ?? type.GetFields(flags).FirstOrDefault(x => string.Equals(x.Name, name, StringComparison.OrdinalIgnoreCase));
                if (field != null && (!requireWritable || !field.IsInitOnly))
                    return field;
                throw new InvalidOperationException($"Public {(requireWritable ? "writable " : string.Empty)}property or field '{name}' was not found on type '{type.FullName}'.");
            }

            private static Type GetMemberType(MemberInfo member)
            {
                return member is PropertyInfo property ? property.PropertyType : ((FieldInfo)member).FieldType;
            }

            private static object GetMemberValue(object target, MemberInfo member)
            {
                return member is PropertyInfo property ? property.GetValue(target) : ((FieldInfo)member).GetValue(target);
            }

            private static void SetMemberValue(object target, MemberInfo member, object value)
            {
                if (member is PropertyInfo property)
                    property.SetValue(target, value);
                else
                    ((FieldInfo)member).SetValue(target, value);
            }
        }
    }
}
