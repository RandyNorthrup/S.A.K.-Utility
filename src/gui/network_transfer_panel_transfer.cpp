// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/app_scanner.h"
#include "sak/config_manager.h"
#include "sak/file_hash.h"
#include "sak/file_scanner.h"
#include "sak/format_utils.h"
#include "sak/layout_constants.h"
#include "sak/logger.h"
#include "sak/mapping_engine.h"
#include "sak/migration_orchestrator.h"
#include "sak/network_transfer_controller.h"
#include "sak/network_transfer_panel.h"
#include "sak/parallel_transfer_manager.h"
#include "sak/path_utils.h"
#include "sak/per_user_customization_dialog.h"
#include "sak/permission_manager.h"
#include "sak/smart_file_filter.h"
#include "sak/version.h"
#include "sak/wifi_profile_scanner.h"
#include "sak/windows_user_scanner.h"

#include <QApplication>
#include <QCheckBox>
#include <QColor>
#include <QComboBox>
#include <QCryptographicHash>
#include <QDataStream>
#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QFrame>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QHostInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QMimeData>
#include <QNetworkInterface>
#include <QProcess>
#include <QProgressBar>
#include <QPushButton>
#include <QScrollArea>
#include <QSet>
#include <QSpinBox>
#include <QStackedWidget>
#include <QStandardPaths>
#include <QTableWidget>
#include <QtConcurrent>
#include <QTextEdit>
#include <QTime>
#include <QUuid>
#include <QVBoxLayout>

#include <filesystem>

namespace sak {

namespace {
constexpr int kUserColSelect = 0;
constexpr int kUserColName = 1;
constexpr int kUserColPath = 2;
constexpr int kUserColSize = 3;

constexpr int kPeerColName = 0;
constexpr int kPeerColIp = 1;
constexpr int kPeerColMode = 2;
constexpr int kPeerColCaps = 3;
constexpr int kPeerColSeen = 4;

QString categorizeApp(const QString& name) {
    Q_ASSERT(!name.isEmpty());
    const QString lower = name.toLower();
    if (lower.contains("chrome") || lower.contains("firefox") || lower.contains("edge") ||
        lower.contains("opera") || lower.contains("brave") || lower.contains("browser")) {
        return QCoreApplication::translate("NetworkTransferPanel", "Browsers");
    }
    if (lower.contains("visual studio") || lower.contains("vscode") ||
        lower.contains("jetbrains") || lower.contains("code") || lower.contains("sublime") ||
        lower.contains("notepad++")) {
        return QCoreApplication::translate("NetworkTransferPanel", "Development");
    }
    if (lower.contains("office") || lower.contains("word") || lower.contains("excel")) {
        return QCoreApplication::translate("NetworkTransferPanel", "Productivity");
    }
    if (lower.contains("discord") || lower.contains("slack") || lower.contains("teams") ||
        lower.contains("zoom")) {
        return QCoreApplication::translate("NetworkTransferPanel", "Communication");
    }
    if (lower.contains("steam") || lower.contains("epic games") || lower.contains("game")) {
        return QCoreApplication::translate("NetworkTransferPanel", "Gaming");
    }
    if (lower.contains("vlc") || lower.contains("spotify") || lower.contains("obs")) {
        return QCoreApplication::translate("NetworkTransferPanel", "Media");
    }
    if (lower.contains("7-zip") || lower.contains("winrar") || lower.contains("ccleaner")) {
        return QCoreApplication::translate("NetworkTransferPanel", "Utilities");
    }
    if (lower.contains("nvidia") || lower.contains("amd") || lower.contains("driver")) {
        return QCoreApplication::translate("NetworkTransferPanel", "Drivers & Hardware");
    }
    return QCoreApplication::translate("NetworkTransferPanel", "Other");
}

qint64 calculateSourceSize(const QString& path) {
    Q_ASSERT(!path.isEmpty());
    QFileInfo info(path);
    if (!info.exists()) {
        return -1;
    }
    if (!info.isDir()) {
        return info.size();
    }
    qint64 size = 0;
    QDirIterator it(path, QDir::Files | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        it.next();
        size += it.fileInfo().size();
    }
    return size;
}

QVector<EthernetConfigInfo> parseNetshEthernetOutput(const QString& output) {
    Q_ASSERT(!output.isEmpty());
    QVector<EthernetConfigInfo> configs;
    EthernetConfigInfo current;
    bool inAdapter = false;

    for (const QString& line : output.split('\n')) {
        QString trimmed = line.trimmed();

        if (trimmed.startsWith("Configuration for interface")) {
            if (inAdapter && !current.adapter_name.isEmpty()) {
                configs.append(current);
            }
            current = EthernetConfigInfo();
            int firstQuote = trimmed.indexOf('"');
            int lastQuote = trimmed.lastIndexOf('"');
            if (firstQuote >= 0 && lastQuote > firstQuote) {
                current.adapter_name = trimmed.mid(firstQuote + 1, lastQuote - firstQuote - 1);
            }
            inAdapter = true;
            continue;
        }
        if (!inAdapter) {
            continue;
        }

        if (trimmed.startsWith("DHCP enabled:", Qt::CaseInsensitive)) {
            current.dhcp_enabled = trimmed.contains("Yes", Qt::CaseInsensitive);
        } else if (trimmed.startsWith("IP Address:", Qt::CaseInsensitive)) {
            int idx = trimmed.indexOf(':');
            if (idx >= 0) {
                current.ip_address = trimmed.mid(idx + 1).trimmed();
            }
        } else if (trimmed.startsWith("Subnet Prefix:", Qt::CaseInsensitive) ||
                   trimmed.startsWith("SubnetMask:", Qt::CaseInsensitive)) {
            int idx = trimmed.indexOf(':');
            if (idx >= 0) {
                current.subnet_mask = trimmed.mid(idx + 1).trimmed();
            }
        } else if (trimmed.startsWith("Default Gateway:", Qt::CaseInsensitive)) {
            int idx = trimmed.indexOf(':');
            if (idx >= 0) {
                current.default_gateway = trimmed.mid(idx + 1).trimmed();
            }
        } else if (trimmed.contains("DNS Server", Qt::CaseInsensitive) ||
                   trimmed.startsWith("Statically Configured DNS", Qt::CaseInsensitive)) {
            int idx = trimmed.indexOf(':');
            if (idx < 0) {
                continue;
            }
            QString dns = trimmed.mid(idx + 1).trimmed();
            if (dns.isEmpty()) {
                continue;
            }
            if (current.dns_primary.isEmpty()) {
                current.dns_primary = dns;
            } else if (current.dns_secondary.isEmpty()) {
                current.dns_secondary = dns;
            }
        }
    }
    if (inAdapter && !current.adapter_name.isEmpty()) {
        configs.append(current);
    }
    return configs;
}

QVector<AppDataSourceInfo> getCommonAppDataSources() {
    return {
        {"Chrome Profiles", "Browsers", "AppData/Local/Google/Chrome/User Data", 0, false, true},
        {"Firefox Profiles",
         "Browsers",
         "AppData/Roaming/Mozilla/Firefox/Profiles",
         0,
         false,
         true},
        {"Edge Profiles", "Browsers", "AppData/Local/Microsoft/Edge/User Data", 0, false, true},
        {"Brave Profiles",
         "Browsers",
         "AppData/Local/BraveSoftware/Brave-Browser/User Data",
         0,
         false,
         true},
        {"Thunderbird Profiles", "Email", "AppData/Roaming/Thunderbird/Profiles", 0, false, true},
        {"Outlook Data", "Email", "AppData/Local/Microsoft/Outlook", 0, false, true},
        {"VS Code Settings", "Development", "AppData/Roaming/Code/User", 0, false, true},
        {"VS Code Extensions", "Development", ".vscode/extensions", 0, false, true},
        {"JetBrains Settings", "Development", "AppData/Roaming/JetBrains", 0, false, true},
        {"Sublime Text Settings", "Development", "AppData/Roaming/Sublime Text 3", 0, false, true},
        {"Notepad++ Settings", "Development", "AppData/Roaming/Notepad++", 0, false, true},
        {"Discord Data", "Communication", "AppData/Roaming/discord", 0, false, true},
        {"Telegram Data", "Communication", "AppData/Roaming/Telegram Desktop", 0, false, true},
        {"Signal Data", "Communication", "AppData/Roaming/Signal", 0, false, true},
        {"Slack Data", "Communication", "AppData/Roaming/Slack", 0, false, true},
        {"Steam Config", "Gaming", "AppData/Local/Steam", 0, false, true},
        {"Minecraft Data", "Gaming", "AppData/Roaming/.minecraft", 0, false, true},
        {"Spotify Data", "Media", "AppData/Roaming/Spotify", 0, false, true},
        {"VLC Settings", "Media", "AppData/Roaming/vlc", 0, false, true},
        {"OBS Studio", "Media", "AppData/Roaming/obs-studio", 0, false, true},
        {"PuTTY Sessions", "Utilities", "AppData/Roaming/PuTTY", 0, false, true},
        {"WinSCP Settings", "Utilities", "AppData/Roaming/WinSCP", 0, false, true},
        {"FileZilla Settings", "Utilities", "AppData/Roaming/FileZilla", 0, false, true},
        {"PowerShell Profile", "Utilities", "Documents/PowerShell", 0, false, true},
        {"Windows Terminal Settings",
         "Utilities",
         "AppData/Local/Packages/Microsoft.WindowsTerminal_8wekyb3d8bbwe/LocalState",
         0,
         false,
         true},
        {"SSH Keys", "System", ".ssh", 0, false, true},
        {"Git Config", "System", ".gitconfig", 0, false, true},
        {"npm Config", "System", "AppData/Roaming/npm", 0, false, true},
        {"pip Config", "System", "AppData/Roaming/pip", 0, false, true},
    };
}
}  // namespace

void NetworkTransferPanel::onScanUsers() {
    Q_ASSERT(m_userTable);
    Q_ASSERT(m_userScanner);
    m_users = m_userScanner->scanUsers();
    m_userTable->setRowCount(0);

    for (int i = 0; i < m_users.size(); ++i) {
        const auto& user = m_users[i];
        m_userTable->insertRow(i);

        auto* selectItem = new QTableWidgetItem();
        selectItem->setCheckState(Qt::Checked);
        m_userTable->setItem(i, kUserColSelect, selectItem);

        m_userTable->setItem(i, kUserColName, new QTableWidgetItem(user.username));
        m_userTable->setItem(i, kUserColPath, new QTableWidgetItem(user.profile_path));
        m_userTable->setItem(i,
                             kUserColSize,
                             new QTableWidgetItem(formatBytes(user.total_size_estimated)));
    }

    Q_EMIT logOutput(tr("Scanned %1 users").arg(m_users.size()));
}

void NetworkTransferPanel::onCustomizeUser() {
    Q_ASSERT(m_userTable);
    auto selected = m_userTable->currentRow();
    if (selected < 0 || selected >= m_users.size()) {
        QMessageBox::information(this, tr("Select User"), tr("Select a user to customize."));
        return;
    }

    auto& profile = m_users[selected];
    PerUserCustomizationDialog dialog(profile, this);
    if (dialog.exec() == QDialog::Accepted) {
        profile.folder_selections = dialog.getFolderSelections();
    }
}

void NetworkTransferPanel::onScanInstalledApps() {
    Q_ASSERT(m_scanAppsButton);
    Q_ASSERT(m_installedAppsLabel);
    m_scanAppsButton->setEnabled(false);
    m_installedAppsLabel->setText(tr("Scanning installed applications..."));
    Q_EMIT logOutput(tr("Scanning installed applications..."));

    QPointer<NetworkTransferPanel> safeThis(this);
    auto* watcher = new QFutureWatcher<QVector<InstalledAppInfo>>(this);

    connect(
        watcher, &QFutureWatcher<QVector<InstalledAppInfo>>::finished, this, [safeThis, watcher]() {
            watcher->deleteLater();
            if (!safeThis) {
                return;
            }

            safeThis->m_scannedApps = watcher->result();
            safeThis->m_scanAppsButton->setEnabled(true);
            safeThis->m_installedAppsLabel->setText(
                QCoreApplication::translate("NetworkTransferPanel", "Found %1 application(s)")
                    .arg(safeThis->m_scannedApps.size()));
            Q_EMIT safeThis->logOutput(
                QCoreApplication::translate("NetworkTransferPanel",
                                            "Found %1 installed application(s)")
                    .arg(safeThis->m_scannedApps.size()));
        });

    watcher->setFuture(QtConcurrent::run([]() -> QVector<InstalledAppInfo> {
        AppScanner scanner;
        auto apps = scanner.scanAll();

        QVector<InstalledAppInfo> result;
        result.reserve(apps.size());
        for (const auto& app : apps) {
            InstalledAppInfo info;
            info.name = app.name;
            info.version = app.version;
            info.publisher = app.publisher;
            info.selected = true;
            info.category = categorizeApp(app.name);
            result.append(info);
        }
        return result;
    }));
}

void NetworkTransferPanel::onScanAppData() {
    Q_ASSERT(m_scanAppDataButton);
    Q_ASSERT(m_appDataLabel);
    m_scanAppDataButton->setEnabled(false);
    m_appDataLabel->setText(tr("Scanning application data..."));
    Q_EMIT logOutput(tr("Scanning application data sources..."));

    QVector<AppDataSourceInfo> allSources;
    auto commonSources = getCommonAppDataSources();

    for (const auto& user : m_users) {
        if (!user.is_selected) {
            continue;
        }

        for (auto source : commonSources) {
            QString fullPath = user.profile_path + "/" + source.relative_path;
            qint64 pathSize = calculateSourceSize(fullPath);
            if (pathSize < 0) {
                continue;
            }
            source.exists = true;
            source.size_bytes = pathSize;
            allSources.append(source);
        }
    }

    m_scannedAppData = allSources;
    m_scanAppDataButton->setEnabled(true);
    m_appDataLabel->setText(tr("Found %1 app data source(s)").arg(allSources.size()));
    Q_EMIT logOutput(tr("Found %1 application data source(s)").arg(allSources.size()));
}

void NetworkTransferPanel::onScanWifiProfiles() {
    Q_ASSERT(m_scanWifiButton);
    Q_ASSERT(m_wifiLabel);
    m_scanWifiButton->setEnabled(false);
    m_wifiLabel->setText(tr("Scanning WiFi profiles..."));
    Q_EMIT logOutput(tr("Scanning WiFi profiles..."));

    const QVector<WifiProfileInfo> profiles = sak::scanAllWifiProfiles();

    m_scannedWifi = profiles;
    m_scanWifiButton->setEnabled(true);
    m_wifiLabel->setText(tr("Found %1 WiFi profile(s)").arg(profiles.size()));
    Q_EMIT logOutput(tr("Found %1 WiFi profile(s)").arg(profiles.size()));
}

void NetworkTransferPanel::onScanEthernetConfigs() {
    Q_ASSERT(m_scanEthernetButton);
    Q_ASSERT(m_ethernetLabel);
    m_scanEthernetButton->setEnabled(false);
    m_ethernetLabel->setText(tr("Scanning ethernet adapters..."));
    Q_EMIT logOutput(tr("Scanning ethernet adapters..."));

    QProcess process;
    process.start("netsh", {"interface", "ipv4", "show", "config"});
    if (!process.waitForStarted(sak::kTimeoutProcessStartMs)) {
        sak::logError("netsh failed to start for ethernet adapter scan");
        m_scanEthernetButton->setEnabled(true);
        m_ethernetLabel->setText(tr("Failed to start ethernet scan"));
        Q_EMIT logOutput(tr("Failed to start ethernet adapter scan"));
        return;
    }
    if (!process.waitForFinished(kTimeoutNetworkReadMs)) {
        sak::logError("Timed out scanning ethernet adapters");
        process.kill();
        process.waitForFinished(2000);
        m_scanEthernetButton->setEnabled(true);
        m_ethernetLabel->setText(tr("Ethernet scan timed out"));
        Q_EMIT logOutput(tr("Ethernet adapter scan timed out"));
        return;
    }

    QString output = QString::fromLocal8Bit(process.readAllStandardOutput());
    auto configs = parseNetshEthernetOutput(output);

    m_scannedEthernet = configs;
    m_scanEthernetButton->setEnabled(true);
    m_ethernetLabel->setText(tr("Found %1 ethernet adapter(s)").arg(configs.size()));
    Q_EMIT logOutput(tr("Found %1 ethernet adapter(s)").arg(configs.size()));
}

void NetworkTransferPanel::onDiscoverPeers() {
    Q_ASSERT(m_peerTable);
    Q_ASSERT(m_controller);
    m_peers.clear();
    m_peerTable->setRowCount(0);
    m_controller->configure(m_settings);
    if (!m_settings.auto_discovery_enabled) {
        QMessageBox::information(this,
                                 tr("Discovery Disabled"),
                                 tr("Enable auto discovery in settings to find peers."));
        return;
    }
    m_controller->startDiscovery("source");
    Q_EMIT logOutput(tr("Peer discovery started"));
}

void NetworkTransferPanel::onStartSource() {
    Q_ASSERT(m_peerTable);
    Q_ASSERT(m_controller);
    buildManifest();

    TransferPeerInfo peer;
    if (m_peerTable->currentRow() >= 0) {
        auto* ipItem = m_peerTable->item(m_peerTable->currentRow(), kPeerColIp);
        if (ipItem) {
            peer.ip_address = ipItem->text();
        }
    }

    if (peer.ip_address.isEmpty()) {
        peer.ip_address = m_manualIpEdit->text();
    }

    if (peer.ip_address.isEmpty()) {
        sak::logWarning("Missing Destination: Select a peer or enter a manual IP.");
        QMessageBox::warning(this,
                             tr("Missing Destination"),
                             tr("Select a peer or enter a manual IP."));
        return;
    }

    m_settings.control_port = static_cast<quint16>(m_manualPortSpin->value());
    peer.control_port = static_cast<quint16>(m_manualPortSpin->value());
    peer.data_port = m_settings.data_port;
    peer.hostname = peer.ip_address;

    m_settings.encryption_enabled = m_encryptCheck->isChecked();
    m_settings.compression_enabled = m_compressCheck->isChecked();
    m_settings.resume_enabled = m_resumeCheck->isChecked();
    m_settings.chunk_size = m_chunkSizeSpin->value() * sak::kBytesPerKB;
    m_settings.max_bandwidth_kbps = m_bandwidthSpin->value();

    if (m_settings.encryption_enabled && m_passphraseEdit->text().isEmpty()) {
        sak::logWarning("Missing Passphrase: Enter a passphrase for encrypted transfers.");
        QMessageBox::warning(this,
                             tr("Missing Passphrase"),
                             tr("Enter a passphrase for encrypted transfers."));
        return;
    }

    if (m_settings.encryption_enabled && m_passphraseEdit->text().size() < 8) {
        sak::logWarning("Weak Passphrase: Passphrase must be at least 8 characters.");
        QMessageBox::warning(this,
                             tr("Weak Passphrase"),
                             tr("Passphrase must be at least 8 characters."));
        return;
    }

    m_transferStarted = QDateTime::currentDateTime();
    m_transferErrors.clear();
    m_isSourceTransfer = true;
    m_sourceTransferActive = true;
    m_sourceTransferPaused = false;
    updateTransferButton();
    updatePauseResumeButton();

    m_controller->configure(m_settings);
    m_controller->startSource(m_currentManifest, m_currentFiles, peer, m_passphraseEdit->text());
}

void NetworkTransferPanel::onStartDestination() {
    Q_ASSERT(m_encryptCheck);
    Q_ASSERT(m_controller);
    m_settings.encryption_enabled = m_encryptCheck->isChecked();
    m_settings.compression_enabled = m_compressCheck->isChecked();
    m_settings.resume_enabled = m_resumeCheck->isChecked();
    m_settings.chunk_size = m_chunkSizeSpin->value() * sak::kBytesPerKB;
    m_settings.max_bandwidth_kbps = m_bandwidthSpin->value();

    if (m_settings.encryption_enabled && m_destinationPassphraseEdit->text().isEmpty()) {
        sak::logWarning("Missing Passphrase: Enter a passphrase for encrypted transfers.");
        QMessageBox::warning(this,
                             tr("Missing Passphrase"),
                             tr("Enter a passphrase for encrypted transfers."));
        return;
    }

    if (m_settings.encryption_enabled && m_destinationPassphraseEdit->text().size() < 8) {
        sak::logWarning("Weak Passphrase: Passphrase must be at least 8 characters.");
        QMessageBox::warning(this,
                             tr("Weak Passphrase"),
                             tr("Passphrase must be at least 8 characters."));
        return;
    }

    const QString base = destinationBase();
    if (base.isEmpty()) {
        sak::logWarning("Missing Destination: Set a destination base path.");
        QMessageBox::warning(this, tr("Missing Destination"), tr("Set a destination base path."));
        return;
    }

    QDir destDir(base);
    if (!destDir.exists() && !destDir.mkpath(".")) {
        sak::logWarning("Destination Error: Failed to create destination base directory.");
        QMessageBox::warning(this,
                             tr("Destination Error"),
                             tr("Failed to create destination base directory."));
        return;
    }

    m_transferStarted = QDateTime::currentDateTime();
    m_transferErrors.clear();
    m_isSourceTransfer = false;
    m_controller->configure(m_settings);
    m_controller->startDestination(m_destinationPassphraseEdit->text(), destinationBase());
}

void NetworkTransferPanel::onConnectOrchestrator() {
    Q_ASSERT(m_orchestratorHostEdit);
    Q_ASSERT(m_controller);
    const QString host = m_orchestratorHostEdit->text().trimmed();
    if (host.isEmpty()) {
        sak::logWarning("Missing Host: Enter an orchestrator host.");
        QMessageBox::warning(this, tr("Missing Host"), tr("Enter an orchestrator host."));
        return;
    }

    DestinationPC destination;
    destination.destination_id = QHostInfo::localHostName();
    destination.hostname = QHostInfo::localHostName();
    destination.ip_address = host;
    destination.control_port = m_settings.control_port;
    destination.data_port = m_settings.data_port;
    destination.status = "ready";
    destination.last_seen = QDateTime::currentDateTimeUtc();

    m_controller->connectToOrchestrator(QHostAddress(host),
                                        static_cast<quint16>(m_orchestratorPortSpin->value()),
                                        destination);
    Q_EMIT logOutput(
        tr("Connecting to orchestrator at %1:%2").arg(host).arg(m_orchestratorPortSpin->value()));
}

void NetworkTransferPanel::onApproveTransfer() {
    m_destinationTransferActive = true;
    if (!m_activeAssignment.job_id.isEmpty()) {
        m_assignmentStatusByJob[m_activeAssignment.job_id] = tr("approved");
        m_assignmentEventByJob[m_activeAssignment.job_id] = tr("Transfer approved");
        refreshAssignmentStatus();
        persistAssignmentQueue();
    }
    m_controller->approveTransfer(true);
}

void NetworkTransferPanel::onRejectTransfer() {
    Q_ASSERT(m_controller);
    Q_ASSERT(m_activeAssignmentLabel);
    m_destinationTransferActive = false;
    m_controller->approveTransfer(false);

    if (!m_activeAssignment.job_id.isEmpty()) {
        m_assignmentStatusByJob[m_activeAssignment.job_id] = tr("rejected");
        m_assignmentEventByJob[m_activeAssignment.job_id] = tr("Transfer rejected");
        refreshAssignmentStatus();
        persistAssignmentQueue();
    }

    m_manifestValidated = false;

    m_activeAssignment = {};
    if (m_activeAssignmentLabel) {
        m_activeAssignmentLabel->setText(tr("No active assignment"));
    }
    if (!m_assignmentQueue.isEmpty()) {
        const auto next = m_assignmentQueue.dequeue();
        activateAssignment(next);
    } else {
        refreshAssignmentQueue();
    }
    persistAssignmentQueue();
}

void NetworkTransferPanel::onPeerDiscovered(const TransferPeerInfo& peer) {
    Q_ASSERT(!m_peers.isEmpty());
    Q_ASSERT(m_peerTable);
    if (m_peers.contains(peer.peer_id)) {
        m_peers[peer.peer_id] = peer;
    } else {
        m_peers.insert(peer.peer_id, peer);
    }

    m_peerTable->setRowCount(0);
    int row = 0;
    for (const auto& entry : m_peers) {
        m_peerTable->insertRow(row);
        m_peerTable->setItem(row, kPeerColName, new QTableWidgetItem(entry.hostname));
        m_peerTable->setItem(row, kPeerColIp, new QTableWidgetItem(entry.ip_address));
        m_peerTable->setItem(row, kPeerColMode, new QTableWidgetItem(entry.mode));
        m_peerTable->setItem(row,
                             kPeerColCaps,
                             new QTableWidgetItem(entry.capabilities.join(", ")));
        m_peerTable->setItem(row,
                             kPeerColSeen,
                             new QTableWidgetItem(entry.last_seen.toString("hh:mm:ss")));
        row++;
    }
}

void NetworkTransferPanel::onManifestReceived(const TransferManifest& manifest) {
    Q_ASSERT(m_manifestText);
    Q_ASSERT(m_controller);
    m_manifestValidated = false;
    m_currentManifest = manifest;
    QJsonDocument doc(manifest.toJson(false));
    m_manifestText->setText(QString::fromUtf8(doc.toJson(QJsonDocument::Indented)));
    m_manifestText->append(tr("\nSummary: %1 users, %2 files, %3 total")
                               .arg(manifest.users.size())
                               .arg(manifest.total_files)
                               .arg(formatBytes(manifest.total_bytes)));

    if (!manifest.installed_apps.isEmpty()) {
        m_manifestText->append(tr("  Installed Apps: %1").arg(manifest.installed_apps.size()));
    }
    if (!manifest.app_data_sources.isEmpty()) {
        m_manifestText->append(tr("  App Data Sources: %1").arg(manifest.app_data_sources.size()));
    }
    if (!manifest.wifi_profiles.isEmpty()) {
        m_manifestText->append(tr("  WiFi Profiles: %1").arg(manifest.wifi_profiles.size()));
    }
    if (!manifest.ethernet_configs.isEmpty()) {
        m_manifestText->append(tr("  Ethernet Configs: %1").arg(manifest.ethernet_configs.size()));
    }

    TransferManifest verifyManifest = manifest;
    verifyManifest.checksum_sha256.clear();
    QJsonDocument verifyDoc(verifyManifest.toJson());
    QByteArray verifyHash = QCryptographicHash::hash(verifyDoc.toJson(QJsonDocument::Compact),
                                                     QCryptographicHash::Sha256);
    if (!manifest.checksum_sha256.isEmpty() &&
        verifyHash.toHex() != manifest.checksum_sha256.toUtf8()) {
        m_manifestText->append(tr("\nWARNING: Manifest checksum mismatch."));
        m_controller->approveTransfer(false);
        m_approveButton->setEnabled(false);
        return;
    }

    auto available =
        path_utils::getAvailableSpace(std::filesystem::path(destinationBase().toStdString()));
    if (!available) {
        m_manifestText->append(tr("\nWARNING: Unable to determine available disk space."));
        m_controller->approveTransfer(false);
        m_approveButton->setEnabled(false);
        return;
    }

    if (static_cast<qint64>(*available) < manifest.total_bytes) {
        m_manifestText->append(tr("\nWARNING: Insufficient disk space. Required: %1, Available: %2")
                                   .arg(formatBytes(manifest.total_bytes))
                                   .arg(formatBytes(static_cast<qint64>(*available))));
        m_controller->approveTransfer(false);
        m_approveButton->setEnabled(false);
    } else {
        m_approveButton->setEnabled(true);
        m_manifestValidated = true;
    }

    if (m_orchestrationAssignmentPending && m_autoApproveOrchestratorCheck &&
        m_autoApproveOrchestratorCheck->isChecked() && m_approveButton->isEnabled()) {
        m_orchestrationAssignmentPending = false;
        onApproveTransfer();
    }
}

void NetworkTransferPanel::onTransferProgress(qint64 bytes, qint64 total) {
    Q_ASSERT(total >= 0);
    if (total > 0) {
        const int percent = static_cast<int>((bytes * 100) / total);
        Q_EMIT progressUpdate(percent, 100);
    }
}

void NetworkTransferPanel::onTransferCompleted(bool success, const QString& message) {
    Q_ASSERT(m_activeAssignmentLabel);
    Q_ASSERT(m_modeCombo);
    Q_EMIT progressUpdate(success ? 100 : 0, 100);
    Q_EMIT logOutput(message);

    m_destinationTransferActive = false;
    m_sourceTransferActive = false;
    m_sourceTransferPaused = false;
    m_manifestValidated = false;
    updateTransferButton();
    updatePauseResumeButton();
    if (!m_activeAssignment.job_id.isEmpty()) {
        m_assignmentStatusByJob[m_activeAssignment.job_id] = success ? tr("completed")
                                                                     : tr("failed");
        m_assignmentEventByJob[m_activeAssignment.job_id] = message;
        refreshAssignmentStatus();
        persistAssignmentQueue();
    }
    m_activeAssignment = {};
    if (m_activeAssignmentLabel) {
        m_activeAssignmentLabel->setText(tr("No active assignment"));
    }

    saveTransferReport(success);

    if (success && m_modeCombo->currentIndex() == 1) {
        writeAdditionalDataFiles(destinationBase(), m_currentManifest);
        if (m_applyRestoreCheck->isChecked()) {
            startPostTransferRestore();
        }
    }

    if (!m_assignmentQueue.isEmpty()) {
        const auto next = m_assignmentQueue.dequeue();
        activateAssignment(next);
    } else {
        refreshAssignmentQueue();
    }
    persistAssignmentQueue();
}

void NetworkTransferPanel::saveTransferReport(bool success) {
    TransferReport report;
    report.transfer_id = m_currentManifest.transfer_id;
    report.source_host = m_currentManifest.source_hostname;
    report.destination_host = QHostInfo::localHostName();
    report.status = success ? "success" : "failed";
    report.started_at = m_transferStarted;
    report.completed_at = QDateTime::currentDateTime();
    report.total_bytes = m_currentManifest.total_bytes;
    report.total_files = m_currentManifest.total_files;
    report.errors = m_transferErrors;
    report.manifest = m_currentManifest;

    QString reportDir;
    if (m_isSourceTransfer) {
        reportDir = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation) +
                    "/SAK/TransferReports";
    } else {
        reportDir = destinationBase() + "/TransferReports";
    }

    QDir dir(reportDir);
    if (!dir.exists()) {
        if (!dir.mkpath(".")) {
            sak::logWarning("Failed to create transfer report directory: {}",
                            reportDir.toStdString());
        }
    }
    const QString reportPath =
        dir.filePath(QString("transfer_%1_%2.json")
                         .arg(m_currentManifest.transfer_id)
                         .arg(QDateTime::currentDateTime().toString("yyyyMMdd_HHmmss")));
    if (!report.saveToFile(reportPath)) {
        Q_EMIT logOutput(tr("Failed to save transfer report."));
    } else {
        Q_EMIT logOutput(tr("Transfer report saved to %1").arg(reportPath));
    }
}

void NetworkTransferPanel::writeAdditionalDataFiles(const QString& basePath,
                                                    const TransferManifest& manifest) {
    QDir dir(basePath);
    if (!dir.exists()) {
        if (!dir.mkpath(".")) {
            sak::logWarning("Failed to create transfer data directory: {}", basePath.toStdString());
        }
    }

    auto writeJson = [&](const QString& filename, const QJsonArray& array) {
        if (array.isEmpty()) {
            return;
        }
        QFile file(dir.filePath(filename));
        if (file.open(QIODevice::WriteOnly)) {
            const QByteArray data = QJsonDocument(array).toJson(QJsonDocument::Indented);
            if (file.write(data) != data.size()) {
                sak::logError("Incomplete write of transfer data file: {}", filename.toStdString());
            }
            Q_EMIT logOutput(tr("Saved %1").arg(filename));
        } else {
            sak::logError("Failed to write transfer data file: {}", filename.toStdString());
        }
    };

    QJsonArray appsArr;
    for (const auto& app : manifest.installed_apps) {
        appsArr.append(app.toJson());
    }
    writeJson("installed_apps.json", appsArr);

    QJsonArray appDataArr;
    for (const auto& src : manifest.app_data_sources) {
        appDataArr.append(src.toJson());
    }
    writeJson("app_data_sources.json", appDataArr);

    QJsonArray wifiArr;
    for (const auto& prof : manifest.wifi_profiles) {
        wifiArr.append(prof.toJson());
    }
    writeJson("wifi_profiles.json", wifiArr);

    QJsonArray ethArr;
    for (const auto& cfg : manifest.ethernet_configs) {
        ethArr.append(cfg.toJson());
    }
    writeJson("ethernet_configs.json", ethArr);
}

void NetworkTransferPanel::startPostTransferRestore() {
    Q_ASSERT(m_restoreWorker);
    Q_ASSERT(m_userScanner);
    if (m_restoreWorker && m_restoreWorker->isRunning()) {
        Q_EMIT logOutput(tr("Restore already running."));
        return;
    }

    // Save manifest to staging directory for restore worker validation
    BackupManifest backupManifest;
    backupManifest.version = "1.0";
    backupManifest.created = QDateTime::currentDateTime();
    backupManifest.source_machine = m_currentManifest.source_hostname;
    backupManifest.sak_version = m_currentManifest.sak_version;
    backupManifest.users = m_currentManifest.users;
    backupManifest.total_backup_size_bytes = m_currentManifest.total_bytes;

    // Pass additional data through to restore
    backupManifest.wifi_profiles = m_currentManifest.wifi_profiles;
    backupManifest.ethernet_configs = m_currentManifest.ethernet_configs;
    backupManifest.app_data_sources = m_currentManifest.app_data_sources;

    const QString manifestPath = destinationBase() + "/manifest.json";
    backupManifest.saveToFile(manifestPath);

    // Build user mappings
    QVector<UserProfile> destUsers = m_userScanner->scanUsers();
    QSet<QString> existing;
    for (const auto& user : destUsers) {
        existing.insert(user.username.toLower());
    }

    QVector<UserMapping> mappings;
    for (const auto& user : backupManifest.users) {
        UserMapping mapping;
        mapping.source_username = user.username;
        mapping.source_sid = user.sid;
        if (existing.contains(user.username.toLower())) {
            mapping.destination_username = user.username;
            mapping.mode = MergeMode::MergeIntoDestination;
        } else {
            mapping.destination_username = user.username;
            mapping.mode = MergeMode::CreateNewUser;
        }
        mapping.conflict_resolution = ConflictResolution::RenameWithSuffix;
        mappings.append(mapping);
    }

    if (!m_restoreWorker) {
        m_restoreWorker = new UserProfileRestoreWorker(this);
        connect(m_restoreWorker,
                &UserProfileRestoreWorker::logMessage,
                this,
                [this](const QString& msg, bool warn) {
                    Q_EMIT logOutput(warn ? tr("RESTORE WARN: %1").arg(msg) : msg);
                });
        connect(m_restoreWorker,
                &UserProfileRestoreWorker::restoreComplete,
                this,
                [this](bool ok, const QString& msg) {
                    Q_EMIT logOutput(ok ? msg : tr("Restore failed: %1").arg(msg));
                });
    }

    Q_EMIT logOutput(tr("Starting profile restore into system profiles..."));
    m_restoreWorker->startRestore(destinationBase(),
                                  backupManifest,
                                  mappings,
                                  ConflictResolution::RenameWithSuffix,
                                  PermissionMode::StripAll,
                                  true);
}

void NetworkTransferPanel::buildManifest() {
    m_currentFiles = buildFileList();
    m_currentManifest = buildManifestPayload(m_currentFiles);
    Q_EMIT logOutput(tr("Manifest built: %1 files (%2)")
                         .arg(m_currentManifest.total_files)
                         .arg(formatBytes(m_currentManifest.total_bytes)));
}

QVector<TransferFileEntry> NetworkTransferPanel::buildFileList() {
    Q_ASSERT(m_userTable);
    QVector<TransferFileEntry> files;

    for (int i = 0; i < m_users.size(); ++i) {
        auto* selectItem = m_userTable->item(i, kUserColSelect);
        if (!selectItem || selectItem->checkState() != Qt::Checked) {
            continue;
        }
        collectUserFiles(files, m_users[i]);
    }

    return files;
}

void NetworkTransferPanel::processScannedFile(QVector<TransferFileEntry>& files,
                                              const std::filesystem::path& fsPath,
                                              const QString& username,
                                              const QString& profilePath,
                                              SmartFileFilter& smartFilter,
                                              PermissionManager& permissionManager,
                                              file_hasher& hasher,
                                              PermissionMode permMode) {
    if (!std::filesystem::is_regular_file(fsPath)) {
        return;
    }

    QFileInfo fileInfo(QString::fromStdString(fsPath.string()));
    if (smartFilter.shouldExcludeFile(fileInfo, profilePath) ||
        smartFilter.exceedsSizeLimit(fileInfo.size())) {
        return;
    }

    TransferFileEntry entry;
    entry.file_id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    entry.absolute_path = QString::fromStdString(fsPath.string());
    auto rel = path_utils::makeRelative(fsPath, std::filesystem::path(profilePath.toStdString()));
    if (!rel) {
        return;
    }
    QString relative = QString::fromStdString(rel->generic_string());
    entry.relative_path = username + "/" + relative;
    entry.size_bytes = static_cast<qint64>(std::filesystem::file_size(fsPath));
    if (permMode == PermissionMode::PreserveOriginal) {
        entry.acl_sddl = permissionManager.getSecurityDescriptorSddl(entry.absolute_path);
    }
    auto hashResult = hasher.calculateHash(fsPath);
    if (hashResult) {
        entry.checksum_sha256 = QString::fromStdString(*hashResult);
    }
    files.append(entry);
}

void NetworkTransferPanel::collectUserFiles(QVector<TransferFileEntry>& files,
                                            const UserProfile& user) {
    file_hasher hasher(hash_algorithm::sha256);
    SmartFileFilter smartFilter{SmartFilter{}};
    PermissionManager permissionManager;
    const auto selectedPermMode =
        static_cast<PermissionMode>(m_permissionModeCombo->currentData().toInt());

    for (const auto& folder : user.folder_selections) {
        if (!folder.selected) {
            continue;
        }

        QString folderPath = QDir(user.profile_path).filePath(folder.relative_path);
        file_scanner scanner;
        scan_options options;
        options.recursive = true;
        options.type_filter = file_type_filter::files_only;

        for (const auto& include : folder.include_patterns) {
            options.include_patterns.push_back(include.toStdString());
        }
        for (const auto& exclude : folder.exclude_patterns) {
            options.exclude_patterns.push_back(exclude.toStdString());
        }

        auto result = scanner.scanAndCollect(folderPath.toStdString(), options);
        if (!result) {
            continue;
        }

        for (const auto& path : *result) {
            processScannedFile(files,
                               path,
                               user.username,
                               user.profile_path,
                               smartFilter,
                               permissionManager,
                               hasher,
                               selectedPermMode);
        }
    }
}

QVector<TransferFileEntry> NetworkTransferPanel::buildFileListForUsers(
    const QVector<UserProfile>& users) {
    QVector<TransferFileEntry> files;
    for (const auto& user : users) {
        collectUserFiles(files, user);
    }
    return files;
}

TransferManifest NetworkTransferPanel::buildManifestPayload(
    const QVector<TransferFileEntry>& files) {
    TransferManifest manifest;
    manifest.transfer_id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    manifest.source_hostname = QHostInfo::localHostName();
    manifest.source_os = "Windows";
    manifest.sak_version = sak::get_version_short();
    manifest.created = QDateTime::currentDateTime();
    manifest.files = files;

    const auto selectedPermMode =
        static_cast<PermissionMode>(m_permissionModeCombo->currentData().toInt());

    for (int i = 0; i < m_users.size(); ++i) {
        auto& user = m_users[i];
        auto* selectItem = m_userTable->item(i, kUserColSelect);
        if (!selectItem || selectItem->checkState() != Qt::Checked) {
            continue;
        }
        BackupUserData userData;
        userData.username = user.username;
        userData.sid = user.sid;
        userData.profile_path = user.profile_path;
        userData.backed_up_folders = user.folder_selections;
        userData.permissions_mode = selectedPermMode;
        manifest.users.append(userData);
    }

    // Include additional scanned data
    manifest.installed_apps = m_scannedApps;
    manifest.app_data_sources = m_scannedAppData;
    manifest.wifi_profiles = m_scannedWifi;
    manifest.ethernet_configs = m_scannedEthernet;

    qint64 totalBytes = 0;
    for (const auto& file : files) {
        totalBytes += file.size_bytes;
    }
    manifest.total_bytes = totalBytes;
    manifest.total_files = files.size();

    QJsonDocument doc(manifest.toJson());
    QByteArray hash = QCryptographicHash::hash(doc.toJson(QJsonDocument::Compact),
                                               QCryptographicHash::Sha256);
    manifest.checksum_sha256 = hash.toHex();

    return manifest;
}

TransferManifest NetworkTransferPanel::buildManifestPayloadForUsers(
    const QVector<TransferFileEntry>& files, const QVector<UserProfile>& users) {
    TransferManifest manifest;
    manifest.transfer_id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    manifest.source_hostname = QHostInfo::localHostName();
    manifest.source_os = "Windows";
    manifest.sak_version = sak::get_version_short();
    manifest.created = QDateTime::currentDateTime();
    manifest.files = files;

    const auto selectedPermMode =
        static_cast<PermissionMode>(m_permissionModeCombo->currentData().toInt());

    for (const auto& user : users) {
        BackupUserData userData;
        userData.username = user.username;
        userData.sid = user.sid;
        userData.profile_path = user.profile_path;
        userData.backed_up_folders = user.folder_selections;
        userData.permissions_mode = selectedPermMode;
        manifest.users.append(userData);
    }

    qint64 totalBytes = 0;
    for (const auto& file : files) {
        totalBytes += file.size_bytes;
    }
    manifest.total_bytes = totalBytes;
    manifest.total_files = files.size();

    QJsonDocument doc(manifest.toJson());
    QByteArray hash = QCryptographicHash::hash(doc.toJson(QJsonDocument::Compact),
                                               QCryptographicHash::Sha256);
    manifest.checksum_sha256 = hash.toHex();

    return manifest;
}

void NetworkTransferPanel::onConnectionStateChanged(bool connected) {
    Q_ASSERT(m_autoApproveOrchestratorCheck);
    Q_ASSERT(m_approveButton);
    if (!connected) {
        return;
    }

    if (m_orchestrationAssignmentPending && m_manifestValidated && m_autoApproveOrchestratorCheck &&
        m_autoApproveOrchestratorCheck->isChecked() && m_approveButton &&
        m_approveButton->isEnabled()) {
        m_orchestrationAssignmentPending = false;
        onApproveTransfer();
    }
}

void NetworkTransferPanel::activateAssignment(const DeploymentAssignment& assignment) {
    Q_ASSERT(m_activeAssignmentLabel);
    Q_ASSERT(m_assignmentBandwidthLabel);
    m_activeAssignment = assignment;
    if (m_activeAssignmentLabel) {
        m_activeAssignmentLabel->setText(
            tr("Active: %1 (%2)").arg(assignment.source_user, assignment.deployment_id));
    }

    if (m_assignmentBandwidthLabel) {
        if (assignment.max_bandwidth_kbps > 0) {
            m_assignmentBandwidthLabel->setText(
                tr("Bandwidth limit: %1 KB/s").arg(assignment.max_bandwidth_kbps));
            m_settings.max_bandwidth_kbps = assignment.max_bandwidth_kbps;
        } else {
            m_assignmentBandwidthLabel->setText(tr("Bandwidth limit: default"));
        }
    }

    refreshAssignmentQueue();
    refreshAssignmentStatus();
    m_orchestrationAssignmentPending = true;
    m_manifestValidated = false;

    if (!assignment.job_id.isEmpty()) {
        m_assignmentStatusByJob[assignment.job_id] = tr("active");
        m_assignmentEventByJob[assignment.job_id] = tr("Activated");
        refreshAssignmentStatus();
        persistAssignmentQueue();
    }

    if (m_modeCombo && m_modeCombo->currentIndex() != 1) {
        m_modeCombo->setCurrentIndex(1);
    }

    if (m_controller && m_controller->mode() != NetworkTransferController::Mode::Destination) {
        const bool hasDestinationBase = !destinationBase().isEmpty();
        const bool hasPassphrase = !m_settings.encryption_enabled ||
                                   !m_destinationPassphraseEdit->text().isEmpty();
        if (hasDestinationBase && hasPassphrase) {
            onStartDestination();
        } else {
            Q_EMIT logOutput(
                tr("Assignment received. Set destination base/passphrase to begin "
                   "listening."));
        }
    }
}

}  // namespace sak
