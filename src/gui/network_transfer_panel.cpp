#include "sak/network_transfer_panel.h"

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

QString formatBytes(qint64 bytes) {
    const double gb = bytes / (1024.0 * 1024.0 * 1024.0);
    if (gb >= 1.0) return QString::number(gb, 'f', 2) + " GB";
    const double mb = bytes / (1024.0 * 1024.0);
    if (mb >= 1.0) return QString::number(mb, 'f', 2) + " MB";
    const double kb = bytes / 1024.0;
    return QString::number(kb, 'f', 2) + " KB";
}

QColor statusColor(const QString& status) {
    const QString value = status.trimmed().toLower();
    if (value.contains("success") || value.contains("complete") || value.contains("ready")) {
        return QColor(56, 142, 60);
    }
    if (value.contains("fail") || value.contains("error") || value.contains("reject") || value.contains("cancel")) {
        return QColor(198, 40, 40);
    }
    if (value.contains("active") || value.contains("transfer") || value.contains("approved") || value.contains("queued") || value.contains("progress")) {
        return QColor(245, 124, 0);
    }
    return QColor(97, 97, 97);
}

QColor progressColor(int percent) {
    if (percent >= 100) {
        return QColor(56, 142, 60);
    }
    if (percent > 0) {
        return QColor(245, 124, 0);
    }
    return QColor(97, 97, 97);
}

void applyStatusColors(QTableWidgetItem* item, const QColor& color) {
    if (!item) {
        return;
    }
    item->setBackground(color);
    item->setForeground(Qt::white);
}

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
    auto* mainLayout = new QVBoxLayout(this);
    mainLayout->setSpacing(8);
    mainLayout->setContentsMargins(8, 8, 8, 8);

    auto* modeLayout = new QHBoxLayout();
    modeLayout->addWidget(new QLabel(tr("Mode:"), this));
    m_modeCombo = new QComboBox(this);
    m_modeCombo->addItems({tr("Source (Send)"), tr("Destination (Receive)"), tr("Orchestrator (Deploy)")});
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

    // Source UI
    auto* sourceWidget = new QWidget(this);
    auto* sourceLayout = new QVBoxLayout(sourceWidget);

    auto* dataGroup = new QGroupBox(tr("Data Selection"), sourceWidget);
    auto* dataLayout = new QVBoxLayout(dataGroup);

    auto* userHeaderLayout = new QHBoxLayout();
    m_scanUsersButton = new QPushButton(tr("Scan Users"), this);
    m_customizeUserButton = new QPushButton(tr("Customize Selected"), this);
    userHeaderLayout->addWidget(m_scanUsersButton);
    userHeaderLayout->addWidget(m_customizeUserButton);
    userHeaderLayout->addStretch();
    dataLayout->addLayout(userHeaderLayout);

    m_userTable = new QTableWidget(0, kUserColCount, this);
    m_userTable->setHorizontalHeaderLabels({"✓", tr("User"), tr("Profile Path"), tr("Size")});
    m_userTable->horizontalHeader()->setSectionResizeMode(kUserColName, QHeaderView::ResizeToContents);
    m_userTable->horizontalHeader()->setSectionResizeMode(kUserColPath, QHeaderView::Stretch);
    m_userTable->horizontalHeader()->setSectionResizeMode(kUserColSize, QHeaderView::ResizeToContents);
    m_userTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_userTable->setSelectionMode(QAbstractItemView::SingleSelection);
    dataLayout->addWidget(m_userTable);

    dataGroup->setLayout(dataLayout);
    sourceLayout->addWidget(dataGroup);

    auto* peerGroup = new QGroupBox(tr("Destination Discovery"), sourceWidget);
    auto* peerLayout = new QVBoxLayout(peerGroup);

    auto* peerHeaderLayout = new QHBoxLayout();
    m_discoverPeersButton = new QPushButton(tr("Discover Peers"), this);
    peerHeaderLayout->addWidget(m_discoverPeersButton);
    peerHeaderLayout->addStretch();
    peerLayout->addLayout(peerHeaderLayout);

    m_peerTable = new QTableWidget(0, kPeerColCount, this);
    m_peerTable->setHorizontalHeaderLabels({tr("Host"), tr("IP"), tr("Mode"), tr("Capabilities"), tr("Last Seen")});
    m_peerTable->horizontalHeader()->setSectionResizeMode(kPeerColName, QHeaderView::ResizeToContents);
    m_peerTable->horizontalHeader()->setSectionResizeMode(kPeerColIp, QHeaderView::ResizeToContents);
    m_peerTable->horizontalHeader()->setSectionResizeMode(kPeerColCaps, QHeaderView::Stretch);
    m_peerTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_peerTable->setSelectionMode(QAbstractItemView::SingleSelection);
    peerLayout->addWidget(m_peerTable);

    auto* manualLayout = new QHBoxLayout();
    manualLayout->addWidget(new QLabel(tr("Manual IP:"), this));
    m_manualIpEdit = new QLineEdit(this);
    m_manualIpEdit->setPlaceholderText(tr("192.168.1.100"));
    manualLayout->addWidget(m_manualIpEdit);
    manualLayout->addWidget(new QLabel(tr("Port:"), this));
    m_manualPortSpin = new QSpinBox(this);
    m_manualPortSpin->setRange(1024, 65535);
    manualLayout->addWidget(m_manualPortSpin);
    peerLayout->addLayout(manualLayout);

    peerGroup->setLayout(peerLayout);
    sourceLayout->addWidget(peerGroup);

    auto* securityGroup = new QGroupBox(tr("Security & Transfer"), sourceWidget);
    auto* securityLayout = new QGridLayout(securityGroup);

    m_encryptCheck = new QCheckBox(tr("Encrypt (AES-256-GCM)"), this);
    m_compressCheck = new QCheckBox(tr("Compress"), this);
    m_resumeCheck = new QCheckBox(tr("Resume"), this);
    securityLayout->addWidget(m_encryptCheck, 0, 0);
    securityLayout->addWidget(m_compressCheck, 0, 1);
    securityLayout->addWidget(m_resumeCheck, 0, 2);

    securityLayout->addWidget(new QLabel(tr("Chunk (KB):"), this), 1, 0);
    m_chunkSizeSpin = new QSpinBox(this);
    m_chunkSizeSpin->setRange(16, 4096);
    securityLayout->addWidget(m_chunkSizeSpin, 1, 1);

    securityLayout->addWidget(new QLabel(tr("Bandwidth (KB/s):"), this), 1, 2);
    m_bandwidthSpin = new QSpinBox(this);
    m_bandwidthSpin->setRange(0, 1024 * 1024);
    m_bandwidthSpin->setToolTip(tr("0 = unlimited"));
    securityLayout->addWidget(m_bandwidthSpin, 1, 3);

    securityLayout->addWidget(new QLabel(tr("Permissions:"), this), 2, 0);
    m_permissionModeCombo = new QComboBox(this);
    m_permissionModeCombo->addItem(tr("Strip All"), static_cast<int>(PermissionMode::StripAll));
    m_permissionModeCombo->addItem(tr("Preserve Original"), static_cast<int>(PermissionMode::PreserveOriginal));
    m_permissionModeCombo->addItem(tr("Assign to Destination"), static_cast<int>(PermissionMode::AssignToDestination));
    m_permissionModeCombo->addItem(tr("Hybrid"), static_cast<int>(PermissionMode::Hybrid));
    securityLayout->addWidget(m_permissionModeCombo, 2, 1, 1, 2);

    securityLayout->addWidget(new QLabel(tr("Passphrase:"), this), 2, 3);
    m_passphraseEdit = new QLineEdit(this);
    m_passphraseEdit->setEchoMode(QLineEdit::Password);
    securityLayout->addWidget(m_passphraseEdit, 2, 4);

    securityGroup->setLayout(securityLayout);
    sourceLayout->addWidget(securityGroup);

    m_startSourceButton = new QPushButton(tr("Start Transfer"), this);
    sourceLayout->addWidget(m_startSourceButton);

    sourceWidget->setLayout(sourceLayout);

    // Destination UI
    auto* destWidget = new QWidget(this);
    auto* destLayout = new QVBoxLayout(destWidget);

    auto* destInfoGroup = new QGroupBox(tr("Destination Setup"), destWidget);
    auto* destInfoLayout = new QVBoxLayout(destInfoGroup);

    m_destinationInfo = new QLabel(this);
    m_destinationInfo->setWordWrap(true);
    destInfoLayout->addWidget(m_destinationInfo);

    auto* destBaseLayout = new QHBoxLayout();
    destBaseLayout->addWidget(new QLabel(tr("Destination Base:"), this));
    m_destinationBaseEdit = new QLineEdit(this);
    destBaseLayout->addWidget(m_destinationBaseEdit);
    destInfoLayout->addLayout(destBaseLayout);

    auto* destPassLayout = new QHBoxLayout();
    destPassLayout->addWidget(new QLabel(tr("Passphrase:"), this));
    m_destinationPassphraseEdit = new QLineEdit(this);
    m_destinationPassphraseEdit->setEchoMode(QLineEdit::Password);
    destPassLayout->addWidget(m_destinationPassphraseEdit);
    destInfoLayout->addLayout(destPassLayout);

    m_startDestinationButton = new QPushButton(tr("Start Listening"), this);
    destInfoLayout->addWidget(m_startDestinationButton);

    auto* orchestratorGroup = new QGroupBox(tr("Orchestrator Connection"), destWidget);
    auto* orchestratorConnectionLayout = new QGridLayout(orchestratorGroup);
    orchestratorConnectionLayout->addWidget(new QLabel(tr("Host:"), this), 0, 0);
    m_orchestratorHostEdit = new QLineEdit(this);
    m_orchestratorHostEdit->setPlaceholderText(tr("192.168.1.10"));
    orchestratorConnectionLayout->addWidget(m_orchestratorHostEdit, 0, 1);
    orchestratorConnectionLayout->addWidget(new QLabel(tr("Port:"), this), 0, 2);
    m_orchestratorPortSpin = new QSpinBox(this);
    m_orchestratorPortSpin->setRange(1024, 65535);
    m_orchestratorPortSpin->setValue(54322);
    orchestratorConnectionLayout->addWidget(m_orchestratorPortSpin, 0, 3);
    m_autoApproveOrchestratorCheck = new QCheckBox(tr("Auto-approve assignments"), this);
    m_autoApproveOrchestratorCheck->setChecked(true);
    orchestratorConnectionLayout->addWidget(m_autoApproveOrchestratorCheck, 1, 0, 1, 3);
    m_connectOrchestratorButton = new QPushButton(tr("Connect"), this);
    orchestratorConnectionLayout->addWidget(m_connectOrchestratorButton, 1, 3);
    orchestratorGroup->setLayout(orchestratorConnectionLayout);
    destInfoLayout->addWidget(orchestratorGroup);

    m_applyRestoreCheck = new QCheckBox(tr("Apply restore into system profiles"), this);
    m_applyRestoreCheck->setChecked(true);
    destInfoLayout->addWidget(m_applyRestoreCheck);

    destInfoGroup->setLayout(destInfoLayout);
    destLayout->addWidget(destInfoGroup);

    auto* manifestGroup = new QGroupBox(tr("Incoming Manifest"), destWidget);
    auto* manifestLayout = new QVBoxLayout(manifestGroup);
    m_manifestText = new QTextEdit(this);
    m_manifestText->setReadOnly(true);
    manifestLayout->addWidget(m_manifestText);

    auto* approveLayout = new QHBoxLayout();
    m_approveButton = new QPushButton(tr("Approve Transfer"), this);
    m_approveButton->setEnabled(false);
    m_rejectButton = new QPushButton(tr("Reject"), this);
    approveLayout->addWidget(m_approveButton);
    approveLayout->addWidget(m_rejectButton);
    approveLayout->addStretch();
    manifestLayout->addLayout(approveLayout);

    manifestGroup->setLayout(manifestLayout);
    destLayout->addWidget(manifestGroup);

    auto* assignmentGroup = new QGroupBox(tr("Assignment Queue"), destWidget);
    auto* assignmentLayout = new QVBoxLayout(assignmentGroup);
    m_activeAssignmentLabel = new QLabel(tr("No active assignment"), this);
    assignmentLayout->addWidget(m_activeAssignmentLabel);
    m_assignmentBandwidthLabel = new QLabel(tr("Bandwidth limit: default"), this);
    assignmentLayout->addWidget(m_assignmentBandwidthLabel);
    m_assignmentQueueTable = new QTableWidget(0, 6, this);
    m_assignmentQueueTable->setHorizontalHeaderLabels({tr("Deployment"), tr("Job"), tr("User"), tr("Size"), tr("Priority"), tr("Bandwidth")});
    m_assignmentQueueTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    m_assignmentQueueTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    m_assignmentQueueTable->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    m_assignmentQueueTable->horizontalHeader()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
    m_assignmentQueueTable->horizontalHeader()->setSectionResizeMode(4, QHeaderView::ResizeToContents);
    m_assignmentQueueTable->horizontalHeader()->setSectionResizeMode(5, QHeaderView::Stretch);
    assignmentLayout->addWidget(m_assignmentQueueTable);

    m_assignmentStatusTable = new QTableWidget(0, 5, this);
    m_assignmentStatusTable->setHorizontalHeaderLabels({tr("Deployment"), tr("Job"), tr("User"), tr("Status"), tr("Last Event")});
    m_assignmentStatusTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    m_assignmentStatusTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    m_assignmentStatusTable->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    m_assignmentStatusTable->horizontalHeader()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
    m_assignmentStatusTable->horizontalHeader()->setSectionResizeMode(4, QHeaderView::Stretch);
    assignmentLayout->addWidget(m_assignmentStatusTable);
    assignmentGroup->setLayout(assignmentLayout);
    destLayout->addWidget(assignmentGroup);

    destWidget->setLayout(destLayout);

    // Orchestrator UI
    auto* orchestratorWidget = new QWidget(this);
    auto* orchestratorLayout = new QVBoxLayout(orchestratorWidget);

    auto* orchestratorServerGroup = new QGroupBox(tr("Orchestrator Server"), orchestratorWidget);
    auto* orchestratorServerLayout = new QHBoxLayout(orchestratorServerGroup);
    orchestratorServerLayout->addWidget(new QLabel(tr("Listen Port:"), this));
    m_orchestratorListenPortSpin = new QSpinBox(this);
    m_orchestratorListenPortSpin->setRange(1024, 65535);
    m_orchestratorListenPortSpin->setValue(54322);
    orchestratorServerLayout->addWidget(m_orchestratorListenPortSpin);
    m_orchestratorListenButton = new QPushButton(tr("Start Server"), this);
    orchestratorServerLayout->addWidget(m_orchestratorListenButton);
    m_orchestratorStatusLabel = new QLabel(tr("Stopped"), this);
    orchestratorServerLayout->addWidget(m_orchestratorStatusLabel, 1);
    orchestratorServerGroup->setLayout(orchestratorServerLayout);
    orchestratorLayout->addWidget(orchestratorServerGroup);

    auto* orchestratorSourcesGroup = new QGroupBox(tr("Source Profiles"), orchestratorWidget);
    auto* orchestratorSourcesLayout = new QVBoxLayout(orchestratorSourcesGroup);
    auto* orchestratorSourceHeader = new QHBoxLayout();
    m_orchestratorScanUsersButton = new QPushButton(tr("Scan Source Users"), this);
    orchestratorSourceHeader->addWidget(m_orchestratorScanUsersButton);
    orchestratorSourceHeader->addStretch();
    orchestratorSourcesLayout->addLayout(orchestratorSourceHeader);
    m_orchestratorUserTable = new QTableWidget(0, 3, this);
    m_orchestratorUserTable->setHorizontalHeaderLabels({"✓", tr("User"), tr("Size")});
    m_orchestratorUserTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    m_orchestratorUserTable->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    m_orchestratorUserTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_orchestratorUserTable->setSelectionMode(QAbstractItemView::SingleSelection);
    m_orchestratorUserTable->setDragEnabled(true);
    m_orchestratorUserTable->setDragDropMode(QAbstractItemView::DragOnly);
    orchestratorSourcesLayout->addWidget(m_orchestratorUserTable);
    orchestratorSourcesGroup->setLayout(orchestratorSourcesLayout);
    orchestratorLayout->addWidget(orchestratorSourcesGroup);

    auto* orchestratorDestGroup = new QGroupBox(tr("Destinations"), orchestratorWidget);
    auto* orchestratorDestLayout = new QVBoxLayout(orchestratorDestGroup);
    m_orchestratorDestTable = new QTableWidget(0, 9, this);
    m_orchestratorDestTable->setHorizontalHeaderLabels({"✓", tr("Host"), tr("IP"), tr("Status"), tr("Free Disk"), tr("CPU%"), tr("RAM%"), tr("Last Seen"), tr("Progress")});
    m_orchestratorDestTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    m_orchestratorDestTable->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    m_orchestratorDestTable->horizontalHeader()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
    m_orchestratorDestTable->horizontalHeader()->setSectionResizeMode(4, QHeaderView::ResizeToContents);
    m_orchestratorDestTable->horizontalHeader()->setSectionResizeMode(7, QHeaderView::ResizeToContents);
    m_orchestratorDestTable->horizontalHeader()->setSectionResizeMode(8, QHeaderView::ResizeToContents);
    m_orchestratorDestTable->horizontalHeader()->setSectionResizeMode(5, QHeaderView::ResizeToContents);
    m_orchestratorDestTable->horizontalHeader()->setSectionResizeMode(6, QHeaderView::ResizeToContents);
    m_orchestratorDestTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    m_orchestratorDestTable->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    m_orchestratorDestTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    m_orchestratorDestTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_orchestratorDestTable->setSelectionMode(QAbstractItemView::SingleSelection);
    m_orchestratorDestTable->setAcceptDrops(true);
    m_orchestratorDestTable->setDragDropMode(QAbstractItemView::DropOnly);
    m_orchestratorDestTable->setDropIndicatorShown(true);
    m_orchestratorDestTable->installEventFilter(this);
    orchestratorDestLayout->addWidget(m_orchestratorDestTable);
    orchestratorDestGroup->setLayout(orchestratorDestLayout);
    orchestratorLayout->addWidget(orchestratorDestGroup);

    auto* deploymentControlGroup = new QGroupBox(tr("Deployment Controls"), orchestratorWidget);
    auto* deploymentControlLayout = new QVBoxLayout(deploymentControlGroup);

    auto* mappingRow = new QHBoxLayout();
    mappingRow->addWidget(new QLabel(tr("Mapping Type:"), this));
    m_mappingTypeCombo = new QComboBox(this);
    m_mappingTypeCombo->addItems({tr("One-to-Many"), tr("Many-to-Many"), tr("Custom Mapping")});
    mappingRow->addWidget(m_mappingTypeCombo);
    mappingRow->addWidget(new QLabel(tr("Strategy:"), this));
    m_mappingStrategyCombo = new QComboBox(this);
    m_mappingStrategyCombo->addItems({tr("Largest Free"), tr("Round Robin")});
    mappingRow->addWidget(m_mappingStrategyCombo);
    deploymentControlLayout->addLayout(mappingRow);

    auto* concurrencyRow = new QHBoxLayout();
    concurrencyRow->addWidget(new QLabel(tr("Max Concurrent:"), this));
    m_maxConcurrentSpin = new QSpinBox(this);
    m_maxConcurrentSpin->setRange(1, 100);
    m_maxConcurrentSpin->setValue(10);
    concurrencyRow->addWidget(m_maxConcurrentSpin);
    concurrencyRow->addWidget(new QLabel(tr("Global BW (Mbps):"), this));
    m_globalBandwidthSpin = new QSpinBox(this);
    m_globalBandwidthSpin->setRange(0, 100000);
    concurrencyRow->addWidget(m_globalBandwidthSpin);
    concurrencyRow->addWidget(new QLabel(tr("Per-Job BW (Mbps):"), this));
    m_perJobBandwidthSpin = new QSpinBox(this);
    m_perJobBandwidthSpin->setRange(0, 100000);
    concurrencyRow->addWidget(m_perJobBandwidthSpin);
    deploymentControlLayout->addLayout(concurrencyRow);

    auto* templateRow = new QHBoxLayout();
    m_useTemplateCheck = new QCheckBox(tr("Use Loaded Template"), this);
    templateRow->addWidget(m_useTemplateCheck);
    m_templateStatusLabel = new QLabel(tr("No template loaded"), this);
    templateRow->addWidget(m_templateStatusLabel, 1);
    m_saveTemplateButton = new QPushButton(tr("Save Template"), this);
    templateRow->addWidget(m_saveTemplateButton);
    m_loadTemplateButton = new QPushButton(tr("Load Template"), this);
    templateRow->addWidget(m_loadTemplateButton);
    deploymentControlLayout->addLayout(templateRow);

    auto* actionRow = new QHBoxLayout();
    m_startDeploymentButton = new QPushButton(tr("Start Deployment"), this);
    m_pauseDeploymentButton = new QPushButton(tr("Pause"), this);
    m_resumeDeploymentButton = new QPushButton(tr("Resume"), this);
    m_cancelDeploymentButton = new QPushButton(tr("Cancel"), this);
    actionRow->addWidget(m_startDeploymentButton);
    actionRow->addWidget(m_pauseDeploymentButton);
    actionRow->addWidget(m_resumeDeploymentButton);
    actionRow->addWidget(m_cancelDeploymentButton);
    actionRow->addStretch();
    deploymentControlLayout->addLayout(actionRow);

    deploymentControlGroup->setLayout(deploymentControlLayout);
    orchestratorLayout->addWidget(deploymentControlGroup);

    auto* customRulesGroup = new QGroupBox(tr("Custom Mapping Rules"), orchestratorWidget);
    auto* customRulesLayout = new QVBoxLayout(customRulesGroup);
    m_customRulesTable = new QTableWidget(0, 2, this);
    m_customRulesTable->setHorizontalHeaderLabels({tr("Source User"), tr("Destination ID")});
    m_customRulesTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_customRulesTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    customRulesLayout->addWidget(m_customRulesTable);
    customRulesGroup->setLayout(customRulesLayout);
    orchestratorLayout->addWidget(customRulesGroup);

    auto* jobsGroup = new QGroupBox(tr("Deployment Jobs"), orchestratorWidget);
    auto* jobsLayout = new QVBoxLayout(jobsGroup);
    m_jobsTable = new QTableWidget(0, 7, this);
    m_jobsTable->setHorizontalHeaderLabels({tr("Job ID"), tr("Deployment"), tr("Source User"), tr("Destination"), tr("Status"), tr("Progress"), tr("Error")});
    m_jobsTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    m_jobsTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    m_jobsTable->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    m_jobsTable->horizontalHeader()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
    m_jobsTable->horizontalHeader()->setSectionResizeMode(4, QHeaderView::ResizeToContents);
    m_jobsTable->horizontalHeader()->setSectionResizeMode(5, QHeaderView::ResizeToContents);
    m_jobsTable->horizontalHeader()->setSectionResizeMode(6, QHeaderView::Stretch);
    jobsLayout->addWidget(m_jobsTable);
    auto* jobActionRow = new QHBoxLayout();
    m_pauseJobButton = new QPushButton(tr("Pause Job"), this);
    m_resumeJobButton = new QPushButton(tr("Resume Job"), this);
    m_retryJobButton = new QPushButton(tr("Retry Job"), this);
    m_cancelJobButton = new QPushButton(tr("Cancel Job"), this);
    jobActionRow->addWidget(m_pauseJobButton);
    jobActionRow->addWidget(m_resumeJobButton);
    jobActionRow->addWidget(m_retryJobButton);
    jobActionRow->addWidget(m_cancelJobButton);
    jobActionRow->addStretch();
    jobsLayout->addLayout(jobActionRow);
    jobsGroup->setLayout(jobsLayout);
    orchestratorLayout->addWidget(jobsGroup);

    auto* deploymentProgressGroup = new QGroupBox(tr("Deployment Progress"), orchestratorWidget);
    auto* deploymentProgressLayout = new QVBoxLayout(deploymentProgressGroup);
    m_deploymentSummaryLabel = new QLabel(tr("0 of 0 complete"), this);
    deploymentProgressLayout->addWidget(m_deploymentSummaryLabel);
    m_deploymentProgressBar = new QProgressBar(this);
    m_deploymentProgressBar->setMinimum(0);
    m_deploymentProgressBar->setMaximum(100);
    deploymentProgressLayout->addWidget(m_deploymentProgressBar);
    m_deploymentEtaLabel = new QLabel(tr("ETA: --"), this);
    deploymentProgressLayout->addWidget(m_deploymentEtaLabel);
    m_exportHistoryButton = new QPushButton(tr("Export History CSV"), this);
    deploymentProgressLayout->addWidget(m_exportHistoryButton);
    auto* summaryExportRow = new QHBoxLayout();
    m_exportSummaryCsvButton = new QPushButton(tr("Export Summary CSV"), this);
    m_exportSummaryPdfButton = new QPushButton(tr("Export Summary PDF"), this);
    summaryExportRow->addWidget(m_exportSummaryCsvButton);
    summaryExportRow->addWidget(m_exportSummaryPdfButton);
    summaryExportRow->addStretch();
    deploymentProgressLayout->addLayout(summaryExportRow);
    m_recoverDeploymentButton = new QPushButton(tr("Recover Last Deployment"), this);
    deploymentProgressLayout->addWidget(m_recoverDeploymentButton);
    deploymentProgressGroup->setLayout(deploymentProgressLayout);
    orchestratorLayout->addWidget(deploymentProgressGroup);

    auto* historyGroup = new QGroupBox(tr("Deployment History"), orchestratorWidget);
    auto* historyLayout = new QVBoxLayout(historyGroup);
    m_historyTable = new QTableWidget(0, 7, this);
    m_historyTable->setHorizontalHeaderLabels({tr("Deployment"), tr("Started"), tr("Completed"), tr("Total"), tr("Completed"), tr("Failed"), tr("Status")});
    m_historyTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    m_historyTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    m_historyTable->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    m_historyTable->horizontalHeader()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
    m_historyTable->horizontalHeader()->setSectionResizeMode(4, QHeaderView::ResizeToContents);
    m_historyTable->horizontalHeader()->setSectionResizeMode(5, QHeaderView::ResizeToContents);
    m_historyTable->horizontalHeader()->setSectionResizeMode(6, QHeaderView::Stretch);
    historyLayout->addWidget(m_historyTable);
    historyGroup->setLayout(historyLayout);
    orchestratorLayout->addWidget(historyGroup);

    auto* legendGroup = new QGroupBox(tr("Status Legend"), orchestratorWidget);
    auto* legendLayout = new QHBoxLayout(legendGroup);
    auto* okLabel = new QLabel(tr("Success"), this);
        okLabel->setStyleSheet("QLabel { background-color: #16a34a; color: white; padding: 6px 10px; border-radius: 10px; }");
    auto* warnLabel = new QLabel(tr("In Progress"), this);
        warnLabel->setStyleSheet("QLabel { background-color: #f59e0b; color: #1e293b; padding: 6px 10px; border-radius: 10px; }");
    auto* errLabel = new QLabel(tr("Error"), this);
        errLabel->setStyleSheet("QLabel { background-color: #dc2626; color: white; padding: 6px 10px; border-radius: 10px; }");
    auto* idleLabel = new QLabel(tr("Idle"), this);
        idleLabel->setStyleSheet("QLabel { background-color: #64748b; color: white; padding: 6px 10px; border-radius: 10px; }");
    legendLayout->addWidget(okLabel);
    legendLayout->addWidget(warnLabel);
    legendLayout->addWidget(errLabel);
    legendLayout->addWidget(idleLabel);
    legendLayout->addStretch();
    legendGroup->setLayout(legendLayout);
    orchestratorLayout->addWidget(legendGroup);

    orchestratorWidget->setLayout(orchestratorLayout);

    m_modeStack->addWidget(wrapScrollable(sourceWidget));
    m_modeStack->addWidget(wrapScrollable(destWidget));
    m_modeStack->addWidget(wrapScrollable(orchestratorWidget));

    mainLayout->addWidget(m_modeStack, 1);

    m_overallProgress = new QProgressBar(this);
    m_overallProgress->setMinimum(0);
    m_overallProgress->setMaximum(100);
    mainLayout->addWidget(m_overallProgress);

    m_stopTransferButton = new QPushButton(tr("Stop Transfer"), this);
    mainLayout->addWidget(m_stopTransferButton);

    m_logText = new QTextEdit(this);
    m_logText->setReadOnly(true);
    m_logText->setMaximumHeight(140);
    mainLayout->addWidget(m_logText);
}

void NetworkTransferPanel::setupConnections() {
    connect(m_modeCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, &NetworkTransferPanel::onModeChanged);
    connect(m_scanUsersButton, &QPushButton::clicked, this, &NetworkTransferPanel::onScanUsers);
    connect(m_customizeUserButton, &QPushButton::clicked, this, &NetworkTransferPanel::onCustomizeUser);
    connect(m_discoverPeersButton, &QPushButton::clicked, this, &NetworkTransferPanel::onDiscoverPeers);
    connect(m_startSourceButton, &QPushButton::clicked, this, &NetworkTransferPanel::onStartSource);
    connect(m_stopTransferButton, &QPushButton::clicked, this, [this]() {
        m_controller->stop();
        m_logText->append(tr("Transfer stopped by user."));
    });

    connect(m_startDestinationButton, &QPushButton::clicked, this, &NetworkTransferPanel::onStartDestination);
    connect(m_connectOrchestratorButton, &QPushButton::clicked, this, &NetworkTransferPanel::onConnectOrchestrator);
    connect(m_approveButton, &QPushButton::clicked, this, &NetworkTransferPanel::onApproveTransfer);
    connect(m_rejectButton, &QPushButton::clicked, this, &NetworkTransferPanel::onRejectTransfer);
    connect(m_orchestratorListenButton, &QPushButton::clicked, this, &NetworkTransferPanel::onStartOrchestratorServer);
    connect(m_orchestratorScanUsersButton, &QPushButton::clicked, this, &NetworkTransferPanel::onScanOrchestratorUsers);
    connect(m_startDeploymentButton, &QPushButton::clicked, this, &NetworkTransferPanel::onStartDeployment);
    connect(m_pauseDeploymentButton, &QPushButton::clicked, this, &NetworkTransferPanel::onPauseDeployment);
    connect(m_resumeDeploymentButton, &QPushButton::clicked, this, &NetworkTransferPanel::onResumeDeployment);
    connect(m_cancelDeploymentButton, &QPushButton::clicked, this, &NetworkTransferPanel::onCancelDeployment);
    connect(m_saveTemplateButton, &QPushButton::clicked, this, &NetworkTransferPanel::onSaveDeploymentTemplate);
    connect(m_loadTemplateButton, &QPushButton::clicked, this, &NetworkTransferPanel::onLoadDeploymentTemplate);
    connect(m_pauseJobButton, &QPushButton::clicked, this, &NetworkTransferPanel::onPauseJob);
    connect(m_resumeJobButton, &QPushButton::clicked, this, &NetworkTransferPanel::onResumeJob);
    connect(m_retryJobButton, &QPushButton::clicked, this, &NetworkTransferPanel::onRetryJob);
    connect(m_cancelJobButton, &QPushButton::clicked, this, &NetworkTransferPanel::onCancelJob);
    connect(m_exportHistoryButton, &QPushButton::clicked, this, &NetworkTransferPanel::onExportDeploymentHistory);
    connect(m_exportSummaryCsvButton, &QPushButton::clicked, this, &NetworkTransferPanel::onExportDeploymentSummaryCsv);
    connect(m_exportSummaryPdfButton, &QPushButton::clicked, this, &NetworkTransferPanel::onExportDeploymentSummaryPdf);
    connect(m_recoverDeploymentButton, &QPushButton::clicked, this, &NetworkTransferPanel::onRecoverLastDeployment);
        connect(m_controller, &NetworkTransferController::orchestrationAssignmentReceived,
            this, &NetworkTransferPanel::onOrchestrationAssignment);

    connect(m_controller, &NetworkTransferController::peerDiscovered, this, &NetworkTransferPanel::onPeerDiscovered);
    connect(m_controller, &NetworkTransferController::manifestReceived, this, &NetworkTransferPanel::onManifestReceived);
    connect(m_controller, &NetworkTransferController::transferProgress, this, &NetworkTransferPanel::onTransferProgress);
    connect(m_controller, &NetworkTransferController::transferCompleted, this, &NetworkTransferPanel::onTransferCompleted);
    connect(m_controller, &NetworkTransferController::orchestrationAssignmentPaused, this, [this](const QString& job_id) {
        const QString key = job_id.isEmpty() ? m_activeAssignment.job_id : job_id;
        if (!key.isEmpty()) {
            m_assignmentStatusByJob[key] = tr("paused");
            m_assignmentEventByJob[key] = tr("Paused by orchestrator");
        }
        m_destinationTransferActive = false;
        refreshAssignmentStatus();
        persistAssignmentQueue();
    });
    connect(m_controller, &NetworkTransferController::orchestrationAssignmentResumed, this, [this](const QString& job_id) {
        const QString key = job_id.isEmpty() ? m_activeAssignment.job_id : job_id;
        if (!key.isEmpty()) {
            m_assignmentStatusByJob[key] = tr("active");
            m_assignmentEventByJob[key] = tr("Resumed by orchestrator");
        }
        refreshAssignmentStatus();
        persistAssignmentQueue();
    });
    connect(m_controller, &NetworkTransferController::orchestrationAssignmentCanceled, this, [this](const QString& job_id) {
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
    connect(m_controller, &NetworkTransferController::statusMessage, this, [this](const QString& msg) {
        m_logText->append(msg);
        Q_EMIT statusMessage(msg, 5000);
    });
    connect(m_controller, &NetworkTransferController::errorMessage, this, [this](const QString& msg) {
        m_logText->append(tr("ERROR: %1").arg(msg));
        m_transferErrors.append(msg);
        Q_EMIT statusMessage(msg, 5000);
    });

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
        connect(m_orchestrator, &MigrationOrchestrator::orchestratorStatus, this, [this](const QString& msg) {
            m_logText->append(msg);
            Q_EMIT statusMessage(msg, 5000);
        });
        }
        if (m_parallelManager) {
        connect(m_parallelManager, &ParallelTransferManager::jobStartRequested,
            this, &NetworkTransferPanel::onJobStartRequested);
        connect(m_parallelManager, &ParallelTransferManager::jobUpdated,
            this, &NetworkTransferPanel::onJobUpdated);
        connect(m_parallelManager, &ParallelTransferManager::jobCompleted,
            this, &NetworkTransferPanel::onJobCompleted);
            connect(m_parallelManager, &ParallelTransferManager::deploymentProgress,
                this, &NetworkTransferPanel::onParallelDeploymentProgress);
            connect(m_parallelManager, &ParallelTransferManager::deploymentComplete,
                this, &NetworkTransferPanel::onParallelDeploymentCompleted);
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

                    if (m_orchestrator) {
                        const QString destination_id = m_jobToDestinationId.value(job_id);
                        const QString deployment_id = m_jobToDeploymentId.value(job_id);
                        if (!destination_id.isEmpty()) {
                            m_orchestrator->pauseAssignment(destination_id, deployment_id, job_id);
                        }
                    }
                });
            connect(m_parallelManager, &ParallelTransferManager::jobResumeRequested,
                this, [this](const QString& job_id) {
                    if (m_jobSourceControllers.contains(job_id)) {
                        m_jobSourceControllers.value(job_id)->resumeTransfer();
                    }

                    if (m_orchestrator) {
                        const QString destination_id = m_jobToDestinationId.value(job_id);
                        const QString deployment_id = m_jobToDeploymentId.value(job_id);
                        if (!destination_id.isEmpty()) {
                            m_orchestrator->resumeAssignment(destination_id, deployment_id, job_id);
                        }
                    }
                });
            connect(m_parallelManager, &ParallelTransferManager::jobCancelRequested,
                this, [this](const QString& job_id) {
                    if (m_jobSourceControllers.contains(job_id)) {
                        auto* controller = m_jobSourceControllers.take(job_id);
                        controller->cancelTransfer();
                        controller->deleteLater();
                    }

                    if (m_orchestrator) {
                        const QString destination_id = m_jobToDestinationId.value(job_id);
                        const QString deployment_id = m_jobToDeploymentId.value(job_id);
                        if (!destination_id.isEmpty()) {
                            m_orchestrator->cancelAssignment(destination_id, deployment_id, job_id);
                        }
                    }
                });
        }
}

void NetworkTransferPanel::loadSettings() {
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
    m_chunkSizeSpin->setValue(m_settings.chunk_size / 1024);
    m_bandwidthSpin->setValue(m_settings.max_bandwidth_kbps);
    m_permissionModeCombo->setCurrentIndex(0);

    m_mappingTypeCombo->setCurrentIndex(config.getValue("orchestration/mapping_type", 0).toInt());
    m_mappingStrategyCombo->setCurrentIndex(config.getValue("orchestration/mapping_strategy", 0).toInt());
    m_maxConcurrentSpin->setValue(config.getValue("orchestration/max_concurrent", 10).toInt());
    m_globalBandwidthSpin->setValue(config.getValue("orchestration/global_bandwidth", 0).toInt());
    m_perJobBandwidthSpin->setValue(config.getValue("orchestration/per_job_bandwidth", 0).toInt());
    m_useTemplateCheck->setChecked(config.getValue("orchestration/use_template", false).toBool());

    m_controller->configure(m_settings);

    QString defaultBase = QDir::toNativeSeparators(QDir::rootPath() + "Users");
    QString stagingBase = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation) + "/SAK/Incoming";
    m_destinationBaseEdit->setText(QDir::toNativeSeparators(stagingBase));

    QStringList addresses;
    for (const auto& iface : QNetworkInterface::allInterfaces()) {
        for (const auto& entry : iface.addressEntries()) {
            if (entry.ip().protocol() == QAbstractSocket::IPv4Protocol && !entry.ip().isLoopback()) {
                addresses.append(entry.ip().toString());
            }
        }
    }
    m_destinationInfo->setText(tr("Listening on ports %1/%2. Local IPs: %3")
                                   .arg(m_settings.control_port)
                                   .arg(m_settings.data_port)
                                   .arg(addresses.join(", ")));

    if (!m_historyManager) {
        QString historyPath = config.getValue("orchestration/history_path").toString();
        if (historyPath.isEmpty()) {
            const QString historyDir = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation) + "/SAK";
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

    if (!m_assignmentQueueStore) {
        QString queuePath = config.getValue("orchestration/assignment_queue_path").toString();
        if (queuePath.isEmpty()) {
            const QString queueDir = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation) + "/SAK";
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
            if (!m_activeAssignment.deployment_id.isEmpty()) {
                if (m_activeAssignmentLabel) {
                    m_activeAssignmentLabel->setText(tr("Active: %1 (%2)")
                                                         .arg(m_activeAssignment.source_user, m_activeAssignment.deployment_id));
                }
            }
            refreshAssignmentQueue();
            refreshAssignmentStatus();
        }
    }

    const QString lastTemplatePath = config.getValue("orchestration/last_template_path").toString();
    if (!lastTemplatePath.isEmpty() && QFileInfo::exists(lastTemplatePath)) {
        m_loadedMapping = m_mappingEngine->loadTemplate(lastTemplatePath);
        if (!m_loadedMapping.sources.isEmpty()) {
            m_loadedTemplatePath = lastTemplatePath;
            m_templateStatusLabel->setText(tr("Loaded template: %1").arg(QFileInfo(lastTemplatePath).fileName()));
        }
    }

    m_activeDeploymentId = config.getValue("orchestration/last_deployment_id").toString();
    const auto started = config.getValue("orchestration/last_deployment_started").toString();
    if (!started.isEmpty()) {
        m_deploymentStartedAt = QDateTime::fromString(started, Qt::ISODate);
    }

    if (!m_activeDeploymentId.isEmpty()) {
        m_logText->append(tr("Last deployment: %1").arg(m_activeDeploymentId));
    }

    refreshDeploymentHistory();
}

void NetworkTransferPanel::onModeChanged(int index) {
    m_modeStack->setCurrentIndex(index);
    m_controller->stopDiscovery();
}

void NetworkTransferPanel::onScanUsers() {
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
        m_userTable->setItem(i, kUserColSize, new QTableWidgetItem(formatBytes(user.total_size_estimated)));
    }

    m_logText->append(tr("Scanned %1 users").arg(m_users.size()));
}

void NetworkTransferPanel::onCustomizeUser() {
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

void NetworkTransferPanel::onDiscoverPeers() {
    m_peers.clear();
    m_peerTable->setRowCount(0);
    m_controller->configure(m_settings);
    if (!m_settings.auto_discovery_enabled) {
        QMessageBox::information(this, tr("Discovery Disabled"), tr("Enable auto discovery in settings to find peers."));
        return;
    }
    m_controller->startDiscovery("source");
    m_logText->append(tr("Peer discovery started"));
}

void NetworkTransferPanel::onStartSource() {
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
        QMessageBox::warning(this, tr("Missing Destination"), tr("Select a peer or enter a manual IP."));
        return;
    }

    m_settings.control_port = static_cast<quint16>(m_manualPortSpin->value());
    peer.control_port = static_cast<quint16>(m_manualPortSpin->value());
    peer.data_port = m_settings.data_port;
    peer.hostname = peer.ip_address;

    m_settings.encryption_enabled = m_encryptCheck->isChecked();
    m_settings.compression_enabled = m_compressCheck->isChecked();
    m_settings.resume_enabled = m_resumeCheck->isChecked();
    m_settings.chunk_size = m_chunkSizeSpin->value() * 1024;
    m_settings.max_bandwidth_kbps = m_bandwidthSpin->value();

    if (m_settings.encryption_enabled && m_passphraseEdit->text().isEmpty()) {
        QMessageBox::warning(this, tr("Missing Passphrase"), tr("Enter a passphrase for encrypted transfers."));
        return;
    }

    if (m_settings.encryption_enabled && m_passphraseEdit->text().size() < 8) {
        QMessageBox::warning(this, tr("Weak Passphrase"), tr("Passphrase must be at least 8 characters."));
        return;
    }

    m_transferStarted = QDateTime::currentDateTime();
    m_transferErrors.clear();
    m_isSourceTransfer = true;

    m_controller->configure(m_settings);
    m_controller->startSource(m_currentManifest, m_currentFiles, peer, m_passphraseEdit->text());
}

void NetworkTransferPanel::onStartDestination() {
    m_settings.encryption_enabled = m_encryptCheck->isChecked();
    m_settings.compression_enabled = m_compressCheck->isChecked();
    m_settings.resume_enabled = m_resumeCheck->isChecked();
    m_settings.chunk_size = m_chunkSizeSpin->value() * 1024;
    m_settings.max_bandwidth_kbps = m_bandwidthSpin->value();

    if (m_settings.encryption_enabled && m_destinationPassphraseEdit->text().isEmpty()) {
        QMessageBox::warning(this, tr("Missing Passphrase"), tr("Enter a passphrase for encrypted transfers."));
        return;
    }

    if (m_settings.encryption_enabled && m_destinationPassphraseEdit->text().size() < 8) {
        QMessageBox::warning(this, tr("Weak Passphrase"), tr("Passphrase must be at least 8 characters."));
        return;
    }

    const QString base = destinationBase();
    if (base.isEmpty()) {
        QMessageBox::warning(this, tr("Missing Destination"), tr("Set a destination base path."));
        return;
    }

    QDir destDir(base);
    if (!destDir.exists() && !destDir.mkpath(".")) {
        QMessageBox::warning(this, tr("Destination Error"), tr("Failed to create destination base directory."));
        return;
    }

    m_transferStarted = QDateTime::currentDateTime();
    m_transferErrors.clear();
    m_isSourceTransfer = false;
    m_controller->configure(m_settings);
    m_controller->startDestination(m_destinationPassphraseEdit->text(), destinationBase());
}

void NetworkTransferPanel::onConnectOrchestrator() {
    const QString host = m_orchestratorHostEdit->text().trimmed();
    if (host.isEmpty()) {
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
    m_logText->append(tr("Connecting to orchestrator at %1:%2")
                          .arg(host)
                          .arg(m_orchestratorPortSpin->value()));
}

void NetworkTransferPanel::onOrchestrationAssignment(const DeploymentAssignment& assignment) {
    if (!assignment.job_id.isEmpty()) {
        m_assignmentStatusByJob[assignment.job_id] = tr("queued");
        m_assignmentEventByJob[assignment.job_id] = tr("Received assignment");
    }

    if (m_destinationTransferActive || !m_activeAssignment.deployment_id.isEmpty()) {
        m_assignmentQueue.enqueue(assignment);
        refreshAssignmentQueue();
        refreshAssignmentStatus();
        persistAssignmentQueue();
        m_logText->append(tr("Queued assignment %1 for %2").arg(assignment.deployment_id, assignment.source_user));
        return;
    }

    activateAssignment(assignment);
}

void NetworkTransferPanel::onStartOrchestratorServer() {
    if (!m_orchestrator) {
        return;
    }

    if (!m_orchestratorServerRunning) {
        const auto port = static_cast<quint16>(m_orchestratorListenPortSpin->value());
        if (!m_orchestrator->startServer(port)) {
            QMessageBox::warning(this, tr("Orchestrator Error"), tr("Failed to start orchestration server."));
            return;
        }
        m_orchestrator->startHealthPolling(10000);
        m_orchestrator->startDiscovery(m_settings.discovery_port);
        m_orchestratorServerRunning = true;
        m_orchestratorListenButton->setText(tr("Stop Server"));
        m_orchestratorStatusLabel->setText(tr("Listening on %1").arg(port));
        m_logText->append(tr("Orchestrator server started on port %1").arg(port));
    } else {
        m_orchestrator->stopHealthPolling();
        m_orchestrator->stopDiscovery();
        m_orchestrator->stopServer();
        m_orchestratorServerRunning = false;
        m_orchestratorListenButton->setText(tr("Start Server"));
        m_orchestratorStatusLabel->setText(tr("Stopped"));
        m_logText->append(tr("Orchestrator server stopped"));
    }
}

void NetworkTransferPanel::onScanOrchestratorUsers() {
    m_users = m_userScanner->scanUsers();
    m_orchestratorUserTable->setRowCount(0);

    for (int i = 0; i < m_users.size(); ++i) {
        const auto& user = m_users[i];
        m_orchestratorUserTable->insertRow(i);

        auto* selectItem = new QTableWidgetItem();
        selectItem->setCheckState(Qt::Checked);
        m_orchestratorUserTable->setItem(i, 0, selectItem);

        auto* userItem = new QTableWidgetItem(user.username);
        userItem->setData(Qt::UserRole, i);
        m_orchestratorUserTable->setItem(i, 1, userItem);
        m_orchestratorUserTable->setItem(i, 2, new QTableWidgetItem(formatBytes(user.total_size_estimated)));
    }

    m_logText->append(tr("Scanned %1 users for deployment").arg(m_users.size()));
}

void NetworkTransferPanel::onStartDeployment() {
    if (!m_parallelManager || !m_orchestrator) {
        return;
    }

    auto mapping = buildDeploymentMapping();
    if (mapping.sources.isEmpty() || mapping.destinations.isEmpty()) {
        QMessageBox::warning(this, tr("Deployment Error"), tr("Select source profiles and destinations first."));
        return;
    }

    if (mapping.deployment_id.isEmpty()) {
        mapping.deployment_id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    }

    QString validationError;
    if (!m_mappingEngine->validateMapping(mapping, validationError)) {
        QMessageBox::warning(this, tr("Deployment Error"), validationError);
        return;
    }

    if (!m_mappingEngine->checkDestinationReadiness(mapping)) {
        QMessageBox::warning(this, tr("Deployment Error"), tr("One or more destinations are not ready."));
        return;
    }

    if (!m_mappingEngine->checkDiskSpace(mapping)) {
        QMessageBox::warning(this, tr("Deployment Error"), tr("Insufficient disk space on one or more destinations."));
        return;
    }

    m_activeDeploymentId = mapping.deployment_id;
    m_deploymentStartedAt = QDateTime::currentDateTimeUtc();

    auto& config = ConfigManager::instance();
    config.setValue("orchestration/last_deployment_id", m_activeDeploymentId);
    config.setValue("orchestration/last_deployment_started", m_deploymentStartedAt.toString(Qt::ISODate));
    config.setValue("orchestration/mapping_type", m_mappingTypeCombo->currentIndex());
    config.setValue("orchestration/mapping_strategy", m_mappingStrategyCombo->currentIndex());
    config.setValue("orchestration/max_concurrent", m_maxConcurrentSpin->value());
    config.setValue("orchestration/global_bandwidth", m_globalBandwidthSpin->value());
    config.setValue("orchestration/per_job_bandwidth", m_perJobBandwidthSpin->value());
    config.setValue("orchestration/use_template", m_useTemplateCheck->isChecked());
    m_orchestrator->setMappingStrategy(m_mappingStrategyCombo->currentIndex() == 0
                                           ? MappingEngine::Strategy::LargestFree
                                           : MappingEngine::Strategy::RoundRobin);
    m_parallelManager->setMaxConcurrentTransfers(m_maxConcurrentSpin->value());
    m_parallelManager->setGlobalBandwidthLimit(m_globalBandwidthSpin->value());
    m_parallelManager->setPerJobBandwidthLimit(m_perJobBandwidthSpin->value());

    m_destinationToJobId.clear();
    m_jobToDestinationId.clear();
    m_jobToDeploymentId.clear();
    m_knownJobIds.clear();

    m_parallelManager->startDeployment(mapping);
    m_logText->append(tr("Deployment %1 started").arg(mapping.deployment_id));
}

void NetworkTransferPanel::onPauseDeployment() {
    if (m_parallelManager) {
        m_parallelManager->pauseDeployment();
    }
}

void NetworkTransferPanel::onResumeDeployment() {
    if (m_parallelManager) {
        m_parallelManager->resumeDeployment();
    }
}

void NetworkTransferPanel::onCancelDeployment() {
    if (m_parallelManager) {
        m_parallelManager->cancelDeployment();
    }
}

void NetworkTransferPanel::onSaveDeploymentTemplate() {
    auto mapping = buildDeploymentMapping();
    if (mapping.sources.isEmpty() || mapping.destinations.isEmpty()) {
        QMessageBox::warning(this, tr("Template Error"), tr("Select sources and destinations first."));
        return;
    }

    const QString filePath = QFileDialog::getSaveFileName(this, tr("Save Template"),
                                                          QDir::homePath(), tr("JSON Files (*.json)"));
    if (filePath.isEmpty()) {
        return;
    }

    if (!m_mappingEngine->saveTemplate(mapping, filePath)) {
        QMessageBox::warning(this, tr("Template Error"), tr("Failed to save template."));
        return;
    }

    m_loadedTemplatePath = filePath;
    m_templateStatusLabel->setText(tr("Template saved: %1").arg(QFileInfo(filePath).fileName()));
    ConfigManager::instance().setValue("orchestration/last_template_path", filePath);
}

void NetworkTransferPanel::onLoadDeploymentTemplate() {
    const QString filePath = QFileDialog::getOpenFileName(this, tr("Load Template"),
                                                          QDir::homePath(), tr("JSON Files (*.json)"));
    if (filePath.isEmpty()) {
        return;
    }

    m_loadedMapping = m_mappingEngine->loadTemplate(filePath);
    if (m_loadedMapping.sources.isEmpty() || m_loadedMapping.destinations.isEmpty()) {
        QMessageBox::warning(this, tr("Template Error"), tr("Template is invalid or empty."));
        return;
    }

    m_loadedTemplatePath = filePath;
    m_templateStatusLabel->setText(tr("Loaded template: %1").arg(QFileInfo(filePath).fileName()));
    m_useTemplateCheck->setChecked(true);
    ConfigManager::instance().setValue("orchestration/last_template_path", filePath);
}

void NetworkTransferPanel::onOrchestratorDestinationRegistered(const DestinationPC& destination) {
    if (!destination.destination_id.isEmpty()) {
        m_destinationStatusHistory[destination.destination_id].append(tr("Registered"));
    }
    refreshOrchestratorDestinations();
}

void NetworkTransferPanel::onOrchestratorDestinationUpdated(const DestinationPC& destination) {
    if (!destination.destination_id.isEmpty()) {
        m_destinationStatusHistory[destination.destination_id].append(tr("Updated: %1").arg(destination.status));
    }
    refreshOrchestratorDestinations();
}

void NetworkTransferPanel::onOrchestratorDestinationRemoved(const QString& destination_id) {
    if (!destination_id.isEmpty()) {
        m_destinationStatusHistory[destination_id].append(tr("Removed"));
    }
    m_destinationProgress.remove(destination_id);
    refreshOrchestratorDestinations();
}

void NetworkTransferPanel::onOrchestratorProgress(const DeploymentProgress& progress) {
    if (!progress.destination_id.isEmpty()) {
        m_destinationProgress.insert(progress.destination_id, progress.progress_percent);
        m_destinationStatusHistory[progress.destination_id].append(tr("Progress %1%").arg(progress.progress_percent));
    }

    QString jobId = progress.job_id;
    if (jobId.isEmpty()) {
        jobId = m_destinationToJobId.value(progress.destination_id);
    }

    if (!jobId.isEmpty() && m_parallelManager) {
        m_parallelManager->updateJobProgress(jobId,
                                             progress.progress_percent,
                                             progress.bytes_transferred,
                                             progress.bytes_total,
                                             progress.transfer_speed_mbps,
                                             progress.current_file);
    }

    refreshOrchestratorDestinations();
}

void NetworkTransferPanel::onOrchestratorCompletion(const DeploymentCompletion& completion) {
    if (!completion.destination_id.isEmpty()) {
        m_destinationStatusHistory[completion.destination_id].append(tr("Completed: %1").arg(completion.status));
    }
    QString jobId = completion.job_id;
    if (jobId.isEmpty()) {
        jobId = m_destinationToJobId.value(completion.destination_id);
    }
    if (!jobId.isEmpty() && m_parallelManager) {
        const bool success = completion.status == "success";
        m_parallelManager->markJobComplete(jobId, success, success ? QString() : completion.status);
    }
    refreshOrchestratorDestinations();
}

void NetworkTransferPanel::onJobStartRequested(const QString& job_id,
                                               const MappingEngine::SourceProfile& source,
                                               const DestinationPC& destination) {
    if (!m_orchestrator) {
        return;
    }

    DeploymentAssignment assignment;
    assignment.deployment_id = m_activeDeploymentId;
    assignment.job_id = job_id;
    assignment.source_user = source.username;
    assignment.profile_size_bytes = source.profile_size_bytes;
    assignment.priority = "normal";
    if (m_perJobBandwidthSpin && m_perJobBandwidthSpin->value() > 0) {
        assignment.max_bandwidth_kbps = m_perJobBandwidthSpin->value() * 1024;
    }

    m_destinationToJobId.insert(destination.destination_id, job_id);
    m_jobToDestinationId.insert(job_id, destination.destination_id);
    m_jobToDeploymentId.insert(job_id, assignment.deployment_id);
    m_knownJobIds.insert(job_id);
    if (!destination.destination_id.isEmpty()) {
        m_destinationStatusHistory[destination.destination_id].append(tr("Job started: %1").arg(job_id));
    }

    m_orchestrator->assignDeploymentToDestination(destination.destination_id,
                                                  assignment,
                                                  assignment.profile_size_bytes);

    const auto it = std::find_if(m_users.begin(), m_users.end(), [&source](const UserProfile& user) {
        return user.username == source.username;
    });

    if (it == m_users.end()) {
        if (m_parallelManager) {
            m_parallelManager->markJobComplete(job_id, false, tr("Source user not found"));
        }
        refreshJobsTable();
        return;
    }

    if (m_settings.encryption_enabled && m_passphraseEdit->text().isEmpty()) {
        if (m_parallelManager) {
            m_parallelManager->markJobComplete(job_id, false, tr("Missing passphrase for encrypted transfer"));
        }
        refreshJobsTable();
        return;
    }

    QVector<UserProfile> selectedUsers{*it};
    const auto files = buildFileListForUsers(selectedUsers);
    const auto manifest = buildManifestPayloadForUsers(files, selectedUsers);

    TransferPeerInfo peer;
    peer.ip_address = destination.ip_address;
    peer.control_port = destination.control_port;
    peer.data_port = destination.data_port;
    peer.hostname = destination.hostname;

    auto* controller = new NetworkTransferController(this);
    TransferSettings settings = m_settings;
    settings.control_port = destination.control_port;
    settings.data_port = destination.data_port;
    settings.max_bandwidth_kbps = assignment.max_bandwidth_kbps > 0
        ? assignment.max_bandwidth_kbps
        : settings.max_bandwidth_kbps;
    controller->configure(settings);

    connect(controller, &NetworkTransferController::transferCompleted, this,
            [this, job_id, controller](bool success, const QString& message) {
                if (!success && m_parallelManager) {
                    m_parallelManager->markJobComplete(job_id, false, message);
                    refreshJobsTable();
                }
                controller->deleteLater();
                m_jobSourceControllers.remove(job_id);
            });

    m_jobSourceControllers.insert(job_id, controller);
    controller->startSource(manifest, files, peer, m_passphraseEdit->text());

    refreshJobsTable();
}

void NetworkTransferPanel::onJobUpdated(const QString& job_id, int progress_percent) {
    Q_UNUSED(progress_percent);
    m_knownJobIds.insert(job_id);
    refreshJobsTable();
}

void NetworkTransferPanel::onJobCompleted(const QString& job_id, bool success, const QString& error_message) {
    Q_UNUSED(success);
    Q_UNUSED(error_message);
    m_knownJobIds.insert(job_id);
    refreshJobsTable();
}

void NetworkTransferPanel::onAggregateProgress(int completed, int total, int percent) {
    if (m_deploymentSummaryLabel) {
        m_deploymentSummaryLabel->setText(tr("%1 of %2 complete").arg(completed).arg(total));
    }
    if (m_deploymentProgressBar) {
        m_deploymentProgressBar->setValue(percent);
    }
}

void NetworkTransferPanel::onParallelDeploymentProgress(int completed, int total) {
    const int percent = total > 0 ? static_cast<int>((completed * 100) / total) : 0;
    onAggregateProgress(completed, total, percent);
}

void NetworkTransferPanel::onPauseJob() {
    if (!m_jobsTable || !m_parallelManager) {
        return;
    }

    const int row = m_jobsTable->currentRow();
    if (row < 0) {
        return;
    }

    auto* jobItem = m_jobsTable->item(row, 0);
    if (!jobItem) {
        return;
    }

    m_parallelManager->pauseJob(jobItem->text());
    refreshJobsTable();
}

void NetworkTransferPanel::onResumeJob() {
    if (!m_jobsTable || !m_parallelManager) {
        return;
    }

    const int row = m_jobsTable->currentRow();
    if (row < 0) {
        return;
    }

    auto* jobItem = m_jobsTable->item(row, 0);
    if (!jobItem) {
        return;
    }

    m_parallelManager->resumeJob(jobItem->text());
    refreshJobsTable();
}

void NetworkTransferPanel::onRetryJob() {
    if (!m_jobsTable || !m_parallelManager) {
        return;
    }

    const int row = m_jobsTable->currentRow();
    if (row < 0) {
        return;
    }

    auto* jobItem = m_jobsTable->item(row, 0);
    if (!jobItem) {
        return;
    }

    m_parallelManager->retryJob(jobItem->text());
    refreshJobsTable();
}

void NetworkTransferPanel::onCancelJob() {
    if (!m_jobsTable || !m_parallelManager) {
        return;
    }

    const int row = m_jobsTable->currentRow();
    if (row < 0) {
        return;
    }

    auto* jobItem = m_jobsTable->item(row, 0);
    if (!jobItem) {
        return;
    }

    m_parallelManager->cancelJob(jobItem->text());
    refreshJobsTable();
}

void NetworkTransferPanel::onExportDeploymentHistory() {
    if (!m_historyManager) {
        return;
    }

    const QString filePath = QFileDialog::getSaveFileName(this, tr("Export Deployment History"),
                                                          QDir::homePath(), tr("CSV Files (*.csv)"));
    if (filePath.isEmpty()) {
        return;
    }

    if (!m_historyManager->exportCsv(filePath)) {
        QMessageBox::warning(this, tr("Export Error"), tr("Failed to export deployment history."));
        return;
    }

    m_logText->append(tr("Deployment history exported to %1").arg(filePath));
}

void NetworkTransferPanel::onExportDeploymentSummaryCsv() {
    if (!m_parallelManager || !m_orchestrator || !m_orchestrator->registry()) {
        return;
    }

    const QString filePath = QFileDialog::getSaveFileName(this, tr("Export Deployment Summary"),
                                                          QDir::homePath(), tr("CSV Files (*.csv)"));
    if (filePath.isEmpty()) {
        return;
    }

    QVector<DeploymentJobSummary> jobs;
    for (const auto& job : m_parallelManager->allJobs()) {
        DeploymentJobSummary summary;
        summary.job_id = job.job_id;
        summary.source_user = job.source.username;
        summary.destination_id = job.destination.destination_id;
        summary.status = job.status;
        summary.bytes_transferred = job.bytes_transferred;
        summary.total_bytes = job.total_bytes;
        summary.error_message = job.error_message;
        jobs.push_back(summary);
    }

    QVector<DeploymentDestinationSummary> destinations;
    const auto registryDestinations = m_orchestrator->registry()->destinations();
    for (const auto& destination : registryDestinations) {
        DeploymentDestinationSummary summary;
        summary.destination_id = destination.destination_id;
        summary.hostname = destination.hostname;
        summary.ip_address = destination.ip_address;
        summary.status = destination.status;
        summary.progress_percent = m_destinationProgress.value(destination.destination_id, 0);
        summary.last_seen = destination.last_seen;
        summary.status_events = m_destinationStatusHistory.value(destination.destination_id);
        destinations.push_back(summary);
    }

    const QDateTime completedAt = QDateTime::currentDateTimeUtc();
    if (!DeploymentSummaryReport::exportCsv(filePath,
                                            m_activeDeploymentId,
                                            m_deploymentStartedAt,
                                            completedAt,
                                            jobs,
                                            destinations)) {
        QMessageBox::warning(this, tr("Export Error"), tr("Failed to export deployment summary."));
        return;
    }

    m_logText->append(tr("Deployment summary exported to %1").arg(filePath));
}

void NetworkTransferPanel::onExportDeploymentSummaryPdf() {
    if (!m_parallelManager || !m_orchestrator || !m_orchestrator->registry()) {
        return;
    }

    const QString filePath = QFileDialog::getSaveFileName(this, tr("Export Deployment Summary"),
                                                          QDir::homePath(), tr("PDF Files (*.pdf)"));
    if (filePath.isEmpty()) {
        return;
    }

    QVector<DeploymentJobSummary> jobs;
    for (const auto& job : m_parallelManager->allJobs()) {
        DeploymentJobSummary summary;
        summary.job_id = job.job_id;
        summary.source_user = job.source.username;
        summary.destination_id = job.destination.destination_id;
        summary.status = job.status;
        summary.bytes_transferred = job.bytes_transferred;
        summary.total_bytes = job.total_bytes;
        summary.error_message = job.error_message;
        jobs.push_back(summary);
    }

    QVector<DeploymentDestinationSummary> destinations;
    const auto registryDestinations = m_orchestrator->registry()->destinations();
    for (const auto& destination : registryDestinations) {
        DeploymentDestinationSummary summary;
        summary.destination_id = destination.destination_id;
        summary.hostname = destination.hostname;
        summary.ip_address = destination.ip_address;
        summary.status = destination.status;
        summary.progress_percent = m_destinationProgress.value(destination.destination_id, 0);
        summary.last_seen = destination.last_seen;
        summary.status_events = m_destinationStatusHistory.value(destination.destination_id);
        destinations.push_back(summary);
    }

    const QDateTime completedAt = QDateTime::currentDateTimeUtc();
    if (!DeploymentSummaryReport::exportPdf(filePath,
                                            m_activeDeploymentId,
                                            m_deploymentStartedAt,
                                            completedAt,
                                            jobs,
                                            destinations)) {
        QMessageBox::warning(this, tr("Export Error"), tr("Failed to export deployment summary."));
        return;
    }

    m_logText->append(tr("Deployment summary exported to %1").arg(filePath));
}

void NetworkTransferPanel::onRecoverLastDeployment() {
    auto& config = ConfigManager::instance();
    const QString deploymentId = config.getValue("orchestration/last_deployment_id").toString();
    const QString status = config.getValue("orchestration/last_deployment_status").toString();
    const QString startedAt = config.getValue("orchestration/last_deployment_started").toString();
    const QString completedAt = config.getValue("orchestration/last_deployment_completed").toString();

    if (deploymentId.isEmpty()) {
        QMessageBox::information(this, tr("Recover Deployment"), tr("No previous deployment state found."));
        return;
    }

    m_activeDeploymentId = deploymentId;
    if (!startedAt.isEmpty()) {
        m_deploymentStartedAt = QDateTime::fromString(startedAt, Qt::ISODate);
    }

    m_mappingTypeCombo->setCurrentIndex(config.getValue("orchestration/mapping_type", 0).toInt());
    m_mappingStrategyCombo->setCurrentIndex(config.getValue("orchestration/mapping_strategy", 0).toInt());
    m_maxConcurrentSpin->setValue(config.getValue("orchestration/max_concurrent", 10).toInt());
    m_globalBandwidthSpin->setValue(config.getValue("orchestration/global_bandwidth", 0).toInt());
    m_perJobBandwidthSpin->setValue(config.getValue("orchestration/per_job_bandwidth", 0).toInt());
    m_useTemplateCheck->setChecked(config.getValue("orchestration/use_template", false).toBool());

    const QString templatePath = config.getValue("orchestration/last_template_path").toString();
    if (!templatePath.isEmpty() && QFileInfo::exists(templatePath)) {
        m_loadedMapping = m_mappingEngine->loadTemplate(templatePath);
        if (!m_loadedMapping.sources.isEmpty()) {
            m_loadedTemplatePath = templatePath;
            m_templateStatusLabel->setText(tr("Loaded template: %1").arg(QFileInfo(templatePath).fileName()));
        }
    }

    refreshDeploymentHistory();

    m_logText->append(tr("Recovered deployment %1 (status: %2, started: %3, completed: %4)")
                          .arg(deploymentId)
                          .arg(status.isEmpty() ? tr("unknown") : status)
                          .arg(startedAt.isEmpty() ? tr("n/a") : startedAt)
                          .arg(completedAt.isEmpty() ? tr("n/a") : completedAt));
}

void NetworkTransferPanel::onParallelDeploymentCompleted(const QString& deployment_id, bool success) {
    Q_UNUSED(deployment_id);

    if (!m_historyManager || !m_parallelManager) {
        return;
    }

    DeploymentHistoryEntry entry;
    entry.deployment_id = m_activeDeploymentId.isEmpty() ? deployment_id : m_activeDeploymentId;
    entry.started_at = m_deploymentStartedAt;
    entry.completed_at = QDateTime::currentDateTimeUtc();
    entry.total_jobs = m_parallelManager->totalJobs();
    entry.completed_jobs = m_parallelManager->completedJobs();
    entry.failed_jobs = m_parallelManager->failedJobs();
    entry.status = success ? "success" : "failed";
    entry.template_path = m_loadedTemplatePath;

    m_historyManager->appendEntry(entry);

    auto& config = ConfigManager::instance();
    config.setValue("orchestration/last_deployment_completed", entry.completed_at.toString(Qt::ISODate));
    config.setValue("orchestration/last_deployment_status", entry.status);

    m_logText->append(tr("Deployment %1 %2. %3/%4 complete, %5 failed.")
                          .arg(entry.deployment_id)
                          .arg(success ? tr("completed") : tr("failed"))
                          .arg(entry.completed_jobs)
                          .arg(entry.total_jobs)
                          .arg(entry.failed_jobs));
    refreshDeploymentHistory();
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
        m_peerTable->setItem(row, kPeerColCaps, new QTableWidgetItem(entry.capabilities.join(", ")));
        m_peerTable->setItem(row, kPeerColSeen, new QTableWidgetItem(entry.last_seen.toString("hh:mm:ss")));
        row++;
    }
}

void NetworkTransferPanel::onManifestReceived(const TransferManifest& manifest) {
    m_manifestValidated = false;
    m_currentManifest = manifest;
    QJsonDocument doc(manifest.toJson(false));
    m_manifestText->setText(QString::fromUtf8(doc.toJson(QJsonDocument::Indented)));
    m_manifestText->append(tr("\nSummary: %1 users, %2 files, %3 total")
        .arg(manifest.users.size())
        .arg(manifest.total_files)
        .arg(formatBytes(manifest.total_bytes)));

    TransferManifest verifyManifest = manifest;
    verifyManifest.checksum_sha256.clear();
    QJsonDocument verifyDoc(verifyManifest.toJson());
    QByteArray verifyHash = QCryptographicHash::hash(verifyDoc.toJson(QJsonDocument::Compact), QCryptographicHash::Sha256);
    if (!manifest.checksum_sha256.isEmpty() && verifyHash.toHex() != manifest.checksum_sha256.toUtf8()) {
        m_manifestText->append(tr("\nWARNING: Manifest checksum mismatch."));
        m_controller->approveTransfer(false);
        m_approveButton->setEnabled(false);
        return;
    }

    auto available = path_utils::getAvailableSpace(std::filesystem::path(destinationBase().toStdString()));
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

    if (m_orchestrationAssignmentPending
        && m_autoApproveOrchestratorCheck
        && m_autoApproveOrchestratorCheck->isChecked()
        && m_approveButton->isEnabled()) {
        m_orchestrationAssignmentPending = false;
        onApproveTransfer();
    }
}

void NetworkTransferPanel::onTransferProgress(qint64 bytes, qint64 total) {
    if (total > 0) {
        const int percent = static_cast<int>((bytes * 100) / total);
        m_overallProgress->setValue(percent);
        Q_EMIT progressUpdate(percent, 100);
    }
}

void NetworkTransferPanel::onTransferCompleted(bool success, const QString& message) {
    m_overallProgress->setValue(success ? 100 : 0);
    m_logText->append(message);

    m_destinationTransferActive = false;
    m_manifestValidated = false;
    if (!m_activeAssignment.job_id.isEmpty()) {
        m_assignmentStatusByJob[m_activeAssignment.job_id] = success ? tr("completed") : tr("failed");
        m_assignmentEventByJob[m_activeAssignment.job_id] = message;
        refreshAssignmentStatus();
        persistAssignmentQueue();
    }
    m_activeAssignment = {};
    if (m_activeAssignmentLabel) {
        m_activeAssignmentLabel->setText(tr("No active assignment"));
    }

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
        reportDir = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation) + "/SAK/TransferReports";
    } else {
        reportDir = destinationBase() + "/TransferReports";
    }

    QDir dir(reportDir);
    if (!dir.exists()) {
        dir.mkpath(".");
    }
    const QString reportPath = dir.filePath(QString("transfer_%1_%2.json")
        .arg(m_currentManifest.transfer_id)
        .arg(QDateTime::currentDateTime().toString("yyyyMMdd_HHmmss")));
    if (!report.saveToFile(reportPath)) {
        m_logText->append(tr("Failed to save transfer report."));
    } else {
        m_logText->append(tr("Transfer report saved to %1").arg(reportPath));
    }

    if (success && m_modeCombo->currentIndex() == 1 && m_applyRestoreCheck->isChecked()) {
        if (m_restoreWorker && m_restoreWorker->isRunning()) {
            m_logText->append(tr("Restore already running."));
        } else {
            // Save manifest to staging directory for restore worker validation
            BackupManifest backupManifest;
            backupManifest.version = "1.0";
            backupManifest.created = QDateTime::currentDateTime();
            backupManifest.source_machine = m_currentManifest.source_hostname;
            backupManifest.sak_version = m_currentManifest.sak_version;
            backupManifest.users = m_currentManifest.users;
            backupManifest.total_backup_size_bytes = m_currentManifest.total_bytes;

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
                connect(m_restoreWorker, &UserProfileRestoreWorker::logMessage, this, [this](const QString& msg, bool warn) {
                    m_logText->append(warn ? tr("RESTORE WARN: %1").arg(msg) : msg);
                });
                connect(m_restoreWorker, &UserProfileRestoreWorker::restoreComplete, this, [this](bool ok, const QString& msg) {
                    m_logText->append(ok ? msg : tr("Restore failed: %1").arg(msg));
                });
            }

            m_logText->append(tr("Starting profile restore into system profiles..."));
            m_restoreWorker->startRestore(destinationBase(), backupManifest, mappings,
                                          ConflictResolution::RenameWithSuffix,
                                          PermissionMode::StripAll,
                                          true);
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

void NetworkTransferPanel::buildManifest() {
    m_currentFiles = buildFileList();
    m_currentManifest = buildManifestPayload(m_currentFiles);
    m_logText->append(tr("Manifest built: %1 files (%2)")
                      .arg(m_currentManifest.total_files)
                      .arg(formatBytes(m_currentManifest.total_bytes)));
}

QVector<TransferFileEntry> NetworkTransferPanel::buildFileList() {
    QVector<TransferFileEntry> files;

    file_hasher hasher(hash_algorithm::sha256);
    SmartFileFilter smartFilter{SmartFilter{}};
    PermissionManager permissionManager;
    const auto selectedPermMode = static_cast<PermissionMode>(m_permissionModeCombo->currentData().toInt());

    for (int i = 0; i < m_users.size(); ++i) {
        auto& user = m_users[i];
        auto* selectItem = m_userTable->item(i, kUserColSelect);
        if (!selectItem || selectItem->checkState() != Qt::Checked) {
            continue;
        }

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
                std::filesystem::path fsPath = path;
                if (!std::filesystem::is_regular_file(fsPath)) {
                    continue;
                }

                QFileInfo fileInfo(QString::fromStdString(fsPath.string()));
                if (smartFilter.shouldExcludeFile(fileInfo, user.profile_path) || smartFilter.exceedsSizeLimit(fileInfo.size())) {
                    continue;
                }

                TransferFileEntry entry;
                entry.file_id = QUuid::createUuid().toString(QUuid::WithoutBraces);
                entry.absolute_path = QString::fromStdString(fsPath.string());
                auto rel = path_utils::makeRelative(fsPath, std::filesystem::path(user.profile_path.toStdString()));
                if (!rel) {
                    continue;
                }
                QString relative = QString::fromStdString(rel->generic_string());
                entry.relative_path = user.username + "/" + relative;
                entry.size_bytes = static_cast<qint64>(std::filesystem::file_size(fsPath));
                if (selectedPermMode == PermissionMode::PreserveOriginal) {
                    entry.acl_sddl = permissionManager.getSecurityDescriptorSddl(entry.absolute_path);
                }
                auto hashResult = hasher.calculateHash(fsPath);
                if (hashResult) {
                    entry.checksum_sha256 = QString::fromStdString(*hashResult);
                }

                files.append(entry);
            }
        }
    }

    return files;
}

QVector<TransferFileEntry> NetworkTransferPanel::buildFileListForUsers(const QVector<UserProfile>& users) {
    QVector<TransferFileEntry> files;

    file_hasher hasher(hash_algorithm::sha256);
    SmartFileFilter smartFilter{SmartFilter{}};
    PermissionManager permissionManager;
    const auto selectedPermMode = static_cast<PermissionMode>(m_permissionModeCombo->currentData().toInt());

    for (const auto& user : users) {
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
                std::filesystem::path fsPath = path;
                if (!std::filesystem::is_regular_file(fsPath)) {
                    continue;
                }

                QFileInfo fileInfo(QString::fromStdString(fsPath.string()));
                if (smartFilter.shouldExcludeFile(fileInfo, user.profile_path) || smartFilter.exceedsSizeLimit(fileInfo.size())) {
                    continue;
                }

                TransferFileEntry entry;
                entry.file_id = QUuid::createUuid().toString(QUuid::WithoutBraces);
                entry.absolute_path = QString::fromStdString(fsPath.string());
                auto rel = path_utils::makeRelative(fsPath, std::filesystem::path(user.profile_path.toStdString()));
                if (!rel) {
                    continue;
                }
                QString relative = QString::fromStdString(rel->generic_string());
                entry.relative_path = user.username + "/" + relative;
                entry.size_bytes = static_cast<qint64>(std::filesystem::file_size(fsPath));
                if (selectedPermMode == PermissionMode::PreserveOriginal) {
                    entry.acl_sddl = permissionManager.getSecurityDescriptorSddl(entry.absolute_path);
                }
                auto hashResult = hasher.calculateHash(fsPath);
                if (hashResult) {
                    entry.checksum_sha256 = QString::fromStdString(*hashResult);
                }

                files.append(entry);
            }
        }
    }

    return files;
}

TransferManifest NetworkTransferPanel::buildManifestPayload(const QVector<TransferFileEntry>& files) {
    TransferManifest manifest;
    manifest.transfer_id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    manifest.source_hostname = QHostInfo::localHostName();
    manifest.source_os = "Windows";
    manifest.sak_version = sak::get_version_short();
    manifest.created = QDateTime::currentDateTime();
    manifest.files = files;

    const auto selectedPermMode = static_cast<PermissionMode>(m_permissionModeCombo->currentData().toInt());

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

    qint64 totalBytes = 0;
    for (const auto& file : files) {
        totalBytes += file.size_bytes;
    }
    manifest.total_bytes = totalBytes;
    manifest.total_files = files.size();

    QJsonDocument doc(manifest.toJson());
    QByteArray hash = QCryptographicHash::hash(doc.toJson(QJsonDocument::Compact), QCryptographicHash::Sha256);
    manifest.checksum_sha256 = hash.toHex();

    return manifest;
}

TransferManifest NetworkTransferPanel::buildManifestPayloadForUsers(const QVector<TransferFileEntry>& files,
                                                                    const QVector<UserProfile>& users) {
    TransferManifest manifest;
    manifest.transfer_id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    manifest.source_hostname = QHostInfo::localHostName();
    manifest.source_os = "Windows";
    manifest.sak_version = sak::get_version_short();
    manifest.created = QDateTime::currentDateTime();
    manifest.files = files;

    const auto selectedPermMode = static_cast<PermissionMode>(m_permissionModeCombo->currentData().toInt());

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
    QByteArray hash = QCryptographicHash::hash(doc.toJson(QJsonDocument::Compact), QCryptographicHash::Sha256);
    manifest.checksum_sha256 = hash.toHex();

    return manifest;
}

void NetworkTransferPanel::refreshOrchestratorDestinations() {
    if (!m_orchestrator || !m_orchestrator->registry() || !m_orchestratorDestTable) {
        return;
    }

    const auto destinations = m_orchestrator->registry()->destinations();
    m_orchestratorDestTable->setRowCount(0);

    int row = 0;
    for (const auto& destination : destinations) {
        m_orchestratorDestTable->insertRow(row);

        auto* selectItem = new QTableWidgetItem();
        selectItem->setCheckState(Qt::Checked);
        selectItem->setData(Qt::UserRole, destination.destination_id);
        m_orchestratorDestTable->setItem(row, 0, selectItem);

        auto* hostItem = new QTableWidgetItem(destination.hostname);
        hostItem->setData(Qt::UserRole, destination.destination_id);
        m_orchestratorDestTable->setItem(row, 1, hostItem);
        m_orchestratorDestTable->setItem(row, 2, new QTableWidgetItem(destination.ip_address));
        auto* statusItem = new QTableWidgetItem(destination.status);
        applyStatusColors(statusItem, statusColor(destination.status));
        m_orchestratorDestTable->setItem(row, 3, statusItem);
        m_orchestratorDestTable->setItem(row, 4, new QTableWidgetItem(formatBytes(destination.health.free_disk_bytes)));
        m_orchestratorDestTable->setItem(row, 5, new QTableWidgetItem(QString::number(destination.health.cpu_usage_percent)));
        m_orchestratorDestTable->setItem(row, 6, new QTableWidgetItem(QString::number(destination.health.ram_usage_percent)));
        m_orchestratorDestTable->setItem(row, 7, new QTableWidgetItem(destination.last_seen.toString("hh:mm:ss")));

        const int progress = m_destinationProgress.value(destination.destination_id, 0);
        auto* progressItem = new QTableWidgetItem(QString::number(progress) + "%");
        applyStatusColors(progressItem, progressColor(progress));
        m_orchestratorDestTable->setItem(row, 8, progressItem);
        row++;
    }
}

void NetworkTransferPanel::refreshJobsTable() {
    if (!m_jobsTable || !m_parallelManager) {
        return;
    }

    m_jobsTable->setRowCount(0);
    int row = 0;
    qint64 remainingBytes = 0;
    double totalSpeedMbps = 0.0;
    for (const auto& jobId : m_knownJobIds) {
        const auto job = m_parallelManager->getJobStatus(jobId);
        if (job.job_id.isEmpty()) {
            continue;
        }

        m_jobsTable->insertRow(row);
        m_jobsTable->setItem(row, 0, new QTableWidgetItem(job.job_id));
        m_jobsTable->setItem(row, 1, new QTableWidgetItem(m_jobToDeploymentId.value(jobId)));
        m_jobsTable->setItem(row, 2, new QTableWidgetItem(job.source.username));
        m_jobsTable->setItem(row, 3, new QTableWidgetItem(job.destination.destination_id));
        auto* statusItem = new QTableWidgetItem(job.status);
        applyStatusColors(statusItem, statusColor(job.status));
        m_jobsTable->setItem(row, 4, statusItem);

        int percent = 0;
        if (job.total_bytes > 0) {
            percent = static_cast<int>((job.bytes_transferred * 100) / job.total_bytes);
        }
        auto* progressItem = new QTableWidgetItem(QString::number(percent) + "%");
        applyStatusColors(progressItem, progressColor(percent));
        m_jobsTable->setItem(row, 5, progressItem);
        auto* errorItem = new QTableWidgetItem(job.error_message);
        if (!job.error_message.isEmpty()) {
            applyStatusColors(errorItem, QColor(198, 40, 40));
        }
        m_jobsTable->setItem(row, 6, errorItem);
        row++;

        if (job.total_bytes > 0 && job.bytes_transferred < job.total_bytes) {
            remainingBytes += (job.total_bytes - job.bytes_transferred);
        }
        if (job.speed_mbps > 0.0 && job.status == "transferring") {
            totalSpeedMbps += job.speed_mbps;
        }
    }

    if (m_deploymentEtaLabel) {
        if (remainingBytes > 0 && totalSpeedMbps > 0.0) {
            const double bytesPerSecond = (totalSpeedMbps * 1024.0 * 1024.0) / 8.0;
            const qint64 etaSeconds = static_cast<qint64>(remainingBytes / bytesPerSecond);
            const QTime etaTime(0, 0, 0);
            m_deploymentEtaLabel->setText(tr("ETA: %1").arg(etaTime.addSecs(static_cast<int>(etaSeconds)).toString("hh:mm:ss")));
        } else {
            m_deploymentEtaLabel->setText(tr("ETA: --"));
        }
    }
}

MappingEngine::DeploymentMapping NetworkTransferPanel::buildDeploymentMapping() {
    if (m_useTemplateCheck && m_useTemplateCheck->isChecked() && !m_loadedMapping.sources.isEmpty()) {
        return m_loadedMapping;
    }

    MappingEngine::DeploymentMapping mapping;
    mapping.deployment_id = m_activeDeploymentId;

    QString localIp;
    for (const auto& iface : QNetworkInterface::allInterfaces()) {
        for (const auto& entry : iface.addressEntries()) {
            if (entry.ip().protocol() == QAbstractSocket::IPv4Protocol && !entry.ip().isLoopback()) {
                localIp = entry.ip().toString();
                break;
            }
        }
        if (!localIp.isEmpty()) {
            break;
        }
    }

    for (int row = 0; row < m_orchestratorUserTable->rowCount(); ++row) {
        auto* selectItem = m_orchestratorUserTable->item(row, 0);
        if (!selectItem || selectItem->checkState() != Qt::Checked) {
            continue;
        }
        auto* userItem = m_orchestratorUserTable->item(row, 1);
        if (!userItem) {
            continue;
        }
        const int index = userItem->data(Qt::UserRole).toInt();
        if (index < 0 || index >= m_users.size()) {
            continue;
        }

        const auto& user = m_users[index];
        MappingEngine::SourceProfile source;
        source.username = user.username;
        source.source_hostname = QHostInfo::localHostName();
        source.source_ip = localIp;
        source.profile_size_bytes = user.total_size_estimated;
        mapping.sources.push_back(source);
    }

    QMap<QString, DestinationPC> destinationMap;
    if (m_orchestrator && m_orchestrator->registry()) {
        const auto destinations = m_orchestrator->registry()->destinations();
        for (const auto& destination : destinations) {
            destinationMap.insert(destination.destination_id, destination);
        }
    }

    for (int row = 0; row < m_orchestratorDestTable->rowCount(); ++row) {
        auto* selectItem = m_orchestratorDestTable->item(row, 0);
        if (!selectItem || selectItem->checkState() != Qt::Checked) {
            continue;
        }

        const QString destinationId = selectItem->data(Qt::UserRole).toString();
        if (destinationId.isEmpty() || !destinationMap.contains(destinationId)) {
            continue;
        }
        mapping.destinations.push_back(destinationMap.value(destinationId));
    }

    const int mappingTypeIndex = m_mappingTypeCombo->currentIndex();
    if (mappingTypeIndex == 1) {
        mapping.type = MappingEngine::MappingType::ManyToMany;
    } else if (mappingTypeIndex == 2) {
        mapping.type = MappingEngine::MappingType::CustomMapping;
    } else {
        mapping.type = MappingEngine::MappingType::OneToMany;
    }

    if (mapping.type == MappingEngine::MappingType::CustomMapping) {
        for (int row = 0; row < m_customRulesTable->rowCount(); ++row) {
            auto* sourceItem = m_customRulesTable->item(row, 0);
            auto* destinationItem = m_customRulesTable->item(row, 1);
            if (!sourceItem || !destinationItem) {
                continue;
            }
            const QString sourceUser = sourceItem->text().trimmed();
            const QString destinationId = destinationItem->text().trimmed();
            if (!sourceUser.isEmpty() && !destinationId.isEmpty()) {
                mapping.custom_rules.insert(sourceUser, destinationId);
            }
        }
    }

    return mapping;
}

void NetworkTransferPanel::refreshAssignmentQueue() {
    if (!m_assignmentQueueTable) {
        return;
    }

    m_assignmentQueueTable->setRowCount(0);
    int row = 0;
    for (const auto& assignment : m_assignmentQueue) {
        m_assignmentQueueTable->insertRow(row);
        m_assignmentQueueTable->setItem(row, 0, new QTableWidgetItem(assignment.deployment_id));
        m_assignmentQueueTable->setItem(row, 1, new QTableWidgetItem(assignment.job_id));
        m_assignmentQueueTable->setItem(row, 2, new QTableWidgetItem(assignment.source_user));
        m_assignmentQueueTable->setItem(row, 3, new QTableWidgetItem(formatBytes(assignment.profile_size_bytes)));
        m_assignmentQueueTable->setItem(row, 4, new QTableWidgetItem(assignment.priority));
        const QString bandwidthText = assignment.max_bandwidth_kbps > 0
            ? tr("%1 KB/s").arg(assignment.max_bandwidth_kbps)
            : tr("default");
        m_assignmentQueueTable->setItem(row, 5, new QTableWidgetItem(bandwidthText));
        row++;
    }
}

bool NetworkTransferPanel::eventFilter(QObject* obj, QEvent* event) {
    if (obj == m_orchestratorDestTable) {
        if (event->type() == QEvent::DragEnter || event->type() == QEvent::DragMove) {
            auto* dragEvent = static_cast<QDragEnterEvent*>(event);
            if (extractDraggedUserName(dragEvent->mimeData()).isEmpty()) {
                return false;
            }
            dragEvent->acceptProposedAction();
            return true;
        }
        if (event->type() == QEvent::Drop) {
            auto* dropEvent = static_cast<QDropEvent*>(event);
            const QString user = extractDraggedUserName(dropEvent->mimeData());
            if (user.isEmpty()) {
                return false;
            }

            const QPoint pos = dropEvent->position().toPoint();
            auto* item = m_orchestratorDestTable->itemAt(pos);
            int row = item ? item->row() : m_orchestratorDestTable->currentRow();
            if (row < 0) {
                return false;
            }

            const QString destinationId = destinationIdForRow(row);
            if (destinationId.isEmpty()) {
                return false;
            }

            auto* selectItem = m_orchestratorDestTable->item(row, 0);
            if (selectItem && selectItem->checkState() != Qt::Checked) {
                selectItem->setCheckState(Qt::Checked);
            }

            upsertCustomRule(user, destinationId);
            dropEvent->acceptProposedAction();
            return true;
        }
    }

    return QWidget::eventFilter(obj, event);
}

void NetworkTransferPanel::upsertCustomRule(const QString& sourceUser, const QString& destinationId) {
    if (!m_customRulesTable || sourceUser.isEmpty() || destinationId.isEmpty()) {
        return;
    }

    for (int row = 0; row < m_customRulesTable->rowCount(); ++row) {
        auto* sourceItem = m_customRulesTable->item(row, 0);
        if (sourceItem && sourceItem->text().trimmed() == sourceUser) {
            auto* destItem = m_customRulesTable->item(row, 1);
            if (!destItem) {
                destItem = new QTableWidgetItem();
                m_customRulesTable->setItem(row, 1, destItem);
            }
            destItem->setText(destinationId);
            if (m_mappingTypeCombo) {
                m_mappingTypeCombo->setCurrentIndex(2);
            }
            return;
        }
    }

    const int row = m_customRulesTable->rowCount();
    m_customRulesTable->insertRow(row);
    m_customRulesTable->setItem(row, 0, new QTableWidgetItem(sourceUser));
    m_customRulesTable->setItem(row, 1, new QTableWidgetItem(destinationId));
    if (m_mappingTypeCombo) {
        m_mappingTypeCombo->setCurrentIndex(2);
    }
}

QString NetworkTransferPanel::destinationIdForRow(int row) const {
    if (!m_orchestratorDestTable || row < 0) {
        return {};
    }

    auto* selectItem = m_orchestratorDestTable->item(row, 0);
    if (selectItem) {
        const QString storedId = selectItem->data(Qt::UserRole).toString();
        if (!storedId.isEmpty()) {
            return storedId;
        }
    }

    auto* hostItem = m_orchestratorDestTable->item(row, 1);
    if (hostItem) {
        return hostItem->text().trimmed();
    }

    return {};
}

QString NetworkTransferPanel::extractDraggedUserName(const QMimeData* mime) const {
    if (!mime || !mime->hasFormat("application/x-qabstractitemmodeldatalist")) {
        return {};
    }

    QByteArray encoded = mime->data("application/x-qabstractitemmodeldatalist");
    QDataStream stream(&encoded, QIODevice::ReadOnly);
    while (!stream.atEnd()) {
        int row = 0;
        int column = 0;
        QMap<int, QVariant> roleDataMap;
        stream >> row >> column >> roleDataMap;
        if (column == 1 && roleDataMap.contains(Qt::DisplayRole)) {
            return roleDataMap.value(Qt::DisplayRole).toString().trimmed();
        }
    }

    return {};
}

void NetworkTransferPanel::refreshAssignmentStatus() {
    if (!m_assignmentStatusTable) {
        return;
    }

    m_assignmentStatusTable->setRowCount(0);
    int row = 0;

    auto addRow = [this, &row](const DeploymentAssignment& assignment, const QString& status, const QString& eventText) {
        m_assignmentStatusTable->insertRow(row);
        m_assignmentStatusTable->setItem(row, 0, new QTableWidgetItem(assignment.deployment_id));
        m_assignmentStatusTable->setItem(row, 1, new QTableWidgetItem(assignment.job_id));
        m_assignmentStatusTable->setItem(row, 2, new QTableWidgetItem(assignment.source_user));
        auto* statusItem = new QTableWidgetItem(status);
        applyStatusColors(statusItem, statusColor(status));
        m_assignmentStatusTable->setItem(row, 3, statusItem);
        m_assignmentStatusTable->setItem(row, 4, new QTableWidgetItem(eventText));
        row++;
    };

    if (!m_activeAssignment.deployment_id.isEmpty()) {
        const QString status = m_assignmentStatusByJob.value(m_activeAssignment.job_id, tr("active"));
        const QString eventText = m_assignmentEventByJob.value(m_activeAssignment.job_id, tr("Active"));
        addRow(m_activeAssignment, status, eventText);
    }

    for (const auto& assignment : m_assignmentQueue) {
        const QString status = m_assignmentStatusByJob.value(assignment.job_id, tr("queued"));
        const QString eventText = m_assignmentEventByJob.value(assignment.job_id, tr("Queued"));
        addRow(assignment, status, eventText);
    }
}

void NetworkTransferPanel::activateAssignment(const DeploymentAssignment& assignment) {
    m_activeAssignment = assignment;
    if (m_activeAssignmentLabel) {
        m_activeAssignmentLabel->setText(tr("Active: %1 (%2)")
                                             .arg(assignment.source_user, assignment.deployment_id));
    }

    if (m_assignmentBandwidthLabel) {
        if (assignment.max_bandwidth_kbps > 0) {
            m_assignmentBandwidthLabel->setText(tr("Bandwidth limit: %1 KB/s").arg(assignment.max_bandwidth_kbps));
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
        const bool hasPassphrase = !m_settings.encryption_enabled || !m_destinationPassphraseEdit->text().isEmpty();
        if (hasDestinationBase && hasPassphrase) {
            onStartDestination();
        } else {
            m_logText->append(tr("Assignment received. Set destination base/passphrase to begin listening."));
        }
    }
}

void NetworkTransferPanel::onConnectionStateChanged(bool connected) {
    if (!connected) {
        return;
    }

    if (m_orchestrationAssignmentPending
        && m_manifestValidated
        && m_autoApproveOrchestratorCheck
        && m_autoApproveOrchestratorCheck->isChecked()
        && m_approveButton
        && m_approveButton->isEnabled()) {
        m_orchestrationAssignmentPending = false;
        onApproveTransfer();
    }
}

void NetworkTransferPanel::persistAssignmentQueue() const {
    if (m_assignmentQueueStore) {
        m_assignmentQueueStore->save(m_activeAssignment, m_assignmentQueue,
                                     m_assignmentStatusByJob, m_assignmentEventByJob);
    }
}

void NetworkTransferPanel::refreshDeploymentHistory() {
    if (!m_historyManager || !m_historyTable) {
        return;
    }

    const auto entries = m_historyManager->loadEntries();
    m_historyTable->setRowCount(0);
    int row = 0;
    for (const auto& entry : entries) {
        m_historyTable->insertRow(row);
        m_historyTable->setItem(row, 0, new QTableWidgetItem(entry.deployment_id));
        m_historyTable->setItem(row, 1, new QTableWidgetItem(entry.started_at.toString("yyyy-MM-dd hh:mm:ss")));
        m_historyTable->setItem(row, 2, new QTableWidgetItem(entry.completed_at.toString("yyyy-MM-dd hh:mm:ss")));
        m_historyTable->setItem(row, 3, new QTableWidgetItem(QString::number(entry.total_jobs)));
        m_historyTable->setItem(row, 4, new QTableWidgetItem(QString::number(entry.completed_jobs)));
        m_historyTable->setItem(row, 5, new QTableWidgetItem(QString::number(entry.failed_jobs)));
        m_historyTable->setItem(row, 6, new QTableWidgetItem(entry.status));
        row++;
    }
}

QString NetworkTransferPanel::destinationBase() const {
    return m_destinationBaseEdit->text().trimmed();
}

} // namespace sak
