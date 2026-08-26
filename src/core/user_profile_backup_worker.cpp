// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/user_profile_backup_worker.h"

#include "sak/elevation_manager.h"
#include "sak/layout_constants.h"

#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QStorageInfo>
#include <QtGlobal>

#include <algorithm>

#ifdef Q_OS_WIN
#include <windows.h>
#endif

namespace sak {

namespace {
// The compression level is range-checked where it is USED, by the codec, which refuses
// anything outside 1-9 rather than clamping. A duplicate check here would be a second
// place to keep in step with zlib.
constexpr int kBackupSizeDisplayPrecision = 2;
constexpr int kProgressEmitFileInterval = 100;
constexpr qint64 kProgressEmitByteInterval = kProgressEmitFileInterval * kBytesPerMB;
constexpr double kDiskSpaceSafetyMultiplier = 1.1;
constexpr int kDiskSpaceDisplayPrecision = 1;
// Number of leading backslashes in a "\\server\share" UNC path, stripped before the
// "\\?\UNC\" extended-length prefix is prepended.
constexpr int kUncLeadingBackslashCount = 2;
// First code point that is not a C0 control character (space, U+0020); anything below it is a
// control character disallowed in a path component.
constexpr int kFirstNonControlCodePoint = 0x20;

// True if the path is an NTFS reparse point (junction/symlink/mount point). Fails CLOSED: a query
// that cannot resolve the path is reported AS a reparse point so the caller refuses to follow it.
bool isReparsePoint(const QString& path) {
#ifdef Q_OS_WIN
    // Query through the \\?\ extended-length form so a path longer than MAX_PATH (this process
    // carries no long-path manifest) still resolves. A bare GetFileAttributesW fails on such a path
    // and -- under the fail-closed rule below -- would refuse every ordinary deep profile path;
    // \\?\ avoids that. It needs a fully-qualified backslash path, and a \\server\share UNC path
    // becomes \\?\UNC\server\share.
    const QString extended = UserProfileBackupWorker::extendedLengthPath(
        QDir::toNativeSeparators(QFileInfo(path).absoluteFilePath()));
    const DWORD attrs = GetFileAttributesW(extended.toStdWString().c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES) {
        return true;  // fail closed: an unresolvable/denied query must not read as "not a link"
    }
    return (attrs & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
#else
    return QFileInfo(path).isSymLink();
#endif
}

// True if @p path resolves through a reparse point anywhere between @p root (inclusive) and the
// leaf. mkpath would silently create the remainder INSIDE a planted junction: an attacker with
// write access to the technician-chosen backup @p root can pre-plant e.g. <root>\<user> ->
// C:\Windows\System32, and the elevated copy would then write profile content through it. Screen
// the destination before it is created. Only ancestors at or below @p root are checked (anything
// above the technician-chosen root is trusted); an empty root and any unresolvable component fail
// closed.
bool destinationRedirectedThroughReparsePoint(const QString& root, const QString& path) {
    if (root.trimmed().isEmpty()) {
        return true;  // no known root to bound the walk -> refuse rather than walk to the drive
    }
    const QString canonicalRoot = QDir::cleanPath(QFileInfo(root).absoluteFilePath());
    QString current = QDir::cleanPath(QFileInfo(path).absoluteFilePath());
    QString previous;
    while (!current.isEmpty() && current != previous) {
        if (QFileInfo::exists(current) && isReparsePoint(current)) {
            return true;
        }
        if (current.compare(canonicalRoot, Qt::CaseInsensitive) == 0) {
            break;  // reached the trusted root; nothing above it is our concern
        }
        previous = current;
        current = QFileInfo(current).path();
    }
    return false;
}

// Deepest directory recursion the copy will follow. A real profile tree is far shallower;
// a crafted tree deeper than this is refused (fail closed) rather than allowed to exhaust
// the worker thread's stack.
constexpr int kMaxCopyDepth = 512;

// True if @p component (a single, separator-free path element) is a Win32 reserved device
// name (CON, PRN, AUX, NUL, COM1-9, LPT1-9), with or without an extension. Win32 resolves
// such a name to the device no matter which directory it sits in.
bool isWindowsReservedDeviceName(const QString& component) {
    QString base = component;
    const int dot = static_cast<int>(base.indexOf(QLatin1Char('.')));
    if (dot >= 0) {
        base = base.left(dot);
    }
    base = base.trimmed().toUpper();
    static const QStringList kReserved = {
        QStringLiteral("CON"),  QStringLiteral("PRN"),  QStringLiteral("AUX"),
        QStringLiteral("NUL"),  QStringLiteral("COM1"), QStringLiteral("COM2"),
        QStringLiteral("COM3"), QStringLiteral("COM4"), QStringLiteral("COM5"),
        QStringLiteral("COM6"), QStringLiteral("COM7"), QStringLiteral("COM8"),
        QStringLiteral("COM9"), QStringLiteral("LPT1"), QStringLiteral("LPT2"),
        QStringLiteral("LPT3"), QStringLiteral("LPT4"), QStringLiteral("LPT5"),
        QStringLiteral("LPT6"), QStringLiteral("LPT7"), QStringLiteral("LPT8"),
        QStringLiteral("LPT9")};
    return kReserved.contains(base);
}

// True if @p component is unsafe as a single Win32 path element beyond the separator/wildcard
// checks the callers already apply: a trailing dot or space (Win32 silently strips these, so
// "foo " aliases "foo"), an embedded control character, or a reserved device name.
bool hasUnsafeComponentTraits(const QString& component) {
    if (component.endsWith(QLatin1Char('.')) || component.endsWith(QLatin1Char(' '))) {
        return true;
    }
    if (std::ranges::any_of(component, [](const QChar ch) {
            return ch.unicode() < kFirstNonControlCodePoint;
        })) {
        return true;
    }
    return isWindowsReservedDeviceName(component);
}

// True if @p child is equal to, or nested under, @p parent (lexical comparison,
// case-insensitive on Windows). Used to refuse a backup destination inside a source profile,
// which would otherwise recursively copy the growing backup into itself.
bool pathIsWithin(const QString& child, const QString& parent) {
    if (child.isEmpty() || parent.isEmpty()) {
        return false;
    }
    const QString c = QDir::cleanPath(QFileInfo(child).absoluteFilePath());
    QString p = QDir::cleanPath(QFileInfo(parent).absoluteFilePath());
    if (c.isEmpty() || p.isEmpty()) {
        return false;
    }
#ifdef Q_OS_WIN
    const Qt::CaseSensitivity cs = Qt::CaseInsensitive;
#else
    const Qt::CaseSensitivity cs = Qt::CaseSensitive;
#endif
    if (c.compare(p, cs) == 0) {
        return true;
    }
    if (!p.endsWith(QLatin1Char('/'))) {
        p.append(QLatin1Char('/'));
    }
    return c.startsWith(p, cs);
}

// True when @p item's realized PARENT directory (junctions/symlinks fully resolved) is at or
// under @p canonicalRoot, which must itself be a canonicalFilePath. The per-entry leaf reparse
// check catches @p item itself, but an ANCESTOR directory that is (or becomes) a junction after it
// was descended into would redirect the read or write outside the root while the leaf still looks
// clean; re-checking the realized parent on every entry catches that. Fails closed on an empty
// root or an unresolvable parent. canonicalFilePath yields '/'-paths, so the trailing-'/' test
// rejects a sibling-prefix (Root vs RootX). Mirrors the restore worker's
// destinationParentWithinRoot.
bool parentWithinCanonicalRoot(const QString& canonicalRoot, const QString& item) {
    if (canonicalRoot.isEmpty()) {
        return false;
    }
    const QString canonicalParent = QFileInfo(QFileInfo(item).absolutePath()).canonicalFilePath();
    if (canonicalParent.isEmpty()) {
        return false;
    }
    return canonicalParent == canonicalRoot ||
           canonicalParent.startsWith(canonicalRoot + QLatin1Char('/'));
}
}  // namespace

UserProfileBackupWorker::UserProfileBackupWorker(QObject* parent)
    : QThread(parent)
    , m_fileFilter(new SmartFileFilter())
    , m_permissionManager(new PermissionManager()) {}

UserProfileBackupWorker::~UserProfileBackupWorker() {
    if (isRunning()) {
        cancel();
        // Never let ~QThread run on a live thread: if an in-flight copy of a
        // large/stalled file outlasts the grace period, block until it finishes.
        if (!wait(kTimeoutThreadTerminateMs)) {
            // SAK-ALLOW-BLOCKING: the bounded wait above already gave up once. Destroying a
            // running QThread aborts the process, and terminate() during a file copy can
            // leave a half-written file in the backup, so blocking is the safer end state.
            wait();
        }
    }
    delete m_fileFilter;
    delete m_permissionManager;
}

void UserProfileBackupWorker::applySmartFilterRules(const SmartFilter& smartFilter) {
    m_fileFilter->setRules(smartFilter);
    // An exclude pattern that fails to compile is dropped, so files the user meant
    // to exclude WILL be backed up (fail-open). Surface it rather than silently
    // backing up what they asked to skip (B8-23).
    const QStringList invalidPatterns = m_fileFilter->invalidPatterns();
    if (!invalidPatterns.isEmpty()) {
        Q_EMIT logMessage(tr("Ignoring %1 invalid exclusion pattern(s); matching files will NOT "
                             "be excluded from the backup: %2")
                              .arg(invalidPatterns.size())
                              .arg(invalidPatterns.join(QStringLiteral("; "))),
                          true);
    }
}

void UserProfileBackupWorker::startBackup(const BackupManifest& manifest,
                                          const QVector<UserProfile>& users,
                                          const QString& destinationPath,
                                          const SmartFilter& smartFilter,
                                          const BackupOptions& options) {
    // No asserts on these. An empty user list is a backup of nothing, which completes
    // with an empty manifest, and an empty destination fails the path validation below.
    // Asserting either would abort a Debug build on input that Release accepts, which is
    // what made the mirror-image restore worker unable to run its own tests in Debug.
    if (isRunning()) {
        Q_EMIT logMessage(tr("Backup already in progress"), true);
        return;
    }

    // Refuse BEFORE starting rather than aborting mid-run. Encryption with no password is
    // the one case that must never degrade to a plaintext copy, and refusing here means
    // the technician is told while they can still fix it.
    if (options.encrypt && options.password.isEmpty()) {
        Q_EMIT logMessage(tr("Encryption was requested without a password; backup not started"),
                          true);
        Q_EMIT backupComplete(false, tr("Encryption requires a password"), manifest);
        return;
    }

    const QMutexLocker locker(&m_mutex);

    // Re-check under the lock: two callers can both pass the unlocked isRunning() check above
    // before either starts the thread, then race to overwrite this shared configuration and
    // double-start. The winner sets config and start()s while holding the lock; a loser sees
    // isRunning()==true here and backs out without clobbering the live run.
    if (isRunning()) {
        return;
    }

    m_manifest = manifest;
    m_users = users;
    m_destinationPath = destinationPath;
    m_smartFilter = smartFilter;
    m_permissionMode = options.permission_mode;
    m_codec.compress = options.compress;
    m_codec.compression_level = options.compression_level;
    m_codec.encrypt = options.encrypt;
    m_codec.password = options.password;
    m_manifest.compressed = options.compress;
    m_manifest.encrypted = options.encrypt;

    m_cancelled = false;
    m_bytesCopied = 0;
    m_filesCopied = 0;
    m_filesSkipped = 0;
    m_filesErrored = 0;
    m_filesElevationSkipped = 0;
    m_lastProgressFileCount = 0;
    m_lastProgressByteCount = 0;
    m_currentUserProfile.clear();
    m_currentSourceRoot.clear();
    m_currentDestRoot.clear();

    applySmartFilterRules(smartFilter);

    start();
}

void UserProfileBackupWorker::cancel() {
    m_cancelled = true;
    Q_EMIT logMessage(tr("Canceling backup..."), false);
}

void UserProfileBackupWorker::run() {
    // See startBackup: an empty user list backs up nothing and is not an abort.
    Q_EMIT logMessage(tr("=== Backup Started ==="), false);
    Q_EMIT logMessage(tr("Destination: %1").arg(m_destinationPath), false);
    Q_EMIT logMessage(tr("Users to backup: %1").arg(m_users.size()), false);

    // Report only what is actually applied. The old wizard logged "Encryption: Enabled
    // (AES-256)" and then refused to encrypt, and logged a compression level it never
    // used; these lines are emitted from the same options the codec is driven by.
    Q_EMIT logMessage(m_codec.compress
                          ? tr("Compression: zlib level %1").arg(m_codec.compression_level)
                          : tr("Compression: off"),
                      false);
    Q_EMIT logMessage(m_codec.encrypt ? tr("Encryption: AES-256 per file (file names and sizes "
                                           "remain visible)")
                                      : tr("Encryption: off"),
                      false);

    // Validate inputs
    if (!validateSourcePaths()) {
        Q_EMIT backupComplete(false, tr("Invalid source paths"), m_manifest);
        return;
    }

    // Calculate the total size first so the disk-space check has a real
    // requirement to compare against (an empty total would always pass).
    Q_EMIT logMessage(tr("Calculating total size..."), false);
    m_totalBytesToCopy = calculateTotalSize();
    Q_EMIT logMessage(tr("Total estimated size: %1 GB")
                          .arg(static_cast<double>(m_totalBytesToCopy) / sak::kBytesPerGBf,
                               0,
                               'f',
                               kBackupSizeDisplayPrecision),
                      false);

    // Check disk space
    if (!checkDiskSpace()) {
        Q_EMIT backupComplete(false, tr("Insufficient disk space"), m_manifest);
        return;
    }

    // Create backup directory structure
    if (!createBackupStructure()) {
        Q_EMIT backupComplete(false, tr("Failed to create backup structure"), m_manifest);
        return;
    }

    // Backup each user
    backupAllUsers();

    // A cancel may have been requested mid-copy; report failure and never fall
    // through to the success summary (exactly one terminal signal is emitted).
    if (m_cancelled) {
        Q_EMIT backupComplete(false, tr("Backup cancelled"), m_manifest);
        return;
    }

    // Save manifest and emit summary
    emitBackupSummary();
}

void UserProfileBackupWorker::backupAllUsers() {
    // An empty list simply iterates zero times; see startBackup.
    int userIndex = 0;
    for (const auto& user : m_users) {
        if (m_cancelled) {
            // run() owns the single terminal backupComplete signal; just stop.
            Q_EMIT logMessage(tr("Backup cancelled by user"), true);
            return;
        }

        if (!user.is_selected) {
            continue;
        }

        // The username becomes a backup subdirectory. Reject a crafted name ("..\\..\\Windows",
        // "C:\\evil") so it can never escape the destination root -- fail closed (counts as an
        // error so the backup is not reported as a clean success).
        if (!isSafePathSegment(user.username)) {
            Q_EMIT logMessage(tr("Skipping user with unsafe name: %1").arg(user.username), true);
            ++m_filesErrored;
            continue;
        }

        Q_EMIT statusUpdate(user.username, tr("Starting backup..."));
        Q_EMIT logMessage(tr("=== Backing up user: %1 ===").arg(user.username), false);

        const QString userBackupPath = m_destinationPath + "/" + user.username;
        if (!backupUser(user, userBackupPath)) {
            Q_EMIT logMessage(tr("Failed to backup user: %1").arg(user.username), true);
            // Continue with other users
        }

        userIndex++;
        Q_EMIT overallProgress(
            userIndex, static_cast<int>(m_users.size()), m_bytesCopied, m_totalBytesToCopy);
    }
}

void UserProfileBackupWorker::emitBackupSummary() {
    // Save manifest
    Q_EMIT logMessage(tr("Saving backup manifest..."), false);
    const bool manifestSaved = saveManifest();
    if (!manifestSaved) {
        Q_EMIT logMessage(tr("Warning: Failed to save manifest"), true);
    }

    // Complete
    const QString summary =
        tr("Backup complete!\nFiles copied: %1\nFiles skipped: %2\n"
           "Skipped (elevation required): %3\nErrors: %4\nTotal "
           "size: %5 MB")
            .arg(m_filesCopied)
            .arg(m_filesSkipped)
            .arg(m_filesElevationSkipped)
            .arg(m_filesErrored)
            .arg(static_cast<double>(m_bytesCopied) / sak::kBytesPerMBf, 0, 'f', 1);

    Q_EMIT logMessage(tr("=== Backup Complete ==="), false);
    Q_EMIT logMessage(summary, false);
    // Report success only when the manifest was written AND nothing was left out:
    // any hard error OR an elevation-required skip means the backup is incomplete
    // (some selected data was not captured), so it must not read as a clean success
    // (B7-20). Intentional filter skips (m_filesSkipped: caches, *.tmp) do not count.
    const bool success = (m_filesErrored == 0) && (m_filesElevationSkipped == 0) && manifestSaved;
    Q_EMIT backupComplete(success, summary, m_manifest);
}

bool UserProfileBackupWorker::backupUser(const UserProfile& user, const QString& userBackupPath) {
    // An empty userBackupPath fails the directory creation below and is reported.
    // Filter exclusion rules are keyed on THIS user's profile path (B7-34).
    m_currentUserProfile = user.profile_path;
    // Create user backup directory
    const QDir dir;
    if (!dir.mkpath(userBackupPath)) {
        Q_EMIT logMessage(tr("Failed to create directory: %1").arg(userBackupPath), true);
        // A user whose backup root cannot be created backed up nothing; count it as
        // an error so the run is not reported as a clean success (B7-20).
        ++m_filesErrored;
        return false;
    }

    // Pin the CANONICAL source-profile and destination-backup roots for this user (both
    // directories exist now: the profile was verified by validateSourcePaths and userBackupPath
    // was just created). Every enumerated entry re-checks its realized parent against these so an
    // ancestor directory that is (or becomes) a junction cannot redirect a read out of the profile
    // or a write out of the backup, mirroring the restore worker's destinationParentWithinRoot
    // (R5-P7-36). Fail closed if either root cannot be resolved.
    m_currentSourceRoot = QFileInfo(user.profile_path).canonicalFilePath();
    m_currentDestRoot = QFileInfo(userBackupPath).canonicalFilePath();
    if (m_currentSourceRoot.isEmpty() || m_currentDestRoot.isEmpty()) {
        Q_EMIT logMessage(
            tr("Unable to resolve canonical backup roots for user: %1").arg(user.username), true);
        ++m_filesErrored;
        return false;
    }

    // Backup each selected folder
    for (const auto& folder : user.folder_selections) {
        if (m_cancelled) {
            return false;
        }

        if (!folder.selected) {
            continue;
        }

        // relative_path is joined onto BOTH the source profile and the destination roots; a "../"
        // in it would read outside the profile and write outside the backup. Reject traversal.
        if (!isSafeRelativePath(folder.relative_path)) {
            Q_EMIT logMessage(tr("Skipping folder with unsafe path: %1").arg(folder.relative_path),
                              true);
            ++m_filesErrored;
            continue;
        }

        Q_EMIT statusUpdate(user.username, tr("Backing up: %1").arg(folder.display_name));

        const QString sourcePath = user.profile_path + "/" + folder.relative_path;
        const QString destPath = userBackupPath + "/" + folder.relative_path;

        if (!backupFolder(folder, sourcePath, destPath)) {
            Q_EMIT logMessage(tr("Warning: Failed to backup folder: %1").arg(folder.display_name),
                              true);
            // Continue with other folders
        }
    }

    return true;
}

bool UserProfileBackupWorker::backupFolder(const FolderSelection& folder,
                                           const QString& sourcePath,
                                           const QString& destPath) {
    // An empty sourcePath fails the exists() check below; see startBackup.
    const QFileInfo sourceInfo(sourcePath);

    if (!sourceInfo.exists()) {
        Q_EMIT logMessage(tr("Source does not exist: %1").arg(sourcePath), true);
        // A selected folder that is missing at backup time is an error, not a silent
        // success: the resulting backup omits data the user asked to keep (B7-20).
        ++m_filesErrored;
        return false;
    }

    // A reparse point (symlink/junction) must never be followed: a directory one loops or escapes
    // the profile, and a FILE one would copy the content of whatever it targets (e.g. a symlink to
    // a privileged system file) into the backup. Skip both, not just directories.
    if (isReparsePoint(sourcePath)) {
        // A selected folder that is (or resolves through) a reparse point is NOT backed up -- we
        // refuse to follow the link. Recording it as a benign skip would let the run report a clean
        // success while omitting data the user asked to keep, so count it as an error, matching the
        // missing-selected-folder case above and the restore worker's refused-reparse handling
        // (B7-20).
        Q_EMIT logMessage(
            tr("Refusing to follow reparse point (not backed up): %1").arg(sourcePath), true);
        ++m_filesErrored;
        return false;
    }

    if (sourceInfo.isDir()) {
        return copyDirectory(sourcePath, destPath, folder);
    }
    if (sourceInfo.isFile()) {
        return copyFileWithFiltering(sourcePath, destPath, sourceInfo.size());
    }

    // Exists but is neither a regular file nor a directory (device/pipe/etc.): it
    // could not be backed up, so record it rather than returning a silent failure.
    Q_EMIT logMessage(tr("Unsupported source type: %1").arg(sourcePath), true);
    ++m_filesErrored;
    return false;
}

bool UserProfileBackupWorker::copyDirectory(const QString& sourceDir,
                                            const QString& destDir,
                                            const FolderSelection& folderConfig,
                                            int depth) {
    // Bound the recursion: a crafted deep tree must not exhaust the worker thread's stack.
    // Beyond the cap this is a hard error (counted so the run cannot report a clean success),
    // not a silent skip.
    if (depth > kMaxCopyDepth) {
        Q_EMIT logMessage(tr("Directory nesting exceeds the %1-level limit; refusing to recurse "
                             "further: %2")
                              .arg(kMaxCopyDepth)
                              .arg(sourceDir),
                          true);
        ++m_filesErrored;
        return false;
    }

    // Empty directories fail the checks below; see startBackup.
    // Check if folder should be excluded
    const QFileInfo sourceDirInfo(sourceDir);
    const QString& currentUserProfile = m_currentUserProfile;

    if (m_fileFilter->shouldExcludeFolder(sourceDirInfo, currentUserProfile)) {
        const QString reason = m_fileFilter->getExclusionReason(sourceDirInfo);
        Q_EMIT logMessage(tr("Skipping folder: %1 (%2)").arg(sourceDir, reason), false);
        m_filesSkipped++;
        return true;  // Not an error, just skipped
    }

    // Check if directory is accessible (cross-user folders may need elevation)
    if (!canReadPath(sourceDir)) {
        Q_EMIT logMessage(tr("Skipping folder (requires elevation): %1").arg(sourceDir), true);
        Q_EMIT elevationSkipped(sourceDir, QString());
        m_filesElevationSkipped++;
        return true;
    }

    // QDirIterator reports a directory it cannot open exactly like an empty one (hasNext() is
    // immediately false), so an enumeration/open failure would copy nothing yet still succeed.
    // canReadPath only ruled out the elevation case; confirm the directory is actually listable
    // so a genuine failure is counted, not mistaken for an empty tree (R5-P7-35).
    if (!QDir(sourceDir).isReadable()) {
        Q_EMIT logMessage(tr("Failed to enumerate source directory: %1").arg(sourceDir), true);
        ++m_filesErrored;
        return false;
    }

    // Create destination directory
    if (!createDirectory(destDir)) {
        // Counted here (once) as the leaf failure; callers only log/propagate so the
        // error is not double-counted (B7-20).
        ++m_filesErrored;
        return false;
    }

    // Iterate through directory contents
    QDirIterator it(sourceDir,
                    QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System);

    while (it.hasNext()) {
        if (m_cancelled) {
            return false;
        }
        copyDirectoryEntry(it.next(), destDir, folderConfig, depth + 1);
    }

    return true;
}

void UserProfileBackupWorker::copyDirectoryEntry(const QString& sourceItem,
                                                 const QString& destDir,
                                                 const FolderSelection& folderConfig,
                                                 int depth) {
    const QFileInfo itemInfo(sourceItem);
    const QString destItem = destDir + "/" + itemInfo.fileName();

    // Never follow a reparse point (junction/symlink), whether it is a directory OR a file: a
    // directory one can loop back into the profile or point outside it (foreign/privileged data),
    // and a FILE symlink would pull the content of its target (e.g. a system file) into the backup.
    if (isReparsePoint(sourceItem)) {
        // A reparse-point child is not followed, so it is not backed up. Count it as an error
        // rather than an untracked silent skip so the run cannot report a clean success while
        // omitting the linked subtree/file (B7-20).
        Q_EMIT logMessage(
            tr("Refusing to follow reparse point (not backed up): %1").arg(sourceItem), true);
        ++m_filesErrored;
        return;
    }

    // Re-validate the realized parent of BOTH sides against the pinned canonical roots: the leaf
    // reparse check above misses an ANCESTOR directory that is (or became) a junction after it was
    // descended into, which would redirect this read out of the profile or this write out of the
    // backup. Fail closed on any escape (R5-P7-36).
    if (!parentWithinCanonicalRoot(m_currentSourceRoot, sourceItem) ||
        !parentWithinCanonicalRoot(m_currentDestRoot, destItem)) {
        Q_EMIT logMessage(
            tr("Refusing entry: path escapes the pinned backup root (reparse point): %1")
                .arg(sourceItem),
            true);
        ++m_filesErrored;
        return;
    }

    if (itemInfo.isDir()) {
        if (!copyDirectory(sourceItem, destItem, folderConfig, depth)) {
            Q_EMIT logMessage(tr("Warning: Failed to copy directory: %1").arg(sourceItem), true);
        }
        return;
    }

    if (itemInfo.isFile()) {
        copyFileWithFiltering(sourceItem, destItem, itemInfo.size());
    }
}

bool UserProfileBackupWorker::copyFileWithFiltering(const QString& sourcePath,
                                                    const QString& destPath,
                                                    qint64 fileSize) {
    // Empty paths fail the file operations below; see startBackup. fileSize stays
    // asserted because it is an invariant this class guarantees: it comes from
    // QFileInfo::size(), which reports 0 - never a negative - for a file it
    // cannot stat.
    Q_ASSERT_X(fileSize >= 0, "copyFileWithFiltering", "fileSize must be non-negative");
    const QFileInfo sourceInfo(sourcePath);
    const QString& currentUserProfile = m_currentUserProfile;

    // Apply smart filtering
    if (m_fileFilter->shouldExcludeFile(sourceInfo, currentUserProfile)) {
        const QString reason = m_fileFilter->getExclusionReason(sourceInfo);
        Q_EMIT logMessage(tr("Skipping file: %1 (%2)").arg(sourceInfo.fileName(), reason), false);
        m_filesSkipped++;
        return true;  // Not an error
    }

    // Check size limit
    if (m_fileFilter->exceedsSizeLimit(fileSize)) {
        Q_EMIT logMessage(tr("Skipping large file: %1 (%2 MB)")
                              .arg(sourceInfo.fileName())
                              .arg(static_cast<double>(fileSize) / sak::kBytesPerMBf, 0, 'f', 1),
                          true);
        m_filesSkipped++;
        return true;
    }

    // Check if file is accessible (cross-user files may require elevation)
    if (!canReadPath(sourcePath)) {
        QString owner;
        if (!ElevationManager::isElevated()) {
            owner = m_permissionManager->getOwner(sourcePath);
        }
        Q_EMIT logMessage(tr("Skipping file (requires elevation): %1").arg(sourceInfo.fileName()),
                          true);
        Q_EMIT elevationSkipped(sourcePath, owner);
        m_filesElevationSkipped++;
        return true;
    }

    // Ensure destination directory exists
    const QFileInfo destInfo(destPath);
    if (!createDirectory(destInfo.absolutePath())) {
        // The file cannot be written without its parent dir; count it (B7-20).
        Q_EMIT logMessage(tr("Failed to create directory for: %1").arg(destPath), true);
        ++m_filesErrored;
        return false;
    }

    if (!storeFile(sourcePath, destPath)) {
        m_filesErrored++;
        return false;
    }

    // Apply permission strategy
    if (!applyPermissions(destPath)) {
        Q_EMIT logMessage(tr("Warning: Failed to adjust permissions: %1").arg(destPath), true);
        // Continue anyway
    }

    // Update progress
    m_filesCopied++;
    updateProgress(fileSize);

    return true;
}

bool UserProfileBackupWorker::storeFile(const QString& sourcePath, const QString& destPath) {
    if (m_codec.isPassThrough()) {
        if (QFile::copy(sourcePath, destPath)) {
            return true;
        }
        Q_EMIT logMessage(tr("Error copying file: %1").arg(sourcePath), true);
        return false;
    }

    // Cancellation is polled inside the codec too, so a multi-gigabyte file does not have
    // to finish before a Stop is noticed.
    auto written = sak::writeBackupFile(sourcePath, destPath, m_codec, [this]() {
        return m_cancelled.load();
    });
    if (written.has_value()) {
        return true;
    }
    if (written.error() == sak::error_code::operation_cancelled) {
        // Not an error to report per-file: run() emits the single cancelled outcome.
        return false;
    }
    Q_EMIT logMessage(tr("Error writing file: %1 (%2)")
                          .arg(sourcePath,
                               QString::fromUtf8(sak::to_string(written.error()).data())),
                      true);
    return false;
}

bool UserProfileBackupWorker::applyPermissions(const QString& filePath) {
    // m_permissionManager stays asserted: it is owned and constructed by this
    // class, so a null there means the object is corrupt. filePath is not
    // asserted - an empty one fails inside the permission manager and is
    // reported.
    Q_ASSERT(m_permissionManager);
    switch (m_permissionMode) {
    case PermissionMode::StripAll:
        return m_permissionManager->stripPermissions(filePath);

    case PermissionMode::PreserveOriginal:
        // Do nothing, keep original permissions
        return true;

    case PermissionMode::AssignToDestination:
        // This would be done during restore, not backup
        return true;
    }

    return true;
}

bool UserProfileBackupWorker::createBackupStructure() {
    const QDir dir;

    // Create main backup directory
    if (!dir.mkpath(m_destinationPath)) {
        Q_EMIT logMessage(tr("Failed to create backup directory: %1").arg(m_destinationPath), true);
        return false;
    }

    Q_EMIT logMessage(tr("Created backup directory: %1").arg(m_destinationPath), false);
    return true;
}

bool UserProfileBackupWorker::saveManifest() {
    // Update manifest with actual results
    m_manifest.total_backup_size_bytes = m_bytesCopied;
    m_manifest.created = QDateTime::currentDateTime();

    // Add user data to manifest
    m_manifest.users.clear();
    for (const auto& user : m_users) {
        if (!user.is_selected) {
            continue;
        }

        BackupUserData userData;
        userData.username = user.username;
        userData.sid = user.sid;
        userData.profile_path = user.profile_path;
        userData.backed_up_folders = user.folder_selections;
        userData.permissions_mode = m_permissionMode;
        // Digest the user's backed-up payload so restore can detect corruption or
        // tampering of the stored files (B7-13).
        userData.checksum_sha256 =
            BackupManifest::hashDirectoryTree(m_destinationPath + "/" + user.username);

        m_manifest.users.append(userData);
    }

    // Seal the manifest with a self-excluding digest over its final content (users
    // + their checksums must already be set) so a modified manifest.json is caught
    // at restore.
    m_manifest.manifest_checksum.clear();
    m_manifest.manifest_checksum = m_manifest.computeManifestChecksum();

    // Save to JSON file
    const QString manifestPath = m_destinationPath + "/manifest.json";
    return m_manifest.saveToFile(manifestPath);
}

qint64 UserProfileBackupWorker::accumulateUserFolderSizes(const UserProfile& user) {
    qint64 size = 0;
    for (const auto& folder : user.folder_selections) {
        if (!folder.selected) {
            continue;
        }
        size += folder.size_bytes;
        m_totalFilesToCopy += folder.file_count;
    }
    return size;
}

qint64 UserProfileBackupWorker::calculateTotalSize() {
    qint64 totalSize = 0;
    m_totalFilesToCopy = 0;

    for (const auto& user : m_users) {
        if (!user.is_selected) {
            continue;
        }
        totalSize += accumulateUserFolderSizes(user);
    }

    return totalSize;
}

void UserProfileBackupWorker::countSelectedUsers(int& currentUser, int& totalUsers) const {
    totalUsers = 0;
    currentUser = 0;
    for (int i = 0; i < m_users.size(); ++i) {
        if (!m_users[i].is_selected) {
            continue;
        }
        totalUsers++;
        if (i < m_users.size() - 1) {
            currentUser++;
        }
    }
}

void UserProfileBackupWorker::updateProgress(qint64 bytesAdded) {
    // Invariant: the sole caller (copyFileWithFiltering) forwards its fileSize, which both of
    // its call sites take from QFileInfo::size() -- 0, never negative, for an unstattable file.
    Q_ASSERT(bytesAdded >= 0);
    m_bytesCopied += bytesAdded;

    if (m_filesCopied - m_lastProgressFileCount >= kProgressEmitFileInterval ||
        m_bytesCopied - m_lastProgressByteCount >= kProgressEmitByteInterval) {
        int currentUser = 0, totalUsers = 0;
        countSelectedUsers(currentUser, totalUsers);

        Q_EMIT overallProgress(currentUser, totalUsers, m_bytesCopied, m_totalBytesToCopy);
        Q_EMIT fileProgress(m_filesCopied, m_totalFilesToCopy);

        m_lastProgressFileCount = m_filesCopied;
        m_lastProgressByteCount = m_bytesCopied;
    }
}

bool UserProfileBackupWorker::createDirectory(const QString& path) {
    // Fail closed on a destination that resolves through a symlink/junction. mkpath would otherwise
    // happily create the tree INSIDE a planted link, redirecting the elevated copy outside the
    // technician-chosen backup root (e.g. into System32). Both call sites pass destination paths.
    if (destinationRedirectedThroughReparsePoint(m_destinationPath, path)) {
        Q_EMIT logMessage(
            tr("Refusing backup directory (path resolves through a symlink/junction): %1")
                .arg(path),
            true);
        return false;
    }
    const QDir dir;
    if (!dir.mkpath(path)) {
        Q_EMIT logMessage(tr("Failed to create directory: %1").arg(path), true);
        return false;
    }
    return true;
}

bool UserProfileBackupWorker::canReadPath(const QString& path) {
#ifdef Q_OS_WIN
    const std::wstring wide = path.toStdWString();
    DWORD const attrs = GetFileAttributesW(wide.c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES) {
        // This predicate answers ONLY "is the path blocked by permissions such that
        // elevation might help" -- its single caller emits "requires elevation" and counts an
        // elevation-skip on false. A denied/sharing-locked path is such a block; a merely
        // missing path (ERROR_PATH_NOT_FOUND) is not, and must not be mislabelled as one, so
        // it reports readable and the copy logic handles the absence.
        DWORD const err = GetLastError();
        return err != ERROR_ACCESS_DENIED && err != ERROR_SHARING_VIOLATION;
    }
    // Confirm read access by actually opening it; a denial here is the elevation case, while
    // any other open failure is left to the copy path rather than reported as needing rights.
    HANDLE handle =
        CreateFileW(wide.c_str(),
                    GENERIC_READ,
                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                    nullptr,
                    OPEN_EXISTING,
                    ((attrs & FILE_ATTRIBUTE_DIRECTORY) != 0u) ? FILE_FLAG_BACKUP_SEMANTICS : 0,
                    nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        return GetLastError() != ERROR_ACCESS_DENIED;
    }
    CloseHandle(handle);
    return true;
#else
    return QFileInfo(path).isReadable();
#endif
}

bool UserProfileBackupWorker::isSafePathSegment(const QString& segment) {
    if (segment.isEmpty() || segment == QStringLiteral(".") || segment == QStringLiteral("..")) {
        return false;
    }
    static const QString kForbidden = QStringLiteral("/\\:*?\"<>|");
    if (std::ranges::any_of(segment, [](const QChar c) { return kForbidden.contains(c); })) {
        return false;
    }
    // An untrusted username becomes a real backup subdirectory: reject trailing dot/space,
    // control characters, and Win32 reserved device names too, any of which Win32 name
    // normalization could alias onto a different target.
    if (hasUnsafeComponentTraits(segment)) {
        return false;
    }
    return true;
}

bool UserProfileBackupWorker::isSafeRelativePath(const QString& rel) {
    if (rel.isEmpty() || QDir::isAbsolutePath(rel) || rel.contains(QLatin1Char(':'))) {
        return false;  // absolute (C:\ or /x) or drive-relative (C:foo)
    }
    if (rel.startsWith(QLatin1Char('/')) || rel.startsWith(QLatin1Char('\\'))) {
        return false;  // leading slash / UNC
    }
    const QStringList parts = QDir::fromNativeSeparators(rel).split(QLatin1Char('/'),
                                                                    Qt::SkipEmptyParts);
    for (const QString& part : parts) {
        if (part == QStringLiteral(".") || part == QStringLiteral("..")) {
            return false;  // traversal component
        }
        // Same Win32 aliasing hazards as a username component: a trailing dot/space, a
        // control character, or a reserved device name in any element.
        if (hasUnsafeComponentTraits(part)) {
            return false;
        }
    }
    return true;
}

QString UserProfileBackupWorker::extendedLengthPath(const QString& absoluteNativePath) {
    // Already extended-length (either "\\?\C:\..." or "\\?\UNC\server\..."): prefixing again
    // would produce a path that resolves to nothing.
    if (absoluteNativePath.startsWith(QStringLiteral("\\\\?\\"))) {
        return absoluteNativePath;
    }
    // A UNC path keeps its server\share, but the two leading backslashes are replaced by the
    // "\\?\UNC\" prefix rather than kept alongside it.
    if (absoluteNativePath.startsWith(QStringLiteral("\\\\"))) {
        return QStringLiteral("\\\\?\\UNC\\") + absoluteNativePath.mid(kUncLeadingBackslashCount);
    }
    return QStringLiteral("\\\\?\\") + absoluteNativePath;
}

bool UserProfileBackupWorker::validateSourcePaths() {
    for (const auto& user : m_users) {
        if (!user.is_selected) {
            continue;
        }

        const QDir profileDir(user.profile_path);
        if (!profileDir.exists()) {
            Q_EMIT logMessage(tr("User profile does not exist: %1").arg(user.profile_path), true);
            return false;
        }

        // Refuse a destination that sits inside a source profile: backing that profile up
        // would then recurse into the growing backup directory and never terminate. Fail
        // closed before any bytes are copied.
        if (pathIsWithin(m_destinationPath, user.profile_path)) {
            Q_EMIT logMessage(tr("Backup destination is inside the source profile '%1' and would "
                                 "recursively copy itself; choose a destination outside the "
                                 "profile")
                                  .arg(user.profile_path),
                              true);
            return false;
        }
    }

    return true;
}

bool UserProfileBackupWorker::checkDiskSpace() {
    const QStorageInfo storage(m_destinationPath);

    if (!storage.isValid() || !storage.isReady()) {
        Q_EMIT logMessage(tr("Invalid or not ready destination: %1").arg(m_destinationPath), true);
        return false;
    }

    const qint64 availableBytes = storage.bytesAvailable();
    qint64 requiredBytes = m_totalBytesToCopy;

    requiredBytes = qRound64(static_cast<double>(requiredBytes) * kDiskSpaceSafetyMultiplier);

    // A negative requirement means the summed folder sizes were corrupt (a negative or
    // overflowed size_bytes). A negative would compare as "always enough space" and slip
    // under the check below, so reject it and fail closed instead.
    if (requiredBytes < 0) {
        Q_EMIT logMessage(tr("Computed backup size is invalid (%1 bytes); refusing to proceed")
                              .arg(requiredBytes),
                          true);
        return false;
    }

    if (availableBytes < requiredBytes) {
        const double availableGB = static_cast<double>(availableBytes) / sak::kBytesPerGBf;
        const double requiredGB = static_cast<double>(requiredBytes) / sak::kBytesPerGBf;

        Q_EMIT logMessage(tr("Insufficient disk space. Available: %1 GB, Required: %2 GB")
                              .arg(availableGB, 0, 'f', kDiskSpaceDisplayPrecision)
                              .arg(requiredGB, 0, 'f', kDiskSpaceDisplayPrecision),
                          true);
        return false;
    }

    Q_EMIT logMessage(tr("Disk space check passed"), false);
    return true;
}

}  // namespace sak
