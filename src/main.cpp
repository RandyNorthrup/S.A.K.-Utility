// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file main.cpp
/// @brief SAK Utility main entry point

#include "sak/error_codes.h"
#include "sak/logger.h"
#include "sak/main_window.h"
#include "sak/splash_screen.h"
#include "sak/version.h"
#include "sak/windows11_theme.h"

#include <QApplication>
#include <QCoreApplication>
#include <QFileInfo>
#include <QIcon>
#include <QMessageBox>

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
