/// @file main.cpp
/// @brief SAK Utility main entry point

#include "sak/version.h"
#include "sak/logger.h"
#include "sak/file_hash.h"
#include "sak/error_codes.h"
#include "sak/main_window.h"
#include <QApplication>
#include <QMessageBox>
#include <filesystem>
#include <print>
#include <iostream>

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
        
        // Create and show main window
        sak::log_info("Creating main window...");
        MainWindow main_window;
        main_window.show();
        
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
