// Copyright (c) Wojciech Figat. All rights reserved.

#if PLATFORM_WINDOWS || PLATFORM_LINUX || PLATFORM_MAC

#include "Engine/Core/Log.h"
#include "Engine/Engine/CommandLine.h"
#include "Engine/Engine/Engine.h"
#include "Engine/Engine/EngineService.h"
#include "Engine/Scripting/Scripting.h"
#include "Editor/Scripting/ScriptsBuilder.h"

#define CATCH_CONFIG_RUNNER
#include <ThirdParty/catch2/catch.hpp>

class TestsRunnerService : public EngineService
{
public:
    TestsRunnerService()
        : EngineService(TEXT("TestsRunnerService"), 10000)
    {
    }

    void Update() override;
};

TestsRunnerService TestsRunnerServiceInstance;

void TestsRunnerService::Update()
{
    // End if failed to perform a startup
    if (ScriptsBuilder::LastCompilationFailed())
    {
        Engine::RequestExit(-1);
        return;
    }

    // Wait for Editor to be ready for running tests (eg. scripting loaded)
    if (!ScriptsBuilder::IsReady() ||
        !Scripting::IsEveryAssemblyLoaded() ||
        !Scripting::HasGameModulesLoaded())
        return;

    // Runs tests
    LOG_FLOOR();
    LOG(Info, "Running Flax Tests...");
    Catch::Session session;
    Array<StringAnsi> arguments;
    if (CommandLine::Options.CmdLine && !CommandLine::ParseArguments(CommandLine::Options.CmdLine, arguments))
    {
        for (const StringAnsi& argument : arguments)
        {
            const StringAnsiView prefix("-test=");
            if (argument.StartsWith(prefix, StringSearchCase::IgnoreCase) && argument.Length() > prefix.Length())
            {
                const StringAnsi filter = argument.Substring(prefix.Length());
                session.configData().testsOrTags.push_back(filter.Get());
            }
        }
    }
    const int result = session.run();
    if (result == 0)
        LOG(Info, "Flax Tests result: {0}", result);
    else
        LOG(Error, "Flax Tests result: {0}", result);
    LOG_FLOOR();
    Engine::RequestExit(result);
}

#endif
