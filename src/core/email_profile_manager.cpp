// Copyright (c) 2025-2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file email_profile_manager.cpp
/// @brief Discovers email client profiles via registry/filesystem

#include "sak/email_profile_manager.h"

#include "sak/email_constants.h"
#include "sak/io_write_utils.h"
#include "sak/logger.h"
#include "sak/process_runner.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSettings>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTextStream>

#ifdef Q_OS_WIN
#include <vector>

#include <windows.h>
#endif

namespace {

/// Stable manifest label for an email client type.
QString clientTypeManifestName(sak::EmailClientType type) {
    switch (type) {
    case sak::EmailClientType::Outlook:
        return QStringLiteral("Outlook");
    case sak::EmailClientType::Thunderbird:
        return QStringLiteral("Thunderbird");
    case sak::EmailClientType::WindowsMail:
        return QStringLiteral("WindowsMail");
    case sak::EmailClientType::Other:
        break;
    }
    return QStringLiteral("Other");
}

/// RAII single-flight guard: clears the active flag when it goes out of scope, so
/// every return path from a guarded operation releases the lock (B7-16).
class ScopedActiveFlag {
public:
    explicit ScopedActiveFlag(std::atomic<bool>& flag) : m_flag(flag) {}
    ~ScopedActiveFlag() { m_flag.store(false); }
    ScopedActiveFlag(const ScopedActiveFlag&) = delete;
    ScopedActiveFlag& operator=(const ScopedActiveFlag&) = delete;
    ScopedActiveFlag(ScopedActiveFlag&&) = delete;
    ScopedActiveFlag& operator=(ScopedActiveFlag&&) = delete;

private:
    std::atomic<bool>& m_flag;
};

#ifdef Q_OS_WIN
/// wchar_t capacity for the GetFinalPathNameByHandleW output buffer.
constexpr int kFinalPathBufferChars = 4096;
/// Length of the Win32 "\\?\" extended-length path prefix stripped from a resolved path.
constexpr int kWin32ExtendedPrefixLength = 4;

/// @brief Fully resolve an EXISTING path through every reparse point (junctions AND symlinks) to
/// its real on-disk location, using GetFinalPathNameByHandleW. QFileInfo::canonicalFilePath does
/// NOT follow directory junctions on Windows, so it cannot detect a junction-based escape. Returns
/// a forward-slash path with the \\?\ prefix stripped, or empty if the path cannot be opened.
QString realCanonicalPath(const QString& path) {
    HANDLE handle = CreateFileW(reinterpret_cast<const wchar_t*>(path.utf16()),
                                0,
                                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                nullptr,
                                OPEN_EXISTING,
                                FILE_FLAG_BACKUP_SEMANTICS,  // required to open a directory handle
                                nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        return {};
    }
    std::vector<wchar_t> buffer(kFinalPathBufferChars);
    const DWORD written = GetFinalPathNameByHandleW(
        handle, buffer.data(), static_cast<DWORD>(buffer.size()), FILE_NAME_NORMALIZED);
    CloseHandle(handle);
    if (written == 0 || written >= buffer.size()) {
        return {};
    }
    QString result = QString::fromWCharArray(buffer.data(), static_cast<int>(written));
    if (result.startsWith(QStringLiteral("\\\\?\\"))) {
        result = result.mid(kWin32ExtendedPrefixLength);
    }
    return QDir::fromNativeSeparators(result);
}
#else
QString realCanonicalPath(const QString& path) {
    return QFileInfo(path).canonicalFilePath();
}
#endif

constexpr int kSupportedManifestVersion = 1;
constexpr int kRegExportTimeoutMs = 10'000;
constexpr int kRegImportTimeoutMs = 10'000;
constexpr int kRegistryPathMinimumBytes = 4;
constexpr qsizetype kMapiPropertyLeafNameLength = 8;
constexpr qsizetype kMapiPropertyTypePrefixLength = 4;
/// Cap on total data files a single Thunderbird discovery may collect. The recursion is
/// already depth-limited, but a very wide/hostile profile tree (or a tampered absolute
/// profiles.ini Path) could otherwise grow the result vector without bound and exhaust memory.
constexpr int kMaxThunderbirdDataFiles = 10'000;

/// Length in bytes of the UTF-16LE byte-order mark (0xFF 0xFE) that reg.exe writes at the start
/// of every export; also the offset past it to the first code unit.
constexpr int kUtf16BomLength = 2;
/// The two bytes, in file order, of the UTF-16LE byte-order mark.
constexpr int kUtf16LeBomByte0 = 0xFF;
constexpr int kUtf16LeBomByte1 = 0xFE;
/// Bytes per UTF-16 code unit, to convert a payload byte count into a code-unit count.
constexpr int kBytesPerUtf16CodeUnit = 2;
/// Characters dropped from a "[...]" .reg key-section header: the leading '[' and trailing ']'.
constexpr int kKeySectionBracketChars = 2;

/// Resolve a manifest-supplied file name strictly inside `dir`. Returns the
/// absolute path only when `name` is a bare basename (no directory component,
/// not absolute, not "."/".."); otherwise returns an empty string so the caller
/// rejects it. This confines both the backup source read and the registry
/// import to the backup directory, blocking "../../payload.reg" style escapes.
QString resolveWithinDirectory(const QString& dir, const QString& name) {
    if (name.isEmpty()) {
        return {};
    }
    const QString bare = QFileInfo(name).fileName();
    if (bare != name || bare == QStringLiteral(".") || bare == QStringLiteral("..")) {
        return {};
    }
    return QDir(dir).absoluteFilePath(bare);
}

/// @brief True for the characters allowed verbatim in a sanitized filename component:
/// [A-Za-z0-9._-].
bool isSafeFileNameChar(QChar ch) {
    return (ch >= QLatin1Char('a') && ch <= QLatin1Char('z')) ||
           (ch >= QLatin1Char('A') && ch <= QLatin1Char('Z')) ||
           (ch >= QLatin1Char('0') && ch <= QLatin1Char('9')) || ch == QLatin1Char('.') ||
           ch == QLatin1Char('_') || ch == QLatin1Char('-');
}

/// @brief Reduce an untrusted profile name to a safe bare filename component: every character
/// outside [A-Za-z0-9._-] becomes '_', and a name that would collapse to empty or "."/".." is
/// replaced with a fixed placeholder. Guarantees no path separators and no "." / ".." traversal.
QString sanitizeFileComponent(const QString& name) {
    QString out;
    out.reserve(name.size());
    for (const QChar ch : name) {
        out.append(isSafeFileNameChar(ch) ? ch : QLatin1Char('_'));
    }
    if (out.isEmpty() || out == QStringLiteral(".") || out == QStringLiteral("..")) {
        return QStringLiteral("profile");
    }
    return out;
}

/// @brief Deepest existing ancestor directory of @p abs_path (which itself may not exist yet),
/// or empty if none of its ancestors exist. A junction/symlink can only redirect through an
/// EXISTING reparse point, so this is the part that must be canonicalized for a real containment
/// check.
QString deepestExistingAncestor(const QString& abs_path) {
    QString existing = abs_path;
    while (!existing.isEmpty() && !QFileInfo::exists(existing)) {
        const QString parent = QFileInfo(existing).path();
        if (parent == existing) {
            return {};  // reached the filesystem root without finding an existing component
        }
        existing = parent;
    }
    return existing;
}

/// @brief True iff @p suffix-carrying @p path names a file type this tool legitimately backs up and
/// restores (Outlook PST/OST, Windows Mail EML/PST, Thunderbird MBOX -- which is extensionless). A
/// restore destination is otherwise confined only to the home tree, which still contains sensitive
/// spots (e.g. the Startup folder): without an extension allowlist a crafted manifest could drop a
/// new .lnk/.cmd there for logon code execution. Empty suffix is allowed because Thunderbird MBOX
/// stores have no extension, and an extensionless file is inert (Windows never auto-runs one).
bool isRestorableDataFile(const QString& path) {
    static const QSet<QString> kAllowedSuffixes = {
        QStringLiteral("pst"),
        QStringLiteral("ost"),
        QStringLiteral("eml"),
        QStringLiteral("mbox"),
    };
    const QString suffix = QFileInfo(path).suffix().toLower();
    if (!suffix.isEmpty()) {
        return kAllowedSuffixes.contains(suffix);
    }
    // Extensionless files (Thunderbird MBOX) are allowed, but "extensionless is
    // inert" is not sufficient: a crafted manifest could drop an extensionless
    // authorized_keys into a dot-directory such as ~/.ssh. Reject any destination
    // that traverses a dot-directory/dotfile -- the sensitive config stores under
    // the home tree -- which no legitimate mail store uses on Windows.
    const QStringList segments = QDir::fromNativeSeparators(path).split(QLatin1Char('/'),
                                                                        Qt::SkipEmptyParts);
    for (const QString& segment : segments) {
        if (segment.startsWith(QLatin1Char('.'))) {
            return false;
        }
    }
    return true;
}

/// @brief True iff @p haystack contains @p marker ending on a path-segment boundary (end-of-string
/// or a following '\'), so "\OUTLOOK\PROFILES" matches "...\Profiles" and "...\Profiles\Sub" but
/// NOT "...\ProfilesEvil". Case-sensitive; callers pass already-upper-cased strings.
bool containsPathSegment(const QString& haystack, const QString& marker) {
    for (int pos = static_cast<int>(haystack.indexOf(marker)); pos >= 0;
         pos = static_cast<int>(haystack.indexOf(marker, pos + 1))) {
        const int after = static_cast<int>(pos + marker.size());
        if (after == haystack.size() || haystack.at(after) == QLatin1Char('\\')) {
            return true;
        }
    }
    return false;
}

/// @brief True iff @p s begins with @p prefix ending on a path-segment boundary (end-of-string or a
/// following '\'), so a root-anchored prefix cannot be laundered by a longer sibling name.
bool startsWithPathSegment(const QString& s, const QString& prefix) {
    if (!s.startsWith(prefix)) {
        return false;
    }
    return s.size() == prefix.size() || s.at(prefix.size()) == QLatin1Char('\\');
}

/// Allowed HKCU Outlook-profile prefix (upper-cased), followed by an "\OUTLOOK\PROFILES" segment.
const QString kOfficeKeyPrefix = QStringLiteral("HKEY_CURRENT_USER\\SOFTWARE\\MICROSOFT\\OFFICE\\");

/// Allowed Windows Messaging Subsystem profiles subtree (upper-cased). WMS profiles are discovered
/// and exported as Outlook, so restore must accept this hive too (finding 6).
const QString kWmsKeyPrefix = QStringLiteral(
    "HKEY_CURRENT_USER\\SOFTWARE\\MICROSOFT\\WINDOWS NT"
    "\\CURRENTVERSION\\WINDOWS MESSAGING SUBSYSTEM\\PROFILES");

/// Outlook version keys in order of preference (newest first)
const QStringList kOutlookVersions = {
    QStringLiteral("16.0"),  // Office 2016/2019/2021/365
    QStringLiteral("15.0"),  // Office 2013
    QStringLiteral("14.0"),  // Office 2010
};

/// Outlook profiles registry path template
const QString kOutlookProfilesPath = QStringLiteral(
    "HKEY_CURRENT_USER\\Software\\Microsoft\\Office\\%1"
    "\\Outlook\\Profiles");

/// Outlook data file registry path
const QString kOutlookDefaultProfilePath = QStringLiteral(
    "HKEY_CURRENT_USER\\Software\\Microsoft\\Office\\%1"
    "\\Outlook");

/// Windows Messaging Subsystem path
const QString kWmsProfilesPath = QStringLiteral(
    "HKEY_CURRENT_USER\\Software\\Microsoft\\Windows NT"
    "\\CurrentVersion\\Windows Messaging Subsystem"
    "\\Profiles");

/// Parse a registry QVariant value into a file path string
QString parseRegistryPathValue(const QVariant& raw, bool is_unicode) {
    if (!raw.isValid()) {
        return {};
    }

    if (raw.typeId() != QMetaType::QByteArray) {
        return raw.toString();
    }

    const QByteArray bytes = raw.toByteArray();
    if (bytes.size() < kRegistryPathMinimumBytes) {
        return {};
    }

    QString path_value;
    if (is_unicode) {
        path_value = QString::fromUtf16(reinterpret_cast<const char16_t*>(bytes.constData()),
                                        bytes.size() / 2);
    } else {
        path_value = QString::fromLocal8Bit(bytes);
    }
    while (path_value.endsWith(QChar(0))) {
        path_value.chop(1);
    }
    return path_value;
}

/// Create an EmailDataFile from a QFileInfo, typed by suffix
sak::EmailDataFile classifyDataFileType(const QFileInfo& fi) {
    sak::EmailDataFile data_file;
    data_file.path = fi.absoluteFilePath();
    data_file.size_bytes = fi.size();
    data_file.is_linked = true;

    const QString suffix = fi.suffix().toLower();
    if (suffix == QStringLiteral("pst")) {
        data_file.type = QStringLiteral("PST");
    } else if (suffix == QStringLiteral("ost")) {
        data_file.type = QStringLiteral("OST");
    } else {
        data_file.type = QStringLiteral("Unknown");
    }
    return data_file;
}

/// Collect PST/OST files from an Outlook profile registry key
QVector<sak::EmailDataFile> findOutlookDataFiles(const QSettings& profile_key) {
    QVector<sak::EmailDataFile> files;

    static const QStringList kPathPropertyIds = {
        QStringLiteral("6610"),
        QStringLiteral("6600"),
    };
    static const QStringList kValidTypePrefixes = {
        QStringLiteral("001f"),
        QStringLiteral("001e"),
    };

    const QStringList groups = profile_key.childGroups();
    for (const auto& group : groups) {
        const QString prefix = group + QStringLiteral("/");
        const QStringList keys = profile_key.allKeys();

        for (const auto& key : keys) {
            if (!key.startsWith(prefix)) {
                continue;
            }
            const QString value_name = key.mid(prefix.size());

            const int last_slash = static_cast<int>(value_name.lastIndexOf(QLatin1Char('/')));
            const QString leaf_name = (last_slash >= 0) ? value_name.mid(last_slash + 1)
                                                        : value_name;

            if (leaf_name.size() != kMapiPropertyLeafNameLength) {
                continue;
            }
            const QString prop_id = leaf_name.mid(kMapiPropertyTypePrefixLength);
            if (!kPathPropertyIds.contains(prop_id)) {
                continue;
            }

            const QString type_prefix = leaf_name.left(kMapiPropertyTypePrefixLength);
            if (!kValidTypePrefixes.contains(type_prefix)) {
                continue;
            }
            const bool is_unicode = (type_prefix == QStringLiteral("001f"));

            const QString path_value = parseRegistryPathValue(profile_key.value(key), is_unicode);
            if (path_value.isEmpty()) {
                continue;
            }

            const QFileInfo fi(path_value);
            if (!fi.exists()) {
                continue;
            }

            files.append(classifyDataFileType(fi));
        }
    }
    return files;
}

}  // namespace

// ============================================================================
// Construction
// ============================================================================

EmailProfileManager::EmailProfileManager(QObject* parent) : QObject(parent) {}

// ============================================================================
// Public API
// ============================================================================

void EmailProfileManager::noteSettingsStatus(const QSettings& settings) {
    if (settings.status() != QSettings::NoError) {
        m_discovery_reliable = false;
    }
}

void EmailProfileManager::discoverProfiles() {
    if (m_operation_active.exchange(true)) {
        Q_EMIT errorOccurred(QStringLiteral("An email-profile operation is already in progress"));
        return;
    }
    const ScopedActiveFlag active_guard(m_operation_active);

    m_cancelled.store(false);
    m_discovery_reliable = true;
    m_profiles.clear();

    auto outlook = discoverOutlookProfiles();
    m_profiles.append(outlook);

    if (!m_cancelled.load()) {
        auto thunderbird = discoverThunderbirdProfiles();
        m_profiles.append(thunderbird);
    }

    if (!m_cancelled.load()) {
        auto windows_mail = discoverWindowsMailProfiles();
        m_profiles.append(windows_mail);
    }

    Q_EMIT profilesDiscovered(m_profiles);
}

void EmailProfileManager::backupProfiles(const QVector<int>& profile_indices,
                                         const QString& backup_path) {
    if (m_operation_active.exchange(true)) {
        Q_EMIT errorOccurred(QStringLiteral("An email-profile operation is already in progress"));
        return;
    }
    const ScopedActiveFlag active_guard(m_operation_active);

    // Fail closed instead of asserting: an empty path is caller/UI error but must
    // not abort the process in a release build or crash the debug test harness.
    if (backup_path.isEmpty()) {
        Q_EMIT errorOccurred(QStringLiteral("Backup path is empty"));
        return;
    }
    m_cancelled.store(false);
    m_backup_dest_names.clear();
    m_backup_reg_exports.clear();
    m_backup_failures = 0;

    const QDir dir(backup_path);
    if (!dir.mkpath(QStringLiteral("."))) {
        Q_EMIT errorOccurred(QStringLiteral("Failed to create backup directory"));
        return;
    }

    const int total_files = countTotalDataFiles(profile_indices);

    int files_done = 0;
    qint64 bytes_copied = 0;
    QVector<sak::EmailClientProfile> backed_up_profiles;

    for (const int idx : profile_indices) {
        if (m_cancelled.load()) {
            break;
        }
        if (idx < 0 || idx >= m_profiles.size()) {
            continue;
        }

        backed_up_profiles.append(m_profiles[idx]);
        backupSingleProfile(m_profiles[idx], backup_path, files_done, total_files, bytes_copied);
    }

    if (!createBackupManifest(backup_path, backed_up_profiles)) {
        // Without a manifest a restore has nothing to read, so the backup is not
        // usable: surface the failure and do NOT report completion (fail closed).
        sak::logWarning("Failed to create backup manifest in: {}", backup_path.toStdString());
        Q_EMIT errorOccurred(
            QStringLiteral("Failed to write backup manifest; backup is incomplete"));
        return;
    }
    // A partial backup (some exports/copies failed) still writes a manifest of the
    // items that DID succeed, but a caller that only watches backupComplete must not
    // read that as a fully successful backup: surface an aggregate failure first.
    if (m_backup_failures > 0) {
        Q_EMIT errorOccurred(QStringLiteral("Backup finished with %1 failed item(s); "
                                            "some profiles were not fully backed up")
                                 .arg(m_backup_failures));
    }
    Q_EMIT backupComplete(backup_path, files_done, bytes_copied);
}

int EmailProfileManager::countTotalDataFiles(const QVector<int>& profile_indices) const {
    int total = 0;
    for (const int idx : profile_indices) {
        if (idx >= 0 && idx < m_profiles.size()) {
            total += static_cast<int>(m_profiles[idx].data_files.size());
        }
    }
    return total;
}

void EmailProfileManager::backupSingleProfile(const sak::EmailClientProfile& profile,
                                              const QString& backup_path,
                                              int& files_done,
                                              int total_files,
                                              qint64& bytes_copied) {
    if (profile.client_type == sak::EmailClientType::Outlook && !profile.profile_path.isEmpty()) {
        // Sanitize the untrusted profile name to a bare filename so the .reg cannot
        // traverse out of the backup dir, and DEDUPE so two profiles that sanitize to
        // the same component do not overwrite one another's export. The manifest reuses
        // the exact on-disk basename we record below.
        const QString reg_file = uniqueRegistryBackupDestination(backup_path, profile.profile_name);
        if (exportRegistryKey(profile.profile_path, reg_file)) {
            // Only a successful export earns a registry_file entry in the manifest, keyed
            // by the (unique) profile path and pointing at the real deduped basename.
            m_backup_reg_exports.insert(profile.profile_path, QFileInfo(reg_file).fileName());
        } else {
            // A failed export is a backup failure, not silent success: surface it and
            // leave this profile without a registry_file so restore never chases a
            // missing .reg.
            ++m_backup_failures;
            sak::logWarning("Failed to export registry key: {}",
                            profile.profile_path.toStdString());
            Q_EMIT errorOccurred(QStringLiteral("Failed to export registry for profile: %1")
                                     .arg(profile.profile_name));
        }
    }

    for (const auto& data_file : profile.data_files) {
        if (m_cancelled.load()) {
            break;
        }

        const QFileInfo fi(data_file.path);
        if (!fi.exists()) {
            // A file discovery recorded is now gone: that is a backup gap, not a no-op.
            ++m_backup_failures;
            Q_EMIT errorOccurred(QStringLiteral("Backup source missing: %1").arg(data_file.path));
            continue;
        }

        // NOTE (inherent limitation): PST/OST/MBOX stores may be open in a live client
        // during this copy. Without a volume snapshot (VSS) or an exclusive lock -- both
        // out of scope for a user-space copy -- the copy of an actively-written store can
        // be internally inconsistent. QFile::copy still fails closed on any I/O error;
        // silent same-size in-place mutation of an open DB is the residual we accept.
        const QString dest = uniqueBackupDestination(backup_path, fi);
        if (!QFile::copy(data_file.path, dest)) {
            // A failed required copy is a backup failure, not silent success:
            // surface it and do not count it toward files_done or the manifest.
            ++m_backup_failures;
            Q_EMIT errorOccurred(QStringLiteral("Failed to back up: %1").arg(data_file.path));
            continue;
        }

        // Record the ACTUAL on-disk name (which may carry a collision suffix) so
        // the manifest points restore at the real file, not the original basename.
        m_backup_dest_names.insert(data_file.path, QFileInfo(dest).fileName());
        bytes_copied += fi.size();
        files_done++;
        Q_EMIT backupProgress(files_done, total_files, bytes_copied);
    }
}

QString EmailProfileManager::uniqueBackupDestination(const QString& backup_path,
                                                     const QFileInfo& source) {
    QString dest = backup_path + QLatin1Char('/') + source.fileName();
    int attempt = 1;
    while (QFile::exists(dest)) {
        const QString suffix_index = attempt > 1 ? QString::number(attempt) : QString();
        dest = backup_path + QLatin1Char('/') + source.completeBaseName() +
               QStringLiteral("_backup") + suffix_index + QLatin1Char('.') + source.suffix();
        ++attempt;
    }
    return dest;
}

QString EmailProfileManager::registryBackupFileName(const QString& profile_name) {
    return QStringLiteral("registry_") + sanitizeFileComponent(profile_name) +
           QStringLiteral(".reg");
}

QString EmailProfileManager::uniqueRegistryBackupDestination(const QString& backup_path,
                                                             const QString& profile_name) {
    const QString base = registryBackupFileName(profile_name);  // registry_<stem>.reg
    QString dest = backup_path + QLatin1Char('/') + base;
    // Insert a collision index before the ".reg" suffix so a second colliding profile
    // gets its own file instead of reg.exe export /y overwriting the first.
    const QString stem = base.left(base.size() - 4);  // drop trailing ".reg"
    int attempt = 1;
    while (QFile::exists(dest)) {
        ++attempt;
        dest = backup_path + QLatin1Char('/') + stem + QLatin1Char('_') + QString::number(attempt) +
               QStringLiteral(".reg");
    }
    return dest;
}

void EmailProfileManager::restoreProfiles(const QString& backup_manifest_path) {
    if (m_operation_active.exchange(true)) {
        Q_EMIT errorOccurred(QStringLiteral("An email-profile operation is already in progress"));
        return;
    }
    const ScopedActiveFlag active_guard(m_operation_active);

    m_cancelled.store(false);

    QJsonArray profiles;
    QString backup_dir;
    if (!readBackupManifest(backup_manifest_path, profiles, backup_dir)) {
        return;  // readBackupManifest already surfaced the specific reason (fail closed)
    }

    int restored = 0;
    for (const auto& prof_val : profiles) {
        if (m_cancelled.load()) {
            break;
        }
        // Count a profile only when it restored with no rejected/missing/registry/
        // copy failures, so "profiles restored" reflects reality (B7-29).
        if (restoreSingleProfile(prof_val.toObject(), backup_dir)) {
            restored++;
        }
    }

    Q_EMIT restoreComplete(restored);
}

bool EmailProfileManager::readBackupManifest(const QString& manifest_path,
                                             QJsonArray& out_profiles,
                                             QString& out_backup_dir) {
    QFile file(manifest_path);
    if (!file.open(QIODevice::ReadOnly)) {
        Q_EMIT errorOccurred(QStringLiteral("Failed to open backup manifest"));
        return false;
    }
    // A manifest is untrusted input. A real backup manifest is a few KiB of profile
    // metadata; cap the read so a hostile/corrupt file cannot force an unbounded
    // allocation + JSON parse (DoS). Fail closed above the cap.
    constexpr qint64 kMaxManifestBytes = 8LL * 1024 * 1024;
    if (file.size() > kMaxManifestBytes) {
        Q_EMIT errorOccurred(QStringLiteral("Backup manifest is too large"));
        return false;
    }
    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
    file.close();
    if (!doc.isObject()) {
        Q_EMIT errorOccurred(QStringLiteral("Invalid backup manifest format"));
        return false;
    }
    // Validate the schema fail-closed: never coerce a missing/wrong-typed field to a default. The
    // version must be exactly the supported integer, and "profiles" must be a real array (a
    // scalar/object silently coercing to [] would hide a corrupt or hostile manifest).
    const QJsonObject root = doc.object();
    const QJsonValue version = root[QStringLiteral("version")];
    if (!version.isDouble() || version.toInt() != kSupportedManifestVersion) {
        Q_EMIT errorOccurred(QStringLiteral("Unsupported or missing backup manifest version"));
        return false;
    }
    const QJsonValue profiles_val = root[QStringLiteral("profiles")];
    if (!profiles_val.isArray()) {
        Q_EMIT errorOccurred(QStringLiteral("Backup manifest has no profiles array"));
        return false;
    }
    out_profiles = profiles_val.toArray();
    out_backup_dir = QFileInfo(manifest_path).absolutePath();
    return true;
}

bool EmailProfileManager::restoreSingleProfile(const QJsonObject& prof, const QString& backup_dir) {
    const bool has_registry = !prof[QStringLiteral("registry_file")].toString().isEmpty();
    const QJsonArray files = prof[QStringLiteral("data_files")].toArray();

    // An empty/garbage profile object (no registry component and no data files) restores
    // nothing; counting it as "restored" would inflate the reported total (finding 7).
    if (!has_registry && files.isEmpty()) {
        return false;
    }

    // Restore data files BEFORE the registry import: a rejected/traversing file entry is
    // then caught before any (non-rollback-able) registry mutation runs, shrinking the
    // partial-state window (finding 7). Evaluate every file so all failures are logged.
    bool all_ok = true;
    const QString home_root = QDir::homePath();
    for (const auto& file_val : files) {
        if (!restoreOneDataFile(file_val.toObject(), backup_dir, home_root)) {
            all_ok = false;
        }
    }
    if (has_registry && !restoreRegistryFromManifest(prof, backup_dir)) {
        all_ok = false;
    }
    return all_ok;
}

bool EmailProfileManager::restoreRegistryFromManifest(const QJsonObject& prof,
                                                      const QString& backup_dir) {
    const QString reg_name = prof[QStringLiteral("registry_file")].toString();
    if (reg_name.isEmpty()) {
        return true;  // no registry component -> nothing to fail
    }
    // Confine the .reg to the backup directory: a manifest is untrusted input and
    // "../../payload.reg" would otherwise import an arbitrary registry file.
    const QString full_reg = resolveWithinDirectory(backup_dir, reg_name);
    if (full_reg.isEmpty()) {
        sak::logWarning("Rejected registry file escaping backup dir: {}", reg_name.toStdString());
        return false;
    }
    if (!QFile::exists(full_reg)) {
        // The manifest referenced a registry export that is missing from the backup.
        sak::logWarning("Registry file missing from backup: {}", full_reg.toStdString());
        return false;
    }
    if (!importRegistryKey(full_reg)) {
        sak::logWarning("Failed to import registry key: {}", full_reg.toStdString());
        return false;
    }
    return true;
}

bool EmailProfileManager::restoreOneDataFile(const QJsonObject& file_obj,
                                             const QString& backup_dir,
                                             const QString& home_root) {
    const QString original = file_obj[QStringLiteral("original_path")].toString();
    const QString backed_up = file_obj[QStringLiteral("backed_up_name")].toString();

    // Source must be a bare name inside the backup dir; destination must be an
    // absolute path confined to the user's home tree.
    const QString source = resolveWithinDirectory(backup_dir, backed_up);
    if (source.isEmpty()) {
        sak::logWarning("Rejected backup source escaping backup dir: {}", backed_up.toStdString());
        return false;
    }
    // The bare-name check above is lexical only: a source file that is itself a
    // junction/symlink would still redirect the read outside the backup dir. Require
    // the resolved source to physically reside inside the backup dir (reparse points
    // followed), mirroring the destination-root confinement.
    if (!destinationWithinRoot(backup_dir, source)) {
        sak::logWarning("Rejected backup source resolving outside backup dir: {}",
                        source.toStdString());
        return false;
    }
    if (!destinationWithinRoot(home_root, original)) {
        sak::logWarning("Rejected restore destination outside user home: {}",
                        original.toStdString());
        return false;
    }
    // Home containment alone is not enough: the home tree still holds sensitive spots
    // (e.g. %APPDATA%\...\Startup, normally empty) where a NEW .lnk/.cmd yields logon
    // code execution. Confine the restore to the email data-file types this tool backs
    // up so a crafted manifest cannot create an arbitrary executable/shortcut (finding 1).
    if (!isRestorableDataFile(original)) {
        sak::logWarning("Rejected restore destination with non-email file type: {}",
                        original.toStdString());
        return false;
    }
    if (!QFile::exists(source)) {
        // A file the manifest says was backed up is gone from the backup.
        sak::logWarning("Backup source missing: {}", source.toStdString());
        return false;
    }
    if (QFile::exists(original)) {
        return true;  // already present -> intentional non-overwrite skip, not a failure
    }
    QDir().mkpath(QFileInfo(original).absolutePath());
    if (!QFile::copy(source, original)) {
        sak::logWarning("Failed to restore file: {}", original.toStdString());
        return false;
    }
    return true;
}

bool EmailProfileManager::destinationWithinRoot(const QString& root, const QString& candidate) {
    if (candidate.isEmpty()) {
        return false;
    }
    // Real root, resolving any junction/symlink in the home path itself.
    QString root_real = realCanonicalPath(root);
    if (root_real.isEmpty()) {
        root_real = QDir::cleanPath(QDir(root).absolutePath());
    }

    // A lexical cleanPath collapses "..", but a JUNCTION under the root would still pass a purely
    // textual prefix check while redirecting the write elsewhere. So fully resolve the deepest
    // EXISTING ancestor (the leaf may not exist yet) through every reparse point -- that is the
    // component a junction redirects -- and require IT to sit inside the real root.
    const QString abs = QDir::cleanPath(QFileInfo(candidate).absoluteFilePath());
    const QString existing = deepestExistingAncestor(abs);
    if (existing.isEmpty()) {
        return false;
    }
    const QString existing_real = realCanonicalPath(existing);
    if (existing_real.isEmpty()) {
        return false;
    }

    QString root_slash = root_real;
    if (!root_slash.endsWith(QLatin1Char('/'))) {
        root_slash += QLatin1Char('/');
    }
    return existing_real.compare(root_real, Qt::CaseInsensitive) == 0 ||
           (existing_real + QLatin1Char('/')).startsWith(root_slash, Qt::CaseInsensitive);
}

QSet<QString> EmailProfileManager::linkedFilePaths() const {
    QSet<QString> paths;
    for (const auto& profile : m_profiles) {
        for (const auto& data_file : profile.data_files) {
            paths.insert(
                QDir::toNativeSeparators(QFileInfo(data_file.path).absoluteFilePath().toLower()));
        }
    }
    return paths;
}

void EmailProfileManager::cancel() {
    m_cancelled.store(true);
}

// ============================================================================
// Outlook Discovery
// ============================================================================

QVector<sak::EmailClientProfile> EmailProfileManager::discoverOutlookProfiles() {
    QVector<sak::EmailClientProfile> results;

    for (const auto& version : kOutlookVersions) {
        if (m_cancelled.load()) {
            break;
        }

        const QString reg_path = kOutlookProfilesPath.arg(version);

        // Use QSettings to read the registry
        QSettings registry(QStringLiteral("HKEY_CURRENT_USER\\Software\\Microsoft"
                                          "\\Office\\%1\\Outlook\\Profiles")
                               .arg(version),
                           QSettings::NativeFormat);

        const QStringList profile_names = registry.childGroups();
        noteSettingsStatus(registry);  // a failed key read must not read as "no profiles"
        if (profile_names.isEmpty()) {
            continue;
        }

        sak::logInfo("ProfileManager: Found {} Outlook {} profiles",
                     profile_names.size(),
                     version.toStdString());

        // Determine which is the default profile
        const QSettings outlook_key(QStringLiteral("HKEY_CURRENT_USER\\Software\\Microsoft"
                                                   "\\Office\\%1\\Outlook")
                                        .arg(version),
                                    QSettings::NativeFormat);
        const QString default_profile =
            outlook_key.value(QStringLiteral("DefaultProfile"), QString()).toString();

        for (const auto& name : profile_names) {
            sak::EmailClientProfile profile;
            profile.client_type = sak::EmailClientType::Outlook;
            profile.client_name = QStringLiteral("Microsoft Outlook ") + version;
            profile.client_version = version;
            profile.profile_name = name;
            profile.profile_path = reg_path + QLatin1Char('\\') + name;

            // Dig into profile subkeys for data files
            registry.beginGroup(name);
            profile.data_files = findOutlookDataFiles(registry);
            registry.endGroup();

            sak::logInfo("ProfileManager: Profile '{}' has {} data files",
                         name.toStdString(),
                         profile.data_files.size());

            // Calculate total size
            for (const auto& df : profile.data_files) {
                profile.total_size_bytes += df.size_bytes;
            }

            results.append(profile);
        }
    }

    discoverWmsProfiles(results);

    return results;
}

void EmailProfileManager::discoverWmsProfiles(QVector<sak::EmailClientProfile>& results) {
    QSettings wms(kWmsProfilesPath, QSettings::NativeFormat);
    const QStringList wms_profiles = wms.childGroups();
    noteSettingsStatus(wms);
    for (const auto& name : wms_profiles) {
        if (m_cancelled.load()) {
            break;
        }

        bool already_found = false;
        for (const auto& existing : results) {
            if (existing.profile_name == name) {
                already_found = true;
                break;
            }
        }
        if (already_found) {
            continue;
        }

        sak::EmailClientProfile profile;
        profile.client_type = sak::EmailClientType::Outlook;
        profile.client_name = QStringLiteral("Windows Messaging Subsystem");
        profile.profile_name = name;
        profile.profile_path = kWmsProfilesPath + QLatin1Char('\\') + name;

        wms.beginGroup(name);
        profile.data_files = findOutlookDataFiles(wms);
        wms.endGroup();

        for (const auto& df : profile.data_files) {
            profile.total_size_bytes += df.size_bytes;
        }

        if (!profile.data_files.isEmpty()) {
            results.append(profile);
        }
    }
}

// ============================================================================
// Thunderbird Discovery
// ============================================================================

QVector<sak::EmailClientProfile> EmailProfileManager::discoverThunderbirdProfiles() {
    QVector<sak::EmailClientProfile> results;

    // Thunderbird stores profiles in %APPDATA%\Thunderbird\profiles.ini
    const QString appdata = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation);
    const QString tb_dir = QDir::homePath() + QStringLiteral("/AppData/Roaming/Thunderbird");
    const QString profiles_ini = tb_dir + QStringLiteral("/profiles.ini");

    if (!QFile::exists(profiles_ini)) {
        return results;
    }

    QSettings ini(profiles_ini, QSettings::IniFormat);
    const QStringList groups = ini.childGroups();
    noteSettingsStatus(ini);  // a present-but-corrupt profiles.ini must not read as "no profiles"

    for (const auto& group : groups) {
        if (m_cancelled.load()) {
            break;
        }
        if (!group.startsWith(QStringLiteral("Profile"), Qt::CaseInsensitive)) {
            continue;
        }

        ini.beginGroup(group);
        QString name = ini.value(QStringLiteral("Name"), QStringLiteral("Default")).toString();
        const QString path = ini.value(QStringLiteral("Path")).toString();
        bool is_relative_ok = false;
        const int is_relative_raw =
            ini.value(QStringLiteral("IsRelative"), 1).toInt(&is_relative_ok);
        ini.endGroup();

        // A malformed IsRelative must not silently coerce to 0 (absolute) and let a crafted
        // profiles.ini point Path at an out-of-tree directory: fail closed and skip the profile.
        if (!is_relative_ok) {
            sak::logWarning("Skipping Thunderbird profile with malformed IsRelative: {}",
                            group.toStdString());
            continue;
        }
        const QString full_path = thunderbirdProfileDir(tb_dir, path, is_relative_raw != 0);
        if (full_path.isEmpty()) {
            sak::logWarning("Skipping Thunderbird profile whose relative Path escapes the root: {}",
                            path.toStdString());
            continue;
        }

        sak::EmailClientProfile profile;
        profile.client_type = sak::EmailClientType::Thunderbird;
        profile.client_name = QStringLiteral("Mozilla Thunderbird");
        profile.profile_name = name;
        profile.profile_path = full_path;

        // Scan profile directory for MBOX files (no extension in TB)
        // and also look for .msf (index) files alongside mbox
        const QDir profile_dir(full_path);
        if (profile_dir.exists()) {
            scanThunderbirdDir(profile_dir, profile.data_files);
        }

        for (const auto& df : profile.data_files) {
            profile.total_size_bytes += df.size_bytes;
        }

        if (!profile.data_files.isEmpty()) {
            results.append(profile);
        }
    }

    return results;
}

// ============================================================================
// Windows Mail Discovery
// ============================================================================

QVector<sak::EmailClientProfile> EmailProfileManager::discoverWindowsMailProfiles() {
    QVector<sak::EmailClientProfile> results;

    // Windows Mail / Windows Live Mail stores data in
    // %LOCALAPPDATA%\Microsoft\Windows Mail
    // or %LOCALAPPDATA%\Microsoft\Windows Live Mail
    const QStringList mail_dirs = {
        QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) +
            QStringLiteral("/Microsoft/Windows Mail"),
        QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) +
            QStringLiteral("/Microsoft/Windows Live Mail"),
    };

    for (const auto& mail_dir : mail_dirs) {
        if (m_cancelled.load()) {
            break;
        }

        const QDir dir(mail_dir);
        if (!dir.exists()) {
            continue;
        }

        sak::EmailClientProfile profile;
        profile.client_type = sak::EmailClientType::WindowsMail;
        profile.client_name = mail_dir.contains(QStringLiteral("Live"))
                                  ? QStringLiteral("Windows Live Mail")
                                  : QStringLiteral("Windows Mail");
        profile.profile_name = QStringLiteral("Default");
        profile.profile_path = mail_dir;

        // Scan for .eml files
        const QStringList filters = {
            QStringLiteral("*.eml"),
            QStringLiteral("*.pst"),
        };
        const QFileInfoList file_list = dir.entryInfoList(filters, QDir::Files, QDir::Name);

        for (const auto& fi : file_list) {
            sak::EmailDataFile data_file;
            data_file.path = fi.absoluteFilePath();
            data_file.type = fi.suffix().toUpper();
            data_file.size_bytes = fi.size();
            data_file.is_linked = true;
            profile.data_files.append(data_file);
        }

        for (const auto& df : profile.data_files) {
            profile.total_size_bytes += df.size_bytes;
        }

        if (!profile.data_files.isEmpty()) {
            results.append(profile);
        }
    }

    return results;
}

// ============================================================================
// Registry Export/Import
// ============================================================================

bool EmailProfileManager::exportRegistryKey(const QString& key_path, const QString& output_file) {
    // System32-qualified reg.exe, never the bare name: CreateProcess searches the current
    // directory ahead of System32, so a planted reg.exe would run with our token. Fail
    // closed when it cannot be resolved rather than export via a PATH-found binary.
    const QString reg_exe = sak::system32Path(QStringLiteral("reg.exe"));
    if (reg_exe.isEmpty()) {
        sak::logWarning("Registry export: cannot resolve the System32 reg.exe path");
        return false;
    }
    // Export into a private, unpredictably-named staging dir first, then place the file at the
    // caller's collision-free destination with a NON-overwriting copy. The backup dir may be
    // attacker-writable; exporting straight there with reg.exe /y would let a process that wins the
    // race between uniqueRegistryBackupDestination's existence check and this write substitute a
    // file or reparse target at the name. A planted file/junction now makes QFile::copy fail (it
    // never overwrites), so we fail closed instead of redirecting the export elsewhere.
    const QTemporaryDir staging;
    if (!staging.isValid()) {
        sak::logWarning("Registry export: could not create a private staging directory");
        return false;
    }
    const QString staged = staging.path() + QStringLiteral("/export.reg");
    const auto result =
        sak::runProcess(reg_exe,
                        {QStringLiteral("export"), key_path, staged, QStringLiteral("/y")},
                        kRegExportTimeoutMs);
    if (!result.succeeded()) {
        return false;
    }
    if (QFile::exists(output_file) || !QFile::copy(staged, output_file)) {
        sak::logWarning("Registry export: could not place staged .reg at {}",
                        output_file.toStdString());
        return false;
    }
    return true;
}

QString EmailProfileManager::decodeRegFile(const QByteArray& bytes) {
    // reg.exe export writes UTF-16LE with a BOM (0xFF 0xFE). Older REGEDIT4 files are ANSI.
    if (bytes.size() >= kUtf16BomLength &&
        static_cast<unsigned char>(bytes[0]) == kUtf16LeBomByte0 &&
        static_cast<unsigned char>(bytes[1]) == kUtf16LeBomByte1) {
        return QString::fromUtf16(reinterpret_cast<const char16_t*>(bytes.constData() +
                                                                    kUtf16BomLength),
                                  (bytes.size() - kUtf16BomLength) / kBytesPerUtf16CodeUnit);
    }
    return QString::fromUtf8(bytes);
}

bool EmailProfileManager::regKeyPathAllowed(const QString& key_path) {
    const QString upper = key_path.trimmed().toUpper();
    // The HKCU Outlook profile subtree a backup legitimately exports:
    // Office\<ver>\Outlook\Profiles.
    // '\OUTLOOK\PROFILES' must match as a whole path segment (not a bare substring), so
    // '...\OUTLOOK\PROFILES_EVIL' / '...\OUTLOOK\PROFILESHACK' are refused. Blocks HKLM entirely
    // and every other HKCU location (Run/RunOnce persistence, shell hooks, etc.).
    if (upper.startsWith(kOfficeKeyPrefix) &&
        containsPathSegment(upper, QStringLiteral("\\OUTLOOK\\PROFILES"))) {
        return true;
    }
    // The Windows Messaging Subsystem profiles subtree: discoverWmsProfiles exports WMS profiles as
    // Outlook, so restore must accept this hive too, matched as a whole segment (finding 6).
    return startsWithPathSegment(upper, kWmsKeyPrefix);
}

bool EmailProfileManager::regContentConfinedToEmailHives(const QString& reg_text) {
    int key_sections = 0;
    const QStringList lines = reg_text.split(QLatin1Char('\n'));
    for (const QString& raw : lines) {
        const QString line = raw.trimmed();
        // Key section headers are the only lines that name a registry path.
        if (!line.startsWith(QLatin1Char('[')) || !line.endsWith(QLatin1Char(']'))) {
            continue;
        }
        const QString key = line.mid(1, line.size() - kKeySectionBracketChars).trimmed();
        if (key.startsWith(QLatin1Char('-'))) {
            // [-HKEY...] is a destructive key DELETION. A genuine `reg export` never emits one, so
            // a backup that contains it is crafted/corrupt: refuse the whole file rather than apply
            // a deletion, even one confined to the Outlook subtree (finding 4).
            sak::logWarning("Refused .reg import (contains a key deletion): {}", key.toStdString());
            return false;
        }
        ++key_sections;
        if (!regKeyPathAllowed(key)) {
            return false;
        }
    }
    // A legitimate export always contains at least one key; zero keys means it is not a real
    // Outlook profile export, so refuse it rather than shelling out to reg.exe on unknown content.
    return key_sections > 0;
}

bool EmailProfileManager::importRegistryKey(const QString& reg_file) {
    QFile f(reg_file);
    if (!f.open(QIODevice::ReadOnly)) {
        sak::logWarning("Registry import: cannot read {}", reg_file.toStdString());
        return false;
    }
    const QByteArray bytes = f.readAll();
    f.close();

    // reg.exe export always writes UTF-16LE with a BOM (0xFF 0xFE). Require that exact encoding so
    // our confinement scan decodes the bytes the SAME way reg.exe will on import. A non-BOM
    // (REGEDIT4/ANSI) file would be read as UTF-8 by decodeRegFile but as ANSI by reg.exe, and a
    // high-bit byte could hide a disallowed key from the scan that the import would still apply.
    // Our own exportRegistryKey never produces a non-BOM file, so this rejects only crafted input.
    if (bytes.size() < kUtf16BomLength ||
        static_cast<unsigned char>(bytes[0]) != kUtf16LeBomByte0 ||
        static_cast<unsigned char>(bytes[1]) != kUtf16LeBomByte1) {
        sak::logWarning("Refused .reg import (not UTF-16LE-with-BOM as reg.exe export writes): {}",
                        reg_file.toStdString());
        return false;
    }

    // A backup is untrusted input. reg.exe import will write EVERY key in the file, so before we
    // run it, confirm the .reg touches only the Outlook profile subtree. Otherwise a crafted backup
    // could import Run-key persistence or overwrite arbitrary HKLM/HKCU settings.
    if (!regContentConfinedToEmailHives(decodeRegFile(bytes))) {
        sak::logWarning("Refused .reg import (keys outside the Outlook profile subtree): {}",
                        reg_file.toStdString());
        return false;
    }

    // Import from a private temp copy of the bytes we JUST validated, not from the backup
    // path. reg.exe re-opens the file it is given, so importing the backup path directly
    // leaves a TOCTOU window: an attacker with write access to the backup dir could swap
    // the .reg between our validation read and reg.exe's read. The private temp has an
    // unpredictable name outside the attacker-writable backup dir, so what we validated is
    // exactly what gets imported (finding 3).
    return importValidatedBytes(bytes);
}

bool EmailProfileManager::importValidatedBytes(const QByteArray& bytes) {
    // Same System32 qualification as the export side: a registry IMPORT is the more
    // dangerous direction, so refuse outright when reg.exe cannot be resolved.
    const QString reg_exe = sak::system32Path(QStringLiteral("reg.exe"));
    if (reg_exe.isEmpty()) {
        sak::logWarning("Registry import: cannot resolve the System32 reg.exe path");
        return false;
    }
    const QTemporaryDir staging;
    if (!staging.isValid()) {
        sak::logWarning("Registry import: could not create a private staging directory");
        return false;
    }
    const QString staged = staging.path() + QStringLiteral("/import.reg");
    QFile out(staged);
    if (!out.open(QIODevice::WriteOnly)) {
        sak::logWarning("Registry import: could not write staged copy {}", staged.toStdString());
        return false;
    }
    if (!sak::writeFully(out, bytes)) {
        out.close();
        sak::logWarning("Registry import: truncated staged copy {}", staged.toStdString());
        return false;
    }
    out.close();
    const auto result =
        sak::runProcess(reg_exe, {QStringLiteral("import"), staged}, kRegImportTimeoutMs);
    return result.succeeded();
}

// ============================================================================
// Backup Manifest
// ============================================================================

bool EmailProfileManager::createBackupManifest(const QString& backup_path,
                                               const QVector<sak::EmailClientProfile>& profiles) {
    QJsonArray profiles_array;
    for (const auto& profile : profiles) {
        QJsonObject prof;
        prof[QStringLiteral("client_name")] = profile.client_name;
        prof[QStringLiteral("profile_name")] = profile.profile_name;
        prof[QStringLiteral("profile_path")] = profile.profile_path;

        prof[QStringLiteral("client_type")] = clientTypeManifestName(profile.client_type);

        // Registry file reference -- only when the export actually succeeded, using the
        // EXACT deduped basename backupSingleProfile recorded, so restore never chases a
        // missing/mis-named .reg or another profile's colliding export.
        const auto reg_it = m_backup_reg_exports.constFind(profile.profile_path);
        if (profile.client_type == sak::EmailClientType::Outlook &&
            reg_it != m_backup_reg_exports.constEnd()) {
            prof[QStringLiteral("registry_file")] = reg_it.value();
        }

        QJsonArray files_array;
        for (const auto& df : profile.data_files) {
            // Only manifest files that were actually copied; the recorded name is
            // the real on-disk basename (with any collision suffix), so restore
            // never points at a missing file or another profile's copy.
            const auto dest_it = m_backup_dest_names.constFind(df.path);
            if (dest_it == m_backup_dest_names.constEnd()) {
                continue;
            }
            QJsonObject file_obj;
            file_obj[QStringLiteral("original_path")] = df.path;
            file_obj[QStringLiteral("type")] = df.type;
            file_obj[QStringLiteral("size_bytes")] = df.size_bytes;
            file_obj[QStringLiteral("backed_up_name")] = dest_it.value();
            files_array.append(file_obj);
        }
        prof[QStringLiteral("data_files")] = files_array;

        profiles_array.append(prof);
    }

    QJsonObject root;
    root[QStringLiteral("version")] = kSupportedManifestVersion;
    root[QStringLiteral("created")] = QDateTime::currentDateTime().toString(Qt::ISODate);
    root[QStringLiteral("tool")] = QStringLiteral("SAK Utility");
    root[QStringLiteral("profiles")] = profiles_array;

    QFile file(backup_path + QStringLiteral("/backup_manifest.json"));
    if (!file.open(QIODevice::WriteOnly)) {
        return false;
    }
    if (!sak::writeFully(file, QJsonDocument(root).toJson(QJsonDocument::Indented))) {
        // A truncated manifest cannot be trusted to restore; fail closed.
        file.close();
        return false;
    }
    file.close();
    return true;
}

// ============================================================================
// Helper: Scan Thunderbird directory for MBOX files
// ============================================================================

QString EmailProfileManager::thunderbirdProfileDir(const QString& tb_dir,
                                                   const QString& path,
                                                   bool is_relative) {
    if (!is_relative) {
        // Absolute Path is a supported Thunderbird configuration (profile stored elsewhere);
        // it is the user's own choice, not confined to the TB root.
        return path;
    }
    const QString full = QDir(tb_dir).absoluteFilePath(path);
    // Confine a relative Path beneath the TB root so a tampered 'Path=../../..' cannot redirect the
    // scan/backup to an unrelated tree. destinationWithinRoot resolves '..' and reparse points.
    if (!destinationWithinRoot(tb_dir, full)) {
        return {};
    }
    return full;
}

void EmailProfileManager::scanThunderbirdDir(const QDir& dir, QVector<sak::EmailDataFile>& files) {
    constexpr int kMaxRecursionDepth = 20;
    scanThunderbirdDirRecursive(dir, files, 0, kMaxRecursionDepth);
}

void EmailProfileManager::scanThunderbirdDirRecursive(const QDir& dir,
                                                      QVector<sak::EmailDataFile>& files,
                                                      int depth,
                                                      int max_depth) {
    // Fail closed against memory exhaustion: once the collected-file budget is reached, stop
    // descending and stop collecting rather than growing the vector without bound.
    if (depth >= max_depth || m_cancelled.load() || files.size() >= kMaxThunderbirdDataFiles) {
        return;
    }

    // Thunderbird MBOX files have no extension and sit alongside .msf files
    const QFileInfoList entries =
        dir.entryInfoList(QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot);

    for (const auto& entry : entries) {
        if (m_cancelled.load() || files.size() >= kMaxThunderbirdDataFiles) {
            break;
        }

        if (entry.isDir()) {
            scanThunderbirdDirRecursive(
                QDir(entry.absoluteFilePath()), files, depth + 1, max_depth);
            continue;
        }

        // An MBOX file is identified by having a companion .msf file
        const QString path = entry.absoluteFilePath();
        if (entry.suffix().isEmpty() && QFile::exists(path + QStringLiteral(".msf"))) {
            sak::EmailDataFile data_file;
            data_file.path = path;
            data_file.type = QStringLiteral("MBOX");
            data_file.size_bytes = entry.size();
            data_file.is_linked = true;
            files.append(data_file);
        }
    }
}
