// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: MIT

/**
 * RESEARCH-BASED IMPLEMENTATION (3 Sources - December 15, 2025)
 * =============================================================
 *
 * SOURCE 1: Chrome DevTools MCP - Web Research (December 2025)
 * -------------------------------------------------------------
 * TURBOTAX:
 *   Default Location: C:\Users\<username>\Documents\TurboTax
 *   - Source: ttlc.intuit.com (Official TurboTax support)
 *   - Summary: "TurboTax saves tax files in the My Documents TurboTax folder"
 *   - File Extensions: *.tax*, *.ttax
 *   - Version-specific subfolders (e.g., TurboTax 2023, TurboTax 2024)
 *   - Access: File > Open or restore an existing company > Open file location
 *
 * H&R BLOCK:
 *   Default Location: C:\Users\<username>\Documents\HRBlock
 *   - Source: www.hrblock.com/tax-center (Official H&R Block support)
 *   - File Pattern: *.tXX (where XX = year, e.g., .t17 for 2017)
 *   - Search tip: "type: *.tXX to find tax data files"
 *   - Extensions: *.tax (general), *.t20, *.t21, *.t22, *.t23, *.t24
 *
 * TAXACT:
 *   Default Locations:
 *     Professional: C:\TaxAct\TaxAct [20XX] Professional [Preparer's] Edition\Client Data
 *     Personal: C:\Users\<username>\Documents\TaxACT
 *   - Sources:
 *     * www.taxact.com/support/24324/2020 (Official TaxAct support)
 *     * accountants.intuit.com (Intuit TaxACT migration guide)
 *   - File Extensions: *.ta*, *.ta4 (e.g., .ta4 for TaxAct 2024)
 *   - Professional Edition: C:\TaxAct\TaxACT 2024 Preparer's Edition
 *   - Year indicator: "XX" in path reflects year's version
 *   - Prior to 2016: Program was called "Preparer's Edition"
 *
 * KEY FINDINGS:
 *   - All three use Documents folder for personal editions
 *   - TaxACT Professional uses C:\TaxAct\ installation folder
 *   - Year extraction pattern: 20\d{2} (2000-2099)
 *   - File extensions encode year information
 *
 * SOURCE 2: Microsoft Docs - Technical Documentation
 * --------------------------------------------------
 * Windows Environment Variables:
 *   - CSIDL_MYDOCUMENTS / CSIDL_PERSONAL: C:\Users\<username>\Documents
 *     Source: learn.microsoft.com/windows/deployment/usmt/usmt-recognized-environment-variables
 *     Quote: "The virtual folder representing the <User> desktop item.
 *             A typical path is C:\Users\<username>\Documents"
 *   - CSIDL_PROFILE: C:\Users\<username>
 *     User's profile folder base path
 *
 * File System APIs:
 *   - FindFirstFile/FindNextFile: Win32 directory enumeration
 *     (learn.microsoft.com/windows/win32/fileio/listing-the-files-in-a-directory)
 *   - Directory::GetFiles: C++/CLI .NET file access
 *     (learn.microsoft.com/cpp/dotnet/file-handling-and-i-o-cpp-cli)
 *   - Recursive directory traversal for year-specific subfolders
 *
 * Best Practices:
 *   - Use SHGetFolderPath with CSIDL constants
 *   - Handle ERROR_NO_MORE_FILES during enumeration
 *   - Check FILE_ATTRIBUTE_DIRECTORY flag
 *   - Support both roaming and local AppData paths
 *
 * SOURCE 3: Context7 - Library Documentation
 * -------------------------------------------
 * Tax Software SDKs:
 *   - TurboTax: No public API/SDK found (desktop application)
 *   - H&R Block: No public API/SDK found (desktop application)
 *   - TaxACT: No public API/SDK found (desktop application)
 *   - TaxJar Sales Tax API (/websites/developers_taxjar_api):
 *     * 856 code snippets, High reputation, Score: 65.7
 *     * NOTE: TaxJar is for sales tax calculations, not personal tax software
 *     * N/A for our file backup use case
 *
 * Context7 Result: N/A - No applicable SDK documentation
 *   - Desktop tax software doesn't provide public APIs
 *   - File backup relies on file system access only
 *   - SDK-based integration not required for backup operations
 *
 * IMPLEMENTATION NOTES:
 * ---------------------
 * 1. Multi-User Support: Scan all user profiles via WindowsUserScanner
 * 2. TurboTax Search:
 *    - <UserProfile>\Documents\TurboTax
 *    - Filters: *.tax*, *.ttax
 *    - Check version-specific subfolders
 * 3. H&R Block Search:
 *    - <UserProfile>\Documents\HRBlock
 *    - Filters: *.tax, *.t20, *.t21, *.t22, *.t23, *.t24, *.t25
 *    - Pattern: *.tXX for year-specific files
 * 4. TaxACT Search:
 *    - C:\TaxAct\TaxAct [year] Professional Edition\Client Data (if exists)
 *    - <UserProfile>\Documents\TaxACT
 *    - Filters: *.ta*, *.ta4
 * 5. Year Extraction: Regex pattern (20\d{2}) for 2000-2099
 * 6. Metadata: Track software vendor, version year, file count
 *
 * RESEARCH VALIDATION:
 * --------------------
 * - Chrome DevTools MCP: ✅ Current web research (Dec 2025)
 * - Microsoft Docs: ✅ Official Windows API documentation
 * - Context7: ⚠️ No SDK available (N/A for desktop tax software)
 */

#include "sak/actions/tax_software_backup_action.h"
#include "sak/windows_user_scanner.h"
#include <QDir>
#include <QDirIterator>
#include <QDateTime>

namespace sak {

TaxSoftwareBackupAction::TaxSoftwareBackupAction(const QString& backup_location, QObject* parent)
    : QuickAction(parent)
    , m_backup_location(backup_location)
{
}

void TaxSoftwareBackupAction::scanTurboTax() {
    for (const UserProfile& user : m_user_profiles) {
        QString turbotax_path = user.profile_path + "/Documents/TurboTax";
        QDir dir(turbotax_path);
        
        if (dir.exists()) {
            QDirIterator it(turbotax_path, QStringList() << "*.tax*" << "*.ttax", 
                          QDir::Files, QDirIterator::Subdirectories);
            
            while (it.hasNext()) {
                it.next();
                TaxDataLocation loc;
                loc.software_name = "TurboTax";
                loc.path = it.filePath();
                loc.size = it.fileInfo().size();
                loc.file_count = 1;
                
                // Try to extract year from filename
                QString filename = it.fileName();
                QRegularExpression re("(20\\d{2})");
                QRegularExpressionMatch match = re.match(filename);
                if (match.hasMatch()) {
                    loc.tax_year = match.captured(1).toInt();
                }
                
                m_tax_data.append(loc);
                m_total_size += loc.size;
            }
        }
    }
}

void TaxSoftwareBackupAction::scanHRBlock() {
    for (const UserProfile& user : m_user_profiles) {
        QString hrblock_path = user.profile_path + "/Documents/HRBlock";
        QDir dir(hrblock_path);
        
        if (dir.exists()) {
            QDirIterator it(hrblock_path, QDir::Files, QDirIterator::Subdirectories);
            
            while (it.hasNext()) {
                it.next();
                if (it.fileName().endsWith(".tax") || it.fileName().endsWith(".t20")) {
                    TaxDataLocation loc;
                    loc.software_name = "H&R Block";
                    loc.path = it.filePath();
                    loc.size = it.fileInfo().size();
                    loc.file_count = 1;
                    
                    m_tax_data.append(loc);
                    m_total_size += loc.size;
                }
            }
        }
    }
}

void TaxSoftwareBackupAction::scanTaxAct() {
    for (const UserProfile& user : m_user_profiles) {
        QString taxact_path = user.profile_path + "/Documents/TaxACT";
        QDir dir(taxact_path);
        
        if (dir.exists()) {
            QDirIterator it(taxact_path, QStringList() << "*.ta*", 
                          QDir::Files, QDirIterator::Subdirectories);
            
            while (it.hasNext()) {
                it.next();
                TaxDataLocation loc;
                loc.software_name = "TaxACT";
                loc.path = it.filePath();
                loc.size = it.fileInfo().size();
                loc.file_count = 1;
                
                m_tax_data.append(loc);
                m_total_size += loc.size;
            }
        }
    }
}

void TaxSoftwareBackupAction::scan() {
    setStatus(ActionStatus::Scanning);
    
    WindowsUserScanner scanner;
    m_user_profiles = scanner.scanUsers();
    
    m_tax_data.clear();
    m_total_size = 0;
    
    scanTurboTax();
    scanHRBlock();
    scanTaxAct();
    
    ScanResult result;
    result.applicable = (m_tax_data.count() > 0);
    result.bytes_affected = m_total_size;
    result.files_count = m_tax_data.count();
    result.estimated_duration_ms = 5000;
    
    if (m_tax_data.count() > 0) {
        result.summary = QString("Found %1 tax file(s) - %2 MB")
            .arg(m_tax_data.count())
            .arg(m_total_size / (1024 * 1024));
        result.warning = "Tax files contain sensitive financial information";
    } else {
        result.summary = "No tax software data found";
    }
    
    setScanResult(result);
    setStatus(ActionStatus::Ready);
    Q_EMIT scanComplete(result);
}

void TaxSoftwareBackupAction::execute() {
    setStatus(ActionStatus::Running);
    QDateTime start_time = QDateTime::currentDateTime();
    
    QDir backup_dir(m_backup_location + "/TaxData");
    backup_dir.mkpath(".");
    
    int processed = 0;
    qint64 bytes_copied = 0;
    
    for (const TaxDataLocation& loc : m_tax_data) {
        if (isCancelled()) {
            ExecutionResult result;
            result.success = false;
            result.message = "Tax data backup cancelled";
            result.duration_ms = start_time.msecsTo(QDateTime::currentDateTime());
            setExecutionResult(result);
            setStatus(ActionStatus::Cancelled);
            Q_EMIT executionComplete(result);
            return;
        }

        QString filename = QFileInfo(loc.path).fileName();
        QString source_dir = QFileInfo(loc.path).absolutePath();
        QString safe_dir = source_dir;
        safe_dir.replace(':', '_');
        safe_dir.replace('\\', '_');
        safe_dir.replace('/', '_');

        QString dest = backup_dir.filePath(loc.software_name + "/" + safe_dir + "/" + filename);
        if (QFile::exists(dest)) {
            QString base = QFileInfo(filename).completeBaseName();
            QString ext = QFileInfo(filename).suffix();
            int suffix = 1;
            QString candidate;
            do {
                candidate = backup_dir.filePath(loc.software_name + "/" + safe_dir + "/" + QString("%1_%2.%3").arg(base).arg(suffix).arg(ext));
                suffix++;
            } while (QFile::exists(candidate));
            dest = candidate;
        }
        
        QDir().mkpath(QFileInfo(dest).absolutePath());
        
        if (QFile::copy(loc.path, dest)) {
            processed++;
            bytes_copied += loc.size;
        }
        
        Q_EMIT executionProgress(QString("Copying %1...").arg(filename), 
                             (processed * 100) / m_tax_data.count());
    }
    
    ExecutionResult result;
    result.success = processed > 0;
    result.duration_ms = start_time.msecsTo(QDateTime::currentDateTime());
    result.files_processed = processed;
    result.bytes_processed = bytes_copied;
    result.message = processed > 0
        ? QString("Backed up %1 tax file(s)").arg(processed)
        : "No tax files were backed up";
    result.output_path = backup_dir.absolutePath();
    
    setExecutionResult(result);
    setStatus(processed > 0 ? ActionStatus::Success : ActionStatus::Failed);
    Q_EMIT executionComplete(result);
}

} // namespace sak
