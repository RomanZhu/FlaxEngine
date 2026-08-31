// Copyright (c) Wojciech Figat. All rights reserved.

#include "AssetModificationProcessor.h"
#include <algorithm>

AssetModificationProcessorRegistration::AssetModificationProcessorRegistration(AssetModificationProcessorRegistration&& other) noexcept
{
    *this = MoveTemp(other);
}

AssetModificationProcessorRegistration& AssetModificationProcessorRegistration::operator=(AssetModificationProcessorRegistration&& other) noexcept
{
    if (this != &other)
    {
        Reset();
        _registry = other._registry;
        _id = MoveTemp(other._id);
        other._registry = nullptr;
    }
    return *this;
}

AssetModificationProcessorRegistration::~AssetModificationProcessorRegistration()
{
    Reset();
}

void AssetModificationProcessorRegistration::Reset()
{
    if (_registry)
        _registry->Unregister(_id);
    _registry = nullptr;
    _id.Clear();
}

bool AssetModificationProcessorRegistry::Register(AssetModificationProcessorDescriptor descriptor, AssetModificationProcessorRegistration& registration, AssetPipelineDiagnostic& diagnostic)
{
    registration.Reset();
    if (descriptor.ID.IsEmpty() || !descriptor.Process.IsBinded())
    {
        diagnostic = AssetPipelineDiagnostic();
        diagnostic.Code = AssetPipelineDiagnosticCode::InvalidSettingsCombination;
        diagnostic.Stage = AssetPipelineDiagnosticStage::Configuration;
        diagnostic.ProcessorId = descriptor.ID;
        diagnostic.Message = TEXT("Modification processor identity or callback is invalid.");
        return true;
    }
    ScopeLock lock(_locker);
    if (_processors.ContainsKey(descriptor.ID))
    {
        diagnostic = AssetPipelineDiagnostic();
        diagnostic.Code = AssetPipelineDiagnosticCode::InvalidSettingsCombination;
        diagnostic.Stage = AssetPipelineDiagnosticStage::Configuration;
        diagnostic.ProcessorId = descriptor.ID;
        diagnostic.Message = TEXT("Modification processor ID is already registered.");
        return true;
    }
    registration._registry = this;
    registration._id = descriptor.ID;
    _processors.Add(descriptor.ID, MoveTemp(descriptor));
    diagnostic = AssetPipelineDiagnostic();
    return false;
}

bool AssetModificationProcessorRegistry::Process(const AssetModificationRequest& request, AssetModificationDecision& decision, AssetPipelineDiagnostic& diagnostic) const
{
    Array<AssetModificationProcessorDescriptor> processors;
    {
        ScopeLock lock(_locker);
        for (const auto& processor : _processors)
            processors.Add(processor.Value);
    }
    std::sort(processors.Get(), processors.Get() + processors.Count(), [](const AssetModificationProcessorDescriptor& a, const AssetModificationProcessorDescriptor& b)
    {
        if (a.Order != b.Order)
            return a.Order < b.Order;
        return a.ID < b.ID;
    });
    decision = AssetModificationDecision();
    for (const AssetModificationProcessorDescriptor& processor : processors)
    {
        if (processor.Process(request, decision, diagnostic))
            return true;
        if (!decision.Allowed || decision.Handled)
            return false;
    }
    diagnostic = AssetPipelineDiagnostic();
    return false;
}

void AssetModificationProcessorRegistry::Unregister(const String& id)
{
    ScopeLock lock(_locker);
    _processors.Remove(id);
}
