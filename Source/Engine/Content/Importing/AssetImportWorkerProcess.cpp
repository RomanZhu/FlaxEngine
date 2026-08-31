// Copyright (c) Wojciech Figat. All rights reserved.

#include "AssetImportWorkerProtocol.h"
#include "Engine/Content/Artifacts/ArtifactStore.h"
#include "Engine/Content/AssetDatabase/AssetPath.h"
#include "Engine/Engine/Globals.h"
#include "Engine/Platform/Platform.h"
#include "Engine/Platform/FileSystem.h"
#include <chrono>

#if PLATFORM_WINDOWS
#include "Engine/Platform/Win32/IncludeWindowsHeaders.h"
#include <aclapi.h>
#include <sddl.h>
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
        diagnostic.Target = String(request.Target.BuildKey(ArtifactTargetDimension::All).ToString());
        diagnostic.Message = message;
        return true;
    }

    bool HasQuote(const StringView& value)
    {
        return value.Find(TEXT('"')) != -1;
    }

#if PLATFORM_WINDOWS
    bool ApplyLowIntegrityWriteCapability(const String& path)
    {
        PSECURITY_DESCRIPTOR descriptor = nullptr;
        if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
            L"S:(ML;OICI;NW;;;LW)", SDDL_REVISION_1, &descriptor, nullptr))
            return true;
        BOOL present = FALSE;
        BOOL defaulted = FALSE;
        PACL label = nullptr;
        const bool failed = !GetSecurityDescriptorSacl(descriptor, &present, &label, &defaulted) || !present || !label ||
            SetNamedSecurityInfoW(const_cast<LPWSTR>(path.Get()), SE_FILE_OBJECT, LABEL_SECURITY_INFORMATION,
                nullptr, nullptr, nullptr, label) != ERROR_SUCCESS;
        LocalFree(descriptor);
        return failed;
    }

    bool CreateLowIntegrityRestrictedToken(HANDLE& result)
    {
        result = nullptr;
        HANDLE processToken = nullptr;
        if (!OpenProcessToken(GetCurrentProcess(), TOKEN_DUPLICATE | TOKEN_QUERY | TOKEN_ASSIGN_PRIMARY |
            TOKEN_ADJUST_DEFAULT, &processToken))
            return true;
        HANDLE restrictedToken = nullptr;
        if (!CreateRestrictedToken(processToken, DISABLE_MAX_PRIVILEGE | LUA_TOKEN, 0, nullptr, 0, nullptr, 0, nullptr,
            &restrictedToken))
        {
            CloseHandle(processToken);
            return true;
        }
        CloseHandle(processToken);
        PSID lowIntegrity = nullptr;
        if (!ConvertStringSidToSidW(L"S-1-16-4096", &lowIntegrity))
        {
            CloseHandle(restrictedToken);
            return true;
        }
        TOKEN_MANDATORY_LABEL label = {};
        label.Label.Attributes = SE_GROUP_INTEGRITY;
        label.Label.Sid = lowIntegrity;
        const bool failed = !SetTokenInformation(restrictedToken, TokenIntegrityLevel, &label,
            sizeof(label) + GetLengthSid(lowIntegrity));
        LocalFree(lowIntegrity);
        if (failed)
        {
            CloseHandle(restrictedToken);
            return true;
        }
        result = restrictedToken;
        return false;
    }
#endif
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
    String stagingPath(request.OutputStagingPath);
    String workerRoot = ArtifactStore::GetTemporaryPath(Globals::ProjectLibraryFolder) / TEXT("CallbackWorkers");
    FileSystem::NormalizePath(stagingPath);
    FileSystem::NormalizePath(workerRoot);
    if (executablePath.IsEmpty() || HasQuote(executablePath) || HasQuote(request.OutputStagingPath) ||
        !FileSystem::FileExists(executablePath))
        return ProcessFail(diagnostic, AssetPipelineDiagnosticCode::ProcessorMissing, request, TEXT("Isolated importer worker executable is missing or invalid."));
    if (!AssetPathPolicy::IsSameOrChild(stagingPath, workerRoot) || FileSystem::AreFilePathsEqual(stagingPath, workerRoot))
        return ProcessFail(diagnostic, AssetPipelineDiagnosticCode::UndeclaredInput, request,
            TEXT("Isolated importer output staging is outside the parent-owned worker capability root."));
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
#if PLATFORM_WINDOWS
    if (ApplyLowIntegrityWriteCapability(request.OutputStagingPath))
        return ProcessFail(diagnostic, AssetPipelineDiagnosticCode::BuildFailed, request,
            TEXT("Cannot restrict isolated importer staging to a low-integrity write capability."));
#endif

    const String requestPath = request.OutputStagingPath / TEXT(".worker-request.bin");
    const String resultPath = request.OutputStagingPath / TEXT(".worker-result.bin");
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
    addEnvironment(TEXT("FLAX_ASSET_IMPORT_STAGING"), request.OutputStagingPath);
    addEnvironment(TEXT("TEMP"), request.OutputStagingPath);
    addEnvironment(TEXT("TMP"), request.OutputStagingPath);
    const Char* inheritedNames[] = { TEXT("SystemRoot"), TEXT("WINDIR"), TEXT("DOTNET_ROOT"), TEXT("DOTNET_ROOT(x86)") };
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
    JOBOBJECT_BASIC_UI_RESTRICTIONS ui = {};
    ui.UIRestrictionsClass = JOB_OBJECT_UILIMIT_DESKTOP | JOB_OBJECT_UILIMIT_DISPLAYSETTINGS |
        JOB_OBJECT_UILIMIT_EXITWINDOWS | JOB_OBJECT_UILIMIT_GLOBALATOMS | JOB_OBJECT_UILIMIT_HANDLES |
        JOB_OBJECT_UILIMIT_READCLIPBOARD | JOB_OBJECT_UILIMIT_SYSTEMPARAMETERS | JOB_OBJECT_UILIMIT_WRITECLIPBOARD;
    if (!SetInformationJobObject(jobHandle, JobObjectBasicUIRestrictions, &ui, sizeof(ui)))
    {
        CloseHandle(jobHandle);
        cleanupProtocolFiles();
        return ProcessFail(diagnostic, AssetPipelineDiagnosticCode::ResourceLimitExceeded, request,
            TEXT("Cannot apply isolated importer UI and handle restrictions."));
    }

    HANDLE restrictedToken = nullptr;
    if (CreateLowIntegrityRestrictedToken(restrictedToken))
    {
        CloseHandle(jobHandle);
        cleanupProtocolFiles();
        return ProcessFail(diagnostic, AssetPipelineDiagnosticCode::BuildFailed, request,
            TEXT("Cannot create the low-integrity restricted importer token."));
    }

    STARTUPINFOW startup = {};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESHOWWINDOW;
    startup.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION process = {};
    const DWORD creationFlags = CREATE_SUSPENDED | CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT;
    if (!CreateProcessAsUserW(restrictedToken, executablePath.Get(), commandLine.Get(), nullptr, nullptr, FALSE,
        creationFlags, environment.Get(), request.OutputStagingPath.Get(), &startup, &process))
    {
        CloseHandle(restrictedToken);
        CloseHandle(jobHandle);
        cleanupProtocolFiles();
        return ProcessFail(diagnostic, AssetPipelineDiagnosticCode::BuildFailed, request, TEXT("Cannot start isolated importer process."));
    }
    CloseHandle(restrictedToken);
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
