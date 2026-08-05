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
    void cancel_beforeAnalysisIsSafe();
};

namespace {

/// Minimal well-formed SATA payload carrying exactly ONE ata_smart_attributes entry.
/// value/worst are healthy normalized numbers and thresh is 0, so SmartAttribute::failing
/// stays false and the only thing that can move overall_health is the attribute's RAW value
/// against the analyzer's per-id warning/critical ladder.
QByteArray sataAttributeJson(int id, const QString& name, qint64 raw) {
    return QStringLiteral(R"({"device":{"type":"sat"},"model_name":"Test SATA",)"
                          R"("smart_status":{"passed":true},"temperature":{"current":35},)"
                          R"("ata_smart_attributes":{"table":[{"id":%1,"name":"%2",)"
                          R"("value":100,"worst":100,"thresh":0,"raw":{"value":%3}}]}})")
        .arg(id)
        .arg(name)
        .arg(raw)
        .toUtf8();
}

sak::SmartHealthStatus healthOf(sak::SmartDiskAnalyzer& analyzer, const QByteArray& json) {
    return analyzer.parseAndAssessForTesting(json, 0).overall_health;
}

}  // namespace

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

// CRASH-REGRESSION TEST. The RETURN VALUE cannot be asserted here: it depends on whether the
// bundled smartmontools
// payload was deployed next to the test binary. What this test does claim is that the probe
// survives a build with NO bundled tools at all -- it first-touches the BundledToolsManager
// singleton and then spawns a child process, so a regression there aborts or hangs rather
// than returning false. The reports() check records the pure-query contract, but note it
// cannot fail on its own: isSmartctlAvailable() is const and m_reports is not mutable. The
// claim this test really carries is the absence of a crash or a hang.
void SmartDiskAnalyzerTests::isSmartctlAvailable_returnsBoolean() {
    sak::SmartDiskAnalyzer analyzer;
    const bool available = analyzer.isSmartctlAvailable();
    Q_UNUSED(available);
    QVERIFY(analyzer.reports().isEmpty());
}

// ============================================================================
// Parse Output Tests -- parseSmartctlOutput is private, so these drive it through
// parseAndAssessForTesting(), the public seam that runs the real
// parse -> assess -> recommend pipeline over raw smartctl JSON bytes.
// ============================================================================

void SmartDiskAnalyzerTests::parseOutput_validSataJson() {
    sak::SmartDiskAnalyzer analyzer;
    const QByteArray json =
        R"({"device":{"type":"sat"},"model_name":"Samsung SSD 860 EVO",)"
        R"("serial_number":"S5XXXXX","firmware_version":"RVT04B6Q",)"
        R"("user_capacity":{"bytes":500107862016},"smart_status":{"passed":true},)"
        R"("temperature":{"current":35},"power_on_time":{"hours":12000},)"
        R"("ata_smart_attributes":{"table":[)"
        R"({"id":5,"name":"Reallocated_Sector_Ct","value":100,"worst":100,"thresh":10,)"
        R"("flags":{"string":"PO--CK"},"raw":{"value":0}}]}})";
    const sak::SmartReport report = analyzer.parseAndAssessForTesting(json, 0);

    // Identity fields. "sat" is smartctl's SATA device type and is normalized to "SATA".
    QCOMPARE(report.device_path, QStringLiteral("\\\\.\\PhysicalDrive0"));
    QCOMPARE(report.model, QStringLiteral("Samsung SSD 860 EVO"));
    QCOMPARE(report.serial_number, QStringLiteral("S5XXXXX"));
    QCOMPARE(report.firmware_version, QStringLiteral("RVT04B6Q"));
    QCOMPARE(report.size_bytes, uint64_t{500'107'862'016});
    QCOMPARE(report.interface_type, QStringLiteral("SATA"));

    // Health fields.
    QCOMPARE(report.smart_status, QStringLiteral("PASSED"));
    QCOMPARE(report.temperature_celsius, 35.0);
    QCOMPARE(report.power_on_hours, static_cast<int64_t>(12'000));

    // The ATA attribute table, including the key-metric extraction for attribute 5.
    QCOMPARE(report.attributes.size(), 1);
    const sak::SmartAttribute& attr = report.attributes.first();
    QCOMPARE(static_cast<int>(attr.id), 5);
    QCOMPARE(attr.name, QStringLiteral("Reallocated_Sector_Ct"));
    QCOMPARE(static_cast<int>(attr.current_value), 100);
    QCOMPARE(static_cast<int>(attr.worst_value), 100);
    QCOMPARE(static_cast<int>(attr.threshold), 10);
    QCOMPARE(attr.flags, QStringLiteral("PO--CK"));
    QCOMPARE(attr.raw_value, static_cast<int64_t>(0));
    QVERIFY(!attr.failing);  // normalized 100 is far above the threshold of 10
    QCOMPARE(report.reallocated_sectors, static_cast<int64_t>(0));

    QVERIFY(!report.nvme_health.has_value());  // a SATA payload must not synthesize an NVMe log
    QCOMPARE(report.overall_health, sak::SmartHealthStatus::Healthy);
    QVERIFY(report.warnings.isEmpty());
}

void SmartDiskAnalyzerTests::parseOutput_validNvmeJson() {
    sak::SmartDiskAnalyzer analyzer;
    const QByteArray json =
        R"({"device":{"type":"nvme"},"model_name":"Samsung SSD 970 EVO Plus 1TB",)"
        R"("nvme_smart_health_information_log":{"percentage_used":3,"data_units_read":1000,)"
        R"("data_units_written":2000,"power_on_hours":5000,"unsafe_shutdowns":4,)"
        R"("media_errors":0,"num_err_log_entries":0,"temperature":42,)"
        R"("available_spare":100,"available_spare_threshold":10}})";
    const sak::SmartReport report = analyzer.parseAndAssessForTesting(json, 1);

    QCOMPARE(report.device_path, QStringLiteral("\\\\.\\PhysicalDrive1"));
    QCOMPARE(report.model, QStringLiteral("Samsung SSD 970 EVO Plus 1TB"));
    // device.type is upper-cased verbatim, so smartctl's "nvme" surfaces as "NVME".
    QCOMPARE(report.interface_type, QStringLiteral("NVME"));

    QVERIFY(report.nvme_health.has_value());
    const sak::NvmeHealthInfo& nvme = report.nvme_health.value();
    QCOMPARE(static_cast<int>(nvme.percentage_used), 3);
    QCOMPARE(nvme.data_units_read, uint64_t{1000});
    QCOMPARE(nvme.data_units_written, uint64_t{2000});
    QCOMPARE(nvme.power_on_hours, uint64_t{5000});
    QCOMPARE(nvme.unsafe_shutdowns, uint32_t{4});
    QCOMPARE(nvme.media_errors, uint32_t{0});
    QCOMPARE(static_cast<int>(nvme.temperature), 42);
    QCOMPARE(static_cast<int>(nvme.available_spare), 100);
    QCOMPARE(static_cast<int>(nvme.available_spare_threshold), 10);

    // The NVMe log also feeds the unified cross-interface metrics.
    QCOMPARE(report.power_on_hours, static_cast<int64_t>(5000));
    QCOMPARE(report.temperature_celsius, 42.0);
    QCOMPARE(report.wear_level_percent, 3.0);

    QVERIFY(report.attributes.isEmpty());  // no ATA table on an NVMe payload
    QCOMPARE(report.overall_health, sak::SmartHealthStatus::Healthy);
    QVERIFY(report.warnings.isEmpty());
}

// A syntactically valid but content-free payload must yield DEFAULTS, not invented values --
// and must still name the drive it failed to read.
void SmartDiskAnalyzerTests::parseOutput_emptyJson() {
    sak::SmartDiskAnalyzer analyzer;
    const sak::SmartReport report = analyzer.parseAndAssessForTesting("{}", 2);

    QCOMPARE(report.device_path, QStringLiteral("\\\\.\\PhysicalDrive2"));
    QVERIFY(report.model.isEmpty());
    QVERIFY(report.serial_number.isEmpty());
    QVERIFY(report.firmware_version.isEmpty());
    QVERIFY(report.interface_type.isEmpty());
    QVERIFY(report.smart_status.isEmpty());
    QCOMPARE(report.size_bytes, uint64_t{0});
    QCOMPARE(report.temperature_celsius, 0.0);
    QCOMPARE(report.power_on_hours, static_cast<int64_t>(0));
    QVERIFY(report.attributes.isEmpty());
    QVERIFY(!report.nvme_health.has_value());
    QCOMPARE(report.overall_health, sak::SmartHealthStatus::Unknown);
}

// A partial payload populates exactly the fields that were present; every absent field keeps
// its default rather than picking up a neighbouring value.
void SmartDiskAnalyzerTests::parseOutput_missingFields() {
    sak::SmartDiskAnalyzer analyzer;
    const QByteArray json =
        R"({"device":{"type":"sat"},"model_name":"Unknown Drive","smart_status":{"passed":true}})";
    const sak::SmartReport report = analyzer.parseAndAssessForTesting(json, 0);

    QCOMPARE(report.model, QStringLiteral("Unknown Drive"));
    QCOMPARE(report.interface_type, QStringLiteral("SATA"));
    QCOMPARE(report.smart_status, QStringLiteral("PASSED"));

    QVERIFY(report.serial_number.isEmpty());
    QVERIFY(report.firmware_version.isEmpty());
    QCOMPARE(report.size_bytes, uint64_t{0});
    QCOMPARE(report.temperature_celsius, 0.0);
    QCOMPARE(report.power_on_hours, static_cast<int64_t>(0));
    QVERIFY(report.attributes.isEmpty());

    // An overall status on its own is enough signal to assess.
    QCOMPARE(report.overall_health, sak::SmartHealthStatus::Healthy);
}

// A JSON parse error must abort the parse cleanly: the report identifies the drive and carries
// nothing else. Anything half-populated here would be read downstream as real SMART data.
void SmartDiskAnalyzerTests::parseOutput_malformedJson() {
    sak::SmartDiskAnalyzer analyzer;
    const sak::SmartReport report =
        analyzer.parseAndAssessForTesting("{\"device\":{\"type\":\"sat\"", 3);

    QCOMPARE(report.device_path, QStringLiteral("\\\\.\\PhysicalDrive3"));
    QVERIFY(report.model.isEmpty());
    QVERIFY(report.interface_type.isEmpty());
    QVERIFY(report.smart_status.isEmpty());
    QCOMPARE(report.size_bytes, uint64_t{0});
    QCOMPARE(report.power_on_hours, static_cast<int64_t>(0));
    QCOMPARE(report.temperature_celsius, 0.0);
    QVERIFY(report.attributes.isEmpty());
    QVERIFY(!report.nvme_health.has_value());
    QCOMPARE(report.overall_health, sak::SmartHealthStatus::Unknown);
}

// ============================================================================
// Health Assessment -- driven through the real parse+assess pipeline so the
// per-attribute warning/critical ladder is actually exercised. The thresholds are
// private constants, so each test pins the BOUNDARY (one raw value either side of
// it) rather than a single point that an off-by-one would sail straight past.
// ============================================================================

void SmartDiskAnalyzerTests::healthAssessment_healthy() {
    sak::SmartDiskAnalyzer analyzer;
    const sak::SmartReport report = analyzer.parseAndAssessForTesting(
        sataAttributeJson(199, QStringLiteral("UDMA_CRC_Error_Count"), 0), 0);

    QCOMPARE(report.overall_health, sak::SmartHealthStatus::Healthy);
    QVERIFY(report.warnings.isEmpty());
    // A clean drive gets the positive note -- and only then.
    QVERIFY(report.recommendations.contains(
        QStringLiteral("Drive health is good -- no action required")));
}

void SmartDiskAnalyzerTests::healthAssessment_warning() {
    sak::SmartDiskAnalyzer analyzer;

    // UDMA_CRC_Error_Count warns at 10 raw errors and only turns critical at 100.
    QCOMPARE(healthOf(analyzer, sataAttributeJson(199, QStringLiteral("UDMA_CRC_Error_Count"), 9)),
             sak::SmartHealthStatus::Healthy);

    const sak::SmartReport report = analyzer.parseAndAssessForTesting(
        sataAttributeJson(199, QStringLiteral("UDMA_CRC_Error_Count"), 10), 0);
    QCOMPARE(report.overall_health, sak::SmartHealthStatus::Warning);
    QVERIFY(report.warnings.contains(QStringLiteral("UDMA CRC errors detected: 10")));
    QVERIFY(report.recommendations.contains(
        QStringLiteral("Check SATA cable connections or replace SATA cable")));
    // A drive with an open warning must NOT also be told it is fine.
    QVERIFY(!report.recommendations.contains(
        QStringLiteral("Drive health is good -- no action required")));
}

void SmartDiskAnalyzerTests::healthAssessment_critical() {
    sak::SmartDiskAnalyzer analyzer;

    // Reallocated_Sector_Ct warns from the first bad sector and turns critical at 50.
    QCOMPARE(healthOf(analyzer, sataAttributeJson(5, QStringLiteral("Reallocated_Sector_Ct"), 49)),
             sak::SmartHealthStatus::Warning);

    const sak::SmartReport report = analyzer.parseAndAssessForTesting(
        sataAttributeJson(5, QStringLiteral("Reallocated_Sector_Ct"), 50), 0);
    QCOMPARE(report.overall_health, sak::SmartHealthStatus::Critical);
    // Attribute 5 also feeds the unified reallocated-sector metric.
    QCOMPARE(report.reallocated_sectors, static_cast<int64_t>(50));
    QVERIFY(report.warnings.contains(QStringLiteral("Reallocated sectors detected: 50")));
    QVERIFY(report.recommendations.contains(
        QStringLiteral("CRITICAL: Drive has significant sector damage -- back up data "
                       "immediately and replace drive")));
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

// Renamed from cancel_stopsAnalysis: nothing here observes an analysis stopping. cancel()
// only stores a private atomic, and its sole effect is inside analyzeAll()'s per-drive loop
// and the runProcess() cancel predicate -- both of which need a bundled smartctl and real
// drives. This is therefore a CRASH-REGRESSION TEST: it catches cancel() faulting on an idle
// analyzer. The reports() check records the no-op-on-the-cache contract but cannot fail by
// itself, because a fresh analyzer's cache is empty by construction.
void SmartDiskAnalyzerTests::cancel_beforeAnalysisIsSafe() {
    sak::SmartDiskAnalyzer analyzer;
    analyzer.cancel();
    analyzer.cancel();  // idempotent
    QVERIFY(analyzer.reports().isEmpty());
}

QTEST_GUILESS_MAIN(SmartDiskAnalyzerTests)
#include "test_smart_disk_analyzer.moc"
