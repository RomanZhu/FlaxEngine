// Copyright (c) Wojciech Figat. All rights reserved.

using System;
using System.Diagnostics;
using System.Runtime.CompilerServices;
using System.Security;
using System.Runtime.InteropServices;
using System.Runtime.InteropServices.Marshalling;
using System.Text;
using System.Text.RegularExpressions;

namespace FlaxEngine
{
    internal partial class DebugLogHandler : ILogHandler
    {
        private static LogMessageDelegate _logMessageReceivers;
        private static readonly Regex StackFrameRegex = new Regex("at (.*) in (.*):(line (\\d*)|(\\d*))");

        /// <summary>
        /// Occurs on sending a log message.
        /// </summary>
        public event LogDelegate SendLog;

        /// <summary>
        /// Occurs on sending a log message.
        /// </summary>
        public event LogExceptionDelegate SendExceptionLog;

        /// <inheritdoc />
        public void LogWrite(LogType logType, string message)
        {
            Internal_LogWrite(logType, message);
        }

        /// <inheritdoc />
        public void LogException(Exception exception, Object context)
        {
            if (exception == null)
                return;
            string stackTrace = exception.StackTrace;
            if (string.IsNullOrEmpty(stackTrace))
                stackTrace = GetUserStackTrace(Environment.StackTrace);
            Internal_LogException(exception, Object.GetUnmanagedPtr(context), stackTrace, Platform.CurrentThreadID);

            SendExceptionLog?.Invoke(exception, context);
        }

        /// <inheritdoc />
        public void Log(LogType logType, Object context, string message)
        {
            if (message == null)
                return;
#if BUILD_RELEASE || !FLAX_EDITOR
            string stackTrace = null;
#else
            string stackTrace = Environment.StackTrace;
#endif
            Internal_Log(logType, message, Object.GetUnmanagedPtr(context), GetUserStackTrace(stackTrace), Platform.CurrentThreadID);
            SendLog?.Invoke(logType, message, context, stackTrace);
        }

        internal static void Internal_SendLog(LogType logType, string message, string stackTrace, ulong threadId)
        {
            if (message == null)
                return;
            var logger = Debug.Logger;
            if (logger.IsLogTypeAllowed(logType))
            {
                Internal_Log(logType, message, IntPtr.Zero, GetUserStackTrace(stackTrace), threadId);
                ((DebugLogHandler)logger.LogHandler).SendLog?.Invoke(logType, message, null, stackTrace);
            }
        }

        internal static void Internal_SendLogMessage(LogType logType, string message, string stackTrace, ulong threadId)
        {
            _logMessageReceivers?.Invoke(logType, message, stackTrace, threadId);
        }

        private static string GetUserStackTrace(string stackTrace)
        {
            if (string.IsNullOrEmpty(stackTrace))
                return stackTrace;

            var matches = StackFrameRegex.Matches(stackTrace);
            bool foundStart = false;
            StringBuilder result = null;
            for (int i = 0; i < matches.Count; i++)
            {
                var match = matches[i];
                string location = match.Groups[1].Value.Trim();
                if (location.StartsWith("FlaxEngine.Debug.", StringComparison.Ordinal) ||
                    location.StartsWith("DebugLog::", StringComparison.Ordinal))
                {
                    foundStart = true;
                }
                else if (foundStart)
                {
                    result ??= new StringBuilder(stackTrace.Length);
                    result.AppendLine(match.Groups[0].Value);
                }
            }
            return result?.ToString() ?? string.Empty;
        }

        internal static void Internal_SendLogException(Exception exception)
        {
            Debug.Logger.LogException(exception);
        }

        internal static void AddLogMessageReceiver(LogMessageDelegate receiver)
        {
            if (receiver == null)
                return;
            bool enable = _logMessageReceivers == null;
            _logMessageReceivers += receiver;
            if (enable)
                Internal_SetLogMessageReceiver(true);
        }

        internal static void RemoveLogMessageReceiver(LogMessageDelegate receiver)
        {
            if (receiver == null)
                return;
            _logMessageReceivers -= receiver;
            if (_logMessageReceivers == null)
                Internal_SetLogMessageReceiver(false);
        }

        [LibraryImport("FlaxEngine", EntryPoint = "DebugLogHandlerInternal_LogWrite", StringMarshalling = StringMarshalling.Custom, StringMarshallingCustomType = typeof(Interop.StringMarshaller))]
        internal static partial void Internal_LogWrite(LogType level, string msg);

        [LibraryImport("FlaxEngine", EntryPoint = "DebugLogHandlerInternal_Log", StringMarshalling = StringMarshalling.Custom, StringMarshallingCustomType = typeof(Interop.StringMarshaller))]
        internal static partial void Internal_Log(LogType level, string msg, IntPtr obj, string stackTrace, ulong threadId);

        [LibraryImport("FlaxEngine", EntryPoint = "DebugLogHandlerInternal_SetLogMessageReceiver")]
        internal static partial void Internal_SetLogMessageReceiver([MarshalAs(UnmanagedType.U1)] bool enabled);

        [LibraryImport("FlaxEngine", EntryPoint = "DebugLogHandlerInternal_LogException", StringMarshalling = StringMarshalling.Custom, StringMarshallingCustomType = typeof(Interop.StringMarshaller))]
        internal static partial void Internal_LogException([MarshalUsing(typeof(Interop.ExceptionMarshaller))] Exception exception, IntPtr obj, string fallbackStackTrace, ulong threadId);

        [SecuritySafeCritical]
        public static string Internal_GetStackTrace()
        {
            var stackTrace = new StackTrace(1, true);
            return stackTrace.ToString();
        }
    }
}
