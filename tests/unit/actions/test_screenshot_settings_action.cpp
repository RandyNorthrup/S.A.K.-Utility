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
#include <QRegularExpression>
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
    void buildScreenshotReportText_omitsFailedSectionOnCleanRun();
    void generateReport_writesAndDetectsFailure();
    // WaveD-09: success requires a full capture AND a written report; a partial
    // capture or an unwritten report must fail closed, not report success.
    void buildExecutionResult_succeedsOnFullCaptureAndWrittenReport();
    void buildExecutionResult_failsClosedOnPartialOrUnwrittenReport();
    void buildExecutionResult_failsClosedOnUnwrittenReport();
    void buildExecutionResult_failsClosedOnZeroCapture();
    void captureWindowToPng_failsClosedWithoutScreen();
    void captureWindowToPng_failsClosedWhenMarshalledFromWorkerThread();

private:
    // The 3-page context both fail-closed slots drive, parameterised only by the one field that
    // distinguishes them. A member, not a file-scope helper: CaptureContext is private to the
    // action and reachable only through this class's friendship.
    static CaptureContext contextWith(bool report_written);
};

ScreenshotSettingsAction::CaptureContext ScreenshotSettingsActionTests::contextWith(
    bool report_written) {
    CaptureContext ctx;
    ctx.total_pages = 3;
    ctx.monitor_count = 1;
    ctx.timestamp = QStringLiteral("ts");
    ctx.start_time = QDateTime::currentDateTime();
    ctx.report_written = report_written;
    return ctx;
}

void ScreenshotSettingsActionTests::reportPathLine_onlyAdvertisesWrittenReport() {
    QCOMPARE(Action::reportPathLine(true, QStringLiteral("C:/reports/r.txt")),
             QStringLiteral("REPORT_PATH:C:/reports/r.txt\n"));
    // A failed write must not advertise a path that does not exist.
    const QString failed = Action::reportPathLine(false, QStringLiteral("C:/reports/r.txt"));
    QVERIFY(!failed.contains(QStringLiteral("REPORT_PATH:")));
    QCOMPARE(failed, QStringLiteral("REPORT_WRITE_FAILED:1\n"));
}

void ScreenshotSettingsActionTests::buildScreenshotReportText_containsPagesAndSections() {
    // Every header counter has TWO candidate sources, and the old fixture made each pair agree:
    // screenshots_taken == captured_pages.size() and failed_attempts == failed_pages.size(), so
    // the whole-report QCOMPARE below could not tell which member the builder actually read and
    // the inline claims ("captured + failed, not captured alone", "captured_pages.size()",
    // "failed_attempts") were unverifiable. Every value here is deliberately DISTINCT -- monitors
    // 7, captured 2, failed_pages 1, screenshots_taken 5, failed_attempts 4 -- so each counter
    // can only be produced by its own source, and screenshots_taken (which the builder must
    // ignore entirely) matches nothing on the page.
    CaptureResult capture;
    capture.captured_pages << QStringLiteral("Display_Settings")
                           << QStringLiteral("Sound_Settings");
    capture.failed_pages << QStringLiteral("WiFi_Settings");
    capture.screenshots_taken = 5;
    capture.failed_attempts = 4;

    const QString text = Action::buildScreenshotReportText(
        7, capture, QStringLiteral("C:/out"), QStringLiteral("20260731_010203"));

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
        QStringLiteral("| Monitors Detected: 7 |"),
        QStringLiteral("| Total Pages: 3 |"),  // captured + failed, not captured alone
        QStringLiteral("| Successful: 2 |"),   // captured_pages.size(), NOT screenshots_taken (5)
        QStringLiteral("| Failed: 4 |"),       // failed_attempts, NOT failed_pages.size() (1)
        bar,
        QStringLiteral("| CAPTURED PAGES |"),
        bar,
        QStringLiteral("| [x] Display_Settings |"),  // captured page -> [x], never [ ]
        QStringLiteral("| [x] Sound_Settings |"),
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

    // The one line left free above is deliberately its own oracle in `expected`, so the QCOMPARE
    // asserts nothing whatever about it -- which left a bare prefix check as the row's ONLY
    // constraint, saying nothing about the stamp's format, its presence, or the closing bar. This
    // row is the sole record of WHEN the evidence was taken (the builder ignores the passed
    // timestamp -- Q_UNUSED -- and stamps the wall clock instead), so the format IS the contract.
    const QRegularExpression timestamp_row(
        QStringLiteral(R"(^\| Timestamp: \d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2} \|$)"));
    QVERIFY2(timestamp_row.match(lines.value(3)).hasMatch(), qPrintable(lines.value(3)));
}

void ScreenshotSettingsActionTests::buildScreenshotReportText_omitsFailedSectionOnCleanRun() {
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

    // Real directory -> report is written and committed. The monitor count is deliberately NOT 1:
    // generateReport forwards its own monitor_count argument into the builder, and that argument
    // is the only wiring this test can observe for the "Monitors Detected:" row -- but with
    // monitor_count == screenshots_taken == captured_pages.size() == 1, every candidate source
    // produced the identical byte and the line-for-line compare below could not tell which one it
    // actually passed.
    const int kMonitorCount = 4;
    QVERIFY(action.generateReport(dir.path(), QStringLiteral("ts1"), kMonitorCount, capture));
    // The committed file must BE the report for THIS capture, not merely a name on disk:
    // compare it line-for-line against the pure builder, skipping only the wall-clock
    // "Timestamp:" line.
    QFile written(dir.filePath(QStringLiteral("Screenshot_Report_ts1.txt")));
    QVERIFY(written.open(QIODevice::ReadOnly | QIODevice::Text));
    const QString report_text = QString::fromUtf8(written.readAll());
    written.close();
    const QStringList file_lines = report_text.split(QLatin1Char('\n'));
    const QStringList built_lines =
        Action::buildScreenshotReportText(kMonitorCount, capture, dir.path(), QStringLiteral("ts1"))
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
    // Deliberately NOT "now": duration_ms is measured from start_time, and with start_time set to
    // the current instant even a correct measurement is ~0 ms, so no assertion could tell a real
    // elapsed time from a hard-coded zero. Backdating it by a known amount makes the measurement
    // observable.
    const qint64 kInjectedElapsedMs = 1500;
    ctx.start_time = QDateTime::currentDateTime().addMSecs(-kInjectedElapsedMs);
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
    // The third sibling, previously read by no assertion in the repository. It is persisted to
    // the saved result JSON and rendered in the assistant transcript, where a non-positive value
    // renders as nothing at all -- so a duration that silently became 0 would simply vanish from
    // the report rather than look wrong.
    QVERIFY2(result.duration_ms >= kInjectedElapsedMs,
             qPrintable(QStringLiteral("duration_ms %1 did not measure from start_time")
                            .arg(result.duration_ms)));
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
        // A failure still has to say where the screenshots that DID land went, and how many.
        // These two were asserted only on the success path, yet production assigns them before
        // the branch -- and only that placement is what makes them survive a failure.
        QCOMPARE(partial.output_path, out.absolutePath());
        QCOMPARE(partial.files_processed, static_cast<qint64>(2));
        // 2 of 3 pages captured: the structured log must carry the REAL counters and the real
        // 2*100/3 = 66% rate, not a flat 100% that hides the failed page. Pinned WHOLE rather
        // than by a contains() of four lines: the log is deterministic, and this is the only
        // fixture in the file that is both a FAILING run and has report_written == true, so it
        // is the only place that can prove a written report is still advertised when the run
        // failed. A contains() left the MONITORS_DETECTED line, the entire REPORT_PATH line and
        // the trailing "Saved to:" tail unpinned, so the positive direction of that guard was
        // proved only on the full-success path.
        QCOMPARE(partial.log,
                 QStringLiteral("MONITORS_DETECTED:1\n"
                                "SUCCESSFUL_CAPTURES:2\n"
                                "FAILED_CAPTURES:1\n"
                                "TOTAL_PAGES:3\n"
                                "SUCCESS_RATE:66%\n"
                                "REPORT_PATH:") +
                     out.filePath(QStringLiteral("Screenshot_Report_ts.txt")) +
                     QStringLiteral("\n\nSaved to: ") + out.absolutePath());
        // The report WAS written here, so the ", report not written" suffix must be absent.
        QCOMPARE(partial.message, QStringLiteral("Incomplete: captured 2/3 pages, 1 failed"));
        QVERIFY2(!action.lastExecutionResult().success, "a partial capture must not be a success");
    }
}

void ScreenshotSettingsActionTests::buildExecutionResult_failsClosedOnUnwrittenReport() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QDir out(dir.path());

    // Every page captured but the report could not be written -> fail closed.
    {
        ScreenshotSettingsAction action(dir.path());
        CaptureResult cap;
        cap.captured_pages << QStringLiteral("a") << QStringLiteral("b") << QStringLiteral("c");
        cap.screenshots_taken = 3;
        action.buildExecutionResult(cap, out, contextWith(false));
        QVERIFY2(!action.lastExecutionResult().success,
                 "an unwritten report must not be a success");
        // This block is the ONLY case in the repository that reaches the ", report not written"
        // suffix, and it never touched result.message -- the block above pins the message with
        // report_written == true, i.e. only the EMPTY arm of that ternary. So the single
        // technician-visible sentence saying the evidence file is missing was unproven: drop the
        // suffix and every page still reports as merely "incomplete", with the log's
        // REPORT_WRITE_FAILED line the only surviving hint that nothing was saved.
        QCOMPARE(action.lastExecutionResult().message,
                 QStringLiteral("Incomplete: captured 3/3 pages, 0 failed, report not written"));
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
    // The zero-capture branch assembles its OWN log tail, and nothing in the repository read it.
    // That branch is exactly the one whose structured log a technician has to parse to find out
    // why nothing was captured -- the monitor count, the counters, and whether a report file
    // exists -- and it is the only branch carrying the remediation line.
    QCOMPARE(zero.log,
             QStringLiteral("MONITORS_DETECTED:1\n"
                            "SUCCESSFUL_CAPTURES:0\n"
                            "FAILED_CAPTURES:0\n"
                            "TOTAL_PAGES:3\n"
                            "SUCCESS_RATE:0%\n"
                            "REPORT_PATH:") +
                 out.filePath(QStringLiteral("Screenshot_Report_ts.txt")) +
                 QStringLiteral("\n\nCheck display permissions and Settings app availability"));
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
