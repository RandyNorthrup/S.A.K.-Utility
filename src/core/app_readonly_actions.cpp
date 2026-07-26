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
#include "sak/app_action_guards.h"
#include "sak/app_action_registry.h"
#include "sak/app_action_service.h"
#include "sak/app_partition_op_parse.h"
#include "sak/connectivity_tester.h"
#include "sak/diagnostic_types.h"
#include "sak/dns_diagnostic_tool.h"
#include "sak/error_codes.h"
#include "sak/firewall_rule_auditor.h"
#include "sak/hardware_inventory_scanner.h"
#include "sak/image_source.h"
#include "sak/mbox_parser.h"
#include "sak/network_adapter_inspector.h"
#include "sak/network_diagnostic_types.h"
#include "sak/network_probe_worker.h"
#include "sak/partition_manager_types.h"
#include "sak/partition_operation_planner.h"
#include "sak/port_scanner.h"
#include "sak/smart_disk_analyzer.h"
#include "sak/storage_inventory_worker.h"
#include "sak/vulnerability_scanner.h"
#include "sak/wifi_analyzer.h"
#include "sak/worker_base.h"

#include <QFileInfo>
#include <QHostAddress>
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
constexpr int kMaxAdapters = 100;
constexpr int kMaxConnections = 500;
constexpr int kMaxFirewallRules = 600;
constexpr int kMaxFirewallConflicts = 200;
constexpr int kMaxFirewallGaps = 100;
constexpr int kMaxDnsAnswers = 100;
constexpr int kMaxWifiNetworks = 200;
constexpr int kMaxWifiChannels = 100;
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

}  // namespace

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
