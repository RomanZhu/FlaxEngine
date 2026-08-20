// Copyright (c) Wojciech Figat. All rights reserved.

#include "AudioSurfaceResolver.h"
#include "AudioSurfaceLibrary.h"

const AudioSurfaceProfile* AudioSurfaceResolver::Resolve(const AudioSurfaceLibrary& library, Tag tag)
{
    if (const auto* exact = library.Profiles.TryGet(tag))
        return exact;
    String name = tag.ToString();
    while (name.HasChars())
    {
        const int32 split = name.FindLast(TEXT('.'));
        if (split < 0)
            break;
        name = name.Left(split);
        const Tag parent = Tags::Find(name);
        if (const auto* profile = library.Profiles.TryGet(parent))
            return profile;
    }
    return &library.DefaultProfile;
}
