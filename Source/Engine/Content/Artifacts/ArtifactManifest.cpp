// Copyright (c) Wojciech Figat. All rights reserved.

#include "ArtifactManifest.h"
#include "Engine/Content/AssetDatabase/AssetPath.h"
#include "Engine/Content/Documents/CanonicalJsonWriter.h"
#include "Engine/Core/Collections/HashSet.h"
#include <algorithm>

namespace
{
    typedef rapidjson_flax::Document JsonDocument;
    typedef rapidjson_flax::Value JsonValue;

    bool Fail(AssetPipelineDiagnostic& diagnostic, const StringView& path, const StringView& message, AssetPipelineDiagnosticCode code = AssetPipelineDiagnosticCode::ArtifactInvalid)
    {
        diagnostic = AssetPipelineDiagnostic();
        diagnostic.Code = code;
        diagnostic.Stage = AssetPipelineDiagnosticStage::Publication;
        diagnostic.SourcePath = path;
        diagnostic.Message = message;
        return true;
    }

    const char* DependencyKindName(AssetDependencyKind kind)
    {
        switch (kind)
        {
        case AssetDependencyKind::ExactSourceFile: return "ExactSourceFile";
        case AssetDependencyKind::Artifact: return "Artifact";
        case AssetDependencyKind::RuntimeReference: return "RuntimeReference";
        case AssetDependencyKind::Toolchain: return "Toolchain";
        case AssetDependencyKind::SourceAsset: return "SourceAsset";
        case AssetDependencyKind::Custom: return "Custom";
        case AssetDependencyKind::Global: return "Global";
        case AssetDependencyKind::Target: return "Target";
        case AssetDependencyKind::ImporterProvider: return "ImporterProvider";
        case AssetDependencyKind::LogicalPath: return "LogicalPath";
        case AssetDependencyKind::Environment: return "Environment";
        default: return "ExactSourceFile";
        }
    }

    bool ParseDependencyKind(const StringAnsiView& text, AssetDependencyKind& kind)
    {
        if (text == "ExactSourceFile" || text == "SourceFile") kind = AssetDependencyKind::ExactSourceFile;
        else if (text == "Artifact" || text == "BuildInput") kind = AssetDependencyKind::Artifact;
        else if (text == "RuntimeReference") kind = AssetDependencyKind::RuntimeReference;
        else if (text == "Toolchain") kind = AssetDependencyKind::Toolchain;
        else if (text == "SourceAsset") kind = AssetDependencyKind::SourceAsset;
        else if (text == "Custom") kind = AssetDependencyKind::Custom;
        else if (text == "Global") kind = AssetDependencyKind::Global;
        else if (text == "Target") kind = AssetDependencyKind::Target;
        else if (text == "ImporterProvider") kind = AssetDependencyKind::ImporterProvider;
        else if (text == "LogicalPath") kind = AssetDependencyKind::LogicalPath;
        else if (text == "Environment") kind = AssetDependencyKind::Environment;
        else return true;
        return false;
    }

    const char* DependencyStateName(AssetDependencyState state)
    {
        switch (state)
        {
        case AssetDependencyState::Present: return "Present";
        case AssetDependencyState::Missing: return "Missing";
        case AssetDependencyState::CurrentArtifact: return "CurrentArtifact";
        case AssetDependencyState::ExactArtifact: return "ExactArtifact";
        default: return "Present";
        }
    }

    bool ParseDependencyState(const StringAnsiView& text, AssetDependencyState& state)
    {
        if (text == "Present") state = AssetDependencyState::Present;
        else if (text == "Missing") state = AssetDependencyState::Missing;
        else if (text == "CurrentArtifact") state = AssetDependencyState::CurrentArtifact;
        else if (text == "ExactArtifact") state = AssetDependencyState::ExactArtifact;
        else return true;
        return false;
    }

    void AddString(JsonValue& object, const char* name, const StringView& value, JsonDocument::AllocatorType& allocator)
    {
        const StringAnsi utf8(value);
        object.AddMember(JsonValue(name, allocator).Move(), JsonValue(utf8.Get(), utf8.Length(), allocator).Move(), allocator);
    }

    void AddAnsi(JsonValue& object, const char* name, const StringAnsiView& value, JsonDocument::AllocatorType& allocator)
    {
        object.AddMember(JsonValue(name, allocator).Move(), JsonValue(value.Get(), value.Length(), allocator).Move(), allocator);
    }

    bool ReadHash(const JsonValue& object, const char* name, ContentHash& hash)
    {
        const auto member = object.FindMember(name);
        return member == object.MemberEnd() || !member->value.IsString() || ContentHash::Parse(StringAnsiView(member->value.GetString(), member->value.GetStringLength()), hash);
    }

    bool ReadOptionalHash(const JsonValue& object, const char* name, ContentHash& hash)
    {
        const auto member = object.FindMember(name);
        if (member == object.MemberEnd() || member->value.IsNull())
            return false;
        return !member->value.IsString() || ContentHash::Parse(StringAnsiView(member->value.GetString(), member->value.GetStringLength()), hash);
    }

    bool ReadString(const JsonValue& object, const char* name, String& value, bool allowEmpty = false)
    {
        const auto member = object.FindMember(name);
        if (member == object.MemberEnd() || !member->value.IsString() || (!allowEmpty && member->value.GetStringLength() == 0))
            return true;
        value = String(StringAnsiView(member->value.GetString(), member->value.GetStringLength()));
        return false;
    }

    bool ReadAnsi(const JsonValue& object, const char* name, StringAnsi& value, bool allowEmpty = false)
    {
        const auto member = object.FindMember(name);
        if (member == object.MemberEnd() || !member->value.IsString() || (!allowEmpty && member->value.GetStringLength() == 0))
            return true;
        value.Set(member->value.GetString(), member->value.GetStringLength());
        return false;
    }
}

bool ArtifactManifest::Validate(const StringView& path, AssetPipelineDiagnostic& diagnostic) const
{
    if ((ManifestVersion < 2 || ManifestVersion > CurrentVersion) || !AssetID.IsValid() || ProcessorID.IsEmpty() || ProcessorImplementationVersion < 1 ||
        InputFingerprint.IsZero() || SourceHash.IsZero() || SettingsHash.IsZero() || BuildID.IsEmpty() || Outputs.IsEmpty() || Objects.IsEmpty())
        return Fail(diagnostic, path, TEXT("Artifact manifest is missing a required identity, version, hash, build, or output field."));
    if (Target.Role != "Editor" && Target.Role != "Runtime")
        return Fail(diagnostic, path, TEXT("Artifact manifest target role must be Editor or Runtime."));

    HashSet<String> dependencyIdentities;
    for (const ArtifactManifestDependency& dependency : Dependencies)
    {
        if (dependency.Identity.IsEmpty())
            return Fail(diagnostic, path, TEXT("Artifact manifest dependency identity is empty."));
        const String identity = String::Format(TEXT("{0}:{1}"), static_cast<int32>(dependency.Kind), dependency.Identity);
        if (dependencyIdentities.Contains(identity))
            return Fail(diagnostic, path, TEXT("Artifact manifest contains a duplicate dependency."));
        dependencyIdentities.Add(identity);
        const bool source = dependency.Kind == AssetDependencyKind::ExactSourceFile || dependency.Kind == AssetDependencyKind::SourceAsset;
        const bool hashed = source || dependency.Kind == AssetDependencyKind::Custom || dependency.Kind == AssetDependencyKind::Global ||
            dependency.Kind == AssetDependencyKind::Target || dependency.Kind == AssetDependencyKind::ImporterProvider ||
            dependency.Kind == AssetDependencyKind::Toolchain || dependency.Kind == AssetDependencyKind::Environment;
        if (dependency.State != AssetDependencyState::Missing && hashed && dependency.Hash.IsZero())
            return Fail(diagnostic, path, TEXT("Artifact manifest dependency lacks an observed content hash."));
        if ((dependency.Kind == AssetDependencyKind::Artifact || dependency.Kind == AssetDependencyKind::RuntimeReference) &&
            !dependency.AssetID.IsValid() && !dependency.ObjectID.IsValid())
            return Fail(diagnostic, path, TEXT("Artifact manifest asset dependency lacks an object identity."));
        if (dependency.Kind == AssetDependencyKind::Artifact && dependency.State != AssetDependencyState::Missing &&
            dependency.ExactArtifact.IsZero() && dependency.InterfaceHash.IsZero())
            return Fail(diagnostic, path, TEXT("Artifact manifest build input lacks an exact artifact or semantic interface hash."));
    }

    for (int32 i = 0; i < ImportReasons.Count(); i++)
    {
        const AssetImportReasonNode& reason = ImportReasons[i];
        if (reason.Code.IsEmpty() || reason.Parent < -1 || reason.Parent >= i)
            return Fail(diagnostic, path, TEXT("Artifact manifest import reason tree is malformed."));
    }

    HashSet<AssetObjectId> objectIds;
    for (const ArtifactManifestObject& object : Objects)
    {
        if (!object.ObjectID.IsValid() || !object.BackingAssetID.IsValid() || object.TypeName.IsEmpty() ||
            object.BackingAssetID != AssetID || objectIds.Contains(object.ObjectID) ||
            (object.IsMainObject && object.ObjectID.LocalId != 1))
            return Fail(diagnostic, path, TEXT("Artifact manifest object inventory contains an invalid or duplicate object."));
        objectIds.Add(object.ObjectID);
    }

    HashSet<StringAnsi> outputKinds;
    HashSet<String> outputPaths;
    for (const ArtifactManifestOutput& output : Outputs)
    {
        String normalized(output.RelativePath);
        normalized.Replace(TEXT('\\'), TEXT('/'));
        if (output.Kind.IsEmpty() || output.FormatVersion < 1 || output.Key.IsZero() || output.Content.IsZero() ||
            output.RelativePath != normalized || output.RelativePath.Contains(TEXT(":")) || output.RelativePath.Contains(TEXT("://")) ||
            !AssetPathPolicy::IsPackageEntryPathValid(PackageEntryPath(output.RelativePath)))
            return Fail(diagnostic, path, TEXT("Artifact manifest output has an invalid kind, version, hash, or Library-relative path."));
        if (outputKinds.Contains(output.Kind) || outputPaths.Contains(output.RelativePath))
            return Fail(diagnostic, path, TEXT("Artifact manifest contains a duplicate output kind or path."));
        outputKinds.Add(output.Kind);
        outputPaths.Add(output.RelativePath);
    }
    diagnostic = AssetPipelineDiagnostic();
    return false;
}

bool ArtifactManifest::Parse(const StringAnsiView& json, const StringView& path, ArtifactManifest& result, AssetPipelineDiagnostic& diagnostic)
{
    result = ArtifactManifest();
    JsonDocument document;
    document.Parse(json.Get(), json.Length());
    if (document.HasParseError())
    {
        diagnostic.Location.Column = static_cast<int32>(document.GetErrorOffset());
        return Fail(diagnostic, path, TEXT("Artifact manifest JSON parsing failed."));
    }
    CanonicalJsonError canonicalError;
    if (CanonicalJsonWriter::Validate(document, canonicalError) || !document.IsObject())
        return Fail(diagnostic, path, canonicalError.Message.IsEmpty() ? TEXT("Artifact manifest root must be an object.") : canonicalError.Message);
    const auto version = document.FindMember("manifestVersion");
    const auto assetGuid = document.FindMember("assetGuid");
    const auto databaseRevision = document.FindMember("databaseRevision");
    const auto processor = document.FindMember("processor");
    const auto target = document.FindMember("target");
    const auto dependencies = document.FindMember("dependencies");
    const auto objects = document.FindMember("objects");
    const auto outputs = document.FindMember("outputs");
    const auto buildId = document.FindMember("buildId");
    if (version == document.MemberEnd() || !version->value.IsInt() || assetGuid == document.MemberEnd() || !assetGuid->value.IsString() ||
        databaseRevision == document.MemberEnd() || !databaseRevision->value.IsUint64() || processor == document.MemberEnd() || !processor->value.IsObject() ||
        target == document.MemberEnd() || !target->value.IsObject() || dependencies == document.MemberEnd() || !dependencies->value.IsArray() ||
        objects == document.MemberEnd() || !objects->value.IsArray() ||
        outputs == document.MemberEnd() || !outputs->value.IsArray() || buildId == document.MemberEnd() || !buildId->value.IsString())
        return Fail(diagnostic, path, TEXT("Artifact manifest is missing a required field or field type."));
    result.ManifestVersion = version->value.GetInt();
    if (Guid::Parse(StringAnsiView(assetGuid->value.GetString(), assetGuid->value.GetStringLength()), result.AssetID))
        return Fail(diagnostic, path, TEXT("Artifact manifest asset GUID is invalid."));
    result.DatabaseRevision = databaseRevision->value.GetUint64();
    if (ReadString(processor->value, "id", result.ProcessorID))
        return Fail(diagnostic, path, TEXT("Artifact manifest processor ID is invalid."));
    const auto implementationVersion = processor->value.FindMember("implementationVersion");
    if (implementationVersion == processor->value.MemberEnd() || !implementationVersion->value.IsUint())
        return Fail(diagnostic, path, TEXT("Artifact manifest processor version is invalid."));
    result.ProcessorImplementationVersion = implementationVersion->value.GetUint();

    const JsonValue& targetObject = target->value;
    if (ReadAnsi(targetObject, "platform", result.Target.Platform, true) || ReadAnsi(targetObject, "architecture", result.Target.Architecture, true) ||
        ReadAnsi(targetObject, "graphics", result.Target.Graphics, true) || ReadAnsi(targetObject, "configuration", result.Target.Configuration, true) ||
        ReadAnsi(targetObject, "quality", result.Target.Quality, true) || ReadAnsi(targetObject, "role", result.Target.Role))
        return Fail(diagnostic, path, TEXT("Artifact manifest target is invalid."));
    ReadAnsi(targetObject, "textureCompression", result.Target.TextureCompression, true);
    ReadAnsi(targetObject, "audioCodec", result.Target.AudioCodec, true);
    ReadAnsi(targetObject, "shaderCompiler", result.Target.ShaderCompiler, true);
    const auto features = targetObject.FindMember("featureFlags");
    if (features != targetObject.MemberEnd())
    {
        if (!features->value.IsArray())
            return Fail(diagnostic, path, TEXT("Artifact manifest target featureFlags is invalid."));
        for (const JsonValue& feature : features->value.GetArray())
        {
            if (!feature.IsString())
                return Fail(diagnostic, path, TEXT("Artifact manifest target feature flag is invalid."));
            result.Target.FeatureFlags.Add(StringAnsi(feature.GetString(), feature.GetStringLength()));
        }
    }
    if (ReadHash(document, "inputFingerprint", result.InputFingerprint.Digest) || ReadHash(document, "sourceHash", result.SourceHash) || ReadHash(document, "settingsHash", result.SettingsHash))
        return Fail(diagnostic, path, TEXT("Artifact manifest root hash is invalid."));

    for (const JsonValue& value : dependencies->value.GetArray())
    {
        if (!value.IsObject())
            return Fail(diagnostic, path, TEXT("Artifact manifest dependency must be an object."));
        ArtifactManifestDependency dependency;
        StringAnsi kind;
        if (ReadAnsi(value, "kind", kind) || ParseDependencyKind(kind, dependency.Kind) || ReadString(value, "identity", dependency.Identity))
            return Fail(diagnostic, path, TEXT("Artifact manifest dependency kind or identity is invalid."));
        const auto dependencyState = value.FindMember("state");
        if (dependencyState != value.MemberEnd())
        {
            if (!dependencyState->value.IsString() || ParseDependencyState(StringAnsiView(dependencyState->value.GetString(), dependencyState->value.GetStringLength()), dependency.State))
                return Fail(diagnostic, path, TEXT("Artifact manifest dependency state is invalid."));
        }
        if (ReadOptionalHash(value, "hash", dependency.Hash) || ReadOptionalHash(value, "metadataHash", dependency.MetadataHash) ||
            ReadOptionalHash(value, "artifactKey", dependency.ExactArtifact.Digest) || ReadOptionalHash(value, "interfaceHash", dependency.InterfaceHash))
            return Fail(diagnostic, path, TEXT("Artifact manifest dependency hash is invalid."));
        const auto dependencyGuid = value.FindMember("assetGuid");
        if (dependencyGuid != value.MemberEnd() && (!dependencyGuid->value.IsString() || Guid::Parse(StringAnsiView(dependencyGuid->value.GetString(), dependencyGuid->value.GetStringLength()), dependency.AssetID)))
            return Fail(diagnostic, path, TEXT("Artifact manifest dependency GUID is invalid."));
        const auto dependencyFileGuid = value.FindMember("fileGuid");
        const auto dependencyLocalId = value.FindMember("localId");
        if (dependencyFileGuid != value.MemberEnd() || dependencyLocalId != value.MemberEnd())
        {
            if (dependencyFileGuid == value.MemberEnd() || !dependencyFileGuid->value.IsString() ||
                dependencyLocalId == value.MemberEnd() || !dependencyLocalId->value.IsInt64() || dependencyLocalId->value.GetInt64() == 0 ||
                Guid::Parse(StringAnsiView(dependencyFileGuid->value.GetString(), dependencyFileGuid->value.GetStringLength()), dependency.ObjectID.Guid))
                return Fail(diagnostic, path, TEXT("Artifact manifest dependency object identity is invalid."));
            dependency.ObjectID.LocalId = dependencyLocalId->value.GetInt64();
        }
        const auto interfaceVersion = value.FindMember("interfaceVersion");
        if (interfaceVersion != value.MemberEnd())
        {
            if (!interfaceVersion->value.IsUint())
                return Fail(diagnostic, path, TEXT("Artifact manifest dependency interface version is invalid."));
            dependency.InterfaceVersion = interfaceVersion->value.GetUint();
        }
        const auto origin = value.FindMember("origin");
        if (origin != value.MemberEnd() && (!origin->value.IsString() || ReadString(value, "origin", dependency.Origin, true)))
            return Fail(diagnostic, path, TEXT("Artifact manifest dependency origin is invalid."));
        if (dependencyState == value.MemberEnd() && dependency.Kind == AssetDependencyKind::Artifact)
            dependency.State = dependency.ExactArtifact.IsZero() ? AssetDependencyState::CurrentArtifact : AssetDependencyState::ExactArtifact;
        result.Dependencies.Add(MoveTemp(dependency));
    }

    for (const JsonValue& value : objects->value.GetArray())
    {
        if (!value.IsObject())
            return Fail(diagnostic, path, TEXT("Artifact manifest object inventory entry must be an object."));
        ArtifactManifestObject object;
        const auto fileGuid = value.FindMember("fileGuid");
        const auto localId = value.FindMember("localId");
        const auto backingGuid = value.FindMember("backingAssetGuid");
        const auto isMain = value.FindMember("isMainObject");
        if (fileGuid == value.MemberEnd() || !fileGuid->value.IsString() ||
            localId == value.MemberEnd() || !localId->value.IsInt64() ||
            backingGuid == value.MemberEnd() || !backingGuid->value.IsString() ||
            isMain == value.MemberEnd() || !isMain->value.IsBool() ||
            ReadString(value, "type", object.TypeName) || ReadString(value, "name", object.Name, true) ||
            ReadString(value, "stableKey", object.StableKey, true) ||
            Guid::Parse(StringAnsiView(fileGuid->value.GetString(), fileGuid->value.GetStringLength()), object.ObjectID.Guid) ||
            Guid::Parse(StringAnsiView(backingGuid->value.GetString(), backingGuid->value.GetStringLength()), object.BackingAssetID))
            return Fail(diagnostic, path, TEXT("Artifact manifest object inventory entry is invalid."));
        object.ObjectID.LocalId = localId->value.GetInt64();
        object.IsMainObject = isMain->value.GetBool();
        result.Objects.Add(MoveTemp(object));
    }

    const auto reasons = document.FindMember("importReasons");
    if (reasons != document.MemberEnd())
    {
        if (!reasons->value.IsArray())
            return Fail(diagnostic, path, TEXT("Artifact manifest importReasons must be an array."));
        for (const JsonValue& value : reasons->value.GetArray())
        {
            AssetImportReasonNode reason;
            if (!value.IsObject())
                return Fail(diagnostic, path, TEXT("Artifact manifest import reason node is invalid."));
            const auto parent = value.FindMember("parent");
            if (parent == value.MemberEnd() || !parent->value.IsInt() || ReadAnsi(value, "code", reason.Code) ||
                ReadString(value, "identity", reason.Identity, true) || ReadAnsi(value, "previousFingerprint", reason.PreviousFingerprint, true) ||
                ReadAnsi(value, "currentFingerprint", reason.CurrentFingerprint, true) || ReadString(value, "explanation", reason.Explanation, true))
                return Fail(diagnostic, path, TEXT("Artifact manifest import reason node is invalid."));
            reason.Parent = parent->value.GetInt();
            result.ImportReasons.Add(MoveTemp(reason));
        }
    }

    for (const JsonValue& value : outputs->value.GetArray())
    {
        if (!value.IsObject())
            return Fail(diagnostic, path, TEXT("Artifact manifest output must be an object."));
        ArtifactManifestOutput output;
        const auto formatVersion = value.FindMember("formatVersion");
        const auto size = value.FindMember("size");
        if (ReadAnsi(value, "kind", output.Kind) || formatVersion == value.MemberEnd() || !formatVersion->value.IsUint() ||
            ReadHash(value, "artifactKey", output.Key.Digest) || ReadString(value, "relativePath", output.RelativePath) ||
            ReadHash(value, "contentHash", output.Content) || size == value.MemberEnd() || !size->value.IsUint64())
            return Fail(diagnostic, path, TEXT("Artifact manifest output is missing a required field or field type."));
        output.FormatVersion = formatVersion->value.GetUint();
        output.Size = size->value.GetUint64();
        const auto compatibility = value.FindMember("compatibility");
        if (compatibility != value.MemberEnd() && (!compatibility->value.IsString() || ReadAnsi(value, "compatibility", output.Compatibility, true)))
            return Fail(diagnostic, path, TEXT("Artifact manifest output compatibility is invalid."));
        result.Outputs.Add(MoveTemp(output));
    }
    result.BuildID = String(StringAnsiView(buildId->value.GetString(), buildId->value.GetStringLength()));
    const auto builtAt = document.FindMember("builtAtUtc");
    if (builtAt != document.MemberEnd() && (!builtAt->value.IsString() || ReadString(document, "builtAtUtc", result.BuiltAtUtc, true)))
        return Fail(diagnostic, path, TEXT("Artifact manifest builtAtUtc is invalid."));
    const auto previous = document.FindMember("previousSuccessfulInputFingerprint");
    if (previous != document.MemberEnd() && !previous->value.IsNull() && (!previous->value.IsString() || ArtifactKey::Parse(StringAnsiView(previous->value.GetString(), previous->value.GetStringLength()), result.PreviousSuccessfulInputFingerprint)))
        return Fail(diagnostic, path, TEXT("Artifact manifest previous successful fingerprint is invalid."));
    const auto components = document.FindMember("keyComponents");
    if (components != document.MemberEnd())
    {
        if (!components->value.IsArray())
            return Fail(diagnostic, path, TEXT("Artifact manifest keyComponents is invalid."));
        for (const JsonValue& value : components->value.GetArray())
        {
            ArtifactKeyComponent component;
            if (!value.IsObject() || ReadAnsi(value, "name", component.Name) || ReadAnsi(value, "type", component.Type) || ReadAnsi(value, "value", component.Value, true))
                return Fail(diagnostic, path, TEXT("Artifact manifest key component is invalid."));
            result.KeyComponents.Add(MoveTemp(component));
        }
    }
    return result.Validate(path, diagnostic);
}

bool ArtifactManifest::ToJson(StringAnsi& json, AssetPipelineDiagnostic& diagnostic) const
{
    json.Clear();
    if (Validate(StringView::Empty, diagnostic))
        return true;
    JsonDocument document;
    document.SetObject();
    auto& allocator = document.GetAllocator();
    document.AddMember("manifestVersion", ManifestVersion, allocator);
    AddString(document, "assetGuid", AssetID.ToString(Guid::FormatType::N).ToLower(), allocator);
    document.AddMember("databaseRevision", DatabaseRevision, allocator);

    JsonValue processor(rapidjson::kObjectType);
    AddString(processor, "id", ProcessorID, allocator);
    processor.AddMember("implementationVersion", ProcessorImplementationVersion, allocator);
    document.AddMember("processor", processor.Move(), allocator);

    JsonValue target(rapidjson::kObjectType);
    AddAnsi(target, "platform", Target.Platform, allocator);
    AddAnsi(target, "architecture", Target.Architecture, allocator);
    AddAnsi(target, "graphics", Target.Graphics, allocator);
    AddAnsi(target, "configuration", Target.Configuration, allocator);
    AddAnsi(target, "quality", Target.Quality, allocator);
    AddAnsi(target, "role", Target.Role, allocator);
    if (Target.TextureCompression.HasChars()) AddAnsi(target, "textureCompression", Target.TextureCompression, allocator);
    if (Target.AudioCodec.HasChars()) AddAnsi(target, "audioCodec", Target.AudioCodec, allocator);
    if (Target.ShaderCompiler.HasChars()) AddAnsi(target, "shaderCompiler", Target.ShaderCompiler, allocator);
    if (Target.FeatureFlags.HasItems())
    {
        Array<StringAnsi> features(Target.FeatureFlags);
        std::sort(features.Get(), features.Get() + features.Count());
        JsonValue values(rapidjson::kArrayType);
        for (const StringAnsi& feature : features)
            values.PushBack(JsonValue(feature.Get(), feature.Length(), allocator).Move(), allocator);
        target.AddMember("featureFlags", values.Move(), allocator);
    }
    document.AddMember("target", target.Move(), allocator);
    AddAnsi(document, "inputFingerprint", InputFingerprint.ToString(), allocator);
    AddAnsi(document, "sourceHash", SourceHash.ToString(), allocator);
    AddAnsi(document, "settingsHash", SettingsHash.ToString(), allocator);

    Array<ArtifactManifestDependency> sortedDependencies(Dependencies);
    std::sort(sortedDependencies.Get(), sortedDependencies.Get() + sortedDependencies.Count(), [](const ArtifactManifestDependency& a, const ArtifactManifestDependency& b)
    {
        if (a.Kind != b.Kind)
            return static_cast<byte>(a.Kind) < static_cast<byte>(b.Kind);
        return a.Identity < b.Identity;
    });
    JsonValue dependencies(rapidjson::kArrayType);
    for (const ArtifactManifestDependency& dependency : sortedDependencies)
    {
        JsonValue value(rapidjson::kObjectType);
        AddAnsi(value, "kind", StringAnsiView(DependencyKindName(dependency.Kind)), allocator);
        AddAnsi(value, "state", StringAnsiView(DependencyStateName(dependency.State)), allocator);
        AddString(value, "identity", dependency.Identity, allocator);
        if (dependency.Hash.IsZero())
            value.AddMember("hash", JsonValue(rapidjson::kNullType).Move(), allocator);
        else
            AddAnsi(value, "hash", dependency.Hash.ToString(), allocator);
        if (!dependency.MetadataHash.IsZero()) AddAnsi(value, "metadataHash", dependency.MetadataHash.ToString(), allocator);
        if (dependency.ObjectID.IsValid())
        {
            AddString(value, "fileGuid", dependency.ObjectID.Guid.ToString(Guid::FormatType::N).ToLower(), allocator);
            value.AddMember("localId", dependency.ObjectID.LocalId, allocator);
        }
        if (dependency.AssetID.IsValid()) AddString(value, "assetGuid", dependency.AssetID.ToString(Guid::FormatType::N).ToLower(), allocator);
        if (!dependency.ExactArtifact.IsZero()) AddAnsi(value, "artifactKey", dependency.ExactArtifact.ToString(), allocator);
        if (!dependency.InterfaceHash.IsZero())
        {
            AddAnsi(value, "interfaceHash", dependency.InterfaceHash.ToString(), allocator);
            value.AddMember("interfaceVersion", dependency.InterfaceVersion, allocator);
        }
        if (!dependency.Origin.IsEmpty()) AddString(value, "origin", dependency.Origin, allocator);
        dependencies.PushBack(value.Move(), allocator);
    }
    document.AddMember("dependencies", dependencies.Move(), allocator);

    Array<ArtifactManifestObject> sortedObjects(Objects);
    std::sort(sortedObjects.Get(), sortedObjects.Get() + sortedObjects.Count(), [](const ArtifactManifestObject& a, const ArtifactManifestObject& b)
    {
        if (a.ObjectID.Guid != b.ObjectID.Guid)
            return a.ObjectID.Guid.ToString(Guid::FormatType::N) < b.ObjectID.Guid.ToString(Guid::FormatType::N);
        return a.ObjectID.LocalId < b.ObjectID.LocalId;
    });
    JsonValue objects(rapidjson::kArrayType);
    for (const ArtifactManifestObject& object : sortedObjects)
    {
        JsonValue value(rapidjson::kObjectType);
        AddString(value, "fileGuid", object.ObjectID.Guid.ToString(Guid::FormatType::N).ToLower(), allocator);
        value.AddMember("localId", object.ObjectID.LocalId, allocator);
        AddString(value, "backingAssetGuid", object.BackingAssetID.ToString(Guid::FormatType::N).ToLower(), allocator);
        AddString(value, "type", object.TypeName, allocator);
        AddString(value, "name", object.Name, allocator);
        AddString(value, "stableKey", object.StableKey, allocator);
        value.AddMember("isMainObject", object.IsMainObject, allocator);
        objects.PushBack(value.Move(), allocator);
    }
    document.AddMember("objects", objects.Move(), allocator);
    if (ImportReasons.HasItems())
    {
        JsonValue reasons(rapidjson::kArrayType);
        for (const AssetImportReasonNode& reason : ImportReasons)
        {
            JsonValue value(rapidjson::kObjectType);
            value.AddMember("parent", reason.Parent, allocator);
            AddAnsi(value, "code", reason.Code, allocator);
            AddString(value, "identity", reason.Identity, allocator);
            AddAnsi(value, "previousFingerprint", reason.PreviousFingerprint, allocator);
            AddAnsi(value, "currentFingerprint", reason.CurrentFingerprint, allocator);
            AddString(value, "explanation", reason.Explanation, allocator);
            reasons.PushBack(value.Move(), allocator);
        }
        document.AddMember("importReasons", reasons.Move(), allocator);
    }

    Array<ArtifactManifestOutput> sortedOutputs(Outputs);
    std::sort(sortedOutputs.Get(), sortedOutputs.Get() + sortedOutputs.Count(), [](const ArtifactManifestOutput& a, const ArtifactManifestOutput& b)
    {
        return a.Kind < b.Kind;
    });
    JsonValue outputs(rapidjson::kArrayType);
    for (const ArtifactManifestOutput& output : sortedOutputs)
    {
        JsonValue value(rapidjson::kObjectType);
        AddAnsi(value, "kind", output.Kind, allocator);
        value.AddMember("formatVersion", output.FormatVersion, allocator);
        AddAnsi(value, "artifactKey", output.Key.ToString(), allocator);
        AddString(value, "relativePath", output.RelativePath, allocator);
        AddAnsi(value, "contentHash", output.Content.ToString(), allocator);
        value.AddMember("size", output.Size, allocator);
        if (output.Compatibility.HasChars()) AddAnsi(value, "compatibility", output.Compatibility, allocator);
        outputs.PushBack(value.Move(), allocator);
    }
    document.AddMember("outputs", outputs.Move(), allocator);
    AddString(document, "buildId", BuildID, allocator);
    if (!BuiltAtUtc.IsEmpty()) AddString(document, "builtAtUtc", BuiltAtUtc, allocator);
    if (PreviousSuccessfulInputFingerprint.IsZero())
        document.AddMember("previousSuccessfulInputFingerprint", JsonValue(rapidjson::kNullType).Move(), allocator);
    else
        AddAnsi(document, "previousSuccessfulInputFingerprint", PreviousSuccessfulInputFingerprint.ToString(), allocator);
    if (KeyComponents.HasItems())
    {
        JsonValue components(rapidjson::kArrayType);
        for (const ArtifactKeyComponent& component : KeyComponents)
        {
            JsonValue value(rapidjson::kObjectType);
            AddAnsi(value, "name", component.Name, allocator);
            AddAnsi(value, "type", component.Type, allocator);
            AddAnsi(value, "value", component.Value, allocator);
            components.PushBack(value.Move(), allocator);
        }
        document.AddMember("keyComponents", components.Move(), allocator);
    }

    Array<StringAnsi> rootOrder;
    rootOrder.Add("manifestVersion");
    rootOrder.Add("assetGuid");
    rootOrder.Add("databaseRevision");
    rootOrder.Add("processor");
    rootOrder.Add("target");
    rootOrder.Add("inputFingerprint");
    rootOrder.Add("sourceHash");
    rootOrder.Add("settingsHash");
    rootOrder.Add("dependencies");
    rootOrder.Add("objects");
    rootOrder.Add("importReasons");
    rootOrder.Add("outputs");
    rootOrder.Add("buildId");
    rootOrder.Add("builtAtUtc");
    rootOrder.Add("previousSuccessfulInputFingerprint");
    rootOrder.Add("keyComponents");
    Dictionary<StringAnsi, Array<StringAnsi>> orders;
    Array<StringAnsi> processorOrder;
    processorOrder.Add("id");
    processorOrder.Add("implementationVersion");
    orders.Add("/processor", processorOrder);
    Array<StringAnsi> targetOrder;
    targetOrder.Add("platform"); targetOrder.Add("architecture"); targetOrder.Add("graphics"); targetOrder.Add("configuration");
    targetOrder.Add("quality"); targetOrder.Add("role"); targetOrder.Add("textureCompression"); targetOrder.Add("audioCodec");
    targetOrder.Add("shaderCompiler"); targetOrder.Add("featureFlags");
    orders.Add("/target", targetOrder);
    Array<StringAnsi> dependencyOrder;
    dependencyOrder.Add("kind"); dependencyOrder.Add("state"); dependencyOrder.Add("identity"); dependencyOrder.Add("hash"); dependencyOrder.Add("metadataHash"); dependencyOrder.Add("assetGuid");
    dependencyOrder.Add("artifactKey"); dependencyOrder.Add("interfaceHash"); dependencyOrder.Add("interfaceVersion"); dependencyOrder.Add("origin");
    orders.Add("/dependencies/*", dependencyOrder);
    Array<StringAnsi> objectOrder;
    objectOrder.Add("fileGuid"); objectOrder.Add("localId"); objectOrder.Add("backingAssetGuid"); objectOrder.Add("type");
    objectOrder.Add("name"); objectOrder.Add("stableKey"); objectOrder.Add("isMainObject");
    orders.Add("/objects/*", objectOrder);
    Array<StringAnsi> reasonOrder;
    reasonOrder.Add("parent"); reasonOrder.Add("code"); reasonOrder.Add("identity"); reasonOrder.Add("previousFingerprint");
    reasonOrder.Add("currentFingerprint"); reasonOrder.Add("explanation");
    orders.Add("/importReasons/*", reasonOrder);
    Array<StringAnsi> outputOrder;
    outputOrder.Add("kind"); outputOrder.Add("formatVersion"); outputOrder.Add("artifactKey"); outputOrder.Add("relativePath");
    outputOrder.Add("contentHash"); outputOrder.Add("size"); outputOrder.Add("compatibility");
    orders.Add("/outputs/*", outputOrder);
    Array<StringAnsi> componentOrder;
    componentOrder.Add("name"); componentOrder.Add("type"); componentOrder.Add("value");
    orders.Add("/keyComponents/*", componentOrder);
    CanonicalJsonError error;
    if (CanonicalJsonWriter::Write(document, json, error, &rootOrder, &orders))
        return Fail(diagnostic, StringView::Empty, error.Message);
    return false;
}
