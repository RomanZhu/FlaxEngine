// Copyright (c) Wojciech Figat. All rights reserved.

#include "AssetImportWorkerProtocol.h"
#include "Engine/Engine/Globals.h"
#include "Engine/Platform/Platform.h"
#include "Engine/Platform/FileSystem.h"
#include <chrono>

#if PLATFORM_WINDOWS
#include "Engine/Platform/Win32/IncludeWindowsHeaders.h"
#endif

namespace
{
    bool ProcessFail(AssetPipelineDiagnostic& diagnostic, AssetPipelineDiagnosticCode code, const AssetImportJobRequest& request,
                     const StringView& message)
    {
        diagnostic = AssetPipelineDiagnostic();
        diagnostic.Code = code;
        diagnostic.Stage = AssetPipelineDiagnosticStage::Build;
        diagnostic.AssetGuid = request.Asset.Value;
        diagnostic.SourcePath = request.SourcePath;
        diagnostic.ProcessorId = request.Importer.ID;
        diagnostic.Target = request.Target;
        diagnostic.Message = message;
        return true;
    }

    bool HasQuote(const StringView& value)
    {
        return value.Find(TEXT('"')) != -1;
    }
}

bool AssetImportWorkerProcess::Run(const StringView& executable, const AssetImportJobRequest& request,
                                   const AssetCancellationToken& cancellation, AssetImportJobResult& result,
                                   AssetPipelineDiagnostic& diagnostic)
{
    result = AssetImportJobResult();
    result.JobID = request.JobID;
    result.Capability = request.Capability;
    if (AssetImportWorkerProtocol::ValidateRequest(request, diagnostic))
        return true;
    const String executablePath(executable);
    if (executablePath.IsEmpty() || HasQuote(executablePath) || HasQuote(request.OutputStagingPath) ||
        !FileSystem::FileExists(executablePath))
        return ProcessFail(diagnostic, AssetPipelineDiagnosticCode::ProcessorMissing, request, TEXT("Isolated importer worker executable is missing or invalid."));
    if (FileSystem::DirectoryExists(request.OutputStagingPath))
    {
        Array<String> existing;
        if (FileSystem::DirectoryGetFiles(existing, request.OutputStagingPath, TEXT("*"), DirectorySearchOption::AllDirectories) || existing.HasItems())
            return ProcessFail(diagnostic, AssetPipelineDiagnosticCode::ArtifactInvalid, request, TEXT("Isolated importer output staging must be a new empty directory."));
    }
    else if (FileSystem::CreateDirectory(request.OutputStagingPath))
    {
        return ProcessFail(diagnostic, AssetPipelineDiagnosticCode::LibraryCreationFailed, request, TEXT("Cannot create isolated importer output staging."));
    }

    const String job = request.JobID.ToString(Guid::FormatType::N);
    const String requestPath = request.OutputStagingPath + TEXT(".request-") + job;
    const String resultPath = request.OutputStagingPath + TEXT(".result-") + job;
    auto cleanupProtocolFiles = [&]()
    {
        FileSystem::DeleteFile(requestPath);
        FileSystem::DeleteFile(resultPath);
    };
    if (AssetImportWorkerProtocol::SaveRequest(requestPath, request, diagnostic))
    {
        cleanupProtocolFiles();
        return true;
    }

#if PLATFORM_WINDOWS
    String arguments;
    if (request.Importer.ProviderKind == AssetProcessorProviderKind::Managed)
    {
        if (Globals::ProjectFolder.IsEmpty() || HasQuote(Globals::ProjectFolder))
        {
            cleanupProtocolFiles();
            return ProcessFail(diagnostic, AssetPipelineDiagnosticCode::BuildFailed, request,
                TEXT("Managed importer worker requires a valid current project path."));
        }
        arguments = String::Format(TEXT("\"{0}\" -headless -null -mute -skipcompile -project \"{1}\""),
            executablePath, Globals::ProjectFolder);
    }
    else
    {
        arguments = String::Format(TEXT("\"{0}\" --flax-asset-import-worker \"{1}\" \"{2}\" \"{3}\""),
            executablePath, requestPath, resultPath, request.Capability.ToString(Guid::FormatType::N));
    }
    Array<Char> commandLine;
    commandLine.Add(arguments.Get(), arguments.Length());
    commandLine.Add(TEXT('\0'));
    const String protocol = String::Format(TEXT("{0}"), request.ProtocolVersion);
    const String capability = request.Capability.ToString(Guid::FormatType::N);
    Array<Char> environment;
    auto addEnvironment = [&environment](const Char* name, const StringView& value)
    {
        const int32 nameLength = StringView(name).Length();
        environment.Add(name, nameLength);
        environment.Add(TEXT('='));
        environment.Add(value.Get(), value.Length());
        environment.Add(TEXT('\0'));
    };
    addEnvironment(TEXT("FLAX_ASSET_IMPORT_WORKER"), TEXT("1"));
    addEnvironment(TEXT("FLAX_ASSET_IMPORT_PROTOCOL"), protocol);
    addEnvironment(TEXT("FLAX_ASSET_IMPORT_CAPABILITY"), capability);
    addEnvironment(TEXT("FLAX_ASSET_IMPORT_REQUEST"), requestPath);
    addEnvironment(TEXT("FLAX_ASSET_IMPORT_RESULT"), resultPath);
    const Char* inheritedNames[] = { TEXT("SystemRoot"), TEXT("WINDIR"), TEXT("TEMP"), TEXT("TMP"), TEXT("DOTNET_ROOT") };
    for (const Char* name : inheritedNames)
    {
        String value;
        if (!Platform::GetEnvironmentVariable(name, value) && value.HasChars())
            addEnvironment(name, value);
    }
    environment.Add(TEXT('\0'));

    HANDLE jobHandle = CreateJobObjectW(nullptr, nullptr);
    if (!jobHandle)
    {
        cleanupProtocolFiles();
        return ProcessFail(diagnostic, AssetPipelineDiagnosticCode::BuildFailed, request, TEXT("Cannot create isolated importer process job."));
    }
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits = {};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE | JOB_OBJECT_LIMIT_ACTIVE_PROCESS | JOB_OBJECT_LIMIT_PROCESS_MEMORY;
    limits.BasicLimitInformation.ActiveProcessLimit = 1;
    limits.ProcessMemoryLimit = static_cast<SIZE_T>(request.Limits.MaximumMemoryBytes);
    if (!SetInformationJobObject(jobHandle, JobObjectExtendedLimitInformation, &limits, sizeof(limits)))
    {
        CloseHandle(jobHandle);
        cleanupProtocolFiles();
        return ProcessFail(diagnostic, AssetPipelineDiagnosticCode::ResourceLimitExceeded, request, TEXT("Cannot apply isolated importer process limits."));
    }

    STARTUPINFOW startup = {};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESHOWWINDOW;
    startup.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION process = {};
    const DWORD creationFlags = CREATE_SUSPENDED | CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT;
    if (!CreateProcessW(executablePath.Get(), commandLine.Get(), nullptr, nullptr, FALSE, creationFlags, environment.Get(),
        request.OutputStagingPath.Get(), &startup, &process))
    {
        CloseHandle(jobHandle);
        cleanupProtocolFiles();
        return ProcessFail(diagnostic, AssetPipelineDiagnosticCode::BuildFailed, request, TEXT("Cannot start isolated importer process."));
    }
    if (!AssignProcessToJobObject(jobHandle, process.hProcess) || ResumeThread(process.hThread) == static_cast<DWORD>(-1))
    {
        TerminateJobObject(jobHandle, 1);
        WaitForSingleObject(process.hProcess, INFINITE);
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
        CloseHandle(jobHandle);
        cleanupProtocolFiles();
        return ProcessFail(diagnostic, AssetPipelineDiagnosticCode::BuildFailed, request, TEXT("Cannot attach isolated importer process limits."));
    }
    CloseHandle(process.hThread);

    const auto started = std::chrono::steady_clock::now();
    bool cancelled = false;
    bool timedOut = false;
    for (;;)
    {
        const DWORD wait = WaitForSingleObject(process.hProcess, 10);
        if (wait == WAIT_OBJECT_0)
            break;
        if (wait == WAIT_FAILED)
        {
            cancelled = true;
            break;
        }
        if (cancellation.IsCancellationRequested())
        {
            cancelled = true;
            break;
        }
        const uint64 elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count();
        if (elapsed >= request.Limits.TimeoutMilliseconds)
        {
            timedOut = true;
            break;
        }
    }
    if (cancelled || timedOut)
    {
        TerminateJobObject(jobHandle, timedOut ? 2 : 3);
        WaitForSingleObject(process.hProcess, INFINITE);
    }
    DWORD exitCode = 1;
    GetExitCodeProcess(process.hProcess, &exitCode);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION usage = {};
    QueryInformationJobObject(jobHandle, JobObjectExtendedLimitInformation, &usage, sizeof(usage), nullptr);
    CloseHandle(process.hProcess);
    CloseHandle(jobHandle);
    if (cancelled)
    {
        cleanupProtocolFiles();
        result.Status = AssetImportWorkerStatus::Cancelled;
        return ProcessFail(diagnostic, AssetPipelineDiagnosticCode::BuildCancelled, request, TEXT("Isolated importer process was cancelled."));
    }
    if (timedOut)
    {
        cleanupProtocolFiles();
        result.Status = AssetImportWorkerStatus::TimedOut;
        return ProcessFail(diagnostic, AssetPipelineDiagnosticCode::ResourceLimitExceeded, request, TEXT("Isolated importer process exceeded its timeout."));
    }
    if (exitCode != 0)
    {
        cleanupProtocolFiles();
        result.Status = AssetImportWorkerStatus::Crashed;
        return ProcessFail(diagnostic, AssetPipelineDiagnosticCode::BuildFailed, request, TEXT("Isolated importer process crashed or exceeded its memory limit."));
    }
    if (AssetImportWorkerProtocol::LoadResult(resultPath, result, diagnostic))
    {
        cleanupProtocolFiles();
        return true;
    }
    if (result.PeakMemory < static_cast<uint64>(usage.PeakProcessMemoryUsed))
        result.PeakMemory = static_cast<uint64>(usage.PeakProcessMemoryUsed);
#else
    cleanupProtocolFiles();
    return ProcessFail(diagnostic, AssetPipelineDiagnosticCode::BuildFailed, request,
        TEXT("Strict isolated importer process execution is not implemented on this platform."));
#endif

    cleanupProtocolFiles();
    return AssetImportWorkerProtocol::ValidateResult(request, result, diagnostic);
}
