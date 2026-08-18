// Copyright (c) Wojciech Figat. All rights reserved.

#include "Engine/Core/Math/BoundingBox.h"
#include "Engine/Renderer/GI/GlobalGIInvalidation.h"
#include <ThirdParty/catch2/catch.hpp>

TEST_CASE("HDDAGI_DynamicRegions")
{
    SECTION("Invalidation Bounding Box Merge and Expansion")
    {
        BoundingBox prevBox(Vector3(0, 0, 0), Vector3(100, 200, 100));
        BoundingBox currBox(Vector3(50, 0, 0), Vector3(150, 200, 100));

        GlobalGIInvalidation invalidation(prevBox, currBox, GlobalGIDirtyFlags::GeometryChanged);
        BoundingBox combined = invalidation.GetCombinedBounds();

        CHECK(combined.Minimum.X == 0.0f);
        CHECK(combined.Maximum.X == 150.0f);
        CHECK(combined.Minimum.Y == 0.0f);
        CHECK(combined.Maximum.Y == 200.0f);

        float cellSize = 10.0f;
        BoundingBox expanded = invalidation.ExpandForVoxelization(cellSize);
        CHECK(expanded.Minimum.X == -10.0f);
        CHECK(expanded.Maximum.X == 160.0f);
    }
}
