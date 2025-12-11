#include "sak/chocolatey_manager.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QDebug>
#include <QThread>
#include <QRegularExpression>

namespace sak {

ChocolateyManager::ChocolateyManager(QObject* parent)
    : QObject(parent)
    , m_initialized(false)
    , m_default_timeout_seconds(300)  // 5 minutes default
    , m_auto_confirm(true)
    , m_current_process(nullptr)
{
}

ChocolateyManager::~ChocolateyManager() {
    if (m_current_process && m_current_process->state() != QProcess::NotRunning) {
        m_current_process->kill();
        m_current_process->waitForFinished(1000);
    }
}

bool ChocolateyManager::initialize(const QString& choco_portable_path) {
    qDebug() << "[ChocolateyManager] Initializing with path:" << choco_portable_path;
    
    m_choco_dir = choco_portable_path;
    
    // Look for choco.exe in common locations within portable directory
    QStringList possible_paths = {
        QDir(m_choco_dir).filePath("choco.exe"),
        QDir(m_choco_dir).filePath("bin/choco.exe"),
        QDir(m_choco_dir).filePath("chocolatey/bin/choco.exe")
    };
    
    for (const QString& path : possible_paths) {
        if (QFile::exists(path)) {
            m_choco_path = path;
            break;
        }
    }
    
    if (m_choco_path.isEmpty()) {
        qWarning() << "[ChocolateyManager] choco.exe not found in" << choco_portable_path;
        return false;
    }
    
    qDebug() << "[ChocolateyManager] Found choco.exe at:" << m_choco_path;
    
    // Verify Chocolatey works
    QString version = getChocoVersion();
    if (version.isEmpty()) {
        qWarning() << "[ChocolateyManager] Failed to get Chocolatey version";
        return false;
    }
    
    m_initialized = true;
    qDebug() << "[ChocolateyManager] Initialized successfully. Version:" << version;
    
    return true;
}

bool ChocolateyManager::isInitialized() const {
    return m_initialized;
}

bool ChocolateyManager::verifyIntegrity() {
    if (!m_initialized) {
        return false;
    }
    
    // Check if choco.exe still exists
    if (!QFile::exists(m_choco_path)) {
        qWarning() << "[ChocolateyManager] choco.exe no longer exists at:" << m_choco_path;
        m_initialized = false;
        return false;
    }
    
    // Try to execute a simple command
    Result result = executeChoco({"--version"}, 5000);
    return result.success;
}

QString ChocolateyManager::getChocoPath() const {
    return m_choco_path;
}

QString ChocolateyManager::getChocoVersion() {
    if (!QFile::exists(m_choco_path)) {
        return QString();
    }
    
    Result result = executeChoco({"--version"}, 5000);
    if (result.success) {
        // Extract version number from output (e.g., "2.4.1")
        QRegularExpression versionRegex(R"(\d+\.\d+\.\d+)");
        QRegularExpressionMatch match = versionRegex.match(result.output);
        if (match.hasMatch()) {
            return match.captured(0);
        }
    }
    
    return QString();
}

ChocolateyManager::Result ChocolateyManager::installPackage(const InstallConfig& config) {
    if (!m_initialized) {
        return {false, "", "ChocolateyManager not initialized", -1};
    }
    
    if (!validatePackageName(config.package_name)) {
        return {false, "", "Invalid package name: " + config.package_name, -1};
    }
    
    if (config.version_locked && !config.version.isEmpty() && !validateVersion(config.version)) {
        return {false, "", "Invalid version format: " + config.version, -1};
    }
    
    Q_EMIT installStarted(config.package_name);
    
    // Build command arguments
    QStringList args = {"install", config.package_name};
    
    // Add version if locked
    if (config.version_locked && !config.version.isEmpty()) {
        args << "--version" << config.version;
        qDebug() << "[ChocolateyManager] Installing" << config.package_name << "version" << config.version;
    } else {
        qDebug() << "[ChocolateyManager] Installing" << config.package_name << "(latest)";
    }
    
    // Add auto-confirm
    if (config.auto_confirm || m_auto_confirm) {
        args << "-y";
    }
    
    // Add force if specified
    if (config.force) {
        args << "--force";
    }
    
    // Add extra arguments
    if (!config.extra_args.isEmpty()) {
        args << config.extra_args;
    }
    
    // Execute with timeout
    int timeout_ms = config.timeout_seconds > 0 ? config.timeout_seconds * 1000 : m_default_timeout_seconds * 1000;
    Result result = executeChoco(args, timeout_ms);
    
    if (result.success) {
        QString installed_version = config.version_locked ? config.version : "latest";
        Q_EMIT installSuccess(config.package_name, installed_version);
        qDebug() << "[ChocolateyManager] Successfully installed" << config.package_name;
    } else {
        Q_EMIT installFailed(config.package_name, result.error_message);
        qWarning() << "[ChocolateyManager] Failed to install" << config.package_name << ":" << result.error_message;
    }
    
    return result;
}

ChocolateyManager::Result ChocolateyManager::uninstallPackage(const QString& package_name, bool auto_confirm) {
    if (!m_initialized) {
        return {false, "", "ChocolateyManager not initialized", -1};
    }
    
    if (!validatePackageName(package_name)) {
        return {false, "", "Invalid package name: " + package_name, -1};
    }
    
    QStringList args = {"uninstall", package_name};
    
    if (auto_confirm || m_auto_confirm) {
        args << "-y";
    }
    
    return executeChoco(args);
}

ChocolateyManager::Result ChocolateyManager::upgradePackage(const QString& package_name, bool auto_confirm) {
    if (!m_initialized) {
        return {false, "", "ChocolateyManager not initialized", -1};
    }
    
    if (!validatePackageName(package_name)) {
        return {false, "", "Invalid package name: " + package_name, -1};
    }
    
    QStringList args = {"upgrade", package_name};
    
    if (auto_confirm || m_auto_confirm) {
        args << "-y";
    }
    
    return executeChoco(args);
}

ChocolateyManager::Result ChocolateyManager::searchPackage(const QString& query, int max_results) {
    if (!m_initialized) {
        return {false, "", "ChocolateyManager not initialized", -1};
    }
    
    Q_EMIT searchStarted(query);
    
    QStringList args = {"search", query, "--limit-output"};
    
    if (max_results > 0) {
        args << "--page-size" << QString::number(max_results);
    }
    
    Result result = executeChoco(args, 30000);  // 30 second timeout for search
    
    if (result.success) {
        auto packages = parseSearchResults(result.output);
        Q_EMIT searchComplete(static_cast<int>(packages.size()));
    }
    
    return result;
}

std::vector<ChocolateyManager::PackageInfo> ChocolateyManager::parseSearchResults(const QString& output) {
    std::vector<PackageInfo> packages;
    
    // Parse limit-output format: "package_id|version"
    QStringList lines = output.split('\n', Qt::SkipEmptyParts);
    
    for (const QString& line : lines) {
        if (line.trimmed().isEmpty() || line.startsWith("Chocolatey")) {
            continue;
        }
        
        QStringList parts = line.split('|');
        if (parts.size() >= 2) {
            PackageInfo pkg;
            pkg.package_id = parts[0].trimmed();
            pkg.version = parts[1].trimmed();
            pkg.title = pkg.package_id;  // Basic info only in limit-output
            pkg.description = "";
            pkg.is_approved = false;
            pkg.download_count = 0;
            
            packages.push_back(pkg);
        }
    }
    
    return packages;
}

bool ChocolateyManager::isPackageInstalled(const QString& package_name) {
    if (!m_initialized) {
        return false;
    }
    
    QStringList args = {"list", "--local-only", package_name, "--exact", "--limit-output"};
    Result result = executeChoco(args, 10000);
    
    return result.success && result.output.contains(package_name);
}

QString ChocolateyManager::getInstalledVersion(const QString& package_name) {
    if (!m_initialized) {
        return QString();
    }
    
    QStringList args = {"list", "--local-only", package_name, "--exact", "--limit-output"};
    Result result = executeChoco(args, 10000);
    
    if (result.success) {
        // Parse "package_name|version"
        QStringList lines = result.output.split('\n', Qt::SkipEmptyParts);
        for (const QString& line : lines) {
            if (line.startsWith(package_name + "|")) {
                QStringList parts = line.split('|');
                if (parts.size() >= 2) {
                    return parts[1].trimmed();
                }
            }
        }
    }
    
    return QString();
}

bool ChocolateyManager::isPackageAvailable(const QString& package_name) {
    if (!m_initialized) {
        return false;
    }
    
    Result result = searchPackage(package_name, 1);
    
    if (result.success) {
        auto packages = parseSearchResults(result.output);
        for (const auto& pkg : packages) {
            if (pkg.package_id.compare(package_name, Qt::CaseInsensitive) == 0) {
                return true;
            }
        }
    }
    
    return false;
}

ChocolateyManager::Result ChocolateyManager::installWithRetry(
    const InstallConfig& config, 
    int max_attempts, 
    int delay_seconds) 
{
    Result last_result;
    
    for (int attempt = 1; attempt <= max_attempts; ++attempt) {
        if (attempt > 1) {
            Q_EMIT installRetrying(config.package_name, attempt, max_attempts);
            qDebug() << "[ChocolateyManager] Retry attempt" << attempt << "of" << max_attempts 
                     << "for" << config.package_name;
            
            // Wait before retry
            QThread::sleep(delay_seconds);
        }
        
        last_result = installPackage(config);
        
        if (last_result.success) {
            return last_result;
        }
        
        // Check if error is retryable
        if (isPermissionError(last_result.output)) {
            qWarning() << "[ChocolateyManager] Permission error - not retrying";
            break;
        }
    }
    
    return last_result;
}

void ChocolateyManager::setDefaultTimeout(int seconds) {
    m_default_timeout_seconds = seconds;
}

int ChocolateyManager::getDefaultTimeout() const {
    return m_default_timeout_seconds;
}

void ChocolateyManager::setAutoConfirm(bool confirm) {
    m_auto_confirm = confirm;
}

bool ChocolateyManager::getAutoConfirm() const {
    return m_auto_confirm;
}

ChocolateyManager::Result ChocolateyManager::executeChoco(const QStringList& args, int timeout_ms) {
    QProcess process;
    
    // Set up environment
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert("ChocolateyInstall", m_choco_dir);
    process.setProcessEnvironment(env);
    
    // Build command
    QString program = m_choco_path;
    
    qDebug() << "[ChocolateyManager] Executing:" << program << args.join(" ");
    
    // Start process
    process.start(program, args);
    
    if (!process.waitForStarted(5000)) {
        return {false, "", "Failed to start choco.exe", -1};
    }
    
    // Wait for finish with timeout
    bool finished = false;
    if (timeout_ms > 0) {
        finished = process.waitForFinished(timeout_ms);
    } else {
        finished = process.waitForFinished(-1);  // No timeout
    }
    
    if (!finished) {
        process.kill();
        process.waitForFinished(1000);
        return {false, "", "Command timed out", -1};
    }
    
    // Get output
    QString stdout_output = QString::fromUtf8(process.readAllStandardOutput());
    QString stderr_output = QString::fromUtf8(process.readAllStandardError());
    QString combined_output = stdout_output + "\n" + stderr_output;
    
    int exit_code = process.exitCode();
    bool success = parseExitCode(exit_code);
    
    QString error_msg;
    if (!success) {
        error_msg = extractErrorMessage(combined_output);
        if (error_msg.isEmpty()) {
            error_msg = "Command failed with exit code " + QString::number(exit_code);
        }
    }
    
    return {success, combined_output, error_msg, exit_code};
}

QString ChocolateyManager::buildChocoCommand(const QStringList& args) const {
    return m_choco_path + " " + args.join(" ");
}

bool ChocolateyManager::parseExitCode(int exit_code) const {
    // Chocolatey exit codes:
    // 0 = success
    // 1 = generic error
    // 2 = nothing to do / no packages found
    // 1641/3010 = success with reboot required
    
    return (exit_code == 0 || exit_code == 1641 || exit_code == 3010);
}

QString ChocolateyManager::extractErrorMessage(const QString& output) const {
    // Look for common error patterns in Chocolatey output
    QStringList lines = output.split('\n');
    
    for (const QString& line : lines) {
        QString trimmed = line.trimmed();
        
        if (trimmed.contains("ERROR", Qt::CaseInsensitive)) {
            // Extract error message after "ERROR:"
            int error_pos = trimmed.indexOf("ERROR", Qt::CaseInsensitive);
            return trimmed.mid(error_pos).trimmed();
        }
        
        if (trimmed.contains("Failed", Qt::CaseInsensitive)) {
            return trimmed;
        }
        
        if (trimmed.contains("not found", Qt::CaseInsensitive)) {
            return trimmed;
        }
    }
    
    return "Unknown error";
}

bool ChocolateyManager::isNetworkError(const QString& output) const {
    QStringList network_keywords = {
        "network", "timeout", "connection", "unreachable", 
        "dns", "proxy", "ssl", "certificate", "tls"
    };
    
    QString lower_output = output.toLower();
    for (const QString& keyword : network_keywords) {
        if (lower_output.contains(keyword)) {
            return true;
        }
    }
    
    return false;
}

bool ChocolateyManager::isDependencyError(const QString& output) const {
    return output.contains("dependency", Qt::CaseInsensitive) ||
           output.contains("requires", Qt::CaseInsensitive);
}

bool ChocolateyManager::isPermissionError(const QString& output) const {
    return output.contains("access denied", Qt::CaseInsensitive) ||
           output.contains("permission", Qt::CaseInsensitive) ||
           output.contains("administrator", Qt::CaseInsensitive) ||
           output.contains("elevated", Qt::CaseInsensitive);
}

bool ChocolateyManager::validatePackageName(const QString& package_name) const {
    if (package_name.isEmpty() || package_name.length() > 100) {
        return false;
    }
    
    // Package names should contain only alphanumeric, dash, dot, underscore
    QRegularExpression valid_name(R"(^[a-zA-Z0-9._-]+$)");
    return valid_name.match(package_name).hasMatch();
}

bool ChocolateyManager::validateVersion(const QString& version) const {
    if (version.isEmpty() || version.length() > 50) {
        return false;
    }
    
    // Version format: digits, dots, and optional prerelease identifiers
    QRegularExpression valid_version(R"(^\d+(\.\d+)*(-[a-zA-Z0-9]+)?$)");
    return valid_version.match(version).hasMatch();
}

} // namespace sak
