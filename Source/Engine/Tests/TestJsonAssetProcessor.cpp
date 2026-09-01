// Copyright (c) Wojciech Figat. All rights reserved.

#if COMPILE_WITH_TESTS && COMPILE_WITH_ASSETS_IMPORTER && USE_EDITOR

#include "Engine/Content/Build/Processors/JsonAssetProcessor.h"
#include "Engine/Content/Build/PrepareAssetContext.h"
#include "Engine/Core/ScopeExit.h"
#include "Engine/Engine/Globals.h"
#include "Engine/Platform/File.h"
#include "Engine/Platform/FileSystem.h"
#include <cstring>
#include <ThirdParty/catch2/catch.hpp>

namespace
{
    AssetRecord MakeSceneRecord(const String& path)
    {
        AssetRecord record;
        record.ID = Guid::New();
        record.SourceAssetID = record.ID;
        record.LocalId = 1;
        record.TypeName = TEXT("FlaxEngine.SceneAsset");
        record.CanonicalPath = CanonicalAssetPath(path);
        record.SourcePath = SourceFilePath(path);
        record.ProcessorID = JsonAssetProcessor::ProcessorID();
        record.SourceKind = AssetSourceKind::TextDocument;
        record.Status = AssetRecordStatus::Ready;
        record.DatabaseRevision = 1;
        return record;
    }

    bool PrepareScene(const String& root, const String& content, const String& library, const char* json,
        AssetPipelineDiagnostic& diagnostic)
    {
        const String path = content / (Guid::New().ToString(Guid::FormatType::N) + TEXT(".scene"));
        if (File::WriteAllBytes(path, reinterpret_cast<const byte*>(json), static_cast<int32>(std::strlen(json))))
            return true;
        const AssetRecord record = MakeSceneRecord(path);
        const AssetProcessorDescriptor descriptor = JsonAssetProcessor::CreateDescriptor();
        SourceHashCache hashCache;
        AssetCancellationSource cancellation;
        PreparedAsset prepared;
        PrepareAssetContext context(root, content, library, record, descriptor, StringAnsiView("{}"), hashCache,
            cancellation.GetToken());
        return JsonAssetProcessor::Prepare(context, prepared, diagnostic);
    }
}

TEST_CASE("JSON scene processor accepts only canonical actor-local structured references")
{
    const String root = Globals::TemporaryFolder / (TEXT("JsonSceneReferences-") + Guid::New().ToString(Guid::FormatType::N));
    const String content = root / TEXT("Content");
    const String library = root / TEXT("Library");
    REQUIRE_FALSE(FileSystem::CreateDirectory(content));
    REQUIRE_FALSE(FileSystem::CreateDirectory(library));
    SCOPE_EXIT { FileSystem::DeleteDirectory(root, true); };

    AssetPipelineDiagnostic diagnostic;
    CHECK_FALSE(PrepareScene(root, content, library,
        R"({"sceneVersion":4,"objects":[{"fileId":1,"type":"FlaxEngine.Scene","Sun":{"kind":1,"guid":"00000000000000000000000000000000","fileId":2,"prefabInstanceFileId":0}},{"fileId":2,"type":"FlaxEngine.DirectionalLight","parentFileId":1}]})",
        diagnostic));

    CHECK(PrepareScene(root, content, library,
        R"({"sceneVersion":4,"objects":[{"fileId":1,"type":"FlaxEngine.Scene","Ref":{"kind":0,"guid":"00000000000000000000000000000000","fileId":2,"prefabInstanceFileId":0}}]})",
        diagnostic));
    CHECK(diagnostic.Code == AssetPipelineDiagnosticCode::InvalidMeta);

    CHECK(PrepareScene(root, content, library,
        R"({"sceneVersion":4,"objects":[{"fileId":1,"type":"FlaxEngine.Scene","Ref":{"kind":2,"guid":"00000000000000000000000000000000","fileId":2,"prefabInstanceFileId":0}}]})",
        diagnostic));
    CHECK(diagnostic.Code == AssetPipelineDiagnosticCode::InvalidMeta);

    CHECK(PrepareScene(root, content, library,
        R"({"sceneVersion":4,"objects":[{"fileId":1,"type":"FlaxEngine.Scene","Ref":{"kind":1,"guid":"00000000000000000000000000000000","fileId":0,"prefabInstanceFileId":0}}]})",
        diagnostic));
    CHECK(diagnostic.Code == AssetPipelineDiagnosticCode::InvalidMeta);

    CHECK(PrepareScene(root, content, library,
        R"({"sceneVersion":4,"objects":[{"fileId":1,"type":"FlaxEngine.Scene","Ref":{"kind":1,"guid":"00000000000000000000000000000000","fileId":2}}]})",
        diagnostic));
    CHECK(diagnostic.Code == AssetPipelineDiagnosticCode::InvalidMeta);

    CHECK(PrepareScene(root, content, library,
        R"({"sceneVersion":4,"objects":[{"fileId":1,"type":"FlaxEngine.Scene","Ref":{"kind":1,"guid":"00000000000000000000000000000000","fileId":2,"prefabInstanceFileId":3}}]})",
        diagnostic));
    CHECK(diagnostic.Code == AssetPipelineDiagnosticCode::InvalidMeta);

    CHECK(PrepareScene(root, content, library,
        R"({"sceneVersion":4,"objects":[{"fileId":1,"type":"FlaxEngine.Scene","Ref":{"kind":9,"guid":"00000000000000000000000000000000","fileId":2,"prefabInstanceFileId":0}}]})",
        diagnostic));
    CHECK(diagnostic.Code == AssetPipelineDiagnosticCode::InvalidMeta);

    CHECK(PrepareScene(root, content, library,
        R"({"sceneVersion":4,"objects":[{"fileId":1,"type":"FlaxEngine.Scene","Ref":{"guid":"00000000000000000000000000000000","fileId":2}}]})",
        diagnostic));
    CHECK(diagnostic.Code == AssetPipelineDiagnosticCode::InvalidMeta);
}

#endif
