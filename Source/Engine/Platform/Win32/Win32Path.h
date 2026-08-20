// Copyright (c) Wojciech Figat. All rights reserved.

#pragma once

#include "Engine/Core/Types/String.h"

/// <summary>Converts long absolute Win32 paths to the extended-length form accepted by filesystem APIs.</summary>
inline String GetWin32FilesystemPath(const StringView& path)
{
    String value(path);
    if (value.Length() < 240 || value.StartsWith(TEXT("\\\\?\\")))
        return value;
    value.Replace(TEXT('/'), TEXT('\\'));
    if (value.StartsWith(TEXT("\\\\")))
        return TEXT("\\\\?\\UNC\\") + value.Substring(2);
    if (value.Length() >= 3 && value[1] == ':' && value[2] == '\\')
        return TEXT("\\\\?\\") + value;
    return String(path);
}
