// Copyright (c) Wojciech Figat. All rights reserved.

#include "CustomDependencyRegistry.h"
#include <algorithm>

uint64 CustomDependencyRegistry::GetGeneration() const
{
    ScopeLock lock(_locker);
    return _generation;
}

bool CustomDependencyRegistry::Register(const StringView& name, const ContentHash& value, bool& changed, AssetPipelineDiagnostic& diagnostic)
{
    changed = false;
    if (name.IsEmpty() || value.IsZero())
    {
        diagnostic = AssetPipelineDiagnostic();
        diagnostic.Code = AssetPipelineDiagnosticCode::InvalidSettingsCombination;
        diagnostic.Stage = AssetPipelineDiagnosticStage::Configuration;
        diagnostic.Message = TEXT("Custom dependency name or value is invalid.");
        return true;
    }
    ScopeLock lock(_locker);
    const String key(name);
    ContentHash* existing = _values.TryGet(key);
    if (existing && *existing == value)
    {
        diagnostic = AssetPipelineDiagnostic();
        return false;
    }
    if (existing)
        *existing = value;
    else
        _values.Add(key, value);
    _changed.Add(key);
    _generation++;
    changed = true;
    diagnostic = AssetPipelineDiagnostic();
    return false;
}

bool CustomDependencyRegistry::Unregister(const StringView& name)
{
    ScopeLock lock(_locker);
    const String key(name);
    if (!_values.ContainsKey(key))
        return false;
    _values.Remove(key);
    _changed.Add(key);
    _generation++;
    return true;
}

bool CustomDependencyRegistry::TryGet(const StringView& name, ContentHash& value) const
{
    ScopeLock lock(_locker);
    const ContentHash* existing = _values.TryGet(String(name));
    if (!existing)
        return false;
    value = *existing;
    return true;
}

void CustomDependencyRegistry::ConsumeChanges(Array<String>& names)
{
    ScopeLock lock(_locker);
    names.Clear();
    for (const auto& bucket : _changed)
        names.Add(bucket.Item);
    std::sort(names.Get(), names.Get() + names.Count());
    _changed.Clear();
}
