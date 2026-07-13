// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/file_explorer_item_model.h"

#include "sak/layout_constants.h"

#include <QBrush>
#include <QDateTime>
#include <QFileInfo>
#include <QGuiApplication>
#include <QHash>
#include <QPalette>
#include <QStringList>

#include <algorithm>
#include <array>
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

int compareSize(const FileManagementEntry& left, const FileManagementEntry& right) {
    if (left.size_bytes == right.size_bytes) {
        return QString::localeAwareCompare(left.name, right.name);
    }
    return left.size_bytes < right.size_bytes ? -1 : 1;
}

int compareDateTime(const QDateTime& left,
                    const QDateTime& right,
                    const QString& left_name,
                    const QString& right_name) {
    if (left == right) {
        return QString::localeAwareCompare(left_name, right_name);
    }
    if (!left.isValid()) {
        return 1;
    }
    if (!right.isValid()) {
        return -1;
    }
    return left < right ? -1 : 1;
}

int compareEntries(const FileManagementEntry& left,
                   const FileManagementEntry& right,
                   const int column) {
    if (left.directory != right.directory) {
        return left.directory ? -1 : 1;
    }

    switch (column) {
    case FileExplorerItemModel::SizeColumn:
        return compareSize(left, right);
    case FileExplorerItemModel::ModifiedColumn:
        return compareDateTime(left.modified_time, right.modified_time, left.name, right.name);
    case FileExplorerItemModel::CreatedColumn:
        return compareDateTime(left.created_time, right.created_time, left.name, right.name);
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

QVariant entryRoleValue(const FileManagementEntry& entry, const int role) {
    using Model = FileExplorerItemModel;
    using Accessor = QVariant (*)(const FileManagementEntry&);
    static const QHash<int, Accessor> kAccessors{
        {Qt::ToolTipRole, [](const FileManagementEntry& e) -> QVariant { return e.path; }},
        {Model::EntryPathRole, [](const FileManagementEntry& e) -> QVariant { return e.path; }},
        {Model::EntryNameRole, [](const FileManagementEntry& e) -> QVariant { return e.name; }},
        {Model::EntryTypeRole, [](const FileManagementEntry& e) -> QVariant { return e.type; }},
        {Model::EntrySizeRole,
         [](const FileManagementEntry& e) -> QVariant {
             return QVariant::fromValue<qulonglong>(e.size_bytes);
         }},
        {Model::EntryIdentifierRole,
         [](const FileManagementEntry& e) -> QVariant { return e.identifier; }},
        {Model::EntryDirectoryRole,
         [](const FileManagementEntry& e) -> QVariant { return e.directory; }},
        {Model::EntryRegularFileRole,
         [](const FileManagementEntry& e) -> QVariant { return e.regular_file; }},
        {Model::EntrySymlinkRole,
         [](const FileManagementEntry& e) -> QVariant { return e.symlink; }},
        {Model::EntryLinkTargetRole,
         [](const FileManagementEntry& e) -> QVariant { return e.link_target; }},
        {Model::EntryAttributeSummaryRole,
         [](const FileManagementEntry& e) -> QVariant { return Model::attributeSummary(e); }},
        {Model::EntryModifiedTimeRole,
         [](const FileManagementEntry& e) -> QVariant { return e.modified_time; }},
        {Model::EntryCreatedTimeRole,
         [](const FileManagementEntry& e) -> QVariant { return e.created_time; }},
    };
    const auto accessor = kAccessors.find(role);
    return accessor != kAccessors.end() ? accessor.value()(entry) : QVariant();
}

QVariant alignmentForColumn(const int column) {
    return column == FileExplorerItemModel::SizeColumn ? QVariant(Qt::AlignRight | Qt::AlignVCenter)
                                                       : QVariant(Qt::AlignLeft | Qt::AlignVCenter);
}

}  // namespace

FileExplorerItemModel::FileExplorerItemModel(QObject* parent) : QAbstractTableModel(parent) {}

int FileExplorerItemModel::rowCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : m_entries.size();
}

int FileExplorerItemModel::columnCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : ColumnCount;
}

QStringList FileExplorerItemModel::tagsForEntry(const FileManagementEntry& entry) const {
    return m_tag_provider ? m_tag_provider(entry.path) : QStringList{};
}

QVariant FileExplorerItemModel::decorationForColumn(const FileManagementEntry& entry,
                                                    const int column) const {
    if (column != NameColumn || !m_icon_provider) {
        return {};
    }
    return m_icon_provider(entry);
}

QVariant FileExplorerItemModel::displayForColumn(const FileManagementEntry& entry,
                                                 const int column) const {
    if (column == TagsColumn) {
        return tagsForEntry(entry).join(QStringLiteral(", "));
    }
    return displayText(entry, column, m_show_file_extensions);
}

QVariant FileExplorerItemModel::data(const QModelIndex& index, const int role) const {
    if (!index.isValid() || index.row() < 0 || index.row() >= m_entries.size()) {
        return {};
    }

    const FileManagementEntry& entry = m_entries.at(index.row());
    switch (role) {
    case Qt::DisplayRole:
        return displayForColumn(entry, index.column());
    case Qt::EditRole:
        return editForColumn(entry, index.column());
    case Qt::DecorationRole:
        return decorationForColumn(entry, index.column());
    case Qt::TextAlignmentRole:
        return alignmentForColumn(index.column());
    case Qt::ForegroundRole:
        return foregroundForEntry(entry);
    case EntryTagsRole:
        return tagsForEntry(entry);
    default:
        return entryRoleValue(entry, role);
    }
}

QVariant FileExplorerItemModel::foregroundForEntry(const FileManagementEntry& entry) const {
    // Files dims cut items (ListedItem.Opacity 0.4); approximated with the
    // disabled text color until the move-paste lands.
    if (!m_cut_paths.contains(entry.path)) {
        return {};
    }
    return QBrush(QGuiApplication::palette().color(QPalette::Disabled, QPalette::Text));
}

QVariant FileExplorerItemModel::editForColumn(const FileManagementEntry& entry,
                                              const int column) const {
    // Inline rename edits what the user sees (extension-hidden aware).
    return column == NameColumn ? displayForColumn(entry, NameColumn) : QVariant();
}

QVariant FileExplorerItemModel::headerData(const int section,
                                           const Qt::Orientation orientation,
                                           const int role) const {
    if (orientation != Qt::Horizontal || role != Qt::DisplayRole) {
        return {};
    }
    static constexpr auto kHeaders = std::to_array<std::pair<int, const char*>>({
        {NameColumn, QT_TR_NOOP("Name")},
        {TypeColumn, QT_TR_NOOP("Type")},
        {SizeColumn, QT_TR_NOOP("Size")},
        {ModifiedColumn, QT_TR_NOOP("Modified")},
        {CreatedColumn, QT_TR_NOOP("Created")},
        {IdentifierColumn, QT_TR_NOOP("ID")},
        {AttributesColumn, QT_TR_NOOP("Attributes")},
        {TagsColumn, QT_TR_NOOP("Tags")},
        {PathColumn, QT_TR_NOOP("Path")},
    });
    const auto it = std::ranges::find(kHeaders, section, &std::pair<int, const char*>::first);
    return it != kHeaders.end() ? QVariant(tr(it->second)) : QVariant();
}

Qt::ItemFlags FileExplorerItemModel::flags(const QModelIndex& index) const {
    if (!index.isValid()) {
        return Qt::NoItemFlags;
    }
    // Only the name is editable (inline rename); every other column is data.
    if (index.column() == NameColumn) {
        return Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsEditable;
    }
    return Qt::ItemIsEnabled | Qt::ItemIsSelectable;
}

bool FileExplorerItemModel::setData(const QModelIndex& index,
                                    const QVariant& value,
                                    const int role) {
    if (role != Qt::EditRole || index.column() != NameColumn || !hasEntry(index.row())) {
        return false;
    }
    const FileManagementEntry entry = m_entries.at(index.row());
    const QString name = completedRenameName(entry, value.toString());
    if (name.isEmpty() || name == entry.name) {
        return false;
    }
    Q_EMIT renameRequested(index.row(), name);
    return false;
}

QString FileExplorerItemModel::completedRenameName(const FileManagementEntry& entry,
                                                   const QString& edited) const {
    // Files CommitRenameAsync: trim whitespace and trailing dots.
    QString name = edited.trimmed();
    while (name.endsWith(QLatin1Char('.'))) {
        name.chop(1);
    }
    // With extensions hidden the editor shows the base name only; re-attach
    // the real suffix before the rename (Files nameIsComplete=false path).
    const QString displayed = displayForColumn(entry, NameColumn).toString();
    if (!m_show_file_extensions && !entry.directory && entry.name.startsWith(displayed) &&
        entry.name.length() > displayed.length() && !name.isEmpty()) {
        name += entry.name.mid(displayed.length());
    }
    return name;
}

void FileExplorerItemModel::sort(const int column, const Qt::SortOrder order) {
    constexpr int kMinSortableEntryCount = 2;
    if (column < 0 || column >= ColumnCount || m_entries.size() < kMinSortableEntryCount) {
        return;
    }

    Q_EMIT layoutAboutToBeChanged();
    std::stable_sort(m_entries.begin(),
                     m_entries.end(),
                     [column, order](const auto& left, const auto& right) {
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

void FileExplorerItemModel::setCutPaths(const QSet<QString>& paths) {
    if (m_cut_paths == paths) {
        return;
    }
    m_cut_paths = paths;
    if (!m_entries.isEmpty()) {
        Q_EMIT dataChanged(index(0, NameColumn),
                           index(m_entries.size() - 1, ColumnCount - 1),
                           {Qt::ForegroundRole});
    }
}

QSet<QString> FileExplorerItemModel::cutPaths() const {
    return m_cut_paths;
}

void FileExplorerItemModel::setTagProvider(TagProvider provider) {
    m_tag_provider = std::move(provider);
    refreshTags();
}

void FileExplorerItemModel::setIconProvider(IconProvider provider) {
    m_icon_provider = std::move(provider);
    if (!m_entries.isEmpty()) {
        Q_EMIT dataChanged(index(0, NameColumn),
                           index(m_entries.size() - 1, NameColumn),
                           {Qt::DecorationRole});
    }
}

void FileExplorerItemModel::refreshTags() {
    if (m_entries.isEmpty()) {
        return;
    }
    Q_EMIT dataChanged(index(0, TagsColumn),
                       index(m_entries.size() - 1, TagsColumn),
                       {Qt::DisplayRole, EntryTagsRole});
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
    constexpr int kSizeTextDecimals = 2;
    if (bytes >= sak::kBytesPerGB) {
        return QStringLiteral("%1 GB").arg(bytes / sak::kBytesPerGBf, 0, 'f', kSizeTextDecimals);
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
    return time.isValid() ? time.toLocalTime().toString(QStringLiteral("yyyy-MM-dd HH:mm"))
                          : QString();
}

}  // namespace sak
