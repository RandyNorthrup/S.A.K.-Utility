// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file test_file_management_explorer_panel.cpp
/// @brief GUI tests for the File Management Explorer shell.

#include "sak/file_explorer_details_view.h"
#include "sak/file_explorer_icon_registry.h"
#include "sak/file_explorer_item_model.h"
#include "sak/file_explorer_pane.h"
#include "sak/file_explorer_sort_filter_model.h"
#include "sak/file_management_explorer_panel.h"

#include <QAbstractItemView>
#include <QAction>
#include <QApplication>
#include <QClipboard>
#include <QComboBox>
#include <QContextMenuEvent>
#include <QCoreApplication>
#include <QDialog>
#include <QDir>
#include <QHeaderView>
#include <QImage>
#include <QLabel>
#include <QLineEdit>
#include <QListView>
#include <QListWidget>
#include <QMenu>
#include <QMimeData>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSettings>
#include <QSlider>
#include <QSplitter>
#include <QStackedWidget>
#include <QTabBar>
#include <QTableView>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QTimer>
#include <QToolButton>
#include <QtTest/QtTest>
#include <QUrl>

#include <algorithm>
#include <tuple>

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
    const QString path = dir.filePath(
        QStringLiteral("artifacts/file-management-explorer-baseline/%1.png").arg(name));
    QVERIFY2(widget->grab().save(path), qPrintable(path));
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

void verifyShellCoreWidgetsExist(sak::FileManagementExplorerPanel& panel) {
    QVERIFY(child<QListWidget>(&panel, "fileExplorerTargetList"));
    QVERIFY(child<QLineEdit>(&panel, "fileExplorerPathEdit"));
    QVERIFY(child<QTableView>(&panel, "fileExplorerTable"));
    QVERIFY(child<QListView>(&panel, "fileExplorerListView"));
    QVERIFY(child<QListView>(&panel, "fileExplorerGridView"));
    QVERIFY(child<QListView>(&panel, "fileExplorerCardsView"));
    QVERIFY(child<QListView>(&panel, "fileExplorerColumnsView"));
    QVERIFY(child<QListView>(&panel, "fileExplorerColumnsPreviewView"));
    QVERIFY(child<QTabWidget>(&panel, "fileExplorerDetailsTabs"));
    QVERIFY(child<QPushButton>(&panel, "fileExplorerSidebarToggleButton"));
    QVERIFY(child<QPushButton>(&panel, "fileExplorerDetailsToggleButton"));
    QVERIFY(child<QPushButton>(&panel, "fileExplorerSearchButton"));
    QVERIFY(child<QPushButton>(&panel, "fileExplorerCommandButton"));
    QVERIFY(child<QPushButton>(&panel, "fileExplorerOpenButton"));
    QVERIFY(child<QPushButton>(&panel, "fileExplorerCopyPathButton"));
    QVERIFY(child<QPushButton>(&panel, "fileExplorerRefreshButton"));
    QVERIFY(child<QPushButton>(&panel, "fileExplorerNewFolderButton"));
    QVERIFY(child<QPushButton>(&panel, "fileExplorerWriteFileButton"));
    QVERIFY(child<QPushButton>(&panel, "fileExplorerRenameButton"));
    QVERIFY(child<QPushButton>(&panel, "fileExplorerDeleteButton"));
    QVERIFY(child<QToolButton>(&panel, "fileExplorerViewButton"));
    QVERIFY(child<QLabel>(&panel, "fileExplorerSummaryLabel"));
    QVERIFY(child<QLabel>(&panel, "fileExplorerStatusLabel"));
}

void verifyShellDetailsAndPreviewPanes(sak::FileManagementExplorerPanel& panel) {
    auto* details = child<QTabWidget>(&panel, "fileExplorerDetailsTabs");
    auto* table = child<QTableView>(&panel, "fileExplorerTable");
    QCOMPARE(details->count(), 4);
    QCOMPARE(table->selectionMode(), QAbstractItemView::ExtendedSelection);
    auto* preview = child<QPlainTextEdit>(&panel, "fileExplorerPreviewText");
    QVERIFY(preview);
    // The persistent preview pane is always populated by the auto-preview wiring (a hint when
    // no single readable file is selected), never left blank for the user to wonder about.
    QVERIFY2(!preview->toPlainText().trimmed().isEmpty(), qPrintable(preview->toPlainText()));
    QVERIFY(child<QPlainTextEdit>(&panel, "fileExplorerPropertiesText"));
    QVERIFY(child<QPlainTextEdit>(&panel, "fileExplorerSafetyText"));
    QVERIFY(child<QPlainTextEdit>(&panel, "fileExplorerEvidenceText"));
}

struct ViewModeWidgets {
    QTableView* table;
    QListView* list;
    QListView* grid;
    QListView* cards;
    QListView* columns;
    QListView* columnsPreview;
};

void switchThroughViewModesAndVerifyVisibility(sak::FileExplorerPane* pane,
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
    QCOMPARE(restoredPane->itemSizePx(), 96);
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
    auto* open = child<QPushButton>(&panel, "fileExplorerOpenButton");
    auto* copyPath = child<QPushButton>(&panel, "fileExplorerCopyPathButton");
    auto* refresh = child<QPushButton>(&panel, "fileExplorerRefreshButton");
    auto* newFolder = child<QPushButton>(&panel, "fileExplorerNewFolderButton");
    auto* writeFile = child<QPushButton>(&panel, "fileExplorerWriteFileButton");
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
    QVERIFY(!open->icon().isNull());
    QVERIFY(!copyPath->icon().isNull());
    QVERIFY(!refresh->icon().isNull());
    QVERIFY(!newFolder->icon().isNull());
    QVERIFY(!writeFile->icon().isNull());
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
    }

    void init() { resetExplorerPanelSettings(); }

    void detailsPanePreviewSwitchesBetweenTextAndImage() {
        sak::FileExplorerDetailsPane pane;
        QVERIFY(pane.previewText());
        QVERIFY(pane.previewImage());
        QVERIFY(pane.previewCaption());
        auto* stack = pane.findChild<QStackedWidget*>(QStringLiteral("fileExplorerPreviewStack"));
        QVERIFY(stack);
        // The Preview tab hosts a text/hex view (index 0) and an image view (index 1).
        pane.showImagePreview(true);
        QCOMPARE(stack->currentIndex(), 1);
        pane.showImagePreview(false);
        QCOMPARE(stack->currentIndex(), 0);
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
        captureBaseline(&panel, QStringLiteral("desktop"));
    }

    void filesCommunityIconRegistryMapsBundledAssets() {
        const QVector<sak::FileExplorerCommandId> mappedCommands = bundledIconMappedCommands();

        for (const auto command : mappedCommands) {
            verifyBundledIconForCommand(command);
        }

        verifyNamedIconDescriptors();
        verifyAllDescriptorIconsRenderVisiblePixels();
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

        auto* slider =
            view->menu()->findChild<QSlider*>(QStringLiteral("fileExplorerItemSizeSlider"));
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

        verifyRestoredPaneMatchesPersistedSettings();
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
        const QPoint center = targetList->viewport()->rect().center();
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
        QVERIFY2(QTest::qWaitFor([&copied]() { return copied.exists(); }, 5000),
                 "paste did not create the destination file");
        QVERIFY(copied.open(QIODevice::ReadOnly));
        QCOMPARE(copied.readAll(), payload);
    }

    void omnibarSearchDialogExposesResultRoutingAndHistory() {
        // The rich omnibar search dialog opens from the search button with a target badge,
        // an editable query pre-seeded from history, a result list, and open/clear actions.
        QSettings settings;
        settings.beginGroup(QStringLiteral("FileManagementExplorer"));
        const QStringList previous = settings.value(QStringLiteral("SearchHistory")).toStringList();
        settings.setValue(QStringLiteral("SearchHistory"),
                          QStringList{QStringLiteral("prior-query")});
        settings.endGroup();
        settings.sync();

        sak::FileManagementExplorerPanel panel;
        panel.resize(1100, 700);
        panel.show();
        QVERIFY(QTest::qWaitForWindowExposed(&panel));

        auto* targetList = child<QListWidget>(&panel, "fileExplorerTargetList");
        auto* searchButton = child<QPushButton>(&panel, "fileExplorerSearchButton");
        QVERIFY(targetList);
        QVERIFY(searchButton);
        const int targetRow = firstTargetRow(targetList);
        if (targetRow < 0) {
            settings.beginGroup(QStringLiteral("FileManagementExplorer"));
            settings.setValue(QStringLiteral("SearchHistory"), previous);
            settings.endGroup();
            QSKIP("No mounted File Explorer targets on this test host.");
        }
        targetList->setCurrentRow(targetRow);
        QApplication::processEvents();

        bool verified = false;
        QTimer::singleShot(0, [&verified]() {
            auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
            if (!dialog) {
                return;
            }
            auto* badge =
                dialog->findChild<QLabel*>(QStringLiteral("fileExplorerSearchTargetBadge"));
            auto* query = dialog->findChild<QComboBox*>(QStringLiteral("fileExplorerSearchQuery"));
            auto* results =
                dialog->findChild<QListWidget*>(QStringLiteral("fileExplorerSearchResults"));
            auto* clear =
                dialog->findChild<QPushButton*>(QStringLiteral("fileExplorerSearchClearButton"));
            auto* open =
                dialog->findChild<QPushButton*>(QStringLiteral("fileExplorerSearchOpenButton"));
            auto* openLocation = dialog->findChild<QPushButton*>(
                QStringLiteral("fileExplorerSearchOpenLocationButton"));
            verified = badge && query && results && clear && open && openLocation &&
                       !badge->text().trimmed().isEmpty() &&
                       query->findText(QStringLiteral("prior-query")) >= 0;
            dialog->reject();
        });
        searchButton->click();
        QApplication::processEvents();

        settings.beginGroup(QStringLiteral("FileManagementExplorer"));
        settings.setValue(QStringLiteral("SearchHistory"), previous);
        settings.endGroup();
        settings.sync();

        QVERIFY2(verified, "omnibar search dialog is missing expected controls or history seed");
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

    void commandPaletteFilterNarrowsCommandList() {
        sak::FileManagementExplorerPanel panel;
        panel.resize(1100, 700);
        panel.show();
        QVERIFY(QTest::qWaitForWindowExposed(&panel));

        int fullCount = -1;
        int filteredCount = -1;
        QTimer::singleShot(0, [&fullCount, &filteredCount]() {
            auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
            if (!dialog) {
                return;
            }
            auto* list =
                dialog->findChild<QListWidget*>(QStringLiteral("fileExplorerCommandPaletteList"));
            auto* filter =
                dialog->findChild<QLineEdit*>(QStringLiteral("fileExplorerCommandPaletteFilter"));
            if (!list || !filter) {
                dialog->reject();
                return;
            }
            fullCount = list->count();
            filter->setText(QStringLiteral("Delete"));
            QApplication::processEvents();
            filteredCount = list->count();
            dialog->reject();
        });

        panel.setFocus();
        QTest::keyClick(&panel, Qt::Key_P, Qt::ControlModifier | Qt::ShiftModifier);
        QVERIFY(fullCount > 0);
        QVERIFY(filteredCount > 0);
        QVERIFY(filteredCount < fullCount);
    }

    void commandPaletteRendersGroupHeaders() {
        sak::FileManagementExplorerPanel panel;
        panel.resize(1100, 700);
        panel.show();
        QVERIFY(QTest::qWaitForWindowExposed(&panel));

        // The palette groups commands under non-selectable section headers whose
        // labels come from the registry group names, and a header always precedes
        // its commands.
        bool sawNavigationHeader = false;
        bool headerPrecedesCommand = false;
        QTimer::singleShot(0, [&sawNavigationHeader, &headerPrecedesCommand]() {
            auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
            if (!dialog) {
                return;
            }
            auto* list =
                dialog->findChild<QListWidget*>(QStringLiteral("fileExplorerCommandPaletteList"));
            if (!list) {
                dialog->reject();
                return;
            }
            bool headerSeen = false;
            for (int i = 0; i < list->count(); ++i) {
                const QListWidgetItem* item = list->item(i);
                const bool isHeader = (item->flags() == Qt::NoItemFlags);
                if (isHeader) {
                    headerSeen = true;
                    if (item->text() == QStringLiteral("Navigation")) {
                        sawNavigationHeader = true;
                    }
                } else if (headerSeen) {
                    headerPrecedesCommand = true;
                }
            }
            dialog->reject();
        });

        panel.setFocus();
        QTest::keyClick(&panel, Qt::Key_P, Qt::ControlModifier | Qt::ShiftModifier);
        QVERIFY(sawNavigationHeader);
        QVERIFY(headerPrecedesCommand);
    }

    void commandPaletteMarksDisabledCommandWithBlocker() {
        sak::FileManagementExplorerPanel panel;
        panel.resize(1100, 700);
        panel.show();
        QVERIFY(QTest::qWaitForWindowExposed(&panel));

        // With no target selected, write/selection commands are disabled and must
        // carry their blocker text inline and in the tooltip, never silently.
        bool foundDisabledWithBlocker = false;
        QTimer::singleShot(0, [&foundDisabledWithBlocker]() {
            auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
            if (!dialog) {
                return;
            }
            auto* list =
                dialog->findChild<QListWidget*>(QStringLiteral("fileExplorerCommandPaletteList"));
            if (!list) {
                dialog->reject();
                return;
            }
            for (int i = 0; i < list->count(); ++i) {
                const QListWidgetItem* item = list->item(i);
                const bool enabled = (item->flags() & Qt::ItemIsEnabled) != 0;
                if (!enabled && item->text().contains(QStringLiteral(" - ")) &&
                    !item->toolTip().isEmpty()) {
                    foundDisabledWithBlocker = true;
                    break;
                }
            }
            dialog->reject();
        });

        panel.setFocus();
        QTest::keyClick(&panel, Qt::Key_P, Qt::ControlModifier | Qt::ShiftModifier);
        QVERIFY(foundDisabledWithBlocker);
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

        // Run "Reopen Closed Tab" through the command palette (it now resolves to
        // enabled from the live context) to restore the closed tab.
        QTimer::singleShot(0, []() {
            auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
            if (!dialog) {
                return;
            }
            auto* list =
                dialog->findChild<QListWidget*>(QStringLiteral("fileExplorerCommandPaletteList"));
            auto* filter =
                dialog->findChild<QLineEdit*>(QStringLiteral("fileExplorerCommandPaletteFilter"));
            if (!list || !filter) {
                dialog->reject();
                return;
            }
            filter->setText(QStringLiteral("Reopen Closed Tab"));
            QApplication::processEvents();
            for (int i = 0; i < list->count(); ++i) {
                QListWidgetItem* item = list->item(i);
                if (item->text().startsWith(QStringLiteral("Reopen Closed Tab")) &&
                    (item->flags() & Qt::ItemIsEnabled) != 0) {
                    list->setCurrentItem(item);
                    dialog->accept();
                    return;
                }
            }
            dialog->reject();
        });

        panel.setFocus();
        QTest::keyClick(&panel, Qt::Key_P, Qt::ControlModifier | Qt::ShiftModifier);
        QApplication::processEvents();
        QCOMPARE(tabs->count(), 2);
    }
};

QTEST_MAIN(FileManagementExplorerPanelTests)
#include "test_file_management_explorer_panel.moc"
