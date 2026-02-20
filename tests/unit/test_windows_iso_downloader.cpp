// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: MIT

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

private:
    WindowsISODownloader* downloader = nullptr;
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
}

void WindowsISODownloaderTests::cleanup()
{
    if (downloader) {
        downloader->cancel();
        delete downloader;
        downloader = nullptr;
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

#include <QApplication>
int main(int argc, char* argv[])
{
    QApplication app(argc, argv);
    WindowsISODownloaderTests tc;
    QTEST_SET_MAIN_SOURCE_PATH
    return QTest::qExec(&tc, argc, argv);
}

#include "test_windows_iso_downloader.moc"

