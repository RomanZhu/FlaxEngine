// Copyright (c) Wojciech Figat. All rights reserved.

#include "FmodBankPathResolver.h"
#include "FmodBankManifest.h"
#include "Engine/Audio/AudioSettings.h"
#include "Engine/Engine/Globals.h"
#include "Engine/Platform/FileSystem.h"
#include "Engine/Core/Log.h"

bool FmodBankPathResolver::Resolve(const StringView& requestedPath, String& result)
{
    result = String(requestedPath);
    if (result.IsEmpty())
        return false;
    if (result.StartsWith(TEXT("bank:/")))
    {
        const String legacyPath = result;
        result = result.Substring(6);
        if (!result.EndsWith(TEXT(".bank")))
            result += TEXT(".bank");
        LOG(Warning, "FMOD AudioBank path '{0}' uses obsolete middleware metadata instead of a compiled bank filename. Migrating to '{1}'; resynchronize the typed bank asset.", legacyPath, result);
    }
    if (!FileSystem::IsRelative(result))
#if USE_EDITOR
        return FileSystem::FileExists(result);
#else
        return false;
#endif

    result.Replace(TEXT("\\"), TEXT("/"));

    // Localized banks are commonly referenced by their logical base name in
    // scenes (for example Dialogue.bank), while FMOD emits Dialogue_EN.bank.
    // Resolve the configured locale first and use English as the deterministic
    // fallback for projects whose locale is left at "default".
    Array<String, InlinedAllocation<3>> paths;
    paths.Add(result);
    const String directory(StringUtils::GetDirectoryName(result));
    const String baseName(StringUtils::GetFileNameWithoutExtension(result));
    String locale = AudioSettings::Get()->AudioLocale.ToUpper();
    if (locale.IsEmpty() || locale == TEXT("DEFAULT"))
        locale = TEXT("EN");
    const auto addLocalized = [&](const StringView& language)
    {
        const String filename = baseName + TEXT("_") + language + TEXT(".bank");
        const String path = directory.HasChars() ? directory / filename : filename;
        if (!paths.Contains(path))
            paths.Add(path);
    };
    addLocalized(locale);
    addLocalized(TEXT("EN"));
#if USE_EDITOR
    // Authoring metadata may use either project-relative paths or paths relative
    // to the conventional Content/Audio/Banks directory.
    for (const String& path : paths)
    {
        String candidate = Globals::ProjectFolder / path;
        if (FileSystem::FileExists(candidate))
        {
            result = MoveTemp(candidate);
            return true;
        }
        candidate = Globals::ProjectContentFolder / TEXT("Audio/Banks") / path;
        if (FileSystem::FileExists(candidate))
        {
            result = MoveTemp(candidate);
            return true;
        }
    }
    return false;
#else
    // Cooked runtime accepts only the platform/locale bank set explicitly selected
    // by CookAudioStep. It deliberately never consults ProjectFolder or arbitrary files.
    for (const String& path : paths)
    {
        if (FmodBankManifest::ResolveBank(path, result))
            return true;
    }
    return false;
#endif
}
