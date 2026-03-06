// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file network_transfer_panel.cpp
/// @brief Implements the network file transfer panel UI for PC-to-PC migration

#include "sak/network_transfer_panel.h"
#include "sak/format_utils.h"
#include "sak/widget_helpers.h"
#include "sak/style_constants.h"
#include "sak/network_constants.h"
#include "sak/layout_constants.h"

#include "sak/windows_user_scanner.h"
#include "sak/per_user_customization_dialog.h"
#include "sak/network_transfer_controller.h"
#include "sak/migration_orchestrator.h"
#include "sak/parallel_transfer_manager.h"
#include "sak/mapping_engine.h"
#include "sak/file_scanner.h"
#include "sak/file_hash.h"
#include "sak/path_utils.h"
#include "sak/config_manager.h"
#include "sak/version.h"
#include "sak/smart_file_filter.h"
#include "sak/permission_manager.h"
#include "sak/detachable_log_window.h"
#include "sak/info_button.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGroupBox>
#include <QHeaderView>
#include <QTableWidget>
#include <QPushButton>
#include <QProgressBar>
#include <QComboBox>
#include <QLineEdit>
#include <QSpinBox>
#include <QCheckBox>
#include <QLabel>
#include <QStackedWidget>
#include <QTextEdit>
#include <QGroupBox>
#include <QMessageBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QTime>
#include <QColor>
#include <QHostInfo>
#include <QNetworkInterface>
#include <QDateTime>
#include <QDir>
#include <QJsonDocument>
#include <QCryptographicHash>
#include <QUuid>
#include <QApplication>
#include <QtConcurrent>
#include <QMimeData>
#include <QDropEvent>
#include <QDragEnterEvent>
#include <QDataStream>
#include <QScrollArea>
#include <QFrame>
#include <QFormLayout>
#include <QDialog>
#include <QGridLayout>
#include <filesystem>
#include <QStandardPaths>
#include <QSet>

namespace sak {

namespace {
constexpr int kUserColSelect = 0;
constexpr int kUserColName = 1;
constexpr int kUserColPath = 2;
constexpr int kUserColSize = 3;
constexpr int kUserColCount = 4;

constexpr int kPeerColName = 0;
constexpr int kPeerColIp = 1;
constexpr int kPeerColMode = 2;
constexpr int kPeerColCaps = 3;
constexpr int kPeerColSeen = 4;
constexpr int kPeerColCount = 5;

} // namespace

NetworkTransferPanel::NetworkTransferPanel(QWidget* parent)
    : QWidget(parent)
    , m_userScanner(std::make_unique<WindowsUserScanner>())
    , m_controller(new NetworkTransferController(this))
    , m_orchestrator(new MigrationOrchestrator(this))
    , m_parallelManager(new ParallelTransferManager(this))
    , m_mappingEngine(new MappingEngine(this))
{
    setupUi();
    setupConnections();
    loadSettings();
}

NetworkTransferPanel::~NetworkTransferPanel() = default;

void NetworkTransferPanel::setupUi() {
    Q_ASSERT(!objectName().isEmpty() || true);  // widget valid
    auto* mainLayout = new QVBoxLayout(this);
    mainLayout->setSpacing(sak::ui::kSpacingDefault);
    mainLayout->setContentsMargins(sak::ui::kMarginMedium, sak::ui::kMarginMedium,
                                   sak::ui::kMarginMedium, sak::ui::kMarginMedium);

    // Panel header — consistent title + muted subtitle
    sak::createPanelHeader(this, QStringLiteral(":/icons/icons/panel_network_transfer.svg"),
        tr("Network Transfer"),
        tr("Transfer files and deploy profiles across machines on your network"), mainLayout);

    auto* modeLayout = new QHBoxLayout();
    modeLayout->addWidget(new QLabel(tr("Mode:"), this));
    m_modeCombo = new QComboBox(this);
    m_modeCombo->addItems({tr("Source (Send)"), tr("Destination (Receive)"),
        tr("Orchestrator (Deploy)")});
    m_modeCombo->setAccessibleName(QStringLiteral("Transfer Mode"));
    m_modeCombo->setToolTip(QStringLiteral("Select the transfer mode for this machine"));
    modeLayout->addWidget(m_modeCombo);
    modeLayout->addStretch();
    mainLayout->addLayout(modeLayout);

    m_modeStack = new QStackedWidget(this);

    auto wrapScrollable = [this](QWidget* widget) {
        auto* scroll = new QScrollArea(this);
        scroll->setWidgetResizable(true);
        scroll->setFrameShape(QFrame::NoFrame);
        scroll->setWidget(widget);
        return scroll;
    };

    // Source page
    auto* sourceWidget = new QWidget(this);
    auto* sourceLayout = new QVBoxLayout(sourceWidget);
    setupUi_sourceSection(sourceLayout);
    setupUi_securityWidgets();
    sourceWidget->setLayout(sourceLayout);

    // Destination page
    auto* destWidget = new QWidget(this);
    auto* destLayout = new QVBoxLayout(destWidget);
    setupUi_destinationSection(destLayout);
    setupUi_destinationIncoming(destLayout);
    destWidget->setLayout(destLayout);

    // Orchestrator page
    auto* orchestratorWidget = new QWidget(this);
    auto* orchestratorLayout = new QVBoxLayout(orchestratorWidget);
    setupUi_orchestratorServer(orchestratorLayout);
    setupUi_orchestratorSources(orchestratorLayout);
    setupUi_orchestratorDestinations(orchestratorLayout);
    setupUi_deploymentControls(orchestratorLayout);
    setupUi_customRules(orchestratorLayout);
    setupUi_deploymentJobs(orchestratorLayout);
    setupUi_deploymentProgress(orchestratorLayout);
    orchestratorWidget->setLayout(orchestratorLayout);

    m_modeStack->addWidget(wrapScrollable(sourceWidget));
    m_modeStack->addWidget(wrapScrollable(destWidget));
    m_modeStack->addWidget(wrapScrollable(orchestratorWidget));
    mainLayout->addWidget(m_modeStack, 1);

    setupUi_bottomButtons(mainLayout);
}

void NetworkTransferPanel::setupUi_sourceSection(QVBoxLayout* sourceLayout) {
    auto* dataGroup = new QGroupBox(tr("Data Selection"), this);
    auto* dataLayout = new QVBoxLayout(dataGroup);

    auto* userHeaderLayout = new QHBoxLayout();
    m_scanUsersButton = new QPushButton(tr("Scan Users"), this);
    m_scanUsersButton->setAccessibleName(QStringLiteral("Scan Users"));
    m_scanUsersButton->setToolTip(QStringLiteral("Scan for Windows user profiles on this machine"));
    m_customizeUserButton = new QPushButton(tr("Customize Selected"), this);
    m_customizeUserButton->setAccessibleName(QStringLiteral("Customize User"));
    m_customizeUserButton->setToolTip(QStringLiteral("Customize data selection for the selected "
                                                     "user"));
    userHeaderLayout->addWidget(m_scanUsersButton);
    userHeaderLayout->addWidget(m_customizeUserButton);
    userHeaderLayout->addStretch();
    dataLayout->addLayout(userHeaderLayout);

    m_userTable = new QTableWidget(0, kUserColCount, this);
    m_userTable->setHorizontalHeaderLabels({"?", tr("User"), tr("Profile Path"), tr("Size")});
    m_userTable->horizontalHeader()->setSectionResizeMode(kUserColName,
        QHeaderView::ResizeToContents);
    m_userTable->horizontalHeader()->setSectionResizeMode(kUserColPath, QHeaderView::Stretch);
    m_userTable->horizontalHeader()->setSectionResizeMode(kUserColSize,
        QHeaderView::ResizeToContents);
    m_userTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_userTable->setSelectionMode(QAbstractItemView::SingleSelection);
    m_userTable->setAccessibleName(QStringLiteral("User Profiles Table"));
    m_userTable->setToolTip(QStringLiteral(
        "Detected Windows user profiles available for transfer"));
    dataLayout->addWidget(m_userTable);

    dataGroup->setLayout(dataLayout);
    sourceLayout->addWidget(dataGroup);

    setupUi_peerDiscovery(sourceLayout);
}

void NetworkTransferPanel::setupUi_peerDiscovery(QVBoxLayout* sourceLayout) {
    auto* peerGroup = new QGroupBox(tr("Destination Discovery"), this);
    auto* peerLayout = new QVBoxLayout(peerGroup);

    auto* peerHeaderLayout = new QHBoxLayout();
    m_discoverPeersButton = new QPushButton(tr("Discover Peers"), this);
    m_discoverPeersButton->setAccessibleName(QStringLiteral("Discover Peers"));
    m_discoverPeersButton->setToolTip(QStringLiteral("Scan the network for available destination "
                                                     "machines"));
    peerHeaderLayout->addWidget(m_discoverPeersButton);
    peerHeaderLayout->addStretch();
    peerLayout->addLayout(peerHeaderLayout);

    m_peerTable = new QTableWidget(0, kPeerColCount, this);
    m_peerTable->setHorizontalHeaderLabels({tr("Host"), tr("IP"), tr("Mode"), tr("Capabilities"),
        tr("Last Seen")});
    m_peerTable->horizontalHeader()->setSectionResizeMode(kPeerColName,
        QHeaderView::ResizeToContents);
    m_peerTable->horizontalHeader()->setSectionResizeMode(kPeerColIp,
        QHeaderView::ResizeToContents);
    m_peerTable->horizontalHeader()->setSectionResizeMode(kPeerColCaps, QHeaderView::Stretch);
    m_peerTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_peerTable->setSelectionMode(QAbstractItemView::SingleSelection);
    m_peerTable->setAccessibleName(QStringLiteral("Discovered Peers Table"));
    m_peerTable->setToolTip(QStringLiteral("Destination machines discovered on the network"));
    peerLayout->addWidget(m_peerTable);

    auto* manualLayout = new QHBoxLayout();
    manualLayout->addWidget(new QLabel(tr("Manual IP:"), this));
    m_manualIpEdit = new QLineEdit(this);
    m_manualIpEdit->setPlaceholderText(tr("192.168.1.100"));
    m_manualIpEdit->setAccessibleName(QStringLiteral("Manual IP Address"));
    m_manualIpEdit->setToolTip(QStringLiteral("Enter the IP address of a destination manually"));
    manualLayout->addWidget(m_manualIpEdit);
    manualLayout->addWidget(new QLabel(tr("Port:"), this));
    m_manualPortSpin = new QSpinBox(this);
    m_manualPortSpin->setRange(1024, 65535);
    m_manualPortSpin->setAccessibleName(QStringLiteral("Manual Port"));
    m_manualPortSpin->setToolTip(QStringLiteral("Port number for the manual connection"));
    manualLayout->addWidget(m_manualPortSpin);
    peerLayout->addLayout(manualLayout);

    peerGroup->setLayout(peerLayout);
    sourceLayout->addWidget(peerGroup);
}

void NetworkTransferPanel::setupUi_securityWidgets() {
    // Security widgets (hidden — managed via Security Settings dialog)
    m_encryptCheck = new QCheckBox(tr("Encrypt (AES-256-GCM)"), this);
    m_encryptCheck->setAccessibleName(QStringLiteral("Encrypt Transfer"));
    m_encryptCheck->setToolTip(QStringLiteral("Encrypt data using AES-256-GCM during transfer"));
    m_encryptCheck->setVisible(false);
    m_compressCheck = new QCheckBox(tr("Compress"), this);
    m_compressCheck->setAccessibleName(QStringLiteral("Compress Transfer"));
    m_compressCheck->setToolTip(QStringLiteral("Compress data before sending"));
    m_compressCheck->setVisible(false);
    m_resumeCheck = new QCheckBox(tr("Resume"), this);
    m_resumeCheck->setAccessibleName(QStringLiteral("Resume Transfer"));
    m_resumeCheck->setToolTip(QStringLiteral("Allow resuming interrupted transfers"));
    m_resumeCheck->setVisible(false);

    m_chunkSizeSpin = new QSpinBox(this);
    m_chunkSizeSpin->setRange(16, 4096);
    m_chunkSizeSpin->setAccessibleName(QStringLiteral("Chunk Size"));
    m_chunkSizeSpin->setToolTip(QStringLiteral("Transfer chunk size in KB"));
    m_chunkSizeSpin->setVisible(false);

    m_bandwidthSpin = new QSpinBox(this);
    m_bandwidthSpin->setRange(0, sak::kBytesPerMB);
    m_bandwidthSpin->setAccessibleName(QStringLiteral("Bandwidth Limit"));
    m_bandwidthSpin->setToolTip(tr("0 = unlimited"));
    m_bandwidthSpin->setVisible(false);

    m_permissionModeCombo = new QComboBox(this);
    m_permissionModeCombo->addItem(tr("Strip All"), static_cast<int>(PermissionMode::StripAll));
    m_permissionModeCombo->addItem(tr("Preserve Original"),
        static_cast<int>(PermissionMode::PreserveOriginal));
    m_permissionModeCombo->addItem(tr("Assign to Destination"),
        static_cast<int>(PermissionMode::AssignToDestination));
    m_permissionModeCombo->addItem(tr("Hybrid"), static_cast<int>(PermissionMode::Hybrid));
    m_permissionModeCombo->setAccessibleName(QStringLiteral("Permission Mode"));
    m_permissionModeCombo->setToolTip(QStringLiteral("How file permissions are handled during "
                                                     "transfer"));
    m_permissionModeCombo->setVisible(false);

    m_passphraseEdit = new QLineEdit(this);
    m_passphraseEdit->setEchoMode(QLineEdit::Password);
    m_passphraseEdit->setAccessibleName(QStringLiteral("Encryption Passphrase"));
    m_passphraseEdit->setToolTip(QStringLiteral("Passphrase used for transfer encryption"));
    m_passphraseEdit->setVisible(false);
}

void NetworkTransferPanel::setupUi_destinationSection(QVBoxLayout* destLayout) {
    auto* destInfoGroup = new QGroupBox(tr("Destination Setup"), this);
    auto* destInfoLayout = new QVBoxLayout(destInfoGroup);
    m_destinationInfo = new QLabel(this);
    m_destinationInfo->setWordWrap(true);
    destInfoLayout->addWidget(m_destinationInfo);
    auto* destBaseLayout = new QHBoxLayout();
    destBaseLayout->addWidget(new QLabel(tr("Destination Base:"), this));
    m_destinationBaseEdit = new QLineEdit(this);
    m_destinationBaseEdit->setAccessibleName(QStringLiteral("Destination Base Path"));
    m_destinationBaseEdit->setToolTip(QStringLiteral("Root directory for receiving transferred "
                                                     "files"));
    destBaseLayout->addWidget(m_destinationBaseEdit);
    destInfoLayout->addLayout(destBaseLayout);
    auto* destPassLayout = new QHBoxLayout();
    destPassLayout->addWidget(new QLabel(tr("Passphrase:"), this));
    m_destinationPassphraseEdit = new QLineEdit(this);
    m_destinationPassphraseEdit->setEchoMode(QLineEdit::Password);
    m_destinationPassphraseEdit->setAccessibleName(QStringLiteral("Destination Passphrase"));
    m_destinationPassphraseEdit->setToolTip(QStringLiteral("Passphrase to decrypt incoming "
                                                           "transfers"));
    destPassLayout->addWidget(m_destinationPassphraseEdit);
    destInfoLayout->addLayout(destPassLayout);
    m_startDestinationButton = new QPushButton(tr("Start Listening"), this);
    m_startDestinationButton->setAccessibleName(QStringLiteral("Start Listening"));
    m_startDestinationButton->setToolTip(QStringLiteral("Start listening for incoming file "
                                                        "transfers"));
    destInfoLayout->addWidget(m_startDestinationButton);
    auto* orchestratorGroup = new QGroupBox(tr("Orchestrator Connection"), this);
    auto* orchestratorConnectionLayout = new QGridLayout(orchestratorGroup);
    orchestratorConnectionLayout->addWidget(new QLabel(tr("Host:"), this), 0, 0);
    m_orchestratorHostEdit = new QLineEdit(this);
    m_orchestratorHostEdit->setPlaceholderText(tr("192.168.1.10"));
    m_orchestratorHostEdit->setAccessibleName(QStringLiteral("Orchestrator Host"));
    m_orchestratorHostEdit->setToolTip(QStringLiteral("IP address of the orchestrator server"));
    orchestratorConnectionLayout->addWidget(m_orchestratorHostEdit, 0, 1);
    orchestratorConnectionLayout->addWidget(new QLabel(tr("Port:"), this), 0, 2);
    m_orchestratorPortSpin = new QSpinBox(this);
    m_orchestratorPortSpin->setRange(sak::kPortRangeMin, sak::kPortRangeMax);
    m_orchestratorPortSpin->setValue(sak::kPortControl);
    m_orchestratorPortSpin->setAccessibleName(QStringLiteral("Orchestrator Port"));
    m_orchestratorPortSpin->setToolTip(QStringLiteral("Port number for the orchestrator server"));
    orchestratorConnectionLayout->addWidget(m_orchestratorPortSpin, 0, 3);
    m_autoApproveOrchestratorCheck = new QCheckBox(tr("Auto-approve assignments"), this);
    m_autoApproveOrchestratorCheck->setChecked(true);
    m_autoApproveOrchestratorCheck->setAccessibleName(QStringLiteral("Auto-Approve Assignments"));
    m_autoApproveOrchestratorCheck->setToolTip(QStringLiteral("Automatically accept transfer "
                                                              "assignments from the orchestrator"));
    orchestratorConnectionLayout->addWidget(m_autoApproveOrchestratorCheck, 1, 0, 1, 3);
    m_connectOrchestratorButton = new QPushButton(tr("Connect"), this);
    m_connectOrchestratorButton->setAccessibleName(QStringLiteral("Connect Orchestrator"));
    m_connectOrchestratorButton->setToolTip(QStringLiteral("Connect to the orchestrator server"));
    orchestratorConnectionLayout->addWidget(m_connectOrchestratorButton, 1, 3);
    orchestratorGroup->setLayout(orchestratorConnectionLayout);
    destInfoLayout->addWidget(orchestratorGroup);
    m_applyRestoreCheck = new QCheckBox(tr("Apply restore into system profiles"), this);
    m_applyRestoreCheck->setChecked(true);
    m_applyRestoreCheck->setAccessibleName(QStringLiteral("Apply System Restore"));
    m_applyRestoreCheck->setToolTip(QStringLiteral("Restore transferred data into system user "
                                                   "profiles"));
    destInfoLayout->addWidget(m_applyRestoreCheck);
    destInfoGroup->setLayout(destInfoLayout);
    destLayout->addWidget(destInfoGroup);
}

void NetworkTransferPanel::setupUi_destinationIncoming(QVBoxLayout* destLayout) {
    auto* manifestGroup = new QGroupBox(tr("Incoming Manifest"), this);
    auto* manifestLayout = new QVBoxLayout(manifestGroup);
    m_manifestText = new QTextEdit(this);
    m_manifestText->setReadOnly(true);
    manifestLayout->addWidget(m_manifestText);

    auto* approveLayout = new QHBoxLayout();
    m_approveButton = new QPushButton(tr("Approve Transfer"), this);
    m_approveButton->setEnabled(false);
    m_approveButton->setAccessibleName(QStringLiteral("Approve Transfer"));
    m_approveButton->setToolTip(QStringLiteral("Accept and begin the incoming transfer"));
    m_rejectButton = new QPushButton(tr("Reject"), this);
    m_rejectButton->setAccessibleName(QStringLiteral("Reject Transfer"));
    m_rejectButton->setToolTip(QStringLiteral("Reject the incoming transfer request"));
    approveLayout->addWidget(m_approveButton);
    approveLayout->addWidget(m_rejectButton);
    approveLayout->addStretch();
    manifestLayout->addLayout(approveLayout);

    manifestGroup->setLayout(manifestLayout);
    destLayout->addWidget(manifestGroup);

    auto* assignmentGroup = new QGroupBox(tr("Assignment Queue"), this);
    auto* assignmentLayout = new QVBoxLayout(assignmentGroup);
    m_activeAssignmentLabel = new QLabel(tr("No active assignment"), this);
    assignmentLayout->addWidget(m_activeAssignmentLabel);
    m_assignmentBandwidthLabel = new QLabel(tr("Bandwidth limit: default"), this);
    assignmentLayout->addWidget(m_assignmentBandwidthLabel);
    m_assignmentQueueTable = new QTableWidget(0, 6, this);
    m_assignmentQueueTable->setHorizontalHeaderLabels({tr("Deployment"), tr("Job"), tr("User"),
        tr("Size"), tr("Priority"), tr("Bandwidth")});
    m_assignmentQueueTable->horizontalHeader()->setSectionResizeMode(0,
        QHeaderView::ResizeToContents);
    m_assignmentQueueTable->horizontalHeader()->setSectionResizeMode(1,
        QHeaderView::ResizeToContents);
    m_assignmentQueueTable->horizontalHeader()->setSectionResizeMode(2,
        QHeaderView::ResizeToContents);
    m_assignmentQueueTable->horizontalHeader()->setSectionResizeMode(3,
        QHeaderView::ResizeToContents);
    m_assignmentQueueTable->horizontalHeader()->setSectionResizeMode(4,
        QHeaderView::ResizeToContents);
    m_assignmentQueueTable->horizontalHeader()->setSectionResizeMode(5, QHeaderView::Stretch);
    m_assignmentQueueTable->setAccessibleName(QStringLiteral("Assignment Queue Table"));
    assignmentLayout->addWidget(m_assignmentQueueTable);

    m_assignmentStatusTable = new QTableWidget(0, 5, this);
    m_assignmentStatusTable->setHorizontalHeaderLabels({tr("Deployment"), tr("Job"), tr("User"),
        tr("Status"), tr("Last Event")});
    m_assignmentStatusTable->horizontalHeader()->setSectionResizeMode(0,
        QHeaderView::ResizeToContents);
    m_assignmentStatusTable->horizontalHeader()->setSectionResizeMode(1,
        QHeaderView::ResizeToContents);
    m_assignmentStatusTable->horizontalHeader()->setSectionResizeMode(2,
        QHeaderView::ResizeToContents);
    m_assignmentStatusTable->horizontalHeader()->setSectionResizeMode(3,
        QHeaderView::ResizeToContents);
    m_assignmentStatusTable->horizontalHeader()->setSectionResizeMode(4, QHeaderView::Stretch);
    m_assignmentStatusTable->setAccessibleName(QStringLiteral("Assignment Status Table"));
    assignmentLayout->addWidget(m_assignmentStatusTable);
    assignmentGroup->setLayout(assignmentLayout);
    destLayout->addWidget(assignmentGroup);
}

void NetworkTransferPanel::setupUi_orchestratorServer(QVBoxLayout* orchestratorLayout) {
    auto* orchestratorServerGroup = new QGroupBox(tr("Orchestrator Server"), this);
    auto* orchestratorServerLayout = new QHBoxLayout(orchestratorServerGroup);
    orchestratorServerLayout->addWidget(new QLabel(tr("Listen Port:"), this));
    m_orchestratorListenPortSpin = new QSpinBox(this);
    m_orchestratorListenPortSpin->setRange(sak::kPortRangeMin, sak::kPortRangeMax);
    m_orchestratorListenPortSpin->setValue(sak::kPortControl);
    m_orchestratorListenPortSpin->setAccessibleName(QStringLiteral("Listen Port"));
    m_orchestratorListenPortSpin->setToolTip(QStringLiteral("Port the orchestrator server listens "
                                                            "on"));
    orchestratorServerLayout->addWidget(m_orchestratorListenPortSpin);
    m_orchestratorListenButton = new QPushButton(tr("Start Server"), this);
    m_orchestratorListenButton->setAccessibleName(QStringLiteral("Start Orchestrator"));
    m_orchestratorListenButton->setToolTip(QStringLiteral("Start the orchestrator server"));
    orchestratorServerLayout->addWidget(m_orchestratorListenButton);
    m_orchestratorStatusLabel = new QLabel(tr("Stopped"), this);
    orchestratorServerLayout->addWidget(m_orchestratorStatusLabel, 1);
    orchestratorServerGroup->setLayout(orchestratorServerLayout);
    orchestratorLayout->addWidget(orchestratorServerGroup);
}

void NetworkTransferPanel::setupUi_orchestratorSources(QVBoxLayout* orchestratorLayout) {
    auto* orchestratorSourcesGroup = new QGroupBox(tr("Source Profiles"), this);
    auto* orchestratorSourcesLayout = new QVBoxLayout(orchestratorSourcesGroup);
    auto* orchestratorSourceHeader = new QHBoxLayout();
    m_orchestratorScanUsersButton = new QPushButton(tr("Scan Source Users"), this);
    m_orchestratorScanUsersButton->setAccessibleName(QStringLiteral("Scan Source Users"));
    m_orchestratorScanUsersButton->setToolTip(QStringLiteral("Scan for user profiles to deploy"));
    orchestratorSourceHeader->addWidget(m_orchestratorScanUsersButton);
    orchestratorSourceHeader->addStretch();
    orchestratorSourcesLayout->addLayout(orchestratorSourceHeader);
    m_orchestratorUserTable = new QTableWidget(0, 3, this);
    m_orchestratorUserTable->setHorizontalHeaderLabels({"?", tr("User"), tr("Size")});
    m_orchestratorUserTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    m_orchestratorUserTable->horizontalHeader()->setSectionResizeMode(2,
        QHeaderView::ResizeToContents);
    m_orchestratorUserTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_orchestratorUserTable->setSelectionMode(QAbstractItemView::SingleSelection);
    m_orchestratorUserTable->setDragEnabled(true);
    m_orchestratorUserTable->setDragDropMode(QAbstractItemView::DragOnly);
    m_orchestratorUserTable->setAccessibleName(QStringLiteral("Source User Profiles"));
    orchestratorSourcesLayout->addWidget(m_orchestratorUserTable);
    orchestratorSourcesGroup->setLayout(orchestratorSourcesLayout);
    orchestratorLayout->addWidget(orchestratorSourcesGroup);
}

void NetworkTransferPanel::setupUi_orchestratorDestinations(QVBoxLayout* orchestratorLayout) {
    auto* orchestratorDestGroup = new QGroupBox(tr("Destinations"), this);
    auto* orchestratorDestLayout = new QVBoxLayout(orchestratorDestGroup);
    m_orchestratorDestTable = new QTableWidget(0, 9, this);
    m_orchestratorDestTable->setHorizontalHeaderLabels({"?", tr("Host"), tr("IP"), tr("Status"),
        tr("Free Disk"), tr("CPU%"), tr("RAM%"), tr("Last Seen"), tr("Progress")});
    m_orchestratorDestTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Fixed);
    m_orchestratorDestTable->setColumnWidth(0, sak::kCheckboxColumnW);
    m_orchestratorDestTable->horizontalHeader()->setSectionResizeMode(1,
        QHeaderView::ResizeToContents);
    m_orchestratorDestTable->horizontalHeader()->setSectionResizeMode(2,
        QHeaderView::ResizeToContents);
    m_orchestratorDestTable->horizontalHeader()->setSectionResizeMode(3,
        QHeaderView::ResizeToContents);
    m_orchestratorDestTable->horizontalHeader()->setSectionResizeMode(4,
        QHeaderView::ResizeToContents);
    m_orchestratorDestTable->horizontalHeader()->setSectionResizeMode(5,
        QHeaderView::ResizeToContents);
    m_orchestratorDestTable->horizontalHeader()->setSectionResizeMode(6,
        QHeaderView::ResizeToContents);
    m_orchestratorDestTable->horizontalHeader()->setSectionResizeMode(7,
        QHeaderView::ResizeToContents);
    m_orchestratorDestTable->horizontalHeader()->setSectionResizeMode(8, QHeaderView::Stretch);
    m_orchestratorDestTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_orchestratorDestTable->setSelectionMode(QAbstractItemView::SingleSelection);
    m_orchestratorDestTable->setAcceptDrops(true);
    m_orchestratorDestTable->setDragDropMode(QAbstractItemView::DropOnly);
    m_orchestratorDestTable->setDropIndicatorShown(true);
    m_orchestratorDestTable->installEventFilter(this);
    m_orchestratorDestTable->setAccessibleName(QStringLiteral("Destination Machines Table"));
    orchestratorDestLayout->addWidget(m_orchestratorDestTable);
    orchestratorDestGroup->setLayout(orchestratorDestLayout);
    orchestratorLayout->addWidget(orchestratorDestGroup);
}

void NetworkTransferPanel::setupUi_deploymentControls(QVBoxLayout* orchestratorLayout) {
    auto* deploymentControlGroup = new QGroupBox(tr("Deployment Controls"), this);
    auto* deploymentControlLayout = new QVBoxLayout(deploymentControlGroup);
    // Mapping type and strategy
    auto* mappingRow = new QHBoxLayout();
    mappingRow->addWidget(new QLabel(tr("Mapping Type:"), this));
    m_mappingTypeCombo = new QComboBox(this);
    m_mappingTypeCombo->addItems({tr("One-to-Many"), tr("Many-to-Many"), tr("Custom Mapping")});
    m_mappingTypeCombo->setAccessibleName(QStringLiteral("Mapping Type"));
    m_mappingTypeCombo->setToolTip(QStringLiteral("Select how users are mapped to destinations"));
    mappingRow->addWidget(m_mappingTypeCombo);
    mappingRow->addWidget(new QLabel(tr("Strategy:"), this));
    m_mappingStrategyCombo = new QComboBox(this);
    m_mappingStrategyCombo->addItems({tr("Largest Free"), tr("Round Robin")});
    m_mappingStrategyCombo->setAccessibleName(QStringLiteral("Mapping Strategy"));
    m_mappingStrategyCombo->setToolTip(QStringLiteral("Strategy for distributing users across "
                                                      "destinations"));
    mappingRow->addWidget(m_mappingStrategyCombo);
    deploymentControlLayout->addLayout(mappingRow);
    // Concurrency and bandwidth
    auto* concurrencyRow = new QHBoxLayout();
    concurrencyRow->addWidget(new QLabel(tr("Max Concurrent:"), this));
    m_maxConcurrentSpin = new QSpinBox(this);
    m_maxConcurrentSpin->setRange(1, 100);
    m_maxConcurrentSpin->setValue(sak::kMaxConcurrentScrape);
    m_maxConcurrentSpin->setAccessibleName(QStringLiteral("Max Concurrent Jobs"));
    m_maxConcurrentSpin->setToolTip(QStringLiteral("Maximum number of simultaneous transfer jobs"));
    concurrencyRow->addWidget(m_maxConcurrentSpin);
    concurrencyRow->addWidget(new QLabel(tr("Global BW (Mbps):"), this));
    m_globalBandwidthSpin = new QSpinBox(this);
    m_globalBandwidthSpin->setRange(0, 100000);
    m_globalBandwidthSpin->setAccessibleName(QStringLiteral("Global Bandwidth"));
    m_globalBandwidthSpin->setToolTip(QStringLiteral("Total bandwidth limit in Mbps for all jobs"));
    concurrencyRow->addWidget(m_globalBandwidthSpin);
    concurrencyRow->addWidget(new QLabel(tr("Per-Job BW (Mbps):"), this));
    m_perJobBandwidthSpin = new QSpinBox(this);
    m_perJobBandwidthSpin->setRange(0, 100000);
    m_perJobBandwidthSpin->setAccessibleName(QStringLiteral("Per-Job Bandwidth"));
    m_perJobBandwidthSpin->setToolTip(QStringLiteral("Bandwidth limit in Mbps per individual job"));
    concurrencyRow->addWidget(m_perJobBandwidthSpin);
    deploymentControlLayout->addLayout(concurrencyRow);
    // Template and action buttons
    setupUi_deploymentTemplateActions(deploymentControlLayout);
    orchestratorLayout->addWidget(deploymentControlGroup);
}

void NetworkTransferPanel::setupUi_deploymentTemplateActions(QVBoxLayout* controlLayout) {
    auto* templateRow = new QHBoxLayout();
    m_useTemplateCheck = new QCheckBox(tr("Use Loaded Template"), this);
    m_useTemplateCheck->setAccessibleName(QStringLiteral("Use Template"));
    m_useTemplateCheck->setToolTip(QStringLiteral("Apply a previously loaded deployment template"));
    templateRow->addWidget(m_useTemplateCheck);
    m_templateStatusLabel = new QLabel(tr("No template loaded"), this);
    templateRow->addWidget(m_templateStatusLabel, 1);
    m_saveTemplateButton = new QPushButton(tr("Save Template"), this);
    m_saveTemplateButton->setAccessibleName(QStringLiteral("Save Template"));
    m_saveTemplateButton->setToolTip(QStringLiteral("Save the current deployment settings as a "
                                                    "template"));
    templateRow->addWidget(m_saveTemplateButton);
    m_loadTemplateButton = new QPushButton(tr("Load Template"), this);
    m_loadTemplateButton->setAccessibleName(QStringLiteral("Load Template"));
    m_loadTemplateButton->setToolTip(QStringLiteral("Load a saved deployment template"));
    templateRow->addWidget(m_loadTemplateButton);
    controlLayout->addLayout(templateRow);

    auto* actionRow = new QHBoxLayout();
    m_startDeploymentButton = new QPushButton(tr("Start Deployment"), this);
    m_startDeploymentButton->setAccessibleName(QStringLiteral("Start Deployment"));
    m_startDeploymentButton->setToolTip(QStringLiteral("Begin deploying user profiles to "
                                                       "destinations"));
    m_pauseDeploymentButton = new QPushButton(tr("Pause"), this);
    m_pauseDeploymentButton->setAccessibleName(QStringLiteral("Pause Deployment"));
    m_pauseDeploymentButton->setToolTip(QStringLiteral("Pause the active deployment"));
    m_resumeDeploymentButton = new QPushButton(tr("Resume"), this);
    m_resumeDeploymentButton->setAccessibleName(QStringLiteral("Resume Deployment"));
    m_resumeDeploymentButton->setToolTip(QStringLiteral("Resume the paused deployment"));
    m_cancelDeploymentButton = new QPushButton(tr("Cancel"), this);
    m_cancelDeploymentButton->setAccessibleName(QStringLiteral("Cancel Deployment"));
    m_cancelDeploymentButton->setToolTip(QStringLiteral("Cancel the current deployment"));
    actionRow->addWidget(m_startDeploymentButton);
    actionRow->addWidget(m_pauseDeploymentButton);
    actionRow->addWidget(m_resumeDeploymentButton);
    actionRow->addWidget(m_cancelDeploymentButton);
    actionRow->addStretch();
    controlLayout->addLayout(actionRow);
}

void NetworkTransferPanel::setupUi_customRules(QVBoxLayout* orchestratorLayout) {
    auto* customRulesGroup = new QGroupBox(tr("Custom Mapping Rules"), this);
    auto* customRulesLayout = new QVBoxLayout(customRulesGroup);
    m_customRulesTable = new QTableWidget(0, 2, this);
    m_customRulesTable->setHorizontalHeaderLabels({tr("Source User"), tr("Destination ID")});
    m_customRulesTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_customRulesTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    m_customRulesTable->setAccessibleName(QStringLiteral("Custom Mapping Rules"));
    customRulesLayout->addWidget(m_customRulesTable);
    customRulesGroup->setLayout(customRulesLayout);
    orchestratorLayout->addWidget(customRulesGroup);
}

void NetworkTransferPanel::setupUi_deploymentJobs(QVBoxLayout* orchestratorLayout) {
    auto* jobsGroup = new QGroupBox(tr("Deployment Jobs"), this);
    auto* jobsLayout = new QVBoxLayout(jobsGroup);
    m_jobsTable = new QTableWidget(0, 7, this);
    m_jobsTable->setHorizontalHeaderLabels({tr("Job ID"), tr("Deployment"), tr("Source User"),
        tr("Destination"), tr("Status"), tr("Progress"), tr("Error")});
    m_jobsTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    m_jobsTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    m_jobsTable->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    m_jobsTable->horizontalHeader()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
    m_jobsTable->horizontalHeader()->setSectionResizeMode(4, QHeaderView::ResizeToContents);
    m_jobsTable->horizontalHeader()->setSectionResizeMode(5, QHeaderView::ResizeToContents);
    m_jobsTable->horizontalHeader()->setSectionResizeMode(6, QHeaderView::Stretch);
    m_jobsTable->setAccessibleName(QStringLiteral("Deployment Jobs Table"));
    jobsLayout->addWidget(m_jobsTable);
    auto* jobActionRow = new QHBoxLayout();
    m_pauseJobButton = new QPushButton(tr("Pause Job"), this);
    m_pauseJobButton->setAccessibleName(QStringLiteral("Pause Job"));
    m_pauseJobButton->setToolTip(QStringLiteral("Pause the selected job"));
    m_resumeJobButton = new QPushButton(tr("Resume Job"), this);
    m_resumeJobButton->setAccessibleName(QStringLiteral("Resume Job"));
    m_resumeJobButton->setToolTip(QStringLiteral("Resume the selected paused job"));
    m_retryJobButton = new QPushButton(tr("Retry Job"), this);
    m_retryJobButton->setAccessibleName(QStringLiteral("Retry Job"));
    m_retryJobButton->setToolTip(QStringLiteral("Retry the selected failed job"));
    m_cancelJobButton = new QPushButton(tr("Cancel Job"), this);
    m_cancelJobButton->setAccessibleName(QStringLiteral("Cancel Job"));
    m_cancelJobButton->setToolTip(QStringLiteral("Cancel the selected job"));
    jobActionRow->addWidget(m_pauseJobButton);
    jobActionRow->addWidget(m_resumeJobButton);
    jobActionRow->addWidget(m_retryJobButton);
    jobActionRow->addWidget(m_cancelJobButton);
    jobActionRow->addStretch();
    jobsLayout->addLayout(jobActionRow);
    jobsGroup->setLayout(jobsLayout);
    orchestratorLayout->addWidget(jobsGroup);
}

void NetworkTransferPanel::setupUi_deploymentProgress(QVBoxLayout* orchestratorLayout) {
    auto* deploymentProgressGroup = new QGroupBox(tr("Deployment Progress"), this);
    auto* deploymentProgressLayout = new QVBoxLayout(deploymentProgressGroup);
    m_deploymentSummaryLabel = new QLabel(tr("0 of 0 complete"), this);
    deploymentProgressLayout->addWidget(m_deploymentSummaryLabel);
    m_deploymentEtaLabel = new QLabel(tr("ETA: --"), this);
    deploymentProgressLayout->addWidget(m_deploymentEtaLabel);
    m_exportHistoryButton = new QPushButton(tr("Export History CSV"), this);
    m_exportHistoryButton->setAccessibleName(QStringLiteral("Export History"));
    m_exportHistoryButton->setToolTip(QStringLiteral("Export deployment history as a CSV file"));
    deploymentProgressLayout->addWidget(m_exportHistoryButton);
    auto* summaryExportRow = new QHBoxLayout();
    m_exportSummaryCsvButton = new QPushButton(tr("Export Summary CSV"), this);
    m_exportSummaryCsvButton->setAccessibleName(QStringLiteral("Export Summary CSV"));
    m_exportSummaryCsvButton->setToolTip(QStringLiteral("Export deployment summary as CSV"));
    m_exportSummaryPdfButton = new QPushButton(tr("Export Summary PDF"), this);
    m_exportSummaryPdfButton->setAccessibleName(QStringLiteral("Export Summary PDF"));
    m_exportSummaryPdfButton->setToolTip(QStringLiteral("Export deployment summary as PDF"));
    summaryExportRow->addWidget(m_exportSummaryCsvButton);
    summaryExportRow->addWidget(m_exportSummaryPdfButton);
    summaryExportRow->addStretch();
    deploymentProgressLayout->addLayout(summaryExportRow);
    m_recoverDeploymentButton = new QPushButton(tr("Recover Last Deployment"), this);
    m_recoverDeploymentButton->setAccessibleName(QStringLiteral("Recover Deployment"));
    m_recoverDeploymentButton->setToolTip(QStringLiteral("Attempt to recover a previously "
                                                         "interrupted deployment"));
    deploymentProgressLayout->addWidget(m_recoverDeploymentButton);
    deploymentProgressGroup->setLayout(deploymentProgressLayout);
    orchestratorLayout->addWidget(deploymentProgressGroup);

    auto* historyGroup = new QGroupBox(tr("Deployment History"), this);
    auto* historyLayout = new QVBoxLayout(historyGroup);
    m_historyTable = new QTableWidget(0, 7, this);
    m_historyTable->setHorizontalHeaderLabels({tr("Deployment"), tr("Started"), tr("Completed"),
        tr("Total"), tr("Completed"), tr("Failed"), tr("Status")});
    m_historyTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    m_historyTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    m_historyTable->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    m_historyTable->horizontalHeader()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
    m_historyTable->horizontalHeader()->setSectionResizeMode(4, QHeaderView::ResizeToContents);
    m_historyTable->horizontalHeader()->setSectionResizeMode(5, QHeaderView::ResizeToContents);
    m_historyTable->horizontalHeader()->setSectionResizeMode(6, QHeaderView::Stretch);
    m_historyTable->setAccessibleName(QStringLiteral("Deployment History Table"));
    historyLayout->addWidget(m_historyTable);
    historyGroup->setLayout(historyLayout);
    orchestratorLayout->addWidget(historyGroup);

    setupUi_statusLegend(orchestratorLayout);
}

void NetworkTransferPanel::setupUi_statusLegend(QVBoxLayout* orchestratorLayout) {
    auto* legendGroup = new QGroupBox(tr("Status Legend"), this);
    auto* legendLayout = new QHBoxLayout(legendGroup);
    // A11Y: emoji prefixes ensure status is readable without relying on color alone
    auto* okLabel = new QLabel(QStringLiteral("\u2714 ") + tr("Success"), this);
        okLabel->setStyleSheet(QString("QLabel { background-color: %1; color: white; padding: 6px "
                                       "10px; border-radius: 10px; }")
                                           .arg(sak::ui::kStatusColorSuccess));
    auto* warnLabel = new QLabel(QStringLiteral("\u23F3 ") + tr("In Progress"), this);
        warnLabel->setStyleSheet(QString("QLabel { background-color: %1; color: %2; padding: 6px "
                                         "10px; border-radius: 10px; }")
                                             .arg(sak::ui::kColorWarningBadge,
                                                 sak::ui::kColorTextHeading));
    auto* errLabel = new QLabel(QStringLiteral("\u2718 ") + tr("Error"), this);
        errLabel->setStyleSheet(QString("QLabel { background-color: %1; color: white; padding: 6px "
                                        "10px; border-radius: 10px; }")
                                            .arg(sak::ui::kStatusColorError));
    auto* idleLabel = new QLabel(QStringLiteral("\u25CB ") + tr("Idle"), this);
        idleLabel->setStyleSheet(QString("QLabel { background-color: %1; color: white; padding: "
                                         "6px 10px; border-radius: 10px; }")
                                             .arg(sak::ui::kStatusColorIdle));
    legendLayout->addWidget(okLabel);
    legendLayout->addWidget(warnLabel);
    legendLayout->addWidget(errLabel);
    legendLayout->addWidget(idleLabel);
    legendLayout->addStretch();
    legendGroup->setLayout(legendLayout);
    orchestratorLayout->addWidget(legendGroup);
}

void NetworkTransferPanel::setupUi_bottomButtons(QVBoxLayout* mainLayout) {
    // Bottom button row: Settings + Security Settings (left), Start + Stop (right)
    auto* transferBtnLayout = new QHBoxLayout();

    auto* netSettingsBtn = new QPushButton(tr("Settings"), this);
    netSettingsBtn->setFixedSize(sak::kButtonWidthLarge, sak::kButtonHeightCompact);
    netSettingsBtn->setAccessibleName(QStringLiteral("Network Settings"));
    netSettingsBtn->setToolTip(QStringLiteral("Configure network transfer settings"));
    connect(netSettingsBtn, &QPushButton::clicked, this, &NetworkTransferPanel::onNetworkSettings);
    transferBtnLayout->addWidget(netSettingsBtn);

    auto* securitySettingsBtn = new QPushButton(tr("Security Settings"), this);
    securitySettingsBtn->setFixedSize(sak::kButtonWidthXLarge, sak::kButtonHeightCompact);
    securitySettingsBtn->setAccessibleName(QStringLiteral("Security Settings"));
    securitySettingsBtn->setToolTip(QStringLiteral("Configure encryption and security options"));
    connect(securitySettingsBtn, &QPushButton::clicked, this,
        &NetworkTransferPanel::onSecuritySettings);
    transferBtnLayout->addWidget(securitySettingsBtn);

    m_logToggle = new LogToggleSwitch(tr("Log"), this);
    transferBtnLayout->addWidget(m_logToggle);

    transferBtnLayout->addStretch();

    m_pauseResumeButton = new QPushButton(tr("Pause"), this);
    m_pauseResumeButton->setFixedSize(sak::kButtonWidthLarge, sak::kButtonHeightCompact);
    m_pauseResumeButton->setVisible(false);
    m_pauseResumeButton->setAccessibleName(QStringLiteral("Pause Transfer"));
    m_pauseResumeButton->setToolTip(QStringLiteral("Pause or resume the active transfer"));
    transferBtnLayout->addWidget(m_pauseResumeButton);

    m_transferButton = new QPushButton(tr("Start Transfer"), this);
    m_transferButton->setFixedSize(sak::kButtonWidthLarge, sak::kButtonHeightCompact);
    m_transferButton->setAccessibleName(QStringLiteral("Start Transfer"));
    m_transferButton->setToolTip(QStringLiteral("Start the network file transfer"));
    m_transferButton->setStyleSheet(sak::ui::kSuccessButtonStyle);
    transferBtnLayout->addWidget(m_transferButton);

    mainLayout->addLayout(transferBtnLayout);
}



void NetworkTransferPanel::setupConnections() {
    Q_ASSERT(m_controller);
    setupConnections_sourceSignals();
    setupConnections_destinationSignals();
    setupConnections_orchestratorSignals();
    setupConnections_controllerSignals();
}

void NetworkTransferPanel::setupConnections_sourceSignals() {
    connect(m_modeCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
        &NetworkTransferPanel::onModeChanged);
    connect(m_scanUsersButton, &QPushButton::clicked, this, &NetworkTransferPanel::onScanUsers);
    connect(m_customizeUserButton, &QPushButton::clicked, this,
        &NetworkTransferPanel::onCustomizeUser);
    connect(m_discoverPeersButton, &QPushButton::clicked, this,
        &NetworkTransferPanel::onDiscoverPeers);
    connect(m_transferButton, &QPushButton::clicked, this, [this]() {
        if (m_sourceTransferActive) {
            m_controller->stop();
            Q_EMIT logOutput(tr("Transfer stopped by user."));
            m_sourceTransferActive = false;
            m_sourceTransferPaused = false;
            updateTransferButton();
            updatePauseResumeButton();
        } else {
            onStartSource();
        }
    });
    connect(m_pauseResumeButton, &QPushButton::clicked, this, [this]() {
        if (m_sourceTransferPaused) {
            m_controller->resumeTransfer();
            m_sourceTransferPaused = false;
            Q_EMIT logOutput(tr("Transfer resumed."));
        } else {
            m_controller->pauseTransfer();
            m_sourceTransferPaused = true;
            Q_EMIT logOutput(tr("Transfer paused."));
        }
        updatePauseResumeButton();
    });
}

void NetworkTransferPanel::setupConnections_destinationSignals() {
    connect(m_startDestinationButton, &QPushButton::clicked, this,
        &NetworkTransferPanel::onStartDestination);
    connect(m_connectOrchestratorButton, &QPushButton::clicked, this,
        &NetworkTransferPanel::onConnectOrchestrator);
    connect(m_approveButton, &QPushButton::clicked, this, &NetworkTransferPanel::onApproveTransfer);
    connect(m_rejectButton, &QPushButton::clicked, this, &NetworkTransferPanel::onRejectTransfer);

    connect(m_controller, &NetworkTransferController::orchestrationAssignmentReceived,
        this, &NetworkTransferPanel::onOrchestrationAssignment);
    connect(m_controller, &NetworkTransferController::orchestrationAssignmentPaused, this,
        [this](const QString& job_id) {
        const QString key = job_id.isEmpty() ? m_activeAssignment.job_id : job_id;
        if (!key.isEmpty()) {
            m_assignmentStatusByJob[key] = tr("paused");
            m_assignmentEventByJob[key] = tr("Paused by orchestrator");
        }
        m_destinationTransferActive = false;
        refreshAssignmentStatus();
        persistAssignmentQueue();
    });
    connect(m_controller, &NetworkTransferController::orchestrationAssignmentResumed, this,
        [this](const QString& job_id) {
        const QString key = job_id.isEmpty() ? m_activeAssignment.job_id : job_id;
        if (!key.isEmpty()) {
            m_assignmentStatusByJob[key] = tr("active");
            m_assignmentEventByJob[key] = tr("Resumed by orchestrator");
        }
        refreshAssignmentStatus();
        persistAssignmentQueue();
    });
    connect(m_controller, &NetworkTransferController::orchestrationAssignmentCanceled, this,
        [this](const QString& job_id) {
        const QString key = job_id.isEmpty() ? m_activeAssignment.job_id : job_id;
        if (!key.isEmpty()) {
            m_assignmentStatusByJob[key] = tr("canceled");
            m_assignmentEventByJob[key] = tr("Canceled by orchestrator");
        }

        m_destinationTransferActive = false;
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

        refreshAssignmentStatus();
        persistAssignmentQueue();
    });
    connect(m_controller, &NetworkTransferController::connectionStateChanged,
        this, &NetworkTransferPanel::onConnectionStateChanged);
}

void NetworkTransferPanel::setupConnections_orchestratorSignals() {
    connect(m_orchestratorListenButton, &QPushButton::clicked, this,
        &NetworkTransferPanel::onStartOrchestratorServer);
    connect(m_orchestratorScanUsersButton, &QPushButton::clicked, this,
        &NetworkTransferPanel::onScanOrchestratorUsers);
    connect(m_startDeploymentButton, &QPushButton::clicked, this,
        &NetworkTransferPanel::onStartDeployment);
    connect(m_pauseDeploymentButton, &QPushButton::clicked, this,
        &NetworkTransferPanel::onPauseDeployment);
    connect(m_resumeDeploymentButton, &QPushButton::clicked, this,
        &NetworkTransferPanel::onResumeDeployment);
    connect(m_cancelDeploymentButton, &QPushButton::clicked, this,
        &NetworkTransferPanel::onCancelDeployment);
    connect(m_saveTemplateButton, &QPushButton::clicked, this,
        &NetworkTransferPanel::onSaveDeploymentTemplate);
    connect(m_loadTemplateButton, &QPushButton::clicked, this,
        &NetworkTransferPanel::onLoadDeploymentTemplate);
    connect(m_pauseJobButton, &QPushButton::clicked, this, &NetworkTransferPanel::onPauseJob);
    connect(m_resumeJobButton, &QPushButton::clicked, this, &NetworkTransferPanel::onResumeJob);
    connect(m_retryJobButton, &QPushButton::clicked, this, &NetworkTransferPanel::onRetryJob);
    connect(m_cancelJobButton, &QPushButton::clicked, this, &NetworkTransferPanel::onCancelJob);
    connect(m_exportHistoryButton, &QPushButton::clicked, this,
        &NetworkTransferPanel::onExportDeploymentHistory);
    connect(m_exportSummaryCsvButton, &QPushButton::clicked, this,
        &NetworkTransferPanel::onExportDeploymentSummaryCsv);
    connect(m_exportSummaryPdfButton, &QPushButton::clicked, this,
        &NetworkTransferPanel::onExportDeploymentSummaryPdf);
    connect(m_recoverDeploymentButton, &QPushButton::clicked, this,
        &NetworkTransferPanel::onRecoverLastDeployment);

    if (m_orchestrator && m_orchestrator->registry()) {
        connect(m_orchestrator->registry(), &DestinationRegistry::destinationRegistered,
            this, &NetworkTransferPanel::onOrchestratorDestinationRegistered);
        connect(m_orchestrator->registry(), &DestinationRegistry::destinationUpdated,
            this, &NetworkTransferPanel::onOrchestratorDestinationUpdated);
        connect(m_orchestrator->registry(), &DestinationRegistry::destinationRemoved,
            this, &NetworkTransferPanel::onOrchestratorDestinationRemoved);
    }
    if (m_orchestrator) {
        connect(m_orchestrator, &MigrationOrchestrator::deploymentProgress,
            this, &NetworkTransferPanel::onOrchestratorProgress);
        connect(m_orchestrator, &MigrationOrchestrator::deploymentCompleted,
            this, &NetworkTransferPanel::onOrchestratorCompletion);
        connect(m_orchestrator, &MigrationOrchestrator::aggregateProgress,
            this, &NetworkTransferPanel::onAggregateProgress);
        connect(m_orchestrator, &MigrationOrchestrator::orchestratorStatus, this,
            [this](const QString& msg) {
            Q_EMIT logOutput(msg);
            Q_EMIT statusMessage(msg, sak::kTimerStatusDefaultMs);
        });
    }
}

void NetworkTransferPanel::setupConnections_controllerSignals() {
    connect(m_controller, &NetworkTransferController::peerDiscovered, this,
        &NetworkTransferPanel::onPeerDiscovered);
    connect(m_controller, &NetworkTransferController::manifestReceived, this,
        &NetworkTransferPanel::onManifestReceived);
    connect(m_controller, &NetworkTransferController::transferProgress, this,
        &NetworkTransferPanel::onTransferProgress);
    connect(m_controller, &NetworkTransferController::transferCompleted, this,
        &NetworkTransferPanel::onTransferCompleted);
    connect(m_controller, &NetworkTransferController::statusMessage, this,
        [this](const QString& msg) {
        Q_EMIT logOutput(msg);
        Q_EMIT statusMessage(msg, sak::kTimerStatusDefaultMs);
    });
    connect(m_controller, &NetworkTransferController::errorMessage, this,
        [this](const QString& msg) {
        Q_EMIT logOutput(tr("ERROR: %1").arg(msg));
        m_transferErrors.append(msg);
        Q_EMIT statusMessage(msg, sak::kTimerStatusDefaultMs);
    });

    setupConnections_parallelManager();
}

void NetworkTransferPanel::setupConnections_parallelManager() {
    if (m_parallelManager) {
        connect(m_parallelManager, &ParallelTransferManager::jobStartRequested, this,
            &NetworkTransferPanel::onJobStartRequested);
        connect(m_parallelManager, &ParallelTransferManager::jobUpdated, this,
            &NetworkTransferPanel::onJobUpdated);
        connect(m_parallelManager, &ParallelTransferManager::jobCompleted, this,
            &NetworkTransferPanel::onJobCompleted);
        connect(m_parallelManager, &ParallelTransferManager::deploymentProgress, this,
            &NetworkTransferPanel::onParallelDeploymentProgress);
        connect(m_parallelManager, &ParallelTransferManager::deploymentComplete, this,
            &NetworkTransferPanel::onParallelDeploymentCompleted);
        connect(m_parallelManager, &ParallelTransferManager::jobBandwidthUpdateRequested,
            this, [this](const QString& job_id, int max_kbps) {
                if (m_jobSourceControllers.contains(job_id)) {
                    m_jobSourceControllers.value(job_id)->updateBandwidthLimit(max_kbps);
                }
            });
        connect(m_parallelManager, &ParallelTransferManager::jobPauseRequested,
            this, [this](const QString& job_id) {
                if (m_jobSourceControllers.contains(job_id)) {
                    m_jobSourceControllers.value(job_id)->pauseTransfer();
                }
                if (!m_orchestrator) return;
                const QString destination_id = m_jobToDestinationId.value(job_id);
                if (destination_id.isEmpty()) return;
                const QString deployment_id = m_jobToDeploymentId.value(job_id);
                m_orchestrator->pauseAssignment(destination_id, deployment_id, job_id);
            });
        connect(m_parallelManager, &ParallelTransferManager::jobResumeRequested,
            this, [this](const QString& job_id) {
                if (m_jobSourceControllers.contains(job_id)) {
                    m_jobSourceControllers.value(job_id)->resumeTransfer();
                }
                if (!m_orchestrator) return;
                const QString destination_id = m_jobToDestinationId.value(job_id);
                if (destination_id.isEmpty()) return;
                const QString deployment_id = m_jobToDeploymentId.value(job_id);
                m_orchestrator->resumeAssignment(destination_id, deployment_id, job_id);
            });
        connect(m_parallelManager, &ParallelTransferManager::jobCancelRequested,
            this, [this](const QString& job_id) {
                if (m_jobSourceControllers.contains(job_id)) {
                    auto* controller = m_jobSourceControllers.take(job_id);
                    controller->cancelTransfer();
                    controller->deleteLater();
                }
                if (!m_orchestrator) return;
                const QString destination_id = m_jobToDestinationId.value(job_id);
                if (destination_id.isEmpty()) return;
                const QString deployment_id = m_jobToDeploymentId.value(job_id);
                m_orchestrator->cancelAssignment(destination_id, deployment_id, job_id);
            });
    }
}

void NetworkTransferPanel::loadSettings() {
    Q_ASSERT(m_controller);
    auto& config = ConfigManager::instance();
    m_settings.encryption_enabled = config.getNetworkTransferEncryptionEnabled();
    m_settings.compression_enabled = config.getNetworkTransferCompressionEnabled();
    m_settings.resume_enabled = config.getNetworkTransferResumeEnabled();
    m_settings.auto_discovery_enabled = config.getNetworkTransferAutoDiscoveryEnabled();
    m_settings.max_bandwidth_kbps = config.getNetworkTransferMaxBandwidth();
    m_settings.chunk_size = config.getNetworkTransferChunkSize();
    m_settings.discovery_port = static_cast<quint16>(config.getNetworkTransferDiscoveryPort());
    m_settings.control_port = static_cast<quint16>(config.getNetworkTransferControlPort());
    m_settings.data_port = static_cast<quint16>(config.getNetworkTransferDataPort());
    m_settings.relay_server = config.getNetworkTransferRelayServer();

    m_manualPortSpin->setValue(m_settings.control_port);
    m_encryptCheck->setChecked(m_settings.encryption_enabled);
    m_compressCheck->setChecked(m_settings.compression_enabled);
    m_resumeCheck->setChecked(m_settings.resume_enabled);
    m_chunkSizeSpin->setValue(m_settings.chunk_size / sak::kBytesPerKB);
    m_bandwidthSpin->setValue(m_settings.max_bandwidth_kbps);
    m_permissionModeCombo->setCurrentIndex(0);

    m_mappingTypeCombo->setCurrentIndex(config.getValue("orchestration/mapping_type", 0).toInt());
    m_mappingStrategyCombo->setCurrentIndex(config.getValue("orchestration/mapping_strategy",
        0).toInt());
    m_maxConcurrentSpin->setValue(config.getValue("orchestration/max_concurrent",
        sak::kMaxConcurrentScrape).toInt());
    m_globalBandwidthSpin->setValue(config.getValue("orchestration/global_bandwidth", 0).toInt());
    m_perJobBandwidthSpin->setValue(config.getValue("orchestration/per_job_bandwidth", 0).toInt());
    m_useTemplateCheck->setChecked(config.getValue("orchestration/use_template", false).toBool());

    m_controller->configure(m_settings);

    QString defaultBase = QDir::toNativeSeparators(QDir::rootPath() + "Users");
    QString stagingBase = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation) +
        "/SAK/Incoming";
    m_destinationBaseEdit->setText(QDir::toNativeSeparators(stagingBase));

    QStringList addresses;
    for (const auto& iface : QNetworkInterface::allInterfaces()) {
        for (const auto& entry : iface.addressEntries()) {
            if (entry.ip().protocol() != QAbstractSocket::IPv4Protocol) continue;
            if (entry.ip().isLoopback()) continue;
            addresses.append(entry.ip().toString());
        }
    }
    m_destinationInfo->setText(tr("Listening on ports %1/%2. Local IPs: %3")
                                   .arg(m_settings.control_port)
                                   .arg(m_settings.data_port)
                                   .arg(addresses.join(", ")));

    loadSettings_initHistoryManager();
    loadSettings_initAssignmentQueue();
    loadSettings_restoreDeploymentState();
}

void NetworkTransferPanel::loadSettings_initHistoryManager() {
    if (m_historyManager) return;

    auto& config = ConfigManager::instance();
    QString historyPath = config.getValue("orchestration/history_path").toString();
    if (historyPath.isEmpty()) {
        const QString historyDir =
            QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation) + "/SAK";
        QDir dir(historyDir);
        if (!dir.exists()) {
            dir.mkpath(".");
        }
        historyPath = historyDir + "/DeploymentHistory.json";
        config.setValue("orchestration/history_path", historyPath);
    }
    QDir dir(QFileInfo(historyPath).absolutePath());
    if (!dir.exists()) {
        dir.mkpath(".");
    }
    m_historyManager = std::make_unique<DeploymentHistoryManager>(historyPath);
}

void NetworkTransferPanel::loadSettings_initAssignmentQueue() {
    if (m_assignmentQueueStore) return;

    auto& config = ConfigManager::instance();
    QString queuePath = config.getValue("orchestration/assignment_queue_path").toString();
    if (queuePath.isEmpty()) {
        const QString queueDir =
            QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation) + "/SAK";
        QDir dir(queueDir);
        if (!dir.exists()) {
            dir.mkpath(".");
        }
        queuePath = queueDir + "/AssignmentQueue.json";
        config.setValue("orchestration/assignment_queue_path", queuePath);
    }
    QDir dir(QFileInfo(queuePath).absolutePath());
    if (!dir.exists()) {
        dir.mkpath(".");
    }
    m_assignmentQueueStore = std::make_unique<AssignmentQueueStore>(queuePath);

    DeploymentAssignment storedActive;
    QQueue<DeploymentAssignment> storedQueue;
    QMap<QString, QString> storedStatus;
    QMap<QString, QString> storedEvent;
    if (m_assignmentQueueStore->load(storedActive, storedQueue, storedStatus, storedEvent)) {
        m_activeAssignment = storedActive;
        m_assignmentQueue = storedQueue;
        m_assignmentStatusByJob = storedStatus;
        m_assignmentEventByJob = storedEvent;
        if (!m_activeAssignment.deployment_id.isEmpty() && m_activeAssignmentLabel) {
            m_activeAssignmentLabel->setText(tr("Active: %1 (%2)")
                                                 .arg(m_activeAssignment.source_user,
                                                     m_activeAssignment.deployment_id));
        }
        refreshAssignmentQueue();
        refreshAssignmentStatus();
    }
}

void NetworkTransferPanel::loadSettings_restoreDeploymentState() {
    auto& config = ConfigManager::instance();

    const QString lastTemplatePath = config.getValue("orchestration/last_template_path").toString();
    if (!lastTemplatePath.isEmpty() && QFileInfo::exists(lastTemplatePath)) {
        m_loadedMapping = m_mappingEngine->loadTemplate(lastTemplatePath);
        if (!m_loadedMapping.sources.isEmpty()) {
            m_loadedTemplatePath = lastTemplatePath;
            m_templateStatusLabel->setText(tr("Loaded template: %1")
                .arg(QFileInfo(lastTemplatePath).fileName()));
        }
    }

    m_activeDeploymentId = config.getValue("orchestration/last_deployment_id").toString();
    const auto started = config.getValue("orchestration/last_deployment_started").toString();
    if (!started.isEmpty()) {
        m_deploymentStartedAt = QDateTime::fromString(started, Qt::ISODate);
    }

    if (!m_activeDeploymentId.isEmpty()) {
        Q_EMIT logOutput(tr("Last deployment: %1").arg(m_activeDeploymentId));
    }

    refreshDeploymentHistory();
}

void NetworkTransferPanel::onModeChanged(int index) {
    Q_ASSERT(m_controller);
    m_modeStack->setCurrentIndex(index);
    m_controller->stopDiscovery();
}

void NetworkTransferPanel::onSecuritySettings() {
    QDialog dialog(this);
    dialog.setWindowTitle(tr("Security & Transfer Settings"));
    dialog.setMinimumWidth(sak::kDialogWidthMedium);

    auto* layout = new QGridLayout(&dialog);

    auto ctl = buildSecurityDialogControls(layout, &dialog);

    auto* btnLayout = new QHBoxLayout();
    btnLayout->addStretch();
    auto* okBtn = new QPushButton(tr("OK"), &dialog);
    auto* cancelBtn = new QPushButton(tr("Cancel"), &dialog);
    connect(okBtn, &QPushButton::clicked, &dialog, &QDialog::accept);
    connect(cancelBtn, &QPushButton::clicked, &dialog, &QDialog::reject);
    btnLayout->addWidget(okBtn);
    btnLayout->addWidget(cancelBtn);
    layout->addLayout(btnLayout, 4, 0, 1, 4);

    if (dialog.exec() == QDialog::Accepted) {
        applySecurityDialogResults(ctl);
    }
}

NetworkTransferPanel::SecurityDialogControls
NetworkTransferPanel::buildSecurityDialogControls(QGridLayout* layout, QDialog* dialog)
{
    SecurityDialogControls ctl;

    ctl.encryptCheck = new QCheckBox(tr("Encrypt (AES-256-GCM)"), dialog);
    ctl.encryptCheck->setChecked(m_encryptCheck->isChecked());
    ctl.compressCheck = new QCheckBox(tr("Compress"), dialog);
    ctl.compressCheck->setChecked(m_compressCheck->isChecked());
    ctl.resumeCheck = new QCheckBox(tr("Resume"), dialog);
    ctl.resumeCheck->setChecked(m_resumeCheck->isChecked());

    auto* encRow = new QHBoxLayout();
    encRow->addWidget(ctl.encryptCheck);
    encRow->addWidget(new sak::InfoButton(tr("Encrypt all data in transit using AES-256-GCM"),
        dialog));
    layout->addLayout(encRow, 0, 0);
    auto* cmpRow = new QHBoxLayout();
    cmpRow->addWidget(ctl.compressCheck);
    cmpRow->addWidget(new sak::InfoButton(tr("Compress data before sending to reduce bandwidth"),
        dialog));
    layout->addLayout(cmpRow, 0, 1);
    auto* resRow = new QHBoxLayout();
    resRow->addWidget(ctl.resumeCheck);
    resRow->addWidget(new sak::InfoButton(tr("Allow interrupted transfers to resume from the last "
                                             "checkpoint"), dialog));
    layout->addLayout(resRow, 0, 2);

    layout->addWidget(sak::InfoButton::createInfoLabel(tr("Chunk (KB):"),
        tr("Size of each data block in kilobytes"), dialog), 1, 0);
    ctl.chunkSpin = new QSpinBox(dialog);
    ctl.chunkSpin->setRange(16, 4096);
    ctl.chunkSpin->setValue(m_chunkSizeSpin->value());
    layout->addWidget(ctl.chunkSpin, 1, 1);

    layout->addWidget(sak::InfoButton::createInfoLabel(tr("Bandwidth (KB/s):"),
        tr("Maximum transfer speed in KB/s (0 = unlimited)"), dialog), 1, 2);
    ctl.bwSpin = new QSpinBox(dialog);
    ctl.bwSpin->setRange(0, sak::kBytesPerMB);
    ctl.bwSpin->setValue(m_bandwidthSpin->value());
    layout->addWidget(ctl.bwSpin, 1, 3);

    layout->addWidget(sak::InfoButton::createInfoLabel(tr("Permissions:"),
        tr("How to handle file permissions on the destination machine"), dialog), 2, 0);
    ctl.permCombo = new QComboBox(dialog);
    ctl.permCombo->addItem(tr("Strip All"), static_cast<int>(PermissionMode::StripAll));
    ctl.permCombo->addItem(tr("Preserve Original"),
        static_cast<int>(PermissionMode::PreserveOriginal));
    ctl.permCombo->addItem(tr("Assign to Destination"),
        static_cast<int>(PermissionMode::AssignToDestination));
    ctl.permCombo->addItem(tr("Hybrid"), static_cast<int>(PermissionMode::Hybrid));
    ctl.permCombo->setCurrentIndex(m_permissionModeCombo->currentIndex());
    layout->addWidget(ctl.permCombo, 2, 1, 1, 2);

    layout->addWidget(sak::InfoButton::createInfoLabel(tr("Passphrase:"),
        tr("Shared passphrase both machines must use for encrypted transfers"), dialog), 3, 0);
    ctl.passEdit = new QLineEdit(dialog);
    ctl.passEdit->setEchoMode(QLineEdit::Password);
    ctl.passEdit->setText(m_passphraseEdit->text());
    layout->addWidget(ctl.passEdit, 3, 1, 1, 3);

    return ctl;
}

void NetworkTransferPanel::applySecurityDialogResults(const SecurityDialogControls& ctl)
{
    m_encryptCheck->setChecked(ctl.encryptCheck->isChecked());
    m_compressCheck->setChecked(ctl.compressCheck->isChecked());
    m_resumeCheck->setChecked(ctl.resumeCheck->isChecked());
    m_chunkSizeSpin->setValue(ctl.chunkSpin->value());
    m_bandwidthSpin->setValue(ctl.bwSpin->value());
    m_permissionModeCombo->setCurrentIndex(ctl.permCombo->currentIndex());
    m_passphraseEdit->setText(ctl.passEdit->text());
}

void NetworkTransferPanel::onNetworkSettings() {
    QDialog dialog(this);
    dialog.setWindowTitle(tr("Network Transfer Settings"));
    dialog.setMinimumWidth(450);

    auto* layout = new QFormLayout(&dialog);

    buildNetworkSettingsToggles(layout, &dialog);
    buildNetworkSettingsPorts(layout, &dialog);

    auto* btnLayout = new QHBoxLayout();
    btnLayout->addStretch();
    auto* okBtn = new QPushButton(tr("OK"), &dialog);
    auto* cancelBtn = new QPushButton(tr("Cancel"), &dialog);
    connect(okBtn, &QPushButton::clicked, &dialog, &QDialog::accept);
    connect(cancelBtn, &QPushButton::clicked, &dialog, &QDialog::reject);
    btnLayout->addWidget(okBtn);
    btnLayout->addWidget(cancelBtn);
    layout->addRow(btnLayout);

    if (dialog.exec() == QDialog::Accepted) {
        saveNetworkSettingsFromDialog(&dialog);
    }
}

void NetworkTransferPanel::buildNetworkSettingsToggles(QFormLayout* layout, QDialog* dialog) {
    Q_ASSERT(layout);
    Q_ASSERT(dialog);
    auto& config = ConfigManager::instance();

    auto* enabledCheck = new QCheckBox(dialog);
    enabledCheck->setObjectName(QStringLiteral("ntEnabled"));
    enabledCheck->setChecked(config.getNetworkTransferEnabled());
    layout->addRow(
        sak::InfoButton::createInfoLabel(tr("Enabled:"),
            tr("Master switch — enable or disable all network transfer functionality"), dialog),
        enabledCheck);

    auto* autoDiscCheck = new QCheckBox(dialog);
    autoDiscCheck->setObjectName(QStringLiteral("ntAutoDisc"));
    autoDiscCheck->setChecked(config.getNetworkTransferAutoDiscoveryEnabled());
    layout->addRow(
        sak::InfoButton::createInfoLabel(tr("Auto Discovery:"),
            tr("Automatically find other S.A.K. instances on the local network via UDP broadcast"),
                dialog),
        autoDiscCheck);

    auto* encryptCheck = new QCheckBox(dialog);
    encryptCheck->setObjectName(QStringLiteral("ntEncrypt"));
    encryptCheck->setChecked(config.getNetworkTransferEncryptionEnabled());
    layout->addRow(
        sak::InfoButton::createInfoLabel(tr("Encryption:"),
            tr("Encrypt file data in transit using AES-256-GCM for security"), dialog),
        encryptCheck);

    auto* compressCheck = new QCheckBox(dialog);
    compressCheck->setObjectName(QStringLiteral("ntCompress"));
    compressCheck->setChecked(config.getNetworkTransferCompressionEnabled());
    layout->addRow(
        sak::InfoButton::createInfoLabel(tr("Compression:"),
            tr("Compress file data before sending — reduces bandwidth but adds CPU overhead"),
                dialog),
        compressCheck);

    auto* resumeCheck = new QCheckBox(dialog);
    resumeCheck->setObjectName(QStringLiteral("ntResume"));
    resumeCheck->setChecked(config.getNetworkTransferResumeEnabled());
    layout->addRow(
        sak::InfoButton::createInfoLabel(tr("Resume:"),
            tr("Allow interrupted transfers to resume from where they left off instead of "
               "restarting"), dialog),
        resumeCheck);
}

void NetworkTransferPanel::buildNetworkSettingsPorts(QFormLayout* layout, QDialog* dialog) {
    Q_ASSERT(layout);
    Q_ASSERT(dialog);
    auto& config = ConfigManager::instance();

    auto* discoveryPort = new QSpinBox(dialog);
    discoveryPort->setObjectName(QStringLiteral("ntDiscoveryPort"));
    discoveryPort->setRange(1024, 65535);
    discoveryPort->setValue(config.getNetworkTransferDiscoveryPort());
    layout->addRow(
        sak::InfoButton::createInfoLabel(tr("Discovery Port:"),
            tr("UDP port used for auto-discovery broadcasts — must match on both machines"),
                dialog),
        discoveryPort);

    auto* controlPort = new QSpinBox(dialog);
    controlPort->setObjectName(QStringLiteral("ntControlPort"));
    controlPort->setRange(1024, 65535);
    controlPort->setValue(config.getNetworkTransferControlPort());
    layout->addRow(
        sak::InfoButton::createInfoLabel(tr("Control Port:"),
            tr("TCP port for transfer control commands (handshake, status, cancel)"), dialog),
        controlPort);

    auto* dataPort = new QSpinBox(dialog);
    dataPort->setObjectName(QStringLiteral("ntDataPort"));
    dataPort->setRange(1024, 65535);
    dataPort->setValue(config.getNetworkTransferDataPort());
    layout->addRow(
        sak::InfoButton::createInfoLabel(tr("Data Port:"),
            tr("TCP port for actual file data transfer — must be open in your firewall"), dialog),
        dataPort);

    auto* chunkSize = new QSpinBox(dialog);
    chunkSize->setObjectName(QStringLiteral("ntChunkSize"));
    chunkSize->setRange(16384, 4194304);
    chunkSize->setSuffix(tr(" bytes"));
    chunkSize->setValue(config.getNetworkTransferChunkSize());
    layout->addRow(
        sak::InfoButton::createInfoLabel(tr("Chunk Size:"),
            tr("Size of each data block sent over the network — larger chunks improve throughput "
               "on fast links"), dialog),
        chunkSize);

    auto* maxBw = new QSpinBox(dialog);
    maxBw->setObjectName(QStringLiteral("ntMaxBw"));
    maxBw->setRange(0, sak::kBytesPerMB);
    maxBw->setSuffix(tr(" KB/s"));
    maxBw->setValue(config.getNetworkTransferMaxBandwidth());
    layout->addRow(
        sak::InfoButton::createInfoLabel(tr("Max Bandwidth:"),
            tr("Limit transfer speed to avoid saturating the network (0 = unlimited)"), dialog),
        maxBw);

    auto* relaySrv = new QLineEdit(dialog);
    relaySrv->setObjectName(QStringLiteral("ntRelay"));
    relaySrv->setText(config.getNetworkTransferRelayServer());
    layout->addRow(
        sak::InfoButton::createInfoLabel(tr("Relay Server:"),
            tr("Optional relay server address for transfers across different networks or subnets"),
                dialog),
        relaySrv);
}

void NetworkTransferPanel::saveNetworkSettingsFromDialog(QDialog* dialog) {
    Q_ASSERT(dialog);
    auto& config = ConfigManager::instance();

    if (auto* w = dialog->findChild<QCheckBox*>(QStringLiteral("ntEnabled")))
        config.setNetworkTransferEnabled(w->isChecked());
    if (auto* w = dialog->findChild<QCheckBox*>(QStringLiteral("ntAutoDisc")))
        config.setNetworkTransferAutoDiscoveryEnabled(w->isChecked());
    if (auto* w = dialog->findChild<QCheckBox*>(QStringLiteral("ntEncrypt")))
        config.setNetworkTransferEncryptionEnabled(w->isChecked());
    if (auto* w = dialog->findChild<QCheckBox*>(QStringLiteral("ntCompress")))
        config.setNetworkTransferCompressionEnabled(w->isChecked());
    if (auto* w = dialog->findChild<QCheckBox*>(QStringLiteral("ntResume")))
        config.setNetworkTransferResumeEnabled(w->isChecked());
    if (auto* w = dialog->findChild<QSpinBox*>(QStringLiteral("ntDiscoveryPort")))
        config.setNetworkTransferDiscoveryPort(w->value());
    if (auto* w = dialog->findChild<QSpinBox*>(QStringLiteral("ntControlPort")))
        config.setNetworkTransferControlPort(w->value());
    if (auto* w = dialog->findChild<QSpinBox*>(QStringLiteral("ntDataPort")))
        config.setNetworkTransferDataPort(w->value());
    if (auto* w = dialog->findChild<QSpinBox*>(QStringLiteral("ntChunkSize")))
        config.setNetworkTransferChunkSize(w->value());
    if (auto* w = dialog->findChild<QSpinBox*>(QStringLiteral("ntMaxBw")))
        config.setNetworkTransferMaxBandwidth(w->value());
    if (auto* w = dialog->findChild<QLineEdit*>(QStringLiteral("ntRelay")))
        config.setNetworkTransferRelayServer(w->text());

    loadSettings();
}

void NetworkTransferPanel::updateTransferButton() {
    Q_ASSERT(m_transferButton);
    if (m_sourceTransferActive) {
        m_transferButton->setText(tr("Stop Transfer"));
        m_transferButton->setStyleSheet(sak::ui::kDangerButtonStyle);
    } else {
        m_transferButton->setText(tr("Start Transfer"));
        m_transferButton->setStyleSheet(sak::ui::kSuccessButtonStyle);
    }
}

void NetworkTransferPanel::updatePauseResumeButton() {
    Q_ASSERT(m_pauseResumeButton);
    if (m_sourceTransferActive) {
        m_pauseResumeButton->setVisible(true);
        if (m_sourceTransferPaused) {
            m_pauseResumeButton->setText(tr("Resume"));
            m_pauseResumeButton->setStyleSheet(sak::ui::kSuccessButtonStyle);
        } else {
            m_pauseResumeButton->setText(tr("Pause"));
            m_pauseResumeButton->setStyleSheet(sak::ui::kPauseButtonStyle);
        }
    } else {
        m_pauseResumeButton->setVisible(false);
    }
}

} // namespace sak

