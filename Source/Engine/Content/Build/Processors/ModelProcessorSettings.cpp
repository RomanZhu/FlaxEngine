// Copyright (c) Wojciech Figat. All rights reserved.

#include "ModelProcessorSettings.h"

#if COMPILE_WITH_MODEL_TOOL && USE_EDITOR

#include "Engine/Content/Documents/CanonicalJsonWriter.h"
#include "Engine/Serialization/JsonWriters.h"
#include <cmath>

namespace
{
    typedef rapidjson_flax::Document JsonDocument;
    typedef rapidjson_flax::Value JsonValue;

    bool Fail(AssetPipelineDiagnostic& diagnostic, const StringView& message)
    {
        diagnostic = AssetPipelineDiagnostic();
        diagnostic.Code = AssetPipelineDiagnosticCode::InvalidMeta;
        diagnostic.Stage = AssetPipelineDiagnosticStage::Prepare;
        diagnostic.ProcessorId = ModelProcessorSettings::ProcessorID();
        diagnostic.Message = message;
        return true;
    }

    const char* GetSection(const StringAnsiView& name)
    {
        const char* asset[] = { "Type" };
        const char* geometry[] = {
            "CalculateNormals", "SmoothingNormalsAngle", "FlipNormals", "CalculateTangents", "SmoothingTangentsAngle",
            "ReverseWindingOrder", "OptimizeMeshes", "MergeMeshes", "ImportLODs", "ImportVertexColors", "ImportBlendShapes",
            "CalculateBoneOffsetMatrices", "LightmapUVsSource", "PositionFormat", "TexCoordFormat"
        };
        const char* collision[] = { "CollisionMeshesPrefix", "CollisionMeshesPostfix", "CollisionType" };
        const char* transform[] = { "Scale", "Rotation", "Translation", "UseLocalOrigin", "CenterGeometry", "IgnoreNodesScale" };
        const char* animation[] = {
            "Duration", "FramesRange", "DefaultFrameRate", "SamplingRate", "SkipEmptyCurves", "OptimizeKeyframes",
            "ImportScaleTracks", "RootMotion", "RootMotionFlags", "RootNodeName"
        };
        const char* lod[] = {
            "IgnoredLODs", "GenerateLODs", "BaseLOD", "LODCount", "TriangleReduction", "SloppyOptimization",
            "LODTargetError", "LODTargetErrorAbsolute", "LODLockBorder", "LODPreserveUVs", "LODPreserveUVsWeight"
        };
        const char* materials[] = {
            "ImportMaterials", "CreateEmptyMaterialSlots", "ImportMaterialsAsInstances", "InstanceToImportAs", "ImportTextures",
            "RestoreMaterialsOnReimport", "SkipExistingMaterialsOnReimport"
        };
        const char* sdf[] = { "GenerateSDF", "SDFResolution" };
        const char* splitting[] = { "SplitObjects", "ObjectIndex", "SubAssetFolder" };
        auto contains = [&name](const char* const* values, int32 count)
        {
            for (int32 i = 0; i < count; i++)
            {
                if (name == values[i])
                    return true;
            }
            return false;
        };
        if (contains(asset, ARRAY_COUNT(asset))) return "asset";
        if (contains(geometry, ARRAY_COUNT(geometry))) return "geometry";
        if (contains(collision, ARRAY_COUNT(collision))) return "collision";
        if (contains(transform, ARRAY_COUNT(transform))) return "transform";
        if (contains(animation, ARRAY_COUNT(animation))) return "animation";
        if (contains(lod, ARRAY_COUNT(lod))) return "lod";
        if (contains(materials, ARRAY_COUNT(materials))) return "materials";
        if (contains(sdf, ARRAY_COUNT(sdf))) return "sdf";
        if (contains(splitting, ARRAY_COUNT(splitting))) return "splitting";
        return nullptr;
    }

    bool SerializeFlat(const ModelTool::Options& options, JsonDocument& document, AssetPipelineDiagnostic& diagnostic)
    {
        ModelTool::Options mutableOptions = options;
        rapidjson_flax::StringBuffer buffer;
        CompactJsonWriter writer(buffer);
        writer.StartObject();
        mutableOptions.Serialize(writer, nullptr);
        writer.EndObject();
        document.Parse(buffer.GetString(), buffer.GetSize());
        if (document.HasParseError() || !document.IsObject())
            return Fail(diagnostic, TEXT("Model importer settings could not be serialized."));
        return false;
    }

    bool BuildGroupedDocument(const ModelTool::Options& options, JsonDocument& grouped, AssetPipelineDiagnostic& diagnostic)
    {
        JsonDocument flat;
        if (SerializeFlat(options, flat, diagnostic))
            return true;
        grouped.SetObject();
        auto& allocator = grouped.GetAllocator();
        const char* sections[] = { "asset", "geometry", "collision", "transform", "animation", "lod", "materials", "sdf", "splitting" };
        for (const char* sectionName : sections)
        {
            JsonValue section(rapidjson::kObjectType);
            for (auto member = flat.MemberBegin(); member != flat.MemberEnd(); ++member)
            {
                const StringAnsiView field(member->name.GetString(), member->name.GetStringLength());
                const char* fieldSection = GetSection(field);
                if (!fieldSection)
                    return Fail(diagnostic, String::Format(TEXT("Model importer option '{0}' has no tracked settings group."), String(field)));
                if (StringAnsiView(fieldSection) != sectionName)
                    continue;
                JsonValue key(member->name.GetString(), member->name.GetStringLength(), allocator);
                JsonValue value;
                value.CopyFrom(member->value, allocator);
                section.AddMember(key.Move(), value.Move(), allocator);
            }
            grouped.AddMember(JsonValue(sectionName, allocator).Move(), section.Move(), allocator);
        }
        return false;
    }

    bool ParseGrouped(const StringAnsiView& json, ModelTool::Options& options, AssetPipelineDiagnostic& diagnostic)
    {
        JsonDocument grouped;
        grouped.Parse(json.Get(), json.Length());
        if (grouped.HasParseError())
        {
            diagnostic.Location.Column = static_cast<int32>(grouped.GetErrorOffset());
            return Fail(diagnostic, TEXT("Model settings JSON parsing failed."));
        }
        CanonicalJsonError error;
        if (CanonicalJsonWriter::Validate(grouped, error) || !grouped.IsObject())
            return Fail(diagnostic, TEXT("Model settings must be a canonical JSON object without duplicate keys or non-finite numbers."));

        JsonDocument flat;
        flat.SetObject();
        auto& allocator = flat.GetAllocator();
        for (auto section = grouped.MemberBegin(); section != grouped.MemberEnd(); ++section)
        {
            const StringAnsiView sectionName(section->name.GetString(), section->name.GetStringLength());
            if (!section->value.IsObject())
                return Fail(diagnostic, String::Format(TEXT("Model settings section '{0}' must be an object."), String(sectionName)));
            for (auto member = section->value.MemberBegin(); member != section->value.MemberEnd(); ++member)
            {
                const StringAnsiView field(member->name.GetString(), member->name.GetStringLength());
                const char* expected = GetSection(field);
                if (!expected || sectionName != expected || flat.HasMember(member->name))
                    return Fail(diagnostic, String::Format(TEXT("Model setting '{0}/{1}' is unknown, duplicated, or in the wrong section."), String(sectionName), String(field)));
                JsonValue key(member->name.GetString(), member->name.GetStringLength(), allocator);
                JsonValue value;
                value.CopyFrom(member->value, allocator);
                flat.AddMember(key.Move(), value.Move(), allocator);
            }
        }
        options = ModelTool::Options();
        options.Deserialize(*reinterpret_cast<ISerializable::DeserializeStream*>(&flat), nullptr);
        options.Cached = nullptr;
        options.ImportTypes = ImportDataTypes::None;
        return false;
    }

    bool IsFinite(float value)
    {
        return std::isfinite(value);
    }
}

const String& ModelProcessorSettings::ProcessorID()
{
    static const String value(TEXT("Flax.Model"));
    return value;
}

ModelProcessorSettings ModelProcessorSettings::Defaults()
{
    ModelProcessorSettings result;
    result.Import = ModelTool::Options();
    return result;
}

AssetProcessorSettingsSchema ModelProcessorSettings::Schema()
{
    AssetProcessorSettingsSchema schema;
    schema.ProcessorID = ProcessorID();
    schema.CurrentVersion = CurrentVersion;
    schema.ImplementationVersion = TEXT("model-settings-v1");
    schema.Upgrade = &Upgrade;
    AssetPipelineDiagnostic diagnostic;
    Defaults().ToJson(schema.NormalizedDefaults, diagnostic);
    ASSERT(diagnostic.Code == AssetPipelineDiagnosticCode::None);
    return schema;
}

bool ModelProcessorSettings::Parse(const StringAnsiView& json, int32 version, ModelProcessorSettings& result, AssetPipelineDiagnostic& diagnostic)
{
    result = Defaults();
    diagnostic = AssetPipelineDiagnostic();
    if (version != CurrentVersion)
        return Fail(diagnostic, version < CurrentVersion ? TEXT("Model settings require a tracked schema upgrade.") : TEXT("Model settings use a newer unsupported schema."));
    if (ParseGrouped(json, result.Import, diagnostic))
        return true;
    return result.Validate(diagnostic);
}

bool ModelProcessorSettings::ToJson(StringAnsi& json, AssetPipelineDiagnostic& diagnostic) const
{
    if (Validate(diagnostic))
        return true;
    JsonDocument grouped;
    if (BuildGroupedDocument(Import, grouped, diagnostic))
        return true;
    CanonicalJsonError error;
    if (CanonicalJsonWriter::Write(grouped, json, error))
        return Fail(diagnostic, error.Message);
    return false;
}

bool ModelProcessorSettings::ToSectionJson(const StringAnsiView& section, StringAnsi& json, AssetPipelineDiagnostic& diagnostic) const
{
    JsonDocument grouped;
    if (BuildGroupedDocument(Import, grouped, diagnostic))
        return true;
    const auto member = grouped.FindMember(section.Get());
    if (member == grouped.MemberEnd() || !member->value.IsObject())
        return Fail(diagnostic, String::Format(TEXT("Unknown model settings key section '{0}'."), String(section)));
    CanonicalJsonError error;
    if (CanonicalJsonWriter::Write(member->value, json, error))
        return Fail(diagnostic, error.Message);
    return false;
}

bool ModelProcessorSettings::Validate(AssetPipelineDiagnostic& diagnostic) const
{
    diagnostic = AssetPipelineDiagnostic();
    const ModelTool::Options& value = Import;
    if (static_cast<uint32>(value.Type) > static_cast<uint32>(ModelTool::ModelType::Prefab))
        return Fail(diagnostic, TEXT("Model output type is invalid."));
    if (!IsFinite(value.SmoothingNormalsAngle) || value.SmoothingNormalsAngle < 0.0f || value.SmoothingNormalsAngle > 175.0f ||
        !IsFinite(value.SmoothingTangentsAngle) || value.SmoothingTangentsAngle < 0.0f || value.SmoothingTangentsAngle > 45.0f)
        return Fail(diagnostic, TEXT("Model smoothing angles are outside supported ranges."));
    if (!IsFinite(value.Scale) || value.Scale < 0.0001f || value.Scale > 100000.0f ||
        !IsFinite(value.Rotation.X) || !IsFinite(value.Rotation.Y) || !IsFinite(value.Rotation.Z) || !IsFinite(value.Rotation.W) ||
        !IsFinite(value.Translation.X) || !IsFinite(value.Translation.Y) || !IsFinite(value.Translation.Z))
        return Fail(diagnostic, TEXT("Model transform contains an invalid or non-finite value."));
    if (!IsFinite(value.FramesRange.X) || !IsFinite(value.FramesRange.Y) || value.FramesRange.X < 0.0f || value.FramesRange.Y < value.FramesRange.X ||
        !IsFinite(value.DefaultFrameRate) || value.DefaultFrameRate < 0.0f || value.DefaultFrameRate > 1000.0f ||
        !IsFinite(value.SamplingRate) || value.SamplingRate < 0.0f || value.SamplingRate > 1000.0f)
        return Fail(diagnostic, TEXT("Model animation timing settings are invalid."));
    if (value.BaseLOD < 0 || value.BaseLOD >= MODEL_MAX_LODS || value.LODCount < 1 || value.LODCount > MODEL_MAX_LODS ||
        !IsFinite(value.TriangleReduction) || value.TriangleReduction < 0.0f || value.TriangleReduction > 1.0f ||
        !IsFinite(value.LODTargetError) || value.LODTargetError < 0.0f || value.LODTargetError > 1.0f ||
        !IsFinite(value.LODPreserveUVsWeight) || value.LODPreserveUVsWeight < 0.001f || value.LODPreserveUVsWeight > 1.0f ||
        (static_cast<uint32>(value.IgnoredLODs) & ~0x3fu) != 0)
        return Fail(diagnostic, TEXT("Model LOD settings are invalid."));
    if (!IsFinite(value.SDFResolution) || value.SDFResolution < 0.0001f || value.SDFResolution > 100.0f)
        return Fail(diagnostic, TEXT("Model SDF resolution is outside the supported range."));
    if (value.ObjectIndex < -1)
        return Fail(diagnostic, TEXT("Model object index cannot be less than -1."));
    if (value.Type == ModelTool::ModelType::Prefab)
        return Fail(diagnostic, TEXT("Canonical model processor does not emit prefab authoring output; use the prefab document pipeline."));
    return false;
}

ModelProcessorSettings ModelProcessorSettings::FromLegacyOptions(const ModelTool::Options& options)
{
    ModelProcessorSettings result;
    result.Import = options;
    result.Import.Cached = nullptr;
    result.Import.ImportTypes = ImportDataTypes::None;
    return result;
}

bool ModelProcessorSettings::Upgrade(int32 fromVersion, const StringAnsiView& input, StringAnsi& output, AssetPipelineDiagnostic& diagnostic)
{
    if (fromVersion != CurrentVersion)
        return Fail(diagnostic, TEXT("No implicit model settings migration is available for this schema version."));
    ModelProcessorSettings settings;
    if (Parse(input, CurrentVersion, settings, diagnostic))
        return true;
    return settings.ToJson(output, diagnostic);
}

#endif
