// Copyright (c) Wojciech Figat. All rights reserved.

#if COMPILE_WITH_TESTS && USE_EDITOR

#include <ThirdParty/catch2/catch.hpp>
#include "Engine/Scripting/Scripting.h"
#include "Engine/Scripting/ManagedCLR/MClass.h"
#include "Engine/Scripting/ManagedCLR/MException.h"
#include "Engine/Scripting/ManagedCLR/MMethod.h"
#include "Engine/Scripting/ManagedCLR/MUtils.h"
#include "Editor/Content/PreviewsCache.h"

TEST_CASE("Warm thumbnail cache retains canonical slots across restart")
{
    CHECK(PreviewsCache::ShouldRetainSlot(true, false, false, AssetRecordStatus::MissingSource));
    CHECK(PreviewsCache::ShouldRetainSlot(false, true, false, AssetRecordStatus::MissingSource));
    CHECK(PreviewsCache::ShouldRetainSlot(false, false, true, AssetRecordStatus::Ready));
    CHECK(PreviewsCache::ShouldRetainSlot(false, false, true, AssetRecordStatus::Stale));
    CHECK_FALSE(PreviewsCache::ShouldRetainSlot(false, false, true, AssetRecordStatus::MissingSource));
    CHECK_FALSE(PreviewsCache::ShouldRetainSlot(false, false, true, AssetRecordStatus::OrphanMeta));
    CHECK_FALSE(PreviewsCache::ShouldRetainSlot(true, true, true, AssetRecordStatus::OrphanMeta));
    CHECK_FALSE(PreviewsCache::ShouldRetainSlot(false, false, false, AssetRecordStatus::Ready));
}

TEST_CASE("Visible thumbnail demand preserves exact replacement lifecycle")
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
