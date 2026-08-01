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
        {Model::EntryHiddenRole, [](const FileManagementEntry& e) -> QVariant { return e.hidden; }},
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
    if (role == Qt::CheckStateRole) {
        return checkStateForEntry(entry, index.column());
    }
    if (role == EntryTagsRole) {
        return tagsForEntry(entry);
    }
    // Presentation roles first; anything they do not answer falls through to
    // the entry-field role table (a presentation role that answers "empty"
    // has no entry-field mapping either, so the fallthrough stays a no-op).
    const QVariant presentation = presentationData(entry, index.column(), role);
    return presentation.isValid() ? presentation : entryRoleValue(entry, role);
}

QVariant FileExplorerItemModel::presentationData(const FileManagementEntry& entry,
                                                 const int column,
                                                 const int role) const {
    switch (role) {
    case Qt::DisplayRole:
        return displayForColumn(entry, column);
    case Qt::EditRole:
        return editForColumn(entry, column);
    case Qt::DecorationRole:
        return decorationForColumn(entry, column);
    case Qt::TextAlignmentRole:
        return alignmentForColumn(column);
    case Qt::ForegroundRole:
        return foregroundForEntry(entry);
    default:
        return {};
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
    const Qt::ItemFlags drag = m_drag_payload_provider ? Qt::ItemIsDragEnabled : Qt::NoItemFlags;
    // Only the name is editable (inline rename); every other column is data.
    if (index.column() == NameColumn) {
        const Qt::ItemFlags check = m_checkboxes_visible && m_check_state_provider
                                        ? Qt::ItemIsUserCheckable
                                        : Qt::NoItemFlags;
        return Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsEditable | drag | check;
    }
    return Qt::ItemIsEnabled | Qt::ItemIsSelectable | drag;
}

QMimeData* FileExplorerItemModel::mimeData(const QModelIndexList& indexes) const {
    if (!m_drag_payload_provider) {
        return nullptr;
    }
    // Collapse the per-column index list into unique, ordered rows.
    QList<int> rows;
    for (const QModelIndex& index : indexes) {
        if (index.isValid() && !rows.contains(index.row())) {
            rows.append(index.row());
        }
    }
    std::sort(rows.begin(), rows.end());
    return rows.isEmpty() ? nullptr : m_drag_payload_provider(rows);
}

Qt::DropActions FileExplorerItemModel::supportedDragActions() const {
    return Qt::CopyAction | Qt::MoveAction;
}

bool FileExplorerItemModel::setData(const QModelIndex& index,
                                    const QVariant& value,
                                    const int role) {
    if (role == Qt::CheckStateRole && index.column() == NameColumn && hasEntry(index.row()) &&
        m_check_toggle_handler) {
        // Files checkbox click: (de)selects the item; the panel owns the
        // selection model, so the toggle routes through the handler.
        m_check_toggle_handler(m_entries.at(index.row()).path,
                               value.value<Qt::CheckState>() == Qt::Checked);
        return true;
    }
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
    // Remember which entry each persistent index points at (by its unique path)
    // BEFORE the permutation, so the selection, the current index, and any open
    // editor follow their items instead of aliasing whatever row later lands at
    // that number. Emitting layoutChanged without remapping persistent indexes
    // violates the model contract and silently retargets the selection.
    const QModelIndexList old_indexes = persistentIndexList();
    QStringList held_paths;
    held_paths.reserve(old_indexes.size());
    for (const QModelIndex& held : old_indexes) {
        held_paths.append(hasEntry(held.row()) ? m_entries.at(held.row()).path : QString());
    }

    std::stable_sort(m_entries.begin(),
                     m_entries.end(),
                     [column, order](const auto& left, const auto& right) {
                         if (left.directory != right.directory) {
                             return left.directory;
                         }
                         const int compared = compareEntries(left, right, column);
                         return order == Qt::AscendingOrder ? compared < 0 : compared > 0;
                     });

    remapPersistentIndexesByPath(old_indexes, held_paths);
    Q_EMIT layoutChanged();
}

void FileExplorerItemModel::remapPersistentIndexesByPath(const QModelIndexList& old_indexes,
                                                         const QStringList& held_paths) {
    QHash<QString, int> path_to_row;
    path_to_row.reserve(m_entries.size());
    for (int row = 0; row < m_entries.size(); ++row) {
        path_to_row.insert(m_entries.at(row).path, row);
    }
    QModelIndexList new_indexes;
    new_indexes.reserve(old_indexes.size());
    for (int i = 0; i < old_indexes.size(); ++i) {
        const int new_row = path_to_row.value(held_paths.at(i), -1);
        new_indexes.append(new_row < 0 ? QModelIndex()
                                       : index(new_row, old_indexes.at(i).column()));
    }
    changePersistentIndexList(old_indexes, new_indexes);
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

void FileExplorerItemModel::setDragPayloadProvider(DragPayloadProvider provider) {
    m_drag_payload_provider = std::move(provider);
}

QVariant FileExplorerItemModel::checkStateForEntry(const FileManagementEntry& entry,
                                                   const int column) const {
    // Files selection checkboxes: the box mirrors the selection state.
    if (column != NameColumn || !m_checkboxes_visible || !m_check_state_provider) {
        return {};
    }
    return m_check_state_provider(entry.path) ? Qt::Checked : Qt::Unchecked;
}

void FileExplorerItemModel::setCheckboxProviders(CheckStateProvider provider,
                                                 CheckToggleHandler handler) {
    m_check_state_provider = std::move(provider);
    m_check_toggle_handler = std::move(handler);
    refreshChecks();
}

void FileExplorerItemModel::setCheckboxesVisible(const bool visible) {
    if (m_checkboxes_visible == visible) {
        return;
    }
    m_checkboxes_visible = visible;
    refreshChecks();
}

bool FileExplorerItemModel::checkboxesVisible() const {
    return m_checkboxes_visible;
}

void FileExplorerItemModel::refreshChecks() {
    if (m_entries.isEmpty()) {
        return;
    }
    Q_EMIT dataChanged(index(0, NameColumn),
                       index(m_entries.size() - 1, NameColumn),
                       {Qt::CheckStateRole});
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
