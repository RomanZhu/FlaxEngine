// Copyright (c) Wojciech Figat. All rights reserved.

#pragma once

#include "AssetImportPlanner.h"
#include "Engine/Core/NonCopyable.h"
#include "Engine/Platform/ConditionVariable.h"
#include "Engine/Platform/CriticalSection.h"
#include <memory>

struct FLAXENGINE_API AssetImportCompletion
{
    AssetGuid Asset;
    String SourcePath;
    ArtifactKey Artifact;
    bool Succeeded = false;
    Array<AssetPipelineDiagnostic> Diagnostics;
};

using AssetPostprocessBatchCallback = Function<bool(const Array<AssetImportCompletion>&, bool&, AssetPipelineDiagnostic&)>;
using AssetPreprocessCallback = Function<bool(const AssetImportPlan&, bool&, AssetPipelineDiagnostic&)>;

struct FLAXENGINE_API AssetPostprocessorDescriptor
{
    String ID;
    uint32 Version = 1;
    ContentHash ImplementationHash;
    int32 Order = 0;
    AssetPreprocessCallback Preprocess;
    AssetPostprocessBatchCallback ProcessBatch;
};

class AssetPostprocessorRegistry;

class FLAXENGINE_API AssetPostprocessorRegistration : public NonCopyable
{
    friend AssetPostprocessorRegistry;
    AssetPostprocessorRegistry* _registry = nullptr;
    String _id;
    uint64 _providerGeneration = 0;

public:
    AssetPostprocessorRegistration() = default;
    AssetPostprocessorRegistration(AssetPostprocessorRegistration&& other) noexcept;
    AssetPostprocessorRegistration& operator=(AssetPostprocessorRegistration&& other) noexcept;
    ~AssetPostprocessorRegistration();
    void Reset();
    bool IsValid() const { return _registry != nullptr; }
};

/// <summary>Deterministically ordered, lifetime-safe batch postprocessor registry.</summary>
class FLAXENGINE_API AssetPostprocessorRegistry : public NonCopyable
{
    friend AssetPostprocessorRegistration;

    struct State
    {
        AssetPostprocessorDescriptor Descriptor;
        uint64 ProviderGeneration = 0;
        int32 ActiveCalls = 0;
        bool Revoking = false;
    };

    mutable CriticalSection _locker;
    ConditionVariable _quiesced;
    Dictionary<String, std::shared_ptr<State>> _states;
    uint64 _generation = 1;
    uint64 _nextProviderGeneration = 1;

public:
    uint64 GetGeneration() const;
    ArtifactKey GetVersionKey() const;
    bool Register(AssetPostprocessorDescriptor descriptor, AssetPostprocessorRegistration& registration, AssetPipelineDiagnostic& diagnostic);
    bool RunPreprocess(const AssetImportPlan& plan, bool& sourceChanged, AssetPipelineDiagnostic& diagnostic);
    bool RunBatch(const Array<AssetImportCompletion>& completed, bool& sourceChanged, AssetPipelineDiagnostic& diagnostic);

private:
    void Unregister(const String& id, uint64 providerGeneration);
    void Release(const String& id, uint64 providerGeneration);
};
