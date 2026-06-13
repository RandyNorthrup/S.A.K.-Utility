// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file file_management_explorer_panel.cpp
/// @brief File Management explorer tab with mounted and raw/image targets.

#include "sak/file_management_explorer_panel.h"

#include "sak/file_explorer_icon_registry.h"
#include "sak/layout_constants.h"
#include "sak/message_box_helpers.h"
#include "sak/storage_inventory_worker.h"
#include "sak/style_constants.h"
#include "sak/widget_helpers.h"

#include <QApplication>
#include <QAction>
#include <QActionGroup>
#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QCryptographicHash>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QFutureWatcher>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QItemSelection>
#include <QItemSelectionModel>
#include <QListWidgetItem>
#include <QMenu>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QShortcut>
#include <QSettings>
#include <QSlider>
#include <QSplitter>
#include <QStyle>
#include <QTableView>
#include <QTimer>
#include <QToolButton>
#include <QVariant>
#include <QVBoxLayout>
#include <QWidgetAction>
#include <QtConcurrent>

#include <algorithm>
#include <utility>

namespace sak {

namespace {

constexpr int kExplorerPreviewMaxBytes = 1024 * 1024;
constexpr int kExplorerListMaxEntries = 10'000;
constexpr qint64 kExplorerWriteMaxBytes = 64LL * 1024LL * 1024LL;
constexpr int kSidebarKindRole = Qt::UserRole + 1;
constexpr int kTargetIndexRole = Qt::UserRole + 2;
constexpr int kCommandIdRole = Qt::UserRole + 3;
constexpr int kCommandEnabledRole = Qt::UserRole + 4;
constexpr int kCommandBlockerRole = Qt::UserRole + 5;
constexpr const char* kExplorerSettingsGroup = "FileManagementExplorer";
constexpr const char* kFavoriteTargetIdsKey = "FavoriteTargetIds";
constexpr const char* kRecentTargetIdsKey = "RecentTargetIds";
constexpr const char* kLastTargetIdKey = "LastTargetId";
constexpr const char* kViewModeKey = "ViewMode";
constexpr const char* kShowHiddenKey = "ShowHiddenItems";
constexpr const char* kShowExtensionsKey = "ShowFileExtensions";
constexpr const char* kItemSizeKey = "ItemSizePx";

enum class SidebarEntryKind {
    Header = 0,
    Target = 1,
    Home = 2,
};

QString fileSystemBadge(const FileManagementTarget& target) {
    const QString fs = target.file_system.trimmed();
    return fs.isEmpty() ? QStringLiteral("unknown") : fs;
}

QString targetSubtitle(const FileManagementTarget& target) {
    QStringList parts;
    parts.append(fileSystemBadge(target));
    parts.append(target.local_file_system ? QStringLiteral("mounted")
                                          : QStringLiteral("raw/image"));
    if (!target.source.trimmed().isEmpty()) {
        parts.append(target.source.trimmed());
    }
    parts.removeDuplicates();
    return parts.join(QStringLiteral(" - "));
}

bool targetMatchesFileSystem(const FileManagementTarget& target, const QStringList& systems) {
    const QString fs = target.file_system.trimmed().toLower();
    for (const QString& system : systems) {
        if (fs == system.toLower()) {
            return true;
        }
    }
    return false;
}

QString targetBadge(const FileManagementTarget& target) {
    if (!target.blockers.isEmpty() && !target.can_browse) {
        return QStringLiteral("Blocked");
    }
    if (target.can_write_files && !target.local_file_system &&
        targetMatchesFileSystem(target,
                                {QStringLiteral("apfs"),
                                 QStringLiteral("hfs+"),
                                 QStringLiteral("hfsx")})) {
        return QStringLiteral("Write certified");
    }
    if (target.read_only || !target.can_write_files) {
        return QStringLiteral("Read-only");
    }
    return QStringLiteral("Writable");
}

QString parentPathFor(const QString& path, bool local) {
    if (path.trimmed().isEmpty() || path == QStringLiteral("/")) {
        return path;
    }
    if (local) {
        const QDir dir(path);
        return dir.absoluteFilePath(QStringLiteral(".."));
    }
    const QString trimmed = path.endsWith(QLatin1Char('/')) && path.size() > 1
                                ? path.left(path.size() - 1)
                                : path;
    const int slash = trimmed.lastIndexOf(QLatin1Char('/'));
    if (slash <= 0) {
        return QStringLiteral("/");
    }
    return trimmed.left(slash);
}

QString childPathFor(QString base, QString name, bool local) {
    name = name.trimmed();
    if (local) {
        return QDir(base).absoluteFilePath(name);
    }
    base.replace(QLatin1Char('\\'), QLatin1Char('/'));
    if (base.trimmed().isEmpty()) {
        base = QStringLiteral("/");
    }
    if (!base.startsWith(QLatin1Char('/'))) {
        base.prepend(QLatin1Char('/'));
    }
    if (!base.endsWith(QLatin1Char('/'))) {
        base.append(QLatin1Char('/'));
    }
    return base + name;
}

QString parentPathForEntry(const QString& path, bool local) {
    return local ? QFileInfo(path).absolutePath() : parentPathFor(path, false);
}

QString nameForPath(const QString& path, bool local) {
    if (local) {
        return QFileInfo(path).fileName();
    }
    QString clean = path;
    clean.replace(QLatin1Char('\\'), QLatin1Char('/'));
    if (clean.endsWith(QLatin1Char('/')) && clean.size() > 1) {
        clean.chop(1);
    }
    const int slash = clean.lastIndexOf(QLatin1Char('/'));
    return slash >= 0 ? clean.mid(slash + 1) : clean;
}

bool isSafeChildName(const QString& name) {
    const QString clean = name.trimmed();
    return !clean.isEmpty() && !clean.contains(QLatin1Char('/')) &&
           !clean.contains(QLatin1Char('\\'));
}

QString viewModeName(const FileExplorerViewMode mode) {
    switch (mode) {
    case FileExplorerViewMode::Details:
        return QStringLiteral("details");
    case FileExplorerViewMode::List:
        return QStringLiteral("list");
    case FileExplorerViewMode::Grid:
        return QStringLiteral("grid");
    case FileExplorerViewMode::Cards:
        return QStringLiteral("cards");
    case FileExplorerViewMode::Columns:
        return QStringLiteral("columns");
    case FileExplorerViewMode::Adaptive:
        return QStringLiteral("adaptive");
    }
    return QStringLiteral("details");
}

FileExplorerViewMode viewModeFromName(const QString& value) {
    const QString clean = value.trimmed().toLower();
    if (clean == QStringLiteral("list")) {
        return FileExplorerViewMode::List;
    }
    if (clean == QStringLiteral("grid")) {
        return FileExplorerViewMode::Grid;
    }
    if (clean == QStringLiteral("cards")) {
        return FileExplorerViewMode::Cards;
    }
    if (clean == QStringLiteral("columns")) {
        return FileExplorerViewMode::Columns;
    }
    if (clean == QStringLiteral("adaptive")) {
        return FileExplorerViewMode::Adaptive;
    }
    return FileExplorerViewMode::Details;
}

FileExplorerViewMode modeForCommand(const FileExplorerCommandId command) {
    switch (command) {
    case FileExplorerCommandId::ViewList:
        return FileExplorerViewMode::List;
    case FileExplorerCommandId::ViewGrid:
        return FileExplorerViewMode::Grid;
    case FileExplorerCommandId::ViewCards:
        return FileExplorerViewMode::Cards;
    case FileExplorerCommandId::ViewColumns:
        return FileExplorerViewMode::Columns;
    case FileExplorerCommandId::ViewAdaptive:
        return FileExplorerViewMode::Adaptive;
    case FileExplorerCommandId::ViewDetails:
    default:
        return FileExplorerViewMode::Details;
    }
}

QString locationViewSettingsGroup(const FileExplorerLocation& location) {
    const QString raw =
        QStringLiteral("%1\n%2").arg(location.target_id.value, location.path);
    const QByteArray digest =
        QCryptographicHash::hash(raw.toUtf8(), QCryptographicHash::Sha256).toHex();
    return QStringLiteral("View/%1").arg(QString::fromLatin1(digest.left(24)));
}

void selectRowInView(QAbstractItemView* view, const int row) {
    if (!view || !view->model() || !view->selectionModel() || row < 0 ||
        row >= view->model()->rowCount()) {
        return;
    }

    const QModelIndex left = view->model()->index(row, 0);
    const QModelIndex right = view->model()->index(row, view->model()->columnCount() - 1);
    view->selectionModel()->select(QItemSelection(left, right),
                                   QItemSelectionModel::ClearAndSelect |
                                       QItemSelectionModel::Rows);
    view->setCurrentIndex(left);
}

void resetPaneNavigationPreservingView(FileExplorerPaneState* state) {
    if (!state) {
        return;
    }
    const FileExplorerViewSettings view_settings = state->view;
    *state = {};
    state->view = view_settings;
}

}  // namespace

FileManagementExplorerPanel::FileManagementExplorerPanel(QWidget* parent) : QWidget(parent) {
    setupUi();
    loadSidebarState();
    setTargets(FileManagementFileSystemBridge::mountedTargets());
}

void FileManagementExplorerPanel::setupUi() {
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(
        ui::kMarginSmall, ui::kMarginSmall, ui::kMarginSmall, ui::kMarginSmall);
    layout->setSpacing(ui::kSpacingDefault);

    m_shell_splitter = new QSplitter(Qt::Horizontal, this);
    m_shell_splitter->setChildrenCollapsible(false);
    layout->addWidget(m_shell_splitter, 1);

    m_sidebar = new FileExplorerSidebar(m_shell_splitter);
    m_target_list = m_sidebar->targetList();
    m_shell_splitter->addWidget(m_sidebar);

    auto* center = new QWidget(m_shell_splitter);
    auto* centerLayout = new QVBoxLayout(center);
    centerLayout->setContentsMargins(0, 0, 0, 0);
    centerLayout->setSpacing(ui::kSpacingSmall);
    m_shell_splitter->addWidget(center);

    m_command_bar = new FileExplorerCommandBar(center);
    m_sidebar_toggle_button = m_command_bar->sidebarToggleButton();
    m_refresh_button = m_command_bar->refreshButton();
    m_scan_disks_button = m_command_bar->scanDisksButton();
    m_add_manual_button = m_command_bar->addManualButton();
    m_new_folder_button = m_command_bar->newFolderButton();
    m_write_file_button = m_command_bar->writeFileButton();
    m_rename_button = m_command_bar->renameButton();
    m_delete_button = m_command_bar->deleteButton();
    m_view_button = m_command_bar->viewButton();
    m_details_toggle_button = m_command_bar->detailsToggleButton();
    centerLayout->addWidget(m_command_bar);

    m_omnibar = new FileExplorerOmnibar(center);
    m_back_button = m_omnibar->backButton();
    m_forward_button = m_omnibar->forwardButton();
    m_up_button = m_omnibar->upButton();
    m_path_edit = m_omnibar->pathEdit();
    m_search_button = m_omnibar->searchButton();
    m_command_button = m_omnibar->commandButton();
    m_open_button = m_omnibar->openButton();
    m_copy_path_button = m_omnibar->copyPathButton();
    centerLayout->addWidget(m_omnibar);

    m_summary_label = new QLabel(tr("No target selected"), this);
    m_summary_label->setObjectName(QStringLiteral("fileExplorerSummaryLabel"));
    m_summary_label->setWordWrap(true);
    m_summary_label->setAccessibleName(tr("Explorer target summary"));
    m_summary_label->setStyleSheet(ui::paddedStatusTextStyle(ui::kColorTextMuted, ui::kFontSizeNote));
    centerLayout->addWidget(m_summary_label);

    m_pane = new FileExplorerPane(center);
    m_item_model = m_pane->itemModel();
    m_status_label = m_pane->statusLabel();
    centerLayout->addWidget(m_pane, 1);

    m_details_pane = new FileExplorerDetailsPane(m_shell_splitter);
    m_details_tabs = m_details_pane;
    m_preview_text = m_details_pane->previewText();
    m_properties_text = m_details_pane->propertiesText();
    m_safety_text = m_details_pane->safetyText();
    m_evidence_text = m_details_pane->evidenceText();
    m_shell_splitter->addWidget(m_details_tabs);
    m_shell_splitter->setStretchFactor(0, 0);
    m_shell_splitter->setStretchFactor(1, 1);
    m_shell_splitter->setStretchFactor(2, 0);

    connect(m_refresh_button,
            &QPushButton::clicked,
            this,
            &FileManagementExplorerPanel::onRefreshMountedTargets);
    connect(m_scan_disks_button,
            &QPushButton::clicked,
            this,
            &FileManagementExplorerPanel::onScanDiskTargets);
    connect(m_add_manual_button,
            &QPushButton::clicked,
            this,
            &FileManagementExplorerPanel::onAddManualTarget);
    connect(m_sidebar_toggle_button, &QPushButton::clicked, this, [this]() {
        if (m_sidebar) {
            m_sidebar->setVisible(!m_sidebar->isVisible());
        }
    });
    connect(m_details_toggle_button, &QPushButton::clicked, this, [this]() {
        m_details_tabs->setVisible(!m_details_tabs->isVisible());
    });
    connect(m_search_button,
            &QPushButton::clicked,
            this,
            &FileManagementExplorerPanel::promptCurrentFolderFilter);
    connect(m_command_button,
            &QPushButton::clicked,
            this,
            &FileManagementExplorerPanel::showCommandPalette);
    connect(m_target_list,
            &QListWidget::currentRowChanged,
            this,
            &FileManagementExplorerPanel::onTargetChanged);
    connect(m_target_list,
            &QListWidget::customContextMenuRequested,
            this,
            &FileManagementExplorerPanel::onTargetContextMenuRequested);
    connect(m_path_edit,
            &QLineEdit::returnPressed,
            this,
            &FileManagementExplorerPanel::onPathReturnPressed);
    connect(m_back_button,
            &QPushButton::clicked,
            this,
            &FileManagementExplorerPanel::onBackClicked);
    connect(m_forward_button,
            &QPushButton::clicked,
            this,
            &FileManagementExplorerPanel::onForwardClicked);
    connect(m_up_button, &QPushButton::clicked, this, &FileManagementExplorerPanel::onUpClicked);
    connect(m_open_button,
            &QPushButton::clicked,
            this,
            &FileManagementExplorerPanel::onOpenSelected);
    connect(m_copy_path_button,
            &QPushButton::clicked,
            this,
            &FileManagementExplorerPanel::onCopyPathClicked);
    connect(m_new_folder_button,
            &QPushButton::clicked,
            this,
            &FileManagementExplorerPanel::onNewFolderClicked);
    connect(m_write_file_button,
            &QPushButton::clicked,
            this,
            &FileManagementExplorerPanel::onWriteFileClicked);
    connect(m_rename_button,
            &QPushButton::clicked,
            this,
            &FileManagementExplorerPanel::onRenameClicked);
    connect(m_delete_button,
            &QPushButton::clicked,
            this,
            &FileManagementExplorerPanel::onDeleteClicked);
    if (m_pane->sharedSelectionModel()) {
        connect(m_pane->sharedSelectionModel(),
                &QItemSelectionModel::selectionChanged,
                this,
                [this]() {
                    updateActionButtons();
                });
    }
    for (auto* view : m_pane->itemViews()) {
        if (!view) {
            continue;
        }
        connect(view,
                &QAbstractItemView::doubleClicked,
                this,
                &FileManagementExplorerPanel::onItemDoubleClicked);
        connect(view,
                &QWidget::customContextMenuRequested,
                this,
                &FileManagementExplorerPanel::onTableContextMenuRequested);
    }
    connect(m_pane,
            &FileExplorerPane::columnsDirectoryPreviewRequested,
            this,
            &FileManagementExplorerPanel::loadColumnsPreview);
    connect(m_pane,
            &FileExplorerPane::columnsChildActivated,
            this,
            [this](const QString& path) {
                loadDirectory(path);
            });
    installCommandShortcuts();
    updateActionButtons();
}

void FileManagementExplorerPanel::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    const int width = event ? event->size().width() : this->width();
    if (m_sidebar && width < 720) {
        m_sidebar->setVisible(false);
    }
    if (m_details_tabs && width < 920) {
        m_details_tabs->setVisible(false);
    }
}

void FileManagementExplorerPanel::installCommandShortcuts() {
    const QVector<FileExplorerCommandId> panelShortcuts{
        FileExplorerCommandId::Back,
        FileExplorerCommandId::Forward,
        FileExplorerCommandId::Up,
        FileExplorerCommandId::Refresh,
        FileExplorerCommandId::CopyItemPath,
        FileExplorerCommandId::SelectAll,
        FileExplorerCommandId::ClearSelection,
        FileExplorerCommandId::NewFolder,
        FileExplorerCommandId::WriteFile,
        FileExplorerCommandId::Rename,
        FileExplorerCommandId::Delete,
        FileExplorerCommandId::ToggleHiddenItems,
        FileExplorerCommandId::ToggleFileExtensions,
        FileExplorerCommandId::ViewDetails,
        FileExplorerCommandId::ViewList,
        FileExplorerCommandId::ViewGrid,
        FileExplorerCommandId::ViewCards,
        FileExplorerCommandId::ViewColumns,
        FileExplorerCommandId::ViewAdaptive,
        FileExplorerCommandId::TogglePreviewPane,
    };

    for (const FileExplorerCommandId command_id : panelShortcuts) {
        const auto command = FileExplorerCommandRegistry::command(command_id);
        if (command.shortcut.trimmed().isEmpty()) {
            continue;
        }
        auto* shortcut = new QShortcut(QKeySequence(command.shortcut), this);
        shortcut->setContext(Qt::WidgetWithChildrenShortcut);
        connect(shortcut, &QShortcut::activated, this, [this, command_id]() {
            executeCommand(command_id);
        });
    }

    auto* searchShortcut = new QShortcut(QKeySequence(QStringLiteral("Ctrl+F")), this);
    searchShortcut->setContext(Qt::WidgetWithChildrenShortcut);
    connect(searchShortcut,
            &QShortcut::activated,
            this,
            &FileManagementExplorerPanel::promptCurrentFolderFilter);

    auto* paletteShortcut = new QShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+P")), this);
    paletteShortcut->setContext(Qt::WidgetWithChildrenShortcut);
    connect(paletteShortcut,
            &QShortcut::activated,
            this,
            &FileManagementExplorerPanel::showCommandPalette);

    const auto openCommand = FileExplorerCommandRegistry::command(FileExplorerCommandId::Open);
    if (!openCommand.shortcut.trimmed().isEmpty()) {
        auto* openShortcut = new QShortcut(QKeySequence(openCommand.shortcut), m_pane);
        openShortcut->setContext(Qt::WidgetWithChildrenShortcut);
        connect(openShortcut, &QShortcut::activated, this, [this]() {
            executeCommand(FileExplorerCommandId::Open);
        });
    }
}

void FileManagementExplorerPanel::setTargets(QVector<FileManagementTarget> targets) {
    m_targets = std::move(targets);
    m_current_target_index = -1;
    resetPaneNavigationPreservingView(&m_pane_state);
    rebuildTargetList(m_last_target_id);
    if (m_targets.isEmpty()) {
        m_item_model->clear();
        m_summary_label->setText(tr("No target selected"));
        if (m_pane) {
            m_pane->showEmptyState(tr("No File Explorer targets are available."));
        }
        updateDetailsPane();
        updateActionButtons();
        return;
    }

    if (m_current_target_index < 0) {
        selectTargetById(FileExplorerTargetId::fromTarget(m_targets.first()).value);
    }
}

void FileManagementExplorerPanel::appendTarget(const FileManagementTarget& target) {
    const QString target_id = FileExplorerTargetId::fromTarget(target).value;
    m_targets.append(target);
    rebuildTargetList(target_id);
}

void FileManagementExplorerPanel::appendSidebarHeader(const QString& text) {
    auto* item = new QListWidgetItem(text, m_target_list);
    item->setData(kSidebarKindRole, static_cast<int>(SidebarEntryKind::Header));
    item->setFlags(Qt::NoItemFlags);
    item->setToolTip(text);
}

void FileManagementExplorerPanel::appendSidebarTarget(const FileManagementTarget& target,
                                                      const int target_index) {
    const auto icon = target.local_file_system ? QStyle::SP_DriveHDIcon : QStyle::SP_FileIcon;
    const QString label =
        QStringLiteral("%1  [%2]\n%3").arg(target.label, targetBadge(target), targetSubtitle(target));
    auto* item = new QListWidgetItem(style()->standardIcon(icon), label, m_target_list);
    item->setData(kSidebarKindRole, static_cast<int>(SidebarEntryKind::Target));
    item->setData(kTargetIndexRole, target_index);
    item->setToolTip(QStringLiteral("%1\n%2")
                         .arg(target.root_path,
                              FileManagementFileSystemBridge::capabilitySummary(target)));
    if (!target.blockers.isEmpty()) {
        item->setStatusTip(target.blockers.join(QStringLiteral("; ")));
    }
}

void FileManagementExplorerPanel::rebuildTargetList(const QString& preferred_target_id) {
    if (!m_target_list) {
        return;
    }

    const QString current_id = !preferred_target_id.trimmed().isEmpty()
                                   ? preferred_target_id.trimmed()
                                   : (m_current_target_index >= 0 && m_current_target_index < m_targets.size()
                                          ? FileExplorerTargetId::fromTarget(
                                                m_targets.at(m_current_target_index))
                                                .value
                                          : QString());

    m_target_list->blockSignals(true);
    m_target_list->clear();

    appendSidebarHeader(tr("Home"));
    auto* home = new QListWidgetItem(style()->standardIcon(QStyle::SP_DirHomeIcon), tr("Home"), m_target_list);
    home->setData(kSidebarKindRole, static_cast<int>(SidebarEntryKind::Home));
    home->setToolTip(tr("Open the first mounted local target."));

    appendSidebarHeader(tr("Favorites"));
    for (const QString& target_id : m_favorite_target_ids) {
        const int index = targetIndexForId(target_id);
        if (index >= 0) {
            appendSidebarTarget(m_targets.at(index), index);
        }
    }

    appendSidebarHeader(tr("This PC"));
    for (int index = 0; index < m_targets.size(); ++index) {
        if (m_targets.at(index).local_file_system) {
            appendSidebarTarget(m_targets.at(index), index);
        }
    }

    appendSidebarHeader(tr("Mounted Volumes"));
    for (int index = 0; index < m_targets.size(); ++index) {
        const auto& target = m_targets.at(index);
        if (target.local_file_system || target.kind == FileManagementTargetKind::LocalPath) {
            appendSidebarTarget(target, index);
        }
    }

    appendSidebarHeader(tr("Disks and Partitions"));
    for (int index = 0; index < m_targets.size(); ++index) {
        if (m_targets.at(index).kind == FileManagementTargetKind::Partition) {
            appendSidebarTarget(m_targets.at(index), index);
        }
    }

    appendSidebarHeader(tr("Raw Images"));
    for (int index = 0; index < m_targets.size(); ++index) {
        const auto& target = m_targets.at(index);
        if (target.kind == FileManagementTargetKind::ImageFile ||
            (!target.local_file_system && target.kind != FileManagementTargetKind::Partition)) {
            appendSidebarTarget(target, index);
        }
    }

    appendSidebarHeader(tr("Recent"));
    for (const QString& target_id : m_recent_target_ids) {
        const int index = targetIndexForId(target_id);
        if (index >= 0) {
            appendSidebarTarget(m_targets.at(index), index);
        }
    }

    appendSidebarHeader(tr("Certification Targets"));
    for (int index = 0; index < m_targets.size(); ++index) {
        const auto& target = m_targets.at(index);
        if (!target.local_file_system && target.can_write_files &&
            targetMatchesFileSystem(target,
                                    {QStringLiteral("apfs"),
                                     QStringLiteral("hfs+"),
                                     QStringLiteral("hfsx")})) {
            appendSidebarTarget(target, index);
        }
    }

    m_target_list->blockSignals(false);
    if (!current_id.isEmpty()) {
        selectTargetById(current_id);
    }
}

void FileManagementExplorerPanel::selectTargetById(const QString& target_id) {
    if (!m_target_list || target_id.trimmed().isEmpty()) {
        return;
    }
    for (int row = 0; row < m_target_list->count(); ++row) {
        auto* item = m_target_list->item(row);
        if (!item || item->data(kSidebarKindRole).toInt() !=
                         static_cast<int>(SidebarEntryKind::Target)) {
            continue;
        }
        const int index = item->data(kTargetIndexRole).toInt();
        if (index >= 0 && index < m_targets.size() &&
            FileExplorerTargetId::fromTarget(m_targets.at(index)).value == target_id) {
            m_target_list->setCurrentRow(row);
            return;
        }
    }
}

void FileManagementExplorerPanel::rememberRecentTarget(const QString& target_id) {
    const QString clean = target_id.trimmed();
    if (clean.isEmpty()) {
        return;
    }
    m_recent_target_ids.removeAll(clean);
    m_recent_target_ids.prepend(clean);
    while (m_recent_target_ids.size() > 10) {
        m_recent_target_ids.removeLast();
    }
    m_last_target_id = clean;
    saveSidebarState();
}

void FileManagementExplorerPanel::loadSidebarState() {
    QSettings settings;
    settings.beginGroup(QString::fromLatin1(kExplorerSettingsGroup));
    m_favorite_target_ids = settings.value(QString::fromLatin1(kFavoriteTargetIdsKey)).toStringList();
    m_recent_target_ids = settings.value(QString::fromLatin1(kRecentTargetIdsKey)).toStringList();
    m_last_target_id = settings.value(QString::fromLatin1(kLastTargetIdKey)).toString();
    settings.endGroup();
    applyViewSettings();
}

void FileManagementExplorerPanel::saveSidebarState() const {
    QSettings settings;
    settings.beginGroup(QString::fromLatin1(kExplorerSettingsGroup));
    settings.setValue(QString::fromLatin1(kFavoriteTargetIdsKey), m_favorite_target_ids);
    settings.setValue(QString::fromLatin1(kRecentTargetIdsKey), m_recent_target_ids);
    settings.setValue(QString::fromLatin1(kLastTargetIdKey), m_last_target_id);
    settings.endGroup();
}

void FileManagementExplorerPanel::applyViewSettings() {
    if (!m_pane) {
        return;
    }
    m_pane_state.view.item_size_px =
        std::clamp(m_pane_state.view.item_size_px,
                   kFileExplorerItemSizeMin,
                   kFileExplorerItemSizeMax);
    m_pane->setViewMode(m_pane_state.view.mode);
    m_pane->setItemSizePx(m_pane_state.view.item_size_px);
    m_pane->setShowHiddenItems(m_pane_state.view.show_hidden);
    m_pane->setShowFileExtensions(m_pane_state.view.show_extensions);
    if (m_view_button) {
        FileExplorerCommandId iconCommand = FileExplorerCommandId::ViewDetails;
        switch (m_pane_state.view.mode) {
        case FileExplorerViewMode::List:
            iconCommand = FileExplorerCommandId::ViewList;
            break;
        case FileExplorerViewMode::Grid:
        case FileExplorerViewMode::Adaptive:
            iconCommand = FileExplorerCommandId::ViewGrid;
            break;
        case FileExplorerViewMode::Cards:
            iconCommand = FileExplorerCommandId::ViewCards;
            break;
        case FileExplorerViewMode::Columns:
            iconCommand = FileExplorerCommandId::ViewColumns;
            break;
        case FileExplorerViewMode::Details:
            break;
        }
        m_view_button->setIcon(FileExplorerIconRegistry::iconForCommand(iconCommand));
    }
}

void FileManagementExplorerPanel::loadViewSettingsForCurrentLocation() {
    if (m_pane_state.location.isEmpty()) {
        return;
    }
    QSettings settings;
    settings.beginGroup(QString::fromLatin1(kExplorerSettingsGroup));
    settings.beginGroup(locationViewSettingsGroup(m_pane_state.location));
    if (settings.contains(QString::fromLatin1(kViewModeKey))) {
        m_pane_state.view.mode =
            viewModeFromName(settings.value(QString::fromLatin1(kViewModeKey)).toString());
    }
    if (settings.contains(QString::fromLatin1(kShowHiddenKey))) {
        m_pane_state.view.show_hidden =
            settings.value(QString::fromLatin1(kShowHiddenKey)).toBool();
    }
    if (settings.contains(QString::fromLatin1(kShowExtensionsKey))) {
        m_pane_state.view.show_extensions =
            settings.value(QString::fromLatin1(kShowExtensionsKey)).toBool();
    }
    if (settings.contains(QString::fromLatin1(kItemSizeKey))) {
        m_pane_state.view.item_size_px =
            settings.value(QString::fromLatin1(kItemSizeKey)).toInt();
    }
    settings.endGroup();
    settings.endGroup();
    applyViewSettings();
}

void FileManagementExplorerPanel::saveViewSettings() const {
    if (m_pane_state.location.isEmpty()) {
        return;
    }
    QSettings settings;
    settings.beginGroup(QString::fromLatin1(kExplorerSettingsGroup));
    settings.beginGroup(locationViewSettingsGroup(m_pane_state.location));
    settings.setValue(QString::fromLatin1(kViewModeKey), viewModeName(m_pane_state.view.mode));
    settings.setValue(QString::fromLatin1(kShowHiddenKey), m_pane_state.view.show_hidden);
    settings.setValue(QString::fromLatin1(kShowExtensionsKey), m_pane_state.view.show_extensions);
    settings.setValue(QString::fromLatin1(kItemSizeKey), m_pane_state.view.item_size_px);
    settings.endGroup();
    settings.endGroup();
}

void FileManagementExplorerPanel::setExplorerViewMode(const FileExplorerViewMode mode) {
    m_pane_state.view.mode = mode;
    applyViewSettings();
    saveViewSettings();
    Q_EMIT statusMessage(tr("Explorer view switched to %1").arg(viewModeName(mode)),
                         sak::kTimerStatusMessageMs);
    QTimer::singleShot(0, this, [this]() {
        updateActionButtons();
    });
}

QAbstractItemView* FileManagementExplorerPanel::currentItemView() const {
    return m_pane ? m_pane->activeItemView() : nullptr;
}

FileManagementTarget FileManagementExplorerPanel::currentTarget() const {
    if (m_current_target_index < 0 || m_current_target_index >= m_targets.size()) {
        return {};
    }
    return m_targets.at(m_current_target_index);
}

int FileManagementExplorerPanel::targetIndexForId(const QString& target_id) const {
    const QString clean = target_id.trimmed();
    if (clean.isEmpty()) {
        return -1;
    }
    for (int index = 0; index < m_targets.size(); ++index) {
        if (FileExplorerTargetId::fromTarget(m_targets.at(index)).value == clean) {
            return index;
        }
    }
    return -1;
}

QString FileManagementExplorerPanel::selectedPath() const {
    const auto* selection_model = m_pane ? m_pane->sharedSelectionModel() : nullptr;
    if (!selection_model || !m_pane) {
        return {};
    }
    const QModelIndexList rows = selection_model->selectedRows();
    if (rows.isEmpty()) {
        return {};
    }
    return m_pane->entryAtViewRow(rows.first().row()).path;
}

bool FileManagementExplorerPanel::selectedIsDirectory() const {
    const auto* selection_model = m_pane ? m_pane->sharedSelectionModel() : nullptr;
    if (!selection_model || !m_pane) {
        return false;
    }
    const QModelIndexList rows = selection_model->selectedRows();
    if (rows.isEmpty()) {
        return false;
    }
    return m_pane->entryAtViewRow(rows.first().row()).directory;
}

QString FileManagementExplorerPanel::targetPathForName(const QString& name) const {
    const auto target = currentTarget();
    if (!isSafeChildName(name)) {
        return {};
    }
    return childPathFor(m_current_path, name, target.local_file_system);
}

bool FileManagementExplorerPanel::validateCurrentTargetIdentity(QString* blocker) const {
    const auto target = currentTarget();
    const QString target_id = FileExplorerTargetId::fromTarget(target).value;
    if (target_id.trimmed().isEmpty() || target.root_path.trimmed().isEmpty()) {
        if (blocker) {
            *blocker = tr("No stable File Explorer target identity is selected.");
        }
        return false;
    }
    if (m_pane_state.location.target_id.value != target_id) {
        if (blocker) {
            *blocker = tr("Selected target identity changed. Refresh target and retry.");
        }
        return false;
    }
    return true;
}

void FileManagementExplorerPanel::loadDirectory(const QString& path, const bool add_history) {
    const auto target = currentTarget();
    if (target.root_path.isEmpty()) {
        ++m_listing_revision;
        ++m_columns_preview_revision;
        if (m_pane) {
            m_pane->showEmptyState(tr("No File Explorer target selected."));
            m_pane->clearColumnsPreview();
        }
        updateDetailsPane();
        updateActionButtons();
        return;
    }
    if (!target.can_browse) {
        ++m_listing_revision;
        ++m_columns_preview_revision;
        m_summary_label->setText(target.blockers.join(QStringLiteral("; ")));
        m_item_model->clear();
        if (m_pane) {
            m_pane->showErrorState(target.blockers.join(QStringLiteral("; ")));
            m_pane->clearColumnsPreview();
        }
        updateDetailsPane();
        updateActionButtons();
        return;
    }
    const QString requested_path = path.trimmed().isEmpty()
                                       ? (target.local_file_system ? target.root_path
                                                                   : QStringLiteral("/"))
                                       : path.trimmed();
    FileExplorerLocation destination;
    destination.target_id = FileExplorerTargetId::fromTarget(target);
    destination.path = requested_path;
    if (add_history) {
        m_pane_state.navigateTo(destination, target.local_file_system);
    } else {
        m_pane_state.location = destination.normalized(target.local_file_system);
        m_pane_state.selection.clear();
    }
    m_current_path = m_pane_state.location.path;
    loadViewSettingsForCurrentLocation();
    m_path_edit->setText(m_current_path);
    if (m_preview_text) {
        m_preview_text->setPlainText(tr("Select a readable file and choose Preview."));
    }

    const quint64 listing_revision = ++m_listing_revision;
    ++m_columns_preview_revision;
    if (m_pane) {
        m_pane->clearColumnsPreview();
        m_pane->showLoadingState(tr("Loading %1...").arg(m_current_path));
    }
    m_summary_label->setText(tr("Loading %1...").arg(m_current_path));
    updateActionButtons();

    auto* watcher = new QFutureWatcher<FileManagementListResult>(this);
    connect(watcher, &QFutureWatcher<FileManagementListResult>::finished, this, [this, watcher, listing_revision]() {
        watcher->deleteLater();
        if (listing_revision != m_listing_revision) {
            return;
        }
        populateTable(watcher->result());
    });
    watcher->setFuture(QtConcurrent::run([target, path = m_current_path]() {
        return FileManagementFileSystemBridge::listDirectory(target, path, kExplorerListMaxEntries);
    }));
}

void FileManagementExplorerPanel::loadColumnsPreview(const QString& path) {
    if (!m_pane) {
        return;
    }
    const auto target = currentTarget();
    if (target.root_path.isEmpty() || !target.can_browse || path.trimmed().isEmpty()) {
        m_pane->clearColumnsPreview();
        return;
    }

    const QString requested_path = path.trimmed();
    const QString target_id = FileExplorerTargetId::fromTarget(target).value;
    const quint64 preview_revision = ++m_columns_preview_revision;
    auto* watcher = new QFutureWatcher<FileManagementListResult>(this);
    connect(watcher,
            &QFutureWatcher<FileManagementListResult>::finished,
            this,
            [this, watcher, preview_revision, requested_path, target_id]() {
                watcher->deleteLater();
                if (preview_revision != m_columns_preview_revision || !m_pane) {
                    return;
                }
                const auto current = currentTarget();
                if (FileExplorerTargetId::fromTarget(current).value != target_id) {
                    return;
                }
                const FileManagementListResult result = watcher->result();
                if (!result.ok) {
                    m_pane->clearColumnsPreview();
                    return;
                }
                m_pane->setColumnsPreviewEntries(requested_path, result.entries);
            });
    watcher->setFuture(QtConcurrent::run([target, requested_path]() {
        return FileManagementFileSystemBridge::listDirectory(target, requested_path, kExplorerListMaxEntries);
    }));
}

void FileManagementExplorerPanel::populateTable(const FileManagementListResult& result) {
    m_item_model->clear();
    if (!result.ok) {
        m_summary_label->setText(result.blockers.join(QStringLiteral("; ")));
        if (m_pane) {
            m_pane->showErrorState(result.blockers.join(QStringLiteral("; ")));
        }
        Q_EMIT statusMessage(tr("Explorer listing failed"), sak::kTimerStatusMessageMs);
        updateDetailsPane();
        updateActionButtons();
        return;
    }

    m_item_model->setEntries(result.entries);
    if (m_pane) {
        if (result.entries.isEmpty()) {
            m_pane->showEmptyState(tr("This folder is empty."));
        } else if (m_pane->sortFilterModel() && m_pane->sortFilterModel()->rowCount() == 0) {
            m_pane->showEmptyState(tr("No items match current view settings."));
        } else {
            m_pane->showReadyState();
        }
    }

    const auto target = currentTarget();
    QString summary = tr("%1 item(s) - %2").arg(result.entries.size()).arg(
        FileManagementFileSystemBridge::capabilitySummary(target));
    if (!result.warnings.isEmpty()) {
        summary += tr(" - %1").arg(result.warnings.join(QStringLiteral("; ")));
    }
    m_summary_label->setText(summary);
    Q_EMIT statusMessage(tr("Explorer loaded %1 item(s)").arg(result.entries.size()),
                         sak::kTimerStatusDefaultMs);
    updateDetailsPane();
    updateActionButtons();
}

void FileManagementExplorerPanel::previewSelectedFile() {
    const QString path = selectedPath();
    if (path.isEmpty()) {
        return;
    }
    const auto target = currentTarget();
    const auto read =
        FileManagementFileSystemBridge::readFile(target, path, kExplorerPreviewMaxBytes);
    if (!read.ok) {
        sak::showWarningLogged(this,
                               tr("Preview File"),
                               read.blockers.join(QStringLiteral("\n")));
        return;
    }

    if (m_preview_text) {
        m_preview_text->setPlainText(QString::fromUtf8(read.data));
    }
    if (m_details_tabs && m_preview_text) {
        m_details_tabs->setCurrentWidget(m_preview_text);
    }

    QDialog dialog(this);
    dialog.setWindowTitle(tr("Preview %1").arg(QFileInfo(path).fileName()));
    dialog.resize(sak::kDialogWidthLarge, sak::kDialogHeightLarge);
    auto* layout = new QVBoxLayout(&dialog);
    auto* text = new QPlainTextEdit(&dialog);
    text->setReadOnly(true);
    text->setPlainText(QString::fromUtf8(read.data));
    text->setAccessibleName(tr("File preview contents"));
    layout->addWidget(text);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok, &dialog);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    layout->addWidget(buttons);
    dialog.exec();
}

void FileManagementExplorerPanel::logMessage(const QString& message) {
    if (!message.trimmed().isEmpty()) {
        Q_EMIT logOutput(message);
    }
}

void FileManagementExplorerPanel::showMutationResult(
    const QString& title,
    const FileManagementMutationResult& result) {
    QStringList details;
    details.append(result.blockers);
    details.append(result.warnings);
    if (result.ok) {
        Q_EMIT statusMessage(tr("%1 complete").arg(title), sak::kTimerStatusDefaultMs);
        logMessage(tr("%1: %2").arg(title, result.path));
        return;
    }
    sak::showWarningLogged(this,
                           title,
                           details.isEmpty()
                               ? tr("Operation failed.")
                               : details.join(QStringLiteral("\n")));
}

FileExplorerSelection FileManagementExplorerPanel::currentSelection() const {
    FileExplorerSelection selection;
    const auto* selection_model = m_pane ? m_pane->sharedSelectionModel() : nullptr;
    if (!selection_model || !m_pane) {
        return selection;
    }

    const QModelIndexList rows = selection_model->selectedRows();
    for (const QModelIndex& model_index : rows) {
        selection.entries.append(m_pane->entryAtViewRow(model_index.row()));
    }

    return selection;
}

FileExplorerCommandContext FileManagementExplorerPanel::commandContext() const {
    FileExplorerCommandContext context;
    context.target = currentTarget();
    context.pane = m_pane_state;
    context.pane.selection = currentSelection();
    return context;
}

void FileManagementExplorerPanel::applyCommandState(
    QPushButton* button,
    const FileExplorerCommandId command,
    const FileExplorerCommandContext& context) {
    if (!button) {
        return;
    }

    const FileExplorerCommandState state = FileExplorerCommandRegistry::state(command, context);
    button->setEnabled(state.enabled);
    button->setAccessibleName(state.command.accessible_name);
    button->setToolTip(state.enabled ? state.command.status_text : state.blocker);
}

QAction* FileManagementExplorerPanel::addCommandMenuAction(
    QMenu* menu,
    const FileExplorerCommandId command,
    const FileExplorerCommandContext& context) {
    if (!menu) {
        return nullptr;
    }

    const FileExplorerCommandState state = FileExplorerCommandRegistry::state(command, context);
    const QString text = state.enabled || state.blocker.isEmpty()
                             ? state.command.text
                             : tr("%1 - %2").arg(state.command.text, state.blocker);
    auto* action = menu->addAction(text);
    const QIcon icon = FileExplorerIconRegistry::iconForCommand(command);
    if (!icon.isNull()) {
        action->setIcon(icon);
    }
    action->setEnabled(state.enabled);
    action->setToolTip(state.enabled ? state.command.status_text : state.blocker);
    action->setStatusTip(action->toolTip());
    if (!state.command.shortcut.trimmed().isEmpty()) {
        action->setShortcut(QKeySequence(state.command.shortcut));
    }
    connect(action, &QAction::triggered, this, [this, command]() {
        executeCommand(command);
    });
    return action;
}

void FileManagementExplorerPanel::rebuildViewMenu(const FileExplorerCommandContext& context) {
    if (!m_view_button) {
        return;
    }

    auto* menu = m_view_button->menu();
    if (!menu) {
        menu = new QMenu(m_view_button);
        menu->setObjectName(QStringLiteral("fileExplorerViewMenu"));
        m_view_button->setMenu(menu);
    }
    menu->clear();

    auto* viewGroup = new QActionGroup(menu);
    viewGroup->setExclusive(true);
    for (const FileExplorerCommandId command : {FileExplorerCommandId::ViewDetails,
                                                FileExplorerCommandId::ViewList,
                                                FileExplorerCommandId::ViewGrid,
                                                FileExplorerCommandId::ViewCards,
                                                FileExplorerCommandId::ViewColumns,
                                                FileExplorerCommandId::ViewAdaptive}) {
        auto* action = addCommandMenuAction(menu, command, context);
        if (!action) {
            continue;
        }
        action->setCheckable(true);
        action->setChecked(modeForCommand(command) == m_pane_state.view.mode);
        viewGroup->addAction(action);
    }
    menu->addSeparator();

    auto* sizeRow = new QWidget(menu);
    sizeRow->setObjectName(QStringLiteral("fileExplorerItemSizeRow"));
    auto* sizeLayout = new QHBoxLayout(sizeRow);
    sizeLayout->setContentsMargins(ui::kMarginSmall, ui::kSpacingTight, ui::kMarginSmall,
                                   ui::kSpacingTight);
    sizeLayout->setSpacing(ui::kSpacingSmall);
    auto* sizeLabel = new QLabel(tr("Item size"), sizeRow);
    sizeLabel->setAccessibleName(tr("Explorer item size label"));
    auto* sizeSlider = new QSlider(Qt::Horizontal, sizeRow);
    sizeSlider->setObjectName(QStringLiteral("fileExplorerItemSizeSlider"));
    sizeSlider->setAccessibleName(tr("Explorer item size"));
    sizeSlider->setRange(kFileExplorerItemSizeMin, kFileExplorerItemSizeMax);
    sizeSlider->setSingleStep(8);
    sizeSlider->setPageStep(16);
    sizeSlider->setValue(m_pane_state.view.item_size_px);
    sizeLabel->setBuddy(sizeSlider);
    sizeLayout->addWidget(sizeLabel);
    sizeLayout->addWidget(sizeSlider, 1);
    auto* sizeAction = new QWidgetAction(menu);
    sizeAction->setDefaultWidget(sizeRow);
    menu->addAction(sizeAction);
    connect(sizeSlider, &QSlider::valueChanged, this, [this](const int value) {
        m_pane_state.view.item_size_px = value;
        applyViewSettings();
        saveViewSettings();
        Q_EMIT statusMessage(tr("Explorer item size set to %1 px").arg(value),
                             sak::kTimerStatusMessageMs);
    });
    menu->addSeparator();
    if (auto* hiddenAction =
            addCommandMenuAction(menu, FileExplorerCommandId::ToggleHiddenItems, context)) {
        hiddenAction->setCheckable(true);
        hiddenAction->setChecked(m_pane_state.view.show_hidden);
    }
    if (auto* extensionAction =
            addCommandMenuAction(menu, FileExplorerCommandId::ToggleFileExtensions, context)) {
        extensionAction->setCheckable(true);
        extensionAction->setChecked(m_pane_state.view.show_extensions);
    }
    menu->addSeparator();
    addCommandMenuAction(menu, FileExplorerCommandId::ToggleDualPane, context);
    addCommandMenuAction(menu, FileExplorerCommandId::OpenInNewTab, context);

    const FileExplorerCommandState detailsState =
        FileExplorerCommandRegistry::state(FileExplorerCommandId::ViewDetails, context);
    m_view_button->setEnabled(detailsState.enabled);
    m_view_button->setToolTip(detailsState.enabled ? tr("Change File Explorer view layout")
                                                   : detailsState.blocker);
}

void FileManagementExplorerPanel::executeCommand(const FileExplorerCommandId command) {
    const FileExplorerCommandState state =
        FileExplorerCommandRegistry::state(command, commandContext());
    if (!state.enabled) {
        if (!state.blocker.isEmpty()) {
            if (m_status_label) {
                m_status_label->setText(state.blocker);
            }
            Q_EMIT statusMessage(state.blocker, sak::kTimerStatusMessageMs);
        }
        return;
    }

    switch (command) {
    case FileExplorerCommandId::Open:
        onOpenSelected();
        break;
    case FileExplorerCommandId::Back:
        onBackClicked();
        break;
    case FileExplorerCommandId::Forward:
        onForwardClicked();
        break;
    case FileExplorerCommandId::Up:
        onUpClicked();
        break;
    case FileExplorerCommandId::Home: {
        const auto target = currentTarget();
        loadDirectory(target.local_file_system ? target.root_path : QStringLiteral("/"));
        break;
    }
    case FileExplorerCommandId::Refresh:
        loadDirectory(m_current_path, false);
        break;
    case FileExplorerCommandId::CopyPath:
        QApplication::clipboard()->setText(m_current_path);
        Q_EMIT statusMessage(tr("Current path copied"), sak::kTimerStatusMessageMs);
        break;
    case FileExplorerCommandId::CopyItemPath:
        QApplication::clipboard()->setText(currentSelection().paths().join(QStringLiteral("\n")));
        Q_EMIT statusMessage(tr("Item path copied"), sak::kTimerStatusMessageMs);
        break;
    case FileExplorerCommandId::Preview:
        previewSelectedFile();
        break;
    case FileExplorerCommandId::Properties:
        updateDetailsPane();
        if (m_details_tabs && m_properties_text) {
            m_details_tabs->setCurrentWidget(m_properties_text);
        }
        break;
    case FileExplorerCommandId::SelectAll:
        if (auto* view = currentItemView()) {
            view->selectAll();
        }
        break;
    case FileExplorerCommandId::ClearSelection:
        if (auto* selection_model = m_pane ? m_pane->sharedSelectionModel() : nullptr) {
            selection_model->clearSelection();
        }
        break;
    case FileExplorerCommandId::InvertSelection:
        if (auto* view = currentItemView(); view && view->selectionModel() && view->model()) {
            auto* selection_model = view->selectionModel();
            for (int row = 0; row < view->model()->rowCount(); ++row) {
                const QModelIndex left = view->model()->index(row, 0);
                const QModelIndex right =
                    view->model()->index(row, view->model()->columnCount() - 1);
                const QItemSelection row_selection(left, right);
                const bool selected = selection_model->isRowSelected(row, QModelIndex());
                selection_model->select(row_selection,
                                        selected ? QItemSelectionModel::Deselect
                                                 : QItemSelectionModel::Select);
            }
        }
        break;
    case FileExplorerCommandId::NewFolder:
        onNewFolderClicked();
        break;
    case FileExplorerCommandId::WriteFile:
        onWriteFileClicked();
        break;
    case FileExplorerCommandId::Rename:
        onRenameClicked();
        break;
    case FileExplorerCommandId::Delete:
        onDeleteClicked();
        break;
    case FileExplorerCommandId::TogglePreviewPane:
        if (m_details_tabs) {
            m_details_tabs->setVisible(!m_details_tabs->isVisible());
        }
        break;
    case FileExplorerCommandId::ToggleHiddenItems:
        m_pane_state.view.show_hidden = !m_pane_state.view.show_hidden;
        applyViewSettings();
        if (m_pane && m_item_model && !m_item_model->entries().isEmpty()) {
            if (m_pane->sortFilterModel() && m_pane->sortFilterModel()->rowCount() == 0) {
                m_pane->showEmptyState(tr("No items match current view settings."));
            } else {
                m_pane->showReadyState();
            }
        }
        saveViewSettings();
        QTimer::singleShot(0, this, [this]() {
            updateActionButtons();
        });
        Q_EMIT statusMessage(m_pane_state.view.show_hidden ? tr("Hidden items shown")
                                                            : tr("Hidden items hidden"),
                             sak::kTimerStatusMessageMs);
        break;
    case FileExplorerCommandId::ToggleFileExtensions:
        m_pane_state.view.show_extensions = !m_pane_state.view.show_extensions;
        applyViewSettings();
        if (m_pane && m_item_model && !m_item_model->entries().isEmpty()) {
            if (m_pane->sortFilterModel() && m_pane->sortFilterModel()->rowCount() == 0) {
                m_pane->showEmptyState(tr("No items match current view settings."));
            } else {
                m_pane->showReadyState();
            }
        }
        saveViewSettings();
        QTimer::singleShot(0, this, [this]() {
            updateActionButtons();
        });
        Q_EMIT statusMessage(m_pane_state.view.show_extensions ? tr("File extensions shown")
                                                               : tr("File extensions hidden"),
                             sak::kTimerStatusMessageMs);
        break;
    case FileExplorerCommandId::ViewDetails:
    case FileExplorerCommandId::ViewList:
    case FileExplorerCommandId::ViewGrid:
    case FileExplorerCommandId::ViewCards:
    case FileExplorerCommandId::ViewColumns:
    case FileExplorerCommandId::ViewAdaptive:
        setExplorerViewMode(modeForCommand(command));
        break;
    case FileExplorerCommandId::OpenInNewTab:
    case FileExplorerCommandId::OpenInSecondPane:
    case FileExplorerCommandId::ToggleDualPane:
        Q_EMIT statusMessage(state.command.status_text, sak::kTimerStatusMessageMs);
        break;
    }
}

void FileManagementExplorerPanel::promptCurrentFolderFilter() {
    if (!m_pane || !m_pane->sortFilterModel()) {
        return;
    }

    bool ok = false;
    const QString current_filter = m_pane->sortFilterModel()->nameFilter();
    const QString filter = QInputDialog::getText(this,
                                                 tr("Filter Current Folder"),
                                                 tr("Name, type, or path contains:"),
                                                 QLineEdit::Normal,
                                                 current_filter,
                                                 &ok);
    if (!ok) {
        return;
    }

    m_pane->sortFilterModel()->setNameFilter(filter);
    const int visible_count = m_pane->sortFilterModel()->rowCount();
    const QString message = filter.trimmed().isEmpty()
                                ? tr("Current folder filter cleared.")
                                : tr("Filter active: %1 item(s) visible.").arg(visible_count);
    if (m_status_label) {
        m_status_label->setText(message);
    }
    Q_EMIT statusMessage(message, sak::kTimerStatusDefaultMs);
    updateActionButtons();
}

void FileManagementExplorerPanel::showCommandPalette() {
    QDialog dialog(this);
    dialog.setWindowTitle(tr("Command Palette"));
    dialog.setMinimumWidth(sak::kDialogWidthLarge);

    auto* layout = new QVBoxLayout(&dialog);
    auto* filter = new QLineEdit(&dialog);
    filter->setObjectName(QStringLiteral("fileExplorerCommandPaletteFilter"));
    filter->setAccessibleName(tr("Filter File Explorer commands"));
    filter->setPlaceholderText(tr("Type a command name"));
    layout->addWidget(filter);

    auto* commands = new QListWidget(&dialog);
    commands->setObjectName(QStringLiteral("fileExplorerCommandPaletteList"));
    commands->setAccessibleName(tr("File Explorer commands"));
    layout->addWidget(commands, 1);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    buttons->button(QDialogButtonBox::Ok)->setText(tr("Run"));
    layout->addWidget(buttons);

    const auto rebuild = [this, commands, filter, buttons]() {
        const QString needle = filter->text().trimmed();
        const auto context = commandContext();
        commands->clear();
        int firstEnabledRow = -1;
        for (const FileExplorerCommand& command : FileExplorerCommandRegistry::commands()) {
            const FileExplorerCommandState state =
                FileExplorerCommandRegistry::state(command.id, context);
            const QString searchable =
                QStringList{state.command.text, state.command.shortcut, state.command.status_text}
                    .join(QLatin1Char(' '));
            if (!needle.isEmpty() && !searchable.contains(needle, Qt::CaseInsensitive)) {
                continue;
            }

            QString label = state.command.text;
            if (!state.command.shortcut.trimmed().isEmpty()) {
                label += tr(" (%1)").arg(state.command.shortcut);
            }
            if (!state.enabled && !state.blocker.isEmpty()) {
                label += tr(" - %1").arg(state.blocker);
            }

            auto* item = new QListWidgetItem(label, commands);
            item->setData(kCommandIdRole, QVariant::fromValue(state.command.id));
            item->setData(kCommandEnabledRole, state.enabled);
            item->setData(kCommandBlockerRole, state.blocker);
            item->setToolTip(state.enabled ? state.command.status_text : state.blocker);
            if (!state.enabled) {
                item->setFlags(item->flags() & ~Qt::ItemIsEnabled);
            } else if (firstEnabledRow < 0) {
                firstEnabledRow = commands->row(item);
            }
        }
        if (commands->count() > 0) {
            commands->setCurrentRow(firstEnabledRow >= 0 ? firstEnabledRow : 0);
        }
        buttons->button(QDialogButtonBox::Ok)->setEnabled(
            commands->currentItem() &&
            commands->currentItem()->data(kCommandEnabledRole).toBool());
    };

    connect(filter, &QLineEdit::textChanged, &dialog, rebuild);
    connect(commands, &QListWidget::currentItemChanged, &dialog, [buttons](QListWidgetItem* item) {
        buttons->button(QDialogButtonBox::Ok)->setEnabled(
            item && item->data(kCommandEnabledRole).toBool());
    });
    connect(commands, &QListWidget::itemDoubleClicked, &dialog, [&dialog](QListWidgetItem* item) {
        if (item && item->data(kCommandEnabledRole).toBool()) {
            dialog.accept();
        }
    });
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);

    rebuild();
    if (dialog.exec() != QDialog::Accepted) {
        return;
    }

    auto* current = commands->currentItem();
    if (!current || !current->data(kCommandEnabledRole).toBool()) {
        return;
    }
    executeCommand(current->data(kCommandIdRole).value<FileExplorerCommandId>());
}

void FileManagementExplorerPanel::updateDetailsPane() {
    const auto target = currentTarget();
    const FileExplorerSelection selection = currentSelection();

    QStringList properties;
    if (target.root_path.isEmpty()) {
        properties.append(tr("No target selected."));
    } else {
        properties.append(tr("Target: %1").arg(target.label));
        properties.append(tr("File system: %1").arg(target.file_system));
        properties.append(tr("Root: %1").arg(target.root_path));
        properties.append(tr("Path: %1").arg(m_current_path));
        properties.append(tr("Capability: %1").arg(
            FileManagementFileSystemBridge::capabilitySummary(target)));
        if (!selection.isEmpty()) {
            properties.append(tr("Selected: %1 item(s)").arg(selection.count()));
            properties.append(selection.paths().join(QStringLiteral("\n")));
        }
    }

    QStringList safety;
    if (target.root_path.isEmpty()) {
        safety.append(tr("No File Explorer target selected."));
    } else {
        safety.append(tr("Write state: %1").arg(target.can_write_files ? tr("enabled") : tr("blocked")));
        safety.append(tr("Read state: %1").arg(target.can_read_files ? tr("enabled") : tr("blocked")));
        safety.append(tr("Browse state: %1").arg(target.can_browse ? tr("enabled") : tr("blocked")));
        if (!target.blockers.isEmpty()) {
            safety.append(tr("Blockers: %1").arg(target.blockers.join(QStringLiteral("; "))));
        }

        const auto context = commandContext();
        for (const FileExplorerCommandId command : {FileExplorerCommandId::NewFolder,
                                                    FileExplorerCommandId::WriteFile,
                                                    FileExplorerCommandId::Rename,
                                                    FileExplorerCommandId::Delete,
                                                    FileExplorerCommandId::OpenInNewTab,
                                                    FileExplorerCommandId::ToggleDualPane}) {
            const FileExplorerCommandState state = FileExplorerCommandRegistry::state(command, context);
            safety.append(tr("%1: %2")
                              .arg(state.command.text,
                                   state.enabled ? tr("available") : state.blocker));
        }
    }

    QStringList evidence;
    evidence.append(tr("Command-route evidence attaches in later certification milestones."));
    if (!target.root_path.isEmpty()) {
        evidence.append(tr("Target ID: %1").arg(target.id));
        evidence.append(tr("Source: %1").arg(target.source));
    }

    if (m_properties_text) {
        m_properties_text->setPlainText(properties.join(QStringLiteral("\n")));
    }
    if (m_safety_text) {
        m_safety_text->setPlainText(safety.join(QStringLiteral("\n")));
    }
    if (m_evidence_text) {
        m_evidence_text->setPlainText(evidence.join(QStringLiteral("\n")));
    }
    if (m_preview_text && m_preview_text->toPlainText().isEmpty()) {
        m_preview_text->setPlainText(tr("Select a readable file and choose Preview."));
    }
    if (m_status_label) {
        if (target.root_path.isEmpty()) {
            m_status_label->setText(tr("No target selected"));
        } else {
            m_status_label->setText(tr("%1 | %2 | %3 selected | writes %4")
                                        .arg(target.label,
                                             target.file_system,
                                             QString::number(selection.count()),
                                             target.can_write_files ? tr("enabled")
                                                                    : tr("blocked")));
        }
    }
}

void FileManagementExplorerPanel::updateActionButtons() {
    const auto context = commandContext();
    applyCommandState(m_back_button, FileExplorerCommandId::Back, context);
    applyCommandState(m_forward_button, FileExplorerCommandId::Forward, context);
    applyCommandState(m_up_button, FileExplorerCommandId::Up, context);
    applyCommandState(m_open_button, FileExplorerCommandId::Open, context);
    applyCommandState(m_copy_path_button, FileExplorerCommandId::CopyPath, context);
    applyCommandState(m_new_folder_button, FileExplorerCommandId::NewFolder, context);
    applyCommandState(m_write_file_button, FileExplorerCommandId::WriteFile, context);
    applyCommandState(m_rename_button, FileExplorerCommandId::Rename, context);
    applyCommandState(m_delete_button, FileExplorerCommandId::Delete, context);
    rebuildViewMenu(context);
    updateDetailsPane();
}

void FileManagementExplorerPanel::onRefreshMountedTargets() {
    setTargets(FileManagementFileSystemBridge::mountedTargets());
    logMessage(tr("File explorer mounted targets refreshed"));
}

void FileManagementExplorerPanel::onScanDiskTargets() {
    Q_EMIT statusMessage(tr("Scanning disk and partition targets..."), 0);
    setEnabled(false);
    const auto inventory = StorageInventoryWorker::scanCurrentSystem();
    setEnabled(true);
    setTargets(FileManagementFileSystemBridge::targetsFromInventory(inventory));
    logMessage(tr("File explorer disk target scan complete"));
}

void FileManagementExplorerPanel::onAddManualTarget() {
    QDialog dialog(this);
    dialog.setWindowTitle(tr("Add Raw or Image Target"));
    dialog.setMinimumWidth(sak::kDialogWidthLarge);
    auto* layout = new QFormLayout(&dialog);

    auto* path = new QLineEdit(&dialog);
    path->setAccessibleName(tr("Raw or image target path"));
    auto* browse = new QPushButton(tr("Browse"), &dialog);
    browse->setStyleSheet(ui::kSecondaryButtonStyle);
    auto* pathRow = new QHBoxLayout();
    pathRow->addWidget(path, 1);
    pathRow->addWidget(browse);
    layout->addRow(tr("Target path:"), pathRow);

    auto* fs = new QComboBox(&dialog);
    fs->addItems({QStringLiteral("ext2"),
                  QStringLiteral("ext3"),
                  QStringLiteral("ext4"),
                  QStringLiteral("HFS+"),
                  QStringLiteral("HFSX"),
                  QStringLiteral("APFS"),
                  QStringLiteral("XFS"),
                  QStringLiteral("Btrfs")});
    fs->setAccessibleName(tr("Manual target file system"));
    layout->addRow(tr("File system:"), fs);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    buttons->button(QDialogButtonBox::Ok)->setText(tr("Add Target"));
    layout->addRow(buttons);
    connect(browse, &QPushButton::clicked, &dialog, [this, path]() {
        const QString file = QFileDialog::getOpenFileName(this, tr("Select Raw or Image Target"));
        if (!file.isEmpty()) {
            path->setText(file);
        }
    });
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);

    if (dialog.exec() != QDialog::Accepted) {
        return;
    }
    if (path->text().trimmed().isEmpty()) {
        sak::showWarningLogged(this, tr("Add Raw or Image Target"), tr("Target path is required."));
        return;
    }
    appendTarget(FileManagementFileSystemBridge::manualTarget(path->text(), fs->currentText()));
}

void FileManagementExplorerPanel::onTargetChanged(int index) {
    if (!m_target_list || index < 0 || index >= m_target_list->count()) {
        m_current_target_index = -1;
        updateDetailsPane();
        updateActionButtons();
        return;
    }

    auto* item = m_target_list->item(index);
    if (!item) {
        return;
    }
    const auto kind = static_cast<SidebarEntryKind>(item->data(kSidebarKindRole).toInt());
    int target_index = -1;
    if (kind == SidebarEntryKind::Home) {
        for (int target_row = 0; target_row < m_targets.size(); ++target_row) {
            if (m_targets.at(target_row).local_file_system) {
                target_index = target_row;
                break;
            }
        }
    } else if (kind == SidebarEntryKind::Target) {
        target_index = item->data(kTargetIndexRole).toInt();
    }

    if (target_index < 0 || target_index >= m_targets.size()) {
        return;
    }

    m_current_target_index = target_index;
    resetPaneNavigationPreservingView(&m_pane_state);
    const auto target = m_targets.at(target_index);
    rememberRecentTarget(FileExplorerTargetId::fromTarget(target).value);
    loadDirectory(target.local_file_system ? target.root_path : QStringLiteral("/"), false);
}

void FileManagementExplorerPanel::onPathReturnPressed() {
    loadDirectory(m_path_edit->text());
}

void FileManagementExplorerPanel::onBackClicked() {
    if (m_pane_state.goBack()) {
        loadDirectory(m_pane_state.location.path, false);
    }
}

void FileManagementExplorerPanel::onForwardClicked() {
    if (m_pane_state.goForward()) {
        loadDirectory(m_pane_state.location.path, false);
    }
}

void FileManagementExplorerPanel::onUpClicked() {
    const auto target = currentTarget();
    if (m_pane_state.goUp(target.local_file_system)) {
        loadDirectory(m_pane_state.location.path, false);
    }
}

void FileManagementExplorerPanel::onOpenSelected() {
    if (selectedIsDirectory()) {
        loadDirectory(selectedPath());
        return;
    }
    previewSelectedFile();
}

void FileManagementExplorerPanel::onCopyPathClicked() {
    const auto selection = currentSelection();
    const QString path = selection.isEmpty() ? m_current_path
                                             : selection.paths().join(QStringLiteral("\n"));
    QApplication::clipboard()->setText(path);
    Q_EMIT statusMessage(tr("Path copied"), sak::kTimerStatusMessageMs);
}

void FileManagementExplorerPanel::onNewFolderClicked() {
    const auto target = currentTarget();
    if (!target.can_write_files) {
        sak::showWarningLogged(this,
                               tr("New Folder"),
                               target.blockers.join(QStringLiteral("\n")));
        return;
    }
    QString identity_blocker;
    if (!validateCurrentTargetIdentity(&identity_blocker)) {
        sak::showWarningLogged(this, tr("New Folder"), identity_blocker);
        return;
    }
    bool ok = false;
    const QString name = QInputDialog::getText(this,
                                               tr("New Folder"),
                                               tr("Folder name:"),
                                               QLineEdit::Normal,
                                               QStringLiteral("New Folder"),
                                               &ok);
    if (!ok) {
        return;
    }
    const QString path = targetPathForName(name);
    if (path.isEmpty()) {
        sak::showWarningLogged(this, tr("New Folder"), tr("Enter a folder name without path separators."));
        return;
    }
    const auto result = FileManagementFileSystemBridge::createDirectory(target, path);
    showMutationResult(tr("New Folder"), result);
    if (result.ok) {
        loadDirectory(m_current_path);
    }
}

void FileManagementExplorerPanel::onWriteFileClicked() {
    const auto target = currentTarget();
    if (!target.can_write_files) {
        sak::showWarningLogged(this,
                               tr("Write File"),
                               target.blockers.join(QStringLiteral("\n")));
        return;
    }
    QString identity_blocker;
    if (!validateCurrentTargetIdentity(&identity_blocker)) {
        sak::showWarningLogged(this, tr("Write File"), identity_blocker);
        return;
    }
    const QString sourcePath = QFileDialog::getOpenFileName(this, tr("Select File to Write"));
    if (sourcePath.isEmpty()) {
        return;
    }
    const QFileInfo sourceInfo(sourcePath);
    if (sourceInfo.size() > kExplorerWriteMaxBytes) {
        sak::showWarningLogged(this,
                               tr("Write File"),
                               tr("File exceeds the 64 MiB File Explorer write cap."));
        return;
    }
    QFile source(sourcePath);
    if (!source.open(QIODevice::ReadOnly)) {
        sak::showWarningLogged(this,
                               tr("Write File"),
                               tr("Unable to read source file: %1").arg(source.errorString()));
        return;
    }
    bool ok = false;
    const QString name = QInputDialog::getText(this,
                                               tr("Write File"),
                                               tr("Target file name:"),
                                               QLineEdit::Normal,
                                               sourceInfo.fileName(),
                                               &ok);
    if (!ok) {
        return;
    }
    const QString targetPath = targetPathForName(name);
    if (targetPath.isEmpty()) {
        sak::showWarningLogged(this, tr("Write File"), tr("Enter a file name without path separators."));
        return;
    }
    const auto result =
        FileManagementFileSystemBridge::writeFile(target, targetPath, source.readAll());
    showMutationResult(tr("Write File"), result);
    if (result.ok) {
        loadDirectory(m_current_path);
    }
}

void FileManagementExplorerPanel::onRenameClicked() {
    const auto target = currentTarget();
    const QString sourcePath = selectedPath();
    if (!target.can_write_files || sourcePath.isEmpty()) {
        return;
    }
    QString identity_blocker;
    if (!validateCurrentTargetIdentity(&identity_blocker)) {
        sak::showWarningLogged(this, tr("Rename"), identity_blocker);
        return;
    }
    bool ok = false;
    const QString newName = QInputDialog::getText(this,
                                                  tr("Rename"),
                                                  tr("New name:"),
                                                  QLineEdit::Normal,
                                                  nameForPath(sourcePath, target.local_file_system),
                                                  &ok);
    if (!ok) {
        return;
    }
    if (!isSafeChildName(newName)) {
        sak::showWarningLogged(this, tr("Rename"), tr("Enter a name without path separators."));
        return;
    }
    const QString destinationPath =
        childPathFor(parentPathForEntry(sourcePath, target.local_file_system),
                     newName,
                     target.local_file_system);
    const auto result =
        FileManagementFileSystemBridge::renameEntry(target, sourcePath, destinationPath);
    showMutationResult(tr("Rename"), result);
    if (result.ok) {
        loadDirectory(m_current_path);
    }
}

void FileManagementExplorerPanel::onDeleteClicked() {
    const auto target = currentTarget();
    const auto selection = currentSelection();
    if (!target.can_write_files || selection.isEmpty()) {
        return;
    }
    QString identity_blocker;
    if (!validateCurrentTargetIdentity(&identity_blocker)) {
        sak::showWarningLogged(this, tr("Delete Entry"), identity_blocker);
        return;
    }
    const QStringList paths = selection.paths();
    const auto response = sak::showQuestionLogged(
        this,
        tr("Delete Entry"),
        tr("Delete %1 item(s) from '%2'? This permanently removes data from the selected target.\n\n%3")
            .arg(QString::number(selection.count()), target.label, paths.join(QStringLiteral("\n"))),
        QMessageBox::Yes | QMessageBox::No,
        QMessageBox::No);
    if (response != QMessageBox::Yes) {
        return;
    }
    QStringList blockers;
    QStringList warnings;
    int deleted = 0;
    for (const FileManagementEntry& entry : selection.entries) {
        const auto result = entry.directory
                                ? FileManagementFileSystemBridge::deleteDirectory(target, entry.path)
                                : FileManagementFileSystemBridge::deleteFile(target, entry.path);
        blockers.append(result.blockers);
        warnings.append(result.warnings);
        if (result.ok) {
            ++deleted;
            logMessage(tr("Delete Entry: %1").arg(entry.path));
        }
    }

    if (deleted == selection.count()) {
        Q_EMIT statusMessage(tr("Delete Entry complete"), sak::kTimerStatusDefaultMs);
        loadDirectory(m_current_path);
        return;
    }

    QStringList details;
    details.append(blockers);
    details.append(warnings);
    sak::showWarningLogged(
        this,
        tr("Delete Entry"),
        details.isEmpty()
            ? tr("Deleted %1 of %2 item(s).").arg(deleted).arg(selection.count())
            : details.join(QStringLiteral("\n")));
    if (deleted > 0) {
        loadDirectory(m_current_path);
    }
}

void FileManagementExplorerPanel::onTableContextMenuRequested(const QPoint& position) {
    auto* view = qobject_cast<QAbstractItemView*>(sender());
    if (!view) {
        view = currentItemView();
    }
    if (!view) {
        return;
    }

    if (const QModelIndex index = view->indexAt(position); index.isValid()) {
        selectRowInView(view, index.row());
    }

    const auto context = commandContext();
    QMenu menu(this);
    menu.setObjectName(QStringLiteral("fileExplorerTableContextMenu"));
    addCommandMenuAction(&menu, FileExplorerCommandId::Open, context);
    addCommandMenuAction(&menu, FileExplorerCommandId::OpenInNewTab, context);
    addCommandMenuAction(&menu, FileExplorerCommandId::OpenInSecondPane, context);
    menu.addSeparator();
    addCommandMenuAction(&menu, FileExplorerCommandId::Preview, context);
    addCommandMenuAction(&menu, FileExplorerCommandId::Properties, context);
    addCommandMenuAction(&menu, FileExplorerCommandId::CopyItemPath, context);
    addCommandMenuAction(&menu, FileExplorerCommandId::CopyPath, context);
    menu.addSeparator();
    addCommandMenuAction(&menu, FileExplorerCommandId::NewFolder, context);
    addCommandMenuAction(&menu, FileExplorerCommandId::WriteFile, context);
    addCommandMenuAction(&menu, FileExplorerCommandId::Rename, context);
    addCommandMenuAction(&menu, FileExplorerCommandId::Delete, context);
    menu.addSeparator();
    addCommandMenuAction(&menu, FileExplorerCommandId::SelectAll, context);
    addCommandMenuAction(&menu, FileExplorerCommandId::ClearSelection, context);
    addCommandMenuAction(&menu, FileExplorerCommandId::InvertSelection, context);
    addCommandMenuAction(&menu, FileExplorerCommandId::Refresh, context);
    menu.exec(view->viewport()->mapToGlobal(position));
}

void FileManagementExplorerPanel::onTargetContextMenuRequested(const QPoint& position) {
    if (!m_target_list) {
        return;
    }

    int menu_target_index = -1;
    if (const QModelIndex index = m_target_list->indexAt(position); index.isValid()) {
        auto* item = m_target_list->item(index.row());
        if (item) {
            const auto kind = static_cast<SidebarEntryKind>(item->data(kSidebarKindRole).toInt());
            if (kind == SidebarEntryKind::Target) {
                menu_target_index = item->data(kTargetIndexRole).toInt();
                m_target_list->setCurrentRow(index.row());
            }
        }
    }
    if (menu_target_index < 0 && m_current_target_index >= 0 &&
        m_current_target_index < m_targets.size()) {
        menu_target_index = m_current_target_index;
    }
    const bool has_menu_target = menu_target_index >= 0 && menu_target_index < m_targets.size();

    QMenu menu(this);
    menu.setObjectName(QStringLiteral("fileExplorerTargetContextMenu"));
    auto* open = menu.addAction(tr("Open Target"));
    open->setEnabled(has_menu_target);
    connect(open, &QAction::triggered, this, [this, menu_target_index]() {
        if (menu_target_index >= 0 && menu_target_index < m_targets.size()) {
            m_current_target_index = menu_target_index;
            const auto target = m_targets.at(menu_target_index);
            rememberRecentTarget(FileExplorerTargetId::fromTarget(target).value);
            loadDirectory(target.local_file_system ? target.root_path : QStringLiteral("/"));
        }
    });
    auto* copyRoot = menu.addAction(tr("Copy Target Root"));
    copyRoot->setEnabled(has_menu_target && !m_targets.at(menu_target_index).root_path.isEmpty());
    connect(copyRoot, &QAction::triggered, this, [this, menu_target_index]() {
        if (menu_target_index < 0 || menu_target_index >= m_targets.size()) {
            return;
        }
        QApplication::clipboard()->setText(m_targets.at(menu_target_index).root_path);
        Q_EMIT statusMessage(tr("Target root copied"), sak::kTimerStatusMessageMs);
    });
    auto* favorite = menu.addAction(
        has_menu_target &&
                m_favorite_target_ids.contains(
                    FileExplorerTargetId::fromTarget(m_targets.at(menu_target_index)).value)
            ? tr("Unpin Favorite")
            : tr("Pin Favorite"));
    favorite->setEnabled(has_menu_target);
    connect(favorite, &QAction::triggered, this, [this, menu_target_index]() {
        if (menu_target_index < 0 || menu_target_index >= m_targets.size()) {
            return;
        }
        const QString target_id = FileExplorerTargetId::fromTarget(m_targets.at(menu_target_index)).value;
        if (m_favorite_target_ids.contains(target_id)) {
            m_favorite_target_ids.removeAll(target_id);
        } else {
            m_favorite_target_ids.prepend(target_id);
        }
        saveSidebarState();
        rebuildTargetList(target_id);
    });
    auto* properties = menu.addAction(tr("Target Properties"));
    properties->setEnabled(has_menu_target);
    connect(properties, &QAction::triggered, this, [this, menu_target_index]() {
        if (menu_target_index < 0 || menu_target_index >= m_targets.size()) {
            return;
        }
        const auto target = m_targets.at(menu_target_index);
        QStringList lines;
        lines.append(tr("Target: %1").arg(target.label));
        lines.append(tr("ID: %1").arg(FileExplorerTargetId::fromTarget(target).value));
        lines.append(tr("Root: %1").arg(target.root_path));
        lines.append(tr("File system: %1").arg(target.file_system));
        lines.append(tr("Source: %1").arg(target.source));
        lines.append(tr("Size: %1 bytes").arg(QString::number(target.size_bytes)));
        lines.append(tr("Capability: %1").arg(
            FileManagementFileSystemBridge::capabilitySummary(target)));
        if (!target.blockers.isEmpty()) {
            lines.append(tr("Blockers: %1").arg(target.blockers.join(QStringLiteral("; "))));
        }
        QMessageBox::information(this, tr("Target Properties"), lines.join(QStringLiteral("\n")));
    });
    menu.addSeparator();
    auto* refresh = menu.addAction(tr("Refresh Mounted Targets"));
    connect(refresh, &QAction::triggered, this, &FileManagementExplorerPanel::onRefreshMountedTargets);
    auto* scan = menu.addAction(tr("Scan Disks"));
    connect(scan, &QAction::triggered, this, &FileManagementExplorerPanel::onScanDiskTargets);
    auto* addManual = menu.addAction(tr("Add Raw/Image"));
    connect(addManual, &QAction::triggered, this, &FileManagementExplorerPanel::onAddManualTarget);
    menu.exec(m_target_list->viewport()->mapToGlobal(position));
}

void FileManagementExplorerPanel::onItemDoubleClicked(const QModelIndex& index) {
    Q_UNUSED(index)
    onOpenSelected();
}

}  // namespace sak
