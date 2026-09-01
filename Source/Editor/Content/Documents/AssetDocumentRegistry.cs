// Copyright (c) Wojciech Figat. All rights reserved.

using System;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using FlaxEditor.Content;
using FlaxEngine;

namespace FlaxEditor.Content.Documents
{
    /// <summary>Deduplicates editor documents by persistent object identity.</summary>
    public static class AssetDocumentRegistry
    {
        private static readonly object Locker = new object();
        private static readonly Dictionary<Guid, AssetDocumentSession> Sessions = new Dictionary<Guid, AssetDocumentSession>();
        private static readonly Dictionary<Guid, int> References = new Dictionary<Guid, int>();

        public static AssetDocumentSession Open(Guid id)
        {
            lock (Locker)
            {
                if (!Sessions.TryGetValue(id, out var session))
                {
                    session = new AssetDocumentSession(id);
                    Sessions.Add(id, session);
                    References.Add(id, 0);
                }
                References[id]++;
                return session;
            }
        }

        /// <summary>Opens a shared document session with a source representation loader.</summary>
        public static AssetDocumentSession Open<TDocument>(Guid id, Func<string, TDocument> documentLoader)
        {
            if (documentLoader == null)
                throw new ArgumentNullException(nameof(documentLoader));
            var session = Open(id);
            session.ConfigureDocumentLoader(path => documentLoader(path));
            return session;
        }

        /// <summary>Opens a graph source session and loads the runtime artifact only as its preview target.</summary>
        public static TAsset OpenGraph<TAsset>(AssetItem item, out AssetDocumentSession session) where TAsset : Asset
        {
            if (item == null)
                throw new ArgumentNullException(nameof(item));
            if (!IsGraphSourcePath(item.Path))
                throw new InvalidOperationException("Graph editors only accept authored graph source documents.");
            session = Open(item.ObjectID, AssetDocumentService.LoadGraphSource);
            if (session.Document is not byte[] surface || surface.Length == 0)
                throw new InvalidDataException("The graph source did not produce an editable surface.");
            return FlaxEngine.Content.LoadAssetAsync<TAsset>(item.ObjectID);
        }

        /// <summary>Closes a shared source document session.</summary>
        public static void Close(AssetItem item, ref AssetDocumentSession session)
        {
            if (session == null)
                return;
            Close(session.ObjectID);
            session = null;
        }

        /// <summary>Creates and imports a source-authored graph document.</summary>
        public static Guid CreateGraph(string path, string typeName, string propertiesJson = null)
        {
            var id = AssetDocumentService.CreateGraphSource(path, typeName, propertiesJson);
            if (id == Guid.Empty)
                return Guid.Empty;
            AssetDatabase.ImportAsset(path, ImportAssetOptions.ForceUpdate | ImportAssetOptions.ForceSynchronousImport);
            return id;
        }

        /// <summary>Returns whether a path is an authored graph source.</summary>
        public static bool IsGraphSourcePath(string path)
        {
            switch (Path.GetExtension(path).ToLowerInvariant())
            {
            case ".materialfunction":
            case ".animgraphfunction":
            case ".animgraph":
            case ".visualscript":
            case ".behaviortree":
            case ".particlefunction":
            case ".particleemitter":
            case ".material":
                return true;
            default:
                return false;
            }
        }

        /// <summary>Builds Visual Script source properties.</summary>
        public static string VisualScriptProperties(string baseType, int flags)
        {
            var type = string.IsNullOrEmpty(baseType) ? "FlaxEngine.Script" : baseType;
            return "{\n  \"baseType\": \"" + type + "\",\n  \"flags\": " + flags + "\n}\n";
        }

        /// <summary>Builds material source properties.</summary>
        public static string MaterialProperties(MaterialInfo info)
        {
            return "{\n  \"blendMode\": " + (int)info.BlendMode +
                   ",\n  \"domain\": " + (int)info.Domain +
                   ",\n  \"maskThreshold\": " + info.MaskThreshold.ToString(CultureInfo.InvariantCulture) +
                   ",\n  \"opacityThreshold\": " + info.OpacityThreshold.ToString(CultureInfo.InvariantCulture) +
                   ",\n  \"shadingModel\": " + (int)info.ShadingModel + "\n}\n";
        }

        public static bool Close(Guid id)
        {
            lock (Locker)
            {
                if (!Sessions.TryGetValue(id, out var session))
                    return false;
                var references = References[id] - 1;
                if (references > 0)
                {
                    References[id] = references;
                    return false;
                }
                Sessions.Remove(id);
                References.Remove(id);
                session.Dispose();
                return true;
            }
        }

        public static void RefreshAll()
        {
            AssetDocumentSession[] sessions;
            lock (Locker)
            {
                sessions = new AssetDocumentSession[Sessions.Count];
                Sessions.Values.CopyTo(sessions, 0);
            }
            for (var i = 0; i < sessions.Length; i++)
                sessions[i].Refresh();
        }
    }
}
