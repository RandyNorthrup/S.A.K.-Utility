#include "sak/migration_report.h"
#include <QFile>
#include <QTextStream>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QSysInfo>
#include <QDebug>
#include <algorithm>

#ifdef Q_OS_WIN
#include <windows.h>
#include <lmcons.h>
#endif

namespace sak {

MigrationReport::MigrationReport() {
    // Initialize metadata
    m_metadata.source_machine = getComputerName();
    m_metadata.source_os = QSysInfo::productType();
    m_metadata.source_os_version = QSysInfo::productVersion();
    m_metadata.created_by = getCurrentUser();
    m_metadata.created_at = QDateTime::currentDateTime();
    m_metadata.report_version = "1.0";
}

void MigrationReport::generateReport(
    const std::vector<AppScanner::AppInfo>& apps,
    const std::vector<PackageMatcher::MatchResult>& matches)
{
    m_entries.clear();
    
    // Create a map of app names to match results for quick lookup
    QMap<QString, PackageMatcher::MatchResult> match_map;
    for (const auto& match : matches) {
        match_map[match.matched_name] = match;
    }
    
    // Generate entries for all apps
    for (const auto& app : apps) {
        MigrationEntry entry;
        
        // App info
        entry.app_name = app.name;
        entry.app_version = app.version;
        entry.app_publisher = app.publisher;
        entry.install_location = app.install_location;
        entry.install_date = QDateTime::fromString(app.install_date, Qt::ISODate);
        entry.registry_key = app.registry_key;
        
        // Find match (try normalized name and original name)
        QString normalized_name = app.name;
        normalized_name.remove(QRegularExpression("[\\(\\)\\[\\]]"));  // Remove brackets
        
        bool found_match = false;
        if (match_map.contains(app.name)) {
            auto& match = match_map[app.name];
            entry.choco_package = match.choco_package;
            entry.confidence = match.confidence;
            entry.match_type = match.match_type;
            entry.available = match.available;
            entry.available_version = match.version;
            found_match = true;
        } else if (match_map.contains(normalized_name)) {
            auto& match = match_map[normalized_name];
            entry.choco_package = match.choco_package;
            entry.confidence = match.confidence;
            entry.match_type = match.match_type;
            entry.available = match.available;
            entry.available_version = match.version;
            found_match = true;
        }
        
        if (!found_match) {
            entry.choco_package = "";
            entry.confidence = 0.0;
            entry.match_type = "none";
            entry.available = false;
            entry.available_version = "";
        }
        
        // Migration control (default: auto-select high confidence matches)
        entry.selected = (entry.confidence >= 0.8);
        entry.version_lock = false;
        entry.notes = "";
        
        // Execution status
        entry.status = "pending";
        entry.error_message = "";
        
        m_entries.push_back(entry);
    }
    
    // Update metadata
    m_metadata.total_apps = static_cast<int>(apps.size());
    m_metadata.matched_apps = getMatchedCount();
    m_metadata.selected_apps = getSelectedCount();
    m_metadata.match_rate = getMatchRate();
    m_metadata.created_at = QDateTime::currentDateTime();
}

void MigrationReport::addEntry(const MigrationEntry& entry) {
    m_entries.push_back(entry);
    m_metadata.total_apps = static_cast<int>(m_entries.size());
}

void MigrationReport::updateEntry(int index, const MigrationEntry& entry) {
    if (index >= 0 && index < static_cast<int>(m_entries.size())) {
        m_entries[index] = entry;
    }
}

void MigrationReport::removeEntry(int index) {
    if (index >= 0 && index < static_cast<int>(m_entries.size())) {
        m_entries.erase(m_entries.begin() + index);
        m_metadata.total_apps = static_cast<int>(m_entries.size());
    }
}

void MigrationReport::selectEntry(int index, bool selected) {
    if (index >= 0 && index < static_cast<int>(m_entries.size())) {
        m_entries[index].selected = selected;
        m_metadata.selected_apps = getSelectedCount();
    }
}

void MigrationReport::selectAll() {
    for (auto& entry : m_entries) {
        entry.selected = true;
    }
    m_metadata.selected_apps = getSelectedCount();
}

void MigrationReport::deselectAll() {
    for (auto& entry : m_entries) {
        entry.selected = false;
    }
    m_metadata.selected_apps = 0;
}

void MigrationReport::selectByMatchType(const QString& match_type) {
    for (auto& entry : m_entries) {
        if (entry.match_type == match_type) {
            entry.selected = true;
        }
    }
    m_metadata.selected_apps = getSelectedCount();
}

void MigrationReport::selectByConfidence(double min_confidence) {
    for (auto& entry : m_entries) {
        if (entry.confidence >= min_confidence) {
            entry.selected = true;
        } else {
            entry.selected = false;
        }
    }
    m_metadata.selected_apps = getSelectedCount();
}

bool MigrationReport::exportToJson(const QString& file_path) const {
    QJsonObject root;
    
    // Metadata
    QJsonObject metadata;
    metadata["source_machine"] = m_metadata.source_machine;
    metadata["source_os"] = m_metadata.source_os;
    metadata["source_os_version"] = m_metadata.source_os_version;
    metadata["created_by"] = m_metadata.created_by;
    metadata["created_at"] = m_metadata.created_at.toString(Qt::ISODate);
    metadata["total_apps"] = m_metadata.total_apps;
    metadata["matched_apps"] = m_metadata.matched_apps;
    metadata["selected_apps"] = m_metadata.selected_apps;
    metadata["match_rate"] = m_metadata.match_rate;
    metadata["report_version"] = m_metadata.report_version;
    root["metadata"] = metadata;
    
    // Entries
    QJsonArray entries;
    for (const auto& entry : m_entries) {
        QJsonObject e;
        e["app_name"] = entry.app_name;
        e["app_version"] = entry.app_version;
        e["app_publisher"] = entry.app_publisher;
        e["install_location"] = entry.install_location;
        e["install_date"] = entry.install_date.toString(Qt::ISODate);
        e["registry_key"] = entry.registry_key;
        
        e["choco_package"] = entry.choco_package;
        e["confidence"] = entry.confidence;
        e["match_type"] = entry.match_type;
        e["available"] = entry.available;
        e["available_version"] = entry.available_version;
        
        e["selected"] = entry.selected;
        e["version_lock"] = entry.version_lock;
        e["notes"] = entry.notes;
        
        e["status"] = entry.status;
        e["error_message"] = entry.error_message;
        if (entry.executed_at.isValid()) {
            e["executed_at"] = entry.executed_at.toString(Qt::ISODate);
        }
        
        entries.append(e);
    }
    root["entries"] = entries;
    
    // Write to file
    QFile file(file_path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        qWarning() << "[MigrationReport] Failed to open file for writing:" << file_path;
        return false;
    }
    
    QTextStream out(&file);
    out << QJsonDocument(root).toJson(QJsonDocument::Indented);
    file.close();
    
    return true;
}

bool MigrationReport::exportToCsv(const QString& file_path) const {
    QFile file(file_path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        qWarning() << "[MigrationReport] Failed to open file for writing:" << file_path;
        return false;
    }
    
    QTextStream out(&file);
    
    // Header
    out << "App Name,Version,Publisher,Install Location,Install Date,"
        << "Chocolatey Package,Confidence,Match Type,Available,Available Version,"
        << "Selected,Version Lock,Notes,Status,Error Message\n";
    
    // Entries
    for (const auto& entry : m_entries) {
        out << escapeCsvField(entry.app_name) << ","
            << escapeCsvField(entry.app_version) << ","
            << escapeCsvField(entry.app_publisher) << ","
            << escapeCsvField(entry.install_location) << ","
            << escapeCsvField(entry.install_date.toString(Qt::ISODate)) << ","
            << escapeCsvField(entry.choco_package) << ","
            << QString::number(entry.confidence, 'f', 2) << ","
            << entry.match_type << ","
            << (entry.available ? "Yes" : "No") << ","
            << escapeCsvField(entry.available_version) << ","
            << (entry.selected ? "Yes" : "No") << ","
            << (entry.version_lock ? "Yes" : "No") << ","
            << escapeCsvField(entry.notes) << ","
            << entry.status << ","
            << escapeCsvField(entry.error_message) << "\n";
    }
    
    file.close();
    return true;
}

bool MigrationReport::exportToHtml(const QString& file_path) const {
    QFile file(file_path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        qWarning() << "[MigrationReport] Failed to open file for writing:" << file_path;
        return false;
    }
    
    QTextStream out(&file);
    out << formatHtmlReport();
    file.close();
    
    return true;
}

bool MigrationReport::importFromJson(const QString& file_path) {
    QFile file(file_path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        qWarning() << "[MigrationReport] Failed to open file for reading:" << file_path;
        return false;
    }
    
    QByteArray data = file.readAll();
    file.close();
    
    QJsonDocument doc = QJsonDocument::fromJson(data);
    if (!doc.isObject()) {
        qWarning() << "[MigrationReport] Invalid JSON format";
        return false;
    }
    
    QJsonObject root = doc.object();
    
    // Metadata
    if (root.contains("metadata")) {
        QJsonObject metadata = root["metadata"].toObject();
        m_metadata.source_machine = metadata["source_machine"].toString();
        m_metadata.source_os = metadata["source_os"].toString();
        m_metadata.source_os_version = metadata["source_os_version"].toString();
        m_metadata.created_by = metadata["created_by"].toString();
        m_metadata.created_at = QDateTime::fromString(metadata["created_at"].toString(), Qt::ISODate);
        m_metadata.total_apps = metadata["total_apps"].toInt();
        m_metadata.matched_apps = metadata["matched_apps"].toInt();
        m_metadata.selected_apps = metadata["selected_apps"].toInt();
        m_metadata.match_rate = metadata["match_rate"].toDouble();
        m_metadata.report_version = metadata["report_version"].toString();
    }
    
    // Entries
    m_entries.clear();
    if (root.contains("entries")) {
        QJsonArray entries = root["entries"].toArray();
        for (const QJsonValue& val : entries) {
            QJsonObject e = val.toObject();
            
            MigrationEntry entry;
            entry.app_name = e["app_name"].toString();
            entry.app_version = e["app_version"].toString();
            entry.app_publisher = e["app_publisher"].toString();
            entry.install_location = e["install_location"].toString();
            entry.install_date = QDateTime::fromString(e["install_date"].toString(), Qt::ISODate);
            entry.registry_key = e["registry_key"].toString();
            
            entry.choco_package = e["choco_package"].toString();
            entry.confidence = e["confidence"].toDouble();
            entry.match_type = e["match_type"].toString();
            entry.available = e["available"].toBool();
            entry.available_version = e["available_version"].toString();
            
            entry.selected = e["selected"].toBool();
            entry.version_lock = e["version_lock"].toBool();
            entry.notes = e["notes"].toString();
            
            entry.status = e["status"].toString();
            entry.error_message = e["error_message"].toString();
            if (e.contains("executed_at")) {
                entry.executed_at = QDateTime::fromString(e["executed_at"].toString(), Qt::ISODate);
            }
            
            m_entries.push_back(entry);
        }
    }
    
    return true;
}

int MigrationReport::getSelectedCount() const {
    return static_cast<int>(std::count_if(m_entries.begin(), m_entries.end(),
        [](const MigrationEntry& e) { return e.selected; }));
}

int MigrationReport::getMatchedCount() const {
    return static_cast<int>(std::count_if(m_entries.begin(), m_entries.end(),
        [](const MigrationEntry& e) { return !e.choco_package.isEmpty(); }));
}

int MigrationReport::getUnmatchedCount() const {
    return static_cast<int>(m_entries.size()) - getMatchedCount();
}

double MigrationReport::getMatchRate() const {
    if (m_entries.empty()) return 0.0;
    return static_cast<double>(getMatchedCount()) / static_cast<double>(m_entries.size());
}

QMap<QString, int> MigrationReport::getMatchTypeDistribution() const {
    QMap<QString, int> dist;
    for (const auto& entry : m_entries) {
        dist[entry.match_type]++;
    }
    return dist;
}

std::vector<MigrationReport::MigrationEntry> MigrationReport::getSelectedEntries() const {
    std::vector<MigrationEntry> selected;
    std::copy_if(m_entries.begin(), m_entries.end(), std::back_inserter(selected),
        [](const MigrationEntry& e) { return e.selected; });
    return selected;
}

std::vector<MigrationReport::MigrationEntry> MigrationReport::getUnmatchedEntries() const {
    std::vector<MigrationEntry> unmatched;
    std::copy_if(m_entries.begin(), m_entries.end(), std::back_inserter(unmatched),
        [](const MigrationEntry& e) { return e.choco_package.isEmpty(); });
    return unmatched;
}

void MigrationReport::clear() {
    m_entries.clear();
    m_metadata.total_apps = 0;
    m_metadata.matched_apps = 0;
    m_metadata.selected_apps = 0;
    m_metadata.match_rate = 0.0;
}

QString MigrationReport::getComputerName() const {
#ifdef Q_OS_WIN
    WCHAR buffer[MAX_COMPUTERNAME_LENGTH + 1];
    DWORD size = MAX_COMPUTERNAME_LENGTH + 1;
    if (GetComputerNameW(buffer, &size)) {
        return QString::fromWCharArray(buffer);
    }
#endif
    return QSysInfo::machineHostName();
}

QString MigrationReport::getCurrentUser() const {
#ifdef Q_OS_WIN
    WCHAR buffer[UNLEN + 1];
    DWORD size = UNLEN + 1;
    if (GetUserNameW(buffer, &size)) {
        return QString::fromWCharArray(buffer);
    }
#endif
    return qEnvironmentVariable("USERNAME");
}

QString MigrationReport::escapeCsvField(const QString& field) const {
    if (field.contains(',') || field.contains('"') || field.contains('\n')) {
        QString escaped = field;
        escaped.replace("\"", "\"\"");
        return "\"" + escaped + "\"";
    }
    return field;
}

QString MigrationReport::formatHtmlReport() const {
    QString html;
    QTextStream out(&html);
    
    out << "<!DOCTYPE html>\n<html>\n<head>\n";
    out << "<meta charset=\"UTF-8\">\n";
    out << "<title>Application Migration Report</title>\n";
    out << "<style>\n";
    out << "body { font-family: Arial, sans-serif; margin: 20px; background-color: #f5f5f5; }\n";
    out << "h1 { color: #333; }\n";
    out << ".metadata { background: white; padding: 20px; margin-bottom: 20px; border-radius: 5px; box-shadow: 0 2px 4px rgba(0,0,0,0.1); }\n";
    out << ".metadata table { width: 100%; border-collapse: collapse; }\n";
    out << ".metadata td { padding: 8px; border-bottom: 1px solid #eee; }\n";
    out << ".metadata td:first-child { font-weight: bold; width: 200px; }\n";
    out << ".stats { background: white; padding: 20px; margin-bottom: 20px; border-radius: 5px; box-shadow: 0 2px 4px rgba(0,0,0,0.1); }\n";
    out << ".stats-grid { display: grid; grid-template-columns: repeat(auto-fit, minmax(200px, 1fr)); gap: 15px; }\n";
    out << ".stat-box { text-align: center; padding: 15px; background: #f8f9fa; border-radius: 5px; }\n";
    out << ".stat-value { font-size: 32px; font-weight: bold; color: #007bff; }\n";
    out << ".stat-label { color: #666; margin-top: 5px; }\n";
    out << "table { width: 100%; border-collapse: collapse; background: white; box-shadow: 0 2px 4px rgba(0,0,0,0.1); }\n";
    out << "th { background: #007bff; color: white; padding: 12px; text-align: left; font-weight: bold; }\n";
    out << "td { padding: 10px; border-bottom: 1px solid #eee; }\n";
    out << "tr:hover { background: #f8f9fa; }\n";
    out << ".selected { color: #28a745; font-weight: bold; }\n";
    out << ".unmatched { color: #dc3545; }\n";
    out << ".exact { background: #d4edda; }\n";
    out << ".fuzzy { background: #fff3cd; }\n";
    out << ".confidence { font-weight: bold; }\n";
    out << "</style>\n</head>\n<body>\n";
    
    // Title
    out << "<h1>Application Migration Report</h1>\n";
    
    // Metadata
    out << "<div class=\"metadata\">\n<h2>Source System Information</h2>\n<table>\n";
    out << "<tr><td>Machine Name:</td><td>" << m_metadata.source_machine << "</td></tr>\n";
    out << "<tr><td>Operating System:</td><td>" << m_metadata.source_os << " " << m_metadata.source_os_version << "</td></tr>\n";
    out << "<tr><td>Created By:</td><td>" << m_metadata.created_by << "</td></tr>\n";
    out << "<tr><td>Created At:</td><td>" << m_metadata.created_at.toString("yyyy-MM-dd HH:mm:ss") << "</td></tr>\n";
    out << "<tr><td>Report Version:</td><td>" << m_metadata.report_version << "</td></tr>\n";
    out << "</table>\n</div>\n";
    
    // Statistics
    out << "<div class=\"stats\">\n<h2>Migration Statistics</h2>\n";
    out << "<div class=\"stats-grid\">\n";
    out << "<div class=\"stat-box\"><div class=\"stat-value\">" << m_metadata.total_apps << "</div><div class=\"stat-label\">Total Apps</div></div>\n";
    out << "<div class=\"stat-box\"><div class=\"stat-value\">" << m_metadata.matched_apps << "</div><div class=\"stat-label\">Matched Apps</div></div>\n";
    out << "<div class=\"stat-box\"><div class=\"stat-value\">" << m_metadata.selected_apps << "</div><div class=\"stat-label\">Selected Apps</div></div>\n";
    out << "<div class=\"stat-box\"><div class=\"stat-value\">" << QString::number(m_metadata.match_rate * 100, 'f', 1) << "%</div><div class=\"stat-label\">Match Rate</div></div>\n";
    out << "</div>\n</div>\n";
    
    // Entries table
    out << "<h2>Application List</h2>\n";
    out << "<table>\n<thead>\n<tr>\n";
    out << "<th>App Name</th><th>Version</th><th>Publisher</th><th>Chocolatey Package</th><th>Confidence</th><th>Match Type</th><th>Selected</th>\n";
    out << "</tr>\n</thead>\n<tbody>\n";
    
    for (const auto& entry : m_entries) {
        QString row_class = entry.match_type == "exact" ? " class=\"exact\"" : (entry.match_type == "fuzzy" ? " class=\"fuzzy\"" : "");
        out << "<tr" << row_class << ">\n";
        out << "<td>" << entry.app_name << "</td>\n";
        out << "<td>" << entry.app_version << "</td>\n";
        out << "<td>" << entry.app_publisher << "</td>\n";
        out << "<td>" << (entry.choco_package.isEmpty() ? "<span class=\"unmatched\">No match</span>" : entry.choco_package) << "</td>\n";
        out << "<td class=\"confidence\">" << QString::number(entry.confidence * 100, 'f', 0) << "%</td>\n";
        out << "<td>" << entry.match_type << "</td>\n";
        out << "<td>" << (entry.selected ? "<span class=\"selected\">✓ Yes</span>" : "No") << "</td>\n";
        out << "</tr>\n";
    }
    
    out << "</tbody>\n</table>\n";
    out << "</body>\n</html>\n";
    
    return html;
}

} // namespace sak
