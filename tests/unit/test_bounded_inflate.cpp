// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file test_bounded_inflate.cpp
/// @brief inflateZlibExactly: a zlib stream decodes to its declared size and never past it.

#include "sak/bounded_inflate.h"

#include "../support/large_allocation_counter.h"

#include <QtTest/QtTest>

namespace {

// qCompress puts the uncompressed size in front of the zlib stream as 4 big-endian bytes.
constexpr qsizetype kQtCompressSizePrefixBytes = 4;
// Deflated, this many zero bytes are about 64 KiB: a "bomb" a crafted image can carry.
constexpr qsizetype kBombInflatedBytes = 64LL * 1024 * 1024;
// What the bomb's container declares it inflates to.
constexpr qsizetype kBombDeclaredBytes = 4096;

QByteArray zlibStream(const QByteArray& data) {
    return qCompress(data, 9).mid(kQtCompressSizePrefixBytes);
}

QByteArray sampleText() {
    QByteArray text;
    for (int line = 0; line < 200; ++line) {
        text.append(QByteArrayLiteral("bounded inflate line ") + QByteArray::number(line) + '\n');
    }
    return text;
}

}  // namespace

class TestBoundedInflate : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void decodesTheDeclaredSizeExactly();
    void refusesAStreamLongerOrShorterThanDeclared();
    void stopsABombAtItsDeclaredSize();
    void ignoresBytesAfterTheStreamEnds();
    void refusesInputThatIsNotAZlibStream();
};

void TestBoundedInflate::decodesTheDeclaredSizeExactly() {
    const QByteArray text = sampleText();
    const auto decoded = sak::inflateZlibExactly(zlibStream(text), text.size());
    QVERIFY(decoded.has_value());
    QCOMPARE(*decoded, text);

    // The smallest content, so the spare byte is the whole margin.
    const auto one = sak::inflateZlibExactly(zlibStream(QByteArrayLiteral("x")), 1);
    QVERIFY(one.has_value());
    QCOMPARE(*one, QByteArrayLiteral("x"));
}

void TestBoundedInflate::refusesAStreamLongerOrShorterThanDeclared() {
    const QByteArray text = sampleText();
    const QByteArray stream = zlibStream(text);
    // One byte more than declared: the stream does not end where its container says it does.
    QVERIFY(!sak::inflateZlibExactly(stream, text.size() - 1).has_value());
    // One byte fewer: it ends short.
    QVERIFY(!sak::inflateZlibExactly(stream, text.size() + 1).has_value());
    // A stream cut off before its end is short too.
    QVERIFY(!sak::inflateZlibExactly(stream.left(stream.size() / 2), text.size()).has_value());
}

// The point of the helper. qUncompress grew its buffer to whatever the stream held, so 64 KiB of
// crafted input declaring 4 KiB made it allocate its way up to 64 MiB before any caller could
// compare sizes. Decoding now never holds more than the declared size and one byte.
void TestBoundedInflate::stopsABombAtItsDeclaredSize() {
    const QByteArray bomb = zlibStream(QByteArray(kBombInflatedBytes, '\0'));
    QVERIFY(bomb.size() < kBombInflatedBytes / 512);  // it really is a bomb
    int large_allocations = 0;
    std::optional<QByteArray> decoded;
    {
        const sak::test::LargeAllocationCounter counter;
        decoded = sak::inflateZlibExactly(bomb, kBombDeclaredBytes);
        large_allocations = sak::test::LargeAllocationCounter::count();
    }
    QVERIFY(!decoded.has_value());
    QVERIFY2(large_allocations == 0,
             qPrintable(QStringLiteral("%1 allocations of 1 MiB or more while decoding a bomb "
                                       "that declares %2 bytes")
                            .arg(large_allocations)
                            .arg(kBombDeclaredBytes)));
}

void TestBoundedInflate::ignoresBytesAfterTheStreamEnds() {
    // qUncompress ignores what follows the end of the stream, and on-disk chunks are padded.
    const QByteArray text = sampleText();
    const auto decoded = sak::inflateZlibExactly(zlibStream(text) + QByteArrayLiteral("padding"),
                                                 text.size());
    QVERIFY(decoded.has_value());
    QCOMPARE(*decoded, text);
}

void TestBoundedInflate::refusesInputThatIsNotAZlibStream() {
    QVERIFY(!sak::inflateZlibExactly(QByteArray(), 0).has_value());
    QVERIFY(
        !sak::inflateZlibExactly(QByteArrayLiteral("not a zlib stream at all"), 24).has_value());
    QVERIFY(!sak::inflateZlibExactly(zlibStream(sampleText()), -1).has_value());
}

QTEST_GUILESS_MAIN(TestBoundedInflate)
#include "test_bounded_inflate.moc"
