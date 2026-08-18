// Copyright (c) Wojciech Figat. All rights reserved.

#include "Engine/Core/Math/BoundingBox.h"
#include "Engine/Core/Math/Transform.h"
#include "Engine/Core/Math/Vector3.h"
#include "Engine/Renderer/GI/GlobalGIInvalidation.h"
#include "Engine/Renderer/GlobalSignDistanceFieldPass.h"
#include <ThirdParty/catch2/catch.hpp>

TEST_CASE("GlobalGIDirtyRegion")
{
    SECTION("Flags")
    {
        GlobalGIDirtyFlags flags = GlobalGIDirtyFlags::None;
        CHECK(flags == GlobalGIDirtyFlags::None);

        flags |= GlobalGIDirtyFlags::GeometryChanged;
        CHECK(EnumHasAnyFlags(flags, GlobalGIDirtyFlags::GeometryChanged));
        CHECK_FALSE(EnumHasAnyFlags(flags, GlobalGIDirtyFlags::LightingChanged));

        flags |= GlobalGIDirtyFlags::LightingChanged;
        CHECK(EnumHasAllFlags(flags, GlobalGIDirtyFlags::GeometryChanged | GlobalGIDirtyFlags::LightingChanged));
        CHECK_FALSE(EnumHasAnyFlags(flags, GlobalGIDirtyFlags::EmissionChanged));

        flags |= GlobalGIDirtyFlags::SurfaceChanged | GlobalGIDirtyFlags::EmissionChanged;
        CHECK(EnumHasAllFlags(flags, GlobalGIDirtyFlags::GeometryChanged | GlobalGIDirtyFlags::SurfaceChanged | GlobalGIDirtyFlags::LightingChanged | GlobalGIDirtyFlags::EmissionChanged));
    }

    SECTION("Default Initialization")
    {
        GlobalGIDirtyRegion region;
        CHECK(EnumHasAllFlags(region.Flags, GlobalGIDirtyFlags::GeometryChanged | GlobalGIDirtyFlags::LightingChanged));
    }

    SECTION("Combined Bounds Union")
    {
        BoundingBox prevBox(Vector3(-100, 0, -50), Vector3(-80, 200, 50));
        BoundingBox currBox(Vector3(100, 0, -50), Vector3(120, 200, 50));

        GlobalGIDirtyRegion region(prevBox, currBox, GlobalGIDirtyFlags::GeometryChanged);
        CHECK(region.PreviousBounds == prevBox);
        CHECK(region.CurrentBounds == currBox);
        CHECK(region.Flags == GlobalGIDirtyFlags::GeometryChanged);

        BoundingBox combined = region.GetCombinedBounds();
        CHECK(combined.Minimum == Vector3(-100, 0, -50));
        CHECK(combined.Maximum == Vector3(120, 200, 50));
    }

    SECTION("Partial Bounds (Add or Remove)")
    {
        BoundingBox newBox(Vector3(0, 0, 0), Vector3(50, 50, 50));
        GlobalGIDirtyRegion addRegion(BoundingBox::Empty, newBox);
        CHECK(addRegion.GetCombinedBounds() == newBox);

        BoundingBox oldBox(Vector3(10, 10, 10), Vector3(60, 60, 60));
        GlobalGIDirtyRegion removeRegion(oldBox, BoundingBox::Empty);
        CHECK(removeRegion.GetCombinedBounds() == oldBox);
    }
}

TEST_CASE("GlobalSDFDynamicInvalidation")
{
    SECTION("Dynamic Door Motion Invalidation")
    {
        // Simulate a rotating/sliding door between Room A and Room B
        const Vector3 doorDimensions(10.0f, 210.0f, 90.0f);
        const Vector3 doorwayCenter(0.0f, 105.0f, 0.0f);

        // Frame 0: Door is closed
        BoundingBox doorClosed(doorwayCenter - doorDimensions * 0.5f, doorwayCenter + doorDimensions * 0.5f);

        // Frame 1: Door rotates 90 degrees (open)
        Vector3 openDoorDimensions(90.0f, 210.0f, 10.0f);
        Vector3 openDoorCenter(45.0f, 105.0f, 45.0f);
        BoundingBox doorOpen(openDoorCenter - openDoorDimensions * 0.5f, openDoorCenter + openDoorDimensions * 0.5f);

        // Invalidation dirty region must cover both the old closed blocker and new open blocker
        GlobalGIDirtyRegion doorMovedRegion(doorClosed, doorOpen);
        BoundingBox unionBounds = doorMovedRegion.GetCombinedBounds();

        // Old position must be strictly inside the dirty region
        CHECK(unionBounds.Contains(doorClosed) == ContainmentType::Contains);
        // New position must be strictly inside the dirty region
        CHECK(unionBounds.Contains(doorOpen) == ContainmentType::Contains);

        // Frame 2: Teleport door across room
        Vector3 teleportCenter(500.0f, 105.0f, 0.0f);
        BoundingBox doorTeleported(teleportCenter - doorDimensions * 0.5f, teleportCenter + doorDimensions * 0.5f);

        GlobalGIDirtyRegion teleportRegion(doorOpen, doorTeleported);
        BoundingBox teleportUnion = teleportRegion.GetCombinedBounds();

        CHECK(teleportUnion.Contains(doorOpen) == ContainmentType::Contains);
        CHECK(teleportUnion.Contains(doorTeleported) == ContainmentType::Contains);
        CHECK(teleportUnion.Minimum.X <= doorOpen.Minimum.X);
        CHECK(teleportUnion.Maximum.X >= doorTeleported.Maximum.X);
    }

    SECTION("Continuous Door Motion Sequence")
    {
        // 10 continuous animation steps
        BoundingBox prevBox(Vector3(0, 0, 0), Vector3(10, 200, 100));
        for (int32 step = 1; step <= 10; step++)
        {
            float offset = (float)step * 10.0f;
            BoundingBox currBox(Vector3(offset, 0, 0), Vector3(offset + 10.0f, 200, 100));

            GlobalGIDirtyRegion stepRegion(prevBox, currBox);
            BoundingBox stepUnion = stepRegion.GetCombinedBounds();

            CHECK(stepUnion.Contains(prevBox) == ContainmentType::Contains);
            CHECK(stepUnion.Contains(currBox) == ContainmentType::Contains);

            prevBox = currBox;
        }
    }
}

TEST_CASE("DynamicGIRoomSetup")
{
    SECTION("Phase 0 Test Scene Structural Specification")
    {
        // Room A (Bright)
        BoundingBox roomA(Vector3(-1000.0f, 0.0f, -500.0f), Vector3(0.0f, 400.0f, 500.0f));

        // Room B (Dark)
        BoundingBox roomB(Vector3(0.0f, 0.0f, -500.0f), Vector3(1000.0f, 400.0f, 500.0f));

        // Shared wall at X = 0
        BoundingBox sharedWall(Vector3(-10.0f, 0.0f, -500.0f), Vector3(10.0f, 400.0f, 500.0f));

        // Doorway opening in shared wall
        BoundingBox doorwayOpening(Vector3(-15.0f, 0.0f, -60.0f), Vector3(15.0f, 220.0f, 60.0f));

        // Closed door positioned inside doorway opening
        BoundingBox doorClosed(Vector3(-10.0f, 0.0f, -50.0f), Vector3(10.0f, 210.0f, 50.0f));

        // Open door swung into Room A
        BoundingBox doorOpen(Vector3(-100.0f, 0.0f, 40.0f), Vector3(0.0f, 210.0f, 60.0f));

        // Light sources
        Vector3 brightLightPosInRoomA(-500.0f, 300.0f, 0.0f);
        Vector3 emissiveObjectPosInRoomB(500.0f, 100.0f, 0.0f);

        // Verification of room boundaries and spatial layout
        CHECK(roomA.Contains(brightLightPosInRoomA) == ContainmentType::Contains);
        CHECK(roomB.Contains(emissiveObjectPosInRoomB) == ContainmentType::Contains);
        CHECK(doorwayOpening.Contains(doorClosed) == ContainmentType::Contains);
        CHECK_FALSE(doorwayOpening.Contains(doorOpen) == ContainmentType::Contains);
        CHECK(roomA.Contains(doorOpen) == ContainmentType::Contains);

        // Shared wall separates Room A and Room B
        CHECK(sharedWall.Intersects(roomA));
        CHECK(sharedWall.Intersects(roomB));
    }
}
