// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: MIT

#include "sak/license_scanner_worker.h"
#include "sak/logger.h"
#include <QSettings>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QTextStream>
#include <QRegularExpression>
#include <QStandardPaths>

namespace sak {

license_scanner_worker::license_scanner_worker(const config& cfg, QObject* parent)
    : WorkerBase(parent)
    , m_config(cfg)
{
}

auto license_scanner_worker::execute() -> std::expected<void, sak::error_code> {
    log_info("Starting license scan");
    
    m_found_licenses.clear();
    m_processed_keys.clear();
    
    int total_sources = 0;
    if (m_config.scan_registry) ++total_sources;
    if (m_config.scan_filesystem) ++total_sources;
    if (!m_config.additional_paths.isEmpty()) ++total_sources;
    
    int current_source = 0;
    
    // Scan Windows Registry for license keys
    if (m_config.scan_registry) {
        Q_EMIT scan_progress(++current_source, total_sources, "Scanning Windows Registry");
        
        if (check_stop()) {
            log_info("License scan cancelled during registry scan");
            return std::unexpected(sak::error_code::operation_cancelled);
        }
        
        auto registry_result = scan_registry();
        if (registry_result.has_value()) {
            for (const auto& license : registry_result.value()) {
                if (!check_and_mark_duplicate(license)) {
                    m_found_licenses.append(license);
                    Q_EMIT license_found(license.product_name, license.license_key);
                }
            }
        }
    }
    
    // Scan filesystem for license files
    if (m_config.scan_filesystem) {
        Q_EMIT scan_progress(++current_source, total_sources, "Scanning filesystem");
        
        if (check_stop()) {
            log_info("License scan cancelled during filesystem scan");
            return std::unexpected(sak::error_code::operation_cancelled);
        }
        
        auto filesystem_result = scan_filesystem();
        if (filesystem_result.has_value()) {
            for (const auto& license : filesystem_result.value()) {
                if (!check_and_mark_duplicate(license)) {
                    m_found_licenses.append(license);
                    Q_EMIT license_found(license.product_name, license.license_key);
                }
            }
        }
    }
    
    // Scan additional user-specified paths
    if (!m_config.additional_paths.isEmpty()) {
        Q_EMIT scan_progress(++current_source, total_sources, "Scanning additional paths");
        
        if (check_stop()) {
            log_info("License scan cancelled during additional paths scan");
            return std::unexpected(sak::error_code::operation_cancelled);
        }
        
        auto locations_result = scan_common_locations();
        if (locations_result.has_value()) {
            for (const auto& license : locations_result.value()) {
                if (!check_and_mark_duplicate(license)) {
                    m_found_licenses.append(license);
                    Q_EMIT license_found(license.product_name, license.license_key);
                }
            }
        }
    }
    
    Q_EMIT scan_complete(m_found_licenses.size());
    log_info("License scan complete. Found {} licenses", m_found_licenses.size());
    return {};
}

auto license_scanner_worker::scan_registry() -> std::expected<QVector<license_info>, sak::error_code> {
    QVector<license_info> licenses;
    
    // Common registry paths where software stores license information
    QStringList registry_paths = {
        "HKEY_LOCAL_MACHINE\\SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion",
        "HKEY_LOCAL_MACHINE\\SOFTWARE\\WOW6432Node",
        "HKEY_CURRENT_USER\\SOFTWARE"
    };
    
    for (const QString& base_path : registry_paths) {
        if (check_stop()) break;
        
        QSettings settings(base_path, QSettings::NativeFormat);
        QStringList groups = settings.childGroups();
        
        for (const QString& group : groups) {
            if (check_stop()) break;
            
            settings.beginGroup(group);
            QStringList keys = settings.allKeys();
            
            for (const QString& key : keys) {
                // Look for common license key field names
                if (key.contains("license", Qt::CaseInsensitive) ||
                    key.contains("productkey", Qt::CaseInsensitive) ||
                    key.contains("serial", Qt::CaseInsensitive) ||
                    key.contains("activation", Qt::CaseInsensitive)) {
                    
                    QString value = settings.value(key).toString();
                    if (!value.isEmpty() && is_valid_license_key(value)) {
                        license_info info;
                        info.product_name = group;
                        info.license_key = normalize_license_key(value);
                        info.registry_path = base_path + "\\" + group;
                        info.version = settings.value("Version", "Unknown").toString();
                        info.installation_path = settings.value("InstallPath", "").toString();
                        info.is_valid = m_config.validate_keys ? true : true; // Simplified validation
                        
                        licenses.append(info);
                    }
                }
            }
            
            settings.endGroup();
        }
    }
    
    return licenses;
}

std::expected<QVector<license_scanner_worker::license_info>, sak::error_code> 
license_scanner_worker::scan_filesystem() {
    QVector<license_info> licenses;
    
    for (const QString& path : m_config.additional_paths) {
        if (check_stop()) break;
        
        QDir dir(path);
        if (!dir.exists()) continue;
        
        QDirIterator it(path, QStringList() << "*.lic" << "*.key" << "license.txt" << "*.license",
                       QDir::Files, QDirIterator::Subdirectories);
        
        while (it.hasNext()) {
            if (check_stop()) break;
            
            QString file_path = it.next();
            QFile file(file_path);
            
            if (file.open(QIODevice::ReadOnly | QIODevice::Text)) {
                QTextStream in(&file);
                QString content = in.readAll();
                file.close();
                
                // Extract potential license keys using pattern matching
                QRegularExpression key_pattern(R"([A-Z0-9]{5}-[A-Z0-9]{5}-[A-Z0-9]{5}-[A-Z0-9]{5}|[A-Z0-9]{4}-[A-Z0-9]{4}-[A-Z0-9]{4}-[A-Z0-9]{4})");
                QRegularExpressionMatchIterator matches = key_pattern.globalMatch(content);
                
                while (matches.hasNext()) {
                    QRegularExpressionMatch match = matches.next();
                    QString key = match.captured(0);
                    
                    if (is_valid_license_key(key)) {
                        license_info info;
                        info.product_name = QFileInfo(file_path).dir().dirName();
                        info.license_key = normalize_license_key(key);
                        info.installation_path = QFileInfo(file_path).absolutePath();
                        info.registry_path = "";
                        info.version = "Unknown";
                        info.is_valid = true;
                        
                        licenses.append(info);
                    }
                }
            }
        }
    }
    
    return licenses;
}

std::expected<QVector<license_scanner_worker::license_info>, sak::error_code> 
license_scanner_worker::scan_common_locations() {
    QVector<license_info> licenses;
    
    // Common locations where software stores license files
    QStringList common_paths;
    
    if (m_config.include_system_licenses) {
        common_paths << QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
        common_paths << QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
        common_paths << "C:/ProgramData";
        common_paths << "C:/Program Files";
        common_paths << "C:/Program Files (x86)";
    }
    
    common_paths << QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
    
    for (const QString& base_path : common_paths) {
        if (check_stop()) break;
        
        QDir dir(base_path);
        if (!dir.exists()) continue;
        
        // Look for common license file patterns
        QDirIterator it(base_path, 
                       QStringList() << "*.lic" << "*.key" << "license.txt" << "*.license" << "activation.dat",
                       QDir::Files,
                       QDirIterator::Subdirectories);
        
        int file_count = 0;
        while (it.hasNext() && file_count < 1000) { // Limit to prevent excessive scanning
            if (check_stop()) break;
            
            QString file_path = it.next();
            QFile file(file_path);
            
            if (file.size() > 1024 * 100) continue; // Skip files larger than 100KB
            
            if (file.open(QIODevice::ReadOnly | QIODevice::Text)) {
                QTextStream in(&file);
                QString content = in.readAll();
                file.close();
                
                // Look for license key patterns
                QRegularExpression key_pattern(R"([A-Z0-9]{5}-[A-Z0-9]{5}-[A-Z0-9]{5}-[A-Z0-9]{5})");
                QRegularExpressionMatchIterator matches = key_pattern.globalMatch(content);
                
                while (matches.hasNext()) {
                    QRegularExpressionMatch match = matches.next();
                    QString key = match.captured(0);
                    
                    if (is_valid_license_key(key)) {
                        license_info info;
                        info.product_name = QFileInfo(file_path).dir().dirName();
                        info.license_key = normalize_license_key(key);
                        info.installation_path = QFileInfo(file_path).absolutePath();
                        info.registry_path = "";
                        info.version = "Unknown";
                        info.is_valid = true;
                        
                        licenses.append(info);
                    }
                }
                
                ++file_count;
            }
        }
    }
    
    return licenses;
}

bool license_scanner_worker::is_valid_license_key(const QString& key) const {
    if (key.isEmpty() || key.length() < 10) return false;
    
    // Basic validation: check for patterns that look like license keys
    QRegularExpression key_pattern(R"([A-Z0-9]{4,5}-[A-Z0-9]{4,5}-[A-Z0-9]{4,5}(-[A-Z0-9]{4,5})?)");
    QRegularExpressionMatch match = key_pattern.match(key);
    
    return match.hasMatch();
}

QString license_scanner_worker::normalize_license_key(const QString& key) const {
    // Remove extra whitespace and convert to uppercase
    QString normalized = key.trimmed().toUpper();
    return normalized;
}

bool license_scanner_worker::check_and_mark_duplicate(const license_info& info) {
    // Check if this license key has already been found
    QString key_signature = info.license_key + "|" + info.product_name;
    if (m_processed_keys.contains(key_signature)) {
        return true;
    }
    m_processed_keys.insert(key_signature);
    return false;
}

} // namespace sak
