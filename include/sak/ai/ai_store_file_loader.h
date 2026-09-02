// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <QByteArray>
#include <QString>
#include <QStringList>

#include <functional>

namespace sak::ai::detail {

/// @brief Limits and names for one AI store's directory-backed file format.
struct StoreFileLoadPolicy {
    QString store_name;
    QString file_extension;
    QString file_type_name;
    qint64 max_file_bytes{0};
    int max_files{0};
};

/// @brief Test-only race hooks invoked around native file open and read operations.
struct StoreFileLoadHooks {
    std::function<void(const QString&)> before_file_open;
    std::function<void(const QString&)> before_file_read;
};

using StoreFileConsumer = std::function<bool(const QString&, const QByteArray&)>;

/// @brief Enumerate and read one Qt-resource or native AI store directory.
/// @details Native paths are opened and verified by handle. Reparse-point directories and files,
///          files whose realized parent differs from the opened directory, unstable reads,
///          oversized files, and over-broad directories are refused. Qt resources retain QFile
///          loading because they are not native filesystem objects.
[[nodiscard]] bool loadStoreDirectoryFiles(const QString& directory,
                                           const StoreFileLoadPolicy& policy,
                                           const StoreFileLoadHooks& hooks,
                                           const StoreFileConsumer& consume,
                                           QStringList* errors);

}  // namespace sak::ai::detail
