// Copyright (c) Wojciech Figat. All rights reserved.

#include "DebugLog.h"
#include "Engine/Scripting/Scripting.h"
#include "Engine/Scripting/BinaryModule.h"
#include "Engine/Scripting/ManagedCLR/MCore.h"
#include "Engine/Scripting/ManagedCLR/MDomain.h"
#include "Engine/Scripting/ManagedCLR/MAssembly.h"
#include "Engine/Scripting/ManagedCLR/MClass.h"
#include "Engine/Scripting/Internal/MainThreadManagedInvokeAction.h"
#include "Engine/Threading/Threading.h"
#include "FlaxEngine.Gen.h"

#if USE_CSHARP

namespace Impl
{
    MMethod* Internal_SendLog = nullptr;
    MMethod* Internal_SendLogMessage = nullptr;
    MMethod* Internal_SendLogException = nullptr;
    MMethod* Internal_GetStackTrace = nullptr;
    CriticalSection Locker;
    bool LogMessageReceiverBound = false;
}

using namespace Impl;

void OnLogMessageDetailed(LogType type, const StringView& message, const StringView& stackTrace, uint64 threadId);

void ClearMethods(MAssembly*)
{
    if (LogMessageReceiverBound)
    {
        Log::Logger::OnMessageDetailed.Unbind<OnLogMessageDetailed>();
        LogMessageReceiverBound = false;
    }
    Internal_SendLog = nullptr;
    Internal_SendLogMessage = nullptr;
    Internal_SendLogException = nullptr;
    Internal_GetStackTrace = nullptr;
}

bool CacheMethods()
{
    if (Internal_SendLog && Internal_SendLogMessage && Internal_SendLogException && Internal_GetStackTrace)
        return false;
    ScopeLock lock(Locker);
    if (Internal_SendLog && Internal_SendLogMessage && Internal_SendLogException && Internal_GetStackTrace)
        return false;

    auto engine = ((NativeBinaryModule*)GetBinaryModuleFlaxEngine())->Assembly;
    if (engine == nullptr || !engine->IsLoaded())
        return true;
    auto debugLogHandlerClass = engine->GetClass("FlaxEngine.DebugLogHandler");
    if (!debugLogHandlerClass)
        return false;

    Internal_SendLog = debugLogHandlerClass->GetMethod("Internal_SendLog", 4);
    if (!Internal_SendLog)
        return false;

    Internal_SendLogMessage = debugLogHandlerClass->GetMethod("Internal_SendLogMessage", 4);
    if (!Internal_SendLogMessage)
        return false;

    Internal_SendLogException = debugLogHandlerClass->GetMethod("Internal_SendLogException", 1);
    if (!Internal_SendLogException)
        return false;

    Internal_GetStackTrace = debugLogHandlerClass->GetMethod("Internal_GetStackTrace");
    if (!Internal_GetStackTrace)
        return false;

    engine->Unloading.Bind<ClearMethods>();

    return false;
}

#endif

void DebugLog::Log(LogType type, const StringView& message)
{
#if USE_CSHARP
    if (CacheMethods())
        return;

    const uint64 threadId = Platform::GetCurrentThreadID();
    auto scriptsDomain = Scripting::GetScriptsDomain();
    MainThreadManagedInvokeAction::ParamsBuilder params;
    params.AddParam(type);
    params.AddParam(message, scriptsDomain);
#if BUILD_RELEASE
    params.AddParam(StringView::Empty, scriptsDomain);
#else
    const String stackTrace = Platform::GetStackTrace(1);
    params.AddParam(stackTrace, scriptsDomain);
#endif
    params.AddParam(threadId);
    MainThreadManagedInvokeAction::Invoke(Internal_SendLog, params);
#endif
}

void OnLogMessageDetailed(const LogType type, const StringView& message, const StringView& stackTrace, const uint64 threadId)
{
#if USE_CSHARP
    if (CacheMethods())
        return;

    auto scriptsDomain = Scripting::GetScriptsDomain();
    MainThreadManagedInvokeAction::ParamsBuilder params;
    params.AddParam(type);
    params.AddParam(message, scriptsDomain);
    params.AddParam(stackTrace, scriptsDomain);
    params.AddParam(threadId);
    MainThreadManagedInvokeAction::Invoke(Internal_SendLogMessage, params);
#endif
}

void DebugLog::SetLogMessageReceiver(const bool enabled)
{
#if USE_CSHARP
    if (enabled && CacheMethods())
        return;

    ScopeLock lock(Locker);
    if (enabled == LogMessageReceiverBound)
        return;
    LogMessageReceiverBound = enabled;
    if (enabled)
        Log::Logger::OnMessageDetailed.Bind<OnLogMessageDetailed>();
    else
        Log::Logger::OnMessageDetailed.Unbind<OnLogMessageDetailed>();
#endif
}

void DebugLog::LogException(MObject* exceptionObject)
{
#if USE_CSHARP
    if (exceptionObject == nullptr || CacheMethods())
        return;

    MainThreadManagedInvokeAction::ParamsBuilder params;
    params.AddParam(exceptionObject);
    MainThreadManagedInvokeAction::Invoke(Internal_SendLogException, params);
#endif
}

String DebugLog::GetStackTrace()
{
    String result;
#if USE_CSHARP
    if (!CacheMethods())
    {
        auto stackTraceObj = Internal_GetStackTrace->Invoke(nullptr, nullptr, nullptr);
        MUtils::ToString((MString*)stackTraceObj, result);
    }
#endif
    return result;
}

void DebugLog::ThrowException(const char* msg)
{
#if USE_CSHARP
    auto ex = MCore::Exception::Get(msg);
    MCore::Exception::Throw(ex);
#endif
}

void DebugLog::ThrowNullReference()
{
    //LOG(Warning, "Invalid null reference.");
    //LOG_STR(Warning, DebugLog::GetStackTrace());

#if USE_CSHARP
    auto ex = MCore::Exception::GetNullReference();
    MCore::Exception::Throw(ex);
#endif
}

void DebugLog::ThrowArgument(const char* arg, const char* msg)
{
#if USE_CSHARP
    auto ex = MCore::Exception::GetArgument(arg, msg);
    MCore::Exception::Throw(ex);
#endif
}

void DebugLog::ThrowArgumentNull(const char* arg)
{
#if USE_CSHARP
    auto ex = MCore::Exception::GetArgumentNull(arg);
    MCore::Exception::Throw(ex);
#endif
}

void DebugLog::ThrowArgumentOutOfRange(const char* arg)
{
#if USE_CSHARP
    auto ex = MCore::Exception::GetArgumentOutOfRange(arg);
    MCore::Exception::Throw(ex);
#endif
}

void DebugLog::ThrowNotSupported(const char* msg)
{
#if USE_CSHARP
    auto ex = MCore::Exception::GetNotSupported(msg);
    MCore::Exception::Throw(ex);
#endif
}
