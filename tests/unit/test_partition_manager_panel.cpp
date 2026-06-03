// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/partition_manager_panel.h"
#include "sak/style_constants.h"

#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QDialog>
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
#include <QMouseEvent>
#include <QPushButton>
#include <QScrollArea>
#include <QSlider>
#include <QSpinBox>
#include <QSplitter>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QTemporaryDir>
#include <QTimer>
#include <QToolButton>
#include <QtTest/QtTest>

#include <algorithm>

class PartitionManagerPanelTests : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void partitionTableUsesAomeiListChrome();
    void sidebarIsFixedAndHasNoRedundantPreviewBox();
    void sidebarActionsRenderAsCompactTextLinks();
    void partitionOperationsScrollInsideGroup();
    void diskMapLegendContainsCommercialColorRoles();
    void diskMapLegendColorsMatchRenderedRoles();
    void ribbonButtonsUseIcons8SvgSources();
    void diskMapUsesCompactSpacing();
    void diskMapHighlightsOnlySelectedPartition();
    void diskMapSegmentsSelectMatchingTableRows();
    void redoButtonEnablesOnlyAfterUndo();
    void diskMapRendersTypeColorInsideNeutralShell();
    void bottomDiskMapCanResizeIntoTableSpace();
    void finalApplyReviewContainsLayoutDiff();
    void propertiesActionIsFirstClass();
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

struct CreateDialogInspection {
    bool inspected{false};
    bool has_size_handle{false};
    bool has_location_handle{false};
    bool size_synced{false};
    bool location_synced{false};
    bool preview_drag_synced{false};
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

void inspectCreateDialog(QDialog* dialog, CreateDialogInspection* result) {
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
    if (sizeHandle && locationHandle && sizeSpin && locationSpin && sizePreview) {
        sizeHandle->setValue(kCreateDialogSizeMegabytes);
        locationHandle->setValue(kCreateDialogBeforeMegabytes);
        result->size_synced = sizeSpin->value() == kCreateDialogSizeMegabytes;
        result->location_synced = locationSpin->value() == kCreateDialogBeforeMegabytes;
        QApplication::processEvents();
        dragPreviewHandle(sizePreview);
        result->preview_drag_synced = sizeSpin->value() >= kPreviewDragExpectedSizeMegabytes &&
                                      locationSpin->value() == kCreateDialogBeforeMegabytes;
    }
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

}  // namespace

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
    for (const auto* swatch : swatches) {
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

    QHash<QString, QString> legendColors;
    const auto swatches = panel.findChildren<QFrame*>(QStringLiteral("partitionLegendSwatch"));
    for (const auto* swatch : swatches) {
        legendColors.insert(swatch->property("colorRole").toString(),
                            swatch->property("colorValue").toString());
    }

    QHash<QString, QString> segmentColors;
    const auto segments = panel.findChildren<QWidget*>(QStringLiteral("partitionDiskMapSegment"));
    for (const auto* segment : segments) {
        segmentColors.insert(segment->property("innerColorRole").toString(),
                             segment->property("innerColorValue").toString());
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

void PartitionManagerPanelTests::ribbonButtonsUseIcons8SvgSources() {
    sak::PartitionManagerPanel panel;

    const QHash<QString, QString> expectedIcons{
        {QStringLiteral("Apply"), QStringLiteral(":/icons/icons/icons8-pm-apply.svg")},
        {QStringLiteral("Discard"), QStringLiteral(":/icons/icons/icons8-pm-discard.svg")},
        {QStringLiteral("Undo"), QStringLiteral(":/icons/icons/icons8-pm-undo.svg")},
        {QStringLiteral("Redo"), QStringLiteral(":/icons/icons/icons8-pm-redo.svg")},
        {QStringLiteral("Reload"), QStringLiteral(":/icons/icons/icons8-pm-refresh.svg")},
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
    auto* diskTile = panel.findChild<QWidget*>(QStringLiteral("partitionDiskMapDiskTile"));
    QVERIFY2(diskTile != nullptr, "Disk map should render disk tiles");
    QVERIFY2(diskTile->property("cornerRadius").toInt() >= 8,
             "Disk tile container should use rounded corners");
    auto* segment = panel.findChild<QWidget*>(QStringLiteral("partitionDiskMapSegment"));
    QVERIFY2(segment != nullptr, "Disk map should render partition segments");
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
                        QStringLiteral("Recover files from an image or raw volume/device path"),
                        true),
        "Data Recovery should expose image and raw path recovery");
    QVERIFY2(hasActionButton(buttons,
                             QStringLiteral("Disk Benchmark"),
                             QStringLiteral("Open Benchmark and Diagnostics"),
                             true),
             "Disk Benchmark should route to the existing benchmark panel");
    QVERIFY2(hasActionButton(buttons,
                             QStringLiteral("Make Bootable Media"),
                             QStringLiteral("Open Image Flasher"),
                             true),
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
                             QStringLiteral("Review BitLocker status and open Windows management"),
                             true),
             "Manage BitLocker should show in-app status before Windows management");
    QVERIFY2(hasActionButton(buttons,
                             QStringLiteral("Disk Defrag"),
                             QStringLiteral("Review defrag/ReTrim commands and open Windows "
                                            "Optimize Drives"),
                             true),
             "Disk Defrag should show in-app guidance before Windows Optimize Drives");
    QVERIFY2(hasActionButton(buttons,
                             QStringLiteral("SSD Secure Erase"),
                             QStringLiteral("Queue SSD/NVMe ReTrim plus clear-level wipe"),
                             true),
             "SSD Secure Erase should expose a queued ReTrim and wipe path");
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
    {
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

        auto* queue = panel.findChild<QListWidget*>();
        QVERIFY2(queue != nullptr, "Pending operation queue should exist");
        QCOMPARE(queue->count(), 1);
        QVERIFY(queue->item(0)->text().contains(QStringLiteral("Resize")));
    }

    {
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

        auto* queue = panel.findChild<QListWidget*>();
        QVERIFY2(queue != nullptr, "Pending operation queue should exist");
        QCOMPARE(queue->count(), 1);
        QVERIFY(queue->item(0)->text().contains(QStringLiteral("Move Partition")));
    }
}

void PartitionManagerPanelTests::formerCommercialCompatibilityActionsQueueDirectEngines() {
    QTemporaryDir backupRoot;
    QVERIFY(backupRoot.isValid());

    struct ExpectedAction {
        QString button;
        QString dialog_name;
        QString backup_accessible_name;
        QString confirm_accessible_name;
        QString queued_text;
        bool dynamic_disk{false};
        int selected_row{1};
    };
    const QVector<ExpectedAction> expected{
        {QStringLiteral("Convert Primary/Logical"),
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
         0},
    };

    for (const auto& action : expected) {
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
            backup->setText(backupRoot.path());

            auto* confirm = findAccessibleWidget<QCheckBox>(dialog, action.confirm_accessible_name);
            QVERIFY2(confirm != nullptr, qPrintable(action.confirm_accessible_name));
            confirm->setChecked(true);
            inspected = true;
            dialog->accept();
        });

        button->click();
        QVERIFY(inspected);

        auto* queue = panel.findChild<QListWidget*>();
        QVERIFY2(queue != nullptr, "Pending operation queue should exist");
        QCOMPARE(queue->count(), 1);
        QVERIFY(queue->item(0)->text().contains(action.queued_text));
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
    QVERIFY(result.size_synced);
    QVERIFY(result.location_synced);
    QVERIFY(result.preview_drag_synced);
}

QTEST_MAIN(PartitionManagerPanelTests)

#include "test_partition_manager_panel.moc"
