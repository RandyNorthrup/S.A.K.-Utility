// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file main.cpp
/// @brief SAK Utility main entry point

#include "sak/accessibility_audit.h"
#include "sak/app_paths.h"
#include "sak/crash_reporter.h"
#include "sak/elevation_manager.h"
#include "sak/error_codes.h"
#include "sak/layout_constants.h"
#include "sak/logger.h"
#include "sak/main_window.h"
#include "sak/message_box_helpers.h"
#include "sak/single_instance_guard.h"
#include "sak/splash_screen.h"
#include "sak/version.h"
#include "sak/wheel_policy.h"
#include "sak/win32mcp/win32_mcp_entry.h"
#include "sak/windows11_theme.h"

#include <QApplication>
#include <QByteArray>
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QFontDatabase>
#include <QIcon>
#include <QMessageBox>
#include <QStringList>
#include <QTextStream>
#include <QTimer>
#include <QWidget>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <exception>
#include <expected>
#include <filesystem>
#include <iostream>
#include <memory>
#include <print>
#include <stdexcept>
#include <thread>

#include <windows.h>

namespace {

constexpr int kAccessibilityOutputWriteFailureExitCode = 3;
constexpr int kAccessibilityMissingNamesExitCode = 2;
// How often the GUI thread re-checks the background font warmup while it keeps pumping.
constexpr int kFontWarmupHeartbeatMs = 50;

struct RuntimeOptions {
    bool accessibility_audit{false};
    QString accessibility_audit_output;
    bool startup_smoke_test{false};
    bool no_splash{false};
    /// Started by Run as Admin: wait for the instance that launched this one to exit.
    bool elevated_relaunch{false};
};

void writeStartupSmokePhase(bool startup_smoke_test, const char* phase) {
    if (!startup_smoke_test) {
        return;
    }
    const auto tick = static_cast<unsigned long long>(::GetTickCount64());
    if (std::fprintf(stderr, "SAK_STARTUP_PHASE tick_ms=%llu phase=%s\n", tick, phase) < 0 ||
        std::fflush(stderr) != 0) {
        throw std::runtime_error("Startup smoke phase output failed");
    }
}

QString findSplashPath() {
    const QString app_dir = QCoreApplication::applicationDirPath();
    const QStringList candidates = {app_dir + "/sak_splash.png",
                                    app_dir + "/resources/sak_splash.png",
                                    app_dir + "/../resources/sak_splash.png",
                                    app_dir + "/../sak_splash.png"};

    auto it = std::ranges::find_if(candidates,
                                   [](const QString& p) { return QFileInfo::exists(p); });
    return it != candidates.end() ? *it : QString{};
}

QString findIconPath() {
    const QString app_dir = QCoreApplication::applicationDirPath();
    const QStringList candidates = {app_dir + "/icon.ico",
                                    app_dir + "/resources/icon.ico",
                                    app_dir + "/../resources/icon.ico"};

    auto it = std::ranges::find_if(candidates,
                                   [](const QString& p) { return QFileInfo::exists(p); });
    return it != candidates.end() ? *it : QString{};
}

bool hasRawArgument(int argc, const char* const argv[], const char* name) noexcept {
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], name) == 0) {
            return true;
        }
    }
    return false;
}

QString rawArgumentValue(int argc, char* argv[], const char* prefix) {
    const QString prefix_text = QString::fromLatin1(prefix);
    for (int i = 1; i < argc; ++i) {
        const QString arg = QString::fromLocal8Bit(argv[i]);
        if (arg.startsWith(prefix_text)) {
            return arg.mid(prefix_text.size());
        }
    }
    return {};
}

bool ciStartupSmokeMode() {
    return !qEnvironmentVariableIsEmpty("SAK_STARTUP_SMOKE_HEADLESS") ||
           !qEnvironmentVariableIsEmpty("SAK_STARTUP_SMOKE_CI_HEADLESS");
}

void prepareMainWindowForStartup(sak::MainWindow& main_window, bool headless_smoke_test) {
    if (headless_smoke_test) {
        sak::logInfo(
            "Startup smoke CI headless mode active; main window constructed but not shown");
        return;
    }

    main_window.show();
}

/// Milliseconds since this process was created (0 if Windows cannot say).
qint64 msSinceProcessStart() {
    FILETIME created{};
    FILETIME exited{};
    FILETIME kernel{};
    FILETIME user{};
    if (::GetProcessTimes(::GetCurrentProcess(), &created, &exited, &kernel, &user) == 0) {
        return 0;
    }
    FILETIME now{};
    ::GetSystemTimeAsFileTime(&now);
    const auto ticks = [](const FILETIME& time) {
        return (static_cast<qint64>(time.dwHighDateTime) << 32) |
               static_cast<qint64>(time.dwLowDateTime);
    };
    constexpr qint64 kFileTimeTicksPerMs = 10'000;
    return (std::max)(qint64{0}, (ticks(now) - ticks(created)) / kFileTimeTicksPerMs);
}

// The one info-level startup line. The phase timings that used to fill the log at info on every
// launch (main window construct and show, font warmup, per-panel setup) are debug now.
void logMainWindowReady(bool headless_smoke_test) {
    if (headless_smoke_test) {
        sak::logInfo("Main window initialized - application ready in {} ms", msSinceProcessStart());
        return;
    }

    sak::logInfo("Main window displayed - application ready in {} ms", msSinceProcessStart());
}

QString accessibilityAuditOutputPath() {
    return qApp->property("sakAccessibilityAuditOutput").toString().trimmed();
}

void writeAccessibilityAuditStatusTo(const QString& output_path, const QString& status) {
    if (output_path.isEmpty()) {
        return;
    }
    QFile file(output_path);
    if (file.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Truncate)) {
        QTextStream out(&file);
        out << "SAK_ACCESSIBILITY_AUDIT_RUNNING " << status << "\n";
    }
}

void writeAccessibilityAuditStatus(const QString& status) {
    writeAccessibilityAuditStatusTo(accessibilityAuditOutputPath(), status);
}

bool writeAccessibilityAuditFile(const QString& output_path,
                                 const sak::AccessibilityFindings& findings) {
    QFile file(output_path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Truncate)) {
        return false;
    }
    QTextStream out(&file);
    out << (findings.passed() ? "SAK_ACCESSIBILITY_AUDIT_OK" : "SAK_ACCESSIBILITY_AUDIT_FAILED")
        << " missing=" << findings.missing.size() << " mislabeled=" << findings.mislabeled.size()
        << " hides_text=" << findings.hides_text.size()
        << " undescribed_image=" << findings.undescribed_image.size()
        << " checked=" << findings.checked << "\n";
    for (const auto& entry : findings.missing) {
        out << entry << "\n";
    }
    for (const auto& entry : findings.mislabeled) {
        out << "mislabeled: " << entry << "\n";
    }
    for (const auto& entry : findings.hides_text) {
        out << "hides its text: " << entry << "\n";
    }
    for (const auto& entry : findings.undescribed_image) {
        out << "image with no description: " << entry << "\n";
    }
    return true;
}

int runAccessibilityAudit(const sak::MainWindow& main_window) {
    const sak::AccessibilityFindings findings = sak::auditApplication(main_window);

    const QString output_path = accessibilityAuditOutputPath();
    if (!output_path.isEmpty() && !writeAccessibilityAuditFile(output_path, findings)) {
        return kAccessibilityOutputWriteFailureExitCode;
    }
    return findings.passed() ? 0 : kAccessibilityMissingNamesExitCode;
}

// Qt's DirectWrite font database walks every installed family on first use. With thousands of
// installed fonts that is seconds warm and tens of seconds cold. The walk runs here, off the GUI
// thread, and waitForFontDatabaseWarmup() keeps the GUI pumping until it finishes, so no window
// is ever frozen behind it. Measured 2026-10-24 on a 2491-family machine: the GUI-thread block
// went from 5.8-10.8 s to under 0.15 s. A former synthetic missing-family match (to pre-pay
// Qt's alias walk) cost 1.3-1.5 s per launch and bought nothing: every UI font list names an
// installed family first, so no lookup misses -- do not reintroduce it.
struct FontWarmupResult {
    std::atomic<bool> done{false};
    std::atomic<qsizetype> family_count{0};
    std::atomic<qint64> census_ms{0};
};
FontWarmupResult g_font_warmup;
std::thread g_font_warmup_thread;

void joinFontDatabaseWarmup() {
    if (g_font_warmup_thread.joinable()) {
        g_font_warmup_thread.join();
    }
}

void startFontDatabaseWarmup() {
    g_font_warmup_thread = std::thread([]() {
        QElapsedTimer timer;
        timer.start();
        g_font_warmup.family_count.store(QFontDatabase::families().size(),
                                         std::memory_order_relaxed);
        g_font_warmup.census_ms.store(timer.elapsed(), std::memory_order_relaxed);
        g_font_warmup.done.store(true, std::memory_order_release);
    });
}

/// @brief Keep the GUI thread responsive until the background font warmup finishes.
// SAK-ALLOW-BLOCKING: deliberately unbounded. No window can be built before Qt's font database
// exists, and a deadline would only move the same wait onto a frozen GUI thread.
void waitForFontDatabaseWarmup() {
    QElapsedTimer waited;
    waited.start();
    qint64 longest_gap_ms = 0;
    if (!g_font_warmup.done.load(std::memory_order_acquire)) {
        // SAK-ALLOW-BLOCKING: runs once during startup, before the main window exists, so no
        // application state can be re-entered. User input is excluded; only paint, timer and
        // system messages are pumped so the splash repaints and Windows never marks the app
        // "Not Responding". The alternative -- constructing widgets now -- blocks the GUI
        // thread on the font database lock the warmup thread holds.
        QEventLoop loop;
        QTimer heartbeat;
        QElapsedTimer gap;
        heartbeat.setInterval(kFontWarmupHeartbeatMs);
        QObject::connect(&heartbeat, &QTimer::timeout, &loop, [&loop, &gap, &longest_gap_ms]() {
            longest_gap_ms = std::max(longest_gap_ms, gap.restart());
            if (g_font_warmup.done.load(std::memory_order_acquire)) {
                loop.quit();
            }
        });
        gap.start();
        heartbeat.start();
        loop.exec(QEventLoop::ExcludeUserInputEvents);
    }
    sak::logDebug(
        "Font database warmup: {} families in {} ms; GUI waited {} ms, longest event-loop gap {} "
        "ms",
        static_cast<qint64>(g_font_warmup.family_count.load(std::memory_order_relaxed)),
        g_font_warmup.census_ms.load(std::memory_order_relaxed),
        waited.elapsed(),
        longest_gap_ms);
}

/// @brief Configure the caller-owned Qt application and apply theming.
void initializeApp(QApplication& app, const RuntimeOptions& options) {
    startFontDatabaseWarmup();
    qAddPostRoutine(joinFontDatabaseWarmup);
    app.setApplicationName(sak::get_product_name());
    app.setApplicationVersion(sak::get_version());
    app.setOrganizationName(SAK_ORGANIZATION_NAME);
    app.setOrganizationDomain(SAK_ORGANIZATION_DOMAIN);
    app.setProperty("sakAccessibilityAudit", options.accessibility_audit);
    app.setProperty("sakAccessibilityAuditOutput", options.accessibility_audit_output);
    app.setProperty("sakStartupSmokeTest", options.startup_smoke_test);
    app.setProperty("sakStartupSmokeHeadless", options.startup_smoke_test && ciStartupSmokeMode());

    const QString icon_path = findIconPath();
    if (!icon_path.isEmpty()) {
        app.setWindowIcon(QIcon(icon_path));
    }

    sak::ui::installWheelPolicy(app);
}

/// @brief Initialize the logger subsystem.
/// @return true on success, false on failure (with user-visible error shown).
bool initializeLogger() {
    const QString log_path = sak::app_paths::logsDirectory();
    auto log_dir = std::filesystem::path(log_path.toStdWString());
    auto& logger = sak::logger::instance();

    if (auto result = logger.initialize(log_dir); !result) {
        sak::showCriticalLogged(
            nullptr,
            "Initialization Error",
            QString("Failed to initialize logger: %1")
                .arg(QString::fromStdString(std::string(sak::to_string(result.error())))));
        return false;
    }

    return true;
}

/// Tells the user why this launch cannot start -- on stderr always, and in a dialog unless the
/// run is non-interactive (startup smoke, accessibility audit).
void reportStartupRefusal(const RuntimeOptions& options, const QString& title, const QString& why) {
    std::cerr << "Fatal: " << why.toStdString() << '\n';
    if (!options.startup_smoke_test && !options.accessibility_audit) {
        sak::showCriticalLogged(nullptr, title, why);
    }
}

/// Decide where data lives (once, no fallback) and tidy this app's own temp leftovers.
/// Returns false -- after telling the user why -- when there is no usable data folder.
bool configurePortableRuntimeDirs(const RuntimeOptions& options) {
    const auto data_root = sak::app_paths::initializeDataRoot();
    if (!data_root) {
        reportStartupRefusal(options,
                             QStringLiteral("S.A.K. Utility: data folder"),
                             data_root.error());
        return false;
    }
    // TMP/TEMP are no longer redirected for the whole process: children that need a portable
    // temp (the bundled Chocolatey) get data/temp/session-<pid> through app_paths::withSessionTemp,
    // which is removed at exit; folders a crashed run left behind are removed here.
    sak::app_paths::removeStaleSessionTempDirectories();
    QObject::connect(qApp, &QCoreApplication::aboutToQuit, []() {
        sak::app_paths::removeSessionTempDirectory();
    });
    return true;
}

/// One S.A.K. Utility per data folder (sak::SingleInstanceGuard). The claim this process keeps
/// for its lifetime, or the exit code when this launch ends here: 0 once the running instance
/// was asked to come forward, 1 (reported) when the instance could not be established. An
/// elevated relaunch waits for the instance that launched it to exit instead of asking it.
std::expected<std::unique_ptr<sak::SingleInstanceGuard>, int> claimSingleInstance(
    const RuntimeOptions& options) {
    const QString title = QStringLiteral("S.A.K. Utility");
    const auto wait = options.elevated_relaunch
                          ? std::chrono::milliseconds(sak::kElevatedRelaunchHandoffLimit)
                          : std::chrono::milliseconds::zero();
    auto claim = sak::SingleInstanceGuard::claim(sak::app_paths::dataRoot(), wait);
    if (!claim) {
        reportStartupRefusal(options, title, claim.error());
        return std::unexpected(1);
    }
    if ((*claim)->role() == sak::SingleInstanceGuard::Role::Primary) {
        return std::move(*claim);
    }
    const auto asked = (*claim)->activateRunningInstance();
    if (!asked) {
        reportStartupRefusal(options, title, asked.error());
        return std::unexpected(1);
    }
    std::cerr << "S.A.K. Utility is already running for this data folder; "
                 "brought it to the front.\n";
    return std::unexpected(0);
}

/// @brief Log which build is starting: one line, so a support log still names the version.
void logStartupBanner() {
    sak::logInfo("SAK Utility {} starting (Qt {}, C++{}, Windows)",
                 sak::get_version(),
                 QT_VERSION_STR,
                 __cplusplus);
}

/// @brief Show splash screen and launch the main window.
std::unique_ptr<sak::ui::SplashScreen> createSplashIfRequested(bool show_splash) {
    if (!show_splash) {
        return nullptr;
    }
    const QString splash_path = findSplashPath();
    if (splash_path.isEmpty()) {
        return nullptr;
    }
    const QPixmap splash_pixmap(splash_path);
    if (splash_pixmap.isNull()) {
        return nullptr;
    }
    auto splash = std::make_unique<sak::ui::SplashScreen>(splash_pixmap);
    splash->showCentered();
    return splash;
}

void scheduleStartupSmokeExitIfRequested(bool startup_smoke_test, QApplication& app) {
    if (!startup_smoke_test) {
        return;
    }
    sak::logInfo("Startup smoke test mode active; closing automatically");
    QTimer::singleShot(sak::kTimerProgressPollMs, &app, &QCoreApplication::quit);
}

RuntimeOptions runtimeOptionsFromArgs(int argc, char* argv[]) {
    RuntimeOptions options;
    options.accessibility_audit = hasRawArgument(argc, argv, "--accessibility-audit");
    options.accessibility_audit_output =
        rawArgumentValue(argc, argv, "--accessibility-audit-output=");
    options.startup_smoke_test = hasRawArgument(argc, argv, "--smoke-test") ||
                                 hasRawArgument(argc, argv, "--startup-smoke-test");
    options.no_splash = hasRawArgument(argc, argv, "--no-splash");
    options.elevated_relaunch =
        hasRawArgument(argc, argv, sak::ElevationManager::kElevatedRelaunchArgument);
    return options;
}

void configureLoggingForMode(const RuntimeOptions& options) {
    if (options.startup_smoke_test || options.accessibility_audit) {
        sak::logger::instance().setConsoleOutput(false);
    }
    if (options.accessibility_audit) {
        sak::logger::instance().setLevel(sak::log_level::warning);
    }
}

bool shouldShowSplash(const RuntimeOptions& options) {
    return !options.no_splash && !options.startup_smoke_test && !options.accessibility_audit;
}

int runMainEventLoop(QApplication& app, bool startup_smoke_test) {
    int result = app.exec();

    if (startup_smoke_test && result == 0 && !app.property("sakStartupAllPanelsBuilt").toBool()) {
        sak::logError("Startup smoke exited before all tool panels were constructed");
        result = 1;
    }

    sak::logInfo("Application shutting down with exit code: {}", result);
    sak::logger::instance().flush();

    if (startup_smoke_test && result == 0) {
        std::println("SAK_STARTUP_SMOKE_OK");
    }

    return result;
}

int runAccessibilityAuditHeadless(QApplication& app, sak::MainWindow& main_window) {
    const bool previous_quit_on_last_window_closed = app.quitOnLastWindowClosed();
    app.setQuitOnLastWindowClosed(false);
    writeAccessibilityAuditStatus(QStringLiteral("starting-event-loop"));
    QTimer::singleShot(sak::kTimerImmediateMs, &app, [&app, &main_window]() {
        writeAccessibilityAuditStatus(QStringLiteral("running-widget-scan"));
        const int audit_result = runAccessibilityAudit(main_window);
        sak::logInfo("Accessibility audit completed with exit code: {}", audit_result);
        sak::logger::instance().flush();
        app.exit(audit_result);
    });

    const int result = app.exec();
    app.setQuitOnLastWindowClosed(previous_quit_on_last_window_closed);
    sak::logInfo("Accessibility audit shutting down with exit code: {}", result);
    sak::logger::instance().flush();
    return result;
}

int showMainWindow(QApplication& app,
                   sak::SingleInstanceGuard& instance,
                   bool startup_smoke_test,
                   bool show_splash,
                   bool accessibility_audit) {
    if (accessibility_audit) {
        writeAccessibilityAuditStatus(QStringLiteral("show-main-window-entry"));
    }
    auto splash = createSplashIfRequested(show_splash);
    if (accessibility_audit) {
        writeAccessibilityAuditStatus(QStringLiteral("splash-ready"));
    }
    // SAK-ALLOW-BLOCKING: responsive wait (it pumps events); see waitForFontDatabaseWarmup.
    waitForFontDatabaseWarmup();
    if (startup_smoke_test) {
        sak::logger::instance().flush();
    }

    sak::logDebug("Creating main window...");
    if (startup_smoke_test) {
        sak::logger::instance().flush();
    }
    if (accessibility_audit) {
        writeAccessibilityAuditStatus(QStringLiteral("constructing-main-window"));
    }
    QElapsedTimer window_timer;
    window_timer.start();
    sak::MainWindow main_window;
    sak::logDebug("Main window constructed in {} ms", window_timer.restart());
    // A second launch on this data folder now brings this window forward.
    instance.listenForActivation();
    QObject::connect(&instance,
                     &sak::SingleInstanceGuard::activationRequested,
                     &main_window,
                     &sak::MainWindow::bringToFront);
    if (startup_smoke_test) {
        sak::logger::instance().flush();
    }
    if (accessibility_audit) {
        writeAccessibilityAuditStatus(QStringLiteral("main-window-constructed"));
    }
    const bool headless_smoke_test = app.property("sakStartupSmokeHeadless").toBool();
    prepareMainWindowForStartup(main_window, headless_smoke_test);
    sak::logDebug("Main window show took {} ms", window_timer.elapsed());

    if (accessibility_audit) {
        return runAccessibilityAuditHeadless(app, main_window);
    }

    scheduleStartupSmokeExitIfRequested(startup_smoke_test, app);

    if (splash) {
        // Close the splash from the event loop rather than here: the current
        // tab's panel is materialized by a single-shot timer queued during
        // show(), so this later-queued single-shot runs after it and the
        // splash stays up over the first panel's construction instead of
        // leaving a frozen, still-empty main window on screen.
        auto* splash_ptr = splash.release();
        QTimer::singleShot(0, &app, [splash_ptr]() {
            splash_ptr->finish();
            delete splash_ptr;
        });
    }

    logMainWindowReady(headless_smoke_test);
    return runMainEventLoop(app, startup_smoke_test);
}

int runApplication(int argc, char* argv[]) {
    const RuntimeOptions options = runtimeOptionsFromArgs(argc, argv);
    writeStartupSmokePhase(options.startup_smoke_test, "before-qt-app");
    if (options.accessibility_audit) {
        writeAccessibilityAuditStatusTo(options.accessibility_audit_output,
                                        QStringLiteral("parsed-arguments"));
    }

    // Qt retains argc by reference. This scope owns both the argument count and
    // the application, so argc remains alive until application teardown finishes.
    QApplication app(argc, argv);
    writeStartupSmokePhase(options.startup_smoke_test, "qt-app-ready");
    initializeApp(app, options);
    writeStartupSmokePhase(options.startup_smoke_test, "app-initialized");
    if (options.accessibility_audit) {
        writeAccessibilityAuditStatus(QStringLiteral("app-initialized"));
    }

    if (!configurePortableRuntimeDirs(options)) {
        return 1;
    }
    if (options.accessibility_audit) {
        writeAccessibilityAuditStatus(QStringLiteral("runtime-dirs-configured"));
    }
    auto instance = claimSingleInstance(options);
    if (!instance) {
        return instance.error();
    }

    configureLoggingForMode(options);
    if (!initializeLogger()) {
        return 1;
    }
    // Install the crash reporter right after the logger, so an unhandled SEH fault at any point
    // in the main run writes a minidump + summary instead of the process vanishing silently.
    if (!sak::CrashReporter::install(
            std::filesystem::path(sak::app_paths::crashesDirectory().toStdWString()))) {
        sak::logWarning("Crash reporter not installed (crash directory unavailable)");
    }
    if (options.accessibility_audit) {
        writeAccessibilityAuditStatus(QStringLiteral("logger-initialized"));
        writeAccessibilityAuditStatus(QStringLiteral("startup-banner-skipped"));
    } else {
        logStartupBanner();
    }
    if (options.startup_smoke_test) {
        sak::logger::instance().flush();
    }
    // The saved theme (Light / Dark / System, from the portable config) once the data folder is
    // decided; System keeps following Windows. Surfaces are flat with 1px borders.
    sak::ui::initializeAppTheme(app);
    return showMainWindow(app,
                          **instance,
                          options.startup_smoke_test,
                          shouldShowSplash(options),
                          options.accessibility_audit);
}

struct FatalErrorOutput {
    decltype(&std::fprintf) write{&std::fprintf};
    decltype(&std::fflush) flush{&std::fflush};
    decltype(&::MessageBoxW) dialog{&::MessageBoxW};
    decltype(&::GetLastError) last_error{&::GetLastError};
};

void writeFatalStderr(const char* text, const FatalErrorOutput& output) noexcept {
    const int written = output.write(stderr, "%s\n", text);
    const int write_error = errno;
    const int flushed = output.flush(stderr);
    const int flush_error = errno;
    if (written < 0 || flushed != 0) {
        sak::logCritical("Fatal stderr output failed (write errno {}, flush errno {})",
                         written < 0 ? write_error : 0,
                         flushed != 0 ? flush_error : 0);
    }
}

void reportFatalErrorTo(const char* detail,
                        bool interactive,
                        const FatalErrorOutput& output) noexcept {
    // QApplication has already unwound. Only C/Win32 output and the Qt-independent
    // noexcept logger are valid here. Critical logging checks its own write/flush;
    // the logger's separate void flush() cannot establish successful persistence.
    writeFatalStderr("Fatal error:", output);
    writeFatalStderr(detail, output);
    // Publish primary diagnostics before the logger's first-use construction.
    // If stderr, logger initialization, or native UI cannot operate, reporting
    // cannot promise a visible/durable artifact; the caller still fails closed.
    sak::logCritical("Fatal error");
    sak::logCritical(detail);
    if (!interactive) {
        return;
    }
    constexpr wchar_t kTitle[] = L"S.A.K. Utility -- Fatal Error";
    constexpr wchar_t kNotice[] =
        L"S.A.K. Utility encountered a fatal error and cannot continue.\r\n\r\n"
        L"Include any available application log or terminal output when reporting this issue.";
    if (output.dialog(nullptr, kNotice, kTitle, MB_OK | MB_ICONERROR | MB_TASKMODAL) != 0) {
        return;
    }
    const DWORD native_error = output.last_error();
    sak::logCritical("Native fatal error notice failed (Windows error {})", native_error);
    constexpr std::size_t kDiagnosticBytes = 96;
    std::array<char, kDiagnosticBytes> diagnostic{};
    const int length = std::snprintf(diagnostic.data(),
                                     diagnostic.size(),
                                     "Native fatal error notice failed (Windows error %lu)",
                                     static_cast<unsigned long>(native_error));
    if (length < 0 || static_cast<std::size_t>(length) >= diagnostic.size()) {
        writeFatalStderr("Native fatal notice failed; its error-code formatting also failed",
                         output);
        return;
    }
    writeFatalStderr(diagnostic.data(), output);
}

void reportFatalError(const char* detail, bool interactive) noexcept {
    reportFatalErrorTo(detail, interactive, FatalErrorOutput{});
}

using ApplicationRunner = int (*)(int, char*[]);
using FatalReporter = void (*)(const char*, bool) noexcept;

int runApplicationGuarded(int argc,
                          char* argv[],
                          ApplicationRunner runner = runApplication,
                          FatalReporter reporter = reportFatalError) noexcept {
    // Preserve noninteractive diagnostic modes even after Qt rewrites argv or
    // its application has been destroyed. This ASCII scan performs no allocation.
    const bool interactive = !hasRawArgument(argc, argv, "--smoke-test") &&
                             !hasRawArgument(argc, argv, "--startup-smoke-test") &&
                             !hasRawArgument(argc, argv, "--accessibility-audit");
    try {
        return runner(argc, argv);
    } catch (const std::exception& error) {
        reporter(error.what(), interactive);
        return 1;
    } catch (...) {
        // Non-standard exceptions still need a Qt-free diagnostic after teardown.
        // Debug retains termination for debugger visibility; Release refuses startup.
        reporter("Unknown unhandled exception", interactive);
#ifndef NDEBUG
        std::terminate();
#else
        return 1;
#endif
    }
}

}  // namespace

/// @brief Main application entry point
/// @param argc Argument count
/// @param argv Argument vector
/// @return Exit code
int main(int argc, char* argv[]) {
    // The win32 MCP server and the Chrome browser-control host are folded into this one
    // binary. When launched in either helper mode (env var set by the provider gateway, or
    // a chrome-extension:// origin passed by Chrome), run headless on stdio and return
    // before any Qt/GUI initialization -- these processes must never open a window.
    if (sak::win32mcp::isWin32McpHelperInvocation(argc, argv)) {
        return sak::win32mcp::runWin32McpProcess(argc, argv);
    }

    return runApplicationGuarded(argc, argv);
}
