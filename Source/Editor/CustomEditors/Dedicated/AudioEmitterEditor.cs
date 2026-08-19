// Copyright (c) Wojciech Figat. All rights reserved.

using System;
using FlaxEngine;
using FlaxEngine.GUI;

namespace FlaxEditor.CustomEditors.Dedicated
{
    /// <summary>
    /// Custom editor for <see cref="AudioEmitter"/>.
    /// </summary>
    [CustomEditor(typeof(AudioEmitter)), DefaultEditor]
    public class AudioEmitterEditor : ActorEditor
    {
        private Label _infoLabel;

        /// <inheritdoc />
        public override void Initialize(LayoutElementsContainer layout)
        {
            base.Initialize(layout);

            if (Editor.IsPlayMode)
            {
                var playbackGroup = layout.Group("Playback Controls");
                playbackGroup.Panel.Open();

                _infoLabel = playbackGroup.Label(string.Empty).Label;
                _infoLabel.AutoHeight = true;

                var grid = playbackGroup.UniformGrid();
                var gridControl = grid.CustomControl;
                gridControl.ClipChildren = false;
                gridControl.Height = Button.DefaultHeight;
                gridControl.SlotsHorizontally = 3;
                gridControl.SlotsVertically = 1;

                grid.Button("Play").Button.Clicked += () => Foreach(x => x.Play());
                grid.Button("Pause").Button.Clicked += () => Foreach(x => x.Pause());
                grid.Button("Stop").Button.Clicked += () => Foreach(x => x.Stop());
            }
        }

        /// <inheritdoc />
        public override void Refresh()
        {
            base.Refresh();

            if (_infoLabel != null)
            {
                var text = string.Empty;
                foreach (var value in Values)
                {
                    if (value is AudioEmitter emitter)
                    {
                        text += $"State: {emitter.PlaybackState} | Playing: {emitter.IsActuallyPlaying} | Volume: {emitter.Volume:0.00}\n";
                    }
                }
                _infoLabel.Text = text;
            }
        }

        private void Foreach(Action<AudioEmitter> func)
        {
            foreach (var value in Values)
            {
                if (value is AudioEmitter emitter)
                    func(emitter);
            }
        }
    }
}
