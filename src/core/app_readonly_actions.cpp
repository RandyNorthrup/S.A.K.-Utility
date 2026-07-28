// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

// Read-only technician ops exposed to the AI assistant. Each invoke thunk calls
// an existing headless src/core service (no re-implementation) and serializes its
// result to a compact, model-facing QJsonObject. All are synchronous and run on
// the caller's worker thread -- no event loop, no GUI, no controller lifetime.

#include "sak/app_readonly_actions.h"

#include "sak/active_connections_monitor.h"
#include "sak/advanced_search_types.h"
#include "sak/advanced_search_worker.h"
#include "sak/advanced_uninstall_types.h"
#include "sak/app_action_guards.h"
#include "sak/app_action_registry.h"
#include "sak/app_action_service.h"
#include "sak/app_organizer_helpers.h"
#include "sak/app_partition_op_parse.h"
#include "sak/connectivity_tester.h"
#include "sak/cpu_benchmark_worker.h"
#include "sak/deadline_canceller.h"
#include "sak/deleted_item_scanner.h"
#include "sak/diagnostic_types.h"
#include "sak/dns_diagnostic_tool.h"
#include "sak/drive_scanner.h"
#include "sak/duplicate_finder_worker.h"
#include "sak/email_constants.h"
#include "sak/email_profile_manager.h"
#include "sak/email_search_worker.h"
#include "sak/email_types.h"
#include "sak/error_codes.h"
#include "sak/file_explorer_archive_service.h"
#include "sak/file_hash.h"
#include "sak/file_recovery_engine.h"
#include "sak/file_scanner.h"
#include "sak/firewall_rule_auditor.h"
#include "sak/hardware_inventory_scanner.h"
#include "sak/image_source.h"
#include "sak/iso_analyzer.h"
#include "sak/leftover_scanner.h"
#include "sak/mbox_parser.h"
#include "sak/memory_benchmark_worker.h"
#include "sak/network_adapter_inspector.h"
#include "sak/network_diagnostic_types.h"
#include "sak/network_probe_worker.h"
#include "sak/network_share_browser.h"
#include "sak/organizer_worker.h"
#include "sak/partition_manager_types.h"
#include "sak/partition_operation_planner.h"
#include "sak/port_scanner.h"
#include "sak/program_enumerator.h"
#include "sak/pst_parser.h"
#include "sak/restore_point_manager.h"
#include "sak/smart_disk_analyzer.h"
#include "sak/storage_inventory_worker.h"
#include "sak/thermal_monitor.h"
#include "sak/user_data_manager.h"
#include "sak/vulnerability_scanner.h"
#include "sak/wifi_analyzer.h"
#include "sak/wifi_profile_scanner.h"
#include "sak/wifi_setup_script.h"
#include "sak/windows_user_scanner.h"
#include "sak/worker_base.h"

#include <QFile>
#include <QFileInfo>
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <QVector>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <stop_token>
#include <thread>
#include <vector>

namespace sak {

namespace {

// Bound model-facing list payloads so a machine with hundreds of programs or a
// large scan does not blow the tool-result size. Truncation is reported, never
// silent.
constexpr int kMaxListedPrograms = 500;
// software.scan_leftovers: cap the item sample surfaced (running risk/size totals stay accurate)
// and the wall time (an Advanced scan shells out to netsh/sc/schtasks + walks standard dirs).
constexpr int kMaxLeftoverItems = 1000;
constexpr int kLeftoverScanTimeoutMs = 5 * 60 * 1000;  // 5 min
constexpr int kMaxReportedFindings = 200;
constexpr int kMaxInventoryWarnings = 200;
constexpr int kMaxPreviewMessages = 200;
constexpr int kMaxAdapters = 100;
constexpr int kMaxConnections = 500;
constexpr int kMaxFirewallRules = 600;
constexpr int kMaxFirewallConflicts = 200;
constexpr int kMaxFirewallGaps = 100;
constexpr int kMaxDnsAnswers = 100;
constexpr int kMaxWifiNetworks = 200;
constexpr int kMaxWifiChannels = 100;
constexpr int kMaxWifiProfiles = 500;
// imaging.list_drives: a machine has at most a handful of physical drives; this cap is a
// defensive ceiling, not an expected limit (truncation would be a real anomaly, still reported).
constexpr int kMaxListedDrives = 64;
// email.search_mbox: cap the hit sample surfaced to the model (the engine bounds the total search
// at sak::email::kMaxSearchResults; total_hits stays accurate). Truncation is reported.
constexpr int kMaxMboxSearchHits = 500;
// files.list_archive: cap the entries surfaced to the model (the engine bounds the total scan)
// and the input archive size (a listing reads only the central directory, so this is generous).
constexpr int kMaxListedArchiveEntries = 2000;
constexpr qint64 kMaxArchiveListBytes = 4LL * 1024 * 1024 * 1024;  // 4 GiB
// files.scan_directory: cap the entry sample surfaced to the model (running totals stay accurate),
// the recursion depth, and the wall time (a full tree walk of a large volume can take minutes).
constexpr int kMaxScanEntriesCollected = 2000;
constexpr int kMaxScanDepth = 64;
constexpr int kScanTimeoutMs = 2 * 60 * 1000;  // 2 min
// files.hash_file: wall-time ceiling on the streamed hash. calculateHash polls the stop_token per
// chunk, so a huge file is cancelled at the deadline and mapped to an honest timeout failure (never
// a partial digest reported as the file's hash).
constexpr int kHashFileTimeoutMs = 2 * 60 * 1000;  // 2 min
// backup.preview_app_data: cap the discovered dirs actually SIZED (each is a full tree walk), the
// dirs surfaced to the model, and the shared wall time across all of them.
constexpr int kMaxAppDataScanDirs = 64;
constexpr int kMaxAppDataReportedPaths = 200;
constexpr int kAppDataPreviewTimeoutMs = 2 * 60 * 1000;  // 2 min
// organizer.preview_organize: cap the per-move sample surfaced (planned counts stay accurate) and
// the wall time. The preview scans only the target's immediate files (non-recursive), so it is
// fast.
constexpr int kMaxPreviewMoveSamples = 500;
constexpr int kOrganizePreviewTimeoutMs = 2 * 60 * 1000;  // 2 min
// In-memory ceiling on how many immediate files the preview collects + plans. Bounds peak RSS on a
// pathological directory (a model-reachable read-only op must not be a memory DoS). Far above any
// real organize target's immediate-file count; beyond it the plan is an honest lower bound
// (plan_truncated). Sibling read ops cap collection the same way (kMaxScanEntriesCollected).
constexpr int kMaxPreviewPlannedFiles = 50'000;
constexpr int kMaxShares = 200;
constexpr int kMaxIsoEditions = 64;
// imaging.scan_recoverable: the model may only REDUCE the scan window/candidate cap below these
// ceilings (which match the engine defaults). The scan reads up to kMaxRecoveryScanBytes into
// memory (one bounded alloc), so the model cannot request a larger spike.
constexpr int kMaxRecoveryScanMb = 512;  // == kFileRecoveryDefaultMaxScanBytes / MiB
constexpr int kMaxRecoveryCandidates = 2048;
constexpr int kMaxReportedRecoveryCandidates = 2048;
// Wall-time ceiling on the carve. The signature scan is O(scan_window * max_candidate_bytes) in the
// worst case (a repeating start marker with no end marker), so the engine polls a cancel flag we
// drive with a DeadlineCanceller; a timed-out scan reports scan_complete=false, never a clean
// empty.
constexpr int kRecoveryScanTimeoutMs = 2 * 60 * 1000;  // 2 min
// PST/OST: open() parses the whole NBT/BBT metadata synchronously, so cap the file size for a
// headless read (larger archives use the GUI, which runs the parse off the UI thread with
// progress). Folder tree and per-folder item payloads are bounded too.
constexpr qint64 kMaxPstBytes = 2LL * 1024 * 1024 * 1024;  // 2 GiB
constexpr int kMaxPstFolders = 2000;
constexpr int kMaxPstItems = 500;
// email.search_pst: wall-time ceiling. The PST search reads message BODIES across every folder, so
// it is O(items); the DeadlineCanceller calls worker.cancel() (the engine polls it) and a cancelled
// search is reported search_complete=false, never a partial result presented as the whole.
constexpr int kPstSearchTimeoutMs = 2 * 60 * 1000;  // 2 min
constexpr int kMaxPstSearchHits = 500;
// email.recover_deleted: cap the recovered-item inventory per source (recoverable + orphaned) and
// bound the extra per-node scan work (readItemDetail for every unreachable message node) with a
// hard wall-time ceiling -- the file-size cap alone leaves a pathological PST scanning for minutes.
constexpr int kMaxRecoverItems = 1000;
constexpr int kRecoverScanTimeoutMs = 5 * 60 * 1000;  // 5 min
constexpr int kDefaultPstItemLimit = 200;
// email.list_profiles: bound the enumerated client-profile inventory and the per-profile data-file
// list so a machine with many linked stores cannot produce an unbounded tool result.
constexpr int kMaxEmailProfiles = 200;
constexpr int kMaxEmailDataFiles = 200;
constexpr int kMaxRestorePoints = 200;
constexpr int kMaxThermalReadings = 64;
constexpr int kMaxUsers = 200;
// diagnostics.run_benchmark: the CPU suite (prime sieve to 10M + 512x512 matrix + 64 MB zlib +
// 256 MB AES + a multi-threaded pass across every core) and the memory suite (256 MB bandwidth
// streams + 64 MB pointer-chase + alloc stress) each run for seconds on typical hardware. Give
// the bridge a generous wall ceiling so a slow-but-real run on modest hardware returns its
// scores instead of a spurious timeout; WorkerBase stop + dtor-join bound it regardless.
constexpr int kBenchmarkTimeoutMs = 5 * 60 * 1000;
// A ping/traceroute/port_scan runs on a worker thread with a hard wall-time ceiling. A
// legit all-timeout run can approach this: port_scan up to 128 ports each costing a connect
// + two fixed 2s banner-grab timers (~4s) is ~512s; traceroute up to 30 hops with per-hop
// reverse-DNS is ~480s. The ceiling sits above both so a slow-but-real scan returns its
// results instead of a spurious timeout; cancel/terminate bound it regardless.
constexpr int kNetworkProbeTimeoutMs = 10 * 60 * 1000;
constexpr int kMaxPingReplies = 64;
constexpr int kMaxTracerouteHops = 64;
// mtr is continuous by nature; headless it must be BOUNDED. cycles*maxHops*timeoutMs is the
// worst-case wall time (a target never reached, every hop timing out every cycle). The clamps
// below cap it at 10*30*1500 = 450s, comfortably under the kNetworkProbeTimeoutMs ceiling, so a
// legit slow run returns its stats instead of a spurious timeout; cancel bounds it regardless.
constexpr int kMaxMtrCycles = 10;
constexpr int kMaxScanPorts = 128;
constexpr int kMaxScanResults = 256;
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

// The partition-op arg mapping (ParsedPartitionOp, parsePartitionOpType,
// supportedPartitionOpTypes, safeByteCount, buildPartitionOpTarget) lives in the
// shared header app_partition_op_parse.h so the preview op (here) and the apply op
// (app_mutating_actions.cpp) can never drift.

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
        buildPartitionOpTarget(args, static_cast<uint32_t>(disk_number), parsed->kind);
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

// software.scan_leftovers
// ---------------------------------------------------------------------------
// Read-only preview of what an uninstall would leave behind: drives the app's OWN LeftoverScanner
// (the same engine the Advanced Uninstall panel + the mutating uninstall_program cleanup phase use)
// at ScanLevel::Moderate (files/folders/registry) or, with advanced=true, ScanLevel::Advanced (adds
// services/tasks/firewall/startup) to find orphaned artifacts for a named program. It only READS +
// classifies (risk Safe/Review/Risky) -- it never deletes anything (the delete step lives in
// UninstallWorker's cleanup, which this op does not touch). The Advanced system-object phases shell
// out (sc/schtasks/netsh) and previously reported a FAILED enumeration as an empty scan
// (fail-open); the engine now returns a LeftoverScanReliability, so a failed phase sets
// scan_complete=false and names the phase in failed_system_phases rather than lying. Intended to
// run AFTER an uninstall; on a still-installed program it will also list the program's LIVE files
// (the scanner cannot tell "orphaned" from "current" without a pre-uninstall snapshot), so the
// count is "items matching this program in leftover locations", not "confirmed orphans".

// clampLine (truncate to a bounded single line) is defined further down; forward-declared so the
// leftover serializers here can reuse it.
QString clampLine(const QString& line);

// Resolve a program NAME to its ProgramInfo for a leftover scan. Unlike the uninstall resolver
// (which requires a SILENT uninstall command, since it is about to RUN the uninstaller), a
// read-only scan works for ANY installed program -- so match purely by display name and refuse only
// on ZERO or AMBIGUOUS matches (distinct by registry key path: an x86/x64 pair or two same-named
// apps would otherwise scan the wrong one's leftovers).
std::optional<AppActionResult> resolveProgramForLeftoverScan(const QString& name,
                                                             ProgramInfo& out) {
    ProgramEnumerator enumerator;
    const QVector<ProgramInfo> programs = enumerator.enumerateRegistryProgramsSync();
    if (programs.isEmpty()) {
        // A running Windows box always has registered uninstall entries, so an empty list is a
        // failed enumeration, not an honest zero (fail-open close).
        return AppActionResult{false,
                               QStringLiteral("Could not read the installed-programs registry"),
                               {}};
    }
    ProgramInfo match;
    QSet<QString> distinct_keys;
    for (const ProgramInfo& program : programs) {
        if (program.displayName.compare(name, Qt::CaseInsensitive) != 0) {
            continue;
        }
        match = program;
        distinct_keys.insert(program.registryKeyPath.isEmpty() ? program.installLocation
                                                               : program.registryKeyPath);
    }
    if (distinct_keys.isEmpty()) {
        return AppActionResult{false,
                               QStringLiteral("No installed program named '%1'").arg(name),
                               {}};
    }
    if (distinct_keys.size() > 1) {
        return AppActionResult{false,
                               QStringLiteral("'%1' matches %2 distinct installed programs; use a "
                                              "more specific name")
                                   .arg(name)
                                   .arg(distinct_keys.size()),
                               {}};
    }
    out = match;
    return std::nullopt;
}

QString leftoverTypeToString(LeftoverItem::Type type) {
    switch (type) {
    case LeftoverItem::Type::File:
        return QStringLiteral("file");
    case LeftoverItem::Type::Folder:
        return QStringLiteral("folder");
    case LeftoverItem::Type::RegistryKey:
        return QStringLiteral("registry_key");
    case LeftoverItem::Type::RegistryValue:
        return QStringLiteral("registry_value");
    case LeftoverItem::Type::Service:
        return QStringLiteral("service");
    case LeftoverItem::Type::ScheduledTask:
        return QStringLiteral("scheduled_task");
    case LeftoverItem::Type::FirewallRule:
        return QStringLiteral("firewall_rule");
    case LeftoverItem::Type::StartupEntry:
        return QStringLiteral("startup_entry");
    case LeftoverItem::Type::ShellExtension:
        return QStringLiteral("shell_extension");
    }
    return QStringLiteral("unknown");
}

QString leftoverRiskToString(LeftoverItem::RiskLevel risk) {
    switch (risk) {
    case LeftoverItem::RiskLevel::Safe:
        return QStringLiteral("safe");
    case LeftoverItem::RiskLevel::Review:
        return QStringLiteral("review");
    case LeftoverItem::RiskLevel::Risky:
        return QStringLiteral("risky");
    }
    return QStringLiteral("unknown");
}

// Totals for the whole item set (accurate even when the surfaced array is truncated).
struct LeftoverTotals {
    int safe = 0;
    int review = 0;
    int risky = 0;
    qint64 total_size = 0;
};

QJsonArray serializeLeftovers(const QVector<LeftoverItem>& items, LeftoverTotals& totals) {
    QJsonArray arr;
    for (const LeftoverItem& item : items) {
        switch (item.risk) {
        case LeftoverItem::RiskLevel::Safe:
            ++totals.safe;
            break;
        case LeftoverItem::RiskLevel::Review:
            ++totals.review;
            break;
        case LeftoverItem::RiskLevel::Risky:
            ++totals.risky;
            break;
        }
        totals.total_size += item.sizeBytes;
        if (arr.size() >= kMaxLeftoverItems) {
            continue;  // keep counting totals, stop growing the sample
        }
        QJsonObject obj{{QStringLiteral("type"), leftoverTypeToString(item.type)},
                        {QStringLiteral("risk"), leftoverRiskToString(item.risk)},
                        {QStringLiteral("path"), clampLine(item.path)},
                        {QStringLiteral("description"), clampLine(item.description)},
                        {QStringLiteral("size_bytes"), static_cast<double>(item.sizeBytes)}};
        if (!item.registryValueName.isEmpty()) {
            obj.insert(QStringLiteral("registry_value_name"), clampLine(item.registryValueName));
        }
        arr.append(obj);
    }
    return arr;
}

// Names of the Advanced system-object phases whose shell-out FAILED (empty result is a failed read,
// not "none found"). Empty for a Moderate scan or when every phase completed.
QStringList failedLeftoverPhases(const LeftoverScanReliability& reliability) {
    QStringList failed;
    if (!reliability.services_ok) {
        failed << QStringLiteral("services");
    }
    if (!reliability.scheduled_tasks_ok) {
        failed << QStringLiteral("scheduled_tasks");
    }
    if (!reliability.firewall_ok) {
        failed << QStringLiteral("firewall_rules");
    }
    if (!reliability.startup_ok) {
        failed << QStringLiteral("startup_entries");
    }
    return failed;
}

QString buildLeftoverMessage(const ProgramInfo& program,
                             int item_count,
                             bool timed_out,
                             const QStringList& failed_phases) {
    if (timed_out) {
        return QStringLiteral("Found at least %1 leftover item(s) for '%2' (scan timed out)")
            .arg(item_count)
            .arg(program.displayName);
    }
    if (!failed_phases.isEmpty()) {
        return QStringLiteral(
                   "Found at least %1 leftover item(s) for '%2' (%3 enumeration failed; "
                   "results are partial)")
            .arg(item_count)
            .arg(program.displayName)
            .arg(failed_phases.join(QStringLiteral(", ")));
    }
    return QStringLiteral("Found %1 leftover item(s) for '%2'")
        .arg(item_count)
        .arg(program.displayName);
}

AppActionResult scanLeftovers(const QJsonObject& args) {
    const QString name = args.value(QStringLiteral("program_name")).toString().trimmed();
    if (name.isEmpty()) {
        return {false, QStringLiteral("scan_leftovers requires a 'program_name' argument"), {}};
    }
    ProgramInfo program;
    if (const std::optional<AppActionResult> error = resolveProgramForLeftoverScan(name, program)) {
        return *error;
    }

    // Moderate = pure Qt filesystem/registry reads. Advanced (advanced=true) ALSO runs the
    // system-object phases that shell out to sc/schtasks/netsh; those used to DROP all results on a
    // nonzero exit / spawn block / their 15s timeout with no error channel (fail-open). The engine
    // now fills a LeftoverScanReliability so a FAILED phase is reported as scan_complete=false +
    // named in failed_system_phases, never as an honest empty scan.
    const bool advanced = args.value(QStringLiteral("advanced")).toBool(false);
    LeftoverScanner scanner(program, advanced ? ScanLevel::Advanced : ScanLevel::Moderate);

    // Run the synchronous scan on this (worker) thread under a wall-time deadline. The scanner
    // polls stopRequested between phases + items AND inside calculateSize's subtree walk, so the
    // DeadlineCanceller flipping the flag actually interrupts a runaway scan. fired() is the honest
    // timeout signal; a per-phase failure (reliability) is the honest fail-open signal. Either
    // makes scan_complete false, so a partial result is never reported as complete.
    std::atomic<bool> stop{false};
    DeadlineCanceller canceller([&stop]() { stop.store(true); }, kLeftoverScanTimeoutMs);
    LeftoverScanReliability reliability;
    const QVector<LeftoverItem> items = scanner.scan(stop, {}, &reliability);
    canceller.finish();
    const bool timed_out = canceller.fired();
    const QStringList failed_phases = failedLeftoverPhases(reliability);

    LeftoverTotals totals;
    const QJsonArray arr = serializeLeftovers(items, totals);
    QJsonArray failed_arr;
    for (const QString& phase : failed_phases) {
        failed_arr.append(phase);
    }
    QJsonObject data{{QStringLiteral("program_name"), program.displayName},
                     {QStringLiteral("install_location"), program.installLocation},
                     {QStringLiteral("scan_level"),
                      advanced ? QStringLiteral("advanced") : QStringLiteral("moderate")},
                     {QStringLiteral("item_count"), static_cast<int>(items.size())},
                     {QStringLiteral("reported_count"), arr.size()},
                     {QStringLiteral("truncated"), static_cast<int>(items.size()) > arr.size()},
                     {QStringLiteral("safe_count"), totals.safe},
                     {QStringLiteral("review_count"), totals.review},
                     {QStringLiteral("risky_count"), totals.risky},
                     {QStringLiteral("total_size_bytes"), static_cast<double>(totals.total_size)},
                     {QStringLiteral("scan_complete"), !timed_out && failed_phases.isEmpty()},
                     {QStringLiteral("failed_system_phases"), failed_arr},
                     {QStringLiteral("items"), arr}};
    return {true,
            buildLeftoverMessage(program, static_cast<int>(items.size()), timed_out, failed_phases),
            data};
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

// Refuse a model-supplied path that is unsafe to stat: a network/UNC/device path (literal) OR a
// reparse point (symlink/junction) whose target could be off-box -- either would leak the NTLM
// hash on the following exists()/isFile()/isDir() stat (see app_action_guards.h). Returns a
// ready-to-return failure, or nullopt. @p noun labels the path in the reparse message. Shared by
// every read-only file/dir op so the guard cannot drift between them or from the mutating module.
std::optional<AppActionResult> refuseUnsafePath(const QString& path,
                                                const QString& op_name,
                                                const QString& noun) {
    if (isNetworkOrDevicePath(path)) {
        return AppActionResult{
            false,
            QStringLiteral("%1 does not allow network/UNC or device paths").arg(op_name),
            {}};
    }
    // Screen the leaf AND every ancestor directory for a reparse point: an ancestor
    // junction/symlink to a UNC target would leak the NTLM hash on the following stat just as a
    // leaf one would (R1).
    if (pathReparseUnsafe(path)) {
        return AppActionResult{
            false,
            QStringLiteral("%1 does not allow a symlink/junction %2 (or one in its path): %3")
                .arg(op_name, noun, path),
            {}};
    }
    return std::nullopt;
}

AppActionResult identifyImage(const QJsonObject& args) {
    const QString path = args.value(QStringLiteral("path")).toString().trimmed();
    if (path.isEmpty()) {
        return {false, QStringLiteral("identify_image requires a 'path' argument"), {}};
    }
    if (const std::optional<AppActionResult> unsafe =
            refuseUnsafePath(path, QStringLiteral("identify_image"), QStringLiteral("path"))) {
        return *unsafe;
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

QString clampField(const QString& value) {
    // Volume-descriptor fields are fixed-width (<=128 bytes) but clamp defensively.
    constexpr int kMaxIsoFieldChars = 256;
    if (value.size() <= kMaxIsoFieldChars) {
        return value;
    }
    return value.left(kMaxIsoFieldChars) + QStringLiteral("...");
}

QJsonObject serializeIsoInfo(const IsoInfo& iso) {
    QJsonArray editions;
    for (const QString& edition : iso.windows_editions) {
        if (editions.size() >= kMaxIsoEditions) {
            break;
        }
        editions.append(clampField(edition));
    }
    return QJsonObject{{QStringLiteral("os_name"), clampField(iso.os_name)},
                       {QStringLiteral("os_version"), clampField(iso.os_version)},
                       {QStringLiteral("os_family"), clampField(iso.os_family)},
                       {QStringLiteral("architecture"), clampField(iso.architecture)},
                       {QStringLiteral("volume_label"), clampField(iso.volume_label)},
                       {QStringLiteral("publisher"), clampField(iso.publisher)},
                       {QStringLiteral("preparer"), clampField(iso.preparer)},
                       {QStringLiteral("application"), clampField(iso.application)},
                       {QStringLiteral("creation_date"), clampField(iso.creation_date)},
                       {QStringLiteral("filesystem"), clampField(iso.filesystem)},
                       {QStringLiteral("is_bootable"), iso.is_bootable},
                       {QStringLiteral("boot_type"), clampField(iso.boot_type)},
                       {QStringLiteral("windows_build"), clampField(iso.windows_build)},
                       {QStringLiteral("windows_editions"), editions},
                       {QStringLiteral("windows_editions_truncated"),
                        iso.windows_editions.size() > editions.size()},
                       {QStringLiteral("distro_name"), clampField(iso.distro_name)},
                       {QStringLiteral("distro_version"), clampField(iso.distro_version)},
                       {QStringLiteral("desktop_env"), clampField(iso.desktop_env)},
                       {QStringLiteral("is_live"), iso.is_live},
                       {QStringLiteral("file_size_bytes"), static_cast<double>(iso.file_size)},
                       {QStringLiteral("volume_size_bytes"), static_cast<double>(iso.volume_size)}};
}

// Analyze an ISO/IMG file via IsoAnalyzer::analyze (a STATIC, thread-safe, local-file-only read of
// the ISO 9660 primary volume descriptor + El Torito boot record + UDF markers -- bounded seeks to
// a few descriptor sectors, never a whole-file scan). Read-only and SYNC. Deeper than
// identify_image (which reads zero bytes and classifies by extension): this reads the actual
// on-disk metadata (volume label, publisher, boot type, OS/distro identity).
//
// Path is validated up front so IsoAnalyzer's own exists()/non-empty preconditions always hold,
// and UNC/device paths are refused (a crafted \\host\share path would pull the read over SMB and
// could leak credentials -- the same guard find_in_files/read_mbox use). A readable file that is
// simply not ISO media is an honest is_valid=false success (the read worked; the content just is
// not an ISO). The pre-open probe converts the COMMON unopenable case (permission denied / already
// locked) into an honest failure; a narrow residual remains (a mid-descriptor I/O error on a
// failing disk, or a TOCTOU vanish after the probe) because IsoAnalyzer::analyze has no error
// channel -- it would surface as an is_valid=false "not ISO media" on a file that was momentarily
// openable. Accepted as low-stakes for this informational read-only op (it gates nothing).
AppActionResult analyzeIso(const QJsonObject& args) {
    const QString path = args.value(QStringLiteral("path")).toString().trimmed();
    if (path.isEmpty()) {
        return {false, QStringLiteral("analyze_iso requires a 'path' argument"), {}};
    }
    if (const std::optional<AppActionResult> unsafe =
            refuseUnsafePath(path, QStringLiteral("analyze_iso"), QStringLiteral("path"))) {
        return *unsafe;
    }
    const QFileInfo info(path);
    if (!info.isFile()) {
        return {false, QStringLiteral("No such image file: %1").arg(path), {}};
    }
    QFile probe(path);
    if (!probe.open(QIODevice::ReadOnly)) {
        return {false, QStringLiteral("Cannot open image file for reading: %1").arg(path), {}};
    }
    probe.close();

    const IsoInfo iso = IsoAnalyzer::analyze(path);
    QJsonObject data = serializeIsoInfo(iso);
    data[QStringLiteral("path")] = info.absoluteFilePath();
    data[QStringLiteral("name")] = info.fileName();
    data[QStringLiteral("is_valid")] = iso.isValid();
    data[QStringLiteral("is_windows_install_media")] = IsoAnalyzer::isWindowsInstallMedia(iso);

    if (!iso.isValid()) {
        return {true,
                QStringLiteral("%1 is readable but not recognized as ISO 9660 / bootable media")
                    .arg(info.fileName()),
                data};
    }
    const QString identity = !iso.os_name.isEmpty()
                                 ? iso.os_name
                                 : (!iso.volume_label.isEmpty() ? iso.volume_label
                                                                : QStringLiteral("unknown media"));
    return {true,
            QStringLiteral("%1: %2%3")
                .arg(info.fileName(),
                     identity,
                     iso.is_bootable ? QStringLiteral(" (bootable)") : QString()),
            data};
}

QString clampLine(const QString& line) {
    if (line.size() <= kMaxMatchLineChars) {
        return line;
    }
    return line.left(kMaxMatchLineChars) + QStringLiteral("...");
}

// imaging.scan_recoverable
// ---------------------------------------------------------------------------
// Read-only signature-carving of an offline disk-image FILE for recoverable (deleted/orphaned)
// files, via the app's OWN FileRecoveryEngine::scanOfflineImage (the Partition Manager
// data-recovery scanner). The source is opened READ-ONLY and NEVER mutated. capture_candidate_bytes
// is forced OFF so only METADATA is returned (format/extension/offset/size/sha256) -- recovered
// file CONTENT is never exfiltrated headless. Bounded: the model may only SHRINK the scan window
// (<= 512 MiB, one alloc) and candidate cap below the engine defaults, never grow them.
// source_opened_read_only is the honesty channel: a failed open is reported as a failure, never as
// an empty "0 candidates" success.
QJsonObject serializeRecoveryCandidate(const FileRecoveryCandidate& candidate) {
    QJsonObject obj{{QStringLiteral("id"), clampLine(candidate.id)},
                    {QStringLiteral("format"), clampLine(candidate.format)},
                    {QStringLiteral("extension"), clampLine(candidate.extension)},
                    {QStringLiteral("offset_bytes"), static_cast<double>(candidate.offset_bytes)},
                    {QStringLiteral("size_bytes"), static_cast<double>(candidate.size_bytes)}};
    if (!candidate.sha256.isEmpty()) {
        obj.insert(QStringLiteral("sha256"), QString::fromLatin1(candidate.sha256.toHex()));
    }
    return obj;
}

// Build the engine scan options from the model args. capture_candidate_bytes is forced OFF; the
// model may only SHRINK the scan window / candidate cap below the engine ceilings, never grow them.
FileRecoveryScanOptions recoveryOptionsFromArgs(const QJsonObject& args, const QString& path) {
    FileRecoveryScanOptions options;
    options.image_path = path;
    options.capture_candidate_bytes = false;  // METADATA only -- never return recovered CONTENT
    if (args.contains(QStringLiteral("max_scan_mb"))) {
        const int mb =
            std::clamp(args.value(QStringLiteral("max_scan_mb")).toInt(kMaxRecoveryScanMb),
                       1,
                       kMaxRecoveryScanMb);
        options.max_scan_bytes = static_cast<uint64_t>(mb) * 1024ULL * 1024ULL;
    }
    if (args.contains(QStringLiteral("max_candidates"))) {
        options.max_candidates =
            std::clamp(args.value(QStringLiteral("max_candidates")).toInt(kMaxRecoveryCandidates),
                       1,
                       kMaxRecoveryCandidates);
    }
    return options;
}

QJsonObject serializeRecoveryScan(const FileRecoveryScanResult& scan, const QFileInfo& info) {
    QJsonArray candidates;
    for (const FileRecoveryCandidate& candidate : scan.candidates) {
        if (candidates.size() >= kMaxReportedRecoveryCandidates) {
            break;
        }
        candidates.append(serializeRecoveryCandidate(candidate));
    }
    QJsonArray warnings;
    for (const QString& warning : scan.warnings) {
        if (warnings.size() >= kMaxInventoryWarnings) {
            break;
        }
        warnings.append(clampLine(warning));
    }
    return QJsonObject{
        {QStringLiteral("image_path"), info.absoluteFilePath()},
        {QStringLiteral("name"), info.fileName()},
        {QStringLiteral("bytes_read"), static_cast<double>(scan.bytes_read)},
        {QStringLiteral("source_read_only"), scan.source_opened_read_only},
        {QStringLiteral("candidate_count"), static_cast<int>(scan.candidates.size())},
        {QStringLiteral("reported_count"), candidates.size()},
        {QStringLiteral("truncated"), static_cast<int>(scan.candidates.size()) > candidates.size()},
        {QStringLiteral("candidates"), candidates},
        {QStringLiteral("warning_count"), static_cast<int>(scan.warnings.size())},
        {QStringLiteral("warnings"), warnings}};
}

AppActionResult scanRecoverable(const QJsonObject& args) {
    const QString path = args.value(QStringLiteral("image_path")).toString().trimmed();
    if (path.isEmpty()) {
        return {false, QStringLiteral("scan_recoverable requires an 'image_path' argument"), {}};
    }
    // refuseUnsafePath FIRST: image_path is opened by name, so a symlink/junction to a UNC target
    // (or a literal UNC path) would open a remote object and leak the NTLM hash. Device paths
    // (\\.\PhysicalDriveN) are also refused, keeping this to offline image FILES.
    if (const std::optional<AppActionResult> unsafe = refuseUnsafePath(
            path, QStringLiteral("scan_recoverable"), QStringLiteral("image_path"))) {
        return *unsafe;
    }
    const QFileInfo info(path);
    if (!info.isFile()) {
        return {false, QStringLiteral("No such image file: %1").arg(path), {}};
    }

    // Carve under a wall-time deadline: the engine polls `stop` in its inner loop, so a runaway
    // O(window * candidate_bytes) scan (a repeating start marker with no end) is interrupted.
    const FileRecoveryScanOptions options = recoveryOptionsFromArgs(args, path);
    std::atomic<bool> stop{false};
    DeadlineCanceller canceller([&stop]() { stop.store(true); }, kRecoveryScanTimeoutMs);
    const FileRecoveryScanResult scan = FileRecoveryEngine::scanOfflineImage(options, &stop);
    canceller.finish();

    if (!scan.source_opened_read_only) {
        const QString reason = scan.warnings.isEmpty()
                                   ? QStringLiteral("could not open the image read-only")
                                   : scan.warnings.first();
        return {false, QStringLiteral("Could not scan %1: %2").arg(info.fileName(), reason), {}};
    }
    // OPEN success is NOT read success: a read of 0 bytes from a non-empty image is a FAILED read
    // (failing media / bad sector at offset 0), not an honest "no recoverable files". Fail closed
    // rather than report a clean empty result.
    if (scan.bytes_read == 0 && info.size() > 0) {
        return {false,
                QStringLiteral("Opened %1 but read 0 bytes from it (the media may be failing)")
                    .arg(info.fileName()),
                {}};
    }
    // scan_complete is honest about COVERAGE: false if the carve was time-limited (scan_cancelled)
    // OR fewer bytes were read than the requested window (a short read = a mid-stream I/O error),
    // so the model never treats a partial scan as an authoritative "these are all the files".
    const uint64_t expected = std::min<uint64_t>(
        static_cast<uint64_t>(std::max<qint64>(0, info.size())), options.max_scan_bytes);
    const bool scan_complete = !scan.scan_cancelled && scan.bytes_read >= expected;

    QJsonObject data = serializeRecoveryScan(scan, info);
    data.insert(QStringLiteral("scan_complete"), scan_complete);
    const QString suffix = scan_complete ? QString()
                                         : QStringLiteral(" (scan incomplete -- partial coverage)");
    return {true,
            QStringLiteral("Found %1 recoverable file candidate(s) in %2%3")
                .arg(scan.candidates.size())
                .arg(info.fileName(), suffix),
            data};
}

// imaging.list_drives
//
// Enumerate the machine's PHYSICAL drives (the Image Flasher's target picker) via the app's OWN
// DriveScanner::enumerateDrivesOnce -- a self-contained, monitor-free enumeration that constructs
// no DriveScanner instance (so it never disturbs a live GUI scanner's singleton or hot-plug
// window). Reports each drive's path, model, size, bus type, mount points, and the flags a
// technician needs before flashing (removable, write-protected, contains-Windows). Read-only:
// every drive handle is opened with 0 access (metadata IOCTLs only), never for reading data, and
// no elevation is ever requested. This is the physical-device lens; partition.list_inventory is
// the partition/volume lens.
QJsonObject serializeDrive(const sak::DriveInfo& drive) {
    QJsonArray mounts;
    for (const QString& mount : drive.mountPoints) {
        mounts.append(mount);
    }
    return QJsonObject{{QStringLiteral("device_path"), drive.devicePath},
                       {QStringLiteral("name"), drive.name},
                       {QStringLiteral("size_bytes"), static_cast<double>(drive.size)},
                       {QStringLiteral("block_size_bytes"), static_cast<double>(drive.blockSize)},
                       {QStringLiteral("bus_type"), drive.busType},
                       {QStringLiteral("is_system"), drive.isSystem},
                       {QStringLiteral("is_removable"), drive.isRemovable},
                       {QStringLiteral("is_read_only"), drive.isReadOnly},
                       {QStringLiteral("mount_points"), mounts},
                       {QStringLiteral("volume_label"), drive.volumeLabel}};
}

AppActionResult listDrives(const QJsonObject& args) {
    const bool removable_only = args.value(QStringLiteral("removable_only")).toBool(false);

    bool enumeration_ok = false;
    const QList<sak::DriveInfo> drives = DriveScanner::enumerateDrivesOnce(enumeration_ok);

    // Every real machine has at least PhysicalDrive0; an empty result means the OS device query
    // failed or no drive could even be opened for metadata. Report that honestly as a failure
    // rather than a misleading "0 drives" success (fail-closed, per the empty-read audit rule).
    if (drives.isEmpty()) {
        return {false,
                QStringLiteral("Could not enumerate any physical drive (the device query failed or "
                               "no drive could be opened for metadata)"),
                {}};
    }

    int removable_count = 0;
    for (const sak::DriveInfo& drive : drives) {
        if (drive.isRemovable) {
            ++removable_count;
        }
    }

    QJsonArray listed;
    for (const sak::DriveInfo& drive : drives) {
        if (removable_only && !drive.isRemovable) {
            continue;
        }
        if (listed.size() >= kMaxListedDrives) {
            break;
        }
        listed.append(serializeDrive(drive));
    }

    const int shown_total = removable_only ? removable_count : drives.size();
    QJsonObject data{
        {QStringLiteral("drive_count"), drives.size()},
        {QStringLiteral("removable_count"), removable_count},
        {QStringLiteral("reported_count"), listed.size()},
        {QStringLiteral("truncated"), shown_total > listed.size()},
        {QStringLiteral("filtered_removable_only"), removable_only},
        // False when QueryDosDeviceW failed and the list is a best-effort probe of drive 0 only,
        // so the model does not treat a possibly-partial list as the complete set of drives.
        {QStringLiteral("enumeration_complete"), enumeration_ok},
        {QStringLiteral("drives"), listed}};
    return {true,
            QStringLiteral("%1 physical drive(s) found (%2 removable)")
                .arg(drives.size())
                .arg(removable_count),
            data};
}

// files.hash_file
//
// Compute a file's SHA-256 (default) or MD5 checksum via the app's OWN file_hasher (chunked,
// memory-efficient streaming, honest std::expected error channel). Read-only. The path goes through
// the shared refuseUnsafePath (UNC/device + leaf-and-ancestor symlink/junction) BEFORE any stat or
// open, so a reparse target can never leak the NTLM hash on the follow. Streamed under a wall-time
// deadline: calculateHash polls the stop_token per chunk and returns operation_cancelled on cancel,
// so a file too large to finish in time is reported as an honest TIMEOUT failure -- never a partial
// digest masquerading as the file's hash.
AppActionResult hashFile(const QJsonObject& args) {
    const QString path = args.value(QStringLiteral("path")).toString().trimmed();
    if (path.isEmpty()) {
        return {false, QStringLiteral("hash_file requires a 'path' argument"), {}};
    }
    if (const std::optional<AppActionResult> unsafe =
            refuseUnsafePath(path, QStringLiteral("hash_file"), QStringLiteral("path"))) {
        return *unsafe;
    }
    const QFileInfo info(path);
    if (!info.isFile()) {
        return {false, QStringLiteral("No such file: %1").arg(path), {}};
    }

    const QString algo_str = args.value(QStringLiteral("algorithm"))
                                 .toString(QStringLiteral("sha256"))
                                 .trimmed()
                                 .toLower();
    hash_algorithm algorithm = hash_algorithm::sha256;
    if (algo_str == QStringLiteral("md5")) {
        algorithm = hash_algorithm::md5;
    } else if (algo_str != QStringLiteral("sha256")) {
        return {false, QStringLiteral("hash_file 'algorithm' must be \"sha256\" or \"md5\""), {}};
    }

    std::stop_source stop;
    DeadlineCanceller deadline([&stop]() { stop.request_stop(); }, kHashFileTimeoutMs);
    const file_hasher hasher(algorithm);
    const std::expected<std::string, error_code> result =
        hasher.calculateHash(std::filesystem::path(path.toStdWString()), nullptr, stop.get_token());
    deadline.finish();

    if (!result) {
        if (deadline.fired() || result.error() == error_code::operation_cancelled) {
            return {false,
                    QStringLiteral("Hashing %1 exceeded the %2s time limit (file too large to hash "
                                   "within the deadline)")
                        .arg(info.fileName())
                        .arg(kHashFileTimeoutMs / 1000),
                    {}};
        }
        const std::string_view reason = sak::to_string(result.error());
        return {false,
                QStringLiteral("Could not hash %1: %2")
                    .arg(info.fileName(),
                         QString::fromUtf8(reason.data(), static_cast<qsizetype>(reason.size()))),
                {}};
    }

    const QString hex = QString::fromStdString(result.value());
    QJsonObject data{{QStringLiteral("path"), info.absoluteFilePath()},
                     {QStringLiteral("algorithm"), algo_str},
                     {QStringLiteral("hash"), hex},
                     {QStringLiteral("size_bytes"), static_cast<double>(info.size())}};
    return {true, QStringLiteral("%1 (%2): %3").arg(info.fileName(), algo_str, hex), data};
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
    config.skip_symlinks = true;  // R2: never open a planted symlink's (possibly UNC) target
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
    if (const std::optional<AppActionResult> unsafe =
            refuseUnsafePath(root, QStringLiteral("search"), QStringLiteral("root_path"))) {
        return *unsafe;
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

// ---------------------------------------------------------------------------
// organizer.find_duplicates
// ---------------------------------------------------------------------------

// Bound what a single headless duplicate scan reports back to the model. The scan still examines
// every file under the roots (that is the point), but the serialized result is capped so a
// directory full of duplicates cannot produce a multi-megabyte tool result.
constexpr int kMaxReportedDupGroups = 200;
constexpr int kMaxDupFilesPerGroup = 100;
constexpr int kMaxDupScanDirs = 64;
// Hashing reads every file in full, so it is heavier than a text search; give the bridge a
// 10-minute wall ceiling (with a real cooperative cancel) so an enormous tree cannot block the
// worker thread indefinitely. The assistant tool layer applies its own outer timeout too.
constexpr int kDupScanTimeoutMs = 10 * 60 * 1000;

QJsonObject serializeDuplicateGroup(const DuplicateFinderWorker::DuplicateGroup& group) {
    QJsonArray files;
    for (const QString& path : group.file_paths) {
        if (files.size() >= kMaxDupFilesPerGroup) {
            break;
        }
        files.append(path);
    }
    return QJsonObject{{QStringLiteral("hash"), group.hash},
                       {QStringLiteral("file_size_bytes"), static_cast<double>(group.file_size)},
                       {QStringLiteral("wasted_bytes"), static_cast<double>(group.wasted_space)},
                       {QStringLiteral("file_count"), static_cast<int>(group.file_paths.size())},
                       {QStringLiteral("files"), files},
                       {QStringLiteral("files_truncated"),
                        group.file_paths.size() > kMaxDupFilesPerGroup}};
}

// Serialize the duplicate groups and compute the totals (returned via out-params for the message).
QJsonObject serializeDuplicates(const std::vector<DuplicateFinderWorker::DuplicateGroup>& groups,
                                int& total_duplicate_files,
                                qint64& total_wasted_bytes) {
    QJsonArray arr;
    total_duplicate_files = 0;
    total_wasted_bytes = 0;
    for (const DuplicateFinderWorker::DuplicateGroup& group : groups) {
        total_duplicate_files += std::max(0, static_cast<int>(group.file_paths.size()) - 1);
        total_wasted_bytes += group.wasted_space;
        if (arr.size() < kMaxReportedDupGroups) {
            arr.append(serializeDuplicateGroup(group));
        }
    }
    return QJsonObject{
        {QStringLiteral("total_groups"), static_cast<int>(groups.size())},
        {QStringLiteral("total_duplicate_files"), total_duplicate_files},
        {QStringLiteral("total_wasted_bytes"), static_cast<double>(total_wasted_bytes)},
        {QStringLiteral("reported_groups"), static_cast<int>(arr.size())},
        {QStringLiteral("report_truncated"), arr.size() < static_cast<int>(groups.size())},
        {QStringLiteral("groups"), arr}};
}

QStringList dupDirectoriesFromArgs(const QJsonObject& args) {
    QStringList dirs;
    for (const QJsonValue& value : args.value(QStringLiteral("directories")).toArray()) {
        const QString dir = value.toString().trimmed();
        if (!dir.isEmpty()) {
            dirs.append(dir);
        }
    }
    return dirs;
}

std::optional<AppActionResult> validateDupInputs(const QStringList& dirs) {
    if (dirs.isEmpty()) {
        return AppActionResult{
            false, QStringLiteral("find_duplicates requires a non-empty 'directories' array"), {}};
    }
    if (dirs.size() > kMaxDupScanDirs) {
        return AppActionResult{
            false,
            QStringLiteral("find_duplicates accepts at most %1 directories").arg(kMaxDupScanDirs),
            {}};
    }
    for (const QString& dir : dirs) {
        // Reject UNC/device roots AND reparse points -- the same SMB/credential-leak guard the
        // other path-taking read-only ops use (a symlink to a UNC target would leak on the stat
        // below). A duplicate scan also HASHES file contents, so a network root would pull large
        // amounts of data over the wire.
        if (const std::optional<AppActionResult> unsafe = refuseUnsafePath(
                dir, QStringLiteral("find_duplicates"), QStringLiteral("directory"))) {
            return *unsafe;
        }
        // Fail CLOSED on a bad directory instead of letting the worker silently skip it: the worker
        // just logs+continues past a nonexistent/non-directory path, so a wrong path would
        // otherwise return a dishonest "0 duplicates" success indistinguishable from a genuinely
        // clean scan.
        const QFileInfo info(dir);
        if (!info.exists() || !info.isDir()) {
            return AppActionResult{
                false,
                QStringLiteral("find_duplicates: not an existing directory: %1").arg(dir),
                {}};
        }
    }
    return std::nullopt;
}

AppActionResult findDuplicates(const QJsonObject& args) {
    const QStringList dirs = dupDirectoriesFromArgs(args);
    if (const std::optional<AppActionResult> error = validateDupInputs(dirs)) {
        return *error;
    }

    DuplicateFinderWorker::Config config;
    config.scanDirectories = dirs;  // QStringList is a QList<QString> == QVector<QString> in Qt6
    config.recursive_scan = args.value(QStringLiteral("recursive")).toBool(true);
    config.parallel_hashing = true;
    config.use_file_system_target = false;  // never the raw/image bridge path (GUI-only)
    config.skip_symlinks = true;  // R2: never hash a planted symlink's (possibly UNC) target
    const double min_size = args.value(QStringLiteral("minimum_file_size")).toDouble(1.0);
    config.minimum_file_size = min_size > 0.0 ? static_cast<qint64>(min_size) : 0;

    // Drive the WORKER directly (not a controller) via the async->sync bridge, exactly like
    // search.find_in_files: the worker runs on its own thread, hashing bounded by kDupScanTimeoutMs
    // and cooperatively cancellable (checkStop). Declared before the invocation so it is destroyed
    // LAST -- on a timeout ~WorkerBase stops+joins its thread, reaping a still-running scan.
    DuplicateFinderWorker worker(config);
    AsyncActionInvocation inv(kDupScanTimeoutMs);
    QObject::connect(&worker, &WorkerBase::finished, inv.context(), [&inv, &worker]() {
        int total_dupes = 0;
        qint64 total_wasted = 0;
        const QJsonObject data =
            serializeDuplicates(worker.duplicateGroups(), total_dupes, total_wasted);
        const QString message =
            QStringLiteral(
                "Found %1 duplicate group(s), %2 redundant file(s), %3 bytes reclaimable")
                .arg(static_cast<int>(worker.duplicateGroups().size()))
                .arg(total_dupes)
                .arg(total_wasted);
        inv.finish({true, message, data});
    });
    QObject::connect(&worker,
                     &WorkerBase::failed,
                     inv.context(),
                     [&inv](int, const QString& error) { inv.finish({false, error, {}}); });

    const AppActionResult result = inv.run([&worker]() { worker.start(); });
    worker.requestStop();  // cooperative stop before teardown (no-op if already finished)
    return result;
}

// organizer.preview_organize
// ---------------------------------------------------------------------------
// Read-only DRY RUN of organizer.organize_directory: drive the SAME OrganizerWorker with
// preview_mode=true so it scans the target's immediate files, categorizes them by the model's
// category_mapping, and PLANS the moves WITHOUT touching a single file (execute() returns right
// after planning). Reports the plan (per-category counts, collisions, a bounded per-move sample)
// so the model can show the user what an apply would do first -- symmetric to
// partition.preview_operation vs partition.apply_operation. A failed scan surfaces the worker's
// failed() signal as an honest failure (not an empty-success).

// Validate the preview inputs. Mirrors the apply's validateOrganizeInputs but omits the
// collision_strategy check (a preview never moves, so the strategy is irrelevant to the plan).
std::optional<AppActionResult> validatePreviewOrganizeInputs(
    const QString& target, const QMap<QString, QStringList>& mapping) {
    if (target.isEmpty()) {
        return AppActionResult{false,
                               QStringLiteral("preview_organize requires a 'target_directory'"),
                               {}};
    }
    if (const std::optional<AppActionResult> unsafe = refuseUnsafePath(
            target, QStringLiteral("preview_organize"), QStringLiteral("target_directory"))) {
        return *unsafe;
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
                                   "preview_organize requires a non-empty 'category_mapping' of "
                                   "category -> file extensions"),
                               {}};
    }
    if (const QString unsafe = firstUnsafeCategory(mapping); !unsafe.isEmpty()) {
        return AppActionResult{
            false,
            QStringLiteral("category name '%1' is not a valid subfolder name (no path separators, "
                           "':', '.' or '..')")
                .arg(unsafe),
            {}};
    }
    return std::nullopt;
}

QJsonObject serializePreviewPlan(const std::vector<OrganizerWorker::MoveOperation>& ops,
                                 const QString& target,
                                 bool plan_truncated) {
    QMap<QString, int> category_counts;
    int collisions = 0;
    QJsonArray moves;
    for (const OrganizerWorker::MoveOperation& op : ops) {
        ++category_counts[op.category];
        if (op.would_overwrite) {
            ++collisions;
        }
        if (moves.size() < kMaxPreviewMoveSamples) {
            moves.append(QJsonObject{{QStringLiteral("source"),
                                      clampLine(QString::fromStdString(op.source.string()))},
                                     {QStringLiteral("destination"),
                                      clampLine(QString::fromStdString(op.destination.string()))},
                                     {QStringLiteral("category"), op.category},
                                     {QStringLiteral("would_overwrite"), op.would_overwrite}});
        }
    }
    QJsonObject categories;
    for (auto it = category_counts.begin(); it != category_counts.end(); ++it) {
        categories.insert(it.key(), it.value());
    }
    // plan_truncated: the scan hit kMaxPreviewPlannedFiles, so files_to_move/categories/collisions
    // count only the scanned prefix and are an honest LOWER BOUND, not the full directory.
    return QJsonObject{{QStringLiteral("target_directory"), target},
                       {QStringLiteral("files_to_move"), static_cast<int>(ops.size())},
                       {QStringLiteral("category_count"), static_cast<int>(category_counts.size())},
                       {QStringLiteral("collisions"), collisions},
                       {QStringLiteral("categories"), categories},
                       {QStringLiteral("moves"), moves},
                       {QStringLiteral("moves_truncated"),
                        static_cast<int>(ops.size()) > moves.size()},
                       {QStringLiteral("plan_truncated"), plan_truncated}};
}

AppActionResult previewOrganize(const QJsonObject& args) {
    const QString target = args.value(QStringLiteral("target_directory")).toString().trimmed();
    const QMap<QString, QStringList> mapping =
        categoryMappingFromArgs(args.value(QStringLiteral("category_mapping")).toObject());
    if (const std::optional<AppActionResult> error = validatePreviewOrganizeInputs(target,
                                                                                   mapping)) {
        return *error;
    }

    OrganizerWorker::Config config;
    config.target_directory = target;
    config.category_mapping = mapping;
    config.preview_mode = true;  // DRY RUN: scan + plan only, never move a file
    config.create_subdirectories = false;
    config.collision_strategy = QStringLiteral("skip");
    config.max_preview_files = kMaxPreviewPlannedFiles;  // bound peak memory on a huge directory

    // Drive the WORKER directly via the async->sync bridge, like organize_directory but in preview
    // mode. Declared before the invocation so it is destroyed LAST: on a timeout ~WorkerBase stops
    // and joins the thread after run() returns, so a still-running scan is reaped, never leaked.
    OrganizerWorker worker(config);
    AsyncActionInvocation inv(kOrganizePreviewTimeoutMs);
    QObject::connect(&worker, &WorkerBase::finished, inv.context(), [&inv, &worker, target]() {
        const bool truncated = worker.planTruncated();
        const QJsonObject data =
            serializePreviewPlan(worker.plannedOperations(), target, truncated);
        const int files = static_cast<int>(worker.plannedOperations().size());
        const QString message =
            truncated ? QStringLiteral(
                            "Preview: at least %1 file(s) would be organized (scan stopped at "
                            "the preview limit)")
                            .arg(files)
                      : QStringLiteral("Preview: %1 file(s) would be organized").arg(files);
        inv.finish({true, message, data});
    });
    QObject::connect(
        &worker, &WorkerBase::failed, inv.context(), [&inv](int, const QString& error) {
            inv.finish({false, error.isEmpty() ? QStringLiteral("Preview failed") : error, {}});
        });

    const AppActionResult result = inv.run([&worker]() { worker.start(); });
    worker.requestStop();  // cooperative stop before teardown (no-op if already finished)
    return result;
}

QJsonObject previewOrganizeParamsSchema() {
    QJsonObject mapping_prop{
        {QStringLiteral("type"), QStringLiteral("object")},
        {QStringLiteral("additionalProperties"),
         QJsonObject{{QStringLiteral("type"), QStringLiteral("array")},
                     {QStringLiteral("items"),
                      QJsonObject{{QStringLiteral("type"), QStringLiteral("string")}}}}},
        {QStringLiteral("description"),
         QStringLiteral("Map of category name -> list of file extensions (no dot); the preview "
                        "reports which files would move into a same-named subfolder of the "
                        "target")}};
    QJsonObject target_prop{
        {QStringLiteral("type"), QStringLiteral("string")},
        {QStringLiteral("description"),
         QStringLiteral("Absolute path of the directory to preview organizing")}};
    QJsonObject properties{{QStringLiteral("target_directory"), target_prop},
                           {QStringLiteral("category_mapping"), mapping_prop}};
    return QJsonObject{{QStringLiteral("type"), QStringLiteral("object")},
                       {QStringLiteral("properties"), properties},
                       {QStringLiteral("required"),
                        QJsonArray{QStringLiteral("target_directory"),
                                   QStringLiteral("category_mapping")}},
                       {QStringLiteral("additionalProperties"), false}};
}

// List a .zip archive's contents WITHOUT extracting. Drives the app's OWN
// FileExplorerArchiveService (reads only the central directory -- no entry payloads, no writes), so
// this is a cheap read-only inspection that pairs with files.compress_zip/extract_zip. The archive
// path goes through the shared refuseUnsafePath (UNC/device + symlink/junction) before any
// following stat. ArchiveListing's ok flag is the value channel: an unreadable/oversize archive is
// an honest failure, not empty.
AppActionResult listArchive(const QJsonObject& args) {
    const QString path = args.value(QStringLiteral("archive_path")).toString().trimmed();
    if (path.isEmpty()) {
        return {false, QStringLiteral("list_archive requires an 'archive_path' argument"), {}};
    }
    if (const std::optional<AppActionResult> unsafe = refuseUnsafePath(
            path, QStringLiteral("list_archive"), QStringLiteral("archive_path"))) {
        return *unsafe;
    }
    const QFileInfo info(path);
    if (!info.isFile()) {
        return {false, QStringLiteral("No such archive file: %1").arg(path), {}};
    }
    if (info.size() > kMaxArchiveListBytes) {
        return {false,
                QStringLiteral("Archive is too large for a headless listing (%1 bytes > %2 limit)")
                    .arg(info.size())
                    .arg(kMaxArchiveListBytes),
                {}};
    }

    const ArchiveListing listing = FileExplorerArchiveService::listEntries(path);
    if (!listing.ok) {
        const QString reason = listing.blockers.isEmpty()
                                   ? QStringLiteral("unreadable or unsupported archive")
                                   : listing.blockers.first();
        return {false, QStringLiteral("Could not list %1: %2").arg(info.fileName(), reason), {}};
    }

    QJsonArray entries;
    for (const ArchiveEntryInfo& entry : listing.entries) {
        if (entries.size() >= kMaxListedArchiveEntries) {
            break;
        }
        entries.append(QJsonObject{{QStringLiteral("path"), clampLine(entry.path)},
                                   {QStringLiteral("size_bytes"), static_cast<double>(entry.size)},
                                   {QStringLiteral("is_dir"), entry.is_dir}});
    }
    QJsonObject data{{QStringLiteral("path"), info.absoluteFilePath()},
                     {QStringLiteral("name"), info.fileName()},
                     {QStringLiteral("total_entries"), listing.total_entries},
                     {QStringLiteral("reported_entries"), entries.size()},
                     {QStringLiteral("truncated"), listing.total_entries > entries.size()},
                     {QStringLiteral("total_uncompressed_bytes"),
                      static_cast<double>(listing.total_uncompressed_bytes)},
                     {QStringLiteral("entries"), entries}};
    return {true,
            QStringLiteral("Archive '%1': %2 entr%3")
                .arg(info.fileName())
                .arg(listing.total_entries)
                .arg(listing.total_entries == 1 ? QStringLiteral("y") : QStringLiteral("ies")),
            data};
}

QJsonObject listArchiveParamsSchema() {
    QJsonObject properties{
        {QStringLiteral("archive_path"),
         QJsonObject{{QStringLiteral("type"), QStringLiteral("string")},
                     {QStringLiteral("description"),
                      QStringLiteral("Absolute path to the .zip archive to list")}}}};
    return QJsonObject{{QStringLiteral("type"), QStringLiteral("object")},
                       {QStringLiteral("properties"), properties},
                       {QStringLiteral("required"), QJsonArray{QStringLiteral("archive_path")}},
                       {QStringLiteral("additionalProperties"), false}};
}

// One entry collected during a directory scan (see runDirectoryScan).
struct ScanCollectEntry {
    QString rel_path;
    qint64 size{0};
    bool is_dir{false};
};

// Accumulated result of a bounded directory scan.
struct DirScanOutcome {
    std::vector<ScanCollectEntry> entries;  // capped sample of entries
    qint64 files{0};
    qint64 directories{0};
    qint64 total_size{0};
    qint64 errors{0};      // engine errors_encountered (only meaningful when `complete`)
    bool complete{false};  // the walk finished fully (not cancelled / timed out)
    bool timed_out{false};
    bool failed{false};
    QString error;
};

// Build scan_options from the model args. follow_symlinks stays FALSE, so the scanner never
// descends a symlinked directory and runs cycle detection -- the walk cannot be steered off-box
// through a planted directory symlink. skip_symlinks is TRUE (R2 fix), so a symlinked FILE entry is
// dropped by a non-following symlink_status check BEFORE its size stat, closing the UNC-leak vector
// that the find_duplicates/search walks also now close. calculate_sizes is off
// (sizes are computed in the callback to avoid a double stat) -- which means the engine's own
// min_file_size filter (gated on calculate_sizes) would be a no-op, so min_file_size is applied in
// the callback instead. include/exclude patterns are honored by the engine unconditionally.
sak::scan_options dirScanOptionsFromArgs(const QJsonObject& args) {
    sak::scan_options options;
    options.recursive = args.value(QStringLiteral("recursive")).toBool(true);
    options.follow_symlinks = false;
    options.skip_symlinks = true;  // R2: never resolve a planted symlink's (possibly UNC) target
    options.calculate_sizes = false;
    options.type_filter = sak::file_type_filter::all;
    const int depth = args.value(QStringLiteral("max_depth")).toInt(0);
    options.max_depth = static_cast<std::size_t>(std::clamp(depth, 0, kMaxScanDepth));
    for (const QJsonValue& value : args.value(QStringLiteral("include_patterns")).toArray()) {
        const QString pattern = value.toString().trimmed();
        if (!pattern.isEmpty()) {
            options.include_patterns.push_back(pattern.toStdString());
        }
    }
    return options;
}

// Run a bounded directory scan under a wall-time ceiling. Running totals are accumulated in the
// callback so they survive a deadline cancel (scan() returns operation_cancelled and DROPS its own
// statistics on cancel). Only the first kMaxScanEntriesCollected entries are kept for the payload.
DirScanOutcome runDirectoryScan(const std::filesystem::path& root, const QJsonObject& args) {
    DirScanOutcome out;
    std::stop_source stop;
    DeadlineCanceller deadline([&stop]() { stop.request_stop(); }, kScanTimeoutMs);

    const qint64 min_size =
        static_cast<qint64>(args.value(QStringLiteral("min_file_size")).toDouble(0));
    sak::scan_options options = dirScanOptionsFromArgs(args);
    options.callback = [&out, &root, min_size](const std::filesystem::path& path,
                                               bool is_dir) -> bool {
        qint64 size = 0;
        if (is_dir) {
            ++out.directories;
        } else {
            std::error_code ec;
            const auto bytes = std::filesystem::file_size(path, ec);
            size = ec ? 0 : static_cast<qint64>(bytes);
            if (min_size > 0 && size < min_size) {
                return true;  // below the requested size floor: excluded from counts + sample
            }
            ++out.files;
            out.total_size += size;
        }
        if (out.entries.size() < static_cast<std::size_t>(kMaxScanEntriesCollected)) {
            // Convert via the WIDE path (symmetric with how root is built); the narrow
            // generic_string() is the ANSI code page on Windows and QString::fromStdString would
            // decode it as UTF-8, corrupting any non-ASCII filename.
            out.entries.push_back(
                {QString::fromStdWString(path.lexically_relative(root).generic_wstring()),
                 size,
                 is_dir});
        }
        return true;  // keep walking for accurate totals; the deadline bounds wall time
    };

    sak::file_scanner scanner;
    const auto result = scanner.scan(root, options, stop.get_token());
    deadline.finish();
    out.timed_out = deadline.fired();
    if (result.has_value()) {
        // Full walk: capture errors_encountered so a partial tree (unreadable/errored subdirs the
        // engine counts but does not fail on) is flagged, not reported as a complete total.
        out.complete = true;
        out.errors = static_cast<qint64>(result->errors_encountered);
    } else if (result.error() != sak::error_code::operation_cancelled) {
        out.failed = true;
        out.error = QString::fromUtf8(sak::to_string(result.error()));
    }
    return out;
}

// Scan a directory tree and report a bounded entry sample + disk-usage totals (files/dirs/bytes).
// Drives the app's OWN file_scanner (synchronous, std::stop_token, follow_symlinks off with cycle
// detection, skip_symlinks on). root_path goes through the shared refuseUnsafePath (UNC/device +
// reparse) then isDir. read_only -> ungated. Totals are the true walk counts for the NON-reparse
// tree: reparse-point entries (symlinks, junctions, and cloud/dedup placeholders) are deliberately
// excluded and never resolved -- a security-conservative choice (a following stat could leak a UNC
// credential hash or hydrate an online-only cloud file), so on a heavily symlinked/cloud-backed
// tree the counts are a lower bound. entries[] is the first N (truncation + timed_out reported), so
// a huge tree yields honest summary numbers without an unbounded payload. Assemble the model-facing
// result from a scan outcome (capped entry array + honest totals + completeness flags).
// scan_complete is true only when the walk FINISHED and hit no errors, so an undercounted partial
// tree (timed out or unreadable subdirs) is never reported as authoritative.
AppActionResult buildDirScanResult(const QFileInfo& info,
                                   const QString& root,
                                   const DirScanOutcome& out) {
    QJsonArray entries;
    for (const ScanCollectEntry& entry : out.entries) {
        entries.append(QJsonObject{{QStringLiteral("path"), clampLine(entry.rel_path)},
                                   {QStringLiteral("size_bytes"), static_cast<double>(entry.size)},
                                   {QStringLiteral("is_dir"), entry.is_dir}});
    }
    const qint64 total_entries = out.files + out.directories;
    const bool scan_complete = out.complete && !out.timed_out && out.errors == 0;
    QJsonObject data{{QStringLiteral("root_path"), info.absoluteFilePath()},
                     {QStringLiteral("files_found"), static_cast<double>(out.files)},
                     {QStringLiteral("directories_found"), static_cast<double>(out.directories)},
                     {QStringLiteral("total_size_bytes"), static_cast<double>(out.total_size)},
                     {QStringLiteral("reported_entries"), entries.size()},
                     {QStringLiteral("truncated"), total_entries > entries.size()},
                     {QStringLiteral("errors_encountered"), static_cast<double>(out.errors)},
                     {QStringLiteral("scan_complete"), scan_complete},
                     {QStringLiteral("timed_out"), out.timed_out},
                     {QStringLiteral("entries"), entries}};
    QString message = QStringLiteral("Scanned '%1': %2 file(s), %3 dir(s), %4 bytes")
                          .arg(info.fileName().isEmpty() ? root : info.fileName())
                          .arg(out.files)
                          .arg(out.directories)
                          .arg(out.total_size);
    if (out.timed_out) {
        message += QStringLiteral(" (scan hit the time limit; results are partial)");
    } else if (out.errors > 0) {
        message += QStringLiteral(" (%1 subdirectorie(s) could not be read; totals are partial)")
                       .arg(out.errors);
    }
    return {true, message, data};
}

AppActionResult scanDirectory(const QJsonObject& args) {
    const QString root = args.value(QStringLiteral("root_path")).toString().trimmed();
    if (root.isEmpty()) {
        return {false, QStringLiteral("scan_directory requires a 'root_path' argument"), {}};
    }
    if (const std::optional<AppActionResult> unsafe =
            refuseUnsafePath(root, QStringLiteral("scan_directory"), QStringLiteral("root_path"))) {
        return *unsafe;
    }
    const QFileInfo info(root);
    if (!info.isDir()) {
        return {false, QStringLiteral("root_path is not an existing directory: %1").arg(root), {}};
    }

    const DirScanOutcome out = runDirectoryScan(std::filesystem::path(root.toStdWString()), args);
    if (out.failed) {
        return {false, QStringLiteral("Directory scan failed: %1").arg(out.error), {}};
    }
    return buildDirScanResult(info, root, out);
}

QJsonObject scanDirectoryParamsSchema() {
    QJsonObject patterns_prop{
        {QStringLiteral("type"), QStringLiteral("array")},
        {QStringLiteral("items"), QJsonObject{{QStringLiteral("type"), QStringLiteral("string")}}},
        {QStringLiteral("description"),
         QStringLiteral("Optional filename glob patterns to include (e.g. \"*.log\")")}};
    QJsonObject properties{
        {QStringLiteral("root_path"),
         QJsonObject{{QStringLiteral("type"), QStringLiteral("string")},
                     {QStringLiteral("description"),
                      QStringLiteral("Absolute path of the directory to scan")}}},
        {QStringLiteral("recursive"),
         QJsonObject{{QStringLiteral("type"), QStringLiteral("boolean")},
                     {QStringLiteral("description"),
                      QStringLiteral("Recurse into subdirectories (default true)")}}},
        {QStringLiteral("max_depth"),
         QJsonObject{{QStringLiteral("type"), QStringLiteral("integer")},
                     {QStringLiteral("description"),
                      QStringLiteral("Max recursion depth (0 = unlimited, capped at 64)")}}},
        {QStringLiteral("min_file_size"),
         QJsonObject{{QStringLiteral("type"), QStringLiteral("integer")},
                     {QStringLiteral("description"),
                      QStringLiteral("Only include files at least this many bytes")}}},
        {QStringLiteral("include_patterns"), patterns_prop}};
    return QJsonObject{{QStringLiteral("type"), QStringLiteral("object")},
                       {QStringLiteral("properties"), properties},
                       {QStringLiteral("required"), QJsonArray{QStringLiteral("root_path")}},
                       {QStringLiteral("additionalProperties"), false}};
}

// backup.preview_app_data
// ---------------------------------------------------------------------------
// Read-only preview of what a user-data backup would capture for a named app. Drives the app's OWN
// UserDataManager::scanForAppData, which substring-matches the names of the REAL immediate subdirs
// of fixed standard data locations (AppData Local/Roaming, ProgramData, Documents, Home). The op
// takes an app NAME (rejected if path-shaped) and never joins it into a path or stats it, so a
// prompt-injected model cannot steer discovery at an arbitrary/off-box location (see
// discoverAppDataDirs for why discoverAppDataPaths is deliberately avoided). Each discovered
// directory's on-disk size is summed with the already-hardened file_scanner (skip_symlinks on,
// follow off) under ONE shared wall-time deadline; a directory that is (or sits under) a reparse
// point or a UNC/device path is screened out, reported separately, and marks the total a lower
// bound
// -- never followed. Nothing is copied or written -- this only reports the paths + total size a
// backup would include, so the technician can size the job before running it.

struct AppDataPreviewOutcome {
    QStringList scanned;    ///< directories actually sized
    QStringList skipped;    ///< directories screened out (reparse / UNC) -- never followed
    qint64 total_size{0};   ///< summed on-disk bytes across scanned dirs
    qint64 file_count{0};   ///< summed regular-file count
    bool truncated{false};  ///< discovery returned more dirs than the per-op scan cap
    bool timed_out{false};  ///< the shared deadline fired mid-walk
    bool complete{true};    ///< every scanned walk finished with no unreadable subtree
};

// Drop any path nested under another kept path (case-insensitive, '/'-boundary). A recursive size
// walk of an ANCESTOR already includes its descendants, so keeping both would double-count that
// subtree. scanForAppData matches on the FULL path string, so a name that is a substring of a
// standard base's own path (e.g. "Documents") co-returns an ancestor and its children; this filter
// keeps only the shallowest of any nested chain so each byte is counted once. @p paths must already
// be cleanPath-normalized (so nesting is a pure lexical '/' prefix, not a separator/case artifact).
QStringList dropNestedPaths(QStringList paths) {
    std::sort(paths.begin(), paths.end(), [](const QString& lhs, const QString& rhs) {
        return lhs.size() < rhs.size();  // ancestors (shorter) first
    });
    QStringList kept;
    for (const QString& path : paths) {
        const QString lower = path.toLower();
        const bool nested =
            std::any_of(kept.cbegin(), kept.cend(), [&lower](const QString& keeper) {
                const QString keeper_lower = keeper.toLower();
                return lower == keeper_lower || lower.startsWith(keeper_lower + QLatin1Char('/'));
            });
        if (!nested) {
            kept.append(path);
        }
    }
    return kept;
}

// Discover the candidate data directories for @p app_name (normalized, deduplicated,
// non-overlapping).
//
// SECURITY: only scanForAppData is used. It substring-matches the names of the REAL immediate
// subdirectories of the fixed standard bases (QDirIterator listing) -- app_name never becomes a
// path or a stat target. UserDataManager::discoverAppDataPaths is DELIBERATELY NOT used: it does
// QDir(base).filePath(variant) then QDir(path).exists(), and QDir::filePath returns an ABSOLUTE
// argument (e.g. a UNC app_name like "\\\\host\\share") VERBATIM, so its following exists() would
// perform an outbound SMB/NTLM handshake to a model-supplied host and leak the credential hash --
// with no local plant and outside any deadline. scanForAppData has neither the path-join nor the
// exists(); a listed directory that is itself a reparse point is screened on the sizing side
// (pathReparseUnsafe in runAppDataPreview) before file_scanner ever follows it.
//
// Paths are QDir::cleanPath-normalized (unifies separators, resolves ".."/"." LEXICALLY -- no
// filesystem access, no symlink following), deduped case-INSENSITIVELY (Windows FS), then
// ancestor-filtered so an overlapping subtree is sized once.
QStringList discoverAppDataDirs(const QString& app_name) {
    UserDataManager manager;
    const QStringList raw = manager.scanForAppData(app_name);
    QStringList normalized;
    QSet<QString> seen;
    for (const QString& path : raw) {
        const QString clean = QDir::cleanPath(path);
        if (clean.isEmpty()) {
            continue;
        }
        const QString key = clean.toLower();  // Windows paths are case-insensitive
        if (!seen.contains(key)) {
            seen.insert(key);
            normalized.append(clean);
        }
    }
    return dropNestedPaths(normalized);
}

// Sum one directory's size via the hardened file_scanner under the shared stop token. Returns false
// if the walk errored/cancelled or hit any unreadable subtree (so the caller marks the total
// partial). skip_symlinks keeps a planted symlink FILE inside the tree from being followed to a UNC
// target; the ROOT is reparse-screened by the caller before we get here.
bool sumAppDataDirSize(const QString& path,
                       const std::stop_token& stop,
                       qint64& size_out,
                       qint64& files_out) {
    sak::scan_options options;
    options.recursive = true;
    options.follow_symlinks = false;
    options.skip_symlinks = true;
    options.calculate_sizes = true;
    sak::file_scanner scanner;
    const auto result = scanner.scan(std::filesystem::path(path.toStdWString()), options, stop);
    if (!result.has_value()) {
        return false;
    }
    size_out += static_cast<qint64>(result->total_size);
    files_out += static_cast<qint64>(result->files_found);
    return result->errors_encountered == 0;
}

AppDataPreviewOutcome runAppDataPreview(const QStringList& paths) {
    AppDataPreviewOutcome out;
    std::stop_source stop;
    DeadlineCanceller deadline([&stop]() { stop.request_stop(); }, kAppDataPreviewTimeoutMs);
    for (const QString& path : paths) {
        if (out.scanned.size() >= kMaxAppDataScanDirs) {
            out.truncated = true;
            out.complete = false;
            break;
        }
        if (sak::isNetworkOrDevicePath(path) || sak::pathReparseUnsafe(path)) {
            // A discovered candidate we refuse to follow (reparse/UNC) is NOT sized, so its bytes
            // are absent from the total -- report the total as a lower bound (scan_complete=false),
            // not a complete figure that silently omits it. skipped_paths[] names which.
            out.skipped.append(path);
            out.complete = false;
            continue;
        }
        if (!sumAppDataDirSize(path, stop.get_token(), out.total_size, out.file_count)) {
            out.complete = false;
        }
        out.scanned.append(path);
    }
    deadline.finish();
    if (deadline.fired()) {
        out.timed_out = true;
        out.complete = false;
    }
    return out;
}

QJsonArray clampPathArray(const QStringList& paths) {
    QJsonArray arr;
    for (const QString& path : paths) {
        if (arr.size() >= kMaxAppDataReportedPaths) {
            break;
        }
        arr.append(clampLine(path));
    }
    return arr;
}

AppActionResult previewAppDataBackup(const QJsonObject& args) {
    const QString app_name = args.value(QStringLiteral("app_name")).toString().trimmed();
    if (app_name.isEmpty()) {
        return {false, QStringLiteral("preview_app_data requires an 'app_name' argument"), {}};
    }
    // app_name is a NAME, never a path. Reject path-shaped input (separators, drive/UNC colon,
    // "..") so it cannot be misused as a filesystem path -- defense in depth on top of
    // discoverAppDataDirs using only the substring-matching scanForAppData (no path join, no
    // exists()).
    if (app_name.contains(QLatin1Char('/')) || app_name.contains(QLatin1Char('\\')) ||
        app_name.contains(QLatin1Char(':')) || app_name.contains(QStringLiteral(".."))) {
        return {false, QStringLiteral("app_name must be a plain application name, not a path"), {}};
    }
    // NOTE (accepted low residual): discovery reads only the user's OWN standard data directories
    // via QDirIterator, which has no hard error channel -- an empty result is an honest "no
    // matching data" (those dirs are always present + readable for the owning user), not a masked
    // failure.
    const QStringList paths = discoverAppDataDirs(app_name);
    const AppDataPreviewOutcome out = runAppDataPreview(paths);

    const bool scan_complete = out.complete && !out.timed_out;
    QJsonObject data{{QStringLiteral("app_name"), app_name},
                     {QStringLiteral("found"), !paths.isEmpty()},
                     {QStringLiteral("directory_count"), static_cast<int>(paths.size())},
                     {QStringLiteral("sized_count"), static_cast<int>(out.scanned.size())},
                     {QStringLiteral("skipped_count"), static_cast<int>(out.skipped.size())},
                     {QStringLiteral("total_size_bytes"), static_cast<double>(out.total_size)},
                     {QStringLiteral("file_count"), static_cast<double>(out.file_count)},
                     {QStringLiteral("paths"), clampPathArray(out.scanned)},
                     {QStringLiteral("skipped_paths"), clampPathArray(out.skipped)},
                     {QStringLiteral("truncated"), out.truncated},
                     {QStringLiteral("timed_out"), out.timed_out},
                     {QStringLiteral("scan_complete"), scan_complete}};
    QString message = paths.isEmpty()
                          ? QStringLiteral("No app-data directories found for '%1'").arg(app_name)
                          : QStringLiteral("'%1': %2 director(ies), %3 bytes across %4 file(s)")
                                .arg(app_name)
                                .arg(paths.size())
                                .arg(out.total_size)
                                .arg(out.file_count);
    if (out.timed_out) {
        message += QStringLiteral(" (sizing hit the time limit; total is partial)");
    } else if (!out.complete) {
        message +=
            QStringLiteral(" (some directories were unreadable or capped; total is partial)");
    }
    return {true, message, data};
}

QJsonObject previewAppDataParamsSchema() {
    QJsonObject app_prop{
        {QStringLiteral("type"), QStringLiteral("string")},
        {QStringLiteral("description"),
         QStringLiteral(
             "Name of the application whose user-data locations to preview (e.g. "
             "\"Google Chrome\"); matched against standard AppData/ProgramData/Documents "
             "folders")}};
    QJsonObject properties{{QStringLiteral("app_name"), app_prop}};
    return QJsonObject{{QStringLiteral("type"), QStringLiteral("object")},
                       {QStringLiteral("properties"), properties},
                       {QStringLiteral("required"), QJsonArray{QStringLiteral("app_name")}},
                       {QStringLiteral("additionalProperties"), false}};
}

QJsonObject dupParamsSchema() {
    QJsonObject dirs_prop{
        {QStringLiteral("type"), QStringLiteral("array")},
        {QStringLiteral("items"), QJsonObject{{QStringLiteral("type"), QStringLiteral("string")}}},
        {QStringLiteral("description"),
         QStringLiteral("Absolute directory paths to scan for duplicate files (matched by content "
                        "hash, not name)")}};
    QJsonObject recursive_prop{{QStringLiteral("type"), QStringLiteral("boolean")},
                               {QStringLiteral("description"),
                                QStringLiteral("Scan subdirectories too (default true)")}};
    QJsonObject minsize_prop{
        {QStringLiteral("type"), QStringLiteral("integer")},
        {QStringLiteral("description"),
         QStringLiteral("Ignore files smaller than this many bytes (default 1, i.e. skip empty "
                        "files)")}};
    QJsonObject properties{{QStringLiteral("directories"), dirs_prop},
                           {QStringLiteral("recursive"), recursive_prop},
                           {QStringLiteral("minimum_file_size"), minsize_prop}};
    return QJsonObject{{QStringLiteral("type"), QStringLiteral("object")},
                       {QStringLiteral("properties"), properties},
                       {QStringLiteral("required"), QJsonArray{QStringLiteral("directories")}},
                       {QStringLiteral("additionalProperties"), false}};
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

// List existing System Restore points via RestorePointManager (Get-ComputerRestorePoint through
// runProcess = a no-shell QProcess with a fixed literal command + a hard 10s timeout; no model
// input reaches the command, so no injection surface). Read-only: it queries state, never
// creates or reverts a restore point. isSystemRestoreEnabled() reports whether protection is on;
// listRestorePoints() returns (date, description) pairs. Reading restore points typically needs
// elevation, so the process's own elevation state is surfaced and, when NOT elevated and the
// list came back empty, the message says the list may be incomplete -- an empty result then does
// not masquerade as a definitive "no restore points".
AppActionResult listRestorePoints(const QJsonObject&) {
    RestorePointManager manager;
    const bool enabled = manager.isSystemRestoreEnabled();
    const bool elevated = RestorePointManager::isElevated();
    bool query_ok = false;
    const QVector<QPair<QDateTime, QString>> points = manager.listRestorePoints(query_ok);

    // Fail CLOSED: if the restore-point query itself failed (non-zero exit / timeout / malformed
    // output) do NOT report a definitive "0 restore points" -- that would masquerade a failed
    // read as "none exist" (the same fail-open honesty hole as the connections/firewall/wifi
    // engines). A genuine empty result has query_ok == true.
    if (!query_ok) {
        return {false,
                QStringLiteral(
                    "Could not read System Restore points (the query failed or timed out%1)")
                    .arg(elevated ? QString()
                                  : QStringLiteral("; reading them may require "
                                                   "elevation")),
                {}};
    }

    QJsonArray listed;
    for (const QPair<QDateTime, QString>& point : points) {
        if (listed.size() >= kMaxRestorePoints) {
            break;
        }
        listed.append(QJsonObject{{QStringLiteral("date"), point.first.toString(Qt::ISODate)},
                                  {QStringLiteral("description"), clampLine(point.second)}});
    }
    QJsonObject data{{QStringLiteral("system_restore_enabled"), enabled},
                     {QStringLiteral("elevated"), elevated},
                     {QStringLiteral("count"), points.size()},
                     {QStringLiteral("reported_count"), listed.size()},
                     {QStringLiteral("truncated"), points.size() > listed.size()},
                     {QStringLiteral("restore_points"), listed}};

    QString message;
    if (!enabled) {
        message = QStringLiteral("System Restore is disabled on the system drive");
    } else {
        message = QStringLiteral("System Restore enabled: %1 restore point(s)").arg(points.size());
        if (!elevated && points.isEmpty()) {
            message += QStringLiteral(" (run elevated to read the full list)");
        }
    }
    return {true, message, data};
}

QJsonObject serializeThermalReading(const ThermalReading& reading) {
    return QJsonObject{{QStringLiteral("component"), reading.component},
                       {QStringLiteral("temperature_celsius"), reading.temperature_celsius}};
}

// Read current temperatures via ThermalMonitor::pollOnce (a single bounded no-shell PowerShell
// poll of the WMI ACPI thermal zone, nvidia-smi/AMD WMI, and storage reliability counters;
// unavailable sensors are omitted, exactly like smart_scan). Read-only and SYNC (pollOnce is a
// static, thread-safe, self-contained call). Fail-CLOSED: a FAILED query (non-zero exit /
// timeout) is reported as an honest failure via the queryOk signal, not as "no thermal data"; a
// genuine empty result (the query ran but no sensor was readable -- common on desktops/VMs, or
// when not elevated: the CPU thermal zone and disk counters need admin) is a success with zero
// sensors, phrased so it never reads as "the system has no thermal concern".
AppActionResult readTemperatures(const QJsonObject&) {
    bool query_ok = false;
    const QVector<ThermalReading> readings = ThermalMonitor::pollOnce(query_ok);
    if (!query_ok) {
        return {false,
                QStringLiteral("Could not read thermal sensors (the query failed or timed out)"),
                {}};
    }

    QJsonArray listed;
    double max_temp = 0.0;
    for (const ThermalReading& reading : readings) {
        max_temp = std::max(max_temp, reading.temperature_celsius);
        if (listed.size() < kMaxThermalReadings) {
            listed.append(serializeThermalReading(reading));
        }
    }
    QJsonObject data{{QStringLiteral("sensor_count"), readings.size()},
                     {QStringLiteral("reported_count"), listed.size()},
                     {QStringLiteral("truncated"), readings.size() > listed.size()},
                     {QStringLiteral("sensors"), listed}};

    const QString message =
        readings.isEmpty()
            ? QStringLiteral(
                  "No readable thermal sensors (they may require elevation or may not "
                  "be exposed on this system)")
            : QStringLiteral("Read %1 thermal sensor(s), max %2 C")
                  .arg(readings.size())
                  .arg(max_temp, 0, 'f', 1);
    return {true, message, data};
}

QJsonObject serializeCpuBenchmark(const CpuBenchmarkResult& r) {
    return QJsonObject{{QStringLiteral("single_thread_score"), r.single_thread_score},
                       {QStringLiteral("multi_thread_score"), r.multi_thread_score},
                       {QStringLiteral("thread_scaling_efficiency"), r.thread_scaling_efficiency},
                       {QStringLiteral("thread_count"), static_cast<int>(r.thread_count)},
                       {QStringLiteral("prime_sieve_time_ms"), r.prime_sieve_time_ms},
                       {QStringLiteral("matrix_multiply_time_ms"), r.matrix_multiply_time_ms},
                       {QStringLiteral("zlib_compression_time_ms"), r.zlib_compression_time_ms},
                       {QStringLiteral("aes_encryption_time_ms"), r.aes_encryption_time_ms},
                       {QStringLiteral("zlib_throughput_mbps"), r.zlib_throughput_mbps},
                       {QStringLiteral("aes_throughput_mbps"), r.aes_throughput_mbps},
                       {QStringLiteral("matrix_gflops"), r.matrix_gflops}};
}

QJsonObject serializeMemoryBenchmark(const MemoryBenchmarkResult& r) {
    return QJsonObject{{QStringLiteral("overall_score"), r.overall_score},
                       {QStringLiteral("read_bandwidth_gbps"), r.read_bandwidth_gbps},
                       {QStringLiteral("write_bandwidth_gbps"), r.write_bandwidth_gbps},
                       {QStringLiteral("copy_bandwidth_gbps"), r.copy_bandwidth_gbps},
                       {QStringLiteral("random_latency_ns"), r.random_latency_ns},
                       {QStringLiteral("max_contiguous_alloc_mb"),
                        static_cast<double>(r.max_contiguous_alloc_mb)},
                       {QStringLiteral("alloc_dealloc_ops_per_sec"), r.alloc_dealloc_ops_per_sec}};
}

// The benchmark workers have NO measurement-failure error channel: execute() returns success even
// when a sub-test could not run (a compression call that did not return Z_OK, or -- for memory -- a
// large buffer allocation that failed), leaving a zero-valued metric. A genuine completed run
// always produces strictly-positive throughput/bandwidth/latency, so a zero in any of those is a
// reliable failed-measurement sentinel. The bridge maps it to an honest op failure rather than
// reporting {success:true, ...0.0...} as a real benchmark (the fail-open pattern this codebase
// treats as must-fix). Fixed at the bridge because the workers are shared with the GUI.
bool cpuBenchmarkMeasurementOk(const CpuBenchmarkResult& r) {
    return r.zlib_throughput_mbps > 0.0 && r.aes_throughput_mbps > 0.0 && r.matrix_gflops > 0.0;
}

bool memoryBenchmarkMeasurementOk(const MemoryBenchmarkResult& r) {
    return r.read_bandwidth_gbps > 0.0 && r.write_bandwidth_gbps > 0.0 &&
           r.copy_bandwidth_gbps > 0.0 && r.random_latency_ns > 0.0;
}

// Drive the CPU benchmark ENGINE (WorkerBase) directly on the AI worker thread via the
// AsyncActionInvocation bridge: it runs on its own thread and posts finished/failed back to the
// caller-thread context, drained by the local event loop -- no GUI thread, no persisted prefs. The
// suite is pure compute (no filesystem or system change), so it stays read-only; it consumes CPU
// transiently. worker.result() is valid once finished has fired (written on the worker thread
// before the signal, so happens-before holds). requestStop() after run() reaps a timed-out run.
AppActionResult runCpuBenchmark() {
    CpuBenchmarkWorker worker;
    AsyncActionInvocation inv(kBenchmarkTimeoutMs);
    QObject::connect(&worker, &WorkerBase::finished, inv.context(), [&inv, &worker]() {
        const CpuBenchmarkResult& r = worker.result();
        if (!cpuBenchmarkMeasurementOk(r)) {
            inv.finish({false,
                        QStringLiteral("CPU benchmark measurement failed (a compute/compression "
                                       "sub-test did not complete)"),
                        {}});
            return;
        }
        const QString message = QStringLiteral(
                                    "CPU benchmark complete -- single-thread %1, multi-thread %2 "
                                    "(normalized: i5-12400 = 1000)")
                                    .arg(r.single_thread_score)
                                    .arg(r.multi_thread_score);
        inv.finish({true, message, serializeCpuBenchmark(r)});
    });
    QObject::connect(&worker,
                     &WorkerBase::failed,
                     inv.context(),
                     [&inv](int, const QString& error) { inv.finish({false, error, {}}); });
    const AppActionResult result = inv.run([&worker]() { worker.start(); });
    worker.requestStop();  // cooperative stop before teardown (no-op if already finished)
    return result;
}

// Same bridge for the memory benchmark engine. Peak transient footprint is bounded (256 MB
// bandwidth buffers + a 64 MB latency array + a single <=1 GB alloc probe, all freed immediately);
// no persistent change, so read-only.
AppActionResult runMemoryBenchmark() {
    MemoryBenchmarkWorker worker;
    AsyncActionInvocation inv(kBenchmarkTimeoutMs);
    QObject::connect(&worker, &WorkerBase::finished, inv.context(), [&inv, &worker]() {
        const MemoryBenchmarkResult& r = worker.result();
        if (!memoryBenchmarkMeasurementOk(r)) {
            inv.finish({false,
                        QStringLiteral("Memory benchmark measurement failed (a bandwidth/latency "
                                       "buffer allocation did not succeed -- the host may be low "
                                       "on memory)"),
                        {}});
            return;
        }
        const QString message = QStringLiteral(
                                    "Memory benchmark complete -- read %1 GB/s, write %2 GB/s, "
                                    "latency %3 ns (score %4)")
                                    .arg(r.read_bandwidth_gbps, 0, 'f', 1)
                                    .arg(r.write_bandwidth_gbps, 0, 'f', 1)
                                    .arg(r.random_latency_ns, 0, 'f', 1)
                                    .arg(r.overall_score);
        inv.finish({true, message, serializeMemoryBenchmark(r)});
    });
    QObject::connect(&worker,
                     &WorkerBase::failed,
                     inv.context(),
                     [&inv](int, const QString& error) { inv.finish({false, error, {}}); });
    const AppActionResult result = inv.run([&worker]() { worker.start(); });
    worker.requestStop();  // cooperative stop before teardown (no-op if already finished)
    return result;
}

// diagnostics.run_benchmark: run one performance benchmark suite. Only "cpu" and "memory" are
// exposed -- both are pure in-memory workloads with no persistent change, so the op stays
// read-only. The DISK benchmark is deliberately NOT exposed here: it writes a multi-GB test file
// to a drive (a mutating side effect + wear), so it belongs on the mutating path if ever added.
AppActionResult benchmarkSystem(const QJsonObject& args) {
    const QString target = args.value(QStringLiteral("target")).toString().trimmed().toLower();
    if (target.isEmpty() || target == QStringLiteral("cpu")) {
        return runCpuBenchmark();
    }
    if (target == QStringLiteral("memory")) {
        return runMemoryBenchmark();
    }
    return {false,
            QStringLiteral("benchmark 'target' must be \"cpu\" or \"memory\" (the disk benchmark "
                           "writes a test file and is not exposed as a read-only op)"),
            {}};
}

QJsonObject serializeUserProfile(const UserProfile& profile) {
    return QJsonObject{{QStringLiteral("username"), clampLine(profile.username)},
                       {QStringLiteral("sid"), clampLine(profile.sid)},
                       {QStringLiteral("profile_path"), clampLine(profile.profile_path)},
                       {QStringLiteral("is_current_user"), profile.is_current_user},
                       {QStringLiteral("estimated_size_bytes"),
                        static_cast<double>(profile.total_size_estimated)}};
}

// List local Windows user accounts via WindowsUserScanner (NetUserEnum for normal accounts, then
// per-user profile-path / SID / current-user / size resolution -- the same headless engine the
// user-profile backup wizard drives). Read-only: it enumerates accounts and reads profile
// metadata; it never creates, modifies, or deletes a user. The scanner only reports accounts that
// have an existing profile directory (service / never-logged-in accounts are filtered out), so the
// message says "with a profile". Detailed enumeration (USER_INFO_3) typically needs elevation, so
// a FAILED enumeration is reported as an honest failure via the queryOk signal rather than a
// definitive "0 users" (the same fail-open honesty hole closed in the restore-point / thermal
// engines); a genuine empty result has queryOk == true.
AppActionResult listUsers(const QJsonObject&) {
    WindowsUserScanner scanner;
    bool query_ok = false;
    const QVector<UserProfile> users = scanner.scanUsers(query_ok);
    if (!query_ok) {
        return {false,
                QStringLiteral("Could not enumerate user accounts (the query failed; reading "
                               "detailed account info may require elevation)"),
                {}};
    }

    QJsonArray listed;
    for (const UserProfile& user : users) {
        if (listed.size() >= kMaxUsers) {
            break;
        }
        listed.append(serializeUserProfile(user));
    }
    QJsonObject data{{QStringLiteral("count"), users.size()},
                     {QStringLiteral("reported_count"), listed.size()},
                     {QStringLiteral("truncated"), users.size() > listed.size()},
                     {QStringLiteral("users"), listed}};
    return {true,
            QStringLiteral("Found %1 local user account(s) with a profile").arg(users.size()),
            data};
}

QString clampHeader(const QString& value) {
    if (value.size() <= kMaxHeaderChars) {
        return value;
    }
    return value.left(kMaxHeaderChars) + QStringLiteral("...");
}

QString emailClientTypeToString(EmailClientType type) {
    switch (type) {
    case EmailClientType::Outlook:
        return QStringLiteral("outlook");
    case EmailClientType::Thunderbird:
        return QStringLiteral("thunderbird");
    case EmailClientType::WindowsMail:
        return QStringLiteral("windows_mail");
    case EmailClientType::Other:
        break;
    }
    return QStringLiteral("other");
}

QJsonObject serializeEmailDataFile(const EmailDataFile& file) {
    return QJsonObject{{QStringLiteral("path"), clampLine(file.path)},
                       {QStringLiteral("type"), file.type},
                       {QStringLiteral("size_bytes"), static_cast<double>(file.size_bytes)},
                       {QStringLiteral("linked"), file.is_linked}};
}

QJsonObject serializeEmailProfile(const EmailClientProfile& profile) {
    QJsonArray files;
    for (const EmailDataFile& file : profile.data_files) {
        if (files.size() >= kMaxEmailDataFiles) {
            break;
        }
        files.append(serializeEmailDataFile(file));
    }
    return QJsonObject{
        {QStringLiteral("client_type"), emailClientTypeToString(profile.client_type)},
        {QStringLiteral("client_name"), clampLine(profile.client_name)},
        {QStringLiteral("client_version"), clampLine(profile.client_version)},
        {QStringLiteral("profile_name"), clampLine(profile.profile_name)},
        {QStringLiteral("profile_path"), clampLine(profile.profile_path)},
        {QStringLiteral("total_size_bytes"), static_cast<double>(profile.total_size_bytes)},
        {QStringLiteral("data_file_count"), static_cast<int>(profile.data_files.size())},
        {QStringLiteral("data_files"), files},
        {QStringLiteral("data_files_truncated"), profile.data_files.size() > files.size()}};
}

// email.list_profiles: enumerate installed email clients (Outlook / Thunderbird / Windows Mail)
// and their profiles + linked data-file paths, driving the app's OWN EmailProfileManager
// discovery (registry + profile-dir scan, no network). discoverProfiles() is fully synchronous
// and emits profilesDiscovered inline, captured by a direct connection with no event loop -- the
// hardware_scan / list_adapters pattern. Read-only: it reads metadata only (client/profile names,
// data-file PATHS and sizes) and never opens a mailbox or exports any content -- the secret-bearing
// copy lives on EmailProfileManager::backupProfiles, which is deliberately NOT exposed here.
// Honesty (fail-open audit): a genuinely mail-clientless machine returns empty, which IS a valid
// normal state -- but a FAILED source read (a corrupt profiles.ini, an unreadable registry key)
// would ALSO return empty. discoveryReliable() distinguishes them (QSettings::status()); an empty
// result from a failed read is reported as an honest failure, not a masked "0 profiles" success.
AppActionResult listEmailProfiles(const QJsonObject&) {
    EmailProfileManager manager;
    QVector<EmailClientProfile> profiles;
    bool captured = false;
    QObject::connect(&manager,
                     &EmailProfileManager::profilesDiscovered,
                     &manager,
                     [&profiles, &captured](QVector<EmailClientProfile> result) {
                         profiles = std::move(result);
                         captured = true;
                     });
    manager.discoverProfiles();
    if (!captured) {
        return {false, QStringLiteral("Email profile discovery did not complete"), {}};
    }
    const bool reliable = manager.discoveryReliable();
    if (!reliable && profiles.isEmpty()) {
        return {false,
                QStringLiteral("Email profile discovery hit a read error (a client's registry key "
                               "or profiles.ini could not be read) and found nothing -- the result "
                               "is unreliable, not a confirmed absence of mail clients"),
                {}};
    }

    QJsonArray listed;
    for (const EmailClientProfile& profile : profiles) {
        if (listed.size() >= kMaxEmailProfiles) {
            break;
        }
        listed.append(serializeEmailProfile(profile));
    }
    QJsonObject data{{QStringLiteral("profile_count"), profiles.size()},
                     {QStringLiteral("reported_count"), listed.size()},
                     {QStringLiteral("truncated"), profiles.size() > listed.size()},
                     {QStringLiteral("discovery_complete"), reliable},
                     {QStringLiteral("profiles"), listed}};
    QString message =
        profiles.isEmpty()
            ? QStringLiteral(
                  "No email client profiles found (no Outlook/Thunderbird/Windows Mail "
                  "configuration detected)")
            : QStringLiteral("Found %1 email client profile(s)").arg(profiles.size());
    if (!reliable) {
        message += QStringLiteral(" (partial: at least one source could not be read)");
    }
    return {true, message, data};
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
    if (const std::optional<AppActionResult> unsafe =
            refuseUnsafePath(path, QStringLiteral("read_mbox"), QStringLiteral("path"))) {
        return *unsafe;
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

QJsonObject serializeSearchHit(const EmailSearchHit& hit) {
    return QJsonObject{{QStringLiteral("subject"), clampHeader(hit.subject)},
                       {QStringLiteral("sender"), clampHeader(hit.sender)},
                       {QStringLiteral("date"),
                        hit.date.isValid() ? hit.date.toString(Qt::ISODate) : QString()},
                       {QStringLiteral("match_field"), hit.match_field},
                       // A small window around the match -- NOT the full message body.
                       {QStringLiteral("context"), clampLine(hit.context_snippet)},
                       {QStringLiteral("folder"), hit.folder_path}};
}

// Validate email.search_mbox inputs (path + query) before opening anything. Returns an error to
// short-circuit on, or nullopt when they are acceptable.
std::optional<AppActionResult> validateMboxSearchInputs(const QString& path, const QString& query) {
    if (path.isEmpty()) {
        return AppActionResult{false, QStringLiteral("search_mbox requires a 'path' argument"), {}};
    }
    if (const std::optional<AppActionResult> unsafe =
            refuseUnsafePath(path, QStringLiteral("search_mbox"), QStringLiteral("path"))) {
        return *unsafe;
    }
    if (query.trimmed().isEmpty()) {
        return AppActionResult{false,
                               QStringLiteral("search_mbox requires a non-empty 'query' argument"),
                               {}};
    }
    const QFileInfo info(path);
    if (!info.isFile()) {
        return AppActionResult{false, QStringLiteral("No such MBOX file: %1").arg(path), {}};
    }
    if (info.size() > kMaxMboxBytes) {
        return AppActionResult{
            false,
            QStringLiteral("MBOX file is too large for a headless search (%1 bytes > %2 limit)")
                .arg(info.size())
                .arg(kMaxMboxBytes),
            {}};
    }
    return std::nullopt;
}

EmailSearchCriteria emailSearchCriteriaFromArgs(const QJsonObject& args, const QString& query) {
    EmailSearchCriteria criteria;
    criteria.query_text = query;
    criteria.case_sensitive = args.value(QStringLiteral("case_sensitive")).toBool(false);
    criteria.search_subject = args.value(QStringLiteral("search_subject")).toBool(true);
    criteria.search_body = args.value(QStringLiteral("search_body")).toBool(true);
    criteria.search_sender = args.value(QStringLiteral("search_sender")).toBool(true);
    return criteria;
}

struct EmailSearchOutcome {
    QVector<EmailSearchHit> hits;
    bool completed = false;
    QString error;
};

// Wire an EmailSearchWorker's inline signals into @p outcome. The connections are DIRECT (worker
// and receiver live on this thread), so they fire synchronously during the worker's Q_EMIT and the
// outcome is fully populated by the time the (synchronous) search call returns.
void connectSearchOutcome(EmailSearchWorker& worker, EmailSearchOutcome& outcome) {
    QObject::connect(&worker,
                     &EmailSearchWorker::searchHit,
                     &worker,
                     [&outcome](const EmailSearchHit& hit) { outcome.hits.append(hit); });
    QObject::connect(&worker, &EmailSearchWorker::searchComplete, &worker, [&outcome](int, double) {
        outcome.completed = true;
    });
    QObject::connect(&worker,
                     &EmailSearchWorker::errorOccurred,
                     &worker,
                     [&outcome](const QString& error) { outcome.error = error; });
}

// Drive the app's OWN EmailSearchWorker synchronously over an already-open MboxParser. A read
// failure emits errorOccurred and NO searchComplete.
EmailSearchOutcome runMboxSearch(MboxParser& parser, const EmailSearchCriteria& criteria) {
    EmailSearchOutcome outcome;
    EmailSearchWorker worker;
    connectSearchOutcome(worker, outcome);
    worker.searchMbox(&parser, criteria);
    return outcome;
}

// email.search_mbox
//
// Full-text search an MBOX file for messages matching a query, via the app's OWN EmailSearchWorker
// (see runMboxSearch). Distinct from email.read_mbox, which lists messages. Read-only. Returns
// per-hit metadata + a CONTEXT SNIPPET around the match (never the full body). A failed message
// read is reported honestly, never a misleading "0 hits" success.
AppActionResult searchMbox(const QJsonObject& args) {
    const QString path = args.value(QStringLiteral("path")).toString().trimmed();
    const QString query = args.value(QStringLiteral("query")).toString();
    if (const std::optional<AppActionResult> invalid = validateMboxSearchInputs(path, query)) {
        return *invalid;
    }
    const QFileInfo info(path);
    MboxParser parser;
    parser.open(path);
    if (!parser.isOpen()) {
        return {false, QStringLiteral("Not a valid MBOX file: %1").arg(path), {}};
    }
    const EmailSearchOutcome outcome = runMboxSearch(parser,
                                                     emailSearchCriteriaFromArgs(args, query));
    if (!outcome.error.isEmpty() || !outcome.completed) {
        return {false,
                outcome.error.isEmpty() ? QStringLiteral("MBOX search did not complete")
                                        : outcome.error,
                {}};
    }

    QJsonArray listed;
    for (const EmailSearchHit& hit : outcome.hits) {
        if (listed.size() >= kMaxMboxSearchHits) {
            break;
        }
        listed.append(serializeSearchHit(hit));
    }
    QJsonObject data{
        {QStringLiteral("query"), query},
        {QStringLiteral("total_hits"), outcome.hits.size()},
        {QStringLiteral("reported_count"), listed.size()},
        {QStringLiteral("truncated"), outcome.hits.size() > listed.size()},
        // True if the engine stopped at its internal result ceiling, so more matches may exist.
        {QStringLiteral("result_cap_reached"),
         outcome.hits.size() >= sak::email::kMaxSearchResults},
        {QStringLiteral("hits"), listed}};
    return {true,
            QStringLiteral("%1 hit(s) for \"%2\" in %3")
                .arg(outcome.hits.size())
                .arg(query, info.fileName()),
            data};
}

// Defined below (near readPst); declared here because searchPst reuses the same PST path validator.
std::optional<AppActionResult> validatePstReadPath(const QString& path,
                                                   const QString& op_name,
                                                   QFileInfo& info);

// email.search_pst
//
// Full-text search a PST/OST file for messages matching a query, via the app's OWN
// EmailSearchWorker driving an already-open PstParser (the search_mbox pattern extended to PST).
// Distinct from email.read_pst, which lists the folder tree. Read-only. Returns per-hit metadata +
// a CONTEXT SNIPPET around the match (never the full body). Bodies are read across every folder, so
// the search runs under a wall-time deadline (worker.cancel()) and reports search_complete
// honestly. A folder read failure emits errorOccurred -> reported as failure, never a misleading
// empty/partial success.
AppActionResult searchPst(const QJsonObject& args) {
    const QString path = args.value(QStringLiteral("path")).toString().trimmed();
    QFileInfo info;
    if (const std::optional<AppActionResult> fail =
            validatePstReadPath(path, QStringLiteral("search_pst"), info)) {
        return *fail;
    }
    const QString query = args.value(QStringLiteral("query")).toString();
    if (query.trimmed().isEmpty()) {
        return {false, QStringLiteral("search_pst requires a non-empty 'query' argument"), {}};
    }

    PstParser parser;
    parser.open(path);
    if (!parser.isOpen()) {
        return {false,
                QStringLiteral("Could not open or parse the PST/OST (not a valid PST/OST, "
                               "unsupported, or corrupt): %1")
                    .arg(info.fileName()),
                {}};
    }

    EmailSearchWorker worker;
    EmailSearchOutcome outcome;
    connectSearchOutcome(worker, outcome);
    // The deadline calls worker.cancel() (the engine polls m_cancelled). worker outlives `deadline`
    // (declared first -> destroyed last), so the monitor never touches a dead worker.
    DeadlineCanceller deadline([&worker]() { worker.cancel(); }, kPstSearchTimeoutMs);
    worker.search(&parser, emailSearchCriteriaFromArgs(args, query));
    deadline.finish();

    // A folder-read failure emits errorOccurred (the outer search still emits searchComplete), so
    // an error is authoritative -- report it rather than a partial-but-"complete" result.
    if (!outcome.error.isEmpty()) {
        return {false, outcome.error, {}};
    }
    if (!outcome.completed) {
        return {false, QStringLiteral("PST/OST search did not complete"), {}};
    }
    // Honest coverage: a deadline cancel or hitting the engine's result ceiling means the search
    // did NOT see every message, so search_complete is false (the hits are a partial set).
    const bool capped = outcome.hits.size() >= sak::email::kMaxSearchResults;
    const bool search_complete = !deadline.fired() && !capped;

    QJsonArray listed;
    for (const EmailSearchHit& hit : outcome.hits) {
        if (listed.size() >= kMaxPstSearchHits) {
            break;
        }
        listed.append(serializeSearchHit(hit));
    }
    QJsonObject data{{QStringLiteral("query"), query},
                     {QStringLiteral("total_hits"), outcome.hits.size()},
                     {QStringLiteral("reported_count"), listed.size()},
                     {QStringLiteral("truncated"), outcome.hits.size() > listed.size()},
                     {QStringLiteral("result_cap_reached"), capped},
                     {QStringLiteral("search_complete"), search_complete},
                     {QStringLiteral("hits"), listed}};
    const QString suffix =
        search_complete ? QString() : QStringLiteral(" (search incomplete -- partial coverage)");
    return {true,
            QStringLiteral("%1 hit(s) for \"%2\" in %3%4")
                .arg(outcome.hits.size())
                .arg(query, info.fileName(), suffix),
            data};
}

QString emailItemTypeToString(EmailItemType type) {
    switch (type) {
    case EmailItemType::Email:
        return QStringLiteral("email");
    case EmailItemType::Contact:
        return QStringLiteral("contact");
    case EmailItemType::Calendar:
        return QStringLiteral("calendar");
    case EmailItemType::Task:
        return QStringLiteral("task");
    case EmailItemType::StickyNote:
        return QStringLiteral("note");
    case EmailItemType::JournalEntry:
        return QStringLiteral("journal");
    case EmailItemType::DistList:
        return QStringLiteral("distlist");
    case EmailItemType::MeetingRequest:
        return QStringLiteral("meeting_request");
    case EmailItemType::Unknown:
        break;
    }
    return QStringLiteral("unknown");
}

QJsonObject serializePstFolder(const PstFolder& folder) {
    return QJsonObject{{QStringLiteral("node_id"), static_cast<double>(folder.node_id)},
                       {QStringLiteral("parent_node_id"),
                        static_cast<double>(folder.parent_node_id)},
                       {QStringLiteral("display_name"), clampHeader(folder.display_name)},
                       {QStringLiteral("content_count"), folder.content_count},
                       {QStringLiteral("unread_count"), folder.unread_count},
                       {QStringLiteral("subfolder_count"), folder.subfolder_count},
                       {QStringLiteral("container_class"), clampHeader(folder.container_class)}};
}

// Flatten the folder hierarchy into a capped flat array (parent_node_id preserves structure). A
// flat list avoids deep JSON nesting and unbounded recursion width while keeping the tree
// reconstructable by the model.
void flattenPstFolders(const PstFolder& folder, QJsonArray& out) {
    if (out.size() >= kMaxPstFolders) {
        return;
    }
    out.append(serializePstFolder(folder));
    for (const PstFolder& child : folder.children) {
        flattenPstFolders(child, out);
    }
}

int countPstFolders(const PstFolder& folder) {
    int total = 1;
    for (const PstFolder& child : folder.children) {
        total += countPstFolders(child);
    }
    return total;
}

QJsonObject serializePstItem(const PstItemSummary& item) {
    return QJsonObject{{QStringLiteral("node_id"), static_cast<double>(item.node_id)},
                       {QStringLiteral("type"), emailItemTypeToString(item.item_type)},
                       {QStringLiteral("subject"), clampHeader(item.subject)},
                       {QStringLiteral("sender_name"), clampHeader(item.sender_name)},
                       {QStringLiteral("sender_email"), clampHeader(item.sender_email)},
                       {QStringLiteral("date"), item.date.toString(Qt::ISODate)},
                       {QStringLiteral("size_bytes"), static_cast<double>(item.size_bytes)},
                       {QStringLiteral("has_attachments"), item.has_attachments},
                       {QStringLiteral("is_read"), item.is_read},
                       {QStringLiteral("importance"), item.importance}};
}

// When a folder_node_id is supplied, read that ONE folder's item headers (synchronous, bounded by
// offset/limit) and fold them into the result. Split from readPst so the op stays under the
// complexity budget. A read failure is reported as a non-fatal items_error, not a whole-op
// failure -- the folder tree is still useful on its own.
void appendPstFolderItems(PstParser& parser, const QJsonObject& args, QJsonObject& data) {
    if (!args.contains(QStringLiteral("folder_node_id"))) {
        return;
    }
    // PST node IDs are 32-bit. Validate before the double->uint64 cast: an out-of-range or
    // non-finite double would be undefined behavior to cast (a bad id otherwise just maps to a
    // non-existent node and reads empty, but the cast itself must be well-defined).
    const double raw_id = args.value(QStringLiteral("folder_node_id")).toDouble(-1);
    if (!(raw_id >= 0.0) || raw_id > static_cast<double>(0xFF'FF'FF'FFu)) {
        data[QStringLiteral("items_error")] =
            QStringLiteral("Invalid folder_node_id (expected a node_id from the folder tree)");
        return;
    }
    const auto folder_id = static_cast<uint64_t>(raw_id);
    const int offset = std::max(0, args.value(QStringLiteral("offset")).toInt(0));
    const int requested = args.value(QStringLiteral("limit")).toInt(kDefaultPstItemLimit);
    const int limit = std::clamp(requested > 0 ? requested : kDefaultPstItemLimit, 1, kMaxPstItems);

    data[QStringLiteral("folder_node_id")] = static_cast<double>(folder_id);
    const auto items = parser.readFolderItems(folder_id, offset, limit);
    if (!items.has_value()) {
        data[QStringLiteral("items_error")] =
            QStringLiteral("Could not read items for the requested folder");
        return;
    }
    QJsonArray listed;
    for (const PstItemSummary& item : *items) {
        listed.append(serializePstItem(item));
    }
    data[QStringLiteral("offset")] = offset;
    data[QStringLiteral("item_count")] = listed.size();
    // A full page means more items may exist beyond this offset+limit window (the module reports
    // truncation, never silent). The folder's content_count in the tree gives the true total.
    data[QStringLiteral("items_truncated")] = listed.size() >= limit;
    data[QStringLiteral("items")] = listed;
}

// Validate a user-supplied PST/OST path for a headless read: reject empty, network/UNC/device (so a
// crafted path can't pull the read over SMB and leak an NTLM handshake), non-file, and over the
// size cap (open() parses all node metadata synchronously). Returns a ready-to-return failure
// result, or nullopt + fills @p info when usable. Shared by read_pst and recover_deleted so the
// guards -- a security boundary -- cannot drift apart (the export_mbox/export_pst shared-validator
// lesson).
std::optional<AppActionResult> validatePstReadPath(const QString& path,
                                                   const QString& op_name,
                                                   QFileInfo& info) {
    if (path.isEmpty()) {
        return AppActionResult{false,
                               QStringLiteral("%1 requires a 'path' argument").arg(op_name),
                               {}};
    }
    if (const std::optional<AppActionResult> unsafe =
            refuseUnsafePath(path, op_name, QStringLiteral("path"))) {
        return *unsafe;
    }
    info = QFileInfo(path);
    if (!info.isFile()) {
        return AppActionResult{false, QStringLiteral("No such PST/OST file: %1").arg(path), {}};
    }
    if (info.size() > kMaxPstBytes) {
        return AppActionResult{false,
                               QStringLiteral("PST/OST file is too large for a headless read (%1 "
                                              "bytes > %2 limit); use the GUI")
                                   .arg(info.size())
                                   .arg(kMaxPstBytes),
                               {}};
    }
    return std::nullopt;
}

// Read a PST/OST mailbox: file metadata + the folder tree, plus (when folder_node_id is given) that
// folder's message headers. Drives PstParser DIRECTLY -- open() is fully synchronous (blocks,
// parses header + NBT/BBT + folder tree, sets isOpen()), and folderTree()/readFolderItems() are
// synchronous worker-thread APIs -- so this runs inline with no event loop or controller, the
// read_mbox pattern extended to PST. isOpen() is the value channel: any open/parse failure leaves
// it false, so a failed read is an honest failure, never an empty-success. Path validated up front
// (UNC/device refused so a crafted path can't pull the read over SMB; isFile; size-capped because
// open() parses all node metadata synchronously). Read-only: never writes or recovers anything.
AppActionResult readPst(const QJsonObject& args) {
    const QString path = args.value(QStringLiteral("path")).toString().trimmed();
    QFileInfo info;
    if (auto fail = validatePstReadPath(path, QStringLiteral("read_pst"), info)) {
        return *fail;
    }

    PstParser parser;
    parser.open(path);
    if (!parser.isOpen()) {
        return {false,
                QStringLiteral("Could not open or parse the PST/OST (not a valid PST/OST, "
                               "unsupported, or corrupt): %1")
                    .arg(info.fileName()),
                {}};
    }

    const PstFileInfo file_info = parser.fileInfo();
    QJsonArray folders;
    int total_folders = 0;
    for (const PstFolder& root : parser.folderTree()) {
        flattenPstFolders(root, folders);
        total_folders += countPstFolders(root);
    }

    QJsonObject data{{QStringLiteral("path"), info.absoluteFilePath()},
                     {QStringLiteral("name"), info.fileName()},
                     {QStringLiteral("display_name"), clampHeader(file_info.display_name)},
                     {QStringLiteral("is_ost"), file_info.is_ost},
                     {QStringLiteral("is_unicode"), file_info.is_unicode},
                     {QStringLiteral("encryption_type"), file_info.encryption_type},
                     {QStringLiteral("total_items"), file_info.total_items},
                     {QStringLiteral("file_size_bytes"),
                      static_cast<double>(file_info.file_size_bytes)},
                     {QStringLiteral("folder_count"), total_folders},
                     {QStringLiteral("reported_folder_count"), folders.size()},
                     {QStringLiteral("folders_truncated"), total_folders > folders.size()},
                     {QStringLiteral("folders"), folders}};
    appendPstFolderItems(parser, args, data);

    return {true,
            QStringLiteral("PST/OST '%1': %2 folder(s)").arg(info.fileName()).arg(total_folders),
            data};
}

// One recovered item as METADATA ONLY -- a recovery INVENTORY, never message bodies/headers/
// attachments. Bounds the payload and discloses only what is needed to decide what to recover.
QJsonObject serializeRecoveredItem(const PstItemDetail& item, const QString& source) {
    return QJsonObject{{QStringLiteral("node_id"), static_cast<double>(item.node_id)},
                       {QStringLiteral("type"), emailItemTypeToString(item.item_type)},
                       {QStringLiteral("subject"), clampHeader(item.subject)},
                       {QStringLiteral("sender_name"), clampHeader(item.sender_name)},
                       {QStringLiteral("sender_email"), clampHeader(item.sender_email)},
                       {QStringLiteral("date"), item.date.toString(Qt::ISODate)},
                       {QStringLiteral("size_bytes"), static_cast<double>(item.size_bytes)},
                       {QStringLiteral("source"), source}};
}

// Serialize a recovered-item vector into the capped output array (kMaxRecoverItems total across all
// sources; truncation is reported by the caller, never silent).
void appendRecoveredItems(const QVector<PstItemDetail>& items,
                          const QString& source,
                          QJsonArray& out) {
    for (const PstItemDetail& item : items) {
        if (out.size() >= kMaxRecoverItems) {
            return;
        }
        out.append(serializeRecoveredItem(item, source));
    }
}

// Outcome of a bounded deleted-item scan (see runDeletedItemScan).
struct DeletedScanOutcome {
    QVector<PstItemDetail> recoverable;
    QVector<PstItemDetail> orphaned;
    bool recoverable_reliable{true};  ///< false when a non-cancel read error truncated recoverable
    bool orphan_reliable{true};       ///< false when the orphan scan was skipped (incomplete set)
    bool timed_out{false};            ///< the wall-time ceiling fired; results are partial
};

// Run the DeletedItemScanner over an open PstParser under a hard wall-time ceiling. A deadline
// monitor jthread cancels the scan (scanner polls m_cancelled in its item/node loops); `done` stops
// the monitor the instant the scan finishes, so it dies long before the scanner is destroyed
// (declared after it -> joined first). The scanner's recursions ride on the parser's depth-capped +
// visited-set-guarded folder tree, so there is no new fan-out surface.
DeletedScanOutcome runDeletedItemScan(PstParser& parser, bool include_orphans) {
    DeletedItemScanner scanner(&parser);
    // scanner.cancel() (which also cancels the parser) is the cancel action; the monitor calls it
    // at the ceiling. scanner is declared BEFORE the canceller, so the monitor is joined before
    // scanner is destroyed (the callback captures &scanner).
    DeadlineCanceller deadline([&scanner]() { scanner.cancel(); }, kRecoverScanTimeoutMs);

    DeletedScanOutcome outcome;
    outcome.recoverable = scanner.scanRecoverableItems();
    outcome.recoverable_reliable = scanner.recoverableReliable();
    outcome.orphaned = include_orphans ? scanner.scanOrphanedNodes() : QVector<PstItemDetail>{};
    outcome.orphan_reliable = !include_orphans || scanner.reachableReliable();
    deadline.finish();
    outcome.timed_out = deadline.fired();
    return outcome;
}

// Assemble the model-facing result from a scan outcome (capped item array + honest counts/flags).
AppActionResult buildRecoverResult(const QFileInfo& info,
                                   const DeletedScanOutcome& outcome,
                                   bool include_orphans) {
    QJsonArray items;
    appendRecoveredItems(outcome.recoverable, QStringLiteral("recoverable"), items);
    appendRecoveredItems(outcome.orphaned, QStringLiteral("orphaned"), items);
    const int total = static_cast<int>(outcome.recoverable.size() + outcome.orphaned.size());

    QJsonObject data{{QStringLiteral("path"), info.absoluteFilePath()},
                     {QStringLiteral("name"), info.fileName()},
                     {QStringLiteral("recoverable_count"),
                      static_cast<int>(outcome.recoverable.size())},
                     {QStringLiteral("orphaned_count"), static_cast<int>(outcome.orphaned.size())},
                     {QStringLiteral("reported_count"), items.size()},
                     {QStringLiteral("truncated"), total > items.size()},
                     {QStringLiteral("recoverable_scan_reliable"), outcome.recoverable_reliable},
                     {QStringLiteral("orphan_scan_included"), include_orphans},
                     {QStringLiteral("orphan_scan_reliable"), outcome.orphan_reliable},
                     {QStringLiteral("timed_out"), outcome.timed_out},
                     {QStringLiteral("items"), items}};

    QString message = QStringLiteral("Recoverable inventory for '%1': %2 recoverable + %3 orphaned")
                          .arg(info.fileName())
                          .arg(outcome.recoverable.size())
                          .arg(outcome.orphaned.size());
    if (!outcome.recoverable_reliable) {
        message += QStringLiteral(" (recoverable scan hit a read error; the count is incomplete)");
    }
    if (!outcome.orphan_reliable) {
        message += QStringLiteral(" (orphan scan skipped: reachability set incomplete)");
    }
    if (outcome.timed_out) {
        message += QStringLiteral(" (scan hit the time limit; results are partial)");
    }
    return {true, message, data};
}

// Recover an inventory of DELETED/orphaned items from a PST/OST: soft-deleted items still in the
// Recoverable Items hierarchy plus (optionally) hard-deleted orphaned NBT message nodes. Drives the
// app's OWN DeletedItemScanner over the already-open PstParser. Synchronous scan methods run inline
// (the read_pst pattern) under a wall-time ceiling. Honesty: isOpen() is the value channel for the
// parse; reachableReliable() distinguishes "0 orphans" from "orphan scan skipped"; timed_out flags
// a partial scan. Read-only: reads message metadata, never writes or restores anything.
AppActionResult recoverDeleted(const QJsonObject& args) {
    const QString path = args.value(QStringLiteral("path")).toString().trimmed();
    QFileInfo info;
    if (auto fail = validatePstReadPath(path, QStringLiteral("recover_deleted"), info)) {
        return *fail;
    }

    PstParser parser;
    parser.open(path);
    if (!parser.isOpen()) {
        return {false,
                QStringLiteral("Could not open or parse the PST/OST (not a valid PST/OST, "
                               "unsupported, or corrupt): %1")
                    .arg(info.fileName()),
                {}};
    }

    const bool include_orphans = args.value(QStringLiteral("include_orphans")).toBool(true);
    const DeletedScanOutcome outcome = runDeletedItemScan(parser, include_orphans);
    return buildRecoverResult(info, outcome, include_orphans);
}

QJsonArray jsonStrings(const QVector<QString>& values) {
    QJsonArray out;
    for (const QString& value : values) {
        out.append(value);
    }
    return out;
}

QJsonObject serializeAdapter(const NetworkAdapterInfo& adapter) {
    return QJsonObject{
        {QStringLiteral("name"), adapter.name},
        {QStringLiteral("description"), adapter.description},
        {QStringLiteral("type"), adapter.adapterType},
        {QStringLiteral("mac"), adapter.macAddress},
        {QStringLiteral("interface_index"), static_cast<int>(adapter.interfaceIndex)},
        {QStringLiteral("connected"), adapter.isConnected},
        {QStringLiteral("link_speed_bps"), static_cast<double>(adapter.linkSpeedBps)},
        {QStringLiteral("media_state"), adapter.mediaState},
        {QStringLiteral("ipv4"), jsonStrings(adapter.ipv4Addresses)},
        {QStringLiteral("ipv4_gateway"), adapter.ipv4Gateway},
        {QStringLiteral("ipv4_dns"), jsonStrings(adapter.ipv4DnsServers)},
        {QStringLiteral("dhcp_enabled"), adapter.dhcpEnabled},
        {QStringLiteral("dhcp_server"), adapter.dhcpServer},
        {QStringLiteral("ipv6"), jsonStrings(adapter.ipv6Addresses)}};
}

// Enumerate network adapters via NetworkAdapterInspector (GetAdaptersAddresses:
// local, no admin, no network traffic). scan() BLOCKS and emits scanComplete inline
// on this thread, so a direct connection captures the result with no event loop --
// the same synchronous pattern as hardware_scan.
AppActionResult listAdapters(const QJsonObject&) {
    NetworkAdapterInspector inspector;
    QVector<NetworkAdapterInfo> adapters;
    bool captured = false;
    QObject::connect(&inspector,
                     &NetworkAdapterInspector::scanComplete,
                     &inspector,
                     [&adapters, &captured](const QVector<NetworkAdapterInfo>& result) {
                         adapters = result;
                         captured = true;
                     });
    inspector.scan();
    if (!captured) {
        return {false, QStringLiteral("Network adapter scan did not complete"), {}};
    }
    QJsonArray listed;
    for (const NetworkAdapterInfo& adapter : adapters) {
        if (listed.size() >= kMaxAdapters) {
            break;
        }
        listed.append(serializeAdapter(adapter));
    }
    QJsonObject data{{QStringLiteral("adapter_count"), adapters.size()},
                     {QStringLiteral("truncated"), adapters.size() > listed.size()},
                     {QStringLiteral("adapters"), listed}};
    return {true, QStringLiteral("Enumerated %1 network adapter(s)").arg(adapters.size()), data};
}

QJsonObject serializeConnection(const ConnectionInfo& c) {
    return QJsonObject{{QStringLiteral("protocol"),
                        c.protocol == ConnectionInfo::Protocol::TCP ? QStringLiteral("TCP")
                                                                    : QStringLiteral("UDP")},
                       {QStringLiteral("local_address"), c.localAddress},
                       {QStringLiteral("local_port"), static_cast<int>(c.localPort)},
                       {QStringLiteral("remote_address"), c.remoteAddress},
                       {QStringLiteral("remote_port"), static_cast<int>(c.remotePort)},
                       {QStringLiteral("state"), c.state},
                       {QStringLiteral("pid"), static_cast<int>(c.processId)},
                       {QStringLiteral("process_name"), c.processName},
                       {QStringLiteral("process_path"), c.processPath},
                       {QStringLiteral("service"), c.serviceName}};
}

// Enumerate active TCP connections + UDP listeners via ActiveConnectionsMonitor
// (GetExtendedTcpTable/GetExtendedUdpTable + PID->process mapping: local, no admin, no
// network traffic). Reverse-DNS is FORCED off (resolveHostnames=false) so the enumeration
// is pure local table reads with no blocking lookup -- the model can dns_query a remote
// address separately. startMonitoring() sets the config and does one blocking refresh inline,
// then stopMonitoring() kills the just-created timer (which never fires -- no event loop pumps
// this thread); the snapshot is read back directly. Read-only.
AppActionResult listConnections(const QJsonObject& args) {
    ActiveConnectionsMonitor monitor;
    ActiveConnectionsMonitor::MonitorConfig config;
    config.resolveHostnames = false;
    config.resolveProcessNames = true;
    config.showTcp = args.value(QStringLiteral("show_tcp")).toBool(true);
    config.showUdp = args.value(QStringLiteral("show_udp")).toBool(true);
    config.filterProcessName = args.value(QStringLiteral("filter_process")).toString().trimmed();
    // (clampArgInt is defined later in this TU; a filter port is a simple in-range check.)
    const int filter_port = args.value(QStringLiteral("filter_port")).toInt(0);
    config.filterPort =
        static_cast<uint16_t>((filter_port >= 1 && filter_port <= 65'535) ? filter_port : 0);
    monitor.startMonitoring(config);
    monitor.stopMonitoring();
    const QVector<ConnectionInfo> connections = monitor.getCurrentConnections();

    // A kernel table read that FAILED (e.g. ERROR_NOT_SUPPORTED on a degraded/IPv6-only host)
    // returns an empty set that is otherwise indistinguishable from "genuinely no connections".
    // If a requested table errored AND nothing was read, report an honest failure rather than a
    // misleading "0 connections" success (mirrors the port_scan all-unreachable guard). A partial
    // read (some rows present) is still reported as success.
    if (monitor.lastRefreshHadError() && connections.isEmpty()) {
        return {false,
                QStringLiteral("Connection-table read failed (the TCP/UDP table could not be "
                               "queried on this host)"),
                {}};
    }

    QJsonArray listed;
    int tcp = 0;
    int udp = 0;
    for (const ConnectionInfo& c : connections) {
        if (c.protocol == ConnectionInfo::Protocol::TCP) {
            ++tcp;
        } else {
            ++udp;
        }
        if (listed.size() < kMaxConnections) {
            listed.append(serializeConnection(c));
        }
    }
    QJsonObject data{{QStringLiteral("count"), connections.size()},
                     {QStringLiteral("tcp_count"), tcp},
                     {QStringLiteral("udp_count"), udp},
                     {QStringLiteral("reported_count"), listed.size()},
                     {QStringLiteral("truncated"), connections.size() > listed.size()},
                     {QStringLiteral("connections"), listed}};
    return {true,
            QStringLiteral("Enumerated %1 connection(s): %2 TCP, %3 UDP")
                .arg(connections.size())
                .arg(tcp)
                .arg(udp),
            data};
}

QJsonObject serializeWifiNetwork(const WiFiNetworkInfo& net) {
    return QJsonObject{{QStringLiteral("ssid"), clampLine(net.ssid)},
                       {QStringLiteral("bssid"), net.bssid},
                       {QStringLiteral("signal_quality"), net.signalQuality},
                       {QStringLiteral("rssi_dbm"), net.rssiDbm},
                       {QStringLiteral("channel"), net.channelNumber},
                       {QStringLiteral("band"), net.band},
                       {QStringLiteral("channel_width_mhz"), net.channelWidthMHz},
                       {QStringLiteral("authentication"), net.authentication},
                       {QStringLiteral("encryption"), net.encryption},
                       {QStringLiteral("secure"), net.isSecure},
                       {QStringLiteral("connected"), net.isConnected},
                       {QStringLiteral("vendor"), clampLine(net.apVendor)}};
}

QJsonObject serializeWifiChannel(const WiFiChannelUtilization& ch) {
    return QJsonObject{{QStringLiteral("channel"), ch.channelNumber},
                       {QStringLiteral("band"), ch.band},
                       {QStringLiteral("network_count"), ch.networkCount},
                       {QStringLiteral("avg_signal_dbm"), ch.averageSignalDbm},
                       {QStringLiteral("interference_score"), ch.interferenceScore}};
}

// Scan for nearby WiFi networks via WiFiAnalyzer (Windows Native WiFi / wlanapi: passive
// listen + a single WlanScan trigger, no target host, no packets to any peer). scan() BLOCKS
// for a fixed ~500ms scan settle plus the local BSS-list read and emits scanComplete inline on
// this thread (captured by a direct connection, no event loop) -- the hardware_scan pattern.
// On a host with no WiFi radio -- or a driver/radio failure that yields no data (radio off) --
// the engine emits errorOccurred, which maps to an honest op failure (fail-closed, not a
// misleading "0 networks" success). Read-only. Also derives channel utilization from the scan.
//
// Privacy note (conscious acceptance, adversarial review): the result exposes nearby BSSIDs (AP
// MAC addresses), which are geolocatable via wardriving databases -- a physical-location axis the
// other network ops lack. Accepted as a standard no-admin diagnostic (equivalent to
// `netsh wlan show networks mode=bssid`), consistent with list_connections already exposing
// remote endpoints and user-profile process paths.
AppActionResult wifiScan(const QJsonObject&) {
    WiFiAnalyzer analyzer;
    QVector<WiFiNetworkInfo> networks;
    bool captured = false;
    QString hard_error;
    QObject::connect(&analyzer,
                     &WiFiAnalyzer::scanComplete,
                     &analyzer,
                     [&networks, &captured](const QVector<WiFiNetworkInfo>& result) {
                         networks = result;
                         captured = true;
                     });
    QObject::connect(&analyzer,
                     &WiFiAnalyzer::errorOccurred,
                     &analyzer,
                     [&hard_error](const QString& error) { hard_error = error; });
    analyzer.scan();

    if (!captured || !hard_error.isEmpty()) {
        return {false,
                hard_error.isEmpty() ? QStringLiteral("WiFi scan did not complete") : hard_error,
                {}};
    }

    QJsonArray listed;
    for (const WiFiNetworkInfo& net : networks) {
        if (listed.size() >= kMaxWifiNetworks) {
            break;
        }
        listed.append(serializeWifiNetwork(net));
    }
    const QVector<WiFiChannelUtilization> utilization =
        WiFiAnalyzer::calculateChannelUtilization(networks);
    QJsonArray channels;
    for (const WiFiChannelUtilization& ch : utilization) {
        if (channels.size() >= kMaxWifiChannels) {
            break;
        }
        channels.append(serializeWifiChannel(ch));
    }
    QJsonObject data{{QStringLiteral("network_count"), networks.size()},
                     {QStringLiteral("reported_count"), listed.size()},
                     {QStringLiteral("truncated"), networks.size() > listed.size()},
                     {QStringLiteral("networks"), listed},
                     {QStringLiteral("channels"), channels},
                     {QStringLiteral("channels_truncated"), utilization.size() > channels.size()}};
    return {true, QStringLiteral("WiFi scan: %1 network(s) found").arg(networks.size()), data};
}

// List the machine's SAVED WiFi profiles (distinct from network.wifi_scan, which scans nearby
// broadcasting APs). Surfaces only the profile name and security type -- the DPAPI-protected key
// material (WLANProfile XML) that scanAllWifiProfiles can export is deliberately NOT requested
// (include_xml=false) and never reaches the model.
AppActionResult listWifiProfiles(const QJsonObject&) {
    bool scan_ok = false;
    const QVector<WifiProfileInfo> profiles =
        scanAllWifiProfiles(nullptr, &scan_ok, /*include_xml=*/false);
    if (!scan_ok) {
        return {false,
                QStringLiteral("Could not enumerate WiFi profiles (WLAN AutoConfig service "
                               "unavailable, no wireless adapter, or netsh failed)"),
                {}};
    }

    QJsonArray listed;
    for (const WifiProfileInfo& profile : profiles) {
        if (listed.size() >= kMaxWifiProfiles) {
            break;
        }
        listed.append(
            QJsonObject{{QStringLiteral("profile_name"), clampLine(profile.profile_name)},
                        {QStringLiteral("security_type"), clampLine(profile.security_type)}});
    }
    QJsonObject data{{QStringLiteral("profile_count"), profiles.size()},
                     {QStringLiteral("reported_count"), listed.size()},
                     {QStringLiteral("truncated"), profiles.size() > listed.size()},
                     {QStringLiteral("profiles"), listed}};
    return {true, QStringLiteral("Found %1 saved WiFi profile(s)").arg(profiles.size()), data};
}

// Build a Windows WLAN setup .cmd for a network (the WiFi panel's "export setup script" feature,
// driving the shared, injection-hardened sak::buildWifiSetupScriptWindows). READ-ONLY: it only
// BUILDS text -- no profile is added, no netsh runs, nothing is written to disk. The generated
// script (which the user saves + runs elevated) adds the profile then connects. An empty/unsafe
// SSID (double quote or control char, e.g. from a rogue AP) is refused fail-closed. The script
// embeds the supplied WiFi password verbatim -- the caller already provided it -- so the payload is
// sensitive; the note says so.
AppActionResult generateWifiSetupScript(const QJsonObject& args) {
    const QString ssid = args.value(QStringLiteral("ssid")).toString();
    if (ssid.trimmed().isEmpty()) {
        return {false,
                QStringLiteral("generate_wifi_setup_script requires a non-empty 'ssid'"),
                {}};
    }
    const QString password = args.value(QStringLiteral("password")).toString();
    const QString security = args.value(QStringLiteral("security")).toString();
    const bool hidden = args.value(QStringLiteral("hidden")).toBool(false);

    const QString script = sak::buildWifiSetupScriptWindows(ssid, password, security, hidden);
    if (script.isEmpty()) {
        return {false,
                QStringLiteral("SSID contains characters that cannot be safely scripted (a double "
                               "quote or a control character); refusing to build the script"),
                {}};
    }
    // Honest disclosure: the profile embeds <keyMaterial> ONLY for a passphrase network (WPA2) with
    // a non-empty password -- an open/WEP network omits it even if a password was supplied.
    const bool embeds_password = !password.isEmpty() && sak::wifiSecurityUsesPassphrase(security);
    const QString note =
        embeds_password
            ? QStringLiteral(
                  "Save as a .cmd and run as Administrator. It adds the WLAN profile then "
                  "connects; the script contains the WiFi password in plain text.")
            : QStringLiteral(
                  "Save as a .cmd and run as Administrator. It adds the WLAN profile then "
                  "connects.");
    QJsonObject data{{QStringLiteral("ssid"), clampLine(ssid)},
                     {QStringLiteral("platform"), QStringLiteral("windows")},
                     {QStringLiteral("security"),
                      security.isEmpty() ? QStringLiteral("wpa2") : clampLine(security)},
                     {QStringLiteral("hidden"), hidden},
                     {QStringLiteral("embeds_password"), embeds_password},
                     {QStringLiteral("script"), script},
                     {QStringLiteral("note"), note}};
    return {true,
            QStringLiteral("Built a Windows WiFi setup script for '%1'").arg(clampLine(ssid)),
            data};
}

QJsonObject generateWifiSetupScriptParamsSchema() {
    const auto strProp = [](const QString& desc) {
        return QJsonObject{{QStringLiteral("type"), QStringLiteral("string")},
                           {QStringLiteral("description"), desc}};
    };
    QJsonObject security_prop{
        {QStringLiteral("type"), QStringLiteral("string")},
        {QStringLiteral("enum"),
         QJsonArray{QStringLiteral("wpa2"), QStringLiteral("wep"), QStringLiteral("open")}},
        {QStringLiteral("description"),
         QStringLiteral("Security type; anything other than wep/open is treated as WPA2-PSK "
                        "(default wpa2)")}};
    QJsonObject hidden_prop{{QStringLiteral("type"), QStringLiteral("boolean")},
                            {QStringLiteral("description"),
                             QStringLiteral("True if the network does not broadcast its SSID")}};
    QJsonObject properties{
        {QStringLiteral("ssid"), strProp(QStringLiteral("Network name (SSID) to connect to"))},
        {QStringLiteral("password"),
         strProp(QStringLiteral("WiFi passphrase (omit/empty for an open network)"))},
        {QStringLiteral("security"), security_prop},
        {QStringLiteral("hidden"), hidden_prop}};
    return QJsonObject{{QStringLiteral("type"), QStringLiteral("object")},
                       {QStringLiteral("properties"), properties},
                       {QStringLiteral("required"), QJsonArray{QStringLiteral("ssid")}},
                       {QStringLiteral("additionalProperties"), false}};
}

QString shareTypeToString(NetworkShareInfo::ShareType type) {
    switch (type) {
    case NetworkShareInfo::ShareType::Disk:
        return QStringLiteral("disk");
    case NetworkShareInfo::ShareType::Printer:
        return QStringLiteral("printer");
    case NetworkShareInfo::ShareType::Device:
        return QStringLiteral("device");
    case NetworkShareInfo::ShareType::IPC:
        return QStringLiteral("ipc");
    case NetworkShareInfo::ShareType::Special:
        break;
    }
    return QStringLiteral("special");
}

QJsonObject serializeShare(const NetworkShareInfo& share) {
    // Access fields (canRead/canWrite) are intentionally omitted: the read-only enumeration
    // performs no access probe, so they carry no meaning here.
    return QJsonObject{{QStringLiteral("share_name"), clampLine(share.shareName)},
                       {QStringLiteral("unc_path"), clampLine(share.uncPath)},
                       {QStringLiteral("type"), shareTypeToString(share.type)},
                       {QStringLiteral("remark"), clampLine(share.remark)},
                       {QStringLiteral("hidden"), share.requiresAuth},
                       {QStringLiteral("host"), clampLine(share.hostName)}};
}

// Enumerate the LOCAL machine's SMB/Windows file shares via NetworkShareBrowser::listSharesReadOnly
// (NetShareEnum with a NULL server name = a pure local API read: no SMB round-trip, no credential
// exposure). READ-ONLY: unlike the panel's discoverShares, this path performs NO write-access
// probe (discoverShares writes a temporary file to every writable disk share -- a mutation), so
// nothing is ever written to any share and canRead/canWrite are not reported. SYNC like
// list_connections: listSharesReadOnly BLOCKS and returns the vector directly (no event loop).
//
// LOCAL-ONLY BY DESIGN (no target argument): enumerating a REMOTE host's shares authenticates
// with the operator's current credentials, an NTLM-relay / credential-exposure vector that a
// prompt-injectable model must not be able to aim at an attacker-controlled host. Taking no
// hostname makes that structurally impossible -- no model-supplied string ever reaches
// NetShareEnum. Remote share enumeration remains available in the GUI panel. Fail-CLOSED: a
// NetShareEnum failure is an honest op failure via the ok signal, never a "0 shares" success.
AppActionResult listShares(const QJsonObject&) {
    NetworkShareBrowser browser;
    bool ok = false;
    // Empty hostname => local enumeration (NULL server name) inside the engine.
    const QVector<NetworkShareInfo> shares = browser.listSharesReadOnly(QString(), ok);
    if (!ok) {
        return {false,
                QStringLiteral("Could not enumerate the local machine's shares (the query failed)"),
                {}};
    }

    QJsonArray listed;
    for (const NetworkShareInfo& share : shares) {
        if (listed.size() >= kMaxShares) {
            break;
        }
        listed.append(serializeShare(share));
    }
    QJsonObject data{{QStringLiteral("count"), shares.size()},
                     {QStringLiteral("reported_count"), listed.size()},
                     {QStringLiteral("truncated"), shares.size() > listed.size()},
                     {QStringLiteral("access_tested"), false},
                     {QStringLiteral("shares"), listed}};
    return {true,
            QStringLiteral("Found %1 share(s) on the local machine").arg(shares.size()),
            data};
}

QString fwDirectionToString(FirewallRule::Direction d) {
    return d == FirewallRule::Direction::Inbound ? QStringLiteral("inbound")
                                                 : QStringLiteral("outbound");
}

QString fwActionToString(FirewallRule::Action a) {
    return a == FirewallRule::Action::Allow ? QStringLiteral("allow") : QStringLiteral("block");
}

QString fwProtocolToString(FirewallRule::Protocol p) {
    switch (p) {
    case FirewallRule::Protocol::TCP:
        return QStringLiteral("TCP");
    case FirewallRule::Protocol::UDP:
        return QStringLiteral("UDP");
    case FirewallRule::Protocol::ICMPv4:
        return QStringLiteral("ICMPv4");
    case FirewallRule::Protocol::ICMPv6:
        return QStringLiteral("ICMPv6");
    case FirewallRule::Protocol::Any:
        return QStringLiteral("Any");
    case FirewallRule::Protocol::Other:
        break;
    }
    return QStringLiteral("Other");
}

QJsonArray fwProfilesToJson(int profiles) {
    QJsonArray out;
    if ((profiles & static_cast<int>(FirewallRule::Profile::Domain)) != 0) {
        out.append(QStringLiteral("Domain"));
    }
    if ((profiles & static_cast<int>(FirewallRule::Profile::Private)) != 0) {
        out.append(QStringLiteral("Private"));
    }
    if ((profiles & static_cast<int>(FirewallRule::Profile::Public)) != 0) {
        out.append(QStringLiteral("Public"));
    }
    return out;
}

QString fwSeverityToString(int severity) {
    // FirewallConflict::Severity {Info=0, Warning=1, Critical=2}; FirewallGap::Severity
    // {Info=0, Warning=1} share the low values, so one mapping serves both.
    switch (severity) {
    case 1:
        return QStringLiteral("warning");
    case 2:
        return QStringLiteral("critical");
    default:
        return QStringLiteral("info");
    }
}

QJsonObject serializeFirewallRule(const FirewallRule& rule) {
    return QJsonObject{{QStringLiteral("name"), clampLine(rule.name)},
                       {QStringLiteral("enabled"), rule.enabled},
                       {QStringLiteral("direction"), fwDirectionToString(rule.direction)},
                       {QStringLiteral("action"), fwActionToString(rule.action)},
                       {QStringLiteral("protocol"), fwProtocolToString(rule.protocol)},
                       {QStringLiteral("local_ports"), clampLine(rule.localPorts)},
                       {QStringLiteral("remote_ports"), clampLine(rule.remotePorts)},
                       {QStringLiteral("local_addresses"), clampLine(rule.localAddresses)},
                       {QStringLiteral("remote_addresses"), clampLine(rule.remoteAddresses)},
                       {QStringLiteral("application_path"), clampLine(rule.applicationPath)},
                       {QStringLiteral("service_name"), clampLine(rule.serviceName)},
                       {QStringLiteral("profiles"), fwProfilesToJson(rule.profiles)},
                       {QStringLiteral("grouping"), clampLine(rule.grouping)}};
}

QJsonObject serializeFirewallConflict(const FirewallConflict& c) {
    return QJsonObject{{QStringLiteral("rule_a"), clampLine(c.ruleA.name)},
                       {QStringLiteral("rule_b"), clampLine(c.ruleB.name)},
                       {QStringLiteral("description"), clampLine(c.conflictDescription)},
                       {QStringLiteral("severity"),
                        fwSeverityToString(static_cast<int>(c.severity))}};
}

QJsonObject serializeFirewallGap(const FirewallGap& g) {
    return QJsonObject{{QStringLiteral("description"), clampLine(g.description)},
                       {QStringLiteral("recommendation"), clampLine(g.recommendation)},
                       {QStringLiteral("severity"),
                        fwSeverityToString(static_cast<int>(g.severity))}};
}

// True if @p rule passes the optional name-substring / enabled-only filter. Filters only trim
// the returned rules array for readability; the conflict/gap analysis always runs over the FULL
// rule set (a coverage gap is a property of the whole policy, not the filtered view).
bool firewallRuleMatchesFilter(const FirewallRule& rule,
                               const QString& name_filter,
                               bool enabled_only) {
    if (enabled_only && !rule.enabled) {
        return false;
    }
    return name_filter.isEmpty() || rule.name.contains(name_filter, Qt::CaseInsensitive);
}

// Build the audit result payload: the returned rules array is filtered + capped, while the
// conflict/gap analysis (computed by the engine over the FULL rule set) is serialized whole and
// capped. Split out of auditFirewall to keep that function within the complexity/length budget.
QJsonObject serializeFirewallAudit(const QVector<FirewallRule>& rules,
                                   const QVector<FirewallConflict>& conflicts,
                                   const QVector<FirewallGap>& gaps,
                                   const QString& name_filter,
                                   bool enabled_only) {
    QJsonArray listed_rules;
    int matched = 0;
    for (const FirewallRule& rule : rules) {
        if (!firewallRuleMatchesFilter(rule, name_filter, enabled_only)) {
            continue;
        }
        ++matched;
        if (listed_rules.size() < kMaxFirewallRules) {
            listed_rules.append(serializeFirewallRule(rule));
        }
    }
    QJsonArray listed_conflicts;
    for (const FirewallConflict& c : conflicts) {
        if (listed_conflicts.size() >= kMaxFirewallConflicts) {
            break;
        }
        listed_conflicts.append(serializeFirewallConflict(c));
    }
    QJsonArray listed_gaps;
    for (const FirewallGap& g : gaps) {
        if (listed_gaps.size() >= kMaxFirewallGaps) {
            break;
        }
        listed_gaps.append(serializeFirewallGap(g));
    }
    return QJsonObject{{QStringLiteral("total_rules"), rules.size()},
                       {QStringLiteral("matched_rules"), matched},
                       {QStringLiteral("reported_rules"), listed_rules.size()},
                       {QStringLiteral("conflict_count"), conflicts.size()},
                       {QStringLiteral("gap_count"), gaps.size()},
                       {QStringLiteral("rules"), listed_rules},
                       {QStringLiteral("conflicts"), listed_conflicts},
                       {QStringLiteral("gaps"), listed_gaps}};
}

// Audit the Windows Firewall via FirewallRuleAuditor (INetFwPolicy2 COM: local, no network).
// fullAudit() BLOCKS and emits auditComplete (rules + conflicts + gaps) inline on this thread --
// captured by a direct connection, no event loop -- with the engine self-initialising COM
// (ComInitializer) so it is thread-safe on the AI worker thread. Any COM failure (init /
// CoCreateInstance / get_Rules / enumerator-acquisition / a mid-enumeration Next failure) emits
// errorOccurred and yields an empty result, which maps to an honest op failure (fail-closed).
// Read-only.
//
// ACCEPTED LOW residual (adversarial review): the engine's conflict scan is O(rules^2) and
// parsePorts expands a numeric port range into one element per port, so a firewall deliberately
// populated with many rule pairs holding huge disjoint numeric ranges (e.g. 1-30000 vs
// 30001-60000) could be pathologically slow -- and this op cannot cancel the stack-local
// auditor. Not reachable via model input (creating such rules needs admin) and sub-second on
// real hosts (rules short-circuit on '*'/direction/action), so it is left as-is like the other
// inline read-only enumerations rather than moved onto a ceiling-guarded worker.
AppActionResult auditFirewall(const QJsonObject& args) {
    const QString name_filter = args.value(QStringLiteral("name_filter")).toString().trimmed();
    const bool enabled_only = args.value(QStringLiteral("enabled_only")).toBool(false);

    FirewallRuleAuditor auditor;
    QVector<FirewallRule> rules;
    QVector<FirewallConflict> conflicts;
    QVector<FirewallGap> gaps;
    bool captured = false;
    QString hard_error;
    QObject::connect(&auditor,
                     &FirewallRuleAuditor::auditComplete,
                     &auditor,
                     [&](const QVector<FirewallRule>& r,
                         const QVector<FirewallConflict>& c,
                         const QVector<FirewallGap>& g) {
                         rules = r;
                         conflicts = c;
                         gaps = g;
                         captured = true;
                     });
    QObject::connect(&auditor,
                     &FirewallRuleAuditor::errorOccurred,
                     &auditor,
                     [&hard_error](const QString& error) { hard_error = error; });
    auditor.fullAudit();

    if (!captured || !hard_error.isEmpty()) {
        return {false,
                hard_error.isEmpty() ? QStringLiteral("Firewall audit did not complete")
                                     : hard_error,
                {}};
    }

    const QJsonObject data =
        serializeFirewallAudit(rules, conflicts, gaps, name_filter, enabled_only);
    return {true,
            QStringLiteral("Firewall: %1 rule(s), %2 conflict(s), %3 coverage gap(s)")
                .arg(rules.size())
                .arg(conflicts.size())
                .arg(gaps.size()),
            data};
}

QJsonObject serializeDnsResult(const DnsQueryResult& result) {
    QJsonArray answers;
    for (const QString& answer : result.answers) {
        if (answers.size() >= kMaxDnsAnswers) {
            break;
        }
        answers.append(answer);
    }
    return QJsonObject{{QStringLiteral("query_name"), result.queryName},
                       {QStringLiteral("record_type"), result.recordType},
                       {QStringLiteral("dns_server"), result.dnsServer},
                       {QStringLiteral("success"), result.success},
                       {QStringLiteral("response_time_ms"), result.responseTimeMs},
                       {QStringLiteral("ttl_seconds"), result.ttlSeconds},
                       {QStringLiteral("answer_count"), result.answers.size()},
                       {QStringLiteral("answers"), answers},
                       {QStringLiteral("error"), result.errorMessage}};
}

// Resolve a hostname via DnsDiagnosticTool (DnsQuery_W). query() BLOCKS and emits
// queryComplete (or errorOccurred) inline on this thread -- captured by direct
// connection, no event loop. Read-only: it performs a DNS lookup only. A failed
// lookup is a SUCCESSFUL op whose result.success is false (with the error), so the
// op only fails on a malformed request or a hard engine error.
AppActionResult dnsQuery(const QJsonObject& args) {
    const QString hostname = args.value(QStringLiteral("hostname")).toString().trimmed();
    if (hostname.isEmpty()) {
        return {false, QStringLiteral("dns_query requires a 'hostname' argument"), {}};
    }
    QString record_type = args.value(QStringLiteral("record_type")).toString().trimmed();
    if (record_type.isEmpty()) {
        record_type = QStringLiteral("A");
    }
    const QString dns_server = args.value(QStringLiteral("dns_server")).toString().trimmed();

    DnsDiagnosticTool tool;
    DnsQueryResult result;
    bool captured = false;
    QString hard_error;
    QObject::connect(&tool,
                     &DnsDiagnosticTool::queryComplete,
                     &tool,
                     [&result, &captured](const DnsQueryResult& value) {
                         result = value;
                         captured = true;
                     });
    QObject::connect(&tool,
                     &DnsDiagnosticTool::errorOccurred,
                     &tool,
                     [&hard_error](const QString& error) { hard_error = error; });
    tool.query(hostname, record_type, dns_server);
    if (!captured) {
        return {false,
                hard_error.isEmpty() ? QStringLiteral("DNS query did not complete") : hard_error,
                {}};
    }
    const QString message =
        result.success
            ? QStringLiteral("%1 %2: %3 answer(s)")
                  .arg(hostname, record_type)
                  .arg(result.answers.size())
            : QStringLiteral("%1 %2: %3").arg(hostname, record_type, result.errorMessage);
    return {true, message, serializeDnsResult(result)};
}

// -- Active network probes (ping / traceroute / port scan) ------------------
//
// Unlike the adapter/DNS ops above (fast, blocking-inline), these engine calls can block
// for their full config-bounded duration against an unreachable target, so each runs on a
// NetworkProbeWorker thread driven by AsyncActionInvocation: a hard wall-time ceiling plus
// a real cancel (WorkerBase stop + engine cancel()), the same pattern as find_in_files and
// partition.apply_operation. Still read-only: they send diagnostic traffic only and mutate
// no local state (parity with the network-diagnostics panel, which drives the SAME engines).

int clampArgInt(const QJsonObject& args, const char* key, int lo, int hi, int def) {
    const int value = args.value(QString::fromLatin1(key)).toInt(def);
    return std::clamp(value, lo, hi);
}

ConnectivityTester::PingConfig pingConfigFromArgs(const QString& target, const QJsonObject& args) {
    ConnectivityTester::PingConfig config;
    config.target = target;
    config.count = clampArgInt(args, "count", 1, 10, 4);
    config.timeoutMs = clampArgInt(args, "timeout_ms", 200, 4000, 2000);
    config.intervalMs = clampArgInt(args, "interval_ms", 0, 2000, 500);
    config.resolveHostnames = args.value(QStringLiteral("resolve_hostnames")).toBool(true);
    return config;
}

ConnectivityTester::TracerouteConfig tracerouteConfigFromArgs(const QString& target,
                                                              const QJsonObject& args) {
    ConnectivityTester::TracerouteConfig config;
    config.target = target;
    config.maxHops = clampArgInt(args, "max_hops", 1, 30, 30);
    config.timeoutMs = clampArgInt(args, "timeout_ms", 500, 3000, 2000);
    config.probesPerHop = clampArgInt(args, "probes_per_hop", 1, 3, 3);
    config.resolveHostnames = args.value(QStringLiteral("resolve_hostnames")).toBool(true);
    return config;
}

ConnectivityTester::MtrConfig mtrConfigFromArgs(const QString& target, const QJsonObject& args) {
    ConnectivityTester::MtrConfig config;
    config.target = target;
    // Hard-clamped so worst-case cycles*maxHops*timeoutMs stays under the wall-time ceiling
    // (see kMaxMtrCycles). Defaults favour a short, useful sample over the engine's 100-cycle GUI
    // default, which would run for many minutes headless.
    config.cycles = clampArgInt(args, "cycles", 1, kMaxMtrCycles, 3);
    config.maxHops = clampArgInt(args, "max_hops", 1, 30, 30);
    config.timeoutMs = clampArgInt(args, "timeout_ms", 500, 1500, 1000);
    config.intervalMs = clampArgInt(args, "interval_ms", 0, 2000, 500);
    return config;
}

void appendPortIfValid(QVector<uint16_t>& out, int port) {
    if (port < 1 || port > 65'535) {
        return;
    }
    const auto value = static_cast<uint16_t>(port);
    if (!out.contains(value)) {
        out.append(value);
    }
}

// Restrict the assistant's port scanner to LOCAL/PRIVATE targets. Unlike a human at the
// panel, the model is prompt-injectable, and an active TCP scan of an arbitrary public host
// is externally indistinguishable from attack reconnaissance sourced from the operator's IP
// (IDS trips, cloud-AUP strikes, blocklisting). A hostname is refused (it could resolve
// anywhere) -- callers pass a private IP literal or "localhost". ping/traceroute stay
// unrestricted: a single ICMP echo / TTL probe is a routine, low-blast-radius diagnostic.
bool isLocalScanTarget(const QString& target) {
    const QString trimmed = target.trimmed();
    if (trimmed.compare(QStringLiteral("localhost"), Qt::CaseInsensitive) == 0) {
        return true;
    }
    const QHostAddress addr(trimmed);
    if (addr.isNull()) {
        return false;  // a hostname could resolve to a public host -- require a local IP literal
    }
    if (addr.isLoopback() || addr.isLinkLocal()) {
        return true;
    }
    static const struct {
        const char* net;
        int bits;
    } kPrivate[] = {{"10.0.0.0", 8}, {"172.16.0.0", 12}, {"192.168.0.0", 16}, {"fc00::", 7}};
    for (const auto& range : kPrivate) {
        if (addr.isInSubnet(QHostAddress(QString::fromLatin1(range.net)), range.bits)) {
            return true;
        }
    }
    return false;
}

// Collect the ports to scan from an explicit "ports" array and/or a
// "port_range_start"/"port_range_end" range, deduped and hard-capped (the engine scans
// sequentially, so an unbounded port list is an unbounded wall time). Returns an error
// result to return verbatim when none are valid or the cap is exceeded. Both inputs are
// bounded BEFORE the work: the explicit array is rejected up front if oversized, and the
// range loop's upper bound is clamped to the valid port space so a start > 65535 (which
// appends nothing, leaving the size-guard untripped) can never spin -- or overflow -- an
// int counter toward INT_MAX.
std::optional<AppActionResult> collectScanPorts(const QJsonObject& args, QVector<uint16_t>& out) {
    const QJsonArray explicit_ports = args.value(QStringLiteral("ports")).toArray();
    if (explicit_ports.size() > kMaxScanPorts) {
        return AppActionResult{false,
                               QStringLiteral("port_scan is limited to %1 ports per call "
                                              "(requested %2)")
                                   .arg(kMaxScanPorts)
                                   .arg(explicit_ports.size()),
                               {}};
    }
    for (const QJsonValue& value : explicit_ports) {
        appendPortIfValid(out, value.toInt(-1));
    }
    const int range_start = args.value(QStringLiteral("port_range_start")).toInt(0);
    const int range_end = args.value(QStringLiteral("port_range_end")).toInt(0);
    if (range_start >= 1 && range_start <= 65'535 && range_end >= range_start) {
        const int last = std::min(range_end, 65'535);
        for (int port = range_start; port <= last && out.size() <= kMaxScanPorts; ++port) {
            appendPortIfValid(out, port);
        }
    }
    if (out.isEmpty()) {
        return AppActionResult{false,
                               QStringLiteral(
                                   "port_scan requires 'ports' and/or a valid 'port_range_start'/"
                                   "'port_range_end'"),
                               {}};
    }
    if (out.size() > kMaxScanPorts) {
        return AppActionResult{false,
                               QStringLiteral("port_scan is limited to %1 ports per call "
                                              "(requested %2)")
                                   .arg(kMaxScanPorts)
                                   .arg(out.size()),
                               {}};
    }
    return std::nullopt;
}

PortScanner::ScanConfig scanConfigFromArgs(const QString& target,
                                           QVector<uint16_t> ports,
                                           const QJsonObject& args) {
    PortScanner::ScanConfig config;
    config.target = target;
    config.ports = std::move(ports);
    config.timeoutMs = clampArgInt(args, "timeout_ms", 200, 3000, 1000);
    config.grabBanners = args.value(QStringLiteral("grab_banners")).toBool(true);
    return config;
}

QJsonObject serializePingReply(const PingReply& reply) {
    return QJsonObject{{QStringLiteral("sequence"), reply.sequenceNumber},
                       {QStringLiteral("success"), reply.success},
                       {QStringLiteral("rtt_ms"), reply.rttMs},
                       {QStringLiteral("ttl"), reply.ttl},
                       {QStringLiteral("reply_from"), reply.replyFrom},
                       {QStringLiteral("error"), reply.errorMessage}};
}

QJsonObject serializePingResult(const PingResult& result) {
    QJsonArray replies;
    for (const PingReply& reply : result.replies) {
        if (replies.size() >= kMaxPingReplies) {
            break;
        }
        replies.append(serializePingReply(reply));
    }
    return QJsonObject{{QStringLiteral("target"), result.target},
                       {QStringLiteral("resolved_ip"), result.resolvedIP},
                       {QStringLiteral("sent"), result.sent},
                       {QStringLiteral("received"), result.received},
                       {QStringLiteral("lost"), result.lost},
                       {QStringLiteral("loss_percent"), result.lossPercent},
                       {QStringLiteral("min_rtt_ms"), result.minRtt},
                       {QStringLiteral("max_rtt_ms"), result.maxRtt},
                       {QStringLiteral("avg_rtt_ms"), result.avgRtt},
                       {QStringLiteral("jitter_ms"), result.jitter},
                       {QStringLiteral("replies"), replies}};
}

QJsonObject serializeTracerouteHop(const TracerouteHop& hop) {
    return QJsonObject{{QStringLiteral("hop"), hop.hopNumber},
                       {QStringLiteral("ip"), hop.ipAddress},
                       {QStringLiteral("hostname"), hop.hostname},
                       {QStringLiteral("rtt1_ms"), hop.rtt1Ms},
                       {QStringLiteral("rtt2_ms"), hop.rtt2Ms},
                       {QStringLiteral("rtt3_ms"), hop.rtt3Ms},
                       {QStringLiteral("avg_rtt_ms"), hop.avgRttMs},
                       {QStringLiteral("timed_out"), hop.timedOut}};
}

QJsonObject serializeTracerouteResult(const TracerouteResult& result) {
    QJsonArray hops;
    for (const TracerouteHop& hop : result.hops) {
        if (hops.size() >= kMaxTracerouteHops) {
            break;
        }
        hops.append(serializeTracerouteHop(hop));
    }
    return QJsonObject{{QStringLiteral("target"), result.target},
                       {QStringLiteral("resolved_ip"), result.resolvedIP},
                       {QStringLiteral("reached_target"), result.reachedTarget},
                       {QStringLiteral("total_hops"), result.totalHops},
                       {QStringLiteral("hops"), hops}};
}

QJsonObject serializeMtrHop(const MtrHopStats& hop) {
    return QJsonObject{{QStringLiteral("hop"), hop.hopNumber},
                       {QStringLiteral("ip"), hop.ipAddress},
                       {QStringLiteral("hostname"), hop.hostname},
                       {QStringLiteral("sent"), hop.sent},
                       {QStringLiteral("received"), hop.received},
                       {QStringLiteral("loss_percent"), hop.lossPercent},
                       {QStringLiteral("last_rtt_ms"), hop.lastRttMs},
                       {QStringLiteral("avg_rtt_ms"), hop.avgRttMs},
                       {QStringLiteral("best_rtt_ms"), hop.bestRttMs},
                       {QStringLiteral("worst_rtt_ms"), hop.worstRttMs},
                       {QStringLiteral("jitter_ms"), hop.jitterMs}};
}

QJsonObject serializeMtrResult(const MtrResult& result) {
    QJsonArray hops;
    for (const MtrHopStats& hop : result.hops) {
        if (hops.size() >= kMaxTracerouteHops) {
            break;
        }
        hops.append(serializeMtrHop(hop));
    }
    return QJsonObject{{QStringLiteral("target"), result.target},
                       {QStringLiteral("total_cycles"), result.totalCycles},
                       {QStringLiteral("hop_count"), result.hops.size()},
                       {QStringLiteral("hops"), hops}};
}

QString portStateToString(PortScanResult::State state) {
    switch (state) {
    case PortScanResult::State::Open:
        return QStringLiteral("open");
    case PortScanResult::State::Closed:
        return QStringLiteral("closed");
    case PortScanResult::State::Filtered:
        return QStringLiteral("filtered");
    case PortScanResult::State::Error:
        break;
    }
    return QStringLiteral("error");
}

QJsonObject serializePortResult(const PortScanResult& result) {
    return QJsonObject{{QStringLiteral("port"), static_cast<int>(result.port)},
                       {QStringLiteral("state"), portStateToString(result.state)},
                       {QStringLiteral("service"), result.serviceName},
                       {QStringLiteral("response_time_ms"), result.responseTimeMs},
                       {QStringLiteral("banner"), clampLine(result.banner)},
                       {QStringLiteral("error"), result.errorMessage}};
}

QJsonObject serializePortScan(const QString& target, const QVector<PortScanResult>& results) {
    QJsonArray listed;
    int open = 0;
    for (const PortScanResult& result : results) {
        if (result.state == PortScanResult::State::Open) {
            ++open;
        }
        if (listed.size() < kMaxScanResults) {
            listed.append(serializePortResult(result));
        }
    }
    return QJsonObject{{QStringLiteral("target"), target},
                       {QStringLiteral("scanned_count"), results.size()},
                       {QStringLiteral("open_count"), open},
                       {QStringLiteral("reported_count"), listed.size()},
                       {QStringLiteral("truncated"), results.size() > listed.size()},
                       {QStringLiteral("results"), listed}};
}

// Drive one NetworkProbeWorker to completion with a hard timeout and a real cancel, then
// map the outcome. A captured run with an empty error() is a success (its data may still
// show 100% loss / target-not-reached / all-filtered -- like a failed DNS lookup); a
// non-empty error() (e.g. a resolve failure, which also emits *Complete) is a failure.
AppActionResult driveNetworkProbe(NetworkProbeWorker& worker,
                                  const std::function<AppActionResult()>& on_captured) {
    AsyncActionInvocation inv(kNetworkProbeTimeoutMs);
    QObject::connect(
        &worker, &WorkerBase::finished, inv.context(), [&inv, &worker, &on_captured]() {
            if (worker.captured() && worker.error().isEmpty()) {
                inv.finish(on_captured());
                return;
            }
            inv.finish({false,
                        worker.error().isEmpty() ? QStringLiteral("Network probe did not complete")
                                                 : worker.error(),
                        {}});
        });
    QObject::connect(&worker,
                     &WorkerBase::failed,
                     inv.context(),
                     [&inv](int, const QString& error) { inv.finish({false, error, {}}); });
    QObject::connect(&worker, &WorkerBase::cancelled, inv.context(), [&inv]() {
        inv.finish({false, QStringLiteral("Network probe was cancelled"), {}});
    });
    const AppActionResult result = inv.run([&worker]() { worker.start(); });
    worker.cancelExecution();  // cooperative stop before teardown (no-op if already finished)
    return result;
}

AppActionResult pingHost(const QJsonObject& args) {
    const QString target = args.value(QStringLiteral("target")).toString().trimmed();
    if (target.isEmpty()) {
        return {false, QStringLiteral("ping requires a 'target' argument"), {}};
    }
    NetworkProbeWorker worker(pingConfigFromArgs(target, args));
    return driveNetworkProbe(worker, [&worker, target]() {
        const PingResult& r = worker.pingResult();
        // QString::arg is not printf: it does not collapse "%%". "%4% loss" fills %4 with
        // the number and leaves the trailing "% loss" literal.
        const QString message = QStringLiteral("%1: %2/%3 replies, %4% loss, avg %5 ms")
                                    .arg(target)
                                    .arg(r.received)
                                    .arg(r.sent)
                                    .arg(r.lossPercent, 0, 'f', 1)
                                    .arg(r.avgRtt, 0, 'f', 1);
        return AppActionResult{true, message, serializePingResult(r)};
    });
}

AppActionResult tracerouteHost(const QJsonObject& args) {
    const QString target = args.value(QStringLiteral("target")).toString().trimmed();
    if (target.isEmpty()) {
        return {false, QStringLiteral("traceroute requires a 'target' argument"), {}};
    }
    NetworkProbeWorker worker(tracerouteConfigFromArgs(target, args));
    return driveNetworkProbe(worker, [&worker, target]() {
        const TracerouteResult& r = worker.tracerouteResult();
        const QString message =
            r.reachedTarget
                ? QStringLiteral("%1: reached in %2 hop(s)").arg(target).arg(r.totalHops)
                : QStringLiteral("%1: %2 hop(s), target not reached").arg(target).arg(r.totalHops);
        return AppActionResult{true, message, serializeTracerouteResult(r)};
    });
}

AppActionResult mtrHost(const QJsonObject& args) {
    const QString target = args.value(QStringLiteral("target")).toString().trimmed();
    if (target.isEmpty()) {
        return {false, QStringLiteral("mtr requires a 'target' argument"), {}};
    }
    NetworkProbeWorker worker(mtrConfigFromArgs(target, args));
    return driveNetworkProbe(worker, [&worker, target]() {
        const MtrResult& r = worker.mtrResult();
        // A resolvable target where NO hop at any TTL ever answers (offline, ICMP-filtered,
        // or an unroutable literal) leaves the hop list empty and totalCycles 0. The engine
        // emits no errorOccurred for this, so without a guard it would map to an affirmative
        // "0 cycle(s), 0 hop(s), worst-hop loss 0.0%" success that reads as healthy
        // connectivity. Report it honestly as a failure, mirroring the port_scan
        // all-unreachable guard. A non-empty hop list always has >=1 responder, so its
        // per-hop loss (up to 100% on a dead final hop) is reported truthfully below.
        if (r.hops.isEmpty()) {
            return AppActionResult{false,
                                   QStringLiteral(
                                       "%1: target unreachable -- no hop produced an ICMP response")
                                       .arg(target),
                                   serializeMtrResult(r)};
        }
        double worst_loss = 0.0;
        for (const MtrHopStats& hop : r.hops) {
            worst_loss = std::max(worst_loss, hop.lossPercent);
        }
        const QString message = QStringLiteral("%1: %2 cycle(s), %3 hop(s), worst-hop loss %4%")
                                    .arg(target)
                                    .arg(r.totalCycles)
                                    .arg(r.hops.size())
                                    .arg(worst_loss, 0, 'f', 1);
        return AppActionResult{true, message, serializeMtrResult(r)};
    });
}

AppActionResult portScan(const QJsonObject& args) {
    const QString target = args.value(QStringLiteral("target")).toString().trimmed();
    if (target.isEmpty()) {
        return {false, QStringLiteral("port_scan requires a 'target' argument"), {}};
    }
    if (!isLocalScanTarget(target)) {
        return {false,
                QStringLiteral("port_scan is limited to local/private targets (loopback, 10/8, "
                               "172.16/12, 192.168/16, link-local, or 'localhost'); scanning "
                               "public or remote hosts is disabled for the assistant -- pass a "
                               "private IP literal"),
                {}};
    }
    QVector<uint16_t> ports;
    if (const std::optional<AppActionResult> error = collectScanPorts(args, ports)) {
        return *error;
    }
    NetworkProbeWorker worker(scanConfigFromArgs(target, std::move(ports), args));
    return driveNetworkProbe(worker, [&worker, target]() {
        const QVector<PortScanResult>& r = worker.portScanResult();
        int open = 0;
        int errored = 0;
        for (const PortScanResult& result : r) {
            if (result.state == PortScanResult::State::Open) {
                ++open;
            } else if (result.state == PortScanResult::State::Error) {
                ++errored;
            }
        }
        // Every port a socket-level Error (not a clean open/closed/filtered) means the host
        // never answered at the TCP layer -- unreachable/offline. Report that honestly as a
        // failure rather than a misleading "0 open of N scanned" that reads as "host up".
        if (!r.isEmpty() && errored == r.size()) {
            return AppActionResult{
                false,
                QStringLiteral("%1: target unreachable -- no port produced a TCP-level response")
                    .arg(target),
                serializePortScan(target, r)};
        }
        const QString message =
            QStringLiteral("%1: %2 open of %3 scanned").arg(target).arg(open).arg(r.size());
        return AppActionResult{true, message, serializePortScan(target, r)};
    });
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

QJsonObject benchmarkParamsSchema() {
    QJsonObject target_prop{
        {QStringLiteral("type"), QStringLiteral("string")},
        {QStringLiteral("enum"), QJsonArray{QStringLiteral("cpu"), QStringLiteral("memory")}},
        {QStringLiteral("description"),
         QStringLiteral("Which subsystem to benchmark: \"cpu\" (compute suite) or \"memory\" "
                        "(bandwidth/latency). Default \"cpu\". The disk benchmark is not exposed "
                        "(it writes a test file).")}};
    return QJsonObject{{QStringLiteral("type"), QStringLiteral("object")},
                       {QStringLiteral("properties"),
                        QJsonObject{{QStringLiteral("target"), target_prop}}},
                       {QStringLiteral("additionalProperties"), false}};
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

QJsonObject scanLeftoversParamsSchema() {
    QJsonObject properties{
        {QStringLiteral("program_name"),
         stringProp(QStringLiteral("Exact display name of the installed program to scan for "
                                   "leftovers (as shown by list_installed_programs)"))},
        {QStringLiteral("advanced"),
         boolProp(QStringLiteral("Also scan system objects (services, scheduled tasks, firewall "
                                 "rules, startup entries) in addition to files + registry. Default "
                                 "false. If a system-object enumeration fails, scan_complete is "
                                 "reported false rather than a dishonest empty result."))}};
    return QJsonObject{{QStringLiteral("type"), QStringLiteral("object")},
                       {QStringLiteral("properties"), properties},
                       {QStringLiteral("required"), QJsonArray{QStringLiteral("program_name")}},
                       {QStringLiteral("additionalProperties"), false}};
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

QJsonObject hashFileParamsSchema() {
    QJsonObject algo_prop{{QStringLiteral("type"), QStringLiteral("string")},
                          {QStringLiteral("enum"),
                           QJsonArray{QStringLiteral("sha256"), QStringLiteral("md5")}},
                          {QStringLiteral("description"),
                           QStringLiteral("Hash algorithm: \"sha256\" (default) or \"md5\"")}};
    QJsonObject properties{{QStringLiteral("path"),
                            stringProp(QStringLiteral("Absolute path to the file to hash"))},
                           {QStringLiteral("algorithm"), algo_prop}};
    return QJsonObject{{QStringLiteral("type"), QStringLiteral("object")},
                       {QStringLiteral("properties"), properties},
                       {QStringLiteral("required"), QJsonArray{QStringLiteral("path")}},
                       {QStringLiteral("additionalProperties"), false}};
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

QJsonObject searchMboxParamsSchema() {
    QJsonObject properties{
        {QStringLiteral("path"), stringProp(QStringLiteral("Absolute path to the MBOX file"))},
        {QStringLiteral("query"), stringProp(QStringLiteral("Text to search for"))},
        {QStringLiteral("case_sensitive"), boolProp(QStringLiteral("Case-sensitive match"))},
        {QStringLiteral("search_subject"),
         boolProp(QStringLiteral("Match the subject (default true)"))},
        {QStringLiteral("search_body"), boolProp(QStringLiteral("Match the body (default true)"))},
        {QStringLiteral("search_sender"),
         boolProp(QStringLiteral("Match the sender (default true)"))}};
    return QJsonObject{{QStringLiteral("type"), QStringLiteral("object")},
                       {QStringLiteral("properties"), properties},
                       {QStringLiteral("required"),
                        QJsonArray{QStringLiteral("path"), QStringLiteral("query")}},
                       {QStringLiteral("additionalProperties"), false}};
}

QJsonObject searchPstParamsSchema() {
    QJsonObject properties{
        {QStringLiteral("path"), stringProp(QStringLiteral("Absolute path to the PST/OST file"))},
        {QStringLiteral("query"), stringProp(QStringLiteral("Text to search for"))},
        {QStringLiteral("case_sensitive"), boolProp(QStringLiteral("Case-sensitive match"))},
        {QStringLiteral("search_subject"),
         boolProp(QStringLiteral("Match the subject (default true)"))},
        {QStringLiteral("search_body"), boolProp(QStringLiteral("Match the body (default true)"))},
        {QStringLiteral("search_sender"),
         boolProp(QStringLiteral("Match the sender (default true)"))}};
    return QJsonObject{{QStringLiteral("type"), QStringLiteral("object")},
                       {QStringLiteral("properties"), properties},
                       {QStringLiteral("required"),
                        QJsonArray{QStringLiteral("path"), QStringLiteral("query")}},
                       {QStringLiteral("additionalProperties"), false}};
}

QJsonObject readPstParamsSchema() {
    QJsonObject properties{
        {QStringLiteral("path"), stringProp(QStringLiteral("Absolute path to the PST/OST file"))},
        {QStringLiteral("folder_node_id"),
         intProp(QStringLiteral("Optional: read this folder's message headers too (use a node_id "
                                "from the returned folder tree)"))},
        {QStringLiteral("offset"),
         intProp(QStringLiteral("First item index within the folder (0-based)"))},
        {QStringLiteral("limit"),
         intProp(QStringLiteral("Max folder items to return (default 200, max 500)"))}};
    return QJsonObject{{QStringLiteral("type"), QStringLiteral("object")},
                       {QStringLiteral("properties"), properties},
                       {QStringLiteral("required"), QJsonArray{QStringLiteral("path")}},
                       {QStringLiteral("additionalProperties"), false}};
}

QJsonObject recoverDeletedParamsSchema() {
    QJsonObject properties{
        {QStringLiteral("path"), stringProp(QStringLiteral("Absolute path to the PST/OST file"))},
        {QStringLiteral("include_orphans"),
         boolProp(QStringLiteral("Also scan the NBT for hard-deleted orphaned message nodes "
                                 "(default true; slower on large files)"))}};
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

QJsonObject isoParamsSchema() {
    QJsonObject path_prop{{QStringLiteral("type"), QStringLiteral("string")},
                          {QStringLiteral("description"),
                           QStringLiteral("Absolute path to the .iso/.img file to analyze")}};
    return QJsonObject{{QStringLiteral("type"), QStringLiteral("object")},
                       {QStringLiteral("properties"),
                        QJsonObject{{QStringLiteral("path"), path_prop}}},
                       {QStringLiteral("required"), QJsonArray{QStringLiteral("path")}},
                       {QStringLiteral("additionalProperties"), false}};
}

QJsonObject scanRecoverableParamsSchema() {
    QJsonObject properties{
        {QStringLiteral("image_path"),
         stringProp(
             QStringLiteral("Absolute path to an offline disk-image FILE to carve for "
                            "recoverable files (a regular file, not a physical device path)"))},
        {QStringLiteral("max_scan_mb"),
         intProp(QStringLiteral("Cap the scan window read from the image, in MiB (1-512, default "
                                "512); may only be lowered"))},
        {QStringLiteral("max_candidates"),
         intProp(QStringLiteral("Cap the number of candidates returned (1-2048, default 2048)"))}};
    return QJsonObject{{QStringLiteral("type"), QStringLiteral("object")},
                       {QStringLiteral("properties"), properties},
                       {QStringLiteral("required"), QJsonArray{QStringLiteral("image_path")}},
                       {QStringLiteral("additionalProperties"), false}};
}

QJsonObject listDrivesParamsSchema() {
    QJsonObject properties{
        {QStringLiteral("removable_only"),
         boolProp(QStringLiteral("Return only removable drives (USB/SD/MMC) -- the drives safe to "
                                 "flash. Default false (all physical drives). removable_count and "
                                 "the total drive_count are reported either way."))}};
    return QJsonObject{{QStringLiteral("type"), QStringLiteral("object")},
                       {QStringLiteral("properties"), properties},
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

QJsonObject dnsParamsSchema() {
    QJsonObject properties{
        {QStringLiteral("hostname"), stringProp(QStringLiteral("Hostname (or IP) to resolve"))},
        {QStringLiteral("record_type"),
         stringProp(QStringLiteral("DNS record type: A/AAAA/CNAME/MX/TXT/NS/... (default A)"))},
        {QStringLiteral("dns_server"),
         stringProp(QStringLiteral("Specific DNS server to query (optional; default system)"))}};
    return QJsonObject{{QStringLiteral("type"), QStringLiteral("object")},
                       {QStringLiteral("properties"), properties},
                       {QStringLiteral("required"), QJsonArray{QStringLiteral("hostname")}},
                       {QStringLiteral("additionalProperties"), false}};
}

QJsonObject firewallParamsSchema() {
    QJsonObject properties{
        {QStringLiteral("name_filter"),
         stringProp(QStringLiteral("Only return rules whose name contains this (substring, "
                                   "case-insensitive); conflicts/gaps still cover all rules"))},
        {QStringLiteral("enabled_only"),
         boolProp(QStringLiteral("Only return enabled rules (default false)"))}};
    return QJsonObject{{QStringLiteral("type"), QStringLiteral("object")},
                       {QStringLiteral("properties"), properties},
                       {QStringLiteral("additionalProperties"), false}};
}

QJsonObject connectionsParamsSchema() {
    QJsonObject properties{
        {QStringLiteral("show_tcp"),
         boolProp(QStringLiteral("Include TCP connections (default true)"))},
        {QStringLiteral("show_udp"),
         boolProp(QStringLiteral("Include UDP listeners (default true)"))},
        {QStringLiteral("filter_process"),
         stringProp(QStringLiteral("Only connections whose process name contains this (substring, "
                                   "case-insensitive)"))},
        {QStringLiteral("filter_port"),
         intProp(
             QStringLiteral("Only connections whose local OR remote port equals this (1-65535)"))}};
    return QJsonObject{{QStringLiteral("type"), QStringLiteral("object")},
                       {QStringLiteral("properties"), properties},
                       {QStringLiteral("additionalProperties"), false}};
}

QJsonObject pingParamsSchema() {
    QJsonObject properties{
        {QStringLiteral("target"), stringProp(QStringLiteral("Hostname or IP to ping"))},
        {QStringLiteral("count"),
         intProp(QStringLiteral("Echo requests to send (1-10, default 4)"))},
        {QStringLiteral("timeout_ms"),
         intProp(QStringLiteral("Per-echo timeout in ms (200-4000, default 2000)"))},
        {QStringLiteral("interval_ms"),
         intProp(QStringLiteral("Delay between echoes in ms (0-2000, default 500)"))},
        {QStringLiteral("resolve_hostnames"),
         boolProp(QStringLiteral("Reverse-resolve reply addresses"))}};
    return QJsonObject{{QStringLiteral("type"), QStringLiteral("object")},
                       {QStringLiteral("properties"), properties},
                       {QStringLiteral("required"), QJsonArray{QStringLiteral("target")}},
                       {QStringLiteral("additionalProperties"), false}};
}

QJsonObject tracerouteParamsSchema() {
    QJsonObject properties{
        {QStringLiteral("target"), stringProp(QStringLiteral("Hostname or IP to trace"))},
        {QStringLiteral("max_hops"),
         intProp(QStringLiteral("Max hops to probe (1-30, default 30)"))},
        {QStringLiteral("timeout_ms"),
         intProp(QStringLiteral("Per-probe timeout in ms (500-3000, default 2000)"))},
        {QStringLiteral("probes_per_hop"),
         intProp(QStringLiteral("Probes per hop (1-3, default 3)"))},
        {QStringLiteral("resolve_hostnames"),
         boolProp(QStringLiteral("Reverse-resolve hop addresses"))}};
    return QJsonObject{{QStringLiteral("type"), QStringLiteral("object")},
                       {QStringLiteral("properties"), properties},
                       {QStringLiteral("required"), QJsonArray{QStringLiteral("target")}},
                       {QStringLiteral("additionalProperties"), false}};
}

QJsonObject mtrParamsSchema() {
    QJsonObject properties{
        {QStringLiteral("target"),
         stringProp(QStringLiteral("Hostname or IP to trace continuously"))},
        {QStringLiteral("cycles"),
         intProp(QStringLiteral("Ping+trace cycles to run (1-10, default 3)"))},
        {QStringLiteral("max_hops"),
         intProp(QStringLiteral("Max hops to probe (1-30, default 30)"))},
        {QStringLiteral("timeout_ms"),
         intProp(QStringLiteral("Per-probe timeout in ms (500-1500, default 1000)"))},
        {QStringLiteral("interval_ms"),
         intProp(QStringLiteral("Delay between cycles in ms (0-2000, default 500)"))}};
    return QJsonObject{{QStringLiteral("type"), QStringLiteral("object")},
                       {QStringLiteral("properties"), properties},
                       {QStringLiteral("required"), QJsonArray{QStringLiteral("target")}},
                       {QStringLiteral("additionalProperties"), false}};
}

QJsonObject portScanParamsSchema() {
    QJsonObject ports_prop{
        {QStringLiteral("type"), QStringLiteral("array")},
        {QStringLiteral("items"), QJsonObject{{QStringLiteral("type"), QStringLiteral("integer")}}},
        {QStringLiteral("description"), QStringLiteral("Explicit ports to scan (1-65535)")}};
    QJsonObject properties{
        {QStringLiteral("target"), stringProp(QStringLiteral("Hostname or IP to scan"))},
        {QStringLiteral("ports"), ports_prop},
        {QStringLiteral("port_range_start"),
         intProp(QStringLiteral("Start of an inclusive port range (with port_range_end)"))},
        {QStringLiteral("port_range_end"),
         intProp(QStringLiteral("End of an inclusive port range"))},
        {QStringLiteral("timeout_ms"),
         intProp(QStringLiteral("Per-port connect timeout in ms (200-3000, default 1000)"))},
        {QStringLiteral("grab_banners"),
         boolProp(QStringLiteral("Read a service banner from open ports"))}};
    return QJsonObject{{QStringLiteral("type"), QStringLiteral("object")},
                       {QStringLiteral("properties"), properties},
                       {QStringLiteral("required"), QJsonArray{QStringLiteral("target")}},
                       {QStringLiteral("additionalProperties"), false}};
}

// Defined just below; declared here because registerNetworkReadOnlyOps tail-calls it.
void registerNetworkProbeReadOnlyOps(const AddActionFn& add);

// Register the read-only network ops (adapter enumeration, DNS lookup, and the active
// ping/traceroute/port_scan probes). Split out to keep registerReadOnlyAppActionsInto
// within the length budget as the op set grows.
void registerNetworkReadOnlyOps(const AddActionFn& add) {
    add(makeDescriptor(QStringLiteral("network.list_adapters"),
                       QStringLiteral("List network adapters"),
                       QStringLiteral("Enumerate network adapters with IP/DNS/DHCP/link details"),
                       QStringLiteral("network"),
                       noParamsSchema()),
        listAdapters);

    add(makeDescriptor(QStringLiteral("network.dns_query"),
                       QStringLiteral("DNS lookup"),
                       QStringLiteral("Resolve a hostname (A/AAAA/MX/TXT/...) via a DNS query"),
                       QStringLiteral("network"),
                       dnsParamsSchema()),
        dnsQuery);

    add(makeDescriptor(QStringLiteral("network.list_connections"),
                       QStringLiteral("List active connections"),
                       QStringLiteral("Enumerate active TCP connections and UDP listeners with "
                                      "owning process (PID/name/path) and state"),
                       QStringLiteral("network"),
                       connectionsParamsSchema()),
        listConnections);

    add(makeDescriptor(QStringLiteral("network.audit_firewall"),
                       QStringLiteral("Audit firewall rules"),
                       QStringLiteral("Enumerate Windows Firewall rules and report rule conflicts "
                                      "and coverage gaps"),
                       QStringLiteral("network"),
                       firewallParamsSchema()),
        auditFirewall);

    add(makeDescriptor(QStringLiteral("network.wifi_scan"),
                       QStringLiteral("Scan WiFi networks"),
                       QStringLiteral("Scan for nearby WiFi networks with signal/channel/security "
                                      "details and channel-utilization analysis"),
                       QStringLiteral("network"),
                       noParamsSchema()),
        wifiScan);

    add(makeDescriptor(QStringLiteral("network.list_wifi_profiles"),
                       QStringLiteral("List saved WiFi profiles"),
                       QStringLiteral(
                           "Enumerate the machine's saved WiFi profiles (name + security "
                           "type); read-only, no key material exposed"),
                       QStringLiteral("network"),
                       noParamsSchema()),
        listWifiProfiles);

    add(makeDescriptor(
            QStringLiteral("network.generate_wifi_setup_script"),
            QStringLiteral("Generate a WiFi setup script"),
            QStringLiteral(
                "Build a Windows .cmd that adds a WLAN profile and connects to a network "
                "(returns the script text; does NOT run it or change any settings)"),
            QStringLiteral("network"),
            generateWifiSetupScriptParamsSchema()),
        generateWifiSetupScript);

    add(makeDescriptor(QStringLiteral("network.list_shares"),
                       QStringLiteral("List file shares"),
                       QStringLiteral("Enumerate SMB/Windows file shares on the local machine; "
                                      "read-only, no access probe (local only)"),
                       QStringLiteral("network"),
                       noParamsSchema()),
        listShares);

    registerNetworkProbeReadOnlyOps(add);
}

// Register the active network probes (ping/traceroute/mtr/port_scan). Split out of
// registerNetworkReadOnlyOps to keep each within the length budget.
void registerNetworkProbeReadOnlyOps(const AddActionFn& add) {
    add(makeDescriptor(QStringLiteral("network.ping"),
                       QStringLiteral("Ping a host"),
                       QStringLiteral("Send ICMP echo requests and report loss/latency/jitter"),
                       QStringLiteral("network"),
                       pingParamsSchema()),
        pingHost);

    add(makeDescriptor(QStringLiteral("network.traceroute"),
                       QStringLiteral("Traceroute to a host"),
                       QStringLiteral("Trace the network path to a host hop by hop (ICMP TTL)"),
                       QStringLiteral("network"),
                       tracerouteParamsSchema()),
        tracerouteHost);

    add(makeDescriptor(QStringLiteral("network.mtr"),
                       QStringLiteral("MTR (my traceroute)"),
                       QStringLiteral("Continuous ping+traceroute: per-hop loss/latency over "
                                      "several cycles"),
                       QStringLiteral("network"),
                       mtrParamsSchema()),
        mtrHost);

    add(makeDescriptor(QStringLiteral("network.port_scan"),
                       QStringLiteral("Scan TCP ports"),
                       QStringLiteral(
                           "TCP-connect scan a host's ports with service/banner detection"),
                       QStringLiteral("network"),
                       portScanParamsSchema()),
        portScan);
}

// Register the read-only diagnostics ops (hardware inventory, SMART health, restore points).
// Split out to keep registerReadOnlyAppActionsInto within the length budget as the set grows.
void registerDiagnosticsReadOnlyOps(const AddActionFn& add) {
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

    add(makeDescriptor(QStringLiteral("diagnostics.list_restore_points"),
                       QStringLiteral("List system restore points"),
                       QStringLiteral("List existing Windows System Restore points and whether "
                                      "System Restore is enabled"),
                       QStringLiteral("diagnostics"),
                       noParamsSchema()),
        listRestorePoints);

    add(makeDescriptor(QStringLiteral("diagnostics.read_temperatures"),
                       QStringLiteral("Read temperatures"),
                       QStringLiteral("Read current CPU/GPU/disk temperatures from system thermal "
                                      "sensors (some need admin)"),
                       QStringLiteral("diagnostics"),
                       noParamsSchema()),
        readTemperatures);

    add(makeDescriptor(QStringLiteral("diagnostics.run_benchmark"),
                       QStringLiteral("Run a performance benchmark"),
                       QStringLiteral("Run a CPU or memory performance benchmark suite and report "
                                      "normalized scores plus throughput/latency (read-only; "
                                      "consumes CPU/RAM transiently, no disk writes)"),
                       QStringLiteral("diagnostics"),
                       benchmarkParamsSchema()),
        benchmarkSystem);
}

}  // namespace

// Register the read-only email ops (MBOX + PST/OST readers). Split out to keep
// registerReadOnlyAppActionsInto within the length budget.
void registerEmailReadOnlyOps(const AddActionFn& add) {
    add(makeDescriptor(QStringLiteral("email.read_mbox"),
                       QStringLiteral("Read an MBOX mailbox"),
                       QStringLiteral("List messages (headers) from an MBOX email file"),
                       QStringLiteral("email"),
                       readMboxParamsSchema()),
        readMbox);

    add(makeDescriptor(QStringLiteral("email.search_mbox"),
                       QStringLiteral("Search an MBOX mailbox"),
                       QStringLiteral(
                           "Full-text search an MBOX file for messages matching a query "
                           "(subject/body/sender); returns per-hit metadata and a context "
                           "snippet around the match (read-only, never the full body)"),
                       QStringLiteral("email"),
                       searchMboxParamsSchema()),
        searchMbox);

    add(makeDescriptor(QStringLiteral("email.read_pst"),
                       QStringLiteral("Read a PST/OST mailbox"),
                       QStringLiteral("List the folder tree (and optionally one folder's message "
                                      "headers) from an Outlook PST/OST file"),
                       QStringLiteral("email"),
                       readPstParamsSchema()),
        readPst);

    add(makeDescriptor(QStringLiteral("email.search_pst"),
                       QStringLiteral("Search a PST/OST mailbox"),
                       QStringLiteral(
                           "Full-text search an Outlook PST/OST file for messages matching "
                           "a query (subject/body/sender); returns per-hit metadata and a "
                           "context snippet around the match (read-only, never the full "
                           "body)"),
                       QStringLiteral("email"),
                       searchPstParamsSchema()),
        searchPst);

    add(makeDescriptor(QStringLiteral("email.recover_deleted"),
                       QStringLiteral("Recover deleted PST/OST items"),
                       QStringLiteral(
                           "Inventory recoverable soft-deleted and orphaned hard-deleted "
                           "message items in an Outlook PST/OST (read-only, metadata "
                           "only -- lists what can be recovered)"),
                       QStringLiteral("email"),
                       recoverDeletedParamsSchema()),
        recoverDeleted);

    add(makeDescriptor(QStringLiteral("email.list_profiles"),
                       QStringLiteral("List email client profiles"),
                       QStringLiteral(
                           "Enumerate installed email clients (Outlook/Thunderbird/Windows Mail) "
                           "and their profiles + linked data-file paths (read-only, metadata "
                           "only -- no mailbox contents)"),
                       QStringLiteral("email"),
                       noParamsSchema()),
        listEmailProfiles);
}

// Register the read-only file-content ops (text search + duplicate finder). Split out to keep
// registerReadOnlyAppActionsInto within the length budget.
// Register the imaging read-only ops. Split out to keep registerReadOnlyAppActionsInto within the
// length budget as the set grows.
void registerImagingReadOnlyOps(const AddActionFn& add) {
    add(makeDescriptor(QStringLiteral("imaging.identify_image"),
                       QStringLiteral("Identify a disk image"),
                       QStringLiteral(
                           "Detect a disk-image file's format/compression by file extension"),
                       QStringLiteral("imaging"),
                       identifyParamsSchema()),
        identifyImage);

    add(makeDescriptor(QStringLiteral("imaging.analyze_iso"),
                       QStringLiteral("Analyze an ISO image"),
                       QStringLiteral("Read an ISO/IMG file's volume metadata, boot type, and "
                                      "OS/distro identity from its on-disk descriptors"),
                       QStringLiteral("imaging"),
                       isoParamsSchema()),
        analyzeIso);

    add(makeDescriptor(QStringLiteral("imaging.scan_recoverable"),
                       QStringLiteral("Scan an image for recoverable files"),
                       QStringLiteral("Signature-carve an offline disk-image file for recoverable "
                                      "(deleted) files and report their metadata (format, offset, "
                                      "size, sha256); read-only, never returns file content"),
                       QStringLiteral("imaging"),
                       scanRecoverableParamsSchema()),
        scanRecoverable);

    add(makeDescriptor(QStringLiteral("imaging.list_drives"),
                       QStringLiteral("List physical drives"),
                       QStringLiteral(
                           "Enumerate the machine's physical drives (the Image Flasher's "
                           "target picker): path, model, size, bus type, mount points, "
                           "and the removable/write-protected/contains-Windows flags. "
                           "Read-only (metadata IOCTLs only, never reads drive data)"),
                       QStringLiteral("imaging"),
                       listDrivesParamsSchema()),
        listDrives);
}

void registerFileReadOnlyOps(const AddActionFn& add) {
    add(makeDescriptor(QStringLiteral("search.find_in_files"),
                       QStringLiteral("Search files"),
                       QStringLiteral("Search a directory tree for text/regex matches in files"),
                       QStringLiteral("search"),
                       searchParamsSchema()),
        searchFiles);

    add(makeDescriptor(QStringLiteral("organizer.find_duplicates"),
                       QStringLiteral("Find duplicate files"),
                       QStringLiteral("Scan directories for duplicate files (by content hash) and "
                                      "report the groups and reclaimable space"),
                       QStringLiteral("organizer"),
                       dupParamsSchema()),
        findDuplicates);

    add(makeDescriptor(QStringLiteral("organizer.preview_organize"),
                       QStringLiteral("Preview organizing a directory"),
                       QStringLiteral("Dry run of organizer.organize_directory: report which files "
                                      "WOULD move into which category subfolder (per-category "
                                      "counts, collisions, a per-move sample) without moving any"),
                       QStringLiteral("organizer"),
                       previewOrganizeParamsSchema()),
        previewOrganize);

    add(makeDescriptor(QStringLiteral("files.list_archive"),
                       QStringLiteral("List a .zip archive"),
                       QStringLiteral("List a .zip archive's entries (paths, sizes, dir flags) "
                                      "without extracting; read-only"),
                       QStringLiteral("files"),
                       listArchiveParamsSchema()),
        listArchive);

    add(makeDescriptor(QStringLiteral("files.scan_directory"),
                       QStringLiteral("Scan a directory"),
                       QStringLiteral("List a directory tree's entries with sizes and report "
                                      "disk-usage totals (files/dirs/bytes); read-only"),
                       QStringLiteral("files"),
                       scanDirectoryParamsSchema()),
        scanDirectory);

    add(makeDescriptor(QStringLiteral("files.hash_file"),
                       QStringLiteral("Hash a file"),
                       QStringLiteral("Compute a file's SHA-256 (default) or MD5 checksum for "
                                      "integrity verification; read-only, streamed under a time "
                                      "limit"),
                       QStringLiteral("files"),
                       hashFileParamsSchema()),
        hashFile);
}

int registerReadOnlyAppActionsInto(AppActionRegistry& registry) {
    int registered = 0;
    const auto add = [&](const AppActionDescriptor& descriptor, AppActionInvoke invoke) {
        if (registry.registerAction(descriptor, std::move(invoke))) {
            ++registered;
        }
    };

    registerPartitionReadOnlyOps(add);
    registerNetworkReadOnlyOps(add);

    add(makeDescriptor(QStringLiteral("security.list_installed_programs"),
                       QStringLiteral("List installed programs"),
                       QStringLiteral("Enumerate installed programs (registry + store apps)"),
                       QStringLiteral("security"),
                       noParamsSchema()),
        listInstalledPrograms);

    add(makeDescriptor(QStringLiteral("software.scan_leftovers"),
                       QStringLiteral("Scan for a program's leftovers"),
                       QStringLiteral("Preview the orphaned files/folders/registry entries a "
                                      "program's uninstall would leave behind (read-only, deletes "
                                      "nothing); classifies each by risk"),
                       QStringLiteral("software"),
                       scanLeftoversParamsSchema()),
        scanLeftovers);

    add(makeDescriptor(QStringLiteral("security.scan_vulnerabilities"),
                       QStringLiteral("Scan for vulnerabilities"),
                       QStringLiteral(
                           "Scan installed programs against CVE/advisory feeds (network, slower)"),
                       QStringLiteral("security"),
                       scanParamsSchema()),
        scanVulnerabilities);

    registerImagingReadOnlyOps(add);

    registerFileReadOnlyOps(add);

    add(makeDescriptor(QStringLiteral("backup.preview_app_data"),
                       QStringLiteral("Preview an app's backup data"),
                       QStringLiteral("Report the standard user-data directories a named app would "
                                      "back up and their total on-disk size, without copying "
                                      "anything (read-only)"),
                       QStringLiteral("backup"),
                       previewAppDataParamsSchema()),
        previewAppDataBackup);

    registerDiagnosticsReadOnlyOps(add);

    add(makeDescriptor(QStringLiteral("system.list_users"),
                       QStringLiteral("List user accounts"),
                       QStringLiteral("List local Windows user accounts that have a profile "
                                      "directory (detailed info may need admin)"),
                       QStringLiteral("system"),
                       noParamsSchema()),
        listUsers);

    registerEmailReadOnlyOps(add);

    return registered;
}

}  // namespace sak
