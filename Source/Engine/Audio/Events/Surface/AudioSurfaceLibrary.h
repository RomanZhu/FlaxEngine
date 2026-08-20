// Copyright (c) Wojciech Figat. All rights reserved.

#pragma once

#include "Engine/Scripting/ScriptingObject.h"
#include "Engine/Core/ISerializable.h"
#include "Engine/Core/Collections/Dictionary.h"
#include "Engine/Level/Tags.h"
#include "Engine/Content/JsonAssetReference.h"
#include "Engine/Physics/Types.h"
#include "Engine/Audio/Events/Assets/AudioEvent.h"
#include "AudioInteractionTypes.h"

/// <summary>Reusable authored interaction event set for one physical surface.</summary>
API_STRUCT() struct FLAXENGINE_API AudioSurfaceEventSet : ISerializable
{
    API_AUTO_SERIALIZATION();
    DECLARE_SCRIPTING_TYPE_MINIMAL(AudioSurfaceEventSet);
    API_FIELD(Attributes="AssetReference(\"FlaxEngine.AudioEvent\")") AssetReference<JsonAsset> Footstep;
    API_FIELD(Attributes="AssetReference(\"FlaxEngine.AudioEvent\")") AssetReference<JsonAsset> Landing;
    API_FIELD(Attributes="AssetReference(\"FlaxEngine.AudioEvent\")") AssetReference<JsonAsset> Impact;
    API_FIELD(Attributes="AssetReference(\"FlaxEngine.AudioEvent\")") AssetReference<JsonAsset> ScrapeLoop;
    API_FIELD(Attributes="AssetReference(\"FlaxEngine.AudioEvent\")") AssetReference<JsonAsset> RollLoop;
};

/// <summary>
/// Audio event mappings for physics surface interactions (footsteps, impacts, scrapes).
/// </summary>
API_STRUCT() struct FLAXENGINE_API AudioSurfaceProfile : ISerializable
{
    API_AUTO_SERIALIZATION();
    DECLARE_SCRIPTING_TYPE_MINIMAL(AudioSurfaceProfile);

    /// <summary>
    /// Event played for footstep sounds on this surface.
    /// </summary>
    API_FIELD(Attributes="EditorDisplay(\"Events\"), AssetReference(\"FlaxEngine.AudioEvent\")")
    AssetReference<JsonAsset> FootstepEvent;

    /// <summary>
    /// Event played for collision impacts on this surface.
    /// </summary>
    API_FIELD(Attributes="EditorDisplay(\"Events\"), AssetReference(\"FlaxEngine.AudioEvent\")")
    AssetReference<JsonAsset> ImpactEvent;

    /// <summary>
    /// Event played for landing impacts on this surface.
    /// </summary>
    API_FIELD(Attributes="EditorDisplay(\"Events\"), AssetReference(\"FlaxEngine.AudioEvent\")")
    AssetReference<JsonAsset> LandEvent;

    /// <summary>Extended authored event set for persistent and transient interactions.</summary>
    API_FIELD(Attributes="EditorDisplay(\"Events\")") AudioSurfaceEventSet Interactions;
};

/// <summary>
/// Surface audio library that resolves PhysicalMaterial Tags into audio middleware event cues.
/// </summary>
API_CLASS(Attributes="ContentContextMenu(\"New/Audio/Audio Surface Library\")")
class FLAXENGINE_API AudioSurfaceLibrary final : public ScriptingObject, public ISerializable
{
    API_AUTO_SERIALIZATION();
    DECLARE_SCRIPTING_TYPE_WITH_CONSTRUCTOR_IMPL(AudioSurfaceLibrary, ScriptingObject);

public:
    /// <summary>
    /// Tag-to-profile mappings for surfaces.
    /// </summary>
    API_FIELD(Attributes="EditorOrder(0), EditorDisplay(\"Surfaces\")")
    Dictionary<Tag, AudioSurfaceProfile> Profiles;

    /// <summary>
    /// Fallback profile used when no matching surface tag is found.
    /// </summary>
    API_FIELD(Attributes="EditorOrder(10), EditorDisplay(\"Surfaces\")")
    AudioSurfaceProfile DefaultProfile;

public:
    /// <summary>
    /// Resolves the audio surface profile for a given material tag.
    /// </summary>
    const AudioSurfaceProfile* GetProfile(Tag surfaceTag) const;

    /// <summary>
    /// Tries to resolve the audio surface profile for a given material tag.
    /// </summary>
    API_FUNCTION() bool TryGetProfile(Tag surfaceTag, API_PARAM(Out) AudioSurfaceProfile& outProfile) const;

    /// <summary>
    /// Plays a footstep sound at a world position resolved from a physics raycast hit.
    /// </summary>
    API_FUNCTION() void PlayFootstep(const RayCastHit& hit, float volume = 1.0f);

    /// <summary>Plays an authored landing sound using the resolved surface profile.</summary>
    API_FUNCTION() void PlayLanding(const RayCastHit& hit, float volume = 1.0f);

    /// <summary>
    /// Plays an impact sound at a world position for a given surface tag.
    /// </summary>
    API_FUNCTION() void PlayImpact(Tag surfaceTag, const Vector3& position, float impulse, float volume = 1.0f);

    /// <summary>Plays the strongest coalesced interaction for a physics contact context.</summary>
    void PlayImpact(const AudioImpactContext& context, float volume = 1.0f) const;

    /// <summary>Resolves an authored persistent roll or scrape event for a contact.</summary>
    const AudioEvent* ResolvePersistentEvent(const AudioImpactContext& context, bool rolling) const;
};
