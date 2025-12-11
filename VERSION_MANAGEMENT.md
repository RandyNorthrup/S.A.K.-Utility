# Version Management

## Current Version: 0.5.0

This document tracks the version history and explains the versioning scheme.

## Versioning Scheme

S.A.K. Utility uses **Semantic Versioning 2.0.0**: `MAJOR.MINOR.PATCH`

- **MAJOR**: Incompatible API changes or major feature overhauls
- **MINOR**: New functionality in a backwards-compatible manner
- **PATCH**: Backwards-compatible bug fixes

## Version Locations

The version number is defined in multiple places and must be kept synchronized:

1. **`VERSION`** - Single source of truth (plain text file)
2. **`include/sak/version.h`** - C++ header with version constants
3. **`CMakeLists.txt`** - CMake project version (line 5)
4. **`Readme.md`** - Documentation (Overview section)
5. **`.github/workflows/build-release.yml`** - CI/CD workflow

## Updating Version

To update the version number:

1. **Edit `include/sak/version.h`**:
   ```cpp
   #define SAK_VERSION_MAJOR 0
   #define SAK_VERSION_MINOR 6
   #define SAK_VERSION_PATCH 0
   #define SAK_VERSION_STRING "0.6.0"
   ```

2. **Edit `CMakeLists.txt`**:
   ```cmake
   project(SAK_Utility
       VERSION 0.6.0
       ...
   )
   ```

3. **Edit `VERSION` file**:
   ```
   0.6.0
   ```

4. **Edit `Readme.md`**:
   ```markdown
   **Version**: 0.6
   ```

5. **Edit `.github/workflows/build-release.yml`**:
   - Update package names: `SAK-Utility-v0.6.0`
   - Update build info display

6. **Commit and tag**:
   ```powershell
   git add .
   git commit -m "Bump version to 0.6.0"
   git tag -a v0.6.0 -m "Release version 0.6.0"
   git push origin main --tags
   ```

## Release Process

### Creating a Release

1. **Update version** in all files listed above
2. **Update `Readme.md`** with new features/changes
3. **Commit changes**:
   ```powershell
   git add .
   git commit -m "Release v0.5.0"
   ```
4. **Create and push tag**:
   ```powershell
   git tag -a v0.5.0 -m "Release version 0.5.0 - Initial beta release"
   git push origin main --tags
   ```
5. **GitHub Actions will automatically**:
   - Build the application
   - Run tests
   - Package with all dependencies
   - Create a GitHub Release
   - Upload ZIP artifact

### Manual Release Build

If you need to build locally:

```powershell
# Configure
cmake -B build -G "Visual Studio 17 2022" -A x64 -DCMAKE_PREFIX_PATH="C:/Qt/6.5.3/msvc2019_64"

# Build
cmake --build build --config Release

# Package
cd build/Release
New-Item -ItemType Directory -Force -Path "SAK-Utility-v0.5.0"
Copy-Item "sak_utility.exe" "SAK-Utility-v0.5.0/"
# ... copy dependencies ...
Compress-Archive -Path "SAK-Utility-v0.5.0" -DestinationPath "SAK-Utility-v0.5.0-Windows-x64.zip"
```

## Version History

### 0.5.0 (December 10, 2025) - Initial Beta Release
- ✅ User Profile Backup & Restore wizards (6 pages each)
- ✅ Application Migration with embedded Chocolatey
- ✅ Directory Organizer
- ✅ Duplicate File Finder (MD5-based)
- ✅ License Key Scanner
- ✅ Portable mode support
- ✅ 12 test executables
- ✅ C++23 with strict compliance
- ✅ Qt 6.5.3 GUI
- ⚠️ Windows 10/11 x64 only

### Planned Releases

#### 0.6.0 (Q1 2026)
- Keep-Awake GUI checkbox
- CTest integration
- Backup compression (ZIP/7z)
- Dark mode theme
- Enhanced logging viewer

#### 0.7.0 (Q2 2026)
- Incremental backup support
- Backup scheduling (Task Scheduler)
- Settings persistence improvements
- Full ACL preservation

#### 1.0.0 (Q3 2026) - Stable Release
- Production-ready
- Full ACL backup/restore verification
- Code signing certificate
- MSI installer
- Comprehensive documentation
