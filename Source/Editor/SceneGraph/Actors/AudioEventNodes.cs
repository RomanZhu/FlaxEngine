// Copyright (c) Wojciech Figat. All rights reserved.

#if USE_LARGE_WORLDS
using Real = System.Double;
#else
using Real = System.Single;
#endif

using FlaxEngine;

namespace FlaxEditor.SceneGraph.Actors
{
    /// <summary>
    /// Base scene tree node for box, sphere, and capsule audio areas.
    /// </summary>
    [HideInEditor]
    public abstract class AudioVolumeNodeBase : ActorNodeWithIcon
    {
        /// <inheritdoc />
        protected AudioVolumeNodeBase(Actor actor)
            : base(actor)
        {
        }

        /// <inheritdoc />
        public override bool RayCastSelf(ref RayCastData ray, out Real distance, out Vector3 normal)
        {
            normal = Vector3.Up;

            if ((ray.Flags & RayCastData.FlagTypes.SkipEditorPrimitives) == 0 && RayCastWire(ref ray.Ray, ref ray.View.Position, out distance))
                return true;

            return base.RayCastSelf(ref ray, out distance, out normal);
        }

        /// <summary>
        /// Tests the visible box wire independently of scene depth competition.
        /// </summary>
        internal bool RayCastWire(ref Ray ray, ref Vector3 viewPosition, out Real distance)
        {
            var audioVolume = (AudioVolumeBase)_actor;
            if (audioVolume.Shape == AudioVolumeShape.Box)
            {
                var halfSize = audioVolume.BoxSize * 0.5f;
                var box = new OrientedBoundingBox(-halfSize, halfSize);
                var transform = audioVolume.Transform;
                box.Transform(ref transform);
                return Utilities.Utils.RayCastWire(ref box, ref ray, out distance, ref viewPosition);
            }

            distance = 0;
            return false;
        }
    }

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
    public sealed class AudioAreaEmitterNode : AudioVolumeNodeBase
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
    public sealed class AudioZoneVolumeNode : AudioVolumeNodeBase
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
    public sealed class AudioTriggerNode : AudioVolumeNodeBase
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
