// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/**
 * @file test_windows_iso_downloader.cpp
 * @brief Tests for UUP dump-based Windows ISO downloader
 */

#include "sak/uup_dump_api.h"
#include "sak/windows_iso_downloader.h"

#include <QSet>
#include <QSignalSpy>
#include <QtTest/QtTest>

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

    void validateFileEntry(const UupDumpApi::FileInfo& entry);
};

void WindowsISODownloaderTests::initTestCase() {
    qInfo() << "=== Windows ISO Downloader Tests (UUP dump) ===";
}

void WindowsISODownloaderTests::cleanupTestCase() {
    qInfo() << "=== All tests completed ===";
}

void WindowsISODownloaderTests::init() {
    // The tests that hit the third-party UUP dump API over the network are opt-in, so the
    // automated suite skips them DETERMINISTICALLY (a network-dependent pass/skip is exactly the
    // accidental environment dependence G18-5 bans, and it made the skip baseline flaky). A
    // technician runs the real API by setting SAK_RUN_LIVE_UUP_TESTS. Centralised here so the
    // guard lives in one place rather than three test bodies.
    static const QSet<QString> live_network_tests = {QStringLiteral("testFetchBuilds"),
                                                     QStringLiteral("testGetFilesReturnsResults"),
                                                     QStringLiteral("testFileUrlsAreValid")};
    if (live_network_tests.contains(QString::fromLatin1(QTest::currentTestFunction())) &&
        !qEnvironmentVariableIsSet("SAK_RUN_LIVE_UUP_TESTS")) {
        QSKIP("Live UUP dump API network test; set SAK_RUN_LIVE_UUP_TESTS=1 to run");
    }
    downloader = new WindowsISODownloader(this);
    api = new UupDumpApi(this);
}

void WindowsISODownloaderTests::cleanup() {
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
void WindowsISODownloaderTests::testAvailableArchitectures() {
    auto archs = WindowsISODownloader::availableArchitectures();
    QCOMPARE(archs.size(), 2);
    QVERIFY(archs.contains("amd64"));
    QVERIFY(archs.contains("arm64"));
}

/**
 * Test availableChannels()
 * Should return exactly the five UUP dump release rings.
 */
void WindowsISODownloaderTests::testAvailableChannels() {
    auto channels = WindowsISODownloader::availableChannels();
    QCOMPARE(channels.size(), 5);
    // Which five, not just how many: dropping Canary while duplicating Dev keeps the count at
    // 5 and silently changes what the wizard can offer, which the bare size check accepted.
    QVERIFY(channels.contains(UupDumpApi::ReleaseChannel::Retail));
    QVERIFY(channels.contains(UupDumpApi::ReleaseChannel::ReleasePreview));
    QVERIFY(channels.contains(UupDumpApi::ReleaseChannel::Beta));
    QVERIFY(channels.contains(UupDumpApi::ReleaseChannel::Dev));
    QVERIFY(channels.contains(UupDumpApi::ReleaseChannel::Canary));
}

/**
 * Test fetchBuilds() with real network call to UUP dump API.
 * Should emit buildsFetched with at least one result.
 */
void WindowsISODownloaderTests::testFetchBuilds() {
    // Opt-in live network test; init() skips it unless SAK_RUN_LIVE_UUP_TESTS is set.
    QSignalSpy buildsSpy(downloader, &WindowsISODownloader::buildsFetched);
    QSignalSpy errorSpy(downloader, &WindowsISODownloader::downloadError);

    downloader->fetchBuilds("amd64", UupDumpApi::ReleaseChannel::Retail);

    // Wait up to 15 seconds for whichever signal lands first. QSignalSpy::wait()
    // latches the count on entry, so an emission that already arrived is invisible to
    // it, and it never looks at the error spy -- an early apiError would still burn the
    // whole timeout. Polling the absolute counts settles as soon as either one fires.
    QTRY_VERIFY_WITH_TIMEOUT(buildsSpy.count() > 0 || errorSpy.count() > 0, 15'000);

    if (errorSpy.count() > 0) {
        qWarning() << "API error:" << errorSpy.at(0).at(0).toString();
        QSKIP("UUP dump API unreachable - skipping network test");
    }

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
 * Test cancel() aborts an in-flight API fetch.
 *
 * The primary claim is a CRASH regression: cancelAll() aborts every pending QNetworkReply, and
 * abort() delivers finished() SYNCHRONOUSLY, whose slot removes the reply from the very list
 * being walked. Iterating that list directly is use-after-free / iterator invalidation, so
 * "the process is still alive after this line" is the assertion QVERIFY(true) stands for.
 * The spy makes the functional half non-vacuous: once aborted, the fetch must never deliver
 * results. (It can be latched before the cancel if the network was fast, hence the baseline.)
 */
void WindowsISODownloaderTests::testCancel() {
    QSignalSpy buildsSpy(downloader, &WindowsISODownloader::buildsFetched);
    downloader->fetchBuilds("amd64", UupDumpApi::ReleaseChannel::Retail);
    QTest::qWait(500);

    const auto delivered_before_cancel = buildsSpy.count();
    downloader->cancel();
    QTest::qWait(500);

    // An aborted fetch reports an apiError, never a buildsFetched: the count cannot grow.
    // (isDownloading() is deliberately NOT asserted here -- fetchBuilds never sets the
    // downloading flag, so it reads false with or without the cancel.)
    QCOMPARE(buildsSpy.count(), delivered_before_cancel);
    // Surviving the abort/teardown above (no crash, no hang) is the rest of the claim.
    QVERIFY(true);
}

/**
 * Test channel display names are non-empty.
 */
void WindowsISODownloaderTests::testChannelDisplayNames() {
    for (auto ch : UupDumpApi::allChannels()) {
        QString name = UupDumpApi::channelToDisplayName(ch);
        QVERIFY2(!name.isEmpty(),
                 qPrintable(QString("Empty name for channel %1").arg(static_cast<int>(ch))));
    }
}

/**
 * Test getFiles() returns non-empty file list.
 * Verifies the URL validation logic accepts Microsoft CDN HTTP URLs.
 */
void WindowsISODownloaderTests::testGetFilesReturnsResults() {
    // Opt-in live network test; init() skips it unless SAK_RUN_LIVE_UUP_TESTS is set.
    // First fetch a build UUID from the API
    QSignalSpy buildsSpy(api, &UupDumpApi::buildsFetched);
    QSignalSpy buildErrorSpy(api, &UupDumpApi::apiError);

    api->fetchAvailableBuilds("amd64", UupDumpApi::ReleaseChannel::Retail);

    // Poll the absolute counts for the same reason as testFetchBuilds. This site skips
    // rather than fails when nothing arrives, so it needs qWaitFor's return value; a
    // QTRY macro would turn an unreachable API into a hard failure.
    const bool ok = QTest::qWaitFor(
        [&]() { return buildsSpy.count() > 0 || buildErrorSpy.count() > 0; }, 15'000);
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

    QTRY_VERIFY_WITH_TIMEOUT(filesSpy.count() > 0 || fileErrorSpy.count() > 0, 15'000);
    if (fileErrorSpy.count() > 0) {
        qWarning() << "API error:" << fileErrorSpy.at(0).at(0).toString();
        QSKIP("getFiles API call failed - skipping");
    }

    QCOMPARE(filesSpy.count(), 1);

    auto files = filesSpy.at(0).at(1).value<QList<UupDumpApi::FileInfo>>();
    qInfo() << "Files fetched:" << files.size();
    QVERIFY2(!files.isEmpty(),
             "Expected non-empty file list (URL validation may be rejecting valid Microsoft CDN "
             "URLs)");
}

/**
 * Test that file URLs from getFiles() are well-formed.
 * Verifies filenames are safe (no path traversal) and URLs are valid.
 */
void WindowsISODownloaderTests::testFileUrlsAreValid() {
    // Opt-in live network test; init() skips it unless SAK_RUN_LIVE_UUP_TESTS is set.
    // First fetch a build UUID
    QSignalSpy buildsSpy(api, &UupDumpApi::buildsFetched);
    QSignalSpy buildErrorSpy(api, &UupDumpApi::apiError);

    api->fetchAvailableBuilds("amd64", UupDumpApi::ReleaseChannel::Retail);

    const bool ok = QTest::qWaitFor(
        [&]() { return buildsSpy.count() > 0 || buildErrorSpy.count() > 0; }, 15'000);
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

    const bool filesOk =
        QTest::qWaitFor([&]() { return filesSpy.count() > 0 || fileErrorSpy.count() > 0; }, 15'000);
    if (fileErrorSpy.count() > 0 || !filesOk) {
        QSKIP("getFiles API call failed - skipping");
    }

    auto files = filesSpy.at(0).at(1).value<QList<UupDumpApi::FileInfo>>();
    if (files.isEmpty()) {
        QSKIP("No files returned - skipping");
    }

    for (const auto& file_entry : files) {
        validateFileEntry(file_entry);
    }

    qInfo() << "Validated" << files.size() << "file URLs successfully";
}

void WindowsISODownloaderTests::validateFileEntry(const UupDumpApi::FileInfo& entry) {
    QVERIFY2(!entry.fileName.contains(".."),
             qPrintable("Path traversal in filename: " + entry.fileName));
    QVERIFY2(!entry.fileName.contains('/') && !entry.fileName.contains('\\'),
             qPrintable("Directory separator in filename: " + entry.fileName));

    QUrl url(entry.url);
    QVERIFY2(url.isValid(), qPrintable("Invalid URL for: " + entry.fileName));

    QString scheme = url.scheme().toLower();
    QVERIFY2(scheme == "http" || scheme == "https",
             qPrintable("Unexpected scheme " + scheme + " for: " + entry.fileName));

    QVERIFY2(!entry.sha1.isEmpty(), qPrintable("Missing SHA-1 for: " + entry.fileName));
}

#include <QApplication>
int main(int argc, char* argv[]) {
    QApplication app(argc, argv);
    WindowsISODownloaderTests tc;
    QTEST_SET_MAIN_SOURCE_PATH
    return QTest::qExec(&tc, argc, argv);
}

#include "test_windows_iso_downloader.moc"
