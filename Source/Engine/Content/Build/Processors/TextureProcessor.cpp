// Copyright (c) Wojciech Figat. All rights reserved.

#include "TextureProcessor.h"
#include "Engine/Content/Build/ArtifactBuildContext.h"
#include "Engine/Content/Build/PrepareAssetContext.h"
#include "Engine/Platform/FileSystem.h"
#include "Engine/Platform/Platform.h"
#include "Engine/Core/ScopeExit.h"
#include "Engine/Platform/File.h"

#if COMPILE_WITH_ASSETS_IMPORTER
#include "Engine/ContentImporters/ImportTexture.h"
#include "Engine/Content/Storage/ContentStorageManager.h"
#include "Engine/Graphics/Textures/TextureData.h"
#include "Engine/Graphics/PixelFormatExtensions.h"
#endif

#if COMPILE_WITH_TEXTURE_TOOL

namespace
{
    struct TextureProbe
    {
        TextureSourceFormat Format = TextureSourceFormat::Unknown;
        int32 Width = 0;
        int32 Height = 0;
        int32 ArraySize = 1;
        int32 Channels = 0;
        int32 BitsPerChannel = 0;
    };

#if COMPILE_WITH_ASSETS_IMPORTER
    struct TextureArtifactAdapterArguments
    {
        TextureData* Data = nullptr;
        TextureTool::Options* Options = nullptr;
    };

    CreateAssetResult CreateTextureArtifact(CreateAssetContext& context)
    {
        const auto* arguments = static_cast<const TextureArtifactAdapterArguments*>(context.CustomArg);
        if (!arguments || !arguments->Data || !arguments->Options)
            return CreateAssetResult::Error;
        return ImportTexture::CreateArtifact(context, *arguments->Data, *arguments->Options);
    }
#endif

    bool PrepareFail(AssetPipelineDiagnostic& diagnostic, AssetPipelineDiagnosticCode code, const PrepareAssetContext& context, const StringView& message)
    {
        diagnostic = AssetPipelineDiagnostic();
        diagnostic.Code = code;
        diagnostic.Stage = AssetPipelineDiagnosticStage::Prepare;
        diagnostic.AssetGuid = context.GetRecord().ID;
        diagnostic.SourcePath = context.GetRecord().SourcePath.Get();
        diagnostic.ProcessorId = TextureProcessorSettings::ProcessorID();
        diagnostic.Message = message;
        return true;
    }

    uint16 ReadU16LE(const byte* data)
    {
        return static_cast<uint16>(data[0]) | static_cast<uint16>(data[1] << 8);
    }

    uint32 ReadU32LE(const byte* data)
    {
        return static_cast<uint32>(data[0]) | (static_cast<uint32>(data[1]) << 8) |
               (static_cast<uint32>(data[2]) << 16) | (static_cast<uint32>(data[3]) << 24);
    }

    uint32 ReadU32BE(const byte* data)
    {
        return (static_cast<uint32>(data[0]) << 24) | (static_cast<uint32>(data[1]) << 16) |
               (static_cast<uint32>(data[2]) << 8) | static_cast<uint32>(data[3]);
    }

    uint16 ReadU16BE(const byte* data)
    {
        return static_cast<uint16>((static_cast<uint16>(data[0]) << 8) | data[1]);
    }

    uint16 ReadU16(const byte* data, bool littleEndian)
    {
        return littleEndian ? ReadU16LE(data) : ReadU16BE(data);
    }

    uint32 ReadU32(const byte* data, bool littleEndian)
    {
        return littleEndian ? ReadU32LE(data) : ReadU32BE(data);
    }

    bool SetProbe(TextureProbe& result, TextureSourceFormat format, uint32 width, uint32 height, int32 channels, int32 bitsPerChannel)
    {
        if (width == 0 || height == 0 || width > MAX_int32 || height > MAX_int32 || channels < 1 || bitsPerChannel < 1)
            return true;
        result.Format = format;
        result.Width = static_cast<int32>(width);
        result.Height = static_cast<int32>(height);
        result.Channels = channels;
        result.BitsPerChannel = bitsPerChannel;
        return false;
    }

    bool EqualAscii(const Array<byte>& data, int32 offset, int32 length, const char* value)
    {
        int32 valueLength = 0;
        while (value[valueLength])
            valueLength++;
        return length == valueLength && offset >= 0 && offset + length <= data.Count() &&
               Platform::MemoryCompare(data.Get() + offset, value, length) == 0;
    }

    bool ReadCString(const Array<byte>& data, int32& offset, int32& start, int32& length)
    {
        start = offset;
        while (offset < data.Count() && data[offset] != 0 && offset - start <= 255)
            offset++;
        if (offset >= data.Count() || offset - start > 255)
            return true;
        length = offset - start;
        offset++;
        return false;
    }

    bool ProbePng(const Array<byte>& data, TextureProbe& result)
    {
        static const byte Signature[] = { 137, 80, 78, 71, 13, 10, 26, 10 };
        if (data.Count() < 33 || Platform::MemoryCompare(data.Get(), Signature, ARRAY_COUNT(Signature)) != 0 ||
            ReadU32BE(data.Get() + 8) != 13 || Platform::MemoryCompare(data.Get() + 12, "IHDR", 4) != 0)
            return true;
        const uint32 width = ReadU32BE(data.Get() + 16);
        const uint32 height = ReadU32BE(data.Get() + 20);
        const byte bitDepth = data[24];
        const byte colorType = data[25];
        const bool validDepth = (colorType == 0 && (bitDepth == 1 || bitDepth == 2 || bitDepth == 4 || bitDepth == 8 || bitDepth == 16)) ||
                                (colorType == 2 && (bitDepth == 8 || bitDepth == 16)) ||
                                (colorType == 3 && (bitDepth == 1 || bitDepth == 2 || bitDepth == 4 || bitDepth == 8)) ||
                                ((colorType == 4 || colorType == 6) && (bitDepth == 8 || bitDepth == 16));
        if (width == 0 || height == 0 || width > MAX_int32 || height > MAX_int32 || !validDepth ||
            data[26] != 0 || data[27] != 0 || data[28] > 1)
            return true;
        result.Format = TextureSourceFormat::Png;
        result.Width = static_cast<int32>(width);
        result.Height = static_cast<int32>(height);
        result.Channels = colorType == 0 || colorType == 3 ? 1 : colorType == 2 ? 3 : colorType == 4 ? 2 : 4;
        result.BitsPerChannel = bitDepth;
        return false;
    }

    bool ProbeTga(const Array<byte>& data, TextureProbe& result)
    {
        if (data.Count() < 18)
            return true;
        const byte colorMapType = data[1];
        const byte imageType = data[2];
        const uint16 colorMapLength = ReadU16LE(data.Get() + 5);
        const byte colorMapDepth = data[7];
        const uint16 width = ReadU16LE(data.Get() + 12);
        const uint16 height = ReadU16LE(data.Get() + 14);
        const byte pixelDepth = data[16];
        const bool colorMapped = imageType == 1 || imageType == 9;
        const bool trueColor = imageType == 2 || imageType == 10;
        const bool gray = imageType == 3 || imageType == 11;
        if ((!colorMapped && !trueColor && !gray) || width == 0 || height == 0 || colorMapType > 1 ||
            (colorMapped && (colorMapType != 1 || colorMapLength == 0 || (colorMapDepth != 15 && colorMapDepth != 16 && colorMapDepth != 24 && colorMapDepth != 32))) ||
            (!colorMapped && colorMapType != 0) ||
            (colorMapped && pixelDepth != 8) ||
            (trueColor && pixelDepth != 16 && pixelDepth != 24 && pixelDepth != 32) ||
            (gray && pixelDepth != 8 && pixelDepth != 16))
            return true;
        result.Format = TextureSourceFormat::Tga;
        result.Width = width;
        result.Height = height;
        result.Channels = colorMapped ? Math::Max<int32>(1, colorMapDepth / 8) : gray ? (pixelDepth == 16 ? 2 : 1) : Math::Max<int32>(1, pixelDepth / 8);
        result.BitsPerChannel = 8;
        return false;
    }

    bool ProbeBmp(const Array<byte>& data, TextureProbe& result)
    {
        if (data.Count() < 26 || data[0] != 'B' || data[1] != 'M')
            return true;
        const uint32 headerSize = ReadU32LE(data.Get() + 14);
        uint32 width;
        uint32 height;
        uint16 planes;
        uint16 bits;
        if (headerSize == 12)
        {
            width = ReadU16LE(data.Get() + 18);
            height = ReadU16LE(data.Get() + 20);
            planes = ReadU16LE(data.Get() + 22);
            bits = ReadU16LE(data.Get() + 24);
        }
        else
        {
            if (headerSize < 40 || data.Count() < 30)
                return true;
            const int32 signedWidth = static_cast<int32>(ReadU32LE(data.Get() + 18));
            const int32 signedHeight = static_cast<int32>(ReadU32LE(data.Get() + 22));
            if (signedWidth <= 0 || signedHeight == 0 || signedHeight == MIN_int32)
                return true;
            width = static_cast<uint32>(signedWidth);
            height = static_cast<uint32>(Math::Abs(signedHeight));
            planes = ReadU16LE(data.Get() + 26);
            bits = ReadU16LE(data.Get() + 28);
        }
        if (planes != 1 || (bits != 1 && bits != 4 && bits != 8 && bits != 16 && bits != 24 && bits != 32))
            return true;
        return SetProbe(result, TextureSourceFormat::Bmp, width, height, bits <= 8 ? 1 : bits <= 24 ? 3 : 4, bits <= 8 ? bits : 8);
    }

    bool ProbeGif(const Array<byte>& data, TextureProbe& result)
    {
        if (data.Count() < 10 ||
            (Platform::MemoryCompare(data.Get(), "GIF87a", 6) != 0 && Platform::MemoryCompare(data.Get(), "GIF89a", 6) != 0))
            return true;
        return SetProbe(result, TextureSourceFormat::Gif, ReadU16LE(data.Get() + 6), ReadU16LE(data.Get() + 8), 4, 8);
    }

    bool ProbeJpeg(const Array<byte>& data, TextureProbe& result)
    {
        if (data.Count() < 4 || data[0] != 0xff || data[1] != 0xd8)
            return true;
        int32 offset = 2;
        while (offset + 1 < data.Count())
        {
            while (offset < data.Count() && data[offset] != 0xff)
                offset++;
            while (offset < data.Count() && data[offset] == 0xff)
                offset++;
            if (offset >= data.Count())
                return true;
            const byte marker = data[offset++];
            if (marker == 0x00 || marker == 0x01 || marker == 0xd8 || (marker >= 0xd0 && marker <= 0xd9))
                continue;
            if (offset + 2 > data.Count())
                return true;
            const uint16 segmentSize = ReadU16BE(data.Get() + offset);
            if (segmentSize < 2 || offset + segmentSize > data.Count())
                return true;
            const bool startOfFrame = (marker >= 0xc0 && marker <= 0xc3) || (marker >= 0xc5 && marker <= 0xc7) ||
                                      (marker >= 0xc9 && marker <= 0xcb) || (marker >= 0xcd && marker <= 0xcf);
            if (startOfFrame)
            {
                if (segmentSize < 8)
                    return true;
                const byte bits = data[offset + 2];
                const uint16 height = ReadU16BE(data.Get() + offset + 3);
                const uint16 width = ReadU16BE(data.Get() + offset + 5);
                const byte channels = data[offset + 7];
                return SetProbe(result, TextureSourceFormat::Jpeg, width, height, channels, bits);
            }
            offset += segmentSize;
        }
        return true;
    }

    bool ReadTiffScalar(const Array<byte>& data, bool littleEndian, uint16 type, uint32 count, const byte* entry, uint32& value)
    {
        if (count < 1 || (type != 3 && type != 4))
            return true;
        const uint32 elementSize = type == 3 ? 2 : 4;
        const uint64 byteCount = static_cast<uint64>(count) * elementSize;
        const byte* values = entry + 8;
        if (byteCount > 4)
        {
            const uint32 valueOffset = ReadU32(entry + 8, littleEndian);
            if (valueOffset > static_cast<uint32>(data.Count()) || byteCount > static_cast<uint64>(data.Count()) - valueOffset)
                return true;
            values = data.Get() + valueOffset;
        }
        value = type == 3 ? ReadU16(values, littleEndian) : ReadU32(values, littleEndian);
        return false;
    }

    bool ProbeTiff(const Array<byte>& data, TextureProbe& result)
    {
        if (data.Count() < 8)
            return true;
        const bool littleEndian = data[0] == 'I' && data[1] == 'I';
        if (!littleEndian && !(data[0] == 'M' && data[1] == 'M'))
            return true;
        if (ReadU16(data.Get() + 2, littleEndian) != 42)
            return true;
        const uint32 directoryOffset = ReadU32(data.Get() + 4, littleEndian);
        if (directoryOffset > static_cast<uint32>(data.Count() - 2))
            return true;
        const uint16 entryCount = ReadU16(data.Get() + directoryOffset, littleEndian);
        if (entryCount > 4096 || static_cast<uint64>(directoryOffset) + 2ull + static_cast<uint64>(entryCount) * 12ull > static_cast<uint64>(data.Count()))
            return true;
        uint32 width = 0;
        uint32 height = 0;
        uint32 bits = 8;
        uint32 channels = 1;
        for (uint16 i = 0; i < entryCount; i++)
        {
            const byte* entry = data.Get() + directoryOffset + 2 + i * 12;
            const uint16 tag = ReadU16(entry, littleEndian);
            if (tag != 256 && tag != 257 && tag != 258 && tag != 277)
                continue;
            uint32 value;
            if (ReadTiffScalar(data, littleEndian, ReadU16(entry + 2, littleEndian), ReadU32(entry + 4, littleEndian), entry, value))
                return true;
            if (tag == 256)
                width = value;
            else if (tag == 257)
                height = value;
            else if (tag == 258)
                bits = value;
            else
                channels = value;
        }
        return SetProbe(result, TextureSourceFormat::Tiff, width, height, static_cast<int32>(channels), static_cast<int32>(bits));
    }

    bool ProbeDds(const Array<byte>& data, TextureProbe& result)
    {
        if (data.Count() < 128 || Platform::MemoryCompare(data.Get(), "DDS ", 4) != 0 ||
            ReadU32LE(data.Get() + 4) != 124 || ReadU32LE(data.Get() + 76) != 32)
            return true;
        if (SetProbe(result, TextureSourceFormat::Dds, ReadU32LE(data.Get() + 16), ReadU32LE(data.Get() + 12), 4, 8))
            return true;
        const uint32 caps2 = ReadU32LE(data.Get() + 112);
        result.ArraySize = (caps2 & 0x200) != 0 ? 6 : 1;
        if (ReadU32LE(data.Get() + 84) == 0x30315844 && data.Count() >= 148)
        {
            const uint32 arraySize = ReadU32LE(data.Get() + 140);
            const bool cube = (ReadU32LE(data.Get() + 136) & 0x4) != 0;
            if (arraySize == 0 || arraySize > static_cast<uint32>(MAX_int32 / (cube ? 6 : 1)))
                return true;
            result.ArraySize = static_cast<int32>(arraySize) * (cube ? 6 : 1);
        }
        return false;
    }

    bool ParsePositiveInteger(const Array<byte>& data, int32& offset, int32& value)
    {
        while (offset < data.Count() && (data[offset] == ' ' || data[offset] == '\t'))
            offset++;
        if (offset >= data.Count() || data[offset] < '0' || data[offset] > '9')
            return true;
        uint64 parsed = 0;
        while (offset < data.Count() && data[offset] >= '0' && data[offset] <= '9')
        {
            parsed = parsed * 10 + (data[offset++] - '0');
            if (parsed > MAX_int32)
                return true;
        }
        value = static_cast<int32>(parsed);
        return value < 1;
    }

    bool ProbeHdr(const Array<byte>& data, TextureProbe& result)
    {
        if (data.Count() < 16 ||
            (Platform::MemoryCompare(data.Get(), "#?RADIANCE", 10) != 0 && Platform::MemoryCompare(data.Get(), "#?RGBE", 6) != 0))
            return true;
        int32 offset = 0;
        while (offset < data.Count())
        {
            const int32 lineStart = offset;
            while (offset < data.Count() && data[offset] != '\n' && data[offset] != '\r')
                offset++;
            const int32 lineEnd = offset;
            while (offset < data.Count() && (data[offset] == '\n' || data[offset] == '\r'))
                offset++;
            if (lineEnd - lineStart < 6)
                continue;
            int32 cursor = lineStart;
            const byte firstSign = data[cursor++];
            const byte firstAxis = cursor < lineEnd ? data[cursor++] : 0;
            if ((firstSign != '+' && firstSign != '-') || (firstAxis != 'X' && firstAxis != 'Y'))
                continue;
            int32 firstSize;
            if (ParsePositiveInteger(data, cursor, firstSize))
                return true;
            while (cursor < lineEnd && (data[cursor] == ' ' || data[cursor] == '\t'))
                cursor++;
            if (cursor + 2 > lineEnd)
                return true;
            const byte secondSign = data[cursor++];
            const byte secondAxis = data[cursor++];
            if ((secondSign != '+' && secondSign != '-') || (secondAxis != 'X' && secondAxis != 'Y') || secondAxis == firstAxis)
                return true;
            int32 secondSize;
            if (ParsePositiveInteger(data, cursor, secondSize))
                return true;
            const uint32 width = firstAxis == 'X' ? firstSize : secondSize;
            const uint32 height = firstAxis == 'Y' ? firstSize : secondSize;
            return SetProbe(result, TextureSourceFormat::Hdr, width, height, 4, 8);
        }
        return true;
    }

    bool ProbeRaw(const Array<byte>& data, TextureProbe& result)
    {
        if (data.IsEmpty() || (data.Count() & 1) != 0)
            return true;
        const int32 size = static_cast<int32>(Math::Sqrt(data.Count() / 2.0f));
        if (size < 1 || static_cast<int64>(size) * size * 2 != data.Count())
            return true;
        return SetProbe(result, TextureSourceFormat::Raw, size, size, 1, 16);
    }

    bool ProbeExrChannels(const Array<byte>& data, int32 start, int32 size, int32& channels, int32& bitsPerChannel)
    {
        const int32 end = start + size;
        int32 offset = start;
        channels = 0;
        bitsPerChannel = 0;
        while (offset < end)
        {
            const int32 nameStart = offset;
            while (offset < end && data[offset] != 0 && offset - nameStart <= 255)
                offset++;
            if (offset >= end || offset - nameStart > 255)
                return true;
            const int32 nameLength = offset - nameStart;
            offset++;
            if (nameLength == 0)
                return offset != end || channels == 0;
            if (channels >= 64 || offset + 16 > end)
                return true;
            const uint32 pixelType = ReadU32LE(data.Get() + offset);
            const uint32 xSampling = ReadU32LE(data.Get() + offset + 8);
            const uint32 ySampling = ReadU32LE(data.Get() + offset + 12);
            if (pixelType > 2 || xSampling == 0 || ySampling == 0)
                return true;
            bitsPerChannel = Math::Max(bitsPerChannel, pixelType == 1 ? 16 : 32);
            channels++;
            offset += 16;
        }
        return true;
    }

    bool ProbeExr(const Array<byte>& data, TextureProbe& result)
    {
        if (data.Count() < 9 || ReadU32LE(data.Get()) != 0x762f3101)
            return true;
        const uint32 version = ReadU32LE(data.Get() + 4);
        if ((version & 0xff) != 2 || (version & 0x1800) != 0)
            return true;

        bool hasChannels = false;
        bool hasCompression = false;
        bool hasDataWindow = false;
        int32 channels = 0;
        int32 bitsPerChannel = 0;
        int32 width = 0;
        int32 height = 0;
        int32 offset = 8;
        while (offset < data.Count())
        {
            int32 nameStart, nameLength;
            if (ReadCString(data, offset, nameStart, nameLength))
                return true;
            if (nameLength == 0)
                break;
            int32 typeStart, typeLength;
            if (ReadCString(data, offset, typeStart, typeLength) || offset + 4 > data.Count())
                return true;
            const uint32 attributeSize = ReadU32LE(data.Get() + offset);
            offset += 4;
            if (attributeSize > static_cast<uint32>(data.Count() - offset))
                return true;
            if (EqualAscii(data, nameStart, nameLength, "channels"))
            {
                if (hasChannels || !EqualAscii(data, typeStart, typeLength, "chlist") ||
                    ProbeExrChannels(data, offset, static_cast<int32>(attributeSize), channels, bitsPerChannel))
                    return true;
                hasChannels = true;
            }
            else if (EqualAscii(data, nameStart, nameLength, "compression"))
            {
                if (hasCompression || !EqualAscii(data, typeStart, typeLength, "compression") || attributeSize != 1 || data[offset] > 9)
                    return true;
                hasCompression = true;
            }
            else if (EqualAscii(data, nameStart, nameLength, "dataWindow"))
            {
                if (hasDataWindow || !EqualAscii(data, typeStart, typeLength, "box2i") || attributeSize != 16)
                    return true;
                const int64 minX = static_cast<int32>(ReadU32LE(data.Get() + offset));
                const int64 minY = static_cast<int32>(ReadU32LE(data.Get() + offset + 4));
                const int64 maxX = static_cast<int32>(ReadU32LE(data.Get() + offset + 8));
                const int64 maxY = static_cast<int32>(ReadU32LE(data.Get() + offset + 12));
                const int64 probedWidth = maxX - minX + 1;
                const int64 probedHeight = maxY - minY + 1;
                if (probedWidth <= 0 || probedHeight <= 0 || probedWidth > MAX_int32 || probedHeight > MAX_int32)
                    return true;
                width = static_cast<int32>(probedWidth);
                height = static_cast<int32>(probedHeight);
                hasDataWindow = true;
            }
            offset += static_cast<int32>(attributeSize);
        }
        if (!hasChannels || !hasCompression || !hasDataWindow)
            return true;
        result.Format = TextureSourceFormat::Exr;
        result.Width = width;
        result.Height = height;
        result.Channels = channels;
        result.BitsPerChannel = bitsPerChannel;
        return false;
    }
}

uint64 TexturePreparedPayload::GetMemoryUsage() const
{
    uint64 result = sizeof(TexturePreparedPayload) + static_cast<uint64>(Settings.Import.Sprites.Count()) * sizeof(Sprite);
    for (const Sprite& sprite : Settings.Import.Sprites)
        result += static_cast<uint64>(sprite.Name.Length()) * sizeof(Char);
    for (const auto& entry : Settings.PlatformOverrides)
        result += entry.Key.Length();
    for (const auto& entry : Settings.UnknownFields)
        result += entry.Key.Length() + entry.Value.Length();
    return result;
}

AssetProcessorDescriptor TextureProcessor::CreateDescriptor()
{
    AssetProcessorDescriptor descriptor;
    descriptor.ID = TextureProcessorSettings::ProcessorID();
    descriptor.ProviderID = TEXT("flax");
    descriptor.SourceExtensions.Add(TEXT(".png"));
    descriptor.SourceExtensions.Add(TEXT(".tga"));
    descriptor.SourceExtensions.Add(TEXT(".exr"));
    descriptor.SourceExtensions.Add(TEXT(".bmp"));
    descriptor.SourceExtensions.Add(TEXT(".gif"));
    descriptor.SourceExtensions.Add(TEXT(".tiff"));
    descriptor.SourceExtensions.Add(TEXT(".tif"));
    descriptor.SourceExtensions.Add(TEXT(".jpeg"));
    descriptor.SourceExtensions.Add(TEXT(".jpg"));
    descriptor.SourceExtensions.Add(TEXT(".dds"));
    descriptor.SourceExtensions.Add(TEXT(".hdr"));
    descriptor.SourceExtensions.Add(TEXT(".raw"));
    descriptor.SourceKinds.Add(AssetSourceKind::ImportedSource);
    descriptor.DocumentTypes.Add(TEXT("FlaxEngine.Texture"));
    descriptor.DocumentTypes.Add(TEXT("FlaxEngine.CubeTexture"));
    descriptor.DocumentTypes.Add(TEXT("FlaxEngine.SpriteAtlas"));
    descriptor.MainOutputType = TEXT("FlaxEngine.Texture");
    descriptor.SettingsSchemaVersion = TextureProcessorSettings::CurrentVersion;
    descriptor.ImplementationVersion = ImplementationVersion;
    descriptor.InterfaceVersion = 1;
    descriptor.MaxParallelismClass = "texture";
    descriptor.MemoryEstimate = 64ull * 1024ull * 1024ull;
    descriptor.UpgradeSettings = &TextureProcessorSettings::Upgrade;
    descriptor.Prepare = &TextureProcessor::Prepare;
    descriptor.Build = &TextureProcessor::Build;

    AssetPipelineDiagnostic diagnostic;
    const bool defaultsFailed = TextureProcessorSettings::Defaults().ToJson(descriptor.NormalizedDefaultSettings, diagnostic);
    ASSERT(!defaultsFailed);

    AssetProcessorOutputDescriptor runtime;
    runtime.Kind = "runtime";
    runtime.Extension = ".flax";
    runtime.FormatVersion = RuntimeFormatVersion;
    runtime.TargetDimensions = ArtifactTargetDimension::Platform | ArtifactTargetDimension::Architecture |
                               ArtifactTargetDimension::Graphics | ArtifactTargetDimension::TextureCompression;
    runtime.CompatibilityTag = "flax-texture-v4";
    runtime.IndependentlyReusable = true;
    descriptor.Outputs.Add(runtime);

    AssetProcessorOutputDescriptor thumbnail;
    thumbnail.Kind = "thumbnail";
    thumbnail.Extension = ".png";
    thumbnail.FormatVersion = ThumbnailFormatVersion;
    thumbnail.TargetDimensions = ArtifactTargetDimension::None;
    thumbnail.CompatibilityTag = "flax-texture-thumbnail-v2";
    thumbnail.IndependentlyReusable = true;
    descriptor.Outputs.Add(thumbnail);
    return descriptor;
}

bool TextureProcessor::BuildOutputKey(const PreparedAsset& prepared, const ArtifactTarget& target, const StringAnsiView& outputKind,
    ArtifactKey& key, Array<ArtifactKeyComponent>& components, AssetPipelineDiagnostic& diagnostic)
{
    key = ArtifactKey();
    components.Clear();
    const auto* payload = static_cast<const TexturePreparedPayload*>(prepared.Payload.get());
    const AssetProcessorDescriptor descriptor = CreateDescriptor();
    const AssetProcessorOutputDescriptor* output = nullptr;
    for (const AssetProcessorOutputDescriptor& candidate : descriptor.Outputs)
    {
        if (candidate.Kind == outputKind)
        {
            output = &candidate;
            break;
        }
    }
    if (!payload || !output)
    {
        diagnostic = AssetPipelineDiagnostic();
        diagnostic.Code = AssetPipelineDiagnosticCode::BuildFailed;
        diagnostic.Stage = AssetPipelineDiagnosticStage::Prepare;
        diagnostic.AssetGuid = prepared.AssetID;
        diagnostic.OutputKind = String(outputKind);
        diagnostic.Message = TEXT("Texture output key requires prepared texture state and a declared output kind.");
        return true;
    }

    const bool runtime = outputKind == "runtime";
    ArtifactKeyBuilder builder(StringAnsiView("flax-texture-output-v1"));
    descriptor.AppendVersionKey(builder, *output);
    builder.AddString(StringAnsiView("output-compatibility"), output->CompatibilityTag);
    if (runtime)
        builder.AddGuid(StringAnsiView("effective-asset"), prepared.AssetID);
    int32 dependencyIndex = 0;
    for (const AssetDependency& dependency : prepared.Dependencies)
    {
        const bool include = dependency.IsSourceDependency() ||
            (dependency.Kind == AssetDependencyKind::Toolchain &&
                (dependency.StableIdentity == TEXT("texture-decoder") || (runtime && dependency.StableIdentity == TEXT("texture-compressor"))));
        if (include)
            dependency.AppendKeyComponents(builder, dependencyIndex++);
    }
    if (runtime)
    {
        TextureProcessorSettings effective = payload->Settings;
        effective.Import = effective.ToImportOptions(target.Platform);
        effective.PlatformOverrides.Clear();
        StringAnsi effectiveJson;
        if (effective.ToJson(effectiveJson, diagnostic))
            return true;
        builder.AddHash(StringAnsiView("effective-settings"), ContentHash::Compute(effectiveJson.Get(), effectiveJson.Length()));
        builder.AddString(StringAnsiView("graphics-runtime-abi"), StringAnsiView("flax-texture-runtime-abi-v4"));
    }
    else
    {
        builder.AddString(StringAnsiView("thumbnail-encoder"), StringAnsiView("flax-png-thumbnail-v2"));
    }
    builder.AddTarget(target, output->TargetDimensions);
    key = builder.Finalize();
    components = builder.GetComponents();
    diagnostic = AssetPipelineDiagnostic();
    return false;
}

bool TextureProcessor::Prepare(PrepareAssetContext& context, PreparedAsset& prepared, AssetPipelineDiagnostic& diagnostic)
{
    prepared = PreparedAsset();
    TextureProcessorSettings settings;
    if (TextureProcessorSettings::Parse(context.GetSettings(), TextureProcessorSettings::CurrentVersion, settings, diagnostic))
    {
        diagnostic.Stage = AssetPipelineDiagnosticStage::Prepare;
        diagnostic.AssetGuid = context.GetRecord().ID;
        diagnostic.SourcePath = context.GetRecord().SourcePath.Get();
        return true;
    }

    Array<byte> source;
    ContentHash sourceHash;
    AssetDependencyOrigin sourceOrigin;
    sourceOrigin.Path = context.GetRecord().SourcePath.Get();
    if (context.ReadSourceFile(context.GetRecord().SourcePath.Get(), source, sourceHash, sourceOrigin, diagnostic))
        return true;
    if (context.GetCancellation().IsCancellationRequested())
        return PrepareFail(diagnostic, AssetPipelineDiagnosticCode::BuildCancelled, context, TEXT("Texture preparation was cancelled after reading the source."));

    TextureProbe probe;
    const String extension = FileSystem::GetExtension(context.GetRecord().SourcePath.Get()).ToLower();
    bool probeFailed = true;
    if (extension == TEXT("png"))
        probeFailed = ProbePng(source, probe);
    else if (extension == TEXT("tga"))
        probeFailed = ProbeTga(source, probe);
    else if (extension == TEXT("exr"))
        probeFailed = ProbeExr(source, probe);
    else if (extension == TEXT("bmp"))
        probeFailed = ProbeBmp(source, probe);
    else if (extension == TEXT("gif"))
        probeFailed = ProbeGif(source, probe);
    else if (extension == TEXT("tiff") || extension == TEXT("tif"))
        probeFailed = ProbeTiff(source, probe);
    else if (extension == TEXT("jpeg") || extension == TEXT("jpg"))
        probeFailed = ProbeJpeg(source, probe);
    else if (extension == TEXT("dds"))
        probeFailed = ProbeDds(source, probe);
    else if (extension == TEXT("hdr"))
        probeFailed = ProbeHdr(source, probe);
    else if (extension == TEXT("raw"))
        probeFailed = ProbeRaw(source, probe);
    if (probeFailed)
        return PrepareFail(diagnostic, AssetPipelineDiagnosticCode::InvalidMeta, context, TEXT("Texture source header is malformed or does not match its supported file extension."));

    if (probe.Width > MaximumDimension || probe.Height > MaximumDimension)
        return PrepareFail(diagnostic, AssetPipelineDiagnosticCode::ResourceLimitExceeded, context, TEXT("Texture source dimensions exceed the bounded decoder limit."));
    const uint64 pixels = static_cast<uint64>(probe.Width) * static_cast<uint64>(probe.Height) * static_cast<uint64>(probe.ArraySize);
    const uint64 sourceBytesPerPixel = probe.Format == TextureSourceFormat::Exr || probe.Format == TextureSourceFormat::Hdr
        ? 16ull
        : Math::Max<uint64>(4, (static_cast<uint64>(probe.Channels) * probe.BitsPerChannel + 7) / 8);
    if (pixels > MaximumDecodedBytes / sourceBytesPerPixel)
        return PrepareFail(diagnostic, AssetPipelineDiagnosticCode::ResourceLimitExceeded, context, TEXT("Texture decompression estimate exceeds the configured memory limit."));
    uint64 decodedBytes = pixels * sourceBytesPerPixel;
    if (settings.Import.GenerateMipMaps)
        decodedBytes += decodedBytes / 3;
    if (decodedBytes > MaximumDecodedBytes)
        return PrepareFail(diagnostic, AssetPipelineDiagnosticCode::ResourceLimitExceeded, context, TEXT("Texture mip-chain estimate exceeds the configured memory limit."));

    const StringAnsi decoderIdentity = StringAnsi("flax-texture-decoder-") + StringAnsi(extension) + "-v1";
    const ContentHash decoderHash = ContentHash::Compute(decoderIdentity.Get(), decoderIdentity.Length());
    static const char CompressorIdentity[] = "flax-texture-compressor-v1";
    const ContentHash compressorHash = ContentHash::Compute(CompressorIdentity, ARRAY_COUNT(CompressorIdentity) - 1);
    if (context.DeclareToolchain(TEXT("texture-decoder"), decoderHash, sourceOrigin, diagnostic) ||
        context.DeclareToolchain(TEXT("texture-compressor"), compressorHash, sourceOrigin, diagnostic) ||
        context.DeclareOutput(StringAnsiView("runtime"), Guid::Empty, diagnostic) ||
        context.DeclareOutput(StringAnsiView("thumbnail"), Guid::Empty, diagnostic))
        return true;

    auto payload = std::make_shared<TexturePreparedPayload>();
    payload->Settings = MoveTemp(settings);
    payload->SourceFormat = probe.Format;
    payload->Width = probe.Width;
    payload->Height = probe.Height;
    payload->SourceArraySize = probe.ArraySize;
    payload->SourceChannels = probe.Channels;
    payload->SourceBitsPerChannel = probe.BitsPerChannel;
    payload->EstimatedDecodedBytes = decodedBytes;
    payload->EstimatedOutputBytes = pixels * 4 + (payload->Settings.Import.GenerateMipMaps ? pixels * 4 / 3 : 0);
    prepared.Payload = payload;
    prepared.MemoryEstimate = decodedBytes + payload->EstimatedOutputBytes;
    diagnostic = AssetPipelineDiagnostic();
    return false;
}

bool TextureProcessor::Build(ArtifactBuildContext& context, AssetPipelineDiagnostic& diagnostic)
{
#if COMPILE_WITH_ASSETS_IMPORTER
    diagnostic = AssetPipelineDiagnostic();
    const PreparedAsset& prepared = context.GetPreparedAsset();
    const auto* payload = static_cast<const TexturePreparedPayload*>(prepared.Payload.get());
    auto fail = [&](AssetPipelineDiagnosticCode code, const StringView& path, const StringView& message)
    {
        diagnostic = AssetPipelineDiagnostic();
        diagnostic.Code = code;
        diagnostic.Stage = AssetPipelineDiagnosticStage::Build;
        diagnostic.AssetGuid = prepared.AssetID;
        diagnostic.SourcePath = path;
        diagnostic.ProcessorId = TextureProcessorSettings::ProcessorID();
        diagnostic.Message = message;
        return true;
    };
    if (!payload)
        return fail(AssetPipelineDiagnosticCode::BuildFailed, StringView::Empty, TEXT("Texture prepared payload is missing."));
    if (context.GetCancellation().IsCancellationRequested())
        return fail(AssetPipelineDiagnosticCode::BuildCancelled, StringView::Empty, TEXT("Texture build was cancelled before decode."));

    bool buildRuntime = false;
    bool buildThumbnail = false;
    for (const DeclaredArtifactOutput& output : prepared.Outputs)
    {
        buildRuntime |= output.Kind == StringAnsiView("runtime");
        buildThumbnail |= output.Kind == StringAnsiView("thumbnail");
    }
    if (!buildRuntime && !buildThumbnail)
        return fail(AssetPipelineDiagnosticCode::ArtifactMissing, StringView::Empty, TEXT("Texture build declares no supported output."));

    const AssetDependency* sourceDependency = nullptr;
    for (const AssetDependency& dependency : prepared.Dependencies)
    {
        if (dependency.Kind == AssetDependencyKind::SourceAsset)
        {
            sourceDependency = &dependency;
            break;
        }
    }
    if (!sourceDependency)
        return fail(AssetPipelineDiagnosticCode::UndeclaredInput, StringView::Empty, TEXT("Texture source dependency is missing from prepared state."));

    Array<byte> sourceBytes;
    ContentHash sourceHash;
    if (context.ReadInput(sourceDependency->StableIdentity, sourceBytes, sourceHash, diagnostic))
        return true;
    String sourcePath;
    if (context.TryGetInputPath(sourceDependency->StableIdentity, sourcePath, diagnostic))
        return true;
    const String sourceExtension = TEXT(".") + FileSystem::GetExtension(sourcePath).ToLower();
    String verifiedSourcePath;
    if (context.CreateScratchFilePath(sourceExtension, verifiedSourcePath, diagnostic) ||
        File::WriteAllBytes(verifiedSourcePath, sourceBytes.Get(), sourceBytes.Count()))
        return fail(AssetPipelineDiagnosticCode::BuildFailed, verifiedSourcePath, TEXT("Texture source snapshot could not be created in job staging."));
    SCOPE_EXIT
    {
        ContentStorageManager::EnsureAccess(verifiedSourcePath);
        FileSystem::DeleteFile(verifiedSourcePath);
    };

    if (buildRuntime)
    {
        TextureTool::Options options = payload->Settings.ToImportOptions(context.GetTarget().Platform);
        ImportTexture::NormalizeOptions(options);
        if (payload->Width > 0 && payload->Height > 0 &&
            (!Math::IsPowerOfTwo(payload->Width) || !Math::IsPowerOfTwo(payload->Height)))
            options.GenerateMipMaps = false;
        TextureData textureData;
        String errorMessage;
        if (TextureTool::ImportTexture(verifiedSourcePath, textureData, options, errorMessage, false))
        {
            const String message = errorMessage.IsEmpty()
                ? TEXT("Texture decoder rejected the verified source snapshot.")
                : TEXT("Texture decoder rejected the verified source snapshot: ") + errorMessage;
            return fail(AssetPipelineDiagnosticCode::BuildFailed, sourcePath, message);
        }
        if (context.GetCancellation().IsCancellationRequested())
            return fail(AssetPipelineDiagnosticCode::BuildCancelled, sourcePath, TEXT("Texture build was cancelled after decode."));

        String runtimeScratchPath;
        if (context.CreateScratchFilePath(TEXT(".flax"), runtimeScratchPath, diagnostic))
            return true;
        SCOPE_EXIT
        {
            ContentStorageManager::EnsureAccess(runtimeScratchPath);
            FileSystem::DeleteFile(runtimeScratchPath);
        };
        TextureArtifactAdapterArguments adapterArguments;
        adapterArguments.Data = &textureData;
        adapterArguments.Options = &options;
        CreateAssetContext importerContext(verifiedSourcePath, runtimeScratchPath, prepared.AssetID, &adapterArguments, true, prepared.OutputType);
        CreateAssetFunction importerCallback = &CreateTextureArtifact;
        const CreateAssetResult importResult = importerContext.Run(importerCallback);
        if (importResult != CreateAssetResult::Ok)
        {
            diagnostic.Related.Add(::ToString(importResult));
            return fail(importResult == CreateAssetResult::Abort ? AssetPipelineDiagnosticCode::BuildCancelled : AssetPipelineDiagnosticCode::BuildFailed,
                sourcePath, TEXT("Texture compatibility importer failed while writing job staging."));
        }
        Array<byte> runtimeBytes;
        if (File::ReadAllBytes(runtimeScratchPath, runtimeBytes))
            return fail(AssetPipelineDiagnosticCode::ArtifactInvalid, runtimeScratchPath, TEXT("Texture compatibility output is unreadable."));
        ArtifactWriter runtimeWriter;
        if (context.OpenOutput(StringAnsiView("runtime"), runtimeWriter, diagnostic) ||
            runtimeWriter.WriteFile(TEXT("texture.flax"), runtimeBytes.Get(), runtimeBytes.Count(), diagnostic))
            return true;
    }

    if (!buildThumbnail)
    {
        diagnostic = AssetPipelineDiagnostic();
        return false;
    }

    if (context.GetCancellation().IsCancellationRequested())
        return fail(AssetPipelineDiagnosticCode::BuildCancelled, sourcePath, TEXT("Texture build was cancelled before thumbnail generation."));
    TextureData thumbnailSource;
    if (TextureTool::ImportTexture(verifiedSourcePath, thumbnailSource, false))
        return fail(AssetPipelineDiagnosticCode::BuildFailed, sourcePath, TEXT("Texture thumbnail decoder rejected the verified source snapshot."));
    TextureData converted;
    TextureData* thumbnail = &thumbnailSource;
    const bool thumbnailIsSrgb = PixelFormatExtensions::IsSRGB(thumbnailSource.Format);
    const PixelFormat thumbnailWorkingFormat = thumbnailIsSrgb ? PixelFormat::R8G8B8A8_UNorm_sRGB : PixelFormat::R8G8B8A8_UNorm;
    if (thumbnailSource.Format != thumbnailWorkingFormat)
    {
        if (TextureTool::Convert(converted, thumbnailSource, thumbnailWorkingFormat))
            return fail(AssetPipelineDiagnosticCode::BuildFailed, sourcePath, TEXT("Texture thumbnail conversion failed."));
        thumbnail = &converted;
    }
    TextureData resized;
    if (thumbnail->Width > 256 || thumbnail->Height > 256)
    {
        const int32 width = thumbnail->Width >= thumbnail->Height ? 256 : Math::Max(1, thumbnail->Width * 256 / thumbnail->Height);
        const int32 height = thumbnail->Height >= thumbnail->Width ? 256 : Math::Max(1, thumbnail->Height * 256 / thumbnail->Width);
        if (TextureTool::Resize(resized, *thumbnail, width, height))
            return fail(AssetPipelineDiagnosticCode::BuildFailed, sourcePath, TEXT("Texture thumbnail resize failed."));
        thumbnail = &resized;
    }
    // PNG stores display-encoded samples. Keep sRGB filtering during conversion/resize, then
    // re-tag the pixels without converting them to linear values before the PNG encoder.
    if (thumbnailIsSrgb)
        thumbnail->Format = PixelFormatExtensions::ToNonsRGB(thumbnail->Format);
    String thumbnailScratchPath;
    if (context.CreateScratchFilePath(TEXT(".png"), thumbnailScratchPath, diagnostic))
        return true;
    SCOPE_EXIT { FileSystem::DeleteFile(thumbnailScratchPath); };
    if (TextureTool::ExportTexture(thumbnailScratchPath, *thumbnail, false))
        return fail(AssetPipelineDiagnosticCode::BuildFailed, thumbnailScratchPath, TEXT("Texture thumbnail encoding failed."));
    Array<byte> thumbnailBytes;
    if (File::ReadAllBytes(thumbnailScratchPath, thumbnailBytes))
        return fail(AssetPipelineDiagnosticCode::ArtifactInvalid, thumbnailScratchPath, TEXT("Texture thumbnail output is unreadable."));
    ArtifactWriter thumbnailWriter;
    if (context.OpenOutput(StringAnsiView("thumbnail"), thumbnailWriter, diagnostic) ||
        thumbnailWriter.WriteFile(TEXT("thumbnail.png"), thumbnailBytes.Get(), thumbnailBytes.Count(), diagnostic))
        return true;
    diagnostic = AssetPipelineDiagnostic();
    return false;
#else
    diagnostic = AssetPipelineDiagnostic();
    diagnostic.Code = AssetPipelineDiagnosticCode::ProcessorMissing;
    diagnostic.Stage = AssetPipelineDiagnosticStage::Build;
    diagnostic.ProcessorId = TextureProcessorSettings::ProcessorID();
    diagnostic.Message = TEXT("Texture compatibility build requires the editor asset importer module.");
    return true;
#endif
}

#endif
