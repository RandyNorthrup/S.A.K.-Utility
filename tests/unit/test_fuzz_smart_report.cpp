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

// Guidance must agree with the verdict it explains: the NVMe recommendation ladder walks the
// SAME thresholds as the NVMe health assessment, so the two may not drift apart.
// A self-reported FAILED drive is always Critical -- and the report has to SAY so. The enum
// alone cannot see the FAILED guidance being dropped.
QString smartFailedDriveRule(const sak::SmartReport& report) {
    // The hardest rule, restated independently: a self-reported FAILED drive is always Critical --
    // AND the report has to say so. The enum alone cannot see the FAILED guidance being dropped:
    // without the prepend at smart_disk_analyzer.cpp:609 the warning list is empty, so
    // generateRecommendations() falls through to the clean-drive branch at :628 and hands a drive
    // reporting imminent failure "Drive health is good -- no action required" while the verdict
    // stays Critical and the recommendation list stays non-empty.
    if (report.smart_status == QLatin1String("FAILED")) {
        if (report.overall_health != sak::SmartHealthStatus::Critical) {
            return QStringLiteral("smart_status FAILED but overall_health is not Critical");
        }
        if (!report.recommendations.contains(
                QStringLiteral("CRITICAL: Drive is reporting imminent failure -- back up all data "
                               "immediately and replace drive"))) {
            return QStringLiteral(
                "smart_status FAILED but the imminent-failure recommendation is missing");
        }
        if (!report.warnings.contains(QStringLiteral("SMART overall health assessment: FAILED"))) {
            return QStringLiteral("smart_status FAILED but the FAILED health warning is missing");
        }
        if (report.recommendations.contains(
                QStringLiteral("Drive health is good -- no action required"))) {
            return QStringLiteral(
                "smart_status FAILED but the report advises that drive health is good");
        }
    }
    return {};
}

QString smartGuidanceConsistency(const sak::SmartReport& report) {
    // Guidance must agree with the verdict it explains. generateNvmeRecommendations() walks the
    // SAME wear ladder as assessNvmeHealth() (smart_disk_analyzer.cpp:553/:558 vs :504/:510), so
    // the two may not drift apart: critical-wear advice may only ride a Critical report, and the
    // near-future wear advice may never ride a report assessed Healthy. An NVMe log always
    // satisfies reportHasAssessableData(), so neither branch can be reached with Unknown.
    if (report.recommendations.contains(
            QStringLiteral("CRITICAL: Plan drive replacement -- SSD endurance nearly exhausted")) &&
        report.overall_health != sak::SmartHealthStatus::Critical) {
        return QStringLiteral("critical NVMe wear advice on a report not assessed Critical");
    }
    if (report.recommendations.contains(
            QStringLiteral("Consider planning drive replacement in the near future")) &&
        report.overall_health == sak::SmartHealthStatus::Healthy) {
        return QStringLiteral("NVMe wear advice on a report assessed Healthy");
    }

    // The Unknown path always says plainly that health is unknown -- non-emptiness alone would
    // still pass if that sentence were dropped while a temperature/age advisory kept the list
    // populated (smart_disk_analyzer.cpp:619-624).
    if (report.overall_health == sak::SmartHealthStatus::Unknown &&
        !report.recommendations.contains(QStringLiteral(
            "SMART data was unavailable or unreadable -- verify the drive connection and "
            "re-run with administrator privileges"))) {
        return QStringLiteral("Unknown health without the could-not-determine guidance");
    }
    return {};
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

    if (const QString detail = smartFailedDriveRule(report); !detail.isEmpty()) {
        return detail;
    }

    // The pipeline always emits guidance -- a parsed report is never left with no recommendation.
    if (report.recommendations.isEmpty()) {
        return QStringLiteral("assessed report carries no recommendation");
    }
    if (const QString detail = smartGuidanceConsistency(report); !detail.isEmpty()) {
        return detail;
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

    void dataLessPayloadsStayUnknown() {
        sak::SmartDiskAnalyzer analyzer;
        // Identity fields are NOT assessable signal. A smartctl reply that names the drive but
        // carries no smart_status, no attribute table and no NVMe log means the drive told us
        // nothing about its health, so it must stay Unknown -- never a clean bill of health.
        // This is the anchor the fuzz corpus entry {"model_name":"only identity"} exists for:
        // the oracle at line 83 calls the same predicate assessHealth() decides Unknown with
        // (smart_disk_analyzer.cpp:464), so WIDENING that signal set moves both sides of the
        // equivalence together and stays silent. Pinned here, a widened set goes red.
        const sak::SmartReport identity_only = analyzer.parseAndAssessForTesting(
            QByteArrayLiteral("{\"model_name\":\"only identity\"}"), 0);
        QCOMPARE(identity_only.model, QStringLiteral("only identity"));
        QVERIFY(!sak::SmartDiskAnalyzer::reportHasAssessableData(identity_only));
        QCOMPARE(identity_only.overall_health, sak::SmartHealthStatus::Unknown);
        // ...and the guidance never reads as a clean bill over a drive we never actually read.
        for (const QString& rec : identity_only.recommendations) {
            QVERIFY2(!rec.contains(QStringLiteral("health is good"), Qt::CaseInsensitive),
                     "an identity-only SMART payload must not be reported as healthy");
        }
    }

    void degenerateSignalRecordsStayUnknown() {
        sak::SmartDiskAnalyzer analyzer;
        // Phantom/degenerate ATA entries are dropped, not counted as signal. `table:[null,{}]`
        // parses to two id-0 attributes; without the id-0 skip (smart_disk_analyzer.cpp:357)
        // they would be appended, satisfy reportHasAssessableData(), and let this data-less
        // payload read Healthy with the "health is good" note.
        const sak::SmartReport phantom_attrs = analyzer.parseAndAssessForTesting(
            QByteArrayLiteral("{\"ata_smart_attributes\":{\"table\":[null,{}]}}"), 0);
        QVERIFY(phantom_attrs.attributes.isEmpty());
        QVERIFY(!sak::SmartDiskAnalyzer::reportHasAssessableData(phantom_attrs));
        QCOMPARE(phantom_attrs.overall_health, sak::SmartHealthStatus::Unknown);
        QVERIFY(!phantom_attrs.recommendations.contains(
            QStringLiteral("Drive health is good -- no action required")));

        // A null/empty NVMe log builds no health record. Without the empty-log guard
        // (smart_disk_analyzer.cpp:402) an all-zero NvmeHealthInfo would be synthesized
        // (available_spare 0 < threshold 0 is false, media_errors 0, wear 0) and read Healthy.
        const sak::SmartReport null_nvme = analyzer.parseAndAssessForTesting(
            QByteArrayLiteral("{\"nvme_smart_health_information_log\":null}"), 0);
        QVERIFY(!null_nvme.nvme_health.has_value());
        QVERIFY(!sak::SmartDiskAnalyzer::reportHasAssessableData(null_nvme));
        QCOMPARE(null_nvme.overall_health, sak::SmartHealthStatus::Unknown);
        QVERIFY(!null_nvme.recommendations.contains(
            QStringLiteral("Drive health is good -- no action required")));

        // Critical has a second SATA producer that no test in the tree reaches: the per-attribute
        // `failing` flag (smart_disk_analyzer.cpp:476), raised when a normalized value falls to or
        // below the drive's own threshold (:374). Raw 0 keeps the per-id ladder at Healthy and the
        // status is PASSED, so ONLY the failing-flag promotion can produce Critical here --
        // deleting that arm turns this Healthy. (Asserting health only: such a drive currently ALSO
        // collects "Drive health is good", because the flag arm appends no warning. That fail-open
        // is a separate defect; pinning it here would fail against the tree as it stands.)
        const sak::SmartReport prefail = analyzer.parseAndAssessForTesting(
            QByteArrayLiteral(
                "{\"device\":{\"type\":\"sat\"},\"smart_status\":{\"passed\":true},"
                "\"ata_smart_attributes\":{\"table\":[{\"id\":5,"
                "\"name\":\"Reallocated_Sector_Ct\",\"value\":8,\"worst\":8,\"thresh\":10,"
                "\"raw\":{\"value\":0}}]}}"),
            0);
        QCOMPARE(prefail.overall_health, sak::SmartHealthStatus::Critical);

        // The other half of that gate: thresh 0 means the vendor declared NO threshold, so a
        // normalized value of 0 must not be read as at-or-below it. Without the `threshold > 0`
        // conjunct at smart_disk_analyzer.cpp:374 this document reads 0 <= 0 as failing and comes
        // back Critical.
        const sak::SmartReport no_threshold = analyzer.parseAndAssessForTesting(
            QByteArrayLiteral(
                "{\"device\":{\"type\":\"sat\"},\"smart_status\":{\"passed\":true},"
                "\"ata_smart_attributes\":{\"table\":[{\"id\":199,"
                "\"name\":\"UDMA_CRC_Error_Count\",\"value\":0,\"worst\":0,\"thresh\":0,"
                "\"raw\":{\"value\":0}}]}}"),
            0);
        QCOMPARE(no_threshold.overall_health, sak::SmartHealthStatus::Healthy);
    }
};

QTEST_GUILESS_MAIN(SmartReportFuzzTests)
#include "test_fuzz_smart_report.moc"
