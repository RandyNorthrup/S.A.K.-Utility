# Windows System Maintenance Commands Reference

**EXACT Commands and PowerShell Cmdlets for Windows 10/11**

This document provides accurate, tested commands for common system maintenance tasks with correct syntax, required permissions, and expected behavior.

---

## 1. Disk Cleanup

### Command Line Usage
```powershell
# Method 1: Run with pre-configured settings
cleanmgr.exe /sagerun:n

# Method 2: Configure cleanup options first, then run
cleanmgr.exe /sageset:1
cleanmgr.exe /sagerun:1

# Method 3: Target specific drive
cleanmgr.exe /d C:
```

### Configuration Process
```powershell
# Step 1: Configure which items to clean (run once)
cleanmgr.exe /sageset:1
# This opens a GUI where you select cleanup options
# Settings are saved to registry with ID "1"

# Step 2: Run cleanup with saved settings
cleanmgr.exe /sagerun:1
```

### Registry Location
- Settings stored in: `HKEY_LOCAL_MACHINE\SOFTWARE\Microsoft\Windows\CurrentVersion\Explorer\VolumeCaches\`
- Each cleanup option gets a `StateFlags0001` value (0=unchecked, 2=checked)

**Required Admin Privileges:** Yes  
**Service Dependencies:** None  
**Expected Output:** GUI shows progress, returns 0 on success  
**Common Errors:**
- Error if cleanmgr.exe not found (Desktop Experience not installed on Server)
- May require reboot for certain cleanup operations

---

## 2. Clear Browser Cache

### Chrome Cache Locations
```powershell
# Chrome Cache
$ChromeCache = "$env:LOCALAPPDATA\Google\Chrome\User Data\Default\Cache"
$ChromeCache2 = "$env:LOCALAPPDATA\Google\Chrome\User Data\Default\Code Cache"

# Delete Chrome cache (close Chrome first)
Remove-Item -Path $ChromeCache -Recurse -Force -ErrorAction SilentlyContinue
Remove-Item -Path $ChromeCache2 -Recurse -Force -ErrorAction SilentlyContinue
```

### Firefox Cache Locations
```powershell
# Firefox Cache (profile folder varies)
$FirefoxCache = "$env:LOCALAPPDATA\Mozilla\Firefox\Profiles\*.default-release\cache2"

# Delete Firefox cache
Get-ChildItem -Path "$env:LOCALAPPDATA\Mozilla\Firefox\Profiles\" -Filter "cache2" -Recurse | 
    Remove-Item -Recurse -Force -ErrorAction SilentlyContinue
```

### Microsoft Edge (Chromium) Cache Locations
```powershell
# Edge Cache
$EdgeCache = "$env:LOCALAPPDATA\Microsoft\Edge\User Data\Default\Cache"
$EdgeCache2 = "$env:LOCALAPPDATA\Microsoft\Edge\User Data\Default\Code Cache"

# Delete Edge cache
Remove-Item -Path $EdgeCache -Recurse -Force -ErrorAction SilentlyContinue
Remove-Item -Path $EdgeCache2 -Recurse -Force -ErrorAction SilentlyContinue
```

**Required Admin Privileges:** No (for user profile), Yes (for all users)  
**Service Dependencies:** Browser must be closed  
**Expected Output:** Folders deleted, may return access denied if browser is running  
**Common Errors:**
- ERROR_SHARING_VIOLATION if browser is open
- Access denied if clearing other user profiles

---

## 3. Windows Update (PowerShell Module)

### Install PSWindowsUpdate Module
```powershell
# Install module (run once, requires admin)
Install-Module -Name PSWindowsUpdate -Force

# Import module
Import-Module PSWindowsUpdate
```

### Check for Updates
```powershell
# Get available updates
Get-WindowsUpdate

# Get updates with details
Get-WindowsUpdate -Verbose
```

### Install Updates
```powershell
# Install all updates with auto-accept and auto-reboot
Install-WindowsUpdate -AcceptAll -AutoReboot

# Install updates without auto-reboot
Install-WindowsUpdate -AcceptAll

# Install specific update by KB number
Install-WindowsUpdate -KBArticleID KB5001234 -AcceptAll
```

### Alternative: Using Windows Update COM API
```powershell
# Check for updates using COM API (no module required)
$UpdateSession = New-Object -ComObject Microsoft.Update.Session
$UpdateSearcher = $UpdateSession.CreateUpdateSearcher()
$SearchResult = $UpdateSearcher.Search("IsInstalled=0 and Type='Software'")

# Display available updates
$SearchResult.Updates | Select-Object Title, IsDownloaded, IsInstalled
```

**Required Admin Privileges:** Yes  
**Service Dependencies:** wuauserv (Windows Update service)  
**Expected Output:** List of updates, installation progress, exit code 0 on success  
**Common Errors:**
- 0x80070002: Files not found
- 0x8024402C: Network connectivity issues
- 0x80244007: Update source not available

---

## 4. Clear Windows Update Cache

### Complete Procedure
```powershell
# Stop Windows Update services
Stop-Service -Name wuauserv -Force
Stop-Service -Name bits -Force
Stop-Service -Name cryptsvc -Force

# Rename SoftwareDistribution folder
Rename-Item -Path "C:\Windows\SoftwareDistribution" -NewName "SoftwareDistribution.old" -Force

# Restart services
Start-Service -Name cryptsvc
Start-Service -Name bits
Start-Service -Name wuauserv
```

### Alternative: Delete and Recreate
```powershell
# Stop services
net stop wuauserv
net stop bits
net stop cryptsvc
net stop msiserver

# Delete cache
Remove-Item -Path "C:\Windows\SoftwareDistribution" -Recurse -Force

# Start services (folder recreated automatically)
net start cryptsvc
net start bits
net start wuauserv
net start msiserver
```

**Required Admin Privileges:** Yes  
**Service Dependencies:** wuauserv, bits, cryptsvc must be stopped first  
**Expected Output:** Services stop/start successfully, folder renamed/deleted  
**Common Errors:**
- Access denied if services still running
- ERROR_SERVICE_DEPENDENCY if dependent services not stopped

---

## 5. Defragment Drives

### Using PowerShell Optimize-Volume
```powershell
# Analyze drive fragmentation
Optimize-Volume -DriveLetter C -Analyze -Verbose

# Defragment a drive (HDD)
Optimize-Volume -DriveLetter C -Defrag -Verbose

# TRIM an SSD
Optimize-Volume -DriveLetter C -ReTrim -Verbose

# Optimize all drives
Get-Volume | Where-Object {$_.DriveType -eq 'Fixed'} | Optimize-Volume -Verbose
```

### Using defrag.exe (Legacy)
```powershell
# Analyze drive C:
defrag C: /A

# Defragment drive C:
defrag C: /O

# Defragment all drives
defrag /C /O

# Verbose output
defrag C: /O /V
```

### Automatic Behavior by Drive Type
- **HDD/Fixed VHD:** Analyze + Defrag
- **SSD with TRIM:** ReTrim
- **Tiered Storage:** TierOptimize
- **Thin Provisioned:** Analyze + SlabConsolidate + ReTrim

**Required Admin Privileges:** Yes  
**Service Dependencies:** defragsvc (Optimize Drives service)  
**Expected Output:** Progress percentage, completion status, exit code 0  
**Common Errors:**
- Drive in use (close programs)
- SSD doesn't need defragmentation (use ReTrim)
- Volume not supported (e.g., network drives)

---

## 6. Disable Startup Programs

### Registry Keys for Startup Programs
```powershell
# Current User
$RegRunCU = "HKCU:\Software\Microsoft\Windows\CurrentVersion\Run"

# Local Machine (All Users)
$RegRunLM = "HKLM:\Software\Microsoft\Windows\CurrentVersion\Run"

# 64-bit on 64-bit Windows
$RegRunWow64 = "HKLM:\SOFTWARE\WOW6432Node\Microsoft\Windows\CurrentVersion\Run"

# List startup programs
Get-ItemProperty -Path $RegRunCU
Get-ItemProperty -Path $RegRunLM

# Remove a startup program
Remove-ItemProperty -Path $RegRunCU -Name "ProgramName"
```

### Using Task Scheduler
```powershell
# List scheduled tasks that run at startup
Get-ScheduledTask | Where-Object {$_.Triggers.CimClass.CimClassName -like "*Boot*" -or 
                                   $_.Triggers.CimClass.CimClassName -like "*Logon*"}

# Disable a scheduled task
Disable-ScheduledTask -TaskName "TaskName" -TaskPath "\Microsoft\Windows\"

# Enable a scheduled task
Enable-ScheduledTask -TaskName "TaskName"
```

### Startup Folder Locations
```powershell
# Current User startup folder
$StartupCU = "$env:APPDATA\Microsoft\Windows\Start Menu\Programs\Startup"

# All Users startup folder
$StartupAU = "$env:ProgramData\Microsoft\Windows\Start Menu\Programs\Startup"

# List startup items
Get-ChildItem -Path $StartupCU
Get-ChildItem -Path $StartupAU
```

**Required Admin Privileges:** No (Current User), Yes (All Users/HKLM)  
**Service Dependencies:** Task Scheduler (for scheduled tasks)  
**Expected Output:** Registry value removed, task disabled  
**Common Errors:**
- Access denied when modifying HKLM without admin rights
- Task not found if path incorrect

---

## 7. Check Disk Health

### Using PowerShell Get-PhysicalDisk
```powershell
# Get all physical disks with health status
Get-PhysicalDisk | Select-Object FriendlyName, SerialNumber, HealthStatus, OperationalStatus

# Get detailed disk information
Get-PhysicalDisk | Format-List *

# Filter unhealthy disks
Get-PhysicalDisk | Where-Object {$_.HealthStatus -ne "Healthy"}

# Get SMART data (requires admin)
Get-PhysicalDisk | Get-StorageReliabilityCounter | Select-Object DeviceId, Temperature, Wear
```

### Using WMIC (Legacy)
```powershell
# Get disk drive information
wmic diskdrive get status, model, serialnumber, size

# Get SMART status
wmic diskdrive get status

# Detailed disk properties
wmic diskdrive list full
```

### Check Disk Errors with CHKDSK
```powershell
# Check disk for errors (read-only scan)
chkdsk C:

# Scan and fix errors (requires restart if system drive)
chkdsk C: /F

# Scan for bad sectors (takes longer)
chkdsk C: /R

# Schedule chkdsk on next reboot
chkdsk C: /F
# Type 'Y' when prompted
```

**Required Admin Privileges:** Yes  
**Service Dependencies:** None  
**Expected Output:** 
- HealthStatus: Healthy, Warning, Unhealthy, Unknown
- OperationalStatus: OK, Error, Degraded, Unknown
- WMIC returns "OK" or "Error"

**Common Errors:**
- Access denied without admin rights
- "Cannot lock drive" if volume in use

---

## 8. Verify System Files

### System File Checker (SFC)
```powershell
# Scan and repair system files
sfc /scannow

# Scan but don't repair
sfc /verifyonly

# Scan specific file
sfc /scanfile=C:\Windows\System32\kernel32.dll

# Offline repair (WinRE)
sfc /scannow /offbootdir=C:\ /offwindir=C:\Windows
```

### DISM (Deployment Image Servicing)
```powershell
# Check image health
DISM /Online /Cleanup-Image /CheckHealth

# Scan image for corruption
DISM /Online /Cleanup-Image /ScanHealth

# Repair image using Windows Update
DISM /Online /Cleanup-Image /RestoreHealth

# Repair using local source
DISM /Online /Cleanup-Image /RestoreHealth /Source:C:\RepairSource\Windows /LimitAccess
```

### Complete Repair Sequence
```powershell
# 1. Run DISM first to repair component store
DISM /Online /Cleanup-Image /RestoreHealth

# 2. Then run SFC to repair system files
sfc /scannow

# 3. Restart computer
Restart-Computer
```

**Required Admin Privileges:** Yes  
**Service Dependencies:** TrustedInstaller (Windows Modules Installer)  
**Expected Output:**
- SFC: "Windows Resource Protection found corrupt files and repaired them"
- DISM: "The restore operation completed successfully"
- Logs: C:\Windows\Logs\CBS\CBS.log

**Common Errors:**
- "Windows Resource Protection could not perform requested operation" (run DISM first)
- 0x800f081f: Source files not found (use /Source parameter)

---

## 9. Reset Network

### Complete Network Reset Sequence
```powershell
# 1. Release and renew IP address
ipconfig /release
ipconfig /renew

# 2. Flush DNS cache
ipconfig /flushdns

# 3. Reset Winsock catalog
netsh winsock reset

# 4. Reset TCP/IP stack
netsh int ip reset

# 5. Reset Windows Firewall
netsh advfirewall reset

# 6. Reset proxy settings
netsh winhttp reset proxy

# 7. Restart computer (required)
Restart-Computer
```

### Individual Components

#### Reset TCP/IP
```powershell
# Reset TCP/IP stack with log file
netsh int ip reset c:\resetlog.txt

# Or without specifying log location
netsh int ip reset resetlog.txt
```

#### Reset Winsock
```powershell
# Reset Winsock catalog
netsh winsock reset

# Reset Winsock catalog with log
netsh winsock reset catalog
```

#### Reset Network Adapters
```powershell
# Disable and re-enable network adapter
$adapter = Get-NetAdapter | Where-Object {$_.Status -eq "Up"}
Disable-NetAdapter -Name $adapter.Name -Confirm:$false
Start-Sleep -Seconds 5
Enable-NetAdapter -Name $adapter.Name -Confirm:$false
```

#### Reset DNS
```powershell
# Clear DNS client cache
Clear-DnsClientCache

# Or using ipconfig
ipconfig /flushdns

# Register DNS
ipconfig /registerdns
```

**Required Admin Privileges:** Yes  
**Service Dependencies:** DHCP Client, DNS Client  
**Expected Output:** "Successfully reset...", exit code 0  
**Common Errors:**
- Access denied without admin rights
- Network temporarily unavailable during reset
- Must restart for changes to take effect

---

## 10. Fix Audio Issues

### Restart Audio Services
```powershell
# Windows Audio service
Restart-Service -Name Audiosrv -Force

# Windows Audio Endpoint Builder
Restart-Service -Name AudioEndpointBuilder -Force

# Restart both services (correct order)
Stop-Service -Name Audiosrv -Force
Stop-Service -Name AudioEndpointBuilder -Force
Start-Service -Name AudioEndpointBuilder
Start-Service -Name Audiosrv
```

### Check Audio Service Status
```powershell
# Get audio service status
Get-Service -Name Audiosrv, AudioEndpointBuilder | 
    Select-Object Name, Status, StartType

# Ensure services are set to automatic
Set-Service -Name Audiosrv -StartupType Automatic
Set-Service -Name AudioEndpointBuilder -StartupType Automatic
```

### Reinstall Audio Drivers
```powershell
# List audio devices
Get-PnpDevice -Class "AudioEndpoint" | 
    Select-Object FriendlyName, Status, InstanceId

# Disable and re-enable audio device
$audioDevice = Get-PnpDevice -FriendlyName "*High Definition Audio*"
Disable-PnpDevice -InstanceId $audioDevice.InstanceId -Confirm:$false
Enable-PnpDevice -InstanceId $audioDevice.InstanceId -Confirm:$false
```

### Reset Audio to Default
```powershell
# Stop audio services
Stop-Service Audiosrv -Force
Stop-Service AudioEndpointBuilder -Force

# Delete audio settings cache (requires admin)
Remove-Item -Path "HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\MMDevices" -Recurse -Force

# Restart services (settings recreated automatically)
Start-Service AudioEndpointBuilder
Start-Service Audiosrv
```

**Required Admin Privileges:** Yes  
**Service Dependencies:** 
- AudioEndpointBuilder (must start first)
- Audiosrv (depends on AudioEndpointBuilder)
- RpcSs (Remote Procedure Call)

**Expected Output:** Services restart successfully, audio resumes  
**Common Errors:**
- Error 1068: Dependency service failed to start
- Audio device not found (driver issue)
- Access denied when modifying registry

---

## Summary Table

| Task | Command | Admin Required | Reboot Required |
|------|---------|----------------|-----------------|
| Disk Cleanup | `cleanmgr /sagerun:1` | Yes | Sometimes |
| Clear Browser Cache | `Remove-Item $env:LOCALAPPDATA\...` | No | No |
| Windows Update | `Install-WindowsUpdate -AcceptAll` | Yes | Sometimes |
| Clear Update Cache | `Stop-Service wuauserv; Remove-Item...` | Yes | No |
| Defragment | `Optimize-Volume -DriveLetter C -Defrag` | Yes | No |
| Disable Startup | `Remove-ItemProperty -Path HKCU:\...` | Depends | No |
| Check Disk Health | `Get-PhysicalDisk` | Yes | No |
| Verify System Files | `DISM /RestoreHealth; sfc /scannow` | Yes | Recommended |
| Reset Network | `netsh int ip reset; netsh winsock reset` | Yes | Yes |
| Fix Audio | `Restart-Service Audiosrv` | Yes | No |

---

## Return Codes

### Common Success Codes
- `0` - Success
- Exit code 0 or $LASTEXITCODE = 0

### Common Error Codes
- `1` - Incorrect function / General error
- `5` - Access denied (run as admin)
- `87` - Invalid parameter
- `126` - Module not found
- `1060` - Service does not exist
- `1068` - Dependency service failed to start
- `0x80070002` - File not found
- `0x80070005` - Access denied
- `0x800F0922` - DISM: The request is not supported

---

## Best Practices

1. **Always run as Administrator** - Most maintenance tasks require elevated privileges
2. **Create restore point** - Before making system changes
3. **Check service dependencies** - Stop/start dependent services in correct order
4. **Log output** - Redirect output to log files for troubleshooting
5. **Test in safe mode** - If issues persist
6. **Close applications** - Before disk operations
7. **Backup data** - Before major repairs
8. **Check disk space** - Ensure sufficient free space (10-15%)

---

## Error Handling Template

```powershell
try {
    # Run maintenance command
    $result = Optimize-Volume -DriveLetter C -Defrag -ErrorAction Stop
    Write-Host "Success: Operation completed" -ForegroundColor Green
}
catch {
    Write-Host "Error: $($_.Exception.Message)" -ForegroundColor Red
    Write-Host "Exit Code: $LASTEXITCODE"
    
    # Check if admin rights needed
    if ($LASTEXITCODE -eq 5) {
        Write-Host "Run as Administrator required" -ForegroundColor Yellow
    }
}
```

---

## References

- [Microsoft Docs - Disk Cleanup](https://learn.microsoft.com/en-us/troubleshoot/windows-server/backup-and-storage/automating-disk-cleanup-tool)
- [Microsoft Docs - Optimize-Volume](https://learn.microsoft.com/en-us/powershell/module/storage/optimize-volume)
- [Microsoft Docs - Get-PhysicalDisk](https://learn.microsoft.com/en-us/powershell/module/storage/get-physicaldisk)
- [Microsoft Docs - DISM](https://learn.microsoft.com/en-us/windows-hardware/manufacture/desktop/repair-a-windows-image)
- [Microsoft Docs - SFC](https://support.microsoft.com/help/929833)
- [Microsoft Docs - NetSh](https://learn.microsoft.com/en-us/windows-server/administration/windows-commands/netsh)

---

**Document Version:** 1.0  
**Last Updated:** December 15, 2025  
**Validated For:** Windows 10/11, Windows Server 2016/2019/2022
