// Copyright (c) Wojciech Figat. All rights reserved.

#include "EngineContentCatalog.h"
#include "SourceHashCache.h"
#include "Engine/Engine/Globals.h"
#include "Engine/Platform/FileSystem.h"

namespace
{
    struct RawEngineContentCatalogEntry
    {
        const Char* RelativePath;
        const Char* ID;
        const Char* TypeName;
        const Char* ContentHash;
        uint64 Size;
        const Char* RuntimeReferences;
    };

    const RawEngineContentCatalogEntry Catalog[] =
    {
#include "EngineContentCatalog.generated.inl"
    };

    bool ParseManagedGuid(const StringView& text, Guid& value)
    {
        String compact(text);
        compact.Replace(TEXT("-"), TEXT(""));
        if (compact.Length() != 32)
            return true;
        uint32 data2, data3;
        if (StringUtils::ParseHex(compact.Get(), 8, &value.A) ||
            StringUtils::ParseHex(compact.Get() + 8, 4, &data2) ||
            StringUtils::ParseHex(compact.Get() + 12, 4, &data3))
            return true;
        value.Raw[4] = static_cast<byte>(data2);
        value.Raw[5] = static_cast<byte>(data2 >> 8);
        value.Raw[6] = static_cast<byte>(data3);
        value.Raw[7] = static_cast<byte>(data3 >> 8);
        for (int32 i = 0; i < 8; i++)
        {
            uint32 part;
            if (StringUtils::ParseHex(compact.Get() + 16 + i * 2, 2, &part))
                return true;
            value.Raw[8 + i] = static_cast<byte>(part);
        }
        return false;
    }

    bool ParseEntry(const RawEngineContentCatalogEntry& raw, Guid& id, ContentHash& content,
        Array<Guid>& runtimeReferences)
    {
        if (ParseManagedGuid(raw.ID, id) || !id.IsValid() ||
            ContentHash::Parse(StringView(raw.ContentHash), content) || content.IsZero())
            return true;
        Array<String> referenceTokens;
        String(raw.RuntimeReferences).Split(';', referenceTokens);
        for (const String& token : referenceTokens)
        {
            if (token.IsEmpty())
                continue;
            Guid reference;
            if (ParseManagedGuid(token, reference) || !reference.IsValid())
                return true;
            if (!runtimeReferences.Contains(reference))
                runtimeReferences.Add(reference);
        }
        return false;
    }

    AssetPipelineDiagnostic MakeDiagnostic(const AssetPipelineDiagnosticCode code, const StringView& path,
        const StringView& message)
    {
        AssetPipelineDiagnostic result;
        result.Code = code;
        result.Stage = AssetPipelineDiagnosticStage::DatabaseScan;
        result.SourcePath = path;
        result.ProcessorId = EngineContentCatalog::ProcessorID;
        result.Message = message;
        return result;
    }
}

bool EngineContentCatalog::Collect(const StringView& engineContentRoot, Array<AssetRecord>& records,
    Array<AssetPipelineDiagnostic>& diagnostics)
{
    SourceHashCache hashCache;
    records.EnsureCapacity(records.Count() + ARRAY_COUNT(Catalog));
    for (const RawEngineContentCatalogEntry& raw : Catalog)
    {
        Guid id;
        ContentHash content;
        Array<Guid> references;
        const String physicalPath = String(engineContentRoot) / raw.RelativePath;
        if (ParseEntry(raw, id, content, references))
        {
            diagnostics.Add(MakeDiagnostic(AssetPipelineDiagnosticCode::InvalidMeta, physicalPath,
                TEXT("The compiled engine-content catalog contains an invalid entry.")));
            return true;
        }
        if (!FileSystem::FileExists(physicalPath) || FileSystem::GetFileSize(physicalPath) != raw.Size)
        {
            diagnostics.Add(MakeDiagnostic(AssetPipelineDiagnosticCode::ArtifactMissing, physicalPath,
                TEXT("A cataloged shipped engine-content blob is missing or has the wrong size.")));
            return true;
        }
        SourceHashFileState state;
        ContentHash observed;
        AssetPipelineDiagnostic hashDiagnostic;
        if (hashCache.HashFile(physicalPath, observed, state, hashDiagnostic))
        {
            hashDiagnostic.ProcessorId = ProcessorID;
            diagnostics.Add(MoveTemp(hashDiagnostic));
            return true;
        }
        if (observed != content)
        {
            diagnostics.Add(MakeDiagnostic(AssetPipelineDiagnosticCode::ArtifactInvalid, physicalPath,
                TEXT("A shipped engine-content blob does not match its compiled catalog hash.")));
            return true;
        }
        AssetRecord record;
        record.ID = id;
        record.SourceAssetID = id;
        record.LocalId = 1;
        record.TypeName = raw.TypeName;
        record.CanonicalPath = CanonicalAssetPath(String(TEXT("EngineContent/")) + raw.RelativePath);
        record.SourcePath = SourceFilePath(physicalPath);
        record.ProcessorID = ProcessorID;
        record.PortabilityKey = String(TEXT("enginecontent/")) + String(raw.RelativePath).ToLower();
        record.MetaSemanticHash = (static_cast<uint64>(content.Values[0]) << 32) | content.Values[1];
        record.RuntimeReferences = MoveTemp(references);
        for (const Guid& reference : record.RuntimeReferences)
            record.RuntimeObjectReferences.Add(AssetObjectId(reference, 1));
        record.SourceKind = AssetSourceKind::PrebuiltArtifact;
        record.Status = AssetRecordStatus::Ready;
        records.Add(MoveTemp(record));
    }
    return false;
}

bool EngineContentCatalog::Resolve(const Guid& assetID, ResolvedArtifact& artifact, ContentHash& content,
    uint64& size, AssetPipelineDiagnostic& diagnostic)
{
    artifact = ResolvedArtifact();
    content = ContentHash();
    size = 0;
    for (const RawEngineContentCatalogEntry& raw : Catalog)
    {
        Guid id;
        Array<Guid> references;
        ContentHash expected;
        if (ParseEntry(raw, id, expected, references) || id != assetID)
            continue;
#if USE_EDITOR
        const String physicalPath = Globals::EngineContentFolder / raw.RelativePath;
#else
        const String physicalPath = Globals::StartupFolder / TEXT("Content") / raw.RelativePath;
#endif
        SourceHashCache cache;
        SourceHashFileState state;
        ContentHash observed;
        if (!FileSystem::FileExists(physicalPath) || FileSystem::GetFileSize(physicalPath) != raw.Size ||
            cache.HashFile(physicalPath, observed, state, diagnostic) || observed != expected)
        {
            diagnostic = MakeDiagnostic(AssetPipelineDiagnosticCode::ArtifactInvalid, physicalPath,
                TEXT("The shipped engine-content blob does not match its compiled catalog hash."));
            return true;
        }
        artifact.AssetID = id;
        artifact.TypeName = raw.TypeName;
        artifact.StoragePath = ArtifactStoragePath(physicalPath);
        artifact.OutputKind = TEXT("prebuilt");
        artifact.Key = String(expected.ToString());
        artifact.StorageKind = ArtifactStorageKind::Generated;
        artifact.IsExact = true;
        artifact.IsLastGood = false;
        content = expected;
        size = raw.Size;
        return false;
    }
    diagnostic = MakeDiagnostic(AssetPipelineDiagnosticCode::ArtifactMissing, assetID.ToString(),
        TEXT("The engine-content catalog contains no entry for the requested identity."));
    return true;
}
