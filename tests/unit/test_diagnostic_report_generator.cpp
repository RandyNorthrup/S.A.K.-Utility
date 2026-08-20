// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file test_diagnostic_report_generator.cpp
/// @brief Unit tests for DiagnosticReportGenerator HTML, JSON, and CSV output

#include "sak/diagnostic_report_generator.h"
#include "sak/diagnostic_types.h"

#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QtTest/QtTest>

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
    void csvNeutralizesFormulaInjection();

private:
    DiagnosticReportData createSampleData();
};

DiagnosticReportData DiagnosticReportGeneratorTests::createSampleData() {
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
    cpu.multi_thread_score = 15'000;
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

void DiagnosticReportGeneratorTests::generatesHtmlReport() {
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

void DiagnosticReportGeneratorTests::generatesJsonReport() {
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

void DiagnosticReportGeneratorTests::generatesCsvReport() {
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
    // The CSV opens with the fixed header then the first sample data row (content is a QByteArray).
    QVERIFY(content.startsWith("Section,Property,Value\n"));
    QVERIFY(content.contains("CPU,Name,Intel Core i7-13700K"));
}

void DiagnosticReportGeneratorTests::emptyDataGeneratesValidReports() {
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    DiagnosticReportGenerator gen;
    DiagnosticReportData empty;
    gen.setReportData(empty);

    QVERIFY(gen.generateHtml(tempDir.filePath("empty.html")));
    QVERIFY(gen.generateJson(tempDir.filePath("empty.json")));
    QVERIFY(gen.generateCsv(tempDir.filePath("empty.csv")));
}

void DiagnosticReportGeneratorTests::htmlContainsKeyData() {
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

void DiagnosticReportGeneratorTests::jsonContainsStructuredData() {
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

    // metadata/hardware/smart/benchmarks/critical_issues/warnings/recommendations are all set
    // unconditionally; the old || chain was satisfied by "metadata" alone and would pass even if
    // the whole hardware payload were dropped. "technician" is nested under metadata (not top-
    // level) and "report" is never emitted -- both were decoy operands.
    QVERIFY(root.contains("metadata"));
    QVERIFY(root.contains("hardware"));
    QVERIFY(root.contains("smart"));
    QVERIFY(root.contains("benchmarks"));
    QVERIFY(root.contains("critical_issues"));
    QVERIFY(root.contains("warnings"));
    QVERIFY(root.contains("recommendations"));
    QVERIFY(!root.contains("technician"));
    QVERIFY(!root.contains("report"));
    QVERIFY(root["metadata"].toObject().contains("technician"));
}

void DiagnosticReportGeneratorTests::csvContainsHeaders() {
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    DiagnosticReportGenerator gen;
    gen.setReportData(createSampleData());

    const QString path = tempDir.filePath("report.csv");
    QVERIFY(gen.generateCsv(path));

    QFile file(path);
    QVERIFY(file.open(QIODevice::ReadOnly | QIODevice::Text));
    const QString content = QString::fromUtf8(file.readAll());

    // The CSV must lead with the exact header line, not merely contain some newline.
    QVERIFY(content.startsWith(QStringLiteral("Section,Property,Value\n")));
}

// P07-11: a hardware string beginning with a formula character must be neutralized (prefixed with
// an apostrophe) so opening the CSV in Excel/Calc does not execute it.
void DiagnosticReportGeneratorTests::csvNeutralizesFormulaInjection() {
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    DiagnosticReportData data = createSampleData();
    data.inventory.cpu.name = QStringLiteral("=HYPERLINK(\"http://evil\",\"x\")");

    DiagnosticReportGenerator gen;
    gen.setReportData(data);
    const QString path = tempDir.filePath("inject.csv");
    QVERIFY(gen.generateCsv(path));

    QFile file(path);
    QVERIFY(file.open(QIODevice::ReadOnly | QIODevice::Text));
    const QString content = QString::fromUtf8(file.readAll());

    // The raw formula (a cell starting with '=') must never appear; the value is quoted and
    // apostrophe-prefixed so the cell reads as text.
    // Pin the full RFC-4180 escaped cell: the old pair passes even on a broken quote-doubler,
    // since the raw formula (apostrophe-prefixed but unescaped) still contains '=HYPERLINK and
    // still lacks ,=HYPERLINK. The exact cell proves the apostrophe AND the quote-doubling.
    QVERIFY(content.contains(
        QStringLiteral("CPU,Name,\"'=HYPERLINK(\"\"http://evil\"\",\"\"x\"\")\"")));
}

QTEST_MAIN(DiagnosticReportGeneratorTests)
#include "test_diagnostic_report_generator.moc"
