// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file main.cpp
/// @brief SAK Utility main entry point

#include "sak/app_paths.h"
#include "sak/error_codes.h"
#include "sak/layout_constants.h"
#include "sak/logger.h"
#include "sak/main_window.h"
#include "sak/message_box_helpers.h"
#include "sak/splash_screen.h"
#include "sak/version.h"
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
#include <QFile>
#include <QFileInfo>
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

    auto it = std::find_if(candidates.begin(), candidates.end(), [](const QString& p) {
        return QFileInfo::exists(p);
    });
    return it != candidates.end() ? *it : QString{};
}

QString findIconPath() {
    const QString app_dir = QCoreApplication::applicationDirPath();
    const QStringList candidates = {app_dir + "/icon.ico",
                                    app_dir + "/resources/icon.ico",
                                    app_dir + "/../resources/icon.ico"};

    auto it = std::find_if(candidates.begin(), candidates.end(), [](const QString& p) {
        return QFileInfo::exists(p);
    });
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
    if (!widget) {
        return true;
    }
    if (qobject_cast<const QScrollBar*>(widget) || qobject_cast<const QHeaderView*>(widget)) {
        return true;
    }
    if (qobject_cast<const QLineEdit*>(widget) &&
        qobject_cast<const QComboBox*>(widget->parentWidget())) {
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
    if (qobject_cast<const QAbstractButton*>(widget)) {
        return true;
    }
    if (qobject_cast<const QLineEdit*>(widget) || qobject_cast<const QComboBox*>(widget)) {
        return true;
    }
    if (qobject_cast<const QAbstractSpinBox*>(widget) ||
        qobject_cast<const QAbstractSlider*>(widget)) {
        return true;
    }
    if (qobject_cast<const QTextEdit*>(widget) || qobject_cast<const QPlainTextEdit*>(widget)) {
        return true;
    }
    return qobject_cast<const QAbstractItemView*>(widget) ||
           qobject_cast<const QTabWidget*>(widget);
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
        button && !button->text().trimmed().isEmpty()) {
        return QStringLiteral(" text=\"%1\"").arg(button->text());
    }
    if (const auto* line_edit = qobject_cast<const QLineEdit*>(widget);
        line_edit && !line_edit->placeholderText().trimmed().isEmpty()) {
        return QStringLiteral(" placeholder=\"%1\"").arg(line_edit->placeholderText());
    }
    if (const auto* text_edit = qobject_cast<const QTextEdit*>(widget);
        text_edit && !text_edit->placeholderText().trimmed().isEmpty()) {
        return QStringLiteral(" placeholder=\"%1\"").arg(text_edit->placeholderText());
    }
    return {};
}

QString parentDebugLabel(const QWidget* widget) {
    QStringList parents;
    const QObject* parent = widget->parent();
    while (parent && parents.size() < kAccessibilityParentDepthLimit) {
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

/// @brief Initialize the Qt application and apply theming.
QApplication& initializeApp(int argc,
                            char* argv[],
                            bool accessibility_audit,
                            const QString& accessibility_audit_output) {
    static QApplication app(argc, argv);
    app.setApplicationName(sak::get_product_name());
    app.setApplicationVersion(sak::get_version());
    app.setOrganizationName(SAK_ORGANIZATION_NAME);
    app.setOrganizationDomain(SAK_ORGANIZATION_DOMAIN);
    app.setProperty("sakAccessibilityAudit", accessibility_audit);
    app.setProperty("sakAccessibilityAuditOutput", accessibility_audit_output);

    const QString icon_path = findIconPath();
    if (!icon_path.isEmpty()) {
        app.setWindowIcon(QIcon(icon_path));
    }

    sak::ui::applyWindows11Theme(app);
    if (!accessibility_audit) {
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
    QPixmap splash_pixmap(splash_path);
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
            std::fflush(stdout);
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
    sak::MainWindow main_window;
    if (accessibility_audit) {
        writeAccessibilityAuditStatus(QStringLiteral("main-window-constructed"));
    }
    const bool headless_smoke_test = startup_smoke_test && ciStartupSmokeMode();
    prepareMainWindowForStartup(main_window, headless_smoke_test);

    if (accessibility_audit) {
        return runAccessibilityAuditHeadless(app, main_window);
    }

    scheduleStartupSmokeExitIfRequested(startup_smoke_test, app);

    if (splash) {
        splash->finish();
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

    QApplication& app =
        initializeApp(argc, argv, options.accessibility_audit, options.accessibility_audit_output);
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
    if (options.accessibility_audit) {
        writeAccessibilityAuditStatus(QStringLiteral("logger-initialized"));
    }

    if (options.accessibility_audit) {
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
