// Copyright (c) Wojciech Figat. All rights reserved.

#include "FmodBankPathResolver.h"
#include "FmodBankManifest.h"
#include "Engine/Engine/Globals.h"
#include "Engine/Platform/FileSystem.h"

bool FmodBankPathResolver::Resolve(const StringView& requestedPath, String& result)
{
    result = String(requestedPath);
    if (result.IsEmpty())
        return false;
    if (!FileSystem::IsRelative(result))
#if USE_EDITOR
        return FileSystem::FileExists(result);
#else
        return false;
#endif

    result.Replace(TEXT("\\"), TEXT("/"));
#if USE_EDITOR
    // Authoring metadata may use either project-relative paths or paths relative
    // to the conventional Content/Audio/Banks directory.
    String candidate = Globals::ProjectFolder / result;
    if (FileSystem::FileExists(candidate))
    {
        result = MoveTemp(candidate);
        return true;
    }
    candidate = Globals::ProjectContentFolder / TEXT("Audio/Banks") / result;
#else
    // Cooked runtime accepts only the platform/locale bank set explicitly selected
    // by CookAudioStep. It deliberately never consults ProjectFolder or arbitrary files.
    return FmodBankManifest::ResolveBank(result, result);
#endif
#if USE_EDITOR
    if (FileSystem::FileExists(candidate))
    {
        result = MoveTemp(candidate);
        return true;
    }
    return false;
#endif
}
