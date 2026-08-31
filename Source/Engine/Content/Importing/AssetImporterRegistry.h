// Copyright (c) Wojciech Figat. All rights reserved.

#pragma once

#include "AssetImporter.h"
#include "Engine/Core/NonCopyable.h"
#include "Engine/Platform/ConditionVariable.h"
#include "Engine/Platform/CriticalSection.h"
#include <memory>

class AssetImporterRegistry;

struct FLAXENGINE_API AssetImporterSelectionRequest
{
    String SourcePath;
    String ExplicitImporterID;
    bool PreferTextFallback = false;
};
class FLAXENGINE_API AssetImporterLease : public NonCopyable
{
    friend AssetImporterRegistry;
    AssetImporterRegistry* _registry = nullptr;
    String _id;
    uint64 _providerGeneration = 0;
    AssetImporterDescriptor _descriptor;

public:
    AssetImporterLease() = default;
    AssetImporterLease(AssetImporterLease&& other) noexcept;
    AssetImporterLease& operator=(AssetImporterLease&& other) noexcept;
    ~AssetImporterLease();

    void Reset();
    bool IsValid() const { return _registry != nullptr; }
    const AssetImporterDescriptor& Get() const { ASSERT(IsValid()); return _descriptor; }
};

class FLAXENGINE_API AssetImporterRegistration : public NonCopyable
{
    friend AssetImporterRegistry;
    AssetImporterRegistry* _registry = nullptr;
    String _id;
    uint64 _providerGeneration = 0;

public:
    AssetImporterRegistration() = default;
    AssetImporterRegistration(AssetImporterRegistration&& other) noexcept;
    AssetImporterRegistration& operator=(AssetImporterRegistration&& other) noexcept;
    ~AssetImporterRegistration();

    void Reset();
    bool IsValid() const { return _registry != nullptr; }
};

/// <summary>Versioned importer registry with deterministic priority and importer-ID selection.</summary>
class FLAXENGINE_API AssetImporterRegistry : public NonCopyable
{
    friend AssetImporterLease;
    friend AssetImporterRegistration;

    struct ProviderState
    {
        AssetImporterDescriptor Descriptor;
        uint64 ProviderGeneration = 0;
        int32 ActiveLeases = 0;
        bool Revoking = false;
    };

    mutable CriticalSection _locker;
    ConditionVariable _quiesced;
    Dictionary<String, std::shared_ptr<ProviderState>> _providers;
    uint64 _generation = 1;
    uint64 _nextProviderGeneration = 1;

public:
    uint64 GetGeneration() const;
    bool Register(AssetImporterDescriptor descriptor, AssetImporterRegistration& registration, AssetPipelineDiagnostic& diagnostic);
    bool Unregister(const StringView& id, uint64 providerGeneration, bool waitForLeases, AssetPipelineDiagnostic& diagnostic);
    bool Resolve(const AssetImporterSelectionRequest& request, AssetImporterLease& lease, AssetPipelineDiagnostic& diagnostic);
    bool TryAcquire(const StringView& id, AssetImporterLease& lease, AssetPipelineDiagnostic& diagnostic);
    void GetDescriptors(Array<AssetImporterDescriptor>& descriptors) const;
    void Clear();

private:
    bool Validate(AssetImporterDescriptor& descriptor, AssetPipelineDiagnostic& diagnostic) const;
    void ReleaseLease(const String& id, uint64 providerGeneration);
};
