// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file file_management_explorer_panel.cpp
/// @brief File Management explorer tab with mounted and raw/image targets.

#include "sak/file_management_explorer_panel.h"

#include "sak/advanced_search_worker.h"
#include "sak/file_explorer_breadcrumb.h"
#include "sak/file_explorer_icon_registry.h"
#include "sak/file_explorer_session_store.h"
#include "sak/file_explorer_style.h"
#include "sak/file_explorer_tag_store.h"
#include "sak/layout_constants.h"
#include "sak/message_box_helpers.h"
#include "sak/storage_inventory_worker.h"
#include "sak/style_constants.h"
#include "sak/widget_helpers.h"

#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QCryptographicHash>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QDirIterator>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QFutureWatcher>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QImage>
#include <QInputDialog>
#include <QItemSelection>
#include <QItemSelectionModel>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QListWidgetItem>
#include <QMenu>
#include <QMessageBox>
#include <QMimeData>
#include <QPixmap>
#include <QPlainTextEdit>
#include <QSettings>
#include <QShortcut>
#include <QSlider>
#include <QSplitter>
#include <QStyle>
#include <QTabBar>
#include <QTableView>
#include <QtConcurrent>
#include <QTimer>
#include <QToolButton>
#include <QUrl>
#include <QVariant>
#include <QVBoxLayout>
#include <QWidgetAction>

#include <algorithm>
#include <array>
#include <utility>

namespace sak {

namespace {

constexpr int kExplorerPreviewMaxBytes = 1024 * 1024;
constexpr int kExplorerImagePreviewMaxPx = 480;
constexpr int kExplorerListMaxEntries = 10'000;
// Raw/non-native reads for on-demand hashing are bounded to keep peak RAM sane; a larger
// raw file is hashed over its first window and reported as capped. Local files hash in full.
constexpr uint64_t kExplorerHashMaxBytes = 512ULL * 1024 * 1024;
// Clipboard payload for explorer Copy/Paste. Local targets also publish real file URLs for
// OS interop; raw targets publish only this internal payload (target id + file paths/sizes).
constexpr const char* kExplorerClipboardMime = "application/x-sak-file-explorer-items";
constexpr int kSidebarKindRole = Qt::UserRole + 1;
constexpr int kTargetIndexRole = Qt::UserRole + 2;
constexpr int kSidebarTagRole = Qt::UserRole + 7;
constexpr int kCommandIdRole = Qt::UserRole + 3;
constexpr int kCommandEnabledRole = Qt::UserRole + 4;
constexpr int kCommandBlockerRole = Qt::UserRole + 5;
constexpr int kCommandHeaderRole = Qt::UserRole + 6;

// Shell layout + control constants.
constexpr int kViewIdDigestChars = 24;
constexpr int kCenterPaneStretchIndex = 2;
constexpr int kSidebarCollapseWidth = 720;
constexpr int kDetailsTabsCollapseWidth = 920;
constexpr int kMaxRecentTargetIds = 10;
constexpr int kSizeSliderSingleStep = 8;
constexpr int kSizeSliderPageStep = 16;
constexpr const char* kExplorerSettingsGroup = "FileManagementExplorer";
constexpr const char* kTabSessionGroup = "FileManagementExplorer/TabSession";
constexpr const char* kTagStoreGroup = "FileManagementExplorer/Tags";
constexpr const char* kFavoriteTargetIdsKey = "FavoriteTargetIds";
constexpr const char* kRecentTargetIdsKey = "RecentTargetIds";
constexpr const char* kLastTargetIdKey = "LastTargetId";
constexpr const char* kViewModeKey = "ViewMode";
constexpr const char* kShowHiddenKey = "ShowHiddenItems";
constexpr const char* kShowExtensionsKey = "ShowFileExtensions";
constexpr const char* kItemSizeKey = "ItemSizePx";
constexpr const char* kSearchHistoryKey = "SearchHistory";
constexpr int kMaxSearchHistoryEntries = 20;
constexpr int kExplorerSearchMaxResults = 500;

enum class SidebarEntryKind {
    Header = 0,
    Target = 1,
    Home = 2,
    Tag = 3,
    StaleFavorite = 4,
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
        targetMatchesFileSystem(
            target, {QStringLiteral("apfs"), QStringLiteral("hfs+"), QStringLiteral("hfsx")})) {
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
    const QString trimmed =
        path.endsWith(QLatin1Char('/')) && path.size() > 1 ? path.left(path.size() - 1) : path;
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

bool isViewModeCommand(const FileExplorerCommandId command) {
    using enum FileExplorerCommandId;
    static constexpr auto kViewCommands =
        std::to_array({ViewDetails, ViewList, ViewGrid, ViewCards, ViewColumns, ViewAdaptive});
    return std::ranges::find(kViewCommands, command) != kViewCommands.end();
}

bool isLocalFsTarget(const FileManagementTarget& target) {
    return target.local_file_system;
}

bool isMountedVolumeTarget(const FileManagementTarget& target) {
    return target.local_file_system || target.kind == FileManagementTargetKind::LocalPath;
}

bool isPartitionTarget(const FileManagementTarget& target) {
    return target.kind == FileManagementTargetKind::Partition;
}

bool isRawImageTarget(const FileManagementTarget& target) {
    return target.kind == FileManagementTargetKind::ImageFile ||
           (!target.local_file_system && target.kind != FileManagementTargetKind::Partition);
}

bool isCertificationTarget(const FileManagementTarget& target) {
    return !target.local_file_system && target.can_write_files &&
           targetMatchesFileSystem(
               target, {QStringLiteral("apfs"), QStringLiteral("hfs+"), QStringLiteral("hfsx")});
}

// Adds one command-palette row; returns its row index when enabled, else -1.
int addPaletteCommandItem(QListWidget* commands,
                          const FileExplorerCommandState& state,
                          const QString& needle) {
    const QString searchable =
        QStringList{state.command.text, state.command.shortcut, state.command.status_text}.join(
            QLatin1Char(' '));
    if (!needle.isEmpty() && !searchable.contains(needle, Qt::CaseInsensitive)) {
        return -1;
    }
    QString label = state.command.text;
    if (!state.command.shortcut.trimmed().isEmpty()) {
        label += QCoreApplication::translate("FileManagementExplorerPanel", " (%1)")
                     .arg(state.command.shortcut);
    }
    if (!state.enabled && !state.blocker.isEmpty()) {
        label +=
            QCoreApplication::translate("FileManagementExplorerPanel", " - %1").arg(state.blocker);
    }
    auto* item = new QListWidgetItem(label, commands);
    item->setData(kCommandIdRole, QVariant::fromValue(state.command.id));
    item->setData(kCommandEnabledRole, state.enabled);
    item->setData(kCommandBlockerRole, state.blocker);
    item->setToolTip(state.enabled ? state.command.status_text : state.blocker);
    if (!state.enabled) {
        item->setFlags(item->flags() & ~Qt::ItemIsEnabled);
        return -1;
    }
    return commands->row(item);
}

// Adds a non-selectable bold section header row for a command group.
QListWidgetItem* addPaletteGroupHeader(QListWidget* commands, const QString& name) {
    auto* header = new QListWidgetItem(name, commands);
    header->setFlags(Qt::NoItemFlags);
    header->setData(kCommandEnabledRole, false);
    header->setData(kCommandHeaderRole, true);
    QFont font = header->font();
    font.setBold(true);
    header->setFont(font);
    return header;
}

void populateCommandPalette(QListWidget* commands,
                            const QString& needle,
                            const FileExplorerCommandContext& context,
                            QDialogButtonBox* buttons) {
    commands->clear();
    int first_enabled_row = -1;
    for (const FileExplorerCommandGroup group : FileExplorerCommandRegistry::groupOrder()) {
        QListWidgetItem* header =
            addPaletteGroupHeader(commands, FileExplorerCommandRegistry::groupName(group));
        const int rows_before = commands->count();
        for (const FileExplorerCommand& command : FileExplorerCommandRegistry::commands()) {
            if (command.group != group) {
                continue;
            }
            const FileExplorerCommandState state = FileExplorerCommandRegistry::state(command.id,
                                                                                      context);
            const int row = addPaletteCommandItem(commands, state, needle);
            if (row >= 0 && first_enabled_row < 0) {
                first_enabled_row = row;
            }
        }
        if (commands->count() == rows_before) {
            delete commands->takeItem(commands->row(header));  // no matching rows for this group
        }
    }
    if (first_enabled_row >= 0) {
        commands->setCurrentRow(first_enabled_row);
    }
    buttons->button(QDialogButtonBox::Ok)
        ->setEnabled(commands->currentItem() &&
                     commands->currentItem()->data(kCommandEnabledRole).toBool());
}

QString locationViewSettingsGroup(const FileExplorerLocation& location) {
    const QString raw = QStringLiteral("%1\n%2").arg(location.target_id.value, location.path);
    const QByteArray digest =
        QCryptographicHash::hash(raw.toUtf8(), QCryptographicHash::Sha256).toHex();
    return QStringLiteral("View/%1").arg(QString::fromLatin1(digest.left(kViewIdDigestChars)));
}

void selectRowInView(QAbstractItemView* view, const int row) {
    if (!view || !view->model() || !view->selectionModel() || row < 0 ||
        row >= view->model()->rowCount()) {
        return;
    }

    const QModelIndex left = view->model()->index(row, 0);
    const QModelIndex right = view->model()->index(row, view->model()->columnCount() - 1);
    view->selectionModel()->select(QItemSelection(left, right),
                                   QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
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

FileManagementExplorerPanel::~FileManagementExplorerPanel() {
    stopExplorerSearch();
    saveTabSession();
}

void FileManagementExplorerPanel::enableTabSessionPersistence() {
    m_tab_session_persistence = true;
    restoreTabSession();
}

void FileManagementExplorerPanel::setupUi() {
    setObjectName(QStringLiteral("fileExplorerRoot"));
    setStyleSheet(ui::fileExplorerShellStyleSheet());

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(
        ui::kMarginSmall, ui::kMarginSmall, ui::kMarginSmall, ui::kMarginSmall);
    layout->setSpacing(ui::kSpacingSmall);

    // Files-style anatomy, top to bottom: tab strip, then the sidebar/content
    // splitter (the nav and command rows live in the center column), then a
    // full-width status row.
    buildTabBar(layout);

    m_shell_splitter = new QSplitter(Qt::Horizontal, this);
    m_shell_splitter->setChildrenCollapsible(false);
    layout->addWidget(m_shell_splitter, 1);

    m_sidebar = new FileExplorerSidebar(m_shell_splitter);
    m_target_list = m_sidebar->targetList();
    m_scan_disks_button = m_sidebar->scanDisksButton();
    m_add_manual_button = m_sidebar->addManualButton();
    m_shell_splitter->addWidget(m_sidebar);

    auto* center = new QWidget(m_shell_splitter);
    auto* centerLayout = new QVBoxLayout(center);
    centerLayout->setContentsMargins(
        ui::kMarginNone, ui::kMarginNone, ui::kMarginNone, ui::kMarginNone);
    centerLayout->setSpacing(ui::kSpacingSmall);
    m_shell_splitter->addWidget(center);

    buildCommandAndNavBars(center, centerLayout);
    buildContentArea(center, centerLayout);
    buildStatusRow(layout);

    connectUiSignals();
    installCommandShortcuts();
    updateActionButtons();
}

void FileManagementExplorerPanel::buildCommandAndNavBars(QWidget* center,
                                                         QVBoxLayout* center_layout) {
    m_omnibar = new FileExplorerOmnibar(center);
    m_sidebar_toggle_button = m_omnibar->sidebarToggleButton();
    m_back_button = m_omnibar->backButton();
    m_forward_button = m_omnibar->forwardButton();
    m_up_button = m_omnibar->upButton();
    m_refresh_button = m_omnibar->refreshButton();
    m_path_edit = m_omnibar->pathEdit();
    m_search_box = m_omnibar->searchBox();
    m_search_button = m_omnibar->searchButton();
    m_command_button = m_omnibar->commandButton();
    center_layout->addWidget(m_omnibar);

    m_command_bar = new FileExplorerCommandBar(center);
    m_new_folder_button = m_command_bar->newFolderButton();
    m_write_file_button = m_command_bar->writeFileButton();
    m_open_button = m_command_bar->openButton();
    m_copy_path_button = m_command_bar->copyPathButton();
    m_rename_button = m_command_bar->renameButton();
    m_delete_button = m_command_bar->deleteButton();
    m_view_button = m_command_bar->viewButton();
    m_details_toggle_button = m_command_bar->detailsToggleButton();
    center_layout->addWidget(m_command_bar);
}

void FileManagementExplorerPanel::buildStatusRow(QVBoxLayout* root_layout) {
    auto* row = new QWidget(this);
    row->setObjectName(QStringLiteral("fileExplorerStatusRow"));
    auto* row_layout = new QHBoxLayout(row);
    row_layout->setContentsMargins(
        ui::kMarginNone, ui::kMarginNone, ui::kMarginNone, ui::kMarginNone);
    row_layout->setSpacing(ui::kSpacingSmall);

    m_summary_label = new QLabel(tr("No target selected"), row);
    m_summary_label->setObjectName(QStringLiteral("fileExplorerSummaryLabel"));
    m_summary_label->setWordWrap(false);
    m_summary_label->setAccessibleName(tr("Explorer target summary"));
    row_layout->addWidget(m_summary_label, 1);

    root_layout->addWidget(row, 0);
}

void FileManagementExplorerPanel::buildTabBar(QVBoxLayout* center_layout) {
    auto* row = new QWidget(this);
    row->setObjectName(QStringLiteral("fileExplorerTabRow"));
    auto* row_layout = new QHBoxLayout(row);
    row_layout->setContentsMargins(0, 0, 0, 0);
    row_layout->setSpacing(ui::kSpacingTight);

    m_tab_bar = new QTabBar(row);
    m_tab_bar->setObjectName(QStringLiteral("fileExplorerTabBar"));
    m_tab_bar->setAccessibleName(tr("Explorer tabs"));
    m_tab_bar->setTabsClosable(true);
    m_tab_bar->setMovable(true);
    m_tab_bar->setExpanding(false);
    m_tab_bar->setDrawBase(false);
    m_tab_bar->addTab(tr("New Tab"));
    nameTabCloseButtons();
    row_layout->addWidget(m_tab_bar, 0);

    auto* new_tab = new QPushButton(row);
    new_tab->setObjectName(QStringLiteral("fileExplorerNewTabButton"));
    new_tab->setIcon(FileExplorerIconRegistry::iconForKey(QStringLiteral("plus")));
    new_tab->setAccessibleName(tr("Open a new explorer tab"));
    new_tab->setToolTip(tr("Open a new tab at the current location"));
    new_tab->setFixedWidth(ui::kUiButtonHeightMini);
    row_layout->addWidget(new_tab, 0);
    row_layout->addStretch(1);

    center_layout->addWidget(row);

    m_tabs.clear();
    m_tabs.append(FileExplorerTabState{});
    m_active_tab = 0;

    connect(m_tab_bar, &QTabBar::currentChanged, this, &FileManagementExplorerPanel::onTabSwitched);
    connect(m_tab_bar,
            &QTabBar::tabCloseRequested,
            this,
            &FileManagementExplorerPanel::onTabCloseRequested);
    connect(new_tab,
            &QPushButton::clicked,
            this,
            &FileManagementExplorerPanel::openCurrentLocationInNewTab);
}

void FileManagementExplorerPanel::nameTabCloseButtons() {
    if (!m_tab_bar) {
        return;
    }
    // QTabBar creates its close buttons automatically once tabs are closable;
    // give each an accessible name so screen readers and the audit can identify it.
    for (int i = 0; i < m_tab_bar->count(); ++i) {
        for (const auto side : {QTabBar::RightSide, QTabBar::LeftSide}) {
            if (QWidget* button = m_tab_bar->tabButton(i, side)) {
                button->setAccessibleName(tr("Close tab"));
                button->setAccessibleDescription(
                    tr("Close explorer tab: %1").arg(m_tab_bar->tabText(i)));
            }
        }
    }
}

void FileManagementExplorerPanel::buildContentArea(QWidget* center, QVBoxLayout* center_layout) {
    m_pane_splitter = new QSplitter(Qt::Horizontal, center);
    m_pane_splitter->setObjectName(QStringLiteral("fileExplorerPaneSplitter"));
    m_pane_a = new FileExplorerPane(m_pane_splitter);
    m_pane_splitter->addWidget(m_pane_a);
    m_pane = m_pane_a;
    m_item_model = m_pane->itemModel();
    installTagProvider(m_item_model);
    m_status_label = m_pane->statusLabel();
    center_layout->addWidget(m_pane_splitter, 1);

    m_details_pane = new FileExplorerDetailsPane(m_shell_splitter);
    m_details_tabs = m_details_pane;
    m_preview_text = m_details_pane->previewText();
    m_properties_text = m_details_pane->propertiesText();
    m_safety_text = m_details_pane->safetyText();
    m_evidence_text = m_details_pane->evidenceText();
    m_shell_splitter->addWidget(m_details_tabs);
    m_shell_splitter->setStretchFactor(0, 0);
    m_shell_splitter->setStretchFactor(1, 1);
    m_shell_splitter->setStretchFactor(kCenterPaneStretchIndex, 0);
}

void FileManagementExplorerPanel::connectUiSignals() {
    connectToolbarSignals();
    connectNavigationSignals();
    connectPaneSignals(m_pane_a, 0);
}

void FileManagementExplorerPanel::connectToolbarSignals() {
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
    connect(m_search_button, &QPushButton::clicked, this, [this]() {
        showExplorerSearchDialog(m_search_box ? m_search_box->text().trimmed() : QString());
    });
    connect(m_search_box, &QLineEdit::returnPressed, this, [this]() {
        showExplorerSearchDialog(m_search_box->text().trimmed());
    });
    connect(m_command_button,
            &QPushButton::clicked,
            this,
            &FileManagementExplorerPanel::showCommandPalette);
}

void FileManagementExplorerPanel::connectNavigationSignals() {
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
    connect(m_omnibar->breadcrumb(),
            &FileExplorerBreadcrumb::segmentActivated,
            this,
            [this](const QString& path) {
                m_path_edit->setText(path);
                onPathReturnPressed();
            });
    connect(
        m_back_button, &QPushButton::clicked, this, &FileManagementExplorerPanel::onBackClicked);
    connect(m_forward_button,
            &QPushButton::clicked,
            this,
            &FileManagementExplorerPanel::onForwardClicked);
    connect(m_up_button, &QPushButton::clicked, this, &FileManagementExplorerPanel::onUpClicked);
    connect(
        m_open_button, &QPushButton::clicked, this, &FileManagementExplorerPanel::onOpenSelected);
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
}

void FileManagementExplorerPanel::connectPaneSignals(FileExplorerPane* pane, int pane_index) {
    if (pane->sharedSelectionModel()) {
        connect(pane->sharedSelectionModel(),
                &QItemSelectionModel::selectionChanged,
                this,
                [this, pane, pane_index]() {
                    // Only a real (non-empty) selection promotes a pane to active, so an empty
                    // selection signal (e.g. a model reset on the hidden second pane) never steals
                    // focus from the pane the user is working in.
                    if (pane->sharedSelectionModel() &&
                        pane->sharedSelectionModel()->hasSelection()) {
                        activatePane(pane_index);
                    }
                    updateActionButtons();
                });
    }
    for (auto* view : pane->itemViews()) {
        if (!view) {
            continue;
        }
        connect(view, &QAbstractItemView::doubleClicked, this, [this, pane_index](const auto& mi) {
            activatePane(pane_index);
            onItemDoubleClicked(mi);
        });
        connect(view,
                &QWidget::customContextMenuRequested,
                this,
                [this, pane_index](const QPoint& point) {
                    activatePane(pane_index);
                    onTableContextMenuRequested(point);
                });
    }
    connect(pane,
            &FileExplorerPane::columnsDirectoryPreviewRequested,
            this,
            [this, pane_index](const QString& path) {
                activatePane(pane_index);
                loadColumnsPreview(path);
            });
    connect(
        pane, &FileExplorerPane::columnsChildActivated, this, [this, pane_index](const auto& p) {
            activatePane(pane_index);
            loadDirectory(p);
        });
}

void FileManagementExplorerPanel::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    const int width = event ? event->size().width() : this->width();
    if (m_sidebar && width < kSidebarCollapseWidth) {
        m_sidebar->setVisible(false);
    }
    if (m_details_tabs && width < kDetailsTabsCollapseWidth) {
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
        FileExplorerCommandId::DuplicateTab,
        FileExplorerCommandId::ReopenClosedTab,
        FileExplorerCommandId::Hash,
        FileExplorerCommandId::CopyOut,
        FileExplorerCommandId::CopyItems,
        FileExplorerCommandId::Paste,
        FileExplorerCommandId::CopyToOtherPane,
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
    const QString label = QStringLiteral("%1  [%2]\n%3")
                              .arg(target.label, targetBadge(target), targetSubtitle(target));
    auto* item = new QListWidgetItem(style()->standardIcon(icon), label, m_target_list);
    item->setData(kSidebarKindRole, static_cast<int>(SidebarEntryKind::Target));
    item->setData(kTargetIndexRole, target_index);
    item->setToolTip(QStringLiteral("%1\n%2").arg(
        target.root_path, FileManagementFileSystemBridge::capabilitySummary(target)));
    if (!target.blockers.isEmpty()) {
        item->setStatusTip(target.blockers.join(QStringLiteral("; ")));
    }
}

void FileManagementExplorerPanel::appendSidebarTargetsWhere(
    const QString& title, bool (*predicate)(const FileManagementTarget&)) {
    appendSidebarHeader(title);
    for (int index = 0; index < m_targets.size(); ++index) {
        if (predicate(m_targets.at(index))) {
            appendSidebarTarget(m_targets.at(index), index);
        }
    }
}

void FileManagementExplorerPanel::appendSidebarTargetsById(const QString& title,
                                                           const QStringList& target_ids,
                                                           const bool warn_when_missing) {
    appendSidebarHeader(title);
    for (const QString& target_id : target_ids) {
        const int index = targetIndexForId(target_id);
        if (index >= 0) {
            appendSidebarTarget(m_targets.at(index), index);
        } else if (warn_when_missing) {
            appendStaleFavoriteRow(target_id);
        }
    }
}

void FileManagementExplorerPanel::appendStaleFavoriteRow(const QString& target_id) {
    // A saved favorite whose target is not currently connected stays visible as a
    // disabled, warning-marked row so the user knows the pin still exists and will
    // resolve again when the device returns, rather than silently vanishing.
    auto* item = new QListWidgetItem(style()->standardIcon(QStyle::SP_MessageBoxWarning),
                                     tr("%1  [offline]").arg(target_id),
                                     m_target_list);
    item->setData(kSidebarKindRole, static_cast<int>(SidebarEntryKind::StaleFavorite));
    item->setData(kSidebarTagRole, target_id);
    item->setFlags(Qt::NoItemFlags);
    item->setToolTip(tr("Saved favorite is not currently connected: %1. Right-click to "
                        "remove the pin.")
                         .arg(target_id));
}

void FileManagementExplorerPanel::rebuildTargetList(const QString& preferred_target_id) {
    if (!m_target_list) {
        return;
    }

    const QString current_id =
        !preferred_target_id.trimmed().isEmpty()
            ? preferred_target_id.trimmed()
            : (m_current_target_index >= 0 && m_current_target_index < m_targets.size()
                   ? FileExplorerTargetId::fromTarget(m_targets.at(m_current_target_index)).value
                   : QString());

    m_target_list->blockSignals(true);
    m_target_list->clear();

    appendSidebarHeader(tr("Home"));
    auto* home = new QListWidgetItem(style()->standardIcon(QStyle::SP_DirHomeIcon),
                                     tr("Home"),
                                     m_target_list);
    home->setData(kSidebarKindRole, static_cast<int>(SidebarEntryKind::Home));
    home->setToolTip(tr("Open the first mounted local target."));

    appendSidebarTargetsById(tr("Favorites"), m_favorite_target_ids, /*warn_when_missing=*/true);
    appendSidebarTargetsWhere(tr("This PC"), &isLocalFsTarget);
    appendSidebarTargetsWhere(tr("Mounted Volumes"), &isMountedVolumeTarget);
    appendSidebarTargetsWhere(tr("Disks and Partitions"), &isPartitionTarget);
    appendSidebarTargetsWhere(tr("Raw Images"), &isRawImageTarget);
    appendSidebarTargetsById(tr("Recent"), m_recent_target_ids);
    appendSidebarTargetsWhere(tr("Certification Targets"), &isCertificationTarget);

    const QStringList tags = allKnownTags();
    if (!tags.isEmpty()) {
        appendSidebarHeader(tr("Tags"));
        for (const QString& tag : tags) {
            auto* item = new QListWidgetItem(style()->standardIcon(QStyle::SP_FileDialogListView),
                                             tag,
                                             m_target_list);
            item->setData(kSidebarKindRole, static_cast<int>(SidebarEntryKind::Tag));
            item->setData(kSidebarTagRole, tag);
            item->setToolTip(tr("Filter the current folder to items tagged '%1'").arg(tag));
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
        if (!item ||
            item->data(kSidebarKindRole).toInt() != static_cast<int>(SidebarEntryKind::Target)) {
            continue;
        }
        const int index = item->data(kTargetIndexRole).toInt();
        if (index >= 0 && index < m_targets.size() &&
            FileExplorerTargetId::fromTarget(m_targets.at(index)).value == target_id) {
            if (index == m_current_target_index) {
                // Re-selecting the already-active target (e.g. after a sidebar rebuild for a
                // tag edit or favorite reorder) must only restore the visual selection; a
                // live signal would reset navigation history and yank the pane to the root.
                const QSignalBlocker blocker(m_target_list);
                m_target_list->setCurrentRow(row);
            } else {
                m_target_list->setCurrentRow(row);
            }
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
    while (m_recent_target_ids.size() > kMaxRecentTargetIds) {
        m_recent_target_ids.removeLast();
    }
    m_last_target_id = clean;
    saveSidebarState();
}

void FileManagementExplorerPanel::loadSidebarState() {
    QSettings settings;
    settings.beginGroup(QString::fromLatin1(kExplorerSettingsGroup));
    m_favorite_target_ids =
        settings.value(QString::fromLatin1(kFavoriteTargetIdsKey)).toStringList();
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
    m_pane_state.view.item_size_px = std::clamp(m_pane_state.view.item_size_px,
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
        m_pane_state.view.item_size_px = settings.value(QString::fromLatin1(kItemSizeKey)).toInt();
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
    QTimer::singleShot(0, this, [this]() { updateActionButtons(); });
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

void FileManagementExplorerPanel::resetListingForUnavailableTarget(const QString& message,
                                                                   const bool is_error) {
    ++m_listing_revision;
    ++m_columns_preview_revision;
    if (is_error) {
        m_summary_label->setText(message);
        m_item_model->clear();
    }
    if (m_pane) {
        if (is_error) {
            m_pane->showErrorState(message);
        } else {
            m_pane->showEmptyState(message);
        }
        m_pane->clearColumnsPreview();
    }
    updateDetailsPane();
    updateActionButtons();
}

void FileManagementExplorerPanel::loadDirectory(const QString& path, const bool add_history) {
    const auto target = currentTarget();
    if (target.root_path.isEmpty()) {
        resetListingForUnavailableTarget(tr("No File Explorer target selected."), false);
        return;
    }
    if (!target.can_browse) {
        resetListingForUnavailableTarget(target.blockers.join(QStringLiteral("; ")), true);
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
    clearCurrentTagFilter();  // tag filter is scoped to one folder view
    loadViewSettingsForCurrentLocation();
    m_path_edit->setText(m_current_path);
    if (m_preview_text) {
        m_preview_text->setPlainText(tr("Select a readable file and choose Preview."));
    }

    const quint64 listing_revision = ++m_listing_revision;
    const int load_pane = m_active_pane_index;
    ++m_columns_preview_revision;
    if (m_pane) {
        m_pane->clearColumnsPreview();
        m_pane->showLoadingState(tr("Loading %1...").arg(m_current_path));
    }
    m_summary_label->setText(tr("Loading %1...").arg(m_current_path));
    updateActionButtons();
    updateActiveTabLabel();

    auto* watcher = new QFutureWatcher<FileManagementListResult>(this);
    connect(watcher,
            &QFutureWatcher<FileManagementListResult>::finished,
            this,
            [this, watcher, listing_revision, load_pane]() {
                watcher->deleteLater();
                // Drop the result if a newer load superseded it, or the user switched the active
                // pane mid-load (the data belongs to the other pane).
                if (listing_revision != m_listing_revision || load_pane != m_active_pane_index) {
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
        return FileManagementFileSystemBridge::listDirectory(target,
                                                             requested_path,
                                                             kExplorerListMaxEntries);
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
    QString summary = tr("%1 item(s) - %2")
                          .arg(result.entries.size())
                          .arg(FileManagementFileSystemBridge::capabilitySummary(target));
    if (!result.warnings.isEmpty()) {
        summary += tr(" - %1").arg(result.warnings.join(QStringLiteral("; ")));
    }
    m_summary_label->setText(summary);
    Q_EMIT statusMessage(tr("Explorer loaded %1 item(s)").arg(result.entries.size()),
                         sak::kTimerStatusDefaultMs);
    selectPendingSearchResult();
    updateDetailsPane();
    updateActionButtons();
}

void FileManagementExplorerPanel::selectPendingSearchResult() {
    // A search "Open Result" navigated to the file's parent folder; now that the (async)
    // listing has arrived, select the target entry so the user lands on it.
    if (m_pending_select_name.isEmpty() || !m_pane) {
        return;
    }
    const QString name = m_pending_select_name;
    m_pending_select_name.clear();
    auto* view = currentItemView();
    auto* proxy = m_pane->sortFilterModel();
    if (!view || !proxy) {
        return;
    }
    for (int row = 0; row < proxy->rowCount(); ++row) {
        const QModelIndex index = proxy->index(row, 0);
        if (index.data(Qt::DisplayRole).toString() == name) {
            selectRowInView(view, row);
            view->scrollTo(index);
            return;
        }
    }
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
        sak::showWarningLogged(this, tr("Preview File"), read.blockers.join(QStringLiteral("\n")));
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

void FileManagementExplorerPanel::hashSelectedFile() {
    const FileExplorerSelection selection = currentSelection();
    if (!selection.hasSingleEntry()) {
        return;
    }
    const FileManagementEntry entry = selection.entries.first();
    if (entry.directory || !entry.regular_file) {
        Q_EMIT statusMessage(tr("Select a single file to hash."), sak::kTimerStatusMessageMs);
        return;
    }
    const FileManagementTarget target = currentTarget();
    const QString path = entry.path;
    Q_EMIT statusMessage(tr("Hashing %1...").arg(entry.name), sak::kTimerStatusMessageMs);

    auto* watcher = new QFutureWatcher<FileManagementHashResult>(this);
    connect(watcher,
            &QFutureWatcher<FileManagementHashResult>::finished,
            this,
            [this, watcher, name = entry.name]() {
                watcher->deleteLater();
                const FileManagementHashResult result = watcher->result();
                if (!result.ok) {
                    Q_EMIT statusMessage(
                        tr("Hash failed: %1").arg(result.blockers.join(QStringLiteral("; "))),
                        sak::kTimerStatusMessageMs);
                    return;
                }
                m_last_hash_name = name;
                m_last_hash_sha256 = result.sha256;
                m_last_hash_capped = result.capped;
                updateDetailsPane();
                Q_EMIT statusMessage(tr("SHA-256 of %1: %2").arg(name, result.sha256),
                                     sak::kTimerStatusMessageMs);
            });
    watcher->setFuture(QtConcurrent::run([target, path]() {
        return FileManagementFileSystemBridge::hashFile(target, path, kExplorerHashMaxBytes);
    }));
}

void FileManagementExplorerPanel::copySelectedFileOut() {
    const FileExplorerSelection selection = currentSelection();
    if (!selection.hasSingleEntry()) {
        Q_EMIT statusMessage(tr("Select a single file or folder to copy out."),
                             sak::kTimerStatusMessageMs);
        return;
    }
    const FileManagementEntry entry = selection.entries.first();
    if (entry.directory) {
        exportSelectedDirectoryOut(entry);
        return;
    }
    if (!entry.regular_file) {
        Q_EMIT statusMessage(tr("Select a single file or folder to copy out."),
                             sak::kTimerStatusMessageMs);
        return;
    }
    const QString destination = QFileDialog::getSaveFileName(
        this, tr("Copy File Out"), QDir(QDir::homePath()).filePath(entry.name));
    if (destination.isEmpty()) {
        return;
    }
    const FileManagementTarget target = currentTarget();
    const QString path = entry.path;
    Q_EMIT statusMessage(tr("Copying %1 out...").arg(entry.name), sak::kTimerStatusMessageMs);

    auto* watcher = new QFutureWatcher<FileManagementExportResult>(this);
    connect(watcher,
            &QFutureWatcher<FileManagementExportResult>::finished,
            this,
            [this, watcher, name = entry.name]() {
                watcher->deleteLater();
                const FileManagementExportResult result = watcher->result();
                if (!result.ok) {
                    Q_EMIT statusMessage(
                        tr("Copy out failed: %1").arg(result.blockers.join(QStringLiteral("; "))),
                        sak::kTimerStatusMessageMs);
                    return;
                }
                m_last_hash_name = name;
                m_last_hash_sha256 = result.sha256;
                m_last_hash_capped = result.capped;
                updateDetailsPane();
                Q_EMIT statusMessage(result.capped
                                         ? tr("Copied %1 out (capped at read window); SHA-256 %2")
                                               .arg(name, result.sha256)
                                         : tr("Copied %1 out; SHA-256 %2").arg(name, result.sha256),
                                     sak::kTimerStatusDefaultMs);
            });
    watcher->setFuture(QtConcurrent::run([target, path, destination]() {
        return FileManagementFileSystemBridge::copyFileToHost(
            target, path, destination, kExplorerHashMaxBytes);
    }));
}

void FileManagementExplorerPanel::copySelectionToClipboard() {
    const FileExplorerSelection selection = currentSelection();
    const FileManagementTarget target = currentTarget();
    QJsonArray items;
    QList<QUrl> urls;
    QStringList lines;
    int skipped = 0;
    for (const FileManagementEntry& entry : selection.entries) {
        if (entry.directory || !entry.regular_file) {
            ++skipped;
            continue;
        }
        QJsonObject item;
        item.insert(QStringLiteral("path"), entry.path);
        item.insert(QStringLiteral("size"), QString::number(entry.size_bytes));
        items.append(item);
        lines.append(entry.path);
        if (target.local_file_system) {
            urls.append(QUrl::fromLocalFile(entry.path));
        }
    }
    if (items.isEmpty()) {
        Q_EMIT statusMessage(tr("Select files to copy (folders are not supported yet)."),
                             sak::kTimerStatusMessageMs);
        return;
    }
    QJsonObject payload;
    payload.insert(QStringLiteral("target"), FileExplorerTargetId::fromTarget(target).value);
    payload.insert(QStringLiteral("items"), items);
    auto* mime = new QMimeData;
    mime->setData(QLatin1String(kExplorerClipboardMime),
                  QJsonDocument(payload).toJson(QJsonDocument::Compact));
    mime->setText(lines.join(QLatin1Char('\n')));
    if (!urls.isEmpty()) {
        mime->setUrls(urls);
    }
    QApplication::clipboard()->setMimeData(mime);
    Q_EMIT statusMessage(
        skipped > 0 ? tr("Copied %1 file(s); %2 folder(s) skipped.").arg(items.size()).arg(skipped)
                    : tr("Copied %1 file(s).").arg(items.size()),
        sak::kTimerStatusMessageMs);
    updateActionButtons();
}

bool FileManagementExplorerPanel::clipboardHasPasteableFiles() const {
    const QMimeData* mime = QApplication::clipboard()->mimeData();
    if (!mime) {
        return false;
    }
    if (mime->hasFormat(QLatin1String(kExplorerClipboardMime))) {
        return true;
    }
    if (!mime->hasUrls()) {
        return false;
    }
    const QList<QUrl> urls = mime->urls();
    return std::ranges::any_of(urls, [](const QUrl& url) {
        return url.isLocalFile() && QFileInfo(url.toLocalFile()).isFile();
    });
}

// Split an internal clipboard payload's items into host files (local source target) or
// raw items (path + size) for the paste routes.
void FileManagementExplorerPanel::appendPayloadItems(const QJsonArray& items,
                                                     const bool source_is_local,
                                                     PasteSources& sources) {
    for (const QJsonValue& value : items) {
        const QJsonObject item = value.toObject();
        const QString path = item.value(QStringLiteral("path")).toString();
        if (path.trimmed().isEmpty()) {
            continue;
        }
        if (source_is_local) {
            sources.host_files.append(path);
        } else {
            sources.raw_items.append(
                {path, item.value(QStringLiteral("size")).toString().toULongLong()});
        }
    }
}

FileManagementExplorerPanel::PasteSources FileManagementExplorerPanel::collectPasteSources(
    const QMimeData* mime) const {
    PasteSources sources;
    if (!mime) {
        return sources;
    }
    if (mime->hasFormat(QLatin1String(kExplorerClipboardMime))) {
        const QJsonObject payload =
            QJsonDocument::fromJson(mime->data(QLatin1String(kExplorerClipboardMime))).object();
        sources.source_target_id = payload.value(QStringLiteral("target")).toString();
        const int source_index = targetIndexForId(sources.source_target_id);
        const bool source_is_local = source_index >= 0 &&
                                     m_targets.at(source_index).local_file_system;
        appendPayloadItems(payload.value(QStringLiteral("items")).toArray(),
                           source_is_local,
                           sources);
        return sources;
    }
    const QList<QUrl> urls = mime->hasUrls() ? mime->urls() : QList<QUrl>{};
    for (const QUrl& url : urls) {
        if (url.isLocalFile() && QFileInfo(url.toLocalFile()).isFile()) {
            sources.host_files.append(url.toLocalFile());
        }
    }
    return sources;
}

bool FileManagementExplorerPanel::confirmTypedRawImport(const FileManagementTarget& target,
                                                        const int file_count) {
    // Typed confirmation before any raw-media import: the write is irreversible and runs
    // through the certified writer, so the user must acknowledge the exact destination.
    bool accepted = false;
    const QString typed = QInputDialog::getText(
        this,
        tr("Confirm Raw Import"),
        tr("This imports %1 file(s) into raw media through the certified writer.\n"
           "Target: %2\nFile system: %3\nIdentity: %4\n\n"
           "Raw writes are irreversible. Type WRITE to continue:")
            .arg(QString::number(file_count),
                 target.label,
                 target.file_system,
                 FileExplorerTargetId::fromTarget(target).value),
        QLineEdit::Normal,
        {},
        &accepted);
    return accepted && typed.trimmed() == QStringLiteral("WRITE");
}

bool FileManagementExplorerPanel::confirmPasteOverwrite(const QString& name) {
    return sak::showQuestionLogged(
               this,
               tr("Paste"),
               tr("'%1' already exists in this folder. Overwrite it?").arg(name),
               QMessageBox::Yes | QMessageBox::No,
               QMessageBox::No) == QMessageBox::Yes;
}

bool FileManagementExplorerPanel::preparePasteDestination(const PasteSources& sources) {
    const FileManagementTarget target = currentTarget();
    if (!sources.raw_items.isEmpty()) {
        if (!target.local_file_system) {
            sak::showWarningLogged(this,
                                   tr("Paste"),
                                   tr("Raw-to-raw paste is not supported. Paste the files "
                                      "into a local folder first, then import them."));
            return false;
        }
        if (targetIndexForId(sources.source_target_id) < 0) {
            sak::showWarningLogged(this,
                                   tr("Paste"),
                                   tr("The copied files' source target is no longer available."));
            return false;
        }
    }
    if (!sources.host_files.isEmpty() && !target.local_file_system &&
        !confirmTypedRawImport(target, static_cast<int>(sources.host_files.size()))) {
        return false;
    }
    QString identity_blocker;
    if (!validateCurrentTargetIdentity(&identity_blocker)) {
        sak::showWarningLogged(this, tr("Paste"), identity_blocker);
        return false;
    }
    return true;
}

void FileManagementExplorerPanel::executePaste(const PasteSources& sources) {
    const FileManagementTarget target = currentTarget();
    QStringList blockers;
    int written = 0;
    int total = 0;
    if (!sources.host_files.isEmpty()) {
        total = static_cast<int>(sources.host_files.size());
        written = pasteHostFiles(target, sources.host_files, &blockers);
    } else {
        total = static_cast<int>(sources.raw_items.size());
        const int source_index = targetIndexForId(sources.source_target_id);
        written =
            pasteRawItemsToLocalFolder(m_targets.at(source_index), sources.raw_items, &blockers);
    }
    if (written > 0) {
        loadDirectory(m_current_path);
    }
    if (!blockers.isEmpty()) {
        sak::showWarningLogged(this, tr("Paste"), blockers.join(QStringLiteral("\n")));
    }
    Q_EMIT statusMessage(tr("Pasted %1 of %2 file(s).").arg(written).arg(total),
                         sak::kTimerStatusDefaultMs);
}

void FileManagementExplorerPanel::pasteClipboardIntoCurrentFolder() {
    const FileExplorerCommandState state =
        FileExplorerCommandRegistry::state(FileExplorerCommandId::Paste, commandContext());
    if (!state.enabled) {
        sak::showWarningLogged(this, tr("Paste"), state.blocker);
        return;
    }
    const PasteSources sources = collectPasteSources(QApplication::clipboard()->mimeData());
    if (sources.host_files.isEmpty() && sources.raw_items.isEmpty()) {
        sak::showWarningLogged(this, tr("Paste"), tr("Clipboard has no files to paste."));
        return;
    }
    if (!preparePasteDestination(sources)) {
        return;
    }
    executePaste(sources);
}

int FileManagementExplorerPanel::pasteHostFiles(const FileManagementTarget& target,
                                                const QStringList& source_paths,
                                                QStringList* blockers) {
    const QVector<FileManagementEntry> entries = m_item_model ? m_item_model->entries()
                                                              : QVector<FileManagementEntry>{};
    int written = 0;
    for (const QString& source : source_paths) {
        const QFileInfo info(source);
        const QString destination = targetPathForName(info.fileName());
        if (destination.isEmpty()) {
            blockers->append(tr("Invalid destination name for %1.").arg(source));
            continue;
        }
        const bool occupied = target.local_file_system
                                  ? QFile::exists(destination)
                                  : std::ranges::any_of(entries,
                                                        [&info](const FileManagementEntry& entry) {
                                                            return entry.name == info.fileName();
                                                        });
        if (occupied && !confirmPasteOverwrite(info.fileName())) {
            continue;
        }
        const auto result =
            FileManagementFileSystemBridge::writeFileFromHostPath(target, destination, source);
        if (result.ok) {
            ++written;
            m_last_mutation = result;
        } else {
            blockers->append(result.blockers);
        }
    }
    return written;
}

int FileManagementExplorerPanel::pasteRawItemsToLocalFolder(
    const FileManagementTarget& source_target,
    const QList<QPair<QString, quint64>>& items,
    QStringList* blockers) {
    int written = 0;
    for (const auto& [path, size] : items) {
        const QString name = nameForPath(path, source_target.local_file_system);
        if (size > kExplorerHashMaxBytes) {
            blockers->append(tr("%1 exceeds the raw read window; a complete paste is not "
                                "possible (use Copy Out for an explicitly capped copy).")
                                 .arg(name));
            continue;
        }
        const QString destination = targetPathForName(name);
        if (destination.isEmpty()) {
            blockers->append(tr("Invalid destination name for %1.").arg(name));
            continue;
        }
        if (QFile::exists(destination) && !confirmPasteOverwrite(name)) {
            continue;
        }
        const auto result = FileManagementFileSystemBridge::copyFileToHost(
            source_target, path, destination, kExplorerHashMaxBytes);
        if (!result.ok) {
            blockers->append(result.blockers);
            continue;
        }
        ++written;
        m_last_hash_name = name;
        m_last_hash_sha256 = result.sha256;
        m_last_hash_capped = result.capped;
    }
    return written;
}

void FileManagementExplorerPanel::exportSelectedDirectoryOut(const FileManagementEntry& entry) {
    const QString destination_root =
        QFileDialog::getExistingDirectory(this, tr("Export Folder To"), QDir::homePath());
    if (destination_root.isEmpty()) {
        return;
    }
    const QString destination = QDir(destination_root).filePath(entry.name);
    const FileManagementTarget target = currentTarget();
    const QString source_path = entry.path;
    Q_EMIT statusMessage(tr("Exporting folder %1...").arg(entry.name), sak::kTimerStatusMessageMs);

    auto* watcher = new QFutureWatcher<FileManagementDirectoryExportResult>(this);
    connect(watcher,
            &QFutureWatcher<FileManagementDirectoryExportResult>::finished,
            this,
            [this, watcher]() {
                watcher->deleteLater();
                const FileManagementDirectoryExportResult result = watcher->result();
                QStringList details;
                details.append(result.blockers);
                details.append(result.warnings);
                if (!details.isEmpty()) {
                    logMessage(details.join(QLatin1Char('\n')));
                }
                if (!result.ok) {
                    sak::showWarningLogged(this,
                                           tr("Export Folder"),
                                           details.isEmpty() ? tr("Folder export failed.")
                                                             : details.join(QLatin1Char('\n')));
                    return;
                }
                Q_EMIT statusMessage(
                    tr("Exported %1 file(s), %2 folder(s), %3 byte(s) to %4%5")
                        .arg(QString::number(result.files_exported),
                             QString::number(result.directories_created),
                             QString::number(result.bytes_written),
                             result.destination,
                             result.capped_files > 0
                                 ? tr(" (%1 file(s) capped)").arg(result.capped_files)
                                 : QString()),
                    sak::kTimerStatusDefaultMs);
            });
    watcher->setFuture(QtConcurrent::run([target, source_path, destination]() {
        return FileManagementFileSystemBridge::exportDirectoryToHost(
            target, source_path, destination, kExplorerHashMaxBytes);
    }));
}

FileManagementTarget FileManagementExplorerPanel::otherPaneTarget() const {
    if (!m_dual_pane_enabled) {
        return {};
    }
    const int index = targetIndexForId(m_secondary_state.location.target_id.value);
    return index >= 0 ? m_targets.at(index) : FileManagementTarget{};
}

void FileManagementExplorerPanel::refreshOtherPane() {
    // Reload the inactive pane's listing (its folder just changed) without disturbing
    // the user's active pane: hop over, reload, hop back.
    if (!m_dual_pane_enabled) {
        return;
    }
    const int active = m_active_pane_index;
    activatePane(active == 0 ? 1 : 0);
    const int target_index = targetIndexForId(m_pane_state.location.target_id.value);
    if (target_index >= 0) {
        m_current_target_index = target_index;
        loadDirectory(m_pane_state.location.path, false);
    }
    activatePane(active);
    m_current_target_index = targetIndexForId(m_pane_state.location.target_id.value);
}

int FileManagementExplorerPanel::crossPaneCopyEntries(const FileManagementTarget& source,
                                                      const FileManagementTarget& destination,
                                                      const QString& destination_dir,
                                                      QStringList* blockers) {
    int written = 0;
    const FileExplorerSelection selection = currentSelection();
    for (const FileManagementEntry& entry : selection.entries) {
        if (entry.directory || !entry.regular_file) {
            blockers->append(tr("Skipped %1: only files copy across panes today.").arg(entry.name));
            continue;
        }
        const QString destination_path =
            childPathFor(destination_dir, entry.name, destination.local_file_system);
        if (source.local_file_system) {
            const auto result = FileManagementFileSystemBridge::writeFileFromHostPath(
                destination, destination_path, entry.path);
            if (result.ok) {
                ++written;
                m_last_mutation = result;
            } else {
                blockers->append(result.blockers);
            }
            continue;
        }
        if (entry.size_bytes > kExplorerHashMaxBytes) {
            blockers->append(tr("%1 exceeds the raw read window; a complete cross-pane copy "
                                "is not possible.")
                                 .arg(entry.name));
            continue;
        }
        const auto exported = FileManagementFileSystemBridge::copyFileToHost(
            source, entry.path, destination_path, kExplorerHashMaxBytes);
        if (exported.ok) {
            ++written;
        } else {
            blockers->append(exported.blockers);
        }
    }
    return written;
}

void FileManagementExplorerPanel::crossPaneCopySelection() {
    const FileExplorerCommandState state = FileExplorerCommandRegistry::state(
        FileExplorerCommandId::CopyToOtherPane, commandContext());
    if (!state.enabled) {
        sak::showWarningLogged(this, tr("Copy to Other Pane"), state.blocker);
        return;
    }
    const FileManagementTarget source = currentTarget();
    const FileManagementTarget destination = otherPaneTarget();
    const QString destination_dir = m_secondary_state.location.path;
    const int file_count = static_cast<int>(currentSelection().entries.size());
    if (!destination.local_file_system && !confirmTypedRawImport(destination, file_count)) {
        return;
    }
    QStringList blockers;
    const int written = crossPaneCopyEntries(source, destination, destination_dir, &blockers);
    if (written > 0) {
        refreshOtherPane();
    }
    if (!blockers.isEmpty()) {
        sak::showWarningLogged(this, tr("Copy to Other Pane"), blockers.join(QLatin1Char('\n')));
    }
    Q_EMIT statusMessage(
        tr("Copied %1 of %2 file(s) to the other pane.").arg(written).arg(file_count),
        sak::kTimerStatusDefaultMs);
}

void FileManagementExplorerPanel::comparePanes() {
    const FileExplorerCommandState state =
        FileExplorerCommandRegistry::state(FileExplorerCommandId::ComparePanes, commandContext());
    if (!state.enabled) {
        sak::showWarningLogged(this, tr("Compare Panes"), state.blocker);
        return;
    }
    const auto listing_a = FileManagementFileSystemBridge::listDirectory(currentTarget(),
                                                                         m_current_path,
                                                                         kExplorerListMaxEntries);
    const auto listing_b = FileManagementFileSystemBridge::listDirectory(
        otherPaneTarget(), m_secondary_state.location.path, kExplorerListMaxEntries);
    if (!listing_a.ok || !listing_b.ok) {
        sak::showWarningLogged(this,
                               tr("Compare Panes"),
                               (listing_a.blockers + listing_b.blockers).join(QLatin1Char('\n')));
        return;
    }
    QHash<QString, quint64> sizes_b;
    for (const FileManagementEntry& entry : listing_b.entries) {
        sizes_b.insert(entry.name, entry.size_bytes);
    }
    QStringList only_a;
    QStringList mismatched;
    for (const FileManagementEntry& entry : listing_a.entries) {
        const auto it = sizes_b.constFind(entry.name);
        if (it == sizes_b.constEnd()) {
            only_a.append(entry.name);
        } else if (!entry.directory && *it != entry.size_bytes) {
            mismatched.append(entry.name);
        }
        sizes_b.remove(entry.name);
    }
    QStringList only_b = sizes_b.keys();
    only_b.sort(Qt::CaseInsensitive);
    const QString summary = tr("Only in this pane (%1): %2\nOnly in other pane (%3): %4\n"
                               "Size differs (%5): %6")
                                .arg(QString::number(only_a.size()),
                                     only_a.join(QStringLiteral(", ")),
                                     QString::number(only_b.size()),
                                     only_b.join(QStringLiteral(", ")),
                                     QString::number(mismatched.size()),
                                     mismatched.join(QStringLiteral(", ")));
    logMessage(summary);
    sak::showInformationLogged(this, tr("Compare Panes"), summary);
}

void FileManagementExplorerPanel::logMessage(const QString& message) {
    if (!message.trimmed().isEmpty()) {
        Q_EMIT logOutput(message);
    }
}

void FileManagementExplorerPanel::showMutationResult(const QString& title,
                                                     const FileManagementMutationResult& result) {
    // Record for the Evidence tab and force the preview to re-read (a mutation may have changed
    // the bytes of the currently selected file).
    m_last_mutation = result;
    m_last_preview_path.clear();
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
                           details.isEmpty() ? tr("Operation failed.")
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
    context.can_create_tabs = true;
    context.can_use_dual_pane = true;
    context.has_closed_tab = !m_closed_tabs.isEmpty();
    context.clipboard_has_files = clipboardHasPasteableFiles();
    context.dual_pane_active = m_dual_pane_enabled;
    context.other_pane_target = otherPaneTarget();
    return context;
}

void FileManagementExplorerPanel::applyCommandState(QPushButton* button,
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
    QMenu* menu, const FileExplorerCommandId command, const FileExplorerCommandContext& context) {
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
    connect(action, &QAction::triggered, this, [this, command]() { executeCommand(command); });
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

    appendItemSizeMenuRow(menu);

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
    auto* stackAction = menu->addAction(tr("Stack Panes Vertically"));
    stackAction->setObjectName(QStringLiteral("fileExplorerStackPanesAction"));
    stackAction->setCheckable(true);
    stackAction->setChecked(m_pane_splitter && m_pane_splitter->orientation() == Qt::Vertical);
    stackAction->setEnabled(m_dual_pane_enabled);
    stackAction->setToolTip(m_dual_pane_enabled
                                ? tr("Switch between side-by-side and stacked panes.")
                                : tr("Enable dual pane first."));
    connect(stackAction,
            &QAction::triggered,
            this,
            &FileManagementExplorerPanel::togglePaneOrientation);
    addCommandMenuAction(menu, FileExplorerCommandId::OpenInNewTab, context);
    addCommandMenuAction(menu, FileExplorerCommandId::DuplicateTab, context);
    addCommandMenuAction(menu, FileExplorerCommandId::ReopenClosedTab, context);

    const FileExplorerCommandState detailsState =
        FileExplorerCommandRegistry::state(FileExplorerCommandId::ViewDetails, context);
    m_view_button->setEnabled(detailsState.enabled);
    m_view_button->setToolTip(detailsState.enabled ? tr("Change File Explorer view layout")
                                                   : detailsState.blocker);
}

void FileManagementExplorerPanel::appendItemSizeMenuRow(QMenu* menu) {
    auto* sizeRow = new QWidget(menu);
    sizeRow->setObjectName(QStringLiteral("fileExplorerItemSizeRow"));
    auto* sizeLayout = new QHBoxLayout(sizeRow);
    sizeLayout->setContentsMargins(
        ui::kMarginSmall, ui::kSpacingTight, ui::kMarginSmall, ui::kSpacingTight);
    sizeLayout->setSpacing(ui::kSpacingSmall);
    auto* sizeLabel = new QLabel(tr("Item size"), sizeRow);
    sizeLabel->setAccessibleName(tr("Explorer item size label"));
    auto* sizeSlider = new QSlider(Qt::Horizontal, sizeRow);
    sizeSlider->setObjectName(QStringLiteral("fileExplorerItemSizeSlider"));
    sizeSlider->setAccessibleName(tr("Explorer item size"));
    sizeSlider->setRange(kFileExplorerItemSizeMin, kFileExplorerItemSizeMax);
    sizeSlider->setSingleStep(kSizeSliderSingleStep);
    sizeSlider->setPageStep(kSizeSliderPageStep);
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
}

void FileManagementExplorerPanel::executeCommand(const FileExplorerCommandId command) {
    const FileExplorerCommandState state = FileExplorerCommandRegistry::state(command,
                                                                              commandContext());
    if (!state.enabled) {
        if (!state.blocker.isEmpty()) {
            if (m_status_label) {
                m_status_label->setText(state.blocker);
            }
            Q_EMIT statusMessage(state.blocker, sak::kTimerStatusMessageMs);
        }
        return;
    }
    if (dispatchNavigationCommand(command)) {
        return;
    }
    if (dispatchSelectionCommand(command)) {
        return;
    }
    dispatchFileViewCommand(command);
}

bool FileManagementExplorerPanel::dispatchNavigationCommand(const FileExplorerCommandId command) {
    switch (command) {
    case FileExplorerCommandId::Open:
        onOpenSelected();
        return true;
    case FileExplorerCommandId::Back:
        onBackClicked();
        return true;
    case FileExplorerCommandId::Forward:
        onForwardClicked();
        return true;
    case FileExplorerCommandId::Up:
        onUpClicked();
        return true;
    case FileExplorerCommandId::Home: {
        const auto target = currentTarget();
        loadDirectory(target.local_file_system ? target.root_path : QStringLiteral("/"));
        return true;
    }
    case FileExplorerCommandId::Refresh:
        loadDirectory(m_current_path, false);
        return true;
    case FileExplorerCommandId::CopyPath:
        QApplication::clipboard()->setText(m_current_path);
        Q_EMIT statusMessage(tr("Current path copied"), sak::kTimerStatusMessageMs);
        return true;
    case FileExplorerCommandId::CopyItemPath:
        QApplication::clipboard()->setText(currentSelection().paths().join(QStringLiteral("\n")));
        Q_EMIT statusMessage(tr("Item path copied"), sak::kTimerStatusMessageMs);
        return true;
    default:
        return false;
    }
}

bool FileManagementExplorerPanel::dispatchSelectionEditCommand(
    const FileExplorerCommandId command) {
    switch (command) {
    case FileExplorerCommandId::SelectAll:
        if (auto* view = currentItemView()) {
            view->selectAll();
        }
        return true;
    case FileExplorerCommandId::ClearSelection:
        if (auto* selection_model = m_pane ? m_pane->sharedSelectionModel() : nullptr) {
            selection_model->clearSelection();
        }
        return true;
    case FileExplorerCommandId::InvertSelection:
        invertCurrentSelection();
        return true;
    default:
        return false;
    }
}

bool FileManagementExplorerPanel::dispatchSelectionCommand(const FileExplorerCommandId command) {
    switch (command) {
    case FileExplorerCommandId::Preview:
        previewSelectedFile();
        return true;
    case FileExplorerCommandId::Properties:
        showSelectedItemProperties();
        return true;
    case FileExplorerCommandId::Hash:
        hashSelectedFile();
        return true;
    case FileExplorerCommandId::CopyOut:
        copySelectedFileOut();
        return true;
    case FileExplorerCommandId::CopyItems:
        copySelectionToClipboard();
        return true;
    case FileExplorerCommandId::CopyToOtherPane:
        crossPaneCopySelection();
        return true;
    default:
        return dispatchSelectionEditCommand(command);
    }
}

bool FileManagementExplorerPanel::dispatchOpenElsewhereCommand(
    const FileExplorerCommandId command) {
    switch (command) {
    case FileExplorerCommandId::OpenInNewTab:
        openCurrentLocationInNewTab();
        return true;
    case FileExplorerCommandId::OpenInSecondPane:
        openSelectionInSecondPane();
        return true;
    case FileExplorerCommandId::ToggleDualPane:
        toggleDualPane();
        return true;
    case FileExplorerCommandId::DuplicateTab:
        duplicateCurrentTab();
        return true;
    case FileExplorerCommandId::ReopenClosedTab:
        reopenClosedTab();
        return true;
    case FileExplorerCommandId::ComparePanes:
        comparePanes();
        return true;
    default:
        return false;
    }
}

bool FileManagementExplorerPanel::dispatchWriteCommand(const FileExplorerCommandId command) {
    switch (command) {
    case FileExplorerCommandId::NewFolder:
        onNewFolderClicked();
        return true;
    case FileExplorerCommandId::WriteFile:
        onWriteFileClicked();
        return true;
    case FileExplorerCommandId::Paste:
        pasteClipboardIntoCurrentFolder();
        return true;
    case FileExplorerCommandId::Rename:
        onRenameClicked();
        return true;
    case FileExplorerCommandId::Delete:
        onDeleteClicked();
        return true;
    default:
        return false;
    }
}

bool FileManagementExplorerPanel::dispatchFileViewCommand(const FileExplorerCommandId command) {
    if (isViewModeCommand(command)) {
        setExplorerViewMode(modeForCommand(command));
        return true;
    }
    if (dispatchOpenElsewhereCommand(command)) {
        return true;
    }
    if (dispatchWriteCommand(command)) {
        return true;
    }
    switch (command) {
    case FileExplorerCommandId::TogglePreviewPane:
        togglePreviewPane();
        return true;
    case FileExplorerCommandId::ToggleHiddenItems:
        toggleHiddenItems();
        return true;
    case FileExplorerCommandId::ToggleFileExtensions:
        toggleFileExtensions();
        return true;
    default:
        return false;
    }
}

void FileManagementExplorerPanel::showSelectedItemProperties() {
    updateDetailsPane();
    if (m_details_tabs && m_properties_text) {
        m_details_tabs->setCurrentWidget(m_properties_text);
    }
}

QStringList FileManagementExplorerPanel::tagsForSelectedItem() const {
    const FileExplorerSelection selection = currentSelection();
    if (!selection.hasSingleEntry()) {
        return {};
    }
    const QString target_id = FileExplorerTargetId::fromTarget(currentTarget()).value;
    if (target_id.isEmpty()) {
        return {};
    }
    QSettings settings;
    return FileExplorerTagStore::tagsFor(
        settings, QString::fromLatin1(kTagStoreGroup), target_id, selection.entries.first().path);
}

QStringList FileManagementExplorerPanel::allKnownTags() const {
    QSettings settings;
    return FileExplorerTagStore::allTags(settings, QString::fromLatin1(kTagStoreGroup));
}

void FileManagementExplorerPanel::installTagProvider(FileExplorerItemModel* model) {
    if (!model) {
        return;
    }
    // The model stays decoupled from the tag store: it calls this lookup, which resolves
    // the current target id and reads the app-level tags for the entry path.
    model->setTagProvider([this](const QString& path) -> QStringList {
        const QString target_id = FileExplorerTargetId::fromTarget(currentTarget()).value;
        if (target_id.isEmpty() || path.trimmed().isEmpty()) {
            return {};
        }
        QSettings settings;
        return FileExplorerTagStore::tagsFor(
            settings, QString::fromLatin1(kTagStoreGroup), target_id, path);
    });
}

void FileManagementExplorerPanel::clearCurrentTagFilter() {
    if (m_pane && m_pane->sortFilterModel()) {
        m_pane->sortFilterModel()->clearTagFilter();
    }
}

void FileManagementExplorerPanel::applyTagFilter(const QString& tag) {
    if (!m_pane || !m_pane->sortFilterModel()) {
        return;
    }
    const QString target_id = FileExplorerTargetId::fromTarget(currentTarget()).value;
    QSettings settings;
    const auto items =
        FileExplorerTagStore::itemsWithTag(settings, QString::fromLatin1(kTagStoreGroup), tag);
    QSet<QString> paths;
    for (const FileExplorerTaggedItem& item : items) {
        if (item.target_id == target_id) {
            paths.insert(item.path);
        }
    }
    m_pane->sortFilterModel()->setTagFilter(paths);
    // Report what the user actually sees: matching rows in THIS folder (the tag may also
    // mark items elsewhere on the target, which the filter cannot show from here).
    const int visible = m_pane->sortFilterModel()->rowCount();
    Q_EMIT statusMessage(tr("Tag '%1': showing %2 matching item(s) in this folder")
                             .arg(tag, QString::number(visible)),
                         sak::kTimerStatusMessageMs);
}

void FileManagementExplorerPanel::editSelectedItemTags() {
    const FileExplorerSelection selection = currentSelection();
    if (!selection.hasSingleEntry()) {
        Q_EMIT statusMessage(tr("Select a single item to tag."), sak::kTimerStatusMessageMs);
        return;
    }
    const QString target_id = FileExplorerTargetId::fromTarget(currentTarget()).value;
    if (target_id.isEmpty()) {
        return;
    }
    const QString path = selection.entries.first().path;
    const QString current = tagsForSelectedItem().join(QStringLiteral(", "));
    bool accepted = false;
    const QString entered = QInputDialog::getText(this,
                                                  tr("Edit Tags"),
                                                  tr("Comma-separated tags for %1 (S.A.K. metadata "
                                                     "only, never written to the file system):")
                                                      .arg(selection.entries.first().name),
                                                  QLineEdit::Normal,
                                                  current,
                                                  &accepted);
    if (!accepted) {
        return;
    }
    const QStringList tags = entered.split(QLatin1Char(','), Qt::SkipEmptyParts);
    QSettings settings;
    FileExplorerTagStore::setTags(
        settings, QString::fromLatin1(kTagStoreGroup), target_id, path, tags);
    updateDetailsPane();
    if (m_item_model) {
        m_item_model->refreshTags();
    }
    rebuildTargetList();
    Q_EMIT statusMessage(tr("Tags updated for %1").arg(selection.entries.first().name),
                         sak::kTimerStatusMessageMs);
}

void FileManagementExplorerPanel::togglePreviewPane() {
    if (m_details_tabs) {
        m_details_tabs->setVisible(!m_details_tabs->isVisible());
    }
}

void FileManagementExplorerPanel::invertCurrentSelection() {
    auto* view = currentItemView();
    if (!view || !view->selectionModel() || !view->model()) {
        return;
    }
    auto* selection_model = view->selectionModel();
    for (int row = 0; row < view->model()->rowCount(); ++row) {
        const QModelIndex left = view->model()->index(row, 0);
        const QModelIndex right = view->model()->index(row, view->model()->columnCount() - 1);
        const QItemSelection row_selection(left, right);
        const bool selected = selection_model->isRowSelected(row, QModelIndex());
        selection_model->select(
            row_selection, selected ? QItemSelectionModel::Deselect : QItemSelectionModel::Select);
    }
}

void FileManagementExplorerPanel::toggleHiddenItems() {
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
    QTimer::singleShot(0, this, [this]() { updateActionButtons(); });
    Q_EMIT statusMessage(m_pane_state.view.show_hidden ? tr("Hidden items shown")
                                                       : tr("Hidden items hidden"),
                         sak::kTimerStatusMessageMs);
}

void FileManagementExplorerPanel::toggleFileExtensions() {
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
    QTimer::singleShot(0, this, [this]() { updateActionButtons(); });
    Q_EMIT statusMessage(m_pane_state.view.show_extensions ? tr("File extensions shown")
                                                           : tr("File extensions hidden"),
                         sak::kTimerStatusMessageMs);
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

QStringList FileManagementExplorerPanel::searchHistory() const {
    QSettings settings;
    settings.beginGroup(QString::fromLatin1(kExplorerSettingsGroup));
    const QStringList history = settings.value(QLatin1String(kSearchHistoryKey)).toStringList();
    settings.endGroup();
    return history;
}

void FileManagementExplorerPanel::rememberSearchQuery(const QString& query) {
    const QString clean = query.trimmed();
    if (clean.isEmpty()) {
        return;
    }
    QStringList history = searchHistory();
    history.removeAll(clean);
    history.prepend(clean);
    while (history.size() > kMaxSearchHistoryEntries) {
        history.removeLast();
    }
    QSettings settings;
    settings.beginGroup(QString::fromLatin1(kExplorerSettingsGroup));
    settings.setValue(QLatin1String(kSearchHistoryKey), history);
    settings.endGroup();
}

void FileManagementExplorerPanel::stopExplorerSearch() {
    if (!m_search_worker) {
        return;
    }
    m_search_worker->requestStop();
    m_search_worker->wait(5000);
    m_search_worker->deleteLater();
    m_search_worker = nullptr;
}

void FileManagementExplorerPanel::startExplorerSearch(const QString& query,
                                                      QListWidget* results,
                                                      QLabel* status) {
    stopExplorerSearch();
    results->clear();
    SearchConfig config;
    config.file_system_target = currentTarget();
    config.use_file_system_target = true;
    config.root_path = m_current_path;
    config.pattern = query;
    config.search_file_metadata = true;  // matches file NAME and path as well as content
    config.max_results = kExplorerSearchMaxResults;
    config.max_file_size = kExplorerPreviewMaxBytes;
    m_search_worker = new AdvancedSearchWorker(config, this);
    connect(m_search_worker,
            &AdvancedSearchWorker::resultsReady,
            results,
            [results](const QVector<SearchMatch>& matches) {
                for (const SearchMatch& match : matches) {
                    if (results->findItems(match.file_path, Qt::MatchFixedString).isEmpty()) {
                        results->addItem(match.file_path);
                    }
                }
            });
    connect(m_search_worker, &QThread::finished, status, [results, status]() {
        status->setText(tr("Search finished: %1 result(s).").arg(results->count()));
    });
    status->setText(tr("Searching..."));
    m_search_worker->start();
}

void FileManagementExplorerPanel::openSearchResult(const QString& path, const bool location_only) {
    const FileManagementTarget target = currentTarget();
    const QString parent = parentPathForEntry(path, target.local_file_system);
    if (!location_only) {
        // Selection happens once the (asynchronous) listing lands - see populateTable.
        m_pending_select_name = nameForPath(path, target.local_file_system);
    }
    loadDirectory(parent);
}

FileManagementExplorerPanel::SearchDialogUi FileManagementExplorerPanel::buildSearchDialogUi(
    QDialog* dialog, const FileManagementTarget& target) const {
    SearchDialogUi ui;
    dialog->setObjectName(QStringLiteral("fileExplorerSearchDialog"));
    dialog->setWindowTitle(tr("Search"));
    dialog->setMinimumWidth(sak::kDialogWidthLarge);
    auto* layout = new QVBoxLayout(dialog);

    auto* badge = new QLabel(
        tr("Target: %1  |  %2  |  under %3").arg(target.label, target.file_system, m_current_path),
        dialog);
    badge->setObjectName(QStringLiteral("fileExplorerSearchTargetBadge"));
    layout->addWidget(badge);

    auto* query_row = new QHBoxLayout();
    ui.query = new QComboBox(dialog);
    ui.query->setEditable(true);
    ui.query->setObjectName(QStringLiteral("fileExplorerSearchQuery"));
    ui.query->setAccessibleName(tr("Search text"));
    ui.query->addItems(searchHistory());
    ui.query->setCurrentText(QString());
    query_row->addWidget(ui.query, 1);
    ui.search = new QPushButton(tr("Search"), dialog);
    ui.search->setObjectName(QStringLiteral("fileExplorerSearchRunButton"));
    query_row->addWidget(ui.search);
    ui.clear = new QPushButton(tr("Clear"), dialog);
    ui.clear->setObjectName(QStringLiteral("fileExplorerSearchClearButton"));
    query_row->addWidget(ui.clear);
    layout->addLayout(query_row);

    ui.results = new QListWidget(dialog);
    ui.results->setObjectName(QStringLiteral("fileExplorerSearchResults"));
    ui.results->setAccessibleName(tr("Search results"));
    layout->addWidget(ui.results, 1);

    ui.status = new QLabel(tr("Names, paths, and text content match under the current folder."),
                           dialog);
    ui.status->setObjectName(QStringLiteral("fileExplorerSearchStatus"));
    layout->addWidget(ui.status);

    auto* action_row = new QHBoxLayout();
    ui.open = new QPushButton(tr("Open Result"), dialog);
    ui.open->setObjectName(QStringLiteral("fileExplorerSearchOpenButton"));
    action_row->addWidget(ui.open);
    ui.open_location = new QPushButton(tr("Open Location"), dialog);
    ui.open_location->setObjectName(QStringLiteral("fileExplorerSearchOpenLocationButton"));
    action_row->addWidget(ui.open_location);
    action_row->addStretch(1);
    auto* close = new QPushButton(tr("Close"), dialog);
    connect(close, &QPushButton::clicked, dialog, &QDialog::reject);
    action_row->addWidget(close);
    layout->addLayout(action_row);
    return ui;
}

void FileManagementExplorerPanel::showExplorerSearchDialog(const QString& initial_query) {
    const FileManagementTarget target = currentTarget();
    if (FileExplorerTargetId::fromTarget(target).isEmpty()) {
        Q_EMIT statusMessage(tr("Select a File Explorer target first."),
                             sak::kTimerStatusMessageMs);
        return;
    }
    QDialog dialog(this);
    const SearchDialogUi ui = buildSearchDialogUi(&dialog, target);

    const auto run_search = [this, ui]() {
        const QString query = ui.query->currentText().trimmed();
        if (query.isEmpty()) {
            ui.status->setText(tr("Type a search text first."));
            return;
        }
        rememberSearchQuery(query);
        startExplorerSearch(query, ui.results, ui.status);
    };
    connect(ui.search, &QPushButton::clicked, &dialog, run_search);
    connect(ui.query->lineEdit(), &QLineEdit::returnPressed, &dialog, run_search);
    connect(ui.clear, &QPushButton::clicked, &dialog, [this, ui]() {
        stopExplorerSearch();
        ui.results->clear();
        ui.status->setText(tr("Search cleared."));
    });
    const auto open_selected = [this, ui, &dialog](const bool location_only) {
        auto* item = ui.results->currentItem();
        if (!item) {
            ui.status->setText(tr("Select a result first."));
            return;
        }
        openSearchResult(item->text(), location_only);
        dialog.accept();
    };
    connect(ui.open, &QPushButton::clicked, &dialog, [open_selected]() { open_selected(false); });
    connect(ui.open_location, &QPushButton::clicked, &dialog, [open_selected]() {
        open_selected(true);
    });
    connect(ui.results, &QListWidget::itemDoubleClicked, &dialog, [open_selected]() {
        open_selected(false);
    });

    if (!initial_query.isEmpty()) {
        ui.query->setCurrentText(initial_query);
        run_search();
    }

    dialog.resize(sak::kDialogWidthLarge, 480);
    dialog.exec();
    stopExplorerSearch();
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
        populateCommandPalette(commands, filter->text().trimmed(), commandContext(), buttons);
    };

    connect(filter, &QLineEdit::textChanged, &dialog, rebuild);
    connect(commands, &QListWidget::currentItemChanged, &dialog, [buttons](QListWidgetItem* item) {
        buttons->button(QDialogButtonBox::Ok)
            ->setEnabled(item && item->data(kCommandEnabledRole).toBool());
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

namespace {

// Reader-provided storage detail lines (resource fork, compression, sparseness).
QStringList describeEntryStorage(const FileManagementEntry& entry) {
    QStringList lines;
    if (entry.resource_fork_bytes > 0) {
        lines.append(FileManagementExplorerPanel::tr("Resource fork: %1 bytes")
                         .arg(entry.resource_fork_bytes));
    }
    QStringList storage;
    if (entry.compressed) {
        storage.append(FileManagementExplorerPanel::tr("compressed"));
    }
    if (entry.sparse) {
        storage.append(FileManagementExplorerPanel::tr("sparse (holes)"));
    }
    if (!storage.isEmpty()) {
        lines.append(
            FileManagementExplorerPanel::tr("Storage: %1").arg(storage.join(QStringLiteral(", "))));
    }
    return lines;
}

// One-entry metadata block for the Properties tab (name, kind, size, dates, identifier, link).
QStringList describeEntry(const FileManagementEntry& entry, const QString& file_system) {
    QStringList lines;
    const QString kind = entry.directory        ? FileManagementExplorerPanel::tr("Folder")
                         : entry.symlink        ? FileManagementExplorerPanel::tr("Symbolic link")
                         : entry.type.isEmpty() ? FileManagementExplorerPanel::tr("File")
                                                : entry.type;
    lines.append(FileManagementExplorerPanel::tr("Name: %1").arg(entry.name));
    lines.append(FileManagementExplorerPanel::tr("Kind: %1").arg(kind));
    if (!entry.directory) {
        lines.append(FileManagementExplorerPanel::tr("Size: %1 bytes").arg(entry.size_bytes));
    }
    if (entry.modified_time.isValid()) {
        lines.append(FileManagementExplorerPanel::tr("Modified: %1")
                         .arg(entry.modified_time.toString(Qt::ISODate)));
    }
    if (entry.created_time.isValid()) {
        lines.append(FileManagementExplorerPanel::tr("Created: %1")
                         .arg(entry.created_time.toString(Qt::ISODate)));
    }
    if (!entry.identifier.isEmpty()) {
        lines.append(FileManagementExplorerPanel::tr("%1: %2").arg(
            FileManagementFileSystemBridge::identifierLabel(file_system), entry.identifier));
    }
    lines.append(describeEntryStorage(entry));
    if (!entry.link_target.isEmpty()) {
        lines.append(FileManagementExplorerPanel::tr("Link target: %1").arg(entry.link_target));
    }
    return lines;
}

}  // namespace

QStringList FileManagementExplorerPanel::buildDetailsProperties(
    const FileManagementTarget& target, const FileExplorerSelection& selection) const {
    QStringList properties;
    if (target.root_path.isEmpty()) {
        properties.append(tr("No target selected."));
        return properties;
    }
    properties.append(tr("Target: %1").arg(target.label));
    properties.append(tr("File system: %1").arg(target.file_system));
    properties.append(tr("Root: %1").arg(target.root_path));
    properties.append(tr("Path: %1").arg(m_current_path));
    properties.append(
        tr("Capability: %1").arg(FileManagementFileSystemBridge::capabilitySummary(target)));
    if (selection.count() == 1) {
        properties.append(QString());
        properties.append(describeEntry(selection.entries.first(), target.file_system));
        const QStringList tags = tagsForSelectedItem();
        properties.append(tags.isEmpty() ? tr("Tags: (none)")
                                         : tr("Tags: %1").arg(tags.join(QStringLiteral(", "))));
    } else if (!selection.isEmpty()) {
        properties.append(tr("Selected: %1 item(s)").arg(selection.count()));
        properties.append(selection.paths().join(QStringLiteral("\n")));
    }
    return properties;
}

QStringList FileManagementExplorerPanel::buildDetailsSafety(
    const FileManagementTarget& target) const {
    QStringList safety;
    if (target.root_path.isEmpty()) {
        safety.append(tr("No File Explorer target selected."));
        return safety;
    }
    safety.append(tr("Target: %1").arg(target.label));
    safety.append(tr("File system: %1").arg(target.file_system));
    safety.append(tr("Identity: %1").arg(target.root_path));
    safety.append(
        tr("Write state: %1").arg(target.can_write_files ? tr("enabled") : tr("blocked")));
    safety.append(tr("Read state: %1").arg(target.can_read_files ? tr("enabled") : tr("blocked")));
    safety.append(tr("Browse state: %1").arg(target.can_browse ? tr("enabled") : tr("blocked")));
    for (const QString& note : FileManagementFileSystemBridge::safetyNotes(target)) {
        safety.append(note);
    }
    if (!target.local_file_system) {
        safety.append(
            tr("Raw/non-native target: create, write, rename, and delete require "
               "explicit confirmation and commit through the certified writers; "
               "browsing and reads stay non-destructive."));
    }
    if (!target.blockers.isEmpty()) {
        safety.append(QString());
        safety.append(tr("Why some actions are unavailable:"));
        for (const QString& blocker : target.blockers) {
            safety.append(tr("- %1").arg(blocker));
        }
    }
    safety.append(QString());
    safety.append(commandAvailabilityLines());
    return safety;
}

QStringList FileManagementExplorerPanel::commandAvailabilityLines() const {
    const auto context = commandContext();
    QStringList lines;
    for (const FileExplorerCommandId command : {FileExplorerCommandId::NewFolder,
                                                FileExplorerCommandId::WriteFile,
                                                FileExplorerCommandId::Rename,
                                                FileExplorerCommandId::Delete,
                                                FileExplorerCommandId::OpenInNewTab,
                                                FileExplorerCommandId::ToggleDualPane}) {
        const FileExplorerCommandState state = FileExplorerCommandRegistry::state(command, context);
        lines.append(
            tr("%1: %2").arg(state.command.text, state.enabled ? tr("available") : state.blocker));
    }
    return lines;
}

QStringList FileManagementExplorerPanel::buildDetailsEvidence(
    const FileManagementTarget& target) const {
    QStringList evidence;
    if (!target.root_path.isEmpty()) {
        evidence.append(tr("Target ID: %1").arg(target.id));
        evidence.append(tr("Source: %1").arg(target.source));
    }
    if (!m_last_hash_sha256.isEmpty()) {
        evidence.append(QString());
        evidence.append(tr("Hashed file: %1").arg(m_last_hash_name));
        evidence.append(m_last_hash_capped
                            ? tr("SHA-256 (capped to first window): %1").arg(m_last_hash_sha256)
                            : tr("SHA-256: %1").arg(m_last_hash_sha256));
    }
    if (m_last_mutation.path.isEmpty()) {
        evidence.append(tr("No File Explorer mutation has run this session."));
        return evidence;
    }
    evidence.append(QString());
    evidence.append(tr("Last operation path: %1").arg(m_last_mutation.path));
    evidence.append(tr("Result: %1").arg(m_last_mutation.ok ? tr("ok") : tr("blocked")));
    if (m_last_mutation.bytes_written > 0) {
        evidence.append(tr("Bytes written: %1").arg(m_last_mutation.bytes_written));
    }
    if (!m_last_mutation.after_sha256.isEmpty()) {
        evidence.append(tr("SHA-256: %1").arg(m_last_mutation.after_sha256));
    }
    if (!m_last_mutation.warnings.isEmpty()) {
        evidence.append(
            tr("Warnings: %1").arg(m_last_mutation.warnings.join(QStringLiteral("; "))));
    }
    appendEvidenceReportLinks(target, &evidence);
    return evidence;
}

void FileManagementExplorerPanel::appendEvidenceReportLinks(const FileManagementTarget& target,
                                                            QStringList* evidence) {
    const QStringList reports = evidenceReportsForTarget(
        QStringLiteral("artifacts/file-management-live-certification"), target.root_path);
    if (reports.isEmpty()) {
        return;
    }
    evidence->append(QString());
    evidence->append(tr("Live certification evidence:"));
    for (const QString& report : reports) {
        evidence->append(tr("- %1").arg(report));
    }
}

QStringList FileManagementExplorerPanel::evidenceReportsForTarget(const QString& evidence_root,
                                                                  const QString& target_root_path) {
    QStringList matches;
    const QString needle = target_root_path.trimmed();
    if (needle.isEmpty()) {
        return matches;
    }
    QDirIterator it(
        evidence_root, {QStringLiteral("*.json")}, QDir::Files, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        const QString report_path = it.next();
        QFile file(report_path);
        if (!file.open(QIODevice::ReadOnly)) {
            continue;
        }
        const QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
        const QJsonArray targets = doc.object().value(QStringLiteral("targets")).toArray();
        const bool hit =
            std::any_of(targets.cbegin(), targets.cend(), [&needle](const auto& value) {
                return value.toObject().value(QStringLiteral("target_path")).toString() == needle;
            });
        if (hit) {
            matches.append(QDir::toNativeSeparators(report_path));
        }
    }
    matches.sort();
    return matches;
}

void FileManagementExplorerPanel::updateDetailsPane() {
    const auto target = currentTarget();
    const FileExplorerSelection selection = currentSelection();

    if (m_properties_text) {
        m_properties_text->setPlainText(
            buildDetailsProperties(target, selection).join(QStringLiteral("\n")));
    }
    if (m_safety_text) {
        m_safety_text->setPlainText(buildDetailsSafety(target).join(QStringLiteral("\n")));
    }
    if (m_evidence_text) {
        m_evidence_text->setPlainText(buildDetailsEvidence(target).join(QStringLiteral("\n")));
    }
    updatePreviewPane(target, selection);
    if (m_status_label) {
        m_status_label->setText(composeStatusText(target, selection));
    }
}

QString FileManagementExplorerPanel::composeStatusText(
    const FileManagementTarget& target, const FileExplorerSelection& selection) const {
    if (target.root_path.isEmpty()) {
        return tr("No target selected");
    }
    QString text = tr("%1 | %2 | %3 selected | writes %4")
                       .arg(target.label,
                            target.file_system,
                            QString::number(selection.count()),
                            target.can_write_files ? tr("enabled") : tr("blocked"));
    if (m_dual_pane_enabled) {
        const QString active_side = m_active_pane_index == 0 ? tr("Left") : tr("Right");
        const QString other_side = m_active_pane_index == 0 ? tr("Right") : tr("Left");
        const QString other_path = m_secondary_state.location.path.isEmpty()
                                       ? tr("(empty)")
                                       : m_secondary_state.location.path;
        text += tr("\nActive pane: %1  |  %2: %3").arg(active_side, other_side, other_path);
    }
    return text;
}

void FileManagementExplorerPanel::updatePreviewPane(const FileManagementTarget& target,
                                                    const FileExplorerSelection& selection) {
    if (!m_preview_text || !m_details_pane) {
        return;
    }
    if (selection.count() != 1) {
        showPreviewHint(selection.isEmpty() ? tr("Select a readable file to preview its contents.")
                                            : tr("%1 items selected.").arg(selection.count()));
        return;
    }
    const FileManagementEntry entry = selection.entries.first();
    if (entry.directory || !entry.regular_file) {
        showPreviewHint(tr("%1 is not a previewable file.").arg(entry.name));
        return;
    }
    if (entry.path == m_last_preview_path) {
        return;
    }
    const auto read =
        FileManagementFileSystemBridge::readFile(target, entry.path, kExplorerPreviewMaxBytes);
    if (!read.ok) {
        showPreviewHint(
            tr("Preview unavailable: %1").arg(read.blockers.join(QStringLiteral("; "))));
        return;
    }
    m_last_preview_path = entry.path;
    renderPreviewForEntry(entry, read.data);
}

void FileManagementExplorerPanel::showPreviewHint(const QString& message) {
    m_last_preview_path.clear();
    m_details_pane->showImagePreview(false);
    if (auto* caption = m_details_pane->previewCaption()) {
        caption->clear();
    }
    if (m_preview_text) {
        m_preview_text->setPlainText(message);
    }
}

void FileManagementExplorerPanel::renderPreviewForEntry(const FileManagementEntry& entry,
                                                        const QByteArray& bytes) {
    QImage image;
    if (image.loadFromData(bytes) && !image.isNull()) {
        showImagePreviewForEntry(entry, image);
        return;
    }
    const bool capped = bytes.size() >= kExplorerPreviewMaxBytes;
    const auto preview = FileManagementFileSystemBridge::renderPreview(bytes, capped);
    QString caption = tr("%1 - %2 bytes - %3")
                          .arg(entry.name,
                               QString::number(entry.size_bytes),
                               preview.is_binary ? tr("binary (hex)") : tr("text"));
    if (preview.truncated) {
        caption += tr(" - showing %1 bytes").arg(preview.shown_bytes);
    }
    m_details_pane->showImagePreview(false);
    if (auto* label = m_details_pane->previewCaption()) {
        label->setText(caption);
    }
    m_preview_text->setPlainText(preview.text);
}

void FileManagementExplorerPanel::showImagePreviewForEntry(const FileManagementEntry& entry,
                                                           const QImage& image) {
    if (auto* label = m_details_pane->previewImage()) {
        const QPixmap pixmap = QPixmap::fromImage(image);
        label->setPixmap(pixmap.scaled(kExplorerImagePreviewMaxPx,
                                       kExplorerImagePreviewMaxPx,
                                       Qt::KeepAspectRatio,
                                       Qt::SmoothTransformation));
    }
    if (auto* caption = m_details_pane->previewCaption()) {
        caption->setText(tr("%1 - %2 x %3 image - %4 bytes")
                             .arg(entry.name)
                             .arg(image.width())
                             .arg(image.height())
                             .arg(entry.size_bytes));
    }
    m_details_pane->showImagePreview(true);
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

int FileManagementExplorerPanel::resolveSidebarTargetIndex(QListWidgetItem* item) const {
    const auto kind = static_cast<SidebarEntryKind>(item->data(kSidebarKindRole).toInt());
    if (kind == SidebarEntryKind::Home) {
        for (int target_row = 0; target_row < m_targets.size(); ++target_row) {
            if (m_targets.at(target_row).local_file_system) {
                return target_row;
            }
        }
        return -1;
    }
    if (kind == SidebarEntryKind::Target) {
        return item->data(kTargetIndexRole).toInt();
    }
    return -1;
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
    if (static_cast<SidebarEntryKind>(item->data(kSidebarKindRole).toInt()) ==
        SidebarEntryKind::Tag) {
        applyTagFilter(item->data(kSidebarTagRole).toString());
        return;
    }
    const int target_index = resolveSidebarTargetIndex(item);
    if (target_index < 0 || target_index >= m_targets.size()) {
        return;
    }

    m_current_target_index = target_index;
    resetPaneNavigationPreservingView(&m_pane_state);
    const auto target = m_targets.at(target_index);
    rememberRecentTarget(FileExplorerTargetId::fromTarget(target).value);
    loadDirectory(target.local_file_system ? target.root_path : QStringLiteral("/"), false);
}

int FileManagementExplorerPanel::findTargetIndexById(const QString& target_id) const {
    for (int index = 0; index < m_targets.size(); ++index) {
        if (FileExplorerTargetId::fromTarget(m_targets.at(index)).value == target_id) {
            return index;
        }
    }
    return -1;
}

QString FileManagementExplorerPanel::tabTitleForCurrentLocation() const {
    const auto target = currentTarget();
    if (target.root_path.isEmpty()) {
        return tr("New Tab");
    }
    const QString leaf =
        m_current_path.section(QLatin1Char('/'), -1, -1, QString::SectionSkipEmpty);
    return leaf.isEmpty() ? target.label : leaf;
}

FileExplorerTabState FileManagementExplorerPanel::captureCurrentTab() const {
    FileExplorerTabState tab;
    tab.title = tabTitleForCurrentLocation();
    if (!m_dual_pane_enabled) {
        tab.primary = m_pane_state;
        return tab;
    }
    // m_pane_state always tracks the ACTIVE pane (states swap on activation), so un-swap
    // here: the tab's primary slot is physical pane A, secondary is pane B.
    tab.primary = m_active_pane_index == 0 ? m_pane_state : m_secondary_state;
    tab.secondary = m_active_pane_index == 0 ? m_secondary_state : m_pane_state;
    tab.secondary_pane_enabled = true;
    tab.active_pane_index = m_active_pane_index;
    tab.split = (m_pane_splitter && m_pane_splitter->orientation() == Qt::Vertical)
                    ? FileExplorerPaneSplit::Horizontal
                    : FileExplorerPaneSplit::Vertical;
    return tab;
}

void FileManagementExplorerPanel::updateActiveTabLabel() {
    if (!m_tab_bar || m_active_tab < 0 || m_active_tab >= m_tab_bar->count()) {
        return;
    }
    const QString title = tabTitleForCurrentLocation();
    m_tab_bar->setTabText(m_active_tab, title);
    m_tab_bar->setTabToolTip(m_active_tab, m_current_path);
    if (m_active_tab < m_tabs.size()) {
        m_tabs[m_active_tab].title = title;
    }
}

void FileManagementExplorerPanel::restoreTab(const FileExplorerTabState& tab) {
    m_restoring_tab = true;
    if (m_dual_pane_enabled && !tab.secondary_pane_enabled) {
        // The incoming tab is single-pane: collapse the split left over from the prior tab.
        if (m_active_pane_index == 1) {
            activatePane(0);
        }
        m_dual_pane_enabled = false;
        if (m_pane_b) {
            m_pane_b->hide();
        }
        highlightActivePane();
    }
    const int target_index = findTargetIndexById(tab.primary.location.target_id.value);
    m_current_target_index = target_index;
    m_pane_state = tab.primary;
    if (m_target_list && target_index >= 0) {
        const QSignalBlocker blocker(m_target_list);
        selectTargetById(tab.primary.location.target_id.value);
    }
    loadDirectory(tab.primary.location.path, false);
    if (tab.secondary_pane_enabled) {
        restoreSecondaryPane(tab);
    }
    m_restoring_tab = false;
}

void FileManagementExplorerPanel::restoreSecondaryPane(const FileExplorerTabState& tab) {
    // Re-open the split, restore pane B's own location/history/view, load its listing,
    // then hand focus back to whichever pane the tab recorded as active.
    ensureSecondPane();
    m_dual_pane_enabled = true;
    m_pane_b->show();
    if (m_pane_splitter) {
        m_pane_splitter->setOrientation(
            tab.split == FileExplorerPaneSplit::Horizontal ? Qt::Vertical : Qt::Horizontal);
    }
    m_secondary_state = tab.secondary;
    activatePane(1);
    const int secondary_target = findTargetIndexById(tab.secondary.location.target_id.value);
    if (secondary_target >= 0) {
        m_current_target_index = secondary_target;
        loadDirectory(m_pane_state.location.path, false);
    }
    if (tab.active_pane_index == 0) {
        activatePane(0);
        m_current_target_index = findTargetIndexById(m_pane_state.location.target_id.value);
    }
    highlightActivePane();
}

void FileManagementExplorerPanel::onTabSwitched(int index) {
    if (m_restoring_tab || index < 0 || index >= m_tabs.size() || index == m_active_tab) {
        return;
    }
    if (m_active_tab >= 0 && m_active_tab < m_tabs.size()) {
        m_tabs[m_active_tab] = captureCurrentTab();
    }
    m_active_tab = index;
    restoreTab(m_tabs.at(index));
}

void FileManagementExplorerPanel::openCurrentLocationInNewTab() {
    if (!m_tab_bar) {
        return;
    }
    if (m_active_tab >= 0 && m_active_tab < m_tabs.size()) {
        m_tabs[m_active_tab] = captureCurrentTab();
    }
    FileExplorerTabState fresh = captureCurrentTab();
    if (selectedIsDirectory()) {
        fresh.primary.location.path = selectedPath();
        fresh.primary.back_stack.clear();
        fresh.primary.forward_stack.clear();
    }
    fresh.title = tr("New Tab");
    m_tabs.append(fresh);
    m_tab_bar->addTab(fresh.title);
    nameTabCloseButtons();
    m_tab_bar->setCurrentIndex(m_tab_bar->count() - 1);
}

void FileManagementExplorerPanel::duplicateCurrentTab() {
    if (!m_tab_bar || m_active_tab < 0 || m_active_tab >= m_tabs.size()) {
        return;
    }
    // Sync the live pane into the active tab, then clone it verbatim (history + title).
    m_tabs[m_active_tab] = captureCurrentTab();
    const FileExplorerTabState copy = m_tabs.at(m_active_tab);
    const int insert_at = m_active_tab + 1;
    m_tabs.insert(insert_at, copy);
    m_tab_bar->insertTab(insert_at, copy.title);
    nameTabCloseButtons();
    m_tab_bar->setCurrentIndex(insert_at);
}

void FileManagementExplorerPanel::onTabCloseRequested(int index) {
    if (!m_tab_bar || m_tabs.size() <= 1 || index < 0 || index >= m_tabs.size()) {
        return;
    }
    // Capture the live pane if closing the active tab so a reopen is byte-accurate.
    if (index == m_active_tab) {
        m_tabs[index] = captureCurrentTab();
    }
    m_closed_tabs.append(m_tabs.at(index));
    const QSignalBlocker blocker(m_tab_bar);
    m_tabs.remove(index);
    m_tab_bar->removeTab(index);
    m_active_tab = m_tab_bar->currentIndex();
    if (m_active_tab >= 0 && m_active_tab < m_tabs.size()) {
        restoreTab(m_tabs.at(m_active_tab));
    }
}

void FileManagementExplorerPanel::reopenClosedTab() {
    if (!m_tab_bar || m_closed_tabs.isEmpty()) {
        return;
    }
    if (m_active_tab >= 0 && m_active_tab < m_tabs.size()) {
        m_tabs[m_active_tab] = captureCurrentTab();
    }
    const FileExplorerTabState tab = m_closed_tabs.takeLast();
    m_tabs.append(tab);
    m_tab_bar->addTab(tab.title);
    nameTabCloseButtons();
    m_tab_bar->setCurrentIndex(m_tab_bar->count() - 1);
}

void FileManagementExplorerPanel::saveTabSession() const {
    if (!m_tab_session_persistence || !m_tab_bar) {
        return;
    }
    FileExplorerTabSession session;
    session.tabs = m_tabs;
    // Fold the live active-pane state into the active tab so an unswitched tab
    // still persists its current location.
    if (m_active_tab >= 0 && m_active_tab < session.tabs.size()) {
        session.tabs[m_active_tab] = captureCurrentTab();
    }
    session.active_index = m_active_tab;

    QSettings settings;
    const QString group = QString::fromLatin1(kTabSessionGroup);
    // Only a genuinely-navigated session is worth persisting; a lone empty tab
    // clears any stale saved session instead of pinning "New Tab" forever.
    const bool worth_saving = session.tabs.size() > 1 ||
                              (!session.tabs.isEmpty() &&
                               !session.tabs.first().primary.location.isEmpty());
    if (worth_saving) {
        FileExplorerSessionStore::save(settings, group, session);
    } else {
        FileExplorerSessionStore::clear(settings, group);
    }
}

void FileManagementExplorerPanel::restoreTabSession() {
    if (!m_tab_session_persistence || !m_tab_bar) {
        return;
    }
    QSettings settings;
    const FileExplorerTabSession session =
        FileExplorerSessionStore::load(settings, QString::fromLatin1(kTabSessionGroup));
    if (session.tabs.isEmpty()) {
        return;
    }

    const QSignalBlocker blocker(m_tab_bar);
    while (m_tab_bar->count() > 0) {
        m_tab_bar->removeTab(0);
    }
    m_tabs.clear();
    for (const FileExplorerTabState& tab : session.tabs) {
        m_tabs.append(tab);
        m_tab_bar->addTab(tab.title.isEmpty() ? tr("New Tab") : tab.title);
    }
    nameTabCloseButtons();
    m_active_tab = std::clamp(session.active_index, 0, static_cast<int>(m_tabs.size()) - 1);
    m_tab_bar->setCurrentIndex(m_active_tab);
    restoreTab(m_tabs.at(m_active_tab));
}

void FileManagementExplorerPanel::ensureSecondPane() {
    if (m_pane_b) {
        return;
    }
    m_pane_b = new FileExplorerPane(m_pane_splitter);
    m_pane_splitter->addWidget(m_pane_b);
    installTagProvider(m_pane_b->itemModel());
    connectPaneSignals(m_pane_b, 1);
}

void FileManagementExplorerPanel::activatePane(int index) {
    if (index == m_active_pane_index || index < 0 || index > 1 || !m_pane_a) {
        return;
    }
    if (index == 1 && !m_pane_b) {
        return;
    }
    std::swap(m_pane_state, m_secondary_state);
    m_active_pane_index = index;
    m_pane = (index == 0) ? m_pane_a : m_pane_b;
    m_item_model = m_pane->itemModel();
    installTagProvider(m_item_model);
    m_status_label = m_pane->statusLabel();
    m_current_path = m_pane_state.location.path;
    if (m_path_edit) {
        m_path_edit->setText(m_current_path);
    }
    applyViewSettings();  // re-apply the now-active pane's own view mode/size/toggles
    highlightActivePane();
    updateActionButtons();
}

void FileManagementExplorerPanel::togglePaneOrientation() {
    if (!m_pane_splitter) {
        return;
    }
    const bool stacked = m_pane_splitter->orientation() == Qt::Vertical;
    m_pane_splitter->setOrientation(stacked ? Qt::Horizontal : Qt::Vertical);
    Q_EMIT statusMessage(stacked ? tr("Panes arranged side by side")
                                 : tr("Panes stacked vertically"),
                         sak::kTimerStatusMessageMs);
}

void FileManagementExplorerPanel::highlightActivePane() {
    if (!m_pane_a) {
        return;
    }
    const QString border = QStringLiteral("FileExplorerPane { border: 1px solid %1; }")
                               .arg(QString::fromLatin1(ui::kColorAccentWindows));
    m_pane_a->setStyleSheet(m_dual_pane_enabled && m_active_pane_index == 0 ? border : QString());
    if (m_pane_b) {
        m_pane_b->setStyleSheet(m_dual_pane_enabled && m_active_pane_index == 1 ? border
                                                                                : QString());
    }
}

void FileManagementExplorerPanel::toggleDualPane() {
    if (!m_dual_pane_enabled) {
        ensureSecondPane();
        m_dual_pane_enabled = true;
        m_pane_b->show();
        m_secondary_state = m_pane_state;
        m_secondary_state.back_stack.clear();
        m_secondary_state.forward_stack.clear();
        activatePane(1);
        loadDirectory(m_pane_state.location.path, false);
        return;
    }
    if (m_active_pane_index == 1) {
        activatePane(0);
    }
    m_dual_pane_enabled = false;
    m_pane_b->hide();
    highlightActivePane();
}

void FileManagementExplorerPanel::openSelectionInSecondPane() {
    const QString path = selectedIsDirectory() ? selectedPath() : m_current_path;
    if (!m_dual_pane_enabled) {
        ensureSecondPane();
        m_dual_pane_enabled = true;
        m_pane_b->show();
        m_secondary_state = m_pane_state;
        m_secondary_state.back_stack.clear();
        m_secondary_state.forward_stack.clear();
    }
    activatePane(1);
    loadDirectory(path, false);
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
        sak::showWarningLogged(this, tr("New Folder"), target.blockers.join(QStringLiteral("\n")));
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
        sak::showWarningLogged(this,
                               tr("New Folder"),
                               tr("Enter a folder name without path separators."));
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
        sak::showWarningLogged(this, tr("Write File"), target.blockers.join(QStringLiteral("\n")));
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
    if (!sourceInfo.isFile()) {
        sak::showWarningLogged(this,
                               tr("Write File"),
                               tr("Unable to read source file: %1").arg(sourcePath));
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
        sak::showWarningLogged(this,
                               tr("Write File"),
                               tr("Enter a file name without path separators."));
        return;
    }
    const auto result =
        FileManagementFileSystemBridge::writeFileFromHostPath(target, targetPath, sourcePath);
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

int FileManagementExplorerPanel::deleteSelectedEntries(const FileManagementTarget& target,
                                                       const FileExplorerSelection& selection,
                                                       QStringList* blockers,
                                                       QStringList* warnings) {
    int deleted = 0;
    for (const FileManagementEntry& entry : selection.entries) {
        const auto result =
            entry.directory ? FileManagementFileSystemBridge::deleteDirectory(target, entry.path)
                            : FileManagementFileSystemBridge::deleteFile(target, entry.path);
        blockers->append(result.blockers);
        warnings->append(result.warnings);
        if (result.ok) {
            ++deleted;
            logMessage(tr("Delete Entry: %1").arg(entry.path));
        }
    }
    return deleted;
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
    const auto response =
        sak::showQuestionLogged(this,
                                tr("Delete Entry"),
                                tr("Delete %1 item(s) from '%2'? This permanently removes data "
                                   "from the selected target.\n\n%3")
                                    .arg(QString::number(selection.count()),
                                         target.label,
                                         paths.join(QStringLiteral("\n"))),
                                QMessageBox::Yes | QMessageBox::No,
                                QMessageBox::No);
    if (response != QMessageBox::Yes) {
        return;
    }
    QStringList blockers;
    QStringList warnings;
    const int deleted = deleteSelectedEntries(target, selection, &blockers, &warnings);

    if (deleted == selection.count()) {
        Q_EMIT statusMessage(tr("Delete Entry complete"), sak::kTimerStatusDefaultMs);
        loadDirectory(m_current_path);
        return;
    }

    QStringList details;
    details.append(blockers);
    details.append(warnings);
    sak::showWarningLogged(this,
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
    addCommandMenuAction(&menu, FileExplorerCommandId::CopyToOtherPane, context);
    addCommandMenuAction(&menu, FileExplorerCommandId::ComparePanes, context);
    menu.addSeparator();
    addCommandMenuAction(&menu, FileExplorerCommandId::Preview, context);
    addCommandMenuAction(&menu, FileExplorerCommandId::Properties, context);
    addCommandMenuAction(&menu, FileExplorerCommandId::Hash, context);
    addCommandMenuAction(&menu, FileExplorerCommandId::CopyOut, context);
    addCommandMenuAction(&menu, FileExplorerCommandId::CopyItems, context);
    addCommandMenuAction(&menu, FileExplorerCommandId::Paste, context);
    addCommandMenuAction(&menu, FileExplorerCommandId::CopyItemPath, context);
    addCommandMenuAction(&menu, FileExplorerCommandId::CopyPath, context);
    auto* editTags = menu.addAction(tr("Edit Tags..."));
    editTags->setObjectName(QStringLiteral("fileExplorerEditTagsAction"));
    editTags->setEnabled(context.pane.selection.hasSingleEntry());
    editTags->setToolTip(
        tr("Tag this item with S.A.K. metadata (never written to the file "
           "system)."));
    connect(
        editTags, &QAction::triggered, this, &FileManagementExplorerPanel::editSelectedItemTags);
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

int FileManagementExplorerPanel::resolveContextMenuTargetIndex(const QPoint& position) {
    if (const QModelIndex index = m_target_list->indexAt(position); index.isValid()) {
        auto* item = m_target_list->item(index.row());
        if (item) {
            const auto kind = static_cast<SidebarEntryKind>(item->data(kSidebarKindRole).toInt());
            if (kind == SidebarEntryKind::Target) {
                m_target_list->setCurrentRow(index.row());
                return item->data(kTargetIndexRole).toInt();
            }
        }
    }
    if (m_current_target_index >= 0 && m_current_target_index < m_targets.size()) {
        return m_current_target_index;
    }
    return -1;
}

QString FileManagementExplorerPanel::favoriteActionLabel(const int target_index,
                                                         const bool has_target) const {
    const bool pinned = has_target &&
                        m_favorite_target_ids.contains(
                            FileExplorerTargetId::fromTarget(m_targets.at(target_index)).value);
    return pinned ? tr("Unpin Favorite") : tr("Pin Favorite");
}

void FileManagementExplorerPanel::openTargetAtIndex(const int target_index) {
    if (target_index < 0 || target_index >= m_targets.size()) {
        return;
    }
    m_current_target_index = target_index;
    const auto target = m_targets.at(target_index);
    rememberRecentTarget(FileExplorerTargetId::fromTarget(target).value);
    loadDirectory(target.local_file_system ? target.root_path : QStringLiteral("/"));
}

void FileManagementExplorerPanel::copyTargetRootAtIndex(const int target_index) {
    if (target_index < 0 || target_index >= m_targets.size()) {
        return;
    }
    QApplication::clipboard()->setText(m_targets.at(target_index).root_path);
    Q_EMIT statusMessage(tr("Target root copied"), sak::kTimerStatusMessageMs);
}

void FileManagementExplorerPanel::toggleFavoriteAtIndex(const int target_index) {
    if (target_index < 0 || target_index >= m_targets.size()) {
        return;
    }
    const QString target_id = FileExplorerTargetId::fromTarget(m_targets.at(target_index)).value;
    if (m_favorite_target_ids.contains(target_id)) {
        m_favorite_target_ids.removeAll(target_id);
    } else {
        m_favorite_target_ids.prepend(target_id);
    }
    saveSidebarState();
    rebuildTargetList(target_id);
}

bool FileManagementExplorerPanel::isFavoriteTargetIndex(const int target_index) const {
    if (target_index < 0 || target_index >= m_targets.size()) {
        return false;
    }
    return m_favorite_target_ids.contains(
        FileExplorerTargetId::fromTarget(m_targets.at(target_index)).value);
}

void FileManagementExplorerPanel::moveFavoriteAtIndex(const int target_index, const int delta) {
    if (target_index < 0 || target_index >= m_targets.size()) {
        return;
    }
    const QString target_id = FileExplorerTargetId::fromTarget(m_targets.at(target_index)).value;
    const int position = m_favorite_target_ids.indexOf(target_id);
    const int destination = position + delta;
    if (position < 0 || destination < 0 || destination >= m_favorite_target_ids.size()) {
        return;
    }
    m_favorite_target_ids.move(position, destination);
    saveSidebarState();
    rebuildTargetList(target_id);
}

void FileManagementExplorerPanel::clearRecentTargets() {
    if (m_recent_target_ids.isEmpty()) {
        return;
    }
    m_recent_target_ids.clear();
    saveSidebarState();
    rebuildTargetList();
    Q_EMIT statusMessage(tr("Recent target list cleared"), sak::kTimerStatusMessageMs);
}

void FileManagementExplorerPanel::showTargetPropertiesAtIndex(const int target_index) {
    if (target_index < 0 || target_index >= m_targets.size()) {
        return;
    }
    const auto target = m_targets.at(target_index);
    QStringList lines;
    lines.append(tr("Target: %1").arg(target.label));
    lines.append(tr("ID: %1").arg(FileExplorerTargetId::fromTarget(target).value));
    lines.append(tr("Root: %1").arg(target.root_path));
    lines.append(tr("File system: %1").arg(target.file_system));
    lines.append(tr("Source: %1").arg(target.source));
    lines.append(tr("Size: %1 bytes").arg(QString::number(target.size_bytes)));
    lines.append(
        tr("Capability: %1").arg(FileManagementFileSystemBridge::capabilitySummary(target)));
    if (!target.blockers.isEmpty()) {
        lines.append(tr("Blockers: %1").arg(target.blockers.join(QStringLiteral("; "))));
    }
    sak::showInformationLogged(this, tr("Target Properties"), lines.join(QStringLiteral("\n")));
}

// A stale "[offline]" favorite has no connected target index, but the pin itself must
// stay removable; show its dedicated menu and return true when the row was a stale pin.
bool FileManagementExplorerPanel::showStaleFavoriteContextMenu(const QPoint& position) {
    const QModelIndex index = m_target_list->indexAt(position);
    if (!index.isValid()) {
        return false;
    }
    auto* item = m_target_list->item(index.row());
    if (!item || static_cast<SidebarEntryKind>(item->data(kSidebarKindRole).toInt()) !=
                     SidebarEntryKind::StaleFavorite) {
        return false;
    }
    const QString stale_id = item->data(kSidebarTagRole).toString();
    QMenu stale_menu(this);
    stale_menu.setObjectName(QStringLiteral("fileExplorerStaleFavoriteContextMenu"));
    auto* remove = stale_menu.addAction(tr("Remove from Favorites"));
    remove->setObjectName(QStringLiteral("fileExplorerRemoveStaleFavorite"));
    connect(remove, &QAction::triggered, this, [this, stale_id]() {
        m_favorite_target_ids.removeAll(stale_id);
        saveSidebarState();
        rebuildTargetList();
        Q_EMIT statusMessage(tr("Removed offline favorite %1").arg(stale_id),
                             sak::kTimerStatusMessageMs);
    });
    stale_menu.exec(m_target_list->viewport()->mapToGlobal(position));
    return true;
}

void FileManagementExplorerPanel::onTargetContextMenuRequested(const QPoint& position) {
    if (!m_target_list) {
        return;
    }
    if (showStaleFavoriteContextMenu(position)) {
        return;
    }
    const int menu_target_index = resolveContextMenuTargetIndex(position);
    const bool has_menu_target = menu_target_index >= 0 && menu_target_index < m_targets.size();

    QMenu menu(this);
    menu.setObjectName(QStringLiteral("fileExplorerTargetContextMenu"));
    auto* open = menu.addAction(tr("Open Target"));
    open->setEnabled(has_menu_target);
    connect(open, &QAction::triggered, this, [this, menu_target_index]() {
        openTargetAtIndex(menu_target_index);
    });
    auto* copyRoot = menu.addAction(tr("Copy Target Root"));
    copyRoot->setEnabled(has_menu_target && !m_targets.at(menu_target_index).root_path.isEmpty());
    connect(copyRoot, &QAction::triggered, this, [this, menu_target_index]() {
        copyTargetRootAtIndex(menu_target_index);
    });
    auto* favorite = menu.addAction(favoriteActionLabel(menu_target_index, has_menu_target));
    favorite->setEnabled(has_menu_target);
    connect(favorite, &QAction::triggered, this, [this, menu_target_index]() {
        toggleFavoriteAtIndex(menu_target_index);
    });
    const bool is_favorite = has_menu_target && isFavoriteTargetIndex(menu_target_index);
    auto* moveUp = menu.addAction(tr("Move Favorite Up"));
    moveUp->setObjectName(QStringLiteral("fileExplorerMoveFavoriteUp"));
    moveUp->setEnabled(is_favorite);
    connect(moveUp, &QAction::triggered, this, [this, menu_target_index]() {
        moveFavoriteAtIndex(menu_target_index, -1);
    });
    auto* moveDown = menu.addAction(tr("Move Favorite Down"));
    moveDown->setObjectName(QStringLiteral("fileExplorerMoveFavoriteDown"));
    moveDown->setEnabled(is_favorite);
    connect(moveDown, &QAction::triggered, this, [this, menu_target_index]() {
        moveFavoriteAtIndex(menu_target_index, 1);
    });
    auto* properties = menu.addAction(tr("Target Properties"));
    properties->setEnabled(has_menu_target);
    connect(properties, &QAction::triggered, this, [this, menu_target_index]() {
        showTargetPropertiesAtIndex(menu_target_index);
    });
    menu.addSeparator();
    auto* refresh = menu.addAction(tr("Refresh Mounted Targets"));
    connect(
        refresh, &QAction::triggered, this, &FileManagementExplorerPanel::onRefreshMountedTargets);
    auto* scan = menu.addAction(tr("Scan Disks"));
    connect(scan, &QAction::triggered, this, &FileManagementExplorerPanel::onScanDiskTargets);
    auto* addManual = menu.addAction(tr("Add Raw/Image"));
    connect(addManual, &QAction::triggered, this, &FileManagementExplorerPanel::onAddManualTarget);
    auto* clearRecent = menu.addAction(tr("Clear Recent"));
    clearRecent->setObjectName(QStringLiteral("fileExplorerClearRecent"));
    clearRecent->setEnabled(!m_recent_target_ids.isEmpty());
    connect(
        clearRecent, &QAction::triggered, this, &FileManagementExplorerPanel::clearRecentTargets);
    menu.exec(m_target_list->viewport()->mapToGlobal(position));
}

void FileManagementExplorerPanel::onItemDoubleClicked(const QModelIndex& index) {
    Q_UNUSED(index)
    onOpenSelected();
}

}  // namespace sak
