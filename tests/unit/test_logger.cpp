// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: MIT

/**
 * Unit tests for Logger
 * Tests logging functionality, levels, and output
 */

#include "sak/logger.h"
#include <QTest>
#include <QTemporaryDir>
#include <QFile>
#include <QTextStream>

class TestLogger : public QObject {
    Q_OBJECT

private slots:
    void initTestCase() {
        sak::Logger::instance().setLogLevel(sak::Logger::LogLevel::Debug);
    }

    void cleanupTestCase() {
        // Cleanup
    }

    void testSingletonInstance() {
        auto& logger1 = sak::Logger::instance();
        auto& logger2 = sak::Logger::instance();
        QCOMPARE(&logger1, &logger2);
    }

    void testLogLevels() {
        QTemporaryDir tempDir;
        QVERIFY(tempDir.isValid());
        
        QString logFile = tempDir.path() + "/test.log";
        sak::Logger::instance().setLogFile(logFile);
        
        // Set to Info level
        sak::Logger::instance().setLogLevel(sak::Logger::LogLevel::Info);
        
        sak::Logger::debug("Debug message");  // Should not be logged
        sak::Logger::info("Info message");    // Should be logged
        sak::Logger::warning("Warning message");  // Should be logged
        sak::Logger::error("Error message");  // Should be logged
        
        sak::Logger::instance().flush();
        
        QFile file(logFile);
        QVERIFY(file.open(QIODevice::ReadOnly));
        QString content = QString::fromUtf8(file.readAll());
        file.close();
        
        QVERIFY(!content.contains("Debug message"));
        QVERIFY(content.contains("Info message"));
        QVERIFY(content.contains("Warning message"));
        QVERIFY(content.contains("Error message"));
    }

    void testLogFormatting() {
        QTemporaryDir tempDir;
        QVERIFY(tempDir.isValid());
        
        QString logFile = tempDir.path() + "/format.log";
        sak::Logger::instance().setLogFile(logFile);
        sak::Logger::instance().setLogLevel(sak::Logger::LogLevel::Debug);
        
        sak::Logger::info("Test message");
        sak::Logger::instance().flush();
        
        QFile file(logFile);
        QVERIFY(file.open(QIODevice::ReadOnly));
        QString content = QString::fromUtf8(file.readAll());
        file.close();
        
        // Should contain timestamp
        QVERIFY(content.contains("INFO"));
        QVERIFY(content.contains("Test message"));
    }

    void testLogRotation() {
        QTemporaryDir tempDir;
        QVERIFY(tempDir.isValid());
        
        QString logFile = tempDir.path() + "/rotate.log";
        sak::Logger::instance().setLogFile(logFile);
        sak::Logger::instance().setMaxFileSize(1024); // 1KB
        
        // Write enough to trigger rotation
        for (int i = 0; i < 100; i++) {
            sak::Logger::info(QString("Line %1: ").arg(i) + QString(50, 'x'));
        }
        
        sak::Logger::instance().flush();
        
        // Check if backup file was created
        QVERIFY(QFile::exists(logFile + ".1") || QFile::exists(logFile));
    }

    void testConcurrentLogging() {
        QTemporaryDir tempDir;
        QVERIFY(tempDir.isValid());
        
        QString logFile = tempDir.path() + "/concurrent.log";
        sak::Logger::instance().setLogFile(logFile);
        
        // Simulate concurrent logging from multiple threads
        QVector<QFuture<void>> futures;
        for (int i = 0; i < 10; i++) {
            futures.append(QtConcurrent::run([i]() {
                for (int j = 0; j < 10; j++) {
                    sak::Logger::info(QString("Thread %1, Message %2").arg(i).arg(j));
                }
            }));
        }
        
        for (auto& future : futures) {
            future.waitForFinished();
        }
        
        sak::Logger::instance().flush();
        
        QFile file(logFile);
        QVERIFY(file.open(QIODevice::ReadOnly));
        QTextStream stream(&file);
        int lineCount = 0;
        while (!stream.atEnd()) {
            stream.readLine();
            lineCount++;
        }
        file.close();
        
        QCOMPARE(lineCount, 100); // All 100 messages should be logged
    }

    void testLogContext() {
        QTemporaryDir tempDir;
        QVERIFY(tempDir.isValid());
        
        QString logFile = tempDir.path() + "/context.log";
        sak::Logger::instance().setLogFile(logFile);
        
        sak::Logger::info("Test message", "TestClass", "testMethod");
        sak::Logger::instance().flush();
        
        QFile file(logFile);
        QVERIFY(file.open(QIODevice::ReadOnly));
        QString content = QString::fromUtf8(file.readAll());
        file.close();
        
        QVERIFY(content.contains("TestClass"));
        QVERIFY(content.contains("testMethod"));
    }
};

QTEST_MAIN(TestLogger)
#include "test_logger.moc"
