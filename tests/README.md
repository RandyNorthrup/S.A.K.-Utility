# SAK Utility - Test Suite Documentation

## Overview

Comprehensive test suite for SAK Utility covering unit tests, integration tests, and end-to-end workflows.

## Test Structure

```
tests/
├── unit/                      # Unit tests for individual components
│   ├── test_path_utils.cpp
│   ├── test_logger.cpp
│   ├── test_config_manager.cpp
│   ├── test_file_scanner.cpp
│   ├── test_encryption.cpp
│   └── actions/               # Unit tests for action classes
│       ├── test_disk_cleanup_action.cpp
│       └── test_backup_browser_data_action.cpp
├── integration/               # Integration tests for workflows
│   ├── test_backup_workflow.cpp
│   └── test_migration_workflow.cpp
├── test_*.cpp                 # Legacy integration/manual tests
└── CMakeLists.txt            # Test build configuration
```

## Running Tests

### Build All Tests

```powershell
# Configure with tests
cmake -S . -B build

# Build all tests
cmake --build build --config Release --target run_all_tests
```

### Run Specific Test Categories

```powershell
# Run all unit tests
cmake --build build --target run_unit_tests

# Run all integration tests
cmake --build build --target run_integration_tests

# Run specific test
.\build\Release\test_path_utils.exe
```

### Using CTest

```powershell
# Run all tests with CTest
cd build
ctest --output-on-failure

# Run tests matching pattern
ctest -R "test_path"

# Run with verbose output
ctest -VV

# Generate XML report
ctest --output-junit test_results.xml
```

## Test Categories

### Unit Tests - Core Components

#### test_path_utils
Tests path manipulation, validation, and sanitization.

**Coverage:**
- Path validation (valid/invalid paths, reserved names)
- Path normalization (forward/back slashes, ..)
- Filename sanitization (remove invalid characters)
- Relative path calculation
- Path joining with proper separators
- Environment variable expansion
- File and directory size calculation
- Directory creation with nested paths
- Safe deletion checks (protect system directories)

**Key Test Cases:**
```cpp
testIsValidPath()           // Validate Windows paths
testNormalizePath()         // Convert mixed slashes
testSanitizeFilename()      // Remove illegal characters
testExpandEnvironmentVariables()  // %USERPROFILE% expansion
testIsSafeToDelete()        // Prevent system folder deletion
```

#### test_logger
Tests logging functionality with different levels and outputs.

**Coverage:**
- Singleton pattern enforcement
- Log level filtering (Debug, Info, Warning, Error)
- Log formatting with timestamps
- File rotation when size limits reached
- Concurrent logging from multiple threads
- Context information (class, method names)

**Key Test Cases:**
```cpp
testLogLevels()             // Filter by minimum level
testLogFormatting()         // Timestamp and level prefix
testLogRotation()           // Auto-rotate large files
testConcurrentLogging()     // Thread-safe operations
```

#### test_config_manager
Tests configuration storage and retrieval.

**Coverage:**
- Default configuration values
- Setting and getting values (string, int, bool)
- Configuration persistence across instances
- Value removal and clearing
- Group and key enumeration
- Portable mode detection (portable.ini)
- Array value storage
- Type conversions (string to int/bool/double)

**Key Test Cases:**
```cpp
testSetAndGetValues()       // Store and retrieve config
testPersistence()           // Save and load from file
testPortableMode()          // Detect portable.ini
testTypeConversions()       // Auto-convert value types
```

#### test_file_scanner
Tests file system scanning and filtering.

**Coverage:**
- Recursive and non-recursive scanning
- Extension filtering (*.txt, *.log)
- Multiple extension support
- Directory exclusion patterns
- File size filtering (min/max)
- Date range filtering (modified after/before)
- Hidden file handling
- Symlink following
- Progress reporting
- Scan cancellation

**Key Test Cases:**
```cpp
testScanWithExtensionFilter()  // Filter by file type
testRecursiveScan()            // Include subdirectories
testExcludeDirectories()       // Skip specified folders
testMinMaxFileSize()           // Filter by size range
testCancelScan()               // Abort long-running scan
```

#### test_encryption
Tests encryption, decryption, and key management.

**Coverage:**
- String encryption/decryption
- Binary data (QByteArray) encryption
- Password verification (wrong password fails)
- Empty data handling
- Large data encryption (10KB+)
- Unicode text support
- Special character handling
- Random key generation (256-bit)
- Salt generation
- Password hashing (with verification)
- File encryption/decryption
- Encryption strength (random IV per encryption)

**Key Test Cases:**
```cpp
testEncryptDecryptString()     // Encrypt and decrypt text
testWrongPassword()            // Fail with incorrect password
testUnicodeData()              // Handle international text
testEncryptFile()              // Encrypt entire files
testEncryptionStrength()       // Different IV each time
```

### Unit Tests - Action Classes

#### test_disk_cleanup_action
Tests disk cleanup with file deletion.

**Coverage:**
- Temp file scanning and size calculation
- Cleanup execution with signals
- Progress reporting during cleanup
- Exclude patterns (keep certain files)
- Dry run mode (calculate without deleting)
- Cancellation support

**Key Test Cases:**
```cpp
testScanTempFiles()            // Find cleanable files
testCleanupExecution()         // Delete files, emit signals
testExcludePatterns()          // Keep *.log files
testDryRun()                   // Calculate without deleting
```

#### test_backup_browser_data_action
Tests browser profile backup.

**Coverage:**
- Chrome profile detection
- Firefox profile detection
- Chrome data backup (Bookmarks, History, Cookies)
- Firefox data backup (places.sqlite, cookies.sqlite)
- Selective backup (choose which items)
- Manifest creation (backup metadata)
- Error handling (invalid paths)

**Key Test Cases:**
```cpp
testDetectChrome()             // Find Chrome profiles
testBackupChrome()             // Copy Chrome data
testSelectiveBackup()          // Backup only selected items
testManifestCreation()         // Create backup metadata
```

### Integration Tests

#### test_backup_workflow
Tests end-to-end backup process.

**Coverage:**
- Full backup workflow (Documents, Desktop, AppData)
- Incremental backup (only changed files)
- Backup verification (hash checks)
- Backup encryption (with password)
- Progress tracking
- Backup cancellation
- File filtering (include/exclude patterns)
- Restore from backup

**Key Test Cases:**
```cpp
testFullBackupWorkflow()       // Complete backup cycle
testIncrementalBackup()        // Only backup changes
testBackupEncryption()         // Encrypt backup data
testRestoreFromBackup()        // Restore backed up files
```

#### test_migration_workflow
Tests app migration process.

**Coverage:**
- App scanning and matching
- Chocolatey package resolution
- Migration report creation
- Report import/export (JSON)
- Migration execution
- Progress tracking
- Error handling

**Key Test Cases:**
```cpp
testAppScanningAndMatching()   // Scan apps, find packages
testMigrationReportCreation()  // Generate migration plan
testMigrationExecution()       // Install matched packages
```

## Test Data

### Fixtures

Tests use temporary directories for isolated file operations:

```cpp
QTemporaryDir tempDir;  // Auto-cleaned after test
QString testPath = tempDir.path();
```

### Mock Data

Integration tests create realistic test data structures:

```cpp
// Browser profile structure
AppData/Local/Google/Chrome/User Data/Default/
    ├── Bookmarks
    ├── History
    ├── Preferences
    └── Cookies

AppData/Roaming/Mozilla/Firefox/Profiles/test.default/
    ├── places.sqlite
    ├── cookies.sqlite
    └── prefs.js
```

## CI/CD Integration

### GitHub Actions

Tests run automatically on:
- Push to main
- Pull requests
- Release tags

```yaml
- name: Run Tests
  run: |
    cmake --build build --target run_all_tests
    cd build
    ctest --output-junit test_results.xml

- name: Upload Test Results
  uses: actions/upload-artifact@v4
  with:
    name: test-results
    path: build/test_results.xml
```

## Writing New Tests

### Unit Test Template

```cpp
#include "sak/my_component.h"
#include <QTest>

class TestMyComponent : public QObject {
    Q_OBJECT

private slots:
    void initTestCase() {
        // Setup before all tests
    }

    void cleanupTestCase() {
        // Cleanup after all tests
    }

    void init() {
        // Setup before each test
    }

    void cleanup() {
        // Cleanup after each test
    }

    void testBasicFunctionality() {
        sak::MyComponent component;
        
        QVERIFY(component.isValid());
        QCOMPARE(component.getValue(), 42);
    }
};

QTEST_MAIN(TestMyComponent)
#include "test_my_component.moc"
```

### Adding to CMakeLists.txt

```cmake
# Add to UNIT_TESTS list
set(UNIT_TESTS
    test_path_utils
    test_logger
    test_my_component  # <-- Add here
)
```

## Best Practices

### 1. Isolation
- Each test should be independent
- Use temporary directories for file operations
- Clean up resources in `cleanup()` slots

### 2. Clarity
- Descriptive test names (`testSanitizeFilenameRemovesIllegalChars`)
- One concept per test
- Clear assertion messages

### 3. Coverage
- Test happy path and error cases
- Test edge cases (empty input, max values)
- Test boundary conditions

### 4. Performance
- Keep tests fast (<1 second each)
- Use mocks for slow operations
- Parallel test execution where possible

### 5. Maintainability
- Avoid hardcoded paths (use QTemporaryDir)
- Use test fixtures for repeated setup
- Document complex test scenarios

## QTest Assertions

```cpp
QVERIFY(condition)                    // Verify condition is true
QCOMPARE(actual, expected)            // Verify exact match
QVERIFY2(condition, "message")        // Verify with message
QTEST(actual, "expected")             // Data-driven testing

// Exceptions and signals
QVERIFY_EXCEPTION_THROWN(code, ExceptionType)
QSignalSpy spy(&object, &Object::signal);
QCOMPARE(spy.count(), 1);

// Skip tests
QSKIP("Test not applicable");

// Expected failures
QEXPECT_FAIL("", "Known issue", Continue);
```

## Debugging Tests

### Visual Studio

1. Set test executable as startup project
2. Add breakpoints in test code
3. Press F5 to debug

### Command Line

```powershell
# Run single test with debugger
windbg .\build\Release\test_path_utils.exe

# Run with verbose output
.\build\Release\test_path_utils.exe -v2
```

## Test Coverage

### Measuring Coverage

```powershell
# Build with coverage flags
cmake -S . -B build -DCMAKE_CXX_FLAGS="--coverage"
cmake --build build

# Run tests
cd build
ctest

# Generate coverage report
gcov **/*.cpp
lcov --capture --directory . --output-file coverage.info
genhtml coverage.info --output-directory coverage_html
```

## Continuous Improvement

### Current Coverage
- ✅ Core utilities (path_utils, logger, config_manager)
- ✅ File operations (file_scanner, encryption)
- ✅ Action system (disk_cleanup, browser_backup)
- ✅ Backup workflow (full, incremental, encrypted)
- ✅ Migration workflow (scan, match, report)

### Future Additions
- [ ] GUI component tests
- [ ] Windows-specific functionality tests
- [ ] Performance benchmarks
- [ ] Stress tests (large datasets)
- [ ] Security tests (privilege escalation)

## Support

For test-related questions:
- Check existing tests for examples
- Review Qt Test documentation
- Open issue on GitHub
- Contact maintainers

---

**Last Updated:** December 16, 2025  
**Test Framework:** Qt Test  
**Qt Version:** 6.5.3  
**Platform:** Windows 10/11
