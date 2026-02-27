// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/**
 * @file test_windows_iso_downloader.cpp
 * @brief Tests for UUP dump-based Windows ISO downloader
 */

#include <QtTest/QtTest>
#include "sak/windows_iso_downloader.h"
#include "sak/uup_dump_api.h"
#include <QSignalSpy>

class WindowsISODownloaderTests : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void initTestCase();
    void cleanupTestCase();
    void init();
    void cleanup();

    // Static helpers
    void testAvailableArchitectures();
    void testAvailableChannels();

    // UUP dump API (network required)
    void testFetchBuilds();
    void testCancel();

    // Channel display names
    void testChannelDisplayNames();

    // UUP dump API file fetching (exercises URL validation)
    void testGetFilesReturnsResults();
    void testFileUrlsAreValid();

private:
    WindowsISODownloader* downloader = nullptr;
    UupDumpApi* api = nullptr;
};

void WindowsISODownloaderTests::initTestCase()
{
    qInfo() << "=== Windows ISO Downloader Tests (UUP dump) ===";
}

void WindowsISODownloaderTests::cleanupTestCase()
{
    qInfo() << "=== All tests completed ===";
}

void WindowsISODownloaderTests::init()
{
    downloader = new WindowsISODownloader(this);
    api = new UupDumpApi(this);
}

void WindowsISODownloaderTests::cleanup()
{
    if (downloader) {
        downloader->cancel();
        delete downloader;
        downloader = nullptr;
    }
    if (api) {
        api->cancelAll();
        delete api;
        api = nullptr;
    }
}

/**
 * Test availableArchitectures()
 * Should return amd64 and arm64.
 */
void WindowsISODownloaderTests::testAvailableArchitectures()
{
    auto archs = WindowsISODownloader::availableArchitectures();
    QCOMPARE(archs.size(), 2);
    QVERIFY(archs.contains("amd64"));
    QVERIFY(archs.contains("arm64"));
}

/**
 * Test availableChannels()
 * Should return 5 channels.
 */
void WindowsISODownloaderTests::testAvailableChannels()
{
    auto channels = WindowsISODownloader::availableChannels();
    QCOMPARE(channels.size(), 5);
}

/**
 * Test fetchBuilds() with real network call to UUP dump API.
 * Should emit buildsFetched with at least one result.
 */
void WindowsISODownloaderTests::testFetchBuilds()
{
    QSignalSpy buildsSpy(downloader, &WindowsISODownloader::buildsFetched);
    QSignalSpy errorSpy(downloader, &WindowsISODownloader::downloadError);

    downloader->fetchBuilds("amd64", UupDumpApi::ReleaseChannel::Retail);

    // Wait up to 15 seconds for API response
    bool ok = buildsSpy.wait(15000) || errorSpy.count() > 0;

    if (errorSpy.count() > 0) {
        qWarning() << "API error:" << errorSpy.at(0).at(0).toString();
        QSKIP("UUP dump API unreachable - skipping network test");
    }

    QVERIFY2(ok, "Timeout waiting for buildsFetched signal");
    QCOMPARE(buildsSpy.count(), 1);

    auto builds = buildsSpy.at(0).at(0).value<QList<UupDumpApi::BuildInfo>>();
    qInfo() << "Builds fetched:" << builds.size();
    QVERIFY2(!builds.isEmpty(), "Expected at least one Retail build");

    // Verify build info fields
    const auto& first = builds.first();
    QVERIFY(!first.uuid.isEmpty());
    QVERIFY(!first.build.isEmpty());
    QCOMPARE(first.arch, "amd64");
}

/**
 * Test cancel() does not crash.
 */
void WindowsISODownloaderTests::testCancel()
{
    downloader->fetchBuilds("amd64", UupDumpApi::ReleaseChannel::Retail);
    QTest::qWait(500);
    downloader->cancel();
    // No crash is success
    QVERIFY(true);
}

/**
 * Test channel display names are non-empty.
 */
void WindowsISODownloaderTests::testChannelDisplayNames()
{
    for (auto ch : UupDumpApi::allChannels()) {
        QString name = UupDumpApi::channelToDisplayName(ch);
        QVERIFY2(!name.isEmpty(),
                 qPrintable(QString("Empty name for channel %1")
                                .arg(static_cast<int>(ch))));
    }
}

/**
 * Test getFiles() returns non-empty file list.
 * Verifies the URL validation logic accepts Microsoft CDN HTTP URLs.
 */
void WindowsISODownloaderTests::testGetFilesReturnsResults()
{
    // First fetch a build UUID from the API
    QSignalSpy buildsSpy(api, &UupDumpApi::buildsFetched);
    QSignalSpy buildErrorSpy(api, &UupDumpApi::apiError);

    api->fetchAvailableBuilds("amd64", UupDumpApi::ReleaseChannel::Retail);

    bool ok = buildsSpy.wait(15000) || buildErrorSpy.count() > 0;
    if (buildErrorSpy.count() > 0 || !ok) {
        QSKIP("UUP dump API unreachable - skipping network test");
    }

    auto builds = buildsSpy.at(0).at(0).value<QList<UupDumpApi::BuildInfo>>();
    if (builds.isEmpty()) {
        QSKIP("No builds returned - skipping");
    }

    // Now fetch files for the first build
    QString uuid = builds.first().uuid;
    QSignalSpy filesSpy(api, &UupDumpApi::filesFetched);
    QSignalSpy fileErrorSpy(api, &UupDumpApi::apiError);

    api->getFiles(uuid, "en-us", "PROFESSIONAL");

    ok = filesSpy.wait(15000) || fileErrorSpy.count() > 0;
    if (fileErrorSpy.count() > 0) {
        qWarning() << "API error:" << fileErrorSpy.at(0).at(0).toString();
        QSKIP("getFiles API call failed - skipping");
    }

    QVERIFY2(ok, "Timeout waiting for filesFetched signal");
    QCOMPARE(filesSpy.count(), 1);

    auto files = filesSpy.at(0).at(1).value<QList<UupDumpApi::FileInfo>>();
    qInfo() << "Files fetched:" << files.size();
    QVERIFY2(!files.isEmpty(),
             "Expected non-empty file list (URL validation may be rejecting valid Microsoft CDN URLs)");
}

/**
 * Test that file URLs from getFiles() are well-formed.
 * Verifies filenames are safe (no path traversal) and URLs are valid.
 */
void WindowsISODownloaderTests::testFileUrlsAreValid()
{
    // First fetch a build UUID
    QSignalSpy buildsSpy(api, &UupDumpApi::buildsFetched);
    QSignalSpy buildErrorSpy(api, &UupDumpApi::apiError);

    api->fetchAvailableBuilds("amd64", UupDumpApi::ReleaseChannel::Retail);

    bool ok = buildsSpy.wait(15000) || buildErrorSpy.count() > 0;
    if (buildErrorSpy.count() > 0 || !ok) {
        QSKIP("UUP dump API unreachable - skipping network test");
    }

    auto builds = buildsSpy.at(0).at(0).value<QList<UupDumpApi::BuildInfo>>();
    if (builds.isEmpty()) {
        QSKIP("No builds returned - skipping");
    }

    QString uuid = builds.first().uuid;
    QSignalSpy filesSpy(api, &UupDumpApi::filesFetched);
    QSignalSpy fileErrorSpy(api, &UupDumpApi::apiError);

    api->getFiles(uuid, "en-us", "PROFESSIONAL");

    ok = filesSpy.wait(15000) || fileErrorSpy.count() > 0;
    if (fileErrorSpy.count() > 0 || !ok) {
        QSKIP("getFiles API call failed - skipping");
    }

    auto files = filesSpy.at(0).at(1).value<QList<UupDumpApi::FileInfo>>();
    if (files.isEmpty()) {
        QSKIP("No files returned - skipping");
    }

    for (const auto& f : files) {
        // Filename must not contain path traversal
        QVERIFY2(!f.fileName.contains(".."),
                 qPrintable("Path traversal in filename: " + f.fileName));
        QVERIFY2(!f.fileName.contains('/') && !f.fileName.contains('\\'),
                 qPrintable("Directory separator in filename: " + f.fileName));

        // URL must be valid
        QUrl url(f.url);
        QVERIFY2(url.isValid(), qPrintable("Invalid URL for: " + f.fileName));

        // URL must be HTTP or HTTPS
        QString scheme = url.scheme().toLower();
        QVERIFY2(scheme == "http" || scheme == "https",
                 qPrintable("Unexpected scheme " + scheme + " for: " + f.fileName));

        // SHA-1 checksum must be present
        QVERIFY2(!f.sha1.isEmpty(),
                 qPrintable("Missing SHA-1 for: " + f.fileName));
    }

    qInfo() << "Validated" << files.size() << "file URLs successfully";
}

#include <QApplication>
int main(int argc, char* argv[])
{
    QApplication app(argc, argv);
    WindowsISODownloaderTests tc;
    QTEST_SET_MAIN_SOURCE_PATH
    return QTest::qExec(&tc, argc, argv);
}

#include "test_windows_iso_downloader.moc"

