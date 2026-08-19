// Copyright (c) Wojciech Figat. All rights reserved.

#include "Engine/Core/Types/DateTime.h"
#include "Engine/Engine/Time.h"
#include <ThirdParty/catch2/catch.hpp>

namespace
{
    struct TimeStateGuard
    {
        bool GamePaused;
        float PhysicsFPS;
        float TimeScale;

        TimeStateGuard()
            : GamePaused(Time::GetGamePaused())
            , PhysicsFPS(Time::PhysicsFPS)
            , TimeScale(Time::TimeScale)
        {
        }

        ~TimeStateGuard()
        {
            Time::SetFixedDeltaTime(false, 0.0f);
            Time::PhysicsFPS = PhysicsFPS;
            Time::TimeScale = TimeScale;
            Time::SetGamePaused(GamePaused);
        }
    };

    int32 ConsumeFixedSteps(Time::FixedStepTickData& data, double time, float physicsFps, float maxCatchUpTime)
    {
        int32 result = 0;
        while (data.OnTickBegin(time, physicsFps, maxCatchUpTime))
        {
            data.OnTickEnd();
            result++;
        }
        return result;
    }
}

TEST_CASE("DateTime")
{
    SECTION("Test Convertion")
    {
        constexpr int year = 2023;
        constexpr int month = 12;
        constexpr int day = 16;
        constexpr int hour = 23;
        constexpr int minute = 50;
        constexpr int second = 13;
        constexpr int millisecond = 5;
        const DateTime dt1(year, month, day, hour, minute, second, millisecond);
        CHECK(dt1.GetYear() == year);
        CHECK(dt1.GetMonth() == month);
        CHECK(dt1.GetDay() == day);
        CHECK(dt1.GetHour() == hour);
        CHECK(dt1.GetMinute() == minute);
        CHECK(dt1.GetSecond() == second);
        CHECK(dt1.GetMillisecond() == millisecond);
    }
}

TEST_CASE("TimeFixedStep")
{
    TimeStateGuard guard;
    Time::SetFixedDeltaTime(false, 0.0f);
    Time::SetGamePaused(false);
    Time::TimeScale = 1.0f;

    SECTION("FixedDeltaTime uses PhysicsFPS")
    {
        Time::PhysicsFPS = 50.0f;
        CHECK(Time::GetFixedDeltaTime() == Approx(0.02f));

        Time::PhysicsFPS = 0.0f;
        CHECK(Time::GetFixedDeltaTime() == Approx(0.0f));
    }

    SECTION("No tick before fixed timestep accumulates")
    {
        Time::FixedStepTickData data;
        data.Synchronize(60.0f, 0.0, true);

        CHECK(ConsumeFixedSteps(data, 1.0 / 120.0, 60.0f, 1.0f) == 0);
        CHECK(ConsumeFixedSteps(data, 1.0 / 60.0, 60.0f, 1.0f) == 1);
        CHECK(data.UnscaledDeltaTime.GetTotalSeconds() == Approx(1.0 / 60.0));
        CHECK(data.DeltaTime.GetTotalSeconds() == Approx(1.0 / 60.0));
    }

    SECTION("Long frame consumes multiple fixed ticks below catch-up cap")
    {
        Time::FixedStepTickData data;
        data.Synchronize(60.0f, 0.0, true);

        CHECK(ConsumeFixedSteps(data, 0.1, 60.0f, 1.0f) == 6);
        CHECK(data.TicksCount == 6);
        CHECK(data.UnscaledTime.GetTotalSeconds() == Approx(6.0 / 60.0));
    }

    SECTION("Catch-up cap limits fixed ticks")
    {
        Time::FixedStepTickData data;
        data.Synchronize(60.0f, 0.0, true);

        CHECK(ConsumeFixedSteps(data, 1.0, 60.0f, 0.1f) == 6);
        CHECK(data.AccumulatedTime == Approx(0.0).margin(1e-8));
    }

    SECTION("Fixed tick delta is stable across catch-up")
    {
        Time::TimeScale = 0.5f;
        Time::FixedStepTickData data;
        data.Synchronize(50.0f, 0.0, true);

        int32 ticks = 0;
        while (data.OnTickBegin(0.1, 50.0f, 1.0f))
        {
            CHECK(data.UnscaledDeltaTime.GetTotalSeconds() == Approx(0.02));
            CHECK(data.DeltaTime.GetTotalSeconds() == Approx(0.01));
            data.OnTickEnd();
            ticks++;
        }
        CHECK(ticks == 5);
    }

    SECTION("Paused or zero time scale does not accumulate backlog")
    {
        Time::FixedStepTickData data;
        data.Synchronize(60.0f, 0.0, true);

        Time::SetGamePaused(true);
        CHECK(ConsumeFixedSteps(data, 1.0, 60.0f, 0.1f) == 0);

        Time::SetGamePaused(false);
        CHECK(ConsumeFixedSteps(data, 1.0 + 1.0 / 60.0, 60.0f, 0.1f) == 1);

        data.Synchronize(60.0f, 0.0, true);
        Time::TimeScale = 0.0f;
        CHECK(ConsumeFixedSteps(data, 1.0, 60.0f, 0.1f) == 0);

        Time::TimeScale = 1.0f;
        CHECK(ConsumeFixedSteps(data, 1.0 + 1.0 / 60.0, 60.0f, 0.1f) == 1);
    }
}
