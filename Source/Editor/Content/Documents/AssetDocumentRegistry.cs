// Copyright (c) Wojciech Figat. All rights reserved.

using System.Collections.Generic;
using FlaxEngine;

namespace FlaxEditor.Content.Documents
{
    /// <summary>Deduplicates editor documents by persistent object identity.</summary>
    public static class AssetDocumentRegistry
    {
        private static readonly object Locker = new object();
        private static readonly Dictionary<AssetObjectId, AssetDocumentSession> Sessions = new Dictionary<AssetObjectId, AssetDocumentSession>();

        public static AssetDocumentSession Open(AssetObjectId id)
        {
            lock (Locker)
            {
                if (!Sessions.TryGetValue(id, out var session))
                {
                    session = new AssetDocumentSession(id);
                    Sessions.Add(id, session);
                }
                return session;
            }
        }

        public static bool Close(AssetObjectId id)
        {
            lock (Locker)
            {
                if (!Sessions.TryGetValue(id, out var session))
                    return false;
                Sessions.Remove(id);
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
