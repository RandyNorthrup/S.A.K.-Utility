// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file partition_executor.h
/// @brief Applies queued Partition Manager operations.

#pragma once

#include "sak/partition_manager_types.h"
#include "sak/partition_script_builder.h"

#include <QObject>

#include <atomic>
#include <mutex>

namespace sak {

class ElevationBroker;

class PartitionExecutor : public QObject {
    Q_OBJECT

public:
    explicit PartitionExecutor(QObject* parent = nullptr);

    [[nodiscard]] PartitionExecutionResult execute(const QVector<PartitionOperation>& operations,
                                                   bool dry_run,
                                                   bool use_elevation);
    void cancel();

Q_SIGNALS:
    void progressUpdated(int percent, const QString& status);
    void logMessage(const QString& message);

private:
    PartitionScriptBuilder m_script_builder;
    std::atomic_bool m_cancelled{false};
    mutable std::mutex m_active_broker_mutex;
    ElevationBroker* m_active_broker{nullptr};

    [[nodiscard]] PartitionExecutionStep executeOperation(const PartitionOperation& operation,
                                                          bool dry_run,
                                                          bool use_elevation);
    [[nodiscard]] PartitionExecutionStep executeScript(const PartitionOperation& operation,
                                                       const PartitionScript& script,
                                                       bool use_elevation);
    [[nodiscard]] PartitionExecutionStep executeElevatedScript(const PartitionOperation& operation,
                                                               const PartitionScript& script);
    [[nodiscard]] PartitionExecutionStep executeLocalScript(const PartitionOperation& operation,
                                                            const PartitionScript& script);
    void setActiveBroker(ElevationBroker* broker);
};

}  // namespace sak
