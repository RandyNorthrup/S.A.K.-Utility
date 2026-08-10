// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <QMap>
#include <QObject>
#include <QString>
#include <QStringList>

#include <vector>

namespace sak {

inline constexpr int kDefaultChocoSearchResults = 10;

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
        bool success{false};
        QString output;
        QString error_message;
        int exit_code{-1};
    };

    /**
     * @brief Package information from Chocolatey search
     */
    struct PackageInfo {
        QString package_id;
        QString version;
        QString title;
        QString description;
        bool is_approved{false};
        int download_count{0};
    };

    /**
     * @brief Installation configuration
     */
    struct InstallConfig {
        QString package_name;
        QString version;               // Empty = latest stable
        bool version_locked{false};    // If true, install specific version
        bool auto_confirm{true};       // -y flag
        bool force{false};             // --force flag
        bool allow_unofficial{false};  // Allow unofficial sources
        int timeout_seconds{0};        // Command timeout (0 = use the default timeout)
        QStringList extra_args;        // Additional choco arguments
    };

    // Initialization
    bool initialize(const QString& choco_portable_path);
    [[nodiscard]] bool isInitialized() const;
    bool verifyIntegrity();
    QString getChocoPath() const;
    QString getChocoVersion();

    // Package operations
    Result installPackage(const InstallConfig& config);
    Result uninstallPackage(const QString& package_name, bool auto_confirm = true);
    Result upgradePackage(const QString& package_name, bool auto_confirm = true);
    Result searchPackage(const QString& query, int max_results = kDefaultChocoSearchResults);

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

    /// @brief True iff @p package_name is a safe Chocolatey/NuGet package id: it
    ///        must START with an alphanumeric (so it can never be read as a CLI
    ///        option like "-force") and otherwise contain only [A-Za-z0-9._-].
    ///        Pure; unit-testable.
    [[nodiscard]] static bool validatePackageName(const QString& package_name);

    /// @brief True iff @p version is a safe version string: NuGet/SemVer numeric
    ///        core with optional dotted prerelease and +build metadata, starting
    ///        with a digit (never an option). Pure; unit-testable.
    [[nodiscard]] static bool validateVersion(const QString& version);

    /// @brief True iff @p signer_subject identifies the official Chocolatey code
    ///        signer. Matching is case-insensitive and requires the whole word
    ///        "Chocolatey" so a lookalike subject that merely embeds the token
    ///        (e.g. "NotChocolateyEvil Ltd") is rejected. Pure; unit-testable.
    [[nodiscard]] static bool isTrustedChocoSigner(const QString& signer_subject);

    /// @brief The install --source that must be pinned for a given trust posture.
    ///        Returns the official Chocolatey community feed when unofficial
    ///        sources are NOT allowed (so a hijacked source entry in the bundled
    ///        choco config cannot redirect the download), or empty when the caller
    ///        has explicitly opted into unofficial/configured sources. Pure;
    ///        unit-testable.
    [[nodiscard]] static QString approvedInstallSource(bool allow_unofficial);

    /// @brief True iff the file at @p choco_path is a genuine, Chocolatey-signed
    ///        binary: valid Authenticode signature chaining to a trusted root AND
    ///        a trusted Chocolatey signer subject. Fails closed on an empty path,
    ///        a missing/unsigned file, or off Windows. Exposed so every path that
    ///        launches the bundled choco.exe (manager execution AND the scanner)
    ///        shares one execution-time authenticity gate.
    [[nodiscard]] static bool isAuthenticChocoBinary(const QString& choco_path);

    /// @brief True iff @p extra_args carries no security-weakening override (e.g.
    ///        --ignore-checksums / --allow-empty-checksums) that would relax
    ///        Chocolatey's integrity policy. Fails closed on any such flag. Pure;
    ///        unit-testable.
    [[nodiscard]] static bool validateExtraArgs(const QStringList& extra_args);

    /// @brief Convert a timeout in seconds to milliseconds without integer
    ///        overflow: @p timeout_seconds when > 0 else @p default_seconds, in a
    ///        64-bit intermediate, clamped to [0, INT_MAX]. Pure; unit-testable.
    [[nodiscard]] static int computeTimeoutMs(int timeout_seconds, int default_seconds);

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
    QStringList buildInstallArgs(const InstallConfig& config) const;
    bool parseExitCode(int exit_code) const;

    /// @brief The fail-closed error for an inconsistent version/lock pairing in
    ///        @p config (locked-but-empty, version-without-lock, or an unparsable
    ///        locked version), or empty when the pairing is sound. Pure.
    [[nodiscard]] static QString versionLockError(const InstallConfig& config);

    // Authenticity: prove the bundled choco.exe is a genuine, Chocolatey-signed
    // binary (valid Authenticode signature AND trusted signer) before executing
    // it. Re-verified immediately before every execution (no cached verdict) so a
    // file swapped on disk after an earlier check cannot ride a stale "authentic".
    bool verifyChocoAuthenticity() const;
    bool ensureChocoAuthentic() const;

    // Output parsing
    QString extractErrorMessage(const QString& output) const;
    bool isNetworkError(const QString& output) const;
    bool isDependencyError(const QString& output) const;
    bool isPermissionError(const QString& output) const;

    // Validation (declared public+static below for headless testability)

    // Member variables
    QString m_choco_path;  // Path to choco.exe
    QString m_choco_dir;   // Root directory of portable Chocolatey
    bool m_initialized;
    int m_default_timeout_seconds;
    bool m_auto_confirm;
};

}  // namespace sak
