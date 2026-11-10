// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file single_instance_guard.h
/// @brief One running S.A.K. Utility per portable data folder.

#pragma once

#include <QObject>
#include <QString>

#include <chrono>
#include <expected>
#include <memory>

class QWinEventNotifier;

namespace sak {

/// How long an elevated relaunch waits for the instance that launched it to finish closing.
inline constexpr std::chrono::seconds kElevatedRelaunchHandoffLimit{60};

/// Keeps one S.A.K. Utility running per data folder. Two instances on one folder share
/// data/config, the logs and the AI sessions; a second launch instead brings the running
/// window to the front and exits. Two portable copies in different folders are different
/// instances and run side by side.
///
/// Two named objects in the session namespace, both named after the data folder's final path
/// (junctions and letter case resolved, then hashed): a mutex the running instance owns, and an
/// auto-reset event a second launch sets to ask it to come forward. Both carry a security
/// descriptor that lets this user open them from any integrity level (an elevated instance and
/// an unelevated launch, either way round) and nobody else.
///
/// The mutex is never released or closed by the running instance: it is let go when the
/// process ends, after the last configuration write (ConfigManager's settings are written as
/// static objects are destroyed). An elevated relaunch waits for exactly that, so the
/// administrator copy never reads Utility.ini while the copy that launched it is still
/// writing it.
class SingleInstanceGuard final : public QObject {
    Q_OBJECT

public:
    enum class Role {
        Primary,    ///< This process is the instance for its data folder.
        Secondary,  ///< Another process already is; ask it to come forward and exit.
    };

    ~SingleInstanceGuard() override;
    SingleInstanceGuard(const SingleInstanceGuard&) = delete;
    SingleInstanceGuard& operator=(const SingleInstanceGuard&) = delete;
    SingleInstanceGuard(SingleInstanceGuard&&) = delete;
    SingleInstanceGuard& operator=(SingleInstanceGuard&&) = delete;

    /// Claims the instance for @p data_root. With a non-zero @p wait_for_previous_exit (the
    /// elevated relaunch) a running instance is waited for, up to that long, instead of being
    /// brought forward: the instance that launched this one is closing. Fails closed with the
    /// reason when the folder's identity or a named object cannot be established, or when the
    /// wait runs out.
    [[nodiscard]] static std::expected<std::unique_ptr<SingleInstanceGuard>, QString> claim(
        const QString& data_root, std::chrono::milliseconds wait_for_previous_exit);

    /// The name stem both named objects share, from a data folder's final path. Pure.
    [[nodiscard]] static QString objectNameStem(const QString& final_data_root_path);

    [[nodiscard]] Role role() const noexcept { return m_role; }

    /// Secondary only: lets the running instance take the foreground (this process was just
    /// started by the user, so it holds that right) and asks it to come forward.
    [[nodiscard]] std::expected<void, QString> activateRunningInstance() const;

    /// Primary only: start emitting activationRequested() when a second launch asks.
    void listenForActivation();

Q_SIGNALS:
    void activationRequested();

private:
    SingleInstanceGuard(Role role, void* instance_mutex, void* activation_event);

    Role m_role;
    void* m_instance_mutex;
    void* m_activation_event;
    QWinEventNotifier* m_notifier{nullptr};
};

}  // namespace sak
