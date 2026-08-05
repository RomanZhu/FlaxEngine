// Copyright (c) Wojciech Figat. All rights reserved.

using System;
using FlaxEditor.Content;
using FlaxEngine;

namespace FlaxEditor
{
    /// <summary>
    /// Type of an undo action target.
    /// </summary>
    [HideInEditor]
    public enum UndoActionTargetType
    {
        /// <summary>
        /// Unknown or unspecified target.
        /// </summary>
        Unknown,

        /// <summary>
        /// Multiple targets.
        /// </summary>
        Multiple,

        /// <summary>
        /// Scene object or scene graph node.
        /// </summary>
        SceneObject,

        /// <summary>
        /// Content browser item.
        /// </summary>
        ContentItem,

        /// <summary>
        /// Asset or asset document.
        /// </summary>
        Asset,

        /// <summary>
        /// Editor or project settings.
        /// </summary>
        Settings,

        /// <summary>
        /// Editor document or tab.
        /// </summary>
        Document,
    }

    /// <summary>
    /// Extra undo action behavior flags used by the editor history coordinator.
    /// </summary>
    [Flags]
    [HideInEditor]
    public enum UndoActionFlags
    {
        /// <summary>
        /// No extra behavior.
        /// </summary>
        None = 0,

        /// <summary>
        /// The related editor document may need to be reopened to display this action target.
        /// </summary>
        RequiresReopen = 1,

        /// <summary>
        /// The related resource should be reloaded or refreshed after applying the action.
        /// </summary>
        RequiresReload = 2,

        /// <summary>
        /// The action changes the content database and related UI should be refreshed.
        /// </summary>
        AffectsContentDatabase = 4,

        /// <summary>
        /// The action stores preserved state on disk instead of only in memory.
        /// </summary>
        DiskBacked = 8,

        /// <summary>
        /// The action changes editor selection state only and should not mark documents as data-edited.
        /// </summary>
        SelectionOnly = 16,

        /// <summary>
        /// The action can replay only while its original owner/source undo context is still alive.
        /// </summary>
        RequiresLiveOwner = 32,

        /// <summary>
        /// The action changes editor-only state and should not mark documents or assets as data-edited.
        /// </summary>
        EditorStateOnly = 64,
    }

    /// <summary>
    /// Policy used to replay an undo action when its target resource is hidden, unloaded, or closed.
    /// </summary>
    [HideInEditor]
    public enum UndoActionReplayPolicy
    {
        /// <summary>
        /// No explicit replay policy.
        /// </summary>
        Unspecified,

        /// <summary>
        /// The action can apply to the target without reopening a document or reloading a resource.
        /// </summary>
        ApplyInPlace,

        /// <summary>
        /// The action should reload or refresh the target resource after replay.
        /// </summary>
        Reload,

        /// <summary>
        /// The action should reopen or focus the target document before replay.
        /// </summary>
        Reopen,

        /// <summary>
        /// The action depends on the original live owner instance.
        /// </summary>
        LiveOwner,

        /// <summary>
        /// The action cannot replay once its target resource is hidden, unloaded, or closed.
        /// </summary>
        Unsupported,
    }

    /// <summary>
    /// Optional metadata attached to undo actions so the global editor history can identify affected resources.
    /// </summary>
    [Serializable]
    [HideInEditor]
    public sealed class UndoActionInfo
    {
        /// <summary>
        /// Empty metadata.
        /// </summary>
        public static readonly UndoActionInfo Empty = new UndoActionInfo();

        /// <summary>
        /// The operation that occurred.
        /// </summary>
        public string Operation { get; set; }

        /// <summary>
        /// The primary target type.
        /// </summary>
        public UndoActionTargetType TargetType { get; set; }

        /// <summary>
        /// The primary target display name.
        /// </summary>
        public string TargetName { get; set; }

        /// <summary>
        /// The primary resource path.
        /// </summary>
        public string TargetPath { get; set; }

        /// <summary>
        /// Secondary resource path, such as a rename or move destination.
        /// </summary>
        public string SecondaryTargetPath { get; set; }

        /// <summary>
        /// The primary asset or object id, if available.
        /// </summary>
        public Guid TargetId { get; set; } = Guid.Empty;

        /// <summary>
        /// Additional target object id or member path for non-asset objects.
        /// </summary>
        public string TargetObjectId { get; set; }

        /// <summary>
        /// The editor/window type that can display the target.
        /// </summary>
        public string DisplayEditorTypeName { get; set; }

        /// <summary>
        /// The persistent owner type name. For document-local actions this is the resource or editor context that owns the edited target.
        /// </summary>
        public string OwnerTypeName { get; set; }

        /// <summary>
        /// The persistent owner resource path.
        /// </summary>
        public string OwnerPath { get; set; }

        /// <summary>
        /// The persistent owner asset or object id, if available.
        /// </summary>
        public Guid OwnerId { get; set; } = Guid.Empty;

        /// <summary>
        /// Extra behavior flags.
        /// </summary>
        public UndoActionFlags Flags { get; set; }

        /// <summary>
        /// The replay policy used when the target resource is hidden, unloaded, or closed.
        /// </summary>
        public UndoActionReplayPolicy ReplayPolicy { get; set; }

        /// <summary>
        /// Approximate memory or disk cost in bytes. Negative means unknown.
        /// </summary>
        public long SizeInBytes { get; set; } = -1;

        /// <summary>
        /// Creates undo action metadata for a content item.
        /// </summary>
        /// <param name="operation">The operation.</param>
        /// <param name="item">The content item.</param>
        /// <param name="flags">Extra behavior flags.</param>
        /// <returns>The metadata.</returns>
        public static UndoActionInfo ForContentItem(string operation, ContentItem item, UndoActionFlags flags = UndoActionFlags.None)
        {
            if (item == null)
                throw new ArgumentNullException(nameof(item));

            var result = new UndoActionInfo
            {
                Operation = operation,
                TargetType = item is AssetItem ? UndoActionTargetType.Asset : UndoActionTargetType.ContentItem,
                TargetName = item.ShortName,
                TargetPath = item.Path,
                Flags = flags,
            };
            if (item is AssetItem assetItem)
                result.TargetId = assetItem.ID;
            return result;
        }

        /// <summary>
        /// Creates undo action owner metadata for a content item.
        /// </summary>
        /// <param name="operation">The operation.</param>
        /// <param name="item">The content item.</param>
        /// <param name="editorType">The editor type that owns or displays the content item.</param>
        /// <param name="flags">Extra behavior flags.</param>
        /// <param name="replayPolicy">The replay policy.</param>
        /// <returns>The metadata.</returns>
        public static UndoActionInfo ForContentOwner(string operation, ContentItem item, Type editorType = null, UndoActionFlags flags = UndoActionFlags.None, UndoActionReplayPolicy replayPolicy = UndoActionReplayPolicy.Unspecified)
        {
            var result = ForContentItem(operation, item, flags);
            result.OwnerTypeName = item.GetType().FullName;
            result.OwnerPath = result.TargetPath;
            result.OwnerId = result.TargetId;
            result.DisplayEditorTypeName = editorType?.FullName;
            result.ReplayPolicy = replayPolicy;
            return result;
        }

        /// <summary>
        /// Creates undo action metadata for a generic object.
        /// </summary>
        /// <param name="operation">The operation.</param>
        /// <param name="target">The target object.</param>
        /// <returns>The metadata.</returns>
        public static UndoActionInfo ForObject(string operation, object target)
        {
            var result = new UndoActionInfo
            {
                Operation = operation,
                TargetType = UndoActionTargetType.Unknown,
            };

            if (target == null)
                return result;

            result.TargetName = target.ToString();
            result.TargetObjectId = target.GetHashCode().ToString();
            if (target is ContentItem item)
            {
                result.TargetType = item is AssetItem ? UndoActionTargetType.Asset : UndoActionTargetType.ContentItem;
                result.TargetName = item.ShortName;
                result.TargetPath = item.Path;
                if (item is AssetItem assetItem)
                    result.TargetId = assetItem.ID;
            }
            else if (target is FlaxEngine.Object flaxObject)
            {
                result.TargetType = UndoActionTargetType.SceneObject;
                result.TargetName = flaxObject.ToString();
                result.TargetId = flaxObject.ID;
                result.TargetObjectId = flaxObject.ID.ToString("N");
            }

            return result;
        }

        /// <summary>
        /// Creates a copy of this metadata.
        /// </summary>
        /// <returns>The copied metadata.</returns>
        public UndoActionInfo Clone()
        {
            return new UndoActionInfo
            {
                Operation = Operation,
                TargetType = TargetType,
                TargetName = TargetName,
                TargetPath = TargetPath,
                SecondaryTargetPath = SecondaryTargetPath,
                TargetId = TargetId,
                TargetObjectId = TargetObjectId,
                DisplayEditorTypeName = DisplayEditorTypeName,
                OwnerTypeName = OwnerTypeName,
                OwnerPath = OwnerPath,
                OwnerId = OwnerId,
                Flags = Flags,
                ReplayPolicy = ReplayPolicy,
                SizeInBytes = SizeInBytes,
            };
        }
    }

    /// <summary>
    /// Optional interface for undo actions that expose target/resource metadata.
    /// </summary>
    public interface IUndoActionMetadata
    {
        /// <summary>
        /// Gets the undo action metadata.
        /// </summary>
        UndoActionInfo ActionInfo { get; }
    }

    /// <summary>
    /// Optional interface for undo owners that expose persistent resource metadata for child actions.
    /// </summary>
    public interface IUndoActionOwnerMetadata
    {
        /// <summary>
        /// Gets metadata for the owner resource that contains child undo actions.
        /// </summary>
        UndoActionInfo UndoOwnerActionInfo { get; }
    }

    /// <summary>
    /// Optional interface for undo owners that can replace a live linked child action with a resource-bound parent action.
    /// </summary>
    public interface IUndoLinkedActionProvider
    {
        /// <summary>
        /// Creates a parent undo action for the source action.
        /// </summary>
        /// <param name="sourceUndo">The source undo context.</param>
        /// <param name="sourceAction">The source action.</param>
        /// <returns>The parent action, or null to use the default live linked action.</returns>
        IUndoAction CreateLinkedUndoAction(Undo sourceUndo, IUndoAction sourceAction);
    }

    /// <summary>
    /// Utility methods for reading undo action metadata.
    /// </summary>
    public static class UndoActionMetadata
    {
        /// <summary>
        /// Gets metadata for an undo action.
        /// </summary>
        /// <param name="action">The undo action.</param>
        /// <returns>The undo action metadata.</returns>
        public static UndoActionInfo GetActionInfo(IUndoAction action)
        {
            if (action is IUndoActionMetadata metadata)
                return metadata.ActionInfo ?? UndoActionInfo.Empty;

            return action != null
                ? new UndoActionInfo { Operation = action.ActionString }
                : UndoActionInfo.Empty;
        }

        /// <summary>
        /// Gets metadata for an undo owner object.
        /// </summary>
        /// <param name="owner">The undo owner.</param>
        /// <returns>The undo owner metadata.</returns>
        public static UndoActionInfo GetOwnerActionInfo(object owner)
        {
            if (owner is IUndoActionOwnerMetadata metadata)
            {
                var ownerInfo = metadata.UndoOwnerActionInfo;
                if (ownerInfo != null)
                    return ownerInfo;
            }

            return owner != null
                ? new UndoActionInfo
                {
                    DisplayEditorTypeName = owner.GetType().FullName,
                    OwnerTypeName = owner.GetType().FullName,
                }
                : UndoActionInfo.Empty;
        }

        /// <summary>
        /// Merges persistent owner metadata into action metadata.
        /// </summary>
        /// <param name="actionInfo">The action metadata to update.</param>
        /// <param name="ownerInfo">The owner metadata.</param>
        public static void ApplyOwnerInfo(UndoActionInfo actionInfo, UndoActionInfo ownerInfo)
        {
            if (actionInfo == null || ownerInfo == null)
                return;

            if (string.IsNullOrEmpty(actionInfo.DisplayEditorTypeName))
                actionInfo.DisplayEditorTypeName = ownerInfo.DisplayEditorTypeName;
            if (string.IsNullOrEmpty(actionInfo.OwnerTypeName))
                actionInfo.OwnerTypeName = !string.IsNullOrEmpty(ownerInfo.OwnerTypeName) ? ownerInfo.OwnerTypeName : ownerInfo.DisplayEditorTypeName;
            if (string.IsNullOrEmpty(actionInfo.OwnerPath))
                actionInfo.OwnerPath = !string.IsNullOrEmpty(ownerInfo.OwnerPath) ? ownerInfo.OwnerPath : ownerInfo.TargetPath;
            if (actionInfo.OwnerId == Guid.Empty)
                actionInfo.OwnerId = ownerInfo.OwnerId != Guid.Empty ? ownerInfo.OwnerId : ownerInfo.TargetId;
            if (actionInfo.ReplayPolicy == UndoActionReplayPolicy.Unspecified)
                actionInfo.ReplayPolicy = ownerInfo.ReplayPolicy;
        }

        /// <summary>
        /// Checks if the undo action changes editor selection only.
        /// </summary>
        /// <param name="action">The undo action.</param>
        /// <returns>True if the action changes selection only, otherwise false.</returns>
        public static bool IsSelectionOnly(IUndoAction action)
        {
            var info = GetActionInfo(action);
            return (info.Flags & UndoActionFlags.SelectionOnly) != 0;
        }

        /// <summary>
        /// Checks if the undo action does not change edited document or asset data.
        /// </summary>
        /// <param name="action">The undo action.</param>
        /// <returns>True if the action changes editor-only or selection-only state, otherwise false.</returns>
        public static bool DoesNotModifyData(IUndoAction action)
        {
            if (action is MultiUndoAction multiAction)
            {
                for (int i = 0; i < multiAction.Actions.Length; i++)
                {
                    if (!DoesNotModifyData(multiAction.Actions[i]))
                        return false;
                }
                return true;
            }

            var info = GetActionInfo(action);
            return (info.Flags & (UndoActionFlags.SelectionOnly | UndoActionFlags.EditorStateOnly)) != 0;
        }
    }
}
