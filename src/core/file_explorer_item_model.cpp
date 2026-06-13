// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/file_explorer_item_model.h"

#include "sak/layout_constants.h"

#include <QFileInfo>
#include <QStringList>

#include <algorithm>
#include <utility>

namespace sak {

namespace {

QString displayName(const FileManagementEntry& entry, const bool showFileExtensions) {
    if (showFileExtensions || !entry.regular_file) {
        return entry.name;
    }

    const QFileInfo info(entry.name);
    if (info.suffix().isEmpty() || info.completeBaseName().isEmpty()) {
        return entry.name;
    }
    return info.completeBaseName();
}

QString displayText(const FileManagementEntry& entry,
                    const int column,
                    const bool showFileExtensions) {
    switch (column) {
    case FileExplorerItemModel::NameColumn:
        return displayName(entry, showFileExtensions);
    case FileExplorerItemModel::TypeColumn:
        return entry.type;
    case FileExplorerItemModel::SizeColumn:
        return entry.directory ? QString() : FileExplorerItemModel::sizeText(entry.size_bytes);
    case FileExplorerItemModel::ModifiedColumn:
        return FileExplorerItemModel::timeText(entry.modified_time);
    case FileExplorerItemModel::CreatedColumn:
        return FileExplorerItemModel::timeText(entry.created_time);
    case FileExplorerItemModel::IdentifierColumn:
        return entry.identifier;
    case FileExplorerItemModel::AttributesColumn:
        return FileExplorerItemModel::attributeSummary(entry);
    case FileExplorerItemModel::PathColumn:
        return entry.path;
    default:
        return {};
    }
}

int compareEntries(const FileManagementEntry& left,
                   const FileManagementEntry& right,
                   const int column) {
    if (left.directory != right.directory) {
        return left.directory ? -1 : 1;
    }

    switch (column) {
    case FileExplorerItemModel::SizeColumn:
        if (left.size_bytes == right.size_bytes) {
            return QString::localeAwareCompare(left.name, right.name);
        }
        return left.size_bytes < right.size_bytes ? -1 : 1;
    case FileExplorerItemModel::ModifiedColumn:
        if (left.modified_time == right.modified_time) {
            return QString::localeAwareCompare(left.name, right.name);
        }
        if (!left.modified_time.isValid()) {
            return 1;
        }
        if (!right.modified_time.isValid()) {
            return -1;
        }
        return left.modified_time < right.modified_time ? -1 : 1;
    case FileExplorerItemModel::CreatedColumn:
        if (left.created_time == right.created_time) {
            return QString::localeAwareCompare(left.name, right.name);
        }
        if (!left.created_time.isValid()) {
            return 1;
        }
        if (!right.created_time.isValid()) {
            return -1;
        }
        return left.created_time < right.created_time ? -1 : 1;
    case FileExplorerItemModel::TypeColumn:
        return QString::localeAwareCompare(left.type, right.type);
    case FileExplorerItemModel::IdentifierColumn:
        return QString::localeAwareCompare(left.identifier, right.identifier);
    case FileExplorerItemModel::PathColumn:
        return QString::localeAwareCompare(left.path, right.path);
    case FileExplorerItemModel::NameColumn:
    default:
        return QString::localeAwareCompare(left.name, right.name);
    }
}

}  // namespace

FileExplorerItemModel::FileExplorerItemModel(QObject* parent) : QAbstractTableModel(parent) {}

int FileExplorerItemModel::rowCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : m_entries.size();
}

int FileExplorerItemModel::columnCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : ColumnCount;
}

QVariant FileExplorerItemModel::data(const QModelIndex& index, const int role) const {
    if (!index.isValid() || index.row() < 0 || index.row() >= m_entries.size()) {
        return {};
    }

    const FileManagementEntry& entry = m_entries.at(index.row());
    switch (role) {
    case Qt::DisplayRole:
        return displayText(entry, index.column(), m_show_file_extensions);
    case Qt::ToolTipRole:
        return entry.path;
    case Qt::TextAlignmentRole:
        return index.column() == SizeColumn ? QVariant(Qt::AlignRight | Qt::AlignVCenter)
                                            : QVariant(Qt::AlignLeft | Qt::AlignVCenter);
    case EntryPathRole:
        return entry.path;
    case EntryNameRole:
        return entry.name;
    case EntryTypeRole:
        return entry.type;
    case EntrySizeRole:
        return QVariant::fromValue<qulonglong>(entry.size_bytes);
    case EntryIdentifierRole:
        return entry.identifier;
    case EntryDirectoryRole:
        return entry.directory;
    case EntryRegularFileRole:
        return entry.regular_file;
    case EntrySymlinkRole:
        return entry.symlink;
    case EntryLinkTargetRole:
        return entry.link_target;
    case EntryAttributeSummaryRole:
        return attributeSummary(entry);
    case EntryModifiedTimeRole:
        return entry.modified_time;
    case EntryCreatedTimeRole:
        return entry.created_time;
    default:
        return {};
    }
}

QVariant FileExplorerItemModel::headerData(const int section,
                                           const Qt::Orientation orientation,
                                           const int role) const {
    if (orientation != Qt::Horizontal || role != Qt::DisplayRole) {
        return {};
    }

    switch (section) {
    case NameColumn:
        return tr("Name");
    case TypeColumn:
        return tr("Type");
    case SizeColumn:
        return tr("Size");
    case ModifiedColumn:
        return tr("Modified");
    case CreatedColumn:
        return tr("Created");
    case IdentifierColumn:
        return tr("ID");
    case AttributesColumn:
        return tr("Attributes");
    case PathColumn:
        return tr("Path");
    default:
        return {};
    }
}

Qt::ItemFlags FileExplorerItemModel::flags(const QModelIndex& index) const {
    if (!index.isValid()) {
        return Qt::NoItemFlags;
    }
    return Qt::ItemIsEnabled | Qt::ItemIsSelectable;
}

void FileExplorerItemModel::sort(const int column, const Qt::SortOrder order) {
    if (column < 0 || column >= ColumnCount || m_entries.size() < 2) {
        return;
    }

    Q_EMIT layoutAboutToBeChanged();
    std::stable_sort(m_entries.begin(), m_entries.end(), [column, order](const auto& left, const auto& right) {
        if (left.directory != right.directory) {
            return left.directory;
        }
        const int compared = compareEntries(left, right, column);
        return order == Qt::AscendingOrder ? compared < 0 : compared > 0;
    });
    Q_EMIT layoutChanged();
}

void FileExplorerItemModel::setEntries(QVector<FileManagementEntry> entries) {
    beginResetModel();
    m_entries = std::move(entries);
    endResetModel();
}

void FileExplorerItemModel::clear() {
    setEntries({});
}

void FileExplorerItemModel::setShowFileExtensions(const bool show) {
    if (m_show_file_extensions == show) {
        return;
    }
    m_show_file_extensions = show;
    if (!m_entries.isEmpty()) {
        Q_EMIT dataChanged(index(0, NameColumn),
                           index(m_entries.size() - 1, NameColumn),
                           {Qt::DisplayRole});
    }
}

QVector<FileManagementEntry> FileExplorerItemModel::entries() const {
    return m_entries;
}

FileManagementEntry FileExplorerItemModel::entryAt(const int row) const {
    return hasEntry(row) ? m_entries.at(row) : FileManagementEntry{};
}

bool FileExplorerItemModel::hasEntry(const int row) const {
    return row >= 0 && row < m_entries.size();
}

bool FileExplorerItemModel::showFileExtensions() const {
    return m_show_file_extensions;
}

QString FileExplorerItemModel::attributeSummary(const FileManagementEntry& entry) {
    QStringList attributes;
    if (entry.directory) {
        attributes.append(QStringLiteral("Directory"));
    }
    if (entry.regular_file) {
        attributes.append(QStringLiteral("File"));
    }
    if (entry.symlink) {
        attributes.append(entry.link_target.trimmed().isEmpty()
                              ? QStringLiteral("Symlink")
                              : QStringLiteral("Symlink -> %1").arg(entry.link_target));
    }
    return attributes.isEmpty() ? QStringLiteral("-") : attributes.join(QStringLiteral(", "));
}

QString FileExplorerItemModel::sizeText(const uint64_t bytes) {
    if (bytes >= sak::kBytesPerGB) {
        return QStringLiteral("%1 GB").arg(bytes / sak::kBytesPerGBf, 0, 'f', 2);
    }
    if (bytes >= sak::kBytesPerMB) {
        return QStringLiteral("%1 MB").arg(bytes / sak::kBytesPerMBf, 0, 'f', 1);
    }
    if (bytes >= sak::kBytesPerKB) {
        return QStringLiteral("%1 KB").arg(bytes / sak::kBytesPerKBf, 0, 'f', 0);
    }
    return QStringLiteral("%1 B").arg(bytes);
}

QString FileExplorerItemModel::timeText(const QDateTime& time) {
    return time.isValid() ? time.toLocalTime().toString(QStringLiteral("yyyy-MM-dd HH:mm")) : QString();
}

}  // namespace sak
