// Copyright (c) Wojciech Figat. All rights reserved.

using FlaxEngine;

namespace FlaxEditor.SceneGraph.Actors
{
    /// <summary>
    /// Scene tree node for <see cref="AudioEmitter"/> actor type.
    /// </summary>
    [HideInEditor]
    public sealed class AudioEmitterNode : ActorNodeWithIcon
    {
        /// <inheritdoc />
        public AudioEmitterNode(Actor actor)
            : base(actor)
        {
        }
    }

    /// <summary>
    /// Scene tree node for <see cref="AudioBankLoader"/> actor type.
    /// </summary>
    [HideInEditor]
    public sealed class AudioBankLoaderNode : ActorNodeWithIcon
    {
        /// <inheritdoc />
        public AudioBankLoaderNode(Actor actor)
            : base(actor)
        {
        }
    }

    /// <summary>
    /// Scene tree node for <see cref="AudioAreaEmitter"/> actor type.
    /// </summary>
    [HideInEditor]
    public sealed class AudioAreaEmitterNode : ActorNodeWithIcon
    {
        /// <inheritdoc />
        public AudioAreaEmitterNode(Actor actor)
            : base(actor)
        {
        }
    }

    /// <summary>
    /// Scene tree node for <see cref="AudioZoneVolume"/> actor type.
    /// </summary>
    [HideInEditor]
    public sealed class AudioZoneVolumeNode : ActorNodeWithIcon
    {
        /// <inheritdoc />
        public AudioZoneVolumeNode(Actor actor)
            : base(actor)
        {
        }
    }

    /// <summary>
    /// Scene tree node for <see cref="AudioTrigger"/> actor type.
    /// </summary>
    [HideInEditor]
    public sealed class AudioTriggerNode : ActorNodeWithIcon
    {
        /// <inheritdoc />
        public AudioTriggerNode(Actor actor)
            : base(actor)
        {
        }
    }

    /// <summary>Scene tree node for <see cref="AudioParameterTrigger"/>.</summary>
    [HideInEditor]
    public sealed class AudioParameterTriggerNode : ActorNodeWithIcon
    {
        /// <inheritdoc />
        public AudioParameterTriggerNode(Actor actor)
            : base(actor)
        {
        }
    }
}
