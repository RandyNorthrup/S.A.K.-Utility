// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

// Unit tests for sak::CrashReporter's pure helpers (R5-G23-2). The minidump
// write itself needs a real SEH fault and is exercised by manual/soak testing;
// these cover the deterministic parts: the artifact naming, the exception-code
// name map, and the summary formatting.

#include "sak/crash_reporter.h"

#include <QtTest/QtTest>

class CrashReporterTests : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void crashFileStemIsDeterministic();
    void crashFileStemZeroPadsFields();
    void exceptionCodeNameMapsKnownCodes();
    void exceptionCodeNameUnknownIsUnknown();
    void summaryContainsAllFields();
    void summaryUsesCrlfLineEndings();
};

void CrashReporterTests::crashFileStemIsDeterministic() {
    const std::wstring stem = sak::CrashReporter::crashFileStem(
        1234, {.year = 2026, .month = 8, .day = 12, .hour = 17, .minute = 5, .second = 9});
    QCOMPARE(QString::fromStdWString(stem), QStringLiteral("sak_crash_1234_20260812_170509"));
}

void CrashReporterTests::crashFileStemZeroPadsFields() {
    // Single-digit month/day/time components must zero-pad so names sort
    // lexicographically by time.
    const std::wstring stem = sak::CrashReporter::crashFileStem(
        7, {.year = 2026, .month = 1, .day = 2, .hour = 3, .minute = 4, .second = 5});
    QCOMPARE(QString::fromStdWString(stem), QStringLiteral("sak_crash_7_20260102_030405"));
}

void CrashReporterTests::exceptionCodeNameMapsKnownCodes() {
    QCOMPARE(QString::fromLatin1(sak::CrashReporter::exceptionCodeName(0xC0'00'00'05)),
             QStringLiteral("ACCESS_VIOLATION"));
    QCOMPARE(QString::fromLatin1(sak::CrashReporter::exceptionCodeName(0xC0'00'00'FD)),
             QStringLiteral("STACK_OVERFLOW"));
    QCOMPARE(QString::fromLatin1(sak::CrashReporter::exceptionCodeName(0xC0'00'00'94)),
             QStringLiteral("INTEGER_DIVIDE_BY_ZERO"));
    QCOMPARE(QString::fromLatin1(sak::CrashReporter::exceptionCodeName(0xE0'6D'73'63)),
             QStringLiteral("CPP_EXCEPTION"));
}

void CrashReporterTests::exceptionCodeNameUnknownIsUnknown() {
    QCOMPARE(QString::fromLatin1(sak::CrashReporter::exceptionCodeName(0x12'34'56'78)),
             QStringLiteral("UNKNOWN"));
}

void CrashReporterTests::summaryContainsAllFields() {
    const void* addr = reinterpret_cast<const void*>(static_cast<std::uintptr_t>(0xDE'AD'BE'EF));
    const std::string summary =
        sak::CrashReporter::formatSummary(0xC0'00'00'05, addr, 4321, 99, "2026-08-12T17:05:09");
    const QString text = QString::fromStdString(summary);
    // The whole summary is deterministic; pin it byte-exact (subsumes the six field probes and
    // catches a dropped header, a reordered field, or a lost 0x-prefix / zero-padding).
    QCOMPARE(text,
             QStringLiteral("SAK-Utility crash report\r\n"
                            "time: 2026-08-12T17:05:09\r\n"
                            "process id: 4321\r\n"
                            "thread id: 99\r\n"
                            "exception code: 0xC0000005 (ACCESS_VIOLATION)\r\n"
                            "fault address: 0x00000000DEADBEEF\r\n"));
}

void CrashReporterTests::summaryUsesCrlfLineEndings() {
    // Written verbatim to a .txt on Windows, so use CRLF for Notepad readability.
    const std::string summary =
        sak::CrashReporter::formatSummary(0, nullptr, 1, 1, "2026-01-01T00:00:00");
    // Byte-exact pin still proves CRLF at every line boundary, and additionally pins the
    // zero-code UNKNOWN name and the 16-wide null fault address.
    QCOMPARE(QString::fromStdString(summary),
             QStringLiteral("SAK-Utility crash report\r\n"
                            "time: 2026-01-01T00:00:00\r\n"
                            "process id: 1\r\n"
                            "thread id: 1\r\n"
                            "exception code: 0x00000000 (UNKNOWN)\r\n"
                            "fault address: 0x0000000000000000\r\n"));
}

QTEST_APPLESS_MAIN(CrashReporterTests)
#include "test_crash_reporter.moc"
