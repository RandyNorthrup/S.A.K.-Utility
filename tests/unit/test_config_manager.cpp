// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: MIT

/**
 * Unit tests for ConfigManager
 * Tests configuration loading, saving, and validation
 */

#include "sak/config_manager.h"
#include <QTest>
#include <QTemporaryDir>
#include <QFile>

class TestConfigManager : public QObject {
    Q_OBJECT

private:
    QString testConfigPath;
    QTemporaryDir* tempDir;

private slots:
    void initTestCase() {
        tempDir = new QTemporaryDir();
        QVERIFY(tempDir->isValid());
        testConfigPath = tempDir->path() + "/test_config.ini";
    }

    void cleanupTestCase() {
        delete tempDir;
    }

    void init() {
        // Clean before each test
        if (QFile::exists(testConfigPath)) {
            QFile::remove(testConfigPath);
        }
    }

    void testDefaultConfiguration() {
        sak::ConfigManager config(testConfigPath);
        
        // Check default values
        QVERIFY(!config.getValue("nonexistent", "").toString().isEmpty() == false);
        QCOMPARE(config.getValue("general/theme", "light").toString(), QString("light"));
    }

    void testSetAndGetValues() {
        sak::ConfigManager config(testConfigPath);
        
        config.setValue("general/app_name", "SAK Utility");
        config.setValue("general/version", "0.5.6");
        config.setValue("backup/compression_level", 9);
        config.setValue("backup/enabled", true);
        
        QCOMPARE(config.getValue("general/app_name").toString(), QString("SAK Utility"));
        QCOMPARE(config.getValue("general/version").toString(), QString("0.5.6"));
        QCOMPARE(config.getValue("backup/compression_level").toInt(), 9);
        QCOMPARE(config.getValue("backup/enabled").toBool(), true);
    }

    void testPersistence() {
        {
            sak::ConfigManager config(testConfigPath);
            config.setValue("test/value", "persistent");
            config.sync();
        }
        
        // Load in new instance
        sak::ConfigManager config2(testConfigPath);
        QCOMPARE(config2.getValue("test/value").toString(), QString("persistent"));
    }

    void testRemoveValue() {
        sak::ConfigManager config(testConfigPath);
        
        config.setValue("test/remove", "value");
        QVERIFY(config.contains("test/remove"));
        
        config.remove("test/remove");
        QVERIFY(!config.contains("test/remove"));
    }

    void testGroups() {
        sak::ConfigManager config(testConfigPath);
        
        config.setValue("group1/key1", "value1");
        config.setValue("group1/key2", "value2");
        config.setValue("group2/key1", "value1");
        
        auto groups = config.getGroups();
        QVERIFY(groups.contains("group1"));
        QVERIFY(groups.contains("group2"));
    }

    void testKeys() {
        sak::ConfigManager config(testConfigPath);
        
        config.setValue("test/key1", "value1");
        config.setValue("test/key2", "value2");
        config.setValue("test/key3", "value3");
        
        auto keys = config.getKeys("test");
        QCOMPARE(keys.size(), 3);
        QVERIFY(keys.contains("key1"));
        QVERIFY(keys.contains("key2"));
        QVERIFY(keys.contains("key3"));
    }

    void testClear() {
        sak::ConfigManager config(testConfigPath);
        
        config.setValue("test1/key", "value");
        config.setValue("test2/key", "value");
        
        config.clear();
        
        QVERIFY(!config.contains("test1/key"));
        QVERIFY(!config.contains("test2/key"));
    }

    void testPortableMode() {
        // Test portable.ini detection
        QString portableIni = tempDir->path() + "/portable.ini";
        QFile file(portableIni);
        file.open(QIODevice::WriteOnly);
        file.close();
        
        sak::ConfigManager config(testConfigPath);
        QVERIFY(config.isPortableMode());
    }

    void testArrayValues() {
        sak::ConfigManager config(testConfigPath);
        
        QStringList list = {"item1", "item2", "item3"};
        config.setArrayValue("test/list", list);
        
        QStringList retrieved = config.getArrayValue("test/list");
        QCOMPARE(retrieved, list);
    }

    void testTypeConversions() {
        sak::ConfigManager config(testConfigPath);
        
        // String to int
        config.setValue("test/number", "42");
        QCOMPARE(config.getValue("test/number").toInt(), 42);
        
        // String to bool
        config.setValue("test/bool1", "true");
        config.setValue("test/bool2", "1");
        QVERIFY(config.getValue("test/bool1").toBool());
        QVERIFY(config.getValue("test/bool2").toBool());
        
        // String to double
        config.setValue("test/double", "3.14");
        QCOMPARE(config.getValue("test/double").toDouble(), 3.14);
    }
};

QTEST_MAIN(TestConfigManager)
#include "test_config_manager.moc"
