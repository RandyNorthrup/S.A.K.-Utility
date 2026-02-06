#pragma once

#include "sak/app_scanner.h"
#include "sak/chocolatey_manager.h"
#include <QString>
#include <QMap>
#include <QCache>
#include <QMutex>
#include <vector>
#include <optional>
#include <QtConcurrent>

namespace sak {

/**
 * @brief PackageMatcher - Matches installed applications to Chocolatey packages
 * 
 * This class uses multiple strategies to match application names from the registry
 * to their corresponding Chocolatey package names:
 * 1. Exact matches (common app name mappings database)
 * 2. Fuzzy matching with Levenshtein distance
 * 3. Chocolatey search API integration
 * 4. Confidence scoring
 * 
 * Designed to work with real scanned data from AppScanner.
 */
class PackageMatcher {
public:
    PackageMatcher();
    ~PackageMatcher() = default;

    /**
     * @brief Match result with confidence score
     */
    struct MatchResult {
        QString choco_package;      // Chocolatey package name
        QString matched_name;       // Name that was matched
        double confidence;          // 0.0 - 1.0 (1.0 = perfect match)
        QString match_type;         // "exact", "fuzzy", "search", "manual"
        bool available;             // Is package available in Chocolatey?
        QString version;            // Latest available version
    };

    /**
     * @brief Match configuration
     */
    struct MatchConfig {
        bool use_exact_mappings{true};  // Use common app mappings
        bool use_fuzzy_matching{true};  // Use fuzzy string matching
        bool use_choco_search{true};    // Query Chocolatey search API
        double min_confidence{0.5};     // Minimum confidence to return
        int max_search_results{5};      // Max results from Chocolatey search
        bool verify_availability{true}; // Verify package exists in Chocolatey
        int thread_count{8};            // Number of parallel threads
        bool use_cache{true};           // Cache search results
    };

    // Matching operations
    std::optional<MatchResult> findMatch(const AppScanner::AppInfo& app, 
                                         ChocolateyManager* choco_mgr = nullptr,
                                         const MatchConfig& config = MatchConfig());
    
    std::vector<MatchResult> findMatches(const std::vector<AppScanner::AppInfo>& apps,
                                         ChocolateyManager* choco_mgr = nullptr,
                                         const MatchConfig& config = MatchConfig());
    
    // Parallel batch processing (optimized for many apps)
    std::vector<MatchResult> findMatchesParallel(const std::vector<AppScanner::AppInfo>& apps,
                                                 ChocolateyManager* choco_mgr = nullptr,
                                                 const MatchConfig& config = MatchConfig());
    
    // Manual mapping management
    void addMapping(const QString& app_name, const QString& choco_package);
    void removeMapping(const QString& app_name);
    bool hasMapping(const QString& app_name) const;
    QString getMapping(const QString& app_name) const;
    
    // Statistics
    int getMappingCount() const;
    int getExactMatchCount() const;
    int getFuzzyMatchCount() const;
    int getSearchMatchCount() const;
    
    // Export/import mappings
    void exportMappings(const QString& file_path) const;
    void importMappings(const QString& file_path);
    
private:
    // Matching strategies
    std::optional<MatchResult> exactMatch(const QString& app_name);
    std::optional<MatchResult> fuzzyMatch(const QString& app_name, ChocolateyManager* choco_mgr);
    std::optional<MatchResult> searchMatch(const QString& app_name, ChocolateyManager* choco_mgr, int max_results);
    
    // Batch operations (for parallel processing)
    std::vector<QString> batchSearchChocolatey(const QStringList& keywords, ChocolateyManager* choco_mgr);
    
    // Cache management
    void clearCache();
    QString getCachedSearch(const QString& keyword) const;
    void cacheSearch(const QString& keyword, const QString& result);
    
    // String processing
    QString normalizeAppName(const QString& app_name) const;
    QString extractBaseAppName(const QString& app_name) const;
    QStringList extractKeywords(const QString& app_name) const;
    
    // Similarity scoring
    double calculateSimilarity(const QString& str1, const QString& str2) const;
    int levenshteinDistance(const QString& s1, const QString& s2) const;
    double jaroWinklerSimilarity(const QString& s1, const QString& s2) const;
    
    // Common mappings database
    void initializeCommonMappings();
    
    // Member variables
    QMap<QString, QString> m_exact_mappings;  // app_name -> choco_package
    QCache<QString, QString> m_search_cache;  // keyword -> search results (thread-safe)
    mutable QMutex m_cache_mutex;             // Protect cache access
    mutable QMutex m_stats_mutex;             // Protect statistics
    
    // Statistics
    mutable int m_exact_match_count;
    mutable int m_fuzzy_match_count;
    mutable int m_search_match_count;
};

} // namespace sak
