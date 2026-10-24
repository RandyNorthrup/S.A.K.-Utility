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

    The token comes from the SAFER "Normal User" level (the level behind runas /trustlevel:0x20000):
    BUILTIN\Administrators becomes deny-only and the administrator privileges are removed. Its
    integrity level is then lowered to Medium. Before launching anything the wrapper checks the
    token itself and refuses to run if it still reports elevation or still carries an enabled
    Administrators group, so a failed de-elevation cannot pass as an unelevated run.

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
    Add-Type -TypeDefinition @'
using System;
using System.ComponentModel;
using System.Runtime.InteropServices;
using System.Text;

public static class SakUnelevated {
    const uint SaferScopeIdUser = 2;
    const uint SaferLevelIdNormalUser = 0x20000;
    const uint SaferLevelOpen = 1;
    const int TokenElevationClass = 20;
    const int TokenIntegrityLevelClass = 25;
    const uint SeGroupIntegrity = 0x20;
    const int StartfUseStdHandles = 0x100;
    const uint HandleFlagInherit = 1;
    const uint Infinite = 0xFFFFFFFF;
    const uint TokenQuery = 0x0008;
    const uint TokenDuplicate = 0x0002;
    const int SecurityImpersonation = 2;

    [StructLayout(LayoutKind.Sequential)]
    struct SidAndAttributes { public IntPtr Sid; public uint Attributes; }

    [StructLayout(LayoutKind.Sequential)]
    struct TokenMandatoryLabel { public SidAndAttributes Label; }

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
    static extern bool SaferCreateLevel(uint scope, uint level, uint flags, out IntPtr handle, IntPtr reserved);
    [DllImport("advapi32.dll", SetLastError = true)]
    static extern bool SaferComputeTokenFromLevel(IntPtr level, IntPtr inToken, out IntPtr outToken, uint flags, IntPtr reserved);
    [DllImport("advapi32.dll", SetLastError = true)]
    static extern bool SaferCloseLevel(IntPtr level);
    [DllImport("advapi32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
    static extern bool ConvertStringSidToSid(string sid, out IntPtr psid);
    [DllImport("advapi32.dll", SetLastError = true)]
    static extern int GetLengthSid(IntPtr psid);
    [DllImport("advapi32.dll", SetLastError = true)]
    static extern bool SetTokenInformation(IntPtr token, int infoClass, ref TokenMandatoryLabel info, int length);
    [DllImport("advapi32.dll", SetLastError = true)]
    static extern bool GetTokenInformation(IntPtr token, int infoClass, out int info, int length, out int returned);
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

    static IntPtr InheritableStd(int which) {
        IntPtr handle = GetStdHandle(which);
        if (handle != IntPtr.Zero && handle != new IntPtr(-1)) {
            SetHandleInformation(handle, HandleFlagInherit, HandleFlagInherit);
        }
        return handle;
    }

    static IntPtr StandardUserToken() {
        IntPtr level;
        if (!SaferCreateLevel(SaferScopeIdUser, SaferLevelIdNormalUser, SaferLevelOpen, out level, IntPtr.Zero))
            throw new Win32Exception(Marshal.GetLastWin32Error(), "SaferCreateLevel");
        IntPtr token;
        try {
            if (!SaferComputeTokenFromLevel(level, IntPtr.Zero, out token, 0, IntPtr.Zero))
                throw new Win32Exception(Marshal.GetLastWin32Error(), "SaferComputeTokenFromLevel");
        } finally {
            SaferCloseLevel(level);
        }
        IntPtr medium;
        if (!ConvertStringSidToSid("S-1-16-8192", out medium))
            throw new Win32Exception(Marshal.GetLastWin32Error(), "ConvertStringSidToSid");
        try {
            var label = new TokenMandatoryLabel();
            label.Label.Sid = medium;
            label.Label.Attributes = SeGroupIntegrity;
            int size = Marshal.SizeOf(typeof(TokenMandatoryLabel)) + GetLengthSid(medium);
            if (!SetTokenInformation(token, TokenIntegrityLevelClass, ref label, size))
                throw new Win32Exception(Marshal.GetLastWin32Error(), "SetTokenInformation(IntegrityLevel)");
        } finally {
            LocalFree(medium);
        }
        return token;
    }

    // Fail closed: the token must not report elevation and must not carry an enabled
    // BUILTIN\Administrators group, or the "unelevated" run would be a relabelled admin run.
    static void RequireUnelevated(IntPtr token) {
        int elevated, returned;
        if (!GetTokenInformation(token, TokenElevationClass, out elevated, 4, out returned))
            throw new Win32Exception(Marshal.GetLastWin32Error(), "GetTokenInformation(TokenElevation)");
        if (elevated != 0) throw new InvalidOperationException("The standard-user token still reports elevation.");
        IntPtr impersonation;
        if (!DuplicateToken(token, SecurityImpersonation, out impersonation))
            throw new Win32Exception(Marshal.GetLastWin32Error(), "DuplicateToken");
        IntPtr admins;
        if (!ConvertStringSidToSid("S-1-5-32-544", out admins))
            throw new Win32Exception(Marshal.GetLastWin32Error(), "ConvertStringSidToSid");
        try {
            bool member;
            if (!CheckTokenMembership(impersonation, admins, out member))
                throw new Win32Exception(Marshal.GetLastWin32Error(), "CheckTokenMembership");
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
                    true, 0, IntPtr.Zero, workingDirectory, ref startup, out info))
                throw new Win32Exception(Marshal.GetLastWin32Error(), "CreateProcessAsUser");
            try {
                WaitForSingleObject(info.hProcess, Infinite);
                uint exitCode;
                if (!GetExitCodeProcess(info.hProcess, out exitCode))
                    throw new Win32Exception(Marshal.GetLastWin32Error(), "GetExitCodeProcess");
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
'@
}

Write-Host "Running unelevated (SAFER Normal User, Medium integrity): $commandLine"
$exitCode = [SakUnelevated]::Run($commandLine, $WorkingDirectory)
exit $exitCode
