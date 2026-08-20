// Copyright (c) Wojciech Figat. All rights reserved.

using FlaxEditor.CustomEditors;
using FlaxEditor.CustomEditors.Editors;
using FlaxEngine;
using FlaxEngine.GUI;

namespace FlaxEditor.FMOD
{
    /// <summary>Editor controls for loading and inspecting an FMOD bank asset.</summary>
    [CustomEditor(typeof(AudioBank)), DefaultEditor]
    public sealed class AudioBankEditor : GenericEditor
    {
        private AudioBank _bank;
        private Label _state;

        public override void Initialize(LayoutElementsContainer layout)
        {
            base.Initialize(layout);
            _bank = Values.Count > 0 ? Values[0] as AudioBank : null;
            var group = layout.Group("FMOD Bank Preview");
            _state = group.Label("Unknown").Label;
            var buttons = group.UniformGrid();
            buttons.CustomControl.Height = Button.DefaultHeight;
            buttons.CustomControl.SlotsHorizontally = 3;
            buttons.CustomControl.SlotsVertically = 1;
            buttons.Button("Load").Button.Clicked += Load;
            buttons.Button("Samples").Button.Clicked += LoadSamples;
            buttons.Button("Unload").Button.Clicked += Unload;
        }

        private void Load()
        {
            if (_bank != null)
                AudioEventSystem.LoadBank(_bank.BackendId, _bank.Path, _bank.NonBlocking);
            Refresh();
        }

        private void LoadSamples()
        {
            if (_bank != null)
                AudioEventSystem.LoadBankSampleData(_bank.BackendId);
            Refresh();
        }

        private void Unload()
        {
            if (_bank != null)
                AudioEventSystem.UnloadBank(_bank.BackendId, _bank.Path);
            Refresh();
        }

        public override void Refresh()
        {
            base.Refresh();
            if (_state != null && _bank != null)
                _state.Text = $"{AudioEventSystem.GetBankState(_bank.BackendId)} | Loaded: {AudioEventSystem.IsBankLoaded(_bank.BackendId)}";
        }
    }
}
