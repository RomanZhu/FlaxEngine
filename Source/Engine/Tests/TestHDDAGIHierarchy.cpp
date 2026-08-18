// Copyright (c) Wojciech Figat. All rights reserved.

#include "Engine/Core/Math/Int3.h"
#include "Engine/Renderer/GI/HDDAGIResources.h"
#include <ThirdParty/catch2/catch.hpp>

TEST_CASE("HDDAGI_Hierarchy")
{
    SECTION("Block and Region Coordinate Calculations")
    {
        Int3 voxelCoord(15, 7, 23);
        Int3 regionCoord = voxelCoord / HDDAGI_REGION_SIZE;
        Int3 blockCoord = voxelCoord / HDDAGI_BLOCK_SIZE;

        CHECK(regionCoord.X == 1);
        CHECK(regionCoord.Y == 0);
        CHECK(regionCoord.Z == 2);

        CHECK(blockCoord.X == 3);
        CHECK(blockCoord.Y == 1);
        CHECK(blockCoord.Z == 5);

        Int3 localVoxel(voxelCoord.X % HDDAGI_BLOCK_SIZE, voxelCoord.Y % HDDAGI_BLOCK_SIZE, voxelCoord.Z % HDDAGI_BLOCK_SIZE);
        CHECK(localVoxel.X == 3);
        CHECK(localVoxel.Y == 3);
        CHECK(localVoxel.Z == 3);

        uint32 bitIndex = localVoxel.Z * 16 + localVoxel.Y * 4 + localVoxel.X;
        CHECK(bitIndex == 63);
    }

    SECTION("Block Mask Bit Packing")
    {
        uint32 blockBits[2] = { 0, 0 };

        // Set bit 10
        uint32 bitIdx = 10;
        blockBits[0] |= (1u << bitIdx);

        // Set bit 45 (bit 13 in high word)
        bitIdx = 45;
        blockBits[1] |= (1u << (bitIdx - 32));

        CHECK((blockBits[0] & (1u << 10)) != 0);
        CHECK((blockBits[0] & (1u << 11)) == 0);
        CHECK((blockBits[1] & (1u << 13)) != 0);
        CHECK((blockBits[1] & (1u << 14)) == 0);
    }

    SECTION("Combined Hierarchy Bitwise Merge")
    {
        uint32 staticBlock[2] = { 0x00FF00FF, 0x0000FFFF };
        uint32 dynamicBlock[2] = { 0xFF00FF00, 0xFFFF0000 };

        uint32 combinedBlock[2];
        combinedBlock[0] = staticBlock[0] | dynamicBlock[0];
        combinedBlock[1] = staticBlock[1] | dynamicBlock[1];

        CHECK(combinedBlock[0] == 0xFFFFFFFF);
        CHECK(combinedBlock[1] == 0xFFFFFFFF);
    }
}
