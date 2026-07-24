// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

// Read-only technician ops exposed to the AI assistant. Each invoke thunk calls
// an existing headless src/core service (no re-implementation) and serializes its
// result to a compact, model-facing QJsonObject. All are synchronous and run on
// the caller's worker thread -- no event loop, no GUI, no controller lifetime.

#include "sak/app_readonly_actions.h"

#include "sak/advanced_search_types.h"
#include "sak/advanced_search_worker.h"
#include "sak/app_action_registry.h"
#include "sak/app_action_service.h"
#include "sak/image_source.h"
#include "sak/partition_manager_types.h"
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
#include <memory>

namespace sak {

namespace {

// Bound model-facing list payloads so a machine with hundreds of programs or a
// large scan does not blow the tool-result size. Truncation is reported, never
// silent.
constexpr int kMaxListedPrograms = 500;
constexpr int kMaxReportedFindings = 200;
constexpr int kMaxInventoryWarnings = 200;
constexpr int kMaxReportedMatches = 500;
constexpr int kMaxMatchLineChars = 400;
constexpr int kDefaultSearchMaxResults = 1000;
constexpr int kSearchMaxResultsCeiling = 5000;

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

bool isNetworkOrDevicePath(const QString& path) {
    // Reject UNC (\\server\share) and Win32 device namespaces (\\.\, \\?\). An
    // ungated read-only op must not be steerable (prompt injection) into opening an
    // SMB connection to an attacker host -- QFileInfo::exists() alone would trigger
    // NTLM auth and leak the user's hash. Windows treats ANY two leading separators
    // as a UNC/device root, including mixed forms (\/, /\), and Qt normalizes '/' to
    // '\\', so reject any pair of leading separators regardless of type.
    const auto isSeparator = [](QChar ch) {
        return ch == QLatin1Char('\\') || ch == QLatin1Char('/');
    };
    return path.size() >= 2 && isSeparator(path.at(0)) && isSeparator(path.at(1));
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

}  // namespace

int registerReadOnlyAppActionsInto(AppActionRegistry& registry) {
    int registered = 0;
    const auto add = [&](const AppActionDescriptor& descriptor, AppActionInvoke invoke) {
        if (registry.registerAction(descriptor, std::move(invoke))) {
            ++registered;
        }
    };

    add(makeDescriptor(QStringLiteral("partition.list_inventory"),
                       QStringLiteral("List storage inventory"),
                       QStringLiteral("Enumerate disks, partitions, and volumes on this system"),
                       QStringLiteral("partition"),
                       noParamsSchema()),
        listInventory);

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

    return registered;
}

}  // namespace sak
