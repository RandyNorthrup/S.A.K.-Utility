// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/app_action_registry.h"

#include <QJsonObject>
#include <QtTest/QtTest>

using sak::AppActionDescriptor;
using sak::AppActionRegistry;
using sak::AppActionResult;

namespace {

AppActionDescriptor readOnlyDescriptor(const QString& id) {
    AppActionDescriptor d;
    d.id = id;
    d.title = QStringLiteral("Title %1").arg(id);
    d.description = QStringLiteral("Does %1").arg(id);
    d.category = QStringLiteral("diagnostics");
    d.read_only = true;
    return d;
}

}  // namespace

class AppActionRegistryTests : public QObject {
    Q_OBJECT

private slots:
    void registerAndInvoke_roundTripsResult();
    void registerRejectsEmptyId();
    void registerRejectsNullInvoke();
    void registerRejectsDuplicateId();
    void invokeUnknownId_returnsFailureAndError();
    void listAndCatalog_areSortedAndCarryRiskFlags();
    void invokeDoesNotHoldLock_soThunkMayReenterRegistry();
};

void AppActionRegistryTests::registerAndInvoke_roundTripsResult() {
    AppActionRegistry registry;
    QJsonObject seen_args;
    QVERIFY(registry.registerAction(
        readOnlyDescriptor(QStringLiteral("diag.scan")), [&](const QJsonObject& args) {
            seen_args = args;
            return AppActionResult{true,
                                   QStringLiteral("done"),
                                   QJsonObject{{QStringLiteral("count"), 3}}};
        }));
    QVERIFY(registry.contains(QStringLiteral("diag.scan")));
    QCOMPARE(registry.count(), 1);

    QString error;
    const AppActionResult result = registry.invoke(QStringLiteral("diag.scan"),
                                                   QJsonObject{{QStringLiteral("depth"), 2}},
                                                   &error);
    QVERIFY(error.isEmpty());
    QVERIFY(result.success);
    QCOMPARE(result.message, QStringLiteral("done"));
    QCOMPARE(result.data.value(QStringLiteral("count")).toInt(), 3);
    QCOMPARE(seen_args.value(QStringLiteral("depth")).toInt(), 2);
}

void AppActionRegistryTests::registerRejectsEmptyId() {
    AppActionRegistry registry;
    QString error;
    QVERIFY(!registry.registerAction(
        readOnlyDescriptor(QStringLiteral("   ")),
        [](const QJsonObject&) { return AppActionResult{}; },
        &error));
    QCOMPARE(error, QStringLiteral("App action id is empty"));
    QCOMPARE(registry.count(), 0);
}

void AppActionRegistryTests::registerRejectsNullInvoke() {
    AppActionRegistry registry;
    QString error;
    QVERIFY(!registry.registerAction(
        readOnlyDescriptor(QStringLiteral("diag.scan")), sak::AppActionInvoke{}, &error));
    QCOMPARE(error, QStringLiteral("App action 'diag.scan' has no invoke handler"));
    QCOMPARE(registry.count(), 0);
}

void AppActionRegistryTests::registerRejectsDuplicateId() {
    AppActionRegistry registry;
    auto thunk = [](const QJsonObject&) {
        return AppActionResult{true, {}, {}};
    };
    QVERIFY(registry.registerAction(readOnlyDescriptor(QStringLiteral("diag.scan")), thunk));
    QString error;
    QVERIFY(
        !registry.registerAction(readOnlyDescriptor(QStringLiteral("diag.scan")), thunk, &error));
    QCOMPARE(error, QStringLiteral("App action 'diag.scan' is already registered"));
    QCOMPARE(registry.count(), 1);
}

void AppActionRegistryTests::invokeUnknownId_returnsFailureAndError() {
    AppActionRegistry registry;
    QString error;
    const AppActionResult result = registry.invoke(QStringLiteral("nope.missing"), {}, &error);
    QVERIFY(!result.success);
    QCOMPARE(error, QStringLiteral("Unknown app action: nope.missing"));
}

void AppActionRegistryTests::listAndCatalog_areSortedAndCarryRiskFlags() {
    AppActionRegistry registry;
    auto thunk = [](const QJsonObject&) {
        return AppActionResult{true, {}, {}};
    };
    registry.registerAction(readOnlyDescriptor(QStringLiteral("zeta.op")), thunk);
    AppActionDescriptor destructive = readOnlyDescriptor(QStringLiteral("alpha.wipe"));
    destructive.read_only = false;
    destructive.mutating = true;
    destructive.destructive = true;
    destructive.requires_admin = true;
    registry.registerAction(destructive, thunk);

    const QVector<AppActionDescriptor> listed = registry.list();
    QCOMPARE(listed.size(), 2);
    // QMap keeps keys sorted: alpha.wipe before zeta.op.
    QCOMPARE(listed.at(0).id, QStringLiteral("alpha.wipe"));
    QCOMPARE(listed.at(1).id, QStringLiteral("zeta.op"));

    const QJsonArray catalog = registry.toJsonCatalog();
    QCOMPARE(catalog.size(), 2);
    const QJsonObject wipe = catalog.at(0).toObject();
    QCOMPARE(wipe.value(QStringLiteral("id")).toString(), QStringLiteral("alpha.wipe"));
    QVERIFY(wipe.value(QStringLiteral("destructive")).toBool());
    QVERIFY(wipe.value(QStringLiteral("requires_admin")).toBool());
    QVERIFY(!wipe.value(QStringLiteral("read_only")).toBool());
}

void AppActionRegistryTests::invokeDoesNotHoldLock_soThunkMayReenterRegistry() {
    AppActionRegistry registry;
    // If invoke() held the read lock while calling the thunk, this re-entrant read
    // (contains) would be fine for a recursive lock but a QReadWriteLock read taken
    // twice on one thread is allowed; the real risk is a write during a held read.
    // Prove the thunk can freely read the registry mid-invoke.
    registry.registerAction(readOnlyDescriptor(QStringLiteral("outer")), [&](const QJsonObject&) {
        const bool sees_self = registry.contains(QStringLiteral("outer"));
        return AppActionResult{sees_self, QStringLiteral("reentrant"), {}};
    });
    const AppActionResult result = registry.invoke(QStringLiteral("outer"), {});
    QVERIFY(result.success);
    QCOMPARE(result.message, QStringLiteral("reentrant"));
}

QTEST_MAIN(AppActionRegistryTests)
#include "test_app_action_registry.moc"
