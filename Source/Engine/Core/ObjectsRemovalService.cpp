// Copyright (c) Wojciech Figat. All rights reserved.

#include "ObjectsRemovalService.h"
#include "Utilities.h"
#include "Collections/Dictionary.h"
#include "Engine/Engine/Time.h"
#include "Engine/Engine/EngineService.h"
#include "Engine/Platform/Platform.h"
#include "Engine/Platform/CriticalSection.h"
#include "Engine/Profiler/ProfilerCPU.h"
#include "Engine/Scripting/ScriptingObject.h"

const Char* BytesSizesData[] = { TEXT("b"), TEXT("Kb"), TEXT("Mb"), TEXT("Gb"), TEXT("Tb"), TEXT("Pb"), TEXT("Eb"), TEXT("Zb"), TEXT("Yb") };
const Char* HertzSizesData[] = { TEXT("Hz"), TEXT("KHz"), TEXT("MHz"), TEXT("GHz"), TEXT("THz"), TEXT("PHz"), TEXT("EHz"), TEXT("ZHz"), TEXT("YHz") };
Span<const Char*> Utilities::Private::BytesSizes(BytesSizesData, ARRAY_COUNT(BytesSizesData));
Span<const Char*> Utilities::Private::HertzSizes(HertzSizesData, ARRAY_COUNT(HertzSizesData));

namespace
{
    struct Removal
    {
        float TimeToLive;
        uint32 Epoch;
    };

    struct RemovalState
    {
        CriticalSection PoolLocker;
        double LastUpdate = 0;
        Dictionary<Object*, Removal> Pool;
        Dictionary<Object*, uint32> Live;
        uint64 PoolCounter = 0;
        uint32 NextEpoch = 1;
        uint16 GameReloadSerial = 1;

        RemovalState()
        {
            Instance = this;
        }

        ~RemovalState()
        {
            if (Instance == this)
                Instance = nullptr;
        }

        static RemovalState* Instance;
    };

    RemovalState* RemovalState::Instance = nullptr;

    RemovalState& GetRemovalState()
    {
        static RemovalState instance;
        return instance;
    }

    bool TryGetLiveEpoch(Object* obj, uint32& epoch)
    {
        return obj && GetRemovalState().Live.TryGet(obj, epoch);
    }

    bool IsPooledObjectCurrent(Object* obj, uint32 epoch)
    {
        uint32 liveEpoch;
        if (!TryGetLiveEpoch(obj, liveEpoch) || liveEpoch != epoch)
            return false;
        const uint16 serial = obj->GameReloadSerial;
        return serial == 0 || serial == GetRemovalState().GameReloadSerial;
    }

    bool CanInvokeObject(Object* obj)
    {
        const uint16 serial = obj->GameReloadSerial;
        return serial == 0 || serial == GetRemovalState().GameReloadSerial;
    }

    void DeleteObjectInternal(Object* obj)
    {
        if (!obj)
            return;
        auto& state = GetRemovalState();
        state.PoolLocker.Lock();
        uint32 liveEpoch;
        if (!state.Live.TryGet(obj, liveEpoch) || !CanInvokeObject(obj) || EnumHasAnyFlags(obj->Flags, ObjectFlags::IsDeleting))
        {
            state.PoolLocker.Unlock();
            return;
        }
        obj->Flags |= ObjectFlags::IsDeleting;
        state.PoolLocker.Unlock();
        obj->OnDeleteObject();
    }
}

class ObjectsRemoval : public EngineService
{
public:
    ObjectsRemoval()
        : EngineService(TEXT("Objects Removal Service"), -1000)
    {
    }

    bool Init() override;
    void LateUpdate() override;
    void Dispose() override;
};

ObjectsRemoval ObjectsRemovalInstance;

bool ObjectsRemovalService::IsLive(Object* obj)
{
    auto& state = GetRemovalState();
    state.PoolLocker.Lock();
    uint32 epoch;
    const bool result = TryGetLiveEpoch(obj, epoch);
    state.PoolLocker.Unlock();
    return result;
}

bool ObjectsRemovalService::IsInPool(Object* obj)
{
    auto& state = GetRemovalState();
    state.PoolLocker.Lock();
    const bool result = state.Pool.ContainsKey(obj);
    state.PoolLocker.Unlock();
    return result;
}

void ObjectsRemovalService::Dereference(Object* obj)
{
    auto& state = GetRemovalState();
    state.PoolLocker.Lock();
    state.Pool.Remove(obj);
    state.PoolLocker.Unlock();
}

void ObjectsRemovalService::Add(Object* obj, float timeToLive, bool useGameTime)
{
    auto& state = GetRemovalState();
    state.PoolLocker.Lock();
    uint32 epoch;
    // Never touch obj->Flags unless this pointer is still a live Object. Leaked ALC
    // callbacks can call DeleteObject on natives that were already destroyed.
    if (!TryGetLiveEpoch(obj, epoch) || !CanInvokeObject(obj) || EnumHasAnyFlags(obj->Flags, ObjectFlags::IsDeleting))
    {
        state.PoolLocker.Unlock();
        return;
    }

    obj->Flags |= ObjectFlags::WasMarkedToDelete;
    if (useGameTime)
        obj->Flags |= ObjectFlags::UseGameTimeForDelete;
    else
        obj->Flags &= ~ObjectFlags::UseGameTimeForDelete;

    state.Pool[obj] = { timeToLive, epoch };
    state.PoolCounter++;
    state.PoolLocker.Unlock();
}

void ObjectsRemovalService::Flush(float dt, float gameDelta)
{
    PROFILE_CPU();

    auto& state = GetRemovalState();
    state.PoolLocker.Lock();
    state.PoolCounter = 0;

    // Update timeouts and delete objects that timed out
    for (auto i = state.Pool.Begin(); i.IsNotEnd(); ++i)
    {
        auto& bucket = *i;
        Object* obj = bucket.Key;
        const uint32 epoch = bucket.Value.Epoch;
        if (!IsPooledObjectCurrent(obj, epoch))
        {
            state.Pool.Remove(i);
            continue;
        }
        const float ttl = bucket.Value.TimeToLive - ((obj->Flags & ObjectFlags::UseGameTimeForDelete) != ObjectFlags::None ? gameDelta : dt);
        if (ttl <= 0.0f)
        {
            state.Pool.Remove(i);
            DeleteObjectInternal(obj);
        }
        else
        {
            bucket.Value.TimeToLive = ttl;
        }
    }

    // If any object was added to the pool while removing objects (by this thread) then retry removing any nested objects (but without delta time)
    if (state.PoolCounter != 0)
    {
    RETRY:
        state.PoolCounter = 0;
        for (auto i = state.Pool.Begin(); i.IsNotEnd(); ++i)
        {
            Object* obj = i->Key;
            const uint32 epoch = i->Value.Epoch;
            if (!IsPooledObjectCurrent(obj, epoch))
            {
                state.Pool.Remove(i);
                continue;
            }
            if (i->Value.TimeToLive <= 0.0f)
            {
                state.Pool.Remove(i);
                DeleteObjectInternal(obj);
            }
        }
        if (state.PoolCounter != 0)
            goto RETRY;
    }

    state.PoolLocker.Unlock();
}

void ObjectsRemovalService::ForceFlush()
{
    PROFILE_CPU();

    auto& state = GetRemovalState();
    state.PoolLocker.Lock();
    do
    {
        state.PoolCounter = 0;
        for (auto i = state.Pool.Begin(); i.IsNotEnd(); ++i)
        {
            Object* obj = i->Key;
            const uint32 epoch = i->Value.Epoch;
            state.Pool.Remove(i);
            if (IsPooledObjectCurrent(obj, epoch))
                DeleteObjectInternal(obj);
        }
    } while (state.PoolCounter != 0);
    state.Pool.Clear();
    state.PoolLocker.Unlock();
}

uint16 ObjectsRemovalService::GetGameReloadSerial()
{
    return GetRemovalState().GameReloadSerial;
}

void ObjectsRemovalService::SealExistingObjects()
{
    auto& state = GetRemovalState();
    state.PoolLocker.Lock();
    uint16 next = (uint16)(state.GameReloadSerial + 1);
    if (next == 0)
        next = 1;
    state.GameReloadSerial = next;
    for (auto i = state.Pool.Begin(); i.IsNotEnd(); ++i)
    {
        Object* obj = i->Key;
        const uint32 epoch = i->Value.Epoch;
        if (!IsPooledObjectCurrent(obj, epoch))
            state.Pool.Remove(i);
    }
    state.PoolLocker.Unlock();
}

bool ObjectsRemoval::Init()
{
    auto& state = GetRemovalState();
    state.Pool.EnsureCapacity(8192);
    state.Live.EnsureCapacity(8192);
    state.LastUpdate = Platform::GetTimeSeconds();
    return false;
}

void ObjectsRemoval::LateUpdate()
{
    PROFILE_CPU();

    auto& state = GetRemovalState();

    // Delete all objects
    const double now = Platform::GetTimeSeconds();
    const float dt = (float)(now - state.LastUpdate);
    float gameDelta = Time::Update.DeltaTime.GetTotalSeconds();
    if (Time::GetGamePaused())
        gameDelta = 0;
    ObjectsRemovalService::Flush(dt, gameDelta);
    state.LastUpdate = now;
}

void ObjectsRemoval::Dispose()
{
    // Collect new objects
    ObjectsRemovalService::Flush();

    // Delete all remaining live objects and drop stale keys
    {
        auto& state = GetRemovalState();
        state.PoolLocker.Lock();
        for (auto i = state.Pool.Begin(); i.IsNotEnd(); ++i)
        {
            Object* obj = i->Key;
            const uint32 epoch = i->Value.Epoch;
            state.Pool.Remove(i);
            if (IsPooledObjectCurrent(obj, epoch))
                DeleteObjectInternal(obj);
        }
        state.Pool.Clear();
        state.PoolLocker.Unlock();
    }
}

Object::Object()
{
    auto& state = GetRemovalState();
    state.PoolLocker.Lock();
    uint32 epoch = state.NextEpoch++;
    if (epoch == 0)
        epoch = state.NextEpoch++;
    state.Live[this] = epoch;
    state.PoolLocker.Unlock();
}

Object::~Object()
{
    if (!RemovalState::Instance)
        return;
    auto& state = GetRemovalState();
    state.PoolLocker.Lock();
    state.Live.Remove(this);
    state.Pool.Remove(this);
    state.PoolLocker.Unlock();
}

void Object::DeleteObjectNow()
{
    ObjectsRemovalService::Dereference(this);

    DeleteObjectInternal(this);
}

void Object::DeleteObject(float timeToLive, bool useGameTime)
{
    // Add to deferred remove (or just update timeout but don't remove object here)
    ObjectsRemovalService::Add(this, timeToLive, useGameTime);
}
