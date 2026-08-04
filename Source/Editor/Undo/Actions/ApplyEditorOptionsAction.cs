// Copyright (c) Wojciech Figat. All rights reserved.

using System;
using FlaxEditor.Options;
using FlaxEditor.Windows;
using FlaxEngine.Json;
using FlaxEngine.Utilities;

namespace FlaxEditor
{
    /// <summary>
    /// Undo action for applying editor options.
    /// </summary>
    /// <seealso cref="IUndoAction" />
    public sealed class ApplyEditorOptionsAction : IUndoAction, IUndoActionMetadata
    {
        private OptionsModule _options;
        private EditorOptions _before;
        private EditorOptions _after;
        private readonly long _sizeInBytes;

        /// <summary>
        /// Initializes a new instance of the <see cref="ApplyEditorOptionsAction"/> class.
        /// </summary>
        /// <param name="options">The editor options module.</param>
        /// <param name="before">The options before applying changes.</param>
        /// <param name="after">The options after applying changes.</param>
        public ApplyEditorOptionsAction(OptionsModule options, EditorOptions before, EditorOptions after)
        {
            _options = options ?? throw new ArgumentNullException(nameof(options));
            _before = before?.DeepClone() ?? throw new ArgumentNullException(nameof(before));
            _after = after?.DeepClone() ?? throw new ArgumentNullException(nameof(after));
            _sizeInBytes = GetOptionsSize(_before) + GetOptionsSize(_after);
        }

        /// <inheritdoc />
        public string ActionString => "Apply editor options";

        /// <inheritdoc />
        public UndoActionInfo ActionInfo => new UndoActionInfo
        {
            Operation = ActionString,
            TargetType = UndoActionTargetType.Settings,
            TargetName = "Editor Options",
            DisplayEditorTypeName = typeof(EditorOptionsWindow).FullName,
            Flags = UndoActionFlags.RequiresReload,
            SizeInBytes = _sizeInBytes,
        };

        /// <inheritdoc />
        public void Do()
        {
            _options?.Apply(_after.DeepClone());
        }

        /// <inheritdoc />
        public void Undo()
        {
            _options?.Apply(_before.DeepClone());
        }

        /// <inheritdoc />
        public void Dispose()
        {
            _options = null;
            _before = null;
            _after = null;
        }

        private static long GetOptionsSize(EditorOptions options)
        {
            var json = JsonSerializer.Serialize(options);
            return json?.Length ?? -1;
        }
    }
}
