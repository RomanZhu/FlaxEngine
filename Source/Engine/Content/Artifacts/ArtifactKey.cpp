// Copyright (c) Wojciech Figat. All rights reserved.

#include "ArtifactKey.h"
#include "Engine/Platform/Platform.h"

namespace
{
    constexpr uint32 RoundConstants[64] =
    {
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
        0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
        0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
        0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
        0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
        0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
        0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
        0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
    };

    FORCE_INLINE uint32 RotateRight(uint32 value, uint32 bits)
    {
        return (value >> bits) | (value << (32 - bits));
    }

    int32 HexValue(char value)
    {
        if (value >= '0' && value <= '9')
            return value - '0';
        if (value >= 'a' && value <= 'f')
            return value - 'a' + 10;
        if (value >= 'A' && value <= 'F')
            return value - 'A' + 10;
        return -1;
    }

    int32 HexValue(Char value)
    {
        if (value >= TEXT('0') && value <= TEXT('9'))
            return value - TEXT('0');
        if (value >= TEXT('a') && value <= TEXT('f'))
            return value - TEXT('a') + 10;
        if (value >= TEXT('A') && value <= TEXT('F'))
            return value - TEXT('A') + 10;
        return -1;
    }

    template<typename View>
    bool ParseHash(const View& text, ContentHash& value)
    {
        if (text.Length() != 64)
            return true;
        ContentHash parsed;
        for (int32 i = 0; i < 32; i++)
        {
            const int32 high = HexValue(text[i * 2]);
            const int32 low = HexValue(text[i * 2 + 1]);
            if (high < 0 || low < 0)
                return true;
            parsed.Bytes[i] = static_cast<byte>((high << 4) | low);
        }
        value = parsed;
        return false;
    }
}

ContentHash::ContentHash()
{
    Platform::MemoryClear(Bytes, sizeof(Bytes));
}

bool ContentHash::operator==(const ContentHash& other) const
{
    uint32 different = 0;
    for (int32 i = 0; i < 8; i++)
        different |= Values[i] ^ other.Values[i];
    return different == 0;
}

bool ContentHash::operator!=(const ContentHash& other) const
{
    return !(*this == other);
}

bool ContentHash::IsZero() const
{
    uint32 value = 0;
    for (int32 i = 0; i < 8; i++)
        value |= Values[i];
    return value == 0;
}

StringAnsi ContentHash::ToString() const
{
    static const char Digits[] = "0123456789abcdef";
    StringAnsi result;
    result.Resize(64);
    for (int32 i = 0; i < 32; i++)
    {
        result[i * 2] = Digits[Bytes[i] >> 4];
        result[i * 2 + 1] = Digits[Bytes[i] & 15];
    }
    return result;
}

bool ContentHash::Parse(const StringAnsiView& text, ContentHash& value)
{
    return ParseHash(text, value);
}

bool ContentHash::Parse(const StringView& text, ContentHash& value)
{
    return ParseHash(text, value);
}

ContentHash ContentHash::Compute(const void* data, uint64 length)
{
    ContentHasher hasher;
    hasher.Update(data, length);
    return hasher.Finalize();
}

ContentHasher::ContentHasher()
{
    Reset();
}

void ContentHasher::Reset()
{
    _state[0] = 0x6a09e667;
    _state[1] = 0xbb67ae85;
    _state[2] = 0x3c6ef372;
    _state[3] = 0xa54ff53a;
    _state[4] = 0x510e527f;
    _state[5] = 0x9b05688c;
    _state[6] = 0x1f83d9ab;
    _state[7] = 0x5be0cd19;
    _totalBytes = 0;
    _blockBytes = 0;
}

void ContentHasher::Update(const void* data, uint64 length)
{
    if (length == 0)
        return;
    ASSERT(data);
    const byte* source = static_cast<const byte*>(data);
    _totalBytes += length;
    while (length > 0)
    {
        const uint32 count = static_cast<uint32>(Math::Min<uint64>(length, 64 - _blockBytes));
        Platform::MemoryCopy(_block + _blockBytes, source, count);
        _blockBytes += count;
        source += count;
        length -= count;
        if (_blockBytes == 64)
        {
            Transform(_block);
            _blockBytes = 0;
        }
    }
}

ContentHash ContentHasher::Finalize() const
{
    ContentHasher copy = *this;
    const uint64 bitLength = copy._totalBytes * 8;
    const byte marker = 0x80;
    copy.Update(&marker, 1);
    const byte zero = 0;
    while (copy._blockBytes != 56)
        copy.Update(&zero, 1);
    byte lengthBytes[8];
    for (int32 i = 0; i < 8; i++)
        lengthBytes[7 - i] = static_cast<byte>(bitLength >> (i * 8));
    copy.Update(lengthBytes, 8);

    ContentHash result;
    for (int32 i = 0; i < 8; i++)
    {
        result.Bytes[i * 4] = static_cast<byte>(copy._state[i] >> 24);
        result.Bytes[i * 4 + 1] = static_cast<byte>(copy._state[i] >> 16);
        result.Bytes[i * 4 + 2] = static_cast<byte>(copy._state[i] >> 8);
        result.Bytes[i * 4 + 3] = static_cast<byte>(copy._state[i]);
    }
    return result;
}

void ContentHasher::Transform(const byte* block)
{
    uint32 words[64];
    for (int32 i = 0; i < 16; i++)
    {
        words[i] = (static_cast<uint32>(block[i * 4]) << 24) |
                   (static_cast<uint32>(block[i * 4 + 1]) << 16) |
                   (static_cast<uint32>(block[i * 4 + 2]) << 8) |
                   static_cast<uint32>(block[i * 4 + 3]);
    }
    for (int32 i = 16; i < 64; i++)
    {
        const uint32 s0 = RotateRight(words[i - 15], 7) ^ RotateRight(words[i - 15], 18) ^ (words[i - 15] >> 3);
        const uint32 s1 = RotateRight(words[i - 2], 17) ^ RotateRight(words[i - 2], 19) ^ (words[i - 2] >> 10);
        words[i] = words[i - 16] + s0 + words[i - 7] + s1;
    }

    uint32 a = _state[0];
    uint32 b = _state[1];
    uint32 c = _state[2];
    uint32 d = _state[3];
    uint32 e = _state[4];
    uint32 f = _state[5];
    uint32 g = _state[6];
    uint32 h = _state[7];
    for (int32 i = 0; i < 64; i++)
    {
        const uint32 sum1 = RotateRight(e, 6) ^ RotateRight(e, 11) ^ RotateRight(e, 25);
        const uint32 choose = (e & f) ^ (~e & g);
        const uint32 temp1 = h + sum1 + choose + RoundConstants[i] + words[i];
        const uint32 sum0 = RotateRight(a, 2) ^ RotateRight(a, 13) ^ RotateRight(a, 22);
        const uint32 majority = (a & b) ^ (a & c) ^ (b & c);
        const uint32 temp2 = sum0 + majority;
        h = g;
        g = f;
        f = e;
        e = d + temp1;
        d = c;
        c = b;
        b = a;
        a = temp1 + temp2;
    }

    _state[0] += a;
    _state[1] += b;
    _state[2] += c;
    _state[3] += d;
    _state[4] += e;
    _state[5] += f;
    _state[6] += g;
    _state[7] += h;
}

bool ArtifactKey::Parse(const StringAnsiView& text, ArtifactKey& value)
{
    ContentHash digest;
    if (ContentHash::Parse(text, digest))
        return true;
    value = ArtifactKey(digest);
    return false;
}

bool ArtifactKey::Parse(const StringView& text, ArtifactKey& value)
{
    ContentHash digest;
    if (ContentHash::Parse(text, digest))
        return true;
    value = ArtifactKey(digest);
    return false;
}
