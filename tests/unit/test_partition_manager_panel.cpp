// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/partition_file_system_detector.h"
#include "sak/partition_file_system_tool_runner.h"
#include "sak/partition_manager_panel.h"
#include "sak/style_constants.h"

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
#include <QJsonArray>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMouseEvent>
#include <QPushButton>
#include <QScrollArea>
#include <QSlider>
#include <QSpinBox>
#include <QSplitter>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QTemporaryDir>
#include <QTextEdit>
#include <QTimer>
#include <QToolButton>
#include <QtTest/QtTest>

#include <algorithm>
#include <cstdlib>

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
    void extFilesystemWriteActionsQueueWithConfirmation();
    void apfsRootFileMutationActionGatesGeneratedLayouts();
    void hfsFileMutationActionQueuesStagedWrite();
    void hfsEmptyFileMutationModesQueueWithoutPayload();
    void manageBitLockerShowsStatusDialog();
    void diskDefragShowsOptimizeDialog();
    void ssdSecureEraseShowsQueueDialog();
    void spaceAnalyzerExposesCommercialViews();
    void changeClusterSizeQueuesVerifiedReformatOperation();
    void allocateFreeSpaceQueuesAdjacentDonorOperation();
    void unallocatedAllocateFreeSpaceQueuesAdjacentEngines();
    void formerCommercialCompatibilityActionsQueueDirectEngines();
    void createDialogExposesSynchronizedHandleControls();
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
    bool has_non_native_file_systems{false};
    bool raw_create_controls_toggle{false};
    bool swap_create_controls_toggle{false};
    bool size_synced{false};
    bool location_synced{false};
    bool preview_drag_synced{false};
    QString file_system_items;
    QString raw_toggle_state;
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

void setComboItem(QComboBox* combo, const QString& text) {
    if (!combo) {
        return;
    }
    for (int index = 0; index < combo->count(); ++index) {
        if (combo->itemText(index).compare(text, Qt::CaseInsensitive) == 0) {
            combo->setCurrentIndex(index);
            return;
        }
    }
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
            comboHasItem(combo, QStringLiteral("ext4"))) {
            return combo;
        }
    }
    return nullptr;
}

QCheckBox* findCreateRawConfirm(QDialog* dialog) {
    auto* confirm =
        findAccessibleWidget<QCheckBox>(dialog, QStringLiteral("Confirm ext filesystem format"));
    if (confirm) {
        return confirm;
    }
    return findAccessibleWidget<QCheckBox>(dialog, QStringLiteral("Confirm raw filesystem format"));
}

void inspectCreateFileSystems(QDialog* dialog, CreateDialogInspection* result) {
    auto* fileSystem = findCreateFileSystemCombo(dialog);
    result->file_system_items = fileSystem ? comboItemsText(fileSystem)
                                           : comboInventoryText(dialog);
    result->has_non_native_file_systems = fileSystem &&
                                          comboHasItem(fileSystem, QStringLiteral("ext4")) &&
                                          comboHasItem(fileSystem, QStringLiteral("HFSX")) &&
                                          comboHasItem(fileSystem, QStringLiteral("Linux swap")) &&
                                          comboHasItem(fileSystem, QStringLiteral("APFS"));
}

void inspectCreateRawControls(QDialog* dialog, CreateDialogInspection* result) {
    auto* fileSystem = findCreateFileSystemCombo(dialog);
    auto* allocationUnit = findAccessibleWidget<QComboBox>(dialog, QStringLiteral("Cluster size"));
    auto* driveLetter = findAccessibleWidget<QComboBox>(dialog, QStringLiteral("Drive letter"));
    auto* confirm = findCreateRawConfirm(dialog);
    auto* swapPageSize = findAccessibleWidget<QComboBox>(dialog,
                                                         QStringLiteral("Linux swap page size"));
    result->raw_toggle_state =
        QStringLiteral("fileSystem=%1 allocation=%2 drive=%3 confirm=%4 swap=%5")
            .arg(fileSystem != nullptr)
            .arg(allocationUnit != nullptr)
            .arg(driveLetter != nullptr)
            .arg(confirm != nullptr)
            .arg(swapPageSize != nullptr);
    if (!fileSystem || !allocationUnit || !driveLetter || !confirm || !swapPageSize) {
        return;
    }

    setComboItem(fileSystem, QStringLiteral("ext4"));
    QApplication::processEvents();
    result->raw_create_controls_toggle = confirm->isVisible() && !allocationUnit->isEnabled() &&
                                         !driveLetter->isEnabled();
    result->raw_toggle_state =
        result->raw_toggle_state +
        QStringLiteral("; confirmVisible=%1 allocationEnabled=%2 driveEnabled=%3")
            .arg(confirm->isVisible())
            .arg(allocationUnit->isEnabled())
            .arg(driveLetter->isEnabled());
    setComboItem(fileSystem, QStringLiteral("Linux swap"));
    QApplication::processEvents();
    result->swap_create_controls_toggle = swapPageSize->isVisible() && confirm->isVisible() &&
                                          confirm->accessibleName() ==
                                              QStringLiteral("Confirm Linux swap format");
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
    inspectCreateRawControls(dialog, result);
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
    partition.disk_number = 0;
    partition.partition_number = 1;
    partition.type_name = QStringLiteral("Basic");
    partition.size_bytes = volume.total_bytes;
    partition.volume = volume;

    sak::PartitionDiskInfo disk;
    disk.disk_number = 0;
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
    donor.partition_number = 2;
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
    partition.disk_number = 0;
    partition.partition_number = 1;
    partition.type_name = dynamicDisk ? QStringLiteral("Simple Volume") : QStringLiteral("Basic");
    partition.offset_bytes = kTestMegabyteBytes;
    partition.size_bytes = volume.total_bytes;
    partition.volume = volume;

    sak::PartitionDiskInfo disk;
    disk.disk_number = 0;
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
    disk.disk_number = 0;
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
    first.partition_number = 1;
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
    second.partition_number = 2;
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
    partition.disk_number = 0;
    partition.partition_number = number;
    partition.type_name = typeName;
    partition.offset_bytes = offsetBytes;
    partition.size_bytes = volume.total_bytes;
    partition.volume = volume;
    return partition;
}

sak::PartitionInventory allColorRolesInventoryFixture() {
    sak::PartitionDiskInfo basicDisk;
    basicDisk.disk_number = 0;
    basicDisk.partition_style = QStringLiteral("MBR");
    basicDisk.health_status = QStringLiteral("Healthy");
    basicDisk.operational_status = QStringLiteral("Online");
    basicDisk.size_bytes = 384 * kTestMegabyteBytes;
    basicDisk.partitions.append(
        rolePartition(1, QStringLiteral("Basic"), QStringLiteral("P"), 1 * kTestMegabyteBytes));
    basicDisk.partitions.append(
        rolePartition(2, QStringLiteral("Logical"), QStringLiteral("L"), 128 * kTestMegabyteBytes));
    basicDisk.unallocated_regions.append({0, 256 * kTestMegabyteBytes, 64 * kTestMegabyteBytes});

    sak::PartitionDiskInfo dynamicDisk;
    dynamicDisk.disk_number = 1;
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
    QVERIFY(inspect->isEnabled());
    QVERIFY(inspect->toolTip().contains(QStringLiteral("Inspect captured read-only HFS+")));
    QVERIFY(browse->isEnabled());
    QVERIFY(browse->toolTip().contains(QStringLiteral("Browse read-only HFS+")));
    QVERIFY(check->isEnabled());
    QVERIFY(check->toolTip().contains(QStringLiteral("fsck_hfs")));
    QVERIFY(!resize->isEnabled());
    QVERIFY(resize->toolTip().contains(QStringLiteral("HFS+ resize is not certified")));
    QVERIFY(!nativeCheck->isEnabled());
    QVERIFY(nativeCheck->toolTip().contains(QStringLiteral("Non-Windows filesystem actions")));
    QVERIFY(!changeCluster->isEnabled());
    QVERIFY(changeCluster->toolTip().contains(QStringLiteral("Non-Windows filesystem actions")));
    QVERIFY(!changeLabel->isEnabled());
    QVERIFY(changeLabel->toolTip().contains(QStringLiteral("Non-Windows filesystem actions")));
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
        QVERIFY(metadata.contains(QStringLiteral("HFS wrapper: Yes")));
        QVERIFY(metadata.contains(QStringLiteral("Block size: 4096")));
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
    QCOMPARE(actionStates.value(QStringLiteral("Resize/Move Partition")), false);
    QCOMPARE(actionStates.value(QStringLiteral("Check File System")), false);
    QCOMPARE(actionStates.value(QStringLiteral("Change Cluster Size")), false);
    QCOMPARE(actionStates.value(QStringLiteral("Change Label")), false);
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

void verifyMetadataCheckDialog(QToolButton* button, const QString& expectedNeedle) {
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
        QVERIFY(
            propertyTableValue(properties, QStringLiteral("Findings")).contains(expectedNeedle));
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
        QVERIFY(metadata.contains(QStringLiteral("Metadata sanity: XFS")));
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
        QVERIFY(metadata.contains(QStringLiteral("Metadata sanity: XFS")));
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

void verifySingleQueuedOperation(sak::PartitionManagerPanel* panel, const QString& text) {
    auto* queue = panel->findChild<QListWidget*>();
    QVERIFY2(queue != nullptr, "Pending operation queue should exist");
    QCOMPARE(queue->count(), 1);
    QVERIFY(queue->item(0)->text().contains(text));
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
    verifySingleQueuedOperation(&panel, QStringLiteral("Format Partition"));
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
    verifySingleQueuedOperation(&panel, QStringLiteral("Format Partition"));
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
    verifySingleQueuedOperation(&panel, QStringLiteral("Format Partition"));
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
        QVERIFY(targetPath->toolTip().contains(QStringLiteral("selected raw partition")));
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
    verifySingleQueuedOperation(&panel, QStringLiteral("Check File System"));
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
        QVERIFY(mode->findText(QStringLiteral("Original HFS+ catalog check now")) >= 0);
        QVERIFY(mode->findText(QStringLiteral("Read-only check now")) >= 0);
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
    verifySingleQueuedOperation(&panel, QStringLiteral("Check File System"));
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
        QVERIFY(mode->findText(QStringLiteral("Read-only check now")) >= 0);
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
    verifySingleQueuedOperation(&panel, QStringLiteral("Check File System"));
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

void queueApfsRootFileMutationAndVerify() {
    sak::PartitionManagerPanel panel;
    panel.setTestInventoryForReview(generatedApfsInventoryFixture());
    auto* table = panel.findChild<QTableWidget*>();
    QVERIFY2(table != nullptr, "Partition table should exist");
    table->selectRow(1);
    QApplication::processEvents();

    auto* button = findToolButtonByName(&panel, QStringLiteral("APFS File"));
    QVERIFY2(button != nullptr, "APFS File action should exist");
    QVERIFY(button->isEnabled());
    QVERIFY(button->toolTip().contains(QStringLiteral("generated APFS")));

    bool inspected = false;
    QTimer::singleShot(0, [&]() {
        auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
        QVERIFY2(dialog != nullptr, "APFS generated file dialog should open");
        auto* mode = findAccessibleWidget<QComboBox>(
            dialog, QStringLiteral("APFS generated file mutation mode"));
        QVERIFY(mode != nullptr);
        QCOMPARE(mode->currentText(), QStringLiteral("Write root file"));
        QVERIFY(comboHasItem(mode, QStringLiteral("Write file in root directory")));
        QVERIFY(comboHasItem(mode, QStringLiteral("Patch file in root directory")));
        QVERIFY(comboHasItem(mode, QStringLiteral("Delete file in root directory")));
        QVERIFY(comboHasItem(mode, QStringLiteral("Create empty root directory")));
        QVERIFY(comboHasItem(mode, QStringLiteral("Delete empty root directory")));
        QVERIFY(comboHasItem(mode, QStringLiteral("Change volume label")));
        auto* fileName =
            findAccessibleWidget<QLineEdit>(dialog, QStringLiteral("APFS file or directory name"));
        QVERIFY(fileName != nullptr);
        fileName->setText(QStringLiteral("panel-test.txt"));
        auto* payload = findAccessibleWidget<QTextEdit>(dialog,
                                                        QStringLiteral("APFS file payload text"));
        QVERIFY(payload != nullptr);
        auto* directoryName =
            findAccessibleWidget<QLineEdit>(dialog, QStringLiteral("APFS root directory name"));
        QVERIFY(directoryName != nullptr);
        auto* patchOffset = findAccessibleWidget<QLineEdit>(
            dialog, QStringLiteral("APFS root file patch byte offset"));
        QVERIFY(patchOffset != nullptr);
        mode->setCurrentText(QStringLiteral("Write file in root directory"));
        QVERIFY(directoryName->isVisible());
        directoryName->setText(QStringLiteral("Proof Folder"));
        QCOMPARE(fileName->placeholderText(), QStringLiteral("Child file name"));
        mode->setCurrentText(QStringLiteral("Patch file in root directory"));
        QVERIFY(directoryName->isVisible());
        QVERIFY(payload->isVisible());
        QVERIFY(patchOffset->isVisible());
        QCOMPARE(fileName->placeholderText(), QStringLiteral("Child file name"));
        mode->setCurrentText(QStringLiteral("Create empty root directory"));
        QVERIFY(!payload->isVisible());
        QVERIFY(!directoryName->isVisible());
        QVERIFY(!patchOffset->isVisible());
        mode->setCurrentText(QStringLiteral("Change volume label"));
        QVERIFY(!payload->isVisible());
        QVERIFY(!directoryName->isVisible());
        QVERIFY(!patchOffset->isVisible());
        QCOMPARE(fileName->placeholderText(), QStringLiteral("Volume label"));
        mode->setCurrentText(QStringLiteral("Write root file"));
        payload->setPlainText(QStringLiteral("panel payload"));
        auto* confirm = findAccessibleWidget<QCheckBox>(
            dialog, QStringLiteral("Confirm APFS generated file mutation"));
        QVERIFY(confirm != nullptr);
        confirm->setChecked(true);
        inspected = true;
        dialog->accept();
    });
    button->click();
    QVERIFY(inspected);
    verifySingleQueuedOperation(&panel, QStringLiteral("APFS Write Root File"));
}

void queueHfsFileMutationAndVerify() {
    sak::PartitionManagerPanel panel;
    configureRawWritePanel(&panel, QStringLiteral("HFS+"));
    QApplication::processEvents();

    auto* button = findToolButtonByName(&panel, QStringLiteral("HFS File"));
    QVERIFY2(button != nullptr, "HFS File action should exist");
    QVERIFY(button->isEnabled());
    QVERIFY(button->toolTip().contains(QStringLiteral("HFS+ staged file")));

    bool inspected = false;
    QTimer::singleShot(0, [&]() {
        auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
        QVERIFY2(dialog != nullptr, "HFS file dialog should open");
        auto* mode = findAccessibleWidget<QComboBox>(dialog,
                                                     QStringLiteral("HFS file mutation mode"));
        QVERIFY(mode != nullptr);
        QCOMPARE(mode->currentText(), QStringLiteral("Replace data fork within allocated blocks"));
        auto* hfsPath = findAccessibleWidget<QLineEdit>(dialog, QStringLiteral("HFS file path"));
        QVERIFY(hfsPath != nullptr);
        hfsPath->setText(QStringLiteral("/panel-test.txt"));
        auto* payload =
            findAccessibleWidget<QTextEdit>(dialog, QStringLiteral("HFS mutation payload text"));
        QVERIFY(payload != nullptr);
        payload->setPlainText(QStringLiteral("panel hfs payload"));
        auto* confirm = findAccessibleWidget<QCheckBox>(
            dialog, QStringLiteral("Confirm HFS staged file mutation"));
        QVERIFY(confirm != nullptr);
        confirm->setChecked(true);
        inspected = true;
        dialog->accept();
    });
    button->click();
    QVERIFY(inspected);
    verifySingleQueuedOperation(&panel, QStringLiteral("HFS Replace File"));
}

void queueHfsAllocationGrowthMutationAndVerify() {
    sak::PartitionManagerPanel panel;
    configureRawWritePanel(&panel, QStringLiteral("HFS+"));
    QApplication::processEvents();

    auto* button = findToolButtonByName(&panel, QStringLiteral("HFS File"));
    QVERIFY2(button != nullptr, "HFS File action should exist");
    QVERIFY(button->isEnabled());

    bool inspected = false;
    QTimer::singleShot(0, [&]() {
        auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
        QVERIFY2(dialog != nullptr, "HFS file dialog should open");
        auto* mode = findAccessibleWidget<QComboBox>(dialog,
                                                     QStringLiteral("HFS file mutation mode"));
        QVERIFY(mode != nullptr);
        const int index = mode->findText(QStringLiteral("Grow data fork with free blocks"));
        QVERIFY(index >= 0);
        mode->setCurrentIndex(index);
        auto* hfsPath = findAccessibleWidget<QLineEdit>(dialog, QStringLiteral("HFS file path"));
        QVERIFY(hfsPath != nullptr);
        hfsPath->setText(QStringLiteral("/panel-growth.bin"));
        auto* payload =
            findAccessibleWidget<QTextEdit>(dialog, QStringLiteral("HFS mutation payload text"));
        QVERIFY(payload != nullptr);
        payload->setPlainText(QStringLiteral("panel hfs allocation growth payload"));
        auto* confirm = findAccessibleWidget<QCheckBox>(
            dialog, QStringLiteral("Confirm HFS staged file mutation"));
        QVERIFY(confirm != nullptr);
        confirm->setChecked(true);
        inspected = true;
        dialog->accept();
    });
    button->click();
    QVERIFY(inspected);
    verifySingleQueuedOperation(&panel, QStringLiteral("HFS Grow File"));
}

void queueHfsCreateFileMutationAndVerify() {
    sak::PartitionManagerPanel panel;
    configureRawWritePanel(&panel, QStringLiteral("HFS+"));
    QApplication::processEvents();

    auto* button = findToolButtonByName(&panel, QStringLiteral("HFS File"));
    QVERIFY2(button != nullptr, "HFS File action should exist");
    QVERIFY(button->isEnabled());

    bool inspected = false;
    QTimer::singleShot(0, [&]() {
        auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
        QVERIFY2(dialog != nullptr, "HFS file dialog should open");
        auto* mode = findAccessibleWidget<QComboBox>(dialog,
                                                     QStringLiteral("HFS file mutation mode"));
        QVERIFY(mode != nullptr);
        const int index = mode->findText(QStringLiteral("Create file with data"));
        QVERIFY(index >= 0);
        mode->setCurrentIndex(index);
        auto* hfsPath = findAccessibleWidget<QLineEdit>(dialog, QStringLiteral("HFS file path"));
        QVERIFY(hfsPath != nullptr);
        hfsPath->setText(QStringLiteral("/panel-created-data.txt"));
        auto* payload =
            findAccessibleWidget<QTextEdit>(dialog, QStringLiteral("HFS mutation payload text"));
        QVERIFY(payload != nullptr);
        QVERIFY(payload->isVisible());
        payload->setPlainText(QStringLiteral("panel created file payload"));
        auto* confirm = findAccessibleWidget<QCheckBox>(
            dialog, QStringLiteral("Confirm HFS staged file mutation"));
        QVERIFY(confirm != nullptr);
        confirm->setChecked(true);
        auto* preview = findAccessibleWidget<QLabel>(dialog, QStringLiteral("Operation preview"));
        QVERIFY(preview != nullptr);
        QVERIFY(preview->text().contains(QStringLiteral("file create")));
        inspected = true;
        dialog->accept();
    });
    button->click();
    QVERIFY(inspected);
    verifySingleQueuedOperation(&panel, QStringLiteral("HFS Create File"));
}

void queueHfsForkAttributeMutationAndVerify() {
    sak::PartitionManagerPanel panel;
    configureRawWritePanel(&panel, QStringLiteral("HFS+"));
    QApplication::processEvents();

    auto* button = findToolButtonByName(&panel, QStringLiteral("HFS File"));
    QVERIFY2(button != nullptr, "HFS File action should exist");
    QVERIFY(button->isEnabled());

    bool inspected = false;
    QTimer::singleShot(0, [&]() {
        auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
        QVERIFY2(dialog != nullptr, "HFS file dialog should open");
        auto* mode = findAccessibleWidget<QComboBox>(dialog,
                                                     QStringLiteral("HFS file mutation mode"));
        QVERIFY(mode != nullptr);
        const int modeIndex = mode->findText(QStringLiteral("Replace fork-backed attribute"));
        QVERIFY2(modeIndex >= 0, "fork-backed attribute mode should exist");
        mode->setCurrentIndex(modeIndex);

        auto* hfsPath = findAccessibleWidget<QLineEdit>(dialog, QStringLiteral("HFS file path"));
        QVERIFY(hfsPath != nullptr);
        QVERIFY(!hfsPath->isVisible());
        auto* payload =
            findAccessibleWidget<QTextEdit>(dialog, QStringLiteral("HFS mutation payload text"));
        QVERIFY(payload != nullptr);
        QVERIFY(payload->isVisible());
        payload->setPlainText(QStringLiteral("panel fork attribute payload"));
        auto* fileId = findAccessibleWidget<QLineEdit>(dialog,
                                                       QStringLiteral("HFS attribute file ID"));
        QVERIFY(fileId != nullptr);
        QVERIFY(fileId->isVisible());
        fileId->setText(QStringLiteral("17"));
        auto* attributeName = findAccessibleWidget<QLineEdit>(dialog,
                                                              QStringLiteral("HFS attribute name"));
        QVERIFY(attributeName != nullptr);
        QVERIFY(attributeName->isVisible());
        attributeName->setText(QStringLiteral("com.apple.ResourceFork"));
        auto* preview = findAccessibleWidget<QLabel>(dialog, QStringLiteral("Operation preview"));
        QVERIFY(preview != nullptr);
        QVERIFY(preview->text().contains(QStringLiteral("fork-backed attribute")));
        auto* confirm = findAccessibleWidget<QCheckBox>(
            dialog, QStringLiteral("Confirm HFS staged file mutation"));
        QVERIFY(confirm != nullptr);
        confirm->setChecked(true);
        QApplication::processEvents();
        auto* buttons = dialog->findChild<QDialogButtonBox*>();
        QVERIFY(buttons != nullptr);
        auto* accept = buttons->button(QDialogButtonBox::Ok);
        QVERIFY(accept != nullptr);
        QVERIFY(accept->isEnabled());
        inspected = true;
        dialog->accept();
    });
    button->click();
    QVERIFY(inspected);
    verifySingleQueuedOperation(&panel, QStringLiteral("HFS Replace Fork Attribute"));
}

void queueHfsForkAttributeGrowthMutationAndVerify() {
    sak::PartitionManagerPanel panel;
    configureRawWritePanel(&panel, QStringLiteral("HFS+"));
    QApplication::processEvents();

    auto* button = findToolButtonByName(&panel, QStringLiteral("HFS File"));
    QVERIFY2(button != nullptr, "HFS File action should exist");
    QVERIFY(button->isEnabled());

    bool inspected = false;
    QTimer::singleShot(0, [&]() {
        auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
        QVERIFY2(dialog != nullptr, "HFS file dialog should open");
        auto* mode = findAccessibleWidget<QComboBox>(dialog,
                                                     QStringLiteral("HFS file mutation mode"));
        QVERIFY(mode != nullptr);
        const int modeIndex =
            mode->findText(QStringLiteral("Grow fork-backed attribute with free blocks"));
        QVERIFY2(modeIndex >= 0, "fork-backed attribute growth mode should exist");
        mode->setCurrentIndex(modeIndex);

        auto* hfsPath = findAccessibleWidget<QLineEdit>(dialog, QStringLiteral("HFS file path"));
        QVERIFY(hfsPath != nullptr);
        QVERIFY(!hfsPath->isVisible());
        auto* payload =
            findAccessibleWidget<QTextEdit>(dialog, QStringLiteral("HFS mutation payload text"));
        QVERIFY(payload != nullptr);
        QVERIFY(payload->isVisible());
        payload->setPlainText(QStringLiteral("panel fork attribute growth payload"));
        auto* fileId = findAccessibleWidget<QLineEdit>(dialog,
                                                       QStringLiteral("HFS attribute file ID"));
        QVERIFY(fileId != nullptr);
        QVERIFY(fileId->isVisible());
        fileId->setText(QStringLiteral("17"));
        auto* attributeName = findAccessibleWidget<QLineEdit>(dialog,
                                                              QStringLiteral("HFS attribute name"));
        QVERIFY(attributeName != nullptr);
        QVERIFY(attributeName->isVisible());
        attributeName->setText(QStringLiteral("com.apple.ResourceFork"));
        auto* preview = findAccessibleWidget<QLabel>(dialog, QStringLiteral("Operation preview"));
        QVERIFY(preview != nullptr);
        QVERIFY(preview->text().contains(QStringLiteral("allocation growth")));
        auto* confirm = findAccessibleWidget<QCheckBox>(
            dialog, QStringLiteral("Confirm HFS staged file mutation"));
        QVERIFY(confirm != nullptr);
        confirm->setChecked(true);
        QApplication::processEvents();
        auto* buttons = dialog->findChild<QDialogButtonBox*>();
        QVERIFY(buttons != nullptr);
        auto* accept = buttons->button(QDialogButtonBox::Ok);
        QVERIFY(accept != nullptr);
        QVERIFY(accept->isEnabled());
        inspected = true;
        dialog->accept();
    });
    button->click();
    QVERIFY(inspected);
    verifySingleQueuedOperation(&panel, QStringLiteral("HFS Grow Fork Attribute"));
}

void queueHfsRenameMoveMutationAndVerify() {
    sak::PartitionManagerPanel panel;
    configureRawWritePanel(&panel, QStringLiteral("HFS+"));
    QApplication::processEvents();

    auto* button = findToolButtonByName(&panel, QStringLiteral("HFS File"));
    QVERIFY2(button != nullptr, "HFS File action should exist");
    QVERIFY(button->isEnabled());

    bool inspected = false;
    QTimer::singleShot(0, [&]() {
        auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
        QVERIFY2(dialog != nullptr, "HFS file dialog should open");
        auto* mode = findAccessibleWidget<QComboBox>(dialog,
                                                     QStringLiteral("HFS file mutation mode"));
        QVERIFY(mode != nullptr);
        const int modeIndex = mode->findText(QStringLiteral("Rename or move catalog entry"));
        QVERIFY2(modeIndex >= 0, "HFS rename/move mode should exist");
        mode->setCurrentIndex(modeIndex);

        auto* hfsPath = findAccessibleWidget<QLineEdit>(dialog, QStringLiteral("HFS file path"));
        QVERIFY(hfsPath != nullptr);
        QVERIFY(hfsPath->isVisible());
        hfsPath->setText(QStringLiteral("/panel-source.txt"));

        auto* destination = findAccessibleWidget<QLineEdit>(dialog,
                                                            QStringLiteral("HFS destination path"));
        QVERIFY(destination != nullptr);
        QVERIFY(destination->isVisible());
        QVERIFY(destination->isEnabled());
        destination->setText(QStringLiteral("/Panel Folder/panel-renamed.txt"));

        auto* payload =
            findAccessibleWidget<QTextEdit>(dialog, QStringLiteral("HFS mutation payload text"));
        QVERIFY(payload != nullptr);
        QVERIFY(!payload->isVisible());
        QVERIFY(!payload->isEnabled());

        auto* preview = findAccessibleWidget<QLabel>(dialog, QStringLiteral("Operation preview"));
        QVERIFY(preview != nullptr);
        QVERIFY(preview->text().contains(QStringLiteral("catalog rename/move")));

        auto* confirm = findAccessibleWidget<QCheckBox>(
            dialog, QStringLiteral("Confirm HFS staged file mutation"));
        QVERIFY(confirm != nullptr);
        confirm->setChecked(true);
        QApplication::processEvents();

        auto* buttons = dialog->findChild<QDialogButtonBox*>();
        QVERIFY(buttons != nullptr);
        auto* accept = buttons->button(QDialogButtonBox::Ok);
        QVERIFY(accept != nullptr);
        QVERIFY(accept->isEnabled());

        inspected = true;
        dialog->accept();
    });
    button->click();
    QVERIFY(inspected);
    verifySingleQueuedOperation(&panel, QStringLiteral("HFS Rename/Move Catalog Entry"));
}

void queueHfsEmptyFileMutationAndVerify(const QString& modeText,
                                        const QString& hfsPath,
                                        const QString& expectedQueueText,
                                        const QString& expectedPreviewText) {
    sak::PartitionManagerPanel panel;
    configureRawWritePanel(&panel, QStringLiteral("HFS+"));
    QApplication::processEvents();

    auto* button = findToolButtonByName(&panel, QStringLiteral("HFS File"));
    QVERIFY2(button != nullptr, "HFS File action should exist");
    QVERIFY(button->isEnabled());

    bool inspected = false;
    QTimer::singleShot(0, [&]() {
        auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
        QVERIFY2(dialog != nullptr, "HFS file dialog should open");
        auto* mode = findAccessibleWidget<QComboBox>(dialog,
                                                     QStringLiteral("HFS file mutation mode"));
        QVERIFY(mode != nullptr);
        const int modeIndex = mode->findText(modeText);
        QVERIFY2(modeIndex >= 0, "HFS empty-file mutation mode should exist");
        mode->setCurrentIndex(modeIndex);

        auto* hfsPathInput = findAccessibleWidget<QLineEdit>(dialog,
                                                             QStringLiteral("HFS file path"));
        QVERIFY(hfsPathInput != nullptr);
        QVERIFY(hfsPathInput->isVisible());
        QVERIFY(hfsPathInput->isEnabled());
        hfsPathInput->setText(hfsPath);

        auto* destination = findAccessibleWidget<QLineEdit>(dialog,
                                                            QStringLiteral("HFS destination path"));
        QVERIFY(destination != nullptr);
        QVERIFY(!destination->isVisible());
        QVERIFY(!destination->isEnabled());

        auto* payload =
            findAccessibleWidget<QTextEdit>(dialog, QStringLiteral("HFS mutation payload text"));
        QVERIFY(payload != nullptr);
        QVERIFY(!payload->isVisible());
        QVERIFY(!payload->isEnabled());

        auto* fileId = findAccessibleWidget<QLineEdit>(dialog,
                                                       QStringLiteral("HFS attribute file ID"));
        QVERIFY(fileId != nullptr);
        QVERIFY(!fileId->isVisible());
        auto* attributeName = findAccessibleWidget<QLineEdit>(dialog,
                                                              QStringLiteral("HFS attribute name"));
        QVERIFY(attributeName != nullptr);
        QVERIFY(!attributeName->isVisible());

        auto* secureWipe = findAccessibleWidget<QCheckBox>(
            dialog, QStringLiteral("Zero released HFS blocks before delete"));
        QVERIFY(secureWipe != nullptr);
        const bool secureWipeExpected = modeText == QStringLiteral("Delete file") ||
                                        modeText == QStringLiteral("Delete folder tree");
        QCOMPARE(secureWipe->isVisible(), secureWipeExpected);
        QCOMPARE(secureWipe->isEnabled(), secureWipeExpected);
        if (secureWipeExpected) {
            secureWipe->setChecked(true);
            QApplication::processEvents();
        }

        auto* preview = findAccessibleWidget<QLabel>(dialog, QStringLiteral("Operation preview"));
        QVERIFY(preview != nullptr);
        QVERIFY(preview->text().contains(expectedPreviewText));
        if (secureWipeExpected) {
            QVERIFY(preview->text().contains(QStringLiteral("Released blocks will be zeroed")));
        }

        auto* confirm = findAccessibleWidget<QCheckBox>(
            dialog, QStringLiteral("Confirm HFS staged file mutation"));
        QVERIFY(confirm != nullptr);
        confirm->setChecked(true);
        QApplication::processEvents();

        auto* buttons = dialog->findChild<QDialogButtonBox*>();
        QVERIFY(buttons != nullptr);
        auto* accept = buttons->button(QDialogButtonBox::Ok);
        QVERIFY(accept != nullptr);
        QVERIFY(accept->isEnabled());

        inspected = true;
        dialog->accept();
    });
    button->click();
    QVERIFY(inspected);
    verifySingleQueuedOperation(&panel, expectedQueueText);
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
    verifySingleQueuedOperation(&panel, QStringLiteral("Resize Partition"));
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
        QCOMPARE(target->currentIndex(), 0);
        inspected = true;
        dialog->accept();
    });
    auto* button = findToolButtonByName(&panel, QStringLiteral("Allocate Free Space"));
    QVERIFY2(button != nullptr, "Allocate Free Space action should exist");
    button->click();
    QVERIFY(inspected);
    verifySingleQueuedOperation(&panel, QStringLiteral("Resize"));
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
    verifySingleQueuedOperation(&panel, QStringLiteral("Move Partition"));
}

struct ExpectedMetadataAction {
    QString button;
    QString dialog_name;
    QString backup_accessible_name;
    QString confirm_accessible_name;
    QString queued_text;
    bool dynamic_disk{false};
    int selected_row{1};
};

QVector<ExpectedMetadataAction> expectedMetadataActions() {
    return {{QStringLiteral("Convert Primary/Logical"),
             QStringLiteral("Convert Primary/Logical"),
             QStringLiteral("Primary logical backup directory"),
             QStringLiteral("Confirm primary logical backup and restore"),
             QStringLiteral("Convert Primary/Logical")},
            {QStringLiteral("Change Serial Number"),
             QStringLiteral("Change Serial Number"),
             QStringLiteral("Volume serial backup directory"),
             QStringLiteral("Confirm volume serial backup and restore"),
             QStringLiteral("Change Volume Serial Number")},
            {QStringLiteral("Convert Dynamic Disk to Basic"),
             QStringLiteral("Convert Dynamic Disk to Basic"),
             QStringLiteral("Dynamic to basic backup directory"),
             QStringLiteral("Confirm dynamic to basic backup and restore"),
             QStringLiteral("Convert Dynamic Disk to Basic"),
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
    QVERIFY(!button->toolTip().contains(QStringLiteral("blocked"), Qt::CaseInsensitive));

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
    QVERIFY(!statusSpy.isEmpty());
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
    QCOMPARE(table->itemDelegate()->objectName(), QStringLiteral("partitionDiskSeparatorDelegate"));
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
    for (const auto* action : actions) {
        QVERIFY2(action->maximumHeight() <= 22, "Sidebar text links should be compact");
        QCOMPARE(action->toolButtonStyle(), Qt::ToolButtonTextBesideIcon);
        QVERIFY2(action->styleSheet().contains(QStringLiteral("background: transparent")),
                 "Sidebar text links should not render as filled buttons");
        QVERIFY2(action->styleSheet().contains(QStringLiteral("border: none")),
                 "Sidebar text links should not render button borders");
    }
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
    sak::PartitionManagerPanel panel;

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
        QCOMPARE(button->minimumSize(), button->maximumSize());
        QCOMPARE(button->property("ribbonButtonWidth").toInt(), button->minimumWidth());
        QCOMPARE(button->property("ribbonButtonHeight").toInt(), button->minimumHeight());
        QVERIFY2(button->minimumHeight() >= 64,
                 "Ribbon buttons should have one consistent commercial-toolbar height");
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

    auto* row = panel.findChild<QWidget*>(QStringLiteral("partitionDiskMapRow"));
    QVERIFY2(row != nullptr, "Disk map should render disk rows");
    QVERIFY2(row->property("cornerRadius").toInt() >= 8,
             "Disk map row container should use rounded corners");
    QCOMPARE(row->contextMenuPolicy(), Qt::CustomContextMenu);
    auto* diskTile = panel.findChild<QWidget*>(QStringLiteral("partitionDiskMapDiskTile"));
    QVERIFY2(diskTile != nullptr, "Disk map should render disk tiles");
    QVERIFY2(diskTile->property("cornerRadius").toInt() >= 8,
             "Disk tile container should use rounded corners");
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
    QVERIFY2(table->rowCount() >= 2, "Fixture should create disk and partition rows");
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
    target.disk_number = 0;
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
    target.disk_number = 0;
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
    QVERIFY2(
        hasActionButton(buttons,
                        QStringLiteral("Data Recovery"),
                        QStringLiteral("Recover files from an image or raw volume/device path")),
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
    QVERIFY(browseButton->toolTip().contains(QStringLiteral("Browse read-only ext4")));

    bool inspected = false;
    QTimer::singleShot(0, [&]() {
        auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
        QVERIFY2(dialog != nullptr, "Properties dialog should open");
        auto* properties =
            dialog->findChild<QTableWidget*>(QStringLiteral("partitionPropertiesTable"));
        QVERIFY2(properties != nullptr, "Properties table should exist");
        const QString metadata = propertyTableValue(properties,
                                                    QStringLiteral("File system metadata"));
        QVERIFY(metadata.contains(QStringLiteral("Block size: 4096")));
        QVERIFY(metadata.contains(QStringLiteral("Volume label: SAK_EXT4")));
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
    QVERIFY(checkButton->toolTip().contains(QStringLiteral("original read-only")));
    QVERIFY(checkButton->toolTip().contains(QStringLiteral("metadata consistency")));
    verifyMetadataCheckDialog(checkButton, QStringLiteral("Metadata sanity: XFS"));
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
    QVERIFY(apfsCheck->toolTip().contains(QStringLiteral("original read-only")));
    verifyMetadataCheckDialog(apfsCheck, QStringLiteral("Metadata sanity: APFS"));
}

void PartitionManagerPanelTests::extFilesystemWriteActionsQueueWithConfirmation() {
    queueExtFormatAndVerify();
    queueLinuxSwapFormatAndVerify();
    queueApfsFormatAndVerify();
    queueExtRepairAndVerify();
    queueHfsRepairAndVerify();
    queueGeneratedApfsRepairAndVerify();
    queueExtResizeAndVerify(true);
    queueExtResizeAndVerify(false);
}

void PartitionManagerPanelTests::apfsRootFileMutationActionGatesGeneratedLayouts() {
    queueApfsRootFileMutationAndVerify();

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

    auto* button = findToolButtonByName(&panel, QStringLiteral("APFS File"));
    QVERIFY2(button != nullptr, "APFS File action should exist");
    QVERIFY(!button->isEnabled());
    QVERIFY(button->toolTip().contains(QStringLiteral("S.A.K. generated APFS layouts")));
}

void PartitionManagerPanelTests::hfsFileMutationActionQueuesStagedWrite() {
    queueHfsFileMutationAndVerify();
    queueHfsAllocationGrowthMutationAndVerify();
    queueHfsCreateFileMutationAndVerify();
    queueHfsForkAttributeMutationAndVerify();
    queueHfsForkAttributeGrowthMutationAndVerify();
    queueHfsRenameMoveMutationAndVerify();

    sak::PartitionManagerPanel panel;
    configureRawWritePanel(&panel, QStringLiteral("APFS"));
    QApplication::processEvents();

    auto* button = findToolButtonByName(&panel, QStringLiteral("HFS File"));
    QVERIFY2(button != nullptr, "HFS File action should exist");
    QVERIFY(!button->isEnabled());
    QVERIFY(button->toolTip().contains(QStringLiteral("Select an HFS+ or HFSX partition")));
}

void PartitionManagerPanelTests::hfsEmptyFileMutationModesQueueWithoutPayload() {
    queueHfsEmptyFileMutationAndVerify(QStringLiteral("Create empty file"),
                                       QStringLiteral("/panel-created.txt"),
                                       QStringLiteral("HFS Create Empty File"),
                                       QStringLiteral("empty-file create"));
    queueHfsEmptyFileMutationAndVerify(QStringLiteral("Delete empty file"),
                                       QStringLiteral("/panel-created.txt"),
                                       QStringLiteral("HFS Delete Empty File"),
                                       QStringLiteral("empty-file delete"));
    queueHfsEmptyFileMutationAndVerify(QStringLiteral("Delete file"),
                                       QStringLiteral("/panel-created.txt"),
                                       QStringLiteral("HFS Delete File"),
                                       QStringLiteral("allocated-file delete"));
    queueHfsEmptyFileMutationAndVerify(QStringLiteral("Create empty folder"),
                                       QStringLiteral("/Panel Folder"),
                                       QStringLiteral("HFS Create Empty Folder"),
                                       QStringLiteral("empty-folder create"));
    queueHfsEmptyFileMutationAndVerify(QStringLiteral("Delete empty folder"),
                                       QStringLiteral("/Panel Folder"),
                                       QStringLiteral("HFS Delete Empty Folder"),
                                       QStringLiteral("empty-folder delete"));
    queueHfsEmptyFileMutationAndVerify(QStringLiteral("Delete folder tree"),
                                       QStringLiteral("/Panel Folder"),
                                       QStringLiteral("HFS Delete Folder Tree"),
                                       QStringLiteral("folder-tree delete"));
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
        QVERIFY(propertyTableValue(status, QStringLiteral("Safe commands"))
                    .contains(QStringLiteral("manage-bde.exe -protectors -disable")));
        QVERIFY(propertyTableValue(status, QStringLiteral("In-app mutation"))
                    .contains(QStringLiteral("Queue unlock")));
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
        QVERIFY(propertyTableValue(status, QStringLiteral("Safe commands"))
                    .contains(QStringLiteral("Optimize-Volume -DriveLetter T -Defrag")));
        QVERIFY(propertyTableValue(status, QStringLiteral("In-app HDD defrag execution"))
                    .contains(QStringLiteral("Queue HDD defrag")));
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
        QVERIFY(propertyTableValue(status, QStringLiteral("Secure erase status"))
                    .contains(QStringLiteral("Ready")));
        QVERIFY(propertyTableValue(status, QStringLiteral("In-app ATA/NVMe purge"))
                    .contains(QStringLiteral("ReTrim")));
        QVERIFY(propertyTableValue(status, QStringLiteral("In-app ATA/NVMe purge"))
                    .contains(QStringLiteral("clear-level disk wipe")));
        QVERIFY(propertyTableValue(status, QStringLiteral("Evidence checklist"))
                    .contains(QStringLiteral("Disposable non-system SSD/NVMe media only")));
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

    const QJsonArray views = result.value(QStringLiteral("view_names")).toArray();
    QVERIFY(views.contains(QStringLiteral("Tree View")));
    QVERIFY(views.contains(QStringLiteral("Largest Files")));
    QVERIFY(views.contains(QStringLiteral("File Types")));

    const QJsonArray actions = result.value(QStringLiteral("context_actions")).toArray();
    QVERIFY(actions.contains(QStringLiteral("Open")));
    QVERIFY(actions.contains(QStringLiteral("Explore in File Explorer")));
    QVERIFY(actions.contains(QStringLiteral("Copy Path")));

    QStringList fileTypes;
    for (const auto& value : result.value(QStringLiteral("file_types")).toArray()) {
        fileTypes.append(value.toObject().value(QStringLiteral("name")).toString());
    }
    QVERIFY(fileTypes.contains(QStringLiteral(".bin")));
    QVERIFY(fileTypes.contains(QStringLiteral(".log")));
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
        QVERIFY(cluster->currentData().toULongLong() != 0);

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
    QVERIFY(queue->item(0)->text().contains(QStringLiteral("Change Cluster Size")));
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
    QVERIFY(queue->item(0)->text().contains(QStringLiteral("Allocate Free Space")));
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
    QVERIFY2(result.has_non_native_file_systems, qPrintable(result.file_system_items));
    QVERIFY2(result.raw_create_controls_toggle, qPrintable(result.raw_toggle_state));
    QVERIFY(result.swap_create_controls_toggle);
    QVERIFY(result.size_synced);
    QVERIFY(result.location_synced);
    QVERIFY(result.preview_drag_synced);
}

QTEST_MAIN(PartitionManagerPanelTests)

#include "test_partition_manager_panel.moc"
