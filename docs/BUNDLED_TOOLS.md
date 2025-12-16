# Bundled Tools and Utilities

## REQUIREMENT: 100% Portable - No Runtime Installation

**All dependencies must be bundled in the repository/build. ZERO downloads or installations at runtime.**

## Strategy

1. **Windows Built-in Tools** - Use when available (cleanmgr, defrag, netsh, etc.)
2. **Bundled Third-Party Tools** - Include portable executables in `tools/` directory
3. **Bundled PowerShell Scripts** - Include our own PS1 scripts in `scripts/` directory
4. **Bundled PowerShell Modules** - Copy module files into `tools/ps_modules/` directory

## Tools to Bundle in Repository

### Portable Executables (Add to tools/ directory)
- **Sysinternals Suite** - Portable system utilities
  - `PsExec.exe` - Run commands with elevated privileges
  - `Autoruns.exe` - Manage startup programs
  - `DiskView.exe` - Disk space visualization
  - License: Free for commercial use, redistributable
  
- **NirSoft Utilities** - Portable system tools
  - `BrowsingHistoryView.exe` - Browser data management
  - `WirelessKeyView.exe` - WiFi password recovery
  - License: Free, redistributable

### PowerShell Modules (Add to tools/ps_modules/)
- **PSWindowsUpdate** - Windows Update management
  - Copy entire module directory
  - Load with `Import-Module` from our bundled path
  - No internet connection needed

### Custom Scripts (Add to scripts/ directory)
- `cleanup_temp.ps1` - Enhanced temp file cleanup
- `browser_cache_clear.ps1` - Multi-browser cache clearing
- `network_reset.ps1` - Complete network stack reset
- `driver_backup.ps1` - Driver backup before updates

## Directory Structure
```
S.A.K.-Utility/
├── tools/
│   ├── sysinternals/
│   │   ├── PsExec.exe
│   │   ├── Autoruns.exe
│   │   └── ...
│   ├── nirsoft/
│   │   └── ...
│   └── ps_modules/
│       └── PSWindowsUpdate/
│           ├── PSWindowsUpdate.psd1
│           ├── PSWindowsUpdate.psm1
│           └── ...
├── scripts/
│   ├── cleanup_temp.ps1
│   ├── browser_cache_clear.ps1
│   └── ...
└── build/
    └── Release/
        ├── sak_utility.exe
        ├── tools/         (copied from above)
        └── scripts/       (copied from above)
```

## CMakeLists.txt Updates Needed

```cmake
# Copy tools directory to build output
install(DIRECTORY ${CMAKE_SOURCE_DIR}/tools
        DESTINATION ${CMAKE_INSTALL_PREFIX}
        COMPONENT tools)

# Copy scripts directory to build output
install(DIRECTORY ${CMAKE_SOURCE_DIR}/scripts
        DESTINATION ${CMAKE_INSTALL_PREFIX}
        COMPONENT scripts)
```

## Action Implementation Strategy

| Action | Implementation | Bundled Tool Needed |
|--------|---------------|---------------------|
| Disk Cleanup | cleanmgr.exe | None (built-in) |
| Clear Browser Cache | Custom PS script | scripts/browser_cache_clear.ps1 |
| Windows Update | PSWindowsUpdate module | tools/ps_modules/PSWindowsUpdate |
| Disable Startup Programs | Autoruns CLI | tools/sysinternals/Autoruns.exe |
| Check Disk Health | Built-in PowerShell | None |
| Verify System Files | sfc.exe + DISM | None (built-in) |
| Reset Network | Custom PS script | scripts/network_reset.ps1 |
| Scan Malware | Windows Defender | None (built-in) |

## Runtime Behavior

1. App launches from `Release/` directory
2. Checks for `tools/` and `scripts/` subdirectories
3. Loads bundled PowerShell modules from `tools/ps_modules/`
4. Executes bundled scripts from `scripts/`
5. Uses bundled .exe files from `tools/`
6. **NO internet access required**
7. **NO installation or downloads**

## License Compliance

All bundled third-party tools must be:
- ✅ Free for commercial use
- ✅ Redistributable
- ✅ Properly attributed in LICENSES.txt

Next steps:
1. Create `tools/` directory
2. Download and add portable utilities
3. Create custom PowerShell scripts
4. Update CMakeLists.txt to copy tools/scripts to build output
5. Update actions to use bundled tools/scripts
