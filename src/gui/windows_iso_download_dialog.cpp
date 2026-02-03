// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: MIT

#include "sak/windows_iso_download_dialog.h"
#include "sak/windows_iso_downloader.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QFileDialog>
#include <QStandardPaths>
#include <QMessageBox>

WindowsISODownloadDialog::WindowsISODownloadDialog(WindowsISODownloader* downloader, QWidget* parent)
    : QDialog(parent)
    , m_downloader(downloader)
    , m_isDownloading(false)
    , m_sessionInitialized(false)
    , m_languagesLoaded(false)
{
    setWindowTitle("Download Windows 11 ISO");
    setModal(true);
    resize(600, 400);
    
    setupUI();
    connectSignals();
    
    // Start fetching product page and languages
    m_statusLabel->setText("Initializing...");
    m_downloader->fetchProductPage();
}

WindowsISODownloadDialog::~WindowsISODownloadDialog() = default;

void WindowsISODownloadDialog::setupUI() {
    auto* mainLayout = new QVBoxLayout(this);
    
    // Selection group
    auto* selectionGroup = new QGroupBox("Download Options", this);
    auto* selectionLayout = new QGridLayout(selectionGroup);
    
    // Language selection
    selectionLayout->addWidget(new QLabel("Language:", selectionGroup), 0, 0);
    m_languageCombo = new QComboBox(selectionGroup);
    m_languageCombo->setEnabled(false);
    selectionLayout->addWidget(m_languageCombo, 0, 1);
    
    // Architecture selection
    selectionLayout->addWidget(new QLabel("Architecture:", selectionGroup), 1, 0);
    m_architectureCombo = new QComboBox(selectionGroup);
    m_architectureCombo->addItem("64-bit (x64)", "x64");
    m_architectureCombo->addItem("ARM64", "ARM64");
    selectionLayout->addWidget(m_architectureCombo, 1, 1);
    
    // Save location
    selectionLayout->addWidget(new QLabel("Save to:", selectionGroup), 2, 0);
    auto* saveLayout = new QHBoxLayout();
    m_saveLocationEdit = new QLineEdit(getDefaultSavePath(), selectionGroup);
    saveLayout->addWidget(m_saveLocationEdit);
    m_browseSaveButton = new QPushButton("Browse...", selectionGroup);
    connect(m_browseSaveButton, &QPushButton::clicked, this, &WindowsISODownloadDialog::onBrowseSaveLocation);
    saveLayout->addWidget(m_browseSaveButton);
    selectionLayout->addLayout(saveLayout, 2, 1);
    
    mainLayout->addWidget(selectionGroup);
    
    // Progress group
    auto* progressGroup = new QGroupBox("Download Progress", this);
    auto* progressLayout = new QVBoxLayout(progressGroup);
    
    m_statusLabel = new QLabel("Ready", progressGroup);
    progressLayout->addWidget(m_statusLabel);
    
    m_progressBar = new QProgressBar(progressGroup);
    m_progressBar->setMinimum(0);
    m_progressBar->setMaximum(100);
    m_progressBar->setValue(0);
    progressLayout->addWidget(m_progressBar);
    
    m_sizeLabel = new QLabel("", progressGroup);
    progressLayout->addWidget(m_sizeLabel);
    
    m_speedLabel = new QLabel("", progressGroup);
    progressLayout->addWidget(m_speedLabel);
    
    mainLayout->addWidget(progressGroup);
    
    // Info label
    auto* infoLabel = new QLabel(
        "<b>Note:</b> Windows 11 ISO is approximately 5.5 GB. "
        "Download URLs are valid for 24 hours.",
        this
    );
    infoLabel->setWordWrap(true);
    infoLabel->setStyleSheet("color: #64748b; font-size: 10pt; padding: 6px;");
    mainLayout->addWidget(infoLabel);
    
    // Buttons
    auto* buttonLayout = new QHBoxLayout();
    buttonLayout->addStretch();
    
    m_downloadButton = new QPushButton("Download", this);
    m_downloadButton->setEnabled(false);
    connect(m_downloadButton, &QPushButton::clicked, this, &WindowsISODownloadDialog::onStartDownload);
    buttonLayout->addWidget(m_downloadButton);
    
    m_cancelButton = new QPushButton("Cancel Download", this);
    m_cancelButton->setEnabled(false);
    connect(m_cancelButton, &QPushButton::clicked, this, &WindowsISODownloadDialog::onCancelDownload);
    buttonLayout->addWidget(m_cancelButton);
    
    m_closeButton = new QPushButton("Close", this);
    connect(m_closeButton, &QPushButton::clicked, this, &QDialog::reject);
    buttonLayout->addWidget(m_closeButton);
    
    mainLayout->addLayout(buttonLayout);
}

void WindowsISODownloadDialog::connectSignals() {
    connect(m_downloader, &WindowsISODownloader::productPageFetched,
            this, &WindowsISODownloadDialog::onProductPageFetched);
    connect(m_downloader, &WindowsISODownloader::languagesFetched,
            this, &WindowsISODownloadDialog::onLanguagesFetched);
    connect(m_downloader, &WindowsISODownloader::downloadUrlReceived,
            this, &WindowsISODownloadDialog::onDownloadUrlReceived);
    connect(m_downloader, &WindowsISODownloader::downloadProgress,
            this, &WindowsISODownloadDialog::onDownloadProgress);
    connect(m_downloader, &WindowsISODownloader::downloadComplete,
            this, &WindowsISODownloadDialog::onDownloadComplete);
    connect(m_downloader, &WindowsISODownloader::downloadError,
            this, &WindowsISODownloadDialog::onDownloadError);
    connect(m_downloader, &WindowsISODownloader::statusMessage,
            this, &WindowsISODownloadDialog::onStatusMessage);
    
    connect(m_languageCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &WindowsISODownloadDialog::updateDownloadButton);
    connect(m_architectureCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &WindowsISODownloadDialog::updateDownloadButton);
}

void WindowsISODownloadDialog::onProductPageFetched(const QString& sessionId, const QString& productEditionId) {
    Q_UNUSED(sessionId);
    Q_UNUSED(productEditionId);
    
    m_sessionInitialized = true;
    m_statusLabel->setText("Session initialized, fetching languages...");
    m_downloader->fetchAvailableLanguages();
}

void WindowsISODownloadDialog::onLanguagesFetched(const QStringList& languages) {
    m_languageCombo->clear();
    m_languageCombo->addItems(languages);
    m_languageCombo->setEnabled(true);
    m_languagesLoaded = true;
    
    // Try to select English by default
    int englishIndex = m_languageCombo->findText("English", Qt::MatchContains);
    if (englishIndex >= 0) {
        m_languageCombo->setCurrentIndex(englishIndex);
    }
    
    m_statusLabel->setText("Ready to download");
    updateDownloadButton();
}

void WindowsISODownloadDialog::onDownloadUrlReceived(const QString& url, const QDateTime& expiresAt) {
    m_downloadUrl = url;
    m_urlExpiresAt = expiresAt;
    
    QString savePath = m_saveLocationEdit->text();
    if (savePath.isEmpty()) {
        savePath = getDefaultSavePath();
        m_saveLocationEdit->setText(savePath);
    }
    
    m_statusLabel->setText("Starting download...");
    m_downloader->downloadISO(url, savePath);
    
    m_isDownloading = true;
    m_downloadButton->setEnabled(false);
    m_cancelButton->setEnabled(true);
    m_languageCombo->setEnabled(false);
    m_architectureCombo->setEnabled(false);
    m_browseSaveButton->setEnabled(false);
}

void WindowsISODownloadDialog::onDownloadProgress(qint64 bytesReceived, qint64 bytesTotal, double speedMBps) {
    if (bytesTotal > 0) {
        int percentage = static_cast<int>((bytesReceived * 100) / bytesTotal);
        m_progressBar->setValue(percentage);
        
        double receivedGB = bytesReceived / (1024.0 * 1024.0 * 1024.0);
        double totalGB = bytesTotal / (1024.0 * 1024.0 * 1024.0);
        m_sizeLabel->setText(QString("Downloaded: %1 GB / %2 GB")
            .arg(receivedGB, 0, 'f', 2)
            .arg(totalGB, 0, 'f', 2));
        
        m_speedLabel->setText(QString("Speed: %1 MB/s").arg(speedMBps, 0, 'f', 2));
        
        if (speedMBps > 0) {
            qint64 remainingBytes = bytesTotal - bytesReceived;
            double remainingMB = remainingBytes / (1024.0 * 1024.0);
            int etaSeconds = static_cast<int>(remainingMB / speedMBps);
            
            int minutes = etaSeconds / 60;
            int seconds = etaSeconds % 60;
            m_statusLabel->setText(QString("Downloading... ETA: %1:%2")
                .arg(minutes)
                .arg(seconds, 2, 10, QChar('0')));
        }
    }
}

void WindowsISODownloadDialog::onDownloadComplete(const QString& filePath, qint64 fileSize) {
    m_downloadedFilePath = filePath;
    m_isDownloading = false;
    
    m_progressBar->setValue(100);
    double sizeGB = fileSize / (1024.0 * 1024.0 * 1024.0);
    m_statusLabel->setText(QString("Download complete! (%1 GB)").arg(sizeGB, 0, 'f', 2));
    m_sizeLabel->setText("");
    m_speedLabel->setText("");
    
    m_cancelButton->setEnabled(false);
    
    QMessageBox::information(this, "Download Complete",
        QString("Windows 11 ISO has been downloaded successfully!\n\nSaved to: %1\n\nClick OK to use this image.")
            .arg(filePath));
    
    Q_EMIT downloadCompleted(filePath);
    accept();
}

void WindowsISODownloadDialog::onDownloadError(const QString& error) {
    m_isDownloading = false;
    
    m_statusLabel->setText(QString("Error: %1").arg(error));
    m_cancelButton->setEnabled(false);
    m_downloadButton->setEnabled(true);
    m_languageCombo->setEnabled(true);
    m_architectureCombo->setEnabled(true);
    m_browseSaveButton->setEnabled(true);
    
    QMessageBox::critical(this, "Download Error",
        QString("Failed to download Windows 11 ISO:\n\n%1\n\nPlease try again or check your internet connection.")
            .arg(error));
}

void WindowsISODownloadDialog::onStatusMessage(const QString& message) {
    if (!m_isDownloading) {
        m_statusLabel->setText(message);
    }
}

void WindowsISODownloadDialog::onBrowseSaveLocation() {
    QString defaultPath = m_saveLocationEdit->text();
    if (defaultPath.isEmpty()) {
        defaultPath = getDefaultSavePath();
    }
    
    QString filePath = QFileDialog::getSaveFileName(
        this,
        "Save Windows 11 ISO",
        defaultPath,
        "ISO Files (*.iso);;All Files (*.*)"
    );
    
    if (!filePath.isEmpty()) {
        if (!filePath.endsWith(".iso", Qt::CaseInsensitive)) {
            filePath += ".iso";
        }
        m_saveLocationEdit->setText(filePath);
    }
}

void WindowsISODownloadDialog::onStartDownload() {
    if (!m_languagesLoaded || m_languageCombo->currentText().isEmpty()) {
        QMessageBox::warning(this, "Invalid Selection", "Please select a language.");
        return;
    }
    
    QString savePath = m_saveLocationEdit->text();
    if (savePath.isEmpty()) {
        QMessageBox::warning(this, "Invalid Path", "Please specify a save location.");
        return;
    }
    
    QString language = m_languageCombo->currentText();
    QString architecture = m_architectureCombo->currentData().toString();
    
    m_statusLabel->setText("Requesting download URL...");
    m_downloadButton->setEnabled(false);
    
    m_downloader->requestDownloadUrl(language, architecture);
}

void WindowsISODownloadDialog::onCancelDownload() {
    auto reply = QMessageBox::question(
        this,
        "Cancel Download",
        "Are you sure you want to cancel the download?",
        QMessageBox::Yes | QMessageBox::No
    );
    
    if (reply == QMessageBox::Yes) {
        m_downloader->cancel();
        m_isDownloading = false;
        m_statusLabel->setText("Download cancelled");
        m_cancelButton->setEnabled(false);
        m_downloadButton->setEnabled(true);
        m_languageCombo->setEnabled(true);
        m_architectureCombo->setEnabled(true);
        m_browseSaveButton->setEnabled(true);
    }
}

void WindowsISODownloadDialog::updateDownloadButton() {
    bool canDownload = m_languagesLoaded 
                    && !m_languageCombo->currentText().isEmpty()
                    && !m_isDownloading;
    m_downloadButton->setEnabled(canDownload);
}

QString WindowsISODownloadDialog::getDefaultSavePath() {
    QString downloadsPath = QStandardPaths::writableLocation(QStandardPaths::DownloadLocation);
    
    QString language = m_languageCombo->currentText();
    if (language.isEmpty()) {
        language = "Windows11";
    } else {
        language.replace(" ", "_");
    }
    
    QString architecture = m_architectureCombo->currentData().toString();
    
    QString fileName = QString("Win11_%1_%2.iso").arg(language).arg(architecture);
    return QDir(downloadsPath).filePath(fileName);
}
