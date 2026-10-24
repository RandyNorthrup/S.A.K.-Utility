// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <QString>
#include <QStringList>

#include <expected>
#include <memory>

/// @file contained_process_quarantine.h
/// @brief Process-lifetime custody for owners whose contained-process cleanup is unproven.
namespace sak {

/// Holds owners of Job-contained child processes whose cleanup could not be proved.
///
/// A destructor that cannot prove its kill-on-close Windows Job empty has no safe local outcome:
/// destroying the live QProcess, Job and worker thread is a use-after-free (or a second fail-fast
/// inside WindowsProcessJob), and crashing takes the whole application down because one child
/// outlived a cleanup deadline under load. Such an owner reports the diagnostic and transfers its
/// ownership here instead (owner decision 2026-10-24, "report + contain").
///
/// Custody is fail-closed. A retained owner is destroyed only after its own cleanup retry proves
/// the contained processes gone, and the custody registry itself is never destroyed. An owner
/// still retained when the application exits keeps its Job handle open until the OS tears the
/// process down; closing that last handle fires JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE, so a contained
/// child can never outlive the application.
class ContainedProcessQuarantine final {
public:
    /// One owner in custody. retryCleanup() makes one bounded cleanup attempt; success proves the
    /// owner's contained processes are gone, after which custody destroys it.
    class Owner {
    public:
        Owner() = default;
        Owner(const Owner&) = delete;
        Owner& operator=(const Owner&) = delete;
        Owner(Owner&&) = delete;
        Owner& operator=(Owner&&) = delete;
        virtual ~Owner() = default;

        [[nodiscard]] virtual std::expected<void, QString> retryCleanup() = 0;
    };

    ContainedProcessQuarantine() = delete;

    /// Takes custody of @p owner and logs @p reason, the cleanup diagnostic that could not be
    /// proved. Callable from any thread.
    static void retain(std::unique_ptr<Owner> owner, const QString& reason);

    /// Number of owners currently in custody.
    [[nodiscard]] static int retainedCount();

    /// The latest unproven-cleanup diagnostic of each owner in custody, oldest first.
    [[nodiscard]] static QStringList retainedReasons();

    /// Retries every owner in custody once, outside the registry lock, and destroys only those
    /// whose retry proved cleanup. Returns the joined diagnostics of owners still in custody.
    [[nodiscard]] static std::expected<void, QString> retryRetained();
};

}  // namespace sak
