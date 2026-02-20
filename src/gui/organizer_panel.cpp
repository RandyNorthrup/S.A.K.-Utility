#include "sak/organizer_panel.h"
#include "sak/organizer_worker.h"
#include "sak/logger.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGroupBox>
#include <QLabel>
#include <QFileDialog>
#include <QMessageBox>
#include <QHeaderView>
#include <QDateTime>
#include <QScrollArea>
#include <QFrame>

OrganizerPanel::OrganizerPanel(QWidget* parent)
    : QWidget(parent)
    , m_worker(nullptr)
{
    setupUi();
    setupDefaultCategories();
    sak::logInfo("OrganizerPanel initialized");
}

OrganizerPanel::~OrganizerPanel()
{
    if (m_worker) {
        m_worker->requestStop();
        if (!m_worker->wait(15000)) {
            sak::logError("OrganizerWorker did not stop within 15s \u2014 potential resource leak");
        }
    }
    sak::logInfo("OrganizerPanel destroyed");
}

void OrganizerPanel::setupUi()
{
    auto* root_layout = new QVBoxLayout(this);
    root_layout->setContentsMargins(0, 0, 0, 0);

    auto* scroll_area = new QScrollArea(this);
    scroll_area->setWidgetResizable(true);
    scroll_area->setFrameShape(QFrame::NoFrame);

    auto* content_widget = new QWidget(scroll_area);
    auto* main_layout = new QVBoxLayout(content_widget);
    main_layout->setContentsMargins(16, 16, 16, 16);
    main_layout->setSpacing(12);

    scroll_area->setWidget(content_widget);
    root_layout->addWidget(scroll_area);

    // Target directory group
    auto* path_group = new QGroupBox("Target Directory", this);
    auto* path_layout = new QHBoxLayout(path_group);
    
    m_target_path = new QLineEdit(this);
    m_target_path->setPlaceholderText("Select directory to organize...");
    path_layout->addWidget(m_target_path, 1);
    
    m_browse_button = new QPushButton("Browse...", this);
    path_layout->addWidget(m_browse_button);
    
    main_layout->addWidget(path_group);

    // Category mapping group
    auto* category_group = new QGroupBox("Category Mapping", this);
    auto* category_layout = new QVBoxLayout(category_group);
    
    m_category_table = new QTableWidget(this);
    m_category_table->setColumnCount(2);
    m_category_table->setHorizontalHeaderLabels({"Category", "Extensions (comma-separated)"});
    m_category_table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    m_category_table->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    m_category_table->setAlternatingRowColors(true);
    m_category_table->setMinimumHeight(200);
    category_layout->addWidget(m_category_table);
    
    auto* button_layout = new QHBoxLayout();
    m_add_category_button = new QPushButton("Add Category", this);
    m_remove_category_button = new QPushButton("Remove Selected", this);
    button_layout->addWidget(m_add_category_button);
    button_layout->addWidget(m_remove_category_button);
    button_layout->addStretch();
    category_layout->addLayout(button_layout);
    
    main_layout->addWidget(category_group);

    // Options group
    auto* options_group = new QGroupBox("Options", this);
    auto* options_layout = new QHBoxLayout(options_group);
    
    options_layout->addWidget(new QLabel("Collision Strategy:", this));
    m_collision_strategy = new QComboBox(this);
    m_collision_strategy->addItems({"Rename", "Skip", "Overwrite"});
    options_layout->addWidget(m_collision_strategy);
    
    m_preview_mode_checkbox = new QCheckBox("Preview Mode (Dry Run)", this);
    m_preview_mode_checkbox->setChecked(true);
    options_layout->addWidget(m_preview_mode_checkbox);
    
    options_layout->addStretch();
    main_layout->addWidget(options_group);

    // Progress group
    auto* progress_group = new QGroupBox("Progress", this);
    auto* progress_layout = new QVBoxLayout(progress_group);
    
    m_progress_bar = new QProgressBar(this);
    m_progress_bar->setTextVisible(true);
    m_progress_bar->setFormat("%v / %m (%p%)");
    progress_layout->addWidget(m_progress_bar);
    
    m_status_label = new QLabel("Ready", this);
    m_status_label->setStyleSheet("font-weight: 600; color: #1e293b;");
    progress_layout->addWidget(m_status_label);
    
    main_layout->addWidget(progress_group);

    // Control buttons
    auto* control_layout = new QHBoxLayout();
    control_layout->addStretch();
    
    m_preview_button = new QPushButton("Preview", this);
    m_preview_button->setMinimumWidth(100);
    control_layout->addWidget(m_preview_button);
    
    m_execute_button = new QPushButton("Execute", this);
    m_execute_button->setMinimumWidth(100);
    control_layout->addWidget(m_execute_button);
    
    m_cancel_button = new QPushButton("Cancel", this);
    m_cancel_button->setMinimumWidth(100);
    m_cancel_button->setEnabled(false);
    control_layout->addWidget(m_cancel_button);
    
    main_layout->addLayout(control_layout);

    // Log viewer
    auto* log_group = new QGroupBox("Log", this);
    auto* log_layout = new QVBoxLayout(log_group);
    
    m_log_viewer = new QTextEdit(this);
    m_log_viewer->setReadOnly(true);
    m_log_viewer->setMaximumHeight(150);
    m_log_viewer->setPlaceholderText("Operation log will appear here...");
    log_layout->addWidget(m_log_viewer);
    
    main_layout->addWidget(log_group);

    main_layout->addStretch(1);

    // Connect signals
    connect(m_browse_button, &QPushButton::clicked, this, &OrganizerPanel::onBrowseClicked);
    connect(m_preview_button, &QPushButton::clicked, this, &OrganizerPanel::onPreviewClicked);
    connect(m_execute_button, &QPushButton::clicked, this, &OrganizerPanel::onExecuteClicked);
    connect(m_cancel_button, &QPushButton::clicked, this, &OrganizerPanel::onCancelClicked);
    connect(m_add_category_button, &QPushButton::clicked, this, &OrganizerPanel::onAddCategoryClicked);
    connect(m_remove_category_button, &QPushButton::clicked, this, &OrganizerPanel::onRemoveCategoryClicked);
}

void OrganizerPanel::setupDefaultCategories()
{
    QMap<QString, QString> defaults = {
        {"Images", "jpg,jpeg,png,gif,bmp,svg,webp,ico"},
        {"Documents", "pdf,doc,docx,txt,rtf,odt,xls,xlsx,ppt,pptx"},
        {"Audio", "mp3,wav,flac,aac,ogg,m4a,wma"},
        {"Video", "mp4,avi,mkv,mov,wmv,flv,webm"},
        {"Archives", "zip,rar,7z,tar,gz,bz2"},
        {"Code", "cpp,h,py,js,java,cs,html,css,json,xml"}
    };

    m_category_table->setRowCount(defaults.size());
    int row = 0;
    for (auto it = defaults.begin(); it != defaults.end(); ++it) {
        m_category_table->setItem(row, 0, new QTableWidgetItem(it.key()));
        m_category_table->setItem(row, 1, new QTableWidgetItem(it.value()));
        ++row;
    }
}

void OrganizerPanel::onBrowseClicked()
{
    QString dir = QFileDialog::getExistingDirectory(
        this, "Select Directory to Organize",
        m_target_path->text(),
        QFileDialog::ShowDirsOnly | QFileDialog::DontResolveSymlinks);

    if (!dir.isEmpty()) {
        m_target_path->setText(dir);
        logMessage(QString("Target directory selected: %1").arg(dir));
    }
}

void OrganizerPanel::onPreviewClicked()
{
    m_preview_mode_checkbox->setChecked(true);
    onExecuteClicked();
}

void OrganizerPanel::onExecuteClicked()
{
    if (m_target_path->text().isEmpty()) {
        QMessageBox::warning(this, "Validation Error", "Please select a target directory.");
        return;
    }

    QDir target_dir(m_target_path->text());
    if (!target_dir.exists()) {
        QMessageBox::warning(this, "Validation Error", "Target directory does not exist.");
        return;
    }

    // Clean up previous worker
    m_worker.reset();

    // Create worker configuration
    OrganizerWorker::Config config;
    config.target_directory = m_target_path->text();
    config.category_mapping = getCategoryMapping();
    config.preview_mode = m_preview_mode_checkbox->isChecked();
    config.create_subdirectories = true;
    
    QString strategy = m_collision_strategy->currentText().toLower();
    config.collision_strategy = strategy;

    // Create and configure worker
    m_worker = std::make_unique<OrganizerWorker>(config, this);

    connect(m_worker.get(), &OrganizerWorker::started, this, &OrganizerPanel::onWorkerStarted);
    connect(m_worker.get(), &OrganizerWorker::finished, this, &OrganizerPanel::onWorkerFinished);
    connect(m_worker.get(), &OrganizerWorker::failed, this, &OrganizerPanel::onWorkerFailed);
    connect(m_worker.get(), &OrganizerWorker::cancelled, this, &OrganizerPanel::onWorkerCancelled);
    connect(m_worker.get(), &OrganizerWorker::fileProgress, this, &OrganizerPanel::onFileProgress);
    connect(m_worker.get(), &OrganizerWorker::previewResults, this, &OrganizerPanel::onPreviewResults);

    setOperationRunning(true);
    m_status_label->setText("Status: Starting...");
    m_worker->start();

    QString mode = config.preview_mode ? "Preview" : "Execute";
    sak::logInfo("Organization operation initiated ({}): {}", mode.toStdString(), 
                  config.target_directory.toStdString());
}

void OrganizerPanel::onCancelClicked()
{
    if (m_worker != nullptr) {
        m_worker->requestStop();
        logMessage("Cancellation requested...");
        m_status_label->setText("Status: Cancelling...");
        sak::logInfo("Organization cancellation requested by user");
    }
}

void OrganizerPanel::onAddCategoryClicked()
{
    int row = m_category_table->rowCount();
    m_category_table->insertRow(row);
    m_category_table->setItem(row, 0, new QTableWidgetItem("New Category"));
    m_category_table->setItem(row, 1, new QTableWidgetItem(""));
    m_category_table->editItem(m_category_table->item(row, 0));
}

void OrganizerPanel::onRemoveCategoryClicked()
{
    auto selected = m_category_table->selectedItems();
    if (selected.isEmpty()) {
        QMessageBox::information(this, "No Selection", "Please select a category to remove.");
        return;
    }

    int row = m_category_table->currentRow();
    if (row >= 0) {
        m_category_table->removeRow(row);
    }
}

void OrganizerPanel::onWorkerStarted()
{
    QString mode = m_preview_mode_checkbox->isChecked() ? "preview" : "organization";
    logMessage(QString("Starting %1...").arg(mode));
    m_status_label->setText(QString("Status: Running %1...").arg(mode));
    Q_EMIT statusMessage(QString("%1 in progress").arg(mode), 0);
}

void OrganizerPanel::onWorkerFinished()
{
    setOperationRunning(false);
    QString mode = m_preview_mode_checkbox->isChecked() ? "Preview" : "Organization";
    m_status_label->setText(QString("Status: %1 complete").arg(mode));
    logMessage(QString("%1 completed successfully").arg(mode));
    QMessageBox::information(this, QString("%1 Complete").arg(mode), 
                            QString("%1 operation completed successfully").arg(mode));
    sak::logInfo("Organization operation completed successfully");
}

void OrganizerPanel::onWorkerFailed(int error_code, const QString& error_message)
{
    setOperationRunning(false);
    m_status_label->setText("Status: Failed");
    logMessage(QString("Organization failed: Error %1: %2").arg(error_code).arg(error_message));
    QMessageBox::warning(this, "Organization Failed", 
                        QString("Error %1: %2").arg(error_code).arg(error_message));
    sak::logError("Organization failed: {}", error_message.toStdString());
}

void OrganizerPanel::onWorkerCancelled()
{
    setOperationRunning(false);
    logMessage("Organization cancelled by user");
    m_status_label->setText("Status: Cancelled");
    Q_EMIT statusMessage("Organization cancelled", 3000);
}

void OrganizerPanel::onFileProgress(int current, int total, const QString& file_path)
{
    m_progress_bar->setMaximum(total);
    m_progress_bar->setValue(current);
    
    QString filename = QFileInfo(file_path).fileName();
    m_status_label->setText(QString("Processing: %1").arg(filename));
}

void OrganizerPanel::onPreviewResults(const QString& summary, int operation_count)
{
    QMessageBox::information(this, "Preview Results", summary);
    logMessage(QString("Preview completed: %1 operations planned").arg(operation_count));
}

QMap<QString, QStringList> OrganizerPanel::getCategoryMapping() const
{
    QMap<QString, QStringList> mapping;

    for (int row = 0; row < m_category_table->rowCount(); ++row) {
        auto* category_item = m_category_table->item(row, 0);
        auto* extensions_item = m_category_table->item(row, 1);

        if (category_item && extensions_item) {
            QString category = category_item->text().trimmed();
            QString extensions_str = extensions_item->text().trimmed();

            if (!category.isEmpty() && !extensions_str.isEmpty()) {
                QStringList extensions = extensions_str.split(',', Qt::SkipEmptyParts);
                for (auto& ext : extensions) {
                    ext = ext.trimmed();
                }
                mapping[category] = extensions;
            }
        }
    }

    return mapping;
}

void OrganizerPanel::setOperationRunning(bool running)
{
    m_operation_running = running;
    
    m_target_path->setEnabled(!running);
    m_browse_button->setEnabled(!running);
    m_category_table->setEnabled(!running);
    m_add_category_button->setEnabled(!running);
    m_remove_category_button->setEnabled(!running);
    m_collision_strategy->setEnabled(!running);
    m_preview_mode_checkbox->setEnabled(!running);
    
    m_preview_button->setEnabled(!running);
    m_execute_button->setEnabled(!running);
    m_cancel_button->setEnabled(running);
}

void OrganizerPanel::logMessage(const QString& message)
{
    QString timestamp = QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss");
    m_log_viewer->append(QString("[%1] %2").arg(timestamp, message));
}
