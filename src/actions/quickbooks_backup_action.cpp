// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: MIT

#include "sak/actions/quickbooks_backup_action.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QDirIterator>
#include <QStandardPaths>
#include <QDateTime>

#ifdef _WIN32
#include <windows.h>
#endif

namespace sak {

QuickBooksBackupAction::QuickBooksBackupAction(const QString& backup_location)
    : QuickAction(nullptr)
    , m_backup_location(backup_location) {
}

void QuickBooksBackupAction::scan() {
    setStatus(ActionStatus::Scanning);
    m_found_files.clear();
    m_total_bytes = 0;

    scanCommonLocations();

    // Build summary
    QString summary;
    if (!m_found_files.empty()) {
        int qbw_count = 0;
        int qbb_count = 0;
        int other_count = 0;

        for (const auto& file : m_found_files) {
            if (file.type == "QBW") qbw_count++;
            else if (file.type == "QBB") qbb_count++;
            else other_count++;
        }

        summary = QString("Found %1 company files, %2 backups, %3 other files")
                     .arg(qbw_count)
                     .arg(qbb_count)
                     .arg(other_count);
    } else {
        summary = "No QuickBooks files found";
    }

    QString warning;
    bool has_open_files = false;
    for (const auto& file : m_found_files) {
        if (file.is_open) {
            has_open_files = true;
            break;
        }
    }

    if (has_open_files) {
        warning = "Some files may be open in QuickBooks. Close QuickBooks before backing up.";
    }

    // Estimate duration (10MB per second)
    qint64 estimated_ms = (m_total_bytes / (10 * 1024 * 1024)) * 1000;
    if (estimated_ms < 1000) {
        estimated_ms = 1000;
    }

    ScanResult result;
    result.applicable = !m_found_files.empty();
    result.summary = summary;
    result.bytes_affected = m_total_bytes;
    result.files_count = static_cast<int>(m_found_files.size());
    result.estimated_duration_ms = estimated_ms;
    result.warning = warning;

    setScanResult(result);
    setStatus(ActionStatus::Ready);
    Q_EMIT scanComplete(result);
}

void QuickBooksBackupAction::execute() {
    if (isCancelled()) {
        return;
    }

    setStatus(ActionStatus::Running);

    // Create backup directory
    QString timestamp = QDateTime::currentDateTime().toString("yyyyMMdd_HHmmss");
    QString backup_dir = m_backup_location + "/QuickBooks_" + timestamp;
    QDir dir;
    if (!dir.mkpath(backup_dir)) {
        ExecutionResult result;
        result.success = false;
        result.message = "Failed to create backup directory";
        setExecutionResult(result);
        setStatus(ActionStatus::Failed);
        Q_EMIT executionComplete(result);
        return;
    }

    qint64 total_copied = 0;
    int files_copied = 0;
    QDateTime start_time = QDateTime::currentDateTime();

    for (const auto& file : m_found_files) {
        if (isCancelled()) {
            setStatus(ActionStatus::Cancelled);
            ExecutionResult result;
            result.success = false;
            result.message = "Backup cancelled by user";
            result.bytes_processed = total_copied;
            result.duration_ms = start_time.msecsTo(QDateTime::currentDateTime());
            result.output_path = backup_dir;
            setExecutionResult(result);
            Q_EMIT executionComplete(result);
            return;
        }

        // Skip open files
        if (file.is_open) {
            continue;
        }

        QString dest_path = backup_dir + "/" + file.filename;
        if (copyFileWithProgress(file.path, dest_path)) {
            total_copied += file.size;
            files_copied++;
        }
    }

    qint64 duration_ms = start_time.msecsTo(QDateTime::currentDateTime());

    ExecutionResult result;
    result.success = files_copied > 0;
    result.message = QString("Backed up %1 files").arg(files_copied);
    result.bytes_processed = total_copied;
    result.duration_ms = duration_ms;
    result.files_processed = files_copied;
    result.output_path = backup_dir;
    result.log = QString("Copied %1 QuickBooks files to %2 in %3ms")
                    .arg(files_copied)
                    .arg(backup_dir)
                    .arg(duration_ms);

    setExecutionResult(result);
    setStatus(result.success ? ActionStatus::Success : ActionStatus::Failed);
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
#ifdef _WIN32
    // Try to open with exclusive access
    HANDLE handle = CreateFileW(
        reinterpret_cast<LPCWSTR>(file_path.utf16()),
        GENERIC_READ,
        0, // No sharing
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        nullptr
    );

    if (handle == INVALID_HANDLE_VALUE) {
        DWORD error = GetLastError();
        CloseHandle(handle);
        return error == ERROR_SHARING_VIOLATION;
    }

    CloseHandle(handle);
    return false;
#else
    Q_UNUSED(file_path)
    return false;
#endif
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
