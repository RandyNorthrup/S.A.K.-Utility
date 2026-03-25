// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file package_list_manager.h
/// @brief Curated package list management for offline deployment
///
/// Manages named lists of Chocolatey packages that can be saved, loaded,
/// and used as the source for offline deployment bundles. Includes built-in
/// preset lists for common deployment scenarios (Office PC, Developer
/// Workstation, Kiosk, etc.).

#pragma once

#include <QString>
#include <QStringList>
#include <QVector>

namespace sak {

/// @brief A single entry in a curated package list
struct PackageListEntry {
    QString package_id;
    QString version;
    QString notes;
    bool pinned{false};
};

/// @brief A named, saveable collection of packages
struct PackageList {
    QString name;
    QString description;
    QString created_date;
    QString modified_date;
    QVector<PackageListEntry> entries;
};

/// @brief Manages curated package lists for offline deployment workflows
///
/// Supports:
///  - Built-in preset lists for common scenarios
///  - Custom user-created lists
///  - Save/load to JSON files
///  - Merge, deduplicate, and validate operations
class PackageListManager {
public:
    PackageListManager() = default;

    /// @brief Get names of all built-in preset lists
    [[nodiscard]] QStringList presetNames() const;

    /// @brief Get a built-in preset list by name
    [[nodiscard]] PackageList preset(const QString& name) const;

    /// @brief Create a new empty package list
    [[nodiscard]] static PackageList createList(const QString& name, const QString& description);

    /// @brief Add a package to a list (skips duplicates)
    /// @return true if added, false if duplicate
    static bool addPackage(PackageList& list,
                           const QString& package_id,
                           const QString& version = QString(),
                           const QString& notes = QString());

    /// @brief Remove a package from a list by ID
    /// @return true if removed, false if not found
    static bool removePackage(PackageList& list, const QString& package_id);

    /// @brief Merge source list into target (skips duplicates)
    /// @return Number of new entries added
    static int mergeLists(PackageList& target, const PackageList& source);

    /// @brief Save a package list to a JSON file
    [[nodiscard]] static bool saveToFile(const PackageList& list, const QString& file_path);

    /// @brief Load a package list from a JSON file
    [[nodiscard]] static PackageList loadFromFile(const QString& file_path);

    /// @brief Convert a list to (package_id, version) pairs for the worker
    [[nodiscard]] static QVector<QPair<QString, QString>> toPackagePairs(const PackageList& list);

private:
    /// @brief Build the Office PC preset
    [[nodiscard]] PackageList buildOfficePreset() const;

    /// @brief Build the Developer Workstation preset
    [[nodiscard]] PackageList buildDeveloperPreset() const;

    /// @brief Build the Kiosk/POS preset
    [[nodiscard]] PackageList buildKioskPreset() const;

    /// @brief Build the Security/IT Admin preset
    [[nodiscard]] PackageList buildSecurityPreset() const;

    /// @brief Build the Education Lab preset
    [[nodiscard]] PackageList buildEducationPreset() const;
};

}  // namespace sak
