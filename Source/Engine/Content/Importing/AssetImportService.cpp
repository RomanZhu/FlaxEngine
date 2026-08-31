// Copyright (c) Wojciech Figat. All rights reserved.

#include "AssetImportService.h"
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
        Array<AssetImporterRegistration> BuiltInRegistrations;
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

}

bool AssetImportService::EnsureInitialized(AssetPipelineDiagnostic& diagnostic)
{
    AssetImportServiceState& state = State();
    std::lock_guard<std::mutex> lock(state.Locker);
    InitializeCore(state);
    diagnostic = AssetPipelineDiagnostic();
    return false;
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
    diagnostic = AssetPipelineDiagnostic();
    return false;
}

bool AssetImportService::RegisterBuiltIn(const AssetProcessorDescriptor& implementation, AssetPipelineDiagnostic& diagnostic,
    AssetImporterBuildRequest requestBuild, AssetImporterBuildStatus getBuildStatus, int32 priority)
{
    AssetImportServiceState& state = State();
    std::lock_guard<std::mutex> lock(state.Locker);
    InitializeCore(state);
    Array<AssetImporterDescriptor> descriptors;
    state.Importers->GetDescriptors(descriptors);
    for (const AssetImporterDescriptor& descriptor : descriptors)
    {
        if (descriptor.ID == implementation.ID)
        {
            diagnostic = AssetPipelineDiagnostic();
            return false;
        }
    }
    AssetImporterDescriptor descriptor = AssetImporterDescriptor::FromBuildImplementation(implementation, priority);
    descriptor.RequestBuild = MoveTemp(requestBuild);
    descriptor.GetBuildStatus = MoveTemp(getBuildStatus);
    if (!descriptor.RequestBuild.IsBinded() || !descriptor.GetBuildStatus.IsBinded())
    {
        diagnostic = AssetPipelineDiagnostic();
        diagnostic.Code = AssetPipelineDiagnosticCode::InvalidSettingsCombination;
        diagnostic.Stage = AssetPipelineDiagnosticStage::Configuration;
        diagnostic.ProcessorId = implementation.ID;
        diagnostic.Message = TEXT("A built-in importer must provide generic build and status callbacks.");
        return true;
    }
    if (descriptor.Extensions.IsEmpty())
    {
        if (implementation.ID == TEXT("Flax.Binary"))
            descriptor.Fallback = AssetImporterFallback::Binary;
        else
        {
            diagnostic = AssetPipelineDiagnostic();
            diagnostic.Code = AssetPipelineDiagnosticCode::InvalidSettingsCombination;
            diagnostic.Stage = AssetPipelineDiagnosticStage::Configuration;
            diagnostic.ProcessorId = implementation.ID;
            diagnostic.Message = TEXT("A built-in importer must declare source extensions or be the default binary importer.");
            return true;
        }
    }
    AssetImporterRegistration registration;
    if (state.Importers->Register(MoveTemp(descriptor), registration, diagnostic))
        return true;
    state.BuiltInRegistrations.Add(MoveTemp(registration));
    return false;
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
    state.BuiltInRegistrations.Clear();
    state.Refresh.reset();
    state.CustomDependencies.reset();
    state.ModificationProcessors.reset();
    state.Postprocessors.reset();
    state.Planner.reset();
    state.Importers.reset();
}
