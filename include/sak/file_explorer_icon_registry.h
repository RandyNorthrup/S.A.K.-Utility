// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "sak/file_explorer_command_registry.h"

#include <QIcon>
#include <QString>
#include <QVector>

namespace sak {

struct FileExplorerIconDescriptor {
    QString key;
    QString resource_path;
    QString upstream_key;
    QString upstream_source;
    QString license;
};

class FileExplorerIconRegistry {
public:
    [[nodiscard]] static QIcon iconForCommand(FileExplorerCommandId command);
    [[nodiscard]] static QIcon iconForKey(const QString& key);
    [[nodiscard]] static QString iconKeyForCommand(FileExplorerCommandId command);
    [[nodiscard]] static FileExplorerIconDescriptor descriptorForKey(const QString& key);
    [[nodiscard]] static QVector<FileExplorerIconDescriptor> descriptors();
};

}  // namespace sak
