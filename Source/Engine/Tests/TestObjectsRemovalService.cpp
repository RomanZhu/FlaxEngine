// Copyright (c) Wojciech Figat. All rights reserved.

#include "Engine/Core/ObjectsRemovalService.h"
#include <ThirdParty/catch2/catch.hpp>

namespace
{
    class TestRemovalObject : public Object
    {
    public:
        enum class ReentryMode
        {
            None,
            Deferred,
            Immediate,
        };

        int32& DeleteCalls;
        int32& DestructorCalls;
        ReentryMode Mode;
        TestRemovalObject* Nested = nullptr;

        TestRemovalObject(int32& deleteCalls, int32& destructorCalls, ReentryMode mode = ReentryMode::None)
            : DeleteCalls(deleteCalls)
            , DestructorCalls(destructorCalls)
            , Mode(mode)
        {
        }

        ~TestRemovalObject() override
        {
            DestructorCalls++;
        }

        String ToString() const override
        {
            return TEXT("TestRemovalObject");
        }

        void OnDeleteObject() override
        {
            DeleteCalls++;
            if (Nested)
            {
                Nested->DeleteObject();
                Nested = nullptr;
            }
            if (Mode == ReentryMode::Deferred)
                DeleteObject();
            else if (Mode == ReentryMode::Immediate)
                DeleteObjectNow();
            Object::OnDeleteObject();
        }
    };
}

TEST_CASE("ObjectsRemovalService")
{
    SECTION("Deferred self-deletion is ignored during deletion")
    {
        int32 deleteCalls = 0;
        int32 destructorCalls = 0;
        auto* object = New<TestRemovalObject>(deleteCalls, destructorCalls, TestRemovalObject::ReentryMode::Deferred);

        object->DeleteObject();
        ObjectsRemovalService::Flush();

        CHECK(deleteCalls == 1);
        CHECK(destructorCalls == 1);
    }

    SECTION("Immediate self-deletion is ignored during deletion")
    {
        int32 deleteCalls = 0;
        int32 destructorCalls = 0;
        auto* object = New<TestRemovalObject>(deleteCalls, destructorCalls, TestRemovalObject::ReentryMode::Immediate);

        object->DeleteObject();
        ObjectsRemovalService::Flush();

        CHECK(deleteCalls == 1);
        CHECK(destructorCalls == 1);
    }

    SECTION("Nested objects are deleted in the same flush")
    {
        int32 deleteCalls = 0;
        int32 destructorCalls = 0;
        auto* nested = New<TestRemovalObject>(deleteCalls, destructorCalls);
        auto* object = New<TestRemovalObject>(deleteCalls, destructorCalls);
        object->Nested = nested;

        object->DeleteObject();
        ObjectsRemovalService::Flush();

        CHECK(deleteCalls == 2);
        CHECK(destructorCalls == 2);
    }

    SECTION("Deletion timeout can be updated before deletion starts")
    {
        int32 deleteCalls = 0;
        int32 destructorCalls = 0;
        auto* object = New<TestRemovalObject>(deleteCalls, destructorCalls);

        object->DeleteObject(1000.0f);
        object->DeleteObject();
        ObjectsRemovalService::Flush();

        CHECK(deleteCalls == 1);
        CHECK(destructorCalls == 1);
    }
}
