// Copyright (c) 2025-2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file test_email_view_ids.cpp
/// @brief Unit tests for fail-closed decoding of email view-item ids

#include "sak/email_view_ids.h"

#include <QtTest/QtTest>
#include <QVariant>

#include <cstdint>

class TestEmailViewIds : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void decodesStoredIntegerIds();
    void zeroIsAnIdNotAnAbsence();
    void refusesUnsetAndNonIntegralRoles();
    void refusesNegativeIds();
};

void TestEmailViewIds::decodesStoredIntegerIds() {
    const auto node_id = sak::decodeEmailViewId(QVariant::fromValue<uint64_t>(2113));
    QVERIFY(node_id.has_value());
    QCOMPARE(*node_id, static_cast<uint64_t>(2113));

    const auto narrow = sak::decodeEmailViewId(QVariant::fromValue<uint32_t>(8));
    QVERIFY(narrow.has_value());
    QCOMPARE(*narrow, static_cast<uint64_t>(8));

    const auto signed_id = sak::decodeEmailViewId(QVariant::fromValue<int>(9));
    QVERIFY(signed_id.has_value());
    QCOMPARE(*signed_id, static_cast<uint64_t>(9));
}

void TestEmailViewIds::zeroIsAnIdNotAnAbsence() {
    // 0 is the MBOX root folder id and the message index of the first message in
    // an MBOX file, so it must decode as a value. Reporting absence as 0 is what
    // silently dropped the first message from every checkbox-driven export.
    const auto decoded = sak::decodeEmailViewId(QVariant::fromValue<uint64_t>(0));
    QVERIFY(decoded.has_value());
    QCOMPARE(*decoded, static_cast<uint64_t>(0));

    // The same must hold when the role was stored as a signed int, which is the
    // only path that reaches the sign guard at all: an off-by-one there (<= 0
    // instead of < 0) would turn the MBOX root folder and the first message back
    // into "this item cannot be acted on".
    const auto signed_zero = sak::decodeEmailViewId(QVariant::fromValue<int>(0));
    QVERIFY(signed_zero.has_value());
    QCOMPARE(*signed_zero, static_cast<uint64_t>(0));
}

void TestEmailViewIds::refusesUnsetAndNonIntegralRoles() {
    // An item that carries no id cannot be acted on. Refusing is the only safe
    // answer: a decoded 0, or a rounded double, would name some other folder or
    // message than the one the user pointed at.
    QVERIFY(!sak::decodeEmailViewId(QVariant()).has_value());
    QVERIFY(!sak::decodeEmailViewId(QVariant(QStringLiteral("17"))).has_value());
    QVERIFY(!sak::decodeEmailViewId(QVariant(QStringLiteral("not-an-id"))).has_value());
    QVERIFY(!sak::decodeEmailViewId(QVariant(1.5)).has_value());
    QVERIFY(!sak::decodeEmailViewId(QVariant(true)).has_value());
}

void TestEmailViewIds::refusesNegativeIds() {
    // Widening a negative value produces a huge unsigned id that names an
    // arbitrary node, so it is refused rather than converted.
    QVERIFY(!sak::decodeEmailViewId(QVariant::fromValue<int>(-1)).has_value());
    QVERIFY(!sak::decodeEmailViewId(QVariant::fromValue<qlonglong>(-9)).has_value());

    // The qlonglong refusal above must come from the sign guard, not from
    // LongLong having quietly dropped out of the accepted-type set: the same
    // type decodes to its exact value when the number is not negative.
    const auto long_positive = sak::decodeEmailViewId(QVariant::fromValue<qlonglong>(9));
    QVERIFY(long_positive.has_value());
    QCOMPARE(*long_positive, static_cast<uint64_t>(9));
}

QTEST_MAIN(TestEmailViewIds)
#include "test_email_view_ids.moc"
