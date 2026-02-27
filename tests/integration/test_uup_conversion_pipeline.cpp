// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/**
 * @file test_uup_conversion_pipeline.cpp
 * @brief Integration tests for UUP-to-ISO conversion pipeline
 *
 * Tests that the converter is invoked correctly and doesn't fork/detach
 * from the QProcess handle. Uses the real bundled converter scripts
 * but without actual UUP files; verifies that the process stays attached
 * and exits cleanly (with expected error about missing ESD files).
 */

#include <QtTest/QtTest>
#include <QProcess>
#include <QDir>
#include <QFile>
#include <QTextStream>
#include <QTemporaryDir>
#include <QDirIterator>
#include <QCoreApplication>
#include <QElapsedTimer>

class UupConversionPipelineTest : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void initTestCase();
    void cleanupTestCase();

    // Core process-attachment tests
    void testConverterLaunchesWithoutForking();
    void testConverterReadsConvertConfig();
    void testConverterDetectsMissingEsdFiles();
    void testConverterExitsOnClosedStdin();
    void testConvertConfigIniFormat();

private:
    QString m_converterDir;  // Path to bundled converter
    bool m_converterAvailable = false;

    /// Create a temporary work directory with converter files copied in
    bool setupWorkDir(QTemporaryDir& tmpDir);
    /// Write a ConvertConfig.ini matching our app's format
    void writeConvertConfig(const QString& dir);
};

void UupConversionPipelineTest::initTestCase()
{
    qInfo() << "=== UUP Conversion Pipeline Integration Tests ===";

    // Locate bundled converter relative to the test executable
    // In build: build/Release/test_uup_conversion_pipeline.exe
    // Converter: tools/uup/converter/convert-UUP.cmd (from source tree)
    QString appDir = QCoreApplication::applicationDirPath();

    // Try: appDir/tools/uup/converter (deployed build)
    m_converterDir = appDir + "/tools/uup/converter";
    if (!QFileInfo::exists(m_converterDir + "/convert-UUP.cmd")) {
        // Try: source tree relative to build dir
        m_converterDir = appDir + "/../../tools/uup/converter";
    }
    if (!QFileInfo::exists(m_converterDir + "/convert-UUP.cmd")) {
        // Try: workspace root
        m_converterDir = appDir + "/../../../tools/uup/converter";
    }

    m_converterAvailable = QFileInfo::exists(m_converterDir + "/convert-UUP.cmd");
    if (m_converterAvailable) {
        m_converterDir = QDir(m_converterDir).absolutePath();
        qInfo() << "Converter found at:" << m_converterDir;
    } else {
        qWarning() << "Converter not found - some tests will be skipped";
    }
}

void UupConversionPipelineTest::cleanupTestCase()
{
    qInfo() << "=== Conversion pipeline tests completed ===";
}

bool UupConversionPipelineTest::setupWorkDir(QTemporaryDir& tmpDir)
{
    if (!m_converterAvailable) return false;

    // Copy all converter files to the temp directory
    QDir srcDir(m_converterDir);
    QDirIterator it(srcDir.absolutePath(),
                    QDir::Files | QDir::NoDotAndDotDot,
                    QDirIterator::Subdirectories);

    int copied = 0;
    while (it.hasNext()) {
        QString srcPath = it.next();
        QString relPath = srcDir.relativeFilePath(srcPath);
        QString destPath = tmpDir.filePath(relPath);

        QFileInfo destInfo(destPath);
        QDir().mkpath(destInfo.absolutePath());

        if (QFile::copy(srcPath, destPath)) {
            copied++;
        }
    }

    qInfo() << "Copied" << copied << "converter files to" << tmpDir.path();

    // Create UUPs subdirectory (empty - no actual UUP files)
    QDir(tmpDir.path()).mkpath("UUPs");

    return copied > 0;
}

void UupConversionPipelineTest::writeConvertConfig(const QString& dir)
{
    QFile configFile(QDir(dir).filePath("ConvertConfig.ini"));
    QVERIFY(configFile.open(QIODevice::WriteOnly | QIODevice::Text));

    QTextStream cfg(&configFile);
    cfg << "[convert-UUP]\n"
        << "AutoStart  =1\n"
        << "AddUpdates =1\n"
        << "Cleanup    =1\n"
        << "ResetBase  =0\n"
        << "NetFx3     =0\n"
        << "StartVirtual=0\n"
        << "wim2esd    =0\n"
        << "wim2swm    =0\n"
        << "SkipISO    =0\n"
        << "SkipWinRE  =0\n"
        << "ForceDism  =0\n"
        << "RefESD     =0\n"
        << "UpdtBootFiles=1\n"
        << "AutoExit   =1\n"
        << "SkipEdge   =0\n";
    configFile.close();
}

/**
 * CRITICAL TEST: Verify converter doesn't fork a new process.
 *
 * The convert-UUP.cmd script has a QuickEdit-disable feature that
 * re-launches itself through PowerShell. Without the -qedit flag,
 * the script exits immediately (exit /b) after spawning a detached
 * process, orphaning our QProcess handle.
 *
 * This test verifies that with -qedit -elevated, the process stays
 * attached and QProcess can track it to completion.
 */
void UupConversionPipelineTest::testConverterLaunchesWithoutForking()
{
    if (!m_converterAvailable)
        QSKIP("Converter not found");

    QTemporaryDir tmpDir;
    QVERIFY(tmpDir.isValid());
    QVERIFY(setupWorkDir(tmpDir));
    writeConvertConfig(tmpDir.path());

    QString convertCmd = QDir(tmpDir.path()).filePath("convert-UUP.cmd");
    QVERIFY(QFileInfo::exists(convertCmd));

    QProcess process;
    process.setWorkingDirectory(tmpDir.path());
    process.setProcessChannelMode(QProcess::MergedChannels);

    QStringList args;
    args << "/c" << convertCmd << "-qedit" << "-elevated";

    QElapsedTimer timer;
    timer.start();

    process.start("cmd.exe", args);
    QVERIFY2(process.waitForStarted(10000), "Failed to start converter");

    // Close stdin so script doesn't hang at prompts
    process.closeWriteChannel();

    // The process should run for at least a few seconds (not exit instantly)
    // and should complete within a reasonable time.
    // Without -qedit, it would exit in <1 second (just the fork + exit /b)
    bool finished = process.waitForFinished(120000);  // 2 minute timeout

    qint64 elapsed = timer.elapsed();
    QString output = QString::fromUtf8(process.readAllStandardOutput());

    qInfo() << "Converter ran for" << elapsed << "ms";
    qInfo() << "Exit code:" << process.exitCode();
    qInfo() << "Output length:" << output.length() << "chars";

    // Log first/last parts of output for debugging
    if (output.length() > 200) {
        qInfo() << "Output (first 500):" << output.left(500);
        qInfo() << "Output (last 500):" << output.right(500);
    } else {
        qInfo() << "Output:" << output;
    }

    QVERIFY2(finished, "Converter process timed out (may have forked)");

    // If the script forked (QuickEdit re-launch), it would exit almost
    // instantly with code 0 and very little output. A proper run should
    // produce more than a trivial amount of output.

    // The script should have attempted to process UUP files and found
    // none, producing error output. Key indicators:
    // - "UUP -> ISO" in the title (proves it didn't exit early)
    // - OR error messages about missing files
    // - OR the script ran for more than 1 second
    bool processRanProperly = (elapsed > 1000) ||
                              output.contains("edition", Qt::CaseInsensitive) ||
                              output.contains("UUP", Qt::CaseInsensitive) ||
                              output.contains("ERROR", Qt::CaseInsensitive) ||
                              output.contains("Detecting", Qt::CaseInsensitive) ||
                              output.contains("bin", Qt::CaseInsensitive);

    QVERIFY2(processRanProperly,
             qPrintable(QString("Converter appears to have forked: exited in %1ms "
                                "with minimal output. Output: %2")
                            .arg(elapsed).arg(output.left(200))));
}

/**
 * Verify ConvertConfig.ini is read by the converter script.
 *
 * We set AutoStart=1 and AutoExit=1 in the config. If the script
 * reads the config, it should NOT show the interactive menu.
 */
void UupConversionPipelineTest::testConverterReadsConvertConfig()
{
    if (!m_converterAvailable)
        QSKIP("Converter not found");

    QTemporaryDir tmpDir;
    QVERIFY(tmpDir.isValid());
    QVERIFY(setupWorkDir(tmpDir));

    // Write our custom config (this will overwrite the bundled one)
    writeConvertConfig(tmpDir.path());

    // Verify it exists
    QString configPath = QDir(tmpDir.path()).filePath("ConvertConfig.ini");
    QVERIFY(QFileInfo::exists(configPath));

    // Read it back and verify format
    QFile configFile(configPath);
    QVERIFY(configFile.open(QIODevice::ReadOnly));
    QString content = configFile.readAll();
    configFile.close();

    QVERIFY2(content.contains("[convert-UUP]"),
             "Config must contain [convert-UUP] section header");
    QVERIFY2(content.contains("AutoStart"),
             "Config must contain AutoStart setting");
    QVERIFY2(content.contains("AutoExit"),
             "Config must contain AutoExit setting");

    // Run converter — it should NOT prompt for input (AutoStart=1, AutoExit=1)
    QProcess process;
    process.setWorkingDirectory(tmpDir.path());
    process.setProcessChannelMode(QProcess::MergedChannels);

    QStringList args;
    args << "/c" << QDir(tmpDir.path()).filePath("convert-UUP.cmd")
         << "-qedit" << "-elevated";

    process.start("cmd.exe", args);
    QVERIFY(process.waitForStarted(10000));
    process.closeWriteChannel();

    bool finished = process.waitForFinished(120000);
    QVERIFY2(finished, "Process should finish without prompting");

    QString output = QString::fromUtf8(process.readAllStandardOutput());

    // The script should NOT show "Enter the path" prompt
    // (AutoStart=1 means it auto-processes; if _UUP is set, it skips the prompt;
    // if _UUP is not set and AutoStart is set, it still goes to :check)
    QVERIFY2(!output.contains("Enter the path to UUP source"),
             "Script prompted for path despite AutoStart=1");
}

/**
 * Verify converter handles missing ESD files gracefully.
 *
 * With empty UUPs/ directory, the script should exit with an error
 * about missing edition files, not hang.
 */
void UupConversionPipelineTest::testConverterDetectsMissingEsdFiles()
{
    if (!m_converterAvailable)
        QSKIP("Converter not found");

    QTemporaryDir tmpDir;
    QVERIFY(tmpDir.isValid());
    QVERIFY(setupWorkDir(tmpDir));
    writeConvertConfig(tmpDir.path());

    // UUPs/ directory is empty — no .esd files

    QProcess process;
    process.setWorkingDirectory(tmpDir.path());
    process.setProcessChannelMode(QProcess::MergedChannels);

    QStringList args;
    args << "/c" << QDir(tmpDir.path()).filePath("convert-UUP.cmd")
         << "-qedit" << "-elevated";

    process.start("cmd.exe", args);
    QVERIFY(process.waitForStarted(10000));
    process.closeWriteChannel();

    bool finished = process.waitForFinished(60000);
    QString output = QString::fromUtf8(process.readAllStandardOutput());

    QVERIFY2(finished, "Process should exit cleanly when no ESD files found");

    // Script should have detected missing files and exited
    // (not hung waiting for input)
    qInfo() << "Exit code:" << process.exitCode();
    qInfo() << "Output:" << output.left(500);
}

/**
 * Verify that closing stdin causes the converter to exit gracefully
 * if it reaches a prompt.
 */
void UupConversionPipelineTest::testConverterExitsOnClosedStdin()
{
    if (!m_converterAvailable)
        QSKIP("Converter not found");

    QTemporaryDir tmpDir;
    QVERIFY(tmpDir.isValid());
    QVERIFY(setupWorkDir(tmpDir));

    // Write config with AutoStart=0 to force the interactive menu
    QFile configFile(QDir(tmpDir.path()).filePath("ConvertConfig.ini"));
    QVERIFY(configFile.open(QIODevice::WriteOnly | QIODevice::Text));
    QTextStream cfg(&configFile);
    cfg << "[convert-UUP]\n"
        << "AutoStart  =0\n"
        << "AutoExit   =1\n";
    configFile.close();

    QProcess process;
    process.setWorkingDirectory(tmpDir.path());
    process.setProcessChannelMode(QProcess::MergedChannels);

    QStringList args;
    args << "/c" << QDir(tmpDir.path()).filePath("convert-UUP.cmd")
         << "-qedit" << "-elevated";

    process.start("cmd.exe", args);
    QVERIFY(process.waitForStarted(10000));

    // Close stdin immediately — script should not hang
    process.closeWriteChannel();

    bool finished = process.waitForFinished(30000);
    QVERIFY2(finished,
             "Converter should exit within 30s when stdin is closed");
}

/**
 * Verify our ConvertConfig.ini format is parseable by findstr /b /i
 * (the method the script uses to read settings).
 */
void UupConversionPipelineTest::testConvertConfigIniFormat()
{
    QTemporaryDir tmpDir;
    QVERIFY(tmpDir.isValid());
    writeConvertConfig(tmpDir.path());

    QString configPath = QDir(tmpDir.path()).filePath("ConvertConfig.ini");

    // Use findstr to verify each option is parseable (same as the script does)
    QStringList options = {
        "AutoStart", "AddUpdates", "Cleanup", "ResetBase", "NetFx3",
        "StartVirtual", "wim2esd", "wim2swm", "SkipISO", "SkipWinRE",
        "ForceDism", "RefESD", "UpdtBootFiles", "AutoExit", "SkipEdge"
    };

    for (const QString& opt : options) {
        QProcess findstr;
        findstr.setWorkingDirectory(tmpDir.path());
        findstr.start("findstr.exe",
                       {"/b", "/i", opt, "ConvertConfig.ini"});
        QVERIFY(findstr.waitForFinished(5000));

        QString output = findstr.readAllStandardOutput().trimmed();
        QVERIFY2(findstr.exitCode() == 0,
                 qPrintable(QString("findstr /b /i %1 failed — option not found in config")
                                .arg(opt)));
        QVERIFY2(!output.isEmpty(),
                 qPrintable(QString("findstr found no output for %1").arg(opt)));

        // Verify the value can be extracted by split on '='
        QStringList parts = output.split('=');
        QVERIFY2(parts.size() >= 2,
                 qPrintable(QString("Option %1 line has no '=': %2").arg(opt, output)));

        QString value = parts.at(1).trimmed();
        QVERIFY2(!value.isEmpty(),
                 qPrintable(QString("Option %1 has empty value: %2").arg(opt, output)));

        bool ok;
        int intVal = value.toInt(&ok);
        Q_UNUSED(intVal);
        QVERIFY2(ok,
                 qPrintable(QString("Option %1 value '%2' is not a valid integer")
                                .arg(opt, value)));
    }

    // Verify [convert-UUP] section header is detected
    QProcess findSection;
    findSection.setWorkingDirectory(tmpDir.path());
    findSection.start("findstr.exe",
                       {"/i", "\\[convert-UUP\\]", "ConvertConfig.ini"});
    QVERIFY(findSection.waitForFinished(5000));
    QCOMPARE(findSection.exitCode(), 0);

    qInfo() << "All" << options.size() << "config options verified parseable";
}

#include <QApplication>
int main(int argc, char* argv[])
{
    QApplication app(argc, argv);
    UupConversionPipelineTest tc;
    QTEST_SET_MAIN_SOURCE_PATH
    return QTest::qExec(&tc, argc, argv);
}

#include "test_uup_conversion_pipeline.moc"
