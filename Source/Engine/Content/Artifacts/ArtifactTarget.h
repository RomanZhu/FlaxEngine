// Copyright (c) Wojciech Figat. All rights reserved.

#pragma once

#include "ArtifactKey.h"
#include "Engine/Core/Collections/Array.h"
#include "Engine/Core/Types/Guid.h"

/// <summary>Independently selectable target dimensions that can affect an output key.</summary>
enum class ArtifactTargetDimension : uint32
{
    None = 0,
    Platform = 1 << 0,
    Architecture = 1 << 1,
    Graphics = 1 << 2,
    Configuration = 1 << 3,
    Quality = 1 << 4,
    TextureCompression = 1 << 5,
    AudioCodec = 1 << 6,
    ShaderCompiler = 1 << 7,
    Role = 1 << 8,
    FeatureFlags = 1 << 9,
    All = (1 << 10) - 1,
};

DECLARE_ENUM_OPERATORS(ArtifactTargetDimension);

/// <summary>Explicit semantic target dimensions used by artifact outputs.</summary>
struct FLAXENGINE_API ArtifactTarget
{
    StringAnsi Platform;
    StringAnsi Architecture;
    StringAnsi Graphics;
    StringAnsi Configuration;
    StringAnsi Quality;
    StringAnsi TextureCompression;
    StringAnsi AudioCodec;
    StringAnsi ShaderCompiler;
    StringAnsi Role;
    Array<StringAnsi> FeatureFlags;

    /// <summary>Builds a stable key using only the dimensions selected by the output.</summary>
    ArtifactKey BuildKey(ArtifactTargetDimension dimensions) const;
};

/// <summary>One human-readable typed component retained for key explanations.</summary>
struct ArtifactKeyComponent
{
    StringAnsi Name;
    StringAnsi Type;
    StringAnsi Value;
};

/// <summary>Length-prefixed, typed, domain-separated artifact key construction.</summary>
class FLAXENGINE_API ArtifactKeyBuilder
{
public:
    explicit ArtifactKeyBuilder(const StringAnsiView& domain = StringAnsiView("flax-artifact-key-v1"));

    void AddBool(const StringAnsiView& name, bool value);
    void AddUInt32(const StringAnsiView& name, uint32 value);
    void AddUInt64(const StringAnsiView& name, uint64 value);
    void AddString(const StringAnsiView& name, const StringAnsiView& value);
    void AddString(const StringAnsiView& name, const StringView& value);
    void AddGuid(const StringAnsiView& name, const Guid& value);
    void AddHash(const StringAnsiView& name, const ContentHash& value);
    void AddKey(const StringAnsiView& name, const ArtifactKey& value);
    void AddSortedStrings(const StringAnsiView& name, const Array<StringAnsi>& values);
    void AddTarget(const ArtifactTarget& target, ArtifactTargetDimension dimensions);

    ArtifactKey Finalize() const;
    const Array<ArtifactKeyComponent>& GetComponents() const
    {
        return _components;
    }

private:
    enum class FieldType : byte
    {
        Boolean = 1,
        UInt32 = 2,
        UInt64 = 3,
        String = 4,
        Guid = 5,
        ContentHash = 6,
        ArtifactKey = 7,
        StringCollection = 8,
    };

    ContentHasher _hasher;
    Array<ArtifactKeyComponent> _components;

    void AddField(FieldType type, const StringAnsiView& name, const void* data, uint32 length, const StringAnsiView& display);
    void AddRawUInt32(uint32 value);
};
