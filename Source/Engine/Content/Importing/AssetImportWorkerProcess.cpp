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
#elif PLATFORM_LINUX || PLATFORM_MAC
#include <cstddef>
#include <cerrno>
#include <fcntl.h>
#include <signal.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#if PLATFORM_LINUX
#include <linux/audit.h>
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <sched.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#ifndef SECCOMP_RET_KILL_PROCESS
#define SECCOMP_RET_KILL_PROCESS SECCOMP_RET_KILL
#endif
#if defined(__has_include)
#if __has_include(<linux/landlock.h>)
#include <linux/landlock.h>
#define FLAX_HAS_LINUX_LANDLOCK 1
#endif
#endif
#ifndef FLAX_HAS_LINUX_LANDLOCK
#define FLAX_HAS_LINUX_LANDLOCK 0
#endif
#endif
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
#elif PLATFORM_LINUX || PLATFORM_MAC
    enum class PosixWorkerSetupFailure : int32
    {
        None = 0,
        ProcessGroup,
        ResourceLimits,
        WorkingDirectory,
        DescriptorIsolation,
        WriteSandbox,
        Execute,
    };

    bool SetWorkerLimit(int32 resource, uint64 softValue, uint64 hardValue)
    {
        const uint64 infinity = static_cast<uint64>(RLIM_INFINITY);
        struct rlimit inherited;
        if (getrlimit(resource, &inherited) != 0)
            return true;
        struct rlimit limit;
        limit.rlim_cur = softValue >= infinity ? RLIM_INFINITY : static_cast<rlim_t>(softValue);
        limit.rlim_max = hardValue >= infinity ? RLIM_INFINITY : static_cast<rlim_t>(hardValue);
        if (inherited.rlim_max != RLIM_INFINITY)
        {
            if (limit.rlim_cur == RLIM_INFINITY || limit.rlim_cur > inherited.rlim_max)
                limit.rlim_cur = inherited.rlim_max;
            if (limit.rlim_max == RLIM_INFINITY || limit.rlim_max > inherited.rlim_max)
                limit.rlim_max = inherited.rlim_max;
        }
        if (limit.rlim_cur > limit.rlim_max)
            limit.rlim_cur = limit.rlim_max;
        return setrlimit(resource, &limit) != 0;
    }

    bool ApplyPosixResourceLimits(const AssetImportWorkerResourceLimits& limits)
    {
        const uint64 requestedCpuSeconds = (static_cast<uint64>(limits.TimeoutMilliseconds) + 999) / 1000;
        const uint64 cpuSeconds = requestedCpuSeconds > 1 ? requestedCpuSeconds : 1;
        if (SetWorkerLimit(RLIMIT_CORE, 0, 0) ||
            SetWorkerLimit(RLIMIT_FSIZE, limits.MaximumOutputBytes, limits.MaximumOutputBytes) ||
            SetWorkerLimit(RLIMIT_AS, limits.MaximumMemoryBytes, limits.MaximumMemoryBytes) ||
            SetWorkerLimit(RLIMIT_CPU, cpuSeconds, cpuSeconds == MAX_uint64 ? cpuSeconds : cpuSeconds + 1))
            return true;

        struct rlimit descriptors;
        if (getrlimit(RLIMIT_NOFILE, &descriptors) != 0)
            return true;
        descriptors.rlim_cur = descriptors.rlim_max < static_cast<rlim_t>(256) ? descriptors.rlim_max : static_cast<rlim_t>(256);
        return setrlimit(RLIMIT_NOFILE, &descriptors) != 0;
    }

#if PLATFORM_LINUX
    bool ApplyLinuxWriteSandbox(const char* stagingPath)
    {
#if FLAX_HAS_LINUX_LANDLOCK && defined(__NR_landlock_create_ruleset) && defined(__NR_landlock_add_rule) && defined(__NR_landlock_restrict_self)
        if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0)
            return true;
        const int abi = static_cast<int>(syscall(__NR_landlock_create_ruleset, nullptr, 0, LANDLOCK_CREATE_RULESET_VERSION));
        if (abi < 1)
            return true;
        __u64 access = LANDLOCK_ACCESS_FS_WRITE_FILE | LANDLOCK_ACCESS_FS_REMOVE_DIR |
            LANDLOCK_ACCESS_FS_REMOVE_FILE | LANDLOCK_ACCESS_FS_MAKE_CHAR | LANDLOCK_ACCESS_FS_MAKE_DIR |
            LANDLOCK_ACCESS_FS_MAKE_REG | LANDLOCK_ACCESS_FS_MAKE_SOCK | LANDLOCK_ACCESS_FS_MAKE_FIFO |
            LANDLOCK_ACCESS_FS_MAKE_BLOCK | LANDLOCK_ACCESS_FS_MAKE_SYM;
#ifdef LANDLOCK_ACCESS_FS_REFER
        if (abi >= 2)
            access |= LANDLOCK_ACCESS_FS_REFER;
#endif
#ifdef LANDLOCK_ACCESS_FS_TRUNCATE
        if (abi >= 3)
            access |= LANDLOCK_ACCESS_FS_TRUNCATE;
#endif
        landlock_ruleset_attr ruleset = {};
        ruleset.handled_access_fs = access;
        const int rulesetFd = static_cast<int>(syscall(__NR_landlock_create_ruleset, &ruleset, sizeof(ruleset), 0));
        if (rulesetFd < 0)
            return true;
        const int stagingFd = open(stagingPath, O_PATH | O_CLOEXEC);
        if (stagingFd < 0)
        {
            close(rulesetFd);
            return true;
        }
        landlock_path_beneath_attr path = {};
        path.allowed_access = access;
        path.parent_fd = stagingFd;
        const bool failed = syscall(__NR_landlock_add_rule, rulesetFd, LANDLOCK_RULE_PATH_BENEATH, &path, 0) != 0 ||
            syscall(__NR_landlock_restrict_self, rulesetFd, 0) != 0;
        close(stagingFd);
        close(rulesetFd);
        return failed;
#else
        return true;
#endif
    }

    bool ApplyLinuxSyscallSandbox()
    {
#if defined(__x86_64__)
        constexpr uint32 auditArchitecture = AUDIT_ARCH_X86_64;
#elif defined(__aarch64__)
        constexpr uint32 auditArchitecture = AUDIT_ARCH_AARCH64;
#else
        return true;
#endif
        const sock_filter filter[] =
        {
            BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(seccomp_data, arch)),
            BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, auditArchitecture, 1, 0),
            BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_KILL_PROCESS),
            BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(seccomp_data, nr)),
#ifdef __NR_fork
            BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, __NR_fork, 0, 1),
            BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | EPERM),
#endif
#ifdef __NR_vfork
            BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, __NR_vfork, 0, 1),
            BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | EPERM),
#endif
#ifdef __NR_clone3
            BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, __NR_clone3, 0, 1),
            BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | ENOSYS),
#endif
#ifdef __NR_unshare
            BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, __NR_unshare, 0, 1),
            BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | EPERM),
#endif
#ifdef __NR_setns
            BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, __NR_setns, 0, 1),
            BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | EPERM),
#endif
#ifdef __NR_connect
            BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, __NR_connect, 0, 1),
            BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | EPERM),
#endif
#ifdef __NR_bind
            BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, __NR_bind, 0, 1),
            BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | EPERM),
#endif
#ifdef __NR_listen
            BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, __NR_listen, 0, 1),
            BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | EPERM),
#endif
#ifdef __NR_accept
            BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, __NR_accept, 0, 1),
            BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | EPERM),
#endif
#ifdef __NR_accept4
            BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, __NR_accept4, 0, 1),
            BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | EPERM),
#endif
#ifdef __NR_sendto
            BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, __NR_sendto, 0, 1),
            BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | EPERM),
#endif
#ifdef __NR_sendmsg
            BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, __NR_sendmsg, 0, 1),
            BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | EPERM),
#endif
#ifdef __NR_sendmmsg
            BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, __NR_sendmmsg, 0, 1),
            BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | EPERM),
#endif
#ifdef __NR_clone
            BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, __NR_clone, 0, 3),
            BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(seccomp_data, args[0])),
            BPF_JUMP(BPF_JMP | BPF_JSET | BPF_K, CLONE_THREAD, 1, 0),
            BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | EPERM),
#endif
            BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
        };
        sock_fprog program = {};
        program.len = static_cast<unsigned short>(ARRAY_COUNT(filter));
        program.filter = const_cast<sock_filter*>(filter);
        return prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0 ||
            prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, &program) != 0;
    }
#elif PLATFORM_MAC
    bool BuildMacSandboxProfile(const StringAnsiView& stagingPath, StringAnsi& profile)
    {
        StringAnsi escaped;
        escaped.ReserveSpace(stagingPath.Length() + 16);
        for (int32 i = 0; i < stagingPath.Length(); i++)
        {
            const char value = stagingPath[i];
            if (value == '\\' || value == '"')
                escaped += '\\';
            else if (static_cast<byte>(value) < 0x20 || static_cast<byte>(value) == 0x7f)
                return true;
            escaped += value;
        }
        profile = StringAnsi::Format(
            "(version 1)\n"
            "(allow default)\n"
            "(deny network*)\n"
            "(deny process-fork)\n"
            "(deny file-write*)\n"
            "(allow file-write* (literal \"/dev/null\"))\n"
            "(allow file-write* (subpath \"{0}\"))\n", escaped);
        return profile.IsEmpty();
    }

#endif

    void ReportPosixSetupFailure(int32 descriptor, PosixWorkerSetupFailure failure)
    {
        const int32 value = static_cast<int32>(failure);
        while (write(descriptor, &value, sizeof(value)) < 0 && errno == EINTR)
        {
        }
        _exit(126);
    }

    void CloseInheritedDescriptors(int32 first, int32 maximum)
    {
#if PLATFORM_LINUX && defined(__NR_close_range)
        if (syscall(__NR_close_range, static_cast<unsigned int>(first), ~0u, 0) == 0)
            return;
#endif
        for (int32 descriptor = first; descriptor < maximum; descriptor++)
            close(descriptor);
    }

    const Char* GetPosixSetupFailureMessage(PosixWorkerSetupFailure failure)
    {
        switch (failure)
        {
        case PosixWorkerSetupFailure::ProcessGroup:
            return TEXT("Cannot isolate the importer worker process group.");
        case PosixWorkerSetupFailure::ResourceLimits:
            return TEXT("Cannot apply isolated importer resource limits.");
        case PosixWorkerSetupFailure::WorkingDirectory:
            return TEXT("Cannot enter the isolated importer staging directory.");
        case PosixWorkerSetupFailure::DescriptorIsolation:
            return TEXT("Cannot isolate inherited importer process descriptors.");
        case PosixWorkerSetupFailure::WriteSandbox:
            return TEXT("Cannot establish the importer write and process sandbox on this host.");
        case PosixWorkerSetupFailure::Execute:
            return TEXT("Cannot execute the isolated importer worker.");
        default:
            return TEXT("Cannot initialize the isolated importer worker.");
        }
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
#if PLATFORM_LINUX || PLATFORM_MAC
    if (request.AllowedTools.HasItems())
    {
        result.Status = AssetImportWorkerStatus::Rejected;
        return ProcessFail(diagnostic, AssetPipelineDiagnosticCode::InvalidSettingsCombination, request,
            TEXT("Strict POSIX importer isolation does not yet support brokered child tools."));
    }
#endif
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
#elif PLATFORM_LINUX || PLATFORM_MAC
    const StringAnsi stagingCapability(stagingPath);
    if (chmod(stagingCapability.Get(), S_IRWXU) != 0)
        return ProcessFail(diagnostic, AssetPipelineDiagnosticCode::BuildFailed, request,
            TEXT("Cannot restrict isolated importer staging permissions."));
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
#elif PLATFORM_LINUX || PLATFORM_MAC
    if (request.Importer.ProviderKind == AssetProcessorProviderKind::Managed &&
        (Globals::ProjectFolder.IsEmpty() || HasQuote(Globals::ProjectFolder)))
    {
        cleanupProtocolFiles();
        return ProcessFail(diagnostic, AssetPipelineDiagnosticCode::BuildFailed, request,
            TEXT("Managed importer worker requires a valid current project path."));
    }

    const String capability = request.Capability.ToString(Guid::FormatType::N);
    const String protocol = String::Format(TEXT("{0}"), request.ProtocolVersion);
    const String posixExecutablePath = FileSystem::ConvertRelativePathToAbsolute(Platform::GetWorkingDirectory(), executablePath);
    const StringAnsi executableAnsi(posixExecutablePath);
    const StringAnsi requestAnsi(requestPath);
    const StringAnsi resultAnsi(resultPath);
    const StringAnsi capabilityAnsi(capability);
    const StringAnsi projectAnsi(Globals::ProjectFolder);
    Array<char*> arguments;
    arguments.Add(const_cast<char*>(executableAnsi.Get()));
    if (request.Importer.ProviderKind == AssetProcessorProviderKind::Managed)
    {
        const char* managedArguments[] = { "-headless", "-null", "-mute", "-nolog", "-skipcompile", "-project" };
        for (const char* value : managedArguments)
            arguments.Add(const_cast<char*>(value));
        arguments.Add(const_cast<char*>(projectAnsi.Get()));
    }
    else
    {
        arguments.Add(const_cast<char*>("--flax-asset-import-worker"));
        arguments.Add(const_cast<char*>(requestAnsi.Get()));
        arguments.Add(const_cast<char*>(resultAnsi.Get()));
        arguments.Add(const_cast<char*>(capabilityAnsi.Get()));
    }
    arguments.Add(nullptr);

    Array<StringAnsi> environmentValues;
    environmentValues.Add(StringAnsi("FLAX_ASSET_IMPORT_WORKER=1"));
    environmentValues.Add(StringAnsi("FLAX_ASSET_IMPORT_PROTOCOL=") + StringAnsi(protocol));
    environmentValues.Add(StringAnsi("FLAX_ASSET_IMPORT_CAPABILITY=") + capabilityAnsi);
    environmentValues.Add(StringAnsi("FLAX_ASSET_IMPORT_REQUEST=") + requestAnsi);
    environmentValues.Add(StringAnsi("FLAX_ASSET_IMPORT_RESULT=") + resultAnsi);
    environmentValues.Add(StringAnsi("FLAX_ASSET_IMPORT_STAGING=") + stagingCapability);
    environmentValues.Add(StringAnsi("HOME=") + stagingCapability);
    environmentValues.Add(StringAnsi("TMPDIR=") + stagingCapability);
    environmentValues.Add(StringAnsi("TMP=") + stagingCapability);
    environmentValues.Add(StringAnsi("TEMP=") + stagingCapability);
    environmentValues.Add(StringAnsi("DOTNET_EnableDiagnostics=0"));
    environmentValues.Add(StringAnsi("COMPlus_EnableDiagnostics=0"));
    auto inheritEnvironment = [&environmentValues](const char* ansiName, const Char* name)
    {
        String value;
        if (!Platform::GetEnvironmentVariable(name, value) && value.HasChars())
            environmentValues.Add(StringAnsi(ansiName) + "=" + StringAnsi(value));
    };
    inheritEnvironment("DOTNET_ROOT", TEXT("DOTNET_ROOT"));
    inheritEnvironment("LD_LIBRARY_PATH", TEXT("LD_LIBRARY_PATH"));
    inheritEnvironment("DYLD_LIBRARY_PATH", TEXT("DYLD_LIBRARY_PATH"));
    inheritEnvironment("LANG", TEXT("LANG"));
    inheritEnvironment("LC_ALL", TEXT("LC_ALL"));
    Array<char*> environment;
    environment.EnsureCapacity(environmentValues.Count() + 1);
    for (StringAnsi& value : environmentValues)
        environment.Add(value.Get());
    environment.Add(nullptr);

#if PLATFORM_MAC
    StringAnsi macSandboxProfile;
    if (BuildMacSandboxProfile(stagingCapability, macSandboxProfile))
    {
        cleanupProtocolFiles();
        return ProcessFail(diagnostic, AssetPipelineDiagnosticCode::BuildFailed, request,
            TEXT("Cannot construct the isolated importer sandbox policy."));
    }
    const String macSandboxExecutable(TEXT("/usr/bin/sandbox-exec"));
    if (!FileSystem::FileExists(macSandboxExecutable))
    {
        cleanupProtocolFiles();
        return ProcessFail(diagnostic, AssetPipelineDiagnosticCode::BuildFailed, request,
            TEXT("Strict macOS importer isolation requires /usr/bin/sandbox-exec."));
    }
    const StringAnsi macSandboxExecutableAnsi(macSandboxExecutable);
    Array<char*> macSandboxArguments;
    macSandboxArguments.Add(const_cast<char*>("sandbox-exec"));
    macSandboxArguments.Add(const_cast<char*>("-p"));
    macSandboxArguments.Add(macSandboxProfile.Get());
    macSandboxArguments.Add(const_cast<char*>(executableAnsi.Get()));
    for (int32 i = 1; i < arguments.Count(); i++)
        macSandboxArguments.Add(arguments[i]);
#endif

    auto moveDescriptorAboveStandardStreams = [](int32& descriptor)
    {
        if (descriptor > STDERR_FILENO)
            return false;
        const int32 moved = fcntl(descriptor, F_DUPFD_CLOEXEC, STDERR_FILENO + 1);
        if (moved < 0)
            return true;
        close(descriptor);
        descriptor = moved;
        return false;
    };
    int32 setupPipe[2] = { -1, -1 };
    if (pipe(setupPipe) != 0 || moveDescriptorAboveStandardStreams(setupPipe[0]) ||
        moveDescriptorAboveStandardStreams(setupPipe[1]) || fcntl(setupPipe[1], F_SETFD, FD_CLOEXEC) != 0 ||
        fcntl(setupPipe[0], F_SETFL, O_NONBLOCK) != 0)
    {
        if (setupPipe[0] >= 0)
            close(setupPipe[0]);
        if (setupPipe[1] >= 0)
            close(setupPipe[1]);
        cleanupProtocolFiles();
        return ProcessFail(diagnostic, AssetPipelineDiagnosticCode::BuildFailed, request,
            TEXT("Cannot create isolated importer setup channel."));
    }
    int32 nullDescriptor = open("/dev/null", O_RDWR | O_CLOEXEC);
    if (nullDescriptor < 0 || moveDescriptorAboveStandardStreams(nullDescriptor))
    {
        if (nullDescriptor >= 0)
            close(nullDescriptor);
        close(setupPipe[0]);
        close(setupPipe[1]);
        cleanupProtocolFiles();
        return ProcessFail(diagnostic, AssetPipelineDiagnosticCode::BuildFailed, request,
            TEXT("Cannot isolate importer standard streams."));
    }
    long openDescriptorLimit = sysconf(_SC_OPEN_MAX);
    if (openDescriptorLimit < 4)
    {
        close(nullDescriptor);
        close(setupPipe[0]);
        close(setupPipe[1]);
        cleanupProtocolFiles();
        return ProcessFail(diagnostic, AssetPipelineDiagnosticCode::BuildFailed, request,
            TEXT("Cannot determine the importer descriptor isolation boundary."));
    }
    if (openDescriptorLimit > MAX_int32)
        openDescriptorLimit = MAX_int32;

    const auto started = std::chrono::steady_clock::now();
    const pid_t process = fork();
    if (process < 0)
    {
        close(nullDescriptor);
        close(setupPipe[0]);
        close(setupPipe[1]);
        cleanupProtocolFiles();
        return ProcessFail(diagnostic, AssetPipelineDiagnosticCode::BuildFailed, request,
            TEXT("Cannot start isolated importer process."));
    }
    if (process == 0)
    {
        close(setupPipe[0]);
        int32 setupDescriptor = setupPipe[1];
        if (setupPipe[1] != 3)
        {
            if (dup2(setupPipe[1], 3) < 0)
                ReportPosixSetupFailure(setupDescriptor, PosixWorkerSetupFailure::DescriptorIsolation);
            close(setupPipe[1]);
            setupDescriptor = 3;
        }
        if (fcntl(setupDescriptor, F_SETFD, FD_CLOEXEC) != 0)
            ReportPosixSetupFailure(setupDescriptor, PosixWorkerSetupFailure::DescriptorIsolation);
        if (setpgid(0, 0) != 0)
            ReportPosixSetupFailure(3, PosixWorkerSetupFailure::ProcessGroup);
        if (ApplyPosixResourceLimits(request.Limits))
            ReportPosixSetupFailure(3, PosixWorkerSetupFailure::ResourceLimits);
        if (chdir(stagingCapability.Get()) != 0)
            ReportPosixSetupFailure(3, PosixWorkerSetupFailure::WorkingDirectory);
#if PLATFORM_LINUX
        if (ApplyLinuxWriteSandbox(stagingCapability.Get()) || ApplyLinuxSyscallSandbox())
            ReportPosixSetupFailure(3, PosixWorkerSetupFailure::WriteSandbox);
#endif
        if (dup2(nullDescriptor, STDIN_FILENO) < 0 || dup2(nullDescriptor, STDOUT_FILENO) < 0 ||
            dup2(nullDescriptor, STDERR_FILENO) < 0)
            ReportPosixSetupFailure(3, PosixWorkerSetupFailure::DescriptorIsolation);
        if (nullDescriptor > 3)
            close(nullDescriptor);
        CloseInheritedDescriptors(4, static_cast<int32>(openDescriptorLimit));
#if PLATFORM_MAC
        execve(macSandboxExecutableAnsi.Get(), macSandboxArguments.Get(), environment.Get());
#else
        execve(executableAnsi.Get(), arguments.Get(), environment.Get());
#endif
        ReportPosixSetupFailure(3, PosixWorkerSetupFailure::Execute);
    }

    close(nullDescriptor);
    close(setupPipe[1]);
    if (setpgid(process, process) != 0 && errno != EACCES && errno != ESRCH)
    {
        kill(process, SIGKILL);
        while (waitpid(process, nullptr, 0) < 0 && errno == EINTR)
        {
        }
        close(setupPipe[0]);
        cleanupProtocolFiles();
        return ProcessFail(diagnostic, AssetPipelineDiagnosticCode::BuildFailed, request,
            TEXT("Cannot attach isolated importer process-tree controls."));
    }

    PosixWorkerSetupFailure setupFailure = PosixWorkerSetupFailure::None;
    int32 waitStatus = 0;
    struct rusage usage = {};
    bool reaped = false;
    bool cancelled = false;
    bool timedOut = false;
    bool waitFailed = false;
    for (;;)
    {
        int32 setupValue = 0;
        const ssize_t setupRead = read(setupPipe[0], &setupValue, sizeof(setupValue));
        if (setupRead == sizeof(setupValue))
        {
            setupFailure = static_cast<PosixWorkerSetupFailure>(setupValue);
            break;
        }
        if (setupRead < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)
        {
            waitFailed = true;
            break;
        }

        const pid_t wait = wait4(process, &waitStatus, WNOHANG, &usage);
        if (wait == process)
        {
            reaped = true;
            break;
        }
        if (wait < 0 && errno != EINTR)
        {
            waitFailed = true;
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
        Platform::Sleep(10);
    }
    close(setupPipe[0]);
    if (!reaped)
    {
        kill(-process, SIGKILL);
        kill(process, SIGKILL);
        while (wait4(process, &waitStatus, 0, &usage) < 0 && errno == EINTR)
        {
        }
    }

    if (cancelled)
    {
        cleanupProtocolFiles();
        result.Status = AssetImportWorkerStatus::Cancelled;
        return ProcessFail(diagnostic, AssetPipelineDiagnosticCode::BuildCancelled, request,
            TEXT("Isolated importer process tree was cancelled."));
    }
    if (timedOut)
    {
        cleanupProtocolFiles();
        result.Status = AssetImportWorkerStatus::TimedOut;
        return ProcessFail(diagnostic, AssetPipelineDiagnosticCode::ResourceLimitExceeded, request,
            TEXT("Isolated importer process tree exceeded its timeout."));
    }
    if (setupFailure != PosixWorkerSetupFailure::None)
    {
        cleanupProtocolFiles();
        result.Status = AssetImportWorkerStatus::Rejected;
        const AssetPipelineDiagnosticCode code = setupFailure == PosixWorkerSetupFailure::ResourceLimits
            ? AssetPipelineDiagnosticCode::ResourceLimitExceeded
            : AssetPipelineDiagnosticCode::BuildFailed;
        return ProcessFail(diagnostic, code, request, GetPosixSetupFailureMessage(setupFailure));
    }
    if (waitFailed)
    {
        cleanupProtocolFiles();
        result.Status = AssetImportWorkerStatus::Crashed;
        return ProcessFail(diagnostic, AssetPipelineDiagnosticCode::BuildFailed, request,
            TEXT("Cannot observe isolated importer process completion."));
    }
    if (WIFSIGNALED(waitStatus))
    {
        cleanupProtocolFiles();
        result.Status = AssetImportWorkerStatus::Crashed;
        const int32 signal = WTERMSIG(waitStatus);
        const AssetPipelineDiagnosticCode code = signal == SIGXCPU || signal == SIGXFSZ
            ? AssetPipelineDiagnosticCode::ResourceLimitExceeded
            : AssetPipelineDiagnosticCode::BuildFailed;
        return ProcessFail(diagnostic, code, request,
            String::Format(TEXT("Isolated importer process terminated by signal {0}."), signal));
    }
    if (!WIFEXITED(waitStatus) || WEXITSTATUS(waitStatus) != 0)
    {
        cleanupProtocolFiles();
        result.Status = AssetImportWorkerStatus::Crashed;
        const int32 exitCode = WIFEXITED(waitStatus) ? WEXITSTATUS(waitStatus) : -1;
        return ProcessFail(diagnostic, AssetPipelineDiagnosticCode::BuildFailed, request,
            String::Format(TEXT("Isolated importer process exited with code {0}."), exitCode));
    }
    if (AssetImportWorkerProtocol::LoadResult(resultPath, result, diagnostic))
    {
        cleanupProtocolFiles();
        return true;
    }
#if PLATFORM_MAC
    const uint64 peakMemory = usage.ru_maxrss > 0 ? static_cast<uint64>(usage.ru_maxrss) : 0;
#else
    const uint64 peakMemory = usage.ru_maxrss > 0 && static_cast<uint64>(usage.ru_maxrss) > MAX_uint64 / 1024
        ? MAX_uint64
        : (usage.ru_maxrss > 0 ? static_cast<uint64>(usage.ru_maxrss) * 1024 : 0);
#endif
    if (result.PeakMemory < peakMemory)
        result.PeakMemory = peakMemory;
#else
    cleanupProtocolFiles();
    return ProcessFail(diagnostic, AssetPipelineDiagnosticCode::BuildFailed, request,
        TEXT("Strict isolated importer process execution is not implemented on this platform."));
#endif

    cleanupProtocolFiles();
    return AssetImportWorkerProtocol::ValidateResult(request, result, diagnostic);
}
