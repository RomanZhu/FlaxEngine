// Copyright (c) Wojciech Figat. All rights reserved.

#pragma once

#include "Engine/Content/AssetPipeline/AssetPipelineDiagnostics.h"
#include "Engine/Core/Collections/Dictionary.h"
#include "Engine/Core/Delegate.h"
#include "Engine/Core/NonCopyable.h"
#include "Engine/Platform/CriticalSection.h"

enum class AssetModificationKind : byte
{
    Create,
    Move,
    Delete,
    Save,
};

struct FLAXENGINE_API AssetModificationRequest
{
    AssetModificationKind Kind = AssetModificationKind::Create;
    String Path;
    String DestinationPath;
    bool IsDirectory = false;
};

struct FLAXENGINE_API AssetModificationDecision
{
    bool Allowed = true;
    bool Handled = false;
    String Message;
};

using AssetModificationCallback = Function<bool(const AssetModificationRequest&, AssetModificationDecision&, AssetPipelineDiagnostic&)>;

struct FLAXENGINE_API AssetModificationProcessorDescriptor
{
    String ID;
    int32 Order = 0;
    AssetModificationCallback Process;
};

class AssetModificationProcessorRegistry;

class FLAXENGINE_API AssetModificationProcessorRegistration : public NonCopyable
{
    friend AssetModificationProcessorRegistry;
    AssetModificationProcessorRegistry* _registry = nullptr;
    String _id;

public:
    AssetModificationProcessorRegistration() = default;
    AssetModificationProcessorRegistration(AssetModificationProcessorRegistration&& other) noexcept;
    AssetModificationProcessorRegistration& operator=(AssetModificationProcessorRegistration&& other) noexcept;
    ~AssetModificationProcessorRegistration();
    void Reset();
};

/// <summary>Ordered editor-facing interception points for source mutations.</summary>
class FLAXENGINE_API AssetModificationProcessorRegistry : public NonCopyable
{
    friend AssetModificationProcessorRegistration;
    mutable CriticalSection _locker;
    Dictionary<String, AssetModificationProcessorDescriptor> _processors;

public:
    bool Register(AssetModificationProcessorDescriptor descriptor, AssetModificationProcessorRegistration& registration, AssetPipelineDiagnostic& diagnostic);
    bool Process(const AssetModificationRequest& request, AssetModificationDecision& decision, AssetPipelineDiagnostic& diagnostic) const;

private:
    void Unregister(const String& id);
};
