/**
 * Test program for UserDataManager
 * Tests backup, restore, discovery, and verification functionality
 */

#include <QCoreApplication>
#include <QDebug>
#include <QDir>
#include <QTimer>
#include <iostream>

#include "sak/user_data_manager.h"

using namespace sak;

// ANSI color codes
#define COLOR_RESET   "\033[0m"
#define COLOR_RED     "\033[31m"
#define COLOR_GREEN   "\033[32m"
#define COLOR_YELLOW  "\033[33m"
#define COLOR_BLUE    "\033[34m"
#define COLOR_CYAN    "\033[36m"

class TestRunner : public QObject {
    Q_OBJECT

public:
    TestRunner(QObject* parent = nullptr) : QObject(parent) {
        // Create manager
        manager = std::make_unique<UserDataManager>();
        
        // Connect signals
        connect(manager.get(), &UserDataManager::operationStarted,
                this, [](const QString& appName, const QString& operation) {
            std::cout << COLOR_CYAN << "[OPERATION] " << COLOR_RESET 
                      << operation.toStdString() << " started for: " 
                      << appName.toStdString() << std::endl;
        });
        
        connect(manager.get(), &UserDataManager::progressUpdate,
                this, [](int current, int total, const QString& message) {
            std::cout << COLOR_BLUE << "[PROGRESS] " << COLOR_RESET 
                      << current << "/" << total << " - " 
                      << message.toStdString() << std::endl;
        });
        
        connect(manager.get(), &UserDataManager::operationCompleted,
                this, [](const QString& appName, bool success, const QString& message) {
            if (success) {
                std::cout << COLOR_GREEN << "[SUCCESS] " << COLOR_RESET 
                          << appName.toStdString() << ": " 
                          << message.toStdString() << std::endl;
            } else {
                std::cout << COLOR_RED << "[FAILED] " << COLOR_RESET 
                          << appName.toStdString() << ": " 
                          << message.toStdString() << std::endl;
            }
        });
        
        connect(manager.get(), &UserDataManager::operationError,
                this, [](const QString& appName, const QString& error) {
            std::cout << COLOR_RED << "[ERROR] " << COLOR_RESET 
                      << appName.toStdString() << ": " 
                      << error.toStdString() << std::endl;
        });
    }

    void run() {
        std::cout << "\n" << COLOR_CYAN << "=== UserDataManager Test Suite ===" << COLOR_RESET << "\n\n";
        
        // Create test directory
        testDir = QDir::temp().absoluteFilePath("sak_backup_test");
        QDir().mkpath(testDir);
        std::cout << "Test directory: " << testDir.toStdString() << "\n\n";
        
        // Run test phases
        QTimer::singleShot(0, this, &TestRunner::testPhase1);
    }

private slots:
    void testPhase1() {
        std::cout << COLOR_YELLOW << "\n=== Phase 1: Discover Common App Data Locations ===" << COLOR_RESET << "\n";
        
        auto locations = manager->getCommonDataLocations();
        std::cout << "Found " << locations.size() << " common data locations:\n";
        
        for (const auto& loc : locations) {
            std::cout << "\n  Pattern: " << loc.pattern.toStdString() << "\n";
            std::cout << "  Description: " << loc.description.toStdString() << "\n";
            std::cout << "  Paths (" << loc.paths.size() << "):\n";
            for (const auto& path : loc.paths) {
                QFileInfo info(path);
                std::cout << "    - " << path.toStdString();
                if (info.exists()) {
                    std::cout << " " << COLOR_GREEN << "[EXISTS]" << COLOR_RESET;
                } else {
                    std::cout << " " << COLOR_YELLOW << "[NOT FOUND]" << COLOR_RESET;
                }
                std::cout << "\n";
            }
        }
        
        QTimer::singleShot(0, this, &TestRunner::testPhase2);
    }
    
    void testPhase2() {
        std::cout << COLOR_YELLOW << "\n=== Phase 2: Create Test Data ===" << COLOR_RESET << "\n";
        
        // Create test app data
        testAppDir = QDir(testDir).absoluteFilePath("TestApp");
        QDir().mkpath(testAppDir);
        
        // Create some test files
        QFile file1(QDir(testAppDir).filePath("config.json"));
        if (file1.open(QIODevice::WriteOnly)) {
            file1.write("{\"setting1\": \"value1\", \"setting2\": \"value2\"}");
            file1.close();
            std::cout << "  Created: config.json\n";
        }
        
        QFile file2(QDir(testAppDir).filePath("user.dat"));
        if (file2.open(QIODevice::WriteOnly)) {
            file2.write("User data content here...");
            file2.close();
            std::cout << "  Created: user.dat\n";
        }
        
        // Create a log file (should be excluded)
        QFile logFile(QDir(testAppDir).filePath("debug.log"));
        if (logFile.open(QIODevice::WriteOnly)) {
            logFile.write("This is a log file that should be excluded from backup");
            logFile.close();
            std::cout << "  Created: debug.log (will be excluded)\n";
        }
        
        // Create a cache subdirectory (should be excluded)
        QString cacheDir = QDir(testAppDir).filePath("cache");
        QDir().mkpath(cacheDir);
        QFile cacheFile(QDir(cacheDir).filePath("cached_data.tmp"));
        if (cacheFile.open(QIODevice::WriteOnly)) {
            cacheFile.write("Cached data that should be excluded");
            cacheFile.close();
            std::cout << "  Created: cache/cached_data.tmp (will be excluded)\n";
        }
        
        std::cout << "\nTest data created at: " << testAppDir.toStdString() << "\n";
        
        QTimer::singleShot(0, this, &TestRunner::testPhase3);
    }
    
    void testPhase3() {
        std::cout << COLOR_YELLOW << "\n=== Phase 3: Calculate Data Size ===" << COLOR_RESET << "\n";
        
        QStringList paths;
        paths << testAppDir;
        
        qint64 totalSize = manager->calculateSize(paths);
        std::cout << "Total size: " << totalSize << " bytes\n";
        
        QTimer::singleShot(0, this, &TestRunner::testPhase4);
    }
    
    void testPhase4() {
        std::cout << COLOR_YELLOW << "\n=== Phase 4: Backup App Data ===" << COLOR_RESET << "\n";
        
        backupDir = QDir(testDir).absoluteFilePath("backups");
        QDir().mkpath(backupDir);
        
        QStringList sourcePaths;
        sourcePaths << testAppDir;
        
        UserDataManager::BackupConfig config;
        config.compress = true;
        config.verify_checksum = true;
        config.exclude_patterns = QStringList{"*.log", "*.tmp", "cache/*", "temp/*"};
        
        std::cout << "Backing up TestApp...\n";
        std::cout << "  Source: " << testAppDir.toStdString() << "\n";
        std::cout << "  Backup dir: " << backupDir.toStdString() << "\n";
        std::cout << "  Exclusions: " << config.exclude_patterns.join(", ").toStdString() << "\n";
        
        backupEntry = manager->backupAppData("TestApp", sourcePaths, backupDir, config);
        
        if (backupEntry.has_value()) {
            std::cout << COLOR_GREEN << "\nBackup successful!" << COLOR_RESET << "\n";
            std::cout << "  Backup path: " << backupEntry->backup_path.toStdString() << "\n";
            std::cout << "  Total size: " << backupEntry->total_size << " bytes\n";
            std::cout << "  Compressed size: " << backupEntry->compressed_size << " bytes\n";
            std::cout << "  Compression ratio: " 
                      << (100.0 * backupEntry->compressed_size / backupEntry->total_size) 
                      << "%\n";
            std::cout << "  Checksum: " << backupEntry->checksum.toStdString() << "\n";
            std::cout << "  Excluded patterns: " 
                      << backupEntry->excluded_patterns.join(", ").toStdString() << "\n";
        } else {
            std::cout << COLOR_RED << "Backup failed!" << COLOR_RESET << "\n";
        }
        
        QTimer::singleShot(0, this, &TestRunner::testPhase5);
    }
    
    void testPhase5() {
        std::cout << COLOR_YELLOW << "\n=== Phase 5: Verify Backup ===" << COLOR_RESET << "\n";
        
        if (!backupEntry.has_value()) {
            std::cout << COLOR_RED << "No backup to verify!" << COLOR_RESET << "\n";
            QTimer::singleShot(0, this, &TestRunner::testPhase8);
            return;
        }
        
        bool valid = manager->verifyBackup(backupEntry->backup_path);
        if (valid) {
            std::cout << COLOR_GREEN << "Backup verification passed!" << COLOR_RESET << "\n";
        } else {
            std::cout << COLOR_RED << "Backup verification failed!" << COLOR_RESET << "\n";
        }
        
        QTimer::singleShot(0, this, &TestRunner::testPhase6);
    }
    
    void testPhase6() {
        std::cout << COLOR_YELLOW << "\n=== Phase 6: List Backups ===" << COLOR_RESET << "\n";
        
        auto backups = manager->listBackups(backupDir);
        std::cout << "Found " << backups.size() << " backup(s):\n";
        
        for (const auto& backup : backups) {
            std::cout << "\n  App: " << backup.app_name.toStdString() << "\n";
            std::cout << "  Version: " << backup.app_version.toStdString() << "\n";
            std::cout << "  Date: " << backup.backup_date.toString().toStdString() << "\n";
            std::cout << "  Size: " << backup.total_size << " bytes\n";
            std::cout << "  Compressed: " << backup.compressed_size << " bytes\n";
            std::cout << "  Path: " << backup.backup_path.toStdString() << "\n";
        }
        
        QTimer::singleShot(0, this, &TestRunner::testPhase7);
    }
    
    void testPhase7() {
        std::cout << COLOR_YELLOW << "\n=== Phase 7: Restore to Different Directory ===" << COLOR_RESET << "\n";
        
        if (!backupEntry.has_value()) {
            std::cout << COLOR_RED << "No backup to restore!" << COLOR_RESET << "\n";
            QTimer::singleShot(0, this, &TestRunner::testPhase8);
            return;
        }
        
        restoreDir = QDir(testDir).absoluteFilePath("restored");
        QDir().mkpath(restoreDir);
        
        UserDataManager::RestoreConfig config;
        config.verify_checksum = true;
        config.create_backup = false;  // Don't backup since this is a new location
        config.overwrite_existing = true;
        
        std::cout << "Restoring TestApp...\n";
        std::cout << "  From: " << backupEntry->backup_path.toStdString() << "\n";
        std::cout << "  To: " << restoreDir.toStdString() << "\n";
        
        bool success = manager->restoreAppData(backupEntry->backup_path, restoreDir, config);
        
        if (success) {
            std::cout << COLOR_GREEN << "\nRestore successful!" << COLOR_RESET << "\n";
            
            // List restored files
            QDir restored(restoreDir);
            QStringList files = restored.entryList(QDir::Files | QDir::NoDotAndDotDot);
            std::cout << "  Restored files:\n";
            for (const auto& file : files) {
                std::cout << "    - " << file.toStdString() << "\n";
            }
            
            // Verify excluded files are NOT restored
            bool hasLog = QFile::exists(QDir(restoreDir).filePath("debug.log"));
            bool hasCache = QDir(restoreDir).exists("cache");
            
            if (!hasLog && !hasCache) {
                std::cout << COLOR_GREEN << "  ✓ Exclusion patterns applied correctly" << COLOR_RESET << "\n";
            } else {
                std::cout << COLOR_RED << "  ✗ Exclusion patterns not applied!" << COLOR_RESET << "\n";
            }
        } else {
            std::cout << COLOR_RED << "Restore failed!" << COLOR_RESET << "\n";
        }
        
        QTimer::singleShot(0, this, &TestRunner::testPhase8);
    }
    
    void testPhase8() {
        std::cout << COLOR_YELLOW << "\n=== Phase 8: Verify Restored Data ===" << COLOR_RESET << "\n";
        
        // Check if config.json was restored
        QString restoredConfig = QDir(restoreDir).filePath("config.json");
        if (QFile::exists(restoredConfig)) {
            QFile file(restoredConfig);
            if (file.open(QIODevice::ReadOnly)) {
                QString content = file.readAll();
                std::cout << "  config.json content: " << content.toStdString() << "\n";
                file.close();
                std::cout << COLOR_GREEN << "  ✓ config.json restored correctly" << COLOR_RESET << "\n";
            }
        } else {
            std::cout << COLOR_RED << "  ✗ config.json not found!" << COLOR_RESET << "\n";
        }
        
        // Check if user.dat was restored
        QString restoredUser = QDir(restoreDir).filePath("user.dat");
        if (QFile::exists(restoredUser)) {
            std::cout << COLOR_GREEN << "  ✓ user.dat restored correctly" << COLOR_RESET << "\n";
        } else {
            std::cout << COLOR_RED << "  ✗ user.dat not found!" << COLOR_RESET << "\n";
        }
        
        // Verify checksums match
        if (backupEntry.has_value()) {
            QString originalConfig = QDir(testAppDir).filePath("config.json");
            if (QFile::exists(originalConfig) && QFile::exists(restoredConfig)) {
                bool match = manager->compareChecksums(originalConfig, restoredConfig);
                if (match) {
                    std::cout << COLOR_GREEN << "  ✓ Checksums match for config.json" << COLOR_RESET << "\n";
                } else {
                    std::cout << COLOR_RED << "  ✗ Checksum mismatch for config.json!" << COLOR_RESET << "\n";
                }
            }
        }
        
        QTimer::singleShot(0, this, &TestRunner::cleanup);
    }
    
    void cleanup() {
        std::cout << COLOR_YELLOW << "\n=== Cleanup ===" << COLOR_RESET << "\n";
        std::cout << "Test files remain at: " << testDir.toStdString() << "\n";
        std::cout << "  - Test data: " << testAppDir.toStdString() << "\n";
        std::cout << "  - Backups: " << backupDir.toStdString() << "\n";
        std::cout << "  - Restored: " << restoreDir.toStdString() << "\n";
        
        std::cout << "\n" << COLOR_GREEN << "=== Test Suite Complete ===" << COLOR_RESET << "\n\n";
        
        QCoreApplication::quit();
    }

private:
    std::unique_ptr<UserDataManager> manager;
    QString testDir;
    QString testAppDir;
    QString backupDir;
    QString restoreDir;
    std::optional<UserDataManager::BackupEntry> backupEntry;
};

int main(int argc, char* argv[]) {
    QCoreApplication app(argc, argv);
    
    TestRunner runner;
    runner.run();
    
    return app.exec();
}

#include "test_user_data_manager.moc"
