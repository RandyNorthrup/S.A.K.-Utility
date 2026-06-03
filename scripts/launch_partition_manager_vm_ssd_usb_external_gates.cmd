@echo off
set LOG=\\vboxsvr\sakrepo\artifacts\partition-manager-certification\vm-lab\launch-vm-ssd-usb-gates.log
echo Starting VM SSD/USB gate launcher at %DATE% %TIME% > "%LOG%"
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "\\vboxsvr\sakrepo\scripts\launch_partition_manager_vm_data_external_gates_local.ps1" -RotationalDiskNumber 1 -NvmeDiskNumber 2 -SsdMediaProof "VirtualBox SATA port 2 configured nonrotational=on; host proof artifact artifacts/partition-manager-certification/vm-lab/vbox-showvminfo-ssd-usb.txt" -GateIdsCsv "external.ssd-retrim,external.ssd-secure-erase,external.usb-removable" >> "%LOG%" 2>>&1
echo Exit code %ERRORLEVEL% at %DATE% %TIME% >> "%LOG%"
exit /b %ERRORLEVEL%
