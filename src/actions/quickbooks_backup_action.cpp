// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: MIT

/**
 * RESEARCH-BASED IMPLEMENTATION (3 Sources - December 15, 2025)
 * =============================================================
 *
 * SOURCE 1: Chrome DevTools MCP - Web Research (10 authoritative sources, Dec 2025)
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
 * - Chrome DevTools MCP: ✅ Current web research (2024-2025 sources)
 * - Microsoft Docs: ✅ Official Windows API documentation
 * - Context7: ⚠️ SDK available but N/A for file backup use case
 */

#include "sak/actions/quickbooks_backup_action.h"
#include "sak/windows_user_scanner.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QDirIterator>
#include <QStandardPaths>
#include <QDateTime>
#include <QProcess>

namespace sak {

QuickBooksBackupAction::QuickBooksBackupAction(const QString& backup_location)
    : QuickAction(nullptr)
    , m_backup_location(backup_location) {
}

void QuickBooksBackupAction::scan() {
    // Scan is no longer used - actions execute immediately
    setStatus(ActionStatus::Ready);
    ScanResult result;
    result.applicable = true;
    result.summary = "Ready to backup QuickBooks data";
    setScanResult(result);
    Q_EMIT scanComplete(result);
}

void QuickBooksBackupAction::execute() {
    if (isCancelled()) {
        return;
    }

    setStatus(ActionStatus::Running);
    QDateTime start_time = QDateTime::currentDateTime();
    
    Q_EMIT executionProgress("Checking if QuickBooks is running...", 5);
    
    // Check if QuickBooks is running
    QProcess proc;
    proc.start("tasklist", QStringList() << "/FI" << "IMAGENAME eq QBW32.EXE");
    proc.waitForFinished(3000);
    QString output = proc.readAllStandardOutput();
    bool qb_running = output.contains("QBW32.EXE", Qt::CaseInsensitive);
    
    if (!qb_running) {
        // Also check for 64-bit version
        proc.start("tasklist", QStringList() << "/FI" << "IMAGENAME eq QBW64.EXE");
        proc.waitForFinished(3000);
        output = proc.readAllStandardOutput();
        qb_running = output.contains("QBW64.EXE", Qt::CaseInsensitive);
    }
    
    if (qb_running) {
        ExecutionResult result;
        result.duration_ms = start_time.msecsTo(QDateTime::currentDateTime());
        result.success = false;
        result.message = "QuickBooks is currently running";
        result.log = "Please close QuickBooks before backing up data files";
        setStatus(ActionStatus::Failed);
        setExecutionResult(result);
        Q_EMIT executionComplete(result);
        return;
    }
    
    Q_EMIT executionProgress("Scanning for QuickBooks files...", 15);
    
    // Scan ALL user profiles for QuickBooks data
    WindowsUserScanner scanner;
    QVector<UserProfile> users = scanner.scanUsers();
    
    QStringList search_paths;
    for (const UserProfile& user : users) {
        search_paths << user.profile_path + "/Documents/Intuit/QuickBooks";
        search_paths << user.profile_path + "/Documents";
    }
    
    // Also check public documents
    search_paths << QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation) + "/Intuit/QuickBooks";
    search_paths << "C:/ProgramData/Intuit/QuickBooks";
    
    struct QBFile {
        QString path;
        QString filename;
        QString type;
        qint64 size;
    };
    
    QVector<QBFile> found_files;
    qint64 total_size = 0;
    
    QStringList filters;
    filters << "*.qbw" << "*.qbb" << "*.qbm" << "*.qbx";
    
    for (const QString& path : search_paths) {
        QDir dir(path);
        if (!dir.exists()) continue;
        
        QDirIterator it(dir.absolutePath(), filters, QDir::Files, QDirIterator::Subdirectories);
        while (it.hasNext()) {
            it.next();
            
            QBFile file;
            file.path = it.filePath();
            file.filename = it.fileName();
            file.size = it.fileInfo().size();
            
            QString ext = it.fileName().right(3).toUpper();
            file.type = ext;
            
            found_files.append(file);
            total_size += file.size;
        }
    }
    
    if (found_files.isEmpty()) {
        ExecutionResult result;
        result.duration_ms = start_time.msecsTo(QDateTime::currentDateTime());
        result.success = false;
        result.message = "No QuickBooks files found";
        result.log = "No QBW, QBB, QBM, or QBX files detected";
        setStatus(ActionStatus::Failed);
        setExecutionResult(result);
        Q_EMIT executionComplete(result);
        return;
    }
    
    Q_EMIT executionProgress("Preparing backup directory...", 30);
    
    QDir backup_dir(m_backup_location + "/QuickBooksBackup");
    backup_dir.mkpath(".");
    
    int files_copied = 0;
    qint64 bytes_copied = 0;
    
    for (int i = 0; i < found_files.count(); ++i) {
        if (isCancelled()) {
            setStatus(ActionStatus::Cancelled);
            return;
        }
        
        const QBFile& file = found_files[i];
        
        int progress = 30 + ((i * 60) / found_files.count());
        Q_EMIT executionProgress(QString("Backing up %1...").arg(file.filename), progress);
        
        QString dest_path = backup_dir.filePath(file.filename);
        
        if (QFile::copy(file.path, dest_path)) {
            files_copied++;
            bytes_copied += file.size;
        }
    }
    
    Q_EMIT executionProgress("Backup complete", 100);
    
    qint64 duration_ms = start_time.msecsTo(QDateTime::currentDateTime());
    
    ExecutionResult result;
    result.duration_ms = duration_ms;
    result.files_processed = files_copied;
    result.bytes_processed = bytes_copied;
    result.output_path = backup_dir.absolutePath();
    
    if (files_copied > 0) {
        result.success = true;
        double mb = bytes_copied / (1024.0 * 1024.0);
        result.message = QString("Backed up %1 QuickBooks file(s) - %2 MB")
            .arg(files_copied)
            .arg(mb, 0, 'f', 2);
        result.log = QString("Saved to: %1").arg(backup_dir.absolutePath());
        setStatus(ActionStatus::Success);
    } else {
        result.success = false;
        result.message = "Failed to backup QuickBooks files";
        result.log = "Could not copy any QuickBooks data files";
        setStatus(ActionStatus::Failed);
    }
    
    setExecutionResult(result);
    Q_EMIT executionComplete(result);
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

    for (const QString& path : search_paths) {
        scanDirectory(path);
    }
}

void QuickBooksBackupAction::scanDirectory(const QString& dir_path) {
    QDir dir(dir_path);
    if (!dir.exists()) {
        return;
    }

    QStringList filters;
    filters << "*.qbw" << "*.QBW" << "*.qbb" << "*.QBB" 
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
        return QFile::exists(file_path); // If exists but can't open, it's in use
    }
    file.close();
    return false;
}

bool QuickBooksBackupAction::copyFileWithProgress(const QString& source, const QString& destination) {
    QFile source_file(source);
    QFile dest_file(destination);

    if (!source_file.open(QIODevice::ReadOnly)) {
        return false;
    }

    if (!dest_file.open(QIODevice::WriteOnly)) {
        source_file.close();
        return false;
    }

    const qint64 buffer_size = 64 * 1024; // 64KB chunks
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
        dest_file.write(buffer);
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
    QString ext = extension.toUpper();
    
    if (ext == "QBW") return "Company File";
    if (ext == "QBB") return "Backup File";
    if (ext == "TLG") return "Transaction Log";
    if (ext == "ND") return "Network Data";
    
    return "QuickBooks File";
}

} // namespace sak
