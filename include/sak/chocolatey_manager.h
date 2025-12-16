#pragma once

#include <QObject>
#include <QString>
#include <QProcess>
#include <QStringList>
#include <QMap>
#include <vector>

namespace sak {

/**
 * @brief ChocolateyManager - Manages embedded portable Chocolatey operations
 * 
 * This class handles all interactions with the bundled portable Chocolatey installation.
 * It initializes the embedded Chocolatey, executes package installations with version locking,
 * and provides retry logic for failed operations.
 * 
 * Key Features:
 * - Embedded portable Chocolatey (no external dependencies)
 * - Version locking support (install specific versions)
 * - Retry logic with configurable attempts
 * - Package search and availability checking
 * - Installation progress tracking
 */
class ChocolateyManager : public QObject {
    Q_OBJECT

public:
    explicit ChocolateyManager(QObject* parent = nullptr);
    ~ChocolateyManager() override;

    /**
     * @brief Result structure for Chocolatey operations
     */
    struct Result {
        bool success;
        QString output;
        QString error_message;
        int exit_code;
    };

    /**
     * @brief Package information from Chocolatey search
     */
    struct PackageInfo {
        QString package_id;
        QString version;
        QString title;
        QString description;
        bool is_approved;
        int download_count;
    };

    /**
     * @brief Installation configuration
     */
    struct InstallConfig {
        QString package_name;
        QString version;          // Empty = latest stable
        bool version_locked;      // If true, install specific version
        bool auto_confirm;        // -y flag
        bool force;               // --force flag
        bool allow_unofficial;    // Allow unofficial sources
        int timeout_seconds;      // Command timeout (0 = no timeout)
        QStringList extra_args;   // Additional choco arguments
    };

    // Initialization
    bool initialize(const QString& choco_portable_path);
    bool isInitialized() const;
    bool verifyIntegrity();
    QString getChocoPath() const;
    QString getChocoVersion();

    // Package operations
    Result installPackage(const InstallConfig& config);
    Result uninstallPackage(const QString& package_name, bool auto_confirm = true);
    Result upgradePackage(const QString& package_name, bool auto_confirm = true);
    Result searchPackage(const QString& query, int max_results = 10);
    
    // Package information
    std::vector<PackageInfo> parseSearchResults(const QString& output);
    bool isPackageInstalled(const QString& package_name);
    QString getInstalledVersion(const QString& package_name);
    bool isPackageAvailable(const QString& package_name);
    QStringList getOutdatedPackages();
    
    // Retry logic
    Result installWithRetry(const InstallConfig& config, int max_attempts, int delay_seconds);
    
    // Configuration
    void setDefaultTimeout(int seconds);
    int getDefaultTimeout() const;
    
    void setAutoConfirm(bool confirm);
    bool getAutoConfirm() const;

Q_SIGNALS:
    void installStarted(const QString& package_name);
    void installProgress(const QString& package_name, const QString& status);
    void installSuccess(const QString& package_name, const QString& version);
    void installFailed(const QString& package_name, const QString& error);
    void installRetrying(const QString& package_name, int attempt, int max_attempts);
    
    void searchStarted(const QString& query);
    void searchComplete(int results_found);

private:
    // Execution helpers
    Result executeChoco(const QStringList& args, int timeout_ms = 0);
    QString buildChocoCommand(const QStringList& args) const;
    bool parseExitCode(int exit_code) const;
    
    // Output parsing
    QString extractErrorMessage(const QString& output) const;
    bool isNetworkError(const QString& output) const;
    bool isDependencyError(const QString& output) const;
    bool isPermissionError(const QString& output) const;
    
    // Validation
    bool validatePackageName(const QString& package_name) const;
    bool validateVersion(const QString& version) const;

    // Member variables
    QString m_choco_path;              // Path to choco.exe
    QString m_choco_dir;               // Root directory of portable Chocolatey
    bool m_initialized;
    int m_default_timeout_seconds;
    bool m_auto_confirm;
    
    QProcess* m_current_process;       // Active process (for cancellation)
};

} // namespace sak
