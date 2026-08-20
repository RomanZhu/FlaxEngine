// Copyright (c) Wojciech Figat. All rights reserved.

#pragma once

#include "Engine/Core/Types/String.h"

/// <summary>A stable 256-bit SHA-256 content digest.</summary>
struct FLAXENGINE_API ContentHash
{
    union
    {
        byte Bytes[32];
        uint32 Values[8];
    };

    ContentHash();

    bool operator==(const ContentHash& other) const;
    bool operator!=(const ContentHash& other) const;
    bool IsZero() const;

    /// <summary>Formats the digest as 64 lowercase hexadecimal characters.</summary>
    StringAnsi ToString() const;

    /// <summary>Parses the canonical 64-character hexadecimal form. Returns true on failure.</summary>
    static bool Parse(const StringAnsiView& text, ContentHash& value);

    /// <summary>Parses the canonical 64-character hexadecimal form. Returns true on failure.</summary>
    static bool Parse(const StringView& text, ContentHash& value);

    /// <summary>Hashes a memory region.</summary>
    static ContentHash Compute(const void* data, uint64 length);
};

/// <summary>Streaming SHA-256 state used for content and typed artifact keys.</summary>
class FLAXENGINE_API ContentHasher
{
public:
    ContentHasher();

    void Reset();
    void Update(const void* data, uint64 length);
    ContentHash Finalize() const;

private:
    uint32 _state[8];
    byte _block[64];
    uint64 _totalBytes;
    uint32 _blockBytes;

    void Transform(const byte* block);
};

/// <summary>A domain-separated key selecting one immutable generated artifact.</summary>
struct FLAXENGINE_API ArtifactKey
{
    ContentHash Digest;

    ArtifactKey() = default;
    explicit ArtifactKey(const ContentHash& digest)
        : Digest(digest)
    {
    }

    bool operator==(const ArtifactKey& other) const
    {
        return Digest == other.Digest;
    }

    bool operator!=(const ArtifactKey& other) const
    {
        return Digest != other.Digest;
    }

    bool IsZero() const
    {
        return Digest.IsZero();
    }

    StringAnsi ToString() const
    {
        return Digest.ToString();
    }

    static bool Parse(const StringAnsiView& text, ArtifactKey& value);
    static bool Parse(const StringView& text, ArtifactKey& value);
};

inline uint32 GetHash(const ContentHash& key)
{
    uint32 result = key.Values[0];
    for (int32 i = 1; i < 8; i++)
        CombineHash(result, key.Values[i]);
    return result;
}

inline uint32 GetHash(const ArtifactKey& key)
{
    return GetHash(key.Digest);
}
