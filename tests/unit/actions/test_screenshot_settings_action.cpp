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
#include <utility>

using sak::ScreenshotSettingsAction;

/// Friend of ScreenshotSettingsAction: reaches the private report seams.
class ScreenshotSettingsActionTests : public QObject {
    Q_OBJECT

    using Action = ScreenshotSettingsAction;
    using CaptureResult = ScreenshotSettingsAction::CaptureResult;
    using CaptureContext = ScreenshotSettingsAction::CaptureContext;

private Q_SLOTS:
    void aRunStartsOnlyWithNoSettingsWindowOpen();
    void settingsWindowIsFoundAndWaitedForWithoutTasklistOrKills();
    void aBlankPictureIsAFailedCapture();
    void picturesOfOnePageDifferByAFewPixelsOnly();
    void aPageIsSavedOnlyOnceItHasHeldStill();
    void aWindowOfItsOwnMustSettleOnTheAboutLearnedFirst();
    void aboutIsKnownOnlyWhenTwoWindowsAgree();
    void everyPageIsOpenedFromAboutInAWindowOfItsOwn();
    void aLinkThatLeavesAboutOnScreenIsNoPage();
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

namespace {

constexpr int kPictureWidth = 100;
constexpr int kPictureHeight = 80;
constexpr int kContentLeft = 10;
constexpr int kContentRight = 90;
constexpr int kContentTop = 10;

// A picture of a Settings page: the light background, and as many rows of ink as the page has
// content. About has 10 rows; a page such as Display has 40, as different as real pages are
// (on testpc1 every page differed from About in 22 to 42 % of the window).
QImage pagePicture(int content_rows, QRgb ink) {
    QImage page(kPictureWidth, kPictureHeight, QImage::Format_RGB32);
    page.fill(QColor(243, 243, 243));
    for (int y = kContentTop; y < kContentTop + content_rows; ++y) {
        for (int x = kContentLeft; x < kContentRight; ++x) {
            page.setPixel(x, y, ink);
        }
    }
    return page;
}

QImage aboutPicture() {
    return pagePicture(10, qRgb(0, 0, 0));
}

QImage displayPicture() {
    return pagePicture(40, qRgb(0, 120, 215));
}

// @p page with @p count pixels of its last rows changed: a caret, a scroll bar fading in, a
// highlight under the pointer.
QImage withPixelsChanged(const QImage& page, int count) {
    QImage changed = page;
    for (int i = 0; i < count; ++i) {
        const int x = i % kPictureWidth;
        const int y = kPictureHeight - 1 - (i / kPictureWidth);
        changed.setPixel(x, y, qRgb(255, 0, 255));
    }
    return changed;
}

qint64 picturePixels() {
    return static_cast<qint64>(kPictureWidth) * kPictureHeight;
}

}  // namespace

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

namespace {

QString actionSource() {
    QFile file(QDir(QStringLiteral(SAK_REPO_ROOT))
                   .filePath(QStringLiteral("src/actions/screenshot_settings_action.cpp")));
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }
    // Line ends as git left them on this checkout; the checks below read "\n" alone.
    return QString::fromUtf8(file.readAll()).remove(QLatin1Char('\r'));
}

// The definition of ScreenshotSettingsAction::@p function, up to its closing brace in column 0.
QString functionBody(const QString& source, const QString& function) {
    const qsizetype start =
        source.indexOf(QStringLiteral("ScreenshotSettingsAction::%1(").arg(function));
    if (start < 0) {
        return {};
    }
    const qsizetype end = source.indexOf(QStringLiteral("\n}\n"), start);
    return end < 0 ? QString() : source.mid(start, end - start);
}

}  // namespace

// The action used to force-kill every SystemSettings.exe by name, and later navigated a Settings
// window the user had open. Each page is now opened in a Settings window of its own, which is
// then closed, and Settings has one window: so the run does not start while one is open, before
// it makes its folder or opens anything. Every window it closes is then one it opened.
void ScreenshotSettingsActionTests::aRunStartsOnlyWithNoSettingsWindowOpen() {
    const QString source = actionSource();
    QVERIFY(!source.isEmpty());
    const QString run = functionBody(source, QStringLiteral("execute"));
    const qsizetype refusal =
        run.indexOf(QStringLiteral("if (refuseWhileSettingsIsOpen(start_time)) {"));
    QVERIFY2(refusal > 0, "execute() must refuse to start while Settings is open");
    QVERIFY(run.indexOf(QStringLiteral("createCaptureFolder(output_dir)")) > refusal);
    QVERIFY(run.indexOf(QStringLiteral("startFromAbout(start_time)")) > refusal);
    const QString refuse = functionBody(source, QStringLiteral("refuseWhileSettingsIsOpen"));
    QVERIFY2(refuse.contains(QStringLiteral("    if (settingsWindowIsOpen()) {")),
             "the run must not start while a Settings window is open");
    QVERIFY(refuse.contains(
        QStringLiteral("Settings is open: close it, then run Screenshot Settings again")));
    // Minimized counts as open.
    QVERIFY(source.contains(QStringLiteral("return findSettingsFrame(true) != nullptr;")));
}

// A page drawn half way is not the same picture as the page drawn, but a caret, a scroll bar
// fading in or a highlight under the pointer leave a page the same page: on testpc1 About's own
// blips changed 32 of 765,440 pixels, and every page differed from About in over a fifth.
void ScreenshotSettingsActionTests::picturesOfOnePageDifferByAFewPixelsOnly() {
    const QImage about = aboutPicture();
    const auto allowed = static_cast<int>(picturePixels() / Action::kSamePagePixelDenominator);
    QVERIFY(!Action::picturesDiffer(about, about));
    QVERIFY(!Action::picturesDiffer(about, withPixelsChanged(about, 4)));
    QVERIFY(!Action::picturesDiffer(about, withPixelsChanged(about, allowed)));
    QVERIFY(Action::picturesDiffer(about, withPixelsChanged(about, allowed + 1)));
    QVERIFY(Action::picturesDiffer(about, displayPicture()));
    QVERIFY(Action::picturesDiffer(about, about.scaled(kPictureWidth / 2, kPictureHeight / 2)));
    QVERIFY(Action::picturesDiffer(about, QImage()));
    QVERIFY(Action::picturesDiffer(QImage(), QImage()));
}

// Round 5's final capture VM run saved two of fourteen pages right. The wait counted a picture
// as drawn once it was not the last picture saved and was the same 0.3 s later. In the VM a page
// drawn half way holds far longer than that. Measured with every page opened as the run opens
// it: after Installed apps' link About stayed 9.9 s, then its title over an empty page held
// 3.83 s and six of its 32 apps 3.62 s. Each such picture was saved, the page went on drawing,
// and the next page's wait took the rest as the next page: Activation was saved as About,
// Display as Activation half drawn, Power as Activation. A page is now drawn only once one
// picture, not close to the About its link was opened on, has held still for the settle time.
void ScreenshotSettingsActionTests::aPageIsSavedOnlyOnceItHasHeldStill() {
    const QImage about = aboutPicture();
    const QImage title_only = pagePicture(3, qRgb(0, 120, 215));
    const QImage six_apps = pagePicture(20, qRgb(0, 120, 215));
    const QImage installed_apps = displayPicture();
    constexpr qint64 kLookMs = 300;
    Action::PageWatch watch;
    watch.opened_on = about;
    QCOMPARE(watch.settle_ms, Action::kPageSettleMs);
    qint64 now = 0;
    for (; now < 9900; now += kLookMs) {
        QVERIFY(watch.look(about, now).isNull());
        QVERIFY(watch.look(withPixelsChanged(about, 30), now + 1).isNull());
    }
    for (const auto& [picture, held_ms] :
         {std::pair{title_only, qint64{3830}}, std::pair{six_apps, qint64{3620}}}) {
        const qint64 first_seen = now;
        for (; now <= first_seen + held_ms; now += kLookMs) {
            QVERIFY2(watch.look(picture, now).isNull(), "a page drawn half way was saved");
        }
    }
    const qint64 drawn_from = now;
    QVERIFY(watch.look(installed_apps, drawn_from).isNull());
    QVERIFY(watch.look(installed_apps, drawn_from + Action::kPageSettleMs - 1).isNull());
    QCOMPARE(watch.look(installed_apps, drawn_from + Action::kPageSettleMs), installed_apps);
    QVERIFY(watch.left_opened_on);

    // Twice the longest any page held drawn half way, in the VM and on testpc1.
    QVERIFY(Action::kPageSettleMs >= 2 * 3830);

    // A blank picture never counts, and restarts the wait for a still picture.
    QImage blank(kPictureWidth, kPictureHeight, QImage::Format_RGB32);
    blank.fill(QColor(243, 243, 243));
    Action::PageWatch flicker;
    flicker.opened_on = about;
    QVERIFY(flicker.look(installed_apps, 0).isNull());
    QVERIFY(flicker.look(blank, Action::kPageSettleMs / 2).isNull());
    QVERIFY(flicker.look(installed_apps, Action::kPageSettleMs).isNull());
    QCOMPARE(flicker.look(installed_apps, 2 * Action::kPageSettleMs), installed_apps);
}

// The first page has nothing before it to compare against: in round 5's final VM run About was
// saved as Game Bar, the page Settings had shown last time, held still in a new window. Every
// later window of its own must settle on the About learned at the start, however long something
// else holds still in it.
void ScreenshotSettingsActionTests::aWindowOfItsOwnMustSettleOnTheAboutLearnedFirst() {
    const QImage about = aboutPicture();
    const QImage game_bar = pagePicture(30, qRgb(16, 124, 16));
    Action::PageWatch fresh = Action::aboutWatch(about);
    QCOMPARE(fresh.expected, about);
    for (qint64 now = 0; now <= 4 * Action::kPageSettleMs; now += Action::kPageSettleMs / 4) {
        QVERIFY2(fresh.look(game_bar, now).isNull(), "a left-over page was taken for About");
    }
    // Once About is known, a picture close to it needs only the shorter hold: nothing else can
    // pass for it, however long it holds.
    QVERIFY(Action::kAboutSettleMs < Action::kPageSettleMs);
    const qint64 about_from = 5 * Action::kPageSettleMs;
    QVERIFY(fresh.look(withPixelsChanged(about, 30), about_from).isNull());
    QVERIFY(
        fresh.look(withPixelsChanged(about, 30), about_from + Action::kAboutSettleMs - 1).isNull());
    QCOMPARE(fresh.look(withPixelsChanged(about, 30), about_from + Action::kAboutSettleMs),
             withPixelsChanged(about, 30));

    // Before About is known nothing can be checked against, so a window of its own waits the
    // full page settle time, and two such windows must agree.
    Action::PageWatch learning = Action::aboutWatch(QImage());
    QVERIFY(learning.expected.isNull());
    QCOMPARE(learning.settle_ms, Action::kPageSettleMs);
    QVERIFY(learning.look(about, 0).isNull());
    QVERIFY(learning.look(about, Action::kAboutSettleMs).isNull());
    QCOMPARE(learning.look(about, Action::kPageSettleMs), about);
}

// So About itself is learned from two windows of its own that agree: a left-over page or a
// splash that holds still in one window does not hold in the next.
void ScreenshotSettingsActionTests::aboutIsKnownOnlyWhenTwoWindowsAgree() {
    const QImage about = aboutPicture();
    const QImage game_bar = pagePicture(30, qRgb(16, 124, 16));
    QVERIFY(!Action::aboutWindowsAgree(QImage(), about));  // a first window agrees with nothing
    QVERIFY(!Action::aboutWindowsAgree(game_bar, about));
    QVERIFY(!Action::aboutWindowsAgree(about, QImage()));
    QVERIFY(Action::aboutWindowsAgree(about, about));
    QVERIFY(Action::aboutWindowsAgree(about, withPixelsChanged(about, 30)));

    const QString learn = functionBody(actionSource(), QStringLiteral("learnAboutPage"));
    QVERIFY(learn.contains(QStringLiteral("m_about_page = QImage();")));  // learned every run
    QVERIFY(learn.contains(QStringLiteral("if (aboutWindowsAgree(previous, about)) {")));
}

// Coordinator, round 5: open each page so it cannot inherit the last one, and compare it against
// what Settings showed when its link was opened. Each try closes the window before it, opens
// About in a window of its own, waits for the About learned at the start, and only then opens
// the page's link, in that window, watching for a picture that is not that About.
void ScreenshotSettingsActionTests::everyPageIsOpenedFromAboutInAWindowOfItsOwn() {
    const QString source = actionSource();
    const QString own_window = functionBody(source, QStringLiteral("openAboutInItsOwnWindow"));
    const qsizetype closed =
        own_window.indexOf(QStringLiteral("if (!closeSettingsWindowWeOpened() ||"));
    QVERIFY2(closed > 0, "a page's window must be opened only after the one before is closed");
    QVERIFY(own_window.indexOf(QStringLiteral("!openSettingsLink(kAboutUri)")) > closed);
    QVERIFY(own_window.contains(QStringLiteral("PageWatch watch = aboutWatch(m_about_page);")));

    const QString one_try = functionBody(source, QStringLiteral("tryPageFromAbout"));
    const qsizetype about_first = one_try.indexOf(QStringLiteral("openAboutInItsOwnWindow("));
    const qsizetype compared = one_try.indexOf(QStringLiteral("watch.opened_on = about;"));
    const qsizetype link = one_try.indexOf(QStringLiteral("openSettingsLink(ms_uri)"));
    QVERIFY(about_first > 0);
    QVERIFY(compared > about_first);
    QVERIFY(link > compared);

    // About itself is the picture two windows agreed on, saved without a third.
    const QString uri = functionBody(source, QStringLiteral("captureSettingsUri"));
    QVERIFY(uri.contains(
        QStringLiteral("return savePage(m_about_page, output_dir, page_name, timestamp);")));
    // Settings is brought to the front first: a window behind another one stops drawing.
    QVERIFY(source.contains(QStringLiteral("SetForegroundWindow(hwnd);")));
}

// Coordinator, round 5: ms-settings:apps-features and ms-settings:gaming are not pages; on
// testpc1 each left About on screen, to the pixel, for the whole wait. A window that showed the
// About its link was opened on, and nothing else but a few pixels' change, did not open a page,
// and the page is reported as one this Windows does not have; nothing is saved for it. No
// picture, a blank one, or a page that never held still is a failed try.
void ScreenshotSettingsActionTests::aLinkThatLeavesAboutOnScreenIsNoPage() {
    const QImage about = aboutPicture();
    QImage blank(kPictureWidth, kPictureHeight, QImage::Format_RGB32);
    blank.fill(QColor(243, 243, 243));

    Action::PageWatch stayed;
    stayed.opened_on = about;
    for (qint64 now = 0; now < 30'000; now += 500) {
        QVERIFY(stayed.look(about, now).isNull());
        QVERIFY(stayed.look(withPixelsChanged(about, 30), now + 250).isNull());
    }
    QCOMPARE(stayed.outcomeAtDeadline(), Action::PageOutcome::NotOnThisWindows);

    Action::PageWatch nothing;
    nothing.opened_on = about;
    QVERIFY(nothing.look(QImage(), 0).isNull());
    QVERIFY(nothing.look(blank, 500).isNull());
    QCOMPARE(nothing.outcomeAtDeadline(), Action::PageOutcome::Failed);

    Action::PageWatch never_still;
    never_still.opened_on = about;
    for (qint64 now = 0; now < 30'000; now += 500) {
        const QImage moving = withPixelsChanged(displayPicture(), static_cast<int>(now / 500));
        QVERIFY(never_still.look(moving, now).isNull());
    }
    QCOMPARE(never_still.outcomeAtDeadline(), Action::PageOutcome::Failed);

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

// Eight pages saved as eight 1024x768 pictures of one colour: copying the pixels of a window the
// compositor draws gives that. A blank is now a failed try, and the capture asks the frame window
// a person sees to render itself.
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

// Detection is EnumWindows over visible top-level windows, waits are for state (window present
// and answering), and nothing is force-killed: no taskkill, no tasklist per page, no fixed sleeps.
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
    for (const QString& present : {QStringLiteral("EnumWindows"),
                                   QStringLiteral("WM_CLOSE"),
                                   QStringLiteral("SendMessageTimeoutW"),
                                   QStringLiteral("const HWND hwnd = findSettingsFrame(true);")}) {
        QVERIFY2(source.contains(present), qPrintable(present));
    }
}

QTEST_GUILESS_MAIN(ScreenshotSettingsActionTests)
#include "test_screenshot_settings_action.moc"
