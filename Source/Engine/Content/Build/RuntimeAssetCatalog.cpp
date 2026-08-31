// Copyright (c) Wojciech Figat. All rights reserved.

#include "RuntimeAssetCatalog.h"
#include "Engine/Core/Collections/HashSet.h"
#include "Engine/Core/ScopeExit.h"
#include "Engine/Core/Types/DataContainer.h"
#include "Engine/Platform/File.h"
#include "Engine/Platform/FileSystem.h"
#include <algorithm>

namespace
{
    constexpr uint32 CatalogMagic = 0x54414346; // FCAT
    constexpr int32 HeaderSize = 4 + 4 + 4 + 32;
    constexpr uint32 MaximumEntries = 10000000;
    constexpr uint32 MaximumStringBytes = 1024 * 1024;
    constexpr uint32 MaximumDependenciesPerEntry = 1000000;

    bool Less(const AssetObjectId& a, const AssetObjectId& b)
    {
        const Guid& left = a.Asset.Value;
        const Guid& right = b.Asset.Value;
        if (left.A != right.A)
            return left.A < right.A;
        if (left.B != right.B)
            return left.B < right.B;
        if (left.C != right.C)
            return left.C < right.C;
        if (left.D != right.D)
            return left.D < right.D;
        return a.LocalId < b.LocalId;
    }

    bool Fail(AssetPipelineDiagnostic& diagnostic, const StringView& path, const StringView& message)
    {
        diagnostic = AssetPipelineDiagnostic();
        diagnostic.Code = AssetPipelineDiagnosticCode::ArtifactInvalid;
        diagnostic.Stage = AssetPipelineDiagnosticStage::Cook;
        diagnostic.SourcePath = path;
        diagnostic.Message = message;
        return true;
    }

    class CatalogWriter
    {
    public:
        Array<byte> Data;

        void WriteByte(byte value)
        {
            Data.Add(value);
        }

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
            for (int32 i = 0; i < 8; i++)
                bytes[i] = static_cast<byte>(value >> (i * 8));
            Data.Add(bytes, ARRAY_COUNT(bytes));
        }

        void WriteHash(const ContentHash& value)
        {
            Data.Add(value.Bytes, ARRAY_COUNT(value.Bytes));
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

        void WriteString(const StringAnsiView& value)
        {
            WriteUInt32(value.Length());
            if (value.HasChars())
                Data.Add(reinterpret_cast<const byte*>(value.Get()), value.Length());
        }
    };

    class CatalogReader
    {
    private:
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

        bool ReadByte(byte& value)
        {
            return ReadBytes(&value, 1);
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
            for (int32 i = 0; i < 8; i++)
                value |= static_cast<uint64>(bytes[i]) << (i * 8);
            return false;
        }

        bool ReadHash(ContentHash& value)
        {
            return ReadBytes(value.Bytes, ARRAY_COUNT(value.Bytes));
        }

        bool ReadGuid(Guid& value)
        {
            return ReadUInt32(value.A) || ReadUInt32(value.B) || ReadUInt32(value.C) || ReadUInt32(value.D);
        }

        bool ReadObject(AssetObjectId& value)
        {
            uint64 localId;
            Guid guid;
            if (ReadGuid(guid) || ReadUInt64(localId))
                return true;
            value = AssetObjectId(AssetGuid(guid), static_cast<int64>(localId));
            return false;
        }

        bool ReadString(StringAnsi& value)
        {
            uint32 length;
            if (ReadUInt32(length) || length > MaximumStringBytes || length > _length - _position)
                return true;
            value.Set(reinterpret_cast<const char*>(_data + _position), length);
            _position += length;
            return false;
        }

        bool AtEnd() const
        {
            return _position == _length;
        }
    };

    bool IsTextValid(const StringAnsiView& value)
    {
        if (value.IsEmpty() || value.Length() > MaximumStringBytes)
            return false;
        for (int32 i = 0; i < value.Length(); i++)
        {
            const byte c = static_cast<byte>(value[i]);
            if (c == 0 || c < 0x20)
                return false;
        }
        return true;
    }
}

bool RuntimeAssetCatalog::IsPackageNameValid(const StringAnsiView& value)
{
    if (!IsTextValid(value) || value[0] == '/' || value.Contains("\\") || value.Contains(":") ||
        value.Contains("//") || value.StartsWith("./") || value.Contains("/./") ||
        value == "." || value == ".." || value.StartsWith("../") || value.Contains("/../") || value.EndsWith("/.."))
        return false;
    StringAnsi lower(value);
    lower = lower.ToLower();
    if (lower == "content" || lower.StartsWith("content/") || lower.Contains("/content/") ||
        lower == "assets" || lower.StartsWith("assets/") || lower.Contains("/assets/") ||
        lower == "canonicalsources" || lower.StartsWith("canonicalsources/") || lower.Contains("/canonicalsources/") ||
        lower == "library" || lower.StartsWith("library/") || lower.Contains("/library/"))
        return false;
    return true;
}

bool RuntimeAssetCatalog::Set(const StringAnsiView& buildID, const ContentHash& targetHash, const Array<RuntimeAssetCatalogEntry>& entries,
    AssetPipelineDiagnostic& diagnostic)
{
    _buildID = StringAnsi(buildID);
    _targetHash = targetHash;
    _entries = entries;
    if (_entries.Count() > 1)
    {
        std::sort(_entries.Get(), _entries.Get() + _entries.Count(), [](const RuntimeAssetCatalogEntry& a, const RuntimeAssetCatalogEntry& b)
        {
            return Less(a.Object, b.Object);
        });
    }
    for (RuntimeAssetCatalogEntry& entry : _entries)
    {
        if (entry.Dependencies.Count() > 1)
            std::sort(entry.Dependencies.Get(), entry.Dependencies.Get() + entry.Dependencies.Count(), Less);
    }
    return ValidateCanonical(diagnostic);
}

bool RuntimeAssetCatalog::ValidateCanonical(AssetPipelineDiagnostic& diagnostic) const
{
    if (!IsTextValid(_buildID) || _targetHash.IsZero() || _entries.IsEmpty() || _entries.Count() > MaximumEntries)
        return Fail(diagnostic, StringView::Empty, TEXT("Runtime catalog requires a build ID, target hash, and bounded object entries."));

    HashSet<AssetObjectId> objects;
    for (int32 i = 0; i < _entries.Count(); i++)
    {
        const RuntimeAssetCatalogEntry& entry = _entries[i];
        if (!entry.Object.IsValid() || !IsTextValid(entry.TypeName) || !IsPackageNameValid(entry.PackageName) || entry.Size == 0 ||
            entry.Offset > MAX_uint64 - entry.Size || entry.Content.IsZero() || entry.Compression > RuntimeAssetCompression::Zstd ||
            entry.Dependencies.Count() > MaximumDependenciesPerEntry || !objects.Add(entry.Object))
            return Fail(diagnostic, StringView::Empty, TEXT("Runtime catalog contains an invalid or duplicate object entry."));
        if (i != 0 && !Less(_entries[i - 1].Object, entry.Object))
            return Fail(diagnostic, StringView::Empty, TEXT("Runtime catalog object entries are not in canonical order."));
        for (int32 dependencyIndex = 0; dependencyIndex < entry.Dependencies.Count(); dependencyIndex++)
        {
            const AssetObjectId& dependency = entry.Dependencies[dependencyIndex];
            if (!dependency.IsValid() || dependency == entry.Object ||
                (dependencyIndex != 0 && !Less(entry.Dependencies[dependencyIndex - 1], dependency)))
                return Fail(diagnostic, StringView::Empty, TEXT("Runtime catalog contains an invalid, duplicate, or self object dependency."));
        }
    }
    for (const RuntimeAssetCatalogEntry& entry : _entries)
    {
        for (const AssetObjectId& dependency : entry.Dependencies)
        {
            if (!objects.Contains(dependency))
                return Fail(diagnostic, StringView::Empty, TEXT("Runtime catalog dependency is absent from the object-level catalog."));
        }
    }
    diagnostic = AssetPipelineDiagnostic();
    return false;
}

bool RuntimeAssetCatalog::TryGet(const AssetObjectId& object, RuntimeAssetCatalogEntry& result) const
{
    int32 left = 0;
    int32 right = _entries.Count() - 1;
    while (left <= right)
    {
        const int32 middle = left + (right - left) / 2;
        const RuntimeAssetCatalogEntry& entry = _entries[middle];
        if (entry.Object == object)
        {
            result = entry;
            return true;
        }
        if (Less(entry.Object, object))
            left = middle + 1;
        else
            right = middle - 1;
    }
    return false;
}

bool RuntimeAssetCatalog::ToBytes(Array<byte>& output, AssetPipelineDiagnostic& diagnostic) const
{
    output.Clear();
    if (ValidateCanonical(diagnostic))
        return true;

    CatalogWriter payload;
    payload.WriteString(_buildID);
    payload.WriteHash(_targetHash);
    payload.WriteUInt32(_entries.Count());
    for (const RuntimeAssetCatalogEntry& entry : _entries)
    {
        payload.WriteObject(entry.Object);
        payload.WriteString(entry.TypeName);
        payload.WriteString(entry.PackageName);
        payload.WriteUInt64(entry.Offset);
        payload.WriteUInt64(entry.Size);
        payload.WriteByte(static_cast<byte>(entry.Compression));
        payload.WriteHash(entry.Content);
        payload.WriteUInt32(entry.Dependencies.Count());
        for (const AssetObjectId& dependency : entry.Dependencies)
            payload.WriteObject(dependency);
    }
    CatalogWriter catalog;
    catalog.WriteUInt32(CatalogMagic);
    catalog.WriteUInt32(FormatVersion);
    catalog.WriteUInt32(payload.Data.Count());
    catalog.WriteHash(ContentHash::Compute(payload.Data.Get(), payload.Data.Count()));
    catalog.Data.Add(payload.Data.Get(), payload.Data.Count());
    output = MoveTemp(catalog.Data);
    diagnostic = AssetPipelineDiagnostic();
    return false;
}

bool RuntimeAssetCatalog::FromBytes(const Span<byte>& input, RuntimeAssetCatalog& result, AssetPipelineDiagnostic& diagnostic)
{
    result = RuntimeAssetCatalog();
    if (input.Length() < HeaderSize)
        return Fail(diagnostic, StringView::Empty, TEXT("Runtime catalog header is missing or truncated."));

    CatalogReader header(input.Get(), input.Length());
    uint32 magic;
    uint32 version;
    uint32 payloadSize;
    ContentHash payloadHash;
    if (header.ReadUInt32(magic) || header.ReadUInt32(version) || header.ReadUInt32(payloadSize) || header.ReadHash(payloadHash) ||
        magic != CatalogMagic || version != FormatVersion || payloadSize != static_cast<uint32>(input.Length() - HeaderSize))
        return Fail(diagnostic, StringView::Empty, TEXT("Runtime catalog header, version, or payload size is invalid."));
    const byte* payloadBytes = input.Get() + HeaderSize;
    if (ContentHash::Compute(payloadBytes, payloadSize) != payloadHash)
        return Fail(diagnostic, StringView::Empty, TEXT("Runtime catalog payload checksum is invalid."));

    CatalogReader payload(payloadBytes, payloadSize);
    uint32 entryCount;
    if (payload.ReadString(result._buildID) || payload.ReadHash(result._targetHash) || payload.ReadUInt32(entryCount) ||
        entryCount == 0 || entryCount > MaximumEntries)
        return Fail(diagnostic, StringView::Empty, TEXT("Runtime catalog payload identity or entry count is invalid."));
    result._entries.Resize(entryCount, false);
    for (RuntimeAssetCatalogEntry& entry : result._entries)
    {
        uint64 localOffset;
        uint64 localSize;
        byte compression;
        uint32 dependencyCount;
        if (payload.ReadObject(entry.Object) || payload.ReadString(entry.TypeName) || payload.ReadString(entry.PackageName) ||
            payload.ReadUInt64(localOffset) || payload.ReadUInt64(localSize) || payload.ReadByte(compression) ||
            payload.ReadHash(entry.Content) || payload.ReadUInt32(dependencyCount) || dependencyCount > MaximumDependenciesPerEntry)
            return Fail(diagnostic, StringView::Empty, TEXT("Runtime catalog contains a truncated or malformed object entry."));
        entry.Offset = localOffset;
        entry.Size = localSize;
        entry.Compression = static_cast<RuntimeAssetCompression>(compression);
        entry.Dependencies.Resize(dependencyCount, false);
        for (AssetObjectId& dependency : entry.Dependencies)
        {
            if (payload.ReadObject(dependency))
                return Fail(diagnostic, StringView::Empty, TEXT("Runtime catalog contains a truncated object dependency."));
        }
    }
    if (!payload.AtEnd())
        return Fail(diagnostic, StringView::Empty, TEXT("Runtime catalog contains unexpected trailing data."));
    return result.ValidateCanonical(diagnostic);
}

bool RuntimeAssetCatalog::SaveAtomic(const StringView& path, AssetPipelineDiagnostic& diagnostic) const
{
    Array<byte> bytes;
    if (ToBytes(bytes, diagnostic))
        return true;
    const String destination(path);
    const String staging = destination + TEXT(".stage-") + Guid::New().ToString(Guid::FormatType::N);
    SCOPE_EXIT { FileSystem::DeleteFile(staging); };
    if (File::WriteAllBytes(staging, bytes.Get(), bytes.Count()) || FileSystem::MoveFile(destination, staging, true))
        return Fail(diagnostic, path, TEXT("Runtime catalog could not be written atomically."));
    diagnostic = AssetPipelineDiagnostic();
    return false;
}

bool RuntimeAssetCatalog::Load(const StringView& path, RuntimeAssetCatalog& result, AssetPipelineDiagnostic& diagnostic)
{
    BytesContainer bytes;
    if (File::ReadAllBytes(path, bytes) || bytes.Length() > MAX_int32)
        return Fail(diagnostic, path, TEXT("Runtime catalog file is missing, unreadable, or too large."));
    if (FromBytes(Span<byte>(bytes.Get(), static_cast<int32>(bytes.Length())), result, diagnostic))
    {
        diagnostic.SourcePath = path;
        return true;
    }
    return false;
}
