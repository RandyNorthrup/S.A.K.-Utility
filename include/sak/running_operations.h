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
    /// @p cannot_stop_note, when given, returns a note while part of the operation is work a
    /// cancel cannot stop -- a full format Windows finishes on its own -- and an empty string
    /// otherwise; the close prompt shows it instead of promising to stop that work.
    static void add(QObject* owner,
                    const QString& name,
                    std::function<bool()> is_running,
                    std::function<void()> request_cancel,
                    std::function<QString()> cannot_stop_note = {});

    /// Names of the registered operations running now, in registration order, without repeats,
    /// leaving out those @p except owns.
    [[nodiscard]] static QStringList runningNames(const QObject* except = nullptr);

    /// The notes of the running operations that are now in work a cancel cannot stop, in
    /// registration order, without repeats, leaving out those @p except owns. Empty when
    /// cancelling would stop all of it.
    [[nodiscard]] static QStringList cannotStopNotes(const QObject* except = nullptr);

    /// An owner says what its probes report has changed -- a note came or went -- or, with
    /// @p finished_owner, that its operations have just ended and are about to report their
    /// result, while their probes may still say running for the length of that report. The close
    /// prompt, while it is open, follows: it never asks about work whose result is on screen.
    static void notifyChanged(const QObject* finished_owner = nullptr);

    /// The one listener notifyChanged reaches: the close prompt, while it is open. An empty
    /// function clears it.
    static void setChangeListener(std::function<void(const QObject* finished_owner)> listener);

    /// Requests cooperative cancellation of every registered operation that is running now.
    static void cancelAllRunning();
};

}  // namespace sak
