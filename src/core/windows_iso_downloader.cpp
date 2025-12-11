// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: MIT

#include "sak/windows_iso_downloader.h"
#include "sak/logger.h"
#include <QNetworkRequest>
#include <QUrlQuery>
#include <QRegularExpression>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QUuid>
#include <QDir>
#include <QTimer>

// API constants
const QString WindowsISODownloader::PRODUCT_PAGE_URL = 
    "https://www.microsoft.com/en-us/software-download/windows11";

const QString WindowsISODownloader::API_BASE_URL = 
    "https://www.microsoft.com/software-download-connector/api";

const QString WindowsISODownloader::USER_AGENT = 
    "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36";

WindowsISODownloader::WindowsISODownloader(QObject* parent)
    : QObject(parent)
    , m_networkManager(new QNetworkAccessManager(this))
    , m_currentReply(nullptr)
    , m_downloadFile(nullptr)
    , m_skuId("")
    , m_orgId("y6jn8c31")
    , m_profileId("606624d44113")
    , m_productEditionId("3262")
    , m_bytesReceived(0)
    , m_bytesTotal(0)
    , m_speedMBps(0.0)
    , m_lastSpeedUpdate(0)
    , m_lastSpeedBytes(0)
    , m_isCancelled(false)
    , m_retryCount(0)
    , m_maxRetries(3)
    , m_retryDelayMs(2000)
    , m_pendingRetry(RetryOperation::None)
{
}

WindowsISODownloader::~WindowsISODownloader() {
    cancel();
    
    if (m_downloadFile) {
        m_downloadFile->close();
        delete m_downloadFile;
    }
}

void WindowsISODownloader::fetchProductPage() {
    m_sessionId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    
    sak::log_info(QString("Whitelisting session ID: %1 (attempt %2/%3)")
        .arg(m_sessionId).arg(m_retryCount + 1).arg(m_maxRetries + 1).toStdString());
    Q_EMIT statusMessage("Initializing download session...");

    QString url = QString("https://vlscppe.microsoft.com/tags?org_id=%1&session_id=%2")
        .arg(m_orgId, m_sessionId);
    
    QNetworkRequest netRequest{QUrl(url)};
    netRequest.setHeader(QNetworkRequest::UserAgentHeader, USER_AGENT);
    netRequest.setTransferTimeout(15000); // 15 second timeout
    
    m_currentReply = m_networkManager->get(netRequest);
    connect(m_currentReply, &QNetworkReply::finished, 
            this, &WindowsISODownloader::onProductPageFinished);
    connect(m_currentReply, &QNetworkReply::errorOccurred,
            this, &WindowsISODownloader::onNetworkError);
}

void WindowsISODownloader::fetchAvailableLanguages() {
    if (m_sessionId.isEmpty()) {
        sak::log_error("Must call fetchProductPage first");
        Q_EMIT downloadError("Session not initialized");
        return;
    }
    
    sak::log_info(QString("Fetching available SKUs/languages (attempt %1/%2)")
        .arg(m_retryCount + 1).arg(m_maxRetries + 1).toStdString());
    Q_EMIT statusMessage("Getting available languages...");
    
    QString url = QString("%1/getskuinformationbyproductedition?profile=%2&productEditionId=%3&SKU=undefined&friendlyFileName=undefined&Locale=en-US&sessionID=%4")
        .arg(API_BASE_URL, m_profileId, m_productEditionId, m_sessionId);
    
    QNetworkRequest netRequest(url);
    netRequest.setHeader(QNetworkRequest::UserAgentHeader, USER_AGENT);
    netRequest.setTransferTimeout(15000); // 15 second timeout
    
    m_currentReply = m_networkManager->get(netRequest);
    connect(m_currentReply, &QNetworkReply::finished,
            this, &WindowsISODownloader::onLanguagesFinished);
    connect(m_currentReply, &QNetworkReply::errorOccurred,
            this, &WindowsISODownloader::onNetworkError);
}

void WindowsISODownloader::requestDownloadUrl(const QString& language, const QString& architecture) {
    if (m_sessionId.isEmpty()) {
        sak::log_error("Must call fetchAvailableLanguages first");
        Q_EMIT downloadError("Session not initialized");
        return;
    }
    
    QString selectedSkuId = m_languageToSkuMap.value(language, m_skuId);
    
    if (selectedSkuId.isEmpty()) {
        sak::log_error(QString("No SKU ID found for language: %1").arg(language).toStdString());
        Q_EMIT downloadError("Invalid language selection");
        return;
    }
    
    sak::log_info(QString("Requesting download URL for %1 %2 (SKU: %3)")
        .arg(language, architecture, selectedSkuId).toStdString());
    Q_EMIT statusMessage(QString("Requesting download link for %1...").arg(language));
    
    QString url = QString("%1/GetProductDownloadLinksBySku?profile=%2&productEditionId=undefined&SKU=%3&friendlyFileName=undefined&Locale=en-US&sessionID=%4")
        .arg(API_BASE_URL, m_profileId, selectedSkuId, m_sessionId);
    
    QNetworkRequest netRequest(url);
    netRequest.setHeader(QNetworkRequest::UserAgentHeader, USER_AGENT);
    netRequest.setRawHeader("Referer", PRODUCT_PAGE_URL.toUtf8());
    netRequest.setTransferTimeout(15000); // 15 second timeout
    
    m_currentReply = m_networkManager->get(netRequest);
    connect(m_currentReply, &QNetworkReply::finished,
            this, &WindowsISODownloader::onDownloadUrlFinished);
    connect(m_currentReply, &QNetworkReply::errorOccurred,
            this, &WindowsISODownloader::onNetworkError);
}

void WindowsISODownloader::downloadISO(const QString& url, const QString& savePath) {
    if (m_currentReply && m_currentReply->isRunning()) {
        sak::log_warning("Download already in progress");
        return;
    }
    
    sak::log_info(QString("Starting download to %1").arg(savePath).toStdString());
    Q_EMIT statusMessage("Starting download...");
    
    m_downloadUrl = url;
    m_savePath = savePath;
    m_isCancelled = false;
    
    // Create download file
    m_downloadFile = new QFile(savePath + ".tmp");
    if (!m_downloadFile->open(QIODevice::WriteOnly)) {
        sak::log_error(QString("Failed to create file: %1").arg(savePath).toStdString());
        Q_EMIT downloadError("Failed to create download file");
        delete m_downloadFile;
        m_downloadFile = nullptr;
        return;
    }
    
    // Start download
    QNetworkRequest netRequest(url);
    netRequest.setHeader(QNetworkRequest::UserAgentHeader, USER_AGENT);
    
    m_currentReply = m_networkManager->get(netRequest);
    connect(m_currentReply, &QNetworkReply::downloadProgress,
            this, &WindowsISODownloader::onDownloadProgress);
    connect(m_currentReply, &QNetworkReply::finished,
            this, &WindowsISODownloader::onDownloadFinished);
    connect(m_currentReply, &QNetworkReply::errorOccurred,
            this, &WindowsISODownloader::onNetworkError);
    connect(m_currentReply, &QNetworkReply::readyRead, [this]() {
        if (m_downloadFile) {
            m_downloadFile->write(m_currentReply->readAll());
        }
    });
    
    m_downloadStartTime.start();
    m_bytesReceived = 0;
    m_lastSpeedUpdate = 0;
    m_lastSpeedBytes = 0;
}

void WindowsISODownloader::cancel() {
    m_isCancelled = true;
    
    if (m_currentReply && m_currentReply->isRunning()) {
        m_currentReply->abort();
        m_currentReply->deleteLater();
        m_currentReply = nullptr;
    }
    
    if (m_downloadFile) {
        m_downloadFile->close();
        m_downloadFile->remove(); // Delete temporary file
        delete m_downloadFile;
        m_downloadFile = nullptr;
    }
}

QStringList WindowsISODownloader::getAvailableLanguages() const {
    return m_availableLanguages;
}

QStringList WindowsISODownloader::getAvailableArchitectures() const {
    return QStringList{"x64", "ARM64"};
}

bool WindowsISODownloader::isDownloadUrlExpired(const QString& url) const {
    QUrl qurl(url);
    QUrlQuery query(qurl);
    
    if (query.hasQueryItem("expires")) {
        QString expiresStr = query.queryItemValue("expires");
        qint64 expiresTimestamp = expiresStr.toLongLong();
        if (expiresTimestamp > 0) {
            QDateTime expiresAt = QDateTime::fromSecsSinceEpoch(expiresTimestamp);
            return QDateTime::currentDateTime() > expiresAt;
        }
    }
    
    if (query.hasQueryItem("P1")) {
        QRegularExpression dateRegex("(\\d{8})");
        QRegularExpressionMatch match = dateRegex.match(url);
        if (match.hasMatch()) {
            QString dateStr = match.captured(1);
            QDateTime urlDate = QDateTime::fromString(dateStr, "yyyyMMdd");
            if (urlDate.isValid()) {
                return QDateTime::currentDateTime() > urlDate.addDays(1);
            }
        }
    }
    
    return false;
}

void WindowsISODownloader::onProductPageFinished() {
    if (!m_currentReply) {
        return;
    }
    
    if (m_currentReply->error() != QNetworkReply::NoError) {
        QString error = m_currentReply->errorString();
        sak::log_error(QString("Session initialization failed: %1").arg(error).toStdString());
        m_currentReply->deleteLater();
        m_currentReply = nullptr;
        scheduleRetry(RetryOperation::FetchProductPage);
        return;
    }
    
    m_currentReply->deleteLater();
    m_currentReply = nullptr;
    m_retryCount = 0; // Reset on success
    
    sak::log_info(QString("Session whitelisted: %1").arg(m_sessionId).toStdString());
    Q_EMIT productPageFetched(m_sessionId, m_productEditionId);
}

void WindowsISODownloader::onLanguagesFinished() {
    if (!m_currentReply) {
        return;
    }
    
    if (m_currentReply->error() != QNetworkReply::NoError) {
        QString error = m_currentReply->errorString();
        sak::log_error(QString("Failed to get languages: %1").arg(error).toStdString());
        m_currentReply->deleteLater();
        m_currentReply = nullptr;
        scheduleRetry(RetryOperation::FetchLanguages);
        return;
    }
    
    QByteArray data = m_currentReply->readAll();
    m_currentReply->deleteLater();
    m_currentReply = nullptr;
    
    sak::log_debug("WindowsISODownloader", QString("SKU response: %1").arg(QString::fromUtf8(data)).toStdString());
    
    QJsonDocument doc = QJsonDocument::fromJson(data);
    if (doc.isNull() || !doc.isObject()) {
        sak::log_error(QString("Failed to parse SKU response. Raw data: %1").arg(QString::fromUtf8(data)).toStdString());
        Q_EMIT downloadError("Invalid response from server");
        return;
    }
    
    QJsonObject obj = doc.object();
    QJsonArray skus = obj["Skus"].toArray();
    
    if (skus.isEmpty()) {
        sak::log_error(QString("No SKUs found in response. Full JSON: %1").arg(QString::fromUtf8(data)).toStdString());
        m_currentReply = nullptr;
        scheduleRetry(RetryOperation::FetchLanguages);
        return;
    }
    
    m_availableLanguages.clear();
    m_languageToSkuMap.clear();
    
    for (const QJsonValue& skuVal : skus) {
        QJsonObject sku = skuVal.toObject();
        QString language = sku["LocalizedLanguage"].toString();
        
        // SKU ID can be either string or number in JSON
        QString skuId;
        QJsonValue idVal = sku["Id"];
        if (idVal.isString()) {
            skuId = idVal.toString();
        } else if (idVal.isDouble()) {
            skuId = QString::number(static_cast<qint64>(idVal.toDouble()));
        }
        
        if (!language.isEmpty() && !skuId.isEmpty()) {
            m_availableLanguages.append(language);
            m_languageToSkuMap[language] = skuId;
            
            sak::log_debug("WindowsISODownloader", QString("Mapped language '%1' to SKU '%2'")
                .arg(language, skuId).toStdString());
            
            if (m_skuId.isEmpty()) {
                m_skuId = skuId;
            }
        }
    }
    
    if (m_availableLanguages.isEmpty()) {
        m_availableLanguages = QStringList{"English (United States)", "English (United Kingdom)"};
        if (m_skuId.isEmpty() && !skus.isEmpty()) {
            QJsonValue idVal = skus[0].toObject()["Id"];
            if (idVal.isString()) {
                m_skuId = idVal.toString();
            } else if (idVal.isDouble()) {
                m_skuId = QString::number(static_cast<qint64>(idVal.toDouble()));
            }
            m_languageToSkuMap[m_availableLanguages[0]] = m_skuId;
        }
    }
    
    sak::log_info(QString("Found %1 languages").arg(m_availableLanguages.size()).toStdString());
    m_retryCount = 0; // Reset on success
    Q_EMIT languagesFetched(m_availableLanguages);
}

void WindowsISODownloader::onDownloadUrlFinished() {
    if (!m_currentReply) {
        return;
    }
    
    if (m_currentReply->error() != QNetworkReply::NoError) {
        QString error = m_currentReply->errorString();
        m_currentReply->deleteLater();
        m_currentReply = nullptr;
        Q_EMIT downloadError(QString("Failed to get download URL: %1").arg(error));
        return;
    }
    
    QByteArray data = m_currentReply->readAll();
    m_currentReply->deleteLater();
    m_currentReply = nullptr;
    
    sak::log_debug("WindowsISODownloader", QString("Download URL response: %1").arg(QString::fromUtf8(data)).toStdString());
    
    QJsonDocument doc = QJsonDocument::fromJson(data);
    if (doc.isNull() || !doc.isObject()) {
        sak::log_error(QString("Failed to parse download URL response. Raw data: %1").arg(QString::fromUtf8(data)).toStdString());
        Q_EMIT downloadError("Invalid response from server");
        return;
    }
    
    QJsonObject obj = doc.object();
    QJsonArray downloadOptions = obj["ProductDownloadOptions"].toArray();
    
    sak::log_debug("WindowsISODownloader", QString("ProductDownloadOptions array size: %1").arg(downloadOptions.size()).toStdString());
    
    if (downloadOptions.isEmpty()) {
        sak::log_error(QString("No download options in response. Full JSON: %1").arg(QString::fromUtf8(doc.toJson())).toStdString());
        Q_EMIT downloadError("No download links available");
        return;
    }
    
    QString downloadUrl = downloadOptions[0].toObject()["Uri"].toString();
    
    if (downloadUrl.isEmpty()) {
        sak::log_error("Failed to extract download URL from response");
        Q_EMIT downloadError("Failed to get download URL. Please try again.");
        return;
    }
    
    QDateTime expiresAt = QDateTime::currentDateTime().addDays(1);
    sak::log_info(QString("Got download URL: %1").arg(downloadUrl).toStdString());
    Q_EMIT downloadUrlReceived(downloadUrl, expiresAt);
}

void WindowsISODownloader::onDownloadProgress(qint64 bytesReceived, qint64 bytesTotal) {
    m_bytesReceived = bytesReceived;
    m_bytesTotal = bytesTotal;
    
    calculateSpeed();
    
    Q_EMIT downloadProgress(bytesReceived, bytesTotal, m_speedMBps);
}

void WindowsISODownloader::onDownloadFinished() {
    if (!m_currentReply || !m_downloadFile) {
        return;
    }
    
    if (m_currentReply->error() != QNetworkReply::NoError || m_isCancelled) {
        m_downloadFile->close();
        m_downloadFile->remove();
        delete m_downloadFile;
        m_downloadFile = nullptr;
        m_currentReply->deleteLater();
        m_currentReply = nullptr;
        return;
    }
    
    // Write any remaining data
    m_downloadFile->write(m_currentReply->readAll());
    m_downloadFile->close();
    
    qint64 fileSize = m_downloadFile->size();
    delete m_downloadFile;
    m_downloadFile = nullptr;
    
    m_currentReply->deleteLater();
    m_currentReply = nullptr;
    
    // Rename from .tmp to final name
    QFile::remove(m_savePath); // Remove if exists
    if (!QFile::rename(m_savePath + ".tmp", m_savePath)) {
        sak::log_error("Failed to rename downloaded file");
        Q_EMIT downloadError("Failed to save downloaded file");
        return;
    }
    
    sak::log_info(QString("Download completed: %1 bytes").arg(fileSize).toStdString());
    Q_EMIT downloadComplete(m_savePath, fileSize);
}

void WindowsISODownloader::onNetworkError(QNetworkReply::NetworkError error) {
    (void)error;
    if (!m_currentReply) {
        return;
    }
    
    QString errorString = m_currentReply->errorString();
    sak::log_error(QString("Network error: %1").arg(errorString).toStdString());
    Q_EMIT downloadError(errorString);
}

QString WindowsISODownloader::parseSessionId(const QString& html) {
    QRegularExpression regex("sessionId[\"']?\\s*[:=]\\s*[\"']([a-f0-9-]+)[\"']", QRegularExpression::CaseInsensitiveOption);
    QRegularExpressionMatch match = regex.match(html);
    if (match.hasMatch()) {
        QString sessionId = match.captured(1);
        if (!sessionId.isEmpty() && sessionId.length() > 10) {
            return sessionId;
        }
    }
    
    QRegularExpression altRegex("id=([a-f0-9-]{32,})", QRegularExpression::CaseInsensitiveOption);
    QRegularExpressionMatch altMatch = altRegex.match(html);
    if (altMatch.hasMatch()) {
        return altMatch.captured(1);
    }
    
    return QUuid::createUuid().toString(QUuid::WithoutBraces);
}

QString WindowsISODownloader::parseProductEditionId(const QString& html) {
    QRegularExpression regex("productEditionId[\"']?\\s*[:=]\\s*[\"']?(\\d+)[\"']?", QRegularExpression::CaseInsensitiveOption);
    QRegularExpressionMatch match = regex.match(html);
    if (match.hasMatch()) {
        return match.captured(1);
    }
    
    QRegularExpression optionRegex("<option[^>]*value=\"(\\d+)\"[^>]*>.*?Windows 11", QRegularExpression::CaseInsensitiveOption);
    QRegularExpressionMatch optionMatch = optionRegex.match(html);
    if (optionMatch.hasMatch()) {
        return optionMatch.captured(1);
    }
    
    return "2618";
}

QStringList WindowsISODownloader::parseLanguages(const QString& html) {
    QStringList languages;
    
    QRegularExpression regex("<option[^>]*value=\"([^\"]+)\"[^>]*>([^<]+)</option>", QRegularExpression::CaseInsensitiveOption);
    QRegularExpressionMatchIterator it = regex.globalMatch(html);
    
    while (it.hasNext()) {
        QRegularExpressionMatch match = it.next();
        QString value = match.captured(1).trimmed();
        QString text = match.captured(2).trimmed();
        
        if (!value.isEmpty() && !text.isEmpty() && value != "0" && text.toLower() != "select one") {
            languages.append(text);
        }
    }
    
    if (languages.isEmpty()) {
        languages = QStringList{
            "English (United States)",
            "English (United Kingdom)",
            "Spanish",
            "French",
            "German",
            "Italian",
            "Portuguese",
            "Japanese",
            "Chinese Simplified",
            "Korean"
        };
    }
    
    return languages;
}

QString WindowsISODownloader::parseDownloadUrl(const QString& html) {
    QRegularExpression urlRegex("https://software-download[^\"'\\s<>]+\\.iso", QRegularExpression::CaseInsensitiveOption);
    QRegularExpressionMatch urlMatch = urlRegex.match(html);
    if (urlMatch.hasMatch()) {
        return urlMatch.captured(0);
    }
    
    QRegularExpression linkRegex("<a[^>]*href=\"(https://[^\"]+\\.iso)\"[^>]*>", QRegularExpression::CaseInsensitiveOption);
    QRegularExpressionMatch linkMatch = linkRegex.match(html);
    if (linkMatch.hasMatch()) {
        return linkMatch.captured(1);
    }
    
    QJsonDocument doc = QJsonDocument::fromJson(html.toUtf8());
    if (!doc.isNull() && doc.isObject()) {
        QJsonObject obj = doc.object();
        if (obj.contains("url")) {
            return obj["url"].toString();
        }
        if (obj.contains("downloadUrl")) {
            return obj["downloadUrl"].toString();
        }
    }
    
    return QString();
}

QDateTime WindowsISODownloader::parseExpirationTime(const QString& url) {
    (void)url;
    // URLs typically expire in 24 hours
    return QDateTime::currentDateTime().addDays(1);
}

void WindowsISODownloader::calculateSpeed() {
    qint64 now = m_downloadStartTime.elapsed();
    
    if (now - m_lastSpeedUpdate < 1000) {
        return;
    }
    
    qint64 bytesDelta = m_bytesReceived - m_lastSpeedBytes;
    qint64 timeDelta = now - m_lastSpeedUpdate;
    
    if (timeDelta > 0) {
        double bytesPerMs = static_cast<double>(bytesDelta) / timeDelta;
        m_speedMBps = (bytesPerMs * 1000.0) / (1024.0 * 1024.0);
    }
    
    m_lastSpeedUpdate = now;
    m_lastSpeedBytes = m_bytesReceived;
}

void WindowsISODownloader::scheduleRetry(RetryOperation operation, const QString& language, const QString& architecture) {
    if (m_retryCount >= m_maxRetries) {
        QString opName;
        switch (operation) {
            case RetryOperation::FetchProductPage: opName = "session initialization"; break;
            case RetryOperation::FetchLanguages: opName = "language fetch"; break;
            case RetryOperation::FetchDownloadUrl: opName = "download URL request"; break;
            default: opName = "operation"; break;
        }
        
        sak::log_error(QString("Max retries (%1) exceeded for %2").arg(m_maxRetries).arg(opName).toStdString());
        Q_EMIT downloadError(QString("Failed after %1 attempts. Please check your internet connection.").arg(m_maxRetries + 1));
        m_retryCount = 0;
        m_pendingRetry = RetryOperation::None;
        return;
    }
    
    m_pendingRetry = operation;
    m_retryLanguage = language;
    m_retryArchitecture = architecture;
    m_retryCount++;
    
    // Exponential backoff: 2s, 4s, 8s
    int delayMs = m_retryDelayMs * (1 << (m_retryCount - 1));
    
    sak::log_info(QString("Retrying in %1ms (attempt %2/%3)")
        .arg(delayMs).arg(m_retryCount + 1).arg(m_maxRetries + 1).toStdString());
    Q_EMIT statusMessage(QString("Retrying in %1 seconds...").arg(delayMs / 1000));
    
    QTimer::singleShot(delayMs, this, &WindowsISODownloader::executeRetry);
}

void WindowsISODownloader::executeRetry() {
    switch (m_pendingRetry) {
        case RetryOperation::FetchProductPage:
            fetchProductPage();
            break;
        case RetryOperation::FetchLanguages:
            fetchAvailableLanguages();
            break;
        case RetryOperation::FetchDownloadUrl:
            requestDownloadUrl(m_retryLanguage, m_retryArchitecture);
            break;
        case RetryOperation::None:
        default:
            break;
    }
    m_pendingRetry = RetryOperation::None;
}







