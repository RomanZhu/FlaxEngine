// Copyright (c) Wojciech Figat. All rights reserved.

#include "Engine/Core/Math/Vector3.h"
#include "Engine/Core/Math/Int3.h"
#include "Engine/Core/Math/Math.h"
#include "Engine/Renderer/GI/HDDAGIResources.h"
#include <ThirdParty/catch2/catch.hpp>

TEST_CASE("HDDAGI_CascadeScrolling")
{
    SECTION("Region Snapped Center Movement")
    {
        float cellSize = 10.0f;
        float regionStep = cellSize * (float)HDDAGI_REGION_SIZE; // 80 units

        Vector3 cameraPos(125.0f, 40.0f, -230.0f);

        Int3 targetRegionCoord = Int3(
            Math::FloorToInt((float)cameraPos.X / regionStep),
            Math::FloorToInt((float)cameraPos.Y / regionStep),
            Math::FloorToInt((float)cameraPos.Z / regionStep)
        );

        CHECK(targetRegionCoord.X == 1);
        CHECK(targetRegionCoord.Y == 0);
        CHECK(targetRegionCoord.Z == -3);
    }
}
