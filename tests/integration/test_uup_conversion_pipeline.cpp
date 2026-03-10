// Copyright (c) 2025-2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/**
 * @file test_uup_conversion_pipeline.cpp
 * @brief Integration tests for UUP-to-ISO conversion pipeline
 *
 * Tests that the bundled UUPMediaConverter.exe is present, launches
 * without forking from the QProcess handle, shows help output, and
 * exits cleanly when given a non-existent UUP directory.
 */

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QProcess>
#include <QTemporaryDir>
#include <QtTest/QtTest>

#include <algorithm>

class UupConversionPipelineTest : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void initTestCase();
    void cleanupTestCase();

    void testConverterExecutableExists();
    void testConverterLaunchesWithoutForking();
    void testConverterShowsHelpOutput();
    void testConverterExitsOnMissingUupDir();
    void testConverterExitsOnEmptyUupDir();

private:
    QString m_converterPath;
    bool m_converterAvailable = false;

    /// Locate UUPMediaConverter.exe relative to the test executable
    QString findConverter() const;
};

QString UupConversionPipelineTest::findConverter() const {
    const QString appDir = QCoreApplication::applicationDirPath();

    // Candidate paths relative to the test executable location
    const QStringList candidates = {
        appDir + "/tools/uup/uupmc/UUPMediaConverter.exe",
        appDir + "/../../tools/uup/uupmc/UUPMediaConverter.exe",
        appDir + "/../../../tools/uup/uupmc/UUPMediaConverter.exe",
    };

    auto found = std::find_if(candidates.begin(), candidates.end(), [](const QString& path) {
        return QFileInfo::exists(path);
    });
    if (found != candidates.end()) {
        return QDir(*found).absolutePath();
    }
    return {};
}

void UupConversionPipelineTest::initTestCase() {
    qInfo() << "=== UUP Conversion Pipeline Integration Tests ===";

    m_converterPath = findConverter();
    m_converterAvailable = !m_converterPath.isEmpty();

    if (m_converterAvailable) {
        qInfo() << "UUPMediaConverter found at:" << m_converterPath;
    } else {
        qWarning() << "UUPMediaConverter.exe not found - some tests will be skipped";
    }
}

void UupConversionPipelineTest::cleanupTestCase() {
    qInfo() << "=== Conversion pipeline tests completed ===";
}

/// Verify the bundled UUPMediaConverter.exe is present.
void UupConversionPipelineTest::testConverterExecutableExists() {
    QVERIFY2(m_converterAvailable, "UUPMediaConverter.exe must be bundled in tools/uup/uupmc/");
    QVERIFY(QFileInfo(m_converterPath).isExecutable());
}

/**
 * CRITICAL TEST: Verify converter stays attached to QProcess.
 *
 * UUPMediaConverter.exe is a self-contained .NET 8 executable.
 * This test ensures it does not fork or detach; QProcess should
 * be able to track the process to completion.
 */
void UupConversionPipelineTest::testConverterLaunchesWithoutForking() {
    if (!m_converterAvailable) {
        QSKIP("UUPMediaConverter.exe not found");
    }

    QProcess process;
    process.setWorkingDirectory(QFileInfo(m_converterPath).absolutePath());
    process.setProcessChannelMode(QProcess::MergedChannels);

    // Invoke with --help; process should print usage and exit
    QStringList args;
    args << "--help";

    QElapsedTimer timer;
    timer.start();

    process.start(m_converterPath, args);
    QVERIFY2(process.waitForStarted(10'000), "Failed to start UUPMediaConverter");

    bool finished = process.waitForFinished(30'000);
    const qint64 elapsed = timer.elapsed();
    const QString output = QString::fromUtf8(process.readAllStandardOutput());

    qInfo() << "Converter ran for" << elapsed << "ms";
    qInfo() << "Exit code:" << process.exitCode();
    qInfo() << "Output length:" << output.length() << "chars";

    QVERIFY2(finished, "UUPMediaConverter timed out (may have forked)");

    // The process must exit quickly for a --help invocation
    QVERIFY2(elapsed < 15'000,
             qPrintable(QString("--help took %1 ms; expected < 15s").arg(elapsed)));
}

/// Verify --help produces recognizable usage text.
void UupConversionPipelineTest::testConverterShowsHelpOutput() {
    if (!m_converterAvailable) {
        QSKIP("UUPMediaConverter.exe not found");
    }

    QProcess process;
    process.setWorkingDirectory(QFileInfo(m_converterPath).absolutePath());
    process.setProcessChannelMode(QProcess::MergedChannels);

    process.start(m_converterPath, {"--help"});
    QVERIFY(process.waitForStarted(10'000));
    QVERIFY(process.waitForFinished(30'000));

    const QString output = QString::fromUtf8(process.readAllStandardOutput());
    qInfo() << "Help output:" << output.left(500);

    // Should mention desktop-convert or usage or help keywords
    const bool has_usage = output.contains("desktop-convert", Qt::CaseInsensitive) ||
                           output.contains("usage", Qt::CaseInsensitive) ||
                           output.contains("help", Qt::CaseInsensitive) ||
                           output.contains("convert", Qt::CaseInsensitive);

    QVERIFY2(has_usage, qPrintable("Expected help/usage output but got: " + output.left(300)));
}

/// Verify converter exits cleanly with an error when the UUP directory is missing.
void UupConversionPipelineTest::testConverterExitsOnMissingUupDir() {
    if (!m_converterAvailable) {
        QSKIP("UUPMediaConverter.exe not found");
    }

    QTemporaryDir tmpDir;
    QVERIFY(tmpDir.isValid());

    const QString bogus_uups = tmpDir.filePath("nonexistent_uups");
    const QString bogus_iso = tmpDir.filePath("output.iso");

    QProcess process;
    process.setWorkingDirectory(QFileInfo(m_converterPath).absolutePath());
    process.setProcessChannelMode(QProcess::MergedChannels);

    QStringList args;
    args << "desktop-convert"
         << "-u" << bogus_uups << "-i" << bogus_iso << "-l" << "en-us"
         << "--no-key-prompt";

    process.start(m_converterPath, args);
    QVERIFY(process.waitForStarted(10'000));

    const bool finished = process.waitForFinished(60'000);
    const QString output = QString::fromUtf8(process.readAllStandardOutput());

    qInfo() << "Exit code:" << process.exitCode();
    qInfo() << "Output:" << output.left(500);

    QVERIFY2(finished, "Converter should exit when UUP directory does not exist");
    QVERIFY2(process.exitCode() != 0,
             "Converter should return non-zero exit code for missing UUP dir");
}

/// Verify converter exits cleanly when the UUP directory is empty (no ESD files).
void UupConversionPipelineTest::testConverterExitsOnEmptyUupDir() {
    if (!m_converterAvailable) {
        QSKIP("UUPMediaConverter.exe not found");
    }

    QTemporaryDir tmpDir;
    QVERIFY(tmpDir.isValid());

    const QString empty_uups = tmpDir.filePath("UUPs");
    QDir().mkpath(empty_uups);
    QVERIFY(QFileInfo::exists(empty_uups));

    const QString bogus_iso = tmpDir.filePath("output.iso");

    QProcess process;
    process.setWorkingDirectory(QFileInfo(m_converterPath).absolutePath());
    process.setProcessChannelMode(QProcess::MergedChannels);

    QStringList args;
    args << "desktop-convert"
         << "-u" << empty_uups << "-i" << bogus_iso << "-l" << "en-us"
         << "--no-key-prompt";

    process.start(m_converterPath, args);
    QVERIFY(process.waitForStarted(10'000));

    const bool finished = process.waitForFinished(60'000);
    const QString output = QString::fromUtf8(process.readAllStandardOutput());

    qInfo() << "Exit code:" << process.exitCode();
    qInfo() << "Output:" << output.left(500);

    QVERIFY2(finished, "Converter should exit when UUP directory is empty");
}

#include <QApplication>
int main(int argc, char* argv[]) {
    QApplication app(argc, argv);
    UupConversionPipelineTest tc;
    // cppcheck-suppress unknownMacro
    QTEST_SET_MAIN_SOURCE_PATH
    return QTest::qExec(&tc, argc, argv);
}

#include "test_uup_conversion_pipeline.moc"
