// Copyright (c) Wojciech Figat. All rights reserved.

#include "Engine/Core/Math/Vector3.h"
#include "Engine/Core/Math/Int3.h"
#include "Engine/Renderer/GI/HDDAGIResources.h"
#include <ThirdParty/catch2/catch.hpp>

TEST_CASE("HDDAGI_HDDA")
{
    SECTION("Ray Cache Packing and Unpacking")
    {
        Int3 hitCell(45, 23, 112);
        uint32 cascade = 2;
        uint32 exitAxis = 1;
        bool hit = true;
        bool valid = true;

        // Pack
        uint32 packed = 0;
        packed |= (uint32(hitCell.X) & 0xFFu);
        packed |= (uint32(hitCell.Y) & 0xFFu) << 8;
        packed |= (uint32(hitCell.Z) & 0xFFu) << 16;
        packed |= (cascade & 0x07u) << 24;
        packed |= (exitAxis & 0x03u) << 27;
        if (hit) packed |= (1u << 30);
        if (valid) packed |= (1u << 31);

        // Unpack
        Int3 outCell;
        outCell.X = int32(packed & 0xFFu);
        outCell.Y = int32((packed >> 8) & 0xFFu);
        outCell.Z = int32((packed >> 16) & 0xFFu);
        uint32 outCascade = (packed >> 24) & 0x07u;
        uint32 outExitAxis = (packed >> 27) & 0x03u;
        bool outHit = (packed & (1u << 30)) != 0;
        bool outValid = (packed & (1u << 31)) != 0;

        CHECK(outCell.X == hitCell.X);
        CHECK(outCell.Y == hitCell.Y);
        CHECK(outCell.Z == hitCell.Z);
        CHECK(outCascade == cascade);
        CHECK(outExitAxis == exitAxis);
        CHECK(outHit == hit);
        CHECK(outValid == valid);
    }
}
