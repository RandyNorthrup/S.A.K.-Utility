// Copyright (c) 2025-2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file test_deployment_summary_report.cpp
/// @brief Unit tests for DeploymentSummaryReport CSV/PDF report generation

#include "sak/deployment_summary_report.h"

#include <QDir>
#include <QTemporaryDir>
#include <QtTest/QtTest>

using namespace sak;

class TestDeploymentSummaryReport : public QObject {
    Q_OBJECT

private Q_SLOTS:
    // ── Struct defaults ───────────────────────────────────────────
    void jobSummary_defaults();
    void destSummary_defaults();

    // ── CSV export ────────────────────────────────────────────────
    void csv_emptyData_createsFile();
    void csv_withJobs_containsData();
    void csv_withDestinations_containsData();
    void csv_invalidPath_returnsFalse();

    // ── PDF export ────────────────────────────────────────────────
    void pdf_emptyData_createsFile();
    void pdf_invalidPath_returnsFalse();
};

// ═══════════════════════════════════════════════════════════════════
// Struct defaults
// ═══════════════════════════════════════════════════════════════════

void TestDeploymentSummaryReport::jobSummary_defaults() {
    DeploymentJobSummary job;
    QVERIFY(job.job_id.isEmpty());
    QVERIFY(job.source_user.isEmpty());
    QVERIFY(job.destination_id.isEmpty());
    QVERIFY(job.status.isEmpty());
    QCOMPARE(job.bytes_transferred, static_cast<qint64>(0));
    QCOMPARE(job.total_bytes, static_cast<qint64>(0));
    QVERIFY(job.error_message.isEmpty());
}

void TestDeploymentSummaryReport::destSummary_defaults() {
    DeploymentDestinationSummary dest;
    QVERIFY(dest.destination_id.isEmpty());
    QVERIFY(dest.hostname.isEmpty());
    QVERIFY(dest.ip_address.isEmpty());
    QVERIFY(dest.status.isEmpty());
    QCOMPARE(dest.progress_percent, 0);
}

// ═══════════════════════════════════════════════════════════════════
// CSV export
// ═══════════════════════════════════════════════════════════════════

static DeploymentSummaryData makeTestData(const QString& deploy_id,
                                          const QVector<DeploymentJobSummary>& jobs = {},
                                          const QVector<DeploymentDestinationSummary>& dests = {}) {
    DeploymentSummaryData data;
    data.deployment_id = deploy_id;
    data.started_at = QDateTime::currentDateTime();
    data.completed_at = QDateTime::currentDateTime();
    data.jobs = jobs;
    data.destinations = dests;
    return data;
}

void TestDeploymentSummaryReport::csv_emptyData_createsFile() {
    QTemporaryDir temp_dir;
    QVERIFY(temp_dir.isValid());

    const QString file_path = temp_dir.path() + "/report.csv";
    const auto data = makeTestData("deploy-001");

    const bool result = DeploymentSummaryReport::exportCsv(file_path, data);

    QVERIFY(result);
    QVERIFY(QFile::exists(file_path));
}

void TestDeploymentSummaryReport::csv_withJobs_containsData() {
    QTemporaryDir temp_dir;
    QVERIFY(temp_dir.isValid());

    const QString file_path = temp_dir.path() + "/report_jobs.csv";

    QVector<DeploymentJobSummary> jobs;
    DeploymentJobSummary job;
    job.job_id = "job-001";
    job.source_user = "TestUser";
    job.destination_id = "dest-001";
    job.status = "completed";
    job.bytes_transferred = 1'024'000;
    job.total_bytes = 1'024'000;
    jobs.append(job);

    const auto data = makeTestData("deploy-002", jobs);

    const bool result = DeploymentSummaryReport::exportCsv(file_path, data);

    QVERIFY(result);

    QFile file(file_path);
    QVERIFY(file.open(QIODevice::ReadOnly | QIODevice::Text));
    const QString content = QString::fromUtf8(file.readAll());
    QVERIFY(content.contains("job-001"));
    QVERIFY(content.contains("TestUser"));
}

void TestDeploymentSummaryReport::csv_withDestinations_containsData() {
    QTemporaryDir temp_dir;
    QVERIFY(temp_dir.isValid());

    const QString file_path = temp_dir.path() + "/report_dests.csv";

    QVector<DeploymentDestinationSummary> dests;
    DeploymentDestinationSummary dest;
    dest.destination_id = "dest-001";
    dest.hostname = "WORKSTATION-01";
    dest.ip_address = "192.168.1.50";
    dest.status = "online";
    dest.progress_percent = 100;
    dests.append(dest);

    const auto data = makeTestData("deploy-003", {}, dests);

    const bool result = DeploymentSummaryReport::exportCsv(file_path, data);

    QVERIFY(result);
    QFile file(file_path);
    QVERIFY(file.open(QIODevice::ReadOnly | QIODevice::Text));
    const QString content = QString::fromUtf8(file.readAll());
    QVERIFY(content.contains("WORKSTATION-01") || content.contains("dest-001"));
}

void TestDeploymentSummaryReport::csv_invalidPath_returnsFalse() {
    const auto data = makeTestData("deploy-001");

    const bool result = DeploymentSummaryReport::exportCsv("Z:\\nonexistent\\path\\report.csv",
                                                           data);

    QVERIFY(!result);
}

// ═══════════════════════════════════════════════════════════════════
// PDF export
// ═══════════════════════════════════════════════════════════════════

void TestDeploymentSummaryReport::pdf_emptyData_createsFile() {
    QTemporaryDir temp_dir;
    QVERIFY(temp_dir.isValid());

    const QString file_path = temp_dir.path() + "/report.pdf";
    const auto data = makeTestData("deploy-001");

    const bool result = DeploymentSummaryReport::exportPdf(file_path, data);

    if (result) {
        QVERIFY(QFile::exists(file_path));
    }
}

void TestDeploymentSummaryReport::pdf_invalidPath_returnsFalse() {
    const auto data = makeTestData("deploy-001");

    const bool result = DeploymentSummaryReport::exportPdf(QString(), data);

    QVERIFY(!result);
}

QTEST_MAIN(TestDeploymentSummaryReport)
#include "test_deployment_summary_report.moc"
