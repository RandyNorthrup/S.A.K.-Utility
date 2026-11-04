// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file partition_manager_scenario_step.cpp
/// @brief One step of a certification row: what a person's Apply does, on one pinned disk.
///
/// A step scans the disks through the real helper, finds its disk by identity, builds the request
/// with the same core functions the panel calls, queues it, and applies it, as the panel does after
/// its dialogs close. It is copied into a folder as sak_utility.exe beside sak_elevated_helper.exe,
/// because the helper accepts only a client of that name in its own folder, and it runs under a
/// standard-user token where that is how the app runs, so the helper's own start is part of what a
/// row proves.
///
/// What a step reports is the app's word. Whether the disk really changed is read back afterwards
/// by scripts/partition_manager_readback.ps1, which shares no code with this.

#include "partition_manager_scenario_step.h"

#include "sak/elevation_broker.h"
#include "sak/elevation_manager.h"
#include "sak/file_hash.h"
#include "sak/file_recovery_engine.h"
#include "sak/partition_manager_controller.h"
#include "sak/partition_operation_requests.h"
#include "sak/partition_safety_validator.h"
#include "sak/partition_script_builder.h"

#include "partition_manager_code_digest.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QSaveFile>
#include <QScopeGuard>
#include <QTimer>

#include <algorithm>
#include <cstdio>
#include <expected>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <utility>

namespace certification {
namespace {

template <typename Value>
using Result = std::expected<Value, QString>;

constexpr int kExitPassed = 0;
constexpr int kExitFailed = 2;
constexpr int kDefaultTimeoutSeconds = 1800;
constexpr int kMillisecondsPerSecond = 1000;
constexpr int kCancelGraceMs = 60'000;
constexpr qsizetype kMaximumStepOutputCharacters = 256 * 1024;
constexpr qint64 kMaximumJobBytes = 256 * 1024;
constexpr uint64_t kMiB = 1024ULL * 1024ULL;
constexpr char kToolName[] = "partition-manager-scenario-step";
constexpr int kSchemaVersion = 1;

auto refused(const QString& error) {
    return std::unexpected(error);
}

QString jsonString(const QJsonObject& object, const char* key) {
    return object.value(QLatin1String(key)).toString();
}

uint64_t jsonUInt64(const QJsonObject& object, const char* key) {
    const QJsonValue value = object.value(QLatin1String(key));
    return value.isString() ? value.toString().toULongLong()
                            : static_cast<uint64_t>(value.toDouble());
}

uint64_t jsonMiB(const QJsonObject& object, const char* key) {
    return jsonUInt64(object, key) * kMiB;
}

/// A secret a job may carry for a fixture stays out of the report, which ends up in a repository.
QJsonObject withoutSecrets(QJsonObject payload) {
    for (const QString& key : payload.keys()) {
        if (key.contains(QStringLiteral("password"), Qt::CaseInsensitive)) {
            payload[key] = QStringLiteral("<withheld>");
        }
    }
    return payload;
}

QString boundedText(const QString& text) {
    return text.size() <= kMaximumStepOutputCharacters ? text
                                                       : text.last(kMaximumStepOutputCharacters);
}

QString sha256OfFile(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }
    QCryptographicHash hash(QCryptographicHash::Sha256);
    hash.addData(&file);
    return QString::fromLatin1(hash.result().toHex());
}

QString sha256OfText(const QString& text) {
    return QString::fromLatin1(
        QCryptographicHash::hash(text.toUtf8(), QCryptographicHash::Sha256).toHex());
}

struct DiskPin {
    QString unique_id;
    QString serial_number;
    QString model;
    uint64_t size_bytes{0};
};

struct StepJob {
    QString scenario_id;
    QString step;
    QString run_id;
    QString operation;
    QString report_path;
    QString confirmation;
    QString expect;
    QString refusal_contains;
    DiskPin pin;
    std::optional<DiskPin> second_pin;
    QJsonObject select;
    QJsonObject choices;
    int timeout_seconds{kDefaultTimeoutSeconds};
    bool allow_elevated_client{false};
};

Result<DiskPin> parsePin(const QJsonObject& object) {
    DiskPin pin{.unique_id = jsonString(object, "unique_id"),
                .serial_number = jsonString(object, "serial_number"),
                .model = jsonString(object, "model"),
                .size_bytes = jsonUInt64(object, "size_bytes")};
    if (pin.unique_id.isEmpty() || pin.model.isEmpty() || pin.size_bytes == 0) {
        return refused(QStringLiteral("A disk pin needs a unique id, a model and a size"));
    }
    return pin;
}

/// What a person has to type before a step may touch a disk. A pin that a script worked out from
/// the disk it found is not the same as someone having said which disk this is.
QString expectedConfirmation(const DiskPin& pin) {
    return QStringLiteral("CERTIFY %1 %2 %3")
        .arg(pin.model, pin.serial_number, QString::number(pin.size_bytes));
}

Result<void> checkJobFields(const StepJob& job) {
    if (job.scenario_id.isEmpty() || job.step.isEmpty() || job.run_id.isEmpty() ||
        job.operation.isEmpty() || job.report_path.isEmpty()) {
        return refused(QStringLiteral(
            "A step job needs a scenario, a step, a run id, an operation and a report path"));
    }
    const QStringList expectations{QStringLiteral("success"),
                                   QStringLiteral("refused"),
                                   QStringLiteral("failed")};
    if (!expectations.contains(job.expect)) {
        return refused(
            QStringLiteral("A step job must say what it expects: success, refused or "
                           "failed"));
    }
    if (job.expect != QStringLiteral("success") && job.refusal_contains.isEmpty()) {
        return refused(QStringLiteral("A step that expects a refusal must name the reason"));
    }
    return {};
}

Result<void> readJobPins(const QJsonObject& root, StepJob* job) {
    const auto pin = parsePin(root.value(QLatin1String("pin")).toObject());
    if (!pin) {
        return refused(pin.error());
    }
    job->pin = *pin;
    if (job->confirmation != expectedConfirmation(job->pin)) {
        return refused(QStringLiteral("The typed confirmation does not name the pinned disk"));
    }
    const QJsonObject second = root.value(QLatin1String("second_pin")).toObject();
    if (second.isEmpty()) {
        return {};
    }
    const auto second_pin = parsePin(second);
    if (!second_pin) {
        return refused(second_pin.error());
    }
    job->second_pin = *second_pin;
    return {};
}

Result<StepJob> parseJob(const QJsonObject& root) {
    StepJob job{.scenario_id = jsonString(root, "scenario_id"),
                .step = jsonString(root, "step"),
                .run_id = jsonString(root, "run_id"),
                .operation = jsonString(root, "operation"),
                .report_path = jsonString(root, "report_path"),
                .confirmation = jsonString(root, "confirmation"),
                .expect = jsonString(root, "expect"),
                .refusal_contains = jsonString(root, "refusal_contains"),
                .select = root.value(QLatin1String("select")).toObject(),
                .choices = root.value(QLatin1String("choices")).toObject(),
                .allow_elevated_client =
                    root.value(QLatin1String("allow_elevated_client")).toBool(false)};
    if (const auto fields = checkJobFields(job); !fields) {
        return refused(fields.error());
    }
    if (const auto pins = readJobPins(root, &job); !pins) {
        return refused(pins.error());
    }
    const int timeout = root.value(QLatin1String("timeout_seconds")).toInt(kDefaultTimeoutSeconds);
    job.timeout_seconds = timeout > 0 ? timeout : kDefaultTimeoutSeconds;
    return job;
}

Result<StepJob> readJob(const QString& job_path) {
    QFile file(job_path);
    if (!file.open(QIODevice::ReadOnly) || file.size() > kMaximumJobBytes) {
        return refused(QStringLiteral("Cannot read the step job: %1").arg(job_path));
    }
    QJsonParseError parse_error;
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &parse_error);
    if (parse_error.error != QJsonParseError::NoError || !document.isObject()) {
        return refused(QStringLiteral("The step job is not a JSON object"));
    }
    return parseJob(document.object());
}

bool pinMatches(const DiskPin& pin, const sak::PartitionDiskInfo& disk) {
    return disk.unique_id.trimmed() == pin.unique_id &&
           disk.serial_number.trimmed() == pin.serial_number.trimmed() &&
           disk.model.trimmed() == pin.model.trimmed() && disk.size_bytes == pin.size_bytes;
}

Result<const sak::PartitionDiskInfo*> findPinned(const sak::PartitionInventory& inventory,
                                                 const DiskPin& pin) {
    const sak::PartitionDiskInfo* found = nullptr;
    for (const auto& disk : inventory.disks) {
        if (pinMatches(pin, disk)) {
            if (found != nullptr) {
                return refused(QStringLiteral("Two disks match the pin"));
            }
            found = &disk;
        }
    }
    if (found == nullptr) {
        return refused(QStringLiteral("No disk matches the pin: %1 %2 bytes")
                           .arg(pin.model)
                           .arg(pin.size_bytes));
    }
    return found;
}

Result<const sak::PartitionInfoEx*> selectPartition(const sak::PartitionDiskInfo& disk,
                                                    const QJsonObject& select) {
    const QString letter = jsonString(select, "drive_letter").toUpper();
    const auto number =
        static_cast<uint32_t>(select.value(QLatin1String("partition_number")).toInt(0));
    const auto it = std::ranges::find_if(disk.partitions, [&](const sak::PartitionInfoEx& item) {
        const bool letter_matches = item.volume &&
                                    item.volume->drive_letter.left(1).toUpper() == letter;
        return (!letter.isEmpty() && letter_matches) ||
               (number != 0 && item.partition_number.value() == number);
    });
    if (it == disk.partitions.cend()) {
        return refused(QStringLiteral("The selected partition is not on the pinned disk"));
    }
    return &(*it);
}

// The free space a person would pick: the one that holds the given offset, or the largest.
Result<sak::UnallocatedRegion> selectRegion(const sak::PartitionDiskInfo& disk,
                                            const QJsonObject& select) {
    if (disk.unallocated_regions.isEmpty()) {
        return refused(QStringLiteral("The pinned disk has no free space"));
    }
    if (!select.contains(QLatin1String("region_offset_bytes"))) {
        return *std::ranges::max_element(disk.unallocated_regions, {}, [](const auto& region) {
            return region.size_bytes;
        });
    }
    const uint64_t wanted = jsonUInt64(select, "region_offset_bytes");
    const auto holding =
        std::ranges::find_if(disk.unallocated_regions, [wanted](const auto& region) {
            return wanted >= region.offset_bytes &&
                   wanted < region.offset_bytes + region.size_bytes;
        });
    if (holding == disk.unallocated_regions.cend()) {
        return refused(QStringLiteral("The pinned disk has no free space at that offset"));
    }
    return *holding;
}

using Requests = QVector<sak::PartitionOperationRequest>;

struct BuildContext {
    const StepJob& job;
    const sak::PartitionInventory& inventory;
    const sak::PartitionDiskInfo& disk;
};

sak::BackupRestoreChoices backupChoices(const QJsonObject& choices) {
    return {.backup_directory = QDir::toNativeSeparators(jsonString(choices, "backup_directory")),
            .confirmed = choices.value(QLatin1String("confirmed")).toBool(true)};
}

sak::FormatPartitionChoices formatChoices(const QJsonObject& choices) {
    return {.file_system = jsonString(choices, "file_system"),
            .allocation_unit_bytes = jsonUInt64(choices, "allocation_unit_bytes"),
            .label = jsonString(choices, "label"),
            .full_format = choices.value(QLatin1String("full_format")).toBool(false)};
}

Requests oneRequest(sak::PartitionOperationType type,
                    const sak::PartitionTarget& target,
                    const QJsonObject& payload) {
    return {{.type = type, .target = target, .payload = payload}};
}

Result<Requests> buildCreate(const BuildContext& context) {
    const auto region = selectRegion(context.disk, context.job.select);
    if (!region) {
        return refused(region.error());
    }
    const QJsonObject& choices = context.job.choices;
    const sak::PartitionTarget target = sak::unallocatedOperationTarget(context.disk.disk_number,
                                                                        region->offset_bytes,
                                                                        region->size_bytes);
    return oneRequest(sak::PartitionOperationType::Create,
                      target,
                      sak::createPartitionPayload(sak::CreatePartitionChoices{
                          .size_bytes = jsonMiB(choices, "size_mib"),
                          .relative_offset_bytes = jsonMiB(choices, "free_before_mib"),
                          .file_system = jsonString(choices, "file_system"),
                          .allocation_unit_bytes = jsonUInt64(choices, "allocation_unit_bytes"),
                          .partition_type = jsonString(choices, "partition_type"),
                          .label = jsonString(choices, "label"),
                          .full_format = choices.value(QLatin1String("full_format")).toBool(false),
                          .drive_letter = jsonString(choices, "drive_letter")}));
}

/// A request whose only input is the selected partition and a payload made from it.
Result<Requests> buildOnPartition(
    const BuildContext& context,
    sak::PartitionOperationType type,
    const std::function<QJsonObject(const sak::PartitionInfoEx&)>& payload) {
    const auto partition = selectPartition(context.disk, context.job.select);
    if (!partition) {
        return refused(partition.error());
    }
    return oneRequest(type, sak::partitionSelectionTarget(**partition), payload(**partition));
}

Result<Requests> buildResize(const BuildContext& context) {
    return buildOnPartition(
        context, sak::PartitionOperationType::Resize, [&](const sak::PartitionInfoEx& partition) {
            return sak::resizePartitionPayload(jsonMiB(context.job.choices, "target_size_mib"),
                                               sak::adjacentFreeBytesAfter(context.disk,
                                                                           partition));
        });
}

Result<Requests> buildMove(const BuildContext& context) {
    return buildOnPartition(
        context,
        sak::PartitionOperationType::MovePartition,
        [&](const sak::PartitionInfoEx& partition) {
            QJsonObject payload = sak::resizePartitionPayload(
                partition.size_bytes, sak::adjacentFreeBytesAfter(context.disk, partition));
            sak::addMovePartitionFields(&payload,
                                        partition,
                                        jsonMiB(context.job.choices, "target_offset_mib"),
                                        backupChoices(context.job.choices));
            return payload;
        });
}

Result<Requests> buildAllocate(const BuildContext& context) {
    const auto partition = selectPartition(context.disk, context.job.select);
    if (!partition) {
        return refused(partition.error());
    }
    const auto end = (*partition)->offset_bytes + (*partition)->size_bytes;
    const auto donor = std::ranges::find_if(context.disk.partitions, [end](const auto& candidate) {
        return candidate.offset_bytes == end;
    });
    if (donor == context.disk.partitions.cend()) {
        return refused(QStringLiteral("No partition follows the selected one to give it space"));
    }
    return oneRequest(sak::PartitionOperationType::AllocateFreeSpace,
                      sak::partitionSelectionTarget(**partition),
                      sak::allocateFreeSpacePayload(*donor,
                                                    jsonMiB(context.job.choices, "allocate_mib"),
                                                    backupChoices(context.job.choices)));
}

Result<Requests> buildDynamicToBasic(const BuildContext& context) {
    if (context.disk.partitions.size() != 1) {
        return refused(QStringLiteral("Only one mounted simple dynamic volume converts directly"));
    }
    return oneRequest(sak::PartitionOperationType::ConvertDynamicDiskToBasic,
                      sak::diskOperationTarget(context.disk),
                      sak::dynamicToBasicPayload(context.disk.partitions.constFirst(),
                                                 backupChoices(context.job.choices)));
}

/// The checks the panel makes before it queues a Quick Partition.
Result<Requests> buildQuickPartition(const BuildContext& context) {
    const QJsonObject& options = context.job.choices;
    const uint64_t usable_bytes = sak::quickPartitionUsableBytes(context.disk);
    const QVector<uint64_t> sizes = sak::quickPartitionSizesFromOptions(options, usable_bytes);
    if (jsonString(options, "partition_scheme") == QStringLiteral("MBR") &&
        sizes.size() > sak::kQuickPartitionMbrDataMaxCount) {
        return refused(QStringLiteral("MBR Quick Partition is limited to four data partitions"));
    }
    if (!sak::quickPartitionSizesAreValid(sizes, usable_bytes)) {
        return refused(QStringLiteral("Quick Partition sizes must fit the disk"));
    }
    const Requests requests = sak::quickPartitionRequests(context.disk, options, sizes);
    if (requests.isEmpty()) {
        return refused(QStringLiteral("Quick Partition request could not be represented safely"));
    }
    return requests;
}

Result<Requests> buildCloneDisk(const BuildContext& context) {
    if (!context.job.second_pin) {
        return refused(QStringLiteral("A disk copy needs a target pin"));
    }
    const auto target = findPinned(context.inventory, *context.job.second_pin);
    if (!target) {
        return refused(target.error());
    }
    const sak::CopyChoices choices{.source_path =
                                       sak::rawDiskPath(context.disk.disk_number.value()),
                                   .target_path = sak::rawDiskPath((*target)->disk_number.value()),
                                   .verify_mode = jsonString(context.job.choices, "verify_mode"),
                                   .source_size_bytes = context.disk.size_bytes,
                                   .target_size_bytes = (*target)->size_bytes,
                                   .target_wipe_confirmed = true};
    return oneRequest(sak::PartitionOperationType::CloneDisk,
                      sak::diskOperationTarget(context.disk),
                      sak::copyPayload(choices));
}

Result<Requests> buildMigrate(const BuildContext& context) {
    // The pin names the throwaway target. The source is the disk this Windows runs from.
    const auto source = std::ranges::find_if(context.inventory.disks,
                                             [](const auto& disk) { return disk.is_boot; });
    if (source == context.inventory.disks.cend()) {
        return refused(QStringLiteral("The inventory names no boot disk"));
    }
    const sak::CopyChoices choices{.source_path = sak::rawDiskPath(source->disk_number.value()),
                                   .target_path =
                                       sak::rawDiskPath(context.disk.disk_number.value()),
                                   .verify_mode = jsonString(context.job.choices, "verify_mode"),
                                   .source_size_bytes = source->size_bytes,
                                   .target_size_bytes = context.disk.size_bytes,
                                   .target_wipe_confirmed = true};
    return oneRequest(sak::PartitionOperationType::MigrateOs,
                      sak::diskOperationTarget(*source),
                      sak::migrateOsPayload(
                          *source,
                          choices,
                          context.job.choices.value(QLatin1String("allow_no_snapshot")).toBool()));
}

Result<Requests> buildClonePartition(const BuildContext& context) {
    const auto partition = selectPartition(context.disk, context.job.select);
    if (!partition || (*partition)->volume == std::nullopt) {
        return refused(QStringLiteral("A partition copy needs a mounted source volume"));
    }
    if (!context.job.second_pin) {
        return refused(QStringLiteral("A partition copy needs a target pin"));
    }
    const auto target = findPinned(context.inventory, *context.job.second_pin);
    if (!target) {
        return refused(target.error());
    }
    const auto region = selectRegion(**target, context.job.choices);
    if (!region) {
        return refused(region.error());
    }
    const sak::CopyChoices choices{
        .source_path =
            QStringLiteral("\\\\.\\%1:").arg((*partition)->volume->drive_letter.left(1).toUpper()),
        .target_path = sak::rawDiskPath((*target)->disk_number.value()),
        .verify_mode = jsonString(context.job.choices, "verify_mode"),
        .source_size_bytes = (*partition)->size_bytes,
        .target_size_bytes = region->size_bytes,
        .target_wipe_confirmed = true};
    return oneRequest(
        sak::PartitionOperationType::ClonePartition,
        sak::partitionSelectionTarget(**partition),
        sak::partitionCopyPayload(choices,
                                  sak::CopyRegion{.disk_number = (*target)->disk_number.value(),
                                                  .offset_bytes = region->offset_bytes}));
}

Result<Requests> buildCreateImage(const BuildContext& context) {
    if (!context.disk.is_offline.has_value()) {
        return refused(QStringLiteral("Source disk online/offline state is unknown"));
    }
    const bool offline = *context.disk.is_offline;
    sak::PartitionTarget target = sak::diskOperationTarget(context.disk);
    if (!context.job.select.isEmpty()) {
        const auto partition = selectPartition(context.disk, context.job.select);
        if (!partition) {
            return refused(partition.error());
        }
        target = sak::partitionSelectionTarget(**partition);
    }
    const QString source_path = sak::createImageSourcePath(target, context.disk, offline);
    const QString image = jsonString(context.job.choices, "image_path");
    if (source_path.isEmpty() || image.isEmpty()) {
        return refused(QStringLiteral("Create Image needs a stable source and an image path"));
    }
    return oneRequest(
        sak::PartitionOperationType::CreateImage,
        target,
        sak::createImagePayload(target, offline, source_path, image, QFileInfo::exists(image)));
}

Result<Requests> buildRestoreImage(const BuildContext& context) {
    const QString image = jsonString(context.job.choices, "image_path");
    const QFileInfo info(image);
    if (!info.isFile() || info.size() <= 0) {
        return refused(QStringLiteral("Select a readable, non-empty disk image file"));
    }
    // The panel pins the image's content before a person approves it, with this hasher.
    const sak::file_hasher hasher;
    const auto digest = hasher.calculateHash(std::filesystem::path(image.toStdWString()));
    if (!digest) {
        return refused(QStringLiteral("Could not read the image to fingerprint it"));
    }
    const sak::PartitionTarget target = sak::diskOperationTarget(context.disk);
    return oneRequest(sak::PartitionOperationType::RestoreImage,
                      target,
                      sak::restoreImagePayload(target,
                                               image,
                                               static_cast<uint64_t>(info.size()),
                                               QString::fromStdString(*digest).toLower()));
}

Result<Requests> buildRestoreRecovered(const BuildContext& context) {
    const QJsonObject& choices = context.job.choices;
    return oneRequest(sak::PartitionOperationType::RestoreRecoveredPartition,
                      sak::diskOperationTarget(context.disk),
                      sak::restoreRecoveredPartitionPayload(sak::RecoveredPartitionChoices{
                          .offset_bytes = jsonUInt64(choices, "offset_bytes"),
                          .size_bytes = jsonUInt64(choices, "size_bytes"),
                          .type_id = jsonString(choices, "type_id"),
                          .partition_style = context.disk.partition_style,
                          .restore_acknowledged = true}));
}

Result<Requests> buildRepairBoot(const BuildContext& context) {
    const QString mode = jsonString(context.job.choices, "boot_mode");
    if (context.disk.is_boot || context.disk.is_system) {
        return oneRequest(
            sak::PartitionOperationType::RepairBoot,
            sak::diskOperationTarget(context.disk),
            sak::repairRunningBootPayload(jsonString(context.job.choices, "windows_path"), mode));
    }
    return oneRequest(sak::PartitionOperationType::RepairBoot,
                      sak::diskOperationTarget(context.disk),
                      sak::repairAttachedBootPayload(mode));
}

Result<Requests> buildWipeDisk(const BuildContext& context) {
    // The panel's Wipe Disk asks with a check box and queues no payload. Its SSD erase asks for
    // typed words and queues the two fields that say so.
    const bool ssd = context.job.choices.value(QLatin1String("ssd_secure_erase")).toBool();
    return oneRequest(sak::PartitionOperationType::WipeDisk,
                      sak::diskOperationTarget(context.disk),
                      ssd ? sak::ssdSecureErasePayload() : QJsonObject{});
}

QString mountPoint(const sak::PartitionInfoEx& partition) {
    return partition.volume
               ? QStringLiteral("%1:").arg(partition.volume->drive_letter.left(1).toUpper())
               : QString();
}

using RequestBuilder = std::function<Result<Requests>(const BuildContext&)>;

void addPayloadBuilders(std::map<QString, RequestBuilder>* builders) {
    using Type = sak::PartitionOperationType;
    builders->emplace(QStringLiteral("Format"), [](const BuildContext& context) {
        return buildOnPartition(context, Type::Format, [&context](const sak::PartitionInfoEx&) {
            return sak::formatPartitionPayload(formatChoices(context.job.choices));
        });
    });
    builders->emplace(QStringLiteral("ChangeClusterSize"), [](const BuildContext& context) {
        return buildOnPartition(
            context, Type::ChangeClusterSize, [&context](const sak::PartitionInfoEx& partition) {
                return sak::clusterSizePayload(partition.volume.value_or(
                                                   sak::PartitionVolumeInfo{}),
                                               formatChoices(context.job.choices),
                                               backupChoices(context.job.choices));
            });
    });
    builders->emplace(QStringLiteral("ConvertPrimaryLogical"), [](const BuildContext& context) {
        return buildOnPartition(context,
                                Type::ConvertPrimaryLogical,
                                [&context](const sak::PartitionInfoEx& partition) {
                                    return sak::primaryLogicalPayload(
                                        partition,
                                        jsonString(context.job.choices, "target_layout"),
                                        backupChoices(context.job.choices));
                                });
    });
    builders->emplace(QStringLiteral("ChangeVolumeSerialNumber"), [](const BuildContext& context) {
        return buildOnPartition(context,
                                Type::ChangeVolumeSerialNumber,
                                [&context](const sak::PartitionInfoEx& partition) {
                                    return sak::volumeSerialPayload(
                                        partition.volume.value_or(sak::PartitionVolumeInfo{}),
                                        backupChoices(context.job.choices));
                                });
    });
    builders->emplace(QStringLiteral("BitLockerUnlock"), [](const BuildContext& context) {
        return buildOnPartition(
            context, Type::BitLockerUnlock, [&context](const sak::PartitionInfoEx& partition) {
                return sak::bitLockerUnlockPayload(
                    mountPoint(partition), jsonString(context.job.choices, "recovery_password"));
            });
    });
    builders->emplace(QStringLiteral("ConvertPartitionStyle"), [](const BuildContext& context) {
        return oneRequest(Type::ConvertPartitionStyle,
                          sak::diskOperationTarget(context.disk),
                          sak::convertStylePayload(
                              context.disk, jsonString(context.job.choices, "target_style")));
    });
    builders->emplace(QStringLiteral("PartitionRecoveryScan"), [](const BuildContext& context) {
        return oneRequest(Type::PartitionRecoveryScan,
                          sak::diskOperationTarget(context.disk),
                          sak::recoveryScanPayload(jsonString(context.job.choices, "scan_mode")));
    });
}

std::map<QString, RequestBuilder> requestBuilders() {
    using Type = sak::PartitionOperationType;
    const auto simple = [](Type type) {
        return [type](const BuildContext& context) {
            return buildOnPartition(context, type, [](const sak::PartitionInfoEx&) {
                return QJsonObject{};
            });
        };
    };
    const auto byLetter = [](Type type) {
        return [type](const BuildContext& context) {
            return buildOnPartition(context, type, [](const sak::PartitionInfoEx& partition) {
                return sak::driveLetterPayload(mountPoint(partition));
            });
        };
    };
    std::map<QString, RequestBuilder> builders{
        {QStringLiteral("Create"), buildCreate},
        {QStringLiteral("Delete"), simple(Type::Delete)},
        {QStringLiteral("ConvertFileSystem"), simple(Type::ConvertFileSystem)},
        {QStringLiteral("Resize"), buildResize},
        {QStringLiteral("MovePartition"), buildMove},
        {QStringLiteral("AllocateFreeSpace"), buildAllocate},
        {QStringLiteral("ConvertDynamicDiskToBasic"), buildDynamicToBasic},
        {QStringLiteral("QuickPartition"), buildQuickPartition},
        {QStringLiteral("CloneDisk"), buildCloneDisk},
        {QStringLiteral("MigrateOs"), buildMigrate},
        {QStringLiteral("ClonePartition"), buildClonePartition},
        {QStringLiteral("CreateImage"), buildCreateImage},
        {QStringLiteral("RestoreImage"), buildRestoreImage},
        {QStringLiteral("RestoreRecoveredPartition"), buildRestoreRecovered},
        {QStringLiteral("RepairBoot"), buildRepairBoot},
        {QStringLiteral("WipeDisk"), buildWipeDisk},
        {QStringLiteral("OptimizeSsd"), byLetter(Type::OptimizeSsd)},
        {QStringLiteral("DefragVolume"), byLetter(Type::DefragVolume)},
        {QStringLiteral("BitLockerSuspend"), byLetter(Type::BitLockerSuspend)},
        {QStringLiteral("BitLockerResume"), byLetter(Type::BitLockerResume)},
    };
    addPayloadBuilders(&builders);
    return builders;
}

QJsonObject targetJson(const sak::PartitionTarget& target) {
    return {{"kind",
             target.kind == sak::PartitionTargetKind::Disk          ? "Disk"
             : target.kind == sak::PartitionTargetKind::Unallocated ? "Unallocated"
                                                                    : "Partition"},
            {"disk_number", static_cast<int>(target.disk_number.value())},
            {"partition_number", static_cast<int>(target.partition_number.value())},
            {"offset_bytes", QString::number(target.offset_bytes)},
            {"size_bytes", QString::number(target.size_bytes)},
            {"drive_letter", target.drive_letter},
            {"disk_unique_id", target.disk_unique_id}};
}

/// The serial goes in as a hash: it proves the same device across steps without publishing it.
QJsonObject diskJson(const sak::PartitionDiskInfo& disk) {
    return {{"disk_number", static_cast<int>(disk.disk_number.value())},
            {"model", disk.model},
            {"bus_type", disk.bus_type},
            {"media_type", disk.media_type},
            {"size_bytes", QString::number(disk.size_bytes)},
            {"partition_style", disk.partition_style},
            {"is_boot", disk.is_boot},
            {"is_system", disk.is_system},
            {"partition_count", static_cast<int>(disk.partitions.size())},
            {"serial_sha256", sha256OfText(disk.serial_number.trimmed())}};
}

QJsonObject stepJson(const sak::PartitionExecutionStep& step) {
    return {{"operation_id", step.operation_id},
            {"summary", step.summary},
            {"success", step.success},
            {"skipped", step.skipped},
            {"error_message", step.error_message},
            {"stdout", boundedText(step.stdout_text)},
            {"stderr", boundedText(step.stderr_text)}};
}

QJsonObject executionJson(const sak::PartitionExecutionResult& result) {
    QJsonArray steps;
    std::ranges::for_each(result.steps,
                          [&steps](const auto& step) { steps.append(stepJson(step)); });
    return {{"success", result.success},
            {"dry_run", result.dry_run},
            {"cancelled", result.cancelled},
            {"timed_out", result.timed_out},
            {"message", result.message},
            {"steps", steps}};
}

QJsonObject clientJson(const StepJob& job) {
    const QString exe = QCoreApplication::applicationFilePath();
    const QString helper = QDir(QCoreApplication::applicationDirPath())
                               .filePath(QStringLiteral("sak_elevated_helper.exe"));
    return {{"file_name", QFileInfo(exe).fileName()},
            {"elevated", sak::ElevationManager::isElevated()},
            {"allow_elevated_client", job.allow_elevated_client},
            {"sha256", sha256OfFile(exe)},
            {"helper_sha256", sha256OfFile(helper)},
            {"helper_found", QFileInfo::exists(helper)}};
}

QJsonObject reportBase(const StepJob& job) {
    return {{"tool", kToolName},
            {"schema_version", kSchemaVersion},
            {"scenario_id", job.scenario_id},
            {"step", job.step},
            {"operation", job.operation},
            {"run_id", job.run_id},
            {"expect", job.expect},
            {"status", "Failed"},
            {"execution_engine", "PartitionManagerController"},
            {"route", "elevated-helper"},
            {"started_utc", QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)},
            {"code_digest", QLatin1String(SAK_CODE_DIGEST)},
            {"code_commit", QLatin1String(SAK_CODE_COMMIT)},
            {"code_dirty", SAK_CODE_DIRTY},
            {"client", clientJson(job)}};
}

/// Runs the controller to rest: a scan, or a queue's Apply through its verification scan.
class ControllerWait {
public:
    ControllerWait(sak::PartitionManagerController& controller, int timeout_seconds)
        : m_controller(controller), m_timeout_ms(timeout_seconds * kMillisecondsPerSecond) {}

    Result<std::optional<sak::PartitionExecutionResult>> run(const std::function<bool()>& start) {
        QEventLoop loop;
        QTimer deadline;
        QTimer grace;
        deadline.setSingleShot(true);
        grace.setSingleShot(true);
        bool timed_out = false;
        bool abandoned = false;
        std::optional<sak::PartitionExecutionResult> execution;
        QObject::connect(&m_controller,
                         &sak::PartitionManagerController::executionFinished,
                         &loop,
                         [&execution](const auto& result) { execution = result; });
        QObject::connect(
            &m_controller, &sak::PartitionManagerController::stateChanged, &loop, [this, &loop]() {
                if (!m_controller.interactionBlocked()) {
                    loop.quit();
                }
            });
        QObject::connect(&deadline, &QTimer::timeout, &loop, [&]() {
            timed_out = true;
            m_controller.cancel();
            grace.start(kCancelGraceMs);
        });
        QObject::connect(&grace, &QTimer::timeout, &loop, [&]() {
            abandoned = true;
            loop.quit();
        });
        deadline.start(m_timeout_ms);
        if (!start()) {
            return refused(QStringLiteral("The controller refused to start"));
        }
        if (m_controller.interactionBlocked()) {
            loop.exec();
        }
        if (abandoned) {
            return refused(QStringLiteral("The controller did not stop after Cancel"));
        }
        if (timed_out) {
            return refused(QStringLiteral("The step ran past its time limit"));
        }
        return execution;
    }

private:
    sak::PartitionManagerController& m_controller;
    int m_timeout_ms;
};

Result<void> refreshInventory(sak::PartitionManagerController& controller, int timeout_seconds) {
    const auto scanned = ControllerWait(controller, timeout_seconds).run([&controller]() {
        controller.refreshInventory();
        return true;
    });
    if (!scanned) {
        return refused(scanned.error());
    }
    if (controller.inventory().isEmpty()) {
        return refused(QStringLiteral("The inventory scan found no disks"));
    }
    return {};
}

/// The panel's Refuse: what the checks said, kept for the row to compare with what it expected.
struct QueueOutcome {
    bool queued{false};
    QStringList reasons;
};

QueueOutcome queueRequests(sak::PartitionManagerController& controller, const Requests& requests) {
    QueueOutcome outcome;
    const QMetaObject::Connection connection =
        QObject::connect(&controller,
                         &sak::PartitionManagerController::queueRefused,
                         &controller,
                         [&outcome](const QStringList& reasons) { outcome.reasons = reasons; });
    outcome.queued = controller.queueOperations(requests);
    QObject::disconnect(connection);
    return outcome;
}

void recordQueued(const sak::PartitionManagerController& controller, QJsonObject* report) {
    const sak::PartitionScriptBuilder builder;
    QJsonArray operations;
    for (const auto& operation : controller.queue().operations()) {
        const auto script = builder.buildScript(operation);
        QJsonArray warnings;
        std::ranges::for_each(operation.warnings,
                              [&warnings](const QString& item) { warnings.append(item); });
        operations.append(QJsonObject{
            {"id", operation.id},
            {"type", sak::toDisplayString(operation.type)},
            {"summary", operation.summary},
            {"target", targetJson(operation.target)},
            {"payload", withoutSecrets(operation.payload)},
            {"warnings", warnings},
            {"script_sha256", script.valid() ? sha256OfText(script.script) : QString()},
        });
    }
    const auto token = controller.queueReviewToken();
    report->insert("queued_operations", operations);
    report->insert("review",
                   QJsonObject{{"revision", QString::number(token.revision)},
                               {"fingerprint", token.fingerprint},
                               {"layout_hash", token.layout_hash}});
}

bool expectedRefusal(const StepJob& job, const QStringList& reasons) {
    return job.expect == QStringLiteral("refused") &&
           reasons.join(QLatin1Char(' ')).contains(job.refusal_contains, Qt::CaseInsensitive);
}

bool executionPassed(const sak::PartitionExecutionResult& result, qsizetype queued) {
    return result.success && !result.dry_run && !result.cancelled && !result.timed_out &&
           result.steps.size() == queued && std::ranges::all_of(result.steps, [](const auto& step) {
               return step.success && !step.skipped;
           });
}

bool expectedFailure(const StepJob& job, const sak::PartitionExecutionResult& result) {
    const QString said = result.message + QLatin1Char(' ') +
                         (result.steps.isEmpty() ? QString()
                                                 : result.steps.constLast().stderr_text);
    return job.expect == QStringLiteral("failed") && !result.success && !result.cancelled &&
           said.contains(job.refusal_contains, Qt::CaseInsensitive);
}

Result<void> applyQueued(sak::PartitionManagerController& controller,
                         const StepJob& job,
                         QJsonObject* report) {
    const qsizetype queued = controller.queue().operations().size();
    const auto token = controller.queueReviewToken();
    QStringList refusal;
    QString status;
    const QMetaObject::Connection on_refusal =
        QObject::connect(&controller,
                         &sak::PartitionManagerController::applyRefused,
                         &controller,
                         [&refusal](const QStringList& reasons) { refusal = reasons; });
    const QMetaObject::Connection on_status =
        QObject::connect(&controller,
                         &sak::PartitionManagerController::statusMessage,
                         &controller,
                         [&status](const QString& message, int) { status = message; });
    const auto stop_listening = qScopeGuard([on_refusal, on_status]() {
        QObject::disconnect(on_refusal);
        QObject::disconnect(on_status);
    });
    const auto execution = ControllerWait(controller, job.timeout_seconds).run([&]() {
        return controller.applyQueue(token, false, true);
    });
    if (!execution) {
        return refused(execution.error());
    }
    if (!execution->has_value()) {
        // Apply read the disks and refused: nothing ran. That is a result the row can expect.
        report->insert("apply_refusal_reasons", QJsonArray::fromStringList(refusal));
        if (expectedRefusal(job, refusal)) {
            report->insert("refused_at", QStringLiteral("apply"));
            report->insert("expectation_met", true);
            return {};
        }
        return refused(QStringLiteral("Apply refused: %1")
                           .arg(refusal.isEmpty() ? status : refusal.join(QStringLiteral("; "))));
    }
    const auto& result = **execution;
    report->insert("execution", executionJson(result));
    report->insert("helper_connected", sak::ElevationBroker::sessionBroker()->isConnected());
    if (!executionPassed(result, queued) && !expectedFailure(job, result)) {
        return refused(
            QStringLiteral("Apply did not do what the step expected: %1").arg(result.message));
    }
    report->insert("expectation_met", true);
    return {};
}

/// Data Recovery runs in the app's own process, with no helper: the panel calls this engine with
/// what its dialogs collected, and reads a raw volume only when the app itself runs as
/// administrator.
Result<void> runFileRecovery(const StepJob& job, QJsonObject* report) {
    const QString source = jsonString(job.choices, "source_path");
    sak::FileRecoveryScanOptions scan_options;
    scan_options.image_path = source;
    scan_options.max_scan_bytes = sak::kFileRecoveryWholeSourceScanBytes;
    scan_options.max_candidates = sak::kFileRecoveryPanelMaxCandidates;
    const auto scan = sak::FileRecoveryEngine::scanOfflineImage(scan_options);
    report->insert("recovery_scan",
                   QJsonObject{{"bytes_read", QString::number(scan.bytes_read)},
                               {"candidates", static_cast<int>(scan.candidates.size())},
                               {"source_opened_read_only", scan.source_opened_read_only},
                               {"warnings", QJsonArray::fromStringList(scan.warnings)}});
    if (sak::classifyFileRecoveryScan(scan) != sak::FileRecoveryScanOutcome::Found) {
        return refused(QStringLiteral("The scan found nothing to restore: %1")
                           .arg(scan.warnings.join(QStringLiteral("; "))));
    }
    sak::FileRecoveryRestoreOptions restore_options;
    restore_options.image_path = source;
    restore_options.destination_directory = jsonString(job.choices, "destination_directory");
    restore_options.candidates = scan.candidates;
    restore_options.source_hash_bytes = sak::kFileRecoveryDefaultMaxScanBytes;
    const auto restore = sak::FileRecoveryEngine::restoreCandidates(restore_options);
    report->insert("recovery_restore",
                   QJsonObject{{"restored", static_cast<int>(restore.restored_paths.size())},
                               {"source_not_mutated", restore.source_not_mutated},
                               {"warnings", QJsonArray::fromStringList(restore.warnings)}});
    report->insert("route", QStringLiteral("in-process"));
    report->insert("expectation_met", true);
    return {};
}

Result<void> checkClient(const StepJob& job) {
    if (QFileInfo(QCoreApplication::applicationFilePath())
            .fileName()
            .compare(QStringLiteral("sak_utility.exe"), Qt::CaseInsensitive) != 0) {
        return refused(
            QStringLiteral("A step runs as sak_utility.exe: the helper accepts no other client"));
    }
    if (sak::ElevationManager::isElevated() && !job.allow_elevated_client) {
        return refused(QStringLiteral(
            "This step runs under a standard-user token, as the app does, and this process is "
            "elevated"));
    }
    return {};
}

Result<void> runStep(sak::PartitionManagerController& controller,
                     const StepJob& job,
                     QJsonObject* report) {
    if (const auto client = checkClient(job); !client) {
        return client;
    }
    if (job.operation == QStringLiteral("FileRecovery")) {
        return runFileRecovery(job, report);
    }
    if (const auto scan = refreshInventory(controller, job.timeout_seconds); !scan) {
        return scan;
    }
    const sak::PartitionInventory inventory = controller.inventory();
    report->insert("inventory_warnings", QJsonArray::fromStringList(inventory.warnings));
    report->insert("helper_connected_after_scan",
                   sak::ElevationBroker::sessionBroker()->isConnected());
    const auto disk = findPinned(inventory, job.pin);
    if (!disk) {
        return refused(disk.error());
    }
    report->insert("disk_before", diskJson(**disk));
    const auto builders = requestBuilders();
    const auto builder = builders.find(job.operation);
    if (builder == builders.cend()) {
        return refused(QStringLiteral("No such operation: %1").arg(job.operation));
    }
    const auto requests = builder->second(BuildContext{job, inventory, **disk});
    if (!requests) {
        return refused(requests.error());
    }
    const QueueOutcome queued = queueRequests(controller, *requests);
    report->insert("queue_refusal_reasons", QJsonArray::fromStringList(queued.reasons));
    if (!queued.queued) {
        if (expectedRefusal(job, queued.reasons)) {
            report->insert("refused_at", QStringLiteral("queue"));
            report->insert("expectation_met", true);
            return {};
        }
        return refused(QStringLiteral("Not added to the queue: %1")
                           .arg(queued.reasons.join(QStringLiteral("; "))));
    }
    recordQueued(controller, report);
    return applyQueued(controller, job, report);
}

int finish(const StepJob& job, const Result<void>& outcome, QJsonObject* report) {
    if (outcome) {
        report->insert("status", "Passed");
    } else {
        report->insert("error", outcome.error());
    }
    report->insert("completed_utc", QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs));
    QSaveFile file(job.report_path);
    if (!file.open(QIODevice::WriteOnly) ||
        file.write(QJsonDocument(*report).toJson(QJsonDocument::Indented)) < 0 || !file.commit()) {
        return kExitFailed;
    }
    return outcome ? kExitPassed : kExitFailed;
}

}  // namespace

int runScenarioStep(const QString& job_path) {
    const auto job = readJob(job_path);
    if (!job) {
        fprintf(stderr, "%s\n", job.error().toUtf8().constData());
        return kExitFailed;
    }
    QJsonObject report = reportBase(*job);
    sak::PartitionManagerController controller;
    const auto outcome = runStep(controller, *job, &report);
    return finish(*job, outcome, &report);
}

namespace {

using AddCheck = std::function<void(const QString&, bool)>;

void selfTestPins(const AddCheck& add) {
    const DiskPin pin{.unique_id = QStringLiteral("ID"),
                      .serial_number = QStringLiteral("SER"),
                      .model = QStringLiteral("Model"),
                      .size_bytes = 100};
    sak::PartitionDiskInfo disk;
    disk.unique_id = QStringLiteral("ID");
    disk.serial_number = QStringLiteral("SER");
    disk.model = QStringLiteral("Model");
    disk.size_bytes = 100;
    add(QStringLiteral("step-pin-matches-exact-identity"), pinMatches(pin, disk));
    for (const auto& [name, change] :
         std::initializer_list<std::pair<const char*, void (*)(sak::PartitionDiskInfo*)>>{
             {"unique-id", [](sak::PartitionDiskInfo* d) { d->unique_id = QStringLiteral("X"); }},
             {"serial", [](sak::PartitionDiskInfo* d) { d->serial_number = QStringLiteral("X"); }},
             {"model", [](sak::PartitionDiskInfo* d) { d->model = QStringLiteral("X"); }},
             {"size", [](sak::PartitionDiskInfo* d) { d->size_bytes = 99; }}}) {
        sak::PartitionDiskInfo other = disk;
        change(&other);
        add(QStringLiteral("step-pin-refuses-other-%1").arg(QLatin1String(name)),
            !pinMatches(pin, other));
    }
    sak::PartitionInventory inventory;
    inventory.disks = {disk, disk};
    add(QStringLiteral("step-pin-refuses-two-matches"), !findPinned(inventory, pin).has_value());
    inventory.disks.clear();
    add(QStringLiteral("step-pin-refuses-no-match"), !findPinned(inventory, pin).has_value());
}

void selfTestJobs(const AddCheck& add) {
    const DiskPin pin{.unique_id = QStringLiteral("ID"),
                      .serial_number = QStringLiteral("SER"),
                      .model = QStringLiteral("Model"),
                      .size_bytes = 100};
    const QJsonObject good_pin{
        {"unique_id", "ID"}, {"serial_number", "SER"}, {"model", "Model"}, {"size_bytes", 100}};
    const QJsonObject job{{"scenario_id", "vhd.x"},
                          {"step", "one"},
                          {"run_id", "r"},
                          {"operation", "Create"},
                          {"report_path", "r.json"},
                          {"expect", "success"},
                          {"pin", good_pin},
                          {"confirmation", expectedConfirmation(pin)}};
    add(QStringLiteral("step-job-accepted"), parseJob(job).has_value());
    QJsonObject wrong_words = job;
    wrong_words["confirmation"] = QStringLiteral("CERTIFY Model SER 101");
    add(QStringLiteral("step-job-refuses-confirmation-of-another-disk"),
        !parseJob(wrong_words).has_value());
    QJsonObject no_expect = job;
    no_expect.remove(QStringLiteral("expect"));
    add(QStringLiteral("step-job-refuses-no-expectation"), !parseJob(no_expect).has_value());
    QJsonObject refusal = job;
    refusal["expect"] = QStringLiteral("refused");
    add(QStringLiteral("step-job-refused-needs-a-reason"), !parseJob(refusal).has_value());
}

void selfTestOutcomes(const AddCheck& add) {
    const QJsonObject redacted = withoutSecrets({{"recovery_password", "123"}, {"a", "b"}});
    add(QStringLiteral("step-report-withholds-a-password"),
        redacted.value(QStringLiteral("recovery_password")).toString() ==
                QStringLiteral("<withheld>") &&
            redacted.value(QStringLiteral("a")).toString() == QStringLiteral("b"));

    const StepJob refused_job{.expect = QStringLiteral("refused"),
                              .refusal_contains = QStringLiteral("BitLocker")};
    add(QStringLiteral("step-refusal-must-name-the-reason"),
        expectedRefusal(refused_job, {QStringLiteral("the volume is locked by BitLocker")}) &&
            !expectedRefusal(refused_job, {QStringLiteral("some other reason")}));
    add(QStringLiteral("step-report-names-its-code"),
        QString::fromLatin1(SAK_CODE_DIGEST).size() == 64);

    sak::PartitionExecutionResult result;
    result.success = true;
    add(QStringLiteral("step-execution-needs-every-queued-step"), !executionPassed(result, 1));
    sak::PartitionExecutionStep step;
    step.success = true;
    result.steps.append(step);
    add(QStringLiteral("step-execution-passes-with-every-step"), executionPassed(result, 1));
    result.steps.first().skipped = true;
    add(QStringLiteral("step-execution-refuses-a-skipped-step"), !executionPassed(result, 1));
    add(QStringLiteral("step-has-a-builder-for-each-operation"), requestBuilders().size() == 27);
}

}  // namespace

void selfTestScenarioStep(QJsonArray* checks) {
    const AddCheck add = [checks](const QString& name, bool passed) {
        checks->append(QJsonObject{{"name", name}, {"passed", passed}});
    };
    selfTestPins(add);
    selfTestJobs(add);
    selfTestOutcomes(add);
}

}  // namespace certification
