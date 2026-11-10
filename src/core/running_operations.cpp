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
    std::function<QString()> cannot_stop_note;
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

std::function<void(const QObject*)>& changeListener() {
    static auto* const listener = new std::function<void(const QObject*)>;
    return *listener;
}

}  // namespace

void RunningOperations::add(QObject* owner,
                            const QString& name,
                            std::function<bool()> is_running,
                            std::function<void()> request_cancel,
                            std::function<QString()> cannot_stop_note) {
    Q_ASSERT(onGuiThread());
    if (owner == nullptr || !is_running || !request_cancel || name.trimmed().isEmpty()) {
        logError("Refused an incomplete running-operation registration: '{}'", name.toStdString());
        return;
    }
    pruneDeadOwners();
    registrations().push_back(Registration{.owner = owner,
                                           .name = name,
                                           .is_running = std::move(is_running),
                                           .request_cancel = std::move(request_cancel),
                                           .cannot_stop_note = std::move(cannot_stop_note)});
}

QStringList RunningOperations::runningNames(const QObject* except) {
    Q_ASSERT(onGuiThread());
    QStringList names;
    // liveRegistrations() just dropped every entry whose owner is gone, and a probe only reads
    // state, so every owner here is alive for its probe.
    for (const Registration& registration : liveRegistrations()) {
        if (registration.owner.data() != except && registration.is_running() &&
            !names.contains(registration.name)) {
            names.append(registration.name);
        }
    }
    return names;
}

QStringList RunningOperations::cannotStopNotes(const QObject* except) {
    Q_ASSERT(onGuiThread());
    QStringList notes;
    for (const Registration& registration : liveRegistrations()) {
        if (registration.owner.data() == except || !registration.cannot_stop_note ||
            !registration.is_running()) {
            continue;
        }
        const QString note = registration.cannot_stop_note();
        if (!note.isEmpty() && !notes.contains(note)) {
            notes.append(note);
        }
    }
    return notes;
}

void RunningOperations::notifyChanged(const QObject* finished_owner) {
    Q_ASSERT(onGuiThread());
    // A copy: the listener may close the prompt, which clears the listener while it runs.
    const std::function<void(const QObject*)> listener = changeListener();
    if (listener) {
        listener(finished_owner);
    }
}

void RunningOperations::setChangeListener(
    std::function<void(const QObject* finished_owner)> listener) {
    Q_ASSERT(onGuiThread());
    changeListener() = std::move(listener);
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
