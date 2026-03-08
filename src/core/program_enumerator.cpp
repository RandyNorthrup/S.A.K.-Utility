// Copyright (c) 2025-2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file program_enumerator.cpp
/// @brief Enumerates all installed Win32 and UWP programs with rich metadata

#include "sak/program_enumerator.h"
#include "sak/layout_constants.h"

#include <QDir>
#include <QDirIterator>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QSet>
#include <QTimer>

#ifdef Q_OS_WIN
#include <shellapi.h>
#pragma comment(lib, "Shell32.lib")
#pragma comment(lib, "Advapi32.lib")
#endif

namespace sak {

namespace {
[[nodiscard]] QString normalizeUninstallExePath(QString uninstallString)
{
    QString exePath = std::move(uninstallString);

    if (exePath.startsWith('"')) {
        const int endQuote = exePath.indexOf('"', 1);
        if (endQuote > 0) {
            exePath = exePath.mid(1, endQuote - 1);
        }
        return exePath;
    }

    const int space = exePath.indexOf(' ');
    if (space > 0) {
        exePath = exePath.left(space);
    }
    return exePath;
}

[[nodiscard]] QJsonArray jsonDocToArray(const QJsonDocument& doc)
{
    if (doc.isArray()) {
        return doc.array();
    }
    if (doc.isObject()) {
        return QJsonArray{doc.object()};
    }
    return {};
}

const auto kUwpPackagesCommand = QStringLiteral(
    "Get-AppxPackage | Select-Object Name, PackageFamilyName, "
    "PackageFullName, Publisher, Version, InstallLocation, "
    "IsFramework, SignatureKind | ConvertTo-Json -Compress");
} // namespace

ProgramEnumerator::ProgramEnumerator(QObject* parent)
    : QObject(parent)
{
}

ProgramEnumerator::~ProgramEnumerator() = default;

void ProgramEnumerator::requestCancel()
{
    m_cancelRequested.store(true, std::memory_order_release);
}

void ProgramEnumerator::resetCancel()
{
    m_cancelRequested.store(false, std::memory_order_release);
}

void ProgramEnumerator::enumerateAll()
{
    if (m_cancelRequested.load(std::memory_order_acquire)) {
        Q_EMIT enumerationFailed("Enumeration cancelled.");
        return;
    }
    Q_EMIT enumerationStarted();

    QVector<ProgramInfo> all_programs;

    try {
#ifdef Q_OS_WIN
        // Phase 1: Win32 registry programs
        auto registry_programs = scanRegistryPrograms();
        if (m_cancelRequested.load(std::memory_order_acquire)) {
            Q_EMIT enumerationFailed("Enumeration cancelled.");
            return;
        }
        all_programs.append(registry_programs);
        Q_EMIT enumerationProgress(50, 100);
#endif

        // Phase 2: UWP packages
        auto uwp_programs = scanUwpPackages();
        if (m_cancelRequested.load(std::memory_order_acquire)) {
            Q_EMIT enumerationFailed("Enumeration cancelled.");
            return;
        }
        all_programs.append(uwp_programs);
        Q_EMIT enumerationProgress(75, 100);

        // Phase 3: Provisioned UWP packages
        auto provisioned = scanProvisionedPackages();
        if (m_cancelRequested.load(std::memory_order_acquire)) {
            Q_EMIT enumerationFailed("Enumeration cancelled.");
            return;
        }
        all_programs.append(provisioned);
        Q_EMIT enumerationProgress(85, 100);

        // Phase 4: Deduplicate
        deduplicatePrograms(all_programs);

        // Phase 5: Detect orphaned entries
        detectOrphaned(all_programs);

        // Phase 6: Mark bloatware
        markBloatware(all_programs);
        Q_EMIT enumerationProgress(95, 100);

        // Phase 7: Extract icons and calculate sizes
        for (int i = 0; i < all_programs.size(); ++i) {
            if (m_cancelRequested.load(std::memory_order_acquire)) {
                Q_EMIT enumerationFailed("Enumeration cancelled.");
                return;
            }
            auto& prog = all_programs[i];

            // Extract icon
            if (!prog.displayIcon.isEmpty()) {
                prog.cachedImage = extractIcon(prog.displayIcon);
            }

            if (prog.cachedImage.isNull() && !prog.installLocation.isEmpty()) {
                QDir dir(prog.installLocation);
                const auto exes = dir.entryList({"*.exe"}, QDir::Files, QDir::Name);
                const QString exe = exes.value(0);
                prog.cachedImage = exe.isEmpty()
                                     ? QImage{}
                                     : extractIcon(dir.filePath(exe));
            }

            // Calculate actual size if install location exists
            if (!prog.installLocation.isEmpty()
                && QDir(prog.installLocation).exists()) {
                prog.actualSizeBytes = calculateDirSize(prog.installLocation);
            } else if (prog.estimatedSizeKB > 0) {
                prog.actualSizeBytes = prog.estimatedSizeKB * 1024;
            }
        }

        Q_EMIT enumerationProgress(100, 100);
        m_cachedPrograms = all_programs;
        Q_EMIT enumerationFinished(all_programs);

    } catch (const std::exception& e) {
        Q_EMIT enumerationFailed(QString("Enumeration error: %1").arg(e.what()));
    }
}

QVector<ProgramInfo> ProgramEnumerator::programs() const
{
    return m_cachedPrograms;
}

void ProgramEnumerator::detectOrphaned(QVector<ProgramInfo>& programs)
{
    for (auto& prog : programs) {
        if (prog.source == ProgramInfo::Source::UWP
            || prog.source == ProgramInfo::Source::Provisioned) {
            continue;  // UWP apps are always "installed"
        }

        const bool installMissing = !prog.installLocation.isEmpty()
                                  && !QDir(prog.installLocation).exists();
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

        prog.isOrphaned = !QFileInfo::exists(exePath);
    }
}

void ProgramEnumerator::markBloatware(QVector<ProgramInfo>& programs)
{
    // Bloatware patterns from CheckBloatwareAction database
    static const QStringList kBloatwarePatterns = {
        "CandyCrush", "FarmVille", "BubbleWitch", "MarchofEmpires",
        "Minecraft", "Solitaire", "Xbox", "Zune",
        "BingNews", "BingWeather", "BingSports", "BingFinance",
        "SkypeApp", "YourPhone", "PhoneLink", "Messaging",
        "GetHelp", "Getstarted", "MicrosoftOfficeHub", "WindowsMaps",
        "WindowsAlarms", "WindowsSoundRecorder", "WindowsFeedbackHub", "Wallet",
        "Microsoft3DViewer", "Print3D", "MixedReality",
        "People", "OneConnect",
        "ActiproSoftware", "king.com", "Facebook", "Twitter",
        "LinkedIn", "Netflix", "Spotify", "Disney"
    };

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

qint64 ProgramEnumerator::calculateDirSize(const QString& path)
{
    qint64 total = 0;
    QDirIterator it(path, QDir::Files | QDir::Hidden | QDir::NoSymLinks,
                    QDirIterator::Subdirectories);
    while (it.hasNext()) {
        it.next();
        total += it.fileInfo().size();
    }
    return total;
}

#ifdef Q_OS_WIN

QVector<ProgramInfo> ProgramEnumerator::scanRegistryPrograms()
{
    QVector<ProgramInfo> all;

    // HKLM 64-bit
    auto hklm64 = scanRegistryHive(HKEY_LOCAL_MACHINE, kUninstallKey64,
                                    ProgramInfo::Source::RegistryHKLM);
    all.append(hklm64);

    // HKLM WOW64 (32-bit apps on 64-bit Windows)
    auto wow64 = scanRegistryHive(HKEY_LOCAL_MACHINE, kUninstallKeyWow64,
                                   ProgramInfo::Source::RegistryHKLM_WOW64);
    all.append(wow64);

    // HKCU
    auto hkcu = scanRegistryHive(HKEY_CURRENT_USER, kUninstallKeyHKCU,
                                  ProgramInfo::Source::RegistryHKCU);
    all.append(hkcu);

    return all;
}

QVector<ProgramInfo> ProgramEnumerator::scanRegistryHive(
    HKEY hive, const wchar_t* subkey, ProgramInfo::Source source)
{
    QVector<ProgramInfo> results;

    HKEY uninstall_key = nullptr;
    LONG rc = RegOpenKeyExW(hive, subkey, 0, KEY_READ, &uninstall_key);
    if (rc != ERROR_SUCCESS) {
        return results;
    }

    DWORD subkey_count = 0;
    RegQueryInfoKeyW(uninstall_key, nullptr, nullptr, nullptr, &subkey_count,
                     nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr);

    wchar_t subkey_name[256];
    for (DWORD i = 0; i < subkey_count; ++i) {
        DWORD name_len = 256;
        rc = RegEnumKeyExW(uninstall_key, i, subkey_name, &name_len,
                           nullptr, nullptr, nullptr, nullptr);
        if (rc != ERROR_SUCCESS) {
            continue;
        }

        HKEY app_key = nullptr;
        rc = RegOpenKeyExW(uninstall_key, subkey_name, 0, KEY_READ, &app_key);
        if (rc != ERROR_SUCCESS) {
            continue;
        }

        // Skip system components
        if (isSystemComponent(app_key)) {
            ProgramInfo prog;
            prog.isSystemComponent = true;
            prog.displayName = readRegString(app_key, L"DisplayName");
            if (!prog.displayName.isEmpty()) {
                prog.publisher = readRegString(app_key, L"Publisher");
                prog.displayVersion = readRegString(app_key, L"DisplayVersion");
                prog.installLocation = readRegString(app_key, L"InstallLocation");
                prog.uninstallString = readRegString(app_key, L"UninstallString");
                prog.source = source;

                // Build registry key path
                QString hive_name;
                if (hive == HKEY_LOCAL_MACHINE) hive_name = "HKLM";
                else if (hive == HKEY_CURRENT_USER) hive_name = "HKCU";
                prog.registryKeyPath = QString("%1\\%2\\%3")
                    .arg(hive_name,
                         QString::fromWCharArray(subkey),
                         QString::fromWCharArray(subkey_name));

                results.append(prog);
            }
            RegCloseKey(app_key);
            continue;
        }

        // Read DisplayName — required field
        QString display_name = readRegString(app_key, L"DisplayName");
        if (display_name.isEmpty()) {
            RegCloseKey(app_key);
            continue;
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

        // Estimated size (DWORD in KB)
        DWORD est_size = readRegDword(app_key, L"EstimatedSize");
        prog.estimatedSizeKB = static_cast<qint64>(est_size);

        // Build registry key path
        QString hive_name;
        if (hive == HKEY_LOCAL_MACHINE) hive_name = "HKLM";
        else if (hive == HKEY_CURRENT_USER) hive_name = "HKCU";
        prog.registryKeyPath = QString("%1\\%2\\%3")
            .arg(hive_name,
                 QString::fromWCharArray(subkey),
                 QString::fromWCharArray(subkey_name));

        RegCloseKey(app_key);
        results.append(prog);
    }

    RegCloseKey(uninstall_key);
    return results;
}

QString ProgramEnumerator::readRegString(HKEY key, const wchar_t* valueName)
{
    DWORD type = 0;
    DWORD size = 0;

    LONG rc = RegQueryValueExW(key, valueName, nullptr, &type, nullptr, &size);
    if (rc != ERROR_SUCCESS || (type != REG_SZ && type != REG_EXPAND_SZ) || size == 0) {
        return {};
    }

    // Allocate buffer
    std::vector<wchar_t> buffer(size / sizeof(wchar_t) + 1, L'\0');
    rc = RegQueryValueExW(key, valueName, nullptr, nullptr,
                          reinterpret_cast<LPBYTE>(buffer.data()), &size);
    if (rc != ERROR_SUCCESS) {
        return {};
    }

    return QString::fromWCharArray(buffer.data()).trimmed();
}

DWORD ProgramEnumerator::readRegDword(HKEY key, const wchar_t* valueName)
{
    DWORD value = 0;
    DWORD size = sizeof(DWORD);
    DWORD type = 0;

    LONG rc = RegQueryValueExW(key, valueName, nullptr, &type,
                               reinterpret_cast<LPBYTE>(&value), &size);
    if (rc != ERROR_SUCCESS || type != REG_DWORD) {
        return 0;
    }
    return value;
}

bool ProgramEnumerator::isSystemComponent(HKEY key)
{
    DWORD sys_comp = readRegDword(key, L"SystemComponent");
    return sys_comp == 1;
}

#endif // Q_OS_WIN

QVector<ProgramInfo> ProgramEnumerator::scanUwpPackages()
{
    QVector<ProgramInfo> results;

    QProcess ps;
    ps.setProgram("powershell.exe");
    ps.setArguments({
        "-NoProfile", "-NonInteractive", "-Command",
        kUwpPackagesCommand
    });
    ps.start();

    if (!ps.waitForStarted(sak::kTimeoutProcessStartMs)) {
        return results;
    }
    if (!ps.waitForFinished(30000)) {
        return results;
    }

    if (ps.exitCode() != 0) {
        return results;
    }

    QByteArray output = ps.readAllStandardOutput();
    QJsonParseError error;
    QJsonDocument doc = QJsonDocument::fromJson(output, &error);
    if (error.error != QJsonParseError::NoError) {
        return results;
    }

    const QJsonArray arr = jsonDocToArray(doc);

    for (const auto& val : arr) {
        QJsonObject obj = val.toObject();

        // Skip framework packages
        if (obj["IsFramework"].toBool(false)) {
            continue;
        }

        ProgramInfo prog;
        prog.displayName = obj["Name"].toString();
        prog.packageFamilyName = obj["PackageFamilyName"].toString();
        prog.packageFullName = obj["PackageFullName"].toString();
        prog.publisher = obj["Publisher"].toString();
        prog.displayVersion = obj["Version"].toString();
        prog.installLocation = obj["InstallLocation"].toString();
        prog.source = ProgramInfo::Source::UWP;

        // Clean display name — strip "Microsoft." prefix for readability
        if (prog.displayName.startsWith("Microsoft.")) {
            // Keep the original but it will be cleaned up in display
        }

        // Calculate size if install location exists
        if (!prog.installLocation.isEmpty()
            && QDir(prog.installLocation).exists()) {
            prog.actualSizeBytes = calculateDirSize(prog.installLocation);
        }

        if (!prog.displayName.isEmpty()) {
            results.append(prog);
        }
    }

    return results;
}

QVector<ProgramInfo> ProgramEnumerator::scanProvisionedPackages()
{
    QVector<ProgramInfo> results;

    QProcess ps;
    ps.setProgram("powershell.exe");
    ps.setArguments({
        "-NoProfile", "-NonInteractive", "-Command",
        "Get-AppxProvisionedPackage -Online -ErrorAction SilentlyContinue | "
        "Select-Object DisplayName, PackageName, Version | "
        "ConvertTo-Json -Compress"
    });
    ps.start();

    if (!ps.waitForStarted(sak::kTimeoutProcessStartMs)) {
        return results;
    }
    if (!ps.waitForFinished(30000)) {
        return results;
    }

    if (ps.exitCode() != 0) {
        return results;
    }

    QByteArray output = ps.readAllStandardOutput();
    QJsonParseError error;
    QJsonDocument doc = QJsonDocument::fromJson(output, &error);
    if (error.error != QJsonParseError::NoError) {
        return results;
    }

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

QImage ProgramEnumerator::extractIcon(const QString& path)
{
    if (path.isEmpty()) {
        return {};
    }

#ifdef Q_OS_WIN
    // Extract path and optional icon index
    QString icon_path = path;
    int icon_index = 0;

    // Handle "path,index" format
    int comma = path.lastIndexOf(',');
    if (comma > 0) {
        bool ok = false;
        int idx = path.mid(comma + 1).trimmed().toInt(&ok);
        if (ok) {
            icon_path = path.left(comma).trimmed();
            icon_index = idx;
        }
    }

    // Strip quotes
    if (icon_path.startsWith('"') && icon_path.endsWith('"')) {
        icon_path = icon_path.mid(1, icon_path.length() - 2);
    }

    SHFILEINFOW sfi{};
    DWORD_PTR result = SHGetFileInfoW(
        reinterpret_cast<LPCWSTR>(icon_path.utf16()),
        0, &sfi, sizeof(sfi), SHGFI_ICON | SHGFI_SMALLICON);

    if (result && sfi.hIcon) {
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

void ProgramEnumerator::deduplicatePrograms(QVector<ProgramInfo>& programs)
{
    QSet<QString> seen;
    QVector<ProgramInfo> unique;
    unique.reserve(programs.size());

    for (const auto& prog : programs) {
        // Use display name + publisher as dedup key
        QString key = prog.displayName.toLower() + "|"
                    + prog.publisher.toLower();

        if (!seen.contains(key)) {
            seen.insert(key);
            unique.append(prog);
        }
    }

    programs = unique;
}

} // namespace sak
