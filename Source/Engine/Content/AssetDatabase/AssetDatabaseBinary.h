// Copyright (c) Wojciech Figat. All rights reserved.

#pragma once

#include "Engine/Core/Collections/Array.h"
#include "Engine/Core/Types/String.h"
#include "Engine/Serialization/MemoryWriteStream.h"

namespace SourceAssetDatabaseBinary
{
    constexpr uint32 MaximumRows = 2000000;
    constexpr uint32 MaximumStringBytes = 16 * 1024 * 1024;

    class Writer
    {
    public:
        MemoryWriteStream Stream;

        template<typename T>
        void Write(const T& value)
        {
            Stream.WriteBytes(&value, sizeof(T));
        }

        void WriteString(const StringView& value)
        {
            const StringAnsi utf8(value);
            const uint32 length = utf8.Length();
            Write(length);
            Stream.WriteBytes(utf8.Get(), length);
        }

        void Finish(Array<byte>& output)
        {
            output.Resize(Stream.GetPosition(), false);
            if (output.HasItems())
                Platform::MemoryCopy(output.Get(), Stream.GetHandle(), output.Count());
        }
    };

    class Reader
    {
    private:
        const byte* _data;
        uint32 _length;
        uint32 _position = 0;

    public:
        Reader(const byte* data, uint32 length)
            : _data(data)
            , _length(length)
        {
        }

        template<typename T>
        bool Read(T& value)
        {
            return ReadBytes(&value, sizeof(T));
        }

        bool ReadBytes(void* output, uint32 length)
        {
            if (length > _length - _position)
                return true;
            Platform::MemoryCopy(output, _data + _position, length);
            _position += length;
            return false;
        }

        bool ReadString(String& output)
        {
            uint32 length;
            if (Read(length) || length > MaximumStringBytes || length > _length - _position)
                return true;
            output = String(StringAnsiView((const char*)_data + _position, length));
            _position += length;
            return false;
        }

        bool ReadCount(uint32& count)
        {
            return Read(count) || count > MaximumRows;
        }

        bool AtEnd() const
        {
            return _position == _length;
        }
    };
}
