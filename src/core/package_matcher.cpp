#include "sak/package_matcher.h"
#include <QRegularExpression>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QDebug>
#include <algorithm>
#include <cmath>

namespace sak {

PackageMatcher::PackageMatcher()
    : m_exact_match_count(0)
    , m_fuzzy_match_count(0)
    , m_search_match_count(0)
{
    initializeCommonMappings();
    m_search_cache.setMaxCost(1000);  // Cache up to 1000 search results
}

void PackageMatcher::initializeCommonMappings() {
    // Common application name to Chocolatey package mappings
    // Based on real scanned apps from Phase 1
    
    // Browsers
    m_exact_mappings["Google Chrome"] = "googlechrome";
    m_exact_mappings["Mozilla Firefox"] = "firefox";
    m_exact_mappings["Microsoft Edge"] = "microsoft-edge";
    m_exact_mappings["Opera"] = "opera";
    m_exact_mappings["Brave"] = "brave";
    
    // Development tools
    m_exact_mappings["Visual Studio Code"] = "vscode";
    m_exact_mappings["Git"] = "git";
    m_exact_mappings["GitHub Desktop"] = "github-desktop";
    m_exact_mappings["GitKraken"] = "gitkraken";
    m_exact_mappings["Docker Desktop"] = "docker-desktop";
    m_exact_mappings["Node.js"] = "nodejs";
    m_exact_mappings["Python"] = "python";
    m_exact_mappings["Java"] = "javaruntime";
    m_exact_mappings["CMake"] = "cmake";
    
    // Compression tools
    m_exact_mappings["7-Zip"] = "7zip";
    m_exact_mappings["WinRAR"] = "winrar";
    m_exact_mappings["WinZip"] = "winzip";
    
    // Media players
    m_exact_mappings["VLC media player"] = "vlc";
    m_exact_mappings["iTunes"] = "itunes";
    m_exact_mappings["Spotify"] = "spotify";
    m_exact_mappings["Audacity"] = "audacity";
    
    // Text editors
    m_exact_mappings["Notepad++"] = "notepadplusplus";
    m_exact_mappings["Sublime Text"] = "sublimetext3";
    m_exact_mappings["Atom"] = "atom";
    
    // Communication
    m_exact_mappings["Discord"] = "discord";
    m_exact_mappings["Slack"] = "slack";
    m_exact_mappings["Zoom"] = "zoom";
    m_exact_mappings["Microsoft Teams"] = "microsoft-teams";
    m_exact_mappings["Skype"] = "skype";
    
    // Utilities
    m_exact_mappings["PuTTY"] = "putty";
    m_exact_mappings["WinSCP"] = "winscp";
    m_exact_mappings["FileZilla"] = "filezilla";
    m_exact_mappings["TeamViewer"] = "teamviewer";
    m_exact_mappings["AnyDesk"] = "anydesk";
    m_exact_mappings["Wireshark"] = "wireshark";
    
    // Office & Productivity
    m_exact_mappings["Adobe Acrobat"] = "adobereader";
    m_exact_mappings["GIMP"] = "gimp";
    m_exact_mappings["Inkscape"] = "inkscape";
    m_exact_mappings["OBS Studio"] = "obs-studio";
    m_exact_mappings["VirtualBox"] = "virtualbox";
    
    // Download managers
    m_exact_mappings["qBittorrent"] = "qbittorrent";
    m_exact_mappings["Steam"] = "steam";
}

std::optional<PackageMatcher::MatchResult> PackageMatcher::findMatch(
    const AppScanner::AppInfo& app,
    ChocolateyManager* choco_mgr,
    const MatchConfig& config)
{
    QString normalized_name = normalizeAppName(app.name);
    QString base_name = extractBaseAppName(app.name);
    
    // Strategy 1: Exact mapping
    if (config.use_exact_mappings) {
        auto exact = exactMatch(base_name);
        if (exact.has_value() && exact->confidence >= config.min_confidence) {
            // Verify availability if requested
            if (config.verify_availability && choco_mgr) {
                exact->available = choco_mgr->isPackageAvailable(exact->choco_package);
            }
            if (!config.verify_availability || exact->available) {
                m_exact_match_count++;
                return exact;
            }
        }
    }
    
    // Strategy 2: Fuzzy matching
    if (config.use_fuzzy_matching && choco_mgr) {
        auto fuzzy = fuzzyMatch(base_name, choco_mgr);
        if (fuzzy.has_value() && fuzzy->confidence >= config.min_confidence) {
            m_fuzzy_match_count++;
            return fuzzy;
        }
    }
    
    // Strategy 3: Chocolatey search
    if (config.use_choco_search && choco_mgr) {
        auto search = searchMatch(base_name, choco_mgr, config.max_search_results);
        if (search.has_value() && search->confidence >= config.min_confidence) {
            m_search_match_count++;
            return search;
        }
    }
    
    return std::nullopt;
}

std::vector<PackageMatcher::MatchResult> PackageMatcher::findMatches(
    const std::vector<AppScanner::AppInfo>& apps,
    ChocolateyManager* choco_mgr,
    const MatchConfig& config)
{
    std::vector<MatchResult> results;
    const size_t app_count = apps.size();
    results.reserve(app_count);
    
    for (const auto& app : apps) {
        auto match = findMatch(app, choco_mgr, config);
        if (match.has_value()) {
            results.push_back(*match);
        }
    }
    
    return results;
}

std::vector<PackageMatcher::MatchResult> PackageMatcher::findMatchesParallel(
    const std::vector<AppScanner::AppInfo>& apps,
    ChocolateyManager* choco_mgr,
    const MatchConfig& config)
{
    // Phase 1: Quick exact matches (no API calls needed)
    std::vector<std::pair<int, MatchResult>> exact_results;
    std::vector<std::pair<int, AppScanner::AppInfo>> fuzzy_candidates;
    
    const size_t app_count = apps.size();
    exact_results.reserve(app_count / 2);  // Estimate 50% exact matches
    fuzzy_candidates.reserve(app_count / 2);
    
    for (size_t i = 0; i < app_count; ++i) {
        QString base_name = extractBaseAppName(apps[i].name);
        auto exact = exactMatch(base_name);
        if (exact.has_value() && exact->confidence >= config.min_confidence) {
            exact_results.push_back({static_cast<int>(i), *exact});
            QMutexLocker locker(&m_stats_mutex);
            m_exact_match_count++;
        } else {
            fuzzy_candidates.push_back({static_cast<int>(i), apps[i]});
        }
    }
    
    // Phase 2: Parallel fuzzy/search matching for remaining apps
    QThreadPool pool;
    pool.setMaxThreadCount(config.thread_count);
    
    auto fuzzy_results = QtConcurrent::blockingMapped<std::vector<std::pair<int, std::optional<MatchResult>>>>(
        &pool,
        fuzzy_candidates,
        [this, choco_mgr, &config](const std::pair<int, AppScanner::AppInfo>& item) -> std::pair<int, std::optional<MatchResult>> {
            const auto& [idx, app] = item;
            QString base_name = extractBaseAppName(app.name);
            
            // Try fuzzy matching first
            if (config.use_fuzzy_matching && choco_mgr) {
                auto fuzzy = fuzzyMatch(base_name, choco_mgr);
                if (fuzzy.has_value() && fuzzy->confidence >= config.min_confidence) {
                    QMutexLocker locker(&m_stats_mutex);
                    m_fuzzy_match_count++;
                    return {idx, fuzzy};
                }
            }
            
            // Fall back to search
            if (config.use_choco_search && choco_mgr) {
                auto search = searchMatch(base_name, choco_mgr, config.max_search_results);
                if (search.has_value() && search->confidence >= config.min_confidence) {
                    QMutexLocker locker(&m_stats_mutex);
                    m_search_match_count++;
                    return {idx, search};
                }
            }
            
            return {idx, std::nullopt};
        }
    );
    
    // Combine results in original order
    std::vector<MatchResult> all_results(apps.size());
    std::vector<bool> has_match(apps.size(), false);
    
    // Add exact matches
    for (const auto& [idx, result] : exact_results) {
        all_results[idx] = result;
        has_match[idx] = true;
    }
    
    // Add fuzzy/search matches
    for (const auto& [idx, result] : fuzzy_results) {
        if (result.has_value()) {
            all_results[idx] = *result;
            has_match[idx] = true;
        }
    }
    
    // Extract only matched apps
    std::vector<MatchResult> final_results;
    for (size_t i = 0; i < apps.size(); ++i) {
        if (has_match[i]) {
            final_results.push_back(all_results[i]);
        }
    }
    
    return final_results;
}

std::optional<PackageMatcher::MatchResult> PackageMatcher::exactMatch(const QString& app_name) {
    QString normalized = normalizeAppName(app_name);
    
    // Check direct mapping
    if (m_exact_mappings.contains(normalized)) {
        QString choco_pkg = m_exact_mappings[normalized];
        return MatchResult{
            choco_pkg,
            normalized,
            1.0,  // Perfect confidence
            "exact",
            true,  // Assume available (will be verified if requested)
            ""
        };
    }
    
    // Check case-insensitive
    for (auto it = m_exact_mappings.begin(); it != m_exact_mappings.end(); ++it) {
        if (it.key().compare(normalized, Qt::CaseInsensitive) == 0) {
            return MatchResult{
                it.value(),
                it.key(),
                0.95,  // High confidence for case-insensitive match
                "exact",
                true,
                ""
            };
        }
    }
    
    return std::nullopt;
}

std::optional<PackageMatcher::MatchResult> PackageMatcher::fuzzyMatch(
    const QString& app_name,
    ChocolateyManager* choco_mgr)
{
    QString normalized = normalizeAppName(app_name);
    QStringList keywords = extractKeywords(app_name);
    
    // Search for each keyword
    double best_similarity = 0.0;
    QString best_package;
    QString best_matched_name;
    
    for (const QString& keyword : keywords) {
        if (keyword.length() < 3) continue;  // Skip short keywords
        
        // Check cache first
        QString cached = getCachedSearch(keyword);
        QString search_output;
        
        if (!cached.isEmpty()) {
            search_output = cached;
        } else {
            // Search Chocolatey
            auto result = choco_mgr->searchPackage(keyword, 10);
            if (!result.success) continue;
            
            search_output = result.output;
            cacheSearch(keyword, search_output);  // Cache for future use
        }
        
        auto packages = choco_mgr->parseSearchResults(search_output);
        
        for (const auto& pkg : packages) {
            double similarity = calculateSimilarity(normalized, pkg.package_id);
            if (similarity > best_similarity) {
                best_similarity = similarity;
                best_package = pkg.package_id;
                best_matched_name = pkg.title;
            }
        }
    }
    
    if (best_similarity >= 0.6) {  // Minimum threshold for fuzzy match
        return MatchResult{
            best_package,
            best_matched_name,
            best_similarity,
            "fuzzy",
            true,
            ""
        };
    }
    
    return std::nullopt;
}

std::optional<PackageMatcher::MatchResult> PackageMatcher::searchMatch(
    const QString& app_name,
    ChocolateyManager* choco_mgr,
    int max_results)
{
    QString base_name = extractBaseAppName(app_name);
    QStringList keywords = extractKeywords(base_name);
    
    // Try searching with the base name first
    auto result = choco_mgr->searchPackage(base_name, max_results);
    if (!result.success) {
        return std::nullopt;
    }
    
    auto packages = choco_mgr->parseSearchResults(result.output);
    if (packages.empty()) {
        return std::nullopt;
    }
    
    // Find best match from search results
    double best_score = 0.0;
    ChocolateyManager::PackageInfo best_package;
    
    for (const auto& pkg : packages) {
        double score = calculateSimilarity(base_name, pkg.package_id);
        
        // Boost score if title matches better
        double title_score = calculateSimilarity(base_name, pkg.title);
        if (title_score > score) {
            score = title_score * 0.9;  // Slightly lower weight for title match
        }
        
        if (score > best_score) {
            best_score = score;
            best_package = pkg;
        }
    }
    
    if (best_score >= 0.5) {  // Minimum threshold for search match
        return MatchResult{
            best_package.package_id,
            best_package.title,
            best_score,
            "search",
            true,
            best_package.version
        };
    }
    
    return std::nullopt;
}

QString PackageMatcher::normalizeAppName(const QString& app_name) const {
    QString normalized = app_name;
    
    // Remove common suffixes
    QStringList suffixes = {
        " (x64)", " (64-bit)", " (x86)", " (32-bit)",
        " (64 bit)", " (32 bit)", " (Remove only)",
        " for Windows", " Desktop", " Application"
    };
    
    for (const QString& suffix : suffixes) {
        if (normalized.endsWith(suffix, Qt::CaseInsensitive)) {
            normalized.chop(suffix.length());
        }
    }
    
    // Remove version numbers at the end
    QRegularExpression versionRegex(R"(\s+v?\d+(\.\d+)*(\s+\w+)?$)");
    normalized.remove(versionRegex);
    
    return normalized.trimmed();
}

QString PackageMatcher::extractBaseAppName(const QString& app_name) const {
    QString base = normalizeAppName(app_name);
    
    // Extract just the main app name (before first space or hyphen)
    QStringList words = base.split(QRegularExpression(R"([\s\-_]+)"), Qt::SkipEmptyParts);
    
    if (!words.isEmpty()) {
        // Return first 1-3 words as base name
        int word_count = std::min(3, static_cast<int>(words.size()));
        QStringList base_words;
        for (int i = 0; i < word_count; ++i) {
            base_words.append(words[i]);
        }
        return base_words.join(" ");
    }
    
    return base;
}

QStringList PackageMatcher::extractKeywords(const QString& app_name) const {
    QString normalized = normalizeAppName(app_name);
    
    // Split into words
    QStringList words = normalized.split(QRegularExpression(R"([\s\-_]+)"), Qt::SkipEmptyParts);
    
    // Remove common words
    QStringList stopWords = {"the", "for", "and", "or", "software", "application", "app"};
    
    QStringList keywords;
    for (const QString& word : words) {
        QString lower = word.toLower();
        if (!stopWords.contains(lower) && word.length() >= 3) {
            keywords.append(word);
        }
    }
    
    return keywords;
}

double PackageMatcher::calculateSimilarity(const QString& str1, const QString& str2) const {
    QString s1 = str1.toLower();
    QString s2 = str2.toLower();
    
    // Exact match
    if (s1 == s2) return 1.0;
    
    // Contains match
    if (s1.contains(s2) || s2.contains(s1)) {
        return 0.85;
    }
    
    // Jaro-Winkler similarity
    double jw = jaroWinklerSimilarity(s1, s2);
    
    // Levenshtein-based similarity
    int lev_dist = levenshteinDistance(s1, s2);
    int max_len = std::max(s1.length(), s2.length());
    double lev_sim = max_len > 0 ? 1.0 - (double(lev_dist) / max_len) : 0.0;
    
    // Average of both methods
    return (jw + lev_sim) / 2.0;
}

int PackageMatcher::levenshteinDistance(const QString& s1, const QString& s2) const {
    const int len1 = s1.length();
    const int len2 = s2.length();
    
    std::vector<std::vector<int>> d(len1 + 1, std::vector<int>(len2 + 1));
    
    for (int i = 0; i <= len1; ++i) d[i][0] = i;
    for (int j = 0; j <= len2; ++j) d[0][j] = j;
    
    for (int i = 1; i <= len1; ++i) {
        for (int j = 1; j <= len2; ++j) {
            int cost = (s1[i-1] == s2[j-1]) ? 0 : 1;
            d[i][j] = std::min({
                d[i-1][j] + 1,      // deletion
                d[i][j-1] + 1,      // insertion
                d[i-1][j-1] + cost  // substitution
            });
        }
    }
    
    return d[len1][len2];
}

double PackageMatcher::jaroWinklerSimilarity(const QString& s1, const QString& s2) const {
    if (s1 == s2) return 1.0;
    
    int len1 = s1.length();
    int len2 = s2.length();
    
    if (len1 == 0 || len2 == 0) return 0.0;
    
    int match_distance = std::max(len1, len2) / 2 - 1;
    if (match_distance < 1) match_distance = 1;
    
    std::vector<bool> s1_matches(len1, false);
    std::vector<bool> s2_matches(len2, false);
    
    int matches = 0;
    int transpositions = 0;
    
    // Find matches
    for (int i = 0; i < len1; ++i) {
        int start = std::max(0, i - match_distance);
        int end = std::min(i + match_distance + 1, len2);
        
        for (int j = start; j < end; ++j) {
            if (s2_matches[j] || s1[i] != s2[j]) continue;
            s1_matches[i] = true;
            s2_matches[j] = true;
            ++matches;
            break;
        }
    }
    
    if (matches == 0) return 0.0;
    
    // Count transpositions
    int k = 0;
    for (int i = 0; i < len1; ++i) {
        if (!s1_matches[i]) continue;
        while (!s2_matches[k]) ++k;
        if (s1[i] != s2[k]) ++transpositions;
        ++k;
    }
    
    double jaro = ((double(matches) / len1) +
                   (double(matches) / len2) +
                   ((matches - transpositions / 2.0) / matches)) / 3.0;
    
    // Jaro-Winkler adjustment
    int prefix = 0;
    int min_len = std::min(static_cast<int>(len1), static_cast<int>(len2));
    for (int i = 0; i < std::min(min_len, 4); ++i) {
        if (s1[i] == s2[i]) ++prefix;
        else break;
    }
    
    return jaro + (prefix * 0.1 * (1.0 - jaro));
}

void PackageMatcher::addMapping(const QString& app_name, const QString& choco_package) {
    QString normalized = normalizeAppName(app_name);
    m_exact_mappings[normalized] = choco_package;
}

void PackageMatcher::removeMapping(const QString& app_name) {
    QString normalized = normalizeAppName(app_name);
    m_exact_mappings.remove(normalized);
}

bool PackageMatcher::hasMapping(const QString& app_name) const {
    QString normalized = normalizeAppName(app_name);
    return m_exact_mappings.contains(normalized);
}

QString PackageMatcher::getMapping(const QString& app_name) const {
    QString normalized = normalizeAppName(app_name);
    return m_exact_mappings.value(normalized, QString());
}

int PackageMatcher::getMappingCount() const {
    return m_exact_mappings.size();
}

int PackageMatcher::getExactMatchCount() const {
    return m_exact_match_count;
}

int PackageMatcher::getFuzzyMatchCount() const {
    return m_fuzzy_match_count;
}

int PackageMatcher::getSearchMatchCount() const {
    return m_search_match_count;
}

void PackageMatcher::exportMappings(const QString& file_path) const {
    QJsonObject root;
    QJsonArray mappings;
    
    for (auto it = m_exact_mappings.begin(); it != m_exact_mappings.end(); ++it) {
        QJsonObject mapping;
        mapping["app_name"] = it.key();
        mapping["choco_package"] = it.value();
        mappings.append(mapping);
    }
    
    root["mappings"] = mappings;
    root["count"] = m_exact_mappings.size();
    
    QFile file(file_path);
    if (file.open(QIODevice::WriteOnly)) {
        file.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
        file.close();
    }
}

void PackageMatcher::importMappings(const QString& file_path) {
    QFile file(file_path);
    if (!file.open(QIODevice::ReadOnly)) {
        qWarning() << "[PackageMatcher] Failed to open mappings file:" << file_path;
        return;
    }
    
    QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
    file.close();
    
    if (!doc.isObject()) return;
    
    QJsonObject root = doc.object();
    QJsonArray mappings = root["mappings"].toArray();
    
    for (const QJsonValue& value : mappings) {
        QJsonObject mapping = value.toObject();
        QString app_name = mapping["app_name"].toString();
        QString choco_package = mapping["choco_package"].toString();
        
        if (!app_name.isEmpty() && !choco_package.isEmpty()) {
            m_exact_mappings[app_name] = choco_package;
        }
    }
}

void PackageMatcher::clearCache() {
    QMutexLocker locker(&m_cache_mutex);
    m_search_cache.clear();
}

QString PackageMatcher::getCachedSearch(const QString& keyword) const {
    QMutexLocker locker(&m_cache_mutex);
    QString* cached = m_search_cache.object(keyword);
    return cached ? *cached : QString();
}

void PackageMatcher::cacheSearch(const QString& keyword, const QString& result) {
    QMutexLocker locker(&m_cache_mutex);
    m_search_cache.insert(keyword, new QString(result), 1);
}

std::vector<QString> PackageMatcher::batchSearchChocolatey(
    const QStringList& keywords,
    ChocolateyManager* choco_mgr)
{
    std::vector<QString> results;
    results.reserve(keywords.size());
    
    // Filter out cached keywords
    QStringList uncached_keywords;
    for (const QString& keyword : keywords) {
        QString cached = getCachedSearch(keyword);
        if (!cached.isEmpty()) {
            results.push_back(cached);
        } else {
            uncached_keywords.append(keyword);
        }
    }
    
    // Batch search uncached keywords (could be optimized further with single choco call)
    for (const QString& keyword : uncached_keywords) {
        auto result = choco_mgr->searchPackage(keyword, 10);
        if (result.success) {
            cacheSearch(keyword, result.output);
            results.push_back(result.output);
        } else {
            results.push_back(QString());
        }
    }
    
    return results;
}

} // namespace sak
