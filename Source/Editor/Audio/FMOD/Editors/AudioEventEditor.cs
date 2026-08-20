// Copyright (c) Wojciech Figat. All rights reserved.

using FlaxEditor.CustomEditors;
using FlaxEditor.CustomEditors.Editors;
using FlaxEngine;
using FlaxEngine.GUI;

namespace FlaxEditor.FMOD
{
    /// <summary>Editor controls for previewing an FMOD event asset outside Play mode.</summary>
    [CustomEditor(typeof(AudioEvent)), DefaultEditor]
    public sealed class AudioEventEditor : GenericEditor
    {
        private AudioEvent _event;
        private AudioEventHandle _handle;
        private Label _state;

        public override void Initialize(LayoutElementsContainer layout)
        {
            base.Initialize(layout);
            _event = Values.Count > 0 ? Values[0] as AudioEvent : null;
            var group = layout.Group("FMOD Preview");
            _state = group.Label("Stopped").Label;
            var buttons = group.UniformGrid();
            buttons.CustomControl.Height = Button.DefaultHeight;
            buttons.CustomControl.SlotsHorizontally = 4;
            buttons.CustomControl.SlotsVertically = 1;
            buttons.Button("Play").Button.Clicked += Play;
            buttons.Button("Pause").Button.Clicked += Pause;
            buttons.Button("Stop").Button.Clicked += Stop;
            buttons.Button("Release").Button.Clicked += Release;
        }

        private void Play()
        {
            Release();
            if (_event == null)
                return;
            _handle = AudioEventSystem.CreatePreviewInstance(_event.BackendId, _event.Path, new AudioEventCreateOptions());
            if (IsHandleValid(_handle))
                AudioEventSystem.PlayPreview(_handle);
            Refresh();
        }

        private void Pause()
        {
            if (IsHandleValid(_handle))
                AudioEventSystem.Pause(_handle);
            Refresh();
        }

        private void Stop()
        {
            if (IsHandleValid(_handle))
                AudioEventSystem.Stop(_handle, AudioStopMode.AllowFadeOut);
            Refresh();
        }

        private void Release()
        {
            if (IsHandleValid(_handle))
                AudioEventSystem.ReleaseInstance(_handle);
            _handle = new AudioEventHandle();
            Refresh();
        }

        public override void Refresh()
        {
            base.Refresh();
            if (_state == null)
                return;
            AudioEventInstanceState state = default;
            if (IsHandleValid(_handle) && AudioEventSystem.QueryInstance(_handle, out state))
                _state.Text = $"{state.PlaybackState} @ {state.TimelinePosition} ms";
            else
                _state.Text = "Stopped";
        }

        protected override void Deinitialize()
        {
            Release();
            base.Deinitialize();
        }

        private static bool IsHandleValid(AudioEventHandle handle) => handle.Generation != 0;
    }
}
