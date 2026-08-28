// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file file_management_explorer_panel.cpp
/// @brief File Management explorer tab with mounted and raw/image targets.

#include "sak/file_management_explorer_panel.h"

#include "sak/advanced_search_worker.h"
#include "sak/app_action_guards.h"
#include "sak/drive_unmounter.h"
#include "sak/file_explorer_archive_service.h"
#include "sak/file_explorer_archive_worker.h"
#include "sak/file_explorer_breadcrumb.h"
#include "sak/file_explorer_icon_registry.h"
#include "sak/file_explorer_layout_metrics.h"
#include "sak/file_explorer_properties_dialog.h"
#include "sak/file_explorer_session_store.h"
#include "sak/file_explorer_status_center_widget.h"
#include "sak/file_explorer_style.h"
#include "sak/file_explorer_style_constants.h"
#include "sak/file_explorer_tag_store.h"
#include "sak/layout_constants.h"
#include "sak/logger.h"
#include "sak/message_box_helpers.h"
#include "sak/process_runner.h"
#include "sak/recycle_bin.h"
#include "sak/rich_text_safety.h"
#include "sak/storage_inventory_worker.h"
#include "sak/style_constants.h"
#include "sak/widget_helpers.h"
// sak::isSafeChildName -- the shared rule for a name that must land as a single child of the
// browsed directory. This panel used to carry a weaker copy that let "..", "." and ':' through.
#include "sak/windows_path_policy.h"

#include <QAbstractItemView>
#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QBuffer>
#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QCryptographicHash>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QDirIterator>
#include <QDrag>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QFutureWatcher>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QImage>
#include <QImageReader>
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
#include <QMouseEvent>
#include <QPixmap>
#include <QPlainTextEdit>
#include <QPointer>
#include <QProcess>
#include <QProgressDialog>
#include <QRegularExpression>
#include <QSet>
#include <QSettings>
#include <QShortcut>
#include <QSlider>
#include <QSplitter>
#include <QStandardPaths>
#include <QStyle>
#include <QTabBar>
#include <QTableView>
#include <QtConcurrent>
#include <QTemporaryDir>
#include <QTimer>
#include <QToolButton>
#include <QUrl>
#include <QVariant>
#include <QVBoxLayout>
#include <QWidgetAction>

#include <algorithm>
#include <array>
#include <memory>
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
// Position of a favorites-section row inside m_favorite_target_ids; only set
// on rows in the Favorites section (drag-reorder scope).
constexpr int kSidebarFavoritePosRole = Qt::UserRole + 8;
// Internal drag payload for reordering favorites within the sidebar.
constexpr const char* kSidebarFavoriteMime = "application/x-sak-explorer-favorite";
constexpr int kCommandIdRole = Qt::UserRole + 3;
constexpr int kCommandEnabledRole = Qt::UserRole + 4;

// Shell layout + control constants.
constexpr int kViewIdDigestChars = 24;
constexpr int kCenterPaneStretchIndex = 2;
constexpr int kSidebarCollapseWidth = 720;
constexpr int kDetailsTabsCollapseWidth = 920;
// Files NavigationToolbar narrow breakpoint is a 540px window; the explorer
// panel sits inside the app shell, so the effective threshold is higher.
constexpr int kNavClusterCollapseWidth = 640;
constexpr int kMaxRecentTargetIds = 10;
// Files BaseLayoutPage tapDebounceTimer: slow second click on the selected
// item's name starts an inline rename after 1500 ms.
constexpr int kRenameTapDebounceMs = 1500;
// Files SpringLoaded timer: hovering a folder for 1300 ms during a drag opens it.
constexpr int kSpringOpenMs = 1300;
constexpr const char* kExplorerSettingsGroup = "FileManagementExplorer";
constexpr const char* kTabSessionGroup = "FileManagementExplorer/TabSession";
constexpr const char* kTagStoreGroup = "FileManagementExplorer/Tags";
constexpr const char* kFavoriteTargetIdsKey = "FavoriteTargetIds";
constexpr const char* kRecentTargetIdsKey = "RecentTargetIds";
constexpr const char* kLastTargetIdKey = "LastTargetId";
constexpr const char* kViewModeKey = "ViewMode";
constexpr const char* kShowHiddenKey = "ShowHiddenItems";
constexpr const char* kShowExtensionsKey = "ShowFileExtensions";
// Files ILayoutSettingsService stores one size kind per layout; the key names
// mirror the Files setting names.
constexpr const char* kDetailsSizeKey = "DetailsViewSize";
constexpr const char* kListSizeKey = "ListViewSize";
constexpr const char* kCardsSizeKey = "CardsViewSize";
constexpr const char* kGridSizeKey = "GridViewSize";
constexpr const char* kColumnsSizeKey = "ColumnsViewSize";
// Files per-folder grouping preferences (GroupOption/GroupDirection/
// GroupByDateUnit setting names).
constexpr const char* kGroupOptionKey = "GroupOption";
constexpr const char* kGroupDirectionKey = "GroupDirection";
constexpr const char* kGroupDateUnitKey = "GroupByDateUnit";
// Files SortDirectoriesAlongsideFiles / SortFilesFirst pair, folded into one
// three-state placement.
constexpr const char* kFolderPlacementKey = "FolderSortPlacement";
// Files sidebar display mode (Compact icon rail vs Expanded), persisted.
constexpr const char* kSidebarCompactKey = "SidebarCompact";
// Files global settings: selection checkboxes (FoldersSettingsService,
// default true) and the flatten-folder opt-in (GeneralSettingsService,
// default false; surfaced on the C6 settings page).
constexpr const char* kShowCheckboxesKey = "ShowCheckboxesWhenSelectingItems";
constexpr const char* kShowFlattenKey = "ShowFlattenOptions";
// Files GeneralSettingsService.ShowFilterHeader (default false).
constexpr const char* kShowFilterHeaderKey = "ShowFilterHeader";
// Files FoldersSettingsService.DoubleClickToGoUp (default true).
constexpr const char* kDoubleClickToGoUpKey = "DoubleClickToGoUp";
// Files AppearanceSettingsService.StatusCenterVisibility (default Always=0;
// 1 = DuringOngoingFileOperations hides the button while idle).
constexpr const char* kStatusCenterVisibilityKey = "StatusCenterVisibility";
// Files search-mode suggestion debounce (200 ms) and cap (FolderSearch
// MaxItemCount 10); FilterHeader debounce is 250 ms with a 180px box.
constexpr int kSearchSuggestDebounceMs = 200;
constexpr int kOmnibarSearchSuggestCap = 10;
constexpr int kFilterDebounceMs = 250;
constexpr int kFilterBoxWidth = 180;
// Suggestion rows that map to a real file carry its path in this role.
constexpr int kSearchPathRole = Qt::UserRole + 9;
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
    const int slash = static_cast<int>(trimmed.lastIndexOf(QLatin1Char('/')));
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

// True when @p path equals @p ancestor or sits anywhere under it. Files
// ShellFilesystemOperations refuses transferring a folder into its own subtree,
// which would otherwise recurse into the growing copy.
bool pathContains(const QString& ancestor, const QString& path, bool local) {
    QString base = ancestor;
    QString candidate = path;
    base.replace(QLatin1Char('\\'), QLatin1Char('/'));
    candidate.replace(QLatin1Char('\\'), QLatin1Char('/'));
    if (local) {
        base = QDir::cleanPath(base);
        candidate = QDir::cleanPath(candidate);
    }
    if (!base.endsWith(QLatin1Char('/'))) {
        base.append(QLatin1Char('/'));
    }
    candidate.append(QLatin1Char('/'));
    // Local Windows paths compare case-insensitively, and raw APFS/HFS+ volumes are
    // case-insensitive by default (a case-sensitive APFS or HFSX volume is the exception),
    // so the raw comparison is case-insensitive too: over-detecting containment only
    // refuses a transfer, while under-detecting lets a folder be moved or copied into
    // its own subtree and recurse into the growing copy.
    return candidate.startsWith(base, Qt::CaseInsensitive);
}

// Lazy drag-out materialization for raw (non-mounted) sources, mirroring the Files
// VirtualStorageItem deferred DataTransfer: the exports through the certified readers
// run only when an external drop target actually requests the file URLs; an in-app
// drop reads the internal payload and never touches this path. The staged copies
// live as long as the drag's mime object.
class RawExportMimeData : public QMimeData {
public:
    RawExportMimeData(FileManagementTarget target, QVector<FileManagementEntry> items)
        : m_target(std::move(target)), m_items(std::move(items)) {}

    [[nodiscard]] QStringList formats() const override {
        QStringList formats = QMimeData::formats();
        if (!formats.contains(QStringLiteral("text/uri-list"))) {
            formats.append(QStringLiteral("text/uri-list"));
        }
        return formats;
    }

    [[nodiscard]] bool hasFormat(const QString& mimetype) const override {
        return mimetype == QStringLiteral("text/uri-list") || QMimeData::hasFormat(mimetype);
    }

protected:
    QVariant retrieveData(const QString& mimetype, QMetaType type) const override {
        if (mimetype == QStringLiteral("text/uri-list")) {
            materialize();
        }
        return QMimeData::retrieveData(mimetype, type);
    }

private:
    void materialize() const {
        if (m_materialized) {
            return;
        }
        m_materialized = true;
        if (!m_staging.isValid()) {
            return;
        }
        QList<QUrl> urls;
        for (const FileManagementEntry& item : m_items) {
            // item.name comes verbatim from a parsed foreign image. exportDirectoryToHost
            // confines the names it walks INTO, but the top-level name is built here, so
            // without this the drag-out is the one export path that never applies the rule:
            // QDir::filePath does not collapse "..", so a raw entry named "../evil" resolves
            // outside the staging directory and QSaveFile writes there.
            const QString staged_name = FileManagementFileSystemBridge::confinedHostName(item.name);
            if (staged_name.isEmpty()) {
                sak::logWarning("Raw drag-out skipped entry with unsafe name: {}",
                                item.path.toStdString());
                continue;
            }
            const QString staged = QDir(m_staging.path()).filePath(staged_name);
            // Publish only a WHOLE export. A raw file past the read cap comes back ok with
            // capped=true, and a directory walk reports complete=false when anything was
            // truncated or skipped -- handing either to an external drop target would deliver
            // a silently short file as if it were the real one.
            bool whole = false;
            if (item.directory) {
                const FileManagementDirectoryExportResult exported =
                    FileManagementFileSystemBridge::exportDirectoryToHost(
                        m_target, item.path, staged, kExplorerHashMaxBytes);
                whole = rawDirectoryExportIsWhole(exported);
            } else {
                const FileManagementExportResult exported =
                    FileManagementFileSystemBridge::copyFileToHost(
                        m_target, item.path, staged, kExplorerHashMaxBytes);
                whole = rawFileExportIsWhole(exported);
            }
            if (whole) {
                urls.append(QUrl::fromLocalFile(staged));
            } else {
                sak::logWarning("Raw drag-out skipped incomplete export of {}",
                                item.path.toStdString());
            }
        }
        if (!urls.isEmpty()) {
            const_cast<RawExportMimeData*>(this)->setUrls(urls);
        }
    }

    FileManagementTarget m_target;
    QVector<FileManagementEntry> m_items;
    QTemporaryDir m_staging;
    mutable bool m_materialized{false};
};

QString nameForPath(const QString& path, bool local) {
    if (local) {
        return QFileInfo(path).fileName();
    }
    QString clean = path;
    clean.replace(QLatin1Char('\\'), QLatin1Char('/'));
    if (clean.endsWith(QLatin1Char('/')) && clean.size() > 1) {
        clean.chop(1);
    }
    const int slash = static_cast<int>(clean.lastIndexOf(QLatin1Char('/')));
    return slash >= 0 ? clean.mid(slash + 1) : clean;
}

// Files FilesystemHelpers.RestrictedFileNames: reserved DOS device names,
// alone or followed by a dot/extension.
bool isReservedWindowsName(const QString& name) {
    static const QRegularExpression kReserved(QStringLiteral(
                                                  "^(CON|PRN|AUX|NUL|COM[1-9]|LPT[1-9])(\\.|$)"),
                                              QRegularExpression::CaseInsensitiveOption);
    return kReserved.match(name.trimmed()).hasMatch();
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

QString sidebarIconKeyForTarget(const FileManagementTarget& target) {
    if (target.kind == FileManagementTargetKind::ImageFile) {
        return QStringLiteral("image-file");
    }
    if (isCertificationTarget(target)) {
        return QStringLiteral("shield");
    }
    if (isLocalFsTarget(target) || isMountedVolumeTarget(target) || isPartitionTarget(target)) {
        return QStringLiteral("drive");
    }
    return QStringLiteral("file");
}

// Adds one command-palette row; returns its row index when enabled, else -1.
QString locationViewSettingsGroup(const FileExplorerLocation& location) {
    const QString raw = QStringLiteral("%1\n%2").arg(location.target_id.value, location.path);
    const QByteArray digest =
        QCryptographicHash::hash(raw.toUtf8(), QCryptographicHash::Sha256).toHex();
    return QStringLiteral("View/%1").arg(QString::fromLatin1(digest.left(kViewIdDigestChars)));
}

void selectRowInView(QAbstractItemView* view, const int row) {
    if ((view == nullptr) || (view->model() == nullptr) || (view->selectionModel() == nullptr) ||
        row < 0 || row >= view->model()->rowCount()) {
        return;
    }

    const QModelIndex left = view->model()->index(row, 0);
    const QModelIndex right = view->model()->index(row, view->model()->columnCount() - 1);
    view->selectionModel()->select(QItemSelection(left, right),
                                   QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
    view->setCurrentIndex(left);
}

void resetPaneNavigationPreservingView(FileExplorerPaneState* state) {
    if (state == nullptr) {
        return;
    }
    const FileExplorerViewSettings view_settings = state->view;
    *state = {};
    state->view = view_settings;
}

}  // namespace

FileManagementExplorerPanel::FileManagementExplorerPanel(QWidget* parent) : QWidget(parent) {
    m_status_center = new FileExplorerStatusCenterModel(this);
    setupUi();
    loadSidebarState();
    // Files always starts with an active sort (SortOption defaults to Name
    // ascending); without this the listing shows raw enumeration order until
    // the user first touches a header or the sort flyout.
    applySortOrder(FileExplorerItemModel::NameColumn, Qt::AscendingOrder);
    setTargets(FileManagementFileSystemBridge::mountedTargets());
}

FileManagementExplorerPanel::~FileManagementExplorerPanel() {
    // Abort a background hash (if any) so a large-file digest does not run on after the panel is
    // gone. The detached QtConcurrent task holds its own token copy and simply discards its
    // result; requesting stop lets it return promptly instead of hashing to completion.
    m_hashStopSource.request_stop();
    if (m_search_worker != nullptr) {
        // Destruction is the one place a join is required: a running child
        // QThread must not be destroyed with the panel. Interactive stops go
        // through the non-blocking stopExplorerSearch instead.
        m_search_worker->requestStop();
        constexpr int kSearchWorkerJoinMs = 5000;
        if (!m_search_worker->wait(kSearchWorkerJoinMs)) {
            // Refuses to stop within the bounded join (e.g. blocked in a slow
            // network-path enumeration that does not poll requestStop in time):
            // detach exactly like the IO-worker path below so ~QObject cannot
            // destroy a live QThread and qFatal-abort the process. Sever the
            // panel-targeted finish handlers and wire a self-owned deleteLater so
            // the orphaned thread still frees itself once it exits.
            AdvancedSearchWorker* worker = m_search_worker;
            disconnect(worker, nullptr, this, nullptr);
            worker->setParent(nullptr);
            QObject::connect(worker, &QThread::finished, worker, &QObject::deleteLater);
            // Race: it may have finished between the wait() timeout and the
            // connect above, so its finished signal already fired and will not
            // replay -- delete it directly then.
            if (worker->isFinished()) {
                worker->deleteLater();
            }
        }
        m_search_worker = nullptr;
    }
    // Same for the MUTATING copy/move + archive workers: a running child QThread must be joined (or
    // detached) before ~QObject destroys it, else the write is aborted mid-flight and Qt fatally
    // reports "QThread destroyed while thread is still running". requestStop() is cooperative (the
    // workers poll it), so this does not freeze teardown; a worker that refuses to stop within the
    // bounded wait is detached (parent cleared -> intentional bounded leak) so it self-cleans via
    // its own deleteLater instead of being destroyed alive.
    constexpr int kIoWorkerJoinMs = 5000;
    for (WorkerBase* worker : std::as_const(m_active_io_workers)) {
        if (worker == nullptr) {
            continue;
        }
        worker->requestStop();
        if (!worker->wait(kIoWorkerJoinMs)) {
            // Refuses to stop: detach so ~QObject cannot destroy a live QThread. Its finish->
            // deleteLater connection had THIS panel as receiver and is severed here, so wire a
            // self-owned deleteLater (receiver = the worker) so the detached thread still frees
            // itself once it finishes instead of leaking permanently.
            worker->setParent(nullptr);
            QObject::connect(worker, &QThread::finished, worker, &QObject::deleteLater);
            // Race: the worker may have finished between the wait() timeout and the connect above,
            // so its finished signal already fired and will not replay -- delete it directly then.
            if (worker->isFinished()) {
                worker->deleteLater();
            }
        }
    }
    m_active_io_workers.clear();
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

    // Files anatomy (MainPage.xaml rows 0-2): tab strip, then the FULL-WIDTH
    // address toolbar above the sidebar, then the sidebar/content splitter
    // (command toolbar and status content live inside the content column).
    buildTabBar(layout);

    m_omnibar = new FileExplorerOmnibar(this);
    m_sidebar_toggle_button = m_omnibar->sidebarToggleButton();
    m_back_button = m_omnibar->backButton();
    m_forward_button = m_omnibar->forwardButton();
    m_up_button = m_omnibar->upButton();
    m_refresh_button = m_omnibar->refreshButton();
    m_path_edit = m_omnibar->pathEdit();
    m_search_box = m_omnibar->searchBox();
    m_search_button = m_omnibar->searchButton();
    m_command_button = m_omnibar->commandButton();
    layout->addWidget(m_omnibar, 0);
    buildFilterHeader(layout);

    m_shell_splitter = new QSplitter(Qt::Horizontal, this);
    m_shell_splitter->setChildrenCollapsible(false);
    layout->addWidget(m_shell_splitter, 1);

    m_sidebar = new FileExplorerSidebar(m_shell_splitter);
    m_target_list = m_sidebar->targetList();
    m_scan_disks_button = m_sidebar->scanDisksButton();
    m_add_manual_button = m_sidebar->addManualButton();
    // Files sidebar footer gear (MainPage SettingsButton).
    connect(m_sidebar->settingsButton(), &QPushButton::clicked, this, [this]() {
        showExplorerSettings();
    });
    // Files SidebarViewModel drag targets: items drop onto tag rows
    // (drag-to-tag) and target rows (copy/move to that location), and
    // favorites reorder by drag within their section.
    m_target_list->setAcceptDrops(true);
    m_target_list->viewport()->setAcceptDrops(true);
    m_target_list->viewport()->installEventFilter(this);
    m_shell_splitter->addWidget(m_sidebar);

    auto* center = new QWidget(m_shell_splitter);
    auto* center_layout = new QVBoxLayout(center);
    center_layout->setContentsMargins(
        ui::kMarginNone, ui::kMarginNone, ui::kMarginNone, ui::kMarginNone);
    center_layout->setSpacing(ui::kSpacingSmall);
    m_shell_splitter->addWidget(center);

    buildCommandAndNavBars(center, center_layout);
    buildContentArea(center, center_layout);
    // Files puts the status bar INSIDE the content column (MainPage.xaml
    // InnerContent row 5), not across the sidebar.
    buildStatusRow(center_layout);

    connectUiSignals();
    installCommandShortcuts();
    updateActionButtons();
}

void FileManagementExplorerPanel::buildCommandAndNavBars(QWidget* center,
                                                         QVBoxLayout* center_layout) {
    m_command_bar = new FileExplorerCommandBar(center);
    m_rename_button = m_command_bar->renameButton();
    m_delete_button = m_command_bar->deleteButton();
    m_view_button = m_command_bar->viewButton();
    m_details_toggle_button = m_command_bar->detailsToggleButton();
    center_layout->addWidget(m_command_bar);

    // Flyout menus rebuild on open so entries always carry the live command
    // context (enabled state + blockers), matching how the context menus work.
    connect(m_command_bar->newMenu(), &QMenu::aboutToShow, this, [this]() {
        QMenu* menu = m_command_bar->newMenu();
        menu->clear();
        const FileExplorerCommandContext context = commandContext();
        addCommandMenuAction(menu, FileExplorerCommandId::NewFolder, context);
        addCommandMenuAction(menu, FileExplorerCommandId::CreateEmptyFile, context);
        addCommandMenuAction(menu, FileExplorerCommandId::WriteFile, context);
    });
    connect(m_command_bar->selectionMenu(), &QMenu::aboutToShow, this, [this]() {
        QMenu* menu = m_command_bar->selectionMenu();
        menu->clear();
        const FileExplorerCommandContext context = commandContext();
        addCommandMenuAction(menu, FileExplorerCommandId::SelectAll, context);
        addCommandMenuAction(menu, FileExplorerCommandId::InvertSelection, context);
        addCommandMenuAction(menu, FileExplorerCommandId::ClearSelection, context);
    });
    connect(m_command_bar->sortMenu(), &QMenu::aboutToShow, this, [this]() {
        rebuildSortMenu(m_command_bar->sortMenu());
    });
    connect(m_command_bar->cutButton(), &QPushButton::clicked, this, [this]() {
        executeCommand(FileExplorerCommandId::CutItems);
    });
    connect(m_command_bar->copyButton(), &QPushButton::clicked, this, [this]() {
        executeCommand(FileExplorerCommandId::CopyItems);
    });
    connect(m_command_bar->pasteButton(), &QPushButton::clicked, this, [this]() {
        executeCommand(FileExplorerCommandId::Paste);
    });
    connect(m_command_bar->propertiesButton(), &QPushButton::clicked, this, [this]() {
        executeCommand(FileExplorerCommandId::Properties);
    });
}

void FileManagementExplorerPanel::rebuildSortMenu(QMenu* menu, const bool include_grouping) {
    if (menu == nullptr) {
        return;
    }
    menu->clear();
    // Files sort flyout (Toolbar.xaml ArrangementOptions): checkable sort-by
    // entries, then Ascending/Descending. Sorting runs through the shared
    // proxy model, so it applies to every layout mode.
    auto* proxy = (m_pane != nullptr) ? m_pane->sortFilterModel() : nullptr;
    if (proxy == nullptr) {
        return;
    }
    const int current_column = proxy->sortColumn() < 0 ? FileExplorerItemModel::NameColumn
                                                       : proxy->sortColumn();
    const Qt::SortOrder current_order = proxy->sortOrder();
    static constexpr std::array kSortColumns = {
        std::pair{FileExplorerItemModel::NameColumn, QT_TR_NOOP("Name")},
        std::pair{FileExplorerItemModel::ModifiedColumn, QT_TR_NOOP("Date modified")},
        std::pair{FileExplorerItemModel::CreatedColumn, QT_TR_NOOP("Date created")},
        std::pair{FileExplorerItemModel::SizeColumn, QT_TR_NOOP("Size")},
        std::pair{FileExplorerItemModel::TypeColumn, QT_TR_NOOP("Type")},
        std::pair{FileExplorerItemModel::TagsColumn, QT_TR_NOOP("Tags")},
        std::pair{FileExplorerItemModel::PathColumn, QT_TR_NOOP("Path")},
    };
    for (const auto& [column, label] : kSortColumns) {
        QAction* action = menu->addAction(tr(label));
        action->setCheckable(true);
        action->setChecked(column == current_column);
        const int sort_column = column;
        connect(action, &QAction::triggered, this, [this, sort_column]() {
            applySortOrder(sort_column, m_pane->sortFilterModel()->sortOrder());
        });
    }
    menu->addSeparator();
    QAction* ascending = menu->addAction(tr("Ascending"));
    ascending->setCheckable(true);
    ascending->setChecked(current_order == Qt::AscendingOrder);
    connect(ascending, &QAction::triggered, this, [this, current_column]() {
        applySortOrder(current_column, Qt::AscendingOrder);
    });
    QAction* descending = menu->addAction(tr("Descending"));
    descending->setCheckable(true);
    descending->setChecked(current_order == Qt::DescendingOrder);
    connect(descending, &QAction::triggered, this, [this, current_column]() {
        applySortOrder(current_column, Qt::DescendingOrder);
    });
    menu->addSeparator();
    addSortPlacementActions(menu);
    // The Files toolbar exposes grouping as its own flyout beside sorting;
    // the S.A.K. command bar reuses the sort flyout for both, while the
    // background context menu adds Group by as a sibling submenu instead.
    if (include_grouping) {
        menu->addSeparator();
        auto* group_menu = menu->addMenu(tr("Group by"));
        group_menu->setObjectName(QStringLiteral("fileExplorerGroupBySubmenu"));
        rebuildGroupMenu(group_menu);
    }
}

void FileManagementExplorerPanel::rebuildGroupMenu(QMenu* menu) {
    if (menu == nullptr) {
        return;
    }
    menu->clear();
    // Files GroupAction set: the folder-general group options, direction, and
    // the date unit for date groupings (GroupBy*Action, GroupAscendingAction/
    // GroupDescendingAction, GroupByYear/Month + ToggleGroupByDateUnitAction).
    const FileExplorerGroupOption current = m_pane_state.view.group_option;
    static constexpr std::array kGroupOptions = {
        std::pair{FileExplorerGroupOption::None, QT_TR_NOOP("None")},
        std::pair{FileExplorerGroupOption::Name, QT_TR_NOOP("Name")},
        std::pair{FileExplorerGroupOption::DateModified, QT_TR_NOOP("Date modified")},
        std::pair{FileExplorerGroupOption::DateCreated, QT_TR_NOOP("Date created")},
        std::pair{FileExplorerGroupOption::Size, QT_TR_NOOP("Size")},
        std::pair{FileExplorerGroupOption::FileType, QT_TR_NOOP("Type")},
        std::pair{FileExplorerGroupOption::FileTag, QT_TR_NOOP("Tag")},
    };
    auto* option_group = new QActionGroup(menu);
    option_group->setExclusive(true);
    for (const auto& [option, label] : kGroupOptions) {
        QAction* action = menu->addAction(tr(label));
        action->setCheckable(true);
        action->setChecked(option == current);
        option_group->addAction(action);
        const FileExplorerGroupOption value = option;
        connect(action, &QAction::triggered, this, [this, value]() {
            applyGrouping(value, m_pane_state.view.group_date_unit, m_pane_state.view.group_order);
        });
    }
    menu->addSeparator();
    addGroupDirectionActions(menu);
    menu->addSeparator();
    addGroupDateUnitActions(menu);
}

void FileManagementExplorerPanel::addGroupDirectionActions(QMenu* menu) {
    // Files GroupAscendingAction/GroupDescendingAction: enabled only while a
    // group option is active.
    const bool grouped = m_pane_state.view.group_option != FileExplorerGroupOption::None;
    QAction* ascending = menu->addAction(tr("Ascending"));
    ascending->setCheckable(true);
    ascending->setChecked(m_pane_state.view.group_order == Qt::AscendingOrder);
    ascending->setEnabled(grouped);
    connect(ascending, &QAction::triggered, this, [this]() {
        applyGrouping(m_pane_state.view.group_option,
                      m_pane_state.view.group_date_unit,
                      Qt::AscendingOrder);
    });
    QAction* descending = menu->addAction(tr("Descending"));
    descending->setCheckable(true);
    descending->setChecked(m_pane_state.view.group_order == Qt::DescendingOrder);
    descending->setEnabled(grouped);
    connect(descending, &QAction::triggered, this, [this]() {
        applyGrouping(m_pane_state.view.group_option,
                      m_pane_state.view.group_date_unit,
                      Qt::DescendingOrder);
    });
}

void FileManagementExplorerPanel::addGroupDateUnitActions(QMenu* menu) {
    // Files GroupByYear/Month/Day actions: only date groupings use the unit.
    const bool date_grouping =
        m_pane_state.view.group_option == FileExplorerGroupOption::DateModified ||
        m_pane_state.view.group_option == FileExplorerGroupOption::DateCreated;
    static constexpr std::array kDateUnits = {
        std::pair{FileExplorerGroupDateUnit::Year, QT_TR_NOOP("Year")},
        std::pair{FileExplorerGroupDateUnit::Month, QT_TR_NOOP("Month")},
        std::pair{FileExplorerGroupDateUnit::Day, QT_TR_NOOP("Day")},
    };
    auto* unit_group = new QActionGroup(menu);
    unit_group->setExclusive(true);
    for (const auto& [unit, label] : kDateUnits) {
        QAction* action = menu->addAction(tr(label));
        action->setCheckable(true);
        action->setChecked(unit == m_pane_state.view.group_date_unit);
        action->setEnabled(date_grouping);
        unit_group->addAction(action);
        const FileExplorerGroupDateUnit value = unit;
        connect(action, &QAction::triggered, this, [this, value]() {
            applyGrouping(m_pane_state.view.group_option, value, m_pane_state.view.group_order);
        });
    }
}

void FileManagementExplorerPanel::applyGrouping(const FileExplorerGroupOption option,
                                                const FileExplorerGroupDateUnit date_unit,
                                                const Qt::SortOrder order) {
    m_pane_state.view.group_option = option;
    m_pane_state.view.group_date_unit = date_unit;
    m_pane_state.view.group_order = order;
    applyViewSettings();
    saveViewSettings();
    Q_EMIT statusMessage(option == FileExplorerGroupOption::None
                             ? tr("Grouping cleared")
                             : tr("Grouped by %1").arg(fileExplorerGroupOptionName(option)),
                         sak::kTimerStatusMessageMs);
}

void FileManagementExplorerPanel::applySortOrder(const int column, const Qt::SortOrder order) {
    if ((m_pane == nullptr) || (m_pane->sortFilterModel() == nullptr)) {
        return;
    }
    m_pane->sortFilterModel()->sort(column, order);
    if (auto* table = m_pane->tableView()) {
        table->horizontalHeader()->setSortIndicator(column, order);
    }
}

// Files SortFoldersFirst / SortFilesFirst / SortFilesAndFoldersTogether radio
// group at the bottom of the sort flyout.
void FileManagementExplorerPanel::addSortPlacementActions(QMenu* menu) {
    static constexpr std::array kPlacements = {
        std::pair{FileExplorerFolderSortPlacement::FoldersFirst, QT_TR_NOOP("Sort folders first")},
        std::pair{FileExplorerFolderSortPlacement::FilesFirst, QT_TR_NOOP("Sort files first")},
        std::pair{FileExplorerFolderSortPlacement::Together,
                  QT_TR_NOOP("Sort files and folders together")},
    };
    for (const auto& [placement, label] : kPlacements) {
        QAction* action = menu->addAction(tr(label));
        action->setCheckable(true);
        action->setChecked(m_pane_state.view.folder_placement == placement);
        const FileExplorerFolderSortPlacement value = placement;
        connect(action, &QAction::triggered, this, [this, value]() {
            applyFolderSortPlacement(value);
        });
    }
}

void FileManagementExplorerPanel::applyFolderSortPlacement(
    const FileExplorerFolderSortPlacement placement) {
    m_pane_state.view.folder_placement = placement;
    applyViewSettings();
    // Placement only shows through an active sort; with no sort column yet
    // the proxy would never re-run lessThan (Files always sorts by name).
    if ((m_pane != nullptr) && (m_pane->sortFilterModel() != nullptr)) {
        const int column = m_pane->sortFilterModel()->sortColumn() < 0
                               ? FileExplorerItemModel::NameColumn
                               : m_pane->sortFilterModel()->sortColumn();
        applySortOrder(column, m_pane->sortFilterModel()->sortOrder());
    }
    saveViewSettings();
}

void FileManagementExplorerPanel::buildStatusRow(QVBoxLayout* root_layout) {
    auto* row = new QWidget(this);
    row->setObjectName(QStringLiteral("fileExplorerStatusRow"));
    auto* row_layout = new QHBoxLayout(row);
    row_layout->setContentsMargins(
        ui::kMarginNone, ui::kMarginNone, ui::kMarginNone, ui::kMarginNone);
    row_layout->setSpacing(ui::kSpacingSmall);

    // Files StatusBar.xaml col0: item count | selected count | selection size.
    m_items_count_label = new QLabel(row);
    m_items_count_label->setObjectName(QStringLiteral("fileExplorerItemsCountLabel"));
    m_items_count_label->setAccessibleName(tr("Folder item count"));
    row_layout->addWidget(m_items_count_label, 0);

    m_selection_count_label = new QLabel(row);
    m_selection_count_label->setObjectName(QStringLiteral("fileExplorerSelectionCountLabel"));
    m_selection_count_label->setAccessibleName(tr("Selected item count and size"));
    m_selection_count_label->setVisible(false);
    row_layout->addWidget(m_selection_count_label, 0);

    m_summary_label = new QLabel(tr("No target selected"), row);
    m_summary_label->setObjectName(QStringLiteral("fileExplorerSummaryLabel"));
    m_summary_label->setWordWrap(false);
    m_summary_label->setAccessibleName(tr("Explorer target summary"));
    row_layout->addStretch(1);
    row_layout->addWidget(m_summary_label, 0);

    root_layout->addWidget(row, 0);
}

namespace {

// Sums the byte sizes of the selected FILE rows (directories excluded),
// mirroring the Files status-bar ItemSize readout.
uint64_t selectedFileBytes(const FileExplorerPane* pane, const QModelIndexList& rows) {
    uint64_t bytes = 0;
    for (const QModelIndex& row : rows) {
        const FileManagementEntry entry = pane->entryAtViewRow(row.row());
        if (!entry.directory) {
            bytes += entry.size_bytes;
        }
    }
    return bytes;
}

}  // namespace

void FileManagementExplorerPanel::updateStatusCounts() {
    if ((m_items_count_label == nullptr) || (m_selection_count_label == nullptr)) {
        return;
    }
    const int item_count = ((m_pane != nullptr) && (m_pane->sortFilterModel() != nullptr))
                               ? m_pane->sortFilterModel()->rowCount()
                               : 0;
    m_items_count_label->setText(tr("%n item(s)", nullptr, item_count));

    QModelIndexList rows = ((m_pane != nullptr) && (m_pane->sharedSelectionModel() != nullptr))
                               ? m_pane->sharedSelectionModel()->selectedRows()
                               : QModelIndexList{};
    rows.removeIf([this](const QModelIndex& index) { return !m_pane->hasViewEntry(index.row()); });
    if (!rows.isEmpty()) {
        QString text = tr("%n item(s) selected", nullptr, static_cast<int>(rows.size()));
        const uint64_t selected_bytes = selectedFileBytes(m_pane, rows);
        if (selected_bytes > 0) {
            text += QStringLiteral("   %1").arg(FileExplorerItemModel::sizeText(selected_bytes));
        }
        m_selection_count_label->setText(text);
    }
    m_selection_count_label->setVisible(!rows.isEmpty());
}

void FileManagementExplorerPanel::buildTabBar(QVBoxLayout* center_layout) {
    auto* row = new QWidget(this);
    row->setObjectName(QStringLiteral("fileExplorerTabRow"));
    auto* row_layout = new QHBoxLayout(row);
    row_layout->setContentsMargins(
        ui::kMarginNone, ui::kMarginNone, ui::kMarginNone, ui::kMarginNone);
    row_layout->setSpacing(ui::kSpacingTight);

    // Files TabBar.xaml TabStripHeader: a 30x30 tab-actions button LEFT of
    // the tabs (pane split/arrange/close menu), rebuilt on open.
    auto* tab_actions = new QToolButton(row);
    tab_actions->setObjectName(QStringLiteral("fileExplorerTabActionsButton"));
    tab_actions->setIcon(FileExplorerIconRegistry::iconForKey(QStringLiteral("dual-pane")));
    tab_actions->setPopupMode(QToolButton::InstantPopup);
    tab_actions->setAccessibleName(tr("Tab actions menu"));
    tab_actions->setToolTip(tr("Split, arrange, or close panes"));
    tab_actions->setFixedSize(ui::kUiButtonHeightMini, ui::kUiButtonHeightMini);
    auto* tab_actions_menu = new QMenu(tab_actions);
    tab_actions_menu->setObjectName(QStringLiteral("fileExplorerTabActionsMenu"));
    tab_actions->setMenu(tab_actions_menu);
    connect(tab_actions_menu, &QMenu::aboutToShow, this, [this, tab_actions_menu]() {
        rebuildTabActionsMenu(tab_actions_menu);
    });
    row_layout->addWidget(tab_actions, 0);

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
    // Files TabBar.xaml TabFlyout: right-click menu on a tab.
    m_tab_bar->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_tab_bar, &QWidget::customContextMenuRequested, this, [this](const QPoint& point) {
        showTabContextMenu(point);
    });
}

void FileManagementExplorerPanel::rebuildTabActionsMenu(QMenu* menu) {
    if (menu == nullptr) {
        return;
    }
    menu->clear();
    // Files tab-actions flyout: split pane V/H, arrange panes V/H, close pane.
    menu->addAction(tr("Split pane vertically"), this, [this]() { splitPane(Qt::Horizontal); });
    menu->addAction(tr("Split pane horizontally"), this, [this]() { splitPane(Qt::Vertical); });
    menu->addSeparator();
    const bool splitter_horizontal = (m_pane_splitter != nullptr) &&
                                     m_pane_splitter->orientation() == Qt::Horizontal;
    QAction* arrange_vertical = menu->addAction(tr("Arrange panes vertically"));
    arrange_vertical->setCheckable(true);
    arrange_vertical->setChecked(m_dual_pane_enabled && splitter_horizontal);
    arrange_vertical->setEnabled(m_dual_pane_enabled);
    connect(arrange_vertical, &QAction::triggered, this, [this]() {
        if (m_pane_splitter) {
            m_pane_splitter->setOrientation(Qt::Horizontal);
        }
    });
    QAction* arrange_horizontal = menu->addAction(tr("Arrange panes horizontally"));
    arrange_horizontal->setCheckable(true);
    arrange_horizontal->setChecked(m_dual_pane_enabled && !splitter_horizontal);
    arrange_horizontal->setEnabled(m_dual_pane_enabled);
    connect(arrange_horizontal, &QAction::triggered, this, [this]() {
        if (m_pane_splitter) {
            m_pane_splitter->setOrientation(Qt::Vertical);
        }
    });
    menu->addSeparator();
    QAction* close_pane = menu->addAction(tr("Close pane"));
    close_pane->setEnabled(m_dual_pane_enabled);
    connect(close_pane, &QAction::triggered, this, [this]() {
        if (m_dual_pane_enabled) {
            toggleDualPane();
        }
    });
}

void FileManagementExplorerPanel::splitPane(const Qt::Orientation orientation) {
    if (!m_dual_pane_enabled) {
        toggleDualPane();
    }
    if (m_pane_splitter != nullptr) {
        m_pane_splitter->setOrientation(orientation);
    }
}

void FileManagementExplorerPanel::showTabContextMenu(const QPoint& point) {
    if (m_tab_bar == nullptr) {
        return;
    }
    const int index = m_tab_bar->tabAt(point);
    QMenu menu(this);
    menu.setObjectName(QStringLiteral("fileExplorerTabContextMenu"));
    // Files TabFlyout order (TabBar.xaml 21-58); "Move tab to new window" is
    // EXCLUDED (the explorer is a tab inside the S.A.K. shell).
    menu.addAction(tr("New tab"), this, &FileManagementExplorerPanel::openCurrentLocationInNewTab);
    QAction* duplicate = menu.addAction(tr("Duplicate tab"), this, [this]() {
        executeCommand(FileExplorerCommandId::DuplicateTab);
    });
    duplicate->setEnabled(index >= 0);
    menu.addSeparator();
    QAction* close_left = menu.addAction(tr("Close tabs to the left"), this, [this, index]() {
        closeTabsRelative(index, -1);
    });
    close_left->setEnabled(index > 0);
    QAction* close_right = menu.addAction(tr("Close tabs to the right"), this, [this, index]() {
        closeTabsRelative(index, 1);
    });
    close_right->setEnabled(index >= 0 && index < m_tab_bar->count() - 1);
    QAction* close_others = menu.addAction(tr("Close other tabs"), this, [this, index]() {
        closeTabsRelative(index, 0);
    });
    close_others->setEnabled(index >= 0 && m_tab_bar->count() > 1);
    menu.addSeparator();
    QAction* reopen = menu.addAction(tr("Reopen tab"), this, [this]() {
        executeCommand(FileExplorerCommandId::ReopenClosedTab);
    });
    reopen->setEnabled(!m_closed_tabs.isEmpty());
    menu.exec(m_tab_bar->mapToGlobal(point));
}

void FileManagementExplorerPanel::closeTabsRelative(const int index, const int direction) {
    if ((m_tab_bar == nullptr) || index < 0) {
        return;
    }
    // Close from the highest index down so remaining indices stay valid.
    for (int i = m_tab_bar->count() - 1; i >= 0; --i) {
        const bool close = (direction < 0 && i < index) || (direction > 0 && i > index) ||
                           (direction == 0 && i != index);
        if (close) {
            onTabCloseRequested(i);
        }
    }
}

void FileManagementExplorerPanel::nameTabCloseButtons() {
    if (m_tab_bar == nullptr) {
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
    installIconProvider(m_item_model);
    installIconProvider(m_pane->columnsPreviewModel());
    m_status_label = m_pane->statusLabel();
    center_layout->addWidget(m_pane_splitter, 1);

    m_details_pane = new FileExplorerDetailsPane(m_shell_splitter);
    m_preview_text = m_details_pane->previewText();
    m_properties_text = m_details_pane->propertiesText();
    m_safety_text = m_details_pane->safetyText();
    m_evidence_text = m_details_pane->evidenceText();
    m_shell_splitter->addWidget(m_details_pane);
    m_shell_splitter->setStretchFactor(0, 0);
    m_shell_splitter->setStretchFactor(1, 1);
    m_shell_splitter->setStretchFactor(kCenterPaneStretchIndex, 0);
}

void FileManagementExplorerPanel::connectUiSignals() {
    m_rename_tap_timer = new QTimer(this);
    m_rename_tap_timer->setSingleShot(true);
    m_rename_tap_timer->setInterval(kRenameTapDebounceMs);
    connect(m_rename_tap_timer,
            &QTimer::timeout,
            this,
            &FileManagementExplorerPanel::onRenameTapTimeout);
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
    // Files ToggleSidebarAction switches Expanded <-> Compact (the 56px
    // icon rail); the narrow-width responsive collapse still hides it.
    connect(m_sidebar_toggle_button, &QPushButton::clicked, this, [this]() {
        if (m_sidebar) {
            setSidebarCompact(!m_sidebar->isCompact());
        }
    });
    connect(m_details_toggle_button,
            &QPushButton::clicked,
            this,
            &FileManagementExplorerPanel::togglePreviewPane);
    // Files SearchAction surfaces: the button enters the omnibar search mode;
    // Enter in the persistent quick-search box submits straight into the
    // listing (Files SubmitSearch).
    connect(m_search_button, &QPushButton::clicked, this, [this]() {
        if (m_omnibar) {
            m_omnibar->setMode(FileExplorerOmnibarMode::Search);
        }
    });
    connect(m_search_box, &QLineEdit::returnPressed, this, [this]() {
        submitExplorerSearch(m_search_box->text().trimmed());
    });
    connect(m_command_button,
            &QPushButton::clicked,
            this,
            &FileManagementExplorerPanel::showCommandPalette);
    // Files ShowStatusCenterButton: opens the status-center flyout; the badge
    // mirrors the view-model aggregates on every model change.
    connect(m_omnibar->statusCenterButton(),
            &QPushButton::clicked,
            this,
            &FileManagementExplorerPanel::toggleStatusCenterFlyout);
    connect(m_status_center,
            &FileExplorerStatusCenterModel::changed,
            this,
            &FileManagementExplorerPanel::syncStatusCenterButton);
    connectOmnibarModeSignals();
}

void FileManagementExplorerPanel::toggleStatusCenterFlyout() {
    if (m_status_flyout == nullptr) {
        m_status_flyout = new FileExplorerStatusCenterFlyout(m_status_center, window());
    }
    if (m_status_flyout->isVisible()) {
        m_status_flyout->hide();
        return;
    }
    // Files Placement=BottomEdgeAlignedRight under the toolbar button.
    const QPushButton* button = m_omnibar->statusCenterButton();
    const QPoint anchor = button->mapTo(window(), button->rect().bottomRight());
    constexpr int kStatusFlyoutGapPx = 2;
    m_status_flyout->openAt(QPoint(anchor.x(), anchor.y() + kStatusFlyoutGapPx));
}

void FileManagementExplorerPanel::syncStatusCenterButton() {
    FileExplorerStatusCenterButton* button = m_omnibar->statusCenterButton();
    button->setBadge(m_status_center->infoBadgeState(),
                     m_status_center->infoBadgeValue(),
                     m_status_center->averageProgress(),
                     m_status_center->showProgressRing());
    // Files ShowStatusCenterButton: Always, or only while operations exist.
    button->setVisible(!statusCenterDuringOperationsOnly() || m_status_center->hasAnyItem());
}

// Inline omnibar modes (Files Omnibar): palette suggestions repopulate on
// every edit and Enter/click executes through the registry; search mode
// shows recents / debounced live matches and submits into the listing.
void FileManagementExplorerPanel::connectOmnibarModeSignals() {
    connect(m_omnibar, &FileExplorerOmnibar::queryTextEdited, this, [this](const QString& text) {
        if (m_omnibar->mode() == FileExplorerOmnibarMode::Palette) {
            populateOmnibarPalette(text.trimmed());
        } else if (m_omnibar->mode() == FileExplorerOmnibarMode::Search) {
            populateOmnibarSearch(text);
        }
    });
    connect(m_omnibar, &FileExplorerOmnibar::querySubmitted, this, [this](const QString& text) {
        if (m_omnibar->mode() == FileExplorerOmnibarMode::Palette) {
            executePaletteSuggestion(m_omnibar->suggestionList()->currentItem(), text);
        } else if (m_omnibar->mode() == FileExplorerOmnibarMode::Search) {
            submitSearchSuggestion(m_omnibar->suggestionList()->currentItem(), text);
        }
    });
    connect(
        m_omnibar, &FileExplorerOmnibar::suggestionActivated, this, [this](QListWidgetItem* item) {
            if (m_omnibar->mode() == FileExplorerOmnibarMode::Palette) {
                executePaletteSuggestion(item, item ? item->text() : QString());
            } else if (m_omnibar->mode() == FileExplorerOmnibarMode::Search) {
                submitSearchSuggestion(item, item ? item->text() : QString());
            }
        });
    // Files search-suggestion debounce (200 ms) ahead of the live worker run.
    m_search_suggest_timer = new QTimer(this);
    m_search_suggest_timer->setSingleShot(true);
    m_search_suggest_timer->setInterval(kSearchSuggestDebounceMs);
    connect(m_search_suggest_timer, &QTimer::timeout, this, [this]() {
        runSearchSuggestions(m_pending_search_suggest);
    });
    // Leaving search mode cancels the pending debounce and the live worker.
    connect(m_omnibar,
            &FileExplorerOmnibar::modeChanged,
            this,
            [this](const FileExplorerOmnibarMode mode) {
                if (mode == FileExplorerOmnibarMode::Path) {
                    m_search_suggest_timer->stop();
                    stopExplorerSearch();
                }
            });
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
    connect(m_back_button, &QWidget::customContextMenuRequested, this, [this](const QPoint& point) {
        showHistoryMenu(true, m_back_button->mapToGlobal(point));
    });
    connect(
        m_forward_button, &QWidget::customContextMenuRequested, this, [this](const QPoint& point) {
            showHistoryMenu(false, m_forward_button->mapToGlobal(point));
        });
    connect(m_forward_button,
            &QPushButton::clicked,
            this,
            &FileManagementExplorerPanel::onForwardClicked);
    connect(m_up_button, &QPushButton::clicked, this, &FileManagementExplorerPanel::onUpClicked);
    connect(m_rename_button,
            &QPushButton::clicked,
            this,
            &FileManagementExplorerPanel::onRenameClicked);
    connect(m_delete_button,
            &QPushButton::clicked,
            this,
            &FileManagementExplorerPanel::onDeleteClicked);
}

void FileManagementExplorerPanel::installSelectionCheckboxes(FileExplorerPane* pane) {
    // Files ShowCheckboxesWhenSelectingItems: the checkbox mirrors the
    // selection, so the model reads a per-pane snapshot of selected paths and
    // checkbox clicks route back into the selection model.
    auto checked_paths = std::make_shared<QSet<QString>>();
    pane->itemModel()->setCheckboxProviders(
        [checked_paths](const QString& path) { return checked_paths->contains(path); },
        [this, pane](const QString& path, const bool checked) {
            toggleSelectionForPath(pane, path, checked);
        });
    pane->itemModel()->setCheckboxesVisible(showCheckboxesEnabled());
    if (pane->sharedSelectionModel() == nullptr) {
        return;
    }
    connect(pane->sharedSelectionModel(),
            &QItemSelectionModel::selectionChanged,
            this,
            [pane, checked_paths]() {
                checked_paths->clear();
                const QModelIndexList rows = pane->sharedSelectionModel()
                                                 ? pane->sharedSelectionModel()->selectedRows()
                                                 : QModelIndexList{};
                for (const QModelIndex& row : rows) {
                    if (pane->hasViewEntry(row.row())) {
                        checked_paths->insert(pane->entryAtViewRow(row.row()).path);
                    }
                }
                pane->itemModel()->refreshChecks();
            });
}

void FileManagementExplorerPanel::connectPaneViewSignals(FileExplorerPane* pane, int pane_index) {
    for (auto* view : pane->itemViews()) {
        if (view == nullptr) {
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
        // Mouse-level interactions (rename tap, mouse4/5, middle-click,
        // empty double-click, Ctrl+wheel), drag/drop, and the Enter
        // activation matrix.
        view->viewport()->installEventFilter(this);
        view->installEventFilter(this);
    }
}

void FileManagementExplorerPanel::connectPaneSignals(FileExplorerPane* pane, int pane_index) {
    installSelectionCheckboxes(pane);
    if (pane->sharedSelectionModel() != nullptr) {
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
    connectPaneViewSignals(pane, pane_index);
    // Drag payloads reuse the clipboard batch builder; the transfer direction
    // is decided at drop time by the modifier cascade.
    pane->itemModel()->setDragPayloadProvider(
        [this, pane_index](const QList<int>& rows) { return buildDragMimeData(pane_index, rows); });
    // Inline rename commits arrive from the model. The victim's path is bound
    // HERE, while the model still holds the row the user edited; the mutation
    // itself is deferred so the editor is fully closed before the bridge write
    // and the listing reload run. A listing that lands in that window would
    // otherwise leave a different entry at the same row.
    connect(pane->itemModel(),
            &FileExplorerItemModel::renameRequested,
            this,
            [this, pane_index, model = pane->itemModel()](const int row, const QString& new_name) {
                const QString source_path = model->hasEntry(row) ? model->entryAt(row).path
                                                                 : QString();
                QTimer::singleShot(0, this, [this, pane_index, row, new_name, source_path]() {
                    activatePane(pane_index);
                    performInlineRename(row, new_name, source_path);
                });
            });
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

bool FileManagementExplorerPanel::eventFilter(QObject* watched, QEvent* event) {
    if (dispatchFilteredEvent(watched, event)) {
        return true;
    }
    return QWidget::eventFilter(watched, event);
}

// Routes a filtered event to the surface that owns the watched widget: the
// filter box, the sidebar viewport, an item view (key presses), or a pane
// viewport. Returns true when the event was consumed.
bool FileManagementExplorerPanel::dispatchFilteredEvent(QObject* watched, QEvent* event) {
    if (event == nullptr) {
        return false;
    }
    if ((m_filter_box != nullptr) && watched == m_filter_box) {
        return handleFilterBoxKeyEvent(event);
    }
    if ((m_target_list != nullptr) && watched == m_target_list->viewport()) {
        return handleSidebarViewportEvent(event);
    }
    if (auto* view = qobject_cast<QAbstractItemView*>(watched)) {
        return event->type() == QEvent::KeyPress &&
               handleViewKeyPress(view, static_cast<QKeyEvent*>(event));
    }
    return filterPaneViewportEvent(watched, event);
}

// Files FilterTextBox_PreviewKeyDown: Esc in the filter box returns focus to
// the file list (the ShortcutOverride claim keeps the ambient ClearSelection
// Esc shortcut from eating the key first).
bool FileManagementExplorerPanel::handleFilterBoxKeyEvent(QEvent* event) {
    if (event->type() == QEvent::ShortcutOverride &&
        static_cast<QKeyEvent*>(event)->key() == Qt::Key_Escape) {
        event->accept();
        return true;
    }
    if (event->type() == QEvent::KeyPress &&
        static_cast<QKeyEvent*>(event)->key() == Qt::Key_Escape) {
        if (auto* view = currentItemView()) {
            view->setFocus(Qt::ShortcutFocusReason);
        }
        return true;
    }
    return false;
}

bool FileManagementExplorerPanel::filterPaneViewportEvent(QObject* watched, QEvent* event) {
    auto* view = qobject_cast<QAbstractItemView*>((watched != nullptr) ? watched->parent()
                                                                       : nullptr);
    if ((view == nullptr) || (event == nullptr)) {
        return false;
    }
    if (handleViewportDragEvent(view, event)) {
        return true;
    }
    if (handleViewportMouseEvent(view, event)) {
        return true;
    }
    handleRenameTapEvent(view, event);
    return false;
}

void FileManagementExplorerPanel::activatePaneForView(QAbstractItemView* view) {
    const bool in_second_pane = (m_pane_b != nullptr) && m_pane_b->itemViews().contains(view);
    activatePane(in_second_pane ? 1 : 0);
}

bool FileManagementExplorerPanel::handleViewKeyPress(QAbstractItemView* view, QKeyEvent* key) {
    if (key->key() != Qt::Key_Return && key->key() != Qt::Key_Enter) {
        return false;
    }
    // Files FileList_PreviewKeyDown activation matrix: Enter opens,
    // Ctrl+Enter opens selected folders in new tabs, Ctrl+Shift+Enter opens
    // in the other pane, Alt+Enter shows properties.
    const Qt::KeyboardModifiers mods = key->modifiers() &
                                       (Qt::ControlModifier | Qt::ShiftModifier | Qt::AltModifier);
    activatePaneForView(view);
    if (mods == Qt::NoModifier) {
        executeCommand(FileExplorerCommandId::Open);
        return true;
    }
    if (mods == Qt::ControlModifier) {
        openSelectedFoldersInNewTabs();
        return true;
    }
    if (mods == (Qt::ControlModifier | Qt::ShiftModifier)) {
        executeCommand(FileExplorerCommandId::OpenInSecondPane);
        return true;
    }
    if (mods == Qt::AltModifier) {
        executeCommand(FileExplorerCommandId::Properties);
        return true;
    }
    return false;
}

bool FileManagementExplorerPanel::handleViewportMouseEvent(QAbstractItemView* view, QEvent* event) {
    if (event->type() == QEvent::MouseButtonPress) {
        return handleViewportMousePress(view, static_cast<QMouseEvent*>(event));
    }
    if (event->type() == QEvent::MouseButtonDblClick) {
        // Files DoubleClickToGoUp (FoldersSettingsService, default true):
        // double-click on empty space navigates to the parent folder.
        const auto* mouse = static_cast<QMouseEvent*>(event);
        if (mouse->button() == Qt::LeftButton && !view->indexAt(mouse->pos()).isValid() &&
            doubleClickToGoUpEnabled()) {
            activatePaneForView(view);
            executeCommand(FileExplorerCommandId::Up);
            return true;
        }
        return false;
    }
    if (event->type() == QEvent::Wheel) {
        // Files BaseLayoutViewModel: Ctrl+mouse-wheel steps the layout size.
        const auto* wheel = static_cast<QWheelEvent*>(event);
        if (wheel->modifiers().testFlag(Qt::ControlModifier)) {
            stepLayoutSize(wheel->angleDelta().y() >= 0 ? 1 : -1);
            return true;
        }
        return false;
    }
    return false;
}

bool FileManagementExplorerPanel::handleViewportMousePress(QAbstractItemView* view,
                                                           const QMouseEvent* mouse) {
    // Files ShellPanesPage Pane_PointerPressed: a press anywhere inside a pane
    // focuses -- and so activates -- that pane. Clicking a ROW was already covered:
    // connectPaneSignals promotes a pane whose selection becomes non-empty. This
    // covers the press that produces no selection change -- empty space below the
    // rows, or a click that re-selects what was already selected -- after which the
    // pane the user is visibly working in was still not the one commands routed to.
    activatePaneForView(view);
    // Files BaseShellPage CoreWindow_PointerPressed: mouse4/5 navigate
    // back/forward (NavigateBack Mouse4 / NavigateForward Mouse5 hotkeys).
    if (mouse->button() == Qt::BackButton) {
        activatePaneForView(view);
        executeCommand(FileExplorerCommandId::Back);
        return true;
    }
    if (mouse->button() == Qt::ForwardButton) {
        activatePaneForView(view);
        executeCommand(FileExplorerCommandId::Forward);
        return true;
    }
    // Files BaseLayoutViewModel ItemPointerPressed: middle-click on a folder
    // always opens it in a new tab.
    if (mouse->button() == Qt::MiddleButton) {
        const QModelIndex index = view->indexAt(mouse->pos());
        if (index.isValid() && index.data(FileExplorerItemModel::EntryDirectoryRole).toBool()) {
            activatePaneForView(view);
            openPathInNewTab(index.data(FileExplorerItemModel::EntryPathRole).toString());
            return true;
        }
    }
    return false;
}

bool FileManagementExplorerPanel::doubleClickToGoUpEnabled() const {
    QSettings settings;
    settings.beginGroup(QString::fromLatin1(kExplorerSettingsGroup));
    return settings.value(QString::fromLatin1(kDoubleClickToGoUpKey), true).toBool();
}

// Files Settings page (OpenSettingsAction, Ctrl+, and the sidebar gear):
// the explorer-global behavior toggles, applied live on OK. Files opens
// Settings as a tab; the S.A.K. explorer hosts location tabs only, so this
// surface is a dialog.
void FileManagementExplorerPanel::showExplorerSettings() {
    QDialog dialog(this);
    dialog.setObjectName(QStringLiteral("fileExplorerSettingsDialog"));
    dialog.setWindowTitle(tr("Explorer Settings"));
    dialog.setMinimumWidth(sak::kDialogWidthLarge);
    auto* layout = new QVBoxLayout(&dialog);

    auto* folders_label = new QLabel(tr("Files and folders"), &dialog);
    QFont section_font = folders_label->font();
    section_font.setBold(true);
    folders_label->setFont(section_font);
    layout->addWidget(folders_label);
    auto* checkboxes = new QCheckBox(tr("Show checkboxes when selecting items"), &dialog);
    checkboxes->setObjectName(QStringLiteral("fileExplorerSettingsCheckboxes"));
    checkboxes->setChecked(showCheckboxesEnabled());
    layout->addWidget(checkboxes);
    auto* double_click_up = new QCheckBox(tr("Double-click a blank space to go up a folder"),
                                          &dialog);
    double_click_up->setObjectName(QStringLiteral("fileExplorerSettingsDoubleClickUp"));
    double_click_up->setChecked(doubleClickToGoUpEnabled());
    layout->addWidget(double_click_up);

    auto* general_label = new QLabel(tr("General"), &dialog);
    general_label->setFont(section_font);
    layout->addWidget(general_label);
    auto* filter_header = new QCheckBox(tr("Show the filter header"), &dialog);
    filter_header->setObjectName(QStringLiteral("fileExplorerSettingsFilterHeader"));
    filter_header->setChecked(showFilterHeaderEnabled());
    layout->addWidget(filter_header);
    auto* flatten = new QCheckBox(tr("Show flatten options (experimental)"), &dialog);
    flatten->setObjectName(QStringLiteral("fileExplorerSettingsFlatten"));
    flatten->setChecked(showFlattenOptionsEnabled());
    layout->addWidget(flatten);
    // Files Appearance > Show the status center (Always vs during operations).
    auto* status_row = new QHBoxLayout();
    status_row->addWidget(new QLabel(tr("Show the status center"), &dialog));
    auto* status_visibility = new QComboBox(&dialog);
    status_visibility->setObjectName(QStringLiteral("fileExplorerSettingsStatusCenter"));
    status_visibility->addItem(tr("Always"));
    status_visibility->addItem(tr("During ongoing file operations"));
    status_visibility->setCurrentIndex(statusCenterDuringOperationsOnly() ? 1 : 0);
    status_row->addWidget(status_visibility, 1);
    layout->addLayout(status_row);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addWidget(buttons);

    if (dialog.exec() != QDialog::Accepted) {
        return;
    }
    applyExplorerSettings(checkboxes->isChecked(),
                          double_click_up->isChecked(),
                          filter_header->isChecked(),
                          flatten->isChecked(),
                          status_visibility->currentIndex());
}

bool FileManagementExplorerPanel::statusCenterDuringOperationsOnly() const {
    QSettings settings;
    settings.beginGroup(QString::fromLatin1(kExplorerSettingsGroup));
    return settings.value(QString::fromLatin1(kStatusCenterVisibilityKey), 0).toInt() == 1;
}

void FileManagementExplorerPanel::applyExplorerSettings(const bool checkboxes,
                                                        const bool double_click_up,
                                                        const bool filter_header,
                                                        const bool flatten,
                                                        const int status_center_visibility) {
    setShowCheckboxes(checkboxes);
    QSettings settings;
    settings.beginGroup(QString::fromLatin1(kExplorerSettingsGroup));
    settings.setValue(QString::fromLatin1(kDoubleClickToGoUpKey), double_click_up);
    settings.setValue(QString::fromLatin1(kShowFilterHeaderKey), filter_header);
    settings.setValue(QString::fromLatin1(kShowFlattenKey), flatten);
    settings.setValue(QString::fromLatin1(kStatusCenterVisibilityKey), status_center_visibility);
    settings.endGroup();
    if (m_filter_header != nullptr) {
        m_filter_header->setVisible(filter_header);
    }
    syncStatusCenterButton();
    Q_EMIT statusMessage(tr("Explorer settings saved."), sak::kTimerStatusMessageMs);
}

void FileManagementExplorerPanel::handleRenameTapEvent(QAbstractItemView* view, QEvent* event) {
    switch (event->type()) {
    case QEvent::MouseButtonPress: {
        const auto* mouse = static_cast<QMouseEvent*>(event);
        if (mouse->button() == Qt::LeftButton) {
            // Selection state is read BEFORE the view processes the press, so
            // this observes whether the clicked item was already selected
            // (Files preRenamingItem semantics).
            armRenameTapCandidate(view, mouse);
        }
        return;
    }
    case QEvent::MouseButtonRelease:
        handleRenameTapRelease(view, static_cast<QMouseEvent*>(event));
        return;
    case QEvent::MouseButtonDblClick:
        // The double-click opened the item instead (Files ResetRenameDoubleClick).
        cancelRenameTap();
        return;
    default:
        return;
    }
}

void FileManagementExplorerPanel::handleRenameTapRelease(QAbstractItemView* view,
                                                         const QMouseEvent* mouse) {
    if (mouse->button() != Qt::LeftButton ||
        (mouse->modifiers() & (Qt::ControlModifier | Qt::ShiftModifier)) != 0) {
        cancelRenameTap();
        return;
    }
    if ((m_rename_tap_timer != nullptr) && m_rename_tap_view == view &&
        m_rename_tap_candidate.isValid() &&
        view->indexAt(mouse->pos()) == QModelIndex(m_rename_tap_candidate)) {
        m_rename_tap_timer->start();
    } else {
        cancelRenameTap();
    }
}

void FileManagementExplorerPanel::armRenameTapCandidate(QAbstractItemView* view,
                                                        const QMouseEvent* mouse) {
    cancelRenameTap();
    const QModelIndex index = view->indexAt(mouse->pos());
    // Files requires the tap to land on the item name; in the details view
    // that is the name column, in the icon views the whole cell is the item.
    if (!index.isValid() || index.column() != FileExplorerItemModel::NameColumn) {
        return;
    }
    const auto* selection_model = view->selectionModel();
    if ((selection_model == nullptr) || !selection_model->isSelected(index) ||
        selection_model->selectedRows().size() != 1) {
        return;
    }
    m_rename_tap_candidate = index;
    m_rename_tap_view = view;
}

void FileManagementExplorerPanel::cancelRenameTap() {
    if (m_rename_tap_timer != nullptr) {
        m_rename_tap_timer->stop();
    }
    m_rename_tap_candidate = QPersistentModelIndex();
    m_rename_tap_view = nullptr;
}

void FileManagementExplorerPanel::onRenameTapTimeout() {
    QAbstractItemView* view = m_rename_tap_view.data();
    const QModelIndex index = m_rename_tap_candidate;
    cancelRenameTap();
    if ((view == nullptr) || !index.isValid() || !currentTarget().can_write_files) {
        return;
    }
    const auto* selection_model = view->selectionModel();
    if ((selection_model == nullptr) || !selection_model->isSelected(index) ||
        selection_model->selectedRows().size() != 1) {
        return;
    }
    view->setCurrentIndex(index);
    view->edit(index);
}

void FileManagementExplorerPanel::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    const int width = (event != nullptr) ? event->size().width() : this->width();
    // Files adaptive triggers work both ways: panes hide below their
    // breakpoints and come back once the shell is wide enough again (a
    // construction-time narrow resize must not hide them permanently).
    if (m_sidebar != nullptr) {
        m_sidebar->setVisible(width >= kSidebarCollapseWidth);
    }
    if (m_details_pane != nullptr) {
        m_details_pane->setVisible(m_details_pane_enabled && width >= kDetailsTabsCollapseWidth);
    }
    if (m_omnibar != nullptr) {
        // Files NavigationToolbar collapses Forward/Up/Refresh into an
        // overflow flyout below its narrow breakpoint.
        m_omnibar->setNarrowMode(width < kNavClusterCollapseWidth);
    }
}

void FileManagementExplorerPanel::installCommandShortcuts() {
    const QVector<FileExplorerCommandId> panel_shortcuts{
        FileExplorerCommandId::Back,
        FileExplorerCommandId::Forward,
        FileExplorerCommandId::Up,
        FileExplorerCommandId::Home,
        FileExplorerCommandId::Refresh,
        FileExplorerCommandId::CopyItemPath,
        FileExplorerCommandId::CopyItemPathQuoted,
        FileExplorerCommandId::Preview,
        FileExplorerCommandId::Properties,
        FileExplorerCommandId::SelectAll,
        FileExplorerCommandId::ToggleSelect,
        FileExplorerCommandId::ClearSelection,
        FileExplorerCommandId::NewFolder,
        FileExplorerCommandId::WriteFile,
        FileExplorerCommandId::Rename,
        FileExplorerCommandId::Delete,
        FileExplorerCommandId::DeletePermanently,
        FileExplorerCommandId::ToggleHiddenItems,
        FileExplorerCommandId::ToggleFileExtensions,
        FileExplorerCommandId::ViewDetails,
        FileExplorerCommandId::ViewList,
        FileExplorerCommandId::ViewGrid,
        FileExplorerCommandId::ViewCards,
        FileExplorerCommandId::ViewColumns,
        FileExplorerCommandId::ViewAdaptive,
        FileExplorerCommandId::TogglePreviewPane,
        FileExplorerCommandId::ToggleDualPane,
        FileExplorerCommandId::DuplicateTab,
        FileExplorerCommandId::ReopenClosedTab,
        FileExplorerCommandId::Hash,
        FileExplorerCommandId::CopyOut,
        FileExplorerCommandId::CopyItems,
        FileExplorerCommandId::CutItems,
        FileExplorerCommandId::Paste,
        FileExplorerCommandId::PasteIntoSelection,
        FileExplorerCommandId::CopyToOtherPane,
        FileExplorerCommandId::FocusOtherPane,
        FileExplorerCommandId::IncreaseSize,
        FileExplorerCommandId::DecreaseSize,
        FileExplorerCommandId::OpenInTerminal,
        FileExplorerCommandId::ExtractFiles,
        FileExplorerCommandId::ExtractHereSmart,
        FileExplorerCommandId::Undo,
        FileExplorerCommandId::Redo,
    };

    for (const FileExplorerCommandId command_id : panel_shortcuts) {
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

    installAuxiliaryShortcuts();
}

void FileManagementExplorerPanel::installAuxiliaryShortcuts() {
    // Files SearchAction: Ctrl+F (and F3 below) enter the omnibar search
    // mode; the filter header moved to Ctrl+Shift+F (ToggleFilterHeaderAction).
    auto* search_shortcut = new QShortcut(QKeySequence(QStringLiteral("Ctrl+F")), this);
    search_shortcut->setContext(Qt::WidgetWithChildrenShortcut);
    connect(search_shortcut, &QShortcut::activated, this, [this]() {
        if (m_omnibar) {
            m_omnibar->setMode(FileExplorerOmnibarMode::Search);
        }
    });

    auto* filter_shortcut = new QShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+F")), this);
    filter_shortcut->setContext(Qt::WidgetWithChildrenShortcut);
    connect(filter_shortcut,
            &QShortcut::activated,
            this,
            &FileManagementExplorerPanel::toggleFilterHeader);

    // Files OpenSettingsAction: Ctrl+, opens the explorer settings.
    auto* settings_shortcut = new QShortcut(QKeySequence(QStringLiteral("Ctrl+,")), this);
    settings_shortcut->setContext(Qt::WidgetWithChildrenShortcut);
    connect(settings_shortcut,
            &QShortcut::activated,
            this,
            &FileManagementExplorerPanel::showExplorerSettings);

    auto* palette_shortcut = new QShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+P")), this);
    palette_shortcut->setContext(Qt::WidgetWithChildrenShortcut);
    connect(palette_shortcut,
            &QShortcut::activated,
            this,
            &FileManagementExplorerPanel::showCommandPalette);

    // Files ToggleSidebarAction: Ctrl+B shows/hides the sidebar pane.
    auto* sidebar_shortcut = new QShortcut(QKeySequence(QStringLiteral("Ctrl+B")), this);
    sidebar_shortcut->setContext(Qt::WidgetWithChildrenShortcut);
    connect(sidebar_shortcut, &QShortcut::activated, this, [this]() {
        if (m_sidebar_toggle_button) {
            m_sidebar_toggle_button->click();
        }
    });

    // Open (Enter) is handled by the per-view key filter, which owns the
    // whole Files Enter activation matrix (plain/Ctrl/Ctrl+Shift/Alt).

    installFilesAliasShortcuts();
    installTabShortcuts();
    installPaneShortcuts();
}

QShortcut* FileManagementExplorerPanel::addPanelShortcut(const QString& key_sequence) {
    auto* shortcut = new QShortcut(QKeySequence(key_sequence), this);
    shortcut->setContext(Qt::WidgetWithChildrenShortcut);
    return shortcut;
}

void FileManagementExplorerPanel::installFilesAliasShortcuts() {
    // Files secondary hotkeys: Backspace = NavigateBack, Ctrl+R =
    // RefreshItems, Ctrl+D = DeleteItem, F3 = Search. Line edits keep these
    // keys because QLineEdit claims them in ShortcutOverride.
    connect(addPanelShortcut(QStringLiteral("Backspace")), &QShortcut::activated, this, [this]() {
        executeCommand(FileExplorerCommandId::Back);
    });
    connect(addPanelShortcut(QStringLiteral("Ctrl+R")), &QShortcut::activated, this, [this]() {
        executeCommand(FileExplorerCommandId::Refresh);
    });
    connect(addPanelShortcut(QStringLiteral("Ctrl+D")), &QShortcut::activated, this, [this]() {
        executeCommand(FileExplorerCommandId::Delete);
    });
    // Files SearchAction second hotkey: F3 also enters the search mode.
    connect(addPanelShortcut(QStringLiteral("F3")), &QShortcut::activated, this, [this]() {
        if (m_omnibar) {
            m_omnibar->setMode(FileExplorerOmnibarMode::Search);
        }
    });
    // Files EditPathAction: Ctrl+L / Alt+D focus the omnibar path editor.
    for (const auto* key : {"Ctrl+L", "Alt+D"}) {
        connect(addPanelShortcut(QString::fromLatin1(key)), &QShortcut::activated, this, [this]() {
            if (m_omnibar) {
                m_omnibar->setAddressEditMode(true);
            }
        });
    }
}

void FileManagementExplorerPanel::installTabShortcuts() {
    // Files NewTab / CloseSelectedTab / NextTab / PreviousTab actions plus
    // the MainPage Ctrl+1..9 tab-selection accelerators.
    connect(addPanelShortcut(QStringLiteral("Ctrl+T")),
            &QShortcut::activated,
            this,
            &FileManagementExplorerPanel::openCurrentLocationInNewTab);
    for (const auto* key : {"Ctrl+W", "Ctrl+F4"}) {
        connect(addPanelShortcut(QString::fromLatin1(key)), &QShortcut::activated, this, [this]() {
            if (m_tab_bar) {
                onTabCloseRequested(m_tab_bar->currentIndex());
            }
        });
    }
    connect(addPanelShortcut(QStringLiteral("Ctrl+Tab")), &QShortcut::activated, this, [this]() {
        cycleTab(1);
    });
    connect(addPanelShortcut(QStringLiteral("Ctrl+Shift+Tab")),
            &QShortcut::activated,
            this,
            [this]() { cycleTab(-1); });
    constexpr int kMaxTabShortcutDigit = 9;
    for (int digit = 1; digit <= kMaxTabShortcutDigit; ++digit) {
        connect(addPanelShortcut(QStringLiteral("Ctrl+%1").arg(digit)),
                &QShortcut::activated,
                this,
                [this, digit]() { selectTabByNumber(digit); });
    }
}

void FileManagementExplorerPanel::installPaneShortcuts() {
    // Files SplitPaneVertically / SplitPaneHorizontally / CloseActivePane.
    connect(addPanelShortcut(QStringLiteral("Alt+Shift+V")), &QShortcut::activated, this, [this]() {
        splitPane(Qt::Horizontal);
    });
    connect(addPanelShortcut(QStringLiteral("Alt+Shift+H")), &QShortcut::activated, this, [this]() {
        splitPane(Qt::Vertical);
    });
    connect(addPanelShortcut(QStringLiteral("Ctrl+Alt+W")), &QShortcut::activated, this, [this]() {
        if (m_dual_pane_enabled) {
            toggleDualPane();
        }
    });
}

void FileManagementExplorerPanel::cycleTab(const int direction) {
    constexpr int kMinTabsToCycle = 2;
    if ((m_tab_bar == nullptr) || m_tab_bar->count() < kMinTabsToCycle) {
        return;
    }
    const int count = m_tab_bar->count();
    m_tab_bar->setCurrentIndex((m_tab_bar->currentIndex() + direction + count) % count);
}

void FileManagementExplorerPanel::selectTabByNumber(const int digit) {
    if (m_tab_bar == nullptr) {
        return;
    }
    // Files MainPageViewModel: Ctrl+9 selects the last tab regardless of count.
    const int index = digit == 9 ? m_tab_bar->count() - 1 : digit - 1;
    if (index >= 0 && index < m_tab_bar->count()) {
        m_tab_bar->setCurrentIndex(index);
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
        if (m_pane != nullptr) {
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
    QFont header_font = m_target_list->font();
    header_font.setPointSize(ui::kFontSizeNote);
    header_font.setWeight(QFont::DemiBold);
    item->setFont(header_font);
}

void FileManagementExplorerPanel::appendSidebarTarget(const FileManagementTarget& target,
                                                      const int target_index) {
    const QIcon icon = FileExplorerIconRegistry::iconForKey(sidebarIconKeyForTarget(target));
    // Files SidebarStyles.xaml ItemNameTextBlock: one line, TextWrapping="NoWrap",
    // TextTrimming="CharacterEllipsis" -- the upstream sidebar row carries a name and
    // an optional decorator, never a second text line. This label used to append
    // "\n" + targetSubtitle(), which the item view never draws: Qt's item text
    // rendering collapses the newline and elides, so every row read as
    // "Home (Randy)  [Writable]..." -- a badge that looked truncated and a subtitle
    // the user could not see at any width. The subtitle moved into the tooltip.
    const QString label = QStringLiteral("%1  [%2]").arg(target.label, targetBadge(target));
    auto* item = new QListWidgetItem(icon, label, m_target_list);
    item->setData(kSidebarKindRole, static_cast<int>(SidebarEntryKind::Target));
    item->setData(kTargetIndexRole, target_index);
    // root_path names a mounted medium (including MTP/phone device names), so the tooltip -- a
    // sink with no plain-text mode -- shows it literally.
    item->setToolTip(
        ui::asLiteralRichText(QStringLiteral("%1\n%2\n%3")
                                  .arg(target.root_path,
                                       FileManagementFileSystemBridge::capabilitySummary(target),
                                       targetSubtitle(target))));
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
    for (int position = 0; position < target_ids.size(); ++position) {
        const QString& target_id = target_ids.at(position);
        const int index = targetIndexForId(target_id);
        if (index >= 0) {
            appendSidebarTarget(m_targets.at(index), index);
            if (warn_when_missing) {
                // Favorites rows carry their pin position for drag-reorder.
                m_target_list->item(m_target_list->count() - 1)
                    ->setData(kSidebarFavoritePosRole, position);
            }
        } else if (warn_when_missing) {
            appendStaleFavoriteRow(target_id);
        }
    }
}

void FileManagementExplorerPanel::appendStaleFavoriteRow(const QString& target_id) {
    // A saved favorite whose target is not currently connected stays visible as a
    // disabled, warning-marked row so the user knows the pin still exists and will
    // resolve again when the device returns, rather than silently vanishing.
    auto* item =
        new QListWidgetItem(FileExplorerIconRegistry::iconForKey(QStringLiteral("status-warning")),
                            tr("%1  [offline]").arg(target_id),
                            m_target_list);
    item->setData(kSidebarKindRole, static_cast<int>(SidebarEntryKind::StaleFavorite));
    item->setData(kSidebarTagRole, target_id);
    item->setFlags(Qt::NoItemFlags);
    item->setToolTip(tr("Saved favorite is not currently connected: %1. Right-click to "
                        "remove the pin.")
                         .arg(target_id));
}

bool FileManagementExplorerPanel::sidebarSectionVisible(const QString& section_id) const {
    QSettings settings;
    settings.beginGroup(QString::fromLatin1(kExplorerSettingsGroup));
    settings.beginGroup(QStringLiteral("SidebarSections"));
    const bool visible = settings.value(section_id, true).toBool();
    settings.endGroup();
    settings.endGroup();
    return visible;
}

void FileManagementExplorerPanel::setSidebarSectionVisible(const QString& section_id,
                                                           const bool visible) {
    QSettings settings;
    settings.beginGroup(QString::fromLatin1(kExplorerSettingsGroup));
    settings.beginGroup(QStringLiteral("SidebarSections"));
    settings.setValue(section_id, visible);
    settings.endGroup();
    settings.endGroup();
    settings.sync();
    rebuildTargetList();
}

void FileManagementExplorerPanel::addSidebarSectionToggleMenu(QMenu* parent_menu) {
    // Files MainPage.xaml SidebarContextMenu: one checkable toggle per
    // sidebar section, applied immediately.
    QMenu* sections = parent_menu->addMenu(tr("Show sections"));
    sections->setObjectName(QStringLiteral("fileExplorerSidebarSectionsMenu"));
    static constexpr std::array kSections = {
        std::pair{"home", QT_TR_NOOP("Home")},
        std::pair{"favorites", QT_TR_NOOP("Favorites")},
        std::pair{"thispc", QT_TR_NOOP("This PC")},
        std::pair{"mounted", QT_TR_NOOP("Mounted Volumes")},
        std::pair{"partitions", QT_TR_NOOP("Disks and Partitions")},
        std::pair{"rawimages", QT_TR_NOOP("Raw Images")},
        std::pair{"recent", QT_TR_NOOP("Recent")},
        std::pair{"certification", QT_TR_NOOP("Certification Targets")},
        std::pair{"tags", QT_TR_NOOP("Tags")},
    };
    for (const auto& [id, label] : kSections) {
        QAction* action = sections->addAction(tr(label));
        action->setCheckable(true);
        const QString section_id = QString::fromLatin1(id);
        action->setChecked(sidebarSectionVisible(section_id));
        connect(action, &QAction::toggled, this, [this, section_id](const bool checked) {
            setSidebarSectionVisible(section_id, checked);
        });
    }
}

void FileManagementExplorerPanel::appendVisibleSidebarSections() {
    if (sidebarSectionVisible(QStringLiteral("favorites"))) {
        appendSidebarTargetsById(tr("Favorites"),
                                 m_favorite_target_ids,
                                 /*warn_when_missing=*/true);
    }
    if (sidebarSectionVisible(QStringLiteral("thispc"))) {
        appendSidebarTargetsWhere(tr("This PC"), &isLocalFsTarget);
    }
    if (sidebarSectionVisible(QStringLiteral("mounted"))) {
        appendSidebarTargetsWhere(tr("Mounted Volumes"), &isMountedVolumeTarget);
    }
    if (sidebarSectionVisible(QStringLiteral("partitions"))) {
        appendSidebarTargetsWhere(tr("Disks and Partitions"), &isPartitionTarget);
    }
    if (sidebarSectionVisible(QStringLiteral("rawimages"))) {
        appendSidebarTargetsWhere(tr("Raw Images"), &isRawImageTarget);
    }
    if (sidebarSectionVisible(QStringLiteral("recent"))) {
        appendSidebarTargetsById(tr("Recent"), m_recent_target_ids);
    }
    if (sidebarSectionVisible(QStringLiteral("certification"))) {
        appendSidebarTargetsWhere(tr("Certification Targets"), &isCertificationTarget);
    }

    appendTagRows();
}

void FileManagementExplorerPanel::appendTagRows() {
    const QStringList tags = allKnownTags();
    if (tags.isEmpty() || !sidebarSectionVisible(QStringLiteral("tags"))) {
        return;
    }
    appendSidebarHeader(tr("Tags"));
    for (const QString& tag : tags) {
        auto* item = new QListWidgetItem(
            FileExplorerIconRegistry::iconForKey(QStringLiteral("tag")), tag, m_target_list);
        item->setData(kSidebarKindRole, static_cast<int>(SidebarEntryKind::Tag));
        item->setData(kSidebarTagRole, tag);
        item->setToolTip(
            ui::asLiteralRichText(tr("Filter the current folder to items tagged '%1'").arg(tag)));
    }
}

void FileManagementExplorerPanel::rebuildTargetList(const QString& preferred_target_id) {
    if (m_target_list == nullptr) {
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

    // Sections are settings-gated in a fixed order, mirroring the Files
    // sidebar (SidebarViewModel.SectionOrder + Show*Section settings).
    if (sidebarSectionVisible(QStringLiteral("home"))) {
        appendSidebarHeader(tr("Home"));
        auto* home =
            new QListWidgetItem(FileExplorerIconRegistry::iconForKey(QStringLiteral("home")),
                                tr("Home"),
                                m_target_list);
        home->setData(kSidebarKindRole, static_cast<int>(SidebarEntryKind::Home));
        home->setToolTip(tr("Open the first mounted local target."));
    }

    appendVisibleSidebarSections();

    m_target_list->blockSignals(false);
    if (m_sidebar != nullptr) {
        m_sidebar->refreshCompactPresentation();
    }
    if (!current_id.isEmpty()) {
        selectTargetById(current_id);
    }
}

void FileManagementExplorerPanel::setSidebarCompact(const bool compact) {
    if (m_sidebar == nullptr) {
        return;
    }
    m_sidebar->setCompact(compact);
    QSettings settings;
    settings.beginGroup(QString::fromLatin1(kExplorerSettingsGroup));
    settings.setValue(QString::fromLatin1(kSidebarCompactKey), compact);
    settings.endGroup();
}

void FileManagementExplorerPanel::selectTargetById(const QString& target_id) {
    if ((m_target_list == nullptr) || target_id.trimmed().isEmpty()) {
        return;
    }
    for (int row = 0; row < m_target_list->count(); ++row) {
        auto* item = m_target_list->item(row);
        if ((item == nullptr) ||
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
    if ((m_sidebar != nullptr) &&
        settings.value(QString::fromLatin1(kSidebarCompactKey), false).toBool()) {
        m_sidebar->setCompact(true);
    }
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
    if (m_pane == nullptr) {
        return;
    }
    clampFileExplorerLayoutSizes(m_pane_state.view.sizes);
    m_pane->setViewMode(m_pane_state.view.mode);
    m_pane->setLayoutSizes(m_pane_state.view.sizes);
    m_pane->setShowHiddenItems(m_pane_state.view.show_hidden);
    m_pane->setShowFileExtensions(m_pane_state.view.show_extensions);
    m_pane->setGrouping(m_pane_state.view.group_option,
                        m_pane_state.view.group_date_unit,
                        m_pane_state.view.group_order);
    if (auto* proxy = m_pane->sortFilterModel()) {
        proxy->setFolderSortPlacement(m_pane_state.view.folder_placement);
    }
    if (m_view_button != nullptr) {
        FileExplorerCommandId icon_command = FileExplorerCommandId::ViewDetails;
        switch (m_pane_state.view.mode) {
        case FileExplorerViewMode::List:
            icon_command = FileExplorerCommandId::ViewList;
            break;
        case FileExplorerViewMode::Grid:
        case FileExplorerViewMode::Adaptive:
            icon_command = FileExplorerCommandId::ViewGrid;
            break;
        case FileExplorerViewMode::Cards:
            icon_command = FileExplorerCommandId::ViewCards;
            break;
        case FileExplorerViewMode::Columns:
            icon_command = FileExplorerCommandId::ViewColumns;
            break;
        case FileExplorerViewMode::Details:
            break;
        }
        m_view_button->setIcon(FileExplorerIconRegistry::iconForCommand(icon_command));
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
    const auto read_size_kind = [&settings](const char* key, int& kind) {
        if (settings.contains(QString::fromLatin1(key))) {
            kind = settings.value(QString::fromLatin1(key)).toInt();
        }
    };
    read_size_kind(kDetailsSizeKey, m_pane_state.view.sizes.details);
    read_size_kind(kListSizeKey, m_pane_state.view.sizes.list);
    read_size_kind(kCardsSizeKey, m_pane_state.view.sizes.cards);
    read_size_kind(kGridSizeKey, m_pane_state.view.sizes.grid);
    read_size_kind(kColumnsSizeKey, m_pane_state.view.sizes.columns);
    if (settings.contains(QString::fromLatin1(kGroupOptionKey))) {
        m_pane_state.view.group_option = fileExplorerGroupOptionFromName(
            settings.value(QString::fromLatin1(kGroupOptionKey)).toString());
    }
    if (settings.contains(QString::fromLatin1(kGroupDirectionKey))) {
        m_pane_state.view.group_order = static_cast<Qt::SortOrder>(
            settings.value(QString::fromLatin1(kGroupDirectionKey)).toInt());
    }
    if (settings.contains(QString::fromLatin1(kGroupDateUnitKey))) {
        m_pane_state.view.group_date_unit = static_cast<FileExplorerGroupDateUnit>(
            settings.value(QString::fromLatin1(kGroupDateUnitKey)).toInt());
    }
    if (settings.contains(QString::fromLatin1(kFolderPlacementKey))) {
        m_pane_state.view.folder_placement = static_cast<FileExplorerFolderSortPlacement>(
            settings.value(QString::fromLatin1(kFolderPlacementKey)).toInt());
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
    settings.setValue(QString::fromLatin1(kDetailsSizeKey), m_pane_state.view.sizes.details);
    settings.setValue(QString::fromLatin1(kListSizeKey), m_pane_state.view.sizes.list);
    settings.setValue(QString::fromLatin1(kCardsSizeKey), m_pane_state.view.sizes.cards);
    settings.setValue(QString::fromLatin1(kGridSizeKey), m_pane_state.view.sizes.grid);
    settings.setValue(QString::fromLatin1(kColumnsSizeKey), m_pane_state.view.sizes.columns);
    settings.setValue(QString::fromLatin1(kGroupOptionKey),
                      fileExplorerGroupOptionName(m_pane_state.view.group_option));
    settings.setValue(QString::fromLatin1(kGroupDirectionKey),
                      static_cast<int>(m_pane_state.view.group_order));
    settings.setValue(QString::fromLatin1(kGroupDateUnitKey),
                      static_cast<int>(m_pane_state.view.group_date_unit));
    settings.setValue(QString::fromLatin1(kFolderPlacementKey),
                      static_cast<int>(m_pane_state.view.folder_placement));
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
    return (m_pane != nullptr) ? m_pane->activeItemView() : nullptr;
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
    const auto* selection_model = (m_pane != nullptr) ? m_pane->sharedSelectionModel() : nullptr;
    if ((selection_model == nullptr) || (m_pane == nullptr)) {
        return {};
    }
    const QModelIndexList rows = selection_model->selectedRows();
    if (rows.isEmpty()) {
        return {};
    }
    return m_pane->entryAtViewRow(rows.first().row()).path;
}

bool FileManagementExplorerPanel::selectedIsDirectory() const {
    const auto* selection_model = (m_pane != nullptr) ? m_pane->sharedSelectionModel() : nullptr;
    if ((selection_model == nullptr) || (m_pane == nullptr)) {
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
        if (blocker != nullptr) {
            *blocker = tr("No stable File Explorer target identity is selected.");
        }
        return false;
    }
    if (m_pane_state.location.target_id.value != target_id) {
        if (blocker != nullptr) {
            *blocker = tr("Selected target identity changed. Refresh target and retry.");
        }
        return false;
    }
    return true;
}

bool FileManagementExplorerPanel::targetStillSelected(const FileManagementTarget& target,
                                                      QString* blocker) const {
    if (!validateCurrentTargetIdentity(blocker)) {
        return false;
    }
    if (FileExplorerTargetId::fromTarget(currentTarget()).value !=
        FileExplorerTargetId::fromTarget(target).value) {
        if (blocker != nullptr) {
            *blocker = tr(
                "The selected target changed while the dialog was open; "
                "nothing was written.");
        }
        return false;
    }
    return true;
}

void FileManagementExplorerPanel::resetListingForUnavailableTarget(const QString& message,
                                                                   const bool is_error) {
    ++m_listing_revision[m_active_pane_index];
    ++m_columns_preview_revision;
    if (is_error) {
        m_summary_label->setText(message);
        m_item_model->clear();
    }
    if (m_pane != nullptr) {
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

// Files ShellViewModel: the folder filter clears on directory change.
void FileManagementExplorerPanel::clearFolderFilterOnNavigation() {
    if ((m_filter_box == nullptr) || m_filter_box->text().isEmpty()) {
        return;
    }
    const QSignalBlocker blocker(m_filter_box);
    m_filter_box->clear();
    if ((m_pane != nullptr) && (m_pane->sortFilterModel() != nullptr)) {
        m_pane->sortFilterModel()->setNameFilter(QString());
    }
}

// Completion routing for one async directory listing: dropped only when a
// NEWER load for the same pane superseded it (per-pane revisions). A result
// for the now-inactive pane (refreshOtherPane, dual-pane restore, or a user
// pane switch mid-load) hops over, populates that pane's own model, and hops
// back -- dropping it left the pane stuck on its loading state.
void FileManagementExplorerPanel::deliverListingResult(const FileManagementListResult& result,
                                                       const quint64 listing_revision,
                                                       const int load_pane) {
    if (listing_revision != m_listing_revision[load_pane]) {
        return;
    }
    if (load_pane != m_active_pane_index) {
        const int current = m_active_pane_index;
        activatePane(load_pane);
        populateTable(result);
        activatePane(current);
        return;
    }
    populateTable(result);
}

void FileManagementExplorerPanel::loadDirectory(const QString& path, const bool add_history) {
    clearFolderFilterOnNavigation();
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
    // A target switch must not leave the previous target's rows -- and their
    // selection -- actionable while the new listing loads: a command fired in
    // that window would run stale paths against the now-current target.
    if (m_pane_state.location.target_id.value != destination.target_id.value) {
        if (m_item_model != nullptr) {
            m_item_model->clear();
        }
        m_pane_state.selection.clear();
    }
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
    if (m_preview_text != nullptr) {
        m_preview_text->setPlainText(tr("Select a readable file and choose Preview."));
    }

    const int load_pane = m_active_pane_index;
    const quint64 listing_revision = ++m_listing_revision[load_pane];
    ++m_columns_preview_revision;
    if (m_pane != nullptr) {
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
                deliverListingResult(watcher->result(), listing_revision, load_pane);
            });
    watcher->setFuture(QtConcurrent::run([target, path = m_current_path]() {
        return FileManagementFileSystemBridge::listDirectory(target, path, kExplorerListMaxEntries);
    }));
}

void FileManagementExplorerPanel::loadColumnsPreview(const QString& path) {
    if (m_pane == nullptr) {
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
        if (m_pane != nullptr) {
            m_pane->showErrorState(result.blockers.join(QStringLiteral("; ")));
        }
        Q_EMIT statusMessage(tr("Explorer listing failed"), sak::kTimerStatusMessageMs);
        updateDetailsPane();
        updateActionButtons();
        return;
    }

    m_item_model->setEntries(result.entries);
    if (m_pane != nullptr) {
        if (result.entries.isEmpty()) {
            m_pane->showEmptyState(tr("This folder is empty."));
        } else if ((m_pane->sortFilterModel() != nullptr) &&
                   m_pane->sortFilterModel()->rowCount() == 0) {
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
    if (m_pending_select_name.isEmpty() || (m_pane == nullptr)) {
        return;
    }
    const QString name = m_pending_select_name;
    m_pending_select_name.clear();
    auto* view = currentItemView();
    if ((view == nullptr) || (view->model() == nullptr)) {
        return;
    }
    // Iterate the view's own model (the group proxy) so the row selected
    // matches the visible row even when group headers are injected.
    for (int row = 0; row < view->model()->rowCount(); ++row) {
        if (!m_pane->hasViewEntry(row)) {
            continue;
        }
        const QModelIndex index = view->model()->index(row, 0);
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

    if (m_preview_text != nullptr) {
        // The info pane preview region is visible on both the Details and
        // Preview tabs (Files InfoPane.xaml), so no tab switch is needed.
        m_preview_text->setPlainText(QString::fromUtf8(read.data));
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

    // Fresh cancel source for this hash (drops any token from a prior run).
    m_hashStopSource = std::stop_source{};
    const std::stop_token stop_token = m_hashStopSource.get_token();

    // A cancellable progress dialog that only appears if the hash runs long (setMinimumDuration),
    // so a quick hash of a small file stays silent while a large local file becomes abortable.
    constexpr int kHashProgressMinDurationMs = 500;
    auto* progress =
        new QProgressDialog(tr("Hashing %1...").arg(entry.name), tr("Cancel"), 0, 0, this);
    progress->setWindowModality(Qt::WindowModal);
    progress->setMinimumDuration(kHashProgressMinDurationMs);
    connect(progress, &QProgressDialog::canceled, this, [this]() {
        m_hashStopSource.request_stop();
    });

    auto* watcher = new QFutureWatcher<FileManagementHashResult>(this);
    const QString hash_target_id = FileExplorerTargetId::fromTarget(target).value;
    connect(watcher,
            &QFutureWatcher<FileManagementHashResult>::finished,
            this,
            [this, watcher, progress, name = entry.name, hash_target_id]() {
                progress->close();
                progress->deleteLater();
                watcher->deleteLater();
                const FileManagementHashResult result = watcher->result();
                if (result.cancelled) {
                    Q_EMIT statusMessage(tr("Hashing of %1 cancelled").arg(name),
                                         sak::kTimerStatusMessageMs);
                    return;
                }
                if (!result.ok) {
                    Q_EMIT statusMessage(
                        tr("Hash failed: %1").arg(result.blockers.join(QStringLiteral("; "))),
                        sak::kTimerStatusMessageMs);
                    return;
                }
                // The Evidence tab is headed by the CURRENT target; record which
                // target this hash actually came from.
                m_last_hash_target_id = hash_target_id;
                m_last_hash_name = name;
                m_last_hash_sha256 = result.sha256;
                m_last_hash_capped = result.capped;
                updateDetailsPane();
                Q_EMIT statusMessage(tr("SHA-256 of %1: %2").arg(name, result.sha256),
                                     sak::kTimerStatusMessageMs);
            });
    watcher->setFuture(QtConcurrent::run([target, path, stop_token]() {
        return FileManagementFileSystemBridge::hashFile(
            target, path, kExplorerHashMaxBytes, stop_token);
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
    // Copy Out runs on the transfer worker with a status-center card (Files
    // CopyItem to a picked destination). Explicit capped semantics: an
    // oversized raw file copies capped and marked instead of failing.
    startExportWorker(currentTarget(),
                      {.source_path = entry.path,
                       .destination_path = destination,
                       .size_bytes = entry.size_bytes,
                       .directory = false},
                      QFileInfo(destination).absolutePath());
}

void FileManagementExplorerPanel::startExportWorker(const FileManagementTarget& source_target,
                                                    const FileExplorerTransferItem& item,
                                                    const QString& destination_dir) {
    FileExplorerTransferRequest request;
    request.source_target = source_target;
    request.destination_target = FileManagementFileSystemBridge::localTarget(QString());
    request.items = {item};
    request.allow_capped_raw_reads = true;
    request.raw_read_cap = kExplorerHashMaxBytes;
    TransferCompletion completion;
    completion.source_target = source_target;
    completion.destination_target = request.destination_target;
    completion.destination_dir = destination_dir;
    completion.status_template = tr("Exported %1 of %2 item(s).");
    completion.requested_count = 1;
    completion.record_history = false;
    startTransferWorker(request, completion);
}

FileManagementExplorerPanel::ClipboardBatch FileManagementExplorerPanel::collectClipboardBatch(
    const FileExplorerSelection& selection, const FileManagementTarget& target) {
    ClipboardBatch batch;
    for (const FileManagementEntry& entry : selection.entries) {
        if (!entry.directory && !entry.regular_file) {
            ++batch.skipped;
            continue;
        }
        QJsonObject item;
        item.insert(QStringLiteral("path"), entry.path);
        item.insert(QStringLiteral("size"), QString::number(entry.size_bytes));
        item.insert(QStringLiteral("dir"), entry.directory);
        batch.items.append(item);
        batch.lines.append(entry.path);
        if (target.local_file_system) {
            batch.urls.append(QUrl::fromLocalFile(entry.path));
        }
    }
    return batch;
}

void FileManagementExplorerPanel::copySelectionToClipboard(const bool move) {
    const FileManagementTarget target = currentTarget();
    // Files TransferHelpers: every new transfer un-dims the prior cut first.
    clearCutMarks();
    const ClipboardBatch batch = collectClipboardBatch(currentSelection(), target);
    if (batch.items.isEmpty()) {
        Q_EMIT statusMessage(tr("Select files or folders to copy."), sak::kTimerStatusMessageMs);
        return;
    }
    QJsonObject payload;
    payload.insert(QStringLiteral("target"), FileExplorerTargetId::fromTarget(target).value);
    // Files DataPackage RequestedOperation: cut publishes Move, copy Copy.
    payload.insert(QStringLiteral("operation"),
                   move ? QStringLiteral("move") : QStringLiteral("copy"));
    payload.insert(QStringLiteral("items"), batch.items);
    auto* mime = new QMimeData;
    mime->setData(QLatin1String(kExplorerClipboardMime),
                  QJsonDocument(payload).toJson(QJsonDocument::Compact));
    mime->setText(batch.lines.join(QLatin1Char('\n')));
    if (!batch.urls.isEmpty()) {
        mime->setUrls(batch.urls);
    }
    QApplication::clipboard()->setMimeData(mime);
    if (move && (m_item_model != nullptr)) {
        // Files dims cut items at 0.4 opacity until the move-paste lands.
        m_item_model->setCutPaths(QSet<QString>(batch.lines.cbegin(), batch.lines.cend()));
    }
    const QString verb = move ? tr("Cut") : tr("Copied");
    Q_EMIT statusMessage(batch.skipped > 0 ? tr("%1 %2 item(s); %3 special item(s) skipped.")
                                                 .arg(verb)
                                                 .arg(batch.items.size())
                                                 .arg(batch.skipped)
                                           : tr("%1 %2 item(s).").arg(verb).arg(batch.items.size()),
                         sak::kTimerStatusMessageMs);
    updateActionButtons();
}

void FileManagementExplorerPanel::clearCutMarks() const {
    for (const FileExplorerPane* pane : {m_pane_a, m_pane_b}) {
        if ((pane != nullptr) && (pane->itemModel() != nullptr)) {
            pane->itemModel()->setCutPaths({});
        }
    }
}

void FileManagementExplorerPanel::finishMovePaste() {
    // Files packageView.ReportOperationCompleted: a completed move consumes
    // the clipboard and clears the cut dimming.
    clearCutMarks();
    QApplication::clipboard()->clear();
    updateActionButtons();
}

bool FileManagementExplorerPanel::clipboardHasPasteableFiles() const {
    return mimeHasPasteableItems(QApplication::clipboard()->mimeData());
}

bool FileManagementExplorerPanel::mimeHasPasteableItems(const QMimeData* mime) {
    if (mime == nullptr) {
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
        if (!url.isLocalFile()) {
            return false;
        }
        const QFileInfo info(url.toLocalFile());
        return info.isFile() || info.isDir();
    });
}

QMimeData* FileManagementExplorerPanel::buildDragMimeData(const int pane_index,
                                                          const QList<int>& rows) {
    // The drag starts in this pane's view; make it the active pane so the
    // payload target identity and any drop-time dialogs match the source.
    activatePane(pane_index);
    if (m_item_model == nullptr) {
        return nullptr;
    }
    FileExplorerSelection selection;
    for (const int row : rows) {
        if (m_item_model->hasEntry(row)) {
            selection.entries.append(m_item_model->entryAt(row));
        }
    }
    const FileManagementTarget target = currentTarget();
    const ClipboardBatch batch = collectClipboardBatch(selection, target);
    if (batch.items.isEmpty()) {
        return nullptr;
    }
    QJsonObject payload;
    payload.insert(QStringLiteral("target"), FileExplorerTargetId::fromTarget(target).value);
    // The transfer direction is decided at drop time by the modifier cascade;
    // the payload advertises copy so an external consumer treats it as one.
    payload.insert(QStringLiteral("operation"), QStringLiteral("copy"));
    payload.insert(QStringLiteral("items"), batch.items);
    QMimeData* mime = target.local_file_system ? new QMimeData
                                               : new RawExportMimeData(target, selection.entries);
    mime->setData(QLatin1String(kExplorerClipboardMime),
                  QJsonDocument(payload).toJson(QJsonDocument::Compact));
    mime->setText(batch.lines.join(QLatin1Char('\n')));
    if (!batch.urls.isEmpty()) {
        mime->setUrls(batch.urls);
    }
    return mime;
}

Qt::DropAction FileManagementExplorerPanel::dropActionFor(const Qt::KeyboardModifiers modifiers,
                                                          const QMimeData* mime) const {
    // Files DragDropHelpers modifier cascade: Ctrl forces Copy and Shift forces
    // Move (the Link leg is excluded - shortcuts are meaningless on raw
    // volumes); an unmodified drop moves within the same target and copies
    // across targets or from external sources.
    if (modifiers.testFlag(Qt::ControlModifier)) {
        return Qt::CopyAction;
    }
    if (modifiers.testFlag(Qt::ShiftModifier)) {
        return Qt::MoveAction;
    }
    if ((mime != nullptr) && mime->hasFormat(QLatin1String(kExplorerClipboardMime))) {
        const QJsonObject payload =
            QJsonDocument::fromJson(mime->data(QLatin1String(kExplorerClipboardMime))).object();
        if (payload.value(QStringLiteral("target")).toString() ==
            FileExplorerTargetId::fromTarget(currentTarget()).value) {
            return Qt::MoveAction;
        }
    }
    return Qt::CopyAction;
}

bool FileManagementExplorerPanel::handleViewportDragEvent(QAbstractItemView* view, QEvent* event) {
    switch (event->type()) {
    case QEvent::DragEnter: {
        auto* drag = static_cast<QDragEnterEvent*>(event);
        if (!mimeHasPasteableItems(drag->mimeData())) {
            return false;
        }
        activatePane(paneIndexForView(view));
        drag->setDropAction(dropActionFor(drag->modifiers(), drag->mimeData()));
        drag->accept();
        return true;
    }
    case QEvent::DragMove: {
        auto* drag = static_cast<QDragMoveEvent*>(event);
        if (!mimeHasPasteableItems(drag->mimeData())) {
            return false;
        }
        const QModelIndex index = view->indexAt(drag->position().toPoint());
        if (index.isValid() && index.data(FileExplorerItemModel::EntryDirectoryRole).toBool()) {
            armSpringOpen(index.data(FileExplorerItemModel::EntryPathRole).toString());
        } else {
            cancelSpringOpen();
        }
        drag->setDropAction(dropActionFor(drag->modifiers(), drag->mimeData()));
        drag->accept();
        return true;
    }
    case QEvent::DragLeave:
        cancelSpringOpen();
        return false;
    case QEvent::Drop:
        handleDropOnView(view, event);
        return true;
    default:
        return false;
    }
}

void FileManagementExplorerPanel::handleDropOnView(QAbstractItemView* view, QEvent* event) {
    auto* drop = static_cast<QDropEvent*>(event);
    cancelSpringOpen();
    if (!mimeHasPasteableItems(drop->mimeData())) {
        drop->ignore();
        return;
    }
    // A drop onto a folder row lands inside that folder; anywhere else lands
    // in the current directory (Files ItemsLayout drop resolution).
    const QModelIndex index = view->indexAt(drop->position().toPoint());
    const bool onto_directory = index.isValid() &&
                                index.data(FileExplorerItemModel::EntryDirectoryRole).toBool();
    const QString destination = onto_directory
                                    ? index.data(FileExplorerItemModel::EntryPathRole).toString()
                                    : m_current_path;
    const Qt::DropAction action = dropActionFor(drop->modifiers(), drop->mimeData());
    PasteSources sources = collectPasteSources(drop->mimeData());
    sources.move = action == Qt::MoveAction;
    sources.clipboard = false;
    drop->setDropAction(action);
    drop->accept();
    // Deferred so dialogs (typed raw confirm, conflict resolution) never run
    // inside the native drag loop; the mime object dies with the drag, so the
    // sources were copied out above.
    QMetaObject::invokeMethod(
        this,
        [this, sources, destination]() { performDrop(sources, destination); },
        Qt::QueuedConnection);
}

void FileManagementExplorerPanel::performDrop(const PasteSources& sources,
                                              const QString& destination_dir) {
    if (sources.host_files.isEmpty() && sources.raw_items.isEmpty()) {
        return;
    }
    if (sources.move && pasteSameTargetMove(currentTarget(), sources, destination_dir)) {
        return;
    }
    if (!preparePasteDestination(sources)) {
        return;
    }
    executePaste(sources, destination_dir);
}

void FileManagementExplorerPanel::armSpringOpen(const QString& directory_path) {
    if (m_spring_open_timer == nullptr) {
        m_spring_open_timer = new QTimer(this);
        m_spring_open_timer->setSingleShot(true);
        m_spring_open_timer->setInterval(kSpringOpenMs);
        connect(m_spring_open_timer, &QTimer::timeout, this, [this]() {
            if (!m_spring_open_path.isEmpty()) {
                loadDirectory(m_spring_open_path);
            }
        });
    }
    if (m_spring_open_path == directory_path && m_spring_open_timer->isActive()) {
        return;
    }
    m_spring_open_path = directory_path;
    m_spring_open_timer->start();
}

void FileManagementExplorerPanel::cancelSpringOpen() {
    if (m_spring_open_timer != nullptr) {
        m_spring_open_timer->stop();
    }
    m_spring_open_path.clear();
}

int FileManagementExplorerPanel::paneIndexForView(const QAbstractItemView* view) const {
    constexpr int kPaneCount = 2;
    for (int index = 0; index < kPaneCount; ++index) {
        const FileExplorerPane* pane = index == 0 ? m_pane_a : m_pane_b;
        if ((pane != nullptr) && pane->itemViews().contains(const_cast<QAbstractItemView*>(view))) {
            return index;
        }
    }
    return m_active_pane_index;
}

// Split an internal clipboard payload's items into host paths (local source target) or
// raw items (path + size + directory flag) for the paste routes.
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
                {.path = path,
                 .size_bytes = item.value(QStringLiteral("size")).toString().toULongLong(),
                 .directory = item.value(QStringLiteral("dir")).toBool()});
        }
    }
}

FileManagementExplorerPanel::PasteSources FileManagementExplorerPanel::collectPasteSources(
    const QMimeData* mime) const {
    PasteSources sources;
    if (mime == nullptr) {
        return sources;
    }
    if (mime->hasFormat(QLatin1String(kExplorerClipboardMime))) {
        const QJsonObject payload =
            QJsonDocument::fromJson(mime->data(QLatin1String(kExplorerClipboardMime))).object();
        sources.source_target_id = payload.value(QStringLiteral("target")).toString();
        sources.move = payload.value(QStringLiteral("operation")).toString() ==
                       QStringLiteral("move");
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
        if (!url.isLocalFile()) {
            continue;
        }
        const QFileInfo info(url.toLocalFile());
        if (info.isFile() || info.isDir()) {
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

bool FileManagementExplorerPanel::confirmTypedRawMove(const FileManagementTarget& target,
                                                      const int file_count) {
    // A move removes the source entries from raw media through the certified
    // writer with no recycle bin behind it, so it takes the same typed
    // acknowledgement a raw import does.
    bool accepted = false;
    const QString typed = QInputDialog::getText(
        this,
        tr("Confirm Raw Move"),
        tr("This moves %1 item(s) on raw media through the certified writer.\n"
           "Raw target: %2\nFile system: %3\nIdentity: %4\n\n"
           "The source entries are removed permanently. Type MOVE to continue:")
            .arg(QString::number(file_count),
                 target.label,
                 target.file_system,
                 FileExplorerTargetId::fromTarget(target).value),
        QLineEdit::Normal,
        {},
        &accepted);
    return accepted && typed.trimmed() == QStringLiteral("MOVE");
}

FileManagementExplorerPanel::PasteCollisionChoice
FileManagementExplorerPanel::resolvePasteCollision(const QString& name,
                                                   const bool multiple,
                                                   PasteCollisionPolicy* policy) {
    if (policy->apply_to_all) {
        return policy->choice;
    }
    // Files conflict dialog (FileSystemDialogViewModel): Generate new name is
    // the default resolve option, with Replace and Skip alternatives and an
    // apply-to-all aggregation when several items collide.
    QMessageBox box(this);
    box.setWindowTitle(tr("Replace or Skip Files"));
    box.setIcon(QMessageBox::Question);
    // The name comes off the (possibly foreign) file system: AutoText would let a
    // crafted name render as markup and spoof this destructive prompt.
    box.setTextFormat(Qt::PlainText);
    box.setText(tr("'%1' already exists in this folder.").arg(name));
    auto* generate = box.addButton(tr("Generate new name"), QMessageBox::AcceptRole);
    auto* replace = box.addButton(tr("Replace"), QMessageBox::DestructiveRole);
    box.addButton(tr("Skip"), QMessageBox::RejectRole);
    box.setDefaultButton(generate);
    QCheckBox* apply_all = nullptr;
    if (multiple) {
        apply_all = new QCheckBox(tr("Do this for all conflicts"), &box);
        box.setCheckBox(apply_all);
    }
    logMessage(tr("Paste conflict for %1").arg(name));
    box.exec();
    PasteCollisionChoice choice = PasteCollisionChoice::GenerateNew;
    if (box.clickedButton() == replace) {
        choice = PasteCollisionChoice::Replace;
    } else if (box.clickedButton() != generate) {
        choice = PasteCollisionChoice::Skip;
    }
    policy->choice = choice;
    policy->apply_to_all = (apply_all != nullptr) && apply_all->isChecked();
    return choice;
}

// Resolve a destination-name collision for one item at destination->path. Returns
// false when the item should be skipped; Generate rewrites the path with the Files
// incremental name; Replace only MARKS the item (destination->replace) - the actual
// occupant delete is deferred to just before that item's own copy, so a cancel or
// failure earlier in the batch never costs destinations that were not rewritten.
bool FileManagementExplorerPanel::resolvePasteDestination(const FileManagementTarget& target,
                                                          const bool multiple,
                                                          PasteCollisionPolicy* policy,
                                                          PasteDestination* destination,
                                                          QStringList* blockers) {
    const QString name = nameForPath(destination->path, target.local_file_system);
    const QString destination_dir = parentPathForEntry(destination->path, target.local_file_system);
    const PasteEntryKind kind = destinationEntryKind(target, destination_dir, name);
    if (kind == PasteEntryKind::None) {
        return true;
    }
    if (kind == PasteEntryKind::Unknown) {
        // The destination listing was not authoritative, so a Replace could
        // delete the wrong kind or an unseen collision could be overwritten;
        // fail closed and let the user retry rather than risk data loss.
        blockers->append(tr("Could not verify whether %1 already exists at the destination; "
                            "the item was skipped.")
                             .arg(name));
        return false;
    }
    const PasteCollisionChoice choice = resolvePasteCollision(name, multiple, policy);
    if (choice == PasteCollisionChoice::Skip) {
        return false;
    }
    if (choice == PasteCollisionChoice::GenerateNew) {
        const QString unique = uniqueChildName(target, destination_dir, name);
        if (unique.isEmpty()) {
            blockers->append(tr("Could not find a free name for %1 at the destination; "
                                "the item was skipped.")
                                 .arg(name));
            return false;
        }
        destination->path = childPathFor(destination_dir, unique, target.local_file_system);
        return true;
    }
    destination->replace = true;
    return true;
}

QString FileManagementExplorerPanel::availableChildName(const FileManagementTarget& target,
                                                        const QString& directory,
                                                        const QString& name) const {
    // The name itself when free; otherwise the Files incremental "{name} (n)".
    return destinationOccupied(target, directory, name) ? uniqueChildName(target, directory, name)
                                                        : name;
}

QString FileManagementExplorerPanel::uniqueChildName(const FileManagementTarget& target,
                                                     const QString& directory,
                                                     const QString& name) const {
    // Files FileOperationsHelpers.GetIncrementalName: "{name} ({n}){ext}"
    // starting at 2.
    const int last_dot = static_cast<int>(name.lastIndexOf(QLatin1Char('.')));
    const QString base = last_dot > 0 ? name.left(last_dot) : name;
    const QString extension = last_dot > 0 ? name.mid(last_dot) : QString();
    constexpr int kIncrementalNameStart = 2;
    constexpr int kMaxIncrementalNameAttempts = 10'000;
    for (int index = kIncrementalNameStart; index < kMaxIncrementalNameAttempts; ++index) {
        const QString candidate = QStringLiteral("%1 (%2)%3").arg(base).arg(index).arg(extension);
        if (!destinationOccupied(target, directory, candidate)) {
            return candidate;
        }
    }
    // Every candidate in the Files window is occupied, or none of them could be
    // verified. Handing back the last one would name an entry that already
    // exists, and the write that follows is overwrite-capable, so return nothing
    // and let the caller block the item.
    return {};
}

FileManagementExplorerPanel::PasteEntryKind FileManagementExplorerPanel::localDestinationEntryKind(
    const QString& directory, const QString& name) {
    const QFileInfo info(childPathFor(directory, name, true));
    if (info.exists()) {
        return info.isDir() ? PasteEntryKind::Directory : PasteEntryKind::File;
    }
    // exists() resolves the link and stats the resolved object, so it is also
    // false for a dangling reparse point and for a name this process cannot
    // stat at all. Neither proves the name is free, and the write that follows
    // is overwrite-capable, so anything short of a readable parent plus a
    // clean absence fails closed.
    if (info.isSymLink() || info.isJunction()) {
        return PasteEntryKind::Unknown;
    }
    const QFileInfo parent_info(directory);
    if (!parent_info.isDir() || !parent_info.isReadable()) {
        return PasteEntryKind::Unknown;
    }
    return PasteEntryKind::None;
}

FileManagementExplorerPanel::PasteEntryKind FileManagementExplorerPanel::rawDestinationEntryKind(
    const FileManagementTarget& target, const QString& directory, const QString& name) {
    // Raw APFS/HFS+ default to case-insensitive, so a differing-case name still
    // collides; matching case-insensitively can only over-detect, which is the
    // safe bias (a false collision picks a new name, never a silent overwrite).
    const auto matches = [&name](const FileManagementEntry& entry) {
        return QString::compare(entry.name, name, Qt::CaseInsensitive) == 0;
    };
    // Query a fresh listing one past the cap so truncation is detectable; the
    // display model may itself be truncated, so it is not authoritative here.
    const FileManagementListResult listing = FileManagementFileSystemBridge::listDirectory(
        target, directory, kExplorerListMaxEntries + 1);
    if (!listing.ok) {
        return PasteEntryKind::Unknown;
    }
    for (const FileManagementEntry& entry : listing.entries) {
        if (matches(entry)) {
            return entry.directory ? PasteEntryKind::Directory : PasteEntryKind::File;
        }
    }
    // A full-to-the-cap listing might hide a collision past the window.
    if (listing.entries.size() > kExplorerListMaxEntries) {
        return PasteEntryKind::Unknown;
    }
    return PasteEntryKind::None;
}

FileManagementExplorerPanel::PasteEntryKind FileManagementExplorerPanel::destinationEntryKind(
    const FileManagementTarget& target, const QString& directory, const QString& name) {
    if (target.local_file_system) {
        return localDestinationEntryKind(directory, name);
    }
    return rawDestinationEntryKind(target, directory, name);
}

bool FileManagementExplorerPanel::destinationOccupied(const FileManagementTarget& target,
                                                      const QString& directory,
                                                      const QString& name) {
    // Unknown (non-authoritative listing) counts as occupied so name-generation
    // fails closed onto a fresh incremental name.
    return destinationEntryKind(target, directory, name) != PasteEntryKind::None;
}

bool FileManagementExplorerPanel::preparePasteDestination(const PasteSources& sources) {
    if (!sources.raw_items.isEmpty() && targetIndexForId(sources.source_target_id) < 0) {
        sak::showWarningLogged(this,
                               tr("Paste"),
                               tr("The copied items' source target is no longer available."));
        return false;
    }
    // The typed raw-write and raw-move confirmations run inside executePasteTo,
    // the single executor every paste path funnels through, so the drop paths
    // that never come through here cannot bypass them.
    QString identity_blocker;
    if (!validateCurrentTargetIdentity(&identity_blocker)) {
        sak::showWarningLogged(this, tr("Paste"), identity_blocker);
        return false;
    }
    return true;
}

// Flatten a paste payload into transfer items: host paths gain their directory flag
// from the host file system; raw items already carry theirs from the source listing.
QList<FileManagementExplorerPanel::PasteItem> FileManagementExplorerPanel::pasteItemsFor(
    const PasteSources& sources) {
    QList<PasteItem> items;
    items.reserve(sources.host_files.size() + sources.raw_items.size());
    for (const QString& path : sources.host_files) {
        const QFileInfo info(path);
        items.append({.path = path,
                      .size_bytes = static_cast<quint64>(std::max<qint64>(info.size(), 0)),
                      .directory = info.isDir()});
    }
    items.append(sources.raw_items);
    return items;
}

void FileManagementExplorerPanel::executePaste(const PasteSources& sources,
                                               const QString& destination_dir) {
    executePasteTo(currentTarget(), sources, destination_dir);
}

// Every irreversible half of a paste takes its own typed acknowledgement before
// any of it starts. Both prompts hang off executePasteTo, the one executor all
// paste paths funnel through, so a drop onto a raw sidebar target cannot land
// what the clipboard path has to prompt for.
bool FileManagementExplorerPanel::confirmPasteWrites(const FileManagementTarget& target,
                                                     const FileManagementTarget& source_target,
                                                     const PasteSources& sources,
                                                     const int requested) {
    if (!target.local_file_system && !confirmTypedRawImport(target, requested)) {
        return false;
    }
    // A move deletes its sources; on raw media that delete is permanent, so it
    // takes its own typed confirmation whatever the destination is.
    if (sources.move && !source_target.local_file_system &&
        !confirmTypedRawMove(source_target, requested)) {
        return false;
    }
    return true;
}

// Settle what happens once the paste worker finishes: the operation families and
// the status wording differ only by move-vs-copy, and only a move that really
// came off the clipboard may consume it.
FileManagementExplorerPanel::TransferCompletion
FileManagementExplorerPanel::pasteTransferCompletion(const PasteBatch& batch,
                                                     const PasteSources& sources,
                                                     const int requested,
                                                     const bool preflight_blocked) {
    TransferCompletion completion;
    completion.history_op = sources.move ? FileExplorerHistoryOperation::Move
                                         : FileExplorerHistoryOperation::Copy;
    completion.card_operation = sources.move ? FileExplorerOperationType::Move
                                             : FileExplorerOperationType::Copy;
    completion.source_target = batch.source_target;
    completion.destination_target = batch.target;
    completion.destination_dir = batch.destination_dir;
    completion.status_template = sources.move ? tr("Moved %1 of %2 item(s).")
                                              : tr("Pasted %1 of %2 item(s).");
    completion.requested_count = requested;
    completion.move = sources.move;
    completion.consume_clipboard = sources.move && sources.clipboard;
    completion.preflight_blocked = preflight_blocked;
    return completion;
}

void FileManagementExplorerPanel::executePasteTo(const FileManagementTarget& target,
                                                 const PasteSources& sources,
                                                 const QString& destination_dir) {
    const int source_index = targetIndexForId(sources.source_target_id);
    // A payload naming raw source items whose target is gone must never be re-read
    // as host paths: those paths would then be resolved against the local file
    // system. Fail closed here as well as in preparePasteDestination, because the
    // sidebar and view drop paths reach this executor directly.
    if (!sources.raw_items.isEmpty() && source_index < 0) {
        sak::showWarningLogged(this,
                               tr("Paste"),
                               tr("The copied items' source target is no longer available."));
        return;
    }
    // External URL drops have no tracked source target; they are host paths, so a
    // plain local target routes them (moves never originate from external payloads).
    const FileManagementTarget source_target =
        source_index >= 0 ? m_targets.at(source_index)
                          : FileManagementFileSystemBridge::localTarget(QString());
    const QList<PasteItem> items = pasteItemsFor(sources);
    const int requested = static_cast<int>(items.size());
    if (!confirmPasteWrites(target, source_target, sources, requested)) {
        return;
    }
    // Source and destination paths share a namespace only when both are host paths or
    // both live on the same raw target; only then can a folder contain its destination.
    const bool same_namespace = (source_target.local_file_system && target.local_file_system) ||
                                FileExplorerTargetId::fromTarget(source_target).value ==
                                    FileExplorerTargetId::fromTarget(target).value;
    const PasteBatch batch{.source_target = source_target,
                           .target = target,
                           .destination_dir = destination_dir,
                           .move = sources.move,
                           .multiple = items.size() > 1,
                           .same_namespace = same_namespace};
    QStringList blockers;
    PasteCollisionPolicy policy;
    const QList<FileExplorerTransferItem> resolved =
        resolveTransferItems(batch, items, &policy, &blockers);
    if (!blockers.isEmpty()) {
        sak::showWarningLogged(this, tr("Paste"), blockers.join(QStringLiteral("\n")));
    }
    if (resolved.isEmpty()) {
        return;
    }
    FileExplorerTransferRequest request;
    request.source_target = source_target;
    request.destination_target = target;
    request.items = resolved;
    request.move = sources.move;
    request.raw_read_cap = kExplorerHashMaxBytes;
    startTransferWorker(request,
                        pasteTransferCompletion(batch, sources, requested, !blockers.isEmpty()));
}

// Non-empty when this item is a folder that would land inside its own subtree.
// Only a shared namespace can express that containment; across namespaces the two
// path strings describe different volumes and cannot nest.
QString FileManagementExplorerPanel::pasteSelfContainmentBlocker(const PasteBatch& batch,
                                                                 const PasteItem& item,
                                                                 const QString& name) {
    if (!item.directory || !batch.same_namespace ||
        !pathContains(item.path, batch.destination_dir, batch.target.local_file_system)) {
        return {};
    }
    return (batch.move ? tr("Cannot move %1 into its own subfolder.")
                       : tr("Cannot paste %1 into its own subfolder."))
        .arg(name);
}

QList<FileExplorerTransferItem> FileManagementExplorerPanel::resolveTransferItems(
    const PasteBatch& batch,
    const QList<PasteItem>& items,
    PasteCollisionPolicy* policy,
    QStringList* blockers) {
    QList<FileExplorerTransferItem> resolved;
    for (const PasteItem& item : items) {
        const QString name = nameForPath(item.path, batch.source_target.local_file_system);
        const QString containment = pasteSelfContainmentBlocker(batch, item, name);
        if (!containment.isEmpty()) {
            blockers->append(containment);
            continue;
        }
        PasteDestination destination{
            .path = childPathFor(batch.destination_dir, name, batch.target.local_file_system),
            .replace = false};
        if (batch.same_namespace && destination.path == item.path) {
            if (batch.move) {
                // Files FilesystemOperations.MoveAsync: same path is a no-op success.
                continue;
            }
            // Same-folder copy-paste duplicates with the Files incremental
            // name; no conflict dialog fires (GetCollisions skips src==dest).
            const QString unique = uniqueChildName(batch.target, batch.destination_dir, name);
            if (unique.isEmpty()) {
                blockers->append(tr("Could not find a free name for %1 at the destination; "
                                    "the item was skipped.")
                                     .arg(name));
                continue;
            }
            destination.path =
                childPathFor(batch.destination_dir, unique, batch.target.local_file_system);
        } else if (!resolvePasteDestination(
                       batch.target, batch.multiple, policy, &destination, blockers)) {
            continue;
        }
        resolved.append({.source_path = item.path,
                         .destination_path = destination.path,
                         .size_bytes = item.size_bytes,
                         .directory = item.directory,
                         .replace_destination = destination.replace});
    }
    return resolved;
}

// The raw endpoints a transfer will mutate, deduplicated. Local endpoints and
// identity-less ones lock nothing, so they are not tracked.
QStringList FileManagementExplorerPanel::rawTargetIdsFor(
    const FileExplorerTransferRequest& request) {
    QStringList raw_ids;
    for (const FileManagementTarget& endpoint :
         {request.source_target, request.destination_target}) {
        const QString id = FileExplorerTargetId::fromTarget(endpoint).value;
        if (!endpoint.local_file_system && !id.isEmpty() && !raw_ids.contains(id)) {
            raw_ids.append(id);
        }
    }
    return raw_ids;
}

// Non-empty when one of those endpoints already has a worker on it. One mutation
// at a time per raw target: a second worker rewriting the same APFS/HFS+ metadata
// from independently read state corrupts the volume, so a run against a busy raw
// target is refused rather than raced.
QString FileManagementExplorerPanel::busyRawTargetBlocker(const QStringList& raw_ids) const {
    for (const QString& id : raw_ids) {
        if (m_busy_raw_target_ids.contains(id)) {
            return tr(
                "Another operation is still running on this raw target. "
                "Wait for it to finish and retry.");
        }
    }
    return {};
}

// Files posts the in-progress card before the worker starts, so the transfer is
// cancellable from the moment it exists.
FileExplorerStatusCardRequest FileManagementExplorerPanel::inProgressTransferCard(
    const FileExplorerTransferRequest& request, const TransferCompletion& completion) {
    FileExplorerStatusCardRequest card_request;
    card_request.result = FileExplorerReturnResult::InProgress;
    card_request.operation = completion.card_operation;
    for (const FileExplorerTransferItem& item : request.items) {
        card_request.source.append(item.source_path);
    }
    card_request.destination = {completion.destination_dir};
    card_request.items_count = request.items.size();
    card_request.can_provide_progress = true;
    card_request.cancelable = true;
    return card_request;
}

// Files two-card pattern: post the in-progress card with a cancel source,
// stream worker progress into it, and swap in the terminal card on finish.
void FileManagementExplorerPanel::startTransferWorker(const FileExplorerTransferRequest& request,
                                                      const TransferCompletion& completion) {
    const QStringList raw_ids = rawTargetIdsFor(request);
    const QString busy_blocker = busyRawTargetBlocker(raw_ids);
    if (!busy_blocker.isEmpty()) {
        sak::showWarningLogged(this,
                               completion.failure_title.isEmpty() ? tr("Paste")
                                                                  : completion.failure_title,
                               busy_blocker);
        return;
    }
    FileExplorerStatusCenterItem* card =
        m_status_center->addItem(inProgressTransferCard(request, completion));

    auto* worker = new FileExplorerTransferWorker(request, this);
    connect(worker,
            &FileExplorerTransferWorker::statusProgress,
            card,
            &FileExplorerStatusCenterItem::reportProgress);
    // requestStop only flips atomics, so the direct connection is safe and
    // reaches the running worker immediately.
    connect(card,
            &FileExplorerStatusCenterItem::cancelRequested,
            worker,
            &FileExplorerTransferWorker::requestStop,
            Qt::DirectConnection);
    // An internal abort raises no per-item blocker, so the failure signal is
    // captured here: without it a failed run would post a Success terminal card.
    auto failure = std::make_shared<QString>();
    connect(worker,
            &FileExplorerTransferWorker::failed,
            this,
            [failure](const int code, const QString& message) {
                *failure = message.trimmed().isEmpty()
                               ? tr("The transfer failed with error %1.").arg(code)
                               : message;
            });
    connect(worker,
            &QThread::finished,
            this,
            // completion arrives as a const reference, so capturing it by name would make the
            // closure member const too and the worker_failure assignment below would not
            // compile. An init-capture takes a plain, writable copy.
            [this, worker, card, completion = completion, failure, raw_ids]() mutable {
                m_active_io_workers.remove(worker);
                for (const QString& id : std::as_const(raw_ids)) {
                    m_busy_raw_target_ids.remove(id);
                }
                completion.worker_failure = *failure;
                finishTransferWorker(worker, card, completion);
                worker->deleteLater();
            });
    m_active_io_workers.insert(worker);
    for (const QString& id : std::as_const(raw_ids)) {
        m_busy_raw_target_ids.insert(id);
    }
    worker->start();
}

void FileManagementExplorerPanel::finishTransferWorker(FileExplorerTransferWorker* worker,
                                                       FileExplorerStatusCenterItem* card,
                                                       const TransferCompletion& completion) {
    const bool canceled = worker->stopRequested();
    const int written = static_cast<int>(worker->completedItems().size());
    // Files removes the in-progress card and posts a separate terminal card.
    m_status_center->removeItem(card);
    m_status_center->addItem(terminalTransferCard(worker, completion, canceled));
    applyTransferSideEffects(worker, completion, written);
    // Blockers recorded BEFORE a cancel are real failures; suppressing them on
    // cancel hid the actual cause. An internal worker abort reports itself here
    // too, since it raises no per-item blocker of its own.
    QStringList problems = worker->blockers();
    if (!completion.worker_failure.isEmpty()) {
        problems.append(completion.worker_failure);
    }
    if (!problems.isEmpty()) {
        sak::showWarningLogged(this,
                               completion.failure_title.isEmpty() ? tr("Paste")
                                                                  : completion.failure_title,
                               problems.join(QStringLiteral("\n")));
    }
    Q_EMIT statusMessage(
        canceled
            ? tr("Canceled after %1 of %2 item(s).").arg(written).arg(completion.requested_count)
            : completion.status_template.arg(written).arg(completion.requested_count),
        sak::kTimerStatusDefaultMs);
}

FileExplorerStatusCardRequest FileManagementExplorerPanel::terminalTransferCard(
    const FileExplorerTransferWorker* worker,
    const TransferCompletion& completion,
    const bool canceled) const {
    FileExplorerStatusCardRequest terminal;
    // Success means the whole requested batch landed: a GUI-thread preflight
    // rejection or an internal worker abort counts against it even when the
    // worker itself recorded no per-item blocker.
    const bool clean = worker->blockers().isEmpty() && !completion.preflight_blocked &&
                       completion.worker_failure.isEmpty();
    terminal.result = canceled ? FileExplorerReturnResult::Cancelled
                               : (clean ? FileExplorerReturnResult::Success
                                        : FileExplorerReturnResult::Failed);
    terminal.operation = completion.card_operation;
    for (const FileExplorerTransferItem& item : worker->completedItems()) {
        terminal.source.append(item.source_path);
    }
    if (terminal.source.isEmpty()) {
        terminal.source = {completion.destination_dir};
    }
    terminal.destination = {completion.destination_dir};
    terminal.items_count = completion.requested_count;
    return terminal;
}

// Journal a finished transfer batch into the undo history.
void FileManagementExplorerPanel::recordTransferHistory(const FileExplorerTransferWorker* worker,
                                                        const TransferCompletion& completion) {
    QList<FileExplorerHistoryItem> journal;
    for (const FileExplorerTransferItem& item : worker->completedItems()) {
        journal.append({.source_path = item.source_path,
                        .destination_path = item.destination_path,
                        .directory = item.directory});
    }
    recordHistory(completion.history_op,
                  completion.source_target,
                  completion.destination_target,
                  std::move(journal));
}

void FileManagementExplorerPanel::applyTransferSideEffects(FileExplorerTransferWorker* worker,
                                                           const TransferCompletion& completion,
                                                           const int written) {
    if (completion.record_history && written > 0) {
        recordTransferHistory(worker, completion);
    }
    if (!worker->lastFileSha256().isEmpty() && !worker->completedItems().isEmpty()) {
        m_last_hash_target_id = FileExplorerTargetId::fromTarget(completion.source_target).value;
        m_last_hash_name = nameForPath(worker->completedItems().last().source_path,
                                       completion.source_target.local_file_system);
        m_last_hash_sha256 = worker->lastFileSha256();
        m_last_hash_capped = worker->lastFileHashCapped();
        updateDetailsPane();
    }
    // Only a clipboard-originated move consumes the clipboard; a drag-drop
    // move must leave the user's clipboard untouched.
    if (completion.consume_clipboard && written > 0) {
        finishMovePaste();
    }
    if (written > 0) {
        loadDirectory(m_current_path);
        if (completion.refresh_other_pane) {
            refreshOtherPane();
        }
    }
    if (!worker->warnings().isEmpty()) {
        logMessage(worker->warnings().join(QLatin1Char('\n')));
    }
}

void FileManagementExplorerPanel::pasteClipboardIntoCurrentFolder() {
    const FileExplorerCommandState state =
        FileExplorerCommandRegistry::state(FileExplorerCommandId::Paste, commandContext());
    if (!state.enabled) {
        sak::showWarningLogged(this, tr("Paste"), state.blocker);
        return;
    }
    pasteClipboardTo(m_current_path);
}

void FileManagementExplorerPanel::pasteClipboardIntoSelection() {
    // Files PasteItemToSelectionAction: paste into the selected folder, or
    // the current folder when nothing is selected.
    const FileExplorerCommandState state = FileExplorerCommandRegistry::state(
        FileExplorerCommandId::PasteIntoSelection, commandContext());
    if (!state.enabled) {
        sak::showWarningLogged(this, tr("Paste"), state.blocker);
        return;
    }
    const FileExplorerSelection selection = currentSelection();
    const bool into_selected_folder = selection.hasSingleEntry() &&
                                      selection.entries.first().directory;
    pasteClipboardTo(into_selected_folder ? selection.entries.first().path : m_current_path);
}

void FileManagementExplorerPanel::pasteClipboardTo(const QString& destination_dir) {
    const PasteSources sources = collectPasteSources(QApplication::clipboard()->mimeData());
    if (sources.host_files.isEmpty() && sources.raw_items.isEmpty()) {
        sak::showWarningLogged(this, tr("Paste"), tr("Clipboard has no files to paste."));
        return;
    }
    if (sources.move && pasteSameTargetMove(currentTarget(), sources, destination_dir)) {
        return;
    }
    if (!preparePasteDestination(sources)) {
        return;
    }
    executePaste(sources, destination_dir);
}

bool FileManagementExplorerPanel::pasteSameTargetMove(const FileManagementTarget& target,
                                                      const PasteSources& sources,
                                                      const QString& destination_dir) {
    // Files MoveItemsFromClipboard on the same target is a real move:
    // renameEntry reparents through the certified writers on raw volumes and
    // QFile::rename locally, no data copy.
    if (sources.source_target_id != FileExplorerTargetId::fromTarget(target).value) {
        return false;
    }
    QString identity_blocker;
    if (!validateCurrentTargetIdentity(&identity_blocker)) {
        sak::showWarningLogged(this, tr("Paste"), identity_blocker);
        return true;
    }
    const QList<PasteItem> items = pasteItemsFor(sources);
    // A same-target move on raw media rewrites the volume through the certified
    // writer and removes the source entries; it never reaches executePasteTo, so
    // it takes the typed confirmation here.
    if (!target.local_file_system && !confirmTypedRawMove(target, static_cast<int>(items.size()))) {
        return true;
    }
    PasteCollisionPolicy policy;
    QStringList blockers;
    const PasteBatch batch{.source_target = target,
                           .target = target,
                           .destination_dir = destination_dir,
                           .move = true,
                           .multiple = items.size() > 1,
                           .same_namespace = true};
    const QList<FileExplorerTransferItem> resolved =
        resolveTransferItems(batch, items, &policy, &blockers);
    if (!blockers.isEmpty()) {
        sak::showWarningLogged(this, tr("Paste"), blockers.join(QStringLiteral("\n")));
    }
    if (resolved.isEmpty()) {
        return true;
    }
    FileExplorerTransferRequest request;
    request.source_target = target;
    request.destination_target = target;
    request.items = resolved;
    request.move = true;
    request.rename_within_target = true;
    request.raw_read_cap = kExplorerHashMaxBytes;
    TransferCompletion completion;
    completion.history_op = FileExplorerHistoryOperation::Move;
    completion.card_operation = FileExplorerOperationType::Move;
    completion.source_target = target;
    completion.destination_target = target;
    completion.destination_dir = destination_dir;
    completion.status_template = tr("Moved %1 of %2 item(s).");
    completion.requested_count = static_cast<int>(items.size());
    completion.move = true;
    completion.consume_clipboard = sources.clipboard;
    completion.preflight_blocked = !blockers.isEmpty();
    startTransferWorker(request, completion);
    return true;
}

// Move a Replace-resolved occupant ASIDE rather than destroying it, and report
// where it went (empty when it could not be staged, with the reason recorded).
// A confirmed replacement must never cost the destination AND leave the source
// where it was, so the occupant has to stay recoverable until the item that
// replaces it has actually landed.
QString FileManagementExplorerPanel::stageReplacedOccupant(const FileManagementTarget& target,
                                                           const QString& destination_dir,
                                                           const QString& name,
                                                           const QString& occupied_path,
                                                           QStringList* blockers) const {
    const QString aside = uniqueChildName(target, destination_dir, name);
    if (aside.isEmpty()) {
        blockers->append(tr("Could not stage a replacement for %1.").arg(name));
        return {};
    }
    const QString displaced = childPathFor(destination_dir, aside, target.local_file_system);
    const auto staged =
        FileManagementFileSystemBridge::renameEntry(target, occupied_path, displaced);
    if (!staged.ok) {
        blockers->append(staged.blockers);
        return {};
    }
    return displaced;
}

// Retire or restore the staged occupant once this item's own rename has been
// attempted: a landed move retires it, a failed one rolls it back into place.
void FileManagementExplorerPanel::settleDisplacedOccupant(const FileManagementTarget& target,
                                                          const QString& displaced,
                                                          const QString& occupied_path,
                                                          const bool renamed,
                                                          QStringList* blockers) {
    if (displaced.isEmpty()) {
        return;
    }
    if (!renamed) {
        // Roll the confirmed occupant back into place.
        const auto restored =
            FileManagementFileSystemBridge::renameEntry(target, displaced, occupied_path);
        if (!restored.ok) {
            blockers->append(restored.blockers);
        }
        return;
    }
    const auto removed = FileManagementFileSystemBridge::removeExistingEntry(
        target, displaced, kExplorerListMaxEntries);
    if (!removed.ok) {
        blockers->append(removed.blockers);
    }
}

int FileManagementExplorerPanel::moveEntriesWithinTarget(const FileManagementTarget& target,
                                                         const QList<PasteItem>& items,
                                                         const QString& destination_dir,
                                                         PasteCollisionPolicy* policy,
                                                         QStringList* blockers) {
    const bool multiple = items.size() > 1;
    int moved = 0;
    for (const PasteItem& item : items) {
        const QString name = nameForPath(item.path, target.local_file_system);
        PasteDestination destination{
            .path = childPathFor(destination_dir, name, target.local_file_system),
            .replace = false};
        if (destination.path == item.path) {
            // Files FilesystemOperations.MoveAsync: same path is a no-op success.
            ++moved;
            continue;
        }
        if (item.directory && pathContains(item.path, destination_dir, target.local_file_system)) {
            blockers->append(tr("Cannot move %1 into its own subfolder.").arg(name));
            continue;
        }
        if (!resolvePasteDestination(target, multiple, policy, &destination, blockers)) {
            continue;
        }
        // The occupant is staged aside immediately before this item's own rename,
        // so nothing is displaced for an item that never gets that far.
        QString displaced;
        if (destination.replace) {
            displaced =
                stageReplacedOccupant(target, destination_dir, name, destination.path, blockers);
            if (displaced.isEmpty()) {
                continue;
            }
        }
        // renameEntry reparents through the certified writers on raw volumes
        // (directories included) and QFile::rename locally, no data copy.
        const auto result =
            FileManagementFileSystemBridge::renameEntry(target, item.path, destination.path);
        settleDisplacedOccupant(target, displaced, destination.path, result.ok, blockers);
        if (!result.ok) {
            blockers->append(result.blockers);
            continue;
        }
        ++moved;
        m_last_mutation = result;
        m_transfer_journal.append({.source_path = item.path,
                                   .destination_path = destination.path,
                                   .directory = item.directory});
    }
    return moved;
}

void FileManagementExplorerPanel::recordHistory(const FileExplorerHistoryOperation operation,
                                                const FileManagementTarget& source_target,
                                                const FileManagementTarget& destination_target,
                                                QVector<FileExplorerHistoryItem> items) {
    // Files records history inside the operation implementations; while an
    // undo/redo replays through the same kernel this stays silent, and empty
    // batches (everything blocked) record nothing.
    if (m_history_busy || items.isEmpty()) {
        return;
    }
    FileExplorerStorageHistory history;
    history.operation = operation;
    history.source_target_id = FileExplorerTargetId::fromTarget(source_target).value;
    history.destination_target_id = FileExplorerTargetId::fromTarget(destination_target).value;
    history.items = std::move(items);
    m_storage_history.add(std::move(history));
}

void FileManagementExplorerPanel::undoLastOperation() {
    // Files UndoAction -> StorageHistoryHelpers.TryUndo: single-flight, and
    // an empty journal reports instead of failing.
    if (m_history_busy) {
        return;
    }
    const FileExplorerStorageHistory* history = m_storage_history.undoTarget();
    if (history == nullptr) {
        Q_EMIT statusMessage(tr("Nothing to undo."), sak::kTimerStatusMessageMs);
        return;
    }
    m_history_busy = true;
    const bool applied = executeHistory(*history, true);
    m_history_busy = false;
    if (applied) {
        m_storage_history.markUndone();
        loadDirectory(m_current_path);
        Q_EMIT statusMessage(tr("Undid the last file operation."), sak::kTimerStatusDefaultMs);
    }
}

void FileManagementExplorerPanel::redoLastOperation() {
    if (m_history_busy) {
        return;
    }
    const FileExplorerStorageHistory* history = m_storage_history.redoTarget();
    if (history == nullptr) {
        Q_EMIT statusMessage(tr("Nothing to redo."), sak::kTimerStatusMessageMs);
        return;
    }
    m_history_busy = true;
    const bool applied = executeHistory(*history, false);
    m_history_busy = false;
    if (applied) {
        m_storage_history.markRedone();
        loadDirectory(m_current_path);
        Q_EMIT statusMessage(tr("Redid the last file operation."), sak::kTimerStatusDefaultMs);
    }
}

// Applies a journal entry (undo replays the inverse; redo replays it again),
// cloning Files StorageHistoryOperations.Undo/Redo. Returns false only when
// the user cancels or a required target is gone, so the journal index stays
// put for a retry; partial failures surface blockers but still advance,
// matching Files (only Cancelled skips the index move).
bool FileManagementExplorerPanel::executeHistory(const FileExplorerStorageHistory& history,
                                                 const bool undo) {
    using Operation = FileExplorerHistoryOperation;
    switch (history.operation) {
    case Operation::Rename:
    case Operation::Move:
        return executeHistoryTransfer(history, undo);
    case Operation::Copy:
        // Undo of a copy deletes the created copies; redo copies again.
        return undo ? undoByDeletingCreatedEntries(history, false)
                    : executeHistoryTransfer(history, false);
    case Operation::CreateNew:
        return undo ? undoByDeletingCreatedEntries(history, true) : redoCreateEntries(history);
    }
    return false;
}

FileExplorerRedoCreateAction fileExplorerRedoCreateAction(const FileExplorerOccupant existing,
                                                          const bool item_is_dir,
                                                          const bool existing_empty_file) {
    if (existing == FileExplorerOccupant::Unknown) {
        return FileExplorerRedoCreateAction::Block;
    }
    if (existing == FileExplorerOccupant::Vacant) {
        return FileExplorerRedoCreateAction::Create;
    }
    const bool existing_dir = existing == FileExplorerOccupant::Directory;
    if (existing_dir != item_is_dir) {
        return FileExplorerRedoCreateAction::Block;
    }
    // Same kind already present: a directory create is idempotent, and an
    // empty file matches what the create produced; a populated file would be
    // clobbered by rewriting it empty, so block that.
    if (item_is_dir || existing_empty_file) {
        return FileExplorerRedoCreateAction::SkipIdentical;
    }
    return FileExplorerRedoCreateAction::Block;
}

FileExplorerHistoryDeleteVerdict fileExplorerHistoryDeleteVerdict(
    const FileExplorerOccupant existing,
    const bool item_is_dir,
    const qint64 observed,
    const qint64 expected) {
    if (existing == FileExplorerOccupant::Vacant) {
        return FileExplorerHistoryDeleteVerdict::Skip;
    }
    if (existing == FileExplorerOccupant::Unknown) {
        return FileExplorerHistoryDeleteVerdict::Block;
    }
    const bool existing_dir = existing == FileExplorerOccupant::Directory;
    if (existing_dir != item_is_dir || observed < 0 || expected < 0 || observed != expected) {
        return FileExplorerHistoryDeleteVerdict::Block;
    }
    return FileExplorerHistoryDeleteVerdict::Delete;
}

FileExplorerOccupant FileManagementExplorerPanel::occupantFor(const PasteEntryKind kind) {
    switch (kind) {
    case PasteEntryKind::None:
        return FileExplorerOccupant::Vacant;
    case PasteEntryKind::File:
        return FileExplorerOccupant::File;
    case PasteEntryKind::Directory:
        return FileExplorerOccupant::Directory;
    case PasteEntryKind::Unknown:
        break;
    }
    return FileExplorerOccupant::Unknown;
}

// Undo of Copy/CreateNew: delete what the operation produced, behind the
// Files forced-confirmation dialog. The destination target is resolved once
// so the confirmation can name paths and the raw-vs-recycle scope.
bool FileManagementExplorerPanel::undoByDeletingCreatedEntries(
    const FileExplorerStorageHistory& history, const bool undo_of_create) {
    const int target_index = targetIndexForId(history.destination_target_id);
    if (target_index < 0) {
        Q_EMIT statusMessage(tr("Undo target is no longer available."), sak::kTimerStatusMessageMs);
        return false;
    }
    const FileManagementTarget target = m_targets.at(target_index);
    if (!confirmHistoryDelete(history, target, undo_of_create)) {
        return false;
    }
    QStringList blockers;
    executeHistoryDelete(history, target, undo_of_create, &blockers);
    if (!blockers.isEmpty()) {
        sak::showWarningLogged(this, tr("Undo"), blockers.join(QStringLiteral("\n")));
    }
    // True means the undo RAN, not that every entry was removed. That is what the caller needs to
    // distinguish it from the two cases above, where nothing was attempted at all -- the target is
    // gone, or the operator declined the confirmation. A blocked entry is reported through
    // `blockers` and recorded on the posted card; it does not turn a performed undo into one that
    // never happened. executeHistoryDelete used to return a bool that was unconditionally true,
    // which made this read as a real outcome check when nothing was being checked.
    return true;
}

// Redo of a create recreates the entries (folders, or empty files), but never
// over data the user put at the path since - a populated file, a kind swap,
// or an unverifiable listing blocks that item.
bool FileManagementExplorerPanel::redoCreateEntries(const FileExplorerStorageHistory& history) {
    const int target_index = targetIndexForId(history.destination_target_id);
    if (target_index < 0) {
        Q_EMIT statusMessage(tr("Redo target is no longer available."), sak::kTimerStatusMessageMs);
        return false;
    }
    const FileManagementTarget target = m_targets.at(target_index);
    QStringList blockers;
    for (const FileExplorerHistoryItem& item : history.items) {
        std::ignore = redoCreateOneEntry(target, item, &blockers);
    }
    if (!blockers.isEmpty()) {
        sak::showWarningLogged(this, tr("Redo"), blockers.join(QStringLiteral("\n")));
    }
    return true;
}

// Recreate one created entry only when the path is vacant or already holds the
// identical empty entry; otherwise record a blocker and leave the occupant
// untouched.
bool FileManagementExplorerPanel::redoCreateOneEntry(const FileManagementTarget& target,
                                                     const FileExplorerHistoryItem& item,
                                                     QStringList* blockers) {
    const QString name = nameForPath(item.destination_path, target.local_file_system);
    const QString parent = parentPathForEntry(item.destination_path, target.local_file_system);
    const PasteEntryKind kind = destinationEntryKind(target, parent, name);
    const bool existing_empty = kind == PasteEntryKind::File &&
                                historyFileSize(target, item.destination_path) == 0;
    switch (fileExplorerRedoCreateAction(occupantFor(kind), item.directory, existing_empty)) {
    case FileExplorerRedoCreateAction::Block:
        blockers->append(tr("%1 is occupied by a different entry; it was not recreated.")
                             .arg(item.destination_path));
        return false;
    case FileExplorerRedoCreateAction::SkipIdentical:
        return true;
    case FileExplorerRedoCreateAction::Create:
        break;
    }
    const auto result = item.directory
                            ? FileManagementFileSystemBridge::createDirectory(target,
                                                                              item.destination_path)
                            : FileManagementFileSystemBridge::writeFile(target,
                                                                        item.destination_path,
                                                                        QByteArray());
    if (!result.ok) {
        blockers->append(result.blockers);
    }
    return result.ok;
}

// Rename/Move inverse: rename back on the same target, or reverse the
// transfer (copy back, then delete the landed side) across targets - the
// same certified kernel legs the forward operation used.
bool FileManagementExplorerPanel::resolveHistoryEndpoints(const FileExplorerStorageHistory& history,
                                                          const bool undo,
                                                          FileManagementTarget* from_target,
                                                          FileManagementTarget* to_target,
                                                          bool* same_target) {
    const QString from_id = undo ? history.destination_target_id : history.source_target_id;
    const QString to_id = undo ? history.source_target_id : history.destination_target_id;
    const int from_index = targetIndexForId(from_id);
    const int to_index = targetIndexForId(to_id);
    if (from_index < 0 || to_index < 0) {
        Q_EMIT statusMessage(
            tr("%1 target is no longer available.").arg(undo ? tr("Undo") : tr("Redo")),
            sak::kTimerStatusMessageMs);
        return false;
    }
    *from_target = m_targets.at(from_index);
    *to_target = m_targets.at(to_index);
    *same_target = from_id == to_id;
    return true;
}

bool FileManagementExplorerPanel::executeHistoryTransfer(const FileExplorerStorageHistory& history,
                                                         const bool undo) {
    FileManagementTarget from_target;
    FileManagementTarget to_target;
    bool same_target = false;
    if (!resolveHistoryEndpoints(history, undo, &from_target, &to_target, &same_target)) {
        return false;
    }
    const bool move = history.operation != FileExplorerHistoryOperation::Copy;
    QStringList blockers;
    QStringList from_paths;
    QString to_parent;
    for (const FileExplorerHistoryItem& item : history.items) {
        const QString from_path = undo ? item.destination_path : item.source_path;
        const QString to_path = undo ? item.source_path : item.destination_path;
        from_paths.append(from_path);
        if (to_parent.isEmpty()) {
            to_parent = historyParentPath(to_path);
        }
        applyHistoryTransferItem(HistoryTransferLeg{.from_target = from_target,
                                                    .to_target = to_target,
                                                    .move = move,
                                                    .same_target = same_target},
                                 item.directory,
                                 from_path,
                                 to_path,
                                 &blockers);
    }
    // Files: the replayed operation posts its own status-center card.
    postHistoryCard(move ? FileExplorerOperationType::Move : FileExplorerOperationType::Copy,
                    from_paths,
                    to_parent,
                    blockers.isEmpty());
    if (!blockers.isEmpty()) {
        sak::showWarningLogged(this,
                               undo ? tr("Undo") : tr("Redo"),
                               blockers.join(QStringLiteral("\n")));
    }
    return true;
}

QString FileManagementExplorerPanel::historyParentPath(const QString& path) {
    QString clean = path;
    clean.replace(QLatin1Char('\\'), QLatin1Char('/'));
    while (clean.endsWith(QLatin1Char('/'))) {
        clean.chop(1);
    }
    const qsizetype slash = clean.lastIndexOf(QLatin1Char('/'));
    return slash <= 0 ? QStringLiteral("/") : clean.left(slash);
}

void FileManagementExplorerPanel::postHistoryCard(const FileExplorerOperationType operation,
                                                  const QStringList& sources,
                                                  const QString& destination_dir,
                                                  const bool ok) {
    FileExplorerStatusCardRequest card;
    card.result = ok ? FileExplorerReturnResult::Success : FileExplorerReturnResult::Failed;
    card.operation = operation;
    card.source = sources;
    card.destination = {destination_dir};
    card.items_count = sources.size();
    m_status_center->addItem(card);
}

void FileManagementExplorerPanel::applyHistoryTransferItem(const HistoryTransferLeg& leg,
                                                           const bool directory,
                                                           const QString& from_path,
                                                           const QString& to_path,
                                                           QStringList* blockers) {
    // The replay destination must be PROVEN vacant: the historical path may hold
    // data the user created after the original operation, and the transfer legs
    // overwrite whatever is in the way.
    const QString to_name = nameForPath(to_path, leg.to_target.local_file_system);
    const QString to_parent = parentPathForEntry(to_path, leg.to_target.local_file_system);
    const PasteEntryKind occupant = destinationEntryKind(leg.to_target, to_parent, to_name);
    if (occupant == PasteEntryKind::Unknown) {
        blockers->append(
            tr("Could not verify whether %1 is free; the item was not replayed.").arg(to_path));
        return;
    }
    if (occupant != PasteEntryKind::None) {
        blockers->append(
            tr("%1 is occupied by another entry; the item was not replayed.").arg(to_path));
        return;
    }
    FileExplorerTransferEngine engine(leg.from_target, leg.to_target, kExplorerHashMaxBytes);
    const FileExplorerTransferItem item{.source_path = from_path,
                                        .destination_path = to_path,
                                        .size_bytes = 0,
                                        .directory = directory};
    if (leg.move && leg.same_target) {
        std::ignore = engine.renameWithinTarget(item);
    } else if (engine.transferEntry(item) && leg.move) {
        // Same two predicates the worker applies before deleting a moved source:
        // a capped raw read or a dropped entry means the copy did not land whole,
        // and deleting the source then loses the only intact data.
        if (engine.lastTransferComplete() && engine.lastTransferLandedAsRequested()) {
            std::ignore = engine.deleteMovedSource(item);
        } else {
            blockers->append(tr("%1 did not copy completely; the source was kept.").arg(from_path));
        }
    }
    blockers->append(engine.blockers());
    if (!engine.warnings().isEmpty()) {
        logMessage(engine.warnings().join(QLatin1Char('\n')));
    }
}

// File size (files) or immediate child count (directories) for an entry, or
// -1 when it is absent, the wrong kind, or the raw listing is not
// authoritative - a negative measure always fails the identity check closed.
qint64 FileManagementExplorerPanel::historyFileSize(const FileManagementTarget& target,
                                                    const QString& path) const {
    if (target.local_file_system) {
        const QFileInfo info(path);
        return (info.exists() && !info.isDir()) ? info.size() : -1;
    }
    const QString name = nameForPath(path, false);
    const QString parent = parentPathForEntry(path, false);
    const FileManagementListResult listing =
        FileManagementFileSystemBridge::listDirectory(target, parent, kExplorerListMaxEntries + 1);
    if (!listing.ok) {
        return -1;
    }
    for (const FileManagementEntry& entry : listing.entries) {
        if (QString::compare(entry.name, name, Qt::CaseInsensitive) == 0) {
            return entry.directory ? -1 : static_cast<qint64>(entry.size_bytes);
        }
    }
    return -1;
}

qint64 FileManagementExplorerPanel::historyDirChildCount(const FileManagementTarget& target,
                                                         const QString& path) const {
    if (target.local_file_system) {
        const QDir dir(path);
        if (!dir.exists()) {
            return -1;
        }
        return dir.entryList(QDir::NoDotAndDotDot | QDir::AllEntries | QDir::Hidden | QDir::System)
            .size();
    }
    const FileManagementListResult listing =
        FileManagementFileSystemBridge::listDirectory(target, path, kExplorerListMaxEntries + 1);
    if (!listing.ok || listing.entries.size() > kExplorerListMaxEntries) {
        return -1;
    }
    return listing.entries.size();
}

qint64 FileManagementExplorerPanel::historyEntryMeasure(const FileManagementTarget& target,
                                                        const QString& path,
                                                        const bool directory) const {
    return directory ? historyDirChildCount(target, path) : historyFileSize(target, path);
}

// Recycle (local) or permanently delete (raw) an entry whose identity has
// already been verified by historyDeleteOneEntry.
void FileManagementExplorerPanel::historyRemoveVerifiedEntry(const FileManagementTarget& target,
                                                             const FileExplorerHistoryItem& item,
                                                             QStringList* blockers) {
    if (target.local_file_system) {
        if (!sak::sendPathToRecycleBin(item.destination_path)) {
            blockers->append(
                tr("Could not move %1 to the Recycle Bin.").arg(item.destination_path));
        }
        return;
    }
    const auto result =
        item.directory
            ? FileManagementFileSystemBridge::deleteDirectoryTree(target, item.destination_path)
            : FileManagementFileSystemBridge::deleteFile(target, item.destination_path);
    if (!result.ok) {
        blockers->append(result.blockers);
    }
}

// Undo-deletes one entry a Copy or CreateNew produced, but only when its
// identity still holds: the on-disk entry must be the same kind AND match the
// captured identity - an empty entry for a create, or the source's size/child
// count for a copy. A same-named entry the user put there since is not the
// produced item and must not be recycled or permanently deleted. An
// already-vacant path needs nothing.
bool FileManagementExplorerPanel::historyDeleteOneEntry(const FileManagementTarget& target,
                                                        const FileManagementTarget& source_target,
                                                        const FileExplorerHistoryItem& item,
                                                        const bool undo_of_create,
                                                        QStringList* blockers) {
    const QString name = nameForPath(item.destination_path, target.local_file_system);
    const QString parent = parentPathForEntry(item.destination_path, target.local_file_system);
    const PasteEntryKind kind = destinationEntryKind(target, parent, name);
    const qint64 observed = historyEntryMeasure(target, item.destination_path, item.directory);
    const qint64 expected =
        undo_of_create ? 0 : historyEntryMeasure(source_target, item.source_path, item.directory);
    const auto verdict =
        fileExplorerHistoryDeleteVerdict(occupantFor(kind), item.directory, observed, expected);
    if (verdict == FileExplorerHistoryDeleteVerdict::Skip) {
        return false;
    }
    if (verdict == FileExplorerHistoryDeleteVerdict::Block) {
        blockers->append(
            tr("%1 no longer matches the entry the operation produced; it was not deleted.")
                .arg(item.destination_path));
        return false;
    }
    // historyRemoveVerifiedEntry records a blocker for every failure, so a run
    // that added none is the only proof the entry actually went away.
    const qsizetype before = blockers->size();
    historyRemoveVerifiedEntry(target, item, blockers);
    return blockers->size() == before;
}

// Deletes the entries a Copy or CreateNew produced: local paths recycle
// (Files undoes these with permanently:false), raw paths delete through the
// certified writers. The Copy source (when still mounted) is the identity
// oracle re-checked per item.
void FileManagementExplorerPanel::executeHistoryDelete(const FileExplorerStorageHistory& history,
                                                       const FileManagementTarget& target,
                                                       const bool undo_of_create,
                                                       QStringList* blockers) {
    FileManagementTarget source_target;
    const int source_index = targetIndexForId(history.source_target_id);
    if (source_index >= 0) {
        source_target = m_targets.at(source_index);
    }
    QStringList removed;
    for (const FileExplorerHistoryItem& item : history.items) {
        // Only what was really deleted goes on the card; a skipped or blocked
        // entry reported as removed is a false record of a destructive act.
        if (historyDeleteOneEntry(target, source_target, item, undo_of_create, blockers)) {
            removed.append(item.destination_path);
        }
    }
    // Files: the undo-delete posts a Delete-family card (Recycle on local
    // volumes, permanent Delete on raw targets).
    postHistoryCard(target.local_file_system ? FileExplorerOperationType::Recycle
                                             : FileExplorerOperationType::Delete,
                    removed,
                    QString(),
                    blockers->isEmpty());
}

// Enumerate the paths an undo will remove, capped so a huge batch does not
// build an unbounded dialog.
QString FileManagementExplorerPanel::historyDeletePathList(
    const QVector<FileExplorerHistoryItem>& items) {
    constexpr int kMaxShown = 15;
    QStringList lines;
    const int shown = std::min<int>(static_cast<int>(items.size()), kMaxShown);
    for (int index = 0; index < shown; ++index) {
        lines.append(items.at(index).destination_path);
    }
    if (items.size() > kMaxShown) {
        lines.append(tr("... and %n more", nullptr, static_cast<int>(items.size()) - kMaxShown));
    }
    return lines.join(QStringLiteral("\n"));
}

QString FileManagementExplorerPanel::historyDeleteScopeText(const bool permanent,
                                                            const bool has_directory) {
    QStringList notes;
    notes.append(permanent ? tr("These are on a raw target and will be deleted PERMANENTLY (not "
                                "sent to the Recycle Bin).")
                           : tr("These will be moved to the Recycle Bin."));
    if (has_directory) {
        notes.append(tr("Any folder is removed together with its entire contents."));
    }
    return notes.join(QStringLiteral("\n"));
}

// Files ShowConfirmationAsync before deleting what an undo removes, but with
// the real scope surfaced: the exact paths, raw-media permanence vs recycle,
// and whether directory trees are in play.
bool FileManagementExplorerPanel::confirmHistoryDelete(const FileExplorerStorageHistory& history,
                                                       const FileManagementTarget& target,
                                                       const bool undo_of_create) {
    const int count = static_cast<int>(history.items.size());
    bool has_directory = false;
    for (const FileExplorerHistoryItem& item : history.items) {
        has_directory = has_directory || item.directory;
    }
    const QString header =
        undo_of_create ? tr("Undoing this create will delete %n item(s):", nullptr, count)
                       : tr("Undoing this copy will delete %n copied item(s):", nullptr, count);
    const QString message =
        QStringLiteral("%1\n\n%2\n\n%3\n\n%4")
            .arg(header,
                 historyDeletePathList(history.items),
                 historyDeleteScopeText(!target.local_file_system, has_directory),
                 tr("Continue?"));
    // The message embeds foreign entry paths on an AutoText sink.
    const auto response = sak::showQuestionLogged(this,
                                                  tr("Undo"),
                                                  ui::asLiteralRichText(message),
                                                  QMessageBox::Yes | QMessageBox::No,
                                                  QMessageBox::No);
    return response == QMessageBox::Yes;
}

void FileManagementExplorerPanel::exportSelectedDirectoryOut(const FileManagementEntry& entry) {
    const QString destination_root =
        QFileDialog::getExistingDirectory(this, tr("Export Folder To"), QDir::homePath());
    if (destination_root.isEmpty()) {
        return;
    }
    // Folder export runs on the transfer worker with a status-center card;
    // the tree lands under <picked dir>/<folder name> as before.
    startExportWorker(currentTarget(),
                      {.source_path = entry.path,
                       .destination_path = QDir(destination_root).filePath(entry.name),
                       .size_bytes = entry.size_bytes,
                       .directory = true},
                      destination_root);
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

// Pre-flight one cross-pane entry (special-entry, subfolder, and self-copy
// guards) into a worker transfer item. Returns false (with a blocker) when the
// entry must be skipped.
bool FileManagementExplorerPanel::resolveCrossPaneCopyItem(const PasteBatch& batch,
                                                           const FileManagementEntry& entry,
                                                           FileExplorerTransferItem* item,
                                                           QStringList* blockers) {
    if (!entry.directory && !entry.regular_file) {
        blockers->append(tr("Skipped special entry %1.").arg(entry.name));
        return false;
    }
    const QString destination_path =
        childPathFor(batch.destination_dir, entry.name, batch.target.local_file_system);
    if (entry.directory && batch.same_namespace &&
        pathContains(entry.path, batch.destination_dir, batch.target.local_file_system)) {
        blockers->append(tr("Cannot copy %1 into its own subfolder.").arg(entry.name));
        return false;
    }
    // Copying an entry onto itself (both panes in the same folder) would
    // otherwise stream a file over its own source; skip it like Files.
    if (batch.same_namespace && destination_path == entry.path) {
        blockers->append(
            tr("Skipped %1: the source and destination are the same.").arg(entry.name));
        return false;
    }
    // The transfer legs write through whatever already holds the destination name
    // (a file is overwritten, a directory merged into). A collision is refused
    // here rather than resolved silently, and a destination that could not be
    // read is not proof that it is free.
    const PasteEntryKind occupant =
        destinationEntryKind(batch.target, batch.destination_dir, entry.name);
    if (occupant == PasteEntryKind::Unknown) {
        blockers->append(
            tr("Could not verify whether %1 already exists in the other pane; it was not copied.")
                .arg(entry.name));
        return false;
    }
    if (occupant != PasteEntryKind::None) {
        blockers->append(
            tr("Skipped %1: an item with that name is already in the other pane.").arg(entry.name));
        return false;
    }
    *item = {.source_path = entry.path,
             .destination_path = destination_path,
             .size_bytes = entry.size_bytes,
             .directory = entry.directory};
    return true;
}

QList<FileExplorerTransferItem> FileManagementExplorerPanel::resolveCrossPaneCopyItems(
    const PasteBatch& batch, QStringList* blockers) {
    QList<FileExplorerTransferItem> items;
    const FileExplorerSelection selection = currentSelection();
    for (const FileManagementEntry& entry : selection.entries) {
        FileExplorerTransferItem item;
        if (resolveCrossPaneCopyItem(batch, entry, &item, blockers)) {
            items.append(item);
        }
    }
    return items;
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
    const bool same_namespace = (source.local_file_system && destination.local_file_system) ||
                                FileExplorerTargetId::fromTarget(source).value ==
                                    FileExplorerTargetId::fromTarget(destination).value;
    const PasteBatch batch{.source_target = source,
                           .target = destination,
                           .destination_dir = destination_dir,
                           .move = false,
                           .multiple = false,
                           .same_namespace = same_namespace};
    QStringList blockers;
    const QList<FileExplorerTransferItem> items = resolveCrossPaneCopyItems(batch, &blockers);
    if (!blockers.isEmpty()) {
        sak::showWarningLogged(this, tr("Copy to Other Pane"), blockers.join(QLatin1Char('\n')));
    }
    if (items.isEmpty()) {
        Q_EMIT statusMessage(
            tr("Copied %1 of %2 item(s) to the other pane.").arg(0).arg(file_count),
            sak::kTimerStatusDefaultMs);
        return;
    }
    // The copy itself runs on the transfer worker with a status-center card
    // (running it synchronously froze the GUI for the whole transfer).
    FileExplorerTransferRequest request;
    request.source_target = source;
    request.destination_target = destination;
    request.items = items;
    request.raw_read_cap = kExplorerHashMaxBytes;
    TransferCompletion completion;
    completion.history_op = FileExplorerHistoryOperation::Copy;
    completion.card_operation = FileExplorerOperationType::Copy;
    completion.source_target = source;
    completion.destination_target = destination;
    completion.destination_dir = destination_dir;
    completion.status_template = tr("Copied %1 of %2 item(s) to the other pane.");
    completion.failure_title = tr("Copy to Other Pane");
    completion.requested_count = file_count;
    completion.refresh_other_pane = true;
    completion.preflight_blocked = !blockers.isEmpty();
    startTransferWorker(request, completion);
}

namespace {

// A listing this comparison cannot trust yields a non-empty reason. A reader
// that failed carries its blockers. A listing that filled the cap, or one the
// reader had to warn about, hides entries: every "only in this pane" line built
// from it would be an absence that is really an omission, so refuse rather than
// present it as a comparison. Empty means both listings are complete.
QString comparePanesBlocker(const FileManagementListResult& listing_a,
                            const FileManagementListResult& listing_b) {
    if (!listing_a.ok || !listing_b.ok) {
        return (listing_a.blockers + listing_b.blockers).join(QLatin1Char('\n'));
    }
    if (listing_a.entries.size() >= kExplorerListMaxEntries ||
        listing_b.entries.size() >= kExplorerListMaxEntries) {
        return FileManagementExplorerPanel::tr(
                   "A folder holds more than %1 entries, so the comparison would "
                   "be incomplete; it was not run.")
            .arg(kExplorerListMaxEntries);
    }
    const QStringList listing_warnings = listing_a.warnings + listing_b.warnings;
    if (!listing_warnings.isEmpty()) {
        return listing_warnings.join(QLatin1Char('\n'));
    }
    return QString();
}

}  // namespace

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
    const QString listing_blocker = comparePanesBlocker(listing_a, listing_b);
    if (!listing_blocker.isEmpty()) {
        sak::showWarningLogged(this, tr("Compare Panes"), listing_blocker);
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
    // the bytes of the currently selected file). The recorded target travels with the
    // result: the Evidence tab is headed by whatever target is current when it is read.
    m_last_mutation = result;
    m_last_mutation_target_id = FileExplorerTargetId::fromTarget(currentTarget()).value;
    m_last_preview_path.clear();
    m_last_preview_target_id.clear();
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
    const auto* selection_model = (m_pane != nullptr) ? m_pane->sharedSelectionModel() : nullptr;
    if ((selection_model == nullptr) || (m_pane == nullptr)) {
        return selection;
    }

    const QModelIndexList rows = selection_model->selectedRows();
    for (const QModelIndex& model_index : rows) {
        // Injected group-header rows carry no entry; skip them so bulk
        // selections (Ctrl+A under grouping) only collect real items.
        if (!m_pane->hasViewEntry(model_index.row())) {
            continue;
        }
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
    context.selection_has_tags = selectionHasTags(context.pane.selection);
    context.dual_pane_active = m_dual_pane_enabled;
    context.show_flatten_options = showFlattenOptionsEnabled();
    context.other_pane_target = otherPaneTarget();
    return context;
}

bool FileManagementExplorerPanel::selectionHasTags(const FileExplorerSelection& selection) const {
    const QString target_id = FileExplorerTargetId::fromTarget(currentTarget()).value;
    QSettings settings;
    return std::ranges::any_of(selection.entries, [&](const FileManagementEntry& entry) {
        return !FileExplorerTagStore::tagsFor(
                    settings, QString::fromLatin1(kTagStoreGroup), target_id, entry.path)
                    .isEmpty();
    });
}

void FileManagementExplorerPanel::applyCommandState(QPushButton* button,
                                                    const FileExplorerCommandId command,
                                                    const FileExplorerCommandContext& context) {
    if (button == nullptr) {
        return;
    }

    const FileExplorerCommandState state = FileExplorerCommandRegistry::state(command, context);
    button->setEnabled(state.enabled);
    button->setAccessibleName(state.command.accessible_name);
    // A blocker embeds the offending path/target name; wrap so the tooltip renders it literally.
    button->setToolTip(
        ui::asLiteralRichText(state.enabled ? state.command.status_text : state.blocker));
}

QAction* FileManagementExplorerPanel::addCommandMenuAction(
    QMenu* menu, const FileExplorerCommandId command, const FileExplorerCommandContext& context) {
    if (menu == nullptr) {
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
    const QString hint = state.enabled ? state.command.status_text : state.blocker;
    action->setToolTip(ui::asLiteralRichText(hint));
    // The status tip goes to the plain status bar, so it keeps the unwrapped text.
    action->setStatusTip(hint);
    if (!state.command.shortcut.trimmed().isEmpty()) {
        action->setShortcut(QKeySequence(state.command.shortcut));
        // Display-only hint: the panel's own QShortcut handles the key.
        // Leaving this at WindowShortcut makes the binding ambiguous with
        // that QShortcut and Qt then fires neither (seen on Ctrl+H).
        action->setShortcutContext(Qt::WidgetShortcut);
    }
    connect(action, &QAction::triggered, this, [this, command]() { executeCommand(command); });
    return action;
}

void FileManagementExplorerPanel::rebuildViewMenu(const FileExplorerCommandContext& context) {
    if (m_view_button == nullptr) {
        return;
    }

    auto* menu = m_view_button->menu();
    if (menu == nullptr) {
        menu = new QMenu(m_view_button);
        menu->setObjectName(QStringLiteral("fileExplorerViewMenu"));
        m_view_button->setMenu(menu);
    }
    menu->clear();

    auto* view_group = new QActionGroup(menu);
    view_group->setExclusive(true);
    for (const FileExplorerCommandId command : {FileExplorerCommandId::ViewDetails,
                                                FileExplorerCommandId::ViewList,
                                                FileExplorerCommandId::ViewGrid,
                                                FileExplorerCommandId::ViewCards,
                                                FileExplorerCommandId::ViewColumns,
                                                FileExplorerCommandId::ViewAdaptive}) {
        auto* action = addCommandMenuAction(menu, command, context);
        if (action == nullptr) {
            continue;
        }
        action->setCheckable(true);
        action->setChecked(modeForCommand(command) == m_pane_state.view.mode);
        view_group->addAction(action);
    }
    menu->addSeparator();

    appendItemSizeMenuRow(menu);

    menu->addSeparator();
    appendViewToggleActions(menu, context);
    menu->addSeparator();
    addCommandMenuAction(menu, FileExplorerCommandId::ToggleDualPane, context);
    auto* stack_action = menu->addAction(tr("Stack Panes Vertically"));
    stack_action->setObjectName(QStringLiteral("fileExplorerStackPanesAction"));
    stack_action->setCheckable(true);
    stack_action->setChecked((m_pane_splitter != nullptr) &&
                             m_pane_splitter->orientation() == Qt::Vertical);
    stack_action->setEnabled(m_dual_pane_enabled);
    stack_action->setToolTip(m_dual_pane_enabled
                                 ? tr("Switch between side-by-side and stacked panes.")
                                 : tr("Enable dual pane first."));
    connect(stack_action,
            &QAction::triggered,
            this,
            &FileManagementExplorerPanel::togglePaneOrientation);
    addCommandMenuAction(menu, FileExplorerCommandId::OpenInNewTab, context);
    addCommandMenuAction(menu, FileExplorerCommandId::DuplicateTab, context);
    addCommandMenuAction(menu, FileExplorerCommandId::ReopenClosedTab, context);

    const FileExplorerCommandState details_state =
        FileExplorerCommandRegistry::state(FileExplorerCommandId::ViewDetails, context);
    m_view_button->setEnabled(details_state.enabled);
    m_view_button->setToolTip(details_state.enabled ? tr("Change File Explorer view layout")
                                                    : details_state.blocker);
}

void FileManagementExplorerPanel::appendViewToggleActions(
    QMenu* menu, const FileExplorerCommandContext& context) {
    if (auto* hidden_action =
            addCommandMenuAction(menu, FileExplorerCommandId::ToggleHiddenItems, context)) {
        hidden_action->setCheckable(true);
        hidden_action->setChecked(m_pane_state.view.show_hidden);
    }
    if (auto* extension_action =
            addCommandMenuAction(menu, FileExplorerCommandId::ToggleFileExtensions, context)) {
        extension_action->setCheckable(true);
        extension_action->setChecked(m_pane_state.view.show_extensions);
    }
    // Files Settings > Folders "Show checkboxes when selecting items";
    // surfaced here until the C6 settings page lands.
    auto* checkboxes_action = menu->addAction(tr("Item Check Boxes"));
    checkboxes_action->setObjectName(QStringLiteral("fileExplorerItemCheckBoxesAction"));
    checkboxes_action->setCheckable(true);
    checkboxes_action->setChecked(showCheckboxesEnabled());
    connect(checkboxes_action, &QAction::triggered, this, [this](const bool checked) {
        setShowCheckboxes(checked);
    });
}

void FileManagementExplorerPanel::appendItemSizeMenuRow(QMenu* menu) {
    // Files Toolbar.xaml layout flyout: a tick-snapped slider per layout bound
    // to that layout's size kind (Details/List/Columns 1-5, Cards 1-4, Grid
    // 1-12). This row binds to the active layout's kind.
    const FileExplorerViewMode mode = m_pane_state.view.mode;
    auto* size_row = new QWidget(menu);
    size_row->setObjectName(QStringLiteral("fileExplorerItemSizeRow"));
    auto* size_layout = new QHBoxLayout(size_row);
    size_layout->setContentsMargins(
        ui::kMarginSmall, ui::kSpacingTight, ui::kMarginSmall, ui::kSpacingTight);
    size_layout->setSpacing(ui::kSpacingSmall);
    auto* size_label = new QLabel(tr("Item size"), size_row);
    size_label->setAccessibleName(tr("Explorer item size label"));
    auto* size_slider = new QSlider(Qt::Horizontal, size_row);
    size_slider->setObjectName(QStringLiteral("fileExplorerItemSizeSlider"));
    size_slider->setAccessibleName(tr("Explorer item size"));
    size_slider->setRange(fileExplorerSizeKindMin(mode), fileExplorerSizeKindMax(mode));
    size_slider->setSingleStep(1);
    size_slider->setPageStep(1);
    size_slider->setTickPosition(QSlider::TicksBelow);
    size_slider->setTickInterval(1);
    size_slider->setValue(fileExplorerSizeKind(m_pane_state.view.sizes, mode));
    size_label->setBuddy(size_slider);
    size_layout->addWidget(size_label);
    size_layout->addWidget(size_slider, 1);
    auto* size_action = new QWidgetAction(menu);
    size_action->setDefaultWidget(size_row);
    menu->addAction(size_action);
    connect(size_slider, &QSlider::valueChanged, this, [this](const int value) {
        setFileExplorerSizeKind(m_pane_state.view.sizes, m_pane_state.view.mode, value);
        applyViewSettings();
        saveViewSettings();
        Q_EMIT statusMessage(tr("Layout size set to %1 of %2")
                                 .arg(value)
                                 .arg(fileExplorerSizeKindMax(m_pane_state.view.mode)),
                             sak::kTimerStatusMessageMs);
    });
}

void FileManagementExplorerPanel::executeCommand(const FileExplorerCommandId command) {
    const FileExplorerCommandState state = FileExplorerCommandRegistry::state(command,
                                                                              commandContext());
    if (!state.enabled) {
        if (!state.blocker.isEmpty()) {
            if (m_status_label != nullptr) {
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
    default:
        return dispatchCopyPathCommand(command);
    }
}

bool FileManagementExplorerPanel::dispatchCopyPathCommand(const FileExplorerCommandId command) {
    switch (command) {
    case FileExplorerCommandId::CopyPath:
        QApplication::clipboard()->setText(m_current_path);
        Q_EMIT statusMessage(tr("Current path copied"), sak::kTimerStatusMessageMs);
        return true;
    case FileExplorerCommandId::CopyItemPath:
        QApplication::clipboard()->setText(currentSelection().paths().join(QStringLiteral("\n")));
        Q_EMIT statusMessage(tr("Item path copied"), sak::kTimerStatusMessageMs);
        return true;
    case FileExplorerCommandId::CopyItemPathQuoted: {
        // Files CopyItemPathWithQuotesAction: "path" per line.
        QStringList quoted;
        for (const QString& path : currentSelection().paths()) {
            quoted.append(QStringLiteral("\"%1\"").arg(path));
        }
        QApplication::clipboard()->setText(quoted.join(QStringLiteral("\n")));
        Q_EMIT statusMessage(tr("Quoted item path copied"), sak::kTimerStatusMessageMs);
        return true;
    }
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
        if (auto* selection_model = (m_pane != nullptr) ? m_pane->sharedSelectionModel()
                                                        : nullptr) {
            selection_model->clearSelection();
        }
        return true;
    case FileExplorerCommandId::InvertSelection:
        invertCurrentSelection();
        return true;
    case FileExplorerCommandId::ToggleSelect:
        toggleCurrentItemSelection();
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
    case FileExplorerCommandId::CutItems:
        copySelectionToClipboard(true);
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
    case FileExplorerCommandId::FocusOtherPane:
        if (m_dual_pane_enabled) {
            activatePane(1 - m_active_pane_index);
        }
        return true;
    case FileExplorerCommandId::OpenInTerminal:
        openTerminalHere();
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
    case FileExplorerCommandId::CreateEmptyFile:
        onCreateFileClicked();
        return true;
    case FileExplorerCommandId::WriteFile:
        onWriteFileClicked();
        return true;
    case FileExplorerCommandId::Paste:
        pasteClipboardIntoCurrentFolder();
        return true;
    case FileExplorerCommandId::PasteIntoSelection:
        pasteClipboardIntoSelection();
        return true;
    case FileExplorerCommandId::Rename:
        onRenameClicked();
        return true;
    case FileExplorerCommandId::Delete:
        onDeleteClicked();
        return true;
    case FileExplorerCommandId::DeletePermanently:
        deleteSelectionWithConfirmation(true);
        return true;
    default:
        return dispatchSelectionToolCommand(command);
    }
}

// C4 selection tools: folder-with-selection, tag clearing, and script editing.
bool FileManagementExplorerPanel::dispatchSelectionToolCommand(
    const FileExplorerCommandId command) {
    switch (command) {
    case FileExplorerCommandId::CreateFolderWithSelection:
        createFolderWithSelection();
        return true;
    case FileExplorerCommandId::RemoveTags:
        removeTagsFromSelection();
        return true;
    case FileExplorerCommandId::EditInNotepad:
        editSelectionInNotepad();
        return true;
    case FileExplorerCommandId::Undo:
        undoLastOperation();
        return true;
    case FileExplorerCommandId::Redo:
        redoLastOperation();
        return true;
    case FileExplorerCommandId::FlattenFolder:
        flattenSelectedFolder();
        return true;
    default:
        return dispatchArchiveCommand(command);
    }
}

bool FileManagementExplorerPanel::dispatchArchiveCommand(const FileExplorerCommandId command) {
    switch (command) {
    case FileExplorerCommandId::CompressIntoZip:
        compressSelectionToZip();
        return true;
    case FileExplorerCommandId::ExtractFiles:
        extractSelection(ExtractMode::Dialog);
        return true;
    case FileExplorerCommandId::ExtractHere:
        extractSelection(ExtractMode::Here);
        return true;
    case FileExplorerCommandId::ExtractHereSmart:
        extractSelection(ExtractMode::Smart);
        return true;
    case FileExplorerCommandId::ExtractToChildFolder:
        extractSelection(ExtractMode::ChildFolder);
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
    case FileExplorerCommandId::IncreaseSize:
        stepLayoutSize(1);
        return true;
    case FileExplorerCommandId::DecreaseSize:
        stepLayoutSize(-1);
        return true;
    default:
        return false;
    }
}

void FileManagementExplorerPanel::showSelectedItemProperties() {
    updateDetailsPane();
    const FileExplorerSelection selection = currentSelection();
    if (selection.isEmpty()) {
        // No selection: fall back to the info pane's Details tab.
        if (m_details_pane != nullptr) {
            m_details_pane->showDetailsTab();
        }
        return;
    }
    // Files OpenPropertiesAction (Alt+Enter): a real Properties window. The
    // name field doubles as a rename that commits on OK. The dialog is
    // non-modal, so capture the folder + target identity at open and refuse the
    // rename if the user navigated away before pressing OK (otherwise the
    // rename would land on a same-named item in whatever folder is now active).
    const QString captured_target_id = FileExplorerTargetId::fromTarget(currentTarget()).value;
    const QString captured_directory = m_current_path;
    auto* dialog = new FileExplorerPropertiesDialog(currentTarget(), selection.entries, this);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    connect(
        dialog, &QDialog::accepted, this, [this, dialog, captured_target_id, captured_directory]() {
            commitPropertiesRename(captured_target_id,
                                   captured_directory,
                                   dialog->originalName(),
                                   dialog->editedName());
        });
    dialog->show();
}

bool FileManagementExplorerPanel::propertiesRenameAllowed(const FileManagementTarget& target,
                                                          const QString& captured_target_id,
                                                          const QString& captured_directory,
                                                          const QString& edited) {
    if (FileExplorerTargetId::fromTarget(target).value != captured_target_id ||
        m_current_path != captured_directory) {
        sak::showWarningLogged(
            this,
            tr("Rename"),
            tr("The location changed while Properties was open; the rename was canceled."));
        return false;
    }
    if (!target.can_write_files || !isSafeChildName(edited)) {
        sak::showWarningLogged(this, tr("Rename"), tr("Enter a name without path separators."));
        return false;
    }
    if (target.local_file_system && isReservedWindowsName(edited)) {
        sak::showWarningLogged(this,
                               tr("Rename"),
                               tr("'%1' is a reserved name on Windows.").arg(edited));
        return false;
    }
    QString identity_blocker;
    if (!validateCurrentTargetIdentity(&identity_blocker)) {
        sak::showWarningLogged(this, tr("Rename"), identity_blocker);
        return false;
    }
    return true;
}

void FileManagementExplorerPanel::commitPropertiesRename(const QString& captured_target_id,
                                                         const QString& captured_directory,
                                                         const QString& original,
                                                         const QString& edited) {
    if (original.isEmpty() || edited.isEmpty() || edited == original) {
        return;
    }
    const FileManagementTarget target = currentTarget();
    if (!propertiesRenameAllowed(target, captured_target_id, captured_directory, edited)) {
        return;
    }
    // The original name is reconstructed from a listing of foreign, attacker-
    // authored bytes; a separator smuggled into it would make childPathFor
    // address an entry outside the captured directory. Screen it like the edited
    // name rather than trusting it because it came from our own model.
    if (!isSafeChildName(original)) {
        sak::showWarningLogged(this, tr("Rename"), tr("Enter a name without path separators."));
        return;
    }
    const QString source_path = childPathFor(m_current_path, original, target.local_file_system);
    const QString destination_path = childPathFor(m_current_path, edited, target.local_file_system);
    const auto result =
        FileManagementFileSystemBridge::renameEntry(target, source_path, destination_path);
    showMutationResult(tr("Rename"), result);
    if (result.ok) {
        recordHistory(FileExplorerHistoryOperation::Rename,
                      target,
                      target,
                      {FileExplorerHistoryItem{.source_path = source_path,
                                               .destination_path = destination_path,
                                               .directory = false}});
        loadDirectory(m_current_path);
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
    if (model == nullptr) {
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

void FileManagementExplorerPanel::installIconProvider(FileExplorerItemModel* model) {
    if (model == nullptr) {
        return;
    }
    // Row icons come from the shared explorer registry (palette-tinted, so they
    // stay legible in dark mode); the model itself stays GUI-free.
    model->setIconProvider([](const FileManagementEntry& entry) -> QVariant {
        static const QIcon kFolderIcon =
            FileExplorerIconRegistry::iconForKey(QStringLiteral("folder"));
        static const QIcon kFileIcon = FileExplorerIconRegistry::iconForKey(QStringLiteral("file"));
        static const QIcon kImageIcon =
            FileExplorerIconRegistry::iconForKey(QStringLiteral("image-file"));
        if (entry.directory) {
            return kFolderIcon;
        }
        static const QSet<QString> kImageSuffixes = {QStringLiteral("png"),
                                                     QStringLiteral("jpg"),
                                                     QStringLiteral("jpeg"),
                                                     QStringLiteral("gif"),
                                                     QStringLiteral("bmp"),
                                                     QStringLiteral("webp"),
                                                     QStringLiteral("svg"),
                                                     QStringLiteral("ico"),
                                                     QStringLiteral("heic")};
        const QString suffix = QFileInfo(entry.name).suffix().toLower();
        return kImageSuffixes.contains(suffix) ? kImageIcon : kFileIcon;
    });
}

void FileManagementExplorerPanel::clearCurrentTagFilter() const {
    if ((m_pane != nullptr) && (m_pane->sortFilterModel() != nullptr)) {
        m_pane->sortFilterModel()->clearTagFilter();
    }
}

void FileManagementExplorerPanel::applyTagFilter(const QString& tag) {
    if ((m_pane == nullptr) || (m_pane->sortFilterModel() == nullptr)) {
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
    // The prompt embeds an entry name read off a foreign volume, and the dialog's
    // label is an AutoText sink; render the name literally rather than as markup.
    const QString entered = QInputDialog::getText(
        this,
        tr("Edit Tags"),
        ui::asLiteralRichText(tr("Comma-separated tags for %1 (S.A.K. metadata only, never "
                                 "written to the file system):")
                                  .arg(selection.entries.first().name)),
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
    if (m_item_model != nullptr) {
        m_item_model->refreshTags();
    }
    rebuildTargetList();
    Q_EMIT statusMessage(tr("Tags updated for %1").arg(selection.entries.first().name),
                         sak::kTimerStatusMessageMs);
}

void FileManagementExplorerPanel::removeTagsFromSelection() {
    // Files RemoveTagsAction: clear every selected item's tags; app-level
    // metadata only, so it works on read-only raw targets too.
    const FileExplorerSelection selection = currentSelection();
    const QString target_id = FileExplorerTargetId::fromTarget(currentTarget()).value;
    if (selection.isEmpty() || target_id.isEmpty()) {
        return;
    }
    QSettings settings;
    for (const FileManagementEntry& entry : selection.entries) {
        FileExplorerTagStore::setTags(
            settings, QString::fromLatin1(kTagStoreGroup), target_id, entry.path, {});
    }
    updateDetailsPane();
    if (m_item_model != nullptr) {
        m_item_model->refreshTags();
    }
    rebuildTargetList();
    Q_EMIT statusMessage(tr("Removed tags from %1 item(s).").arg(selection.count()),
                         sak::kTimerStatusMessageMs);
}

// A create over an existing entry (createDirectory is mkpath, and writeFile
// overwrites) would journal a CreateNew whose undo then removes what was already
// there, so the destination must be PROVEN vacant - and a listing that could not
// be read authoritatively is not proof.
bool FileManagementExplorerPanel::createDestinationVacant(const FileManagementTarget& target,
                                                          const QString& name,
                                                          const QString& title) {
    const PasteEntryKind occupant = destinationEntryKind(target, m_current_path, name);
    if (occupant == PasteEntryKind::Unknown) {
        sak::showWarningLogged(
            this,
            title,
            tr("Could not verify whether %1 already exists here; nothing was created.").arg(name));
        return false;
    }
    if (occupant != PasteEntryKind::None) {
        sak::showWarningLogged(this, title, tr("An item named %1 already exists here.").arg(name));
        return false;
    }
    return true;
}

void FileManagementExplorerPanel::createFolderAndMoveSelection(
    const FileManagementTarget& target,
    const QString& name,
    const FileExplorerSelection& selection) {
    const QString folder_path = childPathFor(m_current_path, name, target.local_file_system);
    const auto created = FileManagementFileSystemBridge::createDirectory(target, folder_path);
    if (!created.ok) {
        sak::showWarningLogged(this,
                               tr("Create Folder"),
                               created.blockers.join(QStringLiteral("\n")));
        return;
    }
    recordHistory(FileExplorerHistoryOperation::CreateNew,
                  target,
                  target,
                  {FileExplorerHistoryItem{.source_path = QString(),
                                           .destination_path = folder_path,
                                           .directory = true}});
    QList<PasteItem> items;
    items.reserve(selection.entries.size());
    for (const FileManagementEntry& entry : selection.entries) {
        items.append(
            {.path = entry.path, .size_bytes = entry.size_bytes, .directory = entry.directory});
    }
    PasteCollisionPolicy policy;
    QStringList blockers;
    m_transfer_journal.clear();
    const int moved = moveEntriesWithinTarget(target, items, folder_path, &policy, &blockers);
    recordHistory(
        FileExplorerHistoryOperation::Move, target, target, std::move(m_transfer_journal));
    m_transfer_journal = {};
    loadDirectory(m_current_path);
    if (!blockers.isEmpty()) {
        sak::showWarningLogged(this, tr("Create Folder"), blockers.join(QStringLiteral("\n")));
    }
    Q_EMIT statusMessage(
        tr("Moved %1 of %2 item(s) into %3.").arg(moved).arg(items.size()).arg(name),
        sak::kTimerStatusDefaultMs);
}

void FileManagementExplorerPanel::createFolderWithSelection() {
    // Files CreateFolderWithSelectionAction: make a folder and move the
    // selection into it - the move runs through the same-target kernel, so
    // raw APFS/HFS selections reparent through the certified writers.
    const FileExplorerSelection selection = currentSelection();
    const FileManagementTarget target = currentTarget();
    if (selection.isEmpty() || !target.can_write_files) {
        return;
    }
    QString identity_blocker;
    if (!validateCurrentTargetIdentity(&identity_blocker)) {
        sak::showWarningLogged(this, tr("Create Folder"), identity_blocker);
        return;
    }
    bool accepted = false;
    const QString name = QInputDialog::getText(this,
                                               tr("Create Folder with Selection"),
                                               tr("Folder name:"),
                                               QLineEdit::Normal,
                                               tr("New folder"),
                                               &accepted);
    if (!accepted || !isSafeChildName(name)) {
        return;
    }
    if (!createDestinationVacant(target, name.trimmed(), tr("Create Folder"))) {
        return;
    }
    // The identity check above ran BEFORE a modal prompt; re-assert it so a target
    // swapped underneath the dialog cannot receive this create-and-move.
    if (!targetStillSelected(target, &identity_blocker)) {
        sak::showWarningLogged(this, tr("Create Folder"), identity_blocker);
        return;
    }
    createFolderAndMoveSelection(target, name, selection);
}

void FileManagementExplorerPanel::openTerminalHere() {
    // Files OpenTerminalAction: Windows Terminal when available, cmd.exe
    // fallback; one window per selected folder, else the current folder.
    // Local volumes only (registry-gated) - raw paths have no host cwd.
    const FileManagementTarget target = currentTarget();
    if (!target.local_file_system) {
        return;
    }
    QStringList directories;
    const FileExplorerSelection selection = currentSelection();
    for (const FileManagementEntry& entry : selection.entries) {
        if (entry.directory) {
            directories.append(entry.path);
        }
    }
    if (directories.isEmpty()) {
        directories.append(m_current_path);
    }
    // The cmd.exe leg is launched with the browsed directory as its working directory, so a
    // bare "cmd.exe" would be resolved by the CreateProcess search order against exactly that
    // (possibly attacker-writable) directory. Use the System32-qualified shell, and skip the
    // leg entirely when it cannot be resolved rather than fall back to the bare name.
    //
    // /D suppresses the Command Processor AutoRun registry command, which is writable by
    // the user (HKCU) and would otherwise run inside this elevated process.
    //
    // Windows Terminal is a Store app reached through an app-execution alias under
    // %LOCALAPPDATA%\Microsoft\WindowsApps; that alias path IS pinned here, because an
    // unqualified "wt.exe" is resolved by the CreateProcess search order, which prefers the
    // application directory and the process working directory over the system ones. A
    // missing alias skips the leg and falls through to the qualified cmd.exe one.
    const QString shell = sak::system32Path(QStringLiteral("cmd.exe"));
    const QString terminal =
        QDir(QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation))
            .filePath(QStringLiteral("Microsoft/WindowsApps/wt.exe"));
    const bool terminal_available = QFileInfo::exists(terminal);
    for (const QString& directory : directories) {
        const QString native = QDir::toNativeSeparators(directory);
        if ((!terminal_available ||
             !QProcess::startDetached(terminal, {QStringLiteral("-d"), native})) &&
            !shell.isEmpty()) {
            QProcess::startDetached(shell, {QStringLiteral("/D")}, native);
        }
        logMessage(tr("Open in Terminal: %1").arg(native));
    }
}

void FileManagementExplorerPanel::editSelectionInNotepad() {
    // Files EditInNotepadAction: notepad per selected script file (LOCAL only).
    const FileManagementTarget target = currentTarget();
    if (!target.local_file_system) {
        return;
    }
    const FileExplorerSelection selection = currentSelection();
    for (const FileManagementEntry& entry : selection.entries) {
        if (entry.regular_file) {
            // Absolute System32 notepad.exe; unresolvable -> open nothing rather than let
            // CreateProcess find a notepad.exe on the search path.
            (void)sak::startDetachedSystem32Tool(QStringLiteral("notepad.exe"),
                                                 {QDir::toNativeSeparators(entry.path)});
        }
    }
}

QString FileManagementExplorerPanel::selectionArchiveBaseName() const {
    const FileExplorerSelection selection = currentSelection();
    QStringList names;
    names.reserve(selection.entries.size());
    for (const FileManagementEntry& entry : selection.entries) {
        names.append(entry.name);
    }
    return FileExplorerArchiveService::archiveBaseName(
        names, nameForPath(m_current_path, currentTarget().local_file_system));
}

void FileManagementExplorerPanel::compressSelectionToZip() {
    // Files CompressIntoZipAction: "{selection name}.zip" beside the
    // selection, packed off the GUI thread with a Compress card. Raw targets
    // stage through the certified readers/writers inside the worker.
    const FileExplorerCommandState state = FileExplorerCommandRegistry::state(
        FileExplorerCommandId::CompressIntoZip, commandContext());
    if (!state.enabled) {
        sak::showWarningLogged(this, tr("Compress"), state.blocker);
        return;
    }
    QString identity_blocker;
    if (!validateCurrentTargetIdentity(&identity_blocker)) {
        sak::showWarningLogged(this, tr("Compress"), identity_blocker);
        return;
    }
    const FileManagementTarget target = currentTarget();
    // availableChildName yields nothing when no free name could be proven; packing
    // into an unverified name would overwrite whatever already holds it.
    const QString zip_name = availableChildName(
        target, m_current_path, selectionArchiveBaseName() + QStringLiteral(".zip"));
    if (zip_name.isEmpty()) {
        sak::showWarningLogged(this,
                               tr("Compress"),
                               tr("Could not find a free archive name in this folder."));
        return;
    }
    FileExplorerArchiveRequest request;
    request.compress = true;
    request.target = target;
    request.directory = m_current_path;
    request.zip_name = zip_name;
    request.raw_read_cap = kExplorerHashMaxBytes;
    const FileExplorerSelection selection = currentSelection();
    for (const FileManagementEntry& entry : selection.entries) {
        request.sources.append({.source_path = entry.path,
                                .destination_path = QString(),
                                .size_bytes = entry.size_bytes,
                                .directory = entry.directory});
    }
    startArchiveWorker(request, tr("Compress"));
}

void FileManagementExplorerPanel::extractSelection(const ExtractMode mode) {
    const FileExplorerCommandId command = mode == ExtractMode::Dialog
                                              ? FileExplorerCommandId::ExtractFiles
                                              : FileExplorerCommandId::ExtractHere;
    const FileExplorerCommandState state = FileExplorerCommandRegistry::state(command,
                                                                              commandContext());
    if (!state.enabled) {
        sak::showWarningLogged(this, tr("Extract"), state.blocker);
        return;
    }
    QString identity_blocker;
    if (!validateCurrentTargetIdentity(&identity_blocker)) {
        sak::showWarningLogged(this, tr("Extract"), identity_blocker);
        return;
    }
    const FileManagementTarget target = currentTarget();
    FileExplorerArchiveRequest request;
    request.target = target;
    request.directory = m_current_path;
    request.raw_read_cap = kExplorerHashMaxBytes;
    request.wrap_mode = mode == ExtractMode::ChildFolder
                            ? FileExplorerArchiveWrapMode::Wrap
                            : (mode == ExtractMode::Here ? FileExplorerArchiveWrapMode::None
                                                         : FileExplorerArchiveWrapMode::Smart);
    request.archives = extractArchiveItems(mode, target);
    if (request.archives.isEmpty()) {
        return;
    }
    // The Dialog leg opens a modal destination chooser per archive; re-assert the
    // identity the check above proved so a target swapped underneath it cannot
    // receive the extraction.
    if (!targetStillSelected(target, &identity_blocker)) {
        sak::showWarningLogged(this, tr("Extract"), identity_blocker);
        return;
    }
    startArchiveWorker(request, tr("Extract"));
}

// Selected archives with every GUI-thread decision resolved: the Dialog leg
// asks for its host destination up front (Files DecompressArchiveDialog; a
// canceled chooser skips that archive).
QList<FileExplorerArchiveExtractItem> FileManagementExplorerPanel::extractArchiveItems(
    const ExtractMode mode, const FileManagementTarget& target) {
    QList<FileExplorerArchiveExtractItem> items;
    const FileExplorerSelection selection = currentSelection();
    for (const FileManagementEntry& entry : selection.entries) {
        FileExplorerArchiveExtractItem item{.source_path = entry.path,
                                            .name = entry.name,
                                            .size_bytes = entry.size_bytes,
                                            .dialog_destination = QString()};
        if (mode == ExtractMode::Dialog) {
            const QString start = target.local_file_system ? QFileInfo(entry.path).absolutePath()
                                                           : QDir::homePath();
            item.dialog_destination =
                QFileDialog::getExistingDirectory(this, tr("Extract Files To"), start);
            if (item.dialog_destination.isEmpty()) {
                continue;
            }
        }
        items.append(item);
    }
    return items;
}

FileExplorerStatusCardRequest FileManagementExplorerPanel::archiveCardRequest(
    const FileExplorerArchiveRequest& request, const FileExplorerReturnResult result) {
    FileExplorerStatusCardRequest card;
    card.result = result;
    card.operation = request.compress ? FileExplorerOperationType::Compressed
                                      : FileExplorerOperationType::Extract;
    if (request.compress) {
        for (const FileExplorerTransferItem& item : request.sources) {
            card.source.append(item.source_path);
        }
        // Files AddCard_Compress points the destination at the archive itself.
        card.destination = {
            childPathFor(request.directory, request.zip_name, request.target.local_file_system)};
        card.items_count = static_cast<int>(request.sources.size());
    } else {
        for (const FileExplorerArchiveExtractItem& archive : request.archives) {
            card.source.append(archive.source_path);
        }
        card.destination = {request.directory};
        card.items_count = static_cast<int>(request.archives.size());
    }
    return card;
}

void FileManagementExplorerPanel::startArchiveWorker(const FileExplorerArchiveRequest& request,
                                                     const QString& failure_title) {
    FileExplorerStatusCenterItem* card =
        m_status_center->addItem(archiveCardRequest(request, FileExplorerReturnResult::InProgress));
    auto* worker = new FileExplorerArchiveWorker(request, this);
    connect(worker, &QThread::finished, this, [this, worker, card, failure_title]() {
        m_active_io_workers.remove(worker);
        finishArchiveWorker(worker, card, failure_title);
        worker->deleteLater();
    });
    m_active_io_workers.insert(worker);
    worker->start();
}

void FileManagementExplorerPanel::finishArchiveWorker(FileExplorerArchiveWorker* worker,
                                                      FileExplorerStatusCenterItem* card,
                                                      const QString& failure_title) {
    const FileExplorerArchiveRequest& request = worker->request();
    // Files removes the in-progress card and posts a separate terminal card.
    m_status_center->removeItem(card);
    m_status_center->addItem(archiveCardRequest(request,
                                                worker->blockers().isEmpty()
                                                    ? FileExplorerReturnResult::Success
                                                    : FileExplorerReturnResult::Failed));
    loadDirectory(m_current_path);
    // A warning here is an OMISSION (a skipped symlink or special entry), so the
    // run did not do what it was asked to. Surfacing it only in the log would let
    // the card and the status line report a partial archive as a clean one.
    const bool skipped = !worker->warnings().isEmpty();
    if (skipped) {
        logMessage(worker->warnings().join(QLatin1Char('\n')));
    }
    if (!worker->blockers().isEmpty()) {
        sak::showWarningLogged(this, failure_title, worker->blockers().join(QStringLiteral("\n")));
        if (request.compress) {
            return;
        }
    } else if (skipped) {
        sak::showWarningLogged(this, failure_title, worker->warnings().join(QStringLiteral("\n")));
    }
    QString status =
        request.compress
            ? tr("Created %1 (%2 file(s)).").arg(request.zip_name).arg(worker->zipEntryCount())
            : tr("Extracted %1 of %2 archive(s).")
                  .arg(worker->completedCount())
                  .arg(request.archives.size());
    if (skipped) {
        status += QLatin1Char(' ') + tr("Some entries were skipped.");
    }
    Q_EMIT statusMessage(status, sak::kTimerStatusDefaultMs);
}

void FileManagementExplorerPanel::togglePreviewPane() {
    if (m_details_pane != nullptr) {
        m_details_pane_enabled = !m_details_pane_enabled;
        m_details_pane->setVisible(m_details_pane_enabled && width() >= kDetailsTabsCollapseWidth);
    }
}

void FileManagementExplorerPanel::invertCurrentSelection() {
    auto* view = currentItemView();
    if ((view == nullptr) || (view->selectionModel() == nullptr) || (view->model() == nullptr)) {
        return;
    }
    auto* selection_model = view->selectionModel();
    for (int row = 0; row < view->model()->rowCount(); ++row) {
        const QModelIndex left = view->model()->index(row, 0);
        // Group-header rows are not selectable items; inverting must not
        // toggle them into the selection.
        if (!(view->model()->flags(left) & Qt::ItemIsSelectable)) {
            continue;
        }
        const QModelIndex right = view->model()->index(row, view->model()->columnCount() - 1);
        const QItemSelection row_selection(left, right);
        const bool selected = selection_model->isRowSelected(row, QModelIndex());
        selection_model->select(
            row_selection, selected ? QItemSelectionModel::Deselect : QItemSelectionModel::Select);
    }
}

void FileManagementExplorerPanel::toggleCurrentItemSelection() {
    // Files ToggleSelectAction (Ctrl+Space): toggle the focused row only.
    auto* view = currentItemView();
    if ((view == nullptr) || (view->selectionModel() == nullptr)) {
        return;
    }
    const QModelIndex current = view->currentIndex();
    if (!current.isValid()) {
        return;
    }
    view->selectionModel()->select(current,
                                   QItemSelectionModel::Toggle | QItemSelectionModel::Rows);
}

void FileManagementExplorerPanel::stepLayoutSize(const int direction) {
    // Files LayoutIncreaseSizeAction/LayoutDecreaseSizeAction: step the active
    // layout's size kind by one; at the bounds LayoutCycler.Cycle switches to
    // the adjacent layout in the Details-List-Cards-Grid-Columns ring, landing
    // on its minimum when growing and its maximum when shrinking, so repeated
    // zoom keeps moving in one direction across every layout.
    const FileExplorerViewMode mode = m_pane_state.view.mode;
    const int kind = fileExplorerSizeKind(m_pane_state.view.sizes, mode);
    const int stepped = kind + direction;
    if (stepped >= fileExplorerSizeKindMin(mode) && stepped <= fileExplorerSizeKindMax(mode)) {
        setFileExplorerSizeKind(m_pane_state.view.sizes, mode, stepped);
        applyViewSettings();
        saveViewSettings();
        return;
    }
    const bool forward = direction > 0;
    const FileExplorerViewMode next = fileExplorerAdjacentLayout(mode, forward);
    setFileExplorerSizeKind(m_pane_state.view.sizes,
                            next,
                            forward ? fileExplorerSizeKindMin(next)
                                    : fileExplorerSizeKindMax(next));
    setExplorerViewMode(next);
}

void FileManagementExplorerPanel::toggleHiddenItems() {
    m_pane_state.view.show_hidden = !m_pane_state.view.show_hidden;
    applyViewSettings();
    if ((m_pane != nullptr) && (m_item_model != nullptr) && !m_item_model->entries().isEmpty()) {
        if ((m_pane->sortFilterModel() != nullptr) && m_pane->sortFilterModel()->rowCount() == 0) {
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
    if ((m_pane != nullptr) && (m_item_model != nullptr) && !m_item_model->entries().isEmpty()) {
        if ((m_pane->sortFilterModel() != nullptr) && m_pane->sortFilterModel()->rowCount() == 0) {
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

bool FileManagementExplorerPanel::showCheckboxesEnabled() const {
    QSettings settings;
    settings.beginGroup(QString::fromLatin1(kExplorerSettingsGroup));
    // Files FoldersSettingsService.ShowCheckboxesWhenSelectingItems: default on.
    const bool enabled = settings.value(QString::fromLatin1(kShowCheckboxesKey), true).toBool();
    settings.endGroup();
    return enabled;
}

void FileManagementExplorerPanel::setShowCheckboxes(const bool enabled) {
    QSettings settings;
    settings.beginGroup(QString::fromLatin1(kExplorerSettingsGroup));
    settings.setValue(QString::fromLatin1(kShowCheckboxesKey), enabled);
    settings.endGroup();
    for (const FileExplorerPane* pane : {m_pane_a, m_pane_b}) {
        if ((pane != nullptr) && (pane->itemModel() != nullptr)) {
            pane->itemModel()->setCheckboxesVisible(enabled);
        }
    }
    Q_EMIT statusMessage(enabled ? tr("Item check boxes shown") : tr("Item check boxes hidden"),
                         sak::kTimerStatusMessageMs);
}

// Checkbox click for @p path: (de)select that row in the pane, matching the
// Files selection checkbox.
void FileManagementExplorerPanel::toggleSelectionForPath(FileExplorerPane* pane,
                                                         const QString& path,
                                                         const bool checked) {
    if ((pane == nullptr) || (pane->groupProxyModel() == nullptr) ||
        (pane->sharedSelectionModel() == nullptr)) {
        return;
    }
    auto* view_model = pane->groupProxyModel();
    for (int row = 0; row < view_model->rowCount(); ++row) {
        if (!pane->hasViewEntry(row) || pane->entryAtViewRow(row).path != path) {
            continue;
        }
        const QModelIndex left = view_model->index(row, 0);
        const QModelIndex right = view_model->index(row, view_model->columnCount() - 1);
        pane->sharedSelectionModel()->select(QItemSelection(left, right),
                                             checked ? QItemSelectionModel::Select
                                                     : QItemSelectionModel::Deselect);
        return;
    }
}

bool FileManagementExplorerPanel::showFlattenOptionsEnabled() const {
    QSettings settings;
    settings.beginGroup(QString::fromLatin1(kExplorerSettingsGroup));
    // Files GeneralSettingsService.ShowFlattenOptions: experimental, default off.
    const bool enabled = settings.value(QString::fromLatin1(kShowFlattenKey), false).toBool();
    settings.endGroup();
    return enabled;
}

void FileManagementExplorerPanel::flattenSelectedFolder() {
    // Files FlattenFolderAction: one selected folder, confirmation first;
    // moves every descendant file up into the selected folder, skipping name
    // collisions, then removes the emptied subfolders. Runs through the
    // bridge, so raw APFS/HFS folders flatten identically to local ones.
    const FileExplorerSelection selection = currentSelection();
    const FileManagementTarget target = currentTarget();
    if (!selection.hasSingleEntry() || !selection.entries.first().directory ||
        !target.can_write_files) {
        return;
    }
    const auto response = sak::showQuestionLogged(
        this,
        tr("Flatten folder"),
        tr("Flattening a folder will move all contents from its subfolders to the selected "
           "location. This operation is permanent and cannot be undone. Continue?"),
        QMessageBox::Yes | QMessageBox::No,
        QMessageBox::No);
    if (response != QMessageBox::Yes) {
        return;
    }
    QStringList blockers;
    const QString root = selection.entries.first().path;
    const int moved = flattenFolderTree(target, root, root, 0, &blockers);
    loadDirectory(m_current_path);
    if (!blockers.isEmpty()) {
        sak::showWarningLogged(this, tr("Flatten folder"), blockers.join(QStringLiteral("\n")));
    }
    Q_EMIT statusMessage(tr("Flattened %1 item(s) into %2")
                             .arg(moved)
                             .arg(nameForPath(root, target.local_file_system)),
                         sak::kTimerStatusDefaultMs);
}

// Files deletes only the EMPTIED subfolder; one still holding skipped
// collisions stays (the local delete is recursive, so an unguarded call
// would destroy the files Files preserves).
void FileManagementExplorerPanel::removeEmptiedSubfolder(const FileManagementTarget& target,
                                                         const QString& path,
                                                         QStringList* blockers) {
    const FileManagementListResult remainder =
        FileManagementFileSystemBridge::listDirectory(target, path, 1);
    if (!remainder.ok || !remainder.entries.isEmpty()) {
        return;
    }
    const auto removed = FileManagementFileSystemBridge::deleteDirectory(target, path);
    if (!removed.ok) {
        blockers->append(removed.blockers);
    }
}

// A junction or a directory symlink is reported as a directory by the listing.
// Recursing through one would move -- and then remove -- content that lives
// OUTSIDE the selected folder, so flatten stops at one instead of following it.
bool FileManagementExplorerPanel::entryLinksOutsideFolder(const FileManagementTarget& target,
                                                          const QString& path) {
    if (!target.local_file_system) {
        return false;
    }
    const QFileInfo link_info(path);
    return link_info.isSymLink() || link_info.isJunction();
}

// Move one leaf up into the flatten root, reporting whether it landed. A special
// entry is refused outright and a name collision is skipped the way Files skips
// it, so neither counts toward the moved tally.
bool FileManagementExplorerPanel::flattenEntryIntoRoot(const FileManagementTarget& target,
                                                       const QString& root,
                                                       const FileManagementEntry& entry,
                                                       QStringList* blockers) {
    if (!entry.regular_file) {
        blockers->append(tr("Skipped special entry %1.").arg(entry.name));
        return false;
    }
    if (destinationOccupied(target, root, entry.name)) {
        // Files skips a name collision; the skip is REPORTED so a folder that kept
        // its subfolders is never mistaken for a fully flattened one.
        blockers->append(
            tr("Skipped %1: an item with that name is already in the folder.").arg(entry.name));
        return false;
    }
    const auto result = FileManagementFileSystemBridge::renameEntry(
        target, entry.path, childPathFor(root, entry.name, target.local_file_system));
    if (!result.ok) {
        blockers->append(result.blockers);
        return false;
    }
    return true;
}

int FileManagementExplorerPanel::flattenFolderTree(const FileManagementTarget& target,
                                                   const QString& root,
                                                   const QString& current,
                                                   const int depth,
                                                   QStringList* blockers) {
    constexpr int kFlattenMaxDepth = 32;
    if (depth > kFlattenMaxDepth) {
        blockers->append(tr("Flatten stopped: folder tree deeper than %1.").arg(kFlattenMaxDepth));
        return 0;
    }
    constexpr int kFlattenListCap = 10'000;
    const FileManagementListResult listing =
        FileManagementFileSystemBridge::listDirectory(target, current, kFlattenListCap);
    if (!listing.ok) {
        blockers->append(listing.blockers);
        return 0;
    }
    // A listing that filled the cap hides the rest of the folder: those entries
    // would never be moved, yet the emptied-subfolder sweep and the "flattened N
    // item(s)" tally would report the folder as done.
    if (listing.entries.size() >= kFlattenListCap) {
        blockers->append(tr("%1 holds more than %2 entries; it was not flattened.")
                             .arg(current)
                             .arg(kFlattenListCap));
        return 0;
    }
    blockers->append(listing.warnings);
    int moved = 0;
    for (const FileManagementEntry& entry : listing.entries) {
        if (entry.directory) {
            if (entryLinksOutsideFolder(target, entry.path)) {
                blockers->append(tr("Skipped %1: it links outside the folder.").arg(entry.name));
                continue;
            }
            moved += flattenFolderTree(target, root, entry.path, depth + 1, blockers);
            removeEmptiedSubfolder(target, entry.path, blockers);
            continue;
        }
        if (current == root) {
            continue;  // already at the destination level
        }
        if (flattenEntryIntoRoot(target, root, entry, blockers)) {
            ++moved;
        }
    }
    return moved;
}

// Files FilterHeader (ModernShellPage.xaml): a "Filtering for" row with a
// Filename box that filters the loaded listing in place, toggled by
// Ctrl+Shift+F behind the persisted ShowFilterHeader setting.
void FileManagementExplorerPanel::buildFilterHeader(QVBoxLayout* layout) {
    m_filter_header = new QWidget(this);
    m_filter_header->setObjectName(QStringLiteral("fileExplorerFilterHeader"));
    auto* row = new QHBoxLayout(m_filter_header);
    row->setContentsMargins(
        ui::kMarginSmall, ui::kSpacingTight, ui::kMarginSmall, ui::kSpacingTight);
    row->setSpacing(ui::kSpacingSmall);
    auto* label = new QLabel(tr("Filtering for"), m_filter_header);
    label->setAccessibleName(tr("Folder filter label"));
    row->addWidget(label);
    m_filter_box = new QLineEdit(m_filter_header);
    m_filter_box->setObjectName(QStringLiteral("fileExplorerFilterBox"));
    m_filter_box->setAccessibleName(tr("Folder filter"));
    m_filter_box->setPlaceholderText(tr("Filename"));
    m_filter_box->setClearButtonEnabled(true);
    m_filter_box->setFixedWidth(kFilterBoxWidth);
    m_filter_box->installEventFilter(this);
    row->addWidget(m_filter_box);
    row->addStretch(1);
    layout->addWidget(m_filter_header, 0);
    m_filter_header->setVisible(showFilterHeaderEnabled());

    // Files FilesAndFoldersFilter: 250 ms debounce, then filter in place.
    m_filter_debounce = new QTimer(this);
    m_filter_debounce->setSingleShot(true);
    m_filter_debounce->setInterval(kFilterDebounceMs);
    connect(m_filter_debounce, &QTimer::timeout, this, [this]() {
        applyFilterHeaderText(m_filter_box ? m_filter_box->text() : QString());
    });
    connect(m_filter_box, &QLineEdit::textChanged, this, [this]() { m_filter_debounce->start(); });
}

bool FileManagementExplorerPanel::showFilterHeaderEnabled() const {
    QSettings settings;
    settings.beginGroup(QString::fromLatin1(kExplorerSettingsGroup));
    // Files GeneralSettingsService.ShowFilterHeader: default off.
    const bool enabled = settings.value(QString::fromLatin1(kShowFilterHeaderKey), false).toBool();
    settings.endGroup();
    return enabled;
}

void FileManagementExplorerPanel::toggleFilterHeader() {
    // Files ToggleFilterHeaderAction (Ctrl+Shift+F): flip the setting; the
    // shown box takes focus.
    const bool enabled = !showFilterHeaderEnabled();
    QSettings settings;
    settings.beginGroup(QString::fromLatin1(kExplorerSettingsGroup));
    settings.setValue(QString::fromLatin1(kShowFilterHeaderKey), enabled);
    settings.endGroup();
    if (m_filter_header != nullptr) {
        m_filter_header->setVisible(enabled);
    }
    if (enabled && (m_filter_box != nullptr)) {
        m_filter_box->setFocus(Qt::ShortcutFocusReason);
    }
}

void FileManagementExplorerPanel::applyFilterHeaderText(const QString& text) {
    if ((m_pane == nullptr) || (m_pane->sortFilterModel() == nullptr)) {
        return;
    }
    m_pane->sortFilterModel()->setNameFilter(text);
    const int visible_count = m_pane->sortFilterModel()->rowCount();
    const QString message = text.trimmed().isEmpty()
                                ? tr("Current folder filter cleared.")
                                : tr("Filter active: %1 item(s) visible.").arg(visible_count);
    if (m_status_label != nullptr) {
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
    if (m_search_worker == nullptr) {
        return;
    }
    AdvancedSearchWorker* worker = m_search_worker;
    m_search_worker = nullptr;
    // An interactive stop must not block the GUI thread (the old wait(5000)
    // froze the panel for up to 5 s per superseded search): detach the result
    // handlers so a stale search cannot repaint the suggestions, orphan the
    // worker so panel teardown cannot destroy a running thread, and let it
    // delete itself once its thread exits.
    disconnect(worker, nullptr, this, nullptr);
    worker->setParent(nullptr);
    worker->requestStop();
    connect(worker, &QThread::finished, worker, &QObject::deleteLater);
    if (!worker->isRunning()) {
        worker->deleteLater();
        return;
    }
    // A stopped worker keeps running until it notices; count the ones still
    // winding down so repeated debounced input cannot accumulate threads without
    // bound (startSearchWorker refuses to add another past the cap).
    ++m_orphaned_searches;
    connect(worker, &QThread::finished, this, [this]() { --m_orphaned_searches; });
}

// Shared worker setup for the inline search surfaces: suggestions cap at 10
// (Files FolderSearch MaxItemCount) and submits run to the full cap.
void FileManagementExplorerPanel::startSearchWorker(
    const QString& query,
    const int max_results,
    std::function<void(const QVector<SearchMatch>&)> on_matches,
    std::function<void()> on_finished) {
    stopExplorerSearch();
    constexpr int kMaxOrphanedSearches = 8;
    if (m_orphaned_searches >= kMaxOrphanedSearches) {
        Q_EMIT statusMessage(tr("Earlier searches are still stopping; try again in a moment."),
                             sak::kTimerStatusMessageMs);
        return;
    }
    SearchConfig config;
    config.file_system_target = currentTarget();
    config.use_file_system_target = true;
    config.root_path = m_current_path;
    config.pattern = query;
    config.search_file_metadata = true;  // matches file NAME and path as well as content
    config.max_results = max_results;
    config.max_file_size = kExplorerPreviewMaxBytes;
    m_search_worker = new AdvancedSearchWorker(config, this);
    // Results are interpreted against whatever target is current when they land,
    // so a run whose target is no longer selected is dropped instead of filling
    // the model (and the openable rows) with another volume's paths.
    const QString search_target_id =
        FileExplorerTargetId::fromTarget(config.file_system_target).value;
    connect(m_search_worker,
            &AdvancedSearchWorker::resultsReady,
            this,
            [this, search_target_id, handler = std::move(on_matches)](
                const QVector<SearchMatch>& matches) {
                if (FileExplorerTargetId::fromTarget(currentTarget()).value != search_target_id) {
                    return;
                }
                handler(matches);
            });
    if (on_finished) {
        connect(
            m_search_worker,
            &QThread::finished,
            this,
            [this, worker = m_search_worker, search_target_id, handler = std::move(on_finished)]() {
                if (FileExplorerTargetId::fromTarget(currentTarget()).value != search_target_id) {
                    return;
                }
                handler();
                // A run that could not cover everything it was asked to (an
                // unreadable file, a truncated listing, a hit result cap) makes
                // the count the handler just reported non-authoritative.
                if (worker->scanIncomplete()) {
                    logMessage(worker->incompleteReasons().join(QLatin1Char('\n')));
                    Q_EMIT statusMessage(
                        tr("The search could not cover everything it was asked to; the "
                           "result list is incomplete."),
                        sak::kTimerStatusDefaultMs);
                }
            });
    }
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

// Files PopulateOmnibarSuggestionsForSearchMode: an empty query lists the
// recent searches; typed text debounces 200 ms into a live capped search.
void FileManagementExplorerPanel::populateOmnibarSearch(const QString& text) {
    if ((m_omnibar == nullptr) || m_omnibar->mode() != FileExplorerOmnibarMode::Search) {
        return;
    }
    const QString clean = text.trimmed();
    if (clean.isEmpty()) {
        m_search_suggest_timer->stop();
        stopExplorerSearch();
        QListWidget* list = m_omnibar->suggestionList();
        list->clear();
        const QStringList history = searchHistory();
        for (int i = 0; i < history.size() && i < kOmnibarSearchSuggestCap; ++i) {
            auto* item = new QListWidgetItem(history.at(i), list);
            item->setToolTip(tr("Recent search"));
        }
        list->setCurrentRow(-1);
        m_omnibar->setSuggestionsVisible(list->count() > 0);
        return;
    }
    m_pending_search_suggest = clean;
    m_search_suggest_timer->start();
}

void FileManagementExplorerPanel::runSearchSuggestions(const QString& query) {
    if ((m_omnibar == nullptr) || m_omnibar->mode() != FileExplorerOmnibarMode::Search) {
        return;
    }
    QListWidget* list = m_omnibar->suggestionList();
    list->clear();
    m_omnibar->setSuggestionsVisible(true);
    const FileManagementTarget target = currentTarget();
    const bool local = target.local_file_system;
    startSearchWorker(
        query,
        kOmnibarSearchSuggestCap,
        [this, list, local](const QVector<SearchMatch>& matches) {
            if (!m_omnibar || m_omnibar->mode() != FileExplorerOmnibarMode::Search) {
                return;
            }
            for (const SearchMatch& match : matches) {
                if (!list->findItems(nameForPath(match.file_path, local), Qt::MatchExactly)
                         .isEmpty()) {
                    continue;
                }
                auto* item = new QListWidgetItem(nameForPath(match.file_path, local), list);
                item->setToolTip(ui::asLiteralRichText(match.file_path));
                item->setData(kSearchPathRole, match.file_path);
            }
        },
        {});
}

// Files SubmitSearch: the full search renders its results in the normal
// layout page (the listing shows the matches; navigating away restores).
void FileManagementExplorerPanel::submitExplorerSearch(const QString& query) {
    const FileManagementTarget target = currentTarget();
    if (FileExplorerTargetId::fromTarget(target).isEmpty()) {
        Q_EMIT statusMessage(tr("Select a File Explorer target first."),
                             sak::kTimerStatusMessageMs);
        return;
    }
    const QString clean = query.trimmed();
    if (clean.isEmpty()) {
        Q_EMIT statusMessage(tr("Type a search text first."), sak::kTimerStatusMessageMs);
        return;
    }
    rememberSearchQuery(clean);
    m_search_result_entries.clear();
    m_search_result_paths.clear();
    Q_EMIT statusMessage(tr("Searching for %1...").arg(clean), sak::kTimerStatusMessageMs);
    const bool local = target.local_file_system;
    startSearchWorker(
        clean,
        kExplorerSearchMaxResults,
        [this, local](const QVector<SearchMatch>& matches) {
            for (const SearchMatch& match : matches) {
                if (m_search_result_paths.contains(match.file_path)) {
                    continue;
                }
                m_search_result_paths.insert(match.file_path);
                FileManagementEntry entry;
                entry.name = nameForPath(match.file_path, local);
                entry.path = match.file_path;
                entry.regular_file = true;
                entry.type = tr("Search result");
                if (local) {
                    const QFileInfo info(match.file_path);
                    entry.size_bytes = static_cast<uint64_t>(std::max<qint64>(info.size(), 0));
                    entry.modified_time = info.lastModified();
                    entry.directory = info.isDir();
                    entry.regular_file = info.isFile();
                }
                m_search_result_entries.append(entry);
            }
        },
        [this, clean]() {
            if (m_item_model) {
                m_item_model->setEntries(m_search_result_entries);
            }
            if (m_pane) {
                if (m_search_result_entries.isEmpty()) {
                    m_pane->showEmptyState(tr("No results for %1.").arg(clean));
                } else {
                    m_pane->showReadyState();
                }
            }
            Q_EMIT statusMessage(tr("%n result(s) for %1.",
                                    nullptr,
                                    static_cast<int>(m_search_result_entries.size()))
                                     .arg(clean),
                                 sak::kTimerStatusDefaultMs);
            updateStatusCounts();
        });
}

// Files Omnibar_QuerySubmitted search branch: a chosen file suggestion opens
// its item; a recent-search row (or the raw text) submits the full search.
void FileManagementExplorerPanel::submitSearchSuggestion(QListWidgetItem* item,
                                                         const QString& typed) {
    QString query = typed.trimmed();
    if (item != nullptr) {
        const QVariant path = item->data(kSearchPathRole);
        if (!path.isNull()) {
            if (m_omnibar != nullptr) {
                m_omnibar->setMode(FileExplorerOmnibarMode::Path);
            }
            openSearchResult(path.toString(), false);
            return;
        }
        query = item->text();
    }
    if (m_omnibar != nullptr) {
        m_omnibar->setMode(FileExplorerOmnibarMode::Path);
    }
    submitExplorerSearch(query);
}

void FileManagementExplorerPanel::showCommandPalette() {
    // Files OpenCommandPaletteAction (Ctrl+Shift+P): switch the omnibar into
    // the inline palette mode; suggestions populate for the empty query.
    if (m_omnibar != nullptr) {
        m_omnibar->setMode(FileExplorerOmnibarMode::Palette);
    }
}

// Files PopulateOmnibarSuggestionsForCommandPaletteMode: a FLAT list of the
// executable commands whose text matches the typed needle (case-insensitive
// contains, unranked, uncapped); no group headers, no disabled rows.
void FileManagementExplorerPanel::populateOmnibarPalette(const QString& needle) {
    if (m_omnibar == nullptr) {
        return;
    }
    QListWidget* list = m_omnibar->suggestionList();
    list->clear();
    const FileExplorerCommandContext context = commandContext();
    for (const FileExplorerCommand& command : FileExplorerCommandRegistry::commands()) {
        const FileExplorerCommandState state = FileExplorerCommandRegistry::state(command.id,
                                                                                  context);
        if (!state.enabled) {
            continue;
        }
        const QString searchable =
            QStringList{command.text,
                        command.status_text,
                        FileExplorerCommandRegistry::commandIdName(command.id)}
                .join(QLatin1Char(' '));
        if (!needle.isEmpty() && !searchable.contains(needle, Qt::CaseInsensitive)) {
            continue;
        }
        QString label = command.text;
        if (!command.shortcut.trimmed().isEmpty()) {
            label += tr(" (%1)").arg(command.shortcut);
        }
        auto* item = new QListWidgetItem(label, list);
        item->setData(kCommandIdRole, QVariant::fromValue(command.id));
        item->setData(kCommandEnabledRole, true);
        item->setToolTip(command.status_text);
    }
    if (list->count() == 0) {
        // Files NoCommandsFound row.
        auto* item = new QListWidgetItem(tr("There are no commands containing %1").arg(needle),
                                         list);
        item->setFlags(Qt::NoItemFlags);
        item->setData(kCommandEnabledRole, false);
    } else {
        list->setCurrentRow(0);
    }
    m_omnibar->setSuggestionsVisible(true);
}

// Files Omnibar_QuerySubmitted palette branch: run the chosen suggestion,
// else the command whose label equals the typed text; no match reports.
void FileManagementExplorerPanel::executePaletteSuggestion(QListWidgetItem* item,
                                                           const QString& typed) {
    FileExplorerCommandId command_id{};
    bool found = false;
    if ((item != nullptr) && item->data(kCommandEnabledRole).toBool()) {
        command_id = item->data(kCommandIdRole).value<FileExplorerCommandId>();
        found = true;
    } else {
        const QString clean = typed.trimmed();
        for (const FileExplorerCommand& command : FileExplorerCommandRegistry::commands()) {
            if (command.text.compare(clean, Qt::CaseInsensitive) == 0) {
                command_id = command.id;
                found = true;
                break;
            }
        }
    }
    if (m_omnibar != nullptr) {
        m_omnibar->setMode(FileExplorerOmnibarMode::Path);
    }
    if (!found) {
        Q_EMIT statusMessage(tr("No command named %1.").arg(typed.trimmed()),
                             sak::kTimerStatusMessageMs);
        return;
    }
    executeCommand(command_id);
    if (auto* view = currentItemView()) {
        view->setFocus();
    }
}

namespace {

// Decode a preview image without letting the file itself dictate the allocation.
// The byte cap on the READ says nothing about the decoded size: a small crafted
// image can declare an enormous geometry, and QImage::loadFromData would expand
// it (and QPixmap::fromImage duplicate it) on the GUI thread. The declared
// geometry is therefore checked against a pixel budget BEFORE any decode, and a
// file that cannot state its geometry is refused rather than decoded blind.
QImage decodePreviewImage(const QByteArray& bytes) {
    QBuffer buffer;
    buffer.setData(bytes);
    if (!buffer.open(QIODevice::ReadOnly)) {
        return {};
    }
    QImageReader reader(&buffer);
    reader.setDecideFormatFromContent(true);
    const QSize size = reader.size();
    if (!size.isValid() || size.isEmpty()) {
        return {};
    }
    // 16 Mpx = 64 MiB as ARGB32, which a 480 px preview never needs to exceed.
    constexpr qint64 kMaxPreviewPixels = 16LL * 1024 * 1024;
    if (static_cast<qint64>(size.width()) * static_cast<qint64>(size.height()) >
        kMaxPreviewPixels) {
        return {};
    }
    return reader.read();
}

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

void FileManagementExplorerPanel::appendHashEvidence(const QString& current_target_id,
                                                     QStringList* evidence) const {
    if (m_last_hash_sha256.isEmpty()) {
        return;
    }
    evidence->append(QString());
    evidence->append(tr("Hashed file: %1").arg(m_last_hash_name));
    // Evidence carries its own provenance: without it, a hash taken on one
    // disk reads as evidence for whichever disk is selected now.
    if (m_last_hash_target_id != current_target_id) {
        evidence->append(
            tr("Recorded on target %1, NOT the target shown above.")
                .arg(m_last_hash_target_id.isEmpty() ? tr("(unknown)") : m_last_hash_target_id));
    }
    evidence->append(m_last_hash_capped
                         ? tr("SHA-256 (capped to first window): %1").arg(m_last_hash_sha256)
                         : tr("SHA-256: %1").arg(m_last_hash_sha256));
}

QStringList FileManagementExplorerPanel::buildDetailsEvidence(
    const FileManagementTarget& target) const {
    QStringList evidence;
    const QString current_target_id = FileExplorerTargetId::fromTarget(target).value;
    if (!target.root_path.isEmpty()) {
        evidence.append(tr("Target ID: %1").arg(target.id));
        evidence.append(tr("Source: %1").arg(target.source));
    }
    appendHashEvidence(current_target_id, &evidence);
    if (m_last_mutation.path.isEmpty()) {
        evidence.append(tr("No File Explorer mutation has run this session."));
        return evidence;
    }
    evidence.append(QString());
    if (m_last_mutation_target_id != current_target_id) {
        evidence.append(tr("The last operation below ran on target %1, NOT the target shown "
                           "above.")
                            .arg(m_last_mutation_target_id.isEmpty() ? tr("(unknown)")
                                                                     : m_last_mutation_target_id));
    }
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
    // The evidence tree is ordinary, user-writable storage: bound what a hostile
    // tree can make this walk allocate (every report is read whole) and how many
    // rows it can push into the Evidence tab.
    constexpr qint64 kMaxReportBytes = 4LL * 1024 * 1024;
    constexpr int kMaxReportsScanned = 2000;
    constexpr int kMaxReportsListed = 50;
    int scanned = 0;
    QDirIterator it(
        evidence_root, {QStringLiteral("*.json")}, QDir::Files, QDirIterator::Subdirectories);
    while (it.hasNext() && scanned < kMaxReportsScanned && matches.size() < kMaxReportsListed) {
        ++scanned;
        const QString report_path = it.next();
        QFile file(report_path);
        if (file.size() > kMaxReportBytes || !file.open(QIODevice::ReadOnly)) {
            continue;
        }
        const QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
        const QJsonArray targets = doc.object().value(QStringLiteral("targets")).toArray();
        const bool hit = std::ranges::any_of(targets, [&needle](const auto& value) {
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

    if (m_properties_text != nullptr) {
        m_properties_text->setPlainText(
            buildDetailsProperties(target, selection).join(QStringLiteral("\n")));
    }
    if (m_safety_text != nullptr) {
        m_safety_text->setPlainText(buildDetailsSafety(target).join(QStringLiteral("\n")));
    }
    if (m_evidence_text != nullptr) {
        m_evidence_text->setPlainText(buildDetailsEvidence(target).join(QStringLiteral("\n")));
    }
    updatePreviewPane(target, selection);
    if (m_status_label != nullptr) {
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

void FileManagementExplorerPanel::onPreviewReadFinished(
    QFutureWatcher<FileManagementReadResult>* watcher,
    quint64 preview_revision,
    const FileManagementEntry& entry,
    const QString& preview_target_id) {
    watcher->deleteLater();
    if (preview_revision != m_preview_revision) {
        return;  // a newer selection superseded this preview
    }
    const FileManagementReadResult read = watcher->result();
    if (!read.ok) {
        showPreviewHint(
            tr("Preview unavailable: %1").arg(read.blockers.join(QStringLiteral("; "))));
        return;
    }
    m_last_preview_path = entry.path;
    m_last_preview_target_id = preview_target_id;
    renderPreviewForEntry(entry, read.data, read.truncated);
    // A warning on an ok read means the bytes shown are not the whole
    // story; showing them with no notice would present a partial read as
    // the file's contents.
    if (!read.warnings.isEmpty()) {
        if (auto* caption = m_details_pane->previewCaption()) {
            caption->setText(
                caption->text() + QLatin1Char('\n') +
                tr("Preview warnings: %1").arg(read.warnings.join(QStringLiteral("; "))));
        }
    }
}

void FileManagementExplorerPanel::updatePreviewPane(const FileManagementTarget& target,
                                                    const FileExplorerSelection& selection) {
    if ((m_preview_text == nullptr) || (m_details_pane == nullptr)) {
        return;
    }
    // Any selection change supersedes an in-flight preview read.
    const quint64 preview_revision = ++m_preview_revision;
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
    // Two targets can hold the same path, so the cached path alone is not an
    // identity: it must be paired with the target the bytes were read from.
    const QString preview_target_id = FileExplorerTargetId::fromTarget(target).value;
    if (entry.path == m_last_preview_path && preview_target_id == m_last_preview_target_id) {
        return;
    }
    // The read runs off the GUI thread: a raw-target preview walks the whole
    // file-system metadata plus up to 1 MiB of extents, which froze the panel
    // on every selection change when it ran synchronously here.
    auto* watcher = new QFutureWatcher<FileManagementReadResult>(this);
    connect(watcher,
            &QFutureWatcher<FileManagementReadResult>::finished,
            this,
            [this, watcher, preview_revision, entry, preview_target_id]() {
                onPreviewReadFinished(watcher, preview_revision, entry, preview_target_id);
            });
    watcher->setFuture(QtConcurrent::run([target, path = entry.path]() {
        return FileManagementFileSystemBridge::readFile(target, path, kExplorerPreviewMaxBytes);
    }));
}

void FileManagementExplorerPanel::showPreviewHint(const QString& message) {
    m_last_preview_path.clear();
    m_last_preview_target_id.clear();
    m_details_pane->showImagePreview(false);
    if (auto* caption = m_details_pane->previewCaption()) {
        caption->clear();
    }
    if (m_preview_text != nullptr) {
        m_preview_text->setPlainText(message);
    }
}

void FileManagementExplorerPanel::renderPreviewForEntry(const FileManagementEntry& entry,
                                                        const QByteArray& bytes,
                                                        const bool reader_truncated) {
    const QImage image = decodePreviewImage(bytes);
    if (!image.isNull()) {
        showImagePreviewForEntry(entry, image);
        return;
    }
    // The reader can stop short without the byte count showing it (see
    // FileManagementReadResult::truncated), so its own verdict counts too.
    const bool capped = reader_truncated || bytes.size() >= kExplorerPreviewMaxBytes;
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
    applyCommandState(m_rename_button, FileExplorerCommandId::Rename, context);
    applyCommandState(m_delete_button, FileExplorerCommandId::Delete, context);
    if (m_command_bar != nullptr) {
        applyCommandState(m_command_bar->cutButton(), FileExplorerCommandId::CutItems, context);
        applyCommandState(m_command_bar->copyButton(), FileExplorerCommandId::CopyItems, context);
        applyCommandState(m_command_bar->pasteButton(), FileExplorerCommandId::Paste, context);
        applyCommandState(m_command_bar->propertiesButton(),
                          FileExplorerCommandId::Properties,
                          context);
    }
    rebuildViewMenu(context);
    updateStatusCounts();
    updateDetailsPane();
}

void FileManagementExplorerPanel::onRefreshMountedTargets() {
    const auto targets = FileManagementFileSystemBridge::mountedTargets();
    // An enumeration that produced nothing is a failure, not "the machine has no
    // volumes": publishing it would wipe the target list and still log success.
    if (targets.isEmpty()) {
        const QString message =
            tr("No mounted targets were found; the target list was left unchanged.");
        logMessage(message);
        Q_EMIT statusMessage(message, sak::kTimerStatusDefaultMs);
        return;
    }
    setTargets(targets);
    logMessage(tr("File explorer mounted targets refreshed"));
}

void FileManagementExplorerPanel::onScanDiskTargets() {
    Q_EMIT statusMessage(tr("Scanning disk and partition targets..."), 0);
    setEnabled(false);
    const auto inventory = StorageInventoryWorker::scanCurrentSystem();
    setEnabled(true);
    // An empty inventory, or one whose partition enumeration failed on any disk,
    // is not a picture of the machine. Replacing the target list from it would
    // silently drop targets and then log the scan as complete.
    if (inventory.isEmpty() || inventory.hasPartitionEnumerationFailure()) {
        const QString message =
            inventory.warnings.isEmpty()
                ? tr("The disk scan did not return a complete inventory; no targets were "
                     "replaced.")
                : inventory.warnings.join(QStringLiteral("\n"));
        logMessage(message);
        Q_EMIT statusMessage(tr("Disk target scan incomplete; the target list was left unchanged."),
                             sak::kTimerStatusDefaultMs);
        return;
    }
    if (!inventory.warnings.isEmpty()) {
        logMessage(inventory.warnings.join(QStringLiteral("\n")));
    }
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
    auto* path_row = new QHBoxLayout();
    path_row->addWidget(path, 1);
    path_row->addWidget(browse);
    layout->addRow(tr("Target path:"), path_row);

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
        // Fail closed on a row whose target index is missing or not an integer;
        // QVariant would otherwise hand back target 0.
        bool index_ok = false;
        const int target_index = item->data(kTargetIndexRole).toInt(&index_ok);
        return index_ok ? target_index : -1;
    }
    return -1;
}

void FileManagementExplorerPanel::onTargetChanged(int index) {
    if ((m_target_list == nullptr) || index < 0 || index >= m_target_list->count()) {
        m_current_target_index = -1;
        updateDetailsPane();
        updateActionButtons();
        return;
    }

    auto* item = m_target_list->item(index);
    if (item == nullptr) {
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
    tab.split = ((m_pane_splitter != nullptr) && m_pane_splitter->orientation() == Qt::Vertical)
                    ? FileExplorerPaneSplit::Horizontal
                    : FileExplorerPaneSplit::Vertical;
    return tab;
}

void FileManagementExplorerPanel::updateActiveTabLabel() {
    if ((m_tab_bar == nullptr) || m_active_tab < 0 || m_active_tab >= m_tab_bar->count()) {
        return;
    }
    const QString title = tabTitleForCurrentLocation();
    m_tab_bar->setTabText(m_active_tab, title);
    // A tooltip has no plain-text mode; the path comes off a foreign volume.
    m_tab_bar->setTabToolTip(m_active_tab, ui::asLiteralRichText(m_current_path));
    if (m_active_tab < m_tabs.size()) {
        m_tabs[m_active_tab].title = title;
    }
}

void FileManagementExplorerPanel::restoreTab(const FileExplorerTabState& tab) {
    m_restoring_tab = true;
    // Everything below addresses the tab's slots as "primary = physical pane A".
    // With pane B still active from the previous tab, m_pane_state IS pane B's
    // slot, so assigning tab.primary to it would load pane A's saved state into
    // pane B and then mix both panes' target/path state.
    if (m_active_pane_index == 1) {
        activatePane(0);
    }
    if (m_dual_pane_enabled && !tab.secondary_pane_enabled) {
        // The incoming tab is single-pane: collapse the split left over from the prior tab.
        m_dual_pane_enabled = false;
        if (m_pane_b != nullptr) {
            m_pane_b->hide();
        }
        highlightActivePane();
    }
    const int target_index = findTargetIndexById(tab.primary.location.target_id.value);
    m_current_target_index = target_index;
    m_pane_state = tab.primary;
    if ((m_target_list != nullptr) && target_index >= 0) {
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
    if (m_pane_splitter != nullptr) {
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

void FileManagementExplorerPanel::openPathInNewTab(const QString& path) {
    if (m_tab_bar == nullptr) {
        return;
    }
    if (m_active_tab >= 0 && m_active_tab < m_tabs.size()) {
        m_tabs[m_active_tab] = captureCurrentTab();
    }
    FileExplorerTabState fresh = captureCurrentTab();
    if (!path.isEmpty()) {
        // captureCurrentTab un-swaps the panes, so `primary` is physical pane A.
        // The path came from the ACTIVE pane and is only meaningful against THAT
        // pane's target; writing it into pane A's slot while pane B was active
        // would open a raw path such as "/folder" on the wrong disk.
        auto& active_slot = (fresh.secondary_pane_enabled && fresh.active_pane_index == 1)
                                ? fresh.secondary
                                : fresh.primary;
        active_slot.location.path = path;
        active_slot.back_stack.clear();
        active_slot.forward_stack.clear();
    }
    fresh.title = tr("New Tab");
    m_tabs.append(fresh);
    m_tab_bar->addTab(fresh.title);
    nameTabCloseButtons();
    m_tab_bar->setCurrentIndex(m_tab_bar->count() - 1);
}

void FileManagementExplorerPanel::openCurrentLocationInNewTab() {
    // Files OpenInNewTab: a selected folder opens its own tab; otherwise the
    // new tab clones the current location (NewTabAction).
    openPathInNewTab(selectedIsDirectory() ? selectedPath() : QString());
}

void FileManagementExplorerPanel::openSelectedFoldersInNewTabs() {
    // Files FileList_PreviewKeyDown Ctrl+Enter: every selected folder opens
    // in a new tab.
    const FileExplorerSelection selection = currentSelection();
    for (const FileManagementEntry& entry : selection.entries) {
        if (entry.directory) {
            openPathInNewTab(entry.path);
        }
    }
}

void FileManagementExplorerPanel::duplicateCurrentTab() {
    if ((m_tab_bar == nullptr) || m_active_tab < 0 || m_active_tab >= m_tabs.size()) {
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
    if ((m_tab_bar == nullptr) || m_tabs.size() <= 1 || index < 0 || index >= m_tabs.size()) {
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
    if ((m_tab_bar == nullptr) || m_closed_tabs.isEmpty()) {
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
    if (!m_tab_session_persistence || (m_tab_bar == nullptr)) {
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

namespace {

// The saved session is ordinary user-writable storage. A record this panel did
// not write is corruption, not a hint: an out-of-range active index or pane
// index would otherwise be coerced into "some tab" / "pane B", and an
// unbounded tab array would build one UI tab per record. So the whole session
// is refused unless every field is in range.
bool tabSessionRestorable(const FileExplorerTabSession& session) {
    constexpr qsizetype kMaxRestoredTabs = 64;
    const auto pane_index_valid = [](const FileExplorerTabState& tab) {
        return !tab.secondary_pane_enabled || tab.active_pane_index == 0 ||
               tab.active_pane_index == 1;
    };
    return session.tabs.size() <= kMaxRestoredTabs && session.active_index >= 0 &&
           session.active_index < session.tabs.size() &&
           std::ranges::all_of(session.tabs, pane_index_valid);
}

}  // namespace

void FileManagementExplorerPanel::restoreTabSession() {
    if (!m_tab_session_persistence || (m_tab_bar == nullptr)) {
        return;
    }
    QSettings settings;
    const FileExplorerTabSession session =
        FileExplorerSessionStore::load(settings, QString::fromLatin1(kTabSessionGroup));
    if (session.tabs.isEmpty()) {
        return;
    }
    if (!tabSessionRestorable(session)) {
        logMessage(tr("The saved File Explorer tab session is malformed; it was not restored."));
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
    // Validated above, so no clamp: a session that could not name its own active
    // tab was already refused whole.
    m_active_tab = session.active_index;
    m_tab_bar->setCurrentIndex(m_active_tab);
    restoreTab(m_tabs.at(m_active_tab));
}

void FileManagementExplorerPanel::ensureSecondPane() {
    if (m_pane_b != nullptr) {
        return;
    }
    m_pane_b = new FileExplorerPane(m_pane_splitter);
    m_pane_splitter->addWidget(m_pane_b);
    installTagProvider(m_pane_b->itemModel());
    installIconProvider(m_pane_b->itemModel());
    installIconProvider(m_pane_b->columnsPreviewModel());
    connectPaneSignals(m_pane_b, 1);
}

// Files ShellPanesPage Pane_GotFocus: the pane that just lost focus drops its
// selection. A command reads the ACTIVE pane's path but the selection came from
// whichever view the user last clicked, so a selection left behind in the other
// pane is a selection naming entries that do not live under the path the command
// is about to act on.
void FileManagementExplorerPanel::clearInactivePaneSelection(const int active_index) {
    FileExplorerPane* inactive = (active_index == 0) ? m_pane_b : m_pane_a;
    if (inactive == nullptr) {
        return;
    }
    if (auto* selection = inactive->sharedSelectionModel()) {
        selection->clearSelection();
    }
}

void FileManagementExplorerPanel::activatePane(int index) {
    if (index == m_active_pane_index || index < 0 || index > 1 || (m_pane_a == nullptr)) {
        return;
    }
    if (index == 1 && (m_pane_b == nullptr)) {
        return;
    }
    std::swap(m_pane_state, m_secondary_state);
    m_active_pane_index = index;
    m_pane = (index == 0) ? m_pane_a : m_pane_b;
    clearInactivePaneSelection(index);
    m_item_model = m_pane->itemModel();
    installTagProvider(m_item_model);
    installIconProvider(m_item_model);
    m_status_label = m_pane->statusLabel();
    m_current_path = m_pane_state.location.path;
    // The pane state carries its own target. Without re-pointing the current
    // target index here, the newly active pane's paths would be resolved against
    // the OTHER pane's target; an id that no longer resolves fails closed to no
    // target, which blocks every write until a listing re-establishes one.
    m_current_target_index = targetIndexForId(m_pane_state.location.target_id.value);
    if (m_path_edit != nullptr) {
        m_path_edit->setText(m_current_path);
    }
    applyViewSettings();  // re-apply the now-active pane's own view mode/size/toggles
    highlightActivePane();
    updateActionButtons();
}

void FileManagementExplorerPanel::togglePaneOrientation() {
    if (m_pane_splitter == nullptr) {
        return;
    }
    const bool stacked = m_pane_splitter->orientation() == Qt::Vertical;
    m_pane_splitter->setOrientation(stacked ? Qt::Horizontal : Qt::Vertical);
    Q_EMIT statusMessage(stacked ? tr("Panes arranged side by side")
                                 : tr("Panes stacked vertically"),
                         sak::kTimerStatusMessageMs);
}

void FileManagementExplorerPanel::highlightActivePane() {
    if (m_pane_a == nullptr) {
        return;
    }
    const QString border = ui::activePaneBorderStyleSheet();
    m_pane_a->setStyleSheet(m_dual_pane_enabled && m_active_pane_index == 0 ? border : QString());
    if (m_pane_b != nullptr) {
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
    // Files EditPath commit: Enter navigates and the breadcrumb replaces the
    // editable field (edit mode otherwise lingers until a focus change).
    if (m_omnibar != nullptr) {
        m_omnibar->setAddressEditMode(false);
    }
}

void FileManagementExplorerPanel::onBackClicked() {
    if (m_pane_state.goBack()) {
        loadDirectory(m_pane_state.location.path, false);
    }
}

void FileManagementExplorerPanel::showHistoryMenu(const bool back, const QPoint& global_pos) {
    const QVector<FileExplorerLocation>& stack = back ? m_pane_state.back_stack
                                                      : m_pane_state.forward_stack;
    if (stack.isEmpty()) {
        return;
    }
    QMenu menu(this);
    menu.setObjectName(back ? QStringLiteral("fileExplorerBackHistoryMenu")
                            : QStringLiteral("fileExplorerForwardHistoryMenu"));
    // Most recent entry first, mirroring the Files history flyouts.
    constexpr int kMaxHistoryEntries = 20;
    int steps = 0;
    for (auto it = stack.crbegin(); it != stack.crend() && steps < kMaxHistoryEntries; ++it) {
        ++steps;
        const QString label = it->path.trimmed().isEmpty() ? QStringLiteral("/") : it->path;
        const int jump = steps;
        menu.addAction(label, this, [this, jump, back]() { jumpHistory(jump, back); });
    }
    menu.exec(global_pos);
}

void FileManagementExplorerPanel::jumpHistory(const int steps, const bool back) {
    bool moved = false;
    for (int i = 0; i < steps; ++i) {
        const bool stepped = back ? m_pane_state.goBack() : m_pane_state.goForward();
        if (!stepped) {
            break;
        }
        moved = true;
    }
    if (moved) {
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
    // createDirectory (mkpath) succeeds on an already-existing folder, which
    // would journal a CreateNew whose undo then recycles the pre-existing
    // folder and its contents; refuse a create over an existing entry, and
    // refuse just as hard when the destination could not be read at all.
    const PasteEntryKind occupant = destinationEntryKind(target, m_current_path, name.trimmed());
    if (occupant == PasteEntryKind::Unknown) {
        sak::showWarningLogged(this,
                               tr("New Folder"),
                               tr("Could not verify whether %1 already exists here; nothing "
                                  "was created.")
                                   .arg(name.trimmed()));
        return;
    }
    if (occupant != PasteEntryKind::None) {
        sak::showWarningLogged(this,
                               tr("New Folder"),
                               tr("An item named %1 already exists here.").arg(name.trimmed()));
        return;
    }
    // The identity check above ran BEFORE a modal prompt; re-assert it so a
    // target swapped underneath the dialog cannot receive this write.
    if (!targetStillSelected(target, &identity_blocker)) {
        sak::showWarningLogged(this, tr("New Folder"), identity_blocker);
        return;
    }
    const auto result = FileManagementFileSystemBridge::createDirectory(target, path);
    showMutationResult(tr("New Folder"), result);
    if (result.ok) {
        recordHistory(FileExplorerHistoryOperation::CreateNew,
                      target,
                      target,
                      {FileExplorerHistoryItem{
                          .source_path = QString(), .destination_path = path, .directory = true}});
        loadDirectory(m_current_path);
    }
}

void FileManagementExplorerPanel::onCreateFileClicked() {
    // Files CreateFileAction (New > File): a name prompt, then an empty file
    // (UIFilesystemHelpers.CreateFileFromDialogResultTypeAsync with
    // AddItemDialogItemType.File and no template payload).
    const auto target = currentTarget();
    if (!target.can_write_files) {
        sak::showWarningLogged(this, tr("New File"), target.blockers.join(QStringLiteral("\n")));
        return;
    }
    QString identity_blocker;
    if (!validateCurrentTargetIdentity(&identity_blocker)) {
        sak::showWarningLogged(this, tr("New File"), identity_blocker);
        return;
    }
    bool ok = false;
    const QString name = QInputDialog::getText(
        this, tr("New File"), tr("File name:"), QLineEdit::Normal, QStringLiteral("New File"), &ok);
    if (!ok) {
        return;
    }
    const QString path = targetPathForName(name);
    if (path.isEmpty()) {
        sak::showWarningLogged(this,
                               tr("New File"),
                               tr("Enter a file name without path separators."));
        return;
    }
    // writeFile would overwrite an existing entry; creating must not - and an
    // unverifiable destination is not proof of vacancy either.
    const PasteEntryKind occupant = destinationEntryKind(target, m_current_path, name.trimmed());
    if (occupant == PasteEntryKind::Unknown) {
        sak::showWarningLogged(this,
                               tr("New File"),
                               tr("Could not verify whether %1 already exists here; nothing "
                                  "was created.")
                                   .arg(name.trimmed()));
        return;
    }
    if (occupant != PasteEntryKind::None) {
        sak::showWarningLogged(this,
                               tr("New File"),
                               tr("An item named %1 already exists here.").arg(name.trimmed()));
        return;
    }
    // The identity check above ran BEFORE a modal prompt; re-assert it so a
    // target swapped underneath the dialog cannot receive this write.
    if (!targetStillSelected(target, &identity_blocker)) {
        sak::showWarningLogged(this, tr("New File"), identity_blocker);
        return;
    }
    const auto result = FileManagementFileSystemBridge::writeFile(target, path, QByteArray());
    showMutationResult(tr("New File"), result);
    if (result.ok) {
        recordHistory(FileExplorerHistoryOperation::CreateNew,
                      target,
                      target,
                      {FileExplorerHistoryItem{
                          .source_path = QString(), .destination_path = path, .directory = false}});
        loadDirectory(m_current_path);
    }
}

// writeFileFromHostPath overwrites whatever already holds the destination name.
// A folder is never replaced, a destination that could not be read is not proof
// of vacancy, and replacing an existing file is a destructive act the user has to
// agree to - it is not journalled and cannot be undone.
bool FileManagementExplorerPanel::writeFileDestinationAllowed(const FileManagementTarget& target,
                                                              const QString& name) {
    const PasteEntryKind occupant = destinationEntryKind(target, m_current_path, name);
    if (occupant == PasteEntryKind::Unknown) {
        sak::showWarningLogged(
            this,
            tr("Write File"),
            tr("Could not verify whether %1 already exists here; nothing was written.").arg(name));
        return false;
    }
    if (occupant == PasteEntryKind::Directory) {
        sak::showWarningLogged(this,
                               tr("Write File"),
                               tr("A folder named %1 already exists here.").arg(name));
        return false;
    }
    if (occupant == PasteEntryKind::File) {
        return sak::showQuestionLogged(
                   this,
                   tr("Write File"),
                   ui::asLiteralRichText(
                       tr("%1 already exists here and will be overwritten. This cannot be "
                          "undone. Continue?")
                           .arg(name)),
                   QMessageBox::Yes | QMessageBox::No,
                   QMessageBox::No) == QMessageBox::Yes;
    }
    return true;
}

namespace {

// isFile() below FOLLOWS a reparse point, and the bridge reopens this same
// pathname later: screen the leaf first so an elevated read cannot be
// redirected onto a substituted symlink/junction target (or a UNC path).
// Non-empty is the reason the chosen source must not be read.
QString writeSourceFileBlocker(const QString& source_path) {
    if (sak::pathIsReparsePoint(source_path)) {
        return FileManagementExplorerPanel::tr(
                   "Refusing to read %1: it is a symbolic link, a junction, or a "
                   "network/device path.")
            .arg(source_path);
    }
    if (!QFileInfo(source_path).isFile()) {
        return FileManagementExplorerPanel::tr("Unable to read source file: %1").arg(source_path);
    }
    return QString();
}

}  // namespace

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
    const QString source_path = QFileDialog::getOpenFileName(this, tr("Select File to Write"));
    if (source_path.isEmpty()) {
        return;
    }
    const QString source_blocker = writeSourceFileBlocker(source_path);
    if (!source_blocker.isEmpty()) {
        sak::showWarningLogged(this, tr("Write File"), source_blocker);
        return;
    }
    const QFileInfo source_info(source_path);
    bool ok = false;
    const QString name = QInputDialog::getText(this,
                                               tr("Write File"),
                                               tr("Target file name:"),
                                               QLineEdit::Normal,
                                               source_info.fileName(),
                                               &ok);
    if (!ok) {
        return;
    }
    const QString target_path = targetPathForName(name);
    if (target_path.isEmpty()) {
        sak::showWarningLogged(this,
                               tr("Write File"),
                               tr("Enter a file name without path separators."));
        return;
    }
    if (!writeFileDestinationAllowed(target, name.trimmed())) {
        return;
    }
    // Modal dialogs ran since the identity check; re-assert it so a target
    // swapped underneath them cannot receive this write.
    if (!targetStillSelected(target, &identity_blocker)) {
        sak::showWarningLogged(this, tr("Write File"), identity_blocker);
        return;
    }
    const auto result =
        FileManagementFileSystemBridge::writeFileFromHostPath(target, target_path, source_path);
    showMutationResult(tr("Write File"), result);
    if (result.ok) {
        loadDirectory(m_current_path);
    }
}

void FileManagementExplorerPanel::onRenameClicked() {
    // Files RenameAction (F2): start an inline rename on the current item.
    // The commit round-trips through the model's renameRequested signal into
    // performInlineRename, on raw and mounted targets alike.
    const auto target = currentTarget();
    if (!target.can_write_files || selectedPath().isEmpty()) {
        return;
    }
    auto* view = currentItemView();
    if ((view == nullptr) || (view->selectionModel() == nullptr)) {
        return;
    }
    QModelIndex current = view->currentIndex();
    if (!current.isValid()) {
        const QModelIndexList rows = view->selectionModel()->selectedRows();
        if (rows.isEmpty()) {
            return;
        }
        current = rows.first();
    }
    const QModelIndex name_index = current.siblingAtColumn(FileExplorerItemModel::NameColumn);
    view->setCurrentIndex(name_index);
    view->edit(name_index);
}

namespace {

// Non-empty when the proposed rename target name is unacceptable: a name that
// carries path separators, or a reserved DOS device name. The reserved-name
// rule is only meaningful on local Windows paths -- raw APFS/HFS volumes
// legitimately allow such names.
QString renameNameBlocker(const QString& new_name, const FileManagementTarget& target) {
    if (!isSafeChildName(new_name)) {
        return FileManagementExplorerPanel::tr("Enter a name without path separators.");
    }
    if (target.local_file_system && isReservedWindowsName(new_name)) {
        return FileManagementExplorerPanel::tr("'%1' is a reserved name on Windows.").arg(new_name);
    }
    return QString();
}

}  // namespace

void FileManagementExplorerPanel::performInlineRename(const int row,
                                                      const QString& new_name,
                                                      const QString& expected_source_path) {
    const auto target = currentTarget();
    if (!target.can_write_files || (m_item_model == nullptr) || !m_item_model->hasEntry(row)) {
        return;
    }
    // The row is a position, not an identity: refuse when the entry sitting there
    // is no longer the one whose editor the user committed.
    if (!expected_source_path.isEmpty() &&
        m_item_model->entryAt(row).path != expected_source_path) {
        sak::showWarningLogged(
            this,
            tr("Rename"),
            tr("The listing changed before the rename was applied; nothing was renamed."));
        return;
    }
    QString identity_blocker;
    if (!validateCurrentTargetIdentity(&identity_blocker)) {
        sak::showWarningLogged(this, tr("Rename"), identity_blocker);
        return;
    }
    const QString name_blocker = renameNameBlocker(new_name, target);
    if (!name_blocker.isEmpty()) {
        sak::showWarningLogged(this, tr("Rename"), name_blocker);
        return;
    }
    const QString source_path = m_item_model->entryAt(row).path;
    const QString destination_path =
        childPathFor(parentPathForEntry(source_path, target.local_file_system),
                     new_name,
                     target.local_file_system);
    const bool is_directory = m_item_model->entryAt(row).directory;
    const auto result =
        FileManagementFileSystemBridge::renameEntry(target, source_path, destination_path);
    showMutationResult(tr("Rename"), result);
    if (result.ok) {
        recordHistory(FileExplorerHistoryOperation::Rename,
                      target,
                      target,
                      {FileExplorerHistoryItem{.source_path = source_path,
                                               .destination_path = destination_path,
                                               .directory = is_directory}});
        loadDirectory(m_current_path);
    }
}

void FileManagementExplorerPanel::onDeleteClicked() {
    deleteSelectionWithConfirmation(false);
}

QString FileManagementExplorerPanel::deleteConfirmationText(
    const bool recycle,
    const FileManagementTarget& target,
    const FileExplorerSelection& selection) const {
    const QString paths = selection.paths().join(QStringLiteral("\n"));
    if (recycle) {
        return tr("Move %1 item(s) to the Recycle Bin?\n\n%2")
            .arg(QString::number(selection.count()), paths);
    }
    return tr("Delete %1 item(s) from '%2'? This permanently removes data "
              "from the selected target.\n\n%3")
        .arg(QString::number(selection.count()), target.label, paths);
}

void FileManagementExplorerPanel::deleteSelectionWithConfirmation(const bool permanent) {
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
    // Files DeleteItemAction: plain Delete recycles on local volumes; Shift+Delete -
    // and every raw-target delete - is permanent. The confirmation always fires
    // (DeleteConfirmationPolicies.Always is the Files default).
    const bool recycle = !permanent && target.local_file_system;
    const auto response = sak::showQuestionLogged(
        this,
        tr("Delete Entry"),
        // Target label and entry paths are foreign, attacker-authored
        // text on an AutoText sink; a destructive confirmation must
        // render them literally, never as markup.
        ui::asLiteralRichText(deleteConfirmationText(recycle, target, selection)),
        QMessageBox::Yes | QMessageBox::No,
        QMessageBox::No);
    if (response != QMessageBox::Yes) {
        return;
    }
    // The confirmation is modal and the entry paths below are re-resolved by
    // name on whatever target is current; re-assert the identity the user was
    // shown before scheduling a permanent delete against it.
    if (!targetStillSelected(target, &identity_blocker)) {
        sak::showWarningLogged(this, tr("Delete Entry"), identity_blocker);
        return;
    }
    // Files posts the Delete-family card and runs the removal on the worker
    // (Recycle keeps the Delete headers per StatusCenterHelper.AddCard_Recycle).
    FileExplorerTransferRequest request;
    request.source_target = target;
    request.destination_target = target;
    request.kind = recycle ? FileExplorerTransferKind::Recycle : FileExplorerTransferKind::Delete;
    for (const FileManagementEntry& entry : selection.entries) {
        request.items.append({.source_path = entry.path,
                              .destination_path = QString(),
                              .size_bytes = entry.size_bytes,
                              .directory = entry.directory});
    }
    TransferCompletion completion;
    completion.card_operation = recycle ? FileExplorerOperationType::Recycle
                                        : FileExplorerOperationType::Delete;
    completion.source_target = target;
    completion.destination_target = target;
    completion.destination_dir = m_current_path;
    completion.status_template = tr("Deleted %1 of %2 item(s).");
    completion.failure_title = tr("Delete Entry");
    completion.requested_count = selection.count();
    completion.record_history = false;
    startTransferWorker(request, completion);
}

void FileManagementExplorerPanel::onTableContextMenuRequested(const QPoint& position) {
    auto* view = qobject_cast<QAbstractItemView*>(sender());
    if (view == nullptr) {
        view = currentItemView();
    }
    if (view == nullptr) {
        return;
    }

    if (const QModelIndex index = view->indexAt(position); index.isValid()) {
        selectRowInView(view, index.row());
    }

    const auto context = commandContext();
    QMenu menu(this);
    menu.setObjectName(QStringLiteral("fileExplorerTableContextMenu"));
    if (context.pane.selection.isEmpty()) {
        buildBackgroundContextMenu(&menu, context);
    } else {
        buildItemContextMenu(&menu, context);
    }
    menu.exec(view->viewport()->mapToGlobal(position));
}

// Item context menu in the Files ContentPageContextFlyoutFactory order for a
// selection: open group, clipboard group, file group, tags/tools, terminal,
// then the S.A.K.-specific raw-evidence commands behind the last separator.
// (Share/Send to/shortcuts/BitLocker/shell extensions are EXCLUDED by plan.)
void FileManagementExplorerPanel::buildItemContextMenu(QMenu* menu,
                                                       const FileExplorerCommandContext& context) {
    addCommandMenuAction(menu, FileExplorerCommandId::Open, context);
    addCommandMenuAction(menu, FileExplorerCommandId::OpenInNewTab, context);
    addCommandMenuAction(menu, FileExplorerCommandId::OpenInSecondPane, context);
    menu->addSeparator();
    addCommandMenuAction(menu, FileExplorerCommandId::CutItems, context);
    addCommandMenuAction(menu, FileExplorerCommandId::CopyItems, context);
    addCommandMenuAction(menu, FileExplorerCommandId::PasteIntoSelection, context);
    addCommandMenuAction(menu, FileExplorerCommandId::CopyItemPath, context);
    addCommandMenuAction(menu, FileExplorerCommandId::CopyItemPathQuoted, context);
    addCommandMenuAction(menu, FileExplorerCommandId::CreateFolderWithSelection, context);
    addCommandMenuAction(menu, FileExplorerCommandId::Rename, context);
    addCommandMenuAction(menu, FileExplorerCommandId::Delete, context);
    addCommandMenuAction(menu, FileExplorerCommandId::Properties, context);
    menu->addSeparator();
    addArchiveSubmenus(menu, context);
    // Files places Flatten folder after the compression group, only while
    // the ShowFlattenOptions setting is on (hidden otherwise, not disabled).
    if (context.show_flatten_options) {
        addCommandMenuAction(menu, FileExplorerCommandId::FlattenFolder, context);
    }
    addTagsSubmenu(menu, context);
    addCommandMenuAction(menu, FileExplorerCommandId::EditInNotepad, context);
    menu->addSeparator();
    addCommandMenuAction(menu, FileExplorerCommandId::OpenInTerminal, context);
    menu->addSeparator();
    addCommandMenuAction(menu, FileExplorerCommandId::Preview, context);
    addCommandMenuAction(menu, FileExplorerCommandId::Hash, context);
    addCommandMenuAction(menu, FileExplorerCommandId::CopyOut, context);
    addCommandMenuAction(menu, FileExplorerCommandId::CopyToOtherPane, context);
    addCommandMenuAction(menu, FileExplorerCommandId::ComparePanes, context);
}

// Background (empty-space) menu in the Files factory order: Layout / Sort by /
// Group by / Refresh, the New group, Paste, then terminal.
void FileManagementExplorerPanel::buildBackgroundContextMenu(
    QMenu* menu, const FileExplorerCommandContext& context) {
    auto* layout_menu = menu->addMenu(tr("Layout"));
    layout_menu->setObjectName(QStringLiteral("fileExplorerContextLayoutMenu"));
    auto* layout_group = new QActionGroup(layout_menu);
    layout_group->setExclusive(true);
    for (const FileExplorerCommandId command : {FileExplorerCommandId::ViewDetails,
                                                FileExplorerCommandId::ViewList,
                                                FileExplorerCommandId::ViewGrid,
                                                FileExplorerCommandId::ViewCards,
                                                FileExplorerCommandId::ViewColumns,
                                                FileExplorerCommandId::ViewAdaptive}) {
        if (auto* action = addCommandMenuAction(layout_menu, command, context)) {
            action->setCheckable(true);
            action->setChecked(modeForCommand(command) == m_pane_state.view.mode);
            layout_group->addAction(action);
        }
    }
    auto* sort_menu = menu->addMenu(tr("Sort by"));
    sort_menu->setObjectName(QStringLiteral("fileExplorerContextSortMenu"));
    rebuildSortMenu(sort_menu, false);
    auto* group_menu = menu->addMenu(tr("Group by"));
    group_menu->setObjectName(QStringLiteral("fileExplorerContextGroupMenu"));
    rebuildGroupMenu(group_menu);
    addCommandMenuAction(menu, FileExplorerCommandId::Refresh, context);
    menu->addSeparator();
    auto* new_menu = menu->addMenu(tr("New"));
    new_menu->setObjectName(QStringLiteral("fileExplorerContextNewMenu"));
    addCommandMenuAction(new_menu, FileExplorerCommandId::NewFolder, context);
    addCommandMenuAction(new_menu, FileExplorerCommandId::CreateEmptyFile, context);
    addCommandMenuAction(new_menu, FileExplorerCommandId::WriteFile, context);
    menu->addSeparator();
    addCommandMenuAction(menu, FileExplorerCommandId::Paste, context);
    addCommandMenuAction(menu, FileExplorerCommandId::SelectAll, context);
    addCommandMenuAction(menu, FileExplorerCommandId::InvertSelection, context);
    menu->addSeparator();
    addCommandMenuAction(menu, FileExplorerCommandId::OpenInTerminal, context);
}

// Files Compress/Extract submenus (ContentPageContextFlyoutFactory rows 43-44):
// Compress carries the dynamic "Create {name}.zip" label; Extract offers the
// dialog, smart, here, and subfolder legs in the factory order.
void FileManagementExplorerPanel::addArchiveSubmenus(QMenu* menu,
                                                     const FileExplorerCommandContext& context) {
    auto* compress_menu = menu->addMenu(tr("Compress"));
    compress_menu->setObjectName(QStringLiteral("fileExplorerCompressMenu"));
    if (auto* zip_action =
            addCommandMenuAction(compress_menu, FileExplorerCommandId::CompressIntoZip, context);
        (zip_action != nullptr) && zip_action->isEnabled()) {
        zip_action->setText(tr("Create %1.zip").arg(selectionArchiveBaseName()));
    }
    auto* extract_menu = menu->addMenu(tr("Extract"));
    extract_menu->setObjectName(QStringLiteral("fileExplorerExtractMenu"));
    addCommandMenuAction(extract_menu, FileExplorerCommandId::ExtractFiles, context);
    addCommandMenuAction(extract_menu, FileExplorerCommandId::ExtractHereSmart, context);
    addCommandMenuAction(extract_menu, FileExplorerCommandId::ExtractHere, context);
    auto* child_action =
        addCommandMenuAction(extract_menu, FileExplorerCommandId::ExtractToChildFolder, context);
    const FileExplorerSelection& selection = context.pane.selection;
    if ((child_action != nullptr) && child_action->isEnabled() && selection.hasSingleEntry()) {
        // Files "Extract to {name}\" label from the archive's own stem.
        child_action->setText(
            tr("Extract to %1\\")
                .arg(QFileInfo(selection.entries.first().name).completeBaseName()));
    }
}

// Files FileTagsContextMenu: one checkable row per known tag (checked when
// every selected item carries it), then Remove tags and the S.A.K. free-text
// editor. Tags are app-level metadata, so this works on raw targets too.
void FileManagementExplorerPanel::addTagsSubmenu(QMenu* menu,
                                                 const FileExplorerCommandContext& context) {
    auto* tags_menu = menu->addMenu(tr("Tags"));
    tags_menu->setObjectName(QStringLiteral("fileExplorerTagsSubmenu"));
    const FileExplorerSelection selection = context.pane.selection;
    const QString target_id = FileExplorerTargetId::fromTarget(currentTarget()).value;
    QSettings settings;
    const QStringList known_tags = allKnownTags();
    // The tag store is user-writable and uncapped. Read each selected entry's tags
    // ONCE (not once per tag x entry, which is a quadratic settings walk on the
    // GUI thread), bound the rows this menu builds, and bound the label length a
    // single record can push into a menu item.
    QList<QStringList> entry_tags;
    entry_tags.reserve(selection.entries.size());
    for (const FileManagementEntry& entry : selection.entries) {
        entry_tags.append(FileExplorerTagStore::tagsFor(
            settings, QString::fromLatin1(kTagStoreGroup), target_id, entry.path));
    }
    constexpr qsizetype kMaxTagMenuRows = 100;
    constexpr qsizetype kMaxTagLabelChars = 120;
    const qsizetype shown_tags = std::min<qsizetype>(known_tags.size(), kMaxTagMenuRows);
    for (qsizetype index = 0; index < shown_tags; ++index) {
        const QString tag = known_tags.at(index);
        const bool on_all = !entry_tags.isEmpty() &&
                            std::ranges::all_of(entry_tags, [&tag](const QStringList& tags) {
                                return tags.contains(tag, Qt::CaseInsensitive);
                            });
        QAction* action = tags_menu->addAction(tag.left(kMaxTagLabelChars));
        action->setCheckable(true);
        action->setChecked(on_all);
        connect(action, &QAction::triggered, this, [this, tag](const bool checked) {
            toggleTagOnSelection(tag, checked);
        });
    }
    if (known_tags.size() > shown_tags) {
        // Never drop rows silently: say how many are not listed.
        QAction* more = tags_menu->addAction(tr(
            "... and %n more tag(s)", nullptr, static_cast<int>(known_tags.size() - shown_tags)));
        more->setEnabled(false);
    }
    if (!known_tags.isEmpty()) {
        tags_menu->addSeparator();
    }
    addCommandMenuAction(tags_menu, FileExplorerCommandId::RemoveTags, context);
    auto* edit_tags = tags_menu->addAction(tr("Edit Tags..."));
    edit_tags->setObjectName(QStringLiteral("fileExplorerEditTagsAction"));
    edit_tags->setEnabled(selection.hasSingleEntry());
    edit_tags->setToolTip(
        tr("Tag this item with S.A.K. metadata (never written to the file system)."));
    connect(
        edit_tags, &QAction::triggered, this, &FileManagementExplorerPanel::editSelectedItemTags);
}

void FileManagementExplorerPanel::toggleTagOnSelection(const QString& tag, const bool add) {
    const FileExplorerSelection selection = currentSelection();
    const QString target_id = FileExplorerTargetId::fromTarget(currentTarget()).value;
    if (selection.isEmpty() || target_id.isEmpty()) {
        return;
    }
    QSettings settings;
    for (const FileManagementEntry& entry : selection.entries) {
        QStringList tags = FileExplorerTagStore::tagsFor(
            settings, QString::fromLatin1(kTagStoreGroup), target_id, entry.path);
        const bool present = tags.contains(tag, Qt::CaseInsensitive);
        if (add && !present) {
            tags.append(tag);
        } else if (!add && present) {
            const auto matching = std::ranges::remove_if(tags, [&tag](const QString& existing) {
                return existing.compare(tag, Qt::CaseInsensitive) == 0;
            });
            tags.erase(matching.begin(), tags.end());
        } else {
            continue;
        }
        FileExplorerTagStore::setTags(
            settings, QString::fromLatin1(kTagStoreGroup), target_id, entry.path, tags);
    }
    updateDetailsPane();
    if (m_item_model != nullptr) {
        m_item_model->refreshTags();
    }
    rebuildTargetList();
}

int FileManagementExplorerPanel::resolveContextMenuTargetIndex(const QPoint& position) {
    if (const QModelIndex index = m_target_list->indexAt(position); index.isValid()) {
        auto* item = m_target_list->item(index.row());
        if (item != nullptr) {
            const auto kind = static_cast<SidebarEntryKind>(item->data(kSidebarKindRole).toInt());
            if (kind == SidebarEntryKind::Target) {
                // A row that cannot say WHICH target it is must not be coerced into
                // target 0 by QVariant's default; a target action would then run
                // against a guessed disk.
                bool index_ok = false;
                const int target_index = item->data(kTargetIndexRole).toInt(&index_ok);
                if (!index_ok) {
                    return -1;
                }
                m_target_list->setCurrentRow(index.row());
                return target_index;
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
    const int position = static_cast<int>(m_favorite_target_ids.indexOf(target_id));
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

// Sidebar drag-and-drop (Files SidebarViewModel): explorer items drop onto
// tag rows (drag-to-tag) or target rows (copy/move to that location), and
// favorites reorder by dragging within their section.
bool FileManagementExplorerPanel::handleSidebarViewportEvent(QEvent* event) {
    switch (event->type()) {
    case QEvent::MouseButtonPress:
    case QEvent::MouseMove:
        return maybeStartFavoriteDrag(event);
    case QEvent::DragEnter:
    case QEvent::DragMove:
        return handleSidebarDragOver(static_cast<QDropEvent*>(event));
    case QEvent::Drop:
        return handleSidebarDrop(static_cast<QDropEvent*>(event));
    default:
        return false;
    }
}

// Files DragDropHelpers cascade against the DROP target: Ctrl forces Copy,
// Shift forces Move, an unmodified drop moves within the payload's own
// target and copies otherwise.
Qt::DropAction FileManagementExplorerPanel::sidebarPasteAction(const FileManagementTarget& target,
                                                               const QDropEvent* drop) const {
    if (drop->modifiers().testFlag(Qt::ControlModifier)) {
        return Qt::CopyAction;
    }
    if (drop->modifiers().testFlag(Qt::ShiftModifier)) {
        return Qt::MoveAction;
    }
    const QMimeData* mime = drop->mimeData();
    if ((mime != nullptr) && mime->hasFormat(QLatin1String(kExplorerClipboardMime))) {
        const QJsonObject payload =
            QJsonDocument::fromJson(mime->data(QLatin1String(kExplorerClipboardMime))).object();
        if (payload.value(QStringLiteral("target")).toString() ==
            FileExplorerTargetId::fromTarget(target).value) {
            return Qt::MoveAction;
        }
    }
    return Qt::CopyAction;
}

bool FileManagementExplorerPanel::handleSidebarDragOver(QDropEvent* drop) {
    const QListWidgetItem* item = m_target_list->itemAt(drop->position().toPoint());
    if (item == nullptr) {
        return false;
    }
    const QMimeData* mime = drop->mimeData();
    if (mime->hasFormat(QLatin1String(kSidebarFavoriteMime))) {
        // Favorites reorder: only other favorites rows are drop positions.
        if (!item->data(kSidebarFavoritePosRole).isNull()) {
            drop->setDropAction(Qt::MoveAction);
            drop->accept();
            return true;
        }
        return false;
    }
    if (!mimeHasPasteableItems(mime)) {
        return false;
    }
    const auto kind = static_cast<SidebarEntryKind>(item->data(kSidebarKindRole).toInt());
    if (kind == SidebarEntryKind::Tag) {
        // Files HandleTagItemDragOverAsync: tagging advertises the Link
        // operation ("Link to {tag}").
        drop->setDropAction(Qt::LinkAction);
        drop->accept();
        return true;
    }
    if (kind == SidebarEntryKind::Target) {
        const int target_index = item->data(kTargetIndexRole).toInt();
        if (target_index >= 0 && target_index < m_targets.size() &&
            m_targets.at(target_index).can_write_files) {
            drop->setDropAction(sidebarPasteAction(m_targets.at(target_index), drop));
            drop->accept();
            return true;
        }
    }
    return false;
}

bool FileManagementExplorerPanel::handleSidebarDrop(QDropEvent* drop) {
    const QListWidgetItem* item = m_target_list->itemAt(drop->position().toPoint());
    if (item == nullptr) {
        return false;
    }
    const QMimeData* mime = drop->mimeData();
    if (mime->hasFormat(QLatin1String(kSidebarFavoriteMime))) {
        const QVariant to_position = item->data(kSidebarFavoritePosRole);
        if (to_position.isNull()) {
            return false;
        }
        drop->acceptProposedAction();
        reorderFavorite(mime->data(QLatin1String(kSidebarFavoriteMime)).toInt(),
                        to_position.toInt());
        return true;
    }
    if (!mimeHasPasteableItems(mime)) {
        return false;
    }
    const auto kind = static_cast<SidebarEntryKind>(item->data(kSidebarKindRole).toInt());
    if (kind == SidebarEntryKind::Tag) {
        drop->acceptProposedAction();
        applyDroppedTag(item->data(kSidebarTagRole).toString(), mime);
        return true;
    }
    if (kind != SidebarEntryKind::Target) {
        return false;
    }
    return handleSidebarTargetDrop(item->data(kTargetIndexRole).toInt(), drop);
}

bool FileManagementExplorerPanel::handleSidebarTargetDrop(const int target_index,
                                                          QDropEvent* drop) {
    if (target_index < 0 || target_index >= m_targets.size() ||
        !m_targets.at(target_index).can_write_files) {
        return false;
    }
    const FileManagementTarget target = m_targets.at(target_index);
    drop->setDropAction(sidebarPasteAction(target, drop));
    drop->accept();
    PasteSources sources = collectPasteSources(drop->mimeData());
    sources.move = drop->dropAction() == Qt::MoveAction;
    sources.clipboard = false;
    const QString destination = target.local_file_system ? target.root_path : QStringLiteral("/");
    // Deferred like the view drop: collision dialogs must not run inside the
    // native drag loop, and the mime data dies with the drag.
    QMetaObject::invokeMethod(
        this,
        [this, target, sources, destination]() { executePasteTo(target, sources, destination); },
        Qt::QueuedConnection);
    return true;
}

// Files HandleTagItemDroppedAsync: append the tag to every dragged item that
// does not carry it yet. Tags are app-level metadata keyed by the payload's
// own target, so this works for raw-target items too.
void FileManagementExplorerPanel::applyDroppedTag(const QString& tag, const QMimeData* mime) {
    const PasteSources sources = collectPasteSources(mime);
    if (tag.trimmed().isEmpty() || sources.source_target_id.isEmpty()) {
        Q_EMIT statusMessage(tr("Drag items from the explorer to tag them."),
                             sak::kTimerStatusMessageMs);
        return;
    }
    QStringList paths = sources.host_files;
    for (const PasteItem& raw_item : sources.raw_items) {
        paths.append(raw_item.path);
    }
    QSettings settings;
    int tagged = 0;
    for (const QString& path : paths) {
        QStringList tags = FileExplorerTagStore::tagsFor(
            settings, QString::fromLatin1(kTagStoreGroup), sources.source_target_id, path);
        if (tags.contains(tag, Qt::CaseInsensitive)) {
            continue;
        }
        tags.append(tag);
        FileExplorerTagStore::setTags(
            settings, QString::fromLatin1(kTagStoreGroup), sources.source_target_id, path, tags);
        ++tagged;
    }
    if (m_item_model != nullptr) {
        m_item_model->refreshTags();
    }
    Q_EMIT statusMessage(tr("Tagged %1 item(s) with %2").arg(tagged).arg(tag),
                         sak::kTimerStatusDefaultMs);
}

void FileManagementExplorerPanel::reorderFavorite(const int from_position, const int to_position) {
    if (from_position < 0 || from_position >= m_favorite_target_ids.size() || to_position < 0 ||
        to_position >= m_favorite_target_ids.size() || from_position == to_position) {
        return;
    }
    m_favorite_target_ids.move(from_position, to_position);
    saveSidebarState();
    rebuildTargetList();
    Q_EMIT statusMessage(tr("Favorites reordered"), sak::kTimerStatusMessageMs);
}

// Manual drag start for favorites rows (the sidebar is a plain QListWidget:
// press arms a candidate, moving past the drag threshold starts the drag).
bool FileManagementExplorerPanel::maybeStartFavoriteDrag(QEvent* event) {
    auto* mouse = static_cast<QMouseEvent*>(event);
    if (event->type() == QEvent::MouseButtonPress) {
        m_sidebar_press_favorite = -1;
        if (mouse->button() == Qt::LeftButton) {
            if (const QListWidgetItem* item = m_target_list->itemAt(mouse->position().toPoint())) {
                const QVariant position_role = item->data(kSidebarFavoritePosRole);
                if (!position_role.isNull()) {
                    m_sidebar_press_favorite = position_role.toInt();
                    m_sidebar_press_pos = mouse->position().toPoint();
                }
            }
        }
        return false;  // selection and click handling proceed normally
    }
    if (m_sidebar_press_favorite < 0 || !mouse->buttons().testFlag(Qt::LeftButton)) {
        return false;
    }
    if ((mouse->position().toPoint() - m_sidebar_press_pos).manhattanLength() <
        QApplication::startDragDistance()) {
        return false;
    }
    const int favorite_position = m_sidebar_press_favorite;
    m_sidebar_press_favorite = -1;
    auto* drag = new QDrag(m_target_list);
    auto* mime = new QMimeData;
    mime->setData(QLatin1String(kSidebarFavoriteMime), QByteArray::number(favorite_position));
    drag->setMimeData(mime);
    drag->exec(Qt::MoveAction);
    return true;
}

// Files ShowEjectDevice: drives only, never the system drive. Dismounting
// flushes and invalidates the mounted volume through the same primitives the
// flasher uses before raw writes.
bool FileManagementExplorerPanel::canEjectTarget(const FileManagementTarget& target) const {
    if (!target.local_file_system || target.kind != FileManagementTargetKind::LocalPath) {
        return false;
    }
    const QString root = QDir::cleanPath(target.root_path);
    constexpr int kDriveSpecifierLength = 2;  // drive letter + ':'
    if (root.size() < kDriveSpecifierLength || root.at(1) != QLatin1Char(':')) {
        return false;
    }
    return !root.startsWith(QDir::rootPath().left(kDriveSpecifierLength), Qt::CaseInsensitive);
}

void FileManagementExplorerPanel::ejectLocalTargetAtIndex(const int target_index) {
    if (target_index < 0 || target_index >= m_targets.size()) {
        return;
    }
    const FileManagementTarget target = m_targets.at(target_index);
    if (!canEjectTarget(target)) {
        return;
    }
    const QString letter = QDir::cleanPath(target.root_path).left(2);
    DriveUnmounter unmounter;
    HANDLE volume = unmounter.lockVolume(QStringLiteral("\\\\.\\%1").arg(letter));
    if (volume == INVALID_HANDLE_VALUE || !unmounter.dismountVolume(volume)) {
        sak::showWarningLogged(this,
                               tr("Eject"),
                               unmounter.lastError().isEmpty()
                                   ? tr("Could not eject %1.").arg(letter)
                                   : unmounter.lastError());
        return;
    }
    logMessage(tr("Ejected volume %1").arg(letter));
    Q_EMIT statusMessage(tr("Ejected %1").arg(letter), sak::kTimerStatusDefaultMs);
    onRefreshMountedTargets();
}

// A stale "[offline]" favorite has no connected target index, but the pin itself must
// stay removable; show its dedicated menu and return true when the row was a stale pin.
bool FileManagementExplorerPanel::showStaleFavoriteContextMenu(const QPoint& position) {
    const QModelIndex index = m_target_list->indexAt(position);
    if (!index.isValid()) {
        return false;
    }
    auto* item = m_target_list->item(index.row());
    if ((item == nullptr) || static_cast<SidebarEntryKind>(item->data(kSidebarKindRole).toInt()) !=
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
    if (m_target_list == nullptr) {
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
    auto* copy_root = menu.addAction(tr("Copy Target Root"));
    copy_root->setEnabled(has_menu_target && !m_targets.at(menu_target_index).root_path.isEmpty());
    connect(copy_root, &QAction::triggered, this, [this, menu_target_index]() {
        copyTargetRootAtIndex(menu_target_index);
    });
    auto* favorite = menu.addAction(favoriteActionLabel(menu_target_index, has_menu_target));
    favorite->setEnabled(has_menu_target);
    connect(favorite, &QAction::triggered, this, [this, menu_target_index]() {
        toggleFavoriteAtIndex(menu_target_index);
    });
    const bool is_favorite = has_menu_target && isFavoriteTargetIndex(menu_target_index);
    auto* move_up = menu.addAction(tr("Move Favorite Up"));
    move_up->setObjectName(QStringLiteral("fileExplorerMoveFavoriteUp"));
    move_up->setEnabled(is_favorite);
    connect(move_up, &QAction::triggered, this, [this, menu_target_index]() {
        moveFavoriteAtIndex(menu_target_index, -1);
    });
    auto* move_down = menu.addAction(tr("Move Favorite Down"));
    move_down->setObjectName(QStringLiteral("fileExplorerMoveFavoriteDown"));
    move_down->setEnabled(is_favorite);
    connect(move_down, &QAction::triggered, this, [this, menu_target_index]() {
        moveFavoriteAtIndex(menu_target_index, 1);
    });
    auto* properties = menu.addAction(tr("Target Properties"));
    properties->setEnabled(has_menu_target);
    connect(properties, &QAction::triggered, this, [this, menu_target_index]() {
        showTargetPropertiesAtIndex(menu_target_index);
    });
    // Files SidebarViewModel ShowEjectDevice: drives only, never the system
    // drive.
    auto* eject = menu.addAction(tr("Eject"));
    eject->setObjectName(QStringLiteral("fileExplorerEjectTarget"));
    eject->setEnabled(has_menu_target && canEjectTarget(m_targets.at(menu_target_index)));
    connect(eject, &QAction::triggered, this, [this, menu_target_index]() {
        ejectLocalTargetAtIndex(menu_target_index);
    });
    menu.addSeparator();
    addSidebarGlobalMenuActions(&menu);
    menu.exec(m_target_list->viewport()->mapToGlobal(position));
}

// The target-independent tail of the sidebar context menu: discovery,
// recents, the Files reorder dialog, and the section toggles.
void FileManagementExplorerPanel::addSidebarGlobalMenuActions(QMenu* menu) {
    auto* refresh = menu->addAction(tr("Refresh Mounted Targets"));
    connect(
        refresh, &QAction::triggered, this, &FileManagementExplorerPanel::onRefreshMountedTargets);
    auto* scan = menu->addAction(tr("Scan Disks"));
    connect(scan, &QAction::triggered, this, &FileManagementExplorerPanel::onScanDiskTargets);
    auto* add_manual = menu->addAction(tr("Add Raw/Image"));
    connect(add_manual, &QAction::triggered, this, &FileManagementExplorerPanel::onAddManualTarget);
    auto* clear_recent = menu->addAction(tr("Clear Recent"));
    clear_recent->setObjectName(QStringLiteral("fileExplorerClearRecent"));
    clear_recent->setEnabled(!m_recent_target_ids.isEmpty());
    connect(
        clear_recent, &QAction::triggered, this, &FileManagementExplorerPanel::clearRecentTargets);
    menu->addSeparator();
    auto* reorder = menu->addAction(tr("Reorder sidebar items..."));
    reorder->setObjectName(QStringLiteral("fileExplorerReorderSidebarItems"));
    reorder->setEnabled(m_favorite_target_ids.size() > 1);
    connect(reorder,
            &QAction::triggered,
            this,
            &FileManagementExplorerPanel::showReorderFavoritesDialog);
    addSidebarSectionToggleMenu(menu);
}

// Files ReorderSidebarItemsDialog: a drag-reorder list of the pinned items
// with Save/Cancel; Save applies the new pin order.
void FileManagementExplorerPanel::showReorderFavoritesDialog() {
    QDialog dialog(this);
    dialog.setObjectName(QStringLiteral("fileExplorerReorderDialog"));
    dialog.setWindowTitle(tr("Reorder sidebar items"));
    auto* layout = new QVBoxLayout(&dialog);
    auto* list = new QListWidget(&dialog);
    list->setObjectName(QStringLiteral("fileExplorerReorderList"));
    list->setDragDropMode(QAbstractItemView::InternalMove);
    for (const QString& target_id : m_favorite_target_ids) {
        const int index = targetIndexForId(target_id);
        auto* item = new QListWidgetItem(
            index >= 0 ? m_targets.at(index).label : tr("%1 [offline]").arg(target_id), list);
        item->setData(Qt::UserRole, target_id);
    }
    layout->addWidget(list);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel,
                                         &dialog);
    buttons->setObjectName(QStringLiteral("fileExplorerReorderButtons"));
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addWidget(buttons);
    if (dialog.exec() != QDialog::Accepted) {
        return;
    }
    QStringList reordered;
    reordered.reserve(list->count());
    for (int row = 0; row < list->count(); ++row) {
        reordered.append(list->item(row)->data(Qt::UserRole).toString());
    }
    m_favorite_target_ids = reordered;
    saveSidebarState();
    rebuildTargetList();
}

void FileManagementExplorerPanel::onItemDoubleClicked(const QModelIndex& index) {
    // Empty-area double-clicks are handled by the viewport filter
    // (DoubleClickToGoUp); only a real item activates.
    if (!index.isValid()) {
        return;
    }
    onOpenSelected();
}

}  // namespace sak
