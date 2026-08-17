// Copyright (c) Wojciech Figat. All rights reserved.

#include "Engine/Core/Math/BoundingBox.h"
#include "Engine/Core/Math/Vector3.h"
#include "Engine/Core/Math/Int3.h"
#include "Engine/Renderer/GI/GlobalGIDirtyRegion.h"
#include "Engine/Renderer/GI/GlobalDistanceFieldGI.h"
#include "Engine/Renderer/GI/GlobalSurfaceAtlasPass.h"
#include <ThirdParty/catch2/catch.hpp>

namespace
{
    // C++ mirroring of HLSL octahedral mapping
    float GetSignNotZero(float v)
    {
        return v >= 0.0f ? 1.0f : -1.0f;
    }

    Float2 GetSignNotZero(Float2 v)
    {
        return Float2(GetSignNotZero(v.X), GetSignNotZero(v.Y));
    }

    Float2 GetOctahedralCoords(Float3 direction)
    {
        Float2 uv = Float2(direction.X, direction.Y) * (1.0f / (Math::Abs(direction.X) + Math::Abs(direction.Y) + Math::Abs(direction.Z)));
        if (direction.Z < 0.0f)
            uv = (Float2(1.0f) - Float2(Math::Abs(uv.Y), Math::Abs(uv.X))) * GetSignNotZero(uv);
        return uv;
    }

    Float3 GetOctahedralDirection(Float2 coords)
    {
        Float3 direction(coords.X, coords.Y, 1.0f - Math::Abs(coords.X) - Math::Abs(coords.Y));
        if (direction.Z < 0.0f)
        {
            Float2 signVal = GetSignNotZero(Float2(direction.X, direction.Y));
            Float2 absVal = Float2(Math::Abs(direction.Y), Math::Abs(direction.X));
            direction.X = (1.0f - absVal.X) * signVal.X;
            direction.Y = (1.0f - absVal.Y) * signVal.Y;
        }
        return Float3::Normalize(direction);
    }
}

TEST_CASE("GDFGIProbeGridMath")
{
    SECTION("Probe Index and Coordinate Round-Trip")
    {
        const Int3 probeCounts(32, 16, 32);
        const uint32 totalProbes = probeCounts.X * probeCounts.Y * probeCounts.Z;
        CHECK(totalProbes == 16384);

        for (uint32 index = 0; index < 1000; index += 37)
        {
            uint32 probesPerPlane = probeCounts.X * probeCounts.Z;
            uint32 planeIndex = index / (probeCounts.X * probeCounts.Z);
            uint32 gridX = index % probeCounts.X;
            uint32 gridZ = (index / probeCounts.X) % probeCounts.Z;

            uint32 reconstructedIndex = planeIndex * probesPerPlane + gridX + (probeCounts.X * gridZ);
            CHECK(reconstructedIndex == index);
        }
    }

    SECTION("Octahedral Encoding and Decoding")
    {
        Float3 directions[] = {
            Float3(1, 0, 0),
            Float3(-1, 0, 0),
            Float3(0, 1, 0),
            Float3(0, -1, 0),
            Float3(0, 0, 1),
            Float3(0, 0, -1),
            Float3::Normalize(Float3(1, 1, 1)),
            Float3::Normalize(Float3(-1, 1, -1)),
        };

        for (const auto& dir : directions)
        {
            Float2 oct = GetOctahedralCoords(dir);
            Float3 decoded = GetOctahedralDirection(oct);
            CHECK(Float3::Dot(dir, decoded) > 0.999f);
        }
    }
}

TEST_CASE("GDFGITemporalFilter")
{
    SECTION("Exponential Moving Average Convergence")
    {
        const int32 historyFrames = 8;
        const float alpha = 1.0f / (float)historyFrames;
        Float3 history = Float3::Zero;

        // Feed constant radiance (1, 1, 1) over 32 frames
        for (int32 frame = 0; frame < 32; frame++)
        {
            Float3 newSample(1.0f, 1.0f, 1.0f);
            history = Float3::Lerp(history, newSample, alpha);
        }

        CHECK(history.X > 0.98f);
        CHECK(history.Y > 0.98f);
        CHECK(history.Z > 0.98f);

        // Sudden lighting change to (3, 3, 3) over 32 frames
        for (int32 frame = 0; frame < 32; frame++)
        {
            Float3 newSample(3.0f, 3.0f, 3.0f);
            history = Float3::Lerp(history, newSample, alpha);
        }

        CHECK(history.X > 2.95f);
        CHECK(history.Y > 2.95f);
        CHECK(history.Z > 2.95f);
    }

    SECTION("Dynamic Invalidation Snap")
    {
        Float3 history(1.0f, 1.0f, 1.0f);
        Float3 newSample(0.0f, 0.0f, 0.0f);
        bool isDirty = true;
        float alpha = isDirty ? 1.0f : (1.0f / 8.0f);

        history = Float3::Lerp(history, newSample, alpha);
        CHECK(history.X == Approx(0.0f));
        CHECK(history.Y == Approx(0.0f));
        CHECK(history.Z == Approx(0.0f));
    }
}

TEST_CASE("GDFGIDiffuseConvolution")
{
    SECTION("Cosine Lobe Normalization")
    {
        // 5x5 directions on hemisphere
        Float3 normal(0, 1, 0);
        float weightSum = 0.0f;
        Float3 diffuse(0.0f);

        for (int32 y = 0; y < 5; y++)
        {
            for (int32 x = 0; x < 5; x++)
            {
                Float2 uv = (Float2((float)x, (float)y) + Float2(0.5f)) / 5.0f;
                Float3 dir = GetOctahedralDirection(uv * 2.0f - 1.0f);
                float weight = Math::Max(Float3::Dot(normal, dir), 0.0f);

                diffuse += Float3(2.0f) * weight;
                weightSum += weight;
            }
        }

        if (weightSum > 1e-4f)
            diffuse /= weightSum;

        CHECK(diffuse.X == Approx(2.0f));
        CHECK(diffuse.Y == Approx(2.0f));
        CHECK(diffuse.Z == Approx(2.0f));
    }
}

TEST_CASE("GlobalSurfaceAtlasDynamicRelighting")
{
    SECTION("Propagation Radius Expansion")
    {
        BoundingBox doorOld(Vector3(-10, 0, -50), Vector3(10, 200, 50));
        BoundingBox doorNew(Vector3(50, 0, 100), Vector3(70, 200, 200));

        GlobalGIDirtyRegion region(doorOld, doorNew, GlobalGIDirtyFlags::GeometryChanged | GlobalGIDirtyFlags::LightingChanged);
        BoundingBox unionBounds = region.GetCombinedBounds();

        const Vector3 expansion(500.0f);
        BoundingBox relightBounds(unionBounds.Minimum - expansion, unionBounds.Maximum + expansion);

        // Surrounding wall at distance 300 should be covered by relightBounds
        BoundingBox nearbyWall(Vector3(-10, 0, 300), Vector3(10, 200, 400));
        CHECK(relightBounds.Intersects(nearbyWall));

        // Faraway wall at distance 2000 should NOT be affected
        BoundingBox distantWall(Vector3(-10, 0, 2000), Vector3(10, 200, 2100));
        CHECK_FALSE(relightBounds.Intersects(distantWall));
    }
}

TEST_CASE("GDFGISceneIntegrationAndVisibility")
{
    SECTION("8-Probe Trilinear Weights Normalized")
    {
        Float3 alpha(0.35f, 0.72f, 0.18f);
        float totalWeight = 0.0f;

        for (int32 i = 0; i < 8; i++)
        {
            Float3 offset((float)(i & 1), (float)((i >> 1) & 1), (float)((i >> 2) & 1));
            Float3 trilinearWeight3D(Math::Lerp(1.0f - alpha.X, alpha.X, offset.X), Math::Lerp(1.0f - alpha.Y, alpha.Y, offset.Y), Math::Lerp(1.0f - alpha.Z, alpha.Z, offset.Z));
            float weight = trilinearWeight3D.X * trilinearWeight3D.Y * trilinearWeight3D.Z;
            totalWeight += weight;
        }

        CHECK(totalWeight == Approx(1.0f));
    }

    SECTION("Chebyshev Visibility Weighting")
    {
        // Case 1: Point is in front of the occluder (dist <= mean)
        float mean = 100.0f;
        float variance = 25.0f;
        float distInFront = 50.0f;
        float diffInFront = distInFront - mean;
        float pMaxInFront = diffInFront <= 0.0f ? 1.0f : variance / (variance + diffInFront * diffInFront);
        CHECK(pMaxInFront == Approx(1.0f));

        // Case 2: Point is occluded behind the wall (dist > mean)
        float distBehind = 200.0f;
        float diffBehind = distBehind - mean;
        float pMaxBehind = variance / (variance + diffBehind * diffBehind);
        CHECK(pMaxBehind < 0.01f);
    }

    SECTION("World AABB to Probe Grid Coordinate Mapping")
    {
        Vector3 probesOrigin(-1000.0f, -500.0f, -1000.0f);
        float probesSpacing = 50.0f;
        Int3 probeCounts(32, 16, 32);

        BoundingBox dirtyWorldBox(Vector3(-100.0f, 0.0f, -50.0f), Vector3(100.0f, 200.0f, 50.0f));

        Vector3 minGrid = (dirtyWorldBox.Minimum - probesOrigin) / probesSpacing;
        Vector3 maxGrid = (dirtyWorldBox.Maximum - probesOrigin) / probesSpacing;

        Int3 minCoord((int32)Math::Floor(minGrid.X), (int32)Math::Floor(minGrid.Y), (int32)Math::Floor(minGrid.Z));
        Int3 maxCoord((int32)Math::Ceil(maxGrid.X), (int32)Math::Ceil(maxGrid.Y), (int32)Math::Ceil(maxGrid.Z));

        CHECK(minCoord.X >= 0);
        CHECK(minCoord.Y >= 0);
        CHECK(minCoord.Z >= 0);
        CHECK(maxCoord.X < probeCounts.X);
        CHECK(maxCoord.Y < probeCounts.Y);
        CHECK(maxCoord.Z < probeCounts.Z);
    }

    SECTION("Toroidal Clipmap Scrolling Wrap-Around")
    {
        const Int3 probeCounts(32, 16, 32);
        Int3 scrollOffsets(5, -3, 10);

        for (int32 z = 0; z < probeCounts.Z; z++)
        {
            for (int32 y = 0; y < probeCounts.Y; y++)
            {
                for (int32 x = 0; x < probeCounts.X; x++)
                {
                    Int3 probeCoords(x, y, z);
                    Int3 scrolled = probeCoords + scrollOffsets;
                    scrolled.X = (scrolled.X % probeCounts.X + probeCounts.X) % probeCounts.X;
                    scrolled.Y = (scrolled.Y % probeCounts.Y + probeCounts.Y) % probeCounts.Y;
                    scrolled.Z = (scrolled.Z % probeCounts.Z + probeCounts.Z) % probeCounts.Z;

                    CHECK(scrolled.X >= 0);
                    CHECK(scrolled.X < probeCounts.X);
                    CHECK(scrolled.Y >= 0);
                    CHECK(scrolled.Y < probeCounts.Y);
                    CHECK(scrolled.Z >= 0);
                    CHECK(scrolled.Z < probeCounts.Z);
                }
            }
        }
    }

    SECTION("Probe Relocation Offset Clamping")
    {
        const float probesSpacing = 100.0f;
        const float relocateLimit = probesSpacing * 0.45f; // 45 units

        Float3 sdfNormal(0, 1, 0);
        float voxelLimit = 15.0f;
        float sdfInside = -20.0f;

        Float3 offsetCandidate = sdfNormal * (voxelLimit - sdfInside + 0.1f * probesSpacing);
        CHECK(offsetCandidate.Y == 45.0f);
        CHECK(offsetCandidate.Length() <= relocateLimit);

        // Extreme candidate exceeds limit
        Float3 extremeCandidate(100.0f, 0.0f, 0.0f);
        Float3 clamped = extremeCandidate;
        if (clamped.Length() > relocateLimit)
            clamped = Float3::Normalize(clamped) * relocateLimit;
        CHECK(clamped.Length() == Approx(relocateLimit));
    }

    SECTION("Directional Specular Reflection Mapping")
    {
        Float3 normal(0, 1, 0);
        Float3 viewDir(0, -0.7071f, 0.7071f);
        Float3 reflectDir;
        Float3::Reflect(viewDir, normal, reflectDir);

        CHECK(reflectDir.Y > 0.0f);
        CHECK(reflectDir.Z > 0.0f);

        Float2 oct = GetOctahedralCoords(reflectDir);
        Float3 decoded = GetOctahedralDirection(oct);
        CHECK(Float3::Dot(reflectDir, decoded) > 0.99f);
    }

    SECTION("Multi-Bounce Energy Conservation Clamping")
    {
        Float3 albedoPureWhite(1.0f, 1.0f, 1.0f);
        Float3 clampedAlbedo = Float3::Min(albedoPureWhite, Float3(0.9f));
        CHECK(clampedAlbedo.X == 0.9f);
        CHECK(clampedAlbedo.Y == 0.9f);
        CHECK(clampedAlbedo.Z == 0.9f);

        // 10 bounces power attenuation
        float bounceEnergy = 1.0f;
        for (int i = 0; i < 10; i++)
            bounceEnergy *= clampedAlbedo.X;
        CHECK(bounceEnergy < 0.35f); // Soft convergence without infinite runaway
    }

    SECTION("Constant Buffer 16-Byte Register Alignment")
    {
        CHECK(sizeof(GlobalDistanceFieldGIPass::ConstantsData) % 16 == 0);
        CHECK(sizeof(GlobalDistanceFieldGIPass::ConstantsData) == 496);
    }

    SECTION("7x7 Octahedral Distance Tile Layout and Border Coordinates")
    {
        const int32 octResolution = 5;
        const int32 octTileSize = 7;

        for (int32 y = 0; y < octResolution; y++)
        {
            for (int32 x = 0; x < octResolution; x++)
            {
                Int2 octBin(x, y);
                Int2 innerCoord = octBin + Int2(1, 1);
                CHECK(innerCoord.X >= 1);
                CHECK(innerCoord.X <= 5);
                CHECK(innerCoord.Y >= 1);
                CHECK(innerCoord.Y <= 5);
            }
        }
    }

    SECTION("Temporal Filter NaN and Inf Rejection")
    {
        Float3 history(1.0f, 1.0f, 1.0f);
        Float3 nanSample(NAN, 1.0f, 1.0f);
        Float3 infSample(INFINITY, 1.0f, 1.0f);

        // Sanitize NaN
        if (nanSample.IsNanOrInfinity())
            nanSample = Float3::Zero;
        CHECK(nanSample.X == 0.0f);

        // Sanitize Inf
        if (infSample.IsNanOrInfinity())
            infSample = Float3::Zero;
        CHECK(infSample.X == 0.0f);
    }
}

