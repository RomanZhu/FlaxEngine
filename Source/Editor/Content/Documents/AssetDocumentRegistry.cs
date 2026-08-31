// Copyright (c) Wojciech Figat. All rights reserved.

using System;
using System.Collections.Generic;
using FlaxEngine;

namespace FlaxEditor.Content.Documents
{
    /// <summary>Deduplicates editor documents by persistent object identity.</summary>
    public static class AssetDocumentRegistry
    {
        private static readonly object Locker = new object();
        private static readonly Dictionary<AssetObjectId, AssetDocumentSession> Sessions = new Dictionary<AssetObjectId, AssetDocumentSession>();
        private static readonly Dictionary<AssetObjectId, int> References = new Dictionary<AssetObjectId, int>();

        public static AssetDocumentSession Open(AssetObjectId id)
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
        public static AssetDocumentSession Open<TDocument>(AssetObjectId id, Func<string, TDocument> documentLoader)
        {
            if (documentLoader == null)
                throw new ArgumentNullException(nameof(documentLoader));
            var session = Open(id);
            session.ConfigureDocumentLoader(path => documentLoader(path));
            return session;
        }

        public static bool Close(AssetObjectId id)
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
