// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

// Mutating technician ops exposed to the AI assistant. Each invoke thunk calls an
// existing headless src/core service (no re-implementation) and serializes its
// result to a compact, model-facing QJsonObject. Every op is marked mutating (and
// destructive / requires_admin where that applies) so the panel's per-action gate
// is enforced -- these thunks never gate themselves and never pop a dialog.

#include "sak/app_mutating_actions.h"

#include "sak/advanced_uninstall_types.h"
#include "sak/app_action_guards.h"
#include "sak/app_action_registry.h"
#include "sak/app_action_service.h"
#include "sak/app_organizer_helpers.h"
#include "sak/app_partition_op_parse.h"
#include "sak/cleanup_worker.h"
#include "sak/dns_diagnostic_tool.h"
#include "sak/email_attachment_saver.h"
#include "sak/email_export_worker.h"
#include "sak/email_types.h"
#include "sak/ethernet_config_manager.h"
#include "sak/file_explorer_archive_service.h"
#include "sak/file_recovery_engine.h"
#include "sak/flash_coordinator.h"
#include "sak/layout_constants.h"
#include "sak/leftover_cleanup_item_guard.h"
#include "sak/leftover_scan_provenance.h"
#include "sak/logger.h"
#include "sak/mbox_parser.h"
#include "sak/organizer_worker.h"
#include "sak/ost_conversion_worker.h"
#include "sak/ost_converter_types.h"
#include "sak/partition_apply_worker.h"
#include "sak/partition_executor.h"
#include "sak/partition_manager_types.h"
#include "sak/partition_operation_planner.h"
#include "sak/partition_safety_validator.h"
#include "sak/program_enumerator.h"
#include "sak/pst_parser.h"
#include "sak/recycle_bin.h"
#include "sak/restore_point_manager.h"
#include "sak/storage_inventory_worker.h"
#include "sak/uninstall_worker.h"
#include "sak/wifi_setup_script.h"
#include "sak/worker_base.h"

#include <QAbstractSocket>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QMap>
#include <QRegularExpression>
#include <QSet>
#include <QString>
#include <QStringList>

#include <algorithm>
#include <cmath>
#include <functional>
#include <optional>

#ifdef Q_OS_WIN
#include <string>

#include <windows.h>
#endif

namespace sak {

namespace {

// Bound how many messages one headless export may write, so a model-supplied (or
// prompt-injected) call cannot spray hundreds of thousands of files onto disk in a
// single gated action. Larger exports use the GUI panel. Truncation is reported.
constexpr int kMaxExportItems = 5000;

// Bound the PST/OST size a headless export will open. PstParser::open() parses ALL node/block
// metadata synchronously up front, so a multi-GB store would spend minutes and a lot of memory
// before a single message is written; larger stores use the GUI. (Matches email.read_pst's cap.)
constexpr qint64 kMaxPstExportBytes = 2LL * 1024 * 1024 * 1024;

// Map the model-facing format string to the MBOX-capable ExportFormat set. MBOX
// export only supports per-message file formats (eml/html/text/pdf); the CSV / VCF /
// ICS formats are PST-only, so they are intentionally not offered here.
// Returns nullopt for an unrecognized format so the op fails CLOSED (never silently downgrades a
// mistyped format to EML). An omitted/empty format is the documented default (EML).
std::optional<ExportFormat> exportFormatFromArg(const QString& value) {
    const QString v = value.trimmed().toLower();
    if (v.isEmpty() || v == QLatin1String("eml")) {
        return ExportFormat::Eml;
    }
    if (v == QLatin1String("html")) {
        return ExportFormat::Html;
    }
    if (v == QLatin1String("text") || v == QLatin1String("txt")) {
        return ExportFormat::Text;
    }
    if (v == QLatin1String("pdf")) {
        return ExportFormat::Pdf;
    }
    return std::nullopt;
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

// Read the optional item_ids array (MBOX message indices) into @p out. An OMITTED item_ids => whole
// mailbox (documented). Fails CLOSED: a wrong-typed item_ids, or ANY entry that is not a
// non-negative whole number, is REFUSED rather than silently dropped -- otherwise an all-invalid
// array would collapse to an empty set and silently widen the export to the WHOLE mailbox. The
// count is capped (reported via @p capped) so the export cannot write an unbounded number of files.
std::optional<AppActionResult> exportItemIdsFromArgs(const QJsonObject& args,
                                                     QVector<uint64_t>& out,
                                                     bool& capped) {
    capped = false;
    if (!args.contains(QStringLiteral("item_ids"))) {
        return std::nullopt;
    }
    const QJsonValue raw_val = args.value(QStringLiteral("item_ids"));
    if (!raw_val.isArray()) {
        return AppActionResult{false,
                               QStringLiteral("item_ids must be an array of message indices"),
                               {}};
    }
    for (const QJsonValue& value : raw_val.toArray()) {
        if (out.size() >= kMaxExportItems) {
            capped = true;
            break;
        }
        const double index = value.isDouble() ? value.toDouble() : -1.0;
        if (!(index >= 0.0) || std::isinf(index) || std::floor(index) != index) {
            return AppActionResult{
                false, QStringLiteral("item_ids entries must be non-negative whole numbers"), {}};
        }
        out.append(static_cast<uint64_t>(index));
    }
    return std::nullopt;
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

// Validate the source/destination paths BEFORE opening anything. Returns an error result to
// short-circuit on, or nullopt when both paths are acceptable. Shared by export_mbox and export_pst
// (op_name/file_kind/max_bytes vary; the security-critical guards must NOT be duplicated or they
// could drift). Guards BOTH paths against UNC/device forms: the source must not open an SMB/device
// path (credential leak) and the destination must not write onto a network/device path.
// Field/verb labels so the shared path validators emit the CALLER's schema field names -- a model
// reading the error must be able to correct the RIGHT argument (extract_zip uses archive_path /
// destination_dir, not path / output_path). Defaults match the email export ops so their messages
// stay byte-identical.
struct ExportPathLabels {
    QString op_name;    // e.g. "export_mbox" / "extract_zip"
    QString file_kind;  // e.g. "MBOX" / "archive"
    QString source_field{QStringLiteral("path")};
    QString dest_field{QStringLiteral("output_path")};
    QString action_noun{QStringLiteral("export")};  // e.g. "export" / "extraction"
};

// Require a NEW or EMPTY output directory (returns a failure result, else nullopt). This is what
// keeps a "writes into a directory" op non-destructive: writers open WriteOnly / QSaveFile with no
// exists-check, so writing into a directory that already holds a same-named file would silently
// overwrite it. Into an empty/new directory nothing pre-existing can be clobbered, so the op only
// ever ADDS files -- hence destructive=false stays honest. Refuses a reparse-point destination
// (a symlink/junction target could be off-box or redirect the writes). Shared by email export
// (validateExportPaths) and files.extract_zip.
std::optional<AppActionResult> requireNewOrEmptyDir(const QString& output_path,
                                                    const QString& dest_field) {
    const QFileInfo out_info(output_path);
    if (pathReparseUnsafe(output_path)) {
        return AppActionResult{false,
                               QStringLiteral(
                                   "%1 must not be a symlink or junction (or under one): %2")
                                   .arg(dest_field, output_path),
                               {}};
    }
    if (out_info.exists()) {
        if (!out_info.isDir()) {
            return AppActionResult{
                false,
                QStringLiteral("%1 exists and is not a directory: %2").arg(dest_field, output_path),
                {}};
        }
        if (!QDir(output_path).isEmpty()) {
            return AppActionResult{
                false,
                QStringLiteral(
                    "%1 must be a new or empty directory (refusing to write "
                    "into a non-empty directory to avoid overwriting existing files): %2")
                    .arg(dest_field, output_path),
                {}};
        }
    }
    return std::nullopt;
}

std::optional<AppActionResult> validateExportPaths(const QString& path,
                                                   const QString& output_path,
                                                   qint64 max_bytes,
                                                   const ExportPathLabels& labels) {
    if (path.isEmpty() || output_path.isEmpty()) {
        return AppActionResult{false,
                               QStringLiteral("%1 requires '%2' and '%3'")
                                   .arg(labels.op_name, labels.source_field, labels.dest_field),
                               {}};
    }
    if (isNetworkOrDevicePath(path) || isNetworkOrDevicePath(output_path)) {
        return AppActionResult{
            false,
            QStringLiteral("%1 does not allow network/UNC or device paths").arg(labels.op_name),
            {}};
    }
    const QFileInfo info(path);
    if (pathReparseUnsafe(path)) {
        return AppActionResult{false,
                               QStringLiteral(
                                   "%1 must not be a symlink or junction (or under one): "
                                   "%2")
                                   .arg(labels.source_field, path),
                               {}};
    }
    if (!info.isFile()) {
        return AppActionResult{false,
                               QStringLiteral("No such %1 file: %2").arg(labels.file_kind, path),
                               {}};
    }
    if (info.size() > max_bytes) {
        return AppActionResult{false,
                               QStringLiteral(
                                   "%1 file is too large for a headless %2 (%3 bytes > %4 limit)")
                                   .arg(labels.file_kind, labels.action_noun)
                                   .arg(info.size())
                                   .arg(max_bytes),
                               {}};
    }
    return requireNewOrEmptyDir(output_path, labels.dest_field);
}

// Map a completed export into the tool result. Success = something was written, OR
// a clean run with nothing to export; failure only when nothing was exported AND
// the engine reported an error/failure.
AppActionResult buildExportResult(const EmailExportResult& captured,
                                  ExportFormat format,
                                  const QString& output_path,
                                  bool item_ids_capped,
                                  const QString& source_label) {
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
        message = QStringLiteral("%1 export failed (%2 item(s) failed)")
                      .arg(source_label)
                      .arg(captured.items_failed);
    } else {
        message = captured.errors.first();
    }
    return {ok, message, serializeExportResult(captured, item_ids_capped)};
}

AppActionResult exportMbox(const QJsonObject& args) {
    const QString path = args.value(QStringLiteral("path")).toString().trimmed();
    const QString output_path = args.value(QStringLiteral("output_path")).toString().trimmed();
    if (const std::optional<AppActionResult> error = validateExportPaths(
            path,
            output_path,
            kMaxMboxBytes,
            ExportPathLabels{QStringLiteral("export_mbox"), QStringLiteral("MBOX")})) {
        return *error;
    }

    const std::optional<ExportFormat> format =
        exportFormatFromArg(args.value(QStringLiteral("format")).toString());
    if (!format) {
        return {false, QStringLiteral("format must be one of eml/html/text/pdf"), {}};
    }
    bool item_ids_capped = false;
    QVector<uint64_t> item_ids;
    if (const std::optional<AppActionResult> error =
            exportItemIdsFromArgs(args, item_ids, item_ids_capped)) {
        return *error;
    }

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
    config.format = *format;
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
    return buildExportResult(
        captured, *format, output_path, item_ids_capped, QStringLiteral("MBOX"));
}

QJsonObject stringProp(const QString& description) {
    return QJsonObject{{QStringLiteral("type"), QStringLiteral("string")},
                       {QStringLiteral("description"), description}};
}

// Shared JSON-Schema for the email export ops (export_mbox / export_pst). Only the source-path and
// item_ids descriptions differ; the format enum + output_path + required set are identical.
QJsonObject emailExportParamsSchema(const QString& path_desc, const QString& ids_desc) {
    QJsonObject format_prop{{QStringLiteral("type"), QStringLiteral("string")},
                            {QStringLiteral("enum"),
                             QJsonArray{QStringLiteral("eml"),
                                        QStringLiteral("html"),
                                        QStringLiteral("text"),
                                        QStringLiteral("pdf")}},
                            {QStringLiteral("description"),
                             QStringLiteral("Output format per message (default eml)")}};
    QJsonObject ids_prop{{QStringLiteral("type"), QStringLiteral("array")},
                         {QStringLiteral("items"),
                          QJsonObject{{QStringLiteral("type"), QStringLiteral("integer")}}},
                         {QStringLiteral("description"), ids_desc}};
    QJsonObject properties{
        {QStringLiteral("path"), stringProp(path_desc)},
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

QJsonObject exportMboxParamsSchema() {
    return emailExportParamsSchema(
        QStringLiteral("Absolute path to the source MBOX file"),
        QStringLiteral("Message indices to export; omit or empty to export all messages"));
}

// ---------------------------------------------------------------------------
// email.export_pst
// ---------------------------------------------------------------------------

// Read the optional item_ids array for a PST/OST export. PST message ids are 32-bit node IDs (NIDs)
// that can exceed INT_MAX, so parse them as doubles with an explicit unsigned-32-bit range check --
// never toInt (which would silently drop a high NID) and never a raw double->uint cast of an
// out-of-range value (UB). Empty => whole store. The count is capped like the MBOX path.
std::optional<AppActionResult> pstExportItemIdsFromArgs(const QJsonObject& args,
                                                        QVector<uint64_t>& out,
                                                        bool& capped) {
    capped = false;
    if (!args.contains(QStringLiteral("item_ids"))) {
        return std::nullopt;
    }
    const QJsonValue raw_val = args.value(QStringLiteral("item_ids"));
    if (!raw_val.isArray()) {
        return AppActionResult{
            false, QStringLiteral("item_ids must be an array of message node IDs (NIDs)"), {}};
    }
    for (const QJsonValue& value : raw_val.toArray()) {
        if (out.size() >= kMaxExportItems) {
            capped = true;
            break;
        }
        const double raw_id = value.isDouble() ? value.toDouble() : -1.0;
        // Fail CLOSED on a wrong-typed, negative, fractional, NaN/inf, or out-of-range NID: a
        // silently-dropped entry could collapse the set to empty and widen the export to the whole
        // store. NIDs are 32-bit whole numbers (0 .. 0xFFFFFFFF).
        if (!(raw_id >= 0.0) || raw_id > 4294967295.0 || std::isinf(raw_id) ||
            std::floor(raw_id) != raw_id) {
            return AppActionResult{false,
                                   QStringLiteral(
                                       "item_ids entries must be whole node IDs in 0..4294967295"),
                                   {}};
        }
        out.append(static_cast<uint64_t>(raw_id));
    }
    return std::nullopt;
}

// Collect every folder's node id from a PST folder tree (depth-first). Used to seed a whole-store
// export when the caller omits item_ids: PST item enumeration is folder-based (unlike MBOX, which
// pages messages directly), so without at least one folder EmailExportWorker::collectItemIds
// returns nothing and the documented "omit item_ids => export everything" mode would silently
// export zero files. The tree comes from the fan-out-hardened PstParser (bounded, acyclic), so this
// walk terminates. A container folder with no messages contributes an id whose page is simply
// empty.
void collectPstFolderNodeIds(const QVector<PstFolder>& folders, QVector<uint64_t>& out) {
    for (const PstFolder& folder : folders) {
        out.append(folder.node_id);
        collectPstFolderNodeIds(folder.children, out);
    }
}

QJsonObject exportPstParamsSchema() {
    return emailExportParamsSchema(
        QStringLiteral("Absolute path to the source PST or OST file"),
        QStringLiteral("Message node IDs (NIDs, from email.read_pst) to export; omit or empty to "
                       "export every message in the store"));
}

// Export messages from a PST/OST file to eml/html/text/pdf files, driving the app's OWN
// EmailExportWorker::exportItems (the same engine the email panel drives). SYNC: exportItems runs
// inline and emits exportComplete on THIS thread (early failures via emitEarlyFailure too), so a
// direct connection captures the result without any event loop -- the export_mbox pattern. The
// source is opened READ-ONLY through the fan-out-hardened PstParser that email.read_pst already
// exposes (no new parser attack surface); the writer only ADDS files under the required-new/empty
// output_path. Mutating, not destructive (no overwrite/delete), no elevation.
AppActionResult exportPst(const QJsonObject& args) {
    const QString path = args.value(QStringLiteral("path")).toString().trimmed();
    const QString output_path = args.value(QStringLiteral("output_path")).toString().trimmed();
    if (const std::optional<AppActionResult> error = validateExportPaths(
            path,
            output_path,
            kMaxPstExportBytes,
            ExportPathLabels{QStringLiteral("export_pst"), QStringLiteral("PST/OST")})) {
        return *error;
    }

    const std::optional<ExportFormat> format =
        exportFormatFromArg(args.value(QStringLiteral("format")).toString());
    if (!format) {
        return {false, QStringLiteral("format must be one of eml/html/text/pdf"), {}};
    }
    bool item_ids_capped = false;
    QVector<uint64_t> item_ids;
    if (const std::optional<AppActionResult> error =
            pstExportItemIdsFromArgs(args, item_ids, item_ids_capped)) {
        return *error;
    }

    PstParser parser;
    parser.open(path);
    if (!parser.isOpen()) {
        return {false, QStringLiteral("Not a valid PST/OST file: %1").arg(path), {}};
    }

    EmailExportConfig config;
    config.format = *format;
    config.output_path = output_path;
    config.item_ids = item_ids;
    // No explicit item_ids => export the WHOLE store. PST enumeration is folder-based, so seed
    // every folder's node id (the schema documents omit-item_ids as "export every message in the
    // store"); without this collectItemIds would find nothing and report a false "No items to
    // export".
    if (item_ids.isEmpty()) {
        collectPstFolderNodeIds(parser.folderTree(), config.folder_ids);
    }

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
    worker.exportItems(&parser, config);

    if (!completed) {
        return {false,
                error_text.isEmpty() ? QStringLiteral("PST export did not complete") : error_text,
                {}};
    }
    return buildExportResult(
        captured, *format, output_path, item_ids_capped, QStringLiteral("PST"));
}

// ---------------------------------------------------------------------------
// email.save_mbox_attachments
// ---------------------------------------------------------------------------
// Extract every attachment of ONE message in an MBOX file to an existing directory, driving the
// app's OWN sak::AttachmentBatchSave / saveAttachmentToDirectory (the same saver the email
// inspector panel and the attachments browser dialog use). Attachment filenames come from untrusted
// email input, so the saver's sanitizeAttachmentFilename neutralizes path separators / traversal,
// and it dedupes rather than overwriting -- the op only ever ADDS files. Mutating, not destructive
// (no overwrite/delete), no elevation.

// Bound how many attachments one gated call will write, so a crafted message with a huge attachment
// list cannot spray files onto disk. Truncation is reported in the result.
constexpr int kMaxSavedAttachments = 256;

// Validate the destination: an EXISTING directory we write NEW files into. Screens for a reparse
// point (leaf + ancestors) BEFORE the target-following isDir(), so a symlink/junction to a UNC
// share cannot leak the NTLM hash here. dir_field is the op's JSON arg name for this directory, so
// the error text matches the schema (never a "field the schema doesn't have" dead-end).
std::optional<AppActionResult> validateSaveOutputDir(const QString& output_dir,
                                                     const QString& dir_field) {
    if (pathReparseUnsafe(output_dir)) {
        return AppActionResult{false,
                               QStringLiteral(
                                   "%1 must not be a symlink or junction (or under one): %2")
                                   .arg(dir_field, output_dir),
                               {}};
    }
    if (!QFileInfo(output_dir).isDir()) {
        return AppActionResult{
            false,
            QStringLiteral("%1 must be an existing directory: %2").arg(dir_field, output_dir),
            {}};
    }
    return std::nullopt;
}

// Names + limits for validateAttachmentSaveSource, bundled so its signature stays within the arg
// budget. src_field / dir_field are the op's JSON arg names, so error text matches the schema.
struct SaveSourceSpec {
    qint64 max_bytes;
    QString op_name;
    QString file_kind;
    QString src_field;
    QString dir_field;
};

// Shared source+destination validation for the file-writing ops (MBOX/PST attachment save, offline
// image recovery). The source file is opened READ-ONLY; the destination must be an EXISTING
// directory we write NEW files into. Screens both paths for network/UNC/device and reparse points
// (leaf + ancestors) BEFORE any target-following stat, so a symlink/junction to a UNC share cannot
// leak the NTLM hash here.
std::optional<AppActionResult> validateAttachmentSaveSource(const QString& path,
                                                            const QString& output_dir,
                                                            const SaveSourceSpec& spec) {
    if (path.isEmpty() || output_dir.isEmpty()) {
        return AppActionResult{false,
                               QStringLiteral("%1 requires '%2' and '%3'")
                                   .arg(spec.op_name, spec.src_field, spec.dir_field),
                               {}};
    }
    if (isNetworkOrDevicePath(path) || isNetworkOrDevicePath(output_dir)) {
        return AppActionResult{
            false,
            QStringLiteral("%1 does not allow network/UNC or device paths").arg(spec.op_name),
            {}};
    }
    if (pathReparseUnsafe(path)) {
        return AppActionResult{false,
                               QStringLiteral(
                                   "%1 file must not be a symlink or junction (or under one): %2")
                                   .arg(spec.file_kind, path),
                               {}};
    }
    const QFileInfo info(path);
    if (!info.isFile()) {
        return AppActionResult{false,
                               QStringLiteral("No such %1 file: %2").arg(spec.file_kind, path),
                               {}};
    }
    if (info.size() > spec.max_bytes) {
        return AppActionResult{false,
                               QStringLiteral(
                                   "%1 file is too large for a headless op (%2 bytes > %3 limit)")
                                   .arg(spec.file_kind)
                                   .arg(info.size())
                                   .arg(spec.max_bytes),
                               {}};
    }
    return validateSaveOutputDir(output_dir, spec.dir_field);
}

// Validate email.save_mbox_attachments inputs (shared source/dest checks + a valid message_index).
std::optional<AppActionResult> validateMboxAttachmentSaveInputs(const QString& path,
                                                                const QString& output_dir,
                                                                int message_index) {
    if (message_index < 0) {
        return AppActionResult{
            false, QStringLiteral("save_mbox_attachments requires a message_index >= 0"), {}};
    }
    return validateAttachmentSaveSource(path,
                                        output_dir,
                                        SaveSourceSpec{kMaxMboxBytes,
                                                       QStringLiteral("save_mbox_attachments"),
                                                       QStringLiteral("MBOX"),
                                                       QStringLiteral("path"),
                                                       QStringLiteral("output_dir")});
}

// One save-attachments run's parameters, bundled to keep the helper signatures within the argument
// budget. total = attachments in the message; attempted = min(total, kMaxSavedAttachments). id_key
// / id_value carry the source's message identifier into the result payload without hard-coding a
// name
// ("message_index" for MBOX, "message_node_id" for PST -- a NID that would overflow an int).
struct AttachmentSaveRequest {
    QString id_key;
    QJsonValue id_value;
    // The same identifier as id_value, in the form the batch saver keys arrivals by: MBOX
    // message_index or PST node id. Kept as a plain integer because AttachmentRef pairs it with an
    // attachment index to identify exactly one attachment.
    uint64_t message_id = 0;
    int total = 0;
    int attempted = 0;
    bool truncated = false;
    QString output_dir;
};

// Expected set for one save run: attachment indices 0 .. attempted-1 of the request's message. The
// batch accepts only these, so a record can never be written into a slot it does not name.
QVector<sak::AttachmentRef> attachmentRefsFor(const AttachmentSaveRequest& req) {
    QVector<sak::AttachmentRef> refs;
    refs.reserve(req.attempted);
    for (int i = 0; i < req.attempted; ++i) {
        refs.append(sak::AttachmentRef{req.message_id, i});
    }
    return refs;
}

// Running tally for a save-attachments run: the saved file paths, per-item error strings, and the
// success/failure counts. Kept separate from AttachmentBatchSave's internal counters because the
// batch does not expose the individual deduped output paths.
struct AttachmentSaveTally {
    QJsonArray saved_paths;
    QJsonArray errors;
    int saved = 0;
    int failed = 0;
};

// Save the first req.attempted attachments, driving the app's OWN AttachmentBatchSave (which calls
// saveAttachmentToDirectory -> sanitize + dedupe + atomic QSaveFile). Each payload already carries
// its decoded bytes paired with the matching name from ONE recursive parse, so name and content
// always correspond; only a write failure can fail an item.
void runAttachmentSaves(const QVector<MboxAttachmentPayload>& payloads,
                        const AttachmentSaveRequest& req,
                        AttachmentSaveTally& tally) {
    sak::AttachmentBatchSave batch;
    if (!batch.begin(req.output_dir, attachmentRefsFor(req))) {
        tally.errors.append(QStringLiteral("Could not start the attachment save batch"));
        tally.failed = req.attempted;
        return;
    }
    for (int i = 0; i < req.attempted; ++i) {
        const sak::AttachmentSaveResult result = batch.recordOne(
            sak::AttachmentRef{req.message_id, i}, payloads[i].filename, payloads[i].data);
        if (result.success) {
            tally.saved_paths.append(result.saved_path);
            ++tally.saved;
        } else {
            tally.errors.append(result.error_message);
            ++tally.failed;
        }
    }
}

// Map a save run into the tool result. Honest: success only when EVERY attempted attachment was
// saved (failed == 0); any read/write failure -> success=false with a partial-save message and the
// per-item errors in the payload, so a partial run is never reported as a full one.
AppActionResult buildAttachmentSaveResult(const AttachmentSaveTally& tally,
                                          const AttachmentSaveRequest& req) {
    QJsonObject data{{QStringLiteral("output_dir"), req.output_dir},
                     {req.id_key, req.id_value},
                     {QStringLiteral("attachment_total"), req.total},
                     {QStringLiteral("attachments_attempted"), req.attempted},
                     {QStringLiteral("saved_count"), tally.saved},
                     {QStringLiteral("failed_count"), tally.failed},
                     {QStringLiteral("truncated"), req.truncated},
                     {QStringLiteral("saved_paths"), tally.saved_paths},
                     {QStringLiteral("errors"), tally.errors}};
    if (tally.failed == 0 && tally.saved == req.attempted) {
        QString msg =
            QStringLiteral("Saved %1 attachment(s) to %2").arg(tally.saved).arg(req.output_dir);
        if (req.truncated) {
            msg += QStringLiteral(" (capped at %1 of %2)").arg(req.attempted).arg(req.total);
        }
        return {true, msg, data};
    }
    return {false,
            QStringLiteral("Saved %1 of %2 attachment(s); %3 failed")
                .arg(tally.saved)
                .arg(req.attempted)
                .arg(tally.failed),
            data};
}

AppActionResult saveMboxAttachments(const QJsonObject& args) {
    const QString path = args.value(QStringLiteral("path")).toString().trimmed();
    const QString output_dir = args.value(QStringLiteral("output_dir")).toString().trimmed();
    const int message_index = args.value(QStringLiteral("message_index")).toInt(-1);
    if (const std::optional<AppActionResult> error =
            validateMboxAttachmentSaveInputs(path, output_dir, message_index)) {
        return *error;
    }

    MboxParser parser;
    parser.open(path);
    if (!parser.isOpen()) {
        return {false, QStringLiteral("Not a valid MBOX file: %1").arg(path), {}};
    }

    // ONE recursive pass yields each attachment's name paired with its decoded bytes, so name and
    // content always correspond (readMessageDetail's recursive index does NOT align with the
    // non-recursive readAttachmentData extractor), and the message is parsed only once.
    const std::expected<QVector<MboxAttachmentPayload>, sak::error_code> payloads =
        parser.readAllAttachments(message_index);
    if (!payloads) {
        return {false,
                QStringLiteral("Could not read message %1 from %2").arg(message_index).arg(path),
                {}};
    }
    if (payloads->isEmpty()) {
        return {false,
                QStringLiteral("Message %1 has no attachments to save").arg(message_index),
                {}};
    }

    AttachmentSaveRequest req;
    req.id_key = QStringLiteral("message_index");
    req.id_value = message_index;
    req.message_id = static_cast<uint64_t>(message_index);
    req.total = payloads->size();
    req.truncated = req.total > kMaxSavedAttachments;
    req.attempted = req.truncated ? kMaxSavedAttachments : req.total;
    req.output_dir = output_dir;

    AttachmentSaveTally tally;
    runAttachmentSaves(*payloads, req, tally);
    return buildAttachmentSaveResult(tally, req);
}

QJsonObject saveMboxAttachmentsParamsSchema() {
    QJsonObject idx_prop{
        {QStringLiteral("type"), QStringLiteral("integer")},
        {QStringLiteral("description"),
         QStringLiteral("0-based index of the message whose attachments to save")}};
    QJsonObject properties{
        {QStringLiteral("path"),
         stringProp(QStringLiteral("Absolute path to the source MBOX file"))},
        {QStringLiteral("message_index"), idx_prop},
        {QStringLiteral("output_dir"),
         stringProp(QStringLiteral("Existing directory to save the attachments into"))}};
    return QJsonObject{{QStringLiteral("type"), QStringLiteral("object")},
                       {QStringLiteral("properties"), properties},
                       {QStringLiteral("required"),
                        QJsonArray{QStringLiteral("path"),
                                   QStringLiteral("message_index"),
                                   QStringLiteral("output_dir")}},
                       {QStringLiteral("additionalProperties"), false}};
}

// ---------------------------------------------------------------------------
// email.save_pst_attachments
// ---------------------------------------------------------------------------
// The PST/OST pair to save_mbox_attachments. Saves one message's attachments (the message is a node
// id / NID, from email.read_pst) into an EXISTING directory via the same app-OWNED AttachmentBatch
// Save. UNLIKE the MBOX parser, PstParser::readAttachments (names) and readAttachmentData (bytes)
// are DELIBERATELY index-aligned -- both walk the message's attachment sub-nodes with the SAME
// readSingleAttachment success gate + compacted index (the GUI's loadAttachmentContent composes
// them the same way), so pairing name[i] with readAttachmentData(nid,i) is safe here. Mutating, not
// destructive (dedupe never overwrites -> only ADDS files), no admin.

// Parse a PST message node id (NID). NIDs are 32-bit and can exceed INT_MAX, so read as a double
// with an explicit unsigned-32-bit range check (never toInt, which would silently drop a high NID).
quint64 parsePstNodeIdArg(const QJsonValue& value, bool& ok) {
    ok = false;
    if (!value.isDouble()) {
        return 0;
    }
    const double raw = value.toDouble(-1.0);
    if (!(raw >= 0.0) || raw > 4294967295.0) {  // NaN-safe; 0 .. 0xFFFFFFFF
        return 0;
    }
    ok = true;
    return static_cast<quint64>(raw);
}

// Save the first req.attempted attachments of a PST message, driving the app's OWN AttachmentBatch
// Save. metas (from readAttachments) and readAttachmentData(node_id, i) share the same compacted
// attachment index, so metas[i]'s name pairs with the bytes at index i. A per-attachment read
// failure is recorded and the run continues.
void runPstAttachmentSaves(PstParser& parser,
                           const QVector<sak::PstAttachmentInfo>& metas,
                           const AttachmentSaveRequest& req,
                           AttachmentSaveTally& tally) {
    sak::AttachmentBatchSave batch;
    if (!batch.begin(req.output_dir, attachmentRefsFor(req))) {
        tally.errors.append(QStringLiteral("Could not start the attachment save batch"));
        tally.failed = req.attempted;
        return;
    }
    for (int i = 0; i < req.attempted; ++i) {
        const std::expected<QByteArray, sak::error_code> bytes =
            parser.readAttachmentData(req.message_id, i);
        if (!bytes) {
            batch.recordError();
            tally.errors.append(QStringLiteral("attachment %1: could not read bytes").arg(i));
            ++tally.failed;
            continue;
        }
        const QString name = metas[i].long_filename.isEmpty() ? metas[i].filename
                                                              : metas[i].long_filename;
        const sak::AttachmentSaveResult result =
            batch.recordOne(sak::AttachmentRef{req.message_id, i}, name, *bytes);
        if (result.success) {
            tally.saved_paths.append(result.saved_path);
            ++tally.saved;
        } else {
            tally.errors.append(result.error_message);
            ++tally.failed;
        }
    }
}

AppActionResult savePstAttachments(const QJsonObject& args) {
    const QString path = args.value(QStringLiteral("path")).toString().trimmed();
    const QString output_dir = args.value(QStringLiteral("output_dir")).toString().trimmed();
    bool node_ok = false;
    const quint64 node_id = parsePstNodeIdArg(args.value(QStringLiteral("message_node_id")),
                                              node_ok);
    if (!node_ok) {
        return {false,
                QStringLiteral(
                    "save_pst_attachments requires a numeric message_node_id (a NID from "
                    "email.read_pst)"),
                {}};
    }
    if (const std::optional<AppActionResult> error =
            validateAttachmentSaveSource(path,
                                         output_dir,
                                         SaveSourceSpec{kMaxPstExportBytes,
                                                        QStringLiteral("save_pst_attachments"),
                                                        QStringLiteral("PST/OST"),
                                                        QStringLiteral("path"),
                                                        QStringLiteral("output_dir")})) {
        return *error;
    }

    PstParser parser;
    parser.open(path);
    if (!parser.isOpen()) {
        return {false, QStringLiteral("Not a valid PST/OST file: %1").arg(path), {}};
    }

    const std::expected<QVector<sak::PstAttachmentInfo>, sak::error_code> metas =
        parser.readAttachments(node_id);
    if (!metas) {
        return {false,
                QStringLiteral("Could not read attachments for node 0x%1 in %2")
                    .arg(node_id, 0, 16)
                    .arg(path),
                {}};
    }
    if (metas->isEmpty()) {
        return {false,
                QStringLiteral("Message node 0x%1 has no attachments to save").arg(node_id, 0, 16),
                {}};
    }

    AttachmentSaveRequest req;
    req.id_key = QStringLiteral("message_node_id");
    req.id_value = static_cast<double>(node_id);  // NID up to 0xFFFFFFFF -> exact as a double
    req.message_id = node_id;
    req.total = metas->size();
    req.truncated = req.total > kMaxSavedAttachments;
    req.attempted = req.truncated ? kMaxSavedAttachments : req.total;
    req.output_dir = output_dir;

    AttachmentSaveTally tally;
    runPstAttachmentSaves(parser, *metas, req, tally);
    return buildAttachmentSaveResult(tally, req);
}

QJsonObject savePstAttachmentsParamsSchema() {
    QJsonObject nid_prop{
        {QStringLiteral("type"), QStringLiteral("integer")},
        {QStringLiteral("description"),
         QStringLiteral("Message node id (NID, from email.read_pst) whose attachments to save")}};
    QJsonObject properties{
        {QStringLiteral("path"),
         stringProp(QStringLiteral("Absolute path to the source PST or OST file"))},
        {QStringLiteral("message_node_id"), nid_prop},
        {QStringLiteral("output_dir"),
         stringProp(QStringLiteral("Existing directory to save the attachments into"))}};
    return QJsonObject{{QStringLiteral("type"), QStringLiteral("object")},
                       {QStringLiteral("properties"), properties},
                       {QStringLiteral("required"),
                        QJsonArray{QStringLiteral("path"),
                                   QStringLiteral("message_node_id"),
                                   QStringLiteral("output_dir")}},
                       {QStringLiteral("additionalProperties"), false}};
}

// ---------------------------------------------------------------------------
// organizer.organize_directory
// ---------------------------------------------------------------------------
// categoryMappingFromArgs / isSafeCategoryName / firstUnsafeCategory are shared with the
// read-only organizer.preview_organize op (app_organizer_helpers.h) so the preview and the
// apply categorize + containment-check a model-supplied mapping identically.

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
    // Screen the leaf AND ancestors for a reparse point before the following exists()/isDir() stat
    // (a symlink/junction -- at the target OR any ancestor -- to a UNC share would leak the NTLM
    // hash; and this op RELOCATES files, so following a link off the intended tree is doubly
    // wrong).
    if (pathReparseUnsafe(target)) {
        return AppActionResult{
            false,
            QStringLiteral("target_directory must not be a symlink or junction (or under one): %1")
                .arg(target),
            {}};
    }
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
    // Screen the leaf AND ancestors for a reparse point before the following isFile() stat (a
    // symlink/junction -- at the image OR any ancestor -- to a UNC image would leak the NTLM hash).
    if (pathReparseUnsafe(image_path)) {
        return {false,
                QStringLiteral("image_path must not be a symlink or junction (or under one): %1")
                    .arg(image_path),
                {}};
    }
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

// ---------------------------------------------------------------------------
// imaging.restore_recoverable
// ---------------------------------------------------------------------------
// The MUTATING pair to imaging.scan_recoverable: recover carved files out of an offline disk-image
// FILE into a directory, driving the app's OWN FileRecoveryEngine::restoreCandidates (the Partition
// Manager data-recovery restore path). The image is opened READ-ONLY and hashed before+after to
// prove it was not mutated. overwrite_existing is forced OFF (only ADDS files -> mutating, not
// destructive), no elevation. The output filename is DERIVED here (recovered_<hexoffset>.<ext>)
// with a sanitized extension -- the model never supplies a raw name that could traverse out of the
// destination.

constexpr int kMaxRestoreCandidates = 256;
constexpr qint64 kMaxRecoverImageBytes = 512LL * 1024 * 1024;  // cap the double-hashed image read

// Reduce a model-supplied extension to a safe [a-z0-9]{1,8} token (default "bin"), so the derived
// output name can never contain a path separator, "..", drive letter, or other traversal.
QString sanitizeRecoverExtension(const QString& raw) {
    QString ext;
    for (const QChar c : raw) {
        if (ext.size() >= 8) {
            break;
        }
        if (c.isLetterOrNumber()) {
            ext.append(c.toLower());
        }
    }
    return ext.isEmpty() ? QStringLiteral("bin") : ext;
}

// Build the safe, fully-derived output basename for a recovered file.
QString recoveredBaseName(quint64 offset, const QString& safe_ext) {
    return QStringLiteral("recovered_%1.%2")
        .arg(QString::number(offset, 16).rightJustified(16, QLatin1Char('0')), safe_ext);
}

// Parse the model's candidate list into engine candidates. Each candidate carries only an
// offset/size range into the image (bytes are re-read by the engine) and a sanitized extension; the
// id (output name) is derived here, never taken from the model. Rejects an out-of-image or
// oversized range. Caps the count (reports truncation).
std::optional<AppActionResult> parseRecoverCandidates(const QJsonArray& raw,
                                                      qint64 image_size,
                                                      QVector<sak::FileRecoveryCandidate>& out,
                                                      bool& truncated) {
    truncated = false;
    double total_size = 0.0;
    for (const QJsonValue& value : raw) {
        if (out.size() >= kMaxRestoreCandidates) {
            truncated = true;
            break;
        }
        const QJsonObject obj = value.toObject();
        const double off = obj.value(QStringLiteral("offset_bytes")).toDouble(-1.0);
        const double size = obj.value(QStringLiteral("size_bytes")).toDouble(-1.0);
        if (!(off >= 0.0) || !(size > 0.0)) {
            return AppActionResult{false,
                                   QStringLiteral(
                                       "each candidate needs offset_bytes >= 0 and size_bytes > 0"),
                                   {}};
        }
        if (size > static_cast<double>(sak::kFileRecoveryDefaultMaxCandidateBytes)) {
            return AppActionResult{
                false,
                QStringLiteral("a candidate size_bytes exceeds the per-file recovery limit"),
                {}};
        }
        if (off + size > static_cast<double>(image_size)) {
            return AppActionResult{false,
                                   QStringLiteral("a candidate byte range falls outside the image"),
                                   {}};
        }
        // Bound the TOTAL bytes written, not just per-file + count: a real carve yields
        // non-overlapping candidates whose sizes sum to at most the image size. Capping the sum at
        // the image size stops overlapping/duplicated ranges from amplifying a small image into a
        // disk-filling write (the sibling ops cap file COUNT for the same reason).
        total_size += size;
        if (total_size > static_cast<double>(image_size)) {
            return AppActionResult{false,
                                   QStringLiteral("the combined candidate size exceeds the image "
                                                  "size (overlapping or duplicated ranges are not "
                                                  "recoverable)"),
                                   {}};
        }
        sak::FileRecoveryCandidate candidate;
        candidate.offset_bytes = static_cast<uint64_t>(off);
        candidate.size_bytes = static_cast<uint64_t>(size);
        candidate.extension =
            sanitizeRecoverExtension(obj.value(QStringLiteral("extension")).toString());
        candidate.id = recoveredBaseName(candidate.offset_bytes, candidate.extension);
        out.append(std::move(candidate));
    }
    if (out.isEmpty()) {
        return AppActionResult{
            false, QStringLiteral("restore_recoverable requires at least one candidate"), {}};
    }
    return std::nullopt;
}

// Copy up to max strings into a JSON array (bounds the model-facing list).
QJsonArray jsonStringArrayCapped(const QStringList& values, int max) {
    QJsonArray out;
    for (const QString& value : values) {
        if (out.size() >= max) {
            break;
        }
        out.append(value);
    }
    return out;
}

// The human-readable reason a restore did not fully succeed.
QString restoreFailureReason(const sak::FileRecoveryRestoreResult& res) {
    if (!res.source_opened_read_only) {
        return QStringLiteral("could not open the image read-only");
    }
    if (!res.source_not_mutated) {
        return QStringLiteral("could not verify the source image stayed unchanged");
    }
    if (!res.source_hash_covered_whole) {
        return QStringLiteral("could only verify a prefix of the source stayed unchanged");
    }
    return res.warnings.isEmpty() ? QStringLiteral("no files were recovered")
                                  : res.warnings.first();
}

// Map the engine restore result into the tool result. Honest: success only when files were actually
// written AND the engine confirmed the source was opened read-only and stayed byte-identical
// (source_not_mutated); per-candidate skips are surfaced as warnings, never as silent success.
AppActionResult buildRestoreResult(const sak::FileRecoveryRestoreResult& res,
                                   int requested,
                                   bool truncated,
                                   const QString& image_path,
                                   const QString& destination) {
    const int restored = static_cast<int>(res.restored_paths.size());
    QJsonObject data{{QStringLiteral("image_path"), image_path},
                     {QStringLiteral("destination_directory"), destination},
                     {QStringLiteral("requested_count"), requested},
                     {QStringLiteral("restored_count"), restored},
                     {QStringLiteral("truncated"), truncated},
                     {QStringLiteral("source_opened_read_only"), res.source_opened_read_only},
                     {QStringLiteral("source_not_mutated"), res.source_not_mutated},
                     {QStringLiteral("source_hash_covered_whole"), res.source_hash_covered_whole},
                     {QStringLiteral("restored_paths"),
                      jsonStringArrayCapped(res.restored_paths, kMaxRestoreCandidates)},
                     {QStringLiteral("warnings"), jsonStringArrayCapped(res.warnings, 50)}};
    if (restored > 0 && res.source_opened_read_only && res.source_not_mutated &&
        res.source_hash_covered_whole) {
        return {true,
                QStringLiteral("Recovered %1 of %2 file(s) to %3")
                    .arg(restored)
                    .arg(requested)
                    .arg(destination),
                data};
    }
    return {false, QStringLiteral("Recovery failed: %1").arg(restoreFailureReason(res)), data};
}

AppActionResult restoreRecoverable(const QJsonObject& args) {
    const QString image_path = args.value(QStringLiteral("image_path")).toString().trimmed();
    const QString destination =
        args.value(QStringLiteral("destination_directory")).toString().trimmed();
    if (const std::optional<AppActionResult> error =
            validateAttachmentSaveSource(image_path,
                                         destination,
                                         SaveSourceSpec{kMaxRecoverImageBytes,
                                                        QStringLiteral("restore_recoverable"),
                                                        QStringLiteral("image"),
                                                        QStringLiteral("image_path"),
                                                        QStringLiteral("destination_directory")})) {
        return *error;
    }

    const qint64 image_size = QFileInfo(image_path).size();
    QVector<sak::FileRecoveryCandidate> candidates;
    bool truncated = false;
    if (const std::optional<AppActionResult> error =
            parseRecoverCandidates(args.value(QStringLiteral("candidates")).toArray(),
                                   image_size,
                                   candidates,
                                   truncated)) {
        return *error;
    }

    sak::FileRecoveryRestoreOptions options;
    options.image_path = image_path;
    options.destination_directory = destination;
    options.candidates = candidates;
    options.source_hash_bytes = 0;  // hash the whole (capped) image -> strongest not-mutated proof
    options.overwrite_existing = false;  // only ADD files; never overwrite -> not destructive

    const sak::FileRecoveryRestoreResult res = sak::FileRecoveryEngine::restoreCandidates(options);
    return buildRestoreResult(res, candidates.size(), truncated, image_path, destination);
}

QJsonObject restoreRecoverableParamsSchema() {
    QJsonObject int_prop{{QStringLiteral("type"), QStringLiteral("integer")}};
    QJsonObject candidate_props{
        {QStringLiteral("offset_bytes"), int_prop},
        {QStringLiteral("size_bytes"), int_prop},
        {QStringLiteral("extension"),
         stringProp(QStringLiteral("Output file extension (letters/digits only; sanitized)"))}};
    QJsonObject candidate_item{{QStringLiteral("type"), QStringLiteral("object")},
                               {QStringLiteral("properties"), candidate_props},
                               {QStringLiteral("required"),
                                QJsonArray{QStringLiteral("offset_bytes"),
                                           QStringLiteral("size_bytes")}},
                               {QStringLiteral("additionalProperties"), false}};
    QJsonObject candidates_prop{
        {QStringLiteral("type"), QStringLiteral("array")},
        {QStringLiteral("items"), candidate_item},
        {QStringLiteral("description"),
         QStringLiteral(
             "Files to recover (from imaging.scan_recoverable): offset_bytes + size_bytes "
             "locate each file's bytes in the image")}};
    QJsonObject properties{
        {QStringLiteral("image_path"),
         stringProp(QStringLiteral(
             "Absolute path to the offline disk-image FILE to recover from (opened read-only)"))},
        {QStringLiteral("destination_directory"),
         stringProp(QStringLiteral("Existing directory to write recovered files into"))},
        {QStringLiteral("candidates"), candidates_prop}};
    return QJsonObject{{QStringLiteral("type"), QStringLiteral("object")},
                       {QStringLiteral("properties"), properties},
                       {QStringLiteral("required"),
                        QJsonArray{QStringLiteral("image_path"),
                                   QStringLiteral("destination_directory"),
                                   QStringLiteral("candidates")}},
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
// Validate ONE optional byte-count field. Present => it must be a JSON number that is finite,
// non-negative, and integral. A wrong-typed (string/bool/object/null) or fractional/NaN/inf value
// is REFUSED here rather than silently coerced to 0 or truncated by a later toDouble() -- a
// mistyped size on a destructive partition op must fail closed, not become a zero-byte operation.
std::optional<AppActionResult> partitionByteFieldError(const QJsonObject& args, const char* key) {
    const QString name = QString::fromLatin1(key);
    if (!args.contains(name)) {
        return std::nullopt;
    }
    const QJsonValue value = args.value(name);
    if (!value.isDouble()) {
        return AppActionResult{false, QStringLiteral("%1 must be a number of bytes").arg(name), {}};
    }
    const double bytes = value.toDouble();
    if (!(bytes >= 0.0) || std::isinf(bytes) || std::floor(bytes) != bytes) {
        return AppActionResult{
            false,
            QStringLiteral("%1 must be a finite, non-negative, whole number of bytes").arg(name),
            {}};
    }
    return std::nullopt;
}

std::optional<AppActionResult> nonNegativeByteArgsError(const QJsonObject& args) {
    for (const char* key : {"offset_bytes", "size_bytes"}) {
        if (std::optional<AppActionResult> error = partitionByteFieldError(args, key)) {
            return error;
        }
    }
    return std::nullopt;
}

// Validate the payload / confirm_layout_hash / dry_run gate arguments. payload (when present) must
// be a JSON object -- a wrong type must NOT coerce to an empty {} that silently drops every
// operation field. confirm_layout_hash is required (drift guard). dry_run (when present) must be a
// real boolean: a present-but-non-bool value read as false via toBool would silently escalate a
// plan-only request into a destructive apply. Split out to keep validatePartitionApplyArgs within
// the cyclomatic-complexity budget.
std::optional<AppActionResult> partitionApplyGateArgsError(const QJsonObject& args) {
    if (args.contains(QStringLiteral("payload")) &&
        !args.value(QStringLiteral("payload")).isObject()) {
        return AppActionResult{false,
                               QStringLiteral("payload must be an object of operation fields"),
                               {}};
    }
    if (args.value(QStringLiteral("confirm_layout_hash")).toString().trimmed().isEmpty()) {
        return AppActionResult{
            false,
            QStringLiteral("apply_operation requires 'confirm_layout_hash' (the layout_hash from a "
                           "prior list_inventory/preview_operation, to detect layout drift)"),
            {}};
    }
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
    if (const std::optional<AppActionResult> error = validatePartitionApplyArgs(args)) {
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

// ---------------------------------------------------------------------------
// software.uninstall_uwp_app / software.uninstall_program: remove an installed
// Store/UWP app or a Win32 program, silently.
// ---------------------------------------------------------------------------

// Outer bridge ceilings. Each is above the worker's own internal ceiling (UWP: a 60s
// PowerShell removal; Win32: a 20-min silent native uninstall + a fast leftover scan), so it
// only fires if the worker itself fails to bound the run.
constexpr int kUwpUninstallTimeoutMs = 3 * 60 * 1000;
constexpr int kWin32UninstallTimeoutMs = 22 * 60 * 1000;

QJsonObject uninstallProgramParamsSchema(const QString& name_description) {
    QJsonObject name_prop{{QStringLiteral("type"), QStringLiteral("string")},
                          {QStringLiteral("description"), name_description}};
    return QJsonObject{{QStringLiteral("type"), QStringLiteral("object")},
                       {QStringLiteral("properties"),
                        QJsonObject{{QStringLiteral("program_name"), name_prop}}},
                       {QStringLiteral("required"), QJsonArray{QStringLiteral("program_name")}},
                       {QStringLiteral("additionalProperties"), false}};
}

// Drive a prepared UninstallWorker to completion with a hard timeout and a real cancel, then
// map the terminal signal to a tool result. uninstallComplete carries a result code (Success for
// the Standard/UWP paths this drives; Skipped for ForcedUninstall), so the ok==Success branch is
// real handling, not dead code; a failed removal returns unexpected -> WorkerBase::failed; a stop
// -> cancelled. The worker is FULLY JOINED before return: the explicit requestStop + bounded wait +
// terminate fallback below reaps the thread, and ~UninstallWorker itself calls stopAndJoin() (it is
// NOT =default) while its members are still alive, so there is no base-runs-too-late UAF.
AppActionResult driveUninstallWorker(UninstallWorker& worker, const QString& name, int timeout_ms) {
    AsyncActionInvocation inv(timeout_ms);
    QObject::connect(
        &worker,
        &UninstallWorker::uninstallComplete,
        inv.context(),
        [&inv, name](const UninstallReport& report) {
            const bool ok = report.uninstallResult == UninstallReport::UninstallResult::Success;
            QJsonObject data{{QStringLiteral("program"), name},
                             {QStringLiteral("result"),
                              ok ? QStringLiteral("success") : QStringLiteral("failed")},
                             {QStringLiteral("leftovers_found"), report.foundLeftovers.size()}};
            inv.finish({ok,
                        ok ? QStringLiteral("Uninstalled '%1'").arg(name)
                           : QStringLiteral("Uninstall of '%1' failed").arg(name),
                        data});
        });
    QObject::connect(
        &worker, &WorkerBase::failed, inv.context(), [&inv, name](int, const QString& error) {
            inv.finish(
                {false,
                 error.isEmpty() ? QStringLiteral("Uninstall of '%1' failed").arg(name) : error,
                 {}});
        });
    QObject::connect(&worker, &WorkerBase::cancelled, inv.context(), [&inv]() {
        inv.finish({false, QStringLiteral("Uninstall was cancelled"), {}});
    });

    const AppActionResult result = inv.run([&worker]() { worker.start(); });
    worker.requestStop();
    if (!worker.wait(sak::kTimeoutThreadShutdownMs)) {
        worker.terminate();
        worker.wait(sak::kTimeoutThreadTerminateMs);
    }
    return result;
}

// Match the model-supplied display name against the SYSTEM's UWP package list and return the
// single authoritative match in @p out. Returns an error result (to return verbatim) on a
// failed scan or no/ambiguous match. The resolved ProgramInfo carries a system-sourced
// PackageFullName -- the model never supplies the value that reaches PowerShell.
std::optional<AppActionResult> resolveUwpProgram(const QString& name, ProgramInfo& out) {
    ProgramEnumerator enumerator;
    bool scan_ok = true;
    const QVector<ProgramInfo> packages = enumerator.enumerateUwpPackagesSync(scan_ok);
    if (!scan_ok) {
        return AppActionResult{false,
                               QStringLiteral("Could not enumerate installed Store/UWP apps "
                                              "(package scan failed); refusing to uninstall"),
                               {}};
    }
    QVector<ProgramInfo> matches;
    for (const ProgramInfo& program : packages) {
        if (program.displayName.compare(name, Qt::CaseInsensitive) == 0) {
            matches.append(program);
        }
    }
    if (matches.isEmpty()) {
        return AppActionResult{false,
                               QStringLiteral("No installed Store/UWP app named '%1' (for a Win32 "
                                              "program use software.uninstall_program)")
                                   .arg(name),
                               {}};
    }
    if (matches.size() > 1) {
        return AppActionResult{false,
                               QStringLiteral("'%1' matches %2 installed packages; use a more "
                                              "specific name")
                                   .arg(name)
                                   .arg(matches.size()),
                               {}};
    }
    out = matches.first();
    if (!isSafePackageFullName(out.packageFullName)) {
        return AppActionResult{false,
                               QStringLiteral("The resolved package has no valid PackageFullName; "
                                              "cannot uninstall it safely"),
                               {}};
    }
    return std::nullopt;
}

// The name-matches from a raw registry list, reduced to what resolution needs: the set of
// DISTINCT silent uninstall commands (same-name entries double-registered across hives collapse
// here; genuinely different programs do not), a representative program, and whether the name hit
// a system component or existed at all.
struct Win32MatchSummary {
    QSet<QString> distinct_commands;
    ProgramInfo representative;
    bool saw_name = false;
    bool saw_system_component = false;
};

Win32MatchSummary collectWin32Matches(const QVector<ProgramInfo>& programs, const QString& name) {
    Win32MatchSummary summary;
    for (const ProgramInfo& program : programs) {
        if (program.displayName.compare(name, Qt::CaseInsensitive) != 0) {
            continue;
        }
        summary.saw_name = true;
        if (program.isSystemComponent) {
            summary.saw_system_component = true;  // hidden runtime/driver -- never silent-removed
            continue;
        }
        QString cmd;
        if (UninstallWorker::buildSilentUninstallCommand(program, cmd)) {
            summary.distinct_commands.insert(cmd);
            summary.representative = program;
        }
    }
    return summary;
}

// Resolve @p name against the SYSTEM's Win32 registry list. Enumerate + delegate to the pure
// core so the safety logic (system-component refusal, distinct-program ambiguity, interactive
// refusal) is unit-testable without a live registry.
std::optional<AppActionResult> resolveWin32Program(const QString& name, ProgramInfo& out) {
    ProgramEnumerator enumerator;
    return resolveWin32ProgramFromList(enumerator.enumerateRegistryProgramsSync(), name, out);
}

AppActionResult uninstallUwpApp(const QJsonObject& args) {
    const QString name = args.value(QStringLiteral("program_name")).toString().trimmed();
    if (name.isEmpty()) {
        return {false, QStringLiteral("uninstall_uwp_app requires a 'program_name' argument"), {}};
    }
    ProgramInfo program;
    if (const std::optional<AppActionResult> error = resolveUwpProgram(name, program)) {
        return *error;
    }
    // UwpRemove mode: Remove-AppxPackage / -Provisioned is silent, bounded (60s), cancellable,
    // and early-returns BEFORE the leftover scan, so this ONLY removes the package (no cleanup).
    // createRestorePoint false: a Store app is reinstallable and a restore point needs admin.
    UninstallWorker worker(program, UninstallWorker::Mode::UwpRemove, ScanLevel::Safe, false);
    return driveUninstallWorker(worker, name, kUwpUninstallTimeoutMs);
}

AppActionResult uninstallWin32Program(const QJsonObject& args) {
    const QString name = args.value(QStringLiteral("program_name")).toString().trimmed();
    if (name.isEmpty()) {
        return {false, QStringLiteral("uninstall_program requires a 'program_name' argument"), {}};
    }
    ProgramInfo program;
    if (const std::optional<AppActionResult> error = resolveWin32Program(name, program)) {
        return *error;
    }
    // Standard mode + setHeadlessSilent: run the SILENT native uninstaller (quietUninstallString
    // or msiexec /qn, hard 20-min cap; interactive-only was already refused in resolve) then a
    // fast, REPORT-ONLY leftover scan (ScanLevel::Safe -- it never deletes; cleanup is a separate
    // step). createRestorePoint false: the gate offers a restore point (a worker one needs admin).
    //
    // ACCEPTED RESIDUALS (adversarial review, all bounded, none corrupting): (a) a publisher that
    // mislabels QuietUninstallString with an interactive command slips past the silent check and
    // pops UI until the 20-min cap kills its tree; (b) an MSI removal's real work runs in the
    // Installer service, so a timeout-kill of the msiexec client may report failure while the
    // service completes the removal; (c) exit code 3010 (reboot-required) is reported as success
    // without a reboot notice (the worker does not surface nativeExitCode). Interactive hang and
    // wrong-program/system-component removal are PREVENTED (resolve refuses them up front).
    UninstallWorker worker(program, UninstallWorker::Mode::Standard, ScanLevel::Safe, false);
    worker.setHeadlessSilent(true);
    return driveUninstallWorker(worker, name, kWin32UninstallTimeoutMs);
}

using AddMutatingActionFn = std::function<void(const AppActionDescriptor&, AppActionInvoke)>;

// Register the software-management ops. Split out to keep registerMutatingAppActionsInto
// within the length budget as the op set grows.
// ---------------------------------------------------------------------------
// software.clean_leftovers: PERMANENTLY delete selected uninstall-leftover items.
// ---------------------------------------------------------------------------
// The mutating counterpart to software.scan_leftovers: drives CleanupWorker (the WORKER, not the
// AdvancedUninstallController, which persists prefs), which deletes files/folders, whole registry
// key trees, registry values, services (sc delete), scheduled tasks (schtasks /delete /f), and
// firewall rules (netsh ... delete rule). This is the technician's arbitrary-destruction surface,
// so it is gated CATASTROPHIC (a human confirms every item even in Unattended) AND every item is
// screened fail-closed by leftover_cleanup_guard.h before the worker is built -- an OS-critical /
// unrecoverable / injection target (HKLM\SYSTEM, a core service, the \Microsoft\Windows task tree,
// firewall name=all, the Windows dir, a drive/Program Files/Users root, a UNC/symlink path) is
// refused outright. Refusal is ALL-OR-NOTHING: any bad item blocks the whole batch, so a poisoned
// batch cannot smuggle a real deletion past a blocked one. The guard's empty-target rejections also
// satisfy CleanupWorker's Q_ASSERT(!x.isEmpty()) preconditions.

constexpr int kMaxCleanupItems = 500;
constexpr int kCleanupTimeoutMs = 10 * 60 *
                                  1000;  // ceiling above many ~10s sc/schtasks/netsh calls

// Truncate a path/name to a bounded single line for reporting (clampLine is file-local to the
// read-only module; this is the mutating module's own small equivalent).
QString clampCleanupLine(const QString& value) {
    constexpr int kMaxCleanupLineChars = 400;
    QString line = value;
    line.replace(QLatin1Char('\n'), QLatin1Char(' ')).replace(QLatin1Char('\r'), QLatin1Char(' '));
    if (line.size() > kMaxCleanupLineChars) {
        line.truncate(kMaxCleanupLineChars);
        line += QStringLiteral("...");
    }
    return line;
}

std::optional<LeftoverItem::Type> leftoverTypeFromString(const QString& raw) {
    static const QMap<QString, LeftoverItem::Type> kMap = {
        {QStringLiteral("file"), LeftoverItem::Type::File},
        {QStringLiteral("folder"), LeftoverItem::Type::Folder},
        {QStringLiteral("registry_key"), LeftoverItem::Type::RegistryKey},
        {QStringLiteral("registry_value"), LeftoverItem::Type::RegistryValue},
        {QStringLiteral("service"), LeftoverItem::Type::Service},
        {QStringLiteral("scheduled_task"), LeftoverItem::Type::ScheduledTask},
        {QStringLiteral("firewall_rule"), LeftoverItem::Type::FirewallRule},
        {QStringLiteral("startup_entry"), LeftoverItem::Type::StartupEntry},
        {QStringLiteral("shell_extension"), LeftoverItem::Type::ShellExtension}};
    const auto it = kMap.constFind(raw.toLower());
    if (it == kMap.constEnd()) {
        return std::nullopt;
    }
    return *it;
}

// Parse + validate ONE model-supplied item and fill @p out. Returns a refusal reason (empty =
// allowed).
QString validateCleanupItem(const QJsonObject& obj, LeftoverItem& out) {
    const QString type_str = obj.value(QStringLiteral("type")).toString().trimmed();
    const std::optional<LeftoverItem::Type> type = leftoverTypeFromString(type_str);
    if (!type) {
        return QStringLiteral("unknown item type '%1'").arg(clampCleanupLine(type_str));
    }
    out.type = *type;
    out.path = obj.value(QStringLiteral("path")).toString().trimmed();
    // Do NOT trim the registry value name: a value name's leading/trailing spaces are significant,
    // so trimming could target a DIFFERENT value than the model named and the human confirmed. The
    // guard (registryValueDeletionRefusal) still rejects an all-whitespace name and screens
    // control/wildcard characters, so preserving the raw name opens no new risk while keeping the
    // deletion exact.
    out.registryValueName = obj.value(QStringLiteral("registry_value_name")).toString();
    out.selected = true;
    out.sizeBytes = 0;
    return cleanupItemRefusal(out);
}

// Accumulated CleanupWorker signal state: filled by the per-item / summary / reboot-pending slots,
// read once by the finished slot.
struct CleanupTally {
    int succeeded = 0;
    int failed = 0;
    QStringList reboot_paths;
    QStringList recycle_fallback_paths;  // items PERMANENTLY deleted after the Recycle Bin failed
    QJsonArray per_item;
};

// Build the tool result from the accumulated tally. ok = no item outright FAILED; a
// reboot-scheduled file still exists (reported separately, count + note) and a recycle-fallback
// item was destroyed permanently (also reported), so neither honesty caveat is folded silently into
// the cleaned count.
AppActionResult buildCleanupResult(int items_total, const CleanupTally& tally) {
    const bool ok = tally.failed == 0;
    QJsonObject data{
        {QStringLiteral("items_total"), items_total},
        {QStringLiteral("succeeded"), tally.succeeded},
        {QStringLiteral("failed"), tally.failed},
        {QStringLiteral("reboot_pending_count"), tally.reboot_paths.size()},
        {QStringLiteral("reboot_pending"), jsonStringArrayCapped(tally.reboot_paths, 50)},
        {QStringLiteral("permanently_deleted_count"), tally.recycle_fallback_paths.size()},
        {QStringLiteral("permanently_deleted"),
         jsonStringArrayCapped(tally.recycle_fallback_paths, 50)},
        {QStringLiteral("items"), tally.per_item}};
    QString message =
        ok ? QStringLiteral("Cleaned %1 leftover item(s)").arg(tally.succeeded)
           : QStringLiteral("Cleaned %1 item(s); %2 failed").arg(tally.succeeded).arg(tally.failed);
    if (!tally.reboot_paths.isEmpty()) {
        message +=
            QStringLiteral(" (%1 path(s) scheduled for removal on next reboot; not yet deleted)")
                .arg(tally.reboot_paths.size());
    }
    if (!tally.recycle_fallback_paths.isEmpty()) {
        // The Recycle Bin failed for these, so they were destroyed permanently despite recycle
        // mode. Surface it explicitly -- the recoverable-default contract was silently broken
        // otherwise.
        message += QStringLiteral(
                       " (%1 item(s) could NOT be sent to the Recycle Bin and were PERMANENTLY "
                       "deleted)")
                       .arg(tally.recycle_fallback_paths.size());
    }
    return {ok, message, data};
}

// Drive a prepared CleanupWorker to completion, accumulating its per-item / summary /
// reboot-pending signals, and map the terminal signal to a tool result. The worker continues past
// individual failures (an un-elevated sc/schtasks/netsh call fails honestly -> counted failed,
// ok=false), so success means EVERY item deleted. Fully joined before return (requestStop +
// wait/terminate), like driveUninstallWorker, so a timeout cannot outlive the worker's stack.
AppActionResult runCleanupWorker(const QVector<LeftoverItem>& items, bool use_recycle_bin) {
    // worker declared BEFORE inv so it is destroyed AFTER inv: a late finished() on teardown then
    // hits an already-gone context and is dropped rather than delivered to a dangling bridge.
    CleanupWorker worker(items, use_recycle_bin);
    // The recycle choice is a RECOVERABILITY contract and this path has no human reviewing each
    // item, so a recycle failure must leave the item in place (reported failed) instead of
    // escalating to a permanent delete. Stated explicitly here, not left to the worker default.
    worker.setRequireRecoverable(use_recycle_bin);
    AsyncActionInvocation inv(kCleanupTimeoutMs);
    CleanupTally tally;

    QObject::connect(&worker,
                     &CleanupWorker::itemCleaned,
                     inv.context(),
                     [&tally](const QString& path, bool ok) {
                         if (tally.per_item.size() < kMaxCleanupItems) {
                             tally.per_item.append(
                                 QJsonObject{{QStringLiteral("path"), clampCleanupLine(path)},
                                             {QStringLiteral("success"), ok}});
                         }
                     });
    QObject::connect(
        &worker, &CleanupWorker::cleanupComplete, inv.context(), [&tally](int s, int f, qint64) {
            tally.succeeded = s;
            tally.failed = f;
        });
    QObject::connect(&worker,
                     &CleanupWorker::rebootPendingItems,
                     inv.context(),
                     [&tally](const QStringList& paths) { tally.reboot_paths = paths; });
    // recycleFallbackItems is emitted right after rebootPendingItems and before finished, into this
    // same queued caller-thread context, so it is populated before buildCleanupResult reads it.
    QObject::connect(&worker,
                     &CleanupWorker::recycleFallbackItems,
                     inv.context(),
                     [&tally](const QStringList& paths) { tally.recycle_fallback_paths = paths; });
    // Finish on WorkerBase::finished (fires after execute() returns, i.e. AFTER cleanupComplete and
    // rebootPendingItems, which are queued to this same caller-thread context in emission order),
    // so every accumulator is populated by the time this runs.
    QObject::connect(&worker, &WorkerBase::finished, inv.context(), [&inv, &items, &tally]() {
        inv.finish(buildCleanupResult(items.size(), tally));
    });
    QObject::connect(
        &worker, &WorkerBase::failed, inv.context(), [&inv](int, const QString& error) {
            inv.finish({false, error.isEmpty() ? QStringLiteral("Cleanup failed") : error, {}});
        });
    QObject::connect(&worker, &WorkerBase::cancelled, inv.context(), [&inv]() {
        inv.finish({false, QStringLiteral("Cleanup was cancelled"), {}});
    });

    const AppActionResult result = inv.run([&worker]() { worker.start(); });
    worker.requestStop();
    if (!worker.wait(sak::kTimeoutThreadShutdownMs)) {
        worker.terminate();
        worker.wait(sak::kTimeoutThreadTerminateMs);
    }
    return result;
}

// Parse + validate + proof-of-scan-bind the model's item array. Fills @p items (accepted for
// deletion), @p refusals (empty/protected/invalid targets), and @p unscanned (valid targets that no
// software.scan_leftovers surfaced this session -- populated only when NOT technician-overridden).
// Split out so cleanLeftovers stays within the complexity budget.
void collectCleanupItems(const QJsonArray& items_arr,
                         bool technician_override,
                         QVector<LeftoverItem>& items,
                         QStringList& refusals,
                         QStringList& unscanned) {
    for (int i = 0; i < items_arr.size(); ++i) {
        if (!items_arr.at(i).isObject()) {
            refusals << QStringLiteral("item %1 is not an object").arg(i);
            continue;
        }
        const QJsonObject obj = items_arr.at(i).toObject();
        LeftoverItem item;
        const QString reason = validateCleanupItem(obj, item);
        const QString label = item.path.isEmpty() ? obj.value(QStringLiteral("type")).toString()
                                                  : item.path;
        if (!reason.isEmpty()) {
            refusals
                << QStringLiteral("item %1 (%2): %3").arg(i).arg(clampCleanupLine(label), reason);
            continue;
        }
        // Proof-of-scan: unless the technician explicitly overrides, refuse any item a prior scan
        // never surfaced (an injected/fabricated path clears the denylist but not this check).
        if (!technician_override && !LeftoverScanProvenance::instance().contains(item)) {
            unscanned << QStringLiteral("item %1 (%2)").arg(i).arg(clampCleanupLine(label));
            continue;
        }
        items.push_back(item);
    }
}

// Result for a batch blocked because items were not produced by a prior scan (proof-of-scan gate).
AppActionResult provenanceRefusalResult(const QStringList& unscanned, int total) {
    return {
        false,
        QStringLiteral(
            "Refused: %1 of %2 item(s) were not produced by a prior software.scan_leftovers in "
            "this "
            "session; run scan_leftovers for this program first (its output is the safe source for "
            "this list), or pass technician_override=true to clean a hand-authored list. No "
            "deletion "
            "performed. %3")
            .arg(unscanned.size())
            .arg(total)
            .arg(unscanned.mid(0, 10).join(QStringLiteral("; "))),
        QJsonObject{{QStringLiteral("unscanned"), jsonStringArrayCapped(unscanned, 20)},
                    {QStringLiteral("hint"),
                     QStringLiteral("call software.scan_leftovers for this program first")}}};
}

// Out-of-band control gating the technician_override JSON flag. A prompt-injected model can set the
// flag itself, so the flag ALONE must never bypass proof-of-scan. The override is honored only when
// the human technician has separately set this environment variable (out of the model's reach) to a
// truthy value; otherwise the flag is advisory and proof-of-scan stays mandatory. Kept an env
// control (not a session/config object) because these thunks are stateless free functions with no
// session handle to read.
[[nodiscard]] bool technicianOverrideAuthorizedOutOfBand() {
    const QString value =
        qEnvironmentVariable("SAK_LEFTOVER_TECHNICIAN_OVERRIDE").trimmed().toLower();
    return value == QLatin1String("1") || value == QLatin1String("true") ||
           value == QLatin1String("yes") || value == QLatin1String("on");
}

AppActionResult cleanLeftovers(const QJsonObject& args) {
    const QJsonValue items_val = args.value(QStringLiteral("items"));
    if (!items_val.isArray()) {
        return {false, QStringLiteral("clean_leftovers requires an 'items' array"), {}};
    }
    const QJsonArray items_arr = items_val.toArray();
    if (items_arr.isEmpty()) {
        return {false, QStringLiteral("clean_leftovers 'items' array is empty"), {}};
    }
    if (items_arr.size() > kMaxCleanupItems) {
        return {false,
                QStringLiteral("clean_leftovers accepts at most %1 items per call")
                    .arg(kMaxCleanupItems),
                {}};
    }

    // The model-supplied flag only REQUESTS a bypass; it is honored solely when the human
    // technician has set the out-of-band control. Absent that control, proof-of-scan remains
    // enforced -- an injected flag cannot rubber-stamp a fabricated deletion list.
    const bool override_requested = args.value(QStringLiteral("technician_override")).toBool(false);
    const bool technician_override = override_requested && technicianOverrideAuthorizedOutOfBand();
    if (override_requested && !technician_override) {
        sak::logWarning(
            "clean_leftovers: technician_override requested but the out-of-band control "
            "(SAK_LEFTOVER_TECHNICIAN_OVERRIDE) is not set -- flag ignored, proof-of-scan "
            "enforced");
    }

    QVector<LeftoverItem> items;
    items.reserve(items_arr.size());
    QStringList refusals;
    QStringList unscanned;
    collectCleanupItems(items_arr, technician_override, items, refusals, unscanned);

    // All-or-nothing: any refused item blocks the WHOLE batch. Nothing is deleted unless every item
    // clears the fail-closed screen. The denylist (protected/invalid) takes priority over the
    // proof-of-scan gate.
    if (!refusals.isEmpty()) {
        return {false,
                QStringLiteral("Refused: %1 of %2 item(s) target protected/invalid resources; no "
                               "deletion performed. %3")
                    .arg(refusals.size())
                    .arg(items_arr.size())
                    .arg(refusals.mid(0, 10).join(QStringLiteral("; "))),
                QJsonObject{{QStringLiteral("refused"), jsonStringArrayCapped(refusals, 20)}}};
    }
    if (!unscanned.isEmpty()) {
        return provenanceRefusalResult(unscanned, items_arr.size());
    }

    if (technician_override) {
        sak::logWarning(
            "clean_leftovers: technician_override set -- proof-of-scan binding bypassed for "
            "this batch; the fail-closed OS-critical denylist is still enforced");
    }

    // Default to the Recycle Bin (files/folders recoverable), and the worker is put in
    // recoverable-only mode for it, so an item that cannot be recycled is LEFT IN PLACE and
    // reported rather than permanently deleted. Registry keys/values, services, tasks, and
    // firewall rules have no recycle equivalent -- their deletion is always permanent.
    const bool use_recycle_bin = args.value(QStringLiteral("use_recycle_bin")).toBool(true);
    return runCleanupWorker(items, use_recycle_bin);
}

QJsonObject cleanLeftoversParamsSchema() {
    QJsonObject type_prop{{QStringLiteral("type"), QStringLiteral("string")},
                          {QStringLiteral("enum"),
                           QJsonArray{QStringLiteral("file"),
                                      QStringLiteral("folder"),
                                      QStringLiteral("registry_key"),
                                      QStringLiteral("registry_value"),
                                      QStringLiteral("service"),
                                      QStringLiteral("scheduled_task"),
                                      QStringLiteral("firewall_rule"),
                                      QStringLiteral("startup_entry"),
                                      QStringLiteral("shell_extension")}},
                          {QStringLiteral("description"),
                           QStringLiteral("Kind of leftover item to delete")}};
    QJsonObject item_props{
        {QStringLiteral("type"), type_prop},
        {QStringLiteral("path"),
         stringProp(QStringLiteral(
             "The item's path or name: an absolute filesystem path (file/folder), a full registry "
             "key (registry_key/registry_value/startup_entry/shell_extension, e.g. "
             "HKLM\\SOFTWARE\\Vendor\\App), a service name, a scheduled-task name, or a firewall "
             "rule name"))},
        {QStringLiteral("registry_value_name"),
         stringProp(QStringLiteral("For registry_value (or a registry-backed startup_entry): the "
                                   "value name to delete under 'path'"))}};
    QJsonObject item_schema{{QStringLiteral("type"), QStringLiteral("object")},
                            {QStringLiteral("properties"), item_props},
                            {QStringLiteral("required"),
                             QJsonArray{QStringLiteral("type"), QStringLiteral("path")}},
                            {QStringLiteral("additionalProperties"), false}};
    QJsonObject items_prop{
        {QStringLiteral("type"), QStringLiteral("array")},
        {QStringLiteral("description"),
         QStringLiteral("Leftover items to permanently delete (typically copied from "
                        "software.scan_leftovers output). OS-critical targets (the Windows dir, "
                        "SYSTEM/SAM/SECURITY and shared SOFTWARE registry roots, core services, "
                        "\\Microsoft\\Windows tasks, firewall name=all, drive/Program Files/Users "
                        "roots, UNC/symlink paths) are always refused; any refused item blocks the "
                        "entire batch.")},
        {QStringLiteral("items"), item_schema}};
    QJsonObject recycle_prop{
        {QStringLiteral("type"), QStringLiteral("boolean")},
        {QStringLiteral("description"),
         QStringLiteral("If true (default), deleted files/folders go to the Recycle Bin "
                        "(recoverable); if false, permanent deletion. Registry keys/values, "
                        "services, tasks, and firewall rules are always permanent.")}};
    QJsonObject override_prop{
        {QStringLiteral("type"), QStringLiteral("boolean")},
        {QStringLiteral("description"),
         QStringLiteral(
             "Leave false. Every item must come from a prior software.scan_leftovers in "
             "this session; items that did not are refused. Setting true only REQUESTS a "
             "bypass of the proof-of-scan check for a hand-authored list -- it is honored "
             "solely when a human technician has separately enabled the out-of-band "
             "control (the SAK_LEFTOVER_TECHNICIAN_OVERRIDE environment variable); "
             "otherwise the flag is ignored and proof-of-scan stays enforced. The "
             "OS-critical denylist always applies and the request is surfaced in the run "
             "log.")}};
    return QJsonObject{{QStringLiteral("type"), QStringLiteral("object")},
                       {QStringLiteral("properties"),
                        QJsonObject{{QStringLiteral("items"), items_prop},
                                    {QStringLiteral("use_recycle_bin"), recycle_prop},
                                    {QStringLiteral("technician_override"), override_prop}}},
                       {QStringLiteral("required"), QJsonArray{QStringLiteral("items")}},
                       {QStringLiteral("additionalProperties"), false}};
}

void registerSoftwareOps(const AddMutatingActionFn& add) {
    // software.uninstall_uwp_app: removes an installed Store/UWP app via UninstallWorker
    // (Remove-AppxPackage: silent, bounded, cancellable; early-returns before any leftover
    // cleanup). Removing an app + its per-user data is destructive; provisioned (all-users)
    // removal needs admin. NOT catastrophic (reinstallable, no disk wipe).
    AppActionDescriptor uninstall_uwp = mutatingDescriptor(
        QStringLiteral("software.uninstall_uwp_app"),
        QStringLiteral("Uninstall a Store/UWP app"),
        QStringLiteral("Remove an installed Microsoft Store / UWP app by name (silent)"),
        QStringLiteral("software"));
    uninstall_uwp.params_schema = uninstallProgramParamsSchema(
        QStringLiteral("Exact display name of the installed Store/UWP app to remove "
                       "(as shown by security.list_installed_programs)"));
    uninstall_uwp.destructive = true;
    uninstall_uwp.requires_admin = true;
    add(uninstall_uwp, uninstallUwpApp);

    // software.uninstall_program: removes an installed Win32 program via UninstallWorker in
    // Standard + headless-silent mode -- only a SILENT command (quietUninstallString or
    // msiexec /qn) with a hard timeout; a program whose uninstaller is interactive-only is
    // refused (it would hang a headless run). Then a fast, REPORT-ONLY leftover scan. Removing
    // a program is destructive + needs admin; NOT catastrophic (no disk wipe).
    AppActionDescriptor uninstall_win32 = mutatingDescriptor(
        QStringLiteral("software.uninstall_program"),
        QStringLiteral("Uninstall a Win32 program"),
        QStringLiteral("Silently uninstall an installed Win32 (desktop) program by name. Only "
                       "programs with a silent/quiet uninstaller are supported; interactive-only "
                       "uninstallers are refused. Also reports leftover files/keys (no delete)."),
        QStringLiteral("software"));
    uninstall_win32.params_schema = uninstallProgramParamsSchema(
        QStringLiteral("Exact display name of the installed Win32 program to remove "
                       "(as shown by security.list_installed_programs)"));
    uninstall_win32.destructive = true;
    uninstall_win32.requires_admin = true;
    add(uninstall_win32, uninstallWin32Program);

    // software.clean_leftovers: PERMANENTLY deletes selected leftover items (files, folders,
    // registry keys/values, services, scheduled tasks, firewall rules, startup entries) found by
    // software.scan_leftovers. The broadest destruction surface the assistant drives -> destructive
    // + CATASTROPHIC (forces a human confirm even in Unattended) + requires_admin (registry HKLM,
    // sc/schtasks/netsh all need elevation). leftover_cleanup_guard.h screens every item
    // fail-closed (OS-critical/unrecoverable/injection targets refused) BEFORE CleanupWorker is
    // built, and any refusal blocks the whole batch.
    AppActionDescriptor clean = mutatingDescriptor(
        QStringLiteral("software.clean_leftovers"),
        QStringLiteral("Delete uninstall leftovers"),
        QStringLiteral("Permanently delete selected leftover items (files, folders, registry "
                       "keys/values, services, scheduled tasks, firewall rules, startup entries) "
                       "found by software.scan_leftovers. ERASES data; OS-critical targets are "
                       "always refused."),
        QStringLiteral("software"));
    clean.params_schema = cleanLeftoversParamsSchema();
    clean.destructive = true;
    clean.catastrophic = true;
    clean.requires_admin = true;
    add(clean, cleanLeftovers);
}

// ---------------------------------------------------------------------------
// network.set_adapter_dhcp: reset an Ethernet adapter to automatic (DHCP).
// ---------------------------------------------------------------------------

QJsonObject setAdapterDhcpParamsSchema() {
    QJsonObject name_prop{
        {QStringLiteral("type"), QStringLiteral("string")},
        {QStringLiteral("description"),
         QStringLiteral("Exact name of the network adapter to reset to DHCP -- any dedicated "
                        "adapter (Ethernet OR Wi-Fi), e.g. \"Ethernet\" or \"Wi-Fi\"; as shown by "
                        "network.list_adapters. Briefly drops that adapter's connectivity.")}};
    return QJsonObject{{QStringLiteral("type"), QStringLiteral("object")},
                       {QStringLiteral("properties"),
                        QJsonObject{{QStringLiteral("adapter_name"), name_prop}}},
                       {QStringLiteral("required"), QJsonArray{QStringLiteral("adapter_name")}},
                       {QStringLiteral("additionalProperties"), false}};
}

// Resolve the model-supplied adapter name against the system's OWN adapter list (case-insensitive
// exact match). listEthernetAdapters() returns every "dedicated" adapter netsh reports -- which
// includes Wi-Fi and other non-loopback adapters, NOT wired-only -- so this op is honestly a
// per-adapter DHCP reset, not Ethernet-exclusive. Returns the authoritative system spelling, or
// empty. This is the anti-injection barrier: only a real, system-sourced name ever reaches netsh,
// so a prompt-injected or bogus name is refused before any command runs.
QString resolveEthernetAdapter(EthernetConfigManager& manager, const QString& requested) {
    const QStringList adapters = manager.listEthernetAdapters();
    for (const QString& candidate : adapters) {
        if (candidate.compare(requested, Qt::CaseInsensitive) == 0) {
            return candidate;
        }
    }
    return QString();
}

// Reset a named network adapter (Ethernet or Wi-Fi) to automatic (DHCP) via EthernetConfigManager
// (netsh interface ip set ... source=dhcp). SYNC + bounded (a couple of ~10s netsh calls, shell-
// free via runProcess -- no interpolation). The adapter name is exact-matched against the system's
// own adapter list first, so a bogus/injected name never reaches netsh. netsh set needs elevation,
// so a non-elevated run fails HONESTLY, never silently. Mutating + requires_admin; NOT destructive
// (reversible config change, no data loss) and not catastrophic -- the panel gate still fires (chat
// refuses; Assisted confirms with the adapter name shown).
AppActionResult setAdapterDhcp(const QJsonObject& args) {
    const QString adapter = args.value(QStringLiteral("adapter_name")).toString().trimmed();
    if (adapter.isEmpty()) {
        return {false, QStringLiteral("set_adapter_dhcp requires an 'adapter_name' argument"), {}};
    }

    EthernetConfigManager manager;
    const QString resolved = resolveEthernetAdapter(manager, adapter);
    if (resolved.isEmpty()) {
        const QStringList adapters = manager.listEthernetAdapters();
        return {false,
                QStringLiteral("No network adapter named '%1' (available: %2)")
                    .arg(adapter,
                         adapters.isEmpty() ? QStringLiteral("none found") : adapters.join(", ")),
                {}};
    }

    QString error_text;
    QObject::connect(&manager,
                     &EthernetConfigManager::errorOccurred,
                     &manager,
                     [&error_text](const QString& error) {
                         if (error_text.isEmpty()) {
                             error_text = error;
                         }
                     });

    // A DHCP snapshot: isValid() requires a non-empty adapterName + backupTimestamp and
    // (dhcpEnabled || an IP); restoreSettings(dhcp) sets IPv4 to DHCP (authoritative, checked) and
    // sets DNS to automatic, now reporting whether that DNS step actually succeeded (dns_applied)
    // rather than implying it always did.
    EthernetConfigSnapshot snapshot;
    snapshot.adapterName = resolved;
    snapshot.dhcpEnabled = true;
    snapshot.backupTimestamp = QDateTime::currentDateTime().toString(Qt::ISODate);
    bool dns_applied = false;
    if (!manager.restoreSettings(snapshot, resolved, &dns_applied)) {
        return {false,
                error_text.isEmpty()
                    ? QStringLiteral("Failed to set adapter '%1' to DHCP (changing network config "
                                     "needs administrator rights; run S.A.K. elevated)")
                          .arg(resolved)
                    : error_text,
                {}};
    }
    const QString message =
        dns_applied
            ? QStringLiteral("Set adapter '%1' to automatic (DHCP), including DNS").arg(resolved)
            : QStringLiteral(
                  "Set adapter '%1' IPv4 to automatic (DHCP), but DNS could NOT be set "
                  "to automatic -- set DNS manually")
                  .arg(resolved);
    return {true,
            message,
            QJsonObject{{QStringLiteral("adapter_name"), resolved},
                        {QStringLiteral("dhcp_enabled"), true},
                        {QStringLiteral("dns_automatic"), dns_applied}}};
}

// ---------------------------------------------------------------------------
// network.set_adapter_static_ip: assign a static IPv4 configuration to an adapter.
// ---------------------------------------------------------------------------

// Cap how many DNS servers a single static-IP call may set. netsh takes a small primary+secondary
// list; a handful is plenty and bounds the number of netsh calls restoreDnsServers issues.
constexpr int kMaxStaticDnsServers = 8;

// True if @p value is a well-formed IPv4 address in exact dotted form. QHostAddress::setAddress
// accepts ONLY a complete address (no CIDR suffix, no surrounding whitespace, no trailing junk), so
// this refuses garbage up front (an honest error instead of an opaque netsh parse failure) and
// guarantees the token shape that reaches netsh. runProcess is already shell-free (each value is a
// single argv token, so there is no flag/shell injection); this is defense-in-depth on top of that.
bool isValidIPv4(const QString& value) {
    QHostAddress address;
    return address.setAddress(value) && address.protocol() == QAbstractSocket::IPv4Protocol;
}

// Validate the required + optional address arguments (adapter/ip/mask/gateway). Returns an error
// result to return verbatim, or nullopt when all are well-formed. Split out to keep the op within
// the complexity/length budget. DNS entries are validated separately (they are a list).
std::optional<AppActionResult> validateStaticIpArgs(const QString& adapter,
                                                    const QString& ip,
                                                    const QString& mask,
                                                    const QString& gateway) {
    if (adapter.isEmpty() || ip.isEmpty() || mask.isEmpty()) {
        return AppActionResult{
            false,
            QStringLiteral(
                "set_adapter_static_ip requires 'adapter_name', 'ip_address', and 'subnet_mask'"),
            {}};
    }
    if (!isValidIPv4(ip)) {
        return AppActionResult{false,
                               QStringLiteral(
                                   "ip_address must be a valid IPv4 address (e.g. 192.168.1.50)"),
                               {}};
    }
    if (!isValidIPv4(mask)) {
        return AppActionResult{false,
                               QStringLiteral(
                                   "subnet_mask must be a dotted IPv4 mask (e.g. 255.255.255.0)"),
                               {}};
    }
    if (!gateway.isEmpty() && !isValidIPv4(gateway)) {
        return AppActionResult{false,
                               QStringLiteral("gateway, if given, must be a valid IPv4 address"),
                               {}};
    }
    return std::nullopt;
}

// Read + validate the optional dns_servers array. Each entry must be a valid IPv4 address; the
// first invalid entry fails the whole op (via @p error). Entries are de-duplicated and normalized
// to their canonical dotted form, then capped at kMaxStaticDnsServers. De-duping matters: netsh's
// "add dnsservers" fails ("The object already exists.", non-zero exit) if the same address is added
// twice, which -- since the static IP is applied FIRST -- would otherwise turn a live, applied
// address into a reported failure (see setAdapterStaticIp). Canonicalizing also feeds netsh the
// exact dotted form even if a non-dotted numeric IPv4 spelling was supplied.
QStringList staticDnsFromArgs(const QJsonObject& args, std::optional<AppActionResult>& error) {
    QStringList dns;
    QSet<QString> seen;
    error = std::nullopt;
    const QJsonArray raw = args.value(QStringLiteral("dns_servers")).toArray();
    for (const QJsonValue& value : raw) {
        const QString entry = value.toString().trimmed();
        if (entry.isEmpty()) {
            continue;
        }
        QHostAddress address;
        if (!address.setAddress(entry) || address.protocol() != QAbstractSocket::IPv4Protocol) {
            error = AppActionResult{
                false,
                QStringLiteral("dns_servers entry '%1' is not a valid IPv4 address").arg(entry),
                {}};
            return {};
        }
        const QString canonical = address.toString();
        if (seen.contains(canonical)) {
            continue;  // drop a duplicate so netsh's "add dnsservers" never fails on it
        }
        if (dns.size() >= kMaxStaticDnsServers) {
            break;
        }
        seen.insert(canonical);
        dns.append(canonical);
    }
    return dns;
}

QJsonObject setAdapterStaticIpParamsSchema() {
    const auto strProp = [](const QString& description) {
        return QJsonObject{{QStringLiteral("type"), QStringLiteral("string")},
                           {QStringLiteral("description"), description}};
    };
    QJsonObject dns_prop{
        {QStringLiteral("type"), QStringLiteral("array")},
        {QStringLiteral("items"), QJsonObject{{QStringLiteral("type"), QStringLiteral("string")}}},
        {QStringLiteral("description"),
         QStringLiteral("Optional DNS server IPv4 addresses (first is primary); omit to leave DNS "
                        "unchanged")}};
    QJsonObject properties{
        {QStringLiteral("adapter_name"),
         strProp(QStringLiteral("Exact name of the network adapter to configure (Ethernet OR "
                                "Wi-Fi), as shown by network.list_adapters"))},
        {QStringLiteral("ip_address"),
         strProp(QStringLiteral("Static IPv4 address to assign, e.g. \"192.168.1.50\""))},
        {QStringLiteral("subnet_mask"),
         strProp(QStringLiteral("Dotted IPv4 subnet mask, e.g. \"255.255.255.0\""))},
        {QStringLiteral("gateway"),
         strProp(QStringLiteral("Optional default gateway IPv4 address, e.g. \"192.168.1.1\""))},
        {QStringLiteral("dns_servers"), dns_prop}};
    return QJsonObject{{QStringLiteral("type"), QStringLiteral("object")},
                       {QStringLiteral("properties"), properties},
                       {QStringLiteral("required"),
                        QJsonArray{QStringLiteral("adapter_name"),
                                   QStringLiteral("ip_address"),
                                   QStringLiteral("subnet_mask")}},
                       {QStringLiteral("additionalProperties"), false}};
}

// Build the human-readable success message describing exactly what was applied.
QString staticIpSuccessMessage(const QString& adapter,
                               const QString& ip,
                               const QString& mask,
                               const QString& gateway,
                               const QStringList& dns) {
    QString message =
        QStringLiteral("Set adapter '%1' to static IPv4 %2 (mask %3)").arg(adapter, ip, mask);
    if (!gateway.isEmpty()) {
        message += QStringLiteral(", gateway %1").arg(gateway);
    }
    if (!dns.isEmpty()) {
        message += QStringLiteral(", DNS %1").arg(dns.join(QStringLiteral(", ")));
    }
    return message;
}

// Map a FAILED restoreSettings into an honest result. restoreSettings applies the address+mask
// (+gateway) in ONE netsh command FIRST, then DNS, folding both into one bool -- so a failure can
// mean either "the address command failed (nothing applied)" or "the address is already LIVE and
// only DNS failed". These are distinguished by which step emitted error_text: the address step
// emits "Failed to set IP address ...", the DNS steps emit "... DNS ...". When the address already
// applied, the caller MUST be told (connectivity may already have moved), so we report
// success=false but disclose the applied config -- an empty "nothing changed" result would be
// dishonest. (Residual: if netsh partially applied the address command itself and still reported
// the IP-address error, we under-report by treating it as nothing-applied -- the safer direction,
// and a netsh-internal edge.)
AppActionResult staticIpFailureResult(const QString& adapter,
                                      const QString& ip,
                                      const QString& mask,
                                      const QString& gateway,
                                      const QString& error_text) {
    const bool address_applied = !error_text.isEmpty() &&
                                 !error_text.contains(QStringLiteral("IP address"));
    if (address_applied) {
        QJsonObject data{{QStringLiteral("adapter_name"), adapter},
                         {QStringLiteral("ip_address"), ip},
                         {QStringLiteral("subnet_mask"), mask},
                         {QStringLiteral("address_applied"), true},
                         {QStringLiteral("dns_applied"), false}};
        if (!gateway.isEmpty()) {
            data.insert(QStringLiteral("gateway"), gateway);
        }
        return {false,
                QStringLiteral("Set adapter '%1' to static IPv4 %2 (mask %3), but configuring DNS "
                               "failed (%4). The adapter is ALREADY on the new static IP -- revert "
                               "with set_adapter_dhcp if this dropped connectivity.")
                    .arg(adapter, ip, mask, error_text),
                data};
    }
    return {false,
            error_text.isEmpty()
                ? QStringLiteral("Failed to set adapter '%1' to static IP %2 (changing network "
                                 "config needs administrator rights; run S.A.K. elevated)")
                      .arg(adapter, ip)
                : error_text,
            {}};
}

// Assign a static IPv4 configuration (address + mask + optional gateway + optional DNS) to a named
// network adapter (Ethernet or Wi-Fi) via EthernetConfigManager (netsh interface ip set address /
// set dnsservers source=static). SYNC + bounded (a few ~10s shell-free netsh calls). The adapter
// name is exact-matched against the system's own adapter list first, so a bogus/injected name never
// reaches netsh; every address value is validated as a well-formed IPv4 before use. netsh set needs
// elevation, so a non-elevated run fails HONESTLY. Mutating + requires_admin; NOT destructive
// (reversible config change, no data loss -- use set_adapter_dhcp to revert) and not catastrophic.
// The panel gate still fires (chat refuses; Assisted confirms with the adapter shown).
AppActionResult setAdapterStaticIp(const QJsonObject& args) {
    const QString adapter = args.value(QStringLiteral("adapter_name")).toString().trimmed();
    const QString ip = args.value(QStringLiteral("ip_address")).toString().trimmed();
    const QString mask = args.value(QStringLiteral("subnet_mask")).toString().trimmed();
    const QString gateway = args.value(QStringLiteral("gateway")).toString().trimmed();
    if (std::optional<AppActionResult> error = validateStaticIpArgs(adapter, ip, mask, gateway)) {
        return *error;
    }
    std::optional<AppActionResult> dns_error;
    const QStringList dns = staticDnsFromArgs(args, dns_error);
    if (dns_error) {
        return *dns_error;
    }

    EthernetConfigManager manager;
    const QString resolved = resolveEthernetAdapter(manager, adapter);
    if (resolved.isEmpty()) {
        const QStringList adapters = manager.listEthernetAdapters();
        return {false,
                QStringLiteral("No network adapter named '%1' (available: %2)")
                    .arg(adapter,
                         adapters.isEmpty() ? QStringLiteral("none found") : adapters.join(", ")),
                {}};
    }

    QString error_text;
    QObject::connect(&manager,
                     &EthernetConfigManager::errorOccurred,
                     &manager,
                     [&error_text](const QString& error) {
                         if (error_text.isEmpty()) {
                             error_text = error;
                         }
                     });

    EthernetConfigSnapshot snapshot;
    snapshot.adapterName = resolved;
    snapshot.dhcpEnabled = false;
    snapshot.ipv4Address = ip;
    snapshot.ipv4SubnetMask = mask;
    snapshot.ipv4Gateway = gateway;
    snapshot.ipv4DnsServers = dns;
    snapshot.backupTimestamp = QDateTime::currentDateTime().toString(Qt::ISODate);
    // restoreSettings(static) applies the address+mask+gateway (authoritative, checked) FIRST, then
    // the static DNS list, folding both into one bool. staticIpFailureResult reads error_text to
    // tell an address-step failure (nothing applied) from a DNS-step failure (address already live)
    // and reports each honestly -- a bare success=false would wrongly imply the adapter is
    // unchanged.
    if (!manager.restoreSettings(snapshot, resolved)) {
        return staticIpFailureResult(resolved, ip, mask, gateway, error_text);
    }
    QJsonObject data{{QStringLiteral("adapter_name"), resolved},
                     {QStringLiteral("ip_address"), ip},
                     {QStringLiteral("subnet_mask"), mask}};
    if (!gateway.isEmpty()) {
        data.insert(QStringLiteral("gateway"), gateway);
    }
    if (!dns.isEmpty()) {
        data.insert(QStringLiteral("dns_servers"), QJsonArray::fromStringList(dns));
    }
    return {true, staticIpSuccessMessage(resolved, ip, mask, gateway, dns), data};
}

// ---------------------------------------------------------------------------
// network.set_adapter_dns: set an adapter's DNS servers, leaving IP config alone.
// ---------------------------------------------------------------------------

QJsonObject setAdapterDnsParamsSchema() {
    QJsonObject name_prop{
        {QStringLiteral("type"), QStringLiteral("string")},
        {QStringLiteral("description"),
         QStringLiteral("Exact name of the network adapter (Ethernet OR Wi-Fi) whose DNS servers "
                        "to set, as shown by network.list_adapters")}};
    QJsonObject dns_prop{
        {QStringLiteral("type"), QStringLiteral("array")},
        {QStringLiteral("items"), QJsonObject{{QStringLiteral("type"), QStringLiteral("string")}}},
        {QStringLiteral("description"),
         QStringLiteral("IPv4 DNS server addresses to set (first is primary); at least one "
                        "required. Use set_adapter_dhcp to return DNS to automatic.")}};
    return QJsonObject{{QStringLiteral("type"), QStringLiteral("object")},
                       {QStringLiteral("properties"),
                        QJsonObject{{QStringLiteral("adapter_name"), name_prop},
                                    {QStringLiteral("dns_servers"), dns_prop}}},
                       {QStringLiteral("required"),
                        QJsonArray{QStringLiteral("adapter_name"), QStringLiteral("dns_servers")}},
                       {QStringLiteral("additionalProperties"), false}};
}

// Map a FAILED setDnsServers into an honest result. A non-elevated run fails at the FIRST command
// (primary set) with nothing applied -> report the elevation hint. But once the primary set
// succeeded (primary_applied), the adapter's DNS is ALREADY redirected even though a later
// secondary `add` failed -> disclose the applied primary (mirrors staticIpFailureResult); a bare
// failure would wrongly imply nothing changed.
AppActionResult dnsFailureResult(const QString& adapter,
                                 const QString& primary_dns,
                                 bool primary_applied,
                                 const QString& error_text) {
    if (primary_applied) {
        return {false,
                QStringLiteral("Set adapter '%1' primary DNS to %2, but adding a secondary DNS "
                               "server failed (%3). The adapter is ALREADY using the new DNS -- "
                               "revert with set_adapter_dhcp if this is wrong.")
                    .arg(adapter,
                         primary_dns,
                         error_text.isEmpty() ? QStringLiteral("unknown error") : error_text),
                QJsonObject{{QStringLiteral("adapter_name"), adapter},
                            {QStringLiteral("primary_dns_applied"), primary_dns},
                            {QStringLiteral("dns_partially_applied"), true}}};
    }
    return {false,
            error_text.isEmpty()
                ? QStringLiteral("Failed to set DNS on adapter '%1' (changing network config needs "
                                 "administrator rights; run S.A.K. elevated)")
                      .arg(adapter)
                : error_text,
            {}};
}

// Set a named adapter's IPv4 DNS servers to a static list via EthernetConfigManager (netsh
// interface ip set/add dnsservers source=static), leaving the adapter's IP mode (DHCP or static)
// untouched. SYNC + bounded (a few ~10s shell-free netsh calls). The adapter name is exact-matched
// against the system's OWN adapter list first (anti-injection: a bogus/injected name never reaches
// netsh); every DNS value is validated + de-duplicated as a well-formed IPv4 (staticDnsFromArgs)
// before use. netsh set needs elevation, so a non-elevated run fails HONESTLY. Mutating +
// requires_admin; NOT destructive (reversible -- set_adapter_dhcp returns DNS to automatic) and not
// catastrophic. The panel gate still fires (chat refuses; Assisted confirms with the adapter
// shown).
AppActionResult setAdapterDns(const QJsonObject& args) {
    const QString adapter = args.value(QStringLiteral("adapter_name")).toString().trimmed();
    if (adapter.isEmpty()) {
        return {false, QStringLiteral("set_adapter_dns requires an 'adapter_name' argument"), {}};
    }
    std::optional<AppActionResult> dns_error;
    const QStringList dns = staticDnsFromArgs(args, dns_error);
    if (dns_error) {
        return *dns_error;
    }
    if (dns.isEmpty()) {
        return {false,
                QStringLiteral("set_adapter_dns requires at least one valid IPv4 address in "
                               "'dns_servers' (use set_adapter_dhcp to return DNS to automatic)"),
                {}};
    }

    EthernetConfigManager manager;
    const QString resolved = resolveEthernetAdapter(manager, adapter);
    if (resolved.isEmpty()) {
        const QStringList adapters = manager.listEthernetAdapters();
        return {false,
                QStringLiteral("No network adapter named '%1' (available: %2)")
                    .arg(adapter,
                         adapters.isEmpty() ? QStringLiteral("none found") : adapters.join(", ")),
                {}};
    }

    QString error_text;
    QObject::connect(&manager,
                     &EthernetConfigManager::errorOccurred,
                     &manager,
                     [&error_text](const QString& error) {
                         if (error_text.isEmpty()) {
                             error_text = error;
                         }
                     });

    bool primary_applied = false;
    if (!manager.setDnsServers(resolved, dns, primary_applied)) {
        return dnsFailureResult(resolved, dns.first(), primary_applied, error_text);
    }
    return {
        true,
        QStringLiteral("Set adapter '%1' DNS to %2").arg(resolved, dns.join(QStringLiteral(", "))),
        QJsonObject{{QStringLiteral("adapter_name"), resolved},
                    {QStringLiteral("dns_servers"), QJsonArray::fromStringList(dns)}}};
}

// ---------------------------------------------------------------------------
// network.flush_dns: clear the local DNS resolver cache.
// ---------------------------------------------------------------------------

QJsonObject flushDnsParamsSchema() {
    return QJsonObject{{QStringLiteral("type"), QStringLiteral("object")},
                       {QStringLiteral("properties"), QJsonObject{}},
                       {QStringLiteral("additionalProperties"), false}};
}

// Flush the local DNS resolver cache via DnsDiagnosticTool (the dnsapi DnsFlushResolverCache API --
// the same call ipconfig /flushdns makes, but with a reliable BOOL result). SYNC: flushDnsCache()
// returns then emits inline on this thread, captured by direct connections with no event loop --
// the dns_query pattern. Mutating (clears a system cache) but NOT requires_admin (the flush needs
// no elevation), NOT destructive (the cache is transient and repopulates on the next lookup -- no
// data loss) and not catastrophic; the panel gate fires at the Assisted-confirm tier. Honesty:
// flushDnsCache emits dnsCacheFlushed on BOTH paths and errorOccurred only on a REAL failure (the
// API returned false), so success is inferred from the ABSENCE of an errorOccurred -- a failed
// flush is reported as an honest failure, never a false success.
AppActionResult flushDns(const QJsonObject&) {
    DnsDiagnosticTool tool;
    bool flushed = false;
    QString error_text;
    QObject::connect(
        &tool, &DnsDiagnosticTool::errorOccurred, &tool, [&error_text](const QString& error) {
            if (error_text.isEmpty()) {
                error_text = error;
            }
        });
    QObject::connect(&tool, &DnsDiagnosticTool::dnsCacheFlushed, &tool, [&flushed]() {
        flushed = true;
    });
    tool.flushDnsCache();
    if (!flushed) {
        return {false, QStringLiteral("DNS cache flush did not complete"), {}};
    }
    if (!error_text.isEmpty()) {
        return {false, error_text, {}};
    }
    return {true, QStringLiteral("Flushed the DNS resolver cache"), {}};
}

// ---------------------------------------------------------------------------
// network.connect_wifi
// ---------------------------------------------------------------------------
// Install a WLAN profile and connect to a network NOW via the shared sak::connectWifiWindows, which
// runs `netsh wlan add profile` + `netsh wlan connect` shell-free (argv, no interpolation). netsh
// needs administrator rights, so a non-elevated run fails HONESTLY (like the adapter-config ops).
// Mutating + requires_admin; NOT destructive (installs a reversible profile + associates -- no data
// loss) and NOT catastrophic. The durable outcome is the installed profile; the immediate
// association may be pending / out of range and is reported separately.
// Accept only the security types the schema advertises (case-insensitive), plus an omitted/empty
// value (the documented default, WPA2-PSK). A present-but-UNKNOWN security, a non-string security,
// or a non-bool hidden is REFUSED here rather than silently coerced -- a mistyped security must
// fail closed, not install a wrong-auth profile that reports success. Shared shape with the
// read-only generate_wifi_setup_script guard.
std::optional<AppActionResult> wifiConnectArgsError(const QJsonObject& args) {
    if (args.contains(QStringLiteral("hidden")) && !args.value(QStringLiteral("hidden")).isBool()) {
        return AppActionResult{false,
                               QStringLiteral("hidden must be a boolean (true or false)"),
                               {}};
    }
    if (!args.contains(QStringLiteral("security"))) {
        return std::nullopt;
    }
    const QJsonValue sec = args.value(QStringLiteral("security"));
    if (!sec.isString()) {
        return AppActionResult{false,
                               QStringLiteral("security must be a string (wpa2, wep, or open)"),
                               {}};
    }
    const QString v = sec.toString().trimmed().toLower();
    if (v.isEmpty() || v == QLatin1String("wpa2") || v == QLatin1String("wep") ||
        v == QLatin1String("open")) {
        return std::nullopt;
    }
    return AppActionResult{false, QStringLiteral("security must be one of wpa2, wep, or open"), {}};
}

AppActionResult connectWifi(const QJsonObject& args) {
    const QString ssid = args.value(QStringLiteral("ssid")).toString();
    if (ssid.trimmed().isEmpty()) {
        return {false, QStringLiteral("connect_wifi requires a non-empty 'ssid'"), {}};
    }
    if (const std::optional<AppActionResult> error = wifiConnectArgsError(args)) {
        return *error;
    }
    const QString password = args.value(QStringLiteral("password")).toString();
    const QString security = args.value(QStringLiteral("security")).toString();
    const bool hidden = args.value(QStringLiteral("hidden")).toBool(false);

    const sak::WifiConnectResult res = sak::connectWifiWindows(ssid, password, security, hidden);
    QJsonObject data{{QStringLiteral("ssid"), ssid},
                     {QStringLiteral("profile_added"), res.profile_added},
                     {QStringLiteral("connect_issued"), res.connect_issued}};
    if (!res.profile_added) {
        return {false, QStringLiteral("Could not connect to '%1': %2").arg(ssid, res.error), data};
    }
    if (res.connect_issued) {
        return {true, QStringLiteral("Connected to '%1' (WLAN profile installed)").arg(ssid), data};
    }
    return {true,
            QStringLiteral(
                "Installed the WLAN profile for '%1'; it will connect when in range (%2)")
                .arg(ssid, res.error),
            data};
}

QJsonObject connectWifiParamsSchema() {
    QJsonObject security_prop{
        {QStringLiteral("type"), QStringLiteral("string")},
        {QStringLiteral("enum"),
         QJsonArray{QStringLiteral("wpa2"), QStringLiteral("wep"), QStringLiteral("open")}},
        {QStringLiteral("description"),
         QStringLiteral("Security type: wpa2 (the default when omitted), wep, or open. Any other "
                        "value is refused rather than assumed.")}};
    QJsonObject hidden_prop{{QStringLiteral("type"), QStringLiteral("boolean")},
                            {QStringLiteral("description"),
                             QStringLiteral("True if the network does not broadcast its SSID")}};
    QJsonObject properties{
        {QStringLiteral("ssid"), stringProp(QStringLiteral("Network name (SSID) to connect to"))},
        {QStringLiteral("password"),
         stringProp(QStringLiteral("WiFi passphrase (omit/empty for an open network)"))},
        {QStringLiteral("security"), security_prop},
        {QStringLiteral("hidden"), hidden_prop}};
    return QJsonObject{{QStringLiteral("type"), QStringLiteral("object")},
                       {QStringLiteral("properties"), properties},
                       {QStringLiteral("required"), QJsonArray{QStringLiteral("ssid")}},
                       {QStringLiteral("additionalProperties"), false}};
}

// Register the network-mutating ops. Split out to keep registerMutatingAppActionsInto within the
// length budget.
void registerNetworkMutatingOps(const AddMutatingActionFn& add) {
    AppActionDescriptor dhcp = mutatingDescriptor(
        QStringLiteral("network.set_adapter_dhcp"),
        QStringLiteral("Set an adapter to DHCP"),
        QStringLiteral("Reset a network adapter (Ethernet or Wi-Fi) to automatic (DHCP) IPv4 and "
                       "DNS; briefly drops that adapter's connectivity"),
        QStringLiteral("network"));
    dhcp.params_schema = setAdapterDhcpParamsSchema();
    dhcp.requires_admin = true;  // netsh set needs elevation; reversible so NOT destructive
    add(dhcp, setAdapterDhcp);

    // network.set_adapter_static_ip: assign a manual IPv4 address/mask/gateway/DNS. Like the DHCP
    // reset it is a reversible config change (no data loss -> NOT destructive) but needs elevation.
    // A wrong value can drop the adapter's connectivity, but set_adapter_dhcp reverts it; NOT
    // catastrophic. The panel gate still fires (chat refuses; Assisted confirms with the adapter).
    AppActionDescriptor static_ip = mutatingDescriptor(
        QStringLiteral("network.set_adapter_static_ip"),
        QStringLiteral("Set a static IP on an adapter"),
        QStringLiteral("Assign a static IPv4 address, subnet mask, optional gateway and DNS to a "
                       "network adapter (Ethernet or Wi-Fi); a wrong value can drop that adapter's "
                       "connectivity until reverted (use set_adapter_dhcp to revert)"),
        QStringLiteral("network"));
    static_ip.params_schema = setAdapterStaticIpParamsSchema();
    static_ip.requires_admin = true;  // netsh set needs elevation; reversible so NOT destructive
    add(static_ip, setAdapterStaticIp);

    // network.set_adapter_dns: set only the DNS servers, leaving the adapter's IP mode alone.
    // Reversible (set_adapter_dhcp returns DNS to automatic) so NOT destructive; needs elevation.
    AppActionDescriptor dns = mutatingDescriptor(
        QStringLiteral("network.set_adapter_dns"),
        QStringLiteral("Set an adapter's DNS servers"),
        QStringLiteral("Set a network adapter's IPv4 DNS servers (Ethernet or Wi-Fi) to a specific "
                       "list, leaving its IP configuration unchanged; reversible with "
                       "set_adapter_dhcp"),
        QStringLiteral("network"));
    dns.params_schema = setAdapterDnsParamsSchema();
    dns.requires_admin = true;  // netsh set needs elevation; reversible so NOT destructive
    add(dns, setAdapterDns);

    // network.flush_dns: clears the local DNS resolver cache -> mutating (a system-cache change)
    // but NOT requires_admin (ipconfig /flushdns needs no elevation) and NOT destructive (the cache
    // is transient and repopulates). The gate fires at the Assisted-confirm tier.
    AppActionDescriptor flush = mutatingDescriptor(
        QStringLiteral("network.flush_dns"),
        QStringLiteral("Flush the DNS cache"),
        QStringLiteral("Clear the local DNS resolver cache (ipconfig /flushdns); forces the next "
                       "lookup to re-resolve. No elevation, no data loss."),
        QStringLiteral("network"));
    flush.params_schema = flushDnsParamsSchema();
    add(flush, flushDns);

    // network.connect_wifi: installs a WLAN profile + connects NOW (netsh, shell-free). Needs admin
    // (a non-elevated run fails honestly) -> requires_admin. NOT destructive (a reversible profile
    // + association, no data loss) and NOT catastrophic; the panel gate still fires (chat refuses).
    AppActionDescriptor connect = mutatingDescriptor(
        QStringLiteral("network.connect_wifi"),
        QStringLiteral("Connect to a WiFi network"),
        QStringLiteral("Install a WLAN profile and connect to a WiFi network now (netsh); needs "
                       "administrator rights"),
        QStringLiteral("network"));
    connect.params_schema = connectWifiParamsSchema();
    connect.requires_admin = true;
    add(connect, connectWifi);
}

// ---------------------------------------------------------------------------
// email.convert_ost
// ---------------------------------------------------------------------------
// Convert an OST/PST mail store to a folder of files, driving the app's OWN OstConversionWorker
// (the OST Converter feature). The source is read through the fan-out-hardened PstParser; the
// worker sanitizes each folder segment (sanitizeFolderSegment) so a crafted folder name cannot
// escape the output root, and writes only into the required-new/empty output_directory. ImapUpload
// (a network egress "format") is NOT selectable -- only file formats are accepted. Mutating, not
// destructive (adds files into a new/empty dir), no elevation.

// Bound the OST/PST size a headless conversion will open (PstParser parses all node metadata up
// front; larger stores use the GUI). Matches the export cap.
constexpr qint64 kMaxConvertSourceBytes = 2LL * 1024 * 1024 * 1024;

// Map the model-facing format string to a FILE OstOutputFormat. Returns nullopt for imap_upload
// (network) or any unknown value, so the op refuses it -- no network egress path is ever taken.
std::optional<OstOutputFormat> convertFormatFromArg(const QString& value) {
    const QString v = value.trimmed().toLower();
    if (v == QLatin1String("pst")) {
        return OstOutputFormat::Pst;
    }
    if (v == QLatin1String("eml")) {
        return OstOutputFormat::Eml;
    }
    if (v == QLatin1String("msg")) {
        return OstOutputFormat::Msg;
    }
    if (v == QLatin1String("mbox")) {
        return OstOutputFormat::Mbox;
    }
    if (v == QLatin1String("dbx")) {
        return OstOutputFormat::Dbx;
    }
    if (v == QLatin1String("html")) {
        return OstOutputFormat::Html;
    }
    if (v == QLatin1String("pdf")) {
        return OstOutputFormat::Pdf;
    }
    return std::nullopt;
}

// Map a completed conversion into the tool result. Honest: a hard error (folder read / open
// failure, captured in result.errors) is a failure; otherwise success requires at least one item
// written, and any per-item failures are surfaced in the message + payload (never hidden).
AppActionResult buildConvertResult(const OstConversionResult& result,
                                   const QString& format,
                                   const QString& output_dir,
                                   const QString& error_text) {
    QJsonObject data{{QStringLiteral("output_directory"), output_dir},
                     {QStringLiteral("format"), format.trimmed().toLower()},
                     {QStringLiteral("items_converted"), result.items_converted},
                     {QStringLiteral("items_failed"), result.items_failed},
                     {QStringLiteral("items_recovered"), result.items_recovered},
                     {QStringLiteral("folders_processed"), result.folders_processed},
                     {QStringLiteral("bytes_written"), static_cast<double>(result.bytes_written)},
                     {QStringLiteral("pst_volumes_created"), result.pst_volumes_created},
                     {QStringLiteral("error_count"), static_cast<int>(result.errors.size())},
                     {QStringLiteral("errors"), jsonStringArrayCapped(result.errors, 50)}};
    // Honest + export-aligned (buildExportResult): a run that WROTE files is a success even if some
    // non-fatal item errors were logged (a dropped attachment, one unreadable folder) -- those are
    // surfaced as warnings in the payload + message, NOT treated as total failure. Failure only
    // when nothing was written.
    const int error_count = static_cast<int>(result.errors.size());
    if (result.items_converted > 0) {
        QString message = QStringLiteral("Converted %1 item(s) to %2 in %3")
                              .arg(result.items_converted)
                              .arg(format.trimmed().toLower(), output_dir);
        if (result.items_failed > 0 || error_count > 0) {
            message += QStringLiteral(" (%1 failed, %2 warning(s))")
                           .arg(result.items_failed)
                           .arg(error_count);
        }
        return {true, message, data};
    }
    // Nothing was written -> an honest failure (a source-open/writer-init failure, an all-failed
    // run, or an empty store): the caller must never mistake an empty output for a completed
    // conversion.
    QString reason;
    if (!result.errors.isEmpty()) {
        reason = result.errors.first();
    } else if (!error_text.isEmpty()) {
        // A source-open / writer-init failure emits errorOccurred but may leave result.errors
        // empty.
        reason = error_text;
    } else {
        reason = QStringLiteral("no items were converted");
    }
    return {false, QStringLiteral("Conversion failed: %1").arg(reason), data};
}

AppActionResult convertOst(const QJsonObject& args) {
    const QString path = args.value(QStringLiteral("path")).toString().trimmed();
    const QString output_dir = args.value(QStringLiteral("output_directory")).toString().trimmed();
    if (const std::optional<AppActionResult> error =
            validateExportPaths(path,
                                output_dir,
                                kMaxConvertSourceBytes,
                                ExportPathLabels{QStringLiteral("convert_ost"),
                                                 QStringLiteral("OST/PST"),
                                                 QStringLiteral("path"),
                                                 QStringLiteral("output_directory"),
                                                 QStringLiteral("conversion")})) {
        return *error;
    }

    const std::optional<OstOutputFormat> format =
        convertFormatFromArg(args.value(QStringLiteral("format")).toString());
    if (!format) {
        return {false,
                QStringLiteral("format must be one of eml/mbox/html/pdf (direct IMAP upload is not "
                               "available in this headless tool)"),
                {}};
    }
    // Refuse gated-off formats up front with a precise reason instead of letting the
    // worker attempt and fail: PST/MSG/DBX writers are not spec-conformant, so their
    // output cannot be opened by any reader (B7-12).
    if (!isOutputFormatSupported(*format)) {
        return {false,
                QStringLiteral("PST/MSG/DBX output is not supported (no spec-conformant writer); "
                               "use eml, mbox, html, or pdf"),
                {}};
    }

    OstConversionConfig config;
    config.format = *format;
    config.output_directory = output_dir;
    config.recover_deleted_items = args.value(QStringLiteral("recover_deleted")).toBool(false);
    // Every other field keeps its default (no folder/sender filters, no PST split, preserve
    // structure). No ImapServerConfig is populated; format != ImapUpload guarantees the worker
    // never takes a network-upload path.

    // OstConversionWorker::convert runs synchronously and emits conversionFinished inline on THIS
    // thread (open/writer-init failures too), so a direct connection captures the result without
    // any event loop -- the export_mbox pattern.
    OstConversionWorker worker;
    OstConversionResult captured;
    bool completed = false;
    QString error_text;
    QObject::connect(&worker,
                     &OstConversionWorker::conversionFinished,
                     &worker,
                     [&captured, &completed](const OstConversionResult& result) {
                         captured = result;
                         completed = true;
                     });
    QObject::connect(&worker,
                     &OstConversionWorker::errorOccurred,
                     &worker,
                     [&error_text](const QString& error) { error_text = error; });
    worker.convert(path, config);

    if (!completed) {
        return {false,
                error_text.isEmpty() ? QStringLiteral("Conversion did not complete") : error_text,
                {}};
    }
    return buildConvertResult(
        captured, args.value(QStringLiteral("format")).toString(), output_dir, error_text);
}

QJsonObject convertOstParamsSchema() {
    QJsonObject format_prop{{QStringLiteral("type"), QStringLiteral("string")},
                            {QStringLiteral("enum"),
                             QJsonArray{QStringLiteral("pst"),
                                        QStringLiteral("eml"),
                                        QStringLiteral("msg"),
                                        QStringLiteral("mbox"),
                                        QStringLiteral("dbx"),
                                        QStringLiteral("html"),
                                        QStringLiteral("pdf")}},
                            {QStringLiteral("description"),
                             QStringLiteral("Output format to convert the store into")}};
    QJsonObject recover_prop{
        {QStringLiteral("type"), QStringLiteral("boolean")},
        {QStringLiteral("description"),
         QStringLiteral(
             "Also recover and convert deleted items found in the store (default false)")}};
    QJsonObject properties{
        {QStringLiteral("path"),
         stringProp(QStringLiteral("Absolute path to the source OST or PST file"))},
        {QStringLiteral("output_directory"),
         stringProp(QStringLiteral("New or empty directory to write the converted files into"))},
        {QStringLiteral("format"), format_prop},
        {QStringLiteral("recover_deleted"), recover_prop}};
    return QJsonObject{{QStringLiteral("type"), QStringLiteral("object")},
                       {QStringLiteral("properties"), properties},
                       {QStringLiteral("required"),
                        QJsonArray{QStringLiteral("path"),
                                   QStringLiteral("output_directory"),
                                   QStringLiteral("format")}},
                       {QStringLiteral("additionalProperties"), false}};
}

// Register the email export ops. Split out to keep registerMutatingAppActionsInto within the
// length budget.
void registerEmailMutatingOps(const AddMutatingActionFn& add) {
    // email.export_mbox: adds files under output_path, never overwrites/deletes, no elevation ->
    // mutating but not destructive and not admin.
    AppActionDescriptor export_mbox = mutatingDescriptor(
        QStringLiteral("email.export_mbox"),
        QStringLiteral("Export MBOX messages"),
        QStringLiteral("Export messages from an MBOX file to eml/html/text/pdf files"),
        QStringLiteral("email"));
    export_mbox.params_schema = exportMboxParamsSchema();
    add(export_mbox, exportMbox);

    // email.export_pst: same as export_mbox but the source is a PST/OST store (Outlook's format),
    // driving EmailExportWorker::exportItems. Adds files under a new/empty output_path, never
    // overwrites/deletes, no elevation -> mutating but not destructive and not admin.
    AppActionDescriptor export_pst = mutatingDescriptor(
        QStringLiteral("email.export_pst"),
        QStringLiteral("Export PST/OST messages"),
        QStringLiteral("Export messages from a PST/OST file to eml/html/text/pdf files"),
        QStringLiteral("email"));
    export_pst.params_schema = exportPstParamsSchema();
    add(export_pst, exportPst);

    // email.save_mbox_attachments: extracts a message's attachments into an EXISTING directory via
    // the app's OWN saver (sanitize + dedupe + atomic write). Only ADDS files (dedupe never
    // overwrites), so mutating but not destructive; no elevation.
    AppActionDescriptor save_attach = mutatingDescriptor(
        QStringLiteral("email.save_mbox_attachments"),
        QStringLiteral("Save MBOX attachments"),
        QStringLiteral("Save all attachments of one MBOX message to an existing directory"),
        QStringLiteral("email"));
    save_attach.params_schema = saveMboxAttachmentsParamsSchema();
    add(save_attach, saveMboxAttachments);

    // email.save_pst_attachments: PST/OST pair to save_mbox_attachments. Same saver + gate tier;
    // reads a message node's attachments through the fan-out-hardened PstParser (whose name/byte
    // enumerations are deliberately index-aligned). Only ADDS files, so mutating not destructive.
    AppActionDescriptor save_pst_attach = mutatingDescriptor(
        QStringLiteral("email.save_pst_attachments"),
        QStringLiteral("Save PST/OST attachments"),
        QStringLiteral("Save all attachments of one PST/OST message to an existing directory"),
        QStringLiteral("email"));
    save_pst_attach.params_schema = savePstAttachmentsParamsSchema();
    add(save_pst_attach, savePstAttachments);

    // email.convert_ost: converts an OST/PST store to a directory of files via the app's OWN
    // OstConversionWorker. Reads through the fan-out-hardened PstParser; the worker sanitizes
    // folder segments and writes only into the required-new/empty output_directory. Direct IMAP
    // upload is not selectable. Only ADDS files -> mutating, not destructive; no elevation.
    AppActionDescriptor convert = mutatingDescriptor(
        QStringLiteral("email.convert_ost"),
        QStringLiteral("Convert an OST/PST store"),
        QStringLiteral(
            "Convert an OST/PST mail store to a directory of files (pst/eml/msg/mbox/dbx/"
            "html/pdf)"),
        QStringLiteral("email"));
    convert.params_schema = convertOstParamsSchema();
    add(convert, convertOst);
}

// files.compress_zip / files.extract_zip: bound the model-facing blocker/warning lists and the
// input archive size (the extraction resource ceilings live inside the engine).
constexpr int kMaxArchiveMessages = 50;
constexpr int kMaxCompressSources = 4096;
constexpr qint64 kMaxArchiveInputBytes = 4LL * 1024 * 1024 * 1024;  // 4 GiB zip input

QJsonArray clampStringList(const QStringList& values, int max) {
    QJsonArray out;
    for (const QString& value : values) {
        if (out.size() >= max) {
            break;
        }
        out.append(value);
    }
    return out;
}

// Map an archive engine result into the tool result. The engine's own ok flag + blockers/warnings
// are the value channel (a failed op reports ok=false with the reason), so this never invents
// success: a compress/extract that hit any blocker is a failure.
AppActionResult buildArchiveResult(const FileExplorerArchiveResult& res, const QString& verb) {
    QJsonObject data{
        {QStringLiteral("output_path"), res.output_path},
        {QStringLiteral("entries"), res.entries},
        {QStringLiteral("blocker_count"), static_cast<int>(res.blockers.size())},
        {QStringLiteral("warning_count"), static_cast<int>(res.warnings.size())},
        {QStringLiteral("blockers"), clampStringList(res.blockers, kMaxArchiveMessages)},
        {QStringLiteral("warnings"), clampStringList(res.warnings, kMaxArchiveMessages)}};
    if (res.ok) {
        return {true,
                QStringLiteral("%1 succeeded: %2 entr%3")
                    .arg(verb)
                    .arg(res.entries)
                    .arg(res.entries == 1 ? QStringLiteral("y") : QStringLiteral("ies")),
                data};
    }
    const QString reason = res.blockers.isEmpty() ? QStringLiteral("unknown error")
                                                  : res.blockers.first();
    return {false, QStringLiteral("%1 failed: %2").arg(verb, reason), data};
}

QStringList compressSourcesFromArgs(const QJsonObject& args) {
    QStringList sources;
    for (const QJsonValue& value : args.value(QStringLiteral("sources")).toArray()) {
        const QString path = value.toString().trimmed();
        if (!path.isEmpty()) {
            sources.append(path);
        }
    }
    return sources;
}

// Reject any compress source that is a network/UNC/device path (SMB/NTLM leak) or does not exist.
std::optional<AppActionResult> validateCompressSources(const QStringList& sources) {
    for (const QString& source : sources) {
        if (isNetworkOrDevicePath(source)) {
            return AppActionResult{
                false,
                QStringLiteral("compress_zip does not allow network/UNC or device sources: %1")
                    .arg(source),
                {}};
        }
        // Screen the leaf AND ancestors for a reparse point BEFORE the target-following exists(): a
        // symlink/junction -- at the source OR any ancestor -- to a UNC share would otherwise leak
        // the NTLM hash here (the engine safely skips symlink sources, but this validation runs
        // first).
        const QFileInfo info(source);
        if (pathReparseUnsafe(source)) {
            return AppActionResult{false,
                                   QStringLiteral("compress_zip does not allow a symlink/junction "
                                                  "source (or one in its path): %1")
                                       .arg(source),
                                   {}};
        }
        if (!info.exists()) {
            return AppActionResult{false, QStringLiteral("No such source: %1").arg(source), {}};
        }
    }
    return std::nullopt;
}

// Validate files.compress_zip inputs. Rejects network/UNC/device paths (SMB/NTLM leak), a
// non-.zip or already-existing output (compressToZip clobbers, and REMOVES the file on failure --
// so an existing file at zip_path is a data-loss hazard), a missing output parent dir, and any
// source that does not exist.
std::optional<AppActionResult> validateCompressInputs(const QStringList& sources,
                                                      const QString& zip_path) {
    if (sources.isEmpty() || zip_path.isEmpty()) {
        return AppActionResult{false,
                               QStringLiteral("compress_zip requires 'sources' and 'output_path'"),
                               {}};
    }
    if (sources.size() > kMaxCompressSources) {
        return AppActionResult{
            false,
            QStringLiteral("compress_zip accepts at most %1 sources").arg(kMaxCompressSources),
            {}};
    }
    if (isNetworkOrDevicePath(zip_path)) {
        return AppActionResult{
            false, QStringLiteral("compress_zip does not allow network/UNC or device paths"), {}};
    }
    if (!FileExplorerArchiveService::isZipName(zip_path)) {
        return AppActionResult{false, QStringLiteral("output_path must end in .zip"), {}};
    }
    const QFileInfo zip_info(zip_path);
    // Screen the leaf AND ancestors before exists() (which follows a symlink/junction target -- at
    // the leaf OR any ancestor -- off-box, leaking the NTLM hash).
    if (pathReparseUnsafe(zip_path)) {
        return AppActionResult{
            false,
            QStringLiteral("output_path must not be a symlink or junction (or under one): %1")
                .arg(zip_path),
            {}};
    }
    if (zip_info.exists()) {
        return AppActionResult{
            false,
            QStringLiteral("output_path already exists (refusing to overwrite): %1").arg(zip_path),
            {}};
    }
    if (!QFileInfo(zip_info.absolutePath()).isDir()) {
        return AppActionResult{
            false,
            QStringLiteral("output_path's parent directory does not exist: %1").arg(zip_path),
            {}};
    }
    return validateCompressSources(sources);
}

// Compress local files/folders into a new .zip. Drives the app's OWN FileExplorerArchiveService
// (static, synchronous, symlink-skipping, entry-capped). Mutating but not destructive: writes ONE
// new archive that must not already exist, never touches the sources.
AppActionResult compressZip(const QJsonObject& args) {
    const QStringList sources = compressSourcesFromArgs(args);
    const QString zip_path = args.value(QStringLiteral("output_path")).toString().trimmed();
    if (auto fail = validateCompressInputs(sources, zip_path)) {
        return *fail;
    }
    return buildArchiveResult(FileExplorerArchiveService::compressToZip(zip_path, sources),
                              QStringLiteral("Compress"));
}

// Extract a .zip into a NEW or EMPTY directory. Drives the app's OWN FileExplorerArchiveService,
// which is already hardened against zip-slip (absolute/traversal entries refused), pre-planted
// junction redirection, symlink entries, and zip-bombs (per-file/total-byte + entry-count
// ceilings). Mutating but not destructive: the new/empty-dir guard means nothing pre-existing is
// clobbered.
AppActionResult extractZip(const QJsonObject& args) {
    const QString archive_path = args.value(QStringLiteral("archive_path")).toString().trimmed();
    const QString destination_dir =
        args.value(QStringLiteral("destination_dir")).toString().trimmed();
    if (auto fail = validateExportPaths(archive_path,
                                        destination_dir,
                                        kMaxArchiveInputBytes,
                                        ExportPathLabels{QStringLiteral("extract_zip"),
                                                         QStringLiteral("archive"),
                                                         QStringLiteral("archive_path"),
                                                         QStringLiteral("destination_dir"),
                                                         QStringLiteral("extraction")})) {
        return *fail;
    }
    return buildArchiveResult(FileExplorerArchiveService::extractZip(archive_path, destination_dir),
                              QStringLiteral("Extract"));
}

QJsonObject compressZipParamsSchema() {
    QJsonObject sources_prop{
        {QStringLiteral("type"), QStringLiteral("array")},
        {QStringLiteral("description"),
         QStringLiteral("Absolute paths of the files/folders to add (folders are recursed)")},
        {QStringLiteral("items"), QJsonObject{{QStringLiteral("type"), QStringLiteral("string")}}}};
    QJsonObject properties{
        {QStringLiteral("sources"), sources_prop},
        {QStringLiteral("output_path"),
         stringProp(QStringLiteral("Absolute path of the .zip to create (must not exist)"))}};
    return QJsonObject{{QStringLiteral("type"), QStringLiteral("object")},
                       {QStringLiteral("properties"), properties},
                       {QStringLiteral("required"),
                        QJsonArray{QStringLiteral("sources"), QStringLiteral("output_path")}},
                       {QStringLiteral("additionalProperties"), false}};
}

QJsonObject extractZipParamsSchema() {
    QJsonObject properties{
        {QStringLiteral("archive_path"),
         stringProp(QStringLiteral("Absolute path to the .zip archive to extract"))},
        {QStringLiteral("destination_dir"),
         stringProp(QStringLiteral("Absolute path of a NEW or EMPTY directory to extract "
                                   "into"))}};
    return QJsonObject{{QStringLiteral("type"), QStringLiteral("object")},
                       {QStringLiteral("properties"), properties},
                       {QStringLiteral("required"),
                        QJsonArray{QStringLiteral("archive_path"),
                                   QStringLiteral("destination_dir")}},
                       {QStringLiteral("additionalProperties"), false}};
}

QJsonObject deleteToRecycleBinParamsSchema() {
    QJsonObject properties{
        {QStringLiteral("path"),
         stringProp(QStringLiteral("Absolute path to the single file or directory to move to the "
                                   "Recycle Bin (no wildcards)"))}};
    return QJsonObject{{QStringLiteral("type"), QStringLiteral("object")},
                       {QStringLiteral("properties"), properties},
                       {QStringLiteral("required"), QJsonArray{QStringLiteral("path")}},
                       {QStringLiteral("additionalProperties"), false}};
}

// Validate a model-supplied recycle target. Returns an error result to short-circuit on, or nullopt
// when the path is safe to send to the Recycle Bin. Every check runs BEFORE the shell op so a
// prompt-injected path cannot cause an unintended delete or a credential leak:
//   - reject an embedded NUL -- sendPathToRecycleBin double-null-terminates pFrom, so a NUL would
//     split it into a MULTI-item delete list (deleting more than the validated item);
//   - reject '*'/'?' -- SHFileOperation EXPANDS wildcards, so one call could delete many files;
//   - require an ABSOLUTE path -- a relative one resolves against the process CWD (unknown target);
//   - reject UNC/device + a leaf-or-ancestor symlink/junction (reparse -> NTLM leak, or off-tree);
//   - resolve to the OS-canonical path (what the shell actually acts on) and refuse a drive/volume
//     root. Validating the canonical string -- not the lexical one -- closes the mismatch where a
//     trailing-dot/space form ("C:\\...", "C:\\ .") survives a lexical isRoot() check yet Win32
//     canonicalizes it to the volume root.
// @param canonical_out On success, the OS-canonical path to hand to sendPathToRecycleBin, so the op
//        validates and deletes the SAME string.
#ifdef Q_OS_WIN
// Real on-disk path of @p path with every ancestor junction/symlink resolved, WITHOUT following a
// leaf reparse point, read through an open handle -- mirrors cleanup_worker's delete-time check.
// Empty when the object cannot be opened (locked/raced/denied) or the name cannot be read, which
// the caller treats as fail-closed.
QString recycleFinalPathByHandle(const QString& path) {
    const HANDLE handle = CreateFileW(reinterpret_cast<LPCWSTR>(path.utf16()),
                                      FILE_READ_ATTRIBUTES,
                                      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                      nullptr,
                                      OPEN_EXISTING,
                                      FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS,
                                      nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        return {};
    }
    std::wstring buffer(1024, L'\0');
    const DWORD flags = FILE_NAME_NORMALIZED | VOLUME_NAME_DOS;
    DWORD length =
        GetFinalPathNameByHandleW(handle, buffer.data(), static_cast<DWORD>(buffer.size()), flags);
    if (length > buffer.size()) {
        buffer.resize(length);
        length = GetFinalPathNameByHandleW(
            handle, buffer.data(), static_cast<DWORD>(buffer.size()), flags);
    }
    CloseHandle(handle);
    if (length == 0 || length > buffer.size()) {
        return {};
    }
    QString resolved = QString::fromWCharArray(buffer.data(), static_cast<int>(length));
    if (resolved.startsWith(QStringLiteral("\\\\?\\UNC\\"))) {
        return QStringLiteral("\\\\") + resolved.mid(8);
    }
    if (resolved.startsWith(QStringLiteral("\\\\?\\"))) {
        return resolved.mid(4);
    }
    return resolved;
}
#endif  // Q_OS_WIN

// Delete-TIME by-handle re-verification for the recycle op. filePathDeletionRefusal screened the
// path STRING at validate time, but SHFileOperationW acts on the path by NAME later, re-resolving
// it -- a local attacker who swaps an ANCESTOR directory into a junction between validate and the
// shell call makes the same string resolve to a DIFFERENT real target. Immediately before the shell
// op, open a handle to the canonical path and read its REAL resolved name, then refuse when it
// cannot be resolved, lands in a protected/critical/root/UNC location, or no longer matches the
// validated path. Mirrors cleanupHandleRedirectRefusal used by the permanent-delete worker. No-op
// on non-Windows.
std::optional<AppActionResult> recycleHandleRedirectRefusal(const QString& canonical) {
#ifdef Q_OS_WIN
    const QString final_path = recycleFinalPathByHandle(canonical);
    const QString refusal = cleanupHandleRedirectRefusal(canonical, final_path);
    if (!refusal.isEmpty()) {
        return AppActionResult{false,
                               QStringLiteral("Refusing to recycle %1: %2").arg(canonical, refusal),
                               {}};
    }
#else
    Q_UNUSED(canonical)
#endif
    return std::nullopt;
}

// Screen the RESOLVED (canonical) recycle target: refuse a drive/volume root, and route it through
// the shared file-deletion denylist so this generic recycle op refuses the same protected locations
// the leftover-cleanup path does (the Windows system tree, boot/system-critical paths, and shared
// system/user ROOT directories -- Program Files, Users, ProgramData, ...). Screening the canonical
// (not lexical) path closes trailing-dot/alias evasion. Split out so validateRecycleTarget stays
// within the complexity budget.
std::optional<AppActionResult> canonicalRecycleRefusal(const QString& canonical) {
    if (QDir(canonical).isRoot()) {
        return AppActionResult{
            false,
            QStringLiteral("Refusing to recycle a drive/volume root: %1").arg(canonical),
            {}};
    }
    const QString protectedRefusal = filePathDeletionRefusal(canonical);
    if (!protectedRefusal.isEmpty()) {
        return AppActionResult{false,
                               QStringLiteral("Refusing to recycle a protected location (%1): %2")
                                   .arg(protectedRefusal, canonical),
                               {}};
    }
    return std::nullopt;
}

std::optional<AppActionResult> validateRecycleTarget(const QString& path, QString& canonical_out) {
    if (path.isEmpty()) {
        return AppActionResult{false,
                               QStringLiteral("delete_to_recycle_bin requires a 'path' argument"),
                               {}};
    }
    if (path.contains(QChar(QChar::Null))) {
        return AppActionResult{false,
                               QStringLiteral(
                                   "delete_to_recycle_bin path must not contain a null character"),
                               {}};
    }
    if (path.contains(QLatin1Char('*')) || path.contains(QLatin1Char('?'))) {
        return AppActionResult{
            false,
            QStringLiteral("delete_to_recycle_bin does not allow wildcard paths ('*' or '?'); pass "
                           "one exact file or directory"),
            {}};
    }
    if (isNetworkOrDevicePath(path)) {
        return AppActionResult{
            false,
            QStringLiteral("delete_to_recycle_bin does not allow network/UNC or device paths"),
            {}};
    }
    const QFileInfo info(path);
    if (!info.isAbsolute()) {
        return AppActionResult{false,
                               QStringLiteral("path must be an absolute path: %1").arg(path),
                               {}};
    }
    // Reparse screen BEFORE canonicalFilePath() (a following resolve): a symlink/junction to a UNC
    // target would leak the NTLM hash on the resolve.
    if (pathReparseUnsafe(path)) {
        return AppActionResult{
            false,
            QStringLiteral("path must not be a symlink or junction (or under one): %1").arg(path),
            {}};
    }
    // canonicalFilePath() returns the real on-disk path (trailing dots/spaces stripped, "."/".."
    // resolved) or empty if it does not exist -- this replaces the exists() check AND matches what
    // SHFileOperationW canonicalizes the path to, so the root check below cannot be lexically
    // evaded.
    const QString canonical = info.canonicalFilePath();
    if (canonical.isEmpty()) {
        return AppActionResult{false,
                               QStringLiteral("No such file or directory: %1").arg(path),
                               {}};
    }
    if (const std::optional<AppActionResult> refused = canonicalRecycleRefusal(canonical)) {
        return refused;
    }
    canonical_out = canonical;
    return std::nullopt;
}

// files.delete_to_recycle_bin: move ONE file or directory to the Windows Recycle Bin via the app's
// OWN sendPathToRecycleBin (SHFileOperationW FO_DELETE + FOF_ALLOWUNDO -- recoverable). Marked
// destructive so the panel gate requires a human confirm. All path guards live in
// validateRecycleTarget above, which also yields the canonical path the delete acts on.
AppActionResult deleteToRecycleBin(const QJsonObject& args) {
    const QString path = args.value(QStringLiteral("path")).toString().trimmed();
    QString canonical;
    if (const std::optional<AppActionResult> invalid = validateRecycleTarget(path, canonical)) {
        return *invalid;
    }
    const QFileInfo info(canonical);
    // Capture the type BEFORE the delete -- after it, the item is gone.
    const bool was_directory = info.isDir();
    const QString name = info.fileName();
    // Re-verify by-handle IMMEDIATELY before the shell op: the string checks above ran at validate
    // time, but SHFileOperationW re-resolves the name, so an ancestor junction/symlink swapped in
    // since would redirect the delete. Refuse if the real target no longer matches or is protected.
    if (const std::optional<AppActionResult> redirected = recycleHandleRedirectRefusal(canonical)) {
        return *redirected;
    }
    if (!sendPathToRecycleBin(canonical)) {
        return {false,
                QStringLiteral("Could not move %1 to the Recycle Bin (it may be in use, protected, "
                               "or on a volume with no Recycle Bin)")
                    .arg(name),
                {}};
    }
    QJsonObject data{{QStringLiteral("path"), canonical},
                     {QStringLiteral("was_directory"), was_directory},
                     {QStringLiteral("recycled"), true}};
    return {true, QStringLiteral("Moved %1 to the Recycle Bin").arg(name), data};
}

// Register the file-archive ops (zip compress/extract). Split out to keep
// registerMutatingAppActionsInto within the length budget.
void registerFileMutatingOps(const AddMutatingActionFn& add) {
    // files.compress_zip: writes ONE new .zip (guarded to not exist), never touches the sources ->
    // mutating, not destructive, no elevation.
    AppActionDescriptor compress = mutatingDescriptor(
        QStringLiteral("files.compress_zip"),
        QStringLiteral("Compress files into a .zip"),
        QStringLiteral("Create a new .zip archive from local files/folders (folders recursed)"),
        QStringLiteral("files"));
    compress.params_schema = compressZipParamsSchema();
    add(compress, compressZip);

    // files.extract_zip: extracts into a new/empty dir (zip-slip/bomb hardened in the engine),
    // never overwrites existing files -> mutating, not destructive, no elevation.
    AppActionDescriptor extract =
        mutatingDescriptor(QStringLiteral("files.extract_zip"),
                           QStringLiteral("Extract a .zip"),
                           QStringLiteral("Extract a .zip archive into a new or empty directory"),
                           QStringLiteral("files"));
    extract.params_schema = extractZipParamsSchema();
    add(extract, extractZip);

    // files.delete_to_recycle_bin: moves ONE file/dir to the Recycle Bin (recoverable) -> mutating
    // + destructive (the panel gate requires a human confirm). NOT catastrophic (recoverable,
    // single target) and no elevation (per-user bin). Wildcard/absolute/reparse/root guards are in
    // the thunk. Permanent only on a volume that has no Recycle Bin -- called out in the
    // description.
    AppActionDescriptor recycle = mutatingDescriptor(
        QStringLiteral("files.delete_to_recycle_bin"),
        QStringLiteral("Delete to Recycle Bin"),
        QStringLiteral(
            "Move one file or directory to the Windows Recycle Bin (recoverable from the "
            "bin; permanent if the volume has no Recycle Bin)"),
        QStringLiteral("files"));
    recycle.params_schema = deleteToRecycleBinParamsSchema();
    recycle.destructive = true;
    add(recycle, deleteToRecycleBin);
}

}  // namespace

std::optional<AppActionResult> validatePartitionApplyArgs(const QJsonObject& args) {
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
    return partitionApplyGateArgsError(args);
}

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

bool isSafePackageFullName(const QString& name) {
    // Appx names are ASCII alphanumerics plus '.' '_' '-' and, for a provisioned (DISM
    // PackageName) neutral ResourceId, '~' (e.g. Foo_1.0_neutral_~_8wekyb3d8bbwe). All are
    // inert inside a single-quoted PowerShell string. Reject anything else (quotes, spaces,
    // ';', backticks, '$', newlines) so the value can never break out of the -Package '<...>'
    // argument.
    static const QRegularExpression kPackageRe(QStringLiteral("\\A[A-Za-z0-9][A-Za-z0-9._~-]*\\z"));
    return !name.isEmpty() && name.size() <= 256 && kPackageRe.match(name).hasMatch();
}

std::optional<AppActionResult> resolveWin32ProgramFromList(const QVector<ProgramInfo>& programs,
                                                           const QString& name,
                                                           ProgramInfo& out) {
    const Win32MatchSummary summary = collectWin32Matches(programs, name);
    if (summary.distinct_commands.size() == 1) {
        out = summary.representative;
        return std::nullopt;
    }
    if (summary.distinct_commands.size() > 1) {
        // Two genuinely different programs share this display name (e.g. an x86/x64 pair or two
        // "Updater"s): refuse rather than silently remove the wrong one.
        return AppActionResult{false,
                               QStringLiteral("'%1' matches %2 distinct installed programs; use a "
                                              "more specific name or the GUI uninstall panel")
                                   .arg(name)
                                   .arg(summary.distinct_commands.size()),
                               {}};
    }
    // No silently-uninstallable match. Distinguish the reasons for an actionable message.
    if (summary.saw_system_component) {
        return AppActionResult{false,
                               QStringLiteral("'%1' is a protected Windows system component; "
                                              "uninstall it from the GUI panel, not headless")
                                   .arg(name),
                               {}};
    }
    if (!summary.saw_name) {
        return AppActionResult{false,
                               QStringLiteral("No installed Win32 program named '%1'").arg(name),
                               {}};
    }
    return AppActionResult{false,
                           QStringLiteral("'%1' has no silent uninstall command (its uninstaller "
                                          "is interactive and cannot run headless); use the GUI "
                                          "uninstall panel")
                               .arg(name),
                           {}};
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

// ---------------------------------------------------------------------------
// diagnostics.create_restore_point: create a System Restore checkpoint.
// ---------------------------------------------------------------------------

// The engine (RestorePointManager) silently truncates the label to 64 chars; enforce the same
// bound here (schema + op guard) so the op never reports a full description the engine shortened.
constexpr int kMaxRestorePointDescriptionChars = 64;

QJsonObject createRestorePointParamsSchema() {
    QJsonObject desc_prop{
        {QStringLiteral("type"), QStringLiteral("string")},
        {QStringLiteral("minLength"), 1},
        {QStringLiteral("maxLength"), kMaxRestorePointDescriptionChars},
        {QStringLiteral("description"),
         QStringLiteral(
             "Label for the restore point (1-64 chars), e.g. \"Before driver update\"")}};
    return QJsonObject{{QStringLiteral("type"), QStringLiteral("object")},
                       {QStringLiteral("properties"),
                        QJsonObject{{QStringLiteral("description"), desc_prop}}},
                       {QStringLiteral("required"), QJsonArray{QStringLiteral("description")}},
                       {QStringLiteral("additionalProperties"), false}};
}

// Create a Windows System Restore checkpoint via RestorePointManager (the same engine the app uses
// to protect its own destructive ops). SYNC + bounded (a single Checkpoint-Computer PowerShell call
// with a 2-min ceiling in the engine). The model-supplied description is REQUIRED non-empty (the
// engine Q_ASSERTs on empty, which would abort a Debug build) and is escaped by the engine before
// it reaches PowerShell (single-quote doubling inside a single-quoted -Command). Mutating +
// requires_admin (Checkpoint-Computer needs elevation, checked inside the engine too); NOT
// destructive (it ADDS a recovery checkpoint, removes nothing) and not catastrophic -- the panel
// gate fires at the Assisted-confirm tier. Honesty: a disabled System Restore, a missing elevation,
// the once-per-24h throttle, or any PowerShell failure are each reported as an honest failure via
// the captured restorePointFailed signal, never a false success.
AppActionResult createRestorePoint(const QJsonObject& args) {
    const QString description = args.value(QStringLiteral("description")).toString().trimmed();
    if (description.isEmpty()) {
        return {false,
                QStringLiteral("create_restore_point requires a non-empty 'description'"),
                {}};
    }
    if (description.size() > kMaxRestorePointDescriptionChars) {
        return {false,
                QStringLiteral("'description' must be %1 characters or fewer")
                    .arg(kMaxRestorePointDescriptionChars),
                {}};
    }

    RestorePointManager manager;
    if (!manager.isSystemRestoreEnabled()) {
        return {false,
                QStringLiteral("System Restore is disabled on this system; enable it before "
                               "creating a restore point"),
                {}};
    }

    QString error_text;
    QObject::connect(&manager,
                     &RestorePointManager::restorePointFailed,
                     &manager,
                     [&error_text](const QString& error) {
                         if (error_text.isEmpty()) {
                             error_text = error;
                         }
                     });

    if (!manager.createRestorePoint(description)) {
        return {false,
                error_text.isEmpty()
                    ? QStringLiteral("Failed to create restore point (creating one needs "
                                     "administrator rights; run S.A.K. elevated)")
                    : error_text,
                {}};
    }
    return {true,
            QStringLiteral("Created system restore point '%1'").arg(description),
            QJsonObject{{QStringLiteral("description"), description}}};
}

// Register the diagnostics-mutating ops. Split out to keep registerMutatingAppActionsInto within
// the length budget.
void registerDiagnosticsMutatingOps(const AddMutatingActionFn& add) {
    // diagnostics.create_restore_point: creates a System Restore checkpoint -> mutating +
    // requires_admin, but NOT destructive (it adds a recovery point, removes nothing) and not
    // catastrophic. The gate fires at the Assisted-confirm tier.
    AppActionDescriptor restore_point = mutatingDescriptor(
        QStringLiteral("diagnostics.create_restore_point"),
        QStringLiteral("Create a system restore point"),
        QStringLiteral("Create a Windows System Restore checkpoint (a recovery point to roll back "
                       "to); adds a checkpoint, changes no user data"),
        QStringLiteral("diagnostics"));
    restore_point.params_schema = createRestorePointParamsSchema();
    restore_point.requires_admin = true;  // Checkpoint-Computer needs elevation; additive, not
                                          // destructive
    add(restore_point, createRestorePoint);
}

// Register the imaging-mutating ops. Split out to keep registerMutatingAppActionsInto within the
// length budget.
void registerImagingMutatingOps(const AddMutatingActionFn& add) {
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

    // imaging.restore_recoverable: recover carved files (from imaging.scan_recoverable) out of an
    // offline disk-image FILE into a directory. Reads the image READ-ONLY (hashed before+after to
    // prove it was not mutated) and only ADDS files (overwrite forced off; derived output names) ->
    // mutating but NOT destructive, no elevation.
    AppActionDescriptor restore = mutatingDescriptor(
        QStringLiteral("imaging.restore_recoverable"),
        QStringLiteral("Recover files from a disk image"),
        QStringLiteral(
            "Recover carved files (from imaging.scan_recoverable) out of an offline "
            "disk-image file into a directory; reads the image read-only, only adds files"),
        QStringLiteral("imaging"));
    restore.params_schema = restoreRecoverableParamsSchema();
    add(restore, restoreRecoverable);
}

int registerMutatingAppActionsInto(AppActionRegistry& registry) {
    int registered = 0;
    const auto add = [&](const AppActionDescriptor& descriptor, AppActionInvoke invoke) {
        if (registry.registerAction(descriptor, std::move(invoke))) {
            ++registered;
        }
    };

    registerEmailMutatingOps(add);

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

    registerImagingMutatingOps(add);

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

    registerDiagnosticsMutatingOps(add);
    registerSoftwareOps(add);
    registerNetworkMutatingOps(add);
    registerFileMutatingOps(add);

    return registered;
}

}  // namespace sak
