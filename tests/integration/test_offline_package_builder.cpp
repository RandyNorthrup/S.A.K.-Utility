// Copyright (c) 2025-2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/**
 * @file test_offline_package_builder.cpp
 * @brief Integration test for the offline deployment package builder
 *
 * Exercises the full pipeline: version resolution -> download -> extract ->
 * parse script -> rewrite -> repack. Uses a small utility package list so CI
 * stays under Qt
 * Test's per-function watchdog. Requires a working Internet
 * connection.
 */

#include "sak/offline_deployment_worker.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStringList>
#include <QTemporaryDir>
#include <QtTest/QtTest>

// ============================================================================
// Test Constants
// ============================================================================

namespace {

constexpr int kBuildTimeoutMs = 240'000;  // Keep below Qt Test's 5-minute watchdog
constexpr int kMinSuccessCount = 3;       // At least 3 of 5 must succeed

/// The Office PC preset — same IDs as PackageListManager::buildOfficePreset()
const QVector<QPair<QString, QString>> kOfficePcPackages = {
    {"7zip", ""},
    {"vlc", ""},
    {"notepadplusplus", ""},
    {"greenshot", ""},
    {"everything", ""},
};

bool isTransientPackageServiceError(const QString& error) {
    return error.contains(QStringLiteral("Nupkg download failed"), Qt::CaseInsensitive) &&
           (error.contains(QStringLiteral("Error transferring"), Qt::CaseInsensitive) ||
            error.contains(QStringLiteral("server replied"), Qt::CaseInsensitive) ||
            error.contains(QStringLiteral("HTTP 503"), Qt::CaseInsensitive) ||
            error.contains(QStringLiteral("HTTP 504"), Qt::CaseInsensitive));
}

bool allPackageFailuresAreTransient(const QHash<QString, QString>& package_errors) {
    const QStringList errors = package_errors.values();
    return !errors.isEmpty() &&
           std::all_of(errors.cbegin(), errors.cend(), isTransientPackageServiceError);
}

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
    struct WorkerDiagnosticsContext {
        QEventLoop* loop;
        sak::BatchStats* final_stats;
        bool* operation_finished;
        QString* operation_error;
        QHash<QString, QString>* package_errors;
    };

    void connectWorkerDiagnostics(sak::OfflineDeploymentWorker* worker,
                                  const WorkerDiagnosticsContext& context);
    void verifyManifest(const sak::BatchStats& final_stats) const;
    void verifyPackageFiles(const sak::BatchStats& final_stats) const;
    void logFinalSummary(const sak::BatchStats& final_stats) const;
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

void TestOfflinePackageBuilder::connectWorkerDiagnostics(sak::OfflineDeploymentWorker* worker,
                                                         const WorkerDiagnosticsContext& context) {
    connect(worker, &sak::OfflineDeploymentWorker::logMessage, this, [](const QString& message) {
        qInfo().noquote() << message;
    });
    connect(worker,
            &sak::OfflineDeploymentWorker::packageProgress,
            this,
            [context](const QString& package_id, bool success, const QString& message) {
                if (!success) {
                    (*context.package_errors)[package_id] = message;
                }
                qInfo().noquote()
                    << QString("[%1] %2: %3").arg(success ? "OK" : "FAIL", package_id, message);
            });
    connect(worker,
            &sak::OfflineDeploymentWorker::operationCompleted,
            context.loop,
            [context](const sak::BatchStats& stats) {
                *context.final_stats = stats;
                *context.operation_finished = true;
                context.loop->quit();
            });
    connect(worker,
            &sak::OfflineDeploymentWorker::operationError,
            context.loop,
            [context](const QString& error) {
                *context.operation_error = error;
                *context.operation_finished = true;
                context.loop->quit();
            });
}

void TestOfflinePackageBuilder::verifyManifest(const sak::BatchStats& final_stats) const {
    const QString manifest_path = m_output_dir.path() + "/manifest.json";
    QVERIFY2(QFileInfo::exists(manifest_path),
             qPrintable("Manifest not found at: " + manifest_path));

    QFile manifest_file(manifest_path);
    QVERIFY(manifest_file.open(QIODevice::ReadOnly));
    QJsonDocument manifest_doc = QJsonDocument::fromJson(manifest_file.readAll());
    QVERIFY2(!manifest_doc.isNull(), "Manifest is not valid JSON");

    QJsonObject manifest_obj = manifest_doc.object();
    QVERIFY(manifest_obj.contains("packages"));
    QVERIFY(manifest_obj.contains("manifest_version"));
    QVERIFY(manifest_obj["packages"].isArray());
    QCOMPARE(manifest_obj["packages"].toArray().size(), final_stats.completed);
}

void TestOfflinePackageBuilder::verifyPackageFiles(const sak::BatchStats& final_stats) const {
    QDir pkg_dir(m_output_dir.path() + "/packages");
    QStringList nupkg_files = pkg_dir.entryList({"*.nupkg"}, QDir::Files);
    QVERIFY2(nupkg_files.size() >= final_stats.completed,
             qPrintable(QString("Expected >= %1 nupkg files, found %2")
                            .arg(final_stats.completed)
                            .arg(nupkg_files.size())));

    for (const QString& nupkg : nupkg_files) {
        QFileInfo info(pkg_dir.filePath(nupkg));
        QVERIFY2(info.size() > 0, qPrintable(QString("Empty nupkg: %1").arg(nupkg)));
    }
}

void TestOfflinePackageBuilder::logFinalSummary(const sak::BatchStats& final_stats) const {
    qInfo().noquote() << QString("Result: %1/%2 succeeded, %3 failed")
                             .arg(final_stats.completed)
                             .arg(final_stats.total)
                             .arg(final_stats.failed);
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

    QHash<QString, QString> package_errors;
    connectWorkerDiagnostics(
        &worker, {&loop, &final_stats, &operation_finished, &operation_error, &package_errors});

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
    if (final_stats.completed == 0 && allPackageFailuresAreTransient(package_errors)) {
        QSKIP("Chocolatey package service/CDN returned only transient transfer errors.");
    }
    QVERIFY2(final_stats.completed > 0, "No packages completed successfully");

    verifyManifest(final_stats);
    verifyPackageFiles(final_stats);

    logFinalSummary(final_stats);

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
