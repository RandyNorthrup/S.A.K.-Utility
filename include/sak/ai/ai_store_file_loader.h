// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <QByteArray>
#include <QString>
#include <QStringList>

#include <functional>
#include <memory>

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
    std::function<void(const QString&)> before_child_open;
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

/// @brief Policy for `<root>/<child>/<fixed file>` stores such as AI session manifests.
struct StoreChildFileLoadPolicy {
    QString store_name;
    QString child_type_name;
    QString file_name;
    qint64 max_file_bytes{0};
    int max_child_directories{0};
};

enum class StoreChildFileLoadResult {
    Loaded,
    Missing,
    Refused
};

using StoreChildFileConsumer =
    std::function<bool(const QString&, const QString&, const QByteArray&)>;

struct StoreChildFileLoadOptions {
    StoreChildFileLoadPolicy policy;
    StoreFileLoadHooks hooks;
    StoreChildFileConsumer consume;
};

/// @brief Enumerate bounded direct child directories and load one fixed file from each.
/// @details Missing fixed files are ignored because a child without that file is not a record.
///          Any identity, reparse, cap, read, or consumer error refuses the whole enumeration.
[[nodiscard]] bool loadStoreChildDirectoryFiles(const QString& root_directory,
                                                const StoreChildFileLoadOptions& options,
                                                QStringList* errors);

/// @brief Load one `<root>/<child>/<fixed file>` record through pinned parent handles.
[[nodiscard]] StoreChildFileLoadResult loadStoreChildDirectoryFile(
    const QString& root_directory,
    const QString& child_name,
    const StoreChildFileLoadOptions& options,
    QStringList* errors);

/// @brief Opaque lifetime pin for one verified native store directory chain.
class StoreDirectoryPin {
public:
    StoreDirectoryPin() = default;
    [[nodiscard]] bool valid() const { return !m_path.isEmpty() && m_state != nullptr; }
    [[nodiscard]] QString path() const { return m_path; }

private:
    StoreDirectoryPin(QString path, std::shared_ptr<void> state);

    QString m_path;
    std::shared_ptr<void> m_state;

    friend StoreDirectoryPin pinOrCreateStoreDirectory(
        const QString&, const QStringList&, int, const StoreFileLoadHooks&, QString*);
};

/// @brief Pin @p root_directory and walk direct child components without following reparses.
/// @param required_component_count Leading components that must already exist; later components
///        are created if absent. Every opened handle is retained without FILE_SHARE_DELETE.
[[nodiscard]] StoreDirectoryPin pinOrCreateStoreDirectory(const QString& root_directory,
                                                          const QStringList& components,
                                                          int required_component_count,
                                                          const StoreFileLoadHooks& hooks,
                                                          QString* error_message);

}  // namespace sak::ai::detail
