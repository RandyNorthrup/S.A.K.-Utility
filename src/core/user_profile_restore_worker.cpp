// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include "sak/user_profile_restore_worker.h"

#include "sak/backup_file_codec.h"
#include "sak/layout_constants.h"
#include "sak/logger.h"
#include "sak/path_utils.h"
#include "sak/permission_manager.h"
#include "sak/process_runner.h"
#include "sak/smart_file_filter.h"
#include "sak/windows_user_scanner.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRandomGenerator>
#include <QTemporaryFile>
#include <QtGlobal>

#include <algorithm>
#include <limits>
#include <optional>
#include <vector>

#include <winsock2.h>
#include <ws2tcpip.h>

#include <windows.h>

#include <iphlpapi.h>

// Link against IP Helper API (structural, locale-independent DHCP-state query).
#pragma comment(lib, "iphlpapi.lib")
#pragma comment(lib, "ws2_32.lib")

namespace sak {

namespace {
constexpr int kRestoreProgressFileInterval = 100;
constexpr qint64 kRestoreProgressByteInterval = kRestoreProgressFileInterval * kBytesPerMB;
constexpr int kRestoreConflictMaxAttempts = 1000;
constexpr int kNetshTimeoutMs = 30'000;
constexpr int kHexBase = 16;  // base for the random staging-token suffix
constexpr unsigned long kAdapterEnumInitialBytes = 15'000;

// Structural, locale-independent DHCP-enabled query for an adapter by its friendly
// name, via the IP Helper API. Returns nullopt when enumeration fails or the
// adapter is not present (so callers never treat "unknown" as "already DHCP").
std::optional<bool> adapterDhcpEnabled(const QString& adapterName) {
    constexpr ULONG kFlags = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST |
                             GAA_FLAG_SKIP_DNS_SERVER;
    ULONG size = kAdapterEnumInitialBytes;
    std::vector<uint8_t> buffer(size);
    ULONG rc = GetAdaptersAddresses(
        AF_UNSPEC, kFlags, nullptr, reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data()), &size);
    if (rc == ERROR_BUFFER_OVERFLOW) {
        buffer.resize(size);
        rc = GetAdaptersAddresses(AF_UNSPEC,
                                  kFlags,
                                  nullptr,
                                  reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data()),
                                  &size);
    }
    if (rc != NO_ERROR) {
        return std::nullopt;
    }
    for (auto* addr = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data()); addr != nullptr;
         addr = addr->Next) {
        if (QString::fromWCharArray(addr->FriendlyName).compare(adapterName, Qt::CaseInsensitive) ==
            0) {
            return (addr->Flags & IP_ADAPTER_DHCP_ENABLED) != 0;
        }
    }
    return std::nullopt;
}

bool buildSafePath(const QString& basePath, const QString& relativePath, QString& outPath) {
    // This is the containment guard for every restore path, so an empty input is
    // REJECTED rather than asserted. An assert would abort a Debug build and
    // vanish entirely in Release, leaving QDir("").filePath("") to produce an
    // empty path that the containment check could accept. Refusing here fails
    // closed in every configuration.
    if (basePath.isEmpty() || relativePath.isEmpty()) {
        return false;
    }
    const QString combined = QDir(basePath).filePath(relativePath);
    auto nativeCombined = QDir::toNativeSeparators(combined);
    auto nativeBase = QDir::toNativeSeparators(basePath);

    // Build the filesystem paths from UTF-16, not a lossy narrow (system-ANSI)
    // std::string: toStdString() would mangle any non-ASCII path component and
    // could make the containment check compare corrupted bytes.
    const std::filesystem::path destPath(nativeCombined.toStdU16String());
    const std::filesystem::path base(nativeBase.toStdU16String());

    auto safe = path_utils::isSafePath(destPath, base);
    if (!safe || !(*safe)) {
        return false;
    }

    outPath = combined;
    return true;
}

// True if the path is a reparse point (junction/symlink), on either the backup
// source or the restore destination side.
bool isReparsePoint(const QFileInfo& info) {
    return info.isSymLink() || info.isJunction();
}

// A junction or symlink on either side lets the copy read from outside the
// backup root or write outside the validated profile root; refuse to traverse.
bool restoreEntryEscapesRoot(const QFileInfo& sourceEntry, const QString& destItem) {
    return isReparsePoint(sourceEntry) || isReparsePoint(QFileInfo(destItem));
}

// True when destItem's realized PARENT directory (junctions/symlinks followed) is at or under
// the canonical profile root. The leaf reparse check catches destItem itself, but an ANCESTOR
// directory that is (or becomes) a junction after mkpath would redirect the write outside the
// root while the leaf still looks clean; re-checking the parent on every entry catches that.
// Fails closed on an empty root or an unresolvable parent. canonicalFilePath yields '/'-paths,
// so the boundary test uses '/' and a sibling-prefix (Root vs RootX) is rejected.
bool destinationParentWithinRoot(const QString& canonicalRoot, const QString& destItem) {
    if (canonicalRoot.isEmpty()) {
        return false;
    }
    const QString canonicalParent =
        QFileInfo(QFileInfo(destItem).absolutePath()).canonicalFilePath();
    if (canonicalParent.isEmpty()) {
        return false;
    }
    return canonicalParent == canonicalRoot ||
           canonicalParent.startsWith(canonicalRoot + QLatin1Char('/'));
}

// Save the pre-restore contents of an about-to-be-overwritten file to a
// recoverable sidecar so the user can undo the overwrite. Returns false (fail
// closed) if the copy fails, so the caller never destroys the original without
// a recovery copy in hand.
bool createRestoreRecoveryCopy(const QString& finalDestPath) {
    const QString recoveryPath = finalDestPath + QStringLiteral(".sakbak");
    QFile::remove(recoveryPath);  // Drop a stale copy from an earlier restore.
    return QFile::copy(finalDestPath, recoveryPath);
}

// An unpredictable per-operation staging path in the destination directory. The internal
// temp/old paths must not be guessable: a fixed suffix let a local attacker pre-plant a
// dangling symlink at the known path in the window between QFile::remove and QFile::copy, so
// the copy would follow it and write outside the profile root. A 64-bit random token closes
// that (QFile::copy still refuses to overwrite an existing target = fail closed).
QString makeRestoreTempPath(const QString& finalDestPath, const QString& tag) {
    const quint64 token = QRandomGenerator::global()->generate64();
    return finalDestPath + QStringLiteral(".sak-") + tag + QLatin1Char('-') +
           QString::number(token, kHexBase) + QStringLiteral(".tmp");
}

// Replace an existing destination file only after the new copy is fully written.
// The original is moved aside by RENAME (never deleted) before the replacement is
// swapped in, and restored if the swap fails, so no failure can lose the original
// destination. When backupExisting is set, the original is also preserved to a
// `.sakbak` recovery copy for user-visible undo.
// Put the backup payload at `target`. A verbatim copy stays a copy; a codec container is
// decoded (decompressed and/or decrypted). readBackupFile stages and verifies before it
// publishes, so a wrong password or a corrupted container leaves no file at `target` --
// which keeps the replace-by-rename logic below correct either way.
bool materializeBackupPayload(const QString& source,
                              const QString& target,
                              const QString& password) {
    if (backupContainerKind(source) == BackupContainerKind::None) {
        return QFile::copy(source, target);
    }
    return readBackupFile(source, target, password).has_value();
}

bool copyFileReplacingExisting(const QString& source,
                               const QString& finalDestPath,
                               bool backupExisting,
                               const QString& password) {
    if (!QFileInfo::exists(finalDestPath)) {
        return materializeBackupPayload(source, finalDestPath, password);
    }
    const QString tempPath = makeRestoreTempPath(finalDestPath, QStringLiteral("restore"));
    QFile::remove(tempPath);
    if (!materializeBackupPayload(source, tempPath, password)) {
        return false;
    }
    if (backupExisting && !createRestoreRecoveryCopy(finalDestPath)) {
        QFile::remove(tempPath);
        return false;  // Never overwrite without a recovery copy in place.
    }

    // Move the ORIGINAL aside by rename (not delete). QFile::rename cannot overwrite an existing
    // target, so the destination must be vacated first -- but by relocating the original, not
    // destroying it, so a failed swap can roll back. The old code removed the original THEN
    // renamed, and a rename failure between the two left the destination gone.
    const QString oldPath = makeRestoreTempPath(finalDestPath, QStringLiteral("old"));
    QFile::remove(oldPath);
    if (!QFile::rename(finalDestPath, oldPath)) {
        QFile::remove(tempPath);
        return false;  // Could not move the original aside -- leave everything intact.
    }
    if (!QFile::rename(tempPath, finalDestPath)) {
        // Roll back: move the original back into place. If even the rollback fails, do
        // NOT delete oldPath -- the original stays there for manual recovery rather
        // than being silently lost, and the failure is surfaced.
        if (!QFile::rename(oldPath, finalDestPath)) {
            sak::logError(
                "Restore rollback failed: original preserved at '{}' (could not move back to '{}')",
                oldPath.toStdString(),
                finalDestPath.toStdString());
            QFile::remove(tempPath);
            return false;
        }
        QFile::remove(tempPath);
        return false;
    }
    QFile::remove(oldPath);  // Replacement is in place; drop the moved-aside original.
    return true;
}
}  // namespace

UserProfileRestoreWorker::UserProfileRestoreWorker(QObject* parent)
    : QThread(parent)
    , m_fileFilter(new SmartFileFilter())
    , m_permissionManager(new PermissionManager()) {}

UserProfileRestoreWorker::~UserProfileRestoreWorker() {
    if (isRunning()) {
        cancel();
        // A large in-flight QFile::copy cannot observe cancellation, so the
        // bounded wait can time out. Block until the thread has fully exited
        // rather than destroying a running QThread (which aborts the process).
        if (!wait(kTimeoutThreadTerminateMs)) {
            // SAK-ALLOW-BLOCKING: the bounded wait above already gave up once. Destroying a
            // running QThread aborts the process, and terminate() during a restore copy can
            // leave a half-written file on the target profile, so blocking is safer.
            wait();
        }
    }
    delete m_fileFilter;
    delete m_permissionManager;
}

bool UserProfileRestoreWorker::startRestore(const QString& backupPath,
                                            const BackupManifest& manifest,
                                            const QVector<UserMapping>& mappings,
                                            const RestoreConfig& config,
                                            const RestoreSelections& selections) {
    // No asserts on backupPath or mappings. An empty backupPath is rejected
    // downstream (buildSafePath fails, and the manifest read reports the missing
    // file), and an empty mapping set is a restore of nothing, which completes
    // with a zero-file summary. Asserting either turned input that Release
    // handles into a Debug-only process abort, which is what made this worker's
    // own test suite impossible to run in Debug.
    // Refuse up front rather than failing file by file. Without the password every file
    // would fail to decode, and the run would report hundreds of errors instead of the
    // one thing that is actually wrong.
    if (manifest.encrypted && config.password.isEmpty()) {
        Q_EMIT logMessage(tr("This backup is encrypted; a password is required"), true);
        Q_EMIT restoreComplete(false, tr("A password is required to restore this backup"));
        return false;
    }

    bool refused = false;
    {
        const QMutexLocker locker(&m_mutex);
        // The busy test lives UNDER the mutex: outside it, two concurrent callers could both
        // see "not running" and then both overwrite the configuration that a just-started
        // run() is already reading. m_configured additionally covers the window between
        // start() and run()'s first line, where the thread is armed but not yet running.
        if (isRunning() || m_configured.load()) {
            refused = true;
        } else {
            m_backupPath = backupPath;
            m_manifest = manifest;
            m_mappings = mappings;
            m_wifiProfiles = selections.wifi_profiles;
            m_ethernetConfigs = selections.ethernet_configs;
            m_appDataSources = selections.app_data_sources;
            m_conflictMode = config.conflict_mode;
            m_permissionMode = config.perm_mode;
            m_verify = config.verify;
            m_createBackup = config.create_backup;
            m_password = config.password;

            m_cancelled = false;
            m_bytesRestored = 0;
            m_filesRestored = 0;
            m_filesSkipped = 0;
            m_filesErrored = 0;
            m_lastProgressFileCount = 0;
            m_lastProgressByteCount = 0;

            // Arm the run BEFORE start(): run() consumes this, so a direct QThread::start()
            // can never execute a stale configuration (see m_configured).
            m_configured = true;
            start();
        }
    }
    if (refused) {
        Q_EMIT logMessage(tr("Restore already in progress"), true);
        return false;
    }
    return true;
}

void UserProfileRestoreWorker::cancel() {
    m_cancelled = true;
    Q_EMIT logMessage(tr("Canceling restore..."), false);
}

// QThread::start() is public, so run() can be entered without a fresh startRestore().
// Consume the arm and refuse otherwise: an unconfigured start() would restore the PREVIOUS
// run's backup path, mappings, permission mode and network selections -- a repeat of a
// destructive operation the user confirmed once. Fail closed. Returns true only when
// startRestore() armed this run.
bool UserProfileRestoreWorker::consumeConfiguredArm() {
    if (m_configured.exchange(false)) {
        return true;
    }
    Q_EMIT logMessage(tr("Restore was not configured by startRestore(); refusing to run"), true);
    Q_EMIT restoreComplete(false, tr("Restore was not configured"));
    return false;
}

// Settle the outcome: build the human-readable summary and report success only when nothing
// errored (a copy/mkpath/remove failure or a refused reparse point) AND the run was not
// cancelled; a partial or cancelled restore must not read as clean success.
void UserProfileRestoreWorker::emitRestoreSummary() {
    const QString summary = tr("Restore complete!\nFiles restored: %1\nFiles skipped: %2\nErrors: "
                               "%3\nTotal size: %4 MB")
                                .arg(m_filesRestored)
                                .arg(m_filesSkipped)
                                .arg(m_filesErrored)
                                .arg(m_bytesRestored / sak::kBytesPerMBf, 0, 'f', 1);

    Q_EMIT logMessage(tr("=== Restore Complete ==="), false);
    Q_EMIT logMessage(summary, false);
    const bool success = (m_filesErrored == 0) && !m_cancelled;
    Q_EMIT restoreComplete(success, summary);
}

void UserProfileRestoreWorker::run() {
    if (!consumeConfiguredArm()) {
        return;
    }

    // No assert on m_mappings: restoring an empty selection is a restore of
    // nothing, which completes with a zero-file summary (see startRestore).
    Q_EMIT logMessage(tr("=== Restore Started ==="), false);
    Q_EMIT logMessage(tr("Backup: %1").arg(m_backupPath), false);
    Q_EMIT logMessage(tr("Users to restore: %1").arg(m_mappings.size()), false);

    // Validate backup
    if (!validateBackup()) {
        Q_EMIT restoreComplete(false, tr("Invalid backup"));
        return;
    }

    // Calculate total size
    Q_EMIT logMessage(tr("Calculating total size..."), false);
    m_totalBytesToRestore = calculateTotalSize();

    // Restore each mapped user
    int userIndex = 0;
    for (const auto& mapping : m_mappings) {
        if (m_cancelled) {
            Q_EMIT logMessage(tr("Restore cancelled by user"), true);
            Q_EMIT restoreComplete(false, tr("Restore cancelled"));
            return;
        }

        if (!mapping.selected) {
            continue;
        }

        Q_EMIT statusUpdate(mapping.source_username, tr("Starting restore..."));
        Q_EMIT logMessage(tr("=== Restoring user: %1 -> %2 ===")
                              .arg(mapping.source_username,
                                   mapping.destination_username.isEmpty()
                                       ? tr("(New)")
                                       : mapping.destination_username),
                          false);

        if (!restoreUser(mapping)) {
            // A whole-user setup failure (missing manifest entry, invalid/undefined
            // destination) must fail the overall result, not read as clean success.
            m_filesErrored++;
            Q_EMIT logMessage(tr("Failed to restore user: %1").arg(mapping.source_username), true);
        }

        userIndex++;
        Q_EMIT overallProgress(
            userIndex, static_cast<int>(m_mappings.size()), m_bytesRestored, m_totalBytesToRestore);
    }

    // Apply the selected machine-level WiFi/Ethernet settings after the file restore.
    if (!m_cancelled) {
        applyNetworkSettings();
    }

    emitRestoreSummary();
}

bool UserProfileRestoreWorker::restoreUser(const UserMapping& mapping) {
    // An empty source_username finds no manifest entry and is reported below; an
    // empty m_backupPath fails buildSafePath. Both are handled, so neither is
    // asserted (see startRestore).

    // Find source user data in manifest
    const BackupUserData* sourceUser = findManifestUser(mapping.source_username);
    if (sourceUser == nullptr) {
        Q_EMIT logMessage(tr("Source user not found in manifest: %1").arg(mapping.source_username),
                          true);
        return false;
    }

    QString sourcePath;
    if (!buildSafePath(m_backupPath, mapping.source_username, sourcePath)) {
        Q_EMIT logMessage(tr("Invalid source path for user: %1").arg(mapping.source_username),
                          true);
        return false;
    }

    QString destProfilePath;
    if (!resolveDestinationProfilePath(mapping, destProfilePath)) {
        return false;
    }

    // Pin the CANONICAL profile root once (the dir exists here: resolveExistingUser required it,
    // resolveCreateNewUser mkpath'd it). Every entry's realized parent is re-checked against this
    // so an ancestor junction cannot redirect a write out of the profile. Fail closed if the
    // root cannot be resolved rather than restoring against an unverifiable destination.
    m_currentProfileRoot = QFileInfo(destProfilePath).canonicalFilePath();
    if (m_currentProfileRoot.isEmpty()) {
        Q_EMIT logMessage(tr("Unable to resolve destination profile root: %1").arg(destProfilePath),
                          true);
        return false;
    }

    // The effective destination user for permission assignment: an explicit
    // destination, else the source (a same-name restore). Consumed by
    // copyFileWithConflictResolution -> applyPermissions (B7-21).
    m_currentDestUser = effectiveDestUser(mapping);

    if (!restoreUserFolders(mapping, *sourceUser, sourcePath, destProfilePath)) {
        return false;  // Cancelled mid-user.
    }

    // Post-restore integrity re-check (fail closed): verifyFile only proves each copied file
    // matched a RE-READ of the source, so a source swapped between pre-restore validation and
    // the copy would pass. Re-hashing the source tree against the sealed manifest digest here
    // binds the restored payload to the digest transitively (source==manifest at T0 AND at T2,
    // dest==source per-file => dest==sealed). Only runs when verification was requested.
    if (m_verify && !verifyUserPayloadChecksum(*sourceUser)) {
        Q_EMIT logMessage(
            tr("Post-restore payload integrity check failed: %1").arg(mapping.source_username),
            true);
        return false;
    }

    return true;
}

bool UserProfileRestoreWorker::restoreUserFolders(const UserMapping& mapping,
                                                  const BackupUserData& sourceUser,
                                                  const QString& sourcePath,
                                                  const QString& destProfilePath) {
    for (const auto& folder : sourceUser.backed_up_folders) {
        if (m_cancelled) {
            return false;
        }

        // Honor the folder-selection page: a folder the user unchecked must not
        // be restored (its `selected` flag was persisted into the manifest).
        if (!folder.selected) {
            continue;
        }

        Q_EMIT statusUpdate(mapping.source_username, tr("Restoring: %1").arg(folder.display_name));

        QString folderSourcePath;
        QString folderDestPath;
        if (!buildSafePath(sourcePath, folder.relative_path, folderSourcePath)) {
            Q_EMIT logMessage(tr("Invalid source folder path: %1").arg(folder.relative_path), true);
            m_filesErrored++;  // A rejected folder must fail the overall result.
            continue;
        }
        if (!buildSafePath(destProfilePath, folder.relative_path, folderDestPath)) {
            Q_EMIT logMessage(tr("Invalid destination folder path: %1").arg(folder.relative_path),
                              true);
            m_filesErrored++;
            continue;
        }

        if (!restoreFolder(folder, folderSourcePath, folderDestPath)) {
            Q_EMIT logMessage(tr("Warning: Failed to restore folder: %1").arg(folder.display_name),
                              true);
            m_filesErrored++;  // Folder-level failure -> overall restore is not clean.
            // Continue with other folders
        }
    }

    return true;
}

bool UserProfileRestoreWorker::resolveCreateNewUser(const UserMapping& mapping,
                                                    const QString& systemDrive,
                                                    QString& destProfilePath) {
    if (systemDrive.isEmpty()) {
        Q_EMIT logMessage(tr("SystemDrive is not set; cannot resolve a new-user profile location"),
                          true);
        return false;
    }
    const QString destUsername = mapping.destination_username.isEmpty()
                                     ? mapping.source_username
                                     : mapping.destination_username;
    const QString baseProfileRoot = systemDrive + "/Users";
    if (!buildSafePath(baseProfileRoot, destUsername, destProfilePath)) {
        Q_EMIT logMessage(tr("Invalid destination username: %1").arg(destUsername), true);
        return false;
    }
    // Fail closed on a name that already has a profile directory. QDir::mkpath() returns true for
    // an EXISTING directory, so without this check leaving the mode on "Create New User" for a name
    // that matches a live local profile would write the foreign backup straight into that user's
    // profile (with the conflict policy applied, so files can be overwritten) while the log below
    // falsely claims a new profile was created. Refuse the collision instead.
    if (QFileInfo::exists(destProfilePath)) {
        Q_EMIT logMessage(
            tr("A profile directory already exists at %1; refusing to restore as a new user")
                .arg(destProfilePath),
            true);
        return false;
    }
    if (!QDir().mkpath(destProfilePath)) {
        Q_EMIT logMessage(tr("Failed to create profile directory: %1").arg(destProfilePath), true);
        return false;
    }
    Q_EMIT logMessage(tr("Created new profile: %1").arg(destProfilePath), false);
    return true;
}

bool UserProfileRestoreWorker::resolveExistingUser(const UserMapping& mapping,
                                                   QString& destProfilePath) {
    if (mapping.destination_username.isEmpty()) {
        Q_EMIT logMessage(tr("Destination username not specified"), true);
        return false;
    }
    destProfilePath = WindowsUserScanner::getProfilePath(mapping.destination_username);
    if (destProfilePath.isEmpty()) {
        Q_EMIT logMessage(
            tr("Failed to resolve destination profile path: %1").arg(mapping.destination_username),
            true);
        return false;
    }
    if (!QDir(destProfilePath).exists()) {
        Q_EMIT logMessage(tr("Destination profile does not exist: %1").arg(destProfilePath), true);
        return false;
    }
    return true;
}

bool UserProfileRestoreWorker::resolveDestinationProfilePath(const UserMapping& mapping,
                                                             QString& destProfilePath) {
    // No "C:" fallback: an unset SystemDrive is surfaced by resolveCreateNewUser
    // (the only branch that needs it), rather than silently guessing a drive.
    const QString systemDrive = QString::fromLocal8Bit(qgetenv("SystemDrive"));

    switch (mapping.mode) {
    case MergeMode::CreateNewUser:
        return resolveCreateNewUser(mapping, systemDrive, destProfilePath);
    case MergeMode::ReplaceDestination:
    case MergeMode::MergeIntoDestination:
        return resolveExistingUser(mapping, destProfilePath);
    }
    // Unknown/out-of-range merge mode: fail closed instead of returning success
    // with an unset destination path.
    Q_EMIT logMessage(tr("Unknown merge mode for user: %1").arg(mapping.source_username), true);
    return false;
}

bool UserProfileRestoreWorker::restoreFolder(const FolderSelection& folder,
                                             const QString& sourcePath,
                                             const QString& destPath) {
    // An empty sourcePath fails the exists() check below and is reported; no
    // assert, which would abort Debug on input Release handles.
    const QFileInfo sourceInfo(sourcePath);

    if (!sourceInfo.exists()) {
        Q_EMIT logMessage(tr("Source folder does not exist: %1").arg(sourcePath), true);
        return false;
    }

    // Create destination directory
    const QDir destDir(destPath);
    if (!destDir.exists()) {
        if (!QDir().mkpath(destPath)) {
            Q_EMIT logMessage(tr("Failed to create directory: %1").arg(destPath), true);
            return false;
        }
    }

    // Recursively copy directory contents. The folder's own relative_path seeds the
    // profile-relative path used for AppData source filtering.
    return copyDirectory(sourcePath, destPath, folder, folder.relative_path);
}

bool UserProfileRestoreWorker::copyDirectory(const QString& sourceDir,
                                             const QString& destDir,
                                             const FolderSelection& folderConfig,
                                             const QString& profileRelativeDir) {
    // An empty sourceDir fails the exists() check below; see restoreFolder.
    const QDir dir(sourceDir);
    if (!dir.exists()) {
        return false;
    }
    // An unreadable source directory would enumerate as an empty list and make the
    // copy silently "succeed" having copied nothing. Fail closed instead.
    if (!dir.isReadable()) {
        Q_EMIT logMessage(tr("Source directory is not readable: %1").arg(sourceDir), true);
        return false;
    }

    // Create destination directory
    if (!QDir().mkpath(destDir)) {
        Q_EMIT logMessage(tr("Failed to create directory: %1").arg(destDir), true);
        return false;
    }

    // Iterate through all entries
    const QFileInfoList entries = dir.entryInfoList(
        QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System);

    // cppcheck-suppress useStlAlgorithm ; loop has side effects (file copy, cancellation)
    for (const QFileInfo& entry : entries) {
        if (m_cancelled) {
            return false;
        }

        const QString destItem = destDir + "/" + entry.fileName();
        const QString entryRelative = profileRelativeDir.isEmpty()
                                          ? entry.fileName()
                                          : profileRelativeDir + "/" + entry.fileName();

        // Honor the AppData page: an entry under an app-data source the user
        // unchecked is intentionally skipped (not an error).
        if (isAppDataPathExcluded(entryRelative, m_appDataSources)) {
            m_filesSkipped++;
            continue;
        }

        copyDirectoryEntry(entry, destItem, folderConfig, entryRelative);
    }

    return true;
}

void UserProfileRestoreWorker::copyDirectoryEntry(const QFileInfo& entry,
                                                  const QString& destItem,
                                                  const FolderSelection& folderConfig,
                                                  const QString& entryRelative) {
    const QString sourceItem = entry.absoluteFilePath();

    // Refuse junctions/symlinks on either side: they can read outside the backup or
    // write outside the approved profile root (e.g. into System32).
    if (restoreEntryEscapesRoot(entry, destItem)) {
        Q_EMIT logMessage(
            tr("Skipping reparse point to prevent restore escape: %1").arg(sourceItem), true);
        m_filesErrored++;
        return;
    }

    // Re-validate the realized destination parent against the pinned profile root: the leaf
    // check above misses an ANCESTOR directory turned into a junction after its mkpath, which
    // would redirect this write outside the profile. Fail closed on any escape.
    if (!destinationParentWithinRoot(m_currentProfileRoot, destItem)) {
        Q_EMIT logMessage(
            tr("Skipping entry: destination escapes the profile root (reparse point): %1")
                .arg(destItem),
            true);
        m_filesErrored++;
        return;
    }

    if (entry.isDir()) {
        if (!copyDirectory(sourceItem, destItem, folderConfig, entryRelative)) {
            Q_EMIT logMessage(tr("Warning: Failed to copy directory: %1").arg(sourceItem), true);
            m_filesErrored++;  // A subtree that failed to copy must not read as success.
        }
        return;
    }

    if (entry.isFile()) {
        copyFileWithConflictResolution(sourceItem, destItem, entry.size());
    }
}

bool UserProfileRestoreWorker::copyFileWithConflictResolution(const QString& source,
                                                              const QString& dest,
                                                              qint64 size) {
    // source/dest emptiness is handled by the file operations below; see
    // restoreFolder. size stays asserted because it is an internal invariant
    // this class guarantees: it comes from QFileInfo::size(), which reports 0 -
    // never a negative - for a file it cannot stat.
    Q_ASSERT_X(size >= 0, "copyFileWithConflictResolution", "size must be non-negative");
    const QFileInfo destInfo(dest);
    QString finalDestPath = dest;

    // Check if destination file exists and resolve conflict
    if (destInfo.exists()) {
        if (!resolveFileConflict(source, destInfo, size, finalDestPath)) {
            return true;  // File was skipped per conflict resolution strategy
        }
    }

    // Ensure destination directory exists
    const QFileInfo finalDestInfo(finalDestPath);
    if (!QDir().mkpath(finalDestInfo.absolutePath())) {
        Q_EMIT logMessage(tr("Failed to create directory: %1").arg(finalDestInfo.absolutePath()),
                          true);
        m_filesErrored++;
        return false;
    }

    // Copy the file (atomically replacing an existing destination, if any). When
    // the safety option is on, the pre-restore file is saved to `.sakbak` first.
    if (!copyFileReplacingExisting(source, finalDestPath, m_createBackup, m_password)) {
        Q_EMIT logMessage(tr("Error copying file: %1").arg(source), true);
        m_filesErrored++;
        return false;
    }

    // Apply permissions based on mode. Pass the current mapping's destination user
    // so AssignToDestination assigns ownership instead of always stripping (B7-21).
    // Fail CLOSED: a file whose ownership/ACL could not be set is NOT a clean restore.
    const bool permsOk = applyPermissions(finalDestPath, m_currentDestUser);
    if (!permsOk) {
        Q_EMIT logMessage(tr("Error: Failed to adjust permissions: %1").arg(finalDestPath), true);
    }

    // Verify file content against the source if requested. A verification failure
    // means the copied bytes differ or are unreadable -> count it as an error.
    const bool verifyOk = !m_verify || verifyFile(source, finalDestPath);
    if (!verifyOk) {
        Q_EMIT logMessage(tr("Error: File verification failed: %1").arg(finalDestPath), true);
    }

    if (!permsOk || !verifyOk) {
        m_filesErrored++;
        return false;
    }

    // Update progress
    m_filesRestored++;
    updateProgress(size);

    return true;
}

QString UserProfileRestoreWorker::generateConflictRenamePath(const QFileInfo& destInfo) {
    const QString baseName = destInfo.completeBaseName();
    const QString extension = destInfo.suffix();
    const QString dirPath = destInfo.absolutePath();
    const QString suffix = extension.isEmpty() ? QString() : "." + extension;

    constexpr int kMaxRenameAttempts = 1000;
    int counter = 1;
    QString renamed;
    do {
        renamed =
            QString("%1/%2_backup%3%4").arg(dirPath, baseName, QString::number(counter++), suffix);
    } while (QFileInfo::exists(renamed) && counter < kMaxRenameAttempts);

    // Exhausted every candidate and the last one still exists: return empty so the
    // caller fails closed instead of overwriting an existing unrelated file.
    if (QFileInfo::exists(renamed)) {
        return {};
    }
    return renamed;
}

bool UserProfileRestoreWorker::resolveFileConflict(const QString& source,
                                                   const QFileInfo& destInfo,
                                                   qint64 size,
                                                   QString& finalDestPath) {
    switch (m_conflictMode) {
    case ConflictResolution::SkipDuplicate:
        Q_EMIT logMessage(tr("Skipping existing file: %1").arg(destInfo.fileName()), false);
        m_filesSkipped++;
        return false;

    case ConflictResolution::RenameWithSuffix:
        return applyRenameTarget(generateConflictRenamePath(destInfo), destInfo, finalDestPath);

    case ConflictResolution::KeepNewer:
        // Do not delete here: copyFileReplacingExisting removes the original only
        // after the replacement is fully written, so a failed copy never leaves
        // the destination missing.
        if (destInfo.lastModified() >= QFileInfo(source).lastModified()) {
            Q_EMIT logMessage(tr("Keeping newer existing file: %1").arg(destInfo.fileName()),
                              false);
            m_filesSkipped++;
            return false;
        }
        Q_EMIT logMessage(tr("Replacing with newer file: %1").arg(destInfo.fileName()), false);
        return true;

    case ConflictResolution::KeepLarger:
        if (destInfo.size() >= size) {
            Q_EMIT logMessage(tr("Keeping larger existing file: %1").arg(destInfo.fileName()),
                              false);
            m_filesSkipped++;
            return false;
        }
        Q_EMIT logMessage(tr("Replacing with larger file: %1").arg(destInfo.fileName()), false);
        return true;

    case ConflictResolution::PromptUser:
        return applyRenameTarget(resolveConflict(finalDestPath), destInfo, finalDestPath);
    }
    // Unknown/out-of-range conflict mode: fail closed rather than overwrite the
    // original with finalDestPath left pointing at the existing destination.
    Q_EMIT logMessage(tr("Unknown conflict resolution mode; skipping: %1").arg(destInfo.fileName()),
                      true);
    m_filesErrored++;
    return false;
}

bool UserProfileRestoreWorker::applyRenameTarget(const QString& candidate,
                                                 const QFileInfo& destInfo,
                                                 QString& finalDestPath) {
    if (candidate.isEmpty()) {
        Q_EMIT logMessage(
            tr("Could not find a free rename target for: %1").arg(destInfo.fileName()), true);
        m_filesErrored++;
        return false;
    }
    finalDestPath = candidate;
    Q_EMIT logMessage(
        tr("Resolving conflict, restoring as: %1").arg(QFileInfo(candidate).fileName()), false);
    return true;
}

UserProfileRestoreWorker::PermissionAction UserProfileRestoreWorker::resolvePermissionAction(
    PermissionMode mode, const QString& destinationUser) {
    switch (mode) {
    case PermissionMode::PreserveOriginal:
        return PermissionAction::PreserveOriginal;
    case PermissionMode::AssignToDestination:
        // The whole point of this mode: assign ownership to the target user. Only
        // fall back to stripping when no user is known (B7-21).
        return destinationUser.isEmpty() ? PermissionAction::StripPermissions
                                         : PermissionAction::AssignOwnership;
    case PermissionMode::StripAll:
        return PermissionAction::StripPermissions;
    }
    return PermissionAction::StripPermissions;
}

QString UserProfileRestoreWorker::effectiveDestUser(const UserMapping& mapping) {
    return mapping.destination_username.isEmpty() ? mapping.source_username
                                                  : mapping.destination_username;
}

bool UserProfileRestoreWorker::assignOwnershipToUser(const QString& filePath,
                                                     const QString& destinationUser) {
    // The caller explicitly chose AssignToDestination. If the SID cannot be resolved
    // or ownership cannot be taken, silently stripping permissions and reporting
    // success would be a policy downgrade the user did not ask for. Fail closed so
    // the restore records the error instead of installing weaker permissions.
    const QString userSID = WindowsUserScanner::getUserSID(destinationUser);
    if (userSID.isEmpty()) {
        sak::logError(
            "Could not resolve SID for user '{}'; failing closed instead of "
            "silently stripping permissions",
            destinationUser.toStdString());
        return false;
    }
    if (!m_permissionManager->takeOwnership(filePath, userSID)) {
        sak::logError("Failed to take ownership of '{}' for the destination user; failing closed",
                      filePath.toStdString());
        return false;
    }
    return m_permissionManager->setStandardUserPermissions(filePath, userSID);
}

bool UserProfileRestoreWorker::applyPermissions(const QString& filePath,
                                                const QString& destinationUser) {
    switch (resolvePermissionAction(m_permissionMode, destinationUser)) {
    case PermissionAction::PreserveOriginal:
        return true;  // Keep permissions already set by the copy.
    case PermissionAction::StripPermissions:
        return m_permissionManager->stripPermissions(filePath);
    case PermissionAction::AssignOwnership:
        return assignOwnershipToUser(filePath, destinationUser);
    }
    return true;
}

bool UserProfileRestoreWorker::validateBackup() {
    const QFileInfo manifestFile(m_backupPath + "/manifest.json");
    if (!manifestFile.exists()) {
        Q_EMIT logMessage(tr("Manifest file not found"), true);
        return false;
    }

    // Integrity: a populated manifest checksum that does not match means the
    // manifest.json was corrupted or tampered with -> refuse (B7-13). A legacy
    // backup with no stored checksum verifies as true (nothing to check).
    if (!m_manifest.verifyManifestChecksum()) {
        Q_EMIT logMessage(tr("Manifest integrity check failed (checksum mismatch)"), true);
        return false;
    }
    if (m_manifest.manifest_checksum.isEmpty()) {
        // A legacy backup with no integrity checksum is intentionally supported (documented in
        // user_profile_types.h). Do NOT couple this to m_verify: verify enables per-file content
        // hashing (verifyFile), which is independent of whether the manifest was ever sealed --
        // failing closed here would wrongly reject content-only verification of a legacy backup.
        Q_EMIT logMessage(
            tr("Manifest has no integrity checksum (legacy backup); skipping verification"), false);
    }

    if (!verifyUserPayloadChecksums()) {
        return false;
    }

    Q_EMIT logMessage(tr("Backup validation passed"), false);
    return true;
}

bool UserProfileRestoreWorker::verifyUserPayloadChecksums() {
    for (const auto& mapping : m_mappings) {
        if (!mapping.selected) {
            continue;
        }
        const auto* user = findManifestUser(mapping.source_username);
        if (user == nullptr) {
            continue;  // Unmapped.
        }
        if (!verifyUserPayloadChecksum(*user)) {
            return false;
        }
    }
    return true;
}

bool UserProfileRestoreWorker::verifyUserPayloadChecksum(const BackupUserData& user) {
    if (user.checksum_sha256.isEmpty()) {
        // Empty per-user digest: acceptable for a fully legacy backup (which has no manifest
        // integrity checksum either). But when the manifest IS sealed (manifest_checksum
        // present and already verified above), a missing payload digest is inconsistent with a
        // sealed manifest -- an attacker who stripped the digest and re-sealed the manifest --
        // so fail closed. This distinguishes genuine legacy from tampering without rejecting
        // legitimate legacy backups.
        return m_manifest.manifest_checksum.isEmpty();
    }
    // user.username comes from the manifest (untrusted): route it through the same
    // containment guard the restore path uses so a traversal name cannot make
    // hashDirectoryTree walk a tree outside the backup root (info exposure / DoS).
    QString userPayloadPath;
    if (!buildSafePath(m_backupPath, user.username, userPayloadPath)) {
        Q_EMIT logMessage(
            tr("Payload path for user '%1' escapes the backup root").arg(user.username), true);
        return false;
    }
    const QString actual = BackupManifest::hashDirectoryTree(userPayloadPath);
    if (actual != user.checksum_sha256) {
        Q_EMIT logMessage(tr("Payload integrity check failed for user '%1' (checksum mismatch)")
                              .arg(user.username),
                          true);
        return false;
    }
    return true;
}

qint64 UserProfileRestoreWorker::calculateTotalSize() {
    qint64 totalSize = 0;
    m_totalFilesToRestore = 0;

    for (const auto& mapping : m_mappings) {
        if (!mapping.selected) {
            continue;
        }

        const auto* user = findManifestUser(mapping.source_username);
        if (user == nullptr) {
            continue;
        }

        for (const auto& folder : user->backed_up_folders) {
            if (!folder.selected) {
                continue;  // Unchecked folders are skipped during restore; exclude from totals.
            }
            accumulateFolderTotals(folder, totalSize);
        }
    }

    return totalSize;
}

void UserProfileRestoreWorker::accumulateFolderTotals(const FolderSelection& folder,
                                                      qint64& totalSize) {
    // Guard against negative or overflowing manifest-declared totals: these feed a
    // progress meter, so wrapping to a tiny value would badly mislead the user.
    // Saturate instead of wrapping, and ignore nonsensical negative counts.
    if (folder.size_bytes > 0) {
        if (totalSize > std::numeric_limits<qint64>::max() - folder.size_bytes) {
            totalSize = std::numeric_limits<qint64>::max();
        } else {
            totalSize += folder.size_bytes;
        }
    }
    if (folder.file_count > 0) {
        if (m_totalFilesToRestore > std::numeric_limits<int>::max() - folder.file_count) {
            m_totalFilesToRestore = std::numeric_limits<int>::max();
        } else {
            m_totalFilesToRestore += folder.file_count;
        }
    }
}

const BackupUserData* UserProfileRestoreWorker::findManifestUser(const QString& username) const {
    auto it = std::find_if(m_manifest.users.begin(),
                           m_manifest.users.end(),
                           [&username](const auto& user) { return user.username == username; });
    return (it != m_manifest.users.end()) ? &(*it) : nullptr;
}

void UserProfileRestoreWorker::updateProgress(qint64 bytesAdded) {
    // Invariant: the sole caller (copyFileWithConflictResolution) forwards its size, which
    // copyDirectoryEntry takes from QFileInfo::size() -- 0, never negative, when it cannot stat.
    Q_ASSERT(bytesAdded >= 0);
    m_bytesRestored += bytesAdded;

    if (m_filesRestored - m_lastProgressFileCount >= kRestoreProgressFileInterval ||
        m_bytesRestored - m_lastProgressByteCount >= kRestoreProgressByteInterval) {
        Q_EMIT fileProgress(m_filesRestored, m_totalFilesToRestore);

        m_lastProgressFileCount = m_filesRestored;
        m_lastProgressByteCount = m_bytesRestored;
    }
}

bool UserProfileRestoreWorker::hashFile(const QString& filePath, QByteArray& outDigest) {
    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly)) {
        return false;
    }
    QCryptographicHash hasher(QCryptographicHash::Sha256);
    if (!hasher.addData(&file)) {
        return false;
    }
    outDigest = hasher.result();
    return true;
}

bool UserProfileRestoreWorker::verifyFile(const QString& sourcePath, const QString& filePath) {
    // Empty paths fail the exists()/isReadable() check below; see restoreFolder.
    const QFileInfo fileInfo(filePath);

    if (!fileInfo.exists() || !fileInfo.isReadable()) {
        Q_EMIT logMessage(tr("Verification failed - file missing or unreadable: %1").arg(filePath),
                          true);
        return false;
    }

    // Re-hash the copied file and the source and require an exact match, so a
    // truncated or corrupted copy is caught post-write (not just readability).
    QByteArray sourceDigest;
    QByteArray destDigest;
    if (!hashFile(sourcePath, sourceDigest) || !hashFile(filePath, destDigest)) {
        Q_EMIT logMessage(tr("Verification failed - could not hash file: %1").arg(filePath), true);
        return false;
    }
    if (sourceDigest != destDigest) {
        Q_EMIT logMessage(tr("Verification failed - content mismatch: %1").arg(filePath), true);
        return false;
    }
    return true;
}

QString UserProfileRestoreWorker::resolveConflict(const QString& destPath) {
    // Generate unique filename by adding suffix
    const QFileInfo destInfo(destPath);
    const QString baseName = destInfo.completeBaseName();
    const QString extension = destInfo.suffix();
    const QString dirPath = destInfo.absolutePath();

    int counter = 1;
    QString newPath;
    do {
        newPath = QString("%1/%2_restored%3%4")
                      .arg(dirPath,
                           baseName,
                           QString::number(counter++),
                           extension.isEmpty() ? "" : "." + extension);
    } while (QFileInfo::exists(newPath) && counter < kRestoreConflictMaxAttempts);

    // Exhausted: return empty so the caller fails closed rather than overwriting.
    if (QFileInfo::exists(newPath)) {
        return {};
    }
    return newPath;
}

// ============================================================================
// Network restore (WiFi / Ethernet) + AppData source filtering
// ============================================================================

QString UserProfileRestoreWorker::resolveSystem32Netsh() {
    // System32-qualify netsh so a hostile netsh.exe planted in the working directory cannot
    // win the search order. %SystemRoot% alone is NOT trustworthy for that: an unprivileged
    // HKCU\Environment write propagates through Explorer into an elevated process, so it is
    // used only as a presence gate -- the path actually returned comes from the OS
    // (GetSystemDirectoryW, via sak::system32Path) and must MATCH the environment-composed
    // one. No fallback anywhere: an unset %SystemRoot%, an unresolvable system directory, or
    // a mismatch (poisoned environment) all return empty and every caller fails closed.
    const QString root = QString::fromLocal8Bit(qgetenv("SystemRoot"));
    if (root.isEmpty()) {
        return {};
    }
    const QString fromEnv =
        QDir::fromNativeSeparators(QDir(root).filePath(QStringLiteral("System32/netsh.exe")));
    const QString fromOs = system32Path(QStringLiteral("netsh.exe"));
    if (fromOs.isEmpty() ||
        QDir::fromNativeSeparators(fromOs).compare(fromEnv, Qt::CaseInsensitive) != 0) {
        return {};
    }
    return QDir::toNativeSeparators(fromOs);
}

bool UserProfileRestoreWorker::isAppDataPathExcluded(const QString& profileRelativePath,
                                                     const QVector<AppDataSourceInfo>& sources) {
    // No AppData selection forwarded (legacy backup, or the page was never shown)
    // means restore everything -- the filter only ever removes, never adds.
    if (sources.isEmpty()) {
        return false;
    }
    const QString norm = QDir::fromNativeSeparators(profileRelativePath).toLower();
    int bestLen = -1;
    bool excluded = false;
    for (const auto& src : sources) {
        QString base = QDir::fromNativeSeparators(src.relative_path).toLower();
        while (base.endsWith(QLatin1Char('/'))) {
            base.chop(1);
        }
        if (base.isEmpty()) {
            continue;
        }
        // Segment-aware prefix match: the path is the source root or lives under
        // it. The most specific (longest) matching source decides, so an unchecked
        // "AppData/Local/Google/Chrome" excludes without affecting a checked
        // sibling under "AppData/Local/Google".
        if (norm == base || norm.startsWith(base + QLatin1Char('/'))) {
            if (base.length() > bestLen) {
                bestLen = static_cast<int>(base.length());
                excluded = !src.selected;
            }
        }
    }
    return excluded;
}

bool UserProfileRestoreWorker::restoreWifiProfile(const WifiProfileInfo& profile) {
    if (profile.xml_data.isEmpty()) {
        Q_EMIT logMessage(
            tr("WiFi profile '%1' has no stored XML; cannot restore").arg(profile.profile_name),
            true);
        return false;
    }
    const QString netsh = resolveSystem32Netsh();
    if (netsh.isEmpty()) {
        Q_EMIT logMessage(tr("Cannot locate netsh.exe (SystemRoot unset)"), true);
        return false;
    }

    // Stage the profile XML to a temp file for `netsh wlan add profile`.
    QTemporaryFile xmlFile(QDir::tempPath() + QStringLiteral("/sak_wlan_XXXXXX.xml"));
    xmlFile.setAutoRemove(true);
    if (!xmlFile.open()) {
        Q_EMIT logMessage(tr("Failed to stage WiFi profile XML for: %1").arg(profile.profile_name),
                          true);
        return false;
    }
    const QByteArray xmlBytes = profile.xml_data.toUtf8();
    if (xmlFile.write(xmlBytes) != xmlBytes.size()) {
        Q_EMIT logMessage(tr("Failed to write WiFi profile XML for: %1").arg(profile.profile_name),
                          true);
        return false;
    }
    xmlFile.flush();

    const QString xmlPath = QDir::toNativeSeparators(xmlFile.fileName());
    const ProcessResult result = runProcess(netsh,
                                            {QStringLiteral("wlan"),
                                             QStringLiteral("add"),
                                             QStringLiteral("profile"),
                                             QStringLiteral("filename=") + xmlPath,
                                             QStringLiteral("user=all")},
                                            kNetshTimeoutMs,
                                            [this] { return m_cancelled.load(); });
    if (!result.completedSuccessfully()) {
        Q_EMIT logMessage(tr("netsh wlan add profile failed for '%1': %2")
                              .arg(profile.profile_name, result.std_err.trimmed()),
                          true);
        return false;
    }
    return true;
}

bool UserProfileRestoreWorker::runNetshLogged(const QString& netsh,
                                              const QStringList& args,
                                              const QString& adapter) {
    const ProcessResult r =
        runProcess(netsh, args, kNetshTimeoutMs, [this] { return m_cancelled.load(); });
    if (!r.completedSuccessfully()) {
        Q_EMIT logMessage(tr("netsh %1 failed for adapter '%2': %3")
                              .arg(args.join(QLatin1Char(' ')), adapter, r.std_err.trimmed()),
                          true);
    }
    return r.completedSuccessfully();
}

bool UserProfileRestoreWorker::restoreEthernetDhcp(const QString& adapter, const QString& netsh) {
    // Return the address to DHCP. netsh returns a non-zero "DHCP is already enabled
    // on this interface" when the adapter is ALREADY DHCP -- that is the desired
    // end-state, not a failure. Skip the redundant switch when already DHCP, and if
    // the switch does error, accept it only when the adapter is verifiably DHCP
    // afterward (IP Helper API, locale-independent). Fail closed otherwise.
    bool addrOk = true;
    const std::optional<bool> alreadyDhcp = adapterDhcpEnabled(adapter);
    if (!(alreadyDhcp.has_value() && *alreadyDhcp)) {
        addrOk = runNetshLogged(netsh,
                                {QStringLiteral("interface"),
                                 QStringLiteral("ip"),
                                 QStringLiteral("set"),
                                 QStringLiteral("address"),
                                 QStringLiteral("name=") + adapter,
                                 QStringLiteral("source=dhcp")},
                                adapter);
        if (!addrOk) {
            const std::optional<bool> nowDhcp = adapterDhcpEnabled(adapter);
            addrOk = nowDhcp.has_value() && *nowDhcp;
        }
    }
    const bool dnsOk = runNetshLogged(netsh,
                                      {QStringLiteral("interface"),
                                       QStringLiteral("ip"),
                                       QStringLiteral("set"),
                                       QStringLiteral("dnsservers"),
                                       QStringLiteral("name=") + adapter,
                                       QStringLiteral("source=dhcp")},
                                      adapter);
    return addrOk && dnsOk;
}

bool UserProfileRestoreWorker::restoreEthernetStatic(const EthernetConfigInfo& config,
                                                     const QString& netsh) {
    const QString& name = config.adapter_name;
    // Static configuration requires at least an address + mask; anything less would
    // leave netsh to guess, so fail closed.
    if (config.ip_address.isEmpty() || config.subnet_mask.isEmpty()) {
        Q_EMIT logMessage(
            tr("Ethernet config '%1' is static but missing IP address/subnet").arg(name), true);
        return false;
    }

    QStringList addressArgs{QStringLiteral("interface"),
                            QStringLiteral("ip"),
                            QStringLiteral("set"),
                            QStringLiteral("address"),
                            QStringLiteral("name=") + name,
                            QStringLiteral("static"),
                            config.ip_address,
                            config.subnet_mask};
    if (!config.default_gateway.isEmpty()) {
        addressArgs << config.default_gateway;
    }
    bool ok = runNetshLogged(netsh, addressArgs, name);

    if (config.dns_primary.isEmpty()) {
        return ok;
    }
    ok = runNetshLogged(netsh,
                        {QStringLiteral("interface"),
                         QStringLiteral("ip"),
                         QStringLiteral("set"),
                         QStringLiteral("dnsservers"),
                         QStringLiteral("name=") + name,
                         QStringLiteral("static"),
                         config.dns_primary,
                         QStringLiteral("primary")},
                        name) &&
         ok;
    if (!config.dns_secondary.isEmpty()) {
        ok = runNetshLogged(netsh,
                            {QStringLiteral("interface"),
                             QStringLiteral("ip"),
                             QStringLiteral("add"),
                             QStringLiteral("dnsservers"),
                             QStringLiteral("name=") + name,
                             config.dns_secondary,
                             QStringLiteral("index=2")},
                            name) &&
             ok;
    }
    return ok;
}

bool UserProfileRestoreWorker::restoreEthernetConfig(const EthernetConfigInfo& config) {
    if (config.adapter_name.isEmpty()) {
        Q_EMIT logMessage(tr("Ethernet configuration has no adapter name; cannot restore"), true);
        return false;
    }
    const QString netsh = resolveSystem32Netsh();
    if (netsh.isEmpty()) {
        Q_EMIT logMessage(tr("Cannot locate netsh.exe (SystemRoot unset)"), true);
        return false;
    }
    return config.dhcp_enabled ? restoreEthernetDhcp(config.adapter_name, netsh)
                               : restoreEthernetStatic(config, netsh);
}

void UserProfileRestoreWorker::applyNetworkSettings() {
    const auto selectedWifi = std::count_if(m_wifiProfiles.begin(),
                                            m_wifiProfiles.end(),
                                            [](const WifiProfileInfo& p) { return p.selected; });
    const auto selectedEth = std::count_if(m_ethernetConfigs.begin(),
                                           m_ethernetConfigs.end(),
                                           [](const EthernetConfigInfo& c) { return c.selected; });
    if (selectedWifi == 0 && selectedEth == 0) {
        return;
    }
    Q_EMIT logMessage(tr("=== Applying network settings ==="), false);
    applyWifiProfiles();
    applyEthernetConfigs();
}

void UserProfileRestoreWorker::applyWifiProfiles() {
    for (const auto& profile : m_wifiProfiles) {
        if (m_cancelled) {
            return;
        }
        if (!profile.selected) {
            continue;
        }
        Q_EMIT statusUpdate(profile.profile_name, tr("Restoring WiFi profile..."));
        if (restoreWifiProfile(profile)) {
            Q_EMIT logMessage(tr("Restored WiFi profile: %1").arg(profile.profile_name), false);
        } else {
            m_filesErrored++;  // A selected item that could not be applied fails the result.
        }
    }
}

void UserProfileRestoreWorker::applyEthernetConfigs() {
    for (const auto& config : m_ethernetConfigs) {
        if (m_cancelled) {
            return;
        }
        if (!config.selected) {
            continue;
        }
        Q_EMIT statusUpdate(config.adapter_name, tr("Restoring Ethernet configuration..."));
        if (restoreEthernetConfig(config)) {
            Q_EMIT logMessage(tr("Restored Ethernet configuration: %1").arg(config.adapter_name),
                              false);
        } else {
            m_filesErrored++;
        }
    }
}

}  // namespace sak
