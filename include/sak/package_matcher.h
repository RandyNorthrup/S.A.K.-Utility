// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "sak/app_scanner.h"

#include <QMap>
#include <QString>

#include <optional>
#include <vector>

class PackageMatcherTests;  // unit-test friend (global namespace)

namespace sak {

/**
 * @brief PackageMatcher - Matches installed applications to Chocolatey packages
 *
 * Matches an application's registry display name against a reviewed table of well-known
 * applications, once the build detail (version, architecture, locale) has been taken off the
 * name. An application the table does not know has no match.
 *
 * It makes no network calls on purpose. `choco search` returns results alphabetically rather than
 * by relevance, takes about two seconds per query, and the community feed answers excess traffic
 * with an hour of 429s. A scan of the hundreds of entries a real PC lists would take many minutes
 * and could cost the technician the install they came for. Whether a package installs is answered
 * by the install itself, and the restore page reports each result.
 */
class PackageMatcher {
    friend class ::PackageMatcherTests;

public:
    PackageMatcher();
    ~PackageMatcher() = default;

    /**
     * @brief Match result with confidence score
     */
    struct MatchResult {
        QString choco_package;   // Chocolatey package name
        QString matched_name;    // Name that was matched
        double confidence{0.0};  // 0.0 - 1.0 (1.0 = perfect match)
        QString match_type;      // "exact", "manual"
        bool available{false};   // Is package available in Chocolatey?
        QString version;         // Latest available version
    };

    /// @brief Find the package for one application; nullopt when the table does not know it.
    [[nodiscard]] std::optional<MatchResult> findMatch(const AppScanner::AppInfo& app) const;

    /// @brief Fill each application's Chocolatey fields where a real match exists: desktop
    ///        applications from the table, and packages already reported by the bundled
    ///        Chocolatey keep their own id. Store (AppX) applications are left unmatched because
    ///        Chocolatey does not install them.
    void applyMatches(std::vector<AppScanner::AppInfo>& apps) const;

    // Manual mapping management
    void addMapping(const QString& app_name, const QString& choco_package);

    int getMappingCount() const;

private:
    std::optional<MatchResult> exactMatch(const QString& app_name) const;

    /// @brief The product name from a registry display name: "7-Zip 22.01 (x64)" -> "7-Zip".
    static QString normalizeAppName(const QString& app_name);

    void initializeCommonMappings();

    QMap<QString, QString> m_exact_mappings;  // normalized app_name -> choco_package
};

}  // namespace sak
