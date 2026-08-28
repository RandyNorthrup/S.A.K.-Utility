// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file test_file_management_explorer_panel.cpp
/// @brief GUI tests for the File Management Explorer shell.

#include "sak/file_explorer_archive_service.h"
#include "sak/file_explorer_breadcrumb.h"
#include "sak/file_explorer_details_pane.h"
#include "sak/file_explorer_details_view.h"
#include "sak/file_explorer_icon_registry.h"
#include "sak/file_explorer_item_model.h"
#include "sak/file_explorer_name_delegate.h"
#include "sak/file_explorer_omnibar.h"
#include "sak/file_explorer_pane.h"
#include "sak/file_explorer_properties_dialog.h"
#include "sak/file_explorer_sidebar.h"
#include "sak/file_explorer_sort_filter_model.h"
#include "sak/file_explorer_tag_store.h"
#include "sak/file_explorer_transfer_worker.h"
#include "sak/file_management_explorer_panel.h"
#include "sak/rich_text_safety.h"

#include <QAbstractItemView>
#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QContextMenuEvent>
#include <QCoreApplication>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QHash>
#include <QHeaderView>
#include <QImage>
#include <QInputDialog>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListView>
#include <QListWidget>
#include <QMenu>
#include <QMessageBox>
#include <QMimeData>
#include <QPainter>
#include <QPalette>
#include <QPixmap>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QScrollArea>
#include <QSettings>
#include <QShortcut>
#include <QSlider>
#include <QSplitter>
#include <QStackedWidget>
#include <QTabBar>
#include <QTableView>
#include <QTemporaryDir>
#include <QTimer>
#include <QToolButton>
#include <QtTest/QtTest>
#include <QUrl>

#include <algorithm>
#include <atomic>
#include <memory>
#include <tuple>

#include <private/qzipwriter_p.h>

namespace sak {
// Defined in file_explorer_properties_dialog.cpp: wraps a directory lister so a
// set cancel flag makes each directory report as unlistable, so the properties
// size walk unwinds instead of freezing the closing dialog's waitForFinished().
DirectoryLister makeCancelableLister(DirectoryLister base,
                                     std::shared_ptr<const std::atomic_bool> cancel);
}  // namespace sak

namespace {

template <typename Widget>
Widget* child(QWidget* parent, const char* name) {
    return parent->findChild<Widget*>(QString::fromLatin1(name));
}

QStringList collectContextMenuTextsAt(QWidget* target, const QPoint& point) {
    QStringList texts;
    QTimer::singleShot(0, [&texts]() {
        auto* menu = qobject_cast<QMenu*>(QApplication::activePopupWidget());
        if (!menu) {
            return;
        }
        const auto actions = menu->actions();
        for (const auto* action : actions) {
            if (!action->isSeparator()) {
                texts.append(action->text());
            }
        }
        menu->close();
    });

    QContextMenuEvent event(QContextMenuEvent::Mouse, point, target->mapToGlobal(point));
    QApplication::sendEvent(target, &event);
    QApplication::processEvents();
    QApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    return texts;
}

QStringList collectContextMenuTexts(QWidget* target) {
    return collectContextMenuTextsAt(target, target->rect().center());
}

bool containsTextStartingWith(const QStringList& texts, const QString& prefix) {
    return std::any_of(texts.cbegin(), texts.cend(), [&](const QString& text) {
        return text.startsWith(prefix);
    });
}

bool hasVisiblePixel(const QPixmap& pixmap) {
    if (pixmap.isNull()) {
        return false;
    }
    const QImage image = pixmap.toImage().convertToFormat(QImage::Format_ARGB32);
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x) {
            if (qAlpha(image.pixel(x, y)) > 0) {
                return true;
            }
        }
    }
    return false;
}

QAction* actionStartingWith(QMenu* menu, const QString& prefix) {
    if (!menu) {
        return nullptr;
    }
    for (auto* action : menu->actions()) {
        if (!action->isSeparator() && action->text().startsWith(prefix)) {
            return action;
        }
    }
    return nullptr;
}

// Opens the context menu and triggers the top-level action starting with
// @p prefix.
bool triggerContextMenuActionStartingWith(QWidget* target, const QString& prefix) {
    bool triggered = false;
    QTimer::singleShot(0, [&triggered, &prefix]() {
        auto* menu = qobject_cast<QMenu*>(QApplication::activePopupWidget());
        if (!menu) {
            return;
        }
        if (QAction* action = actionStartingWith(menu, prefix); action && action->isEnabled()) {
            action->trigger();
            triggered = true;
        }
        menu->close();
    });
    const QPoint point = target->rect().center();
    QContextMenuEvent event(QContextMenuEvent::Mouse, point, target->mapToGlobal(point));
    QApplication::sendEvent(target, &event);
    QApplication::processEvents();
    QApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    return triggered;
}

// Opens the item context menu and triggers the action starting with @p prefix
// inside the named submenu (submenus are populated without being shown).
bool triggerContextSubmenuAction(QWidget* target,
                                 const QString& submenu_object_name,
                                 const QString& prefix,
                                 const QPoint& at = QPoint()) {
    bool triggered = false;
    QTimer::singleShot(0, [&triggered, &submenu_object_name, &prefix]() {
        auto* menu = qobject_cast<QMenu*>(QApplication::activePopupWidget());
        if (!menu) {
            return;
        }
        auto* submenu = menu->findChild<QMenu*>(submenu_object_name);
        if (QAction* action = actionStartingWith(submenu, prefix); action && action->isEnabled()) {
            action->trigger();
            triggered = true;
        }
        menu->close();
    });
    const QPoint point = at.isNull() ? target->rect().center() : at;
    QContextMenuEvent event(QContextMenuEvent::Mouse, point, target->mapToGlobal(point));
    QApplication::sendEvent(target, &event);
    QApplication::processEvents();
    QApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    return triggered;
}

void resetExplorerPanelSettings() {
    QSettings settings;
    settings.beginGroup(QStringLiteral("FileManagementExplorer"));
    settings.remove(QString());
    settings.endGroup();
}

sak::FileManagementEntry testEntry(const QString& name, const bool directory) {
    sak::FileManagementEntry entry;
    entry.name = name;
    entry.path = QStringLiteral("/%1").arg(name);
    entry.type = directory ? QStringLiteral("directory") : QStringLiteral("file");
    entry.directory = directory;
    entry.regular_file = !directory;
    return entry;
}

void captureBaselineImage(const QImage& image, const QString& name) {
    if (qEnvironmentVariableIsEmpty("SAK_CAPTURE_FILE_EXPLORER_BASELINE")) {
        return;
    }

    QDir dir(QDir::currentPath());
    QVERIFY(dir.mkpath(QStringLiteral("artifacts/file-management-explorer-baseline")));
    const QString path = dir.filePath(
        QStringLiteral("artifacts/file-management-explorer-baseline/%1.png").arg(name));
    QVERIFY2(image.save(path), qPrintable(path));
}

void captureBaseline(QWidget* widget, const QString& name) {
    if (qEnvironmentVariableIsEmpty("SAK_CAPTURE_FILE_EXPLORER_BASELINE")) {
        return;
    }
    captureBaselineImage(widget->grab().toImage(), name);
}

// Mean perceived luminance (0..255) of the pixels an icon actually inks, ignoring
// transparent ones. -1 when the pixmap is empty. The bundled SVGs carry a fixed
// dark fill and are recolored at paint time by PaletteTintedIconEngine, so this is
// how a test sees WHICH color the engine chose rather than merely that it drew.
double iconInkLuminance(const QPixmap& pixmap) {
    const QImage image = pixmap.toImage().convertToFormat(QImage::Format_ARGB32);
    double total = 0.0;
    qint64 inked = 0;
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x) {
            const QColor color = image.pixelColor(x, y);
            if (color.alpha() == 0) {
                continue;
            }
            // Weight by alpha so a faint antialiased edge cannot outvote solid ink.
            const double weight = color.alpha() / 255.0;
            total += weight * (0.299 * color.red() + 0.587 * color.green() + 0.114 * color.blue());
            inked += 1;
        }
    }
    return inked == 0 ? -1.0 : total / static_cast<double>(inked);
}

// Restores the application palette however the body exits, so a theme test can
// never leak its palette into the rest of the suite.
struct ApplicationPaletteGuard {
    QPalette original = QApplication::palette();
    ~ApplicationPaletteGuard() { QApplication::setPalette(original); }
};

// Dark application palette matching the shell's dark theme well enough for the
// palette-driven style sheet and the icon tinting engine to resolve against it.
QPalette darkTestPalette() {
    QPalette palette;
    const QColor ground(0x1F, 0x1F, 0x1F);
    const QColor ink(0xF0, 0xF0, 0xF0);
    palette.setColor(QPalette::Window, ground);
    palette.setColor(QPalette::Base, QColor(0x2B, 0x2B, 0x2B));
    palette.setColor(QPalette::AlternateBase, QColor(0x33, 0x33, 0x33));
    palette.setColor(QPalette::Button, ground);
    palette.setColor(QPalette::WindowText, ink);
    palette.setColor(QPalette::Text, ink);
    palette.setColor(QPalette::ButtonText, ink);
    palette.setColor(QPalette::ToolTipBase, ground);
    palette.setColor(QPalette::ToolTipText, ink);
    palette.setColor(QPalette::Highlight, QColor(0x2D, 0x5E, 0xA8));
    palette.setColor(QPalette::HighlightedText, ink);
    return palette;
}

// Geometry of the icon comparison sheet, shared by the frame pass and the two
// per-palette icon passes.
struct IconSheetLayout {
    static constexpr int kRowHeight = 44;
    static constexpr int kLabelWidth = 460;
    static constexpr int kCellWidth = 44;
    static constexpr int kHeaderHeight = 34;
    static constexpr int kGroundGap = 12;

    QVector<int> sizes{16, 20, 24, 32};
    int rows{0};

    [[nodiscard]] int groundWidth() const { return sizes.size() * kCellWidth; }
    [[nodiscard]] int lightX() const { return kLabelWidth; }
    [[nodiscard]] int darkX() const { return kLabelWidth + groundWidth() + kGroundGap; }
    [[nodiscard]] int width() const { return kLabelWidth + (2 * groundWidth()) + (2 * kGroundGap); }
    [[nodiscard]] int height() const { return kHeaderHeight + (rows * kRowHeight) + kGroundGap; }
    [[nodiscard]] int rowY(int row) const { return kHeaderHeight + (row * kRowHeight); }
};

// Labels, rules, and the dark band. Palette-independent, so it is painted once.
QImage renderIconSheetFrame(const QVector<sak::FileExplorerIconDescriptor>& descriptors,
                            const IconSheetLayout& layout) {
    QImage sheet(layout.width(), layout.height(), QImage::Format_ARGB32);
    sheet.fill(QColor(0xFA, 0xFA, 0xFA));
    QPainter painter(&sheet);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setPen(QColor(0x20, 0x20, 0x20));
    painter.drawText(
        QRect(8, 6, IconSheetLayout::kLabelWidth - 16, IconSheetLayout::kHeaderHeight - 10),
        Qt::AlignVCenter | Qt::AlignLeft,
        QStringLiteral("icon key  <-  Files upstream key"));
    painter.drawText(
        QRect(layout.lightX(), 6, layout.groundWidth(), IconSheetLayout::kHeaderHeight - 10),
        Qt::AlignCenter,
        QStringLiteral("light palette"));
    painter.drawText(
        QRect(layout.darkX(), 6, layout.groundWidth(), IconSheetLayout::kHeaderHeight - 10),
        Qt::AlignCenter,
        QStringLiteral("dark palette"));
    for (int row = 0; row < descriptors.size(); ++row) {
        const auto& descriptor = descriptors.at(row);
        const int y = layout.rowY(row);
        painter.fillRect(
            QRect(layout.darkX(), y, layout.groundWidth(), IconSheetLayout::kRowHeight),
            QColor(0x1F, 0x1F, 0x1F));
        painter.setPen(QColor(0x20, 0x20, 0x20));
        const QString upstream = descriptor.upstream_key.isEmpty()
                                     ? QStringLiteral("(S.A.K. original glyph)")
                                     : descriptor.upstream_key;
        painter.drawText(
            QRect(8, y, IconSheetLayout::kLabelWidth - 16, IconSheetLayout::kRowHeight),
            Qt::AlignVCenter | Qt::AlignLeft,
            QStringLiteral("%1  <-  %2").arg(descriptor.key, upstream));
        painter.setPen(QColor(0xDD, 0xDD, 0xDD));
        painter.drawLine(0,
                         y + IconSheetLayout::kRowHeight - 1,
                         layout.width(),
                         y + IconSheetLayout::kRowHeight - 1);
    }
    return sheet;
}

// Paints one column of icons at @p column_x. The caller has already installed the
// palette this column is meant to show, because PaletteTintedIconEngine resolves
// its tint from the application palette at paint time -- rendering both columns
// under one palette would print the same ink twice and prove nothing.
void paintIconSheetColumn(QImage* sheet,
                          const QVector<sak::FileExplorerIconDescriptor>& descriptors,
                          const IconSheetLayout& layout,
                          int column_x) {
    QPainter painter(sheet);
    for (int row = 0; row < descriptors.size(); ++row) {
        const int y = layout.rowY(row);
        const QIcon icon = sak::FileExplorerIconRegistry::iconForKey(descriptors.at(row).key);
        for (int column = 0; column < layout.sizes.size(); ++column) {
            const int size = layout.sizes.at(column);
            const int offset = (column * IconSheetLayout::kCellWidth) +
                               ((IconSheetLayout::kCellWidth - size) / 2);
            painter.drawPixmap(column_x + offset,
                               y + ((IconSheetLayout::kRowHeight - size) / 2),
                               icon.pixmap(size, size));
        }
    }
}

// Probe each selectable sidebar row until the omnibar shows a path on the wanted drive
// (e.g. "C:"); returns the matching row or -1 when no mounted local target covers it.
int selectLocalTargetRowForDrive(QListWidget* targetList,
                                 QLineEdit* pathEdit,
                                 const QString& drive_prefix) {
    for (int row = 0; row < targetList->count(); ++row) {
        const auto* item = targetList->item(row);
        if (!item || !item->flags().testFlag(Qt::ItemIsSelectable)) {
            continue;
        }
        targetList->setCurrentRow(row);
        std::ignore =
            QTest::qWaitFor([pathEdit]() { return !pathEdit->text().trimmed().isEmpty(); }, 2000);
        if (pathEdit->text().left(2).toUpper() == drive_prefix) {
            return row;
        }
    }
    return -1;
}

// Wait until @p file can be opened AND is @p expected_size bytes.
//
// exists() is NOT the postcondition for an asynchronous copy: the entry appears the moment
// the copier creates it, while that handle is still open -- Windows can refuse a second
// reader outright -- and before every byte has landed. Waiting on existence alone made the
// paste round-trip fail intermittently in full-suite runs, on the open() that followed.
bool waitForCompleteFile(QFile& file, qint64 expected_size, int timeout_ms = 5000) {
    return QTest::qWaitFor(
        [&file, expected_size]() {
            if (!file.exists() || !file.open(QIODevice::ReadOnly)) {
                return false;
            }
            const qint64 size = file.size();
            file.close();
            return size == expected_size;
        },
        timeout_ms);
}

// Navigate the omnibar to @p directory and wait for a listed row whose name contains
// @p name_fragment (listing is asynchronous); returns the row or -1 on timeout.
int navigateAndFindRow(QLineEdit* pathEdit,
                       QTableView* table,
                       const QString& directory,
                       const QString& name_fragment) {
    pathEdit->setText(directory);
    QTest::keyClick(pathEdit, Qt::Key_Return);
    int file_row = -1;
    const auto findRow = [table, &file_row, &name_fragment]() {
        for (int row = 0; row < table->model()->rowCount(); ++row) {
            if (table->model()->index(row, 0).data().toString().contains(name_fragment)) {
                file_row = row;
                return true;
            }
        }
        return false;
    };
    std::ignore = QTest::qWaitFor(findRow, 5000);
    return file_row;
}

// Select the row whose name contains the fragment and wait until the
// selection survives an event-loop pass (async listing reloads reset the
// model and wipe selections).
bool selectRowStable(QTableView* table, const QString& name_fragment) {
    return QTest::qWaitFor(
        [table, &name_fragment]() {
            int found = -1;
            for (int row = 0; row < table->model()->rowCount(); ++row) {
                if (table->model()->index(row, 0).data().toString().contains(name_fragment)) {
                    found = row;
                    break;
                }
            }
            if (found < 0) {
                return false;
            }
            // Explicit ClearAndSelect: QTableView::selectRow consults the
            // GLOBAL keyboard-modifier state, and QTest::keyClick with a
            // modifier leaves that state sticky, turning selectRow into a
            // toggle.
            const QModelIndex index = table->model()->index(found, 0);
            table->selectionModel()->setCurrentIndex(index, QItemSelectionModel::NoUpdate);
            table->selectionModel()->select(
                index, QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
            QApplication::processEvents();
            const QModelIndexList rows = table->selectionModel()->selectedRows();
            return rows.size() == 1 && rows.first().data().toString().contains(name_fragment);
        },
        5000);
}

// Let the panel's async listings settle: a late model reset would clear
// selections and race whatever the test does next.
bool waitForListingQuiescence(QTableView* table) {
    return QTest::qWaitFor(
        [table]() {
            const int before = table->model() ? table->model()->rowCount() : 0;
            QTest::qWait(200);
            const int after = table->model() ? table->model()->rowCount() : 0;
            return before == after;
        },
        5000);
}

// The first selectable TARGET row. Target rows are the only selectable sidebar
// entries carrying a capability badge ("Label  [Writable]"); Home and Tag rows
// carry plain text, and stale-favorite rows are Qt::NoItemFlags. This used to key
// on a newline, which stopped identifying anything once the row became one line
// to match the Files sidebar template.
// One Safety-pane expectation: the manual image target to add, and the verdict the
// pane must then show for it.
struct SafetyPaneCase {
    QString image;
    QString file_system;
    bool write_enabled{false};
    bool readable{true};  // browse + read; false only for the metadata-only pair
    QString note_fragment;
};

// A sparse image file of exactly @p size bytes. QVERIFY expands to a bare return,
// so this writes through an out-parameter rather than returning the path.
void makeSizedImage(const QDir& root, const QString& name, qint64 size, QString* out) {
    QFile file(root.filePath(name));
    QVERIFY(file.open(QIODevice::WriteOnly));
    QVERIFY(file.resize(size));
    file.close();
    *out = root.filePath(name);
}

// The six raw families the Safety pane must speak for. A manual image target takes
// its size from the file on disk, and that size is what decides the APFS range
// gate -- so the 64 MiB image is the write-enabled APFS case and the 1 KiB image
// the out-of-range one.
void buildSafetyPaneCases(const QDir& root, QVector<SafetyPaneCase>* cases) {
    constexpr qint64 kCertifiedApfsBytes = 64LL * 1024LL * 1024LL;
    constexpr qint64 kTokenImageBytes = 4096;
    struct Seed {
        const char* file_name;
        qint64 size;
        const char* file_system;
        bool write_enabled;
        bool readable;
        const char* note_fragment;
    };
    const std::array<Seed, 6> seeds{{
        {"lane-ext4.img",
         kTokenImageBytes,
         "ext4",
         false,
         true,
         "ext2/ext3/ext4 targets are read-only browse/read/copy-out"},
        {"lane-hfs.img",
         kTokenImageBytes,
         "HFS+",
         true,
         true,
         "HFS+/HFSX writes commit through the Apple-certified"},
        {"lane-apfs-ok.img",
         kCertifiedApfsBytes,
         "APFS",
         true,
         true,
         "APFS writes commit through the Apple-certified in-place COW engine"},
        {"lane-apfs-small.img", 1024, "APFS", false, true, "its size is unknown or out of range"},
        {"lane-xfs.img",
         kTokenImageBytes,
         "XFS",
         false,
         false,
         "XFS/Btrfs targets are metadata-only in this build"},
        {"lane-btrfs.img",
         kTokenImageBytes,
         "Btrfs",
         false,
         false,
         "XFS/Btrfs targets are metadata-only in this build"},
    }};
    for (const Seed& seed : seeds) {
        QString image;
        makeSizedImage(root, QString::fromLatin1(seed.file_name), seed.size, &image);
        QVERIFY(!image.isEmpty());
        cases->append(SafetyPaneCase{.image = image,
                                     .file_system = QString::fromLatin1(seed.file_system),
                                     .write_enabled = seed.write_enabled,
                                     .readable = seed.readable,
                                     .note_fragment = QString::fromLatin1(seed.note_fragment)});
    }
}

int firstTargetRow(QListWidget* list) {
    if (!list) {
        return -1;
    }
    for (int row = 0; row < list->count(); ++row) {
        const auto* item = list->item(row);
        if (item && item->flags().testFlag(Qt::ItemIsSelectable) &&
            item->text().contains(QStringLiteral("  ["))) {
            return row;
        }
    }
    return -1;
}

void verifyShellCoreWidgetsExist(sak::FileManagementExplorerPanel& panel) {
    QVERIFY(child<QListWidget>(&panel, "fileExplorerTargetList"));
    QVERIFY(child<QLineEdit>(&panel, "fileExplorerPathEdit"));
    QVERIFY(child<QLineEdit>(&panel, "fileExplorerSearchBox"));
    QVERIFY(child<QStackedWidget>(&panel, "fileExplorerAddressStack"));
    QVERIFY(child<sak::FileExplorerBreadcrumb>(&panel, "fileExplorerBreadcrumb"));
    QVERIFY(child<QWidget>(&panel, "fileExplorerTabRow"));
    QVERIFY(child<QWidget>(&panel, "fileExplorerStatusRow"));
    QVERIFY(child<QWidget>(&panel, "fileExplorerSidebarFooter"));
    QVERIFY(child<QPushButton>(&panel, "fileExplorerScanDisksButton"));
    QVERIFY(child<QPushButton>(&panel, "fileExplorerAddRawImageButton"));
    QVERIFY(child<QPushButton>(&panel, "fileExplorerNewTabButton"));
    QVERIFY(child<QTableView>(&panel, "fileExplorerTable"));
    QVERIFY(child<QListView>(&panel, "fileExplorerListView"));
    QVERIFY(child<QListView>(&panel, "fileExplorerGridView"));
    QVERIFY(child<QListView>(&panel, "fileExplorerCardsView"));
    QVERIFY(child<QListView>(&panel, "fileExplorerColumnsView"));
    QVERIFY(child<QListView>(&panel, "fileExplorerColumnsPreviewView"));
    QVERIFY(child<sak::FileExplorerDetailsPane>(&panel, "fileExplorerInfoPane"));
    QVERIFY(child<QPushButton>(&panel, "fileExplorerSidebarToggleButton"));
    QVERIFY(child<QPushButton>(&panel, "fileExplorerDetailsToggleButton"));
    QVERIFY(child<QPushButton>(&panel, "fileExplorerSearchButton"));
    QVERIFY(child<QPushButton>(&panel, "fileExplorerCommandButton"));
    QVERIFY(child<QPushButton>(&panel, "fileExplorerRefreshButton"));
    QVERIFY(child<QToolButton>(&panel, "fileExplorerNewButton"));
    QVERIFY(child<QPushButton>(&panel, "fileExplorerCopyButton"));
    QVERIFY(child<QPushButton>(&panel, "fileExplorerPasteButton"));
    QVERIFY(child<QPushButton>(&panel, "fileExplorerPropertiesButton"));
    QVERIFY(child<QToolButton>(&panel, "fileExplorerSelectionButton"));
    QVERIFY(child<QToolButton>(&panel, "fileExplorerSortButton"));
    QVERIFY(child<QPushButton>(&panel, "fileExplorerRenameButton"));
    QVERIFY(child<QPushButton>(&panel, "fileExplorerDeleteButton"));
    QVERIFY(child<QToolButton>(&panel, "fileExplorerViewButton"));
    QVERIFY(child<QLabel>(&panel, "fileExplorerSummaryLabel"));
    QVERIFY(child<QLabel>(&panel, "fileExplorerStatusLabel"));
}

void verifyShellDetailsAndPreviewPanes(sak::FileManagementExplorerPanel& panel) {
    auto* info = child<sak::FileExplorerDetailsPane>(&panel, "fileExplorerInfoPane");
    auto* table = child<QTableView>(&panel, "fileExplorerTable");
    QVERIFY(info);
    QCOMPARE(table->selectionMode(), QAbstractItemView::ExtendedSelection);

    // Files InfoPane: a Details|Preview segmented pill where Details is the
    // default tab and shows the stacked details scroller below the preview.
    auto* detailsTab = child<QPushButton>(&panel, "fileExplorerInfoPaneDetailsTab");
    auto* previewTab = child<QPushButton>(&panel, "fileExplorerInfoPanePreviewTab");
    auto* scroller = child<QScrollArea>(&panel, "fileExplorerInfoPaneDetailsScroll");
    QVERIFY(detailsTab);
    QVERIFY(previewTab);
    QVERIFY(scroller);
    QVERIFY(detailsTab->isChecked());
    QVERIFY(!previewTab->isChecked());
    QVERIFY(scroller->isVisibleTo(info));

    auto* preview = child<QPlainTextEdit>(&panel, "fileExplorerPreviewText");
    QVERIFY(preview);
    // The persistent preview pane is always populated by the auto-preview wiring (a hint when
    // no single readable file is selected), never left blank for the user to wonder about.
    QVERIFY2(!preview->toPlainText().trimmed().isEmpty(), qPrintable(preview->toPlainText()));
    QVERIFY(child<QPlainTextEdit>(&panel, "fileExplorerPropertiesText"));
    QVERIFY(child<QPlainTextEdit>(&panel, "fileExplorerSafetyText"));
    QVERIFY(child<QPlainTextEdit>(&panel, "fileExplorerEvidenceText"));

    // The preview region stays visible on both tabs; only the details
    // scroller toggles (InfoPane.xaml SelectedTab visual states).
    previewTab->click();
    QVERIFY(previewTab->isChecked());
    QVERIFY(!scroller->isVisibleTo(info));
    QVERIFY(preview->isVisibleTo(info));
    detailsTab->click();
    QVERIFY(scroller->isVisibleTo(info));
}

struct ViewModeWidgets {
    QTableView* table;
    QListView* list;
    QListView* grid;
    QListView* cards;
    QListView* columns;
    QListView* columnsPreview;
};

void switchThroughViewModesAndVerifyVisibility(const sak::FileExplorerPane* pane,
                                               QToolButton* view,
                                               QAction* listAction,
                                               const ViewModeWidgets& widgets) {
    listAction->trigger();
    QApplication::processEvents();
    QTRY_VERIFY(widgets.list->isVisible());
    QVERIFY(!widgets.table->isVisible());
    if (pane->sharedSelectionModel()->model() &&
        pane->sharedSelectionModel()->model()->rowCount() > 0) {
        QCOMPARE(pane->sharedSelectionModel()->selectedRows().size(), 1);
    }

    actionStartingWith(view->menu(), QStringLiteral("Grid"))->trigger();
    QApplication::processEvents();
    QTRY_VERIFY(widgets.grid->isVisible());

    actionStartingWith(view->menu(), QStringLiteral("Cards"))->trigger();
    QApplication::processEvents();
    QTRY_VERIFY(widgets.cards->isVisible());

    actionStartingWith(view->menu(), QStringLiteral("Columns"))->trigger();
    QApplication::processEvents();
    QTRY_VERIFY(widgets.columns->isVisible());
    QVERIFY(widgets.columnsPreview->isVisible());

    actionStartingWith(view->menu(), QStringLiteral("Adaptive"))->trigger();
    QApplication::processEvents();
    QTRY_VERIFY(widgets.grid->isVisible());
    QCOMPARE(pane->viewMode(), sak::FileExplorerViewMode::Adaptive);
}

void verifyPersistedAdaptiveViewMode() {
    QSettings savedModeSettings;
    savedModeSettings.beginGroup(QStringLiteral("FileManagementExplorer"));
    savedModeSettings.beginGroup(QStringLiteral("View"));
    const QStringList locationGroups = savedModeSettings.childGroups();
    QCOMPARE(locationGroups.size(), 1);
    savedModeSettings.beginGroup(locationGroups.first());
    QCOMPARE(savedModeSettings.value(QStringLiteral("ViewMode")).toString(),
             QStringLiteral("adaptive"));
    savedModeSettings.endGroup();
    savedModeSettings.endGroup();
    savedModeSettings.endGroup();
}

void verifyRestoredPaneMatchesPersistedSettings() {
    sak::FileManagementExplorerPanel restored;
    restored.resize(900, 600);
    restored.show();
    QVERIFY(QTest::qWaitForWindowExposed(&restored));
    auto* restoredPane = restored.findChild<sak::FileExplorerPane*>();
    QVERIFY(restoredPane);
    QCOMPARE(restoredPane->viewMode(), sak::FileExplorerViewMode::Adaptive);
    QCOMPARE(restoredPane->layoutSizes().grid, 12);
    QCOMPARE(restoredPane->showFileExtensions(), false);
}

QVector<sak::FileExplorerCommandId> bundledIconMappedCommands() {
    return QVector<sak::FileExplorerCommandId>{
        sak::FileExplorerCommandId::Open,
        sak::FileExplorerCommandId::OpenInNewTab,
        sak::FileExplorerCommandId::CopyItemPath,
        sak::FileExplorerCommandId::Refresh,
        sak::FileExplorerCommandId::NewFolder,
        sak::FileExplorerCommandId::WriteFile,
        sak::FileExplorerCommandId::Rename,
        sak::FileExplorerCommandId::Delete,
        sak::FileExplorerCommandId::ViewDetails,
        sak::FileExplorerCommandId::ViewList,
        sak::FileExplorerCommandId::ViewGrid,
        sak::FileExplorerCommandId::ViewCards,
        sak::FileExplorerCommandId::ViewColumns,
        sak::FileExplorerCommandId::ViewAdaptive,
        sak::FileExplorerCommandId::TogglePreviewPane,
        sak::FileExplorerCommandId::ToggleDualPane,
    };
}

void verifyBundledIconForCommand(sak::FileExplorerCommandId command) {
    const QString key = sak::FileExplorerIconRegistry::iconKeyForCommand(command);
    QVERIFY2(!key.isEmpty(), qPrintable(sak::FileExplorerCommandRegistry::commandIdName(command)));

    const auto descriptor = sak::FileExplorerIconRegistry::descriptorForKey(key);
    QVERIFY2(!descriptor.resource_path.isEmpty(), qPrintable(key));
    QVERIFY2(descriptor.resource_path.startsWith(QStringLiteral(":/icons/icons/files/")),
             qPrintable(descriptor.resource_path));
    QVERIFY2(descriptor.upstream_source.startsWith(
                 QStringLiteral("src/Files.App.Controls/ThemedIcon/Styles/")),
             qPrintable(descriptor.upstream_source));
    QCOMPARE(descriptor.license, QStringLiteral("MIT"));
    QVERIFY2(!sak::FileExplorerIconRegistry::iconForCommand(command).isNull(), qPrintable(key));
}

void verifyNamedIconDescriptors() {
    const auto refreshDescriptor =
        sak::FileExplorerIconRegistry::descriptorForKey(QStringLiteral("refresh"));
    QCOMPARE(refreshDescriptor.upstream_key, QStringLiteral("App.ThemedIcons.Refresh"));
    QCOMPARE(refreshDescriptor.upstream_source,
             QStringLiteral("src/Files.App.Controls/ThemedIcon/Styles/Icons.Common.xaml"));
    QVERIFY(!sak::FileExplorerIconRegistry::iconForKey(QStringLiteral("panel-left")).isNull());
    QVERIFY(!sak::FileExplorerIconRegistry::iconForKey(QStringLiteral("more")).isNull());
    for (const QString& key : {QStringLiteral("view-details-28"),
                               QStringLiteral("view-list-28"),
                               QStringLiteral("view-grid-28"),
                               QStringLiteral("view-cards-28"),
                               QStringLiteral("view-columns-28"),
                               QStringLiteral("favorite"),
                               QStringLiteral("status-warning"),
                               QStringLiteral("properties-general"),
                               QStringLiteral("properties-security")}) {
        const auto descriptor = sak::FileExplorerIconRegistry::descriptorForKey(key);
        QVERIFY2(!descriptor.resource_path.isEmpty(), qPrintable(key));
        QVERIFY2(!sak::FileExplorerIconRegistry::iconForKey(key).isNull(), qPrintable(key));
    }
}

void verifyAllDescriptorIconsRenderVisiblePixels() {
    const auto descriptors = sak::FileExplorerIconRegistry::descriptors();
    for (const auto& descriptor : descriptors) {
        const QIcon icon = sak::FileExplorerIconRegistry::iconForKey(descriptor.key);
        QVERIFY2(!icon.isNull(), qPrintable(descriptor.key));
        for (const int size : {16, 20, 24, 32}) {
            QVERIFY2(hasVisiblePixel(icon.pixmap(size, size, QIcon::Normal)),
                     qPrintable(descriptor.key));
            QVERIFY2(hasVisiblePixel(icon.pixmap(size, size, QIcon::Disabled)),
                     qPrintable(descriptor.key));
            QVERIFY2(hasVisiblePixel(icon.pixmap(size, size, QIcon::Active)),
                     qPrintable(descriptor.key));
            QVERIFY2(hasVisiblePixel(icon.pixmap(size, size, QIcon::Selected)),
                     qPrintable(descriptor.key));
        }
    }
}

void verifyShellAccessibilityAndIcons(sak::FileManagementExplorerPanel& panel) {
    auto* summary = child<QLabel>(&panel, "fileExplorerSummaryLabel");
    auto* sidebarToggle = child<QPushButton>(&panel, "fileExplorerSidebarToggleButton");
    auto* detailsToggle = child<QPushButton>(&panel, "fileExplorerDetailsToggleButton");
    auto* search = child<QPushButton>(&panel, "fileExplorerSearchButton");
    auto* command = child<QPushButton>(&panel, "fileExplorerCommandButton");
    auto* refresh = child<QPushButton>(&panel, "fileExplorerRefreshButton");
    auto* newButton = child<QToolButton>(&panel, "fileExplorerNewButton");
    auto* copy = child<QPushButton>(&panel, "fileExplorerCopyButton");
    auto* paste = child<QPushButton>(&panel, "fileExplorerPasteButton");
    auto* properties = child<QPushButton>(&panel, "fileExplorerPropertiesButton");
    auto* rename = child<QPushButton>(&panel, "fileExplorerRenameButton");
    auto* deleteButton = child<QPushButton>(&panel, "fileExplorerDeleteButton");
    auto* view = child<QToolButton>(&panel, "fileExplorerViewButton");
    QVERIFY(!summary->accessibleName().isEmpty());
    QVERIFY(!sidebarToggle->accessibleName().isEmpty());
    QVERIFY(!detailsToggle->toolTip().isEmpty());
    QVERIFY(!search->toolTip().isEmpty());
    QVERIFY(!command->toolTip().isEmpty());
    QVERIFY(!view->toolTip().isEmpty());
    QVERIFY(!sidebarToggle->icon().isNull());
    QVERIFY(!detailsToggle->icon().isNull());
    QVERIFY(!command->icon().isNull());
    QVERIFY(!refresh->icon().isNull());
    QVERIFY(!newButton->icon().isNull());
    QVERIFY(!newButton->accessibleName().isEmpty());
    QVERIFY(!copy->icon().isNull());
    QVERIFY(!paste->icon().isNull());
    QVERIFY(!properties->icon().isNull());
    QVERIFY(!rename->icon().isNull());
    QVERIFY(!deleteButton->icon().isNull());
    QVERIFY(!view->icon().isNull());
}

}  // namespace

class FileManagementExplorerPanelTests : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void initTestCase() {
        QCoreApplication::setOrganizationName(QStringLiteral("SAKUtilityTests"));
        QCoreApplication::setApplicationName(QStringLiteral("FileExplorerPanelTests"));
        // The info pane persists its selected tab; a stale Preview choice from
        // an earlier run would hide the details scroller mid-suite.
        QSettings settings;
        settings.remove(QStringLiteral("FileManagementExplorer/InfoPane"));
    }

    void init() { resetExplorerPanelSettings(); }

    void detailsPanePreviewSwitchesBetweenTextAndImage() {
        sak::FileExplorerDetailsPane pane;
        QVERIFY(pane.previewText());
        QVERIFY(pane.previewImage());
        QVERIFY(pane.previewCaption());
        // Contract: the Preview region shows the text/hex view OR the image view and
        // swaps between them on showImagePreview(). Assert the caller-observable result --
        // which view the user actually sees -- rather than the internal QStackedWidget
        // handle and its page index. This stays green if the pages are reordered or the
        // stack is swapped for another show/hide mechanism, yet it goes red if
        // showImagePreview() ever surfaces the WRONG view: a page-order swap that still
        // setCurrentIndex(1) would satisfy an index check while showing the text view.
        // isVisibleTo(&pane) reads the explicit hide flags QStackedWidget sets on its
        // non-current page, so it is correct without show()-ing the top-level pane -- the
        // same idiom this suite uses for the details scroller (verifyShellDetailsAndPreviewPanes).
        pane.showImagePreview(true);
        QVERIFY(pane.previewImage()->isVisibleTo(&pane));
        QVERIFY(!pane.previewText()->isVisibleTo(&pane));
        pane.showImagePreview(false);
        QVERIFY(pane.previewText()->isVisibleTo(&pane));
        QVERIFY(!pane.previewImage()->isVisibleTo(&pane));
    }

    void explorerTabsOpenAndSwitch() {
        sak::FileManagementExplorerPanel panel;
        panel.resize(1100, 700);
        panel.show();
        QVERIFY(QTest::qWaitForWindowExposed(&panel));

        auto* tabs = child<QTabBar>(&panel, "fileExplorerTabBar");
        auto* newTab = child<QPushButton>(&panel, "fileExplorerNewTabButton");
        QVERIFY(tabs);
        QVERIFY(newTab);
        QCOMPARE(tabs->count(), 1);

        // The new-tab button opens a second tab and makes it active.
        newTab->click();
        QApplication::processEvents();
        QCOMPARE(tabs->count(), 2);
        QCOMPARE(tabs->currentIndex(), 1);

        // Switching back to the first tab restores it without error.
        tabs->setCurrentIndex(0);
        QApplication::processEvents();
        QCOMPARE(tabs->currentIndex(), 0);
    }

    void shellCreatesFilesLikeRegions() {
        sak::FileManagementExplorerPanel panel;
        panel.resize(1280, 760);
        panel.show();
        QVERIFY(QTest::qWaitForWindowExposed(&panel));

        verifyShellCoreWidgetsExist(panel);
        verifyShellDetailsAndPreviewPanes(panel);
        verifyShellAccessibilityAndIcons(panel);
        // Let the first listing land before grabbing. The status labels size
        // themselves from their text, and the initial listing arrives after the
        // first layout pass -- a grab taken before it caught the summary label at
        // its pre-listing width, so the committed baseline showed "local fil".
        auto* table = child<QTableView>(&panel, "fileExplorerTable");
        QVERIFY(table);
        QVERIFY(waitForListingQuiescence(table));
        QApplication::processEvents();
        captureBaseline(&panel, QStringLiteral("desktop"));
    }

    void breadcrumbMirrorsPathAndEmitsAncestorNavigation() {
        sak::FileManagementExplorerPanel panel;
        panel.resize(1280, 760);
        panel.show();
        QVERIFY(QTest::qWaitForWindowExposed(&panel));

        auto* pathEdit = child<QLineEdit>(&panel, "fileExplorerPathEdit");
        auto* breadcrumb = child<sak::FileExplorerBreadcrumb>(&panel, "fileExplorerBreadcrumb");
        QVERIFY(pathEdit);
        QVERIFY(breadcrumb);

        // The breadcrumb mirrors the path line and renders one button per segment.
        pathEdit->setText(QStringLiteral("C:/Users/Username"));
        QApplication::processEvents();
        QCOMPARE(breadcrumb->path(), QStringLiteral("C:/Users/Username"));
        const auto segments =
            breadcrumb->findChildren<QPushButton*>(QStringLiteral("fileExplorerBreadcrumbSegment"));
        QCOMPARE(segments.size(), 3);
        QCOMPARE(segments.at(0)->text(), QStringLiteral("C:"));
        QCOMPARE(segments.at(2)->text(), QStringLiteral("Username"));

        // Clicking an ancestor segment emits that ancestor's path.
        QSignalSpy activated(breadcrumb, &sak::FileExplorerBreadcrumb::segmentActivated);
        segments.at(1)->click();
        QCOMPARE(activated.count(), 1);
        QCOMPARE(activated.first().first().toString(), QStringLiteral("C:/Users"));

        // Deep paths collapse the leading segments into an overflow menu.
        pathEdit->setText(QStringLiteral("C:/a/b/c/d/e/f/g"));
        QApplication::processEvents();
        QVERIFY(
            breadcrumb->findChild<QPushButton*>(QStringLiteral("fileExplorerBreadcrumbOverflow")));
    }

    void breadcrumbEditModeSwapsToPathEditAndBack() {
        sak::FileManagementExplorerPanel panel;
        panel.resize(1280, 760);
        panel.show();
        QVERIFY(QTest::qWaitForWindowExposed(&panel));

        auto* stack = child<QStackedWidget>(&panel, "fileExplorerAddressStack");
        auto* pathEdit = child<QLineEdit>(&panel, "fileExplorerPathEdit");
        auto* breadcrumb = child<sak::FileExplorerBreadcrumb>(&panel, "fileExplorerBreadcrumb");
        QVERIFY(stack);
        QVERIFY(pathEdit);
        QVERIFY(breadcrumb);

        // Default: breadcrumb showing. Clicking its empty area enters edit mode.
        QCOMPARE(stack->currentWidget(), breadcrumb);
        QTest::mouseClick(breadcrumb, Qt::LeftButton, {}, breadcrumb->rect().center());
        QApplication::processEvents();
        QCOMPARE(stack->currentWidget(), pathEdit);

        // Clicking elsewhere (focus loss) leaves edit mode without navigating.
        // Simulated as a direct FocusOut: synthesized Escape key events are
        // consumed by the application shortcut map before widget filters in
        // test runs, so the Escape path cannot be driven reliably headless.
        QFocusEvent focus_out(QEvent::FocusOut, Qt::MouseFocusReason);
        QApplication::sendEvent(pathEdit, &focus_out);
        QApplication::processEvents();
        QCOMPARE(stack->currentWidget(), breadcrumb);
    }

    void detailsViewFirstRunShowsFilesStyleColumnSet() {
        // No saved header state: the details view hides the power columns and
        // keeps the Files-style default set visible.
        QSettings settings;
        settings.beginGroup(QStringLiteral("FileExplorerDetailsView"));
        settings.remove(QString());
        settings.endGroup();
        settings.sync();

        sak::FileExplorerItemModel model;
        model.setEntries({testEntry(QStringLiteral("docs"), true)});
        sak::FileExplorerDetailsView view;
        view.setModel(&model);
        view.show();
        QVERIFY(QTest::qWaitForWindowExposed(&view));

        using Model = sak::FileExplorerItemModel;
        QVERIFY(!view.isColumnHidden(Model::NameColumn));
        QVERIFY(!view.isColumnHidden(Model::TypeColumn));
        QVERIFY(!view.isColumnHidden(Model::SizeColumn));
        QVERIFY(!view.isColumnHidden(Model::ModifiedColumn));
        QVERIFY(!view.isColumnHidden(Model::TagsColumn));
        QVERIFY(view.isColumnHidden(Model::CreatedColumn));
        QVERIFY(view.isColumnHidden(Model::IdentifierColumn));
        QVERIFY(view.isColumnHidden(Model::AttributesColumn));
        QVERIFY(view.isColumnHidden(Model::PathColumn));
    }

    void itemModelIconProviderDecoratesNameColumnOnly() {
        sak::FileExplorerItemModel model;
        model.setEntries(
            {testEntry(QStringLiteral("docs"), true), testEntry(QStringLiteral("a.txt"), false)});

        // Without a provider, no decoration is exposed.
        QVERIFY(
            model.data(model.index(0, sak::FileExplorerItemModel::NameColumn), Qt::DecorationRole)
                .isNull());

        model.setIconProvider([](const sak::FileManagementEntry& entry) -> QVariant {
            return entry.directory ? QStringLiteral("folder-icon") : QStringLiteral("file-icon");
        });
        QCOMPARE(
            model.data(model.index(0, sak::FileExplorerItemModel::NameColumn), Qt::DecorationRole)
                .toString(),
            QStringLiteral("folder-icon"));
        QCOMPARE(
            model.data(model.index(1, sak::FileExplorerItemModel::NameColumn), Qt::DecorationRole)
                .toString(),
            QStringLiteral("file-icon"));
        QVERIFY(
            model.data(model.index(0, sak::FileExplorerItemModel::TypeColumn), Qt::DecorationRole)
                .isNull());
    }

    void searchBoxEnterSubmitsSearchIntoListing() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        {
            QFile file(QDir(dir.path()).filePath(QStringLiteral("sak-inline-probe.txt")));
            QVERIFY(file.open(QIODevice::WriteOnly));
            QVERIFY(file.write("inline search payload") > 0);
        }

        sak::FileManagementExplorerPanel panel;
        panel.resize(1280, 760);
        panel.show();
        QVERIFY(QTest::qWaitForWindowExposed(&panel));

        auto* targetList = child<QListWidget>(&panel, "fileExplorerTargetList");
        auto* pathEdit = child<QLineEdit>(&panel, "fileExplorerPathEdit");
        auto* table = child<QTableView>(&panel, "fileExplorerTable");
        auto* searchBox = child<QLineEdit>(&panel, "fileExplorerSearchBox");
        QVERIFY(targetList);
        QVERIFY(pathEdit);
        QVERIFY(table);
        QVERIFY(searchBox);
        if (selectLocalTargetRowForDrive(targetList, pathEdit, dir.path().left(2).toUpper()) < 0) {
            QSKIP("No mounted local target for the temp drive on this test host.");
        }
        QVERIFY(navigateAndFindRow(pathEdit, table, dir.path(), QStringLiteral("sak-inline")) >= 0);

        // Enter in the quick-search box submits the full search; the results
        // render in the normal listing (Files SubmitSearch), and the query
        // lands in the persisted history.
        searchBox->setText(QStringLiteral("sak-inline-probe"));
        QTest::keyClick(searchBox, Qt::Key_Return);
        QTRY_VERIFY2(
            [table]() {
                for (int row = 0; row < table->model()->rowCount(); ++row) {
                    if (table->model()->index(row, 0).data().toString().contains(
                            QStringLiteral("sak-inline-probe"))) {
                        return true;
                    }
                }
                return false;
            }(),
            "search results did not land in the listing");
        QSettings settings;
        settings.beginGroup(QStringLiteral("FileManagementExplorer"));
        QVERIFY(settings.value(QStringLiteral("SearchHistory"))
                    .toStringList()
                    .contains(QStringLiteral("sak-inline-probe")));
        settings.endGroup();
    }

    void filesCommunityIconRegistryMapsBundledAssets() {
        const QVector<sak::FileExplorerCommandId> mappedCommands = bundledIconMappedCommands();

        for (const auto command : mappedCommands) {
            verifyBundledIconForCommand(command);
        }

        verifyNamedIconDescriptors();
        verifyAllDescriptorIconsRenderVisiblePixels();
        // The bundled icon registry is a fixed compile-time set: 8 file-action + 5 view-layout +
        // 5 view-layout-28 + 8 panel/status + 21 fluent glyphs = 47 descriptors. The old
        // `>= mappedCommands.size()` compared it to the unrelated 16-entry command map, so dropping
        // a whole descriptor group (e.g. the 21 fluent glyphs, leaving 26) stayed green. Pin the
        // exact count so any added/removed descriptor group is caught.
        QCOMPARE(sak::FileExplorerIconRegistry::descriptors().size(), static_cast<qsizetype>(47));
    }

    void explorerIconsAndShellFollowTheApplicationPalette() {
        // M11/M12 lane: the shell is styled through palette() references and the
        // bundled SVGs carry a FIXED dark fill that PaletteTintedIconEngine recolors
        // at paint time. verifyAllDescriptorIconsRenderVisiblePixels only ever ran
        // under the ambient light palette and only asked whether SOME pixel was
        // opaque -- so an engine that ignored the palette entirely, leaving dark ink
        // invisible on the dark theme, passed it. Compare the ink the engine actually
        // chose under each palette.
        ApplicationPaletteGuard guard;
        const auto descriptors = sak::FileExplorerIconRegistry::descriptors();
        QVERIFY(!descriptors.isEmpty());
        IconSheetLayout layout;
        layout.rows = static_cast<int>(descriptors.size());
        QImage sheet = renderIconSheetFrame(descriptors, layout);

        QHash<QString, double> light_ink;
        QApplication::setPalette(guard.original);
        for (const auto& descriptor : descriptors) {
            const QIcon icon = sak::FileExplorerIconRegistry::iconForKey(descriptor.key);
            const double ink = iconInkLuminance(icon.pixmap(24, 24));
            QVERIFY2(ink >= 0.0, qPrintable(descriptor.key));
            light_ink.insert(descriptor.key, ink);
        }
        paintIconSheetColumn(&sheet, descriptors, layout, layout.lightX());

        QApplication::setPalette(darkTestPalette());
        for (const auto& descriptor : descriptors) {
            const QIcon icon = sak::FileExplorerIconRegistry::iconForKey(descriptor.key);
            const double ink = iconInkLuminance(icon.pixmap(24, 24));
            QVERIFY2(ink >= 0.0, qPrintable(descriptor.key));
            // The dark theme's foreground is light, so the ink must get LIGHTER. A
            // generous margin: the claim is "the tint tracked the palette", not a
            // particular shade. Equal values mean the engine ignored the palette.
            QVERIFY2(ink > light_ink.value(descriptor.key) + 32.0,
                     qPrintable(QStringLiteral("%1: light ink %2, dark ink %3")
                                    .arg(descriptor.key)
                                    .arg(light_ink.value(descriptor.key))
                                    .arg(ink)));
        }
        paintIconSheetColumn(&sheet, descriptors, layout, layout.darkX());
        captureBaselineImage(sheet, QStringLiteral("icon-comparison"));

        // The shell resolves through the same palette. Compare what the widgets
        // actually PAINT, not the QPalette they carry: Qt propagates the palette to
        // every child on its own, so reading table->palette() would stay green
        // against a style sheet that hardcoded its colors and rendered the light
        // theme's dark-on-light in dark mode.
        const double dark_ground = renderShellGroundLuminance(QStringLiteral("desktop-dark"));
        QApplication::setPalette(guard.original);
        const double light_ground = renderShellGroundLuminance(QStringLiteral("desktop-light"));
        QVERIFY2(light_ground - dark_ground > 64.0,
                 qPrintable(QStringLiteral("shell ground: light %1, dark %2")
                                .arg(light_ground)
                                .arg(dark_ground)));
    }

    // Build the explorer under the CURRENT application palette, capture it under
    // @p capture_name, and return the mean luminance of its item-view region.
    double renderShellGroundLuminance(const QString& capture_name) {
        sak::FileManagementExplorerPanel panel;
        panel.resize(1280, 800);
        panel.show();
        if (!QTest::qWaitForWindowExposed(&panel)) {
            return -1.0;
        }
        auto* table = child<QTableView>(&panel, "fileExplorerTable");
        if (table == nullptr) {
            return -1.0;
        }
        captureBaseline(&panel, capture_name);
        const QImage rendered =
            table->viewport()->grab().toImage().convertToFormat(QImage::Format_ARGB32);
        if (rendered.isNull() || rendered.width() == 0 || rendered.height() == 0) {
            return -1.0;
        }
        double total = 0.0;
        for (int y = 0; y < rendered.height(); ++y) {
            for (int x = 0; x < rendered.width(); ++x) {
                const QColor color = rendered.pixelColor(x, y);
                total += (0.299 * color.red()) + (0.587 * color.green()) + (0.114 * color.blue());
            }
        }
        return total / static_cast<double>(rendered.width() * rendered.height());
    }

    void statusRowLabelsRenderWithoutClipping() {
        // M12 Visual QA lane ("no clipped text"), made mechanical. A QLabel does not
        // elide -- it clips -- so a status label narrower than its own text loses
        // characters silently. The desktop capture showed exactly that at 1280 with
        // the info pane open: "29 item(s" and "85 item(s) - Local - local fil".
        sak::FileManagementExplorerPanel panel;
        panel.resize(1280, 800);
        panel.show();
        QVERIFY(QTest::qWaitForWindowExposed(&panel));
        auto* targetList = child<QListWidget>(&panel, "fileExplorerTargetList");
        auto* pathEdit = child<QLineEdit>(&panel, "fileExplorerPathEdit");
        auto* table = child<QTableView>(&panel, "fileExplorerTable");
        QVERIFY(targetList);
        QVERIFY(pathEdit);
        QVERIFY(table);
        const int row = firstTargetRow(targetList);
        if (row < 0) {
            QSKIP("No mounted File Explorer targets on this test host.");
        }
        targetList->setCurrentRow(row);
        QVERIFY(waitForListingQuiescence(table));
        QApplication::processEvents();

        for (const QString& name : {QStringLiteral("fileExplorerItemsCountLabel"),
                                    QStringLiteral("fileExplorerSummaryLabel"),
                                    QStringLiteral("fileExplorerStatusLabel")}) {
            auto* label = panel.findChild<QLabel*>(name);
            QVERIFY2(label != nullptr, qPrintable(name));
            if (!label->isVisible() || label->text().trimmed().isEmpty()) {
                continue;
            }
            // Measure the CONTENT box, not the widget: the status labels carry an
            // 8px style-sheet padding on each side, and a sizeHint()-versus-width()
            // check passes while those 16px are eating the last characters.
            const int available = label->contentsRect().width();
            const int needed = label->fontMetrics().horizontalAdvance(label->text());
            QVERIFY2(available >= needed,
                     qPrintable(QStringLiteral("%1 has %2px of content box but needs %3px "
                                               "for \"%4\" (widget %5px, sizeHint %6px)")
                                    .arg(name)
                                    .arg(available)
                                    .arg(needed)
                                    .arg(label->text())
                                    .arg(label->width())
                                    .arg(label->sizeHint().width())));
        }
    }

    void commandBarIconsAndLabelsAreConsistent() {
        // M11 polish lane: the command bar follows Files ToolbarSections -- labelled
        // flyout groups (New, View) and icon-only item commands. "Consistent" is
        // testable: every control carries a registry icon, a tooltip and an
        // accessible name; the icon-only ones carry NO text; and every icon-only
        // glyph in the row renders at the SAME size, which is what stops the row
        // from looking ragged.
        sak::FileManagementExplorerPanel panel;
        panel.resize(1280, 800);
        panel.show();
        QVERIFY(QTest::qWaitForWindowExposed(&panel));

        const QStringList icon_only{QStringLiteral("fileExplorerCutButton"),
                                    QStringLiteral("fileExplorerCopyButton"),
                                    QStringLiteral("fileExplorerPasteButton"),
                                    QStringLiteral("fileExplorerRenameButton"),
                                    QStringLiteral("fileExplorerDeleteButton"),
                                    QStringLiteral("fileExplorerPropertiesButton"),
                                    QStringLiteral("fileExplorerDetailsToggleButton"),
                                    QStringLiteral("fileExplorerSelectionButton"),
                                    QStringLiteral("fileExplorerSortButton")};
        const QStringList labelled{QStringLiteral("fileExplorerNewButton"),
                                   QStringLiteral("fileExplorerViewButton")};

        QSize shared_icon_size;
        for (const QString& name : icon_only) {
            auto* button = panel.findChild<QAbstractButton*>(name);
            QVERIFY2(button != nullptr, qPrintable(name));
            QVERIFY2(!button->icon().isNull(), qPrintable(name));
            QVERIFY2(button->text().isEmpty(),
                     qPrintable(name + QStringLiteral(": ") + button->text()));
            QVERIFY2(!button->toolTip().trimmed().isEmpty(), qPrintable(name));
            QVERIFY2(!button->accessibleName().trimmed().isEmpty(), qPrintable(name));
            // A control with no text must not fall back to a stock/blank glyph.
            QVERIFY2(hasVisiblePixel(button->icon().pixmap(button->iconSize())), qPrintable(name));
            if (!shared_icon_size.isValid()) {
                shared_icon_size = button->iconSize();
            }
            QVERIFY2(button->iconSize() == shared_icon_size,
                     qPrintable(QStringLiteral("%1 renders %2x%3, the rest %4x%5")
                                    .arg(name)
                                    .arg(button->iconSize().width())
                                    .arg(button->iconSize().height())
                                    .arg(shared_icon_size.width())
                                    .arg(shared_icon_size.height())));
        }
        for (const QString& name : labelled) {
            auto* button = panel.findChild<QToolButton*>(name);
            QVERIFY2(button != nullptr, qPrintable(name));
            QVERIFY2(!button->icon().isNull(), qPrintable(name));
            QVERIFY2(!button->text().trimmed().isEmpty(), qPrintable(name));
            QCOMPARE(button->toolButtonStyle(), Qt::ToolButtonTextBesideIcon);
            QVERIFY2(!button->toolTip().trimmed().isEmpty(), qPrintable(name));
            QVERIFY2(!button->accessibleName().trimmed().isEmpty(), qPrintable(name));
            QVERIFY2(button->iconSize() == shared_icon_size,
                     qPrintable(QStringLiteral("%1 renders %2x%3, the icon-only row %4x%5")
                                    .arg(name)
                                    .arg(button->iconSize().width())
                                    .arg(button->iconSize().height())
                                    .arg(shared_icon_size.width())
                                    .arg(shared_icon_size.height())));
        }
    }

    void layoutPickerExposesFunctionalViewModesWithoutMilestoneText() {
        sak::FileManagementExplorerPanel panel;
        panel.resize(1280, 760);
        panel.show();
        QVERIFY(QTest::qWaitForWindowExposed(&panel));

        auto* view = child<QToolButton>(&panel, "fileExplorerViewButton");
        QVERIFY(view);
        QVERIFY(view->menu());

        QStringList actionTexts;
        const auto actions = view->menu()->actions();
        for (const auto* action : actions) {
            if (!action->isSeparator()) {
                actionTexts.append(action->text());
            }
        }

        QVERIFY(containsTextStartingWith(actionTexts, QStringLiteral("Details")));
        QVERIFY(containsTextStartingWith(actionTexts, QStringLiteral("List")));
        QVERIFY(containsTextStartingWith(actionTexts, QStringLiteral("Grid")));
        QVERIFY(containsTextStartingWith(actionTexts, QStringLiteral("Cards")));
        QVERIFY(containsTextStartingWith(actionTexts, QStringLiteral("Columns")));
        QVERIFY(containsTextStartingWith(actionTexts, QStringLiteral("Adaptive")));
        for (int index = 0; index < 6; ++index) {
            QVERIFY(actions.at(index)->isCheckable());
            QVERIFY(!actions.at(index)->icon().isNull());
        }
        QVERIFY(containsTextStartingWith(actionTexts, QStringLiteral("Hidden Items")));
        QVERIFY(containsTextStartingWith(actionTexts, QStringLiteral("File Extensions")));
        QVERIFY(containsTextStartingWith(actionTexts, QStringLiteral("Dual Pane")));
        QVERIFY(containsTextStartingWith(actionTexts, QStringLiteral("Open in New Tab")));
        for (const QString& text : actionTexts) {
            QVERIFY(!text.contains(QStringLiteral("tracked"), Qt::CaseInsensitive));
            QVERIFY(!text.contains(QStringLiteral("M6"), Qt::CaseInsensitive));
            QVERIFY(!text.contains(QStringLiteral("M8"), Qt::CaseInsensitive));
        }

        const auto* listAction = actions.at(1);
        QVERIFY(listAction->isEnabled());
        QVERIFY(listAction->isCheckable());
        QVERIFY(view->menu()->findChild<QSlider*>(QStringLiteral("fileExplorerItemSizeSlider")));
    }

    void layoutPickerRemainsUsableAtNarrowWidth() {
        sak::FileManagementExplorerPanel panel;
        panel.resize(680, 640);
        panel.show();
        QVERIFY(QTest::qWaitForWindowExposed(&panel));

        auto* view = child<QToolButton>(&panel, "fileExplorerViewButton");
        QVERIFY(view);
        QVERIFY(view->menu());
        QVERIFY(view->isVisible());
        QVERIFY(actionStartingWith(view->menu(), QStringLiteral("Details")));
        QVERIFY(actionStartingWith(view->menu(), QStringLiteral("List")));
        QVERIFY(actionStartingWith(view->menu(), QStringLiteral("Grid")));
        QVERIFY(actionStartingWith(view->menu(), QStringLiteral("Cards")));
        QVERIFY(actionStartingWith(view->menu(), QStringLiteral("Columns")));
        QVERIFY(actionStartingWith(view->menu(), QStringLiteral("Adaptive")));
        auto* slider =
            view->menu()->findChild<QSlider*>(QStringLiteral("fileExplorerItemSizeSlider"));
        QVERIFY(slider);
        QVERIFY(slider->minimum() < slider->maximum());
    }

    void viewModesSwitchAndPersistExplorerSettings() {
        sak::FileManagementExplorerPanel panel;
        panel.resize(1280, 760);
        panel.show();
        QVERIFY(QTest::qWaitForWindowExposed(&panel));

        auto* pane = panel.findChild<sak::FileExplorerPane*>();
        auto* view = child<QToolButton>(&panel, "fileExplorerViewButton");
        auto* table = child<QTableView>(&panel, "fileExplorerTable");
        auto* list = child<QListView>(&panel, "fileExplorerListView");
        auto* grid = child<QListView>(&panel, "fileExplorerGridView");
        auto* cards = child<QListView>(&panel, "fileExplorerCardsView");
        auto* columns = child<QListView>(&panel, "fileExplorerColumnsView");
        auto* columnsPreview = child<QListView>(&panel, "fileExplorerColumnsPreviewView");
        QVERIFY(pane);
        QVERIFY(view);
        QVERIFY(view->menu());
        QVERIFY(table);
        QVERIFY(list);
        QVERIFY(grid);
        QVERIFY(cards);
        QVERIFY(columns);
        QVERIFY(columnsPreview);

        auto* listAction = actionStartingWith(view->menu(), QStringLiteral("List"));
        if (!listAction || !listAction->isEnabled()) {
            QSKIP("No mounted File Explorer target available for view mode switching.");
        }

        QVERIFY(waitForListingQuiescence(table));

        if (table->model() && table->model()->rowCount() > 0) {
            table->selectRow(0);
            QCOMPARE(pane->sharedSelectionModel()->selectedRows().size(), 1);
            listAction = actionStartingWith(view->menu(), QStringLiteral("List"));
            QVERIFY(listAction);
            QVERIFY(listAction->isEnabled());
        }

        switchThroughViewModesAndVerifyVisibility(
            pane,
            view,
            listAction,
            ViewModeWidgets{table, list, grid, cards, columns, columnsPreview});
        verifyPersistedAdaptiveViewMode();

        // The pane is in Adaptive mode here, which shares the Grid size kind,
        // so the slider spans the Files GridViewSizeKind range 1..12.
        auto* slider =
            view->menu()->findChild<QSlider*>(QStringLiteral("fileExplorerItemSizeSlider"));
        QVERIFY(slider);
        QCOMPARE(slider->minimum(), 1);
        QCOMPARE(slider->maximum(), 12);
        slider->setValue(12);
        QApplication::processEvents();
        QCOMPARE(pane->layoutSizes().grid, 12);

        auto* extensions = actionStartingWith(view->menu(), QStringLiteral("File Extensions"));
        QVERIFY(extensions);
        QVERIFY(extensions->isChecked());
        extensions->trigger();
        QApplication::processEvents();
        QCOMPARE(pane->showFileExtensions(), false);

        verifyRestoredPaneMatchesPersistedSettings();
    }

    void zoomStepsSizeKindsAndCyclesLayoutRing() {
        sak::FileManagementExplorerPanel panel;
        panel.resize(1280, 760);
        panel.show();
        QVERIFY(QTest::qWaitForWindowExposed(&panel));

        auto* pane = panel.findChild<sak::FileExplorerPane*>();
        auto* table = child<QTableView>(&panel, "fileExplorerTable");
        auto* targetList = child<QListWidget>(&panel, "fileExplorerTargetList");
        QVERIFY(pane);
        QVERIFY(table);
        QVERIFY(targetList);
        if (firstTargetRow(targetList) < 0) {
            QSKIP("No mounted File Explorer targets on this test host.");
        }
        panel.activateWindow();
        table->setFocus();
        QApplication::processEvents();

        // Files LayoutSizeKindHelper: Details Small (kind 2) is a 36px row.
        QTest::keyClick(table, Qt::Key_1, Qt::ControlModifier | Qt::ShiftModifier);
        QTRY_COMPARE(pane->viewMode(), sak::FileExplorerViewMode::Details);
        QCOMPARE(pane->layoutSizes().details, 2);
        QCOMPARE(table->verticalHeader()->defaultSectionSize(), 36);
        const int gridKindBefore = pane->layoutSizes().grid;

        // In-bounds zoom-out steps the kind: Small -> Compact (28px row).
        QTest::keyClick(table, Qt::Key_Minus, Qt::ControlModifier);
        QTRY_COMPARE(pane->layoutSizes().details, 1);
        QCOMPARE(table->verticalHeader()->defaultSectionSize(), 28);

        // Files LayoutCycler: zoom-out below Details Compact wraps backward
        // to Columns at its maximum kind.
        QTest::keyClick(table, Qt::Key_Minus, Qt::ControlModifier);
        QTRY_COMPARE(pane->viewMode(), sak::FileExplorerViewMode::Columns);
        QCOMPARE(pane->layoutSizes().columns, 5);

        // Zoom-in past Columns ExtraLarge wraps forward to Details at its
        // minimum kind; the Details kind set above is overwritten.
        QTest::keyClick(table, Qt::Key_Equal, Qt::ControlModifier);
        QTRY_COMPARE(pane->viewMode(), sak::FileExplorerViewMode::Details);
        QCOMPARE(pane->layoutSizes().details, 1);

        // Ctrl+mouse-wheel up zooms in one kind (BaseLayoutViewModel wheel).
        QWheelEvent wheelUp(QPointF(10, 10),
                            table->viewport()->mapToGlobal(QPoint(10, 10)),
                            QPoint(),
                            QPoint(0, 120),
                            Qt::NoButton,
                            Qt::ControlModifier,
                            Qt::NoScrollPhase,
                            false);
        QApplication::sendEvent(table->viewport(), &wheelUp);
        QTRY_COMPARE(pane->layoutSizes().details, 2);

        // Each layout keeps an independent kind: the Grid kind is untouched
        // by Details/Columns zooming.
        QCOMPARE(pane->layoutSizes().grid, gridKindBefore);
    }

    void groupByMenuGroupsRowsWithHeadersAndPersists() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const auto writeBytes = [](const QString& path, const int size) {
            QFile file(path);
            if (!file.open(QIODevice::WriteOnly)) {
                return false;
            }
            return file.write(QByteArray(size, 'x')) == size;
        };
        QVERIFY(writeBytes(dir.filePath(QStringLiteral("alpha.txt")), 10));
        QVERIFY(writeBytes(dir.filePath(QStringLiteral("beta.bin")), 42));
        QVERIFY(QDir(dir.path()).mkdir(QStringLiteral("stuff")));

        sak::FileManagementExplorerPanel panel;
        panel.resize(1280, 760);
        panel.show();
        QVERIFY(QTest::qWaitForWindowExposed(&panel));

        auto* pane = panel.findChild<sak::FileExplorerPane*>();
        auto* table = child<QTableView>(&panel, "fileExplorerTable");
        auto* pathEdit = child<QLineEdit>(&panel, "fileExplorerPathEdit");
        auto* targetList = child<QListWidget>(&panel, "fileExplorerTargetList");
        QVERIFY(pane);
        QVERIFY(table);
        QVERIFY(pathEdit);
        QVERIFY(targetList);
        if (selectLocalTargetRowForDrive(targetList, pathEdit, dir.path().left(2).toUpper()) < 0) {
            QSKIP("No local File Explorer target for the temp drive on this host.");
        }
        QVERIFY(navigateAndFindRow(pathEdit, table, dir.path(), QStringLiteral("alpha")) >= 0);

        // Group by Size from the sort flyout (the Files toolbar grouping
        // surface): sections become Folders and Tiny with header rows.
        triggerGroupActionInSortFlyout(panel, QStringLiteral("Size"));

        // 3 items plus 2 injected section headers.
        QTRY_COMPARE(table->model()->rowCount(), 5);
        QCOMPARE(pane->groupProxyModel()->headerRows(), (QVector<int>{0, 2}));
        QCOMPARE(table->model()->index(0, 0).data().toString(), QStringLiteral("Folders"));
        QVERIFY(table->model()->index(2, 0).data().toString().startsWith(QStringLiteral("Tiny")));
        // Details header rows span the full column set (Files full-width
        // group headers).
        QCOMPARE(table->columnSpan(0, 0), table->model()->columnCount());
        QVERIFY(!pane->hasViewEntry(0));
        QVERIFY(pane->hasViewEntry(1));
        QCOMPARE(pane->entryAtViewRow(1).name, QStringLiteral("stuff"));

        verifyGroupedSelectionPersistenceAndReset(panel, pane, table);
    }

    void triggerGroupActionInSortFlyout(sak::FileManagementExplorerPanel& panel,
                                        const QString& prefix) {
        auto* sortButton = child<QToolButton>(&panel, "fileExplorerSortButton");
        QVERIFY(sortButton);
        QVERIFY(sortButton->menu());
        sortButton->menu()->popup(QPoint(10, 10));
        QApplication::processEvents();
        auto* groupMenu =
            sortButton->menu()->findChild<QMenu*>(QStringLiteral("fileExplorerGroupBySubmenu"));
        QVERIFY(groupMenu);
        QAction* action = actionStartingWith(groupMenu, prefix);
        QVERIFY(action);
        action->trigger();
        sortButton->menu()->hide();
        QApplication::processEvents();
    }

    void verifyGroupedSelectionPersistenceAndReset(sak::FileManagementExplorerPanel& panel,
                                                   sak::FileExplorerPane* pane,
                                                   QTableView* table) {
        // Select-all collects only real items, never the header rows.
        table->selectAll();
        QApplication::processEvents();
        int real_rows = 0;
        const QModelIndexList selected = pane->sharedSelectionModel()->selectedRows();
        for (const QModelIndex& index : selected) {
            if (pane->hasViewEntry(index.row())) {
                ++real_rows;
            }
        }
        QCOMPARE(real_rows, 3);

        // The grouping preference persists per-location under the Files
        // setting name.
        QSettings settings;
        settings.beginGroup(QStringLiteral("FileManagementExplorer"));
        settings.beginGroup(QStringLiteral("View"));
        bool found = false;
        const QStringList groups = settings.childGroups();
        for (const QString& group : groups) {
            settings.beginGroup(group);
            if (settings.value(QStringLiteral("GroupOption")).toString() ==
                QStringLiteral("size")) {
                found = true;
            }
            settings.endGroup();
        }
        QVERIFY(found);

        // None restores the flat listing.
        triggerGroupActionInSortFlyout(panel, QStringLiteral("None"));
        QTRY_COMPARE(table->model()->rowCount(), 3);
        QVERIFY(pane->groupProxyModel()->headerRows().isEmpty());
    }

    void undoRedoRevertsRenameAndSameTargetMove() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QDir root(dir.path());
        {
            QFile file(root.filePath(QStringLiteral("undo_src.txt")));
            QVERIFY(file.open(QIODevice::WriteOnly));
            QVERIFY(file.write("undo payload") > 0);
        }
        QVERIFY(root.mkdir(QStringLiteral("pocket")));

        sak::FileManagementExplorerPanel panel;
        panel.resize(1100, 700);
        panel.show();
        QVERIFY(QTest::qWaitForWindowExposed(&panel));
        auto* targetList = child<QListWidget>(&panel, "fileExplorerTargetList");
        auto* pathEdit = child<QLineEdit>(&panel, "fileExplorerPathEdit");
        auto* table = child<QTableView>(&panel, "fileExplorerTable");
        QVERIFY(targetList);
        QVERIFY(pathEdit);
        QVERIFY(table);
        if (selectLocalTargetRowForDrive(targetList, pathEdit, dir.path().left(2).toUpper()) < 0) {
            QSKIP("No mounted local target for the temp drive on this test host.");
        }
        QVERIFY(navigateAndFindRow(pathEdit, table, dir.path(), QStringLiteral("undo_src")) >= 0);

        // Rename through the inline editor, then Ctrl+Z / Ctrl+Y walk the
        // journal (Files UndoAction/RedoAction over the Rename inverse).
        QVERIFY(selectRowStable(table, QStringLiteral("undo_src")));
        QTest::keyClick(&panel, Qt::Key_F2);
        auto* editor = table->findChild<QLineEdit*>(QStringLiteral("fileExplorerRenameEditor"));
        QVERIFY(editor);
        QTRY_COMPARE(editor->selectedText(), QStringLiteral("undo_src"));
        QTest::keyClicks(editor, QStringLiteral("renamed"));
        QTest::keyClick(editor, Qt::Key_Return);
        QTRY_VERIFY(QFile::exists(root.filePath(QStringLiteral("renamed.txt"))));
        QVERIFY(!QFile::exists(root.filePath(QStringLiteral("undo_src.txt"))));

        panel.activateWindow();
        table->setFocus();
        QApplication::processEvents();
        QTest::keyClick(table, Qt::Key_Z, Qt::ControlModifier);
        QTRY_VERIFY(QFile::exists(root.filePath(QStringLiteral("undo_src.txt"))));
        QVERIFY(!QFile::exists(root.filePath(QStringLiteral("renamed.txt"))));
        QTest::keyClick(table, Qt::Key_Y, Qt::ControlModifier);
        QTRY_VERIFY(QFile::exists(root.filePath(QStringLiteral("renamed.txt"))));

        verifyUndoRedoOfSameTargetMove(panel, table, root);
    }

    void verifyUndoRedoOfSameTargetMove(sak::FileManagementExplorerPanel& panel,
                                        QTableView* table,
                                        const QDir& root) {
        // Cut + paste-into-selection moves into the subfolder; Ctrl+Z moves
        // it back through the same-target rename inverse, Ctrl+Y replays it.
        QTRY_VERIFY(selectRowStable(table, QStringLiteral("renamed")));
        QTest::keyClick(table, Qt::Key_X, Qt::ControlModifier);
        QVERIFY(selectRowStable(table, QStringLiteral("pocket")));
        QTest::keyClick(table, Qt::Key_V, Qt::ControlModifier | Qt::ShiftModifier);
        const QString moved = root.filePath(QStringLiteral("pocket/renamed.txt"));
        QTRY_VERIFY(QFile::exists(moved));
        QVERIFY(!QFile::exists(root.filePath(QStringLiteral("renamed.txt"))));
        // The move ran on the transfer worker; its completion records the
        // undo history, so wait for the worker to settle before Ctrl+Z.
        QTRY_COMPARE(panel.statusCenterModel()->inProgressCount(), 0);

        panel.activateWindow();
        table->setFocus();
        QApplication::processEvents();
        QTest::keyClick(table, Qt::Key_Z, Qt::ControlModifier);
        QTRY_VERIFY(QFile::exists(root.filePath(QStringLiteral("renamed.txt"))));
        QVERIFY(!QFile::exists(moved));
        QTest::keyClick(table, Qt::Key_Y, Qt::ControlModifier);
        QTRY_VERIFY(QFile::exists(moved));
        QVERIFY(!QFile::exists(root.filePath(QStringLiteral("renamed.txt"))));
    }

    void undoDeletesCopiesAndCreatedFoldersWithConfirmation() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QDir root(dir.path());
        {
            QFile file(root.filePath(QStringLiteral("seed.txt")));
            QVERIFY(file.open(QIODevice::WriteOnly));
            QVERIFY(file.write("copy payload") > 0);
        }

        sak::FileManagementExplorerPanel panel;
        panel.resize(1100, 700);
        panel.show();
        QVERIFY(QTest::qWaitForWindowExposed(&panel));
        auto* targetList = child<QListWidget>(&panel, "fileExplorerTargetList");
        auto* pathEdit = child<QLineEdit>(&panel, "fileExplorerPathEdit");
        auto* table = child<QTableView>(&panel, "fileExplorerTable");
        QVERIFY(targetList);
        QVERIFY(pathEdit);
        QVERIFY(table);
        if (selectLocalTargetRowForDrive(targetList, pathEdit, dir.path().left(2).toUpper()) < 0) {
            QSKIP("No mounted local target for the temp drive on this test host.");
        }
        QVERIFY(navigateAndFindRow(pathEdit, table, dir.path(), QStringLiteral("seed")) >= 0);
        panel.activateWindow();
        table->setFocus();
        QApplication::processEvents();

        // Same-folder copy-paste duplicates; undoing the copy deletes the
        // duplicate after the Files forced-confirmation dialog, and redo
        // copies it again from the original source.
        QVERIFY(selectRowStable(table, QStringLiteral("seed")));
        QTest::keyClick(table, Qt::Key_C, Qt::ControlModifier);
        QTest::keyClick(table, Qt::Key_V, Qt::ControlModifier);
        const QString copy_path = root.filePath(QStringLiteral("seed (2).txt"));
        QTRY_VERIFY(QFile::exists(copy_path));
        // The copy ran on the transfer worker; wait for its completion (which
        // records the undo history) before Ctrl+Z.
        QTRY_COMPARE(panel.statusCenterModel()->inProgressCount(), 0);

        panel.activateWindow();
        table->setFocus();
        QApplication::processEvents();
        QString question_text;
        armAutoAcceptQuestion(&question_text);
        QTest::keyClick(table, Qt::Key_Z, Qt::ControlModifier);
        QTRY_VERIFY(!QFile::exists(copy_path));
        // Both undo headers contain "delete", so the create wording -- a different count
        // noun and a second scope note -- passed the fragment unchanged.
        verifyUndoConfirmation(question_text,
                               QStringLiteral("Undoing this copy will delete 1 copied item(s):"),
                               QStringLiteral("These will be moved to the Recycle Bin."));
        // The dismissed confirmation cleared focus; the focus-scoped Ctrl+Y
        // shortcut needs it back.
        panel.activateWindow();
        table->setFocus();
        QApplication::processEvents();
        QTest::keyClick(table, Qt::Key_Y, Qt::ControlModifier);
        QTRY_VERIFY(QFile::exists(copy_path));

        verifyUndoRedoOfNewFolder(panel, table, root);
    }

    void verifyUndoRedoOfNewFolder(sak::FileManagementExplorerPanel& panel,
                                   QTableView* table,
                                   const QDir& root) {
        // Ctrl+Shift+N creates "New Folder" (the input dialog auto-accepts
        // its default text); Ctrl+Z deletes the created folder after the
        // confirmation, Ctrl+Y recreates it through the bridge.
        panel.activateWindow();
        table->setFocus();
        QApplication::processEvents();
        QString input_label;
        armAutoAcceptInputDialog(&input_label);
        QTest::keyClick(table, Qt::Key_N, Qt::ControlModifier | Qt::ShiftModifier);
        const QString folder_path = root.filePath(QStringLiteral("New Folder"));
        QTRY_VERIFY(QFileInfo(folder_path).isDir());

        panel.activateWindow();
        table->setFocus();
        QApplication::processEvents();
        QString question_text;
        armAutoAcceptQuestion(&question_text);
        QTest::keyClick(table, Qt::Key_Z, Qt::ControlModifier);
        QTRY_VERIFY(!QFileInfo::exists(folder_path));
        // Ctrl+Shift+N journals a directory, so the scope must carry the second note; the
        // fragment could not see it go missing.
        verifyUndoConfirmation(
            question_text,
            QStringLiteral("Undoing this create will delete 1 item(s):"),
            QStringLiteral("These will be moved to the Recycle Bin.<br/>Any folder is removed "
                           "together with its entire contents."));
        // Re-focus after the dismissed confirmation before the redo shortcut.
        panel.activateWindow();
        table->setFocus();
        QApplication::processEvents();
        QTest::keyClick(table, Qt::Key_Y, Qt::ControlModifier);
        QTRY_VERIFY(QFileInfo(folder_path).isDir());
    }

    void checkboxSettingTogglesSelectionAndFlattenMovesDescendants() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QDir root(dir.path());
        QVERIFY(root.mkpath(QStringLiteral("flatten_me/sub/deep")));
        const auto writeText = [](const QString& path) {
            QFile file(path);
            if (!file.open(QIODevice::WriteOnly)) {
                return false;
            }
            return file.write("flatten payload") > 0;
        };
        QVERIFY(writeText(root.filePath(QStringLiteral("flatten_me/top.txt"))));
        QVERIFY(writeText(root.filePath(QStringLiteral("flatten_me/sub/inner.txt"))));
        QVERIFY(writeText(root.filePath(QStringLiteral("flatten_me/sub/deep/leaf.txt"))));
        {
            // Files ShowFlattenOptions (experimental, default off): opt in.
            QSettings settings;
            settings.beginGroup(QStringLiteral("FileManagementExplorer"));
            settings.setValue(QStringLiteral("ShowFlattenOptions"), true);
            settings.endGroup();
        }

        sak::FileManagementExplorerPanel panel;
        panel.resize(1100, 700);
        panel.show();
        QVERIFY(QTest::qWaitForWindowExposed(&panel));
        auto* pane = panel.findChild<sak::FileExplorerPane*>();
        auto* targetList = child<QListWidget>(&panel, "fileExplorerTargetList");
        auto* pathEdit = child<QLineEdit>(&panel, "fileExplorerPathEdit");
        auto* table = child<QTableView>(&panel, "fileExplorerTable");
        QVERIFY(pane);
        QVERIFY(targetList);
        QVERIFY(pathEdit);
        QVERIFY(table);
        if (selectLocalTargetRowForDrive(targetList, pathEdit, dir.path().left(2).toUpper()) < 0) {
            QSKIP("No mounted local target for the temp drive on this test host.");
        }
        QVERIFY(navigateAndFindRow(pathEdit, table, dir.path(), QStringLiteral("flatten_me")) >= 0);

        // Checkboxes default ON (Files ShowCheckboxesWhenSelectingItems):
        // checking a row selects it, unchecking clears it.
        QAbstractItemModel* view_model = table->model();
        QVERIFY(view_model->flags(view_model->index(0, 0)).testFlag(Qt::ItemIsUserCheckable));
        QVERIFY(view_model->setData(view_model->index(0, 0), Qt::Checked, Qt::CheckStateRole));
        QTRY_VERIFY(pane->sharedSelectionModel()->isRowSelected(0, QModelIndex()));
        QCOMPARE(view_model->index(0, 0).data(Qt::CheckStateRole).value<Qt::CheckState>(),
                 Qt::Checked);
        QVERIFY(view_model->setData(view_model->index(0, 0), Qt::Unchecked, Qt::CheckStateRole));
        QTRY_VERIFY(!pane->sharedSelectionModel()->isRowSelected(0, QModelIndex()));

        // The View-menu toggle turns the boxes off.
        auto* view_button = child<QToolButton>(&panel, "fileExplorerViewButton");
        QVERIFY(view_button);
        QVERIFY(view_button->menu());
        QAction* checkboxes = actionStartingWith(view_button->menu(),
                                                 QStringLiteral("Item Check Boxes"));
        QVERIFY(checkboxes);
        QVERIFY(checkboxes->isChecked());
        checkboxes->trigger();
        QApplication::processEvents();
        QVERIFY(!view_model->flags(view_model->index(0, 0)).testFlag(Qt::ItemIsUserCheckable));

        verifyFlattenFolderMovesDescendants(panel, table, root);
    }

    void verifyFlattenFolderMovesDescendants(sak::FileManagementExplorerPanel& panel,
                                             QTableView* table,
                                             const QDir& root) {
        // Flatten folder (Files FlattenFolderAction): descendants move up
        // into the selected folder, emptied subfolders are removed.
        QVERIFY(selectRowStable(table, QStringLiteral("flatten_me")));
        QString question_text;
        armAutoAcceptQuestion(&question_text);
        QVERIFY(QMetaObject::invokeMethod(&panel, "flattenSelectedFolder", Qt::DirectConnection));
        QTRY_VERIFY(QFile::exists(root.filePath(QStringLiteral("flatten_me/inner.txt"))));
        QVERIFY(QFile::exists(root.filePath(QStringLiteral("flatten_me/leaf.txt"))));
        QVERIFY(QFile::exists(root.filePath(QStringLiteral("flatten_me/top.txt"))));
        QTRY_VERIFY(!QFileInfo::exists(root.filePath(QStringLiteral("flatten_me/sub"))));
        QVERIFY2(question_text.contains(QStringLiteral("Flatten"), Qt::CaseInsensitive),
                 qPrintable(question_text));
    }

    void cardsAndColumnsDelegatesRenderFilesCells() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QVERIFY(QDir(dir.path()).mkdir(QStringLiteral("folder_row")));
        {
            QFile file(QDir(dir.path()).filePath(QStringLiteral("card_row.txt")));
            QVERIFY(file.open(QIODevice::WriteOnly));
            QVERIFY(file.write("card payload") > 0);
        }

        sak::FileManagementExplorerPanel panel;
        panel.resize(1100, 700);
        panel.show();
        QVERIFY(QTest::qWaitForWindowExposed(&panel));
        auto* targetList = child<QListWidget>(&panel, "fileExplorerTargetList");
        auto* pathEdit = child<QLineEdit>(&panel, "fileExplorerPathEdit");
        auto* table = child<QTableView>(&panel, "fileExplorerTable");
        auto* cards = child<QListView>(&panel, "fileExplorerCardsView");
        auto* columns = child<QListView>(&panel, "fileExplorerColumnsView");
        auto* preview = child<QListView>(&panel, "fileExplorerColumnsPreviewView");
        QVERIFY(targetList);
        QVERIFY(pathEdit);
        QVERIFY(table);
        QVERIFY(cards);
        QVERIFY(columns);
        QVERIFY(preview);

        // Files CardsBrowserTemplate delegate on Cards; the columns blades
        // carry the folder-chevron variant (preview blade included).
        QVERIFY(qobject_cast<sak::FileExplorerCardDelegate*>(cards->itemDelegate()));
        auto* columns_delegate =
            qobject_cast<sak::FileExplorerNameDelegate*>(columns->itemDelegate());
        QVERIFY(columns_delegate);
        QVERIFY(columns_delegate->folderChevronEnabled());
        auto* preview_delegate =
            qobject_cast<sak::FileExplorerNameDelegate*>(preview->itemDelegate());
        QVERIFY(preview_delegate);
        QVERIFY(preview_delegate->folderChevronEnabled());
        // Grid/List keep the plain rename delegate (no chevron, no card).
        auto* grid = child<QListView>(&panel, "fileExplorerGridView");
        QVERIFY(grid);
        auto* grid_delegate = qobject_cast<sak::FileExplorerNameDelegate*>(grid->itemDelegate());
        QVERIFY(grid_delegate);
        QVERIFY(!grid_delegate->folderChevronEnabled());
        QVERIFY(!qobject_cast<sak::FileExplorerCardDelegate*>(grid->itemDelegate()));

        if (selectLocalTargetRowForDrive(targetList, pathEdit, dir.path().left(2).toUpper()) < 0) {
            QSKIP("No mounted local target for the temp drive on this test host.");
        }
        QVERIFY(navigateAndFindRow(pathEdit, table, dir.path(), QStringLiteral("card_row")) >= 0);

        // Paint smoke offscreen: both custom paints execute over real rows.
        auto* view_button = child<QToolButton>(&panel, "fileExplorerViewButton");
        QVERIFY(view_button);
        QVERIFY(view_button->menu());
        QAction* cards_action = actionStartingWith(view_button->menu(), QStringLiteral("Cards"));
        QVERIFY(cards_action);
        cards_action->trigger();
        QTRY_VERIFY(cards->isVisible());
        QVERIFY(!cards->grab().isNull());
        QAction* columns_action = actionStartingWith(view_button->menu(),
                                                     QStringLiteral("Columns"));
        QVERIFY(columns_action);
        columns_action->trigger();
        QTRY_VERIFY(columns->isVisible());
        QVERIFY(!columns->grab().isNull());
    }

    void settingsDialogPersistsAndAppliesExplorerToggles() {
        sak::FileManagementExplorerPanel panel;
        panel.resize(1100, 700);
        panel.show();
        QVERIFY(QTest::qWaitForWindowExposed(&panel));

        auto* pane = panel.findChild<sak::FileExplorerPane*>();
        auto* gear = child<QPushButton>(&panel, "fileExplorerSettingsButton");
        auto* filterHeader = child<QWidget>(&panel, "fileExplorerFilterHeader");
        QVERIFY(pane);
        QVERIFY(gear);
        QVERIFY(filterHeader);
        QVERIFY(pane->itemModel()->checkboxesVisible());  // Files default ON
        QVERIFY(!filterHeader->isVisible());              // Files default OFF

        // Drive the dialog: uncheck the selection checkboxes, enable the
        // filter header and the flatten opt-in, then accept.
        bool dialog_seen = false;
        QTimer::singleShot(0, [&dialog_seen]() {
            auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
            if (!dialog || dialog->objectName() != QStringLiteral("fileExplorerSettingsDialog")) {
                return;
            }
            dialog_seen = true;
            dialog->findChild<QCheckBox*>(QStringLiteral("fileExplorerSettingsCheckboxes"))
                ->setChecked(false);
            dialog->findChild<QCheckBox*>(QStringLiteral("fileExplorerSettingsFilterHeader"))
                ->setChecked(true);
            dialog->findChild<QCheckBox*>(QStringLiteral("fileExplorerSettingsFlatten"))
                ->setChecked(true);
            dialog->accept();
        });
        gear->click();
        QVERIFY(dialog_seen);

        // Applied live and persisted under the Files setting names.
        QTRY_VERIFY(!pane->itemModel()->checkboxesVisible());
        QTRY_VERIFY(filterHeader->isVisible());
        QSettings settings;
        settings.beginGroup(QStringLiteral("FileManagementExplorer"));
        QVERIFY(!settings.value(QStringLiteral("ShowCheckboxesWhenSelectingItems"), true).toBool());
        QVERIFY(settings.value(QStringLiteral("ShowFilterHeader"), false).toBool());
        QVERIFY(settings.value(QStringLiteral("ShowFlattenOptions"), false).toBool());
        QVERIFY(settings.value(QStringLiteral("DoubleClickToGoUp"), true).toBool());
        settings.endGroup();
    }

    void sidebarAcceptsTagDropAndFavoriteReorder() {
        // Seed a known tag and two favorites BEFORE the panel builds its
        // sidebar, so the Tags section and Favorites section render rows.
        const auto mounted = sak::FileManagementFileSystemBridge::mountedTargets();
        if (mounted.size() < 2) {
            QSKIP("Need two mounted targets for the favorites reorder leg.");
        }
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        {
            QFile file(QDir(dir.path()).filePath(QStringLiteral("tagme.txt")));
            QVERIFY(file.open(QIODevice::WriteOnly));
            QVERIFY(file.write("tag payload") > 0);
        }
        {
            QSettings settings;
            settings.beginGroup(QStringLiteral("FileManagementExplorer"));
            settings.setValue(QStringLiteral("FavoriteTargetIds"),
                              QStringList{mounted.at(0).id, mounted.at(1).id});
            settings.endGroup();
            sak::FileExplorerTagStore::setTags(settings,
                                               QStringLiteral("FileManagementExplorer/Tags"),
                                               mounted.at(0).id,
                                               QStringLiteral("/tag-seed"),
                                               {QStringLiteral("crimson")});
        }

        sak::FileManagementExplorerPanel panel;
        panel.resize(1100, 700);
        panel.show();
        QVERIFY(QTest::qWaitForWindowExposed(&panel));
        auto* targetList = child<QListWidget>(&panel, "fileExplorerTargetList");
        auto* pathEdit = child<QLineEdit>(&panel, "fileExplorerPathEdit");
        auto* table = child<QTableView>(&panel, "fileExplorerTable");
        QVERIFY(targetList);
        QVERIFY(pathEdit);
        QVERIFY(table);
        if (selectLocalTargetRowForDrive(targetList, pathEdit, dir.path().left(2).toUpper()) < 0) {
            QSKIP("No mounted local target for the temp drive on this test host.");
        }
        QVERIFY(navigateAndFindRow(pathEdit, table, dir.path(), QStringLiteral("tagme")) >= 0);

        verifySidebarTagDrop(panel, targetList, table, dir.path());
        verifySidebarFavoriteReorder(targetList, mounted.at(0).id, mounted.at(1).id);
    }

    void verifySidebarTagDrop(sak::FileManagementExplorerPanel& panel,
                              QListWidget* targetList,
                              QTableView* table,
                              const QString& dir_path) {
        Q_UNUSED(panel);
        // Copy the file to build the internal drag payload, then drop it on
        // the "crimson" tag row (Files HandleTagItemDroppedAsync).
        QVERIFY(selectRowStable(table, QStringLiteral("tagme")));
        QTest::keyClick(table, Qt::Key_C, Qt::ControlModifier);
        const QMimeData* clip = QApplication::clipboard()->mimeData();
        QVERIFY(clip);
        QVERIFY(clip->hasFormat(QStringLiteral("application/x-sak-file-explorer-items")));
        QMimeData drag_mime;
        drag_mime.setData(QStringLiteral("application/x-sak-file-explorer-items"),
                          clip->data(QStringLiteral("application/x-sak-file-explorer-items")));
        const QString payload_target =
            QJsonDocument::fromJson(clip->data(QStringLiteral("application/"
                                                              "x-sak-file-explorer-items")))
                .object()
                .value(QStringLiteral("target"))
                .toString();
        QVERIFY(!payload_target.isEmpty());

        QListWidgetItem* tag_row = nullptr;
        for (int row = 0; row < targetList->count(); ++row) {
            auto* item = targetList->item(row);
            // SidebarEntryKind::Tag == 3 (kSidebarKindRole = UserRole + 1).
            if (item->data(Qt::UserRole + 1).toInt() == 3 &&
                item->data(Qt::UserRole + 7).toString() == QStringLiteral("crimson")) {
                tag_row = item;
                break;
            }
        }
        QVERIFY2(tag_row, "tag row did not render in the sidebar");
        const QPoint drop_pos = targetList->visualItemRect(tag_row).center();
        QDragEnterEvent enter(drop_pos,
                              Qt::CopyAction | Qt::MoveAction | Qt::LinkAction,
                              &drag_mime,
                              Qt::NoButton,
                              Qt::NoModifier);
        QVERIFY(QApplication::sendEvent(targetList->viewport(), &enter));
        QVERIFY(enter.isAccepted());
        QDropEvent drop(drop_pos,
                        Qt::CopyAction | Qt::MoveAction | Qt::LinkAction,
                        &drag_mime,
                        Qt::NoButton,
                        Qt::NoModifier);
        QVERIFY(QApplication::sendEvent(targetList->viewport(), &drop));

        QSettings settings;
        const QString tagged_path =
            QDir::fromNativeSeparators(QDir(dir_path).filePath(QStringLiteral("tagme.txt")));
        // The entry is new for this run and carried no prior tags, so the drop leaves exactly
        // one. A case-insensitive membership probe also passes on a case-mangled "CRIMSON" or
        // on crimson plus unrelated tags harvested out of the drag payload.
        QTRY_COMPARE(sak::FileExplorerTagStore::tagsFor(settings,
                                                        QStringLiteral("FileManagementExplorer/"
                                                                       "Tags"),
                                                        payload_target,
                                                        tagged_path),
                     (QStringList{QStringLiteral("crimson")}));
    }

    void verifySidebarFavoriteReorder(QListWidget* targetList,
                                      const QString& first_id,
                                      const QString& second_id) {
        // Drag favorite 0 onto favorite 1 (kSidebarFavoritePosRole =
        // UserRole + 8): the pin order flips and persists.
        QListWidgetItem* second_favorite = nullptr;
        for (int row = 0; row < targetList->count(); ++row) {
            auto* item = targetList->item(row);
            const QVariant position = item->data(Qt::UserRole + 8);
            if (!position.isNull() && position.toInt() == 1) {
                second_favorite = item;
                break;
            }
        }
        QVERIFY2(second_favorite, "second favorite row did not render");
        QMimeData favorite_mime;
        favorite_mime.setData(QStringLiteral("application/x-sak-explorer-favorite"),
                              QByteArrayLiteral("0"));
        const QPoint drop_pos = targetList->visualItemRect(second_favorite).center();
        QDragEnterEvent enter(
            drop_pos, Qt::MoveAction, &favorite_mime, Qt::NoButton, Qt::NoModifier);
        QVERIFY(QApplication::sendEvent(targetList->viewport(), &enter));
        QVERIFY(enter.isAccepted());
        QDropEvent drop(drop_pos, Qt::MoveAction, &favorite_mime, Qt::NoButton, Qt::NoModifier);
        QVERIFY(QApplication::sendEvent(targetList->viewport(), &drop));

        QSettings settings;
        settings.beginGroup(QStringLiteral("FileManagementExplorer"));
        const QStringList order =
            settings.value(QStringLiteral("FavoriteTargetIds")).toStringList();
        settings.endGroup();
        QCOMPARE(order, (QStringList{second_id, first_id}));
    }

    // The parameter keeps this helper out of QtTest's parameterless-slot
    // test discovery (same pattern as armAutoAcceptQuestion).
    void armAutoAcceptInputDialog(QString* captured_label) {
        auto* accept = new QTimer(this);
        accept->setInterval(50);
        connect(accept, &QTimer::timeout, this, [accept, captured_label]() {
            if (auto* dialog = qobject_cast<QInputDialog*>(QApplication::activeModalWidget())) {
                *captured_label = dialog->labelText();
                accept->stop();
                accept->deleteLater();
                dialog->accept();
            }
        });
        accept->start();
    }

    void sidebarGroupsExposeFilesLikeTargets() {
        sak::FileManagementExplorerPanel panel;
        panel.resize(1280, 760);
        panel.show();
        QVERIFY(QTest::qWaitForWindowExposed(&panel));

        auto* targetList = child<QListWidget>(&panel, "fileExplorerTargetList");
        QVERIFY(targetList);

        QStringList labels;
        for (int row = 0; row < targetList->count(); ++row) {
            labels.append(targetList->item(row)->text());
        }

        QVERIFY(labels.contains(QStringLiteral("Home")));
        QVERIFY(labels.contains(QStringLiteral("Favorites")));
        QVERIFY(labels.contains(QStringLiteral("This PC")));
        QVERIFY(labels.contains(QStringLiteral("Mounted Volumes")));
        QVERIFY(labels.contains(QStringLiteral("Disks and Partitions")));
        QVERIFY(labels.contains(QStringLiteral("Raw Images")));
        QVERIFY(labels.contains(QStringLiteral("Recent")));
        QVERIFY(labels.contains(QStringLiteral("Certification Targets")));
    }

    void sidebarTargetRowsAreSingleLineWithTheDetailInTheTooltip() {
        // M11 density lane, measured against Files SidebarStyles.xaml: the sidebar
        // item template is ONE line (TextWrapping="NoWrap", CharacterEllipsis). A
        // second line in the display string is not merely off-spec -- Qt's item view
        // never renders it, so it silently became an ellipsis and made every row read
        // as a truncated badge. The detail belongs in the tooltip, where it is
        // actually reachable.
        sak::FileManagementExplorerPanel panel;
        panel.resize(1280, 760);
        panel.show();
        QVERIFY(QTest::qWaitForWindowExposed(&panel));
        auto* targetList = child<QListWidget>(&panel, "fileExplorerTargetList");
        QVERIFY(targetList);
        const int row = firstTargetRow(targetList);
        if (row < 0) {
            QSKIP("No mounted File Explorer targets on this test host.");
        }

        int checked = 0;
        for (int index = 0; index < targetList->count(); ++index) {
            const auto* item = targetList->item(index);
            QVERIFY2(!item->text().contains(QLatin1Char('\n')),
                     qPrintable(QStringLiteral("sidebar row %1 is multi-line: \"%2\"")
                                    .arg(index)
                                    .arg(item->text())));
            if (item->flags().testFlag(Qt::ItemIsSelectable) &&
                item->text().contains(QStringLiteral("  ["))) {
                // The row still says WHICH target and what it may do; the rest --
                // identity path, capability summary, and the mounted/raw subtitle --
                // is in the tooltip.
                QVERIFY2(!item->toolTip().trimmed().isEmpty(), qPrintable(item->text()));
                QVERIFY2(item->toolTip().contains(QStringLiteral("mounted")) ||
                             item->toolTip().contains(QStringLiteral("raw/image")),
                         qPrintable(item->toolTip()));
                ++checked;
            }
        }
        QVERIFY2(checked > 0, "no sidebar target row was examined");
    }

    void commandButtonsExposeBlockersWithoutSelection() {
        sak::FileManagementExplorerPanel panel;
        panel.resize(1100, 700);
        panel.show();
        QVERIFY(QTest::qWaitForWindowExposed(&panel));

        auto* table = child<QTableView>(&panel, "fileExplorerTable");
        QVERIFY(table);
        table->clearSelection();
        QApplication::processEvents();

        auto* copy = child<QPushButton>(&panel, "fileExplorerCopyButton");
        auto* rename = child<QPushButton>(&panel, "fileExplorerRenameButton");
        auto* deleteButton = child<QPushButton>(&panel, "fileExplorerDeleteButton");
        QVERIFY(copy);
        QVERIFY(rename);
        QVERIFY(deleteButton);
        QVERIFY(!copy->isEnabled());
        QVERIFY(!rename->isEnabled());
        QVERIFY(!deleteButton->isEnabled());
        // All three are selection_required commands, so they resolve at the same registry
        // gate and must carry the same blocker: either "no target" (nothing mounted on this
        // host) or "no selection". A non-empty check cannot tell those from the fail-closed
        // "Unknown File Explorer command." sentinel, which means the id is not wired at all.
        const QString no_selection =
            sak::ui::asLiteralRichText(QStringLiteral("Select an item "
                                                      "first."));
        const QString no_target =
            sak::ui::asLiteralRichText(QStringLiteral("No File Explorer target selected."));
        QVERIFY2(copy->toolTip() == no_selection || copy->toolTip() == no_target,
                 qPrintable(copy->toolTip()));
        QCOMPARE(rename->toolTip(), copy->toolTip());
        QCOMPARE(deleteButton->toolTip(), copy->toolTip());
    }

    void targetSelectionFeedsOmnibarAndSafetyPane() {
        sak::FileManagementExplorerPanel panel;
        panel.resize(1100, 700);
        panel.show();
        QVERIFY(QTest::qWaitForWindowExposed(&panel));

        auto* targetList = child<QListWidget>(&panel, "fileExplorerTargetList");
        auto* pathEdit = child<QLineEdit>(&panel, "fileExplorerPathEdit");
        auto* safety = child<QPlainTextEdit>(&panel, "fileExplorerSafetyText");
        QVERIFY(targetList);
        QVERIFY(pathEdit);
        QVERIFY(safety);
        const int targetRow = firstTargetRow(targetList);
        if (targetRow < 0) {
            QSKIP("No mounted File Explorer targets on this test host.");
        }

        targetList->setCurrentRow(targetRow);
        QApplication::processEvents();
        QVERIFY(!pathEdit->text().trimmed().isEmpty());
        // buildDetailsSafety emits six fixed lines in a fixed order. The current probe checks
        // one LABEL and ignores its value and every sibling line, so an empty capability word
        // or a lost Read/Browse line passes. The host-dependent label, file system and root
        // path are deliberately left unpinned; the capability words are a closed vocabulary.
        const QStringList safety_lines = safety->toPlainText().split(QLatin1Char('\n'));
        QVERIFY2(safety_lines.size() >= 6, qPrintable(safety->toPlainText()));
        const QStringList prefixes{QStringLiteral("Target: "),
                                   QStringLiteral("File system: "),
                                   QStringLiteral("Identity: ")};
        for (int i = 0; i < prefixes.size(); ++i) {
            QVERIFY2(safety_lines.at(i).startsWith(prefixes.at(i)), qPrintable(safety_lines.at(i)));
        }
        const QStringList capabilities{QStringLiteral("Write state: "),
                                       QStringLiteral("Read state: "),
                                       QStringLiteral("Browse state: ")};
        for (int i = 0; i < capabilities.size(); ++i) {
            const QString line = safety_lines.at(3 + i);
            QVERIFY2(line == capabilities.at(i) + QStringLiteral("enabled") ||
                         line == capabilities.at(i) + QStringLiteral("blocked"),
                     qPrintable(line));
        }
    }

    void previewPaneShowsLocalTextFileContents() {
        // M7 lane: selecting a readable local text file must put the FILE'S OWN
        // TEXT in the persistent preview pane. detailsPanePreviewSwitchesBetweenTextAndImage
        // only proves the text/image views swap; it never reads a file, so a preview
        // that rendered a hint (or the previous selection's bytes) would satisfy it.
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QByteArray payload =
            QByteArrayLiteral("preview-lane-marker: the quick brown fox\nsecond line\n");
        {
            QFile file(QDir(dir.path()).filePath(QStringLiteral("preview.txt")));
            QVERIFY(file.open(QIODevice::WriteOnly));
            QCOMPARE(file.write(payload), payload.size());
        }
        sak::FileManagementExplorerPanel panel;
        panel.resize(1200, 760);
        panel.show();
        QVERIFY(QTest::qWaitForWindowExposed(&panel));
        auto* targetList = child<QListWidget>(&panel, "fileExplorerTargetList");
        auto* pathEdit = child<QLineEdit>(&panel, "fileExplorerPathEdit");
        auto* table = child<QTableView>(&panel, "fileExplorerTable");
        auto* preview = child<QPlainTextEdit>(&panel, "fileExplorerPreviewText");
        QVERIFY(targetList);
        QVERIFY(pathEdit);
        QVERIFY(table);
        QVERIFY(preview);
        if (selectLocalTargetRowForDrive(targetList, pathEdit, dir.path().left(2).toUpper()) < 0) {
            QSKIP("No mounted local target for the temp drive on this test host.");
        }
        QVERIFY(navigateAndFindRow(pathEdit, table, dir.path(), QStringLiteral("preview")) >= 0);
        QVERIFY(waitForListingQuiescence(table));
        QVERIFY(selectRowStable(table, QStringLiteral("preview")));

        // The read is asynchronous (QFutureWatcher), so wait for the decoded text.
        // Pin BOTH lines: a decoder that stopped at the first newline, or one that
        // rendered a hex dump of a plainly decodable file, would pass a single-line check.
        QTRY_VERIFY2(preview->toPlainText().contains(QStringLiteral("preview-lane-marker")),
                     qPrintable(preview->toPlainText()));
        QVERIFY2(preview->toPlainText().contains(QStringLiteral("second line")),
                 qPrintable(preview->toPlainText()));

        // Capture the info pane while it is actually populated -- Preview, Properties,
        // Safety, and Evidence all filled from a real selection.
        auto* info = child<sak::FileExplorerDetailsPane>(&panel, "fileExplorerInfoPane");
        QVERIFY(info);
        captureBaseline(info, QStringLiteral("details-pane"));
    }

    // Drive the "Add Raw/Image" dialog once: fill its path field, pick @p file_system
    // in its combo, and accept. The dialog runs its own exec() loop, so this arms a
    // timer the way the QInputDialog helpers above do.
    void armAutoAcceptManualTargetDialog(const QString& path, const QString& file_system) {
        auto* accept = new QTimer(this);
        accept->setInterval(50);
        connect(accept, &QTimer::timeout, this, [accept, path, file_system]() {
            auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
            if (dialog == nullptr) {
                return;
            }
            auto* edit = dialog->findChild<QLineEdit*>();
            auto* combo = dialog->findChild<QComboBox*>();
            if (edit == nullptr || combo == nullptr) {
                return;
            }
            const int fs_index = combo->findText(file_system);
            if (fs_index < 0) {
                return;
            }
            edit->setText(path);
            combo->setCurrentIndex(fs_index);
            accept->stop();
            accept->deleteLater();
            dialog->accept();
        });
        accept->start();
    }

    // Add a manual raw/image target through the real Add Raw/Image button and select
    // its sidebar row. Returns the row, or -1 when it never appeared.
    int addManualTargetAndSelect(sak::FileManagementExplorerPanel& panel,
                                 QListWidget* targetList,
                                 const QString& image_path,
                                 const QString& file_system) {
        auto* add = child<QPushButton>(&panel, "fileExplorerAddRawImageButton");
        if (add == nullptr) {
            return -1;
        }
        armAutoAcceptManualTargetDialog(image_path, file_system);
        add->click();
        const QString needle = QFileInfo(image_path).fileName();
        int row = -1;
        std::ignore = QTest::qWaitFor(
            [targetList, &needle, &row]() {
                for (int i = 0; i < targetList->count(); ++i) {
                    const auto* item = targetList->item(i);
                    if (item != nullptr && item->flags().testFlag(Qt::ItemIsSelectable) &&
                        item->text().contains(needle)) {
                        row = i;
                        return true;
                    }
                }
                return false;
            },
            5000);
        if (row < 0) {
            return -1;
        }
        targetList->setCurrentRow(row);
        QApplication::processEvents();
        return row;
    }

    void safetyPaneNamesBlockerForEachRawFileSystem() {
        // M7 lane: the Safety pane must state the write/read/browse verdict AND the
        // file-system-specific reason for every raw family S.A.K. can be pointed at.
        // targetSelectionFeedsOmnibarAndSafetyPane only ever sees this host's own
        // mounted volume, so no per-file-system blocker text was pinned in the GUI.
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QVector<SafetyPaneCase> cases;
        buildSafetyPaneCases(QDir(dir.path()), &cases);
        QCOMPARE(cases.size(), 6);

        sak::FileManagementExplorerPanel panel;
        panel.resize(1280, 800);
        panel.show();
        QVERIFY(QTest::qWaitForWindowExposed(&panel));
        auto* targetList = child<QListWidget>(&panel, "fileExplorerTargetList");
        auto* safety = child<QPlainTextEdit>(&panel, "fileExplorerSafetyText");
        QVERIFY(targetList);
        QVERIFY(safety);

        for (const SafetyPaneCase& item : cases) {
            const QString label = QStringLiteral("%1 (%2)").arg(item.file_system, item.image);
            QVERIFY2(addManualTargetAndSelect(panel, targetList, item.image, item.file_system) >= 0,
                     qPrintable(label));
            verifySafetyPaneCase(safety, item);
        }
    }

    // One Safety-pane verdict: identity, capability words, and the file-system note.
    void verifySafetyPaneCase(QPlainTextEdit* safety, const SafetyPaneCase& expected) {
        const QString& file_system = expected.file_system;
        const QString& image_path = expected.image;
        const bool write_enabled = expected.write_enabled;
        const bool readable = expected.readable;
        const QString& note_fragment = expected.note_fragment;
        // The pane repaints from the selection change; wait for THIS target's identity
        // line so a stale previous verdict can never be read as this one's.
        const QString identity = QStringLiteral("Identity: %1").arg(image_path);
        QTRY_VERIFY2(safety->toPlainText().contains(identity), qPrintable(safety->toPlainText()));
        const QString text = safety->toPlainText();
        const QStringList lines = text.split(QLatin1Char('\n'));
        const auto verdict = [](bool enabled) {
            return enabled ? QStringLiteral("enabled") : QStringLiteral("blocked");
        };
        QVERIFY2(lines.contains(QStringLiteral("File system: %1").arg(file_system)),
                 qPrintable(text));
        // Whole-line compares: "Write state: blocked" must not be satisfied by a
        // substring of some other line, and the capability vocabulary is closed.
        QVERIFY2(lines.contains(QStringLiteral("Write state: %1").arg(verdict(write_enabled))),
                 qPrintable(text));
        QVERIFY2(lines.contains(QStringLiteral("Read state: %1").arg(verdict(readable))),
                 qPrintable(text));
        QVERIFY2(lines.contains(QStringLiteral("Browse state: %1").arg(verdict(readable))),
                 qPrintable(text));
        QVERIFY2(text.contains(note_fragment), qPrintable(text));
        // Every raw target carries the non-native confirmation rule, whatever its
        // file system: this is what keeps a write-enabled raw slice from reading
        // like a mounted volume.
        QVERIFY2(text.contains(QStringLiteral("Raw/non-native target:")), qPrintable(text));
        // A target with no reader must SAY it has none, in the blocker list -- the
        // metadata-only note alone does not name what is missing.
        if (!readable) {
            QVERIFY2(
                text.contains(
                    QStringLiteral("No directory browser is registered for %1").arg(file_system)),
                qPrintable(text));
        }
    }

    void contextMenusExposeRegistryActionsAndTargetActions() {
        QTemporaryDir empty_dir;
        QVERIFY(empty_dir.isValid());
        sak::FileManagementExplorerPanel panel;
        panel.resize(1100, 700);
        panel.show();
        QVERIFY(QTest::qWaitForWindowExposed(&panel));

        auto* table = child<QTableView>(&panel, "fileExplorerTable");
        auto* targetList = child<QListWidget>(&panel, "fileExplorerTargetList");
        auto* pathEdit = child<QLineEdit>(&panel, "fileExplorerPathEdit");
        QVERIFY(table);
        QVERIFY(targetList);
        QVERIFY(pathEdit);
        if (selectLocalTargetRowForDrive(targetList, pathEdit, empty_dir.path().left(2).toUpper()) <
            0) {
            QSKIP("No mounted local target for the temp drive on this test host.");
        }
        // An empty folder guarantees the click hits background, not a row.
        pathEdit->setText(empty_dir.path());
        QTest::keyClick(pathEdit, Qt::Key_Return);
        QVERIFY(waitForListingQuiescence(table));
        QTRY_COMPARE(table->model()->rowCount(), 0);

        // No selection -> the Files background menu: Layout / Sort by / Refresh,
        // the New group, Paste, selection helpers, then terminal.
        const QStringList tableActions = collectContextMenuTexts(table->viewport());
        // buildBackgroundContextMenu adds all nine top-level entries unconditionally and in
        // this compile-time order. The `>= 8` floor was blind to the Group by submenu --
        // deleting it leaves size() == 8 with every other probe green -- and the scattered
        // membership checks were blind to duplicates and to order. A disabled command renders
        // as "<text> - <blocker>", so each row is anchored by prefix.
        const QStringList expected_rows{QStringLiteral("Layout"),
                                        QStringLiteral("Sort by"),
                                        QStringLiteral("Group by"),
                                        QStringLiteral("Refresh"),
                                        QStringLiteral("New"),
                                        QStringLiteral("Paste"),
                                        QStringLiteral("Select All"),
                                        QStringLiteral("Invert Selection"),
                                        QStringLiteral("Open in Windows Terminal")};
        QCOMPARE(tableActions.size(), expected_rows.size());
        for (int i = 0; i < expected_rows.size(); ++i) {
            QVERIFY2(tableActions.at(i).startsWith(expected_rows.at(i)),
                     qPrintable(tableActions.join(QStringLiteral(" | "))));
        }
        QVERIFY(!containsTextStartingWith(tableActions, QStringLiteral("Rename")));
        QVERIFY(!containsTextStartingWith(tableActions, QStringLiteral("Delete")));

        const QStringList targetActions = collectContextMenuTexts(targetList->viewport());
        QVERIFY(targetActions.contains(QStringLiteral("Open Target")));
        QVERIFY(targetActions.contains(QStringLiteral("Copy Target Root")));
        QVERIFY(containsTextStartingWith(targetActions, QStringLiteral("Pin Favorite")) ||
                containsTextStartingWith(targetActions, QStringLiteral("Unpin Favorite")));
        QVERIFY(targetActions.contains(QStringLiteral("Target Properties")));
        QVERIFY(targetActions.contains(QStringLiteral("Refresh Mounted Targets")));
        QVERIFY(targetActions.contains(QStringLiteral("Scan Disks")));
        QVERIFY(targetActions.contains(QStringLiteral("Add Raw/Image")));
    }

    void propertiesWindowShowsGeneralComputesHashesAndRenames() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QByteArray payload = QByteArrayLiteral("properties payload");
        {
            QFile file(QDir(dir.path()).filePath(QStringLiteral("props.txt")));
            QVERIFY(file.open(QIODevice::WriteOnly));
            QCOMPARE(file.write(payload), payload.size());
        }
        sak::FileManagementExplorerPanel panel;
        panel.resize(1100, 700);
        panel.show();
        QVERIFY(QTest::qWaitForWindowExposed(&panel));
        auto* targetList = child<QListWidget>(&panel, "fileExplorerTargetList");
        auto* pathEdit = child<QLineEdit>(&panel, "fileExplorerPathEdit");
        auto* table = child<QTableView>(&panel, "fileExplorerTable");
        QVERIFY(targetList);
        QVERIFY(pathEdit);
        QVERIFY(table);
        if (selectLocalTargetRowForDrive(targetList, pathEdit, dir.path().left(2).toUpper()) < 0) {
            QSKIP("No mounted local target for the temp drive on this test host.");
        }
        QVERIFY(navigateAndFindRow(pathEdit, table, dir.path(), QStringLiteral("props")) >= 0);
        QVERIFY(waitForListingQuiescence(table));
        panel.activateWindow();
        table->setFocus();
        QApplication::processEvents();

        // Alt+Enter (Files OpenPropertiesAction) opens the Properties window.
        QVERIFY(selectRowStable(table, QStringLiteral("props")));
        QTest::keyClick(table, Qt::Key_Return, Qt::AltModifier);
        sak::FileExplorerPropertiesDialog* dialog = nullptr;
        QTRY_VERIFY((dialog = panel.findChild<sak::FileExplorerPropertiesDialog*>(
                         QStringLiteral("fileExplorerPropertiesDialog"))) != nullptr);
        auto* name = dialog->findChild<QLineEdit*>(QStringLiteral("fileExplorerPropertiesName"));
        auto* size = dialog->findChild<QLabel*>(QStringLiteral("fileExplorerPropertiesSize"));
        auto* tabs = dialog->findChild<QTabWidget*>(QStringLiteral("fileExplorerPropertiesTabs"));
        QVERIFY(name);
        QVERIFY(size);
        QVERIFY(tabs);
        QCOMPARE(name->text(), QStringLiteral("props.txt"));
        QTRY_VERIFY(size->text() != QStringLiteral("Calculating..."));

        // The Hashes tab computes lazily on first open; SHA-256 must match.
        QCOMPARE(tabs->count(), 2);
        tabs->setCurrentIndex(1);
        auto* sha =
            dialog->findChild<QLabel*>(QStringLiteral("fileExplorerPropertiesHash-SHA-256"));
        QVERIFY(sha);
        const QString expected = QString::fromLatin1(
            QCryptographicHash::hash(payload, QCryptographicHash::Sha256).toHex());
        QTRY_COMPARE_WITH_TIMEOUT(sha->text(), expected, 5000);

        // Editing the name and accepting commits a rename through the bridge.
        name->setText(QStringLiteral("props2.txt"));
        dialog->accept();
        QVERIFY2(QTest::qWaitFor(
                     [&dir]() {
                         return QFile::exists(
                             QDir(dir.path()).filePath(QStringLiteral("props2.txt")));
                     },
                     5000),
                 "properties rename did not land");
        QVERIFY(!QFile::exists(QDir(dir.path()).filePath(QStringLiteral("props.txt"))));
    }

    void archiveServiceRoundTripsAndDetectsSingleRoot() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QDir root(dir.path());
        QVERIFY(root.mkpath(QStringLiteral("bundle/deep")));
        const auto writeFile = [](const QString& path, const QByteArray& data) {
            QFile file(path);
            QVERIFY(file.open(QIODevice::WriteOnly));
            QCOMPARE(file.write(data), data.size());
        };
        writeFile(root.filePath(QStringLiteral("bundle/inner.txt")),
                  QByteArrayLiteral("zip inner payload"));
        writeFile(root.filePath(QStringLiteral("bundle/deep/leaf.bin")),
                  QByteArrayLiteral("zip leaf \x00\x01 payload"));
        writeFile(root.filePath(QStringLiteral("loose.txt")), QByteArrayLiteral("loose payload"));

        // Files naming: one item -> its own name; several -> the parent folder.
        QCOMPARE(sak::FileExplorerArchiveService::archiveBaseName({QStringLiteral("bundle")},
                                                                  QStringLiteral("parent")),
                 QStringLiteral("bundle"));
        QCOMPARE(sak::FileExplorerArchiveService::archiveBaseName(
                     {QStringLiteral("a"), QStringLiteral("b")}, QStringLiteral("parent")),
                 QStringLiteral("parent"));

        // Round trip: folder + loose file -> zip -> extract -> byte compare.
        const QString zip = root.filePath(QStringLiteral("out.zip"));
        const auto compressed = sak::FileExplorerArchiveService::compressToZip(
            zip,
            {root.filePath(QStringLiteral("bundle")), root.filePath(QStringLiteral("loose.txt"))});
        QVERIFY2(compressed.ok, qPrintable(compressed.blockers.join(QStringLiteral("; "))));
        // Directories now count toward the entry total (B8-21), so this is the
        // wrapper "bundle" + inner.txt + the "deep" subdir + deep/leaf.bin +
        // loose.txt = 5 (was 3 when only files were counted).
        QCOMPARE(compressed.entries, 5);
        // Two top-level roots ("bundle", "loose.txt") -> not a single root.
        QVERIFY(!sak::FileExplorerArchiveService::hasSingleTopLevelRoot(zip, nullptr));

        const QString out = root.filePath(QStringLiteral("out"));
        const auto extracted = sak::FileExplorerArchiveService::extractZip(zip, out);
        QVERIFY2(extracted.ok, qPrintable(extracted.blockers.join(QStringLiteral("; "))));
        QFile leaf(QDir(out).filePath(QStringLiteral("bundle/deep/leaf.bin")));
        QVERIFY(leaf.open(QIODevice::ReadOnly));
        QCOMPARE(leaf.readAll(), QByteArrayLiteral("zip leaf \x00\x01 payload"));
        QFile loose(QDir(out).filePath(QStringLiteral("loose.txt")));
        QVERIFY(loose.open(QIODevice::ReadOnly));
        QCOMPARE(loose.readAll(), QByteArrayLiteral("loose payload"));

        // A zip holding only the folder IS single-rooted (smart extract flattens).
        const QString single = root.filePath(QStringLiteral("single.zip"));
        QVERIFY(sak::FileExplorerArchiveService::compressToZip(
                    single, {root.filePath(QStringLiteral("bundle"))})
                    .ok);
        QString detected_root;
        QVERIFY(sak::FileExplorerArchiveService::hasSingleTopLevelRoot(single, &detected_root));
        QCOMPARE(detected_root, QStringLiteral("bundle"));
    }

    void incompleteMoveMarksTransferEngineIncomplete() {
        // A tree deeper than the import bound copies with entries dropped; the
        // transfer engine must report the copy as incomplete so the move worker
        // never deletes the intact source (lastTransferComplete gates the delete).
        QTemporaryDir source;
        QTemporaryDir destination;
        QVERIFY(source.isValid());
        QVERIFY(destination.isValid());
        QString deep = QStringLiteral("root");
        for (int level = 0; level < 40; ++level) {
            deep += QStringLiteral("/d");
        }
        QVERIFY(QDir(source.path()).mkpath(deep));
        {
            QFile bottom(QDir(source.path()).filePath(deep + QStringLiteral("/leaf.txt")));
            QVERIFY(bottom.open(QIODevice::WriteOnly));
            QVERIFY(bottom.write("deep leaf") > 0);
        }

        const auto srcTarget = sak::FileManagementFileSystemBridge::localTarget(source.path());
        const auto dstTarget = sak::FileManagementFileSystemBridge::localTarget(destination.path());
        sak::FileExplorerTransferEngine engine(srcTarget, dstTarget, 0);
        sak::FileExplorerTransferItem item;
        item.source_path = QDir(source.path()).filePath(QStringLiteral("root"));
        item.destination_path = QDir(destination.path()).filePath(QStringLiteral("moved-root"));
        item.directory = true;
        QVERIFY(engine.transferEntry(item));
        QVERIFY(!engine.lastTransferComplete());
        QVERIFY(QFileInfo(QDir(source.path()).filePath(QStringLiteral("root"))).isDir());

        // A complete copy (shallow tree) leaves the engine reporting complete.
        QTemporaryDir shallowSource;
        QVERIFY(shallowSource.isValid());
        {
            QFile flat(QDir(shallowSource.path()).filePath(QStringLiteral("flat.txt")));
            QVERIFY(flat.open(QIODevice::WriteOnly));
            QVERIFY(flat.write("flat") > 0);
        }
        sak::FileExplorerTransferEngine engine2(
            sak::FileManagementFileSystemBridge::localTarget(shallowSource.path()), dstTarget, 0);
        sak::FileExplorerTransferItem flatItem;
        flatItem.source_path = QDir(shallowSource.path()).filePath(QStringLiteral("flat.txt"));
        flatItem.destination_path = QDir(destination.path()).filePath(QStringLiteral("flat.txt"));
        QVERIFY(engine2.transferEntry(flatItem));
        QVERIFY(engine2.lastTransferComplete());
    }

    void replaceDeferredToTransferRemovesOccupantAtCopyTime() {
        // Replace no longer deletes destinations at collision-resolve time: the
        // engine removes the occupant right before the item's own copy and
        // resolves its kind authoritatively then (here a directory tree occupies
        // the path a file is landing on).
        QTemporaryDir source;
        QTemporaryDir destination;
        QVERIFY(source.isValid());
        QVERIFY(destination.isValid());
        {
            QFile payload(QDir(source.path()).filePath(QStringLiteral("a.txt")));
            QVERIFY(payload.open(QIODevice::WriteOnly));
            QVERIFY(payload.write("replacement payload") > 0);
        }
        const QString occupied = QDir(destination.path()).filePath(QStringLiteral("a.txt"));
        QVERIFY(QDir(destination.path()).mkpath(QStringLiteral("a.txt/nested")));
        {
            QFile child(occupied + QStringLiteral("/nested/child.bin"));
            QVERIFY(child.open(QIODevice::WriteOnly));
            QVERIFY(child.write("old") > 0);
        }

        sak::FileExplorerTransferEngine engine(
            sak::FileManagementFileSystemBridge::localTarget(source.path()),
            sak::FileManagementFileSystemBridge::localTarget(destination.path()),
            0);
        sak::FileExplorerTransferItem item;
        item.source_path = QDir(source.path()).filePath(QStringLiteral("a.txt"));
        item.destination_path = occupied;
        item.replace_destination = true;
        QVERIFY(engine.transferEntry(item));
        QVERIFY(QFileInfo(occupied).isFile());
        QFile out(occupied);
        QVERIFY(out.open(QIODevice::ReadOnly));
        QCOMPARE(out.readAll(), QByteArrayLiteral("replacement payload"));

        // An occupant that vanished between resolve and copy is not an error.
        sak::FileExplorerTransferItem vacant;
        vacant.source_path = item.source_path;
        vacant.destination_path = QDir(destination.path()).filePath(QStringLiteral("b.txt"));
        vacant.replace_destination = true;
        QVERIFY(engine.transferEntry(vacant));
        QVERIFY(QFileInfo(vacant.destination_path).isFile());
    }

    void replaceCopyFailurePreservesOriginalDestination() {
        // B8-06: a Replace stages the copy and swaps it in only after it lands whole,
        // so a copy that fails (here: a missing source) must leave the ORIGINAL
        // destination byte-for-byte intact instead of destroying it up front.
        QTemporaryDir source;
        QTemporaryDir destination;
        QVERIFY(source.isValid());
        QVERIFY(destination.isValid());

        const QString occupied = QDir(destination.path()).filePath(QStringLiteral("keep.txt"));
        {
            QFile original(occupied);
            QVERIFY(original.open(QIODevice::WriteOnly));
            QVERIFY(original.write("precious original contents") > 0);
        }

        sak::FileExplorerTransferEngine engine(
            sak::FileManagementFileSystemBridge::localTarget(source.path()),
            sak::FileManagementFileSystemBridge::localTarget(destination.path()),
            0);
        sak::FileExplorerTransferItem item;
        // A source path that does not exist forces the copy leg to fail.
        item.source_path = QDir(source.path()).filePath(QStringLiteral("missing.txt"));
        item.destination_path = occupied;
        item.replace_destination = true;
        QVERIFY(!engine.transferEntry(item));  // the copy failed
        // Named exactly: a regression later in the replace chain (staging, backup, restore)
        // records one of five other blockers and passes the non-empty probe unchanged.
        QCOMPARE(engine.blockers(),
                 (QStringList{
                     QStringLiteral("Source is not a readable file: %1").arg(item.source_path)}));

        // The original destination survived untouched, and no staging/backup cruft
        // was left behind in the destination folder. (Read via a scoped handle so it
        // is closed before the next rename -- Windows will not move an open file.)
        QVERIFY(QFileInfo(occupied).isFile());
        {
            QFile survivor(occupied);
            QVERIFY(survivor.open(QIODevice::ReadOnly));
            QCOMPARE(survivor.readAll(), QByteArrayLiteral("precious original contents"));
        }
        const auto leftovers = QDir(destination.path())
                                   .entryList(QStringList{QStringLiteral(".sak-*")},
                                              QDir::Files | QDir::Dirs | QDir::Hidden);
        QVERIFY2(leftovers.isEmpty(), qPrintable(leftovers.join(QStringLiteral(", "))));

        // A same-kind Replace that succeeds still overwrites the destination whole.
        {
            QFile src(QDir(source.path()).filePath(QStringLiteral("new.txt")));
            QVERIFY(src.open(QIODevice::WriteOnly));
            QVERIFY(src.write("brand new contents") > 0);
        }
        sak::FileExplorerTransferItem good;
        good.source_path = QDir(source.path()).filePath(QStringLiteral("new.txt"));
        good.destination_path = occupied;
        good.replace_destination = true;
        QVERIFY2(engine.transferEntry(good),
                 qPrintable(engine.blockers().join(QStringLiteral(" | "))));
        {
            QFile replaced(occupied);
            QVERIFY(replaced.open(QIODevice::ReadOnly));
            QCOMPARE(replaced.readAll(), QByteArrayLiteral("brand new contents"));
        }
        const auto after = QDir(destination.path())
                               .entryList(QStringList{QStringLiteral(".sak-*")},
                                          QDir::Files | QDir::Dirs | QDir::Hidden);
        QVERIFY2(after.isEmpty(), qPrintable(after.join(QStringLiteral(", "))));
    }

    void replaceSiblingNamesAreUnguessableAndNeverClobberPlantedContent() {
        // R5 p9_filemgmt-5: the staging/backup siblings used to be named with a bare
        // per-engine counter (".sak-stage-<name>.0", ".sak-old-<name>.1"), so content
        // already sitting at those predictable paths was overwritten (a file), merged
        // into (a directory), or deleted by the cleanup. The names now carry system
        // entropy AND the staging name is claimed by an exclusive create, so anything
        // planted at the old predictable paths must survive a Replace untouched.
        QTemporaryDir source;
        QTemporaryDir destination;
        QVERIFY(source.isValid());
        QVERIFY(destination.isValid());
        const QDir dest_dir(destination.path());
        const auto write = [](const QString& path, const QByteArray& data) {
            QFile file(path);
            QVERIFY(file.open(QIODevice::WriteOnly));
            QCOMPARE(file.write(data), data.size());
        };
        write(QDir(source.path()).filePath(QStringLiteral("keep.txt")), "replacement payload");
        const QString occupied = dest_dir.filePath(QStringLiteral("keep.txt"));
        write(occupied, "original payload");

        // Plant at the exact paths the old sequence-only naming would have produced:
        // the staging sibling (seq 0) as a file and the set-aside sibling (seq 1) as a
        // directory holding a file, so both the overwrite and the merge/delete vector
        // are covered.
        const QString planted_stage = dest_dir.filePath(QStringLiteral(".sak-stage-keep.txt.0"));
        const QString planted_backup = dest_dir.filePath(QStringLiteral(".sak-old-keep.txt.1"));
        write(planted_stage, "victim stage payload");
        QVERIFY(dest_dir.mkpath(QStringLiteral(".sak-old-keep.txt.1/nested")));
        write(planted_backup + QStringLiteral("/nested/victim.bin"), "victim backup payload");

        sak::FileExplorerTransferEngine engine(
            sak::FileManagementFileSystemBridge::localTarget(source.path()),
            sak::FileManagementFileSystemBridge::localTarget(destination.path()),
            0);
        sak::FileExplorerTransferItem item;
        item.source_path = QDir(source.path()).filePath(QStringLiteral("keep.txt"));
        item.destination_path = occupied;
        item.replace_destination = true;
        QVERIFY2(engine.transferEntry(item),
                 qPrintable(engine.blockers().join(QStringLiteral(" | "))));

        // The Replace landed...
        {
            QFile replaced(occupied);
            QVERIFY(replaced.open(QIODevice::ReadOnly));
            QCOMPARE(replaced.readAll(), QByteArrayLiteral("replacement payload"));
        }
        // ...and neither planted entry was touched.
        {
            QFile survivor(planted_stage);
            QVERIFY2(survivor.open(QIODevice::ReadOnly), "planted staging file was destroyed");
            QCOMPARE(survivor.readAll(), QByteArrayLiteral("victim stage payload"));
        }
        {
            QFile survivor(planted_backup + QStringLiteral("/nested/victim.bin"));
            QVERIFY2(survivor.open(QIODevice::ReadOnly), "planted backup tree was destroyed");
            QCOMPARE(survivor.readAll(), QByteArrayLiteral("victim backup payload"));
        }
        // The engine's own siblings were cleaned up: only the planted pair remains.
        auto leftovers = dest_dir.entryList(QStringList{QStringLiteral(".sak-*")},
                                            QDir::Files | QDir::Dirs | QDir::Hidden);
        leftovers.sort();
        QCOMPARE(leftovers,
                 (QStringList{QStringLiteral(".sak-old-keep.txt.1"),
                              QStringLiteral(".sak-stage-keep.txt.0")}));
    }

    void compressRefusesToClobberExistingArchive() {
        // B8-07 (already remediated by B6-19/20 exclusive-create): compressToZip must
        // never truncate a file that already occupies the archive path, and its
        // remove-on-failure must never delete that pre-existing file. Certifies the
        // NewOnly guard so a re-Compress onto an existing name cannot destroy it.
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QDir root(dir.path());
        {
            QFile payload(root.filePath(QStringLiteral("payload.txt")));
            QVERIFY(payload.open(QIODevice::WriteOnly));
            QVERIFY(payload.write("to be archived") > 0);
        }

        // A precious file already sits where the archive would be written.
        const QString zip = root.filePath(QStringLiteral("out.zip"));
        const QByteArray precious = QByteArrayLiteral("PRECIOUS pre-existing archive bytes");
        {
            QFile existing(zip);
            QVERIFY(existing.open(QIODevice::WriteOnly));
            QCOMPARE(existing.write(precious), static_cast<qint64>(precious.size()));
        }

        const auto blocked = sak::FileExplorerArchiveService::compressToZip(
            zip, {root.filePath(QStringLiteral("payload.txt"))});
        QVERIFY(!blocked.ok);  // refused: the path is occupied
        // The NewOnly exclusive-create refusal specifically, not one of the three sibling
        // compress failures (inside-source-folder, writer status, add failure).
        QCOMPARE(blocked.blockers,
                 (QStringList{QStringLiteral("Could not create archive %1 (it already exists or "
                                             "is not writable).")
                                  .arg(zip)}));

        // The pre-existing file is byte-for-byte intact -- neither truncated by the
        // writer nor deleted by the failure cleanup.
        QVERIFY(QFileInfo(zip).isFile());
        QFile survivor(zip);
        QVERIFY(survivor.open(QIODevice::ReadOnly));
        QCOMPARE(survivor.readAll(), precious);
    }

    void extractRollsBackPartialTreeOnFailure() {
        // B8-08: when an entry aborts extraction, files written for earlier entries
        // must be rolled back so no half-extracted tree is left behind.
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QDir root(dir.path());

        // A zip whose first entry is valid and whose second aborts the extraction after the
        // first file has already landed.
        const QString zip = root.filePath(QStringLiteral("partial.zip"));
        {
            QZipWriter writer(zip);
            writer.addFile(QStringLiteral("good.txt"),
                           QByteArrayLiteral("landed then rolled back"));
            writer.addFile(QStringLiteral("../escape.txt"), QByteArrayLiteral("evil"));
            writer.close();
            QCOMPARE(writer.status(), QZipWriter::NoError);
        }

        const QString out = root.filePath(QStringLiteral("out"));
        const auto result = sak::FileExplorerArchiveService::extractZip(zip, out);
        QVERIFY(!result.ok);
        // What ACTUALLY aborts this extraction, pinned: QZipReader reports the second entry as
        // "escape.txt" -- the leading "../" is normalized away before any of our guards see it
        // -- so entryEscapesDestination never fires, and fileData() then misses the entry
        // stored under the raw name. The rollback this test exists for runs either way. The
        // absolute-path arm in extractRejectsZipSlipAndPerFileSizeBomb is the fixture that does
        // reach the traversal guard.
        QCOMPARE(result.blockers,
                 (QStringList{QStringLiteral(
                     "Extraction of entry escape.txt failed (corrupt or unsupported).")}));

        // good.txt was written first, then rolled back; nothing we created survives.
        QVERIFY(!QFileInfo(QDir(out).filePath(QStringLiteral("good.txt"))).exists());
        QVERIFY(!QFileInfo(root.filePath(QStringLiteral("escape.txt"))).exists());
        const auto leftover =
            QDir(out).entryList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden);
        QVERIFY2(leftover.isEmpty(), qPrintable(leftover.join(QStringLiteral(", "))));
    }

    void extractRejectsOversizeCentralDirectoryBeforeMaterializing() {
        // B8-09: extractZip must bound the central directory before fileInfoList()
        // materializes it (~2-3x), so a zip whose EOCD claims a multi-GB central
        // directory is refused up front rather than allocated. Build a real zip and
        // tamper the EOCD "size of central directory" field to a huge value.
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QDir root(dir.path());
        const QString zip = root.filePath(QStringLiteral("huge_cd.zip"));
        {
            QZipWriter writer(zip);
            writer.addFile(QStringLiteral("a.txt"), QByteArrayLiteral("payload"));
            writer.close();
            QCOMPARE(writer.status(), QZipWriter::NoError);
        }

        QByteArray bytes;
        {
            QFile f(zip);
            QVERIFY(f.open(QIODevice::ReadOnly));
            bytes = f.readAll();
        }
        const int eocd = bytes.lastIndexOf(QByteArrayLiteral("PK\x05\x06"));
        QVERIFY(eocd >= 0);
        QVERIFY(eocd + 16 <= bytes.size());
        // Size of central directory is a LE uint32 at EOCD+12. Claim ~2 GiB.
        const quint32 huge = 0x7F'FF'FF'FFu;
        for (int i = 0; i < 4; ++i) {
            bytes[eocd + 12 + i] = static_cast<char>((huge >> (8 * i)) & 0xFF);
        }
        {
            QFile f(zip);
            QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
            QCOMPARE(f.write(bytes), static_cast<qint64>(bytes.size()));
        }

        const QString out = root.filePath(QStringLiteral("out"));
        const auto result = sak::FileExplorerArchiveService::extractZip(zip, out);
        QVERIFY(!result.ok);  // refused before materializing the central directory
        // The bound, with both numbers. If zipCentralDirectorySize regressed to -1 the code
        // reports "is not a readable zip archive." instead and the central-directory bound
        // this test exists for is never exercised.
        QCOMPARE(result.blockers,
                 (QStringList{QStringLiteral("%1 has a central directory too large to read (%2 "
                                             "bytes > %3 limit).")
                                  .arg(zip)
                                  .arg(2'147'483'647LL)
                                  .arg(64LL * 1024 * 1024)}));
        // Nothing was extracted.
        QVERIFY(!QFileInfo(QDir(out).filePath(QStringLiteral("a.txt"))).exists());
    }

    void extractRejectsEntryDeclaringOversizeUncompressed() {
        // B8-10: an entry whose DECLARED uncompressed size exceeds the per-file cap
        // must be refused before it is decompressed into RAM (QZipReader decodes each
        // entry whole into one QByteArray). Patch the central-directory record's
        // uncompressed-size field to ~2 GiB -- which the old 4 GiB cap would have let
        // through to a 2 GiB allocation.
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QDir root(dir.path());
        const QString zip = root.filePath(QStringLiteral("bigdecl.zip"));
        {
            QZipWriter writer(zip);
            writer.addFile(QStringLiteral("a.txt"), QByteArrayLiteral("small real payload"));
            writer.close();
            QCOMPARE(writer.status(), QZipWriter::NoError);
        }

        QByteArray bytes;
        {
            QFile f(zip);
            QVERIFY(f.open(QIODevice::ReadOnly));
            bytes = f.readAll();
        }
        // Central-directory file header: PK\x01\x02; uncompressed size is a LE uint32
        // at offset +24.
        const int cd = bytes.indexOf(QByteArrayLiteral("PK\x01\x02"));
        QVERIFY(cd >= 0);
        QVERIFY(cd + 28 <= bytes.size());
        const quint32 declared = 0x7F'00'00'00u;  // ~2.1 GiB, over the 512 MiB cap
        for (int i = 0; i < 4; ++i) {
            bytes[cd + 24 + i] = static_cast<char>((declared >> (8 * i)) & 0xFF);
        }
        {
            QFile f(zip);
            QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
            QCOMPARE(f.write(bytes), static_cast<qint64>(bytes.size()));
        }

        const QString out = root.filePath(QStringLiteral("out"));
        const auto result = sak::FileExplorerArchiveService::extractZip(zip, out);
        QVERIFY(!result.ok);  // refused before decoding into RAM
        // The size cap, naming the entry. The depth and symlink notices go to warnings, not
        // blockers, so this is the whole list.
        QCOMPARE(result.blockers,
                 (QStringList{QStringLiteral("Entry a.txt exceeds the extraction size limit.")}));
        QVERIFY(!QFileInfo(QDir(out).filePath(QStringLiteral("a.txt"))).exists());
    }

    // The fixture that actually reaches entryEscapesDestination. An ABSOLUTE entry name
    // survives QZipReader intact (unlike a leading "../", which it normalizes away), so this
    // is the only zip-slip shape the guard ever sees. The target sits inside the temp tree
    // beside the destination, so a regression writes somewhere harmless and still fails here.
    static void verifyAbsoluteEntryNameIsRefused(const QDir& root) {
        const QString abs_zip = root.filePath(QStringLiteral("abs_slip.zip"));
        const QString abs_target = root.filePath(QStringLiteral("escaped_abs.txt"));
        {
            QZipWriter writer(abs_zip);
            writer.addFile(abs_target, QByteArrayLiteral("owned"));
            writer.close();
            QCOMPARE(writer.status(), QZipWriter::NoError);
        }
        const auto refused = sak::FileExplorerArchiveService::extractZip(
            abs_zip, root.filePath(QStringLiteral("abs_out")));
        QVERIFY(!refused.ok);
        QCOMPARE(refused.blockers,
                 (QStringList{QStringLiteral("Refused entry %1 (path escapes the destination).")
                                  .arg(abs_target)}));
        QVERIFY(!QFile::exists(abs_target));
    }

    void extractRejectsZipSlipAndPerFileSizeBomb() {
        // The bounded extractor must fail closed on a path-traversal (zip-slip)
        // entry and on an entry declaring an oversize expansion, and must not
        // leave the destination populated when it rejects.
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QDir root(dir.path());

        // Hand-build a zip whose single entry name escapes the destination.
        const QString slip_zip = root.filePath(QStringLiteral("slip.zip"));
        {
            QZipWriter writer(slip_zip);
            writer.addFile(QStringLiteral("../escapee.txt"), QByteArrayLiteral("owned"));
            writer.close();
            QCOMPARE(writer.status(), QZipWriter::NoError);
        }
        const QString slip_out = root.filePath(QStringLiteral("slip_out"));
        const auto slip = sak::FileExplorerArchiveService::extractZip(slip_zip, slip_out);
        QVERIFY(!slip.ok);
        // Pinned to what a relative "../" fixture actually exercises. QZipReader normalizes the
        // name to "escapee.txt" before extractZipEntry sees it, so entryEscapesDestination is
        // NOT what refuses this archive -- fileData() misses the entry stored under the raw
        // name. The old non-empty probe read as proof of a traversal guard it never reached.
        QCOMPARE(slip.blockers,
                 (QStringList{QStringLiteral(
                     "Extraction of entry escapee.txt failed (corrupt or unsupported).")}));
        verifyAbsoluteEntryNameIsRefused(root);
        if (QTest::currentTestFailed()) {
            return;
        }
        // The traversal target next to the destination must never be written.
        QVERIFY(!QFile::exists(QDir(root.filePath(QStringLiteral("slip_out")))
                                   .filePath(QStringLiteral("../escapee.txt"))));
        QVERIFY(!QFile::exists(root.filePath(QStringLiteral("escapee.txt"))));

        // A well-formed archive still round-trips through the bounded path.
        const QString ok_zip = root.filePath(QStringLiteral("ok.zip"));
        {
            QFile probe(root.filePath(QStringLiteral("probe.txt")));
            QVERIFY(probe.open(QIODevice::WriteOnly));
            QVERIFY(probe.write("bounded round trip") > 0);
        }
        QVERIFY(sak::FileExplorerArchiveService::compressToZip(
                    ok_zip, {root.filePath(QStringLiteral("probe.txt"))})
                    .ok);
        const QString ok_out = root.filePath(QStringLiteral("ok_out"));
        const auto ok = sak::FileExplorerArchiveService::extractZip(ok_zip, ok_out);
        QVERIFY2(ok.ok, qPrintable(ok.blockers.join(QStringLiteral("; "))));
        QFile extracted(QDir(ok_out).filePath(QStringLiteral("probe.txt")));
        QVERIFY(extracted.open(QIODevice::ReadOnly));
        QCOMPARE(extracted.readAll(), QByteArrayLiteral("bounded round trip"));
    }

    void smartExtractHotkeyFlattensSingleRootArchive() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QDir root(dir.path());
        QVERIFY(root.mkpath(QStringLiteral("payload/deep")));
        {
            QFile file(root.filePath(QStringLiteral("payload/deep/leaf.txt")));
            QVERIFY(file.open(QIODevice::WriteOnly));
            QVERIFY(file.write("smart extract payload") > 0);
        }
        // Single-root archive named differently from its root folder, so the
        // smart flatten is observable ("payload" appears, no "wrapped" folder).
        const QString zip = root.filePath(QStringLiteral("wrapped.zip"));
        QVERIFY(sak::FileExplorerArchiveService::compressToZip(
                    zip, {root.filePath(QStringLiteral("payload"))})
                    .ok);
        QVERIFY(QDir(root.filePath(QStringLiteral("payload"))).removeRecursively());

        sak::FileManagementExplorerPanel panel;
        panel.resize(1100, 700);
        panel.show();
        QVERIFY(QTest::qWaitForWindowExposed(&panel));
        auto* targetList = child<QListWidget>(&panel, "fileExplorerTargetList");
        auto* pathEdit = child<QLineEdit>(&panel, "fileExplorerPathEdit");
        auto* table = child<QTableView>(&panel, "fileExplorerTable");
        QVERIFY(targetList);
        QVERIFY(pathEdit);
        QVERIFY(table);
        if (selectLocalTargetRowForDrive(targetList, pathEdit, dir.path().left(2).toUpper()) < 0) {
            QSKIP("No mounted local target for the temp drive on this test host.");
        }
        QVERIFY(navigateAndFindRow(pathEdit, table, dir.path(), QStringLiteral("wrapped")) >= 0);
        QVERIFY(waitForListingQuiescence(table));
        panel.activateWindow();
        table->setFocus();
        QApplication::processEvents();

        // Ctrl+Shift+E (Files DecompressArchiveHereSmart): the single-root
        // archive flattens - its root folder lands directly in the current dir.
        QVERIFY(selectRowStable(table, QStringLiteral("wrapped")));
        QTest::keyClick(table, Qt::Key_E, Qt::ControlModifier | Qt::ShiftModifier);
        const QString flattened = root.filePath(QStringLiteral("payload/deep/leaf.txt"));
        QVERIFY2(QTest::qWaitFor([&flattened]() { return QFile::exists(flattened); }, 5000),
                 "smart extract did not flatten the single-root archive");
        QVERIFY(!QDir(root.filePath(QStringLiteral("wrapped"))).exists());

        // The extract ran on the archive worker: the Files two-card pattern
        // leaves one terminal Success card ("Extracted \"{zip}\" to ...").
        QTRY_COMPARE(panel.statusCenterModel()->inProgressCount(), 0);
        QTRY_VERIFY(panel.statusCenterModel()->hasAnyItem());
        const auto* card = panel.statusCenterModel()->items().first();
        QCOMPARE(card->kind(), sak::FileExplorerStatusItemKind::Successful);
        // The prefix probe stops immediately before the destination, so an empty m_destination
        // -- the exact failure mode headerText() guards -- renders `to ""` and still passes.
        QCOMPARE(
            card->header(),
            QStringLiteral("Extracted \"wrapped.zip\" to \"%1\"").arg(QDir(dir.path()).dirName()));
    }

    void compressContextMenuRunsOnWorkerWithCompressCard() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QDir root(dir.path());
        {
            QFile file(root.filePath(QStringLiteral("alpha.txt")));
            QVERIFY(file.open(QIODevice::WriteOnly));
            QVERIFY(file.write("compress worker payload") > 0);
        }

        sak::FileManagementExplorerPanel panel;
        panel.resize(1100, 700);
        panel.show();
        QVERIFY(QTest::qWaitForWindowExposed(&panel));
        auto* targetList = child<QListWidget>(&panel, "fileExplorerTargetList");
        auto* pathEdit = child<QLineEdit>(&panel, "fileExplorerPathEdit");
        auto* table = child<QTableView>(&panel, "fileExplorerTable");
        QVERIFY(targetList);
        QVERIFY(pathEdit);
        QVERIFY(table);
        if (selectLocalTargetRowForDrive(targetList, pathEdit, dir.path().left(2).toUpper()) < 0) {
            QSKIP("No mounted local target for the temp drive on this test host.");
        }
        QVERIFY(navigateAndFindRow(pathEdit, table, dir.path(), QStringLiteral("alpha")) >= 0);
        QVERIFY(waitForListingQuiescence(table));

        // Files Compress > "Create alpha.zip" packs on the archive worker and
        // leaves a terminal Compress card pointing at the archive itself.
        QVERIFY(selectRowStable(table, QStringLiteral("alpha")));
        QVERIFY(triggerContextSubmenuAction(table->viewport(),
                                            QStringLiteral("fileExplorerCompressMenu"),
                                            QStringLiteral("Create ")));
        QTRY_VERIFY(panel.statusCenterModel()->hasAnyItem());
        // Files GenerateArchiveNameFromItems: a single item keeps its own
        // full name (extension included), so alpha.txt packs to alpha.txt.zip.
        const QString zip = root.filePath(QStringLiteral("alpha.txt.zip"));
        QVERIFY2(QTest::qWaitFor([&zip]() { return QFile::exists(zip); }, 5000),
                 "compress worker did not write the zip");
        QTRY_COMPARE(panel.statusCenterModel()->inProgressCount(), 0);
        QTRY_VERIFY(panel.statusCenterModel()->hasAnyItem());
        const auto* card = panel.statusCenterModel()->items().first();
        QCOMPARE(card->kind(), sak::FileExplorerStatusItemKind::Successful);
        QCOMPARE(card->header(), QStringLiteral("Compressed 1 item to \"alpha.txt.zip\""));
    }

    // Drives the sidebar context menu's "Reorder sidebar items..." into the
    // Files ReorderSidebarItemsDialog: swap the two pins and Save. Takes
    // parameters so QtTest does not run it as a test slot.
    void driveReorderFavoritesDialog(QListWidget* list, bool* dialog_driven) {
        QTimer::singleShot(0, [dialog_driven]() {
            auto* menu = qobject_cast<QMenu*>(QApplication::activePopupWidget());
            if (!menu) {
                return;
            }
            QAction* reorder = nullptr;
            for (auto* action : menu->actions()) {
                if (action->objectName() == QStringLiteral("fileExplorerReorderSidebarItems")) {
                    reorder = action;
                    break;
                }
            }
            if (reorder && reorder->isEnabled()) {
                QTimer::singleShot(0, [dialog_driven]() {
                    auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
                    if (!dialog) {
                        return;
                    }
                    auto* items =
                        dialog->findChild<QListWidget*>(QStringLiteral("fileExplorerReorderList"));
                    auto* buttons = dialog->findChild<QDialogButtonBox*>(
                        QStringLiteral("fileExplorerReorderButtons"));
                    if (!items || !buttons || items->count() != 2) {
                        dialog->reject();
                        return;
                    }
                    items->insertItem(1, items->takeItem(0));
                    *dialog_driven = true;
                    buttons->button(QDialogButtonBox::Save)->click();
                });
                reorder->trigger();
            }
            menu->close();
        });
        const QPoint corner(4, list->viewport()->rect().bottom() - 4);
        QContextMenuEvent event(QContextMenuEvent::Mouse,
                                corner,
                                list->viewport()->mapToGlobal(corner));
        QApplication::sendEvent(list->viewport(), &event);
        QApplication::processEvents();
        QApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    }

    void sidebarCompactRailAndReorderDialogFollowFiles() {
        QSettings settings;
        settings.beginGroup(QStringLiteral("FileManagementExplorer"));
        settings.setValue(QStringLiteral("FavoriteTargetIds"),
                          QStringList{QStringLiteral("disk:98:partition:8"),
                                      QStringLiteral("disk:97:partition:7")});
        settings.endGroup();
        settings.sync();

        sak::FileManagementExplorerPanel panel;
        panel.resize(1100, 700);
        panel.show();
        QVERIFY(QTest::qWaitForWindowExposed(&panel));
        auto* sidebar = panel.findChild<sak::FileExplorerSidebar*>();
        auto* list = child<QListWidget>(&panel, "fileExplorerTargetList");
        QVERIFY(sidebar);
        QVERIFY(list);

        // Ctrl+B (Files ToggleSidebarAction): Expanded <-> the 56px icon-only
        // compact rail; the sidebar stays visible instead of hiding.
        panel.activateWindow();
        QTest::keyClick(&panel, Qt::Key_B, Qt::ControlModifier);
        QTRY_VERIFY(sidebar->isCompact());
        QVERIFY(sidebar->isVisible());
        QCOMPARE(sidebar->maximumWidth(), 56);
        QVERIFY(list->count() > 0);
        for (int row = 0; row < list->count(); ++row) {
            QVERIFY2(list->item(row)->text().isEmpty(), "compact rail row still shows text");
        }
        QTest::keyClick(&panel, Qt::Key_B, Qt::ControlModifier);
        QTRY_VERIFY(!sidebar->isCompact());
        bool any_text = false;
        for (int row = 0; row < list->count(); ++row) {
            any_text = any_text || !list->item(row)->text().isEmpty();
        }
        QVERIFY2(any_text, "expanded sidebar rows did not restore their text");

        bool dialog_driven = false;
        driveReorderFavoritesDialog(list, &dialog_driven);
        QVERIFY2(dialog_driven, "reorder dialog was not driven");
        QSettings after;
        after.beginGroup(QStringLiteral("FileManagementExplorer"));
        QCOMPARE(after.value(QStringLiteral("FavoriteTargetIds")).toStringList(),
                 (QStringList{QStringLiteral("disk:97:partition:7"),
                              QStringLiteral("disk:98:partition:8")}));
    }

    void selectionCommandsCopyPathAndHiddenToggleFollowFiles() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QDir root(dir.path());
        const auto writeText = [&root](const QString& name) {
            QFile file(root.filePath(name));
            return file.open(QIODevice::WriteOnly) && file.write("selection payload") > 0;
        };
        QVERIFY(writeText(QStringLiteral("a_sel.txt")));
        QVERIFY(writeText(QStringLiteral("b_sel.txt")));
        QVERIFY(writeText(QStringLiteral(".dot_hidden.txt")));

        sak::FileManagementExplorerPanel panel;
        panel.resize(1100, 700);
        panel.show();
        QVERIFY(QTest::qWaitForWindowExposed(&panel));
        auto* targetList = child<QListWidget>(&panel, "fileExplorerTargetList");
        auto* pathEdit = child<QLineEdit>(&panel, "fileExplorerPathEdit");
        auto* table = child<QTableView>(&panel, "fileExplorerTable");
        QVERIFY(targetList);
        QVERIFY(pathEdit);
        QVERIFY(table);
        if (selectLocalTargetRowForDrive(targetList, pathEdit, dir.path().left(2).toUpper()) < 0) {
            QSKIP("No mounted local target for the temp drive on this test host.");
        }
        QVERIFY(navigateAndFindRow(pathEdit, table, dir.path(), QStringLiteral("a_sel")) >= 0);
        QVERIFY(waitForListingQuiescence(table));

        // Files ToggleShowHiddenItemsAction (Ctrl+H): the dot file appears
        // and disappears with the toggle.
        QCOMPARE(table->model()->rowCount(), 2);
        panel.activateWindow();
        table->setFocus();
        QTest::keyClick(table, Qt::Key_H, Qt::ControlModifier);
        QTRY_COMPARE(table->model()->rowCount(), 3);
        QTest::keyClick(table, Qt::Key_H, Qt::ControlModifier);
        QTRY_COMPARE(table->model()->rowCount(), 2);

        // Files SelectAll (Ctrl+A) / InvertSelection (selection flyout) /
        // ClearSelection (Esc).
        QTest::keyClick(table, Qt::Key_A, Qt::ControlModifier);
        QTRY_COMPARE(table->selectionModel()->selectedRows().size(), 2);
        auto* selectionButton = child<QToolButton>(&panel, "fileExplorerSelectionButton");
        QVERIFY(selectionButton);
        QVERIFY(selectionButton->menu());
        selectionButton->menu()->popup(QPoint(10, 10));
        QApplication::processEvents();
        QAction* invert = actionStartingWith(selectionButton->menu(),
                                             QStringLiteral("Invert Selection"));
        QVERIFY(invert);
        invert->trigger();
        selectionButton->menu()->hide();
        QTRY_COMPARE(table->selectionModel()->selectedRows().size(), 0);
        QVERIFY(selectRowStable(table, QStringLiteral("a_sel")));
        QTest::keyClick(table, Qt::Key_Escape);
        QTRY_COMPARE(table->selectionModel()->selectedRows().size(), 0);

        // Files CopyItemPathAction (Ctrl+Shift+C) and the quoted variant
        // (Ctrl+Alt+C).
        QVERIFY(selectRowStable(table, QStringLiteral("a_sel")));
        QTest::keyClick(table, Qt::Key_C, Qt::ControlModifier | Qt::ShiftModifier);
        // The whole line: contains() passes on a truncated path, a wrong parent, or a second
        // leaked row, and the quote probes never look at what got quoted.
        const QString expected_path = root.filePath(QStringLiteral("a_sel.txt"));
        QTRY_COMPARE(QApplication::clipboard()->text(), expected_path);
        QTest::keyClick(table, Qt::Key_C, Qt::ControlModifier | Qt::AltModifier);
        QTRY_COMPARE(QApplication::clipboard()->text(),
                     QStringLiteral("\"%1\"").arg(expected_path));
    }

    void armAutoDismissMessageBox(QString* captured_text) {
        auto* dismiss = new QTimer(this);
        dismiss->setInterval(50);
        connect(dismiss, &QTimer::timeout, this, [dismiss, captured_text]() {
            if (auto* box = qobject_cast<QMessageBox*>(QApplication::activeModalWidget())) {
                *captured_text = box->text();
                dismiss->stop();
                dismiss->deleteLater();
                box->accept();
            }
        });
        dismiss->start();
    }

    void newFolderOverExistingIsRefusedAndKeepsContents() {
        // New Folder uses mkpath, which succeeds on an existing folder; without a
        // guard it journals a CreateNew whose undo would recycle the pre-existing
        // folder. The guard must refuse the create and leave the folder intact.
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QDir root(dir.path());
        QVERIFY(root.mkpath(QStringLiteral("New Folder")));
        const QString keeper = root.filePath(QStringLiteral("New Folder/keep.txt"));
        {
            QFile file(keeper);
            QVERIFY(file.open(QIODevice::WriteOnly));
            QVERIFY(file.write("must survive") > 0);
        }

        sak::FileManagementExplorerPanel panel;
        panel.resize(1100, 700);
        panel.show();
        QVERIFY(QTest::qWaitForWindowExposed(&panel));
        auto* targetList = child<QListWidget>(&panel, "fileExplorerTargetList");
        auto* pathEdit = child<QLineEdit>(&panel, "fileExplorerPathEdit");
        auto* table = child<QTableView>(&panel, "fileExplorerTable");
        QVERIFY(targetList);
        QVERIFY(pathEdit);
        QVERIFY(table);
        if (selectLocalTargetRowForDrive(targetList, pathEdit, dir.path().left(2).toUpper()) < 0) {
            QSKIP("No mounted local target for the temp drive on this test host.");
        }
        QVERIFY(navigateAndFindRow(pathEdit, table, dir.path(), QStringLiteral("New Folder")) >= 0);
        QVERIFY(waitForListingQuiescence(table));

        // Ctrl+Shift+N auto-accepts the default name "New Folder", which already
        // exists; the guard should warn and refuse.
        QString input_label;
        QString warning_text;
        armAutoAcceptInputDialog(&input_label);
        armAutoDismissMessageBox(&warning_text);
        panel.activateWindow();
        table->setFocus();
        QTest::keyClick(table, Qt::Key_N, Qt::ControlModifier | Qt::ShiftModifier);
        // The OTHER branch of the same guard -- "Could not verify whether %1 already exists
        // here; nothing was created." -- also contains the fragment, so a destinationEntryKind()
        // regression returning Unknown (which refuses genuinely vacant paths too) passed today.
        QTRY_COMPARE(warning_text, QStringLiteral("An item named New Folder already exists here."));

        // The pre-existing folder and its file are untouched, and an undo has
        // nothing to remove (no CreateNew was journaled).
        QVERIFY(QFileInfo(keeper).isFile());
        panel.activateWindow();
        table->setFocus();
        QApplication::processEvents();
        QTest::keyClick(table, Qt::Key_Z, Qt::ControlModifier);
        QApplication::processEvents();
        QVERIFY2(QFileInfo(keeper).isFile(), "undo deleted a pre-existing folder");
    }

    void createFolderWithSelectionSwallowsSelection() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QDir root(dir.path());
        {
            QFile file(root.filePath(QStringLiteral("move_me.txt")));
            QVERIFY(file.open(QIODevice::WriteOnly));
            QVERIFY(file.write("swallow payload") > 0);
        }

        sak::FileManagementExplorerPanel panel;
        panel.resize(1100, 700);
        panel.show();
        QVERIFY(QTest::qWaitForWindowExposed(&panel));
        auto* targetList = child<QListWidget>(&panel, "fileExplorerTargetList");
        auto* pathEdit = child<QLineEdit>(&panel, "fileExplorerPathEdit");
        auto* table = child<QTableView>(&panel, "fileExplorerTable");
        QVERIFY(targetList);
        QVERIFY(pathEdit);
        QVERIFY(table);
        if (selectLocalTargetRowForDrive(targetList, pathEdit, dir.path().left(2).toUpper()) < 0) {
            QSKIP("No mounted local target for the temp drive on this test host.");
        }
        QVERIFY(navigateAndFindRow(pathEdit, table, dir.path(), QStringLiteral("move_me")) >= 0);
        QVERIFY(waitForListingQuiescence(table));

        // Files CreateFolderWithSelectionAction: the prompt (auto-accepted
        // default "New folder") creates the folder and moves the selection
        // into it through the same-target kernel.
        QVERIFY(selectRowStable(table, QStringLiteral("move_me")));
        QString input_label;
        armAutoAcceptInputDialog(&input_label);
        QVERIFY(triggerContextMenuActionStartingWith(
            table->viewport(), QStringLiteral("Create folder with selection")));
        const QString moved = root.filePath(QStringLiteral("New folder/move_me.txt"));
        QTRY_VERIFY2(QFile::exists(moved), "selection was not moved into the new folder");
        QVERIFY(!QFile::exists(root.filePath(QStringLiteral("move_me.txt"))));
    }

    // The Files sort-flyout placement radios (folders first / files first /
    // together) reorder the fixture (mmm_folder, aaa.txt, zzz.txt) live.
    // Takes parameters so QtTest does not run it as a test slot.
    void verifySortPlacementRadios(sak::FileManagementExplorerPanel& panel, QTableView* table) {
        const auto rowText = [table](const int row) {
            return table->model()->index(row, 0).data(Qt::DisplayRole).toString();
        };
        // Files default placement: folders before files.
        QCOMPARE(rowText(0), QStringLiteral("mmm_folder"));
        auto* sortButton = child<QToolButton>(&panel, "fileExplorerSortButton");
        QVERIFY(sortButton);
        QVERIFY(sortButton->menu());
        // The flyout rebuilds on aboutToShow, so pop it up before reading.
        const auto sortMenuAction = [sortButton](const QString& prefix) {
            sortButton->menu()->popup(QPoint(10, 10));
            QApplication::processEvents();
            QAction* action = actionStartingWith(sortButton->menu(), prefix);
            sortButton->menu()->hide();
            return action;
        };
        QAction* files_first = sortMenuAction(QStringLiteral("Sort files first"));
        QVERIFY(files_first);
        files_first->trigger();
        QTRY_COMPARE(rowText(2), QStringLiteral("mmm_folder"));
        QAction* together = sortMenuAction(QStringLiteral("Sort files and folders together"));
        QVERIFY(together);
        together->trigger();
        QTRY_COMPARE(rowText(1), QStringLiteral("mmm_folder"));
        QAction* folders_first = sortMenuAction(QStringLiteral("Sort folders first"));
        QVERIFY(folders_first);
        folders_first->trigger();
        QTRY_COMPARE(rowText(0), QStringLiteral("mmm_folder"));
    }

    void initialListingIsSortedByNameAscending() {
        // Files SortOption default: Name ascending with folders first, before
        // the user ever touches a header or the sort flyout (A/B screenshot
        // pass caught the listing in raw enumeration order).
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QDir root(dir.path());
        QVERIFY(root.mkdir(QStringLiteral("nnn_dir")));
        const auto writeText = [&root](const QString& name) {
            QFile file(root.filePath(name));
            return file.open(QIODevice::WriteOnly) && file.write("sorted payload") > 0;
        };
        QVERIFY(writeText(QStringLiteral("zzz.txt")));
        QVERIFY(writeText(QStringLiteral("aaa.txt")));

        sak::FileManagementExplorerPanel panel;
        panel.resize(1100, 700);
        panel.show();
        QVERIFY(QTest::qWaitForWindowExposed(&panel));
        auto* targetList = child<QListWidget>(&panel, "fileExplorerTargetList");
        auto* pathEdit = child<QLineEdit>(&panel, "fileExplorerPathEdit");
        auto* table = child<QTableView>(&panel, "fileExplorerTable");
        QVERIFY(targetList);
        QVERIFY(pathEdit);
        QVERIFY(table);
        if (selectLocalTargetRowForDrive(targetList, pathEdit, dir.path().left(2).toUpper()) < 0) {
            QSKIP("No mounted local target for the temp drive on this test host.");
        }
        QVERIFY(navigateAndFindRow(pathEdit, table, dir.path(), QStringLiteral("aaa")) >= 0);
        const auto rowText = [table](const int row) {
            return table->model()->index(row, 0).data(Qt::DisplayRole).toString();
        };
        QVERIFY2(rowText(0).startsWith(QStringLiteral("nnn_dir")), qPrintable(rowText(0)));
        QVERIFY2(rowText(1).startsWith(QStringLiteral("aaa")), qPrintable(rowText(1)));
        QVERIFY2(rowText(2).startsWith(QStringLiteral("zzz")), qPrintable(rowText(2)));
    }

    void newFileCreatesEmptyAndSortPlacementReorders() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QDir root(dir.path());
        QVERIFY(root.mkdir(QStringLiteral("mmm_folder")));
        const auto writeText = [&root](const QString& name) {
            QFile file(root.filePath(name));
            if (!file.open(QIODevice::WriteOnly)) {
                return false;
            }
            return file.write("placement payload") > 0;
        };
        QVERIFY(writeText(QStringLiteral("aaa.txt")));
        QVERIFY(writeText(QStringLiteral("zzz.txt")));

        sak::FileManagementExplorerPanel panel;
        panel.resize(1100, 700);
        panel.show();
        QVERIFY(QTest::qWaitForWindowExposed(&panel));
        auto* targetList = child<QListWidget>(&panel, "fileExplorerTargetList");
        auto* pathEdit = child<QLineEdit>(&panel, "fileExplorerPathEdit");
        auto* table = child<QTableView>(&panel, "fileExplorerTable");
        QVERIFY(targetList);
        QVERIFY(pathEdit);
        QVERIFY(table);
        if (selectLocalTargetRowForDrive(targetList, pathEdit, dir.path().left(2).toUpper()) < 0) {
            QSKIP("No mounted local target for the temp drive on this test host.");
        }
        QVERIFY(navigateAndFindRow(pathEdit, table, dir.path(), QStringLiteral("aaa")) >= 0);
        QVERIFY(waitForListingQuiescence(table));

        verifySortPlacementRadios(panel, table);
        if (QTest::currentTestFailed()) {
            return;
        }

        // Files New > File (CreateFileAction): name prompt (auto-accepted
        // default "New File"), then an empty file lands through the bridge.
        table->clearSelection();
        QString input_label;
        armAutoAcceptInputDialog(&input_label);
        const QPoint empty_spot(table->viewport()->rect().center().x(),
                                table->viewport()->rect().bottom() - 4);
        QVERIFY(triggerContextSubmenuAction(table->viewport(),
                                            QStringLiteral("fileExplorerContextNewMenu"),
                                            QStringLiteral("File"),
                                            empty_spot));
        const QString created = root.filePath(QStringLiteral("New File"));
        QTRY_VERIFY(QFileInfo::exists(created));
        QCOMPARE(QFileInfo(created).size(), qint64(0));
    }

    void itemContextMenuFollowsFilesOrder() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        {
            QFile file(QDir(dir.path()).filePath(QStringLiteral("menu_probe.txt")));
            QVERIFY(file.open(QIODevice::WriteOnly));
            QVERIFY(file.write("menu probe") > 0);
        }
        sak::FileManagementExplorerPanel panel;
        panel.resize(1100, 700);
        panel.show();
        QVERIFY(QTest::qWaitForWindowExposed(&panel));
        auto* targetList = child<QListWidget>(&panel, "fileExplorerTargetList");
        auto* pathEdit = child<QLineEdit>(&panel, "fileExplorerPathEdit");
        auto* table = child<QTableView>(&panel, "fileExplorerTable");
        QVERIFY(targetList);
        QVERIFY(pathEdit);
        QVERIFY(table);
        if (selectLocalTargetRowForDrive(targetList, pathEdit, dir.path().left(2).toUpper()) < 0) {
            QSKIP("No mounted local target for the temp drive on this test host.");
        }
        QVERIFY(navigateAndFindRow(pathEdit, table, dir.path(), QStringLiteral("menu_probe")) >= 0);
        QVERIFY(waitForListingQuiescence(table));
        QVERIFY(selectRowStable(table, QStringLiteral("menu_probe")));

        // Files ContentPageContextFlyoutFactory order for a selection: these
        // anchors must appear in this exact relative order.
        const QStringList actions = collectContextMenuTexts(table->viewport());
        const QStringList anchors = {QStringLiteral("Open"),
                                     QStringLiteral("Cut"),
                                     QStringLiteral("Copy"),
                                     QStringLiteral("Rename"),
                                     QStringLiteral("Delete"),
                                     QStringLiteral("Properties"),
                                     QStringLiteral("Tags"),
                                     QStringLiteral("Edit in Notepad"),
                                     QStringLiteral("Open in Windows Terminal"),
                                     QStringLiteral("Preview")};
        int cursor = -1;
        for (const QString& anchor : anchors) {
            int found = -1;
            for (int index = cursor + 1; index < actions.size(); ++index) {
                if (actions.at(index).startsWith(anchor)) {
                    found = index;
                    break;
                }
            }
            QVERIFY2(found > cursor,
                     qPrintable(QStringLiteral("anchor '%1' out of order in: %2")
                                    .arg(anchor, actions.join(QStringLiteral(" | ")))));
            cursor = found;
        }
        // Background-only groups never leak into the selection menu.
        QVERIFY(!containsTextStartingWith(actions, QStringLiteral("Layout")));
        QVERIFY(!containsTextStartingWith(actions, QStringLiteral("Sort by")));
    }

    void responsiveLayoutCollapsesAtNarrowWidth() {
        sak::FileManagementExplorerPanel panel;
        panel.resize(680, 640);
        panel.show();
        QVERIFY(QTest::qWaitForWindowExposed(&panel));

        auto* targetList = child<QListWidget>(&panel, "fileExplorerTargetList");
        auto* details = child<QWidget>(&panel, "fileExplorerInfoPane");
        auto* table = child<QTableView>(&panel, "fileExplorerTable");
        QVERIFY(targetList);
        QVERIFY(details);
        QVERIFY(table);
        QApplication::processEvents();

        QResizeEvent narrowResize(QSize(680, 640), panel.size());
        QApplication::sendEvent(&panel, &narrowResize);
        QApplication::processEvents();

        QVERIFY(!targetList->isVisible());
        QVERIFY(!details->isVisible());
        QVERIFY(table->isVisible());
        captureBaseline(&panel, QStringLiteral("narrow"));

        // Files adaptive triggers restore both panes once the shell widens
        // again (a transient narrow resize must not hide them for good).
        panel.resize(1200, 640);
        QApplication::processEvents();
        QVERIFY(targetList->isVisible());
        QVERIFY(details->isVisible());

        // The info-pane toggle is a user preference: once switched off it
        // stays off through adaptive resizes (Files IsInfoPaneEnabled).
        auto* detailsToggle = child<QPushButton>(&panel, "fileExplorerDetailsToggleButton");
        QVERIFY(detailsToggle);
        detailsToggle->click();
        QApplication::processEvents();
        QVERIFY(!details->isVisible());
        panel.resize(1300, 640);
        QApplication::processEvents();
        QVERIFY(!details->isVisible());
        detailsToggle->click();
        QApplication::processEvents();
        QVERIFY(details->isVisible());
    }

    void paneStateLabelTracksLoadingEmptyAndError() {
        sak::FileExplorerPane pane;
        pane.resize(800, 500);
        pane.show();
        QVERIFY(QTest::qWaitForWindowExposed(&pane));

        auto* state = child<QLabel>(&pane, "fileExplorerStateLabel");
        QVERIFY(state);
        QVERIFY(!state->isVisible());

        pane.showLoadingState(QStringLiteral("Loading /fixture..."));
        QVERIFY(state->isVisible());
        QCOMPARE(state->text(), QStringLiteral("Loading /fixture..."));

        pane.showEmptyState(QStringLiteral("This folder is empty."));
        QVERIFY(state->isVisible());
        QCOMPARE(state->text(), QStringLiteral("This folder is empty."));

        pane.showErrorState(QStringLiteral("Listing failed."));
        QVERIFY(state->isVisible());
        QCOMPARE(state->text(), QStringLiteral("Listing failed."));

        pane.showReadyState();
        QVERIFY(!state->isVisible());
    }

    void detailsColumnResizePersists() {
        QCoreApplication::setOrganizationName(QStringLiteral("SAKUtilityTests"));
        QCoreApplication::setApplicationName(QStringLiteral("FileExplorerPanelTests"));
        QSettings settings;
        settings.beginGroup(QStringLiteral("FileExplorerDetailsView"));
        settings.remove(QString());
        settings.endGroup();

        sak::FileExplorerItemModel source;
        source.setEntries({testEntry(QStringLiteral("Docs"), true),
                           testEntry(QStringLiteral("note.txt"), false)});
        sak::FileExplorerSortFilterModel proxy;
        proxy.setSourceModel(&source);

        {
            sak::FileExplorerDetailsView view;
            view.setModel(&proxy);
            view.resize(900, 400);
            view.show();
            QVERIFY(QTest::qWaitForWindowExposed(&view));
            QCOMPARE(view.model()->columnCount(),
                     static_cast<int>(sak::FileExplorerItemModel::ColumnCount));
            view.horizontalHeader()->resizeSection(sak::FileExplorerItemModel::NameColumn, 321);
            QCOMPARE(view.columnWidth(sak::FileExplorerItemModel::NameColumn), 321);
            view.saveColumnState();
        }
        QSettings savedSettings;
        savedSettings.beginGroup(QStringLiteral("FileExplorerDetailsView"));
        const QVariantList savedWidths =
            savedSettings.value(QStringLiteral("ColumnWidths")).toList();
        savedSettings.endGroup();
        QVERIFY(!savedWidths.isEmpty());
        QCOMPARE(savedWidths.at(sak::FileExplorerItemModel::NameColumn).toInt(), 321);

        sak::FileExplorerDetailsView restored;
        restored.setModel(&proxy);
        restored.resize(900, 400);
        restored.show();
        QVERIFY(QTest::qWaitForWindowExposed(&restored));
        QCOMPARE(restored.columnWidth(sak::FileExplorerItemModel::NameColumn), 321);

        settings.beginGroup(QStringLiteral("FileExplorerDetailsView"));
        settings.remove(QString());
        settings.endGroup();
    }

    void doubleClickDirectoryOpensFolderWhenAvailable() {
        sak::FileManagementExplorerPanel panel;
        panel.resize(1100, 700);
        panel.show();
        QVERIFY(QTest::qWaitForWindowExposed(&panel));

        auto* table = child<QTableView>(&panel, "fileExplorerTable");
        auto* pathEdit = child<QLineEdit>(&panel, "fileExplorerPathEdit");
        auto* targetList = child<QListWidget>(&panel, "fileExplorerTargetList");
        QVERIFY(table);
        QVERIFY(pathEdit);
        QVERIFY(targetList);
        if (firstTargetRow(targetList) < 0) {
            QSKIP("No mounted File Explorer targets on this test host.");
        }

        QTRY_VERIFY(table->model() && table->model()->rowCount() > 0);
        int directoryRow = -1;
        for (int row = 0; row < table->model()->rowCount(); ++row) {
            const QModelIndex index = table->model()->index(row,
                                                            sak::FileExplorerItemModel::NameColumn);
            if (index.data(sak::FileExplorerItemModel::EntryDirectoryRole).toBool()) {
                directoryRow = row;
                break;
            }
        }
        if (directoryRow < 0) {
            QSKIP("No directory row available on this host target.");
        }

        const QString beforePath = pathEdit->text();
        const QModelIndex index = table->model()->index(directoryRow,
                                                        sak::FileExplorerItemModel::NameColumn);
        table->selectRow(directoryRow);
        QVERIFY(QMetaObject::invokeMethod(
            &panel, "onItemDoubleClicked", Qt::DirectConnection, Q_ARG(QModelIndex, index)));
        QTRY_VERIFY(pathEdit->text() != beforePath);
    }

    void shortcutsRefreshAndToggleDetailsWithoutLosingPath() {
        sak::FileManagementExplorerPanel panel;
        panel.resize(1100, 700);
        panel.show();
        QVERIFY(QTest::qWaitForWindowExposed(&panel));

        auto* targetList = child<QListWidget>(&panel, "fileExplorerTargetList");
        auto* pathEdit = child<QLineEdit>(&panel, "fileExplorerPathEdit");
        auto* details = child<QWidget>(&panel, "fileExplorerInfoPane");
        QVERIFY(targetList);
        QVERIFY(pathEdit);
        QVERIFY(details);
        if (firstTargetRow(targetList) < 0) {
            QSKIP("No mounted File Explorer targets on this test host.");
        }

        const QString beforePath = pathEdit->text();
        panel.setFocus();
        QTest::keyClick(&panel, Qt::Key_F5);
        QApplication::processEvents();
        QCOMPARE(pathEdit->text(), beforePath);

        QVERIFY(details->isVisible());
        QTest::keyClick(&panel, Qt::Key_I, Qt::ControlModifier | Qt::AltModifier);
        QApplication::processEvents();
        QVERIFY(!details->isVisible());
        QTest::keyClick(&panel, Qt::Key_I, Qt::ControlModifier | Qt::AltModifier);
        QApplication::processEvents();
        QVERIFY(details->isVisible());
    }

    void filesTabHotkeysDriveTabStrip() {
        sak::FileManagementExplorerPanel panel;
        panel.resize(1100, 700);
        panel.show();
        QVERIFY(QTest::qWaitForWindowExposed(&panel));

        auto* tabBar = child<QTabBar>(&panel, "fileExplorerTabBar");
        QVERIFY(tabBar);
        const int before = tabBar->count();
        panel.setFocus();

        // Files NewTabAction: Ctrl+T opens a tab at the current location.
        QTest::keyClick(&panel, Qt::Key_T, Qt::ControlModifier);
        QApplication::processEvents();
        QCOMPARE(tabBar->count(), before + 1);

        // MainPage accelerators: Ctrl+1 selects the first tab, Ctrl+9 the last.
        QTest::keyClick(&panel, Qt::Key_1, Qt::ControlModifier);
        QApplication::processEvents();
        QCOMPARE(tabBar->currentIndex(), 0);
        QTest::keyClick(&panel, Qt::Key_9, Qt::ControlModifier);
        QApplication::processEvents();
        QCOMPARE(tabBar->currentIndex(), tabBar->count() - 1);

        // Files NextTabAction: Ctrl+Tab cycles (wraps to the first tab here).
        QTest::keyClick(&panel, Qt::Key_Tab, Qt::ControlModifier);
        QApplication::processEvents();
        QCOMPARE(tabBar->currentIndex(), 0);

        // Files CloseSelectedTabAction: Ctrl+W closes the current tab.
        QTest::keyClick(&panel, Qt::Key_W, Qt::ControlModifier);
        QApplication::processEvents();
        QCOMPARE(tabBar->count(), before);
    }

    void inlineRenameCommitsThroughBridgeAndRevertsOnEscape() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString source_file = QDir(dir.path()).filePath(QStringLiteral("alpha.txt"));
        {
            QFile file(source_file);
            QVERIFY(file.open(QIODevice::WriteOnly));
            QVERIFY(file.write("inline rename payload") > 0);
        }

        sak::FileManagementExplorerPanel panel;
        panel.resize(1100, 700);
        panel.show();
        QVERIFY(QTest::qWaitForWindowExposed(&panel));
        auto* targetList = child<QListWidget>(&panel, "fileExplorerTargetList");
        auto* pathEdit = child<QLineEdit>(&panel, "fileExplorerPathEdit");
        auto* table = child<QTableView>(&panel, "fileExplorerTable");
        QVERIFY(targetList);
        QVERIFY(pathEdit);
        QVERIFY(table);
        if (selectLocalTargetRowForDrive(targetList, pathEdit, dir.path().left(2).toUpper()) < 0) {
            QSKIP("No mounted local target for the temp drive on this test host.");
        }
        const int row = navigateAndFindRow(pathEdit, table, dir.path(), QStringLiteral("alpha"));
        QVERIFY2(row >= 0, "source file not listed after navigation");
        table->selectRow(row);
        QApplication::processEvents();

        // F2 (Files RenameAction) opens the inline editor with the base name
        // selected and the extension excluded.
        QTest::keyClick(&panel, Qt::Key_F2);
        auto* editor = table->findChild<QLineEdit*>(QStringLiteral("fileExplorerRenameEditor"));
        QVERIFY2(editor, "inline rename editor did not open");
        // Base-name selection is applied one event-loop tick after open.
        QTRY_COMPARE(editor->selectedText(), QStringLiteral("alpha"));

        // Restricted characters are stripped live (Files BeforeTextChanging).
        QTest::keyClicks(editor, QStringLiteral("be?ta"));
        QCOMPARE(editor->text(), QStringLiteral("beta.txt"));

        // Enter commits through the bridge; the file is renamed on disk.
        QTest::keyClick(editor, Qt::Key_Return);
        QVERIFY2(QTest::qWaitFor(
                     [&dir]() {
                         return QFile::exists(
                             QDir(dir.path()).filePath(QStringLiteral("beta.txt")));
                     },
                     5000),
                 "rename did not land on disk");
        QVERIFY(!QFile::exists(source_file));

        // Escape reverts: the editor claims the key ahead of the panel's
        // clear-selection shortcut and no rename is issued.
        QVERIFY(navigateAndFindRow(pathEdit, table, dir.path(), QStringLiteral("beta")) >= 0);
        QVERIFY(selectRowStable(table, QStringLiteral("beta")));
        QTest::keyClick(&panel, Qt::Key_F2);
        auto* second_editor =
            table->findChild<QLineEdit*>(QStringLiteral("fileExplorerRenameEditor"));
        QVERIFY(second_editor);
        QTest::keyClicks(second_editor, QStringLiteral("gamma"));
        QTest::keyClick(second_editor, Qt::Key_Escape);
        QTest::qWait(200);
        QVERIFY(QFile::exists(QDir(dir.path()).filePath(QStringLiteral("beta.txt"))));
        QVERIFY(!QFile::exists(QDir(dir.path()).filePath(QStringLiteral("gamma.txt"))));
    }

    void cutPasteIntoSelectedFolderMovesFile() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QVERIFY(QDir(dir.path()).mkdir(QStringLiteral("dest")));
        const QString source_file = QDir(dir.path()).filePath(QStringLiteral("mover.txt"));
        {
            QFile file(source_file);
            QVERIFY(file.open(QIODevice::WriteOnly));
            QVERIFY(file.write("move payload") > 0);
        }

        sak::FileManagementExplorerPanel panel;
        panel.resize(1100, 700);
        panel.show();
        QVERIFY(QTest::qWaitForWindowExposed(&panel));
        auto* targetList = child<QListWidget>(&panel, "fileExplorerTargetList");
        auto* pathEdit = child<QLineEdit>(&panel, "fileExplorerPathEdit");
        auto* table = child<QTableView>(&panel, "fileExplorerTable");
        QVERIFY(targetList);
        QVERIFY(pathEdit);
        QVERIFY(table);
        if (selectLocalTargetRowForDrive(targetList, pathEdit, dir.path().left(2).toUpper()) < 0) {
            QSKIP("No mounted local target for the temp drive on this test host.");
        }
        QVERIFY(navigateAndFindRow(pathEdit, table, dir.path(), QStringLiteral("mover")) >= 0);

        // Files CutItemAction: Ctrl+X publishes a move operation and dims
        // the cut row.
        QVERIFY(selectRowStable(table, QStringLiteral("mover")));
        QTest::keyClick(table, Qt::Key_X, Qt::ControlModifier);
        const QMimeData* mime = QApplication::clipboard()->mimeData();
        QVERIFY(mime && mime->hasFormat(QStringLiteral("application/x-sak-file-explorer-items")));
        QVERIFY(
            QString::fromUtf8(mime->data(QStringLiteral("application/x-sak-file-explorer-items")))
                .contains(QStringLiteral("\"operation\":\"move\"")));

        // Files PasteItemToSelectionAction (Ctrl+Shift+V): pasting a cut
        // file into the selected folder moves it (same-target renameEntry).
        QVERIFY(selectRowStable(table, QStringLiteral("dest")));
        QTest::keyClick(table, Qt::Key_V, Qt::ControlModifier | Qt::ShiftModifier);
        const QString moved = QDir(dir.path()).filePath(QStringLiteral("dest/mover.txt"));
        QVERIFY2(QTest::qWaitFor([&moved]() { return QFile::exists(moved); }, 5000),
                 "cut-paste did not move the file");
        QVERIFY(!QFile::exists(source_file));
        // A completed move consumes the clipboard (ReportOperationCompleted).
        QTRY_VERIFY(!QApplication::clipboard()->mimeData() ||
                    !QApplication::clipboard()->mimeData()->hasFormat(
                        QStringLiteral("application/x-sak-file-explorer-items")));
    }

    void copyPasteInSameFolderDuplicatesWithIncrementalName() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString source_file = QDir(dir.path()).filePath(QStringLiteral("dup.txt"));
        {
            QFile file(source_file);
            QVERIFY(file.open(QIODevice::WriteOnly));
            QVERIFY(file.write("duplicate payload") > 0);
        }

        sak::FileManagementExplorerPanel panel;
        panel.resize(1100, 700);
        panel.show();
        QVERIFY(QTest::qWaitForWindowExposed(&panel));
        auto* targetList = child<QListWidget>(&panel, "fileExplorerTargetList");
        auto* pathEdit = child<QLineEdit>(&panel, "fileExplorerPathEdit");
        auto* table = child<QTableView>(&panel, "fileExplorerTable");
        QVERIFY(targetList);
        QVERIFY(pathEdit);
        QVERIFY(table);
        if (selectLocalTargetRowForDrive(targetList, pathEdit, dir.path().left(2).toUpper()) < 0) {
            QSKIP("No mounted local target for the temp drive on this test host.");
        }
        QVERIFY(navigateAndFindRow(pathEdit, table, dir.path(), QStringLiteral("dup")) >= 0);

        // Files same-folder paste: no dialog, the copy lands as
        // "dup (2).txt" (GetIncrementalName starts at 2).
        QVERIFY(selectRowStable(table, QStringLiteral("dup")));
        QTest::keyClick(table, Qt::Key_C, Qt::ControlModifier);
        QTest::keyClick(table, Qt::Key_V, Qt::ControlModifier);
        const QString duplicate = QDir(dir.path()).filePath(QStringLiteral("dup (2).txt"));
        QVERIFY2(QTest::qWaitFor([&duplicate]() { return QFile::exists(duplicate); }, 5000),
                 "same-folder paste did not duplicate");
        QVERIFY(QFile::exists(source_file));
    }

    void folderCopyPasteRecursesGuardsSubtreeAndMoves() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QDir root(dir.path());
        // Folder names must not contain each other: selectRowStable matches by
        // fragment, and the listing order depends on the persisted sort.
        QVERIFY(root.mkpath(QStringLiteral("bundle/deep")));
        QVERIFY(root.mkdir(QStringLiteral("copy_pocket")));
        QVERIFY(root.mkdir(QStringLiteral("move_pocket")));
        const auto writeFile = [](const QString& path, const QByteArray& data) {
            QFile file(path);
            QVERIFY(file.open(QIODevice::WriteOnly));
            QCOMPARE(file.write(data), data.size());
        };
        writeFile(root.filePath(QStringLiteral("bundle/inner.txt")),
                  QByteArrayLiteral("inner payload"));
        writeFile(root.filePath(QStringLiteral("bundle/deep/leaf.txt")),
                  QByteArrayLiteral("leaf payload"));

        sak::FileManagementExplorerPanel panel;
        panel.resize(1100, 700);
        panel.show();
        QVERIFY(QTest::qWaitForWindowExposed(&panel));
        auto* targetList = child<QListWidget>(&panel, "fileExplorerTargetList");
        auto* pathEdit = child<QLineEdit>(&panel, "fileExplorerPathEdit");
        auto* table = child<QTableView>(&panel, "fileExplorerTable");
        QVERIFY(targetList);
        QVERIFY(pathEdit);
        QVERIFY(table);
        if (selectLocalTargetRowForDrive(targetList, pathEdit, dir.path().left(2).toUpper()) < 0) {
            QSKIP("No mounted local target for the temp drive on this test host.");
        }
        QVERIFY(navigateAndFindRow(pathEdit, table, dir.path(), QStringLiteral("bundle")) >= 0);
        // A late listing reset would wipe the selection and reroute the paste
        // to the current folder; settle before selecting. The panel shortcuts
        // are focus-scoped, so pin the focus before the first key sequence.
        QVERIFY(waitForListingQuiescence(table));
        panel.activateWindow();
        table->setFocus();
        QApplication::processEvents();

        // Copying a folder and pasting it into another folder lands the whole
        // tree (importDirectoryFromHost), source left intact.
        QVERIFY(selectRowStable(table, QStringLiteral("bundle")));
        QTest::keyClick(table, Qt::Key_C, Qt::ControlModifier);
        QVERIFY(selectRowStable(table, QStringLiteral("copy_pocket")));
        QTest::keyClick(table, Qt::Key_V, Qt::ControlModifier | Qt::ShiftModifier);
        const QString copied_leaf =
            root.filePath(QStringLiteral("copy_pocket/bundle/deep/leaf.txt"));
        // inner.txt sorts after the "deep" subdirectory, so the walk writes it LAST;
        // waiting on the leaf alone races the still-unwritten inner.txt. Wait on both.
        const QString copied_inner = root.filePath(QStringLiteral("copy_pocket/bundle/inner.txt"));
        QVERIFY2(QTest::qWaitFor(
                     [&copied_leaf, &copied_inner]() {
                         return QFile::exists(copied_leaf) && QFile::exists(copied_inner);
                     },
                     5000),
                 "folder paste did not recurse");
        QFile copied(copied_inner);
        QVERIFY(copied.open(QIODevice::ReadOnly));
        QCOMPARE(copied.readAll(), QByteArrayLiteral("inner payload"));
        QVERIFY(QFile::exists(root.filePath(QStringLiteral("bundle/inner.txt"))));

        verifyFolderSubtreeGuardAndCutMove(panel, pathEdit, table, dir.path());
    }

    void verifyFolderSubtreeGuardAndCutMove(sak::FileManagementExplorerPanel& panel,
                                            QLineEdit* pathEdit,
                                            QTableView* table,
                                            const QString& root_path) {
        const QDir root(root_path);

        // Pasting a folder into its own subtree fails closed (Files
        // ShellFilesystemOperations subtree guard); the warning box is captured
        // and dismissed by a timer so the run stays non-interactive.
        QVERIFY(navigateAndFindRow(pathEdit, table, root_path, QStringLiteral("bundle")) >= 0);
        // The copy-paste reload above resets the model; settle before selecting.
        QVERIFY(waitForListingQuiescence(table));
        QVERIFY(selectRowStable(table, QStringLiteral("bundle")));
        QTest::keyClick(table, Qt::Key_C, Qt::ControlModifier);
        QString warning_text;
        QTimer dismiss;
        dismiss.setInterval(50);
        connect(&dismiss, &QTimer::timeout, this, [&warning_text, &dismiss]() {
            if (auto* box = qobject_cast<QMessageBox*>(QApplication::activeModalWidget())) {
                warning_text = box->text();
                dismiss.stop();
                box->close();
            }
        });
        dismiss.start();
        QTest::keyClick(table, Qt::Key_V, Qt::ControlModifier | Qt::ShiftModifier);
        dismiss.stop();
        // Ctrl+C makes this a COPY, so the paste wording is the contract; the move and copy
        // siblings at two other call sites carry the same fragment.
        QCOMPARE(warning_text, QStringLiteral("Cannot paste bundle into its own subfolder."));
        QVERIFY(!QDir(root.filePath(QStringLiteral("bundle/bundle"))).exists());

        // The dismissed modal cleared the focus; the panel shortcuts are
        // focus-scoped, so restore it before the next key sequence.
        panel.activateWindow();
        table->setFocus();
        QApplication::processEvents();

        // Cut + paste into another folder is a real same-target folder move
        // (renameEntry): the tree relocates and the source disappears.
        QVERIFY(selectRowStable(table, QStringLiteral("bundle")));
        QTest::keyClick(table, Qt::Key_X, Qt::ControlModifier);
        QVERIFY(selectRowStable(table, QStringLiteral("move_pocket")));
        QTest::keyClick(table, Qt::Key_V, Qt::ControlModifier | Qt::ShiftModifier);
        const QString moved_leaf =
            root.filePath(QStringLiteral("move_pocket/bundle/deep/leaf.txt"));
        QVERIFY2(QTest::qWaitFor([&moved_leaf]() { return QFile::exists(moved_leaf); }, 5000),
                 "folder cut-paste did not move the tree");
        QVERIFY(!QDir(root.filePath(QStringLiteral("bundle"))).exists());
    }

    // Deliver a synthetic drag-enter + drop at @p pos on the view's viewport.
    // Takes parameters so QtTest does not run it as a test slot.
    void sendDrop(QAbstractItemView* view,
                  const QPointF& pos,
                  const QMimeData* mime,
                  Qt::KeyboardModifiers modifiers) {
        QDragEnterEvent enter(
            pos.toPoint(), Qt::CopyAction | Qt::MoveAction, mime, Qt::LeftButton, modifiers);
        QApplication::sendEvent(view->viewport(), &enter);
        QDropEvent drop(pos, Qt::CopyAction | Qt::MoveAction, mime, Qt::LeftButton, modifiers);
        QApplication::sendEvent(view->viewport(), &drop);
        QApplication::processEvents();
    }

    void dropMovesWithinTargetAndCopiesExternalUrls() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QDir root(dir.path());
        QVERIFY(root.mkdir(QStringLiteral("drop_zone")));
        {
            QFile file(root.filePath(QStringLiteral("dragme.txt")));
            QVERIFY(file.open(QIODevice::WriteOnly));
            QVERIFY(file.write("drag payload") > 0);
        }
        QTemporaryDir outside_dir;
        QVERIFY(outside_dir.isValid());
        const QString outside = QDir(outside_dir.path()).filePath(QStringLiteral("outside.txt"));
        {
            QFile file(outside);
            QVERIFY(file.open(QIODevice::WriteOnly));
            QVERIFY(file.write("external payload") > 0);
        }

        sak::FileManagementExplorerPanel panel;
        panel.resize(1100, 700);
        panel.show();
        QVERIFY(QTest::qWaitForWindowExposed(&panel));
        auto* targetList = child<QListWidget>(&panel, "fileExplorerTargetList");
        auto* pathEdit = child<QLineEdit>(&panel, "fileExplorerPathEdit");
        auto* table = child<QTableView>(&panel, "fileExplorerTable");
        QVERIFY(targetList);
        QVERIFY(pathEdit);
        QVERIFY(table);
        if (selectLocalTargetRowForDrive(targetList, pathEdit, dir.path().left(2).toUpper()) < 0) {
            QSKIP("No mounted local target for the temp drive on this test host.");
        }
        const int zone_row =
            navigateAndFindRow(pathEdit, table, dir.path(), QStringLiteral("drop_zone"));
        QVERIFY(zone_row >= 0);
        QVERIFY(waitForListingQuiescence(table));
        panel.activateWindow();
        table->setFocus();
        QApplication::processEvents();

        // Same-target drop with no modifiers is a MOVE (Files DragDropHelpers
        // cascade): reuse the copy payload shape via Ctrl+C, drop onto the folder row.
        QVERIFY(selectRowStable(table, QStringLiteral("dragme")));
        QTest::keyClick(table, Qt::Key_C, Qt::ControlModifier);
        const QMimeData* clip = QApplication::clipboard()->mimeData();
        QVERIFY(clip && clip->hasFormat(QStringLiteral("application/x-sak-file-explorer-items")));
        QMimeData drag_mime;
        drag_mime.setData(QStringLiteral("application/x-sak-file-explorer-items"),
                          clip->data(QStringLiteral("application/x-sak-file-explorer-items")));
        const QPointF zone_center = table->visualRect(table->model()->index(zone_row, 0)).center();
        sendDrop(table, zone_center, &drag_mime, Qt::NoModifier);
        const QString moved = root.filePath(QStringLiteral("drop_zone/dragme.txt"));
        QVERIFY2(QTest::qWaitFor([&moved]() { return QFile::exists(moved); }, 5000),
                 "drop did not move the file into the folder");
        QVERIFY(!QFile::exists(root.filePath(QStringLiteral("dragme.txt"))));

        // External URL drop onto empty space copies into the current folder.
        QMimeData url_mime;
        url_mime.setUrls({QUrl::fromLocalFile(outside)});
        const QPointF empty_spot(table->viewport()->rect().bottomRight() - QPoint(4, 4));
        sendDrop(table, empty_spot, &url_mime, Qt::NoModifier);
        const QString copied = root.filePath(QStringLiteral("outside.txt"));
        QVERIFY2(QTest::qWaitFor([&copied]() { return QFile::exists(copied); }, 5000),
                 "external drop did not copy the file");
        QVERIFY(QFile::exists(outside));
    }

    // Auto-accept the next modal question box (clicks Yes) and capture its text,
    // keeping offscreen runs non-interactive. Takes a parameter so QtTest does
    // not run it as a test slot.
    void armAutoAcceptQuestion(QString* captured) {
        auto* accept = new QTimer(this);
        accept->setInterval(50);
        connect(accept, &QTimer::timeout, this, [accept, captured]() {
            if (auto* box = qobject_cast<QMessageBox*>(QApplication::activeModalWidget())) {
                *captured = box->text();
                accept->stop();
                accept->deleteLater();
                box->button(QMessageBox::Yes)->click();
            }
        });
        accept->start();
    }

    // confirmHistoryDelete builds a fixed four-part message -- header, path list, scope
    // notes, "Continue?" -- and hands it to ui::asLiteralRichText, which escapes it and
    // turns each newline into <br/>. Pinning the head and the tail leaves only the
    // host-dependent temp path list unchecked.
    static void verifyUndoConfirmation(const QString& question_text,
                                       const QString& header,
                                       const QString& scope) {
        QVERIFY2(question_text.startsWith(sak::ui::literalRichTextOpenTag() + header +
                                          QStringLiteral("<br/><br/>")),
                 qPrintable(question_text));
        QVERIFY2(question_text.endsWith(QStringLiteral("<br/><br/>") + scope +
                                        QStringLiteral("<br/><br/>Continue?") +
                                        sak::ui::literalRichTextCloseTag()),
                 qPrintable(question_text));
    }

    // deleteConfirmationText() is "<head>\n\n<paths>" through the same wrapper; the paths
    // are host-dependent, so the tail is anchored on the entry name that is about to be
    // destroyed -- the field the old fragment probes never looked at.
    static void verifyDeleteConfirmation(const QString& question_text,
                                         const QString& head,
                                         const QString& entry_name) {
        QVERIFY2(question_text.startsWith(sak::ui::literalRichTextOpenTag() + head),
                 qPrintable(question_text));
        QVERIFY2(question_text.endsWith(entry_name + sak::ui::literalRichTextCloseTag()),
                 qPrintable(question_text));
    }

    // The permanent leg names the host-dependent target label between two fixed halves, so
    // the middle is bridged by a third anchor instead of being pinned.
    static void verifyPermanentDeleteConfirmation(const QString& question_text,
                                                  const QString& entry_name) {
        verifyDeleteConfirmation(question_text,
                                 QStringLiteral("Delete 1 item(s) from '"),
                                 entry_name);
        QVERIFY2(question_text.contains(QStringLiteral("'? This permanently removes data from "
                                                       "the selected target.<br/><br/>")),
                 qPrintable(question_text));
    }

    void deleteRecyclesLocallyAndShiftDeleteIsPermanent() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QDir root(dir.path());
        const auto makeFile = [&root](const QString& name) {
            QFile file(root.filePath(name));
            QVERIFY(file.open(QIODevice::WriteOnly));
            QVERIFY(file.write("delete payload") > 0);
        };
        makeFile(QStringLiteral("bin_me.txt"));
        makeFile(QStringLiteral("perma.txt"));

        sak::FileManagementExplorerPanel panel;
        panel.resize(1100, 700);
        panel.show();
        QVERIFY(QTest::qWaitForWindowExposed(&panel));
        auto* targetList = child<QListWidget>(&panel, "fileExplorerTargetList");
        auto* pathEdit = child<QLineEdit>(&panel, "fileExplorerPathEdit");
        auto* table = child<QTableView>(&panel, "fileExplorerTable");
        QVERIFY(targetList);
        QVERIFY(pathEdit);
        QVERIFY(table);
        if (selectLocalTargetRowForDrive(targetList, pathEdit, dir.path().left(2).toUpper()) < 0) {
            QSKIP("No mounted local target for the temp drive on this test host.");
        }
        QVERIFY(navigateAndFindRow(pathEdit, table, dir.path(), QStringLiteral("bin_me")) >= 0);
        QVERIFY(waitForListingQuiescence(table));
        panel.activateWindow();
        table->setFocus();
        QApplication::processEvents();

        // Files DeleteItemAction: plain Delete on a local volume recycles after
        // the always-on confirmation. The dialog is auto-accepted by a timer so
        // the run stays non-interactive.
        QString question_text;
        QVERIFY(selectRowStable(table, QStringLiteral("bin_me")));
        armAutoAcceptQuestion(&question_text);
        QTest::keyClick(table, Qt::Key_Delete);
        QVERIFY2(QTest::qWaitFor(
                     [&root]() {
                         return !QFile::exists(root.filePath(QStringLiteral("bin_me.txt")));
                     },
                     5000),
                 "delete did not recycle the file");
        // The count and the path list -- what tells the user what is about to be destroyed --
        // are what the fragment never looked at.
        verifyDeleteConfirmation(question_text,
                                 QStringLiteral("Move 1 item(s) to the Recycle Bin?<br/><br/>"),
                                 QStringLiteral("bin_me.txt"));

        // Shift+Delete is the permanent path (DeleteItemPermanentlyAction).
        panel.activateWindow();
        table->setFocus();
        QApplication::processEvents();
        QVERIFY(selectRowStable(table, QStringLiteral("perma")));
        question_text.clear();
        armAutoAcceptQuestion(&question_text);
        QTest::keyClick(table, Qt::Key_Delete, Qt::ShiftModifier);
        QVERIFY2(QTest::qWaitFor(
                     [&root]() {
                         return !QFile::exists(root.filePath(QStringLiteral("perma.txt")));
                     },
                     5000),
                 "shift-delete did not remove the file");
        verifyPermanentDeleteConfirmation(question_text, QStringLiteral("perma.txt"));
    }

    void activationMatrixAndMouseNavigation() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QVERIFY(QDir(dir.path()).mkdir(QStringLiteral("subdir")));

        sak::FileManagementExplorerPanel panel;
        panel.resize(1100, 700);
        panel.show();
        QVERIFY(QTest::qWaitForWindowExposed(&panel));
        auto* targetList = child<QListWidget>(&panel, "fileExplorerTargetList");
        auto* pathEdit = child<QLineEdit>(&panel, "fileExplorerPathEdit");
        auto* table = child<QTableView>(&panel, "fileExplorerTable");
        auto* tabBar = child<QTabBar>(&panel, "fileExplorerTabBar");
        QVERIFY(targetList);
        QVERIFY(pathEdit);
        QVERIFY(table);
        QVERIFY(tabBar);
        if (selectLocalTargetRowForDrive(targetList, pathEdit, dir.path().left(2).toUpper()) < 0) {
            QSKIP("No mounted local target for the temp drive on this test host.");
        }
        QVERIFY(navigateAndFindRow(pathEdit, table, dir.path(), QStringLiteral("subdir")) >= 0);

        // Ctrl+Enter opens the selected folder in a new tab (Files
        // FileList_PreviewKeyDown) and focuses it.
        const int tabs_before = tabBar->count();
        QVERIFY(selectRowStable(table, QStringLiteral("subdir")));
        QTest::keyClick(table, Qt::Key_Return, Qt::ControlModifier);
        QTRY_COMPARE(tabBar->count(), tabs_before + 1);
        QTRY_VERIFY(pathEdit->text().contains(QStringLiteral("subdir")));

        // Plain Enter opens the selected folder in place (OpenItemAction).
        QTest::keyClick(&panel, Qt::Key_1, Qt::ControlModifier);
        QTRY_VERIFY(!pathEdit->text().contains(QStringLiteral("subdir")));
        QVERIFY(selectRowStable(table, QStringLiteral("subdir")));
        QTest::keyClick(table, Qt::Key_Return);
        QTRY_VERIFY(pathEdit->text().contains(QStringLiteral("subdir")));

        verifyMouseNavigationAndWheel(panel, pathEdit, table);
    }

    void verifyMouseNavigationAndWheel(sak::FileManagementExplorerPanel& panel,
                                       QLineEdit* pathEdit,
                                       QTableView* table) {
        // Mouse button 4 navigates back (NavigateBackAction ThirdHotKey).
        {
            const QPoint pos = table->viewport()->rect().center();
            QMouseEvent back_press(QEvent::MouseButtonPress,
                                   pos,
                                   table->viewport()->mapToGlobal(pos),
                                   Qt::BackButton,
                                   Qt::BackButton,
                                   Qt::NoModifier);
            QVERIFY(QApplication::sendEvent(table->viewport(), &back_press));
        }
        QTRY_VERIFY(!pathEdit->text().contains(QStringLiteral("subdir")));

        // Double-click on empty space navigates up (DoubleClickToGoUp,
        // Files default true).
        const QString before_up = pathEdit->text();
        QTest::mouseDClick(table->viewport(),
                           Qt::LeftButton,
                           {},
                           table->viewport()->rect().bottomRight() - QPoint(5, 5));
        QTRY_VERIFY(pathEdit->text() != before_up);

        // Ctrl+mouse-wheel zooms the ACTIVE layout only (BaseLayoutViewModel
        // routes to LayoutIncreaseSize): the Details size kind steps while
        // the other layouts' kinds stay untouched.
        auto* pane = panel.findChild<sak::FileExplorerPane*>();
        QVERIFY(pane);
        QCOMPARE(pane->viewMode(), sak::FileExplorerViewMode::Details);
        const int details_before = pane->layoutSizes().details;
        const int grid_before = pane->layoutSizes().grid;
        const QPoint wheel_pos = table->viewport()->rect().center();
        QWheelEvent wheel(wheel_pos,
                          table->viewport()->mapToGlobal(wheel_pos),
                          QPoint(),
                          QPoint(0, 120),
                          Qt::NoButton,
                          Qt::ControlModifier,
                          Qt::NoScrollPhase,
                          false);
        QVERIFY(QApplication::sendEvent(table->viewport(), &wheel));
        QTRY_VERIFY(pane->layoutSizes().details != details_before);
        QCOMPARE(pane->layoutSizes().grid, grid_before);
    }

    void headerMenuOffersSizeAllColumnsToFit() {
        sak::FileManagementExplorerPanel panel;
        panel.resize(1100, 700);
        panel.show();
        QVERIFY(QTest::qWaitForWindowExposed(&panel));
        auto* table = child<QTableView>(&panel, "fileExplorerTable");
        QVERIFY(table);

        // Files DetailsLayoutPage header menu ends with the auto-fit entry.
        // (Context-menu events on a scroll area land on its viewport.)
        const QStringList headerActions =
            collectContextMenuTexts(table->horizontalHeader()->viewport());
        // One checkable entry per column except Name, which showColumnMenu deliberately skips
        // so it can never be hidden -- a functional contract the membership probe cannot see,
        // along with a lost, renamed or reordered column.
        QCOMPARE(headerActions,
                 (QStringList{QStringLiteral("Type"),
                              QStringLiteral("Size"),
                              QStringLiteral("Modified"),
                              QStringLiteral("Created"),
                              QStringLiteral("ID"),
                              QStringLiteral("Attributes"),
                              QStringLiteral("Tags"),
                              QStringLiteral("Path"),
                              QStringLiteral("Size all columns to fit")}));

        auto* details = qobject_cast<sak::FileExplorerDetailsView*>(table);
        QVERIFY(details);
        table->setColumnWidth(sak::FileExplorerItemModel::NameColumn, 555);
        details->autoFitAllColumns();
        QVERIFY(table->columnWidth(sak::FileExplorerItemModel::NameColumn) != 555);
    }

    void filterHeaderTogglesAndFiltersListing() {
        sak::FileManagementExplorerPanel panel;
        panel.resize(1100, 700);
        panel.show();
        QVERIFY(QTest::qWaitForWindowExposed(&panel));

        auto* pane = panel.findChild<sak::FileExplorerPane*>();
        QVERIFY(pane);
        QVERIFY(pane->sortFilterModel());

        // Files ToggleFilterHeaderAction: hidden by default, Ctrl+Shift+F
        // shows the "Filtering for" row and focuses the Filename box.
        auto* header = child<QWidget>(&panel, "fileExplorerFilterHeader");
        auto* filterBox = child<QLineEdit>(&panel, "fileExplorerFilterBox");
        QVERIFY(header);
        QVERIFY(filterBox);
        QVERIFY(!header->isVisible());
        panel.setFocus();
        QTest::keyClick(&panel, Qt::Key_F, Qt::ControlModifier | Qt::ShiftModifier);
        QTRY_VERIFY(header->isVisible());

        // Typing filters the loaded listing after the 250 ms debounce.
        filterBox->setText(QStringLiteral("codex-filter-no-match"));
        QTRY_COMPARE_WITH_TIMEOUT(pane->sortFilterModel()->nameFilter(),
                                  QStringLiteral("codex-filter-no-match"),
                                  2000);

        // Esc returns focus to the file list (FilterTextBox_PreviewKeyDown).
        QTest::keyClick(filterBox, Qt::Key_Escape);
        QApplication::processEvents();
        QVERIFY(!filterBox->hasFocus());

        // Toggling again hides the row; the persisted setting flips too.
        QTest::keyClick(&panel, Qt::Key_F, Qt::ControlModifier | Qt::ShiftModifier);
        QTRY_VERIFY(!header->isVisible());
        QSettings settings;
        settings.beginGroup(QStringLiteral("FileManagementExplorer"));
        QVERIFY(!settings.value(QStringLiteral("ShowFilterHeader"), false).toBool());
        settings.endGroup();
    }

    void commandPaletteShortcutEntersInlineOmnibarMode() {
        sak::FileManagementExplorerPanel panel;
        panel.resize(1100, 700);
        panel.show();
        QVERIFY(QTest::qWaitForWindowExposed(&panel));

        // Ctrl+Shift+P switches the omnibar into the inline palette mode
        // (Files Omnibar): palette placeholder, suggestion popup visible with
        // executable commands, Esc backs out to the breadcrumb.
        auto* pathEdit = child<QLineEdit>(&panel, "fileExplorerPathEdit");
        QVERIFY(pathEdit);
        panel.setFocus();
        QTest::keyClick(&panel, Qt::Key_P, Qt::ControlModifier | Qt::ShiftModifier);
        QApplication::processEvents();
        QCOMPARE(pathEdit->placeholderText(), QStringLiteral("Find features and commands..."));
        auto* suggestions =
            panel.findChild<QListWidget*>(QStringLiteral("fileExplorerOmnibarSuggestions"));
        QVERIFY(suggestions);
        QTRY_VERIFY(suggestions->isVisible());
        QVERIFY(suggestions->count() > 0);
        // Rows are executable-only and carry the command id role (UserRole+3).
        for (int i = 0; i < suggestions->count(); ++i) {
            QVERIFY(suggestions->item(i)->data(Qt::UserRole + 4).toBool());
        }

        // Two-stage Escape: first closes the popup, second reverts to Path
        // mode (breadcrumb visible again).
        QTest::keyClick(pathEdit, Qt::Key_Escape);
        QTRY_VERIFY(!suggestions->isVisible());
        QTest::keyClick(pathEdit, Qt::Key_Escape);
        QTRY_VERIFY(!panel.findChild<sak::FileExplorerOmnibar*>()->addressEditMode());
    }

    void editPathShortcutsEnterAddressEditMode() {
        sak::FileManagementExplorerPanel panel;
        panel.resize(1100, 700);
        panel.show();
        QVERIFY(QTest::qWaitForWindowExposed(&panel));

        // Files EditPathAction: Ctrl+L and Alt+D swap the breadcrumb for the
        // editable path field. MainWindow must not shadow Ctrl+L with an
        // application-wide shortcut (the log toggle lives on Ctrl+Shift+L);
        // an ambiguous binding leaves Qt firing neither and the keystrokes
        // leak into the item view's type-ahead search.
        auto* omnibar = panel.findChild<sak::FileExplorerOmnibar*>();
        QVERIFY(omnibar);
        QVERIFY(!omnibar->addressEditMode());
        panel.setFocus();
        QTest::keyClick(&panel, Qt::Key_L, Qt::ControlModifier);
        QTRY_VERIFY(omnibar->addressEditMode());

        auto* pathEdit = child<QLineEdit>(&panel, "fileExplorerPathEdit");
        QVERIFY(pathEdit);
        QTest::keyClick(pathEdit, Qt::Key_Escape);
        QTRY_VERIFY(!omnibar->addressEditMode());

        QTest::keyClick(&panel, Qt::Key_D, Qt::AltModifier);
        QTRY_VERIFY(omnibar->addressEditMode());
        // Files EditPath commit: Enter navigates and restores the breadcrumb.
        QTest::keyClick(pathEdit, Qt::Key_Return);
        QTRY_VERIFY(!omnibar->addressEditMode());
    }

    void explorerTabCloseRemovesTabKeepingLast() {
        sak::FileManagementExplorerPanel panel;
        panel.resize(1100, 700);
        panel.show();
        QVERIFY(QTest::qWaitForWindowExposed(&panel));

        auto* tabs = child<QTabBar>(&panel, "fileExplorerTabBar");
        auto* newTab = child<QPushButton>(&panel, "fileExplorerNewTabButton");
        QVERIFY(tabs);
        QVERIFY(newTab);

        newTab->click();
        QApplication::processEvents();
        QCOMPARE(tabs->count(), 2);

        // Close the second tab through its auto-generated close button.
        QWidget* closeButton = tabs->tabButton(1, QTabBar::RightSide);
        if (!closeButton) {
            closeButton = tabs->tabButton(1, QTabBar::LeftSide);
        }
        QVERIFY(closeButton);
        QTest::mouseClick(closeButton, Qt::LeftButton);
        QApplication::processEvents();
        QCOMPARE(tabs->count(), 1);

        // Closing the final tab is refused so the explorer always has one tab.
        QWidget* lastClose = tabs->tabButton(0, QTabBar::RightSide);
        if (!lastClose) {
            lastClose = tabs->tabButton(0, QTabBar::LeftSide);
        }
        if (lastClose) {
            QTest::mouseClick(lastClose, Qt::LeftButton);
            QApplication::processEvents();
        }
        QCOMPARE(tabs->count(), 1);
    }

    void dualPaneToggleAddsSecondPane() {
        sak::FileManagementExplorerPanel panel;
        panel.resize(1200, 760);
        panel.show();
        QVERIFY(QTest::qWaitForWindowExposed(&panel));

        auto* view = child<QToolButton>(&panel, "fileExplorerViewButton");
        QVERIFY(view);
        QVERIFY(view->menu());

        QAction* dualPane = actionStartingWith(view->menu(), QStringLiteral("Dual Pane"));
        QVERIFY(dualPane);
        QVERIFY(dualPane->isEnabled());

        QCOMPARE(panel.findChildren<sak::FileExplorerPane*>().size(), 1);
        dualPane->trigger();
        QApplication::processEvents();
        QCOMPARE(panel.findChildren<sak::FileExplorerPane*>().size(), 2);
    }

    // Shared M8 dual-pane fixture: a parent folder holding "sub/inner.txt" and
    // "top.txt", navigated to in pane A with "sub" selected. Returns false (after
    // QSKIP/QVERIFY inside) when this host has no local target for the temp drive.
    bool openDualPaneFixture(sak::FileManagementExplorerPanel& panel,
                             const QString& parent_path,
                             sak::FileExplorerPane** pane_a) {
        auto* targetList = child<QListWidget>(&panel, "fileExplorerTargetList");
        auto* pathEdit = child<QLineEdit>(&panel, "fileExplorerPathEdit");
        auto* table = child<QTableView>(&panel, "fileExplorerTable");
        if (targetList == nullptr || pathEdit == nullptr || table == nullptr) {
            return false;
        }
        if (selectLocalTargetRowForDrive(targetList, pathEdit, parent_path.left(2).toUpper()) < 0) {
            return false;
        }
        if (navigateAndFindRow(pathEdit, table, parent_path, QStringLiteral("sub")) < 0) {
            return false;
        }
        if (!waitForListingQuiescence(table) || !selectRowStable(table, QStringLiteral("sub"))) {
            return false;
        }
        const auto panes = panel.findChildren<sak::FileExplorerPane*>();
        if (panes.size() != 1) {
            return false;
        }
        *pane_a = panes.first();
        // Files FileList_PreviewKeyDown: Ctrl+Shift+Enter opens the selection in
        // the other pane (OpenInSecondPane), creating that pane if needed.
        panel.activateWindow();
        table->setFocus();
        QApplication::processEvents();
        QTest::keyClick(table, Qt::Key_Return, Qt::ControlModifier | Qt::ShiftModifier);
        return true;
    }

    // The pane that is not @p pane_a, once the split exists.
    static sak::FileExplorerPane* otherPane(sak::FileManagementExplorerPanel& panel,
                                            sak::FileExplorerPane* pane_a) {
        for (auto* pane : panel.findChildren<sak::FileExplorerPane*>()) {
            if (pane != pane_a) {
                return pane;
            }
        }
        return nullptr;
    }

    // Names listed in @p pane's own item model (the unfiltered source rows).
    static QStringList paneEntryNames(const sak::FileExplorerPane* pane) {
        QStringList names;
        const auto* model = pane->itemModel();
        for (int row = 0; row < model->rowCount(); ++row) {
            names.append(model->index(row, 0).data().toString());
        }
        return names;
    }

    void openFolderInSecondPaneListsThatFolderInPaneB() {
        // M8 lane: Open In Second Pane must land the SELECTED FOLDER's contents in
        // pane B and leave pane A where it was. dualPaneToggleAddsSecondPane only
        // counts panes, so a second pane that opened the wrong path -- or mirrored
        // pane A -- satisfied it.
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QDir root(dir.path());
        QVERIFY(root.mkpath(QStringLiteral("sub")));
        const auto write = [&root](const QString& relative) {
            QFile file(root.filePath(relative));
            QVERIFY(file.open(QIODevice::WriteOnly));
            QVERIFY(file.write("dual-pane fixture") > 0);
        };
        write(QStringLiteral("top.txt"));
        write(QStringLiteral("sub/inner.txt"));

        sak::FileManagementExplorerPanel panel;
        panel.resize(1280, 800);
        panel.show();
        QVERIFY(QTest::qWaitForWindowExposed(&panel));
        sak::FileExplorerPane* pane_a = nullptr;
        if (!openDualPaneFixture(panel, dir.path(), &pane_a)) {
            QSKIP("No mounted local target for the temp drive on this test host.");
        }

        QTRY_COMPARE(panel.findChildren<sak::FileExplorerPane*>().size(), 2);
        auto* pane_b = otherPane(panel, pane_a);
        QVERIFY(pane_b);
        // Pane B shows sub/, pane A still shows the parent. Both halves matter: a
        // second pane that simply cloned pane A would show "sub" and "top.txt" too.
        QTRY_VERIFY2(paneEntryNames(pane_b).contains(QStringLiteral("inner.txt")),
                     qPrintable(paneEntryNames(pane_b).join(QLatin1Char(','))));
        QVERIFY2(!paneEntryNames(pane_b).contains(QStringLiteral("top.txt")),
                 qPrintable(paneEntryNames(pane_b).join(QLatin1Char(','))));
        const QStringList left = paneEntryNames(pane_a);
        QVERIFY2(left.contains(QStringLiteral("top.txt")), qPrintable(left.join(QLatin1Char(','))));
        QVERIFY2(left.contains(QStringLiteral("sub")), qPrintable(left.join(QLatin1Char(','))));
        captureBaseline(&panel, QStringLiteral("dual-pane"));
    }

    void commandsActOnTheActivePaneOnly() {
        // M8 lane: a command must act on the ACTIVE pane's folder, and clicking into
        // the other pane must make that pane active first (Files ShellPanesPage
        // Pane_PointerPressed -> Pane_GotFocus). Before that wiring existed, only
        // Enter / mouse4-5 / middle-click / double-click-empty switched panes, so a
        // left-click in the inactive pane followed by New Folder or Delete acted on
        // the pane the user had visibly left.
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QDir root(dir.path());
        QVERIFY(root.mkpath(QStringLiteral("sub")));
        {
            QFile file(root.filePath(QStringLiteral("sub/inner.txt")));
            QVERIFY(file.open(QIODevice::WriteOnly));
            QVERIFY(file.write("dual-pane fixture") > 0);
        }
        {
            QFile file(root.filePath(QStringLiteral("top.txt")));
            QVERIFY(file.open(QIODevice::WriteOnly));
            QVERIFY(file.write("dual-pane fixture") > 0);
        }

        sak::FileManagementExplorerPanel panel;
        panel.resize(1280, 800);
        panel.show();
        QVERIFY(QTest::qWaitForWindowExposed(&panel));
        sak::FileExplorerPane* pane_a = nullptr;
        if (!openDualPaneFixture(panel, dir.path(), &pane_a)) {
            QSKIP("No mounted local target for the temp drive on this test host.");
        }
        QTRY_COMPARE(panel.findChildren<sak::FileExplorerPane*>().size(), 2);
        auto* pane_b = otherPane(panel, pane_a);
        QVERIFY(pane_b);
        QTRY_VERIFY(paneEntryNames(pane_b).contains(QStringLiteral("inner.txt")));
        // activatePane() re-points the omnibar at the newly active pane, so the
        // address field is the direct read-out of WHICH pane is active -- pin it
        // rather than inferring activation from where a command happened to land.
        auto* pathEdit = child<QLineEdit>(&panel, "fileExplorerPathEdit");
        QVERIFY(pathEdit);
        const QString sub_path = root.filePath(QStringLiteral("sub"));
        QTRY_COMPARE(pathEdit->text(), sub_path);

        verifyNewFolderFollowsTheActivePane(panel, pane_a, pane_b, pathEdit, root);
        verifyEmptySpacePressActivatesPane(pane_b, pathEdit, sub_path);
    }

    // New Folder must land under whichever pane is active, and clicking into the
    // other pane must move activation there and drop the selection left behind.
    void verifyNewFolderFollowsTheActivePane(sak::FileManagementExplorerPanel& panel,
                                             sak::FileExplorerPane* pane_a,
                                             sak::FileExplorerPane* pane_b,
                                             QLineEdit* pathEdit,
                                             const QDir& root) {
        const QString in_sub = root.filePath(QStringLiteral("sub/New Folder"));
        const QString in_parent = root.filePath(QStringLiteral("New Folder"));
        QString label;
        armAutoAcceptInputDialog(&label);
        panel.activateWindow();
        pane_b->tableView()->setFocus();
        QApplication::processEvents();
        QTest::keyClick(pane_b->tableView(), Qt::Key_N, Qt::ControlModifier | Qt::ShiftModifier);
        QTRY_VERIFY2(QFileInfo(in_sub).isDir(), qPrintable(in_sub));
        QVERIFY2(!QFileInfo::exists(in_parent), "New Folder landed in the INACTIVE pane's folder");

        // Selecting a row in pane B and then clicking into pane A must transfer
        // activation AND clear pane B's selection, so the next command can neither
        // read pane B's path nor its rows.
        QVERIFY(selectRowStable(pane_b->tableView(), QStringLiteral("inner.txt")));
        auto* view_a = pane_a->tableView();
        QVERIFY(view_a->model()->rowCount() > 0);
        const QRect row_rect = view_a->visualRect(view_a->model()->index(0, 0));
        QVERIFY2(row_rect.isValid() && !row_rect.isEmpty(), "pane A has no laid-out row to click");
        QTest::mouseClick(view_a->viewport(), Qt::LeftButton, Qt::NoModifier, row_rect.center());
        QApplication::processEvents();
        QTRY_COMPARE(pathEdit->text(), root.path());
        QTRY_COMPARE(pane_b->sharedSelectionModel()->selectedRows().size(), 0);

        // Pane A is now the active pane: the same command lands under the parent.
        QString second_label;
        armAutoAcceptInputDialog(&second_label);
        panel.activateWindow();
        view_a->setFocus();
        QApplication::processEvents();
        QTest::keyClick(view_a, Qt::Key_N, Qt::ControlModifier | Qt::ShiftModifier);
        QTRY_VERIFY2(QFileInfo(in_parent).isDir(), qPrintable(in_parent));
        // sub/ still holds exactly the one folder made while pane B was active.
        QCOMPARE(QDir(root.filePath(QStringLiteral("sub")))
                     .entryList(QDir::Dirs | QDir::NoDotAndDotDot)
                     .size(),
                 1);
    }

    // Files Pane_PointerPressed: a press on EMPTY space in the inactive pane
    // activates it too. This is the case selection cannot cover -- clicking a row
    // promotes a pane through connectPaneSignals' selectionChanged handler, but a
    // press below the last row changes no selection, so without the press-time
    // activation the pane the user just clicked into stays inactive and the next
    // command silently runs against the other pane's folder.
    void verifyEmptySpacePressActivatesPane(sak::FileExplorerPane* pane_b,
                                            QLineEdit* pathEdit,
                                            const QString& sub_path) {
        auto* view_b = pane_b->tableView();
        const int last_row = view_b->model()->rowCount() - 1;
        QVERIFY(last_row >= 0);
        const QRect last_rect = view_b->visualRect(view_b->model()->index(last_row, 0));
        const QPoint empty_space(view_b->viewport()->rect().center().x(),
                                 view_b->viewport()->rect().bottom() - 4);
        QVERIFY2(empty_space.y() > last_rect.bottom(), "pane B has no empty space to click");
        QVERIFY(!view_b->indexAt(empty_space).isValid());
        QTest::mouseClick(view_b->viewport(), Qt::LeftButton, Qt::NoModifier, empty_space);
        QApplication::processEvents();
        QTRY_COMPARE(pathEdit->text(), sub_path);
        QCOMPARE(pane_b->sharedSelectionModel()->selectedRows().size(), 0);
    }

    void staleFavoriteRendersOfflineSidebarRow() {
        // Seed a favorite id that cannot resolve to any connected target, then verify
        // the sidebar keeps it as a disabled, warning-marked "offline" row instead of
        // dropping it silently.
        QSettings settings;
        settings.beginGroup(QStringLiteral("FileManagementExplorer"));
        const QStringList previous =
            settings.value(QStringLiteral("FavoriteTargetIds")).toStringList();
        settings.setValue(QStringLiteral("FavoriteTargetIds"),
                          QStringList{QStringLiteral("disk:99:partition:9")});
        settings.endGroup();
        settings.sync();

        bool foundOffline = false;
        {
            sak::FileManagementExplorerPanel panel;
            panel.resize(1100, 700);
            panel.show();
            QVERIFY(QTest::qWaitForWindowExposed(&panel));
            auto* list = child<QListWidget>(&panel, "fileExplorerTargetList");
            QVERIFY(list);
            for (int i = 0; i < list->count(); ++i) {
                const QListWidgetItem* item = list->item(i);
                if (item->text().contains(QStringLiteral("[offline]")) &&
                    item->flags() == Qt::NoItemFlags) {
                    foundOffline = true;
                    break;
                }
            }
        }

        settings.beginGroup(QStringLiteral("FileManagementExplorer"));
        settings.setValue(QStringLiteral("FavoriteTargetIds"), previous);
        settings.endGroup();
        settings.sync();

        QVERIFY(foundOffline);
    }

    void staleFavoriteContextMenuRemovesThePin() {
        // An offline favorite has no connected target, but the pin itself must stay
        // removable through its own context menu (the only defect-free removal path).
        QSettings settings;
        settings.beginGroup(QStringLiteral("FileManagementExplorer"));
        const QStringList previous =
            settings.value(QStringLiteral("FavoriteTargetIds")).toStringList();
        settings.setValue(QStringLiteral("FavoriteTargetIds"),
                          QStringList{QStringLiteral("disk:99:partition:9")});
        settings.endGroup();
        settings.sync();

        {
            sak::FileManagementExplorerPanel panel;
            panel.resize(1100, 700);
            panel.show();
            QVERIFY(QTest::qWaitForWindowExposed(&panel));
            auto* list = child<QListWidget>(&panel, "fileExplorerTargetList");
            QVERIFY(list);
            int staleRow = -1;
            for (int i = 0; i < list->count(); ++i) {
                if (list->item(i)->text().contains(QStringLiteral("[offline]"))) {
                    staleRow = i;
                    break;
                }
            }
            QVERIFY2(staleRow >= 0, "offline favorite row not rendered");

            QTimer::singleShot(0, [&panel]() {
                auto* menu = qobject_cast<QMenu*>(QApplication::activePopupWidget());
                if (!menu) {
                    return;
                }
                for (auto* action : menu->actions()) {
                    if (action->objectName() == QStringLiteral("fileExplorerRemoveStaleFavorite")) {
                        action->trigger();
                        break;
                    }
                }
                menu->close();
            });
            const QRect rect = list->visualItemRect(list->item(staleRow));
            QContextMenuEvent event(QContextMenuEvent::Mouse,
                                    rect.center(),
                                    list->viewport()->mapToGlobal(rect.center()));
            QApplication::sendEvent(list->viewport(), &event);
            QApplication::processEvents();
            QApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        }

        settings.beginGroup(QStringLiteral("FileManagementExplorer"));
        const QStringList after =
            settings.value(QStringLiteral("FavoriteTargetIds")).toStringList();
        settings.setValue(QStringLiteral("FavoriteTargetIds"), previous);
        settings.endGroup();
        settings.sync();

        QVERIFY2(!after.contains(QStringLiteral("disk:99:partition:9")),
                 "stale favorite id still pinned after Remove from Favorites");
    }

    void sidebarRebuildKeepsCurrentFolder() {
        // Sidebar rebuilds (tag edits, favorite reorder, clear-recent) re-select the
        // active target; that reselect must not reset navigation back to the root.
        QSettings settings;
        settings.beginGroup(QStringLiteral("FileManagementExplorer"));
        const QStringList previous_favorites =
            settings.value(QStringLiteral("FavoriteTargetIds")).toStringList();
        settings.endGroup();
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QVERIFY(QDir(dir.path()).mkdir(QStringLiteral("inner")));

        sak::FileManagementExplorerPanel panel;
        panel.resize(1100, 700);
        panel.show();
        QVERIFY(QTest::qWaitForWindowExposed(&panel));

        auto* targetList = child<QListWidget>(&panel, "fileExplorerTargetList");
        auto* pathEdit = child<QLineEdit>(&panel, "fileExplorerPathEdit");
        QVERIFY(targetList);
        QVERIFY(pathEdit);
        if (selectLocalTargetRowForDrive(targetList, pathEdit, dir.path().left(2).toUpper()) < 0) {
            QSKIP("No mounted local target for the temp drive on this test host.");
        }
        const QString inner = QDir(dir.path()).filePath(QStringLiteral("inner"));
        pathEdit->setText(inner);
        QTest::keyClick(pathEdit, Qt::Key_Return);
        QVERIFY(QTest::qWaitFor(
            [pathEdit]() {
                return QDir::fromNativeSeparators(pathEdit->text())
                    .contains(QStringLiteral("inner"));
            },
            5000));

        // Pin the current target as a favorite: this saves state and rebuilds the sidebar.
        QTimer::singleShot(0, []() {
            auto* menu = qobject_cast<QMenu*>(QApplication::activePopupWidget());
            if (!menu) {
                return;
            }
            for (auto* action : menu->actions()) {
                if (action->text().startsWith(QStringLiteral("Pin Favorite")) ||
                    action->text().startsWith(QStringLiteral("Unpin Favorite"))) {
                    action->trigger();
                    break;
                }
            }
            menu->close();
        });
        // Context-click the CURRENT target row (not blind viewport center:
        // shell geometry changes must not silently retarget the test).
        QVERIFY(targetList->currentItem());
        const QPoint center = targetList->visualItemRect(targetList->currentItem()).center();
        QContextMenuEvent event(QContextMenuEvent::Mouse,
                                center,
                                targetList->viewport()->mapToGlobal(center));
        QApplication::sendEvent(targetList->viewport(), &event);
        QApplication::processEvents();
        QApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);

        // Navigation must be exactly where it was.
        const QString path_after_rebuild = pathEdit->text();
        settings.beginGroup(QStringLiteral("FileManagementExplorer"));
        settings.setValue(QStringLiteral("FavoriteTargetIds"), previous_favorites);
        settings.endGroup();
        settings.sync();
        QVERIFY2(QDir::fromNativeSeparators(path_after_rebuild).contains(QStringLiteral("inner")),
                 qPrintable(QStringLiteral("pane jumped to: ") + path_after_rebuild));
    }

    void dualPaneSurvivesTabSessionRoundTrip() {
        // Dual-pane arrangement is part of the tab session: enabling the split, saving
        // the session (panel destruction), and restoring must bring the split back.
        QSettings settings;
        settings.beginGroup(QStringLiteral("FileManagementExplorer/TabSession"));
        settings.remove(QString());
        settings.endGroup();
        settings.sync();

        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        {
            sak::FileManagementExplorerPanel panel;
            panel.enableTabSessionPersistence();
            panel.resize(1200, 760);
            panel.show();
            QVERIFY(QTest::qWaitForWindowExposed(&panel));
            auto* targetList = child<QListWidget>(&panel, "fileExplorerTargetList");
            auto* pathEdit = child<QLineEdit>(&panel, "fileExplorerPathEdit");
            QVERIFY(targetList);
            QVERIFY(pathEdit);
            if (selectLocalTargetRowForDrive(targetList, pathEdit, dir.path().left(2).toUpper()) <
                0) {
                QSKIP("No mounted local target for the temp drive on this test host.");
            }
            auto* view = child<QToolButton>(&panel, "fileExplorerViewButton");
            QVERIFY(view && view->menu());
            actionStartingWith(view->menu(), QStringLiteral("Dual Pane"))->trigger();
            QApplication::processEvents();
        }

        {
            sak::FileManagementExplorerPanel panel;
            panel.enableTabSessionPersistence();
            panel.resize(1200, 760);
            panel.show();
            QVERIFY(QTest::qWaitForWindowExposed(&panel));
            QApplication::processEvents();
            auto* splitter = child<QSplitter>(&panel, "fileExplorerPaneSplitter");
            QVERIFY(splitter);
            int visible_panes = 0;
            for (int i = 0; i < splitter->count(); ++i) {
                if (splitter->widget(i)->isVisible()) {
                    ++visible_panes;
                }
            }
            QCOMPARE(visible_panes, 2);
        }

        settings.beginGroup(QStringLiteral("FileManagementExplorer/TabSession"));
        settings.remove(QString());
        settings.endGroup();
        settings.sync();
    }

    void favoritesAndRecentPersistAcrossConstruction() {
        // Favorites and recent target ids saved to QSettings are re-read on the next
        // panel construction and rendered as sidebar rows (persistence round trip).
        QSettings settings;
        settings.beginGroup(QStringLiteral("FileManagementExplorer"));
        const QStringList prevFav =
            settings.value(QStringLiteral("FavoriteTargetIds")).toStringList();
        const QStringList prevRecent =
            settings.value(QStringLiteral("RecentTargetIds")).toStringList();
        settings.setValue(QStringLiteral("FavoriteTargetIds"),
                          QStringList{QStringLiteral("disk:77:partition:3")});
        settings.setValue(QStringLiteral("RecentTargetIds"),
                          QStringList{QStringLiteral("disk:88:partition:4")});
        settings.endGroup();
        settings.sync();

        bool foundFavorite = false;
        bool foundRecent = false;
        {
            sak::FileManagementExplorerPanel panel;
            panel.resize(1100, 700);
            panel.show();
            QVERIFY(QTest::qWaitForWindowExposed(&panel));
            auto* list = child<QListWidget>(&panel, "fileExplorerTargetList");
            QVERIFY(list);
            for (int i = 0; i < list->count(); ++i) {
                const QString text = list->item(i)->text();
                foundFavorite = foundFavorite ||
                                text.contains(QStringLiteral("disk:77:partition:3"));
                foundRecent = foundRecent || text.contains(QStringLiteral("disk:88:partition:4"));
            }
        }

        settings.beginGroup(QStringLiteral("FileManagementExplorer"));
        settings.setValue(QStringLiteral("FavoriteTargetIds"), prevFav);
        settings.setValue(QStringLiteral("RecentTargetIds"), prevRecent);
        settings.endGroup();
        settings.sync();

        // The offline favorite renders as its stale row; the recent id, absent from the
        // connected targets, is dropped silently (recents do not warn). Assert the
        // favorite persisted and rendered.
        QVERIFY2(foundFavorite, "persisted favorite id not rendered after reconstruction");
        // foundRecent was computed above and never asserted, so the other half of the contract
        // -- only Favorites passes warn_when_missing, so a disconnected RECENT id must render
        // nothing at all -- went unchecked. A connected target renders its label, never the
        // raw id, so this stays host-independent.
        QVERIFY2(!foundRecent, "an unconnected recent id must not render a sidebar row");
    }

    void tagColumnShowsProviderTagsInDetailsView() {
        // The details view exposes a Tags column backed by the injected provider, and it
        // repaints after a tag edit (refreshTags).
        sak::FileExplorerItemModel model;
        sak::FileManagementEntry entry;
        entry.name = QStringLiteral("tagged.txt");
        entry.path = QStringLiteral("/tagged.txt");
        entry.regular_file = true;
        model.setEntries({entry});
        QStringList provided;
        model.setTagProvider([&provided](const QString&) { return provided; });
        QCOMPARE(model.index(0, sak::FileExplorerItemModel::TagsColumn).data().toString(),
                 QString());
        provided = {QStringLiteral("project-x")};
        model.refreshTags();
        QCOMPARE(model.index(0, sak::FileExplorerItemModel::TagsColumn).data().toString(),
                 QStringLiteral("project-x"));
    }

    void dualPaneStackActionFlipsSplitterOrientation() {
        sak::FileManagementExplorerPanel panel;
        panel.resize(1200, 760);
        panel.show();
        QVERIFY(QTest::qWaitForWindowExposed(&panel));

        auto* view = child<QToolButton>(&panel, "fileExplorerViewButton");
        QVERIFY(view);
        QVERIFY(view->menu());
        auto* splitter = child<QSplitter>(&panel, "fileExplorerPaneSplitter");
        QVERIFY(splitter);
        QCOMPARE(splitter->orientation(), Qt::Horizontal);

        // Stacking is inert until dual pane is active.
        QAction* stack = actionStartingWith(view->menu(), QStringLiteral("Stack Panes Vertically"));
        QVERIFY(stack);
        QVERIFY(!stack->isEnabled());

        actionStartingWith(view->menu(), QStringLiteral("Dual Pane"))->trigger();
        QApplication::processEvents();

        stack = actionStartingWith(view->menu(), QStringLiteral("Stack Panes Vertically"));
        QVERIFY(stack->isEnabled());
        stack->trigger();
        QApplication::processEvents();
        QCOMPARE(splitter->orientation(), Qt::Vertical);

        // Menu rebuild reflects the current orientation as checked.
        stack = actionStartingWith(view->menu(), QStringLiteral("Stack Panes Vertically"));
        QVERIFY(stack->isChecked());
        stack->trigger();
        QApplication::processEvents();
        QCOMPARE(splitter->orientation(), Qt::Horizontal);
    }

    void copyPasteRoundTripsLocalFileThroughClipboard() {
        // End-to-end M9 local paste: copy a file from one local folder through the explorer
        // Copy command, navigate to another local folder, Paste, and verify a byte-exact copy.
        QTemporaryDir source_dir;
        QTemporaryDir destination_dir;
        QVERIFY(source_dir.isValid());
        QVERIFY(destination_dir.isValid());
        const QByteArray payload = QByteArrayLiteral("sak paste round trip \x01\x02\x03 payload");
        const QString source_file = QDir(source_dir.path()).filePath(QStringLiteral("note.bin"));
        {
            QFile file(source_file);
            QVERIFY(file.open(QIODevice::WriteOnly));
            QCOMPARE(file.write(payload), payload.size());
        }

        sak::FileManagementExplorerPanel panel;
        panel.resize(1100, 700);
        panel.show();
        QVERIFY(QTest::qWaitForWindowExposed(&panel));

        auto* targetList = child<QListWidget>(&panel, "fileExplorerTargetList");
        auto* pathEdit = child<QLineEdit>(&panel, "fileExplorerPathEdit");
        auto* table = child<QTableView>(&panel, "fileExplorerTable");
        QVERIFY(targetList);
        QVERIFY(pathEdit);
        QVERIFY(table);

        if (selectLocalTargetRowForDrive(
                targetList, pathEdit, source_dir.path().left(2).toUpper()) < 0) {
            QSKIP("No mounted local target for the temp drive on this test host.");
        }

        const int file_row =
            navigateAndFindRow(pathEdit, table, source_dir.path(), QStringLiteral("note"));
        QVERIFY2(file_row >= 0, "source file not listed after navigation");
        table->selectRow(file_row);
        QApplication::processEvents();

        // Copy through the explorer command; the clipboard must carry the internal
        // payload plus real file URLs for OS interop.
        QTest::keyClick(table, Qt::Key_C, Qt::ControlModifier);
        QApplication::processEvents();
        const QMimeData* mime = QApplication::clipboard()->mimeData();
        QVERIFY(mime);
        QVERIFY(mime->hasFormat(QStringLiteral("application/x-sak-file-explorer-items")));
        QVERIFY(mime->hasUrls());

        // Navigate to the destination folder and paste.
        pathEdit->setText(destination_dir.path());
        QTest::keyClick(pathEdit, Qt::Key_Return);
        QVERIFY(QTest::qWaitFor(
            [pathEdit, &destination_dir]() {
                return QDir::fromNativeSeparators(pathEdit->text())
                    .contains(QDir(destination_dir.path()).dirName());
            },
            5000));
        QTest::keyClick(table, Qt::Key_V, Qt::ControlModifier);
        QApplication::processEvents();

        QFile copied(QDir(destination_dir.path()).filePath(QStringLiteral("note.bin")));
        QVERIFY2(waitForCompleteFile(copied, payload.size()),
                 "paste did not produce a complete, readable destination file");
        QVERIFY(copied.open(QIODevice::ReadOnly));
        QCOMPARE(copied.readAll(), payload);
    }

    void omnibarSearchModeShowsRecentsAndLiveSuggestions() {
        // Files Omnibar search mode: empty query lists the recent searches;
        // typed text debounces into live file suggestions; Enter on a file
        // suggestion navigates to it.
        QSettings settings;
        settings.beginGroup(QStringLiteral("FileManagementExplorer"));
        settings.setValue(QStringLiteral("SearchHistory"),
                          QStringList{QStringLiteral("prior-query")});
        settings.endGroup();
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        {
            QFile file(QDir(dir.path()).filePath(QStringLiteral("sak-suggest-probe.txt")));
            QVERIFY(file.open(QIODevice::WriteOnly));
            QVERIFY(file.write("suggestion payload") > 0);
        }

        sak::FileManagementExplorerPanel panel;
        panel.resize(1100, 700);
        panel.show();
        QVERIFY(QTest::qWaitForWindowExposed(&panel));

        auto* targetList = child<QListWidget>(&panel, "fileExplorerTargetList");
        auto* pathEdit = child<QLineEdit>(&panel, "fileExplorerPathEdit");
        auto* table = child<QTableView>(&panel, "fileExplorerTable");
        auto* searchButton = child<QPushButton>(&panel, "fileExplorerSearchButton");
        QVERIFY(targetList);
        QVERIFY(pathEdit);
        QVERIFY(table);
        QVERIFY(searchButton);
        if (selectLocalTargetRowForDrive(targetList, pathEdit, dir.path().left(2).toUpper()) < 0) {
            QSKIP("No mounted local target for the temp drive on this test host.");
        }
        QVERIFY(navigateAndFindRow(pathEdit, table, dir.path(), QStringLiteral("sak-suggest")) >=
                0);

        // The search button enters search mode: search placeholder + recents.
        searchButton->click();
        QApplication::processEvents();
        QCOMPARE(pathEdit->placeholderText(), QStringLiteral("Search for files and folders..."));
        auto* suggestions =
            panel.findChild<QListWidget*>(QStringLiteral("fileExplorerOmnibarSuggestions"));
        QVERIFY(suggestions);
        QTRY_VERIFY(suggestions->isVisible());
        bool recent_seen = false;
        for (int i = 0; i < suggestions->count(); ++i) {
            recent_seen = recent_seen ||
                          suggestions->item(i)->text() == QStringLiteral("prior-query");
        }
        QVERIFY2(recent_seen, "recent search query row missing");

        verifyLiveSearchSuggestionOpens(panel, pathEdit, suggestions, dir.path());
    }

    void verifyLiveSearchSuggestionOpens(sak::FileManagementExplorerPanel& panel,
                                         QLineEdit* pathEdit,
                                         QListWidget* suggestions,
                                         const QString& dir_path) {
        // Typing repopulates with debounced live matches carrying the path
        // (kSearchPathRole = UserRole + 9).
        QTest::keyClicks(pathEdit, QStringLiteral("sak-suggest-probe"));
        QTRY_VERIFY2_WITH_TIMEOUT(
            [suggestions]() {
                for (int i = 0; i < suggestions->count(); ++i) {
                    if (!suggestions->item(i)->data(Qt::UserRole + 9).isNull()) {
                        return true;
                    }
                }
                return false;
            }(),
            "no live file suggestion appeared",
            8000);

        // Enter on the file suggestion opens its location and selects it.
        for (int i = 0; i < suggestions->count(); ++i) {
            if (!suggestions->item(i)->data(Qt::UserRole + 9).isNull()) {
                suggestions->setCurrentRow(i);
                break;
            }
        }
        QTest::keyClick(pathEdit, Qt::Key_Return);
        QTRY_VERIFY(pathEdit->text().contains(dir_path.mid(3), Qt::CaseInsensitive) ||
                    pathEdit->text().compare(dir_path, Qt::CaseInsensitive) == 0);
        auto* omnibar = panel.findChild<sak::FileExplorerOmnibar*>();
        QVERIFY(omnibar);
        QCOMPARE(omnibar->mode(), sak::FileExplorerOmnibarMode::Path);
    }

    void evidenceReportsMatchTargetByPath() {
        // The Evidence pane surfaces live-cert report paths whose target_path matches the
        // current target; non-matching and malformed reports are ignored.
        QTemporaryDir root;
        QVERIFY(root.isValid());
        const QString target_path =
            QStringLiteral("\\\\?\\GLOBALROOT\\Device\\Harddisk9\\Partition2");
        const QDir dir(root.path());
        QVERIFY(dir.mkpath(QStringLiteral("run-a")));
        QVERIFY(dir.mkpath(QStringLiteral("run-b")));
        const auto writeReport = [](const QString& path, const QString& reportedTarget) {
            QFile file(path);
            QVERIFY(file.open(QIODevice::WriteOnly));
            const QString json =
                QStringLiteral("{\"targets\":[{\"target_path\":\"%1\"}]}")
                    .arg(QString(reportedTarget)
                             .replace(QStringLiteral("\\"), QStringLiteral("\\\\")));
            file.write(json.toUtf8());
        };
        writeReport(dir.filePath(QStringLiteral("run-a/report.json")), target_path);
        writeReport(dir.filePath(QStringLiteral("run-b/report.json")),
                    QStringLiteral("\\\\?\\GLOBALROOT\\Device\\Harddisk1\\Partition1"));
        {
            QFile junk(dir.filePath(QStringLiteral("run-b/notes.json")));
            QVERIFY(junk.open(QIODevice::WriteOnly));
            junk.write("not json at all");
        }

        const QStringList matches =
            sak::FileManagementExplorerPanel::evidenceReportsForTarget(root.path(), target_path);
        QCOMPARE(matches.size(), 1);
        QVERIFY(matches.first().contains(QStringLiteral("run-a")));

        // No target path -> no matches.
        QVERIFY(sak::FileManagementExplorerPanel::evidenceReportsForTarget(root.path(), QString())
                    .isEmpty());
    }

    void commandPaletteFilterNarrowsAndExecutesInline() {
        sak::FileManagementExplorerPanel panel;
        panel.resize(1100, 700);
        panel.show();
        QVERIFY(QTest::qWaitForWindowExposed(&panel));

        auto* targetList = child<QListWidget>(&panel, "fileExplorerTargetList");
        QVERIFY(targetList);
        if (firstTargetRow(targetList) < 0) {
            QSKIP("No mounted File Explorer targets on this test host.");
        }
        auto* pathEdit = child<QLineEdit>(&panel, "fileExplorerPathEdit");
        QVERIFY(pathEdit);
        panel.setFocus();
        QTest::keyClick(&panel, Qt::Key_P, Qt::ControlModifier | Qt::ShiftModifier);
        QApplication::processEvents();
        auto* suggestions =
            panel.findChild<QListWidget*>(QStringLiteral("fileExplorerOmnibarSuggestions"));
        QVERIFY(suggestions);
        const int fullCount = suggestions->count();
        QVERIFY(fullCount > 0);

        // Typing narrows by case-insensitive contains (Files palette filter).
        QTest::keyClicks(pathEdit, QStringLiteral("Hidden"));
        QApplication::processEvents();
        const int filteredCount = suggestions->count();
        QVERIFY(filteredCount > 0);
        QVERIFY(filteredCount < fullCount);
        // ToggleHiddenItems is the only command whose searchable text matches, and the row
        // label carries its shortcut suffix. The count floor and the fragment hide both a
        // dropped suffix and a filter that stopped narrowing.
        QCOMPARE(filteredCount, 1);
        QCOMPARE(suggestions->item(0)->text(), QStringLiteral("Hidden Items (Ctrl+H)"));

        // Enter executes the selected suggestion (ToggleHiddenItems) and the
        // omnibar reverts to Path mode.
        auto* pane = panel.findChild<sak::FileExplorerPane*>();
        QVERIFY(pane);
        const bool hidden_before = pane->showHiddenItems();
        QTest::keyClick(pathEdit, Qt::Key_Return);
        QApplication::processEvents();
        QTRY_COMPARE(pane->showHiddenItems(), !hidden_before);
        QTRY_VERIFY(!suggestions->isVisible());
        auto* omnibar = panel.findChild<sak::FileExplorerOmnibar*>();
        QVERIFY(omnibar);
        QCOMPARE(omnibar->mode(), sak::FileExplorerOmnibarMode::Path);
    }

    void commandPaletteReportsNoMatchRow() {
        sak::FileManagementExplorerPanel panel;
        panel.resize(1100, 700);
        panel.show();
        QVERIFY(QTest::qWaitForWindowExposed(&panel));

        auto* pathEdit = child<QLineEdit>(&panel, "fileExplorerPathEdit");
        QVERIFY(pathEdit);
        panel.setFocus();
        QTest::keyClick(&panel, Qt::Key_P, Qt::ControlModifier | Qt::ShiftModifier);
        QApplication::processEvents();
        auto* suggestions =
            panel.findChild<QListWidget*>(QStringLiteral("fileExplorerOmnibarSuggestions"));
        QVERIFY(suggestions);

        // Files NoCommandsFound: a single non-selectable row reports the miss.
        QTest::keyClicks(pathEdit, QStringLiteral("zz-no-such-command"));
        QApplication::processEvents();
        QCOMPARE(suggestions->count(), 1);
        QCOMPARE(suggestions->item(0)->flags(), Qt::NoItemFlags);
        // A row that lost .arg(needle) still contains the fragment.
        QCOMPARE(suggestions->item(0)->text(),
                 QStringLiteral("There are no commands containing zz-no-such-command"));
    }

    void duplicateTabClonesCurrentTab() {
        sak::FileManagementExplorerPanel panel;
        panel.resize(1100, 700);
        panel.show();
        QVERIFY(QTest::qWaitForWindowExposed(&panel));

        auto* tabs = child<QTabBar>(&panel, "fileExplorerTabBar");
        auto* view = child<QToolButton>(&panel, "fileExplorerViewButton");
        QVERIFY(tabs);
        QVERIFY(view);
        QVERIFY(view->menu());
        QCOMPARE(tabs->count(), 1);

        QAction* duplicate = actionStartingWith(view->menu(), QStringLiteral("Duplicate Tab"));
        QVERIFY(duplicate);
        QVERIFY(duplicate->isEnabled());
        duplicate->trigger();
        QApplication::processEvents();
        QCOMPARE(tabs->count(), 2);
    }

    void reopenClosedTabRestoresLastClosedTab() {
        sak::FileManagementExplorerPanel panel;
        panel.resize(1100, 700);
        panel.show();
        QVERIFY(QTest::qWaitForWindowExposed(&panel));

        auto* tabs = child<QTabBar>(&panel, "fileExplorerTabBar");
        auto* newTab = child<QPushButton>(&panel, "fileExplorerNewTabButton");
        auto* view = child<QToolButton>(&panel, "fileExplorerViewButton");
        QVERIFY(tabs);
        QVERIFY(newTab);
        QVERIFY(view);

        // Reopen is disabled until a tab has actually been closed.
        QVERIFY(
            !actionStartingWith(view->menu(), QStringLiteral("Reopen Closed Tab"))->isEnabled());

        newTab->click();
        QApplication::processEvents();
        QCOMPARE(tabs->count(), 2);

        QWidget* closeButton = tabs->tabButton(1, QTabBar::RightSide);
        if (!closeButton) {
            closeButton = tabs->tabButton(1, QTabBar::LeftSide);
        }
        QVERIFY(closeButton);
        QTest::mouseClick(closeButton, Qt::LeftButton);
        QApplication::processEvents();
        QCOMPARE(tabs->count(), 1);

        // Run "Reopen Closed Tab" through the inline palette (it now resolves
        // to enabled from the live context) to restore the closed tab.
        auto* pathEdit = child<QLineEdit>(&panel, "fileExplorerPathEdit");
        QVERIFY(pathEdit);
        panel.setFocus();
        QTest::keyClick(&panel, Qt::Key_P, Qt::ControlModifier | Qt::ShiftModifier);
        QApplication::processEvents();
        auto* suggestions =
            panel.findChild<QListWidget*>(QStringLiteral("fileExplorerOmnibarSuggestions"));
        QVERIFY(suggestions);
        QTest::keyClicks(pathEdit, QStringLiteral("Reopen Closed Tab"));
        QApplication::processEvents();
        // Exactly one command matches the typed phrase, and its label carries the shortcut.
        // The count floor plus the prefix probe accept a dropped suffix and extra rows.
        QCOMPARE(suggestions->count(), 1);
        QCOMPARE(suggestions->item(0)->text(), QStringLiteral("Reopen Closed Tab (Ctrl+Shift+T)"));
        QTest::keyClick(pathEdit, Qt::Key_Return);
        QApplication::processEvents();
        QCOMPARE(tabs->count(), 2);
    }

    void transferWorkerCopiesTreeReportsProgressAndCancels() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QDir root(dir.path());
        QVERIFY(root.mkpath(QStringLiteral("bundle/deep")));
        QVERIFY(root.mkdir(QStringLiteral("dest")));
        const auto writeFile = [](const QString& path, const QByteArray& data) {
            QFile file(path);
            QVERIFY(file.open(QIODevice::WriteOnly));
            QCOMPARE(file.write(data), data.size());
        };
        const QByteArray big_payload(1'500'000, 'a');
        const QByteArray leaf_payload(500'000, 'b');
        writeFile(root.filePath(QStringLiteral("bundle/a.bin")), big_payload);
        writeFile(root.filePath(QStringLiteral("bundle/deep/b.bin")), leaf_payload);

        // Full run: discovery counts the tree, byte progress reaches the
        // total, and the copy lands byte-exact.
        sak::FileExplorerTransferRequest request;
        request.source_target = sak::FileManagementFileSystemBridge::localTarget(dir.path());
        request.destination_target = request.source_target;
        request.items = {{root.filePath(QStringLiteral("bundle")),
                          root.filePath(QStringLiteral("dest/bundle")),
                          0,
                          true}};
        request.raw_read_cap = 512ULL * 1024 * 1024;
        sak::FileExplorerTransferWorker worker(request);
        QSignalSpy progress_spy(&worker, &sak::FileExplorerTransferWorker::statusProgress);
        QSignalSpy finished_spy(&worker, &QThread::finished);
        worker.start();
        QTRY_COMPARE_WITH_TIMEOUT(finished_spy.count(), 1, 10'000);
        QVERIFY2(worker.blockers().isEmpty(),
                 qPrintable(worker.blockers().join(QStringLiteral("; "))));
        QCOMPARE(worker.completedItems().size(), 1);
        QFile copied(root.filePath(QStringLiteral("dest/bundle/deep/b.bin")));
        QVERIFY(copied.open(QIODevice::ReadOnly));
        QCOMPARE(copied.readAll(), leaf_payload);
        QVERIFY(progress_spy.count() > 0);
        const auto last = progress_spy.last().first().value<sak::FileExplorerStatusProgress>();
        QVERIFY(last.enumeration_completed);
        QCOMPARE(last.processed_size,
                 static_cast<qint64>(big_payload.size() + leaf_payload.size()));
        QCOMPARE(last.total_size, last.processed_size);
        QCOMPARE(last.processed_items_count, 1);

        // Cancel at start (WorkerBase::run resets the stop flag on entry, so
        // the request lands via started() on the worker thread): the worker
        // reports cancelled and copies nothing.
        sak::FileExplorerTransferRequest canceled_request = request;
        canceled_request.items = {{root.filePath(QStringLiteral("bundle")),
                                   root.filePath(QStringLiteral("dest/bundle2")),
                                   0,
                                   true}};
        sak::FileExplorerTransferWorker canceled_worker(canceled_request);
        QSignalSpy cancelled_spy(&canceled_worker, &WorkerBase::cancelled);
        QSignalSpy canceled_finished(&canceled_worker, &QThread::finished);
        connect(&canceled_worker,
                &WorkerBase::started,
                &canceled_worker,
                &WorkerBase::requestStop,
                Qt::DirectConnection);
        canceled_worker.start();
        QTRY_COMPARE_WITH_TIMEOUT(canceled_finished.count(), 1, 10'000);
        QCOMPARE(cancelled_spy.count(), 1);
        QVERIFY(canceled_worker.completedItems().isEmpty());
        QVERIFY(!QDir(root.filePath(QStringLiteral("dest/bundle2"))).exists());
    }

    void incompleteCopyIsNotReportedAsACompletedItem() {
        // R5 p9_filemgmt-10: completedItems() promises "landed whole", but an ordinary
        // (non-move) copy was appended to it on transferEntry's plain ok, even when the
        // walk silently dropped entries at the depth bound. Such a copy must now be
        // reported as a failed item with a blocker instead.
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QDir root(dir.path());
        QString deep = QStringLiteral("tree");
        for (int level = 0; level < 40; ++level) {
            deep += QStringLiteral("/d");
        }
        QVERIFY(root.mkpath(deep));
        {
            QFile bottom(root.filePath(deep + QStringLiteral("/leaf.txt")));
            QVERIFY(bottom.open(QIODevice::WriteOnly));
            QVERIFY(bottom.write("deep leaf") > 0);
        }
        QVERIFY(root.mkdir(QStringLiteral("dest")));

        sak::FileExplorerTransferRequest request;
        request.source_target = sak::FileManagementFileSystemBridge::localTarget(dir.path());
        request.destination_target = request.source_target;
        request.items = {{root.filePath(QStringLiteral("tree")),
                          root.filePath(QStringLiteral("dest/tree")),
                          0,
                          true}};
        request.raw_read_cap = 512ULL * 1024 * 1024;
        sak::FileExplorerTransferWorker worker(request);
        QSignalSpy finished_spy(&worker, &QThread::finished);
        worker.start();
        QTRY_COMPARE_WITH_TIMEOUT(finished_spy.count(), 1, 20'000);

        QVERIFY2(worker.completedItems().isEmpty(),
                 "a copy that dropped entries was still listed as completed");
        // Both path arguments, in order: the joined-substring probe hides a mis-pairing of
        // source and destination -- the classic bug in a two-.arg() message -- and hides the
        // list size, so extra blockers from the depth cap would go unnoticed.
        QCOMPARE(worker.blockers(),
                 (QStringList{QStringLiteral("%1 did not copy whole; the partial copy at %2 is "
                                             "not reported as a completed item.")
                                  .arg(root.filePath(QStringLiteral("tree")),
                                       root.filePath(QStringLiteral("dest/tree")))}));
        // The source is untouched (this is a copy, not a move).
        QVERIFY(QFileInfo(root.filePath(QStringLiteral("tree"))).isDir());
    }

    void itemCountedAndZeroByteRunsReachATerminalStatus() {
        // R5 p9_filemgmt-11: the reporter's auto-success needs a non-zero size
        // denominator, which the delete family (and a batch of zero-byte files) never
        // has, and execute() only ever set Cancelled/Failed. A clean run therefore sat
        // at InProgress forever: the card spun and completed-item cleanup never reaped
        // it. execute() now sets Success explicitly on a clean run.
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QDir root(dir.path());
        {
            QFile doomed(root.filePath(QStringLiteral("doomed.txt")));
            QVERIFY(doomed.open(QIODevice::WriteOnly));
            QVERIFY(doomed.write("delete me") > 0);
        }

        sak::FileExplorerTransferRequest deletion;
        deletion.source_target = sak::FileManagementFileSystemBridge::localTarget(dir.path());
        deletion.destination_target = deletion.source_target;
        deletion.kind = sak::FileExplorerTransferKind::Delete;
        deletion.items = {{root.filePath(QStringLiteral("doomed.txt")), QString(), 9, false}};
        sak::FileExplorerTransferWorker delete_worker(deletion);
        QSignalSpy delete_progress(&delete_worker,
                                   &sak::FileExplorerTransferWorker::statusProgress);
        QSignalSpy delete_finished(&delete_worker, &QThread::finished);
        delete_worker.start();
        QTRY_COMPARE_WITH_TIMEOUT(delete_finished.count(), 1, 10'000);
        QVERIFY2(delete_worker.blockers().isEmpty(),
                 qPrintable(delete_worker.blockers().join(QStringLiteral("; "))));
        QVERIFY(!QFile::exists(root.filePath(QStringLiteral("doomed.txt"))));
        QVERIFY(delete_progress.count() > 0);
        QCOMPARE(delete_progress.last().first().value<sak::FileExplorerStatusProgress>().status,
                 sak::FileExplorerReturnResult::Success);

        // A byte-moving transfer whose items are all zero-byte has the same zero
        // denominator, so it needs the same explicit terminal status.
        QVERIFY(root.mkdir(QStringLiteral("empty-dest")));
        {
            QFile empty(root.filePath(QStringLiteral("empty.txt")));
            QVERIFY(empty.open(QIODevice::WriteOnly));
        }
        sak::FileExplorerTransferRequest copy;
        copy.source_target = deletion.source_target;
        copy.destination_target = deletion.source_target;
        copy.items = {{root.filePath(QStringLiteral("empty.txt")),
                       root.filePath(QStringLiteral("empty-dest/empty.txt")),
                       0,
                       false}};
        sak::FileExplorerTransferWorker copy_worker(copy);
        QSignalSpy copy_progress(&copy_worker, &sak::FileExplorerTransferWorker::statusProgress);
        QSignalSpy copy_finished(&copy_worker, &QThread::finished);
        copy_worker.start();
        QTRY_COMPARE_WITH_TIMEOUT(copy_finished.count(), 1, 10'000);
        QVERIFY2(copy_worker.blockers().isEmpty(),
                 qPrintable(copy_worker.blockers().join(QStringLiteral("; "))));
        QCOMPARE(copy_worker.completedItems().size(), 1);
        QVERIFY(copy_progress.count() > 0);
        QCOMPARE(copy_progress.last().first().value<sak::FileExplorerStatusProgress>().status,
                 sak::FileExplorerReturnResult::Success);
    }

    void recycleRefusesVolumesWithoutABinInsteadOfDeletingPermanently() {
        // R5 p9_filemgmt-7: on a volume with no Recycle Bin (a UNC share is the
        // canonical case) the shell's FOF_ALLOWUNDO delete is PERMANENT, so a recycle
        // there silently destroyed the item while the worker reported a recoverable
        // move to the bin. The worker now refuses outright and names the reason. The
        // path is rejected before any shell/network call, so no host is contacted.
        sak::FileExplorerTransferRequest request;
        request.source_target = sak::FileManagementFileSystemBridge::localTarget(QString());
        request.destination_target = request.source_target;
        request.kind = sak::FileExplorerTransferKind::Recycle;
        request.items = {
            {QStringLiteral("\\\\sak-no-such-host\\share\\payload.txt"), QString(), 0, false}};
        sak::FileExplorerTransferWorker worker(request);
        QSignalSpy finished_spy(&worker, &QThread::finished);
        worker.start();
        QTRY_COMPARE_WITH_TIMEOUT(finished_spy.count(), 1, 10'000);

        QVERIFY(worker.completedItems().isEmpty());
        // The whole blocker: the sibling this test exists to exclude is "Could not move %1 to
        // the Recycle Bin.", which is what a regressed pathVolumeHasRecycleBin would produce
        // after actually attempting the shell delete.
        QCOMPARE(worker.blockers(),
                 (QStringList{QStringLiteral("\\\\sak-no-such-host\\share\\payload.txt is not on "
                                             "a volume with a Recycle Bin, so it was left in "
                                             "place; use Delete to remove it permanently.")}));
    }

    void pasteCopyRunsOnWorkerWithStatusCards() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QDir root(dir.path());
        QVERIFY(root.mkdir(QStringLiteral("pocket")));
        {
            QFile file(root.filePath(QStringLiteral("payload.txt")));
            QVERIFY(file.open(QIODevice::WriteOnly));
            QVERIFY(file.write("worker paste payload") > 0);
        }

        sak::FileManagementExplorerPanel panel;
        panel.resize(1100, 700);
        panel.show();
        QVERIFY(QTest::qWaitForWindowExposed(&panel));
        auto* targetList = child<QListWidget>(&panel, "fileExplorerTargetList");
        auto* pathEdit = child<QLineEdit>(&panel, "fileExplorerPathEdit");
        auto* table = child<QTableView>(&panel, "fileExplorerTable");
        QVERIFY(targetList);
        QVERIFY(pathEdit);
        QVERIFY(table);
        if (selectLocalTargetRowForDrive(targetList, pathEdit, dir.path().left(2).toUpper()) < 0) {
            QSKIP("No mounted local target for the temp drive on this test host.");
        }
        QVERIFY(navigateAndFindRow(pathEdit, table, dir.path(), QStringLiteral("payload")) >= 0);

        // Copy + paste into the selected folder runs on the transfer worker:
        // the file lands, and the Files two-card pattern leaves one terminal
        // Success card ("Copied 1 item to ...") in the status center.
        QVERIFY(selectRowStable(table, QStringLiteral("payload")));
        QTest::keyClick(table, Qt::Key_C, Qt::ControlModifier);
        QVERIFY(selectRowStable(table, QStringLiteral("pocket")));
        QTest::keyClick(table, Qt::Key_V, Qt::ControlModifier | Qt::ShiftModifier);
        const QString pasted = root.filePath(QStringLiteral("pocket/payload.txt"));
        QVERIFY2(QTest::qWaitFor([&pasted]() { return QFile::exists(pasted); }, 5000),
                 "worker paste did not land");
        QTRY_COMPARE(panel.statusCenterModel()->inProgressCount(), 0);
        QTRY_VERIFY(panel.statusCenterModel()->hasAnyItem());
        const auto* card = panel.statusCenterModel()->items().first();
        QCOMPARE(card->kind(), sak::FileExplorerStatusItemKind::Successful);
        QCOMPARE(card->header(), QStringLiteral("Copied 1 item to \"pocket\""));
    }

    void statusCenterFlyoutOpensWithEmptyState() {
        sak::FileManagementExplorerPanel panel;
        panel.resize(1100, 700);
        panel.show();
        QVERIFY(QTest::qWaitForWindowExposed(&panel));

        // Files ShowStatusCenterButton at the toolbar's right edge toggles
        // the flyout; with no operations it shows the empty state and a
        // disabled "Clear completed".
        auto* button = child<QPushButton>(&panel, "fileExplorerStatusCenterButton");
        QVERIFY(button);
        QVERIFY(!button->icon().isNull());
        QTest::mouseClick(button, Qt::LeftButton);
        QApplication::processEvents();
        auto* flyout =
            panel.window()->findChild<QFrame*>(QStringLiteral("fileExplorerStatusCenterFlyout"));
        QVERIFY(flyout);
        QTRY_VERIFY(flyout->isVisible());
        auto* title = child<QLabel>(flyout, "fileExplorerStatusCenterTitle");
        QVERIFY(title);
        QCOMPARE(title->text(), QStringLiteral("Status center"));
        auto* empty = child<QLabel>(flyout, "fileExplorerStatusEmptyLabel");
        QVERIFY(empty);
        QVERIFY(empty->isVisible());
        QCOMPARE(empty->text(), QStringLiteral("No ongoing file operations"));
        auto* clear = child<QPushButton>(flyout, "fileExplorerStatusClearCompleted");
        QVERIFY(clear);
        QVERIFY(!clear->isEnabled());

        // Second click closes the flyout again.
        QTest::mouseClick(button, Qt::LeftButton);
        QTRY_VERIFY(!flyout->isVisible());
    }

    void verifyInProgressCardAndCancel(QFrame* flyout, sak::FileExplorerStatusCenterItem* item) {
        auto* card = flyout->findChild<QFrame*>(QStringLiteral("fileExplorerStatusCard"));
        QVERIFY(card);
        auto* title = child<QLabel>(card, "fileExplorerStatusCardTitle");
        QVERIFY(title);
        QCOMPARE(title->text(), QStringLiteral("Discovered 2 items"));
        auto* progressBar = child<QProgressBar>(card, "fileExplorerStatusCardProgressBar");
        QVERIFY(progressBar);
        QVERIFY(progressBar->isVisible());
        // In-progress cards carry no dismiss X (Files CloseItemButton loads
        // only for completed cards) but do offer the expand chevron.
        auto* dismiss = child<QToolButton>(card, "fileExplorerStatusCardDismiss");
        QVERIFY(dismiss);
        QVERIFY(!dismiss->isVisible());
        auto* expand = child<QToolButton>(card, "fileExplorerStatusCardExpand");
        QVERIFY(expand);
        QVERIFY(expand->isVisible());

        // A progress report drives the bar; expanding swaps it for the speed
        // graph with the Speed/Name rows (Files expanded card).
        sak::FileExplorerStatusProgress progress;
        progress.enumeration_completed = true;
        progress.total_size = 1000;
        progress.processed_size = 400;
        progress.items_count = 2;
        progress.processed_items_count = 1;
        progress.size_speed = 800.0;
        progress.file_name = QStringLiteral("a.txt");
        item->reportProgress(progress);
        QApplication::processEvents();
        QCOMPARE(progressBar->value(), 40);
        expand->click();
        QApplication::processEvents();
        auto* graphFrame = child<QFrame>(card, "fileExplorerStatusCardGraphFrame");
        QVERIFY(graphFrame);
        QTRY_VERIFY(graphFrame->isVisible());
        QVERIFY(!progressBar->isVisible());
        auto* speed = child<QLabel>(card, "fileExplorerStatusCardSpeed");
        QVERIFY(speed);
        QCOMPARE(speed->text(), QStringLiteral("Speed: 800 bytes/s"));
        auto* name = child<QLabel>(card, "fileExplorerStatusCardName");
        QVERIFY(name);
        QVERIFY(name->isVisible());
        QCOMPARE(name->text(), QStringLiteral("Name: a.txt"));
        expand->click();
        QApplication::processEvents();

        // Cancel through the more-options flyout: the card flags Canceling,
        // goes indeterminate, and loses its cancel affordance.
        auto* more = child<QToolButton>(card, "fileExplorerStatusCardMore");
        QVERIFY(more);
        QVERIFY(more->isVisible());
        auto* cancelAction =
            card->findChild<QAction*>(QStringLiteral("fileExplorerStatusCardCancel"));
        QVERIFY(cancelAction);
        cancelAction->trigger();
        QApplication::processEvents();
        QVERIFY(item->isCancelRequested());
        // requestCancel prepends, so the surviving tail is the contract: the rebuilt copy
        // header plus the live percentage the progress report above drove to 40.
        QCOMPARE(title->text(), QStringLiteral("Canceling - Copying 2 items to \"Bundle\" (40%)"));
        QVERIFY(!more->isVisible());
    }

    void statusCenterCardsRenderCancelAndDismiss() {
        sak::FileManagementExplorerPanel panel;
        panel.resize(1100, 700);
        panel.show();
        QVERIFY(QTest::qWaitForWindowExposed(&panel));

        // An in-progress copy card: the toolbar button swaps its icon for the
        // ring+badge overlays (Files hides the icon while the ring shows).
        sak::FileExplorerStatusCardRequest request;
        request.operation = sak::FileExplorerOperationType::Copy;
        request.result = sak::FileExplorerReturnResult::InProgress;
        request.destination = {QStringLiteral("C:/exports/Bundle")};
        request.items_count = 2;
        request.can_provide_progress = true;
        request.cancelable = true;
        auto* item = panel.statusCenterModel()->addItem(request);
        auto* button = child<QPushButton>(&panel, "fileExplorerStatusCenterButton");
        QVERIFY(button);
        QTRY_VERIFY(button->icon().isNull());

        QTest::mouseClick(button, Qt::LeftButton);
        QApplication::processEvents();
        auto* flyout =
            panel.window()->findChild<QFrame*>(QStringLiteral("fileExplorerStatusCenterFlyout"));
        QVERIFY(flyout);
        QTRY_VERIFY(flyout->isVisible());
        verifyInProgressCardAndCancel(flyout, item);
        if (QTest::currentTestFailed()) {
            return;
        }

        // The Files two-card pattern: the in-progress card is removed and a
        // terminal success card takes its place, dismissible via its X.
        panel.statusCenterModel()->removeItem(item);
        request.result = sak::FileExplorerReturnResult::Success;
        request.cancelable = false;
        panel.statusCenterModel()->addItem(request);
        QApplication::processEvents();
        auto* card = flyout->findChild<QFrame*>(QStringLiteral("fileExplorerStatusCard"));
        QVERIFY(card);
        auto* titleLabel = child<QLabel>(card, "fileExplorerStatusCardTitle");
        QVERIFY(titleLabel);
        QCOMPARE(titleLabel->text(), QStringLiteral("Copied 2 items to \"Bundle\""));
        auto* dismiss = child<QToolButton>(card, "fileExplorerStatusCardDismiss");
        QVERIFY(dismiss);
        QTRY_VERIFY(dismiss->isVisible());
        QTest::mouseClick(dismiss, Qt::LeftButton);
        QApplication::processEvents();
        QVERIFY(!panel.statusCenterModel()->hasAnyItem());
        auto* empty = child<QLabel>(flyout, "fileExplorerStatusEmptyLabel");
        QVERIFY(empty);
        QTRY_VERIFY(empty->isVisible());
    }

    void permanentDeleteRunsOnWorkerWithDeleteCard() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString doomed = QDir(dir.path()).filePath(QStringLiteral("doomed.txt"));
        {
            QFile file(doomed);
            QVERIFY(file.open(QIODevice::WriteOnly));
            QVERIFY(file.write("delete me") > 0);
        }

        sak::FileManagementExplorerPanel panel;
        panel.resize(1100, 700);
        panel.show();
        QVERIFY(QTest::qWaitForWindowExposed(&panel));
        auto* targetList = child<QListWidget>(&panel, "fileExplorerTargetList");
        auto* pathEdit = child<QLineEdit>(&panel, "fileExplorerPathEdit");
        auto* table = child<QTableView>(&panel, "fileExplorerTable");
        QVERIFY(targetList);
        QVERIFY(pathEdit);
        QVERIFY(table);
        if (selectLocalTargetRowForDrive(targetList, pathEdit, dir.path().left(2).toUpper()) < 0) {
            QSKIP("No mounted local target for the temp drive on this test host.");
        }
        QVERIFY(navigateAndFindRow(pathEdit, table, dir.path(), QStringLiteral("doomed")) >= 0);

        // Shift+Delete: permanent delete runs on the worker and posts the
        // Files Delete-family card ("Deleted N items from ...").
        QVERIFY(selectRowStable(table, QStringLiteral("doomed")));
        panel.activateWindow();
        table->setFocus();
        QApplication::processEvents();
        QString question_text;
        armAutoAcceptQuestion(&question_text);
        QTest::keyClick(table, Qt::Key_Delete, Qt::ShiftModifier);
        QTRY_VERIFY(!QFile::exists(doomed));
        verifyPermanentDeleteConfirmation(question_text, QStringLiteral("doomed.txt"));
        QTRY_COMPARE(panel.statusCenterModel()->inProgressCount(), 0);
        QTRY_VERIFY(panel.statusCenterModel()->hasAnyItem());
        const auto* card = panel.statusCenterModel()->items().first();
        QCOMPARE(card->kind(), sak::FileExplorerStatusItemKind::Successful);
        // The prefix probe drops the second field entirely, so `Deleted 1 item from ""` -- the
        // exact empty-m_source failure mode -- passes.
        QCOMPARE(card->header(),
                 QStringLiteral("Deleted 1 item from \"%1\"").arg(QDir(dir.path()).dirName()));
    }

    void statusCenterVisibilitySettingTogglesButton() {
        sak::FileManagementExplorerPanel panel;
        panel.resize(1100, 700);
        panel.show();
        QVERIFY(QTest::qWaitForWindowExposed(&panel));
        auto* button = child<QPushButton>(&panel, "fileExplorerStatusCenterButton");
        QVERIFY(button);
        QVERIFY(button->isVisible());

        // Files StatusCenterVisibility = DuringOngoingFileOperations: the
        // settings dialog persists it and the idle button hides; any card
        // brings it back.
        QTimer::singleShot(0, [&panel]() {
            auto* dialog = panel.findChild<QDialog*>(QStringLiteral("fileExplorerSettingsDialog"));
            QVERIFY(dialog);
            auto* combo =
                dialog->findChild<QComboBox*>(QStringLiteral("fileExplorerSettingsStatusCenter"));
            QVERIFY(combo);
            QCOMPARE(combo->currentIndex(), 0);
            combo->setCurrentIndex(1);
            dialog->accept();
        });
        QTest::keyClick(&panel, Qt::Key_Comma, Qt::ControlModifier);
        QApplication::processEvents();
        QTRY_VERIFY(!button->isVisible());

        sak::FileExplorerStatusCardRequest request;
        request.result = sak::FileExplorerReturnResult::Success;
        request.operation = sak::FileExplorerOperationType::Copy;
        request.destination = {QStringLiteral("C:/exports/Bundle")};
        request.items_count = 1;
        panel.statusCenterModel()->addItem(request);
        QTRY_VERIFY(button->isVisible());
    }

    // CODEX_REVIEW_2 file_management_explorer_panel.cpp:3800 -- redo of a
    // create must never rewrite an empty file over data (or a folder) the user
    // placed at the historical path since.
    void redoCreateActionRefusesToClobberOccupiedPaths() {
        using sak::FileExplorerOccupant;
        using sak::FileExplorerRedoCreateAction;

        // Vacant paths recreate; an unauthoritative listing fails closed.
        QCOMPARE(sak::fileExplorerRedoCreateAction(FileExplorerOccupant::Vacant, false, false),
                 FileExplorerRedoCreateAction::Create);
        QCOMPARE(sak::fileExplorerRedoCreateAction(FileExplorerOccupant::Vacant, true, false),
                 FileExplorerRedoCreateAction::Create);
        QCOMPARE(sak::fileExplorerRedoCreateAction(FileExplorerOccupant::Unknown, false, false),
                 FileExplorerRedoCreateAction::Block);

        // A file create finds a same-named file: only an empty one is the item
        // it produced; a populated file would be clobbered, so block it.
        QCOMPARE(sak::fileExplorerRedoCreateAction(FileExplorerOccupant::File, false, true),
                 FileExplorerRedoCreateAction::SkipIdentical);
        QCOMPARE(sak::fileExplorerRedoCreateAction(FileExplorerOccupant::File, false, false),
                 FileExplorerRedoCreateAction::Block);

        // Kind swaps never recreate over the other kind.
        QCOMPARE(sak::fileExplorerRedoCreateAction(FileExplorerOccupant::Directory, false, false),
                 FileExplorerRedoCreateAction::Block);
        QCOMPARE(sak::fileExplorerRedoCreateAction(FileExplorerOccupant::File, true, false),
                 FileExplorerRedoCreateAction::Block);

        // A directory create is idempotent when a directory already exists.
        QCOMPARE(sak::fileExplorerRedoCreateAction(FileExplorerOccupant::Directory, true, false),
                 FileExplorerRedoCreateAction::SkipIdentical);
    }

    // CODEX_REVIEW_2 file_management_explorer_panel.cpp:3925 -- undo-delete
    // must re-verify identity (kind + size/child-count), not just kind, before
    // recycling or (on raw targets) permanently deleting.
    void historyDeleteVerdictRefusesMismatchedIdentity() {
        using sak::FileExplorerHistoryDeleteVerdict;
        using sak::FileExplorerOccupant;

        // Vacant needs nothing; an unauthoritative listing fails closed.
        QCOMPARE(sak::fileExplorerHistoryDeleteVerdict(FileExplorerOccupant::Vacant, false, -1, 0),
                 FileExplorerHistoryDeleteVerdict::Skip);
        QCOMPARE(sak::fileExplorerHistoryDeleteVerdict(FileExplorerOccupant::Unknown, false, 0, 0),
                 FileExplorerHistoryDeleteVerdict::Block);

        // Identity holds: a file whose size still matches the captured value
        // (0 for a created empty file, N for a copy) may be deleted.
        QCOMPARE(sak::fileExplorerHistoryDeleteVerdict(FileExplorerOccupant::File, false, 0, 0),
                 FileExplorerHistoryDeleteVerdict::Delete);
        QCOMPARE(sak::fileExplorerHistoryDeleteVerdict(FileExplorerOccupant::File, false, 42, 42),
                 FileExplorerHistoryDeleteVerdict::Delete);

        // A different size, an unmeasurable entry, or a missing identity
        // source all block so an unrelated same-named entry is never deleted.
        QCOMPARE(sak::fileExplorerHistoryDeleteVerdict(FileExplorerOccupant::File, false, 99, 0),
                 FileExplorerHistoryDeleteVerdict::Block);
        QCOMPARE(sak::fileExplorerHistoryDeleteVerdict(FileExplorerOccupant::File, false, -1, 0),
                 FileExplorerHistoryDeleteVerdict::Block);
        QCOMPARE(sak::fileExplorerHistoryDeleteVerdict(FileExplorerOccupant::File, false, 42, -1),
                 FileExplorerHistoryDeleteVerdict::Block);

        // Kind mismatch (a folder now sits where a file was) blocks; matching
        // empty directories delete; non-empty ones block.
        QCOMPARE(
            sak::fileExplorerHistoryDeleteVerdict(FileExplorerOccupant::Directory, false, 0, 0),
            FileExplorerHistoryDeleteVerdict::Block);
        QCOMPARE(sak::fileExplorerHistoryDeleteVerdict(FileExplorerOccupant::Directory, true, 0, 0),
                 FileExplorerHistoryDeleteVerdict::Delete);
        QCOMPARE(sak::fileExplorerHistoryDeleteVerdict(FileExplorerOccupant::Directory, true, 3, 0),
                 FileExplorerHistoryDeleteVerdict::Block);
    }

    // The properties size walk runs on a QtConcurrent worker whose future ignores
    // QFuture::cancel(); cancellation is cooperative via a shared flag threaded
    // through the directory lister. A set flag must stop the walk cold so the
    // dialog's destructor waitForFinished() returns instead of freezing the GUI.
    void propertiesSizeWalkStopsWhenCancelFlagIsSet() {
        int calls = 0;
        sak::DirectoryLister base = [&calls](const QString& path, int) {
            ++calls;
            sak::FileManagementListResult listing;
            listing.ok = true;
            sak::FileManagementEntry file;
            file.regular_file = true;
            file.size_bytes = 100;
            file.path = path + QStringLiteral("/f");
            listing.entries.append(file);
            return listing;
        };

        auto flag = std::make_shared<std::atomic_bool>(false);
        const sak::DirectoryLister lister = sak::makeCancelableLister(base, flag);

        const sak::TreeSizeResult counted = sak::treeSize(lister, QStringLiteral("/root"), 3, 100);
        QVERIFY(counted.complete);
        QCOMPARE(counted.bytes, quint64(100));
        QVERIFY(calls > 0);

        calls = 0;
        flag->store(true);
        const sak::TreeSizeResult cancelled =
            sak::treeSize(lister, QStringLiteral("/root"), 3, 100);
        // Cancelled: base lister never runs, nothing is counted, and the total is
        // flagged incomplete so it can only ever be shown as a lower bound.
        QCOMPARE(calls, 0);
        QCOMPARE(cancelled.bytes, quint64(0));
        QVERIFY(!cancelled.complete);
    }
};

QTEST_MAIN(FileManagementExplorerPanelTests)
#include "test_file_management_explorer_panel.moc"
