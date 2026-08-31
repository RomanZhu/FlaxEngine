// Copyright (c) Wojciech Figat. All rights reserved.

#pragma once

#include "Engine/Content/Artifacts/ArtifactKey.h"
#include "Engine/Content/AssetPipeline/AssetPipelineDiagnostics.h"
#include "Engine/Core/Collections/Dictionary.h"
#include "Engine/Core/Collections/HashSet.h"
#include "Engine/Core/NonCopyable.h"
#include "Engine/Platform/CriticalSection.h"

/// <summary>Versioned named hashes for inputs not represented by source files or assets.</summary>
class FLAXENGINE_API CustomDependencyRegistry : public NonCopyable
{
    mutable CriticalSection _locker;
    Dictionary<String, ContentHash> _values;
    HashSet<String> _changed;
    uint64 _generation = 1;

public:
    uint64 GetGeneration() const;
    bool Register(const StringView& name, const ContentHash& value, bool& changed, AssetPipelineDiagnostic& diagnostic);
    bool Unregister(const StringView& name);
    bool TryGet(const StringView& name, ContentHash& value) const;
    void ConsumeChanges(Array<String>& names);
};
