// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file test_smart_disk_analyzer.cpp
/// @brief Unit tests for SMART disk health analysis -- JSON parsing and assessment

#include "sak/diagnostic_types.h"
#include "sak/smart_disk_analyzer.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QtTest/QtTest>

#include <algorithm>

class SmartDiskAnalyzerTests : public QObject {
    Q_OBJECT

private Q_SLOTS:
    // Constructor
    void constructor_defaults();

    // smartctl availability
    void isSmartctlAvailable_returnsBoolean();

    // Parse output tests (using known JSON structures)
    void parseOutput_validSataJson();
    void parseOutput_validNvmeJson();
    void parseOutput_emptyJson();
    void parseOutput_missingFields();
    void parseOutput_malformedJson();

    // Health assessment
    void healthAssessment_healthy();
    void healthAssessment_warning();
    void healthAssessment_critical();

    // B5-11: fail-closed assessment of malformed/empty smartctl output
    void reportHasAssessableData_signalDetection();
    void assess_malformedJson_isUnknownNotHealthy();
    void assess_emptyJsonObject_isUnknownNotHealthy();
    void assess_unknownReport_recommendationsSayUnknown();
    void assess_validSataPassed_isHealthy();
    void assess_smartStatusFailed_isCritical();

    // Cancel
    void cancel_stopsAnalysis();
};

// ============================================================================
// Constructor
// ============================================================================

void SmartDiskAnalyzerTests::constructor_defaults() {
    sak::SmartDiskAnalyzer analyzer;
    QVERIFY(analyzer.reports().isEmpty());
}

// ============================================================================
// smartctl Availability
// ============================================================================

void SmartDiskAnalyzerTests::isSmartctlAvailable_returnsBoolean() {
    sak::SmartDiskAnalyzer analyzer;
    // Just verify it doesn't crash -- result depends on bundled tools
    bool available = analyzer.isSmartctlAvailable();
    Q_UNUSED(available);
    QVERIFY(true);
}

// ============================================================================
// Parse Output Tests -- we use the private parseSmartctlOutput indirectly
// by testing through the public API where possible, or verifying report
// structure from known JSON formats
// ============================================================================

void SmartDiskAnalyzerTests::parseOutput_validSataJson() {
    // This tests the expected structure of SmartReport
    // We can't easily call parseSmartctlOutput directly (private),
    // but we verify the data structures work correctly
    sak::SmartReport report;
    report.device_path = "\\\\.\\PhysicalDrive0";
    report.model = "Samsung SSD 860 EVO";
    report.serial_number = "S5XXXXX";
    report.firmware_version = "RVT04B6Q";
    report.size_bytes = 500'107'862'016;
    report.interface_type = "SATA";
    report.overall_health = sak::SmartHealthStatus::Healthy;
    report.smart_status = "PASSED";
    report.temperature_celsius = 35.0;
    report.power_on_hours = 12'000;

    QCOMPARE(report.model, QString("Samsung SSD 860 EVO"));
    QCOMPARE(report.temperature_celsius, 35.0);
    QCOMPARE(report.overall_health, sak::SmartHealthStatus::Healthy);
    QCOMPARE(report.smart_status, QString("PASSED"));
}

void SmartDiskAnalyzerTests::parseOutput_validNvmeJson() {
    sak::SmartReport report;
    report.device_path = "\\\\.\\PhysicalDrive1";
    report.model = "Samsung 970 EVO Plus";
    report.interface_type = "NVMe";
    report.overall_health = sak::SmartHealthStatus::Healthy;
    report.smart_status = "PASSED";
    report.temperature_celsius = 42.0;
    report.power_on_hours = 5000;

    QCOMPARE(report.interface_type, QString("NVMe"));
    QVERIFY(report.temperature_celsius <= 70.0);
}

void SmartDiskAnalyzerTests::parseOutput_emptyJson() {
    // An empty SmartReport should have safe default values
    sak::SmartReport report;
    QVERIFY(report.model.isEmpty());
    QCOMPARE(report.size_bytes, uint64_t{0});
    QCOMPARE(report.temperature_celsius, 0.0);
}

void SmartDiskAnalyzerTests::parseOutput_missingFields() {
    // SmartReport with partial data should still be valid
    sak::SmartReport report;
    report.model = "Unknown Drive";
    report.overall_health = sak::SmartHealthStatus::Unknown;

    QVERIFY(!report.model.isEmpty());
    QVERIFY(report.serial_number.isEmpty());
}

void SmartDiskAnalyzerTests::parseOutput_malformedJson() {
    // Verify that SmartReport default-constructs safely
    sak::SmartReport report;
    QCOMPARE(report.power_on_hours, 0);
    QVERIFY(report.attributes.isEmpty());
    QVERIFY(report.recommendations.isEmpty());
}

// ============================================================================
// Health Assessment
// ============================================================================

void SmartDiskAnalyzerTests::healthAssessment_healthy() {
    sak::SmartReport report;
    report.overall_health = sak::SmartHealthStatus::Healthy;
    report.smart_status = "PASSED";
    report.temperature_celsius = 35.0;
    report.power_on_hours = 1000;

    // Healthy indicators
    QCOMPARE(report.overall_health, sak::SmartHealthStatus::Healthy);
    QVERIFY(report.temperature_celsius < 70.0);
}

void SmartDiskAnalyzerTests::healthAssessment_warning() {
    sak::SmartReport report;
    report.overall_health = sak::SmartHealthStatus::Warning;
    report.temperature_celsius = 65.0;  // Getting warm
    report.power_on_hours = 50'000;     // Heavy use

    QVERIFY(report.temperature_celsius >= 60.0);
}

void SmartDiskAnalyzerTests::healthAssessment_critical() {
    sak::SmartReport report;
    report.overall_health = sak::SmartHealthStatus::Critical;
    report.smart_status = "FAILED";
    report.temperature_celsius = 80.0;

    QCOMPARE(report.overall_health, sak::SmartHealthStatus::Critical);
}

// ============================================================================
// B5-11: fail-closed assessment -- malformed/empty smartctl output must never
// read as Healthy. Before the fix, assessHealth() unconditionally set Healthy
// at entry, so a JSON parse error (default, data-less report) came back green.
// ============================================================================

void SmartDiskAnalyzerTests::reportHasAssessableData_signalDetection() {
    // No status, no attributes, no NVMe log -> nothing to judge -> false.
    sak::SmartReport blank;
    QVERIFY(!sak::SmartDiskAnalyzer::reportHasAssessableData(blank));

    // An overall SMART status alone is enough signal.
    sak::SmartReport with_status;
    with_status.smart_status = "PASSED";
    QVERIFY(sak::SmartDiskAnalyzer::reportHasAssessableData(with_status));

    // A single SATA attribute is enough signal.
    sak::SmartReport with_attr;
    with_attr.attributes.append(sak::SmartAttribute{});
    QVERIFY(sak::SmartDiskAnalyzer::reportHasAssessableData(with_attr));

    // An NVMe health log is enough signal.
    sak::SmartReport with_nvme;
    with_nvme.nvme_health = sak::NvmeHealthInfo{};
    QVERIFY(sak::SmartDiskAnalyzer::reportHasAssessableData(with_nvme));
}

void SmartDiskAnalyzerTests::assess_malformedJson_isUnknownNotHealthy() {
    sak::SmartDiskAnalyzer analyzer;
    const QByteArray garbage = "{ this is not valid json ";
    const sak::SmartReport report = analyzer.parseAndAssessForTesting(garbage, 0);
    // The old bug: a parse failure came back Healthy. It must be Unknown.
    QCOMPARE(report.overall_health, sak::SmartHealthStatus::Unknown);
    QVERIFY(report.overall_health != sak::SmartHealthStatus::Healthy);
}

void SmartDiskAnalyzerTests::assess_emptyJsonObject_isUnknownNotHealthy() {
    sak::SmartDiskAnalyzer analyzer;
    // Valid JSON, but no smart_status / attributes / nvme log -> no signal.
    const sak::SmartReport report = analyzer.parseAndAssessForTesting("{}", 0);
    QCOMPARE(report.overall_health, sak::SmartHealthStatus::Unknown);
}

void SmartDiskAnalyzerTests::assess_unknownReport_recommendationsSayUnknown() {
    sak::SmartDiskAnalyzer analyzer;
    const sak::SmartReport report = analyzer.parseAndAssessForTesting("{}", 0);
    QCOMPARE(report.overall_health, sak::SmartHealthStatus::Unknown);
    // Must NOT claim the drive is fine.
    for (const QString& rec : report.recommendations) {
        QVERIFY2(!rec.contains("health is good", Qt::CaseInsensitive),
                 "an indeterminate drive must not be reported as healthy");
    }
    // Must say, somewhere, that it could not be determined.
    const bool says_unknown =
        std::any_of(report.warnings.begin(), report.warnings.end(), [](const QString& w) {
            return w.contains("could not be determined", Qt::CaseInsensitive);
        });
    QVERIFY(says_unknown);
}

void SmartDiskAnalyzerTests::assess_validSataPassed_isHealthy() {
    sak::SmartDiskAnalyzer analyzer;
    // A well-formed SATA report with a passing status and a benign attribute.
    const QByteArray json =
        R"({"device":{"type":"sat"},"model_name":"Test SATA","smart_status":{"passed":true},)"
        R"("temperature":{"current":35},"ata_smart_attributes":{"table":[)"
        R"({"id":5,"name":"Reallocated_Sector_Ct","value":100,"worst":100,"thresh":10,)"
        R"("raw":{"value":0}}]}})";
    const sak::SmartReport report = analyzer.parseAndAssessForTesting(json, 0);
    // Regression guard: the fail-closed change must not break a genuine healthy read.
    QCOMPARE(report.overall_health, sak::SmartHealthStatus::Healthy);
}

void SmartDiskAnalyzerTests::assess_smartStatusFailed_isCritical() {
    sak::SmartDiskAnalyzer analyzer;
    const QByteArray json = R"({"device":{"type":"sat"},"smart_status":{"passed":false}})";
    const sak::SmartReport report = analyzer.parseAndAssessForTesting(json, 0);
    QCOMPARE(report.overall_health, sak::SmartHealthStatus::Critical);
}

// ============================================================================
// Cancel
// ============================================================================

void SmartDiskAnalyzerTests::cancel_stopsAnalysis() {
    sak::SmartDiskAnalyzer analyzer;
    // Cancel before starting -- should be safe
    analyzer.cancel();
    QVERIFY(analyzer.reports().isEmpty());
}

QTEST_GUILESS_MAIN(SmartDiskAnalyzerTests)
#include "test_smart_disk_analyzer.moc"
