// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file test_advanced_uninstall_panel.cpp
/// @brief A program's install-folder size is measured on demand, off the GUI thread, and cached.

#include "sak/advanced_uninstall_panel.h"
#include "sak/advanced_uninstall_types.h"

#include <QDialog>
#include <QDir>
#include <QFile>
#include <QFormLayout>
#include <QLabel>
#include <QPushButton>
#include <QTemporaryDir>
#include <QtTest/QtTest>

namespace {
constexpr qint64 kPayloadBytes = 64 * 1024;
constexpr int kMeasureTimeoutMs = 30'000;

QPushButton* measureButton(QDialog* dialog) {
    for (auto* button : dialog->findChildren<QPushButton*>()) {
        if (button->accessibleName() == QStringLiteral("Measure install folder size")) {
            return button;
        }
    }
    return nullptr;
}

/// The value label sits beside the Measure button in the same row widget.
QLabel* measuredValue(QPushButton* button) {
    return button->parentWidget()->findChild<QLabel*>();
}
}  // namespace

class AdvancedUninstallPanelTests : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void installFolderIsMeasuredOnDemandAndCached();
    void remoteInstallFolderOffersNoMeasurement();
};

// The program list used to walk every program's install folder during enumeration. It now
// shows the registry's EstimatedSize, and a folder is measured only when the user asks, for one
// program; the answer is kept for the next time that program's properties are opened.
void AdvancedUninstallPanelTests::installFolderIsMeasuredOnDemandAndCached() {
    QTemporaryDir temp_dir;
    QVERIFY(temp_dir.isValid());
    QFile file(QDir(temp_dir.path()).filePath(QStringLiteral("payload.bin")));
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write(QByteArray(kPayloadBytes, 'P'));
    file.close();

    sak::AdvancedUninstallPanel panel;
    sak::ProgramInfo program;
    program.displayName = QStringLiteral("Fixture");
    program.installLocation = temp_dir.path();
    const QString expected = panel.formatSize(kPayloadBytes);

    {
        QDialog dialog;
        auto* form = new QFormLayout(&dialog);
        panel.addInstallFolderSizeRow(program, &dialog, form);
        auto* button = measureButton(&dialog);
        QVERIFY2(button != nullptr, "a local install folder offers a Measure button");
        auto* value = measuredValue(button);
        QVERIFY(value != nullptr);
        QCOMPARE(value->text(), QStringLiteral("Not measured"));

        button->click();
        QTRY_COMPARE_WITH_TIMEOUT(value->text(), expected, kMeasureTimeoutMs);
        QVERIFY(button->isEnabled());
        QCOMPARE(button->text(), QStringLiteral("Measure Again"));
    }

    // Reopening the properties shows the measured size at once, without walking again.
    QDialog reopened;
    auto* form = new QFormLayout(&reopened);
    panel.addInstallFolderSizeRow(program, &reopened, form);
    auto* button = measureButton(&reopened);
    QVERIFY(button != nullptr);
    QCOMPARE(measuredValue(button)->text(), expected);
    QCOMPARE(button->text(), QStringLiteral("Measure Again"));
}

void AdvancedUninstallPanelTests::remoteInstallFolderOffersNoMeasurement() {
    sak::AdvancedUninstallPanel panel;
    sak::ProgramInfo program;
    program.installLocation = QStringLiteral("\\\\server\\share\\App");
    QDialog dialog;
    auto* form = new QFormLayout(&dialog);
    panel.addInstallFolderSizeRow(program, &dialog, form);
    QVERIFY(measureButton(&dialog) == nullptr);
    QCOMPARE(form->rowCount(), 0);
}

QTEST_MAIN(AdvancedUninstallPanelTests)
#include "test_advanced_uninstall_panel.moc"
