// Copyright (c) 2025-2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/**
 * @file test_offline_package_builder.cpp
 * @brief Integration test for the offline deployment package builder
 *
 * Exercises the full pipeline: version resolution -> download -> extract ->
 * parse script -> rewrite -> repack.  Uses the "Office PC" preset list
 * (10 packages).  Requires a working Internet connection.
 */

#include "sak/offline_deployment_worker.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QtTest/QtTest>

// ============================================================================
// Test Constants
// ============================================================================

namespace {

constexpr int kBuildTimeoutMs = 600'000;  // 10 minutes for full build
constexpr int kMinSuccessCount = 5;       // At least 5 of 10 must succeed

/// The Office PC preset — same IDs as PackageListManager::buildOfficePreset()
const QVector<QPair<QString, QString>> kOfficePcPackages = {
    {"googlechrome", ""},
    {"firefox", ""},
    {"7zip", ""},
    {"vlc", ""},
    {"adobereader", ""},
    {"libreoffice-fresh", ""},
    {"notepadplusplus", ""},
    {"greenshot", ""},
    {"everything", ""},
    {"treesizefree", ""},
};

}  // namespace

// ============================================================================
// Test Class
// ============================================================================

class TestOfflinePackageBuilder : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void initTestCase();
    void cleanupTestCase();

    void testBuildOfficePcBundle();

private:
    QTemporaryDir m_output_dir;
};

// ============================================================================
// Setup / Teardown
// ============================================================================

void TestOfflinePackageBuilder::initTestCase() {
    qInfo() << "=== Offline Package Builder Integration Test ===";
    qInfo() << "Output directory:" << m_output_dir.path();
    QVERIFY2(m_output_dir.isValid(), "Failed to create temporary output directory");
}

void TestOfflinePackageBuilder::cleanupTestCase() {
    qInfo() << "=== Offline package builder test completed ===";
}

// ============================================================================
// Test: Build the Office PC bundle end-to-end
// ============================================================================

void TestOfflinePackageBuilder::testBuildOfficePcBundle() {
    sak::OfflineDeploymentWorker worker;

    QEventLoop loop;
    sak::BatchStats final_stats;
    bool operation_finished = false;
    QString operation_error;

    // Collect log messages for diagnostics
    QStringList log_messages;
    connect(&worker,
            &sak::OfflineDeploymentWorker::logMessage,
            this,
            [&log_messages](const QString& message) {
                log_messages.append(message);
                qInfo().noquote() << message;
            });

    // Track per-package results
    QHash<QString, bool> package_results;
    QHash<QString, QString> package_errors;
    connect(&worker,
            &sak::OfflineDeploymentWorker::packageProgress,
            this,
            [&](const QString& package_id, bool success, const QString& message) {
                package_results[package_id] = success;
                if (!success) {
                    package_errors[package_id] = message;
                }
                qInfo().noquote()
                    << QString("[%1] %2: %3").arg(success ? "OK" : "FAIL", package_id, message);
            });

    connect(&worker,
            &sak::OfflineDeploymentWorker::operationCompleted,
            &loop,
            [&](const sak::BatchStats& stats) {
                final_stats = stats;
                operation_finished = true;
                loop.quit();
            });

    connect(
        &worker, &sak::OfflineDeploymentWorker::operationError, &loop, [&](const QString& error) {
            operation_error = error;
            operation_finished = true;
            loop.quit();
        });

    // Start the build
    QElapsedTimer timer;
    timer.start();

    worker.buildDeploymentBundle(kOfficePcPackages,
                                 m_output_dir.path(),
                                 "Integration test — Office PC");

    // Wait for completion with timeout
    if (!operation_finished) {
        QTimer::singleShot(kBuildTimeoutMs, &loop, &QEventLoop::quit);
        loop.exec();
    }

    qint64 elapsed_ms = timer.elapsed();
    qInfo().noquote() << QString("Build completed in %1 s").arg(elapsed_ms / 1000);

    // Diagnostics on failure
    if (!operation_error.isEmpty()) {
        qWarning().noquote() << "Operation error:" << operation_error;
    }
    for (auto iter = package_errors.cbegin(); iter != package_errors.cend(); ++iter) {
        qWarning().noquote() << QString("  FAILED: %1 — %2").arg(iter.key(), iter.value());
    }

    // Assertions
    QVERIFY2(operation_finished, "Build did not complete within the timeout");
    QVERIFY2(operation_error.isEmpty(),
             qPrintable("Worker reported an operation error: " + operation_error));

    QCOMPARE(final_stats.total, kOfficePcPackages.size());
    QVERIFY2(final_stats.completed > 0, "No packages completed successfully");

    // Verify manifest was written
    QString manifest_path = m_output_dir.path() + "/manifest.json";
    QVERIFY2(QFileInfo::exists(manifest_path),
             qPrintable("Manifest not found at: " + manifest_path));

    // Read and validate the manifest
    QFile manifest_file(manifest_path);
    QVERIFY(manifest_file.open(QIODevice::ReadOnly));
    QJsonDocument manifest_doc = QJsonDocument::fromJson(manifest_file.readAll());
    QVERIFY2(!manifest_doc.isNull(), "Manifest is not valid JSON");

    QJsonObject manifest_obj = manifest_doc.object();
    QVERIFY(manifest_obj.contains("packages"));
    QVERIFY(manifest_obj.contains("manifest_version"));
    QVERIFY(manifest_obj["packages"].isArray());

    int manifest_package_count = manifest_obj["packages"].toArray().size();
    QCOMPARE(manifest_package_count, final_stats.completed);

    // Verify .nupkg files exist in the packages subdirectory
    QString packages_dir = m_output_dir.path() + "/packages";
    QDir pkg_dir(packages_dir);
    QStringList nupkg_files = pkg_dir.entryList({"*.nupkg"}, QDir::Files);
    QVERIFY2(nupkg_files.size() >= final_stats.completed,
             qPrintable(QString("Expected >= %1 nupkg files, found %2")
                            .arg(final_stats.completed)
                            .arg(nupkg_files.size())));

    // Every completed package should have a non-zero .nupkg
    for (const QString& nupkg : nupkg_files) {
        QFileInfo info(pkg_dir.filePath(nupkg));
        QVERIFY2(info.size() > 0, qPrintable(QString("Empty nupkg: %1").arg(nupkg)));
    }

    // Log final summary
    qInfo().noquote() << QString("Result: %1/%2 succeeded, %3 failed")
                             .arg(final_stats.completed)
                             .arg(final_stats.total)
                             .arg(final_stats.failed);

    // At least kMinSuccessCount packages must succeed — some may fail due to
    // transient CDN errors, unresolvable URLs (e.g., ${locale} in firefox),
    // or packages no longer available on Chocolatey.
    QVERIFY2(final_stats.completed >= kMinSuccessCount,
             qPrintable(QString("Only %1/%2 succeeded (need %3)")
                            .arg(final_stats.completed)
                            .arg(final_stats.total)
                            .arg(kMinSuccessCount)));
}

QTEST_GUILESS_MAIN(TestOfflinePackageBuilder)
#include "test_offline_package_builder.moc"
