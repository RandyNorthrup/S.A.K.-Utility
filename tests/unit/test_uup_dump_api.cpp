// Copyright (c) 2025-2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file test_uup_dump_api.cpp
/// @brief Unit tests for UupDumpApi static helpers and channel mappings

#include "sak/network_transfer_runner.h"
#include "sak/uup_dump_api.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTimer>
#include <QtTest/QtTest>

#include <algorithm>
#include <cstring>

namespace {

constexpr int kTestMetadataTimeoutMs = 1000;
constexpr qint64 kTestMetadataResponseCap = 64;
constexpr qint64 kTestSmallMetadataCap = 4;
constexpr qint64 kTestOversizedProgress = 5;
constexpr int kLastSuccessfulHttpStatus = 299;
constexpr int kRedirectHttpStatus = 300;
constexpr int kOkHttpStatus = 200;

class TransportFailureReply final : public QNetworkReply {
public:
    TransportFailureReply(const QNetworkRequest& request,
                          NetworkError initial_error,
                          bool fail_on_read,
                          QObject* parent)
        : QNetworkReply(parent), m_fail_on_read(fail_on_read) {
        setRequest(request);
        setUrl(request.url());
        setAttribute(QNetworkRequest::HttpStatusCodeAttribute, kOkHttpStatus);
        if (initial_error != NoError) {
            setError(initial_error, QStringLiteral("Native transport timeout"));
        }
        open(QIODevice::ReadOnly);
        QTimer::singleShot(0, this, [this]() { finish(); });
    }

    void abort() override {
        if (!isFinished()) {
            setError(OperationCanceledError, QStringLiteral("Transport cancelled"));
            finish();
        }
    }

    [[nodiscard]] qint64 bytesAvailable() const override {
        return m_body.size() - m_offset + QNetworkReply::bytesAvailable();
    }

protected:
    qint64 readData(char* data, qint64 max_size) override {
        const qint64 count = std::min(max_size, m_body.size() - m_offset);
        if (count <= 0) {
            return -1;
        }
        std::memcpy(data, m_body.constData() + m_offset, static_cast<std::size_t>(count));
        m_offset += count;
        if (m_fail_on_read) {
            setError(UnknownContentError, QStringLiteral("Invalid compressed metadata"));
        }
        return count;
    }

private:
    void finish() {
        if (!isFinished()) {
            setFinished(true);
            Q_EMIT finished();
        }
    }

    const QByteArray m_body{QByteArrayLiteral("{}")};
    qint64 m_offset{0};
    bool m_fail_on_read{false};
};

class TransportFailureManager final : public QNetworkAccessManager {
public:
    TransportFailureManager(QNetworkReply::NetworkError initial_error, bool fail_on_read)
        : m_initial_error(initial_error), m_fail_on_read(fail_on_read) {}

protected:
    QNetworkReply* createRequest(Operation, const QNetworkRequest& request, QIODevice*) override {
        return new TransportFailureReply(request, m_initial_error, m_fail_on_read, this);
    }

private:
    QNetworkReply::NetworkError m_initial_error{QNetworkReply::NoError};
    bool m_fail_on_read{false};
};

QString readRepoSource(const QString& relative_path) {
    const QDir repo_root(
        QDir(QCoreApplication::applicationDirPath()).absoluteFilePath(QStringLiteral("../..")));
    QFile file(repo_root.filePath(relative_path));
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }
    return QString::fromUtf8(file.readAll());
}

sak::NetworkMetadataRequest validMetadataRequest() {
    return {.url = QUrl(QStringLiteral("https://metadata.example.test/value.json")),
            .timeout_ms = kTestMetadataTimeoutMs,
            .max_response_bytes = kTestMetadataResponseCap};
}

sak::NetworkTransferResult consumeMetadataReply(QNetworkReply* reply) {
    if (!reply->isFinished()) {
        QSignalSpy finished(reply, &QNetworkReply::finished);
        if (!finished.wait(kTestMetadataTimeoutMs)) {
            QTest::qFail("Metadata reply did not finish within the test deadline",
                         __FILE__,
                         __LINE__);
            reply->abort();
            return {};
        }
    }
    return sak::takeNetworkMetadataResult(reply);
}

sak::NetworkTransferResult runInjectedMetadata(
    const sak::NetworkMetadataRequest& request,
    const sak::NetworkTransferResult& injected,
    const std::function<void(QNetworkReply*)>& before_finish = {}) {
    sak::ScopedNetworkMetadataFaultInjector injector(
        [injected](const auto&) { return std::optional{injected}; });
    QNetworkAccessManager manager;
    QNetworkReply* reply = sak::startNetworkMetadataGet(manager, request);
    if (before_finish) {
        before_finish(reply);
    }
    return consumeMetadataReply(reply);
}

}  // namespace

class TestUupDumpApi : public QObject {
    Q_OBJECT

private Q_SLOTS:
    // -- Construction ----------------------------------------------
    void construction_default();
    void construction_nonCopyable();
    void metadataFetch_offlineFailsClosed();
    void metadataFetch_malformedFailsClosed();
    void metadataFetch_positiveControl();
    void metadataFetch_isCentralizedAndBounded();
    void metadataSeam_rejectsInvalidSchemeTimeoutAndCap();
    void metadataSeam_acceptsOnly2xxStatus();
    void metadataSeam_rejectsOversizedFinalBody();
    void metadataSeam_abortsOversizedLiveProgress();
    void metadataSeam_classifiesCancellationSeparately();
    void metadataSeam_classifiesNativeTimeout();
    void metadataSeam_rejectsReadFailureAfterFinished();

    // -- channelToRing ---------------------------------------------
    void channelToRing_retail();
    void channelToRing_releasePreview();
    void channelToRing_beta();
    void channelToRing_dev();
    void channelToRing_canary();
    void channelToRing_allNonEmpty();

    // -- channelToDisplayName --------------------------------------
    void channelToDisplayName_retail();
    void channelToDisplayName_dev();
    void channelToDisplayName_allNonEmpty();

    // -- allChannels -----------------------------------------------
    void allChannels_nonEmpty();
    void allChannels_containsRetail();
    void allChannels_noDuplicates();

    // -- BuildInfo defaults ----------------------------------------
    void buildInfo_defaults();

    // -- FileInfo defaults -----------------------------------------
    void fileInfo_defaults();

    // -- SHA-1 validation (B10-20) ---------------------------------
    void isValidSha1_acceptsWellFormed();
    void isValidSha1_rejectsMalformed();

    // -- isSafeAria2FileEntry (aria2 manifest injection guard) -----
    // fileName/url/sha1 are serialized into an attacker-influenced aria2 input
    // manifest; each slot pins one rejection predicate (or a scan boundary) with
    // a positive (accept) and a negative (reject) case.
    void isSafeAria2FileEntry_acceptsNormalEntry();
    void isSafeAria2FileEntry_rejectsPathTraversal();
    void isSafeAria2FileEntry_rejectsPathSeparators();
    void isSafeAria2FileEntry_rejectsDriveOrAds();
    void isSafeAria2FileEntry_rejectsTrailingDotOrSpace();
    void isSafeAria2FileEntry_rejectsReservedDeviceName();
    void isSafeAria2FileEntry_rejectsControlCharInFileName();
    void isSafeAria2FileEntry_rejectsControlCharInUrl();
    void isSafeAria2FileEntry_rejectsControlCharInSha1();
    void isSafeAria2FileEntry_controlCharBoundary();
};

// ===================================================================
// Construction
// ===================================================================

void TestUupDumpApi::construction_default() {
    UupDumpApi api;
    QVERIFY(api.parent() == nullptr);
    // parent()==nullptr on a default construction is true of ANY QObject-derived type, and
    // the is_copy_constructible_v line that stood here only repeated construction_nonCopyable.
    // What is load-bearing is the ownership graph: uup_dump_api.cpp:41 parents the
    // QNetworkAccessManager to `this` and that parent edge is its ONLY owner (~UupDumpApi,
    // :45-47, just calls cancelAll()), so `new QNetworkAccessManager()` would leak one
    // manager -- sockets and thread included -- per UupDumpApi destroyed.
    QVERIFY(api.findChild<QNetworkAccessManager*>() != nullptr);
    // ...and the parent overload must actually forward (uup_dump_api.cpp:41 `: QObject(parent)`),
    // the ownership contract the explicit ctor (uup_dump_api.h:76) offers its callers.
    QObject owner;
    UupDumpApi child(&owner);
    QCOMPARE(child.parent(), &owner);
    QVERIFY(owner.children().contains(&child));
    QVERIFY(!std::is_copy_constructible_v<UupDumpApi>);
}

void TestUupDumpApi::construction_nonCopyable() {
    QVERIFY(!std::is_copy_constructible_v<UupDumpApi>);
    QVERIFY(!std::is_move_constructible_v<UupDumpApi>);
}

void TestUupDumpApi::metadataFetch_offlineFailsClosed() {
    sak::ScopedNetworkMetadataFaultInjector inject([](const auto&) {
        sak::NetworkTransferResult result;
        result.error_message = QStringLiteral("injected offline");
        return std::optional{result};
    });
    UupDumpApi api;
    QSignalSpy error_spy(&api, &UupDumpApi::apiError);
    bool succeeded = false;
    connect(&api, &UupDumpApi::buildsFetched, &api, [&](const auto&) { succeeded = true; });

    api.fetchAvailableBuilds();

    QCOMPARE(error_spy.count(), 0);
    QTRY_COMPARE(error_spy.count(), 1);
    QVERIFY(!succeeded);
    QVERIFY(error_spy.at(0).at(0).toString().contains(QStringLiteral("injected offline")));
}

void TestUupDumpApi::metadataFetch_malformedFailsClosed() {
    sak::ScopedNetworkMetadataFaultInjector inject([](const auto&) {
        sak::NetworkTransferResult result;
        result.success = true;
        result.http_status = 200;
        result.body = QByteArrayLiteral("{not-json");
        return std::optional{result};
    });
    UupDumpApi api;
    QSignalSpy error_spy(&api, &UupDumpApi::apiError);
    bool succeeded = false;
    connect(&api, &UupDumpApi::buildsFetched, &api, [&](const auto&) { succeeded = true; });

    api.fetchAvailableBuilds();

    QTRY_COMPARE(error_spy.count(), 1);
    QVERIFY(!succeeded);
    QVERIFY(error_spy.at(0).at(0).toString().contains(QStringLiteral("JSON parse error")));
}

void TestUupDumpApi::metadataFetch_positiveControl() {
    sak::ScopedNetworkMetadataFaultInjector inject([](const auto&) {
        sak::NetworkTransferResult result;
        result.success = true;
        result.http_status = 200;
        result.body = QByteArrayLiteral(
            R"({"response":{"builds":{"uuid-1":{"uuid":"uuid-1","title":"Windows 11","build":"26100","arch":"amd64","created":1}}}})");
        return std::optional{result};
    });
    UupDumpApi api;
    QSignalSpy error_spy(&api, &UupDumpApi::apiError);
    bool succeeded = false;
    connect(&api, &UupDumpApi::buildsFetched, &api, [&](const auto&) { succeeded = true; });

    api.fetchAvailableBuilds();

    QTRY_VERIFY(succeeded);
    QCOMPARE(error_spy.count(), 0);
}

void TestUupDumpApi::metadataFetch_isCentralizedAndBounded() {
    const QString iso = readRepoSource(QStringLiteral("src/core/linux_iso_downloader.cpp"));
    const QString catalog = readRepoSource(QStringLiteral("src/core/linux_distro_catalog.cpp"));
    const QString uup = readRepoSource(QStringLiteral("src/core/uup_dump_api.cpp"));
    const QString runner = readRepoSource(QStringLiteral("src/core/network_transfer_runner.cpp"));
    QVERIFY2(!iso.isEmpty() && !catalog.isEmpty() && !uup.isEmpty() && !runner.isEmpty(),
             "metadata fetch sources could not be read");
    QCOMPARE(iso.count(QStringLiteral("startNetworkMetadataGet(")), 2);
    QCOMPARE(catalog.count(QStringLiteral("startNetworkMetadataGet(")), 1);
    QCOMPARE(uup.count(QStringLiteral("startNetworkMetadataGet(")), 1);
    QVERIFY(!iso.contains(QStringLiteral("nam->get(")));
    QVERIFY(!catalog.contains(QStringLiteral("m_networkManager->get(")));
    QVERIFY(!uup.contains(QStringLiteral("m_networkManager->get(")));
    QVERIFY(runner.contains(QStringLiteral("deadline->start(request.timeout_ms)")));
    QVERIFY(runner.contains(QStringLiteral("kMetadataOversizeProperty")));
    QVERIFY(runner.contains(QStringLiteral("!isSuccessfulHttpStatus(result->http_status)")));
    QVERIFY(runner.contains(QStringLiteral("result->timed_out")));
    QVERIFY(runner.contains(QStringLiteral("reply->error() != QNetworkReply::NoError")));
    QVERIFY(runner.contains(QStringLiteral("g_network_metadata_fault_injector")));
    QVERIFY(runner.contains(QStringLiteral("thread_local NetworkMetadataFaultInjector")));
    QVERIFY(runner.contains(QStringLiteral("QTimer::singleShot(0")));
    QVERIFY(!runner.contains(QStringLiteral("fallback_manager")));
}

void TestUupDumpApi::metadataSeam_rejectsInvalidSchemeTimeoutAndCap() {
    QNetworkAccessManager manager;
    sak::ScopedNetworkMetadataFaultInjector injector([](const auto&) {
        sak::NetworkTransferResult result;
        result.success = true;
        result.http_status = kOkHttpStatus;
        result.body = QByteArrayLiteral("ok");
        return std::optional{result};
    });
    auto request = validMetadataRequest();
    request.url = QUrl(QStringLiteral("http://metadata.example.test/value.json"));
    auto result = consumeMetadataReply(sak::startNetworkMetadataGet(manager, request));
    QVERIFY(!result.success);
    QVERIFY(result.error_message.contains(QStringLiteral("HTTPS")));

    request = validMetadataRequest();
    request.timeout_ms = 0;
    result = consumeMetadataReply(sak::startNetworkMetadataGet(manager, request));
    QVERIFY(!result.success);
    QVERIFY(result.error_message.contains(QStringLiteral("timeout")));

    request = validMetadataRequest();
    request.max_response_bytes = 0;
    result = consumeMetadataReply(sak::startNetworkMetadataGet(manager, request));
    QVERIFY(!result.success);
    QVERIFY(result.error_message.contains(QStringLiteral("positive")));
}

void TestUupDumpApi::metadataSeam_acceptsOnly2xxStatus() {
    sak::NetworkTransferResult injected;
    injected.success = true;
    injected.http_status = kLastSuccessfulHttpStatus;
    injected.body = QByteArrayLiteral("ok");
    QVERIFY(runInjectedMetadata(validMetadataRequest(), injected).success);

    injected.http_status = kRedirectHttpStatus;
    const auto redirect = runInjectedMetadata(validMetadataRequest(), injected);
    QVERIFY(!redirect.success);
    QCOMPARE(redirect.http_status, kRedirectHttpStatus);
    QVERIFY(redirect.error_message.contains(QStringLiteral("HTTP 300")));

    injected.http_status = -1;
    const auto invalid = runInjectedMetadata(validMetadataRequest(), injected);
    QVERIFY(!invalid.success);
    QCOMPARE(invalid.http_status, -1);
    QVERIFY(invalid.body.isEmpty());
    QVERIFY(invalid.error_message.contains(QStringLiteral("HTTP -1")));
}

void TestUupDumpApi::metadataSeam_rejectsOversizedFinalBody() {
    auto request = validMetadataRequest();
    request.max_response_bytes = kTestSmallMetadataCap;
    sak::NetworkTransferResult injected;
    injected.success = true;
    injected.http_status = kOkHttpStatus;
    injected.body = QByteArrayLiteral("12345");
    const auto result = runInjectedMetadata(request, injected);
    QVERIFY(!result.success);
    QVERIFY(result.body.isEmpty());
    QCOMPARE(result.bytes_received, 0);
    QVERIFY(result.error_message.contains(QStringLiteral("size limit")));
}

void TestUupDumpApi::metadataSeam_abortsOversizedLiveProgress() {
    auto request = validMetadataRequest();
    request.max_response_bytes = kTestSmallMetadataCap;
    sak::NetworkTransferResult injected;
    injected.success = true;
    injected.http_status = kOkHttpStatus;
    injected.body = QByteArrayLiteral("1234");
    const auto result = runInjectedMetadata(request, injected, [](QNetworkReply* reply) {
        QVERIFY(QMetaObject::invokeMethod(reply,
                                          "downloadProgress",
                                          Qt::DirectConnection,
                                          Q_ARG(qint64, kTestOversizedProgress),
                                          Q_ARG(qint64, kTestOversizedProgress)));
    });
    QVERIFY(!result.success);
    QVERIFY(!result.cancelled);
    QVERIFY(result.error_message.contains(QStringLiteral("size limit")));
}

void TestUupDumpApi::metadataSeam_classifiesCancellationSeparately() {
    sak::NetworkTransferResult injected;
    injected.cancelled = true;
    const auto cancelled = runInjectedMetadata(validMetadataRequest(), injected);
    QVERIFY(!cancelled.success);
    QVERIFY(cancelled.cancelled);
    QVERIFY(!cancelled.timed_out);

    injected = {};
    injected.timed_out = true;
    const auto timed_out = runInjectedMetadata(validMetadataRequest(), injected);
    QVERIFY(!timed_out.success);
    QVERIFY(timed_out.timed_out);
    QVERIFY(!timed_out.cancelled);
}

void TestUupDumpApi::metadataSeam_classifiesNativeTimeout() {
    TransportFailureManager manager(QNetworkReply::TimeoutError, false);
    const auto result =
        consumeMetadataReply(sak::startNetworkMetadataGet(manager, validMetadataRequest()));
    QVERIFY(!result.success);
    QVERIFY(result.timed_out);
    QVERIFY(!result.cancelled);
    QVERIFY(result.body.isEmpty());
    QCOMPARE(result.error_message, QStringLiteral("Network metadata request timed out"));
}

void TestUupDumpApi::metadataSeam_rejectsReadFailureAfterFinished() {
    TransportFailureManager manager(QNetworkReply::NoError, true);
    const auto result =
        consumeMetadataReply(sak::startNetworkMetadataGet(manager, validMetadataRequest()));
    QVERIFY(!result.success);
    QVERIFY(!result.timed_out);
    QVERIFY(!result.cancelled);
    QVERIFY(result.body.isEmpty());
    QCOMPARE(result.bytes_received, 0);
    QCOMPARE(result.error_message, QStringLiteral("Invalid compressed metadata"));
}

// ===================================================================
// channelToRing
// ===================================================================

void TestUupDumpApi::channelToRing_retail() {
    const auto ring = UupDumpApi::channelToRing(UupDumpApi::ReleaseChannel::Retail);
    // The ring string is the load-bearing API parameter; `!isEmpty()` would pass a wrong mapping.
    QCOMPARE(ring, QStringLiteral("Retail"));
}

void TestUupDumpApi::channelToRing_releasePreview() {
    const auto ring = UupDumpApi::channelToRing(UupDumpApi::ReleaseChannel::ReleasePreview);
    QCOMPARE(ring, QStringLiteral("ReleasePreview"));
}

void TestUupDumpApi::channelToRing_beta() {
    const auto ring = UupDumpApi::channelToRing(UupDumpApi::ReleaseChannel::Beta);
    QCOMPARE(ring, QStringLiteral("Beta"));
}

void TestUupDumpApi::channelToRing_dev() {
    const auto ring = UupDumpApi::channelToRing(UupDumpApi::ReleaseChannel::Dev);
    QCOMPARE(ring, QStringLiteral("Dev"));
}

void TestUupDumpApi::channelToRing_canary() {
    const auto ring = UupDumpApi::channelToRing(UupDumpApi::ReleaseChannel::Canary);
    QCOMPARE(ring, QStringLiteral("Canary"));
}

void TestUupDumpApi::channelToRing_allNonEmpty() {
    using RC = UupDumpApi::ReleaseChannel;
    // Every channel maps to its exact ring string; `!isEmpty()` in the old loop caught no wrong
    // mapping.
    QCOMPARE(UupDumpApi::channelToRing(RC::Retail), QStringLiteral("Retail"));
    QCOMPARE(UupDumpApi::channelToRing(RC::ReleasePreview), QStringLiteral("ReleasePreview"));
    QCOMPARE(UupDumpApi::channelToRing(RC::Beta), QStringLiteral("Beta"));
    QCOMPARE(UupDumpApi::channelToRing(RC::Dev), QStringLiteral("Dev"));
    QCOMPARE(UupDumpApi::channelToRing(RC::Canary), QStringLiteral("Canary"));
}

// ===================================================================
// channelToDisplayName
// ===================================================================

void TestUupDumpApi::channelToDisplayName_retail() {
    const auto name = UupDumpApi::channelToDisplayName(UupDumpApi::ReleaseChannel::Retail);
    // Exact: the three-way substring OR passed for any of "Retail"/"Stable"/"Release".
    QCOMPARE(name, QStringLiteral("Public Release"));
}

void TestUupDumpApi::channelToDisplayName_dev() {
    const auto name = UupDumpApi::channelToDisplayName(UupDumpApi::ReleaseChannel::Dev);
    QCOMPARE(name, QStringLiteral("Dev Channel"));
}

void TestUupDumpApi::channelToDisplayName_allNonEmpty() {
    using RC = UupDumpApi::ReleaseChannel;
    // Every channel maps to its exact display name.
    QCOMPARE(UupDumpApi::channelToDisplayName(RC::Retail), QStringLiteral("Public Release"));
    QCOMPARE(UupDumpApi::channelToDisplayName(RC::ReleasePreview),
             QStringLiteral("Release Preview"));
    QCOMPARE(UupDumpApi::channelToDisplayName(RC::Beta), QStringLiteral("Beta Channel"));
    QCOMPARE(UupDumpApi::channelToDisplayName(RC::Dev), QStringLiteral("Dev Channel"));
    QCOMPARE(UupDumpApi::channelToDisplayName(RC::Canary), QStringLiteral("Canary Channel"));
}

// ===================================================================
// allChannels
// ===================================================================

void TestUupDumpApi::allChannels_nonEmpty() {
    const auto channels = UupDumpApi::allChannels();
    // Exactly the five Windows Update rings.
    QCOMPARE(channels.size(), qsizetype(5));
}

void TestUupDumpApi::allChannels_containsRetail() {
    const auto channels = UupDumpApi::allChannels();
    QVERIFY(channels.contains(UupDumpApi::ReleaseChannel::Retail));
}

void TestUupDumpApi::allChannels_noDuplicates() {
    const auto channels = UupDumpApi::allChannels();
    QSet<int> unique;
    for (const auto channel : channels) {
        unique.insert(static_cast<int>(channel));
    }
    QCOMPARE(unique.size(), channels.size());
}

// ===================================================================
// BuildInfo / FileInfo defaults
// ===================================================================

void TestUupDumpApi::buildInfo_defaults() {
    UupDumpApi::BuildInfo info;
    QVERIFY(info.uuid.isEmpty());
    QVERIFY(info.title.isEmpty());
    QVERIFY(info.build.isEmpty());
    QVERIFY(info.arch.isEmpty());
    QCOMPARE(info.created, static_cast<qint64>(0));
}

void TestUupDumpApi::fileInfo_defaults() {
    UupDumpApi::FileInfo info;
    QVERIFY(info.fileName.isEmpty());
    QVERIFY(info.sha1.isEmpty());
    QCOMPARE(info.size, static_cast<qint64>(0));
    QVERIFY(info.url.isEmpty());
}

// ===================================================================
// SHA-1 validation (B10-20)
// ===================================================================

void TestUupDumpApi::isValidSha1_acceptsWellFormed() {
    // 40 hex chars, lower/upper/mixed case.
    QVERIFY(UupDumpApi::isValidSha1(QStringLiteral("da39a3ee5e6b4b0d3255bfef95601890afd80709")));
    QVERIFY(UupDumpApi::isValidSha1(QStringLiteral("DA39A3EE5E6B4B0D3255BFEF95601890AFD80709")));
    QVERIFY(UupDumpApi::isValidSha1(QStringLiteral("Da39A3ee5e6B4b0d3255bFEF95601890afd80709")));
}

void TestUupDumpApi::isValidSha1_rejectsMalformed() {
    QVERIFY(!UupDumpApi::isValidSha1(QString()));                        // empty
    QVERIFY(!UupDumpApi::isValidSha1(QStringLiteral("da39a3ee")));       // too short
    QVERIFY(!UupDumpApi::isValidSha1(
        QStringLiteral("da39a3ee5e6b4b0d3255bfef95601890afd80709aa")));  // too long
    QVERIFY(!UupDumpApi::isValidSha1(
        QStringLiteral("da39a3ee5e6b4b0d3255bfef95601890afd8070g")));    // non-hex 'g'
    // The characters immediately OUTSIDE each accepted range: '/' one below '0', ':' one above
    // '9', '@' one below 'A', 'G' one above 'F', '`' one below 'a'. 'g' alone probes a single
    // endpoint; widening any range by one (e.g. `ch <= ':'`) ships green and lets a non-hex
    // "digest" through the integrity gate that is the sole justification for the plain-HTTP CDN
    // allowance.
    const QString base39 = QStringLiteral("da39a3ee5e6b4b0d3255bfef95601890afd8070");
    QCOMPARE(base39.size(), qsizetype(39));
    for (const QChar bad : {QChar(u'/'), QChar(u':'), QChar(u'@'), QChar(u'G'), QChar(u'`')}) {
        const QString candidate = base39 + bad;
        QCOMPARE(candidate.size(), qsizetype(40));
        QVERIFY2(!UupDumpApi::isValidSha1(candidate), qPrintable(candidate));
    }
    QVERIFY(!UupDumpApi::isValidSha1(
        QStringLiteral("da39a3ee5e6b4b0d3255 fef95601890afd80709")));  // embedded space
}

// ===================================================================
// isSafeAria2FileEntry (aria2 manifest injection guard)
// ===================================================================

namespace {

// A fully safe file entry: every field is accepted by the guard. Individual
// slots mutate one field to a boundary/attack value and re-check.
UupDumpApi::FileInfo safeBaselineEntry() {
    UupDumpApi::FileInfo info;
    info.fileName = QStringLiteral("core_en-us.esd");
    info.url = QStringLiteral("https://amd64.uup.microsoft.com/core_en-us.esd");
    info.sha1 = QStringLiteral("da39a3ee5e6b4b0d3255bfef95601890afd80709");
    info.size = 12'345;
    return info;
}

}  // namespace

void TestUupDumpApi::isSafeAria2FileEntry_acceptsNormalEntry() {
    // Ordinary CDN file names with underscores, hyphens, and dots are accepted.
    QVERIFY(UupDumpApi::isSafeAria2FileEntry(safeBaselineEntry()));

    auto e = safeBaselineEntry();
    e.fileName = QStringLiteral("professional_x64-26100.1.esd");
    QVERIFY(UupDumpApi::isSafeAria2FileEntry(e));

    e.fileName = QStringLiteral("UUPDump-metadata.txt");
    QVERIFY(UupDumpApi::isSafeAria2FileEntry(e));
}

void TestUupDumpApi::isSafeAria2FileEntry_rejectsPathTraversal() {
    auto e = safeBaselineEntry();
    // ".." anywhere is traversal -- even with no separator. Pins the ".." predicate
    // in isolation so dropping it cannot be masked by the '/' or '\\' checks.
    e.fileName = QStringLiteral("foo..esd");
    QVERIFY(!UupDumpApi::isSafeAria2FileEntry(e));
    e.fileName = QStringLiteral("../evil.esd");
    QVERIFY(!UupDumpApi::isSafeAria2FileEntry(e));
    e.fileName = QStringLiteral("..\\evil.esd");
    QVERIFY(!UupDumpApi::isSafeAria2FileEntry(e));
}

void TestUupDumpApi::isSafeAria2FileEntry_rejectsPathSeparators() {
    auto e = safeBaselineEntry();
    // '/' only (no ".."), isolates the forward-slash predicate.
    e.fileName = QStringLiteral("sub/core.esd");
    QVERIFY(!UupDumpApi::isSafeAria2FileEntry(e));
    // '\\' only, isolates the backslash predicate.
    e.fileName = QStringLiteral("sub\\core.esd");
    QVERIFY(!UupDumpApi::isSafeAria2FileEntry(e));
    // Absolute POSIX and UNC forms remain rejected via the same separators.
    e.fileName = QStringLiteral("/etc/passwd");
    QVERIFY(!UupDumpApi::isSafeAria2FileEntry(e));
    e.fileName = QStringLiteral("\\\\server\\share");
    QVERIFY(!UupDumpApi::isSafeAria2FileEntry(e));
}

void TestUupDumpApi::isSafeAria2FileEntry_rejectsDriveOrAds() {
    auto e = safeBaselineEntry();
    // ':' only (no separator) -> NTFS alternate data stream. Isolates the ':' predicate.
    e.fileName = QStringLiteral("core.esd:zone");
    QVERIFY(!UupDumpApi::isSafeAria2FileEntry(e));
    // Drive-absolute form.
    e.fileName = QStringLiteral("C:core.esd");
    QVERIFY(!UupDumpApi::isSafeAria2FileEntry(e));
}

void TestUupDumpApi::isSafeAria2FileEntry_rejectsTrailingDotOrSpace() {
    auto e = safeBaselineEntry();
    // Windows trims a trailing dot -> on-disk name differs from the integrity-checked
    // name. Isolates the trailing-dot predicate (no "..", no separator, base not reserved).
    e.fileName = QStringLiteral("core.esd.");
    QVERIFY(!UupDumpApi::isSafeAria2FileEntry(e));
    // Windows trims a trailing space likewise. Isolates the trailing-space predicate.
    e.fileName = QStringLiteral("core.esd ");
    QVERIFY(!UupDumpApi::isSafeAria2FileEntry(e));
}

void TestUupDumpApi::isSafeAria2FileEntry_rejectsReservedDeviceName() {
    auto e = safeBaselineEntry();
    // A reserved DOS device name, with or without extension, any case: aria2 out=con
    // would open the console device instead of creating a file.
    e.fileName = QStringLiteral("CON.esd");
    QVERIFY(!UupDumpApi::isSafeAria2FileEntry(e));
    e.fileName = QStringLiteral("nul");
    QVERIFY(!UupDumpApi::isSafeAria2FileEntry(e));
    e.fileName = QStringLiteral("COM1.txt");
    QVERIFY(!UupDumpApi::isSafeAria2FileEntry(e));
    // The WHOLE catalog (uup_dump_api.cpp:531-539), bare and with an extension: sampling
    // three members left the other nineteen free to fall out of the list -- only CON, NUL,
    // COM1 and LPT9 are named anywhere in the suite, so dropping PRN/AUX (the two names a
    // "generate COM1..9/LPT1..9 in a loop" rewrite forgets) would ship green and let
    // aria2 out=PRN.esd open the printer device.
    const QStringList reserved = QStringLiteral(
                                     "CON PRN AUX NUL COM1 COM2 COM3 COM4 COM5 COM6 COM7 COM8 COM9 "
                                     "LPT1 LPT2 LPT3 LPT4 LPT5 LPT6 LPT7 LPT8 LPT9")
                                     .split(QLatin1Char(' '));
    QCOMPARE(reserved.size(), qsizetype(22));
    for (const QString& name : reserved) {
        e.fileName = name;
        QVERIFY2(!UupDumpApi::isSafeAria2FileEntry(e), qPrintable(e.fileName));
        e.fileName = name.toLower() + QStringLiteral(".esd");
        QVERIFY2(!UupDumpApi::isSafeAria2FileEntry(e), qPrintable(e.fileName));
    }
    // ...and ONLY the exact pre-dot base is reserved (uup_dump_api.cpp:529-530,540): the
    // guard must not turn into a prefix match and start refusing ordinary CDN names, and
    // COM0/LPT0 are not devices.
    e.fileName = QStringLiteral("COM0.esd");
    QVERIFY(UupDumpApi::isSafeAria2FileEntry(e));
    e.fileName = QStringLiteral("LPT0.esd");
    QVERIFY(UupDumpApi::isSafeAria2FileEntry(e));
    e.fileName = QStringLiteral("console_en-us.esd");
    QVERIFY(UupDumpApi::isSafeAria2FileEntry(e));
    e.fileName = QStringLiteral("auxiliary.esd");
    QVERIFY(UupDumpApi::isSafeAria2FileEntry(e));
}

void TestUupDumpApi::isSafeAria2FileEntry_rejectsControlCharInFileName() {
    auto e = safeBaselineEntry();
    // A C0 control char in the name could truncate the aria2 line or break the record.
    e.fileName = QStringLiteral("core") + QChar(u'\n') + QStringLiteral(".esd");        // LF
    QVERIFY(!UupDumpApi::isSafeAria2FileEntry(e));
    e.fileName = QStringLiteral("core") + QChar(u'\r') + QStringLiteral(".esd");        // CR
    QVERIFY(!UupDumpApi::isSafeAria2FileEntry(e));
    e.fileName = QStringLiteral("core") + QChar(u'\t') + QStringLiteral(".esd");        // TAB
    QVERIFY(!UupDumpApi::isSafeAria2FileEntry(e));
    e.fileName = QStringLiteral("core") + QChar(char16_t(0)) + QStringLiteral(".esd");  // NUL
    QVERIFY(!UupDumpApi::isSafeAria2FileEntry(e));
}

void TestUupDumpApi::isSafeAria2FileEntry_rejectsControlCharInUrl() {
    auto e = safeBaselineEntry();
    // CRLF in the URL could break the aria2 line and inject an out=/uri directive.
    e.url = QStringLiteral("https://amd64.uup.microsoft.com/x") + QChar(u'\r') + QChar(u'\n') +
            QStringLiteral(" out=/etc/passwd");
    QVERIFY(!UupDumpApi::isSafeAria2FileEntry(e));
}

void TestUupDumpApi::isSafeAria2FileEntry_rejectsControlCharInSha1() {
    auto e = safeBaselineEntry();
    // A control char in the checksum field is likewise serialized -> must be rejected.
    e.sha1 = QStringLiteral("da39a3ee5e6b4b0d3255bfef95601890afd8") + QChar(u'\n') +
             QStringLiteral("0709");
    QVERIFY(!UupDumpApi::isSafeAria2FileEntry(e));
}

void TestUupDumpApi::isSafeAria2FileEntry_controlCharBoundary() {
    auto e = safeBaselineEntry();
    // 0x20 (space) is the first printable code unit -> an INTERNAL space is accepted.
    // Pins the '< 0x20' lower boundary: relaxing '<' to '<=' would reject this.
    e.fileName = QStringLiteral("core update.esd");
    QVERIFY(UupDumpApi::isSafeAria2FileEntry(e));
    // 0x7e ('~') sits just below DEL and is printable -> accepted.
    e.fileName = QStringLiteral("core~1.esd");
    QVERIFY(UupDumpApi::isSafeAria2FileEntry(e));
    // 0x1f (unit separator, < 0x20) -> rejected.
    e.fileName = QStringLiteral("core") + QChar(char16_t(0x1f)) + QStringLiteral(".esd");
    QVERIFY(!UupDumpApi::isSafeAria2FileEntry(e));
    // 0x7f (DEL) -> rejected. Pins the '== 0x7f' predicate specifically (the '< 0x20'
    // test does not cover it).
    e.fileName = QStringLiteral("core") + QChar(char16_t(0x7f)) + QStringLiteral(".esd");
    QVERIFY(!UupDumpApi::isSafeAria2FileEntry(e));
}

QTEST_MAIN(TestUupDumpApi)
#include "test_uup_dump_api.moc"
