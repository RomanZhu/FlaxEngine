// Copyright (c) Wojciech Figat. All rights reserved.

using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Threading;
using System.Threading.Tasks;
using FlaxEngine;
using FlaxEngine.Utilities;

namespace FlaxEditor
{
    /// <summary>
    /// Generic editor asset operations backed by the canonical source database.
    /// </summary>
    public static class AssetDatabase
    {
        private static readonly HashSet<string> DeferredImports = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        private static readonly Dictionary<Guid, AssetObjectId> AuthoredObjectIds = new Dictionary<Guid, AssetObjectId>();
        private static readonly Dictionary<Guid, string> AuthoredObjectPaths = new Dictionary<Guid, string>();
        private static int _assetEditingDepth;
        private static int _autoRefreshSuppressionDepth;
        private static bool _deferredRefresh;
        private static ImportAssetOptions _deferredRefreshOptions;
        private static int _callbackDepth;
        private static int _operationDepth;
        private static ulong _globalArtifactProcessedVersion;

        static AssetDatabase()
        {
            AssetDatabaseFacade.ArtifactPublished += OnArtifactPublished;
        }

        private static void OnArtifactPublished(Guid assetId)
        {
            _globalArtifactProcessedVersion++;
            var path = AssetDatabaseFacade.GetCanonicalSourcePath(assetId);
            if (!string.IsNullOrEmpty(path))
                AssetPipelineCallbacks.PostprocessAll(new[] { ToLogicalPath(path) }, Array.Empty<string>(), Array.Empty<string>(), Array.Empty<string>(), false);
        }

        /// <summary>Returns the source GUID at a canonical logical or absolute path.</summary>
        public static string AssetPathToGUID(string path, AssetPathToGUIDOptions options = AssetPathToGUIDOptions.IncludeRecentlyDeletedAssets)
        {
            var id = AssetDatabaseFacade.AssetPathToGUID(path);
            return id == Guid.Empty ? string.Empty : id.ToString("N");
        }

        /// <summary>Alias for <see cref="AssetPathToGUID"/>.</summary>
        public static string GUIDFromAssetPath(string path, AssetPathToGUIDOptions options = AssetPathToGUIDOptions.IncludeRecentlyDeletedAssets)
        {
            return AssetPathToGUID(path, options);
        }

        /// <summary>Returns the canonical logical path for a live source GUID.</summary>
        public static string GUIDToAssetPath(string guid)
        {
            return Guid.TryParse(guid, out var id) ? AssetDatabaseFacade.GUIDToAssetPath(id) : string.Empty;
        }

        /// <summary>Returns the canonical asset path adjacent to a registered metadata path.</summary>
        public static string GetAssetPathFromTextMetaFilePath(string metaPath)
        {
            if (string.IsNullOrWhiteSpace(metaPath) || !metaPath.EndsWith(".meta", StringComparison.OrdinalIgnoreCase))
                return string.Empty;
            var sourcePath = metaPath.Substring(0, metaPath.Length - 5);
            var record = GetMainRecord(sourcePath);
            return record?.CanonicalPath ?? string.Empty;
        }

        /// <summary>Returns the adjacent metadata path for a registered source.</summary>
        public static string GetTextMetaFilePathFromAssetPath(string path)
        {
            var record = GetMainRecord(path);
            return record.HasValue ? record.Value.CanonicalPath + ".meta" : string.Empty;
        }

        /// <summary>Resolves a loaded main asset to its file GUID and local file ID.</summary>
        public static bool TryGetGUIDAndLocalFileIdentifier(FlaxEngine.Object obj, out string guid, out long localId)
        {
            if (obj != null && AuthoredObjectIds.TryGetValue(obj.ID, out var authoredId))
            {
                guid = authoredId.Guid.ToString("N");
                localId = authoredId.LocalId;
                return true;
            }
            if (obj is Asset asset && AssetDatabaseFacade.TryGetAssetObjectId(asset, out var id))
            {
                guid = id.Guid.ToString("N");
                localId = id.LocalId;
                return true;
            }
            guid = string.Empty;
            localId = 0;
            return false;
        }

        /// <summary>Resolves a loaded persistent object to its file GUID and local file ID.</summary>
        public static bool TryGetAssetObjectId(FlaxEngine.Object obj, out AssetObjectId id)
        {
            if (obj != null && AuthoredObjectIds.TryGetValue(obj.ID, out id))
                return true;
            if (obj is Asset asset)
                return AssetDatabaseFacade.TryGetAssetObjectId(asset, out id);
            id = default;
            return false;
        }

        /// <summary>Returns the owning canonical source path for a persistent object.</summary>
        public static string GetAssetPath(FlaxEngine.Object obj)
        {
            return TryGetAssetObjectId(obj, out var id) ? GetAssetPath(id) : string.Empty;
        }

        /// <summary>Returns the owning canonical source path for a persistent object identifier.</summary>
        public static string GetAssetPath(AssetObjectId id)
        {
            return id.Guid == Guid.Empty ? string.Empty : AssetDatabaseFacade.GUIDToAssetPath(id.Guid);
        }

        /// <summary>Returns true when the committed database contains a live source at the path.</summary>
        public static bool AssetPathExists(string path)
        {
            return GetMainRecord(path).HasValue;
        }

        /// <summary>Returns true when an object belongs to a persistent registered source.</summary>
        public static bool Contains(FlaxEngine.Object obj)
        {
            return TryGetAssetObjectId(obj, out var id) && id.IsValid;
        }

        /// <summary>Returns true when an object is the declared main object of its source.</summary>
        public static bool IsMainAsset(FlaxEngine.Object obj)
        {
            if (!TryGetAssetObjectId(obj, out var id))
                return false;
            return AssetDatabaseFacade.GetRecords().Any(x => x.SourceAssetID == id.Guid && x.LocalId == id.LocalId && x.IsMain);
        }

        /// <summary>Returns true when an object is a live subasset.</summary>
        public static bool IsSubAsset(FlaxEngine.Object obj)
        {
            if (!TryGetAssetObjectId(obj, out var id))
                return false;
            return AssetDatabaseFacade.GetRecords().Any(x => x.SourceAssetID == id.Guid && x.LocalId == id.LocalId && !x.IsMain);
        }

        /// <summary>Returns true for authored source-document assets.</summary>
        public static bool IsNativeAsset(FlaxEngine.Object obj)
        {
            if (!TryGetAssetObjectId(obj, out var id))
                return false;
            return AssetDatabaseFacade.GetRecords().Any(x => x.SourceAssetID == id.Guid &&
                (x.SourceKind == AssetSourceKind.TextDocument || x.SourceKind == AssetSourceKind.ExistingJson));
        }

        /// <summary>Returns true for transient or unregistered objects.</summary>
        public static bool IsForeignAsset(FlaxEngine.Object obj)
        {
            return obj != null && !Contains(obj);
        }

        /// <summary>Returns true when the committed database contains a live folder record.</summary>
        public static bool IsValidFolder(string path)
        {
            var record = GetMainRecord(path);
            return record.HasValue && record.Value.SourceKind == AssetSourceKind.Folder;
        }

        /// <summary>Gets direct child folders from the committed source database.</summary>
        public static string[] GetSubFolders(string path)
        {
            var parent = GetMainRecord(path);
            if (!parent.HasValue || parent.Value.SourceKind != AssetSourceKind.Folder)
                return Array.Empty<string>();
            var parentPath = (parent.Value.CanonicalPath ?? string.Empty).TrimEnd('/');
            return AssetDatabaseFacade.GetRecords()
                .Where(x => x.IsMain && x.SourceKind == AssetSourceKind.Folder &&
                    string.Equals((Path.GetDirectoryName(x.CanonicalPath ?? string.Empty) ?? string.Empty).Replace('\\', '/'), parentPath, StringComparison.OrdinalIgnoreCase))
                .Select(x => x.CanonicalPath)
                .OrderBy(x => x, StringComparer.Ordinal)
                .ToArray();
        }

        /// <summary>Gets the main-object type without loading it.</summary>
        public static Type GetMainAssetTypeAtPath(string path)
        {
            var record = GetMainRecord(path);
            return record.HasValue ? TypeUtils.GetType(record.Value.TypeName).Type : null;
        }

        /// <summary>Gets the main-object type for a live source GUID without loading it.</summary>
        public static Type GetMainAssetTypeFromGUID(string guid)
        {
            var path = GUIDToAssetPath(guid);
            return string.IsNullOrEmpty(path) ? null : GetMainAssetTypeAtPath(path);
        }

        /// <summary>Gets an object type from the source path and stable local file ID.</summary>
        public static Type GetTypeFromPathAndFileID(string path, long localId)
        {
            var record = GetMainRecord(path);
            if (!record.HasValue)
                return null;
            var match = AssetDatabaseFacade.GetRecords().FirstOrDefault(x => x.SourceAssetID == record.Value.SourceAssetID && x.LocalId == localId);
            return match.SourceAssetID == Guid.Empty ? null : TypeUtils.GetType(match.TypeName).Type;
        }

        /// <summary>Gets all live main source paths.</summary>
        public static string[] GetAllAssetPaths()
        {
            return AssetDatabaseFacade.GetAllAssetPaths();
        }

        /// <summary>Searches committed records using bare, type, and label terms.</summary>
        public static string[] FindAssets(string filter, string[] searchInFolders = null)
        {
            var terms = (filter ?? string.Empty).Split(new[] { ' ' }, StringSplitOptions.RemoveEmptyEntries);
            var bare = terms.Where(x => !x.StartsWith("t:", StringComparison.OrdinalIgnoreCase) && !x.StartsWith("l:", StringComparison.OrdinalIgnoreCase)).ToArray();
            var types = terms.Where(x => x.StartsWith("t:", StringComparison.OrdinalIgnoreCase)).Select(x => x.Substring(2)).Where(x => x.Length != 0).ToArray();
            var labels = terms.Where(x => x.StartsWith("l:", StringComparison.OrdinalIgnoreCase)).Select(x => x.Substring(2)).Where(x => x.Length != 0).ToArray();
            var folderPrefixes = searchInFolders?.Select(x => ToLogicalPath(ResolvePhysicalPath(x)).TrimEnd('/') + "/").ToArray();
            var allRecords = AssetDatabaseFacade.GetRecords();
            var mains = allRecords.Where(x => x.IsMain && x.Status != AssetRecordStatus.MissingSource);
            var matches = new List<AssetDatabaseRecordInfo>();
            foreach (var main in mains)
            {
                var path = main.CanonicalPath ?? string.Empty;
                if (folderPrefixes != null && folderPrefixes.Length != 0 && !folderPrefixes.Any(x => path.StartsWith(x, StringComparison.OrdinalIgnoreCase)))
                    continue;
                if (bare.Any(x => path.IndexOf(x, StringComparison.OrdinalIgnoreCase) < 0))
                    continue;
                if (types.Length != 0)
                {
                    var objectTypes = allRecords.Where(x => x.SourceAssetID == main.SourceAssetID).Select(x => x.TypeName ?? string.Empty).ToArray();
                    if (types.Any(type => !objectTypes.Any(x => string.Equals(x, type, StringComparison.OrdinalIgnoreCase) || x.EndsWith("." + type, StringComparison.OrdinalIgnoreCase))))
                        continue;
                }
                if (labels.Length != 0)
                {
                    var sourceLabels = ParseLabels(main.LabelsSerialized);
                    if (labels.Any(label => !sourceLabels.Contains(label, StringComparer.OrdinalIgnoreCase)))
                        continue;
                }
                matches.Add(main);
            }
            return matches.OrderBy(x => x.CanonicalPath, StringComparer.OrdinalIgnoreCase)
                .ThenBy(x => x.SourceAssetID)
                .Select(x => x.SourceAssetID.ToString("N"))
                .ToArray();
        }

        /// <summary>Alias returning matching source GUIDs.</summary>
        public static string[] FindAssetGUIDs(string filter, string[] searchInFolders = null)
        {
            return FindAssets(filter, searchInFolders);
        }

        /// <summary>Gets source dependencies for one path.</summary>
        public static string[] GetDependencies(string path, bool recursive = false)
        {
            var record = GetMainRecord(path);
            return record.HasValue ? AssetDatabaseFacade.GetDependencies(record.Value.SourceAssetID, recursive) : Array.Empty<string>();
        }

        /// <summary>Gets the union of source dependencies for multiple paths.</summary>
        public static string[] GetDependencies(string[] paths, bool recursive = false)
        {
            if (paths == null)
                throw new ArgumentNullException(nameof(paths));
            return paths.SelectMany(path => GetDependencies(path, recursive))
                .Distinct(StringComparer.OrdinalIgnoreCase)
                .OrderBy(path => path, StringComparer.Ordinal)
                .ToArray();
        }

        /// <summary>Gets the deterministic selected dependency-closure hash.</summary>
        public static Hash128 GetAssetDependencyHash(string path)
        {
            var record = GetMainRecord(path);
            if (!record.HasValue)
                return default;
            var value = AssetDatabaseFacade.GetDependencyHash(record.Value.SourceAssetID);
            if (value == Guid.Empty)
                return default;
            var bytes = value.ToByteArray();
            return new Hash128(BitConverter.ToUInt64(bytes, 8), BitConverter.ToUInt64(bytes, 0));
        }

        /// <summary>Gets source-side labels without loading the asset.</summary>
        public static string[] GetLabels(FlaxEngine.Object obj)
        {
            if (!TryGetAssetObjectId(obj, out var id))
                return Array.Empty<string>();
            var record = AssetDatabaseFacade.GetRecords().FirstOrDefault(x => x.IsMain && x.SourceAssetID == id.Guid);
            return record.SourceAssetID == Guid.Empty ? Array.Empty<string>() : ParseLabels(record.LabelsSerialized);
        }

        /// <summary>Persists normalized source labels.</summary>
        public static void SetLabels(FlaxEngine.Object obj, string[] labels)
        {
            EnsureCoordinatorWrite();
            if (!Contains(obj))
                throw new ArgumentException("Labels require a persistent asset object.", nameof(obj));
            if (labels == null)
                throw new ArgumentNullException(nameof(labels));
            TryGetAssetObjectId(obj, out var id);
            var record = AssetDatabaseFacade.GetRecords().First(x => x.IsMain && x.SourceAssetID == id.Guid);
            labels = labels.Where(x => !string.IsNullOrWhiteSpace(x)).Distinct(StringComparer.Ordinal).OrderBy(x => x, StringComparer.Ordinal).ToArray();
            if (AssetDatabaseFacade.SetLabels(id.Guid, labels, record.Revision))
                throw new InvalidOperationException(GetLastDiagnostic("Asset label update failed or conflicted with a newer metadata revision."));
        }

        /// <summary>Clears source-side labels.</summary>
        public static void ClearLabels(FlaxEngine.Object obj)
        {
            SetLabels(obj, Array.Empty<string>());
        }

        /// <summary>Gets the selected importer proxy type for a path.</summary>
        public static Type GetImporterType(string path)
        {
            var importer = AssetImporter.GetAtPath(path);
            return importer?.GetType();
        }

        /// <summary>Gets registered managed importer candidates for a path.</summary>
        public static Type[] GetAvailableImporters(string path)
        {
            return ScriptedImporterRegistry.GetAvailable(path);
        }

        /// <summary>Gets the deterministic default managed importer for a path.</summary>
        public static Type GetDefaultImporter(string path)
        {
            return ScriptedImporterRegistry.GetDefault(path);
        }

        /// <summary>Gets the explicit managed importer override, if any.</summary>
        public static Type GetImporterOverride(string path)
        {
            var record = GetMainRecord(path);
            if (!record.HasValue)
                return null;
            return ScriptedImporterRegistry.CreateSelected(record.Value.ProcessorID)?.GetType();
        }

        /// <summary>Sets and persists an importer override.</summary>
        public static void SetImporterOverride<T>(string path) where T : AssetImporter
        {
            EnsureCoordinatorWrite();
            var candidates = ScriptedImporterRegistry.GetAvailable(path, true);
            if (!candidates.Contains(typeof(T)))
                throw new InvalidOperationException($"Importer '{typeof(T).FullName}' is not a registered override for '{path}'.");
            var record = GetMainRecord(path).Value;
            var metadata = AssetDatabaseFacade.GetImporterMetadata(record.SourceAssetID);
            if (AssetDatabaseFacade.ApplyImporterMetadata(record.SourceAssetID, record.Revision,
                    ScriptedImporterRegistry.GetId(typeof(T)), 1, "{}", "[]", metadata.UserData,
                    metadata.AssetBundleName, metadata.AssetBundleVariant))
                throw new InvalidOperationException(GetLastDiagnostic("Importer override update failed."));
            QueueImport(path);
        }

        /// <summary>Clears and persists an importer override.</summary>
        public static void ClearImporterOverride(string path)
        {
            EnsureCoordinatorWrite();
            if (!AssetPathExists(path))
                throw new ArgumentException("The source path is not registered.", nameof(path));
            var record = GetMainRecord(path).Value;
            var defaultImporter = ScriptedImporterRegistry.GetDefault(path);
            bool failed;
            if (defaultImporter != null)
            {
                var metadata = AssetDatabaseFacade.GetImporterMetadata(record.SourceAssetID);
                failed = AssetDatabaseFacade.ApplyImporterMetadata(record.SourceAssetID, record.Revision,
                    ScriptedImporterRegistry.GetId(defaultImporter), 1, "{}", "[]", metadata.UserData,
                    metadata.AssetBundleName, metadata.AssetBundleVariant);
            }
            else
            {
                failed = AssetDatabaseFacade.ResetImporterMetadataToDefault(record.SourceAssetID, record.Revision);
            }
            if (failed)
                throw new InvalidOperationException(GetLastDiagnostic("Importer override reset failed."));
            QueueImport(path);
        }

        /// <summary>Loads the main asset at a canonical source path.</summary>
        public static T LoadAssetAtPath<T>(string path) where T : FlaxEngine.Object
        {
            return (T)LoadAssetAtPath(path, typeof(T));
        }

        /// <summary>Loads the first compatible object, preferring the declared main object.</summary>
        public static FlaxEngine.Object LoadAssetAtPath(string path, Type type)
        {
            if (type == null)
                throw new ArgumentNullException(nameof(type));
            if (!typeof(FlaxEngine.Object).IsAssignableFrom(type))
                throw new ArgumentException("Expected type must derive from FlaxEngine.Object.", nameof(type));
            var record = GetMainRecord(path);
            if (!record.HasValue)
                return null;
            var records = AssetDatabaseFacade.GetRecords()
                .Where(x => x.SourceAssetID == record.Value.SourceAssetID && x.Status != AssetRecordStatus.MissingSource)
                .OrderByDescending(x => x.IsMain)
                .ThenBy(x => x.LocalId);
            foreach (var candidate in records)
            {
                var candidateType = TypeUtils.GetType(candidate.TypeName).Type;
                if (candidateType == null || !type.IsAssignableFrom(candidateType))
                    continue;
                return FlaxEngine.Content.LoadAsync(candidate.ID, type);
            }
            return null;
        }

        /// <summary>Loads the main asset at a canonical source path.</summary>
        public static FlaxEngine.Object LoadMainAssetAtPath(string path)
        {
            var record = GetMainRecord(path);
            return record.HasValue ? FlaxEngine.Content.LoadAsync(record.Value.ID, TypeUtils.GetType(record.Value.TypeName).Type ?? typeof(Asset)) : null;
        }

        /// <summary>Loads the main object for a live source GUID.</summary>
        public static FlaxEngine.Object LoadAssetByGUID(string guid)
        {
            var path = GUIDToAssetPath(guid);
            return string.IsNullOrEmpty(path) ? null : LoadMainAssetAtPath(path);
        }

        /// <summary>Loads the object currently mapped to a persistent file GUID and local file ID.</summary>
        public static Asset LoadAssetObject(AssetObjectId objectID)
        {
            return LoadAsset(objectID) as Asset;
        }

        /// <summary>Loads one exact persistent object.</summary>
        public static T LoadAsset<T>(AssetObjectId id) where T : FlaxEngine.Object
        {
            return (T)LoadAsset(id, typeof(T));
        }

        /// <summary>Loads one exact persistent object and validates its declared type.</summary>
        public static FlaxEngine.Object LoadAsset(AssetObjectId id, Type expectedType = null)
        {
            if (!id.IsValid)
                return null;
            expectedType = expectedType ?? typeof(Asset);
            if (!typeof(FlaxEngine.Object).IsAssignableFrom(expectedType))
                throw new ArgumentException("Expected type must derive from FlaxEngine.Object.", nameof(expectedType));
            var record = AssetDatabaseFacade.GetRecords().FirstOrDefault(x => x.SourceAssetID == id.Guid && x.LocalId == id.LocalId);
            if (record.SourceAssetID == Guid.Empty)
                return null;
            var declaredType = TypeUtils.GetType(record.TypeName).Type;
            if (declaredType != null && !expectedType.IsAssignableFrom(declaredType))
                return null;
            var backingId = AssetDatabaseFacade.GetBackingAssetID(id);
            return backingId == Guid.Empty ? null : FlaxEngine.Content.LoadAsync(backingId, expectedType);
        }

        /// <summary>Asynchronously loads one exact persistent object.</summary>
        public static AssetLoadRequest LoadObjectAsync(AssetObjectId id, Type expectedType = null, CancellationToken cancellationToken = default)
        {
            return new AssetLoadRequest(id, expectedType, cancellationToken);
        }

        /// <summary>Loads the main object and all live subassets at a canonical source path.</summary>
        public static FlaxEngine.Object[] LoadAllAssetsAtPath(string path)
        {
            var main = GetMainRecord(path);
            if (!main.HasValue)
                return Array.Empty<FlaxEngine.Object>();
            var records = AssetDatabaseFacade.GetRecords()
                .Where(x => x.SourceAssetID == main.Value.SourceAssetID && x.Status != AssetRecordStatus.MissingSource)
                .OrderByDescending(x => x.IsMain)
                .ThenBy(x => x.LocalId);
            var result = new List<FlaxEngine.Object>();
            foreach (var record in records)
            {
                var asset = FlaxEngine.Content.LoadAsync(record.ID, TypeUtils.GetType(record.TypeName).Type ?? typeof(Asset));
                if (asset != null)
                    result.Add(asset);
            }
            return result.ToArray();
        }

        /// <summary>Loads live subassets only, excluding the declared main object.</summary>
        public static FlaxEngine.Object[] LoadAllAssetRepresentationsAtPath(string path)
        {
            var main = GetMainRecord(path);
            if (!main.HasValue)
                return Array.Empty<FlaxEngine.Object>();
            return AssetDatabaseFacade.GetRecords()
                .Where(x => x.SourceAssetID == main.Value.SourceAssetID && !x.IsMain && x.Status != AssetRecordStatus.MissingSource)
                .OrderBy(x => x.LocalId)
                .Select(x => (FlaxEngine.Object)FlaxEngine.Content.LoadAsync(x.ID, TypeUtils.GetType(x.TypeName).Type ?? typeof(Asset)))
                .Where(x => x != null)
                .ToArray();
        }

        /// <summary>Returns true when the current main backing object is registered in the content pool.</summary>
        public static bool IsMainAssetAtPathLoaded(string path)
        {
            var main = GetMainRecord(path);
            return main.HasValue && FlaxEngine.Content.Assets.Any(x => x.ID == main.Value.ID);
        }

        /// <summary>Creates a supported authored source document and imports it.</summary>
        public static void CreateAsset(FlaxEngine.Object asset, string path)
        {
            EnsureCoordinatorWrite();
            if (asset == null)
                throw new ArgumentNullException(nameof(asset));
            var physicalPath = ResolvePhysicalPath(path);
            if (!IsProjectContentPath(physicalPath) || !AssetMountRegistry.IsWritable(ToLogicalPath(physicalPath)))
                throw new ArgumentException("Authored assets must be created inside the project Content folder.", nameof(path));
            if (File.Exists(physicalPath) || Directory.Exists(physicalPath) || AssetPathExists(path))
                throw new IOException("An asset already exists at the destination path.");
            AssetPipelineCallbacks.WillCreate(ToLogicalPath(physicalPath));
            using (AssetPipelineCallbacks.BypassNativeDecision())
            {
            if (!(asset is BinaryAsset binaryAsset))
            {
                if (!string.Equals(Path.GetExtension(physicalPath), ".asset", StringComparison.OrdinalIgnoreCase))
                    throw new ArgumentException("Generic authored objects use the .asset source extension.", nameof(path));
                var data = FlaxEngine.Json.JsonSerializer.Serialize(asset, asset.GetType());
                var authoredId = AuthoredAssetFacade.CreateAsset(physicalPath, asset.ID, asset.GetType().FullName, asset.GetType().Name, data);
                if (authoredId == Guid.Empty)
                    throw new InvalidOperationException(AuthoredAssetFacade.GetLastError());
                AuthoredObjectIds[asset.ID] = new AssetObjectId(authoredId, 1);
                AuthoredObjectPaths[asset.ID] = physicalPath;
                QueueImport(physicalPath);
                return;
            }
            Guid id;
            if (asset is Material)
                id = AssetDatabaseFacade.CreateGraphDocument(physicalPath, asset.GetType().FullName);
            else
                id = AssetDatabaseFacade.CreateAuthoredDocument(physicalPath, asset.GetType().FullName);
            if (id == Guid.Empty)
                throw new InvalidOperationException(GetLastDiagnostic("Authored asset creation failed."));
            var saveFailed = asset is Material material
                ? AssetDatabaseFacade.SaveMaterialDocument(material, id)
                : AssetDatabaseFacade.SaveAuthoredDocument(binaryAsset, id);
            if (saveFailed)
                throw new InvalidOperationException(GetLastDiagnostic("The initial authored asset state could not be serialized."));
            }
            QueueImport(physicalPath);
        }

        /// <summary>Adds an object to an authored multi-object source document.</summary>
        public static void AddObjectToAsset(FlaxEngine.Object objectToAdd, string path)
        {
            EnsureCoordinatorWrite();
            if (objectToAdd == null)
                throw new ArgumentNullException(nameof(objectToAdd));
            var physicalPath = ResolvePhysicalPath(path);
            if (!IsProjectContentPath(physicalPath))
                throw new ArgumentException("Authored objects can only be added to project Content sources.", nameof(path));
            var main = GetMainRecord(physicalPath);
            var stagedOwner = AuthoredObjectPaths.FirstOrDefault(x => string.Equals(x.Value, physicalPath, StringComparison.OrdinalIgnoreCase));
            var sourceId = main?.SourceAssetID ?? (stagedOwner.Key != Guid.Empty && AuthoredObjectIds.TryGetValue(stagedOwner.Key, out var stagedId) ? stagedId.Guid : Guid.Empty);
            if (sourceId == Guid.Empty || (main.HasValue && !string.Equals(main.Value.ProcessorID, "Flax.AuthoredObject", StringComparison.Ordinal)))
                throw new InvalidOperationException("AddObjectToAsset only supports generic authored multi-object sources.");
            var data = FlaxEngine.Json.JsonSerializer.Serialize(objectToAdd, objectToAdd.GetType());
            var localId = AuthoredAssetFacade.AddObjectToAsset(physicalPath, objectToAdd.ID, objectToAdd.GetType().FullName, objectToAdd.GetType().Name, data);
            if (localId == 0)
                throw new InvalidOperationException(AuthoredAssetFacade.GetLastError());
            AuthoredObjectIds[objectToAdd.ID] = new AssetObjectId(sourceId, localId);
            AuthoredObjectPaths[objectToAdd.ID] = physicalPath;
            QueueImport(physicalPath);
        }

        /// <summary>Removes an object from an authored multi-object source document.</summary>
        public static void RemoveObjectFromAsset(FlaxEngine.Object objectToRemove)
        {
            EnsureCoordinatorWrite();
            if (objectToRemove == null)
                throw new ArgumentNullException(nameof(objectToRemove));
            if (!TryGetAssetObjectId(objectToRemove, out var id))
                throw new InvalidOperationException("The object is not stored in an authored source.");
            var physicalPath = AuthoredObjectPaths.TryGetValue(objectToRemove.ID, out var stagedPath)
                ? stagedPath
                : ResolvePhysicalPath(GUIDToAssetPath(id.Guid.ToString("N")));
            if (string.IsNullOrEmpty(physicalPath) || AuthoredAssetFacade.RemoveObjectFromAsset(physicalPath, id.LocalId))
                throw new InvalidOperationException(AuthoredAssetFacade.GetLastError());
            AuthoredObjectIds.Remove(objectToRemove.ID);
            AuthoredObjectPaths.Remove(objectToRemove.ID);
            QueueImport(physicalPath);
        }

        /// <summary>Persists an authored source's main-object selection.</summary>
        public static void SetMainObject(FlaxEngine.Object mainObject, string assetPath)
        {
            EnsureCoordinatorWrite();
            if (mainObject == null)
                throw new ArgumentNullException(nameof(mainObject));
            if (!TryGetAssetObjectId(mainObject, out var id))
                throw new InvalidOperationException("The object is not stored in an authored source.");
            var physicalPath = ResolvePhysicalPath(assetPath);
            var owner = GetMainRecord(physicalPath);
            if (!owner.HasValue || owner.Value.SourceAssetID != id.Guid)
                throw new ArgumentException("The object does not belong to the selected authored source.", nameof(assetPath));
            if (AuthoredAssetFacade.SetMainObject(physicalPath, id.LocalId))
                throw new InvalidOperationException(AuthoredAssetFacade.GetLastError());
            QueueImport(physicalPath);
        }

        /// <summary>Reconciles and imports one source path.</summary>
        public static void ImportAsset(string path, ImportAssetOptions options = ImportAssetOptions.Default)
        {
            EnsureCoordinatorWrite();
            if (_assetEditingDepth != 0 || _callbackDepth != 0)
            {
                DeferredImports.Add(path);
                return;
            }
            var resolvedPath = ResolvePhysicalPath(path);
            if (Directory.Exists(resolvedPath))
            {
                foreach (var sourcePath in Directory.GetFiles(resolvedPath, "*", SearchOption.AllDirectories)
                             .Where(x => !x.EndsWith(".meta", StringComparison.OrdinalIgnoreCase))
                             .OrderBy(x => x, StringComparer.OrdinalIgnoreCase))
                    ImportAsset(sourcePath, options & ~ImportAssetOptions.ImportRecursive);
                return;
            }
            _operationDepth++;
            try
            {
                var physicalPath = resolvedPath;
                var callbackHash = File.Exists(physicalPath) ? PreprocessImport(physicalPath) : string.Empty;
                if (ScriptedImporterRegistry.TryImport(path, options, callbackHash))
                    return;
                if (AssetDatabaseFacade.ImportAsset(path, options))
                    throw new InvalidOperationException(GetLastDiagnostic("Asset import failed."));
            }
            finally
            {
                if (--_operationDepth == 0)
                    FlushDeferredImports();
            }
        }

        /// <summary>Reconciles all canonical sources.</summary>
        public static void Refresh(ImportAssetOptions options = ImportAssetOptions.Default)
        {
            EnsureCoordinatorWrite();
            if (_assetEditingDepth != 0 || _callbackDepth != 0)
            {
                _deferredRefresh = true;
                _deferredRefreshOptions |= options;
                return;
            }
            _operationDepth++;
            try
            {
                if (AssetDatabaseFacade.Refresh(options))
                    throw new InvalidOperationException(GetLastDiagnostic("Asset refresh failed."));
                var forceManaged = (options & ImportAssetOptions.ForceUpdate) != 0;
                var change = AssetDatabaseFacade.GetLastChange();
                var changed = new HashSet<Guid>(change.Added.Concat(change.Changed).Concat(change.StatusChanged));
                foreach (var record in AssetDatabaseFacade.GetRecords()
                             .Where(x => x.IsMain && (forceManaged || changed.Contains(x.ID)))
                             .OrderBy(x => x.CanonicalPath, StringComparer.OrdinalIgnoreCase))
                {
                    var selected = ScriptedImporterRegistry.CreateSelected(record.ProcessorID);
                    if (selected == null && (!string.Equals(record.ProcessorID, "Flax.Unsupported", StringComparison.Ordinal) ||
                                             ScriptedImporterRegistry.GetDefault(record.CanonicalPath) == null))
                        continue;
                    var physicalPath = ResolvePhysicalPath(record.CanonicalPath);
                    if (!File.Exists(physicalPath))
                        continue;
                    var callbackHash = PreprocessImport(physicalPath);
                    ScriptedImporterRegistry.TryImport(record.CanonicalPath, options, callbackHash);
                }
            }
            finally
            {
                if (--_operationDepth == 0)
                    FlushDeferredImports();
            }
        }

        /// <summary>Moves a source and its metadata while preserving identity.</summary>
        public static string MoveAsset(string oldPath, string newPath)
        {
            EnsureCoordinatorWrite();
            var validation = ValidateMoveAsset(oldPath, newPath, out var callbackHandled);
            if (!string.IsNullOrEmpty(validation) || callbackHandled)
                return validation;
            var source = ResolvePhysicalPath(oldPath);
            var destination = ResolvePhysicalPath(newPath);
            AssetMutationResultInfo result;
            using (AssetPipelineCallbacks.BypassNativeDecision())
                result = AssetDatabaseFacade.MoveAssetPair(source, destination);
            if (!result.Succeeded)
                return string.IsNullOrEmpty(result.Message) ? "Asset move transaction failed." : result.Message;
            QueueImport(destination);
            return string.Empty;
        }

        /// <summary>Moves source/metadata pairs under one durable native transaction.</summary>
        internal static string MoveAssets(string[] oldPaths, string[] newPaths)
        {
            if (oldPaths == null)
                throw new ArgumentNullException(nameof(oldPaths));
            if (newPaths == null)
                throw new ArgumentNullException(nameof(newPaths));
            if (oldPaths.Length != newPaths.Length)
                throw new ArgumentException("Source and destination arrays must have matching indexes.");
            EnsureCoordinatorWrite();
            if (oldPaths.Length == 0)
                return string.Empty;
            var sources = new List<string>(oldPaths.Length);
            var destinations = new List<string>(newPaths.Length);
            var logicalSources = new List<string>(oldPaths.Length);
            var logicalDestinations = new List<string>(newPaths.Length);
            var callbackHandled = false;
            for (var i = 0; i < oldPaths.Length; i++)
            {
                var validation = ValidateMoveAsset(oldPaths[i], newPaths[i], out var handled);
                if (!string.IsNullOrEmpty(validation))
                    return validation;
                if (handled)
                {
                    callbackHandled = true;
                    continue;
                }
                var source = ResolvePhysicalPath(oldPaths[i]);
                var destination = ResolvePhysicalPath(newPaths[i]);
                sources.Add(source);
                destinations.Add(destination);
                logicalSources.Add(ToLogicalPath(source));
                logicalDestinations.Add(ToLogicalPath(destination));
            }
            if (callbackHandled && sources.Count != 0)
            {
                Refresh(ImportAssetOptions.Default);
                return "A modification callback handled only part of the move selection. The remaining paths were excluded from the native atomic batch.";
            }
            if (sources.Count == 0)
                return string.Empty;
            using (AssetEditingScope())
            using (AssetPipelineCallbacks.BypassNativeDecision())
            {
                var result = AssetDatabaseFacade.MoveAssetPairs(sources.ToArray(), destinations.ToArray());
                if (!result.Succeeded)
                    return string.IsNullOrEmpty(result.Message) ? "Asset move batch transaction failed." : result.Message;
                for (var i = 0; i < destinations.Count; i++)
                    QueueImport(destinations[i]);
            }
            return string.Empty;
        }

        /// <summary>Runs complete managed move preflight without mutating source state.</summary>
        public static string ValidateMoveAsset(string oldPath, string newPath)
        {
            return ValidateMoveAsset(oldPath, newPath, out _);
        }

        private static string ValidateMoveAsset(string oldPath, string newPath, out bool callbackHandled)
        {
            callbackHandled = false;
            var source = ResolvePhysicalPath(oldPath);
            var destination = ResolvePhysicalPath(newPath);
            if (!IsProjectContentPath(source) || !IsProjectContentPath(destination) ||
                !AssetMountRegistry.IsWritable(ToLogicalPath(source)) || !AssetMountRegistry.IsWritable(ToLogicalPath(destination)))
                return "Asset moves must remain inside the project Content folder.";
            var nativeValidation = AssetDatabaseFacade.ValidateAssetMove(source, destination);
            if (!nativeValidation.Succeeded)
                return string.IsNullOrEmpty(nativeValidation.Message) ? "Asset move validation failed." : nativeValidation.Message;
            return AssetPipelineCallbacks.ValidateMove(ToLogicalPath(source), ToLogicalPath(destination), out callbackHandled);
        }

        /// <summary>Renames a source while preserving identity.</summary>
        public static string RenameAsset(string path, string newName)
        {
            EnsureCoordinatorWrite();
            var source = ResolvePhysicalPath(path);
            if (string.IsNullOrWhiteSpace(newName) || Path.GetFileName(newName) != newName)
                return "The new asset name is invalid.";
            var extension = File.Exists(source) ? Path.GetExtension(source) : string.Empty;
            if (extension.Length != 0 && string.IsNullOrEmpty(Path.GetExtension(newName)))
                newName += extension;
            return MoveAsset(source, Path.Combine(Path.GetDirectoryName(source) ?? string.Empty, newName));
        }

        /// <summary>Copies a source and its importer settings with a new file GUID.</summary>
        public static bool CopyAsset(string sourcePath, string destinationPath)
        {
            EnsureCoordinatorWrite();
            var source = ResolvePhysicalPath(sourcePath);
            var destination = ResolvePhysicalPath(destinationPath);
            if (!IsProjectContentPath(source) || !IsProjectContentPath(destination) ||
                !AssetMountRegistry.IsWritable(ToLogicalPath(source)) || !AssetMountRegistry.IsWritable(ToLogicalPath(destination)))
                return false;
            AssetPipelineCallbacks.WillCreate(ToLogicalPath(destination));
            AssetMutationResultInfo result;
            using (AssetPipelineCallbacks.BypassNativeDecision())
                result = AssetDatabaseFacade.CopyAssetPair(source, destination);
            if (!result.Succeeded)
                return false;
            QueueImport(destination);
            AssetPipelineCallbacks.PostprocessAll(new[] { ToLogicalPath(destination) }, Array.Empty<string>(), Array.Empty<string>(), Array.Empty<string>(), false);
            return true;
        }

        /// <summary>Copies multiple source trees as one deferred-import batch.</summary>
        public static bool CopyAssets(string[] sourcePaths, string[] destinationPaths)
        {
            if (sourcePaths == null)
                throw new ArgumentNullException(nameof(sourcePaths));
            if (destinationPaths == null)
                throw new ArgumentNullException(nameof(destinationPaths));
            if (sourcePaths.Length != destinationPaths.Length)
                throw new ArgumentException("Source and destination arrays must have matching indexes.");
            EnsureCoordinatorWrite();
            if (sourcePaths.Length == 0)
                return true;
            var sources = new string[sourcePaths.Length];
            var destinations = new string[destinationPaths.Length];
            var logicalDestinations = new string[destinationPaths.Length];
            for (var i = 0; i < sourcePaths.Length; i++)
            {
                sources[i] = ResolvePhysicalPath(sourcePaths[i]);
                destinations[i] = ResolvePhysicalPath(destinationPaths[i]);
                if (!IsProjectContentPath(sources[i]) || !IsProjectContentPath(destinations[i]) ||
                    !AssetMountRegistry.IsWritable(ToLogicalPath(sources[i])) || !AssetMountRegistry.IsWritable(ToLogicalPath(destinations[i])))
                    return false;
                logicalDestinations[i] = ToLogicalPath(destinations[i]);
                AssetPipelineCallbacks.WillCreate(logicalDestinations[i]);
            }
            using (AssetEditingScope())
            using (AssetPipelineCallbacks.BypassNativeDecision())
            {
                var result = AssetDatabaseFacade.CopyAssetPairs(sources, destinations);
                if (!result.Succeeded)
                    return false;
                for (var i = 0; i < destinations.Length; i++)
                    QueueImport(destinations[i]);
                AssetPipelineCallbacks.PostprocessAll(logicalDestinations, Array.Empty<string>(), Array.Empty<string>(), Array.Empty<string>(), false);
            }
            return true;
        }

        /// <summary>Deletes a source and its adjacent metadata.</summary>
        public static bool DeleteAsset(string path)
        {
            EnsureCoordinatorWrite();
            var physicalPath = ResolvePhysicalPath(path);
            if (!IsProjectContentPath(physicalPath) || !AssetMountRegistry.IsWritable(ToLogicalPath(physicalPath)))
                return false;
            if (!AssetPipelineCallbacks.ValidateDelete(ToLogicalPath(physicalPath), out var callbackHandled))
                return false;
            if (callbackHandled)
                return true;
            AssetMutationResultInfo result;
            using (AssetPipelineCallbacks.BypassNativeDecision())
                result = AssetDatabaseFacade.DeleteAssetPairToRecovery(physicalPath);
            return result.Succeeded;
        }

        /// <summary>Deletes multiple source trees and reports paths that failed.</summary>
        public static bool DeleteAssets(string[] paths, List<string> failedPaths)
        {
            if (paths == null)
                throw new ArgumentNullException(nameof(paths));
            if (failedPaths == null)
                throw new ArgumentNullException(nameof(failedPaths));
            return DeleteAssetsBatch(paths, failedPaths);
        }

        /// <summary>Requests a recoverable deletion.</summary>
        public static bool MoveAssetToTrash(string path)
        {
            EnsureCoordinatorWrite();
            var physicalPath = ResolvePhysicalPath(path);
            if (!IsProjectContentPath(physicalPath) || !AssetMountRegistry.IsWritable(ToLogicalPath(physicalPath)) ||
                !AssetPipelineCallbacks.ValidateDelete(ToLogicalPath(physicalPath), out var callbackHandled))
                return false;
            if (callbackHandled)
                return true;
            AssetMutationResultInfo result;
            using (AssetPipelineCallbacks.BypassNativeDecision())
                result = AssetDatabaseFacade.DeleteAssetPairToRecovery(physicalPath);
            return result.Succeeded;
        }

        /// <summary>Requests recoverable deletion for multiple source trees.</summary>
        public static bool MoveAssetsToTrash(string[] paths, List<string> failedPaths)
        {
            if (paths == null)
                throw new ArgumentNullException(nameof(paths));
            if (failedPaths == null)
                throw new ArgumentNullException(nameof(failedPaths));
            return DeleteAssetsBatch(paths, failedPaths);
        }

        private static bool DeleteAssetsBatch(string[] paths, List<string> failedPaths)
        {
            EnsureCoordinatorWrite();
            if (paths.Length == 0)
                return true;
            var nativePaths = new List<string>(paths.Length);
            var nativeInputs = new List<string>(paths.Length);
            var callbackHandledAny = false;
            for (var i = 0; i < paths.Length; i++)
            {
                var physicalPath = ResolvePhysicalPath(paths[i]);
                var logicalPath = ToLogicalPath(physicalPath);
                if (!IsProjectContentPath(physicalPath) || !AssetMountRegistry.IsWritable(logicalPath) ||
                    !AssetPipelineCallbacks.ValidateDelete(logicalPath, out var callbackHandled))
                {
                    failedPaths.Add(paths[i]);
                    return false;
                }
                if (callbackHandled)
                {
                    callbackHandledAny = true;
                    continue;
                }
                nativePaths.Add(physicalPath);
                nativeInputs.Add(paths[i]);
            }
            if (callbackHandledAny && nativePaths.Count != 0)
            {
                Editor.LogWarning("A modification callback handled only part of the delete selection. Callback-handled paths are explicitly outside the native transaction; the remaining paths were not mutated.");
                failedPaths.AddRange(nativeInputs);
                return false;
            }
            if (nativePaths.Count == 0)
                return true;
            using (AssetEditingScope())
            using (AssetPipelineCallbacks.BypassNativeDecision())
            {
                var result = AssetDatabaseFacade.DeleteAssetPairsToRecovery(nativePaths.ToArray());
                if (!result.Succeeded)
                {
                    failedPaths.AddRange(nativeInputs);
                    return false;
                }
            }
            return true;
        }

        /// <summary>Creates a folder and schedules metadata reconciliation.</summary>
        public static string CreateFolder(string parentFolder, string newFolderName)
        {
            EnsureCoordinatorWrite();
            var parent = ResolvePhysicalPath(parentFolder);
            if (!IsProjectContentPath(parent) || !AssetMountRegistry.IsWritable(ToLogicalPath(parent)) ||
                string.IsNullOrWhiteSpace(newFolderName) || Path.GetFileName(newFolderName) != newFolderName)
                return string.Empty;
            var path = Path.Combine(parent, newFolderName);
            AssetPipelineCallbacks.WillCreate(ToLogicalPath(path));
            AssetMutationResultInfo result;
            using (AssetPipelineCallbacks.BypassNativeDecision())
                result = AssetDatabaseFacade.CreateAssetFolder(path);
            if (!result.Succeeded)
                return string.Empty;
            QueueImport(path);
            AssetPipelineCallbacks.PostprocessAll(new[] { ToLogicalPath(path) }, Array.Empty<string>(), Array.Empty<string>(), Array.Empty<string>(), false);
            return AssetPathToGUID(path);
        }

        /// <summary>Generates a non-colliding source path.</summary>
        public static string GenerateUniqueAssetPath(string path)
        {
            var physicalPath = ResolvePhysicalPath(path);
            if (!File.Exists(physicalPath) && !Directory.Exists(physicalPath))
                return path;
            var directory = Path.GetDirectoryName(physicalPath) ?? string.Empty;
            var extension = Path.GetExtension(physicalPath);
            var name = Path.GetFileNameWithoutExtension(physicalPath);
            for (var index = 1; ; index++)
            {
                var candidate = Path.Combine(directory, $"{name} {index}{extension}");
                if (!File.Exists(candidate) && !Directory.Exists(candidate))
                    return ToLogicalPath(candidate);
            }
        }

        /// <summary>Writes dirty importer settings without importing.</summary>
        public static bool WriteImportSettingsIfDirty(string path)
        {
            EnsureCoordinatorWrite();
            var importer = AssetImporter.GetAtPath(path);
            return importer != null && importer.WriteImportSettingsIfDirty();
        }

        /// <summary>Saves editor-owned authored sources through their registered serializers.</summary>
        public static void SaveAssets()
        {
            EnsureCoordinatorWrite();
            var dirty = AuthoredAssetFacade.GetDirtyPaths()
                .Split(new[] { '\n' }, StringSplitOptions.RemoveEmptyEntries)
                .Select(ResolvePhysicalPath)
                .Where(path => AssetMountRegistry.IsWritable(ToLogicalPath(path)))
                .ToDictionary(ToLogicalPath, path => path, StringComparer.OrdinalIgnoreCase);
            var selected = AssetPipelineCallbacks.WillSave(dirty.Keys.OrderBy(path => path, StringComparer.Ordinal).ToArray());
            foreach (var logicalPath in selected.Distinct(StringComparer.OrdinalIgnoreCase))
            {
                if (!dirty.TryGetValue(ToLogicalPath(ResolvePhysicalPath(logicalPath)), out var physicalPath))
                    continue;
                bool failed;
                using (AssetPipelineCallbacks.BypassNativeDecision())
                    failed = AuthoredAssetFacade.SaveAssetIfDirty(physicalPath);
                if (failed)
                    throw new InvalidOperationException(AuthoredAssetFacade.GetLastError());
                QueueImport(physicalPath);
            }
        }

        /// <summary>Saves the containing authored source when the object is writable.</summary>
        public static bool SaveAssetIfDirty(FlaxEngine.Object obj)
        {
            EnsureCoordinatorWrite();
            if (obj is AssetImporter importer)
                return importer.WriteImportSettingsIfDirty();
            if (!TryGetAssetObjectId(obj, out var id))
                return false;
            var record = AssetDatabaseFacade.GetRecords().FirstOrDefault(x => x.IsMain && x.SourceAssetID == id.Guid);
            var physicalPath = AuthoredObjectPaths.TryGetValue(obj.ID, out var stagedPath)
                ? stagedPath
                : record.SourceAssetID != Guid.Empty ? ResolvePhysicalPath(record.CanonicalPath) : string.Empty;
            var genericAuthored = !string.IsNullOrEmpty(physicalPath) &&
                (AuthoredObjectPaths.ContainsKey(obj.ID) || string.Equals(record.ProcessorID, "Flax.AuthoredObject", StringComparison.Ordinal));
            if (genericAuthored)
            {
                var value = obj is JsonAsset jsonAsset && jsonAsset.Instance != null ? jsonAsset.Instance : (object)obj;
                var data = FlaxEngine.Json.JsonSerializer.Serialize(value, value.GetType());
                if (AuthoredAssetFacade.StageObjectData(physicalPath, id.LocalId, data, "SaveAssetIfDirty"))
                    throw new InvalidOperationException(AuthoredAssetFacade.GetLastError());
                var logicalPath = ToLogicalPath(physicalPath);
                var selectedGeneric = AssetPipelineCallbacks.WillSave(new[] { logicalPath });
                if (!selectedGeneric.Contains(logicalPath, StringComparer.OrdinalIgnoreCase))
                    return false;
                bool failed;
                using (AssetPipelineCallbacks.BypassNativeDecision())
                    failed = AuthoredAssetFacade.SaveAssetIfDirty(physicalPath);
                if (failed)
                    throw new InvalidOperationException(AuthoredAssetFacade.GetLastError());
                QueueImport(physicalPath);
                return true;
            }
            if (!(obj is Asset asset) || record.SourceAssetID == Guid.Empty || record.SourceKind != AssetSourceKind.TextDocument)
                return false;
            var selected = AssetPipelineCallbacks.WillSave(new[] { record.CanonicalPath });
            if (!selected.Contains(record.CanonicalPath, StringComparer.OrdinalIgnoreCase))
                return false;
            using (AssetPipelineCallbacks.BypassNativeDecision())
                return !Editor.Instance.ContentDatabase.SaveAsset(asset);
        }

        /// <summary>Saves the authored source identified by a file GUID.</summary>
        public static bool SaveAssetIfDirty(string guid)
        {
            EnsureCoordinatorWrite();
            if (!Guid.TryParse(guid, out var id))
                return false;
            var record = AssetDatabaseFacade.GetRecords().FirstOrDefault(x => x.IsMain && x.SourceAssetID == id);
            if (record.SourceAssetID == Guid.Empty)
                return false;
            if (string.Equals(record.ProcessorID, "Flax.AuthoredObject", StringComparison.Ordinal) &&
                AuthoredAssetFacade.IsDirty(ResolvePhysicalPath(record.CanonicalPath)))
            {
                var logicalPath = record.CanonicalPath;
                if (!AssetPipelineCallbacks.WillSave(new[] { logicalPath }).Contains(logicalPath, StringComparer.OrdinalIgnoreCase))
                    return false;
                bool failed;
                using (AssetPipelineCallbacks.BypassNativeDecision())
                    failed = AuthoredAssetFacade.SaveAssetIfDirty(ResolvePhysicalPath(logicalPath));
                if (failed)
                    throw new InvalidOperationException(AuthoredAssetFacade.GetLastError());
                QueueImport(logicalPath);
                return true;
            }
            var asset = FlaxEngine.Content.LoadAsync<Asset>(record.ID);
            return SaveAssetIfDirty(asset);
        }

        /// <summary>Rewrites selected authored sources using current canonical serializers.</summary>
        public static void ForceReserializeAssets(IEnumerable<string> paths = null,
            ForceReserializeAssetsOptions options = ForceReserializeAssetsOptions.ReserializeAssets)
        {
            EnsureCoordinatorWrite();
            var requested = paths?.Select(path => ToLogicalPath(ResolvePhysicalPath(path))).Distinct(StringComparer.OrdinalIgnoreCase).ToArray()
                            ?? Array.Empty<string>();
            if ((options & ForceReserializeAssetsOptions.ReserializeMetadata) != 0 &&
                AssetDatabaseFacade.ForceReserializeMetadata(requested))
                throw new InvalidOperationException(GetLastDiagnostic("Metadata reserialization failed."));
            if ((options & ForceReserializeAssetsOptions.ReserializeAssets) == 0)
                return;
            var selected = requested.Length == 0 ? null : new HashSet<string>(requested.Select(ResolvePhysicalPath), StringComparer.OrdinalIgnoreCase);
            var records = AssetDatabaseFacade.GetRecords().Where(x => x.IsMain &&
                (x.SourceKind == AssetSourceKind.TextDocument || x.SourceKind == AssetSourceKind.ExistingJson) &&
                (selected == null || selected.Contains(ResolvePhysicalPath(x.CanonicalPath)))).ToArray();
            foreach (var record in records)
            {
                if (string.Equals(record.ProcessorID, "Flax.AuthoredObject", StringComparison.Ordinal))
                {
                    if (AuthoredAssetFacade.ForceReserialize(ResolvePhysicalPath(record.CanonicalPath),
                            (options & ForceReserializeAssetsOptions.ReserializeMetadata) != 0))
                        throw new InvalidOperationException(AuthoredAssetFacade.GetLastError());
                    QueueImport(record.CanonicalPath);
                    continue;
                }
                var asset = FlaxEngine.Content.LoadAsync<Asset>(record.ID);
                if (asset != null && Editor.Instance.ContentDatabase.SaveAsset(asset))
                    throw new InvalidOperationException($"Cannot reserialize authored source '{record.CanonicalPath}'.");
            }
        }

        /// <summary>Suspends automatic imports until the balanced stop call.</summary>
        public static void StartAssetEditing()
        {
            EnsureCoordinatorWrite();
            if (_assetEditingDepth == 0)
                Editor.Instance.ContentDatabase.SuspendAssetDatabaseAutoRefresh();
            _assetEditingDepth++;
        }

        /// <summary>Ends one asset-editing scope and imports accumulated paths when balanced.</summary>
        public static void StopAssetEditing()
        {
            EnsureCoordinatorWrite();
            if (_assetEditingDepth == 0)
                throw new InvalidOperationException("StopAssetEditing called without a matching StartAssetEditing.");
            if (--_assetEditingDepth != 0)
                return;
            Editor.Instance.ContentDatabase.ResumeAssetDatabaseAutoRefresh();
            FlushDeferredImports();
        }

        /// <summary>Creates a balanced nested asset-editing scope.</summary>
        public static IDisposable AssetEditingScope()
        {
            StartAssetEditing();
            return new Scope(StopAssetEditing);
        }

        /// <summary>Suppresses focus/watcher initiated refresh requests.</summary>
        public static void DisallowAutoRefresh()
        {
            EnsureCoordinatorWrite();
            if (_autoRefreshSuppressionDepth == 0)
                Editor.Instance.ContentDatabase.SuspendAssetDatabaseAutoRefresh();
            _autoRefreshSuppressionDepth++;
        }

        /// <summary>Balances one auto-refresh suppression request.</summary>
        public static void AllowAutoRefresh()
        {
            EnsureCoordinatorWrite();
            if (_autoRefreshSuppressionDepth == 0)
                throw new InvalidOperationException("AllowAutoRefresh called without a matching DisallowAutoRefresh.");
            if (--_autoRefreshSuppressionDepth != 0)
                return;
            Editor.Instance.ContentDatabase.ResumeAssetDatabaseAutoRefresh();
            FlushDeferredImports();
        }

        /// <summary>Returns true inside an isolated managed importer worker.</summary>
        public static bool IsAssetImportWorkerProcess()
        {
            return Environment.GetCommandLineArgs().Any(x => string.Equals(x, "-assetImportWorker", StringComparison.OrdinalIgnoreCase));
        }

        private static void EnsureCoordinatorWrite()
        {
            if (IsAssetImportWorkerProcess())
                throw new InvalidOperationException("Asset database and source mutations are forbidden inside an isolated importer worker.");
        }

        /// <summary>Gets or requests the desired import worker count.</summary>
        public static int DesiredWorkerCount
        {
            get => AssetDatabaseFacade.DesiredWorkerCount;
            set
            {
                if (value < 1 || value > 64)
                    throw new ArgumentOutOfRangeException(nameof(value), "Worker count must be between 1 and 64.");
                AssetDatabaseFacade.DesiredWorkerCount = value;
            }
        }

        /// <summary>Registers a named custom dependency value.</summary>
        public static void RegisterCustomDependency(string dependency, Hash128 hash)
        {
            EnsureCoordinatorWrite();
            if (string.IsNullOrWhiteSpace(dependency))
                throw new ArgumentException("A dependency name is required.", nameof(dependency));
            if (hash.IsZero)
                throw new ArgumentException("A custom dependency hash must be nonzero.", nameof(hash));
            if (AssetDatabaseFacade.RegisterCustomDependency(dependency, Guid.ParseExact(hash.ToString(), "N")))
                throw new InvalidOperationException(GetLastDiagnostic("Custom dependency registration failed."));
        }

        /// <summary>Removes matching custom dependency values.</summary>
        public static void UnregisterCustomDependencyPrefixFilter(string prefixFilter)
        {
            EnsureCoordinatorWrite();
            if (string.IsNullOrWhiteSpace(prefixFilter))
                throw new ArgumentException("A dependency prefix is required.", nameof(prefixFilter));
            if (AssetDatabaseFacade.UnregisterCustomDependencyPrefix(prefixFilter))
                throw new InvalidOperationException(GetLastDiagnostic("Custom dependency removal failed."));
        }

        /// <summary>Gets a stable 128-bit projection of the current published artifact key.</summary>
        public static Hash128 GetArtifactHash(string guid, Content.Settings.BuildTarget target)
        {
            if (!Guid.TryParse(guid, out var id))
                return default;
            var cacheId = AssetDatabaseFacade.GetPublishedArtifactCacheID(id, "runtime");
            if (cacheId == Guid.Empty)
                return default;
            var bytes = cacheId.ToByteArray();
            return new Hash128(BitConverter.ToUInt64(bytes, 8), BitConverter.ToUInt64(bytes, 0));
        }

        /// <summary>Gets immutable metadata for the current artifact generation.</summary>
        public static ArtifactInfo GetCurrentArtifact(string guid, Content.Settings.BuildTarget target)
        {
            var hash = GetArtifactHash(guid, target);
            return new ArtifactInfo
            {
                Guid = guid,
                Key = new ArtifactKey(hash),
                Target = target,
                IsCurrent = !hash.IsZero,
                DatabaseRevision = AssetDatabaseFacade.Revision,
            };
        }

        /// <summary>Produces the host target artifact without exposing mutable artifact paths.</summary>
        public static Task<ArtifactInfo> ProduceArtifactAsync(string guid, Content.Settings.BuildTarget target,
            ImportAssetOptions options = ImportAssetOptions.Default)
        {
            return System.Threading.Tasks.Task.Run(() =>
            {
                var path = GUIDToAssetPath(guid);
                if (string.IsNullOrEmpty(path))
                    throw new ArgumentException("The asset GUID is not registered.", nameof(guid));
                ImportAsset(path, options | ImportAssetOptions.ForceSynchronousImport);
                return GetCurrentArtifact(guid, target);
            });
        }

        /// <summary>Monotonic source/global input revision for diagnostics and waiting.</summary>
        public static ulong GlobalArtifactDependencyVersion => AssetDatabaseFacade.Revision;

        /// <summary>Monotonic count of artifact publication notifications in this editor session.</summary>
        public static ulong GlobalArtifactProcessedVersion => _globalArtifactProcessedVersion;

        private static string PreprocessImport(string physicalPath)
        {
            return AssetPipelineCallbacks.Preprocess(ToLogicalPath(physicalPath));
        }

        private static void QueueImport(string path)
        {
            if (_assetEditingDepth != 0 || _callbackDepth != 0)
                DeferredImports.Add(path);
            else
                ImportAsset(path, Directory.Exists(path) ? ImportAssetOptions.ImportRecursive : ImportAssetOptions.Default);
        }

        private static void FlushDeferredImports()
        {
            if (_assetEditingDepth != 0 || _callbackDepth != 0)
                return;
            if (_deferredRefresh)
            {
                var options = _deferredRefreshOptions;
                _deferredRefresh = false;
                _deferredRefreshOptions = ImportAssetOptions.Default;
                Refresh(options);
            }
            var paths = new List<string>(DeferredImports);
            DeferredImports.Clear();
            for (var i = 0; i < paths.Count; i++)
                QueueImport(paths[i]);
        }

        internal static AssetDatabaseRecordInfo? GetMainRecord(string path)
        {
            if (string.IsNullOrWhiteSpace(path))
                return null;
            var id = AssetDatabaseFacade.AssetPathToGUID(path);
            var records = AssetDatabaseFacade.GetRecords();
            if (id != Guid.Empty)
            {
                for (var i = 0; i < records.Length; i++)
                {
                    if (records[i].IsMain && (records[i].SourceAssetID == id || records[i].ID == id) && records[i].Status != AssetRecordStatus.MissingSource)
                        return records[i];
                }
            }
            var physicalPath = ResolvePhysicalPath(path);
            for (var i = 0; i < records.Length; i++)
            {
                if (!records[i].IsMain || records[i].Status == AssetRecordStatus.MissingSource || string.IsNullOrEmpty(records[i].SourcePath))
                    continue;
                if (string.Equals(Path.GetFullPath(records[i].SourcePath), physicalPath, StringComparison.OrdinalIgnoreCase))
                    return records[i];
            }
            return null;
        }

        internal static IDisposable EnterCallbackScope()
        {
            _callbackDepth++;
            return new Scope(() =>
            {
                if (--_callbackDepth == 0 && _operationDepth == 0)
                    FlushDeferredImports();
            });
        }

        internal static bool IsCallbackScopeActive => _callbackDepth != 0;

        internal static string ResolvePhysicalPathInternal(string path)
        {
            return ResolvePhysicalPath(path);
        }

        internal static string ToLogicalPathInternal(string path)
        {
            return ToLogicalPath(path);
        }

        private static string[] ParseLabels(string labels)
        {
            return string.IsNullOrEmpty(labels)
                ? Array.Empty<string>()
                : labels.Split(new[] { '\n' }, StringSplitOptions.RemoveEmptyEntries);
        }

        private static string ResolvePhysicalPath(string path)
        {
            if (string.IsNullOrWhiteSpace(path))
                return string.Empty;
            path = path.Replace('/', Path.DirectorySeparatorChar);
            return Path.GetFullPath(Path.IsPathRooted(path) ? path : Path.Combine(Globals.ProjectFolder, path));
        }

        private static string ToLogicalPath(string path)
        {
            var root = Path.GetFullPath(Globals.ProjectFolder).TrimEnd(Path.DirectorySeparatorChar) + Path.DirectorySeparatorChar;
            var fullPath = Path.GetFullPath(path);
            return fullPath.StartsWith(root, StringComparison.OrdinalIgnoreCase)
                ? fullPath.Substring(root.Length).Replace(Path.DirectorySeparatorChar, '/')
                : fullPath.Replace(Path.DirectorySeparatorChar, '/');
        }

        private static bool IsProjectContentPath(string path)
        {
            if (string.IsNullOrEmpty(path))
                return false;
            var root = Path.GetFullPath(Globals.ProjectContentFolder).TrimEnd(Path.DirectorySeparatorChar);
            var fullPath = Path.GetFullPath(path);
            return string.Equals(fullPath, root, StringComparison.OrdinalIgnoreCase) ||
                   fullPath.StartsWith(root + Path.DirectorySeparatorChar, StringComparison.OrdinalIgnoreCase);
        }

        private static string GetLastDiagnostic(string fallback)
        {
            var diagnostics = AssetDatabaseFacade.GetDiagnostics();
            return diagnostics.Length == 0 ? fallback : diagnostics[0].Message;
        }

        private sealed class Scope : IDisposable
        {
            private Action _dispose;

            public Scope(Action dispose)
            {
                _dispose = dispose ?? throw new ArgumentNullException(nameof(dispose));
            }

            public void Dispose()
            {
                var dispose = _dispose;
                if (dispose == null)
                    return;
                _dispose = null;
                dispose();
            }
        }
    }
}
