// Copyright (c) Wojciech Figat. All rights reserved.

#include "LegacyAssetMigrator.h"
#include "Engine/Content/Documents/CanonicalJsonWriter.h"
#include "Engine/Content/Documents/GraphDocument.h"
#include "Engine/Content/Documents/MaterialInstanceDocument.h"
#include "Engine/Content/Documents/SceneAnimationDocument.h"
#include "Engine/Content/Documents/ParticleSystemDocument.h"
#include "Engine/Content/Documents/CollisionDataDocument.h"
#include "Engine/Content/Storage/ContentStorageManager.h"
#include "Engine/Content/Storage/FlaxStorage.h"
#include "Engine/Content/Assets/Material.h"
#include "Engine/Content/Assets/MaterialInstance.h"
#include "Engine/Content/Assets/SkeletonMask.h"
#include "Engine/Content/Assets/Animation.h"
#include "Engine/Animations/SceneAnimations/SceneAnimation.h"
#include "Engine/Engine/GameplayGlobals.h"
#include "Engine/Particles/ParticleSystem.h"
#include "Engine/Physics/CollisionData.h"
#include "Engine/Graphics/Shaders/Cache/ShaderStorage.h"
#include "Engine/Platform/File.h"
#include "Engine/Platform/FileSystem.h"
#include "Engine/Platform/StringUtils.h"
#include "Engine/Serialization/MemoryReadStream.h"
#include "Engine/Serialization/MemoryWriteStream.h"
#include "Engine/Core/Types/Span.h"
#include "Engine/Utilities/Encryption.h"
#include "Engine/Content/Assets/Shader.h"
#if COMPILE_WITH_ASSETS_IMPORTER
#include "Engine/ContentImporters/Types.h"
#endif
#include <algorithm>

namespace
{
    typedef rapidjson_flax::Document JsonDocument;
    typedef rapidjson_flax::Value JsonValue;
    typedef JsonDocument::AllocatorType JsonAlloc;

    bool Fail(AssetPipelineDiagnostic& diagnostic, const StringView& message)
    {
        diagnostic = AssetPipelineDiagnostic();
        diagnostic.Code = AssetPipelineDiagnosticCode::MigrationFailed;
        diagnostic.Stage = AssetPipelineDiagnosticStage::Migration;
        diagnostic.Message = message;
        return true;
    }

    StringAnsi GuidKey(const Guid& id)
    {
        const String wide = id.ToString(Guid::FormatType::N);
        StringAnsi result;
        result.Resize(wide.Length());
        for (int32 i = 0; i < wide.Length(); i++)
        {
            const Char c = wide[i];
            result[i] = (c >= 'A' && c <= 'F') ? static_cast<char>(c - 'A' + 'a') : static_cast<char>(c);
        }
        return result;
    }

    const FlaxChunk* Chunk(const AssetInitData& data, int32 index)
    {
        if (index < 0 || index >= ASSET_FILE_DATA_CHUNKS)
            return nullptr;
        const FlaxChunk* chunk = data.Header.Chunks[index];
        return chunk && chunk->IsLoaded() && chunk->Size() > 0 ? chunk : nullptr;
    }

    bool EnsureDirectory(const StringView& path)
    {
        const String folder(StringUtils::GetDirectoryName(path));
        return folder.HasChars() && !FileSystem::DirectoryExists(folder) && FileSystem::CreateDirectory(folder);
    }

    bool WriteText(const StringView& path, const StringAnsiView& json, AssetPipelineDiagnostic& diagnostic)
    {
        if (EnsureDirectory(path) || File::WriteAllBytes(path, json.Get(), json.Length()))
            return Fail(diagnostic, TEXT("Canonical document could not be written."));
        return false;
    }

    void AddAnsi(JsonValue& object, const char* key, const StringAnsiView& value, JsonAlloc& allocator)
    {
        object.AddMember(JsonValue(key, allocator), JsonValue(value.Get(), value.Length(), allocator), allocator);
    }

    StringAnsi EncodeHex(const void* data, int32 length)
    {
        static const char digits[] = "0123456789abcdef";
        StringAnsi result;
        result.Resize(length * 2);
        const byte* bytes = static_cast<const byte*>(data);
        for (int32 i = 0; i < length; i++)
        {
            result[i * 2] = digits[bytes[i] >> 4];
            result[i * 2 + 1] = digits[bytes[i] & 15];
        }
        return result;
    }

    StringAnsi MaterialPropertiesFromHeader(const AssetInitData& data)
    {
        if (data.CustomData.Length() < static_cast<int32>(sizeof(ShaderStorage::Header20)))
            return StringAnsi();
        const auto* header = reinterpret_cast<const ShaderStorage::Header20*>(data.CustomData.Get());
        JsonDocument json;
        json.SetObject();
        JsonAlloc& allocator = json.GetAllocator();
        json.AddMember("blendMode", JsonValue(static_cast<int32>(header->Material.Info.BlendMode)), allocator);
        json.AddMember("domain", JsonValue(static_cast<int32>(header->Material.Info.Domain)), allocator);
        json.AddMember("maskThreshold", JsonValue(header->Material.Info.MaskThreshold), allocator);
        json.AddMember("opacityThreshold", JsonValue(header->Material.Info.OpacityThreshold), allocator);
        json.AddMember("shadingModel", JsonValue(static_cast<int32>(header->Material.Info.ShadingModel)), allocator);
        StringAnsi output;
        CanonicalJsonError error;
        Array<StringAnsi> order;
        order.Add("blendMode");
        order.Add("domain");
        order.Add("maskThreshold");
        order.Add("opacityThreshold");
        order.Add("shadingModel");
        CanonicalJsonWriter::Write(json, output, error, &order);
        return output;
    }

    StringAnsi VisualScriptPropertiesFromChunk(const FlaxChunk* chunk)
    {
        if (!chunk)
            return StringAnsi();
        MemoryReadStream stream(chunk->Get(), chunk->Size());
        int32 version = 0;
        stream.Read(version);
        String baseType;
        stream.Read(baseType, 31);
        int32 flags = 0;
        stream.Read(flags);
        JsonDocument json;
        json.SetObject();
        JsonAlloc& allocator = json.GetAllocator();
        const StringAnsi type(baseType);
        AddAnsi(json, "baseType", type, allocator);
        json.AddMember("flags", JsonValue(flags), allocator);
        StringAnsi output;
        CanonicalJsonError error;
        Array<StringAnsi> order;
        order.Add("baseType");
        order.Add("flags");
        CanonicalJsonWriter::Write(json, output, error, &order);
        return output;
    }

    bool ConvertGraph(const StringView& destination, const Guid& id, const StringView& typeName, const AssetInitData& data, AssetPipelineDiagnostic& diagnostic)
    {
        const bool material = typeName == Material::TypeName;
        const bool shaderGraph = material || typeName == TEXT("FlaxEngine.ParticleEmitter");
        const FlaxChunk* surface = Chunk(data, shaderGraph ? SHADER_FILE_CHUNK_VISJECT_SURFACE : 0);
        if (!surface)
            return Fail(diagnostic, TEXT("Legacy graph flax has no Visject surface chunk."));
        GraphDocument document;
        if (GraphDocumentCodec::FromSurface(typeName, Span<byte>(surface->Get(), surface->Size()), document, diagnostic))
            return true;
        if (material)
            document.PropertiesJson = MaterialPropertiesFromHeader(data);
        else if (typeName == TEXT("FlaxEngine.VisualScript"))
            document.PropertiesJson = VisualScriptPropertiesFromChunk(Chunk(data, 1));
        StringAnsi json;
        if (GraphDocumentCodec::ToCanonicalJson(document, json, diagnostic))
            return true;
        if (WriteText(destination, json, diagnostic))
            return true;
        return LegacyAssetMigrator::WriteSidecar(destination, id, typeName, AssetSourceKind::TextDocument, TEXT("Flax.GraphDocument"), nullptr, diagnostic);
    }

    bool WriteSimpleDocument(const StringView& destination, const Guid& id, const StringView& typeName, const StringView& processor, JsonDocument& json, const Array<StringAnsi>& order, AssetPipelineDiagnostic& diagnostic)
    {
        StringAnsi output;
        CanonicalJsonError error;
        if (CanonicalJsonWriter::Write(json, output, error, &order))
            return Fail(diagnostic, TEXT("Authored JSON document could not be serialized."));
        if (WriteText(destination, output, diagnostic))
            return true;
        return LegacyAssetMigrator::WriteSidecar(destination, id, typeName, AssetSourceKind::TextDocument, processor, nullptr, diagnostic);
    }

    bool ConvertMaterialInstance(const StringView& destination, const Guid& id, const AssetInitData& data, AssetPipelineDiagnostic& diagnostic)
    {
        const FlaxChunk* chunk = Chunk(data, 0);
        if (!chunk)
            return Fail(diagnostic, TEXT("Material instance flax is missing parameter data."));
        JsonDocument json;
        String error;
        if (MaterialInstanceDocument::DecodeLegacy(Span<byte>(chunk->Get(), chunk->Size()), json, error))
            return Fail(diagnostic, error);
        Array<StringAnsi> order;
        order.Add("documentVersion");
        order.Add("type");
        order.Add("baseMaterial");
        order.Add("overrides");
        return WriteSimpleDocument(destination, id, MaterialInstance::TypeName, TEXT("Flax.MaterialInstance"), json, order, diagnostic);
    }

    bool ConvertSkeletonMask(const StringView& destination, const Guid& id, const AssetInitData& data, AssetPipelineDiagnostic& diagnostic)
    {
        const FlaxChunk* chunk = Chunk(data, 0);
        if (!chunk)
            return Fail(diagnostic, TEXT("Skeleton mask flax is missing node data."));
        MemoryReadStream stream(chunk->Get(), chunk->Size());
        Guid skeletonId;
        stream.Read(skeletonId);
        int32 count = 0;
        stream.ReadInt32(&count);
        JsonDocument json;
        json.SetObject();
        JsonAlloc& allocator = json.GetAllocator();
        json.AddMember("documentVersion", 1, allocator);
        AddAnsi(json, "type", "FlaxEngine.SkeletonMask", allocator);
        AddAnsi(json, "skeleton", GuidKey(skeletonId), allocator);
        JsonValue nodes(rapidjson::kArrayType);
        for (int32 i = 0; i < count; i++)
        {
            String name;
            stream.Read(name, -13);
            const StringAnsi ansi(name);
            nodes.PushBack(JsonValue(ansi.Get(), ansi.Length(), allocator), allocator);
        }
        json.AddMember("maskedNodes", nodes, allocator);
        Array<StringAnsi> order;
        order.Add("documentVersion");
        order.Add("type");
        order.Add("skeleton");
        order.Add("maskedNodes");
        return WriteSimpleDocument(destination, id, SkeletonMask::TypeName, TEXT("Flax.SkeletonMask"), json, order, diagnostic);
    }

    bool ConvertSceneAnimation(const StringView& destination, const Guid& id, const AssetInitData& data, AssetPipelineDiagnostic& diagnostic)
    {
        const FlaxChunk* chunk = Chunk(data, 0);
        if (!chunk)
            return Fail(diagnostic, TEXT("Scene animation flax is missing timeline data."));
        JsonDocument json;
        String error;
        if (SceneAnimationDocument::DecodeLegacy(Span<byte>(chunk->Get(), chunk->Size()), json, error))
            return Fail(diagnostic, error);
        Array<StringAnsi> order;
        order.Add("documentVersion");
        order.Add("type");
        order.Add("framesPerSecond");
        order.Add("durationFrames");
        order.Add("tracks");
        return WriteSimpleDocument(destination, id, SceneAnimation::TypeName, TEXT("Flax.SceneAnimation"), json, order, diagnostic);
    }

    bool ConvertParticleSystem(const StringView& destination, const Guid& id, const AssetInitData& data, AssetPipelineDiagnostic& diagnostic)
    {
        const FlaxChunk* chunk = Chunk(data, 0);
        if (!chunk)
            return Fail(diagnostic, TEXT("Particle system flax is missing timeline data."));
        JsonDocument json;
        String error;
        if (ParticleSystemDocument::DecodeLegacy(Span<byte>(chunk->Get(), chunk->Size()), json, error))
            return Fail(diagnostic, error);
        Array<StringAnsi> order;
        order.Add("documentVersion");
        order.Add("type");
        order.Add("framesPerSecond");
        order.Add("durationFrames");
        order.Add("tracks");
        order.Add("parameterOverrides");
        return WriteSimpleDocument(destination, id, ParticleSystem::TypeName, TEXT("Flax.ParticleSystem"), json, order, diagnostic);
    }

    bool ConvertCollisionData(const StringView& destination, const Guid& id, const AssetInitData& data, AssetPipelineDiagnostic& diagnostic)
    {
        const FlaxChunk* chunk = Chunk(data, 0);
        if (!chunk || chunk->Size() < sizeof(CollisionData::SerializedOptions))
            return Fail(diagnostic, TEXT("Collision data flax is missing its recipe."));
        JsonDocument json;
        String error;
        if (CollisionDataDocument::DecodeLegacy(*reinterpret_cast<const CollisionData::SerializedOptions*>(chunk->Get()), json, error))
            return Fail(diagnostic, error);
        Array<StringAnsi> order;
        order.Add("documentVersion");
        order.Add("type");
        order.Add("collisionType");
        order.Add("sourceModel");
        order.Add("modelLodIndex");
        order.Add("materialSlotsMask");
        order.Add("convexFlags");
        order.Add("convexVertexLimit");
        return WriteSimpleDocument(destination, id, CollisionData::TypeName, TEXT("Flax.CollisionData"), json, order, diagnostic);
    }

    bool ConvertRuntimePayload(const StringView& destination, const Guid& id, const StringView& typeName,
        const StringView& processorId, const AssetInitData& data, const Array<Guid>* references, AssetPipelineDiagnostic& diagnostic)
    {
        const FlaxChunk* chunk = Chunk(data, 0);
        if (!chunk)
            return Fail(diagnostic, TEXT("Authored flax is missing its runtime data chunk."));
        JsonDocument json;
        json.SetObject();
        JsonAlloc& allocator = json.GetAllocator();
        json.AddMember("documentVersion", 1, allocator);
        AddAnsi(json, "type", StringAnsi(typeName), allocator);
        AddAnsi(json, "payloadEncoding", "hex", allocator);
        AddAnsi(json, "runtimeChunk", EncodeHex(chunk->Get(), chunk->Size()), allocator);
        JsonValue referenceValues(rapidjson::kArrayType);
        if (references)
        {
            Array<StringAnsi> keys;
            for (const Guid& reference : *references)
            {
                if (reference.IsValid())
                    keys.Add(GuidKey(reference));
            }
            if (keys.Count() > 1)
            {
                std::sort(keys.Get(), keys.Get() + keys.Count(), [](const StringAnsi& a, const StringAnsi& b) { return a < b; });
                for (int32 i = keys.Count() - 1; i > 0; i--)
                {
                    if (keys[i] == keys[i - 1])
                        keys.RemoveAt(i);
                }
            }
            for (const StringAnsi& key : keys)
                referenceValues.PushBack(JsonValue(key.Get(), key.Length(), allocator), allocator);
        }
        json.AddMember("references", referenceValues, allocator);
        Array<StringAnsi> order;
        order.Add("documentVersion");
        order.Add("type");
        order.Add("payloadEncoding");
        order.Add("runtimeChunk");
        order.Add("references");
        return WriteSimpleDocument(destination, id, typeName, processorId, json, order, diagnostic);
    }

    bool ConvertShader(const StringView& destination, const Guid& id, const AssetInitData& data, AssetPipelineDiagnostic& diagnostic)
    {
        const FlaxChunk* chunk = Chunk(data, SHADER_FILE_CHUNK_SOURCE);
        if (!chunk)
            return Fail(diagnostic, TEXT("Shader flax has no source chunk."));
        Array<byte> bytes;
        bytes.Set(chunk->Get(), chunk->Size());
        if (bytes.IsEmpty())
            return Fail(diagnostic, TEXT("Shader flax source chunk is empty."));
        Encryption::DecryptBytes(bytes.Get(), bytes.Count());
        int32 length = bytes.Count();
        while (length > 0 && bytes[length - 1] == 0)
            length--;
        if (length < 10)
            return Fail(diagnostic, TEXT("Shader flax source is too short."));
        if (WriteText(destination, StringAnsiView(reinterpret_cast<const char*>(bytes.Get()), length), diagnostic))
            return true;
        return LegacyAssetMigrator::WriteSidecar(destination, id, Shader::TypeName, AssetSourceKind::ImportedSource, TEXT("Flax.ShaderSource"), nullptr, diagnostic);
    }
}

bool LegacyAssetMigrator::WriteSidecar(const StringView& documentPath, const Guid& id, const StringView& typeName, AssetSourceKind kind, const StringView& processorId, const Dictionary<String, SubAssetMeta>* subAssets, AssetPipelineDiagnostic& diagnostic)
{
    AssetMeta meta;
    meta.ID = id;
    meta.AssetType = typeName;
    meta.SourceKind = kind;
    meta.Processor.ID = processorId;
    meta.Processor.SettingsVersion = 1;
    meta.Processor.SettingsJson = "{}\n";
    if (subAssets)
        meta.SubAssets = *subAssets;
    if (AssetMeta::SaveAtomic(String(documentPath) + TEXT(".meta"), meta, diagnostic))
        return Fail(diagnostic, TEXT("Migrated sidecar could not be written."));
    diagnostic = AssetPipelineDiagnostic();
    return false;
}

bool LegacyAssetMigrator::SeedModelSubAssets(const StringView& flaxPath, AssetMeta& meta, AssetPipelineDiagnostic& diagnostic)
{
    Array<FlaxStorage::Entry> entries;
    {
        FlaxStorageReference storage = ContentStorageManager::GetStorage(flaxPath, true);
        if (!storage || storage->GetEntriesCount() < 1)
            return Fail(diagnostic, TEXT("Model flax package could not be opened for subasset mapping."));
        entries.Resize(storage->GetEntriesCount());
        for (int32 i = 0; i < entries.Count(); i++)
            storage->GetEntry(i, entries[i]);
    }
    ContentStorageManager::EnsureAccess(flaxPath);
    const FlaxStorage::Entry& root = entries[0];
    meta.ID = root.ID;
    meta.AssetType = root.TypeName;
    meta.SourceKind = AssetSourceKind::ImportedSource;
    if (meta.Processor.ID.IsEmpty())
        meta.Processor.ID = TEXT("Flax.Model");
    if (meta.Processor.SettingsJson.IsEmpty())
        meta.Processor.SettingsJson = "{}\n";
    HashSet<int64> reservedLocalIds;
    reservedLocalIds.Add(1);
    for (int32 i = 1; i < entries.Count(); i++)
    {
        const FlaxStorage::Entry& entry = entries[i];
        String kind = TEXT("subasset");
        if (entry.TypeName.Contains(TEXT("Animation")))
            kind = TEXT("animation");
        else if (entry.TypeName.Contains(TEXT("Material")))
            kind = TEXT("material");
        else if (entry.TypeName.Contains(TEXT("Model")))
            kind = TEXT("mesh");
        SubAssetMeta sub;
        sub.TypeName = entry.TypeName;
        sub.DisplayName = StringUtils::GetFileNameWithoutExtension(flaxPath);
        if (i > 1)
            sub.DisplayName += String::Format(TEXT("-{0}"), i - 1);
        const String key = kind + TEXT(":") + sub.DisplayName;
        sub.LocalId = SubAssetPolicy::AllocateLocalId(meta.Processor.ID, key, sub.TypeName, reservedLocalIds);
        meta.SubAssets.Add(key, sub);
    }
    diagnostic = AssetPipelineDiagnostic();
    return false;
}

bool LegacyAssetMigrator::ConvertFlax(const StringView& sourcePath, const StringView& destinationPath, const Guid& preservedId, const StringView& typeName,
    AssetPipelineDiagnostic& diagnostic, const Array<Guid>* references)
{
    FlaxChunk ownedChunks[ASSET_FILE_DATA_CHUNKS];
    AssetInitData data;
    Guid id;
    String resolvedType;
    {
        FlaxStorageReference storage = ContentStorageManager::GetStorage(sourcePath, true);
        if (!storage || storage->GetEntriesCount() < 1)
            return Fail(diagnostic, TEXT("Legacy flax storage could not be opened."));
        FlaxStorage::Entry entry;
        storage->GetEntry(0, entry);
        if (storage->LoadAssetHeader(0, data))
            return Fail(diagnostic, TEXT("Legacy flax header could not be loaded."));
        for (int32 i = 0; i < ASSET_FILE_DATA_CHUNKS; i++)
        {
            if (data.Header.Chunks[i] && storage->LoadAssetChunk(data.Header.Chunks[i]))
                return Fail(diagnostic, TEXT("Legacy flax chunk could not be loaded."));
        }
        id = preservedId.IsValid() ? preservedId : entry.ID;
        resolvedType = typeName.HasChars() ? String(typeName) : entry.TypeName;
        for (int32 i = 0; i < ASSET_FILE_DATA_CHUNKS; i++)
        {
            FlaxChunk* chunk = data.Header.Chunks[i];
            if (chunk && chunk->IsLoaded() && chunk->Size() > 0)
            {
                ownedChunks[i].Data.Copy(chunk->Get(), chunk->Size());
                data.Header.Chunks[i] = &ownedChunks[i];
            }
            else
            {
                data.Header.Chunks[i] = nullptr;
            }
        }
    }
    ContentStorageManager::EnsureAccess(sourcePath);
    if (GraphDocumentCodec::IsSupportedType(resolvedType))
        return ConvertGraph(destinationPath, id, resolvedType, data, diagnostic);
    if (resolvedType == MaterialInstance::TypeName)
        return ConvertMaterialInstance(destinationPath, id, data, diagnostic);
    if (resolvedType == SkeletonMask::TypeName)
        return ConvertSkeletonMask(destinationPath, id, data, diagnostic);
    if (resolvedType == SceneAnimation::TypeName)
        return ConvertSceneAnimation(destinationPath, id, data, diagnostic);
    if (resolvedType == ParticleSystem::TypeName)
        return ConvertParticleSystem(destinationPath, id, data, diagnostic);
    if (resolvedType == CollisionData::TypeName)
        return ConvertCollisionData(destinationPath, id, data, diagnostic);
    if (resolvedType == Animation::TypeName)
        return ConvertRuntimePayload(destinationPath, id, Animation::TypeName, TEXT("Flax.Animation"), data, references, diagnostic);
    if (resolvedType == GameplayGlobals::TypeName)
        return ConvertRuntimePayload(destinationPath, id, GameplayGlobals::TypeName, TEXT("Flax.GameplayGlobals"), data, references, diagnostic);
    if (resolvedType == Shader::TypeName)
        return ConvertShader(destinationPath, id, data, diagnostic);
    return Fail(diagnostic, TEXT("No flax-to-canonical converter is registered for this type."));
}
