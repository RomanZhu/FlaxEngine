// Copyright (c) Wojciech Figat. All rights reserved.

using System;
using System.IO;
using System.Security.Cryptography;
using FlaxEngine;

namespace FlaxEditor.Content.Documents
{
    /// <summary>Editor session for one authoritative source document.</summary>
    public sealed class AssetDocumentSession : IDisposable
    {
        private Func<string, object> _documentLoader;
        private byte[] _baseSourceSnapshot;

        /// <summary>Gets the persistent identity of the edited object.</summary>
        public AssetObjectId ObjectID { get; }

        /// <summary>Gets the database revision on which the local document is based.</summary>
        public ulong ObservedRevision { get; private set; }

        /// <summary>Gets the canonical source path.</summary>
        public string SourcePath { get; private set; }

        /// <summary>Gets the hash of the source snapshot on which the local document is based.</summary>
        public string BaseSourceHash { get; private set; }

        /// <summary>Gets a copy of the source snapshot used as the merge base.</summary>
        public byte[] BaseSourceSnapshot => _baseSourceSnapshot == null ? null : (byte[])_baseSourceSnapshot.Clone();

        /// <summary>Gets the parsed, editable source document.</summary>
        public object Document { get; private set; }

        /// <summary>Gets whether the document has unsaved changes.</summary>
        public bool IsDirty { get; private set; }

        /// <summary>Gets whether the source is missing or changed outside this session.</summary>
        public bool IsStale { get; private set; }

        /// <summary>Gets whether an external source change conflicts with local edits.</summary>
        public bool HasExternalConflict { get; private set; }

        /// <summary>Gets the most recent import error after a successful source save.</summary>
        public string LastImportError { get; private set; }

        /// <summary>Occurs when the session state changes.</summary>
        public event Action<AssetDocumentSession> Changed;

        internal AssetDocumentSession(AssetObjectId objectId)
        {
            if (objectId.IsNull)
                throw new ArgumentException("A document session requires a persistent asset object ID.", nameof(objectId));
            ObjectID = objectId;
            Refresh();
        }

        internal void ConfigureDocumentLoader(Func<string, object> documentLoader)
        {
            if (documentLoader == null)
                throw new ArgumentNullException(nameof(documentLoader));
            if (_documentLoader == null)
                _documentLoader = documentLoader;
            if (Document == null && !string.IsNullOrEmpty(SourcePath) && File.Exists(SourcePath))
                Document = _documentLoader(SourcePath);
        }

        /// <summary>Gets the editable source document with the requested representation.</summary>
        public T GetDocument<T>()
        {
            if (Document is T value)
                return value;
            throw new InvalidOperationException($"The source document is not represented as {typeof(T).FullName}.");
        }

        /// <summary>Replaces the editable source document and marks the session dirty.</summary>
        public void SetDocument<T>(T document)
        {
            if (document is null)
                throw new ArgumentNullException(nameof(document));
            Document = document;
            MarkDirty();
        }

        /// <summary>Marks the document as having unsaved changes.</summary>
        public void MarkDirty()
        {
            if (IsDirty)
                return;
            IsDirty = true;
            Changed?.Invoke(this);
        }

        /// <summary>Refreshes database and conflict state without overwriting dirty local edits.</summary>
        public void Refresh()
        {
            Refresh(false);
        }

        /// <summary>Reloads the source document, discarding local edits and conflicts.</summary>
        public bool ReloadFromDisk()
        {
            return Refresh(true);
        }

        private bool Refresh(bool discardLocalEdits)
        {
            if (!AssetWorkspaceQuery.TryGet(ObjectID, out var entry))
            {
                IsStale = true;
                HasExternalConflict = IsDirty;
                Changed?.Invoke(this);
                return false;
            }

            SourcePath = entry.SourcePath;
            if (!TryReadSource(SourcePath, out var snapshot, out var hash))
            {
                IsStale = true;
                HasExternalConflict = IsDirty;
                Changed?.Invoke(this);
                return false;
            }

            var sourceChanged = BaseSourceHash != null && !string.Equals(BaseSourceHash, hash, StringComparison.Ordinal);
            if (sourceChanged && IsDirty && !discardLocalEdits)
            {
                IsStale = true;
                HasExternalConflict = true;
                Changed?.Invoke(this);
                return false;
            }

            if (discardLocalEdits || sourceChanged || Document == null)
            {
                if (_documentLoader != null)
                    Document = _documentLoader(SourcePath);
            }

            ObservedRevision = entry.Revision;
            BaseSourceHash = hash;
            _baseSourceSnapshot = snapshot;
            IsDirty = false;
            IsStale = false;
            HasExternalConflict = false;
            LastImportError = null;
            Changed?.Invoke(this);
            return true;
        }

        /// <summary>Saves the canonical source and optionally imports it synchronously.</summary>
        /// <param name="saveSource">Callback that atomically writes the edited source. Returns true on success.</param>
        /// <param name="allowOverwriteConflict">Whether to replace an externally changed source.</param>
        /// <param name="importAfterSave">Whether this session must request import after the callback.</param>
        /// <returns>True when the source was committed. Import failure is reported separately.</returns>
        public bool Save(Func<AssetDocumentSession, bool> saveSource, bool allowOverwriteConflict = false, bool importAfterSave = true)
        {
            if (!IsDirty)
                return true;
            if (saveSource == null)
                throw new ArgumentNullException(nameof(saveSource));

            if (!TryReadSource(SourcePath, out _, out var currentHash))
            {
                IsStale = true;
                HasExternalConflict = true;
                Changed?.Invoke(this);
                return false;
            }
            if (!allowOverwriteConflict && BaseSourceHash != null && !string.Equals(BaseSourceHash, currentHash, StringComparison.Ordinal))
            {
                IsStale = true;
                HasExternalConflict = true;
                Changed?.Invoke(this);
                return false;
            }
            if (!saveSource(this))
                return false;

            if (!TryReadSource(SourcePath, out var snapshot, out var hash))
                return false;
            BaseSourceHash = hash;
            _baseSourceSnapshot = snapshot;
            IsDirty = false;
            IsStale = false;
            HasExternalConflict = false;
            LastImportError = null;

            if (importAfterSave)
            {
                try
                {
                    AssetDatabase.ImportAsset(SourcePath, ImportAssetOptions.ForceSynchronousImport);
                }
                catch (Exception ex)
                {
                    LastImportError = ex.Message;
                }
            }

            if (AssetWorkspaceQuery.TryGet(ObjectID, out var entry))
                ObservedRevision = entry.Revision;
            Changed?.Invoke(this);
            return true;
        }

        private static bool TryReadSource(string path, out byte[] snapshot, out string hash)
        {
            snapshot = null;
            hash = null;
            if (string.IsNullOrEmpty(path))
                return false;
            try
            {
                snapshot = File.ReadAllBytes(path);
                using var algorithm = SHA256.Create();
                hash = BitConverter.ToString(algorithm.ComputeHash(snapshot)).Replace("-", string.Empty);
                return true;
            }
            catch (IOException)
            {
                return false;
            }
            catch (UnauthorizedAccessException)
            {
                return false;
            }
        }

        /// <inheritdoc />
        public void Dispose()
        {
            Document = null;
            _documentLoader = null;
            _baseSourceSnapshot = null;
            Changed = null;
        }
    }
}
