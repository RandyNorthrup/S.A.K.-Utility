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
#include "sak/email_export_worker.h"
#include "sak/email_types.h"
#include "sak/mbox_parser.h"
#include "sak/organizer_worker.h"
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

}  // namespace

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

    return registered;
}

}  // namespace sak
