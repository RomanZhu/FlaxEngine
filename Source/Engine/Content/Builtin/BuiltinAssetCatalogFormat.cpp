// Copyright (c) Wojciech Figat. All rights reserved.

#include "BuiltinAssetCatalogFormat.h"
#include "Engine/Content/Artifacts/ArtifactKey.h"
#include "Engine/Core/Collections/HashSet.h"
#include "Engine/Platform/FileSystem.h"
#include "Engine/Platform/StringUtils.h"

namespace
{
    constexpr uint32 CatalogMagic = 0x43494246; // FBIC
    constexpr int32 CatalogHeaderSize = sizeof(uint32) * 3 + sizeof(ContentHash);
    constexpr uint32 MaximumCatalogEntries = 1000000;
    constexpr uint32 MaximumCatalogStringBytes = 1024 * 1024;

    class CatalogWriter
    {
    public:
        Array<byte> Data;

        void WriteUInt32(uint32 value)
        {
            const byte bytes[] =
            {
                static_cast<byte>(value), static_cast<byte>(value >> 8),
                static_cast<byte>(value >> 16), static_cast<byte>(value >> 24),
            };
            Data.Add(bytes, ARRAY_COUNT(bytes));
        }

        void WriteUInt64(uint64 value)
        {
            byte bytes[8];
            for (int32 i = 0; i < ARRAY_COUNT(bytes); i++)
                bytes[i] = static_cast<byte>(value >> (i * 8));
            Data.Add(bytes, ARRAY_COUNT(bytes));
        }

        void WriteGuid(const Guid& value)
        {
            WriteUInt32(value.A);
            WriteUInt32(value.B);
            WriteUInt32(value.C);
            WriteUInt32(value.D);
        }

        void WriteObject(const AssetObjectId& value)
        {
            WriteGuid(value.Asset.Value);
            WriteUInt64(static_cast<uint64>(value.LocalId));
        }

        void WriteHash(const ContentHash& value)
        {
            Data.Add(value.Bytes, ARRAY_COUNT(value.Bytes));
        }

        void WriteString(const StringView& value)
        {
            const StringAnsi utf8(value);
            WriteUInt32(utf8.Length());
            if (utf8.HasChars())
                Data.Add(reinterpret_cast<const byte*>(utf8.Get()), utf8.Length());
        }
    };

    class CatalogReader
    {
        const byte* _data;
        uint32 _length;
        uint32 _position = 0;

    public:
        CatalogReader(const byte* data, uint32 length)
            : _data(data)
            , _length(length)
        {
        }

        bool ReadBytes(void* output, uint32 length)
        {
            if (length > _length - _position)
                return true;
            Platform::MemoryCopy(output, _data + _position, length);
            _position += length;
            return false;
        }

        bool ReadUInt32(uint32& value)
        {
            byte bytes[4];
            if (ReadBytes(bytes, ARRAY_COUNT(bytes)))
                return true;
            value = static_cast<uint32>(bytes[0]) | (static_cast<uint32>(bytes[1]) << 8) |
                    (static_cast<uint32>(bytes[2]) << 16) | (static_cast<uint32>(bytes[3]) << 24);
            return false;
        }

        bool ReadUInt64(uint64& value)
        {
            byte bytes[8];
            if (ReadBytes(bytes, ARRAY_COUNT(bytes)))
                return true;
            value = 0;
            for (int32 i = 0; i < ARRAY_COUNT(bytes); i++)
                value |= static_cast<uint64>(bytes[i]) << (i * 8);
            return false;
        }

        bool ReadGuid(Guid& value)
        {
            return ReadUInt32(value.A) || ReadUInt32(value.B) || ReadUInt32(value.C) || ReadUInt32(value.D);
        }

        bool ReadObject(AssetObjectId& value)
        {
            Guid guid;
            uint64 localId;
            if (ReadGuid(guid) || ReadUInt64(localId))
                return true;
            value = AssetObjectId(AssetGuid(guid), static_cast<int64>(localId));
            return false;
        }

        bool ReadHash(ContentHash& value)
        {
            return ReadBytes(value.Bytes, ARRAY_COUNT(value.Bytes));
        }

        bool ReadString(String& value)
        {
            uint32 length;
            if (ReadUInt32(length) || length > MaximumCatalogStringBytes || length > _length - _position)
                return true;
            value = String(StringAnsiView(reinterpret_cast<const char*>(_data + _position), length));
            _position += length;
            return false;
        }

        bool AtEnd() const
        {
            return _position == _length;
        }
    };

    bool Fail(AssetPipelineDiagnostic& diagnostic, const StringView& message, const Guid& id = Guid::Empty)
    {
        diagnostic = AssetPipelineDiagnostic();
        diagnostic.Code = AssetPipelineDiagnosticCode::ArtifactInvalid;
        diagnostic.Stage = AssetPipelineDiagnosticStage::Configuration;
        diagnostic.AssetGuid = id;
        diagnostic.Message = message;
        return true;
    }

    bool IsPortableRelativePath(const StringView& value)
    {
        if (value.IsEmpty() || value.StartsWith(TEXT("/")) || value.StartsWith(TEXT("\\")) || value.Contains(TEXT(":")) ||
            value.Contains(TEXT("\\")) || value.Contains(TEXT("//")) || value == TEXT(".") || value == TEXT("..") ||
            value.StartsWith(TEXT("../")) || value.Contains(TEXT("/../")) || value.EndsWith(TEXT("/..")))
            return false;
        return FileSystem::GetExtension(value).ToLower() == TEXT("flax");
    }

    bool IsStringBounded(const StringView& value)
    {
        return value.HasChars() && StringAnsi(value).Length() <= MaximumCatalogStringBytes;
    }

    bool IsEntryValid(const BuiltinAssetCatalogSerializedEntry& entry)
    {
        return entry.ObjectID.IsValid() && IsStringBounded(entry.TypeName) && IsStringBounded(entry.RelativePath) &&
            IsStringBounded(entry.Uri) && IsPortableRelativePath(entry.RelativePath) &&
            entry.Uri.StartsWith(TEXT("builtin://"), StringSearchCase::IgnoreCase);
    }

    bool ValidateEntries(const Array<BuiltinAssetCatalogSerializedEntry>& entries, AssetPipelineDiagnostic& diagnostic)
    {
        HashSet<AssetObjectId> objects;
        HashSet<String> uris;
        for (const BuiltinAssetCatalogSerializedEntry& entry : entries)
        {
            if (!IsEntryValid(entry))
                return Fail(diagnostic, TEXT("Built-in catalog contains an invalid object identity, type, path, or URI."), entry.ObjectID.Asset.Value);
            if (!objects.Add(entry.ObjectID) || !uris.Add(entry.Uri.ToLower()))
                return Fail(diagnostic, TEXT("Built-in catalog contains a duplicate object identity or URI."), entry.ObjectID.Asset.Value);
        }
        return false;
    }
}

bool BuiltinAssetCatalogFormat::ToBytes(const Array<BuiltinAssetCatalogSerializedEntry>& entries, Array<byte>& output,
    AssetPipelineDiagnostic& diagnostic)
{
    output.Clear();
    if (entries.IsEmpty() || entries.Count() > MaximumCatalogEntries)
        return Fail(diagnostic, TEXT("Built-in catalog entry count is invalid."));
    if (ValidateEntries(entries, diagnostic))
        return true;

    CatalogWriter payload;
    payload.WriteUInt32(entries.Count());
    for (const BuiltinAssetCatalogSerializedEntry& entry : entries)
    {
        payload.WriteObject(entry.ObjectID);
        payload.WriteString(entry.TypeName);
        payload.WriteString(entry.RelativePath);
        payload.WriteString(entry.Uri);
    }

    CatalogWriter catalog;
    catalog.WriteUInt32(CatalogMagic);
    catalog.WriteUInt32(Version);
    catalog.WriteUInt32(payload.Data.Count());
    catalog.WriteHash(ContentHash::Compute(payload.Data.Get(), payload.Data.Count()));
    catalog.Data.Add(payload.Data.Get(), payload.Data.Count());
    output = MoveTemp(catalog.Data);
    diagnostic = AssetPipelineDiagnostic();
    return false;
}

bool BuiltinAssetCatalogFormat::FromBytes(const Span<byte>& input, Array<BuiltinAssetCatalogSerializedEntry>& entries,
    AssetPipelineDiagnostic& diagnostic)
{
    entries.Clear();
    if (input.Length() < CatalogHeaderSize)
        return Fail(diagnostic, TEXT("Built-in catalog header is missing or truncated."));

    CatalogReader header(input.Get(), input.Length());
    uint32 magic;
    uint32 version;
    uint32 payloadSize;
    ContentHash payloadHash;
    if (header.ReadUInt32(magic) || header.ReadUInt32(version) || header.ReadUInt32(payloadSize) || header.ReadHash(payloadHash) ||
        magic != CatalogMagic || version != Version || payloadSize != static_cast<uint32>(input.Length() - CatalogHeaderSize))
        return Fail(diagnostic, TEXT("Built-in catalog header, version, or payload size is invalid."));

    const byte* payloadBytes = input.Get() + CatalogHeaderSize;
    if (ContentHash::Compute(payloadBytes, payloadSize) != payloadHash)
        return Fail(diagnostic, TEXT("Built-in catalog payload checksum is invalid."));

    CatalogReader payload(payloadBytes, payloadSize);
    uint32 count;
    if (payload.ReadUInt32(count) || count == 0 || count > MaximumCatalogEntries)
        return Fail(diagnostic, TEXT("Built-in catalog entry count is invalid."));
    entries.Resize(count, false);
    for (BuiltinAssetCatalogSerializedEntry& entry : entries)
    {
        if (payload.ReadObject(entry.ObjectID) || payload.ReadString(entry.TypeName) ||
            payload.ReadString(entry.RelativePath) || payload.ReadString(entry.Uri))
            return Fail(diagnostic, TEXT("Built-in catalog contains a truncated entry."));
    }
    if (!payload.AtEnd())
        return Fail(diagnostic, TEXT("Built-in catalog contains unexpected trailing data."));
    if (ValidateEntries(entries, diagnostic))
        return true;
    diagnostic = AssetPipelineDiagnostic();
    return false;
}

bool BuiltinAssetCatalogFormat::IsLegacyVersion(const Span<byte>& input)
{
    if (input.Length() < sizeof(uint32) * 2)
        return false;
    CatalogReader header(input.Get(), input.Length());
    uint32 magic;
    uint32 version;
    return !header.ReadUInt32(magic) && !header.ReadUInt32(version) && magic == CatalogMagic && version == 1;
}
