// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file test_wifi_manager_panel.cpp
/// @brief Unit tests for WifiManagerPanel::buildWindowsScript SSID injection safety

#include "sak/style_constants.h"
#include "sak/wifi_manager_panel.h"

#include <QApplication>
#include <QDialog>
#include <QHeaderView>
#include <QLabel>
#include <QLayout>
#include <QLineEdit>
#include <QPushButton>
#include <QStackedWidget>
#include <QTableWidget>
#include <QTimer>
#include <QtTest/QtTest>

#include <functional>

using namespace sak;

namespace {

QPushButton* buttonNamed(const QWidget& owner, const QString& name) {
    for (auto* button : owner.findChildren<QPushButton*>()) {
        if (button->accessibleName() == name || button->text() == name) {
            return button;
        }
    }
    return nullptr;
}

/// What one page of a QR dialog shows: whether Cancel sits right of the action, which buttons are
/// primary, and the Browse button's style.
struct QrButtonRow {
    bool cancel_last{false};
    QStringList primary;
    QString browse_style;
};

QrButtonRow readQrButtonRow(QWidget* dialog, const QString& action) {
    QrButtonRow row;
    QPushButton* action_button = nullptr;
    QPushButton* cancel = nullptr;
    for (auto* button : dialog->findChildren<QPushButton*>()) {
        if (!button->isVisibleTo(dialog)) {
            continue;
        }
        if (button->styleSheet() == sak::ui::kPrimaryButtonStyle) {
            row.primary.append(button->text());
        }
        if (button->text() == action) {
            action_button = button;
        } else if (button->text() == QStringLiteral("Cancel")) {
            cancel = button;
        } else if (button->text() == QStringLiteral("Browse...")) {
            row.browse_style = button->styleSheet();
        }
    }
    row.cancel_last = action_button != nullptr && cancel != nullptr &&
                      cancel->mapTo(dialog, QPoint()).x() >
                          action_button->mapTo(dialog, QPoint()).x();
    return row;
}

/// Presses Generate QR Code and hands the dialog it opens to @p inspect, inside its own loop.
void inspectQrDialog(const WifiManagerPanel& panel, const std::function<void(QDialog*)>& inspect) {
    bool seen = false;
    QTimer::singleShot(1, qApp, [&seen, inspect]() {
        auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
        if (dialog == nullptr) {
            return;
        }
        seen = true;
        inspect(dialog);
        dialog->reject();
    });
    QPushButton* generate = buttonNamed(panel, QStringLiteral("Generate QR Code"));
    QVERIFY(generate != nullptr);
    generate->click();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
    QVERIFY(seen);
}

}  // namespace

class WifiManagerPanelTests : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void savedNetworksShowNoRowNumbers();
    void normalSsidProducesScript();
    void percentSsidIsDoubled();
    void ampersandSsidIsCaretEscaped();
    void quoteSsidIsRejected();
    void controlCharSsidIsRejected();
    void envVarSsidNotExpandedInConnect();
    void jsonWriteFailsClosedOnShortWrite();
    void installRefusesIncompleteRows();
    void deleteSelectedNeedsASelectedRow();
    void thePasswordRowLinesUpWithTheOthers();
    void anEmptyTableSaysHowToFillIt();
    void qrDialogsPutCancelLastWithOnePrimaryButton();
};

// Review 3 (X10): the QR dialogs put Cancel before Next, Generate and Batch Generate, where every
// other dialog puts it last, and Browse was a second primary button beside Generate.
void WifiManagerPanelTests::qrDialogsPutCancelLastWithOnePrimaryButton() {
    WifiManagerPanel panel;
    QLineEdit* ssid = nullptr;
    for (auto* edit : panel.findChildren<QLineEdit*>()) {
        if (edit->accessibleName() == QStringLiteral("Network SSID")) {
            ssid = edit;
        }
    }
    QVERIFY(ssid != nullptr);
    ssid->setText(QStringLiteral("Example-Guest"));

    QrButtonRow format_page;
    QrButtonRow output_page;
    inspectQrDialog(panel, [&](QDialog* dialog) {
        auto* pages = dialog->findChild<QStackedWidget*>();
        QVERIFY(pages != nullptr);
        format_page = readQrButtonRow(dialog, QStringLiteral("Next >"));
        pages->setCurrentIndex(1);
        dialog->layout()->activate();
        output_page = readQrButtonRow(dialog, QStringLiteral("Generate"));
    });
    QVERIFY(format_page.cancel_last);
    QCOMPARE(format_page.primary, QStringList{QStringLiteral("Next >")});
    QVERIFY(output_page.cancel_last);
    QCOMPARE(output_page.primary, QStringList{QStringLiteral("Generate")});
    QCOMPARE(output_page.browse_style, QString(sak::ui::kSecondaryButtonStyle));

    // Two ticked rows take the batch dialog.
    QPushButton* add = buttonNamed(panel, QStringLiteral("Add to Table"));
    QVERIFY(add != nullptr);
    add->click();
    ssid->setText(QStringLiteral("Example-Office"));
    add->click();
    QTableWidget* table = nullptr;
    for (auto* candidate : panel.findChildren<QTableWidget*>()) {
        if (candidate->accessibleName() == QStringLiteral("Saved WiFi Networks Table")) {
            table = candidate;
        }
    }
    QVERIFY(table != nullptr);
    QCOMPARE(table->rowCount(), 2);
    for (int row = 0; row < table->rowCount(); ++row) {
        QVERIFY(table->item(row, 0) != nullptr);
        table->item(row, 0)->setCheckState(Qt::Checked);
    }
    QrButtonRow batch;
    inspectQrDialog(panel, [&](QDialog* dialog) {
        batch = readQrButtonRow(dialog, QStringLiteral("Batch Generate"));
    });
    QVERIFY(batch.cancel_last);
    QCOMPARE(batch.browse_style, QString(sak::ui::kSecondaryButtonStyle));
    // Batch Generate starts disabled, until a folder is chosen; it is still the one primary.
    QCOMPARE(batch.primary, QStringList{QStringLiteral("Batch Generate")});
}

// Review 2: the Password field stood 10 px in from the fields above and below it, its label riding
// above it, because its row kept a layout's own margins.
void WifiManagerPanelTests::thePasswordRowLinesUpWithTheOthers() {
    WifiManagerPanel panel;
    panel.resize(1280, 800);
    panel.show();
    QVERIFY(QTest::qWaitForWindowExposed(&panel));
    QLineEdit* ssid = nullptr;
    QLineEdit* password = nullptr;
    for (auto* edit : panel.findChildren<QLineEdit*>()) {
        if (edit->accessibleName() == QStringLiteral("Network SSID")) {
            ssid = edit;
        } else if (edit->accessibleName() == QStringLiteral("Network Password")) {
            password = edit;
        }
    }
    QVERIFY(ssid != nullptr && password != nullptr);
    QCOMPARE(password->mapTo(&panel, QPoint(0, 0)).x(), ssid->mapTo(&panel, QPoint(0, 0)).x());
}

// Review 2 (F35): the empty Saved Networks table said nothing.
void WifiManagerPanelTests::anEmptyTableSaysHowToFillIt() {
    WifiManagerPanel panel;
    QTableWidget* table = nullptr;
    for (auto* candidate : panel.findChildren<QTableWidget*>()) {
        if (candidate->accessibleName() == QStringLiteral("Saved WiFi Networks Table")) {
            table = candidate;
        }
    }
    QVERIFY(table != nullptr);
    const QLabel* empty = table->viewport()->findChild<QLabel*>();
    QVERIFY(empty != nullptr);
    QVERIFY2(empty->text().startsWith(QStringLiteral("No networks yet.")),
             qPrintable(empty->text()));
}

// A benign SSID yields a runnable script with the base64 profile and netsh call.
void WifiManagerPanelTests::normalSsidProducesScript() {
    const QString script = WifiManagerPanel::buildWindowsScript("MyNetwork", "pw", "WPA2", false);
    QVERIFY(!script.isEmpty());
    // netsh is invoked by its absolute System32 path (anti-hijack), so match the
    // command + args rather than a bare "netsh" token.
    QVERIFY(
        script.contains(QStringLiteral("\"%SystemRoot%\\System32\\netsh.exe\" wlan add profile")));
    QVERIFY(script.contains(
        QStringLiteral("\"%SystemRoot%\\System32\\netsh.exe\" wlan connect name=\"MyNetwork\"")));
}

// P-W10 (SSID injection): '%' must be doubled so it is not expanded as an
// environment variable when the generated .cmd runs.
void WifiManagerPanelTests::percentSsidIsDoubled() {
    const QString script = WifiManagerPanel::buildWindowsScript("Net%PATH%", "pw", "WPA2", false);
    QVERIFY(!script.isEmpty());
    QVERIFY(script.contains(QStringLiteral("Net%%PATH%%")));
    QVERIFY(!script.contains(QStringLiteral("Net%PATH%")));  // raw form must never appear
}

// Caret metacharacters stay neutralized with a leading '^'.
void WifiManagerPanelTests::ampersandSsidIsCaretEscaped() {
    const QString script = WifiManagerPanel::buildWindowsScript("A&B|C", "pw", "WPA2", false);
    QVERIFY(!script.isEmpty());
    QVERIFY(script.contains(QStringLiteral("A^&B^|C")));
}

// A double quote would break out of the quoted netsh argument: fail closed.
void WifiManagerPanelTests::quoteSsidIsRejected() {
    const QString script =
        WifiManagerPanel::buildWindowsScript("Evil\" & calc & \"", "pw", "WPA2", false);
    QVERIFY(script.isEmpty());
}

// Control characters break the .cmd line structure: fail closed.
void WifiManagerPanelTests::controlCharSsidIsRejected() {
    QVERIFY(WifiManagerPanel::buildWindowsScript("Net\nrogue", "pw", "WPA2", false).isEmpty());
    QVERIFY(WifiManagerPanel::buildWindowsScript(QStringLiteral("Net\rX"), "pw", "WPA2", false)
                .isEmpty());
}

// The connect line must not carry an expandable env-var reference either.
void WifiManagerPanelTests::envVarSsidNotExpandedInConnect() {
    const QString script = WifiManagerPanel::buildWindowsScript("%COMSPEC%", "pw", "WPA2", false);
    QVERIFY(!script.isEmpty());
    QVERIFY(script.contains(QStringLiteral("name=\"%%COMSPEC%%\"")));
    QVERIFY(!script.contains(QStringLiteral("name=\"%COMSPEC%\"")));
}

// Fail-closed rule: the credential-table JSON write reports success only when
// every byte landed AND the atomic commit succeeded. A short write or a failed
// commit must never be reported as success (a truncated credential table).
void WifiManagerPanelTests::jsonWriteFailsClosedOnShortWrite() {
    // Full write + successful commit -> success.
    QVERIFY(WifiManagerPanel::jsonWriteSucceeded(128, 128, true));
    // Short write (partial byte count) -> failure regardless of commit flag.
    QVERIFY(!WifiManagerPanel::jsonWriteSucceeded(64, 128, true));
    QVERIFY(!WifiManagerPanel::jsonWriteSucceeded(64, 128, false));
    // Full byte count but the commit failed -> failure.
    QVERIFY(!WifiManagerPanel::jsonWriteSucceeded(128, 128, false));
    // QIODevice::write returns -1 on error -> failure.
    QVERIFY(!WifiManagerPanel::jsonWriteSucceeded(-1, 128, true));
}

// What a security label MEANS is no longer decided here. This panel used to carry its own
// resolver, and it disagreed with the shared one on the panel's OWN default label: it tested for
// "WPA3" before "WPA", so "WPA/WPA2/WPA3" -- the first entry in the panel's security combo, and
// therefore the setting every network gets unless the technician changes it -- resolved to a
// WPA3SAE-ONLY profile, which cannot associate with a WPA2-only access point and is rejected
// outright by pre-1903 Windows. Resolution now happens once, in sak::addWifiProfileWindows, whose
// own test pins every mapping including that combined label.
//
// What remains the panel's own rule is whether a ROW is complete enough to install at all.
void WifiManagerPanelTests::installRefusesIncompleteRows() {
    const auto row = [](const QString& ssid, const QString& security) {
        WifiManagerPanel::WifiConfig cfg;
        cfg.ssid = ssid;
        cfg.security = security;
        return cfg;
    };

    // A complete row is NOT refused -- without this the assertions below would pass just as
    // happily against a rule that refused everything.
    QCOMPARE(WifiManagerPanel::wifiProfileInstallRefusal(
                 row(QStringLiteral("Lab"), QStringLiteral("WPA/WPA2/WPA3"))),
             QString());

    QVERIFY(!WifiManagerPanel::wifiProfileInstallRefusal(
                 row(QString(), QStringLiteral("WPA/WPA2/WPA3")))
                 .isEmpty());
    QVERIFY(!WifiManagerPanel::wifiProfileInstallRefusal(
                 row(QStringLiteral("   "), QStringLiteral("WPA/WPA2/WPA3")))
                 .isEmpty());

    // An EMPTY security label is refused HERE, even though the shared installer would read it as
    // "caller did not specify, use the interoperable default". That default is right for the
    // assistant's connect_wifi, where security is an optional argument; it is wrong for this
    // table, where every row has a security column, so an empty one means the row is malformed.
    QVERIFY(!WifiManagerPanel::wifiProfileInstallRefusal(row(QStringLiteral("Lab"), QString()))
                 .isEmpty());
    QVERIFY(!WifiManagerPanel::wifiProfileInstallRefusal(
                 row(QStringLiteral("Lab"), QStringLiteral("  ")))
                 .isEmpty());

    // The two refusals name different causes, so a log line says which field was missing.
    QVERIFY(WifiManagerPanel::wifiProfileInstallRefusal(
                row(QString(), QStringLiteral("WPA/WPA2/WPA3"))) !=
            WifiManagerPanel::wifiProfileInstallRefusal(row(QStringLiteral("Lab"), QString())));
}

// UI review F20: the danger button was enabled with an empty table and nothing selected.
void WifiManagerPanelTests::deleteSelectedNeedsASelectedRow() {
    WifiManagerPanel panel;
    QPushButton* remove = nullptr;
    for (QPushButton* button : panel.findChildren<QPushButton*>()) {
        if (button->accessibleName() == QStringLiteral("Delete Selected Networks")) {
            remove = button;
        }
    }
    QVERIFY(remove != nullptr);
    QVERIFY(!remove->isEnabled());
    QTableWidget* table = nullptr;
    for (QTableWidget* candidate : panel.findChildren<QTableWidget*>()) {
        if (candidate->selectionBehavior() == QAbstractItemView::SelectRows &&
            candidate->selectionMode() == QAbstractItemView::ExtendedSelection) {
            table = candidate;
        }
    }
    QVERIFY(table != nullptr);
    table->insertRow(0);
    for (int column = 0; column < table->columnCount(); ++column) {
        table->setItem(0, column, new QTableWidgetItem(QStringLiteral("Cafe")));
    }
    QVERIFY(!remove->isEnabled());
    table->selectRow(0);
    QVERIFY(remove->isEnabled());
    table->clearSelection();
    QVERIFY(!remove->isEnabled());
}

// UI review F42: only this table showed row numbers, "1", "2", beside its rows.
void WifiManagerPanelTests::savedNetworksShowNoRowNumbers() {
    WifiManagerPanel panel;
    QTableWidget* table = nullptr;
    for (QTableWidget* candidate : panel.findChildren<QTableWidget*>()) {
        if (candidate->accessibleName() == QStringLiteral("Saved WiFi Networks Table")) {
            table = candidate;
        }
    }
    QVERIFY(table != nullptr);
    QVERIFY(table->verticalHeader()->isHidden());
}

QTEST_MAIN(WifiManagerPanelTests)
#include "test_wifi_manager_panel.moc"
