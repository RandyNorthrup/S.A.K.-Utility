// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/partition_file_system_detector.h"
#include "sak/partition_file_system_tool_runner.h"
#include "sak/partition_manager_panel.h"
#include "sak/style_constants.h"

#include <QAbstractItemDelegate>
#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QContextMenuEvent>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFile>
#include <QFrame>
#include <QHash>
#include <QHeaderView>
#include <QIcon>
#include <QImage>
#include <QInputDialog>
#include <QJsonArray>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPainter>
#include <QPalette>
#include <QPushButton>
#include <QScopeGuard>
#include <QScrollArea>
#include <QSlider>
#include <QSpinBox>
#include <QSplitter>
#include <QStyleOptionViewItem>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QTemporaryDir>
#include <QTextEdit>
#include <QTimer>
#include <QToolButton>
#include <QtTest/QtTest>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <limits>

class PartitionManagerPanelTests : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void scanButtonIsStatefulAndInventorySummaryUsesStatusBar();
    void partitionTableUsesAomeiListChrome();
    void sidebarIsFixedAndHasNoRedundantPreviewBox();
    void sidebarActionsRenderAsCompactTextLinks();
    void sidebarActionsGateBySelectedTargetKind();
    void nonNativeFilesystemActionsExposeReadOnlyHfsCheck();
    void partitionOperationsScrollInsideGroup();
    void diskMapLegendContainsCommercialColorRoles();
    void diskMapLegendColorsMatchRenderedRoles();
    void unallocatedRoleUsesDarkGrayPalette();
    void ribbonButtonsUseIcons8SvgSources();
    void diskMapUsesCompactSpacing();
    void diskMapHighlightsOnlySelectedPartition();
    void diskMapHighlightsSelectedDiskRow();
    void diskMapContextMenuSelectsMatchingTargets();
    void contextMenuOmitsRibbonAndQueueControls();
    void diskMapSegmentsSelectMatchingTableRows();
    void redoButtonEnablesOnlyAfterUndo();
    void diskMapRendersTypeColorInsideNeutralShell();
    void bottomDiskMapCanResizeIntoTableSpace();
    void finalApplyReviewContainsLayoutDiff();
    void propertiesActionIsFirstClass();
    void propertiesDialogShowsRawFilesystemMetadata();
    void propertiesAndInspectShowRawFilesystemSanityNotes();
    void extFilesystemWriteActionsQueueWithConfirmation() const;
    void apfsContainerActionAllowsWritableApfsVolumes();
    void manageBitLockerShowsStatusDialog();
    void diskDefragShowsOptimizeDialog();
    void ssdSecureEraseShowsQueueDialog();
    void spaceAnalyzerExposesCommercialViews();
    void changeClusterSizeQueuesVerifiedReformatOperation();
    void allocateFreeSpaceQueuesAdjacentDonorOperation();
    void unallocatedAllocateFreeSpaceQueuesAdjacentEngines();
    void formerCommercialCompatibilityActionsQueueDirectEngines();
    void createDialogExposesSynchronizedHandleControls();
    void wipeActionLetsUserChooseScope();
    void quickPartitionSizesFailClosedOnMalformedCustomAndOverflow();
    void wizardEntryPointsRespectRunningOperationGuard() const;
    void inventoryStateIsHonestAboutFailedPartitionEnumeration();
};

namespace {

constexpr uint64_t kTestMegabyteBytes = 1024 * 1024;
constexpr uint64_t kCreateDialogFreeMegabytes = 512;
constexpr int kCreateDialogSizeMegabytes = 256;
constexpr int kCreateDialogBeforeMegabytes = 128;
constexpr int kOperationSizePreviewLabelWidth = 92;
constexpr int kOperationSizePreviewRowHeight = 24;
constexpr int kPreviewDragTargetEndMegabytes = 448;
constexpr int kPreviewDragExpectedSizeMegabytes = 300;
constexpr int kHorizontalMarginCount = 2;
constexpr int kRenderedSegmentFillLightness = 160;

struct CreateDialogInspection {
    bool inspected{false};
    bool has_size_handle{false};
    bool has_location_handle{false};
    bool windows_native_file_systems_only{false};
    bool size_synced{false};
    bool location_synced{false};
    bool preview_drag_synced{false};
    QString file_system_items;
};

template <typename Widget>
Widget* findAccessibleWidget(QDialog* dialog, const QString& accessibleName) {
    const auto widgets = dialog->findChildren<Widget*>();
    const auto it = std::find_if(widgets.cbegin(), widgets.cend(), [&](const Widget* widget) {
        return widget->accessibleName() == accessibleName;
    });
    return it == widgets.cend() ? nullptr : *it;
}

bool hasActionButton(const QList<QToolButton*>& buttons,
                     const QString& name,
                     const QString& tooltip,
                     bool requireEnabled = false) {
    return std::any_of(buttons.cbegin(), buttons.cend(), [&](const QToolButton* button) {
        return button->accessibleName() == name && button->toolTip() == tooltip &&
               (!requireEnabled || button->isEnabled());
    });
}

QToolButton* findToolButtonByName(QWidget* root, const QString& name) {
    const auto buttons = root->findChildren<QToolButton*>();
    const auto it = std::find_if(buttons.cbegin(), buttons.cend(), [&](const QToolButton* button) {
        return button->accessibleName() == name;
    });
    return it == buttons.cend() ? nullptr : *it;
}

QString propertyTableValue(QTableWidget* table, const QString& propertyName) {
    for (int row = 0; row < table->rowCount(); ++row) {
        const auto* name = table->item(row, 0);
        const auto* value = table->item(row, 1);
        if (name && value && name->text() == propertyName) {
            return value->text();
        }
    }
    return {};
}

void addUnallocatedTestSelection(QTableWidget* table) {
    table->setRowCount(1);
    QVariantMap rowData{{QStringLiteral("kind"), QStringLiteral("unallocated")},
                        {QStringLiteral("disk"), 1},
                        {QStringLiteral("offset"), QStringLiteral("0")},
                        {QStringLiteral("size"),
                         QString::number(kCreateDialogFreeMegabytes * kTestMegabyteBytes)}};
    auto* item = new QTableWidgetItem(QStringLiteral("Unallocated"));
    item->setData(Qt::UserRole, rowData);
    table->setItem(0, 0, item);
    table->selectRow(0);
}

void sendMouse(QWidget* target,
               QEvent::Type type,
               const QPoint& position,
               Qt::MouseButton button,
               Qt::MouseButtons buttons) {
    QMouseEvent event(
        type, position, target->mapToGlobal(position), button, buttons, Qt::NoModifier);
    QApplication::sendEvent(target, &event);
}

QPoint previewHandlePoint(const QWidget* preview, int endMegabytes) {
    const int trackLeft = sak::ui::kMarginSmall + kOperationSizePreviewLabelWidth;
    const int trackWidth = preview->width() - (sak::ui::kMarginSmall * kHorizontalMarginCount) -
                           kOperationSizePreviewLabelWidth;
    return {trackLeft +
                ((endMegabytes * trackWidth) / static_cast<int>(kCreateDialogFreeMegabytes)),
            sak::ui::kMarginSmall + (kOperationSizePreviewRowHeight / kHorizontalMarginCount)};
}

void dragPreviewHandle(QWidget* preview) {
    const auto startPoint =
        previewHandlePoint(preview, kCreateDialogBeforeMegabytes + kCreateDialogSizeMegabytes);
    const auto targetPoint = previewHandlePoint(preview, kPreviewDragTargetEndMegabytes);
    sendMouse(preview, QEvent::MouseButtonPress, startPoint, Qt::LeftButton, Qt::LeftButton);
    sendMouse(preview, QEvent::MouseMove, targetPoint, Qt::NoButton, Qt::LeftButton);
    sendMouse(preview, QEvent::MouseButtonRelease, targetPoint, Qt::LeftButton, Qt::NoButton);
}

void flushDeferredDeletes() {
    QApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    QApplication::processEvents();
}

void closeNextPopup() {
    QTimer::singleShot(0, []() {
        if (auto* popup = QApplication::activePopupWidget()) {
            popup->close();
        }
    });
}

void sendContextMenu(QWidget* target) {
    const QPoint point = target->rect().center();
    closeNextPopup();
    QContextMenuEvent event(QContextMenuEvent::Mouse, point, target->mapToGlobal(point));
    QApplication::sendEvent(target, &event);
    QApplication::processEvents();
    flushDeferredDeletes();
}

QStringList contextMenuActionTexts(QWidget* target) {
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
    flushDeferredDeletes();
    return texts;
}

QHash<QString, bool> contextMenuActionStates(QWidget* target) {
    QHash<QString, bool> states;
    QTimer::singleShot(0, [&states]() {
        auto* menu = qobject_cast<QMenu*>(QApplication::activePopupWidget());
        if (!menu) {
            return;
        }
        const auto actions = menu->actions();
        for (const auto* action : actions) {
            if (!action->isSeparator()) {
                states.insert(action->text(), action->isEnabled());
            }
        }
        menu->close();
    });
    const QPoint point = target->rect().center();
    QContextMenuEvent event(QContextMenuEvent::Mouse, point, target->mapToGlobal(point));
    QApplication::sendEvent(target, &event);
    QApplication::processEvents();
    flushDeferredDeletes();
    return states;
}

bool comboHasItem(const QComboBox* combo, const QString& text) {
    if (!combo) {
        return false;
    }
    for (int index = 0; index < combo->count(); ++index) {
        if (combo->itemText(index).compare(text, Qt::CaseInsensitive) == 0) {
            return true;
        }
    }
    return false;
}

QString comboItemsText(const QComboBox* combo) {
    QStringList items;
    for (int index = 0; combo && index < combo->count(); ++index) {
        items.append(combo->itemText(index));
    }
    return items.join('|');
}

QString comboInventoryText(QDialog* dialog) {
    QStringList comboDescriptions;
    const auto combos = dialog->findChildren<QComboBox*>();
    for (const auto* combo : combos) {
        comboDescriptions.append(
            QStringLiteral("%1=[%2]").arg(combo->accessibleName(), comboItemsText(combo)));
    }
    return comboDescriptions.join(QStringLiteral("; "));
}

QComboBox* findCreateFileSystemCombo(QDialog* dialog) {
    const auto combos = dialog->findChildren<QComboBox*>();
    for (auto* combo : combos) {
        if (comboHasItem(combo, QStringLiteral("NTFS")) &&
            comboHasItem(combo, QStringLiteral("FAT32")) &&
            comboHasItem(combo, QStringLiteral("exFAT"))) {
            return combo;
        }
    }
    return nullptr;
}

void inspectCreateFileSystems(QDialog* dialog, CreateDialogInspection* result) {
    const auto* fileSystem = findCreateFileSystemCombo(dialog);
    result->file_system_items = fileSystem ? comboItemsText(fileSystem)
                                           : comboInventoryText(dialog);
    // Create formats Windows-native file systems only; non-native formatting moved to Format.
    result->windows_native_file_systems_only =
        fileSystem && comboHasItem(fileSystem, QStringLiteral("NTFS")) &&
        comboHasItem(fileSystem, QStringLiteral("exFAT")) &&
        comboHasItem(fileSystem, QStringLiteral("FAT32")) &&
        !comboHasItem(fileSystem, QStringLiteral("ext4")) &&
        !comboHasItem(fileSystem, QStringLiteral("HFSX")) &&
        !comboHasItem(fileSystem, QStringLiteral("APFS")) &&
        !comboHasItem(fileSystem, QStringLiteral("Linux swap"));
}

void inspectCreateHandleControls(QDialog* dialog, CreateDialogInspection* result) {
    auto* sizeHandle = findAccessibleWidget<QSlider>(dialog,
                                                     QStringLiteral("Partition size handle"));
    auto* locationHandle =
        findAccessibleWidget<QSlider>(dialog, QStringLiteral("Free space before handle"));
    auto* sizeSpin = findAccessibleWidget<QSpinBox>(dialog, QStringLiteral("Partition size"));
    auto* locationSpin =
        findAccessibleWidget<QSpinBox>(dialog, QStringLiteral("Free space before new partition"));
    auto* sizePreview =
        dialog->findChild<QWidget*>(QStringLiteral("partitionOperationSizePreview"));
    result->has_size_handle = sizeHandle != nullptr;
    result->has_location_handle = locationHandle != nullptr;
    if (!sizeHandle || !locationHandle || !sizeSpin || !locationSpin || !sizePreview) {
        return;
    }

    sizeHandle->setValue(kCreateDialogSizeMegabytes);
    locationHandle->setValue(kCreateDialogBeforeMegabytes);
    result->size_synced = sizeSpin->value() == kCreateDialogSizeMegabytes;
    result->location_synced = locationSpin->value() == kCreateDialogBeforeMegabytes;
    QApplication::processEvents();
    dragPreviewHandle(sizePreview);
    result->preview_drag_synced = sizeSpin->value() >= kPreviewDragExpectedSizeMegabytes &&
                                  locationSpin->value() == kCreateDialogBeforeMegabytes;
}

void inspectCreateDialog(QDialog* dialog, CreateDialogInspection* result) {
    inspectCreateFileSystems(dialog, result);
    inspectCreateHandleControls(dialog, result);
    result->inspected = true;
}

QColor averageColor(const QImage& image, const QRect& rect) {
    uint64_t red = 0;
    uint64_t green = 0;
    uint64_t blue = 0;
    uint64_t count = 0;
    const QRect bounds = rect.intersected(image.rect());
    for (int y = bounds.top(); y <= bounds.bottom(); ++y) {
        for (int x = bounds.left(); x <= bounds.right(); ++x) {
            const QColor color = image.pixelColor(x, y);
            red += static_cast<uint64_t>(color.red());
            green += static_cast<uint64_t>(color.green());
            blue += static_cast<uint64_t>(color.blue());
            ++count;
        }
    }
    if (count == 0) {
        return {};
    }
    return {static_cast<int>(red / count),
            static_cast<int>(green / count),
            static_cast<int>(blue / count)};
}

// True when the widget's own paintEvent leaves the extreme (0,0) corner unpainted while filling
// its interior -- i.e. it actually paints a rounded fill. Forces the fill role(s) (Base, and
// AlternateBase for a gradient tile) to a distinct marker and selected=false, renders only this
// widget's paintEvent (empty flags: no child overpaint, no auto background fill) onto a
// contrasting ground, and samples centre vs corner. Non-vacuous: a square fill would tint the
// corner the fill marker and fail cornerClipped, while the centre pixel proves the body is filled.
bool rendersRoundedCorners(QWidget* widget) {
    const QColor fillMarker(255, 0, 0);
    const QColor groundMarker(0, 0, 255);
    QPalette pal = widget->palette();
    pal.setColor(QPalette::Base, fillMarker);
    pal.setColor(QPalette::AlternateBase, fillMarker);
    widget->setPalette(pal);
    widget->setProperty("selected", false);
    widget->resize(60, 40);
    const QSize sz = widget->size();
    QImage image(sz, QImage::Format_ARGB32);
    image.fill(groundMarker);
    widget->render(&image, QPoint(), QRegion(), QWidget::RenderFlags());
    const bool interiorFilled = image.pixelColor(sz.width() / 2, sz.height() / 2) == fillMarker;
    const bool cornerClipped = image.pixelColor(0, 0) == groundMarker;
    return interiorFilled && cornerClipped;
}

// True when the widget paints NOTHING (no fill and no border) along its right edge -- i.e. it
// renders as a flat text link, not a filled/bordered button. The widget is made wide so the
// right-edge strip is clear of the left-aligned icon/text, then only its own paintEvent is
// rendered (empty flags: no auto background fill) over a ground marker and the strip is sampled.
// A filled or bordered button paints that strip a non-ground colour and fails -- which is exactly
// the "should not render as filled buttons" counterfactual this certifies against.
bool paintsFlatRightEdge(QWidget* widget) {
    const QColor ground(0, 0, 255);
    widget->resize(200, 22);
    const QSize sz = widget->size();
    QImage image(sz, QImage::Format_ARGB32);
    image.fill(ground);
    widget->render(&image, QPoint(), QRegion(), QWidget::RenderFlags());
    const int x = sz.width() - 1;
    return image.pixelColor(x, 1) == ground && image.pixelColor(x, sz.height() / 2) == ground &&
           image.pixelColor(x, sz.height() - 2) == ground;
}

int chroma(const QColor& color) {
    const int high = std::max({color.red(), color.green(), color.blue()});
    const int low = std::min({color.red(), color.green(), color.blue()});
    return high - low;
}

int colorDistance(const QColor& lhs, const QColor& rhs) {
    return std::abs(lhs.red() - rhs.red()) + std::abs(lhs.green() - rhs.green()) +
           std::abs(lhs.blue() - rhs.blue());
}

sak::PartitionInventory applyReviewInventoryFixture() {
    sak::PartitionVolumeInfo volume;
    volume.drive_letter = QStringLiteral("T");
    volume.file_system = QStringLiteral("NTFS");
    volume.total_bytes = 128 * kTestMegabyteBytes;
    volume.free_bytes = 64 * kTestMegabyteBytes;

    sak::PartitionInfoEx partition;
    partition.disk_number = sak::DiskNumber{0};
    partition.partition_number = sak::PartitionNumber{1};
    partition.type_name = QStringLiteral("Basic");
    partition.size_bytes = volume.total_bytes;
    partition.volume = volume;

    sak::PartitionDiskInfo disk;
    disk.disk_number = sak::DiskNumber{0};
    disk.partition_style = QStringLiteral("GPT");
    disk.health_status = QStringLiteral("Healthy");
    disk.operational_status = QStringLiteral("Online");
    disk.size_bytes = volume.total_bytes;
    disk.partitions.append(partition);

    sak::PartitionInventory inventory;
    inventory.layout_hash = QStringLiteral("panel-apply-review-layout");
    inventory.disks.append(disk);
    return inventory;
}

sak::PartitionInventory allocateFreeSpaceInventoryFixture() {
    auto inventory = applyReviewInventoryFixture();
    auto& disk = inventory.disks[0];
    auto& target = disk.partitions[0];
    target.offset_bytes = kTestMegabyteBytes;
    disk.size_bytes = 512 * kTestMegabyteBytes;

    sak::PartitionVolumeInfo donorVolume;
    donorVolume.drive_letter = QStringLiteral("D");
    donorVolume.file_system = QStringLiteral("NTFS");
    donorVolume.label = QStringLiteral("Donor");
    donorVolume.total_bytes = 256 * kTestMegabyteBytes;
    donorVolume.free_bytes = 192 * kTestMegabyteBytes;
    donorVolume.health_status = QStringLiteral("Healthy");

    sak::PartitionInfoEx donor;
    donor.disk_number = disk.disk_number;
    donor.partition_number = sak::PartitionNumber{2};
    donor.type_name = QStringLiteral("Basic");
    donor.offset_bytes = target.offset_bytes + target.size_bytes;
    donor.size_bytes = donorVolume.total_bytes;
    donor.volume = donorVolume;
    disk.partitions.append(donor);
    return inventory;
}

sak::PartitionInventory metadataRebuildInventoryFixture(bool dynamicDisk = false) {
    sak::PartitionVolumeInfo volume;
    volume.drive_letter = QStringLiteral("T");
    volume.file_system = QStringLiteral("NTFS");
    volume.label = dynamicDisk ? QStringLiteral("DynData") : QStringLiteral("Data");
    volume.total_bytes = 256 * kTestMegabyteBytes;
    volume.free_bytes = 192 * kTestMegabyteBytes;
    volume.health_status = QStringLiteral("Healthy");

    sak::PartitionInfoEx partition;
    partition.disk_number = sak::DiskNumber{0};
    partition.partition_number = sak::PartitionNumber{1};
    partition.type_name = dynamicDisk ? QStringLiteral("Simple Volume") : QStringLiteral("Basic");
    partition.offset_bytes = kTestMegabyteBytes;
    partition.size_bytes = volume.total_bytes;
    partition.volume = volume;

    sak::PartitionDiskInfo disk;
    disk.disk_number = sak::DiskNumber{0};
    disk.partition_style = dynamicDisk ? QStringLiteral("Dynamic") : QStringLiteral("MBR");
    disk.is_dynamic = dynamicDisk;
    disk.health_status = QStringLiteral("Healthy");
    disk.operational_status = QStringLiteral("Online");
    disk.size_bytes = 1024 * kTestMegabyteBytes;
    disk.partitions.append(partition);

    sak::PartitionInventory inventory;
    inventory.layout_hash = dynamicDisk ? QStringLiteral("dynamic-rebuild-layout")
                                        : QStringLiteral("metadata-rebuild-layout");
    inventory.disks.append(disk);
    return inventory;
}

sak::PartitionInventory unallocatedAllocateInventoryFixture() {
    sak::PartitionDiskInfo disk;
    disk.disk_number = sak::DiskNumber{0};
    disk.partition_style = QStringLiteral("MBR");
    disk.health_status = QStringLiteral("Healthy");
    disk.operational_status = QStringLiteral("Online");
    disk.size_bytes = 1024 * kTestMegabyteBytes;

    sak::PartitionVolumeInfo firstVolume;
    firstVolume.drive_letter = QStringLiteral("P");
    firstVolume.file_system = QStringLiteral("NTFS");
    firstVolume.label = QStringLiteral("Previous");
    firstVolume.total_bytes = 256 * kTestMegabyteBytes;
    firstVolume.free_bytes = 192 * kTestMegabyteBytes;
    firstVolume.health_status = QStringLiteral("Healthy");

    sak::PartitionInfoEx first;
    first.disk_number = disk.disk_number;
    first.partition_number = sak::PartitionNumber{1};
    first.type_name = QStringLiteral("Basic");
    first.offset_bytes = kTestMegabyteBytes;
    first.size_bytes = firstVolume.total_bytes;
    first.volume = firstVolume;
    disk.partitions.append(first);

    disk.unallocated_regions.append(
        {disk.disk_number, first.offset_bytes + first.size_bytes, 128 * kTestMegabyteBytes});

    sak::PartitionVolumeInfo secondVolume;
    secondVolume.drive_letter = QStringLiteral("T");
    secondVolume.file_system = QStringLiteral("NTFS");
    secondVolume.label = QStringLiteral("Next");
    secondVolume.total_bytes = 256 * kTestMegabyteBytes;
    secondVolume.free_bytes = 192 * kTestMegabyteBytes;
    secondVolume.health_status = QStringLiteral("Healthy");

    sak::PartitionInfoEx second;
    second.disk_number = disk.disk_number;
    second.partition_number = sak::PartitionNumber{2};
    second.type_name = QStringLiteral("Basic");
    second.offset_bytes = disk.unallocated_regions.first().offset_bytes +
                          disk.unallocated_regions.first().size_bytes;
    second.size_bytes = secondVolume.total_bytes;
    second.volume = secondVolume;
    disk.partitions.append(second);

    sak::PartitionInventory inventory;
    inventory.layout_hash = QStringLiteral("unallocated-allocate-layout");
    inventory.disks.append(disk);
    return inventory;
}

sak::PartitionInfoEx rolePartition(uint32_t number,
                                   const QString& typeName,
                                   const QString& driveLetter,
                                   uint64_t offsetBytes) {
    sak::PartitionVolumeInfo volume;
    volume.drive_letter = driveLetter;
    volume.file_system = QStringLiteral("NTFS");
    volume.total_bytes = 96 * kTestMegabyteBytes;
    volume.free_bytes = 48 * kTestMegabyteBytes;

    sak::PartitionInfoEx partition;
    partition.disk_number = sak::DiskNumber{0};
    partition.partition_number = sak::PartitionNumber{number};
    partition.type_name = typeName;
    partition.offset_bytes = offsetBytes;
    partition.size_bytes = volume.total_bytes;
    partition.volume = volume;
    return partition;
}

sak::PartitionInventory allColorRolesInventoryFixture() {
    sak::PartitionDiskInfo basicDisk;
    basicDisk.disk_number = sak::DiskNumber{0};
    basicDisk.partition_style = QStringLiteral("MBR");
    basicDisk.health_status = QStringLiteral("Healthy");
    basicDisk.operational_status = QStringLiteral("Online");
    basicDisk.size_bytes = 384 * kTestMegabyteBytes;
    basicDisk.partitions.append(
        rolePartition(1, QStringLiteral("Basic"), QStringLiteral("P"), 1 * kTestMegabyteBytes));
    basicDisk.partitions.append(
        rolePartition(2, QStringLiteral("Logical"), QStringLiteral("L"), 128 * kTestMegabyteBytes));
    basicDisk.unallocated_regions.append(
        {sak::DiskNumber{0}, 256 * kTestMegabyteBytes, 64 * kTestMegabyteBytes});

    sak::PartitionDiskInfo dynamicDisk;
    dynamicDisk.disk_number = sak::DiskNumber{1};
    dynamicDisk.partition_style = QStringLiteral("Dynamic");
    dynamicDisk.is_dynamic = true;
    dynamicDisk.health_status = QStringLiteral("Healthy");
    dynamicDisk.operational_status = QStringLiteral("Online");
    dynamicDisk.size_bytes = 640 * kTestMegabyteBytes;
    dynamicDisk.partitions.append(rolePartition(
        1, QStringLiteral("Simple Volume"), QStringLiteral("S"), 1 * kTestMegabyteBytes));
    dynamicDisk.partitions.append(rolePartition(
        2, QStringLiteral("Spanned Volume"), QStringLiteral("N"), 128 * kTestMegabyteBytes));
    dynamicDisk.partitions.append(rolePartition(
        3, QStringLiteral("Striped Volume"), QStringLiteral("T"), 256 * kTestMegabyteBytes));
    dynamicDisk.partitions.append(rolePartition(
        4, QStringLiteral("Mirrored Volume"), QStringLiteral("M"), 384 * kTestMegabyteBytes));
    dynamicDisk.partitions.append(rolePartition(
        5, QStringLiteral("RAID5 Volume"), QStringLiteral("R"), 512 * kTestMegabyteBytes));
    for (auto& partition : dynamicDisk.partitions) {
        partition.disk_number = dynamicDisk.disk_number;
    }

    sak::PartitionInventory inventory;
    inventory.layout_hash = QStringLiteral("panel-all-color-roles");
    inventory.disks.append(basicDisk);
    inventory.disks.append(dynamicDisk);
    return inventory;
}

void setRawFileSystem(sak::PartitionVolumeInfo* volume,
                      const QString& fileSystem,
                      const QStringList& details = {}) {
    volume->file_system = fileSystem;
    volume->file_system_source = sak::PartitionFileSystemDetector::rawSignatureSource();
    volume->file_system_details = details;
}

void setRawExtVolumeForResize(sak::PartitionInventory* inventory, bool usePartitionReference) {
    auto& partition = inventory->disks[0].partitions[0];
    auto& volume = partition.volume.value();
    setRawFileSystem(&volume, QStringLiteral("ext4"));
    volume.drive_letter.clear();
    volume.total_bytes = usePartitionReference ? partition.size_bytes
                                               : inventory->disks[0].partitions[0].size_bytes;
    volume.free_bytes = inventory->disks[0].partitions[0].size_bytes / 2;
}

void configureRawHfsPanel(sak::PartitionManagerPanel* panel) {
    auto inventory = unallocatedAllocateInventoryFixture();
    setRawFileSystem(&inventory.disks[0].partitions[0].volume.value(),
                     QStringLiteral("HFS+"),
                     {QStringLiteral("HFS wrapper: Yes"),
                      QStringLiteral("Version: 4"),
                      QStringLiteral("Block size: 4096")});
    panel->setTestInventoryForReview(inventory);
}

void verifyRawHfsSidebarControls(sak::PartitionManagerPanel* panel) {
    auto* table = panel->findChild<QTableWidget*>();
    QVERIFY2(table != nullptr, "Partition table should exist");
    table->selectRow(1);
    QApplication::processEvents();

    auto* inspect = findToolButtonByName(panel, QStringLiteral("Inspect Non-Windows File System"));
    auto* browse = findToolButtonByName(panel, QStringLiteral("Browse Non-Windows File System"));
    auto* check = findToolButtonByName(panel, QStringLiteral("Check Non-Windows File System"));
    auto* resize = findToolButtonByName(panel, QStringLiteral("Resize/Move Partition"));
    auto* nativeCheck = findToolButtonByName(panel, QStringLiteral("Check File System"));
    auto* changeCluster = findToolButtonByName(panel, QStringLiteral("Change Cluster Size"));
    auto* changeLabel = findToolButtonByName(panel, QStringLiteral("Change Label"));
    QVERIFY2(inspect != nullptr && browse != nullptr && check != nullptr, "HFS actions exist");
    QVERIFY2(resize != nullptr && nativeCheck != nullptr, "Native actions exist");
    QVERIFY2(changeCluster != nullptr && changeLabel != nullptr, "Metadata actions exist");
    // Every tooltip here is a DISABLED-reason or a capability claim the operator reads before
    // touching a foreign filesystem, and each is built by .arg(file_system) from a shared
    // template -- so a contains() on the prefix cannot see the wrong filesystem name
    // interpolated, nor the three "Non-Windows filesystem actions" reasons diverging.
    const QString nativeDisabledReason = QStringLiteral(
        "Use the Non-Windows filesystem actions for HFS+; Windows-native file-system action is "
        "disabled.");
    QVERIFY(inspect->isEnabled());
    QCOMPARE(inspect->toolTip(), QStringLiteral("Inspect captured read-only HFS+ metadata"));
    QVERIFY(browse->isEnabled());
    QCOMPARE(browse->toolTip(), QStringLiteral("Browse read-only HFS+ directory entries"));
    QVERIFY(check->isEnabled());
    QVERIFY(check->toolTip().contains(QStringLiteral("fsck_hfs")));
    QVERIFY(!resize->isEnabled());
    QCOMPARE(resize->toolTip(),
             QStringLiteral("HFS+ resize is not supported yet. Non-Windows resize currently "
                            "supports ext2/ext3/ext4 only."));
    QVERIFY(!nativeCheck->isEnabled());
    QCOMPARE(nativeCheck->toolTip(), nativeDisabledReason);
    QVERIFY(!changeCluster->isEnabled());
    QCOMPARE(changeCluster->toolTip(), nativeDisabledReason);
    QVERIFY(!changeLabel->isEnabled());
    QCOMPARE(changeLabel->toolTip(), nativeDisabledReason);
}

void verifyRawHfsInspectDialog(sak::PartitionManagerPanel* panel) {
    bool inspected = false;
    QTimer::singleShot(0, [&]() {
        auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
        QVERIFY2(dialog != nullptr, "Inspect filesystem dialog should open");
        auto* properties =
            dialog->findChild<QTableWidget*>(QStringLiteral("partitionPropertiesTable"));
        QVERIFY2(properties != nullptr, "Inspect filesystem table should exist");
        const QString metadata = propertyTableValue(properties,
                                                    QStringLiteral("Read-only metadata"));
        // The whole read-only metadata block, in order: two contains() probes could not see a
        // third line appearing, a line dropping, or the values pairing with the wrong labels.
        QCOMPARE(metadata, QStringLiteral("HFS wrapper: Yes\nVersion: 4\nBlock size: 4096"));
        QCOMPARE(propertyTableValue(properties, QStringLiteral("File system")),
                 QStringLiteral("HFS+"));
        inspected = true;
        dialog->reject();
    });
    auto* inspect = findToolButtonByName(panel, QStringLiteral("Inspect Non-Windows File System"));
    QVERIFY2(inspect != nullptr, "Inspect Non-Windows File System action should exist");
    inspect->click();
    QVERIFY(inspected);
}

void verifyRawHfsContextMenu(sak::PartitionManagerPanel* panel) {
    auto* segment = panel->findChild<QWidget*>(QStringLiteral("partitionDiskMapSegment"));
    QVERIFY2(segment != nullptr, "Disk map should render a partition segment");
    const QStringList actions = contextMenuActionTexts(segment);
    QVERIFY(actions.contains(QStringLiteral("Inspect Non-Windows File System")));
    QVERIFY(actions.contains(QStringLiteral("Browse Non-Windows File System")));
    QVERIFY(actions.contains(QStringLiteral("Check Non-Windows File System")));
    const auto actionStates = contextMenuActionStates(segment);
    // These native actions must be PRESENT and disabled for a raw HFS+ partition. The default is
    // `true` so a MISSING key (the action dropped from the menu entirely) fails: QHash::value()
    // with no default returns a default-constructed false, which would make a removed action pass
    // this `== false` check just like a present-and-disabled one.
    QCOMPARE(actionStates.value(QStringLiteral("Resize/Move Partition"), true), false);
    QCOMPARE(actionStates.value(QStringLiteral("Check File System"), true), false);
    QCOMPARE(actionStates.value(QStringLiteral("Change Cluster Size"), true), false);
    QCOMPARE(actionStates.value(QStringLiteral("Change Label"), true), false);
    // These already expect `true`, so a removed action (default false) would fail them anyway.
    QCOMPARE(actionStates.value(QStringLiteral("Browse Non-Windows File System")), true);
    QCOMPARE(actionStates.value(QStringLiteral("Check Non-Windows File System")), true);
}

void configureRawMetadataPanel(sak::PartitionManagerPanel* panel,
                               const QString& fileSystem,
                               const QStringList& details) {
    auto inventory = applyReviewInventoryFixture();
    setRawFileSystem(&inventory.disks[0].partitions[0].volume.value(), fileSystem, details);
    panel->setTestInventoryForReview(inventory);
    auto* table = panel->findChild<QTableWidget*>();
    QVERIFY2(table != nullptr, "Partition table should exist");
    table->selectRow(1);
}

void verifyMetadataCheckDialog(QToolButton* button, const QString& expectedFinding) {
    bool inspected = false;
    QTimer::singleShot(0, [&]() {
        auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
        QVERIFY2(dialog != nullptr, "Metadata check dialog should open");
        auto* properties =
            dialog->findChild<QTableWidget*>(QStringLiteral("partitionPropertiesTable"));
        QVERIFY2(properties != nullptr, "Metadata check table should exist");
        QCOMPARE(propertyTableValue(properties, QStringLiteral("Check type")),
                 QStringLiteral("Original read-only metadata consistency check"));
        QCOMPARE(propertyTableValue(properties, QStringLiteral("Result")),
                 QStringLiteral("No sanity warnings"));
        // The findings cell is the check's ENTIRE verdict list. A contains() on the
        // "Metadata sanity: <FS>" prefix passed regardless of what the sentence went on to
        // say -- including a warning line the "No sanity warnings" result contradicts.
        QCOMPARE(propertyTableValue(properties, QStringLiteral("Findings")), expectedFinding);
        inspected = true;
        dialog->reject();
    });
    button->click();
    QVERIFY(inspected);
}

void verifyXfsPropertiesAndInspect(sak::PartitionManagerPanel* panel) {
    bool propertiesInspected = false;
    QTimer::singleShot(0, [&]() {
        auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
        QVERIFY2(dialog != nullptr, "Properties dialog should open");
        auto* properties =
            dialog->findChild<QTableWidget*>(QStringLiteral("partitionPropertiesTable"));
        QVERIFY2(properties != nullptr, "Properties table should exist");
        const QString metadata = propertyTableValue(properties,
                                                    QStringLiteral("File system metadata"));
        // All three captured probe lines, in order: the geometry values are what the sanity
        // verdict was computed FROM, so a verdict shown without them is unsupported.
        QCOMPARE(metadata,
                 QStringLiteral("Block size: 4096\nData blocks: 32768\nMetadata sanity: XFS "
                                "superblock geometry is internally consistent"));
        propertiesInspected = true;
        dialog->reject();
    });
    auto* propertiesButton = findToolButtonByName(panel, QStringLiteral("Properties"));
    QVERIFY2(propertiesButton != nullptr, "Properties action should exist");
    propertiesButton->click();
    QVERIFY(propertiesInspected);

    bool inspectInspected = false;
    QTimer::singleShot(0, [&]() {
        auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
        QVERIFY2(dialog != nullptr, "Inspect filesystem dialog should open");
        auto* properties =
            dialog->findChild<QTableWidget*>(QStringLiteral("partitionPropertiesTable"));
        QVERIFY2(properties != nullptr, "Inspect filesystem table should exist");
        const QString metadata = propertyTableValue(properties,
                                                    QStringLiteral("Read-only metadata"));
        // Inspect must show the SAME captured block as Properties above, not a shortened one.
        QCOMPARE(metadata,
                 QStringLiteral("Block size: 4096\nData blocks: 32768\nMetadata sanity: XFS "
                                "superblock geometry is internally consistent"));
        inspectInspected = true;
        dialog->reject();
    });
    auto* inspectButton = findToolButtonByName(panel,
                                               QStringLiteral("Inspect Non-Windows File System"));
    QVERIFY2(inspectButton != nullptr, "Inspect Non-Windows File System action should exist");
    inspectButton->click();
    QVERIFY(inspectInspected);
}

void configureRawWritePanel(sak::PartitionManagerPanel* panel, const QString& fileSystem) {
    auto inventory = applyReviewInventoryFixture();
    setRawFileSystem(&inventory.disks[0].partitions[0].volume.value(), fileSystem);
    panel->setTestInventoryForReview(inventory);
    auto* table = panel->findChild<QTableWidget*>();
    QVERIFY2(table != nullptr, "Partition table should exist");
    table->selectRow(1);
}

// The queued line is "<operation display name> - <target summary>", and a blocked operation
// appends " - BLOCKED: <reasons>" (partition_manager_panel.cpp refreshPendingOperations). Compare
// the WHOLE line: a contains() on the name half could not see the operation queued against the
// wrong disk/partition, nor a BLOCKED suffix appended to an operation the caller expects to run.
void verifySingleQueuedOperation(sak::PartitionManagerPanel* panel, const QString& text) {
    auto* queue = panel->findChild<QListWidget*>();
    QVERIFY2(queue != nullptr, "Pending operation queue should exist");
    QCOMPARE(queue->count(), 1);
    QCOMPARE(queue->item(0)->text(), text);
}

void queueExtFormatAndVerify() {
    sak::PartitionManagerPanel panel;
    configureRawWritePanel(&panel, QStringLiteral("ext4"));
    bool inspected = false;
    QTimer::singleShot(0, [&]() {
        auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
        QVERIFY2(dialog != nullptr, "Format dialog should open");
        auto* fileSystem = findAccessibleWidget<QComboBox>(dialog, QStringLiteral("File system"));
        QVERIFY(fileSystem != nullptr);
        QCOMPARE(fileSystem->currentText(), QStringLiteral("ext4"));
        auto* confirm = findAccessibleWidget<QCheckBox>(
            dialog, QStringLiteral("Confirm ext filesystem format"));
        QVERIFY(confirm != nullptr);
        confirm->setChecked(true);
        inspected = true;
        dialog->accept();
    });
    auto* format = findToolButtonByName(&panel, QStringLiteral("Format Partition"));
    QVERIFY2(format != nullptr, "Format action should exist");
    format->click();
    QVERIFY(inspected);
    verifySingleQueuedOperation(&panel, QStringLiteral("Format Partition - Disk 0 Partition 1"));
}

void queueLinuxSwapFormatAndVerify() {
    sak::PartitionManagerPanel panel;
    configureRawWritePanel(&panel, QStringLiteral("Linux swap"));
    bool inspected = false;
    QTimer::singleShot(0, [&]() {
        auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
        QVERIFY2(dialog != nullptr, "Format dialog should open");
        auto* fileSystem = findAccessibleWidget<QComboBox>(dialog, QStringLiteral("File system"));
        QVERIFY(fileSystem != nullptr);
        QCOMPARE(fileSystem->currentText(), QStringLiteral("Linux swap"));
        auto* pageSize = findAccessibleWidget<QComboBox>(dialog,
                                                         QStringLiteral("Linux swap page size"));
        QVERIFY(pageSize != nullptr);
        QCOMPARE(pageSize->currentText(), QStringLiteral("4096"));
        auto* confirm =
            findAccessibleWidget<QCheckBox>(dialog, QStringLiteral("Confirm Linux swap format"));
        QVERIFY(confirm != nullptr);
        confirm->setChecked(true);
        inspected = true;
        dialog->accept();
    });
    auto* format = findToolButtonByName(&panel, QStringLiteral("Format Partition"));
    QVERIFY2(format != nullptr, "Format action should exist");
    format->click();
    QVERIFY(inspected);
    verifySingleQueuedOperation(&panel, QStringLiteral("Format Partition - Disk 0 Partition 1"));
}

void queueApfsFormatAndVerify() {
    sak::PartitionManagerPanel panel;
    configureRawWritePanel(&panel, QStringLiteral("APFS"));
    bool inspected = false;
    QTimer::singleShot(0, [&]() {
        auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
        QVERIFY2(dialog != nullptr, "Format dialog should open");
        auto* fileSystem = findAccessibleWidget<QComboBox>(dialog, QStringLiteral("File system"));
        QVERIFY(fileSystem != nullptr);
        QCOMPARE(fileSystem->currentText(), QStringLiteral("APFS"));
        auto* confirm = findAccessibleWidget<QCheckBox>(dialog,
                                                        QStringLiteral("Confirm APFS format"));
        QVERIFY(confirm != nullptr);
        confirm->setChecked(true);
        inspected = true;
        dialog->accept();
    });
    auto* format = findToolButtonByName(&panel, QStringLiteral("Format Partition"));
    QVERIFY2(format != nullptr, "Format action should exist");
    format->click();
    QVERIFY(inspected);
    verifySingleQueuedOperation(&panel, QStringLiteral("Format Partition - Disk 0 Partition 1"));
}

void queueExtRepairAndVerify() {
    sak::PartitionManagerPanel panel;
    configureRawWritePanel(&panel, QStringLiteral("ext4"));
    bool inspected = false;
    QTimer::singleShot(0, [&]() {
        auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
        QVERIFY2(dialog != nullptr, "Non-Windows check dialog should open");
        auto* mode = findAccessibleWidget<QComboBox>(
            dialog, QStringLiteral("Non-Windows filesystem check mode"));
        QVERIFY(mode != nullptr);
        const int repairIndex =
            mode->findData(sak::PartitionFileSystemToolRunner::repairOperation());
        QVERIFY(repairIndex >= 0);
        mode->setCurrentIndex(repairIndex);
        auto* targetPath = findAccessibleWidget<QLineEdit>(
            dialog, QStringLiteral("Non-Windows filesystem target path"));
        QVERIFY(targetPath != nullptr);
        QVERIFY(targetPath->isReadOnly());
        // The read-only field's tooltip is the reason it cannot be typed into: the target is
        // taken from the selection, never from operator input. The other branch of that same
        // ternary ("Run read-only check for %1.") also mentions no such phrase, so pin it whole.
        QCOMPARE(targetPath->toolTip(),
                 QStringLiteral("Queued repair uses the selected raw partition target."));
        auto* confirm = findAccessibleWidget<QCheckBox>(
            dialog, QStringLiteral("Confirm ext filesystem repair"));
        QVERIFY(confirm != nullptr);
        confirm->setChecked(true);
        inspected = true;
        dialog->accept();
    });
    auto* check = findToolButtonByName(&panel, QStringLiteral("Check Non-Windows File System"));
    QVERIFY2(check != nullptr, "Non-Windows check action should exist");
    check->click();
    QVERIFY(inspected);
    verifySingleQueuedOperation(&panel, QStringLiteral("Check File System - Disk 0 Partition 1"));
}

void queueHfsRepairAndVerify() {
    sak::PartitionManagerPanel panel;
    configureRawWritePanel(&panel, QStringLiteral("HFS+"));
    bool inspected = false;
    QTimer::singleShot(0, [&]() {
        auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
        QVERIFY2(dialog != nullptr, "Non-Windows check dialog should open");
        auto* mode = findAccessibleWidget<QComboBox>(
            dialog, QStringLiteral("Non-Windows filesystem check mode"));
        QVERIFY(mode != nullptr);
        // The mode list is the CLOSED set of things this dialog can do to an HFS+ volume, and
        // each entry's data value is what actually selects the operation. findText() >= 0
        // proved only presence: a fourth destructive mode, or a read-only entry carrying the
        // REPAIR operation payload, was invisible.
        QCOMPARE(mode->count(), 3);
        QCOMPARE(mode->itemText(0), QStringLiteral("Original HFS+ catalog check now"));
        QCOMPARE(mode->itemText(1), QStringLiteral("Read-only check now"));
        QCOMPARE(mode->itemData(1).toString(),
                 sak::PartitionFileSystemToolRunner::readOnlyCheckOperation());
        QCOMPARE(mode->itemText(2), QStringLiteral("Queue HFS+ repair"));
        QCOMPARE(mode->itemData(2).toString(),
                 sak::PartitionFileSystemToolRunner::repairOperation());
        const int repairIndex =
            mode->findData(sak::PartitionFileSystemToolRunner::repairOperation());
        QVERIFY(repairIndex >= 0);
        mode->setCurrentIndex(repairIndex);
        auto* targetPath = findAccessibleWidget<QLineEdit>(
            dialog, QStringLiteral("Non-Windows filesystem target path"));
        QVERIFY(targetPath != nullptr);
        QVERIFY(targetPath->isReadOnly());
        auto* confirm = findAccessibleWidget<QCheckBox>(
            dialog, QStringLiteral("Confirm HFS+ filesystem repair"));
        QVERIFY(confirm != nullptr);
        confirm->setChecked(true);
        inspected = true;
        dialog->accept();
    });
    auto* check = findToolButtonByName(&panel, QStringLiteral("Check Non-Windows File System"));
    QVERIFY2(check != nullptr, "Non-Windows check action should exist");
    check->click();
    QVERIFY(inspected);
    verifySingleQueuedOperation(&panel, QStringLiteral("Check File System - Disk 0 Partition 1"));
}

void queueGeneratedApfsRepairAndVerify() {
    sak::PartitionManagerPanel panel;
    auto inventory = applyReviewInventoryFixture();
    setRawFileSystem(
        &inventory.disks[0].partitions[0].volume.value(),
        QStringLiteral("APFS"),
        {QStringLiteral("Metadata sanity: APFS container block geometry is internally consistent"),
         QStringLiteral("APFS space manager block: 10"),
         QStringLiteral("APFS volume candidate block 6 index 0 name SAK APFS volume object map OID "
                        "103 root tree OID 104"),
         QStringLiteral("Volume OIDs: 102")});
    panel.setTestInventoryForReview(inventory);
    auto* table = panel.findChild<QTableWidget*>();
    QVERIFY2(table != nullptr, "Partition table should exist");
    table->selectRow(1);
    bool inspected = false;
    QTimer::singleShot(0, [&]() {
        auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
        QVERIFY2(dialog != nullptr, "Non-Windows check dialog should open");
        auto* mode = findAccessibleWidget<QComboBox>(
            dialog, QStringLiteral("Non-Windows filesystem check mode"));
        QVERIFY(mode != nullptr);
        // APFS offers exactly the read-only check and the generated repair -- no catalog-check
        // entry (that is HFS+ only). Pinning count + text + data payload is what shows the
        // read-only entry is not silently wired to the repair operation.
        QCOMPARE(mode->count(), 2);
        QCOMPARE(mode->itemText(0), QStringLiteral("Read-only check now"));
        QCOMPARE(mode->itemData(0).toString(),
                 sak::PartitionFileSystemToolRunner::readOnlyCheckOperation());
        QCOMPARE(mode->itemText(1), QStringLiteral("Queue generated APFS repair"));
        QCOMPARE(mode->itemData(1).toString(),
                 sak::PartitionFileSystemToolRunner::repairOperation());
        const int repairIndex =
            mode->findData(sak::PartitionFileSystemToolRunner::repairOperation());
        QVERIFY(repairIndex >= 0);
        mode->setCurrentIndex(repairIndex);
        auto* confirm = findAccessibleWidget<QCheckBox>(
            dialog, QStringLiteral("Confirm APFS filesystem repair"));
        QVERIFY(confirm != nullptr);
        confirm->setChecked(true);
        inspected = true;
        dialog->accept();
    });
    auto* check = findToolButtonByName(&panel, QStringLiteral("Check Non-Windows File System"));
    QVERIFY2(check != nullptr, "Non-Windows check action should exist");
    check->click();
    QVERIFY(inspected);
    verifySingleQueuedOperation(&panel, QStringLiteral("Check File System - Disk 0 Partition 1"));
}

sak::PartitionInventory generatedApfsInventoryFixture() {
    auto inventory = applyReviewInventoryFixture();
    setRawFileSystem(
        &inventory.disks[0].partitions[0].volume.value(),
        QStringLiteral("APFS"),
        {QStringLiteral("Metadata sanity: APFS container block geometry is internally consistent"),
         QStringLiteral("APFS space manager block: 10"),
         QStringLiteral("APFS volume candidate block 6 index 0 name SAK APFS volume object map OID "
                        "103 root tree OID 104"),
         QStringLiteral("Volume OIDs: 102")});
    return inventory;
}

void verifyApfsContainerModeItems(QComboBox* mode) {
    // Volume-label rename moved to the unified "Change Label" action, so the container
    // dialog now offers only snapshot and resize modes.
    QVERIFY(!comboHasItem(mode, QStringLiteral("Change volume label")));
    QCOMPARE(mode->currentText(), QStringLiteral("Create snapshot"));
    // The exact ordered set. Four presence probes plus one absence probe still allowed a FIFTH
    // container mode to appear, and could not see the four reordered -- which matters because
    // index 0 is the default the dialog opens on (asserted above).
    QCOMPARE(mode->count(), 4);
    QCOMPARE(comboItemsText(mode),
             QStringLiteral("Create snapshot|Delete snapshot|Revert to snapshot|Resize container "
                            "to fill partition"));
}

void queueApfsVolumeLabelChangeAndVerify() {
    sak::PartitionManagerPanel panel;
    panel.setTestInventoryForReview(generatedApfsInventoryFixture());
    auto* table = panel.findChild<QTableWidget*>();
    QVERIFY2(table != nullptr, "Partition table should exist");
    table->selectRow(1);
    QApplication::processEvents();

    // A generated APFS volume renames through the unified Change Label verb (no Windows
    // drive letter required); it routes to the certified COW volume-label commit.
    auto* changeLabel = findToolButtonByName(&panel, QStringLiteral("Change Label"));
    QVERIFY2(changeLabel != nullptr, "Change Label action should exist");
    QVERIFY2(changeLabel->isEnabled(), "Change Label should enable for a generated APFS volume");

    bool handled = false;
    QTimer::singleShot(0, [&]() {
        auto* dialog = qobject_cast<QInputDialog*>(QApplication::activeModalWidget());
        QVERIFY2(dialog != nullptr, "Change Label input dialog should open");
        dialog->setTextValue(QStringLiteral("Renamed"));
        handled = true;
        dialog->accept();
    });
    changeLabel->click();
    QVERIFY(handled);
    verifySingleQueuedOperation(&panel,
                                QStringLiteral("APFS Change Volume Label - Disk 0 Partition 1"));
}

// Drives the APFS Container action dialog into snapshot-create mode (checking
// the resize/name-field visibility toggles on the way) and verifies the queued
// operation. The former per-file APFS/HFS mutation dialogs moved to the File
// Management explorer; their coverage lives in the explorer bridge suites.
void queueApfsContainerSnapshotAndVerify() {
    sak::PartitionManagerPanel panel;
    panel.setTestInventoryForReview(generatedApfsInventoryFixture());
    auto* table = panel.findChild<QTableWidget*>();
    QVERIFY2(table != nullptr, "Partition table should exist");
    table->selectRow(1);
    QApplication::processEvents();

    auto* button = findToolButtonByName(&panel, QStringLiteral("APFS Container"));
    QVERIFY2(button != nullptr, "APFS Container action should exist");
    QVERIFY(button->isEnabled());
    // Two different tooltips in production contain this fragment -- the enabled invitation and
    // the longer "snapshot or resize" static label -- so the fragment could not tell an enabled
    // action from a disabled one carrying a leftover reason string.
    QCOMPARE(button->toolTip(), QStringLiteral("Queue an APFS container action."));

    bool inspected = false;
    QTimer::singleShot(0, [&]() {
        auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
        QVERIFY2(dialog != nullptr, "APFS container dialog should open");
        auto* mode = findAccessibleWidget<QComboBox>(dialog,
                                                     QStringLiteral("APFS container action mode"));
        QVERIFY(mode != nullptr);
        verifyApfsContainerModeItems(mode);
        auto* name = findAccessibleWidget<QLineEdit>(dialog,
                                                     QStringLiteral("APFS container action name"));
        QVERIFY(name != nullptr);
        // Resize hides the name field; snapshot/label require it.
        mode->setCurrentText(QStringLiteral("Resize container to fill partition"));
        QVERIFY(!name->isVisible());
        mode->setCurrentText(QStringLiteral("Create snapshot"));
        QVERIFY(name->isVisible());
        name->setText(QStringLiteral("panel-snap"));
        auto* confirm = findAccessibleWidget<QCheckBox>(
            dialog, QStringLiteral("Confirm APFS container action"));
        QVERIFY(confirm != nullptr);
        confirm->setChecked(true);
        inspected = true;
        dialog->accept();
    });
    button->click();
    QVERIFY(inspected);
    verifySingleQueuedOperation(&panel,
                                QStringLiteral("APFS Snapshot Create - Disk 0 Partition 1"));
}

void queueExtResizeAndVerify(bool grow) {
    sak::PartitionManagerPanel panel;
    auto inventory = unallocatedAllocateInventoryFixture();
    setRawExtVolumeForResize(&inventory, grow);
    panel.setTestInventoryForReview(inventory);
    auto* table = panel.findChild<QTableWidget*>();
    QVERIFY2(table != nullptr, "Partition table should exist");
    table->selectRow(1);
    bool inspected = false;
    QTimer::singleShot(0, [&]() {
        auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
        QVERIFY2(dialog != nullptr, "Resize dialog should open");
        auto* size = findAccessibleWidget<QSpinBox>(dialog,
                                                    QStringLiteral("Target partition size"));
        QVERIFY(size != nullptr);
        size->setValue(size->value() + (grow ? 64 : -64));
        auto* confirm = findAccessibleWidget<QCheckBox>(
            dialog, QStringLiteral("Confirm ext filesystem resize"));
        QVERIFY(confirm != nullptr);
        confirm->setChecked(true);
        inspected = true;
        dialog->accept();
    });
    auto* resize = findToolButtonByName(&panel, QStringLiteral("Resize/Move Partition"));
    QVERIFY2(resize != nullptr, "Resize action should exist");
    resize->click();
    QVERIFY(inspected);
    verifySingleQueuedOperation(&panel, QStringLiteral("Resize Partition - Disk 0 Partition 1"));
}

void queueUnallocatedAllocateAndVerifyResize() {
    sak::PartitionManagerPanel panel;
    panel.setTestInventoryForReview(unallocatedAllocateInventoryFixture());
    auto* table = panel.findChild<QTableWidget*>();
    QVERIFY2(table != nullptr, "Partition table should exist");
    table->selectRow(table->rowCount() - 1);
    bool inspected = false;
    QTimer::singleShot(0, [&]() {
        auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
        QVERIFY2(dialog != nullptr, "Allocate Free Space To dialog should open");
        QCOMPARE(dialog->accessibleName(), QStringLiteral("Allocate Free Space To"));
        auto* target = findAccessibleWidget<QComboBox>(
            dialog, QStringLiteral("Allocate free space target partition"));
        QVERIFY(target != nullptr);
        // Which partitions are offered, and in which order, decides what index 0 (the default
        // this test then accepts) actually resizes. currentIndex()==0 alone said nothing about
        // that: an empty or reordered list satisfies it while allocating the free space to a
        // different partition than the operator expects.
        QCOMPARE(target->count(), 2);
        QCOMPARE(target->itemText(0), QStringLiteral("Extend Partition 1 (P:)"));
        QCOMPARE(target->itemText(1), QStringLiteral("Move/extend Partition 2 (T:)"));
        QCOMPARE(target->currentIndex(), 0);
        inspected = true;
        dialog->accept();
    });
    auto* button = findToolButtonByName(&panel, QStringLiteral("Allocate Free Space"));
    QVERIFY2(button != nullptr, "Allocate Free Space action should exist");
    button->click();
    QVERIFY(inspected);
    verifySingleQueuedOperation(&panel, QStringLiteral("Resize Partition - Disk 0 Partition 1"));
}

void queueUnallocatedAllocateAndVerifyMove() {
    QTemporaryDir backupRoot;
    QVERIFY(backupRoot.isValid());
    sak::PartitionManagerPanel panel;
    panel.setTestInventoryForReview(unallocatedAllocateInventoryFixture());
    auto* table = panel.findChild<QTableWidget*>();
    QVERIFY2(table != nullptr, "Partition table should exist");
    table->selectRow(table->rowCount() - 1);
    bool inspected = false;
    QTimer::singleShot(0, [&]() {
        auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
        QVERIFY2(dialog != nullptr, "Allocate Free Space To dialog should open");
        QCOMPARE(dialog->accessibleName(), QStringLiteral("Allocate Free Space To"));
        auto* target = findAccessibleWidget<QComboBox>(
            dialog, QStringLiteral("Allocate free space target partition"));
        QVERIFY(target != nullptr);
        target->setCurrentIndex(1);
        auto* backup = findAccessibleWidget<QLineEdit>(
            dialog, QStringLiteral("Allocate free space to backup directory"));
        QVERIFY(backup != nullptr);
        backup->setText(backupRoot.path());
        auto* confirm = findAccessibleWidget<QCheckBox>(
            dialog, QStringLiteral("Confirm allocate free space to backup and restore"));
        QVERIFY(confirm != nullptr);
        confirm->setChecked(true);
        inspected = true;
        dialog->accept();
    });
    auto* button = findToolButtonByName(&panel, QStringLiteral("Allocate Free Space"));
    QVERIFY2(button != nullptr, "Allocate Free Space action should exist");
    button->click();
    QVERIFY(inspected);
    verifySingleQueuedOperation(&panel, QStringLiteral("Move Partition - Disk 0 Partition 2"));
}

struct ExpectedMetadataAction {
    QString button;
    QString dialog_name;
    QString backup_accessible_name;
    QString confirm_accessible_name;
    QString queued_text;
    QString tooltip;
    bool dynamic_disk{false};
    int selected_row{1};
};

QVector<ExpectedMetadataAction> expectedMetadataActions() {
    return {{QStringLiteral("Convert Primary/Logical"),
             QStringLiteral("Convert Primary/Logical"),
             QStringLiteral("Primary logical backup directory"),
             QStringLiteral("Confirm primary logical backup and restore"),
             QStringLiteral("Convert Primary/Logical - Disk 0 Partition 1"),
             QStringLiteral("Back up, rebuild MBR primary/logical layout, restore, and verify")},
            {QStringLiteral("Change Serial Number"),
             QStringLiteral("Change Serial Number"),
             QStringLiteral("Volume serial backup directory"),
             QStringLiteral("Confirm volume serial backup and restore"),
             QStringLiteral("Change Volume Serial Number - Disk 0 Partition 1"),
             QStringLiteral("Back up, reformat to regenerate volume serial, restore, and verify")},
            {QStringLiteral("Convert Dynamic Disk to Basic"),
             QStringLiteral("Convert Dynamic Disk to Basic"),
             QStringLiteral("Dynamic to basic backup directory"),
             QStringLiteral("Confirm dynamic to basic backup and restore"),
             QStringLiteral("Convert Dynamic Disk to Basic - Disk 0"),
             QStringLiteral("Back up dynamic volume, convert disk to basic, restore, and verify"),
             true,
             0}};
}

void queueMetadataActionAndVerify(const ExpectedMetadataAction& action,
                                  const QString& backupDirectory) {
    sak::PartitionManagerPanel panel;
    panel.setTestInventoryForReview(metadataRebuildInventoryFixture(action.dynamic_disk));
    auto* table = panel.findChild<QTableWidget*>();
    QVERIFY2(table != nullptr, "Partition table should exist");
    table->selectRow(action.selected_row);
    auto* button = findToolButtonByName(&panel, action.button);
    QVERIFY2(button != nullptr, qPrintable(action.button));
    QVERIFY(button->isEnabled());
    // The tooltip is the operator's one-line summary of what the action DOES (back up, mutate,
    // restore, verify). "does not say blocked" was satisfied by an empty tooltip, or by one
    // copied from a different action -- either of which misdescribes a destructive rebuild.
    QCOMPARE(button->toolTip(), action.tooltip);

    bool inspected = false;
    QTimer::singleShot(0, [&]() {
        auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
        QVERIFY2(dialog != nullptr, "Operation dialog should open");
        QCOMPARE(dialog->accessibleName(), action.dialog_name);
        auto* backup = findAccessibleWidget<QLineEdit>(dialog, action.backup_accessible_name);
        QVERIFY2(backup != nullptr, qPrintable(action.backup_accessible_name));
        backup->setText(backupDirectory);
        auto* confirm = findAccessibleWidget<QCheckBox>(dialog, action.confirm_accessible_name);
        QVERIFY2(confirm != nullptr, qPrintable(action.confirm_accessible_name));
        confirm->setChecked(true);
        inspected = true;
        dialog->accept();
    });
    button->click();
    QVERIFY(inspected);
    verifySingleQueuedOperation(&panel, action.queued_text);
}

}  // namespace

void PartitionManagerPanelTests::scanButtonIsStatefulAndInventorySummaryUsesStatusBar() {
    sak::PartitionManagerPanel panel;

    auto* table = panel.findChild<QTableWidget*>();
    QVERIFY2(table != nullptr, "Partition Manager table should exist");
    QCOMPARE(table->rowCount(), 0);

    auto* scan = findToolButtonByName(&panel, QStringLiteral("Scan Disks"));
    QVERIFY2(scan != nullptr, "Initial inventory button should be Scan Disks");
    QCOMPARE(scan->toolTip(), QStringLiteral("Scan disk inventory"));
    QVERIFY(panel.findChild<QLabel*>(QStringLiteral("Partition manager summary")) == nullptr);

    panel.resize(900, 640);
    panel.show();
    QVERIFY(QTest::qWaitForWindowExposed(&panel));
    QTest::qWait(50);
    QCOMPARE(table->rowCount(), 0);

    QSignalSpy statusSpy(&panel, &sak::PartitionManagerPanel::statusMessage);
    panel.setTestInventoryForReview(applyReviewInventoryFixture());
    // Exactly one status message: !isEmpty() tolerated a burst of intermediate messages, and
    // takeLast() then hid all but the final one -- so a spurious earlier status (or a duplicate
    // inventory summary) was invisible.
    QCOMPARE(statusSpy.count(), 1);
    const auto lastStatus = statusSpy.takeLast();
    QCOMPARE(lastStatus.at(0).toString(), QStringLiteral("1 disk(s), layout panel-appl"));
    QCOMPARE(lastStatus.at(1).toInt(), 0);

    auto* refresh = findToolButtonByName(&panel, QStringLiteral("Refresh Disks"));
    QVERIFY2(refresh != nullptr, "Inventory button should change to Refresh Disks after scan");
    QCOMPARE(refresh->toolTip(), QStringLiteral("Refresh disk inventory"));
}

void PartitionManagerPanelTests::partitionTableUsesAomeiListChrome() {
    sak::PartitionManagerPanel panel;

    auto* table = panel.findChild<QTableWidget*>();
    QVERIFY2(table != nullptr, "Partition Manager table should exist");
    QVERIFY(!table->verticalHeader()->isVisible());
    QVERIFY(!table->showGrid());
    QVERIFY(!table->isCornerButtonEnabled());
    QCOMPARE(table->contextMenuPolicy(), Qt::CustomContextMenu);
    QCOMPARE(table->frameShape(), QFrame::NoFrame);
    QCOMPARE(table->lineWidth(), 0);
    QCOMPARE(table->midLineWidth(), 0);
    // Re-homed off the delegate's internal objectName (a pure test handle -- no QSS selector
    // references "partitionDiskSeparatorDelegate", so a rename would break this test while the
    // user sees the identical table) onto the delegate's actual visible contract: it paints a
    // 1px QPalette::Mid separator line along the bottom edge of a "disk" row and nothing on a
    // non-disk row. Drive the installed delegate's paint() over a synthetic one-column model
    // (column 0 == ColPartition) with a controlled palette whose Mid role is a distinctive red,
    // then sample the bottom-edge pixel. This survives a delegate rename and additionally pins
    // the kind-gating (disk vs partition) that the objectName check never verified.
    auto* delegate = table->itemDelegate();
    QVERIFY(delegate != nullptr);

    QTableWidget rows;
    rows.setColumnCount(1);
    rows.setRowCount(2);
    auto* diskItem = new QTableWidgetItem();
    diskItem->setData(Qt::UserRole, QVariantMap{{QStringLiteral("kind"), QStringLiteral("disk")}});
    rows.setItem(0, 0, diskItem);
    auto* partitionItem = new QTableWidgetItem();
    partitionItem->setData(Qt::UserRole,
                           QVariantMap{{QStringLiteral("kind"), QStringLiteral("partition")}});
    rows.setItem(1, 0, partitionItem);

    const QColor separatorColor(255, 0, 0);
    QStyleOptionViewItem option;
    option.rect = QRect(0, 0, 40, 16);
    option.palette.setColor(QPalette::Mid, separatorColor);
    const int sampleX = option.rect.width() / 2;
    const int bottomY = option.rect.bottom();

    // Paint the delegate for one model row into a white canvas and return the bottom-edge pixel.
    // The base QStyledItemDelegate::paint runs first and is identical for both rows, so the only
    // thing that can tint the bottom edge red is the disk separator drawn on top.
    const auto bottomPixelForRow = [&](int row) {
        QImage image(option.rect.size(), QImage::Format_ARGB32);
        image.fill(Qt::white);
        {
            QPainter painter(&image);
            delegate->paint(&painter, option, rows.model()->index(row, 0));
        }
        return image.pixelColor(sampleX, bottomY);
    };

    QCOMPARE(bottomPixelForRow(0), separatorColor);   // disk row: Mid separator painted
    QVERIFY(bottomPixelForRow(1) != separatorColor);  // partition row: kind-gated, no separator
}

void PartitionManagerPanelTests::sidebarIsFixedAndHasNoRedundantPreviewBox() {
    sak::PartitionManagerPanel panel;

    auto* actionsPane = panel.findChild<QFrame*>(QStringLiteral("partitionActionsPane"));
    QVERIFY2(actionsPane != nullptr, "Partition Manager action pane should exist");
    QCOMPARE(actionsPane->minimumWidth(), actionsPane->maximumWidth());
    QCOMPARE(actionsPane->sizePolicy().horizontalPolicy(), QSizePolicy::Fixed);

    const auto splitters = panel.findChildren<QSplitter*>();
    const auto hasHorizontalSplitter =
        std::any_of(splitters.cbegin(), splitters.cend(), [](const QSplitter* splitter) {
            return splitter->orientation() == Qt::Horizontal;
        });
    QVERIFY(!hasHorizontalSplitter);

    auto* redundantPreview =
        panel.findChild<QWidget*>(QStringLiteral("partitionApplyLayoutDiffPreview"));
    QVERIFY2(redundantPreview == nullptr, "Sidebar should not contain a queued layout preview box");

    const auto labels = panel.findChildren<QLabel*>();
    const auto hasQueuedPreviewLabel =
        std::any_of(labels.cbegin(), labels.cend(), [](const QLabel* label) {
            return label->text().contains(QStringLiteral("Queued Layout Preview"));
        });
    QVERIFY2(!hasQueuedPreviewLabel, "Sidebar should not show a Queued Layout Preview section");
}

void PartitionManagerPanelTests::sidebarActionsRenderAsCompactTextLinks() {
    sak::PartitionManagerPanel panel;

    const auto actions =
        panel.findChildren<QToolButton*>(QStringLiteral("partitionActionTextLink"));
    QVERIFY2(!actions.isEmpty(), "Sidebar actions should render as icon text links");
    const auto migrateIt =
        std::find_if(actions.cbegin(), actions.cend(), [](const QToolButton* button) {
            return button->accessibleName() == QStringLiteral("Migrate OS to SSD/HDD Wizard");
        });
    QVERIFY2(migrateIt != actions.cend(), "Wizard action should use text-link styling");
    // Re-homed off the raw QSS-string asserts (action->styleSheet().contains("background:
    // transparent"/"border: none")) -- brittle to a rephrase (background-color:, a merged rule,
    // moving the flat styling into a global sheet or a QStyle) that renders identically yet fails
    // the substring match. Assert the OBSERVABLE flat rendering instead: the link paints no fill
    // and no border along its right edge, so it reads as a text link rather than a filled button.
    for (auto* action : actions) {
        QVERIFY2(action->maximumHeight() <= 22, "Sidebar text links should be compact");
        QCOMPARE(action->toolButtonStyle(), Qt::ToolButtonTextBesideIcon);
        QVERIFY2(paintsFlatRightEdge(action),
                 "Sidebar text links should render flat (no button fill or border), not filled");
    }

    // Non-vacuity control: the same right-edge probe MUST reject an actually filled/bordered
    // button, proving the assertion above is not passing merely because render() never touches
    // the sampled strip.
    QToolButton filledControl;
    filledControl.setStyleSheet(
        QStringLiteral("QToolButton { background: #ff0000; border: 2px solid #ff0000; }"));
    QVERIFY2(!paintsFlatRightEdge(&filledControl),
             "Flat-render probe must reject a filled button (self-check)");
}

void PartitionManagerPanelTests::sidebarActionsGateBySelectedTargetKind() {
    sak::PartitionManagerPanel panel;
    panel.setTestInventoryForReview(unallocatedAllocateInventoryFixture());

    auto* table = panel.findChild<QTableWidget*>();
    QVERIFY2(table != nullptr, "Partition table should exist");

    auto* quickPartition = findToolButtonByName(&panel, QStringLiteral("Quick Partition"));
    auto* copyDisk = findToolButtonByName(&panel, QStringLiteral("Copy Disk Wizard"));
    auto* copyPartition = findToolButtonByName(&panel, QStringLiteral("Copy Partition Wizard"));
    auto* resize = findToolButtonByName(&panel, QStringLiteral("Resize/Move Partition"));
    auto* create = findToolButtonByName(&panel, QStringLiteral("Create Partition"));
    auto* allocate = findToolButtonByName(&panel, QStringLiteral("Allocate Free Space"));
    auto* manageBitLocker = findToolButtonByName(&panel, QStringLiteral("Manage BitLocker"));
    auto* dataRecovery = findToolButtonByName(&panel, QStringLiteral("Data Recovery"));
    auto* properties = findToolButtonByName(&panel, QStringLiteral("Properties"));
    QVERIFY(quickPartition && copyDisk && copyPartition && resize && create && allocate &&
            manageBitLocker && dataRecovery && properties);

    QVERIFY(!quickPartition->isEnabled());
    QVERIFY(!copyDisk->isEnabled());
    QVERIFY(!resize->isEnabled());
    QVERIFY(!create->isEnabled());
    QVERIFY(!dataRecovery->isEnabled());
    QVERIFY(!properties->isEnabled());

    table->selectRow(0);
    QApplication::processEvents();
    QVERIFY(quickPartition->isEnabled());
    QVERIFY(copyDisk->isEnabled());
    QVERIFY(dataRecovery->isEnabled());
    QVERIFY(properties->isEnabled());
    QVERIFY(!copyPartition->isEnabled());
    QVERIFY(!resize->isEnabled());
    QVERIFY(!create->isEnabled());
    QVERIFY(!manageBitLocker->isEnabled());

    table->selectRow(1);
    QApplication::processEvents();
    QVERIFY(copyPartition->isEnabled());
    QVERIFY(resize->isEnabled());
    QVERIFY(manageBitLocker->isEnabled());
    QVERIFY(dataRecovery->isEnabled());
    QVERIFY(properties->isEnabled());
    QVERIFY(!quickPartition->isEnabled());
    QVERIFY(!copyDisk->isEnabled());
    QVERIFY(!create->isEnabled());

    table->selectRow(table->rowCount() - 1);
    QApplication::processEvents();
    QVERIFY(create->isEnabled());
    QVERIFY(allocate->isEnabled());
    QVERIFY(dataRecovery->isEnabled());
    QVERIFY(properties->isEnabled());
    QVERIFY(!quickPartition->isEnabled());
    QVERIFY(!copyPartition->isEnabled());
    QVERIFY(!resize->isEnabled());
}

void PartitionManagerPanelTests::nonNativeFilesystemActionsExposeReadOnlyHfsCheck() {
    sak::PartitionManagerPanel panel;
    configureRawHfsPanel(&panel);
    panel.resize(900, 640);
    panel.show();
    QVERIFY(QTest::qWaitForWindowExposed(&panel));
    QApplication::processEvents();
    verifyRawHfsSidebarControls(&panel);
    verifyRawHfsInspectDialog(&panel);
    verifyRawHfsContextMenu(&panel);
}

void PartitionManagerPanelTests::partitionOperationsScrollInsideGroup() {
    sak::PartitionManagerPanel panel;

    const auto scrollAreas = panel.findChildren<QScrollArea*>();
    const auto hasOperationsScroll =
        std::any_of(scrollAreas.cbegin(), scrollAreas.cend(), [](const QScrollArea* scroll) {
            return scroll->accessibleName() == QStringLiteral("Partition operations") &&
                   scroll->widgetResizable() && scroll->frameShape() == QFrame::NoFrame;
        });
    QVERIFY(hasOperationsScroll);
    const auto hasWholePaneScroll =
        std::any_of(scrollAreas.cbegin(), scrollAreas.cend(), [](const QScrollArea* scroll) {
            return scroll->accessibleName() == QStringLiteral("Partition actions");
        });
    QVERIFY(!hasWholePaneScroll);
}

void PartitionManagerPanelTests::diskMapLegendContainsCommercialColorRoles() {
    sak::PartitionManagerPanel panel;

    const QStringList expectedRoles{QStringLiteral("GPT/Primary"),
                                    QStringLiteral("Logical"),
                                    QStringLiteral("Simple"),
                                    QStringLiteral("Spanned"),
                                    QStringLiteral("Striped"),
                                    QStringLiteral("Mirrored"),
                                    QStringLiteral("RAID5"),
                                    QStringLiteral("Unallocated")};
    const auto swatches = panel.findChildren<QFrame*>(QStringLiteral("partitionLegendSwatch"));
    QStringList actualRoles;
    for (auto* swatch : swatches) {
        actualRoles.append(swatch->property("colorRole").toString());
    }
    for (const auto& role : expectedRoles) {
        QVERIFY2(actualRoles.contains(role),
                 qPrintable(QStringLiteral("Missing partition legend role: %1").arg(role)));
    }
}

void PartitionManagerPanelTests::diskMapLegendColorsMatchRenderedRoles() {
    sak::PartitionManagerPanel panel;
    panel.setTestInventoryForReview(allColorRolesInventoryFixture());
    panel.resize(900, 640);
    panel.show();
    QVERIFY(QTest::qWaitForWindowExposed(&panel));
    QApplication::processEvents();

    QHash<QString, QString> legendColors;
    const auto swatches = panel.findChildren<QFrame*>(QStringLiteral("partitionLegendSwatch"));
    for (auto* swatch : swatches) {
        const QString role = swatch->property("colorRole").toString();
        const QColor expectedColor(swatch->property("colorValue").toString());
        const QImage image = swatch->grab().toImage();
        const QColor renderedColor =
            averageColor(image, QRect(3, 3, image.width() - 6, image.height() - 6));
        QVERIFY2(colorDistance(renderedColor, expectedColor) < 40,
                 qPrintable(
                     QStringLiteral("Legend swatch for %1 is not visibly painted").arg(role)));
        legendColors.insert(role, expectedColor.name());
    }

    QHash<QString, QString> segmentColors;
    const auto segments = panel.findChildren<QWidget*>(QStringLiteral("partitionDiskMapSegment"));
    for (auto* segment : segments) {
        const QString role = segment->property("innerColorRole").toString();
        const QString colorName = segment->property("innerColorValue").toString();
        const QColor expectedColor(colorName);
        const QImage image = segment->grab().toImage();
        const QRect sampleRect((image.width() * 3) / 4, sak::ui::kMarginSmall + 4, 8, 8);
        const QColor renderedColor = averageColor(image, sampleRect);
        QVERIFY2(colorDistance(renderedColor,
                               expectedColor.lighter(kRenderedSegmentFillLightness)) < 140,
                 qPrintable(
                     QStringLiteral("Disk-map segment for %1 is not visibly colored").arg(role)));
        segmentColors.insert(role, colorName);
        QCOMPARE(segment->property("outerColorRole").toString(), QStringLiteral("Neutral"));
    }

    const QStringList expectedRoles{QStringLiteral("GPT/Primary"),
                                    QStringLiteral("Logical"),
                                    QStringLiteral("Simple"),
                                    QStringLiteral("Spanned"),
                                    QStringLiteral("Striped"),
                                    QStringLiteral("Mirrored"),
                                    QStringLiteral("RAID5"),
                                    QStringLiteral("Unallocated")};
    for (const auto& role : expectedRoles) {
        QVERIFY2(segmentColors.contains(role),
                 qPrintable(QStringLiteral("Missing rendered disk-map role: %1").arg(role)));
        QCOMPARE(segmentColors.value(role), legendColors.value(role));
    }
}

void PartitionManagerPanelTests::unallocatedRoleUsesDarkGrayPalette() {
    sak::PartitionManagerPanel panel;
    panel.setTestInventoryForReview(allColorRolesInventoryFixture());
    panel.resize(900, 640);
    panel.show();
    QVERIFY(QTest::qWaitForWindowExposed(&panel));
    QApplication::processEvents();

    QFrame* unallocatedSwatch = nullptr;
    const auto swatches = panel.findChildren<QFrame*>(QStringLiteral("partitionLegendSwatch"));
    for (auto* swatch : swatches) {
        if (swatch->property("colorRole").toString() == QStringLiteral("Unallocated")) {
            unallocatedSwatch = swatch;
            break;
        }
    }
    QVERIFY2(unallocatedSwatch != nullptr, "Unallocated legend swatch should exist");
    const QColor unallocatedColor(unallocatedSwatch->property("colorValue").toString());
    QVERIFY2(unallocatedColor.lightness() < 110, "Unallocated should be dark gray, not white");
    const QImage swatchImage = unallocatedSwatch->grab().toImage();
    const QColor renderedSwatch =
        averageColor(swatchImage, QRect(3, 3, swatchImage.width() - 6, swatchImage.height() - 6));
    QVERIFY2(renderedSwatch.lightness() < 130,
             "Rendered Unallocated swatch should be dark gray, not white");

    const auto segments = panel.findChildren<QWidget*>(QStringLiteral("partitionDiskMapSegment"));
    const auto it = std::find_if(segments.cbegin(), segments.cend(), [](const QWidget* segment) {
        return segment->property("innerColorRole").toString() == QStringLiteral("Unallocated");
    });
    QVERIFY2(it != segments.cend(), "Unallocated disk-map segment should render");
    QCOMPARE((*it)->property("innerColorValue").toString(),
             unallocatedSwatch->property("colorValue").toString());
}

void PartitionManagerPanelTests::ribbonButtonsUseIcons8SvgSources() {
    // Run under the production tool-button style sheet: its padding is exactly
    // what fixed-width font-metrics sizing used to undercount, eliding wider
    // captions such as "Scan Disks".
    const QString previousStyleSheet = qApp->styleSheet();
    const auto restoreStyleSheet =
        qScopeGuard([previousStyleSheet] { qApp->setStyleSheet(previousStyleSheet); });
    qApp->setStyleSheet(sak::ui::actionButtonStyle("QPushButton, QToolButton",
                                                   sak::ui::kPrimaryButtonTone,
                                                   false,
                                                   sak::ui::kButtonPaddingCompactCss));

    sak::PartitionManagerPanel panel;
    panel.show();
    QVERIFY(QTest::qWaitForWindowExposed(&panel));

    const QHash<QString, QString> expectedIcons{
        {QStringLiteral("Apply"), QStringLiteral(":/icons/icons/icons8-pm-apply.svg")},
        {QStringLiteral("Discard"), QStringLiteral(":/icons/icons/icons8-pm-discard.svg")},
        {QStringLiteral("Undo"), QStringLiteral(":/icons/icons/icons8-pm-undo.svg")},
        {QStringLiteral("Redo"), QStringLiteral(":/icons/icons/icons8-pm-redo.svg")},
        {QStringLiteral("Scan Disks"), QStringLiteral(":/icons/icons/icons8-pm-refresh.svg")},
        {QStringLiteral("Dry Run"), QStringLiteral(":/icons/icons/icons8-pm-dry-run.svg")},
    };

    for (auto it = expectedIcons.cbegin(); it != expectedIcons.cend(); ++it) {
        const auto* button = findToolButtonByName(&panel, it.key());
        QVERIFY2(button != nullptr,
                 qPrintable(QStringLiteral("Missing ribbon button: %1").arg(it.key())));
        QCOMPARE(button->property("iconSource").toString(), QStringLiteral("Icons8 SVG"));
        QCOMPARE(button->property("iconPath").toString(), it.value());
        QFile iconFile(it.value());
        QVERIFY2(iconFile.open(QIODevice::ReadOnly),
                 qPrintable(QStringLiteral("Ribbon SVG resource missing: %1").arg(it.value())));
        const QByteArray iconBytes = iconFile.readAll();
        QVERIFY(iconBytes.contains("<svg"));
        QVERIFY(iconBytes.contains("viewBox"));
        QCOMPARE(button->property("iconModes").toString(),
                 QStringLiteral("Normal,Disabled,Active,Selected"));
        const auto disabledIconSize =
            button->icon().actualSize(QSize(30, 30), QIcon::Disabled, QIcon::Off);
        QVERIFY2(disabledIconSize.width() >= 24 && disabledIconSize.height() >= 24,
                 "Disabled ribbon icons should still come from crisp SVG resources");
        // Truncation-proof sizing: the layout hint must come from the active
        // style (which knows the style-sheet padding and current font), the
        // minimum hint must equal it so layouts can never shrink the button
        // below it, and the realized geometry must grant the full hint.
        QCOMPARE(button->minimumSizeHint(), button->sizeHint());
        QVERIFY2(button->sizeHint().height() >= 64,
                 "Ribbon buttons should have one consistent commercial-toolbar height");
        QVERIFY2(button->sizeHint().width() > button->fontMetrics().horizontalAdvance(it.key()),
                 qPrintable(QStringLiteral("Ribbon hint must exceed the bare caption width: %1")
                                .arg(it.key())));
        QVERIFY2(button->width() >= button->sizeHint().width() &&
                     button->height() >= button->sizeHint().height(),
                 qPrintable(
                     QStringLiteral("Ribbon label must not be truncated: %1").arg(it.key())));
    }
}

void PartitionManagerPanelTests::diskMapUsesCompactSpacing() {
    sak::PartitionManagerPanel panel;
    panel.setTestInventoryForReview(applyReviewInventoryFixture());

    auto* pane = panel.findChild<QWidget*>(QStringLiteral("partitionDiskMapPane"));
    QVERIFY2(pane != nullptr, "Disk map pane should exist");
    auto* paneFrame = qobject_cast<QFrame*>(pane);
    QVERIFY2(paneFrame == nullptr || paneFrame->frameShape() == QFrame::NoFrame,
             "Disk map should not add an extra framed box around the map");

    const auto scrollAreas = panel.findChildren<QScrollArea*>();
    const auto diskMapScrollIt =
        std::find_if(scrollAreas.cbegin(), scrollAreas.cend(), [](const QScrollArea* scroll) {
            return scroll->accessibleName() == QStringLiteral("Partition disk map");
        });
    QVERIFY2(diskMapScrollIt != scrollAreas.cend(), "Disk map scroll area should exist");
    auto* diskMapScroll = *diskMapScrollIt;
    QVERIFY2(diskMapScroll != nullptr, "Disk map scroll area should exist");
    QVERIFY(diskMapScroll->widgetResizable());
    QCOMPARE(diskMapScroll->frameShape(), QFrame::NoFrame);

    auto* mapLayout = qobject_cast<QVBoxLayout*>(diskMapScroll->widget()->layout());
    QVERIFY2(mapLayout != nullptr, "Disk map container should use a vertical layout");
    const QMargins margins = mapLayout->contentsMargins();
    QVERIFY2(margins.left() <= 1 && margins.top() <= 1 && margins.right() <= 1 &&
                 margins.bottom() <= 1,
             "Disk map should keep only a very small outer margin");
    QVERIFY2(mapLayout->spacing() <= 2, "Disk map rows should have a compact gap");

    // Re-homed off the mirror `cornerRadius` dynamic property (a pure test handle). The row and
    // tile paintEvents round with the kDiskMapRowRadius constant directly via addRoundedRect; the
    // property only shadows that constant, so it could be renamed, removed, or left stale while
    // the widget still rounds -- and, worse, it would keep passing if the addRoundedRect were ever
    // replaced by a square fill. Assert the OBSERVABLE rounding via rendersRoundedCorners (renders
    // the widget's own fill onto a contrasting ground and proves the extreme corner is left
    // unpainted while the interior is filled). Strictly stronger and refactor-robust.
    auto* row = panel.findChild<QWidget*>(QStringLiteral("partitionDiskMapRow"));
    QVERIFY2(row != nullptr, "Disk map should render disk rows");
    QVERIFY2(rendersRoundedCorners(row),
             "Disk map row should paint a rounded fill (corner clipped, interior filled)");
    QCOMPARE(row->contextMenuPolicy(), Qt::CustomContextMenu);
    auto* diskTile = panel.findChild<QWidget*>(QStringLiteral("partitionDiskMapDiskTile"));
    QVERIFY2(diskTile != nullptr, "Disk map should render disk tiles");
    QVERIFY2(rendersRoundedCorners(diskTile),
             "Disk tile should paint a rounded fill (corner clipped, interior filled)");
    QCOMPARE(diskTile->contextMenuPolicy(), Qt::CustomContextMenu);
    auto* segment = panel.findChild<QWidget*>(QStringLiteral("partitionDiskMapSegment"));
    QVERIFY2(segment != nullptr, "Disk map should render partition segments");
    QCOMPARE(segment->contextMenuPolicy(), Qt::CustomContextMenu);
    QCOMPARE(segment->minimumHeight(), diskTile->minimumHeight());
    QCOMPARE(segment->sizeHint().height(), diskTile->minimumHeight());
}

void PartitionManagerPanelTests::diskMapHighlightsOnlySelectedPartition() {
    sak::PartitionManagerPanel panel;
    panel.setTestInventoryForReview(applyReviewInventoryFixture());

    auto selectedSegments = panel.findChildren<QWidget*>(QStringLiteral("partitionDiskMapSegment"));
    QVERIFY2(!selectedSegments.isEmpty(), "Disk map should render partition segments");
    const auto selectedBefore =
        std::count_if(selectedSegments.cbegin(), selectedSegments.cend(), [](const QWidget* item) {
            return item->property("selected").toBool();
        });
    QCOMPARE(selectedBefore, 0);
    QCOMPARE(selectedSegments.first()->property("colorRole").toString(),
             QStringLiteral("GPT/Primary"));
    QCOMPARE(selectedSegments.first()->property("outerColorRole").toString(),
             QStringLiteral("Neutral"));
    QCOMPARE(selectedSegments.first()->property("innerColorRole").toString(),
             QStringLiteral("GPT/Primary"));

    auto* table = panel.findChild<QTableWidget*>();
    QVERIFY2(table != nullptr, "Partition table should exist");
    // The fixture is exactly one disk with one partition and no unallocated regions, which
    // rebuildTable maps 1:1 to a disk row + a partition row = 2. `>= 2` would miss a spurious
    // extra/phantom row.
    QCOMPARE(table->rowCount(), 2);
    table->selectRow(1);
    QApplication::processEvents();
    flushDeferredDeletes();

    selectedSegments = panel.findChildren<QWidget*>(QStringLiteral("partitionDiskMapSegment"));
    const auto selectedAfter =
        std::count_if(selectedSegments.cbegin(), selectedSegments.cend(), [](const QWidget* item) {
            return item->property("selected").toBool();
        });
    QCOMPARE(selectedAfter, 1);
}

void PartitionManagerPanelTests::diskMapHighlightsSelectedDiskRow() {
    sak::PartitionManagerPanel panel;
    panel.setTestInventoryForReview(applyReviewInventoryFixture());

    auto* table = panel.findChild<QTableWidget*>();
    QVERIFY2(table != nullptr, "Partition table should exist");

    auto* row = panel.findChild<QWidget*>(QStringLiteral("partitionDiskMapRow"));
    auto* tile = panel.findChild<QWidget*>(QStringLiteral("partitionDiskMapDiskTile"));
    QVERIFY2(row != nullptr, "Disk map row should exist");
    QVERIFY2(tile != nullptr, "Disk tile should exist");
    QVERIFY(!row->property("selected").toBool());
    QVERIFY(!tile->property("selected").toBool());

    table->selectRow(0);
    QApplication::processEvents();
    flushDeferredDeletes();

    row = panel.findChild<QWidget*>(QStringLiteral("partitionDiskMapRow"));
    tile = panel.findChild<QWidget*>(QStringLiteral("partitionDiskMapDiskTile"));
    QVERIFY(row->property("selected").toBool());
    QVERIFY(tile->property("selected").toBool());
    QCOMPARE(row->property("selectedColorRole").toString(), QStringLiteral("GPT/Primary"));

    const auto selectedSegments =
        panel.findChildren<QWidget*>(QStringLiteral("partitionDiskMapSegment"));
    const auto selectedSegmentCount =
        std::count_if(selectedSegments.cbegin(), selectedSegments.cend(), [](const QWidget* item) {
            return item->property("selected").toBool();
        });
    QCOMPARE(selectedSegmentCount, 0);
}

void PartitionManagerPanelTests::diskMapContextMenuSelectsMatchingTargets() {
    sak::PartitionManagerPanel panel;
    panel.setTestInventoryForReview(applyReviewInventoryFixture());
    panel.resize(900, 640);
    panel.show();
    QVERIFY(QTest::qWaitForWindowExposed(&panel));
    QApplication::processEvents();

    auto* table = panel.findChild<QTableWidget*>();
    auto* diskTile = panel.findChild<QWidget*>(QStringLiteral("partitionDiskMapDiskTile"));
    QVERIFY2(table != nullptr, "Partition table should exist");
    QVERIFY2(diskTile != nullptr, "Disk-map disk tile should exist");

    sendContextMenu(diskTile);
    QCOMPARE(table->currentRow(), 0);

    auto* segment = panel.findChild<QWidget*>(QStringLiteral("partitionDiskMapSegment"));
    QVERIFY2(segment != nullptr, "Disk-map partition segment should exist");
    sendContextMenu(segment);
    QCOMPARE(table->currentRow(), 1);
}

void PartitionManagerPanelTests::contextMenuOmitsRibbonAndQueueControls() {
    sak::PartitionManagerPanel panel;
    panel.setTestInventoryForReview(applyReviewInventoryFixture());
    panel.resize(900, 640);
    panel.show();
    QVERIFY(QTest::qWaitForWindowExposed(&panel));
    QApplication::processEvents();

    auto* diskTile = panel.findChild<QWidget*>(QStringLiteral("partitionDiskMapDiskTile"));
    QVERIFY2(diskTile != nullptr, "Disk-map disk tile should exist");
    const QStringList actions = contextMenuActionTexts(diskTile);
    QVERIFY2(actions.contains(QStringLiteral("Migrate OS to SSD/HDD Wizard")),
             "Target menu should still contain disk actions");

    const QStringList forbidden{QStringLiteral("Scan Disks"),
                                QStringLiteral("Refresh Disks"),
                                QStringLiteral("Apply Pending Changes"),
                                QStringLiteral("Dry Run Pending Changes"),
                                QStringLiteral("Cancel Running Operation"),
                                QStringLiteral("Undo"),
                                QStringLiteral("Redo"),
                                QStringLiteral("Discard")};
    for (const auto& text : forbidden) {
        QVERIFY2(!actions.contains(text),
                 qPrintable(QStringLiteral("Context menu should not contain %1").arg(text)));
    }
}

void PartitionManagerPanelTests::diskMapSegmentsSelectMatchingTableRows() {
    sak::PartitionManagerPanel panel;
    panel.setTestInventoryForReview(applyReviewInventoryFixture());
    panel.resize(900, 640);
    panel.show();
    QVERIFY(QTest::qWaitForWindowExposed(&panel));
    QApplication::processEvents();

    auto* table = panel.findChild<QTableWidget*>();
    QVERIFY2(table != nullptr, "Partition table should exist");
    QCOMPARE(table->currentRow(), -1);

    auto* segment = panel.findChild<QWidget*>(QStringLiteral("partitionDiskMapSegment"));
    QVERIFY2(segment != nullptr, "Disk map should render a partition segment");
    const QPoint segmentCenter = segment->rect().center();
    sendMouse(segment, QEvent::MouseButtonPress, segmentCenter, Qt::LeftButton, Qt::LeftButton);
    sendMouse(segment, QEvent::MouseButtonRelease, segmentCenter, Qt::LeftButton, Qt::NoButton);
    QApplication::processEvents();
    flushDeferredDeletes();
    QCOMPARE(table->currentRow(), 1);

    auto selectedSegments = panel.findChildren<QWidget*>(QStringLiteral("partitionDiskMapSegment"));
    const auto selectedAfterMapClick =
        std::count_if(selectedSegments.cbegin(), selectedSegments.cend(), [](const QWidget* item) {
            return item->property("selected").toBool();
        });
    QCOMPARE(selectedAfterMapClick, 1);

    table->selectRow(0);
    QApplication::processEvents();
    flushDeferredDeletes();
    selectedSegments = panel.findChildren<QWidget*>(QStringLiteral("partitionDiskMapSegment"));
    const auto selectedAfterDiskRow =
        std::count_if(selectedSegments.cbegin(), selectedSegments.cend(), [](const QWidget* item) {
            return item->property("selected").toBool();
        });
    QCOMPARE(selectedAfterDiskRow, 0);
}

void PartitionManagerPanelTests::redoButtonEnablesOnlyAfterUndo() {
    sak::PartitionManagerPanel panel;
    panel.setTestInventoryForReview(applyReviewInventoryFixture());

    auto* undo = findToolButtonByName(&panel, QStringLiteral("Undo"));
    auto* redo = findToolButtonByName(&panel, QStringLiteral("Redo"));
    QVERIFY2(undo != nullptr, "Undo ribbon button should exist");
    QVERIFY2(redo != nullptr, "Redo ribbon button should exist");
    QVERIFY(!redo->isEnabled());

    sak::PartitionTarget target;
    target.kind = sak::PartitionTargetKind::Disk;
    target.disk_number = sak::DiskNumber{0};
    panel.queueTestOperationForReview(sak::PartitionOperationType::OptimizeSsd, target);
    QApplication::processEvents();
    QVERIFY(undo->isEnabled());
    QVERIFY(!redo->isEnabled());

    undo->click();
    QApplication::processEvents();
    QVERIFY(!undo->isEnabled());
    QVERIFY(redo->isEnabled());

    redo->click();
    QApplication::processEvents();
    QVERIFY(undo->isEnabled());
    QVERIFY(!redo->isEnabled());
}

void PartitionManagerPanelTests::diskMapRendersTypeColorInsideNeutralShell() {
    sak::PartitionManagerPanel panel;
    panel.setTestInventoryForReview(applyReviewInventoryFixture());
    panel.resize(900, 640);
    panel.show();
    QVERIFY(QTest::qWaitForWindowExposed(&panel));
    QApplication::processEvents();

    auto* segment = panel.findChild<QWidget*>(QStringLiteral("partitionDiskMapSegment"));
    QVERIFY2(segment != nullptr, "Disk map should render a partition segment");
    const QImage image = segment->grab().toImage();
    QVERIFY2(image.width() > 30 && image.height() > 30,
             "Rendered segment should have measurable pixels");

    const QColor inner = averageColor(image,
                                      QRect(image.width() / 4, sak::ui::kMarginSmall + 4, 8, 8));
    const QColor outer = averageColor(image, QRect(4, image.height() / 2, 6, 6));
    QVERIFY2(chroma(inner) > chroma(outer) + 30,
             "Partition type color should be in the inner usage bar, not the neutral shell");
    QCOMPARE(segment->property("outerColorRole").toString(), QStringLiteral("Neutral"));
    QCOMPARE(segment->property("innerColorRole").toString(), QStringLiteral("GPT/Primary"));
}

void PartitionManagerPanelTests::bottomDiskMapCanResizeIntoTableSpace() {
    sak::PartitionManagerPanel panel;

    const auto splitters = panel.findChildren<QSplitter*>();
    const auto hasVerticalWorkspaceSplitter =
        std::any_of(splitters.cbegin(), splitters.cend(), [](const QSplitter* splitter) {
            return splitter->orientation() == Qt::Vertical && splitter->count() >= 2 &&
                   !splitter->childrenCollapsible();
        });
    QVERIFY(hasVerticalWorkspaceSplitter);
}

void PartitionManagerPanelTests::finalApplyReviewContainsLayoutDiff() {
    sak::PartitionManagerPanel panel;
    panel.setTestInventoryForReview(applyReviewInventoryFixture());
    sak::PartitionTarget target;
    target.kind = sak::PartitionTargetKind::Disk;
    target.disk_number = sak::DiskNumber{0};
    panel.queueTestOperationForReview(sak::PartitionOperationType::SurfaceTest, target);

    bool inspected = false;
    QTimer::singleShot(0, [&]() {
        auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
        QVERIFY2(dialog != nullptr, "Apply review dialog should open");
        auto* preview =
            dialog->findChild<QWidget*>(QStringLiteral("partitionApplyLayoutDiffPreview"));
        QVERIFY2(preview != nullptr,
                 "Final Apply review should contain before and after layout diff");
        QCOMPARE(preview->accessibleName(),
                 QStringLiteral("Queued partition layout before and after preview"));
        QVERIFY(preview->minimumHeight() > 0);
        inspected = true;
        dialog->reject();
    });

    QVERIFY(!panel.showApplyReviewDialogForTest());
    QVERIFY(inspected);
}

void PartitionManagerPanelTests::propertiesActionIsFirstClass() {
    sak::PartitionManagerPanel panel;

    const auto buttons = panel.findChildren<QToolButton*>();
    QVERIFY2(hasActionButton(
                 buttons, QStringLiteral("Properties"), QStringLiteral("Show selected properties")),
             "Properties should be a real sidebar action");
    QVERIFY2(hasActionButton(
                 buttons, QStringLiteral("Explore Partition"), QStringLiteral("Open in Explorer")),
             "Explore should be a real sidebar action");
    QVERIFY2(hasActionButton(buttons,
                             QStringLiteral("Data Recovery"),
                             QStringLiteral("Standalone tool: scan an image or raw volume and "
                                            "restore found files now (runs immediately, not added "
                                            "to the queue)")),
             "Data Recovery should expose image and raw path recovery");
    QVERIFY2(hasActionButton(buttons,
                             QStringLiteral("Disk Benchmark"),
                             QStringLiteral("Open Benchmark and Diagnostics")),
             "Disk Benchmark should route to the existing benchmark panel");
    QVERIFY2(hasActionButton(buttons,
                             QStringLiteral("Make Bootable Media"),
                             QStringLiteral("Open Image Flasher")),
             "Bootable media should route to Image Flasher");
    QVERIFY2(hasActionButton(buttons,
                             QStringLiteral("Space Analyzer"),
                             QStringLiteral("Analyze tree, file, and file-type usage")),
             "Space Analyzer should be a read-only action");
    QVERIFY2(hasActionButton(buttons,
                             QStringLiteral("Quick Partition"),
                             QStringLiteral("Queue custom or equal-size disk layout")),
             "Quick Partition should be a real queued disk-layout action");
    QVERIFY2(hasActionButton(buttons,
                             QStringLiteral("Extend Partition Wizard"),
                             QStringLiteral("Extend into adjacent free space")),
             "Extend Partition Wizard should be a queued resize path");
    QVERIFY2(hasActionButton(buttons,
                             QStringLiteral("Manage BitLocker"),
                             QStringLiteral("Review BitLocker status and open Windows management")),
             "Manage BitLocker should show in-app status before Windows management");
    QVERIFY2(hasActionButton(buttons,
                             QStringLiteral("Disk Defrag"),
                             QStringLiteral("Review defrag/ReTrim commands and open Windows "
                                            "Optimize Drives")),
             "Disk Defrag should show in-app guidance before Windows Optimize Drives");
    QVERIFY2(hasActionButton(buttons,
                             QStringLiteral("SSD Secure Erase"),
                             QStringLiteral("Queue SSD/NVMe ReTrim plus clear-level wipe")),
             "SSD Secure Erase should expose a queued ReTrim and wipe path");
}

void PartitionManagerPanelTests::propertiesDialogShowsRawFilesystemMetadata() {
    sak::PartitionManagerPanel panel;
    auto inventory = applyReviewInventoryFixture();
    auto& volume = inventory.disks[0].partitions[0].volume.value();
    volume.file_system = QStringLiteral("ext4");
    volume.file_system_source = sak::PartitionFileSystemDetector::rawSignatureSource();
    volume.file_system_details = {QStringLiteral("Block size: 4096"),
                                  QStringLiteral("Total blocks: 2048"),
                                  QStringLiteral("Volume label: SAK_EXT4")};
    panel.setTestInventoryForReview(inventory);

    auto* table = panel.findChild<QTableWidget*>();
    QVERIFY2(table != nullptr, "Partition table should exist");
    table->selectRow(1);
    auto* fileSystem = table->item(1, 1);
    QVERIFY2(fileSystem != nullptr, "File system cell should exist");
    QVERIFY(fileSystem->toolTip().contains(QStringLiteral("Block size: 4096")));
    auto* browseButton = findToolButtonByName(&panel,
                                              QStringLiteral("Browse Non-Windows File System"));
    QVERIFY2(browseButton != nullptr, "Non-native filesystem browse action should exist");
    QVERIFY(browseButton->isEnabled());
    QCOMPARE(browseButton->toolTip(), QStringLiteral("Browse read-only ext4 directory entries"));

    bool inspected = false;
    QTimer::singleShot(0, [&]() {
        auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
        QVERIFY2(dialog != nullptr, "Properties dialog should open");
        auto* properties =
            dialog->findChild<QTableWidget*>(QStringLiteral("partitionPropertiesTable"));
        QVERIFY2(properties != nullptr, "Properties table should exist");
        const QString metadata = propertyTableValue(properties,
                                                    QStringLiteral("File system metadata"));
        // All three probe lines, in order. The two contains() checks skipped over the middle
        // line entirely, so a dropped or reordered detail was invisible.
        QCOMPARE(metadata,
                 QStringLiteral("Block size: 4096\nTotal blocks: 2048\nVolume label: SAK_EXT4"));
        inspected = true;
        dialog->reject();
    });

    auto* propertiesButton = findToolButtonByName(&panel, QStringLiteral("Properties"));
    QVERIFY2(propertiesButton != nullptr, "Properties action should exist");
    propertiesButton->click();
    QVERIFY(inspected);
}

void PartitionManagerPanelTests::propertiesAndInspectShowRawFilesystemSanityNotes() {
    sak::PartitionManagerPanel panel;
    configureRawMetadataPanel(
        &panel,
        QStringLiteral("XFS"),
        {QStringLiteral("Block size: 4096"),
         QStringLiteral("Data blocks: 32768"),
         QStringLiteral("Metadata sanity: XFS superblock geometry is internally consistent")});

    auto* table = panel.findChild<QTableWidget*>();
    QVERIFY2(table != nullptr, "Partition table should exist");
    auto* fileSystem = table->item(1, 1);
    QVERIFY2(fileSystem != nullptr, "File system cell should exist");
    QVERIFY(fileSystem->toolTip().contains(QStringLiteral("Metadata sanity: XFS")));

    auto* checkButton = findToolButtonByName(&panel,
                                             QStringLiteral("Check Non-Windows File System"));
    QVERIFY2(checkButton != nullptr, "Non-native filesystem check action should exist");
    QVERIFY(checkButton->isEnabled());
    // The tooltip is built by .arg(file_system) from one shared template, so the two fragments
    // held even if the WRONG filesystem name was interpolated into an XFS partition's action.
    QCOMPARE(checkButton->toolTip(),
             QStringLiteral("Run original read-only XFS metadata consistency check from captured "
                            "probe data"));
    verifyMetadataCheckDialog(checkButton,
                              QStringLiteral("Metadata sanity: XFS superblock geometry is "
                                             "internally consistent"));
    verifyXfsPropertiesAndInspect(&panel);

    sak::PartitionManagerPanel apfsPanel;
    configureRawMetadataPanel(
        &apfsPanel,
        QStringLiteral("APFS"),
        {QStringLiteral("Block size: 4096"),
         QStringLiteral("Container blocks: 4096"),
         QStringLiteral(
             "Metadata sanity: APFS container block geometry is internally consistent")});
    auto* apfsCheck = findToolButtonByName(&apfsPanel,
                                           QStringLiteral("Check Non-Windows File System"));
    QVERIFY2(apfsCheck != nullptr, "APFS non-native filesystem check action should exist");
    QVERIFY(apfsCheck->isEnabled());
    QCOMPARE(apfsCheck->toolTip(),
             QStringLiteral("Run original read-only APFS metadata consistency check from captured "
                            "probe data"));
    verifyMetadataCheckDialog(apfsCheck,
                              QStringLiteral("Metadata sanity: APFS container block geometry is "
                                             "internally consistent"));
}

void PartitionManagerPanelTests::extFilesystemWriteActionsQueueWithConfirmation() const {
    queueExtFormatAndVerify();
    queueLinuxSwapFormatAndVerify();
    queueApfsFormatAndVerify();
    queueExtRepairAndVerify();
    queueHfsRepairAndVerify();
    queueGeneratedApfsRepairAndVerify();
    queueExtResizeAndVerify(true);
    queueExtResizeAndVerify(false);
}

void PartitionManagerPanelTests::apfsContainerActionAllowsWritableApfsVolumes() {
    queueApfsContainerSnapshotAndVerify();
    queueApfsVolumeLabelChangeAndVerify();

    sak::PartitionManagerPanel panel;
    auto inventory = applyReviewInventoryFixture();
    setRawFileSystem(
        &inventory.disks[0].partitions[0].volume.value(),
        QStringLiteral("APFS"),
        {QStringLiteral(
            "Metadata sanity: APFS container block geometry is internally consistent")});
    panel.setTestInventoryForReview(inventory);
    auto* table = panel.findChild<QTableWidget*>();
    QVERIFY2(table != nullptr, "Partition table should exist");
    table->selectRow(1);
    QApplication::processEvents();

    // A non-generated (real Apple) APFS volume: snapshot/resize and rename are all COW-certified
    // for foreign containers (host apfsck + macOS-kernel mount/listSnapshots/fsck), so the APFS
    // Container action now enables for it too.
    auto* button = findToolButtonByName(&panel, QStringLiteral("APFS Container"));
    QVERIFY2(button != nullptr, "APFS Container action should exist");
    QVERIFY2(button->isEnabled(), "APFS Container should enable for a foreign APFS volume");
    QVERIFY(button->toolTip().contains(QStringLiteral("Queue an APFS container action")));

    auto* changeLabel = findToolButtonByName(&panel, QStringLiteral("Change Label"));
    QVERIFY2(changeLabel != nullptr, "Change Label action should exist");
    QVERIFY2(changeLabel->isEnabled(), "Change Label should enable for a foreign APFS volume");
    QVERIFY(changeLabel->toolTip().contains(QStringLiteral("Rename this APFS volume")));

    bool handled = false;
    QTimer::singleShot(0, [&]() {
        auto* dialog = qobject_cast<QInputDialog*>(QApplication::activeModalWidget());
        QVERIFY2(dialog != nullptr, "Change Label input dialog should open");
        dialog->setTextValue(QStringLiteral("ForeignRenamed"));
        handled = true;
        dialog->accept();
    });
    changeLabel->click();
    QVERIFY(handled);
    verifySingleQueuedOperation(&panel,
                                QStringLiteral("APFS Change Volume Label - Disk 0 Partition 1"));
}

void PartitionManagerPanelTests::manageBitLockerShowsStatusDialog() {
    sak::PartitionManagerPanel panel;
    auto inventory = applyReviewInventoryFixture();
    inventory.disks[0].model = QStringLiteral("Fixture SSD");
    inventory.disks[0].partitions[0].volume->volume_guid = QStringLiteral("volume-guid");
    inventory.disks[0].partitions[0].volume->bitlocker_enabled = true;
    inventory.disks[0].partitions[0].volume->bitlocker_locked = true;
    panel.setTestInventoryForReview(inventory);

    auto* table = panel.findChild<QTableWidget*>();
    QVERIFY2(table != nullptr, "Partition table should exist");
    table->selectRow(1);

    auto* button = findToolButtonByName(&panel, QStringLiteral("Manage BitLocker"));
    QVERIFY2(button != nullptr, "Manage BitLocker action should exist");

    bool inspected = false;
    QTimer::singleShot(0, [&]() {
        auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
        QVERIFY2(dialog != nullptr, "BitLocker dialog should open");
        QCOMPARE(dialog->objectName(), QStringLiteral("partitionBitLockerDialog"));
        QCOMPARE(dialog->accessibleName(), QStringLiteral("BitLocker management"));

        auto* status =
            dialog->findChild<QTableWidget*>(QStringLiteral("partitionBitLockerStatusTable"));
        QVERIFY2(status != nullptr, "BitLocker status table should exist");
        QCOMPARE(status->accessibleName(), QStringLiteral("BitLocker status table"));
        QCOMPARE(propertyTableValue(status, QStringLiteral("Protection")),
                 QStringLiteral("Protection on"));
        QCOMPARE(propertyTableValue(status, QStringLiteral("Lock state")),
                 QStringLiteral("Locked"));
        // These are commands an operator is invited to run by hand against an ENCRYPTED
        // volume, so the whole list matters: contains() on one line could not see the mount
        // point interpolated wrong (targeting a different drive), a command missing, or an
        // extra unvetted one appended. The recovery-password form is the dangerous one.
        QCOMPARE(propertyTableValue(status, QStringLiteral("Safe commands")),
                 QStringLiteral("manage-bde.exe -status T:\n"
                                "manage-bde.exe -protectors -disable T:\n"
                                "manage-bde.exe -protectors -enable T:\n"
                                "Suspend-BitLocker -MountPoint \"T:\" -RebootCount 1\n"
                                "Resume-BitLocker -MountPoint \"T:\"\n"
                                "Unlock-BitLocker -MountPoint \"T:\" -RecoveryPassword "
                                "\"<48-digit-recovery-key>\""));
        QCOMPARE(propertyTableValue(status, QStringLiteral("In-app mutation")),
                 QStringLiteral("Queue unlock, suspend, or resume through elevated Apply"));
        auto* unlockButton =
            findAccessibleWidget<QPushButton>(dialog, QStringLiteral("Queue BitLocker unlock"));
        QVERIFY(unlockButton != nullptr);
        QVERIFY(unlockButton->isEnabled());
        auto* suspendButton =
            findAccessibleWidget<QPushButton>(dialog, QStringLiteral("Queue BitLocker suspend"));
        QVERIFY(suspendButton != nullptr);
        QVERIFY(!suspendButton->isEnabled());
        QVERIFY(findAccessibleWidget<QPushButton>(
                    dialog, QStringLiteral("Copy BitLocker commands")) != nullptr);
        QVERIFY(findAccessibleWidget<QPushButton>(
                    dialog, QStringLiteral("Open Windows BitLocker management")) != nullptr);
        inspected = true;
        dialog->reject();
    });

    button->click();
    QVERIFY(inspected);
}

void PartitionManagerPanelTests::diskDefragShowsOptimizeDialog() {
    sak::PartitionManagerPanel panel;
    auto inventory = applyReviewInventoryFixture();
    inventory.disks[0].media_type = QStringLiteral("HDD");
    panel.setTestInventoryForReview(inventory);

    auto* table = panel.findChild<QTableWidget*>();
    QVERIFY2(table != nullptr, "Partition table should exist");
    table->selectRow(1);

    auto* button = findToolButtonByName(&panel, QStringLiteral("Disk Defrag"));
    QVERIFY2(button != nullptr, "Disk Defrag action should exist");

    bool inspected = false;
    QTimer::singleShot(0, [&]() {
        auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
        QVERIFY2(dialog != nullptr, "Optimize Drives dialog should open");
        QCOMPARE(dialog->objectName(), QStringLiteral("partitionOptimizeDrivesDialog"));
        QCOMPARE(dialog->accessibleName(), QStringLiteral("Disk defrag and optimization"));

        auto* status =
            dialog->findChild<QTableWidget*>(QStringLiteral("partitionOptimizeStatusTable"));
        QVERIFY2(status != nullptr, "Optimize status table should exist");
        QCOMPARE(status->accessibleName(), QStringLiteral("Disk optimization status table"));
        QCOMPARE(propertyTableValue(status, QStringLiteral("Optimization mode")),
                 QStringLiteral("HDD analyze and defrag"));
        // The command list is media-type dependent: an HDD gets Analyze + Defrag, an SSD gets
        // ReTrim instead. contains("-Defrag") passed even if the ReTrim line were ALSO listed
        // for this HDD -- which would invite an operator to trim a spinning disk.
        QCOMPARE(propertyTableValue(status, QStringLiteral("Safe commands")),
                 QStringLiteral("Optimize-Volume -DriveLetter T -Analyze -Verbose\n"
                                "Optimize-Volume -DriveLetter T -Defrag -Verbose"));
        QCOMPARE(propertyTableValue(status, QStringLiteral("In-app HDD defrag execution")),
                 QStringLiteral("Queue HDD defrag through cancellable elevated Apply"));
        auto* defragButton = findAccessibleWidget<QPushButton>(dialog,
                                                               QStringLiteral("Queue HDD defrag"));
        QVERIFY(defragButton != nullptr);
        QVERIFY(defragButton->isEnabled());
        auto* retrimButton = findAccessibleWidget<QPushButton>(dialog,
                                                               QStringLiteral("Queue SSD ReTrim"));
        QVERIFY(retrimButton != nullptr);
        QVERIFY(!retrimButton->isEnabled());
        QVERIFY(findAccessibleWidget<QPushButton>(
                    dialog, QStringLiteral("Copy Optimize commands")) != nullptr);
        QVERIFY(findAccessibleWidget<QPushButton>(
                    dialog, QStringLiteral("Open Windows Optimize Drives")) != nullptr);
        inspected = true;
        dialog->reject();
    });

    button->click();
    QVERIFY(inspected);
}

void PartitionManagerPanelTests::ssdSecureEraseShowsQueueDialog() {
    sak::PartitionManagerPanel panel;
    auto inventory = applyReviewInventoryFixture();
    inventory.disks[0].model = QStringLiteral("Fixture NVMe");
    inventory.disks[0].serial_number = QStringLiteral("SER123");
    inventory.disks[0].bus_type = QStringLiteral("NVMe");
    inventory.disks[0].media_type = QStringLiteral("SSD");
    panel.setTestInventoryForReview(inventory);

    auto* table = panel.findChild<QTableWidget*>();
    QVERIFY2(table != nullptr, "Partition table should exist");
    table->selectRow(0);

    auto* button = findToolButtonByName(&panel, QStringLiteral("SSD Secure Erase"));
    QVERIFY2(button != nullptr, "SSD Secure Erase action should exist");

    bool inspected = false;
    QTimer::singleShot(0, [&]() {
        auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
        QVERIFY2(dialog != nullptr, "SSD Secure Erase dialog should open");
        QCOMPARE(dialog->objectName(), QStringLiteral("partitionSecureEraseDialog"));
        QCOMPARE(dialog->accessibleName(), QStringLiteral("SSD Secure Erase readiness"));

        auto* status =
            dialog->findChild<QTableWidget*>(QStringLiteral("partitionSecureEraseStatusTable"));
        QVERIFY2(status != nullptr, "SSD Secure Erase readiness table should exist");
        QCOMPARE(status->accessibleName(), QStringLiteral("SSD Secure Erase readiness table"));
        QCOMPARE(propertyTableValue(status, QStringLiteral("Device class")),
                 QStringLiteral("NVMe SSD"));
        // "Ready" is a substring of the NOT-ready gate texts too, so it could not distinguish
        // a permitted erase from a refused one; pin the whole gate sentence.
        QCOMPARE(propertyTableValue(status, QStringLiteral("Secure erase status")),
                 QStringLiteral("Ready: queue ReTrim plus clear-level wipe through Apply"));
        QCOMPARE(propertyTableValue(status, QStringLiteral("In-app ATA/NVMe purge")),
                 QStringLiteral("Uses Windows ReTrim followed by clear-level disk wipe"));
        // The checklist is the evidence record for an IRREVERSIBLE purge. One contains() left
        // the target-identity line (disk number, model, serial -- the proof the right device
        // was wiped) and every other step unasserted.
        QCOMPARE(propertyTableValue(status, QStringLiteral("Evidence checklist")),
                 QStringLiteral(
                     "Target identity: Disk 0, model Fixture NVMe, serial SER123\n"
                     "Disposable non-system SSD/NVMe media only\n"
                     "Record bus type, firmware, SMART health, and wear before erase\n"
                     "Verify vendor ATA Secure Erase or NVMe Format/Sanitize support externally\n"
                     "Show purge warning and typed operator confirmation in evidence\n"
                     "Capture before/after layout and post-erase readback proof\n"
                     "Attach evidence to external.ssd-retrim or hardware wipe certification"));
        QVERIFY(findAccessibleWidget<QPushButton>(
                    dialog, QStringLiteral("Copy SSD Secure Erase evidence checklist")) != nullptr);
        QVERIFY(findAccessibleWidget<QPushButton>(
                    dialog, QStringLiteral("Queue SSD Secure Erase")) != nullptr);
        inspected = true;
        dialog->reject();
    });

    button->click();
    QVERIFY(inspected);
}

void PartitionManagerPanelTests::spaceAnalyzerExposesCommercialViews() {
    QTemporaryDir temp;
    QVERIFY(temp.isValid());
    QDir root(temp.path());
    QVERIFY(root.mkpath(QStringLiteral("Logs")));

    QFile logFile(root.filePath(QStringLiteral("Logs/install.log")));
    QVERIFY(logFile.open(QIODevice::WriteOnly));
    logFile.write(QByteArray(32, 'l'));
    logFile.close();

    QFile binFile(root.filePath(QStringLiteral("disk.bin")));
    QVERIFY(binFile.open(QIODevice::WriteOnly));
    binFile.write(QByteArray(64, 'b'));
    binFile.close();

    const QJsonObject result = sak::partitionManagerAnalyzeSpaceForTest(temp.path());
    QVERIFY(!result.value(QStringLiteral("root_missing")).toBool());
    QCOMPARE(result.value(QStringLiteral("top_level_count")).toInt(), 2);
    QCOMPARE(result.value(QStringLiteral("largest_file_count")).toInt(), 2);
    QCOMPARE(result.value(QStringLiteral("file_type_count")).toInt(), 2);
    QCOMPARE(result.value(QStringLiteral("scanned_entries")).toInt(), 2);

    // Both catalogs are the CLOSED sets the analyzer offers, in tab/menu order. Presence
    // probes could not see a fourth view or context action appearing, nor the order changing
    // (which decides the default tab and the top menu entry).
    const QJsonArray views = result.value(QStringLiteral("view_names")).toArray();
    QCOMPARE(views.size(), 3);
    QCOMPARE(views.at(0).toString(), QStringLiteral("Tree View"));
    QCOMPARE(views.at(1).toString(), QStringLiteral("Largest Files"));
    QCOMPARE(views.at(2).toString(), QStringLiteral("File Types"));

    const QJsonArray actions = result.value(QStringLiteral("context_actions")).toArray();
    QCOMPARE(actions.size(), 3);
    QCOMPARE(actions.at(0).toString(), QStringLiteral("Open"));
    QCOMPARE(actions.at(1).toString(), QStringLiteral("Explore in File Explorer"));
    QCOMPARE(actions.at(2).toString(), QStringLiteral("Copy Path"));

    // The rows carry the analysis itself. Collecting only the names threw away the byte and
    // count columns -- the numbers an operator uses to decide what to delete -- so a report
    // that attributed the 64-byte .bin total to .log passed unnoticed.
    const QJsonArray typeRows = result.value(QStringLiteral("file_types")).toArray();
    QCOMPARE(typeRows.size(), 2);
    const QJsonObject binRow = typeRows.at(0).toObject();
    QCOMPARE(binRow.value(QStringLiteral("name")).toString(), QStringLiteral(".bin"));
    QCOMPARE(binRow.value(QStringLiteral("bytes")).toString(), QStringLiteral("64"));
    QCOMPARE(binRow.value(QStringLiteral("count")).toString(), QStringLiteral("1 file(s)"));
    const QJsonObject logRow = typeRows.at(1).toObject();
    QCOMPARE(logRow.value(QStringLiteral("name")).toString(), QStringLiteral(".log"));
    QCOMPARE(logRow.value(QStringLiteral("bytes")).toString(), QStringLiteral("32"));
    QCOMPARE(logRow.value(QStringLiteral("count")).toString(), QStringLiteral("1 file(s)"));
    QCOMPARE(result.value(QStringLiteral("total_bytes")).toString(), QStringLiteral("96"));
}

void PartitionManagerPanelTests::changeClusterSizeQueuesVerifiedReformatOperation() {
    QTemporaryDir backupRoot;
    QVERIFY(backupRoot.isValid());

    sak::PartitionManagerPanel panel;
    panel.setTestInventoryForReview(applyReviewInventoryFixture());

    auto* table = panel.findChild<QTableWidget*>();
    QVERIFY2(table != nullptr, "Partition table should exist");
    table->selectRow(1);

    auto* button = findToolButtonByName(&panel, QStringLiteral("Change Cluster Size"));
    QVERIFY2(button != nullptr, "Change Cluster Size action should exist");
    QCOMPARE(button->toolTip(),
             QStringLiteral("Back up, reformat with selected cluster size, restore, and verify"));

    bool inspected = false;
    QTimer::singleShot(0, [&]() {
        auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
        QVERIFY2(dialog != nullptr, "Change Cluster Size dialog should open");
        QCOMPARE(dialog->accessibleName(), QStringLiteral("Change Cluster Size"));

        auto* cluster = findAccessibleWidget<QComboBox>(dialog,
                                                        QStringLiteral("Target cluster size"));
        QVERIFY(cluster != nullptr);
        // addClusterSizeControls pins the default selection to 4 KB (kAllocationUnit4KbBytes) via
        // setAllocationUnitBytes; `!= 0` only rejected the "Default" sentinel and would pass a
        // wrong-but-nonzero cluster size.
        QCOMPARE(cluster->currentData().toULongLong(), static_cast<qulonglong>(4 * 1024));

        auto* backup = findAccessibleWidget<QLineEdit>(dialog,
                                                       QStringLiteral("Cluster backup directory"));
        QVERIFY(backup != nullptr);
        backup->setText(backupRoot.path());

        auto* confirm = findAccessibleWidget<QCheckBox>(
            dialog, QStringLiteral("Confirm cluster-size reformat backup and restore"));
        QVERIFY(confirm != nullptr);
        confirm->setChecked(true);
        inspected = true;
        dialog->accept();
    });

    button->click();
    QVERIFY(inspected);

    auto* queue = panel.findChild<QListWidget*>();
    QVERIFY2(queue != nullptr, "Pending operation queue should exist");
    QCOMPARE(queue->count(), 1);
    // Whole line: the target half is what proves the reformat was queued against the selected
    // partition, and a " - BLOCKED: ..." suffix would otherwise pass silently.
    QCOMPARE(queue->item(0)->text(), QStringLiteral("Change Cluster Size - Disk 0 Partition 1"));
}

void PartitionManagerPanelTests::allocateFreeSpaceQueuesAdjacentDonorOperation() {
    QTemporaryDir backupRoot;
    QVERIFY(backupRoot.isValid());

    sak::PartitionManagerPanel panel;
    panel.setTestInventoryForReview(allocateFreeSpaceInventoryFixture());

    auto* table = panel.findChild<QTableWidget*>();
    QVERIFY2(table != nullptr, "Partition table should exist");
    table->selectRow(1);

    auto* button = findToolButtonByName(&panel, QStringLiteral("Allocate Free Space"));
    QVERIFY2(button != nullptr, "Allocate Free Space action should exist");
    QCOMPARE(button->toolTip(),
             QStringLiteral(
                 "Back up adjacent donor, extend target, recreate donor, restore, and verify"));

    bool inspected = false;
    QTimer::singleShot(0, [&]() {
        auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
        QVERIFY2(dialog != nullptr, "Allocate Free Space dialog should open");
        QCOMPARE(dialog->accessibleName(), QStringLiteral("Allocate Free Space"));

        auto* amount = findAccessibleWidget<QSpinBox>(dialog,
                                                      QStringLiteral("Allocate free space amount"));
        QVERIFY(amount != nullptr);
        amount->setValue(64);

        auto* backup = findAccessibleWidget<QLineEdit>(
            dialog, QStringLiteral("Allocate donor backup directory"));
        QVERIFY(backup != nullptr);
        backup->setText(backupRoot.path());

        auto* confirm = findAccessibleWidget<QCheckBox>(
            dialog, QStringLiteral("Confirm allocate-free-space backup and restore"));
        QVERIFY(confirm != nullptr);
        confirm->setChecked(true);
        inspected = true;
        dialog->accept();
    });

    button->click();
    QVERIFY(inspected);

    auto* queue = panel.findChild<QListWidget*>();
    QVERIFY2(queue != nullptr, "Pending operation queue should exist");
    QCOMPARE(queue->count(), 1);
    QCOMPARE(queue->item(0)->text(), QStringLiteral("Allocate Free Space - Disk 0 Partition 1"));
}

void PartitionManagerPanelTests::unallocatedAllocateFreeSpaceQueuesAdjacentEngines() {
    queueUnallocatedAllocateAndVerifyResize();
    queueUnallocatedAllocateAndVerifyMove();
}

void PartitionManagerPanelTests::formerCommercialCompatibilityActionsQueueDirectEngines() {
    QTemporaryDir backupRoot;
    QVERIFY(backupRoot.isValid());

    for (const auto& action : expectedMetadataActions()) {
        queueMetadataActionAndVerify(action, backupRoot.path());
    }
}

void PartitionManagerPanelTests::createDialogExposesSynchronizedHandleControls() {
    sak::PartitionManagerPanel panel;
    auto* table = panel.findChild<QTableWidget*>();
    QVERIFY2(table != nullptr, "Partition Manager table should exist");
    addUnallocatedTestSelection(table);

    CreateDialogInspection result;

    QTimer::singleShot(0, [&]() {
        auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
        if (!dialog) {
            return;
        }
        inspectCreateDialog(dialog, &result);
        dialog->reject();
    });

    QVERIFY(QMetaObject::invokeMethod(&panel, "onCreatePartition", Qt::DirectConnection));
    QVERIFY(result.inspected);
    QVERIFY(result.has_size_handle);
    QVERIFY(result.has_location_handle);
    QVERIFY2(result.windows_native_file_systems_only, qPrintable(result.file_system_items));
    QVERIFY(result.size_synced);
    QVERIFY(result.location_synced);
    QVERIFY(result.preview_drag_synced);
}

void PartitionManagerPanelTests::wipeActionLetsUserChooseScope() {
    sak::PartitionManagerPanel panel;
    panel.setTestInventoryForReview(applyReviewInventoryFixture());
    auto* table = panel.findChild<QTableWidget*>();
    QVERIFY2(table != nullptr, "Partition table should exist");
    table->selectRow(1);
    QApplication::processEvents();

    bool inspected = false;
    QTimer::singleShot(0, [&]() {
        auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
        QVERIFY2(dialog != nullptr, "Wipe dialog should open");
        auto* mode = findAccessibleWidget<QComboBox>(dialog, QStringLiteral("Wipe scope"));
        QVERIFY2(mode != nullptr, "Wipe scope selector should exist for a partition target");
        const int index = mode->findText(QStringLiteral("Entire partition (erase all data)"));
        QVERIFY(index >= 0);
        mode->setCurrentIndex(index);
        auto* confirm = findAccessibleWidget<QCheckBox>(dialog,
                                                        QStringLiteral("Confirm wipe partition"));
        QVERIFY(confirm != nullptr);
        confirm->setChecked(true);
        inspected = true;
        dialog->accept();
    });

    QVERIFY(QMetaObject::invokeMethod(&panel, "onWipeSelected", Qt::DirectConnection));
    QVERIFY(inspected);
    verifySingleQueuedOperation(&panel, QStringLiteral("Wipe Partition - Disk 0 Partition 1"));
}

// Fail-closed rule for Quick Partition size derivation: malformed custom sizes must NOT be
// silently substituted with an equal-size layout, and adversarial near-UINT64_MAX sizes must not
// overflow the total into a value that slips past the "total <= usable" validity check.
void PartitionManagerPanelTests::quickPartitionSizesFailClosedOnMalformedCustomAndOverflow() {
    const uint64_t usable = 400 * kTestMegabyteBytes;

    // Equal mode yields a valid even split.
    const QJsonObject equalOpts{{QStringLiteral("partition_count"), 4},
                                {QStringLiteral("size_mode"), QStringLiteral("equal")}};
    const auto equalSizes = sak::partitionQuickSizesForOptionsForTest(equalOpts, usable);
    // "Even split" is the actual claim: a count of 4 plus the validity predicate (total <=
    // usable) is equally satisfied by 4 unequal sizes, or by 4 sizes that leave most of the
    // disk unallocated. 400 MiB / 4 = 100 MiB each, exactly.
    QCOMPARE(equalSizes,
             QVector<uint64_t>({100 * kTestMegabyteBytes,
                                100 * kTestMegabyteBytes,
                                100 * kTestMegabyteBytes,
                                100 * kTestMegabyteBytes}));
    QVERIFY(sak::partitionQuickSizesAreValidForTest(equalSizes, usable));

    // Custom mode with a malformed (zero) size fails closed: empty result, never an equal-size
    // substitution, and nothing enqueueable.
    const QJsonArray badSizes{QString::number(100ULL * kTestMegabyteBytes), QStringLiteral("0")};
    const QJsonObject badOpts{{QStringLiteral("partition_count"), 2},
                              {QStringLiteral("size_mode"), QStringLiteral("custom")},
                              {QStringLiteral("custom_size_bytes"), badSizes}};
    const auto bad = sak::partitionQuickSizesForOptionsForTest(badOpts, usable);
    QVERIFY(bad.isEmpty());
    QVERIFY(!sak::partitionQuickSizesAreValidForTest(bad, usable));

    // Custom array count mismatching partition_count is malformed -> refused.
    const QJsonArray shortSizes{QString::number(100ULL * kTestMegabyteBytes)};
    const QJsonObject mismatchOpts{{QStringLiteral("partition_count"), 2},
                                   {QStringLiteral("size_mode"), QStringLiteral("custom")},
                                   {QStringLiteral("custom_size_bytes"), shortSizes}};
    QVERIFY(sak::partitionQuickSizesForOptionsForTest(mismatchOpts, usable).isEmpty());

    // Overflow: two near-UINT64_MAX custom sizes parse fine but their saturated total must stay
    // greater than usable so validity is false (no wrap-around acceptance).
    const QString huge = QString::number(std::numeric_limits<uint64_t>::max() - 10);
    const QJsonArray overflowSizes{huge, huge};
    const QJsonObject overflowOpts{{QStringLiteral("partition_count"), 2},
                                   {QStringLiteral("size_mode"), QStringLiteral("custom")},
                                   {QStringLiteral("custom_size_bytes"), overflowSizes}};
    const auto overflow = sak::partitionQuickSizesForOptionsForTest(overflowOpts, usable);
    QCOMPARE(overflow.size(), 2);
    QVERIFY(!sak::partitionQuickSizesAreValidForTest(overflow, usable));
}

namespace {

// Dismisses whatever modal is up and reports whether it was a WIZARD (a message box is a
// modal too, and an action that only warns has not opened its wizard).
void dismissModalAndRecordWizard(bool* wizard_opened) {
    auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
    if (dialog == nullptr) {
        return;
    }
    if (qobject_cast<QMessageBox*>(dialog) == nullptr) {
        *wizard_opened = true;
    }
    dialog->reject();
}

bool invokeWizardSlot(sak::PartitionManagerPanel* panel, const char* slot) {
    bool wizard_opened = false;
    QTimer::singleShot(0, [&wizard_opened]() { dismissModalAndRecordWizard(&wizard_opened); });
    const bool invoked = QMetaObject::invokeMethod(panel, slot, Qt::DirectConnection);
    QApplication::processEvents();
    flushDeferredDeletes();
    return invoked && wizard_opened;
}

// A wizard entry point must refuse while the executor is consuming the queue, and must still
// work from an idle controller (so the refusal came from the guard, not an unqualified
// fixture). Toolbar/context-menu enablement is deliberately not relied on: the context menu
// stays live during an Apply.
void verifyWizardGuardedWhileApplying(const char* slot,
                                      const sak::PartitionInventory& inventory,
                                      int select_row) {
    sak::PartitionManagerPanel panel;
    panel.setTestInventoryForReview(inventory);
    auto* table = panel.findChild<QTableWidget*>();
    QVERIFY2(table != nullptr, "Partition table should exist");
    table->selectRow(select_row);
    QApplication::processEvents();

    panel.setTestApplyStateForReview(sak::PartitionManagerState::Applying);
    // The wizard not opening is necessary but not sufficient: a slot that silently did nothing
    // (a missing entry point, a null selection) also satisfies it. The guard is what must have
    // fired, and it announces itself exactly once through statusMessage.
    QSignalSpy guardSpy(&panel, &sak::PartitionManagerPanel::statusMessage);
    QVERIFY2(!invokeWizardSlot(&panel, slot), slot);
    QCOMPARE(guardSpy.count(), 1);
    QCOMPARE(guardSpy.takeFirst().at(0).toString(),
             QStringLiteral("Finish or cancel the running operation before changing the queue"));
    auto* queue = panel.findChild<QListWidget*>();
    QVERIFY2(queue != nullptr, "Pending operation queue should exist");
    QCOMPARE(queue->count(), 0);

    panel.setTestApplyStateForReview(sak::PartitionManagerState::Ready);
    QVERIFY2(invokeWizardSlot(&panel, slot), slot);
}

}  // namespace

// R5 p11_gui-3: onQuickPartition / onAllocateFreeSpace / onConvertDynamicDiskToBasic called
// the controller's queueOperation directly, bypassing queueMutationBlockedByRunningOperation.
// The controller queues unconditionally (PlanningOperation -> QueueDirty), so a wizard run
// during an Apply corrupted the state machine mid-apply and mutated the queue the executor
// was consuming. Every entry point now goes through the one central guard.
void PartitionManagerPanelTests::wizardEntryPointsRespectRunningOperationGuard() const {
    verifyWizardGuardedWhileApplying("onQuickPartition", unallocatedAllocateInventoryFixture(), 0);
    verifyWizardGuardedWhileApplying("onAllocateFreeSpace", allocateFreeSpaceInventoryFixture(), 1);
    verifyWizardGuardedWhileApplying("onConvertDynamicDiskToBasic",
                                     metadataRebuildInventoryFixture(true),
                                     0);
}

// The scan-completion report is a pure function of the inventory, so the honesty rule is
// asserted without a live PowerShell scan: a disk whose partition enumeration failed can never
// be announced as a plain "inventory ready".
void PartitionManagerPanelTests::inventoryStateIsHonestAboutFailedPartitionEnumeration() {
    sak::PartitionInventory healthy = applyReviewInventoryFixture();
    QVERIFY(!healthy.hasPartitionEnumerationFailure());
    QCOMPARE(sak::PartitionManagerController::inventoryReadyState(healthy),
             sak::PartitionManagerState::Ready);
    QCOMPARE(sak::PartitionManagerController::inventoryStatusMessage(healthy),
             QStringLiteral("Partition Manager: inventory ready"));

    sak::PartitionInventory degraded = healthy;
    degraded.disks[0].partition_enumeration_failed = true;
    degraded.disks[0].partition_enumeration_error = QStringLiteral("Access is denied.");
    QVERIFY(degraded.hasPartitionEnumerationFailure());
    QCOMPARE(sak::PartitionManagerController::inventoryReadyState(degraded),
             sak::PartitionManagerState::ReadyPartial);

    // The honesty claim is the WHOLE sentence: it must name the failed disk AND state that
    // operations on it are refused. The three fragment checks were jointly satisfied by a
    // message that flagged INCOMPLETE and listed the disk while omitting the refusal promise
    // -- which is the part an operator acts on.
    QCOMPARE(sak::PartitionManagerController::inventoryStatusMessage(degraded),
             QStringLiteral("Partition Manager: inventory INCOMPLETE -- partition enumeration "
                            "failed for disk(s) 0; operations on them are refused until a "
                            "refresh succeeds"));
}

QTEST_MAIN(PartitionManagerPanelTests)

#include "test_partition_manager_panel.moc"
