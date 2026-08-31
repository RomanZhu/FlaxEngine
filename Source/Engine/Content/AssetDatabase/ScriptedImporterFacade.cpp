// Copyright (c) Wojciech Figat. All rights reserved.

#include "ScriptedImporterFacade.h"

#if USE_EDITOR && COMPILE_WITH_ASSETS_IMPORTER

#include "AssetDatabase.h"
#include "AssetDatabaseFacade.h"
#include "AssetDatabaseStorage.h"
#include "AssetMeta.h"
#include "AssetMount.h"
#include "SubAssetReconciler.h"
#include "Engine/Content/Artifacts/ArtifactManifest.h"
#include "Engine/Content/Artifacts/ArtifactPublisher.h"
#include "Engine/Content/Artifacts/ArtifactStore.h"
#include "Engine/Content/Build/ArtifactBuildContext.h"
#include "Engine/Content/Build/AssetProcessor.h"
#include "Engine/Content/Build/PrepareAssetContext.h"
#include "Engine/Content/Build/Processors/TexturePipelineService.h"
#include "Engine/Content/BinaryAsset.h"
#include "Engine/Content/Content.h"
#include "Engine/Content/Storage/ContentStorageManager.h"
#include "Engine/Content/Storage/FlaxStorage.h"
#include "Engine/Core/ScopeExit.h"
#include "Engine/Engine/Globals.h"
#include "Engine/Engine/Engine.h"
#include "Engine/Platform/File.h"
#include "Engine/Platform/FileSystem.h"
#include "Engine/Scripting/Scripting.h"
#include "Engine/Serialization/Json.h"
#include "Engine/Serialization/JsonWriters.h"
#include "Engine/Utilities/Encryption.h"
#include "FlaxEngine.Gen.h"
#include <algorithm>

namespace
{
    struct ManagedOutput
    {
        String Identifier;
        String StableKey;
        String TypeName;
        String DisplayName;
        StringAnsi Format;
        Array<byte> Bytes;
        bool Main = false;
    };

    struct ManagedAuxOutput
    {
        StringAnsi Kind;
        Array<byte> Bytes;
    };

    CriticalSection ErrorLocker;
    String LastError;

    bool Fail(const StringView& message)
    {
        ScopeLock lock(ErrorLocker);
        LastError = message;
        return true;
    }

    bool Fail(const AssetPipelineDiagnostic& diagnostic, const StringView& fallback)
    {
        return Fail(diagnostic.Message.HasChars() ? diagnostic.Message : fallback);
    }

    void ClearError()
    {
        ScopeLock lock(ErrorLocker);
        LastError.Clear();
    }

    String ResolvePath(const StringView& path)
    {
        String value(path);
        value.Replace('\\', '/');
        AssetMountResolution resolution;
        AssetPipelineDiagnostic diagnostic;
        if (FileSystem::IsRelative(value))
        {
            if (AssetMountRegistry::Get().ResolveLogical(value, resolution, diagnostic))
                return String::Empty;
            return resolution.PhysicalPath;
        }
        FileSystem::NormalizePath(value);
        if (AssetMountRegistry::Get().ResolvePhysical(value, resolution, diagnostic))
            return String::Empty;
        return resolution.PhysicalPath;
    }

    bool RefreshSource(const StringView& sourcePath)
    {
        Array<String> paths;
        paths.Add(String(sourcePath));
        return AssetDatabaseFacade::RefreshSources(paths);
    }

    bool ReadString(const rapidjson_flax::Value& object, const char* name, String& value)
    {
        const auto member = object.FindMember(name);
        if (member == object.MemberEnd() || !member->value.IsString())
            return true;
        value = String(member->value.GetStringAnsiView());
        return false;
    }

    bool ReadAnsi(const rapidjson_flax::Value& object, const char* name, StringAnsi& value)
    {
        const auto member = object.FindMember(name);
        if (member == object.MemberEnd() || !member->value.IsString())
            return true;
        value = StringAnsi(member->value.GetString(), member->value.GetStringLength());
        return false;
    }

    bool DecodeBytes(const rapidjson_flax::Value& object, const char* name, Array<byte>& value)
    {
        const auto member = object.FindMember(name);
        if (member == object.MemberEnd() || !member->value.IsString())
            return true;
        const int32 length = member->value.GetStringLength();
        if (length == 0)
        {
            value.Clear();
            return false;
        }
        Encryption::Base64Decode(member->value.GetString(), length, value);
        return value.IsEmpty();
    }

    bool FindRecordByPath(const StringView& path, AssetRecord& result)
    {
        const String resolved = ResolvePath(path);
        if (resolved.IsEmpty())
            return false;
        const AssetDatabaseSnapshot snapshot = AssetDatabase::Get().GetSnapshot();
        for (const AssetRecord& record : snapshot.Records)
        {
            if (record.IsMainAsset() && FileSystem::AreFilePathsEqual(record.SourcePath.Get(), resolved))
            {
                result = record;
                return true;
            }
        }
        return false;
    }

    bool HashFile(const StringView& path, ContentHash& hash)
    {
        Array<byte> bytes;
        if (File::ReadAllBytes(path, bytes))
            return true;
        hash = ContentHash::Compute(bytes.Get(), bytes.Count());
        return false;
    }

    bool ReadCurrentArtifact(const AssetRecord& record, const ArtifactTarget& target, ArtifactKey& key,
        const StringAnsiView& outputKind, ArtifactManifest* resultManifest);
}

static String ReadArtifactOutputInternal(const Guid& sourceAssetId, const StringView& outputKind)
{
    ClearError();
    if (!sourceAssetId.IsValid() || outputKind.IsEmpty())
    {
        Fail(TEXT("Immutable importer artifact reads require a source GUID and output kind."));
        return String::Empty;
    }
    AssetRecord record;
    for (const AssetRecord& candidate : AssetDatabase::Get().GetSnapshot().Records)
    {
        if (candidate.IsMainAsset() && (candidate.SourceAssetID == sourceAssetId || candidate.ID == sourceAssetId))
        {
            record = candidate;
            break;
        }
    }
    if (!record.ID.IsValid())
    {
        Fail(TEXT("The artifact dependency source is not registered."));
        return String::Empty;
    }
    const StringAnsi outputKindAnsi(outputKind);
    ArtifactKey key;
    ArtifactManifest manifest;
    if (ReadCurrentArtifact(record, TexturePipelineService::GetHostTarget(), key, outputKindAnsi, &manifest))
    {
        Fail(TEXT("The requested artifact output is not currently available."));
        return String::Empty;
    }
    const ArtifactManifestOutput* selected = nullptr;
    for (const ArtifactManifestOutput& output : manifest.Outputs)
    {
        if (output.Kind == outputKindAnsi)
        {
            selected = &output;
            break;
        }
    }
    ArtifactStoragePath path;
    AssetPipelineDiagnostic diagnostic;
    Array<byte> bytes;
    ContentHash content;
    if (!selected || ArtifactStore::TryResolveLibraryRelative(Globals::ProjectLibraryFolder, selected->RelativePath, path, diagnostic) ||
        File::ReadAllBytes(path.Get(), bytes) || HashFile(path.Get(), content) || content != selected->Content ||
        static_cast<uint64>(bytes.Count()) != selected->Size)
    {
        Fail(diagnostic, TEXT("The immutable artifact output failed path, size, or content verification."));
        return String::Empty;
    }
    Array<char> encoded;
    Encryption::Base64Encode(bytes.Get(), bytes.Count(), encoded);
    const StringAnsi keyText(key.ToString());
    const StringAnsi contentText(content.ToString());
    rapidjson_flax::StringBuffer buffer;
    CompactJsonWriter writer(buffer);
    writer.StartObject();
    writer.JKEY("artifactKey");
    writer.String(keyText.Get(), keyText.Length());
    writer.JKEY("contentHash");
    writer.String(contentText.Get(), contentText.Length());
    writer.JKEY("outputKind");
    writer.String(outputKindAnsi.Get(), outputKindAnsi.Length());
    writer.JKEY("data");
    writer.String(encoded.Get(), encoded.Count());
    writer.EndObject();
    return String(StringAnsiView(buffer.GetString(), static_cast<int32>(buffer.GetSize())));
}

String ScriptedImporterFacade::ReadArtifactOutput(const Guid& sourceAssetId, const StringView& outputKind)
{
    if (!Engine::GetCommandLine().Contains(TEXT("-assetImportWorker"), StringSearchCase::IgnoreCase))
    {
        ClearError();
        Fail(TEXT("Immutable importer artifact reads are available only inside an import worker."));
        return String::Empty;
    }
    return ReadArtifactOutputInternal(sourceAssetId, outputKind);
}

String ScriptedImporterFacade::ReadArtifactOutputForCoordinator(const Guid& sourceAssetId, const StringView& outputKind)
{
    if (Engine::GetCommandLine().Contains(TEXT("-assetImportWorker"), StringSearchCase::IgnoreCase))
    {
        ClearError();
        Fail(TEXT("Coordinator artifact staging cannot run inside an importer worker."));
        return String::Empty;
    }
    return ReadArtifactOutputInternal(sourceAssetId, outputKind);
}

bool ScriptedImporterFacade::EnsureMetadata(const StringView& sourcePath, const StringView& importerId, int32 settingsSchemaVersion)
{
    const String resolved = ResolvePath(sourcePath);
    if (resolved.IsEmpty() || !FileSystem::FileExists(resolved) || importerId.IsEmpty() || settingsSchemaVersion < 1)
        return Fail(TEXT("Managed importer metadata requires an existing mounted source and a valid importer identity."));
    const String metaPath = resolved + TEXT(".meta");
    const bool metadataExists = FileSystem::FileExists(metaPath);
    AssetPipelineDiagnostic diagnostic;
    AssetMeta meta;
    if (metadataExists)
    {
        if (AssetMeta::Load(metaPath, meta, diagnostic))
            return Fail(diagnostic, TEXT("Managed importer metadata is unreadable."));
        if (meta.Processor.ID != importerId && meta.Processor.ID != TEXT("Flax.Unsupported"))
            return Fail(TEXT("The source is already owned by a different selected importer."));
    }
    else
    {
        meta.ID = Guid::New();
        meta.AssetType = TEXT("FlaxEngine.JsonAsset");
        meta.SourceKind = AssetSourceKind::ImportedSource;
    }
    meta.Processor.ID = importerId;
    meta.Processor.SettingsVersion = settingsSchemaVersion;
    if (meta.Processor.SettingsJson.IsEmpty())
        meta.Processor.SettingsJson = "{}\n";
    if (AssetDatabaseFacade::CommitMetadata(resolved, meta, metadataExists, diagnostic))
        return Fail(diagnostic, TEXT("Managed importer metadata could not be published."));
    ClearError();
    return false;
}

namespace
{
    bool ParseResult(const rapidjson_flax::Document& document, Array<ManagedOutput>& outputs, Array<ManagedAuxOutput>& auxiliaries,
        int32& implementationVersion, ContentHash& providerHash, ContentHash& postprocessorHash)
    {
        if (!document.IsObject())
            return Fail(TEXT("Managed importer result root must be an object."));
        const auto version = document.FindMember("implementationVersion");
        const auto objects = document.FindMember("objects");
        if (version == document.MemberEnd() || !version->value.IsInt() || version->value.GetInt() < 1 ||
            objects == document.MemberEnd() || !objects->value.IsArray() || objects->value.Empty())
            return Fail(TEXT("Managed importer result has no implementation version or objects."));
        implementationVersion = version->value.GetInt();
        const auto provider = document.FindMember("providerHash");
        if (provider == document.MemberEnd() || !provider->value.IsString() ||
            ContentHash::Parse(provider->value.GetStringAnsiView(), providerHash))
            return Fail(TEXT("Managed importer provider fingerprint is missing or invalid."));
        int32 mainCount = 0;
        HashSet<String> identifiers;
        for (const rapidjson_flax::Value& value : objects->value.GetArray())
        {
            if (!value.IsObject())
                return Fail(TEXT("Managed importer object declaration is malformed."));
            ManagedOutput output;
            const auto main = value.FindMember("main");
            if (ReadString(value, "identifier", output.Identifier) || ReadString(value, "type", output.TypeName) ||
                ReadString(value, "name", output.DisplayName) || ReadAnsi(value, "format", output.Format) ||
                DecodeBytes(value, "data", output.Bytes) || main == value.MemberEnd() || !main->value.IsBool())
                return Fail(TEXT("Managed importer object declaration is incomplete."));
            output.Main = main->value.GetBool();
            if (output.Identifier.IsEmpty() || output.TypeName.IsEmpty() || !identifiers.Add(output.Identifier) ||
                (output.Format != "flax" && output.Format != "json"))
                return Fail(TEXT("Managed importer object identifiers, types, or formats are invalid."));
            output.StableKey = TEXT("scripted:") + SubAssetPolicy::NormalizeKey(output.Identifier);
            if (!output.Main && !SubAssetPolicy::IsKeyValid(output.StableKey))
                return Fail(TEXT("Managed importer returned an invalid stable object identifier."));
            if (output.Main)
                mainCount++;
            outputs.Add(MoveTemp(output));
        }
        if (mainCount != 1)
            return Fail(TEXT("Managed importer must select exactly one main object."));

        const auto auxiliary = document.FindMember("outputData");
        if (auxiliary != document.MemberEnd())
        {
            if (!auxiliary->value.IsObject())
                return Fail(TEXT("Managed importer auxiliary output data is malformed."));
            for (auto member = auxiliary->value.MemberBegin(); member != auxiliary->value.MemberEnd(); ++member)
            {
                if (!member->value.IsString() || member->name.GetStringLength() == 0)
                    return Fail(TEXT("Managed importer auxiliary output entry is malformed."));
                ManagedAuxOutput output;
                output.Kind = StringAnsi("data-") + StringAnsi(member->name.GetString(), member->name.GetStringLength());
                Encryption::Base64Decode(member->value.GetString(), member->value.GetStringLength(), output.Bytes);
                if (output.Bytes.IsEmpty())
                    return Fail(TEXT("Managed importer auxiliary outputs cannot be empty."));
                auxiliaries.Add(MoveTemp(output));
            }
        }
        postprocessorHash = ContentHash::Compute("[]", 2);
        const auto postprocessors = document.FindMember("postprocessorHash");
        if (postprocessors != document.MemberEnd())
        {
            if (!postprocessors->value.IsString() || ContentHash::Parse(postprocessors->value.GetStringAnsiView(), postprocessorHash))
                return Fail(TEXT("Managed importer postprocessor fingerprint is invalid."));
        }
        return false;
    }

    bool ReconcileMetadata(const StringView& sourcePath, AssetMeta& meta, const rapidjson_flax::Document& document,
        Array<ManagedOutput>& outputs, AssetPipelineDiagnostic& diagnostic)
    {
        Array<SubAssetCandidate> candidates;
        for (const ManagedOutput& output : outputs)
        {
            if (output.Main)
            {
                meta.AssetType = output.TypeName;
                continue;
            }
            SubAssetCandidate candidate;
            candidate.StableKey = output.StableKey;
            candidate.TypeName = output.TypeName;
            candidate.DisplayName = output.DisplayName;
            const auto renames = document.FindMember("identityRenames");
            if (renames != document.MemberEnd() && renames->value.IsObject())
            {
                for (auto rename = renames->value.MemberBegin(); rename != renames->value.MemberEnd(); ++rename)
                {
                    if (!rename->value.IsString())
                        continue;
                    const String newIdentifier(rename->value.GetStringAnsiView());
                    if (newIdentifier == output.Identifier)
                    {
                        candidate.PreviousKeys.Add(TEXT("scripted:") + SubAssetPolicy::NormalizeKey(String(rename->name.GetStringAnsiView())));
                        candidate.RenameEvidenceReliable = true;
                    }
                }
            }
            candidates.Add(MoveTemp(candidate));
        }
        SubAssetReconcileResult reconciliation = SubAssetReconciler::Reconcile(meta, candidates, true);
        if (reconciliation.RequiresUserReconciliation)
        {
            if (reconciliation.Diagnostics.HasItems())
                diagnostic = reconciliation.Diagnostics[0];
            return true;
        }
        meta.SubAssets = MoveTemp(reconciliation.Resolved);
        return AssetDatabaseFacade::CommitMetadata(sourcePath, meta, true, diagnostic);
    }

    bool DeclareSourceRecord(PrepareAssetContext& context, const AssetRecord& dependencyRecord,
        const AssetDependencyOrigin& origin, AssetPipelineDiagnostic& diagnostic)
    {
        ContentHash content;
        ContentHash metadata;
        if (HashFile(dependencyRecord.SourcePath.Get(), content))
            return context.DeclareSourceAssetByGuid(dependencyRecord.SourceAssetID, ContentHash(), ContentHash(), true, origin, diagnostic);
        if (dependencyRecord.MetaPath.Get().HasChars() && FileSystem::FileExists(dependencyRecord.MetaPath.Get()))
            HashFile(dependencyRecord.MetaPath.Get(), metadata);
        return context.DeclareSourceAssetByGuid(dependencyRecord.SourceAssetID, content, metadata, false, origin, diagnostic);
    }

    bool ReadCurrentArtifact(const AssetRecord& record, const ArtifactTarget& target, ArtifactKey& key,
        const StringAnsiView& outputKind = StringAnsiView("runtime"), ArtifactManifest* resultManifest = nullptr)
    {
        String manifestPath;
        AssetPipelineDiagnostic diagnostic;
        if (AssetDatabaseStorage::GetCurrentArtifactManifest(Globals::ProjectLibraryFolder, record.ID,
            target.BuildKey(ArtifactTargetDimension::All), manifestPath, diagnostic) || manifestPath.IsEmpty())
            return true;
        StringAnsi json;
        ArtifactManifest manifest;
        if (File::ReadAllText(manifestPath, json) || ArtifactManifest::Parse(json, manifestPath, manifest, diagnostic))
            return true;
        for (const ArtifactManifestOutput& output : manifest.Outputs)
        {
            if (output.Kind == outputKind)
            {
                key = output.Key;
                if (resultManifest)
                    *resultManifest = manifest;
                return false;
            }
        }
        return true;
    }

    bool DeclareDependencies(PrepareAssetContext& context, const rapidjson_flax::Document& document,
        const AssetRecord& owner, const ArtifactTarget& target, AssetPipelineDiagnostic& diagnostic)
    {
        const auto root = document.FindMember("dependencies");
        if (root == document.MemberEnd())
            return false;
        if (!root->value.IsObject())
            return Fail(TEXT("Managed importer dependencies are malformed."));
        const rapidjson_flax::Value& dependencies = root->value;
        AssetDependencyOrigin origin;
        origin.Path = owner.SourcePath.Get();

        const auto sourcePaths = dependencies.FindMember("sourcePaths");
        if (sourcePaths != dependencies.MemberEnd())
        {
            if (!sourcePaths->value.IsArray())
                return Fail(TEXT("Managed importer source-path dependencies are malformed."));
            for (const auto& item : sourcePaths->value.GetArray())
            {
                if (!item.IsString())
                    return Fail(TEXT("Managed importer source-path dependency is not a path."));
                AssetRecord record;
                const String path(item.GetStringAnsiView());
                if (FindRecordByPath(path, record))
                {
                    if (DeclareSourceRecord(context, record, origin, diagnostic))
                        return true;
                }
                else
                {
                    const String resolved = ResolvePath(path);
                    if (resolved.IsEmpty() || context.DeclareExactSourceFile(resolved, ContentHash(), true, origin, diagnostic))
                        return true;
                }
            }
        }

        const auto sourceGuids = dependencies.FindMember("sourceGuids");
        if (sourceGuids != dependencies.MemberEnd())
        {
            if (!sourceGuids->value.IsArray())
                return Fail(TEXT("Managed importer source-GUID dependencies are malformed."));
            for (const auto& item : sourceGuids->value.GetArray())
            {
                Guid id;
                AssetRecord record;
                if (!item.IsString() || Guid::Parse(item.GetStringAnsiView(), id))
                    return Fail(TEXT("Managed importer source dependency GUID is invalid."));
                if (AssetDatabase::Get().TryGetRecord(id, record))
                {
                    if (DeclareSourceRecord(context, record, origin, diagnostic))
                        return true;
                }
                else if (context.DeclareSourceAssetByGuid(id, ContentHash(), ContentHash(), true, origin, diagnostic))
                    return true;
            }
        }

        auto declareArtifact = [&](const AssetRecord* record, const Guid& id, const StringView& identity)
        {
            ArtifactKey key;
            const bool missing = !record || ReadCurrentArtifact(*record, target, key);
            AssetSemanticInterface interface;
            return context.DeclareArtifactDependency(identity, id,
                missing ? AssetDependencyState::Missing : AssetDependencyState::CurrentArtifact,
                key, interface, origin, diagnostic);
        };
        const auto artifactPaths = dependencies.FindMember("artifactPaths");
        if (artifactPaths != dependencies.MemberEnd())
        {
            if (!artifactPaths->value.IsArray())
                return Fail(TEXT("Managed importer artifact-path dependencies are malformed."));
            for (const auto& item : artifactPaths->value.GetArray())
            {
                AssetRecord record;
                if (!item.IsString())
                    return Fail(TEXT("Managed importer artifact-path dependency is not a path."));
                const String path(item.GetStringAnsiView());
                if (!FindRecordByPath(path, record))
                {
                    const ContentHash missingHash = ContentHash::Compute(item.GetString(), item.GetStringLength());
                    if (context.DeclareToolchain(TEXT("missing-artifact:") + path, missingHash, origin, diagnostic))
                        return true;
                }
                else if (declareArtifact(&record, record.ID, path))
                    return true;
            }
        }
        const auto artifactGuids = dependencies.FindMember("artifactGuids");
        if (artifactGuids != dependencies.MemberEnd())
        {
            if (!artifactGuids->value.IsArray())
                return Fail(TEXT("Managed importer artifact-GUID dependencies are malformed."));
            for (const auto& item : artifactGuids->value.GetArray())
            {
                Guid id;
                AssetRecord record;
                if (!item.IsString() || Guid::Parse(item.GetStringAnsiView(), id))
                    return Fail(TEXT("Managed importer artifact dependency GUID is invalid."));
                const bool found = AssetDatabase::Get().TryGetRecord(id, record);
                if (declareArtifact(found ? &record : nullptr, id, TEXT("guid:") + id.ToString(Guid::FormatType::N).ToLower()))
                    return true;
            }
        }

        const auto observedArtifacts = dependencies.FindMember("observedArtifacts");
        if (observedArtifacts != dependencies.MemberEnd())
        {
            if (!observedArtifacts->value.IsObject())
                return Fail(TEXT("Managed importer observed-artifact dependencies are malformed."));
            for (auto item = observedArtifacts->value.MemberBegin(); item != observedArtifacts->value.MemberEnd(); ++item)
            {
                const String identity(item->name.GetStringAnsiView());
                const int32 separator = identity.Find('/');
                Guid sourceId;
                ArtifactKey expected;
                if (!item->value.IsString() || !identity.StartsWith(TEXT("guid:")) || separator <= 5 ||
                    Guid::Parse(identity.Substring(5, separator - 5), sourceId) ||
                    ArtifactKey::Parse(item->value.GetStringAnsiView(), expected))
                    return Fail(TEXT("Managed importer observed-artifact identity or key is invalid."));
                AssetRecord record;
                for (const AssetRecord& candidate : AssetDatabase::Get().GetSnapshot().Records)
                {
                    if (candidate.IsMainAsset() && candidate.SourceAssetID == sourceId)
                    {
                        record = candidate;
                        break;
                    }
                }
                const StringAnsi kind(identity.Substring(separator + 1));
                ArtifactKey current;
                if (!record.ID.IsValid() || ReadCurrentArtifact(record, target, current, kind) || current != expected)
                    return Fail(TEXT("An artifact read by the managed importer changed before publication; the staged result was discarded."));
                AssetSemanticInterface interface;
                if (context.DeclareArtifactDependency(identity, record.ID, AssetDependencyState::ExactArtifact,
                    expected, interface, origin, diagnostic))
                    return true;
            }
        }

        const auto exactArtifacts = dependencies.FindMember("exactArtifacts");
        if (exactArtifacts != dependencies.MemberEnd())
        {
            if (!exactArtifacts->value.IsArray())
                return Fail(TEXT("Managed importer exact-artifact dependencies are malformed."));
            for (const auto& item : exactArtifacts->value.GetArray())
            {
                ArtifactKey key;
                if (!item.IsString() || ArtifactKey::Parse(item.GetStringAnsiView(), key))
                    return Fail(TEXT("Managed importer exact-artifact key is invalid."));
                if (context.DeclareToolchain(TEXT("exact-artifact:") + String(item.GetStringAnsiView()), key.Digest, origin, diagnostic))
                    return true;
            }
        }

        const auto custom = dependencies.FindMember("custom");
        if (custom != dependencies.MemberEnd())
        {
            if (!custom->value.IsArray())
                return Fail(TEXT("Managed importer custom dependencies are malformed."));
            for (const auto& item : custom->value.GetArray())
            {
                if (!item.IsString())
                    return Fail(TEXT("Managed importer custom dependency is invalid."));
                const String name(item.GetStringAnsiView());
                Guid registeredHash;
                uint64 registeredRevision;
                bool found;
                if (AssetDatabaseStorage::GetCustomDependency(Globals::ProjectLibraryFolder, name,
                        registeredHash, registeredRevision, found, diagnostic))
                    return true;
                ArtifactKeyBuilder dependencyKey(StringAnsiView("flax-custom-dependency-v1"));
                dependencyKey.AddString(StringAnsiView("name"), StringAnsi(name));
                dependencyKey.AddBool(StringAnsiView("registered"), found);
                if (found)
                {
                    dependencyKey.AddGuid(StringAnsiView("value"), registeredHash);
                    dependencyKey.AddUInt64(StringAnsiView("revision"), registeredRevision);
                }
                const ContentHash hash = dependencyKey.Finalize().Digest;
                if (context.DeclareCustomDependency(name, hash, origin, diagnostic))
                    return true;
            }
        }
        const auto global = dependencies.FindMember("global");
        if (global != dependencies.MemberEnd())
        {
            if (!global->value.IsObject())
                return Fail(TEXT("Managed importer global dependencies are malformed."));
            for (auto item = global->value.MemberBegin(); item != global->value.MemberEnd(); ++item)
            {
                if (!item->value.IsString())
                    return Fail(TEXT("Managed importer global dependency hash is invalid."));
                const ContentHash hash = ContentHash::Compute(item->value.GetString(), item->value.GetStringLength());
                if (context.DeclareGlobalDependency(String(item->name.GetStringAnsiView()), hash, origin, diagnostic))
                    return true;
            }
        }

        const auto tools = dependencies.FindMember("tools");
        if (tools != dependencies.MemberEnd())
        {
            if (!tools->value.IsObject())
                return Fail(TEXT("Managed importer tool dependencies are malformed."));
            for (auto item = tools->value.MemberBegin(); item != tools->value.MemberEnd(); ++item)
            {
                if (!item->value.IsString())
                    return Fail(TEXT("Managed importer tool dependency hash is invalid."));
                const ContentHash hash = ContentHash::Compute(item->value.GetString(), item->value.GetStringLength());
                if (context.DeclareToolchain(TEXT("managed-tool:") + String(item->name.GetStringAnsiView()), hash, origin, diagnostic))
                    return true;
            }
        }

        const auto logicalPath = dependencies.FindMember("logicalPath");
        if (logicalPath != dependencies.MemberEnd())
        {
            if (!logicalPath->value.IsBool())
                return Fail(TEXT("Managed importer logical-path dependency is malformed."));
            if (logicalPath->value.GetBool())
            {
                const StringAnsi canonicalPath(owner.CanonicalPath.Get());
                const ContentHash hash = ContentHash::Compute(canonicalPath.Get(), canonicalPath.Length());
                if (context.DeclareToolchain(TEXT("managed-logical-path"), hash, origin, diagnostic))
                    return true;
            }
        }

        const auto observedSources = dependencies.FindMember("observedSources");
        if (observedSources != dependencies.MemberEnd())
        {
            if (!observedSources->value.IsObject())
                return Fail(TEXT("Managed importer observed-source dependencies are malformed."));
            for (auto item = observedSources->value.MemberBegin(); item != observedSources->value.MemberEnd(); ++item)
            {
                if (!item->value.IsString())
                    return Fail(TEXT("Managed importer observed-source hash is invalid."));
                const String identity(item->name.GetStringAnsiView());
                String observedPath;
                if (identity == TEXT("source"))
                {
                    observedPath = owner.SourcePath.Get();
                }
                else if (identity.StartsWith(TEXT("guid:")))
                {
                    Guid id;
                    AssetRecord record;
                    if (Guid::Parse(identity.Substring(5), id) || !AssetDatabase::Get().TryGetRecord(id, record))
                        return Fail(TEXT("A managed importer observed source GUID is no longer registered."));
                    observedPath = record.SourcePath.Get();
                }
                else if (identity.StartsWith(TEXT("path:")))
                {
                    observedPath = ResolvePath(identity.Substring(5));
                }
                else
                {
                    return Fail(TEXT("Managed importer observed-source identity is invalid."));
                }
                ContentHash expected;
                ContentHash current;
                if (observedPath.IsEmpty() || ContentHash::Parse(item->value.GetStringAnsiView(), expected) ||
                    HashFile(observedPath, current) || current != expected)
                {
                    return Fail(TEXT("A source read by the managed importer changed before publication; the staged result was discarded."));
                }
            }
        }
        return false;
    }

    bool MakeRuntimeBytes(ArtifactBuildContext& context, const ManagedOutput& output, const Guid& expectedId,
        Array<byte>& bytes, AssetPipelineDiagnostic& diagnostic)
    {
        if (output.Format == "json")
        {
            rapidjson_flax::Document json;
            json.Parse(reinterpret_cast<const char*>(output.Bytes.Get()), output.Bytes.Count());
            if (json.HasParseError() || !json.IsObject())
                return Fail(TEXT("Managed importer JSON object output is malformed."));
            auto id = json.FindMember("ID");
            auto type = json.FindMember("TypeName");
            auto data = json.FindMember("Data");
            if (id == json.MemberEnd() || !id->value.IsString() || type == json.MemberEnd() || !type->value.IsString() || data == json.MemberEnd())
                return Fail(TEXT("Managed importer JSON object output has no asset header."));
            const StringAnsi idText(expectedId.ToString(Guid::FormatType::N).ToLower());
            id->value.SetString(idText.Get(), idText.Length(), json.GetAllocator());
            rapidjson_flax::StringBuffer buffer;
            CompactJsonWriter writer(buffer);
            json.Accept(writer.GetWriter());
            bytes.Set(reinterpret_cast<const byte*>(buffer.GetString()), static_cast<int32>(buffer.GetSize()));
            return false;
        }

        String scratchPath;
        if (context.CreateScratchFilePath(TEXT(".flax"), scratchPath, diagnostic) ||
            File::WriteAllBytes(scratchPath, output.Bytes.Get(), output.Bytes.Count()))
            return Fail(diagnostic, TEXT("Managed importer binary output could not be staged."));
        SCOPE_EXIT
        {
            ContentStorageManager::EnsureAccess(scratchPath);
            FileSystem::DeleteFile(scratchPath);
        };
        ContentStorageManager::EnsureAccess(scratchPath);
        auto storage = ContentStorageManager::GetStorage(scratchPath);
        if (!storage || storage->IsReadOnly() || storage->GetEntriesCount() != 1)
            return Fail(TEXT("Managed importer binary output is not a single writable Flax asset."));
        FlaxStorage::Entry entry;
        storage->GetEntry(0, entry);
        if (entry.TypeName != output.TypeName)
            return Fail(TEXT("Managed importer binary output type does not match its declaration."));
        if (entry.ID != expectedId && storage->ChangeAssetID(entry, expectedId))
            return Fail(TEXT("Managed importer binary output identity could not be assigned."));
        ContentStorageManager::EnsureAccess(scratchPath);
        if (File::ReadAllBytes(scratchPath, bytes))
            return Fail(TEXT("Managed importer binary output could not be read after identity assignment."));
        return false;
    }

    void NotifyPublished(const ArtifactManifest& manifest, const String& typeName)
    {
        const Guid assetId = manifest.AssetID;
        const ArtifactManifestOutput* runtime = nullptr;
        for (const ArtifactManifestOutput& output : manifest.Outputs)
        {
            if (output.Kind == StringAnsiView("runtime"))
            {
                runtime = &output;
                break;
            }
        }
        ResolvedArtifact artifact;
        bool canSwitch = false;
        if (runtime && runtime->RelativePath.EndsWith(TEXT(".flax"), StringSearchCase::IgnoreCase))
        {
            ArtifactStoragePath path;
            AssetPipelineDiagnostic diagnostic;
            if (!ArtifactStore::TryResolveLibraryRelative(Globals::ProjectLibraryFolder, runtime->RelativePath, path, diagnostic))
            {
                artifact.AssetID = assetId;
                artifact.TypeName = typeName;
                artifact.StoragePath = path;
                artifact.OutputKind = TEXT("runtime");
                artifact.Key = String(runtime->Key.ToString());
                artifact.StorageKind = ArtifactStorageKind::Generated;
                artifact.IsExact = true;
                canSwitch = true;
            }
        }
        Scripting::InvokeOnUpdate([assetId, artifact, canSwitch]()
        {
            if (canSwitch)
            {
                Asset* asset = Content::GetAsset(assetId);
                auto* binary = asset ? ScriptingObject::Cast<BinaryAsset>(asset) : nullptr;
                if (binary && binary->GetTypeName() == artifact.TypeName && (binary->IsLoaded() || binary->LastLoadFailed()))
                    binary->SwitchStorage(artifact);
            }
            AssetDatabaseFacade::NotifyArtifactPublished(assetId);
        });
    }

    bool PublishObject(const rapidjson_flax::Document& document, const AssetMeta& meta, const AssetRecord& record,
        const ManagedOutput& output, const Array<ManagedAuxOutput>& auxiliaries, int32 implementationVersion,
        const ContentHash& providerHash, const ContentHash& postprocessorHash, ArtifactManifest& publishedManifest,
        AssetPipelineDiagnostic& diagnostic)
    {
        const ArtifactTarget target = TexturePipelineService::GetHostTarget();
        AssetProcessorDescriptor descriptor;
        descriptor.ID = meta.Processor.ID;
        descriptor.ProviderID = TEXT("managed");
        descriptor.ProviderKind = AssetProcessorProviderKind::Managed;
        descriptor.TrustMode = AssetProcessorTrustMode::IsolatedProcess;
        descriptor.SettingsSchemaVersion = meta.Processor.SettingsVersion;
        descriptor.ImplementationVersion = implementationVersion;
        descriptor.InterfaceVersion = 1;
        descriptor.ProviderSemanticIdentity = providerHash;
        descriptor.SourceKinds.Add(AssetSourceKind::ImportedSource);
        descriptor.MainOutputType = record.TypeName;
        descriptor.SupportsSubAssets = true;
        descriptor.UsesExternalProcess = true;
        AssetProcessorOutputDescriptor runtime;
        runtime.Kind = "runtime";
        runtime.Extension = output.Format == "json" ? ".json" : ".flax";
        runtime.FormatVersion = 1;
        runtime.TargetDimensions = ArtifactTargetDimension::Platform | ArtifactTargetDimension::Architecture;
        runtime.CompatibilityTag = output.Format == "json" ? "flax-scripted-json-v1" : "flax-scripted-binary-v1";
        runtime.IndependentlyReusable = true;
        descriptor.Outputs.Add(runtime);
        if (output.Main)
        {
            for (const ManagedAuxOutput& auxiliary : auxiliaries)
            {
                AssetProcessorOutputDescriptor extra;
                extra.Kind = auxiliary.Kind;
                extra.Extension = ".bin";
                extra.FormatVersion = 1;
                extra.TargetDimensions = ArtifactTargetDimension::Platform | ArtifactTargetDimension::Architecture;
                extra.CompatibilityTag = "flax-scripted-data-v1";
                extra.IndependentlyReusable = true;
                descriptor.Outputs.Add(MoveTemp(extra));
            }
        }

        static SourceHashCache hashCache;
        AssetCancellationSource cancellation;
        PrepareAssetContext prepare(Globals::ProjectFolder, Globals::ProjectContentFolder, Globals::ProjectLibraryFolder,
            record, descriptor, meta.Processor.SettingsJson, target, hashCache, cancellation.GetToken());
        Array<byte> sourceBytes;
        ContentHash sourceHash;
        AssetDependencyOrigin origin;
        origin.Path = record.SourcePath.Get();
        if (prepare.ReadSourceFile(record.SourcePath.Get(), sourceBytes, sourceHash, origin, diagnostic) ||
            DeclareDependencies(prepare, document, record, target, diagnostic))
            return true;
        prepare.SetPostprocessorFingerprint(postprocessorHash);
        prepare.SetSourceSerializerVersion(1);
        if (prepare.DeclareToolchain(TEXT("managed-importer"), providerHash, origin, diagnostic) ||
            prepare.DeclareOutput(StringAnsiView("runtime"), record.ID, diagnostic))
            return true;
        if (output.Main)
        {
            for (const ManagedAuxOutput& auxiliary : auxiliaries)
            {
                if (prepare.DeclareOutput(auxiliary.Kind, record.ID, diagnostic))
                    return true;
            }
        }
        PreparedAsset prepared;
        prepared.MemoryEstimate = sourceBytes.Count() + output.Bytes.Count();
        for (const ManagedAuxOutput& auxiliary : auxiliaries)
            prepared.MemoryEstimate += auxiliary.Bytes.Count();
        if (prepare.Finalize(record.DatabaseRevision, prepared, diagnostic))
            return true;

        Array<ArtifactBuildInput> inputs;
        for (const AssetDependency& dependency : prepared.Dependencies)
        {
            if (dependency.Kind == AssetDependencyKind::SourceAsset && dependency.AssetID == record.SourceAssetID)
            {
                ArtifactBuildInput input;
                input.StableIdentity = dependency.StableIdentity;
                input.Path = record.SourcePath.Get();
                input.ExpectedContent = dependency.Content;
                inputs.Add(MoveTemp(input));
                break;
            }
        }
        const Guid jobId = Guid::New();
        ArtifactBuildContext build(Globals::ProjectFolder, Globals::ProjectContentFolder, Globals::ProjectLibraryFolder,
            jobId, prepared, inputs, cancellation.GetToken(), target);
        if (build.Initialize(diagnostic))
            return true;
        Array<byte> runtimeBytes;
        if (MakeRuntimeBytes(build, output, record.ID, runtimeBytes, diagnostic))
            return true;
        ArtifactWriter runtimeWriter;
        if (build.OpenOutput(StringAnsiView("runtime"), runtimeWriter, diagnostic) ||
            runtimeWriter.WriteFile(output.Format == "json" ? TEXT("scripted.json") : TEXT("scripted.flax"),
                runtimeBytes.Get(), runtimeBytes.Count(), diagnostic))
            return true;
        if (output.Main)
        {
            for (const ManagedAuxOutput& auxiliary : auxiliaries)
            {
                ArtifactWriter writer;
                if (build.OpenOutput(auxiliary.Kind, writer, diagnostic) ||
                    writer.WriteFile(TEXT("data.bin"), auxiliary.Bytes.Get(), auxiliary.Bytes.Count(), diagnostic))
                    return true;
            }
        }

        ArtifactOutputValidatorRegistry validators;
        const Guid expectedId = record.ID;
        const String expectedType = record.TypeName;
        const bool rawJson = output.Format == "json";
        ArtifactOutputValidator validateRuntime = [expectedId, expectedType, rawJson](const StringView& path,
            const ArtifactManifestOutput& manifestOutput, AssetPipelineDiagnostic& result)
        {
            if (manifestOutput.Size == 0 || manifestOutput.Size != FileSystem::GetFileSize(path))
            {
                result.Code = AssetPipelineDiagnosticCode::ArtifactInvalid;
                result.Stage = AssetPipelineDiagnosticStage::Publication;
                result.AssetGuid = expectedId;
                result.Message = TEXT("Managed importer runtime artifact is empty or truncated.");
                return true;
            }
            if (!rawJson)
            {
                ContentStorageManager::EnsureAccess(path);
                auto storage = ContentStorageManager::GetStorage(path);
                FlaxStorage::Entry entry;
                if (!storage || storage->GetEntriesCount() != 1)
                {
                    result.Code = AssetPipelineDiagnosticCode::ArtifactInvalid;
                    result.Stage = AssetPipelineDiagnosticStage::Publication;
                    result.AssetGuid = expectedId;
                    result.Message = TEXT("Managed importer runtime artifact identity or type is invalid.");
                    return true;
                }
                storage->GetEntry(0, entry);
                if (entry.ID != expectedId || entry.TypeName != expectedType)
                {
                    result.Code = AssetPipelineDiagnosticCode::ArtifactInvalid;
                    result.Stage = AssetPipelineDiagnosticStage::Publication;
                    result.AssetGuid = expectedId;
                    result.Message = TEXT("Managed importer runtime artifact identity or type is invalid.");
                    return true;
                }
            }
            result = AssetPipelineDiagnostic();
            return false;
        };
        if (validators.Register(StringAnsiView("runtime"), expectedType, validateRuntime, diagnostic))
            return true;
        if (output.Main)
        {
            for (const ManagedAuxOutput& auxiliary : auxiliaries)
            {
                ArtifactOutputValidator validateData = [](const StringView& path, const ArtifactManifestOutput& manifestOutput,
                    AssetPipelineDiagnostic& result)
                {
                    if (manifestOutput.Size == 0 || manifestOutput.Size != FileSystem::GetFileSize(path))
                    {
                        result.Code = AssetPipelineDiagnosticCode::ArtifactInvalid;
                        result.Stage = AssetPipelineDiagnosticStage::Publication;
                        result.Message = TEXT("Managed importer auxiliary artifact is empty or truncated.");
                        return true;
                    }
                    result = AssetPipelineDiagnostic();
                    return false;
                };
                if (validators.Register(auxiliary.Kind, StringView::Empty, validateData, diagnostic))
                    return true;
            }
        }

        ArtifactPublicationRequest publication;
        publication.Target = target;
        publication.ProcessorID = meta.Processor.ID;
        publication.ProcessorImplementationVersion = implementationVersion;
        publication.BuildID = jobId.ToString(Guid::FormatType::N);
        for (const DeclaredArtifactOutput& declared : prepared.Outputs)
        {
            ArtifactPublicationOutputPlan plan;
            plan.Kind = declared.Kind;
            ArtifactKeyBuilder keyBuilder(StringAnsiView("flax-scripted-importer-output-v1"));
            keyBuilder.AddKey(StringAnsiView("prepared-input"), prepared.InputFingerprint);
            keyBuilder.AddString(StringAnsiView("kind"), declared.Kind);
            keyBuilder.AddGuid(StringAnsiView("effective-asset"), declared.EffectiveAssetID);
            keyBuilder.AddTarget(target, declared.TargetDimensions);
            plan.Key = keyBuilder.Finalize();
            publication.Outputs.Add(MoveTemp(plan));
        }
        publication.QueryCurrentState = [assetId = record.ID, revision = record.DatabaseRevision,
            fingerprint = prepared.InputFingerprint](uint64& currentRevision, ArtifactKey& currentFingerprint)
        {
            AssetRecord current;
            if (AssetDatabase::Get().TryGetRecord(assetId, current) && current.DatabaseRevision == revision)
            {
                currentRevision = revision;
                currentFingerprint = fingerprint;
            }
            else
            {
                currentRevision = 0;
                currentFingerprint = ArtifactKey();
            }
        };
        publication.DeferDurableCommit = true;
        ArtifactPublicationResult result;
        if (ArtifactPublisher::Publish(Globals::ProjectLibraryFolder, prepared, build, publication, validators, result, diagnostic))
            return true;
        if (result.WasSuperseded || result.Manifest.AssetID != record.ID)
        {
            diagnostic.Code = AssetPipelineDiagnosticCode::PrepareInvalidated;
            diagnostic.Stage = AssetPipelineDiagnosticStage::Publication;
            diagnostic.AssetGuid = record.ID;
            diagnostic.Message = TEXT("Managed importer result was superseded before its source batch could commit.");
            return true;
        }
        publishedManifest = MoveTemp(result.Manifest);
        return false;
    }
}

bool ScriptedImporterFacade::Publish(const StringView& sourcePath, const StringView& resultJson)
{
    const String resolved = ResolvePath(sourcePath);
    if (resolved.IsEmpty() || !FileSystem::FileExists(resolved))
        return Fail(TEXT("Managed importer publication source is missing or outside registered mounts."));
    rapidjson_flax::Document document;
    const StringAnsi json(resultJson);
    document.Parse(json.Get(), json.Length());
    if (document.HasParseError())
        return Fail(TEXT("Managed importer result JSON is malformed."));
    Array<ManagedOutput> outputs;
    Array<ManagedAuxOutput> auxiliaries;
    int32 implementationVersion;
    ContentHash providerHash;
    ContentHash postprocessorHash;
    if (ParseResult(document, outputs, auxiliaries, implementationVersion, providerHash, postprocessorHash))
        return true;

    AssetPipelineDiagnostic diagnostic;
    AssetMeta meta;
    if (AssetMeta::Load(resolved + TEXT(".meta"), meta, diagnostic))
        return Fail(diagnostic, TEXT("Managed importer metadata is unreadable."));
    if (ReconcileMetadata(resolved, meta, document, outputs, diagnostic))
        return Fail(diagnostic, TEXT("Managed importer object identity reconciliation failed."));
    if (AssetMeta::Load(resolved + TEXT(".meta"), meta, diagnostic))
        return Fail(diagnostic, TEXT("Managed importer reconciled metadata is unreadable."));

    const AssetDatabaseSnapshot snapshot = AssetDatabase::Get().GetSnapshot();
    Array<ArtifactManifest> manifests;
    Array<String> manifestTypes;
    manifests.EnsureCapacity(outputs.Count());
    manifestTypes.EnsureCapacity(outputs.Count());
    for (const ManagedOutput& output : outputs)
    {
        const AssetRecord* selected = nullptr;
        for (const AssetRecord& record : snapshot.Records)
        {
            if (record.SourceAssetID != meta.ID)
                continue;
            if ((output.Main && record.IsMainAsset()) || (!output.Main && record.SubAsset.Get() == output.StableKey))
            {
                selected = &record;
                break;
            }
        }
        if (!selected)
            return Fail(TEXT("Managed importer reconciled object is absent from the asset database."));
        ArtifactManifest manifest;
        if (PublishObject(document, meta, *selected, output, auxiliaries, implementationVersion, providerHash, postprocessorHash, manifest, diagnostic))
            return Fail(diagnostic, TEXT("Managed importer artifact publication failed."));
        manifests.Add(MoveTemp(manifest));
        manifestTypes.Add(selected->TypeName);
    }
    for (const ArtifactManifest& manifest : manifests)
    {
        AssetRecord current;
        if (!AssetDatabase::Get().TryGetRecord(manifest.AssetID, current) || current.DatabaseRevision != manifest.DatabaseRevision)
            return Fail(TEXT("Managed importer source batch changed before its atomic artifact commit."));
    }
    if (AssetDatabaseStorage::PublishArtifacts(Globals::ProjectLibraryFolder, manifests, diagnostic))
        return Fail(diagnostic, TEXT("Managed importer artifact batch commit failed."));
    for (int32 i = 0; i < manifests.Count(); i++)
        NotifyPublished(manifests[i], manifestTypes[i]);
    ClearError();
    return false;
}

#else

bool ScriptedImporterFacade::EnsureMetadata(const StringView&, const StringView&, int32)
{
    return true;
}

bool ScriptedImporterFacade::Publish(const StringView&, const StringView&)
{
    return true;
}

String ScriptedImporterFacade::ReadArtifactOutput(const Guid&, const StringView&)
{
    return String::Empty;
}

String ScriptedImporterFacade::ReadArtifactOutputForCoordinator(const Guid&, const StringView&)
{
    return String::Empty;
}

#endif

String ScriptedImporterFacade::GetLastError()
{
#if USE_EDITOR && COMPILE_WITH_ASSETS_IMPORTER
    ScopeLock lock(ErrorLocker);
    return LastError;
#else
    return TEXT("Managed scripted importers are unavailable in this build.");
#endif
}
