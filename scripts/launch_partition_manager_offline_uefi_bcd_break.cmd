@echo off
set LOG=\\vboxsvr\sakrepo\artifacts\partition-manager-certification\vm-lab\launch-offline-uefi-break.log
echo Starting offline UEFI BCD break at %DATE% %TIME% > "%LOG%"
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "\\vboxsvr\sakrepo\scripts\launch_partition_manager_offline_uefi_bcd_break_local.ps1" >> "%LOG%" 2>>&1
echo Exit code %ERRORLEVEL% at %DATE% %TIME% >> "%LOG%"
exit /b %ERRORLEVEL%
