// Copyright (c) Wojciech Figat. All rights reserved.

#pragma once

#include "Engine/Core/Types/StringView.h"

namespace DurableAssetFileSystem
{
    /// <summary>
    /// Flushes file data and metadata to durable storage. Returns true on failure.
    /// </summary>
    bool FlushFile(const StringView& path);

    /// <summary>
    /// Flushes a directory entry table where supported. Returns true on failure.
    /// </summary>
    bool FlushDirectory(const StringView& path);

    /// <summary>
    /// Creates a directory tree and durably publishes new entries. Returns true on failure.
    /// </summary>
    bool EnsureDirectory(const StringView& path);

    /// <summary>
    /// Atomically replaces a file with an adjacent staging file. Returns true on failure.
    /// </summary>
    bool Replace(const StringView& destination, const StringView& staging);

    /// <summary>
    /// Renames a file or directory and durably publishes both parent directories. Returns true on failure.
    /// </summary>
    bool Move(const StringView& destination, const StringView& source, bool overwrite);

    /// <summary>
    /// Deletes a file and durably publishes its parent directory. Returns true on failure.
    /// </summary>
    bool DeleteFile(const StringView& path);

    /// <summary>
    /// Deletes a directory and durably publishes its parent directory. Returns true on failure.
    /// </summary>
    bool DeleteDirectory(const StringView& path, bool deleteContents);

    /// <summary>
    /// Writes and flushes a file and its parent directory. Returns true on failure.
    /// </summary>
    bool WriteFile(const StringView& path, const void* data, int32 length);
}
