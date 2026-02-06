#pragma once

#include "sak/app_scanner.h"
#include "sak/package_matcher.h"
#include <QString>
#include <QDateTime>
#include <vector>
#include <optional>

namespace sak {

/**
 * @brief MigrationReport - Export/import application migration plans
 * 
 * This class manages the creation, export, and import of migration reports
 * that combine scanned apps with their matched Chocolatey packages. Reports
 * can be exported to JSON or CSV for documentation, planning, or execution
 * on target machines.
 * 
 * Use cases:
 * - Document current system state
 * - Plan migration to new machine
 * - Share migration list with team
 * - Import migration list on target machine
 */
class MigrationReport {
public:
    MigrationReport();
    ~MigrationReport() = default;

    /**
     * @brief Migration entry - combines app info with match result
     */
    struct MigrationEntry {
        // Source app information
        QString app_name;
        QString app_version;
        QString app_publisher;
        QString install_location;
        QDateTime install_date;
        QString registry_key;
        
        // Match information
        QString choco_package;
        double confidence;
        QString match_type;  // "exact", "fuzzy", "search", "manual", "none"
        bool available;
        QString available_version;
        
        // Migration control
        bool selected;           // User wants to migrate this app
        bool version_lock;       // Install specific version (if available)
        QString locked_version;  // Version to install when locked
        QString notes;           // User notes
        
        // Execution status
        QString status;          // "pending", "installing", "success", "failed", "skipped"
        QString error_message;
        QDateTime executed_at;
    };

    /**
     * @brief Report metadata
     */
    struct ReportMetadata {
        QString source_machine;
        QString source_os;
        QString source_os_version;
        QString created_by;
        QDateTime created_at;
        int total_apps;
        int matched_apps;
        int selected_apps;
        double match_rate;
        QString report_version;  // Format version for compatibility
    };

    // Report generation
    void generateReport(const std::vector<AppScanner::AppInfo>& apps,
                       const std::vector<PackageMatcher::MatchResult>& matches);
    
    void addEntry(const MigrationEntry& entry);
    void updateEntry(int index, const MigrationEntry& entry);
    void removeEntry(int index);
    
    // Selection management
    void selectEntry(int index, bool selected = true);
    void selectAll();
    void deselectAll();
    void selectByMatchType(const QString& match_type);
    void selectByConfidence(double min_confidence);
    
    // Export formats
    bool exportToJson(const QString& file_path) const;
    bool exportToCsv(const QString& file_path) const;
    bool exportToHtml(const QString& file_path) const;  // Human-readable report
    
    // Import
    bool importFromJson(const QString& file_path);
    
    // Accessors
    const std::vector<MigrationEntry>& getEntries() const { return m_entries; }
    std::vector<MigrationEntry>& getEntries() { return m_entries; }
    const MigrationEntry& getEntry(int index) const { return m_entries[index]; }
    MigrationEntry& getEntry(int index) { return m_entries[index]; }
    
    int getEntryCount() const { return static_cast<int>(m_entries.size()); }
    int getSelectedCount() const;
    int getMatchedCount() const;
    int getUnmatchedCount() const;
    
    const ReportMetadata& getMetadata() const { return m_metadata; }
    ReportMetadata& getMetadata() { return m_metadata; }
    
    // Statistics
    double getMatchRate() const;
    QMap<QString, int> getMatchTypeDistribution() const;
    std::vector<MigrationEntry> getSelectedEntries() const;
    std::vector<MigrationEntry> getUnmatchedEntries() const;
    
    // Clear
    void clear();
    
private:
    // Helper methods
    QString getSystemInfo() const;
    QString getOSVersion() const;
    QString getComputerName() const;
    QString getCurrentUser() const;
    
    QString escapeJsonString(const QString& str) const;
    QString escapeCsvField(const QString& field) const;
    QString formatHtmlReport() const;
    
    // Data members
    std::vector<MigrationEntry> m_entries;
    ReportMetadata m_metadata;
};

} // namespace sak
