// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: MIT

#include <QtTest/QtTest>
#include <QNetworkAccessManager>
#include <QNetworkRequest>
#include <QNetworkReply>
#include <QEventLoop>
#include <QTimer>
#include <QRegularExpression>

/**
 * @brief Test Mac User-Agent trick for direct Windows 11 ISO download links
 * 
 * Microsoft's Windows 11 download page shows direct ISO download links when
 * accessed from macOS (Safari browser), because Mac users can't run the
 * Windows Media Creation Tool.
 */
class TestMacUADownload : public QObject {
    Q_OBJECT

private slots:
    void initTestCase() {
        m_manager = new QNetworkAccessManager(this);
    }

    void cleanupTestCase() {
        delete m_manager;
    }

    /**
     * @brief Test fetching Windows 11 download page with Mac Safari User-Agent
     * 
     * When accessing https://www.microsoft.com/en-us/software-download/windows11
     * with Safari UA, Microsoft should return HTML with direct ISO download links.
     */
    void testFetchDownloadPageWithMacUA() {
        const QString macSafariUA = "Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) "
                                     "AppleWebKit/605.1.15 (KHTML, like Gecko) "
                                     "Version/17.2 Safari/605.1.15";
        
        QUrl url("https://www.microsoft.com/en-us/software-download/windows11");
        QNetworkRequest request(url);
        request.setHeader(QNetworkRequest::UserAgentHeader, macSafariUA);
        
        qDebug() << "\n=== Testing Mac UA Download Page ===";
        qDebug() << "URL:" << url.toString();
        qDebug() << "User-Agent:" << macSafariUA;
        
        QNetworkReply* reply = m_manager->get(request);
        
        QEventLoop loop;
        QTimer timeout;
        timeout.setSingleShot(true);
        timeout.setInterval(30000); // 30 second timeout
        
        connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
        connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
        
        timeout.start();
        loop.exec();
        
        QVERIFY2(!timeout.isActive() || reply->isFinished(), "Request timed out");
        timeout.stop();
        
        // Check response
        int statusCode = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        qDebug() << "Status Code:" << statusCode;
        
        if (reply->error() != QNetworkReply::NoError) {
            qDebug() << "Error:" << reply->errorString();
        }
        
        QCOMPARE(reply->error(), QNetworkReply::NoError);
        QCOMPARE(statusCode, 200);
        
        // Read response body
        QByteArray responseData = reply->readAll();
        QString html = QString::fromUtf8(responseData);
        
        qDebug() << "Response size:" << responseData.size() << "bytes";
        qDebug() << "HTML preview (first 500 chars):";
        qDebug() << html.left(500);
        
        // Look for ISO download links in HTML
        // Microsoft typically provides direct links like:
        // https://software.download.prss.microsoft.com/...Win11_25H2_English_x64.iso
        QRegularExpression isoLinkRegex(
            R"(https://[^"'\s]+\.iso)",
            QRegularExpression::CaseInsensitiveOption
        );
        
        QRegularExpressionMatchIterator matches = isoLinkRegex.globalMatch(html);
        QStringList isoLinks;
        
        while (matches.hasNext()) {
            QRegularExpressionMatch match = matches.next();
            QString link = match.captured(0);
            isoLinks.append(link);
        }
        
        qDebug() << "\n=== Found ISO Links ===";
        qDebug() << "Total ISO links found:" << isoLinks.size();
        
        for (int i = 0; i < isoLinks.size(); ++i) {
            qDebug() << "Link" << (i+1) << ":" << isoLinks[i];
        }
        
        // Also look for download buttons or product editions
        QRegularExpression editionRegex(
            R"(Windows\s+11\s+(?:Home|Pro|Enterprise)?.*?(?:64-bit|x64))",
            QRegularExpression::CaseInsensitiveOption
        );
        
        QRegularExpressionMatchIterator editionMatches = editionRegex.globalMatch(html);
        qDebug() << "\n=== Found Windows 11 References ===";
        
        int editionCount = 0;
        while (editionMatches.hasNext() && editionCount < 10) {
            QRegularExpressionMatch match = editionMatches.next();
            qDebug() << "Reference" << (editionCount+1) << ":" << match.captured(0);
            editionCount++;
        }
        
        // Check if page contains download-related elements
        bool hasDownloadButton = html.contains("download", Qt::CaseInsensitive);
        bool hasISOReference = html.contains(".iso", Qt::CaseInsensitive);
        bool hasWindows11 = html.contains("Windows 11", Qt::CaseInsensitive);
        
        qDebug() << "\n=== Page Content Analysis ===";
        qDebug() << "Has 'download' text:" << hasDownloadButton;
        qDebug() << "Has '.iso' reference:" << hasISOReference;
        qDebug() << "Has 'Windows 11' text:" << hasWindows11;
        
        // We should find at least some Windows 11 references
        QVERIFY2(hasWindows11, "Page should contain Windows 11 references");
        
        // Save HTML for manual inspection
        QFile htmlFile("mac_ua_response.html");
        if (htmlFile.open(QIODevice::WriteOnly | QIODevice::Text)) {
            htmlFile.write(responseData);
            htmlFile.close();
            qDebug() << "\nSaved HTML response to: mac_ua_response.html";
        }
        
        reply->deleteLater();
    }

private:
    QNetworkAccessManager* m_manager{nullptr};
};

QTEST_MAIN(TestMacUADownload)
#include "test_mac_ua_download.moc"
