@echo off
set LOG=\\vboxsvr\sakrepo\artifacts\partition-manager-certification\vm-lab\launch-vm-data-gates-nonusb.log
echo Starting VM data gate launcher at %DATE% %TIME% > "%LOG%"
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "\\vboxsvr\sakrepo\scripts\launch_partition_manager_vm_data_external_gates_local.ps1" -RotationalDiskNumber 1 -GateIdsCsv "external.ssd-retrim,external.ssd-secure-erase,external.partition-move,external.primary-logical-conversion,external.volume-serial-number,external.dynamic-to-basic,external.hardware-wipe" >> "%LOG%" 2>>&1
echo Exit code %ERRORLEVEL% at %DATE% %TIME% >> "%LOG%"
exit /b %ERRORLEVEL%
