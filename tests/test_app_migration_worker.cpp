#include <QCoreApplication>
#include <QDebug>
#include <memory>
#include "sak/app_scanner.h"
#include "sak/chocolatey_manager.h"
#include "sak/package_matcher.h"
#include "sak/migration_report.h"
#include "sak/app_migration_worker.h"

using namespace sak;

int main(int argc, char *argv[]) {
    QCoreApplication app(argc, argv);
    
    qDebug() << "\n=== AppMigrationWorker Test ===\n";
    
    // Phase 1: Scan applications
    qDebug() << "Phase 1: Scanning installed applications...";
    AppScanner scanner;
    auto apps = scanner.scanAll();
    qDebug() << "Found" << apps.size() << "applications\n";
    
    if (apps.empty()) {
        qWarning() << "No applications found!";
        return 1;
    }
    
    // Phase 2: Initialize ChocolateyManager
    qDebug() << "Phase 2: Initializing ChocolateyManager...";
    auto chocoManager = std::make_shared<ChocolateyManager>();
    
    if (!chocoManager->initialize("../../tools/chocolatey")) {
        qWarning() << "ChocolateyManager not available!";
        return 1;
    }
    qDebug() << "✓ ChocolateyManager initialized\n";
    
    // Phase 3: Match packages (use first 50 for speed)
    qDebug() << "Phase 3: Matching packages (first 50 apps)...";
    PackageMatcher matcher;
    
    size_t matchCount = std::min<size_t>(50, apps.size());
    std::vector<AppScanner::AppInfo> appsToMatch(apps.begin(), apps.begin() + matchCount);
    
    PackageMatcher::MatchConfig config;
    config.use_choco_search = false;  // No search for speed
    auto matches = matcher.findMatchesParallel(appsToMatch, chocoManager.get(), config);
    qDebug() << "Matched" << matches.size() << "out of" << appsToMatch.size() << "apps\n";
    
    // Phase 4: Generate migration report
    qDebug() << "Phase 4: Generating migration report...";
    auto report = std::make_shared<MigrationReport>();
    report->generateReport(appsToMatch, matches);
    
    // Select only high-confidence matches for testing
    report->selectByConfidence(0.95);
    
    int totalApps = report->getEntryCount();
    int matchedApps = report->getMatchedCount();
    int selectedApps = report->getSelectedCount();
    double matchRate = report->getMatchRate();
    
    qDebug() << "  Total apps:" << totalApps;
    qDebug() << "  Matched:" << matchedApps;
    qDebug() << "  Selected:" << selectedApps;
    qDebug() << "  Match rate:" << QString::number(matchRate, 'f', 1) << "%\n";
    
    if (selectedApps == 0) {
        qWarning() << "No apps selected for migration!";
        qDebug() << "Note: This is expected if no high-confidence matches found.";
        qDebug() << "Test completed successfully (no jobs to run).\n";
        return 0;
    }
    
    // Phase 5: Create worker
    qDebug() << "Phase 5: Creating AppMigrationWorker...";
    auto worker = std::make_shared<AppMigrationWorker>(chocoManager);
    
    // Connect signals
    int completedJobs = 0;
    int totalJobs = 0;
    
    QObject::connect(worker.get(), &AppMigrationWorker::migrationStarted,
                     [&](int total) {
        totalJobs = total;
        qDebug() << "\n✓ Migration started with" << total << "jobs\n";
    });
    
    QObject::connect(worker.get(), &AppMigrationWorker::jobStatusChanged,
                     [&](int /*entryIndex*/, const MigrationJob& job) {
        QString statusStr;
        switch (job.status) {
            case MigrationStatus::Queued: statusStr = "Queued"; break;
            case MigrationStatus::Installing: statusStr = "Installing"; break;
            case MigrationStatus::Success: statusStr = "✓ Success"; completedJobs++; break;
            case MigrationStatus::Failed: statusStr = "✗ Failed"; completedJobs++; break;
            case MigrationStatus::Cancelled: statusStr = "Cancelled"; break;
            default: statusStr = "Unknown"; break;
        }
        
        qDebug() << QString("  [%1/%2] %3 → %4")
                    .arg(completedJobs)
                    .arg(totalJobs)
                    .arg(job.appName)
                    .arg(statusStr);
        
        if (job.status == MigrationStatus::Failed && !job.errorMessage.isEmpty()) {
            qDebug() << "    Error:" << job.errorMessage;
        }
    });
    
    QObject::connect(worker.get(), &AppMigrationWorker::jobProgress,
                     [](int /*entryIndex*/, const QString& message) {
        if (message.contains("Installing") || message.contains("Success") || message.contains("Failed")) {
            qDebug() << "    " << message;
        }
    });
    
    QObject::connect(worker.get(), &AppMigrationWorker::migrationPaused,
                     []() {
        qDebug() << "\n⏸ Migration paused\n";
    });
    
    QObject::connect(worker.get(), &AppMigrationWorker::migrationResumed,
                     []() {
        qDebug() << "\n▶ Migration resumed\n";
    });
    
    QObject::connect(worker.get(), &AppMigrationWorker::migrationCompleted,
                     [&](const AppMigrationWorker::Stats& finalStats) {
        qDebug() << "\n=== Migration Completed ===";
        qDebug() << "  Total jobs:" << finalStats.total;
        qDebug() << "  Success:" << finalStats.success;
        qDebug() << "  Failed:" << finalStats.failed;
        qDebug() << "  Cancelled:" << finalStats.cancelled;
        qDebug() << "  Success rate:" 
                 << QString::number(finalStats.total > 0 
                                   ? (finalStats.success * 100.0 / finalStats.total) 
                                   : 0.0, 'f', 1) << "%";
        qDebug() << "\n✓ Test completed successfully\n";
        
        app.quit();
    });
    
    // Phase 6: Start migration
    qDebug() << "Phase 6: Starting migration...";
    qDebug() << "WARNING: This will attempt to install real packages!";
    qDebug() << "         Press Ctrl+C to cancel within 5 seconds...\n";
    
    // Give user time to cancel
    QThread::sleep(5);
    
    qDebug() << "Starting migration with 2 concurrent jobs...\n";
    int jobsQueued = worker->startMigration(report, 2);
    
    if (jobsQueued == 0) {
        qWarning() << "No jobs were queued!";
        return 1;
    }
    
    // Test pause/resume after 10 seconds (if migration still running)
    QTimer::singleShot(10000, [&]() {
        if (worker->isRunning() && !worker->isPaused()) {
            qDebug() << "\nTesting pause...";
            worker->pause();
            
            // Resume after 3 seconds
            QTimer::singleShot(3000, [&]() {
                if (worker->isPaused()) {
                    qDebug() << "Testing resume...";
                    worker->resume();
                }
            });
        }
    });
    
    // Run event loop until migration completes
    return app.exec();
}
