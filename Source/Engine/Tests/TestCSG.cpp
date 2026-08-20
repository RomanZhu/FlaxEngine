// Copyright (c) Wojciech Figat. All rights reserved.

#include "Engine/CSG/CSGData.h"
#include "Engine/CSG/CSGMesh.h"
#include "Engine/CSG/CSGStackEvaluator.h"
#include <ThirdParty/catch2/catch.hpp>

namespace
{
    class TestBoxBrush : public CSG::Brush
    {
    public:
        bool FlipNormals = false;

        Scene* GetBrushScene() const override
        {
            return nullptr;
        }

        Guid GetBrushID() const override
        {
            return Guid::Empty;
        }

        CSG::Mode GetBrushMode() const override
        {
            return CSG::Mode::Additive;
        }

        bool GetBrushFlipNormals() const override
        {
            return FlipNormals;
        }

        void GetSurfaces(Array<CSG::Surface>& surfaces) override
        {
            surfaces.Clear();
            surfaces.Resize(6, false);
            surfaces[0] = CSG::Surface(Vector3::Right, 50.0f);
            surfaces[1] = CSG::Surface(Vector3::Left, 50.0f);
            surfaces[2] = CSG::Surface(Vector3::Up, 50.0f);
            surfaces[3] = CSG::Surface(Vector3::Down, 50.0f);
            surfaces[4] = CSG::Surface(Vector3::Forward, 50.0f);
            surfaces[5] = CSG::Surface(Vector3::Backward, 50.0f);
        }

        int32 GetSurfacesCount() override
        {
            return 6;
        }
    };

    Array<CSG::MeshVertex> TriangulateBox(bool flipNormals)
    {
        TestBoxBrush brush;
        brush.FlipNormals = flipNormals;

        CSG::Mesh mesh;
        mesh.Build(&brush);

        CSG::RawData data;
        Array<CSG::MeshVertex> vertices;
        CHECK_FALSE(mesh.Triangulate(data, vertices));
        return vertices;
    }

    CSG::Operand CreateBoxOperand(const Vector3& center, const Vector3& halfSize, CSG::Mode mode, int32 opIndex = 0)
    {
        CSG::Operand op;
        op.Mode = mode;
        op.OperationIndex = opIndex;
        op.Bounds.Clear();
        op.Bounds.Add(center - halfSize);
        op.Bounds.Add(center + halfSize);
        op.Surfaces.Resize(6, false);
        op.Surfaces[0] = CSG::Surface(Vector3::Right, (Real)(center.X + halfSize.X));
        op.Surfaces[1] = CSG::Surface(Vector3::Left, (Real)(-center.X + halfSize.X));
        op.Surfaces[2] = CSG::Surface(Vector3::Up, (Real)(center.Y + halfSize.Y));
        op.Surfaces[3] = CSG::Surface(Vector3::Down, (Real)(-center.Y + halfSize.Y));
        op.Surfaces[4] = CSG::Surface(Vector3::Forward, (Real)(center.Z + halfSize.Z));
        op.Surfaces[5] = CSG::Surface(Vector3::Backward, (Real)(-center.Z + halfSize.Z));
        return op;
    }
}

TEST_CASE("CSG normal flipping")
{
    const Array<CSG::MeshVertex> outward = TriangulateBox(false);
    const Array<CSG::MeshVertex> inward = TriangulateBox(true);

    REQUIRE(outward.HasItems());
    REQUIRE(outward.Count() == inward.Count());
    REQUIRE(outward.Count() % 3 == 0);
    for (int32 i = 0; i < outward.Count(); i += 3)
    {
        CHECK(Float3::NearEqual(outward[i + 0].Position, inward[i + 2].Position));
        CHECK(Float3::NearEqual(outward[i + 1].Position, inward[i + 1].Position));
        CHECK(Float3::NearEqual(outward[i + 2].Position, inward[i + 0].Position));
        CHECK(Float3::NearEqual(outward[i].Normal, -inward[i].Normal));
    }
}

TEST_CASE("CSG ordered point occupancy evaluator")
{
    // Case A: +Outer (50 half-size)
    const auto outer = CreateBoxOperand(Vector3::Zero, Vector3(50, 50, 50), CSG::Mode::Additive, 0);

    Array<CSG::Operand> ops;
    ops.Add(outer);

    auto state = CSG::CSGStackEvaluator::EvaluatePoint(Vector3::Zero, Span<const CSG::Operand>(ops.Get(), ops.Count()));
    CHECK(state.Solid);
    CHECK(state.LastInfluencingOperation == 0);

    state = CSG::CSGStackEvaluator::EvaluatePoint(Vector3(100, 0, 0), Span<const CSG::Operand>(ops.Get(), ops.Count()));
    CHECK_FALSE(state.Solid);
    CHECK(state.LastInfluencingOperation == -1);

    // Case B: +Outer -Interior (40 half-size)
    const auto interior = CreateBoxOperand(Vector3::Zero, Vector3(40, 40, 40), CSG::Mode::Subtractive, 1);
    ops.Add(interior);

    // Center is now inside interior subtraction -> empty
    state = CSG::CSGStackEvaluator::EvaluatePoint(Vector3::Zero, Span<const CSG::Operand>(ops.Get(), ops.Count()));
    CHECK_FALSE(state.Solid);
    CHECK(state.LastInfluencingOperation == 1);

    // Shell wall is still solid
    state = CSG::CSGStackEvaluator::EvaluatePoint(Vector3(45, 0, 0), Span<const CSG::Operand>(ops.Get(), ops.Count()));
    CHECK(state.Solid);
    CHECK(state.LastInfluencingOperation == 0);

    // Case C: +Outer -Interior +Wall (x: [0, 10], y: [-40, 40], z: [-40, 40])
    const auto wall = CreateBoxOperand(Vector3(5, 0, 0), Vector3(5, 40, 40), CSG::Mode::Additive, 2);
    ops.Add(wall);

    // Point in wall inside cavity is now restored to solid!
    state = CSG::CSGStackEvaluator::EvaluatePoint(Vector3(5, 0, 0), Span<const CSG::Operand>(ops.Get(), ops.Count()));
    CHECK(state.Solid);
    CHECK(state.LastInfluencingOperation == 2);

    // Point in cavity outside wall remains empty
    state = CSG::CSGStackEvaluator::EvaluatePoint(Vector3(-20, 0, 0), Span<const CSG::Operand>(ops.Get(), ops.Count()));
    CHECK_FALSE(state.Solid);
    CHECK(state.LastInfluencingOperation == 1);

    // Case D: +Outer -Interior +Wall -Door (x: [-5, 15], y: [-40, -20], z: [-10, 10])
    const auto door = CreateBoxOperand(Vector3(5, -30, 0), Vector3(10, 10, 10), CSG::Mode::Subtractive, 3);
    ops.Add(door);

    // Point in door opening is now subtracted -> empty
    state = CSG::CSGStackEvaluator::EvaluatePoint(Vector3(5, -30, 0), Span<const CSG::Operand>(ops.Get(), ops.Count()));
    CHECK_FALSE(state.Solid);
    CHECK(state.LastInfluencingOperation == 3);

    // Point in wall above door remains solid
    state = CSG::CSGStackEvaluator::EvaluatePoint(Vector3(5, 0, 0), Span<const CSG::Operand>(ops.Get(), ops.Count()));
    CHECK(state.Solid);
    CHECK(state.LastInfluencingOperation == 2);
}

TEST_CASE("CSG stack evaluation")
{
    // Single box
    {
        Array<CSG::Operand> ops;
        ops.Add(CreateBoxOperand(Vector3::Zero, Vector3(50, 50, 50), CSG::Mode::Additive, 0));

        CSG::Mesh mesh;
        CSG::StackBuildStats stats;
        REQUIRE(CSG::CSGStackEvaluator::EvaluateStack(Span<const CSG::Operand>(ops.Get(), ops.Count()), mesh, &stats));
        CHECK(stats.FinalFragmentCount == 6);

        CSG::RawData data;
        Array<CSG::MeshVertex> vertices;
        CHECK_FALSE(mesh.Triangulate(data, vertices));
        CHECK(vertices.Count() == 36); // 6 faces * 2 triangles * 3 vertices
    }

    // Disjoint boxes
    {
        Array<CSG::Operand> ops;
        ops.Add(CreateBoxOperand(Vector3(-100, 0, 0), Vector3(20, 20, 20), CSG::Mode::Additive, 0));
        ops.Add(CreateBoxOperand(Vector3(100, 0, 0), Vector3(20, 20, 20), CSG::Mode::Additive, 1));

        CSG::Mesh mesh;
        CSG::StackBuildStats stats;
        REQUIRE(CSG::CSGStackEvaluator::EvaluateStack(Span<const CSG::Operand>(ops.Get(), ops.Count()), mesh, &stats));
        CHECK(stats.FinalFragmentCount == 12);
        CHECK(stats.DisjointPairsCount == 2);
        CHECK(stats.OverlappingPairsCount == 0);

        CSG::RawData data;
        Array<CSG::MeshVertex> vertices;
        CHECK_FALSE(mesh.Triangulate(data, vertices));
        CHECK(vertices.Count() == 72);
    }

    // Overlapping additive boxes (union without internal faces)
    {
        Array<CSG::Operand> ops;
        ops.Add(CreateBoxOperand(Vector3(-25, 0, 0), Vector3(50, 50, 50), CSG::Mode::Additive, 0));
        ops.Add(CreateBoxOperand(Vector3(25, 0, 0), Vector3(50, 50, 50), CSG::Mode::Additive, 1));

        CSG::Mesh mesh;
        CSG::StackBuildStats stats;
        REQUIRE(CSG::CSGStackEvaluator::EvaluateStack(Span<const CSG::Operand>(ops.Get(), ops.Count()), mesh, &stats));
        CHECK(stats.DiscardedInternalCount > 0);
        CHECK(stats.OverlappingPairsCount == 2);
        CHECK(stats.DisjointPairsCount == 0);

        CSG::RawData data;
        Array<CSG::MeshVertex> vertices;
        CHECK_FALSE(mesh.Triangulate(data, vertices));
        CHECK(vertices.HasItems());
    }

    // Hollow shell: +Outer -Interior
    {
        Array<CSG::Operand> ops;
        ops.Add(CreateBoxOperand(Vector3::Zero, Vector3(50, 50, 50), CSG::Mode::Additive, 0));
        ops.Add(CreateBoxOperand(Vector3::Zero, Vector3(40, 40, 40), CSG::Mode::Subtractive, 1));

        CSG::Mesh mesh;
        CSG::StackBuildStats stats;
        REQUIRE(CSG::CSGStackEvaluator::EvaluateStack(Span<const CSG::Operand>(ops.Get(), ops.Count()), mesh, &stats));
        CHECK(stats.FinalFragmentCount == 60); // 54 outer quad fragments + 6 inner cavity quads
        CHECK(stats.OverlappingPairsCount == 2);
        CHECK(stats.DisjointPairsCount == 0);

        CSG::RawData data;
        Array<CSG::MeshVertex> vertices;
        CHECK_FALSE(mesh.Triangulate(data, vertices));
        CHECK(vertices.Count() == 360); // 60 quads * 6 vertices
    }

    // Critical: +Outer -Interior +Wall -Door
    {
        Array<CSG::Operand> ops;
        ops.Add(CreateBoxOperand(Vector3::Zero, Vector3(50, 50, 50), CSG::Mode::Additive, 0));
        ops.Add(CreateBoxOperand(Vector3::Zero, Vector3(40, 40, 40), CSG::Mode::Subtractive, 1));
        ops.Add(CreateBoxOperand(Vector3(5, 0, 0), Vector3(5, 40, 40), CSG::Mode::Additive, 2));
        ops.Add(CreateBoxOperand(Vector3(5, -30, 0), Vector3(10, 10, 10), CSG::Mode::Subtractive, 3));

        CSG::Mesh mesh;
        CSG::StackBuildStats stats;
        REQUIRE(CSG::CSGStackEvaluator::EvaluateStack(Span<const CSG::Operand>(ops.Get(), ops.Count()), mesh, &stats));

        CSG::RawData data;
        Array<CSG::MeshVertex> vertices;
        CHECK_FALSE(mesh.Triangulate(data, vertices));
        CHECK(vertices.HasItems());
    }
}


