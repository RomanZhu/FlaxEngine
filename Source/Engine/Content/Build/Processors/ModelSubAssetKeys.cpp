// Copyright (c) Wojciech Figat. All rights reserved.

#include "ModelSubAssetKeys.h"

#if COMPILE_WITH_MODEL_TOOL && USE_EDITOR

#include "Engine/Content/Assets/Animation.h"
#include "Engine/Content/Assets/Material.h"
#include "Engine/Content/Assets/Model.h"
#include "Engine/Content/Assets/SkinnedModel.h"
#include <algorithm>

namespace
{
    void HashString(ContentHasher& hasher, const StringView& value)
    {
        const StringAnsi utf8(value);
        const uint32 length = utf8.Length();
        hasher.Update(&length, sizeof(length));
        hasher.Update(utf8.Get(), utf8.Length());
    }

    template<typename T>
    void HashValue(ContentHasher& hasher, const T& value)
    {
        hasher.Update(&value, sizeof(value));
    }

    String Escape(const StringView& value)
    {
        const StringAnsi utf8(String(value).TrimTrailing());
        static const char Digits[] = "0123456789abcdef";
        StringAnsi encoded;
        for (int32 i = 0; i < utf8.Length(); i++)
        {
            const byte c = static_cast<byte>(utf8[i]);
            const bool safe = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == ' ';
            if (safe)
                encoded.Append(static_cast<char>(c));
            else
            {
                encoded.Append('%');
                encoded.Append(Digits[c >> 4]);
                encoded.Append(Digits[c & 15]);
            }
        }
        if (encoded.IsEmpty())
            encoded = "unnamed";
        return String(encoded);
    }

    String Suffix(const ContentHash& hash)
    {
        return String(hash.ToString().Substring(0, 12));
    }

    ContentHash HashMesh(const MeshData& mesh, const ModelData& data)
    {
        ContentHasher hasher;
        static const char Domain[] = "flax-model-mesh-semantic-v1";
        hasher.Update(Domain, ARRAY_COUNT(Domain) - 1);
        HashString(hasher, mesh.Name);
        if (mesh.MaterialSlotIndex >= 0 && mesh.MaterialSlotIndex < data.Materials.Count())
            HashString(hasher, data.Materials[mesh.MaterialSlotIndex].Name);
        const int32 positions = mesh.Positions.Count();
        const int32 indices = mesh.Indices.Count();
        HashValue(hasher, positions);
        HashValue(hasher, indices);
        if (positions)
            hasher.Update(mesh.Positions.Get(), static_cast<uint64>(positions) * sizeof(Float3));
        if (indices)
            hasher.Update(mesh.Indices.Get(), static_cast<uint64>(indices) * sizeof(uint32));
        return hasher.Finalize();
    }

    ContentHash HashAnimation(const AnimationData& animation)
    {
        ContentHasher hasher;
        static const char Domain[] = "flax-model-animation-semantic-v1";
        hasher.Update(Domain, ARRAY_COUNT(Domain) - 1);
        HashValue(hasher, animation.Duration);
        HashValue(hasher, animation.FramesPerSecond);
        Array<String> channels;
        channels.EnsureCapacity(animation.Channels.Count());
        for (const NodeAnimationData& channel : animation.Channels)
            channels.Add(channel.NodeName);
        if (channels.Count() > 1)
            std::sort(channels.Get(), channels.Get() + channels.Count());
        for (const String& channel : channels)
            HashString(hasher, channel);
        return hasher.Finalize();
    }

    ContentHash HashMaterial(const MaterialSlotEntry& material)
    {
        ContentHasher hasher;
        static const char Domain[] = "flax-model-material-semantic-v1";
        hasher.Update(Domain, ARRAY_COUNT(Domain) - 1);
        HashValue(hasher, material.Diffuse.Color);
        HashValue(hasher, material.Emissive.Color);
        HashValue(hasher, material.Opacity.Value);
        HashValue(hasher, material.Roughness.Value);
        HashValue(hasher, material.Metalness.Value);
        HashValue(hasher, material.TwoSided);
        HashValue(hasher, material.Wireframe);
        return hasher.Finalize();
    }

    bool Failure(AssetPipelineDiagnostic& diagnostic, const StringView& message, const StringView& key = StringView::Empty)
    {
        diagnostic = AssetPipelineDiagnostic();
        diagnostic.Code = AssetPipelineDiagnosticCode::SubAssetReconcileRequired;
        diagnostic.Stage = AssetPipelineDiagnosticStage::Prepare;
        diagnostic.ProcessorId = TEXT("Flax.Model");
        diagnostic.Message = message;
        if (!key.IsEmpty())
            diagnostic.Related.Add(key);
        return true;
    }

    void AddInfo(Array<ModelSubAssetInfo>& infos, ModelSubAssetKind kind, const StringView& baseKey, const StringView& displayName,
        const StringView& typeName, const ContentHash& semanticHash, int32 sourceIndex, bool disambiguate)
    {
        ModelSubAssetInfo info;
        info.Kind = kind;
        info.StableKey = disambiguate ? String(baseKey) + TEXT("#") + Suffix(semanticHash) : String(baseKey);
        info.DisplayName = displayName.IsEmpty() ? TEXT("Unnamed") : String(displayName);
        info.TypeName = typeName;
        info.SemanticHash = semanticHash;
        info.SourceIndex = sourceIndex;
        infos.Add(MoveTemp(info));
    }
}

bool ModelSubAssetKeys::Enumerate(const ModelData& data, Array<ModelSubAssetInfo>& infos, Array<SubAssetCandidate>& candidates, AssetPipelineDiagnostic& diagnostic)
{
    infos.Clear();
    candidates.Clear();
    diagnostic = AssetPipelineDiagnostic();

    if (data.LODs.HasItems())
    {
        Dictionary<String, int32> occurrences;
        for (const MeshData* mesh : data.LODs[0].Meshes)
        {
            if (!mesh)
                return Failure(diagnostic, TEXT("Model source contains a null mesh entry."));
            const String baseKey = TEXT("mesh:") + Escape(mesh->Name);
            occurrences[baseKey] = occurrences.ContainsKey(baseKey) ? occurrences[baseKey] + 1 : 1;
        }
        HashSet<String> emittedGroups;
        for (int32 index = 0; index < data.LODs[0].Meshes.Count(); index++)
        {
            const MeshData& mesh = *data.LODs[0].Meshes[index];
            const String baseKey = TEXT("mesh:") + Escape(mesh.Name);
            if (!emittedGroups.Add(baseKey))
                continue;
            ContentHasher groupHasher;
            static const char Domain[] = "flax-model-mesh-group-v1";
            groupHasher.Update(Domain, ARRAY_COUNT(Domain) - 1);
            bool skinned = false;
            Array<ContentHash> meshHashes;
            for (const MeshData* grouped : data.LODs[0].Meshes)
            {
                if (grouped->Name != mesh.Name)
                    continue;
                meshHashes.Add(HashMesh(*grouped, data));
                skinned |= grouped->BlendIndices.HasItems() || grouped->BlendShapes.HasItems();
            }
            if (meshHashes.Count() > 1)
            {
                std::sort(meshHashes.Get(), meshHashes.Get() + meshHashes.Count(), [](const ContentHash& a, const ContentHash& b)
                {
                    return Platform::MemoryCompare(a.Bytes, b.Bytes, sizeof(a.Bytes)) < 0;
                });
            }
            for (const ContentHash& meshHash : meshHashes)
                groupHasher.Update(meshHash.Bytes, sizeof(meshHash.Bytes));
            AddInfo(infos, ModelSubAssetKind::Mesh, baseKey, mesh.Name, skinned ? SkinnedModel::TypeName : Model::TypeName,
                groupHasher.Finalize(), index, false);
        }
    }

    Dictionary<String, int32> animationNames;
    for (const AnimationData& animation : data.Animations)
    {
        const String key = TEXT("animation:") + Escape(animation.Name);
        animationNames[key] = animationNames.ContainsKey(key) ? animationNames[key] + 1 : 1;
    }
    for (int32 index = 0; index < data.Animations.Count(); index++)
    {
        const AnimationData& animation = data.Animations[index];
        const String baseKey = TEXT("animation:") + Escape(animation.Name);
        AddInfo(infos, ModelSubAssetKind::Animation, baseKey, animation.Name, Animation::TypeName,
            HashAnimation(animation), index, animationNames[baseKey] > 1);
    }

    Dictionary<String, int32> materialNames;
    for (const MaterialSlotEntry& material : data.Materials)
    {
        const String key = TEXT("material:") + Escape(material.Name);
        materialNames[key] = materialNames.ContainsKey(key) ? materialNames[key] + 1 : 1;
    }
    for (int32 index = 0; index < data.Materials.Count(); index++)
    {
        const MaterialSlotEntry& material = data.Materials[index];
        const String baseKey = TEXT("material:") + Escape(material.Name);
        AddInfo(infos, ModelSubAssetKind::Material, baseKey, material.Name, Material::TypeName,
            HashMaterial(material), index, materialNames[baseKey] > 1);
    }

    if (infos.Count() > 1)
    {
        std::sort(infos.Get(), infos.Get() + infos.Count(), [](const ModelSubAssetInfo& a, const ModelSubAssetInfo& b)
        {
            return a.StableKey < b.StableKey;
        });
    }
    for (int32 i = 0; i < infos.Count(); i++)
    {
        if (i > 0 && infos[i - 1].StableKey == infos[i].StableKey)
            return Failure(diagnostic, TEXT("Model stable-key derivation produced an ambiguous collision."), infos[i].StableKey);
        SubAssetCandidate candidate;
        candidate.StableKey = infos[i].StableKey;
        candidate.TypeName = infos[i].TypeName;
        candidate.DisplayName = infos[i].DisplayName;
        candidates.Add(MoveTemp(candidate));
    }
    return false;
}

const ModelSubAssetInfo* ModelSubAssetKeys::Find(const Array<ModelSubAssetInfo>& infos, const StringView& stableKey)
{
    for (const ModelSubAssetInfo& info : infos)
    {
        if (info.StableKey == stableKey)
            return &info;
    }
    return nullptr;
}

#endif
