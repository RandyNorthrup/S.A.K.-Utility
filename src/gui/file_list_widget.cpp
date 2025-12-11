#include "gui/file_list_widget.h"

#include <QMenu>
#include <QAction>
#include <QContextMenuEvent>
#include <QMouseEvent>
#include <QDesktopServices>
#include <QUrl>
#include <QClipboard>
#include <QApplication>
#include <QFileIconProvider>
#include <algorithm>

namespace sak {

// ============================================================================
// FileListModel Implementation
// ============================================================================

FileListModel::FileListModel(QObject* parent)
    : QAbstractItemModel(parent)
{
}

QModelIndex FileListModel::index(int row, int column, const QModelIndex& parent) const
{
    if (parent.isValid() || row < 0 || row >= static_cast<int>(m_files.size()) || 
        column < 0 || column >= ColumnCount) {
        return {};
    }
    
    return createIndex(row, column, nullptr);
}

QModelIndex FileListModel::parent([[maybe_unused]] const QModelIndex& index) const
{
    // Flat list - no parent
    return {};
}

int FileListModel::rowCount(const QModelIndex& parent) const
{
    return parent.isValid() ? 0 : static_cast<int>(m_files.size());
}

int FileListModel::columnCount([[maybe_unused]] const QModelIndex& parent) const
{
    return ColumnCount;
}

QVariant FileListModel::data(const QModelIndex& index, int role) const
{
    if (!index.isValid() || index.row() >= static_cast<int>(m_files.size())) {
        return {};
    }

    const auto& file = m_files[static_cast<size_t>(index.row())];

    if (role == Qt::DisplayRole || role == Qt::EditRole) {
        switch (index.column()) {
            case NameColumn:
                return QString::fromStdString(file.path.filename().string());
            case SizeColumn:
                return file.is_directory ? QVariant() : QVariant::fromValue(file.size);
            case TypeColumn:
                return file.type;
            case ModifiedColumn:
                return file.modified.toString("yyyy-MM-dd hh:mm:ss");
            default:
                return {};
        }
    }

    if (role == Qt::ToolTipRole) {
        return QString::fromStdString(file.path.string());
    }

    if (role == Qt::DecorationRole && index.column() == NameColumn) {
        static QFileIconProvider iconProvider;
        QFileInfo qFileInfo(QString::fromStdString(file.path.string()));
        return iconProvider.icon(qFileInfo);
    }

    return {};
}

QVariant FileListModel::headerData(int section, Qt::Orientation orientation, int role) const
{
    if (orientation != Qt::Horizontal || role != Qt::DisplayRole) {
        return {};
    }

    switch (section) {
        case NameColumn: return tr("Name");
        case SizeColumn: return tr("Size");
        case TypeColumn: return tr("Type");
        case ModifiedColumn: return tr("Modified");
        default: return {};
    }
}

void FileListModel::sort(int column, Qt::SortOrder order)
{
    if (m_files.empty()) {
        return;
    }

    Q_EMIT layoutAboutToBeChanged();

    m_sortColumn = column;
    m_sortOrder = order;

    auto comparator = [column, order](const FileInfo& a, const FileInfo& b) -> bool {
        bool result = false;
        switch (column) {
            case NameColumn:
                result = a.path.filename().string() < b.path.filename().string();
                break;
            case SizeColumn:
                result = a.size < b.size;
                break;
            case TypeColumn:
                result = a.type < b.type;
                break;
            case ModifiedColumn:
                result = a.modified < b.modified;
                break;
            default:
                result = false;
        }
        return order == Qt::AscendingOrder ? result : !result;
    };

    std::sort(m_files.begin(), m_files.end(), comparator);

    Q_EMIT layoutChanged();
}

void FileListModel::setFiles(std::vector<FileInfo> files)
{
    beginResetModel();
    m_files = std::move(files);
    endResetModel();

    // Apply current sort
    if (!m_files.empty()) {
        sort(m_sortColumn, m_sortOrder);
    }
}

void FileListModel::addFile(const FileInfo& file)
{
    int row = static_cast<int>(m_files.size());
    beginInsertRows(QModelIndex(), row, row);
    m_files.push_back(file);
    endInsertRows();
}

void FileListModel::clear()
{
    beginResetModel();
    m_files.clear();
    endResetModel();
}

const FileInfo* FileListModel::getFileInfo(const QModelIndex& index) const
{
    if (!index.isValid() || index.row() >= static_cast<int>(m_files.size())) {
        return nullptr;
    }
    return &m_files[static_cast<size_t>(index.row())];
}

// ============================================================================
// SizeDelegate Implementation
// ============================================================================

SizeDelegate::SizeDelegate(QObject* parent)
    : QStyledItemDelegate(parent)
{
}

QString SizeDelegate::displayText(const QVariant& value, [[maybe_unused]] const QLocale& locale) const
{
    bool ok = false;
    qint64 size = value.toLongLong(&ok);
    
    if (!ok || size < 0) {
        return QString();
    }

    // Format size in human-readable format
    const qint64 KB = 1024;
    const qint64 MB = KB * 1024;
    const qint64 GB = MB * 1024;

    if (size >= GB) {
        return QString::number(size / static_cast<double>(GB), 'f', 2) + " GB";
    } else if (size >= MB) {
        return QString::number(size / static_cast<double>(MB), 'f', 2) + " MB";
    } else if (size >= KB) {
        return QString::number(size / static_cast<double>(KB), 'f', 2) + " KB";
    } else {
        return QString::number(size) + " bytes";
    }
}

// ============================================================================
// FileListWidget Implementation
// ============================================================================

FileListWidget::FileListWidget(QWidget* parent)
    : QTreeView(parent)
{
    setupUI();
}

void FileListWidget::setupUI()
{
    // Create model
    m_model = new FileListModel(this);
    setModel(m_model);

    // Create and set size delegate
    m_sizeDelegate = new SizeDelegate(this);
    setItemDelegateForColumn(FileListModel::SizeColumn, m_sizeDelegate);

    // Configure view
    setRootIsDecorated(false);
    setAlternatingRowColors(true);
    setSelectionMode(QAbstractItemView::ExtendedSelection);
    setSelectionBehavior(QAbstractItemView::SelectRows);
    setSortingEnabled(true);
    setUniformRowHeights(true); // Performance optimization
    
    // Configure header
    header()->setStretchLastSection(false);
    header()->setSectionResizeMode(FileListModel::NameColumn, QHeaderView::Stretch);
    header()->setSectionResizeMode(FileListModel::SizeColumn, QHeaderView::ResizeToContents);
    header()->setSectionResizeMode(FileListModel::TypeColumn, QHeaderView::ResizeToContents);
    header()->setSectionResizeMode(FileListModel::ModifiedColumn, QHeaderView::ResizeToContents);
    header()->setSortIndicatorShown(true);
    header()->setSectionsClickable(true);

    // Connect signals
    connect(selectionModel(), &QItemSelectionModel::selectionChanged, 
            this, &FileListWidget::onSelectionChanged);
}

void FileListWidget::setFiles(std::vector<FileInfo> files)
{
    m_model->setFiles(std::move(files));
    Q_EMIT selectionChanged(0);
}

void FileListWidget::addFile(const FileInfo& file)
{
    m_model->addFile(file);
}

void FileListWidget::clear()
{
    m_model->clear();
    Q_EMIT selectionChanged(0);
}

std::vector<FileInfo> FileListWidget::getSelectedFiles() const
{
    std::vector<FileInfo> selected;
    const auto indexes = selectionModel()->selectedRows();
    
    selected.reserve(static_cast<size_t>(indexes.size()));
    
    for (const auto& index : indexes) {
        if (const auto* fileInfo = m_model->getFileInfo(index)) {
            selected.push_back(*fileInfo);
        }
    }
    
    return selected;
}

std::vector<FileInfo> FileListWidget::getAllFiles() const
{
    std::vector<FileInfo> all;
    const int count = m_model->rowCount();
    
    all.reserve(static_cast<size_t>(count));
    
    for (int i = 0; i < count; ++i) {
        auto index = m_model->index(i, 0);
        if (const auto* fileInfo = m_model->getFileInfo(index)) {
            all.push_back(*fileInfo);
        }
    }
    
    return all;
}

size_t FileListWidget::fileCount() const
{
    return m_model->fileCount();
}

void FileListWidget::contextMenuEvent(QContextMenuEvent* event)
{
    createContextMenu(event->pos());
}

void FileListWidget::mouseDoubleClickEvent(QMouseEvent* event)
{
    QTreeView::mouseDoubleClickEvent(event);
    
    const QModelIndex index = indexAt(event->pos());
    if (index.isValid()) {
        if (const auto* fileInfo = m_model->getFileInfo(index)) {
            Q_EMIT fileDoubleClicked(*fileInfo);
        }
    }
}

void FileListWidget::onSelectionChanged()
{
    const int count = selectionModel()->selectedRows().count();
    Q_EMIT selectionChanged(count);
}

void FileListWidget::createContextMenu(const QPoint& pos)
{
    const QModelIndex index = indexAt(pos);
    if (!index.isValid()) {
        return;
    }

    QMenu menu(this);

    // Open action
    auto* openAction = menu.addAction(tr("Open"));
    connect(openAction, &QAction::triggered, this, [this, index]() {
        if (const auto* fileInfo = m_model->getFileInfo(index)) {
            QString path = QString::fromStdString(fileInfo->path.string());
            QDesktopServices::openUrl(QUrl::fromLocalFile(path));
        }
    });

    // Open containing folder
    auto* openFolderAction = menu.addAction(tr("Open Containing Folder"));
    connect(openFolderAction, &QAction::triggered, this, [this, index]() {
        if (const auto* fileInfo = m_model->getFileInfo(index)) {
            QString dirPath = QString::fromStdString(fileInfo->path.parent_path().string());
            QDesktopServices::openUrl(QUrl::fromLocalFile(dirPath));
        }
    });

    menu.addSeparator();

    // Copy path
    auto* copyPathAction = menu.addAction(tr("Copy Path"));
    connect(copyPathAction, &QAction::triggered, this, [this, index]() {
        if (const auto* fileInfo = m_model->getFileInfo(index)) {
            QString path = QString::fromStdString(fileInfo->path.string());
            QApplication::clipboard()->setText(path);
        }
    });

    // Copy name
    auto* copyNameAction = menu.addAction(tr("Copy Name"));
    connect(copyNameAction, &QAction::triggered, this, [this, index]() {
        if (const auto* fileInfo = m_model->getFileInfo(index)) {
            QString name = QString::fromStdString(fileInfo->path.filename().string());
            QApplication::clipboard()->setText(name);
        }
    });

    menu.exec(viewport()->mapToGlobal(pos));
}

} // namespace sak
