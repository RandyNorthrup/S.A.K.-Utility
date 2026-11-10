// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file test_single_instance_guard.cpp
/// @brief One running S.A.K. Utility per data folder; an elevated relaunch waits its turn.

#include "sak/elevation_manager.h"
#include "sak/main_window.h"
#include "sak/single_instance_guard.h"

#include <QCoreApplication>
#include <QDir>
#include <QProcess>
#include <QScopeGuard>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest/QtTest>

#include <atomic>
#include <chrono>
#include <thread>

using sak::SingleInstanceGuard;
using namespace std::chrono_literals;

namespace {

constexpr auto kNoWait = 0ms;
constexpr auto kShortWait = 300ms;
constexpr auto kHandoffWait = 10s;
constexpr int kSignalTimeoutMs = 5000;
constexpr int kProcessTimeoutMs = 30'000;

/// A data folder of its own: the named objects a claim leaves held live as long as this test
/// process, so no two cases share a folder.
QString freshFolder(const QTemporaryDir& root, const QString& name) {
    const QString path = QDir(root.path()).filePath(name);
    if (!QDir().mkpath(path)) {
        return {};
    }
    return path;
}

}  // namespace

class SingleInstanceGuardTests : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void initTestCase();
    void nameStemIgnoresSpelling();
    void oneInstancePerDataFolder();
    void folderReachedThroughAJunctionIsTheSameInstance();
    void secondLaunchAsksTheRunningInstanceToComeForward();
    void elevatedRelaunchWaitsForItsLauncherToExit();
    void elevatedRelaunchCarriesTheHandoffArgument();
    void askedWindowIsRestoredAndBroughtForward();

private:
    QTemporaryDir m_root;
};

void SingleInstanceGuardTests::initTestCase() {
    QVERIFY(m_root.isValid());
}

void SingleInstanceGuardTests::nameStemIgnoresSpelling() {
    const QString stem =
        SingleInstanceGuard::objectNameStem(QStringLiteral("C:\\Tools\\SAK\\data"));
    QCOMPARE(SingleInstanceGuard::objectNameStem(QStringLiteral("c:/tools/sak/DATA/")), stem);
    QVERIFY(SingleInstanceGuard::objectNameStem(QStringLiteral("C:\\Tools\\SAK2\\data")) != stem);
    QVERIFY(stem.startsWith(QStringLiteral("SAKUtility-")));
}

// A second claim on one folder is the second launch; another folder is another portable copy,
// which runs side by side.
void SingleInstanceGuardTests::oneInstancePerDataFolder() {
    const QString first = freshFolder(m_root, QStringLiteral("copy-a"));
    const QString second = freshFolder(m_root, QStringLiteral("copy-b"));
    QVERIFY(!first.isEmpty() && !second.isEmpty());

    auto running = SingleInstanceGuard::claim(first, kNoWait);
    QVERIFY2(running.has_value(), qPrintable(running.error_or(QString())));
    QCOMPARE((*running)->role(), SingleInstanceGuard::Role::Primary);

    auto launch_again = SingleInstanceGuard::claim(first, kNoWait);
    QVERIFY(launch_again.has_value());
    QCOMPARE((*launch_again)->role(), SingleInstanceGuard::Role::Secondary);

    auto other_copy = SingleInstanceGuard::claim(second, kNoWait);
    QVERIFY(other_copy.has_value());
    QCOMPARE((*other_copy)->role(), SingleInstanceGuard::Role::Primary);
}

// The folder's final path names the instance, so a junction to it is not a way round the guard.
void SingleInstanceGuardTests::folderReachedThroughAJunctionIsTheSameInstance() {
    const QString target = freshFolder(m_root, QStringLiteral("copy-c"));
    QVERIFY(!target.isEmpty());
    const QString junction = QDir(m_root.path()).filePath(QStringLiteral("copy-c-junction"));
    QProcess mklink;
    mklink.start(QStringLiteral("cmd.exe"),
                 {QStringLiteral("/d"),
                  QStringLiteral("/c"),
                  QStringLiteral("mklink"),
                  QStringLiteral("/J"),
                  QDir::toNativeSeparators(junction),
                  QDir::toNativeSeparators(target)});
    QVERIFY(mklink.waitForFinished(kProcessTimeoutMs));
    QCOMPARE(mklink.exitCode(), 0);

    auto running = SingleInstanceGuard::claim(target, kNoWait);
    QVERIFY(running.has_value());
    QCOMPARE((*running)->role(), SingleInstanceGuard::Role::Primary);
    auto through_junction = SingleInstanceGuard::claim(junction, kNoWait);
    QVERIFY(through_junction.has_value());
    QCOMPARE((*through_junction)->role(), SingleInstanceGuard::Role::Secondary);
}

void SingleInstanceGuardTests::secondLaunchAsksTheRunningInstanceToComeForward() {
    const QString folder = freshFolder(m_root, QStringLiteral("copy-d"));
    QVERIFY(!folder.isEmpty());
    auto running = SingleInstanceGuard::claim(folder, kNoWait);
    QVERIFY(running.has_value());
    (*running)->listenForActivation();
    QSignalSpy asked(running->get(), &SingleInstanceGuard::activationRequested);

    auto launch_again = SingleInstanceGuard::claim(folder, kNoWait);
    QVERIFY(launch_again.has_value());
    QCOMPARE((*launch_again)->role(), SingleInstanceGuard::Role::Secondary);
    const auto activated = (*launch_again)->activateRunningInstance();
    QVERIFY2(activated.has_value(), qPrintable(activated.error_or(QString())));
    QTRY_COMPARE_WITH_TIMEOUT(asked.count(), 1, kSignalTimeoutMs);
}

// Run as Admin starts the elevated copy and then closes this one. The elevated copy must not
// bring the closing one forward and quit; it waits for it to end and then becomes the instance.
// The launcher here is a thread that holds its claim until told to finish; a mutex whose owner
// ends is handed to the waiter, as when the launching process exits.
void SingleInstanceGuardTests::elevatedRelaunchWaitsForItsLauncherToExit() {
    const QString folder = freshFolder(m_root, QStringLiteral("copy-e"));
    QVERIFY(!folder.isEmpty());
    std::atomic<bool> claimed{false};
    std::atomic<bool> finish{false};
    std::thread launcher([&folder, &claimed, &finish]() {
        auto running = SingleInstanceGuard::claim(folder, kNoWait);
        claimed = running.has_value() && (*running)->role() == SingleInstanceGuard::Role::Primary;
        while (!finish) {
            std::this_thread::sleep_for(10ms);
        }
    });
    // A failed check returns early; the launcher still has to be told to finish and joined.
    const auto end_launcher = qScopeGuard([&finish, &launcher]() {
        finish = true;
        if (launcher.joinable()) {
            launcher.join();
        }
    });
    QTRY_VERIFY_WITH_TIMEOUT(claimed.load(), kSignalTimeoutMs);

    const auto too_soon = SingleInstanceGuard::claim(folder, kShortWait);
    QVERIFY2(!too_soon.has_value(), "the relaunch must not start while its launcher runs");
    QVERIFY(too_soon.error().contains(QStringLiteral("did not close")));

    std::thread release([&finish]() {
        std::this_thread::sleep_for(kShortWait);
        finish = true;
    });
    const auto handed_over = SingleInstanceGuard::claim(folder, kHandoffWait);
    release.join();
    launcher.join();
    QVERIFY2(handed_over.has_value(), qPrintable(handed_over.error_or(QString())));
    QCOMPARE((*handed_over)->role(), SingleInstanceGuard::Role::Primary);
}

// The relaunch is told it is one, keeping the arguments it was started with.
void SingleInstanceGuardTests::elevatedRelaunchCarriesTheHandoffArgument() {
    QCOMPARE(QString::fromStdWString(sak::ElevationManager::relaunchArguments(L"")),
             QStringLiteral("--elevated-relaunch"));
    QCOMPARE(QString::fromStdWString(sak::ElevationManager::relaunchArguments(L"--no-splash")),
             QStringLiteral("--no-splash --elevated-relaunch"));
    QCOMPARE(QString::fromLatin1(sak::ElevationManager::kElevatedRelaunchArgument),
             QStringLiteral("--elevated-relaunch"));
}

// What the running instance does when a second launch asks: a minimized window is restored and
// brought forward; a window that was never shown (a headless smoke run) stays hidden.
void SingleInstanceGuardTests::askedWindowIsRestoredAndBroughtForward() {
    sak::MainWindow never_shown;
    never_shown.bringToFront();
    QVERIFY(!never_shown.isVisible());

    sak::MainWindow window;
    window.showMinimized();
    QTRY_VERIFY(window.isMinimized());
    window.bringToFront();
    QTRY_VERIFY(!window.isMinimized());
    QVERIFY(window.isVisible());
}

QTEST_MAIN(SingleInstanceGuardTests)
#include "test_single_instance_guard.moc"
