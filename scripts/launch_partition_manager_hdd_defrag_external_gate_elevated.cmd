@echo off
setlocal

rem This elevates and then runs a DESTRUCTIVE certification gate against a whole physical
rem disk. It used to hardcode "-TargetDiskNumber 1 -Force", so double-clicking it destroyed
rem whatever was disk 1 on that machine with no prompt and no way to say otherwise. The disk
rem is now a required argument.
if "%~1"=="" (
    echo ERROR: a target disk number is required.
    echo.
    echo   usage: %~nx0 ^<disk-number^>
    echo.
    echo Run "Get-Disk" first and confirm the number belongs to the disposable certification
    echo disk. EVERY PARTITION ON THAT DISK WILL BE DESTROYED, and this runs elevated.
    exit /b 2
)

rem Only a plain decimal disk number may reach an elevated command line.
echo %~1| findstr /r "^[0-9][0-9]*$" >nul
if errorlevel 1 (
    echo ERROR: "%~1" is not a disk number.
    exit /b 2
)

rem System32-qualified: an unqualified powershell.exe resolves through the current directory
rem and PATH ahead of the real one, which is exactly the hijack an elevated launch must not
rem allow.
set "PS=%SystemRoot%\System32\WindowsPowerShell\v1.0\powershell.exe"
if not exist "%PS%" (
    echo ERROR: PowerShell was not found at "%PS%".
    exit /b 3
)

"%PS%" -NoProfile -ExecutionPolicy Bypass -Command "Start-Process -FilePath '%PS%' -Verb RunAs -ArgumentList '-NoProfile -NoExit -ExecutionPolicy Bypass -File \"\\vboxsvr\sakrepo\scripts\run_partition_manager_hdd_defrag_external_gate.ps1\" -TargetDiskNumber %~1 -Force'"
exit /b %ERRORLEVEL%
