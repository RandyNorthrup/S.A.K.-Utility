// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file partition_file_system_registry.h
/// @brief File-system support capability registry for Partition Manager.

#pragma once

#include <QString>
#include <QStringList>

namespace sak {

struct PartitionFileSystemCapability {
    QString id;
    QString display_name;
    QString support_level;
    QStringList aliases;
    QStringList available_actions;
    QStringList blocked_actions;
    QStringList required_tools;
    bool non_native{false};
};

class PartitionFileSystemRegistry {
public:
    [[nodiscard]] static PartitionFileSystemCapability capabilityFor(const QString& file_system);
    [[nodiscard]] static QString actionSummary(const QStringList& actions);
};

}  // namespace sak
