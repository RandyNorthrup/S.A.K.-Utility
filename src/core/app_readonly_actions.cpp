// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

// Read-only technician ops exposed to the AI assistant. Each invoke thunk calls
// an existing headless src/core service (no re-implementation) and serializes its
// result to a compact, model-facing QJsonObject. All are synchronous and run on
// the caller's worker thread -- no event loop, no GUI, no controller lifetime.

#include "sak/app_readonly_actions.h"

#include "sak/advanced_search_types.h"
#include "sak/advanced_search_worker.h"
#include "sak/app_action_guards.h"
#include "sak/app_action_registry.h"
#include "sak/app_action_service.h"
#include "sak/diagnostic_types.h"
#include "sak/error_codes.h"
#include "sak/hardware_inventory_scanner.h"
#include "sak/image_source.h"
#include "sak/mbox_parser.h"
#include "sak/partition_manager_types.h"
#include "sak/partition_operation_planner.h"
#include "sak/smart_disk_analyzer.h"
#include "sak/storage_inventory_worker.h"
#include "sak/vulnerability_scanner.h"
#include "sak/worker_base.h"

#include <QFileInfo>
#include <QJsonArray>
#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <QVector>

#include <algorithm>
#include <functional>
#include <limits>
#include <memory>
#include <optional>

namespace sak {

namespace {

// Bound model-facing list payloads so a machine with hundreds of programs or a
// large scan does not blow the tool-result size. Truncation is reported, never
// silent.
constexpr int kMaxListedPrograms = 500;
constexpr int kMaxReportedFindings = 200;
constexpr int kMaxInventoryWarnings = 200;
constexpr int kMaxPreviewMessages = 200;
constexpr int kMaxReportedMatches = 500;
constexpr int kMaxMatchLineChars = 400;
constexpr int kDefaultSearchMaxResults = 1000;
constexpr int kSearchMaxResultsCeiling = 5000;
constexpr int kDefaultMboxLimit = 200;
constexpr int kMboxLimitCeiling = 1000;
constexpr int kMaxHeaderChars = 1000;

QString imageFormatToString(ImageFormat format) {
    struct Entry {
        ImageFormat format;
        const char* name;
    };
    static constexpr Entry kMap[] = {{ImageFormat::ISO, "iso"},
                                     {ImageFormat::IMG, "img"},
                                     {ImageFormat::WIC, "wic"},
                                     {ImageFormat::ZIP, "zip"},
                                     {ImageFormat::GZIP, "gzip"},
                                     {ImageFormat::BZIP2, "bzip2"},
                                     {ImageFormat::XZ, "xz"},
                                     {ImageFormat::DMG, "dmg"},
                                     {ImageFormat::DSK, "dsk"}};
    for (const Entry& entry : kMap) {
        if (entry.format == format) {
            return QString::fromLatin1(entry.name);
        }
    }
    return QStringLiteral("unknown");
}

QJsonObject serializeVolume(const PartitionVolumeInfo& volume) {
    return QJsonObject{{QStringLiteral("drive_letter"), volume.drive_letter},
                       {QStringLiteral("label"), volume.label},
                       {QStringLiteral("file_system"), volume.file_system},
                       {QStringLiteral("total_bytes"), static_cast<double>(volume.total_bytes)},
                       {QStringLiteral("free_bytes"), static_cast<double>(volume.free_bytes)},
                       {QStringLiteral("bitlocker_enabled"), volume.bitlocker_enabled},
                       {QStringLiteral("bitlocker_locked"), volume.bitlocker_locked}};
}

QJsonObject serializePartition(const PartitionInfoEx& part) {
    QJsonObject obj{{QStringLiteral("partition_number"), static_cast<int>(part.partition_number)},
                    {QStringLiteral("type_name"), part.type_name},
                    {QStringLiteral("size_bytes"), static_cast<double>(part.size_bytes)},
                    {QStringLiteral("is_system"), part.is_system},
                    {QStringLiteral("is_efi"), part.is_efi},
                    {QStringLiteral("is_recovery"), part.is_recovery},
                    {QStringLiteral("is_read_only"), part.is_read_only}};
    if (part.hasVolume()) {
        obj.insert(QStringLiteral("volume"), serializeVolume(*part.volume));
    }
    return obj;
}

QJsonObject serializeDisk(const PartitionDiskInfo& disk) {
    QJsonArray partitions;
    for (const PartitionInfoEx& part : disk.partitions) {
        partitions.append(serializePartition(part));
    }
    return QJsonObject{{QStringLiteral("disk_number"), static_cast<int>(disk.disk_number)},
                       {QStringLiteral("model"), disk.model},
                       {QStringLiteral("bus_type"), disk.bus_type},
                       {QStringLiteral("media_type"), disk.media_type},
                       {QStringLiteral("partition_style"), disk.partition_style},
                       {QStringLiteral("size_bytes"), static_cast<double>(disk.size_bytes)},
                       {QStringLiteral("health_status"), disk.health_status},
                       {QStringLiteral("smart_summary"), disk.smart_summary},
                       {QStringLiteral("is_system"), disk.is_system},
                       {QStringLiteral("is_removable"), disk.is_removable},
                       {QStringLiteral("is_read_only"), disk.is_read_only},
                       {QStringLiteral("partitions"), partitions}};
}

AppActionResult listInventory(const QJsonObject&) {
    // allow_elevation=false: a read-only, ungated op must never trigger a UAC
    // prompt / elevated helper. Foreign partitions that need a privileged raw
    // probe keep an empty file_system and a warning instead of escalating.
    const PartitionInventory inventory = StorageInventoryWorker::scanCurrentSystem(false);
    QJsonArray disks;
    for (const PartitionDiskInfo& disk : inventory.disks) {
        disks.append(serializeDisk(disk));
    }
    QJsonArray warnings;
    for (const QString& warning : inventory.warnings) {
        if (warnings.size() >= kMaxInventoryWarnings) {
            break;
        }
        warnings.append(warning);
    }
    QJsonObject data{{QStringLiteral("disk_count"), disks.size()},
                     {QStringLiteral("layout_hash"), inventory.layout_hash},
                     {QStringLiteral("disks"), disks},
                     {QStringLiteral("warning_count"), inventory.warnings.size()},
                     {QStringLiteral("warnings"), warnings}};
    return {true, QStringLiteral("Enumerated %1 disk(s)").arg(disks.size()), data};
}

// A parsed preview op: its enum type plus the target KIND the safety validator
// expects for it. Kind is fixed by the operation, never inferred from which args
// the model supplied -- a partition-scoped op (format/delete/resize/...) must be
// validated on the partition path even when partition_number is omitted, so that a
// missing/invalid partition surfaces as a BLOCKER, never a false "ALLOWED" from the
// rule-less whole-disk path.
struct ParsedPartitionOp {
    PartitionOperationType type;
    PartitionTargetKind kind;
};

// Canonical snake_case name -> (operation type, required target kind). Deliberately
// a CURATED subset of PartitionOperationType: the disk/partition layout ops a
// technician assistant reasons about. The APFS/HFS foreign-file-surgery, imaging,
// and BitLocker/defrag types are a different domain and are omitted -- this op
// previews "what would a partition-layout change do", nothing else.
std::optional<ParsedPartitionOp> parsePartitionOpType(const QString& name) {
    struct Entry {
        const char* name;
        PartitionOperationType type;
        PartitionTargetKind kind;
    };
    constexpr PartitionTargetKind kDisk = PartitionTargetKind::Disk;
    constexpr PartitionTargetKind kPart = PartitionTargetKind::Partition;
    constexpr PartitionTargetKind kFree = PartitionTargetKind::Unallocated;
    static constexpr Entry kMap[] = {
        // Unallocated-scoped (the only op the validator accepts for free space).
        {"create", PartitionOperationType::Create, kFree},
        // Partition-scoped.
        {"delete", PartitionOperationType::Delete, kPart},
        {"format", PartitionOperationType::Format, kPart},
        {"resize", PartitionOperationType::Resize, kPart},
        {"allocate_free_space", PartitionOperationType::AllocateFreeSpace, kPart},
        {"set_drive_letter", PartitionOperationType::SetDriveLetter, kPart},
        {"set_partition_label", PartitionOperationType::SetPartitionLabel, kPart},
        {"set_partition_active", PartitionOperationType::SetPartitionActive, kPart},
        {"set_partition_hidden", PartitionOperationType::SetPartitionHidden, kPart},
        {"set_partition_type_id", PartitionOperationType::SetPartitionTypeId, kPart},
        {"check_file_system", PartitionOperationType::CheckFileSystem, kPart},
        {"merge", PartitionOperationType::Merge, kPart},
        {"split", PartitionOperationType::Split, kPart},
        {"move_partition", PartitionOperationType::MovePartition, kPart},
        {"clone_partition", PartitionOperationType::ClonePartition, kPart},
        {"wipe_partition", PartitionOperationType::WipePartition, kPart},
        {"wipe_free_space", PartitionOperationType::WipeFreeSpace, kPart},
        // Disk-scoped.
        {"convert_partition_style", PartitionOperationType::ConvertPartitionStyle, kDisk},
        {"initialize_disk", PartitionOperationType::InitializeDisk, kDisk},
        {"delete_all_partitions", PartitionOperationType::DeleteAllPartitions, kDisk},
        {"clone_disk", PartitionOperationType::CloneDisk, kDisk},
        {"wipe_disk", PartitionOperationType::WipeDisk, kDisk},
    };
    for (const Entry& entry : kMap) {
        if (name == QLatin1String(entry.name)) {
            return ParsedPartitionOp{entry.type, entry.kind};
        }
    }
    return std::nullopt;
}

// Convert an untrusted JSON byte-count double to uint64_t with no UB: a negative,
// NaN, or out-of-range magnitude never reaches a floating-to-unsigned cast that
// [conv.fpint] leaves undefined. Callers reject negatives up front; this is the
// belt-and-suspenders clamp.
uint64_t safeByteCount(double value) {
    if (!(value > 0.0)) {  // <= 0 or NaN
        return 0;
    }
    constexpr double kUint64Ceiling = 18446744073709551616.0;  // 2^64
    if (value >= kUint64Ceiling) {
        return std::numeric_limits<uint64_t>::max();
    }
    return static_cast<uint64_t>(value);
}

QString supportedPartitionOpTypes() {
    return QStringLiteral(
        "create, delete, format, resize, allocate_free_space, set_drive_letter, "
        "set_partition_label, set_partition_active, set_partition_hidden, "
        "set_partition_type_id, check_file_system, convert_partition_style, merge, split, "
        "move_partition, initialize_disk, delete_all_partitions, clone_disk, clone_partition, "
        "wipe_partition, wipe_disk, wipe_free_space");
}

// Build the validator target. The KIND is fixed by the operation (passed in), not
// inferred from which args are present, so a partition-scoped op with no/invalid
// partition_number is still validated on the partition path. offset/size can exceed
// 2^31, so read them as double (exact for ints < 2^53) and convert without UB.
PartitionTarget buildPreviewTarget(const QJsonObject& args,
                                   uint32_t disk_number,
                                   PartitionTargetKind kind) {
    PartitionTarget target;
    target.kind = kind;
    target.disk_number = disk_number;
    target.partition_number =
        static_cast<uint32_t>(args.value(QStringLiteral("partition_number")).toInt(0));
    target.offset_bytes = safeByteCount(args.value(QStringLiteral("offset_bytes")).toDouble(0.0));
    target.size_bytes = safeByteCount(args.value(QStringLiteral("size_bytes")).toDouble(0.0));
    return target;
}

QJsonArray cappedMessages(const QStringList& messages) {
    QJsonArray out;
    for (const QString& message : messages) {
        if (out.size() >= kMaxPreviewMessages) {
            break;
        }
        out.append(message);
    }
    return out;
}

// Validate the numeric preview args up front. Returns an error result to return
// verbatim, or nullopt when disk_number/partition_number/offset/size are all
// well-formed (present-and-non-negative). Split out to keep previewPartitionOperation
// within the cyclomatic-complexity budget.
std::optional<AppActionResult> validatePreviewNumericArgs(const QJsonObject& args) {
    if (!args.contains(QStringLiteral("disk_number"))) {
        return AppActionResult{
            false, QStringLiteral("preview_operation requires a 'disk_number' argument"), {}};
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

QJsonObject serializePreviewOperation(const PartitionOperation& operation) {
    return QJsonObject{{QStringLiteral("type"), toDisplayString(operation.type)},
                       {QStringLiteral("risk"), toDisplayString(operation.risk)},
                       {QStringLiteral("summary"), operation.summary},
                       {QStringLiteral("warnings"), cappedMessages(operation.warnings)},
                       {QStringLiteral("blockers"), cappedMessages(operation.blockers)}};
}

// Plan (never execute) a single partition-layout operation and report whether the
// safety validator would allow it, with the blockers/warnings it raises. Purely
// read-only: PartitionOperationPlanner::previewOperation runs the same validator the
// GUI uses over a fresh no-elevation inventory and touches no disk. A BLOCKED
// operation is a successful preview whose answer is "not allowed" (like a dry run),
// so success stays true; only a malformed request fails.
AppActionResult previewPartitionOperation(const QJsonObject& args) {
    const QString type_name = args.value(QStringLiteral("operation")).toString().trimmed();
    if (type_name.isEmpty()) {
        return {false, QStringLiteral("preview_operation requires an 'operation' argument"), {}};
    }
    const std::optional<ParsedPartitionOp> parsed = parsePartitionOpType(type_name);
    if (!parsed) {
        return {false,
                QStringLiteral("Unsupported operation '%1'. Supported: %2")
                    .arg(type_name, supportedPartitionOpTypes()),
                {}};
    }
    if (const std::optional<AppActionResult> error = validatePreviewNumericArgs(args)) {
        return *error;
    }
    const int disk_number = args.value(QStringLiteral("disk_number")).toInt(-1);

    const PartitionInventory inventory = StorageInventoryWorker::scanCurrentSystem(false);
    const PartitionTarget target =
        buildPreviewTarget(args, static_cast<uint32_t>(disk_number), parsed->kind);
    const QJsonObject payload = args.value(QStringLiteral("payload")).toObject();
    const PartitionOperation operation =
        PartitionOperationPlanner::makeOperation(parsed->type, target, payload);

    PartitionOperationPlanner planner;
    const OperationPreview preview = planner.previewOperation(inventory, operation);

    QJsonArray operations;
    for (const PartitionOperation& op : preview.operations) {
        operations.append(serializePreviewOperation(op));
    }
    QJsonObject data{{QStringLiteral("can_apply"), preview.canApply()},
                     {QStringLiteral("before_layout_hash"), preview.before_layout_hash},
                     {QStringLiteral("after_layout_description"), preview.after_layout_description},
                     {QStringLiteral("blocker_count"), preview.blockers.size()},
                     {QStringLiteral("warning_count"), preview.warnings.size()},
                     {QStringLiteral("blockers"), cappedMessages(preview.blockers)},
                     {QStringLiteral("warnings"), cappedMessages(preview.warnings)},
                     {QStringLiteral("operations"), operations}};
    const QString message =
        preview.canApply()
            ? QStringLiteral("Operation ALLOWED (%1 warning(s))").arg(preview.warnings.size())
            : QStringLiteral("Operation BLOCKED: %1")
                  .arg(preview.blockers.isEmpty() ? QStringLiteral("(no reason given)")
                                                  : preview.blockers.first());
    return {true, message, data};
}

QJsonObject serializeProgram(const ProgramInfo& program) {
    return QJsonObject{{QStringLiteral("name"), program.displayName},
                       {QStringLiteral("publisher"), program.publisher},
                       {QStringLiteral("version"), program.displayVersion},
                       {QStringLiteral("install_location"), program.installLocation}};
}

AppActionResult listInstalledPrograms(const QJsonObject&) {
    const QVector<ProgramInfo> programs = VulnerabilityScanner::enumerateInstalledProgramsFast();
    QJsonArray listed;
    for (const ProgramInfo& program : programs) {
        if (listed.size() >= kMaxListedPrograms) {
            break;
        }
        listed.append(serializeProgram(program));
    }
    const bool truncated = programs.size() > listed.size();
    QJsonObject data{{QStringLiteral("total_count"), programs.size()},
                     {QStringLiteral("listed_count"), listed.size()},
                     {QStringLiteral("truncated"), truncated},
                     {QStringLiteral("programs"), listed}};
    return {true, QStringLiteral("Found %1 installed program(s)").arg(programs.size()), data};
}

VulnerabilityScanOptions scanOptionsFromArgs(const QJsonObject& args) {
    // Only the options the installed-programs scan actually honors are exposed:
    // scanInstalledPrograms reads queryNvd (+ CISA KEV, always) and, gated by it,
    // broadNvdInstalledScan. queryGithub/queryOsv apply only to scanSoftware, so
    // exposing them here would let the model believe it queried sources it did not.
    VulnerabilityScanOptions options;
    if (args.contains(QStringLiteral("query_nvd"))) {
        options.queryNvd = args.value(QStringLiteral("query_nvd")).toBool(options.queryNvd);
    }
    if (args.contains(QStringLiteral("broad_nvd"))) {
        options.broadNvdInstalledScan =
            args.value(QStringLiteral("broad_nvd")).toBool(options.broadNvdInstalledScan);
    }
    return options;
}

AppActionResult scanVulnerabilities(const QJsonObject& args) {
    const QVector<ProgramInfo> programs = VulnerabilityScanner::enumerateInstalledProgramsFast();
    const VulnerabilityScanOptions options = scanOptionsFromArgs(args);
    const VulnerabilityScanResult scan = VulnerabilityScanner::scanInstalledPrograms(programs,
                                                                                     options);

    QJsonArray findings;
    for (const VulnerabilityFinding& finding : scan.findings) {
        if (findings.size() >= kMaxReportedFindings) {
            break;
        }
        findings.append(VulnerabilityScanner::findingToJson(finding));
    }
    QJsonArray source_errors;
    for (const QString& err : scan.sourceErrors) {
        source_errors.append(err);
    }
    QJsonObject data{{QStringLiteral("installed_apps_scanned"), scan.installedAppsScanned},
                     {QStringLiteral("total_findings"), scan.findings.size()},
                     {QStringLiteral("reported_findings"), findings.size()},
                     {QStringLiteral("critical_count"), scan.criticalCount},
                     {QStringLiteral("actively_exploited_count"), scan.activelyExploitedCount},
                     {QStringLiteral("truncated"), scan.findings.size() > findings.size()},
                     {QStringLiteral("findings"), findings},
                     {QStringLiteral("source_errors"), source_errors}};
    const QString message = QStringLiteral("Scanned %1 app(s): %2 finding(s), %3 critical")
                                .arg(scan.installedAppsScanned)
                                .arg(scan.findings.size())
                                .arg(scan.criticalCount);
    return {true, message, data};
}

AppActionResult identifyImage(const QJsonObject& args) {
    const QString path = args.value(QStringLiteral("path")).toString().trimmed();
    if (path.isEmpty()) {
        return {false, QStringLiteral("identify_image requires a 'path' argument"), {}};
    }
    const QFileInfo info(path);
    if (!info.exists() || !info.isFile()) {
        return {false, QStringLiteral("No such image file: %1").arg(path), {}};
    }
    // NOTE: the app's detectFormat/isCompressed classify by FILE EXTENSION only --
    // they read zero bytes, so this is a filename-based hint, not content
    // validation. Uncompressed size and checksum (which would read the whole file)
    // are intentionally not computed here; expose them via a future op.
    const ImageFormat format = FileImageSource::detectFormat(path);
    const bool is_compressed = CompressedImageSource::isCompressed(path);
    QJsonObject data{{QStringLiteral("path"), info.absoluteFilePath()},
                     {QStringLiteral("name"), info.fileName()},
                     {QStringLiteral("detection"), QStringLiteral("extension")},
                     {QStringLiteral("format"), imageFormatToString(format)},
                     {QStringLiteral("size_bytes"), static_cast<double>(info.size())},
                     {QStringLiteral("is_compressed"), is_compressed},
                     {QStringLiteral("extension_recognized"), format != ImageFormat::Unknown}};
    const QString message = QStringLiteral("%1 -- format %2 (by extension)%3")
                                .arg(info.fileName(),
                                     imageFormatToString(format),
                                     is_compressed ? QStringLiteral(", compressed") : QString());
    return {true, message, data};
}

QString clampLine(const QString& line) {
    if (line.size() <= kMaxMatchLineChars) {
        return line;
    }
    return line.left(kMaxMatchLineChars) + QStringLiteral("...");
}

QJsonObject serializeMatch(const SearchMatch& match) {
    return QJsonObject{{QStringLiteral("file_path"), match.file_path},
                       {QStringLiteral("line_number"), match.line_number},
                       {QStringLiteral("line_content"), clampLine(match.line_content)},
                       {QStringLiteral("match_start"), match.match_start},
                       {QStringLiteral("match_end"), match.match_end}};
}

SearchConfig searchConfigFromArgs(const QJsonObject& args,
                                  const QString& root,
                                  const QString& pattern) {
    SearchConfig config;
    config.root_path = root;
    config.pattern = pattern;
    config.case_sensitive = args.value(QStringLiteral("case_sensitive")).toBool(false);
    config.use_regex = args.value(QStringLiteral("use_regex")).toBool(false);
    config.whole_word = args.value(QStringLiteral("whole_word")).toBool(false);
    const int requested = args.value(QStringLiteral("max_results")).toInt(kDefaultSearchMaxResults);
    // Clamp to a server ceiling so a model-supplied max_results cannot force
    // unbounded in-memory accumulation.
    config.max_results = std::clamp(requested > 0 ? requested : kDefaultSearchMaxResults,
                                    1,
                                    kSearchMaxResultsCeiling);
    const QJsonArray exts = args.value(QStringLiteral("file_extensions")).toArray();
    for (const QJsonValue& ext : exts) {
        const QString value = ext.toString().trimmed();
        if (!value.isEmpty()) {
            config.file_extensions.append(value);
        }
    }
    return config;
}

QJsonObject serializeSearch(const QVector<SearchMatch>& matches, int total_files, int max_results) {
    QJsonArray listed;
    for (const SearchMatch& match : matches) {
        if (listed.size() >= kMaxReportedMatches) {
            break;
        }
        listed.append(serializeMatch(match));
    }
    const int total_matches = matches.size();
    return QJsonObject{{QStringLiteral("total_matches"), total_matches},
                       {QStringLiteral("total_files"), total_files},
                       {QStringLiteral("reported_matches"), listed.size()},
                       {QStringLiteral("report_truncated"), listed.size() < total_matches},
                       // The search itself stopped at max_results, so more matches
                       // may exist beyond what was counted.
                       {QStringLiteral("search_capped"), total_matches >= max_results},
                       {QStringLiteral("matches"), listed}};
}

AppActionResult searchFiles(const QJsonObject& args) {
    const QString root = args.value(QStringLiteral("root_path")).toString().trimmed();
    const QString pattern = args.value(QStringLiteral("pattern")).toString();
    if (root.isEmpty() || pattern.isEmpty()) {
        return {false, QStringLiteral("search requires 'root_path' and 'pattern'"), {}};
    }
    if (isNetworkOrDevicePath(root)) {
        return {false, QStringLiteral("search does not allow network/UNC or device paths"), {}};
    }
    if (!QFileInfo(root).exists()) {
        return {false, QStringLiteral("root_path does not exist: %1").arg(root), {}};
    }
    const SearchConfig config = searchConfigFromArgs(args, root, pattern);
    const int cap = config.max_results;

    // Drive the search ENGINE (worker) directly, not AdvancedSearchController: the
    // controller loads/persists shared preferences and prepends the pattern to the
    // user's on-disk search history, which would (a) be a write from a "read-only"
    // op and (b) race the unlocked ConfigManager singleton off the GUI thread. The
    // worker touches neither. It runs on its own thread and posts results back,
    // drained by the local event loop; the worker (destroyed last) stops+joins its
    // thread on teardown, so a timed-out search is reaped, never leaked.
    AdvancedSearchWorker worker(config);
    AsyncActionInvocation inv;
    auto matches = std::make_shared<QVector<SearchMatch>>();
    auto file_count = std::make_shared<int>(0);

    QObject::connect(&worker,
                     &AdvancedSearchWorker::resultsReady,
                     inv.context(),
                     [matches](const QVector<SearchMatch>& batch) { *matches += batch; });
    QObject::connect(&worker,
                     &AdvancedSearchWorker::fileSearched,
                     inv.context(),
                     [file_count](const QString&, int) { ++(*file_count); });
    QObject::connect(
        &worker, &WorkerBase::finished, inv.context(), [&inv, matches, file_count, cap]() {
            const QString message = QStringLiteral("%1 match(es) across %2 file(s)")
                                        .arg(matches->size())
                                        .arg(*file_count);
            inv.finish({true, message, serializeSearch(*matches, *file_count, cap)});
        });
    QObject::connect(&worker,
                     &WorkerBase::failed,
                     inv.context(),
                     [&inv](int, const QString& error) { inv.finish({false, error, {}}); });

    const AppActionResult result = inv.run([&worker]() { worker.start(); });
    worker.requestStop();  // cooperative stop before teardown (no-op if already finished)
    return result;
}

QJsonObject serializeCpu(const CpuInfo& cpu) {
    return QJsonObject{{QStringLiteral("name"), cpu.name},
                       {QStringLiteral("manufacturer"), cpu.manufacturer},
                       {QStringLiteral("cores"), static_cast<int>(cpu.cores)},
                       {QStringLiteral("threads"), static_cast<int>(cpu.threads)},
                       {QStringLiteral("base_clock_mhz"), static_cast<int>(cpu.base_clock_mhz)},
                       {QStringLiteral("max_clock_mhz"), static_cast<int>(cpu.max_clock_mhz)}};
}

QJsonObject serializeMemory(const MemorySummary& memory) {
    return QJsonObject{{QStringLiteral("total_bytes"), static_cast<double>(memory.total_bytes)},
                       {QStringLiteral("available_bytes"),
                        static_cast<double>(memory.available_bytes)},
                       {QStringLiteral("slots_used"), static_cast<int>(memory.slots_used)},
                       {QStringLiteral("slots_total"), static_cast<int>(memory.slots_total)},
                       {QStringLiteral("module_count"), memory.modules.size()}};
}

QJsonObject serializeStorageDevice(const StorageDeviceInfo& device) {
    return QJsonObject{{QStringLiteral("model"), device.model},
                       {QStringLiteral("size_bytes"), static_cast<double>(device.size_bytes)},
                       {QStringLiteral("interface_type"), device.interface_type},
                       {QStringLiteral("media_type"), device.media_type},
                       {QStringLiteral("temperature_celsius"), device.temperature},
                       {QStringLiteral("disk_number"), static_cast<int>(device.disk_number)}};
}

QJsonObject serializeGpu(const GpuInfo& gpu) {
    return QJsonObject{{QStringLiteral("name"), gpu.name},
                       {QStringLiteral("manufacturer"), gpu.manufacturer},
                       {QStringLiteral("vram_bytes"), static_cast<double>(gpu.vram_bytes)},
                       {QStringLiteral("driver_version"), gpu.driver_version}};
}

QJsonObject serializeHardwareInventory(const HardwareInventory& inventory) {
    QJsonArray storage;
    for (const StorageDeviceInfo& device : inventory.storage) {
        storage.append(serializeStorageDevice(device));
    }
    QJsonArray gpus;
    for (const GpuInfo& gpu : inventory.gpus) {
        gpus.append(serializeGpu(gpu));
    }
    return QJsonObject{
        {QStringLiteral("cpu"), serializeCpu(inventory.cpu)},
        {QStringLiteral("memory"), serializeMemory(inventory.memory)},
        {QStringLiteral("storage"), storage},
        {QStringLiteral("gpus"), gpus},
        {QStringLiteral("motherboard"),
         QJsonObject{{QStringLiteral("manufacturer"), inventory.motherboard.manufacturer},
                     {QStringLiteral("product"), inventory.motherboard.product},
                     {QStringLiteral("bios_version"), inventory.motherboard.bios_version}}},
        {QStringLiteral("battery"),
         QJsonObject{{QStringLiteral("present"), inventory.battery.present},
                     {QStringLiteral("health_percent"), inventory.battery.health_percent},
                     {QStringLiteral("status"), inventory.battery.status}}},
        {QStringLiteral("os_name"), inventory.os_name},
        {QStringLiteral("os_version"), inventory.os_version},
        {QStringLiteral("os_build"), inventory.os_build},
        {QStringLiteral("os_architecture"), inventory.os_architecture},
        {QStringLiteral("uptime_seconds"), static_cast<double>(inventory.uptime_seconds)}};
}

AppActionResult hardwareScan(const QJsonObject&) {
    // scan() blocks and emits scanComplete inline on THIS thread, so a direct
    // connection captures the result without any event loop.
    HardwareInventoryScanner scanner;
    HardwareInventory inventory;
    bool captured = false;
    QObject::connect(&scanner,
                     &HardwareInventoryScanner::scanComplete,
                     &scanner,
                     [&inventory, &captured](const HardwareInventory& result) {
                         inventory = result;
                         captured = true;
                     });
    scanner.scan();
    if (!captured) {
        return {false, QStringLiteral("Hardware scan did not complete"), {}};
    }
    return {true,
            QStringLiteral("Hardware inventory collected"),
            serializeHardwareInventory(inventory)};
}

QString smartHealthToString(SmartHealthStatus status) {
    switch (status) {
    case SmartHealthStatus::Healthy:
        return QStringLiteral("healthy");
    case SmartHealthStatus::Warning:
        return QStringLiteral("warning");
    case SmartHealthStatus::Critical:
        return QStringLiteral("critical");
    case SmartHealthStatus::Unknown:
        break;
    }
    return QStringLiteral("unknown");
}

QJsonObject serializeSmartReport(const SmartReport& report) {
    QJsonArray warnings;
    for (const QString& warning : report.warnings) {
        warnings.append(warning);
    }
    QJsonArray recommendations;
    for (const QString& rec : report.recommendations) {
        recommendations.append(rec);
    }
    return QJsonObject{
        {QStringLiteral("device_path"), report.device_path},
        {QStringLiteral("model"), report.model},
        {QStringLiteral("serial_number"), report.serial_number},
        {QStringLiteral("size_bytes"), static_cast<double>(report.size_bytes)},
        {QStringLiteral("interface_type"), report.interface_type},
        {QStringLiteral("overall_health"), smartHealthToString(report.overall_health)},
        {QStringLiteral("smart_status"), report.smart_status},
        {QStringLiteral("power_on_hours"), static_cast<double>(report.power_on_hours)},
        {QStringLiteral("temperature_celsius"), report.temperature_celsius},
        {QStringLiteral("reallocated_sectors"), static_cast<double>(report.reallocated_sectors)},
        {QStringLiteral("pending_sectors"), static_cast<double>(report.pending_sectors)},
        {QStringLiteral("wear_level_percent"), report.wear_level_percent},
        {QStringLiteral("warnings"), warnings},
        {QStringLiteral("recommendations"), recommendations}};
}

AppActionResult smartScan(const QJsonObject&) {
    // analyzeAll() blocks (runs bundled smartctl per drive) and populates reports().
    // Requires admin for full raw-disk data; without it a drive whose smartctl read
    // fails is dropped (drive_count shrinks), so smartctl_available conveys the
    // degraded state. It degrades gracefully -- it never prompts for elevation.
    SmartDiskAnalyzer analyzer;
    analyzer.analyzeAll();
    QJsonArray drives;
    for (const SmartReport& report : analyzer.reports()) {
        drives.append(serializeSmartReport(report));
    }
    QJsonObject data{{QStringLiteral("drive_count"), drives.size()},
                     {QStringLiteral("smartctl_available"), analyzer.isSmartctlAvailable()},
                     {QStringLiteral("drives"), drives}};
    return {true, QStringLiteral("Analyzed SMART data for %1 drive(s)").arg(drives.size()), data};
}

QString clampHeader(const QString& value) {
    if (value.size() <= kMaxHeaderChars) {
        return value;
    }
    return value.left(kMaxHeaderChars) + QStringLiteral("...");
}

QJsonObject serializeMboxMessage(const MboxMessage& message) {
    // Header fields are clamped: a crafted message can carry a header folded up to
    // the per-message size cap, which must not become a giant JSON string.
    return QJsonObject{{QStringLiteral("index"), message.message_index},
                       {QStringLiteral("subject"), clampHeader(message.subject)},
                       {QStringLiteral("from"), clampHeader(message.from)},
                       {QStringLiteral("to"), clampHeader(message.to)},
                       {QStringLiteral("cc"), clampHeader(message.cc)},
                       {QStringLiteral("date"), message.date.toString(Qt::ISODate)},
                       {QStringLiteral("size_bytes"), static_cast<double>(message.message_size)},
                       {QStringLiteral("has_attachments"), message.has_attachments}};
}

AppActionResult readMbox(const QJsonObject& args) {
    const QString path = args.value(QStringLiteral("path")).toString().trimmed();
    if (path.isEmpty()) {
        return {false, QStringLiteral("read_mbox requires a 'path' argument"), {}};
    }
    if (isNetworkOrDevicePath(path)) {
        return {false, QStringLiteral("read_mbox does not allow network/UNC or device paths"), {}};
    }
    const QFileInfo info(path);
    if (!info.isFile()) {
        return {false, QStringLiteral("No such MBOX file: %1").arg(path), {}};
    }
    if (info.size() > kMaxMboxBytes) {
        return {false,
                QStringLiteral("MBOX file is too large for a headless read (%1 bytes > %2 limit)")
                    .arg(info.size())
                    .arg(kMaxMboxBytes),
                {}};
    }
    const int offset = std::max(0, args.value(QStringLiteral("offset")).toInt(0));
    const int requested = args.value(QStringLiteral("limit")).toInt(kDefaultMboxLimit);
    const int limit =
        std::clamp(requested > 0 ? requested : kDefaultMboxLimit, 1, kMboxLimitCeiling);

    // MboxParser opens the file READ-ONLY and its readMessages() is a synchronous
    // worker-thread API (lazily builds the message index), so this runs inline with
    // no event loop and no controller.
    MboxParser parser;
    parser.open(path);
    if (!parser.isOpen()) {
        return {false, QStringLiteral("Not a valid MBOX file: %1").arg(path), {}};
    }
    const auto result = parser.readMessages(offset, limit);
    if (!result.has_value()) {
        return {false, QStringLiteral("Failed to read MBOX messages from %1").arg(path), {}};
    }

    QJsonArray messages;
    for (const MboxMessage& message : *result) {
        messages.append(serializeMboxMessage(message));
    }
    const int total = parser.messageCount();
    QJsonObject data{{QStringLiteral("message_count"), total},
                     {QStringLiteral("offset"), offset},
                     {QStringLiteral("returned"), messages.size()},
                     {QStringLiteral("messages"), messages}};
    const QString summary =
        QStringLiteral("Returned %1 of %2 message(s)").arg(messages.size()).arg(total);
    return {true, summary, data};
}

AppActionDescriptor makeDescriptor(const QString& id,
                                   const QString& title,
                                   const QString& description,
                                   const QString& category,
                                   const QJsonObject& params_schema) {
    AppActionDescriptor descriptor;
    descriptor.id = id;
    descriptor.title = title;
    descriptor.description = description;
    descriptor.category = category;
    descriptor.params_schema = params_schema;
    descriptor.read_only = true;
    descriptor.mutating = false;
    descriptor.destructive = false;
    descriptor.requires_admin = false;
    return descriptor;
}

QJsonObject noParamsSchema() {
    return QJsonObject{{QStringLiteral("type"), QStringLiteral("object")},
                       {QStringLiteral("properties"), QJsonObject{}},
                       {QStringLiteral("additionalProperties"), false}};
}

QJsonObject boolProp(const QString& description) {
    return QJsonObject{{QStringLiteral("type"), QStringLiteral("boolean")},
                       {QStringLiteral("description"), description}};
}

QJsonObject scanParamsSchema() {
    QJsonObject properties{
        {QStringLiteral("query_nvd"),
         boolProp(QStringLiteral("Query the NVD CVE feed (CISA KEV is always checked)"))},
        {QStringLiteral("broad_nvd"),
         boolProp(QStringLiteral("Broaden the NVD keyword scan (slower, sends program names)"))}};
    return QJsonObject{{QStringLiteral("type"), QStringLiteral("object")},
                       {QStringLiteral("properties"), properties},
                       {QStringLiteral("additionalProperties"), false}};
}

QJsonObject stringProp(const QString& description) {
    return QJsonObject{{QStringLiteral("type"), QStringLiteral("string")},
                       {QStringLiteral("description"), description}};
}

QJsonObject searchParamsSchema() {
    QJsonObject ext_prop{
        {QStringLiteral("type"), QStringLiteral("array")},
        {QStringLiteral("items"), QJsonObject{{QStringLiteral("type"), QStringLiteral("string")}}},
        {QStringLiteral("description"),
         QStringLiteral("Restrict to these file extensions (no dot); empty = all files")}};
    QJsonObject max_prop{{QStringLiteral("type"), QStringLiteral("integer")},
                         {QStringLiteral("description"),
                          QStringLiteral("Cap total matches (default 1000)")}};
    QJsonObject properties{
        {QStringLiteral("root_path"),
         stringProp(QStringLiteral("Directory or file to search (absolute path)"))},
        {QStringLiteral("pattern"), stringProp(QStringLiteral("Text or regex to find"))},
        {QStringLiteral("case_sensitive"), boolProp(QStringLiteral("Case-sensitive match"))},
        {QStringLiteral("use_regex"), boolProp(QStringLiteral("Treat pattern as a regex"))},
        {QStringLiteral("whole_word"), boolProp(QStringLiteral("Match whole words only"))},
        {QStringLiteral("max_results"), max_prop},
        {QStringLiteral("file_extensions"), ext_prop}};
    return QJsonObject{{QStringLiteral("type"), QStringLiteral("object")},
                       {QStringLiteral("properties"), properties},
                       {QStringLiteral("required"),
                        QJsonArray{QStringLiteral("root_path"), QStringLiteral("pattern")}},
                       {QStringLiteral("additionalProperties"), false}};
}

QJsonObject intProp(const QString& description) {
    return QJsonObject{{QStringLiteral("type"), QStringLiteral("integer")},
                       {QStringLiteral("description"), description}};
}

QJsonObject readMboxParamsSchema() {
    QJsonObject properties{
        {QStringLiteral("path"), stringProp(QStringLiteral("Absolute path to the MBOX file"))},
        {QStringLiteral("offset"),
         intProp(QStringLiteral("First message index to return (0-based)"))},
        {QStringLiteral("limit"), intProp(QStringLiteral("Max messages to return (default 200)"))}};
    return QJsonObject{{QStringLiteral("type"), QStringLiteral("object")},
                       {QStringLiteral("properties"), properties},
                       {QStringLiteral("required"), QJsonArray{QStringLiteral("path")}},
                       {QStringLiteral("additionalProperties"), false}};
}

QJsonObject identifyParamsSchema() {
    QJsonObject path_prop{{QStringLiteral("type"), QStringLiteral("string")},
                          {QStringLiteral("description"),
                           QStringLiteral("Absolute path to the disk-image file to identify")}};
    return QJsonObject{{QStringLiteral("type"), QStringLiteral("object")},
                       {QStringLiteral("properties"),
                        QJsonObject{{QStringLiteral("path"), path_prop}}},
                       {QStringLiteral("required"), QJsonArray{QStringLiteral("path")}},
                       {QStringLiteral("additionalProperties"), false}};
}

QJsonObject previewParamsSchema() {
    QJsonObject payload_prop{
        {QStringLiteral("type"), QStringLiteral("object")},
        {QStringLiteral("description"),
         QStringLiteral("Operation-specific fields (e.g. file_system, label, drive_letter)")},
        {QStringLiteral("additionalProperties"), true}};
    QJsonObject properties{
        {QStringLiteral("operation"),
         stringProp(QStringLiteral("Operation to preview (one of: ") + supportedPartitionOpTypes() +
                    QStringLiteral(")"))},
        {QStringLiteral("disk_number"),
         intProp(QStringLiteral("Target disk number (0-based, from list_inventory)"))},
        {QStringLiteral("partition_number"),
         intProp(QStringLiteral("Target partition number (1-based) for a partition-scoped op "
                                "(delete/format/resize/set_*/merge/split/move/wipe_partition)"))},
        {QStringLiteral("offset_bytes"),
         intProp(QStringLiteral("Byte offset of the unallocated region for a create"))},
        {QStringLiteral("size_bytes"),
         intProp(QStringLiteral("Size in bytes (for create/resize/allocate)"))},
        {QStringLiteral("payload"), payload_prop}};
    return QJsonObject{{QStringLiteral("type"), QStringLiteral("object")},
                       {QStringLiteral("properties"), properties},
                       {QStringLiteral("required"),
                        QJsonArray{QStringLiteral("operation"), QStringLiteral("disk_number")}},
                       {QStringLiteral("additionalProperties"), false}};
}

using AddActionFn = std::function<void(const AppActionDescriptor&, AppActionInvoke)>;

// Register the read-only partition ops. Split out of registerReadOnlyAppActionsInto
// to keep that function within the length budget as the op set grows.
void registerPartitionReadOnlyOps(const AddActionFn& add) {
    add(makeDescriptor(QStringLiteral("partition.list_inventory"),
                       QStringLiteral("List storage inventory"),
                       QStringLiteral("Enumerate disks, partitions, and volumes on this system"),
                       QStringLiteral("partition"),
                       noParamsSchema()),
        listInventory);

    add(makeDescriptor(
            QStringLiteral("partition.preview_operation"),
            QStringLiteral("Preview a partition operation"),
            QStringLiteral(
                "Plan (never execute) a disk/partition-layout operation and report "
                "whether it is allowed, with blockers/warnings from the safety validator"),
            QStringLiteral("partition"),
            previewParamsSchema()),
        previewPartitionOperation);
}

}  // namespace

int registerReadOnlyAppActionsInto(AppActionRegistry& registry) {
    int registered = 0;
    const auto add = [&](const AppActionDescriptor& descriptor, AppActionInvoke invoke) {
        if (registry.registerAction(descriptor, std::move(invoke))) {
            ++registered;
        }
    };

    registerPartitionReadOnlyOps(add);

    add(makeDescriptor(QStringLiteral("security.list_installed_programs"),
                       QStringLiteral("List installed programs"),
                       QStringLiteral("Enumerate installed programs (registry + store apps)"),
                       QStringLiteral("security"),
                       noParamsSchema()),
        listInstalledPrograms);

    add(makeDescriptor(QStringLiteral("security.scan_vulnerabilities"),
                       QStringLiteral("Scan for vulnerabilities"),
                       QStringLiteral(
                           "Scan installed programs against CVE/advisory feeds (network, slower)"),
                       QStringLiteral("security"),
                       scanParamsSchema()),
        scanVulnerabilities);

    add(makeDescriptor(QStringLiteral("imaging.identify_image"),
                       QStringLiteral("Identify a disk image"),
                       QStringLiteral(
                           "Detect a disk-image file's format/compression by file extension"),
                       QStringLiteral("imaging"),
                       identifyParamsSchema()),
        identifyImage);

    add(makeDescriptor(QStringLiteral("search.find_in_files"),
                       QStringLiteral("Search files"),
                       QStringLiteral("Search a directory tree for text/regex matches in files"),
                       QStringLiteral("search"),
                       searchParamsSchema()),
        searchFiles);

    add(makeDescriptor(QStringLiteral("diagnostics.hardware_scan"),
                       QStringLiteral("Scan hardware inventory"),
                       QStringLiteral("Enumerate CPU/memory/storage/GPU/motherboard/battery/OS"),
                       QStringLiteral("diagnostics"),
                       noParamsSchema()),
        hardwareScan);

    add(makeDescriptor(QStringLiteral("diagnostics.smart_scan"),
                       QStringLiteral("Scan drive SMART health"),
                       QStringLiteral(
                           "Read SMART health for each drive (needs admin for full data)"),
                       QStringLiteral("diagnostics"),
                       noParamsSchema()),
        smartScan);

    add(makeDescriptor(QStringLiteral("email.read_mbox"),
                       QStringLiteral("Read an MBOX mailbox"),
                       QStringLiteral("List messages (headers) from an MBOX email file"),
                       QStringLiteral("email"),
                       readMboxParamsSchema()),
        readMbox);

    return registered;
}

}  // namespace sak
