// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <QString>
#include <QStringList>

#include <functional>
#include <memory>

class QThread;

/// @file thread_custody.h
/// @brief Process-lifetime custody for a thread that outlived its cooperative stop deadline.
namespace sak {

/// Holds a thread that did not stop within its bounded cooperative deadline, together with every
/// object that thread still uses, until the thread finishes.
///
/// An owner tearing down a worker has three bad options once a cooperative stop expires:
/// QThread::terminate() kills the thread wherever it is -- mid-write, holding a lock, half way
/// through a file -- so it is never used; destroying the QThread (or the state it touches) while
/// it runs is a use-after-free, and Qt itself aborts on a running QThread's destruction; and an
/// unbounded wait freezes whichever thread is tearing down, usually the GUI. Custody is the fourth
/// option (owner decision 2026-10-24, "report + contain"): the owner reports the overrun and
/// hands the thread and its state here instead.
///
/// The state is destroyed on the application thread once the thread finishes. If the application
/// stops processing events first, the state is simply kept: process exit ends the thread, which
/// is the documented end state. The custody registry itself is never destroyed, for the same
/// reason sak::ContainedProcessQuarantine's is not: a static destructor would destroy state a
/// still-running thread is using.
///
/// This is the thread counterpart of sak::ContainedProcessQuarantine, which keeps a child
/// process's kill-on-close Job open instead; a thread has no such handle to keep armed, only
/// memory that must stay alive.
class ThreadCustody final {
public:
    ThreadCustody() = delete;

    /// Takes ownership of @p state -- whatever @p thread still uses, which may be the QThread
    /// object itself -- and logs @p reason as an error. @p thread must stay valid for as long as
    /// @p state lives. Call it on the thread that @p thread's QThread object belongs to.
    static void retain(QThread* thread, std::shared_ptr<void> state, const QString& reason);

    /// Stops and destroys a thread object its owner no longer needs -- a QThread subclass that
    /// owns everything its run() uses -- without ever terminating it. A thread that is not
    /// running is destroyed at once. A running one is asked to stop through @p request_stop and
    /// given @p stop_wait_ms; still running after that, the overrun is reported and the object
    /// is retained here until it finishes. Its signals are disconnected first, so a retained
    /// thread can no longer reach the owner that gave it up. Call it on the thread the object
    /// belongs to. Returns true when the object was destroyed, false when it was retained.
    static bool release(std::unique_ptr<QThread> thread,
                        const std::function<void()>& request_stop,
                        int stop_wait_ms) noexcept;

    /// The destructor-side half of release(), for a thread object destroying itself after it
    /// requested its own stop: joins @p thread within @p stop_wait_ms. A destructor cannot keep
    /// its own object alive, so a thread still running past the bound -- one an owner destroyed
    /// directly instead of releasing it -- is a programming error: returning would run the thread
    /// into freed members, and Qt aborts on destroying a running QThread anyway. That stops the
    /// process with the real reason; it is never answered with terminate().
    static void joinOrFailInDestructor(QThread& thread, int stop_wait_ms) noexcept;

    /// Number of threads currently in custody.
    [[nodiscard]] static int retainedCount();

    /// The diagnostic each thread in custody was retained with, oldest first.
    [[nodiscard]] static QStringList retainedReasons();
};

}  // namespace sak
