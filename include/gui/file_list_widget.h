#pragma once

#include <QAbstractItemModel>
#include <QTreeView>
#include <QHeaderView>
#include <QStyledItemDelegate>
#include <QDateTime>
#include <vector>
#include <memory>
#include <filesystem>

namespace sak {

/**
 * @brief File information structure
 */
struct FileInfo {
    std::filesystem::path path;
    qint64 size{0};
    QString type;
    QDateTime modified;
    bool is_directory{false};
    
    FileInfo() = default;
    explicit FileInfo(const std::filesystem::path& p) : path(p) {}
};

/**
 * @brief High-performance file list model
 * 
 * Optimized for displaying 10k+ files with:
 * - Lazy loading
 * - Efficient sorting
 * - Minimal memory footprint
 * - Virtual scrolling support
 */
class FileListModel : public QAbstractItemModel {
    Q_OBJECT

public:
    enum Column {
        NameColumn = 0,
        SizeColumn,
        TypeColumn,
        ModifiedColumn,
        ColumnCount
    };

    explicit FileListModel(QObject* parent = nullptr);
    ~FileListModel() override = default;

    // QAbstractItemModel interface
    [[nodiscard]] QModelIndex index(int row, int column, const QModelIndex& parent = QModelIndex()) const override;
    [[nodiscard]] QModelIndex parent(const QModelIndex& index) const override;
    [[nodiscard]] int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    [[nodiscard]] int columnCount(const QModelIndex& parent = QModelIndex()) const override;
    [[nodiscard]] QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
    [[nodiscard]] QVariant headerData(int section, Qt::Orientation orientation, int role = Qt::DisplayRole) const override;
    
    void sort(int column, Qt::SortOrder order = Qt::AscendingOrder) override;

    // Custom methods
    void setFiles(std::vector<FileInfo> files);
    void addFile(const FileInfo& file);
    void clear();
    [[nodiscard]] const FileInfo* getFileInfo(const QModelIndex& index) const;
    [[nodiscard]] size_t fileCount() const { return m_files.size(); }

private:
    std::vector<FileInfo> m_files;
    int m_sortColumn{NameColumn};
    Qt::SortOrder m_sortOrder{Qt::AscendingOrder};
};

/**
 * @brief Size formatting delegate
 * 
 * Displays file sizes in human-readable format (KB, MB, GB)
 */
class SizeDelegate : public QStyledItemDelegate {
    Q_OBJECT

public:
    explicit SizeDelegate(QObject* parent = nullptr);
    ~SizeDelegate() override = default;

    [[nodiscard]] QString displayText(const QVariant& value, const QLocale& locale) const override;
};

/**
 * @brief High-performance file list widget
 * 
 * Optimized tree view for displaying large file lists with:
 * - Virtual scrolling (handles 100k+ items)
 * - Efficient sorting and filtering
 * - Context menu support
 * - Multi-selection
 * - Column resizing and reordering
 */
class FileListWidget : public QTreeView {
    Q_OBJECT

public:
    explicit FileListWidget(QWidget* parent = nullptr);
    ~FileListWidget() override = default;

    FileListWidget(const FileListWidget&) = delete;
    FileListWidget& operator=(const FileListWidget&) = delete;
    FileListWidget(FileListWidget&&) = delete;
    FileListWidget& operator=(FileListWidget&&) = delete;

    /**
     * @brief Set files to display
     * @param files Vector of file information
     */
    void setFiles(std::vector<FileInfo> files);

    /**
     * @brief Add single file to list
     * @param file File information
     */
    void addFile(const FileInfo& file);

    /**
     * @brief Clear all files
     */
    void clear();

    /**
     * @brief Get selected files
     * @return Vector of selected file info
     */
    [[nodiscard]] std::vector<FileInfo> getSelectedFiles() const;

    /**
     * @brief Get all files
     * @return Vector of all file info
     */
    [[nodiscard]] std::vector<FileInfo> getAllFiles() const;

    /**
     * @brief Get file count
     * @return Number of files in list
     */
    [[nodiscard]] size_t fileCount() const;

Q_SIGNALS:
    /**
     * @brief Emitted when selection changes
     * @param count Number of selected files
     */
    void selectionChanged(int count);

    /**
     * @brief Emitted when file is double-clicked
     * @param file File information
     */
    void fileDoubleClicked(const FileInfo& file);

protected:
    void contextMenuEvent(QContextMenuEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;

private Q_SLOTS:
    void onSelectionChanged();

private:
    void setupUI();
    void createContextMenu(const QPoint& pos);

    FileListModel* m_model{nullptr};
    SizeDelegate* m_sizeDelegate{nullptr};
};

} // namespace sak
