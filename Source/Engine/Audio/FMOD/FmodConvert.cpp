// Copyright (c) Wojciech Figat. All rights reserved.

#include "FmodConvert.h"

#if AUDIO_EVENT_API_FMOD

#include "Engine/Core/Log.h"

FMOD_GUID FmodConvert::ToFmodGuid(const Guid& guid)
{
    FMOD_GUID fg;
    fg.Data1 = guid.A;
    // FMOD's runtime ID APIs consume the native Windows/System.Guid byte
    // layout. Canonical Studio text IDs are converted separately by editor
    // authoring code when catalogs are generated.
    fg.Data2 = (unsigned short)(guid.B & 0xFFFF);
    fg.Data3 = (unsigned short)(guid.B >> 16);
    fg.Data4[0] = (unsigned char)(guid.C & 0xFF);
    fg.Data4[1] = (unsigned char)((guid.C >> 8) & 0xFF);
    fg.Data4[2] = (unsigned char)((guid.C >> 16) & 0xFF);
    fg.Data4[3] = (unsigned char)(guid.C >> 24);
    fg.Data4[4] = (unsigned char)(guid.D & 0xFF);
    fg.Data4[5] = (unsigned char)((guid.D >> 8) & 0xFF);
    fg.Data4[6] = (unsigned char)((guid.D >> 16) & 0xFF);
    fg.Data4[7] = (unsigned char)(guid.D >> 24);
    return fg;
}

Guid FmodConvert::FromFmodGuid(const FMOD_GUID& fg)
{
    uint32 a = fg.Data1;
    uint32 b = ((uint32)fg.Data3 << 16) | (uint32)fg.Data2;
    uint32 c = (uint32)fg.Data4[0] | ((uint32)fg.Data4[1] << 8) | ((uint32)fg.Data4[2] << 16) | ((uint32)fg.Data4[3] << 24);
    uint32 d = (uint32)fg.Data4[4] | ((uint32)fg.Data4[5] << 8) | ((uint32)fg.Data4[6] << 16) | ((uint32)fg.Data4[7] << 24);
    return Guid(a, b, c, d);
}

FMOD_GUID FmodConvert::ToFmodStudioGuid(const Guid& guid)
{
    FMOD_GUID fg;
    fg.Data1 = guid.A;
    fg.Data2 = (unsigned short)(guid.B >> 16);
    fg.Data3 = (unsigned short)(guid.B & 0xFFFF);
    fg.Data4[0] = (unsigned char)(guid.C >> 24);
    fg.Data4[1] = (unsigned char)((guid.C >> 16) & 0xFF);
    fg.Data4[2] = (unsigned char)((guid.C >> 8) & 0xFF);
    fg.Data4[3] = (unsigned char)(guid.C & 0xFF);
    fg.Data4[4] = (unsigned char)(guid.D >> 24);
    fg.Data4[5] = (unsigned char)((guid.D >> 16) & 0xFF);
    fg.Data4[6] = (unsigned char)((guid.D >> 8) & 0xFF);
    fg.Data4[7] = (unsigned char)(guid.D & 0xFF);
    return fg;
}

Guid FmodConvert::FromFmodStudioGuid(const FMOD_GUID& fg)
{
    const uint32 b = ((uint32)fg.Data2 << 16) | (uint32)fg.Data3;
    const uint32 c = ((uint32)fg.Data4[0] << 24) | ((uint32)fg.Data4[1] << 16) |
                     ((uint32)fg.Data4[2] << 8) | (uint32)fg.Data4[3];
    const uint32 d = ((uint32)fg.Data4[4] << 24) | ((uint32)fg.Data4[5] << 16) |
                     ((uint32)fg.Data4[6] << 8) | (uint32)fg.Data4[7];
    return Guid(fg.Data1, b, c, d);
}

bool FmodConvert::CheckResult(FMOD_RESULT result, const char* operation)
{
    if (result != FMOD_OK)
    {
        LOG(Warning, "[FMOD] {0} failed: ({1}) {2}", String(operation), (int32)result, String(FMOD_ErrorString(result)));
        return false;
    }
    return true;
}

#endif
