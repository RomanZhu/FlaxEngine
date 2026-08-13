// Copyright (c) Wojciech Figat. All rights reserved.

#include "Engine/CSG/CSGData.h"
#include "Engine/CSG/CSGMesh.h"
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
