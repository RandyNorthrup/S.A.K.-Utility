// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <QString>
#include <QStringList>

#include <functional>

class QObject;

/// @file running_operations.h
/// @brief The application's one list of long operations that are running right now.
namespace sak {

/// Lets closing the window, or relaunching as administrator, find out what is running and ask
/// before it cancels it (owner decision 2026-10-24: ask before exit when work is running, and
/// cancel it cooperatively).
///
/// A panel registers each kind of long operation it owns ONCE, with a probe that says whether
/// that operation is running now and a request that cancels it cooperatively. Nothing is begun or
/// ended per run: the probe reads the panel's own live state, so a terminal path that forgets to
/// report back cannot leave a stale entry behind, and an entry disappears with the object that
/// registered it.
///
/// Register an operation whose interruption the user would notice -- one that changes the
/// machine or produces output the user is waiting for. A read-only scan that closing simply
/// abandons is not registered; its worker still stops cooperatively when its owner is destroyed.
///
/// GUI thread only. Probes and cancel requests run on the GUI thread and must not block.
class RunningOperations final {
public:
    RunningOperations() = delete;

    /// Registers one kind of long operation owned by @p owner, shown to the user as @p name.
    /// @p is_running says whether it is running now; @p request_cancel asks it to stop
    /// cooperatively and returns without waiting. The entry lives as long as @p owner.
    static void add(QObject* owner,
                    const QString& name,
                    std::function<bool()> is_running,
                    std::function<void()> request_cancel);

    /// Names of the registered operations running now, in registration order, without repeats.
    [[nodiscard]] static QStringList runningNames();

    /// Requests cooperative cancellation of every registered operation that is running now.
    static void cancelAllRunning();
};

}  // namespace sak
