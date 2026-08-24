// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file test_fuzz_command_classifier.cpp
/// @brief Property-fuzz of the AI command risk classifier (G14-18).
///
/// A real shipped bug: "fo^rmat C:" bypassed all three risk classifiers because a
/// single cmd.exe caret split the keyword and every whole-word regex missed it, so a
/// disk format ran with no lease, no restore point, and no human confirmation. The
/// static tools could not have caught it; only a property test over generated
/// obfuscations does. This is that test.
///
/// The property: a command that IS catastrophic stays catastrophic under every
/// shell-transparent obfuscation the interpreter strips before executing --
/// intra-word cmd carets (fo^rmat), intra-word PowerShell backticks (for`mat),
/// executable suffixes (format.com), and case changes (FoRmAt). Each transform leaves
/// a command that executes identically, so classify() MUST be invariant under it. The
/// mutually reinforcing property runs the same transforms over benign read-only
/// commands and asserts they are NEVER promoted to catastrophic, so a future tightening
/// of the escape-stripping cannot start flagging safe diagnostics.
///
/// The generator reuses the deterministic PRNG from the fuzz core, so a violating
/// command is reproducible and printed verbatim. The obfuscation operators are
/// deliberately confined to the intra-word escape shape the classifier is designed to
/// strip (a caret between two word characters), which is exactly the shipped-bug shape;
/// a non-intra-word caret is a different, intentionally-untouched case and is out of
/// scope here.

#include "sak/ai/ai_tool_policy.h"

#include "../fuzz/fuzz_harness.h"

#include <QChar>
#include <QString>
#include <QStringList>
#include <QtTest/QtTest>

#include <functional>
#include <vector>

namespace {

bool isWordChar(QChar c) {
    return (c >= QLatin1Char('a') && c <= QLatin1Char('z')) ||
           (c >= QLatin1Char('A') && c <= QLatin1Char('Z')) ||
           (c >= QLatin1Char('0') && c <= QLatin1Char('9')) || c == QLatin1Char('_');
}

// Insert @p escape between two adjacent word characters -- the exact intra-word split
// the classifier strips. Returns false when the string has no such position.
bool insertIntraWordEscape(QString& command, sak::fuzz::Prng& prng, QChar escape) {
    std::vector<int> positions;
    for (int i = 1; i < command.size(); ++i) {
        if (isWordChar(command.at(i - 1)) && isWordChar(command.at(i))) {
            positions.push_back(i);
        }
    }
    if (positions.empty()) {
        return false;
    }
    command.insert(positions[prng.below(static_cast<uint32_t>(positions.size()))], escape);
    return true;
}

// Flip the case of one random ASCII letter; the risk regexes are case-insensitive.
void flipOneLetterCase(QString& command, sak::fuzz::Prng& prng) {
    std::vector<int> letters;
    for (int i = 0; i < command.size(); ++i) {
        if (command.at(i).isLetter()) {
            letters.push_back(i);
        }
    }
    if (letters.empty()) {
        return;
    }
    const int index = letters[prng.below(static_cast<uint32_t>(letters.size()))];
    const QChar c = command.at(index);
    command[index] = c.isUpper() ? c.toLower() : c.toUpper();
}

// Append an executable suffix to the first whitespace-delimited token; the classifier
// strips these before matching so "format.com D:" is caught like "format D:".
void appendExeSuffix(QString& command, sak::fuzz::Prng& prng) {
    static const QStringList suffixes = {QStringLiteral(".exe"),
                                         QStringLiteral(".com"),
                                         QStringLiteral(".cmd"),
                                         QStringLiteral(".bat"),
                                         QStringLiteral(".ps1"),
                                         QStringLiteral(".msc")};
    int end = 0;
    while (end < command.size() && !command.at(end).isSpace()) {
        ++end;
    }
    if (end == 0) {
        return;
    }
    command.insert(
        end, suffixes.at(static_cast<int>(prng.below(static_cast<uint32_t>(suffixes.size())))));
}

// Number of distinct obfuscation operators, and the cap on how many are stacked per
// mutant (kept below the classifier's 8-pass escape-strip budget).
constexpr uint32_t kObfuscationOperatorCount = 4;
constexpr uint32_t kMaxObfuscationsPerMutant = 5;

void applyOneObfuscation(QString& command, sak::fuzz::Prng& prng) {
    switch (prng.below(kObfuscationOperatorCount)) {
    case 0:
        insertIntraWordEscape(command, prng, QLatin1Char('^'));
        break;
    case 1:
        insertIntraWordEscape(command, prng, QLatin1Char('`'));
        break;
    case 2:
        flipOneLetterCase(command, prng);
        break;
    default:
        appendExeSuffix(command, prng);
        break;
    }
}

QString obfuscate(const QString& base, sak::fuzz::Prng& prng) {
    QString out = base;
    const uint32_t rounds = 1u + prng.below(kMaxObfuscationsPerMutant);
    for (uint32_t i = 0; i < rounds; ++i) {
        applyOneObfuscation(out, prng);
    }
    return out;
}

// Catastrophic base commands spanning the classifier's branches. Each must already be
// caught unobfuscated (asserted before any mutation).
std::vector<QString> catastrophicSeeds() {
    return {
        QStringLiteral("format c:"),
        QStringLiteral("format /q d:"),
        QStringLiteral("diskpart"),
        QStringLiteral("clear-disk -number 0"),
        QStringLiteral("remove-partition -driveletter d"),
        QStringLiteral("bcdedit /set nx alwayson"),
        QStringLiteral("vssadmin delete shadows /all"),
        QStringLiteral("wbadmin delete catalog -quiet"),
        QStringLiteral("cipher /w:c"),
        QStringLiteral("set-executionpolicy bypass"),
        // Both ExecutionPolicy values the catastrophic arm names must be pinned: the arm is
        // `\bset-executionpolicy\b\s+\S*(unrestricted|bypass)`, and Unrestricted is the wider of
        // the two -- it also drops the remote-signed check for downloaded scripts. No obfuscation
        // operator can turn "bypass" into "unrestricted", so without this seed the `unrestricted|`
        // alternative can be deleted with the whole suite still green, silently losing the
        // human-confirmation gate for `Set-ExecutionPolicy Unrestricted`.
        QStringLiteral("set-executionpolicy unrestricted"),
        QStringLiteral("mkfs ext4 /dev/sdb"),
        QStringLiteral("dd if=/dev/zero of=/dev/sdb"),
    };
}

// Benign read-only commands that must never be promoted to catastrophic.
std::vector<QString> benignSeeds() {
    return {
        QStringLiteral("ipconfig /all"),
        QStringLiteral("get-process"),
        QStringLiteral("ping 8.8.8.8"),
        QStringLiteral("systeminfo"),
        QStringLiteral("tasklist"),
        QStringLiteral("format-list"),  // a PowerShell display cmdlet, not disk format
    };
}

struct PropertyOutcome {
    bool ok = true;
    int checks_run = 0;
    QString failing_command;
    QString detail;
};

// Run @p predicate over every seed and over generated obfuscations of the seeds. The
// predicate returns true when the invariant holds for that command.
PropertyOutcome runProperty(const std::vector<QString>& seeds,
                            const std::function<bool(const QString&)>& predicate,
                            const QString& violation) {
    PropertyOutcome outcome;
    for (const QString& seed : seeds) {
        ++outcome.checks_run;
        if (!predicate(seed)) {
            return {false, outcome.checks_run, seed, QStringLiteral("seed: ") + violation};
        }
    }
    sak::fuzz::Prng prng(sak::fuzz::seedFromEnv());
    const auto seed_count = static_cast<uint32_t>(seeds.size());
    const int iterations = sak::fuzz::iterationsFromEnv();
    for (int i = 0; i < iterations; ++i) {
        const QString mutant = obfuscate(seeds[prng.below(seed_count)], prng);
        ++outcome.checks_run;
        if (!predicate(mutant)) {
            return {false, outcome.checks_run, mutant, violation};
        }
    }
    return outcome;
}

void assertProperty(const PropertyOutcome& outcome, int seed_count) {
    if (!outcome.ok) {
        const QByteArray banner =
            QStringLiteral("classifier property failed after %1 checks: %2\n  command: \"%3\"")
                .arg(outcome.checks_run)
                .arg(outcome.detail, outcome.failing_command)
                .toUtf8();
        QVERIFY2(false, banner.constData());
    }
    // iterationsFromEnv clamps a non-positive SAK_FUZZ_ITERS back to kDefaultIterations. Without
    // that clamp the mutant loop runs zero times and this file silently degenerates into a
    // 12-command smoke test.
    const int iterations = sak::fuzz::iterationsFromEnv();
    QVERIFY(iterations > 0);
    // runProperty increments checks_run once per seed plus once per mutant, so the exact count
    // is knowable; a > 0 floor is satisfied by the seed pass alone. Same convention as the
    // sibling fuzz suites.
    QCOMPARE(outcome.checks_run, seed_count + iterations);
}

}  // namespace

class CommandClassifierFuzzTests : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void catastrophicSurvivesObfuscation() {
        const std::vector<QString> seeds = catastrophicSeeds();
        assertProperty(runProperty(
                           seeds,
                           [](const QString& c) { return sak::ai::commandLooksCatastrophic(c); },
                           QStringLiteral("catastrophic command was NOT classified catastrophic")),
                       static_cast<int>(seeds.size()));
    }

    void benignNeverBecomesCatastrophic() {
        const std::vector<QString> seeds = benignSeeds();
        assertProperty(runProperty(
                           seeds,
                           [](const QString& c) { return !sak::ai::commandLooksCatastrophic(c); },
                           QStringLiteral("benign command WAS classified catastrophic")),
                       static_cast<int>(seeds.size()));
        // The catastrophic TIER is `shell && (catastrophic || obfuscated)`, so
        // !commandLooksCatastrophic alone cannot establish this test's own claim. A tightening
        // of the escape/indirection detector that starts firing on an intra-word caret --
        // undoing the deliberate carve-out -- escalates these safe diagnostics to the mandatory
        // human-confirmation gate with the assertion above still green.
        assertProperty(runProperty(
                           seeds,
                           [](const QString& c) { return !sak::ai::commandLooksObfuscated(c); },
                           QStringLiteral("benign command WAS classified obfuscated")),
                       static_cast<int>(seeds.size()));
    }

    // The disk-format arm accepts ANY drive letter (`[a-z]:`), but the seed corpus only ever
    // presents c: and d: and no obfuscation operator can change a drive letter -- an intra-word
    // escape needs two adjacent word characters so it never splits "c:", and a case flip is
    // absorbed by CaseInsensitiveOption. Narrowing the class therefore survives the entire fuzz
    // run untouched, while "format E:" wipes an attached data volume just as irreversibly and
    // must still reach the confirmation gate. Proving the whole class also kills sibling
    // narrowings such as [a-e]: that one extra seed would survive.
    void everyDriveLetterFormatIsCatastrophic() {
        const QString drive_letters = QStringLiteral("abcdefghijklmnopqrstuvwxyz");
        for (const QChar drive : drive_letters) {
            const QString bare = QStringLiteral("format ") + drive + QLatin1Char(':');
            QVERIFY2(sak::ai::commandLooksCatastrophic(bare), qPrintable(bare));
            const QString switched = QStringLiteral("format /q /fs:ntfs ") + drive +
                                     QLatin1Char(':');
            QVERIFY2(sak::ai::commandLooksCatastrophic(switched), qPrintable(switched));
            const QString split = QStringLiteral("fo^rmat ") + drive + QLatin1Char(':');
            QVERIFY2(sak::ai::commandLooksCatastrophic(split), qPrintable(split));
        }
    }
};

QTEST_APPLESS_MAIN(CommandClassifierFuzzTests)
#include "test_fuzz_command_classifier.moc"
