// Copyright (c) Wojciech Figat. All rights reserved.

#include "Engine/Content/AssetDatabase/MigrationInventory.h"
#include "Engine/Content/AssetDatabase/MigrationJournal.h"
#include "Engine/Content/AssetDatabase/LegacyAssetMigrator.h"
#include "Engine/Content/AssetDatabase/AssetMeta.h"
#include "Engine/Content/AssetDatabase/AssetDatabaseFacade.h"
#include "Engine/Content/Documents/GraphDocument.h"
#include "Engine/Content/Documents/MaterialInstanceDocument.h"
#include "Engine/Content/Documents/SceneAnimationDocument.h"
#include "Engine/Content/Assets/Material.h"
#include "Engine/Content/Assets/MaterialInstance.h"
#include "Engine/Content/Assets/Model.h"
#include "Engine/Content/Assets/Animation.h"
#include "Engine/Content/Assets/Shader.h"
#include "Engine/Animations/SceneAnimations/SceneAnimation.h"
#include "Engine/Content/Storage/FlaxStorage.h"
#include "Engine/Engine/Globals.h"
#include "Engine/Graphics/Shaders/Cache/ShaderStorage.h"
#include "Engine/Graphics/Materials/MaterialParams.h"
#include "Engine/Platform/File.h"
#include "Engine/Platform/FileSystem.h"
#include "Engine/Platform/Platform.h"
#include "Engine/Serialization/MemoryWriteStream.h"
#include "Engine/Serialization/MemoryReadStream.h"
#include "Engine/Utilities/Encryption.h"
#include <ThirdParty/catch2/catch.hpp>

namespace
{
    AssetRecord MakeRecord(const Guid& id, const String& typeName, const String& path, AssetSourceKind kind, AssetRecordStatus status = AssetRecordStatus::Ready)
    {
        AssetRecord record;
        record.ID = id;
        record.SourceAssetID = id;
        record.TypeName = typeName;
        record.CanonicalPath = CanonicalAssetPath(path);
        record.SourcePath = SourceFilePath(path);
        record.SourceKind = kind;
        record.Status = status;
        return record;
    }

    bool WriteStarterFlax(const StringView& path, const Guid& id, const StringView& typeName)
    {
        GraphDocument document;
        AssetPipelineDiagnostic diagnostic;
        if (GraphDocumentCodec::CreateStarter(typeName, document, diagnostic))
            return true;
        Array<byte> surface;
        if (GraphDocumentCompiler::CompileDocument(document, surface, diagnostic))
            return true;
        FlaxChunk surfaceChunk;
        surfaceChunk.Data.Copy(surface.Get(), surface.Count());
        AssetInitData data;
        data.Header.ID = id;
        data.Header.TypeName = typeName;
        if (typeName == Material::TypeName)
        {
            data.SerializedVersion = 20;
            data.Header.Chunks[SHADER_FILE_CHUNK_VISJECT_SURFACE] = &surfaceChunk;
            ShaderStorage::Header20 shaderHeader;
            Platform::MemoryClear(&shaderHeader, sizeof(shaderHeader));
            data.CustomData.Copy(&shaderHeader);
        }
        else
        {
            data.SerializedVersion = 1;
            data.Header.Chunks[0] = &surfaceChunk;
        }
        return FlaxStorage::Create(path, data);
    }
}

TEST_CASE("Migration inventory is read-only, deterministic, and classifies mixed mode")
{
    Array<AssetRecord> records;
    records.Add(MakeRecord(Guid(2, 0, 0, 0), TEXT("FlaxEngine.Texture"), TEXT("Content/Tex.flax"), AssetSourceKind::LegacyBinary));
    records.Add(MakeRecord(Guid(1, 0, 0, 0), TEXT("FlaxEngine.Material"), TEXT("Content/Mat.flax"), AssetSourceKind::LegacyBinary));
    records.Add(MakeRecord(Guid(3, 0, 0, 0), TEXT("FlaxEngine.Material"), TEXT("Content/Done.material"), AssetSourceKind::TextDocument));
    records.Add(MakeRecord(Guid(4, 0, 0, 0), TEXT("FlaxEngine.Scene"), TEXT("Content/Level.scene"), AssetSourceKind::LegacyBinary));
    records.Add(MakeRecord(Guid(5, 0, 0, 0), TEXT("FlaxEngine.Material"), TEXT("Content/Dup.flax"), AssetSourceKind::LegacyBinary, AssetRecordStatus::DuplicateGuid));
    records.Add(MakeRecord(Guid(6, 0, 0, 0), TEXT("FlaxEngine.MaterialInstance"), TEXT("Content/Inst.flax"), AssetSourceKind::LegacyBinary));
    records.Add(MakeRecord(Guid(7, 0, 0, 0), TEXT("FlaxEngine.Shader"), TEXT("Content/Lit.flax"), AssetSourceKind::LegacyBinary));

    Array<MigrationInventoryEntry> entries;
    MigrationInventory::Build(records, entries);
    REQUIRE(entries.Count() == 7);
    CHECK(entries[0].ID == Guid(1, 0, 0, 0));
    CHECK(entries[0].Eligibility == TEXT("ReadyToMigrate"));
    CHECK(entries[0].ProposedDestination.EndsWith(TEXT(".material")));
    CHECK(entries[1].Eligibility == TEXT("MissingOriginalSource"));
    CHECK(entries[2].Eligibility == TEXT("AlreadyMigrated"));
    CHECK(entries[3].Eligibility == TEXT("Unsupported"));
    CHECK(entries[4].Eligibility == TEXT("Conflict"));
    CHECK(entries[5].Eligibility == TEXT("ReadyToMigrate"));
    CHECK(entries[5].ProposedDestination.EndsWith(TEXT(".materialinstance")));
    CHECK(entries[6].Eligibility == TEXT("ReadyToMigrate"));
    CHECK(entries[6].ProposedDestination.EndsWith(TEXT(".shader")));
    CHECK(MigrationInventory::HasBlockingConflict(entries));

    AssetPipelineDiagnostic diagnostic;
    StringAnsi json;
    REQUIRE_FALSE(MigrationInventory::WriteCanonicalJson(entries, json, diagnostic));
    StringAnsi again;
    REQUIRE_FALSE(MigrationInventory::WriteCanonicalJson(entries, again, diagnostic));
    CHECK(json == again);
    CHECK(json.Contains("ReadyToMigrate"));
    CHECK_FALSE(json.Contains("Library"));
}

TEST_CASE("Migration journal can dry-run, resume, commit, and hash-safe roll back")
{
    const String root = Globals::TemporaryFolder / (TEXT("MigrationSession-") + Guid::New().ToString(Guid::FormatType::N));
    const String content = root / TEXT("Content");
    const String backup = root / TEXT("Backup");
    REQUIRE_FALSE(FileSystem::CreateDirectory(content));
    const String source = content / TEXT("Mat.flax");
    const Guid id(1, 0, 0, 0);
    REQUIRE_FALSE(WriteStarterFlax(source, id, Material::TypeName));
    Array<byte> original;
    REQUIRE_FALSE(File::ReadAllBytes(source, original));

    Array<AssetRecord> records;
    records.Add(MakeRecord(id, TEXT("FlaxEngine.Material"), source, AssetSourceKind::LegacyBinary));
    Array<MigrationInventoryEntry> inventory;
    MigrationInventory::Build(records, inventory);

    AssetPipelineDiagnostic diagnostic;
    Array<Guid> selected;
    selected.Add(id);
    MigrationJournal journal;
    REQUIRE_FALSE(MigrationSession::CreatePlan(inventory, selected, backup, journal, diagnostic));
    CHECK(journal.State == TEXT("Planned"));
    CHECK_FALSE(FileSystem::FileExists(journal.Operations[0].BackupPath));
    CHECK(FileSystem::FileExists(source));

    CHECK(MigrationSession::Commit(journal, diagnostic));
    CHECK(diagnostic.Code == AssetPipelineDiagnosticCode::MigrationFailed);

    REQUIRE_FALSE(MigrationSession::Backup(journal, diagnostic));
    CHECK(journal.State == TEXT("BackedUp"));
    CHECK(FileSystem::FileExists(journal.Operations[0].BackupPath));
    REQUIRE_FALSE(MigrationSession::Backup(journal, diagnostic));

    REQUIRE_FALSE(MigrationSession::Publish(journal, diagnostic));
    CHECK(journal.State == TEXT("Published"));
    CHECK_FALSE(FileSystem::FileExists(source));
    CHECK(FileSystem::FileExists(journal.Operations[0].DestinationPath));
    CHECK(FileSystem::FileExists(journal.Operations[0].DestinationPath + TEXT(".meta")));
    StringAnsi published;
    REQUIRE_FALSE(File::ReadAllText(journal.Operations[0].DestinationPath, published));
    CHECK(published.Contains("documentVersion"));
    CHECK_FALSE(published.Contains("canonical-migrated"));
    AssetMeta meta;
    REQUIRE_FALSE(AssetMeta::Load(journal.Operations[0].DestinationPath + TEXT(".meta"), meta, diagnostic));
    CHECK(meta.ID == id);
    CHECK(meta.Processor.ID == TEXT("Flax.GraphDocument"));

    const String journalPath = root / TEXT("journal.json");
    REQUIRE_FALSE(MigrationSession::SaveAtomic(journalPath, journal, diagnostic));
    MigrationJournal loaded;
    REQUIRE_FALSE(MigrationSession::Load(journalPath, loaded, diagnostic));
    CHECK(loaded.PlanFingerprint == journal.PlanFingerprint);
    REQUIRE_FALSE(MigrationSession::Commit(loaded, diagnostic));
    CHECK(loaded.State == TEXT("Committed"));

    records[0].SourcePath = SourceFilePath(source + TEXT(".changed"));
    MigrationInventory::Build(records, inventory);
    CHECK(MigrationSession::EnsureCurrentFingerprint(inventory, loaded, diagnostic));

    REQUIRE_FALSE(MigrationSession::Rollback(loaded, diagnostic));
    CHECK(loaded.State == TEXT("RolledBack"));
    CHECK(FileSystem::FileExists(source));
    CHECK_FALSE(FileSystem::FileExists(loaded.Operations[0].DestinationPath));
    Array<byte> restored;
    REQUIRE_FALSE(File::ReadAllBytes(source, restored));
    REQUIRE(restored.Count() == original.Count());
    CHECK(restored[0] == original[0]);

    FileSystem::DeleteDirectory(root, true);
}

TEST_CASE("Migration rollback refuses post-migration edits and corrupt journals")
{
    const String root = Globals::TemporaryFolder / (TEXT("MigrationConflict-") + Guid::New().ToString(Guid::FormatType::N));
    const String content = root / TEXT("Content");
    const String backup = root / TEXT("Backup");
    REQUIRE_FALSE(FileSystem::CreateDirectory(content));
    const String source = content / TEXT("Mat.flax");
    const Guid id(8, 0, 0, 0);
    REQUIRE_FALSE(WriteStarterFlax(source, id, TEXT("FlaxEngine.VisualScript")));

    Array<AssetRecord> records;
    records.Add(MakeRecord(id, TEXT("FlaxEngine.VisualScript"), source, AssetSourceKind::LegacyBinary));
    Array<MigrationInventoryEntry> inventory;
    MigrationInventory::Build(records, inventory);
    CHECK(inventory[0].ProposedDestination.EndsWith(TEXT(".visualscript")));

    AssetPipelineDiagnostic diagnostic;
    Array<Guid> selected;
    selected.Add(id);
    MigrationJournal journal;
    REQUIRE_FALSE(MigrationSession::CreatePlan(inventory, selected, backup, journal, diagnostic));
    REQUIRE_FALSE(MigrationSession::Backup(journal, diagnostic));
    REQUIRE_FALSE(MigrationSession::Publish(journal, diagnostic));

    const byte edited[] = { 9, 9, 9 };
    REQUIRE_FALSE(File::WriteAllBytes(journal.Operations[0].DestinationPath, edited, ARRAY_COUNT(edited)));
    CHECK(MigrationSession::Rollback(journal, diagnostic));
    CHECK(diagnostic.Message.Contains(TEXT("edited")));
    CHECK(FileSystem::FileExists(journal.Operations[0].DestinationPath));

    CHECK(MigrationSession::ParseCanonicalJson("{\"formatVersion\":1}", journal, diagnostic));
    CHECK(diagnostic.Code == AssetPipelineDiagnosticCode::MigrationFailed);

    const String staging = root / TEXT("journal.json.tmp");
    REQUIRE_FALSE(File::WriteAllBytes(staging, edited, ARRAY_COUNT(edited)));
    CHECK(MigrationSession::Load(staging, journal, diagnostic));
    CHECK(diagnostic.Message.Contains(TEXT("staging")));

    FileSystem::DeleteDirectory(root, true);
}

TEST_CASE("Existing JSON scenes get identity sidecars without rewriting document bytes")
{
    const String root = Globals::TemporaryFolder / (TEXT("JsonSidecar-") + Guid::New().ToString(Guid::FormatType::N));
    REQUIRE_FALSE(FileSystem::CreateDirectory(root));
    const String scene = root / TEXT("Level.scene");
    const Guid id(11, 12, 13, 14);
    const StringAnsi json = StringAnsi::Format("{{\n  \"ID\": \"{0}\",\n  \"TypeName\": \"FlaxEngine.Scene\"\n}}\n", StringAnsi(id.ToString(Guid::FormatType::N)));
    REQUIRE_FALSE(File::WriteAllBytes(scene, json.Get(), json.Length()));
    CHECK(AssetDatabaseFacade::CreateExistingJsonMetadata(scene) == id);
    AssetPipelineDiagnostic diagnostic;
    AssetMeta meta;
    REQUIRE_FALSE(AssetMeta::Load(scene + TEXT(".meta"), meta, diagnostic));
    CHECK(meta.ID == id);
    CHECK(meta.SourceKind == AssetSourceKind::ExistingJson);
    CHECK(meta.Processor.ID == TEXT("Flax.ExistingJson"));
    StringAnsi after;
    REQUIRE_FALSE(File::ReadAllText(scene, after));
    CHECK(after == json);
    FileSystem::DeleteDirectory(root, true);
}

TEST_CASE("Model flax packages seed subasset GUIDs into the root sidecar")
{
    const String root = Globals::TemporaryFolder / (TEXT("ModelSeed-") + Guid::New().ToString(Guid::FormatType::N));
    REQUIRE_FALSE(FileSystem::CreateDirectory(root));
    const String flax = root / TEXT("Hero.flaxpac");
    const Guid rootId(21, 22, 23, 24);
    const Guid walkId(31, 32, 33, 34);
    byte dummy = 1;
    FlaxChunk rootChunk;
    rootChunk.Data.Copy(&dummy, 1);
    FlaxChunk walkChunk;
    walkChunk.Data.Copy(&dummy, 1);
    AssetInitData rootAsset;
    rootAsset.Header.ID = rootId;
    rootAsset.Header.TypeName = Model::TypeName;
    rootAsset.SerializedVersion = 1;
    rootAsset.Header.Chunks[0] = &rootChunk;
    AssetInitData walkAsset;
    walkAsset.Header.ID = walkId;
    walkAsset.Header.TypeName = Animation::TypeName;
    walkAsset.SerializedVersion = 1;
    walkAsset.Header.Chunks[0] = &walkChunk;
    Array<AssetInitData> assets;
    assets.Add(rootAsset);
    assets.Add(walkAsset);
    REQUIRE_FALSE(FlaxStorage::Create(flax, assets));
    AssetMeta meta;
    AssetPipelineDiagnostic diagnostic;
    REQUIRE_FALSE(LegacyAssetMigrator::SeedModelSubAssets(flax, meta, diagnostic));
    CHECK(meta.ID == rootId);
    CHECK(meta.Processor.ID == TEXT("Flax.Model"));
    REQUIRE(meta.SubAssets.ContainsKey(TEXT("animation:Hero")));
    CHECK(meta.SubAssets[TEXT("animation:Hero")].ID == walkId);
    FileSystem::DeleteDirectory(root, true);
}

TEST_CASE("Legacy shader flax converts to canonical source text")
{
    const String root = Globals::TemporaryFolder / (TEXT("ShaderConvert-") + Guid::New().ToString(Guid::FormatType::N));
    REQUIRE_FALSE(FileSystem::CreateDirectory(root));
    const String flax = root / TEXT("Lit.flax");
    const String shader = root / TEXT("Lit.shader");
    const Guid id(41, 42, 43, 44);
    const char source[] = "META\n{\n}\nfloat4 PS() { return 1; }\n";
    const int32 sourceLength = ARRAY_COUNT(source) - 1;
    Array<byte> encrypted;
    encrypted.Set(reinterpret_cast<const byte*>(source), sourceLength);
    encrypted.Add(0);
    Encryption::EncryptBytes(encrypted.Get(), sourceLength);
    FlaxChunk chunk;
    chunk.Data.Copy(encrypted.Get(), encrypted.Count());
    AssetInitData data;
    data.Header.ID = id;
    data.Header.TypeName = Shader::TypeName;
    data.SerializedVersion = 20;
    data.Header.Chunks[SHADER_FILE_CHUNK_SOURCE] = &chunk;
    REQUIRE_FALSE(FlaxStorage::Create(flax, data));
    AssetPipelineDiagnostic diagnostic;
    REQUIRE_FALSE(LegacyAssetMigrator::ConvertFlax(flax, shader, id, Shader::TypeName, diagnostic));
    StringAnsi published;
    REQUIRE_FALSE(File::ReadAllText(shader, published));
    CHECK(published.Contains("float4 PS()"));
    AssetMeta meta;
    REQUIRE_FALSE(AssetMeta::Load(shader + TEXT(".meta"), meta, diagnostic));
    CHECK(meta.ID == id);
    CHECK(meta.Processor.ID == TEXT("Flax.ShaderSource"));
    FileSystem::DeleteDirectory(root, true);
}

TEST_CASE("Legacy material instance flax converts to authored JSON")
{
    const String root = Globals::TemporaryFolder / (TEXT("InstanceConvert-") + Guid::New().ToString(Guid::FormatType::N));
    REQUIRE_FALSE(FileSystem::CreateDirectory(root));
    const String flax = root / TEXT("Inst.flax");
    const String document = root / TEXT("Inst.materialinstance");
    const Guid id(51, 52, 53, 54);
    const Guid baseId(61, 62, 63, 64);
    const Guid parameterId(71, 72, 73, 74);
    SerializedMaterialParam parameter;
    parameter.Type = MaterialParameterType::Float;
    parameter.ID = parameterId;
    parameter.IsPublic = true;
    parameter.Override = true;
    parameter.Name = TEXT("Roughness");
    parameter.ShaderName.Clear();
    parameter.AsFloat = 0.8f;
    parameter.RegisterIndex = 0;
    parameter.Offset = 0;
    Array<SerializedMaterialParam> parameters;
    parameters.Add(parameter);
    MemoryWriteStream chunkStream;
    chunkStream.Write(baseId);
    MaterialParams::Save(&chunkStream, &parameters);
    FlaxChunk chunk;
    chunk.Data.Copy(chunkStream.GetHandle(), static_cast<int32>(chunkStream.GetPosition()));
    AssetInitData data;
    data.Header.ID = id;
    data.Header.TypeName = MaterialInstance::TypeName;
    data.SerializedVersion = 4;
    data.Header.Chunks[0] = &chunk;
    REQUIRE_FALSE(FlaxStorage::Create(flax, data));
    AssetPipelineDiagnostic diagnostic;
    REQUIRE_FALSE(LegacyAssetMigrator::ConvertFlax(flax, document, id, MaterialInstance::TypeName, diagnostic));
    StringAnsi published;
    REQUIRE_FALSE(File::ReadAllText(document, published));
    CHECK(published.Contains("documentVersion"));
    CHECK(published.Contains("baseMaterial"));
    CHECK(published.Contains("\"overrides\": {"));
    CHECK(published.Contains("\"type\": \"Float\""));
    CHECK(published.Contains("\"value\": 0.8"));
    CHECK_FALSE(published.Contains("parameters"));
    rapidjson_flax::Document parsed;
    parsed.Parse(published.Get(), published.Length());
    REQUIRE_FALSE(parsed.HasParseError());
    Array<byte> compiled;
    String error;
    REQUIRE_FALSE(MaterialInstanceDocument::Compile(parsed, compiled, nullptr, error));
    MemoryReadStream compiledStream(compiled.Get(), compiled.Count());
    Guid compiledBase;
    compiledStream.Read(compiledBase);
    CHECK(compiledBase == baseId);
    MaterialParams compiledParameters;
    REQUIRE_FALSE(compiledParameters.Load(&compiledStream));
    REQUIRE(compiledParameters.Count() == 1);
    CHECK(compiledParameters[0].GetParameterID() == parameterId);
    CHECK(compiledParameters[0].IsOverride());
    CHECK(static_cast<float>(compiledParameters[0].GetValue()) == Approx(0.8f));
    AssetMeta meta;
    REQUIRE_FALSE(AssetMeta::Load(document + TEXT(".meta"), meta, diagnostic));
    CHECK(meta.ID == id);
    CHECK(meta.Processor.ID == TEXT("Flax.MaterialInstance"));
    FileSystem::DeleteDirectory(root, true);
}

TEST_CASE("Legacy scene animation flax converts to semantic authored JSON")
{
    const String root = Globals::TemporaryFolder / (TEXT("SceneAnimationConvert-") + Guid::New().ToString(Guid::FormatType::N));
    REQUIRE_FALSE(FileSystem::CreateDirectory(root));
    const String flax = root / TEXT("Cutscene.flax");
    const String document = root / TEXT("Cutscene.sceneanimation");
    const Guid id(81, 82, 83, 84);
    MemoryWriteStream timeline;
    timeline.WriteInt32(4);
    timeline.WriteFloat(30.0f);
    timeline.WriteInt32(60);
    timeline.WriteInt32(1);
    timeline.WriteByte(static_cast<byte>(SceneAnimation::Track::Types::Folder));
    timeline.WriteByte(0);
    timeline.WriteInt32(-1);
    timeline.WriteInt32(0);
    timeline.Write(TEXT("Root"), -13);
    timeline.Write(Color32::White);
    FlaxChunk chunk;
    chunk.Data.Copy(timeline.GetHandle(), static_cast<int32>(timeline.GetPosition()));
    AssetInitData data;
    data.Header.ID = id;
    data.Header.TypeName = SceneAnimation::TypeName;
    data.SerializedVersion = 1;
    data.Header.Chunks[0] = &chunk;
    REQUIRE_FALSE(FlaxStorage::Create(flax, data));
    AssetPipelineDiagnostic diagnostic;
    REQUIRE_FALSE(LegacyAssetMigrator::ConvertFlax(flax, document, id, SceneAnimation::TypeName, diagnostic));
    StringAnsi published;
    REQUIRE_FALSE(File::ReadAllText(document, published));
    CHECK(published.Contains("\"framesPerSecond\": 30.0"));
    CHECK(published.Contains("\"durationFrames\": 60"));
    CHECK(published.Contains("\"tracks\": ["));
    CHECK(published.Contains("\"type\": \"Folder\""));
    CHECK_FALSE(published.Contains("\"timeline\""));
    rapidjson_flax::Document parsed;
    parsed.Parse(published.Get(), published.Length());
    REQUIRE_FALSE(parsed.HasParseError());
    Array<byte> compiled;
    String error;
    REQUIRE_FALSE(SceneAnimationDocument::Compile(parsed, compiled, nullptr, error));
    MemoryReadStream compiledStream(compiled.Get(), compiled.Count());
    int32 compiledVersion;
    compiledStream.ReadInt32(&compiledVersion);
    CHECK(compiledVersion == 5);
    FileSystem::DeleteDirectory(root, true);
}
