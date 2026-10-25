// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file running_operations.cpp
/// @brief Registry of long operations that closing or relaunching must ask about first.

#include "sak/running_operations.h"

#include "sak/logger.h"

#include <QCoreApplication>
#include <QObject>
#include <QPointer>
#include <QThread>

#include <utility>
#include <vector>

namespace sak {

namespace {

struct Registration {
    QPointer<QObject> owner;
    QString name;
    std::function<bool()> is_running;
    std::function<void()> request_cancel;
};

// Deliberately never destroyed: entries capture their owners, and a static destructor running
// after those owners is not a moment to touch them.
std::vector<Registration>& registrations() {
    static auto* const instance = new std::vector<Registration>;
    return *instance;
}

bool onGuiThread() {
    const QCoreApplication* const application = QCoreApplication::instance();
    return application == nullptr || QThread::currentThread() == application->thread();
}

// Drops entries whose owner is gone, so a probe never runs against a destroyed panel.
void pruneDeadOwners() {
    std::erase_if(registrations(),
                  [](const Registration& registration) { return registration.owner.isNull(); });
}

// Copies of the live registrations: a probe or cancel request may itself register or destroy
// something, which must not invalidate the iteration.
std::vector<Registration> liveRegistrations() {
    pruneDeadOwners();
    return registrations();
}

}  // namespace

void RunningOperations::add(QObject* owner,
                            const QString& name,
                            std::function<bool()> is_running,
                            std::function<void()> request_cancel) {
    Q_ASSERT(onGuiThread());
    if (owner == nullptr || !is_running || !request_cancel || name.trimmed().isEmpty()) {
        logError("Refused an incomplete running-operation registration: '{}'", name.toStdString());
        return;
    }
    pruneDeadOwners();
    registrations().push_back(Registration{.owner = owner,
                                           .name = name,
                                           .is_running = std::move(is_running),
                                           .request_cancel = std::move(request_cancel)});
}

QStringList RunningOperations::runningNames() {
    Q_ASSERT(onGuiThread());
    QStringList names;
    // liveRegistrations() just dropped every entry whose owner is gone, and a probe only reads
    // state, so every owner here is alive for its probe.
    for (const Registration& registration : liveRegistrations()) {
        if (registration.is_running() && !names.contains(registration.name)) {
            names.append(registration.name);
        }
    }
    return names;
}

void RunningOperations::cancelAllRunning() {
    Q_ASSERT(onGuiThread());
    for (const Registration& registration : liveRegistrations()) {
        // Re-checked per entry: an earlier cancel request may have destroyed a later owner.
        if (!registration.owner.isNull() && registration.is_running()) {
            logInfo("Cancelling running operation before exit: {}",
                    registration.name.toStdString());
            registration.request_cancel();
        }
    }
}

}  // namespace sak
