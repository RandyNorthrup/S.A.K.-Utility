// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

/// @file elevation_gate.h
/// @brief Reusable UI gate for features that require administrator privileges
///
/// Features that need the whole process elevated offer to restart the app as administrator,
/// through the same routine as the status bar's Run as Admin (requestElevatedRestart).

#include "sak/elevation_manager.h"
#include "sak/logger.h"
#include "sak/message_box_helpers.h"
#include "sak/running_work_exit_guard.h"

#include <QCoreApplication>
#include <QMessageBox>
#include <QPointer>
#include <QString>
#include <QWidget>

#include <optional>

namespace sak {

/// @brief Result of an elevation gate check
enum class ElevationGateResult : uint8_t {
    AlreadyElevated,   ///< Process is already admin -- proceed normally
    RestartRequested,  ///< User accepted restart as admin -- caller should return
    Declined           ///< User declined elevation -- caller should abort or degrade
};

/// @brief Return the dialog-free gate result when elevation already satisfies the request.
///
/// A non-elevated state returns std::nullopt, meaning the interactive question must run. Keeping
/// this decision pure lets both process-token arms be verified without launching an elevated test.
[[nodiscard]] inline std::optional<ElevationGateResult> elevationGatePreflight(
    bool is_elevated) noexcept {
    if (is_elevated) {
        return ElevationGateResult::AlreadyElevated;
    }
    return std::nullopt;
}

/// @brief Relaunch the app as administrator now; this instance quits once the elevated copy has
///        started. A declined UAC prompt is the user's answer: nothing happens and nothing is
///        shown. Any other failure is reported. Returns true when the relaunch started.
inline bool relaunchElevated(QWidget* parent) {
    const auto result = ElevationManager::restartElevated();
    if (result) {
        QCoreApplication::quit();
        return true;
    }
    if (!ElevationManager::restartFailureNeedsNotice(result.error())) {
        return false;
    }
    sak::logError("Failed to restart elevated: {}", to_string(result.error()));
    sak::showCriticalLogged(parent,
                            QObject::tr("Elevation Failed"),
                            QObject::tr("Failed to restart with administrator privileges.\n\n"
                                        "Try right-clicking the application and selecting "
                                        "\"Run as administrator\"."));
    return false;
}

/// @brief The one way to restart as administrator -- the status bar's Run as Admin and every
///        elevation gate. Running work is asked about first, exactly as closing does, through the
///        main window's RunningWorkExitGuard (found up @p parent's widget chain); the relaunch
///        happens now, or once that work has stopped.
/// @return RestartRequested when the relaunch started or waits for running work to stop;
///         Declined when it was declined, refused or failed.
inline ElevationGateResult requestElevatedRestart(QWidget* parent) {
    RunningWorkExitGuard* guard = nullptr;
    for (QWidget* widget = parent; widget != nullptr && guard == nullptr;
         widget = widget->parentWidget()) {
        guard = widget->findChild<RunningWorkExitGuard*>(QString(), Qt::FindDirectChildrenOnly);
    }
    if (guard == nullptr) {
        // A caller outside the main window: relaunching without asking about running work could
        // leave this instance running beside the elevated one, so refuse visibly.
        sak::logError("Elevated restart requested outside the main window; refused");
        sak::showCriticalLogged(parent,
                                QObject::tr("Elevation Failed"),
                                QObject::tr("Use Run as Admin in the status bar to restart S.A.K. "
                                            "Utility as administrator."));
        return ElevationGateResult::Declined;
    }
    const QPointer<QWidget> deferred_parent(parent);
    if (guard->proceedNowOrAfterCancelling(RunningWorkExitGuard::Intent::RestartElevated,
                                           [deferred_parent]() {
                                               relaunchElevated(deferred_parent.data());
                                           })) {
        return relaunchElevated(parent) ? ElevationGateResult::RestartRequested
                                        : ElevationGateResult::Declined;
    }
    return guard->isWaitingForCancellation() ? ElevationGateResult::RestartRequested
                                             : ElevationGateResult::Declined;
}

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
/// @return Gate result -- caller decides how to proceed
[[nodiscard]] inline ElevationGateResult showElevationGate(QWidget* parent,
                                                           const QString& feature,
                                                           const QString& reason) {
    if (const auto preflight = elevationGatePreflight(ElevationManager::isElevated())) {
        return *preflight;
    }

    sak::logInfo("Elevation gate triggered for: {}", feature.toStdString());

    auto response = sak::showQuestionLogged(
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
    return requestElevatedRestart(parent);
}

}  // namespace sak
