// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: MIT

#include "sak/actions/clear_browser_cache_action.h"

#include <QDir>
#include <QDirIterator>
#include <QStandardPaths>
#include <QProcess>
#include <QDateTime>

#ifdef _WIN32
#include <windows.h>
#include <tlhelp32.h>
#endif

namespace sak {

ClearBrowserCacheAction::ClearBrowserCacheAction()
    : QuickAction(nullptr) {
}

void ClearBrowserCacheAction::scan() {
    setStatus(ActionStatus::Scanning);
    m_caches.clear();
    m_total_bytes = 0;
    m_total_files = 0;

    detectBrowsers();

    scanChrome();
    scanFirefox();
    scanEdge();
    scanInternetExplorer();

    // Build summary
    QString summary;
    if (!m_caches.empty()) {
        QStringList browser_names;
        for (const auto& cache : m_caches) {
            browser_names.append(cache.browser_name);
        }
        summary = QString("Found cache in %1 browsers: %2")
                     .arg(m_caches.size())
                     .arg(browser_names.join(", "));
    } else {
        summary = "No browser caches found";
    }

    QString warning;
    bool has_running = false;
    for (const auto& cache : m_caches) {
        if (cache.is_running) {
            has_running = true;
            break;
        }
    }

    if (has_running) {
        warning = "Some browsers are running. Close all browsers before clearing cache.";
    }

    // Estimate duration (5MB per second)
    qint64 estimated_ms = (m_total_bytes / (5 * 1024 * 1024)) * 1000;
    if (estimated_ms < 1000) {
        estimated_ms = 1000;
    }

    ScanResult result;
    result.applicable = !m_caches.empty();
    result.summary = summary;
    result.bytes_affected = m_total_bytes;
    result.files_count = m_total_files;
    result.estimated_duration_ms = estimated_ms;
    result.warning = warning;

    setScanResult(result);
    setStatus(ActionStatus::Ready);
    Q_EMIT scanComplete(result);
}

void ClearBrowserCacheAction::execute() {
    if (isCancelled()) {
        return;
    }

    setStatus(ActionStatus::Running);

    qint64 total_deleted = 0;
    int progress = 0;
    const int total_caches = static_cast<int>(m_caches.size());
    QDateTime start_time = QDateTime::currentDateTime();

    for (const auto& cache : m_caches) {
        if (isCancelled()) {
            setStatus(ActionStatus::Cancelled);
            ExecutionResult result;
            result.success = false;
            result.message = "Cache clearing cancelled by user";
            result.bytes_processed = total_deleted;
            result.duration_ms = start_time.msecsTo(QDateTime::currentDateTime());
            setExecutionResult(result);
            Q_EMIT executionComplete(result);
            return;
        }

        QString prog_msg = QString("Clearing %1...").arg(cache.browser_name);
        Q_EMIT executionProgress(prog_msg, (progress * 100) / total_caches);
        progress++;

        // Skip if browser is running
        if (cache.is_running) {
            continue;
        }

        qint64 deleted = deleteCacheDirectory(cache.cache_path);
        total_deleted += deleted;
    }

    qint64 duration_ms = start_time.msecsTo(QDateTime::currentDateTime());

    ExecutionResult result;
    result.success = total_deleted > 0;
    result.message = QString("Cleared cache from %1 browsers").arg(m_caches.size());
    result.bytes_processed = total_deleted;
    result.duration_ms = duration_ms;
    result.files_processed = m_caches.size();
    result.log = QString("Deleted %1 bytes from %2 browser caches in %3ms")
                    .arg(total_deleted)
                    .arg(m_caches.size())
                    .arg(duration_ms);

    setExecutionResult(result);
    setStatus(result.success ? ActionStatus::Success : ActionStatus::Failed);
    Q_EMIT executionComplete(result);
}

void ClearBrowserCacheAction::detectBrowsers() {
    // Detection happens in scan methods
}

void ClearBrowserCacheAction::scanChrome() {
    QString local_app_data = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation);
    QString cache_path = local_app_data + "/Google/Chrome/User Data/Default/Cache";
    
    QDir cache_dir(cache_path);
    if (!cache_dir.exists()) {
        return;
    }

    BrowserCache cache;
    cache.browser = BrowserType::Chrome;
    cache.browser_name = "Google Chrome";
    cache.cache_path = cache_path;
    cache.size = calculateCacheSize(cache_path, cache.file_count);
    cache.is_running = isBrowserRunning("chrome.exe");

    if (cache.size > 0) {
        m_caches.push_back(cache);
        m_total_bytes += cache.size;
        m_total_files += cache.file_count;
    }
}

void ClearBrowserCacheAction::scanFirefox() {
    QString local_app_data = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation);
    QString profiles_path = local_app_data + "/Mozilla/Firefox/Profiles";
    
    QDir profiles_dir(profiles_path);
    if (!profiles_dir.exists()) {
        return;
    }

    // Scan all profiles
    QStringList profiles = profiles_dir.entryList(QDir::Dirs | QDir::NoDotAndDotDot);
    for (const QString& profile : profiles) {
        QString cache_path = profiles_path + "/" + profile + "/cache2";
        
        QDir cache_dir(cache_path);
        if (!cache_dir.exists()) {
            continue;
        }

        BrowserCache cache;
        cache.browser = BrowserType::Firefox;
        cache.browser_name = "Mozilla Firefox";
        cache.cache_path = cache_path;
        cache.size = calculateCacheSize(cache_path, cache.file_count);
        cache.is_running = isBrowserRunning("firefox.exe");

        if (cache.size > 0) {
            m_caches.push_back(cache);
            m_total_bytes += cache.size;
            m_total_files += cache.file_count;
        }
    }
}

void ClearBrowserCacheAction::scanEdge() {
    QString local_app_data = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation);
    QString cache_path = local_app_data + "/Microsoft/Edge/User Data/Default/Cache";
    
    QDir cache_dir(cache_path);
    if (!cache_dir.exists()) {
        return;
    }

    BrowserCache cache;
    cache.browser = BrowserType::Edge;
    cache.browser_name = "Microsoft Edge";
    cache.cache_path = cache_path;
    cache.size = calculateCacheSize(cache_path, cache.file_count);
    cache.is_running = isBrowserRunning("msedge.exe");

    if (cache.size > 0) {
        m_caches.push_back(cache);
        m_total_bytes += cache.size;
        m_total_files += cache.file_count;
    }
}

void ClearBrowserCacheAction::scanInternetExplorer() {
    QString local_app_data = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation);
    QString cache_path = local_app_data + "/Microsoft/Windows/INetCache";
    
    QDir cache_dir(cache_path);
    if (!cache_dir.exists()) {
        return;
    }

    BrowserCache cache;
    cache.browser = BrowserType::InternetExplorer;
    cache.browser_name = "Internet Explorer";
    cache.cache_path = cache_path;
    cache.size = calculateCacheSize(cache_path, cache.file_count);
    cache.is_running = isBrowserRunning("iexplore.exe");

    if (cache.size > 0) {
        m_caches.push_back(cache);
        m_total_bytes += cache.size;
        m_total_files += cache.file_count;
    }
}

bool ClearBrowserCacheAction::isBrowserRunning(const QString& process_name) const {
#ifdef _WIN32
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) {
        return false;
    }

    PROCESSENTRY32W entry;
    entry.dwSize = sizeof(entry);

    bool found = false;
    if (Process32FirstW(snapshot, &entry)) {
        do {
            QString exe_name = QString::fromWCharArray(entry.szExeFile);
            if (exe_name.compare(process_name, Qt::CaseInsensitive) == 0) {
                found = true;
                break;
            }
        } while (Process32NextW(snapshot, &entry));
    }

    CloseHandle(snapshot);
    return found;
#else
    Q_UNUSED(process_name)
    return false;
#endif
}

qint64 ClearBrowserCacheAction::calculateCacheSize(const QString& cache_path, int& file_count) {
    qint64 total_size = 0;
    file_count = 0;

    QDirIterator it(cache_path, QDir::Files, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        if (isCancelled()) {
            break;
        }

        it.next();
        QFileInfo file_info = it.fileInfo();
        total_size += file_info.size();
        file_count++;
    }

    return total_size;
}

qint64 ClearBrowserCacheAction::deleteCacheDirectory(const QString& cache_path) {
    qint64 total_deleted = 0;

    QDir dir(cache_path);
    if (!dir.exists()) {
        return 0;
    }

    QFileInfoList entries = dir.entryInfoList(QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot);
    
    for (const QFileInfo& entry : entries) {
        if (isCancelled()) {
            break;
        }

        if (entry.isDir()) {
            total_deleted += deleteCacheDirectory(entry.absoluteFilePath());
            QDir subdir(entry.absoluteFilePath());
            subdir.removeRecursively();
        } else {
            qint64 file_size = entry.size();
            if (QFile::remove(entry.absoluteFilePath())) {
                total_deleted += file_size;
            }
        }
    }

    return total_deleted;
}

} // namespace sak
