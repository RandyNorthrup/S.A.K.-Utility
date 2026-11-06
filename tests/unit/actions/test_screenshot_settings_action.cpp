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
#include <QImage>
#include <QSet>
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
    void windowToClose_neverTheUsersOwnSettingsWindow();
    void settingsWindowIsFoundAndWaitedForWithoutTasklistOrKills();
    void aBlankPictureIsAFailedCapture();
    void aPageIsSavedOnlyOnceSettingsHasDrawnIt();
    void aPageSettingsNeverOpensIsNotOnThisWindows();
    void aPageIsMissingOnlyWhenEveryTryFindsThePageBefore();
    void settingsPagesUsePublishedNames();
    void buildExecutionResult_namesPagesThisPcDoesNotHave();
    void reportPathLine_onlyAdvertisesWrittenReport();
    void createCaptureFolder_reportsAFolderItCannotCreate();
    void buildScreenshotReportText_containsPagesAndSections();
    void buildScreenshotReportText_omitsFailedSectionOnCleanRun();
    void generateReport_writesAndDetectsFailure();
    // WaveD-09: success requires a full capture AND a written report; a partial
    // capture or an unwritten report must fail closed, not report success.
    void buildExecutionResult_succeedsOnFullCaptureAndWrittenReport();
    void buildExecutionResult_failsClosedOnPartialOrUnwrittenReport();
    void buildExecutionResult_failsClosedOnUnwrittenReport();
    void buildExecutionResult_failsClosedOnZeroCapture();
    void renderOnGui_failsClosedWithoutAWindow();
    void renderOnGui_failsClosedWhenMarshalledFromWorkerThread();

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

// The capture folder is created before any Settings page is opened, and a folder that cannot
// be created ends the run with that error; it used to log a warning and open every page anyway,
// each capture then failing on its own.
void ScreenshotSettingsActionTests::createCaptureFolder_reportsAFolderItCannotCreate() {
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const QDir fresh(root.filePath(QStringLiteral("SettingsScreenshots/20261101_120000")));
    QCOMPARE(Action::createCaptureFolder(fresh), QString());
    QVERIFY(fresh.exists());

    QFile blocker(root.filePath(QStringLiteral("not-a-folder")));
    QVERIFY(blocker.open(QIODevice::WriteOnly));
    blocker.close();
    const QDir blocked(root.filePath(QStringLiteral("not-a-folder/SettingsScreenshots/ts")));
    QCOMPARE(Action::createCaptureFolder(blocked),
             QStringLiteral("Could not create the screenshot folder: %1")
                 .arg(QDir::toNativeSeparators(blocked.absolutePath())));
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

    // The builder is pure and deterministic, so pin the whole report shape: every header
    // counter, WHICH marker each page gets ([x] captured vs [ ]
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
        QStringLiteral("| Timestamp: 20260731_010203 |"),
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
            .arg(QDir::toNativeSeparators(QDir(QStringLiteral("C:/out")).absolutePath())),
        bar,
    };
    QCOMPARE(lines, expected);
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
    // compare it byte-for-byte against the pure deterministic builder.
    QFile written(dir.filePath(QStringLiteral("Screenshot_Report_ts1.txt")));
    QVERIFY(written.open(QIODevice::ReadOnly | QIODevice::Text));
    const QString report_text = QString::fromUtf8(written.readAll());
    written.close();
    const QString built_text = Action::buildScreenshotReportText(
        kMonitorCount, capture, dir.path(), QStringLiteral("ts1"));
    QCOMPARE(report_text, built_text);
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
    QCOMPARE(result.message, QStringLiteral("Captured 3/3 settings pages (1 monitor detected)"));
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

void ScreenshotSettingsActionTests::renderOnGui_failsClosedWithoutAWindow() {
    // CODEX_REVIEW_4 M-B3-9: the picture is taken on the GUI thread and fails closed. With no
    // window there is nothing to render, so the picture is null (rather than a crash, or a picture
    // of whatever is on top).
    QVERIFY(ScreenshotSettingsAction::renderOnGui(0).isNull());
}

void ScreenshotSettingsActionTests::renderOnGui_failsClosedWhenMarshalledFromWorkerThread() {
    // Force the M-B3-9 marshalled arm: calling from a worker thread makes
    // QThread::currentThread() != app->thread(), so the same-thread early return is skipped --
    // the arm the case above can never reach. The local event loop below runs on the GUI thread
    // and services the BlockingQueuedConnection, so the returned picture must be the GUI-side
    // result, not the mere fact that the invocation was delivered.
    QImage poisoned(4, 4, QImage::Format_RGB32);  // a fail-open path would leave this set
    poisoned.fill(Qt::red);
    QImage rendered = poisoned;
    std::unique_ptr<QThread> worker(
        QThread::create([&rendered]() { rendered = ScreenshotSettingsAction::renderOnGui(0); }));
    QEventLoop loop;
    QObject::connect(worker.get(), &QThread::finished, &loop, &QEventLoop::quit);
    worker->start();
    loop.exec();
    QVERIFY(worker->wait(30'000));
    QVERIFY2(rendered.isNull(),
             "a worker-thread capture must return the GUI-side picture, not merely that the "
             "invocation was delivered");
}

// The action used to force-kill every SystemSettings.exe by name -- including a Settings window
// the user had open. Settings is single-instance, so a window that was already open is theirs:
// it is never closed. Only a window this run brought up is asked to close (WM_CLOSE).
void ScreenshotSettingsActionTests::windowToClose_neverTheUsersOwnSettingsWindow() {
    constexpr quintptr kUsers = 0x1234;
    constexpr quintptr kOurs = 0x5678;
    QCOMPARE(Action::windowToClose(kUsers, kUsers), quintptr{0});
    QCOMPARE(Action::windowToClose(0, kOurs), kOurs);
    QCOMPARE(Action::windowToClose(kUsers, kOurs), kOurs);
    QCOMPARE(Action::windowToClose(0, 0), quintptr{0});
}

// Detection is EnumWindows over visible top-level windows, waits are for state (window present
// and answering), and nothing is force-killed: no taskkill, no tasklist per page, no fixed sleeps.
// Eight pages saved as eight 1024x768 pictures of one colour: copying the pixels of a window the
// compositor draws gives that. A blank is now a failed try, and the capture asks the frame window
// a person sees to render itself.
// Settings navigates its one window, so a picture taken as soon as it answered was the page before:
// in the capture VM all fifteen pages were saved, and every one after the first showed System >
// About. A page is saved once its picture is not blank, has stopped changing, and is not the
// last page saved.
void ScreenshotSettingsActionTests::aPageIsSavedOnlyOnceSettingsHasDrawnIt() {
    QImage about(64, 48, QImage::Format_RGB32);
    about.fill(QColor(243, 243, 243));
    about.setPixel(10, 10, qRgb(0, 0, 0));
    QImage display = about;
    display.setPixel(20, 20, qRgb(0, 120, 215));
    QImage spinner = display;
    spinner.setPixel(30, 30, qRgb(128, 128, 128));
    QImage blank(64, 48, QImage::Format_RGB32);
    blank.fill(QColor(243, 243, 243));

    QVERIFY(Action::pageIsDrawn(display, display, about));
    QVERIFY(Action::pageIsDrawn(about, about, QImage()));    // the first page has no page before it
    QVERIFY(!Action::pageIsDrawn(about, about, about));      // still the last page
    QVERIFY(!Action::pageIsDrawn(display, spinner, about));  // still changing
    QVERIFY(!Action::pageIsDrawn(display, QImage(), about));  // nothing to compare with yet
    QVERIFY(!Action::pageIsDrawn(blank, blank, about));
    // A page that never settles (a list filling in, an animation) is kept once the wait runs out,
    // if it is a new page at all.
    QVERIFY(Action::pageHasChanged(spinner, about));
    QVERIFY(!Action::pageHasChanged(about, about));
    QVERIFY(!Action::pageHasChanged(blank, about));

    // Settings is brought to the front first: a window behind another one stops drawing.
    QFile file(QDir(QStringLiteral(SAK_REPO_ROOT))
                   .filePath(QStringLiteral("src/actions/screenshot_settings_action.cpp")));
    QVERIFY(file.open(QIODevice::ReadOnly));
    const QString source = QString::fromUtf8(file.readAll());
    QVERIFY(source.contains(QStringLiteral("SetForegroundWindow(hwnd);")));
    QVERIFY(source.contains(
        QStringLiteral("const PageOutcome outcome = waitForDrawnPage(settings_window, &page);")));

    // The wait itself: a page that keeps moving is kept once the wait runs out.
    Action::PageWatch moving;
    QVERIFY(moving.look(display, about).isNull());
    QVERIFY(moving.look(spinner, about).isNull());
    QCOMPARE(moving.outcomeAtDeadline(), Action::PageOutcome::Captured);
    QCOMPARE(moving.changed, spinner);
    Action::PageWatch settles;
    QVERIFY(settles.look(display, about).isNull());
    QCOMPARE(settles.look(display, about), display);
}

// Coordinator, round 5: in the capture VM Installed apps and Gaming never left the page before
// them in three 10 s tries apiece. Their names, ms-settings:apps-features and ms-settings:gaming,
// are not pages at all: on testpc1 (Windows 11 25H2) each left System > About on screen. A window
// that shows only the page before for the whole wait did not open this page, and the page is
// reported as one this Windows does not have; nothing is saved for it. Blank or no window is a
// failed try, as before.
void ScreenshotSettingsActionTests::aPageSettingsNeverOpensIsNotOnThisWindows() {
    QImage about(64, 48, QImage::Format_RGB32);
    about.fill(QColor(243, 243, 243));
    about.setPixel(10, 10, qRgb(0, 0, 0));
    QImage blank(64, 48, QImage::Format_RGB32);
    blank.fill(QColor(243, 243, 243));

    Action::PageWatch stayed;
    for (int look = 0; look < 5; ++look) {
        QVERIFY(stayed.look(about, about).isNull());
    }
    QCOMPARE(stayed.outcomeAtDeadline(), Action::PageOutcome::NotOnThisWindows);

    Action::PageWatch nothing;
    QVERIFY(nothing.look(blank, about).isNull());
    QVERIFY(nothing.look(QImage(), about).isNull());
    QCOMPARE(nothing.outcomeAtDeadline(), Action::PageOutcome::Failed);

    // The first page has no page before it: a window showing something is that page.
    Action::PageWatch first;
    QVERIFY(first.look(about, QImage()).isNull());
    QCOMPARE(first.look(about, QImage()), about);
    Action::PageWatch first_blank;
    QVERIFY(first_blank.look(blank, QImage()).isNull());
    QCOMPARE(first_blank.outcomeAtDeadline(), Action::PageOutcome::Failed);

    QCOMPARE(Action::absentReason(Action::PageOutcome::NotOnThisWindows, false),
             QStringLiteral("this page does not exist on this Windows"));
    QCOMPARE(Action::absentReason(Action::PageOutcome::NotOnThisWindows, true),
             QStringLiteral("this PC has no Wi-Fi adapter"));
}

// In the capture VM Game Mode took longer than one 10 s wait to draw: the first try saw only the
// page before, and the next found Game Mode drawn. One slow try does not make a page missing.
void ScreenshotSettingsActionTests::aPageIsMissingOnlyWhenEveryTryFindsThePageBefore() {
    using Outcome = Action::PageOutcome;
    QCOMPARE(Action::outcomeOfAttempts({Outcome::NotOnThisWindows, Outcome::Captured}),
             Outcome::Captured);
    QCOMPARE(Action::outcomeOfAttempts(
                 {Outcome::NotOnThisWindows, Outcome::NotOnThisWindows, Outcome::NotOnThisWindows}),
             Outcome::NotOnThisWindows);
    QCOMPARE(Action::outcomeOfAttempts(
                 {Outcome::NotOnThisWindows, Outcome::Failed, Outcome::NotOnThisWindows}),
             Outcome::Failed);
    QCOMPARE(Action::outcomeOfAttempts({Outcome::Failed, Outcome::Failed, Outcome::Failed}),
             Outcome::Failed);
    QCOMPARE(Action::outcomeOfAttempts({}), Outcome::Failed);
}
// The ms-settings: names held against Microsoft's published list ("Launch Windows Settings",
// learn.microsoft.com/windows/apps/develop/launch/launch-settings, read 2026-11-06), which serves
// Windows 10 and 11. The first name of every page is published; a later one is a name testpc1's
// Windows 11 also opened. apps-features and gaming open nothing.
void ScreenshotSettingsActionTests::settingsPagesUsePublishedNames() {
    const QSet<QString> published = {QStringLiteral("about"),
                                     QStringLiteral("activation"),
                                     QStringLiteral("display"),
                                     QStringLiteral("powersleep"),
                                     QStringLiteral("storagesense"),
                                     QStringLiteral("sound"),
                                     QStringLiteral("notifications"),
                                     QStringLiteral("personalization"),
                                     QStringLiteral("privacy"),
                                     QStringLiteral("privacy-general"),
                                     QStringLiteral("windowsupdate"),
                                     QStringLiteral("network-status"),
                                     QStringLiteral("network-ethernet"),
                                     QStringLiteral("network-wifi"),
                                     QStringLiteral("appsfeatures"),
                                     QStringLiteral("gaming-gamebar"),
                                     QStringLiteral("gaming-gamemode")};
    // Opened on testpc1 (Windows 11 25H2) by the round 5 probe, not in the published list.
    const QSet<QString> windows11_too = {QStringLiteral("network"),
                                         QStringLiteral("installed-apps")};
    const QList<Action::SettingsPage> pages = Action::settingsPages();
    QCOMPARE(pages.size(), qsizetype{15});
    QCOMPARE(pages.first().name, QStringLiteral("System_About"));
    QSet<QString> names;
    for (const Action::SettingsPage& page : pages) {
        QVERIFY2(!page.uris.isEmpty(), qPrintable(page.name));
        QVERIFY2(published.contains(page.uris.first()), qPrintable(page.uris.first()));
        for (const QString& uri : page.uris) {
            QVERIFY2(published.contains(uri) || windows11_too.contains(uri), qPrintable(uri));
        }
        names.insert(page.name);
        QCOMPARE(page.needs_wifi, page.name == QStringLiteral("WiFi_Settings"));
    }
    QCOMPARE(names.size(), pages.size());

    sak::NetworkAdapterInfo ethernet;
    ethernet.adapterType = QStringLiteral("Ethernet");
    sak::NetworkAdapterInfo wifi;
    wifi.adapterType = QStringLiteral("WiFi");
    QVERIFY(!Action::hasWifiAdapter({}));
    QVERIFY(!Action::hasWifiAdapter({ethernet}));
    QVERIFY(Action::hasWifiAdapter({ethernet, wifi}));
}

// A page this PC does not have is said, in the message, the log and the report, and is not a
// failure: the capture VM has no Wi-Fi adapter, and its run read "Incomplete" for it.
void ScreenshotSettingsActionTests::buildExecutionResult_namesPagesThisPcDoesNotHave() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QDir out(dir.path());
    ScreenshotSettingsAction action(dir.path());
    CaptureResult cap;
    cap.captured_pages << QStringLiteral("a") << QStringLiteral("b");
    cap.screenshots_taken = 2;
    cap.absent_pages << QStringLiteral("WiFi_Settings: this PC has no Wi-Fi adapter");
    CaptureContext ctx = contextWith(true);
    ctx.total_pages = 2;
    ctx.monitor_count = 2;
    action.buildExecutionResult(cap, out, ctx);
    const auto& result = action.lastExecutionResult();
    QVERIFY2(result.success, "a page this PC does not have is not a failed capture");
    QCOMPARE(result.message,
             QStringLiteral("Captured 2/2 settings pages (2 monitors detected). Not on this PC: "
                            "WiFi_Settings: this PC has no Wi-Fi adapter"));
    QVERIFY(result.log.contains(
        QStringLiteral("\nNOT_ON_THIS_PC:WiFi_Settings: this PC has no Wi-Fi adapter\n")));

    const QString text = Action::buildScreenshotReportText(
        1, cap, QStringLiteral("C:/out"), QStringLiteral("20261106_010203"));
    QVERIFY(text.contains(QStringLiteral("NOT ON THIS PC")));
    QVERIFY(text.contains(QStringLiteral("| [-] WiFi_Settings: this PC has no Wi-Fi adapter")));
}

void ScreenshotSettingsActionTests::aBlankPictureIsAFailedCapture() {
    QImage blank(1024, 768, QImage::Format_RGB32);
    blank.fill(QColor(32, 32, 32));
    QVERIFY(ScreenshotSettingsAction::pictureIsBlank(blank));
    QVERIFY(ScreenshotSettingsAction::pictureIsBlank(QImage()));
    QImage page = blank;
    for (int x = 100; x < 400; ++x) {
        page.setPixel(x, 200, qRgb(240, 240, 240));
        page.setPixel(x, 201, qRgb(240, 240, 240));
    }
    for (int y = 0; y < 768; ++y) {
        page.setPixel(512, y, qRgb(0, 120, 215));
    }
    QVERIFY(!ScreenshotSettingsAction::pictureIsBlank(page));

    QFile file(QDir(QStringLiteral(SAK_REPO_ROOT))
                   .filePath(QStringLiteral("src/actions/screenshot_settings_action.cpp")));
    QVERIFY(file.open(QIODevice::ReadOnly));
    const QString source = QString::fromUtf8(file.readAll());
    QVERIFY(source.contains(QStringLiteral("PrintWindow(hwnd, memory, kRenderFullContent)")));
    QVERIFY(!source.contains(QStringLiteral("grabWindow(")));
    QVERIFY(source.contains(QStringLiteral("EnumChildWindows(")));
}

void ScreenshotSettingsActionTests::settingsWindowIsFoundAndWaitedForWithoutTasklistOrKills() {
    QFile file(QDir(QStringLiteral(SAK_REPO_ROOT))
                   .filePath(QStringLiteral("src/actions/screenshot_settings_action.cpp")));
    QVERIFY(file.open(QIODevice::ReadOnly));
    const QString source = QString::fromUtf8(file.readAll());
    for (const QString& gone : {QStringLiteral("taskkill"),
                                QStringLiteral("tasklist"),
                                QStringLiteral("/IM"),
                                QStringLiteral("kSettingsCaptureInitialWaitMs"),
                                QStringLiteral("kTimerRetryBaseMs")}) {
        QVERIFY2(!source.contains(gone), qPrintable(gone));
    }
    for (const QString& present :
         {QStringLiteral("EnumWindows"),
          QStringLiteral("WM_CLOSE"),
          QStringLiteral("SendMessageTimeoutW"),
          QStringLiteral("windowToClose(m_settings_window_before_launch")}) {
        QVERIFY2(source.contains(present), qPrintable(present));
    }
}

QTEST_GUILESS_MAIN(ScreenshotSettingsActionTests)
#include "test_screenshot_settings_action.moc"
