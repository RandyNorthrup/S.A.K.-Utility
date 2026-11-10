// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/contained_process_quarantine.h"

#include "sak/logger.h"

#include <QMutex>
#include <QMutexLocker>

#include <iterator>
#include <utility>
#include <vector>

namespace sak {

namespace {

struct RetainedOwner {
    std::unique_ptr<ContainedProcessQuarantine::Owner> owner;
    QString reason;
};

struct Custody {
    QMutex mutex;
    std::vector<RetainedOwner> owners;
};

// Deliberately never destroyed. A static object's destructor would run at exit and destroy every
// owner whose cleanup is unproven -- the exact live QProcess/Job/thread teardown custody exists to
// prevent. Leaving it to the OS is what keeps each retained Job handle open until process exit.
Custody& custody() {
    static auto* const instance = new Custody;
    return *instance;
}

}  // namespace

void ContainedProcessQuarantine::retain(std::unique_ptr<Owner> owner, const QString& reason) {
    Q_ASSERT(owner);
    if (!owner) {
        sak::logError("Contained process custody was handed no owner for: {}",
                      reason.toStdString());
        return;
    }
    sak::logError(
        "Contained process cleanup unproven; owner retained for process lifetime with its "
        "kill-on-close Job armed: {}",
        reason.toStdString());
    Custody& registry = custody();
    const QMutexLocker lock(&registry.mutex);
    registry.owners.push_back(RetainedOwner{std::move(owner), reason});
}

int ContainedProcessQuarantine::retainedCount() {
    Custody& registry = custody();
    const QMutexLocker lock(&registry.mutex);
    return static_cast<int>(registry.owners.size());
}

QStringList ContainedProcessQuarantine::retainedReasons() {
    Custody& registry = custody();
    const QMutexLocker lock(&registry.mutex);
    QStringList reasons;
    reasons.reserve(static_cast<qsizetype>(registry.owners.size()));
    for (const RetainedOwner& retained : registry.owners) {
        reasons.append(retained.reason);
    }
    return reasons;
}

std::expected<void, QString> ContainedProcessQuarantine::retryRetained() {
    Custody& registry = custody();
    std::vector<RetainedOwner> attempted;
    {
        // Moving the owners out gives this call exclusive use of each one, so no owner is ever
        // retried from two threads at once, and a bounded retry never blocks retain().
        const QMutexLocker lock(&registry.mutex);
        attempted.swap(registry.owners);
    }
    std::vector<std::unique_ptr<Owner>> proved;
    std::vector<RetainedOwner> unproven;
    QStringList errors;
    for (RetainedOwner& retained : attempted) {
        const auto cleaned = retained.owner->retryCleanup();
        if (cleaned) {
            proved.push_back(std::move(retained.owner));
            continue;
        }
        retained.reason = cleaned.error();
        errors.append(cleaned.error());
        unproven.push_back(std::move(retained));
    }
    {
        // Owners retained while this retry ran stay behind the older ones that were retried.
        const QMutexLocker lock(&registry.mutex);
        registry.owners.insert(registry.owners.begin(),
                               std::make_move_iterator(unproven.begin()),
                               std::make_move_iterator(unproven.end()));
    }
    // Proved owners are destroyed here, outside the lock: an owner's destructor may re-enter
    // retain() if it unexpectedly finds more to contain.
    proved.clear();
    if (!errors.isEmpty()) {
        return std::unexpected(errors.join(QStringLiteral("; ")));
    }
    return {};
}

}  // namespace sak
