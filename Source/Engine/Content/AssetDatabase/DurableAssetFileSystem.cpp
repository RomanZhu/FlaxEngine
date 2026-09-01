// Copyright (c) Wojciech Figat. All rights reserved.

#include "DurableAssetFileSystem.h"
#include "Engine/Platform/File.h"
#include "Engine/Platform/FileSystem.h"
#include "Engine/Platform/StringUtils.h"
#if PLATFORM_WINDOWS
#include "Engine/Platform/Win32/IncludeWindowsHeaders.h"
#elif PLATFORM_LINUX || PLATFORM_MAC
#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>
#endif

namespace DurableAssetFileSystem
{
    bool FlushFile(const StringView& path)
    {
#if PLATFORM_WINDOWS
        const String value(path);
        HANDLE handle = CreateFileW(*value, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (handle == INVALID_HANDLE_VALUE)
            return true;
        const bool failed = FlushFileBuffers(handle) == 0;
        CloseHandle(handle);
        return failed;
#elif PLATFORM_LINUX || PLATFORM_MAC
        const StringAnsi value(path);
        const int handle = open(value.Get(), O_RDWR);
        if (handle == -1)
            return true;
        const bool failed = fsync(handle) != 0;
        close(handle);
        return failed;
#else
        return false;
#endif
    }

    bool FlushDirectory(const StringView& path)
    {
#if PLATFORM_LINUX || PLATFORM_MAC
        const String directory = path.IsEmpty() ? TEXT(".") : String(path);
        const StringAnsi value(directory);
        int flags = O_RDONLY;
#if PLATFORM_LINUX
        flags |= O_DIRECTORY;
#endif
        const int handle = open(value.Get(), flags);
        if (handle == -1)
            return true;
        const bool failed = fsync(handle) != 0;
        close(handle);
        return failed;
#else
        return false;
#endif
    }

    bool EnsureDirectory(const StringView& path)
    {
        if (path.IsEmpty() || FileSystem::DirectoryExists(path))
            return false;
#if PLATFORM_LINUX || PLATFORM_MAC
        const String parent = StringUtils::GetDirectoryName(path);
        if (parent.HasChars() && parent != path && EnsureDirectory(parent))
            return true;
        if (FileSystem::CreateDirectory(path) && !FileSystem::DirectoryExists(path))
            return true;
        return parent.HasChars() && FlushDirectory(parent);
#else
        return FileSystem::CreateDirectory(path);
#endif
    }

    bool Replace(const StringView& destination, const StringView& staging)
    {
#if PLATFORM_WINDOWS
        const String destinationPath(destination);
        const String stagingPath(staging);
        return MoveFileExW(*stagingPath, *destinationPath, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) == 0;
#else
        return Move(destination, staging, true);
#endif
    }

    bool Move(const StringView& destination, const StringView& source, bool overwrite)
    {
#if PLATFORM_LINUX || PLATFORM_MAC
        if (!overwrite && (FileSystem::FileExists(destination) || FileSystem::DirectoryExists(destination)))
            return true;
        const StringAnsi destinationPath(destination);
        const StringAnsi sourcePath(source);
        if (rename(sourcePath.Get(), destinationPath.Get()) != 0)
            return true;
#else
        if (FileSystem::MoveFile(destination, source, overwrite))
            return true;
#endif
        const String destinationParent = StringUtils::GetDirectoryName(destination);
        const String sourceParent = StringUtils::GetDirectoryName(source);
        return FlushDirectory(destinationParent) ||
               (sourceParent != destinationParent && FlushDirectory(sourceParent));
    }

    bool DeleteFile(const StringView& path)
    {
        return FileSystem::DeleteFile(path) || FlushDirectory(StringUtils::GetDirectoryName(path));
    }

    bool DeleteDirectory(const StringView& path, bool deleteContents)
    {
        return FileSystem::DeleteDirectory(path, deleteContents) || FlushDirectory(StringUtils::GetDirectoryName(path));
    }

    bool WriteFile(const StringView& path, const void* data, int32 length)
    {
        const String parent = StringUtils::GetDirectoryName(path);
        return EnsureDirectory(parent) || File::WriteAllBytes(path, data, length) || FlushFile(path) || FlushDirectory(parent);
    }
}
