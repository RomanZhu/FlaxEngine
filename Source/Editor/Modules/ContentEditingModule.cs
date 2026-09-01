// Copyright (c) Wojciech Figat. All rights reserved.

using System;
using System.IO;
using System.Runtime.InteropServices;
using FlaxEditor.Content;
using FlaxEditor.GUI.Docking;
using FlaxEditor.Windows;
using FlaxEngine;

namespace FlaxEditor.Modules
{
    /// <summary>
    /// Opening/editing asset windows module.
    /// </summary>
    /// <seealso cref="FlaxEditor.Modules.EditorModule" />
    public sealed class ContentEditingModule : EditorModule
    {
        internal ContentEditingModule(Editor editor)
        : base(editor)
        {
        }

        /// <summary>
        /// Opens the specified asset in dedicated editor window.
        /// </summary>
        /// <param name="asset">The asset.</param>
        /// <param name="disableAutoShow">True if disable automatic window showing. Used during workspace layout loading to deserialize it faster.</param>
        /// <returns>Opened window or null if cannot open item.</returns>
        public EditorWindow Open(Asset asset, bool disableAutoShow = false)
        {
            if (asset == null)
                throw new ArgumentNullException();
            var item = Editor.ContentDatabase.FindAsset(asset.ID);
            return item != null ? Open(item) : null;
        }

        /// <summary>
        /// Opens the specified item in dedicated editor window.
        /// </summary>
        /// <param name="item">The content item.</param>
        /// <param name="disableAutoShow">True if disable automatic window showing. Used during workspace layout loading to deserialize it faster.</param>
        /// <returns>Opened window or null if cannot open item.</returns>
        public EditorWindow Open(ContentItem item, bool disableAutoShow = false)
        {
            if (item == null)
                throw new ArgumentNullException();

            // Check if any window is already editing this item
            var window = Editor.Windows.FindEditor(item);
            if (window != null)
            {
                window.Focus();
                return window;
            }

            // Find proxy object
            var proxy = Editor.ContentDatabase.GetProxy(item);
            if (proxy == null)
            {
                Editor.Log("Missing content proxy object for " + item);
                return null;
            }
            if (item is AssetItem { IsCanonicalSubAsset: false } authoredItem &&
                IsAuthoredDocumentProcessor(authoredItem.ProcessorID) && proxy is AssetProxy assetProxy &&
                !assetProxy.AcceptsAsset(authoredItem.TypeName, authoredItem.Path))
            {
                Editor.LogWarning($"Refusing to open authored document '{authoredItem.Path}' through mismatched proxy {proxy.GetType().FullName}.");
                return null;
            }

            // Open
            try
            {
                window = proxy.Open(Editor, item);
            }
            catch (Exception ex)
            {
                Editor.LogWarning(ex);
            }
            if (window != null && !disableAutoShow)
            {
                Editor.Windows.Open(window);
            }

            return window;
        }

        private static bool IsAuthoredDocumentProcessor(string processor)
        {
            return processor is "Flax.GraphDocument" or "Flax.MaterialInstance" or "Flax.SkeletonMask" or
                   "Flax.SceneAnimation" or "Flax.ParticleSystem" or "Flax.CollisionData" or
                   "Flax.JsonDocument" or "Flax.Settings";
        }

        /// <summary>
        /// Determines whether specified new short name is valid name for the given content item.
        /// </summary>
        /// <param name="item">The item.</param>
        /// <param name="shortName">The new short name.</param>
        /// <param name="hint">The hint text if name is invalid.</param>
        /// <returns><c>true</c> if name is valid; otherwise, <c>false</c>.</returns>
        public bool IsValidAssetName(ContentItem item, string shortName, out string hint)
        {
            var trimmedName = shortName?.Trim();
            if (shortName != trimmedName && !string.IsNullOrEmpty(shortName) && char.IsWhiteSpace(shortName[shortName.Length - 1]))
            {
                hint = "Name cannot end with whitespace.";
                return false;
            }
            shortName = trimmedName;
            if (string.IsNullOrEmpty(shortName))
            {
                hint = "Name cannot be empty.";
                return false;
            }
            if (shortName.EndsWith(".", StringComparison.Ordinal) || shortName.EndsWith(" ", StringComparison.Ordinal))
            {
                hint = "Name cannot end with a dot or space.";
                return false;
            }
            if (RuntimeInformation.IsOSPlatform(OSPlatform.Windows))
            {
                switch (Path.GetFileNameWithoutExtension(shortName).ToUpperInvariant())
                {
                case "CON": case "PRN": case "AUX": case "NUL":
                case "COM1": case "COM2": case "COM3": case "COM4": case "COM5": case "COM6": case "COM7": case "COM8": case "COM9":
                case "LPT1": case "LPT2": case "LPT3": case "LPT4": case "LPT5": case "LPT6": case "LPT7": case "LPT8": case "LPT9":
                    hint = "Name is reserved by Windows.";
                    return false;
                }
            }

            // Check if name is the same except has some chars in upper case and some in lower case
            if (shortName.Equals(item.ShortName, StringComparison.OrdinalIgnoreCase))
            {
                // The same file names but some chars have different case
            }
            else
            {
                // Validate length
                if (shortName.Length == 0)
                {
                    hint = "Name cannot be empty.";
                    return false;
                }
                if (shortName.Length > 60)
                {
                    hint = "Too long name.";
                    return false;
                }

                // Find invalid characters
                if (Utilities.Utils.HasInvalidPathChar(shortName))
                {
                    hint = "Name contains invalid character.";
                    return false;
                }

                // Check proxy name restrictions
                if (item is NewItem ni)
                {
                    if (!ni.Proxy.IsFileNameValid(shortName))
                    {
                        hint = "Name does not follow " + ni.Proxy.Name + " name restrictions !";
                        return false;
                    }
                }
                else
                {
                    var proxy = Editor.ContentDatabase.GetProxy(item);
                    if (proxy != null && !proxy.IsFileNameValid(shortName))
                    {
                        hint = "Name does not follow " + proxy.Name + " name restrictions !";
                        return false;
                    }
                }

                // Cache data
                string sourcePath = item.Path;
                string sourceFolder = System.IO.Path.GetDirectoryName(sourcePath);
                string extension = item.IsFolder ? "" : System.IO.Path.GetExtension(sourcePath);
                string destinationPath = StringUtils.CombinePaths(sourceFolder, shortName + extension);

                var pathComparison = RuntimeInformation.IsOSPlatform(OSPlatform.Windows) ? StringComparison.OrdinalIgnoreCase : StringComparison.Ordinal;
                var isSamePath = string.Equals(Path.GetFullPath(sourcePath), Path.GetFullPath(destinationPath), pathComparison);
                if (!isSamePath && (Directory.Exists(destinationPath) || File.Exists(destinationPath)))
                {
                    hint = "A file or folder with this name already exists.";
                    return false;
                }
            }

            hint = string.Empty;
            return true;
        }

        /// <summary>
        /// Clones the asset to the temporary folder.
        /// </summary>
        /// <param name="srcPath">The path of the source asset to clone.</param>
        /// <param name="resultPath">The result path.</param>
        /// <returns>True if failed, otherwise false.</returns>
        public bool FastTempAssetClone(string srcPath, out string resultPath)
        {
            var extension = System.IO.Path.GetExtension(srcPath);
            var id = Guid.NewGuid();
            resultPath = StringUtils.CombinePaths(Globals.TemporaryFolder, id.ToString("N")) + extension;

            if (CloneAssetFile(srcPath, resultPath, id))
                return true;

            return false;
        }

        internal bool TryGetBinaryAssetStorageId(string path, out Guid storageId)
        {
            return !Editor.Internal_GetBinaryAssetStorageId(path, out storageId);
        }

        internal bool RepairBinaryAssetStorageId(string path, Guid currentId, Guid expectedId)
        {
            bool failed = true;
            Editor.ContentDatabase.BeginAssetSave(path);
            try
            {
                failed = Editor.Internal_RepairBinaryAssetStorageId(path, ref currentId, ref expectedId);
            }
            finally
            {
                Editor.ContentDatabase.EndAssetSave(path, !failed);
            }
            return !failed;
        }

        /// <summary>
        /// Duplicates the asset file and changes it's ID.
        /// </summary>
        /// <param name="srcPath">The source file path.</param>
        /// <param name="dstPath">The destination file path.</param>
        /// <param name="dstId">The destination asset identifier.</param>
        /// <param name="overwrite">Whether an existing file should be replaced, with rollback on failure.</param>
        /// <returns>True if cannot perform that operation, otherwise false.</returns>
        public bool CloneAssetFile(string srcPath, string dstPath, Guid dstId, bool overwrite = false)
        {
            // Use internal call
            return Editor.Internal_CloneAssetFile(dstPath, srcPath, ref dstId, overwrite);
        }
    }
}
