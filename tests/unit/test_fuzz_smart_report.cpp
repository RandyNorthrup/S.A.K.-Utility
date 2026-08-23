// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file test_fuzz_smart_report.cpp
/// @brief Mutation-fuzz of the smartctl JSON health-report parser (G14 parser sweep).
///
/// SmartDiskAnalyzer parses the JSON that the bundled smartctl emits for a physical drive. That
/// document is not trusted input: a failing drive's firmware, a corrupted capture, or a truncated
/// pipe can all hand the parser malformed or partial JSON. The central security contract is
/// fail-closed -- a payload that carries no usable SMART signal must resolve to Unknown health and
/// never read as a clean drive ([[no-fallbacks-fail-closed]]). parseAndAssessForTesting() runs the
/// whole pipeline (parse -> assess -> recommend) over raw bytes, so this harness drives it over
/// thousands of mutated smartctl documents and asserts for EVERY input:
///
///   1. No crash and no hang (a fault never returns the empty string; a hang trips the ctest
///      timeout with the reproducer recorded in fuzz_harness.h).
///   2. Determinism: parsing the same bytes twice yields the same report.
///   3. Fail-closed equivalence: overall_health is Unknown IFF the report carries no assessable
///      signal. A data-less payload can never earn Healthy/Warning/Critical, and a payload with
///      real signal always earns a definite verdict.
///   4. A drive that self-reports smart_status FAILED is always assessed Critical (an independent
///      restatement of the hardest health rule).
///   5. The pipeline always produces at least one recommendation -- the report is never silently
///      empty of guidance, including on the Unknown path.

#include "sak/diagnostic_types.h"
#include "sak/smart_disk_analyzer.h"

#include "../fuzz/fuzz_harness.h"

#include <QByteArray>
#include <QString>
#include <QtTest/QtTest>

#include <vector>

namespace {

// A compact signature of the report fields the fuzzer reasons about; two runs over identical bytes
// must produce identical signatures (determinism).
struct ReportSignature {
    int health = -1;
    QString smart_status;
    int attribute_count = 0;
    bool has_nvme = false;
    int warning_count = 0;
    int recommendation_count = 0;
    qint64 power_on_hours = 0;
    qint64 reallocated = 0;
    qint64 pending = 0;

    bool operator==(const ReportSignature&) const = default;
};

ReportSignature signatureOf(const sak::SmartReport& r) {
    ReportSignature sig;
    sig.health = static_cast<int>(r.overall_health);
    sig.smart_status = r.smart_status;
    sig.attribute_count = static_cast<int>(r.attributes.size());
    sig.has_nvme = r.nvme_health.has_value();
    sig.warning_count = static_cast<int>(r.warnings.size());
    sig.recommendation_count = static_cast<int>(r.recommendations.size());
    sig.power_on_hours = r.power_on_hours;
    sig.reallocated = r.reallocated_sectors;
    sig.pending = r.pending_sectors;
    return sig;
}

QString smartReportInvariant(const QByteArray& input) {
    sak::SmartDiskAnalyzer analyzer;
    const sak::SmartReport report = analyzer.parseAndAssessForTesting(input, 0);

    // Determinism: a second parse of the same bytes must reach the same report.
    const sak::SmartReport again = analyzer.parseAndAssessForTesting(input, 0);
    if (signatureOf(report) != signatureOf(again)) {
        return QStringLiteral("parseAndAssessForTesting is non-deterministic on identical input");
    }

    // Fail-closed equivalence: Unknown IFF no assessable signal. Neither a data-less payload
    // reading as a definite verdict, nor a signal-carrying payload left Unknown, is allowed.
    const bool isUnknown = report.overall_health == sak::SmartHealthStatus::Unknown;
    const bool hasData = sak::SmartDiskAnalyzer::reportHasAssessableData(report);
    if (isUnknown == hasData) {
        return isUnknown ? QStringLiteral("Unknown health despite assessable SMART data present")
                         : QStringLiteral(
                               "definite health verdict over a data-less SMART payload "
                               "(fail-open)");
    }

    // The hardest rule, restated independently: a self-reported FAILED drive is always Critical.
    if (report.smart_status == QLatin1String("FAILED") &&
        report.overall_health != sak::SmartHealthStatus::Critical) {
        return QStringLiteral("smart_status FAILED but overall_health is not Critical");
    }

    // The pipeline always emits guidance -- a parsed report is never left with no recommendation.
    if (report.recommendations.isEmpty()) {
        return QStringLiteral("assessed report carries no recommendation");
    }
    return {};
}

QByteArray failureBanner(const sak::fuzz::FuzzOutcome& outcome) {
    const QString message =
        QStringLiteral("smart report fuzz failed after %1 inputs: %2\n  reproducer (hex): %3")
            .arg(outcome.iterations_run)
            .arg(outcome.failure_detail, sak::fuzz::reproducerHex(outcome.failing_input));
    return message.toUtf8();
}

// A realistic healthy-SATA document -- the primary accept-path seed the mutator degrades from.
QByteArray healthySataSeed() {
    return QByteArrayLiteral(
        "{\"device\":{\"type\":\"sat\"},\"model_name\":\"ACME SSD\",\"serial_number\":\"S1\","
        "\"firmware_version\":\"FW1\",\"user_capacity\":{\"bytes\":512110190592},"
        "\"smart_status\":{\"passed\":true},\"temperature\":{\"current\":30},"
        "\"power_on_time\":{\"hours\":1200},\"ata_smart_attributes\":{\"table\":["
        "{\"id\":5,\"name\":\"Reallocated_Sector_Ct\",\"value\":100,\"worst\":100,\"thresh\":10,"
        "\"raw\":{\"value\":0}},"
        "{\"id\":199,\"name\":\"UDMA_CRC_Error_Count\",\"value\":200,\"worst\":200,\"thresh\":0,"
        "\"raw\":{\"value\":0}}]}}");
}

}  // namespace

class SmartReportFuzzTests : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void parserNeverCrashesAndStaysFailClosed() {
        std::vector<QByteArray> corpus{
            healthySataSeed(),
            // Self-reported failure.
            QByteArrayLiteral("{\"smart_status\":{\"passed\":false}}"),
            // NVMe health log, worn.
            QByteArrayLiteral(
                "{\"device\":{\"type\":\"nvme\"},\"nvme_smart_health_information_log\":"
                "{\"percentage_used\":90,\"media_errors\":0,\"available_spare\":50,"
                "\"available_spare_threshold\":10,\"temperature\":40,"
                "\"power_on_hours\":9001,\"unsafe_shutdowns\":3}}"),
            // Data-less object -- must resolve to Unknown.
            QByteArrayLiteral("{}"),
            QByteArrayLiteral("{\"model_name\":\"only identity\"}"),
            // Phantom/degenerate attribute tables.
            QByteArrayLiteral("{\"ata_smart_attributes\":{\"table\":[null,{}]}}"),
            QByteArrayLiteral("{\"nvme_smart_health_information_log\":null}"),
            // Not JSON at all / truncated.
            QByteArrayLiteral("not json"),
            QByteArrayLiteral("{\"ata_smart_attributes\":{\"table\":[{\"id\":5,"),
            QByteArray(),
        };
        const sak::fuzz::Target target = [](const QByteArray& input) {
            return smartReportInvariant(input);
        };
        const sak::fuzz::FuzzOutcome outcome = sak::fuzz::run(
            corpus, target, sak::fuzz::iterationsFromEnv(), sak::fuzz::seedFromEnv());
        if (!outcome.ok) {
            const QByteArray banner = failureBanner(outcome);
            QVERIFY2(false, banner.constData());
        }
        // Exact count on the all-pass path (any failure QVERIFY2(false)-returns above): run()
        // increments iterations_run once per seed plus once per mutation iteration. The old >=
        // bound would still pass if the mutation loop ran ZERO iterations.
        QCOMPARE(outcome.iterations_run,
                 static_cast<int>(corpus.size()) + sak::fuzz::iterationsFromEnv());
    }

    // Pin the anchors the fuzz oracle is built around: the healthy seed is assessable and
    // classifies as Healthy, a data-less payload is Unknown, and a FAILED payload is Critical.
    void knownDocumentsClassifyAsExpected() {
        sak::SmartDiskAnalyzer analyzer;

        const sak::SmartReport healthy = analyzer.parseAndAssessForTesting(healthySataSeed(), 0);
        QVERIFY(sak::SmartDiskAnalyzer::reportHasAssessableData(healthy));
        // The healthy seed resolves deterministically to Healthy (no failing attr, no threshold
        // breach); != Unknown alone would still pass if a threshold regression downgraded it to
        // Warning or Critical.
        QCOMPARE(healthy.overall_health, sak::SmartHealthStatus::Healthy);

        const sak::SmartReport empty = analyzer.parseAndAssessForTesting(QByteArrayLiteral("{}"),
                                                                         0);
        QVERIFY(!sak::SmartDiskAnalyzer::reportHasAssessableData(empty));
        QCOMPARE(empty.overall_health, sak::SmartHealthStatus::Unknown);

        const sak::SmartReport failed = analyzer.parseAndAssessForTesting(
            QByteArrayLiteral("{\"smart_status\":{\"passed\":false}}"), 0);
        QCOMPARE(failed.overall_health, sak::SmartHealthStatus::Critical);
    }
};

QTEST_GUILESS_MAIN(SmartReportFuzzTests)
#include "test_fuzz_smart_report.moc"
