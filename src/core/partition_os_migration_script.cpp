// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file partition_os_migration_script.cpp
/// @brief Puts a Migrate OS plan's numbers in front of the scripts in src/core/partition_scripts.

#include "sak/partition_os_migration.h"
#include "sak/partition_raw_device_io.h"
#include "sak/partition_script_builder.h"

namespace sak {

// CMake turns src/core/partition_scripts into this table: see cmake/embed_text_files.cmake.
namespace embedded_partition_scripts {
extern const char kOsMigrationNativeCs[];
extern const char kOsMigrationPs1[];
extern const char kTargetBootFilesPs1[];
extern const char kAttachedBootRepairPs1[];
extern const char kDynamicDisksPs1[];
}  // namespace embedded_partition_scripts

namespace {

QString embedded(const char* text) {
    return QString::fromUtf8(text).replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
}

QString quoted(const QString& text) {
    return PartitionScriptBuilder::quotePowerShell(text);
}

QString nativeTypesScript() {
    return QStringLiteral(
               "if (-not ('SakMigration.Copy' -as [type])) {\n"
               "Add-Type -TypeDefinition @'\n") +
           embedded(embedded_partition_scripts::kOsMigrationNativeCs) +
           QStringLiteral(
               "'@\n"
               "}\n");
}

QString partitionLiteral(const OsMigrationPartition& entry) {
    return QStringLiteral(
               "  @{ SourceOffset = [int64]%1; SourceSize = [int64]%2; TargetOffset = [int64]%3; "
               "InitialSize = [int64]%4; FinalSize = [int64]%5; Kind = %6; Letter = %7; "
               "FileSystem = %8; Snapshot = $%9; SourceNumber = [uint32]0; VolumePath = $null; "
               "Device = $null }")
        .arg(QString::number(entry.source_offset),
             QString::number(entry.source_size),
             QString::number(entry.target_offset),
             QString::number(entry.initial_size),
             QString::number(entry.final_size),
             quoted(osMigrationKindName(entry.kind)),
             quoted(entry.drive_letter.left(1).toUpper()),
             quoted(entry.file_system.trimmed().toUpper()),
             entry.from_snapshot ? QStringLiteral("true") : QStringLiteral("false"));
}

QString pinLiteral(const QString& unique_id, const QString& serial, uint64_t size) {
    return QStringLiteral("@{ UniqueId = %1; Serial = %2; Size = [uint64]%3 }")
        .arg(quoted(unique_id.trimmed()), quoted(serial.trimmed()), QString::number(size));
}

QString migrationParametersScript(const PartitionOperation& operation,
                                  const OsMigrationPlan& plan,
                                  uint32_t target_disk) {
    QStringList partitions;
    for (const auto& entry : plan.partitions) {
        partitions.append(partitionLiteral(entry));
    }
    const auto text = [&operation](const char* key) {
        return operation.payload.value(QLatin1String(key)).toString();
    };
    return QStringLiteral(
               "$sakSourceDisk = [uint32]%1\n"
               "$sakTargetDisk = [uint32]%2\n"
               "$sakSourcePin = %3\n"
               "$sakTargetPin = %4\n"
               "$sakScheme = %5\n"
               "$sakRoute = %6\n"
               "$sakVerify = %7\n"
               "$sakAllowNoSnapshot = $%8\n"
               "$sakScratchOffset = [int64]%9\n")
               .arg(QString::number(operation.target.disk_number.value()),
                    QString::number(target_disk),
                    pinLiteral(operation.target.disk_unique_id,
                               operation.target.disk_serial_number,
                               operation.target.disk_size_bytes),
                    pinLiteral(text("target_disk_unique_id"),
                               text("target_disk_serial_number"),
                               text("target_disk_size_bytes").toULongLong()),
                    quoted(plan.partition_scheme),
                    quoted(plan.route == OsMigrationRoute::Image ? QStringLiteral("Image")
                                                                 : QStringLiteral("Blocks")),
                    quoted(text("verify_mode")),
                    operation.payload.value(QStringLiteral("allow_no_snapshot")).toBool()
                        ? QStringLiteral("true")
                        : QStringLiteral("false"),
                    QString::number(plan.scratch_offset)) +
           QStringLiteral("$sakScratchSize = [int64]%1\n$sakPartitions = @(\n%2\n)\n")
               .arg(QString::number(plan.scratch_size), partitions.join(QStringLiteral(",\n"))) +
           QStringLiteral(
               "$script:sakSnapshotSet = $null\n$script:sakMappedLetter = $null\n"
               "$script:sakMappedDevice = $null\n$script:sakHiveLoaded = $false\n"
               "$script:sakTargetOffline = $false\n$script:sakTargetTouched = $false\n");
}

}  // namespace

QString dynamicDiskFunctionsScript() {
    return embedded(embedded_partition_scripts::kDynamicDisksPs1);
}

QString targetBootFilesFunctionScript() {
    return PartitionScriptBuilder::freeBootLetterFunctionScript() +
           embedded(embedded_partition_scripts::kTargetBootFilesPs1);
}

QString buildAttachedBootRepairScriptText(uint32_t disk_number, const QString& boot_mode) {
    return QStringLiteral(
               "$sakRepairDisk = [uint32]%1\n$sakRepairMode = %2\n"
               "$script:sakHiveLoaded = $false\n")
               .arg(QString::number(disk_number), quoted(boot_mode)) +
           targetBootFilesFunctionScript() +
           embedded(embedded_partition_scripts::kAttachedBootRepairPs1);
}

QString buildOsMigrationScriptText(const PartitionOperation& operation,
                                   const OsMigrationPlan& plan,
                                   QString* blocker) {
    const auto target_disk = rawDevicePhysicalDriveNumber(
        operation.payload.value(QStringLiteral("target_path")).toString().trimmed());
    if (!target_disk) {
        *blocker = QStringLiteral("Restore Image and OS migration require a physical target disk");
        return {};
    }
    const QString target_id =
        operation.payload.value(QStringLiteral("target_disk_unique_id")).toString().trimmed();
    if (target_id.isEmpty() || operation.target.disk_unique_id.trimmed().isEmpty() ||
        operation.target.disk_size_bytes == 0 ||
        operation.payload.value(QStringLiteral("target_disk_size_bytes")).toString().isEmpty()) {
        *blocker = QStringLiteral("Raw target disk identity was not pinned");
        return {};
    }
    return migrationParametersScript(operation, plan, *target_disk) + nativeTypesScript() +
           targetBootFilesFunctionScript() + embedded(embedded_partition_scripts::kOsMigrationPs1);
}

}  // namespace sak
