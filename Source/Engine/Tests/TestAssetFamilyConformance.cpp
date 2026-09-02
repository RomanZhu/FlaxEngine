// Copyright (c) Wojciech Figat. All rights reserved.

#if COMPILE_WITH_TESTS && USE_EDITOR

#include "Engine/Content/AssetDatabase/AssetDatabaseServices.h"
#include "Engine/Content/Assets/RawDataAsset.h"
#include "Engine/Core/Log.h"
#include "Engine/Core/ScopeExit.h"
#include "Engine/Engine/Globals.h"
#include "Engine/Platform/File.h"
#include "Engine/Platform/FileSystem.h"
#include <ThirdParty/catch2/catch.hpp>

namespace
{
    struct SupportedFamilyEvidence
    {
        const char* Family;
        const char* Extension;
        const char* Processor;
        const char* Type;
        const char* EvidenceCase;
    };

    const SupportedFamilyEvidence SupportedFamilies[] =
    {
        { "texture", ".png", "Flax.Texture", "FlaxEngine.Texture", "Representative PNG texture survives editor lifecycle invalidation and cook" },
        { "model root", ".glb", "Flax.Model", "FlaxEngine.Model/SkinnedModel", "Representative GLB model family survives editor lifecycle invalidation and cook" },
        { "model mesh", ".glb", "Flax.Model", "FlaxEngine.Model", "Representative GLB model family survives editor lifecycle invalidation and cook" },
        { "model material", ".glb", "Flax.Model", "FlaxEngine.Material", "Representative GLB model family survives editor lifecycle invalidation and cook" },
        { "model animation", ".glb", "Flax.Model", "FlaxEngine.Animation", "Representative GLB model family survives editor lifecycle invalidation and cook" },
        { "scene", ".scene", "Flax.JsonDocument", "FlaxEngine.SceneAsset", "Current scene and prefab documents retain nested references for cook closure" },
        { "prefab", ".prefab", "Flax.JsonDocument", "FlaxEngine.Prefab", "Current scene and prefab documents retain nested references for cook closure" },
        { "particle emitter", ".particleemitter", "Flax.GraphDocument", "FlaxEngine.ParticleEmitter", "Project panel preserves authored particle and collision text lifecycle" },
        { "particle system", ".particlesystem", "Flax.ParticleSystem", "FlaxEngine.ParticleSystem", "Project panel preserves authored particle and collision text lifecycle" },
        { "collision data", ".collisiondata", "Flax.CollisionData", "FlaxEngine.CollisionData", "Project panel preserves authored particle and collision text lifecycle" },
        { "material", ".material", "Flax.GraphDocument", "FlaxEngine.Material", "Project panel preserves additional authored text family lifecycles" },
        { "material function", ".materialfunction", "Flax.GraphDocument", "FlaxEngine.MaterialFunction", "Project panel preserves additional authored text family lifecycles" },
        { "animation graph", ".animgraph", "Flax.GraphDocument", "FlaxEngine.AnimationGraph", "Project panel preserves additional authored text family lifecycles" },
        { "animation graph function", ".animgraphfunction", "Flax.GraphDocument", "FlaxEngine.AnimationGraphFunction", "Project panel preserves additional authored text family lifecycles" },
        { "visual script", ".visualscript", "Flax.GraphDocument", "FlaxEngine.VisualScript", "Project panel preserves additional authored text family lifecycles" },
        { "behavior tree", ".behaviortree", "Flax.GraphDocument", "FlaxEngine.BehaviorTree", "Project panel preserves additional authored text family lifecycles" },
        { "particle emitter function", ".particlefunction", "Flax.GraphDocument", "FlaxEngine.ParticleEmitterFunction", "Project panel preserves additional authored text family lifecycles" },
        { "material instance", ".materialinstance", "Flax.MaterialInstance", "FlaxEngine.MaterialInstance", "Project panel preserves additional authored text family lifecycles" },
        { "skeleton mask", ".skeletonmask", "Flax.SkeletonMask", "FlaxEngine.SkeletonMask", "Project panel preserves additional authored text family lifecycles" },
        { "scene animation", ".sceneanimation", "Flax.SceneAnimation", "FlaxEngine.SceneAnimation", "Project panel preserves additional authored text family lifecycles" },
    };

    struct DeferredFamily
    {
        const char* Family;
        const char* Extensions;
    };

    // Intentionally concise: these registered legacy importer families are outside the required production cohort.
    const DeferredFamily DeferredFamilies[] =
    {
        { "audio", ".wav/.mp3/.ogg" },
        { "font", ".ttf/.otf" },
        { "video", ".mp4/.webm/.mov/.mkv" },
        { "shader source", ".shader" },
        { "IES profile", ".ies" },
        { "generic text", ".txt" },
    };

    bool HasRegisteredCase(const char* name)
    {
        const auto& tests = Catch::getRegistryHub().getTestCaseRegistry().getAllTests();
        for (const Catch::TestCase& test : tests)
        {
            if (test.name == name)
                return true;
        }
        return false;
    }
}

TEST_CASE("Required asset families publish concise executable conformance evidence")
{
    REQUIRE(ARRAY_COUNT(SupportedFamilies) == 20);
    for (int32 i = 0; i < ARRAY_COUNT(SupportedFamilies); i++)
    {
        const SupportedFamilyEvidence& family = SupportedFamilies[i];
        INFO("family=" << family.Family << " extension=" << family.Extension << " processor=" << family.Processor <<
             " type=" << family.Type << " evidence=" << family.EvidenceCase);
        CHECK(family.Extension[0] == '.');
        CHECK(StringAnsiView(family.Type) != StringAnsiView("FlaxEngine.RawDataAsset"));
        CHECK(HasRegisteredCase(family.EvidenceCase));
        for (int32 j = 0; j < i; j++)
            CHECK(StringAnsiView(family.Family) != StringAnsiView(SupportedFamilies[j].Family));
    }

    REQUIRE(ARRAY_COUNT(DeferredFamilies) == 6);
    for (const DeferredFamily& family : DeferredFamilies)
    {
        INFO("deferred-family=" << family.Family << " extensions=" << family.Extensions);
        CHECK(family.Extensions[0] == '.');
    }

    const String root = Globals::ProjectContentFolder / (TEXT("__UnsupportedConformance_") + Guid::New().ToString(Guid::FormatType::N));
    const String source = root / TEXT("Unsupported.asset63unsupported");
    Array<String> refreshPaths;
    refreshPaths.Add(root);
    REQUIRE_FALSE(FileSystem::CreateDirectory(root));
    SCOPE_EXIT
    {
        FileSystem::DeleteDirectory(root, true);
        AssetPipelineService::RefreshSources(refreshPaths);
    };
    const char payload[] = "unsupported canonical source";
    REQUIRE_FALSE(File::WriteAllBytes(source, payload, ARRAY_COUNT(payload) - 1));

    Array<String> warningOrErrorOutput;
    Delegate<LogType, const StringView&>::FunctionType logHandler = [&warningOrErrorOutput](LogType type, const StringView& message)
    {
        if (type == LogType::Warning || type == LogType::Error || type == LogType::Fatal)
            warningOrErrorOutput.Add(String(message));
    };
    Log::Logger::OnMessage.Bind(logHandler);
    SCOPE_EXIT { Log::Logger::OnMessage.Unbind(logHandler); };

    Array<String> sourceRefresh;
    sourceRefresh.Add(source);
    REQUIRE_FALSE(AssetPipelineService::RefreshSources(sourceRefresh));
    AssetDatabaseRecordInfo unsupported;
    REQUIRE(AssetDatabaseQueryService::TryGetMainRecordAtPath(source, unsupported));
    CHECK(unsupported.TypeName == RawDataAsset::TypeName);
    CHECK(unsupported.ProcessorID == TEXT("Flax.Unsupported"));
    CHECK(unsupported.Status == AssetRecordStatus::UnsupportedProcessor);
    CHECK(AssetPipelineService::ImportAsset(source));
    CHECK_FALSE(AssetPipelineService::IsArtifactCurrent(unsupported.ID));

    const Array<AssetPipelineDiagnostic> diagnostics = AssetDatabaseQueryService::GetDiagnostics();
    REQUIRE(diagnostics.Count() == 1);
    CHECK(diagnostics[0].Code == AssetPipelineDiagnosticCode::ProcessorMissing);
    CHECK(diagnostics[0].AssetGuid == unsupported.ID);
    CHECK(FileSystem::AreFilePathsEqual(diagnostics[0].SourcePath, source));
    CHECK(warningOrErrorOutput.IsEmpty());
}

#endif
