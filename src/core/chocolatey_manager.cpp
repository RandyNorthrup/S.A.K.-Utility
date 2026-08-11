// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/chocolatey_manager.h"

#include "sak/action_constants.h"
#include "sak/layout_constants.h"
#include "sak/logger.h"
#include "sak/process_runner.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QThread>

#include <algorithm>
#include <limits>
#include <string>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
// clang-format off
// Include order is load-bearing: windows.h first, then the crypto/trust headers
// that depend on its types (wintrust.h before softpub.h, which consumes it).
#include <windows.h>
#include <wincrypt.h>
#include <wintrust.h>
#include <softpub.h>
// clang-format on
#pragma comment(lib, "wintrust.lib")
#pragma comment(lib, "crypt32.lib")
#endif

namespace sak {

namespace {

constexpr int kVersionProbeTimeoutMs = kTimeoutProcessShortMs;
constexpr int kSearchTimeoutMs = kTimeoutProcessLongMs;
constexpr int kPackageQueryTimeoutMs = kTimeoutProcessMediumMs;
constexpr int kPackageRecordMinimumParts = 2;
constexpr int kOutdatedRecordMinimumParts = 3;
constexpr int kExitRebootInitiated = 1641;
constexpr int kMaxPackageNameLength = 100;
constexpr int kMaxVersionLength = 50;

// Official Chocolatey community feed. Pinned as the install --source for an
// official (allow_unofficial=false) install so a rogue source entry in the
// bundled choco config cannot silently redirect the download to an attacker feed.
constexpr char kApprovedChocoSource[] = "https://community.chocolatey.org/api/v2/";

#ifdef _WIN32

// True iff @p path carries a valid Authenticode signature chaining to a trusted
// root. WinVerifyTrust is the OS's own signature/chain validation; a swapped or
// unsigned choco.exe fails here.
bool winVerifyTrustFile(const std::wstring& path) {
    WINTRUST_FILE_INFO file_info{};
    file_info.cbStruct = sizeof(file_info);
    file_info.pcwszFilePath = path.c_str();

    WINTRUST_DATA wd{};
    wd.cbStruct = sizeof(wd);
    wd.dwUIChoice = WTD_UI_NONE;
    // Check the whole chain for revocation so a revoked-but-otherwise-genuine
    // Chocolatey cert is rejected. WTD_CACHE_ONLY_URL_RETRIEVAL keeps the check to
    // locally cached CRL/OCSP data: it still catches a known-revoked cert without
    // stalling on (or false-rejecting during) the offline servicing this portable
    // tool is built for.
    wd.fdwRevocationChecks = WTD_REVOKE_WHOLECHAIN;
    wd.dwProvFlags = WTD_CACHE_ONLY_URL_RETRIEVAL | WTD_REVOCATION_CHECK_CHAIN;
    wd.dwUnionChoice = WTD_CHOICE_FILE;
    wd.pFile = &file_info;
    wd.dwStateAction = WTD_STATEACTION_VERIFY;

    GUID action = WINTRUST_ACTION_GENERIC_VERIFY_V2;
    const LONG status = WinVerifyTrust(nullptr, &action, &wd);

    wd.dwStateAction = WTD_STATEACTION_CLOSE;
    WinVerifyTrust(nullptr, &action, &wd);
    return status == ERROR_SUCCESS;
}

// Human-readable subject (organization) name of a signing certificate.
QString certSubjectDisplayName(PCCERT_CONTEXT cert) {
    const DWORD len =
        CertGetNameStringW(cert, CERT_NAME_SIMPLE_DISPLAY_TYPE, 0, nullptr, nullptr, 0);
    if (len <= 1) {
        return {};
    }
    std::vector<wchar_t> buf(len);
    CertGetNameStringW(cert, CERT_NAME_SIMPLE_DISPLAY_TYPE, 0, nullptr, buf.data(), len);
    return QString::fromWCharArray(buf.data());
}

// Subject name of the certificate that actually signed the PKCS7 message.
QString subjectFromSignedMsg(HCERTSTORE store, HCRYPTMSG msg) {
    DWORD info_size = 0;
    if (!CryptMsgGetParam(msg, CMSG_SIGNER_CERT_INFO_PARAM, 0, nullptr, &info_size) ||
        info_size == 0) {
        return {};
    }
    std::vector<BYTE> info_buf(info_size);
    if (!CryptMsgGetParam(msg, CMSG_SIGNER_CERT_INFO_PARAM, 0, info_buf.data(), &info_size)) {
        return {};
    }
    auto* info = reinterpret_cast<CERT_INFO*>(info_buf.data());
    PCCERT_CONTEXT cert = CertFindCertificateInStore(
        store, X509_ASN_ENCODING | PKCS_7_ASN_ENCODING, 0, CERT_FIND_SUBJECT_CERT, info, nullptr);
    if (cert == nullptr) {
        return {};
    }
    const QString subject = certSubjectDisplayName(cert);
    CertFreeCertificateContext(cert);
    return subject;
}

// Signer subject of the embedded Authenticode signature on @p path (empty if the
// file carries no embedded signed message).
QString signerSubjectName(const std::wstring& path) {
    HCERTSTORE store = nullptr;
    HCRYPTMSG msg = nullptr;
    if (!CryptQueryObject(CERT_QUERY_OBJECT_FILE,
                          path.c_str(),
                          CERT_QUERY_CONTENT_FLAG_PKCS7_SIGNED_EMBED,
                          CERT_QUERY_FORMAT_FLAG_BINARY,
                          0,
                          nullptr,
                          nullptr,
                          nullptr,
                          &store,
                          &msg,
                          nullptr)) {
        return {};
    }
    QString subject;
    if (msg != nullptr && store != nullptr) {
        subject = subjectFromSignedMsg(store, msg);
    }
    if (msg != nullptr) {
        CryptMsgClose(msg);
    }
    if (store != nullptr) {
        CertCloseStore(store, 0);
    }
    return subject;
}

#endif  // _WIN32

}  // namespace

ChocolateyManager::ChocolateyManager(QObject* parent)
    : QObject(parent)
    , m_initialized(false)
    , m_default_timeout_seconds(kChocoTimeoutDefaultSec)
    , m_auto_confirm(true) {}

ChocolateyManager::~ChocolateyManager() = default;

bool ChocolateyManager::initialize(const QString& choco_portable_path) {
    if (choco_portable_path.isEmpty()) {
        sak::logError("[ChocolateyManager] initialize: choco_portable_path is empty");
        return false;
    }
    m_choco_dir = choco_portable_path;
    // Reset stale state so a re-init to a directory lacking choco.exe fails closed instead of
    // silently retaining the previously found executable and reporting initialized.
    m_choco_path.clear();
    m_initialized = false;

    // Look for choco.exe in common locations within portable directory
    QStringList possible_paths = {QDir(m_choco_dir).filePath("choco.exe"),
                                  QDir(m_choco_dir).filePath("bin/choco.exe"),
                                  QDir(m_choco_dir).filePath("chocolatey/bin/choco.exe")};

    const auto it = std::find_if(possible_paths.cbegin(),
                                 possible_paths.cend(),
                                 [](const QString& path) { return QFile::exists(path); });
    if (it != possible_paths.cend()) {
        m_choco_path = *it;
    }

    if (m_choco_path.isEmpty()) {
        sak::logWarning("[ChocolateyManager] choco.exe not found in {}",
                        choco_portable_path.toStdString());
        return false;
    }

    // Startup initialization must not execute external tools on the UI path.
    // Commands verify Chocolatey health when a package operation explicitly runs.
    m_initialized = true;

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
        sak::logWarning("[ChocolateyManager] choco.exe no longer exists at: {}",
                        m_choco_path.toStdString());
        m_initialized = false;
        return false;
    }

    // Existence plus a --version handshake is NOT integrity: a swapped or planted
    // choco.exe would also exist and print a version. Prove the binary is genuine
    // (valid Authenticode signature AND a trusted Chocolatey signer) before we
    // run it. Fail closed on any doubt.
    if (!ensureChocoAuthentic()) {
        m_initialized = false;
        return false;
    }

    // Try to execute a simple command
    Result result = executeChoco({"--version"}, kVersionProbeTimeoutMs);
    return result.success;
}

bool ChocolateyManager::isAuthenticChocoBinary(const QString& choco_path) {
#ifdef _WIN32
    if (choco_path.isEmpty()) {
        return false;
    }
    const std::wstring path = choco_path.toStdWString();
    if (!winVerifyTrustFile(path)) {
        sak::logWarning("[ChocolateyManager] choco.exe Authenticode verification failed: {}",
                        choco_path.toStdString());
        return false;
    }
    const QString signer = signerSubjectName(path);
    if (!isTrustedChocoSigner(signer)) {
        sak::logWarning("[ChocolateyManager] choco.exe signer not trusted: '{}'",
                        signer.toStdString());
        return false;
    }
    return true;
#else
    // No Authenticode facility off Windows: refuse rather than run unverified.
    Q_UNUSED(choco_path);
    return false;
#endif
}

bool ChocolateyManager::verifyChocoAuthenticity() const {
    return isAuthenticChocoBinary(m_choco_path);
}

bool ChocolateyManager::ensureChocoAuthentic() const {
    // Re-verify on every call rather than caching a permanent verdict: a binary
    // proven genuine once could be swapped on disk afterward (TOCTOU). A fresh
    // local signature check per execution closes that window and fails closed.
    return verifyChocoAuthenticity();
}

QString ChocolateyManager::getChocoPath() const {
    return m_choco_path;
}

QString ChocolateyManager::getChocoVersion() {
    if (!QFile::exists(m_choco_path)) {
        return QString();
    }

    Result result = executeChoco({"--version"}, kVersionProbeTimeoutMs);
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

QStringList ChocolateyManager::buildInstallArgs(const InstallConfig& config) const {
    QStringList args = {"install", config.package_name};
    if (config.version_locked && !config.version.isEmpty()) {
        args << "--version" << config.version;
    }
    if (config.auto_confirm || m_auto_confirm) {
        args << "-y";
    }
    if (config.force) {
        args << "--force";
    }
    if (!config.extra_args.isEmpty()) {
        args << config.extra_args;
    }
    // Pin the install to the approved feed unless the caller explicitly opts into
    // unofficial/configured sources. Appended last so it takes precedence over any
    // --source an extra_args entry may have supplied on an official install.
    const QString source = approvedInstallSource(config.allow_unofficial);
    if (!source.isEmpty()) {
        args << "--source" << source;
    }
    return args;
}

QString ChocolateyManager::approvedInstallSource(bool allow_unofficial) {
    if (allow_unofficial) {
        return {};  // caller explicitly accepts configured/unofficial sources
    }
    return QString::fromLatin1(kApprovedChocoSource);
}

bool ChocolateyManager::validateExtraArgs(const QStringList& extra_args) {
    // Checksum/integrity overrides would let a tampered or corrupt package install
    // silently. Refuse them outright rather than pass them through to choco.
    static const QStringList kForbidden = {QStringLiteral("--ignore-checksum"),
                                           QStringLiteral("--ignore-checksums"),
                                           QStringLiteral("--allow-empty-checksums"),
                                           QStringLiteral("--allow-empty-checksums-secure")};
    return std::none_of(extra_args.cbegin(), extra_args.cend(), [](const QString& arg) {
        const QString lower = arg.trimmed().toLower();
        return std::any_of(kForbidden.cbegin(), kForbidden.cend(), [&lower](const QString& bad) {
            return lower == bad || lower.startsWith(bad + QStringLiteral("="));
        });
    });
}

int ChocolateyManager::computeTimeoutMs(int timeout_seconds, int default_seconds) {
    const long long seconds = timeout_seconds > 0 ? timeout_seconds : default_seconds;
    const long long ms = seconds * static_cast<long long>(kMillisecondsPerSecond);
    if (ms < 0) {
        return 0;
    }
    if (ms > std::numeric_limits<int>::max()) {
        return std::numeric_limits<int>::max();
    }
    return static_cast<int>(ms);
}

// The version/lock pairing must be internally consistent before an install runs.
// Returns the specific fail-closed failure message, or empty when the pairing is
// sound. Kept as one guard so a lock/version contradiction can never slip through
// as a silent "install latest".
QString ChocolateyManager::versionLockError(const InstallConfig& config) {
    // Fail closed: a locked install with no version would silently resolve to the
    // latest, defeating the pin. Require the version when the lock is requested.
    if (config.version_locked && config.version.isEmpty()) {
        return "version_locked requires a non-empty version";
    }

    // Fail closed on the inverse contradiction: a version supplied WITHOUT the lock
    // would be silently ignored and the latest installed instead. Surface it rather
    // than install an unintended version.
    if (!config.version_locked && !config.version.isEmpty()) {
        return "version supplied without version_locked";
    }

    if (config.version_locked && !validateVersion(config.version)) {
        return "Invalid version format: " + config.version;
    }

    return {};
}

ChocolateyManager::Result ChocolateyManager::installPackage(const InstallConfig& config) {
    if (!m_initialized) {
        return {false, "", "ChocolateyManager not initialized", -1};
    }

    if (!validatePackageName(config.package_name)) {
        return {false, "", "Invalid package name: " + config.package_name, -1};
    }

    const QString version_error = versionLockError(config);
    if (!version_error.isEmpty()) {
        return {false, "", version_error, -1};
    }

    if (!validateExtraArgs(config.extra_args)) {
        return {false, "", "extra_args contains a forbidden security override", -1};
    }

    Q_EMIT installStarted(config.package_name);

    QStringList args = buildInstallArgs(config);
    const int timeout_ms = computeTimeoutMs(config.timeout_seconds, m_default_timeout_seconds);
    Result result = executeChoco(args, timeout_ms);

    if (result.success) {
        const QString ver = config.version_locked ? config.version : "latest";
        Q_EMIT installSuccess(config.package_name, ver);
    } else {
        Q_EMIT installFailed(config.package_name, result.error_message);
        sak::logWarning("[ChocolateyManager] Failed to install {}: {}",
                        config.package_name.toStdString(),
                        result.error_message.toStdString());
    }

    return result;
}

ChocolateyManager::Result ChocolateyManager::uninstallPackage(const QString& package_name,
                                                              bool auto_confirm) {
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

ChocolateyManager::Result ChocolateyManager::upgradePackage(const QString& package_name,
                                                            bool auto_confirm) {
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
    if (query.isEmpty()) {
        return {false, "", "Search query is empty", -1};
    }
    // Fail closed: a query beginning with '-' would be parsed by choco as an option
    // (e.g. "--source"), not a search term, since it becomes a bare argv element.
    if (query.trimmed().startsWith(QLatin1Char('-'))) {
        return {false, "", "Search query must not start with '-'", -1};
    }
    // Fail closed: a negative limit previously coerced to 0, which drops the
    // --page-size flag and silently returns an UNBOUNDED result set.
    if (max_results < 0) {
        return {false, "", "max_results must not be negative", -1};
    }
    if (!m_initialized) {
        return {false, "", "ChocolateyManager not initialized", -1};
    }

    Q_EMIT searchStarted(query);

    QStringList args = {"search", query, "--limit-output"};

    if (max_results > 0) {
        args << "--page-size" << QString::number(max_results);
    }

    Result result = executeChoco(args, kSearchTimeoutMs);

    if (result.success) {
        auto packages = parseSearchResults(result.output);
        Q_EMIT searchComplete(static_cast<int>(packages.size()));
    }

    return result;
}

std::vector<ChocolateyManager::PackageInfo> ChocolateyManager::parseSearchResults(
    const QString& output) {
    std::vector<PackageInfo> packages;

    // Parse limit-output format: "package_id|version"
    QStringList lines = output.split('\n', Qt::SkipEmptyParts);

    for (const QString& line : lines) {
        if (line.trimmed().isEmpty() || line.startsWith("Chocolatey")) {
            continue;
        }

        QStringList parts = line.split('|');
        if (parts.size() >= kPackageRecordMinimumParts) {
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
    // Reuse the STRUCTURED "name|version" record parse instead of a substring
    // contains(): a bare contains() over the raw output could match the package
    // name appearing incidentally elsewhere and report a false positive.
    return !getInstalledVersion(package_name).isEmpty();
}

QString ChocolateyManager::getInstalledVersion(const QString& package_name) {
    // Validate before the name becomes a bare choco argv element: an option-like id
    // (leading '-') could otherwise be parsed by choco as a flag, not a package.
    if (!validatePackageName(package_name)) {
        return {};
    }
    if (!m_initialized) {
        return QString();
    }

    QStringList args = {"list", "--local-only", package_name, "--exact", "--limit-output"};
    Result result = executeChoco(args, kPackageQueryTimeoutMs);

    if (!result.success) {
        return QString();
    }

    // Parse "package_name|version"
    QStringList lines = result.output.split('\n', Qt::SkipEmptyParts);
    for (const QString& line : lines) {
        if (!line.startsWith(package_name + "|")) {
            continue;
        }
        QStringList parts = line.split('|');
        if (parts.size() >= kPackageRecordMinimumParts) {
            return parts[1].trimmed();
        }
    }

    return QString();
}

bool ChocolateyManager::isPackageAvailable(const QString& package_name) {
    // Validate before the name is handed to choco (defense in depth): an option-like
    // id must never be interpreted as a search flag rather than a package.
    if (!validatePackageName(package_name)) {
        return false;
    }
    if (!m_initialized) {
        return false;
    }

    Result result = searchPackage(package_name, 1);
    if (!result.success) {
        return false;
    }

    auto packages = parseSearchResults(result.output);
    return std::any_of(packages.cbegin(), packages.cend(), [&package_name](const auto& pkg) {
        return pkg.package_id.compare(package_name, Qt::CaseInsensitive) == 0;
    });
}

QStringList ChocolateyManager::getOutdatedPackages() {
    QStringList outdated;

    if (!m_initialized) {
        sak::logWarning("ChocolateyManager not initialized");
        return outdated;
    }

    auto result = executeChoco({"outdated", "-r"}, kSearchTimeoutMs);
    if (!result.success) {
        return outdated;
    }

    QStringList lines = result.output.split('\n', Qt::SkipEmptyParts);
    for (const QString& line : lines) {
        QStringList parts = line.split('|');
        if (parts.size() >= kOutdatedRecordMinimumParts) {
            outdated.append(parts[0]);  // Package name
        }
    }

    return outdated;
}

ChocolateyManager::Result ChocolateyManager::installWithRetry(const InstallConfig& config,
                                                              int max_attempts,
                                                              int delay_seconds) {
    // Fail closed on invalid retry parameters: a non-positive attempt count would
    // otherwise return an empty-error failure, and a negative delay would convert to
    // a huge unsigned value in QThread::sleep and hang the operation indefinitely.
    if (max_attempts <= 0) {
        return {false, "", "installWithRetry: max_attempts must be positive", -1};
    }
    if (delay_seconds < 0) {
        return {false, "", "installWithRetry: delay_seconds must not be negative", -1};
    }

    Result last_result;

    for (int attempt = 1; attempt <= max_attempts; ++attempt) {
        if (attempt > 1) {
            Q_EMIT installRetrying(config.package_name, attempt, max_attempts);

            // Wait before retry
            QThread::sleep(delay_seconds);
        }

        last_result = installPackage(config);

        if (last_result.success) {
            return last_result;
        }

        // Check if error is retryable
        if (isPermissionError(last_result.output)) {
            sak::logWarning("[ChocolateyManager] Permission error - not retrying");
            break;
        }
    }

    return last_result;
}

void ChocolateyManager::setDefaultTimeout(int seconds) {
    // Reject a negative default rather than storing it: a negative would later be
    // treated as "no explicit timeout" and mask the caller's invalid input.
    if (seconds < 0) {
        sak::logWarning("[ChocolateyManager] setDefaultTimeout: ignoring negative value {}",
                        seconds);
        return;
    }
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
    if (args.isEmpty()) {
        return {false, "", "No arguments provided to Chocolatey", -1};
    }
    timeout_ms = std::max(timeout_ms, 0);
    // Fail closed: never launch the bundled choco.exe unless it has been proven a
    // genuine, Chocolatey-signed binary. This is the single execution choke point,
    // so the authenticity gate covers every choco invocation, not just install.
    if (!ensureChocoAuthentic()) {
        return {false, "", "Bundled choco.exe failed authenticity verification", -1};
    }
    // Set up environment
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert("ChocolateyInstall", m_choco_dir);

    // Build command
    QString program = m_choco_path;
    const int effective_timeout_ms =
        timeout_ms > 0 ? timeout_ms : sak::kChocoTimeoutExtendedSec * kMillisecondsPerSecond;
    const auto process_result =
        sak::runProcessWithEnvironment(program, args, effective_timeout_ms, env);

    if (process_result.timed_out) {
        return {false, "", "Command timed out", -1};
    }

    // Get output
    QString stdout_output = process_result.std_out;
    QString stderr_output = process_result.std_err;
    QString combined_output = stdout_output + "\n" + stderr_output;

    int exit_code = process_result.exit_code;
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

bool ChocolateyManager::parseExitCode(int exit_code) const {
    // Chocolatey exit codes:
    // 0 = success
    // 1 = generic error
    // 2 = nothing to do / no packages found
    // 1641/3010 = success with reboot required

    return (exit_code == kExitSuccess || exit_code == kExitRebootInitiated ||
            exit_code == kExitRebootRequired);
}

QString ChocolateyManager::extractErrorMessage(const QString& output) const {
    if (output.isEmpty()) {
        return "Unknown error";
    }
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
    if (output.isEmpty()) {
        return false;
    }
    QStringList network_keywords = {"network",
                                    "timeout",
                                    "connection",
                                    "unreachable",
                                    "dns",
                                    "proxy",
                                    "ssl",
                                    "certificate",
                                    "tls"};

    QString lower_output = output.toLower();
    return std::any_of(network_keywords.cbegin(),
                       network_keywords.cend(),
                       [&lower_output](const QString& keyword) {
                           return lower_output.contains(keyword);
                       });
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

bool ChocolateyManager::validatePackageName(const QString& package_name) {
    if (package_name.isEmpty() || package_name.length() > kMaxPackageNameLength) {
        return false;
    }

    // Refuse Chocolatey's reserved bulk target "all": `choco uninstall all` / `choco upgrade all`
    // operate on EVERY installed package, so a single unbound token (an AI-planned or
    // injection-influenced package_id) would uninstall/upgrade the whole bundled tree. A real
    // bulk operation must go through an explicit, separately-confirmed path, never this
    // per-package validator.
    if (package_name.compare(QLatin1String("all"), Qt::CaseInsensitive) == 0) {
        return false;
    }

    // Must START with an alphanumeric so a crafted id (e.g. "-force",
    // "--source=evil") can never be parsed by choco as an OPTION rather than a
    // package; the remainder may contain [A-Za-z0-9._-].
    static const QRegularExpression valid_name(R"(^[a-zA-Z0-9][a-zA-Z0-9._-]*$)");
    return valid_name.match(package_name).hasMatch();
}

bool ChocolateyManager::validateVersion(const QString& version) {
    if (version.isEmpty() || version.length() > kMaxVersionLength) {
        return false;
    }

    // NuGet/SemVer: numeric core "1", "1.2", "1.2.3", "1.2.3.4"; optional
    // dotted prerelease "-beta.1" / "-rc.1"; optional "+build.5" metadata. Must
    // start with a digit (never an option). Previously a dotted prerelease or
    // build metadata was wrongly rejected.
    static const QRegularExpression valid_version(
        R"(^\d+(\.\d+)*(-[a-zA-Z0-9.-]+)?(\+[a-zA-Z0-9.-]+)?$)");
    return valid_version.match(version).hasMatch();
}

bool ChocolateyManager::isTrustedChocoSigner(const QString& signer_subject) {
    if (signer_subject.isEmpty()) {
        return false;
    }
    // Chocolatey's Authenticode leaf certificate is issued to the organization
    // "Chocolatey Software, Inc." Require the WHOLE word "Chocolatey" (case
    // insensitive) so a subject that merely embeds the token (e.g.
    // "NotChocolateyEvil Ltd") cannot masquerade as the genuine signer.
    static const QRegularExpression choco_signer(R"(\bChocolatey\b)",
                                                 QRegularExpression::CaseInsensitiveOption);
    return choco_signer.match(signer_subject).hasMatch();
}

}  // namespace sak
