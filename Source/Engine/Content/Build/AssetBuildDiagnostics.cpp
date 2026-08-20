// Copyright (c) Wojciech Figat. All rights reserved.

#include "AssetBuildDiagnostics.h"
#include "Engine/Content/Documents/CanonicalJsonWriter.h"

namespace
{
    typedef rapidjson_flax::Document JsonDocument;
    typedef rapidjson_flax::Value JsonValue;

    bool DiagnosticFail(AssetPipelineDiagnostic& error, const StringView& message)
    {
        error = AssetPipelineDiagnostic();
        error.Code = AssetPipelineDiagnosticCode::ArtifactInvalid;
        error.Stage = AssetPipelineDiagnosticStage::Build;
        error.Message = message;
        return true;
    }

    const char* SeverityName(AssetPipelineDiagnosticSeverity severity)
    {
        switch (severity)
        {
        case AssetPipelineDiagnosticSeverity::Info: return "Info";
        case AssetPipelineDiagnosticSeverity::Warning: return "Warning";
        default: return "Error";
        }
    }

    const char* StageName(AssetPipelineDiagnosticStage stage)
    {
        switch (stage)
        {
        case AssetPipelineDiagnosticStage::Configuration: return "Configuration";
        case AssetPipelineDiagnosticStage::DatabaseScan: return "DatabaseScan";
        case AssetPipelineDiagnosticStage::Prepare: return "Prepare";
        case AssetPipelineDiagnosticStage::Build: return "Build";
        case AssetPipelineDiagnosticStage::Publication: return "Publication";
        case AssetPipelineDiagnosticStage::Resolution: return "Resolution";
        case AssetPipelineDiagnosticStage::Cook: return "Cook";
        case AssetPipelineDiagnosticStage::Migration: return "Migration";
        default: return "Configuration";
        }
    }

    void AddStringOrNull(JsonValue& object, const char* name, const StringView& value, JsonDocument::AllocatorType& allocator)
    {
        if (value.IsEmpty())
            object.AddMember(JsonValue(name, allocator).Move(), JsonValue(rapidjson::kNullType).Move(), allocator);
        else
        {
            const StringAnsi utf8(value);
            object.AddMember(JsonValue(name, allocator).Move(), JsonValue(utf8.Get(), utf8.Length(), allocator).Move(), allocator);
        }
    }

    void AddAnsi(JsonValue& object, const char* name, const StringAnsiView& value, JsonDocument::AllocatorType& allocator)
    {
        object.AddMember(JsonValue(name, allocator).Move(), JsonValue(value.Get(), value.Length(), allocator).Move(), allocator);
    }

    bool ReadOptionalString(const JsonValue& object, const char* name, String& value)
    {
        const auto member = object.FindMember(name);
        if (member == object.MemberEnd())
            return true;
        if (member->value.IsNull())
        {
            value.Clear();
            return false;
        }
        if (!member->value.IsString())
            return true;
        value = String(StringAnsiView(member->value.GetString(), member->value.GetStringLength()));
        return false;
    }
}

bool AssetBuildDiagnostics::DiagnosticToJson(const AssetPipelineDiagnostic& diagnostic, StringAnsi& json, AssetPipelineDiagnostic& error)
{
    JsonDocument document(rapidjson::kObjectType);
    auto& allocator = document.GetAllocator();
    document.AddMember("schemaVersion", diagnostic.SchemaVersion, allocator);
    AddStringOrNull(document, "code", GetAssetPipelineDiagnosticCodeName(diagnostic.Code), allocator);
    AddAnsi(document, "severity", StringAnsiView(SeverityName(diagnostic.Severity)), allocator);
    AddAnsi(document, "stage", StringAnsiView(StageName(diagnostic.Stage)), allocator);
    AddStringOrNull(document, "assetGuid", diagnostic.AssetGuid.IsValid() ? diagnostic.AssetGuid.ToString(Guid::FormatType::N) : String(), allocator);
    AddStringOrNull(document, "sourcePath", diagnostic.SourcePath, allocator);
    AddStringOrNull(document, "processorId", diagnostic.ProcessorId, allocator);
    AddStringOrNull(document, "target", diagnostic.Target, allocator);
    AddStringOrNull(document, "outputKind", diagnostic.OutputKind, allocator);
    JsonValue location(rapidjson::kObjectType);
    AddStringOrNull(location, "file", diagnostic.Location.File, allocator);
    location.AddMember("line", diagnostic.Location.Line, allocator);
    location.AddMember("column", diagnostic.Location.Column, allocator);
    AddStringOrNull(location, "graphNode", diagnostic.Location.GraphNode, allocator);
    AddStringOrNull(location, "graphPin", diagnostic.Location.GraphPin, allocator);
    document.AddMember("location", location.Move(), allocator);
    AddStringOrNull(document, "message", diagnostic.Message, allocator);
    AddStringOrNull(document, "remediation", diagnostic.Remediation, allocator);
    JsonValue related(rapidjson::kArrayType);
    for (const String& value : diagnostic.Related)
    {
        const StringAnsi utf8(value);
        related.PushBack(JsonValue(utf8.Get(), utf8.Length(), allocator).Move(), allocator);
    }
    document.AddMember("related", related.Move(), allocator);
    Array<StringAnsi> order;
    order.Add("schemaVersion"); order.Add("code"); order.Add("severity"); order.Add("stage"); order.Add("assetGuid");
    order.Add("sourcePath"); order.Add("processorId"); order.Add("target"); order.Add("outputKind"); order.Add("location");
    order.Add("message"); order.Add("remediation"); order.Add("related");
    Dictionary<StringAnsi, Array<StringAnsi>> orders;
    Array<StringAnsi> locationOrder;
    locationOrder.Add("file"); locationOrder.Add("line"); locationOrder.Add("column"); locationOrder.Add("graphNode"); locationOrder.Add("graphPin");
    orders.Add("/location", locationOrder);
    CanonicalJsonError canonicalError;
    if (CanonicalJsonWriter::Write(document, json, canonicalError, &order, &orders))
        return DiagnosticFail(error, canonicalError.Message);
    error = AssetPipelineDiagnostic();
    return false;
}

bool AssetBuildDiagnostics::DiagnosticFromJson(const StringAnsiView& json, AssetPipelineDiagnostic& diagnostic, AssetPipelineDiagnostic& error)
{
    diagnostic = AssetPipelineDiagnostic();
    JsonDocument document;
    document.Parse(json.Get(), json.Length());
    CanonicalJsonError canonicalError;
    if (document.HasParseError() || !document.IsObject() || CanonicalJsonWriter::Validate(document, canonicalError) || document.MemberCount() != 13)
        return DiagnosticFail(error, TEXT("Structured asset diagnostic JSON is malformed or non-canonical."));
    const auto schema = document.FindMember("schemaVersion");
    const auto code = document.FindMember("code");
    const auto severity = document.FindMember("severity");
    const auto stage = document.FindMember("stage");
    const auto asset = document.FindMember("assetGuid");
    const auto location = document.FindMember("location");
    const auto related = document.FindMember("related");
    if (schema == document.MemberEnd() || !schema->value.IsInt() || code == document.MemberEnd() || !code->value.IsString() ||
        severity == document.MemberEnd() || !severity->value.IsString() || stage == document.MemberEnd() || !stage->value.IsString() ||
        asset == document.MemberEnd() || (!asset->value.IsNull() && !asset->value.IsString()) || location == document.MemberEnd() || !location->value.IsObject() ||
        related == document.MemberEnd() || !related->value.IsArray() || location->value.MemberCount() != 5)
        return DiagnosticFail(error, TEXT("Structured asset diagnostic is missing a required field or field type."));
    diagnostic.SchemaVersion = schema->value.GetInt();
    const String codeText(StringAnsiView(code->value.GetString(), code->value.GetStringLength()));
    bool foundCode = false;
    for (int32 i = 0; i <= static_cast<int32>(AssetPipelineDiagnosticCode::ArtifactIncompatible); i++)
    {
        const auto candidate = static_cast<AssetPipelineDiagnosticCode>(i);
        if (codeText == GetAssetPipelineDiagnosticCodeName(candidate))
        {
            diagnostic.Code = candidate;
            foundCode = true;
            break;
        }
    }
    const StringAnsiView severityText(severity->value.GetString(), severity->value.GetStringLength());
    if (severityText == "Info") diagnostic.Severity = AssetPipelineDiagnosticSeverity::Info;
    else if (severityText == "Warning") diagnostic.Severity = AssetPipelineDiagnosticSeverity::Warning;
    else if (severityText == "Error") diagnostic.Severity = AssetPipelineDiagnosticSeverity::Error;
    else return DiagnosticFail(error, TEXT("Structured asset diagnostic severity is invalid."));
    const StringAnsiView stageText(stage->value.GetString(), stage->value.GetStringLength());
    bool foundStage = false;
    for (int32 i = 0; i <= static_cast<int32>(AssetPipelineDiagnosticStage::Migration); i++)
    {
        const auto candidate = static_cast<AssetPipelineDiagnosticStage>(i);
        if (stageText == StageName(candidate))
        {
            diagnostic.Stage = candidate;
            foundStage = true;
            break;
        }
    }
    if (!foundCode || !foundStage || diagnostic.SchemaVersion != 1)
        return DiagnosticFail(error, TEXT("Structured asset diagnostic code, stage, or schema is unsupported."));
    if (asset->value.IsString() && Guid::Parse(StringAnsiView(asset->value.GetString(), asset->value.GetStringLength()), diagnostic.AssetGuid))
        return DiagnosticFail(error, TEXT("Structured asset diagnostic GUID is invalid."));
    if (ReadOptionalString(document, "sourcePath", diagnostic.SourcePath) || ReadOptionalString(document, "processorId", diagnostic.ProcessorId) ||
        ReadOptionalString(document, "target", diagnostic.Target) || ReadOptionalString(document, "outputKind", diagnostic.OutputKind) ||
        ReadOptionalString(document, "message", diagnostic.Message) || ReadOptionalString(document, "remediation", diagnostic.Remediation))
        return DiagnosticFail(error, TEXT("Structured asset diagnostic contains an invalid optional string."));
    if (ReadOptionalString(location->value, "file", diagnostic.Location.File) || ReadOptionalString(location->value, "graphNode", diagnostic.Location.GraphNode) ||
        ReadOptionalString(location->value, "graphPin", diagnostic.Location.GraphPin))
        return DiagnosticFail(error, TEXT("Structured asset diagnostic location is invalid."));
    const auto line = location->value.FindMember("line");
    const auto column = location->value.FindMember("column");
    if (line == location->value.MemberEnd() || !line->value.IsInt() || column == location->value.MemberEnd() || !column->value.IsInt())
        return DiagnosticFail(error, TEXT("Structured asset diagnostic line or column is invalid."));
    diagnostic.Location.Line = line->value.GetInt();
    diagnostic.Location.Column = column->value.GetInt();
    for (const JsonValue& value : related->value.GetArray())
    {
        if (!value.IsString())
            return DiagnosticFail(error, TEXT("Structured asset diagnostic related entry is invalid."));
        diagnostic.Related.Add(String(StringAnsiView(value.GetString(), value.GetStringLength())));
    }
    error = AssetPipelineDiagnostic();
    return false;
}

void AssetBuildDiagnostics::DiffKeyComponents(const Array<ArtifactKeyComponent>& previous, const Array<ArtifactKeyComponent>& current,
    Array<AssetKeyDifference>& differences)
{
    differences.Clear();
    for (const ArtifactKeyComponent& oldComponent : previous)
    {
        const ArtifactKeyComponent* matching = nullptr;
        for (const ArtifactKeyComponent& candidate : current)
        {
            if (candidate.Name == oldComponent.Name && candidate.Type == oldComponent.Type)
            {
                matching = &candidate;
                break;
            }
        }
        if (!matching || matching->Value != oldComponent.Value)
        {
            AssetKeyDifference difference;
            difference.Name = oldComponent.Name;
            difference.Type = oldComponent.Type;
            difference.PreviousValue = RedactAbsolutePath(oldComponent.Value);
            if (matching)
                difference.CurrentValue = RedactAbsolutePath(matching->Value);
            differences.Add(MoveTemp(difference));
        }
    }
    for (const ArtifactKeyComponent& newComponent : current)
    {
        bool found = false;
        for (const ArtifactKeyComponent& candidate : previous)
            found |= candidate.Name == newComponent.Name && candidate.Type == newComponent.Type;
        if (!found)
        {
            AssetKeyDifference difference;
            difference.Name = newComponent.Name;
            difference.Type = newComponent.Type;
            difference.CurrentValue = RedactAbsolutePath(newComponent.Value);
            differences.Add(MoveTemp(difference));
        }
    }
}

StringAnsi AssetBuildDiagnostics::RedactAbsolutePath(const StringAnsiView& value)
{
    if ((value.Length() >= 3 && value[1] == ':' && (value[2] == '\\' || value[2] == '/')) ||
        (value.Length() >= 2 && value[0] == '/' && value[1] != '/') || (value.Length() >= 2 && value[0] == '\\' && value[1] == '\\'))
        return StringAnsi("<absolute-path-redacted>");
    return StringAnsi(value);
}
