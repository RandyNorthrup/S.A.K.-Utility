// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file quick_action_fakes.h
/// @brief Fakes for driving the quick-action controller without touching the system: an action
///        that carries a real action's name and admin flag, and an elevated-helper broker that
///        launches nothing. Shared by test_quick_action_controller and test_quick_action_panels.
///
/// The fakes carry the real action names, so the controller routes them as it routes the real
/// ones: an administrator action goes to the elevated-helper broker (a FakeElevationBroker that
/// records the task and its payload), every other action runs in-process on the controller's
/// worker thread.

#pragma once

#include "sak/action_constants.h"
#include "sak/elevation_broker.h"
#include "sak/quick_action.h"
#include "sak/quick_action_controller.h"

#include <QDir>
#include <QFile>
#include <QJsonObject>
#include <QSet>
#include <QStringList>
#include <QThread>

#include <algorithm>
#include <atomic>
#include <expected>
#include <memory>
#include <mutex>

namespace quick_action_fakes {

inline constexpr unsigned long kHoldPollMs = 2;

inline const QString kScreenshotSettings = QStringLiteral("Screenshot Settings");
inline const QString kBitLocker =
    QString::fromLatin1(sak::quick_action_elevation::kBackupBitLockerKeysName);
inline const QString kOptimizePower = QStringLiteral("Optimize Power Settings");
inline const QString kVerifySystemFiles =
    QString::fromLatin1(sak::quick_action_elevation::kVerifySystemFilesName);
inline const QString kCheckDisk =
    QString::fromLatin1(sak::quick_action_elevation::kCheckDiskErrorsName);
inline const QString kInventoryReport = QStringLiteral("System Inventory Report");
inline const QString kResetNetwork =
    QString::fromLatin1(sak::quick_action_elevation::kResetNetworkName);

/// What the fakes did, shared with the test so it outlives them.
struct FakeSystem {
    std::mutex mutex;
    QStringList in_process_runs;    ///< "<action>|<output folder>" per in-process run
    QStringList elevated_tasks;     ///< helper task ids, in order
    QList<QJsonObject> elevated_payloads;
    std::atomic<bool> hold{false};  ///< while set, every run waits (until cancelled)
    std::atomic<int> brokers_created{0};
    std::atomic<int> brokers_destroyed{0};
    std::atomic<int> helper_cancels{0};
    std::atomic<bool> helper_cancel_pending{false};

    QStringList inProcessRuns() {
        const std::lock_guard<std::mutex> lock(mutex);
        return in_process_runs;
    }
    QStringList elevatedTasks() {
        const std::lock_guard<std::mutex> lock(mutex);
        return elevated_tasks;
    }
    QList<QJsonObject> elevatedPayloads() {
        const std::lock_guard<std::mutex> lock(mutex);
        return elevated_payloads;
    }
};

/// An action with a real action's name and admin flag. An in-process run records itself, writes
/// "<name>.txt" into its output folder when it has one, and reports that file as its output.
class FakeQuickAction final : public sak::QuickAction {
public:
    FakeQuickAction(QString name, bool admin, std::shared_ptr<FakeSystem> system)
        : m_name(std::move(name)), m_admin(admin), m_system(std::move(system)) {}

    QString name() const override { return m_name; }
    QString description() const override { return m_name + QStringLiteral(" (fake)"); }
    ActionCategory category() const override { return ActionCategory::Maintenance; }
    QIcon icon() const override { return {}; }
    bool requiresAdmin() const override { return m_admin; }

    void execute() override {
        setStatus(ActionStatus::Running);
        Q_EMIT executionProgress(QStringLiteral("fake step"), 50);
        while (m_system->hold.load() && !isCancelled()) {
            QThread::msleep(kHoldPollMs);
        }
        if (isCancelled()) {
            emitCancelledResult(QStringLiteral("Cancelled by user"));
            return;
        }
        const QString folder = outputFolder();
        {
            const std::lock_guard<std::mutex> lock(m_system->mutex);
            m_system->in_process_runs << m_name + QLatin1Char('|') + folder;
        }
        ExecutionResult result;
        result.success = true;
        result.message = QStringLiteral("fake ok");
        if (!folder.isEmpty()) {
            const QString path = QDir(folder).filePath(m_name + QStringLiteral(".txt"));
            QFile file(path);
            if (!file.open(QIODevice::WriteOnly) || file.write(m_name.toUtf8()) < 0) {
                result.success = false;
                result.message = QStringLiteral("fake could not write %1").arg(path);
                finishWithResult(result, ActionStatus::Failed);
                return;
            }
            result.output_path = path;
        }
        finishWithResult(result, ActionStatus::Success);
    }

private:
    QString m_name;
    bool m_admin;
    std::shared_ptr<FakeSystem> m_system;
};

/// The elevated helper, faked at the broker: records each task and its payload, waits while the
/// test holds it, and answers a cancel the way the helper does (a cancelled, failed result).
class FakeElevationBroker final : public sak::ElevationBroker {
public:
    explicit FakeElevationBroker(std::shared_ptr<FakeSystem> system) : m_system(std::move(system)) {
        m_system->brokers_created.fetch_add(1);
    }
    ~FakeElevationBroker() override { m_system->brokers_destroyed.fetch_add(1); }
    FakeElevationBroker(const FakeElevationBroker&) = delete;
    FakeElevationBroker& operator=(const FakeElevationBroker&) = delete;
    FakeElevationBroker(FakeElevationBroker&&) = delete;
    FakeElevationBroker& operator=(FakeElevationBroker&&) = delete;

    auto executeTask(const QString& task_id, const QString& reason, const QJsonObject& payload)
        -> std::expected<sak::ElevatedTaskResult, sak::error_code> override {
        static_cast<void>(reason);
        {
            const std::lock_guard<std::mutex> lock(m_system->mutex);
            m_system->elevated_tasks << task_id;
            m_system->elevated_payloads << payload;
        }
        Q_EMIT progressUpdated(40, QStringLiteral("fake helper working"));
        while (m_system->hold.load() && !m_system->helper_cancel_pending.load()) {
            QThread::msleep(kHoldPollMs);
        }
        sak::ElevatedTaskResult result;
        if (m_system->helper_cancel_pending.exchange(false)) {
            result.success = false;
            result.data =
                QJsonObject{{QStringLiteral("message"), QStringLiteral("Cancelled by user")},
                            {QStringLiteral("status"),
                             static_cast<int>(sak::QuickAction::ActionStatus::Cancelled)}};
            return result;
        }
        result.success = true;
        result.data = QJsonObject{{QStringLiteral("message"), QStringLiteral("fake elevated ok")}};
        return result;
    }

    void cancelCurrentTask() override {
        m_system->helper_cancels.fetch_add(1);
        m_system->helper_cancel_pending.store(true);
    }

private:
    std::shared_ptr<FakeSystem> m_system;
};

/// Registers fakes for @p names on @p controller, admin flags as the real actions have them.
[[nodiscard]] inline bool registerFakes(sak::QuickActionController& controller,
                                        const std::shared_ptr<FakeSystem>& system,
                                        const QStringList& names) {
    static const QSet<QString> admin_actions{
        kBitLocker, kVerifySystemFiles, kCheckDisk, kResetNetwork};
    return std::ranges::all_of(names, [&](const QString& name) {
        return !controller
                    .registerAction(std::make_unique<FakeQuickAction>(
                        name, admin_actions.contains(name), system))
                    .isEmpty();
    });
}

/// Builds the controller's broker as a FakeElevationBroker over @p system.
inline void useFakeElevatedHelper(const std::shared_ptr<FakeSystem>& system) {
    sak::QuickActionController::setElevationBrokerFactoryForTesting(
        [system]() -> std::unique_ptr<sak::ElevationBroker> {
            return std::make_unique<FakeElevationBroker>(system);
        });
}

inline void useRealElevatedHelper() {
    sak::QuickActionController::setElevationBrokerFactoryForTesting({});
}

}  // namespace quick_action_fakes
