// Copyright (c) Wojciech Figat. All rights reserved.

#include "BakedAssetFacade.h"

#if COMPILE_WITH_ASSETS_IMPORTER && USE_EDITOR

#include "AssetDatabaseFacade.h"
#include "AssetMutationService.h"
#include "AssetPath.h"
#include "Engine/Content/Build/Processors/BakedAssetProcessor.h"
#include "Engine/Content/Build/Processors/GraphPipelineService.h"
#include "Engine/Content/Storage/ContentStorageManager.h"
#include "Engine/Content/Storage/FlaxStorage.h"
#include "Engine/Core/Log.h"
#include "Engine/Core/ScopeExit.h"
#include "Engine/Engine/Globals.h"
#include "Engine/Platform/FileSystem.h"
#include "Engine/Serialization/JsonWriters.h"
#include "Engine/Utilities/Encryption.h"

namespace
{
    AssetMutationService& Mutations()
    {
        static AssetMutationService service(Globals::ProjectFolder, Globals::ProjectContentFolder,
            Globals::ProjectLibraryFolder / TEXT("AssetDatabase/MutationJournals"),
            Globals::ProjectLibraryFolder / TEXT("AssetDatabase/Recovery"));
        static bool databaseCommitBound = false;
        if (!databaseCommitBound)
        {
            service.DatabaseCommitHook = [](const AssetMutationResult& pending)
            {
                return AssetDatabaseFacade::RefreshSources(pending.ChangedPaths);
            };
            databaseCommitBound = true;
        }
        return service;
    }

    AssetMeta CreateFolderMeta()
    {
        AssetMeta meta;
        meta.ID = Guid::New();
        meta.FolderAsset = true;
        meta.AssetType = TEXT("FlaxEngine.Folder");
        meta.SourceKind = AssetSourceKind::Folder;
        meta.Processor.ID = TEXT("Flax.Folder");
        meta.Processor.SettingsVersion = 1;
        meta.Processor.SettingsJson = "{}\n";
        return meta;
    }

    bool ValidateFolderPair(const StringView& path, String& error)
    {
        AssetMeta meta;
        AssetPipelineDiagnostic diagnostic;
        if (AssetMeta::Load(String(path) + TEXT(".meta"), meta, diagnostic) || !meta.ID.IsValid() || !meta.FolderAsset ||
            meta.SourceKind != AssetSourceKind::Folder || meta.Processor.ID != TEXT("Flax.Folder"))
        {
            error = diagnostic.Message.HasChars() ? diagnostic.Message : TEXT("Folder metadata is invalid.");
            return true;
        }
        return false;
    }

    bool EnsureParentFolderPairs(const StringView& sourcePath, String& error)
    {
        Array<String> folders;
        String folder = StringUtils::GetDirectoryName(sourcePath);
        while (!FileSystem::AreFilePathsEquivalent(folder, Globals::ProjectContentFolder))
        {
            if (folder.IsEmpty() || !AssetPathPolicy::IsSameOrChild(folder, Globals::ProjectContentFolder))
            {
                error = TEXT("Baked asset parent escaped the canonical Content root.");
                return true;
            }
            folders.Add(folder);
            const String parent = StringUtils::GetDirectoryName(folder);
            if (parent.IsEmpty() || FileSystem::AreFilePathsEquivalent(parent, folder))
            {
                error = TEXT("Baked asset parent hierarchy could not be resolved.");
                return true;
            }
            folder = parent;
        }

        for (int32 i = folders.Count() - 1; i >= 0; i--)
        {
            const String& current = folders[i];
            const bool hasFolder = FileSystem::DirectoryExists(current);
            const bool hasMeta = FileSystem::FileExists(current + TEXT(".meta"));
            if (hasFolder && hasMeta)
            {
                if (ValidateFolderPair(current, error))
                    return true;
                continue;
            }
            if (!hasFolder && hasMeta)
            {
                error = TEXT("Baked asset parent has orphaned folder metadata: '") + current + TEXT(".meta'.");
                return true;
            }

            AssetMutationResult mutation;
            const bool failed = hasFolder
                ? Mutations().RegisterExisting(current, CreateFolderMeta(), false, mutation)
                : Mutations().CreateFolder(current, mutation);
            if (failed)
            {
                error = mutation.Message;
                return true;
            }
        }
        return false;
    }

    void WriteBytes(CompactJsonWriter& writer, const BytesContainer& bytes)
    {
        Array<char> encoded;
        if (bytes.Length())
            Encryption::Base64Encode(bytes.Get(), bytes.Length(), encoded);
        writer.String(encoded.HasItems() ? encoded.Get() : "", encoded.Count());
    }

    bool EncodeDocument(FlaxStorage* storage, const Guid& assetID, StringAnsi& output, String& typeName)
    {
        if (!storage || storage->GetEntriesCount() != 1)
            return true;
        AssetInitData data;
        if (storage->LoadAssetHeader(0, data) || data.Header.ID != assetID || data.Header.TypeName.IsEmpty())
            return true;
        for (int32 i = 0; i < ASSET_FILE_DATA_CHUNKS; i++)
        {
            FlaxChunk* chunk = data.Header.Chunks[i];
            if (chunk && storage->LoadAssetChunk(chunk))
                return true;
        }
        typeName = data.Header.TypeName;
        rapidjson_flax::StringBuffer buffer;
        CompactJsonWriter writer(buffer);
        writer.StartObject();
        writer.JKEY("flaxSourceVersion");
        writer.Uint(1);
        writer.JKEY("documentType");
        writer.String("Flax.BakedAsset", 15);
        writer.JKEY("type");
        const StringAnsi typeAnsi(typeName);
        writer.String(typeAnsi.Get(), typeAnsi.Length());
        writer.JKEY("serializedVersion");
        writer.Uint(data.SerializedVersion);
        writer.JKEY("customData");
        WriteBytes(writer, data.CustomData);
        writer.JKEY("chunks");
        writer.StartArray();
        for (int32 i = 0; i < ASSET_FILE_DATA_CHUNKS; i++)
        {
            const FlaxChunk* chunk = data.Header.Chunks[i];
            if (!chunk || !chunk->Data.IsValid())
                continue;
            writer.StartObject();
            writer.JKEY("index");
            writer.Int(i);
            writer.JKEY("flags");
            writer.Uint(static_cast<uint32>(chunk->Flags & FlaxChunkFlags::CompressedLZ4));
            writer.JKEY("data");
            WriteBytes(writer, chunk->Data);
            writer.EndObject();
        }
        writer.EndArray();
        writer.JKEY("runtimeReferences");
        writer.StartArray();
        HashSet<Guid> references;
        for (const Pair<Guid, DateTime>& dependency : data.Dependencies)
        {
            if (!dependency.First.IsValid() || dependency.First == assetID || references.Contains(dependency.First))
                continue;
            references.Add(dependency.First);
            writer.StartObject();
            writer.JKEY("guid");
            const StringAnsi guid(dependency.First.ToString(Guid::FormatType::N).ToLower());
            writer.String(guid.Get(), guid.Length());
            writer.JKEY("localId");
            writer.Int64(1);
            writer.EndObject();
        }
        writer.EndArray();
        writer.EndObject();
        output = StringAnsi(buffer.GetString(), static_cast<int32>(buffer.GetSize()));
        return output.IsEmpty();
    }
}

bool BakedAssetFacade::Create(const CreateAssetFunction& encoder, const StringView& sourcePath, Guid& assetID, void* argument)
{
    if (!encoder.IsBinded() || sourcePath.IsEmpty() ||
        FileSystem::GetExtension(sourcePath).ToLower() != TEXT("bakedasset") ||
        !AssetPathPolicy::IsSameOrChild(sourcePath, Globals::ProjectContentFolder))
    {
        LOG(Error, "Persistent tool output must target a canonical .bakedasset source under Content: '{0}'.", sourcePath);
        return true;
    }
    const String metaPath = String(sourcePath) + TEXT(".meta");
    const bool hasSource = FileSystem::FileExists(sourcePath);
    const bool hasMeta = FileSystem::FileExists(metaPath);
    if (hasSource != hasMeta)
    {
        LOG(Error, "Cannot replace incomplete baked asset source pair '{0}'.", sourcePath);
        return true;
    }
    AssetMeta meta;
    AssetPipelineDiagnostic diagnostic;
    if (hasMeta)
    {
        if (AssetMeta::Load(metaPath, meta, diagnostic) || meta.SourceKind != AssetSourceKind::TextDocument ||
            meta.Processor.ID != BakedAssetProcessor::ProcessorID())
        {
            LOG(Error, "Cannot replace baked asset source '{0}': {1}", sourcePath,
                diagnostic.Message.HasChars() ? diagnostic.Message : TEXT("metadata identity or processor mismatch"));
            return true;
        }
        assetID = meta.ID;
    }
    else
    {
        if (!assetID.IsValid())
            assetID = Guid::New();
        meta.ID = assetID;
    }

    const String stagingFolder = Globals::ProjectLibraryFolder / TEXT("Temp/BakedAssetSources");
    if (FileSystem::CreateDirectory(stagingFolder))
    {
        LOG(Error, "Cannot create baked asset staging folder '{0}'.", stagingFolder);
        return true;
    }
    const String stagingPath = stagingFolder / Guid::New().ToString(Guid::FormatType::N) + TEXT(".flax");
    SCOPE_EXIT
    {
        ContentStorageManager::EnsureAccess(stagingPath);
        FileSystem::DeleteFile(stagingPath);
    };
    CreateAssetContext context(StringView::Empty, stagingPath, assetID, argument, true, StringView::Empty);
    if (context.Run(encoder) != CreateAssetResult::Ok)
    {
        LOG(Error, "Baked asset encoder failed for '{0}'.", sourcePath);
        return true;
    }
    StringAnsi source;
    String typeName;
    {
        FlaxStorageReference storage = ContentStorageManager::GetStorage(stagingPath, true);
        if (!storage || EncodeDocument(storage.Get(), assetID, source, typeName))
        {
            LOG(Error, "Baked asset staging output is invalid for '{0}'.", sourcePath);
            return true;
        }
    }
    ContentStorageManager::EnsureAccess(stagingPath);

    meta.ID = assetID;
    meta.AssetType = typeName;
    meta.SourceKind = AssetSourceKind::TextDocument;
    meta.Processor.ID = BakedAssetProcessor::ProcessorID();
    meta.Processor.SettingsVersion = 1;
    meta.Processor.SettingsJson = "{}\n";
    String parentError;
    if (EnsureParentFolderPairs(sourcePath, parentError))
    {
        LOG(Error, "Cannot prepare baked asset parent folders for '{0}': {1}", sourcePath, parentError);
        return true;
    }
    AssetMutationResult mutation;
    const bool mutationFailed = hasSource
        ? Mutations().ReplaceAsset(sourcePath, source, meta, mutation)
        : Mutations().CreateAsset(sourcePath, source, meta, mutation);
    if (mutationFailed)
    {
        LOG(Error, "Cannot publish baked asset source '{0}': {1}", sourcePath, mutation.Message);
        return true;
    }
    if (GraphPipelineService::RequestBuildAndWait(assetID, true, diagnostic))
    {
        LOG(Error, "Cannot publish baked asset artifact for '{0}': {1}", sourcePath, diagnostic.Message);
        return true;
    }
    return false;
}

#endif
