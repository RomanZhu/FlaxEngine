// Copyright (c) Wojciech Figat. All rights reserved.

#pragma once

#include "Engine/Core/Collections/Dictionary.h"
#include "Engine/Core/Collections/Array.h"
#include "Engine/Core/Types/Guid.h"
#include "Engine/Core/Types/String.h"

class AudioBank;
class AudioEvent;

/// <summary>
/// Runtime catalog mapping durable middleware GUIDs to bank locations and dependencies.
/// </summary>
class FLAXENGINE_API AudioEventCatalog
{
private:
    struct BankRecord
    {
        String Path;
        Array<Guid> Dependencies;
        bool NonBlocking = false;
    };

    static Dictionary<Guid, BankRecord> _banks;
    static bool EnsureBankLoaded(const Guid& bankId, bool preloadSampleData, Array<Guid>& visiting);

public:
    static void Clear();
    static void RegisterBank(const AudioBank* bank);
    static bool EnsureBankLoaded(const Guid& bankId, bool preloadSampleData = false);
    static bool EnsureDependenciesLoaded(const AudioEvent* event);
};
