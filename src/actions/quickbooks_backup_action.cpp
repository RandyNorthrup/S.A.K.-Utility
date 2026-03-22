// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file quickbooks_backup_action.cpp
/// @brief Implements QuickBooks company file backup and discovery

/**
 * RESEARCH-BASED IMPLEMENTATION (3 Sources - December 15, 2025)
 * ---------------------------------------------------------------------------------
 * Default QuickBooks File Locations:
 *   - Primary: C:\Users\Public\Documents\Intuit\QuickBooks\Company Files
 *   - Alternative: C:\Users\Public\Public Documents\Intuit\QuickBooks\Company Files
 *   - User-specific: C:\Users\<username>\Documents\Intuit\QuickBooks
 *
 * QuickBooks File Types:
 *   - .qbw = Company file (primary working file)
 *   - .qbb = Backup file (compressed)
 *   - .qbm = Portable company file (for moving between computers)
 *   - .qbx = Accountant's copy (for sharing with accountants)
 *
 * Visual Identification:
 *   - .qbw files have lime-greenish tint icon
 *   - F2 in QuickBooks shows Product Information with file location
 *
 * Network Paths:
 *   - Supports UNC paths: \\server\share\path\file.qbw
 *   - Can be relative paths on network servers
 *
 * Key Findings (2024-2025 sources):
 *   1. quickbooks.intuit.com (Dec 15, 2023): Official default path confirmed
 *   2. blog.accountinghelpline.com (Sep 24, 2025): Public Documents default
 *   3. dancingnumbers.com (Dec 5, 2025): Safe QBW file relocation guidance
 *   4. ticket.summithosting.com: Multiple default locations documented
 *   5. cleverence.com (Aug 17, 2025): QB 2023 default location update
 *   6. asquarecloudhosting.com (Jul 9, 2024): Path validation requirements
 *   7. kb.bullhorn.com (Sep 3, 2025): F2 shortcut for file path discovery
 *   8. success.procurify.com: Lowercase .qbw extension requirement
 *   9. quickbooks.intuit.com/en-ca: Canadian support documentation
 *   10. help.petrosoftinc.com: POS integration file path examples
 *
 * SOURCE 2: Microsoft Docs - Technical Documentation
 * --------------------------------------------------
 * Windows Environment Variables:
 *   - CSIDL_COMMON_DOCUMENTS: C:\Users\Public\Documents
 *     Typical path documented in learn.microsoft.com/windows/deployment/usmt
 *   - CSIDL_MYDOCUMENTS: C:\Users\<username>\Documents
 *     Per-user Documents folder
 *
 * File System APIs:
 *   - FindFirstFile/FindNextFile: Standard Win32 file enumeration
 *     (learn.microsoft.com/windows/win32/fileio/listing-the-files-in-a-directory)
 *   - Directory::GetFiles: .NET file system access
 *     (learn.microsoft.com/cpp/dotnet/file-handling-and-i-o-cpp-cli)
 *
 * Best Practices:
 *   - Use CSIDL constants for folder locations (avoids hardcoded paths)
 *   - Check FILE_ATTRIBUTE_DIRECTORY flag during enumeration
 *   - Handle ERROR_NO_MORE_FILES at enumeration completion
 *
 * SOURCE 3: Context7 - Library Documentation
 * -------------------------------------------
 * QuickBooks Desktop SDK: /websites/developer_intuit_app_developer_qbdesktop
 *   - 16,083 code snippets, High reputation, Score: 65.7
 *   - SDK provides API integration for QuickBooks Desktop & POS
 *   - NOTE: SDK is for API calls, not file backup
 *   - Our use case (file system scanning) doesn't require SDK
 *
 * IMPLEMENTATION NOTES:
 * ---------------------
 * 1. Process Detection: Check QBW32.EXE and QBW64.EXE before backup
 * 2. Multi-User Support: Scan all user profiles via WindowsUserScanner
 * 3. Search Locations:
 *    - <UserProfile>\Documents\Intuit\QuickBooks
 *    - C:\Users\Public\Documents\Intuit\QuickBooks
 *    - C:\ProgramData\Intuit\QuickBooks (company files)
 * 4. File Filters: *.qbw, *.qbb, *.qbm, *.qbx
 * 5. Network Considerations: UNC paths supported but may require special handling
 *
 * RESEARCH VALIDATION:
 * --------------------
 * - Chrome DevTools MCP: [OK] Current web research (2024-2025 sources)
 * - Microsoft Docs: [OK] Official Windows API documentation
 * - Context7: (!) SDK available but N/A for file backup use case
 */

#include "sak/actions/quickbooks_backup_action.h"

#include "sak/layout_constants.h"
#include "sak/logger.h"
#include "sak/process_runner.h"
#include "sak/windows_user_scanner.h"

#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QStandardPaths>

#include <algorithm>

namespace sak {

QuickBooksBackupAction::QuickBooksBackupAction(const QString& backup_location)
    : QuickAction(nullptr), m_backup_location(backup_location) {}

void QuickBooksBackupAction::scan() {
    setStatus(ActionStatus::Scanning);
    Q_ASSERT(status() == ActionStatus::Scanning);
    m_found_files.clear();
    m_total_bytes = 0;

    Q_EMIT scanProgress("Scanning for QuickBooks data files...");
    scanCommonLocations();

    ScanResult result;
    result.applicable = !m_found_files.empty();
    result.bytes_affected = m_total_bytes;
    result.files_count = static_cast<qint64>(m_found_files.size());
    result.estimated_duration_ms = std::max<qint64>(5000, m_total_bytes / (1024 * 10));

    if (!result.applicable) {
        result.summary = "No QuickBooks files found";
        result.details = "Check default QuickBooks locations or map network drives.";
        Q_ASSERT(!result.summary.isEmpty());
        setScanResult(result);
        setStatus(ActionStatus::Ready);
        Q_EMIT scanComplete(result);
        return;
    }

    double mb = m_total_bytes / sak::kBytesPerMBf;
    result.summary = QString("Found %1 files (%2 MB)").arg(result.files_count).arg(mb, 0, 'f', 1);

    int open_files = static_cast<int>(std::count_if(
        m_found_files.begin(), m_found_files.end(), [](const auto& file) { return file.is_open; }));

    if (open_files > 0) {
        result.warning = QString("%1 file(s) appear to be in use. Close QuickBooks before backup.")
                             .arg(open_files);
    }

    setScanResult(result);
    setStatus(ActionStatus::Ready);
    Q_EMIT scanComplete(result);
}

void QuickBooksBackupAction::execute() {
    if (isCancelled()) {
        emitCancelledResult("QuickBooks backup cancelled");
        return;
    }

    setStatus(ActionStatus::Running);
    Q_ASSERT(status() == ActionStatus::Running);
    QDateTime start_time = QDateTime::currentDateTime();
    Q_EMIT executionProgress("Checking if QuickBooks is running...", 5);

    if (isQuickBooksRunning()) {
        emitFailedResult("QuickBooks is currently running",
                         "Please close QuickBooks before backing up data files",
                         start_time);
        return;
    }

    Q_EMIT executionProgress("Scanning for QuickBooks files...", 15);

    m_found_files.clear();
    m_total_bytes = 0;
    scanCommonLocations();

    if (m_found_files.empty()) {
        emitFailedResult("No QuickBooks files found",
                         "No QBW, QBB, QBM, or QBX files detected",
                         start_time);
        return;
    }

    Q_EMIT executionProgress("Preparing backup directory...", 30);

    QDir backup_dir(m_backup_location + "/QuickBooksBackup");
    if (!backup_dir.mkpath(".")) {
        sak::logWarning("Failed to create QuickBooks backup directory: {}",
                        backup_dir.absolutePath().toStdString());
    }

    CopyStats copy_stats;
    executeCopyFiles(backup_dir, start_time, copy_stats);
    if (isCancelled()) {
        return;
    }

    executeBuildResult(start_time, backup_dir, copy_stats);
}

bool QuickBooksBackupAction::isQuickBooksRunning() {
    ProcessResult proc = runProcess("tasklist",
                                    QStringList() << "/FI" << "IMAGENAME eq QBW32.EXE",
                                    sak::kTimeoutThermalQueryMs);
    if (proc.std_out.contains("QBW32.EXE", Qt::CaseInsensitive)) {
        return true;
    }
    proc = runProcess("tasklist",
                      QStringList() << "/FI" << "IMAGENAME eq QBW64.EXE",
                      sak::kTimeoutThermalQueryMs);
    return proc.std_out.contains("QBW64.EXE", Qt::CaseInsensitive);
}

void QuickBooksBackupAction::executeCopyFiles(const QDir& backup_dir,
                                              const QDateTime& start_time,
                                              CopyStats& stats) {
    for (size_t i = 0; i < m_found_files.size(); ++i) {
        if (isCancelled()) {
            emitCancelledResult("QuickBooks backup cancelled", start_time);
            return;
        }

        const auto& file = m_found_files[i];

        int progress = 30 + static_cast<int>((i * 60) / m_found_files.size());
        Q_EMIT executionProgress(QString("Backing up %1...").arg(file.filename), progress);

        if (file.is_open) {
            stats.files_skipped_open++;
            continue;
        }

        QString source_dir = QFileInfo(file.path).absolutePath();
        QString safe_dir = sanitizePathForBackup(source_dir);

        QDir target_dir(backup_dir.filePath(safe_dir));
        if (!target_dir.mkpath(".")) {
            sak::logWarning("Failed to create QuickBooks backup subdirectory: {}",
                            target_dir.absolutePath().toStdString());
        }
        QString dest_path = target_dir.filePath(file.filename);

        if (copyFileWithProgress(file.path, dest_path)) {
            stats.files_copied++;
            stats.bytes_copied += file.size;
            stats.copied_files << dest_path;
        }
    }
}

void QuickBooksBackupAction::executeBuildResult(const QDateTime& start_time,
                                                const QDir& backup_dir,
                                                const CopyStats& stats) {
    Q_EMIT executionProgress("Backup complete", 100);

    qint64 duration_ms = start_time.msecsTo(QDateTime::currentDateTime());

    ExecutionResult result;
    result.duration_ms = duration_ms;
    result.files_processed = stats.files_copied;
    result.bytes_processed = stats.bytes_copied;
    result.output_path = backup_dir.absolutePath();

    if (stats.files_copied > 0) {
        result.success = true;
        result.message = QString("Backed up %1 QuickBooks file(s) - %2")
                             .arg(stats.files_copied)
                             .arg(formatFileSize(stats.bytes_copied));
        result.log = QString("Saved to: %1\nFiles:\n%2")
                         .arg(backup_dir.absolutePath())
                         .arg(stats.copied_files.join("\n"));

        if (stats.files_skipped_open > 0) {
            result.log +=
                QString("\n\nSkipped %1 file(s) currently in use").arg(stats.files_skipped_open);
        }
        finishWithResult(result, ActionStatus::Success);
    } else {
        result.success = false;
        result.message = "Failed to backup QuickBooks files";
        result.log = "Could not copy any QuickBooks data files";
        finishWithResult(result, ActionStatus::Failed);
    }
}

void QuickBooksBackupAction::scanCommonLocations() {
    QStringList search_paths;

    // Public documents
    search_paths.append("C:\\Users\\Public\\Documents\\Intuit\\QuickBooks");

    // User documents
    QString user_docs = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
    search_paths.append(user_docs + "/QuickBooks");
    search_paths.append(user_docs + "/Intuit/QuickBooks");

    // Company files
    search_paths.append("C:\\QuickBooks");
    search_paths.append("C:\\QB");

    // Scan all user profiles
    WindowsUserScanner scanner;
    QVector<UserProfile> users = scanner.scanUsers();
    for (const UserProfile& user : users) {
        search_paths << user.profile_path + "/Documents/Intuit/QuickBooks";
        search_paths << user.profile_path + "/Documents";
    }

    // ProgramData
    search_paths.append("C:/ProgramData/Intuit/QuickBooks");

    for (const QString& path : search_paths) {
        scanDirectory(path);
    }
}

void QuickBooksBackupAction::scanDirectory(const QString& dir_path) {
    Q_ASSERT(!dir_path.isEmpty());
    QDir dir(dir_path);
    if (!dir.exists()) {
        return;
    }

    QStringList filters;
    filters << "*.qbw" << "*.QBW" << "*.qbb" << "*.QBB"
            << "*.qbm" << "*.QBM" << "*.qbx" << "*.QBX"
            << "*.tlg" << "*.TLG" << "*.nd" << "*.ND";

    QDirIterator it(dir_path, filters, QDir::Files, QDirIterator::Subdirectories);

    while (it.hasNext()) {
        if (isCancelled()) {
            break;
        }

        it.next();
        QFileInfo file_info = it.fileInfo();

        QuickBooksFile qb_file;
        qb_file.path = file_info.absoluteFilePath();
        qb_file.filename = file_info.fileName();
        qb_file.type = file_info.suffix().toUpper();
        qb_file.size = file_info.size();
        qb_file.modified = file_info.lastModified();
        qb_file.is_open = isFileOpen(qb_file.path);

        m_found_files.push_back(qb_file);
        m_total_bytes += qb_file.size;
    }
}

bool QuickBooksBackupAction::isFileOpen(const QString& file_path) const {
    // Try to open file exclusively to detect if it's in use
    QFile file(file_path);
    if (!file.open(QIODevice::ReadOnly)) {
        // File cannot be opened - likely in use or doesn't exist
        return QFile::exists(file_path);  // If exists but can't open, it's in use
    }
    file.close();
    return false;
}

bool QuickBooksBackupAction::copyFileWithProgress(const QString& source,
                                                  const QString& destination) {
    QFile source_file(source);
    QFile dest_file(destination);

    if (!source_file.open(QIODevice::ReadOnly)) {
        return false;
    }

    if (!dest_file.open(QIODevice::WriteOnly)) {
        source_file.close();
        return false;
    }

    const qint64 buffer_size = 64 * 1024;  // 64KB chunks
    qint64 total_read = 0;
    qint64 file_size = source_file.size();

    while (!source_file.atEnd()) {
        if (isCancelled()) {
            source_file.close();
            dest_file.close();
            dest_file.remove();
            return false;
        }

        QByteArray buffer = source_file.read(buffer_size);
        if (buffer.isEmpty()) {
            break;
        }
        qint64 written = dest_file.write(buffer);
        if (written != buffer.size()) {
            sak::logError("File copy write failed: expected {} bytes, wrote {}",
                          static_cast<qint64>(buffer.size()),
                          written);
            return false;
        }
        total_read += buffer.size();

        // Progress update
        if (file_size > 0) {
            int progress = static_cast<int>((total_read * 100) / file_size);
            Q_EMIT executionProgress(QString("Copying..."), progress);
        }
    }

    source_file.close();
    dest_file.close();

    // Preserve timestamps
    QFileInfo source_info(source);
    dest_file.setFileTime(source_info.lastModified(), QFileDevice::FileModificationTime);

    return true;
}

QString QuickBooksBackupAction::getFileTypeDescription(const QString& extension) const {
    Q_ASSERT(!extension.isEmpty());
    QString ext = extension.toUpper();

    if (ext == "QBW") {
        return "Company File";
    }
    if (ext == "QBB") {
        return "Backup File";
    }
    if (ext == "QBM") {
        return "Portable File";
    }
    if (ext == "QBX") {
        return "Accountant Copy";
    }
    if (ext == "TLG") {
        return "Transaction Log";
    }
    if (ext == "ND") {
        return "Network Data";
    }

    return "QuickBooks File";
}

}  // namespace sak
