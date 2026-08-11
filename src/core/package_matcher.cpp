// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/package_matcher.h"

#include "sak/logger.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSaveFile>

#include <algorithm>
#include <cmath>

namespace {

constexpr int kSearchCacheMaxCost = 1000;
constexpr size_t kCandidateReserveDivisor = 2;
constexpr double kCaseInsensitiveExactConfidence = 0.95;
constexpr int kChocoSearchResultLimit = 10;
constexpr int kMinimumKeywordLength = 3;
constexpr double kFuzzyMatchThreshold = 0.6;
constexpr double kTitleMatchWeight = 0.9;
constexpr double kSearchMatchThreshold = 0.5;
constexpr int kBaseNameMaxWordCount = 3;
constexpr double kContainedNameSimilarity = 0.85;
constexpr double kSimilarityAverageDivisor = 2.0;
constexpr int kJaroMatchWindowDivisor = 2;
// Upper bound on either string length fed to the Levenshtein matrix (~4 MiB of ints at the cap).
constexpr int kMaxLevenshteinLen = 1024;

/// @brief Find a matching character in s2 within match_distance of position i in s1.
/// @return Index in s2 if found, -1 otherwise.
int findJaroMatch(const QString& s1,
                  const QString& s2,
                  int i,
                  int match_distance,
                  std::vector<bool>& s2_matches) {
    const int start = std::max(0, i - match_distance);
    const int end = std::min(i + match_distance + 1, static_cast<int>(s2.length()));
    for (int j = start; j < end; ++j) {
        if (s2_matches[j] || s1[i] != s2[j]) {
            continue;
        }
        s2_matches[j] = true;
        return j;
    }
    return -1;
}

}  // namespace

namespace sak {

PackageMatcher::PackageMatcher()
    : m_exact_match_count(0), m_fuzzy_match_count(0), m_search_match_count(0) {
    initializeCommonMappings();
    m_search_cache.setMaxCost(kSearchCacheMaxCost);
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

std::optional<PackageMatcher::MatchResult> PackageMatcher::resolveExactMatch(
    const QString& base_name, ChocolateyManager* choco_mgr, const MatchConfig& config) {
    auto exact = exactMatch(base_name);
    if (!exact.has_value() || exact->confidence < config.min_confidence) {
        return std::nullopt;
    }
    if (config.verify_availability && choco_mgr) {
        exact->available = choco_mgr->isPackageAvailable(exact->choco_package);
    }
    if (config.verify_availability && !exact->available) {
        return std::nullopt;
    }
    m_exact_match_count++;
    return exact;
}

std::optional<PackageMatcher::MatchResult> PackageMatcher::tryFuzzyMatch(
    const QString& base_name, ChocolateyManager* choco_mgr, double min_confidence) {
    if (choco_mgr == nullptr) {
        return std::nullopt;
    }
    auto fuzzy = fuzzyMatch(base_name, choco_mgr);
    if (fuzzy.has_value() && fuzzy->confidence >= min_confidence) {
        m_fuzzy_match_count++;
        return fuzzy;
    }
    return std::nullopt;
}

std::optional<PackageMatcher::MatchResult> PackageMatcher::trySearchMatch(
    const QString& base_name, ChocolateyManager* choco_mgr, const MatchConfig& config) const {
    if (choco_mgr == nullptr) {
        return std::nullopt;
    }
    auto search = searchMatch(base_name, choco_mgr, config.max_search_results);
    if (search.has_value() && search->confidence >= config.min_confidence) {
        m_search_match_count++;
        return search;
    }
    return std::nullopt;
}

std::optional<PackageMatcher::MatchResult> PackageMatcher::findMatch(const AppScanner::AppInfo& app,
                                                                     ChocolateyManager* choco_mgr,
                                                                     const MatchConfig& config) {
    QString base_name = extractBaseAppName(app.name);

    if (config.use_exact_mappings) {
        auto exact = resolveExactMatch(base_name, choco_mgr, config);
        if (exact.has_value()) {
            return exact;
        }
    }

    if (config.use_fuzzy_matching) {
        auto fuzzy = tryFuzzyMatch(base_name, choco_mgr, config.min_confidence);
        if (fuzzy.has_value()) {
            return fuzzy;
        }
    }

    if (config.use_choco_search) {
        return trySearchMatch(base_name, choco_mgr, config);
    }

    return std::nullopt;
}

std::vector<PackageMatcher::MatchResult> PackageMatcher::findMatches(
    const std::vector<AppScanner::AppInfo>& apps,
    ChocolateyManager* choco_mgr,
    const MatchConfig& config) {
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
    const MatchConfig& config) {
    // Phase 1: Quick exact matches (no API calls needed)
    std::vector<std::pair<int, MatchResult>> exact_results;
    std::vector<std::pair<int, AppScanner::AppInfo>> fuzzy_candidates;
    collectExactMatches(apps, choco_mgr, config, exact_results, fuzzy_candidates);

    // Phase 2: Parallel fuzzy/search matching for remaining apps.
    // NOTE: the shared ChocolateyManager is touched from every worker; its own internal state
    // (m_choco_authentic, emitted signals) is not this class's to make thread-safe -- that
    // hardening belongs in ChocolateyManager and is tracked separately. Here we at least fail
    // closed on a pathological thread count: setMaxThreadCount(0) stalls blockingMapped and a
    // negative value is implementation-defined, so clamp to a minimum of one worker.
    QThreadPool pool;
    pool.setMaxThreadCount(std::max(1, config.thread_count));

    auto fuzzy_results =
        QtConcurrent::blockingMapped<std::vector<std::pair<int, std::optional<MatchResult>>>>(
            &pool,
            fuzzy_candidates,
            [this, choco_mgr, &config](const std::pair<int, AppScanner::AppInfo>& item) {
                return matchSingleApp(item.first, item.second, choco_mgr, config);
            });

    return mergeMatchResults(apps.size(), exact_results, fuzzy_results);
}

std::pair<int, std::optional<PackageMatcher::MatchResult>> PackageMatcher::matchSingleApp(
    int idx,
    const AppScanner::AppInfo& app,
    ChocolateyManager* choco_mgr,
    const MatchConfig& config) {
    QString base_name = extractBaseAppName(app.name);

    if (config.use_fuzzy_matching && choco_mgr) {
        auto fuzzy = fuzzyMatch(base_name, choco_mgr);
        if (fuzzy.has_value() && fuzzy->confidence >= config.min_confidence) {
            const QMutexLocker locker(&m_stats_mutex);
            m_fuzzy_match_count++;
            return {idx, fuzzy};
        }
    }

    if (config.use_choco_search && choco_mgr) {
        auto search = searchMatch(base_name, choco_mgr, config.max_search_results);
        if (search.has_value() && search->confidence >= config.min_confidence) {
            const QMutexLocker locker(&m_stats_mutex);
            m_search_match_count++;
            return {idx, search};
        }
    }

    return {idx, std::nullopt};
}

void PackageMatcher::collectExactMatches(
    const std::vector<AppScanner::AppInfo>& apps,
    ChocolateyManager* choco_mgr,
    const MatchConfig& config,
    std::vector<std::pair<int, MatchResult>>& exact_results,
    std::vector<std::pair<int, AppScanner::AppInfo>>& fuzzy_candidates) {
    const size_t app_count = apps.size();
    exact_results.reserve(app_count / kCandidateReserveDivisor);
    fuzzy_candidates.reserve(app_count / kCandidateReserveDivisor);

    // Route through resolveExactMatch (which honors use_exact_mappings, min_confidence, and
    // verify_availability and updates m_exact_match_count) so the parallel path matches findMatch
    // instead of admitting disabled/sub-threshold/unavailable exact hits. Phase 1 is single-
    // threaded (runs before the Phase 2 QThreadPool), so no stats lock is needed.
    for (size_t i = 0; i < app_count; ++i) {
        const QString base_name = extractBaseAppName(apps[i].name);
        std::optional<MatchResult> exact;
        if (config.use_exact_mappings) {
            exact = resolveExactMatch(base_name, choco_mgr, config);
        }
        if (exact.has_value()) {
            exact_results.push_back({static_cast<int>(i), *exact});
        } else {
            fuzzy_candidates.push_back({static_cast<int>(i), apps[i]});
        }
    }
}

std::vector<PackageMatcher::MatchResult> PackageMatcher::mergeMatchResults(
    size_t total_count,
    const std::vector<std::pair<int, MatchResult>>& exact_results,
    const std::vector<std::pair<int, std::optional<MatchResult>>>& fuzzy_results) {
    std::vector<MatchResult> all_results(total_count);
    std::vector<bool> has_match(total_count, false);

    for (const auto& [idx, result] : exact_results) {
        all_results[idx] = result;
        has_match[idx] = true;
    }

    for (const auto& [idx, result] : fuzzy_results) {
        if (result.has_value()) {
            all_results[idx] = *result;
            has_match[idx] = true;
        }
    }

    std::vector<MatchResult> final_results;
    for (size_t i = 0; i < total_count; ++i) {
        if (has_match[i]) {
            final_results.push_back(all_results[i]);
        }
    }

    return final_results;
}

std::optional<PackageMatcher::MatchResult> PackageMatcher::exactMatch(const QString& app_name) {
    const QString normalized = normalizeAppName(app_name);

    // Check direct mapping
    if (m_exact_mappings.contains(normalized)) {
        const QString choco_pkg = m_exact_mappings[normalized];
        return MatchResult{choco_pkg,
                           normalized,
                           1.0,   // Perfect confidence
                           "exact",
                           true,  // Assume available (will be verified if requested)
                           ""};
    }

    // Check case-insensitive
    for (auto it = m_exact_mappings.begin(); it != m_exact_mappings.end(); ++it) {
        if (it.key().compare(normalized, Qt::CaseInsensitive) == 0) {
            return MatchResult{
                it.value(), it.key(), kCaseInsensitiveExactConfidence, "exact", true, ""};
        }
    }

    return std::nullopt;
}

QString PackageMatcher::fetchSearchOutput(const QString& keyword, ChocolateyManager* choco_mgr) {
    // Private helper. Both callers of fuzzyMatch (tryFuzzyMatch, matchSingleApp) return early
    // on a null choco_mgr, and fuzzyMatch skips keywords shorter than kMinimumKeywordLength.
    Q_ASSERT(choco_mgr);
    Q_ASSERT(!keyword.isEmpty());
    QString cached = getCachedSearch(keyword);
    if (!cached.isEmpty()) {
        return cached;
    }
    auto result = choco_mgr->searchPackage(keyword, kChocoSearchResultLimit);
    if (!result.success) {
        return {};
    }
    cacheSearch(keyword, result.output);
    return result.output;
}

void PackageMatcher::updateBestFuzzyMatch(
    const QString& normalized,
    const std::vector<ChocolateyManager::PackageInfo>& packages,
    double& best_similarity,
    QString& best_package,
    QString& best_matched_name) const {
    for (const auto& pkg : packages) {
        const double similarity = calculateSimilarity(normalized, pkg.package_id);
        if (similarity <= best_similarity) {
            continue;
        }
        best_similarity = similarity;
        best_package = pkg.package_id;
        best_matched_name = pkg.title;
    }
}

std::optional<PackageMatcher::MatchResult> PackageMatcher::fuzzyMatch(
    const QString& app_name, ChocolateyManager* choco_mgr) {
    const QString normalized = normalizeAppName(app_name);
    const QStringList keywords = extractKeywords(app_name);

    double best_similarity = 0.0;
    QString best_package;
    QString best_matched_name;

    for (const QString& keyword : keywords) {
        if (keyword.length() < kMinimumKeywordLength) {
            continue;
        }

        const QString search_output = fetchSearchOutput(keyword, choco_mgr);
        if (search_output.isEmpty()) {
            continue;
        }

        auto packages = choco_mgr->parseSearchResults(search_output);
        updateBestFuzzyMatch(
            normalized, packages, best_similarity, best_package, best_matched_name);
    }

    if (best_similarity >= kFuzzyMatchThreshold) {
        return MatchResult{best_package, best_matched_name, best_similarity, "fuzzy", true, ""};
    }

    return std::nullopt;
}

std::optional<PackageMatcher::MatchResult> PackageMatcher::searchMatch(const QString& app_name,
                                                                       ChocolateyManager* choco_mgr,
                                                                       int max_results) const {
    const QString base_name = extractBaseAppName(app_name);
    const QStringList keywords = extractKeywords(base_name);

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
        const double title_score = calculateSimilarity(base_name, pkg.title);
        if (title_score > score) {
            score = title_score * kTitleMatchWeight;
        }

        if (score > best_score) {
            best_score = score;
            best_package = pkg;
        }
    }

    if (best_score >= kSearchMatchThreshold) {
        return MatchResult{best_package.package_id,
                           best_package.title,
                           best_score,
                           "search",
                           true,
                           best_package.version};
    }

    return std::nullopt;
}

QString PackageMatcher::normalizeAppName(const QString& app_name) {
    QString normalized = app_name;

    // Remove common suffixes
    const QStringList suffixes = {" (x64)",
                                  " (64-bit)",
                                  " (x86)",
                                  " (32-bit)",
                                  " (64 bit)",
                                  " (32 bit)",
                                  " (Remove only)",
                                  " for Windows",
                                  " Desktop",
                                  " Application"};

    for (const QString& suffix : suffixes) {
        if (normalized.endsWith(suffix, Qt::CaseInsensitive)) {
            normalized.chop(suffix.length());
        }
    }

    // Remove version numbers at the end
    const QRegularExpression versionRegex(R"(\s+v?\d+(\.\d+)*(\s+\w+)?$)");
    normalized.remove(versionRegex);

    return normalized.trimmed();
}

QString PackageMatcher::extractBaseAppName(const QString& app_name) {
    QString base = normalizeAppName(app_name);

    // Extract just the main app name (before first space or hyphen)
    QStringList words = base.split(QRegularExpression(R"([\s\-_]+)"), Qt::SkipEmptyParts);

    if (!words.isEmpty()) {
        // Return first 1-3 words as base name
        const int word_count = std::min(kBaseNameMaxWordCount, static_cast<int>(words.size()));
        QStringList base_words;
        for (int i = 0; i < word_count; ++i) {
            base_words.append(words[i]);
        }
        return base_words.join(" ");
    }

    return base;
}

QStringList PackageMatcher::extractKeywords(const QString& app_name) {
    const QString normalized = normalizeAppName(app_name);

    // Split into words
    const QStringList words = normalized.split(QRegularExpression(R"([\s\-_]+)"),
                                               Qt::SkipEmptyParts);

    // Remove common words
    const QStringList stopWords = {"the", "for", "and", "or", "software", "application", "app"};

    QStringList keywords;
    for (const QString& word : words) {
        const QString lower = word.toLower();
        if (!stopWords.contains(lower) && word.length() >= kMinimumKeywordLength) {
            keywords.append(word);
        }
    }

    return keywords;
}

double PackageMatcher::calculateSimilarity(const QString& str1, const QString& str2) const {
    const QString s1 = str1.toLower();
    const QString s2 = str2.toLower();

    // Exact match
    if (s1 == s2) {
        return 1.0;
    }

    // Contains match. By design this substring score (and the 0.5/0.6 thresholds) can admit a
    // loose match: the results are MIGRATION SUGGESTIONS surfaced to the user for review (selected
    // / available lists), never auto-installed, and install-time validatePackageName is the hard
    // gate. A low-confidence suggestion is therefore acceptable, not a fail-open.
    if (s1.contains(s2) || s2.contains(s1)) {
        return kContainedNameSimilarity;
    }

    // Jaro-Winkler similarity
    const double jw = jaroWinklerSimilarity(s1, s2);

    // Levenshtein-based similarity
    const int lev_dist = levenshteinDistance(s1, s2);
    const int max_len = std::max(s1.length(), s2.length());
    const double lev_sim = max_len > 0 ? 1.0 - (double(lev_dist) / max_len) : 0.0;

    // Average of both methods
    return (jw + lev_sim) / kSimilarityAverageDivisor;
}

int PackageMatcher::levenshteinDistance(const QString& s1, const QString& s2) const {
    const int len1 = s1.length();
    const int len2 = s2.length();

    // Bound the O(len1*len2) matrix. Registry DisplayNames / package ids are practically short, but
    // a pathological input must not allocate an enormous matrix. Past a sane cap, fail closed by
    // treating the pair as maximally distant (max_len distance -> similarity 0, never a false
    // match).
    if (len1 > kMaxLevenshteinLen || len2 > kMaxLevenshteinLen) {
        return std::max(len1, len2);
    }

    std::vector<std::vector<int>> d(len1 + 1, std::vector<int>(len2 + 1));

    for (int i = 0; i <= len1; ++i) {
        d[i][0] = i;
    }
    for (int j = 0; j <= len2; ++j) {
        d[0][j] = j;
    }

    for (int i = 1; i <= len1; ++i) {
        for (int j = 1; j <= len2; ++j) {
            const int cost = (s1[i - 1] == s2[j - 1]) ? 0 : 1;
            d[i][j] = std::min({
                d[i - 1][j] + 1,        // deletion
                d[i][j - 1] + 1,        // insertion
                d[i - 1][j - 1] + cost  // substitution
            });
        }
    }

    return d[len1][len2];
}

namespace {

int countJaroTranspositions(const QString& str1,
                            const QString& str2,
                            const std::vector<bool>& s1_matches,
                            const std::vector<bool>& s2_matches) {
    int transpositions = 0;
    int k_idx = 0;
    for (int idx = 0; idx < str1.length(); ++idx) {
        if (!s1_matches[idx]) {
            continue;
        }
        while (!s2_matches[k_idx]) {
            ++k_idx;
        }
        if (str1[idx] != str2[k_idx]) {
            ++transpositions;
        }
        ++k_idx;
    }
    return transpositions;
}

int countCommonPrefix(const QString& str1, const QString& str2) {
    constexpr int kMaxPrefixLen = 4;
    const int limit =
        std::min({static_cast<int>(str1.length()), static_cast<int>(str2.length()), kMaxPrefixLen});
    int prefix = 0;
    for (int idx = 0; idx < limit; ++idx) {
        if (str1[idx] != str2[idx]) {
            break;
        }
        ++prefix;
    }
    return prefix;
}

}  // namespace

double PackageMatcher::jaroWinklerSimilarity(const QString& s1, const QString& s2) {
    if (s1 == s2) {
        return 1.0;
    }

    const int len1 = s1.length();
    const int len2 = s2.length();
    if (len1 == 0 || len2 == 0) {
        return 0.0;
    }

    int match_distance = (std::max(len1, len2) / kJaroMatchWindowDivisor) - 1;
    match_distance = std::max(match_distance, 1);

    std::vector<bool> s1_matches(len1, false);
    std::vector<bool> s2_matches(len2, false);
    int matches = 0;

    for (int idx = 0; idx < len1; ++idx) {
        const int jdx = findJaroMatch(s1, s2, idx, match_distance, s2_matches);
        if (jdx < 0) {
            continue;
        }
        s1_matches[idx] = true;
        ++matches;
    }

    if (matches == 0) {
        return 0.0;
    }

    const int transpositions = countJaroTranspositions(s1, s2, s1_matches, s2_matches);

    constexpr double kJaroParts = 3.0;
    const double jaro = ((double(matches) / len1) + (double(matches) / len2) +
                         ((matches - (transpositions / 2.0)) / matches)) /
                        kJaroParts;

    constexpr double kWinklerScaling = 0.1;
    const int prefix = countCommonPrefix(s1, s2);
    return jaro + (prefix * kWinklerScaling * (1.0 - jaro));
}

void PackageMatcher::addMapping(const QString& app_name, const QString& choco_package) {
    const QString normalized = normalizeAppName(app_name);
    if (normalized.isEmpty() || choco_package.isEmpty()) {
        // Never store a blank mapping: it would surface as an empty suggestion and cannot map
        // anything. Matches importMappings(), which already skips blank entries. Fail closed.
        return;
    }
    m_exact_mappings[normalized] = choco_package;
}

void PackageMatcher::removeMapping(const QString& app_name) {
    const QString normalized = normalizeAppName(app_name);
    m_exact_mappings.remove(normalized);
}

bool PackageMatcher::hasMapping(const QString& app_name) const {
    const QString normalized = normalizeAppName(app_name);
    return m_exact_mappings.contains(normalized);
}

QString PackageMatcher::getMapping(const QString& app_name) const {
    const QString normalized = normalizeAppName(app_name);
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

bool PackageMatcher::exportMappings(const QString& file_path) const {
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

    // QSaveFile writes to a temp file and atomically renames on commit(), so a
    // crash or short write never truncates the user's existing mappings file.
    QSaveFile file(file_path);
    if (!file.open(QIODevice::WriteOnly)) {
        sak::logWarning("[PackageMatcher] Failed to open mappings file for write: {}",
                        file_path.toStdString());
        return false;
    }
    const QByteArray json_bytes = QJsonDocument(root).toJson(QJsonDocument::Indented);
    if (file.write(json_bytes) != json_bytes.size() || !file.commit()) {
        sak::logWarning("[PackageMatcher] Failed to write mappings file (original left intact): {}",
                        file_path.toStdString());
        return false;
    }
    return true;
}

bool PackageMatcher::importMappings(const QString& file_path) {
    QFile file(file_path);
    if (!file.open(QIODevice::ReadOnly)) {
        sak::logWarning("[PackageMatcher] Failed to open mappings file: {}",
                        file_path.toStdString());
        return false;
    }

    // Bound the read: a mappings file is a small hand-editable list. Refuse an implausibly large
    // file rather than readAll() it into memory unbounded (fail closed on a DoS-sized input).
    constexpr qint64 kMaxMappingsFileBytes = 8LL * 1024 * 1024;
    if (file.size() > kMaxMappingsFileBytes) {
        sak::logWarning("[PackageMatcher] Mappings file exceeds size cap, refusing to import: {}",
                        file_path.toStdString());
        return false;
    }

    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
    file.close();

    if (!doc.isObject()) {
        sak::logWarning("Package mappings file is not a valid JSON object: {}",
                        file_path.toStdString());
        return false;
    }

    QJsonObject root = doc.object();
    const QJsonArray mappings = root["mappings"].toArray();

    for (const QJsonValue& value : mappings) {
        QJsonObject mapping = value.toObject();
        const QString app_name = mapping["app_name"].toString();
        const QString choco_package = mapping["choco_package"].toString();

        if (!app_name.isEmpty() && !choco_package.isEmpty()) {
            m_exact_mappings[app_name] = choco_package;
        }
    }
    return true;
}

void PackageMatcher::clearCache() {
    const QMutexLocker locker(&m_cache_mutex);
    m_search_cache.clear();
}

QString PackageMatcher::getCachedSearch(const QString& keyword) const {
    const QMutexLocker locker(&m_cache_mutex);
    const QString* cached = m_search_cache.object(keyword);
    return (cached != nullptr) ? *cached : QString();
}

void PackageMatcher::cacheSearch(const QString& keyword, const QString& result) {
    const QMutexLocker locker(&m_cache_mutex);
    m_search_cache.insert(keyword, new QString(result), 1);
}

std::vector<QString> PackageMatcher::batchSearchChocolatey(const QStringList& keywords,
                                                           ChocolateyManager* choco_mgr) {
    // Preserve positional correspondence: results[i] MUST track keywords[i]. The old two-pass form
    // (cached hits first, then uncached) reordered the output, silently breaking any caller that
    // relies on result[i] matching keyword[i].
    std::vector<QString> results;
    results.reserve(keywords.size());

    for (const QString& keyword : keywords) {
        const QString cached = getCachedSearch(keyword);
        if (!cached.isEmpty()) {
            results.push_back(cached);
            continue;
        }
        auto result = choco_mgr->searchPackage(keyword, kChocoSearchResultLimit);
        if (result.success) {
            cacheSearch(keyword, result.output);
            results.push_back(result.output);
        } else {
            results.push_back(QString());
        }
    }

    return results;
}

}  // namespace sak
