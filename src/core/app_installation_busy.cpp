// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/app_installation_busy.h"

namespace sak {

bool packageOperationInFlight(const PackageOperationActivity& activity) noexcept {
    // Deliberately ORs BOTH paths: an online install and an offline bundle install both
    // drive Chocolatey against the same machine locations, so either one in flight makes
    // the panel busy for the other. Checking only your own path is what allowed the two
    // to run concurrently.
    return activity.online_claimed || activity.online_worker_running || activity.offline_claimed ||
           activity.offline_worker_running;
}

}  // namespace sak
