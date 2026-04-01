// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

/// @file elevation_gate.h
/// @brief Reusable UI gate for features that require administrator privileges
///
/// Phase 1 approach: offer to restart the whole app as admin.
/// Phase 2 will replace these gates with the elevated helper broker.

#include "sak/elevation_manager.h"
#include "sak/logger.h"

#include <QApplication>
#include <QMessageBox>
#include <QString>
#include <QWidget>

namespace sak {

/// @brief Result of an elevation gate check
enum class ElevationGateResult : uint8_t {
    AlreadyElevated,   ///< Process is already admin — proceed normally
    RestartRequested,  ///< User accepted restart as admin — caller should return
    Declined           ///< User declined elevation — caller should abort or degrade
};

/// @brief Check elevation and show a restart-as-admin dialog if needed
///
/// Call this at the top of any Tier 2 (Elevated) feature handler.
/// If the process is already elevated, returns AlreadyElevated immediately.
/// Otherwise, shows a dialog explaining why admin is needed and offers to
/// restart the application with elevation.
///
/// @param parent  Parent widget for the dialog
/// @param feature Human-readable feature name (e.g. "USB Flash")
/// @param reason  Why elevation is needed (shown in dialog body)
/// @return Gate result — caller decides how to proceed
[[nodiscard]] inline ElevationGateResult showElevationGate(QWidget* parent,
                                                           const QString& feature,
                                                           const QString& reason) {
    if (ElevationManager::isElevated()) {
        return ElevationGateResult::AlreadyElevated;
    }

    sak::logInfo("Elevation gate triggered for: {}", feature.toStdString());

    auto response = QMessageBox::question(
        parent,
        QObject::tr("Administrator Required"),
        QObject::tr("%1 requires administrator privileges.\n\n"
                    "%2\n\n"
                    "Would you like to restart S.A.K. Utility as administrator?")
            .arg(feature, reason),
        QMessageBox::Yes | QMessageBox::No);

    if (response != QMessageBox::Yes) {
        sak::logInfo("User declined elevation for: {}", feature.toStdString());
        return ElevationGateResult::Declined;
    }

    sak::logInfo("User accepted elevation restart for: {}", feature.toStdString());
    auto result = ElevationManager::restartElevated();
    if (result) {
        QApplication::quit();
    } else {
        sak::logError("Failed to restart elevated: {}", to_string(result.error()));
        QMessageBox::critical(parent,
                              QObject::tr("Elevation Failed"),
                              QObject::tr("Failed to restart with administrator privileges.\n\n"
                                          "Try right-clicking the application and selecting "
                                          "\"Run as administrator\"."));
    }
    return ElevationGateResult::RestartRequested;
}

}  // namespace sak
