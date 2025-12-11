// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: MIT

/**
 * @file test_backup_wizard.cpp
 * @brief Test program for BackupWizard GUI component
 */

#include "sak/backup_wizard.h"
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
    
    printPhaseHeader("Backup Wizard Test Program");
    printInfo("S.A.K. Utility - Application Data Backup Wizard");
    printInfo("Copyright (c) 2025 Randy Northrup");
    std::cout << std::endl;
    
    // Phase 1: Create wizard
    printPhaseHeader("Phase 1: Create Backup Wizard");
    
    sak::BackupWizard wizard;
    printSuccess("BackupWizard instance created");
    printInfo(QString("Window title: %1").arg(wizard.windowTitle()));
    printInfo(QString("Minimum size: %1x%2").arg(wizard.minimumWidth()).arg(wizard.minimumHeight()));
    
    // Phase 2: Verify pages
    printPhaseHeader("Phase 2: Verify Wizard Pages");
    
    QStringList pageIds;
    pageIds << "Welcome" << "Select Apps" << "Configure" << "Progress";
    
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
    printInfo("  2. Select Apps - Choose apps or browse custom paths");
    printInfo("  3. Configure - Set backup location and options");
    printInfo("  4. Progress - Watch backup execute (or test with empty selection)");
    printInfo("");
    printInfo("The wizard will execute backup operations if you select apps.");
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
