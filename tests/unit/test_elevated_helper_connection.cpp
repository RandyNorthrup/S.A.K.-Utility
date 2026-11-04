// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file test_elevated_helper_connection.cpp
/// @brief The real helper, started the real way, running a real task over the real pipe.
///
/// From 2026-08-02 until a VM run in November every administrator task failed with connection
/// error 411, because each end compared the other's path with different slashes. Every test of
/// that comparison passed both paths in one spelling, and nothing connected the two ends. This
/// does.
///
/// Each end checks which program is on the other: the helper accepts only a client named
/// sak_utility.exe in its own folder. So this test copies itself there under that name, beside a
/// copy of the helper, and runs that copy. It asks for elevation, so it runs where elevation asks
/// nobody anything (a CI runner) and skips everywhere else.

#include "sak/elevation_broker.h"

#include "../support/test_hardware_guard.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QJsonObject>
#include <QProcess>
#include <QTemporaryDir>
#include <QtTest/QtTest>

#include <cstdio>
#include <cstring>

namespace {

constexpr auto kClientSwitch = "--act-as-the-application";
constexpr int kClientTimeoutMs = 180'000;
const QString kAnswer = QStringLiteral("the helper answered");

/// What the copy does, named sak_utility.exe, beside the helper.
int runAsTheApplication() {
    sak::ElevationBroker broker;
    QJsonObject payload;
    payload[QStringLiteral("command")] = QStringLiteral("Write-Output '%1'").arg(kAnswer);
    payload[QStringLiteral("timeout_seconds")] = 60;
    const auto result = broker.executeTask(QStringLiteral("RunPowerShell"),
                                           QStringLiteral("Helper connection test"),
                                           payload);
    if (!result) {
        std::printf("broker error %d\n", static_cast<int>(result.error()));
        broker.shutdown();
        return 2;
    }
    const QString output = result->data.value(QStringLiteral("stdout")).toString();
    std::printf("success=%d output=%s error=%s\n",
                result->success ? 1 : 0,
                qPrintable(output),
                qPrintable(result->error_message));
    broker.shutdown();
    return result->success && output.contains(kAnswer) ? 0 : 1;
}

}  // namespace

class ElevatedHelperConnectionTests : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void realHelperRunsATaskOverARealPipe();
};

void ElevatedHelperConnectionTests::realHelperRunsATaskOverARealPipe() {
    SAK_SKIP_UNLESS_REAL_HARDWARE_ALLOWED();
    const QDir build(QCoreApplication::applicationDirPath());
    const QString helper = build.filePath(QStringLiteral("sak_elevated_helper.exe"));
    QVERIFY2(QFile::exists(helper), qPrintable(helper));

    QTemporaryDir stage;
    QVERIFY(stage.isValid());
    const QString client = stage.filePath(QStringLiteral("sak_utility.exe"));
    QVERIFY(QFile::copy(QCoreApplication::applicationFilePath(), client));
    QVERIFY(QFile::copy(helper, stage.filePath(QStringLiteral("sak_elevated_helper.exe"))));
    // An elevated process does not get this one's PATH, so what the two need goes beside them.
    for (const auto& library : build.entryList({QStringLiteral("*.dll")}, QDir::Files)) {
        QVERIFY(QFile::copy(build.filePath(library), stage.filePath(library)));
    }

    QProcess process;
    process.setProcessChannelMode(QProcess::MergedChannels);
    process.setWorkingDirectory(stage.path());
    process.start(client, {QString::fromLatin1(kClientSwitch)});
    QVERIFY2(process.waitForStarted(), qPrintable(process.errorString()));
    const bool finished = process.waitForFinished(kClientTimeoutMs);
    const QString output = QString::fromLocal8Bit(process.readAll());
    if (!finished) {
        process.kill();
        process.waitForFinished();
    }
    QVERIFY2(finished, qPrintable(output));
    QVERIFY2(process.exitCode() == 0, qPrintable(output));
    QVERIFY2(output.contains(kAnswer), qPrintable(output));
}

int main(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], kClientSwitch) == 0) {
            const QCoreApplication app(argc, argv);
            return runAsTheApplication();
        }
    }
    QCoreApplication app(argc, argv);
    ElevatedHelperConnectionTests tests;
    return QTest::qExec(&tests, argc, argv);
}

#include "test_elevated_helper_connection.moc"
