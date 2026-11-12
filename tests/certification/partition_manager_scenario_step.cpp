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
#include "sak/partition_executor.h"
#include "sak/partition_file_system_tool_runner.h"
#include "sak/partition_manager_controller.h"
#include "sak/partition_operation_planner.h"
#include "sak/partition_operation_requests.h"
#include "sak/partition_safety_validator.h"
#include "sak/partition_script_builder.h"
#include "sak/restore_point_manager.h"
#include "sak/storage_inventory_worker.h"

#include "partition_manager_code_digest.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QSaveFile>
#include <QScopeGuard>
#include <QtConcurrent/QtConcurrentRun>
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
    /// A job from scripts/run_partition_manager_operation_proof.ps1. Only such a job reaches the
    /// operation-proof builders and routes below; a certification job never does.
    bool operation_proof{false};
    /// "panel" (the default: the controller's queue and Apply) or "assistant" (what the AI
    /// assistant's partition.apply_operation runs). Operation proof only.
    QString route;
    /// Stop the Apply this long after it starts, as a person pressing Stop. Operation proof only.
    int cancel_after_seconds{0};
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

/// What a job expects of its step. A stopped Apply is something only an operation-proof job asks
/// for, and only it may pick a route or a stop.
Result<void> checkJobExpectation(const StepJob& job) {
    const QStringList expectations{QStringLiteral("success"),
                                   QStringLiteral("refused"),
                                   QStringLiteral("failed")};
    const bool proof_cancel = job.operation_proof && job.expect == QStringLiteral("cancelled");
    if (!expectations.contains(job.expect) && !proof_cancel) {
        return refused(
            QStringLiteral("A step job must say what it expects: success, refused or "
                           "failed"));
    }
    if (job.expect != QStringLiteral("success") && !proof_cancel &&
        job.refusal_contains.isEmpty()) {
        return refused(QStringLiteral("A step that expects a refusal must name the reason"));
    }
    if (!job.operation_proof && (!job.route.isEmpty() || job.cancel_after_seconds != 0)) {
        return refused(QStringLiteral("Only an operation-proof job picks a route or a stop"));
    }
    return {};
}

Result<void> checkJobFields(const StepJob& job) {
    if (job.scenario_id.isEmpty() || job.step.isEmpty() || job.run_id.isEmpty() ||
        job.operation.isEmpty() || job.report_path.isEmpty()) {
        return refused(QStringLiteral(
            "A step job needs a scenario, a step, a run id, an operation and a report path"));
    }
    return checkJobExpectation(job);
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
                    root.value(QLatin1String("allow_elevated_client")).toBool(false),
                .operation_proof = root.value(QLatin1String("operation_proof")).toBool(false),
                .route = jsonString(root, "route"),
                .cancel_after_seconds = root.value(QLatin1String("cancel_after_seconds")).toInt(0)};
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
            {"cancelled", step.cancelled},
            {"may_still_run_in_windows", step.may_still_run_in_windows},
            {"stop_cleanup_removed", QJsonArray::fromStringList(step.stop_cleanup_removed)},
            {"stop_cleanup_not_removed", QJsonArray::fromStringList(step.stop_cleanup_not_removed)},
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

/// Why a scan gave no disks, in the scan's own words: a scan that timed out, and one that found
/// nothing, read the same without them.
QString emptyScanRefusal(const sak::PartitionInventory& inventory) {
    if (inventory.warnings.isEmpty()) {
        return QStringLiteral("The inventory scan found no disks");
    }
    return QStringLiteral("The inventory scan found no disks: %1")
        .arg(inventory.warnings.join(QStringLiteral("; ")));
}

Result<void> refreshInventory(sak::PartitionManagerController& controller, int timeout_seconds) {
    const auto scanned = ControllerWait(controller, timeout_seconds).run([&controller]() {
        controller.refreshInventory();
        return true;
    });
    if (!scanned) {
        return refused(scanned.error());
    }
    if (controller.inventory().isEmpty()) {
        return refused(emptyScanRefusal(controller.inventory()));
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

/// An operation-proof step that pressed Stop expects the batch to say it was stopped.
bool expectedCancel(const StepJob& job, const sak::PartitionExecutionResult& result) {
    return job.operation_proof && job.expect == QStringLiteral("cancelled") && result.cancelled;
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
    // Only an operation-proof job presses Stop; it says how long after the Apply starts.
    QTimer stop;
    stop.setSingleShot(true);
    QObject::connect(&stop, &QTimer::timeout, &controller, [&controller]() {
        controller.cancel();
    });
    const auto execution = ControllerWait(controller, job.timeout_seconds).run([&]() {
        const bool started = controller.applyQueue(token, false, true);
        if (started && job.cancel_after_seconds > 0) {
            stop.start(job.cancel_after_seconds * kMillisecondsPerSecond);
        }
        return started;
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
    if (!executionPassed(result, queued) && !expectedFailure(job, result) &&
        !expectedCancel(job, result)) {
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

/// The panel's queue and Apply for requests already built: a refusal the step expected is its
/// result, and anything else that is not queued stops the step.
Result<void> queueAndApply(sak::PartitionManagerController& controller,
                           const StepJob& job,
                           const Requests& requests,
                           QJsonObject* report) {
    const QueueOutcome queued = queueRequests(controller, requests);
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

// --- Operation proof ----------------------------------------------------------------------------
// scripts/run_partition_manager_operation_proof.ps1 runs every operation a person can reach, not
// only the ones the certification rows name. Its jobs say operation_proof, and only they reach
// what follows, so no certification row's steps change.
//
// Where the panel builds a payload with a core function, so does this. Where the panel builds it
// inline in src/gui/partition_manager_panel.cpp, this builds it the same way and the step's report
// names the panel function it follows (payload_source), so a reader knows which payloads are
// copies of the panel's and which are the panel's own code.

using OpType = sak::PartitionOperationType;

bool isPanelNonNativeFormat(const QString& file_system) {
    // isExtFilesystem, isHfsFilesystem, isApfsFilesystem and isLinuxSwapFilesystem in the panel.
    static const QStringList kNonNative{QStringLiteral("ext2"),
                                        QStringLiteral("ext3"),
                                        QStringLiteral("ext4"),
                                        QStringLiteral("hfs+"),
                                        QStringLiteral("hfsx"),
                                        QStringLiteral("hfsplus"),
                                        QStringLiteral("apfs"),
                                        QStringLiteral("linux swap"),
                                        QStringLiteral("linux-swap"),
                                        QStringLiteral("swap")};
    return kNonNative.contains(file_system.trimmed().toLower());
}

bool isPanelLinuxSwap(const QString& file_system) {
    const QString token = file_system.trimmed().toLower();
    return token == QStringLiteral("linux swap") || token == QStringLiteral("linux-swap") ||
           token == QStringLiteral("swap");
}

bool isPanelExt(const QString& file_system) {
    const QString token = file_system.trimmed().toLower();
    return token == QStringLiteral("ext2") || token == QStringLiteral("ext3") ||
           token == QStringLiteral("ext4");
}

/// nonNativeFilesystemWriteTargetPath in the panel: what a non-Windows tool writes through.
QString panelRawPartitionPath(const sak::PartitionInfoEx& partition) {
    return QStringLiteral("\\\\?\\GLOBALROOT\\Device\\Harddisk%1\\Partition%2")
        .arg(partition.disk_number.value())
        .arg(partition.partition_number.value());
}

/// nonNativeFilesystemTargetPath in the panel: what a read-only check reads.
QString panelReadOnlyCheckPath(const sak::PartitionTarget& target,
                               const sak::PartitionInfoEx& partition) {
    if (!target.drive_letter.trimmed().isEmpty()) {
        return QStringLiteral("\\\\.\\%1:").arg(target.drive_letter.left(1).toUpper());
    }
    return panelRawPartitionPath(partition);
}

QString volumeFileSystem(const sak::PartitionInfoEx& partition) {
    return partition.volume ? partition.volume->file_system.trimmed() : QString();
}

/// The fields every non-Windows tool payload carries, with the confirmation the dialog's check box
/// gives.
void addPanelNonNativeFields(QJsonObject* payload, const sak::PartitionInfoEx& partition) {
    payload->insert(QStringLiteral("non_native_file_system_tool"), true);
    payload->insert(QStringLiteral("target_path"), panelRawPartitionPath(partition));
    payload->insert(QStringLiteral("target_wipe_confirmed"), true);
}

/// apfsContainerBasePayload in the panel.
QJsonObject panelApfsBasePayload(const sak::PartitionInfoEx& partition) {
    QJsonObject payload;
    addPanelNonNativeFields(&payload, partition);
    payload.insert(QStringLiteral("file_system"), QStringLiteral("APFS"));
    payload.insert(QStringLiteral("apfs_cow_mutation_certified"), true);
    return payload;
}

using ProofPayload = std::function<QJsonObject(const BuildContext&, const sak::PartitionInfoEx&)>;

struct ProofBuilder {
    RequestBuilder build;
    QString payload_source;
};

ProofBuilder onSelectedPartition(OpType type, const ProofPayload& payload, const char* source) {
    return {.build =
                [type, payload](const BuildContext& context) {
                    return buildOnPartition(context,
                                            type,
                                            [&](const sak::PartitionInfoEx& partition) {
                                                return payload(context, partition);
                                            });
                },
            .payload_source = QString::fromLatin1(source)};
}

/// The panel's withValue(key, value): one field, carried from the job with its JSON type.
ProofBuilder withChoice(OpType type, const char* key, const char* source) {
    return onSelectedPartition(
        type,
        [key](const BuildContext& context, const sak::PartitionInfoEx&) {
            return QJsonObject{{QLatin1String(key), context.job.choices.value(QLatin1String(key))}};
        },
        source);
}

ProofBuilder onDisk(OpType type, const QJsonObject& payload, const char* source) {
    return {.build =
                [type, payload](const BuildContext& context) {
                    return oneRequest(type, sak::diskOperationTarget(context.disk), payload);
                },
            .payload_source = QString::fromLatin1(source)};
}

QJsonObject panelFormatPayload(const BuildContext& context, const sak::PartitionInfoEx& partition) {
    // formatPartitionPayload(widgets, partition): the core payload, and for a non-Windows file
    // system the tool's fields. Additional APFS volumes and encryption are left at their defaults.
    const QJsonObject& choices = context.job.choices;
    QJsonObject payload = sak::formatPartitionPayload(formatChoices(choices));
    const QString file_system = jsonString(choices, "file_system");
    if (isPanelNonNativeFormat(file_system)) {
        addPanelNonNativeFields(&payload, partition);
    }
    if (isPanelLinuxSwap(file_system)) {
        payload.insert(QStringLiteral("linux_swap_page_size_bytes"),
                       jsonString(choices, "linux_swap_page_size_bytes"));
    }
    return payload;
}

QJsonObject panelResizePayload(const BuildContext& context, const sak::PartitionInfoEx& partition) {
    // resizePartitionPayload(widgets, partition): an ext volume adds the tool's fields.
    QJsonObject payload =
        sak::resizePartitionPayload(jsonMiB(context.job.choices, "target_size_mib"),
                                    sak::adjacentFreeBytesAfter(context.disk, partition));
    if (isPanelExt(volumeFileSystem(partition))) {
        addPanelNonNativeFields(&payload, partition);
        payload.insert(QStringLiteral("file_system"), volumeFileSystem(partition));
    }
    return payload;
}

QJsonObject panelRepairPayload(const BuildContext&, const sak::PartitionInfoEx& partition) {
    // nonNativeRepairPayload in the panel, queued as Check File System with repair chosen.
    QJsonObject payload{{QStringLiteral("file_system"), volumeFileSystem(partition)}};
    addPanelNonNativeFields(&payload, partition);
    return payload;
}

QJsonObject panelMergePayload(const BuildContext& context, const sak::PartitionInfoEx&) {
    const int source = context.job.choices.value(QLatin1String("source_partition_number")).toInt();
    return {{"source_partition_number", QString::number(source)},
            {"target_folder", QStringLiteral("Merged-Partition-%1").arg(source)}};
}

QJsonObject panelSplitPayload(const BuildContext& context, const sak::PartitionInfoEx&) {
    return {{"first_size_bytes", QString::number(jsonMiB(context.job.choices, "first_size_mib"))}};
}

QJsonObject panelApfsPayload(const BuildContext& context, const sak::PartitionInfoEx& partition) {
    // apfsVolumeLabelPayload and apfsContainerActionPayload in the panel.
    QJsonObject payload = panelApfsBasePayload(partition);
    for (const char* key : {"label", "apfs_snapshot_name"}) {
        if (context.job.choices.contains(QLatin1String(key))) {
            payload.insert(QLatin1String(key), jsonString(context.job.choices, key));
        }
    }
    return payload;
}

void addProofPartitionBuilders(std::map<QString, ProofBuilder>* builders) {
    builders->emplace(
        QStringLiteral("Format"),
        onSelectedPartition(OpType::Format, panelFormatPayload, "formatPartitionPayload"));
    builders->emplace(
        QStringLiteral("Resize"),
        onSelectedPartition(OpType::Resize, panelResizePayload, "resizePartitionPayload"));
    builders->emplace(
        QStringLiteral("RepairFileSystem"),
        onSelectedPartition(OpType::CheckFileSystem, panelRepairPayload, "nonNativeRepairPayload"));
    builders->emplace(QStringLiteral("Merge"),
                      onSelectedPartition(OpType::Merge, panelMergePayload, "onMergePartitions"));
    builders->emplace(QStringLiteral("Split"),
                      onSelectedPartition(OpType::Split, panelSplitPayload, "onSplitPartition"));
    for (const auto& [name, type] : std::initializer_list<std::pair<const char*, OpType>>{
             {"ApfsChangeVolumeLabel", OpType::ApfsChangeVolumeLabel},
             {"ApfsSnapshotCreate", OpType::ApfsSnapshotCreate},
             {"ApfsSnapshotDelete", OpType::ApfsSnapshotDelete},
             {"ApfsSnapshotRevert", OpType::ApfsSnapshotRevert},
             {"ApfsResizeContainer", OpType::ApfsResizeContainer}}) {
        builders->emplace(QString::fromLatin1(name),
                          onSelectedPartition(type, panelApfsPayload, "apfsContainerBasePayload"));
    }
}

void addProofValueBuilders(std::map<QString, ProofBuilder>* builders) {
    builders->emplace(QStringLiteral("SetDriveLetter"),
                      withChoice(OpType::SetDriveLetter, "new_drive_letter", "onSetDriveLetter"));
    builders->emplace(QStringLiteral("SetPartitionLabel"),
                      withChoice(OpType::SetPartitionLabel, "label", "onSetPartitionLabel"));
    builders->emplace(QStringLiteral("SetPartitionHidden"),
                      withChoice(OpType::SetPartitionHidden, "hidden", "onSetPartitionHidden"));
    builders->emplace(QStringLiteral("SetPartitionActive"),
                      withChoice(OpType::SetPartitionActive, "active", "onSetPartitionActive"));
    builders->emplace(QStringLiteral("SetPartitionTypeId"),
                      withChoice(OpType::SetPartitionTypeId, "type_id", "onSetPartitionTypeId"));
    const auto no_payload = [](const BuildContext&, const sak::PartitionInfoEx&) {
        return QJsonObject{};
    };
    builders->emplace(QStringLiteral("WipePartition"),
                      onSelectedPartition(OpType::WipePartition, no_payload, "onWipeSelected"));
    builders->emplace(QStringLiteral("WipeFreeSpace"),
                      onSelectedPartition(OpType::WipeFreeSpace, no_payload, "onWipeSelected"));
    builders->emplace(QStringLiteral("DeleteAllPartitions"),
                      onDisk(OpType::DeleteAllPartitions, {}, "onDeleteAllPartitions"));
}

/// The builders only an operation-proof job reaches. A name here takes the place of the
/// certification builder of the same name for that job.
std::map<QString, ProofBuilder> operationProofBuilders() {
    std::map<QString, ProofBuilder> builders;
    addProofPartitionBuilders(&builders);
    addProofValueBuilders(&builders);
    // InitializeDisk takes the style the person picked, so it is built where the job is known.
    const RequestBuilder initialize = [](const BuildContext& context) {
        const QJsonObject payload{
            {"target_style", jsonString(context.job.choices, "target_style")}};
        return oneRequest(OpType::InitializeDisk, sak::diskOperationTarget(context.disk), payload);
    };
    builders.emplace(QStringLiteral("InitializeDisk"),
                     ProofBuilder{.build = initialize,
                                  .payload_source = QStringLiteral("onInitializeDisk")});
    return builders;
}

/// The builder an operation-proof job runs, and where its payload comes from.
Result<std::pair<RequestBuilder, QString>> proofBuilderFor(const QString& operation) {
    const auto proof = operationProofBuilders();
    if (const auto found = proof.find(operation); found != proof.cend()) {
        return std::make_pair(found->second.build,
                              QStringLiteral("replicated from PartitionManagerPanel %1")
                                  .arg(found->second.payload_source));
    }
    const auto builders = requestBuilders();
    if (const auto found = builders.find(operation); found != builders.cend()) {
        return std::make_pair(found->second, QStringLiteral("core partition_operation_requests"));
    }
    return refused(QStringLiteral("No such operation: %1").arg(operation));
}

QJsonObject toolRunJson(const sak::PartitionFileSystemToolRunResult& result) {
    return {{"success", result.success},
            {"blocked", result.blocked},
            {"tool_id", result.tool_id},
            {"tool_path", result.tool_path},
            {"arguments", QJsonArray::fromStringList(result.arguments)},
            {"exit_code", result.exit_code},
            {"timed_out", result.timed_out},
            {"cancelled", result.cancelled},
            {"stdout", boundedText(result.std_out)},
            {"stderr", boundedText(result.std_err)},
            {"blockers", QJsonArray::fromStringList(result.blockers)}};
}

/// The panel's read-only Check File System for a non-Windows file system, run now and not queued.
Result<void> runNonNativeCheck(sak::PartitionManagerController& controller,
                               const BuildContext& context,
                               QJsonObject* report) {
    const auto partition = selectPartition(context.disk, context.job.select);
    if (!partition) {
        return refused(partition.error());
    }
    const QString file_system = volumeFileSystem(**partition);
    const QString path = panelReadOnlyCheckPath(sak::partitionSelectionTarget(**partition),
                                                **partition);
    std::optional<sak::PartitionFileSystemToolRunResult> finished;
    QEventLoop loop;
    QTimer deadline;
    deadline.setSingleShot(true);
    QObject::connect(&controller,
                     &sak::PartitionManagerController::fileSystemCheckFinished,
                     &loop,
                     [&](const auto& result) {
                         finished = result;
                         loop.quit();
                     });
    QObject::connect(&deadline, &QTimer::timeout, &loop, &QEventLoop::quit);
    deadline.start(context.job.timeout_seconds * kMillisecondsPerSecond);
    controller.runReadOnlyFileSystemCheck(file_system, path);
    if (!finished) {
        loop.exec();
    }
    report->insert("read_only_check", QJsonObject{{"file_system", file_system}, {"path", path}});
    if (!finished) {
        return refused(QStringLiteral("The file system check did not finish in time"));
    }
    report->insert("file_system_check", toolRunJson(*finished));
    const QString said = finished->blockers.join(QStringLiteral("; ")) + QLatin1Char(' ') +
                         finished->std_out + QLatin1Char(' ') + finished->std_err;
    const bool met = context.job.expect == QStringLiteral("success")
                         ? finished->success
                         : !finished->success &&
                               said.contains(context.job.refusal_contains, Qt::CaseInsensitive);
    if (!met) {
        return refused(QStringLiteral("The file system check did not do what the step expected: %1")
                           .arg(said.trimmed()));
    }
    report->insert("expectation_met", true);
    return {};
}

/// Check File System (Windows) and Scan File Systems: the panel runs these now, not from the queue.
Result<void> runReadOnlyNow(sak::PartitionManagerController& controller,
                            const BuildContext& context,
                            OpType type,
                            QJsonObject* report) {
    sak::PartitionTarget target = sak::diskOperationTarget(context.disk);
    if (type == OpType::CheckFileSystem) {
        const auto partition = selectPartition(context.disk, context.job.select);
        if (!partition) {
            return refused(partition.error());
        }
        target = sak::partitionSelectionTarget(**partition);
    }
    const sak::PartitionScriptBuilder builder;
    const auto script =
        builder.buildScript(sak::PartitionOperationPlanner::makeOperation(type, target));
    report->insert("queued_operations",
                   QJsonArray{
                       QJsonObject{{"type", sak::toDisplayString(type)},
                                   {"target", targetJson(target)},
                                   {"script_sha256",
                                    script.valid() ? sha256OfText(script.script) : QString()}}});
    const auto execution = ControllerWait(controller, context.job.timeout_seconds).run([&]() {
        return controller.runReadOnlyOperation(type, target);
    });
    if (!execution) {
        return refused(execution.error());
    }
    if (!execution->has_value()) {
        return refused(QStringLiteral("The read-only run gave no result"));
    }
    report->insert("execution", executionJson(**execution));
    if (!executionPassed(**execution, 1) && !expectedFailure(context.job, **execution)) {
        return refused(QStringLiteral("The read-only run did not do what the step expected: %1")
                           .arg((*execution)->message));
    }
    report->insert("expectation_met", true);
    return {};
}

/// What the assistant's partition.apply_operation runs (runApplyOperation in
/// src/core/app_mutating_actions.cpp): one operation from the planner, the safety validator, and
/// the executor with elevation, on a thread of its own as PartitionApplyWorker runs it.
Result<void> runAssistantRoute(const StepJob& job, const Requests& requests, QJsonObject* report) {
    if (requests.size() != 1) {
        return refused(QStringLiteral("The assistant applies one operation at a time"));
    }
    const auto operation = sak::PartitionOperationPlanner::makeOperation(
        requests.constFirst().type, requests.constFirst().target, requests.constFirst().payload);
    // The assistant reads the disks itself, without elevation, before it validates.
    const sak::PartitionInventory inventory = sak::StorageInventoryWorker::scanCurrentSystem(false);
    const auto validation = sak::PartitionSafetyValidator::validate(inventory, operation);
    const sak::PartitionScriptBuilder builder;
    const auto script = builder.buildScript(operation);
    report->insert("queued_operations",
                   QJsonArray{
                       QJsonObject{{"type", sak::toDisplayString(operation.type)},
                                   {"target", targetJson(operation.target)},
                                   {"payload", withoutSecrets(operation.payload)},
                                   {"script_sha256",
                                    script.valid() ? sha256OfText(script.script) : QString()}}});
    report->insert("assistant_blockers", QJsonArray::fromStringList(validation.blockers));
    if (!validation.allowed()) {
        if (expectedRefusal(job, validation.blockers)) {
            report->insert("refused_at", QStringLiteral("assistant-validator"));
            report->insert("expectation_met", true);
            return {};
        }
        return refused(QStringLiteral("Operation is blocked by the safety validator: %1")
                           .arg(validation.blockers.join(QStringLiteral("; "))));
    }
    sak::PartitionExecutor executor;
    QFutureWatcher<sak::PartitionExecutionResult> watcher;
    QEventLoop loop;
    QObject::connect(&watcher,
                     &QFutureWatcher<sak::PartitionExecutionResult>::finished,
                     &loop,
                     &QEventLoop::quit);
    QTimer deadline;
    deadline.setSingleShot(true);
    QObject::connect(&deadline, &QTimer::timeout, &loop, [&executor]() { executor.cancel(); });
    watcher.setFuture(QtConcurrent::run(
        [&executor, operation]() { return executor.execute({operation}, false, true); }));
    deadline.start(job.timeout_seconds * kMillisecondsPerSecond);
    loop.exec();
    watcher.waitForFinished();
    const sak::PartitionExecutionResult result = watcher.result();
    report->insert("execution", executionJson(result));
    if (!executionPassed(result, 1) && !expectedFailure(job, result)) {
        return refused(
            QStringLiteral("Apply did not do what the step expected: %1").arg(result.message));
    }
    report->insert("expectation_met", true);
    return {};
}

/// The assistant's diagnostics.create_restore_point: RestorePointManager in the app's own process,
/// which has to be running as administrator. No helper is involved.
Result<void> runRestorePoint(const StepJob& job, QJsonObject* report) {
    const QString description = jsonString(job.choices, "description");
    sak::RestorePointManager manager;
    QString error_text;
    QObject::connect(&manager,
                     &sak::RestorePointManager::restorePointFailed,
                     &manager,
                     [&error_text](const QString& error) {
                         if (error_text.isEmpty()) {
                             error_text = error;
                         }
                     });
    const bool enabled = manager.isSystemRestoreEnabled();
    const bool created = enabled && manager.createRestorePoint(description);
    report->insert("route", QStringLiteral("in-process"));
    report->insert("restore_point",
                   QJsonObject{{"description", description},
                               {"system_restore_enabled", enabled},
                               {"created", created},
                               {"error", error_text}});
    if (!created) {
        return refused(QStringLiteral("No restore point was created: %1").arg(error_text));
    }
    report->insert("expectation_met", true);
    return {};
}

/// The panel's Scan Disks with administrator rights: the inventory read through the helper.
Result<void> runElevatedInventory(sak::PartitionManagerController& controller,
                                  const StepJob& job,
                                  QJsonObject* report) {
    const auto scanned = ControllerWait(controller, job.timeout_seconds).run([&controller]() {
        return controller.refreshInventoryWithAdministratorRights();
    });
    if (!scanned) {
        return refused(scanned.error());
    }
    const sak::PartitionInventory& inventory = controller.inventory();
    QJsonArray disks;
    std::ranges::for_each(inventory.disks,
                          [&disks](const auto& disk) { disks.append(diskJson(disk)); });
    report->insert("elevated_inventory",
                   QJsonObject{{"disks", disks},
                               {"warnings", QJsonArray::fromStringList(inventory.warnings)},
                               {"helper_connected",
                                sak::ElevationBroker::sessionBroker()->isConnected()}});
    if (inventory.isEmpty()) {
        return refused(emptyScanRefusal(inventory));
    }
    report->insert("expectation_met", true);
    return {};
}

/// The operations the panel runs at once instead of queueing, or that run with no disk at all.
std::optional<Result<void>> runProofNow(sak::PartitionManagerController& controller,
                                        const BuildContext& context,
                                        QJsonObject* report) {
    const QString& operation = context.job.operation;
    if (operation == QStringLiteral("CheckFileSystem")) {
        return runReadOnlyNow(controller, context, OpType::CheckFileSystem, report);
    }
    if (operation == QStringLiteral("ScanFileSystems")) {
        return runReadOnlyNow(controller, context, OpType::SurfaceTest, report);
    }
    if (operation == QStringLiteral("CheckNonNativeFileSystem")) {
        return runNonNativeCheck(controller, context, report);
    }
    if (operation == QStringLiteral("ElevatedInventoryScan")) {
        return runElevatedInventory(controller, context.job, report);
    }
    return std::nullopt;
}

/// The assistant's payload is whatever the model writes, so an assistant-route job may add fields
/// no panel request function sets (choices.assistant_payload), as a model may. A panel route never
/// takes them.
Requests withAssistantPayload(Requests requests, const QJsonObject& model_fields) {
    for (auto& request : requests) {
        for (auto field = model_fields.constBegin(); field != model_fields.constEnd(); ++field) {
            request.payload.insert(field.key(), field.value());
        }
    }
    return requests;
}

/// An operation-proof step on its pinned disk: one of the operations run now, or a request built
/// as the panel builds it, then queued and applied, or handed to the assistant's route.
Result<void> runProofStep(sak::PartitionManagerController& controller,
                          const BuildContext& context,
                          QJsonObject* report) {
    report->insert("operation_proof", true);
    report->insert("route_requested",
                   context.job.route.isEmpty() ? QStringLiteral("panel") : context.job.route);
    if (auto now = runProofNow(controller, context, report)) {
        return *now;
    }
    const auto builder = proofBuilderFor(context.job.operation);
    if (!builder) {
        return refused(builder.error());
    }
    report->insert("payload_source", builder->second);
    const auto requests = builder->first(context);
    if (!requests) {
        return refused(requests.error());
    }
    if (context.job.route == QStringLiteral("assistant")) {
        const QJsonObject model_fields =
            context.job.choices.value(QLatin1String("assistant_payload")).toObject();
        return runAssistantRoute(context.job,
                                 withAssistantPayload(*requests, model_fields),
                                 report);
    }
    return queueAndApply(controller, context.job, *requests, report);
}

/// The steps the app runs in its own process, with no helper and no disk to find first.
std::optional<Result<void>> runInProcess(const StepJob& job, QJsonObject* report) {
    if (job.operation == QStringLiteral("FileRecovery")) {
        return runFileRecovery(job, report);
    }
    if (job.operation_proof && job.operation == QStringLiteral("RestorePoint")) {
        return runRestorePoint(job, report);
    }
    return std::nullopt;
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
    if (auto in_process = runInProcess(job, report)) {
        return *in_process;
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
    if (job.operation_proof) {
        return runProofStep(controller, BuildContext{job, inventory, **disk}, report);
    }
    const auto builders = requestBuilders();
    const auto builder = builders.find(job.operation);
    if (builder == builders.cend()) {
        return refused(QStringLiteral("No such operation: %1").arg(job.operation));
    }
    const auto requests = builder->second(BuildContext{job, inventory, **disk});
    if (!requests) {
        return refused(requests.error());
    }
    return queueAndApply(controller, job, *requests, report);
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
    // What only an operation-proof job may ask for stays out of a certification job.
    QJsonObject routed = job;
    routed["route"] = QStringLiteral("assistant");
    add(QStringLiteral("step-certification-job-refuses-a-route"), !parseJob(routed).has_value());
    QJsonObject stopped = job;
    stopped["expect"] = QStringLiteral("cancelled");
    add(QStringLiteral("step-certification-job-refuses-a-stop"), !parseJob(stopped).has_value());
    stopped["operation_proof"] = true;
    stopped["cancel_after_seconds"] = 5;
    add(QStringLiteral("step-proof-job-may-stop"), parseJob(stopped).has_value());
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
    sak::PartitionInventory unfinished;
    unfinished.warnings = {QStringLiteral("The disk scan did not finish")};
    add(QStringLiteral("step-empty-scan-says-why"),
        emptyScanRefusal(unfinished).endsWith(QStringLiteral(": The disk scan did not finish")) &&
            emptyScanRefusal({}) == QStringLiteral("The inventory scan found no disks"));

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

void selfTestOperationProof(const AddCheck& add) {
    add(QStringLiteral("step-proof-has-a-builder-for-each-panel-payload"),
        operationProofBuilders().size() == 19);
    sak::PartitionInfoEx partition;
    partition.disk_number = sak::DiskNumber{3};
    partition.partition_number = sak::PartitionNumber{2};
    add(QStringLiteral("step-proof-writes-through-the-panel-raw-path"),
        panelRawPartitionPath(partition) ==
            QStringLiteral("\\\\?\\GLOBALROOT\\Device\\Harddisk3\\Partition2"));
    const Requests one{{.type = OpType::Create, .payload = QJsonObject{{"file_system", "ext4"}}}};
    const Requests merged = withAssistantPayload(
        one, QJsonObject{{"non_native_file_system_tool", true}, {"target_wipe_confirmed", true}});
    add(QStringLiteral("step-proof-assistant-payload-adds-the-model-fields"),
        merged.size() == 1 &&
            merged.constFirst().payload.value(QLatin1String("file_system")).toString() ==
                QStringLiteral("ext4") &&
            merged.constFirst()
                .payload.value(QLatin1String("non_native_file_system_tool"))
                .toBool());
    add(QStringLiteral("step-proof-format-adds-tool-fields-only-for-non-windows"),
        isPanelNonNativeFormat(QStringLiteral("ext4")) &&
            isPanelNonNativeFormat(QStringLiteral("Linux swap")) &&
            !isPanelNonNativeFormat(QStringLiteral("exFAT")) &&
            !isPanelNonNativeFormat(QStringLiteral("NTFS")));
}

}  // namespace

void selfTestScenarioStep(QJsonArray* checks) {
    const AddCheck add = [checks](const QString& name, bool passed) {
        checks->append(QJsonObject{{"name", name}, {"passed", passed}});
    };
    selfTestPins(add);
    selfTestJobs(add);
    selfTestOutcomes(add);
    selfTestOperationProof(add);
}

}  // namespace certification
