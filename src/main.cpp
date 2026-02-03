/// @file main.cpp
/// @brief SAK Utility main entry point

#include "sak/version.h"
#include "sak/logger.h"
#include "sak/file_hash.h"
#include "sak/error_codes.h"
#include "sak/main_window.h"
#include "sak/quick_action_controller.h"
#include "sak/actions/action_factory.h"
#include "sak/quick_action_result_io.h"
#include "gui/windows11_theme.h"
#include "gui/splash_screen.h"
#include <QApplication>
#include <QCoreApplication>
#include <QMessageBox>
#include <QFileInfo>
#include <QIcon>
#include <filesystem>
#include <print>
#include <iostream>
#include <memory>

namespace {

QString findSplashPath() {
    const QString app_dir = QCoreApplication::applicationDirPath();
    const QStringList candidates = {
        app_dir + "/sak_splash.png",
        app_dir + "/resources/sak_splash.png",
        app_dir + "/../resources/sak_splash.png",
        app_dir + "/../sak_splash.png"
    };

    for (const auto& path : candidates) {
        if (QFileInfo::exists(path)) {
            return path;
        }
    }

    return {};
}

QString findIconPath() {
    const QString app_dir = QCoreApplication::applicationDirPath();
    const QStringList candidates = {
        app_dir + "/icon.ico",
        app_dir + "/resources/icon.ico",
        app_dir + "/../resources/icon.ico"
    };

    for (const auto& path : candidates) {
        if (QFileInfo::exists(path)) {
            return path;
        }
    }

    return {};
}

} // namespace

/// @brief Main application entry point
/// @param argc Argument count
/// @param argv Argument vector
/// @return Exit code
int main(int argc, char* argv[]) {
    try {
        // Initialize Qt application
        QApplication app(argc, argv);
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
        
        // Setup log directory
        auto log_dir = std::filesystem::current_path() / "_logs";
        
        // Initialize logger
        auto& logger = sak::logger::instance();
        if (auto result = logger.initialize(log_dir); !result) {
            QMessageBox::critical(
                nullptr,
                "Initialization Error",
                QString("Failed to initialize logger: %1")
                    .arg(QString::fromStdString(std::string(sak::to_string(result.error())))));
            return 1;
        }
        
        sak::log_info("===========================================");
        sak::log_info("SAK Utility Starting");
        sak::log_info("Version: {}", sak::get_version());
        sak::log_info("C++ Standard: C++{}", __cplusplus);
#ifdef SAK_PLATFORM_WINDOWS
        sak::log_info("Platform: Windows");
#elif defined(SAK_PLATFORM_MACOS)
        sak::log_info("Platform: macOS");
#elif defined(SAK_PLATFORM_LINUX)
        sak::log_info("Platform: Linux");
#endif
        sak::log_info("Qt Version: {}", QT_VERSION_STR);
        sak::log_info("===========================================");

        // Headless quick action runner (elevated mode)
        QString action_to_run;
        QString backup_location = "C:/SAK_Backups";
        QString result_file;

        for (int i = 1; i < argc; ++i) {
            const QString arg = QString::fromLocal8Bit(argv[i]);
            if (arg == "--run-quick-action" && i + 1 < argc) {
                action_to_run = QString::fromLocal8Bit(argv[++i]);
            } else if (arg == "--backup-location" && i + 1 < argc) {
                backup_location = QString::fromLocal8Bit(argv[++i]);
            } else if (arg == "--result-file" && i + 1 < argc) {
                result_file = QString::fromLocal8Bit(argv[++i]);
            }
        }

        if (!action_to_run.isEmpty()) {
            sak::log_info("Running elevated quick action: {}", action_to_run.toStdString());

            sak::QuickActionController controller;
            controller.setBackupLocation(backup_location);

            QObject::connect(&controller, &sak::QuickActionController::logMessage, [](const QString& message) {
                sak::log_info("{}", message.toStdString());
            });

            auto actions = sak::ActionFactory::createAllActions(backup_location);
            for (auto& action : actions) {
                controller.registerAction(std::move(action));
            }

            sak::QuickAction* action = controller.getAction(action_to_run);
            if (!action) {
                sak::QuickAction::ExecutionResult result;
                result.success = false;
                result.message = "Action not found";
                result.log = QString("No action registered with name: %1").arg(action_to_run);
                if (!result_file.isEmpty()) {
                    QString error_message;
                    if (!sak::writeExecutionResultFile(result_file, result, sak::QuickAction::ActionStatus::Failed, &error_message)) {
                        sak::log_warning("Failed to write result file: {}", error_message.toStdString());
                    }
                }
                return 1;
            }

            QObject::connect(&controller, &sak::QuickActionController::actionExecutionComplete,
                             &app, [&app, action, result_file](sak::QuickAction* completed) {
                if (completed != action) {
                    return;
                }

                if (!result_file.isEmpty()) {
                    QString error_message;
                    if (!sak::writeExecutionResultFile(
                            result_file,
                            action->lastExecutionResult(),
                            action->status(),
                            &error_message)) {
                        sak::log_warning("Failed to write result file: {}", error_message.toStdString());
                    }
                }

                const int exit_code = action->lastExecutionResult().success ? 0 : 2;
                app.exit(exit_code);
            }, Qt::QueuedConnection);

            controller.executeAction(action->name(), false);
            return app.exec();
        }
        
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

        // Create and show main window
        sak::log_info("Creating main window...");
        MainWindow main_window;
        main_window.show();

        if (splash) {
            splash->finish();
        }
        
        sak::log_info("Main window displayed - application ready");
        
        // Enter Qt event loop
        int result = app.exec();
        
        sak::log_info("Application shutting down with exit code: {}", result);
        logger.flush();
        
        return result;
        
    } catch (const std::exception& e) {
        std::println(std::cerr, "Fatal error: {}", e.what());
        QMessageBox::critical(
            nullptr,
            "Fatal Error",
            QString("Unhandled exception: %1").arg(e.what()));
        return 1;
    } catch (...) {
        std::println(std::cerr, "Unknown fatal error");
        QMessageBox::critical(
            nullptr,
            "Fatal Error",
            "Unknown unhandled exception");
        return 1;
    }
}
