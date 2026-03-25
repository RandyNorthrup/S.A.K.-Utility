// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file main.cpp
/// @brief SAK Utility main entry point

#include "sak/actions/backup_bitlocker_keys_action.h"
#include "sak/actions/check_disk_errors_action.h"
#include "sak/actions/generate_system_report_action.h"
#include "sak/actions/optimize_power_settings_action.h"
#include "sak/actions/reset_network_action.h"
#include "sak/actions/screenshot_settings_action.h"
#include "sak/actions/verify_system_files_action.h"
#include "sak/error_codes.h"
#include "sak/logger.h"
#include "sak/main_window.h"
#include "sak/quick_action_controller.h"
#include "sak/quick_action_result_io.h"
#include "sak/splash_screen.h"
#include "sak/version.h"
#include "sak/windows11_theme.h"

#include <QApplication>
#include <QCoreApplication>
#include <QFileInfo>
#include <QIcon>
#include <QMessageBox>
#include <QStandardPaths>

#include <algorithm>
#include <filesystem>
#include <iostream>
#include <memory>
#include <print>

namespace {

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

/// @brief Write an execution result file if a result file path was provided.
void writeResultIfNeeded(const QString& result_file,
                         const sak::QuickAction::ExecutionResult& result,
                         sak::QuickAction::ActionStatus status) {
    if (result_file.isEmpty()) {
        return;
    }
    QString error_message;
    if (!sak::writeExecutionResultFile(result_file, result, status, &error_message)) {
        sak::logWarning("Failed to write result file: {}", error_message.toStdString());
    }
}

/// @brief Run a quick action in headless/elevated mode and return exit code.
int runElevatedQuickAction(QApplication& app,
                           const QString& action_to_run,
                           const QString& backup_location,
                           const QString& result_file) {
    sak::logInfo("Running elevated quick action: {}", action_to_run.toStdString());

    sak::QuickActionController controller;
    controller.setBackupLocation(backup_location);

    QObject::connect(&controller,
                     &sak::QuickActionController::logMessage,
                     [](const QString& message) { sak::logInfo("{}", message.toStdString()); });

    auto actions = std::vector<std::unique_ptr<sak::QuickAction>>{};
    actions.push_back(std::make_unique<sak::BackupBitlockerKeysAction>(backup_location));
    actions.push_back(std::make_unique<sak::CheckDiskErrorsAction>());
    actions.push_back(std::make_unique<sak::GenerateSystemReportAction>(backup_location));
    actions.push_back(std::make_unique<sak::OptimizePowerSettingsAction>());
    actions.push_back(std::make_unique<sak::ResetNetworkAction>());
    actions.push_back(std::make_unique<sak::ScreenshotSettingsAction>(backup_location));
    actions.push_back(std::make_unique<sak::VerifySystemFilesAction>());
    for (auto& action : actions) {
        controller.registerAction(std::move(action));
    }

    sak::QuickAction* action = controller.getAction(action_to_run);
    if (!action) {
        sak::QuickAction::ExecutionResult result;
        result.success = false;
        result.message = "Action not found";
        result.log = QString("No action registered with name: %1").arg(action_to_run);
        writeResultIfNeeded(result_file, result, sak::QuickAction::ActionStatus::Failed);
        return 1;
    }

    QObject::connect(
        &controller,
        &sak::QuickActionController::actionExecutionComplete,
        &app,
        [&app, action, result_file](sak::QuickAction* completed) {
            if (completed != action) {
                return;
            }
            writeResultIfNeeded(result_file, action->lastExecutionResult(), action->status());
            const int exit_code = action->lastExecutionResult().success ? 0 : 2;
            app.exit(exit_code);
        },
        Qt::QueuedConnection);

    controller.executeAction(action->name(), false);
    return app.exec();
}

/// @brief Initialize the Qt application and apply theming.
QApplication& initializeApp(int argc, char* argv[]) {
    static QApplication app(argc, argv);
    app.setApplicationName(sak::get_product_name());
    app.setApplicationVersion(sak::get_version());
    app.setOrganizationName(SAK_ORGANIZATION_NAME);
    app.setOrganizationDomain(SAK_ORGANIZATION_DOMAIN);

    const QString icon_path = findIconPath();
    if (!icon_path.isEmpty()) {
        app.setWindowIcon(QIcon(icon_path));
    }

    sak::ui::applyWindows11Theme(app);
    sak::ui::installTooltipHelper(app);

    return app;
}

/// @brief Initialize the logger subsystem.
/// @return true on success, false on failure (with user-visible error shown).
bool initializeLogger() {
    auto log_dir = std::filesystem::current_path() / "_logs";
    auto& logger = sak::logger::instance();

    if (auto result = logger.initialize(log_dir); !result) {
        QMessageBox::critical(
            nullptr,
            "Initialization Error",
            QString("Failed to initialize logger: %1")
                .arg(QString::fromStdString(std::string(sak::to_string(result.error())))));
        return false;
    }

    return true;
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

/// @brief Parsed command-line arguments.
struct CommandLineArgs {
    QString action_to_run;
    QString backup_location = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) +
                              QStringLiteral("/SAK_Backups");
    QString result_file;
};

/// @brief Parse command-line arguments.
CommandLineArgs parseCommandLine(int argc, char* argv[]) {
    CommandLineArgs args;
    for (int i = 1; i < argc; ++i) {
        const QString arg = QString::fromLocal8Bit(argv[i]);
        if (arg == "--run-quick-action" && i + 1 < argc) {
            args.action_to_run = QString::fromLocal8Bit(argv[++i]);
        } else if (arg == "--backup-location" && i + 1 < argc) {
            args.backup_location = QString::fromLocal8Bit(argv[++i]);
        } else if (arg == "--result-file" && i + 1 < argc) {
            args.result_file = QString::fromLocal8Bit(argv[++i]);
        }
    }
    return args;
}

/// @brief Show splash screen and launch the main window.
int showMainWindow(QApplication& app) {
    std::unique_ptr<sak::ui::SplashScreen> splash;
    const QString splash_path = findSplashPath();
    if (!splash_path.isEmpty()) {
        QPixmap splash_pixmap(splash_path);
        if (!splash_pixmap.isNull()) {
            splash = std::make_unique<sak::ui::SplashScreen>(splash_pixmap);
            splash->showCentered();
            app.processEvents();
        }
    }

    sak::logInfo("Creating main window...");
    sak::MainWindow main_window;
    main_window.show();

    if (splash) {
        splash->finish();
    }

    sak::logInfo("Main window displayed - application ready");

    int result = app.exec();

    sak::logInfo("Application shutting down with exit code: {}", result);
    sak::logger::instance().flush();

    return result;
}

}  // namespace

/// @brief Main application entry point
/// @param argc Argument count
/// @param argv Argument vector
/// @return Exit code
int main(int argc, char* argv[]) {
    try {
        QApplication& app = initializeApp(argc, argv);

        if (!initializeLogger()) {
            return 1;
        }

        logStartupBanner();

        CommandLineArgs cli = parseCommandLine(argc, argv);

        if (!cli.action_to_run.isEmpty()) {
            return runElevatedQuickAction(
                app, cli.action_to_run, cli.backup_location, cli.result_file);
        }

        return showMainWindow(app);

    } catch (const std::exception& e) {
        sak::logError("Fatal error: {}", e.what());
        std::println(std::cerr, "Fatal error: {}", e.what());
        QMessageBox::critical(nullptr,
                              "Fatal Error",
                              QString("Unhandled exception: %1").arg(e.what()));
        return 1;
    } catch (...) {  // Final safety net: re-throw in debug, exit in release
        sak::logError("Unknown fatal error");
        std::println(std::cerr, "Unknown fatal error");
        QMessageBox::critical(nullptr, "Fatal Error", "Unknown unhandled exception");
#ifndef NDEBUG
        // cppcheck-suppress throwInEntryPoint ; intentional re-throw in debug builds only
        throw;
#else
        return 1;
#endif
    }
}
