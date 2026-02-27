// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file test_diagnostic_report_generator.cpp
/// @brief Unit tests for DiagnosticReportGenerator HTML, JSON, and CSV output

#include <QtTest/QtTest>

#include "sak/diagnostic_report_generator.h"
#include "sak/diagnostic_types.h"

#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>

using namespace sak;

class DiagnosticReportGeneratorTests : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void generatesHtmlReport();
    void generatesJsonReport();
    void generatesCsvReport();
    void emptyDataGeneratesValidReports();
    void htmlContainsKeyData();
    void jsonContainsStructuredData();
    void csvContainsHeaders();

private:
    DiagnosticReportData createSampleData();
};

DiagnosticReportData DiagnosticReportGeneratorTests::createSampleData()
{
    DiagnosticReportData data;
    data.technician_name = "Test Tech";
    data.ticket_number = "TICKET-001";
    data.notes = "Test notes";
    data.report_timestamp = QDateTime(QDate(2024, 6, 15), QTime(14, 30, 0));
    data.overall_status = DiagnosticStatus::AllPassed;

    // Hardware inventory
    data.inventory.cpu.name = "Intel Core i7-13700K";
    data.inventory.cpu.cores = 16;
    data.inventory.cpu.threads = 24;
    data.inventory.memory.total_bytes = 32ULL * 1024 * 1024 * 1024;
    data.inventory.os_name = "Windows 11 Pro";
    data.inventory.os_version = "23H2";
    data.inventory.scan_timestamp = data.report_timestamp;

    // CPU benchmark
    CpuBenchmarkResult cpu;
    cpu.single_thread_score = 1200;
    cpu.multi_thread_score = 15000;
    cpu.thread_count = 24;
    data.cpu_benchmark = cpu;

    // Disk benchmark
    DiskBenchmarkResult disk;
    disk.drive_path = "C:\\";
    disk.seq_read_mbps = 5500.0;
    disk.seq_write_mbps = 4800.0;
    disk.rand_4k_read_iops = 750000.0;
    disk.overall_score = 950;
    data.disk_benchmark = disk;

    // Memory benchmark
    MemoryBenchmarkResult mem;
    mem.read_bandwidth_gbps = 45.0;
    mem.write_bandwidth_gbps = 42.0;
    mem.random_latency_ns = 55.0;
    mem.overall_score = 1100;
    data.memory_benchmark = mem;

    return data;
}

void DiagnosticReportGeneratorTests::generatesHtmlReport()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    DiagnosticReportGenerator gen;
    gen.setReportData(createSampleData());

    const QString path = tempDir.filePath("report.html");
    QVERIFY(gen.generateHtml(path));
    QVERIFY(QFile::exists(path));

    QFile file(path);
    QVERIFY(file.open(QIODevice::ReadOnly));
    const auto content = file.readAll();
    QVERIFY(content.size() > 100);
}

void DiagnosticReportGeneratorTests::generatesJsonReport()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    DiagnosticReportGenerator gen;
    gen.setReportData(createSampleData());

    const QString path = tempDir.filePath("report.json");
    QVERIFY(gen.generateJson(path));
    QVERIFY(QFile::exists(path));

    QFile file(path);
    QVERIFY(file.open(QIODevice::ReadOnly));
    const auto json = QJsonDocument::fromJson(file.readAll());
    QVERIFY(!json.isNull());
    QVERIFY(json.isObject());
}

void DiagnosticReportGeneratorTests::generatesCsvReport()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    DiagnosticReportGenerator gen;
    gen.setReportData(createSampleData());

    const QString path = tempDir.filePath("report.csv");
    QVERIFY(gen.generateCsv(path));
    QVERIFY(QFile::exists(path));

    QFile file(path);
    QVERIFY(file.open(QIODevice::ReadOnly | QIODevice::Text));
    const auto content = file.readAll();
    QVERIFY(content.contains(","));  // CSV has commas
}

void DiagnosticReportGeneratorTests::emptyDataGeneratesValidReports()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    DiagnosticReportGenerator gen;
    DiagnosticReportData empty;
    gen.setReportData(empty);

    QVERIFY(gen.generateHtml(tempDir.filePath("empty.html")));
    QVERIFY(gen.generateJson(tempDir.filePath("empty.json")));
    QVERIFY(gen.generateCsv(tempDir.filePath("empty.csv")));
}

void DiagnosticReportGeneratorTests::htmlContainsKeyData()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    DiagnosticReportGenerator gen;
    gen.setReportData(createSampleData());

    const QString path = tempDir.filePath("report.html");
    QVERIFY(gen.generateHtml(path));

    QFile file(path);
    QVERIFY(file.open(QIODevice::ReadOnly | QIODevice::Text));
    const QString content = QString::fromUtf8(file.readAll());

    QVERIFY(content.contains("Intel Core i7-13700K"));
    QVERIFY(content.contains("Test Tech"));
    QVERIFY(content.contains("TICKET-001"));
}

void DiagnosticReportGeneratorTests::jsonContainsStructuredData()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    DiagnosticReportGenerator gen;
    gen.setReportData(createSampleData());

    const QString path = tempDir.filePath("report.json");
    QVERIFY(gen.generateJson(path));

    QFile file(path);
    QVERIFY(file.open(QIODevice::ReadOnly));
    const auto doc = QJsonDocument::fromJson(file.readAll());
    const auto root = doc.object();

    // Verify key sections exist
    QVERIFY(root.contains("metadata") || root.contains("technician") ||
            root.contains("hardware") || root.contains("report"));
}

void DiagnosticReportGeneratorTests::csvContainsHeaders()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    DiagnosticReportGenerator gen;
    gen.setReportData(createSampleData());

    const QString path = tempDir.filePath("report.csv");
    QVERIFY(gen.generateCsv(path));

    QFile file(path);
    QVERIFY(file.open(QIODevice::ReadOnly | QIODevice::Text));
    const QString content = QString::fromUtf8(file.readAll());

    // CSV should have at least a header line
    QVERIFY(content.contains("\n"));
}

QTEST_MAIN(DiagnosticReportGeneratorTests)
#include "test_diagnostic_report_generator.moc"
