// Copyright (c) Wojciech Figat. All rights reserved.

#include "Engine/Audio/Events/AudioEventBackendNone.h"
#include "Engine/Audio/Events/AudioEventSystem.h"
#include "Engine/Audio/Events/Actors/AudioVolumeBase.h"
#include "Engine/Audio/Events/Actors/AudioAreaEmitter.h"
#include "Engine/Audio/Events/Actors/AudioZoneVolume.h"
#include "Engine/Audio/Events/Surface/AudioSurfaceLibrary.h"
#include "Engine/Scripting/ScriptingObject.h"
#include <ThirdParty/catch2/catch.hpp>

TEST_CASE("AudioVolumesAndSurfaces")
{
    SECTION("Box Volume Evaluation and Signed Distance")
    {
        AudioAreaEmitter* emitter = New<AudioAreaEmitter>(ScriptingObject::SpawnParams(Guid::New(), AudioAreaEmitter::TypeInitializer));
        emitter->SetShape(AudioVolumeShape::Box);
        emitter->SetBoxSize(Vector3(200.0f, 200.0f, 200.0f));
        emitter->SetBlendDistanceOutside(100.0f);
        emitter->SetBlendDistanceInside(50.0f);
        emitter->SetPosition(Vector3::Zero);

        // Center sample (inside)
        AudioVolumeSample center = emitter->Evaluate(Vector3::Zero);
        CHECK(center.IsInside);
        CHECK(center.Weight == 1.0f);

        // Outside sample at +150 on X (50cm outside the 100cm half-extent)
        AudioVolumeSample outside = emitter->Evaluate(Vector3(150.0f, 0.0f, 0.0f));
        CHECK(!outside.IsInside);
        CHECK(outside.SignedDistance == Approx(50.0f));
        CHECK(outside.Weight == Approx(0.5f)); // 1.0 - 50/100

        // Far outside sample at +300 on X (200cm outside)
        AudioVolumeSample farOutside = emitter->Evaluate(Vector3(300.0f, 0.0f, 0.0f));
        CHECK(!farOutside.IsInside);
        CHECK(farOutside.Weight == 0.0f);

        Delete(emitter);
    }

    SECTION("Sphere and Capsule Shapes")
    {
        AudioAreaEmitter* emitter = New<AudioAreaEmitter>(ScriptingObject::SpawnParams(Guid::New(), AudioAreaEmitter::TypeInitializer));
        emitter->SetShape(AudioVolumeShape::Sphere);
        emitter->SetSphereRadius(100.0f);
        emitter->SetBlendDistanceOutside(100.0f);
        emitter->SetPosition(Vector3::Zero);

        AudioVolumeSample sample = emitter->Evaluate(Vector3(150.0f, 0.0f, 0.0f));
        CHECK(!sample.IsInside);
        CHECK(sample.SignedDistance == Approx(50.0f));
        CHECK(sample.Weight == Approx(0.5f));

        emitter->SetShape(AudioVolumeShape::Capsule);
        emitter->SetCapsuleRadius(50.0f);
        emitter->SetCapsuleHeight(100.0f);
        AudioVolumeSample capSample = emitter->Evaluate(Vector3(0.0f, 50.0f, 0.0f));
        CHECK(capSample.IsInside);

        Delete(emitter);
    }

    SECTION("Surface Library Profile Lookup")
    {
        AudioSurfaceLibrary* lib = New<AudioSurfaceLibrary>(ScriptingObject::SpawnParams(Guid::New(), AudioSurfaceLibrary::TypeInitializer));
        Tag metalTag = Tags::Get(TEXT("Surface.Metal"));
        Tag woodTag = Tags::Get(TEXT("Surface.Wood"));

        AudioSurfaceProfile metalProfile;
        lib->Profiles[metalTag] = metalProfile;

        const auto* found = lib->GetProfile(metalTag);
        CHECK(found != nullptr);

        const auto* def = lib->GetProfile(woodTag);
        CHECK(def == &lib->DefaultProfile);

        Delete(lib);
    }
}
