// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/file_explorer_types.h"

#include <QDir>

#include <algorithm>

namespace sak {

bool FileExplorerTargetId::isEmpty() const {
    return value.trimmed().isEmpty();
}

FileExplorerTargetId FileExplorerTargetId::fromTarget(const FileManagementTarget& target) {
    return FileExplorerTargetId{target.id.trimmed()};
}

bool FileExplorerLocation::isEmpty() const {
    return target_id.isEmpty() && path.trimmed().isEmpty();
}

FileExplorerLocation FileExplorerLocation::normalized(const bool local_file_system) const {
    FileExplorerLocation result = *this;
    result.path = normalizePath(path, local_file_system);
    return result;
}

bool FileExplorerLocation::atRoot(const bool local_file_system) const {
    const QString normalized_path = normalizePath(path, local_file_system);
    if (normalized_path.isEmpty()) {
        return true;
    }

    if (!local_file_system) {
        return normalized_path == QStringLiteral("/");
    }

    return QDir(normalized_path).isRoot();
}

QString FileExplorerLocation::normalizePath(const QString& path, const bool local_file_system) {
    QString normalized_path = QDir::fromNativeSeparators(path.trimmed());
    if (normalized_path.isEmpty()) {
        return local_file_system ? QString() : QStringLiteral("/");
    }

    normalized_path = QDir::cleanPath(normalized_path);
    if (normalized_path == QStringLiteral(".")) {
        return local_file_system ? QString() : QStringLiteral("/");
    }

    if (!local_file_system) {
        while (normalized_path.startsWith(QStringLiteral("//"))) {
            normalized_path.remove(0, 1);
        }
        if (!normalized_path.startsWith(QLatin1Char('/'))) {
            normalized_path.prepend(QLatin1Char('/'));
        }
        if (normalized_path.size() > 1 && normalized_path.endsWith(QLatin1Char('/'))) {
            normalized_path.chop(1);
        }
    }

    return normalized_path;
}

QString FileExplorerLocation::parentPath(const QString& path, const bool local_file_system) {
    const QString normalized_path = normalizePath(path, local_file_system);
    if (normalized_path.isEmpty()) {
        return QString();
    }

    if (!local_file_system) {
        if (normalized_path == QStringLiteral("/")) {
            return QStringLiteral("/");
        }

        const int separator = normalized_path.lastIndexOf(QLatin1Char('/'));
        if (separator <= 0) {
            return QStringLiteral("/");
        }
        return normalized_path.left(separator);
    }

    QDir directory(normalized_path);
    if (directory.isRoot()) {
        return normalized_path;
    }

    if (directory.cdUp()) {
        return QDir::fromNativeSeparators(QDir::cleanPath(directory.absolutePath()));
    }

    const int separator = normalized_path.lastIndexOf(QLatin1Char('/'));
    if (separator < 0) {
        return QString();
    }
    if (separator == 2 && normalized_path.size() >= 3 && normalized_path.at(1) == QLatin1Char(':')) {
        return normalized_path.left(3);
    }
    return separator == 0 ? QStringLiteral("/") : normalized_path.left(separator);
}

void FileExplorerSelection::clear() {
    entries.clear();
}

bool FileExplorerSelection::isEmpty() const {
    return entries.isEmpty();
}

bool FileExplorerSelection::hasSingleEntry() const {
    return entries.size() == 1;
}

int FileExplorerSelection::count() const {
    return entries.size();
}

bool FileExplorerSelection::containsDirectory() const {
    return std::any_of(entries.cbegin(), entries.cend(), [](const FileManagementEntry& entry) {
        return entry.directory;
    });
}

bool FileExplorerSelection::containsRegularFile() const {
    return std::any_of(entries.cbegin(), entries.cend(), [](const FileManagementEntry& entry) {
        return entry.regular_file;
    });
}

uint64_t FileExplorerSelection::totalRegularFileBytes() const {
    uint64_t total = 0;
    for (const FileManagementEntry& entry : entries) {
        if (entry.regular_file) {
            total += entry.size_bytes;
        }
    }
    return total;
}

QStringList FileExplorerSelection::paths() const {
    QStringList result;
    result.reserve(entries.size());
    for (const FileManagementEntry& entry : entries) {
        result.append(entry.path);
    }
    return result;
}

FileExplorerItemCapabilities FileExplorerItemCapabilities::fromTargetAndEntry(
    const FileManagementTarget& target,
    const FileManagementEntry& entry) {
    FileExplorerItemCapabilities capabilities;
    capabilities.can_open = target.can_browse;
    capabilities.can_preview = target.can_read_files && entry.regular_file;
    capabilities.can_copy_path = !entry.path.trimmed().isEmpty();
    capabilities.can_rename = target.can_write_files && !entry.path.trimmed().isEmpty();
    capabilities.can_delete = target.can_write_files && !entry.path.trimmed().isEmpty();

    if (!target.can_browse) {
        capabilities.blockers.append(QStringLiteral("Selected target cannot browse files."));
    }
    if (entry.regular_file && !target.can_read_files) {
        capabilities.blockers.append(QStringLiteral("Selected target cannot read files."));
    }
    if ((!capabilities.can_rename || !capabilities.can_delete) && !target.can_write_files) {
        const QString write_blocker = target.blockers.join(QStringLiteral("; "));
        capabilities.blockers.append(write_blocker.isEmpty()
                                         ? QStringLiteral("Selected target does not allow file writes.")
                                         : write_blocker);
    }
    if (entry.path.trimmed().isEmpty()) {
        capabilities.blockers.append(QStringLiteral("Selected item has no stable path."));
    }
    capabilities.blockers.removeDuplicates();
    return capabilities;
}

bool FileExplorerPaneState::canGoBack() const {
    return !back_stack.isEmpty();
}

bool FileExplorerPaneState::canGoForward() const {
    return !forward_stack.isEmpty();
}

void FileExplorerPaneState::navigateTo(const FileExplorerLocation& destination,
                                       const bool local_file_system) {
    const FileExplorerLocation normalized_destination = destination.normalized(local_file_system);
    if (location == normalized_destination) {
        return;
    }

    if (!location.isEmpty()) {
        back_stack.append(location);
    }

    location = normalized_destination;
    forward_stack.clear();
    selection.clear();
}

bool FileExplorerPaneState::goBack() {
    if (back_stack.isEmpty()) {
        return false;
    }

    if (!location.isEmpty()) {
        forward_stack.append(location);
    }
    location = back_stack.takeLast();
    selection.clear();
    return true;
}

bool FileExplorerPaneState::goForward() {
    if (forward_stack.isEmpty()) {
        return false;
    }

    if (!location.isEmpty()) {
        back_stack.append(location);
    }
    location = forward_stack.takeLast();
    selection.clear();
    return true;
}

bool FileExplorerPaneState::goUp(const bool local_file_system) {
    const QString parent = FileExplorerLocation::parentPath(location.path, local_file_system);
    if (parent.isEmpty() || parent == FileExplorerLocation::normalizePath(location.path, local_file_system)) {
        return false;
    }

    FileExplorerLocation destination = location;
    destination.path = parent;
    navigateTo(destination, local_file_system);
    return true;
}

bool FileExplorerTabState::hasSecondaryPane() const {
    return secondary_pane_enabled && split != FileExplorerPaneSplit::None;
}

void FileExplorerTabState::setSecondaryPaneEnabled(const bool enabled,
                                                   const FileExplorerPaneSplit requested_split) {
    secondary_pane_enabled = enabled;
    split = enabled ? requested_split : FileExplorerPaneSplit::None;
    if (!hasSecondaryPane() && active_pane_index == 1) {
        active_pane_index = 0;
    }
}

}  // namespace sak
