// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file partition_manager_panel.cpp
/// @brief Partition Manager panel UI.

#include "sak/partition_manager_panel.h"

#include "sak/config_manager.h"
#include "sak/detachable_log_window.h"
#include "sak/file_recovery_engine.h"
#include "sak/layout_constants.h"
#include "sak/message_box_helpers.h"
#include "sak/partition_apfs_file_system_reader.h"
#include "sak/partition_ext_file_system_reader.h"
#include "sak/partition_file_system_detector.h"
#include "sak/partition_file_system_registry.h"
#include "sak/partition_file_system_tool_manifest.h"
#include "sak/partition_file_system_tool_runner.h"
#include "sak/partition_hfs_file_system_reader.h"
#include "sak/style_constants.h"

#include <QAbstractButton>
#include <QAbstractItemView>
#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QDesktopServices>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QFrame>
#include <QFutureWatcher>
#include <QGroupBox>
#include <QHash>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QIcon>
#include <QInputDialog>
#include <QJsonArray>
#include <QJsonDocument>
#include <QKeyEvent>
#include <QLabel>
#include <QLinearGradient>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPalette>
#include <QPixmap>
#include <QProcess>
#include <QProgressBar>
#include <QPushButton>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QSizePolicy>
#include <QSlider>
#include <QSpinBox>
#include <QSplitter>
#include <QStyledItemDelegate>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QTabWidget>
#include <QtConcurrent>
#include <QTextEdit>
#include <QTimer>
#include <QToolButton>
#include <QUrl>
#include <QVariantMap>
#include <QVBoxLayout>
#include <QWizard>
#include <QWizardPage>

#include <algorithm>
#include <atomic>
#include <functional>
#include <initializer_list>
#include <limits>
#include <memory>
#include <utility>

namespace sak {

namespace {

constexpr int kRuntimeManifestSourceFallbackDepth = 3;

enum TableColumn {
    ColPartition = 0,
    ColFileSystem,
    ColCapacity,
    ColUsed,
    ColUnused,
    ColFlags,
    ColStatus,
    ColCount
};

enum PropertyColumn {
    PropertyColName = 0,
    PropertyColValue,
    PropertyColCount
};

enum SpaceAnalyzerColumn {
    SpaceAnalyzerColName = 0,
    SpaceAnalyzerColType,
    SpaceAnalyzerColSize,
    SpaceAnalyzerColPercent,
    SpaceAnalyzerColPath,
    SpaceAnalyzerColCount
};

enum FileRecoveryColumn {
    FileRecoveryColName = 0,
    FileRecoveryColFormat,
    FileRecoveryColSize,
    FileRecoveryColOffset,
    FileRecoveryColCount
};

constexpr int kDefaultCreateSizeMb = 1024;
constexpr int kMaxSizeInputMb = 4 * 1024 * 1024;
constexpr int kMegabyteBytes = 1024 * 1024;
constexpr int kMaxPartitionNumberInput = 256;
constexpr int kActionsPaneWidth = 304;
constexpr int kRibbonIconSize = 30;
constexpr int kRibbonButtonWidth = 74;
constexpr int kRibbonButtonHeight = 72;
constexpr int kActionIconSize = 16;
constexpr int kActionLinkHeight = 22;
constexpr int kDiskIconSize = 34;
constexpr int kDiskTileWidth = 86;
constexpr int kDiskTileMinHeight = 86;
constexpr int kDiskMapMinHeight = 230;
constexpr int kDiskMapOuterMargin = 1;
constexpr int kDiskMapRowSpacing = 2;
constexpr int kDiskMapSegmentSpacing = 2;
constexpr int kDiskMapRowInnerMargin = 1;
constexpr int kDiskMapRowRadius = 8;
constexpr int kPendingQueueMinHeight = 92;
constexpr int kPartitionTableRowHeight = 30;
constexpr int kPartitionProgressMax = 100;
constexpr int kDiskMapStretchScale = 1000;
constexpr int kDiskMapMinSegmentStretch = 1;
constexpr int kPartitionSegmentMinWidth = 112;
constexpr int kPartitionSegmentMinHeight = kDiskTileMinHeight;
constexpr int kPartitionSegmentTrackHeight = 18;
constexpr int kPartitionSegmentRadius = 6;
constexpr int kPartitionSegmentFillLightness = 160;
constexpr int kPartitionSegmentTrackLightness = 118;
constexpr int kPartitionSegmentBorderDarkness = 130;
constexpr int kPartitionSegmentSelectedBorderWidth = 3;
constexpr int kPartitionSegmentLabelTopPad = 4;
constexpr int kPartitionLegendSwatchRadius = 2;
constexpr int kTableDiskSeparatorWidth = 1;
constexpr int kWorkspaceInitialTableHeight = 360;
constexpr int kWorkspaceInitialMapHeight = 240;
constexpr int kOperationDialogMinWidth = 460;
constexpr int kApplyReviewDialogMinWidth = 620;
constexpr int kApplyReviewDialogMinHeight = 420;
constexpr int kApplyDiffPreviewMinHeight = 130;
constexpr int kApplyDiffPreviewRowHeight = 24;
constexpr int kApplyDiffPreviewTitleWidth = 58;
constexpr int kApplyDiffPreviewRowCount = 2;
constexpr int kHorizontalMarginCount = 2;
constexpr int kCenteringDivisor = 2;
constexpr int kOperationSizePreviewMinHeight = 82;
constexpr int kOperationSizePreviewRowHeight = 24;
constexpr int kOperationSizePreviewLabelWidth = 92;
constexpr int kSizePreviewNoInteractiveRow = -1;
constexpr int kSizePreviewCreateRow = 0;
constexpr int kSizePreviewTargetRow = 1;
constexpr int kSizePreviewHandleHalfWidth = 5;
constexpr int kSizePreviewHandleHitWidth = 12;
constexpr int kSizeHandleSingleStepMb = 1;
constexpr int kSizeHandlePageStepMb = 1024;
constexpr int kPropertiesDialogMinWidth = 560;
constexpr int kPropertiesDialogMinHeight = 420;
constexpr int kPropertiesColumnCount = PropertyColCount;
constexpr int kExtBrowserColumnCount = 6;
constexpr int kExtBrowserColumnName = 0;
constexpr int kExtBrowserColumnType = 1;
constexpr int kExtBrowserColumnSize = 2;
constexpr int kExtBrowserColumnInode = 3;
constexpr int kExtBrowserColumnLinkTarget = 4;
constexpr int kExtBrowserColumnPath = 5;
constexpr int kHfsBrowserColumnCount = 6;
constexpr int kHfsBrowserColumnName = 0;
constexpr int kHfsBrowserColumnType = 1;
constexpr int kHfsBrowserColumnSize = 2;
constexpr int kHfsBrowserColumnResourceSize = 3;
constexpr int kHfsBrowserColumnCatalogId = 4;
constexpr int kHfsBrowserColumnPath = 5;
constexpr qsizetype kHfsAttributeExportNameMaxChars = 180;
constexpr int kApfsBrowserColumnCount = 5;
constexpr int kApfsBrowserColumnName = 0;
constexpr int kApfsBrowserColumnType = 1;
constexpr int kApfsBrowserColumnSize = 2;
constexpr int kApfsBrowserColumnObjectId = 3;
constexpr int kApfsBrowserColumnPath = 4;
constexpr uint64_t kExtBrowserExtractMaxBytes = 512ULL * 1024ULL * 1024ULL;
constexpr int kNonNativeBrowserExportMaxEntries = 10'000;
constexpr uint64_t kNonNativeBrowserExportMaxTotalBytes = 4ULL * 1024ULL * 1024ULL * 1024ULL;
constexpr int kBitLockerDialogMinWidth = 620;
constexpr int kBitLockerDialogMinHeight = 420;
constexpr int kOptimizeDialogMinWidth = 620;
constexpr int kOptimizeDialogMinHeight = 420;
constexpr int kSecureEraseDialogMinWidth = 640;
constexpr int kSecureEraseDialogMinHeight = 440;
constexpr int kSpaceAnalyzerColumnCount = SpaceAnalyzerColCount;
constexpr int kFileRecoveryColumnCount = FileRecoveryColCount;
constexpr int kClonePreviewMinHeight = 136;
constexpr int kClonePreviewRowHeight = 48;
constexpr int kClonePreviewLabelWidth = 82;
constexpr int kClonePreviewBarHeight = 22;
constexpr int kClonePreviewTitleGap = 8;
constexpr int kSpaceAnalyzerDialogMinWidth = 720;
constexpr int kSpaceAnalyzerDialogMinHeight = 520;
constexpr int kSpaceAnalyzerPathPreviewChars = 90;
constexpr int kSpaceAnalyzerMaxFileRows = 500;
constexpr int kSpaceAnalyzerPathRole = Qt::UserRole + 1;
constexpr int kSpaceAnalyzerTreeViewIndex = 0;
constexpr int kSpaceAnalyzerLargestFilesViewIndex = 1;
constexpr int kSpaceAnalyzerFileTypesViewIndex = 2;
constexpr int kSpaceAnalyzerOpenActionIndex = 0;
constexpr int kSpaceAnalyzerExploreActionIndex = 1;
constexpr int kSpaceAnalyzerCopyActionIndex = 2;
constexpr int kFileRecoveryDialogMinWidth = 620;
constexpr int kFileRecoveryDialogMinHeight = 420;
constexpr int kApfsRootFilePayloadMinHeight = 96;

const QColor kPartitionColorGptPrimary{42, 151, 207};
const QColor kPartitionColorLogical{35, 196, 211};
const QColor kPartitionColorSimple{122, 183, 91};
const QColor kPartitionColorSpanned{198, 140, 48};
const QColor kPartitionColorStriped{188, 98, 193};
const QColor kPartitionColorMirrored{219, 201, 0};
const QColor kPartitionColorRaid5{226, 84, 74};
const QColor kPartitionColorUnallocated{70, 78, 88};
constexpr const char* kActionTargetKindsProperty = "partitionActionTargetKinds";
constexpr const char* kActionTargetAny = "any";
constexpr const char* kActionTargetDisk = "disk";
constexpr const char* kActionTargetPartition = "partition";
constexpr const char* kActionTargetUnallocated = "unallocated";
constexpr const char* kNonNativeFilesystemActionProperty = "partitionNonNativeFilesystemAction";
constexpr const char* kInspectNonNativeFilesystemActionProperty =
    "partitionInspectNonNativeFilesystemAction";
constexpr const char* kBrowseNonNativeFilesystemActionProperty =
    "partitionBrowseNonNativeFilesystemAction";
constexpr const char* kApfsRootFileMutationActionProperty = "partitionApfsRootFileMutationAction";
constexpr const char* kHfsFileMutationActionProperty = "partitionHfsFileMutationAction";
constexpr const char* kHfsCatalogCheckOperation = "hfs-catalog-check";
constexpr const char* kApfsRootFileWriteMode = "write";
constexpr const char* kApfsRootFilePatchMode = "patch";
constexpr const char* kApfsRootFileDeleteMode = "delete";
constexpr const char* kApfsRootDirectoryFileWriteMode = "write-directory-file";
constexpr const char* kApfsRootDirectoryFilePatchMode = "patch-directory-file";
constexpr const char* kApfsRootDirectoryFileDeleteMode = "delete-directory-file";
constexpr const char* kApfsRootDirectoryCreateMode = "create-directory";
constexpr const char* kApfsRootDirectoryDeleteMode = "delete-directory";
constexpr const char* kApfsVolumeLabelMode = "change-volume-label";
constexpr const char* kHfsOverwriteFileMode = "overwrite-file";
constexpr const char* kHfsReplaceFileMode = "replace-file";
constexpr const char* kHfsGrowFileMode = "grow-file";
constexpr const char* kHfsTruncateFileMode = "truncate-file";
constexpr const char* kHfsReplaceResourceForkMode = "replace-resource-fork";
constexpr const char* kHfsGrowResourceForkMode = "grow-resource-fork";
constexpr const char* kHfsTruncateResourceForkMode = "truncate-resource-fork";
constexpr const char* kHfsCreateEmptyFileMode = "create-empty-file";
constexpr const char* kHfsCreateFileMode = "create-file";
constexpr const char* kHfsDeleteEmptyFileMode = "delete-empty-file";
constexpr const char* kHfsDeleteFileMode = "delete-file";
constexpr const char* kHfsCreateEmptyFolderMode = "create-empty-folder";
constexpr const char* kHfsDeleteEmptyFolderMode = "delete-empty-folder";
constexpr const char* kHfsDeleteFolderTreeMode = "delete-folder-tree";
constexpr const char* kHfsRenameMoveCatalogEntryMode = "rename-move-catalog-entry";
constexpr const char* kHfsReplaceInlineAttributeMode = "replace-inline-attribute";
constexpr const char* kHfsReplaceForkAttributeMode = "replace-fork-attribute";
constexpr const char* kHfsGrowForkAttributeMode = "grow-fork-attribute";
constexpr const char* kActionDefaultTooltipProperty = "partitionActionDefaultTooltip";
constexpr const char* kActionRequiresDriveLetterProperty = "partitionActionRequiresDriveLetter";
constexpr const char* kActionWindowsNativeFilesystemProperty =
    "partitionActionWindowsNativeFilesystem";
constexpr const char* kActionResizeFilesystemProperty = "partitionActionResizeFilesystem";

QString partitionColorRole(const PartitionDiskInfo& disk, const PartitionInfoEx& partition) {
    const QString typeText =
        QStringLiteral("%1 %2").arg(partition.type_name, partition.gpt_type).toLower();
    if (disk.is_dynamic || typeText.contains(QStringLiteral("dynamic"))) {
        if (typeText.contains(QStringLiteral("raid"))) {
            return QStringLiteral("RAID5");
        }
        if (typeText.contains(QStringLiteral("mirror"))) {
            return QStringLiteral("Mirrored");
        }
        if (typeText.contains(QStringLiteral("stripe"))) {
            return QStringLiteral("Striped");
        }
        if (typeText.contains(QStringLiteral("span"))) {
            return QStringLiteral("Spanned");
        }
        return QStringLiteral("Simple");
    }
    if (typeText.contains(QStringLiteral("logical"))) {
        return QStringLiteral("Logical");
    }
    return QStringLiteral("GPT/Primary");
}

QColor partitionColorForRole(const QString& role) {
    if (role == QStringLiteral("Unallocated")) {
        return kPartitionColorUnallocated;
    }
    if (role == QStringLiteral("Logical")) {
        return kPartitionColorLogical;
    }
    if (role == QStringLiteral("Simple")) {
        return kPartitionColorSimple;
    }
    if (role == QStringLiteral("Spanned")) {
        return kPartitionColorSpanned;
    }
    if (role == QStringLiteral("Striped")) {
        return kPartitionColorStriped;
    }
    if (role == QStringLiteral("Mirrored")) {
        return kPartitionColorMirrored;
    }
    if (role == QStringLiteral("RAID5")) {
        return kPartitionColorRaid5;
    }
    return kPartitionColorGptPrimary;
}

QString diskSelectionColorRole(const PartitionDiskInfo& disk) {
    if (disk.is_dynamic) {
        return QStringLiteral("Simple");
    }
    if (disk.partitions.isEmpty() && !disk.unallocated_regions.isEmpty()) {
        return QStringLiteral("Unallocated");
    }
    return QStringLiteral("GPT/Primary");
}

QString targetKindToken(PartitionTargetKind kind) {
    switch (kind) {
    case PartitionTargetKind::Disk:
        return QString::fromLatin1(kActionTargetDisk);
    case PartitionTargetKind::Partition:
    case PartitionTargetKind::Volume:
        return QString::fromLatin1(kActionTargetPartition);
    case PartitionTargetKind::Unallocated:
        return QString::fromLatin1(kActionTargetUnallocated);
    }
    return {};
}

QStringList actionTargetKindList(std::initializer_list<const char*> kinds) {
    QStringList values;
    for (const auto* kind : kinds) {
        values.append(QString::fromLatin1(kind));
    }
    return values;
}

void setRequiresDriveLetter(QAbstractButton* button) {
    if (button) {
        button->setProperty(kActionRequiresDriveLetterProperty, true);
    }
}

void setWindowsNativeFilesystemAction(QAbstractButton* button) {
    if (button) {
        button->setProperty(kActionWindowsNativeFilesystemProperty, true);
    }
}

void setResizeFilesystemAction(QAbstractButton* button) {
    if (button) {
        button->setProperty(kActionResizeFilesystemProperty, true);
    }
}

bool targetMatchesDisk(const std::optional<PartitionTarget>& target,
                       const PartitionDiskInfo& disk) {
    return target && target->kind == PartitionTargetKind::Disk &&
           target->disk_number == disk.disk_number;
}

bool buttonAllowsTarget(const QAbstractButton* button,
                        const std::optional<PartitionTarget>& target) {
    if (!button || !target) {
        return false;
    }
    const QStringList targets = button->property(kActionTargetKindsProperty).toStringList();
    if (targets.contains(QString::fromLatin1(kActionTargetAny))) {
        return true;
    }
    return targets.contains(targetKindToken(target->kind));
}

bool isNonNativeFilesystemAction(const QAbstractButton* button) {
    return button && button->property(kNonNativeFilesystemActionProperty).toBool();
}

bool isInspectNonNativeFilesystemAction(const QAbstractButton* button) {
    return button && button->property(kInspectNonNativeFilesystemActionProperty).toBool();
}

bool isBrowseNonNativeFilesystemAction(const QAbstractButton* button) {
    return button && button->property(kBrowseNonNativeFilesystemActionProperty).toBool();
}

bool isApfsRootFileMutationAction(const QAbstractButton* button) {
    return button && button->property(kApfsRootFileMutationActionProperty).toBool();
}

bool isHfsFileMutationAction(const QAbstractButton* button) {
    return button && button->property(kHfsFileMutationActionProperty).toBool();
}

bool isExtFilesystem(const QString& fileSystem);

struct ContextActionSpec {
    QString text;
    QString icon_path;
    bool enabled{true};
    QString disabled_reason;
};

struct PartitionActionAvailability {
    bool enabled{true};
    QString reason;
};

struct PartitionActionPolicy {
    bool requires_drive_letter{false};
    bool windows_native_filesystem{false};
    bool resize_filesystem{false};
};

PartitionActionPolicy driveLetterPolicy() {
    return {.requires_drive_letter = true};
}

PartitionActionPolicy windowsNativePolicy() {
    return {.requires_drive_letter = true, .windows_native_filesystem = true};
}

PartitionActionPolicy resizeFilesystemPolicy() {
    return {.resize_filesystem = true};
}

bool partitionHasDriveLetter(const PartitionInfoEx* partition) {
    return partition && partition->volume && !partition->volume->drive_letter.trimmed().isEmpty();
}

QString partitionFileSystemName(const PartitionInfoEx* partition) {
    if (!partition || !partition->volume) {
        return {};
    }
    return partition->volume->file_system.trimmed();
}

PartitionActionAvailability fileSystemActionAvailability(const QString& fileSystem,
                                                         const PartitionActionPolicy& policy) {
    if (fileSystem.isEmpty()) {
        return {false, QObject::tr("Selected partition does not report a file system.")};
    }

    const auto capability = PartitionFileSystemRegistry::capabilityFor(fileSystem);
    if (policy.windows_native_filesystem && capability.non_native) {
        return {false,
                QObject::tr("Use the Non-Windows filesystem actions for %1; Windows-native "
                            "file-system action is disabled.")
                    .arg(fileSystem)};
    }

    if (policy.resize_filesystem && capability.non_native && !isExtFilesystem(fileSystem)) {
        return {false,
                QObject::tr("%1 resize is not certified. Non-Windows resize currently supports "
                            "ext2/ext3/ext4 only.")
                    .arg(fileSystem)};
    }

    return {};
}

PartitionActionAvailability partitionActionAvailability(const PartitionInfoEx* partition,
                                                        const PartitionActionPolicy& policy) {
    if (policy.requires_drive_letter && !partitionHasDriveLetter(partition)) {
        return {false, QObject::tr("Selected partition has no mounted drive letter.")};
    }

    if (!policy.windows_native_filesystem && !policy.resize_filesystem) {
        return {};
    }

    return fileSystemActionAvailability(partitionFileSystemName(partition), policy);
}

PartitionActionAvailability buttonActionAvailability(const QAbstractButton* button,
                                                     const std::optional<PartitionTarget>& target,
                                                     const PartitionInfoEx* partition) {
    if (!button || !target) {
        return {false, QObject::tr("Select a disk, partition, or unallocated region first.")};
    }
    if (!buttonAllowsTarget(button, target)) {
        return {false, QObject::tr("Selected target does not support this action.")};
    }
    return partitionActionAvailability(
        partition,
        {button->property(kActionRequiresDriveLetterProperty).toBool(),
         button->property(kActionWindowsNativeFilesystemProperty).toBool(),
         button->property(kActionResizeFilesystemProperty).toBool()});
}

ContextActionSpec partitionContextActionSpec(const QString& text,
                                             const QString& iconPath,
                                             const PartitionInfoEx* partition,
                                             const PartitionActionPolicy& policy = {}) {
    const auto availability = partitionActionAvailability(partition, policy);
    return {text, iconPath, availability.enabled, availability.reason};
}

struct NonNativeFilesystemCheckState {
    bool enabled{false};
    bool tool_check_available{false};
    bool internal_metadata_check{false};
    bool hfs_consistency_check{false};
    bool repair_available{false};
    QString file_system;
    QString target_path;
    QStringList metadata_details;
    QString reason;
    QString repair_reason;
};

struct NonNativeFilesystemInspectState {
    bool enabled{false};
    QString file_system;
    QString reason;
};

struct NonNativeFilesystemBrowseState {
    bool enabled{false};
    QString file_system;
    QString target_path;
    QString reason;
};

struct NonNativeFilesystemCheckRequest {
    QString mode;
    QString target_path;
    bool destructive_confirmed{false};
};

struct ApfsRootFileMutationState {
    bool enabled{false};
    QString target_path;
    QString reason;
};

struct HfsFileMutationState {
    bool enabled{false};
    QString file_system;
    QString target_path;
    bool wrapped{false};
    bool journaled{false};
    QString reason;
};

bool isExtFilesystem(const QString& fileSystem) {
    const QString token = fileSystem.trimmed().toLower();
    return token == QStringLiteral("ext2") || token == QStringLiteral("ext3") ||
           token == QStringLiteral("ext4");
}

bool isLinuxSwapFilesystem(const QString& fileSystem) {
    const QString token = fileSystem.trimmed().toLower();
    return token == QStringLiteral("linux swap") || token == QStringLiteral("linux-swap") ||
           token == QStringLiteral("swap");
}

bool isHfsFilesystem(const QString& fileSystem) {
    const QString token = fileSystem.trimmed().toLower();
    return token == QStringLiteral("hfs+") || token == QStringLiteral("hfsx") ||
           token == QStringLiteral("hfsplus");
}

bool isApfsFilesystem(const QString& fileSystem) {
    return fileSystem.trimmed().compare(QStringLiteral("apfs"), Qt::CaseInsensitive) == 0;
}

bool isRepairableNonNativeCheckFilesystem(const QString& fileSystem) {
    return isExtFilesystem(fileSystem) || isHfsFilesystem(fileSystem) ||
           isApfsFilesystem(fileSystem);
}

bool partitionHasProtectedRole(const PartitionInfoEx& partition) {
    return partition.is_system || partition.is_boot || partition.is_efi || partition.is_recovery ||
           partition.is_msr;
}

bool partitionAllowsNonNativeRepair(const PartitionInfoEx& partition) {
    return !partitionHasProtectedRole(partition) && !partition.is_read_only;
}

bool hasGeneratedApfsRepairEvidence(const QStringList& details);

void applyNonNativeRepairState(NonNativeFilesystemCheckState* state,
                               const PartitionInfoEx& partition) {
    if (!state || !isRepairableNonNativeCheckFilesystem(state->file_system)) {
        return;
    }

    if (isApfsFilesystem(state->file_system) &&
        !hasGeneratedApfsRepairEvidence(state->metadata_details)) {
        state->repair_reason = QObject::tr(
            "APFS repair is enabled only for S.A.K. generated APFS layouts with captured "
            "layout evidence; arbitrary Apple APFS repair remains blocked.");
        return;
    }

    state->repair_available = partitionAllowsNonNativeRepair(partition);
    if (state->repair_available) {
        return;
    }

    state->repair_reason = QObject::tr(
        "Repair is disabled for system, boot, EFI, MSR, recovery, or read-only partitions.");
}

bool hasGeneratedApfsRepairEvidence(const QStringList& details) {
    const QString joined = details.join(QLatin1Char('\n'));
    return joined.contains(QStringLiteral("APFS space manager block: 10")) &&
           joined.contains(QStringLiteral("APFS volume candidate block 6")) &&
           joined.contains(QStringLiteral("volume object map OID 103")) &&
           joined.contains(QStringLiteral("root tree OID 104")) &&
           joined.contains(QStringLiteral("Volume OIDs: 102"));
}

bool hasInternalMetadataCheck(const QString& fileSystem) {
    const QString token = fileSystem.trimmed().toLower();
    return token == QStringLiteral("xfs") || token == QStringLiteral("btrfs") ||
           token == QStringLiteral("apfs");
}

bool hasMetadataSanityEvidence(const QStringList& details) {
    return std::any_of(details.cbegin(), details.cend(), [](const QString& detail) {
        return detail.startsWith(QStringLiteral("Metadata sanity:"), Qt::CaseInsensitive) ||
               detail.startsWith(QStringLiteral("Metadata sanity warning:"), Qt::CaseInsensitive);
    });
}

bool hasMetadataSanityWarnings(const QStringList& details) {
    return std::any_of(details.cbegin(), details.cend(), [](const QString& detail) {
        return detail.startsWith(QStringLiteral("Metadata sanity warning:"), Qt::CaseInsensitive);
    });
}

QStringList metadataSanityFindings(const QStringList& details) {
    QStringList findings;
    for (const auto& detail : details) {
        if (detail.startsWith(QStringLiteral("Metadata sanity:"), Qt::CaseInsensitive) ||
            detail.startsWith(QStringLiteral("Metadata sanity warning:"), Qt::CaseInsensitive)) {
            findings.append(detail);
        }
    }
    return findings;
}

bool fillInternalMetadataCheckState(NonNativeFilesystemCheckState* state) {
    if (!state || !hasInternalMetadataCheck(state->file_system)) {
        return false;
    }
    if (!hasMetadataSanityEvidence(state->metadata_details)) {
        state->reason = QObject::tr(
                            "No captured %1 metadata sanity evidence is available; refresh disk "
                            "inventory first.")
                            .arg(state->file_system);
        return true;
    }
    state->enabled = true;
    state->internal_metadata_check = true;
    state->reason =
        QObject::tr("Run original read-only %1 metadata consistency check from captured probe data")
            .arg(state->file_system);
    return true;
}

bool fillHfsConsistencyCheckState(NonNativeFilesystemCheckState* state) {
    if (!state || !isHfsFilesystem(state->file_system)) {
        return false;
    }
    state->enabled = true;
    state->hfs_consistency_check = true;
    state->reason =
        QObject::tr(
            "Run original read-only %1 catalog consistency check with attributes B-tree key scan")
            .arg(state->file_system);
    return true;
}

const PartitionVolumeInfo* selectedFilesystemVolume(const PartitionInfoEx* partition) {
    if (!partition || !partition->volume) {
        return nullptr;
    }
    const auto* volume = &partition->volume.value();
    return volume->file_system.trimmed().isEmpty() ? nullptr : volume;
}

QStringList nonNativeToolBlockers(const PartitionFileSystemCapability& capability,
                                  const QStringList& blockers) {
    QStringList reasons = blockers;
    reasons.append(capability.blocked_actions);
    if (!capability.required_tools.isEmpty()) {
        reasons.append(
            QObject::tr("Required bundled tools: %1")
                .arg(PartitionFileSystemRegistry::actionSummary(capability.required_tools)));
    }
    return reasons;
}

QString runtimeFilesystemManifestPath();

NonNativeFilesystemCheckState resolveNonNativeCheckToolState(
    NonNativeFilesystemCheckState state, const PartitionFileSystemCapability& capability) {
    const bool hfsConsistencyAvailable = fillHfsConsistencyCheckState(&state);

    const auto command = PartitionFileSystemToolRunner::buildReadOnlyCheckCommand(
        state.file_system, state.target_path);
    if (!command.ok()) {
        if (hfsConsistencyAvailable) {
            return state;
        }
        if (fillInternalMetadataCheckState(&state)) {
            return state;
        }
        state.reason = PartitionFileSystemRegistry::actionSummary(
            nonNativeToolBlockers(capability, command.blockers));
        return state;
    }

    const QString manifestPath = runtimeFilesystemManifestPath();
    const auto resolution =
        PartitionFileSystemToolRunner::resolveApprovedTool(manifestPath,
                                                           QFileInfo(manifestPath).absolutePath(),
                                                           command.tool_id,
                                                           command.operation,
                                                           command.file_system);
    if (resolution.ok) {
        state.enabled = true;
        state.tool_check_available = true;
        state.reason = QObject::tr("Run read-only check with %1 against %2")
                           .arg(resolution.tool.display_name, state.target_path);
        return state;
    }

    if (hfsConsistencyAvailable) {
        return state;
    }

    if (fillInternalMetadataCheckState(&state)) {
        return state;
    }

    state.reason = PartitionFileSystemRegistry::actionSummary(
        nonNativeToolBlockers(capability, resolution.blockers));
    return state;
}

QString runtimeFilesystemManifestPath() {
    QDir sourceCandidate = QDir::current();
    for (int depth = 0; depth < kRuntimeManifestSourceFallbackDepth; ++depth) {
        const QString sourceManifest =
            sourceCandidate.filePath(PartitionFileSystemToolManifest::defaultRuntimeRelativePath());
        const bool sourceCandidateLooksLikeSourceTree =
            QFileInfo::exists(sourceCandidate.filePath(QStringLiteral("CMakeLists.txt"))) &&
            QFileInfo::exists(
                sourceCandidate.filePath(QStringLiteral("src/gui/partition_manager_panel.cpp")));
        if (sourceCandidateLooksLikeSourceTree && QFileInfo::exists(sourceManifest)) {
            return sourceManifest;
        }
        if (!sourceCandidate.cdUp()) {
            break;
        }
    }

    const QString appManifest = PartitionFileSystemToolManifest::defaultRuntimeManifestPath(
        QApplication::applicationDirPath());
    if (QFileInfo::exists(appManifest)) {
        return appManifest;
    }
    return QDir::current().filePath(PartitionFileSystemToolManifest::defaultRuntimeRelativePath());
}

QString nonNativeFilesystemTargetPath(const std::optional<PartitionTarget>& target,
                                      const PartitionInfoEx* partition) {
    if (!target || !partition || partition->partition_number == 0) {
        return {};
    }
    if (!target->drive_letter.trimmed().isEmpty()) {
        return QStringLiteral("\\\\.\\%1:").arg(target->drive_letter.left(1).toUpper());
    }
    return QStringLiteral("\\\\?\\GLOBALROOT\\Device\\Harddisk%1\\Partition%2")
        .arg(partition->disk_number)
        .arg(partition->partition_number);
}

QString nonNativeFilesystemWriteTargetPath(const PartitionInfoEx* partition) {
    if (!partition || partition->partition_number == 0) {
        return {};
    }
    return QStringLiteral("\\\\?\\GLOBALROOT\\Device\\Harddisk%1\\Partition%2")
        .arg(partition->disk_number)
        .arg(partition->partition_number);
}

NonNativeFilesystemCheckState nonNativeFilesystemCheckState(
    const std::optional<PartitionTarget>& target, const PartitionInfoEx* partition) {
    NonNativeFilesystemCheckState state;
    const auto* volume = selectedFilesystemVolume(partition);
    if (!volume) {
        state.reason = QObject::tr("Select a partition with a detected non-Windows file system.");
        return state;
    }
    state.file_system = volume->file_system.trimmed();
    const auto capability = PartitionFileSystemRegistry::capabilityFor(state.file_system);
    if (!capability.non_native) {
        state.reason =
            QObject::tr("Selected file system uses Windows-native Partition Manager actions.");
        return state;
    }
    state.metadata_details = volume->file_system_details;

    state.target_path = nonNativeFilesystemTargetPath(target, partition);
    if (state.target_path.isEmpty()) {
        state.reason = QObject::tr("Selected partition does not expose a usable raw target path.");
        return state;
    }
    applyNonNativeRepairState(&state, *partition);

    return resolveNonNativeCheckToolState(state, capability);
}

ApfsRootFileMutationState apfsRootFileMutationState(const std::optional<PartitionTarget>& target,
                                                    const PartitionInfoEx* partition) {
    ApfsRootFileMutationState state;
    Q_UNUSED(target);
    const auto* volume = selectedFilesystemVolume(partition);
    if (!volume || !isApfsFilesystem(volume->file_system)) {
        state.reason = QObject::tr("Select an APFS partition.");
        return state;
    }
    if (!hasGeneratedApfsRepairEvidence(volume->file_system_details)) {
        state.reason = QObject::tr(
            "APFS generated file mutation is enabled only for S.A.K. generated APFS layouts.");
        return state;
    }
    if (!partitionAllowsNonNativeRepair(*partition)) {
        state.reason = QObject::tr(
            "APFS generated file mutation is disabled for system, boot, EFI, MSR, recovery, "
            "or read-only partitions.");
        return state;
    }
    state.target_path = nonNativeFilesystemWriteTargetPath(partition);
    if (state.target_path.isEmpty()) {
        state.reason = QObject::tr("Selected APFS partition does not expose a raw target path.");
        return state;
    }
    state.enabled = true;
    state.reason = QObject::tr("Queue generated APFS root-file write, patch, or delete.");
    return state;
}

bool detailsContainYes(const QStringList& details, const QString& key) {
    return details.join(QLatin1Char('\n')).contains(key + QStringLiteral(": Yes"));
}

HfsFileMutationState hfsFileMutationState(const std::optional<PartitionTarget>& target,
                                          const PartitionInfoEx* partition) {
    Q_UNUSED(target);
    HfsFileMutationState state;
    const auto* volume = selectedFilesystemVolume(partition);
    if (!volume || !isHfsFilesystem(volume->file_system)) {
        state.reason = QObject::tr("Select an HFS+ or HFSX partition.");
        return state;
    }
    if (!partitionAllowsNonNativeRepair(*partition)) {
        state.reason = QObject::tr(
            "HFS+ file mutation is disabled for system, boot, EFI, MSR, recovery, "
            "or read-only partitions.");
        return state;
    }
    state.target_path = nonNativeFilesystemWriteTargetPath(partition);
    if (state.target_path.isEmpty()) {
        state.reason = QObject::tr("Selected HFS+ partition does not expose a raw target path.");
        return state;
    }
    state.file_system = volume->file_system.trimmed();
    state.wrapped = detailsContainYes(volume->file_system_details, QStringLiteral("HFS wrapper"));
    state.journaled = detailsContainYes(volume->file_system_details, QStringLiteral("Journaled"));
    state.enabled = true;
    state.reason =
        QObject::tr("Queue HFS+ staged file, resource fork, or inline attribute mutation.");
    return state;
}

NonNativeFilesystemInspectState nonNativeFilesystemInspectState(const PartitionInfoEx* partition) {
    NonNativeFilesystemInspectState state;
    if (!partition || !partition->volume || partition->volume->file_system.trimmed().isEmpty()) {
        state.reason = QObject::tr("Select a partition with a detected non-Windows file system.");
        return state;
    }

    state.file_system = partition->volume->file_system.trimmed();
    const auto capability = PartitionFileSystemRegistry::capabilityFor(state.file_system);
    if (!capability.non_native) {
        state.reason =
            QObject::tr("Selected file system uses Windows-native Partition Manager actions.");
        return state;
    }
    if (partition->volume->file_system_details.isEmpty()) {
        state.reason = QObject::tr("No read-only raw metadata was captured for this file system.");
        return state;
    }

    state.enabled = true;
    state.reason = QObject::tr("Inspect captured read-only %1 metadata").arg(state.file_system);
    return state;
}

NonNativeFilesystemBrowseState nonNativeFilesystemBrowseState(
    const std::optional<PartitionTarget>& target, const PartitionInfoEx* partition) {
    NonNativeFilesystemBrowseState state;
    if (!partition || !partition->volume || partition->volume->file_system.trimmed().isEmpty()) {
        state.reason = QObject::tr("Select a partition with a detected non-Windows file system.");
        return state;
    }

    state.file_system = partition->volume->file_system.trimmed();
    const auto capability = PartitionFileSystemRegistry::capabilityFor(state.file_system);
    if (!capability.non_native) {
        state.reason =
            QObject::tr("Selected file system uses Windows-native Partition Manager actions.");
        return state;
    }
    if (!isExtFilesystem(state.file_system) && !isHfsFilesystem(state.file_system) &&
        !isApfsFilesystem(state.file_system)) {
        state.reason = QObject::tr(
            "Read-only browsing is currently available for ext2/ext3/ext4, HFS+/HFSX, and APFS "
            "only.");
        return state;
    }

    state.target_path = nonNativeFilesystemTargetPath(target, partition);
    if (state.target_path.isEmpty()) {
        state.reason = QObject::tr("Selected partition does not expose a usable raw target path.");
        return state;
    }

    state.enabled = true;
    state.reason = QObject::tr("Browse read-only %1 directory entries").arg(state.file_system);
    return state;
}

void updateNonNativeFilesystemButton(QAbstractButton* button,
                                     const std::optional<PartitionTarget>& target,
                                     const PartitionInfoEx* partition) {
    const auto state = nonNativeFilesystemCheckState(target, partition);
    button->setEnabled(state.enabled);
    button->setToolTip(state.reason);
    button->setStatusTip(state.reason);
    button->setAccessibleDescription(state.reason);
}

void updateBrowseNonNativeFilesystemButton(QAbstractButton* button,
                                           bool operationRunning,
                                           const std::optional<PartitionTarget>& target,
                                           const PartitionInfoEx* partition) {
    const auto state = nonNativeFilesystemBrowseState(target, partition);
    const bool enabled = state.enabled && !operationRunning;
    button->setEnabled(enabled);
    const QString reason = operationRunning ? QObject::tr("Partition operation is already running.")
                                            : state.reason;
    button->setToolTip(reason);
    button->setStatusTip(reason);
    button->setAccessibleDescription(reason);
}

void updateInspectNonNativeFilesystemButton(QAbstractButton* button,
                                            bool operationRunning,
                                            const PartitionInfoEx* partition) {
    const auto state = nonNativeFilesystemInspectState(partition);
    button->setEnabled(state.enabled && !operationRunning);
    button->setToolTip(state.reason);
    button->setStatusTip(state.reason);
    button->setAccessibleDescription(state.reason);
}

void updateApfsRootFileMutationButton(QAbstractButton* button,
                                      bool operationRunning,
                                      const std::optional<PartitionTarget>& target,
                                      const PartitionInfoEx* partition) {
    const auto state = apfsRootFileMutationState(target, partition);
    const bool enabled = state.enabled && !operationRunning;
    button->setEnabled(enabled);
    const QString reason = operationRunning ? QObject::tr("Partition operation is already running.")
                                            : state.reason;
    button->setToolTip(reason);
    button->setStatusTip(reason);
    button->setAccessibleDescription(reason);
}

void updateHfsFileMutationButton(QAbstractButton* button,
                                 bool operationRunning,
                                 const std::optional<PartitionTarget>& target,
                                 const PartitionInfoEx* partition) {
    const auto state = hfsFileMutationState(target, partition);
    const bool enabled = state.enabled && !operationRunning;
    button->setEnabled(enabled);
    const QString reason = operationRunning ? QObject::tr("Partition operation is already running.")
                                            : state.reason;
    button->setToolTip(reason);
    button->setStatusTip(reason);
    button->setAccessibleDescription(reason);
}

bool updateSpecialTargetButtonState(QAbstractButton* button,
                                    bool operationRunning,
                                    const std::optional<PartitionTarget>& target,
                                    const PartitionInfoEx* partition) {
    if (isHfsFileMutationAction(button)) {
        updateHfsFileMutationButton(button, operationRunning, target, partition);
        return true;
    }
    if (isApfsRootFileMutationAction(button)) {
        updateApfsRootFileMutationButton(button, operationRunning, target, partition);
        return true;
    }
    if (isBrowseNonNativeFilesystemAction(button)) {
        updateBrowseNonNativeFilesystemButton(button, operationRunning, target, partition);
        return true;
    }
    if (isInspectNonNativeFilesystemAction(button)) {
        updateInspectNonNativeFilesystemButton(button, operationRunning, partition);
        return true;
    }
    if (isNonNativeFilesystemAction(button)) {
        updateNonNativeFilesystemButton(button, target, partition);
        return true;
    }
    return false;
}

void updateTargetButtonState(QAbstractButton* button,
                             bool operationRunning,
                             const std::optional<PartitionTarget>& target,
                             const PartitionInfoEx* partition) {
    if (updateSpecialTargetButtonState(button, operationRunning, target, partition)) {
        return;
    }
    const auto availability = buttonActionAvailability(button, target, partition);
    const bool enabled = !operationRunning && availability.enabled;
    button->setEnabled(enabled);
    const QString defaultTooltip = button->property(kActionDefaultTooltipProperty).toString();
    const bool targetMismatch = !target || !buttonAllowsTarget(button, target);
    const QString reason = operationRunning ? QObject::tr("Partition operation is already running.")
                                            : (targetMismatch || availability.reason.isEmpty()
                                                   ? defaultTooltip
                                                   : availability.reason);
    button->setToolTip(reason);
    button->setStatusTip(reason);
    button->setAccessibleDescription(reason);
}

QString inventorySummaryText(const PartitionInventory& inventory) {
    return QObject::tr("%1 disk(s), layout %2")
        .arg(inventory.disks.size())
        .arg(inventory.layout_hash.left(kPartitionLayoutHashPreviewChars));
}

bool targetMatchesPartition(const std::optional<PartitionTarget>& target,
                            const PartitionDiskInfo& disk,
                            const PartitionInfoEx& partition) {
    return target && target->kind == PartitionTargetKind::Partition &&
           target->disk_number == disk.disk_number &&
           target->partition_number == partition.partition_number;
}

bool targetMatchesRegion(const std::optional<PartitionTarget>& target,
                         const UnallocatedRegion& region) {
    return target && target->kind == PartitionTargetKind::Unallocated &&
           target->disk_number == region.disk_number &&
           target->offset_bytes == region.offset_bytes && target->size_bytes == region.size_bytes;
}

QString tableKindForTarget(const PartitionTarget& target) {
    if (target.kind == PartitionTargetKind::Disk) {
        return QStringLiteral("disk");
    }
    if (target.kind == PartitionTargetKind::Unallocated) {
        return QStringLiteral("unallocated");
    }
    return QStringLiteral("partition");
}

bool rowTargetBaseMatches(const QVariantMap& row_data, const PartitionTarget& target) {
    return row_data.value(QStringLiteral("kind")).toString() == tableKindForTarget(target) &&
           row_data.value(QStringLiteral("disk")).toUInt() == target.disk_number;
}

bool rowTargetDetailsMatch(const QVariantMap& row_data, const PartitionTarget& target) {
    if (target.kind == PartitionTargetKind::Disk) {
        return true;
    }
    if (target.kind == PartitionTargetKind::Unallocated) {
        return row_data.value(QStringLiteral("offset")).toString().toULongLong() ==
                   target.offset_bytes &&
               row_data.value(QStringLiteral("size")).toString().toULongLong() == target.size_bytes;
    }
    return row_data.value(QStringLiteral("partition")).toUInt() == target.partition_number;
}

bool rowMatchesTarget(const QVariantMap& row_data, const PartitionTarget& target) {
    return rowTargetBaseMatches(row_data, target) && rowTargetDetailsMatch(row_data, target);
}
constexpr int kQuickPartitionDefaultCount = 2;
constexpr int kQuickPartitionMaxCount = 9;
constexpr int kQuickPartitionMbrDataMaxCount = 4;
constexpr int kQuickPartitionMinimumSizeMb = 64;
constexpr int kQuickPartitionReservedTailMb = 32;
constexpr int kQuickPartitionLabelColumn = 0;
constexpr int kQuickPartitionSizeColumn = 1;
constexpr int kQuickPartitionColumnCount = 2;
constexpr int kQuickPartitionTableMinHeight = 148;
constexpr int kQuickPartitionPresetNameMaxChars = 64;
constexpr int kWizardDiskSizeRole = Qt::UserRole + 1;
constexpr int kWizardRegionDiskRole = Qt::UserRole + 1;
constexpr int kWizardRegionOffsetRole = Qt::UserRole + 2;
constexpr int kWizardRegionSizeRole = Qt::UserRole + 3;
constexpr uint64_t kDiskMapMinimumBytes = 1;
constexpr uint64_t kAllocationUnitDefaultBytes = 0;
constexpr uint64_t kAllocationUnit512Bytes = 512;
constexpr uint64_t kAllocationUnit1KbBytes = 1024;
constexpr uint64_t kAllocationUnit2KbBytes = 2 * 1024;
constexpr uint64_t kAllocationUnit4KbBytes = 4 * 1024;
constexpr uint64_t kAllocationUnit8KbBytes = 8 * 1024;
constexpr uint64_t kAllocationUnit16KbBytes = 16 * 1024;
constexpr uint64_t kAllocationUnit32KbBytes = 32 * 1024;
constexpr uint64_t kAllocationUnit64KbBytes = 64 * 1024;

const QString kIconApply = QStringLiteral(":/icons/icons/icons8-pm-apply.svg");
const QString kIconUndo = QStringLiteral(":/icons/icons/icons8-pm-undo.svg");
const QString kIconRedo = QStringLiteral(":/icons/icons/icons8-pm-redo.svg");
const QString kIconDiscard = QStringLiteral(":/icons/icons/icons8-pm-discard.svg");
const QString kIconCopy = QStringLiteral(":/icons/icons/icons8-pm-copy.svg");
const QString kIconProperties = QStringLiteral(":/icons/icons/icons8-pm-properties.svg");
const QString kIconDisk = QStringLiteral(":/icons/icons/icons8-pm-disk.svg");
const QString kIconRecovery = QStringLiteral(":/icons/icons/icons8-pm-data-recovery.svg");
const QString kIconAlign = QStringLiteral(":/icons/icons/icons8-pm-align.svg");
const QString kIconDelete = QStringLiteral(":/icons/icons/icons8-pm-delete.svg");
const QString kIconSurface = QStringLiteral(":/icons/icons/icons8-pm-surface-test.svg");
const QString kIconDryRun = QStringLiteral(":/icons/icons/icons8-pm-dry-run.svg");
const QString kIconWipe = QStringLiteral(":/icons/icons/icons8-pm-wipe.svg");
const QString kIconConvert = QStringLiteral(":/icons/icons/icons8-pm-convert.svg");
const QString kIconRefresh = QStringLiteral(":/icons/icons/icons8-pm-refresh.svg");
const QString kIconCreate = QStringLiteral(":/icons/icons/icons8-pm-create.svg");
const QString kIconResize = QStringLiteral(":/icons/icons/icons8-pm-resize.svg");
const QString kIconSplit = QStringLiteral(":/icons/icons/icons8-pm-split.svg");
const QString kIconOsDrive = QStringLiteral(":/icons/icons/icons8-pm-os-drive.svg");
const QString kQuickPartitionPresetsConfigKey =
    QStringLiteral("partition_manager/quick_partition_presets");

QIcon icons8RibbonIcon(const QString& icon_path) {
    QIcon icon;
    const QSize iconSize(kRibbonIconSize, kRibbonIconSize);
    icon.addFile(icon_path, iconSize, QIcon::Normal);
    icon.addFile(icon_path, iconSize, QIcon::Disabled);
    icon.addFile(icon_path, iconSize, QIcon::Active);
    icon.addFile(icon_path, iconSize, QIcon::Selected);
    return icon;
}

QJsonObject withValue(const QString& key, const QJsonValue& value) {
    QJsonObject object;
    object.insert(key, value);
    return object;
}

struct SpaceAnalyzerEntry {
    QString name;
    QString type;
    QString path;
    uint64_t bytes{0};
};

struct SpaceAnalyzerResult {
    QVector<SpaceAnalyzerEntry> entries;
    QVector<SpaceAnalyzerEntry> largest_files;
    QVector<SpaceAnalyzerEntry> file_types;
    uint64_t total_bytes{0};
    int scanned_entries{0};
    bool cancelled{false};
    bool root_missing{false};
};

struct SpaceAnalyzerScanState {
    QVector<SpaceAnalyzerEntry> largest_files;
    QHash<QString, uint64_t> file_type_bytes;
    QHash<QString, int> file_type_counts;
    int scanned_entries{0};
};

template <typename Callback>
QAction* addContextMenuAction(QMenu& menu,
                              QObject* receiver,
                              const ContextActionSpec& spec,
                              Callback callback) {
    auto* action = menu.addAction(QIcon(spec.icon_path), spec.text);
    action->setEnabled(spec.enabled);
    if (!spec.enabled && !spec.disabled_reason.isEmpty()) {
        action->setToolTip(spec.disabled_reason);
        action->setStatusTip(spec.disabled_reason);
    }
    QObject::connect(action, &QAction::triggered, receiver, callback);
    return action;
}

uint64_t jsonUInt64(const QJsonObject& payload, const QString& key) {
    const auto value = payload.value(key);
    if (value.isDouble()) {
        return static_cast<uint64_t>(std::max(0.0, value.toDouble()));
    }
    bool ok = false;
    const auto parsed = value.toString().toULongLong(&ok);
    return ok ? parsed : 0;
}

uint64_t saturatingAdd(uint64_t left, uint64_t right) {
    if (std::numeric_limits<uint64_t>::max() - left < right) {
        return std::numeric_limits<uint64_t>::max();
    }
    return left + right;
}

bool cancelRequested(const std::shared_ptr<std::atomic_bool>& cancelFlag) {
    return cancelFlag && cancelFlag->load(std::memory_order_relaxed);
}

uint64_t fileInfoSize(const QFileInfo& info) {
    return info.isFile() ? static_cast<uint64_t>(std::max<qint64>(0, info.size())) : 0;
}

QString fileExtensionLabel(const QFileInfo& info) {
    const QString suffix = info.suffix().trimmed();
    if (suffix.isEmpty()) {
        return QObject::tr("(no extension)");
    }
    return QStringLiteral(".%1").arg(suffix.toLower());
}

void recordLargestFile(QVector<SpaceAnalyzerEntry>* largestFiles,
                       const QFileInfo& info,
                       uint64_t bytes) {
    if (!largestFiles) {
        return;
    }
    SpaceAnalyzerEntry row;
    row.name = info.fileName();
    row.type = QObject::tr("File");
    row.path = info.absoluteFilePath();
    row.bytes = bytes;
    if (largestFiles->size() < kSpaceAnalyzerMaxFileRows) {
        largestFiles->append(row);
        return;
    }
    auto smallest =
        std::min_element(largestFiles->begin(),
                         largestFiles->end(),
                         [](const SpaceAnalyzerEntry& left, const SpaceAnalyzerEntry& right) {
                             return left.bytes < right.bytes;
                         });
    if (smallest != largestFiles->end() && bytes > smallest->bytes) {
        *smallest = row;
    }
}

void recordFileAnalysis(const QFileInfo& info, SpaceAnalyzerScanState* state) {
    if (!info.isFile() || info.isSymLink()) {
        return;
    }
    const uint64_t bytes = fileInfoSize(info);
    recordLargestFile(&state->largest_files, info, bytes);
    const QString extension = fileExtensionLabel(info);
    state->file_type_bytes.insert(extension,
                                  saturatingAdd(state->file_type_bytes.value(extension), bytes));
    state->file_type_counts.insert(extension, state->file_type_counts.value(extension) + 1);
    ++state->scanned_entries;
}

uint64_t scanDirectoryBytes(const QString& directoryPath,
                            const std::shared_ptr<std::atomic_bool>& cancelFlag,
                            SpaceAnalyzerScanState* state) {
    uint64_t bytes = 0;
    QDirIterator iterator(directoryPath,
                          QDir::Files | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot,
                          QDirIterator::Subdirectories);
    while (iterator.hasNext() && !cancelRequested(cancelFlag)) {
        iterator.next();
        const QFileInfo info = iterator.fileInfo();
        if (info.isSymLink()) {
            continue;
        }
        bytes = saturatingAdd(bytes, fileInfoSize(info));
        recordFileAnalysis(info, state);
    }
    return bytes;
}

void sortSpaceAnalyzerRows(QVector<SpaceAnalyzerEntry>* rows) {
    if (!rows) {
        return;
    }
    std::sort(rows->begin(),
              rows->end(),
              [](const SpaceAnalyzerEntry& left, const SpaceAnalyzerEntry& right) {
                  if (left.bytes == right.bytes) {
                      return left.name.localeAwareCompare(right.name) < 0;
                  }
                  return left.bytes > right.bytes;
              });
}

SpaceAnalyzerResult analyzeVolumeSpace(const QString& rootPath,
                                       std::shared_ptr<std::atomic_bool> cancelFlag) {
    SpaceAnalyzerResult result;
    const QDir root(rootPath);
    if (!root.exists()) {
        result.root_missing = true;
        return result;
    }

    const auto entries = root.entryInfoList(QDir::Dirs | QDir::Files | QDir::Hidden | QDir::System |
                                                QDir::NoDotAndDotDot,
                                            QDir::Name | QDir::DirsFirst);
    result.entries.reserve(entries.size());
    SpaceAnalyzerScanState scanState;
    for (const auto& entry : entries) {
        if (cancelRequested(cancelFlag)) {
            result.cancelled = true;
            break;
        }
        SpaceAnalyzerEntry row;
        row.name = entry.fileName();
        row.path = entry.absoluteFilePath();
        if (entry.isDir() && !entry.isSymLink()) {
            row.type = QObject::tr("Folder");
            row.bytes = scanDirectoryBytes(row.path, cancelFlag, &scanState);
        } else {
            row.type = entry.isSymLink() ? QObject::tr("Link") : QObject::tr("File");
            row.bytes = fileInfoSize(entry);
            recordFileAnalysis(entry, &scanState);
        }
        result.total_bytes = saturatingAdd(result.total_bytes, row.bytes);
        result.entries.append(row);
    }

    result.largest_files = scanState.largest_files;
    result.scanned_entries = scanState.scanned_entries;
    result.file_types.reserve(scanState.file_type_bytes.size());
    for (auto it = scanState.file_type_bytes.cbegin(); it != scanState.file_type_bytes.cend();
         ++it) {
        SpaceAnalyzerEntry row;
        row.name = it.key();
        row.type = QObject::tr("File type");
        row.path = QObject::tr("%1 file(s)").arg(scanState.file_type_counts.value(it.key()));
        row.bytes = it.value();
        result.file_types.append(row);
    }

    sortSpaceAnalyzerRows(&result.entries);
    sortSpaceAnalyzerRows(&result.largest_files);
    sortSpaceAnalyzerRows(&result.file_types);
    return result;
}

QStringList spaceAnalyzerViewNames() {
    return {QObject::tr("Tree View"), QObject::tr("Largest Files"), QObject::tr("File Types")};
}

QStringList spaceAnalyzerContextActionNames() {
    return {QObject::tr("Open"), QObject::tr("Explore in File Explorer"), QObject::tr("Copy Path")};
}

QString percentageText(uint64_t value, uint64_t total) {
    if (total == 0) {
        return QStringLiteral("0.0%");
    }
    const double percent = (static_cast<double>(value) * 100.0) / static_cast<double>(total);
    return QStringLiteral("%1%").arg(percent, 0, 'f', 1);
}

int resizeInputMb(uint64_t bytes, bool roundUp) {
    uint64_t megabytes = bytes / kMegabyteBytes;
    if (roundUp && bytes % kMegabyteBytes != 0) {
        ++megabytes;
    }
    return static_cast<int>(
        std::clamp<uint64_t>(megabytes, 1, static_cast<uint64_t>(kMaxSizeInputMb)));
}

uint64_t resizeTargetBytesFromInput(int inputMb, int noChangeInputMb, uint64_t currentBytes) {
    if (inputMb == noChangeInputMb) {
        return currentBytes;
    }
    return static_cast<uint64_t>(inputMb) * kMegabyteBytes;
}

uint64_t partitionUsedBytesForResize(const PartitionInfoEx& partition) {
    if (!partition.volume || partition.volume->total_bytes < partition.volume->free_bytes) {
        return 0;
    }
    return partition.volume->total_bytes - partition.volume->free_bytes;
}

uint64_t adjacentFreeBytesAfter(const PartitionDiskInfo* disk, const PartitionInfoEx* partition) {
    if (!disk || !partition) {
        return 0;
    }
    const uint64_t partitionEnd = saturatingAdd(partition->offset_bytes, partition->size_bytes);
    const auto it = std::find_if(disk->unallocated_regions.cbegin(),
                                 disk->unallocated_regions.cend(),
                                 [partitionEnd](const auto& region) {
                                     return region.offset_bytes == partitionEnd;
                                 });
    return it == disk->unallocated_regions.cend() ? 0 : it->size_bytes;
}

const PartitionInfoEx* partitionBeforeRegion(const PartitionDiskInfo* disk,
                                             const PartitionTarget& region) {
    if (!disk || region.kind != PartitionTargetKind::Unallocated) {
        return nullptr;
    }
    const auto it = std::find_if(
        disk->partitions.cbegin(), disk->partitions.cend(), [&region](const auto& candidate) {
            return saturatingAdd(candidate.offset_bytes, candidate.size_bytes) ==
                   region.offset_bytes;
        });
    return it == disk->partitions.cend() ? nullptr : &(*it);
}

const PartitionInfoEx* partitionAfterRegion(const PartitionDiskInfo* disk,
                                            const PartitionTarget& region) {
    if (!disk || region.kind != PartitionTargetKind::Unallocated) {
        return nullptr;
    }
    const uint64_t regionEnd = saturatingAdd(region.offset_bytes, region.size_bytes);
    const auto it = std::find_if(disk->partitions.cbegin(),
                                 disk->partitions.cend(),
                                 [regionEnd](const auto& candidate) {
                                     return candidate.offset_bytes == regionEnd;
                                 });
    return it == disk->partitions.cend() ? nullptr : &(*it);
}

const PartitionInfoEx* adjacentDonorPartitionAfter(const PartitionDiskInfo* disk,
                                                   const PartitionInfoEx* partition) {
    if (!disk || !partition) {
        return nullptr;
    }
    const uint64_t partitionEnd = saturatingAdd(partition->offset_bytes, partition->size_bytes);
    const auto it = std::find_if(disk->partitions.cbegin(),
                                 disk->partitions.cend(),
                                 [partitionEnd](const auto& candidate) {
                                     return candidate.offset_bytes == partitionEnd;
                                 });
    return it == disk->partitions.cend() ? nullptr : &(*it);
}

uint64_t allocatableFreeBytesFromDonor(const PartitionInfoEx& donor) {
    if (!donor.volume || donor.volume->total_bytes < donor.volume->free_bytes) {
        return 0;
    }
    constexpr uint64_t kReserveBytes = static_cast<uint64_t>(kQuickPartitionMinimumSizeMb) *
                                       kMegabyteBytes;
    return donor.volume->free_bytes > kReserveBytes ? donor.volume->free_bytes - kReserveBytes : 0;
}

void clearLayout(QLayout* layout) {
    while (auto* item = layout->takeAt(0)) {
        if (auto* widget = item->widget()) {
            widget->hide();
            widget->deleteLater();
        }
        delete item;
    }
}

int stretchForBytes(uint64_t bytes, uint64_t disk_bytes) {
    const auto safe_total = std::max(disk_bytes, kDiskMapMinimumBytes);
    const auto scaled = static_cast<int>((bytes * kDiskMapStretchScale) / safe_total);
    return std::max(kDiskMapMinSegmentStretch, scaled);
}

void applyWindowFill(QWidget* widget, const QColor& color) {
    auto palette = widget->palette();
    palette.setColor(QPalette::Window, color);
    widget->setAutoFillBackground(true);
    widget->setPalette(palette);
}

class PartitionTableDelegate : public QStyledItemDelegate {
public:
    explicit PartitionTableDelegate(QObject* parent = nullptr) : QStyledItemDelegate(parent) {
        setObjectName(QStringLiteral("partitionDiskSeparatorDelegate"));
    }

    void paint(QPainter* painter,
               const QStyleOptionViewItem& option,
               const QModelIndex& index) const override {
        QStyledItemDelegate::paint(painter, option, index);
        const auto rowData = index.siblingAtColumn(ColPartition).data(Qt::UserRole).toMap();
        if (rowData.value(QStringLiteral("kind")).toString() != QStringLiteral("disk")) {
            return;
        }

        painter->save();
        painter->setPen(QPen(option.palette.color(QPalette::Mid), kTableDiskSeparatorWidth));
        painter->drawLine(option.rect.bottomLeft(), option.rect.bottomRight());
        painter->restore();
    }
};

class DiskMapRowFrame : public QFrame {
public:
    explicit DiskMapRowFrame(QWidget* parent = nullptr) : QFrame(parent) {
        setObjectName(QStringLiteral("partitionDiskMapRow"));
        setProperty("cornerRadius", kDiskMapRowRadius);
        setProperty("rowSpacing", kDiskMapRowSpacing);
        setFrameShape(QFrame::NoFrame);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    }

protected:
    void paintEvent(QPaintEvent* /*event*/) override {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);

        const bool selected = property("selected").toBool();
        const QColor selectedColor(property("selectedColorValue").toString().isEmpty()
                                       ? palette().color(QPalette::Highlight)
                                       : property("selectedColorValue").toString());
        const QRectF panel =
            rect().adjusted(0, 0, -kTableDiskSeparatorWidth, -kTableDiskSeparatorWidth);
        QPainterPath panelPath;
        panelPath.addRoundedRect(panel, kDiskMapRowRadius, kDiskMapRowRadius);
        painter.fillPath(panelPath,
                         selected ? selectedColor.lighter(kPartitionSegmentFillLightness)
                                  : palette().color(QPalette::Base));
        painter.setPen(
            QPen(selected ? selectedColor : palette().color(QPalette::Mid),
                 selected ? kPartitionSegmentSelectedBorderWidth : kTableDiskSeparatorWidth));
        painter.drawPath(panelPath);
    }
};

class PartitionLegendSwatch : public QFrame {
public:
    PartitionLegendSwatch(const QString& role, const QColor& color, QWidget* parent)
        : QFrame(parent), m_color(color) {
        setObjectName(QStringLiteral("partitionLegendSwatch"));
        setProperty("colorRole", role);
        setProperty("colorValue", color.name());
        setAccessibleName(QObject::tr("%1 legend color").arg(role));
        setFrameShape(QFrame::NoFrame);
        setFixedSize(ui::kUiIconCompact, ui::kUiIconCompact);
    }

protected:
    void paintEvent(QPaintEvent* /*event*/) override {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        const QRectF swatchRect = rect().adjusted(1, 1, -1, -1);
        QPainterPath path;
        path.addRoundedRect(swatchRect, kPartitionLegendSwatchRadius, kPartitionLegendSwatchRadius);
        painter.fillPath(path, m_color);
        painter.setPen(QPen(m_color.darker(kPartitionSegmentBorderDarkness), 1));
        painter.drawPath(path);
    }

private:
    QColor m_color;
};

struct PartitionSegmentSpec {
    QString text;
    QString tooltip;
    QString color_role;
    QColor color;
    int used_percent{0};
    bool selected{false};
    std::function<void()> on_activated;
};

class PartitionSegmentWidget : public QWidget {
public:
    explicit PartitionSegmentWidget(PartitionSegmentSpec spec, QWidget* parent = nullptr)
        : QWidget(parent)
        , m_text(std::move(spec.text))
        , m_color_role(std::move(spec.color_role))
        , m_color(std::move(spec.color))
        , m_used_percent(std::clamp(spec.used_percent, 0, kPartitionProgressMax))
        , m_selected(spec.selected) {
        setObjectName(QStringLiteral("partitionDiskMapSegment"));
        setProperty("cornerRadius", kPartitionSegmentRadius);
        setProperty("colorRole", m_color_role);
        setProperty("selected", m_selected);
        setProperty("outerColorRole", QStringLiteral("Neutral"));
        setProperty("innerColorRole", m_color_role);
        setProperty("innerColorValue", m_color.name());
        setToolTip(spec.tooltip);
        setAccessibleName(spec.tooltip);
        setAccessibleDescription(m_selected
                                     ? tr("Selected %1 partition map segment").arg(m_color_role)
                                     : tr("%1 partition map segment").arg(m_color_role));
        setMinimumSize(kPartitionSegmentMinWidth, kPartitionSegmentMinHeight);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        m_on_activated = std::move(spec.on_activated);
        if (m_on_activated) {
            setCursor(Qt::PointingHandCursor);
            setFocusPolicy(Qt::StrongFocus);
        }
    }

    [[nodiscard]] QSize sizeHint() const override {
        return {kPartitionSegmentMinWidth, kPartitionSegmentMinHeight};
    }

protected:
    void paintEvent(QPaintEvent* /*event*/) override {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);

        const QRectF outer =
            rect().adjusted(0, 0, -kTableDiskSeparatorWidth, -kTableDiskSeparatorWidth);
        QPainterPath outerPath;
        outerPath.addRoundedRect(outer, kPartitionSegmentRadius, kPartitionSegmentRadius);
        QLinearGradient fill(outer.topLeft(), outer.bottomLeft());
        fill.setColorAt(0, palette().color(QPalette::Base));
        fill.setColorAt(1, palette().color(QPalette::AlternateBase));
        painter.fillPath(outerPath, fill);
        painter.setPen(
            QPen(m_selected ? palette().color(QPalette::Highlight) : palette().color(QPalette::Mid),
                 m_selected ? kPartitionSegmentSelectedBorderWidth : kTableDiskSeparatorWidth));
        painter.drawPath(outerPath);

        const QRectF track(outer.left() + ui::kMarginSmall,
                           outer.top() + ui::kMarginSmall,
                           outer.width() - (ui::kMarginSmall * 2),
                           kPartitionSegmentTrackHeight);
        QPainterPath trackPath;
        trackPath.addRoundedRect(track, kPartitionSegmentRadius, kPartitionSegmentRadius);
        painter.fillPath(trackPath, m_color.lighter(kPartitionSegmentFillLightness));
        painter.setPen(
            QPen(m_color.darker(kPartitionSegmentBorderDarkness), kTableDiskSeparatorWidth));
        painter.drawPath(trackPath);

        if (m_used_percent > 0) {
            QRectF used = track;
            used.setWidth(track.width() * m_used_percent / kPartitionProgressMax);
            QPainterPath usedPath;
            usedPath.addRoundedRect(used, kPartitionSegmentRadius, kPartitionSegmentRadius);
            QLinearGradient usedFill(used.topLeft(), used.bottomLeft());
            usedFill.setColorAt(0, m_color.lighter());
            usedFill.setColorAt(1, m_color.darker(kPartitionSegmentTrackLightness));
            painter.fillPath(usedPath, usedFill);
        }

        const QRect textRect = outer
                                   .adjusted(ui::kMarginSmall,
                                             ui::kMarginSmall + kPartitionSegmentTrackHeight +
                                                 kPartitionSegmentLabelTopPad,
                                             -ui::kMarginSmall,
                                             -ui::kMarginTight)
                                   .toRect();
        const QString text =
            painter.fontMetrics().elidedText(m_text, Qt::ElideRight, textRect.width());
        painter.setPen(palette().color(QPalette::Text));
        painter.drawText(textRect, Qt::AlignCenter, text);
    }

    void mouseReleaseEvent(QMouseEvent* event) override {
        if (event->button() == Qt::LeftButton && rect().contains(event->pos())) {
            activate();
            event->accept();
            return;
        }
        QWidget::mouseReleaseEvent(event);
    }

    void keyPressEvent(QKeyEvent* event) override {
        if (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter ||
            event->key() == Qt::Key_Space) {
            activate();
            event->accept();
            return;
        }
        QWidget::keyPressEvent(event);
    }

private:
    void activate() {
        if (m_on_activated) {
            m_on_activated();
        }
    }

    QString m_text;
    QString m_color_role;
    QColor m_color;
    int m_used_percent;
    bool m_selected;
    std::function<void()> m_on_activated;
};

struct DiskTileSpec {
    QString name;
    QString detail;
    QIcon icon;
    QString accessible;
    QColor color;
    bool selected{false};
    std::function<void()> on_activated;
};

class DiskTileWidget : public QWidget {
public:
    explicit DiskTileWidget(DiskTileSpec spec, QWidget* parent)
        : QWidget(parent)
        , m_name(std::move(spec.name))
        , m_detail(std::move(spec.detail))
        , m_icon(std::move(spec.icon))
        , m_color(std::move(spec.color))
        , m_selected(spec.selected)
        , m_on_activated(std::move(spec.on_activated)) {
        setObjectName(QStringLiteral("partitionDiskMapDiskTile"));
        setProperty("cornerRadius", kDiskMapRowRadius);
        setProperty("selected", m_selected);
        setProperty("selectedColorValue", m_color.name());
        setAccessibleName(std::move(spec.accessible));
        setFixedWidth(kDiskTileWidth);
        setMinimumHeight(kDiskTileMinHeight);
        setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Expanding);
        if (m_on_activated) {
            setCursor(Qt::PointingHandCursor);
            setFocusPolicy(Qt::StrongFocus);
        }
    }

protected:
    void paintEvent(QPaintEvent* /*event*/) override {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        const QRectF panel =
            rect().adjusted(0, 0, -kTableDiskSeparatorWidth, -kTableDiskSeparatorWidth);
        QPainterPath panelPath;
        panelPath.addRoundedRect(panel, kDiskMapRowRadius, kDiskMapRowRadius);
        QLinearGradient fill(panel.topLeft(), panel.bottomLeft());
        fill.setColorAt(0, palette().color(QPalette::Base));
        fill.setColorAt(1, palette().color(QPalette::AlternateBase));
        if (m_selected) {
            fill.setColorAt(0, m_color.lighter(kPartitionSegmentFillLightness));
            fill.setColorAt(1, palette().color(QPalette::AlternateBase));
        }
        painter.fillPath(panelPath, fill);
        painter.setPen(
            QPen(m_selected ? m_color : palette().color(QPalette::Mid),
                 m_selected ? kPartitionSegmentSelectedBorderWidth : kTableDiskSeparatorWidth));
        painter.drawPath(panelPath);

        const QRect iconRect((width() - kDiskIconSize) / kCenteringDivisor,
                             ui::kMarginSmall,
                             kDiskIconSize,
                             kDiskIconSize);
        m_icon.paint(&painter, iconRect, Qt::AlignCenter, QIcon::Normal, QIcon::On);

        auto font = painter.font();
        font.setBold(true);
        painter.setFont(font);
        painter.setPen(palette().color(QPalette::Text));
        const QRect nameRect(0,
                             ui::kMarginSmall + kDiskIconSize + ui::kSpacingTight,
                             width(),
                             painter.fontMetrics().height());
        painter.drawText(nameRect, Qt::AlignCenter, m_name);

        font.setBold(false);
        painter.setFont(font);
        const QRect detailRect(0,
                               nameRect.bottom() + ui::kSpacingTight,
                               width(),
                               height() - nameRect.bottom() - ui::kSpacingTight);
        painter.drawText(detailRect, Qt::AlignHCenter | Qt::AlignTop, m_detail);
    }

    void mouseReleaseEvent(QMouseEvent* event) override {
        if (event->button() == Qt::LeftButton && rect().contains(event->pos())) {
            activate();
            event->accept();
            return;
        }
        QWidget::mouseReleaseEvent(event);
    }

    void keyPressEvent(QKeyEvent* event) override {
        if (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter ||
            event->key() == Qt::Key_Space) {
            activate();
            event->accept();
            return;
        }
        QWidget::keyPressEvent(event);
    }

private:
    void activate() {
        if (m_on_activated) {
            m_on_activated();
        }
    }

    QString m_name;
    QString m_detail;
    QIcon m_icon;
    QColor m_color;
    bool m_selected;
    std::function<void()> m_on_activated;
};

struct ClonePreviewSegment {
    QString label;
    uint64_t bytes{0};
    QColor color;
    int used_percent{0};
};

class CloneLayoutPreviewWidget : public QWidget {
public:
    explicit CloneLayoutPreviewWidget(QWidget* parent = nullptr) : QWidget(parent) {
        setObjectName(QStringLiteral("diskCopyLayoutPreview"));
        setAccessibleName(QStringLiteral("Copy wizard source and target layout preview"));
        setMinimumHeight(kClonePreviewMinHeight);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    }

    void configure(const PartitionDiskInfo* sourceDisk,
                   uint64_t targetBytes,
                   const QString& layoutMode) {
        m_source_disk = sourceDisk;
        m_target_bytes = targetBytes;
        m_layout_mode = layoutMode;
        update();
    }

protected:
    void paintEvent(QPaintEvent* /*event*/) override {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        const QRect sourceRow(ui::kMarginSmall,
                              ui::kMarginSmall,
                              width() - (ui::kMarginSmall * 2),
                              kClonePreviewRowHeight);
        const QRect targetRow(sourceRow.left(),
                              sourceRow.bottom() + ui::kSpacingMedium,
                              sourceRow.width(),
                              kClonePreviewRowHeight);
        drawPreviewRow(&painter, sourceRow, tr("Source"), sourceSegments(), sourceTotalBytes());
        drawPreviewRow(&painter, targetRow, tr("Target"), targetSegments(), targetTotalBytes());
    }

private:
    [[nodiscard]] uint64_t sourceTotalBytes() const {
        return m_source_disk ? m_source_disk->size_bytes : 0;
    }

    [[nodiscard]] uint64_t targetTotalBytes() const {
        return m_target_bytes == 0 ? sourceTotalBytes() : m_target_bytes;
    }

    [[nodiscard]] QVector<ClonePreviewSegment> sourceSegments() const {
        QVector<ClonePreviewSegment> segments;
        if (!m_source_disk) {
            return segments;
        }
        for (const auto& partition : m_source_disk->partitions) {
            segments.append(partitionSegment(partition));
        }
        for (const auto& region : m_source_disk->unallocated_regions) {
            segments.append(
                {tr("Unallocated"), region.size_bytes, palette().color(QPalette::Midlight), 0});
        }
        return segments;
    }

    [[nodiscard]] QVector<ClonePreviewSegment> targetSegments() const {
        const uint64_t sourceBytes = sourceTotalBytes();
        const uint64_t targetBytes = targetTotalBytes();
        if (targetBytes != 0 && sourceBytes > targetBytes) {
            return {{tr("Target too small"),
                     targetBytes,
                     QColor(QString::fromLatin1(ui::kColorError)),
                     0}};
        }
        return m_layout_mode.startsWith(QStringLiteral("Fit")) ? fittedTargetSegments()
                                                               : keptTargetSegments();
    }

    [[nodiscard]] QVector<ClonePreviewSegment> fittedTargetSegments() const {
        auto segments = sourceSegments();
        const uint64_t sourceBytes = std::max(sourceTotalBytes(), kDiskMapMinimumBytes);
        const uint64_t targetBytes = targetTotalBytes();
        for (auto& segment : segments) {
            segment.bytes = (segment.bytes * targetBytes) / sourceBytes;
        }
        return segments;
    }

    [[nodiscard]] QVector<ClonePreviewSegment> keptTargetSegments() const {
        auto segments = sourceSegments();
        const uint64_t sourceBytes = sourceTotalBytes();
        const uint64_t targetBytes = targetTotalBytes();
        if (targetBytes > sourceBytes) {
            segments.append({tr("Unallocated"),
                             targetBytes - sourceBytes,
                             palette().color(QPalette::Midlight),
                             0});
        }
        return segments;
    }

    [[nodiscard]] ClonePreviewSegment partitionSegment(const PartitionInfoEx& partition) const {
        const QString label = partition.volume && !partition.volume->drive_letter.isEmpty()
                                  ? tr("%1:").arg(partition.volume->drive_letter)
                                  : tr("*:");
        const QColor color = partition.is_efi || partition.is_msr || partition.is_recovery
                                 ? QColor(QString::fromLatin1(ui::kColorPrimary))
                                 : QColor(QString::fromLatin1(ui::kColorSuccess));
        return {label, partition.size_bytes, color, partitionUsedPercent(partition)};
    }

    [[nodiscard]] static int partitionUsedPercent(const PartitionInfoEx& partition) {
        if (!partition.volume || partition.volume->total_bytes == 0 ||
            partition.volume->total_bytes < partition.volume->free_bytes) {
            return 0;
        }
        const auto used = partition.volume->total_bytes - partition.volume->free_bytes;
        return static_cast<int>(std::min<uint64_t>(
            kPartitionProgressMax, (used * kPartitionProgressMax) / partition.volume->total_bytes));
    }

    void drawPreviewRow(QPainter* painter,
                        const QRect& row,
                        const QString& title,
                        const QVector<ClonePreviewSegment>& segments,
                        uint64_t totalBytes) {
        painter->setPen(palette().color(QPalette::Text));
        painter->drawText(
            row.left(), row.top(), kClonePreviewLabelWidth, row.height(), Qt::AlignVCenter, title);
        const QRect bar(row.left() + kClonePreviewLabelWidth + kClonePreviewTitleGap,
                        row.top() + ((row.height() - kClonePreviewBarHeight) / 2),
                        row.width() - kClonePreviewLabelWidth - kClonePreviewTitleGap,
                        kClonePreviewBarHeight);
        drawSegmentTrack(painter, bar, segments, totalBytes);
    }

    void drawSegmentTrack(QPainter* painter,
                          const QRect& bar,
                          const QVector<ClonePreviewSegment>& segments,
                          uint64_t totalBytes) {
        QPainterPath trackPath;
        trackPath.addRoundedRect(bar, kPartitionSegmentRadius, kPartitionSegmentRadius);
        painter->fillPath(trackPath, palette().color(QPalette::AlternateBase));
        const uint64_t safeTotal = std::max(totalBytes, kDiskMapMinimumBytes);
        int x = bar.left();
        uint64_t consumed = 0;
        for (qsizetype i = 0; i < segments.size(); ++i) {
            const auto& segment = segments.at(i);
            consumed += segment.bytes;
            const int right = i == segments.size() - 1
                                  ? bar.right()
                                  : bar.left() +
                                        static_cast<int>((consumed * bar.width()) / safeTotal);
            const QRect segmentRect(
                x, bar.top(), std::max(kTableDiskSeparatorWidth, right - x), bar.height());
            drawSegment(painter, segmentRect, segment);
            x = right;
        }
        painter->setPen(QPen(palette().color(QPalette::Mid), kTableDiskSeparatorWidth));
        painter->drawPath(trackPath);
    }

    void drawSegment(QPainter* painter, const QRect& rect, const ClonePreviewSegment& segment) {
        if (rect.width() <= 0) {
            return;
        }
        painter->fillRect(rect, segment.color.lighter(kPartitionSegmentFillLightness));
        if (segment.used_percent > 0) {
            QRect used = rect;
            used.setWidth((rect.width() * segment.used_percent) / kPartitionProgressMax);
            painter->fillRect(used, segment.color.darker(kPartitionSegmentTrackLightness));
        }
        const QString text =
            painter->fontMetrics().elidedText(segment.label, Qt::ElideRight, rect.width());
        painter->setPen(palette().color(QPalette::Text));
        painter->drawText(rect.adjusted(ui::kSpacingTight, 0, -ui::kSpacingTight, 0),
                          Qt::AlignCenter,
                          text);
    }

    const PartitionDiskInfo* m_source_disk{nullptr};
    uint64_t m_target_bytes{0};
    QString m_layout_mode;
};

struct ApplyDiffSegment {
    QString label;
    uint64_t offset{0};
    uint64_t bytes{0};
    QColor color;
    int used_percent{0};
    int partition_number{0};
    bool unallocated{false};
    bool changed{false};
};

class ApplyLayoutDiffWidget : public QWidget {
public:
    explicit ApplyLayoutDiffWidget(QWidget* parent = nullptr) : QWidget(parent) {
        setObjectName(QStringLiteral("partitionApplyLayoutDiffPreview"));
        setAccessibleName(QStringLiteral("Queued partition layout before and after preview"));
        setMinimumHeight(kApplyDiffPreviewMinHeight);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    }

    void configure(PartitionInventory inventory, QVector<PartitionOperation> operations) {
        m_inventory = std::move(inventory);
        m_operations = std::move(operations);
        updateGeometry();
        update();
    }

    [[nodiscard]] QSize sizeHint() const override {
        return {kApplyReviewDialogMinWidth, kApplyDiffPreviewMinHeight};
    }

protected:
    void paintEvent(QPaintEvent* /*event*/) override {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.fillRect(rect(), palette().color(QPalette::Base));
        if (m_inventory.disks.isEmpty() || m_operations.isEmpty()) {
            drawPlaceholder(&painter);
            return;
        }

        int top = ui::kMarginSmall;
        for (const auto& disk : affectedDisks()) {
            const int diskHeight = (kApplyDiffPreviewRowHeight * kApplyDiffPreviewRowCount) +
                                   ui::kSpacingSmall;
            const QRect diskRect(ui::kMarginSmall,
                                 top,
                                 width() - (ui::kMarginSmall * kHorizontalMarginCount),
                                 diskHeight);
            drawDiskDiff(&painter, diskRect, disk);
            top += diskHeight + ui::kSpacingMedium;
            if (top > height()) {
                break;
            }
        }
    }

private:
    [[nodiscard]] QVector<PartitionDiskInfo> affectedDisks() const {
        QVector<PartitionDiskInfo> disks;
        for (const auto& disk : m_inventory.disks) {
            const bool affected =
                std::any_of(m_operations.cbegin(), m_operations.cend(), [&disk](const auto& op) {
                    return op.target.disk_number == disk.disk_number;
                });
            if (affected) {
                disks.append(disk);
            }
        }
        return disks;
    }

    void drawPlaceholder(QPainter* painter) {
        painter->setPen(palette().color(QPalette::PlaceholderText));
        painter->drawText(rect().adjusted(ui::kMarginSmall,
                                          ui::kMarginSmall,
                                          -ui::kMarginSmall,
                                          -ui::kMarginSmall),
                          Qt::AlignCenter,
                          tr("Queue operations to preview layout changes"));
    }

    void drawDiskDiff(QPainter* painter, const QRect& rect, const PartitionDiskInfo& disk) {
        painter->setPen(palette().color(QPalette::Text));
        painter->drawText(rect.left(),
                          rect.top(),
                          kApplyDiffPreviewTitleWidth,
                          kApplyDiffPreviewRowHeight,
                          Qt::AlignVCenter,
                          tr("Disk %1").arg(disk.disk_number));
        drawTrack(painter,
                  QRect(rect.left() + kApplyDiffPreviewTitleWidth,
                        rect.top(),
                        rect.width() - kApplyDiffPreviewTitleWidth,
                        kApplyDiffPreviewRowHeight),
                  currentSegments(disk),
                  disk.size_bytes,
                  tr("Before"));
        drawTrack(painter,
                  QRect(rect.left() + kApplyDiffPreviewTitleWidth,
                        rect.top() + kApplyDiffPreviewRowHeight + ui::kSpacingSmall,
                        rect.width() - kApplyDiffPreviewTitleWidth,
                        kApplyDiffPreviewRowHeight),
                  queuedSegments(disk),
                  disk.size_bytes,
                  tr("After"));
    }

    [[nodiscard]] QVector<ApplyDiffSegment> currentSegments(const PartitionDiskInfo& disk) const {
        QVector<ApplyDiffSegment> segments;
        for (const auto& partition : disk.partitions) {
            const QString label = partition.volume && !partition.volume->drive_letter.isEmpty()
                                      ? tr("%1:").arg(partition.volume->drive_letter)
                                      : segmentPartitionLabel(partition);
            segments.append({label,
                             partition.offset_bytes,
                             partition.size_bytes,
                             segmentPartitionColor(partition),
                             segmentUsedPercent(partition),
                             static_cast<int>(partition.partition_number),
                             false,
                             false});
        }
        for (const auto& region : disk.unallocated_regions) {
            segments.append({tr("Free"),
                             region.offset_bytes,
                             region.size_bytes,
                             segmentUnallocatedColor(),
                             0,
                             0,
                             true,
                             false});
        }
        sortSegments(&segments);
        return segments;
    }

    [[nodiscard]] static QString segmentPartitionLabel(const PartitionInfoEx& partition) {
        if (partition.is_efi) {
            return QStringLiteral("EFI");
        }
        if (partition.is_msr) {
            return QStringLiteral("MSR");
        }
        if (partition.is_recovery) {
            return QStringLiteral("Recovery");
        }
        if (partition.volume && !partition.volume->drive_letter.isEmpty()) {
            return QStringLiteral("%1:").arg(partition.volume->drive_letter);
        }
        return QStringLiteral("*:");
    }

    [[nodiscard]] static QColor segmentPartitionColor(const PartitionInfoEx& partition) {
        if (partition.is_efi || partition.is_msr || partition.is_recovery) {
            return QColor(QString::fromLatin1(ui::kColorPrimary));
        }
        return QColor(QString::fromLatin1(ui::kColorSuccess));
    }

    [[nodiscard]] static QColor segmentUnallocatedColor() {
        return QApplication::palette().color(QPalette::Midlight);
    }

    [[nodiscard]] static int segmentUsedPercent(const PartitionInfoEx& partition) {
        if (!partition.volume || partition.volume->total_bytes == 0 ||
            partition.volume->total_bytes < partition.volume->free_bytes) {
            return 0;
        }
        const auto used = partition.volume->total_bytes - partition.volume->free_bytes;
        return static_cast<int>(std::min<uint64_t>(
            kPartitionProgressMax, (used * kPartitionProgressMax) / partition.volume->total_bytes));
    }

    [[nodiscard]] QVector<ApplyDiffSegment> queuedSegments(const PartitionDiskInfo& disk) const {
        auto segments = currentSegments(disk);
        for (const auto& operation : m_operations) {
            if (operation.target.disk_number != disk.disk_number) {
                continue;
            }
            applyOperationPreview(operation, disk, &segments);
        }
        return segments;
    }

    void applyOperationPreview(const PartitionOperation& operation,
                               const PartitionDiskInfo& disk,
                               QVector<ApplyDiffSegment>* segments) const {
        if (operation.type == PartitionOperationType::Create) {
            previewCreate(operation, segments);
            return;
        }
        if (operation.type == PartitionOperationType::RestoreRecoveredPartition) {
            previewRecoveredPartition(operation, segments);
            return;
        }
        if (operation.type == PartitionOperationType::Delete) {
            markPartition(segments,
                          operation.target.partition_number,
                          tr("Delete"),
                          segmentUnallocatedColor(),
                          true);
            return;
        }
        if (operation.type == PartitionOperationType::Resize) {
            resizePartition(operation, segments);
            return;
        }
        if (operation.type == PartitionOperationType::MovePartition) {
            movePartition(operation, segments);
            return;
        }
        if (operation.type == PartitionOperationType::AllocateFreeSpace) {
            allocateFreeSpace(operation, segments);
            return;
        }
        if (operation.type == PartitionOperationType::Split) {
            splitPartition(operation, segments);
            return;
        }
        if (operation.type == PartitionOperationType::Merge) {
            mergePartition(operation, disk, segments);
            return;
        }
        if (isWholeDiskLayoutRewrite(operation.type)) {
            *segments = {wholeDiskChange(operation, disk)};
            return;
        }
        markPartition(segments,
                      operation.target.partition_number,
                      toDisplayString(operation.type),
                      QColor(QString::fromLatin1(ui::kColorWarning)),
                      false);
        sortSegments(segments);
    }

    [[nodiscard]] static bool isWholeDiskLayoutRewrite(PartitionOperationType type) {
        return type == PartitionOperationType::InitializeDisk ||
               type == PartitionOperationType::DeleteAllPartitions ||
               type == PartitionOperationType::CloneDisk ||
               type == PartitionOperationType::RestoreImage ||
               type == PartitionOperationType::MigrateOs ||
               type == PartitionOperationType::WipeDisk;
    }

    void previewCreate(const PartitionOperation& operation,
                       QVector<ApplyDiffSegment>* segments) const {
        const uint64_t offset = operation.target.offset_bytes;
        const uint64_t size = jsonUInt64(operation.payload, QStringLiteral("size_bytes"));
        if (size == 0) {
            return;
        }
        replaceFreeSpan(
            segments, offset, size, tr("New"), QColor(QString::fromLatin1(ui::kColorSuccess)));
    }

    void previewRecoveredPartition(const PartitionOperation& operation,
                                   QVector<ApplyDiffSegment>* segments) const {
        const uint64_t offset = jsonUInt64(operation.payload, QStringLiteral("offset_bytes"));
        const uint64_t size = jsonUInt64(operation.payload, QStringLiteral("size_bytes"));
        if (size == 0) {
            return;
        }
        replaceFreeSpan(segments,
                        offset,
                        size,
                        tr("Recovered"),
                        QColor(QString::fromLatin1(ui::kColorWarning)));
    }

    void resizePartition(const PartitionOperation& operation,
                         QVector<ApplyDiffSegment>* segments) const {
        const uint64_t targetSize = jsonUInt64(operation.payload,
                                               QStringLiteral("target_size_bytes"));
        if (targetSize == 0) {
            return;
        }
        for (qsizetype i = 0; i < segments->size(); ++i) {
            auto segment = segments->at(i);
            if (segment.unallocated ||
                segment.partition_number != static_cast<int>(operation.target.partition_number)) {
                continue;
            }
            const uint64_t previousSize = segment.bytes;
            segment.bytes = targetSize;
            segment.label = targetSize > previousSize ? tr("Extended") : tr("Shrunk");
            segment.changed = true;
            (*segments)[i] = segment;
            if (targetSize > previousSize) {
                consumeFreeSpan(segments, segment.offset + previousSize, targetSize - previousSize);
            } else if (targetSize < previousSize) {
                segments->append({tr("Free"),
                                  segment.offset + targetSize,
                                  previousSize - targetSize,
                                  segmentUnallocatedColor(),
                                  0,
                                  0,
                                  true,
                                  true});
            }
            sortSegments(segments);
            return;
        }
    }

    void movePartition(const PartitionOperation& operation,
                       QVector<ApplyDiffSegment>* segments) const {
        const uint64_t targetOffset = jsonUInt64(operation.payload,
                                                 QStringLiteral("target_offset_bytes"));
        const uint64_t targetSize = jsonUInt64(operation.payload,
                                               QStringLiteral("target_size_bytes"));
        if (targetSize == 0) {
            return;
        }

        uint64_t oldOffset = 0;
        uint64_t oldSize = 0;
        bool found = false;
        for (auto& segment : *segments) {
            if (segment.unallocated ||
                segment.partition_number != static_cast<int>(operation.target.partition_number)) {
                continue;
            }
            oldOffset = segment.offset;
            oldSize = segment.bytes;
            segment.offset = targetOffset;
            segment.bytes = targetSize;
            segment.label = tr("Moved");
            segment.changed = true;
            found = true;
            break;
        }
        if (!found) {
            return;
        }

        subtractAllocatedSpan(segments, targetOffset, targetSize);
        const uint64_t oldEnd = saturatingAdd(oldOffset, oldSize);
        const uint64_t targetEnd = saturatingAdd(targetOffset, targetSize);
        if (oldOffset < std::min(targetOffset, oldEnd)) {
            segments->append({tr("Free"),
                              oldOffset,
                              std::min(targetOffset, oldEnd) - oldOffset,
                              segmentUnallocatedColor(),
                              0,
                              0,
                              true,
                              true});
        }
        if (targetEnd < oldEnd) {
            segments->append({tr("Free"),
                              targetEnd,
                              oldEnd - targetEnd,
                              segmentUnallocatedColor(),
                              0,
                              0,
                              true,
                              true});
        }
        sortSegments(segments);
    }

    void allocateFreeSpace(const PartitionOperation& operation,
                           QVector<ApplyDiffSegment>* segments) const {
        const uint64_t amount = jsonUInt64(operation.payload, QStringLiteral("bytes_to_allocate"));
        const int donorNumber = static_cast<int>(
            jsonUInt64(operation.payload, QStringLiteral("source_partition_number")));
        if (amount == 0 || donorNumber == 0) {
            return;
        }
        for (auto& segment : *segments) {
            if (segment.unallocated) {
                continue;
            }
            if (segment.partition_number == static_cast<int>(operation.target.partition_number)) {
                segment.bytes = saturatingAdd(segment.bytes, amount);
                segment.label = tr("Allocated");
                segment.changed = true;
            } else if (segment.partition_number == donorNumber && segment.bytes > amount) {
                segment.offset = saturatingAdd(segment.offset, amount);
                segment.bytes -= amount;
                segment.label = tr("Donor restored");
                segment.changed = true;
            }
        }
        sortSegments(segments);
    }

    void splitPartition(const PartitionOperation& operation,
                        QVector<ApplyDiffSegment>* segments) const {
        const uint64_t firstSize = jsonUInt64(operation.payload,
                                              QStringLiteral("first_size_bytes"));
        for (qsizetype i = 0; i < segments->size(); ++i) {
            auto segment = segments->at(i);
            if (segment.unallocated ||
                segment.partition_number != static_cast<int>(operation.target.partition_number) ||
                firstSize == 0 || firstSize >= segment.bytes) {
                continue;
            }
            segment.bytes = firstSize;
            segment.label = tr("Split A");
            segment.changed = true;
            ApplyDiffSegment second{tr("Split B"),
                                    segment.offset + firstSize,
                                    operation.target.size_bytes - firstSize,
                                    QColor(QString::fromLatin1(ui::kColorSuccess)),
                                    0,
                                    0,
                                    false,
                                    true};
            (*segments)[i] = segment;
            segments->insert(i + 1, second);
            return;
        }
    }

    void subtractAllocatedSpan(QVector<ApplyDiffSegment>* segments,
                               uint64_t offset,
                               uint64_t size) const {
        const uint64_t end = saturatingAdd(offset, size);
        for (qsizetype i = segments->size() - 1; i >= 0; --i) {
            const auto segment = segments->at(i);
            if (!segment.unallocated) {
                continue;
            }
            const uint64_t segmentEnd = saturatingAdd(segment.offset, segment.bytes);
            if (offset >= segmentEnd || end <= segment.offset) {
                continue;
            }

            QVector<ApplyDiffSegment> replacement;
            if (segment.offset < offset) {
                replacement.append({tr("Free"),
                                    segment.offset,
                                    offset - segment.offset,
                                    segmentUnallocatedColor(),
                                    0,
                                    0,
                                    true,
                                    segment.changed});
            }
            if (end < segmentEnd) {
                replacement.append({tr("Free"),
                                    end,
                                    segmentEnd - end,
                                    segmentUnallocatedColor(),
                                    0,
                                    0,
                                    true,
                                    segment.changed});
            }
            segments->removeAt(i);
            for (qsizetype j = replacement.size() - 1; j >= 0; --j) {
                segments->insert(i, replacement.at(j));
            }
        }
    }

    void mergePartition(const PartitionOperation& operation,
                        const PartitionDiskInfo& disk,
                        QVector<ApplyDiffSegment>* segments) const {
        const auto sourceNumber = static_cast<int>(
            jsonUInt64(operation.payload, QStringLiteral("source_partition_number")));
        const auto* source = findPartition(disk, static_cast<uint32_t>(sourceNumber));
        for (auto& segment : *segments) {
            if (!segment.unallocated &&
                segment.partition_number == static_cast<int>(operation.target.partition_number) &&
                source != nullptr) {
                segment.bytes += source->size_bytes;
                segment.label = tr("Merged");
                segment.changed = true;
            } else if (!segment.unallocated && segment.partition_number == sourceNumber) {
                segment.label = tr("Merged out");
                segment.color = segmentUnallocatedColor();
                segment.unallocated = true;
                segment.changed = true;
            }
        }
    }

    void replaceFreeSpan(QVector<ApplyDiffSegment>* segments,
                         uint64_t offset,
                         uint64_t size,
                         const QString& label,
                         const QColor& color) const {
        for (qsizetype i = 0; i < segments->size(); ++i) {
            const auto segment = segments->at(i);
            const uint64_t segmentEnd = segment.offset + segment.bytes;
            if (!segment.unallocated || offset < segment.offset || offset + size > segmentEnd) {
                continue;
            }
            QVector<ApplyDiffSegment> replacement;
            if (offset > segment.offset) {
                replacement.append({tr("Free"),
                                    segment.offset,
                                    offset - segment.offset,
                                    segmentUnallocatedColor(),
                                    0,
                                    0,
                                    true,
                                    false});
            }
            replacement.append({label, offset, size, color, 0, 0, false, true});
            const uint64_t newEnd = offset + size;
            if (newEnd < segmentEnd) {
                replacement.append({tr("Free"),
                                    newEnd,
                                    segmentEnd - newEnd,
                                    segmentUnallocatedColor(),
                                    0,
                                    0,
                                    true,
                                    false});
            }
            segments->removeAt(i);
            for (qsizetype j = replacement.size() - 1; j >= 0; --j) {
                segments->insert(i, replacement.at(j));
            }
            return;
        }
    }

    void consumeFreeSpan(QVector<ApplyDiffSegment>* segments,
                         uint64_t offset,
                         uint64_t size) const {
        if (size == 0) {
            return;
        }
        for (qsizetype i = 0; i < segments->size(); ++i) {
            auto segment = segments->at(i);
            const uint64_t segmentEnd = segment.offset + segment.bytes;
            if (!segment.unallocated || offset < segment.offset || offset + size > segmentEnd) {
                continue;
            }
            if (offset == segment.offset && size == segment.bytes) {
                segments->removeAt(i);
                return;
            }
            if (offset == segment.offset) {
                segment.offset += size;
                segment.bytes -= size;
                (*segments)[i] = segment;
                return;
            }
            if (offset + size == segmentEnd) {
                segment.bytes -= size;
                (*segments)[i] = segment;
                return;
            }
            ApplyDiffSegment after{tr("Free"),
                                   offset + size,
                                   segmentEnd - (offset + size),
                                   segmentUnallocatedColor(),
                                   0,
                                   0,
                                   true,
                                   false};
            segment.bytes = offset - segment.offset;
            (*segments)[i] = segment;
            segments->insert(i + 1, after);
            return;
        }
    }

    void markPartition(QVector<ApplyDiffSegment>* segments,
                       uint32_t partitionNumber,
                       const QString& label,
                       const QColor& color,
                       bool unallocated) const {
        for (auto& segment : *segments) {
            if (!segment.unallocated &&
                segment.partition_number == static_cast<int>(partitionNumber)) {
                segment.label = label;
                segment.color = color;
                segment.unallocated = unallocated;
                segment.changed = true;
                return;
            }
        }
    }

    [[nodiscard]] ApplyDiffSegment wholeDiskChange(const PartitionOperation& operation,
                                                   const PartitionDiskInfo& disk) const {
        const bool leavesFreeSpace = operation.type == PartitionOperationType::InitializeDisk ||
                                     operation.type == PartitionOperationType::DeleteAllPartitions;
        return {toDisplayString(operation.type),
                0,
                disk.size_bytes,
                leavesFreeSpace ? segmentUnallocatedColor()
                                : QColor(QString::fromLatin1(ui::kColorWarning)),
                0,
                0,
                leavesFreeSpace,
                true};
    }

    [[nodiscard]] static const PartitionInfoEx* findPartition(const PartitionDiskInfo& disk,
                                                              uint32_t partitionNumber) {
        const auto it = std::find_if(disk.partitions.cbegin(),
                                     disk.partitions.cend(),
                                     [partitionNumber](const auto& partition) {
                                         return partition.partition_number == partitionNumber;
                                     });
        return it == disk.partitions.cend() ? nullptr : &(*it);
    }

    static void sortSegments(QVector<ApplyDiffSegment>* segments) {
        std::sort(segments->begin(), segments->end(), [](const auto& a, const auto& b) {
            return a.offset < b.offset;
        });
    }

    void drawTrack(QPainter* painter,
                   const QRect& rect,
                   const QVector<ApplyDiffSegment>& segments,
                   uint64_t totalBytes,
                   const QString& label) {
        painter->setPen(palette().color(QPalette::Text));
        painter->drawText(rect.left(),
                          rect.top(),
                          kApplyDiffPreviewTitleWidth,
                          rect.height(),
                          Qt::AlignVCenter,
                          label);
        const QRect track(rect.left() + kApplyDiffPreviewTitleWidth,
                          rect.top() + ui::kSpacingTight,
                          rect.width() - kApplyDiffPreviewTitleWidth,
                          rect.height() - (ui::kSpacingTight * 2));
        painter->fillRect(track, palette().color(QPalette::AlternateBase));
        const uint64_t safeTotal = std::max(totalBytes, kDiskMapMinimumBytes);
        for (const auto& segment : segments) {
            const int x = track.left() +
                          static_cast<int>((segment.offset * track.width()) / safeTotal);
            const int width =
                std::max(kTableDiskSeparatorWidth,
                         static_cast<int>((segment.bytes * track.width()) / safeTotal));
            QRect segmentRect(x, track.top(), width, track.height());
            segmentRect = segmentRect.intersected(track);
            if (segmentRect.isEmpty()) {
                continue;
            }
            painter->fillRect(segmentRect,
                              segment.changed
                                  ? segment.color
                                  : segment.color.lighter(kPartitionSegmentFillLightness));
            if (segment.used_percent > 0) {
                QRect usedRect = segmentRect;
                usedRect.setWidth((segmentRect.width() * segment.used_percent) /
                                  kPartitionProgressMax);
                painter->fillRect(usedRect, segment.color.darker(kPartitionSegmentTrackLightness));
            }
            const QString text = painter->fontMetrics().elidedText(segment.label,
                                                                   Qt::ElideRight,
                                                                   segmentRect.width());
            painter->setPen(palette().color(QPalette::Text));
            painter->drawText(segmentRect.adjusted(ui::kSpacingTight, 0, -ui::kSpacingTight, 0),
                              Qt::AlignCenter,
                              text);
        }
        painter->setPen(QPen(palette().color(QPalette::Mid), kTableDiskSeparatorWidth));
        painter->drawRect(track);
    }

    PartitionInventory m_inventory;
    QVector<PartitionOperation> m_operations;
};

QColor lightenedPaletteColor(QPalette::ColorRole role) {
    return QApplication::palette().color(role).lighter();
}

struct SizePreviewRow {
    QString label;
    uint64_t total_bytes{0};
    uint64_t filled_bytes{0};
    QColor fill_color;
    QString text;
    uint64_t offset_bytes{0};
};

class OperationSizePreviewWidget : public QWidget {
public:
    explicit OperationSizePreviewWidget(QWidget* parent = nullptr) : QWidget(parent) {
        setObjectName(QStringLiteral("partitionOperationSizePreview"));
        setAccessibleName(QStringLiteral("Partition operation graphical size preview"));
        setMinimumHeight(kOperationSizePreviewMinHeight);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        setMouseTracking(true);
    }

    void setRows(QVector<SizePreviewRow> rows) {
        m_rows = std::move(rows);
        update();
    }

    void setInteractiveSegment(int rowIndex,
                               bool leftHandle,
                               bool rightHandle,
                               std::function<void(uint64_t, uint64_t)> onChanged) {
        m_interaction.row_index = rowIndex;
        m_interaction.left_handle = leftHandle;
        m_interaction.right_handle = rightHandle;
        m_interaction.on_changed = std::move(onChanged);
        setToolTip(QObject::tr("Drag the partition bar handles to update size and location."));
        update();
    }

protected:
    void paintEvent(QPaintEvent* /*event*/) override {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.fillRect(rect(), palette().color(QPalette::Base));
        int top = ui::kMarginSmall;
        for (int rowIndex = 0; rowIndex < m_rows.size(); ++rowIndex) {
            const auto& row = m_rows.at(rowIndex);
            drawRow(&painter,
                    rowIndex,
                    row,
                    QRect(ui::kMarginSmall,
                          top,
                          width() - ui::kMarginSmall - ui::kMarginSmall,
                          kOperationSizePreviewRowHeight));
            top += kOperationSizePreviewRowHeight + ui::kSpacingSmall;
        }
    }

    void mousePressEvent(QMouseEvent* event) override {
        m_drag_handle = handleAt(event->position().toPoint());
        if (m_drag_handle == DragHandle::None) {
            QWidget::mousePressEvent(event);
            return;
        }
        setCursor(Qt::SizeHorCursor);
        updateDrag(event->position().toPoint());
        event->accept();
    }

    void mouseMoveEvent(QMouseEvent* event) override {
        if (m_drag_handle != DragHandle::None) {
            updateDrag(event->position().toPoint());
            event->accept();
            return;
        }
        const auto hoverHandle = handleAt(event->position().toPoint());
        if (hoverHandle == DragHandle::None) {
            unsetCursor();
        } else {
            setCursor(Qt::SizeHorCursor);
        }
        QWidget::mouseMoveEvent(event);
    }

    void mouseReleaseEvent(QMouseEvent* event) override {
        m_drag_handle = DragHandle::None;
        unsetCursor();
        QWidget::mouseReleaseEvent(event);
    }

private:
    enum class DragHandle {
        None,
        Left,
        Right,
    };

    struct DragInteraction {
        int row_index{kSizePreviewNoInteractiveRow};
        bool left_handle{false};
        bool right_handle{false};
        std::function<void(uint64_t, uint64_t)> on_changed;
    };

    void drawRow(QPainter* painter, int rowIndex, const SizePreviewRow& row, const QRect& rect) {
        painter->setPen(palette().color(QPalette::Text));
        painter->drawText(rect.left(),
                          rect.top(),
                          kOperationSizePreviewLabelWidth,
                          rect.height(),
                          Qt::AlignVCenter,
                          row.label);
        const QRect track(rect.left() + kOperationSizePreviewLabelWidth,
                          rect.top() + ui::kSpacingTight,
                          rect.width() - kOperationSizePreviewLabelWidth,
                          rect.height() - (ui::kSpacingTight * 2));
        painter->fillRect(track, palette().color(QPalette::AlternateBase));
        const uint64_t safeTotal = std::max(row.total_bytes, kDiskMapMinimumBytes);
        const int fillX = track.left() +
                          static_cast<int>((row.offset_bytes * track.width()) / safeTotal);
        QRect filled(fillX,
                     track.top(),
                     std::max(kTableDiskSeparatorWidth,
                              static_cast<int>((row.filled_bytes * track.width()) / safeTotal)),
                     track.height());
        painter->fillRect(filled.intersected(track), row.fill_color);
        drawInteractiveHandles(painter, rowIndex, row, track);
        painter->setPen(QPen(palette().color(QPalette::Mid), kTableDiskSeparatorWidth));
        painter->drawRect(track);
        painter->setPen(palette().color(QPalette::Text));
        painter->drawText(
            track.adjusted(ui::kSpacingTight, 0, -ui::kSpacingTight, 0),
            Qt::AlignCenter,
            painter->fontMetrics().elidedText(row.text, Qt::ElideRight, track.width()));
    }

    void drawInteractiveHandles(QPainter* painter,
                                int rowIndex,
                                const SizePreviewRow& row,
                                const QRect& track) {
        if (rowIndex != m_interaction.row_index) {
            return;
        }
        if (m_interaction.left_handle) {
            drawHandle(painter, handleX(track, row.total_bytes, row.offset_bytes), track);
        }
        if (m_interaction.right_handle) {
            drawHandle(
                painter,
                handleX(track, row.total_bytes, saturatingAdd(row.offset_bytes, row.filled_bytes)),
                track);
        }
    }

    void drawHandle(QPainter* painter, int centerX, const QRect& track) {
        QRect handle(centerX - kSizePreviewHandleHalfWidth,
                     track.top(),
                     kSizePreviewHandleHalfWidth * kHorizontalMarginCount,
                     track.height());
        handle = handle.intersected(track.adjusted(-kSizePreviewHandleHalfWidth,
                                                   ui::kMarginNone,
                                                   kSizePreviewHandleHalfWidth,
                                                   ui::kMarginNone));
        painter->fillRect(handle, palette().color(QPalette::Highlight));
        painter->setPen(QPen(palette().color(QPalette::HighlightedText), kTableDiskSeparatorWidth));
        painter->drawLine(handle.center().x(), handle.top(), handle.center().x(), handle.bottom());
    }

    [[nodiscard]] QRect trackRectForInteractiveRow() const {
        if (m_interaction.row_index < kSizePreviewCreateRow) {
            return {};
        }
        const int top = ui::kMarginSmall + (m_interaction.row_index *
                                            (kOperationSizePreviewRowHeight + ui::kSpacingSmall));
        const QRect rowRect(ui::kMarginSmall,
                            top,
                            width() - (ui::kMarginSmall * kHorizontalMarginCount),
                            kOperationSizePreviewRowHeight);
        return QRect(rowRect.left() + kOperationSizePreviewLabelWidth,
                     rowRect.top() + ui::kSpacingTight,
                     rowRect.width() - kOperationSizePreviewLabelWidth,
                     rowRect.height() - (ui::kSpacingTight * kHorizontalMarginCount));
    }

    [[nodiscard]] const SizePreviewRow* interactiveRow() const {
        if (m_interaction.row_index < kSizePreviewCreateRow ||
            m_interaction.row_index >= m_rows.size()) {
            return nullptr;
        }
        return &m_rows.at(m_interaction.row_index);
    }

    [[nodiscard]] DragHandle handleAt(const QPoint& position) const {
        const auto* row = interactiveRow();
        const QRect track = trackRectForInteractiveRow();
        if (!row || track.isEmpty() ||
            !track
                 .adjusted(ui::kMarginNone,
                           -kSizePreviewHandleHitWidth,
                           ui::kMarginNone,
                           kSizePreviewHandleHitWidth)
                 .contains(position)) {
            return DragHandle::None;
        }
        if (m_interaction.left_handle &&
            withinHandle(position.x(), handleX(track, row->total_bytes, row->offset_bytes))) {
            return DragHandle::Left;
        }
        if (m_interaction.right_handle &&
            withinHandle(position.x(),
                         handleX(track,
                                 row->total_bytes,
                                 saturatingAdd(row->offset_bytes, row->filled_bytes)))) {
            return DragHandle::Right;
        }
        return DragHandle::None;
    }

    [[nodiscard]] static bool withinHandle(int positionX, int handleCenterX) {
        const int distance = positionX > handleCenterX ? positionX - handleCenterX
                                                       : handleCenterX - positionX;
        return distance <= kSizePreviewHandleHitWidth;
    }

    [[nodiscard]] static int handleX(const QRect& track, uint64_t totalBytes, uint64_t bytes) {
        const uint64_t safeTotal = std::max(totalBytes, kDiskMapMinimumBytes);
        const uint64_t clampedBytes = std::min(bytes, safeTotal);
        return track.left() + static_cast<int>((clampedBytes * track.width()) / safeTotal);
    }

    [[nodiscard]] static uint64_t bytesForX(const QRect& track, uint64_t totalBytes, int x) {
        const int clampedX = std::clamp(x, track.left(), track.right());
        const uint64_t safeTotal = std::max(totalBytes, kDiskMapMinimumBytes);
        return (static_cast<uint64_t>(clampedX - track.left()) * safeTotal) /
               std::max(static_cast<uint64_t>(track.width()), kDiskMapMinimumBytes);
    }

    void updateDrag(const QPoint& position) {
        const auto* row = interactiveRow();
        const QRect track = trackRectForInteractiveRow();
        if (!row || track.isEmpty() || !m_interaction.on_changed) {
            return;
        }
        const uint64_t minimumSegmentBytes =
            std::min<uint64_t>(kMegabyteBytes, std::max(row->total_bytes, kDiskMapMinimumBytes));
        const uint64_t selectedBytes = bytesForX(track, row->total_bytes, position.x());
        const uint64_t startBytes = row->offset_bytes;
        const uint64_t endBytes = std::min(row->total_bytes,
                                           saturatingAdd(row->offset_bytes, row->filled_bytes));
        if (m_drag_handle == DragHandle::Left) {
            const uint64_t maxStart =
                endBytes > minimumSegmentBytes ? endBytes - minimumSegmentBytes : ui::kMarginNone;
            const uint64_t newStart = std::min(selectedBytes, maxStart);
            m_interaction.on_changed(newStart, endBytes - newStart);
        } else if (m_drag_handle == DragHandle::Right) {
            const uint64_t minEnd = std::min(row->total_bytes, startBytes + minimumSegmentBytes);
            const uint64_t newEnd = std::clamp(selectedBytes, minEnd, row->total_bytes);
            m_interaction.on_changed(startBytes, newEnd - startBytes);
        }
    }

    QVector<SizePreviewRow> m_rows;
    DragInteraction m_interaction;
    DragHandle m_drag_handle{DragHandle::None};
};

class PartitionOperationDialog : public QDialog {
public:
    PartitionOperationDialog(const QString& title,
                             const QString& targetIdentity,
                             const QString& warningText,
                             QWidget* parent = nullptr)
        : QDialog(parent) {
        setWindowTitle(title);
        setModal(true);
        setMinimumWidth(kOperationDialogMinWidth);
        setAccessibleName(title);

        auto* root = new QVBoxLayout(this);
        root->setContentsMargins(
            ui::kMarginMedium, ui::kMarginMedium, ui::kMarginMedium, ui::kMarginMedium);
        root->setSpacing(ui::kSpacingMedium);

        auto* target = new QLabel(targetIdentity, this);
        target->setWordWrap(true);
        target->setAccessibleName(QStringLiteral("Selected partition target"));
        root->addWidget(target);

        m_form = new QFormLayout();
        m_form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
        root->addLayout(m_form);

        m_visualLayout = new QVBoxLayout();
        m_visualLayout->setContentsMargins(
            ui::kMarginNone, ui::kMarginNone, ui::kMarginNone, ui::kMarginNone);
        root->addLayout(m_visualLayout);

        m_preview = new QLabel(this);
        m_preview->setWordWrap(true);
        m_preview->setAccessibleName(QStringLiteral("Operation preview"));
        root->addWidget(m_preview);

        auto* warning = new QLabel(warningText, this);
        warning->setWordWrap(true);
        warning->setAccessibleName(QStringLiteral("Operation warning"));
        root->addWidget(warning);

        auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
        m_accept_button = buttons->button(QDialogButtonBox::Ok);
        m_accept_button->setText(QStringLiteral("Add to Queue"));
        connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
        connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
        root->addWidget(buttons);
    }

    [[nodiscard]] QFormLayout* formLayout() const noexcept { return m_form; }

    void addVisualPreview(QWidget* preview) { m_visualLayout->addWidget(preview); }

    void setPreviewText(const QString& text) { m_preview->setText(text); }

    void setAcceptEnabled(bool enabled) {
        if (m_accept_button) {
            m_accept_button->setEnabled(enabled);
        }
    }

private:
    QFormLayout* m_form{nullptr};
    QVBoxLayout* m_visualLayout{nullptr};
    QLabel* m_preview{nullptr};
    QPushButton* m_accept_button{nullptr};
};

struct NonNativeCheckDialogControls {
    PartitionOperationDialog* dialog{nullptr};
    QComboBox* mode{nullptr};
    QLineEdit* target_path{nullptr};
    QCheckBox* repair_confirm{nullptr};
    QString read_only_target;
    QString write_target;
    QString file_system;
};

bool nonNativeCheckRepairMode(const QComboBox* mode) {
    return mode &&
           mode->currentData().toString() == PartitionFileSystemToolRunner::repairOperation();
}

bool nonNativeCheckHfsCatalogMode(const QString& mode) {
    return mode == QString::fromLatin1(kHfsCatalogCheckOperation);
}

void syncNonNativeCheckDialog(const NonNativeCheckDialogControls& controls) {
    const bool repairMode = nonNativeCheckRepairMode(controls.mode);
    const QString currentTarget = controls.target_path->text().trimmed();
    if (repairMode && currentTarget == controls.read_only_target &&
        !controls.write_target.isEmpty()) {
        controls.target_path->setText(controls.write_target);
    } else if (!repairMode && currentTarget == controls.write_target) {
        controls.target_path->setText(controls.read_only_target);
    }

    const QString target = controls.target_path->text().trimmed();
    controls.target_path->setReadOnly(repairMode);
    controls.target_path->setToolTip(
        repairMode
            ? QObject::tr("Queued repair uses the selected raw partition target.")
            : QObject::tr("Read-only checks can inspect the selected raw target or a lab image."));
    controls.repair_confirm->setVisible(repairMode);
    controls.dialog->setAcceptEnabled(!target.isEmpty() &&
                                      (!repairMode || controls.repair_confirm->isChecked()));
    controls.dialog->setPreviewText(
        repairMode ? QObject::tr("Queue %1 repair for %2.").arg(controls.file_system, target)
                   : QObject::tr("Run read-only check for %1.").arg(target));
}

void addHfsCatalogCheckMode(QComboBox* mode, const NonNativeFilesystemCheckState& state) {
    if (state.hfs_consistency_check) {
        mode->addItem(QObject::tr("Original HFS+ catalog check now"),
                      QString::fromLatin1(kHfsCatalogCheckOperation));
    }
}

void addNonNativeReadOnlyToolMode(QComboBox* mode, const NonNativeFilesystemCheckState& state) {
    if (!state.hfs_consistency_check || state.tool_check_available) {
        mode->addItem(QObject::tr("Read-only check now"),
                      PartitionFileSystemToolRunner::readOnlyCheckOperation());
    }
}

void addNonNativeRepairMode(QComboBox* mode, const NonNativeFilesystemCheckState& state) {
    if (!state.repair_available ||
        (!state.tool_check_available && !isApfsFilesystem(state.file_system))) {
        return;
    }
    if (isExtFilesystem(state.file_system)) {
        mode->addItem(QObject::tr("Queue ext repair"),
                      PartitionFileSystemToolRunner::repairOperation());
        return;
    }
    if (isHfsFilesystem(state.file_system)) {
        mode->addItem(QObject::tr("Queue HFS+ repair"),
                      PartitionFileSystemToolRunner::repairOperation());
        return;
    }
    if (isApfsFilesystem(state.file_system)) {
        mode->addItem(QObject::tr("Queue generated APFS repair"),
                      PartitionFileSystemToolRunner::repairOperation());
    }
}

QString nonNativeRepairToolId(const QString& fileSystem) {
    if (isApfsFilesystem(fileSystem)) {
        return QStringLiteral("sak_apfs_writer_cli");
    }
    return isHfsFilesystem(fileSystem) ? QStringLiteral("fsck_hfs") : QStringLiteral("e2fsck");
}

QString nonNativeRepairFamilyLabel(const QString& fileSystem) {
    if (isApfsFilesystem(fileSystem)) {
        return QStringLiteral("APFS");
    }
    return isHfsFilesystem(fileSystem) ? QStringLiteral("HFS+") : QStringLiteral("ext");
}

QString nonNativeRepairConfirmationText(const QString& fileSystem, const QString& toolId) {
    if (isHfsFilesystem(fileSystem)) {
        return QObject::tr(
                   "I understand this will stage a sparse HFS image, run bundled %1, "
                   "and write repaired metadata back on Apply.")
            .arg(toolId);
    }
    if (isApfsFilesystem(fileSystem)) {
        return QObject::tr(
                   "I understand this will run %1 against only S.A.K. generated APFS metadata "
                   "and write repaired checksums back on Apply.")
            .arg(toolId);
    }
    return QObject::tr("I understand this will run bundled %1 repair on Apply.").arg(toolId);
}

std::optional<NonNativeFilesystemCheckRequest> showNonNativeCheckRequestDialog(
    QWidget* parent, const NonNativeFilesystemCheckState& state, const QString& writeTarget) {
    PartitionOperationDialog dialog(
        QObject::tr("Check Non-Windows File System"),
        state.target_path,
        QObject::tr("Run a read-only check now or queue an approved repair for Apply."),
        parent);
    auto* mode = new QComboBox(&dialog);
    mode->setAccessibleName(QObject::tr("Non-Windows filesystem check mode"));
    addHfsCatalogCheckMode(mode, state);
    addNonNativeReadOnlyToolMode(mode, state);
    addNonNativeRepairMode(mode, state);

    auto* targetPath = new QLineEdit(state.target_path, &dialog);
    targetPath->setAccessibleName(QObject::tr("Non-Windows filesystem target path"));
    const QString repairTool = nonNativeRepairToolId(state.file_system);
    const QString repairFamily = nonNativeRepairFamilyLabel(state.file_system);
    auto* repairConfirm =
        new QCheckBox(nonNativeRepairConfirmationText(state.file_system, repairTool), &dialog);
    repairConfirm->setAccessibleName(QObject::tr("Confirm %1 filesystem repair").arg(repairFamily));
    dialog.formLayout()->addRow(QObject::tr("Mode:"), mode);
    dialog.formLayout()->addRow(QObject::tr("Target path:"), targetPath);
    dialog.formLayout()->addRow(QString(), repairConfirm);
    if (!state.repair_available && !state.repair_reason.isEmpty()) {
        mode->setToolTip(state.repair_reason);
    }

    const NonNativeCheckDialogControls controls{&dialog,
                                                mode,
                                                targetPath,
                                                repairConfirm,
                                                state.target_path,
                                                writeTarget,
                                                state.file_system};
    auto updatePreview = [&controls]() {
        syncNonNativeCheckDialog(controls);
    };
    QObject::connect(mode, &QComboBox::currentTextChanged, &dialog, updatePreview);
    QObject::connect(targetPath, &QLineEdit::textChanged, &dialog, updatePreview);
    QObject::connect(repairConfirm, &QCheckBox::toggled, &dialog, updatePreview);
    syncNonNativeCheckDialog(controls);

    if (dialog.exec() != QDialog::Accepted) {
        return std::nullopt;
    }
    return NonNativeFilesystemCheckRequest{mode->currentData().toString(),
                                           targetPath->text().trimmed(),
                                           repairConfirm->isChecked()};
}

struct CreatePartitionWidgets {
    QSpinBox* size_mb{nullptr};
    QSpinBox* free_before_mb{nullptr};
    QSlider* size_handle{nullptr};
    QSlider* location_handle{nullptr};
    QComboBox* partition_type{nullptr};
    QComboBox* file_system{nullptr};
    QComboBox* allocation_unit{nullptr};
    QComboBox* swap_page_size{nullptr};
    QLineEdit* label{nullptr};
    QComboBox* drive_letter{nullptr};
    QCheckBox* full_format{nullptr};
    QCheckBox* raw_format_confirm{nullptr};
    OperationSizePreviewWidget* size_preview{nullptr};
};

enum class RawFormatKind {
    None,
    Ext,
    Hfs,
    Swap,
    Apfs
};

RawFormatKind rawFormatKindForFileSystem(const QString& fileSystem);
QString rawFormatConfirmationAccessibleName(RawFormatKind kind);
QString rawFormatConfirmationText(RawFormatKind kind);
QComboBox* createLinuxSwapPageSizeSelector(QWidget* parent);

struct ResizePartitionWidgets {
    QComboBox* mode{nullptr};
    QSpinBox* target_size_mb{nullptr};
    QSlider* target_size_handle{nullptr};
    QSpinBox* target_offset_mb{nullptr};
    QLabel* adjacent_free_label{nullptr};
    QComboBox* donor_partition{nullptr};
    QLineEdit* backup_directory{nullptr};
    QPushButton* browse_backup{nullptr};
    QCheckBox* confirmation{nullptr};
    QCheckBox* non_native_confirmation{nullptr};
    QLabel* mode_status{nullptr};
    OperationSizePreviewWidget* size_preview{nullptr};
    QString file_system;
    bool non_native_ext{false};
    int current_mb{1};
    int current_offset_mb{0};
    uint64_t current_bytes{0};
    uint64_t current_offset_bytes{0};
    uint64_t adjacent_free_bytes{0};
    uint64_t max_online_bytes{0};
    uint64_t disk_bytes{0};
};

struct QuickPartitionWidgets {
    QComboBox* preset_selector{nullptr};
    QPushButton* preset_load{nullptr};
    QPushButton* preset_save{nullptr};
    QPushButton* preset_delete{nullptr};
    QSpinBox* partition_count{nullptr};
    QComboBox* partition_style{nullptr};
    QComboBox* size_mode{nullptr};
    QTableWidget* partition_table{nullptr};
    QComboBox* file_system{nullptr};
    QComboBox* allocation_unit{nullptr};
    QLineEdit* label_prefix{nullptr};
    QCheckBox* full_format{nullptr};
};

struct ClusterSizeWidgets {
    QComboBox* file_system{nullptr};
    QComboBox* allocation_unit{nullptr};
    QLineEdit* label{nullptr};
    QLineEdit* backup_directory{nullptr};
    QPushButton* browse_backup{nullptr};
    QCheckBox* confirmation{nullptr};
};

struct AllocateFreeSpaceWidgets {
    QSpinBox* amount_mb{nullptr};
    QSlider* amount_handle{nullptr};
    QLineEdit* backup_directory{nullptr};
    QPushButton* browse_backup{nullptr};
    QCheckBox* confirmation{nullptr};
    QLabel* donor_status{nullptr};
    OperationSizePreviewWidget* size_preview{nullptr};
    uint64_t target_bytes{0};
    uint64_t donor_bytes{0};
    uint64_t max_allocate_bytes{0};
};

struct BackupRestoreWidgets {
    QLineEdit* backup_directory{nullptr};
    QPushButton* browse_backup{nullptr};
    QCheckBox* confirmation{nullptr};
    QLabel* status{nullptr};
};

struct FreeSpaceAllocationChoice {
    uint32_t partition_number{0};
    QString label;
    bool move_partition_start{false};
};

struct FreeSpaceAllocationWidgets {
    QComboBox* target_partition{nullptr};
    QSpinBox* amount_mb{nullptr};
    QSlider* amount_handle{nullptr};
    BackupRestoreWidgets backup;
    OperationSizePreviewWidget* preview{nullptr};
    QVector<FreeSpaceAllocationChoice> choices;
    uint64_t free_bytes{0};
};

struct PropertyRow {
    QString name;
    QString value;
};

QString gptBasicDataType() {
    return QStringLiteral("{EBD0A0A2-B9E5-4433-87C0-68B6B72699C7}");
}

QString gptRecoveryType() {
    return QStringLiteral("{DE94BBA4-06D1-4D40-A16A-BFD50179D6AC}");
}

QString gptEfiSystemType() {
    return QStringLiteral("{C12A7328-F81F-11D2-BA4B-00A0C93EC93B}");
}

QString gptApfsType() {
    return QStringLiteral("{7C3457EF-0000-11AA-AA11-00306543ECAC}");
}

QString partitionTypePayloadValue(const QString& key, const QString& value) {
    return key + QLatin1Char(':') + value;
}

void addPartitionTypeChoice(QComboBox* combo,
                            const QString& label,
                            const QString& key,
                            const QString& value) {
    combo->addItem(label, partitionTypePayloadValue(key, value));
}

QComboBox* createPartitionTypeSelector(QWidget* parent, const PartitionDiskInfo* disk) {
    auto* combo = new QComboBox(parent);
    combo->setAccessibleName(QObject::tr("Partition type"));
    combo->addItem(QObject::tr("Default data partition"), QString());
    const QString partitionScheme = disk ? disk->partition_style.toUpper() : QString();
    if (partitionScheme == QStringLiteral("GPT")) {
        addPartitionTypeChoice(
            combo, QObject::tr("GPT Basic Data"), QStringLiteral("gpt"), gptBasicDataType());
        addPartitionTypeChoice(
            combo, QObject::tr("GPT Recovery"), QStringLiteral("gpt"), gptRecoveryType());
        addPartitionTypeChoice(
            combo, QObject::tr("GPT EFI System"), QStringLiteral("gpt"), gptEfiSystemType());
        addPartitionTypeChoice(
            combo, QObject::tr("GPT Apple APFS"), QStringLiteral("gpt"), gptApfsType());
    } else if (partitionScheme == QStringLiteral("MBR")) {
        addPartitionTypeChoice(combo,
                               QObject::tr("MBR IFS/NTFS primary"),
                               QStringLiteral("mbr"),
                               QStringLiteral("IFS"));
        addPartitionTypeChoice(combo,
                               QObject::tr("MBR FAT32 primary"),
                               QStringLiteral("mbr"),
                               QStringLiteral("FAT32"));
    }
    return combo;
}

QComboBox* createAllocationUnitSelector(QWidget* parent) {
    auto* combo = new QComboBox(parent);
    combo->setAccessibleName(QObject::tr("Cluster size"));
    combo->addItem(QObject::tr("Default"),
                   QVariant::fromValue<qulonglong>(kAllocationUnitDefaultBytes));
    combo->addItem(QObject::tr("512 bytes"),
                   QVariant::fromValue<qulonglong>(kAllocationUnit512Bytes));
    combo->addItem(QObject::tr("1 KB"), QVariant::fromValue<qulonglong>(kAllocationUnit1KbBytes));
    combo->addItem(QObject::tr("2 KB"), QVariant::fromValue<qulonglong>(kAllocationUnit2KbBytes));
    combo->addItem(QObject::tr("4 KB"), QVariant::fromValue<qulonglong>(kAllocationUnit4KbBytes));
    combo->addItem(QObject::tr("8 KB"), QVariant::fromValue<qulonglong>(kAllocationUnit8KbBytes));
    combo->addItem(QObject::tr("16 KB"), QVariant::fromValue<qulonglong>(kAllocationUnit16KbBytes));
    combo->addItem(QObject::tr("32 KB"), QVariant::fromValue<qulonglong>(kAllocationUnit32KbBytes));
    combo->addItem(QObject::tr("64 KB"), QVariant::fromValue<qulonglong>(kAllocationUnit64KbBytes));
    return combo;
}

uint64_t selectedAllocationUnitBytes(const QComboBox* combo) {
    return combo->currentData().toULongLong();
}

QString selectedAllocationUnitText(const QComboBox* combo) {
    return selectedAllocationUnitBytes(combo) == kAllocationUnitDefaultBytes
               ? QObject::tr("default cluster size")
               : QObject::tr("%1 cluster size").arg(combo->currentText());
}

QString selectedPartitionTypeText(const QComboBox* combo) {
    return combo->currentData().toString().isEmpty() ? QObject::tr("default type")
                                                     : combo->currentText();
}

QComboBox* createCreatePartitionFileSystemSelector(QWidget* parent) {
    auto* combo = new QComboBox(parent);
    combo->addItems({QStringLiteral("NTFS"),
                     QStringLiteral("exFAT"),
                     QStringLiteral("FAT32"),
                     QStringLiteral("ext2"),
                     QStringLiteral("ext3"),
                     QStringLiteral("ext4"),
                     QStringLiteral("HFS+"),
                     QStringLiteral("HFSX"),
                     QStringLiteral("APFS"),
                     QStringLiteral("Linux swap")});
    combo->setAccessibleName(QObject::tr("File system"));
    return combo;
}

QComboBox* createDriveLetterSelector(QWidget* parent) {
    auto* combo = new QComboBox(parent);
    combo->addItem(QObject::tr("Automatic"), QString());
    for (QChar letterChar = QLatin1Char('D'); letterChar <= QLatin1Char('Z');
         letterChar = QChar(letterChar.unicode() + 1)) {
        combo->addItem(QString(letterChar) + QStringLiteral(":"), QString(letterChar));
    }
    combo->setAccessibleName(QObject::tr("Drive letter"));
    return combo;
}

QSlider* createSizeHandle(
    QWidget* parent, const QString& accessibleName, int minimumMb, int maximumMb, int valueMb) {
    auto* slider = new QSlider(Qt::Horizontal, parent);
    slider->setRange(minimumMb, maximumMb);
    slider->setValue(std::clamp(valueMb, minimumMb, maximumMb));
    slider->setSingleStep(kSizeHandleSingleStepMb);
    slider->setPageStep(kSizeHandlePageStepMb);
    slider->setAccessibleName(accessibleName);
    slider->setToolTip(accessibleName);
    return slider;
}

int inputMegabytesFromBytes(uint64_t bytes, int minimumMb, int maximumMb) {
    const auto megabytes = static_cast<int>(
        std::min<uint64_t>(static_cast<uint64_t>(kMaxSizeInputMb), bytes / kMegabyteBytes));
    return std::clamp(megabytes, minimumMb, maximumMb);
}

QString defaultFileSystemForPartitionType(const QString& value) {
    if (value == partitionTypePayloadValue(QStringLiteral("gpt"), gptEfiSystemType()) ||
        value == partitionTypePayloadValue(QStringLiteral("mbr"), QStringLiteral("FAT32"))) {
        return QStringLiteral("FAT32");
    }
    if (value == partitionTypePayloadValue(QStringLiteral("gpt"), gptRecoveryType()) ||
        value == partitionTypePayloadValue(QStringLiteral("mbr"), QStringLiteral("IFS"))) {
        return QStringLiteral("NTFS");
    }
    if (value == partitionTypePayloadValue(QStringLiteral("gpt"), gptApfsType())) {
        return QStringLiteral("APFS");
    }
    return QString();
}

void selectApfsPartitionTypeIfDefault(const CreatePartitionWidgets& widgets) {
    if (!isApfsFilesystem(widgets.file_system->currentText())) {
        return;
    }
    const QString apfsType = partitionTypePayloadValue(QStringLiteral("gpt"), gptApfsType());
    const QString currentType = widgets.partition_type->currentData().toString();
    if (!currentType.isEmpty() &&
        currentType != partitionTypePayloadValue(QStringLiteral("gpt"), gptBasicDataType())) {
        return;
    }
    const int apfsIndex = widgets.partition_type->findData(apfsType);
    if (apfsIndex >= 0) {
        widgets.partition_type->setCurrentIndex(apfsIndex);
    }
}

void applyPartitionTypeFileSystemDefault(const CreatePartitionWidgets& widgets) {
    const QString targetFileSystem =
        defaultFileSystemForPartitionType(widgets.partition_type->currentData().toString());
    if (!targetFileSystem.isEmpty() &&
        widgets.file_system->currentText().compare(targetFileSystem, Qt::CaseInsensitive) != 0) {
        widgets.file_system->setCurrentText(targetFileSystem);
    }
    selectApfsPartitionTypeIfDefault(widgets);
}

void addCreatePartitionFormRows(const PartitionOperationDialog& dialog,
                                const CreatePartitionWidgets& widgets) {
    dialog.formLayout()->addRow(QObject::tr("Size:"), widgets.size_mb);
    dialog.formLayout()->addRow(QObject::tr("Size handle:"), widgets.size_handle);
    dialog.formLayout()->addRow(QObject::tr("Free space before:"), widgets.free_before_mb);
    dialog.formLayout()->addRow(QObject::tr("Location handle:"), widgets.location_handle);
    dialog.formLayout()->addRow(QObject::tr("Partition type:"), widgets.partition_type);
    dialog.formLayout()->addRow(QObject::tr("File system:"), widgets.file_system);
    dialog.formLayout()->addRow(QObject::tr("Cluster size:"), widgets.allocation_unit);
    dialog.formLayout()->addRow(QObject::tr("Swap page size:"), widgets.swap_page_size);
    dialog.formLayout()->addRow(QObject::tr("Label:"), widgets.label);
    dialog.formLayout()->addRow(QObject::tr("Drive letter:"), widgets.drive_letter);
    dialog.formLayout()->addRow(QString(), widgets.full_format);
    dialog.formLayout()->addRow(QString(), widgets.raw_format_confirm);
}

CreatePartitionWidgets addCreatePartitionControls(PartitionOperationDialog& dialog,
                                                  uint64_t free_bytes,
                                                  const PartitionDiskInfo* disk) {
    CreatePartitionWidgets widgets;
    widgets.size_mb = new QSpinBox(&dialog);
    widgets.size_mb->setRange(
        1,
        std::max(
            1, static_cast<int>(std::min<uint64_t>(kMaxSizeInputMb, free_bytes / kMegabyteBytes))));
    widgets.size_mb->setValue(std::min(kDefaultCreateSizeMb, widgets.size_mb->maximum()));
    widgets.size_mb->setSuffix(QObject::tr(" MB"));
    widgets.size_mb->setAccessibleName(QObject::tr("Partition size"));
    widgets.free_before_mb = new QSpinBox(&dialog);
    widgets.free_before_mb->setRange(
        0,
        std::max(
            0, static_cast<int>(std::min<uint64_t>(kMaxSizeInputMb, free_bytes / kMegabyteBytes))));
    widgets.free_before_mb->setSuffix(QObject::tr(" MB"));
    widgets.free_before_mb->setAccessibleName(QObject::tr("Free space before new partition"));
    widgets.size_handle = createSizeHandle(&dialog,
                                           QObject::tr("Partition size handle"),
                                           widgets.size_mb->minimum(),
                                           widgets.size_mb->maximum(),
                                           widgets.size_mb->value());
    widgets.location_handle = createSizeHandle(&dialog,
                                               QObject::tr("Free space before handle"),
                                               widgets.free_before_mb->minimum(),
                                               widgets.free_before_mb->maximum(),
                                               widgets.free_before_mb->value());

    widgets.partition_type = createPartitionTypeSelector(&dialog, disk);
    widgets.file_system = createCreatePartitionFileSystemSelector(&dialog);
    widgets.allocation_unit = createAllocationUnitSelector(&dialog);
    widgets.swap_page_size = createLinuxSwapPageSizeSelector(&dialog);
    widgets.label = new QLineEdit(QStringLiteral("Data"), &dialog);
    widgets.label->setAccessibleName(QObject::tr("Partition label"));
    widgets.drive_letter = createDriveLetterSelector(&dialog);
    widgets.full_format = new QCheckBox(QObject::tr("Full format"), &dialog);
    widgets.full_format->setAccessibleName(QObject::tr("Full format"));
    widgets.raw_format_confirm = new QCheckBox(
        QObject::tr("I understand this will run bundled mke2fs against the raw partition."),
        &dialog);
    widgets.raw_format_confirm->setAccessibleName(QObject::tr("Confirm ext filesystem format"));

    addCreatePartitionFormRows(dialog, widgets);
    widgets.size_preview = new OperationSizePreviewWidget(&dialog);
    dialog.addVisualPreview(widgets.size_preview);
    return widgets;
}

void applyRawFormatControlState(PartitionOperationDialog& dialog,
                                const CreatePartitionWidgets& widgets,
                                RawFormatKind rawKind) {
    const bool rawSelected = rawKind != RawFormatKind::None;
    const bool swapSelected = rawKind == RawFormatKind::Swap;

    widgets.allocation_unit->setEnabled(!rawSelected);
    widgets.drive_letter->setEnabled(!rawSelected);
    widgets.full_format->setEnabled(!rawSelected);
    widgets.swap_page_size->setVisible(swapSelected);
    widgets.swap_page_size->setEnabled(swapSelected);
    widgets.raw_format_confirm->setVisible(rawSelected);
    widgets.raw_format_confirm->setAccessibleName(rawFormatConfirmationAccessibleName(rawKind));
    widgets.raw_format_confirm->setText(rawFormatConfirmationText(rawKind));
    dialog.setAcceptEnabled(!rawSelected || widgets.raw_format_confirm->isChecked());
}

QString createPartitionFormatText(const CreatePartitionWidgets& widgets, RawFormatKind rawKind) {
    switch (rawKind) {
    case RawFormatKind::Ext:
        return QObject::tr(" Format with bundled mke2fs.");
    case RawFormatKind::Hfs:
        return QObject::tr(" Format through sparse staging with bundled newfs_hfs.");
    case RawFormatKind::Swap:
        return QObject::tr(" Write Linux SWAPSPACE2 metadata with %1 byte pages.")
            .arg(widgets.swap_page_size->currentText());
    case RawFormatKind::Apfs:
        return QObject::tr(" Format with S.A.K. generated APFS metadata.");
    case RawFormatKind::None:
        break;
    }
    return QObject::tr(" Format with %1.").arg(selectedAllocationUnitText(widgets.allocation_unit));
}

void updateCreatePartitionPreview(PartitionOperationDialog& dialog,
                                  const CreatePartitionWidgets& widgets,
                                  uint64_t free_bytes) {
    applyPartitionTypeFileSystemDefault(widgets);
    const RawFormatKind rawKind = rawFormatKindForFileSystem(widgets.file_system->currentText());
    applyRawFormatControlState(dialog, widgets, rawKind);

    const uint64_t requestedBytes = static_cast<uint64_t>(widgets.size_mb->value()) *
                                    kMegabyteBytes;
    const int maxBeforeMb = static_cast<int>(std::min<uint64_t>(
        kMaxSizeInputMb,
        (free_bytes > requestedBytes ? (free_bytes - requestedBytes) / kMegabyteBytes : 0)));
    widgets.free_before_mb->setMaximum(maxBeforeMb);
    {
        const QSignalBlocker sizeBlocker(widgets.size_handle);
        widgets.size_handle->setRange(widgets.size_mb->minimum(), widgets.size_mb->maximum());
        widgets.size_handle->setValue(widgets.size_mb->value());
    }
    {
        const QSignalBlocker locationBlocker(widgets.location_handle);
        widgets.location_handle->setRange(widgets.free_before_mb->minimum(), maxBeforeMb);
        widgets.location_handle->setValue(widgets.free_before_mb->value());
    }
    const uint64_t beforeBytes = static_cast<uint64_t>(widgets.free_before_mb->value()) *
                                 kMegabyteBytes;
    const uint64_t usedBytes = beforeBytes + requestedBytes;
    const uint64_t afterBytes = free_bytes > usedBytes ? free_bytes - usedBytes : 0;
    widgets.size_preview->setRows({{QObject::tr("Free space"),
                                    free_bytes,
                                    requestedBytes,
                                    QColor(QString::fromLatin1(ui::kColorSuccess)),
                                    QObject::tr("New %1 | Remaining %2")
                                        .arg(formatPartitionBytes(requestedBytes),
                                             formatPartitionBytes(beforeBytes + afterBytes)),
                                    beforeBytes}});
    const QString formatText = createPartitionFormatText(widgets, rawKind);
    dialog.setPreviewText(QObject::tr("Create %1 MB %2 %3 partition labeled \"%4\".")
                              .arg(widgets.size_mb->value())
                              .arg(selectedPartitionTypeText(widgets.partition_type),
                                   widgets.file_system->currentText(),
                                   widgets.label->text()) +
                          formatText);
}

QJsonObject createPartitionPayload(const CreatePartitionWidgets& widgets) {
    QJsonObject payload;
    payload[QStringLiteral("size_bytes")] =
        QString::number(static_cast<uint64_t>(widgets.size_mb->value()) * kMegabyteBytes);
    payload[QStringLiteral("relative_offset_bytes")] =
        QString::number(static_cast<uint64_t>(widgets.free_before_mb->value()) * kMegabyteBytes);
    payload[QStringLiteral("file_system")] = widgets.file_system->currentText();
    payload[QStringLiteral("allocation_unit_bytes")] =
        QString::number(selectedAllocationUnitBytes(widgets.allocation_unit));
    const RawFormatKind rawKind = rawFormatKindForFileSystem(widgets.file_system->currentText());
    if (rawKind != RawFormatKind::None) {
        payload[QStringLiteral("non_native_file_system_tool")] = true;
        payload[QStringLiteral("target_wipe_confirmed")] = widgets.raw_format_confirm->isChecked();
    }
    if (rawKind == RawFormatKind::Swap) {
        payload[QStringLiteral("linux_swap_page_size_bytes")] =
            widgets.swap_page_size->currentText();
    }
    const QString typeValue = widgets.partition_type->currentData().toString();
    if (typeValue.startsWith(QStringLiteral("gpt:"))) {
        payload[QStringLiteral("gpt_type")] = typeValue.mid(QStringLiteral("gpt:").size());
    } else if (typeValue.startsWith(QStringLiteral("mbr:"))) {
        payload[QStringLiteral("mbr_type")] = typeValue.mid(QStringLiteral("mbr:").size());
    }
    payload[QStringLiteral("label")] = widgets.label->text();
    payload[QStringLiteral("full_format")] = widgets.full_format->isChecked();
    const QString letter = widgets.drive_letter->currentData().toString();
    if (!letter.isEmpty()) {
        payload[QStringLiteral("drive_letter")] = letter;
    }
    return payload;
}

uint64_t alignDownToMegabyte(uint64_t bytes) {
    const auto megabyte = static_cast<uint64_t>(kMegabyteBytes);
    return (bytes / megabyte) * megabyte;
}

uint64_t quickPartitionUsableBytes(const PartitionDiskInfo& disk) {
    const auto megabyte = static_cast<uint64_t>(kMegabyteBytes);
    const auto reservedBytes = static_cast<uint64_t>(kQuickPartitionReservedTailMb + 1) * megabyte;
    return disk.size_bytes > reservedBytes ? alignDownToMegabyte(disk.size_bytes - reservedBytes)
                                           : 0;
}

uint64_t quickPartitionSliceBytes(uint64_t usableBytes, int partitionCount, int partitionIndex) {
    const auto count = static_cast<uint64_t>(std::max(1, partitionCount));
    const uint64_t baseBytes = alignDownToMegabyte(usableBytes / count);
    const uint64_t usedBefore = baseBytes * static_cast<uint64_t>(partitionIndex);
    return partitionIndex == partitionCount - 1 ? usableBytes - usedBefore : baseBytes;
}

bool quickPartitionCustomMode(const QuickPartitionWidgets& widgets) {
    return widgets.size_mode &&
           widgets.size_mode->currentData().toString() == QStringLiteral("custom");
}

QLineEdit* quickPartitionLabelEditor(const QuickPartitionWidgets& widgets, int row) {
    return widgets.partition_table
               ? qobject_cast<QLineEdit*>(
                     widgets.partition_table->cellWidget(row, kQuickPartitionLabelColumn))
               : nullptr;
}

QSpinBox* quickPartitionSizeEditor(const QuickPartitionWidgets& widgets, int row) {
    return widgets.partition_table
               ? qobject_cast<QSpinBox*>(
                     widgets.partition_table->cellWidget(row, kQuickPartitionSizeColumn))
               : nullptr;
}

QVector<uint64_t> quickPartitionEqualSizes(uint64_t usableBytes, int partitionCount) {
    QVector<uint64_t> sizes;
    sizes.reserve(partitionCount);
    for (int index = 0; index < partitionCount; ++index) {
        sizes.append(quickPartitionSliceBytes(usableBytes, partitionCount, index));
    }
    return sizes;
}

int quickPartitionRawMaxCount(const PartitionDiskInfo& disk) {
    const auto minimumBytes = static_cast<uint64_t>(kQuickPartitionMinimumSizeMb) * kMegabyteBytes;
    return static_cast<int>(
        std::max<uint64_t>(1,
                           std::min<uint64_t>(kQuickPartitionMaxCount,
                                              quickPartitionUsableBytes(disk) / minimumBytes)));
}

int quickPartitionMaxCountForScheme(const PartitionDiskInfo& disk, const QString& scheme) {
    const int rawMax = quickPartitionRawMaxCount(disk);
    return scheme.compare(QStringLiteral("MBR"), Qt::CaseInsensitive) == 0
               ? std::min(rawMax, kQuickPartitionMbrDataMaxCount)
               : rawMax;
}

QVector<uint64_t> quickPartitionSizesFromWidgets(const QuickPartitionWidgets& widgets,
                                                 uint64_t usableBytes) {
    const int count = widgets.partition_count ? widgets.partition_count->value() : 0;
    if (!quickPartitionCustomMode(widgets)) {
        return quickPartitionEqualSizes(usableBytes, count);
    }

    QVector<uint64_t> sizes;
    sizes.reserve(count);
    for (int row = 0; row < count; ++row) {
        const auto* sizeEditor = quickPartitionSizeEditor(widgets, row);
        const auto sizeMb = static_cast<uint64_t>(sizeEditor ? sizeEditor->value() : 0);
        sizes.append(sizeMb * kMegabyteBytes);
    }
    return sizes;
}

QVector<uint64_t> quickPartitionSizesFromOptions(const QJsonObject& options, uint64_t usableBytes) {
    const int count = options.value(QStringLiteral("partition_count")).toInt();
    if (options.value(QStringLiteral("size_mode")).toString() != QStringLiteral("custom")) {
        return quickPartitionEqualSizes(usableBytes, count);
    }

    const auto array = options.value(QStringLiteral("custom_size_bytes")).toArray();
    if (array.size() != count) {
        return quickPartitionEqualSizes(usableBytes, count);
    }
    QVector<uint64_t> sizes;
    sizes.reserve(count);
    for (const auto& value : array) {
        bool ok = false;
        const auto sizeBytes = value.toString().toULongLong(&ok);
        if (!ok || sizeBytes == 0) {
            return quickPartitionEqualSizes(usableBytes, count);
        }
        sizes.append(sizeBytes);
    }
    return sizes;
}

uint64_t quickPartitionTotalBytes(const QVector<uint64_t>& sizes) {
    uint64_t total = 0;
    for (const auto size : sizes) {
        total += size;
    }
    return total;
}

QJsonArray quickPartitionLabelsJson(const QuickPartitionWidgets& widgets) {
    QJsonArray labels;
    const int count = widgets.partition_count ? widgets.partition_count->value() : 0;
    for (int row = 0; row < count; ++row) {
        const auto* labelEditor = quickPartitionLabelEditor(widgets, row);
        labels.append(labelEditor ? labelEditor->text() : QString());
    }
    return labels;
}

QJsonArray quickPartitionSizeJson(const QVector<uint64_t>& sizes) {
    QJsonArray values;
    for (const auto size : sizes) {
        values.append(QString::number(size));
    }
    return values;
}

QString quickPartitionLabelFromOptions(const QJsonObject& options, int partitionIndex) {
    if (options.value(QStringLiteral("size_mode")).toString() == QStringLiteral("custom")) {
        const auto labels = options.value(QStringLiteral("custom_labels")).toArray();
        const QString customLabel = partitionIndex < labels.size()
                                        ? labels.at(partitionIndex).toString().trimmed()
                                        : QString();
        if (!customLabel.isEmpty()) {
            return customLabel;
        }
    }
    return QStringLiteral("%1%2")
        .arg(options.value(QStringLiteral("label_prefix")).toString())
        .arg(partitionIndex + 1);
}

bool quickPartitionSizesAreValid(const QVector<uint64_t>& sizes, uint64_t usableBytes) {
    const auto minimumBytes = static_cast<uint64_t>(kQuickPartitionMinimumSizeMb) * kMegabyteBytes;
    if (sizes.isEmpty() || quickPartitionTotalBytes(sizes) > usableBytes) {
        return false;
    }
    return std::all_of(sizes.cbegin(), sizes.cend(), [minimumBytes](uint64_t size) {
        return size >= minimumBytes;
    });
}

QJsonArray quickPartitionPresetArray() {
    const QString raw =
        ConfigManager::instance().getValue(kQuickPartitionPresetsConfigKey).toString();
    if (raw.trimmed().isEmpty()) {
        return {};
    }
    QJsonParseError error;
    const auto document = QJsonDocument::fromJson(raw.toUtf8(), &error);
    return error.error == QJsonParseError::NoError && document.isArray() ? document.array()
                                                                         : QJsonArray{};
}

void storeQuickPartitionPresetArray(const QJsonArray& presets) {
    ConfigManager::instance().setValue(kQuickPartitionPresetsConfigKey,
                                       QString::fromUtf8(
                                           QJsonDocument(presets).toJson(QJsonDocument::Compact)));
    ConfigManager::instance().sync();
}

QString quickPartitionPresetName(const QJsonObject& preset) {
    return preset.value(QStringLiteral("name")).toString().trimmed();
}

std::optional<QJsonObject> findQuickPartitionPreset(const QString& name) {
    const auto presets = quickPartitionPresetArray();
    for (const auto& value : presets) {
        const auto object = value.toObject();
        if (quickPartitionPresetName(object).compare(name, Qt::CaseInsensitive) == 0) {
            return object;
        }
    }
    return std::nullopt;
}

void removeQuickPartitionPreset(const QString& name) {
    QJsonArray filtered;
    const auto presets = quickPartitionPresetArray();
    for (const auto& value : presets) {
        const auto object = value.toObject();
        if (quickPartitionPresetName(object).compare(name, Qt::CaseInsensitive) != 0) {
            filtered.append(object);
        }
    }
    storeQuickPartitionPresetArray(filtered);
}

QJsonArray quickPartitionCustomSizeMegabytesJson(const QuickPartitionWidgets& widgets) {
    QJsonArray values;
    const int count = widgets.partition_count ? widgets.partition_count->value() : 0;
    for (int row = 0; row < count; ++row) {
        const auto* sizeEditor = quickPartitionSizeEditor(widgets, row);
        values.append(sizeEditor ? sizeEditor->value() : kQuickPartitionMinimumSizeMb);
    }
    return values;
}

QJsonObject quickPartitionOptions(const QuickPartitionWidgets& widgets);

QJsonObject quickPartitionPresetFromWidgets(const QString& name,
                                            const QuickPartitionWidgets& widgets) {
    auto preset = quickPartitionOptions(widgets);
    preset[QStringLiteral("name")] = name.left(kQuickPartitionPresetNameMaxChars);
    if (quickPartitionCustomMode(widgets)) {
        preset[QStringLiteral("custom_size_mb")] = quickPartitionCustomSizeMegabytesJson(widgets);
    }
    return preset;
}

void saveQuickPartitionPreset(const QString& name, const QuickPartitionWidgets& widgets) {
    QJsonArray presets;
    const auto existing = quickPartitionPresetArray();
    for (const auto& value : existing) {
        const auto object = value.toObject();
        if (quickPartitionPresetName(object).compare(name, Qt::CaseInsensitive) != 0) {
            presets.append(object);
        }
    }
    presets.append(quickPartitionPresetFromWidgets(name, widgets));
    storeQuickPartitionPresetArray(presets);
}

void setComboCurrentData(QComboBox* combo, const QString& data) {
    if (!combo) {
        return;
    }
    const int index = combo->findData(data);
    if (index >= 0) {
        combo->setCurrentIndex(index);
    }
}

void setComboCurrentTextCaseInsensitive(QComboBox* combo, const QString& text) {
    if (!combo || text.isEmpty()) {
        return;
    }
    for (int index = 0; index < combo->count(); ++index) {
        if (combo->itemText(index).compare(text, Qt::CaseInsensitive) == 0) {
            combo->setCurrentIndex(index);
            return;
        }
    }
}

struct FormatPartitionWidgets {
    QComboBox* file_system{nullptr};
    QComboBox* allocation_unit{nullptr};
    QComboBox* swap_page_size{nullptr};
    QLineEdit* label{nullptr};
    QLineEdit* apfs_additional_volumes{nullptr};
    QCheckBox* full_format{nullptr};
    QCheckBox* raw_format_confirm{nullptr};
};

RawFormatKind rawFormatKindForFileSystem(const QString& fileSystem) {
    if (isExtFilesystem(fileSystem)) {
        return RawFormatKind::Ext;
    }
    if (isHfsFilesystem(fileSystem)) {
        return RawFormatKind::Hfs;
    }
    if (isLinuxSwapFilesystem(fileSystem)) {
        return RawFormatKind::Swap;
    }
    if (isApfsFilesystem(fileSystem)) {
        return RawFormatKind::Apfs;
    }
    return RawFormatKind::None;
}

QString rawFormatConfirmationAccessibleName(RawFormatKind kind) {
    switch (kind) {
    case RawFormatKind::Ext:
        return QObject::tr("Confirm ext filesystem format");
    case RawFormatKind::Hfs:
        return QObject::tr("Confirm HFS+ filesystem format");
    case RawFormatKind::Swap:
        return QObject::tr("Confirm Linux swap format");
    case RawFormatKind::Apfs:
        return QObject::tr("Confirm APFS format");
    case RawFormatKind::None:
        return QObject::tr("Confirm raw filesystem format");
    }
    return QObject::tr("Confirm raw filesystem format");
}

QString rawFormatConfirmationText(RawFormatKind kind) {
    switch (kind) {
    case RawFormatKind::Ext:
        return QObject::tr("I understand this will run bundled mke2fs against the raw partition.");
    case RawFormatKind::Hfs:
        return QObject::tr(
            "I understand this will stage a sparse HFS image with bundled newfs_hfs and write "
            "the resulting metadata to the raw partition.");
    case RawFormatKind::Swap:
        return QObject::tr(
            "I understand this will overwrite the first swap page with Linux "
            "SWAPSPACE2 metadata.");
    case RawFormatKind::Apfs:
        return QObject::tr(
            "I understand this will run the S.A.K. APFS writer helper and "
            "overwrite the selected raw partition with generated APFS metadata.");
    case RawFormatKind::None:
        return {};
    }
    return {};
}

QString formatPartitionPreviewText(const FormatPartitionWidgets& widgets, RawFormatKind kind) {
    switch (kind) {
    case RawFormatKind::Ext:
        return QObject::tr("Format as %1 with bundled mke2fs and label \"%2\".")
            .arg(widgets.file_system->currentText(), widgets.label->text());
    case RawFormatKind::Hfs:
        return QObject::tr(
                   "Format as %1 through sparse staging with bundled newfs_hfs and label \"%2\".")
            .arg(widgets.file_system->currentText(), widgets.label->text());
    case RawFormatKind::Swap:
        return QObject::tr("Format as Linux swap with page size %1 and label \"%2\".")
            .arg(widgets.swap_page_size->currentText(), widgets.label->text());
    case RawFormatKind::Apfs:
        return QObject::tr("Format as APFS with S.A.K. generated metadata and label \"%1\".")
            .arg(widgets.label->text());
    case RawFormatKind::None:
        return QObject::tr("Format as %1 with label \"%2\" and %3.")
            .arg(widgets.file_system->currentText(),
                 widgets.label->text(),
                 selectedAllocationUnitText(widgets.allocation_unit));
    }
    return {};
}

QComboBox* createLinuxSwapPageSizeSelector(QWidget* parent) {
    auto* combo = new QComboBox(parent);
    combo->addItems({QStringLiteral("4096"),
                     QStringLiteral("8192"),
                     QStringLiteral("16384"),
                     QStringLiteral("65536")});
    combo->setAccessibleName(QObject::tr("Linux swap page size"));
    return combo;
}

FormatPartitionWidgets addFormatPartitionControls(PartitionOperationDialog& dialog,
                                                  const PartitionInfoEx& partition) {
    FormatPartitionWidgets widgets;
    widgets.file_system = new QComboBox(&dialog);
    widgets.file_system->addItems({QStringLiteral("NTFS"),
                                   QStringLiteral("exFAT"),
                                   QStringLiteral("FAT32"),
                                   QStringLiteral("ext2"),
                                   QStringLiteral("ext3"),
                                   QStringLiteral("ext4"),
                                   QStringLiteral("HFS+"),
                                   QStringLiteral("HFSX"),
                                   QStringLiteral("APFS"),
                                   QStringLiteral("Linux swap")});
    widgets.file_system->setAccessibleName(QObject::tr("File system"));
    if (partition.volume && (isExtFilesystem(partition.volume->file_system) ||
                             isHfsFilesystem(partition.volume->file_system) ||
                             isApfsFilesystem(partition.volume->file_system) ||
                             isLinuxSwapFilesystem(partition.volume->file_system))) {
        setComboCurrentTextCaseInsensitive(widgets.file_system, partition.volume->file_system);
    }

    widgets.allocation_unit = createAllocationUnitSelector(&dialog);
    widgets.swap_page_size = createLinuxSwapPageSizeSelector(&dialog);
    widgets.label = new QLineEdit(partition.volume ? partition.volume->label : QString(), &dialog);
    widgets.label->setAccessibleName(QObject::tr("Volume label"));
    widgets.apfs_additional_volumes = new QLineEdit(&dialog);
    widgets.apfs_additional_volumes->setAccessibleName(QObject::tr("Additional APFS volume names"));
    widgets.apfs_additional_volumes->setPlaceholderText(
        QObject::tr("Comma-separated extra volume names (one space manager)"));
    widgets.full_format = new QCheckBox(QObject::tr("Full format"), &dialog);
    widgets.full_format->setAccessibleName(QObject::tr("Full format"));
    widgets.raw_format_confirm = new QCheckBox(
        QObject::tr("I understand this will run bundled mke2fs against the raw partition."),
        &dialog);
    widgets.raw_format_confirm->setAccessibleName(QObject::tr("Confirm ext filesystem format"));

    dialog.formLayout()->addRow(QObject::tr("File system:"), widgets.file_system);
    dialog.formLayout()->addRow(QObject::tr("Cluster size:"), widgets.allocation_unit);
    dialog.formLayout()->addRow(QObject::tr("Swap page size:"), widgets.swap_page_size);
    dialog.formLayout()->addRow(QObject::tr("Label:"), widgets.label);
    dialog.formLayout()->addRow(QObject::tr("Additional APFS volumes:"),
                                widgets.apfs_additional_volumes);
    dialog.formLayout()->addRow(QString(), widgets.full_format);
    dialog.formLayout()->addRow(QString(), widgets.raw_format_confirm);
    return widgets;
}

void updateFormatPartitionPreview(PartitionOperationDialog& dialog,
                                  const FormatPartitionWidgets& widgets) {
    const RawFormatKind rawKind = rawFormatKindForFileSystem(widgets.file_system->currentText());
    const bool rawSelected = rawKind != RawFormatKind::None;
    const bool swapSelected = rawKind == RawFormatKind::Swap;
    const bool apfsSelected = isApfsFilesystem(widgets.file_system->currentText());

    widgets.apfs_additional_volumes->setVisible(apfsSelected);
    widgets.apfs_additional_volumes->setEnabled(apfsSelected);
    widgets.allocation_unit->setEnabled(!rawSelected);
    widgets.full_format->setEnabled(!rawSelected);
    widgets.swap_page_size->setVisible(swapSelected);
    widgets.swap_page_size->setEnabled(swapSelected);
    widgets.raw_format_confirm->setVisible(rawSelected);
    widgets.raw_format_confirm->setAccessibleName(rawFormatConfirmationAccessibleName(rawKind));
    widgets.raw_format_confirm->setText(rawFormatConfirmationText(rawKind));
    dialog.setAcceptEnabled(!rawSelected || widgets.raw_format_confirm->isChecked());
    dialog.setPreviewText(formatPartitionPreviewText(widgets, rawKind));
}

void connectFormatPartitionControls(PartitionOperationDialog& dialog,
                                    const FormatPartitionWidgets& widgets) {
    auto updatePreview = [&dialog, widgets]() {
        updateFormatPartitionPreview(dialog, widgets);
    };
    QObject::connect(widgets.file_system, &QComboBox::currentTextChanged, &dialog, updatePreview);
    QObject::connect(
        widgets.allocation_unit, &QComboBox::currentTextChanged, &dialog, updatePreview);
    QObject::connect(
        widgets.swap_page_size, &QComboBox::currentTextChanged, &dialog, updatePreview);
    QObject::connect(widgets.label, &QLineEdit::textChanged, &dialog, updatePreview);
    QObject::connect(
        widgets.apfs_additional_volumes, &QLineEdit::textChanged, &dialog, updatePreview);
    QObject::connect(widgets.raw_format_confirm, &QCheckBox::toggled, &dialog, updatePreview);
}

// Parses the Format dialog's comma-separated "Additional APFS volumes" field into a
// trimmed, non-empty JSON array (empty when no extra volumes were entered).
QJsonArray apfsAdditionalVolumesArray(const QLineEdit* field) {
    QJsonArray volumes;
    if (!field) {
        return volumes;
    }
    const QStringList names = field->text().split(QLatin1Char(','), Qt::SkipEmptyParts);
    for (const QString& name : names) {
        const QString trimmed = name.trimmed();
        if (!trimmed.isEmpty()) {
            volumes.append(trimmed);
        }
    }
    return volumes;
}

QJsonObject formatPartitionPayload(const FormatPartitionWidgets& widgets,
                                   const PartitionInfoEx& partition) {
    QJsonObject payload;
    const QString fileSystem = widgets.file_system->currentText();
    payload[QStringLiteral("file_system")] = fileSystem;
    payload[QStringLiteral("allocation_unit_bytes")] =
        QString::number(selectedAllocationUnitBytes(widgets.allocation_unit));
    payload[QStringLiteral("label")] = widgets.label->text();
    payload[QStringLiteral("full_format")] = widgets.full_format->isChecked();
    if (isExtFilesystem(fileSystem) || isHfsFilesystem(fileSystem) ||
        isApfsFilesystem(fileSystem) || isLinuxSwapFilesystem(fileSystem)) {
        payload[QStringLiteral("non_native_file_system_tool")] = true;
        payload[QStringLiteral("target_path")] = nonNativeFilesystemWriteTargetPath(&partition);
        payload[QStringLiteral("target_wipe_confirmed")] = widgets.raw_format_confirm->isChecked();
    }
    if (isApfsFilesystem(fileSystem)) {
        const QJsonArray additionalVolumes =
            apfsAdditionalVolumesArray(widgets.apfs_additional_volumes);
        if (!additionalVolumes.isEmpty()) {
            payload[QStringLiteral("apfs_additional_volume_names")] = additionalVolumes;
        }
    }
    if (isLinuxSwapFilesystem(fileSystem)) {
        payload[QStringLiteral("linux_swap_page_size_bytes")] =
            widgets.swap_page_size->currentText();
    }
    return payload;
}

void setAllocationUnitBytes(QComboBox* combo, uint64_t bytes) {
    if (!combo) {
        return;
    }
    for (int index = 0; index < combo->count(); ++index) {
        if (combo->itemData(index).toULongLong() == bytes) {
            combo->setCurrentIndex(index);
            return;
        }
    }
}

ClusterSizeWidgets addClusterSizeControls(PartitionOperationDialog& dialog,
                                          const PartitionVolumeInfo& volume) {
    ClusterSizeWidgets widgets;
    widgets.file_system = new QComboBox(&dialog);
    widgets.file_system->addItems({QStringLiteral("NTFS"),
                                   QStringLiteral("exFAT"),
                                   QStringLiteral("FAT32"),
                                   QStringLiteral("ReFS")});
    widgets.file_system->setAccessibleName(QObject::tr("Cluster file system"));
    setComboCurrentTextCaseInsensitive(widgets.file_system, volume.file_system.trimmed());

    widgets.allocation_unit = createAllocationUnitSelector(&dialog);
    widgets.allocation_unit->setAccessibleName(QObject::tr("Target cluster size"));
    setAllocationUnitBytes(widgets.allocation_unit, kAllocationUnit4KbBytes);

    widgets.label = new QLineEdit(volume.label, &dialog);
    widgets.label->setAccessibleName(QObject::tr("Cluster volume label"));

    auto* backupRow = new QWidget(&dialog);
    auto* backupLayout = new QHBoxLayout(backupRow);
    backupLayout->setContentsMargins(
        ui::kMarginNone, ui::kMarginNone, ui::kMarginNone, ui::kMarginNone);
    backupLayout->setSpacing(ui::kSpacingSmall);
    widgets.backup_directory = new QLineEdit(&dialog);
    widgets.backup_directory->setAccessibleName(QObject::tr("Cluster backup directory"));
    widgets.browse_backup = new QPushButton(QObject::tr("Browse"), &dialog);
    widgets.browse_backup->setAccessibleName(QObject::tr("Browse cluster backup directory"));
    backupLayout->addWidget(widgets.backup_directory, 1);
    backupLayout->addWidget(widgets.browse_backup);

    widgets.confirmation = new QCheckBox(
        QObject::tr("I understand this will reformat the selected volume after backup."), &dialog);
    widgets.confirmation->setAccessibleName(
        QObject::tr("Confirm cluster-size reformat backup and restore"));

    dialog.formLayout()->addRow(QObject::tr("File system:"), widgets.file_system);
    dialog.formLayout()->addRow(QObject::tr("Target cluster size:"), widgets.allocation_unit);
    dialog.formLayout()->addRow(QObject::tr("Label:"), widgets.label);
    dialog.formLayout()->addRow(QObject::tr("Backup directory:"), backupRow);
    dialog.formLayout()->addRow(QString(), widgets.confirmation);
    return widgets;
}

bool clusterBackupIsOffTarget(const QString& backup, const PartitionVolumeInfo& volume) {
    const QString targetPrefix = volume.drive_letter.left(1).toUpper() + QStringLiteral(":");
    return !backup.isEmpty() && !backup.startsWith(targetPrefix, Qt::CaseInsensitive);
}

void updateClusterSizePreview(PartitionOperationDialog& dialog,
                              const ClusterSizeWidgets& widgets,
                              const PartitionVolumeInfo& volume) {
    const QString backup = QDir::toNativeSeparators(widgets.backup_directory->text().trimmed());
    const bool clusterSelected = selectedAllocationUnitBytes(widgets.allocation_unit) !=
                                 kAllocationUnitDefaultBytes;
    dialog.setAcceptEnabled(clusterSelected && clusterBackupIsOffTarget(backup, volume) &&
                            widgets.confirmation->isChecked());
    dialog.setPreviewText(
        QObject::tr("Back up %1: to %2. Reformat as %3 with %4. Restore files and "
                    "compare SHA-256 manifests before reporting success.")
            .arg(volume.drive_letter.left(1).toUpper(),
                 backup.isEmpty() ? QObject::tr("(choose backup directory)") : backup,
                 widgets.file_system->currentText(),
                 selectedAllocationUnitText(widgets.allocation_unit)));
}

void connectClusterSizeControls(PartitionOperationDialog& dialog,
                                const ClusterSizeWidgets& widgets,
                                const PartitionVolumeInfo& volume) {
    const ClusterSizeWidgets& controls = widgets;
    const PartitionVolumeInfo& volumeCopy = volume;
    auto updatePreview = [&dialog, controls, volumeCopy]() {
        updateClusterSizePreview(dialog, controls, volumeCopy);
    };
    QObject::connect(controls.file_system, &QComboBox::currentTextChanged, &dialog, updatePreview);
    QObject::connect(
        controls.allocation_unit, &QComboBox::currentTextChanged, &dialog, updatePreview);
    QObject::connect(controls.label, &QLineEdit::textChanged, &dialog, updatePreview);
    QObject::connect(controls.backup_directory, &QLineEdit::textChanged, &dialog, updatePreview);
    QObject::connect(controls.confirmation, &QCheckBox::toggled, &dialog, updatePreview);
    QObject::connect(controls.browse_backup, &QPushButton::clicked, &dialog, [&dialog, controls]() {
        const QString path =
            QFileDialog::getExistingDirectory(&dialog,
                                              QObject::tr("Cluster Size Backup Directory"),
                                              controls.backup_directory->text());
        if (!path.isEmpty()) {
            controls.backup_directory->setText(QDir::toNativeSeparators(path));
        }
    });
}

QJsonObject clusterSizePayload(const ClusterSizeWidgets& widgets,
                               const PartitionVolumeInfo& volume) {
    QJsonObject payload;
    payload[QStringLiteral("drive_letter")] = volume.drive_letter.left(1).toUpper();
    payload[QStringLiteral("file_system")] = widgets.file_system->currentText();
    payload[QStringLiteral("allocation_unit_bytes")] =
        QString::number(selectedAllocationUnitBytes(widgets.allocation_unit));
    payload[QStringLiteral("label")] = widgets.label->text();
    payload[QStringLiteral("backup_directory")] =
        QDir::toNativeSeparators(widgets.backup_directory->text().trimmed());
    payload[QStringLiteral("target_wipe_confirmed")] = widgets.confirmation->isChecked();
    return payload;
}

uint64_t selectedAllocateBytes(const AllocateFreeSpaceWidgets& widgets) {
    const uint64_t requested = static_cast<uint64_t>(widgets.amount_mb->value()) * kMegabyteBytes;
    return std::min(requested, widgets.max_allocate_bytes);
}

bool backupIsOffAllocatedVolumes(const QString& backup,
                                 const PartitionInfoEx& target,
                                 const PartitionInfoEx& donor) {
    if (backup.isEmpty()) {
        return false;
    }
    const QString native = QDir::toNativeSeparators(backup);
    if (target.volume &&
        native.startsWith(target.volume->drive_letter.left(1).toUpper() + QStringLiteral(":"),
                          Qt::CaseInsensitive)) {
        return false;
    }
    return !donor.volume ||
           !native.startsWith(donor.volume->drive_letter.left(1).toUpper() + QStringLiteral(":"),
                              Qt::CaseInsensitive);
}

bool backupIsOffPartitionVolume(const QString& backup, const PartitionInfoEx& partition) {
    if (backup.isEmpty()) {
        return false;
    }
    if (!partition.volume || partition.volume->drive_letter.isEmpty()) {
        return false;
    }
    return !QDir::toNativeSeparators(backup).startsWith(
        partition.volume->drive_letter.left(1).toUpper() + QStringLiteral(":"),
        Qt::CaseInsensitive);
}

BackupRestoreWidgets addBackupRestoreControls(PartitionOperationDialog& dialog,
                                              const QString& backupAccessibleName,
                                              const QString& browseAccessibleName,
                                              const QString& confirmationText,
                                              const QString& confirmationAccessibleName) {
    BackupRestoreWidgets widgets;
    auto* backupRow = new QWidget(&dialog);
    auto* backupLayout = new QHBoxLayout(backupRow);
    backupLayout->setContentsMargins(
        ui::kMarginNone, ui::kMarginNone, ui::kMarginNone, ui::kMarginNone);
    backupLayout->setSpacing(ui::kSpacingSmall);
    widgets.backup_directory = new QLineEdit(&dialog);
    widgets.backup_directory->setAccessibleName(backupAccessibleName);
    widgets.browse_backup = new QPushButton(QObject::tr("Browse"), &dialog);
    widgets.browse_backup->setAccessibleName(browseAccessibleName);
    backupLayout->addWidget(widgets.backup_directory, 1);
    backupLayout->addWidget(widgets.browse_backup);
    widgets.confirmation = new QCheckBox(confirmationText, &dialog);
    widgets.confirmation->setAccessibleName(confirmationAccessibleName);
    widgets.status = new QLabel(&dialog);
    widgets.status->setWordWrap(true);
    widgets.status->setAccessibleName(QObject::tr("Backup restore status"));
    dialog.formLayout()->addRow(QObject::tr("Backup directory:"), backupRow);
    dialog.formLayout()->addRow(QObject::tr("Status:"), widgets.status);
    dialog.formLayout()->addRow(QString(), widgets.confirmation);
    return widgets;
}

void connectBackupBrowse(QDialog& dialog,
                         const BackupRestoreWidgets& widgets,
                         const QString& title) {
    QObject::connect(
        widgets.browse_backup, &QPushButton::clicked, &dialog, [&dialog, widgets, title]() {
            const QString path =
                QFileDialog::getExistingDirectory(&dialog, title, widgets.backup_directory->text());
            if (!path.isEmpty()) {
                widgets.backup_directory->setText(QDir::toNativeSeparators(path));
            }
        });
}

QString freeSpaceChoiceLabel(const PartitionInfoEx& partition, bool movePartitionStart) {
    const QString action = movePartitionStart ? QObject::tr("Move/extend") : QObject::tr("Extend");
    if (partition.volume && !partition.volume->drive_letter.isEmpty()) {
        return QObject::tr("%1 Partition %2 (%3:)")
            .arg(action)
            .arg(partition.partition_number)
            .arg(partition.volume->drive_letter.left(1).toUpper());
    }
    return QObject::tr("%1 Partition %2").arg(action).arg(partition.partition_number);
}

QVector<FreeSpaceAllocationChoice> freeSpaceAllocationChoices(const PartitionInfoEx* previous,
                                                              const PartitionInfoEx* next) {
    QVector<FreeSpaceAllocationChoice> choices;
    if (previous) {
        choices.append({previous->partition_number, freeSpaceChoiceLabel(*previous, false), false});
    }
    if (next) {
        choices.append({next->partition_number, freeSpaceChoiceLabel(*next, true), true});
    }
    return choices;
}

FreeSpaceAllocationWidgets addFreeSpaceAllocationControls(
    PartitionOperationDialog& dialog,
    const QVector<FreeSpaceAllocationChoice>& choices,
    uint64_t freeBytes) {
    FreeSpaceAllocationWidgets widgets;
    widgets.choices = choices;
    widgets.free_bytes = freeBytes;

    widgets.target_partition = new QComboBox(&dialog);
    widgets.target_partition->setAccessibleName(
        QObject::tr("Allocate free space target partition"));
    for (const auto& choice : choices) {
        widgets.target_partition->addItem(choice.label);
    }
    dialog.formLayout()->addRow(QObject::tr("Target:"), widgets.target_partition);

    const int maxMb = std::max(
        1, static_cast<int>(std::min<uint64_t>(kMaxSizeInputMb, freeBytes / kMegabyteBytes)));
    widgets.amount_mb = new QSpinBox(&dialog);
    widgets.amount_mb->setRange(1, maxMb);
    widgets.amount_mb->setValue(maxMb);
    widgets.amount_mb->setSuffix(QObject::tr(" MB"));
    widgets.amount_mb->setAccessibleName(QObject::tr("Allocate free space amount"));
    widgets.amount_handle = createSizeHandle(&dialog,
                                             QObject::tr("Allocate free space amount handle"),
                                             widgets.amount_mb->minimum(),
                                             widgets.amount_mb->maximum(),
                                             widgets.amount_mb->value());
    dialog.formLayout()->addRow(QObject::tr("Amount:"), widgets.amount_mb);
    dialog.formLayout()->addRow(QObject::tr("Amount handle:"), widgets.amount_handle);

    widgets.backup =
        addBackupRestoreControls(dialog,
                                 QObject::tr("Allocate free space to backup directory"),
                                 QObject::tr("Browse allocate free space to backup directory"),
                                 QObject::tr("I understand this will back up, delete, recreate, "
                                             "restore, and verify the moved partition."),
                                 QObject::tr("Confirm allocate free space to backup and restore"));
    widgets.preview = new OperationSizePreviewWidget(&dialog);
    dialog.addVisualPreview(widgets.preview);
    return widgets;
}

uint64_t selectedFreeSpaceAllocationBytes(const FreeSpaceAllocationWidgets& widgets) {
    const uint64_t requested = static_cast<uint64_t>(widgets.amount_mb->value()) * kMegabyteBytes;
    return std::min(requested, widgets.free_bytes);
}

FreeSpaceAllocationChoice selectedFreeSpaceChoice(const FreeSpaceAllocationWidgets& widgets) {
    const int maxIndex = std::max(0, static_cast<int>(widgets.choices.size()) - 1);
    const int index = std::clamp(widgets.target_partition->currentIndex(), 0, maxIndex);
    return widgets.choices.at(index);
}

const PartitionInfoEx* selectedFreeSpacePartition(const FreeSpaceAllocationWidgets& widgets,
                                                  const PartitionDiskInfo& disk) {
    return PartitionSafetyValidator::findPartition(
        disk, selectedFreeSpaceChoice(widgets).partition_number);
}

void updateFreeSpaceBackupControls(const BackupRestoreWidgets& backup, bool moveMode) {
    backup.backup_directory->setEnabled(moveMode);
    backup.browse_backup->setEnabled(moveMode);
    backup.confirmation->setEnabled(moveMode);
    backup.status->setText(
        moveMode
            ? QObject::tr("Moving a partition into preceding free space uses off-volume backup, "
                          "recreate, restore, SHA-256 compare, and repair scan.")
            : QObject::tr("Extending into following free space uses Windows online resize; no "
                          "backup/recreate step is needed for this path."));
}

void updateFreeSpaceAllocationPreview(PartitionOperationDialog& dialog,
                                      const FreeSpaceAllocationWidgets& widgets,
                                      const PartitionDiskInfo& disk) {
    const auto choice = selectedFreeSpaceChoice(widgets);
    const auto* partition = selectedFreeSpacePartition(widgets, disk);
    const uint64_t amountBytes = selectedFreeSpaceAllocationBytes(widgets);
    if (!partition || amountBytes == 0) {
        dialog.setAcceptEnabled(false);
        return;
    }

    const QString backupPath =
        QDir::toNativeSeparators(widgets.backup.backup_directory->text().trimmed());
    const bool moveMode = choice.move_partition_start;
    const bool canQueue = !moveMode || (partition->volume &&
                                        backupIsOffPartitionVolume(backupPath, *partition) &&
                                        widgets.backup.confirmation->isChecked());
    updateFreeSpaceBackupControls(widgets.backup, moveMode);
    widgets.preview->setRows(
        {{QObject::tr("Target"),
          saturatingAdd(partition->size_bytes, amountBytes),
          saturatingAdd(partition->size_bytes, amountBytes),
          QColor(QString::fromLatin1(ui::kColorSuccess)),
          QObject::tr("%1 after allocation")
              .arg(formatPartitionBytes(saturatingAdd(partition->size_bytes, amountBytes)))},
         {QObject::tr("Free"),
          widgets.free_bytes,
          widgets.free_bytes - amountBytes,
          QApplication::palette().color(QPalette::Midlight),
          QObject::tr("%1 remains").arg(formatPartitionBytes(widgets.free_bytes - amountBytes))}});
    dialog.setAcceptEnabled(canQueue);
    dialog.setPreviewText(
        moveMode ? QObject::tr("Back up partition %1, recreate it %2 earlier and %3 larger, "
                               "restore files, and verify hashes.")
                       .arg(partition->partition_number)
                       .arg(formatPartitionBytes(amountBytes), formatPartitionBytes(amountBytes))
                 : QObject::tr("Extend partition %1 by %2 into selected adjacent free space.")
                       .arg(partition->partition_number)
                       .arg(formatPartitionBytes(amountBytes)));
}

PartitionTarget partitionTargetFromPartition(const PartitionInfoEx& partition) {
    PartitionTarget target;
    target.kind = PartitionTargetKind::Partition;
    target.disk_number = partition.disk_number;
    target.partition_number = partition.partition_number;
    target.partition_guid = partition.partition_guid;
    target.offset_bytes = partition.offset_bytes;
    target.size_bytes = partition.size_bytes;
    if (partition.volume) {
        target.volume_guid = partition.volume->volume_guid;
        target.drive_letter = partition.volume->drive_letter;
    }
    return target;
}

QJsonObject freeSpaceMovePayload(const FreeSpaceAllocationWidgets& widgets,
                                 const PartitionInfoEx& partition,
                                 uint64_t amountBytes) {
    QJsonObject payload;
    payload[QStringLiteral("target_offset_bytes")] =
        QString::number(partition.offset_bytes - amountBytes);
    payload[QStringLiteral("target_size_bytes")] =
        QString::number(saturatingAdd(partition.size_bytes, amountBytes));
    if (partition.volume) {
        payload[QStringLiteral("drive_letter")] = partition.volume->drive_letter.left(1).toUpper();
        payload[QStringLiteral("file_system")] = partition.volume->file_system;
        payload[QStringLiteral("label")] = partition.volume->label;
    }
    payload[QStringLiteral("backup_directory")] =
        QDir::toNativeSeparators(widgets.backup.backup_directory->text().trimmed());
    payload[QStringLiteral("target_wipe_confirmed")] = widgets.backup.confirmation->isChecked();
    return payload;
}

QJsonObject freeSpaceResizePayload(const PartitionInfoEx& partition,
                                   uint64_t freeBytes,
                                   uint64_t amountBytes) {
    QJsonObject payload;
    payload[QStringLiteral("target_size_bytes")] =
        QString::number(saturatingAdd(partition.size_bytes, amountBytes));
    payload[QStringLiteral("adjacent_free_bytes")] = QString::number(freeBytes);
    return payload;
}

AllocateFreeSpaceWidgets addAllocateFreeSpaceControls(PartitionOperationDialog& dialog,
                                                      const PartitionInfoEx& target,
                                                      const PartitionInfoEx& donor,
                                                      uint64_t max_allocate_bytes) {
    AllocateFreeSpaceWidgets widgets;
    widgets.target_bytes = target.size_bytes;
    widgets.donor_bytes = donor.size_bytes;
    widgets.max_allocate_bytes = max_allocate_bytes;

    const int maxMb = std::max(
        1,
        static_cast<int>(std::min<uint64_t>(kMaxSizeInputMb, max_allocate_bytes / kMegabyteBytes)));
    widgets.amount_mb = new QSpinBox(&dialog);
    widgets.amount_mb->setRange(1, maxMb);
    widgets.amount_mb->setValue(std::min(kDefaultCreateSizeMb, maxMb));
    widgets.amount_mb->setSuffix(QObject::tr(" MB"));
    widgets.amount_mb->setAccessibleName(QObject::tr("Allocate free space amount"));
    widgets.amount_handle = createSizeHandle(&dialog,
                                             QObject::tr("Allocate free space amount handle"),
                                             widgets.amount_mb->minimum(),
                                             widgets.amount_mb->maximum(),
                                             widgets.amount_mb->value());

    auto* backupRow = new QWidget(&dialog);
    auto* backupLayout = new QHBoxLayout(backupRow);
    backupLayout->setContentsMargins(
        ui::kMarginNone, ui::kMarginNone, ui::kMarginNone, ui::kMarginNone);
    backupLayout->setSpacing(ui::kSpacingSmall);
    widgets.backup_directory = new QLineEdit(&dialog);
    widgets.backup_directory->setAccessibleName(QObject::tr("Allocate donor backup directory"));
    widgets.browse_backup = new QPushButton(QObject::tr("Browse"), &dialog);
    widgets.browse_backup->setAccessibleName(QObject::tr("Browse allocate donor backup directory"));
    backupLayout->addWidget(widgets.backup_directory, 1);
    backupLayout->addWidget(widgets.browse_backup);

    widgets.confirmation =
        new QCheckBox(QObject::tr("I understand the donor volume will be backed up, deleted, "
                                  "recreated, restored, and verified."),
                      &dialog);
    widgets.confirmation->setAccessibleName(
        QObject::tr("Confirm allocate-free-space backup and restore"));
    widgets.donor_status = new QLabel(&dialog);
    widgets.donor_status->setWordWrap(true);
    widgets.donor_status->setAccessibleName(QObject::tr("Allocate free space donor status"));

    dialog.formLayout()->addRow(QObject::tr("Amount:"), widgets.amount_mb);
    dialog.formLayout()->addRow(QObject::tr("Amount handle:"), widgets.amount_handle);
    dialog.formLayout()->addRow(QObject::tr("Backup directory:"), backupRow);
    dialog.formLayout()->addRow(QObject::tr("Donor:"), widgets.donor_status);
    dialog.formLayout()->addRow(QString(), widgets.confirmation);
    widgets.size_preview = new OperationSizePreviewWidget(&dialog);
    dialog.addVisualPreview(widgets.size_preview);
    return widgets;
}

void updateAllocateFreeSpacePreview(PartitionOperationDialog& dialog,
                                    const AllocateFreeSpaceWidgets& widgets,
                                    const PartitionInfoEx& target,
                                    const PartitionInfoEx& donor) {
    const uint64_t amountBytes = selectedAllocateBytes(widgets);
    const QString backup = QDir::toNativeSeparators(widgets.backup_directory->text().trimmed());
    const uint64_t targetAfter = saturatingAdd(widgets.target_bytes, amountBytes);
    const uint64_t donorAfter =
        widgets.donor_bytes > amountBytes ? widgets.donor_bytes - amountBytes : 0;
    const uint64_t scaleBytes = std::max(targetAfter, widgets.donor_bytes);
    widgets.size_preview->setRows(
        {{QObject::tr("Target"),
          scaleBytes,
          targetAfter,
          QColor(QString::fromLatin1(ui::kColorSuccess)),
          QObject::tr("%1 after allocation").arg(formatPartitionBytes(targetAfter))},
         {QObject::tr("Donor"),
          scaleBytes,
          donorAfter,
          QColor(QString::fromLatin1(ui::kColorWarning)),
          QObject::tr("%1 after restore").arg(formatPartitionBytes(donorAfter))}});
    const QString donorLetter = donor.volume ? donor.volume->drive_letter.left(1).toUpper()
                                             : QStringLiteral("?");
    widgets.donor_status->setText(
        QObject::tr("Disk %1 Partition %2 (%3:) can donate up to %4 while retaining existing "
                    "data plus reserve.")
            .arg(donor.disk_number)
            .arg(donor.partition_number)
            .arg(donorLetter, formatPartitionBytes(widgets.max_allocate_bytes)));
    {
        const QSignalBlocker handleBlocker(widgets.amount_handle);
        widgets.amount_handle->setRange(widgets.amount_mb->minimum(), widgets.amount_mb->maximum());
        widgets.amount_handle->setValue(widgets.amount_mb->value());
    }
    const bool canQueue = amountBytes > 0 && backupIsOffAllocatedVolumes(backup, target, donor) &&
                          widgets.confirmation->isChecked();
    dialog.setAcceptEnabled(canQueue);
    dialog.setPreviewText(
        QObject::tr("Back up donor %1:, delete donor, extend selected partition by %2, recreate "
                    "donor, restore files, and compare SHA-256 manifests. Backup: %3")
            .arg(donorLetter,
                 formatPartitionBytes(amountBytes),
                 backup.isEmpty() ? QObject::tr("(choose backup directory)") : backup));
}

void connectAllocateFreeSpaceControls(PartitionOperationDialog& dialog,
                                      const AllocateFreeSpaceWidgets& widgets,
                                      const PartitionInfoEx& target,
                                      const PartitionInfoEx& donor) {
    const AllocateFreeSpaceWidgets& controls = widgets;
    const PartitionInfoEx& targetCopy = target;
    const PartitionInfoEx& donorCopy = donor;
    auto updatePreview = [&dialog, controls, targetCopy, donorCopy]() {
        updateAllocateFreeSpacePreview(dialog, controls, targetCopy, donorCopy);
    };
    QObject::connect(controls.amount_mb, &QSpinBox::valueChanged, &dialog, updatePreview);
    QObject::connect(controls.amount_handle,
                     &QSlider::valueChanged,
                     &dialog,
                     [controls](int value) { controls.amount_mb->setValue(value); });
    QObject::connect(controls.backup_directory, &QLineEdit::textChanged, &dialog, updatePreview);
    QObject::connect(controls.confirmation, &QCheckBox::toggled, &dialog, updatePreview);
    QObject::connect(controls.browse_backup, &QPushButton::clicked, &dialog, [&dialog, controls]() {
        const QString path =
            QFileDialog::getExistingDirectory(&dialog,
                                              QObject::tr("Allocate Free Space Backup Directory"),
                                              controls.backup_directory->text());
        if (!path.isEmpty()) {
            controls.backup_directory->setText(QDir::toNativeSeparators(path));
        }
    });
}

QJsonObject allocateFreeSpacePayload(const AllocateFreeSpaceWidgets& widgets,
                                     const PartitionInfoEx& donor) {
    QJsonObject payload;
    payload[QStringLiteral("source_partition_number")] = QString::number(donor.partition_number);
    payload[QStringLiteral("source_size_bytes")] = QString::number(donor.size_bytes);
    payload[QStringLiteral("bytes_to_allocate")] = QString::number(selectedAllocateBytes(widgets));
    if (donor.volume) {
        payload[QStringLiteral("source_drive_letter")] =
            donor.volume->drive_letter.left(1).toUpper();
        payload[QStringLiteral("source_file_system")] = donor.volume->file_system;
        payload[QStringLiteral("source_label")] = donor.volume->label;
    }
    payload[QStringLiteral("backup_directory")] =
        QDir::toNativeSeparators(widgets.backup_directory->text().trimmed());
    payload[QStringLiteral("target_wipe_confirmed")] = widgets.confirmation->isChecked();
    return payload;
}

QString selectedQuickPartitionPresetName(const QuickPartitionWidgets& widgets) {
    return widgets.preset_selector ? widgets.preset_selector->currentData().toString() : QString();
}

void refreshQuickPartitionPresetSelector(const QuickPartitionWidgets& widgets,
                                         const QString& selectName = QString()) {
    if (!widgets.preset_selector) {
        return;
    }
    const QSignalBlocker blocker(widgets.preset_selector);
    widgets.preset_selector->clear();
    widgets.preset_selector->addItem(QObject::tr("No saved preset"), QString());
    const auto presets = quickPartitionPresetArray();
    for (const auto& value : presets) {
        const QString name = quickPartitionPresetName(value.toObject());
        if (!name.isEmpty()) {
            widgets.preset_selector->addItem(name, name);
        }
    }
    if (!selectName.isEmpty()) {
        const int index = widgets.preset_selector->findData(selectName);
        if (index >= 0) {
            widgets.preset_selector->setCurrentIndex(index);
        }
    }
    const bool hasPreset = !widgets.preset_selector->currentData().toString().isEmpty();
    if (widgets.preset_load) {
        widgets.preset_load->setEnabled(hasPreset);
    }
    if (widgets.preset_delete) {
        widgets.preset_delete->setEnabled(hasPreset);
    }
}

PartitionTarget quickPartitionCreateTarget(const PartitionDiskInfo& disk,
                                           uint64_t offsetBytes,
                                           uint64_t sizeBytes) {
    PartitionTarget target;
    target.kind = PartitionTargetKind::Unallocated;
    target.disk_number = disk.disk_number;
    target.offset_bytes = offsetBytes;
    target.size_bytes = sizeBytes;
    return target;
}

QJsonObject quickPartitionOptions(const QuickPartitionWidgets& widgets) {
    QJsonObject options;
    options[QStringLiteral("partition_count")] = widgets.partition_count->value();
    options[QStringLiteral("partition_scheme")] = widgets.partition_style->currentData().toString();
    options[QStringLiteral("size_mode")] = widgets.size_mode->currentData().toString();
    options[QStringLiteral("file_system")] = widgets.file_system->currentText();
    options[QStringLiteral("allocation_unit_bytes")] =
        QString::number(selectedAllocationUnitBytes(widgets.allocation_unit));
    options[QStringLiteral("label_prefix")] = widgets.label_prefix->text();
    if (quickPartitionCustomMode(widgets)) {
        options[QStringLiteral("custom_labels")] = quickPartitionLabelsJson(widgets);
        options[QStringLiteral("custom_size_bytes")] =
            quickPartitionSizeJson(quickPartitionSizesFromWidgets(widgets, 0));
    }
    options[QStringLiteral("full_format")] = widgets.full_format->isChecked();
    return options;
}

QJsonObject quickPartitionCreatePayload(const QJsonObject& options,
                                        uint64_t sizeBytes,
                                        int partitionIndex) {
    QJsonObject payload;
    payload[QStringLiteral("size_bytes")] = QString::number(sizeBytes);
    payload[QStringLiteral("relative_offset_bytes")] = QStringLiteral("0");
    payload[QStringLiteral("file_system")] =
        options.value(QStringLiteral("file_system")).toString();
    payload[QStringLiteral("allocation_unit_bytes")] =
        options.value(QStringLiteral("allocation_unit_bytes")).toString();
    payload[QStringLiteral("label")] = quickPartitionLabelFromOptions(options, partitionIndex);
    payload[QStringLiteral("full_format")] =
        options.value(QStringLiteral("full_format")).toBool(false);
    if (options.value(QStringLiteral("partition_scheme")).toString() == QStringLiteral("GPT")) {
        payload[QStringLiteral("gpt_type")] = gptBasicDataType();
    } else if (options.value(QStringLiteral("partition_scheme")).toString() ==
               QStringLiteral("MBR")) {
        payload[QStringLiteral("mbr_type")] = payload.value(QStringLiteral("file_system"))
                                                          .toString()
                                                          .compare(QStringLiteral("FAT32"),
                                                                   Qt::CaseInsensitive) == 0
                                                  ? QStringLiteral("FAT32")
                                                  : QStringLiteral("IFS");
    }
    return payload;
}

bool diskNeedsInitializeForQuickPartition(const PartitionDiskInfo& disk) {
    const QString style = disk.partition_style.toUpper();
    return style != QStringLiteral("GPT") && style != QStringLiteral("MBR");
}

QString selectedQuickPartitionScheme(const QuickPartitionWidgets& widgets,
                                     const PartitionDiskInfo& disk) {
    const QString selected =
        widgets.partition_style ? widgets.partition_style->currentData().toString() : QString();
    if (!selected.isEmpty()) {
        return selected;
    }
    return disk.partition_style.toUpper();
}

QString quickPartitionBlocker(const PartitionDiskInfo* disk) {
    if (!disk) {
        return QObject::tr("Select a disk before quick partition.");
    }
    if (disk->is_system || disk->is_read_only || disk->is_dynamic || disk->is_storage_spaces) {
        return QObject::tr(
            "Quick Partition is blocked for system, read-only, dynamic, or Storage Spaces disks.");
    }
    const auto minimumBytes = static_cast<uint64_t>(kQuickPartitionMinimumSizeMb) * kMegabyteBytes;
    return quickPartitionUsableBytes(*disk) < minimumBytes
               ? QObject::tr("Disk is too small for the minimum quick partition size.")
               : QString();
}

void updateQuickPartitionCountRange(const QuickPartitionWidgets& widgets,
                                    const PartitionDiskInfo& disk) {
    if (!widgets.partition_count) {
        return;
    }
    const int maxPartitions =
        quickPartitionMaxCountForScheme(disk, selectedQuickPartitionScheme(widgets, disk));
    const QSignalBlocker blocker(widgets.partition_count);
    widgets.partition_count->setRange(1, std::max(1, maxPartitions));
    widgets.partition_count->setValue(
        std::min(widgets.partition_count->value(), widgets.partition_count->maximum()));
}

void rebuildQuickPartitionTable(const QuickPartitionWidgets& widgets,
                                const PartitionDiskInfo& disk) {
    if (!widgets.partition_table || !widgets.partition_count) {
        return;
    }
    const QSignalBlocker blocker(widgets.partition_table);
    const int count = widgets.partition_count->value();
    const uint64_t usableBytes = quickPartitionUsableBytes(disk);
    const QVector<uint64_t> equalSizes = quickPartitionEqualSizes(usableBytes, count);
    const int usableMb =
        static_cast<int>(std::min<uint64_t>(usableBytes / kMegabyteBytes, kMaxSizeInputMb));
    widgets.partition_table->setRowCount(count);
    for (int row = 0; row < count; ++row) {
        auto* label = new QLineEdit(QStringLiteral("Data%1").arg(row + 1), widgets.partition_table);
        label->setAccessibleName(QObject::tr("Quick partition %1 label").arg(row + 1));
        widgets.partition_table->setCellWidget(row, kQuickPartitionLabelColumn, label);

        auto* size = new QSpinBox(widgets.partition_table);
        size->setRange(kQuickPartitionMinimumSizeMb,
                       std::max(kQuickPartitionMinimumSizeMb, usableMb));
        size->setSingleStep(kSizeHandleSingleStepMb);
        size->setSuffix(QObject::tr(" MB"));
        size->setAccessibleName(QObject::tr("Quick partition %1 size").arg(row + 1));
        const int defaultSizeMb =
            static_cast<int>(std::min<uint64_t>(equalSizes.value(row) / kMegabyteBytes, usableMb));
        size->setValue(std::max(kQuickPartitionMinimumSizeMb, defaultSizeMb));
        widgets.partition_table->setCellWidget(row, kQuickPartitionSizeColumn, size);
    }
    widgets.partition_table->resizeColumnsToContents();
}

void updateQuickPartitionTableMode(const QuickPartitionWidgets& widgets) {
    const bool custom = quickPartitionCustomMode(widgets);
    if (!widgets.partition_table) {
        return;
    }
    for (int row = 0; row < widgets.partition_table->rowCount(); ++row) {
        if (auto* label = quickPartitionLabelEditor(widgets, row)) {
            label->setEnabled(custom);
        }
        if (auto* size = quickPartitionSizeEditor(widgets, row)) {
            size->setEnabled(custom);
        }
    }
}

void applyQuickPartitionPresetBasics(const QuickPartitionWidgets& widgets,
                                     const PartitionDiskInfo& disk,
                                     const QJsonObject& preset) {
    const QString scheme = preset.value(QStringLiteral("partition_scheme")).toString();
    if (widgets.partition_style && widgets.partition_style->isEnabled()) {
        setComboCurrentData(widgets.partition_style, scheme);
    }
    updateQuickPartitionCountRange(widgets, disk);
    if (widgets.partition_count) {
        const int requestedCount =
            preset.value(QStringLiteral("partition_count")).toInt(widgets.partition_count->value());
        widgets.partition_count->setValue(std::clamp(requestedCount,
                                                     widgets.partition_count->minimum(),
                                                     widgets.partition_count->maximum()));
    }
    setComboCurrentData(widgets.size_mode, preset.value(QStringLiteral("size_mode")).toString());
    setComboCurrentTextCaseInsensitive(widgets.file_system,
                                       preset.value(QStringLiteral("file_system")).toString());
    setAllocationUnitBytes(
        widgets.allocation_unit,
        preset.value(QStringLiteral("allocation_unit_bytes")).toString().toULongLong());
    if (widgets.label_prefix) {
        widgets.label_prefix->setText(
            preset.value(QStringLiteral("label_prefix")).toString(QStringLiteral("Data")));
    }
    if (widgets.full_format) {
        widgets.full_format->setChecked(preset.value(QStringLiteral("full_format")).toBool(false));
    }
}

int quickPartitionPresetSizeMegabytes(const QJsonArray& sizeMb,
                                      const QJsonArray& sizeBytes,
                                      int row) {
    const int requestedMb = row < sizeMb.size() ? sizeMb.at(row).toInt() : 0;
    if (requestedMb > 0 || row >= sizeBytes.size()) {
        return requestedMb;
    }
    return static_cast<int>(sizeBytes.at(row).toString().toULongLong() / kMegabyteBytes);
}

void applyQuickPartitionPresetLabels(const QuickPartitionWidgets& widgets,
                                     const QJsonArray& labels) {
    for (int row = 0; row < widgets.partition_table->rowCount(); ++row) {
        auto* label = quickPartitionLabelEditor(widgets, row);
        if (label && row < labels.size()) {
            label->setText(labels.at(row).toString());
        }
    }
}

void applyQuickPartitionPresetSizes(const QuickPartitionWidgets& widgets,
                                    const QJsonArray& sizeMb,
                                    const QJsonArray& sizeBytes) {
    for (int row = 0; row < widgets.partition_table->rowCount(); ++row) {
        auto* size = quickPartitionSizeEditor(widgets, row);
        const int requestedMb = quickPartitionPresetSizeMegabytes(sizeMb, sizeBytes, row);
        if (size && requestedMb > 0) {
            size->setValue(std::clamp(requestedMb, size->minimum(), size->maximum()));
        }
    }
}

void applyQuickPartitionPresetCustomRows(const QuickPartitionWidgets& widgets,
                                         const QJsonObject& preset) {
    if (!quickPartitionCustomMode(widgets)) {
        return;
    }
    applyQuickPartitionPresetLabels(widgets,
                                    preset.value(QStringLiteral("custom_labels")).toArray());
    applyQuickPartitionPresetSizes(widgets,
                                   preset.value(QStringLiteral("custom_size_mb")).toArray(),
                                   preset.value(QStringLiteral("custom_size_bytes")).toArray());
}

void applyQuickPartitionPreset(const QuickPartitionWidgets& widgets,
                               const PartitionDiskInfo& disk,
                               const QJsonObject& preset) {
    applyQuickPartitionPresetBasics(widgets, disk, preset);
    rebuildQuickPartitionTable(widgets, disk);
    applyQuickPartitionPresetCustomRows(widgets, preset);
    updateQuickPartitionTableMode(widgets);
}

QWidget* createQuickPartitionPresetRow(PartitionOperationDialog& dialog,
                                       QuickPartitionWidgets& widgets) {
    widgets.preset_selector = new QComboBox(&dialog);
    widgets.preset_selector->setAccessibleName(QObject::tr("Quick partition preset"));
    widgets.preset_load = new QPushButton(QObject::tr("Load"), &dialog);
    widgets.preset_load->setAccessibleName(QObject::tr("Load quick partition preset"));
    widgets.preset_save = new QPushButton(QObject::tr("Save"), &dialog);
    widgets.preset_save->setAccessibleName(QObject::tr("Save quick partition preset"));
    widgets.preset_delete = new QPushButton(QObject::tr("Delete"), &dialog);
    widgets.preset_delete->setAccessibleName(QObject::tr("Delete quick partition preset"));

    auto* presetRow = new QWidget(&dialog);
    auto* presetLayout = new QHBoxLayout(presetRow);
    presetLayout->setContentsMargins(
        ui::kMarginNone, ui::kMarginNone, ui::kMarginNone, ui::kMarginNone);
    presetLayout->setSpacing(ui::kSpacingTight);
    presetLayout->addWidget(widgets.preset_selector, 1);
    presetLayout->addWidget(widgets.preset_load);
    presetLayout->addWidget(widgets.preset_save);
    presetLayout->addWidget(widgets.preset_delete);
    return presetRow;
}

QTableWidget* createQuickPartitionTable(QWidget* parent) {
    auto* table = new QTableWidget(parent);
    table->setColumnCount(kQuickPartitionColumnCount);
    table->setHorizontalHeaderLabels({QObject::tr("Label"), QObject::tr("Size")});
    table->setMinimumHeight(kQuickPartitionTableMinHeight);
    table->horizontalHeader()->setStretchLastSection(true);
    table->verticalHeader()->setVisible(false);
    table->setSelectionMode(QAbstractItemView::NoSelection);
    table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table->setAccessibleName(QObject::tr("Quick partition labels and sizes"));
    return table;
}

void addQuickPartitionFormRows(const PartitionOperationDialog& dialog,
                               const QuickPartitionWidgets& widgets,
                               QWidget* presetRow) {
    dialog.formLayout()->addRow(QObject::tr("Preset:"), presetRow);
    dialog.formLayout()->addRow(QObject::tr("Partitions:"), widgets.partition_count);
    dialog.formLayout()->addRow(QObject::tr("Partition style:"), widgets.partition_style);
    dialog.formLayout()->addRow(QObject::tr("Size mode:"), widgets.size_mode);
    dialog.formLayout()->addRow(QObject::tr("Partitions:"), widgets.partition_table);
    dialog.formLayout()->addRow(QObject::tr("File system:"), widgets.file_system);
    dialog.formLayout()->addRow(QObject::tr("Cluster size:"), widgets.allocation_unit);
    dialog.formLayout()->addRow(QObject::tr("Label prefix:"), widgets.label_prefix);
    dialog.formLayout()->addRow(QString(), widgets.full_format);
}

QuickPartitionWidgets addQuickPartitionControls(PartitionOperationDialog& dialog,
                                                const PartitionDiskInfo& disk) {
    QuickPartitionWidgets widgets;
    auto* presetRow = createQuickPartitionPresetRow(dialog, widgets);

    widgets.partition_count = new QSpinBox(&dialog);
    widgets.partition_count->setRange(1, quickPartitionRawMaxCount(disk));
    widgets.partition_count->setValue(
        std::min(kQuickPartitionDefaultCount, widgets.partition_count->maximum()));
    widgets.partition_count->setAccessibleName(QObject::tr("Quick partition count"));

    widgets.partition_style = new QComboBox(&dialog);
    widgets.partition_style->setAccessibleName(QObject::tr("Quick partition style"));
    if (diskNeedsInitializeForQuickPartition(disk)) {
        widgets.partition_style->addItem(QStringLiteral("GPT"), QStringLiteral("GPT"));
        widgets.partition_style->addItem(QStringLiteral("MBR"), QStringLiteral("MBR"));
    } else {
        const QString style = disk.partition_style.toUpper();
        widgets.partition_style->addItem(QObject::tr("Keep %1").arg(style), style);
        widgets.partition_style->setEnabled(false);
    }

    widgets.size_mode = new QComboBox(&dialog);
    widgets.size_mode->addItem(QObject::tr("Equal size"), QStringLiteral("equal"));
    widgets.size_mode->addItem(QObject::tr("Custom sizes"), QStringLiteral("custom"));
    widgets.size_mode->setAccessibleName(QObject::tr("Quick partition size mode"));

    widgets.partition_table = createQuickPartitionTable(&dialog);
    widgets.file_system = new QComboBox(&dialog);
    widgets.file_system->addItems(
        {QStringLiteral("NTFS"), QStringLiteral("exFAT"), QStringLiteral("FAT32")});
    widgets.file_system->setAccessibleName(QObject::tr("Quick partition file system"));
    widgets.allocation_unit = createAllocationUnitSelector(&dialog);
    widgets.label_prefix = new QLineEdit(QStringLiteral("Data"), &dialog);
    widgets.label_prefix->setAccessibleName(QObject::tr("Quick partition label prefix"));
    widgets.full_format = new QCheckBox(QObject::tr("Full format"), &dialog);
    widgets.full_format->setAccessibleName(QObject::tr("Quick partition full format"));

    addQuickPartitionFormRows(dialog, widgets, presetRow);
    refreshQuickPartitionPresetSelector(widgets);
    updateQuickPartitionCountRange(widgets, disk);
    rebuildQuickPartitionTable(widgets, disk);
    updateQuickPartitionTableMode(widgets);
    return widgets;
}

void updateQuickPartitionPreview(PartitionOperationDialog& dialog,
                                 const QuickPartitionWidgets& widgets,
                                 const PartitionDiskInfo& disk) {
    const uint64_t usableBytes = quickPartitionUsableBytes(disk);
    const int count = widgets.partition_count->value();
    const QVector<uint64_t> sizes = quickPartitionSizesFromWidgets(widgets, usableBytes);
    const uint64_t totalBytes = quickPartitionTotalBytes(sizes);
    const uint64_t firstSliceBytes = sizes.isEmpty() ? 0 : sizes.first();
    const auto minimumBytes = static_cast<uint64_t>(kQuickPartitionMinimumSizeMb) * kMegabyteBytes;
    const QString actionText = diskNeedsInitializeForQuickPartition(disk)
                                   ? QObject::tr("Initialize as %1, then create")
                                         .arg(widgets.partition_style->currentText())
                               : !disk.partitions.isEmpty()
                                   ? QObject::tr("Delete existing partitions, then create")
                                   : QObject::tr("Create");
    updateQuickPartitionTableMode(widgets);
    if (widgets.label_prefix) {
        widgets.label_prefix->setEnabled(!quickPartitionCustomMode(widgets));
    }
    const bool sizesValid = quickPartitionSizesAreValid(sizes, usableBytes);
    dialog.setAcceptEnabled(sizesValid && firstSliceBytes >= minimumBytes);
    if (quickPartitionCustomMode(widgets)) {
        const QString remaining = totalBytes <= usableBytes
                                      ? formatPartitionBytes(usableBytes - totalBytes)
                                      : QObject::tr("over capacity");
        const QString schemeNote =
            selectedQuickPartitionScheme(widgets, disk) == QStringLiteral("MBR")
                ? QObject::tr(" MBR uses primary partitions and is capped at four.")
                : QString();
        dialog.setPreviewText(QObject::tr("%1 %2 custom %3 partition(s), total %4, remaining %5.%6")
                                  .arg(actionText,
                                       QString::number(count),
                                       widgets.file_system->currentText(),
                                       formatPartitionBytes(totalBytes),
                                       remaining,
                                       schemeNote));
        return;
    }
    const QString schemeNote = selectedQuickPartitionScheme(widgets, disk) == QStringLiteral("MBR")
                                   ? QObject::tr(" MBR primary cap: four partitions.")
                                   : QString();
    dialog.setPreviewText(
        QObject::tr("%1 %2 equal %3 partition(s), about %4 each, label prefix \"%5\".%6")
            .arg(actionText,
                 QString::number(count),
                 widgets.file_system->currentText(),
                 formatPartitionBytes(firstSliceBytes),
                 widgets.label_prefix->text(),
                 schemeNote));
}

using QuickPartitionCallback = std::function<void()>;

void connectQuickPartitionTableEditors(const QuickPartitionWidgets& widgets,
                                       QDialog& dialog,
                                       const QuickPartitionCallback& updatePreview) {
    for (int row = 0; row < widgets.partition_table->rowCount(); ++row) {
        if (auto* label = quickPartitionLabelEditor(widgets, row)) {
            QObject::connect(label, &QLineEdit::textChanged, &dialog, updatePreview);
        }
        if (auto* size = quickPartitionSizeEditor(widgets, row)) {
            QObject::connect(size, &QSpinBox::valueChanged, &dialog, updatePreview);
        }
    }
}

void updateQuickPartitionPresetButtons(const QuickPartitionWidgets& widgets) {
    const bool hasPreset = !selectedQuickPartitionPresetName(widgets).isEmpty();
    widgets.preset_load->setEnabled(hasPreset);
    widgets.preset_delete->setEnabled(hasPreset);
}

void loadQuickPartitionPresetForDialog(const QuickPartitionWidgets& widgets,
                                       const PartitionDiskInfo& disk,
                                       const QuickPartitionCallback& reconnectEditors,
                                       const QuickPartitionCallback& updatePreview) {
    const auto preset = findQuickPartitionPreset(selectedQuickPartitionPresetName(widgets));
    if (!preset) {
        return;
    }
    applyQuickPartitionPreset(widgets, disk, *preset);
    reconnectEditors();
    updatePreview();
}

void saveQuickPartitionPresetForDialog(QDialog& dialog, const QuickPartitionWidgets& widgets) {
    bool ok = false;
    const QString currentName = selectedQuickPartitionPresetName(widgets);
    const QString suggestedName = currentName.isEmpty() ? QObject::tr("Quick Partition Preset")
                                                        : currentName;
    const QString name = QInputDialog::getText(&dialog,
                                               QObject::tr("Save Quick Partition Preset"),
                                               QObject::tr("Preset name:"),
                                               QLineEdit::Normal,
                                               suggestedName,
                                               &ok)
                             .trimmed()
                             .left(kQuickPartitionPresetNameMaxChars);
    if (!ok || name.isEmpty()) {
        return;
    }
    saveQuickPartitionPreset(name, widgets);
    refreshQuickPartitionPresetSelector(widgets, name);
    updateQuickPartitionPresetButtons(widgets);
}

void deleteQuickPartitionPresetForDialog(const QuickPartitionWidgets& widgets) {
    const QString name = selectedQuickPartitionPresetName(widgets);
    if (name.isEmpty()) {
        return;
    }
    removeQuickPartitionPreset(name);
    refreshQuickPartitionPresetSelector(widgets);
    updateQuickPartitionPresetButtons(widgets);
}

void rebuildQuickPartitionControlsAndPreview(const QuickPartitionWidgets& widgets,
                                             const PartitionDiskInfo& disk,
                                             const QuickPartitionCallback& reconnectEditors,
                                             const QuickPartitionCallback& updatePreview) {
    updateQuickPartitionCountRange(widgets, disk);
    rebuildQuickPartitionTable(widgets, disk);
    reconnectEditors();
    updatePreview();
}

void connectQuickPartitionPresetControls(QDialog& dialog,
                                         const QuickPartitionWidgets& widgets,
                                         const PartitionDiskInfo& disk,
                                         const QuickPartitionCallback& reconnectEditors,
                                         const QuickPartitionCallback& updatePreview) {
    QObject::connect(widgets.preset_selector, &QComboBox::currentTextChanged, &dialog, [widgets]() {
        updateQuickPartitionPresetButtons(widgets);
    });
    QObject::connect(widgets.preset_load,
                     &QPushButton::clicked,
                     &dialog,
                     [widgets, disk, reconnectEditors, updatePreview]() {
                         loadQuickPartitionPresetForDialog(
                             widgets, disk, reconnectEditors, updatePreview);
                     });
    QObject::connect(widgets.preset_save, &QPushButton::clicked, &dialog, [&dialog, widgets]() {
        saveQuickPartitionPresetForDialog(dialog, widgets);
    });
    QObject::connect(widgets.preset_delete, &QPushButton::clicked, &dialog, [widgets]() {
        deleteQuickPartitionPresetForDialog(widgets);
    });
}

void connectQuickPartitionLiveControls(QDialog& dialog,
                                       const QuickPartitionWidgets& widgets,
                                       const QuickPartitionCallback& rebuildAndPreview,
                                       const QuickPartitionCallback& updatePreview) {
    QObject::connect(widgets.partition_count, &QSpinBox::valueChanged, &dialog, rebuildAndPreview);
    QObject::connect(
        widgets.partition_style, &QComboBox::currentTextChanged, &dialog, rebuildAndPreview);
    QObject::connect(widgets.size_mode, &QComboBox::currentTextChanged, &dialog, updatePreview);
    QObject::connect(widgets.file_system, &QComboBox::currentTextChanged, &dialog, updatePreview);
    QObject::connect(
        widgets.allocation_unit, &QComboBox::currentTextChanged, &dialog, updatePreview);
    QObject::connect(widgets.label_prefix, &QLineEdit::textChanged, &dialog, updatePreview);
}

uint64_t selectedResizeTargetBytes(const ResizePartitionWidgets& widgets) {
    return resizeTargetBytesFromInput(widgets.target_size_mb->value(),
                                      widgets.current_mb,
                                      widgets.current_bytes);
}

QString adjacentResizeMode() {
    return QStringLiteral("adjacent");
}

QString moveStartResizeMode() {
    return QStringLiteral("move_start");
}

QString donorResizeMode() {
    return QStringLiteral("donor");
}

QString selectedResizeMode(const ResizePartitionWidgets& widgets) {
    return widgets.mode->currentData().toString();
}

bool selectedResizeModeCanQueue(const ResizePartitionWidgets& widgets) {
    const QString mode = selectedResizeMode(widgets);
    return mode == adjacentResizeMode() || mode == moveStartResizeMode();
}

bool selectedResizeTargetChanged(const ResizePartitionWidgets& widgets) {
    return selectedResizeTargetBytes(widgets) != widgets.current_bytes;
}

QComboBox* createResizeModeSelector(QWidget* parent) {
    auto* combo = new QComboBox(parent);
    combo->setAccessibleName(QObject::tr("Move resize mode"));
    combo->addItem(QObject::tr("Resize or extend with adjacent free space"), adjacentResizeMode());
    combo->addItem(QObject::tr("Move partition start"), moveStartResizeMode());
    combo->addItem(QObject::tr("Extend from donor partition"), donorResizeMode());
    return combo;
}

QComboBox* createResizeDonorSelector(QWidget* parent,
                                     const PartitionDiskInfo& disk,
                                     const PartitionInfoEx& selectedPartition) {
    auto* combo = new QComboBox(parent);
    combo->setAccessibleName(QObject::tr("Donor partition"));
    combo->addItem(QObject::tr("Select donor partition"), QString());
    for (const auto& partition : disk.partitions) {
        if (partition.partition_number == selectedPartition.partition_number) {
            continue;
        }
        const QString label = partition.volume && !partition.volume->drive_letter.isEmpty()
                                  ? QObject::tr("Partition %1 (%2:, %3)")
                                        .arg(partition.partition_number)
                                        .arg(partition.volume->drive_letter,
                                             formatPartitionBytes(partition.size_bytes))
                                  : QObject::tr("Partition %1 (%2)")
                                        .arg(partition.partition_number)
                                        .arg(formatPartitionBytes(partition.size_bytes));
        combo->addItem(label, QString::number(partition.partition_number));
    }
    return combo;
}

void addResizeTargetSizeControls(PartitionOperationDialog& dialog,
                                 ResizePartitionWidgets* widgets,
                                 const PartitionInfoEx& partition) {
    widgets->target_size_mb = new QSpinBox(&dialog);
    const int minimumMb = std::min(widgets->current_mb,
                                   resizeInputMb(partitionUsedBytesForResize(partition), true));
    const int maximumMb = std::max(widgets->current_mb,
                                   resizeInputMb(widgets->max_online_bytes, false));
    widgets->target_size_mb->setRange(minimumMb, maximumMb);
    widgets->target_size_mb->setValue(widgets->current_mb);
    widgets->target_size_mb->setSuffix(QObject::tr(" MB"));
    widgets->target_size_mb->setAccessibleName(QObject::tr("Target partition size"));
    dialog.formLayout()->addRow(QObject::tr("Target size:"), widgets->target_size_mb);
    widgets->target_size_handle = createSizeHandle(&dialog,
                                                   QObject::tr("Target partition size handle"),
                                                   widgets->target_size_mb->minimum(),
                                                   widgets->target_size_mb->maximum(),
                                                   widgets->target_size_mb->value());
    dialog.formLayout()->addRow(QObject::tr("Size handle:"), widgets->target_size_handle);
}

void addResizeOffsetControls(PartitionOperationDialog& dialog,
                             ResizePartitionWidgets* widgets,
                             const PartitionDiskInfo& disk) {
    widgets->target_offset_mb = new QSpinBox(&dialog);
    widgets->target_offset_mb->setRange(
        0,
        std::max(1,
                 static_cast<int>(
                     std::min<uint64_t>(kMaxSizeInputMb, disk.size_bytes / kMegabyteBytes))));
    widgets->target_offset_mb->setValue(widgets->current_offset_mb);
    widgets->target_offset_mb->setSuffix(QObject::tr(" MB"));
    widgets->target_offset_mb->setAccessibleName(QObject::tr("Target partition start offset"));
    dialog.formLayout()->addRow(QObject::tr("Start offset:"), widgets->target_offset_mb);
}

void addResizeAdjacentFreeControl(PartitionOperationDialog& dialog,
                                  ResizePartitionWidgets* widgets,
                                  uint64_t adjacentFreeBytes) {
    widgets->adjacent_free_label = new QLabel(formatPartitionBytes(adjacentFreeBytes), &dialog);
    widgets->adjacent_free_label->setAccessibleName(
        QObject::tr("Contiguous free space after selected partition"));
    dialog.formLayout()->addRow(QObject::tr("Adjacent free after:"), widgets->adjacent_free_label);
}

void addResizeExtAndStatusControls(PartitionOperationDialog& dialog,
                                   ResizePartitionWidgets* widgets) {
    widgets->non_native_confirmation =
        new QCheckBox(QObject::tr("I understand this will resize the partition and then run "
                                  "bundled resize2fs on the ext file system."),
                      &dialog);
    widgets->non_native_confirmation->setAccessibleName(
        QObject::tr("Confirm ext filesystem resize"));
    dialog.formLayout()->addRow(QString(), widgets->non_native_confirmation);
    widgets->mode_status = new QLabel(&dialog);
    widgets->mode_status->setWordWrap(true);
    widgets->mode_status->setAccessibleName(QObject::tr("Move resize support status"));
    dialog.formLayout()->addRow(QObject::tr("Support:"), widgets->mode_status);
}

ResizePartitionWidgets addResizePartitionControls(PartitionOperationDialog& dialog,
                                                  const PartitionDiskInfo& disk,
                                                  const PartitionInfoEx& partition,
                                                  uint64_t adjacent_free_bytes) {
    ResizePartitionWidgets widgets;
    widgets.current_bytes = partition.size_bytes;
    widgets.current_offset_bytes = partition.offset_bytes;
    widgets.adjacent_free_bytes = adjacent_free_bytes;
    widgets.file_system = partition.volume ? partition.volume->file_system.trimmed() : QString();
    widgets.non_native_ext = isExtFilesystem(widgets.file_system);
    widgets.max_online_bytes = saturatingAdd(partition.size_bytes, adjacent_free_bytes);
    widgets.disk_bytes = disk.size_bytes;
    widgets.current_mb = resizeInputMb(partition.size_bytes, true);
    widgets.current_offset_mb = inputMegabytesFromBytes(partition.offset_bytes, 0, kMaxSizeInputMb);

    widgets.mode = createResizeModeSelector(&dialog);
    dialog.formLayout()->addRow(QObject::tr("Mode:"), widgets.mode);

    addResizeTargetSizeControls(dialog, &widgets, partition);
    addResizeOffsetControls(dialog, &widgets, disk);
    addResizeAdjacentFreeControl(dialog, &widgets, adjacent_free_bytes);
    widgets.donor_partition = createResizeDonorSelector(&dialog, disk, partition);
    dialog.formLayout()->addRow(QObject::tr("Donor partition:"), widgets.donor_partition);

    auto backup =
        addBackupRestoreControls(dialog,
                                 QObject::tr("Move partition backup directory"),
                                 QObject::tr("Browse move partition backup directory"),
                                 QObject::tr("I understand this will back up, delete, recreate, "
                                             "restore, and verify the selected partition."),
                                 QObject::tr("Confirm move partition backup and restore"));
    widgets.backup_directory = backup.backup_directory;
    widgets.browse_backup = backup.browse_backup;
    widgets.confirmation = backup.confirmation;
    addResizeExtAndStatusControls(dialog, &widgets);
    widgets.size_preview = new OperationSizePreviewWidget(&dialog);
    dialog.addVisualPreview(widgets.size_preview);
    return widgets;
}

QString resizeModeStatusText(const ResizePartitionWidgets& widgets) {
    const QString mode = selectedResizeMode(widgets);
    if (widgets.non_native_ext) {
        const uint64_t targetBytes = selectedResizeTargetBytes(widgets);
        if (mode != adjacentResizeMode()) {
            return QObject::tr(
                "Ext resize supports same-start adjacent resize only. Move and donor rebuild "
                "remain blocked "
                "until destructive VM certification.");
        }
        if (targetBytes < widgets.current_bytes) {
            return QObject::tr(
                "Ext shrink will run e2fsck, shrink the file system with bundled resize2fs, "
                "then shrink the partition and recheck it.");
        }
        if (!widgets.non_native_confirmation || !widgets.non_native_confirmation->isChecked()) {
            return QObject::tr("Confirm ext filesystem resize before queueing.");
        }
        return QObject::tr(
            "Ext grow will resize the partition, verify the bundled resize2fs hash, then grow the "
            "file system.");
    }
    if (mode == moveStartResizeMode()) {
        return QObject::tr(
            "Offline move will back up, delete, recreate at the selected offset, restore, "
            "compare SHA-256 manifests, and repair-scan the volume.");
    }
    if (mode == donorResizeMode()) {
        return QObject::tr(
            "Donor-space extend still requires a full multi-partition layout rebuild. Use "
            "Move partition start for one selected volume.");
    }
    if (!selectedResizeTargetChanged(widgets)) {
        return QObject::tr("Choose a target size different from the current size.");
    }
    return QObject::tr(
        "Supported online resize. Apply still rechecks Windows supported size "
        "limits before execution.");
}

QString resizeActionText(uint64_t targetBytes, uint64_t currentBytes) {
    if (targetBytes > currentBytes) {
        return QObject::tr("Extend");
    }
    if (targetBytes < currentBytes) {
        return QObject::tr("Shrink");
    }
    return QObject::tr("No change");
}

void updateResizePreviewRows(const ResizePartitionWidgets& widgets,
                             uint64_t targetBytes,
                             uint64_t scaleBytes) {
    widgets.size_preview->setRows({{QObject::tr("Current"),
                                    scaleBytes,
                                    widgets.current_bytes,
                                    QColor(QString::fromLatin1(ui::kColorPrimary)),
                                    formatPartitionBytes(widgets.current_bytes)},
                                   {QObject::tr("Target"),
                                    scaleBytes,
                                    targetBytes,
                                    QColor(QString::fromLatin1(ui::kColorSuccess)),
                                    formatPartitionBytes(targetBytes)},
                                   {QObject::tr("Max online"),
                                    scaleBytes,
                                    widgets.max_online_bytes,
                                    QColor(QString::fromLatin1(ui::kColorWarning)),
                                    QObject::tr("%1 current + %2 free")
                                        .arg(formatPartitionBytes(widgets.current_bytes),
                                             formatPartitionBytes(widgets.adjacent_free_bytes))}});
}

bool resizeMoveReady(const ResizePartitionWidgets& widgets,
                     bool moveMode,
                     uint64_t targetBytes,
                     uint64_t targetOffsetBytes) {
    if (!moveMode) {
        return false;
    }
    const bool changed = targetOffsetBytes != widgets.current_offset_bytes ||
                         targetBytes != widgets.current_bytes;
    const QString backup =
        widgets.backup_directory
            ? QDir::toNativeSeparators(widgets.backup_directory->text().trimmed())
            : QString();
    return changed && !backup.isEmpty() && widgets.confirmation &&
           widgets.confirmation->isChecked();
}

void updateResizeControlState(const ResizePartitionWidgets& widgets, bool moveMode) {
    widgets.target_size_mb->setEnabled(selectedResizeModeCanQueue(widgets));
    widgets.target_size_handle->setEnabled(selectedResizeModeCanQueue(widgets));
    widgets.target_offset_mb->setEnabled(moveMode);
    widgets.backup_directory->setEnabled(moveMode);
    widgets.browse_backup->setEnabled(moveMode);
    widgets.confirmation->setEnabled(moveMode);
    widgets.non_native_confirmation->setVisible(widgets.non_native_ext);
    widgets.non_native_confirmation->setEnabled(
        widgets.non_native_ext && selectedResizeMode(widgets) == adjacentResizeMode());
    widgets.donor_partition->setEnabled(selectedResizeMode(widgets) == donorResizeMode());
}

void updateResizePartitionPreview(PartitionOperationDialog& dialog,
                                  const ResizePartitionWidgets& widgets) {
    const uint64_t targetBytes = selectedResizeTargetBytes(widgets);
    const bool moveMode = selectedResizeMode(widgets) == moveStartResizeMode();
    const uint64_t targetOffsetBytes = static_cast<uint64_t>(widgets.target_offset_mb->value()) *
                                       kMegabyteBytes;
    const uint64_t scaleBytes = std::max(moveMode ? widgets.disk_bytes : widgets.max_online_bytes,
                                         targetBytes);
    updateResizePreviewRows(widgets, targetBytes, scaleBytes);
    const bool moveReady = resizeMoveReady(widgets, moveMode, targetBytes, targetOffsetBytes);
    const bool nativeAdjacentReady = !widgets.non_native_ext &&
                                     selectedResizeMode(widgets) == adjacentResizeMode() &&
                                     selectedResizeTargetChanged(widgets);
    const bool extAdjacentReady = widgets.non_native_ext &&
                                  selectedResizeMode(widgets) == adjacentResizeMode() &&
                                  targetBytes != widgets.current_bytes &&
                                  widgets.non_native_confirmation &&
                                  widgets.non_native_confirmation->isChecked();
    const bool adjacentReady = nativeAdjacentReady || extAdjacentReady;
    const bool queueable = adjacentReady || moveReady;
    updateResizeControlState(widgets, moveMode);
    {
        const QSignalBlocker targetHandleBlocker(widgets.target_size_handle);
        widgets.target_size_handle->setRange(widgets.target_size_mb->minimum(),
                                             widgets.target_size_mb->maximum());
        widgets.target_size_handle->setValue(widgets.target_size_mb->value());
    }
    widgets.mode_status->setText(resizeModeStatusText(widgets));
    dialog.setAcceptEnabled(queueable);
    dialog.setPreviewText(QObject::tr("%1 from %2 to %3. %4")
                              .arg(resizeActionText(targetBytes, widgets.current_bytes),
                                   formatPartitionBytes(widgets.current_bytes),
                                   formatPartitionBytes(targetBytes),
                                   resizeModeStatusText(widgets)));
}

void connectResizePartitionControls(PartitionOperationDialog& dialog,
                                    const ResizePartitionWidgets& widgets) {
    auto updatePreview = [&dialog, widgets]() {
        updateResizePartitionPreview(dialog, widgets);
    };
    QObject::connect(widgets.mode, &QComboBox::currentTextChanged, &dialog, updatePreview);
    QObject::connect(widgets.target_size_mb, &QSpinBox::valueChanged, &dialog, updatePreview);
    QObject::connect(widgets.target_offset_mb, &QSpinBox::valueChanged, &dialog, updatePreview);
    QObject::connect(widgets.target_size_handle,
                     &QSlider::valueChanged,
                     &dialog,
                     [widgets](int value) { widgets.target_size_mb->setValue(value); });
    QObject::connect(widgets.backup_directory, &QLineEdit::textChanged, &dialog, updatePreview);
    QObject::connect(widgets.confirmation, &QCheckBox::toggled, &dialog, updatePreview);
    QObject::connect(widgets.non_native_confirmation, &QCheckBox::toggled, &dialog, updatePreview);
    connectBackupBrowse(
        dialog,
        {widgets.backup_directory, widgets.browse_backup, widgets.confirmation, nullptr},
        QObject::tr("Move Partition Backup Directory"));
    widgets.size_preview->setInteractiveSegment(
        kSizePreviewTargetRow, false, true, [widgets](uint64_t, uint64_t sizeBytes) {
            widgets.target_size_mb->setValue(inputMegabytesFromBytes(
                sizeBytes, widgets.target_size_mb->minimum(), widgets.target_size_mb->maximum()));
        });
    QObject::connect(
        widgets.donor_partition, &QComboBox::currentTextChanged, &dialog, updatePreview);
}

bool missingResizeSelection(const std::optional<PartitionTarget>& target,
                            const PartitionDiskInfo* disk,
                            const PartitionInfoEx* partition) {
    return !target || !disk || !partition;
}

QString unchangedResizeStatus(const ResizePartitionWidgets& widgets,
                              const PartitionInfoEx& partition) {
    const bool moveMode = selectedResizeMode(widgets) == moveStartResizeMode();
    const uint64_t targetOffsetBytes = static_cast<uint64_t>(widgets.target_offset_mb->value()) *
                                       kMegabyteBytes;
    if (!moveMode && selectedResizeTargetBytes(widgets) == partition.size_bytes) {
        return QObject::tr("Resize target unchanged");
    }
    if (moveMode && selectedResizeTargetBytes(widgets) == partition.size_bytes &&
        targetOffsetBytes == partition.offset_bytes) {
        return QObject::tr("Move target unchanged");
    }
    return {};
}

PartitionOperationType resizeOperationType(const ResizePartitionWidgets& widgets) {
    if (selectedResizeMode(widgets) == moveStartResizeMode()) {
        return PartitionOperationType::MovePartition;
    }
    return PartitionOperationType::Resize;
}

QJsonObject resizePartitionPayload(const ResizePartitionWidgets& widgets,
                                   const PartitionInfoEx& partition) {
    QJsonObject payload;
    payload[QStringLiteral("target_size_bytes")] =
        QString::number(selectedResizeTargetBytes(widgets));
    payload[QStringLiteral("adjacent_free_bytes")] = QString::number(widgets.adjacent_free_bytes);
    if (widgets.non_native_ext && selectedResizeMode(widgets) == adjacentResizeMode()) {
        payload[QStringLiteral("non_native_file_system_tool")] = true;
        payload[QStringLiteral("file_system")] = widgets.file_system;
        payload[QStringLiteral("target_path")] = nonNativeFilesystemWriteTargetPath(&partition);
        payload[QStringLiteral("target_wipe_confirmed")] =
            widgets.non_native_confirmation && widgets.non_native_confirmation->isChecked();
    }
    if (selectedResizeMode(widgets) == moveStartResizeMode()) {
        payload[QStringLiteral("target_offset_bytes")] = QString::number(
            static_cast<uint64_t>(widgets.target_offset_mb->value()) * kMegabyteBytes);
        if (partition.volume) {
            payload[QStringLiteral("drive_letter")] =
                partition.volume->drive_letter.left(1).toUpper();
            payload[QStringLiteral("file_system")] = partition.volume->file_system;
            payload[QStringLiteral("label")] = partition.volume->label;
        }
        payload[QStringLiteral("backup_directory")] =
            QDir::toNativeSeparators(widgets.backup_directory->text().trimmed());
        payload[QStringLiteral("target_wipe_confirmed")] = widgets.confirmation->isChecked();
    }
    return payload;
}

QString propertyValue(const QString& value) {
    return value.trimmed().isEmpty() ? QObject::tr("Not reported") : value;
}

QString propertyYesNo(bool value) {
    return value ? QObject::tr("Yes") : QObject::tr("No");
}

QString bytesProperty(uint64_t bytes) {
    return QObject::tr("%1 (%2 bytes)").arg(formatPartitionBytes(bytes), QString::number(bytes));
}

void addProperty(QVector<PropertyRow>* rows, const QString& name, const QString& value) {
    rows->append({name, propertyValue(value)});
}

void addProperty(QVector<PropertyRow>* rows, const QString& name, uint64_t bytes) {
    rows->append({name, bytesProperty(bytes)});
}

QString partitionFlagsText(const PartitionInfoEx& partition) {
    QStringList flags;
    const QVector<QPair<QString, bool>> flagStates{
        {QObject::tr("System"), partition.is_system},
        {QObject::tr("Boot"), partition.is_boot},
        {QObject::tr("EFI"), partition.is_efi},
        {QObject::tr("MSR"), partition.is_msr},
        {QObject::tr("Recovery"), partition.is_recovery},
        {QObject::tr("Active"), partition.is_active},
        {QObject::tr("Read-only"), partition.is_read_only},
        {QObject::tr("BitLocker"), partition.volume && partition.volume->bitlocker_enabled},
        {QObject::tr("Dirty file system"), partition.volume && partition.volume->dirty_bit_set},
    };
    for (const auto& flag : flagStates) {
        if (flag.second) {
            flags << flag.first;
        }
    }
    return flags.isEmpty() ? QObject::tr("None") : flags.join(QStringLiteral(", "));
}

void appendDiskProperties(QVector<PropertyRow>* rows, const PartitionDiskInfo& disk) {
    addProperty(rows, QObject::tr("Disk number"), QString::number(disk.disk_number));
    addProperty(rows, QObject::tr("Model"), disk.model);
    addProperty(rows, QObject::tr("Serial number"), disk.serial_number);
    addProperty(rows, QObject::tr("Device path"), disk.device_path);
    addProperty(rows, QObject::tr("Bus type"), disk.bus_type);
    addProperty(rows, QObject::tr("Media type"), disk.media_type);
    addProperty(rows, QObject::tr("Partition style"), disk.partition_style);
    addProperty(rows, QObject::tr("Disk size"), disk.size_bytes);
    addProperty(rows, QObject::tr("Health"), disk.health_status);
    addProperty(rows, QObject::tr("Operational status"), disk.operational_status);
    addProperty(rows, QObject::tr("System disk"), propertyYesNo(disk.is_system));
    addProperty(rows, QObject::tr("Read-only disk"), propertyYesNo(disk.is_read_only));
    addProperty(rows, QObject::tr("Dynamic disk"), propertyYesNo(disk.is_dynamic));
    addProperty(rows, QObject::tr("Storage Spaces"), propertyYesNo(disk.is_storage_spaces));
    addProperty(rows, QObject::tr("Partitions"), QString::number(disk.partitions.size()));
    addProperty(rows,
                QObject::tr("Unallocated regions"),
                QString::number(disk.unallocated_regions.size()));
}

void appendSmartProperties(QVector<PropertyRow>* rows, const PartitionDiskInfo& disk) {
    addProperty(rows, QObject::tr("SMART summary"), disk.smart_summary);
    addProperty(rows,
                QObject::tr("Temperature"),
                disk.temperature_celsius >= 0 ? QObject::tr("%1 C").arg(disk.temperature_celsius)
                                              : QString());
    addProperty(rows, QObject::tr("Power-on hours"), QString::number(disk.power_on_hours));
    addProperty(rows, QObject::tr("Read errors"), QString::number(disk.read_errors_total));
    addProperty(rows, QObject::tr("Write errors"), QString::number(disk.write_errors_total));
    addProperty(rows, QObject::tr("Wear"), QObject::tr("%1%").arg(disk.wear_percent));
}

void appendPartitionProperties(QVector<PropertyRow>* rows, const PartitionInfoEx& partition) {
    addProperty(rows, QObject::tr("Partition number"), QString::number(partition.partition_number));
    addProperty(rows, QObject::tr("Partition GUID"), partition.partition_guid);
    addProperty(rows, QObject::tr("Type name"), partition.type_name);
    addProperty(rows, QObject::tr("GPT/MBR type"), partition.gpt_type);
    addProperty(rows, QObject::tr("Offset"), partition.offset_bytes);
    addProperty(rows, QObject::tr("Size"), partition.size_bytes);
    addProperty(rows, QObject::tr("Flags"), partitionFlagsText(partition));
    if (!partition.volume) {
        addProperty(rows, QObject::tr("Volume"), QObject::tr("No mounted volume"));
        return;
    }
    addProperty(rows, QObject::tr("Drive letter"), partition.volume->drive_letter);
    addProperty(rows, QObject::tr("Volume label"), partition.volume->label);
    addProperty(rows, QObject::tr("File system"), partition.volume->file_system);
    const QString source =
        PartitionFileSystemDetector::sourceDisplayName(partition.volume->file_system_source);
    if (!source.isEmpty()) {
        addProperty(rows, QObject::tr("File system source"), source);
    }
    if (!partition.volume->file_system_details.isEmpty()) {
        addProperty(rows,
                    QObject::tr("File system metadata"),
                    partition.volume->file_system_details.join(QLatin1Char('\n')));
    }
    const auto capability =
        PartitionFileSystemRegistry::capabilityFor(partition.volume->file_system);
    addProperty(rows, QObject::tr("S.A.K. filesystem support"), capability.support_level);
    addProperty(rows,
                QObject::tr("Supported S.A.K. actions"),
                PartitionFileSystemRegistry::actionSummary(capability.available_actions));
    addProperty(rows,
                QObject::tr("Blocked filesystem actions"),
                PartitionFileSystemRegistry::actionSummary(capability.blocked_actions));
    if (!capability.required_tools.isEmpty()) {
        addProperty(rows,
                    QObject::tr("Required bundled tools"),
                    PartitionFileSystemRegistry::actionSummary(capability.required_tools));
    }
    addProperty(rows, QObject::tr("Volume health"), partition.volume->health_status);
    addProperty(rows, QObject::tr("Volume size"), partition.volume->total_bytes);
    addProperty(rows, QObject::tr("Free space"), partition.volume->free_bytes);
    addProperty(rows,
                QObject::tr("BitLocker locked"),
                propertyYesNo(partition.volume->bitlocker_locked));
}

QString fileSystemTooltipText(const PartitionVolumeInfo& volume) {
    if (volume.file_system.trimmed().isEmpty()) {
        return {};
    }

    QStringList tooltipParts;
    const QString source =
        PartitionFileSystemDetector::sourceDisplayName(volume.file_system_source);
    if (!source.isEmpty()) {
        tooltipParts.append(QObject::tr("Detected by %1").arg(source));
    }
    const auto capability = PartitionFileSystemRegistry::capabilityFor(volume.file_system);
    if (!capability.support_level.isEmpty()) {
        tooltipParts.append(QObject::tr("S.A.K. support: %1").arg(capability.support_level));
    }
    if (!volume.file_system_details.isEmpty()) {
        tooltipParts.append(volume.file_system_details.join(QStringLiteral("\n")));
    }
    return tooltipParts.join(QStringLiteral("\n"));
}

void appendFilesystemInspectionRows(QVector<PropertyRow>* rows,
                                    const PartitionDiskInfo* disk,
                                    const PartitionInfoEx& partition,
                                    const QString& rawTargetPath) {
    const auto& volume = partition.volume.value();
    addProperty(rows,
                QObject::tr("Disk"),
                disk ? QString::number(disk->disk_number) : QString::number(partition.disk_number));
    addProperty(rows, QObject::tr("Partition"), QString::number(partition.partition_number));
    addProperty(rows, QObject::tr("Raw target path"), rawTargetPath);
    addProperty(rows, QObject::tr("File system"), volume.file_system);
    const QString source =
        PartitionFileSystemDetector::sourceDisplayName(volume.file_system_source);
    addProperty(rows, QObject::tr("Detection source"), source);
    addProperty(rows, QObject::tr("Read-only metadata"), volume.file_system_details.join('\n'));

    const auto capability = PartitionFileSystemRegistry::capabilityFor(volume.file_system);
    addProperty(rows, QObject::tr("S.A.K. filesystem support"), capability.support_level);
    addProperty(rows,
                QObject::tr("Supported S.A.K. actions"),
                PartitionFileSystemRegistry::actionSummary(capability.available_actions));
    addProperty(rows,
                QObject::tr("Blocked filesystem actions"),
                PartitionFileSystemRegistry::actionSummary(capability.blocked_actions));
    if (!capability.required_tools.isEmpty()) {
        addProperty(rows,
                    QObject::tr("Required bundled tools"),
                    PartitionFileSystemRegistry::actionSummary(capability.required_tools));
    }
}

void appendUnallocatedProperties(QVector<PropertyRow>* rows, const PartitionTarget& target) {
    addProperty(rows, QObject::tr("Region type"), QObject::tr("Unallocated space"));
    addProperty(rows, QObject::tr("Disk number"), QString::number(target.disk_number));
    addProperty(rows, QObject::tr("Offset"), target.offset_bytes);
    addProperty(rows, QObject::tr("Size"), target.size_bytes);
}

QString propertiesClipboardText(const QVector<PropertyRow>& rows) {
    QStringList lines;
    lines.reserve(rows.size());
    for (const auto& row : rows) {
        lines.append(QStringLiteral("%1: %2").arg(row.name, row.value));
    }
    return lines.join(QLatin1Char('\n'));
}

QTableWidget* createPropertiesTable(QWidget* parent, const QVector<PropertyRow>& rows) {
    auto* table = new QTableWidget(rows.size(), kPropertiesColumnCount, parent);
    table->setObjectName(QStringLiteral("partitionPropertiesTable"));
    table->setAccessibleName(QObject::tr("Partition properties table"));
    table->setHorizontalHeaderLabels({QObject::tr("Property"), QObject::tr("Value")});
    table->verticalHeader()->setVisible(false);
    table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table->setSelectionBehavior(QAbstractItemView::SelectRows);
    table->setAlternatingRowColors(true);
    table->horizontalHeader()->setStretchLastSection(true);
    for (qsizetype row = 0; row < rows.size(); ++row) {
        table->setItem(static_cast<int>(row),
                       PropertyColName,
                       new QTableWidgetItem(rows.at(row).name));
        table->setItem(static_cast<int>(row),
                       PropertyColValue,
                       new QTableWidgetItem(rows.at(row).value));
    }
    return table;
}

void showPropertiesDialog(QWidget* parent, const QString& title, const QVector<PropertyRow>& rows) {
    QDialog dialog(parent);
    dialog.setObjectName(QStringLiteral("partitionPropertiesDialog"));
    dialog.setWindowTitle(title);
    dialog.setAccessibleName(title);
    dialog.setMinimumSize(kPropertiesDialogMinWidth, kPropertiesDialogMinHeight);

    auto* layout = new QVBoxLayout(&dialog);
    layout->setContentsMargins(
        ui::kMarginMedium, ui::kMarginMedium, ui::kMarginMedium, ui::kMarginMedium);
    auto* table = createPropertiesTable(&dialog, rows);
    layout->addWidget(table);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, &dialog);
    auto* copyButton = buttons->addButton(QObject::tr("Copy Details"),
                                          QDialogButtonBox::ActionRole);
    copyButton->setAccessibleName(QObject::tr("Copy partition properties"));
    QObject::connect(copyButton, &QPushButton::clicked, [&rows]() {
        QApplication::clipboard()->setText(propertiesClipboardText(rows));
    });
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addWidget(buttons);
    dialog.exec();
}

void showMetadataConsistencyDialog(QWidget* parent, const NonNativeFilesystemCheckState& state) {
    const QStringList findings = metadataSanityFindings(state.metadata_details);
    const bool warning = hasMetadataSanityWarnings(state.metadata_details);
    QVector<PropertyRow> rows;
    addProperty(&rows, QObject::tr("File system"), state.file_system);
    addProperty(&rows, QObject::tr("Raw target path"), state.target_path);
    addProperty(&rows,
                QObject::tr("Check type"),
                QObject::tr("Original read-only metadata consistency check"));
    addProperty(&rows,
                QObject::tr("Result"),
                warning ? QObject::tr("Warnings found") : QObject::tr("No sanity warnings"));
    addProperty(&rows, QObject::tr("Findings"), findings.join('\n'));
    addProperty(&rows, QObject::tr("Read-only metadata"), state.metadata_details.join('\n'));
    showPropertiesDialog(parent, QObject::tr("Check %1 Metadata").arg(state.file_system), rows);
}

QString hfsConsistencyResultText(const PartitionHfsConsistencyCheckResult& result) {
    if (!result.blockers.isEmpty()) {
        return QObject::tr("Blocked");
    }
    return result.warnings.isEmpty() ? QObject::tr("No consistency blockers")
                                     : QObject::tr("Warnings found");
}

PartitionHfsConsistencyCheckResult showHfsConsistencyDialog(
    QWidget* parent, const NonNativeFilesystemCheckState& state) {
    auto result = PartitionHfsFileSystemReader::checkConsistencyFromImage(
        state.target_path, kPartitionHfsDefaultCheckRecordLimit);
    QVector<PropertyRow> rows;
    addProperty(&rows,
                QObject::tr("File system"),
                result.file_system.trimmed().isEmpty() ? state.file_system : result.file_system);
    addProperty(&rows, QObject::tr("Raw target path"), state.target_path);
    addProperty(
        &rows,
        QObject::tr("Check type"),
        QObject::tr(
            "Original read-only HFS+ catalog consistency check with attributes B-tree key scan"));
    addProperty(&rows, QObject::tr("Result"), hfsConsistencyResultText(result));
    addProperty(&rows,
                QObject::tr("Catalog records scanned"),
                QString::number(result.records_scanned));
    addProperty(&rows, QObject::tr("Directories"), QString::number(result.directories));
    addProperty(&rows, QObject::tr("Files"), QString::number(result.files));
    addProperty(&rows, QObject::tr("Thread records"), QString::number(result.threads));
    addProperty(&rows, QObject::tr("Other records"), QString::number(result.other_records));
    addProperty(&rows,
                QObject::tr("Attributes file"),
                result.attributes_present ? QObject::tr("Present") : QObject::tr("Not present"));
    addProperty(&rows,
                QObject::tr("Attribute records scanned"),
                QString::number(result.attribute_records_scanned));
    addProperty(&rows,
                QObject::tr("Inline attribute records"),
                QString::number(result.inline_attribute_records));
    addProperty(&rows,
                QObject::tr("Fork attribute records"),
                QString::number(result.fork_attribute_records));
    addProperty(&rows,
                QObject::tr("Extent attribute records"),
                QString::number(result.extent_attribute_records));
    addProperty(&rows,
                QObject::tr("Other attribute records"),
                QString::number(result.other_attribute_records));
    addProperty(&rows, QObject::tr("Attribute names"), result.attribute_names.join('\n'));
    addProperty(&rows, QObject::tr("Attribute metadata"), result.attribute_metadata.join('\n'));
    addProperty(&rows, QObject::tr("Findings"), result.details.join('\n'));
    addProperty(&rows, QObject::tr("Warnings"), result.warnings.join('\n'));
    addProperty(&rows, QObject::tr("Blockers"), result.blockers.join('\n'));
    showPropertiesDialog(parent, QObject::tr("Check %1 Catalog").arg(state.file_system), rows);
    return result;
}

struct NonNativeBrowseRequest {
    QString target_path;
    QString file_system_path;
};

std::optional<NonNativeBrowseRequest> showNonNativeBrowseRequestDialog(
    QWidget* parent, const NonNativeFilesystemBrowseState& state) {
    QDialog dialog(parent);
    dialog.setObjectName(QStringLiteral("partitionNonNativeBrowseRequestDialog"));
    dialog.setWindowTitle(QObject::tr("Browse Non-Windows File System"));
    dialog.setAccessibleName(QObject::tr("Browse non-Windows file system request"));
    dialog.setMinimumWidth(kOperationDialogMinWidth);

    auto* layout = new QVBoxLayout(&dialog);
    auto* form = new QFormLayout();
    auto* target = new QLineEdit(state.target_path, &dialog);
    target->setAccessibleName(QObject::tr("Read-only target path"));
    auto* path = new QLineEdit(QStringLiteral("/"), &dialog);
    path->setAccessibleName(QObject::tr("Non-Windows directory path"));
    form->addRow(QObject::tr("Target:"), target);
    form->addRow(QObject::tr("Path:"), path);
    layout->addLayout(form);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    buttons->button(QDialogButtonBox::Ok)->setText(QObject::tr("Browse"));
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addWidget(buttons);

    if (dialog.exec() != QDialog::Accepted) {
        return std::nullopt;
    }
    return NonNativeBrowseRequest{target->text().trimmed(), path->text().trimmed()};
}

QString extBrowserClipboardText(const PartitionExtFileReadResult& result) {
    QStringList lines;
    lines.append(QObject::tr("File system: %1").arg(result.file_system));
    for (const auto& warning : result.warnings) {
        lines.append(QObject::tr("Warning: %1").arg(warning));
    }
    for (const auto& entry : result.entries) {
        lines.append(QStringLiteral("%1\t%2\t%3\t%4\t%5")
                         .arg(entry.path,
                              entry.type,
                              QString::number(entry.size_bytes),
                              QString::number(entry.inode),
                              entry.symlink_target));
    }
    return lines.join(QLatin1Char('\n'));
}

const PartitionExtFileEntry* selectedExtBrowserEntry(const QTableWidget* table,
                                                     const PartitionExtFileReadResult& result) {
    if (!table || table->currentRow() < 0 || table->currentRow() >= result.entries.size()) {
        return nullptr;
    }
    return &result.entries.at(table->currentRow());
}

void updateExtExtractButton(QPushButton* button,
                            const QTableWidget* table,
                            const PartitionExtFileReadResult& result) {
    const auto* entry = selectedExtBrowserEntry(table, result);
    button->setEnabled(entry && entry->regular_file);
}

void extractSelectedExtEntry(QWidget* parent,
                             const QString& targetPath,
                             const PartitionExtFileEntry& entry) {
    if (!entry.regular_file) {
        showWarningLogged(parent,
                          QObject::tr("Extract Non-Windows File"),
                          QObject::tr("Select a regular file to extract."));
        return;
    }

    const QString outputPath = QFileDialog::getSaveFileName(parent,
                                                            QObject::tr("Extract Non-Windows File"),
                                                            QDir::home().filePath(entry.name));
    if (outputPath.trimmed().isEmpty()) {
        return;
    }

    const auto file = PartitionExtFileSystemReader::readFileFromImage(targetPath,
                                                                      entry.path,
                                                                      kExtBrowserExtractMaxBytes);
    if (!file.ok) {
        showWarningLogged(parent,
                          QObject::tr("Extract Non-Windows File"),
                          file.blockers.join(QStringLiteral("\n")));
        return;
    }

    QFile output(outputPath);
    if (!output.open(QIODevice::WriteOnly | QIODevice::Truncate) ||
        output.write(file.data) != file.data.size()) {
        showWarningLogged(parent,
                          QObject::tr("Extract Non-Windows File"),
                          QObject::tr("Could not write selected output file."));
    }
}

void exportExtDirectory(QWidget* parent, const QString& targetPath, const QString& extPath) {
    const QString outputDirectory = QFileDialog::getExistingDirectory(
        parent, QObject::tr("Export ext Directory"), QDir::homePath());
    if (outputDirectory.trimmed().isEmpty()) {
        return;
    }

    const PartitionExtDirectoryExportOptions options{kNonNativeBrowserExportMaxEntries,
                                                     kExtBrowserExtractMaxBytes,
                                                     kNonNativeBrowserExportMaxTotalBytes};
    const auto result = PartitionExtFileSystemReader::exportDirectoryFromImage(
        targetPath, extPath, outputDirectory, options);
    if (!result.ok) {
        showWarningLogged(parent,
                          QObject::tr("Export ext Directory"),
                          result.blockers.join(QStringLiteral("\n")));
        return;
    }
    showInformationLogged(parent,
                          QObject::tr("Export ext Directory"),
                          QObject::tr(
                              "Exported %1 file(s), %2 folder(s), %3 symlink sidecar(s), %4 bytes.")
                              .arg(result.files_exported)
                              .arg(result.directories_exported)
                              .arg(result.symlinks_exported)
                              .arg(result.bytes_exported));
}

QTableWidget* createExtBrowserTable(QWidget* parent, const PartitionExtFileReadResult& result) {
    auto* table = new QTableWidget(result.entries.size(), kExtBrowserColumnCount, parent);
    table->setObjectName(QStringLiteral("partitionExtBrowserTable"));
    table->setAccessibleName(QObject::tr("ext directory listing table"));
    table->setHorizontalHeaderLabels({QObject::tr("Name"),
                                      QObject::tr("Type"),
                                      QObject::tr("Size"),
                                      QObject::tr("Inode"),
                                      QObject::tr("Link Target"),
                                      QObject::tr("Path")});
    table->verticalHeader()->setVisible(false);
    table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table->setSelectionBehavior(QAbstractItemView::SelectRows);
    table->setAlternatingRowColors(true);
    table->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    for (int row = 0; row < result.entries.size(); ++row) {
        const auto& entry = result.entries.at(row);
        table->setItem(row, kExtBrowserColumnName, new QTableWidgetItem(entry.name));
        table->setItem(row, kExtBrowserColumnType, new QTableWidgetItem(entry.type));
        table->setItem(row,
                       kExtBrowserColumnSize,
                       new QTableWidgetItem(formatPartitionBytes(entry.size_bytes)));
        table->setItem(row,
                       kExtBrowserColumnInode,
                       new QTableWidgetItem(QString::number(entry.inode)));
        table->setItem(row,
                       kExtBrowserColumnLinkTarget,
                       new QTableWidgetItem(entry.symlink_target));
        table->setItem(row, kExtBrowserColumnPath, new QTableWidgetItem(entry.path));
    }
    return table;
}

void showExtDirectoryListingDialog(QWidget* parent,
                                   const QString& title,
                                   const QString& targetPath,
                                   const QString& extPath,
                                   const PartitionExtFileReadResult& result) {
    QDialog dialog(parent);
    dialog.setObjectName(QStringLiteral("partitionExtBrowserDialog"));
    dialog.setWindowTitle(title);
    dialog.setAccessibleName(title);
    dialog.setMinimumSize(kPropertiesDialogMinWidth, kPropertiesDialogMinHeight);

    auto* layout = new QVBoxLayout(&dialog);
    auto* summary =
        new QLabel(QObject::tr("Target: %1\nPath: %2")
                       .arg(targetPath, extPath.isEmpty() ? QStringLiteral("/") : extPath),
                   &dialog);
    summary->setAccessibleName(QObject::tr("ext browser summary"));
    summary->setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard);
    layout->addWidget(summary);

    auto* table = createExtBrowserTable(&dialog, result);
    layout->addWidget(table, 1);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, &dialog);
    auto* copyButton = buttons->addButton(QObject::tr("Copy Listing"),
                                          QDialogButtonBox::ActionRole);
    copyButton->setAccessibleName(QObject::tr("Copy ext directory listing"));
    auto* extractButton = buttons->addButton(QObject::tr("Extract Selected"),
                                             QDialogButtonBox::ActionRole);
    extractButton->setAccessibleName(QObject::tr("Extract selected ext file"));
    auto* exportButton = buttons->addButton(QObject::tr("Export Directory"),
                                            QDialogButtonBox::ActionRole);
    exportButton->setAccessibleName(QObject::tr("Export current ext directory"));
    updateExtExtractButton(extractButton, table, result);
    QObject::connect(copyButton, &QPushButton::clicked, [&result]() {
        QApplication::clipboard()->setText(extBrowserClipboardText(result));
    });
    QObject::connect(table, &QTableWidget::itemSelectionChanged, [&]() {
        updateExtExtractButton(extractButton, table, result);
    });
    QObject::connect(extractButton, &QPushButton::clicked, [&]() {
        if (const auto* entry = selectedExtBrowserEntry(table, result)) {
            extractSelectedExtEntry(&dialog, targetPath, *entry);
        }
    });
    QObject::connect(exportButton, &QPushButton::clicked, [&]() {
        exportExtDirectory(&dialog, targetPath, extPath);
    });
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addWidget(buttons);
    dialog.exec();
}

QString hfsBrowserClipboardText(const PartitionHfsFileReadResult& result) {
    QStringList lines;
    lines.append(QObject::tr("File system: %1").arg(result.file_system));
    for (const auto& warning : result.warnings) {
        lines.append(QObject::tr("Warning: %1").arg(warning));
    }
    for (const auto& entry : result.entries) {
        lines.append(QStringLiteral("%1\t%2\t%3\t%4\t%5")
                         .arg(entry.path,
                              entry.type,
                              QString::number(entry.size_bytes),
                              QString::number(entry.resource_fork_size_bytes),
                              QString::number(entry.catalog_id)));
    }
    return lines.join(QLatin1Char('\n'));
}

const PartitionHfsFileEntry* selectedHfsBrowserEntry(const QTableWidget* table,
                                                     const PartitionHfsFileReadResult& result) {
    if (!table || table->currentRow() < 0 || table->currentRow() >= result.entries.size()) {
        return nullptr;
    }
    return &result.entries.at(table->currentRow());
}

void updateHfsExtractButton(QPushButton* button,
                            const QTableWidget* table,
                            const PartitionHfsFileReadResult& result) {
    const auto* entry = selectedHfsBrowserEntry(table, result);
    button->setEnabled(entry && entry->regular_file);
}

void updateHfsResourceExtractButton(QPushButton* button,
                                    const QTableWidget* table,
                                    const PartitionHfsFileReadResult& result) {
    const auto* entry = selectedHfsBrowserEntry(table, result);
    button->setEnabled(entry && entry->regular_file && entry->resource_fork_size_bytes > 0);
}

void updateHfsAttributeExtractButton(QPushButton* button,
                                     const QTableWidget* table,
                                     const PartitionHfsFileReadResult& result) {
    const auto* entry = selectedHfsBrowserEntry(table, result);
    button->setEnabled(entry && entry->regular_file && entry->catalog_id != 0);
}

QString safeHfsAttributeFileName(const QString& fileName, QString attributeName) {
    attributeName = attributeName.trimmed();
    static const QRegularExpression unsafeChars(QStringLiteral(R"([<>:"/\\|?*\x00-\x1F])"));
    QString safeAttribute = attributeName.replace(unsafeChars, QStringLiteral("_"));
    while (safeAttribute.endsWith(QLatin1Char('.')) || safeAttribute.endsWith(QLatin1Char(' '))) {
        safeAttribute.chop(1);
    }
    if (safeAttribute.isEmpty()) {
        safeAttribute = QStringLiteral("attribute");
    }
    return QStringLiteral("%1.%2.attr")
        .arg(fileName, safeAttribute.left(kHfsAttributeExportNameMaxChars));
}

void writeExtractedHfsFork(QWidget* parent,
                           const QString& outputPath,
                           const PartitionHfsFileReadResult& file) {
    if (!file.ok) {
        showWarningLogged(parent,
                          QObject::tr("Extract Non-Windows File"),
                          file.blockers.join(QStringLiteral("\n")));
        return;
    }

    QFile output(outputPath);
    if (!output.open(QIODevice::WriteOnly | QIODevice::Truncate) ||
        output.write(file.data) != file.data.size()) {
        showWarningLogged(parent,
                          QObject::tr("Extract Non-Windows File"),
                          QObject::tr("Could not write selected output file."));
    }
}

void writeExtractedHfsAttribute(QWidget* parent,
                                const QString& outputPath,
                                const PartitionHfsAttributeReadResult& attribute) {
    if (!attribute.ok) {
        showWarningLogged(parent,
                          QObject::tr("Extract HFS+ Attribute"),
                          attribute.blockers.join(QStringLiteral("\n")));
        return;
    }

    QFile output(outputPath);
    if (!output.open(QIODevice::WriteOnly | QIODevice::Truncate) ||
        output.write(attribute.data) != attribute.data.size()) {
        showWarningLogged(parent,
                          QObject::tr("Extract HFS+ Attribute"),
                          QObject::tr("Could not write selected output file."));
    }
}

void extractSelectedHfsEntry(QWidget* parent,
                             const QString& targetPath,
                             const PartitionHfsFileEntry& entry) {
    if (!entry.regular_file) {
        showWarningLogged(parent,
                          QObject::tr("Extract Non-Windows File"),
                          QObject::tr("Select a regular file to extract."));
        return;
    }

    const QString outputPath = QFileDialog::getSaveFileName(parent,
                                                            QObject::tr("Extract Non-Windows File"),
                                                            QDir::home().filePath(entry.name));
    if (outputPath.trimmed().isEmpty()) {
        return;
    }

    const auto file = PartitionHfsFileSystemReader::readFileFromImage(targetPath,
                                                                      entry.path,
                                                                      kExtBrowserExtractMaxBytes);
    writeExtractedHfsFork(parent, outputPath, file);
}

void extractSelectedHfsAttribute(QWidget* parent,
                                 const QString& targetPath,
                                 const PartitionHfsFileEntry& entry) {
    if (!entry.regular_file || entry.catalog_id == 0) {
        showWarningLogged(parent,
                          QObject::tr("Extract HFS+ Attribute"),
                          QObject::tr("Select a regular HFS+ file."));
        return;
    }

    bool accepted = false;
    const QString attributeName = QInputDialog::getText(parent,
                                                        QObject::tr("Extract HFS+ Attribute"),
                                                        QObject::tr("Attribute name"),
                                                        QLineEdit::Normal,
                                                        QStringLiteral("com.apple.FinderInfo"),
                                                        &accepted)
                                      .trimmed();
    if (!accepted || attributeName.isEmpty()) {
        return;
    }

    const QString outputPath = QFileDialog::getSaveFileName(
        parent,
        QObject::tr("Extract HFS+ Attribute"),
        QDir::home().filePath(safeHfsAttributeFileName(entry.name, attributeName)));
    if (outputPath.trimmed().isEmpty()) {
        return;
    }
    const auto attribute = PartitionHfsFileSystemReader::readAttributeValueFromImage(
        targetPath, entry.catalog_id, attributeName, kExtBrowserExtractMaxBytes);
    writeExtractedHfsAttribute(parent, outputPath, attribute);
}

void extractSelectedHfsResourceFork(QWidget* parent,
                                    const QString& targetPath,
                                    const PartitionHfsFileEntry& entry) {
    if (!entry.regular_file || entry.resource_fork_size_bytes == 0) {
        showWarningLogged(parent,
                          QObject::tr("Extract Non-Windows File"),
                          QObject::tr("Select a regular file with a resource fork."));
        return;
    }

    const QString outputPath = QFileDialog::getSaveFileName(
        parent,
        QObject::tr("Extract HFS+ Resource Fork"),
        QDir::home().filePath(QStringLiteral("%1.rsrc").arg(entry.name)));
    if (outputPath.trimmed().isEmpty()) {
        return;
    }
    const auto file = PartitionHfsFileSystemReader::readResourceForkFromImage(
        targetPath, entry.path, kExtBrowserExtractMaxBytes);
    writeExtractedHfsFork(parent, outputPath, file);
}

void exportHfsDirectory(QWidget* parent, const QString& targetPath, const QString& hfsPath) {
    const QString outputDirectory = QFileDialog::getExistingDirectory(
        parent, QObject::tr("Export HFS+ Directory"), QDir::homePath());
    if (outputDirectory.trimmed().isEmpty()) {
        return;
    }

    const PartitionHfsDirectoryExportOptions options{kNonNativeBrowserExportMaxEntries,
                                                     kExtBrowserExtractMaxBytes,
                                                     kNonNativeBrowserExportMaxTotalBytes};
    const auto result = PartitionHfsFileSystemReader::exportDirectoryFromImage(
        targetPath, hfsPath, outputDirectory, options);
    if (!result.ok) {
        showWarningLogged(parent,
                          QObject::tr("Export HFS+ Directory"),
                          result.blockers.join(QStringLiteral("\n")));
        return;
    }
    showInformationLogged(
        parent,
        QObject::tr("Export HFS+ Directory"),
        QObject::tr("Exported %1 data file(s), %2 resource fork(s), %3 folder(s), %4 bytes.")
            .arg(result.files_exported)
            .arg(result.resource_forks_exported)
            .arg(result.directories_exported)
            .arg(result.bytes_exported));
}

void configureNonNativeBrowserTable(QTableWidget* table, const QString& accessibleName) {
    table->setAccessibleName(accessibleName);
    table->verticalHeader()->setVisible(false);
    table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table->setSelectionBehavior(QAbstractItemView::SelectRows);
    table->setAlternatingRowColors(true);
    table->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
}

void addHfsBrowserSummary(QVBoxLayout* layout,
                          QDialog* dialog,
                          const QString& targetPath,
                          const QString& hfsPath) {
    auto* summary =
        new QLabel(QObject::tr("Target: %1\nPath: %2")
                       .arg(targetPath, hfsPath.isEmpty() ? QStringLiteral("/") : hfsPath),
                   dialog);
    summary->setAccessibleName(QObject::tr("HFS+ browser summary"));
    summary->setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard);
    layout->addWidget(summary);
}

QTableWidget* createHfsBrowserTable(QDialog* dialog, const PartitionHfsFileReadResult& result) {
    auto* table = new QTableWidget(result.entries.size(), kHfsBrowserColumnCount, dialog);
    table->setObjectName(QStringLiteral("partitionHfsBrowserTable"));
    table->setHorizontalHeaderLabels({QObject::tr("Name"),
                                      QObject::tr("Type"),
                                      QObject::tr("Data Size"),
                                      QObject::tr("Resource Size"),
                                      QObject::tr("Catalog ID"),
                                      QObject::tr("Path")});
    configureNonNativeBrowserTable(table, QObject::tr("HFS+ directory listing table"));
    for (int row = 0; row < result.entries.size(); ++row) {
        const auto& entry = result.entries.at(row);
        table->setItem(row, kHfsBrowserColumnName, new QTableWidgetItem(entry.name));
        table->setItem(row, kHfsBrowserColumnType, new QTableWidgetItem(entry.type));
        table->setItem(row,
                       kHfsBrowserColumnSize,
                       new QTableWidgetItem(formatPartitionBytes(entry.size_bytes)));
        table->setItem(row,
                       kHfsBrowserColumnResourceSize,
                       new QTableWidgetItem(formatPartitionBytes(entry.resource_fork_size_bytes)));
        table->setItem(row,
                       kHfsBrowserColumnCatalogId,
                       new QTableWidgetItem(QString::number(entry.catalog_id)));
        table->setItem(row, kHfsBrowserColumnPath, new QTableWidgetItem(entry.path));
    }
    return table;
}

QDialogButtonBox* createHfsBrowserButtons(QDialog* dialog,
                                          QTableWidget* table,
                                          const NonNativeBrowseRequest& request,
                                          const PartitionHfsFileReadResult& result) {
    const auto* resultPtr = &result;
    const QString targetPath = request.target_path;
    const QString hfsPath = request.file_system_path;
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, dialog);
    auto* copyButton = buttons->addButton(QObject::tr("Copy Listing"),
                                          QDialogButtonBox::ActionRole);
    copyButton->setAccessibleName(QObject::tr("Copy HFS+ directory listing"));
    auto* extractButton = buttons->addButton(QObject::tr("Extract Selected"),
                                             QDialogButtonBox::ActionRole);
    extractButton->setAccessibleName(QObject::tr("Extract selected HFS+ file"));
    auto* extractResourceButton = buttons->addButton(QObject::tr("Extract Resource Fork"),
                                                     QDialogButtonBox::ActionRole);
    extractResourceButton->setAccessibleName(QObject::tr("Extract selected HFS+ resource fork"));
    auto* extractAttributeButton = buttons->addButton(QObject::tr("Extract Attribute"),
                                                      QDialogButtonBox::ActionRole);
    extractAttributeButton->setAccessibleName(QObject::tr("Extract selected HFS+ attribute"));
    auto* exportButton = buttons->addButton(QObject::tr("Export Directory"),
                                            QDialogButtonBox::ActionRole);
    exportButton->setAccessibleName(QObject::tr("Export current HFS+ directory"));
    updateHfsExtractButton(extractButton, table, *resultPtr);
    updateHfsResourceExtractButton(extractResourceButton, table, *resultPtr);
    updateHfsAttributeExtractButton(extractAttributeButton, table, *resultPtr);
    QObject::connect(copyButton, &QPushButton::clicked, [resultPtr]() {
        QApplication::clipboard()->setText(hfsBrowserClipboardText(*resultPtr));
    });
    QObject::connect(
        table,
        &QTableWidget::itemSelectionChanged,
        [extractButton, extractResourceButton, extractAttributeButton, table, resultPtr]() {
            updateHfsExtractButton(extractButton, table, *resultPtr);
            updateHfsResourceExtractButton(extractResourceButton, table, *resultPtr);
            updateHfsAttributeExtractButton(extractAttributeButton, table, *resultPtr);
        });
    QObject::connect(extractButton,
                     &QPushButton::clicked,
                     [dialog, table, targetPath, resultPtr]() {
                         if (const auto* entry = selectedHfsBrowserEntry(table, *resultPtr)) {
                             extractSelectedHfsEntry(dialog, targetPath, *entry);
                         }
                     });
    QObject::connect(extractResourceButton,
                     &QPushButton::clicked,
                     [dialog, table, targetPath, resultPtr]() {
                         if (const auto* entry = selectedHfsBrowserEntry(table, *resultPtr)) {
                             extractSelectedHfsResourceFork(dialog, targetPath, *entry);
                         }
                     });
    QObject::connect(extractAttributeButton,
                     &QPushButton::clicked,
                     [dialog, table, targetPath, resultPtr]() {
                         if (const auto* entry = selectedHfsBrowserEntry(table, *resultPtr)) {
                             extractSelectedHfsAttribute(dialog, targetPath, *entry);
                         }
                     });
    QObject::connect(exportButton, &QPushButton::clicked, [dialog, targetPath, hfsPath]() {
        exportHfsDirectory(dialog, targetPath, hfsPath);
    });
    QObject::connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::reject);
    return buttons;
}

void showHfsDirectoryListingDialog(QWidget* parent,
                                   const QString& title,
                                   const QString& targetPath,
                                   const QString& hfsPath,
                                   const PartitionHfsFileReadResult& result) {
    QDialog dialog(parent);
    dialog.setObjectName(QStringLiteral("partitionHfsBrowserDialog"));
    dialog.setWindowTitle(title);
    dialog.setAccessibleName(title);
    dialog.setMinimumSize(kPropertiesDialogMinWidth, kPropertiesDialogMinHeight);

    auto* layout = new QVBoxLayout(&dialog);
    addHfsBrowserSummary(layout, &dialog, targetPath, hfsPath);
    auto* table = createHfsBrowserTable(&dialog, result);
    layout->addWidget(table, 1);
    const NonNativeBrowseRequest request{targetPath, hfsPath};
    layout->addWidget(createHfsBrowserButtons(&dialog, table, request, result));
    dialog.exec();
}

QString apfsBrowserClipboardText(const PartitionApfsFileReadResult& result) {
    QStringList lines;
    lines.append(QObject::tr("File system: %1").arg(result.file_system));
    if (!result.volume_name.trimmed().isEmpty()) {
        lines.append(QObject::tr("Volume: %1").arg(result.volume_name));
    }
    for (const auto& warning : result.warnings) {
        lines.append(QObject::tr("Warning: %1").arg(warning));
    }
    for (const auto& entry : result.entries) {
        lines.append(QStringLiteral("%1\t%2\t%3\t%4")
                         .arg(entry.path,
                              entry.type,
                              QString::number(entry.size_bytes),
                              QString::number(entry.object_id)));
    }
    return lines.join(QLatin1Char('\n'));
}

const PartitionApfsFileEntry* selectedApfsBrowserEntry(const QTableWidget* table,
                                                       const PartitionApfsFileReadResult& result) {
    if (!table || table->currentRow() < 0 || table->currentRow() >= result.entries.size()) {
        return nullptr;
    }
    return &result.entries.at(table->currentRow());
}

void updateApfsExtractButton(QPushButton* button,
                             const QTableWidget* table,
                             const PartitionApfsFileReadResult& result) {
    const auto* entry = selectedApfsBrowserEntry(table, result);
    button->setEnabled(entry && entry->regular_file);
}

void extractSelectedApfsEntry(QWidget* parent,
                              const QString& targetPath,
                              const PartitionApfsFileEntry& entry) {
    if (!entry.regular_file) {
        showWarningLogged(parent,
                          QObject::tr("Extract Non-Windows File"),
                          QObject::tr("Select a regular file to extract."));
        return;
    }

    const QString outputPath = QFileDialog::getSaveFileName(parent,
                                                            QObject::tr("Extract Non-Windows File"),
                                                            QDir::home().filePath(entry.name));
    if (outputPath.trimmed().isEmpty()) {
        return;
    }

    const auto file = PartitionApfsFileSystemReader::readFileFromImage(targetPath,
                                                                       entry.path,
                                                                       kExtBrowserExtractMaxBytes);
    if (!file.ok) {
        showWarningLogged(parent,
                          QObject::tr("Extract Non-Windows File"),
                          file.blockers.join(QStringLiteral("\n")));
        return;
    }

    QFile output(outputPath);
    if (!output.open(QIODevice::WriteOnly | QIODevice::Truncate) ||
        output.write(file.data) != file.data.size()) {
        showWarningLogged(parent,
                          QObject::tr("Extract Non-Windows File"),
                          QObject::tr("Could not write selected output file."));
    }
}

void exportApfsDirectory(QWidget* parent, const QString& targetPath, const QString& apfsPath) {
    const QString outputDirectory = QFileDialog::getExistingDirectory(
        parent, QObject::tr("Export APFS Directory"), QDir::homePath());
    if (outputDirectory.trimmed().isEmpty()) {
        return;
    }

    const PartitionApfsDirectoryExportOptions options{kNonNativeBrowserExportMaxEntries,
                                                      kExtBrowserExtractMaxBytes,
                                                      kNonNativeBrowserExportMaxTotalBytes};
    const auto result = PartitionApfsFileSystemReader::exportDirectoryFromImage(
        targetPath, apfsPath, outputDirectory, options);
    if (!result.ok) {
        showWarningLogged(parent,
                          QObject::tr("Export APFS Directory"),
                          result.blockers.join(QStringLiteral("\n")));
        return;
    }
    showInformationLogged(parent,
                          QObject::tr("Export APFS Directory"),
                          QObject::tr(
                              "Exported %1 file(s), %2 folder(s), skipped %3 symlink(s), %4 bytes.")
                              .arg(result.files_exported)
                              .arg(result.directories_exported)
                              .arg(result.symlinks_skipped)
                              .arg(result.bytes_exported));
}

void addApfsBrowserSummary(QVBoxLayout* layout,
                           QDialog* dialog,
                           const QString& targetPath,
                           const QString& apfsPath,
                           const PartitionApfsFileReadResult& result) {
    QString summaryText = QObject::tr("Target: %1\nVolume: %2\nPath: %3")
                              .arg(targetPath,
                                   result.volume_name.trimmed().isEmpty() ? QStringLiteral("APFS")
                                                                          : result.volume_name,
                                   apfsPath.isEmpty() ? QStringLiteral("/") : apfsPath);
    if (!result.warnings.isEmpty()) {
        summaryText.append(
            QObject::tr("\nWarnings: %1").arg(result.warnings.join(QStringLiteral("; "))));
    }
    auto* summary = new QLabel(summaryText, dialog);
    summary->setAccessibleName(QObject::tr("APFS browser summary"));
    summary->setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard);
    layout->addWidget(summary);
}

QTableWidget* createApfsBrowserTable(QDialog* dialog, const PartitionApfsFileReadResult& result) {
    auto* table = new QTableWidget(result.entries.size(), kApfsBrowserColumnCount, dialog);
    table->setObjectName(QStringLiteral("partitionApfsBrowserTable"));
    table->setHorizontalHeaderLabels({QObject::tr("Name"),
                                      QObject::tr("Type"),
                                      QObject::tr("Size"),
                                      QObject::tr("Object ID"),
                                      QObject::tr("Path")});
    configureNonNativeBrowserTable(table, QObject::tr("APFS directory listing table"));
    for (int row = 0; row < result.entries.size(); ++row) {
        const auto& entry = result.entries.at(row);
        table->setItem(row, kApfsBrowserColumnName, new QTableWidgetItem(entry.name));
        table->setItem(row, kApfsBrowserColumnType, new QTableWidgetItem(entry.type));
        table->setItem(row,
                       kApfsBrowserColumnSize,
                       new QTableWidgetItem(formatPartitionBytes(entry.size_bytes)));
        table->setItem(row,
                       kApfsBrowserColumnObjectId,
                       new QTableWidgetItem(QString::number(entry.object_id)));
        table->setItem(row, kApfsBrowserColumnPath, new QTableWidgetItem(entry.path));
    }
    return table;
}

QDialogButtonBox* createApfsBrowserButtons(QDialog* dialog,
                                           QTableWidget* table,
                                           const NonNativeBrowseRequest& request,
                                           const PartitionApfsFileReadResult& result) {
    const auto* resultPtr = &result;
    const QString targetPath = request.target_path;
    const QString apfsPath = request.file_system_path;
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, dialog);
    auto* copyButton = buttons->addButton(QObject::tr("Copy Listing"),
                                          QDialogButtonBox::ActionRole);
    copyButton->setAccessibleName(QObject::tr("Copy APFS directory listing"));
    auto* extractButton = buttons->addButton(QObject::tr("Extract Selected"),
                                             QDialogButtonBox::ActionRole);
    extractButton->setAccessibleName(QObject::tr("Extract selected APFS file"));
    auto* exportButton = buttons->addButton(QObject::tr("Export Directory"),
                                            QDialogButtonBox::ActionRole);
    exportButton->setAccessibleName(QObject::tr("Export current APFS directory"));
    updateApfsExtractButton(extractButton, table, *resultPtr);
    QObject::connect(copyButton, &QPushButton::clicked, [resultPtr]() {
        QApplication::clipboard()->setText(apfsBrowserClipboardText(*resultPtr));
    });
    QObject::connect(table,
                     &QTableWidget::itemSelectionChanged,
                     [extractButton, table, resultPtr]() {
                         updateApfsExtractButton(extractButton, table, *resultPtr);
                     });
    QObject::connect(extractButton,
                     &QPushButton::clicked,
                     [dialog, table, targetPath, resultPtr]() {
                         if (const auto* entry = selectedApfsBrowserEntry(table, *resultPtr)) {
                             extractSelectedApfsEntry(dialog, targetPath, *entry);
                         }
                     });
    QObject::connect(exportButton, &QPushButton::clicked, [dialog, targetPath, apfsPath]() {
        exportApfsDirectory(dialog, targetPath, apfsPath);
    });
    QObject::connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::reject);
    return buttons;
}

void showApfsDirectoryListingDialog(QWidget* parent,
                                    const QString& title,
                                    const QString& targetPath,
                                    const QString& apfsPath,
                                    const PartitionApfsFileReadResult& result) {
    QDialog dialog(parent);
    dialog.setObjectName(QStringLiteral("partitionApfsBrowserDialog"));
    dialog.setWindowTitle(title);
    dialog.setAccessibleName(title);
    dialog.setMinimumSize(kPropertiesDialogMinWidth, kPropertiesDialogMinHeight);

    auto* layout = new QVBoxLayout(&dialog);
    addApfsBrowserSummary(layout, &dialog, targetPath, apfsPath, result);
    auto* table = createApfsBrowserTable(&dialog, result);
    layout->addWidget(table, 1);
    const NonNativeBrowseRequest request{targetPath, apfsPath};
    layout->addWidget(createApfsBrowserButtons(&dialog, table, request, result));
    dialog.exec();
}

struct NonNativeBrowseResult {
    int entries{0};
    QStringList blockers;
};

QString nonNativeBrowseTitle(const NonNativeFilesystemBrowseState& state) {
    return QObject::tr("Browse %1 File System").arg(state.file_system);
}

NonNativeBrowseResult browseHfsFileSystem(QWidget* parent,
                                          const NonNativeFilesystemBrowseState& state,
                                          const NonNativeBrowseRequest& request) {
    const auto result = PartitionHfsFileSystemReader::listDirectoryFromImage(
        request.target_path, request.file_system_path, kPartitionHfsDefaultBrowseEntryLimit);
    if (!result.ok) {
        return {0, result.blockers};
    }
    showHfsDirectoryListingDialog(
        parent, nonNativeBrowseTitle(state), request.target_path, request.file_system_path, result);
    return {static_cast<int>(result.entries.size()), {}};
}

NonNativeBrowseResult browseApfsFileSystem(QWidget* parent,
                                           const NonNativeFilesystemBrowseState& state,
                                           const NonNativeBrowseRequest& request) {
    const auto result = PartitionApfsFileSystemReader::listDirectoryFromImage(
        request.target_path, request.file_system_path, kPartitionApfsDefaultBrowseEntryLimit);
    if (!result.ok) {
        return {0, result.blockers};
    }
    showApfsDirectoryListingDialog(
        parent, nonNativeBrowseTitle(state), request.target_path, request.file_system_path, result);
    return {static_cast<int>(result.entries.size()), {}};
}

NonNativeBrowseResult browseExtFileSystem(QWidget* parent,
                                          const NonNativeFilesystemBrowseState& state,
                                          const NonNativeBrowseRequest& request) {
    const auto result = PartitionExtFileSystemReader::listDirectoryFromImage(
        request.target_path, request.file_system_path, kPartitionExtDefaultBrowseEntryLimit);
    if (!result.ok) {
        return {0, result.blockers};
    }
    showExtDirectoryListingDialog(
        parent, nonNativeBrowseTitle(state), request.target_path, request.file_system_path, result);
    return {static_cast<int>(result.entries.size()), {}};
}

NonNativeBrowseResult browseNonNativeFileSystem(QWidget* parent,
                                                const NonNativeFilesystemBrowseState& state,
                                                const NonNativeBrowseRequest& request) {
    if (isHfsFilesystem(state.file_system)) {
        return browseHfsFileSystem(parent, state, request);
    }
    if (isApfsFilesystem(state.file_system)) {
        return browseApfsFileSystem(parent, state, request);
    }
    return browseExtFileSystem(parent, state, request);
}

QString bitLockerMountPoint(const PartitionVolumeInfo* volume) {
    if (!volume || volume->drive_letter.trimmed().isEmpty()) {
        return {};
    }
    return QStringLiteral("%1:").arg(volume->drive_letter.left(1).toUpper());
}

QString bitLockerProtectionText(const PartitionVolumeInfo* volume) {
    if (!volume) {
        return QObject::tr("No mounted volume selected");
    }
    return volume->bitlocker_enabled ? QObject::tr("Protection on")
                                     : QObject::tr("Protection off or not detected");
}

QString bitLockerLockText(const PartitionVolumeInfo* volume) {
    if (!volume || !volume->bitlocker_enabled) {
        return QObject::tr("Not locked");
    }
    return volume->bitlocker_locked ? QObject::tr("Locked") : QObject::tr("Unlocked");
}

enum class BitLockerDialogAction {
    None,
    Unlock,
    Suspend,
    Resume,
};

PartitionOperationType bitLockerOperationType(BitLockerDialogAction action) {
    return action == BitLockerDialogAction::Suspend ? PartitionOperationType::BitLockerSuspend
                                                    : PartitionOperationType::BitLockerResume;
}

QStringList bitLockerCommandLines(const PartitionVolumeInfo* volume) {
    const QString mountPoint = bitLockerMountPoint(volume);
    if (mountPoint.isEmpty()) {
        return {QStringLiteral("manage-bde.exe -status")};
    }
    return {QStringLiteral("manage-bde.exe -status %1").arg(mountPoint),
            QStringLiteral("manage-bde.exe -protectors -disable %1").arg(mountPoint),
            QStringLiteral("manage-bde.exe -protectors -enable %1").arg(mountPoint),
            QStringLiteral("Suspend-BitLocker -MountPoint \"%1\" -RebootCount 1").arg(mountPoint),
            QStringLiteral("Resume-BitLocker -MountPoint \"%1\"").arg(mountPoint),
            QStringLiteral("Unlock-BitLocker -MountPoint \"%1\" -RecoveryPassword "
                           "\"<48-digit-recovery-key>\"")
                .arg(mountPoint)};
}

QVector<PropertyRow> bitLockerRows(const PartitionDiskInfo* disk,
                                   const PartitionInfoEx* partition) {
    const PartitionVolumeInfo* volume = partition && partition->volume ? &partition->volume.value()
                                                                       : nullptr;
    QVector<PropertyRow> rows;
    addProperty(&rows,
                QObject::tr("Target"),
                partition ? QObject::tr("Disk %1 Partition %2")
                                .arg(partition->disk_number)
                                .arg(partition->partition_number)
                          : QObject::tr("No partition selected"));
    addProperty(&rows,
                QObject::tr("Disk"),
                disk ? QObject::tr("Disk %1 %2").arg(disk->disk_number).arg(disk->model)
                     : QString());
    addProperty(&rows, QObject::tr("Mount point"), bitLockerMountPoint(volume));
    addProperty(&rows, QObject::tr("Protection"), bitLockerProtectionText(volume));
    addProperty(&rows, QObject::tr("Lock state"), bitLockerLockText(volume));
    addProperty(&rows, QObject::tr("Volume GUID"), volume ? volume->volume_guid : QString());
    addProperty(&rows,
                QObject::tr("In-app mutation"),
                volume && volume->bitlocker_enabled
                    ? QObject::tr("Queue unlock, suspend, or resume through elevated Apply")
                    : QObject::tr("Select a BitLocker-protected mounted volume"));
    addProperty(&rows, QObject::tr("Safe commands"), bitLockerCommandLines(volume).join('\n'));
    return rows;
}

BitLockerDialogAction showBitLockerDialog(QWidget* parent,
                                          const QVector<PropertyRow>& rows,
                                          const QStringList& commandLines,
                                          bool canManage,
                                          bool locked) {
    QDialog dialog(parent);
    dialog.setObjectName(QStringLiteral("partitionBitLockerDialog"));
    dialog.setWindowTitle(QObject::tr("Manage BitLocker"));
    dialog.setAccessibleName(QObject::tr("BitLocker management"));
    dialog.setMinimumSize(kBitLockerDialogMinWidth, kBitLockerDialogMinHeight);

    auto* layout = new QVBoxLayout(&dialog);
    layout->setContentsMargins(
        ui::kMarginMedium, ui::kMarginMedium, ui::kMarginMedium, ui::kMarginMedium);

    auto* table = createPropertiesTable(&dialog, rows);
    table->setObjectName(QStringLiteral("partitionBitLockerStatusTable"));
    table->setAccessibleName(QObject::tr("BitLocker status table"));
    layout->addWidget(table);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, &dialog);
    BitLockerDialogAction selectedAction = BitLockerDialogAction::None;
    if (canManage) {
        auto* unlockButton = buttons->addButton(QObject::tr("Queue Unlock"),
                                                QDialogButtonBox::ActionRole);
        unlockButton->setAccessibleName(QObject::tr("Queue BitLocker unlock"));
        unlockButton->setEnabled(locked);
        QObject::connect(unlockButton, &QPushButton::clicked, [&]() {
            selectedAction = BitLockerDialogAction::Unlock;
            dialog.accept();
        });

        auto* suspendButton = buttons->addButton(QObject::tr("Queue Suspend"),
                                                 QDialogButtonBox::ActionRole);
        suspendButton->setAccessibleName(QObject::tr("Queue BitLocker suspend"));
        suspendButton->setEnabled(!locked);
        QObject::connect(suspendButton, &QPushButton::clicked, [&]() {
            selectedAction = BitLockerDialogAction::Suspend;
            dialog.accept();
        });

        auto* resumeButton = buttons->addButton(QObject::tr("Queue Resume"),
                                                QDialogButtonBox::ActionRole);
        resumeButton->setAccessibleName(QObject::tr("Queue BitLocker resume"));
        resumeButton->setEnabled(!locked);
        QObject::connect(resumeButton, &QPushButton::clicked, [&]() {
            selectedAction = BitLockerDialogAction::Resume;
            dialog.accept();
        });
    }
    auto* copyButton = buttons->addButton(QObject::tr("Copy Commands"),
                                          QDialogButtonBox::ActionRole);
    copyButton->setAccessibleName(QObject::tr("Copy BitLocker commands"));
    auto* openButton = buttons->addButton(QObject::tr("Open Windows BitLocker"),
                                          QDialogButtonBox::ActionRole);
    openButton->setAccessibleName(QObject::tr("Open Windows BitLocker management"));
    QObject::connect(copyButton, &QPushButton::clicked, [commandLines]() {
        QApplication::clipboard()->setText(commandLines.join('\n'));
    });
    QObject::connect(openButton, &QPushButton::clicked, []() {
        QProcess::startDetached(QStringLiteral("control.exe"),
                                {QStringLiteral("/name"),
                                 QStringLiteral("Microsoft.BitLockerDriveEncryption")});
    });
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addWidget(buttons);
    dialog.exec();
    return selectedAction;
}

QString optimizeMountPoint(const PartitionVolumeInfo* volume) {
    if (!volume || volume->drive_letter.trimmed().isEmpty()) {
        return {};
    }
    return QStringLiteral("%1:").arg(volume->drive_letter.left(1).toUpper());
}

bool diskLooksSsd(const PartitionDiskInfo* disk) {
    const QString media =
        disk ? QStringLiteral("%1 %2").arg(disk->media_type, disk->bus_type).toUpper() : QString();
    return media.contains(QStringLiteral("SSD")) || media.contains(QStringLiteral("NVME"));
}

bool diskLooksHdd(const PartitionDiskInfo* disk) {
    const QString media = disk ? disk->media_type.toUpper() : QString();
    return media.contains(QStringLiteral("HDD")) || media.contains(QStringLiteral("HARD"));
}

QString optimizeModeText(const PartitionDiskInfo* disk) {
    if (diskLooksSsd(disk)) {
        return QObject::tr("SSD/NVMe ReTrim");
    }
    if (diskLooksHdd(disk)) {
        return QObject::tr("HDD analyze and defrag");
    }
    return QObject::tr("Analyze first, then choose ReTrim or defrag");
}

enum class OptimizeDialogAction {
    None,
    ReTrim,
    Defrag,
};

struct OptimizeDialogSpec {
    QVector<PropertyRow> rows;
    QStringList command_lines;
    bool can_queue{false};
    bool ssd_mode{false};
    bool hdd_mode{false};
};

QStringList optimizeCommandLines(const PartitionDiskInfo* disk, const PartitionVolumeInfo* volume) {
    const QString mountPoint = optimizeMountPoint(volume);
    if (mountPoint.isEmpty()) {
        return {QStringLiteral("dfrgui.exe")};
    }
    const QString driveLetter = mountPoint.left(1);
    QStringList commands{
        QStringLiteral("Optimize-Volume -DriveLetter %1 -Analyze -Verbose").arg(driveLetter)};
    if (diskLooksSsd(disk)) {
        commands
            << QStringLiteral("fsutil behavior query DisableDeleteNotify")
            << QStringLiteral("Optimize-Volume -DriveLetter %1 -ReTrim -Verbose").arg(driveLetter);
    } else if (diskLooksHdd(disk)) {
        commands
            << QStringLiteral("Optimize-Volume -DriveLetter %1 -Defrag -Verbose").arg(driveLetter);
    }
    return commands;
}

QVector<PropertyRow> optimizeRows(const PartitionDiskInfo* disk, const PartitionInfoEx* partition) {
    const PartitionVolumeInfo* volume = partition && partition->volume ? &partition->volume.value()
                                                                       : nullptr;
    QVector<PropertyRow> rows;
    addProperty(&rows,
                QObject::tr("Target"),
                partition ? QObject::tr("Disk %1 Partition %2")
                                .arg(partition->disk_number)
                                .arg(partition->partition_number)
                          : QObject::tr("No partition selected"));
    addProperty(&rows, QObject::tr("Mount point"), optimizeMountPoint(volume));
    addProperty(&rows, QObject::tr("Media type"), disk ? disk->media_type : QString());
    addProperty(&rows, QObject::tr("Optimization mode"), optimizeModeText(disk));
    addProperty(&rows,
                QObject::tr("In-app HDD defrag execution"),
                !volume || optimizeMountPoint(volume).isEmpty()
                    ? QObject::tr("Select a mounted volume before queueing optimization")
                : diskLooksHdd(disk)
                    ? QObject::tr("Queue HDD defrag through cancellable elevated Apply")
                : diskLooksSsd(disk)
                    ? QObject::tr("Queue SSD ReTrim through cancellable elevated Apply")
                    : QObject::tr("Direct queue disabled until Windows reports SSD or HDD media"));
    addProperty(&rows, QObject::tr("Safe commands"), optimizeCommandLines(disk, volume).join('\n'));
    return rows;
}

OptimizeDialogAction showOptimizeDrivesDialog(QWidget* parent, const OptimizeDialogSpec& spec) {
    QDialog dialog(parent);
    dialog.setObjectName(QStringLiteral("partitionOptimizeDrivesDialog"));
    dialog.setWindowTitle(QObject::tr("Disk Defrag and Optimization"));
    dialog.setAccessibleName(QObject::tr("Disk defrag and optimization"));
    dialog.setMinimumSize(kOptimizeDialogMinWidth, kOptimizeDialogMinHeight);

    auto* layout = new QVBoxLayout(&dialog);
    layout->setContentsMargins(
        ui::kMarginMedium, ui::kMarginMedium, ui::kMarginMedium, ui::kMarginMedium);

    auto* table = createPropertiesTable(&dialog, spec.rows);
    table->setObjectName(QStringLiteral("partitionOptimizeStatusTable"));
    table->setAccessibleName(QObject::tr("Disk optimization status table"));
    layout->addWidget(table);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, &dialog);
    OptimizeDialogAction selectedAction = OptimizeDialogAction::None;
    if (spec.can_queue) {
        auto* retrimButton = buttons->addButton(QObject::tr("Queue ReTrim"),
                                                QDialogButtonBox::ActionRole);
        retrimButton->setAccessibleName(QObject::tr("Queue SSD ReTrim"));
        retrimButton->setEnabled(spec.ssd_mode);
        QObject::connect(retrimButton, &QPushButton::clicked, [&]() {
            selectedAction = OptimizeDialogAction::ReTrim;
            dialog.accept();
        });

        auto* defragButton = buttons->addButton(QObject::tr("Queue Defrag"),
                                                QDialogButtonBox::ActionRole);
        defragButton->setAccessibleName(QObject::tr("Queue HDD defrag"));
        defragButton->setEnabled(spec.hdd_mode);
        QObject::connect(defragButton, &QPushButton::clicked, [&]() {
            selectedAction = OptimizeDialogAction::Defrag;
            dialog.accept();
        });
    }
    auto* copyButton = buttons->addButton(QObject::tr("Copy Optimize Commands"),
                                          QDialogButtonBox::ActionRole);
    copyButton->setAccessibleName(QObject::tr("Copy Optimize commands"));
    auto* openButton = buttons->addButton(QObject::tr("Open Windows Optimize Drives"),
                                          QDialogButtonBox::ActionRole);
    openButton->setAccessibleName(QObject::tr("Open Windows Optimize Drives"));
    QObject::connect(copyButton, &QPushButton::clicked, [commands = spec.command_lines]() {
        QApplication::clipboard()->setText(commands.join('\n'));
    });
    QObject::connect(openButton, &QPushButton::clicked, []() {
        QProcess::startDetached(QStringLiteral("dfrgui.exe"));
    });
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addWidget(buttons);
    dialog.exec();
    return selectedAction;
}

bool diskLooksSecureEraseCapable(const PartitionDiskInfo* disk) {
    return diskLooksSsd(disk);
}

QString secureEraseDeviceClass(const PartitionDiskInfo* disk) {
    if (!disk) {
        return QObject::tr("No disk selected");
    }
    const QString bus = disk->bus_type.toUpper();
    if (bus.contains(QStringLiteral("NVME"))) {
        return QObject::tr("NVMe SSD");
    }
    if (diskLooksSsd(disk)) {
        return QObject::tr("ATA/SATA SSD");
    }
    return QObject::tr("Not SSD/NVMe");
}

QString secureEraseGateText(const PartitionDiskInfo* disk) {
    if (!disk) {
        return QObject::tr("Select a disk to review SSD Secure Erase readiness");
    }
    if (disk->is_system) {
        return QObject::tr("Blocked: current system disk secure erase is not allowed");
    }
    if (!diskLooksSecureEraseCapable(disk)) {
        return QObject::tr("Blocked: selected disk is not reported as SSD/NVMe");
    }
    return QObject::tr("Ready: queue ReTrim plus clear-level wipe through Apply");
}

bool canQueueSsdSecureErase(const PartitionDiskInfo* disk) {
    return disk != nullptr && diskLooksSecureEraseCapable(disk) && !disk->is_system &&
           !disk->is_boot && !disk->is_read_only && !disk->is_dynamic && !disk->is_storage_spaces;
}

QStringList secureEraseChecklist(const PartitionDiskInfo* disk) {
    const QString diskText = disk ? QObject::tr("Disk %1, model %2, serial %3")
                                        .arg(disk->disk_number)
                                        .arg(propertyValue(disk->model))
                                        .arg(propertyValue(disk->serial_number))
                                  : QObject::tr("No disk selected");
    return {QObject::tr("Target identity: %1").arg(diskText),
            QObject::tr("Disposable non-system SSD/NVMe media only"),
            QObject::tr("Record bus type, firmware, SMART health, and wear before erase"),
            QObject::tr(
                "Verify vendor ATA Secure Erase or NVMe Format/Sanitize support externally"),
            QObject::tr("Show purge warning and typed operator confirmation in evidence"),
            QObject::tr("Capture before/after layout and post-erase readback proof"),
            QObject::tr("Attach evidence to external.ssd-retrim or hardware wipe certification")};
}

QVector<PropertyRow> secureEraseRows(const PartitionDiskInfo* disk) {
    QVector<PropertyRow> rows;
    addProperty(&rows,
                QObject::tr("Target disk"),
                disk ? QString::number(disk->disk_number) : QString());
    addProperty(&rows, QObject::tr("Model"), disk ? disk->model : QString());
    addProperty(&rows, QObject::tr("Serial number"), disk ? disk->serial_number : QString());
    addProperty(&rows, QObject::tr("Bus type"), disk ? disk->bus_type : QString());
    addProperty(&rows, QObject::tr("Media type"), disk ? disk->media_type : QString());
    addProperty(&rows, QObject::tr("Device class"), secureEraseDeviceClass(disk));
    addProperty(&rows, QObject::tr("Secure erase status"), secureEraseGateText(disk));
    addProperty(&rows,
                QObject::tr("In-app ATA/NVMe purge"),
                QObject::tr("Uses Windows ReTrim followed by clear-level disk wipe"));
    addProperty(&rows, QObject::tr("Evidence checklist"), secureEraseChecklist(disk).join('\n'));
    return rows;
}

bool showSecureEraseDialog(QWidget* parent,
                           const QVector<PropertyRow>& rows,
                           const QStringList& checklist,
                           bool canQueue) {
    QDialog dialog(parent);
    dialog.setObjectName(QStringLiteral("partitionSecureEraseDialog"));
    dialog.setWindowTitle(QObject::tr("SSD Secure Erase"));
    dialog.setAccessibleName(QObject::tr("SSD Secure Erase readiness"));
    dialog.setMinimumSize(kSecureEraseDialogMinWidth, kSecureEraseDialogMinHeight);

    auto* layout = new QVBoxLayout(&dialog);
    layout->setContentsMargins(
        ui::kMarginMedium, ui::kMarginMedium, ui::kMarginMedium, ui::kMarginMedium);

    auto* table = createPropertiesTable(&dialog, rows);
    table->setObjectName(QStringLiteral("partitionSecureEraseStatusTable"));
    table->setAccessibleName(QObject::tr("SSD Secure Erase readiness table"));
    layout->addWidget(table);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, &dialog);
    auto* copyButton = buttons->addButton(QObject::tr("Copy Evidence Checklist"),
                                          QDialogButtonBox::ActionRole);
    copyButton->setAccessibleName(QObject::tr("Copy SSD Secure Erase evidence checklist"));
    auto* queueButton = buttons->addButton(QObject::tr("Queue ReTrim + Wipe"),
                                           QDialogButtonBox::AcceptRole);
    queueButton->setAccessibleName(QObject::tr("Queue SSD Secure Erase"));
    queueButton->setEnabled(canQueue);
    QObject::connect(copyButton, &QPushButton::clicked, [checklist]() {
        QApplication::clipboard()->setText(checklist.join('\n'));
    });
    QObject::connect(queueButton, &QPushButton::clicked, &dialog, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addWidget(buttons);
    return dialog.exec() == QDialog::Accepted;
}

QTableWidget* createSpaceAnalyzerTable(QWidget* parent,
                                       const QString& objectName,
                                       const QString& lastColumnTitle,
                                       bool pathActions) {
    auto* table = new QTableWidget(0, kSpaceAnalyzerColumnCount, parent);
    table->setObjectName(objectName);
    table->setAccessibleName(QObject::tr("Space analyzer results"));
    table->setHorizontalHeaderLabels({QObject::tr("Name"),
                                      QObject::tr("Type"),
                                      QObject::tr("Size"),
                                      QObject::tr("Percent"),
                                      lastColumnTitle});
    table->verticalHeader()->setVisible(false);
    table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table->setSelectionBehavior(QAbstractItemView::SelectRows);
    table->setAlternatingRowColors(true);
    table->horizontalHeader()->setStretchLastSection(true);
    if (pathActions) {
        table->setContextMenuPolicy(Qt::CustomContextMenu);
        QObject::connect(
            table,
            &QTableWidget::customContextMenuRequested,
            table,
            [table](const QPoint& position) {
                auto* current = table->item(table->currentRow(), SpaceAnalyzerColName);
                const QString path = current ? current->data(kSpaceAnalyzerPathRole).toString()
                                             : QString();
                if (path.isEmpty()) {
                    return;
                }

                QMenu menu(table);
                const auto actions = spaceAnalyzerContextActionNames();
                auto* open = menu.addAction(actions.at(kSpaceAnalyzerOpenActionIndex));
                auto* explore = menu.addAction(actions.at(kSpaceAnalyzerExploreActionIndex));
                auto* copy = menu.addAction(actions.at(kSpaceAnalyzerCopyActionIndex));
                QObject::connect(open, &QAction::triggered, table, [path]() {
                    QDesktopServices::openUrl(QUrl::fromLocalFile(path));
                });
                QObject::connect(explore, &QAction::triggered, table, [path]() {
                    const QFileInfo info(path);
                    if (info.isDir()) {
                        QDesktopServices::openUrl(QUrl::fromLocalFile(path));
                        return;
                    }
                    QProcess::startDetached(QStringLiteral("explorer.exe"),
                                            {QStringLiteral("/select,"),
                                             QDir::toNativeSeparators(path)});
                });
                QObject::connect(copy, &QAction::triggered, table, [path]() {
                    QApplication::clipboard()->setText(QDir::toNativeSeparators(path));
                });
                menu.exec(table->viewport()->mapToGlobal(position));
            });
    }
    return table;
}

void setSpaceAnalyzerRow(QTableWidget* table,
                         int row,
                         const SpaceAnalyzerEntry& entry,
                         uint64_t totalBytes) {
    auto* sizeItem = new QTableWidgetItem(formatPartitionBytes(entry.bytes));
    sizeItem->setData(Qt::UserRole, QVariant::fromValue<qulonglong>(entry.bytes));
    auto* nameItem = new QTableWidgetItem(entry.name);
    nameItem->setData(kSpaceAnalyzerPathRole, entry.path);
    table->setItem(row, SpaceAnalyzerColName, nameItem);
    table->setItem(row, SpaceAnalyzerColType, new QTableWidgetItem(entry.type));
    table->setItem(row, SpaceAnalyzerColSize, sizeItem);
    table->setItem(row,
                   SpaceAnalyzerColPercent,
                   new QTableWidgetItem(percentageText(entry.bytes, totalBytes)));
    table->setItem(row,
                   SpaceAnalyzerColPath,
                   new QTableWidgetItem(QDir::toNativeSeparators(entry.path)));
}

void populateSpaceAnalyzerTable(QTableWidget* table, const SpaceAnalyzerResult& result) {
    table->setSortingEnabled(false);
    table->setRowCount(result.entries.size());
    for (qsizetype row = 0; row < result.entries.size(); ++row) {
        setSpaceAnalyzerRow(
            table, static_cast<int>(row), result.entries.at(row), result.total_bytes);
    }
    table->resizeColumnsToContents();
    table->setSortingEnabled(true);
    table->sortItems(SpaceAnalyzerColSize, Qt::DescendingOrder);
}

void populateSpaceAnalyzerTabs(QTabWidget* tabs, const SpaceAnalyzerResult& result) {
    auto* tree = tabs->findChild<QTableWidget*>(QStringLiteral("partitionSpaceAnalyzerTreeTable"));
    auto* files = tabs->findChild<QTableWidget*>(QStringLiteral("partitionSpaceAnalyzerFileTable"));
    auto* types = tabs->findChild<QTableWidget*>(QStringLiteral("partitionSpaceAnalyzerTypeTable"));
    if (tree) {
        SpaceAnalyzerResult view;
        view.entries = result.entries;
        view.total_bytes = result.total_bytes;
        populateSpaceAnalyzerTable(tree, view);
    }
    if (files) {
        SpaceAnalyzerResult view;
        view.entries = result.largest_files;
        view.total_bytes = result.total_bytes;
        populateSpaceAnalyzerTable(files, view);
    }
    if (types) {
        SpaceAnalyzerResult view;
        view.entries = result.file_types;
        view.total_bytes = result.total_bytes;
        populateSpaceAnalyzerTable(types, view);
    }
}

QTabWidget* createSpaceAnalyzerTabs(QWidget* parent) {
    auto* tabs = new QTabWidget(parent);
    tabs->setObjectName(QStringLiteral("partitionSpaceAnalyzerTabs"));
    tabs->setAccessibleName(QObject::tr("Space analyzer result views"));

    const auto names = spaceAnalyzerViewNames();
    tabs->addTab(createSpaceAnalyzerTable(tabs,
                                          QStringLiteral("partitionSpaceAnalyzerTreeTable"),
                                          QObject::tr("Path"),
                                          true),
                 names.at(kSpaceAnalyzerTreeViewIndex));
    tabs->addTab(createSpaceAnalyzerTable(tabs,
                                          QStringLiteral("partitionSpaceAnalyzerFileTable"),
                                          QObject::tr("Path"),
                                          true),
                 names.at(kSpaceAnalyzerLargestFilesViewIndex));
    tabs->addTab(createSpaceAnalyzerTable(tabs,
                                          QStringLiteral("partitionSpaceAnalyzerTypeTable"),
                                          QObject::tr("Count"),
                                          false),
                 names.at(kSpaceAnalyzerFileTypesViewIndex));
    return tabs;
}

QString spaceAnalyzerStatusText(const QString& rootPath, const SpaceAnalyzerResult& result) {
    if (result.root_missing) {
        return QObject::tr("Volume path is not available: %1").arg(rootPath);
    }
    if (result.cancelled) {
        return QObject::tr("Scan cancelled. Partial total: %1 across %2 file(s).")
            .arg(formatPartitionBytes(result.total_bytes))
            .arg(result.scanned_entries);
    }
    return QObject::tr("Total scanned usage: %1 across %2 file(s), %3 file type(s).")
        .arg(formatPartitionBytes(result.total_bytes))
        .arg(result.scanned_entries)
        .arg(result.file_types.size());
}

void showSpaceAnalyzerDialog(QWidget* parent, const QString& rootPath, const QString& title) {
    QDialog dialog(parent);
    dialog.setObjectName(QStringLiteral("partitionSpaceAnalyzerDialog"));
    dialog.setWindowTitle(title);
    dialog.setAccessibleName(title);
    dialog.setMinimumSize(kSpaceAnalyzerDialogMinWidth, kSpaceAnalyzerDialogMinHeight);

    auto* layout = new QVBoxLayout(&dialog);
    layout->setContentsMargins(
        ui::kMarginMedium, ui::kMarginMedium, ui::kMarginMedium, ui::kMarginMedium);
    auto* status = new QLabel(
        QObject::tr("Scanning %1...")
            .arg(QDir::toNativeSeparators(rootPath).left(kSpaceAnalyzerPathPreviewChars)),
        &dialog);
    status->setAccessibleName(QObject::tr("Space analyzer status"));
    status->setWordWrap(true);
    layout->addWidget(status);

    auto* progress = new QProgressBar(&dialog);
    progress->setRange(0, 0);
    progress->setAccessibleName(QObject::tr("Space analyzer scan progress"));
    layout->addWidget(progress);

    auto* tabs = createSpaceAnalyzerTabs(&dialog);
    layout->addWidget(tabs, 1);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, &dialog);
    buttons->button(QDialogButtonBox::Close)->setText(QObject::tr("Cancel"));
    layout->addWidget(buttons);

    auto cancelFlag = std::make_shared<std::atomic_bool>(false);
    QFutureWatcher<SpaceAnalyzerResult> watcher(&dialog);
    QObject::connect(&dialog, &QDialog::finished, [&cancelFlag]() {
        cancelFlag->store(true, std::memory_order_relaxed);
    });
    QObject::connect(&watcher, &QFutureWatcher<SpaceAnalyzerResult>::finished, &dialog, [&]() {
        const auto result = watcher.result();
        populateSpaceAnalyzerTabs(tabs, result);
        status->setText(spaceAnalyzerStatusText(rootPath, result));
        progress->setVisible(false);
        buttons->button(QDialogButtonBox::Close)->setText(QObject::tr("Close"));
    });
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    watcher.setFuture(QtConcurrent::run(
        [rootPath, cancelFlag]() { return analyzeVolumeSpace(rootPath, cancelFlag); }));
    dialog.exec();
}

QTableWidget* createFileRecoveryTable(QWidget* parent) {
    auto* table = new QTableWidget(0, kFileRecoveryColumnCount, parent);
    table->setObjectName(QStringLiteral("partitionFileRecoveryCandidateTable"));
    table->setAccessibleName(QObject::tr("File recovery candidates"));
    table->setHorizontalHeaderLabels({QObject::tr("Recovered File"),
                                      QObject::tr("Format"),
                                      QObject::tr("Size"),
                                      QObject::tr("Offset")});
    table->verticalHeader()->setVisible(false);
    table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table->setSelectionBehavior(QAbstractItemView::SelectRows);
    table->setAlternatingRowColors(true);
    table->horizontalHeader()->setStretchLastSection(true);
    return table;
}

void populateFileRecoveryTable(QTableWidget* table,
                               const QVector<FileRecoveryCandidate>& candidates) {
    table->setRowCount(candidates.size());
    for (qsizetype row = 0; row < candidates.size(); ++row) {
        const auto& candidate = candidates.at(row);
        table->setItem(static_cast<int>(row),
                       FileRecoveryColName,
                       new QTableWidgetItem(candidate.id));
        table->setItem(static_cast<int>(row),
                       FileRecoveryColFormat,
                       new QTableWidgetItem(candidate.format));
        table->setItem(static_cast<int>(row),
                       FileRecoveryColSize,
                       new QTableWidgetItem(formatPartitionBytes(candidate.size_bytes)));
        table->setItem(static_cast<int>(row),
                       FileRecoveryColOffset,
                       new QTableWidgetItem(QString::number(candidate.offset_bytes)));
    }
    table->resizeColumnsToContents();
}

bool showFileRecoveryReviewDialog(QWidget* parent,
                                  const FileRecoveryScanResult& scan,
                                  const QString& sourcePath,
                                  const QString& destinationPath) {
    QDialog dialog(parent);
    dialog.setWindowTitle(QObject::tr("Data Recovery"));
    dialog.setAccessibleName(QObject::tr("Offline image data recovery review"));
    dialog.setMinimumSize(kFileRecoveryDialogMinWidth, kFileRecoveryDialogMinHeight);
    auto* layout = new QVBoxLayout(&dialog);
    auto* summary = new QLabel(
        QObject::tr("%1 recoverable file(s) found in %2. Restore destination: %3")
            .arg(scan.candidates.size())
            .arg(QDir::toNativeSeparators(sourcePath), QDir::toNativeSeparators(destinationPath)),
        &dialog);
    summary->setAccessibleName(QObject::tr("Data recovery summary"));
    summary->setWordWrap(true);
    layout->addWidget(summary);
    auto* table = createFileRecoveryTable(&dialog);
    populateFileRecoveryTable(table, scan.candidates);
    layout->addWidget(table, 1);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    buttons->button(QDialogButtonBox::Ok)->setText(QObject::tr("Restore"));
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addWidget(buttons);
    return dialog.exec() == QDialog::Accepted;
}

QString defaultRawRecoverySourcePath(const std::optional<PartitionTarget>& selected) {
    if (selected && !selected->drive_letter.isEmpty()) {
        return QStringLiteral("\\\\.\\%1:").arg(selected->drive_letter.left(1).toUpper());
    }
    return QStringLiteral("\\\\.\\D:");
}

QString selectDataRecoverySourcePath(QWidget* parent,
                                     const std::optional<PartitionTarget>& selected) {
    QMessageBox sourceDialog(parent);
    sourceDialog.setWindowTitle(QObject::tr("Data Recovery Source"));
    sourceDialog.setText(QObject::tr("Select the recovery source."));
    sourceDialog.setIcon(QMessageBox::Question);
    auto* imageButton = sourceDialog.addButton(QObject::tr("Image File"), QMessageBox::AcceptRole);
    auto* rawButton = sourceDialog.addButton(QObject::tr("Raw Path"), QMessageBox::ActionRole);
    sourceDialog.addButton(QMessageBox::Cancel);
    sourceDialog.exec();

    if (sourceDialog.clickedButton() == imageButton) {
        return QFileDialog::getOpenFileName(parent,
                                            QObject::tr("Data Recovery Source Image"),
                                            QString(),
                                            QObject::tr(
                                                "Disk images (*.img *.bin);;All files (*)"));
    }
    if (sourceDialog.clickedButton() != rawButton) {
        return {};
    }

    bool ok = false;
    const QString sourcePath = QInputDialog::getText(parent,
                                                     QObject::tr("Data Recovery Raw Source"),
                                                     QObject::tr("Source path:"),
                                                     QLineEdit::Normal,
                                                     defaultRawRecoverySourcePath(selected),
                                                     &ok)
                                   .trimmed();
    return ok ? sourcePath : QString();
}

QString targetIdentityText(const std::optional<PartitionTarget>& target,
                           const PartitionDiskInfo* disk,
                           const PartitionInfoEx* partition) {
    if (!target) {
        return QStringLiteral("No target selected");
    }
    if (target->kind == PartitionTargetKind::Unallocated) {
        return QStringLiteral("Disk %1 unallocated space\nOffset: %2\nSize: %3")
            .arg(target->disk_number)
            .arg(formatPartitionBytes(target->offset_bytes),
                 formatPartitionBytes(target->size_bytes));
    }
    if (partition) {
        const QString letter = partition->volume && !partition->volume->drive_letter.isEmpty()
                                   ? partition->volume->drive_letter + QStringLiteral(":")
                                   : QStringLiteral("(no drive letter)");
        const QString fs = partition->volume ? partition->volume->file_system
                                             : QStringLiteral("(no file system)");
        return QStringLiteral("Disk %1 Partition %2\n%3 %4\nSize: %5")
            .arg(partition->disk_number)
            .arg(partition->partition_number)
            .arg(letter, fs, formatPartitionBytes(partition->size_bytes));
    }
    if (disk) {
        return QStringLiteral("Disk %1\n%2\nStyle: %3\nSize: %4")
            .arg(disk->disk_number)
            .arg(disk->model, disk->partition_style, formatPartitionBytes(disk->size_bytes));
    }
    return QStringLiteral("Disk %1").arg(target->disk_number);
}

QString applyReviewText(const QVector<PartitionOperation>& operations,
                        const PartitionInventory& inventory) {
    QStringList lines;
    lines << QStringLiteral("Layout hash: %1")
                 .arg(inventory.layout_hash.left(kPartitionLayoutHashPreviewChars));
    lines << QStringLiteral("Operations: %1").arg(operations.size());
    lines << QString();
    for (qsizetype i = 0; i < operations.size(); ++i) {
        const auto& operation = operations.at(i);
        lines << QStringLiteral("%1. %2 [%3]")
                     .arg(i + 1)
                     .arg(operation.summary, toDisplayString(operation.risk));
        if (!operation.warnings.isEmpty()) {
            lines << QStringLiteral("   Warnings: %1")
                         .arg(operation.warnings.join(QStringLiteral("; ")));
        }
        if (!operation.blockers.isEmpty()) {
            lines << QStringLiteral("   Blockers: %1")
                         .arg(operation.blockers.join(QStringLiteral("; ")));
        }
    }
    return lines.join(QLatin1Char('\n'));
}

struct PartitionWizardResult {
    QString target_path;
    QString layout_mode;
    QString verify_mode;
    QString scan_mode;
    uint64_t source_size_bytes{0};
    uint64_t target_size_bytes{0};
    uint32_t target_disk_number{0};
    uint64_t target_offset_bytes{0};
    uint64_t recovery_offset_bytes{0};
    uint64_t recovery_size_bytes{0};
    QString recovery_type_id;
    bool align_1mb{true};
    bool target_gpt{false};
    bool target_wipe_confirmed{false};
    bool target_region_selected{false};
    bool recovery_restore_requested{false};
    bool recovery_restore_acknowledged{false};
};

class ConfirmationWizardPage final : public QWizardPage {
public:
    explicit ConfirmationWizardPage(QWidget* parent = nullptr) : QWizardPage(parent) {}

    void setConfirmationBox(QCheckBox* box) {
        m_box = box;
        connect(m_box, &QCheckBox::toggled, this, &QWizardPage::completeChanged);
    }

    [[nodiscard]] bool isComplete() const override {
        return QWizardPage::isComplete() && m_box != nullptr && m_box->isChecked();
    }

private:
    QCheckBox* m_box{nullptr};
};

struct WizardReviewSpec {
    QString operation;
    QString source;
    QString target;
    QString mode;
    QString verify;
    QString sizing;
};

struct DiskCopyWizardConfig {
    QWidget* parent{nullptr};
    QString title;
    QString source_identity;
    const PartitionInventory* inventory{nullptr};
    uint32_t source_disk{0};
    bool os_migration{false};
};

struct DiskCopyWizardWidgets {
    QComboBox* target_disk{nullptr};
    QLineEdit* custom_target{nullptr};
    QComboBox* layout_mode{nullptr};
    QComboBox* verify_mode{nullptr};
    QCheckBox* align{nullptr};
    QCheckBox* target_gpt{nullptr};
    QCheckBox* target_wipe{nullptr};
    CloneLayoutPreviewWidget* layout_preview{nullptr};
    QLabel* review{nullptr};
};

struct PartitionCopyWizardWidgets {
    QComboBox* target_region{nullptr};
    QLineEdit* target_path{nullptr};
    QComboBox* verify_mode{nullptr};
    QCheckBox* target_wipe{nullptr};
    OperationSizePreviewWidget* size_preview{nullptr};
    QLabel* review{nullptr};
};

struct RecoveryWizardWidgets {
    QComboBox* scan_mode{nullptr};
    QCheckBox* restore_candidate{nullptr};
    QLineEdit* offset_bytes{nullptr};
    QLineEdit* size_bytes{nullptr};
    QLineEdit* type_id{nullptr};
    QCheckBox* acknowledge{nullptr};
    QLabel* review{nullptr};
};

void addSourcePage(QWizard* wizard, const QString& title, const QString& sourceText) {
    auto* page = new QWizardPage(wizard);
    page->setTitle(title);
    auto* layout = new QVBoxLayout(page);
    auto* source = new QLabel(sourceText, page);
    source->setWordWrap(true);
    source->setAccessibleName(QStringLiteral("Wizard source target"));
    layout->addWidget(source);
    wizard->addPage(page);
}

QString physicalDrivePath(uint32_t diskNumber) {
    return QStringLiteral("\\\\.\\PhysicalDrive%1").arg(diskNumber);
}

const PartitionDiskInfo* findWizardDisk(const PartitionInventory& inventory, uint32_t sourceDisk) {
    for (const auto& disk : inventory.disks) {
        if (disk.disk_number == sourceDisk) {
            return &disk;
        }
    }
    return nullptr;
}

uint64_t sourceDiskSize(const PartitionInventory& inventory, uint32_t sourceDisk) {
    const auto* disk = findWizardDisk(inventory, sourceDisk);
    return disk ? disk->size_bytes : 0;
}

uint64_t parseWizardUInt64(const QLineEdit* edit) {
    bool ok = false;
    const uint64_t value = edit->text().trimmed().toULongLong(&ok);
    return ok ? value : 0;
}

QComboBox* addTargetDiskSelector(QFormLayout* form,
                                 QWidget* parent,
                                 const PartitionInventory& inventory,
                                 uint32_t sourceDisk) {
    auto* combo = new QComboBox(parent);
    combo->setAccessibleName(QStringLiteral("Target disk"));
    for (const auto& disk : inventory.disks) {
        if (disk.disk_number == sourceDisk) {
            continue;
        }
        combo->addItem(QStringLiteral("Disk %1 - %2 - %3")
                           .arg(disk.disk_number)
                           .arg(disk.model, formatPartitionBytes(disk.size_bytes)),
                       physicalDrivePath(disk.disk_number));
        combo->setItemData(combo->count() - 1,
                           QVariant::fromValue<qulonglong>(disk.size_bytes),
                           kWizardDiskSizeRole);
    }
    combo->addItem(QStringLiteral("Custom path"), QString());
    form->addRow(QStringLiteral("Target disk:"), combo);
    return combo;
}

void syncWizardReview(QLabel* review, const WizardReviewSpec& spec) {
    review->setText(
        QStringLiteral("%1\nSource: %2\nTarget: %3\nLayout: %4\nVerify: %5\n%6")
            .arg(spec.operation, spec.source, spec.target, spec.mode, spec.verify, spec.sizing));
}

void addDiskTargetPage(QWizard* wizard,
                       const DiskCopyWizardConfig& config,
                       DiskCopyWizardWidgets* widgets) {
    auto* targetPage = new QWizardPage(wizard);
    targetPage->setTitle(QStringLiteral("Target Disk"));
    auto* targetLayout = new QFormLayout(targetPage);
    widgets->target_disk =
        addTargetDiskSelector(targetLayout, targetPage, *config.inventory, config.source_disk);
    widgets->custom_target = new QLineEdit(targetPage);
    widgets->custom_target->setAccessibleName(QStringLiteral("Custom target path"));
    targetLayout->addRow(QStringLiteral("Custom path:"), widgets->custom_target);
    wizard->addPage(targetPage);
}

void addDiskOptionsPage(QWizard* wizard,
                        const DiskCopyWizardConfig& config,
                        DiskCopyWizardWidgets* widgets) {
    auto* optionsPage = new QWizardPage(wizard);
    optionsPage->setTitle(QStringLiteral("Options"));
    auto* optionsLayout = new QFormLayout(optionsPage);
    widgets->layout_mode = new QComboBox(optionsPage);
    widgets->layout_mode->addItems({QStringLiteral("Keep original layout"),
                                    QStringLiteral("Fit partitions to target"),
                                    QStringLiteral("Edit layout after clone")});
    widgets->layout_mode->setAccessibleName(QStringLiteral("Layout mode"));
    widgets->verify_mode = new QComboBox(optionsPage);
    widgets->verify_mode->addItems(
        {QStringLiteral("Sample verification"), QStringLiteral("Full verification")});
    widgets->verify_mode->setAccessibleName(QStringLiteral("Verification mode"));
    widgets->align = new QCheckBox(QStringLiteral("Align partitions to 1 MB"), optionsPage);
    widgets->align->setChecked(true);
    widgets->align->setAccessibleName(QStringLiteral("Align partitions to one megabyte"));
    widgets->target_gpt = new QCheckBox(QStringLiteral("Prefer GPT target boot layout"),
                                        optionsPage);
    widgets->target_gpt->setChecked(config.os_migration);
    widgets->target_gpt->setAccessibleName(QStringLiteral("Prefer GPT target boot layout"));
    optionsLayout->addRow(QStringLiteral("Layout:"), widgets->layout_mode);
    optionsLayout->addRow(QStringLiteral("Verify:"), widgets->verify_mode);
    optionsLayout->addRow(QString(), widgets->align);
    if (config.os_migration) {
        optionsLayout->addRow(QString(), widgets->target_gpt);
    }
    wizard->addPage(optionsPage);
}

void addDiskReviewPage(QWizard* wizard, DiskCopyWizardWidgets* widgets) {
    auto* reviewPage = new ConfirmationWizardPage(wizard);
    reviewPage->setTitle(QStringLiteral("Review"));
    auto* reviewLayout = new QVBoxLayout(reviewPage);
    widgets->layout_preview = new CloneLayoutPreviewWidget(reviewPage);
    reviewLayout->addWidget(widgets->layout_preview);
    widgets->review = new QLabel(reviewPage);
    widgets->review->setWordWrap(true);
    widgets->review->setAccessibleName(QStringLiteral("Wizard review"));
    reviewLayout->addWidget(widgets->review);
    widgets->target_wipe = new QCheckBox(
        QStringLiteral("I understand the selected target disk will be overwritten."), reviewPage);
    widgets->target_wipe->setAccessibleName(QStringLiteral("Confirm target disk overwrite"));
    reviewLayout->addWidget(widgets->target_wipe);
    reviewPage->setConfirmationBox(widgets->target_wipe);
    wizard->addPage(reviewPage);
}

QString selectedDiskWizardTarget(const DiskCopyWizardWidgets& widgets) {
    const QString selectedTarget = widgets.target_disk->currentData().toString();
    return selectedTarget.isEmpty() ? widgets.custom_target->text() : selectedTarget;
}

uint64_t selectedDiskWizardTargetSize(const DiskCopyWizardWidgets& widgets) {
    return widgets.target_disk->currentData(kWizardDiskSizeRole).toULongLong();
}

QString diskWizardSizingText(const DiskCopyWizardConfig& config,
                             const DiskCopyWizardWidgets& widgets) {
    const uint64_t sourceSize = sourceDiskSize(*config.inventory, config.source_disk);
    const uint64_t targetSize = selectedDiskWizardTargetSize(widgets);
    QString text = QStringLiteral("Source size: %1\nTarget size: %2")
                       .arg(formatPartitionBytes(sourceSize),
                            targetSize == 0 ? QStringLiteral("custom/unknown")
                                            : formatPartitionBytes(targetSize));
    if (targetSize != 0 && sourceSize > targetSize) {
        text += QStringLiteral("\nBlocked: target disk is smaller than source disk.");
    }
    return text;
}

void updateDiskWizardReview(const DiskCopyWizardConfig& config,
                            const DiskCopyWizardWidgets& widgets) {
    widgets.layout_preview->configure(findWizardDisk(*config.inventory, config.source_disk),
                                      selectedDiskWizardTargetSize(widgets),
                                      widgets.layout_mode->currentText());
    syncWizardReview(widgets.review,
                     {config.title,
                      physicalDrivePath(config.source_disk),
                      selectedDiskWizardTarget(widgets),
                      widgets.layout_mode->currentText(),
                      widgets.verify_mode->currentText(),
                      diskWizardSizingText(config, widgets)});
}

void connectDiskWizardReviewSignals(QWizard* wizard,
                                    const DiskCopyWizardConfig& config,
                                    DiskCopyWizardWidgets* widgets) {
    const auto updateReview = [config, widgets]() {
        updateDiskWizardReview(config, *widgets);
    };
    QObject::connect(widgets->target_disk,
                     qOverload<int>(&QComboBox::currentIndexChanged),
                     wizard,
                     updateReview);
    QObject::connect(widgets->custom_target, &QLineEdit::textChanged, wizard, updateReview);
    QObject::connect(widgets->layout_mode, &QComboBox::currentTextChanged, wizard, updateReview);
    QObject::connect(widgets->verify_mode, &QComboBox::currentTextChanged, wizard, updateReview);
    updateReview();
}

PartitionWizardResult diskCopyWizardResult(const DiskCopyWizardConfig& config,
                                           const DiskCopyWizardWidgets& widgets) {
    PartitionWizardResult result;
    result.target_path = selectedDiskWizardTarget(widgets);
    result.layout_mode = widgets.layout_mode->currentText();
    result.verify_mode = widgets.verify_mode->currentText();
    result.source_size_bytes = sourceDiskSize(*config.inventory, config.source_disk);
    result.target_size_bytes = selectedDiskWizardTargetSize(widgets);
    result.align_1mb = widgets.align->isChecked();
    result.target_gpt = widgets.target_gpt->isChecked();
    result.target_wipe_confirmed = widgets.target_wipe->isChecked();
    return result;
}

std::optional<PartitionWizardResult> runDiskCopyWizard(const DiskCopyWizardConfig& config) {
    QWizard wizard(config.parent);
    wizard.setWindowTitle(config.title);
    wizard.setAccessibleName(config.title);
    addSourcePage(&wizard, QStringLiteral("Source"), config.source_identity);

    DiskCopyWizardWidgets widgets;
    addDiskTargetPage(&wizard, config, &widgets);
    addDiskOptionsPage(&wizard, config, &widgets);
    addDiskReviewPage(&wizard, &widgets);
    connectDiskWizardReviewSignals(&wizard, config, &widgets);

    if (wizard.exec() != QDialog::Accepted) {
        return std::nullopt;
    }
    const PartitionWizardResult result = diskCopyWizardResult(config, widgets);
    return result.target_path.isEmpty() ? std::nullopt : std::make_optional(result);
}

bool partitionRegionSelected(const PartitionCopyWizardWidgets& widgets) {
    return widgets.target_region != nullptr && widgets.target_region->currentIndex() > 0;
}

uint32_t selectedPartitionRegionDisk(const PartitionCopyWizardWidgets& widgets) {
    return static_cast<uint32_t>(
        widgets.target_region->currentData(kWizardRegionDiskRole).toULongLong());
}

uint64_t selectedPartitionRegionOffset(const PartitionCopyWizardWidgets& widgets) {
    return widgets.target_region->currentData(kWizardRegionOffsetRole).toULongLong();
}

uint64_t selectedPartitionRegionSize(const PartitionCopyWizardWidgets& widgets) {
    return partitionRegionSelected(widgets)
               ? widgets.target_region->currentData(kWizardRegionSizeRole).toULongLong()
               : 0;
}

void addPartitionRegionChoice(QComboBox* combo, const UnallocatedRegion& region) {
    combo->addItem(QStringLiteral("Disk %1 unallocated - %2 at %3")
                       .arg(region.disk_number)
                       .arg(formatPartitionBytes(region.size_bytes),
                            formatPartitionBytes(region.offset_bytes)));
    const int index = combo->count() - 1;
    combo->setItemData(index,
                       QVariant::fromValue<qulonglong>(region.disk_number),
                       kWizardRegionDiskRole);
    combo->setItemData(index,
                       QVariant::fromValue<qulonglong>(region.offset_bytes),
                       kWizardRegionOffsetRole);
    combo->setItemData(index,
                       QVariant::fromValue<qulonglong>(region.size_bytes),
                       kWizardRegionSizeRole);
}

void addPartitionTargetPage(QWizard* wizard,
                            const PartitionInventory& inventory,
                            PartitionCopyWizardWidgets* widgets) {
    auto* targetPage = new QWizardPage(wizard);
    targetPage->setTitle(QStringLiteral("Target"));
    auto* targetLayout = new QFormLayout(targetPage);
    widgets->target_region = new QComboBox(targetPage);
    widgets->target_region->setAccessibleName(QStringLiteral("Target unallocated region"));
    widgets->target_region->addItem(QStringLiteral("Image or custom volume/device path"));
    for (const auto& disk : inventory.disks) {
        for (const auto& region : disk.unallocated_regions) {
            addPartitionRegionChoice(widgets->target_region, region);
        }
    }
    widgets->target_path = new QLineEdit(targetPage);
    widgets->target_path->setAccessibleName(QStringLiteral("Target image or volume path"));
    widgets->size_preview = new OperationSizePreviewWidget(targetPage);
    targetLayout->addRow(QStringLiteral("Target region:"), widgets->target_region);
    targetLayout->addRow(QStringLiteral("Target path:"), widgets->target_path);
    targetLayout->addRow(QString(), widgets->size_preview);
    wizard->addPage(targetPage);
}

void addPartitionCopyOptionsPage(QWizard* wizard, PartitionCopyWizardWidgets* widgets) {
    auto* optionsPage = new QWizardPage(wizard);
    optionsPage->setTitle(QStringLiteral("Options"));
    auto* optionsLayout = new QFormLayout(optionsPage);
    widgets->verify_mode = new QComboBox(optionsPage);
    widgets->verify_mode->addItems(
        {QStringLiteral("Sample verification"), QStringLiteral("Full verification")});
    widgets->verify_mode->setAccessibleName(QStringLiteral("Verification mode"));
    optionsLayout->addRow(QStringLiteral("Verify:"), widgets->verify_mode);
    wizard->addPage(optionsPage);
}

void addPartitionCopyReviewPage(QWizard* wizard, PartitionCopyWizardWidgets* widgets) {
    auto* reviewPage = new ConfirmationWizardPage(wizard);
    reviewPage->setTitle(QStringLiteral("Review"));
    auto* reviewLayout = new QVBoxLayout(reviewPage);
    widgets->review = new QLabel(reviewPage);
    widgets->review->setWordWrap(true);
    widgets->review->setAccessibleName(QStringLiteral("Wizard review"));
    reviewLayout->addWidget(widgets->review);
    widgets->target_wipe = new QCheckBox(
        QStringLiteral("I understand the selected target path or region will be overwritten."),
        reviewPage);
    widgets->target_wipe->setAccessibleName(
        QStringLiteral("Confirm partition clone target overwrite"));
    reviewLayout->addWidget(widgets->target_wipe);
    reviewPage->setConfirmationBox(widgets->target_wipe);
    wizard->addPage(reviewPage);
}

QString selectedPartitionCopyTargetPath(const PartitionCopyWizardWidgets& widgets) {
    if (partitionRegionSelected(widgets)) {
        return physicalDrivePath(selectedPartitionRegionDisk(widgets));
    }
    return widgets.target_path->text().trimmed();
}

bool partitionCopyTargetsRawDevice(const PartitionCopyWizardWidgets& widgets) {
    return selectedPartitionCopyTargetPath(widgets).startsWith(QStringLiteral("\\\\.\\"),
                                                               Qt::CaseInsensitive);
}

void syncPartitionCopyTargetPath(const PartitionCopyWizardWidgets& widgets) {
    if (partitionRegionSelected(widgets)) {
        widgets.target_path->setText(physicalDrivePath(selectedPartitionRegionDisk(widgets)));
        widgets.target_path->setReadOnly(true);
        return;
    }
    widgets.target_path->setReadOnly(false);
}

QString partitionCopySizingText(const PartitionCopyWizardWidgets& widgets, uint64_t sourceSize) {
    const uint64_t targetSize = selectedPartitionRegionSize(widgets);
    QString text = QStringLiteral("Source size: %1\nTarget size: %2")
                       .arg(formatPartitionBytes(sourceSize),
                            targetSize == 0 ? QStringLiteral("custom/unknown")
                                            : formatPartitionBytes(targetSize));
    if (partitionRegionSelected(widgets)) {
        text += QStringLiteral("\nTarget offset: %1")
                    .arg(formatPartitionBytes(selectedPartitionRegionOffset(widgets)));
    }
    if (targetSize != 0 && sourceSize > targetSize) {
        text += QStringLiteral("\nBlocked: target region is smaller than source partition.");
    }
    if (partitionCopyTargetsRawDevice(widgets)) {
        text += QStringLiteral(
            "\nRaw device and target-region writes require target identity review and "
            "overwrite confirmation.");
    }
    return text;
}

void updatePartitionCopyPreview(const QString& sourcePath,
                                uint64_t sourceSize,
                                const PartitionCopyWizardWidgets& widgets) {
    syncPartitionCopyTargetPath(widgets);
    const uint64_t targetSize = selectedPartitionRegionSize(widgets);
    const uint64_t scaleBytes = std::max(sourceSize, targetSize);
    const QColor targetColor = targetSize != 0 && sourceSize > targetSize
                                   ? QColor(QString::fromLatin1(ui::kColorError))
                                   : QColor(QString::fromLatin1(ui::kColorSuccess));
    widgets.size_preview->setRows(
        {{QObject::tr("Source"),
          scaleBytes,
          sourceSize,
          QColor(QString::fromLatin1(ui::kColorPrimary)),
          formatPartitionBytes(sourceSize)},
         {QObject::tr("Target"),
          scaleBytes,
          targetSize,
          targetColor,
          targetSize == 0 ? QObject::tr("custom/unknown") : formatPartitionBytes(targetSize)}});
    syncWizardReview(widgets.review,
                     {QStringLiteral("Copy Partition Wizard"),
                      sourcePath,
                      selectedPartitionCopyTargetPath(widgets),
                      QStringLiteral("Partition copy"),
                      widgets.verify_mode->currentText(),
                      partitionCopySizingText(widgets, sourceSize)});
}

void connectPartitionCopyWizardSignals(QWizard* wizard,
                                       const QString& sourcePath,
                                       uint64_t sourceSize,
                                       PartitionCopyWizardWidgets* widgets) {
    const auto updateReview = [sourcePath, sourceSize, widgets]() {
        updatePartitionCopyPreview(sourcePath, sourceSize, *widgets);
    };
    QObject::connect(widgets->target_region,
                     qOverload<int>(&QComboBox::currentIndexChanged),
                     wizard,
                     updateReview);
    QObject::connect(widgets->target_path, &QLineEdit::textChanged, wizard, updateReview);
    QObject::connect(widgets->verify_mode, &QComboBox::currentTextChanged, wizard, updateReview);
    updateReview();
}

PartitionWizardResult partitionCopyWizardResult(const PartitionCopyWizardWidgets& widgets,
                                                uint64_t sourceSize) {
    PartitionWizardResult result;
    result.target_path = selectedPartitionCopyTargetPath(widgets);
    result.verify_mode = widgets.verify_mode->currentText();
    result.source_size_bytes = sourceSize;
    result.target_size_bytes = selectedPartitionRegionSize(widgets);
    result.target_wipe_confirmed = widgets.target_wipe->isChecked();
    result.target_region_selected = partitionRegionSelected(widgets);
    if (result.target_region_selected) {
        result.target_disk_number = selectedPartitionRegionDisk(widgets);
        result.target_offset_bytes = selectedPartitionRegionOffset(widgets);
    }
    return result;
}

std::optional<PartitionWizardResult> runPartitionCopyWizard(QWidget* parent,
                                                            const QString& sourceIdentity,
                                                            const QString& sourcePath,
                                                            const PartitionInventory& inventory,
                                                            uint64_t sourceSize) {
    QWizard wizard(parent);
    wizard.setWindowTitle(QStringLiteral("Copy Partition Wizard"));
    wizard.setAccessibleName(QStringLiteral("Copy Partition Wizard"));
    addSourcePage(&wizard, QStringLiteral("Source Partition"), sourceIdentity);

    PartitionCopyWizardWidgets widgets;
    addPartitionTargetPage(&wizard, inventory, &widgets);
    addPartitionCopyOptionsPage(&wizard, &widgets);
    addPartitionCopyReviewPage(&wizard, &widgets);
    connectPartitionCopyWizardSignals(&wizard, sourcePath, sourceSize, &widgets);

    if (wizard.exec() != QDialog::Accepted) {
        return std::nullopt;
    }
    const PartitionWizardResult result = partitionCopyWizardResult(widgets, sourceSize);
    return result.target_path.isEmpty() ? std::nullopt : std::make_optional(result);
}

void addRecoveryScanPage(QWizard* wizard, RecoveryWizardWidgets* widgets) {
    auto* scanPage = new QWizardPage(wizard);
    scanPage->setTitle(QStringLiteral("Scan Mode"));
    auto* scanLayout = new QFormLayout(scanPage);
    widgets->scan_mode = new QComboBox(scanPage);
    widgets->scan_mode->addItems({QStringLiteral("Quick"), QStringLiteral("Full")});
    widgets->scan_mode->setAccessibleName(QStringLiteral("Partition recovery scan mode"));
    scanLayout->addRow(QStringLiteral("Mode:"), widgets->scan_mode);
    wizard->addPage(scanPage);
}

void addRecoveryCandidatePage(QWizard* wizard, RecoveryWizardWidgets* widgets) {
    auto* page = new QWizardPage(wizard);
    page->setTitle(QStringLiteral("Candidate Restore"));
    auto* layout = new QFormLayout(page);
    widgets->restore_candidate =
        new QCheckBox(QStringLiteral("Queue recovered partition write-back candidate"), page);
    widgets->restore_candidate->setAccessibleName(
        QStringLiteral("Queue recovered partition write-back candidate"));
    widgets->offset_bytes = new QLineEdit(page);
    widgets->offset_bytes->setAccessibleName(QStringLiteral("Recovered partition offset in bytes"));
    widgets->size_bytes = new QLineEdit(page);
    widgets->size_bytes->setAccessibleName(QStringLiteral("Recovered partition size in bytes"));
    widgets->type_id = new QLineEdit(page);
    widgets->type_id->setAccessibleName(QStringLiteral("Recovered partition type ID"));
    widgets->acknowledge = new QCheckBox(
        QStringLiteral(
            "I understand the recovered partition candidate will be written to the selected disk."),
        page);
    widgets->acknowledge->setAccessibleName(
        QStringLiteral("Acknowledge recovered partition write-back"));
    layout->addRow(QString(), widgets->restore_candidate);
    layout->addRow(QStringLiteral("Offset bytes:"), widgets->offset_bytes);
    layout->addRow(QStringLiteral("Size bytes:"), widgets->size_bytes);
    layout->addRow(QStringLiteral("Type ID:"), widgets->type_id);
    layout->addRow(QString(), widgets->acknowledge);
    wizard->addPage(page);
}

void updateRecoveryWizardReview(const RecoveryWizardWidgets& widgets) {
    QString text = QStringLiteral("Scan selected disk for lost partition boot sectors.\nMode: %1")
                       .arg(widgets.scan_mode->currentText());
    if (widgets.restore_candidate->isChecked()) {
        text += QStringLiteral(
                    "\nWrite-back candidate: offset %1, size %2, type %3\nApply will create "
                    "the candidate partition after overlap and disk-bound checks.")
                    .arg(widgets.offset_bytes->text(),
                         widgets.size_bytes->text(),
                         widgets.type_id->text());
    }
    widgets.review->setText(text);
}

void addRecoveryReviewPage(QWizard* wizard, RecoveryWizardWidgets* widgets) {
    auto* reviewPage = new QWizardPage(wizard);
    reviewPage->setTitle(QStringLiteral("Review"));
    auto* reviewLayout = new QVBoxLayout(reviewPage);
    widgets->review = new QLabel(reviewPage);
    widgets->review->setWordWrap(true);
    widgets->review->setAccessibleName(QStringLiteral("Wizard review"));
    reviewLayout->addWidget(widgets->review);
    wizard->addPage(reviewPage);
}

void connectRecoveryWizardSignals(QWizard* wizard, RecoveryWizardWidgets* widgets) {
    const auto updateReview = [widgets]() {
        updateRecoveryWizardReview(*widgets);
    };
    QObject::connect(widgets->scan_mode, &QComboBox::currentTextChanged, wizard, updateReview);
    QObject::connect(widgets->restore_candidate, &QCheckBox::toggled, wizard, updateReview);
    QObject::connect(widgets->offset_bytes, &QLineEdit::textChanged, wizard, updateReview);
    QObject::connect(widgets->size_bytes, &QLineEdit::textChanged, wizard, updateReview);
    QObject::connect(widgets->type_id, &QLineEdit::textChanged, wizard, updateReview);
    updateReview();
}

PartitionWizardResult recoveryWizardResult(const RecoveryWizardWidgets& widgets) {
    PartitionWizardResult result;
    result.scan_mode = widgets.scan_mode->currentText();
    result.recovery_restore_requested = widgets.restore_candidate->isChecked();
    result.recovery_offset_bytes = parseWizardUInt64(widgets.offset_bytes);
    result.recovery_size_bytes = parseWizardUInt64(widgets.size_bytes);
    result.recovery_type_id = widgets.type_id->text().trimmed();
    result.recovery_restore_acknowledged = widgets.acknowledge->isChecked();
    return result;
}

std::optional<PartitionWizardResult> runRecoveryWizard(QWidget* parent,
                                                       const QString& sourceIdentity) {
    QWizard wizard(parent);
    wizard.setWindowTitle(QStringLiteral("Partition Recovery Wizard"));
    wizard.setAccessibleName(QStringLiteral("Partition Recovery Wizard"));
    addSourcePage(&wizard, QStringLiteral("Source Disk"), sourceIdentity);
    RecoveryWizardWidgets widgets;
    addRecoveryScanPage(&wizard, &widgets);
    addRecoveryCandidatePage(&wizard, &widgets);
    addRecoveryReviewPage(&wizard, &widgets);
    connectRecoveryWizardSignals(&wizard, &widgets);
    if (wizard.exec() != QDialog::Accepted) {
        return std::nullopt;
    }
    return recoveryWizardResult(widgets);
}

}  // namespace

PartitionManagerPanel::PartitionManagerPanel(QWidget* parent)
    : QWidget(parent), m_controller(std::make_unique<PartitionManagerController>(this)) {
    setupUi();
    connectController();
    updateRefreshButtonState();
    updateActionState();
}

#ifdef SAK_PARTITION_MANAGER_PANEL_TEST_HOOKS
QJsonObject partitionManagerAnalyzeSpaceForTest(const QString& rootPath) {
    const auto result = analyzeVolumeSpace(rootPath, std::make_shared<std::atomic_bool>(false));
    QJsonArray viewNames;
    for (const auto& name : spaceAnalyzerViewNames()) {
        viewNames.append(name);
    }
    QJsonArray contextActions;
    for (const auto& action : spaceAnalyzerContextActionNames()) {
        contextActions.append(action);
    }
    QJsonArray fileTypes;
    for (const auto& entry : result.file_types) {
        QJsonObject row;
        row.insert(QStringLiteral("name"), entry.name);
        row.insert(QStringLiteral("bytes"), QString::number(entry.bytes));
        row.insert(QStringLiteral("count"), entry.path);
        fileTypes.append(row);
    }
    return QJsonObject{
        {QStringLiteral("root_missing"), result.root_missing},
        {QStringLiteral("top_level_count"), result.entries.size()},
        {QStringLiteral("largest_file_count"), result.largest_files.size()},
        {QStringLiteral("file_type_count"), result.file_types.size()},
        {QStringLiteral("total_bytes"), QString::number(result.total_bytes)},
        {QStringLiteral("scanned_entries"), result.scanned_entries},
        {QStringLiteral("view_names"), viewNames},
        {QStringLiteral("context_actions"), contextActions},
        {QStringLiteral("file_types"), fileTypes},
    };
}

void PartitionManagerPanel::setTestInventoryForReview(const PartitionInventory& inventory) {
    m_controller->setTestInventory(inventory);
}

void PartitionManagerPanel::queueTestOperationForReview(PartitionOperationType type,
                                                        const PartitionTarget& target) {
    m_controller->queueOperation(type, target);
}

bool PartitionManagerPanel::showApplyReviewDialogForTest() {
    return showApplyReviewDialog();
}
#endif

void PartitionManagerPanel::setupUi() {
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(ui::kMarginNone, ui::kMarginNone, ui::kMarginNone, ui::kMarginNone);
    root->setSpacing(ui::kSpacingNone);
    createRibbon(root);

    auto* body = new QWidget(this);
    body->setObjectName(QStringLiteral("partitionManagerBody"));
    auto* bodyLayout = new QHBoxLayout(body);
    bodyLayout->setContentsMargins(
        ui::kMarginNone, ui::kMarginNone, ui::kMarginNone, ui::kMarginNone);
    bodyLayout->setSpacing(ui::kSpacingNone);
    bodyLayout->addWidget(createActionsPane(), 0);
    bodyLayout->addWidget(createWorkspace(body), 1);
    root->addWidget(body, 1);
}

void PartitionManagerPanel::createRibbon(QVBoxLayout* root) {
    auto* ribbon = new QFrame(this);
    ribbon->setFrameShape(QFrame::StyledPanel);
    applyWindowFill(ribbon, lightenedPaletteColor(QPalette::Window));
    auto* layout = new QHBoxLayout(ribbon);
    layout->setContentsMargins(
        ui::kMarginSmall, ui::kMarginTight, ui::kMarginSmall, ui::kMarginTight);
    layout->setSpacing(ui::kSpacingSmall);

    m_applyButton =
        createRibbonButton(ribbon, tr("Apply"), kIconApply, tr("Apply pending operations"));
    m_cancelButton =
        createRibbonButton(ribbon, tr("Cancel"), kIconDiscard, tr("Cancel running operation"));
    m_discardButton =
        createRibbonButton(ribbon, tr("Discard"), kIconDiscard, tr("Discard pending operations"));
    m_undoButton =
        createRibbonButton(ribbon, tr("Undo"), kIconUndo, tr("Undo last queued operation"));
    m_redoButton =
        createRibbonButton(ribbon, tr("Redo"), kIconRedo, tr("Redo last undone operation"));
    m_refreshButton =
        createRibbonButton(ribbon, tr("Scan Disks"), kIconRefresh, tr("Scan disk inventory"));
    m_dryRunButton =
        createRibbonButton(ribbon, tr("Dry Run"), kIconDryRun, tr("Preview scripts only"));

    for (auto* button :
         {m_applyButton, m_cancelButton, m_discardButton, m_undoButton, m_redoButton}) {
        layout->addWidget(button);
    }
    auto* separator = new QFrame(ribbon);
    separator->setFrameShape(QFrame::VLine);
    layout->addWidget(separator);
    layout->addWidget(m_refreshButton);
    layout->addWidget(m_dryRunButton);
    layout->addStretch();

    root->addWidget(ribbon);
}

QWidget* PartitionManagerPanel::createActionsPane() {
    auto* pane = new QFrame(this);
    pane->setObjectName(QStringLiteral("partitionActionsPane"));
    pane->setFrameShape(QFrame::StyledPanel);
    pane->setFixedWidth(kActionsPaneWidth);
    pane->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Expanding);
    applyWindowFill(pane, QApplication::palette().color(QPalette::AlternateBase));
    auto* layout = new QVBoxLayout(pane);
    layout->setContentsMargins(
        ui::kMarginSmall, ui::kMarginSmall, ui::kMarginSmall, ui::kMarginSmall);
    layout->setSpacing(ui::kSpacingSmall);

    addActionsPaneTitle(layout, pane);
    addActionSpecSection(layout, pane, tr("Wizards"), wizardActionSpecs());
    addActionSpecSection(
        layout, pane, tr("Partition Operations"), partitionOperationActionSpecs(), true);
    addPendingOperationsSection(layout, pane);
    return pane;
}

void PartitionManagerPanel::addActionsPaneTitle(QVBoxLayout* layout, QWidget* pane) {
    auto* title = new QLabel(tr("Actions and Wizards"), pane);
    title->setAccessibleName(tr("Partition actions and wizards"));
    auto titleFont = title->font();
    titleFont.setBold(true);
    title->setFont(titleFont);
    layout->addWidget(title);
}

QToolButton* PartitionManagerPanel::createConfiguredActionLink(QWidget* parent,
                                                               const ActionLinkSpec& spec) {
    auto* button = createActionLink(parent, spec.text, spec.icon_path, spec.tooltip);
    if (spec.slot) {
        connect(button, &QToolButton::clicked, this, spec.slot);
    }
    button->setProperty(kActionTargetKindsProperty, spec.options.target_kinds);
    if (spec.options.requires_drive_letter) {
        setRequiresDriveLetter(button);
    }
    if (spec.options.windows_native_filesystem) {
        setWindowsNativeFilesystemAction(button);
    }
    if (spec.options.resize_filesystem) {
        setResizeFilesystemAction(button);
    }
    if (spec.options.inspect_non_native_filesystem) {
        button->setProperty(kInspectNonNativeFilesystemActionProperty, true);
    }
    if (spec.options.browse_non_native_filesystem) {
        button->setProperty(kBrowseNonNativeFilesystemActionProperty, true);
    }
    if (spec.options.check_non_native_filesystem) {
        button->setProperty(kNonNativeFilesystemActionProperty, true);
    }
    if (spec.options.apfs_root_file_mutation) {
        button->setProperty(kApfsRootFileMutationActionProperty, true);
    }
    if (spec.options.hfs_file_mutation) {
        button->setProperty(kHfsFileMutationActionProperty, true);
    }
    m_targetButtons.append(button);
    return button;
}

void PartitionManagerPanel::addActionSpecSection(QVBoxLayout* layout,
                                                 QWidget* pane,
                                                 const QString& title,
                                                 const QVector<ActionLinkSpec>& specs,
                                                 bool scroll_buttons) {
    QVector<QToolButton*> buttons;
    buttons.reserve(specs.size());
    for (const auto& spec : specs) {
        buttons.append(createConfiguredActionLink(pane, spec));
    }
    addSidebarSection(layout, title, buttons, scroll_buttons);
}

void PartitionManagerPanel::addPendingOperationsSection(QVBoxLayout* layout, QWidget* pane) {
    m_pendingLabel = new QLabel(tr("0 Operations Pending"), pane);
    m_pendingLabel->setAccessibleName(tr("Pending operation count"));
    layout->addWidget(m_pendingLabel);
    m_queueList = new QListWidget(pane);
    m_queueList->setMinimumHeight(kPendingQueueMinHeight);
    setAccessible(m_queueList, tr("Pending operations"), tr("Queued partition operations"));
    layout->addWidget(m_queueList, 1);
}

PartitionManagerPanel::ActionLinkSpec PartitionManagerPanel::makeActionSpec(
    const QString& text,
    const QString& icon_path,
    const QString& tooltip,
    void (PartitionManagerPanel::*slot)(),
    const ActionLinkOptions& options) const {
    return {text, icon_path, tooltip, slot, options};
}

QVector<PartitionManagerPanel::ActionLinkSpec> PartitionManagerPanel::wizardActionSpecs() const {
    return {makeActionSpec(tr("Migrate OS to SSD/HDD Wizard"),
                           kIconOsDrive,
                           tr("Queue OS migration plan"),
                           &PartitionManagerPanel::onMigrateOs,
                           {actionTargetKindList({kActionTargetDisk})}),
            makeActionSpec(tr("Copy Partition Wizard"),
                           kIconCopy,
                           tr("Create a partition image"),
                           &PartitionManagerPanel::onCopyPartitionWizard,
                           {actionTargetKindList({kActionTargetPartition})}),
            makeActionSpec(tr("Copy Disk Wizard"),
                           kIconDisk,
                           tr("Queue disk clone plan"),
                           &PartitionManagerPanel::onCloneDisk,
                           {actionTargetKindList({kActionTargetDisk})}),
            makeActionSpec(tr("Partition Recovery Wizard"),
                           kIconRecovery,
                           tr("Scan and recover lost partitions"),
                           &PartitionManagerPanel::onPartitionRecoveryWizard,
                           {actionTargetKindList({kActionTargetDisk})}),
            makeActionSpec(tr("Data Recovery"),
                           kIconRecovery,
                           tr("Recover files from an image or raw volume/device path"),
                           &PartitionManagerPanel::onDataRecovery,
                           {actionTargetKindList({kActionTargetAny})}),
            makeActionSpec(tr("Extend Partition Wizard"),
                           kIconResize,
                           tr("Extend into adjacent free space"),
                           &PartitionManagerPanel::onExtendPartitionWizard,
                           {actionTargetKindList({kActionTargetPartition})})};
}

QVector<PartitionManagerPanel::ActionLinkSpec>
PartitionManagerPanel::partitionOperationActionSpecs() const {
    auto specs = layoutOperationActionSpecs();
    specs += filesystemOperationActionSpecs();
    specs += maintenanceOperationActionSpecs();
    specs += advancedOperationActionSpecs();
    return specs;
}

QVector<PartitionManagerPanel::ActionLinkSpec> PartitionManagerPanel::layoutOperationActionSpecs()
    const {
    return {makeActionSpec(tr("Resize/Move Partition"),
                           kIconResize,
                           tr("Resize partition"),
                           &PartitionManagerPanel::onResizePartition,
                           {actionTargetKindList({kActionTargetPartition}), false, false, true}),
            makeActionSpec(
                tr("Allocate Free Space"),
                kIconResize,
                tr("Back up adjacent donor, extend target, recreate donor, restore, and verify"),
                &PartitionManagerPanel::onAllocateFreeSpace,
                {actionTargetKindList({kActionTargetPartition, kActionTargetUnallocated})}),
            makeActionSpec(tr("Merge Partitions"),
                           kIconSplit,
                           tr("Merge adjacent partitions"),
                           &PartitionManagerPanel::onMergePartitions,
                           {actionTargetKindList({kActionTargetPartition})}),
            makeActionSpec(tr("Split Partition"),
                           kIconSplit,
                           tr("Split selected partition"),
                           &PartitionManagerPanel::onSplitPartition,
                           {actionTargetKindList({kActionTargetPartition})}),
            makeActionSpec(tr("Quick Partition"),
                           kIconCreate,
                           tr("Queue custom or equal-size disk layout"),
                           &PartitionManagerPanel::onQuickPartition,
                           {actionTargetKindList({kActionTargetDisk})}),
            makeActionSpec(tr("Create Partition"),
                           kIconCreate,
                           tr("Create partition"),
                           &PartitionManagerPanel::onCreatePartition,
                           {actionTargetKindList({kActionTargetUnallocated})}),
            makeActionSpec(tr("Delete Partition"),
                           kIconDelete,
                           tr("Delete partition"),
                           &PartitionManagerPanel::onDeletePartition,
                           {actionTargetKindList({kActionTargetPartition})}),
            makeActionSpec(tr("Convert MBR/GPT"),
                           kIconConvert,
                           tr("Convert partition table"),
                           &PartitionManagerPanel::onConvertStyle,
                           {actionTargetKindList({kActionTargetDisk})}),
            makeActionSpec(tr("Initialize Disk"),
                           kIconDisk,
                           tr("Initialize empty disk"),
                           &PartitionManagerPanel::onInitializeDisk,
                           {actionTargetKindList({kActionTargetDisk})}),
            makeActionSpec(tr("Delete All Partitions"),
                           kIconDelete,
                           tr("Delete all disk partitions"),
                           &PartitionManagerPanel::onDeleteAllPartitions,
                           {actionTargetKindList({kActionTargetDisk})})};
}

QVector<PartitionManagerPanel::ActionLinkSpec> PartitionManagerPanel::nativeFilesystemActionSpecs()
    const {
    return {makeActionSpec(tr("Format Partition"),
                           kIconDisk,
                           tr("Format partition"),
                           &PartitionManagerPanel::onFormatPartition,
                           {actionTargetKindList({kActionTargetPartition})}),
            makeActionSpec(tr("Explore Partition"),
                           kIconProperties,
                           tr("Open in Explorer"),
                           &PartitionManagerPanel::onExploreSelected,
                           {actionTargetKindList({kActionTargetPartition}), true}),
            makeActionSpec(tr("Convert File System"),
                           kIconConvert,
                           tr("Convert partition file system"),
                           &PartitionManagerPanel::onConvertFileSystem,
                           {actionTargetKindList({kActionTargetPartition}), true, true}),
            makeActionSpec(tr("Change Cluster Size"),
                           kIconProperties,
                           tr("Back up, reformat with selected cluster size, restore, and verify"),
                           &PartitionManagerPanel::onChangeClusterSize,
                           {actionTargetKindList({kActionTargetPartition}), true, true}),
            makeActionSpec(tr("Change Drive Letter"),
                           kIconProperties,
                           tr("Set drive letter"),
                           &PartitionManagerPanel::onSetDriveLetter,
                           {actionTargetKindList({kActionTargetPartition})}),
            makeActionSpec(tr("Change Label"),
                           kIconProperties,
                           tr("Set partition label"),
                           &PartitionManagerPanel::onSetPartitionLabel,
                           {actionTargetKindList({kActionTargetPartition}), true, true}),
            makeActionSpec(tr("Check File System"),
                           kIconSurface,
                           tr("Scan file system"),
                           &PartitionManagerPanel::onCheckFileSystem,
                           {actionTargetKindList({kActionTargetPartition}), true, true})};
}

QVector<PartitionManagerPanel::ActionLinkSpec>
PartitionManagerPanel::nonNativeFilesystemActionSpecs() const {
    return {
        makeActionSpec(tr("Inspect Non-Windows File System"),
                       kIconProperties,
                       tr("Inspect captured read-only filesystem metadata"),
                       &PartitionManagerPanel::onInspectNonNativeFileSystem,
                       {actionTargetKindList({kActionTargetPartition}), false, false, false, true}),
        makeActionSpec(
            tr("Browse Non-Windows File System"),
            kIconProperties,
            tr("List supported non-Windows directory entries without mounting"),
            &PartitionManagerPanel::onBrowseNonNativeFileSystem,
            {actionTargetKindList({kActionTargetPartition}), false, false, false, false, true}),
        makeActionSpec(tr("Check Non-Windows File System"),
                       kIconSurface,
                       tr("Requires manifest-approved bundled filesystem tools"),
                       &PartitionManagerPanel::onCheckNonNativeFileSystem,
                       {actionTargetKindList({kActionTargetPartition}),
                        false,
                        false,
                        false,
                        false,
                        false,
                        true}),
        makeActionSpec(tr("APFS File"),
                       kIconProperties,
                       tr("Queue generated APFS root-file write, patch, or delete"),
                       &PartitionManagerPanel::onApfsRootFileMutation,
                       {actionTargetKindList({kActionTargetPartition}),
                        false,
                        false,
                        false,
                        false,
                        false,
                        false,
                        true}),
        makeActionSpec(tr("HFS File"),
                       kIconProperties,
                       tr("Queue staged HFS+ file, resource fork, or inline attribute mutation"),
                       &PartitionManagerPanel::onHfsFileMutation,
                       {actionTargetKindList({kActionTargetPartition}),
                        false,
                        false,
                        false,
                        false,
                        false,
                        false,
                        false,
                        true})};
}

QVector<PartitionManagerPanel::ActionLinkSpec>
PartitionManagerPanel::filesystemOperationActionSpecs() const {
    QVector<ActionLinkSpec> specs = nativeFilesystemActionSpecs();
    specs += nonNativeFilesystemActionSpecs();
    return specs;
}

QVector<PartitionManagerPanel::ActionLinkSpec>
PartitionManagerPanel::maintenanceOperationActionSpecs() const {
    return {makeActionSpec(tr("Partition Alignment"),
                           kIconAlign,
                           tr("Run SSD optimization"),
                           &PartitionManagerPanel::onOptimizeSsd,
                           {actionTargetKindList({kActionTargetDisk, kActionTargetPartition})}),
            makeActionSpec(tr("Wipe Partition"),
                           kIconWipe,
                           tr("Wipe selected target"),
                           &PartitionManagerPanel::onWipeSelected,
                           {actionTargetKindList({kActionTargetDisk, kActionTargetPartition})}),
            makeActionSpec(tr("Surface Test"),
                           kIconSurface,
                           tr("Run read-only surface test"),
                           &PartitionManagerPanel::onSurfaceTest,
                           {actionTargetKindList({kActionTargetAny})}),
            makeActionSpec(tr("Disk Benchmark"),
                           kIconSurface,
                           tr("Open Benchmark and Diagnostics"),
                           &PartitionManagerPanel::onOpenDiskBenchmark,
                           {actionTargetKindList({kActionTargetAny})}),
            makeActionSpec(tr("Space Analyzer"),
                           kIconSurface,
                           tr("Analyze tree, file, and file-type usage"),
                           &PartitionManagerPanel::onSpaceAnalyzer,
                           {actionTargetKindList({kActionTargetPartition}), true}),
            makeActionSpec(tr("Disk Defrag"),
                           kIconAlign,
                           tr("Review defrag/ReTrim commands and open Windows Optimize Drives"),
                           &PartitionManagerPanel::onOpenOptimizeDrives,
                           {actionTargetKindList({kActionTargetDisk, kActionTargetPartition}),
                            true}),
            makeActionSpec(tr("SSD Secure Erase"),
                           kIconWipe,
                           tr("Queue SSD/NVMe ReTrim plus clear-level wipe"),
                           &PartitionManagerPanel::onSsdSecureErase,
                           {actionTargetKindList({kActionTargetDisk})}),
            makeActionSpec(tr("Manage BitLocker"),
                           kIconProperties,
                           tr("Review BitLocker status and open Windows management"),
                           &PartitionManagerPanel::onManageBitLocker,
                           {actionTargetKindList({kActionTargetPartition}), true}),
            makeActionSpec(tr("Make Bootable Media"),
                           kIconOsDrive,
                           tr("Open Image Flasher"),
                           &PartitionManagerPanel::onOpenBootableMedia,
                           {actionTargetKindList({kActionTargetAny})})};
}

QVector<PartitionManagerPanel::ActionLinkSpec> PartitionManagerPanel::advancedOperationActionSpecs()
    const {
    return {makeActionSpec(tr("Hide/Unhide Partition"),
                           kIconProperties,
                           tr("Toggle hidden flag"),
                           &PartitionManagerPanel::onSetPartitionHidden,
                           {actionTargetKindList({kActionTargetPartition})}),
            makeActionSpec(tr("Set Active/Inactive"),
                           kIconProperties,
                           tr("Toggle MBR active flag"),
                           &PartitionManagerPanel::onSetPartitionActive,
                           {actionTargetKindList({kActionTargetPartition})}),
            makeActionSpec(tr("Change Partition Type ID"),
                           kIconProperties,
                           tr("Set partition type ID"),
                           &PartitionManagerPanel::onSetPartitionTypeId,
                           {actionTargetKindList({kActionTargetPartition})}),
            makeActionSpec(tr("Convert Primary/Logical"),
                           kIconConvert,
                           tr("Back up, rebuild MBR primary/logical layout, restore, and verify"),
                           &PartitionManagerPanel::onConvertPrimaryLogical,
                           {actionTargetKindList({kActionTargetPartition})}),
            makeActionSpec(tr("Change Serial Number"),
                           kIconProperties,
                           tr("Back up, reformat to regenerate volume serial, restore, and verify"),
                           &PartitionManagerPanel::onChangeVolumeSerialNumber,
                           {actionTargetKindList({kActionTargetPartition})}),
            makeActionSpec(tr("Convert Dynamic Disk to Basic"),
                           kIconConvert,
                           tr("Back up dynamic volume, convert disk to basic, restore, and verify"),
                           &PartitionManagerPanel::onConvertDynamicDiskToBasic,
                           {actionTargetKindList({kActionTargetDisk})}),
            makeActionSpec(tr("Properties"),
                           kIconProperties,
                           tr("Show selected properties"),
                           &PartitionManagerPanel::onShowProperties,
                           {actionTargetKindList({kActionTargetAny})})};
}

QWidget* PartitionManagerPanel::createWorkspace(QWidget* parent) {
    auto* workspace = new QWidget(parent);
    auto* layout = new QVBoxLayout(workspace);
    layout->setContentsMargins(
        ui::kMarginSmall, ui::kMarginSmall, ui::kMarginSmall, ui::kMarginSmall);
    layout->setSpacing(ui::kSpacingSmall);

    m_table = new QTableWidget(workspace);
    m_table->setColumnCount(ColCount);
    m_table->setHorizontalHeaderLabels({tr("Partition"),
                                        tr("File System"),
                                        tr("Capacity"),
                                        tr("Used Space"),
                                        tr("Free Space"),
                                        tr("Flag"),
                                        tr("Status")});
    m_table->setCornerButtonEnabled(false);
    m_table->setFrameShape(QFrame::NoFrame);
    m_table->setLineWidth(0);
    m_table->setMidLineWidth(0);
    m_table->setShowGrid(false);
    m_table->setItemDelegate(new PartitionTableDelegate(m_table));
    m_table->horizontalHeader()->setStretchLastSection(true);
    m_table->horizontalHeader()->setHighlightSections(false);
    m_table->horizontalHeader()->setDefaultAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    m_table->verticalHeader()->setVisible(false);
    m_table->verticalHeader()->setDefaultSectionSize(kPartitionTableRowHeight);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setSelectionMode(QAbstractItemView::SingleSelection);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->setContextMenuPolicy(Qt::CustomContextMenu);
    setAccessible(m_table, tr("Disk and partition table"), tr("Select a disk or partition"));

    auto* workspaceSplitter = new QSplitter(Qt::Vertical, workspace);
    workspaceSplitter->setChildrenCollapsible(false);
    workspaceSplitter->setAccessibleName(tr("Partition table and disk map splitter"));
    workspaceSplitter->addWidget(m_table);
    workspaceSplitter->addWidget(createDiskMapPane(workspaceSplitter));
    workspaceSplitter->setStretchFactor(0, kPartitionCenterTableStretch);
    workspaceSplitter->setStretchFactor(1, 1);
    workspaceSplitter->setSizes({kWorkspaceInitialTableHeight, kWorkspaceInitialMapHeight});
    layout->addWidget(workspaceSplitter, 1);
    layout->addWidget(createLegend(workspace));
    return workspace;
}

QWidget* PartitionManagerPanel::createDiskMapPane(QWidget* parent) {
    auto* frame = new QWidget(parent);
    frame->setObjectName(QStringLiteral("partitionDiskMapPane"));
    frame->setMinimumHeight(kDiskMapMinHeight);
    frame->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    applyWindowFill(frame, QApplication::palette().color(QPalette::AlternateBase));
    auto* frameLayout = new QVBoxLayout(frame);
    frameLayout->setContentsMargins(
        ui::kMarginNone, ui::kMarginNone, ui::kMarginNone, ui::kMarginNone);
    auto* scroll = new QScrollArea(frame);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    m_diskMapContainer = new QWidget(scroll);
    m_diskMapContainer->setObjectName(QStringLiteral("partitionDiskMapContainer"));
    applyWindowFill(m_diskMapContainer, QApplication::palette().color(QPalette::Base));
    auto* mapLayout = new QVBoxLayout(m_diskMapContainer);
    mapLayout->setContentsMargins(
        kDiskMapOuterMargin, kDiskMapOuterMargin, kDiskMapOuterMargin, kDiskMapOuterMargin);
    mapLayout->setSpacing(kDiskMapRowSpacing);
    scroll->setWidget(m_diskMapContainer);
    setAccessible(scroll,
                  tr("Partition disk map"),
                  tr("Visual proportional map of disks, partitions, and free space"));
    frameLayout->addWidget(scroll);
    return frame;
}

QWidget* PartitionManagerPanel::createLegend(QWidget* parent) {
    auto* legend = new QWidget(parent);
    auto* layout = new QHBoxLayout(legend);
    layout->setContentsMargins(ui::kMarginNone, ui::kMarginNone, ui::kMarginNone, ui::kMarginNone);
    layout->setSpacing(ui::kSpacingMedium);
    addLegendItem(layout, tr("GPT/Primary"), kPartitionColorGptPrimary);
    addLegendItem(layout, tr("Logical"), kPartitionColorLogical);
    addLegendItem(layout, tr("Simple"), kPartitionColorSimple);
    addLegendItem(layout, tr("Spanned"), kPartitionColorSpanned);
    addLegendItem(layout, tr("Striped"), kPartitionColorStriped);
    addLegendItem(layout, tr("Mirrored"), kPartitionColorMirrored);
    addLegendItem(layout, tr("RAID5"), kPartitionColorRaid5);
    addLegendItem(layout, tr("Unallocated"), unallocatedColor());
    layout->addStretch();
    m_logToggle = new LogToggleSwitch(tr("Log"), legend);
    layout->addWidget(m_logToggle);
    return legend;
}

QToolButton* PartitionManagerPanel::createRibbonButton(QWidget* parent,
                                                       const QString& text,
                                                       const QString& icon_path,
                                                       const QString& tooltip) {
    auto* button = new QToolButton(parent);
    button->setText(text);
    button->setObjectName(QStringLiteral("partitionRibbonButton"));
    button->setIcon(icons8RibbonIcon(icon_path));
    button->setIconSize(QSize(kRibbonIconSize, kRibbonIconSize));
    button->setToolButtonStyle(Qt::ToolButtonTextUnderIcon);
    button->setAutoRaise(true);
    button->setProperty("iconPath", icon_path);
    button->setProperty("iconSource", QStringLiteral("Icons8 SVG"));
    button->setProperty("iconModes", QStringLiteral("Normal,Disabled,Active,Selected"));
    button->setProperty("ribbonButtonWidth", kRibbonButtonWidth);
    button->setProperty("ribbonButtonHeight", kRibbonButtonHeight);
    button->setAccessibleName(text);
    button->setToolTip(tooltip);
    button->setFixedSize(kRibbonButtonWidth, kRibbonButtonHeight);
    return button;
}

QToolButton* PartitionManagerPanel::createActionLink(QWidget* parent,
                                                     const QString& text,
                                                     const QString& icon_path,
                                                     const QString& tooltip) {
    auto* button = new QToolButton(parent);
    button->setText(text);
    button->setObjectName(QStringLiteral("partitionActionTextLink"));
    button->setIcon(QIcon(icon_path));
    button->setIconSize(QSize(kActionIconSize, kActionIconSize));
    button->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    button->setAutoRaise(true);
    button->setCursor(Qt::PointingHandCursor);
    button->setAccessibleName(text);
    button->setToolTip(tooltip);
    button->setProperty(kActionDefaultTooltipProperty, tooltip);
    button->setMinimumHeight(kActionLinkHeight);
    button->setMaximumHeight(kActionLinkHeight);
    button->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    button->setStyleSheet(ui::partitionActionTextLinkStyle());
    return button;
}

QToolButton* PartitionManagerPanel::createDisabledActionLink(QWidget* parent,
                                                             const QString& text,
                                                             const QString& icon_path,
                                                             const QString& reason) {
    auto* button = createActionLink(parent, text, icon_path, reason);
    button->setEnabled(false);
    button->setAccessibleDescription(reason);
    return button;
}

void PartitionManagerPanel::addSidebarSection(QVBoxLayout* layout,
                                              const QString& title,
                                              const QVector<QToolButton*>& buttons,
                                              bool scroll_buttons) {
    auto* group = new QGroupBox(title, this);
    group->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Minimum);
    auto* groupLayout = new QVBoxLayout(group);
    groupLayout->setContentsMargins(
        ui::kMarginSmall, ui::kMarginSmall, ui::kMarginSmall, ui::kMarginSmall);
    groupLayout->setSpacing(ui::kSpacingTight);

    auto* buttonLayout = groupLayout;
    if (scroll_buttons) {
        auto* scroll = new QScrollArea(group);
        scroll->setWidgetResizable(true);
        scroll->setFrameShape(QFrame::NoFrame);
        setAccessible(scroll, tr("Partition operations"), tr("Scroll partition operation buttons"));
        auto* buttonHost = new QWidget(scroll);
        buttonLayout = new QVBoxLayout(buttonHost);
        buttonLayout->setContentsMargins(
            ui::kMarginNone, ui::kMarginNone, ui::kMarginNone, ui::kMarginNone);
        buttonLayout->setSpacing(ui::kSpacingTight);
        scroll->setWidget(buttonHost);
        groupLayout->addWidget(scroll);
    }

    for (auto* button : buttons) {
        buttonLayout->addWidget(button);
    }
    if (scroll_buttons) {
        buttonLayout->addStretch();
    }
    layout->addWidget(group, scroll_buttons ? 1 : 0);
}

void PartitionManagerPanel::addLegendItem(QHBoxLayout* layout,
                                          const QString& text,
                                          const QColor& color) {
    auto* item = new QWidget(this);
    auto* itemLayout = new QHBoxLayout(item);
    itemLayout->setContentsMargins(
        ui::kMarginNone, ui::kMarginNone, ui::kMarginNone, ui::kMarginNone);
    auto* swatch = new PartitionLegendSwatch(text, color, item);
    auto* label = new QLabel(text, item);
    itemLayout->addWidget(swatch);
    itemLayout->addWidget(label);
    layout->addWidget(item);
}

void PartitionManagerPanel::connectController() {
    connect(
        m_refreshButton, &QAbstractButton::clicked, this, &PartitionManagerPanel::refreshInventory);
    connect(m_applyButton, &QAbstractButton::clicked, this, &PartitionManagerPanel::applyQueue);
    connect(m_cancelButton, &QAbstractButton::clicked, this, &PartitionManagerPanel::cancelApply);
    connect(m_dryRunButton, &QAbstractButton::clicked, this, &PartitionManagerPanel::dryRunQueue);
    connect(m_discardButton, &QAbstractButton::clicked, this, &PartitionManagerPanel::discardQueue);
    connect(m_undoButton, &QAbstractButton::clicked, this, &PartitionManagerPanel::undoQueue);
    connect(m_redoButton, &QAbstractButton::clicked, this, &PartitionManagerPanel::redoQueue);
    connect(m_table,
            &QTableWidget::itemSelectionChanged,
            this,
            &PartitionManagerPanel::onTableSelectionChanged);
    connect(m_table,
            &QWidget::customContextMenuRequested,
            this,
            &PartitionManagerPanel::showPartitionContextMenu);

    connect(m_controller.get(),
            &PartitionManagerController::inventoryChanged,
            this,
            &PartitionManagerPanel::rebuildTable);
    connect(m_controller.get(),
            &PartitionManagerController::queueChanged,
            this,
            &PartitionManagerPanel::rebuildQueue);
    connect(m_controller.get(),
            &PartitionManagerController::statusMessage,
            this,
            &PartitionManagerPanel::statusMessage);
    connect(m_controller.get(),
            &PartitionManagerController::progressUpdate,
            this,
            &PartitionManagerPanel::progressUpdate);
    connect(m_controller.get(),
            &PartitionManagerController::logOutput,
            this,
            &PartitionManagerPanel::logOutput);
    connect(m_controller.get(),
            &PartitionManagerController::executionFinished,
            this,
            &PartitionManagerPanel::onExecutionFinished);
    connect(m_controller.get(), &PartitionManagerController::stateChanged, this, [this]() {
        updateActionState();
    });
}

void PartitionManagerPanel::refreshInventory() {
    m_inventoryLoadStarted = true;
    updateRefreshButtonState();
    Q_EMIT statusMessage(tr("Scanning disks..."), 0);
    m_controller->refreshInventory();
}

void PartitionManagerPanel::applyQueue() {
    if (!showApplyReviewDialog()) {
        return;
    }
    m_controller->applyQueue(false, true);
}

void PartitionManagerPanel::cancelApply() {
    m_controller->cancel();
}

void PartitionManagerPanel::dryRunQueue() {
    m_controller->applyQueue(true, false);
}

void PartitionManagerPanel::discardQueue() {
    m_controller->discardQueue();
}

void PartitionManagerPanel::undoQueue() {
    m_controller->undo();
}

void PartitionManagerPanel::redoQueue() {
    m_controller->redo();
}

void PartitionManagerPanel::rebuildTable(const PartitionInventory& inventory) {
    m_inventoryLoadStarted = true;
    updateRefreshButtonState();
    rebuildDiskMap(inventory);
    m_table->setRowCount(0);
    for (const auto& disk : inventory.disks) {
        addDiskRow(disk);
        for (const auto& partition : disk.partitions) {
            addPartitionRow(disk, partition);
        }
        for (const auto& region : disk.unallocated_regions) {
            addUnallocatedRow(region);
        }
    }
    m_table->resizeColumnsToContents();
    Q_EMIT statusMessage(inventorySummaryText(inventory), 0);
    updateActionState();
}

void PartitionManagerPanel::rebuildDiskMap(const PartitionInventory& inventory) {
    auto* layout = qobject_cast<QVBoxLayout*>(m_diskMapContainer->layout());
    if (!layout) {
        return;
    }
    clearLayout(layout);
    for (const auto& disk : inventory.disks) {
        addDiskMapRow(layout, disk);
    }
    layout->addStretch();
}

void PartitionManagerPanel::addDiskMapRow(QVBoxLayout* layout, const PartitionDiskInfo& disk) {
    auto* row = new DiskMapRowFrame(m_diskMapContainer);
    const auto selected = selectedTarget();
    const bool diskSelected = targetMatchesDisk(selected, disk);
    const QString selectedColorRole = diskSelectionColorRole(disk);
    const QColor selectedColor = partitionColorForRole(selectedColorRole);
    PartitionTarget diskTarget;
    diskTarget.kind = PartitionTargetKind::Disk;
    diskTarget.disk_number = disk.disk_number;
    diskTarget.size_bytes = disk.size_bytes;
    attachDiskMapContextMenu(row, diskTarget);
    row->setProperty("selected", diskSelected);
    row->setProperty("selectedColorRole", selectedColorRole);
    row->setProperty("selectedColorValue", selectedColor.name());
    auto* rowLayout = new QHBoxLayout(row);
    rowLayout->setContentsMargins(kDiskMapRowInnerMargin,
                                  kDiskMapRowInnerMargin,
                                  kDiskMapRowInnerMargin,
                                  kDiskMapRowInnerMargin);
    rowLayout->setSpacing(kDiskMapSegmentSpacing);

    rowLayout->addWidget(createDiskTile(disk));

    auto* bar = new QWidget(row);
    attachDiskMapContextMenu(bar, diskTarget);
    auto* barLayout = new QHBoxLayout(bar);
    barLayout->setContentsMargins(
        ui::kMarginNone, ui::kMarginNone, ui::kMarginNone, ui::kMarginNone);
    barLayout->setSpacing(kDiskMapSegmentSpacing);
    addPartitionSegmentsToDiskMap(barLayout, disk, selected);
    addUnallocatedSegmentsToDiskMap(barLayout, disk, selected);
    rowLayout->addWidget(bar);
    layout->addWidget(row);
}

void PartitionManagerPanel::addPartitionSegmentsToDiskMap(
    QHBoxLayout* layout,
    const PartitionDiskInfo& disk,
    const std::optional<PartitionTarget>& selected) {
    for (const auto& partition : disk.partitions) {
        layout->addWidget(createPartitionSegment(disk, partition, selected),
                          stretchForBytes(partition.size_bytes, disk.size_bytes));
    }
}

void PartitionManagerPanel::addUnallocatedSegmentsToDiskMap(
    QHBoxLayout* layout,
    const PartitionDiskInfo& disk,
    const std::optional<PartitionTarget>& selected) {
    for (const auto& region : disk.unallocated_regions) {
        layout->addWidget(createUnallocatedSegment(region, selected),
                          stretchForBytes(region.size_bytes, disk.size_bytes));
    }
}

QWidget* PartitionManagerPanel::createPartitionSegment(
    const PartitionDiskInfo& disk,
    const PartitionInfoEx& partition,
    const std::optional<PartitionTarget>& selected) {
    PartitionTarget segmentTarget;
    segmentTarget.kind = PartitionTargetKind::Partition;
    segmentTarget.disk_number = disk.disk_number;
    segmentTarget.partition_number = partition.partition_number;
    segmentTarget.partition_guid = partition.partition_guid;
    segmentTarget.offset_bytes = partition.offset_bytes;
    segmentTarget.size_bytes = partition.size_bytes;
    if (partition.volume) {
        segmentTarget.volume_guid = partition.volume->volume_guid;
        segmentTarget.drive_letter = partition.volume->drive_letter;
    }

    const QString label = tr("%1 %2").arg(partitionLabel(partition),
                                          formatPartitionBytes(partition.size_bytes));
    const QString tooltip = tr("Disk %1 partition %2, %3, %4")
                                .arg(disk.disk_number)
                                .arg(partition.partition_number)
                                .arg(partition.volume ? partition.volume->file_system
                                                      : partition.type_name,
                                     formatPartitionBytes(partition.size_bytes));
    auto* segment = new PartitionSegmentWidget({label,
                                                tooltip,
                                                partitionColorRole(disk, partition),
                                                partitionColor(disk, partition),
                                                usedPercent(partition),
                                                targetMatchesPartition(selected, disk, partition),
                                                [this, segmentTarget]() {
                                                    selectTargetInTable(segmentTarget);
                                                }},
                                               m_diskMapContainer);
    attachDiskMapContextMenu(segment, segmentTarget);
    return segment;
}

QWidget* PartitionManagerPanel::createUnallocatedSegment(
    const UnallocatedRegion& region, const std::optional<PartitionTarget>& selected) {
    PartitionTarget segmentTarget;
    segmentTarget.kind = PartitionTargetKind::Unallocated;
    segmentTarget.disk_number = region.disk_number;
    segmentTarget.offset_bytes = region.offset_bytes;
    segmentTarget.size_bytes = region.size_bytes;
    auto* segment = new PartitionSegmentWidget(
        {tr("Unallocated %1").arg(formatPartitionBytes(region.size_bytes)),
         tr("Disk %1 unallocated space").arg(region.disk_number),
         QStringLiteral("Unallocated"),
         unallocatedColor(),
         0,
         targetMatchesRegion(selected, region),
         [this, segmentTarget]() { selectTargetInTable(segmentTarget); }},
        m_diskMapContainer);
    attachDiskMapContextMenu(segment, segmentTarget);
    return segment;
}

QWidget* PartitionManagerPanel::createDiskTile(const PartitionDiskInfo& disk) {
    const auto selected = selectedTarget();
    const QString colorRole = diskSelectionColorRole(disk);
    PartitionTarget target;
    target.kind = PartitionTargetKind::Disk;
    target.disk_number = disk.disk_number;
    target.size_bytes = disk.size_bytes;
    DiskTileSpec spec;
    spec.name = tr("Disk %1").arg(disk.disk_number);
    spec.detail = tr("%1\n%2").arg(disk.partition_style, formatPartitionBytes(disk.size_bytes));
    spec.icon = QIcon(kIconDisk);
    spec.accessible = tr("Disk %1, %2, %3")
                          .arg(disk.disk_number)
                          .arg(disk.partition_style, formatPartitionBytes(disk.size_bytes));
    spec.color = partitionColorForRole(colorRole);
    spec.selected = targetMatchesDisk(selected, disk);
    spec.on_activated = [this, target]() {
        selectTargetInTable(target);
    };
    auto* tile = new DiskTileWidget(std::move(spec), m_diskMapContainer);
    attachDiskMapContextMenu(tile, target);
    return tile;
}

void PartitionManagerPanel::selectTargetInTable(const PartitionTarget& target) {
    if (!m_table) {
        return;
    }

    for (int row = 0; row < m_table->rowCount(); ++row) {
        auto* item = m_table->item(row, ColPartition);
        if (!item) {
            continue;
        }
        if (!rowMatchesTarget(item->data(Qt::UserRole).toMap(), target)) {
            continue;
        }

        m_table->setCurrentItem(item);
        m_table->selectRow(row);
        m_table->scrollToItem(item, QAbstractItemView::PositionAtCenter);
        return;
    }
}

void PartitionManagerPanel::attachDiskMapContextMenu(QWidget* widget,
                                                     const PartitionTarget& target) {
    if (!widget) {
        return;
    }
    widget->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(widget,
            &QWidget::customContextMenuRequested,
            this,
            [this, widget, target](const QPoint& position) {
                const QPoint globalPosition = widget->mapToGlobal(position);
                selectTargetInTable(target);
                showSelectedTargetContextMenuAt(globalPosition);
            });
}

void PartitionManagerPanel::addDiskRow(const PartitionDiskInfo& disk) {
    const int row = m_table->rowCount();
    m_table->insertRow(row);
    QVariantMap rowData{{QStringLiteral("kind"), rowKindName(TableRowKind::Disk)},
                        {QStringLiteral("disk"), static_cast<int>(disk.disk_number)}};
    auto* target = new QTableWidgetItem(QIcon(kIconDisk), tr("Disk %1").arg(disk.disk_number));
    auto font = target->font();
    font.setBold(true);
    target->setFont(font);
    target->setData(Qt::UserRole, rowData);
    target->setToolTip(
        tr("Disk %1, %2, %3, %4")
            .arg(disk.disk_number)
            .arg(disk.partition_style, formatPartitionBytes(disk.size_bytes), disk.health_status));
    m_table->setItem(row, ColPartition, target);
    QStringList statusParts;
    if (!disk.health_status.isEmpty()) {
        statusParts.append(disk.health_status);
    }
    if (!disk.operational_status.isEmpty()) {
        statusParts.append(disk.operational_status);
    }
    if (disk.temperature_celsius >= 0) {
        statusParts.append(tr("%1 C").arg(disk.temperature_celsius));
    }
    if (disk.wear_percent > 0) {
        statusParts.append(tr("%1% wear").arg(disk.wear_percent));
    }
    if (disk.read_errors_total > 0 || disk.write_errors_total > 0) {
        statusParts.append(
            tr("R/W errors %1/%2").arg(disk.read_errors_total).arg(disk.write_errors_total));
    }
    if (!statusParts.isEmpty()) {
        target->setToolTip(target->toolTip() +
                           tr(", %1").arg(statusParts.join(QStringLiteral(" | "))));
    }
    for (int column = ColFileSystem; column < ColCount; ++column) {
        m_table->setItem(row, column, new QTableWidgetItem(QString()));
    }
}

void PartitionManagerPanel::addPartitionRow(const PartitionDiskInfo& disk,
                                            const PartitionInfoEx& partition) {
    const int row = m_table->rowCount();
    m_table->insertRow(row);
    QVariantMap rowData{{QStringLiteral("kind"), rowKindName(TableRowKind::Partition)},
                        {QStringLiteral("disk"), static_cast<int>(disk.disk_number)},
                        {QStringLiteral("partition"), static_cast<int>(partition.partition_number)},
                        {QStringLiteral("offset"), QString::number(partition.offset_bytes)},
                        {QStringLiteral("size"), QString::number(partition.size_bytes)}};
    if (partition.volume) {
        rowData[QStringLiteral("letter")] = partition.volume->drive_letter;
    }

    auto* target = new QTableWidgetItem(tr("  %1").arg(partitionLabel(partition)));
    target->setData(Qt::UserRole, rowData);
    m_table->setItem(row, ColPartition, target);
    auto* fileSystemItem = new QTableWidgetItem(partition.volume ? partition.volume->file_system
                                                                 : QString());
    if (partition.volume) {
        fileSystemItem->setToolTip(fileSystemTooltipText(*partition.volume));
    }
    m_table->setItem(row, ColFileSystem, fileSystemItem);
    m_table->setItem(row,
                     ColCapacity,
                     new QTableWidgetItem(formatPartitionBytes(partition.size_bytes)));
    m_table->setItem(row,
                     ColUsed,
                     new QTableWidgetItem(formatPartitionBytes(usedBytes(partition))));
    m_table->setItem(row,
                     ColUnused,
                     new QTableWidgetItem(partition.volume
                                              ? formatPartitionBytes(partition.volume->free_bytes)
                                              : QString()));
    m_table->setItem(row, ColFlags, new QTableWidgetItem(flagsForPartition(partition)));
    m_table->setItem(row,
                     ColStatus,
                     new QTableWidgetItem(partition.volume ? partition.volume->health_status
                                                           : QString()));
}

void PartitionManagerPanel::addUnallocatedRow(const UnallocatedRegion& region) {
    if (region.size_bytes == 0) {
        return;
    }
    const int row = m_table->rowCount();
    m_table->insertRow(row);
    QVariantMap rowData{{QStringLiteral("kind"), rowKindName(TableRowKind::Unallocated)},
                        {QStringLiteral("disk"), static_cast<int>(region.disk_number)},
                        {QStringLiteral("offset"), QString::number(region.offset_bytes)},
                        {QStringLiteral("size"), QString::number(region.size_bytes)}};
    auto* target = new QTableWidgetItem(tr("  Unallocated"));
    target->setData(Qt::UserRole, rowData);
    m_table->setItem(row, ColPartition, target);
    m_table->setItem(row, ColFileSystem, new QTableWidgetItem(tr("Unallocated")));
    m_table->setItem(row,
                     ColCapacity,
                     new QTableWidgetItem(formatPartitionBytes(region.size_bytes)));
    m_table->setItem(row, ColUsed, new QTableWidgetItem(formatPartitionBytes(0)));
    m_table->setItem(row, ColUnused, new QTableWidgetItem(formatPartitionBytes(region.size_bytes)));
    m_table->setItem(row, ColFlags, new QTableWidgetItem(tr("Free space")));
    m_table->setItem(row, ColStatus, new QTableWidgetItem(tr("None")));
}

void PartitionManagerPanel::rebuildQueue(const QVector<PartitionOperation>& operations) {
    if (m_pendingLabel) {
        m_pendingLabel->setText(tr("%1 Operations Pending").arg(operations.size()));
    }
    if (!m_queueList) {
        updateActionState();
        return;
    }
    m_queueList->clear();
    for (const auto& operation : operations) {
        QString text = operation.summary;
        if (!operation.blockers.isEmpty()) {
            text += tr(" - BLOCKED: %1").arg(operation.blockers.join(QStringLiteral("; ")));
        }
        m_queueList->addItem(text);
    }
    updateActionState();
}

void PartitionManagerPanel::onTableSelectionChanged() {
    updateDetails();
    rebuildDiskMap(m_controller->inventory());
    updateActionState();
}

void PartitionManagerPanel::updateActionState() {
    const auto state = m_controller->state();
    const bool operationRunning = state == PartitionManagerState::AwaitingElevation ||
                                  state == PartitionManagerState::Applying ||
                                  state == PartitionManagerState::Verifying;
    const auto target = selectedTarget();
    const auto* partition = selectedPartition();
    for (auto* button : m_targetButtons) {
        updateTargetButtonState(button, operationRunning, target, partition);
    }
    const bool hasQueue = !m_controller->queue().isEmpty();
    m_applyButton->setEnabled(
        !operationRunning && m_controller->queue().canApply(m_controller->inventory().layout_hash));
    m_cancelButton->setEnabled(operationRunning);
    m_dryRunButton->setEnabled(hasQueue && !operationRunning);
    m_discardButton->setEnabled(hasQueue && !operationRunning);
    m_undoButton->setEnabled(hasQueue && !operationRunning);
    m_redoButton->setEnabled(m_controller->queue().canRedo() && !operationRunning);
    m_refreshButton->setEnabled(!operationRunning);
}

void PartitionManagerPanel::updateRefreshButtonState() {
    if (!m_refreshButton) {
        return;
    }
    const QString text = m_inventoryLoadStarted ? tr("Refresh Disks") : tr("Scan Disks");
    m_refreshButton->setText(text);
    m_refreshButton->setAccessibleName(text);
    m_refreshButton->setToolTip(m_inventoryLoadStarted ? tr("Refresh disk inventory")
                                                       : tr("Scan disk inventory"));
}

void PartitionManagerPanel::updateDetails() {
    if (!m_details) {
        return;
    }
    const auto target = selectedTarget();
    if (!target) {
        m_details->setPlainText(tr("Select a disk, partition, or unallocated region."));
        return;
    }
    QString text;
    if (const auto* disk = selectedDisk()) {
        text += tr("Disk %1\nModel: %2\nStyle: %3\nSize: %4\nHealth: %5\n\n")
                    .arg(disk->disk_number)
                    .arg(disk->model, disk->partition_style)
                    .arg(formatPartitionBytes(disk->size_bytes), disk->health_status);
        if (disk->is_system) {
            text += tr("System disk destructive operations are blocked in v1.\n");
        }
        if (disk->is_dynamic || disk->is_storage_spaces) {
            text += tr(
                "Dynamic disks block normal writes; one-volume dynamic-to-basic "
                "conversion uses backup/restore.\n");
        }
    }
    if (const auto* partition = selectedPartition()) {
        text += tr("\nPartition %1\nType: %2\nSize: %3\nFlags: %4\n")
                    .arg(partition->partition_number)
                    .arg(partition->type_name)
                    .arg(formatPartitionBytes(partition->size_bytes),
                         flagsForPartition(*partition));
    }
    m_details->setPlainText(text);
}

void PartitionManagerPanel::queueOperation(PartitionOperationType type,
                                           const QJsonObject& payload) {
    const auto target = selectedTarget();
    if (!target) {
        Q_EMIT statusMessage(tr("Select a target first"), sak::kTimerStatusDefaultMs);
        return;
    }
    m_controller->queueOperation(type, *target, payload);
}

void PartitionManagerPanel::onShowProperties() {
    const auto target = selectedTarget();
    if (!target) {
        showWarningLogged(this,
                          tr("Partition Properties"),
                          tr("Select a disk, partition, or unallocated region first."));
        return;
    }
    const auto* disk = selectedDisk();
    if (!disk) {
        showWarningLogged(this, tr("Partition Properties"), tr("Selected disk was not found."));
        return;
    }

    QVector<PropertyRow> rows;
    appendDiskProperties(&rows, *disk);
    appendSmartProperties(&rows, *disk);
    QString title = tr("Disk %1 Properties").arg(disk->disk_number);
    if (target->kind == PartitionTargetKind::Partition ||
        target->kind == PartitionTargetKind::Volume) {
        if (const auto* partition = selectedPartition()) {
            appendPartitionProperties(&rows, *partition);
            title = tr("Disk %1 Partition %2 Properties")
                        .arg(disk->disk_number)
                        .arg(partition->partition_number);
        }
    } else if (target->kind == PartitionTargetKind::Unallocated) {
        appendUnallocatedProperties(&rows, *target);
        title = tr("Disk %1 Unallocated Space Properties").arg(disk->disk_number);
    }
    showPropertiesDialog(this, title, rows);
}

bool PartitionManagerPanel::showApplyReviewDialog() {
    const auto operations = m_controller->queue().operations();
    if (operations.isEmpty()) {
        Q_EMIT statusMessage(tr("No partition operations queued"), sak::kTimerStatusDefaultMs);
        return false;
    }
    if (!m_controller->queue().canApply(m_controller->inventory().layout_hash)) {
        showWarningLogged(this,
                          tr("Apply Partition Operations"),
                          tr("Pending operations have blockers or the disk layout changed. Refresh "
                             "and fix blockers before applying."));
        return false;
    }

    QDialog dialog(this);
    dialog.setWindowTitle(tr("Apply Partition Operations"));
    dialog.setModal(true);
    dialog.setMinimumSize(kApplyReviewDialogMinWidth, kApplyReviewDialogMinHeight);
    dialog.setAccessibleName(tr("Apply partition operations review"));

    auto* layout = new QVBoxLayout(&dialog);
    layout->setContentsMargins(
        ui::kMarginMedium, ui::kMarginMedium, ui::kMarginMedium, ui::kMarginMedium);
    layout->setSpacing(ui::kSpacingMedium);

    auto* diffPreview = new ApplyLayoutDiffWidget(&dialog);
    diffPreview->configure(m_controller->inventory(), operations);
    layout->addWidget(diffPreview);

    auto* review = new QTextEdit(&dialog);
    review->setReadOnly(true);
    review->setPlainText(applyReviewText(operations, m_controller->inventory()));
    review->setAccessibleName(tr("Pending partition operation review"));
    layout->addWidget(review);

    auto* warning = new QLabel(tr("Applying can permanently change disk layout. Confirm backup and "
                                  "stable power before continuing."),
                               &dialog);
    warning->setWordWrap(true);
    warning->setAccessibleName(tr("Apply partition operations warning"));
    layout->addWidget(warning);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Apply | QDialogButtonBox::Cancel,
                                         &dialog);
    connect(
        buttons->button(QDialogButtonBox::Apply), &QPushButton::clicked, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addWidget(buttons);

    return dialog.exec() == QDialog::Accepted;
}

void PartitionManagerPanel::showPartitionContextMenu(const QPoint& position) {
    if (auto* item = m_table->itemAt(position)) {
        m_table->selectRow(item->row());
    }
    showSelectedTargetContextMenuAt(m_table->viewport()->mapToGlobal(position));
}

void PartitionManagerPanel::showSelectedTargetContextMenuAt(const QPoint& global_position) {
    const auto target = selectedTarget();
    QMenu menu(this);
    menu.setAccessibleName(tr("Partition context menu"));

    if (target) {
        switch (target->kind) {
        case PartitionTargetKind::Disk:
            addDiskContextMenuActions(menu);
            break;
        case PartitionTargetKind::Partition:
        case PartitionTargetKind::Volume: {
            const auto* partition = selectedPartition();
            const bool hasDriveLetter = partition && partition->volume &&
                                        !partition->volume->drive_letter.isEmpty();
            addPartitionContextMenuActions(menu, hasDriveLetter);
            break;
        }
        case PartitionTargetKind::Unallocated:
            addUnallocatedContextMenuActions(menu);
            break;
        }
    }
    addContextMenuAction(menu,
                         this,
                         {tr("Properties"),
                          kIconProperties,
                          target.has_value(),
                          tr("Select a disk, partition, or unallocated region first.")},
                         [this]() { onShowProperties(); });
    menu.exec(global_position);
}

void PartitionManagerPanel::addUnallocatedContextMenuActions(QMenu& menu) {
    addContextMenuAction(menu, this, {tr("Create Partition"), kIconCreate}, [this]() {
        onCreatePartition();
    });
    addContextMenuAction(
        menu,
        this,
        {tr("Allocate Free Space To"),
         kIconResize,
         true,
         tr("Extend the previous partition or move the following partition into this free space.")},
        [this]() { onAllocateFreeSpace(); });
    addContextMenuAction(menu, this, {tr("Partition Recovery Wizard"), kIconRecovery}, [this]() {
        onPartitionRecoveryWizard();
    });
    addContextMenuAction(menu,
                         this,
                         {tr("Data Recovery"),
                          kIconRecovery,
                          true,
                          tr("Recover files from an image or raw volume/device path.")},
                         [this]() { onDataRecovery(); });
    addContextMenuAction(menu, this, {tr("Surface Test"), kIconSurface}, [this]() {
        onSurfaceTest();
    });
    menu.addSeparator();
}

void PartitionManagerPanel::addPartitionContextMenuActions(QMenu& menu, bool has_drive_letter) {
    Q_UNUSED(has_drive_letter);
    const auto* partition = selectedPartition();
    addPartitionLayoutContextMenuActions(menu, partition);
    addPartitionFilesystemContextMenuActions(menu, partition);
    addPartitionMaintenanceContextMenuActions(menu, partition);
    addPartitionAdvancedContextMenuActions(menu);
    menu.addSeparator();
}

void PartitionManagerPanel::addPartitionLayoutContextMenuActions(QMenu& menu,
                                                                 const PartitionInfoEx* partition) {
    addContextMenuAction(
        menu,
        this,
        partitionContextActionSpec(tr("Explore"), kIconProperties, partition, driveLetterPolicy()),
        [this]() { onExploreSelected(); });
    addContextMenuAction(menu,
                         this,
                         partitionContextActionSpec(tr("Resize/Move Partition"),
                                                    kIconResize,
                                                    partition,
                                                    resizeFilesystemPolicy()),
                         [this]() { onResizePartition(); });
    addContextMenuAction(menu,
                         this,
                         partitionContextActionSpec(tr("Extend Partition"),
                                                    kIconResize,
                                                    partition,
                                                    resizeFilesystemPolicy()),
                         [this]() { onResizePartition(); });
    addContextMenuAction(menu, this, {tr("Allocate Free Space"), kIconResize}, [this]() {
        onAllocateFreeSpace();
    });
    addContextMenuAction(menu, this, {tr("Merge Partitions"), kIconSplit}, [this]() {
        onMergePartitions();
    });
    addContextMenuAction(menu, this, {tr("Split Partition"), kIconSplit}, [this]() {
        onSplitPartition();
    });
    addContextMenuAction(menu, this, {tr("Copy Partition Wizard"), kIconCopy}, [this]() {
        onCopyPartitionWizard();
    });
    addContextMenuAction(menu,
                         this,
                         {tr("Data Recovery"),
                          kIconRecovery,
                          true,
                          tr("Recover files from an image or raw volume/device path.")},
                         [this]() { onDataRecovery(); });
    addContextMenuAction(menu, this, {tr("Create Partition Image"), kIconCopy}, [this]() {
        onCreateImage();
    });
    addContextMenuAction(menu, this, {tr("Delete Partition"), kIconDelete}, [this]() {
        onDeletePartition();
    });
    addContextMenuAction(menu, this, {tr("Format Partition"), kIconDisk}, [this]() {
        onFormatPartition();
    });
}

void PartitionManagerPanel::addPartitionFilesystemContextMenuActions(
    QMenu& menu, const PartitionInfoEx* partition) {
    addContextMenuAction(menu, this, {tr("Change Drive Letter"), kIconProperties}, [this]() {
        onSetDriveLetter();
    });
    addContextMenuAction(menu,
                         this,
                         partitionContextActionSpec(
                             tr("Change Label"), kIconProperties, partition, windowsNativePolicy()),
                         [this]() { onSetPartitionLabel(); });
    addContextMenuAction(menu,
                         this,
                         partitionContextActionSpec(tr("Convert File System"),
                                                    kIconConvert,
                                                    partition,
                                                    windowsNativePolicy()),
                         [this]() { onConvertFileSystem(); });
    addContextMenuAction(menu,
                         this,
                         partitionContextActionSpec(tr("Change Cluster Size"),
                                                    kIconProperties,
                                                    partition,
                                                    windowsNativePolicy()),
                         [this]() { onChangeClusterSize(); });
    addContextMenuAction(menu,
                         this,
                         partitionContextActionSpec(tr("Check File System"),
                                                    kIconSurface,
                                                    partition,
                                                    windowsNativePolicy()),
                         [this]() { onCheckFileSystem(); });
    const auto nonNativeInspect = nonNativeFilesystemInspectState(selectedPartition());
    addContextMenuAction(menu,
                         this,
                         {tr("Inspect Non-Windows File System"),
                          kIconProperties,
                          nonNativeInspect.enabled,
                          nonNativeInspect.reason},
                         [this]() { onInspectNonNativeFileSystem(); });
    const auto nonNativeBrowse = nonNativeFilesystemBrowseState(selectedTarget(),
                                                                selectedPartition());
    addContextMenuAction(menu,
                         this,
                         {tr("Browse Non-Windows File System"),
                          kIconProperties,
                          nonNativeBrowse.enabled,
                          nonNativeBrowse.reason},
                         [this]() { onBrowseNonNativeFileSystem(); });
    const auto nonNativeCheck = nonNativeFilesystemCheckState(selectedTarget(),
                                                              selectedPartition());
    addContextMenuAction(menu,
                         this,
                         {tr("Check Non-Windows File System"),
                          kIconSurface,
                          nonNativeCheck.enabled,
                          nonNativeCheck.reason},
                         [this]() { onCheckNonNativeFileSystem(); });
    const auto apfsMutation = apfsRootFileMutationState(selectedTarget(), selectedPartition());
    addContextMenuAction(
        menu,
        this,
        {tr("APFS File"), kIconProperties, apfsMutation.enabled, apfsMutation.reason},
        [this]() { onApfsRootFileMutation(); });
    const auto hfsMutation = hfsFileMutationState(selectedTarget(), selectedPartition());
    addContextMenuAction(menu,
                         this,
                         {tr("HFS File"), kIconProperties, hfsMutation.enabled, hfsMutation.reason},
                         [this]() { onHfsFileMutation(); });
}

void PartitionManagerPanel::addPartitionMaintenanceContextMenuActions(
    QMenu& menu, const PartitionInfoEx* partition) {
    addContextMenuAction(menu, this, {tr("Align Partition"), kIconAlign}, [this]() {
        onOptimizeSsd();
    });
    addContextMenuAction(menu, this, {tr("Wipe Partition / Free Space"), kIconWipe}, [this]() {
        onWipeSelected();
    });
    addContextMenuAction(menu, this, {tr("Surface Test"), kIconSurface}, [this]() {
        onSurfaceTest();
    });
    addContextMenuAction(menu, this, {tr("Disk Benchmark"), kIconSurface}, [this]() {
        onOpenDiskBenchmark();
    });
    addContextMenuAction(menu,
                         this,
                         partitionContextActionSpec(
                             tr("Space Analyzer"), kIconSurface, partition, driveLetterPolicy()),
                         [this]() { onSpaceAnalyzer(); });
    addContextMenuAction(menu, this, {tr("Hide/Unhide Partition"), kIconProperties}, [this]() {
        onSetPartitionHidden();
    });
    addContextMenuAction(menu,
                         this,
                         partitionContextActionSpec(tr("Manage BitLocker"),
                                                    kIconProperties,
                                                    partition,
                                                    driveLetterPolicy()),
                         [this]() { onManageBitLocker(); });
}

void PartitionManagerPanel::addPartitionAdvancedContextMenuActions(QMenu& menu) {
    addContextMenuAction(menu, this, {tr("Set Active/Inactive"), kIconProperties}, [this]() {
        onSetPartitionActive();
    });
    addContextMenuAction(menu, this, {tr("Change Partition Type ID"), kIconProperties}, [this]() {
        onSetPartitionTypeId();
    });
    addContextMenuAction(menu, this, {tr("Convert Primary/Logical"), kIconConvert}, [this]() {
        onConvertPrimaryLogical();
    });
    addContextMenuAction(menu, this, {tr("Change Serial Number"), kIconProperties}, [this]() {
        onChangeVolumeSerialNumber();
    });
}

void PartitionManagerPanel::addDiskContextMenuActions(QMenu& menu) {
    addContextMenuAction(menu, this, {tr("Migrate OS to SSD/HDD Wizard"), kIconOsDrive}, [this]() {
        onMigrateOs();
    });
    addContextMenuAction(menu, this, {tr("Copy Disk Wizard"), kIconDisk}, [this]() {
        onCloneDisk();
    });
    addContextMenuAction(menu, this, {tr("Create Disk Image"), kIconCopy}, [this]() {
        onCreateImage();
    });
    addContextMenuAction(menu, this, {tr("Restore Disk Image"), kIconCopy}, [this]() {
        onRestoreImage();
    });
    addContextMenuAction(menu, this, {tr("Partition Recovery Wizard"), kIconRecovery}, [this]() {
        onPartitionRecoveryWizard();
    });
    addContextMenuAction(menu,
                         this,
                         {tr("Data Recovery"),
                          kIconRecovery,
                          true,
                          tr("Recover files from an image or raw volume/device path.")},
                         [this]() { onDataRecovery(); });
    addContextMenuAction(menu, this, {tr("Quick Partition"), kIconCreate}, [this]() {
        onQuickPartition();
    });
    addContextMenuAction(menu, this, {tr("Convert MBR/GPT"), kIconConvert}, [this]() {
        onConvertStyle();
    });
    addContextMenuAction(menu, this, {tr("Rebuild MBR / Boot Repair"), kIconSurface}, [this]() {
        onRepairBoot();
    });
    addContextMenuAction(menu,
                         this,
                         {tr("Align All Partitions / SSD Optimize"), kIconAlign},
                         [this]() { onOptimizeSsd(); });
    addContextMenuAction(menu, this, {tr("Wipe Disk"), kIconWipe}, [this]() { onWipeSelected(); });
    addContextMenuAction(menu, this, {tr("SSD Secure Erase"), kIconWipe}, [this]() {
        onSsdSecureErase();
    });
    addContextMenuAction(menu, this, {tr("Initialize to MBR/GPT"), kIconDisk}, [this]() {
        onInitializeDisk();
    });
    addContextMenuAction(menu, this, {tr("Delete All Partitions"), kIconDelete}, [this]() {
        onDeleteAllPartitions();
    });
    addContextMenuAction(menu, this, {tr("Convert Dynamic Disk to Basic"), kIconConvert}, [this]() {
        onConvertDynamicDiskToBasic();
    });
    addContextMenuAction(menu, this, {tr("Disk Surface Test"), kIconSurface}, [this]() {
        onSurfaceTest();
    });
    addContextMenuAction(menu, this, {tr("Disk Benchmark"), kIconSurface}, [this]() {
        onOpenDiskBenchmark();
    });
    addContextMenuAction(menu,
                         this,
                         {tr("Space Analyzer"),
                          kIconSurface,
                          false,
                          tr("Select a mounted partition to analyze volume space.")},
                         [this]() {});
    addContextMenuAction(menu, this, {tr("Disk Defrag"), kIconAlign}, [this]() {
        onOpenOptimizeDrives();
    });
    addContextMenuAction(menu, this, {tr("Make Bootable Media"), kIconOsDrive}, [this]() {
        onOpenBootableMedia();
    });
    menu.addSeparator();
}

void PartitionManagerPanel::onCreatePartition() {
    const auto target = selectedTarget();
    if (!target || target->kind != PartitionTargetKind::Unallocated) {
        showWarningLogged(this,
                          tr("Create Partition"),
                          tr("Select unallocated space before creating a partition."));
        return;
    }

    PartitionOperationDialog dialog(tr("Create Partition"),
                                    targetIdentityText(target, selectedDisk(), selectedPartition()),
                                    tr("Queued only. Review Pending Operations before Apply."),
                                    this);
    const auto widgets = addCreatePartitionControls(dialog, target->size_bytes, selectedDisk());
    auto updatePreview = [&]() {
        updateCreatePartitionPreview(dialog, widgets, target->size_bytes);
    };
    connect(widgets.size_mb, &QSpinBox::valueChanged, &dialog, updatePreview);
    connect(widgets.free_before_mb, &QSpinBox::valueChanged, &dialog, updatePreview);
    connect(widgets.size_handle, &QSlider::valueChanged, &dialog, [&widgets](int value) {
        widgets.size_mb->setValue(value);
    });
    connect(widgets.location_handle, &QSlider::valueChanged, &dialog, [&widgets](int value) {
        widgets.free_before_mb->setValue(value);
    });
    widgets.size_preview->setInteractiveSegment(
        kSizePreviewCreateRow, true, true, [&widgets](uint64_t offsetBytes, uint64_t sizeBytes) {
            widgets.size_mb->setValue(inputMegabytesFromBytes(
                sizeBytes, widgets.size_mb->minimum(), widgets.size_mb->maximum()));
            widgets.free_before_mb->setValue(inputMegabytesFromBytes(
                offsetBytes, widgets.free_before_mb->minimum(), widgets.free_before_mb->maximum()));
        });
    connect(widgets.partition_type, &QComboBox::currentTextChanged, &dialog, updatePreview);
    connect(widgets.file_system, &QComboBox::currentTextChanged, &dialog, updatePreview);
    connect(widgets.allocation_unit, &QComboBox::currentTextChanged, &dialog, updatePreview);
    connect(widgets.swap_page_size, &QComboBox::currentTextChanged, &dialog, updatePreview);
    connect(widgets.label, &QLineEdit::textChanged, &dialog, updatePreview);
    connect(widgets.raw_format_confirm, &QCheckBox::toggled, &dialog, updatePreview);
    updatePreview();
    if (dialog.exec() != QDialog::Accepted) {
        return;
    }

    queueOperation(PartitionOperationType::Create, createPartitionPayload(widgets));
}

void PartitionManagerPanel::onDeletePartition() {
    queueOperation(PartitionOperationType::Delete);
}

void PartitionManagerPanel::onFormatPartition() {
    const auto target = selectedTarget();
    const auto* partition = selectedPartition();
    if (!target || !partition) {
        showWarningLogged(this, tr("Format Partition"), tr("Select a partition before format."));
        return;
    }

    PartitionOperationDialog dialog(tr("Format Partition"),
                                    targetIdentityText(target, selectedDisk(), partition),
                                    tr("Format destroys files. Queued only until Apply."),
                                    this);
    const FormatPartitionWidgets widgets = addFormatPartitionControls(dialog, *partition);
    connectFormatPartitionControls(dialog, widgets);
    updateFormatPartitionPreview(dialog, widgets);
    if (dialog.exec() != QDialog::Accepted) {
        return;
    }

    queueOperation(PartitionOperationType::Format, formatPartitionPayload(widgets, *partition));
}

void PartitionManagerPanel::onSetDriveLetter() {
    bool ok = false;
    const QString letter =
        QInputDialog::getText(
            this, tr("Drive Letter"), tr("New drive letter:"), QLineEdit::Normal, QString(), &ok)
            .left(1)
            .toUpper();
    if (ok && !letter.isEmpty()) {
        queueOperation(PartitionOperationType::SetDriveLetter,
                       withValue(QStringLiteral("new_drive_letter"), letter));
    }
}

void PartitionManagerPanel::onSetPartitionLabel() {
    const auto* partition = selectedPartition();
    if (!partition || !partition->volume || partition->volume->drive_letter.isEmpty()) {
        showWarningLogged(this,
                          tr("Change Label"),
                          tr("Select a mounted partition before changing its label."));
        return;
    }
    const auto availability = partitionActionAvailability(partition, windowsNativePolicy());
    if (!availability.enabled) {
        showWarningLogged(this, tr("Change Label"), availability.reason);
        return;
    }

    bool ok = false;
    const QString label = QInputDialog::getText(this,
                                                tr("Change Label"),
                                                tr("New partition label:"),
                                                QLineEdit::Normal,
                                                partition->volume->label,
                                                &ok);
    if (ok) {
        queueOperation(PartitionOperationType::SetPartitionLabel,
                       withValue(QStringLiteral("label"), label));
    }
}

void PartitionManagerPanel::onExploreSelected() {
    const auto* partition = selectedPartition();
    if (!partition || !partition->volume || partition->volume->drive_letter.isEmpty()) {
        showWarningLogged(this,
                          tr("Explore Partition"),
                          tr("Select a mounted partition with a drive letter first."));
        return;
    }

    const QUrl location =
        QUrl::fromLocalFile(QStringLiteral("%1:/").arg(partition->volume->drive_letter.left(1)));
    if (!QDesktopServices::openUrl(location)) {
        Q_EMIT statusMessage(tr("Could not open selected partition"), sak::kTimerStatusDefaultMs);
    }
}

void PartitionManagerPanel::onCheckFileSystem() {
    const auto availability = partitionActionAvailability(selectedPartition(),
                                                          windowsNativePolicy());
    if (!availability.enabled) {
        showWarningLogged(this, tr("Check File System"), availability.reason);
        return;
    }
    queueOperation(PartitionOperationType::CheckFileSystem);
}

void PartitionManagerPanel::onInspectNonNativeFileSystem() {
    const auto* partition = selectedPartition();
    const auto state = nonNativeFilesystemInspectState(partition);
    if (!state.enabled || !partition || !partition->volume) {
        showWarningLogged(this, tr("Inspect Non-Windows File System"), state.reason);
        return;
    }

    QVector<PropertyRow> rows;
    appendFilesystemInspectionRows(&rows,
                                   selectedDisk(),
                                   *partition,
                                   nonNativeFilesystemTargetPath(selectedTarget(), partition));
    showPropertiesDialog(this, tr("Inspect %1 File System").arg(state.file_system), rows);
    Q_EMIT statusMessage(tr("Reviewed read-only %1 metadata").arg(state.file_system),
                         sak::kTimerStatusDefaultMs);
}

void PartitionManagerPanel::onBrowseNonNativeFileSystem() {
    const auto state = nonNativeFilesystemBrowseState(selectedTarget(), selectedPartition());
    if (!state.enabled) {
        showWarningLogged(this, tr("Browse Non-Windows File System"), state.reason);
        return;
    }

    const auto request = showNonNativeBrowseRequestDialog(this, state);
    if (!request.has_value() || request->target_path.isEmpty()) {
        Q_EMIT statusMessage(tr("Filesystem browse cancelled"), sak::kTimerStatusDefaultMs);
        return;
    }

    const auto result = browseNonNativeFileSystem(this, state, *request);
    if (!result.blockers.isEmpty()) {
        showWarningLogged(this,
                          tr("Browse Non-Windows File System"),
                          result.blockers.join(QStringLiteral("\n")));
        return;
    }
    Q_EMIT statusMessage(tr("Listed %1 read-only filesystem entries").arg(result.entries),
                         sak::kTimerStatusDefaultMs);
}

enum class NonNativeCheckActionKind {
    QueueRepair,
    StatusOnly,
    RunReadOnlyTool,
};

struct NonNativeCheckAction {
    NonNativeCheckActionKind kind{NonNativeCheckActionKind::StatusOnly};
    QJsonObject payload;
    QString target_path;
    QString status;
};

QString metadataConsistencyStatusText(const NonNativeFilesystemCheckState& state) {
    if (hasMetadataSanityWarnings(state.metadata_details)) {
        return QObject::tr("Reviewed %1 metadata sanity warnings").arg(state.file_system);
    }
    return QObject::tr("Reviewed %1 metadata consistency").arg(state.file_system);
}

void showMetadataConsistencyWithStatus(QWidget* parent,
                                       const NonNativeFilesystemCheckState& state,
                                       QString* status) {
    showMetadataConsistencyDialog(parent, state);
    *status = metadataConsistencyStatusText(state);
}

std::optional<QString> showImmediateMetadataCheck(QWidget* parent,
                                                  const NonNativeFilesystemCheckState& state) {
    if (!state.internal_metadata_check || state.repair_available) {
        return std::nullopt;
    }
    QString status;
    showMetadataConsistencyWithStatus(parent, state, &status);
    return status;
}

QJsonObject nonNativeRepairPayload(const NonNativeFilesystemCheckState& state,
                                   const NonNativeFilesystemCheckRequest& request) {
    return QJsonObject{{QStringLiteral("non_native_file_system_tool"), true},
                       {QStringLiteral("file_system"), state.file_system},
                       {QStringLiteral("target_path"), request.target_path},
                       {QStringLiteral("target_wipe_confirmed"), request.destructive_confirmed}};
}

QString showHfsConsistencyWithStatus(QWidget* parent, const NonNativeFilesystemCheckState& state) {
    const auto result = showHfsConsistencyDialog(parent, state);
    if (result.ok) {
        return QObject::tr("Reviewed %1 catalog and attribute keys").arg(state.file_system);
    }
    return QObject::tr("Reviewed %1 catalog/attribute blockers").arg(state.file_system);
}

NonNativeCheckAction resolveNonNativeCheckAction(QWidget* parent,
                                                 const NonNativeFilesystemCheckState& state,
                                                 const NonNativeFilesystemCheckRequest& request) {
    if (request.mode == PartitionFileSystemToolRunner::repairOperation()) {
        return {NonNativeCheckActionKind::QueueRepair, nonNativeRepairPayload(state, request)};
    }
    if (nonNativeCheckHfsCatalogMode(request.mode)) {
        return {NonNativeCheckActionKind::StatusOnly,
                {},
                {},
                showHfsConsistencyWithStatus(parent, state)};
    }
    if (state.internal_metadata_check) {
        QString status;
        showMetadataConsistencyWithStatus(parent, state, &status);
        return {NonNativeCheckActionKind::StatusOnly, {}, {}, status};
    }
    return {NonNativeCheckActionKind::RunReadOnlyTool, {}, request.target_path};
}

struct ApfsRootFileMutationDialogWidgets {
    PartitionOperationDialog* dialog{nullptr};
    QComboBox* mode{nullptr};
    QLineEdit* directory_name{nullptr};
    QLineEdit* file_name{nullptr};
    QTextEdit* payload{nullptr};
    QLineEdit* patch_offset{nullptr};
    QCheckBox* compress{nullptr};
    QCheckBox* confirm{nullptr};
};

struct ApfsRootFileMutationRequest {
    PartitionOperationType type{PartitionOperationType::ApfsWriteRootFile};
    QString directory_name;
    QString entry_name;
    QString payload_text;
    uint64_t patch_offset_bytes{0};
    bool compress_zlib{false};
};

// A5: inline zlib transparent compression (com.apple.decmpfs) only applies to new file
// writes (commit-raw-file-write / commit-raw-directory-child-write), not patch/delete.
bool apfsMutationSupportsCompression(PartitionOperationType type) {
    return type == PartitionOperationType::ApfsWriteRootFile ||
           type == PartitionOperationType::ApfsWriteRootDirectoryFile;
}

PartitionOperationType apfsMutationTypeForMode(const QString& mode) {
    if (mode == QString::fromLatin1(kApfsRootFilePatchMode)) {
        return PartitionOperationType::ApfsPatchRootFile;
    }
    if (mode == QString::fromLatin1(kApfsRootFileDeleteMode)) {
        return PartitionOperationType::ApfsDeleteRootFile;
    }
    if (mode == QString::fromLatin1(kApfsRootDirectoryFileWriteMode)) {
        return PartitionOperationType::ApfsWriteRootDirectoryFile;
    }
    if (mode == QString::fromLatin1(kApfsRootDirectoryFilePatchMode)) {
        return PartitionOperationType::ApfsPatchRootDirectoryFile;
    }
    if (mode == QString::fromLatin1(kApfsRootDirectoryFileDeleteMode)) {
        return PartitionOperationType::ApfsDeleteRootDirectoryFile;
    }
    if (mode == QString::fromLatin1(kApfsRootDirectoryCreateMode)) {
        return PartitionOperationType::ApfsCreateRootDirectory;
    }
    if (mode == QString::fromLatin1(kApfsRootDirectoryDeleteMode)) {
        return PartitionOperationType::ApfsDeleteRootDirectory;
    }
    if (mode == QString::fromLatin1(kApfsVolumeLabelMode)) {
        return PartitionOperationType::ApfsChangeVolumeLabel;
    }
    return PartitionOperationType::ApfsWriteRootFile;
}

bool apfsMutationNeedsPayload(PartitionOperationType type) {
    return type == PartitionOperationType::ApfsWriteRootFile ||
           type == PartitionOperationType::ApfsWriteRootDirectoryFile ||
           type == PartitionOperationType::ApfsPatchRootDirectoryFile ||
           type == PartitionOperationType::ApfsPatchRootFile;
}

bool apfsMutationIsDirectory(PartitionOperationType type) {
    return type == PartitionOperationType::ApfsCreateRootDirectory ||
           type == PartitionOperationType::ApfsDeleteRootDirectory;
}

bool apfsMutationIsDirectoryFile(PartitionOperationType type) {
    return type == PartitionOperationType::ApfsWriteRootDirectoryFile ||
           type == PartitionOperationType::ApfsPatchRootDirectoryFile ||
           type == PartitionOperationType::ApfsDeleteRootDirectoryFile;
}

bool apfsMutationIsVolumeLabel(PartitionOperationType type) {
    return type == PartitionOperationType::ApfsChangeVolumeLabel;
}

QString apfsMutationPreview(PartitionOperationType type, const QString& entryName) {
    switch (type) {
    case PartitionOperationType::ApfsWriteRootFile:
        return QObject::tr("Queue APFS generated root-file write for %1.").arg(entryName);
    case PartitionOperationType::ApfsPatchRootFile:
        return QObject::tr("Queue APFS generated root-file patch for %1.").arg(entryName);
    case PartitionOperationType::ApfsDeleteRootFile:
        return QObject::tr("Queue APFS generated root-file delete for %1.").arg(entryName);
    case PartitionOperationType::ApfsWriteRootDirectoryFile:
        return QObject::tr("Queue APFS generated root-directory child-file write for %1.")
            .arg(entryName);
    case PartitionOperationType::ApfsPatchRootDirectoryFile:
        return QObject::tr("Queue APFS generated root-directory child-file patch for %1.")
            .arg(entryName);
    case PartitionOperationType::ApfsDeleteRootDirectoryFile:
        return QObject::tr("Queue APFS generated root-directory child-file delete for %1.")
            .arg(entryName);
    case PartitionOperationType::ApfsCreateRootDirectory:
        return QObject::tr("Queue APFS generated empty root-directory create for %1.")
            .arg(entryName);
    case PartitionOperationType::ApfsDeleteRootDirectory:
        return QObject::tr("Queue APFS generated empty root-directory delete for %1.")
            .arg(entryName);
    case PartitionOperationType::ApfsChangeVolumeLabel:
        return QObject::tr("Queue APFS generated volume-label change to %1.").arg(entryName);
    default:
        return {};
    }
}

std::optional<uint64_t> parsedPatchOffset(const QString& text) {
    bool ok = false;
    const uint64_t value = text.trimmed().toULongLong(&ok);
    return ok ? std::optional<uint64_t>(value) : std::nullopt;
}

void applyApfsMutationModeControls(const ApfsRootFileMutationDialogWidgets& widgets,
                                   PartitionOperationType type) {
    const bool needsPayload = apfsMutationNeedsPayload(type);
    const bool patchMode = type == PartitionOperationType::ApfsPatchRootFile ||
                           type == PartitionOperationType::ApfsPatchRootDirectoryFile;
    const bool directoryFileMode = apfsMutationIsDirectoryFile(type);
    const bool compressMode = apfsMutationSupportsCompression(type);
    widgets.payload->setEnabled(needsPayload);
    widgets.payload->setVisible(needsPayload);
    widgets.patch_offset->setEnabled(patchMode);
    widgets.patch_offset->setVisible(patchMode);
    widgets.directory_name->setEnabled(directoryFileMode);
    widgets.directory_name->setVisible(directoryFileMode);
    widgets.compress->setEnabled(compressMode);
    widgets.compress->setVisible(compressMode);
}

QString apfsMutationFilePlaceholder(PartitionOperationType type) {
    if (apfsMutationIsDirectory(type)) {
        return QObject::tr("Root directory name");
    }
    if (apfsMutationIsDirectoryFile(type)) {
        return QObject::tr("Child file name");
    }
    if (apfsMutationIsVolumeLabel(type)) {
        return QObject::tr("Volume label");
    }
    return QObject::tr("Root file name");
}

QString apfsMutationPreviewName(PartitionOperationType type,
                                const QString& directoryName,
                                const QString& entryName) {
    if (!apfsMutationIsDirectoryFile(type) || directoryName.isEmpty() || entryName.isEmpty()) {
        return entryName;
    }
    return QStringLiteral("%1/%2").arg(directoryName, entryName);
}

QString apfsMutationFallbackName(PartitionOperationType type) {
    if (apfsMutationIsDirectory(type)) {
        return QObject::tr("(root directory)");
    }
    if (apfsMutationIsDirectoryFile(type)) {
        return QObject::tr("(directory/file)");
    }
    if (apfsMutationIsVolumeLabel(type)) {
        return QObject::tr("(volume label)");
    }
    return QObject::tr("(root file)");
}

bool apfsMutationDialogCanAccept(const ApfsRootFileMutationDialogWidgets& widgets,
                                 PartitionOperationType type,
                                 const QString& directoryName,
                                 const QString& entryName) {
    const bool needsPayload = apfsMutationNeedsPayload(type);
    const bool patchMode = type == PartitionOperationType::ApfsPatchRootFile ||
                           type == PartitionOperationType::ApfsPatchRootDirectoryFile;
    const bool directoryFileMode = apfsMutationIsDirectoryFile(type);
    const bool hasPayload = !needsPayload || !widgets.payload->toPlainText().isEmpty();
    const bool hasOffset = !patchMode ||
                           parsedPatchOffset(widgets.patch_offset->text()).has_value();
    const bool hasDirectory = !directoryFileMode || !directoryName.isEmpty();
    return !entryName.isEmpty() && hasDirectory && hasPayload && hasOffset &&
           widgets.confirm->isChecked();
}

void syncApfsRootFileMutationDialog(const ApfsRootFileMutationDialogWidgets& widgets) {
    const auto type = apfsMutationTypeForMode(widgets.mode->currentData().toString());
    applyApfsMutationModeControls(widgets, type);
    widgets.file_name->setPlaceholderText(apfsMutationFilePlaceholder(type));
    const QString entryName = widgets.file_name->text().trimmed();
    const QString directoryName = widgets.directory_name->text().trimmed();
    const QString previewName = apfsMutationPreviewName(type, directoryName, entryName);
    widgets.dialog->setAcceptEnabled(
        apfsMutationDialogCanAccept(widgets, type, directoryName, entryName));
    widgets.dialog->setPreviewText(apfsMutationPreview(
        type, previewName.isEmpty() ? apfsMutationFallbackName(type) : previewName));
}

void populateApfsRootFileMutationModes(QComboBox* mode) {
    mode->setAccessibleName(QObject::tr("APFS generated file mutation mode"));
    mode->addItem(QObject::tr("Write root file"), QString::fromLatin1(kApfsRootFileWriteMode));
    mode->addItem(QObject::tr("Patch root file"), QString::fromLatin1(kApfsRootFilePatchMode));
    mode->addItem(QObject::tr("Delete root file"), QString::fromLatin1(kApfsRootFileDeleteMode));
    mode->addItem(QObject::tr("Write file in root directory"),
                  QString::fromLatin1(kApfsRootDirectoryFileWriteMode));
    mode->addItem(QObject::tr("Patch file in root directory"),
                  QString::fromLatin1(kApfsRootDirectoryFilePatchMode));
    mode->addItem(QObject::tr("Delete file in root directory"),
                  QString::fromLatin1(kApfsRootDirectoryFileDeleteMode));
    mode->addItem(QObject::tr("Create empty root directory"),
                  QString::fromLatin1(kApfsRootDirectoryCreateMode));
    mode->addItem(QObject::tr("Delete empty root directory"),
                  QString::fromLatin1(kApfsRootDirectoryDeleteMode));
    mode->addItem(QObject::tr("Change volume label"), QString::fromLatin1(kApfsVolumeLabelMode));
}

void connectApfsRootFileMutationDialog(PartitionOperationDialog& dialog,
                                       const ApfsRootFileMutationDialogWidgets& widgets) {
    auto updatePreview = [widgets]() {
        syncApfsRootFileMutationDialog(widgets);
    };
    QObject::connect(widgets.mode, &QComboBox::currentTextChanged, &dialog, updatePreview);
    QObject::connect(widgets.directory_name, &QLineEdit::textChanged, &dialog, updatePreview);
    QObject::connect(widgets.file_name, &QLineEdit::textChanged, &dialog, updatePreview);
    QObject::connect(widgets.payload, &QTextEdit::textChanged, &dialog, updatePreview);
    QObject::connect(widgets.patch_offset, &QLineEdit::textChanged, &dialog, updatePreview);
    QObject::connect(widgets.confirm, &QCheckBox::toggled, &dialog, updatePreview);
    syncApfsRootFileMutationDialog(widgets);
}

std::optional<ApfsRootFileMutationRequest> showApfsRootFileMutationDialog(
    QWidget* parent, const ApfsRootFileMutationState& state) {
    PartitionOperationDialog dialog(
        QObject::tr("APFS Generated File"),
        state.target_path,
        QObject::tr("Queue a S.A.K. generated APFS file or directory mutation."),
        parent);
    auto* mode = new QComboBox(&dialog);
    populateApfsRootFileMutationModes(mode);

    auto* directoryName = new QLineEdit(&dialog);
    directoryName->setAccessibleName(QObject::tr("APFS root directory name"));
    auto* fileName = new QLineEdit(&dialog);
    fileName->setAccessibleName(QObject::tr("APFS file or directory name"));
    auto* payload = new QTextEdit(&dialog);
    payload->setAcceptRichText(false);
    payload->setAccessibleName(QObject::tr("APFS file payload text"));
    payload->setMinimumHeight(kApfsRootFilePayloadMinHeight);
    auto* patchOffset = new QLineEdit(QStringLiteral("0"), &dialog);
    patchOffset->setAccessibleName(QObject::tr("APFS root file patch byte offset"));
    auto* compress = new QCheckBox(QObject::tr("Store compressed (inline zlib com.apple.decmpfs)"),
                                   &dialog);
    compress->setAccessibleName(QObject::tr("Compress APFS file with inline zlib"));
    auto* confirm = new QCheckBox(
        QObject::tr("I understand this only supports S.A.K. generated APFS layouts and will "
                    "mutate the selected raw partition on Apply."),
        &dialog);
    confirm->setAccessibleName(QObject::tr("Confirm APFS generated file mutation"));

    dialog.formLayout()->addRow(QObject::tr("Mode:"), mode);
    dialog.formLayout()->addRow(QObject::tr("Directory:"), directoryName);
    dialog.formLayout()->addRow(QObject::tr("Name:"), fileName);
    dialog.formLayout()->addRow(QObject::tr("Payload:"), payload);
    dialog.formLayout()->addRow(QObject::tr("Patch offset:"), patchOffset);
    dialog.formLayout()->addRow(QString(), compress);
    dialog.formLayout()->addRow(QString(), confirm);

    const ApfsRootFileMutationDialogWidgets widgets{
        &dialog, mode, directoryName, fileName, payload, patchOffset, compress, confirm};
    connectApfsRootFileMutationDialog(dialog, widgets);

    if (dialog.exec() != QDialog::Accepted) {
        return std::nullopt;
    }

    const auto type = apfsMutationTypeForMode(mode->currentData().toString());
    return ApfsRootFileMutationRequest{type,
                                       directoryName->text().trimmed(),
                                       fileName->text().trimmed(),
                                       payload->toPlainText(),
                                       parsedPatchOffset(patchOffset->text()).value_or(0),
                                       compress->isChecked() &&
                                           apfsMutationSupportsCompression(type)};
}

QJsonObject apfsRootFileMutationPayload(const ApfsRootFileMutationState& state,
                                        const ApfsRootFileMutationRequest& request) {
    QJsonObject payload{{QStringLiteral("non_native_file_system_tool"), true},
                        {QStringLiteral("file_system"), QStringLiteral("APFS")},
                        {QStringLiteral("target_path"), state.target_path},
                        {QStringLiteral("target_wipe_confirmed"), true},
                        {QStringLiteral("apfs_generated_layout_confirmed"), true}};
    if (apfsMutationIsVolumeLabel(request.type)) {
        payload[QStringLiteral("label")] = request.entry_name;
        return payload;
    }
    payload[QStringLiteral("apfs_root_file_name")] = request.entry_name;
    if (apfsMutationIsDirectory(request.type)) {
        payload[QStringLiteral("apfs_root_directory_name")] = request.entry_name;
    }
    if (apfsMutationIsDirectoryFile(request.type)) {
        payload[QStringLiteral("apfs_root_directory_name")] = request.directory_name;
    }
    if (apfsMutationNeedsPayload(request.type)) {
        payload[QStringLiteral("apfs_root_file_payload_text")] = request.payload_text;
    }
    if (request.compress_zlib) {
        payload[QStringLiteral("apfs_compress_zlib")] = true;
    }
    if (request.type == PartitionOperationType::ApfsPatchRootFile ||
        request.type == PartitionOperationType::ApfsPatchRootDirectoryFile) {
        payload[QStringLiteral("apfs_root_file_patch_offset_bytes")] =
            QString::number(request.patch_offset_bytes);
    }
    return payload;
}

struct HfsFileMutationDialogWidgets {
    PartitionOperationDialog* dialog{nullptr};
    QComboBox* mode{nullptr};
    QLineEdit* hfs_path{nullptr};
    QLineEdit* destination_hfs_path{nullptr};
    QTextEdit* payload{nullptr};
    QLineEdit* file_id{nullptr};
    QLineEdit* attribute_name{nullptr};
    QCheckBox* allow_journaled{nullptr};
    QCheckBox* allow_wrapped{nullptr};
    QCheckBox* secure_wipe{nullptr};
    QCheckBox* confirm{nullptr};
};

struct HfsFileMutationRequest {
    PartitionOperationType type{PartitionOperationType::HfsReplaceFile};
    QString hfs_path;
    QString destination_hfs_path;
    QString payload_text;
    uint64_t file_id{0};
    QString attribute_name;
    bool allow_journaled{false};
    bool allow_wrapped{false};
    bool secure_wipe{false};
};

PartitionOperationType hfsMutationTypeForMode(const QString& mode) {
    static const QHash<QString, PartitionOperationType> kTypes{
        {QString::fromLatin1(kHfsOverwriteFileMode), PartitionOperationType::HfsOverwriteFile},
        {QString::fromLatin1(kHfsReplaceFileMode), PartitionOperationType::HfsReplaceFile},
        {QString::fromLatin1(kHfsGrowFileMode), PartitionOperationType::HfsGrowFile},
        {QString::fromLatin1(kHfsTruncateFileMode), PartitionOperationType::HfsTruncateFile},
        {QString::fromLatin1(kHfsReplaceResourceForkMode),
         PartitionOperationType::HfsReplaceResourceFork},
        {QString::fromLatin1(kHfsGrowResourceForkMode),
         PartitionOperationType::HfsGrowResourceFork},
        {QString::fromLatin1(kHfsTruncateResourceForkMode),
         PartitionOperationType::HfsTruncateResourceFork},
        {QString::fromLatin1(kHfsCreateEmptyFileMode), PartitionOperationType::HfsCreateEmptyFile},
        {QString::fromLatin1(kHfsCreateFileMode), PartitionOperationType::HfsCreateFile},
        {QString::fromLatin1(kHfsDeleteEmptyFileMode), PartitionOperationType::HfsDeleteEmptyFile},
        {QString::fromLatin1(kHfsDeleteFileMode), PartitionOperationType::HfsDeleteFile},
        {QString::fromLatin1(kHfsCreateEmptyFolderMode),
         PartitionOperationType::HfsCreateEmptyFolder},
        {QString::fromLatin1(kHfsDeleteEmptyFolderMode),
         PartitionOperationType::HfsDeleteEmptyFolder},
        {QString::fromLatin1(kHfsDeleteFolderTreeMode),
         PartitionOperationType::HfsDeleteFolderTree},
        {QString::fromLatin1(kHfsRenameMoveCatalogEntryMode),
         PartitionOperationType::HfsRenameMoveCatalogEntry},
        {QString::fromLatin1(kHfsReplaceInlineAttributeMode),
         PartitionOperationType::HfsReplaceInlineAttribute},
        {QString::fromLatin1(kHfsReplaceForkAttributeMode),
         PartitionOperationType::HfsReplaceForkAttribute},
        {QString::fromLatin1(kHfsGrowForkAttributeMode),
         PartitionOperationType::HfsGrowForkAttribute},
    };
    return kTypes.value(mode, PartitionOperationType::HfsReplaceFile);
}

bool hfsMutationNeedsPayload(PartitionOperationType type) {
    return type == PartitionOperationType::HfsOverwriteFile ||
           type == PartitionOperationType::HfsReplaceFile ||
           type == PartitionOperationType::HfsGrowFile ||
           type == PartitionOperationType::HfsCreateFile ||
           type == PartitionOperationType::HfsReplaceResourceFork ||
           type == PartitionOperationType::HfsGrowResourceFork ||
           type == PartitionOperationType::HfsReplaceInlineAttribute ||
           type == PartitionOperationType::HfsReplaceForkAttribute ||
           type == PartitionOperationType::HfsGrowForkAttribute;
}

bool hfsMutationNeedsPath(PartitionOperationType type) {
    return type != PartitionOperationType::HfsReplaceInlineAttribute &&
           type != PartitionOperationType::HfsReplaceForkAttribute &&
           type != PartitionOperationType::HfsGrowForkAttribute;
}

bool hfsMutationNeedsDestinationPath(PartitionOperationType type) {
    return type == PartitionOperationType::HfsRenameMoveCatalogEntry;
}

bool hfsMutationIsAttribute(PartitionOperationType type) {
    return type == PartitionOperationType::HfsReplaceInlineAttribute ||
           type == PartitionOperationType::HfsReplaceForkAttribute ||
           type == PartitionOperationType::HfsGrowForkAttribute;
}

bool hfsMutationCanSecureWipe(PartitionOperationType type) {
    return type == PartitionOperationType::HfsDeleteFile ||
           type == PartitionOperationType::HfsDeleteFolderTree;
}

std::optional<uint64_t> parsedPositiveInteger(const QString& text) {
    bool ok = false;
    const uint64_t value = text.trimmed().toULongLong(&ok);
    return ok && value > 0 ? std::optional<uint64_t>(value) : std::nullopt;
}

QString hfsMutationPreviewTemplate(PartitionOperationType type) {
    static const QHash<int, QString> kTemplates{
        {static_cast<int>(PartitionOperationType::HfsOverwriteFile),
         QObject::tr("Queue HFS+ same-size data-fork overwrite for %1.")},
        {static_cast<int>(PartitionOperationType::HfsReplaceFile),
         QObject::tr("Queue HFS+ allocated-block data-fork replacement for %1.")},
        {static_cast<int>(PartitionOperationType::HfsGrowFile),
         QObject::tr("Queue HFS+ data-fork replacement with bounded allocation growth for %1.")},
        {static_cast<int>(PartitionOperationType::HfsTruncateFile),
         QObject::tr("Queue HFS+ data-fork truncate for %1.")},
        {static_cast<int>(PartitionOperationType::HfsReplaceResourceFork),
         QObject::tr("Queue HFS+ allocated-block resource-fork replacement for %1.")},
        {static_cast<int>(PartitionOperationType::HfsGrowResourceFork),
         QObject::tr(
             "Queue HFS+ resource-fork replacement with bounded allocation growth for %1.")},
        {static_cast<int>(PartitionOperationType::HfsTruncateResourceFork),
         QObject::tr("Queue HFS+ resource-fork truncate for %1.")},
        {static_cast<int>(PartitionOperationType::HfsCreateEmptyFile),
         QObject::tr("Queue HFS+ empty-file create for %1.")},
        {static_cast<int>(PartitionOperationType::HfsCreateFile),
         QObject::tr("Queue HFS+ file create with bounded data-fork allocation for %1.")},
        {static_cast<int>(PartitionOperationType::HfsDeleteEmptyFile),
         QObject::tr("Queue HFS+ empty-file delete for %1.")},
        {static_cast<int>(PartitionOperationType::HfsDeleteFile),
         QObject::tr("Queue HFS+ allocated-file delete for %1.")},
        {static_cast<int>(PartitionOperationType::HfsCreateEmptyFolder),
         QObject::tr("Queue HFS+ empty-folder create for %1.")},
        {static_cast<int>(PartitionOperationType::HfsDeleteEmptyFolder),
         QObject::tr("Queue HFS+ empty-folder delete for %1.")},
        {static_cast<int>(PartitionOperationType::HfsDeleteFolderTree),
         QObject::tr("Queue HFS+ folder-tree delete with block release for %1.")},
        {static_cast<int>(PartitionOperationType::HfsRenameMoveCatalogEntry),
         QObject::tr("Queue HFS+ catalog rename/move for %1.")},
        {static_cast<int>(PartitionOperationType::HfsReplaceInlineAttribute),
         QObject::tr("Queue HFS+ inline attribute replacement.")},
        {static_cast<int>(PartitionOperationType::HfsReplaceForkAttribute),
         QObject::tr("Queue HFS+ fork-backed attribute replacement within allocated blocks.")},
        {static_cast<int>(PartitionOperationType::HfsGrowForkAttribute),
         QObject::tr(
             "Queue HFS+ fork-backed attribute replacement with bounded allocation growth.")}};
    return kTemplates.value(static_cast<int>(type));
}

QString hfsMutationPreview(PartitionOperationType type, const QString& hfsPath) {
    const QString previewTemplate = hfsMutationPreviewTemplate(type);
    return previewTemplate.contains(QStringLiteral("%1")) ? previewTemplate.arg(hfsPath)
                                                          : previewTemplate;
}

void setHfsMutationFieldActive(QWidget* field, bool active) {
    field->setEnabled(active);
    field->setVisible(active);
}

bool hfsMutationPathReady(const HfsFileMutationDialogWidgets& widgets, bool needsPath) {
    if (!needsPath) {
        return true;
    }
    return !widgets.hfs_path->text().trimmed().isEmpty();
}

bool hfsMutationPayloadReady(const HfsFileMutationDialogWidgets& widgets, bool needsPayload) {
    if (!needsPayload) {
        return true;
    }
    return !widgets.payload->toPlainText().isEmpty();
}

bool hfsMutationAttributeReady(const HfsFileMutationDialogWidgets& widgets, bool attributeMode) {
    if (!attributeMode) {
        return true;
    }
    return parsedPositiveInteger(widgets.file_id->text()).has_value() &&
           !widgets.attribute_name->text().trimmed().isEmpty();
}

bool hfsMutationDialogReady(const HfsFileMutationDialogWidgets& widgets,
                            bool needsPath,
                            bool needsPayload,
                            bool needsDestinationPath,
                            bool attributeMode) {
    if (!widgets.confirm->isChecked()) {
        return false;
    }
    if (!hfsMutationPathReady(widgets, needsPath)) {
        return false;
    }
    if (needsDestinationPath && widgets.destination_hfs_path->text().trimmed().isEmpty()) {
        return false;
    }
    if (!hfsMutationPayloadReady(widgets, needsPayload)) {
        return false;
    }
    return hfsMutationAttributeReady(widgets, attributeMode);
}

QString hfsMutationPreviewPath(const HfsFileMutationDialogWidgets& widgets) {
    const QString path = widgets.hfs_path->text().trimmed();
    if (!path.isEmpty()) {
        return path;
    }
    return QObject::tr("(HFS path)");
}

QString hfsMutationDialogPreviewText(const HfsFileMutationDialogWidgets& widgets,
                                     PartitionOperationType type,
                                     bool secureWipeMode) {
    QString preview = hfsMutationPreview(type, hfsMutationPreviewPath(widgets));
    if (secureWipeMode && widgets.secure_wipe->isChecked()) {
        preview += QObject::tr(" Released blocks will be zeroed before release.");
    }
    return preview;
}

void syncHfsFileMutationDialog(const HfsFileMutationDialogWidgets& widgets) {
    const auto type = hfsMutationTypeForMode(widgets.mode->currentData().toString());
    const bool needsPayload = hfsMutationNeedsPayload(type);
    const bool needsPath = hfsMutationNeedsPath(type);
    const bool needsDestinationPath = hfsMutationNeedsDestinationPath(type);
    const bool attributeMode = hfsMutationIsAttribute(type);
    const bool secureWipeMode = hfsMutationCanSecureWipe(type);

    setHfsMutationFieldActive(widgets.hfs_path, needsPath);
    setHfsMutationFieldActive(widgets.destination_hfs_path, needsDestinationPath);
    setHfsMutationFieldActive(widgets.payload, needsPayload);
    setHfsMutationFieldActive(widgets.file_id, attributeMode);
    setHfsMutationFieldActive(widgets.attribute_name, attributeMode);
    setHfsMutationFieldActive(widgets.secure_wipe, secureWipeMode);

    widgets.dialog->setAcceptEnabled(hfsMutationDialogReady(
        widgets, needsPath, needsPayload, needsDestinationPath, attributeMode));
    widgets.dialog->setPreviewText(hfsMutationDialogPreviewText(widgets, type, secureWipeMode));
}

void populateHfsFileMutationModes(QComboBox* mode) {
    mode->setAccessibleName(QObject::tr("HFS file mutation mode"));
    mode->addItem(QObject::tr("Replace data fork within allocated blocks"),
                  QString::fromLatin1(kHfsReplaceFileMode));
    mode->addItem(QObject::tr("Grow data fork with free blocks"),
                  QString::fromLatin1(kHfsGrowFileMode));
    mode->addItem(QObject::tr("Overwrite data fork same size"),
                  QString::fromLatin1(kHfsOverwriteFileMode));
    mode->addItem(QObject::tr("Truncate data fork"), QString::fromLatin1(kHfsTruncateFileMode));
    mode->addItem(QObject::tr("Replace resource fork within allocated blocks"),
                  QString::fromLatin1(kHfsReplaceResourceForkMode));
    mode->addItem(QObject::tr("Grow resource fork with free blocks"),
                  QString::fromLatin1(kHfsGrowResourceForkMode));
    mode->addItem(QObject::tr("Truncate resource fork"),
                  QString::fromLatin1(kHfsTruncateResourceForkMode));
    mode->addItem(QObject::tr("Create empty file"), QString::fromLatin1(kHfsCreateEmptyFileMode));
    mode->addItem(QObject::tr("Create file with data"), QString::fromLatin1(kHfsCreateFileMode));
    mode->addItem(QObject::tr("Delete empty file"), QString::fromLatin1(kHfsDeleteEmptyFileMode));
    mode->addItem(QObject::tr("Delete file"), QString::fromLatin1(kHfsDeleteFileMode));
    mode->addItem(QObject::tr("Create empty folder"),
                  QString::fromLatin1(kHfsCreateEmptyFolderMode));
    mode->addItem(QObject::tr("Delete empty folder"),
                  QString::fromLatin1(kHfsDeleteEmptyFolderMode));
    mode->addItem(QObject::tr("Delete folder tree"), QString::fromLatin1(kHfsDeleteFolderTreeMode));
    mode->addItem(QObject::tr("Rename or move catalog entry"),
                  QString::fromLatin1(kHfsRenameMoveCatalogEntryMode));
    mode->addItem(QObject::tr("Replace inline attribute"),
                  QString::fromLatin1(kHfsReplaceInlineAttributeMode));
    mode->addItem(QObject::tr("Replace fork-backed attribute"),
                  QString::fromLatin1(kHfsReplaceForkAttributeMode));
    mode->addItem(QObject::tr("Grow fork-backed attribute with free blocks"),
                  QString::fromLatin1(kHfsGrowForkAttributeMode));
}

HfsFileMutationDialogWidgets createHfsFileMutationWidgets(PartitionOperationDialog& dialog,
                                                          const HfsFileMutationState& state) {
    auto* mode = new QComboBox(&dialog);
    populateHfsFileMutationModes(mode);

    auto* hfsPath = new QLineEdit(QStringLiteral("/hello.txt"), &dialog);
    hfsPath->setAccessibleName(QObject::tr("HFS file path"));
    auto* destinationHfsPath = new QLineEdit(QStringLiteral("/renamed.txt"), &dialog);
    destinationHfsPath->setAccessibleName(QObject::tr("HFS destination path"));
    auto* payload = new QTextEdit(&dialog);
    payload->setAcceptRichText(false);
    payload->setAccessibleName(QObject::tr("HFS mutation payload text"));
    payload->setMinimumHeight(kApfsRootFilePayloadMinHeight);
    auto* fileId = new QLineEdit(&dialog);
    fileId->setAccessibleName(QObject::tr("HFS attribute file ID"));
    auto* attributeName = new QLineEdit(&dialog);
    attributeName->setAccessibleName(QObject::tr("HFS attribute name"));
    auto* allowJournaled = new QCheckBox(QObject::tr("Allow journaled HFS+ staging"), &dialog);
    allowJournaled->setAccessibleName(QObject::tr("Allow journaled HFS+ staging"));
    allowJournaled->setChecked(state.journaled);
    auto* allowWrapped = new QCheckBox(QObject::tr("Allow classic HFS wrapper"), &dialog);
    allowWrapped->setAccessibleName(QObject::tr("Allow classic HFS wrapper"));
    allowWrapped->setChecked(state.wrapped);
    auto* secureWipe = new QCheckBox(QObject::tr("Zero released file blocks before delete"),
                                     &dialog);
    secureWipe->setAccessibleName(QObject::tr("Zero released HFS blocks before delete"));
    auto* confirm = new QCheckBox(
        QObject::tr("I understand this stages the selected raw HFS+ partition, mutates the staged "
                    "image, then writes changed HFS ranges back on Apply."),
        &dialog);
    confirm->setAccessibleName(QObject::tr("Confirm HFS staged file mutation"));

    dialog.formLayout()->addRow(QObject::tr("Mode:"), mode);
    dialog.formLayout()->addRow(QObject::tr("HFS path:"), hfsPath);
    dialog.formLayout()->addRow(QObject::tr("Destination:"), destinationHfsPath);
    dialog.formLayout()->addRow(QObject::tr("Payload:"), payload);
    dialog.formLayout()->addRow(QObject::tr("File ID:"), fileId);
    dialog.formLayout()->addRow(QObject::tr("Attribute:"), attributeName);
    dialog.formLayout()->addRow(QString(), allowJournaled);
    dialog.formLayout()->addRow(QString(), allowWrapped);
    dialog.formLayout()->addRow(QString(), secureWipe);
    dialog.formLayout()->addRow(QString(), confirm);

    return HfsFileMutationDialogWidgets{&dialog,
                                        mode,
                                        hfsPath,
                                        destinationHfsPath,
                                        payload,
                                        fileId,
                                        attributeName,
                                        allowJournaled,
                                        allowWrapped,
                                        secureWipe,
                                        confirm};
}

void connectHfsFileMutationDialog(PartitionOperationDialog& dialog,
                                  const HfsFileMutationDialogWidgets& widgets) {
    auto updatePreview = [widgets]() {
        syncHfsFileMutationDialog(widgets);
    };
    QObject::connect(widgets.mode, &QComboBox::currentTextChanged, &dialog, updatePreview);
    QObject::connect(widgets.hfs_path, &QLineEdit::textChanged, &dialog, updatePreview);
    QObject::connect(widgets.destination_hfs_path, &QLineEdit::textChanged, &dialog, updatePreview);
    QObject::connect(widgets.payload, &QTextEdit::textChanged, &dialog, updatePreview);
    QObject::connect(widgets.file_id, &QLineEdit::textChanged, &dialog, updatePreview);
    QObject::connect(widgets.attribute_name, &QLineEdit::textChanged, &dialog, updatePreview);
    QObject::connect(widgets.secure_wipe, &QCheckBox::toggled, &dialog, updatePreview);
    QObject::connect(widgets.confirm, &QCheckBox::toggled, &dialog, updatePreview);
    syncHfsFileMutationDialog(widgets);
}

std::optional<HfsFileMutationRequest> showHfsFileMutationDialog(QWidget* parent,
                                                                const HfsFileMutationState& state) {
    PartitionOperationDialog dialog(QObject::tr("HFS File"),
                                    state.target_path,
                                    QObject::tr("Queue a staged HFS+ file mutation."),
                                    parent);
    const HfsFileMutationDialogWidgets widgets = createHfsFileMutationWidgets(dialog, state);
    connectHfsFileMutationDialog(dialog, widgets);

    if (dialog.exec() != QDialog::Accepted) {
        return std::nullopt;
    }

    const auto type = hfsMutationTypeForMode(widgets.mode->currentData().toString());
    return HfsFileMutationRequest{type,
                                  widgets.hfs_path->text().trimmed(),
                                  widgets.destination_hfs_path->text().trimmed(),
                                  widgets.payload->toPlainText(),
                                  parsedPositiveInteger(widgets.file_id->text()).value_or(0),
                                  widgets.attribute_name->text().trimmed(),
                                  widgets.allow_journaled->isChecked(),
                                  widgets.allow_wrapped->isChecked(),
                                  widgets.secure_wipe->isChecked()};
}

QJsonObject hfsFileMutationPayload(const HfsFileMutationState& state,
                                   const HfsFileMutationRequest& request) {
    QJsonObject payload{{QStringLiteral("non_native_file_system_tool"), true},
                        {QStringLiteral("file_system"), state.file_system},
                        {QStringLiteral("target_path"), state.target_path},
                        {QStringLiteral("target_wipe_confirmed"), true},
                        {QStringLiteral("hfs_allow_journaled_volume"), request.allow_journaled},
                        {QStringLiteral("hfs_allow_wrapped_volume"), request.allow_wrapped}};
    if (hfsMutationNeedsPath(request.type)) {
        payload[QStringLiteral("hfs_path")] = request.hfs_path;
    }
    if (hfsMutationNeedsPayload(request.type)) {
        payload[QStringLiteral("hfs_payload_text")] = request.payload_text;
    }
    if (hfsMutationNeedsDestinationPath(request.type)) {
        payload[QStringLiteral("hfs_destination_path")] = request.destination_hfs_path;
    }
    if (hfsMutationIsAttribute(request.type)) {
        payload[QStringLiteral("hfs_file_id")] = QString::number(request.file_id);
        payload[QStringLiteral("hfs_attribute_name")] = request.attribute_name;
    }
    if (hfsMutationCanSecureWipe(request.type) && request.secure_wipe) {
        payload[QStringLiteral("hfs_secure_wipe_released_blocks")] = true;
    }
    return payload;
}

void PartitionManagerPanel::onCheckNonNativeFileSystem() {
    const auto* partition = selectedPartition();
    const auto state = nonNativeFilesystemCheckState(selectedTarget(), partition);
    if (!state.enabled) {
        showWarningLogged(this, tr("Check Non-Windows File System"), state.reason);
        return;
    }
    const auto immediateStatus = showImmediateMetadataCheck(this, state);
    if (immediateStatus.has_value()) {
        Q_EMIT statusMessage(*immediateStatus, sak::kTimerStatusDefaultMs);
        return;
    }

    const auto request =
        showNonNativeCheckRequestDialog(this, state, nonNativeFilesystemWriteTargetPath(partition));
    if (!request.has_value()) {
        Q_EMIT statusMessage(tr("Filesystem check cancelled"), sak::kTimerStatusDefaultMs);
        return;
    }

    const auto action = resolveNonNativeCheckAction(this, state, *request);
    if (action.kind == NonNativeCheckActionKind::QueueRepair) {
        queueOperation(PartitionOperationType::CheckFileSystem, action.payload);
        return;
    }
    if (action.kind == NonNativeCheckActionKind::StatusOnly) {
        Q_EMIT statusMessage(action.status, sak::kTimerStatusDefaultMs);
        return;
    }
    m_controller->runReadOnlyFileSystemCheck(state.file_system, action.target_path);
}

void PartitionManagerPanel::onApfsRootFileMutation() {
    const auto* partition = selectedPartition();
    const auto state = apfsRootFileMutationState(selectedTarget(), partition);
    if (!state.enabled) {
        showWarningLogged(this, tr("APFS File"), state.reason);
        return;
    }

    const auto request = showApfsRootFileMutationDialog(this, state);
    if (!request.has_value()) {
        Q_EMIT statusMessage(tr("APFS generated file mutation cancelled"),
                             sak::kTimerStatusDefaultMs);
        return;
    }

    queueOperation(request->type, apfsRootFileMutationPayload(state, *request));
}

void PartitionManagerPanel::onHfsFileMutation() {
    const auto* partition = selectedPartition();
    const auto state = hfsFileMutationState(selectedTarget(), partition);
    if (!state.enabled) {
        showWarningLogged(this, tr("HFS File"), state.reason);
        return;
    }

    const auto request = showHfsFileMutationDialog(this, state);
    if (!request.has_value()) {
        Q_EMIT statusMessage(tr("HFS file mutation cancelled"), sak::kTimerStatusDefaultMs);
        return;
    }

    queueOperation(request->type, hfsFileMutationPayload(state, *request));
}

void PartitionManagerPanel::onSurfaceTest() {
    queueOperation(PartitionOperationType::SurfaceTest);
}

void PartitionManagerPanel::onSpaceAnalyzer() {
    const auto* partition = selectedPartition();
    if (!partition || !partition->volume || partition->volume->drive_letter.isEmpty()) {
        showWarningLogged(this,
                          tr("Space Analyzer"),
                          tr("Select a mounted partition with a drive letter first."));
        return;
    }

    const QString driveLetter = partition->volume->drive_letter.left(1).toUpper();
    const QString rootPath = QStringLiteral("%1:/").arg(driveLetter);
    showSpaceAnalyzerDialog(this, rootPath, tr("Space Analyzer - %1:").arg(driveLetter));
    Q_EMIT statusMessage(tr("Space Analyzer closed"), sak::kTimerStatusDefaultMs);
}

void PartitionManagerPanel::onOpenDiskBenchmark() {
    Q_EMIT openBenchmarkRequested();
}

void PartitionManagerPanel::onOpenBootableMedia() {
    Q_EMIT openImageFlasherRequested();
}

void PartitionManagerPanel::onSetPartitionHidden() {
    const auto target = selectedTarget();
    if (!target || target->kind != PartitionTargetKind::Partition) {
        showWarningLogged(this,
                          tr("Hide/Unhide Partition"),
                          tr("Select a partition before changing hidden state."));
        return;
    }
    QStringList choices{tr("Hide"), tr("Unhide")};
    bool ok = false;
    const QString choice = QInputDialog::getItem(
        this, tr("Hide/Unhide Partition"), tr("Action:"), choices, 0, false, &ok);
    if (ok) {
        queueOperation(PartitionOperationType::SetPartitionHidden,
                       withValue(QStringLiteral("hidden"), choice == choices.first()));
    }
}

void PartitionManagerPanel::onSetPartitionActive() {
    const auto target = selectedTarget();
    if (!target || target->kind != PartitionTargetKind::Partition) {
        showWarningLogged(this,
                          tr("Set Active/Inactive"),
                          tr("Select an MBR partition before changing active state."));
        return;
    }
    QStringList choices{tr("Active"), tr("Inactive")};
    bool ok = false;
    const QString choice = QInputDialog::getItem(
        this, tr("Set Active/Inactive"), tr("Action:"), choices, 0, false, &ok);
    if (ok) {
        queueOperation(PartitionOperationType::SetPartitionActive,
                       withValue(QStringLiteral("active"), choice == choices.first()));
    }
}

void PartitionManagerPanel::onSetPartitionTypeId() {
    const auto target = selectedTarget();
    if (!target || target->kind != PartitionTargetKind::Partition) {
        showWarningLogged(this,
                          tr("Change Partition Type ID"),
                          tr("Select a partition before changing its type ID."));
        return;
    }
    bool ok = false;
    const QString typeId = QInputDialog::getText(this,
                                                 tr("Change Partition Type ID"),
                                                 tr("New MBR type or GPT GUID:"),
                                                 QLineEdit::Normal,
                                                 QString(),
                                                 &ok);
    if (ok && !typeId.trimmed().isEmpty()) {
        queueOperation(PartitionOperationType::SetPartitionTypeId,
                       withValue(QStringLiteral("type_id"), typeId.trimmed()));
    }
}

void PartitionManagerPanel::onManageBitLocker() {
    const auto* partition = selectedPartition();
    const PartitionVolumeInfo* volume = partition && partition->volume ? &partition->volume.value()
                                                                       : nullptr;
    const QStringList commands = bitLockerCommandLines(volume);
    const bool canManage = volume && volume->bitlocker_enabled &&
                           !bitLockerMountPoint(volume).isEmpty();
    const bool locked = volume && volume->bitlocker_locked;
    const auto action = showBitLockerDialog(
        this, bitLockerRows(selectedDisk(), partition), commands, canManage, locked);
    if (action == BitLockerDialogAction::None) {
        Q_EMIT statusMessage(tr("Reviewed BitLocker status"), sak::kTimerStatusDefaultMs);
        return;
    }

    QJsonObject payload;
    payload[QStringLiteral("drive_letter")] = bitLockerMountPoint(volume).left(1);
    if (action == BitLockerDialogAction::Unlock) {
        queueBitLockerUnlock(payload);
        return;
    }
    queueOperation(bitLockerOperationType(action), payload);
}

void PartitionManagerPanel::queueBitLockerUnlock(QJsonObject payload) {
    bool ok = false;
    const QString recoveryPassword = QInputDialog::getText(this,
                                                           tr("Unlock BitLocker"),
                                                           tr("Recovery password:"),
                                                           QLineEdit::Password,
                                                           QString(),
                                                           &ok);
    if (!ok || recoveryPassword.trimmed().isEmpty()) {
        Q_EMIT statusMessage(tr("BitLocker unlock cancelled"), sak::kTimerStatusDefaultMs);
        return;
    }
    payload[QStringLiteral("recovery_password")] = recoveryPassword.trimmed();
    queueOperation(PartitionOperationType::BitLockerUnlock, payload);
}

void PartitionManagerPanel::onOpenOptimizeDrives() {
    const auto* disk = selectedDisk();
    const auto* partition = selectedPartition();
    const PartitionVolumeInfo* volume = partition && partition->volume ? &partition->volume.value()
                                                                       : nullptr;
    const QStringList commands = optimizeCommandLines(disk, volume);
    const bool canQueue = volume && !optimizeMountPoint(volume).isEmpty();
    const auto action = showOptimizeDrivesDialog(this,
                                                 {optimizeRows(disk, partition),
                                                  commands,
                                                  canQueue,
                                                  diskLooksSsd(disk),
                                                  diskLooksHdd(disk)});
    if (action == OptimizeDialogAction::None) {
        Q_EMIT statusMessage(tr("Reviewed disk optimization guidance"), sak::kTimerStatusDefaultMs);
        return;
    }

    QJsonObject payload;
    payload[QStringLiteral("drive_letter")] = optimizeMountPoint(volume).left(1);
    queueOperation(action == OptimizeDialogAction::Defrag ? PartitionOperationType::DefragVolume
                                                          : PartitionOperationType::OptimizeSsd,
                   payload);
}

void PartitionManagerPanel::onSsdSecureErase() {
    const auto selected = selectedTarget();
    const auto* disk = selectedDisk();
    const QStringList checklist = secureEraseChecklist(disk);
    const bool queueRequested =
        showSecureEraseDialog(this, secureEraseRows(disk), checklist, canQueueSsdSecureErase(disk));
    if (!queueRequested) {
        Q_EMIT statusMessage(tr("Reviewed SSD Secure Erase readiness"), sak::kTimerStatusDefaultMs);
        return;
    }
    if (!selected || selected->kind != PartitionTargetKind::Disk || !disk) {
        Q_EMIT statusMessage(tr("Select an SSD/NVMe disk first"), sak::kTimerStatusDefaultMs);
        return;
    }
    const QString expected = QStringLiteral("ERASE DISK %1").arg(disk->disk_number);
    bool ok = false;
    const QString typed = QInputDialog::getText(this,
                                                tr("SSD Secure Erase"),
                                                tr("Type %1 to queue:").arg(expected),
                                                QLineEdit::Normal,
                                                QString(),
                                                &ok)
                              .trimmed()
                              .toUpper();
    if (!ok || typed != expected) {
        Q_EMIT statusMessage(tr("SSD Secure Erase queue cancelled"), sak::kTimerStatusDefaultMs);
        return;
    }

    QJsonObject payload;
    payload[QStringLiteral("ssd_secure_erase")] = true;
    payload[QStringLiteral("target_wipe_confirmed")] = true;
    queueOperation(PartitionOperationType::WipeDisk, payload);
}

void PartitionManagerPanel::onInitializeDisk() {
    const auto target = selectedTarget();
    if (!target || target->kind != PartitionTargetKind::Disk) {
        showWarningLogged(this, tr("Initialize Disk"), tr("Select a disk before initializing."));
        return;
    }
    QStringList styles{QStringLiteral("GPT"), QStringLiteral("MBR")};
    bool ok = false;
    const QString partitionScheme = QInputDialog::getItem(
        this, tr("Initialize Disk"), tr("Partition style:"), styles, 0, false, &ok);
    if (ok) {
        queueOperation(PartitionOperationType::InitializeDisk,
                       withValue(QStringLiteral("target_style"), partitionScheme));
    }
}

void PartitionManagerPanel::onDeleteAllPartitions() {
    const auto target = selectedTarget();
    if (!target || target->kind != PartitionTargetKind::Disk) {
        showWarningLogged(this,
                          tr("Delete All Partitions"),
                          tr("Select a disk before deleting partitions."));
        return;
    }
    const auto result = showQuestionLogged(
        this,
        tr("Delete All Partitions"),
        tr("Queue deletion of every partition on Disk %1? This is destructive and still requires "
           "Apply before execution.")
            .arg(target->disk_number),
        QMessageBox::Yes | QMessageBox::No,
        QMessageBox::No);
    if (result == QMessageBox::Yes) {
        queueOperation(PartitionOperationType::DeleteAllPartitions);
    }
}

void PartitionManagerPanel::onResizePartition() {
    const auto target = selectedTarget();
    const auto* disk = selectedDisk();
    const auto* partition = selectedPartition();
    if (missingResizeSelection(target, disk, partition)) {
        showWarningLogged(this, tr("Resize Partition"), tr("Select a partition before resizing."));
        return;
    }
    const auto availability = partitionActionAvailability(partition, resizeFilesystemPolicy());
    if (!availability.enabled) {
        showWarningLogged(this, tr("Resize Partition"), availability.reason);
        return;
    }

    PartitionOperationDialog dialog(
        tr("Resize/Move Partition"),
        targetIdentityText(target, selectedDisk(), partition),
        tr("Windows online resize cannot move partition start. Queued only until Apply."),
        this);
    const auto widgets = addResizePartitionControls(
        dialog, *disk, *partition, adjacentFreeBytesAfter(disk, partition));
    connectResizePartitionControls(dialog, widgets);
    updateResizePartitionPreview(dialog, widgets);
    if (dialog.exec() != QDialog::Accepted) {
        return;
    }
    const QString unchangedStatus = unchangedResizeStatus(widgets, *partition);
    if (!unchangedStatus.isEmpty()) {
        Q_EMIT statusMessage(unchangedStatus, sak::kTimerStatusDefaultMs);
        return;
    }

    queueOperation(resizeOperationType(widgets), resizePartitionPayload(widgets, *partition));
}

bool PartitionManagerPanel::queueUnallocatedFreeSpace(const PartitionTarget& target,
                                                      const PartitionDiskInfo& disk) {
    const auto choices = freeSpaceAllocationChoices(partitionBeforeRegion(&disk, target),
                                                    partitionAfterRegion(&disk, target));
    if (choices.isEmpty()) {
        showWarningLogged(this,
                          tr("Allocate Free Space"),
                          tr("Selected free space is not adjacent to a partition."));
        return false;
    }

    PartitionOperationDialog dialog(
        tr("Allocate Free Space To"),
        targetIdentityText(target, &disk, nullptr),
        tr("Queues adjacent online resize when free space follows a partition, or offline "
           "backup/delete/recreate/restore when free space precedes a partition."),
        this);
    const auto widgets = addFreeSpaceAllocationControls(dialog, choices, target.size_bytes);
    connectBackupBrowse(dialog, widgets.backup, tr("Allocate Free Space Backup Directory"));
    auto updatePreview = [&]() {
        updateFreeSpaceAllocationPreview(dialog, widgets, disk);
    };
    connect(widgets.target_partition, &QComboBox::currentIndexChanged, &dialog, updatePreview);
    connect(widgets.amount_mb, &QSpinBox::valueChanged, &dialog, updatePreview);
    connect(widgets.amount_handle, &QSlider::valueChanged, &dialog, [widgets](int value) {
        widgets.amount_mb->setValue(value);
    });
    connect(widgets.backup.backup_directory, &QLineEdit::textChanged, &dialog, updatePreview);
    connect(widgets.backup.confirmation, &QCheckBox::toggled, &dialog, updatePreview);
    updatePreview();
    if (dialog.exec() != QDialog::Accepted) {
        return false;
    }

    const auto choice = selectedFreeSpaceChoice(widgets);
    const auto* partition = selectedFreeSpacePartition(widgets, disk);
    const uint64_t amountBytes = selectedFreeSpaceAllocationBytes(widgets);
    if (!partition || amountBytes == 0) {
        return false;
    }

    m_controller->queueOperation(
        choice.move_partition_start ? PartitionOperationType::MovePartition
                                    : PartitionOperationType::Resize,
        partitionTargetFromPartition(*partition),
        choice.move_partition_start
            ? freeSpaceMovePayload(widgets, *partition, amountBytes)
            : freeSpaceResizePayload(*partition, target.size_bytes, amountBytes));
    return true;
}

bool PartitionManagerPanel::queueAdjacentDonorFreeSpace(const PartitionTarget& target,
                                                        const PartitionDiskInfo& disk,
                                                        const PartitionInfoEx& partition) {
    const auto* donor = adjacentDonorPartitionAfter(&disk, &partition);
    if (!donor) {
        showWarningLogged(this,
                          tr("Allocate Free Space"),
                          tr("No adjacent donor partition follows the selected partition."));
        return false;
    }
    if (!donor->volume || donor->volume->drive_letter.isEmpty()) {
        showWarningLogged(this,
                          tr("Allocate Free Space"),
                          tr("Adjacent donor partition must be mounted with a drive letter."));
        return false;
    }
    const uint64_t maxAllocateBytes = allocatableFreeBytesFromDonor(*donor);
    if (maxAllocateBytes == 0) {
        showWarningLogged(
            this,
            tr("Allocate Free Space"),
            tr("Adjacent donor does not have enough free space after safety reserve."));
        return false;
    }

    PartitionOperationDialog dialog(
        tr("Allocate Free Space"),
        targetIdentityText(target, &disk, &partition),
        tr("Queues adjacent donor backup/delete, target extend, donor recreate, restore, and "
           "SHA-256 manifest verification."),
        this);
    const AllocateFreeSpaceWidgets widgets =
        addAllocateFreeSpaceControls(dialog, partition, *donor, maxAllocateBytes);
    connectAllocateFreeSpaceControls(dialog, widgets, partition, *donor);
    updateAllocateFreeSpacePreview(dialog, widgets, partition, *donor);
    if (dialog.exec() != QDialog::Accepted) {
        return false;
    }

    queueOperation(PartitionOperationType::AllocateFreeSpace,
                   allocateFreeSpacePayload(widgets, *donor));
    return true;
}

void PartitionManagerPanel::onAllocateFreeSpace() {
    const auto target = selectedTarget();
    const auto* disk = selectedDisk();
    if (target && target->kind == PartitionTargetKind::Unallocated && disk) {
        queueUnallocatedFreeSpace(*target, *disk);
        return;
    }

    const auto* partition = selectedPartition();
    if (!target || !disk || !partition) {
        showWarningLogged(this,
                          tr("Allocate Free Space"),
                          tr("Select a target partition before allocating donor space."));
        return;
    }
    queueAdjacentDonorFreeSpace(*target, *disk, *partition);
}

void PartitionManagerPanel::onConvertPrimaryLogical() {
    const auto target = selectedTarget();
    const auto* disk = selectedDisk();
    const auto* partition = selectedPartition();
    if (!target || !disk || !partition) {
        showWarningLogged(this,
                          tr("Convert Primary/Logical"),
                          tr("Select a mounted MBR data partition before converting."));
        return;
    }
    const bool currentlyLogical = partition->type_name.contains(QStringLiteral("Logical"),
                                                                Qt::CaseInsensitive);
    const QString targetLayout = currentlyLogical ? QStringLiteral("primary")
                                                  : QStringLiteral("logical");
    PartitionOperationDialog dialog(
        tr("Convert Primary/Logical"),
        targetIdentityText(target, disk, partition),
        tr("Backs up the selected single-volume MBR data disk, rebuilds as %1, restores files, "
           "compares SHA-256 manifests, and repair-scans.")
            .arg(targetLayout),
        this);
    const auto widgets = addBackupRestoreControls(
        dialog,
        tr("Primary logical backup directory"),
        tr("Browse primary logical backup directory"),
        tr("I understand this will clear and rebuild this single-volume MBR data disk."),
        tr("Confirm primary logical backup and restore"));
    auto updatePreview = [&]() {
        const QString backup = QDir::toNativeSeparators(widgets.backup_directory->text().trimmed());
        const bool canQueue = backupIsOffPartitionVolume(backup, *partition) &&
                              widgets.confirmation->isChecked();
        widgets.status->setText(
            tr("Target layout: %1. Backup must be outside the selected volume.").arg(targetLayout));
        dialog.setAcceptEnabled(canQueue);
        dialog.setPreviewText(
            tr("Back up %1:, clear disk %2, recreate as %3, restore files, verify hashes.")
                .arg(partition->volume ? partition->volume->drive_letter.left(1).toUpper()
                                       : QStringLiteral("?"))
                .arg(disk->disk_number)
                .arg(targetLayout));
    };
    connect(widgets.backup_directory, &QLineEdit::textChanged, &dialog, updatePreview);
    connect(widgets.confirmation, &QCheckBox::toggled, &dialog, updatePreview);
    connectBackupBrowse(dialog, widgets, tr("Primary Logical Backup Directory"));
    updatePreview();
    if (dialog.exec() != QDialog::Accepted) {
        return;
    }

    QJsonObject payload;
    payload[QStringLiteral("target_layout")] = targetLayout;
    payload[QStringLiteral("source_size_bytes")] = QString::number(partition->size_bytes);
    if (partition->volume) {
        payload[QStringLiteral("drive_letter")] = partition->volume->drive_letter.left(1).toUpper();
        payload[QStringLiteral("file_system")] = partition->volume->file_system;
        payload[QStringLiteral("label")] = partition->volume->label;
    }
    payload[QStringLiteral("backup_directory")] =
        QDir::toNativeSeparators(widgets.backup_directory->text().trimmed());
    payload[QStringLiteral("target_wipe_confirmed")] = widgets.confirmation->isChecked();
    queueOperation(PartitionOperationType::ConvertPrimaryLogical, payload);
}

void PartitionManagerPanel::onChangeVolumeSerialNumber() {
    const auto target = selectedTarget();
    const auto* partition = selectedPartition();
    if (!target || !partition || !partition->volume || partition->volume->drive_letter.isEmpty()) {
        showWarningLogged(this,
                          tr("Change Serial Number"),
                          tr("Select a mounted data partition before changing its serial number."));
        return;
    }
    PartitionOperationDialog dialog(
        tr("Change Serial Number"),
        targetIdentityText(target, selectedDisk(), partition),
        tr("Backs up the volume, reformats it to regenerate the file-system serial number, "
           "restores files, compares SHA-256 manifests, and repair-scans."),
        this);
    const auto widgets = addBackupRestoreControls(
        dialog,
        tr("Volume serial backup directory"),
        tr("Browse volume serial backup directory"),
        tr("I understand this will reformat the selected volume, then restore and verify files."),
        tr("Confirm volume serial backup and restore"));
    auto updatePreview = [&]() {
        const QString backup = QDir::toNativeSeparators(widgets.backup_directory->text().trimmed());
        const bool canQueue = backupIsOffPartitionVolume(backup, *partition) &&
                              widgets.confirmation->isChecked();
        widgets.status->setText(tr("Backup must be outside %1:.")
                                    .arg(partition->volume->drive_letter.left(1).toUpper()));
        dialog.setAcceptEnabled(canQueue);
        dialog.setPreviewText(tr("Back up %1:, reformat as %2, restore files, verify hashes.")
                                  .arg(partition->volume->drive_letter.left(1).toUpper(),
                                       partition->volume->file_system));
    };
    connect(widgets.backup_directory, &QLineEdit::textChanged, &dialog, updatePreview);
    connect(widgets.confirmation, &QCheckBox::toggled, &dialog, updatePreview);
    connectBackupBrowse(dialog, widgets, tr("Volume Serial Backup Directory"));
    updatePreview();
    if (dialog.exec() != QDialog::Accepted) {
        return;
    }

    QJsonObject payload;
    payload[QStringLiteral("drive_letter")] = partition->volume->drive_letter.left(1).toUpper();
    payload[QStringLiteral("file_system")] = partition->volume->file_system;
    payload[QStringLiteral("label")] = partition->volume->label;
    payload[QStringLiteral("backup_directory")] =
        QDir::toNativeSeparators(widgets.backup_directory->text().trimmed());
    payload[QStringLiteral("target_wipe_confirmed")] = widgets.confirmation->isChecked();
    queueOperation(PartitionOperationType::ChangeVolumeSerialNumber, payload);
}

void PartitionManagerPanel::onConvertDynamicDiskToBasic() {
    const auto* disk = selectedDisk();
    if (!disk) {
        showWarningLogged(this,
                          tr("Convert Dynamic Disk to Basic"),
                          tr("Select a dynamic data disk before converting."));
        return;
    }
    const PartitionInfoEx* sourcePartition =
        disk->partitions.size() == 1 ? &disk->partitions.first() : nullptr;
    if (!sourcePartition || !sourcePartition->volume ||
        sourcePartition->volume->drive_letter.isEmpty()) {
        showWarningLogged(this,
                          tr("Convert Dynamic Disk to Basic"),
                          tr("Only one mounted simple dynamic volume can be converted directly."));
        return;
    }
    PartitionOperationDialog dialog(
        tr("Convert Dynamic Disk to Basic"),
        targetIdentityText(
            PartitionTarget{
                PartitionTargetKind::Disk, disk->disk_number, 0, {}, {}, {}, 0, disk->size_bytes},
            disk,
            nullptr),
        tr("Backs up the dynamic volume, deletes it, converts the disk to basic, recreates a "
           "primary partition, restores files, compares SHA-256 manifests, and repair-scans."),
        this);
    const auto widgets = addBackupRestoreControls(
        dialog,
        tr("Dynamic to basic backup directory"),
        tr("Browse dynamic to basic backup directory"),
        tr("I understand this will delete the dynamic volume and rebuild it as basic."),
        tr("Confirm dynamic to basic backup and restore"));
    auto updatePreview = [&]() {
        const QString backup = QDir::toNativeSeparators(widgets.backup_directory->text().trimmed());
        const bool canQueue = backupIsOffPartitionVolume(backup, *sourcePartition) &&
                              widgets.confirmation->isChecked();
        widgets.status->setText(tr("Source volume: %1:. Backup must be outside that volume.")
                                    .arg(sourcePartition->volume->drive_letter.left(1).toUpper()));
        dialog.setAcceptEnabled(canQueue);
        dialog.setPreviewText(
            tr("Back up %1:, convert disk %2 to basic, restore files, verify hashes.")
                .arg(sourcePartition->volume->drive_letter.left(1).toUpper())
                .arg(disk->disk_number));
    };
    connect(widgets.backup_directory, &QLineEdit::textChanged, &dialog, updatePreview);
    connect(widgets.confirmation, &QCheckBox::toggled, &dialog, updatePreview);
    connectBackupBrowse(dialog, widgets, tr("Dynamic to Basic Backup Directory"));
    updatePreview();
    if (dialog.exec() != QDialog::Accepted) {
        return;
    }

    QJsonObject payload;
    payload[QStringLiteral("drive_letter")] =
        sourcePartition->volume->drive_letter.left(1).toUpper();
    payload[QStringLiteral("source_size_bytes")] = QString::number(sourcePartition->size_bytes);
    payload[QStringLiteral("file_system")] = sourcePartition->volume->file_system;
    payload[QStringLiteral("label")] = sourcePartition->volume->label;
    payload[QStringLiteral("backup_directory")] =
        QDir::toNativeSeparators(widgets.backup_directory->text().trimmed());
    payload[QStringLiteral("target_wipe_confirmed")] = widgets.confirmation->isChecked();
    PartitionTarget diskTarget;
    diskTarget.kind = PartitionTargetKind::Disk;
    diskTarget.disk_number = disk->disk_number;
    diskTarget.size_bytes = disk->size_bytes;
    m_controller->queueOperation(PartitionOperationType::ConvertDynamicDiskToBasic,
                                 diskTarget,
                                 payload);
}

void PartitionManagerPanel::onExtendPartitionWizard() {
    const auto* disk = selectedDisk();
    const auto* partition = selectedPartition();
    if (!disk || !partition) {
        showWarningLogged(this,
                          tr("Extend Partition Wizard"),
                          tr("Select a partition before extending it."));
        return;
    }
    if (adjacentFreeBytesAfter(disk, partition) == 0) {
        showWarningLogged(this,
                          tr("Extend Partition Wizard"),
                          tr("No adjacent free space follows the selected partition. Donor-space "
                             "extension remains blocked until the offline move engine is "
                             "certified for direct execution."));
        return;
    }
    onResizePartition();
}

void PartitionManagerPanel::onQuickPartition() {
    const auto* disk = selectedDisk();
    const QString blocker = quickPartitionBlocker(disk);
    if (!blocker.isEmpty()) {
        showWarningLogged(this, tr("Quick Partition"), blocker);
        return;
    }

    PartitionOperationDialog dialog(
        tr("Quick Partition"),
        tr("Disk %1 - %2").arg(disk->disk_number).arg(formatPartitionBytes(disk->size_bytes)),
        tr("Queues a full-disk layout. Review Pending Operations before Apply."),
        this);
    const auto widgets = addQuickPartitionControls(dialog, *disk);
    auto updatePreview = [&]() {
        updateQuickPartitionPreview(dialog, widgets, *disk);
    };
    auto connectTableEditors = [&]() {
        connectQuickPartitionTableEditors(widgets, dialog, updatePreview);
    };
    auto rebuildAndPreview = [&]() {
        rebuildQuickPartitionControlsAndPreview(widgets, *disk, connectTableEditors, updatePreview);
    };
    connectQuickPartitionPresetControls(dialog, widgets, *disk, connectTableEditors, updatePreview);
    connectQuickPartitionLiveControls(dialog, widgets, rebuildAndPreview, updatePreview);
    connectTableEditors();
    updatePreview();
    if (dialog.exec() != QDialog::Accepted) {
        return;
    }

    queueQuickPartitionOperations(*disk, quickPartitionOptions(widgets));
    Q_EMIT statusMessage(tr("Quick Partition queued"), sak::kTimerStatusDefaultMs);
}

void PartitionManagerPanel::queueQuickPartitionOperations(const PartitionDiskInfo& disk,
                                                          const QJsonObject& options) {
    PartitionTarget diskTarget;
    diskTarget.kind = PartitionTargetKind::Disk;
    diskTarget.disk_number = disk.disk_number;
    diskTarget.size_bytes = disk.size_bytes;
    if (diskNeedsInitializeForQuickPartition(disk)) {
        m_controller->queueOperation(PartitionOperationType::InitializeDisk,
                                     diskTarget,
                                     withValue(QStringLiteral("target_style"),
                                               options.value(QStringLiteral("partition_scheme"))));
    }
    if (!disk.partitions.isEmpty()) {
        m_controller->queueOperation(PartitionOperationType::DeleteAllPartitions, diskTarget);
    }
    const auto alignmentBytes = static_cast<uint64_t>(kMegabyteBytes);
    uint64_t offsetBytes = alignmentBytes;
    const uint64_t usableBytes = quickPartitionUsableBytes(disk);
    const QVector<uint64_t> partitionSizes = quickPartitionSizesFromOptions(options, usableBytes);
    if (options.value(QStringLiteral("partition_scheme")).toString() == QStringLiteral("MBR") &&
        partitionSizes.size() > kQuickPartitionMbrDataMaxCount) {
        showWarningLogged(this,
                          tr("Quick Partition"),
                          tr("MBR Quick Partition is limited to four data partitions. Windows may "
                             "represent the fourth as a logical partition in an extended "
                             "container."));
        return;
    }
    if (!quickPartitionSizesAreValid(partitionSizes, usableBytes)) {
        showWarningLogged(this,
                          tr("Quick Partition"),
                          tr("Quick Partition sizes must fit the disk and each partition must be "
                             "at least the minimum size."));
        return;
    }
    for (int i = 0; i < partitionSizes.size(); ++i) {
        const uint64_t sizeBytes = partitionSizes.at(i);
        m_controller->queueOperation(PartitionOperationType::Create,
                                     quickPartitionCreateTarget(disk, offsetBytes, sizeBytes),
                                     quickPartitionCreatePayload(options, sizeBytes, i));
        offsetBytes += sizeBytes;
    }
}

void PartitionManagerPanel::onMergePartitions() {
    bool ok = false;
    const int sourcePartition =
        QInputDialog::getInt(this,
                             tr("Merge Partitions"),
                             tr("Partition number to merge into selection:"),
                             1,
                             1,
                             kMaxPartitionNumberInput,
                             1,
                             &ok);
    if (ok) {
        QJsonObject payload;
        payload[QStringLiteral("source_partition_number")] = QString::number(sourcePartition);
        payload[QStringLiteral("target_folder")] =
            QStringLiteral("Merged-Partition-%1").arg(sourcePartition);
        queueOperation(PartitionOperationType::Merge, payload);
    }
}

void PartitionManagerPanel::onSplitPartition() {
    bool ok = false;
    const int sizeMb = QInputDialog::getInt(this,
                                            tr("Split Partition"),
                                            tr("First partition size in MB:"),
                                            kDefaultCreateSizeMb,
                                            1,
                                            kMaxSizeInputMb,
                                            1,
                                            &ok);
    if (ok) {
        queueOperation(PartitionOperationType::Split,
                       withValue(QStringLiteral("first_size_bytes"),
                                 QString::number(static_cast<uint64_t>(sizeMb) * kMegabyteBytes)));
    }
}

void PartitionManagerPanel::onConvertStyle() {
    QStringList styles{QStringLiteral("GPT"), QStringLiteral("MBR")};
    bool ok = false;
    const QString targetScheme = QInputDialog::getItem(
        this, tr("Convert MBR/GPT"), tr("Target partition type:"), styles, 0, false, &ok);
    if (ok) {
        QJsonObject payload = withValue(QStringLiteral("target_style"), targetScheme);
        if (const auto* disk = selectedDisk();
            disk && disk->is_system &&
            disk->partition_style.compare(QStringLiteral("MBR"), Qt::CaseInsensitive) == 0 &&
            targetScheme == QStringLiteral("GPT")) {
            payload[QStringLiteral("mode")] = QStringLiteral("mbr2gpt");
        }
        queueOperation(PartitionOperationType::ConvertPartitionStyle, payload);
    }
}

void PartitionManagerPanel::onConvertFileSystem() {
    const auto availability = partitionActionAvailability(selectedPartition(),
                                                          windowsNativePolicy());
    if (!availability.enabled) {
        showWarningLogged(this, tr("Convert File System"), availability.reason);
        return;
    }
    queueOperation(PartitionOperationType::ConvertFileSystem);
}

void PartitionManagerPanel::onChangeClusterSize() {
    const auto target = selectedTarget();
    const auto* partition = selectedPartition();
    const PartitionVolumeInfo* volume = partition && partition->volume ? &partition->volume.value()
                                                                       : nullptr;
    if (!target || !partition || !volume || volume->drive_letter.isEmpty()) {
        showWarningLogged(this,
                          tr("Change Cluster Size"),
                          tr("Select a mounted partition before changing cluster size."));
        return;
    }
    const auto availability = partitionActionAvailability(partition, windowsNativePolicy());
    if (!availability.enabled) {
        showWarningLogged(this, tr("Change Cluster Size"), availability.reason);
        return;
    }

    PartitionOperationDialog dialog(
        tr("Change Cluster Size"),
        targetIdentityText(target, selectedDisk(), partition),
        tr("This queues a destructive backup, reformat, restore, and hash verification. "
           "Use a backup directory on a different volume."),
        this);
    const ClusterSizeWidgets widgets = addClusterSizeControls(dialog, *volume);
    connectClusterSizeControls(dialog, widgets, *volume);
    updateClusterSizePreview(dialog, widgets, *volume);
    if (dialog.exec() != QDialog::Accepted) {
        return;
    }

    queueOperation(PartitionOperationType::ChangeClusterSize, clusterSizePayload(widgets, *volume));
}

void PartitionManagerPanel::onCloneDisk() {
    const auto selected = selectedTarget();
    if (!selected || selected->kind != PartitionTargetKind::Disk) {
        Q_EMIT statusMessage(tr("Select a source disk first"), sak::kTimerStatusDefaultMs);
        return;
    }
    const auto result = runDiskCopyWizard({this,
                                           tr("Copy Disk Wizard"),
                                           targetIdentityText(selected, selectedDisk(), nullptr),
                                           &m_controller->inventory(),
                                           selected->disk_number,
                                           false});
    if (!result) {
        return;
    }
    QJsonObject payload;
    payload[QStringLiteral("source_path")] = physicalDrivePath(selected->disk_number);
    payload[QStringLiteral("target_path")] = result->target_path;
    payload[QStringLiteral("layout_mode")] = result->layout_mode;
    payload[QStringLiteral("verify_mode")] = result->verify_mode;
    payload[QStringLiteral("source_size_bytes")] = QString::number(result->source_size_bytes);
    payload[QStringLiteral("target_size_bytes")] = QString::number(result->target_size_bytes);
    payload[QStringLiteral("align_1mb")] = result->align_1mb;
    payload[QStringLiteral("target_wipe_confirmed")] = result->target_wipe_confirmed;
    queueOperation(PartitionOperationType::CloneDisk, payload);
}

void PartitionManagerPanel::onCopyPartitionWizard() {
    const auto selected = selectedTarget();
    if (!selected || selected->kind != PartitionTargetKind::Partition) {
        Q_EMIT statusMessage(tr("Select a source partition first"), sak::kTimerStatusDefaultMs);
        return;
    }
    if (selected->drive_letter.isEmpty()) {
        showWarningLogged(this,
                          tr("Copy Partition Wizard"),
                          tr("Partition copy requires a mounted source volume in v1."));
        return;
    }
    const QString sourcePath =
        QStringLiteral("\\\\.\\%1:").arg(selected->drive_letter.left(1).toUpper());
    const auto result =
        runPartitionCopyWizard(this,
                               targetIdentityText(selected, selectedDisk(), selectedPartition()),
                               sourcePath,
                               m_controller->inventory(),
                               selected->size_bytes);
    if (!result) {
        return;
    }
    QJsonObject payload;
    payload[QStringLiteral("source_path")] = sourcePath;
    payload[QStringLiteral("target_path")] = result->target_path;
    payload[QStringLiteral("verify_mode")] = result->verify_mode;
    payload[QStringLiteral("source_size_bytes")] = QString::number(result->source_size_bytes);
    payload[QStringLiteral("target_size_bytes")] = QString::number(result->target_size_bytes);
    payload[QStringLiteral("target_wipe_confirmed")] = result->target_wipe_confirmed;
    if (result->target_region_selected) {
        payload[QStringLiteral("target_disk_number")] =
            static_cast<int>(result->target_disk_number);
        payload[QStringLiteral("target_offset_bytes")] =
            QString::number(result->target_offset_bytes);
    }
    queueOperation(PartitionOperationType::ClonePartition, payload);
}

void PartitionManagerPanel::onCreateImage() {
    const auto selected = selectedTarget();
    if (!selected || (selected->kind != PartitionTargetKind::Disk &&
                      selected->kind != PartitionTargetKind::Partition)) {
        Q_EMIT statusMessage(tr("Select a source disk or partition first"),
                             sak::kTimerStatusDefaultMs);
        return;
    }

    QString sourcePath;
    if (selected->kind == PartitionTargetKind::Disk) {
        sourcePath = QStringLiteral("\\\\.\\PhysicalDrive%1").arg(selected->disk_number);
    } else if (!selected->drive_letter.isEmpty()) {
        sourcePath = QStringLiteral("\\\\.\\%1:").arg(selected->drive_letter.left(1).toUpper());
    } else {
        showWarningLogged(this,
                          tr("Create Image"),
                          tr("Partition images require a mounted volume in v1."));
        return;
    }

    const QString path = QFileDialog::getSaveFileName(
        this, tr("Create Image"), QString(), tr("Disk images (*.img);;All files (*)"));
    if (!path.isEmpty()) {
        QJsonObject payload;
        payload[QStringLiteral("source_path")] = sourcePath;
        payload[QStringLiteral("target_path")] = path;
        payload[QStringLiteral("source_size_bytes")] = QString::number(selected->size_bytes);
        queueOperation(PartitionOperationType::CreateImage, payload);
    }
}

void PartitionManagerPanel::onRestoreImage() {
    const auto selected = selectedTarget();
    if (!selected || selected->kind != PartitionTargetKind::Disk) {
        Q_EMIT statusMessage(tr("Select a target disk first"), sak::kTimerStatusDefaultMs);
        return;
    }
    const QString path = QFileDialog::getOpenFileName(
        this, tr("Restore Image"), QString(), tr("Disk images (*.img);;All files (*)"));
    if (path.isEmpty()) {
        return;
    }

    const QFileInfo imageInfo(path);
    if (!imageInfo.exists() || !imageInfo.isFile() || imageInfo.size() <= 0) {
        showWarningLogged(this,
                          tr("Restore Image"),
                          tr("Select a readable, non-empty disk image file."));
        return;
    }
    const auto imageSize = static_cast<uint64_t>(imageInfo.size());
    if (selected->size_bytes == 0) {
        showWarningLogged(this,
                          tr("Restore Image"),
                          tr("Target disk size is unknown. Refresh inventory before restoring."));
        return;
    }
    if (imageSize > selected->size_bytes) {
        showWarningLogged(this,
                          tr("Restore Image"),
                          tr("Image size (%1) is larger than target Disk %2 (%3).")
                              .arg(formatPartitionBytes(imageSize))
                              .arg(selected->disk_number)
                              .arg(formatPartitionBytes(selected->size_bytes)));
        return;
    }

    const auto result = showQuestionLogged(
        this,
        tr("Restore Image"),
        tr("Queue restore of %1 to Disk %2? This overwrites the target disk and still requires "
           "Apply before execution.\n\nImage: %3\nTarget: %4")
            .arg(QDir::toNativeSeparators(path))
            .arg(selected->disk_number)
            .arg(formatPartitionBytes(imageSize))
            .arg(formatPartitionBytes(selected->size_bytes)),
        QMessageBox::Yes | QMessageBox::No,
        QMessageBox::No);
    if (result != QMessageBox::Yes) {
        return;
    }

    QJsonObject payload;
    payload[QStringLiteral("source_path")] = path;
    payload[QStringLiteral("target_path")] =
        QStringLiteral("\\\\.\\PhysicalDrive%1").arg(selected->disk_number);
    payload[QStringLiteral("source_size_bytes")] = QString::number(imageSize);
    payload[QStringLiteral("target_size_bytes")] = QString::number(selected->size_bytes);
    payload[QStringLiteral("target_wipe_confirmed")] = true;
    queueOperation(PartitionOperationType::RestoreImage, payload);
}

void PartitionManagerPanel::onDataRecovery() {
    const QString sourcePath = selectDataRecoverySourcePath(this, selectedTarget());
    if (sourcePath.isEmpty()) {
        return;
    }
    const QString destinationPath =
        QFileDialog::getExistingDirectory(this, tr("Data Recovery Restore Destination"));
    if (destinationPath.isEmpty()) {
        return;
    }

    FileRecoveryScanOptions scanOptions;
    scanOptions.image_path = sourcePath;
    const auto scan = FileRecoveryEngine::scanOfflineImage(scanOptions);
    if (scan.candidates.isEmpty()) {
        showInformationLogged(this,
                              tr("Data Recovery"),
                              tr("No recoverable PNG, JPEG, or PDF file signatures found."));
        return;
    }
    if (!showFileRecoveryReviewDialog(this, scan, sourcePath, destinationPath)) {
        return;
    }

    FileRecoveryRestoreOptions restoreOptions;
    restoreOptions.image_path = sourcePath;
    restoreOptions.destination_directory = destinationPath;
    restoreOptions.candidates = scan.candidates;
    const auto restore = FileRecoveryEngine::restoreCandidates(restoreOptions);
    const QString warningText = restore.warnings.isEmpty()
                                    ? QString()
                                    : tr("\nWarnings: %1").arg(restore.warnings.join("; "));
    showInformationLogged(this,
                          tr("Data Recovery"),
                          tr("Restored %1 file(s). Source image unchanged: %2%3")
                              .arg(restore.restored_paths.size())
                              .arg(restore.source_not_mutated ? tr("yes") : tr("no"), warningText));
    Q_EMIT statusMessage(tr("Data Recovery restored %1 file(s)").arg(restore.restored_paths.size()),
                         sak::kTimerStatusDefaultMs);
}

void PartitionManagerPanel::onPartitionRecoveryWizard() {
    const auto selected = selectedTarget();
    if (!selected || selected->kind != PartitionTargetKind::Disk) {
        Q_EMIT statusMessage(tr("Select a disk before partition recovery scan"),
                             sak::kTimerStatusDefaultMs);
        return;
    }
    const auto result = runRecoveryWizard(this,
                                          targetIdentityText(selected, selectedDisk(), nullptr));
    if (result) {
        queueOperation(PartitionOperationType::PartitionRecoveryScan,
                       withValue(QStringLiteral("scan_mode"), result->scan_mode));
        if (result->recovery_restore_requested) {
            QJsonObject payload;
            payload[QStringLiteral("offset_bytes")] =
                QString::number(result->recovery_offset_bytes);
            payload[QStringLiteral("size_bytes")] = QString::number(result->recovery_size_bytes);
            payload[QStringLiteral("type_id")] = result->recovery_type_id;
            payload[QStringLiteral("partition_style")] =
                selectedDisk() ? selectedDisk()->partition_style : QStringLiteral("GPT");
            payload[QStringLiteral("restore_acknowledged")] = result->recovery_restore_acknowledged;
            queueOperation(PartitionOperationType::RestoreRecoveredPartition, payload);
        }
    }
}

void PartitionManagerPanel::onMigrateOs() {
    const auto selected = selectedTarget();
    if (!selected || selected->kind != PartitionTargetKind::Disk) {
        Q_EMIT statusMessage(tr("Select a source system disk first"), sak::kTimerStatusDefaultMs);
        return;
    }
    const auto result = runDiskCopyWizard({this,
                                           tr("Migrate OS to SSD/HDD Wizard"),
                                           targetIdentityText(selected, selectedDisk(), nullptr),
                                           &m_controller->inventory(),
                                           selected->disk_number,
                                           true});
    if (!result) {
        return;
    }
    QJsonObject payload;
    payload[QStringLiteral("source_path")] = physicalDrivePath(selected->disk_number);
    payload[QStringLiteral("target_path")] = result->target_path;
    payload[QStringLiteral("layout_mode")] = result->layout_mode;
    payload[QStringLiteral("verify_mode")] = result->verify_mode;
    payload[QStringLiteral("source_size_bytes")] = QString::number(result->source_size_bytes);
    payload[QStringLiteral("target_size_bytes")] = QString::number(result->target_size_bytes);
    payload[QStringLiteral("align_1mb")] = result->align_1mb;
    payload[QStringLiteral("target_gpt")] = result->target_gpt;
    payload[QStringLiteral("target_wipe_confirmed")] = result->target_wipe_confirmed;
    queueOperation(PartitionOperationType::MigrateOs, payload);
}

void PartitionManagerPanel::onRepairBoot() {
    bool ok = false;
    const QStringList modes{tr("UEFI"), tr("BIOS")};
    const QString mode = QInputDialog::getItem(
        this, tr("Rebuild MBR / Boot Repair"), tr("Boot mode:"), modes, 0, false, &ok);
    if (!ok) {
        return;
    }
    QJsonObject payload;
    payload[QStringLiteral("windows_path")] = QStringLiteral("C:\\Windows");
    payload[QStringLiteral("esp_letter")] = QStringLiteral("S");
    payload[QStringLiteral("boot_mode")] = mode;
    queueOperation(PartitionOperationType::RepairBoot, payload);
}

void PartitionManagerPanel::onOptimizeSsd() {
    const auto* partition = selectedPartition();
    const PartitionVolumeInfo* volume = partition && partition->volume ? &partition->volume.value()
                                                                       : nullptr;
    const QString mountPoint = optimizeMountPoint(volume);
    if (mountPoint.isEmpty()) {
        showWarningLogged(this,
                          tr("Align All Partitions / SSD Optimize"),
                          tr("Select a mounted volume before queueing SSD ReTrim."));
        return;
    }
    queueOperation(PartitionOperationType::OptimizeSsd,
                   withValue(QStringLiteral("drive_letter"), mountPoint.left(1)));
}

void PartitionManagerPanel::onWipeSelected() {
    const auto target = selectedTarget();
    if (!target) {
        return;
    }
    const auto type = target->kind == PartitionTargetKind::Disk
                          ? PartitionOperationType::WipeDisk
                          : (target->drive_letter.isEmpty()
                                 ? PartitionOperationType::WipePartition
                                 : PartitionOperationType::WipeFreeSpace);
    queueOperation(type);
}

void PartitionManagerPanel::onExecutionFinished(const PartitionExecutionResult& result) {
    Q_EMIT logOutput(result.report_html);
    showInformationLogged(this, tr("Partition Manager"), result.message);
}

std::optional<PartitionTarget> PartitionManagerPanel::selectedTarget() const {
    const auto selected = m_table->selectedItems();
    if (selected.isEmpty()) {
        return std::nullopt;
    }
    const auto* rowItem = m_table->item(selected.first()->row(), ColPartition);
    if (!rowItem) {
        return std::nullopt;
    }
    const auto rowData = rowItem->data(Qt::UserRole).toMap();
    PartitionTarget target;
    const QString kind = rowData.value(QStringLiteral("kind")).toString();
    target.disk_number = static_cast<uint32_t>(rowData.value(QStringLiteral("disk")).toInt());
    target.partition_number =
        static_cast<uint32_t>(rowData.value(QStringLiteral("partition")).toInt());
    target.offset_bytes = rowData.value(QStringLiteral("offset")).toString().toULongLong();
    target.size_bytes = rowData.value(QStringLiteral("size")).toString().toULongLong();
    target.drive_letter = rowData.value(QStringLiteral("letter")).toString();
    target.kind = kind == rowKindName(TableRowKind::Disk) ? PartitionTargetKind::Disk
                  : kind == rowKindName(TableRowKind::Unallocated)
                      ? PartitionTargetKind::Unallocated
                      : PartitionTargetKind::Partition;
    return target;
}

const PartitionDiskInfo* PartitionManagerPanel::selectedDisk() const {
    const auto target = selectedTarget();
    if (!target) {
        return nullptr;
    }
    return PartitionSafetyValidator::findDisk(m_controller->inventory(), target->disk_number);
}

const PartitionInfoEx* PartitionManagerPanel::selectedPartition() const {
    const auto target = selectedTarget();
    const auto* disk = selectedDisk();
    if (!target || !disk || target->partition_number == 0) {
        return nullptr;
    }
    return PartitionSafetyValidator::findPartition(*disk, target->partition_number);
}

QString PartitionManagerPanel::flagsForPartition(const PartitionInfoEx& partition) {
    QStringList flags;
    if (partition.is_system) {
        flags << QStringLiteral("System");
    }
    if (partition.is_boot) {
        flags << QStringLiteral("Boot");
    }
    if (partition.is_efi) {
        flags << QStringLiteral("EFI");
    }
    if (partition.is_msr) {
        flags << QStringLiteral("MSR");
    }
    if (partition.is_recovery) {
        flags << QStringLiteral("Recovery");
    }
    if (partition.volume && partition.volume->bitlocker_enabled) {
        flags << QStringLiteral("BitLocker");
    }
    return flags.join(QStringLiteral(", "));
}

QString PartitionManagerPanel::rowKindName(TableRowKind kind) {
    switch (kind) {
    case TableRowKind::Disk:
        return QStringLiteral("disk");
    case TableRowKind::Partition:
        return QStringLiteral("partition");
    case TableRowKind::Unallocated:
        return QStringLiteral("unallocated");
    }
    return QStringLiteral("unknown");
}

int PartitionManagerPanel::usedPercent(const PartitionInfoEx& partition) {
    if (!partition.volume || partition.volume->total_bytes == 0) {
        return 0;
    }
    const auto used = usedBytes(partition);
    return static_cast<int>(std::min<uint64_t>(
        kPartitionProgressMax, (used * kPartitionProgressMax) / partition.volume->total_bytes));
}

uint64_t PartitionManagerPanel::usedBytes(const PartitionInfoEx& partition) {
    if (!partition.volume || partition.volume->total_bytes < partition.volume->free_bytes) {
        return 0;
    }
    return partition.volume->total_bytes - partition.volume->free_bytes;
}

QString PartitionManagerPanel::partitionLabel(const PartitionInfoEx& partition) {
    if (partition.volume && !partition.volume->drive_letter.isEmpty()) {
        return tr("%1:").arg(partition.volume->drive_letter);
    }
    if (partition.is_efi || partition.is_msr || partition.is_recovery) {
        return tr("*:");
    }
    return tr("Partition %1").arg(partition.partition_number);
}

QColor PartitionManagerPanel::partitionColor(const PartitionDiskInfo& disk,
                                             const PartitionInfoEx& partition) {
    return partitionColorForRole(partitionColorRole(disk, partition));
}

QColor PartitionManagerPanel::unallocatedColor() {
    return kPartitionColorUnallocated;
}

}  // namespace sak
