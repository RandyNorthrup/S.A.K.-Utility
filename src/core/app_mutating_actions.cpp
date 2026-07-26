// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

// Mutating technician ops exposed to the AI assistant. Each invoke thunk calls an
// existing headless src/core service (no re-implementation) and serializes its
// result to a compact, model-facing QJsonObject. Every op is marked mutating (and
// destructive / requires_admin where that applies) so the panel's per-action gate
// is enforced -- these thunks never gate themselves and never pop a dialog.

#include "sak/app_mutating_actions.h"

#include "sak/app_action_guards.h"
#include "sak/app_action_registry.h"
#include "sak/app_action_service.h"
#include "sak/app_partition_op_parse.h"
#include "sak/email_export_worker.h"
#include "sak/email_types.h"
#include "sak/flash_coordinator.h"
#include "sak/mbox_parser.h"
#include "sak/organizer_worker.h"
#include "sak/partition_apply_worker.h"
#include "sak/partition_executor.h"
#include "sak/partition_manager_types.h"
#include "sak/partition_operation_planner.h"
#include "sak/partition_safety_validator.h"
#include "sak/storage_inventory_worker.h"
#include "sak/worker_base.h"

#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonObject>
#include <QMap>
#include <QString>
#include <QStringList>

#include <algorithm>
#include <optional>

namespace sak {

namespace {

// Bound how many messages one headless export may write, so a model-supplied (or
// prompt-injected) call cannot spray hundreds of thousands of files onto disk in a
// single gated action. Larger exports use the GUI panel. Truncation is reported.
constexpr int kMaxExportItems = 5000;

// Map the model-facing format string to the MBOX-capable ExportFormat set. MBOX
// export only supports per-message file formats (eml/html/text/pdf); the CSV / VCF /
// ICS formats are PST-only, so they are intentionally not offered here.
ExportFormat exportFormatFromArg(const QString& value) {
    const QString v = value.trimmed().toLower();
    if (v == QLatin1String("html")) {
        return ExportFormat::Html;
    }
    if (v == QLatin1String("text") || v == QLatin1String("txt")) {
        return ExportFormat::Text;
    }
    if (v == QLatin1String("pdf")) {
        return ExportFormat::Pdf;
    }
    return ExportFormat::Eml;  // default + fallback for any unrecognized value
}

QString exportFormatLabel(ExportFormat format) {
    switch (format) {
    case ExportFormat::Html:
        return QStringLiteral("html");
    case ExportFormat::Text:
        return QStringLiteral("text");
    case ExportFormat::Pdf:
        return QStringLiteral("pdf");
    default:
        return QStringLiteral("eml");
    }
}

// Read the optional item_ids array (MBOX message indices). Empty => whole mailbox.
// Non-integer / negative entries are dropped. The count is capped so the export
// cannot be steered into writing an unbounded number of files.
QVector<uint64_t> exportItemIdsFromArgs(const QJsonObject& args, bool& capped) {
    QVector<uint64_t> ids;
    capped = false;
    const QJsonArray raw = args.value(QStringLiteral("item_ids")).toArray();
    for (const QJsonValue& value : raw) {
        if (!value.isDouble()) {
            continue;
        }
        const int index = value.toInt(-1);
        if (index < 0) {
            continue;
        }
        if (ids.size() >= kMaxExportItems) {
            capped = true;
            break;
        }
        ids.append(static_cast<uint64_t>(index));
    }
    return ids;
}

QJsonObject serializeExportResult(const EmailExportResult& result, bool item_ids_capped) {
    QJsonArray errors;
    for (const QString& error : result.errors) {
        errors.append(error);
    }
    return QJsonObject{{QStringLiteral("export_path"), result.export_path},
                       {QStringLiteral("export_format"), result.export_format},
                       {QStringLiteral("items_exported"), result.items_exported},
                       {QStringLiteral("items_failed"), result.items_failed},
                       {QStringLiteral("total_bytes"), static_cast<double>(result.total_bytes)},
                       {QStringLiteral("item_ids_capped"), item_ids_capped},
                       {QStringLiteral("errors"), errors}};
}

// Validate the source/destination paths BEFORE opening anything. Returns an error
// result to short-circuit on, or nullopt when both paths are acceptable. Guards
// BOTH paths against UNC/device forms: the source must not open an SMB/device path
// (credential leak) and the destination must not write onto a network/device path.
std::optional<AppActionResult> validateExportInputs(const QString& path,
                                                    const QString& output_path) {
    if (path.isEmpty() || output_path.isEmpty()) {
        return AppActionResult{false,
                               QStringLiteral("export_mbox requires 'path' and 'output_path'"),
                               {}};
    }
    if (isNetworkOrDevicePath(path) || isNetworkOrDevicePath(output_path)) {
        return AppActionResult{
            false, QStringLiteral("export_mbox does not allow network/UNC or device paths"), {}};
    }
    const QFileInfo info(path);
    if (!info.isFile()) {
        return AppActionResult{false, QStringLiteral("No such MBOX file: %1").arg(path), {}};
    }
    if (info.size() > kMaxMboxBytes) {
        return AppActionResult{
            false,
            QStringLiteral("MBOX file is too large for a headless export (%1 bytes > %2 limit)")
                .arg(info.size())
                .arg(kMaxMboxBytes),
            {}};
    }
    // Require a NEW or EMPTY output directory. This is what actually keeps the op
    // non-destructive: the html/pdf message writers de-duplicate collisions only
    // within a single run and open with WriteOnly / QSaveFile (no exists-check), so
    // writing into a directory that already holds a same-named file would silently
    // overwrite it. Into an empty/new directory no pre-existing file can be clobbered
    // by any format, so the op only ever ADDS files -- hence destructive=false stays
    // honest. (A Windows restore point would not protect arbitrary user files here,
    // so preventing the overwrite is the correct safeguard, not a higher gate tier.)
    const QFileInfo out_info(output_path);
    if (out_info.exists()) {
        if (!out_info.isDir()) {
            return AppActionResult{
                false,
                QStringLiteral("output_path exists and is not a directory: %1").arg(output_path),
                {}};
        }
        if (!QDir(output_path).isEmpty()) {
            return AppActionResult{
                false,
                QStringLiteral(
                    "output_path must be a new or empty directory (refusing to write "
                    "into a non-empty directory to avoid overwriting existing files): %1")
                    .arg(output_path),
                {}};
        }
    }
    return std::nullopt;
}

// Map a completed export into the tool result. Success = something was written, OR
// a clean run with nothing to export; failure only when nothing was exported AND
// the engine reported an error/failure.
AppActionResult buildExportResult(const EmailExportResult& captured,
                                  ExportFormat format,
                                  const QString& output_path,
                                  bool item_ids_capped) {
    const bool ok = captured.items_exported > 0 ||
                    (captured.errors.isEmpty() && captured.items_failed == 0);
    QString message;
    if (ok) {
        message = QStringLiteral("Exported %1 message(s) as %2 to %3")
                      .arg(captured.items_exported)
                      .arg(exportFormatLabel(format), output_path);
        // Surface partial failures in the human-readable message too, not only in
        // the structured payload, so a consumer reading just the message is not
        // misled into thinking every message exported.
        if (captured.items_failed > 0) {
            message += QStringLiteral(" (%1 failed)").arg(captured.items_failed);
        }
    } else if (captured.errors.isEmpty()) {
        message =
            QStringLiteral("MBOX export failed (%1 item(s) failed)").arg(captured.items_failed);
    } else {
        message = captured.errors.first();
    }
    return {ok, message, serializeExportResult(captured, item_ids_capped)};
}

AppActionResult exportMbox(const QJsonObject& args) {
    const QString path = args.value(QStringLiteral("path")).toString().trimmed();
    const QString output_path = args.value(QStringLiteral("output_path")).toString().trimmed();
    if (const std::optional<AppActionResult> error = validateExportInputs(path, output_path)) {
        return *error;
    }

    const ExportFormat format =
        exportFormatFromArg(args.value(QStringLiteral("format")).toString());
    bool item_ids_capped = false;
    const QVector<uint64_t> item_ids = exportItemIdsFromArgs(args, item_ids_capped);

    // MboxParser opens the file READ-ONLY; the export writer creates NEW files under
    // output_path, which validateExportInputs has already required to be new/empty, so
    // no pre-existing file can be overwritten -- the op only ADDS files, never
    // overwrites or deletes. Hence mutating, not destructive.
    MboxParser parser;
    parser.open(path);
    if (!parser.isOpen()) {
        return {false, QStringLiteral("Not a valid MBOX file: %1").arg(path), {}};
    }

    EmailExportConfig config;
    config.format = format;
    config.output_path = output_path;
    config.item_ids = item_ids;

    // exportMboxItems() runs synchronously and emits exportComplete inline on THIS
    // thread (early failures too, via emitEarlyFailure), so a direct connection
    // captures the result without any event loop. errorOccurred is not used by the
    // MBOX path, but is captured defensively in case a writer emits it.
    EmailExportWorker worker;
    EmailExportResult captured;
    bool completed = false;
    QString error_text;
    QObject::connect(&worker,
                     &EmailExportWorker::exportComplete,
                     &worker,
                     [&captured, &completed](const EmailExportResult& result) {
                         captured = result;
                         completed = true;
                     });
    QObject::connect(&worker,
                     &EmailExportWorker::errorOccurred,
                     &worker,
                     [&error_text](const QString& error) { error_text = error; });
    worker.exportMboxItems(&parser, config);

    if (!completed) {
        return {false,
                error_text.isEmpty() ? QStringLiteral("MBOX export did not complete") : error_text,
                {}};
    }
    return buildExportResult(captured, format, output_path, item_ids_capped);
}

QJsonObject stringProp(const QString& description) {
    return QJsonObject{{QStringLiteral("type"), QStringLiteral("string")},
                       {QStringLiteral("description"), description}};
}

QJsonObject exportMboxParamsSchema() {
    QJsonObject format_prop{{QStringLiteral("type"), QStringLiteral("string")},
                            {QStringLiteral("enum"),
                             QJsonArray{QStringLiteral("eml"),
                                        QStringLiteral("html"),
                                        QStringLiteral("text"),
                                        QStringLiteral("pdf")}},
                            {QStringLiteral("description"),
                             QStringLiteral("Output format per message (default eml)")}};
    QJsonObject ids_prop{
        {QStringLiteral("type"), QStringLiteral("array")},
        {QStringLiteral("items"), QJsonObject{{QStringLiteral("type"), QStringLiteral("integer")}}},
        {QStringLiteral("description"),
         QStringLiteral("Message indices to export; omit or empty to export all messages")}};
    QJsonObject properties{
        {QStringLiteral("path"),
         stringProp(QStringLiteral("Absolute path to the source MBOX file"))},
        {QStringLiteral("output_path"),
         stringProp(QStringLiteral("Directory to write exported message files into"))},
        {QStringLiteral("format"), format_prop},
        {QStringLiteral("item_ids"), ids_prop}};
    return QJsonObject{{QStringLiteral("type"), QStringLiteral("object")},
                       {QStringLiteral("properties"), properties},
                       {QStringLiteral("required"),
                        QJsonArray{QStringLiteral("path"), QStringLiteral("output_path")}},
                       {QStringLiteral("additionalProperties"), false}};
}

// ---------------------------------------------------------------------------
// organizer.organize_directory
// ---------------------------------------------------------------------------

// Convert the model-facing {category: [ext, ...]} object into the worker's
// QMap<category, extensions>. Non-string / empty extensions are dropped; a category
// with no usable extensions is dropped. Returns empty if nothing usable was given.
QMap<QString, QStringList> categoryMappingFromArgs(const QJsonObject& mapping) {
    QMap<QString, QStringList> result;
    for (auto it = mapping.begin(); it != mapping.end(); ++it) {
        if (it.key().isEmpty() || !it.value().isArray()) {
            continue;
        }
        QStringList extensions;
        for (const QJsonValue& ext : it.value().toArray()) {
            QString value = ext.toString().trimmed();
            if (value.startsWith(QLatin1Char('.'))) {
                value = value.mid(1);
            }
            if (!value.isEmpty()) {
                extensions.append(value);
            }
        }
        if (!extensions.isEmpty()) {
            result.insert(it.key(), extensions);
        }
    }
    return result;
}

// A category name becomes a SINGLE subdirectory component under the target
// (planMove: target_dir / category). Reject anything that is not a plain component
// so it cannot escape the target: a separator would make it multi-component or
// (with std::filesystem operator/) an absolute path that REPLACES the target; a
// colon is a Windows drive / alternate-data-stream; "." / ".." are traversal. This
// is the containment guard for a prompt-injected category_mapping.
bool isSafeCategoryName(const QString& name) {
    if (name == QLatin1String(".") || name == QLatin1String("..")) {
        return false;
    }
    for (const QChar ch : name) {
        if (ch == QLatin1Char('/') || ch == QLatin1Char('\\') || ch == QLatin1Char(':')) {
            return false;
        }
    }
    return true;
}

// Returns the first category key that is not a safe subdirectory name, or empty if
// all are safe.
QString firstUnsafeCategory(const QMap<QString, QStringList>& mapping) {
    for (auto it = mapping.begin(); it != mapping.end(); ++it) {
        if (!isSafeCategoryName(it.key())) {
            return it.key();
        }
    }
    return QString();
}

// Validate the target directory + collision strategy. Returns an error result to
// short-circuit on, or nullopt when the inputs are acceptable.
std::optional<AppActionResult> validateOrganizeInputs(const QString& target,
                                                      const QString& collision_strategy,
                                                      const QMap<QString, QStringList>& mapping) {
    if (target.isEmpty()) {
        return AppActionResult{false,
                               QStringLiteral("organize_directory requires a 'target_directory'"),
                               {}};
    }
    if (isNetworkOrDevicePath(target)) {
        return AppActionResult{false,
                               QStringLiteral(
                                   "organize_directory does not allow network/UNC or device paths"),
                               {}};
    }
    const QFileInfo info(target);
    if (!info.exists() || !info.isDir()) {
        return AppActionResult{
            false,
            QStringLiteral("target_directory is not an existing directory: %1").arg(target),
            {}};
    }
    if (mapping.isEmpty()) {
        return AppActionResult{false,
                               QStringLiteral(
                                   "organize_directory requires a non-empty 'category_mapping' of "
                                   "category -> file extensions"),
                               {}};
    }
    if (const QString unsafe = firstUnsafeCategory(mapping); !unsafe.isEmpty()) {
        return AppActionResult{
            false,
            QStringLiteral("category name '%1' is not a valid subfolder name (no path separators, "
                           "':', '.' or '..'): it must not escape the target directory")
                .arg(unsafe),
            {}};
    }
    // "overwrite" would let the move silently replace an existing file in the target
    // category folder (data loss). A headless run only ever relocates or renames, so
    // the destructive-but-recoverable classification stays honest.
    if (collision_strategy != QLatin1String("rename") &&
        collision_strategy != QLatin1String("skip")) {
        return AppActionResult{
            false,
            QStringLiteral("collision_strategy must be 'rename' or 'skip' (got '%1'); 'overwrite' "
                           "is not allowed for a headless organize")
                .arg(collision_strategy),
            {}};
    }
    return std::nullopt;
}

AppActionResult organizeDirectory(const QJsonObject& args) {
    const QString target = args.value(QStringLiteral("target_directory")).toString().trimmed();
    const QString collision_strategy =
        args.value(QStringLiteral("collision_strategy")).toString().trimmed().isEmpty()
            ? QStringLiteral("rename")
            : args.value(QStringLiteral("collision_strategy")).toString().trimmed().toLower();
    const QMap<QString, QStringList> mapping =
        categoryMappingFromArgs(args.value(QStringLiteral("category_mapping")).toObject());
    if (const std::optional<AppActionResult> error =
            validateOrganizeInputs(target, collision_strategy, mapping)) {
        return *error;
    }

    OrganizerWorker::Config config;
    config.target_directory = target;
    config.category_mapping = mapping;
    config.preview_mode = false;
    config.create_subdirectories = args.value(QStringLiteral("create_subdirectories")).toBool(true);
    config.collision_strategy = collision_strategy;

    // Drive the worker (a QThread) directly through the async->sync bridge, like the
    // read-only search. Declared before the invocation so it is destroyed LAST: on a
    // timeout its ~WorkerBase requests stop and joins the thread after run() returns,
    // so a still-running organize is reaped, never leaked into a destroyed context.
    OrganizerWorker worker(config);
    AsyncActionInvocation inv;
    QObject::connect(&worker, &WorkerBase::finished, inv.context(), [&inv, &worker, target]() {
        const int moved = worker.movedCount();
        QJsonObject data{{QStringLiteral("target_directory"), target},
                         {QStringLiteral("files_moved"), moved}};
        inv.finish(
            {true, QStringLiteral("Organized %1 (%2 file(s) moved)").arg(target).arg(moved), data});
    });
    QObject::connect(
        &worker, &WorkerBase::failed, inv.context(), [&inv](int, const QString& error) {
            inv.finish({false, error.isEmpty() ? QStringLiteral("Organize failed") : error, {}});
        });

    const AppActionResult result = inv.run([&worker]() { worker.start(); });
    worker.requestStop();  // cooperative stop before teardown (no-op if already finished)
    return result;
}

QJsonObject organizeParamsSchema() {
    QJsonObject mapping_prop{
        {QStringLiteral("type"), QStringLiteral("object")},
        {QStringLiteral("additionalProperties"),
         QJsonObject{{QStringLiteral("type"), QStringLiteral("array")},
                     {QStringLiteral("items"),
                      QJsonObject{{QStringLiteral("type"), QStringLiteral("string")}}}}},
        {QStringLiteral("description"),
         QStringLiteral("Map of category name -> list of file extensions (no dot); files are "
                        "moved into a same-named subfolder of the target")}};
    QJsonObject collision_prop{
        {QStringLiteral("type"), QStringLiteral("string")},
        {QStringLiteral("enum"), QJsonArray{QStringLiteral("rename"), QStringLiteral("skip")}},
        {QStringLiteral("description"),
         QStringLiteral("On name collision: 'rename' (append a counter) or 'skip' (leave in "
                        "place). Default rename. 'overwrite' is not allowed.")}};
    QJsonObject subdirs_prop{
        {QStringLiteral("type"), QStringLiteral("boolean")},
        {QStringLiteral("description"),
         QStringLiteral("Create the category subfolders if missing (default true)")}};
    QJsonObject properties{
        {QStringLiteral("target_directory"),
         stringProp(QStringLiteral("Absolute path to the directory whose top-level files to "
                                   "organize (not recursive)"))},
        {QStringLiteral("category_mapping"), mapping_prop},
        {QStringLiteral("collision_strategy"), collision_prop},
        {QStringLiteral("create_subdirectories"), subdirs_prop}};
    return QJsonObject{{QStringLiteral("type"), QStringLiteral("object")},
                       {QStringLiteral("properties"), properties},
                       {QStringLiteral("required"),
                        QJsonArray{QStringLiteral("target_directory"),
                                   QStringLiteral("category_mapping")}},
                       {QStringLiteral("additionalProperties"), false}};
}

// ---------------------------------------------------------------------------
// imaging.flash_image (CATASTROPHIC)
// ---------------------------------------------------------------------------

// Flashing a disk can legitimately run for many minutes; give the bridge a generous
// ceiling (the assistant tool layer applies its own outer timeout too).
constexpr int kFlashTimeoutMs = 60 * 60 * 1000;

QString flashTargetDescription(int disk_number, const PartitionDiskInfo& disk) {
    const QString model = disk.model.isEmpty() ? QStringLiteral("unknown model") : disk.model;
    const QString bus = disk.bus_type.isEmpty() ? QStringLiteral("?") : disk.bus_type;
    const QString removable = disk.is_removable ? QStringLiteral(", removable") : QString();
    return QStringLiteral("disk %1: %2 (%3, %4 bytes%5)")
        .arg(disk_number)
        .arg(model, bus)
        .arg(static_cast<double>(disk.size_bytes))
        .arg(removable);
}

// Leading letter of the running OS drive (e.g. "C"). Read from %SystemDrive%; this
// is a Get-Disk-INDEPENDENT signal, so it still identifies the OS disk even if
// Get-Disk's IsSystem/IsBoot came back null (coerced to false).
QString systemDriveLetter() {
    const QString drive = qEnvironmentVariable("SystemDrive");  // "C:" on a normal install
    return drive.isEmpty() ? QStringLiteral("C") : drive.left(1).toUpper();
}

// True if any partition on @p disk is an OS boot/system/EFI partition, or hosts the
// running system volume (%SystemDrive%). Uses per-partition data + drive letters,
// which do not all silently degrade to "safe" the way a single disk-level bool can.
bool diskHostsSystemVolume(const PartitionDiskInfo& disk, const QString& system_letter) {
    for (const PartitionInfoEx& part : disk.partitions) {
        if (part.is_boot || part.is_system || part.is_efi) {
            return true;
        }
        if (part.hasVolume() && part.volume->hasDriveLetter() &&
            part.volume->drive_letter.left(1).toUpper() == system_letter) {
            return true;
        }
    }
    return false;
}

// Reason a disk must NOT be flashed, or empty if it is a safe target. Fail CLOSED
// with several INDEPENDENT signals so no single null-coerced Get-Disk flag can
// expose the OS disk, and refuse whole-pool/mirror members (dynamic / Storage
// Spaces) whose loss cascades beyond the one disk.
QString unsafeFlashReason(const PartitionDiskInfo& disk, const QString& system_letter) {
    if (disk.is_system || disk.is_boot) {
        return QStringLiteral("it is the system/boot disk");
    }
    if (disk.is_dynamic || disk.is_storage_spaces) {
        return QStringLiteral(
            "it is a dynamic / Storage Spaces disk (flashing it would destroy "
            "the whole volume set or pool)");
    }
    if (disk.is_read_only) {
        return QStringLiteral("it is read-only/write-protected");
    }
    if (diskHostsSystemVolume(disk, system_letter)) {
        return QStringLiteral("it hosts a system/boot/EFI partition or the running OS volume");
    }
    return QString();
}

QJsonObject serializeFlashResult(const sak::FlashResult& result, const QString& device_path) {
    QJsonArray errors;
    for (const QString& error : result.errorMessages) {
        errors.append(error);
    }
    return QJsonObject{
        {QStringLiteral("device_path"), device_path},
        {QStringLiteral("bytes_written"), static_cast<double>(result.bytesWritten)},
        {QStringLiteral("elapsed_seconds"), result.elapsedSeconds},
        {QStringLiteral("source_checksum"), result.sourceChecksum},
        {QStringLiteral("successful_drives"), QJsonArray::fromStringList(result.successfulDrives)},
        {QStringLiteral("failed_drives"), QJsonArray::fromStringList(result.failedDrives)},
        {QStringLiteral("errors"), errors}};
}

// Run the flash to completion synchronously via the async->sync bridge. The
// coordinator is declared before the invocation so it is destroyed LAST (its dtor
// cancels + joins the flash workers); on timeout the invocation returns and cancel()
// stops the in-flight write.
AppActionResult runFlash(const QString& image_path, const FlashTargetResolution& target) {
    FlashCoordinator coordinator;
    AsyncActionInvocation inv(kFlashTimeoutMs);
    const QString device_path = target.device_path;
    QObject::connect(&coordinator,
                     &FlashCoordinator::flashCompleted,
                     inv.context(),
                     [&inv, device_path](const sak::FlashResult& result) {
                         const bool ok = result.success && !result.hasErrors();
                         const QString message =
                             ok ? QStringLiteral("Flashed image to %1").arg(device_path)
                                : QStringLiteral("Flash to %1 failed").arg(device_path);
                         inv.finish({ok, message, serializeFlashResult(result, device_path)});
                     });
    QObject::connect(&coordinator,
                     &FlashCoordinator::flashError,
                     inv.context(),
                     [&inv](const QString& error) { inv.finish({false, error, {}}); });

    const AppActionResult result = inv.run([&coordinator, &inv, image_path, device_path]() {
        // startFlash emits flashError synchronously (same-thread, direct) on an early
        // failure, which finishes the invocation; only synthesize an error if it
        // returned false without doing so.
        if (!coordinator.startFlash(image_path, QStringList{device_path}) && !inv.isDone()) {
            inv.finish({false, QStringLiteral("Flash failed to start"), {}});
        }
    });
    coordinator.cancel();  // stop any in-flight write before teardown (no-op if done)
    return result;
}

AppActionResult flashImage(const QJsonObject& args) {
    const QString image_path = args.value(QStringLiteral("image_path")).toString().trimmed();
    if (image_path.isEmpty() || !args.value(QStringLiteral("disk_number")).isDouble()) {
        return {false,
                QStringLiteral("flash_image requires 'image_path' and a numeric 'disk_number'"),
                {}};
    }
    if (isNetworkOrDevicePath(image_path)) {
        return {false, QStringLiteral("flash_image does not allow a network/UNC image path"), {}};
    }
    const QFileInfo info(image_path);
    if (!info.isFile()) {
        return {false, QStringLiteral("No such image file: %1").arg(image_path), {}};
    }
    const int disk_number = args.value(QStringLiteral("disk_number")).toInt(-1);

    // Resolve + SAFETY-validate the target against the authoritative synchronous
    // inventory (no elevation) BEFORE touching any device. The gate has already shown
    // the disk to the human and forced a confirm; this refuses the OS/read-only disk.
    const PartitionInventory inventory = StorageInventoryWorker::scanCurrentSystem(false);
    const FlashTargetResolution target = resolveFlashTarget(inventory, disk_number);
    if (!target.ok) {
        return target.error;
    }
    return runFlash(image_path, target);
}

QJsonObject flashParamsSchema() {
    QJsonObject disk_prop{
        {QStringLiteral("type"), QStringLiteral("integer")},
        {QStringLiteral("description"),
         QStringLiteral("Physical disk number to OVERWRITE (from partition.list_inventory). The "
                        "system/boot disk and read-only disks are always refused.")}};
    QJsonObject properties{
        {QStringLiteral("image_path"),
         stringProp(QStringLiteral("Absolute path to the local disk-image file to write"))},
        {QStringLiteral("disk_number"), disk_prop}};
    return QJsonObject{{QStringLiteral("type"), QStringLiteral("object")},
                       {QStringLiteral("properties"), properties},
                       {QStringLiteral("required"),
                        QJsonArray{QStringLiteral("image_path"), QStringLiteral("disk_number")}},
                       {QStringLiteral("additionalProperties"), false}};
}

// Base descriptor for a mutating op: read_only=false, mutating=true, and both
// destructive and requires_admin default false. A caller sets params_schema and,
// for a destructive or admin op, flips those flags on the returned descriptor.
AppActionDescriptor mutatingDescriptor(const QString& id,
                                       const QString& title,
                                       const QString& description,
                                       const QString& category) {
    AppActionDescriptor descriptor;
    descriptor.id = id;
    descriptor.title = title;
    descriptor.description = description;
    descriptor.category = category;
    descriptor.read_only = false;
    descriptor.mutating = true;
    descriptor.destructive = false;
    descriptor.requires_admin = false;
    return descriptor;
}

// ---------------------------------------------------------------------------
// partition.apply_operation: apply ONE partition-layout operation to a disk.
// ---------------------------------------------------------------------------

// A partition op can legitimately run for up to the executor's own per-op ceiling
// (kPartitionLongTaskTimeoutSeconds == 3600s). Set the outer bridge timeout just above
// that so it never preempts a legitimate long op -- the executor's own timeout ends the
// op first; this only fires if the executor itself wedges.
constexpr int kApplyTimeoutMs = 65 * 60 * 1000;

QJsonObject serializeApplyStep(const PartitionExecutionStep& step) {
    QJsonObject obj{{QStringLiteral("summary"), step.summary},
                    {QStringLiteral("success"), step.success},
                    {QStringLiteral("skipped"), step.skipped}};
    if (!step.error_message.isEmpty()) {
        obj.insert(QStringLiteral("error"), step.error_message);
    }
    // A dry run performs no I/O; it puts the script it WOULD run into stdout_text.
    // Surface that (capped) so the model can inspect the plan.
    if (step.skipped && !step.stdout_text.isEmpty()) {
        QString preview = step.stdout_text;
        if (preview.size() > kPartitionReportOutputPreviewChars) {
            preview = preview.left(kPartitionReportOutputPreviewChars) + QStringLiteral("...");
        }
        obj.insert(QStringLiteral("script_preview"), preview);
    }
    return obj;
}

QJsonObject serializeApplyResult(const PartitionExecutionResult& result) {
    QJsonArray steps;
    for (const PartitionExecutionStep& step : result.steps) {
        steps.append(serializeApplyStep(step));
    }
    return QJsonObject{{QStringLiteral("success"), result.success},
                       {QStringLiteral("dry_run"), result.dry_run},
                       {QStringLiteral("cancelled"), result.cancelled},
                       {QStringLiteral("message"), result.message},
                       {QStringLiteral("steps"), steps}};
}

// Validate the numeric args + require confirm_layout_hash. Returns an error result to
// return verbatim, or nullopt when everything is well-formed. Split out to keep
// applyPartitionOperation within the cyclomatic-complexity budget.
std::optional<AppActionResult> nonNegativeByteArgsError(const QJsonObject& args) {
    for (const char* key : {"offset_bytes", "size_bytes"}) {
        const QString name = QString::fromLatin1(key);
        if (args.contains(name) && args.value(name).toDouble(0.0) < 0.0) {
            return AppActionResult{false,
                                   QStringLiteral("%1 must be a non-negative number").arg(name),
                                   {}};
        }
    }
    return std::nullopt;
}

std::optional<AppActionResult> validateApplyArgs(const QJsonObject& args) {
    if (!args.value(QStringLiteral("disk_number")).isDouble()) {
        return AppActionResult{false,
                               QStringLiteral("apply_operation requires a numeric 'disk_number'"),
                               {}};
    }
    if (args.value(QStringLiteral("disk_number")).toInt(-1) < 0) {
        return AppActionResult{false,
                               QStringLiteral("disk_number must be a non-negative integer"),
                               {}};
    }
    if (args.contains(QStringLiteral("partition_number")) &&
        args.value(QStringLiteral("partition_number")).toInt(-1) < 0) {
        return AppActionResult{false,
                               QStringLiteral("partition_number must be a non-negative integer"),
                               {}};
    }
    if (std::optional<AppActionResult> byte_error = nonNegativeByteArgsError(args)) {
        return byte_error;
    }
    if (args.value(QStringLiteral("confirm_layout_hash")).toString().trimmed().isEmpty()) {
        return AppActionResult{
            false,
            QStringLiteral("apply_operation requires 'confirm_layout_hash' (the layout_hash from a "
                           "prior list_inventory/preview_operation, to detect layout drift)"),
            {}};
    }
    // dry_run is the single most safety-relevant argument, so it must be a real
    // boolean -- never coerced. A present-but-non-boolean value (e.g. the string
    // "true") would otherwise silently read as false via toBool(false).
    if (args.contains(QStringLiteral("dry_run")) &&
        !args.value(QStringLiteral("dry_run")).isBool()) {
        return AppActionResult{false,
                               QStringLiteral("dry_run must be a boolean (true or false)"),
                               {}};
    }
    return std::nullopt;
}

AppActionResult applyResultFromExecution(const PartitionExecutionResult& result,
                                         PartitionOperationType type,
                                         const QString& description,
                                         bool dry_run) {
    const QString verb = dry_run ? QStringLiteral("Dry-run") : QStringLiteral("Apply");
    const QString message = QStringLiteral("%1 of %2 on %3: %4")
                                .arg(verb)
                                .arg(toDisplayString(type))
                                .arg(description)
                                .arg(result.success ? QStringLiteral("OK")
                                                    : QStringLiteral("FAILED"));
    return {result.success, message, serializeApplyResult(result)};
}

// Validate the op against the safety validator, then run it on a WORKER THREAD via the
// AsyncActionInvocation bridge (the certified flash-path lifetime). Running the blocking,
// possibly-ELEVATED PartitionExecutor off the tool worker thread keeps it cancellable
// (worker.cancelExecution -> PartitionExecutor::cancel) and bounds teardown (the
// PartitionApplyWorker destructor cooperatively cancels then wait/terminate-joins). A dry
// run performs no I/O and never elevates; use_elevation is true only for a real apply.
AppActionResult runApplyOperation(const ParsedPartitionOp& parsed,
                                  const QJsonObject& args,
                                  const PartitionApplyResolution& target,
                                  const PartitionInventory& inventory,
                                  bool dry_run) {
    const int disk_number = args.value(QStringLiteral("disk_number")).toInt(-1);
    const PartitionTarget pt =
        buildPartitionOpTarget(args, static_cast<uint32_t>(disk_number), parsed.kind);
    const QJsonObject payload = args.value(QStringLiteral("payload")).toObject();
    const PartitionOperation op =
        PartitionOperationPlanner::makeOperation(parsed.type, pt, payload);

    // Independent of the OS-disk guard: refuse an op the safety validator blocks (the
    // same rules preview_operation reports), so a knowingly-invalid or
    // protected-partition op never reaches the executor.
    PartitionSafetyValidator validator;
    const PartitionValidationResult validation = validator.validate(inventory, op);
    if (!validation.allowed()) {
        return {false,
                QStringLiteral("Operation is blocked by the safety validator: %1")
                    .arg(validation.blockers.join(QStringLiteral("; "))),
                {}};
    }

    // worker declared BEFORE inv so it is destroyed AFTER inv: by the time the worker
    // destructor joins the thread, inv.context() is already gone, so a late finished()
    // is dropped rather than delivered to a dangling bridge.
    const PartitionOperationType type = parsed.type;
    const QString description = target.description;
    PartitionApplyWorker worker(QVector<PartitionOperation>{op},
                                dry_run,
                                /*use_elevation=*/!dry_run);
    AsyncActionInvocation inv(kApplyTimeoutMs);
    QObject::connect(&worker,
                     &WorkerBase::finished,
                     inv.context(),
                     [&inv, &worker, type, description, dry_run]() {
                         inv.finish(
                             applyResultFromExecution(worker.result(), type, description, dry_run));
                     });
    QObject::connect(&worker,
                     &WorkerBase::failed,
                     inv.context(),
                     [&inv](int, const QString& error) { inv.finish({false, error, {}}); });
    QObject::connect(&worker, &WorkerBase::cancelled, inv.context(), [&inv]() {
        inv.finish({false, QStringLiteral("Partition apply was cancelled"), {}});
    });

    // worker.start() (QThread::start) returns void; an OS thread-creation failure only logs
    // and never runs execute(), so no signal fires and inv.run() falls back to its timeout.
    // Accepted (matches the organizer/search worker path): a launch failure needs thread
    // exhaustion and merely delays the failure result rather than misreporting it.
    const AppActionResult result = inv.run([&worker]() { worker.start(); });
    worker.cancelExecution();  // cooperative cancel on timeout/teardown before the dtor joins
    return result;
}

AppActionResult applyPartitionOperation(const QJsonObject& args) {
    const QString type_name = args.value(QStringLiteral("operation")).toString().trimmed();
    const std::optional<ParsedPartitionOp> parsed = parsePartitionOpType(type_name);
    if (!parsed) {
        return {false,
                QStringLiteral("Unsupported or missing operation '%1'. Supported: %2")
                    .arg(type_name, supportedPartitionOpTypes()),
                {}};
    }
    if (const std::optional<AppActionResult> error = validateApplyArgs(args)) {
        return *error;
    }
    const bool dry_run = args.value(QStringLiteral("dry_run")).toBool(false);
    const int disk_number = args.value(QStringLiteral("disk_number")).toInt(-1);
    const QString confirm_hash =
        args.value(QStringLiteral("confirm_layout_hash")).toString().trimmed();

    // Authoritative synchronous inventory (no elevation). The guard refuses a degraded
    // scan, a drifted layout, a missing disk, and the OS/dynamic/read-only disk BEFORE
    // anything is written. The catastrophic gate has already forced a human confirm.
    const PartitionInventory inventory = StorageInventoryWorker::scanCurrentSystem(false);
    const PartitionApplyResolution target =
        resolvePartitionApplyTarget(inventory, disk_number, confirm_hash);
    if (!target.ok) {
        return target.error;
    }
    return runApplyOperation(*parsed, args, target, inventory, dry_run);
}

QJsonObject applyParamsSchema() {
    const auto intProp = [](const QString& description) {
        return QJsonObject{{QStringLiteral("type"), QStringLiteral("integer")},
                           {QStringLiteral("description"), description}};
    };
    QJsonObject payload_prop{
        {QStringLiteral("type"), QStringLiteral("object")},
        {QStringLiteral("description"),
         QStringLiteral("Operation-specific fields (e.g. file_system, label, target_size_bytes)")},
        {QStringLiteral("additionalProperties"), true}};
    QJsonObject dry_prop{
        {QStringLiteral("type"), QStringLiteral("boolean")},
        {QStringLiteral("description"),
         QStringLiteral("If true, return the exact script this operation WOULD run WITHOUT "
                        "elevating or writing (plan only). If false (default), apply it for real "
                        "(elevated, ERASES data) once the human confirm is granted.")}};
    QJsonObject properties{
        {QStringLiteral("operation"),
         stringProp(QStringLiteral("Operation to apply (one of: ") + supportedPartitionOpTypes() +
                    QStringLiteral(")"))},
        {QStringLiteral("disk_number"),
         intProp(
             QStringLiteral("Target disk number (from list_inventory). The OS/system, "
                            "dynamic/Storage-Spaces, and read-only disks are always refused."))},
        {QStringLiteral("partition_number"),
         intProp(QStringLiteral("Target partition number (1-based) for a partition-scoped op"))},
        {QStringLiteral("offset_bytes"),
         intProp(QStringLiteral("Byte offset of the unallocated region for a create"))},
        {QStringLiteral("size_bytes"), intProp(QStringLiteral("Size in bytes where applicable"))},
        {QStringLiteral("payload"), payload_prop},
        {QStringLiteral("confirm_layout_hash"),
         stringProp(QStringLiteral("The layout_hash observed from a prior list_inventory or "
                                   "preview_operation; the apply is REFUSED if the disk layout "
                                   "changed since (drift guard)"))},
        {QStringLiteral("dry_run"), dry_prop}};
    return QJsonObject{{QStringLiteral("type"), QStringLiteral("object")},
                       {QStringLiteral("properties"), properties},
                       {QStringLiteral("required"),
                        QJsonArray{QStringLiteral("operation"),
                                   QStringLiteral("disk_number"),
                                   QStringLiteral("confirm_layout_hash")}},
                       {QStringLiteral("additionalProperties"), false}};
}

}  // namespace

FlashTargetResolution resolveFlashTarget(const PartitionInventory& inventory, int disk_number) {
    const auto refuse = [](const QString& message) {
        FlashTargetResolution resolution;
        resolution.ok = false;
        resolution.error = {false, message, {}};
        return resolution;
    };
    if (disk_number < 0) {
        return refuse(QStringLiteral("flash_image requires a non-negative disk_number"));
    }
    // A degraded/partial inventory scan cannot be trusted to have populated the safety
    // flags (Get-Disk IsSystem/IsBoot coerce null -> false), so refuse rather than risk
    // flashing the OS disk on a scan that half-failed.
    if (!inventory.warnings.isEmpty()) {
        return refuse(
            QStringLiteral("Refusing to flash: the disk inventory scan reported "
                           "warnings and cannot be trusted to identify the system disk"));
    }
    const PartitionDiskInfo* target = nullptr;
    for (const PartitionDiskInfo& disk : inventory.disks) {
        if (static_cast<int>(disk.disk_number) == disk_number) {
            target = &disk;
            break;
        }
    }
    if (target == nullptr) {
        return refuse(QStringLiteral("No physical disk with number %1 was found").arg(disk_number));
    }
    if (const QString reason = unsafeFlashReason(*target, systemDriveLetter()); !reason.isEmpty()) {
        return refuse(QStringLiteral("Refusing to flash disk %1 (%2): %3")
                          .arg(disk_number)
                          .arg(target->model, reason));
    }
    FlashTargetResolution resolution;
    resolution.ok = true;
    resolution.device_path = QStringLiteral("\\\\.\\PhysicalDrive%1").arg(disk_number);
    resolution.description = flashTargetDescription(disk_number, *target);
    return resolution;
}

PartitionApplyResolution resolvePartitionApplyTarget(const PartitionInventory& inventory,
                                                     int disk_number,
                                                     const QString& confirm_layout_hash) {
    const auto refuse = [](const QString& message) {
        PartitionApplyResolution resolution;
        resolution.ok = false;
        resolution.error = {false, message, {}};
        return resolution;
    };
    if (disk_number < 0) {
        return refuse(QStringLiteral("apply_operation requires a non-negative disk_number"));
    }
    // A degraded/partial scan cannot be trusted to have set the safety flags (Get-Disk
    // IsSystem/IsBoot coerce null -> false), so refuse rather than risk the OS disk.
    if (!inventory.warnings.isEmpty()) {
        return refuse(
            QStringLiteral("Refusing to apply: the disk inventory scan reported warnings and "
                           "cannot be trusted to identify the system disk"));
    }
    // Drift guard (mirrors PartitionOperationQueue::canApply): the layout the model
    // reasoned about must still be the current one, or a stale disk_number could target
    // a different disk than was previewed.
    if (confirm_layout_hash != inventory.layout_hash) {
        return refuse(
            QStringLiteral("Refusing to apply: the disk layout changed since it was previewed "
                           "(confirm_layout_hash does not match the current inventory); re-run "
                           "preview_operation and retry"));
    }
    const PartitionDiskInfo* target = nullptr;
    for (const PartitionDiskInfo& disk : inventory.disks) {
        if (static_cast<int>(disk.disk_number) == disk_number) {
            target = &disk;
            break;
        }
    }
    if (target == nullptr) {
        return refuse(QStringLiteral("No physical disk with number %1 was found").arg(disk_number));
    }
    // Reuse the flash OS-disk denylist: refuse the system/boot disk, dynamic / Storage
    // Spaces members, read-only disks, and any disk hosting a system/boot/EFI partition
    // or the running OS volume. Envelope: any non-system disk (use the GUI for the OS disk).
    if (const QString reason = unsafeFlashReason(*target, systemDriveLetter()); !reason.isEmpty()) {
        return refuse(QStringLiteral("Refusing to apply to disk %1 (%2): %3")
                          .arg(disk_number)
                          .arg(target->model, reason));
    }
    PartitionApplyResolution resolution;
    resolution.ok = true;
    resolution.device_path = QStringLiteral("\\\\.\\PhysicalDrive%1").arg(disk_number);
    resolution.description = flashTargetDescription(disk_number, *target);
    return resolution;
}

int registerMutatingAppActionsInto(AppActionRegistry& registry) {
    int registered = 0;
    const auto add = [&](const AppActionDescriptor& descriptor, AppActionInvoke invoke) {
        if (registry.registerAction(descriptor, std::move(invoke))) {
            ++registered;
        }
    };

    // email.export_mbox: adds files under output_path, never overwrites/deletes, no
    // elevation -> mutating but not destructive and not admin.
    AppActionDescriptor export_mbox = mutatingDescriptor(
        QStringLiteral("email.export_mbox"),
        QStringLiteral("Export MBOX messages"),
        QStringLiteral("Export messages from an MBOX file to eml/html/text/pdf files"),
        QStringLiteral("email"));
    export_mbox.params_schema = exportMboxParamsSchema();
    add(export_mbox, exportMbox);

    // organizer.organize_directory: relocates existing user files into category
    // subfolders (a bulk, not-trivially-undoable filesystem change) -> destructive.
    // "overwrite" is refused so it never replaces a file; no elevation needed.
    AppActionDescriptor organize = mutatingDescriptor(
        QStringLiteral("organizer.organize_directory"),
        QStringLiteral("Organize a directory"),
        QStringLiteral("Move a directory's top-level files into category subfolders by extension"),
        QStringLiteral("organizer"));
    organize.params_schema = organizeParamsSchema();
    organize.destructive = true;
    add(organize, organizeDirectory);

    // imaging.flash_image: OVERWRITES an entire physical disk with a disk image ->
    // destructive + CATASTROPHIC (forces a human confirm even in Unattended) +
    // requires_admin (raw disk write). The system/boot disk and read-only disks are
    // refused by resolveFlashTarget before any device is touched.
    AppActionDescriptor flash = mutatingDescriptor(
        QStringLiteral("imaging.flash_image"),
        QStringLiteral("Flash a disk image"),
        QStringLiteral("Write a disk image to a physical drive, ERASING all data on it"),
        QStringLiteral("imaging"));
    flash.params_schema = flashParamsSchema();
    flash.destructive = true;
    flash.catastrophic = true;
    flash.requires_admin = true;
    add(flash, flashImage);

    // partition.apply_operation: applies ONE partition-layout op (diskpart/format/etc.) to
    // a disk -> destructive + CATASTROPHIC (forces a human confirm even in Unattended) +
    // requires_admin (raw disk write). resolvePartitionApplyTarget refuses the
    // OS/dynamic/read-only disk + a drifted/degraded scan; the safety validator + script
    // builder + the generated script's own IsSystem/IsBoot guard are further independent
    // layers. The op runs on a PartitionApplyWorker thread (cancellable, bounded teardown);
    // dry_run=true returns the script without elevating or writing.
    AppActionDescriptor apply = mutatingDescriptor(
        QStringLiteral("partition.apply_operation"),
        QStringLiteral("Apply a partition operation"),
        QStringLiteral("Apply one disk/partition-layout operation (create/delete/format/resize/"
                       "wipe/...) to a NON-system disk; ERASES data. Refused on the OS disk. Pass "
                       "dry_run=true to get the exact script without running it."),
        QStringLiteral("partition"));
    apply.params_schema = applyParamsSchema();
    apply.destructive = true;
    apply.catastrophic = true;
    apply.requires_admin = true;
    add(apply, applyPartitionOperation);

    return registered;
}

}  // namespace sak
