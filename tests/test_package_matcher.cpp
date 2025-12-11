#include "sak/app_scanner.h"
#include "sak/chocolatey_manager.h"
#include "sak/package_matcher.h"
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QDebug>
#include <iostream>
#include <iomanip>

int main(int argc, char* argv[]) {
    QCoreApplication qapp(argc, argv);
    
    std::cout << "=== PackageMatcher Test with Real Scanned Apps ===" << std::endl;
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
    std::cout << "✅ ChocolateyManager initialized (v" << choco_mgr.getChocoVersion().toStdString() << ")" << std::endl;
    std::cout << std::endl;
    
    // Phase 3: Initialize PackageMatcher
    std::cout << "Phase 3: Initializing PackageMatcher..." << std::endl;
    sak::PackageMatcher matcher;
    std::cout << "✅ PackageMatcher initialized with " << matcher.getMappingCount() << " common mappings" << std::endl;
    std::cout << std::endl;
    
    // Configure matching
    sak::PackageMatcher::MatchConfig config;
    config.use_exact_mappings = true;
    config.use_fuzzy_matching = true;
    config.use_choco_search = true;
    config.min_confidence = 0.6;  // 60% confidence minimum
    config.max_search_results = 5;
    config.verify_availability = true;
    config.thread_count = 8;      // Use 8 parallel threads
    config.use_cache = true;      // Enable caching
    
    std::cout << "Match Configuration:" << std::endl;
    std::cout << "  Exact mappings: " << (config.use_exact_mappings ? "enabled" : "disabled") << std::endl;
    std::cout << "  Fuzzy matching: " << (config.use_fuzzy_matching ? "enabled" : "disabled") << std::endl;
    std::cout << "  Chocolatey search: " << (config.use_choco_search ? "enabled" : "disabled") << std::endl;
    std::cout << "  Minimum confidence: " << (config.min_confidence * 100) << "%" << std::endl;
    std::cout << "  Parallel threads: " << config.thread_count << std::endl;
    std::cout << "  Caching: " << (config.use_cache ? "enabled" : "disabled") << std::endl;
    std::cout << std::endl;
    
    // Run matching with ALL apps using parallel processing
    std::cout << "Running PackageMatcher on ALL " << apps.size() << " apps (parallel mode)..." << std::endl;
    std::cout << std::endl;
    
    QElapsedTimer timer;
    timer.start();
    
    std::cout << "Matching applications to Chocolatey packages..." << std::endl;
    auto results = matcher.findMatchesParallel(apps, &choco_mgr, config);
    
    qint64 elapsed_ms = timer.elapsed();
    double elapsed_sec = elapsed_ms / 1000.0;
    
    std::cout << std::endl;
    std::cout << "=== MATCHING RESULTS ===" << std::endl;
    std::cout << "Total apps tested: " << apps.size() << std::endl;
    std::cout << "Total time: " << std::fixed << std::setprecision(2) << elapsed_sec << " seconds" << std::endl;
    std::cout << "Apps per second: " << std::fixed << std::setprecision(1) << (apps.size() / elapsed_sec) << std::endl;
    std::cout << std::endl;
    
    // Count match types
    int exact_matches = 0;
    int fuzzy_matches = 0;
    int search_matches = 0;
    
    for (const auto& result : results) {
        if (result.match_type == "exact") exact_matches++;
        else if (result.match_type == "fuzzy") fuzzy_matches++;
        else if (result.match_type == "search") search_matches++;
    }
    
    std::cout << "Matched: " << results.size() << " (" << std::fixed << std::setprecision(1) 
              << (results.size() * 100.0 / apps.size()) << "%)" << std::endl;
    std::cout << "  Exact matches: " << exact_matches << std::endl;
    std::cout << "  Fuzzy matches: " << fuzzy_matches << std::endl;
    std::cout << "  Search matches: " << search_matches << std::endl;
    std::cout << "Not matched: " << (apps.size() - results.size()) << " (" 
              << ((apps.size() - results.size()) * 100.0 / apps.size()) << "%)" << std::endl;
    std::cout << std::endl;
    
    // Show sample of matches
    std::cout << "Sample of matched apps (first 20):" << std::endl;
    for (size_t i = 0; i < std::min(size_t(20), results.size()); ++i) {
        const auto& result = results[i];
        QString symbol = "✅";
        if (result.match_type == "fuzzy") symbol = "🔍";
        else if (result.match_type == "search") symbol = "🔎";
        
        std::cout << symbol.toStdString() << " " 
                  << std::setw(40) << std::left << result.matched_name.left(40).toStdString()
                  << " → " << std::setw(25) << std::left << result.choco_package.toStdString()
                  << " (" << std::setw(5) << std::right << std::fixed << std::setprecision(1) 
                  << (result.confidence * 100) << "%, "
                  << result.match_type.toStdString() << ")"
                  << std::endl;
    }
    std::cout << std::endl;
    
    std::cout << "=== STATISTICS ===" << std::endl;
    std::cout << "Common mappings database: " << matcher.getMappingCount() << " entries" << std::endl;
    std::cout << "Match rate improvement: +" << std::fixed << std::setprecision(1) 
              << ((results.size() * 100.0 / apps.size()) - 13.5) << "% vs Phase 2 baseline" << std::endl;
    std::cout << "Performance: " << std::fixed << std::setprecision(2) 
              << elapsed_sec << "s total, " 
              << (elapsed_ms / apps.size()) << "ms per app average" << std::endl;
    std::cout << std::endl;
    
    // Export mappings
    QString export_path = app_dir + "/../../package_mappings.json";
    matcher.exportMappings(export_path);
    std::cout << "✅ Exported mappings to: " << export_path.toStdString() << std::endl;
    std::cout << std::endl;
    
    std::cout << "=== TEST COMPLETE ===" << std::endl;
    std::cout << "✅ PackageMatcher optimized with parallel processing" << std::endl;
    std::cout << "✅ Caching enabled for faster subsequent runs" << std::endl;
    std::cout << "✅ Ready for Phase 4: MigrationReport" << std::endl;
    
    return 0;
}
