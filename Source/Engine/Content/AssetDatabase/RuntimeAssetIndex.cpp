// Copyright (c) Wojciech Figat. All rights reserved.

#include "RuntimeAssetIndex.h"
#include "Engine/Content/Artifacts/ArtifactTarget.h"
#include "Engine/Content/Documents/CanonicalJsonWriter.h"
#include "Engine/Core/Collections/HashSet.h"
#include "Engine/Platform/File.h"
#include "Engine/Platform/FileSystem.h"
#include <algorithm>

namespace
{
    typedef rapidjson_flax::Document JsonDocument;
    typedef rapidjson_flax::Value JsonValue;
    typedef JsonDocument::AllocatorType JsonAlloc;

    bool Fail(AssetPipelineDiagnostic& diagnostic, const StringView& message)
    {
        diagnostic = AssetPipelineDiagnostic();
        diagnostic.Code = AssetPipelineDiagnosticCode::ArtifactInvalid;
        diagnostic.Stage = AssetPipelineDiagnosticStage::Cook;
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

    StringAnsi ObjectKey(const AssetObjectId& id)
    {
        return StringAnsi::Format("{0}:{1}", GuidKey(id.Guid), id.LocalId);
    }

    void AddAnsi(JsonValue& object, const char* name, const StringAnsiView& value, JsonAlloc& allocator);

    bool LessObjectId(const AssetObjectId& a, const AssetObjectId& b)
    {
        const StringAnsi aGuid = GuidKey(a.Guid);
        const StringAnsi bGuid = GuidKey(b.Guid);
        return aGuid == bGuid ? a.LocalId < b.LocalId : aGuid < bGuid;
    }

    void AddObjectIds(JsonValue& object, const char* name, const Array<AssetObjectId>& values, JsonAlloc& allocator)
    {
        Array<AssetObjectId> sorted(values);
        if (sorted.Count() > 1)
            std::sort(sorted.Get(), sorted.Get() + sorted.Count(), LessObjectId);
        JsonValue array(rapidjson::kArrayType);
        for (const AssetObjectId& id : sorted)
        {
            JsonValue item(rapidjson::kObjectType);
            AddAnsi(item, "fileGuid", GuidKey(id.Guid), allocator);
            item.AddMember("localId", id.LocalId, allocator);
            array.PushBack(item, allocator);
        }
        object.AddMember(JsonValue(name, allocator), array, allocator);
    }

    bool LessPreload(const RuntimeAssetPreload& a, const RuntimeAssetPreload& b)
    {
        if (a.Required != b.Required)
            return a.Required > b.Required;
        if (a.Priority != b.Priority)
            return a.Priority > b.Priority;
        return LessObjectId(a.ID, b.ID);
    }

    void AddPreloads(JsonValue& object, const Array<RuntimeAssetPreload>& values, JsonAlloc& allocator)
    {
        Array<RuntimeAssetPreload> sorted(values);
        if (sorted.Count() > 1)
            std::sort(sorted.Get(), sorted.Get() + sorted.Count(), LessPreload);
        JsonValue array(rapidjson::kArrayType);
        for (const RuntimeAssetPreload& preload : sorted)
        {
            JsonValue item(rapidjson::kObjectType);
            AddAnsi(item, "fileGuid", GuidKey(preload.ID.Guid), allocator);
            item.AddMember("localId", preload.ID.LocalId, allocator);
            item.AddMember("priority", preload.Priority, allocator);
            item.AddMember("estimatedBytes", preload.EstimatedBytes, allocator);
            item.AddMember("required", preload.Required, allocator);
            array.PushBack(item, allocator);
        }
        object.AddMember("preload", array, allocator);
    }

    void AddString(JsonValue& object, const char* name, const StringView& value, JsonAlloc& allocator)
    {
        const StringAnsi text(value);
        object.AddMember(JsonValue(name, allocator), JsonValue(text.Get(), text.Length(), allocator), allocator);
    }

    void AddAnsi(JsonValue& object, const char* name, const StringAnsiView& value, JsonAlloc& allocator)
    {
        object.AddMember(JsonValue(name, allocator), JsonValue(value.Get(), value.Length(), allocator), allocator);
    }

    bool ValidateEntry(const RuntimeAssetIndexEntry& entry, AssetPipelineDiagnostic& diagnostic)
    {
        if (!entry.ID.IsValid() || !entry.BackingAssetID.IsValid() || !entry.PackageID.IsValid() || entry.TypeName.IsEmpty() || entry.PackagedPath.IsEmpty() || entry.Size == 0)
            return Fail(diagnostic, TEXT("Runtime asset index entries require object identity, backing GUID, type, package identity, and a non-empty package range."));
        if (RuntimeAssetIndex::ContainsLibraryPath(entry.PackagedPath) || RuntimeAssetIndex::ContainsLibraryPath(entry.CanonicalPath))
            return Fail(diagnostic, TEXT("Runtime asset index must not refer to project Library storage."));
        if (EnumHasAnyFlags(entry.Flags, RuntimeAssetIndexFlags::ExactArtifact) && entry.ExactArtifact.IsZero())
            return Fail(diagnostic, TEXT("Runtime asset index exact-artifact entries require the immutable artifact key."));
        if (entry.Preload.HasItems() && entry.PreloadBudgetBytes == 0)
            return Fail(diagnostic, TEXT("Runtime asset preload plans require an explicit non-zero byte budget."));
        return false;
    }

    bool ValidateGraph(const Array<RuntimeAssetIndexEntry>& entries, AssetPipelineDiagnostic& diagnostic)
    {
        HashSet<AssetObjectId> available;
        for (const RuntimeAssetIndexEntry& entry : entries)
            available.Add(entry.ID);
        for (const RuntimeAssetIndexEntry& entry : entries)
        {
            HashSet<AssetObjectId> dependencies;
            HashSet<AssetObjectId> preload;
            HashSet<AssetObjectId> required;
            for (const AssetObjectId& dependency : entry.Dependencies)
            {
                if (!dependency.IsValid() || dependency == entry.ID || !available.Contains(dependency) || !dependencies.Add(dependency))
                    return Fail(diagnostic, TEXT("Runtime asset dependencies must be unique, packaged, exact object identities other than the owner."));
            }
            for (const RuntimeAssetPreload& request : entry.Preload)
            {
                const AssetObjectId& dependency = request.ID;
                if (!dependency.IsValid() || dependency == entry.ID || !available.Contains(dependency) || request.Priority == 0 ||
                    request.EstimatedBytes == 0 || !preload.Add(dependency))
                    return Fail(diagnostic, TEXT("Runtime asset preload closure must contain unique packaged exact object identities other than the owner."));
                if (request.Required)
                    required.Add(dependency);
            }
            for (const AssetObjectId& dependency : entry.Dependencies)
            {
                if (!required.Contains(dependency))
                    return Fail(diagnostic, TEXT("Runtime asset preload plan must mark every direct runtime dependency as required."));
            }
        }
        return false;
    }

    bool BuildAssetsObject(const Array<RuntimeAssetIndexEntry>& sorted, JsonValue& assets, JsonAlloc& allocator, AssetPipelineDiagnostic& diagnostic)
    {
        assets.SetObject();
        AssetObjectId previous;
        for (const RuntimeAssetIndexEntry& entry : sorted)
        {
            if (ValidateEntry(entry, diagnostic))
                return true;
            if (previous.IsValid() && previous == entry.ID)
                return Fail(diagnostic, TEXT("Runtime asset index contains a duplicate file GUID/local file ID."));
            previous = entry.ID;

            JsonValue item(rapidjson::kObjectType);
            AddAnsi(item, "fileGuid", GuidKey(entry.ID.Guid), allocator);
            item.AddMember("localId", entry.ID.LocalId, allocator);
            AddAnsi(item, "backingGuid", GuidKey(entry.BackingAssetID), allocator);
            AddString(item, "type", entry.TypeName, allocator);
            AddString(item, "canonicalPath", entry.CanonicalPath, allocator);
            AddString(item, "packagePath", entry.PackagedPath, allocator);
            AddAnsi(item, "packageId", GuidKey(entry.PackageID), allocator);
            item.AddMember("chunkId", entry.ChunkID, allocator);
            item.AddMember("offset", entry.Offset, allocator);
            item.AddMember("size", entry.Size, allocator);
            item.AddMember("assetFormatVersion", entry.AssetFormatVersion, allocator);
            item.AddMember("flags", static_cast<uint32>(entry.Flags), allocator);
            if (entry.ExactArtifact.IsZero())
                item.AddMember("exactArtifact", JsonValue(rapidjson::kNullType), allocator);
            else
                AddAnsi(item, "exactArtifact", entry.ExactArtifact.ToString(), allocator);
            AddObjectIds(item, "dependencies", entry.Dependencies, allocator);
            item.AddMember("preloadBudgetBytes", entry.PreloadBudgetBytes, allocator);
            AddPreloads(item, entry.Preload, allocator);
            const StringAnsi key = ObjectKey(entry.ID);
            assets.AddMember(JsonValue(key.Get(), key.Length(), allocator), item, allocator);
        }
        return false;
    }

    bool Serialize(JsonDocument& json, StringAnsi& output, const Array<StringAnsi>& order, AssetPipelineDiagnostic& diagnostic)
    {
        CanonicalJsonError error;
        if (CanonicalJsonWriter::Write(json, output, error, &order))
            return Fail(diagnostic, TEXT("Runtime asset index canonical serialization failed."));
        return false;
    }

    const JsonValue* Member(const JsonValue& object, const char* name)
    {
        const auto member = object.FindMember(name);
        return member == object.MemberEnd() ? nullptr : &member->value;
    }

    bool ReadString(const JsonValue& object, const char* name, String& value)
    {
        const JsonValue* member = Member(object, name);
        if (!member || !member->IsString())
            return true;
        value = String(StringAnsiView(member->GetString(), member->GetStringLength()));
        return false;
    }

    bool ReadGuid(const JsonValue& object, const char* name, Guid& value)
    {
        const JsonValue* member = Member(object, name);
        return !member || !member->IsString() || Guid::Parse(StringAnsiView(member->GetString(), member->GetStringLength()), value);
    }

    bool ReadObjectIds(const JsonValue& object, const char* name, Array<AssetObjectId>& values)
    {
        values.Clear();
        const JsonValue* member = Member(object, name);
        if (!member || !member->IsArray())
            return true;
        values.EnsureCapacity(static_cast<int32>(member->Size()));
        for (const JsonValue& item : member->GetArray())
        {
            if (!item.IsObject() || item.MemberCount() != 2)
                return true;
            AssetObjectId id;
            const JsonValue* localId = Member(item, "localId");
            if (ReadGuid(item, "fileGuid", id.Guid) || !localId || !localId->IsInt64())
                return true;
            id.LocalId = localId->GetInt64();
            if (!id.IsValid())
                return true;
            values.Add(id);
        }
        return false;
    }

    bool ReadPreloads(const JsonValue& object, Array<RuntimeAssetPreload>& values)
    {
        values.Clear();
        const JsonValue* member = Member(object, "preload");
        if (!member || !member->IsArray())
            return true;
        values.EnsureCapacity(static_cast<int32>(member->Size()));
        for (const JsonValue& item : member->GetArray())
        {
            if (!item.IsObject() || item.MemberCount() != 5)
                return true;
            RuntimeAssetPreload value;
            const JsonValue* localId = Member(item, "localId");
            const JsonValue* priority = Member(item, "priority");
            const JsonValue* estimatedBytes = Member(item, "estimatedBytes");
            const JsonValue* required = Member(item, "required");
            if (ReadGuid(item, "fileGuid", value.ID.Guid) || !localId || !localId->IsInt64() ||
                !priority || !priority->IsUint() || !estimatedBytes || !estimatedBytes->IsUint64() ||
                !required || !required->IsBool())
                return true;
            value.ID.LocalId = localId->GetInt64();
            value.Priority = priority->GetUint();
            value.EstimatedBytes = estimatedBytes->GetUint64();
            value.Required = required->GetBool();
            if (!value.ID.IsValid())
                return true;
            values.Add(value);
        }
        return false;
    }

    bool ValidateReproducibilityJson(const StringAnsiView& input, ArtifactKey& inputHash, ContentHash& contentHash,
        AssetPipelineDiagnostic& diagnostic)
    {
        JsonDocument json;
        json.Parse(input.Get(), input.Length());
        if (json.HasParseError() || !json.IsObject() || json.MemberCount() != 9)
            return Fail(diagnostic, TEXT("Cook reproducibility manifest is malformed or has unexpected root fields."));
        const JsonValue* version = Member(json, "formatVersion");
        const JsonValue* storedContentHash = Member(json, "contentHash");
        const JsonValue* storedInputHash = Member(json, "inputHash");
        const JsonValue* engineBuild = Member(json, "engineBuild");
        const JsonValue* target = Member(json, "targetFingerprint");
        const JsonValue* deterministic = Member(json, "deterministic");
        const JsonValue* roots = Member(json, "roots");
        const JsonValue* artifacts = Member(json, "artifacts");
        const JsonValue* packages = Member(json, "packages");
        if (!version || !version->IsInt() || version->GetInt() != 1 || !storedContentHash || !storedContentHash->IsString() ||
            !storedInputHash || !storedInputHash->IsString() || !engineBuild || !engineBuild->IsInt() || !target || !target->IsString() ||
            !deterministic || !deterministic->IsBool() || !roots || !roots->IsArray() || !artifacts || !artifacts->IsObject() ||
            !packages || !packages->IsObject() || ArtifactKey::Parse(storedInputHash->GetStringAnsiView(), inputHash) ||
            ContentHash::Parse(storedContentHash->GetStringAnsiView(), contentHash))
            return Fail(diagnostic, TEXT("Cook reproducibility manifest fields have invalid types or hashes."));

        Array<StringAnsi> fullOrder;
        fullOrder.Add("formatVersion");
        fullOrder.Add("contentHash");
        fullOrder.Add("inputHash");
        fullOrder.Add("engineBuild");
        fullOrder.Add("targetFingerprint");
        fullOrder.Add("deterministic");
        fullOrder.Add("roots");
        fullOrder.Add("artifacts");
        fullOrder.Add("packages");
        StringAnsi canonical;
        if (Serialize(json, canonical, fullOrder, diagnostic) || canonical.Length() != input.Length() ||
            Platform::MemoryCompare(canonical.Get(), input.Get(), input.Length()) != 0)
            return Fail(diagnostic, TEXT("Cook reproducibility manifest is not canonical."));

        JsonDocument payload;
        payload.CopyFrom(json, payload.GetAllocator());
        payload.RemoveMember("contentHash");
        Array<StringAnsi> payloadOrder;
        payloadOrder.Add("formatVersion");
        payloadOrder.Add("inputHash");
        payloadOrder.Add("engineBuild");
        payloadOrder.Add("targetFingerprint");
        payloadOrder.Add("deterministic");
        payloadOrder.Add("roots");
        payloadOrder.Add("artifacts");
        payloadOrder.Add("packages");
        StringAnsi payloadText;
        if (Serialize(payload, payloadText, payloadOrder, diagnostic) ||
            ContentHash::Compute(payloadText.Get(), payloadText.Length()) != contentHash)
            return Fail(diagnostic, TEXT("Cook reproducibility manifest content hash verification failed."));
        return false;
    }
}

bool RuntimeAssetIndex::ContainsLibraryPath(const StringView& path)
{
    String value(path);
    value.Replace('\\', '/');
    value = value.ToLower();
    return value.Contains(TEXT("/library/")) || value.StartsWith(TEXT("library/"));
}

bool RuntimeAssetIndex::WriteCanonicalJson(const Array<RuntimeAssetIndexEntry>& entries, StringAnsi& output, AssetPipelineDiagnostic& diagnostic)
{
    Array<RuntimeAssetIndexEntry> sorted = entries;
    if (sorted.Count() > 1)
        std::sort(sorted.Get(), sorted.Get() + sorted.Count(), [](const RuntimeAssetIndexEntry& a, const RuntimeAssetIndexEntry& b)
        {
            return LessObjectId(a.ID, b.ID);
        });
    if (ValidateGraph(sorted, diagnostic))
        return true;

    JsonDocument payload;
    payload.SetObject();
    JsonAlloc& payloadAllocator = payload.GetAllocator();
    payload.AddMember("formatVersion", FormatVersion, payloadAllocator);
    JsonValue payloadAssets;
    if (BuildAssetsObject(sorted, payloadAssets, payloadAllocator, diagnostic))
        return true;
    payload.AddMember("assets", payloadAssets, payloadAllocator);
    Array<StringAnsi> payloadOrder;
    payloadOrder.Add("formatVersion");
    payloadOrder.Add("assets");
    StringAnsi payloadText;
    if (Serialize(payload, payloadText, payloadOrder, diagnostic))
        return true;
    const StringAnsi contentHash = ContentHash::Compute(payloadText.Get(), payloadText.Length()).ToString();

    JsonDocument json;
    json.SetObject();
    JsonAlloc& allocator = json.GetAllocator();
    json.AddMember("formatVersion", FormatVersion, allocator);
    AddAnsi(json, "contentHash", contentHash, allocator);
    JsonValue assets;
    if (BuildAssetsObject(sorted, assets, allocator, diagnostic))
        return true;
    json.AddMember("assets", assets, allocator);
    Array<StringAnsi> rootOrder;
    rootOrder.Add("formatVersion");
    rootOrder.Add("contentHash");
    rootOrder.Add("assets");
    if (Serialize(json, output, rootOrder, diagnostic))
        return true;
    diagnostic = AssetPipelineDiagnostic();
    return false;
}

bool RuntimeAssetIndex::Parse(const StringAnsiView& input, Array<RuntimeAssetIndexEntry>& entries, AssetPipelineDiagnostic& diagnostic)
{
    entries.Clear();
    JsonDocument json;
    json.Parse(input.Get(), input.Length());
    if (json.HasParseError() || !json.IsObject() || json.MemberCount() != 3)
        return Fail(diagnostic, TEXT("Runtime asset index is malformed or has unexpected root fields."));
    const JsonValue* version = Member(json, "formatVersion");
    const JsonValue* hash = Member(json, "contentHash");
    const JsonValue* assets = Member(json, "assets");
    if (!version || !version->IsInt() || version->GetInt() != FormatVersion || !hash || !hash->IsString() || !assets || !assets->IsObject())
        return Fail(diagnostic, TEXT("Runtime asset index version, hash, or assets table is invalid."));

    HashSet<AssetObjectId> objectIds;
    HashSet<Guid> backingIds;
    entries.EnsureCapacity(static_cast<int32>(assets->MemberCount()));
    for (auto i = assets->MemberBegin(); i != assets->MemberEnd(); ++i)
    {
        if (!i->name.IsString() || !i->value.IsObject() || i->value.MemberCount() != 16)
            return Fail(diagnostic, TEXT("Runtime asset index contains a malformed asset location."));
        const JsonValue& item = i->value;
        RuntimeAssetIndexEntry entry;
        const JsonValue* localId = Member(item, "localId");
        const JsonValue* chunkId = Member(item, "chunkId");
        const JsonValue* offset = Member(item, "offset");
        const JsonValue* size = Member(item, "size");
        const JsonValue* assetFormatVersion = Member(item, "assetFormatVersion");
        const JsonValue* flags = Member(item, "flags");
        const JsonValue* artifact = Member(item, "exactArtifact");
        const JsonValue* preloadBudget = Member(item, "preloadBudgetBytes");
        if (ReadGuid(item, "fileGuid", entry.ID.Guid) || !localId || !localId->IsInt64() ||
            ReadGuid(item, "backingGuid", entry.BackingAssetID) || ReadString(item, "type", entry.TypeName) ||
            ReadString(item, "canonicalPath", entry.CanonicalPath) || ReadString(item, "packagePath", entry.PackagedPath) ||
            ReadGuid(item, "packageId", entry.PackageID) || !chunkId || !chunkId->IsUint() || !offset || !offset->IsUint64() ||
            !size || !size->IsUint64() || !assetFormatVersion || !assetFormatVersion->IsUint() || !flags || !flags->IsUint() || !artifact ||
            !preloadBudget || !preloadBudget->IsUint64() || ReadObjectIds(item, "dependencies", entry.Dependencies) || ReadPreloads(item, entry.Preload))
            return Fail(diagnostic, TEXT("Runtime asset index asset location fields have invalid types."));
        entry.ID.LocalId = localId->GetInt64();
        entry.ChunkID = chunkId->GetUint();
        entry.Offset = offset->GetUint64();
        entry.Size = size->GetUint64();
        entry.AssetFormatVersion = assetFormatVersion->GetUint();
        entry.Flags = static_cast<RuntimeAssetIndexFlags>(flags->GetUint());
        entry.PreloadBudgetBytes = preloadBudget->GetUint64();
        if (!artifact->IsNull() && (!artifact->IsString() || ArtifactKey::Parse(StringAnsiView(artifact->GetString(), artifact->GetStringLength()), entry.ExactArtifact)))
            return Fail(diagnostic, TEXT("Runtime asset index exact artifact key is invalid."));
        if (ValidateEntry(entry, diagnostic) || ObjectKey(entry.ID) != StringAnsiView(i->name.GetString(), i->name.GetStringLength()) ||
            !objectIds.Add(entry.ID) || !backingIds.Add(entry.BackingAssetID))
            return Fail(diagnostic, TEXT("Runtime asset index identity keys are inconsistent or duplicated."));
        entries.Add(MoveTemp(entry));
    }

    StringAnsi canonical;
    if (WriteCanonicalJson(entries, canonical, diagnostic))
        return true;
    if (canonical.Length() != input.Length() || Platform::MemoryCompare(canonical.Get(), input.Get(), input.Length()) != 0)
        return Fail(diagnostic, TEXT("Runtime asset index is not canonical or failed content-hash verification."));
    diagnostic = AssetPipelineDiagnostic();
    return false;
}

bool RuntimeAssetIndex::Load(const StringView& path, Array<RuntimeAssetIndexEntry>& entries, AssetPipelineDiagnostic& diagnostic)
{
    StringAnsi json;
    if (File::ReadAllText(path, json))
        return Fail(diagnostic, TEXT("Runtime asset index could not be read."));
    return Parse(json, entries, diagnostic);
}

bool RuntimeAssetIndex::SaveAtomic(const StringView& path, const Array<RuntimeAssetIndexEntry>& entries, AssetPipelineDiagnostic& diagnostic)
{
    StringAnsi json;
    if (WriteCanonicalJson(entries, json, diagnostic))
        return true;
    const String destination(path);
    const String staging = destination + TEXT(".tmp");
    if (File::WriteAllBytes(staging, json.Get(), json.Length()) || FileSystem::MoveFile(destination, staging, true))
    {
        FileSystem::DeleteFile(staging);
        return Fail(diagnostic, TEXT("Runtime asset index could not be written atomically."));
    }
    diagnostic = AssetPipelineDiagnostic();
    return false;
}

bool RuntimeAssetIndex::WriteReproducibilityJson(const RuntimeBuildReproducibility& manifest, StringAnsi& output, AssetPipelineDiagnostic& diagnostic)
{
    if (manifest.EngineBuild <= 0 || manifest.TargetFingerprint.IsZero())
        return Fail(diagnostic, TEXT("Cook reproducibility manifest requires an engine build and target fingerprint."));

    Array<AssetObjectId> roots(manifest.Roots);
    if (roots.Count() > 1)
        std::sort(roots.Get(), roots.Get() + roots.Count(), LessObjectId);
    HashSet<AssetObjectId> rootIds;
    for (const AssetObjectId& root : roots)
    {
        if (!root.IsValid() || !rootIds.Add(root))
            return Fail(diagnostic, TEXT("Cook reproducibility roots must be unique exact object identities."));
    }

    Array<RuntimeBuildArtifactEvidence> artifacts(manifest.Artifacts);
    if (artifacts.Count() > 1)
        std::sort(artifacts.Get(), artifacts.Get() + artifacts.Count(), [](const RuntimeBuildArtifactEvidence& a, const RuntimeBuildArtifactEvidence& b)
        {
            return LessObjectId(a.ID, b.ID);
        });
    HashSet<AssetObjectId> artifactIds;
    for (const RuntimeBuildArtifactEvidence& artifact : artifacts)
    {
        if (!artifact.ID.IsValid() || artifact.Artifact.IsZero() || artifact.InputFingerprint.IsZero() ||
            artifact.SettingsHash.IsZero() || artifact.ProcessorID.IsEmpty() || artifact.ProcessorVersion == 0 || !artifactIds.Add(artifact.ID))
            return Fail(diagnostic, TEXT("Cook reproducibility artifact evidence is incomplete or duplicated."));
        HashSet<String> environmentIds;
        for (const RuntimeBuildDependencyEvidence& dependency : artifact.Environment)
        {
            const String key = dependency.Kind + TEXT(":") + dependency.Identity;
            if (dependency.Kind.IsEmpty() || dependency.Identity.IsEmpty() || dependency.Hash.IsZero() || !environmentIds.Add(key))
                return Fail(diagnostic, TEXT("Cook reproducibility provider/toolchain/environment evidence is incomplete or duplicated."));
        }
    }

    Array<RuntimeBuildPackageEvidence> packages(manifest.Packages);
    if (packages.Count() > 1)
        std::sort(packages.Get(), packages.Get() + packages.Count(), [](const RuntimeBuildPackageEvidence& a, const RuntimeBuildPackageEvidence& b)
        {
            return GuidKey(a.PackageID) < GuidKey(b.PackageID);
        });
    HashSet<Guid> packageIds;
    for (const RuntimeBuildPackageEvidence& package : packages)
    {
        if (!package.PackageID.IsValid() || package.Path.IsEmpty() || package.Content.IsZero() || package.Size == 0 ||
            ContainsLibraryPath(package.Path) || !packageIds.Add(package.PackageID))
            return Fail(diagnostic, TEXT("Cook reproducibility package evidence is incomplete, duplicated, or refers to Library."));
    }

    ArtifactKeyBuilder inputBuilder(StringAnsiView("flax-cook-reproducibility-input-v1"));
    inputBuilder.AddUInt32(StringAnsiView("engineBuild"), static_cast<uint32>(manifest.EngineBuild));
    inputBuilder.AddKey(StringAnsiView("target"), manifest.TargetFingerprint);
    inputBuilder.AddBool(StringAnsiView("deterministic"), manifest.Deterministic);
    for (int32 i = 0; i < roots.Count(); i++)
    {
        inputBuilder.AddGuid(StringAnsi::Format("root.{0}.guid", i), roots[i].Guid);
        inputBuilder.AddUInt64(StringAnsi::Format("root.{0}.localId", i), static_cast<uint64>(roots[i].LocalId));
    }
    for (int32 i = 0; i < artifacts.Count(); i++)
    {
        const RuntimeBuildArtifactEvidence& artifact = artifacts[i];
        inputBuilder.AddGuid(StringAnsi::Format("artifact.{0}.guid", i), artifact.ID.Guid);
        inputBuilder.AddUInt64(StringAnsi::Format("artifact.{0}.localId", i), static_cast<uint64>(artifact.ID.LocalId));
        inputBuilder.AddKey(StringAnsi::Format("artifact.{0}.output", i), artifact.Artifact);
        inputBuilder.AddKey(StringAnsi::Format("artifact.{0}.input", i), artifact.InputFingerprint);
        inputBuilder.AddHash(StringAnsi::Format("artifact.{0}.settings", i), artifact.SettingsHash);
        inputBuilder.AddString(StringAnsi::Format("artifact.{0}.processor", i), artifact.ProcessorID);
        inputBuilder.AddUInt32(StringAnsi::Format("artifact.{0}.processorVersion", i), artifact.ProcessorVersion);
        Array<RuntimeBuildDependencyEvidence> environment(artifact.Environment);
        if (environment.Count() > 1)
            std::sort(environment.Get(), environment.Get() + environment.Count(), [](const RuntimeBuildDependencyEvidence& a, const RuntimeBuildDependencyEvidence& b)
            {
                return a.Kind == b.Kind ? a.Identity < b.Identity : a.Kind < b.Kind;
            });
        for (int32 j = 0; j < environment.Count(); j++)
        {
            inputBuilder.AddString(StringAnsi::Format("artifact.{0}.environment.{1}.kind", i, j), environment[j].Kind);
            inputBuilder.AddString(StringAnsi::Format("artifact.{0}.environment.{1}.identity", i, j), environment[j].Identity);
            inputBuilder.AddHash(StringAnsi::Format("artifact.{0}.environment.{1}.hash", i, j), environment[j].Hash);
        }
    }
    const StringAnsi inputHash = inputBuilder.Finalize().ToString();

    auto buildDocument = [&](JsonDocument& json, const StringAnsi* contentHash)
    {
        json.SetObject();
        JsonAlloc& allocator = json.GetAllocator();
        json.AddMember("formatVersion", 1, allocator);
        if (contentHash)
            AddAnsi(json, "contentHash", *contentHash, allocator);
        AddAnsi(json, "inputHash", inputHash, allocator);
        json.AddMember("engineBuild", manifest.EngineBuild, allocator);
        AddAnsi(json, "targetFingerprint", manifest.TargetFingerprint.ToString(), allocator);
        json.AddMember("deterministic", manifest.Deterministic, allocator);
        AddObjectIds(json, "roots", roots, allocator);

        JsonValue artifactTable(rapidjson::kObjectType);
        for (const RuntimeBuildArtifactEvidence& artifact : artifacts)
        {
            JsonValue item(rapidjson::kObjectType);
            AddAnsi(item, "artifact", artifact.Artifact.ToString(), allocator);
            AddAnsi(item, "inputFingerprint", artifact.InputFingerprint.ToString(), allocator);
            AddString(item, "processor", artifact.ProcessorID, allocator);
            item.AddMember("processorVersion", artifact.ProcessorVersion, allocator);
            AddAnsi(item, "settingsHash", artifact.SettingsHash.ToString(), allocator);
            Array<RuntimeBuildDependencyEvidence> environment(artifact.Environment);
            if (environment.Count() > 1)
                std::sort(environment.Get(), environment.Get() + environment.Count(), [](const RuntimeBuildDependencyEvidence& a, const RuntimeBuildDependencyEvidence& b)
                {
                    return a.Kind == b.Kind ? a.Identity < b.Identity : a.Kind < b.Kind;
                });
            JsonValue environmentArray(rapidjson::kArrayType);
            for (const RuntimeBuildDependencyEvidence& dependency : environment)
            {
                JsonValue dependencyValue(rapidjson::kObjectType);
                AddString(dependencyValue, "kind", dependency.Kind, allocator);
                AddString(dependencyValue, "identity", dependency.Identity, allocator);
                AddAnsi(dependencyValue, "hash", dependency.Hash.ToString(), allocator);
                environmentArray.PushBack(dependencyValue, allocator);
            }
            item.AddMember("environment", environmentArray, allocator);
            const StringAnsi key = ObjectKey(artifact.ID);
            artifactTable.AddMember(JsonValue(key.Get(), key.Length(), allocator), item, allocator);
        }
        json.AddMember("artifacts", artifactTable, allocator);

        JsonValue packageTable(rapidjson::kObjectType);
        for (const RuntimeBuildPackageEvidence& package : packages)
        {
            JsonValue item(rapidjson::kObjectType);
            AddAnsi(item, "content", package.Content.ToString(), allocator);
            AddString(item, "path", package.Path, allocator);
            item.AddMember("size", package.Size, allocator);
            const StringAnsi key = GuidKey(package.PackageID);
            packageTable.AddMember(JsonValue(key.Get(), key.Length(), allocator), item, allocator);
        }
        json.AddMember("packages", packageTable, allocator);
    };

    Array<StringAnsi> payloadOrder;
    payloadOrder.Add("formatVersion");
    payloadOrder.Add("inputHash");
    payloadOrder.Add("engineBuild");
    payloadOrder.Add("targetFingerprint");
    payloadOrder.Add("deterministic");
    payloadOrder.Add("roots");
    payloadOrder.Add("artifacts");
    payloadOrder.Add("packages");
    JsonDocument payload;
    buildDocument(payload, nullptr);
    StringAnsi payloadText;
    if (Serialize(payload, payloadText, payloadOrder, diagnostic))
        return true;
    const StringAnsi hash = ContentHash::Compute(payloadText.Get(), payloadText.Length()).ToString();

    JsonDocument json;
    buildDocument(json, &hash);
    Array<StringAnsi> order;
    order.Add("formatVersion");
    order.Add("contentHash");
    order.Add("inputHash");
    order.Add("engineBuild");
    order.Add("targetFingerprint");
    order.Add("deterministic");
    order.Add("roots");
    order.Add("artifacts");
    order.Add("packages");
    if (Serialize(json, output, order, diagnostic))
        return true;
    diagnostic = AssetPipelineDiagnostic();
    return false;
}

bool RuntimeAssetIndex::SaveReproducibilityAtomic(const StringView& path, const RuntimeBuildReproducibility& manifest, AssetPipelineDiagnostic& diagnostic)
{
    StringAnsi json;
    if (WriteReproducibilityJson(manifest, json, diagnostic))
        return true;
    const String destination(path);
    const String staging = destination + TEXT(".tmp");
    if (File::WriteAllBytes(staging, json.Get(), json.Length()))
    {
        FileSystem::DeleteFile(staging);
        return Fail(diagnostic, TEXT("Cook reproducibility manifest could not be written atomically."));
    }
    if (FileSystem::FileExists(destination))
    {
        StringAnsi baselineJson;
        JsonDocument baseline;
        if (!File::ReadAllText(destination, baselineJson))
            baseline.Parse(baselineJson.Get(), baselineJson.Length());
        if (!baseline.HasParseError() && baseline.IsObject() && Member(baseline, "inputHash"))
        {
            bool sameInputs = false;
            bool identical = false;
            String difference;
            if (CompareReproducibilityFiles(destination, staging, sameInputs, identical, difference, diagnostic))
            {
                FileSystem::DeleteFile(staging);
                return true;
            }
            if (sameInputs && !identical)
            {
                FileSystem::DeleteFile(staging);
                return Fail(diagnostic, TEXT("Identical cook inputs produced different package hashes; reproducibility check rejected the build."));
            }
            if (identical)
            {
                FileSystem::DeleteFile(staging);
                diagnostic = AssetPipelineDiagnostic();
                return false;
            }
        }
    }
    if (FileSystem::MoveFile(destination, staging, true))
    {
        FileSystem::DeleteFile(staging);
        return Fail(diagnostic, TEXT("Cook reproducibility manifest could not be written atomically."));
    }
    diagnostic = AssetPipelineDiagnostic();
    return false;
}

bool RuntimeAssetIndex::CompareReproducibilityFiles(const StringView& baselinePath, const StringView& candidatePath,
    bool& sameInputs, bool& identical, String& difference, AssetPipelineDiagnostic& diagnostic)
{
    sameInputs = false;
    identical = false;
    difference.Clear();
    StringAnsi baselineJson;
    StringAnsi candidateJson;
    if (File::ReadAllText(baselinePath, baselineJson) || File::ReadAllText(candidatePath, candidateJson))
        return Fail(diagnostic, TEXT("Cook reproducibility comparison could not read both manifests."));
    ArtifactKey baselineInput;
    ArtifactKey candidateInput;
    ContentHash baselineContent;
    ContentHash candidateContent;
    if (ValidateReproducibilityJson(baselineJson, baselineInput, baselineContent, diagnostic) ||
        ValidateReproducibilityJson(candidateJson, candidateInput, candidateContent, diagnostic))
        return true;
    sameInputs = baselineInput == candidateInput;
    identical = baselineContent == candidateContent;
    if (!sameInputs)
        difference = TEXT("Cook input fingerprints differ.");
    else if (!identical)
        difference = TEXT("Identical cook inputs produced different package/index evidence.");
    diagnostic = AssetPipelineDiagnostic();
    return false;
}
