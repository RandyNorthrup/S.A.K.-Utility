<#
.SYNOPSIS
    Runs a command under a standard-user token derived from the caller's elevated token.

.DESCRIPTION
    S.A.K. Utility runs as a standard user and elevates per task, and the test suite is written
    for that: an unelevated host is what makes a plain PowerShell child "not elevated", what makes
    the current user (not BUILTIN\Administrators) the owner of a new file, and what keeps the AI
    tool policy on its read-only allowlist. A GitHub-hosted Windows runner executes every step
    as an elevated administrator, so the same assertions fail there for a reason that has
    nothing to do with the code. This wrapper gives the command the token a standard user gets.

    The token is filtered the way UAC filters an administrator's token for a standard-user
    session: CreateRestrictedToken with LUA_TOKEN, BUILTIN\Administrators and "Local account and
    member of Administrators group" deny-only, every privilege a standard user lacks deleted,
    Medium integrity, and the user as the default owner of new objects. (The SAFER "Normal User"
    level, runas /trustlevel:0x20000, was tried first; under it, on a hosted runner,
    test_process_runner's PowerShell child printed nothing inside its 5-second window and
    test_drive_scanner found no drives, neither of which happens elevated.) Before
    launching anything the wrapper checks the token itself and refuses to run if it is still an
    enabled member of Administrators (the membership the product's own elevation check reads), so
    a failed de-elevation cannot pass as an unelevated run.

    The child inherits this process's environment, working directory and standard handles, and
    its exit code is returned as this script's exit code.

.PARAMETER FilePath
    The executable to run.

.PARAMETER ArgumentList
    Its arguments, passed with the Windows command-line quoting rules.

.PARAMETER WorkingDirectory
    The child's working directory. Defaults to the current PowerShell location.

.EXAMPLE
    pwsh scripts/invoke_unelevated.ps1 -FilePath ctest -- --test-dir build -C Release -L product
#>
[CmdletBinding(PositionalBinding = $false)]
param(
    [Parameter(Mandatory = $true)][string]$FilePath,
    [Parameter(ValueFromRemainingArguments = $true)][string[]]$ArgumentList = @(),
    [string]$WorkingDirectory = ""
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

if ([string]::IsNullOrWhiteSpace($WorkingDirectory)) {
    $WorkingDirectory = (Get-Location).ProviderPath
}
$command = Get-Command $FilePath -CommandType Application -ErrorAction Stop | Select-Object -First 1
$exe = $command.Source

# CommandLineToArgvW quoting: wrap in quotes when needed, double the backslashes that precede a
# quote or the closing quote, and escape embedded quotes.
function ConvertTo-WindowsArgument([string]$Value) {
    if ($Value.Length -gt 0 -and $Value -notmatch '[\s"]') { return $Value }
    $builder = [System.Text.StringBuilder]::new('"')
    $slashes = 0
    foreach ($ch in $Value.ToCharArray()) {
        if ($ch -eq '\') { $slashes++; continue }
        if ($ch -eq '"') {
            [void]$builder.Append('\', $slashes * 2 + 1)
        } elseif ($slashes -gt 0) {
            [void]$builder.Append('\', $slashes)
        }
        $slashes = 0
        [void]$builder.Append($ch)
    }
    [void]$builder.Append('\', $slashes * 2)
    [void]$builder.Append('"')
    return $builder.ToString()
}

$commandLine = (@($exe) + @($ArgumentList) | ForEach-Object { ConvertTo-WindowsArgument $_ }) -join ' '

if (-not ('SakUnelevated' -as [type])) {
    Add-Type -TypeDefinition @"
using System;
using System.Collections.Generic;
using System.ComponentModel;
using System.Runtime.InteropServices;
using System.Text;

public static class SakUnelevated {
    const uint TokenAllAccess = 0xF01FF;
    const uint LuaToken = 0x4;
    const int TokenPrivilegesClass = 3;
    const int TokenOwnerClass = 4;
    const int TokenElevationClass = 20;
    const int TokenIntegrityLevelClass = 25;
    const int TokenUserClass = 1;
    const uint SeGroupIntegrity = 0x20;
    const int StartfUseStdHandles = 0x100;
    const uint HandleFlagInherit = 1;
    const uint Infinite = 0xFFFFFFFF;
    const int SecurityImpersonation = 2;

    // The privileges Windows leaves in a standard user's (UAC-filtered) token.
    static readonly HashSet<string> StandardPrivileges = new HashSet<string>(StringComparer.OrdinalIgnoreCase) {
        "SeChangeNotifyPrivilege", "SeShutdownPrivilege", "SeUndockPrivilege",
        "SeIncreaseWorkingSetPrivilege", "SeTimeZonePrivilege" };

    // BUILTIN\Administrators and "Local account and member of Administrators group", the two
    // groups UAC makes deny-only.
    static readonly string[] AdminSids = { "S-1-5-32-544", "S-1-5-114" };

    [StructLayout(LayoutKind.Sequential)]
    struct SidAndAttributes { public IntPtr Sid; public uint Attributes; }

    [StructLayout(LayoutKind.Sequential)]
    struct Luid { public uint Low; public int High; }

    [StructLayout(LayoutKind.Sequential)]
    struct LuidAndAttributes { public Luid Luid; public uint Attributes; }

    [StructLayout(LayoutKind.Sequential)]
    struct TokenMandatoryLabel { public SidAndAttributes Label; }

    [StructLayout(LayoutKind.Sequential)]
    struct TokenOwner { public IntPtr Owner; }

    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
    struct StartupInfo {
        public int cb; public string lpReserved; public string lpDesktop; public string lpTitle;
        public int dwX, dwY, dwXSize, dwYSize, dwXCountChars, dwYCountChars, dwFillAttribute;
        public int dwFlags; public short wShowWindow, cbReserved2; public IntPtr lpReserved2;
        public IntPtr hStdInput, hStdOutput, hStdError;
    }

    [StructLayout(LayoutKind.Sequential)]
    struct ProcessInformation { public IntPtr hProcess, hThread; public int dwProcessId, dwThreadId; }

    [DllImport("advapi32.dll", SetLastError = true)]
    static extern bool OpenProcessToken(IntPtr process, uint access, out IntPtr token);
    [DllImport("kernel32.dll")]
    static extern IntPtr GetCurrentProcess();
    [DllImport("advapi32.dll", SetLastError = true)]
    static extern bool CreateRestrictedToken(IntPtr existing, uint flags, uint disableCount, SidAndAttributes[] sidsToDisable,
        uint deleteCount, LuidAndAttributes[] privilegesToDelete, uint restrictCount, IntPtr sidsToRestrict, out IntPtr newToken);
    [DllImport("advapi32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
    static extern bool ConvertStringSidToSid(string sid, out IntPtr psid);
    [DllImport("advapi32.dll", SetLastError = true)]
    static extern int GetLengthSid(IntPtr psid);
    [DllImport("advapi32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
    static extern bool LookupPrivilegeName(string system, ref Luid luid, StringBuilder name, ref int length);
    [DllImport("advapi32.dll", SetLastError = true)]
    static extern bool SetTokenInformation(IntPtr token, int infoClass, ref TokenMandatoryLabel info, int length);
    [DllImport("advapi32.dll", SetLastError = true, EntryPoint = "SetTokenInformation")]
    static extern bool SetTokenOwner(IntPtr token, int infoClass, ref TokenOwner info, int length);
    [DllImport("advapi32.dll", SetLastError = true)]
    static extern bool GetTokenInformation(IntPtr token, int infoClass, IntPtr info, int length, out int returned);
    [DllImport("advapi32.dll", SetLastError = true)]
    static extern bool DuplicateToken(IntPtr token, int level, out IntPtr duplicate);
    [DllImport("advapi32.dll", SetLastError = true)]
    static extern bool CheckTokenMembership(IntPtr token, IntPtr sid, out bool isMember);
    [DllImport("advapi32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
    static extern bool CreateProcessAsUser(IntPtr token, string application, StringBuilder commandLine,
        IntPtr processAttributes, IntPtr threadAttributes, bool inheritHandles, uint creationFlags,
        IntPtr environment, string currentDirectory, ref StartupInfo startupInfo, out ProcessInformation info);
    [DllImport("kernel32.dll", SetLastError = true)]
    static extern IntPtr GetStdHandle(int which);
    [DllImport("kernel32.dll", SetLastError = true)]
    static extern bool SetHandleInformation(IntPtr handle, uint mask, uint flags);
    [DllImport("kernel32.dll", SetLastError = true)]
    static extern uint WaitForSingleObject(IntPtr handle, uint milliseconds);
    [DllImport("kernel32.dll", SetLastError = true)]
    static extern bool GetExitCodeProcess(IntPtr process, out uint exitCode);
    [DllImport("kernel32.dll", SetLastError = true)]
    static extern bool CloseHandle(IntPtr handle);
    [DllImport("kernel32.dll")]
    static extern IntPtr LocalFree(IntPtr memory);

    public static int TokenElevation;
    public static string RemovedPrivileges = "";

    static Exception Fail(string api) { return new Win32Exception(Marshal.GetLastWin32Error(), api); }

    static IntPtr InheritableStd(int which) {
        IntPtr handle = GetStdHandle(which);
        if (handle != IntPtr.Zero && handle != new IntPtr(-1)) {
            SetHandleInformation(handle, HandleFlagInherit, HandleFlagInherit);
        }
        return handle;
    }

    static IntPtr ReadTokenInformation(IntPtr token, int infoClass) {
        int needed;
        GetTokenInformation(token, infoClass, IntPtr.Zero, 0, out needed);
        IntPtr buffer = Marshal.AllocHGlobal(Math.Max(needed, 4));
        if (!GetTokenInformation(token, infoClass, buffer, needed, out needed)) {
            Marshal.FreeHGlobal(buffer);
            throw Fail("GetTokenInformation(" + infoClass + ")");
        }
        return buffer;
    }

    static LuidAndAttributes[] AdministratorPrivileges(IntPtr token, List<string> removed) {
        IntPtr buffer = ReadTokenInformation(token, TokenPrivilegesClass);
        try {
            int count = Marshal.ReadInt32(buffer);
            int size = Marshal.SizeOf(typeof(LuidAndAttributes));
            var delete = new List<LuidAndAttributes>();
            for (int i = 0; i < count; i++) {
                var entry = (LuidAndAttributes)Marshal.PtrToStructure(buffer + 4 + i * size, typeof(LuidAndAttributes));
                var name = new StringBuilder(64);
                int length = name.Capacity;
                Luid luid = entry.Luid;
                if (!LookupPrivilegeName(null, ref luid, name, ref length)) throw Fail("LookupPrivilegeName");
                if (!StandardPrivileges.Contains(name.ToString())) { delete.Add(entry); removed.Add(name.ToString()); }
            }
            return delete.ToArray();
        } finally {
            Marshal.FreeHGlobal(buffer);
        }
    }

    // A filtered token built the way UAC builds a standard user's: LUA_TOKEN, the administrator
    // groups deny-only, every privilege a standard user lacks deleted, Medium integrity, and the
    // user (not Administrators) as the default owner of new objects.
    static IntPtr StandardUserToken() {
        IntPtr process;
        if (!OpenProcessToken(GetCurrentProcess(), TokenAllAccess, out process)) throw Fail("OpenProcessToken");
        var sids = new List<SidAndAttributes>();
        IntPtr medium = IntPtr.Zero;
        IntPtr userBuffer = IntPtr.Zero;
        try {
            foreach (string text in AdminSids) {
                IntPtr sid;
                if (!ConvertStringSidToSid(text, out sid)) throw Fail("ConvertStringSidToSid");
                sids.Add(new SidAndAttributes { Sid = sid, Attributes = 0 });
            }
            var removed = new List<string>();
            LuidAndAttributes[] privileges = AdministratorPrivileges(process, removed);
            RemovedPrivileges = string.Join(",", removed);
            IntPtr token;
            if (!CreateRestrictedToken(process, LuaToken, (uint)sids.Count, sids.ToArray(),
                    (uint)privileges.Length, privileges, 0, IntPtr.Zero, out token)) throw Fail("CreateRestrictedToken");

            if (!ConvertStringSidToSid("S-1-16-8192", out medium)) throw Fail("ConvertStringSidToSid");
            var label = new TokenMandatoryLabel();
            label.Label.Sid = medium;
            label.Label.Attributes = SeGroupIntegrity;
            if (!SetTokenInformation(token, TokenIntegrityLevelClass, ref label,
                    Marshal.SizeOf(typeof(TokenMandatoryLabel)) + GetLengthSid(medium))) throw Fail("SetTokenInformation(IntegrityLevel)");

            userBuffer = ReadTokenInformation(token, TokenUserClass);
            var owner = new TokenOwner { Owner = Marshal.ReadIntPtr(userBuffer) };
            if (!SetTokenOwner(token, TokenOwnerClass, ref owner, Marshal.SizeOf(typeof(TokenOwner)))) throw Fail("SetTokenInformation(Owner)");
            return token;
        } finally {
            foreach (var sid in sids) LocalFree(sid.Sid);
            if (medium != IntPtr.Zero) LocalFree(medium);
            if (userBuffer != IntPtr.Zero) Marshal.FreeHGlobal(userBuffer);
            CloseHandle(process);
        }
    }

    // Fail closed: the token must not carry an enabled BUILTIN\Administrators group, which is
    // exactly what the product's own elevation test (ElevationManager::isElevated,
    // CheckTokenMembership) reads. TokenElevation is reported, not required: with UAC disabled,
    // as on a hosted runner, Windows can still report a filtered admin token as elevated.
    static void RequireUnelevated(IntPtr token) {
        IntPtr elevation = ReadTokenInformation(token, TokenElevationClass);
        TokenElevation = Marshal.ReadInt32(elevation);
        Marshal.FreeHGlobal(elevation);
        IntPtr impersonation;
        if (!DuplicateToken(token, SecurityImpersonation, out impersonation)) throw Fail("DuplicateToken");
        IntPtr admins;
        if (!ConvertStringSidToSid("S-1-5-32-544", out admins)) throw Fail("ConvertStringSidToSid");
        try {
            bool member;
            if (!CheckTokenMembership(impersonation, admins, out member)) throw Fail("CheckTokenMembership");
            if (member) throw new InvalidOperationException("The standard-user token is still an enabled member of Administrators.");
        } finally {
            LocalFree(admins);
            CloseHandle(impersonation);
        }
    }

    public static int Run(string commandLine, string workingDirectory) {
        IntPtr token = StandardUserToken();
        try {
            RequireUnelevated(token);
            var startup = new StartupInfo();
            startup.cb = Marshal.SizeOf(typeof(StartupInfo));
            startup.dwFlags = StartfUseStdHandles;
            startup.hStdInput = InheritableStd(-10);
            startup.hStdOutput = InheritableStd(-11);
            startup.hStdError = InheritableStd(-12);
            ProcessInformation info;
            if (!CreateProcessAsUser(token, null, new StringBuilder(commandLine), IntPtr.Zero, IntPtr.Zero,
                    true, 0, IntPtr.Zero, workingDirectory, ref startup, out info)) throw Fail("CreateProcessAsUser");
            try {
                WaitForSingleObject(info.hProcess, Infinite);
                uint exitCode;
                if (!GetExitCodeProcess(info.hProcess, out exitCode)) throw Fail("GetExitCodeProcess");
                return unchecked((int)exitCode);
            } finally {
                CloseHandle(info.hThread);
                CloseHandle(info.hProcess);
            }
        } finally {
            CloseHandle(token);
        }
    }
}
"@
}

Write-Host "Running unelevated (UAC-style filtered token, Medium integrity): $commandLine"
$exitCode = [SakUnelevated]::Run($commandLine, $WorkingDirectory)
Write-Host "Unelevated command exited $exitCode (Administrators deny-only; TokenElevation=$([SakUnelevated]::TokenElevation); removed privileges: $([SakUnelevated]::RemovedPrivileges))"
exit $exitCode
