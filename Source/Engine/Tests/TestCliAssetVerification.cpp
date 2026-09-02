// Copyright (c) Wojciech Figat. All rights reserved.

#if COMPILE_WITH_TESTS && USE_EDITOR

#include <ThirdParty/catch2/catch.hpp>
#include "Engine/Scripting/Scripting.h"
#include "Engine/Scripting/ManagedCLR/MClass.h"
#include "Engine/Scripting/ManagedCLR/MException.h"
#include "Engine/Scripting/ManagedCLR/MMethod.h"
#include "Engine/Scripting/ManagedCLR/MUtils.h"

TEST_CASE("Headless scene creation shares one persistent source and metadata GUID")
{
#if USE_CSHARP && USE_NETCORE
    MClass* testClass = Scripting::FindClass("FlaxEngine.Tests.TestCliAssetVerification");
    REQUIRE(testClass);
    MMethod* testMethod = testClass->GetMethod("RunHeadlessSceneCreatePublishesMetadata", 0);
    REQUIRE(testMethod);
    MObject* exception = nullptr;
    MObject* result = testMethod->Invoke(nullptr, nullptr, &exception);
    if (exception)
        MException(exception).Log(LogType::Error, TEXT("TestCliAssetVerification"));
    CHECK_FALSE(exception);
    REQUIRE(result);
    CHECK(MUtils::Unbox<int32>(result) == 0);
#endif
}

#endif
