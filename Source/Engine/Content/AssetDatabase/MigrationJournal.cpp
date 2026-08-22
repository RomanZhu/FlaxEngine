// Copyright (c) Wojciech Figat. All rights reserved.

#include "MigrationJournal.h"
#include "Engine/Content/Artifacts/ArtifactKey.h"
#include "Engine/Content/Documents/CanonicalJsonWriter.h"
#include "Engine/Platform/File.h"
#include "Engine/Platform/FileSystem.h"
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

    void AddAnsi(JsonValue& object, const char* key, const StringAnsiView& value, JsonAlloc& allocator)
    {
        object.AddMember(JsonValue(key, allocator), JsonValue(value.Get(), value.Length(), allocator), allocator);
    }

    void AddString(JsonValue& object, const char* key, const String& value, JsonAlloc& allocator)
    {
        AddAnsi(object, key, StringAnsi(value), allocator);
    }

    bool ReadString(const JsonValue& object, const char* key, String& value)
    {
        const auto* member = object.FindMember(key);
        if (member == object.MemberEnd() || !member->value.IsString())
            return true;
        value = String(StringAnsiView(member->value.GetString(), member->value.GetStringLength()));
        return false;
    }

    bool HashFile(const StringView& path, String& hash, AssetPipelineDiagnostic& diagnostic)
    {
        Array<byte> bytes;
        if (File::ReadAllBytes(path, bytes))
            return Fail(diagnostic, TEXT("Migration could not hash a required file."));
        hash = String(ContentHash::Compute(bytes.Get(), bytes.Count()).ToString());
        diagnostic = AssetPipelineDiagnostic();
        return false;
    }

    bool EnsureDirectory(const StringView& path)
    {
        const String folder(StringUtils::GetDirectoryName(path));
        return folder.HasChars() && !FileSystem::DirectoryExists(folder) && FileSystem::CreateDirectory(folder);
    }

    const MigrationInventoryEntry* FindEntry(const Array<MigrationInventoryEntry>& inventory, const Guid& id)
    {
        for (const MigrationInventoryEntry& entry : inventory)
        {
            if (entry.ID == id)
                return &entry;
        }
        return nullptr;
    }
}

const Char* MigrationSession::GetStateName(MigrationJournalState state)
{
    switch (state)
    {
    case MigrationJournalState::Planned: return TEXT("Planned");
    case MigrationJournalState::BackedUp: return TEXT("BackedUp");
    case MigrationJournalState::Published: return TEXT("Published");
    case MigrationJournalState::Committed: return TEXT("Committed");
    case MigrationJournalState::RolledBack: return TEXT("RolledBack");
    case MigrationJournalState::Failed: return TEXT("Failed");
    default: return TEXT("None");
    }
}

bool MigrationSession::ParseState(const StringView& text, MigrationJournalState& state)
{
    if (text == TEXT("Planned"))
        state = MigrationJournalState::Planned;
    else if (text == TEXT("BackedUp"))
        state = MigrationJournalState::BackedUp;
    else if (text == TEXT("Published"))
        state = MigrationJournalState::Published;
    else if (text == TEXT("Committed"))
        state = MigrationJournalState::Committed;
    else if (text == TEXT("RolledBack"))
        state = MigrationJournalState::RolledBack;
    else if (text == TEXT("Failed"))
        state = MigrationJournalState::Failed;
    else if (text == TEXT("None"))
        state = MigrationJournalState::None;
    else
        return true;
    return false;
}

bool MigrationSession::CreatePlan(const Array<MigrationInventoryEntry>& inventory, const Array<Guid>& selected, const StringView& backupRoot, MigrationJournal& journal, AssetPipelineDiagnostic& diagnostic)
{
    if (backupRoot.IsEmpty())
        return Fail(diagnostic, TEXT("Migration plan requires a durable backup root outside Library."));
    journal = MigrationJournal();
    journal.BackupRoot = backupRoot;
    journal.Selected = selected;
    if (journal.Selected.Count() > 1)
    {
        std::sort(journal.Selected.Get(), journal.Selected.Get() + journal.Selected.Count(), [](const Guid& a, const Guid& b)
        {
            return GuidKey(a) < GuidKey(b);
        });
    }
    for (const Guid& id : journal.Selected)
    {
        const MigrationInventoryEntry* entry = FindEntry(inventory, id);
        if (entry == nullptr)
            return Fail(diagnostic, TEXT("Migration plan references an asset that is not in the inventory."));
        if (entry->Eligibility != TEXT("ReadyToMigrate"))
            return Fail(diagnostic, TEXT("Migration plan cannot include unresolved or unsupported assets."));
        MigrationJournalOperation operation;
        operation.AssetID = entry->ID;
        operation.Kind = TEXT("GraphDocument");
        operation.SourcePath = entry->SourcePath;
        operation.DestinationPath = entry->ProposedDestination;
        operation.BackupPath = String(backupRoot) / String(GuidKey(entry->ID)) / String(StringUtils::GetFileName(entry->SourcePath));
        operation.State = GetStateName(MigrationJournalState::Planned);
        journal.Operations.Add(operation);
    }
    if (MigrationInventory::Fingerprint(inventory, journal.Selected, backupRoot, journal.PlanFingerprint, diagnostic))
        return true;
    journal.State = GetStateName(MigrationJournalState::Planned);
    diagnostic = AssetPipelineDiagnostic();
    return false;
}

bool MigrationSession::WriteCanonicalJson(const MigrationJournal& journal, StringAnsi& output, AssetPipelineDiagnostic& diagnostic)
{
    JsonDocument json;
    json.SetObject();
    JsonAlloc& allocator = json.GetAllocator();
    json.AddMember("formatVersion", journal.FormatVersion, allocator);
    AddAnsi(json, "planFingerprint", journal.PlanFingerprint, allocator);
    AddString(json, "backupRoot", journal.BackupRoot, allocator);
    AddString(json, "state", journal.State, allocator);
    JsonValue selected(rapidjson::kArrayType);
    for (const Guid& id : journal.Selected)
    {
        const StringAnsi key = GuidKey(id);
        selected.PushBack(JsonValue(key.Get(), key.Length(), allocator), allocator);
    }
    json.AddMember("selected", selected, allocator);
    JsonValue operations(rapidjson::kArrayType);
    for (const MigrationJournalOperation& operation : journal.Operations)
    {
        JsonValue item(rapidjson::kObjectType);
        const StringAnsi id = GuidKey(operation.AssetID);
        item.AddMember("id", JsonValue(id.Get(), id.Length(), allocator), allocator);
        AddString(item, "kind", operation.Kind, allocator);
        AddString(item, "sourcePath", operation.SourcePath, allocator);
        AddString(item, "destinationPath", operation.DestinationPath, allocator);
        AddString(item, "backupPath", operation.BackupPath, allocator);
        AddString(item, "beforeHash", operation.BeforeHash, allocator);
        AddString(item, "afterHash", operation.AfterHash, allocator);
        AddString(item, "state", operation.State, allocator);
        operations.PushBack(item, allocator);
    }
    json.AddMember("operations", operations, allocator);
    CanonicalJsonError error;
    Array<StringAnsi> rootOrder;
    rootOrder.Add("formatVersion");
    rootOrder.Add("planFingerprint");
    rootOrder.Add("backupRoot");
    rootOrder.Add("state");
    rootOrder.Add("selected");
    rootOrder.Add("operations");
    if (CanonicalJsonWriter::Write(json, output, error, &rootOrder))
        return Fail(diagnostic, TEXT("Migration journal canonical serialization failed."));
    diagnostic = AssetPipelineDiagnostic();
    return false;
}

bool MigrationSession::ParseCanonicalJson(const StringAnsiView& json, MigrationJournal& journal, AssetPipelineDiagnostic& diagnostic)
{
    journal = MigrationJournal();
    JsonDocument document;
    document.Parse(json.Get(), json.Length());
    if (document.HasParseError() || !document.IsObject())
        return Fail(diagnostic, TEXT("Migration journal is malformed."));
    const auto* version = document.FindMember("formatVersion");
    if (version == document.MemberEnd() || !version->value.IsInt() || version->value.GetInt() != MigrationInventory::FormatVersion)
        return Fail(diagnostic, TEXT("Migration journal format is unsupported."));
    String fingerprint;
    if (ReadString(document, "planFingerprint", fingerprint) || ReadString(document, "backupRoot", journal.BackupRoot) || ReadString(document, "state", journal.State))
        return Fail(diagnostic, TEXT("Migration journal is missing required fields."));
    journal.PlanFingerprint = StringAnsi(fingerprint);
    MigrationJournalState parsed;
    if (ParseState(journal.State, parsed) || parsed == MigrationJournalState::None)
        return Fail(diagnostic, TEXT("Migration journal state is contradictory."));
    const auto* selected = document.FindMember("selected");
    const auto* operations = document.FindMember("operations");
    if (selected == document.MemberEnd() || !selected->value.IsArray() || operations == document.MemberEnd() || !operations->value.IsArray())
        return Fail(diagnostic, TEXT("Migration journal is missing selected operations."));
    for (const JsonValue& value : selected->value.GetArray())
    {
        Guid id;
        if (!value.IsString() || Guid::Parse(StringAnsiView(value.GetString(), value.GetStringLength()), id) || !id.IsValid())
            return Fail(diagnostic, TEXT("Migration journal selected GUID is invalid."));
        journal.Selected.Add(id);
    }
    for (const JsonValue& value : operations->value.GetArray())
    {
        if (!value.IsObject())
            return Fail(diagnostic, TEXT("Migration journal operation is invalid."));
        MigrationJournalOperation operation;
        String idText;
        if (ReadString(value, "id", idText) || Guid::Parse(idText, operation.AssetID) || !operation.AssetID.IsValid())
            return Fail(diagnostic, TEXT("Migration journal operation GUID is invalid."));
        if (ReadString(value, "kind", operation.Kind) || ReadString(value, "sourcePath", operation.SourcePath) ||
            ReadString(value, "destinationPath", operation.DestinationPath) || ReadString(value, "backupPath", operation.BackupPath) ||
            ReadString(value, "beforeHash", operation.BeforeHash) || ReadString(value, "afterHash", operation.AfterHash) ||
            ReadString(value, "state", operation.State))
            return Fail(diagnostic, TEXT("Migration journal operation is missing required fields."));
        journal.Operations.Add(operation);
    }
    diagnostic = AssetPipelineDiagnostic();
    return false;
}

bool MigrationSession::SaveAtomic(const StringView& path, const MigrationJournal& journal, AssetPipelineDiagnostic& diagnostic)
{
    StringAnsi json;
    if (WriteCanonicalJson(journal, json, diagnostic))
        return true;
    const String destination(path);
    const String staging = destination + TEXT(".tmp");
    if (EnsureDirectory(destination) || File::WriteAllBytes(staging, json.Get(), json.Length()) || FileSystem::MoveFile(destination, staging, true))
    {
        FileSystem::DeleteFile(staging);
        return Fail(diagnostic, TEXT("Migration journal could not be written atomically."));
    }
    diagnostic = AssetPipelineDiagnostic();
    return false;
}

bool MigrationSession::Load(const StringView& path, MigrationJournal& journal, AssetPipelineDiagnostic& diagnostic)
{
    Array<byte> bytes;
    if (File::ReadAllBytes(path, bytes))
        return Fail(diagnostic, TEXT("Migration journal could not be read."));
    return ParseCanonicalJson(StringAnsiView(reinterpret_cast<const char*>(bytes.Get()), bytes.Count()), journal, diagnostic);
}

bool MigrationSession::EnsureCurrentFingerprint(const Array<MigrationInventoryEntry>& inventory, const MigrationJournal& journal, AssetPipelineDiagnostic& diagnostic)
{
    StringAnsi current;
    if (MigrationInventory::Fingerprint(inventory, journal.Selected, journal.BackupRoot, current, diagnostic))
        return true;
    if (current != journal.PlanFingerprint)
        return Fail(diagnostic, TEXT("Migration plan is stale because inventory, selection, or backup root changed."));
    diagnostic = AssetPipelineDiagnostic();
    return false;
}

bool MigrationSession::Backup(MigrationJournal& journal, AssetPipelineDiagnostic& diagnostic)
{
    MigrationJournalState state;
    if (ParseState(journal.State, state) || (state != MigrationJournalState::Planned && state != MigrationJournalState::BackedUp && state != MigrationJournalState::Failed))
        return Fail(diagnostic, TEXT("Migration backup is not valid from the current journal state."));
    for (MigrationJournalOperation& operation : journal.Operations)
    {
        if (!FileSystem::FileExists(operation.SourcePath))
            return Fail(diagnostic, TEXT("Migration backup source is missing."));
        if (HashFile(operation.SourcePath, operation.BeforeHash, diagnostic))
            return true;
        if (FileSystem::FileExists(operation.BackupPath))
        {
            String existing;
            if (HashFile(operation.BackupPath, existing, diagnostic))
                return true;
            if (existing != operation.BeforeHash)
                return Fail(diagnostic, TEXT("Existing migration backup does not match the current source."));
        }
        else
        {
            if (EnsureDirectory(operation.BackupPath) || FileSystem::CopyFile(operation.BackupPath, operation.SourcePath))
                return Fail(diagnostic, TEXT("Migration backup copy failed."));
            String copied;
            if (HashFile(operation.BackupPath, copied, diagnostic) || copied != operation.BeforeHash)
                return Fail(diagnostic, TEXT("Migration backup hash does not match the source."));
        }
        operation.State = GetStateName(MigrationJournalState::BackedUp);
    }
    journal.State = GetStateName(MigrationJournalState::BackedUp);
    diagnostic = AssetPipelineDiagnostic();
    return false;
}

bool MigrationSession::Publish(MigrationJournal& journal, AssetPipelineDiagnostic& diagnostic)
{
    MigrationJournalState state;
    if (ParseState(journal.State, state) || state != MigrationJournalState::BackedUp)
        return Fail(diagnostic, TEXT("Migration publish requires a verified backup."));
    for (MigrationJournalOperation& operation : journal.Operations)
    {
        if (!FileSystem::FileExists(operation.BackupPath))
            return Fail(diagnostic, TEXT("Migration publish refused because the backup is missing."));
        Array<byte> payload;
        StringAnsi marker("canonical-migrated:");
        marker += GuidKey(operation.AssetID);
        marker += "\n";
        payload.Set(reinterpret_cast<const byte*>(marker.Get()), marker.Length());
        if (EnsureDirectory(operation.DestinationPath) || File::WriteAllBytes(operation.DestinationPath, payload.Get(), payload.Count()))
            return Fail(diagnostic, TEXT("Migration publish could not write the canonical destination."));
        if (HashFile(operation.DestinationPath, operation.AfterHash, diagnostic))
            return true;
        if (FileSystem::FileExists(operation.SourcePath) && FileSystem::DeleteFile(operation.SourcePath))
            return Fail(diagnostic, TEXT("Migration publish could not retire the legacy source."));
        operation.State = GetStateName(MigrationJournalState::Published);
    }
    journal.State = GetStateName(MigrationJournalState::Published);
    diagnostic = AssetPipelineDiagnostic();
    return false;
}

bool MigrationSession::Commit(MigrationJournal& journal, AssetPipelineDiagnostic& diagnostic)
{
    MigrationJournalState state;
    if (ParseState(journal.State, state) || state != MigrationJournalState::Published)
        return Fail(diagnostic, TEXT("Migration commit is illegal before publish and verification."));
    for (const MigrationJournalOperation& operation : journal.Operations)
    {
        if (operation.AfterHash.IsEmpty() || !FileSystem::FileExists(operation.DestinationPath))
            return Fail(diagnostic, TEXT("Migration commit refused because published output is missing."));
        String current;
        if (HashFile(operation.DestinationPath, current, diagnostic) || current != operation.AfterHash)
            return Fail(diagnostic, TEXT("Migration commit refused because published output changed."));
        if (FileSystem::FileExists(operation.SourcePath))
            return Fail(diagnostic, TEXT("Migration commit refused because the legacy source is still present."));
    }
    journal.State = GetStateName(MigrationJournalState::Committed);
    for (MigrationJournalOperation& operation : journal.Operations)
        operation.State = journal.State;
    diagnostic = AssetPipelineDiagnostic();
    return false;
}

bool MigrationSession::Rollback(MigrationJournal& journal, AssetPipelineDiagnostic& diagnostic)
{
    MigrationJournalState state;
    if (ParseState(journal.State, state) || state == MigrationJournalState::None || state == MigrationJournalState::RolledBack)
        return Fail(diagnostic, TEXT("Migration rollback is not valid from the current journal state."));
    for (MigrationJournalOperation& operation : journal.Operations)
    {
        if (FileSystem::FileExists(operation.DestinationPath))
        {
            String current;
            if (HashFile(operation.DestinationPath, current, diagnostic))
                return true;
            if (operation.AfterHash.HasChars() && current != operation.AfterHash)
                return Fail(diagnostic, TEXT("Migration rollback refused because the destination was edited after migration."));
            if (FileSystem::DeleteFile(operation.DestinationPath))
                return Fail(diagnostic, TEXT("Migration rollback could not remove the published destination."));
        }
        if (!FileSystem::FileExists(operation.BackupPath))
            return Fail(diagnostic, TEXT("Migration rollback cannot restore because the backup is missing."));
        if (EnsureDirectory(operation.SourcePath) || FileSystem::CopyFile(operation.SourcePath, operation.BackupPath))
            return Fail(diagnostic, TEXT("Migration rollback could not restore the legacy source."));
        String restored;
        if (HashFile(operation.SourcePath, restored, diagnostic))
            return true;
        if (operation.BeforeHash.HasChars() && restored != operation.BeforeHash)
            return Fail(diagnostic, TEXT("Migration rollback restored a source that does not match the backup hash."));
        operation.State = GetStateName(MigrationJournalState::RolledBack);
    }
    journal.State = GetStateName(MigrationJournalState::RolledBack);
    diagnostic = AssetPipelineDiagnostic();
    return false;
}
