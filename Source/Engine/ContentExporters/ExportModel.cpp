// Copyright (c) Wojciech Figat. All rights reserved.

#include "AssetExporters.h"

#if COMPILE_WITH_ASSETS_EXPORTER

#include "Engine/Content/Assets/Model.h"
#include "Engine/Content/Assets/SkinnedModel.h"
#include "Engine/Core/DeleteMe.h"
#include "Engine/Graphics/Models/MeshAccessor.h"
#include "Engine/Serialization/FileWriteStream.h"
#include "Engine/Serialization/Json.h"
#include "Engine/Serialization/MemoryReadStream.h"
#include "Engine/Serialization/MemoryWriteStream.h"

namespace
{
    struct GltfBufferView
    {
        uint32 Offset;
        uint32 Length;
        uint32 Target;
    };

    struct GltfAccessor
    {
        int32 View;
        uint32 Count;
        uint32 ComponentType;
        const char* Type;
        bool HasBounds = false;
        Float3 Minimum;
        Float3 Maximum;
    };

    struct GltfPrimitive
    {
        int32 Position = -1;
        int32 Normal = -1;
        int32 TexCoord = -1;
        int32 Color = -1;
        int32 Indices = -1;
        int32 Material = -1;
    };

    void Align4(MemoryWriteStream& stream)
    {
        while ((stream.GetPosition() & 3u) != 0)
            stream.WriteByte(0);
    }

    int32 AddView(Array<GltfBufferView>& views, uint32 offset, uint32 length, uint32 target)
    {
        GltfBufferView view = { offset, length, target };
        views.Add(view);
        return views.Count() - 1;
    }

    int32 AddAccessor(Array<GltfAccessor>& accessors, int32 view, uint32 count, uint32 componentType, const char* type)
    {
        GltfAccessor accessor = { view, count, componentType, type };
        accessors.Add(accessor);
        return accessors.Count() - 1;
    }

}

ExportAssetResult AssetExporters::ExportModelGlb(ExportAssetContext& context)
{
        auto* asset = static_cast<ModelBase*>(context.Asset.Get());
        auto lock = asset->Storage->LockSafe();
        const auto path = GET_OUTPUT_PATH(context, "glb");
        constexpr int32 lodIndex = 0;

        const int32 chunkIndex = MODEL_LOD_TO_CHUNK_INDEX(lodIndex);
        if (asset->LoadChunk(chunkIndex))
            return ExportAssetResult::CannotLoadData;
        const auto chunk = asset->GetChunk(chunkIndex);
        if (!chunk || asset->GetLODsCount() <= lodIndex)
            return ExportAssetResult::CannotLoadData;

        MemoryReadStream stream(chunk->Get(), chunk->Size());
        const byte meshVersion = stream.ReadByte();
        Array<MeshBase*> meshes;
        asset->GetMeshes(meshes, lodIndex);
        Array<GltfBufferView> views;
        Array<GltfAccessor> accessors;
        Array<GltfPrimitive> primitives;
        MemoryWriteStream binary(Math::Max<uint32>(chunk->Size(), 4096));
        ModelBase::MeshData meshData;

        for (int32 meshIndex = 0; meshIndex < meshes.Count(); meshIndex++)
        {
            MeshBase* mesh = meshes[meshIndex];
            if (asset->LoadMesh(stream, meshVersion, mesh, &meshData) || meshData.Vertices == 0 || meshData.Triangles == 0)
                return ExportAssetResult::CannotLoadData;
            MeshAccessor source;
            if (source.LoadFromMeshData(&meshData))
                return ExportAssetResult::CannotLoadAsset;

            GltfPrimitive primitive;
            primitive.Material = mesh->GetMaterialSlotIndex();

            const auto positions = source.Position();
            if (!positions.IsValid())
                return ExportAssetResult::CannotLoadData;
            Align4(binary);
            const uint32 positionOffset = binary.GetPosition();
            Float3 minimum(MAX_float), maximum(MIN_float);
            for (uint32 i = 0; i < meshData.Vertices; i++)
            {
                Float3 value = positions.GetFloat3(i) * 0.01f;
                value.Z = -value.Z;
                minimum = Float3::Min(minimum, value);
                maximum = Float3::Max(maximum, value);
                binary.WriteBytes(&value, sizeof(value));
            }
            primitive.Position = AddAccessor(accessors, AddView(views, positionOffset, binary.GetPosition() - positionOffset, 34962), meshData.Vertices, 5126, "VEC3");
            accessors[primitive.Position].HasBounds = true;
            accessors[primitive.Position].Minimum = minimum;
            accessors[primitive.Position].Maximum = maximum;

            const auto normals = source.Normal();
            if (normals.IsValid())
            {
                Align4(binary);
                const uint32 offset = binary.GetPosition();
                for (uint32 i = 0; i < meshData.Vertices; i++)
                {
                    Float3 value = normals.GetFloat3(i);
                    MeshAccessor::UnpackNormal(value);
                    value.Z = -value.Z;
                    binary.WriteBytes(&value, sizeof(value));
                }
                primitive.Normal = AddAccessor(accessors, AddView(views, offset, binary.GetPosition() - offset, 34962), meshData.Vertices, 5126, "VEC3");
            }

            const auto texCoords = source.TexCoord();
            if (texCoords.IsValid())
            {
                Align4(binary);
                const uint32 offset = binary.GetPosition();
                for (uint32 i = 0; i < meshData.Vertices; i++)
                {
                    Float2 value = texCoords.GetFloat2(i);
                    value.Y = 1.0f - value.Y;
                    binary.WriteBytes(&value, sizeof(value));
                }
                primitive.TexCoord = AddAccessor(accessors, AddView(views, offset, binary.GetPosition() - offset, 34962), meshData.Vertices, 5126, "VEC2");
            }

            const auto colors = source.Color();
            if (colors.IsValid())
            {
                Align4(binary);
                const uint32 offset = binary.GetPosition();
                for (uint32 i = 0; i < meshData.Vertices; i++)
                {
                    const Float4 value = colors.GetFloat4(i);
                    binary.WriteBytes(&value, sizeof(value));
                }
                primitive.Color = AddAccessor(accessors, AddView(views, offset, binary.GetPosition() - offset, 34962), meshData.Vertices, 5126, "VEC4");
            }

            Align4(binary);
            const uint32 indexOffset = binary.GetPosition();
            if (meshData.IBStride == sizeof(uint16))
            {
                const auto* indices = static_cast<const uint16*>(meshData.IBData);
                for (uint32 i = 0; i < meshData.Triangles; i++)
                {
                    const uint32 triangle[] = { indices[i * 3], indices[i * 3 + 2], indices[i * 3 + 1] };
                    binary.WriteBytes(triangle, sizeof(triangle));
                }
            }
            else
            {
                const auto* indices = static_cast<const uint32*>(meshData.IBData);
                for (uint32 i = 0; i < meshData.Triangles; i++)
                {
                    const uint32 triangle[] = { indices[i * 3], indices[i * 3 + 2], indices[i * 3 + 1] };
                    binary.WriteBytes(triangle, sizeof(triangle));
                }
            }
            primitive.Indices = AddAccessor(accessors, AddView(views, indexOffset, binary.GetPosition() - indexOffset, 34963), meshData.Triangles * 3, 5125, "SCALAR");
            primitives.Add(primitive);
        }

        rapidjson_flax::StringBuffer json;
        rapidjson_flax::Writer<rapidjson_flax::StringBuffer> writer(json);
        writer.StartObject();
        writer.Key("asset"); writer.StartObject(); writer.Key("generator"); writer.String("Flax Engine source extractor"); writer.Key("version"); writer.String("2.0"); writer.EndObject();
        writer.Key("scene"); writer.Int(0);
        writer.Key("scenes"); writer.StartArray(); writer.StartObject(); writer.Key("nodes"); writer.StartArray(); writer.Int(0); writer.EndArray(); writer.EndObject(); writer.EndArray();
        writer.Key("nodes"); writer.StartArray(); writer.StartObject(); writer.Key("mesh"); writer.Int(0); writer.EndObject(); writer.EndArray();
        writer.Key("meshes"); writer.StartArray(); writer.StartObject();
        writer.Key("name");
        const StringAnsi modelName(StringUtils::GetFileNameWithoutExtension(asset->GetPath()));
        writer.String(modelName.Get(), modelName.Length());
        writer.Key("primitives"); writer.StartArray();
        for (const GltfPrimitive& primitive : primitives)
        {
            writer.StartObject(); writer.Key("attributes"); writer.StartObject();
            writer.Key("POSITION"); writer.Int(primitive.Position);
            if (primitive.Normal >= 0) { writer.Key("NORMAL"); writer.Int(primitive.Normal); }
            if (primitive.TexCoord >= 0) { writer.Key("TEXCOORD_0"); writer.Int(primitive.TexCoord); }
            if (primitive.Color >= 0) { writer.Key("COLOR_0"); writer.Int(primitive.Color); }
            writer.EndObject(); writer.Key("indices"); writer.Int(primitive.Indices);
            if (primitive.Material >= 0 && primitive.Material < asset->MaterialSlots.Count()) { writer.Key("material"); writer.Int(primitive.Material); }
            writer.EndObject();
        }
        writer.EndArray(); writer.EndObject(); writer.EndArray();
        if (asset->MaterialSlots.HasItems())
        {
            writer.Key("materials"); writer.StartArray();
            for (int32 slotIndex = 0; slotIndex < asset->MaterialSlots.Count(); slotIndex++)
            {
                const MaterialSlot& slot = asset->MaterialSlots[slotIndex];
                const StringAnsi name(slot.Name);
                writer.StartObject(); writer.Key("name"); writer.String(name.Get(), name.Length());
                writer.Key("pbrMetallicRoughness"); writer.StartObject();
                writer.Key("baseColorFactor"); writer.StartArray();
                writer.Double(1.0); writer.Double(1.0); writer.Double(1.0); writer.Double(1.0);
                writer.EndArray();
                writer.Key("metallicFactor"); writer.Double(0.0);
                writer.Key("roughnessFactor"); writer.Double(1.0);
                writer.EndObject();
                writer.EndObject();
            }
            writer.EndArray();
        }
        writer.Key("buffers"); writer.StartArray(); writer.StartObject(); writer.Key("byteLength"); writer.Uint(binary.GetPosition()); writer.EndObject(); writer.EndArray();
        writer.Key("bufferViews"); writer.StartArray();
        for (const GltfBufferView& view : views)
        {
            writer.StartObject(); writer.Key("buffer"); writer.Int(0); writer.Key("byteOffset"); writer.Uint(view.Offset); writer.Key("byteLength"); writer.Uint(view.Length); writer.Key("target"); writer.Uint(view.Target); writer.EndObject();
        }
        writer.EndArray();
        writer.Key("accessors"); writer.StartArray();
        for (const GltfAccessor& accessor : accessors)
        {
            writer.StartObject(); writer.Key("bufferView"); writer.Int(accessor.View); writer.Key("componentType"); writer.Uint(accessor.ComponentType); writer.Key("count"); writer.Uint(accessor.Count); writer.Key("type"); writer.String(accessor.Type);
            if (accessor.HasBounds)
            {
                writer.Key("min"); writer.StartArray(); writer.Double(accessor.Minimum.X); writer.Double(accessor.Minimum.Y); writer.Double(accessor.Minimum.Z); writer.EndArray();
                writer.Key("max"); writer.StartArray(); writer.Double(accessor.Maximum.X); writer.Double(accessor.Maximum.Y); writer.Double(accessor.Maximum.Z); writer.EndArray();
            }
            writer.EndObject();
        }
        writer.EndArray(); writer.EndObject();

        const uint32 jsonLength = static_cast<uint32>(json.GetSize());
        const uint32 paddedJsonLength = (jsonLength + 3u) & ~3u;
        const uint32 binaryLength = binary.GetPosition();
        const uint32 paddedBinaryLength = (binaryLength + 3u) & ~3u;
        const uint32 totalLength = 12u + 8u + paddedJsonLength + 8u + paddedBinaryLength;
        FileWriteStream* output = FileWriteStream::Open(path);
        if (!output)
            return ExportAssetResult::Error;
        DeleteMe<FileWriteStream> outputDeleteMe(output);
        output->WriteUint32(0x46546c67);
        output->WriteUint32(2);
        output->WriteUint32(totalLength);
        output->WriteUint32(paddedJsonLength);
        output->WriteUint32(0x4e4f534a);
        output->WriteBytes(json.GetString(), jsonLength);
        for (uint32 i = jsonLength; i < paddedJsonLength; i++) output->WriteByte(' ');
        output->WriteUint32(paddedBinaryLength);
        output->WriteUint32(0x004e4942);
        output->WriteBytes(binary.GetHandle(), binaryLength);
        for (uint32 i = binaryLength; i < paddedBinaryLength; i++) output->WriteByte(0);
        return output->HasError() ? ExportAssetResult::Error : ExportAssetResult::Ok;
}

ExportAssetResult AssetExporters::ExportModel(ExportAssetContext& context)
{
    return ExportModelGlb(context);
}

ExportAssetResult AssetExporters::ExportSkinnedModel(ExportAssetContext& context)
{
    return ExportModelGlb(context);
}

#endif
