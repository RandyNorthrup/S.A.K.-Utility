// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file screenshot_settings_action.cpp
/// @brief Implements configured Windows Settings page screenshot capture

#include "sak/actions/screenshot_settings_action.h"

#include "sak/action_constants.h"
#include "sak/layout_constants.h"
#include "sak/logger.h"
#include "sak/network_adapter_inspector.h"
#include "sak/process_runner.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDeadlineTimer>
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QGuiApplication>
#include <QImage>
#include <QProcess>
#include <QSaveFile>
#include <QScreen>
#include <QTextStream>
#include <QThread>

#include <algorithm>

#include <Windows.h>

namespace sak {

namespace {
constexpr int kSettingsCaptureFirstAttempt = 1;
constexpr int kSettingsCaptureMaxAttempts = 3;
// Every page is opened from About, in a Settings window of its own. About is known once two
// windows of its own in a row settle on the same picture, in at most this many windows.
constexpr auto kAboutUri = QLatin1StringView("about");
constexpr int kAboutMaxWindows = 4;
// How long a Settings window may take to come up and answer, how often to look, and how long one
// "are you responsive" probe may block. Measured 2026-11-06: a new window showed its page 1.0 to
// 1.8 s after its link on testpc1, and 16.8 to 21.7 s in the capture VM.
constexpr int kSettingsWindowWaitMs = 45'000;
constexpr int kSettingsPollMs = 100;
constexpr UINT kSettingsResponsiveTimeoutMs = 2000;
constexpr int kSettingsCloseWaitMs = 10'000;
// How long a page may take to draw and hold still once its link is opened in About's window.
// Measured: About stayed 0.4 to 1.2 s after the link on testpc1 and 5.8 to 19.4 s in the capture
// VM; there the last page drew by 19.4 s, and Storage, whose loading ring never stops, first
// held still for the settle time 33 s after its link.
constexpr qint64 kSettingsPageDrawWaitMs = 60'000;
constexpr int kSettingsLookMs = 100;
constexpr int kMsPerSecond = 1000;
constexpr int kScreenshotReportFieldWidth = 61;

}  // namespace

ScreenshotSettingsAction::ScreenshotSettingsAction(const QString& output_location, QObject* parent)
    : QuickAction(parent) {
    setOutputFolder(output_location);
}

namespace {

// Settings is drawn by the compositor, so copying its window's pixels (QScreen::grabWindow) gave
// a single colour. PrintWindow with PW_RENDERFULLCONTENT asks the window for what it shows.
QImage renderWindowContent(HWND hwnd) {
    RECT rect{};
    if (hwnd == nullptr || GetWindowRect(hwnd, &rect) == 0) {
        return {};
    }
    const int width = rect.right - rect.left;
    const int height = rect.bottom - rect.top;
    if (width <= 0 || height <= 0) {
        return {};
    }
    HDC screen = GetDC(nullptr);
    HDC memory = CreateCompatibleDC(screen);
    HBITMAP bitmap = CreateCompatibleBitmap(screen, width, height);
    HGDIOBJ previous = SelectObject(memory, bitmap);
    constexpr UINT kRenderFullContent = 0x00'00'00'02;  // PW_RENDERFULLCONTENT
    const bool rendered = PrintWindow(hwnd, memory, kRenderFullContent) != 0;
    SelectObject(memory, previous);
    QImage image = rendered ? QImage::fromHBITMAP(bitmap) : QImage();
    DeleteObject(bitmap);
    DeleteDC(memory);
    ReleaseDC(nullptr, screen);
    return image;
}

}  // namespace

bool ScreenshotSettingsAction::pictureIsBlank(const QImage& image) {
    if (image.isNull()) {
        return true;
    }
    // Every pixel: a Settings page is mostly one background with thin text, and a sampled grid
    // can step over all of it.
    const QImage pixels = image.convertToFormat(QImage::Format_RGB32);
    const QRgb first = pixels.pixel(0, 0);
    for (int y = 0; y < pixels.height(); ++y) {
        const auto* line = reinterpret_cast<const QRgb*>(pixels.constScanLine(y));
        if (std::any_of(line, line + pixels.width(), [first](QRgb p) { return p != first; })) {
            return false;
        }
    }
    return true;
}

namespace {

qint64 differingPixels(const QRgb* left, const QRgb* right, int width) {
    qint64 differing = 0;
    for (int x = 0; x < width; ++x) {
        differing += (left[x] & RGB_MASK) != (right[x] & RGB_MASK) ? 1 : 0;
    }
    return differing;
}

}  // namespace

bool ScreenshotSettingsAction::picturesDiffer(const QImage& a, const QImage& b) {
    if (a.isNull() || b.isNull() || a.size() != b.size()) {
        return true;
    }
    const QImage left = a.convertToFormat(QImage::Format_RGB32);
    const QImage right = b.convertToFormat(QImage::Format_RGB32);
    const qint64 allowed = static_cast<qint64>(left.width()) * left.height() /
                           kSamePagePixelDenominator;
    qint64 differing = 0;
    for (int y = 0; y < left.height(); ++y) {
        differing += differingPixels(reinterpret_cast<const QRgb*>(left.constScanLine(y)),
                                     reinterpret_cast<const QRgb*>(right.constScanLine(y)),
                                     left.width());
        if (differing > allowed) {
            return true;
        }
    }
    return false;
}

QImage ScreenshotSettingsAction::renderOnGui(WId window) {
    // execute() runs on the quick-action worker thread; the picture is taken on the GUI thread,
    // and the worker blocks until it is. Fail closed (a null picture) if there is no application
    // object or the invocation cannot be marshalled.
    QCoreApplication* app = QCoreApplication::instance();
    if (app == nullptr) {
        sak::logError("ScreenshotSettingsAction: no QCoreApplication to marshal capture onto");
        return {};
    }
    if (QThread::currentThread() == app->thread()) {
        return renderWindowContent(reinterpret_cast<HWND>(window));
    }
    QImage shot;
    const bool invoked = QMetaObject::invokeMethod(
        app,
        [window, &shot]() { shot = renderWindowContent(reinterpret_cast<HWND>(window)); },
        Qt::BlockingQueuedConnection);
    if (!invoked) {
        sak::logError("ScreenshotSettingsAction: failed to marshal capture onto the GUI thread");
        return {};
    }
    return shot;
}

QImage ScreenshotSettingsAction::PageWatch::look(const QImage& now, qint64 now_ms) {
    if (pictureIsBlank(now)) {
        held = QImage();
        return {};
    }
    if (now != held) {
        held = now;
        held_since_ms = now_ms;
    }
    if (!opened_on.isNull() && !picturesDiffer(now, opened_on)) {
        // Settings still shows the page it was on when the link was opened.
        saw_opened_on = true;
        return {};
    }
    left_opened_on = true;
    // Drawn means held still: in the capture VM a page drawn half way (its title over an empty
    // page) held for up to 3.83 s, and with a 0.3 s wait such a picture was saved, and the rest
    // of the page was saved under the next page's name.
    if (now_ms - held_since_ms < settle_ms) {
        return {};
    }
    // A window of its own that is meant to show About shows About, not whatever it drew first.
    if (!expected.isNull() && picturesDiffer(now, expected)) {
        return {};
    }
    return now;
}

ScreenshotSettingsAction::PageOutcome ScreenshotSettingsAction::PageWatch::outcomeAtDeadline()
    const {
    // Settings stays on the page it showed when it is asked for a page this Windows does not
    // have: on testpc1 (Windows 11 25H2) ms-settings:apps-features left System > About on screen.
    return saw_opened_on && !left_opened_on ? PageOutcome::NotOnThisWindows : PageOutcome::Failed;
}

ScreenshotSettingsAction::PageOutcome ScreenshotSettingsAction::waitForDrawnPage(WId window,
                                                                                 PageWatch* watch,
                                                                                 QImage* page) {
    // A Settings window behind another one stops drawing, so it would never show the new page.
    const auto hwnd = reinterpret_cast<HWND>(window);
    if (IsIconic(hwnd) != 0) {
        ShowWindow(hwnd, SW_RESTORE);
    }
    SetForegroundWindow(hwnd);
    QElapsedTimer clock;
    clock.start();
    while (!isCancelled()) {
        QImage drawn = watch->look(renderOnGui(window), clock.elapsed());
        if (!drawn.isNull()) {
            *page = std::move(drawn);
            return PageOutcome::Captured;
        }
        if (clock.elapsed() >= kSettingsPageDrawWaitMs) {
            return watch->outcomeAtDeadline();
        }
        QThread::msleep(kSettingsLookMs);
    }
    return PageOutcome::Failed;
}

void ScreenshotSettingsAction::recordOutcome(const QString& page_name,
                                             PageOutcome outcome,
                                             bool no_wifi_adapter,
                                             CaptureResult* capture) {
    switch (outcome) {
    case PageOutcome::Captured:
        capture->captured_pages.append(page_name);
        capture->screenshots_taken++;
        break;
    case PageOutcome::NotOnThisWindows:
        capture->absent_pages.append(
            QStringLiteral("%1: %2").arg(page_name, absentReason(outcome, no_wifi_adapter)));
        break;
    case PageOutcome::Failed:
        capture->failed_pages.append(page_name);
        capture->failed_attempts++;
        break;
    }
}

void ScreenshotSettingsAction::execute() {
    if (isCancelled()) {
        emitCancelledResult("Screenshot capture cancelled");
        return;
    }
    if (rejectInvalidOutputFolder()) {
        return;
    }
    setStatus(ActionStatus::Running);
    const QDateTime start_time = QDateTime::currentDateTime();
    Q_EMIT executionProgress("Detecting monitor configuration...", progress::kStep3);
    const int monitor_count = detectMonitorCount();
    if (refuseWhileSettingsIsOpen(start_time)) {
        return;
    }

    Q_EMIT executionProgress("Preparing screenshot directory...", progress::kStep5);
    const QString timestamp = QDateTime::currentDateTime().toString("yyyyMMdd_HHmmss");
    const QDir output_dir(outputFolder() + "/SettingsScreenshots/" + timestamp);
    // Fail closed: without the folder every capture would fail one by one, after Settings pages
    // had been opened for nothing.
    if (const QString folder_error = createCaptureFolder(output_dir); !folder_error.isEmpty()) {
        emitFailedResult(folder_error, folder_error, start_time);
        return;
    }
    if (!startFromAbout(start_time)) {
        return;
    }
    const QList<SettingsPage> settings_pages = settingsPages();
    // Asked once: Settings has no Wi-Fi page on a PC without an adapter. When the adapters cannot
    // be listed, the page is tried like any other.
    const bool no_wifi_adapter = wifiAdapterPresent() == std::optional<bool>(false);
    CaptureResult capture;
    int processed = 0;

    for (const SettingsPage& page : settings_pages) {
        if (isCancelled()) {
            closeSettingsWindowWeOpened();
            emitCancelledResult("Settings screenshots cancelled", start_time);
            return;
        }

        const int progress = static_cast<int>(
            progress::kStep5 + ((processed * progress::kStep90) / settings_pages.size()));
        Q_EMIT executionProgress(QString("Capturing %1...").arg(page.name), progress);
        processed++;

        const bool skip_for_no_wifi = page.needs_wifi && no_wifi_adapter;
        const PageOutcome outcome =
            skip_for_no_wifi ? PageOutcome::NotOnThisWindows
                             : captureSettingsPage(page, output_dir.absolutePath(), timestamp);
        recordOutcome(page.name, outcome, skip_for_no_wifi, &capture);
    }
    closeSettingsWindowWeOpened();

    Q_EMIT executionProgress("Generating report...", progress::kStep95);
    const bool report_written =
        generateReport(output_dir.absolutePath(), timestamp, monitor_count, capture);
    Q_EMIT executionProgress("Screenshots complete", progress::kComplete);

    const CaptureContext context{.total_pages = static_cast<int>(settings_pages.size()) -
                                                static_cast<int>(capture.absent_pages.size()),
                                 .monitor_count = monitor_count,
                                 .timestamp = timestamp,
                                 .start_time = start_time,
                                 .report_written = report_written};
    buildExecutionResult(capture, output_dir, context);
}

namespace {

/// ". Not on this PC: WiFi_Settings: this PC has no Wi-Fi adapter", or nothing.
QString notOnThisPcNote(const QStringList& absent_pages) {
    return absent_pages.isEmpty()
               ? QString()
               : QStringLiteral(". Not on this PC: ") + absent_pages.join(QStringLiteral("; "));
}

}  // namespace

void ScreenshotSettingsAction::buildExecutionResult(const CaptureResult& capture,
                                                    const QDir& output_dir,
                                                    const CaptureContext& context) {
    const qint64 duration_ms = context.start_time.msecsTo(QDateTime::currentDateTime());

    ExecutionResult result;
    result.duration_ms = duration_ms;
    result.files_processed = capture.screenshots_taken;
    result.output_path = output_dir.absolutePath();

    QString structured_log;
    structured_log += QString("MONITORS_DETECTED:%1\n").arg(context.monitor_count);
    structured_log += QString("SUCCESSFUL_CAPTURES:%1\n").arg(capture.captured_pages.size());
    structured_log += QString("FAILED_CAPTURES:%1\n").arg(capture.failed_attempts);
    structured_log += QString("TOTAL_PAGES:%1\n").arg(context.total_pages);
    structured_log += QString("SUCCESS_RATE:%1%\n")
                          .arg(context.total_pages > 0 ? capture.captured_pages.size() *
                                                             kPercentMax / context.total_pages
                                                       : 0);
    for (const QString& absent : capture.absent_pages) {
        structured_log += QStringLiteral("NOT_ON_THIS_PC:%1\n").arg(absent);
    }
    const QString report_path =
        output_dir.filePath(QString("Screenshot_Report_%1.txt").arg(context.timestamp));
    structured_log += reportPathLine(context.report_written, report_path);

    // Fail closed: only a full capture (every page) with a durably written report
    // is a success. A partial capture or an unwritten report is surfaced as a
    // failure rather than passed off as a complete settings backup.
    const bool full_success = capture.screenshots_taken > 0 && capture.failed_attempts == 0 &&
                              context.report_written;
    result.success = full_success;

    if (full_success) {
        result.message = QString("Captured %1/%2 settings pages (%3 %4 detected)")
                             .arg(capture.captured_pages.size())
                             .arg(context.total_pages)
                             .arg(context.monitor_count)
                             .arg(context.monitor_count == 1 ? QStringLiteral("monitor")
                                                             : QStringLiteral("monitors"));
        // What this PC does not have is said, not counted as a failure.
        result.message += notOnThisPcNote(capture.absent_pages);
        result.log = structured_log + QString("\nSaved to: %1").arg(output_dir.absolutePath());
    } else if (capture.screenshots_taken == 0) {
        result.message = "Failed to capture any screenshots";
        result.log = structured_log + "\nCheck display permissions and Settings app availability";
    } else {
        result.message = QString("Incomplete: captured %1/%2 pages, %3 failed%4")
                             .arg(capture.captured_pages.size())
                             .arg(context.total_pages)
                             .arg(capture.failed_attempts)
                             .arg(context.report_written ? QString()
                                                         : QStringLiteral(", report not written"));
        result.message += notOnThisPcNote(capture.absent_pages);
        result.log = structured_log + QString("\nSaved to: %1").arg(output_dir.absolutePath());
    }

    finishWithResult(result, result.success ? ActionStatus::Success : ActionStatus::Failed);
}

// --- Private Helpers ------------------------------------------------------------

QList<ScreenshotSettingsAction::SettingsPage> ScreenshotSettingsAction::settingsPages() {
    // The names are Microsoft's published ms-settings: list ("Launch Windows Settings",
    // learn.microsoft.com/windows/apps/develop/launch/launch-settings, read 2026-11-06), which
    // gives these pages one name on Windows 10 and 11. The second name, where there is one, is
    // the one Windows 11 also answers to (opened on testpc1, 25H2). "apps-features" and "gaming"
    // were here before and are no page at all: Settings stayed on the page before them.
    return {
        {.name = QStringLiteral("System_About"), .uris = {QStringLiteral("about")}},
        {.name = QStringLiteral("System_Activation"), .uris = {QStringLiteral("activation")}},
        {.name = QStringLiteral("Display_Settings"), .uris = {QStringLiteral("display")}},
        {.name = QStringLiteral("Power_Sleep"), .uris = {QStringLiteral("powersleep")}},
        {.name = QStringLiteral("Storage_Settings"), .uris = {QStringLiteral("storagesense")}},
        {.name = QStringLiteral("Sound_Settings"), .uris = {QStringLiteral("sound")}},
        {.name = QStringLiteral("Notifications"), .uris = {QStringLiteral("notifications")}},
        {.name = QStringLiteral("Personalization"), .uris = {QStringLiteral("personalization")}},
        {.name = QStringLiteral("Privacy_General"),
         .uris = {QStringLiteral("privacy"), QStringLiteral("privacy-general")}},
        {.name = QStringLiteral("Windows_Update"), .uris = {QStringLiteral("windowsupdate")}},
        {.name = QStringLiteral("Network_Status"),
         .uris = {QStringLiteral("network-status"), QStringLiteral("network")}},
        {.name = QStringLiteral("Ethernet_Settings"), .uris = {QStringLiteral("network-ethernet")}},
        {.name = QStringLiteral("WiFi_Settings"),
         .uris = {QStringLiteral("network-wifi")},
         .needs_wifi = true},
        {.name = QStringLiteral("Installed_Apps"),
         .uris = {QStringLiteral("appsfeatures"), QStringLiteral("installed-apps")}},
        // Game Mode is the gaming setting that changes how the PC runs; Game Bar is the page
        // Windows 10 opens Gaming on.
        {.name = QStringLiteral("Gaming_Settings"),
         .uris = {QStringLiteral("gaming-gamemode"), QStringLiteral("gaming-gamebar")}},
    };
}

bool ScreenshotSettingsAction::hasWifiAdapter(const QVector<NetworkAdapterInfo>& adapters) {
    return std::ranges::any_of(adapters, [](const NetworkAdapterInfo& adapter) {
        return adapter.adapterType == QStringLiteral("WiFi");
    });
}

std::optional<bool> ScreenshotSettingsAction::wifiAdapterPresent() {
    // GetAdaptersAddresses lists the adapters Windows has, connected or not; one turned off is not
    // listed, and Settings has no Wi-Fi page for it either.
    NetworkAdapterInspector inspector;
    std::optional<bool> present;
    QObject::connect(&inspector,
                     &NetworkAdapterInspector::scanComplete,
                     &inspector,
                     [&present](const QVector<NetworkAdapterInfo>& adapters) {
                         present = hasWifiAdapter(adapters);
                     });
    inspector.scan();
    return present;
}

QString ScreenshotSettingsAction::absentReason(PageOutcome outcome, bool no_wifi_adapter) {
    Q_ASSERT(outcome == PageOutcome::NotOnThisWindows);
    return no_wifi_adapter ? QStringLiteral("this PC has no Wi-Fi adapter")
                           : QStringLiteral("this page does not exist on this Windows");
}

namespace {

// Whether @p hwnd belongs to a process whose executable is exactly SystemSettings.exe.
bool windowIsSettingsProcess(HWND hwnd) {
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    HANDLE proc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!proc) {
        return false;
    }
    wchar_t exe_path[MAX_PATH] = {};
    DWORD path_size = MAX_PATH;
    bool found = false;
    if (QueryFullProcessImageNameW(proc, 0, exe_path, &path_size)) {
        // Require an EXACT executable-name match, not a substring anywhere in the path: a decoy
        // exe under a folder named SystemSettings (or SystemSettingsX.exe) is not Settings.
        const QString exe_name = QFileInfo(QString::fromWCharArray(exe_path)).fileName();
        found = exe_name.compare(QStringLiteral("SystemSettings.exe"), Qt::CaseInsensitive) == 0;
    }
    CloseHandle(proc);
    return found;
}

struct SettingsFrameSearch {
    bool include_minimized{false};
    HWND frame{nullptr};
};

// Settings is a Store app: the window a person sees is ApplicationFrameHost's frame, with the
// SystemSettings.exe window inside it. The visible frame holding that window is the one to
// capture and to close. SystemSettings.exe's own top-level window was captured before, and gave
// a blank 1024x768 picture for every page.
HWND findSettingsFrame(bool include_minimized) {
    SettingsFrameSearch search{.include_minimized = include_minimized};
    EnumWindows(
        [](HWND hwnd, LPARAM lparam) -> BOOL {
            auto* wanted = reinterpret_cast<SettingsFrameSearch*>(lparam);
            if (!IsWindowVisible(hwnd) || (IsIconic(hwnd) && !wanted->include_minimized)) {
                return TRUE;
            }
            bool hosts_settings = false;
            EnumChildWindows(
                hwnd,
                [](HWND child, LPARAM found) -> BOOL {
                    if (windowIsSettingsProcess(child)) {
                        *reinterpret_cast<bool*>(found) = true;
                        return FALSE;
                    }
                    return TRUE;
                },
                reinterpret_cast<LPARAM>(&hosts_settings));
            if (hosts_settings) {
                wanted->frame = hwnd;
                return FALSE;
            }
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&search));
    return search.frame;
}

}  // namespace

WId ScreenshotSettingsAction::findSettingsWindow() {
    return reinterpret_cast<WId>(findSettingsFrame(false));
}

bool ScreenshotSettingsAction::settingsWindowIsOpen() {
    return findSettingsFrame(true) != nullptr;
}

WId ScreenshotSettingsAction::findRespondingSettingsWindow() {
    // Wait for state, not for a fixed time: the window exists, is visible, and its UI thread
    // answers (it has processed the navigation message that opened this page).
    QDeadlineTimer deadline(kSettingsWindowWaitMs);
    for (;;) {
        if (isCancelled()) {
            return 0;
        }
        const auto hwnd = reinterpret_cast<HWND>(findSettingsWindow());
        DWORD_PTR ignored = 0;
        if (hwnd != nullptr && SendMessageTimeoutW(hwnd,
                                                   WM_NULL,
                                                   0,
                                                   0,
                                                   SMTO_ABORTIFHUNG | SMTO_BLOCK,
                                                   kSettingsResponsiveTimeoutMs,
                                                   &ignored) != 0) {
            return reinterpret_cast<WId>(hwnd);
        }
        if (deadline.hasExpired()) {
            return 0;
        }
        QThread::msleep(kSettingsPollMs);
    }
}

bool ScreenshotSettingsAction::closeSettingsWindowWeOpened() {
    // The run starts only with no Settings window open, so any Settings window now is one this
    // run opened. Minimized or not, it is asked to close (WM_CLOSE), as its own close button
    // would; nothing is force-killed. Then the wait for it to go is bounded.
    const HWND hwnd = findSettingsFrame(true);
    if (hwnd == nullptr) {
        return true;
    }
    PostMessageW(hwnd, WM_CLOSE, 0, 0);
    QDeadlineTimer deadline(kSettingsCloseWaitMs);
    while (IsWindow(hwnd) && !deadline.hasExpired()) {
        QThread::msleep(kSettingsPollMs);
    }
    if (IsWindow(hwnd)) {
        Q_EMIT logMessage(QStringLiteral("Settings window did not close within %1 s; left open")
                              .arg(kSettingsCloseWaitMs / kMsPerSecond));
        return false;
    }
    return true;
}

bool ScreenshotSettingsAction::openSettingsLink(const QString& ms_uri) {
    // explorer.exe by absolute Windows-directory path (it is NOT a System32 binary): a
    // shell-open resolves through the same CreateProcess search order as any other launch, so a
    // bare name lets a planted explorer.exe run. Refuse the try rather than fall back to it.
    return sak::startDetachedWindowsTool(QStringLiteral("explorer.exe"),
                                         {QString("ms-settings:%1").arg(ms_uri)});
}

ScreenshotSettingsAction::PageOutcome ScreenshotSettingsAction::openAboutInItsOwnWindow(
    WId* window, QImage* about) {
    // A window of its own: the one before is closed first, so no page is drawn over another
    // page's leftovers.
    if (!closeSettingsWindowWeOpened() || !openSettingsLink(kAboutUri)) {
        return PageOutcome::Failed;
    }
    *window = findRespondingSettingsWindow();
    if (*window == 0) {
        // Refuse to capture an arbitrary foreground window: the run asked for Settings.
        sak::logWarning("ScreenshotSettingsAction: no Settings window answered within {} s",
                        kSettingsWindowWaitMs / kMsPerSecond);
        return PageOutcome::Failed;
    }
    PageWatch watch = aboutWatch(m_about_page);
    return waitForDrawnPage(*window, &watch, about);
}

ScreenshotSettingsAction::PageWatch ScreenshotSettingsAction::aboutWatch(
    const QImage& learned_about) {
    PageWatch watch;
    watch.expected = learned_about;
    watch.settle_ms = learned_about.isNull() ? kPageSettleMs : kAboutSettleMs;
    return watch;
}

bool ScreenshotSettingsAction::learnAboutPage() {
    // A new window has nothing before it to compare against: its first steady picture can be
    // Settings' splash or a page it drew last time (in the capture VM the first page was saved
    // as Game Bar). About is known once two windows of its own settle on the same picture. Each
    // run learns it again: the window's size, theme or text may have changed since the last.
    m_about_page = QImage();
    QImage previous;
    for (int window_count = 0; window_count < kAboutMaxWindows && !isCancelled(); ++window_count) {
        WId window = 0;
        QImage about;
        if (openAboutInItsOwnWindow(&window, &about) != PageOutcome::Captured) {
            continue;
        }
        if (aboutWindowsAgree(previous, about)) {
            m_about_page = about;
            return true;
        }
        previous = about;
    }
    return false;
}

bool ScreenshotSettingsAction::aboutWindowsAgree(const QImage& previous, const QImage& now) {
    // A null picture differs from every picture, so a first window agrees with nothing.
    return !picturesDiffer(previous, now);
}

bool ScreenshotSettingsAction::refuseWhileSettingsIsOpen(const QDateTime& start_time) {
    // Each page is opened in a Settings window of its own, which is then closed. Settings has one
    // window, so a link would navigate the user's own window: the run does not start while one
    // is open.
    if (settingsWindowIsOpen()) {
        sak::logWarning("ScreenshotSettingsAction: Settings is open; the run did not start");
        emitFailedResult(
            QStringLiteral("Settings is open: close it, then run Screenshot Settings again"),
            QStringLiteral("Each page is opened in a Settings window of its own, and Settings "
                           "has one window"),
            start_time);
        return true;
    }
    return false;
}

bool ScreenshotSettingsAction::startFromAbout(const QDateTime& start_time) {
    Q_EMIT executionProgress("Opening About in Settings...", progress::kStep5);
    if (learnAboutPage()) {
        return true;
    }
    closeSettingsWindowWeOpened();
    if (isCancelled()) {
        emitCancelledResult("Settings screenshots cancelled", start_time);
        return false;
    }
    sak::logWarning("ScreenshotSettingsAction: no two Settings windows drew About alike in {}",
                    kAboutMaxWindows);
    emitFailedResult(
        QStringLiteral("Settings did not draw its About page alike twice; no page was captured"),
        QStringLiteral("Every page is opened from About, which is known once two Settings "
                       "windows of its own draw it alike"),
        start_time);
    return false;
}

ScreenshotSettingsAction::PageOutcome ScreenshotSettingsAction::tryPageFromAbout(
    const QString& ms_uri, const QString& page_name, QImage* page) {
    WId window = 0;
    QImage about;
    if (openAboutInItsOwnWindow(&window, &about) != PageOutcome::Captured) {
        sak::logWarning("ScreenshotSettingsAction: Settings did not open About for page {}",
                        page_name.toStdString());
        return PageOutcome::Failed;
    }
    // The page is compared against what Settings showed when its link was opened: About, held
    // still and checked against the About learned at the start.
    PageWatch watch;
    watch.opened_on = about;
    if (!openSettingsLink(ms_uri)) {
        return PageOutcome::Failed;
    }
    const PageOutcome outcome = waitForDrawnPage(window, &watch, page);
    if (outcome == PageOutcome::NotOnThisWindows) {
        sak::logWarning("ScreenshotSettingsAction: Settings stayed on About for page {}",
                        page_name.toStdString());
    } else if (outcome == PageOutcome::Failed) {
        sak::logWarning("ScreenshotSettingsAction: Settings did not draw page {} within {} s",
                        page_name.toStdString(),
                        kSettingsPageDrawWaitMs / kMsPerSecond);
    }
    return outcome;
}

ScreenshotSettingsAction::PageOutcome ScreenshotSettingsAction::savePage(const QImage& page,
                                                                         const QDir& output_dir,
                                                                         const QString& page_name,
                                                                         const QString& timestamp) {
    const QString filepath = output_dir.filePath(QString("%1_%2.png").arg(page_name, timestamp));
    return page.save(filepath, "PNG") ? PageOutcome::Captured : PageOutcome::Failed;
}

ScreenshotSettingsAction::PageOutcome ScreenshotSettingsAction::outcomeOfAttempts(
    const QList<PageOutcome>& attempts) {
    if (attempts.contains(PageOutcome::Captured)) {
        return PageOutcome::Captured;
    }
    // Not on this Windows only when every try saw nothing but About after the link: in the
    // capture VM Game Mode once took longer than one wait to draw, and the next try found it.
    const bool never_left = !attempts.isEmpty() &&
                            std::ranges::all_of(attempts, [](PageOutcome outcome) {
                                return outcome == PageOutcome::NotOnThisWindows;
                            });
    return never_left ? PageOutcome::NotOnThisWindows : PageOutcome::Failed;
}

ScreenshotSettingsAction::PageOutcome ScreenshotSettingsAction::captureSettingsUri(
    const QString& ms_uri,
    const QString& page_name,
    const QDir& output_dir,
    const QString& timestamp) {
    // About was drawn alike by two windows of its own before any page was opened.
    if (ms_uri == kAboutUri) {
        return savePage(m_about_page, output_dir, page_name, timestamp);
    }
    QList<PageOutcome> attempts;
    for (int attempt = kSettingsCaptureFirstAttempt; attempt <= kSettingsCaptureMaxAttempts;
         ++attempt) {
        QImage page;
        attempts.append(tryPageFromAbout(ms_uri, page_name, &page));
        if (attempts.last() == PageOutcome::Captured) {
            return savePage(page, output_dir, page_name, timestamp);
        }
        if (isCancelled()) {
            break;
        }
    }
    return outcomeOfAttempts(attempts);
}

ScreenshotSettingsAction::PageOutcome ScreenshotSettingsAction::captureSettingsPage(
    const SettingsPage& page, const QString& output_dir_path, const QString& timestamp) {
    const QDir output_dir(output_dir_path);
    for (const QString& uri : page.uris) {
        const PageOutcome outcome = captureSettingsUri(uri, page.name, output_dir, timestamp);
        if (outcome != PageOutcome::NotOnThisWindows) {
            return outcome;
        }
        Q_EMIT logMessage(
            QStringLiteral("%1: ms-settings:%2 is not a page on this Windows").arg(page.name, uri));
    }
    return PageOutcome::NotOnThisWindows;
}

QString ScreenshotSettingsAction::buildScreenshotReportText(int monitor_count,
                                                            const CaptureResult& capture,
                                                            const QString& output_dir_path,
                                                            const QString& timestamp) {
    const int total_pages =
        static_cast<int>(capture.captured_pages.size() + capture.failed_pages.size());

    QString text;
    QTextStream report(&text);
    report.setEncoding(QStringConverter::Utf8);

    report << "+==============================================================+\n";
    report << "|         WINDOWS SETTINGS SCREENSHOT REPORT                   |\n";
    report << "+==============================================================+\n";
    report << QString("| Timestamp:         %1                    |\n").arg(timestamp);
    report << QString("| Monitors Detected: %1                                       |\n")
                  .arg(monitor_count);
    report << QString("| Total Pages:       %1                                      |\n")
                  .arg(total_pages);
    report << QString("| Successful:        %1                                      |\n")
                  .arg(capture.captured_pages.size());
    report << QString("| Failed:            %1                                       |\n")
                  .arg(capture.failed_attempts);
    report << "+==============================================================+\n";
    report << "|                    CAPTURED PAGES                            |\n";
    report << "+==============================================================+\n";

    for (const QString& page : capture.captured_pages) {
        report << QString("| [x] %1").arg(page).leftJustified(kScreenshotReportFieldWidth, ' ')
               << "|\n";
    }

    if (!capture.failed_pages.isEmpty()) {
        report << "+==============================================================+\n";
        report << "|                     FAILED PAGES                             |\n";
        report << "+==============================================================+\n";
        for (const QString& page : capture.failed_pages) {
            report << QString("| [ ] %1").arg(page).leftJustified(kScreenshotReportFieldWidth, ' ')
                   << "|\n";
        }
    }

    if (!capture.absent_pages.isEmpty()) {
        report << "+==============================================================+\n";
        report << "|                     NOT ON THIS PC                           |\n";
        report << "+==============================================================+\n";
        for (const QString& page : capture.absent_pages) {
            report << QString("| [-] %1").arg(page).leftJustified(kScreenshotReportFieldWidth, ' ')
                   << "|\n";
        }
    }

    report << "+==============================================================+\n";
    report << QString("| Output Location: %1")
                  .arg(QDir::toNativeSeparators(QDir(output_dir_path).absolutePath()))
                  .leftJustified(kScreenshotReportFieldWidth, ' ')
           << "|\n";
    report << "+==============================================================+\n";
    report.flush();
    return text;
}

QString ScreenshotSettingsAction::reportPathLine(bool report_written, const QString& report_path) {
    // Only advertise the report path when the file actually exists; otherwise
    // surface the write failure instead of pointing at a file that was not saved.
    if (report_written) {
        return QString("REPORT_PATH:%1\n").arg(report_path);
    }
    return QStringLiteral("REPORT_WRITE_FAILED:1\n");
}

bool ScreenshotSettingsAction::generateReport(const QString& output_dir_path,
                                              const QString& timestamp,
                                              int monitor_count,
                                              const CaptureResult& capture) {
    const QDir output_dir(output_dir_path);
    const QString report_path =
        output_dir.filePath(QString("Screenshot_Report_%1.txt").arg(timestamp));

    QSaveFile report_file(report_path);
    if (!report_file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        Q_EMIT logMessage("Failed to open screenshot report for writing: " + report_path);
        return false;
    }

    const QByteArray data =
        buildScreenshotReportText(monitor_count, capture, output_dir_path, timestamp).toUtf8();
    if (report_file.write(data) != data.size()) {
        report_file.cancelWriting();
        Q_EMIT logMessage("Incomplete write of screenshot report: " + report_path);
        return false;
    }
    if (!report_file.commit()) {
        Q_EMIT logMessage("Failed to commit screenshot report: " + report_path);
        return false;
    }
    return true;
}

// Helper method: Detect monitor count for multi-monitor support
bool ScreenshotSettingsAction::rejectInvalidOutputFolder() {
    // Fail closed on a blank or relative output location: "" makes the output path
    // "/SettingsScreenshots/..." (drive root) and a relative one resolves against the
    // working directory, so screenshots and the report could land outside the folder the
    // user chose.
    if (!outputFolder().trimmed().isEmpty() && QDir::isAbsolutePath(outputFolder())) {
        return false;
    }
    ExecutionResult result;
    result.success = false;
    result.message = QStringLiteral("Screenshot output location is invalid");
    result.log = QStringLiteral("Refusing to capture to a blank or relative path");
    finishWithResult(result, ActionStatus::Failed);
    return true;
}

QString ScreenshotSettingsAction::createCaptureFolder(const QDir& output_dir) {
    if (output_dir.mkpath(QStringLiteral("."))) {
        return {};
    }
    sak::logWarning("Failed to create screenshot output directory: {}",
                    output_dir.absolutePath().toStdString());
    return QStringLiteral("Could not create the screenshot folder: %1")
        .arg(QDir::toNativeSeparators(output_dir.absolutePath()));
}

int ScreenshotSettingsAction::detectMonitorCount() {
    // QGuiApplication::screens()/QScreen access is GUI-thread-affine, but execute() runs
    // on the quick-action worker thread. Marshal the query onto the GUI thread and block
    // until it finishes; fail closed (0) if there is no application object to marshal onto
    // or the invocation cannot be marshalled -- never touch QScreen off the GUI thread.
    QCoreApplication* app = QCoreApplication::instance();
    if (app == nullptr) {
        sak::logWarning("ScreenshotSettingsAction: no QCoreApplication to detect monitors");
        return 0;
    }
    if (QThread::currentThread() == app->thread()) {
        return countMonitorsOnGui();
    }
    int count = 0;
    const bool invoked = QMetaObject::invokeMethod(
        app, [&count]() { count = countMonitorsOnGui(); }, Qt::BlockingQueuedConnection);
    if (!invoked) {
        sak::logWarning(
            "ScreenshotSettingsAction: failed to marshal monitor detection onto the GUI thread");
        return 0;
    }
    return count;
}

int ScreenshotSettingsAction::countMonitorsOnGui() {
    QList<QScreen*> screens = QGuiApplication::screens();
    const int count = static_cast<int>(screens.size());

    sak::logDebug("Detected {} monitor(s)", count);

    // Log each monitor's properties
    for (int i = 0; i < count; i++) {
        const QScreen* screen = screens[i];
        const QRect geometry = screen->geometry();
        sak::logDebug("Monitor {}: {}x{} at {},{}",
                      i + 1,
                      geometry.width(),
                      geometry.height(),
                      geometry.x(),
                      geometry.y());
    }

    return count;
}

}  // namespace sak
