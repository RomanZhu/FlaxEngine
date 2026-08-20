// Copyright (c) Wojciech Figat. All rights reserved.

#pragma once

#include "AssetProcessor.h"
#include "Engine/Core/Collections/Dictionary.h"
#include "Engine/Core/NonCopyable.h"
#include "Engine/Platform/ConditionVariable.h"
#include "Engine/Platform/CriticalSection.h"
#include <memory>

class AssetProcessorRegistry;

/// <summary>Stage for which a provider generation is pinned.</summary>
enum class AssetProcessorInvocationStage : byte
{
    Prepare,
    Build,
};

/// <summary>Move-only active provider generation lease.</summary>
class FLAXENGINE_API AssetProcessorLease : public NonCopyable
{
    friend AssetProcessorRegistry;

private:
    AssetProcessorRegistry* _registry = nullptr;
    String _id;
    uint64 _generation = 0;
    AssetProcessorDescriptor _descriptor;

public:
    AssetProcessorLease() = default;
    AssetProcessorLease(AssetProcessorLease&& other) noexcept;
    AssetProcessorLease& operator=(AssetProcessorLease&& other) noexcept;
    ~AssetProcessorLease();

    void Reset();
    bool IsValid() const
    {
        return _registry != nullptr;
    }
    const AssetProcessorDescriptor& Get() const
    {
        ASSERT(IsValid());
        return _descriptor;
    }
};

/// <summary>Move-only revocable registration handle tied to provider lifetime.</summary>
class FLAXENGINE_API AssetProcessorRegistration : public NonCopyable
{
    friend AssetProcessorRegistry;

private:
    AssetProcessorRegistry* _registry = nullptr;
    String _id;
    uint64 _generation = 0;

public:
    AssetProcessorRegistration() = default;
    AssetProcessorRegistration(AssetProcessorRegistration&& other) noexcept;
    AssetProcessorRegistration& operator=(AssetProcessorRegistration&& other) noexcept;
    ~AssetProcessorRegistration();

    void Reset();
    bool IsValid() const
    {
        return _registry != nullptr;
    }
    uint64 GetGeneration() const
    {
        return _generation;
    }
};

/// <summary>Thread-safe versioned registry shared by native, managed, and plugin processors.</summary>
class FLAXENGINE_API AssetProcessorRegistry : public NonCopyable
{
    friend AssetProcessorLease;
    friend AssetProcessorRegistration;

private:
    struct ProviderState
    {
        AssetProcessorDescriptor Descriptor;
        int32 ActiveInvocations = 0;
        bool Revoking = false;
    };

    mutable CriticalSection _locker;
    ConditionVariable _quiesced;
    Dictionary<String, std::shared_ptr<ProviderState>> _providers;
    uint64 _nextGeneration = 1;
    bool _requireThirdPartyIsolation = false;
    uint32 _engineApiLevel = 1;

public:
    AssetProcessorRegistry() = default;

    static AssetProcessorRegistry& Get();

    void SetTrustPolicy(bool requireThirdPartyIsolation);
    void SetEngineApiLevel(uint32 value);

    /// <summary>Registers a native or third-party descriptor. Returns true on failure.</summary>
    bool Register(AssetProcessorDescriptor descriptor, AssetProcessorRegistration& registration, AssetPipelineDiagnostic& diagnostic);

    /// <summary>Managed-provider bridge that uses the exact same validation and lifecycle path.</summary>
    bool RegisterManaged(AssetProcessorDescriptor descriptor, AssetProcessorRegistration& registration, AssetPipelineDiagnostic& diagnostic);

    /// <summary>Revokes a generation. Optional waiting cancels provider work and waits for active leases.</summary>
    bool Unregister(const StringView& id, uint64 generation, bool waitForQuiescence, AssetPipelineDiagnostic& diagnostic);

    bool TryAcquire(const StringView& id, AssetProcessorInvocationStage stage, AssetProcessorLease& lease, AssetPipelineDiagnostic& diagnostic);
    bool TryGetDescriptor(const StringView& id, AssetProcessorDescriptor& descriptor) const;
    void GetDescriptors(Array<AssetProcessorDescriptor>& descriptors) const;
    void Clear();

private:
    bool ValidateDescriptor(AssetProcessorDescriptor& descriptor, AssetPipelineDiagnostic& diagnostic) const;
    void ReleaseLease(const String& id, uint64 generation);
};
