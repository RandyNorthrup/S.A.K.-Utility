# SAK Utility Build Guide

## Quick Start

### Prerequisites
- Visual Studio 2022 (v17) with C++ Desktop Development workload
- CMake 3.28 or later
- vcpkg (installed at `C:\vcpkg`)
- Qt 6.5.3 MSVC 2019 64-bit (installed at `C:\Qt\6.5.3\msvc2019_64`)

### Simple Build
```powershell
# Configure
cmake -B build -G "Visual Studio 17 2022" -A x64

# Build (Release)
cmake --build build --config Release --parallel

# Build (Debug)
cmake --build build --config Debug --parallel

# Run
.\build\Release\sak_utility.exe
```

### Verify Build Configuration
```powershell
# Run comprehensive verification
.\scripts\verify_build.ps1

# Perform clean build test
.\scripts\verify_build.ps1 -FullClean

# Skip build, only check files
.\scripts\verify_build.ps1 -SkipBuild
```

---

## Build Configuration

### Persistent Settings (cmake/SAK_BuildConfig.cmake)

All critical build settings are stored in `cmake/SAK_BuildConfig.cmake`:

- **vcpkg Toolchain**: `C:/vcpkg/scripts/buildsystems/vcpkg.cmake`
- **vcpkg Triplet**: `x64-windows-static` (static linking)
- **Qt Path**: `C:/Qt/6.5.3/msvc2019_64`
- **MSVC Runtime**: `MultiThreaded` (static, no DLL dependencies)

**This file survives CMake cache clearing!** No need to re-specify paths after clean builds.

### Static Runtime Linking

The application uses **static MSVC runtime linking** to eliminate DLL dependencies:

- **No MSVCP140.DLL required**
- **No VCRUNTIME140.DLL required**
- **No VCRUNTIME140_1.DLL required**

Configuration:
```cmake
CMAKE_MSVC_RUNTIME_LIBRARY = "MultiThreaded$<$<CONFIG:Debug>:Debug>"
Compiler Flags: /MT (Release) or /MTd (Debug)
```

This ensures the application runs on fresh Windows installations without requiring Visual C++ Redistributables.

---

## Dependencies

### vcpkg Dependencies (Static)

Defined in `vcpkg.json`:

```json
{
  "name": "sak-utility",
  "version": "1.0.0",
  "dependencies": ["zlib", "bzip2", "liblzma"],
  "builtin-baseline": "2b6a882f61eaa764ec3cf816e2265fe804a44cd0"
}
```

**Install static libraries:**
```powershell
cd C:\vcpkg
.\vcpkg install zlib:x64-windows-static
.\vcpkg install bzip2:x64-windows-static
.\vcpkg install liblzma:x64-windows-static
```

### Qt Deployment

Qt DLLs are automatically deployed using `windeployqt` with the following flags:

```cmake
windeployqt --release --no-translations sak_utility.exe
```

- `--release`: Deploy release DLLs (not debug)
- `--no-translations`: Exclude translation files (reduces size)

This happens automatically during the build (POST_BUILD custom command).

---

## Source File Organization

### All 39 Quick Actions

The build system includes all 39 action files organized by category:

#### System Optimization (8 actions)
- `disk_cleanup_action.cpp`
- `clear_browser_cache_action.cpp`
- `defragment_drives_action.cpp`
- `clear_windows_update_cache_action.cpp`
- `disable_startup_programs_action.cpp`
- `clear_event_logs_action.cpp`
- `optimize_power_settings_action.cpp`
- `disable_visual_effects_action.cpp`

#### Quick Backups (8 actions)
- `quickbooks_backup_action.cpp`
- `browser_profile_backup_action.cpp`
- `outlook_backup_action.cpp`
- `sticky_notes_backup_action.cpp`
- `saved_game_data_backup_action.cpp`
- `tax_software_backup_action.cpp`
- `photo_management_backup_action.cpp`
- `development_configs_backup_action.cpp`

#### Maintenance (8 actions)
- `check_disk_health_action.cpp`
- `update_all_apps_action.cpp`
- `windows_update_action.cpp`
- `verify_system_files_action.cpp`
- `check_disk_errors_action.cpp`
- `rebuild_icon_cache_action.cpp`
- `reset_network_action.cpp`
- `clear_print_spooler_action.cpp`

#### Troubleshooting (6 actions)
- `generate_system_report_action.cpp`
- `check_bloatware_action.cpp`
- `test_network_speed_action.cpp`
- `scan_malware_action.cpp`
- `repair_windows_store_action.cpp`
- `fix_audio_issues_action.cpp`

#### Emergency Recovery (8 actions)
- `backup_browser_data_action.cpp`
- `backup_email_data_action.cpp`
- `create_restore_point_action.cpp`
- `export_registry_keys_action.cpp`
- `backup_activation_keys_action.cpp`
- `screenshot_settings_action.cpp`
- `backup_desktop_wallpaper_action.cpp`
- `backup_printer_settings_action.cpp`

#### Action Factory
- `action_factory.cpp` - Central factory for creating action instances

**All 39 actions + factory = 40 files total**

---

## Clean Build Process

### Why Clean Builds Matter

Cleaning the build directory removes the CMake cache, which normally requires re-specifying:
- CMAKE_TOOLCHAIN_FILE (vcpkg path)
- VCPKG_TARGET_TRIPLET (static linkage)
- CMAKE_PREFIX_PATH (Qt location)
- CMAKE_MSVC_RUNTIME_LIBRARY (static runtime)

### How SAK_BuildConfig.cmake Solves This

The `cmake/SAK_BuildConfig.cmake` file is **version-controlled** and contains all these settings.

CMakeLists.txt loads it **before** the project() call:

```cmake
cmake_minimum_required(VERSION 3.28 FATAL_ERROR)

# Load persistent build configuration (vcpkg, Qt, static runtime)
include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/SAK_BuildConfig.cmake)

project(SAK_Utility ...)
```

### Performing a Clean Build

```powershell
# 1. Remove entire build directory
Remove-Item -Path build -Recurse -Force

# 2. Configure (settings auto-loaded from SAK_BuildConfig.cmake)
cmake -B build -G "Visual Studio 17 2022" -A x64

# 3. Build
cmake --build build --config Release --parallel

# 4. Run
.\build\Release\sak_utility.exe
```

**No manual path specification required!**

---

## Troubleshooting

### Issue: "Cannot find MSVCP140.DLL"

**Solution**: This should NOT happen anymore. Verify static runtime:

```powershell
# Check CMAKE_MSVC_RUNTIME_LIBRARY in CMake cache
cmake -B build -LA | Select-String "CMAKE_MSVC_RUNTIME_LIBRARY"

# Expected output: CMAKE_MSVC_RUNTIME_LIBRARY:STRING=MultiThreaded$<$<CONFIG:Debug>:Debug>
```

If you see "DLL" in the runtime library name, reconfigure:

```powershell
Remove-Item -Path build -Recurse -Force
cmake -B build -G "Visual Studio 17 2022" -A x64
```

### Issue: "Qt DLLs not found"

**Solution**: Verify Qt path in `cmake/SAK_BuildConfig.cmake`:

```cmake
if(NOT DEFINED CMAKE_PREFIX_PATH)
    set(CMAKE_PREFIX_PATH "C:/Qt/6.5.3/msvc2019_64" 
        CACHE PATH "Qt installation path")
endif()
```

Manually deploy Qt DLLs if needed:

```powershell
C:\Qt\6.5.3\msvc2019_64\bin\windeployqt.exe --release --no-translations .\build\Release\sak_utility.exe
```

### Issue: "vcpkg dependencies not found"

**Solution**: Install static vcpkg packages:

```powershell
cd C:\vcpkg
.\vcpkg install zlib:x64-windows-static bzip2:x64-windows-static liblzma:x64-windows-static
```

Verify vcpkg baseline in `vcpkg.json` is current:

```json
{
  "builtin-baseline": "2b6a882f61eaa764ec3cf816e2265fe804a44cd0"
}
```

### Issue: "Action files missing from build"

**Solution**: Run verification script:

```powershell
.\scripts\verify_build.ps1
```

This checks:
- All 39 action .cpp files exist
- All files referenced in CMakeLists.txt
- action_factory.cpp present

### Issue: Build succeeds but linker errors in tests

**Known Issue**: Test executables have logger-related linker errors. This does NOT affect the main executable.

**Workaround**: Build only the main target:

```powershell
cmake --build build --config Release --target sak_utility --parallel
```

---

## Build Targets

### Main Executable
```powershell
cmake --build build --config Release --target sak_utility
```

### All Targets (including tests)
```powershell
cmake --build build --config Release --parallel
```

### Individual Test
```powershell
cmake --build build --config Release --target test_app_scanner
```

### Available Tests
- `test_app_scanner` - Test application scanner
- `test_chocolatey_manager` - Test Chocolatey manager
- `test_choco_install` - Test real package installation
- `test_integration_phase1_2` - Integration test
- `test_package_matcher` - Test package matcher
- `test_migration_report` - Test migration report
- `test_app_migration_worker` - Test migration worker
- `test_user_data_manager` - Test user data manager
- `test_backup_wizard` - Test backup wizard GUI
- `test_restore_wizard` - Test restore wizard GUI

**Note**: Some tests have linker errors (logger symbols). Main executable is unaffected.

---

## Deployment

### Release Package Contents

After building, the `build\Release` directory contains:

```
sak_utility.exe          - Main executable (static runtime, no MSVC DLLs needed)
Qt6Core.dll              - Qt core library (release)
Qt6Widgets.dll           - Qt widgets library (release)
Qt6Gui.dll               - Qt GUI library (release)
platforms\               - Qt platform plugins
  qwindows.dll           - Windows platform plugin
styles\                  - Qt style plugins
imageformats\            - Qt image format plugins
```

### Creating Portable Package

```powershell
# Create distribution directory
New-Item -Path "dist" -ItemType Directory -Force

# Copy executable and Qt DLLs
Copy-Item -Path "build\Release\*" -Destination "dist\" -Recurse -Include "*.exe","*.dll"
Copy-Item -Path "build\Release\platforms" -Destination "dist\platforms" -Recurse
Copy-Item -Path "build\Release\styles" -Destination "dist\styles" -Recurse

# Add configuration
Copy-Item -Path "portable.ini" -Destination "dist\"

# Create ZIP
Compress-Archive -Path "dist\*" -DestinationPath "SAK_Utility_v0.5.5_Portable.zip"
```

### Testing on Fresh PC

1. Copy entire `dist` folder to test PC
2. Run `sak_utility.exe` directly
3. **No Visual C++ Redistributable required**
4. **No Qt installation required**
5. All dependencies embedded

---

## Development Workflow

### Adding New Actions

1. Create action files:
   ```
   src/actions/my_new_action.cpp
   include/sak/actions/my_new_action.h
   ```

2. Add to `CMakeLists.txt` QUICK_ACTIONS_SOURCES:
   ```cmake
   src/actions/my_new_action.cpp
   include/sak/actions/my_new_action.h
   ```

3. Register in `action_factory.cpp`

4. Rebuild:
   ```powershell
   cmake --build build --config Release --target sak_utility
   ```

### Modifying Build Configuration

**To change vcpkg path:**
Edit `cmake/SAK_BuildConfig.cmake` line 9:
```cmake
set(CMAKE_TOOLCHAIN_FILE "C:/your/vcpkg/path/scripts/buildsystems/vcpkg.cmake" 
    CACHE STRING "Vcpkg toolchain file")
```

**To change Qt path:**
Edit `cmake/SAK_BuildConfig.cmake` line 20:
```cmake
set(CMAKE_PREFIX_PATH "C:/Qt/6.5.3/msvc2019_64" 
    CACHE PATH "Qt installation path")
```

**Rebuild required after changes:**
```powershell
Remove-Item -Path build -Recurse -Force
cmake -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

---

## CI/CD Integration

### GitHub Actions Example

```yaml
name: Build SAK Utility

on: [push, pull_request]

jobs:
  build:
    runs-on: windows-latest
    
    steps:
    - uses: actions/checkout@v3
    
    - name: Setup vcpkg
      run: |
        git clone https://github.com/Microsoft/vcpkg.git C:\vcpkg
        C:\vcpkg\bootstrap-vcpkg.bat
        C:\vcpkg\vcpkg integrate install
    
    - name: Install Qt
      uses: jurplel/install-qt-action@v3
      with:
        version: '6.5.3'
        arch: 'win64_msvc2019_64'
    
    - name: Configure
      run: cmake -B build -G "Visual Studio 17 2022" -A x64
    
    - name: Build
      run: cmake --build build --config Release --target sak_utility --parallel
    
    - name: Verify
      run: .\scripts\verify_build.ps1 -SkipBuild
    
    - name: Upload Artifact
      uses: actions/upload-artifact@v3
      with:
        name: sak-utility-windows
        path: build/Release/
```

---

## Performance Notes

### Build Times

Typical build times on modern hardware (Ryzen 9/i9):

- **Clean build**: 2-3 minutes (all 39 actions + core)
- **Incremental build**: 10-30 seconds (modified files only)
- **Link time**: 5-15 seconds (static linking overhead)

### Optimization Flags

Release build uses:
- `/O2` - Maximize speed optimization
- `/GL` - Whole program optimization
- `/LTCG` - Link-time code generation
- `/MT` - Static runtime (increases binary size but eliminates DLLs)

### Binary Size

- **Executable only**: ~2-4 MB (static runtime + compression)
- **With Qt DLLs**: ~20-30 MB total
- **Full distribution**: ~50-80 MB (includes plugins)

Static linking increases executable size but eliminates 5+ separate DLL files.

---

## Best Practices

1. **Always use SAK_BuildConfig.cmake** - Never manually specify CMAKE_TOOLCHAIN_FILE
2. **Run verify_build.ps1 after major changes** - Catches missing files early
3. **Build Release for testing** - Debug builds are 5-10x larger
4. **Use --parallel for faster builds** - Leverages multi-core CPUs
5. **Target sak_utility specifically** - Skips test executables with known issues
6. **Clean build after dependency changes** - vcpkg baseline updates require fresh configuration

---

## Version Information

- **Project Version**: 0.5.5
- **CMake Minimum**: 3.28
- **C++ Standard**: C++23
- **Build System**: CMake + Visual Studio 2022
- **Static Runtime**: MultiThreaded (no DLL dependencies)
- **vcpkg Baseline**: 2b6a882f61eaa764ec3cf816e2265fe804a44cd0

---

## Support

For build issues:
1. Run `.\scripts\verify_build.ps1` to diagnose
2. Check this document for troubleshooting
3. Verify SAK_BuildConfig.cmake has correct paths
4. Try clean build: `Remove-Item build -Recurse; cmake -B build ...`

For development questions:
1. Check CMakeLists.txt structure
2. Review action_factory.cpp for action registration
3. Verify source files in correct directories (src/actions, src/core, src/gui)
