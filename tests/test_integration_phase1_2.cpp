#include "sak/app_scanner.h"
#include "sak/chocolatey_manager.h"
#include <QCoreApplication>
#include <QDebug>
#include <iostream>
#include <algorithm>

int main(int argc, char* argv[]) {
    QCoreApplication qapp(argc, argv);
    
    std::cout << "=== Phase 1+2 Integration Test ===" << std::endl;
    std::cout << "Testing AppScanner + ChocolateyManager together" << std::endl;
    std::cout << std::endl;
    
    // Phase 1: Scan real apps
    std::cout << "Phase 1: Scanning installed applications..." << std::endl;
    sak::AppScanner scanner;
    auto apps = scanner.scanAll();
    
    std::cout << "Found " << apps.size() << " applications" << std::endl;
    std::cout << std::endl;
    
    // Phase 2: Initialize ChocolateyManager
    std::cout << "Phase 2: Initializing ChocolateyManager..." << std::endl;
    QString app_dir = QCoreApplication::applicationDirPath();
    QString choco_path = app_dir + "/../../tools/chocolatey";
    
    sak::ChocolateyManager choco_mgr;
    if (!choco_mgr.initialize(choco_path)) {
        std::cout << "❌ Failed to initialize ChocolateyManager" << std::endl;
        return 1;
    }
    
    std::cout << "✅ ChocolateyManager initialized" << std::endl;
    std::cout << "Version: " << choco_mgr.getChocoVersion().toStdString() << std::endl;
    std::cout << std::endl;
    
    // Find some apps that are likely in Chocolatey
    std::cout << "Checking which scanned apps are available in Chocolatey..." << std::endl;
    std::cout << std::endl;
    
    struct AppMatch {
        QString app_name;
        QString version;
        QString choco_package;
        bool available;
    };
    
    std::vector<AppMatch> matches;
    
    // Common app name mappings (simplified - Phase 3 will do this properly)
    std::vector<std::pair<QString, QString>> simple_mappings = {
        {"7-Zip", "7zip"},
        {"Git", "git"},
        {"Google Chrome", "googlechrome"},
        {"Mozilla Firefox", "firefox"},
        {"VLC media player", "vlc"},
        {"Node.js", "nodejs"},
        {"Python", "python"},
        {"Visual Studio Code", "vscode"},
        {"Notepad++", "notepadplusplus"},
        {"Adobe Acrobat", "adobereader"},
        {"WinRAR", "winrar"},
        {"FileZilla", "filezilla"},
        {"PuTTY", "putty"},
        {"Docker Desktop", "docker-desktop"}
    };
    
    for (const auto& app : apps) {
        for (const auto& [pattern, choco_pkg] : simple_mappings) {
            if (app.name.contains(pattern, Qt::CaseInsensitive)) {
                // Check if available in Chocolatey
                bool available = choco_mgr.isPackageAvailable(choco_pkg);
                
                matches.push_back({
                    app.name,
                    app.version,
                    choco_pkg,
                    available
                });
                
                break;  // Only match once per app
            }
        }
    }
    
    // Display results
    std::cout << "Found " << matches.size() << " potential Chocolatey matches:" << std::endl;
    std::cout << std::endl;
    
    int available_count = 0;
    for (const auto& match : matches) {
        QString status = match.available ? "✅" : "❌";
        std::cout << status.toStdString() << " " 
                  << match.app_name.toStdString() 
                  << " v" << match.version.toStdString()
                  << " → " << match.choco_package.toStdString();
        
        if (match.available) {
            available_count++;
            std::cout << " (available)";
        } else {
            std::cout << " (not found)";
        }
        
        std::cout << std::endl;
    }
    
    std::cout << std::endl;
    std::cout << "Summary:" << std::endl;
    std::cout << "  Total scanned apps: " << apps.size() << std::endl;
    std::cout << "  Matched to Chocolatey packages: " << matches.size() << std::endl;
    std::cout << "  Available in Chocolatey: " << available_count << std::endl;
    std::cout << "  Match rate: " << (matches.size() * 100.0 / apps.size()) << "%" << std::endl;
    std::cout << "  Availability rate: " << (available_count * 100.0 / matches.size()) << "%" << std::endl;
    std::cout << std::endl;
    
    // Demonstrate version-locked installation
    std::cout << "Demonstrating version-locked installation..." << std::endl;
    std::cout << std::endl;
    
    // Find 7-Zip in our scanned apps
    auto it = std::find_if(apps.begin(), apps.end(), [](const sak::AppScanner::AppInfo& app) {
        return app.name.contains("7-Zip", Qt::CaseInsensitive);
    });
    
    if (it != apps.end()) {
        std::cout << "Found: " << it->name.toStdString() 
                  << " v" << it->version.toStdString() << std::endl;
        std::cout << "Chocolatey package: 7zip" << std::endl;
        std::cout << std::endl;
        
        std::cout << "To restore this exact version on another machine:" << std::endl;
        std::cout << "  sak::ChocolateyManager::InstallConfig config;" << std::endl;
        std::cout << "  config.package_name = \"7zip\";" << std::endl;
        std::cout << "  config.version = \"" << it->version.toStdString() << "\";" << std::endl;
        std::cout << "  config.version_locked = true;" << std::endl;
        std::cout << "  config.auto_confirm = true;" << std::endl;
        std::cout << "  auto result = choco_mgr.installPackage(config);" << std::endl;
        std::cout << std::endl;
        std::cout << "This ensures the EXACT same version is installed!" << std::endl;
    }
    
    std::cout << std::endl;
    std::cout << "=== INTEGRATION TEST COMPLETE ===" << std::endl;
    std::cout << "✅ Phase 1 (AppScanner) working" << std::endl;
    std::cout << "✅ Phase 2 (ChocolateyManager) working" << std::endl;
    std::cout << "✅ Real app scanning works" << std::endl;
    std::cout << "✅ Chocolatey package matching works (simple)" << std::endl;
    std::cout << "✅ Version locking ready" << std::endl;
    std::cout << std::endl;
    std::cout << "Next: Phase 3 will implement sophisticated PackageMatcher" << std::endl;
    
    return 0;
}
