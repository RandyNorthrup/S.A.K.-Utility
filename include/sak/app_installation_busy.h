// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file app_installation_busy.h
/// @brief Single authority for "is a package operation in flight" in the App
/// Installation panel.
///
/// The panel drives two paths that both mutate machine locations through
/// Chocolatey: the online install queue (AppInstallationWorker) and the offline
/// deployment path (OfflineDeploymentWorker: build bundle, install from bundle,
/// direct download). They are mutually exclusive -- two concurrent choco runs
/// fight over the same package store and install root -- so they must be gated by
/// ONE predicate. Two independent flags could disagree, which is exactly how an
/// offline bundle install could start on top of a running online install.

#pragma once

namespace sak {

/// @brief Every source of in-flight package work the panel owns, sampled at one instant.
struct PackageOperationActivity {
    /// The panel has claimed the ONLINE path. Set the moment the install is dispatched, so
    /// it covers the window before the worker thread reports itself running.
    bool online_claimed{false};
    /// The online install worker thread is running.
    bool online_worker_running{false};
    /// The panel has claimed the OFFLINE path (bundle build/install, direct download).
    bool offline_claimed{false};
    /// The offline deployment worker thread is running.
    bool offline_worker_running{false};
};

/// @brief True when either path has package work in flight. Both entry-point gating and
/// control enablement read this one predicate, so they can never disagree.
[[nodiscard]] bool packageOperationInFlight(const PackageOperationActivity& activity) noexcept;

}  // namespace sak
