// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file test_app_installation_panel.cpp
/// @brief App Installation is built the same way in every run mode.

#include "sak/app_installation_panel.h"

#include <QAbstractButton>
#include <QApplication>
#include <QMessageBox>
#include <QPushButton>
#include <QTimer>
#include <QtTest/QtTest>

namespace {

constexpr int kDialogPollMs = 20;
constexpr int kDialogTimeoutMs = 5000;

/// Records and dismisses the next message box that opens. Polls, because the box runs its own
/// nested event loop inside the click that opened it.
class MessageBoxRecorder : public QObject {
public:
    MessageBoxRecorder() {
        m_timer.setInterval(kDialogPollMs);
        connect(&m_timer, &QTimer::timeout, this, [this]() { closeIfOpen(); });
        m_timer.start();
        m_deadline.start();
    }
    QString text;
    bool seen{false};

private:
    void closeIfOpen() {
        if (m_deadline.elapsed() > kDialogTimeoutMs) {
            m_timer.stop();
            return;
        }
        auto* box = qobject_cast<QMessageBox*>(QApplication::activeModalWidget());
        if (box == nullptr) {
            return;
        }
        text = box->text();
        seen = true;
        m_timer.stop();
        box->done(QMessageBox::Ok);
    }
    QTimer m_timer;
    QElapsedTimer m_deadline;
};

// The online and offline tabs each have a "Search Packages" button; the tooltip tells them apart.
QPushButton* onlineSearchButton(const QWidget& root) {
    const auto buttons = root.findChildren<QPushButton*>();
    for (QPushButton* button : buttons) {
        if (button->accessibleName() == QStringLiteral("Search Packages") &&
            button->toolTip() == QStringLiteral("Search the Chocolatey repository")) {
            return button;
        }
    }
    return nullptr;
}

}  // namespace

class AppInstallationPanelTests : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void cleanup();
    void startupWorkRunsInEveryMode_data();
    void startupWorkRunsInEveryMode();
};

void AppInstallationPanelTests::cleanup() {
    qApp->setProperty("sakStartupSmokeTest", QVariant());
    qApp->setProperty("sakAccessibilityAudit", QVariant());
}

void AppInstallationPanelTests::startupWorkRunsInEveryMode_data() {
    QTest::addColumn<QByteArray>("mode");
    QTest::newRow("interactive") << QByteArray();
    QTest::newRow("startup smoke") << QByteArray("sakStartupSmokeTest");
    QTest::newRow("accessibility audit") << QByteArray("sakAccessibilityAudit");
}

// The accessibility audit used to stop the panel's constructor after its widgets were built:
// no signal connections and no Chocolatey initialization, so the gate that exists to look at
// the startup a user gets looked at a panel whose buttons did nothing. A Search click with an
// empty query proves both now run in every mode: the click reaches onSearch only through
// setupConnections, and onSearch asks for a search term only once the bundled choco.exe
// (staged beside every test executable) has been initialized; without it the answer is
// "Chocolatey is not initialized".
void AppInstallationPanelTests::startupWorkRunsInEveryMode() {
    QFETCH(QByteArray, mode);
    if (!mode.isEmpty()) {
        qApp->setProperty(mode.constData(), true);
    }
    sak::AppInstallationPanel panel;
    QPushButton* search = onlineSearchButton(panel);
    QVERIFY(search != nullptr);

    MessageBoxRecorder recorder;
    search->click();
    QTRY_VERIFY_WITH_TIMEOUT(recorder.seen, kDialogTimeoutMs);
    QCOMPARE(recorder.text, QStringLiteral("Please enter a search term."));
}

QTEST_MAIN(AppInstallationPanelTests)
#include "test_app_installation_panel.moc"
