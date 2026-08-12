// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file main.cpp
/// @brief SAK Utility main entry point

#include "sak/app_paths.h"
#include "sak/crash_reporter.h"
#include "sak/error_codes.h"
#include "sak/layout_constants.h"
#include "sak/logger.h"
#include "sak/main_window.h"
#include "sak/message_box_helpers.h"
#include "sak/splash_screen.h"
#include "sak/version.h"
#include "sak/win32mcp/win32_mcp_entry.h"
#include "sak/windows11_theme.h"

#include <QAbstractButton>
#include <QAbstractItemView>
#include <QAbstractSlider>
#include <QAbstractSpinBox>
#include <QApplication>
#include <QByteArray>
#include <QComboBox>
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QFont>
#include <QFontDatabase>
#include <QFontMetrics>
#include <QHeaderView>
#include <QIcon>
#include <QLineEdit>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QScrollBar>
#include <QStringList>
#include <QTabWidget>
#include <QTextEdit>
#include <QTextStream>
#include <QTimer>
#include <QWidget>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>
#include <print>
#include <thread>

namespace {

constexpr int kAccessibilityParentDepthLimit = 5;
constexpr int kAccessibilityOutputWriteFailureExitCode = 3;
constexpr int kAccessibilityMissingNamesExitCode = 2;

struct RuntimeOptions {
    bool accessibility_audit{false};
    QString accessibility_audit_output;
    bool startup_smoke_test{false};
    bool no_splash{false};
};

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

bool hasRawArgument(int argc, char* argv[], const char* name) {
    for (int i = 1; i < argc; ++i) {
        if (QString::fromLocal8Bit(argv[i]) == QString::fromLatin1(name)) {
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

void logMainWindowReady(bool headless_smoke_test) {
    if (headless_smoke_test) {
        sak::logInfo("Main window initialized - application ready");
        return;
    }

    sak::logInfo("Main window displayed - application ready");
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

bool isKnownQtInternalWidget(const QWidget* widget) {
    if (widget == nullptr) {
        return true;
    }
    if ((qobject_cast<const QScrollBar*>(widget) != nullptr) ||
        (qobject_cast<const QHeaderView*>(widget) != nullptr)) {
        return true;
    }
    if ((qobject_cast<const QLineEdit*>(widget) != nullptr) &&
        (qobject_cast<const QComboBox*>(widget->parentWidget()) != nullptr)) {
        return true;
    }
    const QString class_name = QString::fromLatin1(widget->metaObject()->className());
    const QString object_name = widget->objectName();
    static const QStringList kInternalClasses = {QStringLiteral("QTableCornerButton"),
                                                 QStringLiteral("QComboBoxListView"),
                                                 QStringLiteral("QLineEditIconButton"),
                                                 QStringLiteral("CheckHeaderView")};
    static const QStringList kInternalObjects = {QStringLiteral("qt_spinbox_lineedit"),
                                                 QStringLiteral("qt_menubar_ext_button")};
    return kInternalClasses.contains(class_name) || kInternalObjects.contains(object_name);
}

bool isAuditedInteractiveWidget(const QWidget* widget) {
    if (qobject_cast<const QAbstractButton*>(widget) != nullptr) {
        return true;
    }
    if ((qobject_cast<const QLineEdit*>(widget) != nullptr) ||
        (qobject_cast<const QComboBox*>(widget) != nullptr)) {
        return true;
    }
    if ((qobject_cast<const QAbstractSpinBox*>(widget) != nullptr) ||
        (qobject_cast<const QAbstractSlider*>(widget) != nullptr)) {
        return true;
    }
    if ((qobject_cast<const QTextEdit*>(widget) != nullptr) ||
        (qobject_cast<const QPlainTextEdit*>(widget) != nullptr)) {
        return true;
    }
    return (qobject_cast<const QAbstractItemView*>(widget) != nullptr) ||
           (qobject_cast<const QTabWidget*>(widget) != nullptr);
}

bool requiresExplicitAccessibleName(const QWidget* widget) {
    return !isKnownQtInternalWidget(widget) && isAuditedInteractiveWidget(widget);
}

QString objectDebugLabel(const QObject* object) {
    QString label = QString::fromLatin1(object->metaObject()->className());
    if (!object->objectName().isEmpty()) {
        label += QStringLiteral("#%1").arg(object->objectName());
    }
    return label;
}

QString textDebugLabel(const QWidget* widget) {
    if (const auto* button = qobject_cast<const QAbstractButton*>(widget);
        (button != nullptr) && !button->text().trimmed().isEmpty()) {
        return QStringLiteral(" text=\"%1\"").arg(button->text());
    }
    if (const auto* line_edit = qobject_cast<const QLineEdit*>(widget);
        (line_edit != nullptr) && !line_edit->placeholderText().trimmed().isEmpty()) {
        return QStringLiteral(" placeholder=\"%1\"").arg(line_edit->placeholderText());
    }
    if (const auto* text_edit = qobject_cast<const QTextEdit*>(widget);
        (text_edit != nullptr) && !text_edit->placeholderText().trimmed().isEmpty()) {
        return QStringLiteral(" placeholder=\"%1\"").arg(text_edit->placeholderText());
    }
    return {};
}

QString parentDebugLabel(const QWidget* widget) {
    QStringList parents;
    const QObject* parent = widget->parent();
    while ((parent != nullptr) && parents.size() < kAccessibilityParentDepthLimit) {
        parents << objectDebugLabel(parent);
        parent = parent->parent();
    }
    return parents.isEmpty()
               ? QString{}
               : QStringLiteral(" parent=\"%1\"").arg(parents.join(QStringLiteral(" > ")));
}

QString accessibilityAuditLabel(const QWidget* widget) {
    return objectDebugLabel(widget) + textDebugLabel(widget) + parentDebugLabel(widget);
}

int runAccessibilityAudit(sak::MainWindow& main_window) {
    QStringList missing;
    auto widgets = main_window.findChildren<QWidget*>();
    widgets.prepend(&main_window);
    for (const QWidget* widget : widgets) {
        if (!requiresExplicitAccessibleName(widget)) {
            continue;
        }
        if (widget->accessibleName().trimmed().isEmpty()) {
            missing << accessibilityAuditLabel(widget);
        }
    }

    const QString output_path = accessibilityAuditOutputPath();
    bool output_written = output_path.isEmpty();
    if (!output_path.isEmpty()) {
        QFile file(output_path);
        if (file.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Truncate)) {
            QTextStream out(&file);
            out << (missing.isEmpty() ? "SAK_ACCESSIBILITY_AUDIT_OK"
                                      : "SAK_ACCESSIBILITY_AUDIT_FAILED")
                << " missing=" << missing.size() << " checked=" << widgets.size() << "\n";
            for (const auto& entry : missing) {
                out << entry << "\n";
            }
            output_written = true;
        }
    }

    if (!output_written) {
        return kAccessibilityOutputWriteFailureExitCode;
    }
    if (!missing.isEmpty()) {
        return kAccessibilityMissingNamesExitCode;
    }

    return 0;
}

/// @brief Populate the font database off the GUI thread.
///
/// The first widget polish otherwise pays the full enumeration on the GUI
/// thread - over a second with thousands of installed font families, and tens
/// of seconds when a freshly built binary's font-file reads are additionally
/// being scanned by real-time antivirus. QFontDatabase is thread-safe; if the
/// GUI thread needs fonts before the warmup finishes it simply blocks on the
/// shared mutex for the remaining time instead of the full cost.
// The warmup thread is owned, never detached: it calls into the Qt font
// database, and a detached copy still running during QApplication teardown would
// touch those internals as they are destroyed (observed as a heap-corruption
// crash at process exit). joinFontDatabaseWarmup() is registered with
// qAddPostRoutine so it is joined at the very start of ~QCoreApplication, before
// any font-database teardown, and normally the thread has long since finished.
std::thread g_font_warmup_thread;

void joinFontDatabaseWarmup() {
    if (g_font_warmup_thread.joinable()) {
        g_font_warmup_thread.join();
    }
}

void startFontDatabaseWarmup() {
    g_font_warmup_thread = std::thread([]() {
        QFontDatabase::families();
        // Force a full font MATCH for a family that is not installed: the
        // first such miss builds the alias table by walking every installed
        // family (a second multi-second pass with large font collections),
        // and style-sheet font stacks trigger exactly that miss. Fonts are
        // thread-safe in Qt 6, so pay it here instead of on the GUI thread.
        QFontMetrics(QFont(QStringLiteral("sak-alias-warmup-probe"))).height();
    });
}

#if defined(_WIN32)
/// @brief Force Qt onto the GDI font database instead of the DirectWrite backend.
///
/// Qt 6 defaults to the DirectWrite font database on Windows. Its enumeration
/// re-reads every installed font file on each launch unless the DirectWrite font
/// cache service (FontCache3.0.0.0) is running; where that service is stopped,
/// real-time antivirus scans each of those thousands of reads and the first font
/// query blocks the GUI thread for tens of seconds (measured 31 s here with 2491
/// families and the service stopped). The GDI font database instead uses the
/// always-on FontCache service, populates the identical family set in about a
/// second, and does not depend on any per-machine service state. Must run before
/// QApplication is constructed so the platform plugin reads it during init.
void forceGdiFontDatabase() {
    // Escape hatch: a machine whose DirectWrite font cache service is healthy can
    // set SAK_USE_DIRECTWRITE=1 to keep Qt's default DirectWrite backend and its
    // rendering. Left off, the GDI database is forced (the safe default above).
    if (qEnvironmentVariableIntValue("SAK_USE_DIRECTWRITE") == 1) {
        return;
    }
    qputenv("QT_NO_DIRECTWRITE", "1");
}
#endif

/// @brief Initialize the Qt application and apply theming.
QApplication& initializeApp(int argc, char* argv[], const RuntimeOptions& options) {
#if defined(_WIN32)
    forceGdiFontDatabase();
#endif
    static QApplication app(argc, argv);
    startFontDatabaseWarmup();
    qAddPostRoutine(joinFontDatabaseWarmup);
    app.setApplicationName(sak::get_product_name());
    app.setApplicationVersion(sak::get_version());
    app.setOrganizationName(SAK_ORGANIZATION_NAME);
    app.setOrganizationDomain(SAK_ORGANIZATION_DOMAIN);
    app.setProperty("sakAccessibilityAudit", options.accessibility_audit);
    app.setProperty("sakAccessibilityAuditOutput", options.accessibility_audit_output);
    app.setProperty("sakStartupSmokeTest", options.startup_smoke_test);

    const QString icon_path = findIconPath();
    if (!icon_path.isEmpty()) {
        app.setWindowIcon(QIcon(icon_path));
    }

    sak::ui::applyWindows11Theme(app);
    if (!options.accessibility_audit) {
        sak::ui::installThemePolishHelper(app);
    }

    return app;
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

void configurePortableRuntimeDirs() {
    const QString temp_dir = sak::app_paths::tempDirectory();
    if (!sak::app_paths::ensureDirectory(temp_dir)) {
        std::cerr << "Warning: failed to create portable temp directory: " << temp_dir.toStdString()
                  << '\n';
    }
    const QByteArray native_temp = QDir::toNativeSeparators(temp_dir).toLocal8Bit();
    qputenv("TMP", native_temp);
    qputenv("TEMP", native_temp);
}

/// @brief Log startup banner with version and platform info.
void logStartupBanner() {
    sak::logInfo("===========================================");
    sak::logInfo("SAK Utility Starting");
    sak::logInfo("Version: {}", sak::get_version());
    sak::logInfo("C++ Standard: C++{}", __cplusplus);
#ifdef SAK_PLATFORM_WINDOWS
    sak::logInfo("Platform: Windows");
#elif defined(SAK_PLATFORM_MACOS)
    sak::logInfo("Platform: macOS");
#elif defined(SAK_PLATFORM_LINUX)
    sak::logInfo("Platform: Linux");
#endif
    sak::logInfo("Qt Version: {}", QT_VERSION_STR);
    sak::logInfo("===========================================");
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

int runMainEventLoop(QApplication& app, bool startup_smoke_test, bool headless_smoke_test) {
    const int result = app.exec();

    sak::logInfo("Application shutting down with exit code: {}", result);
    sak::logger::instance().flush();

    if (startup_smoke_test && result == 0) {
        std::println("SAK_STARTUP_SMOKE_OK");
        if (headless_smoke_test) {
            (void)std::fflush(stdout);
            std::_Exit(0);
        }
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
    if (result == 0) {
        // Audit status is already written; avoid Qt/shared-folder teardown crashes in automation.
        (void)std::fflush(stdout);
        std::_Exit(0);
    }
    return result;
}

int showMainWindow(QApplication& app,
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

    sak::logInfo("Creating main window...");
    if (accessibility_audit) {
        writeAccessibilityAuditStatus(QStringLiteral("constructing-main-window"));
    }
    QElapsedTimer window_timer;
    window_timer.start();
    sak::MainWindow main_window;
    sak::logInfo("Main window constructed in {} ms", window_timer.restart());
    if (accessibility_audit) {
        writeAccessibilityAuditStatus(QStringLiteral("main-window-constructed"));
    }
    const bool headless_smoke_test = startup_smoke_test && ciStartupSmokeMode();
    prepareMainWindowForStartup(main_window, headless_smoke_test);
    sak::logInfo("Main window show took {} ms", window_timer.elapsed());

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
    return runMainEventLoop(app, startup_smoke_test, headless_smoke_test);
}

int runApplication(int argc, char* argv[]) {
    const RuntimeOptions options = runtimeOptionsFromArgs(argc, argv);
    if (options.accessibility_audit) {
        writeAccessibilityAuditStatusTo(options.accessibility_audit_output,
                                        QStringLiteral("parsed-arguments"));
    }

    QApplication& app = initializeApp(argc, argv, options);
    if (options.accessibility_audit) {
        writeAccessibilityAuditStatus(QStringLiteral("app-initialized"));
    }

    configurePortableRuntimeDirs();
    if (options.accessibility_audit) {
        writeAccessibilityAuditStatus(QStringLiteral("runtime-dirs-configured"));
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
    return showMainWindow(
        app, options.startup_smoke_test, shouldShowSplash(options), options.accessibility_audit);
}

}  // namespace

/// @brief Main application entry point
/// @param argc Argument count
/// @param argv Argument vector
/// @return Exit code
int main(int argc, char* argv[]) {
#if defined(_WIN32)
    // The win32 MCP server and the Chrome browser-control host are folded into this one
    // binary. When launched in either helper mode (env var set by the provider gateway, or
    // a chrome-extension:// origin passed by Chrome), run headless on stdio and return
    // before any Qt/GUI initialization -- these processes must never open a window.
    if (sak::win32mcp::isWin32McpHelperInvocation(argc, argv)) {
        return sak::win32mcp::runWin32McpProcess(argc, argv);
    }
#endif

    try {
        return runApplication(argc, argv);

    } catch (const std::exception& e) {
        sak::logError("Fatal error: {}", e.what());
        std::println(std::cerr, "Fatal error: {}", e.what());
        sak::showCriticalLogged(nullptr,
                                "Fatal Error",
                                QString("Unhandled exception: %1").arg(e.what()));
        return 1;
    } catch (...) {  // Final safety net: re-throw in debug, exit in release
        sak::logError("Unknown fatal error");
        std::println(std::cerr, "Unknown fatal error");
        sak::showCriticalLogged(nullptr, "Fatal Error", "Unknown unhandled exception");
#ifndef NDEBUG
        // cppcheck-suppress throwInEntryPoint ; intentional re-throw in debug builds only
        throw;
#else
        return 1;
#endif
    }
}
