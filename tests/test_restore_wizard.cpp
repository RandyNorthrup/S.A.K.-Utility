// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: MIT

/**
 * @file test_restore_wizard.cpp
 * @brief Test program for RestoreWizard GUI component
 */

#include "sak/restore_wizard.h"
#include <QApplication>
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
    
    printPhaseHeader("Restore Wizard Test Program");
    printInfo("S.A.K. Utility - Application Data Restore Wizard");
    printInfo("Copyright (c) 2025 Randy Northrup");
    std::cout << std::endl;
    
    // Phase 1: Create wizard
    printPhaseHeader("Phase 1: Create Restore Wizard");
    
    sak::RestoreWizard wizard;
    printSuccess("RestoreWizard instance created");
    printInfo(QString("Window title: %1").arg(wizard.windowTitle()));
    printInfo(QString("Minimum size: %1x%2").arg(wizard.minimumWidth()).arg(wizard.minimumHeight()));
    
    // Phase 2: Verify pages
    printPhaseHeader("Phase 2: Verify Wizard Pages");
    
    QStringList pageIds;
    pageIds << "Welcome" << "Select Backup" << "Configure" << "Progress";
    
    for (int i = 0; i < pageIds.size(); i++) {
        auto* page = wizard.page(i);
        if (page) {
            printSuccess(QString("Page %1 (%2): %3").arg(i).arg(pageIds[i]).arg(page->title()));
        } else {
            std::cerr << "[ERROR] Page " << i << " is null" << std::endl;
        }
    }
    
    // Phase 3: Display wizard
    printPhaseHeader("Phase 3: Display Wizard");
    printInfo("Showing wizard window...");
    printInfo("Please interact with the wizard:");
    printInfo("  1. Welcome Page - Click Next");
    printInfo("  2. Select Backup - Browse for backup directory");
    printInfo("      (Default: Documents/SAK Backups)");
    printInfo("      Select backups, optionally verify them");
    printInfo("  3. Configure - Choose restore location and options");
    printInfo("      (Can use original location or browse)");
    printInfo("  4. Progress - Watch restore execute");
    printInfo("");
    printInfo("NOTE: You need existing backups to test restore functionality.");
    printInfo("      Run test_backup_wizard first to create test backups.");
    printInfo("Close the wizard to complete the test.");
    std::cout << std::endl;
    
    wizard.show();
    
    // Run event loop
    int result = app.exec();
    
    // Phase 4: Test completion
    printPhaseHeader("Phase 4: Test Results");
    
    if (wizard.result() == QDialog::Accepted) {
        printSuccess("Wizard completed successfully (user clicked Finish)");
    } else {
        printInfo("Wizard cancelled by user");
    }
    
    printInfo("");
    printInfo("Test program finished");
    std::cout << std::string(60, '=') << "\n" << std::endl;
    
    return result;
}
