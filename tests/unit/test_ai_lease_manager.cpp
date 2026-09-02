// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/ai/ai_lease_manager.h"

#include <QRegularExpression>
#include <QtTest/QtTest>

#include <algorithm>
#include <atomic>
#include <barrier>
#include <thread>
#include <type_traits>
#include <vector>

namespace {

template <typename Manager>
concept HasTtlReclaimApi = requires {
    &Manager::leaseTtlSeconds;
    &Manager::reclaimExpired;
};

}  // namespace

class AiLeaseManagerTests : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void acquireBlocksUntilExactRelease();
    void leaseIdsCarryUnpredictableBearerTokens();
    void refusalAndLabelsDoNotDiscloseBearerToken();
    void separateManagersShareOneProcessLease();
    void concurrentAcquireHasOneWinner();
    void timeBasedReclamationApiIsAbsent();
};

void AiLeaseManagerTests::acquireBlocksUntilExactRelease() {
    sak::ai::AiLeaseManager manager;
    const auto first = manager.acquire(QStringLiteral("overseer"));
    QVERIFY(first.granted);
    QVERIFY(!first.lease.lease_id.isEmpty());
    QCOMPARE(first.lease.agent_id, QStringLiteral("overseer"));

    const auto blocked = manager.acquire(QStringLiteral("subagent"));
    QVERIFY(!blocked.granted);
    QVERIFY(blocked.lease.lease_id.isEmpty());
    QVERIFY(blocked.reason.contains(QStringLiteral("overseer")));
    QVERIFY(!manager.release(QStringLiteral("wrong-id")));
    QVERIFY(!manager.release(QString()));
    QVERIFY(manager.release(first.lease.lease_id));
    QVERIFY(!manager.release(first.lease.lease_id));
    const auto next = manager.acquire(QStringLiteral("subagent"));
    QVERIFY(next.granted);
    QVERIFY(manager.release(next.lease.lease_id));
}

void AiLeaseManagerTests::leaseIdsCarryUnpredictableBearerTokens() {
    sak::ai::AiLeaseManager manager;
    const auto first = manager.acquire(QStringLiteral("first"));
    QVERIFY(first.granted);
    QVERIFY(manager.release(first.lease.lease_id));
    const auto second = manager.acquire(QStringLiteral("second"));
    QVERIFY(second.granted);

    const QRegularExpression shape(QStringLiteral("^lease_[0-9]{4}_[0-9a-f]{16}$"));
    QVERIFY(shape.match(first.lease.lease_id).hasMatch());
    QVERIFY(shape.match(second.lease.lease_id).hasMatch());
    QVERIFY(first.lease.lease_id != second.lease.lease_id);
    QVERIFY(first.lease.lease_id.section(QLatin1Char('_'), 2) !=
            second.lease.lease_id.section(QLatin1Char('_'), 2));
    QVERIFY(manager.release(second.lease.lease_id));
}

void AiLeaseManagerTests::refusalAndLabelsDoNotDiscloseBearerToken() {
    sak::ai::AiLeaseManager manager;
    const auto held = manager.acquire(QStringLiteral("owner"));
    QVERIFY(held.granted);
    const QString label = sak::ai::AiLeaseManager::publicLabel(held.lease.lease_id);
    QVERIFY(QRegularExpression(QStringLiteral("^lease_[0-9]{4}$")).match(label).hasMatch());

    const auto blocked = manager.acquire(QStringLiteral("other"));
    QVERIFY(blocked.reason.contains(label));
    QVERIFY(!blocked.reason.contains(held.lease.lease_id));
    QCOMPARE(sak::ai::AiLeaseManager::publicLabel(QStringLiteral("odd")), QStringLiteral("odd"));
    QCOMPARE(sak::ai::AiLeaseManager::publicLabel(QString()), QString());
    QVERIFY(manager.release(held.lease.lease_id));
}

void AiLeaseManagerTests::separateManagersShareOneProcessLease() {
    sak::ai::AiLeaseManager first_manager;
    sak::ai::AiLeaseManager second_manager;
    const auto held = first_manager.acquire(QStringLiteral("first_manager"));
    QVERIFY(held.granted);
    const auto blocked = second_manager.acquire(QStringLiteral("second_manager"));
    QVERIFY(!blocked.granted);
    QVERIFY(blocked.reason.contains(QStringLiteral("first_manager")));
    QVERIFY(second_manager.release(held.lease.lease_id));
    const auto next = second_manager.acquire(QStringLiteral("second_manager"));
    QVERIFY(next.granted);
    QVERIFY(first_manager.release(next.lease.lease_id));
}

void AiLeaseManagerTests::concurrentAcquireHasOneWinner() {
    constexpr int kContenders = 16;
    sak::ai::AiLeaseManager manager;
    std::barrier start_line(kContenders);
    std::atomic_int grants{0};
    std::vector<QString> lease_ids(static_cast<std::size_t>(kContenders));
    std::vector<std::thread> threads;
    threads.reserve(kContenders);

    for (int index = 0; index < kContenders; ++index) {
        threads.emplace_back([&, index]() {
            start_line.arrive_and_wait();
            const auto acquired = manager.acquire(QStringLiteral("agent_%1").arg(index));
            if (acquired.granted) {
                ++grants;
                lease_ids[static_cast<std::size_t>(index)] = acquired.lease.lease_id;
            }
        });
    }
    for (auto& thread : threads) {
        thread.join();
    }
    QCOMPARE(grants.load(), 1);
    const auto winner = std::ranges::find_if(lease_ids,
                                             [](const QString& id) { return !id.isEmpty(); });
    QVERIFY(winner != lease_ids.cend());
    QVERIFY(manager.release(*winner));
}

void AiLeaseManagerTests::timeBasedReclamationApiIsAbsent() {
    using Manager = sak::ai::AiLeaseManager;
    QVERIFY(!HasTtlReclaimApi<Manager>);
    QVERIFY((!std::is_constructible_v<Manager, qint64>));
}

QTEST_GUILESS_MAIN(AiLeaseManagerTests)
#include "test_ai_lease_manager.moc"
