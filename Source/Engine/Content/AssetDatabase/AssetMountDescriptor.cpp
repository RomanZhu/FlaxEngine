// Copyright (c) Wojciech Figat. All rights reserved.

#include "AssetMountDescriptor.h"
#include "Engine/Content/Documents/CanonicalJsonWriter.h"
#include "Engine/Platform/FileSystem.h"
#include "Engine/Platform/StringUtils.h"

const Char* AssetMountDescriptorCodec::TypeName = TEXT("FlaxEditor.Content.Settings.AssetMountSettings");

namespace
{
    typedef rapidjson_flax::Document JsonDocument;
    typedef rapidjson_flax::Value JsonValue;
    typedef JsonDocument::AllocatorType JsonAlloc;

    bool Fail(AssetPipelineDiagnostic& diagnostic, const StringView& path, const StringView& message)
    {
        diagnostic = AssetPipelineDiagnostic();
        diagnostic.Code = AssetPipelineDiagnosticCode::InvalidSettingsCombination;
        diagnostic.Stage = AssetPipelineDiagnosticStage::Configuration;
        diagnostic.SourcePath = path;
        diagnostic.Message = message;
        return true;
    }

    bool ReadString(const JsonValue& object, const char* name, String& value)
    {
        const auto member = object.FindMember(name);
        if (member == object.MemberEnd() || !member->value.IsString())
            return true;
        value = String(StringAnsiView(member->value.GetString(), member->value.GetStringLength()));
        return false;
    }

    bool ParseKind(const StringView& text, AssetMountKind& kind)
    {
        if (text == TEXT("EngineContent"))
            kind = AssetMountKind::EngineContent;
        else if (text == TEXT("PluginContent"))
            kind = AssetMountKind::PluginContent;
        else if (text == TEXT("ExternalReadOnlyContent"))
            kind = AssetMountKind::ExternalReadOnlyContent;
        else
            return true;
        return false;
    }

    const Char* KindName(AssetMountKind kind)
    {
        switch (kind)
        {
        case AssetMountKind::EngineContent: return TEXT("EngineContent");
        case AssetMountKind::PluginContent: return TEXT("PluginContent");
        case AssetMountKind::ExternalReadOnlyContent: return TEXT("ExternalReadOnlyContent");
        default: return TEXT("ProjectContent");
        }
    }

    void AddString(JsonValue& object, const char* name, const StringView& value, JsonAlloc& allocator)
    {
        const StringAnsi ansi(value);
        object.AddMember(JsonValue(name, allocator), JsonValue(ansi.Get(), ansi.Length(), allocator), allocator);
    }
}

bool AssetMountDescriptorCodec::ResolveRoot(const StringView& expression, const StringView& projectRoot, const StringView& engineRoot,
    String& physicalRoot, AssetPipelineDiagnostic& diagnostic)
{
    physicalRoot = String::Empty;
    if (expression.IsEmpty() || expression.Contains(TEXT("\\")))
        return Fail(diagnostic, expression, TEXT("Mount root expression is empty or uses non-portable separators."));
    if (expression == TEXT("$(ProjectPath)"))
        physicalRoot = projectRoot;
    else if (expression.StartsWith(TEXT("$(ProjectPath)/")))
        physicalRoot = String(projectRoot) / expression.Substring(15);
    else if (expression == TEXT("$(EnginePath)"))
        physicalRoot = engineRoot;
    else if (expression.StartsWith(TEXT("$(EnginePath)/")))
        physicalRoot = String(engineRoot) / expression.Substring(14);
    else
        return Fail(diagnostic, expression, TEXT("Tracked mount roots must use $(ProjectPath) or $(EnginePath), not absolute machine paths."));
    StringUtils::PathRemoveRelativeParts(physicalRoot);
    if (!FileSystem::DirectoryExists(physicalRoot))
        return Fail(diagnostic, physicalRoot, TEXT("Declared content mount root does not exist."));
    diagnostic = AssetPipelineDiagnostic();
    return false;
}

bool AssetMountDescriptorCodec::Parse(const StringAnsiView& source, const StringView& sourcePath, const StringView& projectRoot,
    const StringView& engineRoot, Array<AssetMount>& mounts, AssetPipelineDiagnostic& diagnostic)
{
    mounts.Clear();
    StringAnsi canonical;
    CanonicalJsonError canonicalError;
    if (CanonicalJsonWriter::Canonicalize(source, canonical, canonicalError))
        return Fail(diagnostic, sourcePath, TEXT("Content mount settings are not valid deterministic JSON."));
    JsonDocument document;
    document.Parse(canonical.Get(), canonical.Length());
    if (document.HasParseError() || !document.IsObject())
        return Fail(diagnostic, sourcePath, TEXT("Content mount settings root must be a JSON object."));
    String typeName;
    if (ReadString(document, "TypeName", typeName) || typeName != TypeName)
        return Fail(diagnostic, sourcePath, TEXT("Content mount settings have the wrong TypeName."));
    const auto data = document.FindMember("Data");
    if (data == document.MemberEnd() || !data->value.IsObject())
        return Fail(diagnostic, sourcePath, TEXT("Content mount settings are missing the Data object."));
    const auto items = data->value.FindMember("Mounts");
    if (items == data->value.MemberEnd() || !items->value.IsArray())
        return Fail(diagnostic, sourcePath, TEXT("Content mount settings are missing the Mounts array."));

    AssetMountTable validation;
    for (const JsonValue& value : items->value.GetArray())
    {
        if (!value.IsObject())
            return Fail(diagnostic, sourcePath, TEXT("Content mount descriptor must be an object."));
        String idText;
        String kindText;
        String rootExpression;
        AssetMount mount;
        if (ReadString(value, "MountId", idText) || Guid::Parse(idText, mount.MountId) || !mount.MountId.IsValid() ||
            ReadString(value, "LogicalPrefix", mount.LogicalPrefix) || ReadString(value, "Root", rootExpression) ||
            ReadString(value, "Kind", kindText) || ParseKind(kindText, mount.Kind))
            return Fail(diagnostic, sourcePath, TEXT("Content mount descriptor has an invalid ID, prefix, root, or kind."));
        const auto allowLinkedRoot = value.FindMember("AllowLinkedRoot");
        if (allowLinkedRoot != value.MemberEnd())
        {
            if (!allowLinkedRoot->value.IsBool())
                return Fail(diagnostic, sourcePath, TEXT("AllowLinkedRoot must be a boolean."));
            mount.AllowLinkedRoot = allowLinkedRoot->value.GetBool();
        }
        mount.Writable = false;
        if (ResolveRoot(rootExpression, projectRoot, engineRoot, mount.PhysicalRoot, diagnostic) || validation.Register(mount, diagnostic))
            return true;
        mounts.Add(MoveTemp(mount));
    }
    diagnostic = AssetPipelineDiagnostic();
    return false;
}

bool AssetMountDescriptorCodec::Write(const Guid& sourceGuid, const Array<AssetMountSourceDescriptor>& descriptors, StringAnsi& source,
    AssetPipelineDiagnostic& diagnostic)
{
    if (!sourceGuid.IsValid())
        return Fail(diagnostic, StringView::Empty, TEXT("Content mount settings require a valid source GUID."));
    JsonDocument document;
    document.SetObject();
    JsonAlloc& allocator = document.GetAllocator();
    AddString(document, "ID", sourceGuid.ToString(Guid::FormatType::N), allocator);
    AddString(document, "TypeName", TypeName, allocator);
    JsonValue data(rapidjson::kObjectType);
    JsonValue mounts(rapidjson::kArrayType);
    for (const AssetMountSourceDescriptor& descriptor : descriptors)
    {
        if (!descriptor.MountId.IsValid() || descriptor.Kind == AssetMountKind::ProjectContent)
            return Fail(diagnostic, StringView::Empty, TEXT("Content mount descriptor is invalid or attempts to redeclare the project mount."));
        JsonValue value(rapidjson::kObjectType);
        AddString(value, "MountId", descriptor.MountId.ToString(Guid::FormatType::N), allocator);
        AddString(value, "LogicalPrefix", descriptor.LogicalPrefix, allocator);
        AddString(value, "Root", descriptor.Root, allocator);
        AddString(value, "Kind", KindName(descriptor.Kind), allocator);
        value.AddMember("AllowLinkedRoot", descriptor.AllowLinkedRoot, allocator);
        mounts.PushBack(value, allocator);
    }
    data.AddMember("Mounts", mounts, allocator);
    document.AddMember("Data", data, allocator);
    Array<StringAnsi> rootOrder;
    rootOrder.Add("ID");
    rootOrder.Add("TypeName");
    rootOrder.Add("Data");
    Dictionary<StringAnsi, Array<StringAnsi>> fieldOrders;
    Array<StringAnsi> mountOrder;
    mountOrder.Add("MountId");
    mountOrder.Add("LogicalPrefix");
    mountOrder.Add("Root");
    mountOrder.Add("Kind");
    mountOrder.Add("AllowLinkedRoot");
    fieldOrders.Add("Data/Mounts/*", mountOrder);
    CanonicalJsonError error;
    if (CanonicalJsonWriter::Write(document, source, error, &rootOrder, &fieldOrders))
        return Fail(diagnostic, StringView::Empty, TEXT("Content mount settings could not be serialized deterministically."));
    diagnostic = AssetPipelineDiagnostic();
    return false;
}
