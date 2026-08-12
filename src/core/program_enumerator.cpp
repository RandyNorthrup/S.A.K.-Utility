// Copyright (c) 2025-2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file program_enumerator.cpp
/// @brief Enumerates all installed Win32 and UWP programs with rich metadata

#include "sak/program_enumerator.h"

#include "sak/layout_constants.h"
#include "sak/process_runner.h"

#include <QDir>
#include <QDirIterator>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>
#include <QTimer>

#include <limits>

#ifdef Q_OS_WIN
#include <shellapi.h>
#pragma comment(lib, "Shell32.lib")
#pragma comment(lib, "Advapi32.lib")
#endif

namespace sak {

namespace {
constexpr int kRegistryScanProgress = 50;
constexpr int kUwpScanProgress = 75;
constexpr int kProvisionedScanProgress = 85;
constexpr int kMetadataScanProgress = 95;
constexpr int kQuotedPathTrimChars = 2;

[[nodiscard]] QString normalizeUninstallExePath(QString uninstallString) {
    QString exePath = std::move(uninstallString);

    if (exePath.startsWith('"')) {
        const int endQuote = static_cast<int>(exePath.indexOf('"', 1));
        if (endQuote > 0) {
            exePath = exePath.mid(1, endQuote - 1);
        }
        return exePath;
    }

    const int space = static_cast<int>(exePath.indexOf(' '));
    if (space > 0) {
        exePath = exePath.left(space);
    }
    return exePath;
}

// A registry-supplied program path is untrusted (HKLM needs admin, but HKCU is user-writable).
// Reject UNC and Win32 device/extended namespaces (\\server\share, \\.\dev, \\?\...) before
// touching the filesystem: walking or stat-ing such a path can trigger SMB authentication as the
// (often elevated) caller and unbounded remote I/O, letting an attacker-writable Uninstall value
// coerce network access. Ordinary local install paths begin with a drive letter, so this only
// skips remote/device targets -- their size/icon/existence are simply not probed.
[[nodiscard]] bool isRemoteOrDevicePath(const QString& path) {
    return path.startsWith(QLatin1String("\\\\")) || path.startsWith(QLatin1String("//"));
}

[[nodiscard]] QJsonArray jsonDocToArray(const QJsonDocument& doc) {
    if (doc.isArray()) {
        return doc.array();
    }
    if (doc.isObject()) {
        return QJsonArray{doc.object()};
    }
    return {};
}

const auto kUwpPackagesCommand = QStringLiteral(
    // -ErrorAction Stop: a per-user package-enumeration error must FAIL the scan (non-zero exit)
    // rather than emit valid JSON for only the packages that did enumerate. Reporting that partial
    // set as complete is a fail-open; a failed scan instead warns (enumerateAll) or safely refuses
    // a headless match (enumerateUwpPackagesSync).
    "Get-AppxPackage -ErrorAction Stop | Select-Object Name, PackageFamilyName, "
    "PackageFullName, Publisher, Version, InstallLocation, "
    "IsFramework, SignatureKind | ConvertTo-Json -Compress");
}  // namespace

ProgramEnumerator::ProgramEnumerator(QObject* parent) : QObject(parent) {}

ProgramEnumerator::~ProgramEnumerator() = default;

void ProgramEnumerator::requestCancel() {
    m_cancelRequested.store(true, std::memory_order_release);
}

void ProgramEnumerator::resetCancel() {
    m_cancelRequested.store(false, std::memory_order_release);
}

void ProgramEnumerator::enumerateAll(int generation) {
    m_generation = generation;  // stamped on every terminal signal from this run
    if (m_cancelRequested.load(std::memory_order_acquire)) {
        Q_EMIT enumerationFailed(m_generation, "Enumeration cancelled.");
        return;
    }
    Q_EMIT enumerationStarted();

    QVector<ProgramInfo> all_programs;

    try {
#ifdef Q_OS_WIN
        // Phase 1: Win32 registry programs
        auto registry_programs = scanRegistryPrograms();
        if (m_cancelRequested.load(std::memory_order_acquire)) {
            Q_EMIT enumerationFailed(m_generation, "Enumeration cancelled.");
            return;
        }
        all_programs.append(registry_programs);
        Q_EMIT enumerationProgress(kRegistryScanProgress, kPercentMax);
#endif

        // Phase 2: UWP packages
        bool uwpOk = true;
        auto uwp_programs = scanUwpPackages(uwpOk);
        if (m_cancelRequested.load(std::memory_order_acquire)) {
            Q_EMIT enumerationFailed(m_generation, "Enumeration cancelled.");
            return;
        }
        all_programs.append(uwp_programs);
        Q_EMIT enumerationProgress(kUwpScanProgress, kPercentMax);

        // Phase 3: Provisioned UWP packages
        bool provisionedOk = true;
        auto provisioned = scanProvisionedPackages(provisionedOk);
        if (m_cancelRequested.load(std::memory_order_acquire)) {
            Q_EMIT enumerationFailed(m_generation, "Enumeration cancelled.");
            return;
        }
        all_programs.append(provisioned);
        Q_EMIT enumerationProgress(kProvisionedScanProgress, kPercentMax);
        warnIfAppxIncomplete(uwpOk, provisionedOk);

        // Phase 4: Deduplicate
        deduplicatePrograms(all_programs);

        // Phase 5: Detect orphaned entries
        detectOrphaned(all_programs);

        // Phase 6: Mark bloatware
        markBloatware(all_programs);
        Q_EMIT enumerationProgress(kMetadataScanProgress, kPercentMax);

        // Phase 7: Extract icons and calculate sizes
        if (!enrichWithIconsAndSizes(all_programs)) {
            return;
        }

        Q_EMIT enumerationProgress(kPercentMax, kPercentMax);
        m_cachedPrograms = all_programs;
        Q_EMIT enumerationFinished(m_generation, all_programs);

    } catch (const std::exception& e) {
        Q_EMIT enumerationFailed(m_generation, QString("Enumeration error: %1").arg(e.what()));
    }
}

QVector<ProgramInfo> ProgramEnumerator::enumerateUwpPackagesSync(bool& scanOk) {
    // Reuse the exact Appx scanners enumerateAll uses -- just the UWP phases, deduped, with no
    // registry/orphan/bloatware/icon/dir-size work.
    //
    // scanOk reflects only the PER-USER scan (Get-AppxPackage, no admin) -- the authoritative
    // list for headless resolution. The provisioned scan (Get-AppxProvisionedPackage -Online)
    // needs admin and is best-effort: its failure merely OMITS all-users entries from the list,
    // which -- because resolution is an EXACT display-name match -- can only cause a safe
    // not-found refusal, never a wrong match. So a provisioned failure does not fail the scan.
    bool uwp_ok = true;
    [[maybe_unused]] bool provisioned_ok = true;
    QVector<ProgramInfo> packages = scanUwpPackages(uwp_ok);
    packages.append(scanProvisionedPackages(provisioned_ok));
    // Guard the dedup: unlike enumerateAll (which always has registry entries by here),
    // BOTH Appx scans can legitimately be empty (a scan failure -- the fail-closed case that
    // the caller handles via scanOk) and deduplicatePrograms asserts on an empty vector.
    if (!packages.isEmpty()) {
        deduplicatePrograms(packages);
    }
    scanOk = uwp_ok;
    return packages;
}

QVector<ProgramInfo> ProgramEnumerator::enumerateRegistryProgramsSync() {
#ifdef Q_OS_WIN
    // Reuse the exact registry scanner enumerateAll uses -- just the Win32 phase, with no
    // UWP/orphan/bloatware/icon/dir-size work. Intentionally NOT deduped: the dedup key
    // (displayName|publisher) can merge two genuinely-distinct same-name programs, which would
    // hide an ambiguous match from a headless resolver. Callers that need the authoritative
    // single match must disambiguate themselves (e.g. by the actual uninstall command). The
    // registry read has no external process to fail (unlike the Appx scans), so no scanOk here.
    return scanRegistryPrograms();
#else
    return {};
#endif
}

void ProgramEnumerator::warnIfAppxIncomplete(bool uwpOk, bool provisionedOk) {
    // Fail closed: a PowerShell failure/timeout/parse error in either Appx scan means the list
    // may be incomplete. Warn (never silently claim completeness) but keep the registry list.
    if (!uwpOk || !provisionedOk) {
        Q_EMIT enumerationWarning(
            QStringLiteral("Some app packages could not be enumerated; "
                           "the installed-programs list may be incomplete."));
    }
}

QVector<ProgramInfo> ProgramEnumerator::programs() const {
    return m_cachedPrograms;
}

void ProgramEnumerator::resolveProgramIcon(ProgramInfo& program, bool locationIsLocal) {
    if (!program.displayIcon.isEmpty()) {
        program.cachedImage = extractIcon(program.displayIcon);
    }

    // No usable displayIcon: fall back to the first .exe in a LOCAL install dir only
    // (locationIsLocal is already false for the remote/device paths we must never scan).
    if (program.cachedImage.isNull() && locationIsLocal) {
        const QDir dir(program.installLocation);
        const auto exes = dir.entryList({"*.exe"}, QDir::Files, QDir::Name);
        const QString exe = exes.value(0);
        program.cachedImage = exe.isEmpty() ? QImage{} : extractIcon(dir.filePath(exe));
    }
}

bool ProgramEnumerator::enrichWithIconsAndSizes(QVector<ProgramInfo>& programs) {
    // An empty program list is a legitimate enumeration result (locked-down
    // machine, or all scans returned nothing); the loop below handles it. Do NOT
    // assert non-empty -- that aborts a debug build on a valid empty scan.
    for (int i = 0; i < programs.size(); ++i) {
        if (m_cancelRequested.load(std::memory_order_acquire)) {
            Q_EMIT enumerationFailed(m_generation, "Enumeration cancelled.");
            return false;
        }
        auto& prog = programs[i];

        // A remote/device installLocation must never be scanned or stat-ed (SMB auth / remote I/O
        // from an attacker-writable value); fall back to the estimated size instead.
        const bool locationIsLocal = !prog.installLocation.isEmpty() &&
                                     !isRemoteOrDevicePath(prog.installLocation);

        resolveProgramIcon(prog, locationIsLocal);

        if (locationIsLocal && QDir(prog.installLocation).exists()) {
            prog.actualSizeBytes = calculateDirSize(prog.installLocation);
        } else if (prog.estimatedSizeKB > 0) {
            prog.actualSizeBytes = prog.estimatedSizeKB * kBytesPerKB;
        }
    }
    return true;
}

void ProgramEnumerator::detectOrphaned(QVector<ProgramInfo>& programs) {
    // Empty is valid (see enrichWithIconsAndSizes) -- no non-empty assert.
    for (auto& prog : programs) {
        if (prog.source == ProgramInfo::Source::UWP ||
            prog.source == ProgramInfo::Source::Provisioned) {
            continue;  // UWP apps are always "installed"
        }

        // Never probe a remote/device install path for existence: that stat would trigger SMB
        // auth / remote I/O as the elevated caller from an attacker-writable Uninstall value. An
        // unverifiable location is left un-orphaned rather than driving network access.
        const bool installMissing = !prog.installLocation.isEmpty() &&
                                    !isRemoteOrDevicePath(prog.installLocation) &&
                                    !QDir(prog.installLocation).exists();
        if (installMissing) {
            prog.isOrphaned = true;
            continue;
        }

        const QString exePath = normalizeUninstallExePath(prog.uninstallString);
        if (exePath.isEmpty()) {
            prog.isOrphaned = false;
            continue;
        }

        if (exePath.contains("msiexec", Qt::CaseInsensitive)) {
            prog.isOrphaned = false;
            continue;
        }

        if (isRemoteOrDevicePath(exePath)) {
            // Cannot verify a remote/device target without remote I/O; do not claim it orphaned.
            prog.isOrphaned = false;
            continue;
        }

        prog.isOrphaned = !QFileInfo::exists(exePath);
    }
}

void ProgramEnumerator::markBloatware(QVector<ProgramInfo>& programs) {
    // Empty is valid (see enrichWithIconsAndSizes) -- no non-empty assert.
    // Bloatware patterns from CheckBloatwareAction database
    static const QStringList kBloatwarePatterns = {"CandyCrush",
                                                   "FarmVille",
                                                   "BubbleWitch",
                                                   "MarchofEmpires",
                                                   "Minecraft",
                                                   "Solitaire",
                                                   "Xbox",
                                                   "Zune",
                                                   "BingNews",
                                                   "BingWeather",
                                                   "BingSports",
                                                   "BingFinance",
                                                   "SkypeApp",
                                                   "YourPhone",
                                                   "PhoneLink",
                                                   "Messaging",
                                                   "GetHelp",
                                                   "Getstarted",
                                                   "MicrosoftOfficeHub",
                                                   "WindowsMaps",
                                                   "WindowsAlarms",
                                                   "WindowsSoundRecorder",
                                                   "WindowsFeedbackHub",
                                                   "Wallet",
                                                   "Microsoft3DViewer",
                                                   "Print3D",
                                                   "MixedReality",
                                                   "People",
                                                   "OneConnect",
                                                   "ActiproSoftware",
                                                   "king.com",
                                                   "Facebook",
                                                   "Twitter",
                                                   "LinkedIn",
                                                   "Netflix",
                                                   "Spotify",
                                                   "Disney"};

    for (auto& prog : programs) {
        const QString name_lower = prog.displayName.toLower();
        const QString pkg_lower = prog.packageFamilyName.toLower();

        for (const auto& pattern : kBloatwarePatterns) {
            const QString pat_lower = pattern.toLower();
            if (name_lower.contains(pat_lower) || pkg_lower.contains(pat_lower)) {
                prog.isBloatware = true;
                break;
            }
        }
    }
}

qint64 ProgramEnumerator::calculateDirSize(const QString& path) {
    if (path.isEmpty()) {
        // Fail closed: an empty path must size NOTHING. QDirIterator("") walks the CURRENT working
        // directory, so without this a program with an empty installLocation would report the CWD's
        // size (and, in the test harness, walk the whole build tree). The old Q_ASSERT was a no-op
        // in Release.
        return 0;
    }
    if (isRemoteOrDevicePath(path)) {
        // Never walk a UNC/device tree: it would trigger SMB auth / unbounded remote I/O as the
        // (often elevated) caller from an attacker-writable Uninstall value.
        return 0;
    }
    qint64 total = 0;
    QDirIterator it(path,
                    QDir::Files | QDir::Hidden | QDir::NoSymLinks,
                    QDirIterator::Subdirectories);
    while (it.hasNext()) {
        if (m_cancelRequested.load(std::memory_order_acquire)) {
            break;  // honor cancellation instead of walking an arbitrarily large/deep tree
        }
        it.next();
        const qint64 sz = it.fileInfo().size();
        // Saturate rather than let the signed accumulation overflow (undefined behavior): a
        // crafted tree of large sparse files could otherwise sum past INT64_MAX.
        if (sz > 0 && total > std::numeric_limits<qint64>::max() - sz) {
            total = std::numeric_limits<qint64>::max();
            break;
        }
        total += sz;
    }
    return total;
}

#ifdef Q_OS_WIN

QVector<ProgramInfo> ProgramEnumerator::scanRegistryPrograms() {
    QVector<ProgramInfo> all;

    // HKLM 64-bit
    auto hklm64 =
        scanRegistryHive(HKEY_LOCAL_MACHINE, kUninstallKey64, ProgramInfo::Source::RegistryHKLM);
    all.append(hklm64);

    // HKLM WOW64 (32-bit apps on 64-bit Windows)
    auto wow64 = scanRegistryHive(HKEY_LOCAL_MACHINE,
                                  kUninstallKeyWow64,
                                  ProgramInfo::Source::RegistryHKLM_WOW64);
    all.append(wow64);

    // HKCU
    auto hkcu =
        scanRegistryHive(HKEY_CURRENT_USER, kUninstallKeyHKCU, ProgramInfo::Source::RegistryHKCU);
    all.append(hkcu);

    return all;
}

namespace {

QString hivePrefix(HKEY hive) {
    if (hive == HKEY_LOCAL_MACHINE) {
        return QStringLiteral("HKLM");
    }
    if (hive == HKEY_CURRENT_USER) {
        return QStringLiteral("HKCU");
    }
    return {};
}

// Shared completeness warning for a registry Uninstall scan that could not be fully read (a
// denied/erroring hive, or a subkey that could not be counted, enumerated, or opened). Mirrors
// warnIfAppxIncomplete: the partial list is still returned, but the caller is told it may be
// incomplete rather than silently reading fewer programs as the whole truth.
const auto kRegistryIncompleteMessage = QStringLiteral(
    "Some installed programs could not be read from the registry; "
    "the installed-programs list may be incomplete.");

// Number of direct subkeys under an opened Uninstall key. @p ok is false when the count could not
// be read: the hive then enumerates as empty, which is an INCOMPLETE result (surfaced by the
// caller), never a genuine "no programs installed".
DWORD registrySubkeyCount(HKEY key, bool& ok) {
    DWORD count = 0;
    ok = RegQueryInfoKeyW(key,
                          nullptr,
                          nullptr,
                          nullptr,
                          &count,
                          nullptr,
                          nullptr,
                          nullptr,
                          nullptr,
                          nullptr,
                          nullptr,
                          nullptr) == ERROR_SUCCESS;
    return count;
}

}  // namespace

QVector<ProgramInfo> ProgramEnumerator::scanRegistryHive(HKEY hive,
                                                         const wchar_t* subkey,
                                                         ProgramInfo::Source source) {
    QVector<ProgramInfo> results;

    HKEY uninstall_key = nullptr;
    LONG rc = RegOpenKeyExW(hive, subkey, 0, KEY_READ, &uninstall_key);
    if (rc != ERROR_SUCCESS) {
        // An ABSENT Uninstall key (ERROR_FILE_NOT_FOUND: no WOW64 view on 32-bit Windows, or no
        // per-user key on a fresh profile) is a legitimately empty -- but COMPLETE -- hive. Any
        // other failure (notably ACCESS_DENIED) means the hive could not be read, so the inventory
        // is incomplete: warn rather than silently returning fewer programs.
        if (rc != ERROR_FILE_NOT_FOUND) {
            Q_EMIT enumerationWarning(kRegistryIncompleteMessage);
        }
        return results;
    }

    // Completeness signal threaded through this hive scan (mirrors the vuln scanner's
    // scanFastRegistryHive): a failed count/enum/sub-open means an entry may be missing, so the
    // list is surfaced as possibly-incomplete instead of being read as an authoritative empty.
    bool count_ok = true;
    const DWORD subkey_count = registrySubkeyCount(uninstall_key, count_ok);
    bool complete = count_ok;

    const QString hive_name = hivePrefix(hive);
    constexpr DWORD kMaxSubkeyName = 256;
    wchar_t subkey_name[kMaxSubkeyName];

    for (DWORD i = 0; i < subkey_count; ++i) {
        DWORD name_len = kMaxSubkeyName;
        rc = RegEnumKeyExW(
            uninstall_key, i, subkey_name, &name_len, nullptr, nullptr, nullptr, nullptr);
        if (rc != ERROR_SUCCESS) {
            complete = false;  // a subkey name could not be read -> an entry was missed
            continue;
        }

        HKEY app_key = nullptr;
        rc = RegOpenKeyExW(uninstall_key, subkey_name, 0, KEY_READ, &app_key);
        if (rc != ERROR_SUCCESS) {
            complete = false;  // this program's subkey could not be opened -> inventory incomplete
            continue;
        }

        const QString reg_path = QString("%1\\%2\\%3")
                                     .arg(hive_name,
                                          QString::fromWCharArray(subkey),
                                          QString::fromWCharArray(subkey_name));

        parseRegistryEntry(app_key, source, reg_path, results);
        RegCloseKey(app_key);
    }

    RegCloseKey(uninstall_key);
    if (!complete) {
        Q_EMIT enumerationWarning(kRegistryIncompleteMessage);
    }
    return results;
}

void ProgramEnumerator::parseRegistryEntry(HKEY app_key,
                                           ProgramInfo::Source source,
                                           const QString& reg_path,
                                           QVector<ProgramInfo>& results) {
    if (isSystemComponent(app_key)) {
        ProgramInfo prog;
        prog.isSystemComponent = true;
        prog.displayName = readRegString(app_key, L"DisplayName");
        if (prog.displayName.isEmpty()) {
            return;
        }
        prog.publisher = readRegString(app_key, L"Publisher");
        prog.displayVersion = readRegString(app_key, L"DisplayVersion");
        prog.installLocation = readRegString(app_key, L"InstallLocation");
        prog.uninstallString = readRegString(app_key, L"UninstallString");
        prog.source = source;
        prog.registryKeyPath = reg_path;
        results.append(prog);
        return;
    }

    const QString display_name = readRegString(app_key, L"DisplayName");
    if (display_name.isEmpty()) {
        return;
    }

    ProgramInfo prog;
    prog.displayName = display_name;
    prog.publisher = readRegString(app_key, L"Publisher");
    prog.displayVersion = readRegString(app_key, L"DisplayVersion");
    prog.installDate = readRegString(app_key, L"InstallDate");
    prog.installLocation = readRegString(app_key, L"InstallLocation");
    prog.uninstallString = readRegString(app_key, L"UninstallString");
    prog.quietUninstallString = readRegString(app_key, L"QuietUninstallString");
    prog.modifyPath = readRegString(app_key, L"ModifyPath");
    prog.displayIcon = readRegString(app_key, L"DisplayIcon");
    prog.source = source;
    prog.estimatedSizeKB = static_cast<qint64>(readRegDword(app_key, L"EstimatedSize"));
    prog.registryKeyPath = reg_path;
    results.append(prog);
}

QString ProgramEnumerator::readRegString(HKEY key, const wchar_t* valueName) {
    // Every caller (parseRegistryEntry) passes a wide string literal.
    Q_ASSERT(valueName);
    DWORD type = 0;
    DWORD size = 0;

    LONG rc = RegQueryValueExW(key, valueName, nullptr, &type, nullptr, &size);
    if (rc != ERROR_SUCCESS || (type != REG_SZ && type != REG_EXPAND_SZ) || size == 0) {
        return {};
    }

    // Bound the allocation: DisplayName/paths are tiny, but the size comes from an
    // attacker-writable value, so a hostile multi-hundred-MB REG_SZ must not force an unbounded
    // allocation (OOM). Reject anything past a generous ceiling.
    constexpr DWORD kMaxRegStringBytes = 1u << 20;  // 1 MiB
    if (size > kMaxRegStringBytes) {
        return {};
    }

    // Allocate buffer
    std::vector<wchar_t> buffer((size / sizeof(wchar_t)) + 1, L'\0');
    DWORD type2 = 0;
    DWORD size2 = size;
    rc = RegQueryValueExW(
        key, valueName, nullptr, &type2, reinterpret_cast<LPBYTE>(buffer.data()), &size2);
    // Re-validate the type on the second read: the value can be swapped between the two queries,
    // and a same-sized non-string replacement would otherwise be read as UTF-16. A grown value
    // makes this call return ERROR_MORE_DATA (rc != SUCCESS), which is also rejected.
    if (rc != ERROR_SUCCESS || (type2 != REG_SZ && type2 != REG_EXPAND_SZ) || size2 > size) {
        return {};
    }

    return QString::fromWCharArray(buffer.data()).trimmed();
}

DWORD ProgramEnumerator::readRegDword(HKEY key, const wchar_t* valueName) {
    // Every caller (parseRegistryEntry, isSystemComponent) passes a wide string literal.
    Q_ASSERT(valueName);
    DWORD value = 0;
    DWORD size = sizeof(DWORD);
    DWORD type = 0;

    LONG const rc =
        RegQueryValueExW(key, valueName, nullptr, &type, reinterpret_cast<LPBYTE>(&value), &size);
    // Require the exact 4-byte REG_DWORD width: a wrong-typed or short value must read as 0, not
    // as a partially-written DWORD.
    if (rc != ERROR_SUCCESS || type != REG_DWORD || size != sizeof(DWORD)) {
        return 0;
    }
    return value;
}

bool ProgramEnumerator::isSystemComponent(HKEY key) {
    DWORD const sys_comp = readRegDword(key, L"SystemComponent");
    return sys_comp == 1;
}

#endif  // Q_OS_WIN

void ProgramEnumerator::parseUwpPackage(const QJsonObject& obj, QVector<ProgramInfo>& results) {
    if (obj["IsFramework"].toBool(false)) {
        return;
    }

    ProgramInfo prog;
    prog.displayName = obj["Name"].toString();
    prog.packageFamilyName = obj["PackageFamilyName"].toString();
    prog.packageFullName = obj["PackageFullName"].toString();
    prog.publisher = obj["Publisher"].toString();
    prog.displayVersion = obj["Version"].toString();
    prog.installLocation = obj["InstallLocation"].toString();
    prog.source = ProgramInfo::Source::UWP;

    if (!prog.installLocation.isEmpty() && QDir(prog.installLocation).exists()) {
        prog.actualSizeBytes = calculateDirSize(prog.installLocation);
    }

    if (!prog.displayName.isEmpty()) {
        results.append(prog);
    }
}

bool ProgramEnumerator::appxScanSucceeded(const ProcessResult& result) {
    if (!result.succeeded()) {
        return false;
    }
    QJsonParseError error;
    QJsonDocument::fromJson(result.std_out.toUtf8(), &error);
    return error.error == QJsonParseError::NoError;
}

QVector<ProgramInfo> ProgramEnumerator::scanUwpPackages(bool& scanOk) {
    QVector<ProgramInfo> results;

    // System32-qualified interpreter, never a bare "powershell.exe": the enumerated
    // programs feed an elevated uninstall, so a PATH/CWD-planted powershell must not be
    // able to supply the inventory. Unresolvable -> FAILED scan (scanOk false).
    const QString powershell = sak::systemPowerShellPath();
    if (powershell.isEmpty()) {
        scanOk = false;
        return results;
    }

    const auto result = sak::runProcess(powershell,
                                        {QStringLiteral("-NoProfile"),
                                         QStringLiteral("-NonInteractive"),
                                         QStringLiteral("-Command"),
                                         kUwpPackagesCommand},
                                        30'000);
    scanOk = appxScanSucceeded(result);
    if (!scanOk) {
        return results;
    }

    const QByteArray output = result.std_out.toUtf8();
    const QJsonDocument doc = QJsonDocument::fromJson(output);

    const QJsonArray arr = jsonDocToArray(doc);
    for (const auto& val : arr) {
        parseUwpPackage(val.toObject(), results);
    }

    return results;
}

QVector<ProgramInfo> ProgramEnumerator::scanProvisionedPackages(bool& scanOk) {
    QVector<ProgramInfo> results;

    // Same System32 qualification as scanUwpPackages: unresolvable -> FAILED scan.
    const QString powershell = sak::systemPowerShellPath();
    if (powershell.isEmpty()) {
        scanOk = false;
        return results;
    }

    const auto result = sak::runProcess(
        powershell,
        {QStringLiteral("-NoProfile"),
         QStringLiteral("-NonInteractive"),
         QStringLiteral("-Command"),
         QStringLiteral("Get-AppxProvisionedPackage -Online -ErrorAction SilentlyContinue | "
                        "Select-Object DisplayName, PackageName, Version | "
                        "ConvertTo-Json -Compress")},
        30'000);
    scanOk = appxScanSucceeded(result);
    if (!scanOk) {
        return results;
    }

    const QByteArray output = result.std_out.toUtf8();
    const QJsonDocument doc = QJsonDocument::fromJson(output);

    QJsonArray arr;
    if (doc.isArray()) {
        arr = doc.array();
    } else if (doc.isObject()) {
        arr.append(doc.object());
    }

    for (const auto& val : arr) {
        QJsonObject obj = val.toObject();

        ProgramInfo prog;
        prog.displayName = obj["DisplayName"].toString();
        prog.packageFullName = obj["PackageName"].toString();
        prog.displayVersion = obj["Version"].toString();
        prog.source = ProgramInfo::Source::Provisioned;

        if (!prog.displayName.isEmpty()) {
            results.append(prog);
        }
    }

    return results;
}

QImage ProgramEnumerator::extractIcon(const QString& path) {
    if (path.isEmpty()) {
        return {};
    }

#ifdef Q_OS_WIN
    // Extract path and optional icon index
    QString icon_path = path;
    int icon_index = 0;

    // Handle "path,index" format
    const int comma = static_cast<int>(path.lastIndexOf(','));
    if (comma > 0) {
        bool ok = false;
        const int idx = path.mid(comma + 1).trimmed().toInt(&ok);
        if (ok) {
            icon_path = path.left(comma).trimmed();
            icon_index = idx;
        }
    }

    // Strip quotes
    if (icon_path.startsWith('"') && icon_path.endsWith('"')) {
        icon_path = icon_path.mid(1, icon_path.length() - kQuotedPathTrimChars);
    }

    if (isRemoteOrDevicePath(icon_path)) {
        // Never resolve an icon from a UNC/device path (SMB auth / remote I/O from an
        // attacker-writable DisplayIcon value).
        return {};
    }

    SHFILEINFOW sfi{};
    DWORD_PTR const result = SHGetFileInfoW(reinterpret_cast<LPCWSTR>(icon_path.utf16()),
                                            0,
                                            &sfi,
                                            sizeof(sfi),
                                            SHGFI_ICON | SHGFI_SMALLICON);

    if ((result != 0u) && (sfi.hIcon != nullptr)) {
        QImage image = QImage::fromHICON(sfi.hIcon);
        DestroyIcon(sfi.hIcon);
        return image;
    }

    Q_UNUSED(icon_index)
#else
    Q_UNUSED(path)
#endif

    return {};
}

void ProgramEnumerator::deduplicatePrograms(QVector<ProgramInfo>& programs) {
    // Empty is valid (see enrichWithIconsAndSizes) -- no non-empty assert.
    QSet<QString> seen;
    QVector<ProgramInfo> unique;
    unique.reserve(programs.size());

    for (const auto& prog : programs) {
        // Use display name + publisher as dedup key
        const QString key = prog.displayName.toLower() + "|" + prog.publisher.toLower();

        if (!seen.contains(key)) {
            seen.insert(key);
            unique.append(prog);
        }
    }

    programs = unique;
}

}  // namespace sak
