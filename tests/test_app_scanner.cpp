#include "sak/app_scanner.h"
#include <QDebug>
#include <QCoreApplication>

/**
 * Test program to run real system scan
 * 
 * SCAN-FIRST APPROACH:
 * - This will scan your actual installed applications
 * - Inspect the output to see what data is available
 * - Use this to validate AppInfo structure
 * - Save output to tests/fixtures/scan_results.txt for reference
 */

int main(int argc, char *argv[]) {
    QCoreApplication qapp(argc, argv);
    
    qDebug() << "=== Application Scanner Test ===";
    qDebug() << "Scanning installed applications...";
    qDebug() << "";
    
    sak::AppScanner scanner;
    auto apps = scanner.scanAll();
    
    qDebug() << "";
    qDebug() << "=== SCAN RESULTS ===";
    qDebug() << "Total applications found:" << apps.size();
    qDebug() << "";
    
    // Display detailed info for inspection
    int count = 0;
    for (const auto& app : apps) {
        count++;
        qDebug() << "--- Application" << count << "---";
        qDebug() << "  Name:             " << app.name;
        qDebug() << "  Version:          " << app.version;
        qDebug() << "  Publisher:        " << app.publisher;
        qDebug() << "  Install Date:     " << app.install_date;
        qDebug() << "  Install Location: " << app.install_location;
        qDebug() << "  Uninstall String: " << app.uninstall_string;
        qDebug() << "  Registry Key:     " << app.registry_key;
        qDebug() << "  Source:           " << (app.source == sak::AppScanner::AppInfo::Source::Registry ? "Registry" : "AppX");
        qDebug() << "";
        
        // Stop after 20 apps to avoid spam
        if (count >= 20) {
            qDebug() << "... (showing first 20 apps only)";
            break;
        }
    }
    
    qDebug() << "";
    qDebug() << "=== VALIDATION CHECKLIST ===";
    qDebug() << "Review the output above:";
    qDebug() << "  [ ] Are all app names populated?";
    qDebug() << "  [ ] Are versions present for most apps?";
    qDebug() << "  [ ] Are publishers captured?";
    qDebug() << "  [ ] Are install locations valid paths?";
    qDebug() << "  [ ] Any missing fields that should be added to AppInfo?";
    qDebug() << "  [ ] Any fields with unexpected data?";
    qDebug() << "";
    qDebug() << "Next steps:";
    qDebug() << "  1. Review this output carefully";
    qDebug() << "  2. Adjust AppInfo structure if needed";
    qDebug() << "  3. Save this output to tests/fixtures/scan_results.txt";
    qDebug() << "  4. Proceed to Phase 2: ChocolateyManager";
    
    return 0;
}
