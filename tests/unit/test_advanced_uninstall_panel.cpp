// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file test_advanced_uninstall_panel.cpp
/// @brief A program's install-folder size is measured on demand, off the GUI thread, and cached.

#include "sak/advanced_uninstall_controller.h"
#include "sak/advanced_uninstall_panel.h"
#include "sak/advanced_uninstall_types.h"
#include "sak/style_constants.h"

#include <QApplication>
#include <QDialog>
#include <QDir>
#include <QFile>
#include <QFormLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSignalSpy>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QTimer>
#include <QtTest/QtTest>

#include <memory>

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
    void versionSizeAndDateColumnsFitWhatTheyHold();
    void remoteInstallFolderOffersNoMeasurement();
    void addToBatchTicksTheProgramsRow();
    void ticksSurviveTheSearchText();
    void aProvisionedPackageSaysWhatItIs();
    void aFinishedRefreshLeavesNoProgressBar();
    void aRegistryKeyIsShownWholeOnOneLine();
    void theUninstallConfirmationMatchesTheOtherUninstalls();
    void theNameColoursHaveAKey();
    void theBatchDialogHasOneStrongButtonAndCountsItsPrograms();
};

// Review 3 (X10): Remove Selected and Clear Queue were primary blue beside the danger-toned Start
// Batch Uninstall. They edit the queue, so they are secondary. The header said "1 programs".
void AdvancedUninstallPanelTests::theBatchDialogHasOneStrongButtonAndCountsItsPrograms() {
    sak::AdvancedUninstallPanel panel;
    sak::ProgramInfo program;
    program.displayName = QStringLiteral("7-Zip 26.05 (x64)");
    program.registryKeyPath = QStringLiteral("HKLM/Uninstall/7-Zip");
    QVERIFY(panel.m_controller->addToQueue(
        program, panel.m_controller->defaultScanLevel(), panel.m_controller->autoCleanSafe()));
    QHash<QString, QString> styles;
    QStringList labels;
    bool seen = false;
    QTimer::singleShot(0, &panel, [&]() {
        auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
        if (dialog == nullptr) {
            return;
        }
        seen = true;
        for (auto* button : dialog->findChildren<QPushButton*>()) {
            styles.insert(button->text(), button->styleSheet());
        }
        for (auto* label : dialog->findChildren<QLabel*>()) {
            labels.append(label->text());
        }
        dialog->reject();
    });
    panel.showBatchUninstallDialog();
    QVERIFY(seen);
    QCOMPARE(styles.value(QStringLiteral("Remove Selected")), sak::ui::kSecondaryButtonStyle);
    QCOMPARE(styles.value(QStringLiteral("Clear Queue")), sak::ui::kSecondaryButtonStyle);
    QCOMPARE(styles.value(QStringLiteral("Start Batch Uninstall")), sak::ui::kDangerButtonStyle);
    QVERIFY2(labels.contains(QStringLiteral("<b>Batch Uninstall Queue</b> - 1 program")),
             qPrintable(labels.join(QLatin1Char('|'))));
}

// The batch is built from the ticked rows. Add to Batch Queue once wrote to a separate queue, which
// pressing Batch emptied, so it did nothing at all.
// Tick 7-Zip under the search "7-Zip", search "VLC" and tick VLC: Batch listed only VLC, and with
// the search cleared it listed nothing. A new search text rebuilt the table and its ticks.
void AdvancedUninstallPanelTests::ticksSurviveTheSearchText() {
    sak::AdvancedUninstallPanel panel;
    sak::ProgramInfo zip;
    zip.displayName = QStringLiteral("7-Zip 26.05 (x64)");
    zip.registryKeyPath = QStringLiteral("HKLM/Uninstall/7-Zip");
    sak::ProgramInfo vlc;
    vlc.displayName = QStringLiteral("VLC media player");
    vlc.registryKeyPath = QStringLiteral("HKLM/Uninstall/VLC");
    panel.onEnumerationFinished({zip, vlc});
    const auto tick_only_row = [&panel]() {
        QCOMPARE(panel.m_program_table->rowCount(), 1);
        panel.m_program_table->item(0, 0)->setCheckState(Qt::Checked);
    };
    panel.onSearchTextChanged(QStringLiteral("7-Zip"));
    tick_only_row();
    panel.onSearchTextChanged(QStringLiteral("VLC"));
    tick_only_row();
    QCOMPARE(panel.selectedPrograms().size(), 2);
    panel.onSearchTextChanged(QString());
    QCOMPARE(panel.m_program_table->rowCount(), 2);
    bool all_resolved = false;
    QCOMPARE(panel.selectedPrograms(&all_resolved).size(), 2);
    QVERIFY(all_resolved);
    for (int row = 0; row < 2; ++row) {
        QCOMPARE(panel.m_program_table->item(row, 0)->checkState(), Qt::Checked);
    }
    // Untick one and it leaves the batch, whatever the search shows.
    panel.m_program_table->item(0, 0)->setCheckState(Qt::Unchecked);
    QCOMPARE(panel.selectedPrograms().size(), 1);
}

// Clipchamp.Clipchamp appeared twice, once with a publisher and once without: the installed
// package and the copy Windows provisions for new accounts.
void AdvancedUninstallPanelTests::aProvisionedPackageSaysWhatItIs() {
    sak::AdvancedUninstallPanel panel;
    sak::ProgramInfo installed;
    installed.displayName = QStringLiteral("Clipchamp.Clipchamp");
    installed.source = sak::ProgramInfo::Source::UWP;
    sak::ProgramInfo provisioned = installed;
    provisioned.source = sak::ProgramInfo::Source::Provisioned;
    panel.onEnumerationFinished({installed, provisioned});
    QStringList names;
    for (int row = 0; row < panel.m_program_table->rowCount(); ++row) {
        names.append(panel.m_program_table->item(row, 2)->text());
    }
    QVERIFY2(names.contains(QStringLiteral("Clipchamp.Clipchamp")),
             qPrintable(names.join(QLatin1Char('|'))));
    QVERIFY2(names.contains(QStringLiteral("Clipchamp.Clipchamp (provisioned for new accounts)")),
             qPrintable(names.join(QLatin1Char('|'))));
}

void AdvancedUninstallPanelTests::addToBatchTicksTheProgramsRow() {
    sak::AdvancedUninstallPanel panel;
    sak::ProgramInfo first;
    first.displayName = QStringLiteral("First App");
    sak::ProgramInfo second;
    second.displayName = QStringLiteral("Second App");
    panel.m_all_programs = {first, second};
    panel.m_filtered_programs = {first, second};
    panel.populateProgramTable({first, second});
    QCOMPARE(panel.selectedPrograms().size(), 0);

    panel.m_program_table->setCurrentCell(1, 0);
    panel.contextAddToQueue();

    const auto selected = panel.selectedPrograms();
    QCOMPARE(selected.size(), 1);
    QCOMPARE(selected.first().displayName, panel.selectedProgram().displayName);
}

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

// UI re-check N5: after Refresh the status bar kept a 0% bar beside "Ready": each end sent 0 of 1.
// An end is its maximum, which the status bar shows briefly and hides.
void AdvancedUninstallPanelTests::aFinishedRefreshLeavesNoProgressBar() {
    sak::AdvancedUninstallPanel panel;
    QSignalSpy progress(&panel, &sak::AdvancedUninstallPanel::progressUpdate);
    panel.onEnumerationFinished({});
    QVERIFY(!progress.isEmpty());
    const QList<QVariant> last = progress.last();
    QVERIFY(last.at(1).toInt() > 0);
    QCOMPARE(last.at(0).toInt(), last.at(1).toInt());
}

// Review 2 (N7): the Registry Key label wrapped "...\Notepad++" into "Notepad+" and a lone "+".
// Commands and keys are read-only one-line fields, whole and copyable.
void AdvancedUninstallPanelTests::aRegistryKeyIsShownWholeOnOneLine() {
    const sak::AdvancedUninstallPanel panel;
    sak::ProgramInfo program;
    program.displayName = QStringLiteral("Notepad++ (64-bit x64)");
    program.registryKeyPath =
        QStringLiteral("HKLM\\SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\Notepad++");
    program.uninstallString = QStringLiteral("\"C:\\Program Files\\Notepad++\\uninstall.exe\"");
    QWidget form_owner;
    auto* form = new QFormLayout(&form_owner);
    panel.populateProgramPropertiesForm(program, &form_owner, form);
    QHash<QString, QLineEdit*> fields;
    for (auto* field : form_owner.findChildren<QLineEdit*>()) {
        fields.insert(field->accessibleName(), field);
    }
    for (const auto& [name, value] :
         {std::pair{QStringLiteral("Registry Key"), program.registryKeyPath},
          std::pair{QStringLiteral("Uninstall Command"), program.uninstallString}}) {
        QLineEdit* field = fields.value(name);
        QVERIFY2(field != nullptr, qPrintable(name));
        QVERIFY(field->isReadOnly());
        QCOMPARE(field->text(), value);
    }
}

// Review 2 (N8): the bold name broke at "++", and Uninstall was primary blue where Force Uninstall
// and Batch Uninstall are the danger tone.
void AdvancedUninstallPanelTests::theUninstallConfirmationMatchesTheOtherUninstalls() {
    sak::AdvancedUninstallPanel panel;
    sak::ProgramInfo program;
    program.displayName = QStringLiteral("Notepad++ (64-bit x64)");
    QString header;
    QString button_style;
    QTimer::singleShot(0, &panel, [&]() {
        auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
        if (dialog == nullptr) {
            return;
        }
        for (auto* label : dialog->findChildren<QLabel*>()) {
            if (label->text().startsWith(QStringLiteral("Uninstall <b>"))) {
                header = label->text();
            }
        }
        for (auto* button : dialog->findChildren<QPushButton*>()) {
            if (button->text() == QStringLiteral("Uninstall")) {
                button_style = button->styleSheet();
            }
        }
        dialog->reject();
    });
    panel.showUninstallConfirmation(program);
    QCOMPARE(header, QStringLiteral("Uninstall <b>Notepad+\u2060+ (64-bit x64)</b>?"));
    QCOMPARE(button_style, sak::ui::kDangerButtonStyle);
}

// Review 2 (DF10): a red program name, and orange ones in the bloatware view, with nothing saying
// why. The list keeps a key in those colours, in the words each name's tooltip uses.
void AdvancedUninstallPanelTests::theNameColoursHaveAKey() {
    sak::AdvancedUninstallPanel panel;
    sak::ProgramInfo bloat;
    bloat.displayName = QStringLiteral("Candy Game");
    bloat.isBloatware = true;
    sak::ProgramInfo orphan;
    orphan.displayName = QStringLiteral("Gone Tool");
    orphan.isOrphaned = true;
    const std::unique_ptr<QTableWidgetItem> bloat_name(panel.programNameItem(bloat, 0));
    const std::unique_ptr<QTableWidgetItem> orphan_name(panel.programNameItem(orphan, 1));
    auto* bloat_key = panel.findChild<QLabel*>(QStringLiteral("programColorKeyBloatware"));
    auto* orphan_key = panel.findChild<QLabel*>(QStringLiteral("programColorKeyOrphaned"));
    QVERIFY(bloat_key != nullptr && orphan_key != nullptr);
    QCOMPARE(bloat_key->text(), bloat_name->toolTip());
    QCOMPARE(orphan_key->text(), orphan_name->toolTip());
}

// Review 3 (S15): a fixed Version column cut "10.0.19640...." beside half-empty Size and Install
// Date columns.
void AdvancedUninstallPanelTests::versionSizeAndDateColumnsFitWhatTheyHold() {
    sak::AdvancedUninstallPanel panel;
    QTableWidget* programs = panel.m_program_table;
    QVERIFY(programs != nullptr);
    for (int column = 0; column < programs->columnCount(); ++column) {
        const QString label = programs->horizontalHeaderItem(column) != nullptr
                                  ? programs->horizontalHeaderItem(column)->text()
                                  : QString();
        if (label == QStringLiteral("Version") || label == QStringLiteral("Size") ||
            label == QStringLiteral("Install Date")) {
            QCOMPARE(programs->horizontalHeader()->sectionResizeMode(column),
                     QHeaderView::ResizeToContents);
        }
    }
}

QTEST_MAIN(AdvancedUninstallPanelTests)
#include "test_advanced_uninstall_panel.moc"
