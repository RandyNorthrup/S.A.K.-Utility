@echo off
setlocal
powershell.exe -NoProfile -ExecutionPolicy Bypass -Command "Start-Process -FilePath powershell.exe -Verb RunAs -ArgumentList '-NoProfile -NoExit -ExecutionPolicy Bypass -File ""\\vboxsvr\sakrepo\scripts\run_partition_manager_hdd_defrag_external_gate.ps1"" -TargetDiskNumber 1 -Force'"
