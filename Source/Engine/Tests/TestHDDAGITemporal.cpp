// Copyright (c) Wojciech Figat. All rights reserved.

#include "Engine/Core/Math/Vector3.h"
#include "Engine/Core/Math/Math.h"
#include <ThirdParty/catch2/catch.hpp>

TEST_CASE("HDDAGI_Temporal")
{
    SECTION("Moving Window History Ring and Running Sum")
    {
        const int32 historyFrames = 6;
        Float3 historyRing[historyFrames] = {};
        Float3 runningSum = Float3::Zero;
        uint32 validCount = 0;

        Float3 constantRadiance(10.0f, 15.0f, 20.0f);

        for (uint32 frame = 0; frame < 12; frame++)
        {
            uint32 slot = frame % historyFrames;
            Float3 oldSample = historyRing[slot];
            Float3 newSample = constantRadiance;

            runningSum += (newSample - oldSample);
            historyRing[slot] = newSample;

            validCount = Math::Min(validCount + 1u, (uint32)historyFrames);
            Float3 average = runningSum / (float)validCount;

            if (frame >= (uint32)(historyFrames - 1))
            {
                CHECK(Math::NearEqual(average.X, constantRadiance.X));
                CHECK(Math::NearEqual(average.Y, constantRadiance.Y));
                CHECK(Math::NearEqual(average.Z, constantRadiance.Z));
            }
        }
    }
}
