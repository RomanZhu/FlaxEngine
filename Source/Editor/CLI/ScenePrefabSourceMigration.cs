// Copyright (c) Wojciech Figat. All rights reserved.

using System;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Linq;
using Newtonsoft.Json;
using Newtonsoft.Json.Linq;

namespace FlaxEditor
{
    /// <summary>Destructive legacy scene/prefab identity migration.</summary>
    internal static class ScenePrefabSourceMigration
    {
        private sealed class Document
        {
            public string Path;
            public bool Scene;
            public Guid SourceGuid;
            public JArray LegacyObjects;
            public readonly Dictionary<Guid, long> ObjectIds = new();
            public readonly Dictionary<Guid, JObject> ObjectRecords = new();
        }

        private readonly struct ObjectLocation
        {
            public readonly Document Document;
            public readonly Guid LegacyId;

            public ObjectLocation(Document document, Guid legacyId)
            {
                Document = document;
                LegacyId = legacyId;
            }
        }

        /// <summary>Converts every legacy scene and prefab below a Content root. Returns the converted count.</summary>
        public static int ConvertProject(string contentRoot)
        {
            var documents = Discover(contentRoot);
            if (documents.Count == 0)
                return 0;

            var documentsBySource = documents.ToDictionary(x => x.SourceGuid);
            var objects = new Dictionary<Guid, ObjectLocation>();
            foreach (var document in documents)
            {
                AllocateObjectIds(document);
                foreach (var id in document.ObjectIds.Keys)
                {
                    if (!objects.TryAdd(id, new ObjectLocation(document, id)))
                        throw new InvalidDataException($"Legacy object GUID {id:N} occurs in more than one scene/prefab source.");
                }
            }

            var knownAssetSources = ReadKnownAssetSources(contentRoot);
            foreach (var document in documents)
            {
                var source = ConvertDocument(document, documentsBySource, objects, knownAssetSources);
                WriteDocumentAndMetadata(document, source);
            }
            return documents.Count;
        }

        private static List<Document> Discover(string contentRoot)
        {
            var result = new List<Document>();
            var paths = Directory.EnumerateFiles(contentRoot, "*", SearchOption.AllDirectories)
                .Where(x => string.Equals(System.IO.Path.GetExtension(x), ".scene", StringComparison.OrdinalIgnoreCase) ||
                            string.Equals(System.IO.Path.GetExtension(x), ".prefab", StringComparison.OrdinalIgnoreCase))
                .OrderBy(x => x, StringComparer.OrdinalIgnoreCase);
            foreach (var path in paths)
            {
                var json = JObject.Parse(File.ReadAllText(path));
                if (json["sceneVersion"] != null || json["prefabVersion"] != null)
                    continue;
                if (!TryReadGuid(json["ID"], out var embeddedGuid) || json["Data"] is not JArray objects || objects.Count == 0)
                    throw new InvalidDataException($"Legacy scene/prefab '{path}' has no valid ID and Data object array.");
                if (json["ExternalActors"]?.Value<bool>() == true)
                    throw new InvalidDataException($"Legacy external-actor scene '{path}' requires companion source migration before identity conversion.");
                var metadataGuid = ReadMetadataGuid(path + ".meta");
                if (metadataGuid != Guid.Empty && metadataGuid != embeddedGuid)
                    throw new InvalidDataException($"Legacy source identity mismatch for '{path}' (document {embeddedGuid:N}, metadata {metadataGuid:N}).");
                var document = new Document
                {
                    Path = path,
                    Scene = string.Equals(System.IO.Path.GetExtension(path), ".scene", StringComparison.OrdinalIgnoreCase),
                    SourceGuid = metadataGuid == Guid.Empty ? embeddedGuid : metadataGuid,
                    LegacyObjects = objects,
                };
                foreach (var token in objects)
                {
                    if (token is not JObject record || !TryReadGuid(record["ID"], out var objectId))
                        throw new InvalidDataException($"Legacy source '{path}' contains an object without a GUID ID.");
                    if (!document.ObjectRecords.TryAdd(objectId, record))
                        throw new InvalidDataException($"Legacy source '{path}' repeats object GUID {objectId:N}.");
                }
                result.Add(document);
            }
            return result;
        }

        private static void AllocateObjectIds(Document document)
        {
            var reserved = new HashSet<long> { 0, 1 };
            if (document.Scene)
            {
                var first = (JObject)document.LegacyObjects[0];
                var root = ParseGuid(first["ID"], document.Path);
                if (root != document.SourceGuid)
                    throw new InvalidDataException($"Legacy scene '{document.Path}' root ID must equal its source GUID.");
                document.ObjectIds.Add(root, 1);
            }
            foreach (var id in document.ObjectRecords.Keys.OrderBy(x => x.ToString("N"), StringComparer.Ordinal))
            {
                if (document.ObjectIds.ContainsKey(id))
                    continue;
                var candidate = MakeLocalFileId(id);
                while (!reserved.Add(candidate))
                {
                    candidate++;
                    if (candidate <= 1)
                        candidate = 2;
                }
                document.ObjectIds.Add(id, candidate);
            }
        }

        private static JObject ConvertDocument(Document document, IReadOnlyDictionary<Guid, Document> documentsBySource,
            IReadOnlyDictionary<Guid, ObjectLocation> objects, ISet<Guid> knownAssetSources)
        {
            var output = new JObject
            {
                [document.Scene ? "sceneVersion" : "prefabVersion"] = 4,
            };
            var converted = new JArray();
            foreach (var token in document.LegacyObjects)
            {
                var record = (JObject)token.DeepClone();
                var oldId = ParseGuid(record["ID"], document.Path);
                record.Remove("ID");
                record["fileId"] = document.ObjectIds[oldId];
                Rename(record, "TypeName", "type");
                Rename(record, "Name", "name");

                if (record.TryGetValue("ParentID", out var parentToken))
                {
                    var parent = ParseGuid(parentToken, document.Path);
                    if (!document.ObjectIds.TryGetValue(parent, out var parentFileId))
                        throw new InvalidDataException($"Object {oldId:N} in '{document.Path}' has an unknown ParentID {parent:N}.");
                    record.Remove("ParentID");
                    record["parentFileId"] = parentFileId;
                }

                Guid prefabGuid = Guid.Empty;
                if (record.TryGetValue("PrefabID", out var prefabToken))
                {
                    prefabGuid = ParseGuid(prefabToken, document.Path);
                    record.Remove("PrefabID");
                    record["prefabGuid"] = prefabGuid.ToString("N");
                }
                if (record.TryGetValue("PrefabObjectID", out var prefabObjectToken))
                {
                    var legacyPrefabObject = ParseGuid(prefabObjectToken, document.Path);
                    if (prefabGuid == Guid.Empty || !documentsBySource.TryGetValue(prefabGuid, out var prefabDocument) ||
                        !prefabDocument.ObjectIds.TryGetValue(legacyPrefabObject, out var prefabFileId))
                        throw new InvalidDataException($"Object {oldId:N} in '{document.Path}' has an unresolved prefab object identity.");
                    record.Remove("PrefabObjectID");
                    record["prefabObjectFileId"] = prefabFileId;
                }
                if (record.TryGetValue("RemovedObjects", out var removedToken))
                {
                    if (prefabGuid == Guid.Empty || !documentsBySource.TryGetValue(prefabGuid, out var prefabDocument) || removedToken is not JArray removed)
                        throw new InvalidDataException($"Object {oldId:N} in '{document.Path}' has malformed removed prefab objects.");
                    var ids = new JArray();
                    foreach (var item in removed)
                    {
                        var removedGuid = ParseGuid(item, document.Path);
                        if (!prefabDocument.ObjectIds.TryGetValue(removedGuid, out var removedFileId))
                            throw new InvalidDataException($"Object {oldId:N} in '{document.Path}' removes unknown prefab object {removedGuid:N}.");
                        ids.Add(removedFileId);
                    }
                    record.Remove("RemovedObjects");
                    record["removedObjects"] = ids;
                }
                RewriteReferences(record, document, documentsBySource, objects, knownAssetSources);
                converted.Add(record);
            }
            output["objects"] = converted;
            return output;
        }

        private static void RewriteReferences(JToken token, Document owner, IReadOnlyDictionary<Guid, Document> documentsBySource,
            IReadOnlyDictionary<Guid, ObjectLocation> objects, ISet<Guid> knownAssetSources)
        {
            if (token is JObject obj)
            {
                if (obj["guid"]?.Type == JTokenType.String && obj["fileId"]?.Type == JTokenType.Integer)
                    return;
                foreach (var property in obj.Properties().ToArray())
                {
                    if (property.Name == "prefabGuid")
                        continue;
                    if (property.Value.Type == JTokenType.String && TryReadGuid(property.Value, out var id))
                    {
                        if (objects.TryGetValue(id, out var location))
                            property.Value = MakeGlobalReference(location, documentsBySource);
                        else if (knownAssetSources.Contains(id))
                            property.Value = new JObject { ["guid"] = id.ToString("N"), ["fileId"] = 1L };
                    }
                    else
                    {
                        RewriteReferences(property.Value, owner, documentsBySource, objects, knownAssetSources);
                    }
                }
            }
            else if (token is JArray array)
            {
                for (var i = 0; i < array.Count; i++)
                {
                    if (array[i]?.Type == JTokenType.String && TryReadGuid(array[i], out var id))
                    {
                        if (objects.TryGetValue(id, out var location))
                            array[i] = MakeGlobalReference(location, documentsBySource);
                        else if (knownAssetSources.Contains(id))
                            array[i] = new JObject { ["guid"] = id.ToString("N"), ["fileId"] = 1L };
                    }
                    else
                    {
                        RewriteReferences(array[i], owner, documentsBySource, objects, knownAssetSources);
                    }
                }
            }
        }

        private static JObject MakeGlobalReference(ObjectLocation location, IReadOnlyDictionary<Guid, Document> documentsBySource)
        {
            var record = location.Document.ObjectRecords[location.LegacyId];
            if (TryReadGuid(record["PrefabID"], out var prefabGuid) && TryReadGuid(record["PrefabObjectID"], out var prefabObjectGuid))
            {
                if (!documentsBySource.TryGetValue(prefabGuid, out var prefab) || !prefab.ObjectIds.TryGetValue(prefabObjectGuid, out var prefabObjectFileId))
                    throw new InvalidDataException($"Cannot resolve prefab identity for legacy object {location.LegacyId:N}.");
                var instanceRoot = FindPrefabInstanceRoot(location.Document, location.LegacyId, prefabGuid);
                return new JObject
                {
                    ["kind"] = 2,
                    ["guid"] = prefabGuid.ToString("N"),
                    ["fileId"] = prefabObjectFileId,
                    ["prefabInstanceFileId"] = location.Document.ObjectIds[instanceRoot],
                };
            }
            return new JObject
            {
                ["kind"] = location.Document.Scene ? 1 : 2,
                ["guid"] = location.Document.SourceGuid.ToString("N"),
                ["fileId"] = location.Document.ObjectIds[location.LegacyId],
                ["prefabInstanceFileId"] = 0L,
            };
        }

        private static Guid FindPrefabInstanceRoot(Document document, Guid objectId, Guid prefabGuid)
        {
            var current = objectId;
            while (document.ObjectRecords[current].TryGetValue("ParentID", out var parentToken) && TryReadGuid(parentToken, out var parentId) &&
                   document.ObjectRecords.TryGetValue(parentId, out var parent) && TryReadGuid(parent["PrefabID"], out var parentPrefab) && parentPrefab == prefabGuid)
                current = parentId;
            return current;
        }

        private static HashSet<Guid> ReadKnownAssetSources(string contentRoot)
        {
            var result = new HashSet<Guid>();
            foreach (var path in Directory.EnumerateFiles(contentRoot, "*.meta", SearchOption.AllDirectories))
            {
                try
                {
                    var json = JObject.Parse(File.ReadAllText(path));
                    if (TryReadGuid(json["guid"], out var guid))
                        result.Add(guid);
                }
                catch (JsonException)
                {
                }
            }
            return result;
        }

        private static Guid ReadMetadataGuid(string path)
        {
            if (!File.Exists(path))
                return Guid.Empty;
            var json = JObject.Parse(File.ReadAllText(path));
            if (!TryReadGuid(json["guid"], out var guid))
                throw new InvalidDataException($"Scene/prefab metadata '{path}' has no valid guid.");
            return guid;
        }

        private static void WriteDocumentAndMetadata(Document document, JObject source)
        {
            var metadataPath = document.Path + ".meta";
            var sourceText = source.ToString(Formatting.Indented) + Environment.NewLine;
            var metadata = new JObject
            {
                ["fileFormatVersion"] = 1,
                ["guid"] = document.SourceGuid.ToString("N"),
                ["folderAsset"] = false,
                ["importer"] = new JObject
                {
                    ["id"] = "Flax.JsonDocument",
                    ["version"] = 1,
                    ["settings"] = new JObject(),
                },
                ["objectIds"] = new JObject
                {
                    ["main"] = new JObject
                    {
                        ["fileId"] = 1L,
                        ["type"] = document.Scene ? "FlaxEngine.SceneAsset" : "FlaxEngine.Prefab",
                    },
                },
                ["labels"] = new JArray(),
                ["userData"] = new JObject(),
            };
            if (File.Exists(metadataPath))
            {
                var oldMetadata = JObject.Parse(File.ReadAllText(metadataPath));
                if (oldMetadata["labels"] is JArray labels)
                    metadata["labels"] = labels.DeepClone();
                if (oldMetadata["userData"] is JObject userData)
                    metadata["userData"] = userData.DeepClone();
            }
            AtomicWrite(document.Path, sourceText);
            AtomicWrite(metadataPath, metadata.ToString(Formatting.Indented) + Environment.NewLine);
        }

        private static void AtomicWrite(string path, string text)
        {
            var staging = path + ".migration-" + Guid.NewGuid().ToString("N");
            try
            {
                File.WriteAllText(staging, text);
                File.Move(staging, path, true);
            }
            finally
            {
                if (File.Exists(staging))
                    File.Delete(staging);
            }
        }

        private static long MakeLocalFileId(Guid guid)
        {
            var text = guid.ToString("N");
            var a = uint.Parse(text.Substring(0, 8), NumberStyles.HexNumber, CultureInfo.InvariantCulture);
            var b = uint.Parse(text.Substring(8, 8), NumberStyles.HexNumber, CultureInfo.InvariantCulture);
            var c = uint.Parse(text.Substring(16, 8), NumberStyles.HexNumber, CultureInfo.InvariantCulture);
            var d = uint.Parse(text.Substring(24, 8), NumberStyles.HexNumber, CultureInfo.InvariantCulture);
            unchecked
            {
                var value = ((ulong)a << 32) | b;
                value ^= ((ulong)c << 17) | ((ulong)d << 49);
                value ^= value >> 33;
                value *= 0xff51afd7ed558ccdUL;
                value ^= value >> 33;
                value &= long.MaxValue;
                if (value <= 1)
                    value += 2;
                return (long)value;
            }
        }

        private static void Rename(JObject value, string oldName, string newName)
        {
            if (!value.TryGetValue(oldName, out var token))
                return;
            value.Remove(oldName);
            value[newName] = token;
        }

        private static Guid ParseGuid(JToken token, string path)
        {
            if (!TryReadGuid(token, out var result))
                throw new InvalidDataException($"Legacy source '{path}' contains an invalid object GUID.");
            return result;
        }

        private static bool TryReadGuid(JToken token, out Guid result)
        {
            result = Guid.Empty;
            return token?.Type == JTokenType.String && Guid.TryParseExact(token.Value<string>(), "N", out result) && result != Guid.Empty;
        }
    }
}
