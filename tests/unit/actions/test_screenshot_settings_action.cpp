// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file test_screenshot_settings_action.cpp
/// @brief Unit tests for ScreenshotSettingsAction report seams (Codex B6-17):
///        a report-write failure must be surfaced (no advertised path for an
///        unwritten file), and the report text builder is exercised directly.

#include "sak/actions/screenshot_settings_action.h"

#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QThread>
#include <QtTest/QtTest>

#include <memory>

using sak::ScreenshotSettingsAction;

/// Friend of ScreenshotSettingsAction: reaches the private report seams.
class ScreenshotSettingsActionTests : public QObject {
    Q_OBJECT

    using Action = ScreenshotSettingsAction;
    using CaptureResult = ScreenshotSettingsAction::CaptureResult;
    using CaptureContext = ScreenshotSettingsAction::CaptureContext;

private Q_SLOTS:
    void reportPathLine_onlyAdvertisesWrittenReport();
    void buildScreenshotReportText_containsPagesAndSections();
    void generateReport_writesAndDetectsFailure();
    // WaveD-09: success requires a full capture AND a written report; a partial
    // capture or an unwritten report must fail closed, not report success.
    void buildExecutionResult_succeedsOnFullCaptureAndWrittenReport();
    void buildExecutionResult_failsClosedOnPartialOrUnwrittenReport();
    void buildExecutionResult_failsClosedOnZeroCapture();
    void captureWindowToPng_failsClosedWithoutScreen();
    void captureWindowToPng_failsClosedWhenMarshalledFromWorkerThread();
};

void ScreenshotSettingsActionTests::reportPathLine_onlyAdvertisesWrittenReport() {
    QCOMPARE(Action::reportPathLine(true, QStringLiteral("C:/reports/r.txt")),
             QStringLiteral("REPORT_PATH:C:/reports/r.txt\n"));
    // A failed write must not advertise a path that does not exist.
    const QString failed = Action::reportPathLine(false, QStringLiteral("C:/reports/r.txt"));
    QVERIFY(!failed.contains(QStringLiteral("REPORT_PATH:")));
    QCOMPARE(failed, QStringLiteral("REPORT_WRITE_FAILED:1\n"));
}

void ScreenshotSettingsActionTests::buildScreenshotReportText_containsPagesAndSections() {
    CaptureResult capture;
    capture.captured_pages << QStringLiteral("Display_Settings");
    capture.failed_pages << QStringLiteral("WiFi_Settings");
    capture.screenshots_taken = 1;
    capture.failed_attempts = 1;

    const QString text = Action::buildScreenshotReportText(
        2, capture, QStringLiteral("C:/out"), QStringLiteral("20260731_010203"));

    // The builder is pure and deterministic apart from the Timestamp line, so pin the whole
    // report shape: every header counter, WHICH marker each page gets ([x] captured vs [ ]
    // failed), and the trailing Output Location line. Compared on simplified() lines because
    // the column-padding constant is file-local to the .cpp.
    QStringList lines;
    const QStringList raw_lines = text.split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    for (const QString& raw_line : raw_lines) {
        lines << raw_line.simplified();
    }

    const QString bar =
        QStringLiteral("+==============================================================+");
    const QStringList expected{
        bar,
        QStringLiteral("| WINDOWS SETTINGS SCREENSHOT REPORT |"),
        bar,
        lines.value(3),  // Timestamp: wall clock, the only non-deterministic line
        QStringLiteral("| Monitors Detected: 2 |"),
        QStringLiteral("| Total Pages: 2 |"),  // captured + failed, not captured alone
        QStringLiteral("| Successful: 1 |"),   // captured_pages.size()
        QStringLiteral("| Failed: 1 |"),       // failed_attempts
        bar,
        QStringLiteral("| CAPTURED PAGES |"),
        bar,
        QStringLiteral("| [x] Display_Settings |"),  // captured page -> [x], never [ ]
        bar,
        QStringLiteral("| FAILED PAGES |"),
        bar,
        QStringLiteral("| [ ] WiFi_Settings |"),  // failed page -> [ ], never [x]
        bar,
        QStringLiteral("| Output Location: %1 |")
            .arg(QDir(QStringLiteral("C:/out")).absolutePath()),
        bar,
    };
    QCOMPARE(lines, expected);

    // The one line left free above is still constrained to be the timestamp.
    QVERIFY(lines.value(3).startsWith(QStringLiteral("| Timestamp: ")));

    // The FAILED PAGES section is guarded: a capture with no failures must NOT print a failure
    // banner. A report that says "FAILED PAGES" on a perfect run is a false alarm to the
    // customer.
    CaptureResult clean;
    clean.captured_pages << QStringLiteral("Display_Settings");
    clean.screenshots_taken = 1;

    const QString clean_text = Action::buildScreenshotReportText(
        1, clean, QStringLiteral("C:/out"), QStringLiteral("20260731_010203"));

    QVERIFY(clean_text.contains(QStringLiteral("Display_Settings")));
    QVERIFY2(!clean_text.contains(QStringLiteral("FAILED PAGES")),
             "a capture with no failed pages must not emit a FAILED PAGES section");
    QVERIFY2(!clean_text.contains(QStringLiteral("[ ]")),
             "a capture with no failed pages must not emit any failed-page row");
}

void ScreenshotSettingsActionTests::generateReport_writesAndDetectsFailure() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    ScreenshotSettingsAction action(dir.path());
    CaptureResult capture;
    capture.captured_pages << QStringLiteral("Display_Settings");
    capture.screenshots_taken = 1;

    // Real directory -> report is written and committed.
    QVERIFY(action.generateReport(dir.path(), QStringLiteral("ts1"), 1, capture));
    // The committed file must BE the report for THIS capture, not merely a name on disk:
    // compare it line-for-line against the pure builder, skipping only the wall-clock
    // "Timestamp:" line.
    QFile written(dir.filePath(QStringLiteral("Screenshot_Report_ts1.txt")));
    QVERIFY(written.open(QIODevice::ReadOnly | QIODevice::Text));
    const QString report_text = QString::fromUtf8(written.readAll());
    written.close();
    const QStringList file_lines = report_text.split(QLatin1Char('\n'));
    const QStringList built_lines =
        Action::buildScreenshotReportText(1, capture, dir.path(), QStringLiteral("ts1"))
            .split(QLatin1Char('\n'));
    QCOMPARE(file_lines.size(), built_lines.size());
    for (int i = 0; i < built_lines.size(); ++i) {
        if (built_lines.at(i).contains(QStringLiteral("| Timestamp:"))) {
            continue;  // wall clock, not a wiring contract
        }
        QCOMPARE(file_lines.at(i), built_lines.at(i));
    }
    QVERIFY(report_text.contains(QStringLiteral("Display_Settings")));

    // Non-existent parent directory -> open fails -> reported as a write failure.
    const QString missing = dir.filePath(QStringLiteral("no_such_subdir"));
    QVERIFY(!action.generateReport(missing, QStringLiteral("ts2"), 1, capture));
}

void ScreenshotSettingsActionTests::buildExecutionResult_succeedsOnFullCaptureAndWrittenReport() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QDir out(dir.path());

    CaptureContext ctx;
    ctx.total_pages = 3;
    ctx.monitor_count = 1;
    ctx.timestamp = QStringLiteral("ts");
    ctx.start_time = QDateTime::currentDateTime();
    ctx.report_written = true;

    ScreenshotSettingsAction action(dir.path());
    CaptureResult cap;
    cap.captured_pages << QStringLiteral("a") << QStringLiteral("b") << QStringLiteral("c");
    cap.screenshots_taken = 3;
    action.buildExecutionResult(cap, out, ctx);
    const auto& result = action.lastExecutionResult();
    QVERIFY2(result.success, "full capture + report is a success");
    // The technician-visible siblings: where the screenshots went and how many.
    QCOMPARE(result.output_path, out.absolutePath());
    QCOMPARE(result.files_processed, static_cast<qint64>(3));
    QCOMPARE(result.message, QStringLiteral("Captured 3/3 settings pages (1 monitors detected)"));
    // The structured log the caller parses, REPORT_PATH line included.
    QCOMPARE(result.log,
             QStringLiteral("MONITORS_DETECTED:1\n"
                            "SUCCESSFUL_CAPTURES:3\n"
                            "FAILED_CAPTURES:0\n"
                            "TOTAL_PAGES:3\n"
                            "SUCCESS_RATE:100%\n"
                            "REPORT_PATH:") +
                 out.filePath(QStringLiteral("Screenshot_Report_ts.txt")) +
                 QStringLiteral("\n\nSaved to: ") + out.absolutePath());
}

void ScreenshotSettingsActionTests::buildExecutionResult_failsClosedOnPartialOrUnwrittenReport() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QDir out(dir.path());

    auto contextWith = [&](bool report_written) {
        CaptureContext ctx;
        ctx.total_pages = 3;
        ctx.monitor_count = 1;
        ctx.timestamp = QStringLiteral("ts");
        ctx.start_time = QDateTime::currentDateTime();
        ctx.report_written = report_written;
        return ctx;
    };

    // Some pages failed, even though the report was written -> fail closed.
    {
        ScreenshotSettingsAction action(dir.path());
        CaptureResult cap;
        cap.captured_pages << QStringLiteral("a") << QStringLiteral("b");
        cap.screenshots_taken = 2;
        cap.failed_attempts = 1;
        action.buildExecutionResult(cap, out, contextWith(true));
        const auto& partial = action.lastExecutionResult();
        QVERIFY2(!partial.success, "a partial capture must not be a success");
        // 2 of 3 pages captured: the structured log must carry the REAL counters and the
        // real 2*100/3 = 66% rate, not a flat 100% that hides the failed page.
        QVERIFY2(partial.log.contains(QStringLiteral("SUCCESSFUL_CAPTURES:2\n"
                                                     "FAILED_CAPTURES:1\n"
                                                     "TOTAL_PAGES:3\n"
                                                     "SUCCESS_RATE:66%\n")),
                 qPrintable(partial.log));
        // The report WAS written here, so the ", report not written" suffix must be absent.
        QCOMPARE(partial.message, QStringLiteral("Incomplete: captured 2/3 pages, 1 failed"));
        QVERIFY2(!action.lastExecutionResult().success, "a partial capture must not be a success");
    }

    // Every page captured but the report could not be written -> fail closed.
    {
        ScreenshotSettingsAction action(dir.path());
        CaptureResult cap;
        cap.captured_pages << QStringLiteral("a") << QStringLiteral("b") << QStringLiteral("c");
        cap.screenshots_taken = 3;
        action.buildExecutionResult(cap, out, contextWith(false));
        QVERIFY2(!action.lastExecutionResult().success,
                 "an unwritten report must not be a success");
        // "Not a success" says nothing about what the log ADVERTISES. An unwritten report must
        // not hand the caller a REPORT_PATH for a file that does not exist, and must surface the
        // write failure instead.
        const QString unwritten_log = action.lastExecutionResult().log;
        QVERIFY2(!unwritten_log.contains(QStringLiteral("REPORT_PATH:")),
                 "an unwritten report must not advertise a report path");
        QVERIFY2(unwritten_log.contains(QStringLiteral("REPORT_WRITE_FAILED:1")),
                 "an unwritten report must surface the write failure in the log");
    }
}

void ScreenshotSettingsActionTests::buildExecutionResult_failsClosedOnZeroCapture() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QDir out(dir.path());

    // Nothing captured AND nothing recorded as failed (every page short-circuited before an
    // attempt was even counted) -> fail closed. This is the only case in which the
    // `screenshots_taken > 0` arm of full_success decides the outcome, and the only one that
    // reaches the zero-capture message branch.
    CaptureContext ctx;
    ctx.total_pages = 3;
    ctx.monitor_count = 1;
    ctx.timestamp = QStringLiteral("ts");
    ctx.start_time = QDateTime::currentDateTime();
    ctx.report_written = true;

    ScreenshotSettingsAction action(dir.path());
    CaptureResult cap;  // no captured pages, no failed attempts
    action.buildExecutionResult(cap, out, ctx);
    const auto& zero = action.lastExecutionResult();
    QVERIFY2(!zero.success, "a zero-capture run must not be a success");
    QCOMPARE(zero.message, QStringLiteral("Failed to capture any screenshots"));
}

void ScreenshotSettingsActionTests::captureWindowToPng_failsClosedWithoutScreen() {
    // CODEX_REVIEW_4 M-B3-9: the capture now marshals onto the GUI thread (grabWindow has
    // GUI-thread affinity) and fails closed. In this GUILESS harness there is no primary screen,
    // so the grab must return false and never write a file (rather than crash or claim success).
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = QDir(dir.path()).filePath(QStringLiteral("shot.png"));
    QVERIFY(!ScreenshotSettingsAction::captureWindowToPng(0, path));
    QVERIFY(!QFileInfo::exists(path));
}

void ScreenshotSettingsActionTests::captureWindowToPng_failsClosedWhenMarshalledFromWorkerThread() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = QDir(dir.path()).filePath(QStringLiteral("worker_shot.png"));

    // Force the M-B3-9 marshalled arm: calling from a worker thread makes
    // QThread::currentThread() != app->thread(), so the same-thread early return is skipped --
    // the arm the case above can never reach. The local event loop below runs on the GUI thread
    // and services the BlockingQueuedConnection, so the returned value must be the GUI-side grab
    // result, not the mere fact that the invocation was delivered.
    bool captured = true;  // poisoned: a fail-open `return invoked` would leave this true
    std::unique_ptr<QThread> worker(QThread::create(
        [&captured, path]() { captured = ScreenshotSettingsAction::captureWindowToPng(0, path); }));
    QEventLoop loop;
    QObject::connect(worker.get(), &QThread::finished, &loop, &QEventLoop::quit);
    worker->start();
    loop.exec();
    QVERIFY(worker->wait(30'000));

    QVERIFY2(!captured,
             "a worker-thread capture must return the GUI-side grab result, not merely that "
             "the invocation was delivered");
    QVERIFY(!QFileInfo::exists(path));
}

QTEST_GUILESS_MAIN(ScreenshotSettingsActionTests)
#include "test_screenshot_settings_action.moc"
