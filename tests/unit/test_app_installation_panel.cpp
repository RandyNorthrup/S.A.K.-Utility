// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file test_app_installation_panel.cpp
/// @brief App Installation is built the same way in every run mode.

#include "sak/app_installation_panel.h"
#include "sak/offline_deployment_worker.h"

#include <QAbstractButton>
#include <QApplication>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QScrollArea>
#include <QSignalSpy>
#include <QTabWidget>
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
    void offlineBundleNamesWhatFailedAndEndsTheProgress();
    void offlineFailureTextNeverWidensThePage();
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

// "Complete: 9 succeeded, 1 failed" without saying which package or why, and the status-bar
// progress left at 80%, because it follows successes and the run had a failure.
void AppInstallationPanelTests::offlineBundleNamesWhatFailedAndEndsTheProgress() {
    sak::AppInstallationPanel panel;
    QSignalSpy progress(&panel, &sak::AppInstallationPanel::progressUpdated);
    Q_EMIT panel.m_offline_worker->operationStarted(10);
    Q_EMIT panel.m_offline_worker->batchProgress(8, 10, QStringLiteral("vlc"));
    Q_EMIT panel.m_offline_worker->packageProgress(QStringLiteral("adobereader"),
                                                   false,
                                                   QStringLiteral("Download failed: HTTP 404"));
    sak::BatchStats stats;
    stats.total = 10;
    stats.completed = 9;
    stats.failed = 1;
    Q_EMIT panel.m_offline_worker->operationCompleted(stats);

    const QString status = panel.m_offlineStatusLabel->text();
    QVERIFY2(status.startsWith(QStringLiteral("Incomplete: 9 succeeded, 1 failed")),
             qPrintable(status));
    QVERIFY2(status.contains(QStringLiteral("adobereader (Download failed: HTTP 404)")),
             qPrintable(status));
    QVERIFY(!progress.isEmpty());
    const auto last = progress.last();
    QCOMPARE(last.at(0).toInt(), last.at(1).toInt());
}

// Review 3 (RG1): the failure named in the status label carried a download URL, and unwrapped at
// its own width it made Offline Deploy about 2200 px wide, with its buttons off screen.
void AppInstallationPanelTests::offlineFailureTextNeverWidensThePage() {
    sak::AppInstallationPanel panel;
    panel.resize(1280, 800);
    panel.show();
    QVERIFY(QTest::qWaitForWindowExposed(&panel));
    for (int index = 0; index < panel.m_tabWidget->count(); ++index) {
        if (panel.m_tabWidget->tabText(index) == QStringLiteral("Offline Deploy")) {
            panel.m_tabWidget->setCurrentIndex(index);
        }
    }
    QCOMPARE(panel.m_tabWidget->tabText(panel.m_tabWidget->currentIndex()),
             QStringLiteral("Offline Deploy"));
    // The page as a scroll area holds it: its minimum width is what the horizontal bar shows.
    QWidget* page = panel.m_offlineStatusLabel;
    while (page->parentWidget() != nullptr &&
           qobject_cast<QScrollArea*>(page->parentWidget()->parentWidget()) == nullptr) {
        page = page->parentWidget();
    }
    const int before = page->minimumSizeHint().width();

    Q_EMIT panel.m_offline_worker->operationStarted(2);
    Q_EMIT panel.m_offline_worker->packageProgress(
        QStringLiteral("adobereader"),
        false,
        QStringLiteral("Not bundled: the installer at "
                       "https://ardownload3.adobe.com/pub/adobe/acrobat/win/AcrobatDC/2500120756/"
                       "AcroRdrDCx642500120756_MUI.exe does not match the SHA-256 checksum the "
                       "adobereader package declares, so it was left out of the bundle"));
    sak::BatchStats stats;
    stats.total = 2;
    stats.completed = 1;
    stats.failed = 1;
    Q_EMIT panel.m_offline_worker->operationCompleted(stats);
    QCoreApplication::processEvents();

    QVERIFY(panel.m_offlineStatusLabel->isVisible());
    QVERIFY(panel.m_offlineStatusLabel->toolTip().contains(QStringLiteral("AcroRdrDCx64")));
    const int after = page->minimumSizeHint().width();
    QVERIFY2(after <= before, qPrintable(QStringLiteral("%1 -> %2 px").arg(before).arg(after)));
}

QTEST_MAIN(AppInstallationPanelTests)
#include "test_app_installation_panel.moc"
