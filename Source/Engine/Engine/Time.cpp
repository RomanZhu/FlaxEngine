// Copyright (c) Wojciech Figat. All rights reserved.

#include "Time.h"
#include "EngineService.h"
#include "Engine/Core/Math/Math.h"
#include "Engine/Platform/Platform.h"
#include "Engine/Core/Config/TimeSettings.h"
#include "Engine/Serialization/Serialization.h"
#if !USE_EDITOR
#include "Engine/Engine/Engine.h"
#endif

namespace
{
    bool FixedDeltaTimeEnable;
    float FixedDeltaTimeValue;
    float MaxUpdateDeltaTime = 0.1f;
    float MaxPhysicsCatchUpTime = 0.3f;

    double GetFixedStep(float fps)
    {
        return fps > ZeroTolerance ? 1.0 / fps : 0.0;
    }
#if USE_EDITOR
    constexpr bool _gamePausedUnfocsed = false;
    #define GetFps(fps) fps
#else
    bool _gamePausedUnfocsed = false;
    float UnfocusedMaxFPS = 0.0f;
    bool UnfocusedPause = false;
    float GetFps(float fps)
    {
        if (UnfocusedMaxFPS > 0 && !Engine::HasFocus && fps > UnfocusedMaxFPS)
            fps = UnfocusedMaxFPS;
        return fps;
    }
#endif
}

bool Time::_gamePaused = false;
DateTime Time::StartupTime;
float Time::UpdateFPS = 60.0f;
float Time::PhysicsFPS = 60.0f;
float Time::TimeScale = 1.0f;
Time::TickData Time::Update;
Time::FixedStepTickData Time::Physics;
Time::TickData Time::Draw;
Time::TickData* Time::Current = nullptr;

class TimeService : public EngineService
{
public:

    TimeService()
        : EngineService(TEXT("Time"), -850)
    {
        FixedDeltaTimeEnable = false;
        FixedDeltaTimeValue = 0.0f;

#if USE_EDITOR
        // Disable gameplay in Editor on startup
        Time::_gamePaused = true;
#endif
    }
};

TimeService TimeServiceInstance;

void TimeSettings::Apply()
{
    Time::UpdateFPS = UpdateFPS;
    Time::PhysicsFPS = PhysicsFPS;
    Time::TimeScale = TimeScale;
    ::MaxUpdateDeltaTime = MaxUpdateDeltaTime;
    ::MaxPhysicsCatchUpTime = MaxPhysicsCatchUpTime;
#if !USE_EDITOR
    ::UnfocusedMaxFPS = UnfocusedMaxFPS;
    ::UnfocusedPause = UnfocusedPause;
#endif
}

void Time::TickData::Synchronize(float targetFps, double currentTime, bool resetTotalTime)
{
    OnReset(targetFps, currentTime);
    if (resetTotalTime)
        Time = UnscaledTime = TimeSpan::Zero();
    NextBegin = targetFps > ZeroTolerance ? LastBegin + (1.0f / targetFps) : 0.0;
}

void Time::TickData::OnReset(float targetFps, double currentTime)
{
    DeltaTime = UnscaledDeltaTime = targetFps > ZeroTolerance ? TimeSpan::FromSeconds(1.0f / targetFps) : TimeSpan::Zero();
    LastLength = static_cast<double>(DeltaTime.Ticks) / TimeSpan::TicksPerSecond;
    LastBegin = currentTime - LastLength;
    LastEnd = currentTime;
}

bool Time::TickData::OnTickBegin(double time, float targetFps, float maxDeltaTime)
{
    // Check if can perform a tick
    double deltaTime;
    if (FixedDeltaTimeEnable)
    {
        deltaTime = (double)FixedDeltaTimeValue;
    }
    else
    {
        if (time < NextBegin)
            return false;

        deltaTime = Math::Max((time - LastBegin), 0.0);
        if (deltaTime > maxDeltaTime)
        {
            deltaTime = (double)maxDeltaTime;
            NextBegin = time;
        }

        if (targetFps > ZeroTolerance)
        {
            int skip = (int)(1 + (time - NextBegin) * targetFps);
            NextBegin += (1.0 / targetFps) * skip;
        }
    }

    // Update data
    Advance(time, deltaTime);

    return true;
}

void Time::TickData::OnTickEnd()
{
    const double time = Platform::GetTimeSeconds();
    LastEnd = time;
    LastLength = time - LastBegin;
}

void Time::TickData::Advance(double time, double deltaTime)
{
    float timeScale = TimeScale;
    if (_gamePaused || _gamePausedUnfocsed)
        timeScale = 0.0f;
    LastBegin = time;
    UnscaledDeltaTime = TimeSpan::FromSeconds(deltaTime);
    UnscaledTime += UnscaledDeltaTime;
    DeltaTime = TimeSpan::FromSeconds(deltaTime * (double)timeScale);
    Time += DeltaTime;
    TicksCount++;
}

void Time::FixedStepTickData::OnReset(float targetFps, double currentTime)
{
    FixedDeltaTime = GetFixedStep(targetFps);
    AccumulatedTime = 0.0;
    PendingSteps = 0;
    LastUpdateTime = currentTime;
    DeltaTime = TimeSpan::FromSeconds(FixedDeltaTime * (double)TimeScale);
    UnscaledDeltaTime = TimeSpan::FromSeconds(FixedDeltaTime);
    LastBegin = currentTime;
    LastEnd = currentTime;
    LastLength = 0.0;
    NextBegin = FixedDeltaTime > ZeroToleranceDouble ? currentTime + FixedDeltaTime : 0.0;
}

bool Time::FixedStepTickData::OnTickBegin(double time, float targetFps, float maxCatchUpTime)
{
    FixedDeltaTime = GetFixedStep(targetFps);
    if (FixedDeltaTime <= ZeroToleranceDouble)
    {
        AccumulatedTime = 0.0;
        PendingSteps = 0;
        LastUpdateTime = time;
        NextBegin = 0.0;
        return false;
    }

    MaxCatchUpTime = Math::Max((double)maxCatchUpTime, FixedDeltaTime);
    if (PendingSteps <= 0)
    {
        const double elapsedTime = Math::Max(time - LastUpdateTime, 0.0);
        LastUpdateTime = time;
        if (_gamePaused || _gamePausedUnfocsed || TimeScale <= ZeroTolerance)
        {
            AccumulatedTime = 0.0;
            NextBegin = time + FixedDeltaTime;
            return false;
        }

        AccumulatedTime = Math::Min(AccumulatedTime + elapsedTime, MaxCatchUpTime);
        PendingSteps = (int32)((AccumulatedTime + ZeroToleranceDouble) / FixedDeltaTime);
        if (PendingSteps <= 0)
        {
            NextBegin = time + Math::Max(FixedDeltaTime - AccumulatedTime, 0.0);
            return false;
        }
    }

    PendingSteps--;
    AccumulatedTime = Math::Max(AccumulatedTime - FixedDeltaTime, 0.0);
    NextBegin = PendingSteps > 0 ? time : time + Math::Max(FixedDeltaTime - AccumulatedTime, 0.0);
    Advance(Platform::GetTimeSeconds(), FixedDeltaTime);

    return true;
}

double Time::GetNextTick()
{
    const double nextUpdate = Time::Update.NextBegin;
    const double nextPhysics = Time::Physics.NextBegin;

    double nextTick = MAX_double;
    if (UpdateFPS > ZeroTolerance && nextUpdate < nextTick)
        nextTick = nextUpdate;
    if (PhysicsFPS > ZeroTolerance && nextPhysics < nextTick)
        nextTick = nextPhysics;

    if (nextTick == MAX_double)
        return 0.0;
    return nextTick;
}

void Time::SetGamePaused(bool value)
{
    if (_gamePaused == value)
        return;
    _gamePaused = value;
    if (_gamePausedUnfocsed)
        return;

    // Reset ticking
    const double time = Platform::GetTimeSeconds();
    Update.OnReset(UpdateFPS, time);
    Physics.OnReset(PhysicsFPS, time);
    Draw.OnReset(UpdateFPS, time);
}

float Time::GetDeltaTime()
{
    auto* data = Current ? Current : &Update;
    return data->DeltaTime.GetTotalSeconds();
}

float Time::GetGameTime()
{
    auto* data = Current ? Current : &Update;
    return data->Time.GetTotalSeconds();
}

float Time::GetUnscaledDeltaTime()
{
    auto* data = Current ? Current : &Update;
    return data->UnscaledDeltaTime.GetTotalSeconds();
}

float Time::GetFixedDeltaTime()
{
    return (float)GetFixedStep(GetFps(PhysicsFPS));
}

float Time::GetUnscaledGameTime()
{
    auto* data = Current ? Current : &Update;
    return data->UnscaledTime.GetTotalSeconds();
}

float Time::GetTimeSinceStartup()
{
    return (DateTime::Now() - StartupTime).GetTotalSeconds();
}

void Time::SetFixedDeltaTime(bool enable, float value)
{
    FixedDeltaTimeEnable = enable;
    FixedDeltaTimeValue = value;
}

void Time::Synchronize(bool resetTotalTime)
{
    // Initialize tick data (based on a time settings)
    const double time = Platform::GetTimeSeconds();
    Update.Synchronize(UpdateFPS, time, resetTotalTime);
    Physics.Synchronize(PhysicsFPS, time, resetTotalTime);
    Draw.Synchronize(UpdateFPS, time, resetTotalTime);
}

bool Time::OnBeginUpdate(double time)
{
#if !USE_EDITOR
    // Pause game if window lost focus (based on game settings)
    bool gamePausedUnfocsed = !Engine::HasFocus && UnfocusedPause;
    if (gamePausedUnfocsed != _gamePausedUnfocsed)
    {
        _gamePausedUnfocsed = gamePausedUnfocsed;
        if (!gamePausedUnfocsed)
        {
            // Reset ticking
            const double time = Platform::GetTimeSeconds();
            Update.OnReset(UpdateFPS, time);
            Physics.OnReset(PhysicsFPS, time);
            Draw.OnReset(UpdateFPS, time);
        }
    }
#endif

    if (Update.OnTickBegin(time, GetFps(UpdateFPS), MaxUpdateDeltaTime))
    {
        Current = &Update;
        return true;
    }
    return false;
}

bool Time::OnBeginPhysics(double time)
{
    if (Physics.OnTickBegin(time, GetFps(PhysicsFPS), MaxPhysicsCatchUpTime))
    {
        Current = &Physics;
        return true;
    }
    return false;
}

void Time::OnBeginDraw()
{
    Draw.LastBegin = Update.LastBegin;
    Draw.NextBegin = Update.NextBegin;
    Draw.DeltaTime = Update.DeltaTime;
    Draw.Time = Update.Time;
    Draw.UnscaledDeltaTime = Update.UnscaledDeltaTime;
    Draw.UnscaledTime = Update.UnscaledTime;
    Draw.TicksCount = Update.TicksCount;
    Current = &Draw;
}

void Time::OnEndUpdate()
{
    Update.OnTickEnd();
    Current = nullptr;
}

void Time::OnEndPhysics()
{
    Physics.OnTickEnd();
    Current = nullptr;
}

void Time::OnEndDraw()
{
    Draw.OnTickEnd();
    Current = nullptr;
}
