// Copyright (c) Wojciech Figat. All rights reserved.

#include "MigrationInventory.h"
#include "Engine/Content/Artifacts/ArtifactKey.h"
#include "Engine/Content/Documents/CanonicalJsonWriter.h"
#include "Engine/Platform/StringUtils.h"
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

    const Char* SourceKindName(AssetSourceKind kind)
    {
        switch (kind)
        {
        case AssetSourceKind::ImportedSource: return TEXT("ImportedSource");
        case AssetSourceKind::TextDocument: return TEXT("TextDocument");
        case AssetSourceKind::ExistingJson: return TEXT("ExistingJson");
        default: return TEXT("LegacyBinary");
        }
    }

    bool IsGraphType(const String& typeName, String& extension)
    {
        if (typeName == TEXT("FlaxEngine.Material"))
        {
            extension = TEXT("material");
            return true;
        }
        if (typeName == TEXT("FlaxEngine.MaterialFunction"))
        {
            extension = TEXT("materialfunction");
            return true;
        }
        if (typeName == TEXT("FlaxEngine.AnimationGraph"))
        {
            extension = TEXT("animgraph");
            return true;
        }
        if (typeName == TEXT("FlaxEngine.AnimationGraphFunction"))
        {
            extension = TEXT("animgraphfunction");
            return true;
        }
        if (typeName == TEXT("FlaxEngine.VisualScript"))
        {
            extension = TEXT("visualscript");
            return true;
        }
        if (typeName == TEXT("FlaxEngine.BehaviorTree"))
        {
            extension = TEXT("behaviortree");
            return true;
        }
        if (typeName == TEXT("FlaxEngine.ParticleEmitterFunction"))
        {
            extension = TEXT("particlefunction");
            return true;
        }
        return false;
    }

    bool IsImportedType(const String& typeName)
    {
        return typeName == TEXT("FlaxEngine.Texture") ||
            typeName == TEXT("FlaxEngine.CubeTexture") ||
            typeName == TEXT("FlaxEngine.Model") ||
            typeName == TEXT("FlaxEngine.SkinnedModel") ||
            typeName == TEXT("FlaxEngine.Animation") ||
            typeName == TEXT("FlaxEngine.AudioClip") ||
            typeName == TEXT("FlaxEngine.FontAsset") ||
            typeName == TEXT("FlaxEngine.Video");
    }

    String ReplaceExtension(const String& path, const String& extension)
    {
        const String folder = StringUtils::GetDirectoryName(path);
        const String name = StringUtils::GetFileNameWithoutExtension(path);
        if (folder.IsEmpty())
            return name + TEXT(".") + extension;
        return folder / name + TEXT(".") + extension;
    }

    void AddString(JsonValue& object, const char* key, const String& value, JsonAlloc& allocator)
    {
        const StringAnsi ansi(value);
        object.AddMember(JsonValue(key, allocator), JsonValue(ansi.Get(), ansi.Length(), allocator), allocator);
    }
}

const Char* MigrationInventory::GetEligibilityName(MigrationEligibility eligibility)
{
    switch (eligibility)
    {
    case MigrationEligibility::AlreadyMigrated: return TEXT("AlreadyMigrated");
    case MigrationEligibility::ReadyToMigrate: return TEXT("ReadyToMigrate");
    case MigrationEligibility::MissingOriginalSource: return TEXT("MissingOriginalSource");
    case MigrationEligibility::Conflict: return TEXT("Conflict");
    default: return TEXT("Unsupported");
    }
}

MigrationEligibility MigrationInventory::Classify(const AssetRecord& record, String& reason, String& proposedDestination)
{
    reason = String::Empty;
    proposedDestination = record.SourcePath.Get();
    if (record.Status == AssetRecordStatus::DuplicateGuid || record.Status == AssetRecordStatus::PathCollision)
    {
        reason = TEXT("Legacy and canonical records claim the same identity or display path.");
        return MigrationEligibility::Conflict;
    }
    if (record.SourceKind != AssetSourceKind::LegacyBinary)
    {
        reason = TEXT("Record is already registered as a canonical source.");
        return MigrationEligibility::AlreadyMigrated;
    }
    String extension;
    if (IsGraphType(record.TypeName, extension))
    {
        proposedDestination = ReplaceExtension(record.SourcePath.Get(), extension);
        reason = TEXT("Legacy graph binary can be converted to a text document while preserving the GUID.");
        return MigrationEligibility::ReadyToMigrate;
    }
    if (IsImportedType(record.TypeName))
    {
        reason = TEXT("Legacy imported binary remains until the original source file is present beside a sidecar.");
        return MigrationEligibility::MissingOriginalSource;
    }
    reason = TEXT("No migrator is registered for this type.");
    return MigrationEligibility::Unsupported;
}

void MigrationInventory::Build(const Array<AssetRecord>& records, Array<MigrationInventoryEntry>& entries)
{
    entries.Clear();
    Array<AssetRecord> mains;
    for (const AssetRecord& record : records)
    {
        if (record.IsMainAsset())
            mains.Add(record);
    }
    if (mains.Count() > 1)
    {
        std::sort(mains.Get(), mains.Get() + mains.Count(), [](const AssetRecord& a, const AssetRecord& b)
        {
            return GuidKey(a.ID) < GuidKey(b.ID);
        });
    }
    for (const AssetRecord& record : mains)
    {
        MigrationInventoryEntry entry;
        entry.ID = record.ID;
        entry.TypeName = record.TypeName;
        entry.SourcePath = record.SourcePath.Get();
        entry.SourceKind = SourceKindName(record.SourceKind);
        const MigrationEligibility eligibility = Classify(record, entry.Reason, entry.ProposedDestination);
        entry.Eligibility = GetEligibilityName(eligibility);
        entries.Add(entry);
    }
}

bool MigrationInventory::WriteCanonicalJson(const Array<MigrationInventoryEntry>& entries, StringAnsi& output, AssetPipelineDiagnostic& diagnostic)
{
    JsonDocument json;
    json.SetObject();
    JsonAlloc& allocator = json.GetAllocator();
    json.AddMember("formatVersion", FormatVersion, allocator);
    JsonValue assets(rapidjson::kArrayType);
    for (const MigrationInventoryEntry& entry : entries)
    {
        JsonValue item(rapidjson::kObjectType);
        const StringAnsi id = GuidKey(entry.ID);
        item.AddMember("id", JsonValue(id.Get(), id.Length(), allocator), allocator);
        AddString(item, "type", entry.TypeName, allocator);
        AddString(item, "sourcePath", entry.SourcePath, allocator);
        AddString(item, "proposedDestination", entry.ProposedDestination, allocator);
        AddString(item, "sourceKind", entry.SourceKind, allocator);
        AddString(item, "eligibility", entry.Eligibility, allocator);
        AddString(item, "reason", entry.Reason, allocator);
        assets.PushBack(item, allocator);
    }
    json.AddMember("assets", assets, allocator);
    CanonicalJsonError error;
    Array<StringAnsi> rootOrder;
    rootOrder.Add("formatVersion");
    rootOrder.Add("assets");
    if (CanonicalJsonWriter::Write(json, output, error, &rootOrder))
        return Fail(diagnostic, TEXT("Migration inventory canonical serialization failed."));
    diagnostic = AssetPipelineDiagnostic();
    return false;
}

bool MigrationInventory::Fingerprint(const Array<MigrationInventoryEntry>& entries, const Array<Guid>& selected, const StringView& backupRoot, StringAnsi& fingerprint, AssetPipelineDiagnostic& diagnostic)
{
    StringAnsi inventory;
    if (WriteCanonicalJson(entries, inventory, diagnostic))
        return true;
    Array<Guid> ids = selected;
    if (ids.Count() > 1)
    {
        std::sort(ids.Get(), ids.Get() + ids.Count(), [](const Guid& a, const Guid& b)
        {
            return GuidKey(a) < GuidKey(b);
        });
    }
    ContentHasher hasher;
    hasher.Update(inventory.Get(), inventory.Length());
    hasher.Update("|", 1);
    const StringAnsi root(backupRoot);
    hasher.Update(root.Get(), root.Length());
    for (const Guid& id : ids)
    {
        hasher.Update("|", 1);
        const StringAnsi key = GuidKey(id);
        hasher.Update(key.Get(), key.Length());
    }
    fingerprint = hasher.Finalize().ToString();
    diagnostic = AssetPipelineDiagnostic();
    return false;
}

bool MigrationInventory::HasBlockingConflict(const Array<MigrationInventoryEntry>& entries)
{
    for (const MigrationInventoryEntry& entry : entries)
    {
        if (entry.Eligibility == TEXT("Conflict"))
            return true;
    }
    return false;
}
