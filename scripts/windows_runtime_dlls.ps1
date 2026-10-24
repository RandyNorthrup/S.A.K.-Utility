# Shared Windows runtime DLL classification and package-local OS-shim refusal.
# The exact names belong to the supported Windows OS, not to an app-local package.

function Test-IsSystemDll {
    param([string]$Name)

    $lower = $Name.ToLowerInvariant()
    if ($lower -like "api-ms-win-*.dll" -or $lower -like "ext-ms-*.dll") {
        return $true
    }

    $systemDlls = @(
        "advapi32.dll",
        # Windows Authz resource-manager API (authz.h / Authz.lib).
        "authz.dll",
        "bcrypt.dll",
        "comdlg32.dll",
        "crypt32.dll",
        # Windows Direct3D runtimes, imported by Qt Gui and the Windows platform plugin.
        # API DLL contracts: Direct3DCreate9, D3D11CreateDevice and D3D12CreateDevice.
        "d3d9.dll",
        "d3d11.dll",
        "d3d12.dll",
        # DbgHelp is a Windows System32 component. CrashReporter links dbghelp.lib for
        # MiniDumpWriteDump; copying a private dbghelp.dll beside the app would replace the
        # serviced OS implementation and is neither required nor desirable.
        "dbghelp.dll",
        "dnsapi.dll",
        "dwmapi.dll",
        # Windows DirectWrite factory API (DWriteCreateFactory / Dwrite.lib).
        "dwrite.dll",
        "dxgi.dll",
        "gdi32.dll",
        # Windows ICU C APIs: icuuc/icuin are OS components since Windows 10 1703;
        # the combined icu.dll is provided by Windows 10 1903 and later.
        # Do not match versioned private ICU libraries such as icuuc78.dll.
        # https://learn.microsoft.com/en-us/windows/win32/intl/international-components-for-unicode--icu-
        "icu.dll",
        "icuin.dll",
        "icuuc.dll",
        # Windows image-helper API (MapFileAndCheckSumW / Imagehlp.lib).
        "imagehlp.dll",
        "imm32.dll",
        "iphlpapi.dll",
        "kernel32.dll",
        # Windows network-provider routing API (winnetwk.h / Mpr.lib).
        "mpr.dll",
        "msimg32.dll",
        # The Windows-protected CRT used by the bundled D3D compiler is not the
        # versioned MSVC redistributable runtime (msvcr100.dll, vcruntime140.dll, etc.).
        "msvcrt.dll",
        # Windows CNG key storage API used by Qt Schannel (NCryptOpenStorageProvider).
        "ncrypt.dll",
        "netapi32.dll",
        # Windows native runtime, also used by Qt's Windows platform implementation.
        "ntdll.dll",
        "ole32.dll",
        "oleacc.dll",
        "oleaut32.dll",
        "powrprof.dll",
        # Windows RPC runtime (RpcStringFreeW / Rpcrt4.lib).
        "rpcrt4.dll",
        "rstrtmgr.dll",
        "secur32.dll",
        "setupapi.dll",
        "shell32.dll",
        "shlwapi.dll",
        # Windows UI Automation provider API (UiaReturnRawElementProvider).
        "uiautomationcore.dll",
        "user32.dll",
        "userenv.dll",
        "uxtheme.dll",
        "version.dll",
        # Windows Virtual Disk API, supplied by the OS rather than the application package.
        # https://learn.microsoft.com/en-us/windows/win32/api/virtdisk/nf-virtdisk-attachvirtualdisk
        "virtdisk.dll",
        "winhttp.dll",
        "winmm.dll",
        # Windows Trust Verification API, System32 only and never redistributable.
        # ChocolateyManager calls WinVerifyTrust to Authenticode-check the installer it
        # is about to run, which is what put this in the import table.
        "wintrust.dll",
        "wlanapi.dll",
        # Windows session-information API (WTSQuerySessionInformationW / Wtsapi32.lib).
        "wtsapi32.dll",
        "ws2_32.dll"
    )

    return $systemDlls -contains $lower
}

function Assert-NoPackageSystemDlls {
    param([Parameter(Mandatory = $true)][string]$Directory)

    # Inspect the app's package-local DLL search directory, including hidden files.
    # This is a refusal, not cleanup: existing build or package files are never deleted.
    $leaked = @(Get-ChildItem -LiteralPath $Directory -Filter "*.dll" -File -Force -ErrorAction Stop |
        Where-Object { Test-IsSystemDll $_.Name })
    if ($leaked.Count -gt 0) {
        $names = ($leaked | ForEach-Object { $_.Name } | Sort-Object) -join ", "
        throw "Package contains app-local Windows system DLL(s): $names. Use target Windows components instead."
    }
}
