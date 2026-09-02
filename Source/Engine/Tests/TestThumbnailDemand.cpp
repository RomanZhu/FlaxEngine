// Copyright (c) Wojciech Figat. All rights reserved.

#if COMPILE_WITH_TESTS && USE_EDITOR

#include <ThirdParty/catch2/catch.hpp>
#include "Engine/Scripting/Scripting.h"
#include "Engine/Scripting/ManagedCLR/MClass.h"
#include "Engine/Scripting/ManagedCLR/MException.h"
#include "Engine/Scripting/ManagedCLR/MMethod.h"
#include "Engine/Scripting/ManagedCLR/MUtils.h"

TEST_CASE("Visible thumbnail demand expires without removing ordinary ownership")
{
#if USE_CSHARP && USE_NETCORE
    MClass* testClass = Scripting::FindClass("FlaxEngine.Tests.TestThumbnailDemand");
    REQUIRE(testClass);
    MMethod* testMethod = testClass->GetMethod("RunOrdinaryOwnershipDoesNotCreateThumbnailDemand", 0);
    REQUIRE(testMethod);
    MObject* exception = nullptr;
    MObject* result = testMethod->Invoke(nullptr, nullptr, &exception);
    if (exception)
        MException(exception).Log(LogType::Error, TEXT("TestThumbnailDemand"));
    CHECK_FALSE(exception);
    REQUIRE(result);
    CHECK(MUtils::Unbox<int32>(result) == 0);
#endif
}

#endif
