// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file test_file_management_explorer_panel.cpp
/// @brief GUI tests for the File Management Explorer shell.

#include "sak/file_management_explorer_panel.h"
#include "sak/file_explorer_details_view.h"
#include "sak/file_explorer_icon_registry.h"
#include "sak/file_explorer_item_model.h"
#include "sak/file_explorer_pane.h"
#include "sak/file_explorer_sort_filter_model.h"

#include <QAbstractItemView>
#include <QAction>
#include <QApplication>
#include <QContextMenuEvent>
#include <QCoreApplication>
#include <QDir>
#include <QDialog>
#include <QHeaderView>
#include <QImage>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QListView>
#include <QMenu>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSettings>
#include <QSlider>
#include <QTabWidget>
#include <QTableView>
#include <QTimer>
#include <QToolButton>
#include <QtTest/QtTest>

#include <algorithm>

namespace {

template <typename Widget>
Widget* child(QWidget* parent, const char* name) {
    return parent->findChild<Widget*>(QString::fromLatin1(name));
}

QStringList collectContextMenuTexts(QWidget* target) {
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

    const QPoint point = target->rect().center();
    QContextMenuEvent event(QContextMenuEvent::Mouse, point, target->mapToGlobal(point));
    QApplication::sendEvent(target, &event);
    QApplication::processEvents();
    QApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    return texts;
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

void captureBaseline(QWidget* widget, const QString& name) {
    if (qEnvironmentVariableIsEmpty("SAK_CAPTURE_FILE_EXPLORER_BASELINE")) {
        return;
    }

    QDir dir(QDir::currentPath());
    QVERIFY(dir.mkpath(QStringLiteral("artifacts/file-management-explorer-baseline")));
    const QString path =
        dir.filePath(QStringLiteral("artifacts/file-management-explorer-baseline/%1.png").arg(name));
    QVERIFY2(widget->grab().save(path), qPrintable(path));
}

int firstTargetRow(QListWidget* list) {
    if (!list) {
        return -1;
    }
    for (int row = 0; row < list->count(); ++row) {
        const auto* item = list->item(row);
        if (item && item->flags().testFlag(Qt::ItemIsSelectable) &&
            item->text().contains(QLatin1Char('\n'))) {
            return row;
        }
    }
    return -1;
}

}  // namespace

class FileManagementExplorerPanelTests : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void initTestCase() {
        QCoreApplication::setOrganizationName(QStringLiteral("SAKUtilityTests"));
        QCoreApplication::setApplicationName(QStringLiteral("FileExplorerPanelTests"));
    }

    void init() {
        resetExplorerPanelSettings();
    }

    void shellCreatesFilesLikeRegions() {
        sak::FileManagementExplorerPanel panel;
        panel.resize(1280, 760);
        panel.show();
        QVERIFY(QTest::qWaitForWindowExposed(&panel));

        auto* targetList = child<QListWidget>(&panel, "fileExplorerTargetList");
        auto* pathEdit = child<QLineEdit>(&panel, "fileExplorerPathEdit");
        auto* table = child<QTableView>(&panel, "fileExplorerTable");
        auto* list = child<QListView>(&panel, "fileExplorerListView");
        auto* grid = child<QListView>(&panel, "fileExplorerGridView");
        auto* cards = child<QListView>(&panel, "fileExplorerCardsView");
        auto* columns = child<QListView>(&panel, "fileExplorerColumnsView");
        auto* columnsPreview = child<QListView>(&panel, "fileExplorerColumnsPreviewView");
        auto* details = child<QTabWidget>(&panel, "fileExplorerDetailsTabs");
        auto* sidebarToggle = child<QPushButton>(&panel, "fileExplorerSidebarToggleButton");
        auto* detailsToggle = child<QPushButton>(&panel, "fileExplorerDetailsToggleButton");
        auto* search = child<QPushButton>(&panel, "fileExplorerSearchButton");
        auto* command = child<QPushButton>(&panel, "fileExplorerCommandButton");
        auto* open = child<QPushButton>(&panel, "fileExplorerOpenButton");
        auto* copyPath = child<QPushButton>(&panel, "fileExplorerCopyPathButton");
        auto* refresh = child<QPushButton>(&panel, "fileExplorerRefreshButton");
        auto* newFolder = child<QPushButton>(&panel, "fileExplorerNewFolderButton");
        auto* writeFile = child<QPushButton>(&panel, "fileExplorerWriteFileButton");
        auto* rename = child<QPushButton>(&panel, "fileExplorerRenameButton");
        auto* deleteButton = child<QPushButton>(&panel, "fileExplorerDeleteButton");
        auto* view = child<QToolButton>(&panel, "fileExplorerViewButton");
        auto* summary = child<QLabel>(&panel, "fileExplorerSummaryLabel");
        auto* status = child<QLabel>(&panel, "fileExplorerStatusLabel");

        QVERIFY(targetList);
        QVERIFY(pathEdit);
        QVERIFY(table);
        QVERIFY(list);
        QVERIFY(grid);
        QVERIFY(cards);
        QVERIFY(columns);
        QVERIFY(columnsPreview);
        QVERIFY(details);
        QVERIFY(sidebarToggle);
        QVERIFY(detailsToggle);
        QVERIFY(search);
        QVERIFY(command);
        QVERIFY(open);
        QVERIFY(copyPath);
        QVERIFY(refresh);
        QVERIFY(newFolder);
        QVERIFY(writeFile);
        QVERIFY(rename);
        QVERIFY(deleteButton);
        QVERIFY(view);
        QVERIFY(summary);
        QVERIFY(status);
        QCOMPARE(details->count(), 4);
        QCOMPARE(table->selectionMode(), QAbstractItemView::ExtendedSelection);
        QVERIFY(child<QPlainTextEdit>(&panel, "fileExplorerPreviewText"));
        QVERIFY(child<QPlainTextEdit>(&panel, "fileExplorerPropertiesText"));
        QVERIFY(child<QPlainTextEdit>(&panel, "fileExplorerSafetyText"));
        QVERIFY(child<QPlainTextEdit>(&panel, "fileExplorerEvidenceText"));
        QVERIFY(!summary->accessibleName().isEmpty());
        QVERIFY(!sidebarToggle->accessibleName().isEmpty());
        QVERIFY(!detailsToggle->toolTip().isEmpty());
        QVERIFY(!search->toolTip().isEmpty());
        QVERIFY(!command->toolTip().isEmpty());
        QVERIFY(!view->toolTip().isEmpty());
        QVERIFY(!sidebarToggle->icon().isNull());
        QVERIFY(!detailsToggle->icon().isNull());
        QVERIFY(!command->icon().isNull());
        QVERIFY(!open->icon().isNull());
        QVERIFY(!copyPath->icon().isNull());
        QVERIFY(!refresh->icon().isNull());
        QVERIFY(!newFolder->icon().isNull());
        QVERIFY(!writeFile->icon().isNull());
        QVERIFY(!rename->icon().isNull());
        QVERIFY(!deleteButton->icon().isNull());
        QVERIFY(!view->icon().isNull());
        captureBaseline(&panel, QStringLiteral("desktop"));
    }

    void filesCommunityIconRegistryMapsBundledAssets() {
        const QVector<sak::FileExplorerCommandId> mappedCommands{
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

        for (const auto command : mappedCommands) {
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
        QVERIFY(sak::FileExplorerIconRegistry::descriptors().size() >= mappedCommands.size());
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
        auto* slider = view->menu()->findChild<QSlider*>(QStringLiteral("fileExplorerItemSizeSlider"));
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

        if (table->model() && table->model()->rowCount() > 0) {
            table->selectRow(0);
            QCOMPARE(pane->sharedSelectionModel()->selectedRows().size(), 1);
            listAction = actionStartingWith(view->menu(), QStringLiteral("List"));
            QVERIFY(listAction);
            QVERIFY(listAction->isEnabled());
        }

        listAction->trigger();
        QApplication::processEvents();
        QTRY_VERIFY(list->isVisible());
        QVERIFY(!table->isVisible());
        if (pane->sharedSelectionModel()->model() && pane->sharedSelectionModel()->model()->rowCount() > 0) {
            QCOMPARE(pane->sharedSelectionModel()->selectedRows().size(), 1);
        }

        actionStartingWith(view->menu(), QStringLiteral("Grid"))->trigger();
        QApplication::processEvents();
        QTRY_VERIFY(grid->isVisible());

        actionStartingWith(view->menu(), QStringLiteral("Cards"))->trigger();
        QApplication::processEvents();
        QTRY_VERIFY(cards->isVisible());

        actionStartingWith(view->menu(), QStringLiteral("Columns"))->trigger();
        QApplication::processEvents();
        QTRY_VERIFY(columns->isVisible());
        QVERIFY(columnsPreview->isVisible());

        actionStartingWith(view->menu(), QStringLiteral("Adaptive"))->trigger();
        QApplication::processEvents();
        QTRY_VERIFY(grid->isVisible());
        QCOMPARE(pane->viewMode(), sak::FileExplorerViewMode::Adaptive);

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

        auto* slider = view->menu()->findChild<QSlider*>(QStringLiteral("fileExplorerItemSizeSlider"));
        QVERIFY(slider);
        slider->setValue(96);
        QApplication::processEvents();
        QCOMPARE(pane->itemSizePx(), 96);

        auto* extensions = actionStartingWith(view->menu(), QStringLiteral("File Extensions"));
        QVERIFY(extensions);
        QVERIFY(extensions->isChecked());
        extensions->trigger();
        QApplication::processEvents();
        QCOMPARE(pane->showFileExtensions(), false);

        sak::FileManagementExplorerPanel restored;
        restored.resize(900, 600);
        restored.show();
        QVERIFY(QTest::qWaitForWindowExposed(&restored));
        auto* restoredPane = restored.findChild<sak::FileExplorerPane*>();
        QVERIFY(restoredPane);
        QCOMPARE(restoredPane->viewMode(), sak::FileExplorerViewMode::Adaptive);
        QCOMPARE(restoredPane->itemSizePx(), 96);
        QCOMPARE(restoredPane->showFileExtensions(), false);
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

    void commandButtonsExposeBlockersWithoutSelection() {
        sak::FileManagementExplorerPanel panel;
        panel.resize(1100, 700);
        panel.show();
        QVERIFY(QTest::qWaitForWindowExposed(&panel));

        auto* table = child<QTableView>(&panel, "fileExplorerTable");
        QVERIFY(table);
        table->clearSelection();
        QApplication::processEvents();

        auto* open = child<QPushButton>(&panel, "fileExplorerOpenButton");
        auto* rename = child<QPushButton>(&panel, "fileExplorerRenameButton");
        auto* deleteButton = child<QPushButton>(&panel, "fileExplorerDeleteButton");
        QVERIFY(open);
        QVERIFY(rename);
        QVERIFY(deleteButton);
        QVERIFY(!open->isEnabled());
        QVERIFY(open->toolTip().contains(QStringLiteral("Select an item first")));
        QVERIFY(!rename->isEnabled());
        QVERIFY(!rename->toolTip().isEmpty());
        QVERIFY(!deleteButton->isEnabled());
        QVERIFY(!deleteButton->toolTip().isEmpty());
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
        QVERIFY(safety->toPlainText().contains(QStringLiteral("Write state:")));
    }

    void contextMenusExposeRegistryActionsAndTargetActions() {
        sak::FileManagementExplorerPanel panel;
        panel.resize(1100, 700);
        panel.show();
        QVERIFY(QTest::qWaitForWindowExposed(&panel));

        auto* table = child<QTableView>(&panel, "fileExplorerTable");
        auto* targetList = child<QListWidget>(&panel, "fileExplorerTargetList");
        QVERIFY(table);
        QVERIFY(targetList);

        const QStringList tableActions = collectContextMenuTexts(table->viewport());
        QVERIFY(containsTextStartingWith(tableActions, QStringLiteral("Open")));
        QVERIFY(containsTextStartingWith(tableActions, QStringLiteral("Open in New Tab")));
        QVERIFY(containsTextStartingWith(tableActions, QStringLiteral("New Folder")));
        QVERIFY(containsTextStartingWith(tableActions, QStringLiteral("Delete")));

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

    void responsiveLayoutCollapsesAtNarrowWidth() {
        sak::FileManagementExplorerPanel panel;
        panel.resize(680, 640);
        panel.show();
        QVERIFY(QTest::qWaitForWindowExposed(&panel));

        auto* targetList = child<QListWidget>(&panel, "fileExplorerTargetList");
        auto* details = child<QTabWidget>(&panel, "fileExplorerDetailsTabs");
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
            QCOMPARE(view.model()->columnCount(), static_cast<int>(sak::FileExplorerItemModel::ColumnCount));
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
            const QModelIndex index = table->model()->index(row, sak::FileExplorerItemModel::NameColumn);
            if (index.data(sak::FileExplorerItemModel::EntryDirectoryRole).toBool()) {
                directoryRow = row;
                break;
            }
        }
        if (directoryRow < 0) {
            QSKIP("No directory row available on this host target.");
        }

        const QString beforePath = pathEdit->text();
        const QModelIndex index = table->model()->index(directoryRow, sak::FileExplorerItemModel::NameColumn);
        table->selectRow(directoryRow);
        QVERIFY(QMetaObject::invokeMethod(&panel,
                                          "onItemDoubleClicked",
                                          Qt::DirectConnection,
                                          Q_ARG(QModelIndex, index)));
        QTRY_VERIFY(pathEdit->text() != beforePath);
    }

    void shortcutsRefreshAndToggleDetailsWithoutLosingPath() {
        sak::FileManagementExplorerPanel panel;
        panel.resize(1100, 700);
        panel.show();
        QVERIFY(QTest::qWaitForWindowExposed(&panel));

        auto* targetList = child<QListWidget>(&panel, "fileExplorerTargetList");
        auto* pathEdit = child<QLineEdit>(&panel, "fileExplorerPathEdit");
        auto* details = child<QTabWidget>(&panel, "fileExplorerDetailsTabs");
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

    void searchShortcutAppliesCurrentFolderFilter() {
        sak::FileManagementExplorerPanel panel;
        panel.resize(1100, 700);
        panel.show();
        QVERIFY(QTest::qWaitForWindowExposed(&panel));

        auto* pane = panel.findChild<sak::FileExplorerPane*>();
        QVERIFY(pane);
        QVERIFY(pane->sortFilterModel());

        bool dialogSeen = false;
        QTimer::singleShot(0, [&dialogSeen]() {
            auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
            if (!dialog) {
                return;
            }
            auto* edit = dialog->findChild<QLineEdit*>();
            if (!edit) {
                return;
            }
            dialogSeen = true;
            edit->setText(QStringLiteral("codex-filter-no-match"));
            dialog->accept();
        });

        panel.setFocus();
        QTest::keyClick(&panel, Qt::Key_F, Qt::ControlModifier);
        QVERIFY(dialogSeen);
        QCOMPARE(pane->sortFilterModel()->nameFilter(), QStringLiteral("codex-filter-no-match"));
    }

    void commandPaletteShortcutOpensRegistryBackedDialog() {
        sak::FileManagementExplorerPanel panel;
        panel.resize(1100, 700);
        panel.show();
        QVERIFY(QTest::qWaitForWindowExposed(&panel));

        bool paletteSeen = false;
        bool listSeen = false;
        QTimer::singleShot(0, [&paletteSeen, &listSeen]() {
            auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
            if (!dialog) {
                return;
            }
            paletteSeen = dialog->windowTitle() == QStringLiteral("Command Palette");
            listSeen = dialog->findChild<QListWidget*>(
                           QStringLiteral("fileExplorerCommandPaletteList")) != nullptr;
            dialog->reject();
        });

        panel.setFocus();
        QTest::keyClick(&panel, Qt::Key_P, Qt::ControlModifier | Qt::ShiftModifier);
        QVERIFY(paletteSeen);
        QVERIFY(listSeen);
    }
};

QTEST_MAIN(FileManagementExplorerPanelTests)
#include "test_file_management_explorer_panel.moc"
