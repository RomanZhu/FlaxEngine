// Copyright (c) Wojciech Figat. All rights reserved.

using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Linq;
using System.Runtime.InteropServices;
using System.Text;

namespace FlaxEditor
{
    /// <summary>OS process boundary used for managed importer workers.</summary>
    internal sealed class ScriptedImporterWorkerSandbox : IDisposable
    {
        private readonly Process _process;
        private string _appContainerProfile;
        private IntPtr _appContainerSid;
        private List<string> _grantedPaths;
        private IntPtr _job;

        private ScriptedImporterWorkerSandbox(Process process, IntPtr job, string appContainerProfile = null,
            IntPtr appContainerSid = default, List<string> grantedPaths = null)
        {
            _process = process;
            _job = job;
            _appContainerProfile = appContainerProfile;
            _appContainerSid = appContainerSid;
            _grantedPaths = grantedPaths;
        }

        internal int ExitCode => _process.ExitCode;

        internal bool WaitForExit(int milliseconds)
        {
            return _process.WaitForExit(milliseconds);
        }

        internal void WaitForExit()
        {
            _process.WaitForExit();
        }

        internal void Kill()
        {
            if (_job != IntPtr.Zero && OperatingSystem.IsWindows())
                NativeMethods.TerminateJobObject(_job, unchecked((uint)-1));
            else if (!_process.HasExited)
                _process.Kill(true);
        }

        internal static ScriptedImporterWorkerSandbox Start(ProcessStartInfo start, string writableRoot, IEnumerable<string> readablePaths)
        {
            if (!OperatingSystem.IsWindows())
            {
                var process = Process.Start(start) ?? throw new InvalidOperationException("Failed to start the scripted importer worker process.");
                return new ScriptedImporterWorkerSandbox(process, IntPtr.Zero);
            }
            return StartWindows(start, writableRoot, readablePaths);
        }

        private static InvalidOperationException Win32Failure(string message, int error = -1)
        {
            if (error < 0)
                error = Marshal.GetLastWin32Error();
            return new InvalidOperationException($"{message} Win32 error: {error}.");
        }

        private static ScriptedImporterWorkerSandbox StartWindows(ProcessStartInfo start, string writableRoot, IEnumerable<string> readablePaths)
        {
            SetLowIntegrityDirectory(writableRoot);
            var profileName = "Flax.ScriptedImporter." + Guid.NewGuid().ToString("N");
            var createProfileResult = NativeMethods.CreateAppContainerProfile(profileName, profileName,
                "Ephemeral Flax scripted importer worker", IntPtr.Zero, 0, out var appContainerSid);
            if (createProfileResult < 0 || appContainerSid == IntPtr.Zero)
                throw Win32Failure("Cannot create the scripted importer AppContainer profile.", createProfileResult);
            var grantedPaths = new List<string>();
            try
            {
                GrantPathAccess(writableRoot, appContainerSid, true, grantedPaths);
                var executableDirectory = Path.GetDirectoryName(Path.GetFullPath(start.FileName));
                if (!string.IsNullOrEmpty(executableDirectory))
                    GrantPathAccess(executableDirectory, appContainerSid, false, grantedPaths);
                foreach (var path in readablePaths ?? Array.Empty<string>())
                {
                    if (!string.IsNullOrWhiteSpace(path) && (File.Exists(path) || Directory.Exists(path)))
                        GrantPathAccess(path, appContainerSid, false, grantedPaths);
                }
            }
            catch
            {
                CleanupAppContainer(profileName, appContainerSid, grantedPaths);
                throw;
            }
            var job = NativeMethods.CreateJobObject(IntPtr.Zero, null);
            if (job == IntPtr.Zero)
            {
                CleanupAppContainer(profileName, appContainerSid, grantedPaths);
                throw Win32Failure("Cannot create the scripted importer worker Job Object.");
            }

            var processHandle = IntPtr.Zero;
            var threadHandle = IntPtr.Zero;
            var attributeList = IntPtr.Zero;
            var securityCapabilitiesBuffer = IntPtr.Zero;
            try
            {
                var limits = new NativeMethods.JobObjectExtendedLimitInformation
                {
                    BasicLimitInformation = new NativeMethods.JobObjectBasicLimitInformation
                    {
                        LimitFlags = NativeMethods.JobObjectLimitKillOnJobClose | NativeMethods.JobObjectLimitActiveProcess,
                        ActiveProcessLimit = 1,
                    },
                };
                if (!NativeMethods.SetInformationJobObject(job, NativeMethods.JobObjectInfoType.ExtendedLimitInformation,
                    ref limits, (uint)Marshal.SizeOf<NativeMethods.JobObjectExtendedLimitInformation>()))
                    throw Win32Failure("Cannot configure the scripted importer worker Job Object.");

                var uiLimits = new NativeMethods.JobObjectBasicUiRestrictions
                {
                    UIRestrictionsClass = NativeMethods.JobObjectUiLimitHandles |
                                          NativeMethods.JobObjectUiLimitReadClipboard |
                                          NativeMethods.JobObjectUiLimitWriteClipboard |
                                          NativeMethods.JobObjectUiLimitSystemParameters |
                                          NativeMethods.JobObjectUiLimitDisplaySettings |
                                          NativeMethods.JobObjectUiLimitExitWindows,
                };
                if (!NativeMethods.SetInformationJobObject(job, NativeMethods.JobObjectInfoType.BasicUiRestrictions,
                    ref uiLimits, (uint)Marshal.SizeOf<NativeMethods.JobObjectBasicUiRestrictions>()))
                    throw Win32Failure("Cannot configure scripted importer worker UI restrictions.");

                var commandLine = new StringBuilder(BuildCommandLine(start));
                var startupInfo = new NativeMethods.StartupInfoEx
                {
                    StartupInfo = new NativeMethods.StartupInfo
                    {
                        cb = (uint)Marshal.SizeOf<NativeMethods.StartupInfoEx>(),
                    },
                };
                UIntPtr attributeListSize = UIntPtr.Zero;
                NativeMethods.InitializeProcThreadAttributeList(IntPtr.Zero, 1, 0, ref attributeListSize);
                attributeList = Marshal.AllocHGlobal(checked((int)attributeListSize.ToUInt64()));
                if (!NativeMethods.InitializeProcThreadAttributeList(attributeList, 1, 0, ref attributeListSize))
                    throw Win32Failure("Cannot initialize AppContainer process attributes.");
                startupInfo.AttributeList = attributeList;
                var securityCapabilities = new NativeMethods.SecurityCapabilities
                {
                    AppContainerSid = appContainerSid,
                    Capabilities = IntPtr.Zero,
                    CapabilityCount = 0,
                    Reserved = 0,
                };
                securityCapabilitiesBuffer = Marshal.AllocHGlobal(Marshal.SizeOf<NativeMethods.SecurityCapabilities>());
                Marshal.StructureToPtr(securityCapabilities, securityCapabilitiesBuffer, false);
                if (!NativeMethods.UpdateProcThreadAttribute(attributeList, 0,
                    (IntPtr)NativeMethods.ProcThreadAttributeSecurityCapabilities, securityCapabilitiesBuffer,
                    (IntPtr)Marshal.SizeOf<NativeMethods.SecurityCapabilities>(), IntPtr.Zero, IntPtr.Zero))
                    throw Win32Failure("Cannot bind AppContainer security capabilities to the worker process.");
                var environment = BuildEnvironmentBlock(start);
                if (!NativeMethods.CreateProcessAppContainer(null, commandLine, IntPtr.Zero, IntPtr.Zero, false,
                    NativeMethods.CreateSuspended | NativeMethods.CreateNoWindow | NativeMethods.CreateUnicodeEnvironment |
                    NativeMethods.ExtendedStartupInfoPresent,
                    environment, start.WorkingDirectory, ref startupInfo, out var processInfo))
                    throw Win32Failure("Cannot create the AppContainer scripted importer worker process.");
                processHandle = processInfo.hProcess;
                threadHandle = processInfo.hThread;

                if (!NativeMethods.AssignProcessToJobObject(job, processHandle))
                    throw Win32Failure("Cannot assign the scripted importer worker to its Job Object.");
                if (NativeMethods.ResumeThread(threadHandle) == uint.MaxValue)
                    throw Win32Failure("Cannot resume the sandboxed scripted importer worker.");

                var process = Process.GetProcessById((int)processInfo.dwProcessId);
                NativeMethods.CloseHandle(threadHandle);
                threadHandle = IntPtr.Zero;
                NativeMethods.CloseHandle(processHandle);
                processHandle = IntPtr.Zero;
                NativeMethods.DeleteProcThreadAttributeList(attributeList);
                Marshal.FreeHGlobal(attributeList);
                attributeList = IntPtr.Zero;
                Marshal.FreeHGlobal(securityCapabilitiesBuffer);
                securityCapabilitiesBuffer = IntPtr.Zero;
                return new ScriptedImporterWorkerSandbox(process, job, profileName, appContainerSid, grantedPaths);
            }
            catch
            {
                if (processHandle != IntPtr.Zero)
                    NativeMethods.TerminateProcess(processHandle, unchecked((uint)-1));
                if (threadHandle != IntPtr.Zero)
                    NativeMethods.CloseHandle(threadHandle);
                if (processHandle != IntPtr.Zero)
                    NativeMethods.CloseHandle(processHandle);
                if (attributeList != IntPtr.Zero)
                {
                    NativeMethods.DeleteProcThreadAttributeList(attributeList);
                    Marshal.FreeHGlobal(attributeList);
                }
                if (securityCapabilitiesBuffer != IntPtr.Zero)
                    Marshal.FreeHGlobal(securityCapabilitiesBuffer);
                NativeMethods.CloseHandle(job);
                CleanupAppContainer(profileName, appContainerSid, grantedPaths);
                throw;
            }
        }

        private static void GrantPathAccess(string path, IntPtr sid, bool writable, List<string> grantedPaths)
        {
            var fullPath = Path.GetFullPath(path).TrimEnd(Path.DirectorySeparatorChar, Path.AltDirectorySeparatorChar);
            if (grantedPaths.Contains(fullPath, StringComparer.OrdinalIgnoreCase))
                return;
            ChangePathAccess(fullPath, sid, writable ? NativeMethods.GenericAll : NativeMethods.GenericRead | NativeMethods.GenericExecute,
                NativeMethods.AccessMode.SetAccess);
            grantedPaths.Add(fullPath);
        }

        private static void RevokePathAccess(string path, IntPtr sid)
        {
            ChangePathAccess(path, sid, 0, NativeMethods.AccessMode.RevokeAccess);
        }

        private static void ChangePathAccess(string path, IntPtr sid, uint permissions, NativeMethods.AccessMode mode)
        {
            var result = NativeMethods.GetNamedSecurityInfo(path, NativeMethods.SeObjectType.FileObject,
                NativeMethods.DaclSecurityInformation, out _, out _, out var oldAcl, out _, out var securityDescriptor);
            if (result != 0)
                throw Win32Failure($"Cannot read filesystem ACL for AppContainer capability path '{path}'.", (int)result);
            IntPtr newAcl = IntPtr.Zero;
            try
            {
                var access = new NativeMethods.ExplicitAccess
                {
                    AccessPermissions = permissions,
                    AccessMode = mode,
                    Inheritance = Directory.Exists(path) ? NativeMethods.SubContainersAndObjectsInherit : 0,
                    Trustee = new NativeMethods.Trustee
                    {
                        TrusteeForm = NativeMethods.TrusteeForm.IsSid,
                        TrusteeType = NativeMethods.TrusteeType.IsUser,
                        Name = sid,
                    },
                };
                result = NativeMethods.SetEntriesInAcl(1, ref access, oldAcl, out newAcl);
                if (result != 0)
                    throw Win32Failure($"Cannot create filesystem ACL for AppContainer capability path '{path}'.", (int)result);
                result = NativeMethods.SetNamedSecurityInfo(path, NativeMethods.SeObjectType.FileObject,
                    NativeMethods.DaclSecurityInformation, IntPtr.Zero, IntPtr.Zero, newAcl, IntPtr.Zero);
                if (result != 0)
                    throw Win32Failure($"Cannot publish filesystem ACL for AppContainer capability path '{path}'.", (int)result);
            }
            finally
            {
                if (newAcl != IntPtr.Zero)
                    NativeMethods.LocalFree(newAcl);
                if (securityDescriptor != IntPtr.Zero)
                    NativeMethods.LocalFree(securityDescriptor);
            }
        }

        private static void CleanupAppContainer(string profileName, IntPtr sid, List<string> grantedPaths)
        {
            if (sid != IntPtr.Zero)
            {
                for (var i = (grantedPaths?.Count ?? 0) - 1; i >= 0; i--)
                {
                    try
                    {
                        if (File.Exists(grantedPaths[i]) || Directory.Exists(grantedPaths[i]))
                            RevokePathAccess(grantedPaths[i], sid);
                    }
                    catch
                    {
                    }
                }
            }
            if (!string.IsNullOrEmpty(profileName))
                NativeMethods.DeleteAppContainerProfile(profileName);
            if (sid != IntPtr.Zero)
                NativeMethods.FreeSid(sid);
        }

        private static void SetLowIntegrityDirectory(string path)
        {
            if (!NativeMethods.ConvertStringSecurityDescriptorToSecurityDescriptor("S:(ML;OICI;NW;;;LW)", 1,
                out var descriptor, out _))
                throw Win32Failure("Cannot create the worker directory integrity descriptor.");
            try
            {
                if (!NativeMethods.GetSecurityDescriptorSacl(descriptor, out var present, out var sacl, out _) || !present)
                    throw Win32Failure("Cannot read the worker directory integrity descriptor.");
                var result = NativeMethods.SetNamedSecurityInfo(path, NativeMethods.SeObjectType.FileObject,
                    NativeMethods.LabelSecurityInformation, IntPtr.Zero, IntPtr.Zero, IntPtr.Zero, sacl);
                if (result != 0)
                    throw Win32Failure("Cannot mark the worker result directory as low integrity.", (int)result);
            }
            finally
            {
                NativeMethods.LocalFree(descriptor);
            }
        }

        private static void SetLowIntegrityProcess(IntPtr process)
        {
            if (!NativeMethods.OpenProcessToken(process, NativeMethods.TokenQuery | NativeMethods.TokenAdjustDefault, out var token))
                throw Win32Failure("Cannot open the worker process token.");
            try
            {
                if (!NativeMethods.ConvertStringSidToSid("S-1-16-4096", out var sid))
                    throw Win32Failure("Cannot create the low-integrity SID.");
                try
                {
                    var label = new NativeMethods.TokenMandatoryLabel
                    {
                        Label = new NativeMethods.SidAndAttributes
                        {
                            Sid = sid,
                            Attributes = NativeMethods.SeGroupIntegrity,
                        },
                    };
                    var size = Marshal.SizeOf<NativeMethods.TokenMandatoryLabel>() + (int)NativeMethods.GetLengthSid(sid);
                    var buffer = Marshal.AllocHGlobal(size);
                    try
                    {
                        Marshal.StructureToPtr(label, buffer, false);
                        if (!NativeMethods.SetTokenInformation(token, NativeMethods.TokenInformationClass.IntegrityLevel, buffer, (uint)size))
                            throw Win32Failure("Cannot lower the worker process integrity level.");
                    }
                    finally
                    {
                        Marshal.FreeHGlobal(buffer);
                    }
                }
                finally
                {
                    NativeMethods.LocalFree(sid);
                }
            }
            finally
            {
                NativeMethods.CloseHandle(token);
            }
        }

        private static string BuildCommandLine(ProcessStartInfo start)
        {
            var values = new List<string> { start.FileName };
            values.AddRange(start.ArgumentList);
            return string.Join(" ", values.Select(QuoteArgument));
        }

        private static string QuoteArgument(string value)
        {
            if (value.Length != 0 && value.All(x => !char.IsWhiteSpace(x) && x != '"'))
                return value;
            var result = new StringBuilder("\"");
            var slashes = 0;
            foreach (var character in value)
            {
                if (character == '\\')
                {
                    slashes++;
                    continue;
                }
                if (character == '"')
                {
                    result.Append('\\', slashes * 2 + 1);
                    result.Append('"');
                    slashes = 0;
                    continue;
                }
                result.Append('\\', slashes);
                slashes = 0;
                result.Append(character);
            }
            result.Append('\\', slashes * 2);
            result.Append('"');
            return result.ToString();
        }

        private static string BuildEnvironmentBlock(ProcessStartInfo start)
        {
            return string.Join('\0', start.Environment.OrderBy(x => x.Key, StringComparer.OrdinalIgnoreCase)
                       .Select(x => x.Key + "=" + x.Value)) + "\0\0";
        }

        public void Dispose()
        {
            _process.Dispose();
            if (_job != IntPtr.Zero)
            {
                NativeMethods.CloseHandle(_job);
                _job = IntPtr.Zero;
            }
            if (_appContainerSid != IntPtr.Zero || !string.IsNullOrEmpty(_appContainerProfile))
            {
                var profile = _appContainerProfile;
                var sid = _appContainerSid;
                var paths = _grantedPaths;
                _appContainerProfile = null;
                _appContainerSid = IntPtr.Zero;
                _grantedPaths = null;
                CleanupAppContainer(profile, sid, paths);
            }
        }

        private static class NativeMethods
        {
            internal const uint CreateSuspended = 0x00000004;
            internal const uint CreateNoWindow = 0x08000000;
            internal const uint CreateUnicodeEnvironment = 0x00000400;
            internal const uint ExtendedStartupInfoPresent = 0x00080000;
            internal const long ProcThreadAttributeSecurityCapabilities = 0x00020005;
            internal const uint JobObjectLimitActiveProcess = 0x00000008;
            internal const uint JobObjectLimitKillOnJobClose = 0x00002000;
            internal const uint JobObjectUiLimitHandles = 0x00000001;
            internal const uint JobObjectUiLimitReadClipboard = 0x00000002;
            internal const uint JobObjectUiLimitWriteClipboard = 0x00000004;
            internal const uint JobObjectUiLimitSystemParameters = 0x00000008;
            internal const uint JobObjectUiLimitDisplaySettings = 0x00000010;
            internal const uint JobObjectUiLimitExitWindows = 0x00000080;
            internal const uint TokenAssignPrimary = 0x0001;
            internal const uint TokenQuery = 0x0008;
            internal const uint TokenAdjustDefault = 0x0080;
            internal const uint SeGroupIntegrity = 0x00000020;
            internal const uint LabelSecurityInformation = 0x00000010;
            internal const uint DaclSecurityInformation = 0x00000004;
            internal const uint GenericRead = 0x80000000;
            internal const uint GenericExecute = 0x20000000;
            internal const uint GenericAll = 0x10000000;
            internal const uint SubContainersAndObjectsInherit = 0x00000003;

            internal enum AccessMode
            {
                SetAccess = 2,
                RevokeAccess = 4,
            }

            internal enum TrusteeForm
            {
                IsSid = 0,
            }

            internal enum TrusteeType
            {
                IsUser = 1,
            }

            internal enum JobObjectInfoType
            {
                BasicUiRestrictions = 4,
                ExtendedLimitInformation = 9,
            }

            internal enum TokenInformationClass
            {
                IntegrityLevel = 25,
            }

            internal enum SeObjectType
            {
                FileObject = 1,
            }

            [StructLayout(LayoutKind.Sequential)]
            internal struct StartupInfo
            {
                internal uint cb;
                internal IntPtr lpReserved;
                internal IntPtr lpDesktop;
                internal IntPtr lpTitle;
                internal uint dwX;
                internal uint dwY;
                internal uint dwXSize;
                internal uint dwYSize;
                internal uint dwXCountChars;
                internal uint dwYCountChars;
                internal uint dwFillAttribute;
                internal uint dwFlags;
                internal ushort wShowWindow;
                internal ushort cbReserved2;
                internal IntPtr lpReserved2;
                internal IntPtr hStdInput;
                internal IntPtr hStdOutput;
                internal IntPtr hStdError;
            }

            [StructLayout(LayoutKind.Sequential)]
            internal struct StartupInfoEx
            {
                internal StartupInfo StartupInfo;
                internal IntPtr AttributeList;
            }

            [StructLayout(LayoutKind.Sequential)]
            internal struct SecurityCapabilities
            {
                internal IntPtr AppContainerSid;
                internal IntPtr Capabilities;
                internal uint CapabilityCount;
                internal uint Reserved;
            }

            [StructLayout(LayoutKind.Sequential)]
            internal struct ProcessInformation
            {
                internal IntPtr hProcess;
                internal IntPtr hThread;
                internal uint dwProcessId;
                internal uint dwThreadId;
            }

            [StructLayout(LayoutKind.Sequential)]
            internal struct JobObjectBasicLimitInformation
            {
                internal long PerProcessUserTimeLimit;
                internal long PerJobUserTimeLimit;
                internal uint LimitFlags;
                internal UIntPtr MinimumWorkingSetSize;
                internal UIntPtr MaximumWorkingSetSize;
                internal uint ActiveProcessLimit;
                internal UIntPtr Affinity;
                internal uint PriorityClass;
                internal uint SchedulingClass;
            }

            [StructLayout(LayoutKind.Sequential)]
            internal struct IoCounters
            {
                internal ulong ReadOperationCount;
                internal ulong WriteOperationCount;
                internal ulong OtherOperationCount;
                internal ulong ReadTransferCount;
                internal ulong WriteTransferCount;
                internal ulong OtherTransferCount;
            }

            [StructLayout(LayoutKind.Sequential)]
            internal struct JobObjectExtendedLimitInformation
            {
                internal JobObjectBasicLimitInformation BasicLimitInformation;
                internal IoCounters IoInfo;
                internal UIntPtr ProcessMemoryLimit;
                internal UIntPtr JobMemoryLimit;
                internal UIntPtr PeakProcessMemoryUsed;
                internal UIntPtr PeakJobMemoryUsed;
            }

            [StructLayout(LayoutKind.Sequential)]
            internal struct JobObjectBasicUiRestrictions
            {
                internal uint UIRestrictionsClass;
            }

            [StructLayout(LayoutKind.Sequential)]
            internal struct SidAndAttributes
            {
                internal IntPtr Sid;
                internal uint Attributes;
            }

            [StructLayout(LayoutKind.Sequential)]
            internal struct TokenMandatoryLabel
            {
                internal SidAndAttributes Label;
            }

            [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
            internal struct Trustee
            {
                internal IntPtr MultipleTrustee;
                internal int MultipleTrusteeOperation;
                internal TrusteeForm TrusteeForm;
                internal TrusteeType TrusteeType;
                internal IntPtr Name;
            }

            [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
            internal struct ExplicitAccess
            {
                internal uint AccessPermissions;
                internal AccessMode AccessMode;
                internal uint Inheritance;
                internal Trustee Trustee;
            }

            [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
            internal static extern bool CreateProcess(string applicationName, StringBuilder commandLine, IntPtr processAttributes,
                IntPtr threadAttributes, bool inheritHandles, uint creationFlags, string environment, string currentDirectory,
                ref StartupInfo startupInfo, out ProcessInformation processInformation);

            [DllImport("kernel32.dll", EntryPoint = "CreateProcessW", CharSet = CharSet.Unicode, SetLastError = true)]
            internal static extern bool CreateProcessAppContainer(string applicationName, StringBuilder commandLine, IntPtr processAttributes,
                IntPtr threadAttributes, bool inheritHandles, uint creationFlags, string environment, string currentDirectory,
                ref StartupInfoEx startupInfo, out ProcessInformation processInformation);

            [DllImport("kernel32.dll", SetLastError = true)]
            internal static extern bool InitializeProcThreadAttributeList(IntPtr attributeList, int attributeCount,
                uint flags, ref UIntPtr size);

            [DllImport("kernel32.dll", SetLastError = true)]
            internal static extern bool UpdateProcThreadAttribute(IntPtr attributeList, uint flags, IntPtr attribute,
                IntPtr value, IntPtr size, IntPtr previousValue, IntPtr returnSize);

            [DllImport("kernel32.dll")]
            internal static extern void DeleteProcThreadAttributeList(IntPtr attributeList);

            [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
            internal static extern IntPtr CreateJobObject(IntPtr attributes, string name);

            [DllImport("kernel32.dll", SetLastError = true)]
            internal static extern bool SetInformationJobObject(IntPtr job, JobObjectInfoType infoType,
                ref JobObjectExtendedLimitInformation info, uint length);

            [DllImport("kernel32.dll", SetLastError = true)]
            internal static extern bool SetInformationJobObject(IntPtr job, JobObjectInfoType infoType,
                ref JobObjectBasicUiRestrictions info, uint length);

            [DllImport("kernel32.dll", SetLastError = true)]
            internal static extern bool AssignProcessToJobObject(IntPtr job, IntPtr process);

            [DllImport("kernel32.dll", SetLastError = true)]
            internal static extern bool TerminateJobObject(IntPtr job, uint exitCode);

            [DllImport("kernel32.dll", SetLastError = true)]
            internal static extern bool TerminateProcess(IntPtr process, uint exitCode);

            [DllImport("kernel32.dll", SetLastError = true)]
            internal static extern uint ResumeThread(IntPtr thread);

            [DllImport("kernel32.dll", SetLastError = true)]
            internal static extern bool CloseHandle(IntPtr handle);

            [DllImport("kernel32.dll", SetLastError = true)]
            internal static extern IntPtr LocalFree(IntPtr memory);

            [DllImport("advapi32.dll", SetLastError = true)]
            internal static extern bool OpenProcessToken(IntPtr process, uint desiredAccess, out IntPtr token);

            [DllImport("advapi32.dll", SetLastError = true)]
            internal static extern bool SetTokenInformation(IntPtr token, TokenInformationClass informationClass,
                IntPtr information, uint informationLength);

            [DllImport("advapi32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
            internal static extern bool ConvertStringSidToSid(string stringSid, out IntPtr sid);

            [DllImport("advapi32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
            internal static extern bool ConvertStringSecurityDescriptorToSecurityDescriptor(string descriptor, uint revision,
                out IntPtr securityDescriptor, out uint securityDescriptorSize);

            [DllImport("advapi32.dll", SetLastError = true)]
            internal static extern bool GetSecurityDescriptorSacl(IntPtr securityDescriptor, out bool saclPresent,
                out IntPtr sacl, out bool saclDefaulted);

            [DllImport("advapi32.dll", CharSet = CharSet.Unicode)]
            internal static extern uint SetNamedSecurityInfo(string objectName, SeObjectType objectType, uint securityInfo,
                IntPtr owner, IntPtr group, IntPtr dacl, IntPtr sacl);

            [DllImport("advapi32.dll", CharSet = CharSet.Unicode)]
            internal static extern uint GetNamedSecurityInfo(string objectName, SeObjectType objectType, uint securityInfo,
                out IntPtr owner, out IntPtr group, out IntPtr dacl, out IntPtr sacl, out IntPtr securityDescriptor);

            [DllImport("advapi32.dll", EntryPoint = "SetEntriesInAclW", CharSet = CharSet.Unicode)]
            internal static extern uint SetEntriesInAcl(uint count, ref ExplicitAccess entries, IntPtr oldAcl, out IntPtr newAcl);

            [DllImport("advapi32.dll")]
            internal static extern uint GetLengthSid(IntPtr sid);

            [DllImport("advapi32.dll")]
            internal static extern IntPtr FreeSid(IntPtr sid);

            [DllImport("userenv.dll", CharSet = CharSet.Unicode)]
            internal static extern int CreateAppContainerProfile(string appContainerName, string displayName, string description,
                IntPtr capabilities, uint capabilityCount, out IntPtr appContainerSid);

            [DllImport("userenv.dll", CharSet = CharSet.Unicode)]
            internal static extern int DeleteAppContainerProfile(string appContainerName);
        }
    }
}
