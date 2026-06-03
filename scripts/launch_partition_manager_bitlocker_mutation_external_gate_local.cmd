@echo off
setlocal
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "\\vboxsvr\sakrepo\scripts\launch_partition_manager_bitlocker_mutation_external_gate_local.ps1"
if errorlevel 1 pause
