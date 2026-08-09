@echo off
setlocal

rem The gate this launches is DESTRUCTIVE: it runs the file-recovery certification against a
rem whole physical disk and forwards -Force. It used to be invoked with no arguments, which
rem let the PowerShell side fall back to disk 1 -- meaning a double-click destroyed whatever
rem happened to be disk 1 on that machine. The disk is now an explicit, required argument on
rem both sides; this wrapper refuses rather than picking one.
if "%~1"=="" (
    echo ERROR: a target disk number is required.
    echo.
    echo   usage: %~nx0 ^<disk-number^> [additional -Switches...]
    echo.
    echo Run "Get-Disk" in PowerShell first and confirm the number belongs to the disposable
    echo certification disk. Every partition on that disk will be destroyed.
    exit /b 2
)

rem System32-qualified: an unqualified powershell.exe resolves through the current directory
rem and PATH ahead of the real one, and this runs elevated.
set "PS=%SystemRoot%\System32\WindowsPowerShell\v1.0\powershell.exe"
if not exist "%PS%" (
    echo ERROR: PowerShell was not found at "%PS%".
    exit /b 3
)

"%PS%" -NoProfile -ExecutionPolicy Bypass -File "\\vboxsvr\sakrepo\scripts\launch_partition_manager_file_recovery_external_gate_local.ps1" -TargetDiskNumber %1 %2 %3 %4 %5 %6 %7 %8 %9
set "RC=%ERRORLEVEL%"
if not "%RC%"=="0" pause
exit /b %RC%
