// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: MIT

/**
 * @file test_app_migration_panel.cpp
 * @brief Test program for AppMigrationPanel GUI component
 */

#include "sak/app_migration_panel.h"
#include <QApplication>
#include <QMainWindow>
#include <QMenuBar>
#include <QStatusBar>
#include <QDebug>
#include <iostream>

void printPhaseHeader(const QString& phase) {
    std::cout << "\n" << std::string(60, '=') << "\n";
    std::cout << phase.toStdString() << "\n";
    std::cout << std::string(60, '=') << "\n" << std::endl;
}

void printSuccess(const QString& message) {
    std::cout << "[SUCCESS] " << message.toStdString() << std::endl;
}

void printInfo(const QString& message) {
    std::cout << "[INFO] " << message.toStdString() << std::endl;
}

int main(int argc, char* argv[]) {
    QApplication app(argc, argv);
    
    printPhaseHeader("App Migration Panel Test Program");
    printInfo("S.A.K. Utility - Application Migration Panel");
    printInfo("Copyright (c) 2025 Randy Northrup");
    std::cout << std::endl;
    
    // Phase 1: Create main window
    printPhaseHeader("Phase 1: Create Main Window");
    
    QMainWindow mainWindow;
    mainWindow.setWindowTitle("S.A.K. Utility - App Migration Panel Test");
    mainWindow.resize(1200, 700);
    
    printSuccess("Main window created");
    printInfo(QString("Window size: %1x%2").arg(mainWindow.width()).arg(mainWindow.height()));
    
    // Phase 2: Create migration panel
    printPhaseHeader("Phase 2: Create App Migration Panel");
    
    sak::AppMigrationPanel* panel = new sak::AppMigrationPanel(&mainWindow);
    mainWindow.setCentralWidget(panel);
    
    printSuccess("AppMigrationPanel instance created");
    
    // Connect panel signals to status bar
    auto* statusBar = mainWindow.statusBar();
    QObject::connect(panel, &sak::AppMigrationPanel::status_message,
                    [statusBar](const QString& msg, int timeout) {
                        statusBar->showMessage(msg, timeout);
                    });
    
    printSuccess("Panel signals connected to status bar");
    
    // Phase 3: Display window
    printPhaseHeader("Phase 3: Display Main Window");
    printInfo("Showing main window with migration panel...");
    printInfo("");
    printInfo("Available Actions:");
    printInfo("  1. Scan Apps - Scans installed applications on this system");
    printInfo("  2. Match Packages - Matches applications to Chocolatey packages");
    printInfo("  3. Backup Data - Opens wizard to backup user data");
    printInfo("  4. Install - Installs selected packages via Chocolatey");
    printInfo("  5. Restore Data - Opens wizard to restore user data");
    printInfo("  6. Export Report - Generates migration report");
    printInfo("  7. Load Report - Loads existing migration report");
    printInfo("");
    printInfo("Usage Pattern:");
    printInfo("  Source Machine:");
    printInfo("    1. Scan Apps → Match Packages → Backup Data → Export Report");
    printInfo("  Target Machine:");
    printInfo("    1. Load Report → Install → Restore Data");
    printInfo("");
    printInfo("Close the window to complete the test.");
    std::cout << std::endl;
    
    mainWindow.show();
    
    // Run event loop
    int result = app.exec();
    
    // Phase 4: Test completion
    printPhaseHeader("Phase 4: Test Results");
    
    printSuccess("Panel displayed successfully");
    printInfo("User closed the window");
    printInfo("");
    printInfo("Test program finished");
    std::cout << std::string(60, '=') << "\n" << std::endl;
    
    return result;
}
