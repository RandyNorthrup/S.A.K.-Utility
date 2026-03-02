// Copyright (c) 2025-2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file restore_point_manager.h
/// @brief System Restore point creation and availability checking

#pragma once

#include <QDateTime>
#include <QObject>
#include <QPair>
#include <QString>
#include <QVector>

#include <type_traits>

namespace sak {

/// @brief Manages Windows System Restore points via PowerShell
///
/// Checks if System Restore is enabled, creates restore points before
/// uninstallation, and lists existing restore points. Requires elevation
/// for restore point creation.
class RestorePointManager : public QObject {
    Q_OBJECT

public:
    explicit RestorePointManager(QObject* parent = nullptr);
    ~RestorePointManager() override = default;

    RestorePointManager(const RestorePointManager&) = delete;
    RestorePointManager& operator=(const RestorePointManager&) = delete;
    RestorePointManager(RestorePointManager&&) = delete;
    RestorePointManager& operator=(RestorePointManager&&) = delete;

    /// @brief Check if System Restore is enabled on the system drive
    [[nodiscard]] bool isSystemRestoreEnabled() const;

    /// @brief Create a system restore point
    /// @param description Description for the restore point (max 64 chars)
    /// @return true if created successfully
    [[nodiscard]] bool createRestorePoint(const QString& description);

    /// @brief Get list of existing restore points
    [[nodiscard]] QVector<QPair<QDateTime, QString>> listRestorePoints() const;

    /// @brief Check if the current process has administrative privileges
    [[nodiscard]] static bool isElevated();

Q_SIGNALS:
    void restorePointCreated(const QString& description);
    void restorePointFailed(const QString& error);

private:
    static constexpr int kMaxDescriptionLength = 64;
    static constexpr int kCreateTimeoutMs = 120000;  ///< 2 min timeout
    static constexpr int kCheckTimeoutMs = 10000;    ///< 10 sec timeout
};

// ── Compile-Time Invariants ─────────────────────────────────────────────────

static_assert(std::is_base_of_v<QObject, RestorePointManager>,
    "RestorePointManager must inherit QObject.");
static_assert(!std::is_copy_constructible_v<RestorePointManager>,
    "RestorePointManager must not be copy-constructible.");

} // namespace sak
