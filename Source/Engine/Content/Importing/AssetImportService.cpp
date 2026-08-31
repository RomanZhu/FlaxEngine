// Copyright (c) Wojciech Figat. All rights reserved.

#include "AssetImportService.h"
#include "Engine/Content/Build/AssetProcessorRegistry.h"
#include <memory>
#include <mutex>

namespace
{
    struct AssetImportServiceState
    {
        std::mutex Locker;
        std::unique_ptr<AssetImporterRegistry> Importers;
        std::unique_ptr<AssetImportPlanner> Planner;
        std::unique_ptr<AssetImportScheduler> Scheduler;
        std::unique_ptr<AssetPostprocessorRegistry> Postprocessors;
        std::unique_ptr<AssetModificationProcessorRegistry> ModificationProcessors;
        std::unique_ptr<CustomDependencyRegistry> CustomDependencies;
        std::unique_ptr<AssetRefreshCoordinator> Refresh;
        Array<AssetImporterRegistration> ProcessorBridges;
        AssetBuildService* Builds = nullptr;
    };

    AssetImportServiceState& State()
    {
        static AssetImportServiceState state;
        return state;
    }

    void InitializeCore(AssetImportServiceState& state)
    {
        if (state.Importers)
            return;
        state.Importers = std::make_unique<AssetImporterRegistry>();
        state.Planner = std::make_unique<AssetImportPlanner>(*state.Importers);
        state.Postprocessors = std::make_unique<AssetPostprocessorRegistry>();
        state.ModificationProcessors = std::make_unique<AssetModificationProcessorRegistry>();
        state.CustomDependencies = std::make_unique<CustomDependencyRegistry>();
        state.Refresh = std::make_unique<AssetRefreshCoordinator>(*state.Importers, *state.Planner, *state.Postprocessors);
    }

    bool Synchronize(AssetImportServiceState& state, AssetPipelineDiagnostic& diagnostic)
    {
        Array<AssetProcessorDescriptor> processors;
        AssetProcessorRegistry::Get().GetDescriptors(processors);
        Array<AssetImporterDescriptor> importers;
        state.Importers->GetDescriptors(importers);
        for (const AssetProcessorDescriptor& processor : processors)
        {
            bool exists = false;
            for (const AssetImporterDescriptor& importer : importers)
            {
                if (importer.ID == processor.ID)
                {
                    exists = true;
                    break;
                }
            }
            if (exists)
                continue;
            AssetImporterRegistration registration;
            if (state.Importers->Register(AssetImporterDescriptor::FromProcessor(processor), registration, diagnostic))
                return true;
            state.ProcessorBridges.Add(MoveTemp(registration));
        }
        diagnostic = AssetPipelineDiagnostic();
        return false;
    }
}

bool AssetImportService::EnsureInitialized(AssetPipelineDiagnostic& diagnostic)
{
    AssetImportServiceState& state = State();
    std::lock_guard<std::mutex> lock(state.Locker);
    InitializeCore(state);
    return Synchronize(state, diagnostic);
}

bool AssetImportService::AttachBuildService(AssetBuildService& builds, AssetPipelineDiagnostic& diagnostic)
{
    AssetImportServiceState& state = State();
    std::lock_guard<std::mutex> lock(state.Locker);
    InitializeCore(state);
    if (state.Builds && state.Builds != &builds)
    {
        diagnostic = AssetPipelineDiagnostic();
        diagnostic.Code = AssetPipelineDiagnosticCode::InvalidSettingsCombination;
        diagnostic.Stage = AssetPipelineDiagnosticStage::Configuration;
        diagnostic.Message = TEXT("Asset import service is already attached to another build service.");
        return true;
    }
    if (!state.Scheduler)
        state.Scheduler = std::make_unique<AssetImportScheduler>(builds);
    state.Builds = &builds;
    return Synchronize(state, diagnostic);
}

bool AssetImportService::SynchronizeProcessorDescriptors(AssetPipelineDiagnostic& diagnostic)
{
    AssetImportServiceState& state = State();
    std::lock_guard<std::mutex> lock(state.Locker);
    InitializeCore(state);
    return Synchronize(state, diagnostic);
}

bool AssetImportService::IsInitialized()
{
    AssetImportServiceState& state = State();
    std::lock_guard<std::mutex> lock(state.Locker);
    return state.Importers != nullptr;
}

#define GET_IMPORT_SERVICE_MEMBER(method, member, type) \
    type* AssetImportService::method() \
    { \
        AssetImportServiceState& state = State(); \
        std::lock_guard<std::mutex> lock(state.Locker); \
        return state.member.get(); \
    }

GET_IMPORT_SERVICE_MEMBER(GetImporterRegistry, Importers, AssetImporterRegistry)
GET_IMPORT_SERVICE_MEMBER(GetPlanner, Planner, AssetImportPlanner)
GET_IMPORT_SERVICE_MEMBER(GetScheduler, Scheduler, AssetImportScheduler)
GET_IMPORT_SERVICE_MEMBER(GetPostprocessorRegistry, Postprocessors, AssetPostprocessorRegistry)
GET_IMPORT_SERVICE_MEMBER(GetModificationProcessorRegistry, ModificationProcessors, AssetModificationProcessorRegistry)
GET_IMPORT_SERVICE_MEMBER(GetCustomDependencyRegistry, CustomDependencies, CustomDependencyRegistry)
GET_IMPORT_SERVICE_MEMBER(GetRefreshCoordinator, Refresh, AssetRefreshCoordinator)

#undef GET_IMPORT_SERVICE_MEMBER

void AssetImportService::Shutdown()
{
    AssetImportServiceState& state = State();
    std::lock_guard<std::mutex> lock(state.Locker);
    state.Scheduler.reset();
    state.Builds = nullptr;
    state.ProcessorBridges.Clear();
    state.Refresh.reset();
    state.CustomDependencies.reset();
    state.ModificationProcessors.reset();
    state.Postprocessors.reset();
    state.Planner.reset();
    state.Importers.reset();
}
