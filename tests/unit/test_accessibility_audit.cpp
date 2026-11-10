// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file test_accessibility_audit.cpp
/// @brief What the accessibility audit holds a widget to, and that the application passes it.

#include "sak/accessibility_audit.h"
#include "sak/main_window.h"

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QImage>
#include <QLabel>
#include <QLineEdit>
#include <QPixmap>
#include <QPushButton>
#include <QtTest/QtTest>
#include <QVBoxLayout>
#include <QWidget>
#include <QWizard>

class AccessibilityAuditTests : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void cleanup();
    void aControlWithNoNameIsFound();
    void aNamedControlPasses();
    void aButtonWithWordsNeedsNoOtherName();
    void aButtonWithOnlyAnIconNeedsAName();
    void aButtonNameMustHoldItsWords();
    void aLabelWithNoNameIsReadByItsText();
    void aLabelNameThatHidesTheTextIsFound();
    void aLabelNameThatHoldsTheTextPasses();
    void aRichTextLabelIsJudgedByItsWords();
    void aLabelOfSymbolsMayBeNamed();
    void anImageNeedsADescription();
    void anImageWithNameAndDescriptionPasses();
    void anEmptyLabelWithANameIsHeldToTheImageRule();
    void everyKindOfFindingFailsTheAudit();
    void itCountsWhatItChecked();
    void whatEachWalkFoundAddsUp();
    void theApplicationPasses();
};

namespace {
QString findingsText(const sak::AccessibilityFindings& findings) {
    QStringList lines;
    lines << QStringLiteral("checked %1").arg(findings.checked);
    const auto section = [&lines](const char* title, const QStringList& entries) {
        for (const QString& entry : entries) {
            lines << QString::fromLatin1(title) + QStringLiteral(": ") + entry;
        }
    };
    section("missing", findings.missing);
    section("mislabeled", findings.mislabeled);
    section("hides text", findings.hides_text);
    section("undescribed image", findings.undescribed_image);
    return lines.join(QLatin1Char('\n'));
}

QPixmap smallPixmap() {
    QImage image(2, 2, QImage::Format_RGB32);
    image.fill(Qt::black);
    return QPixmap::fromImage(image);
}
}  // namespace

void AccessibilityAuditTests::cleanup() {
    qApp->setProperty("sakAccessibilityAudit", QVariant());
}

void AccessibilityAuditTests::aControlWithNoNameIsFound() {
    QWidget root;
    new QLineEdit(&root);
    const auto findings = sak::auditWidgetTree(root);
    QCOMPARE(findings.missing.size(), 1);
    QVERIFY(!findings.passed());
}

void AccessibilityAuditTests::aNamedControlPasses() {
    QWidget root;
    auto* edit = new QLineEdit(&root);
    edit->setAccessibleName(QStringLiteral("Search"));
    auto* combo = new QComboBox(&root);
    combo->setAccessibleName(QStringLiteral("Kind"));
    QVERIFY2(sak::auditWidgetTree(root).passed(),
             qPrintable(findingsText(sak::auditWidgetTree(root))));
}

void AccessibilityAuditTests::aButtonWithWordsNeedsNoOtherName() {
    QWidget root;
    new QPushButton(QStringLiteral("Save"), &root);
    new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &root);
    QVERIFY2(sak::auditWidgetTree(root).passed(),
             qPrintable(findingsText(sak::auditWidgetTree(root))));
    QWizard wizard;
    wizard.addPage(new QWizardPage(&wizard));
    QVERIFY2(sak::auditWidgetTree(wizard).passed(),
             qPrintable(findingsText(sak::auditWidgetTree(wizard))));
}

void AccessibilityAuditTests::aButtonWithOnlyAnIconNeedsAName() {
    QWidget root;
    new QPushButton(&root);
    QCOMPARE(sak::auditWidgetTree(root).missing.size(), 1);
}

void AccessibilityAuditTests::aButtonNameMustHoldItsWords() {
    QWidget root;
    auto* button = new QPushButton(QStringLiteral("Run &Test"), &root);
    button->setAccessibleName(QStringLiteral("Start the check"));
    QCOMPARE(sak::auditWidgetTree(root).mislabeled.size(), 1);
    button->setAccessibleName(QStringLiteral("Run test of the link"));
    QVERIFY(sak::auditWidgetTree(root).passed());
}

void AccessibilityAuditTests::aLabelWithNoNameIsReadByItsText() {
    QWidget root;
    new QLabel(QStringLiteral("Connected to the server"), &root);
    QVERIFY(sak::auditWidgetTree(root).passed());
}

void AccessibilityAuditTests::aLabelNameThatHidesTheTextIsFound() {
    QWidget root;
    auto* label = new QLabel(QStringLiteral("Connected to the server"), &root);
    label->setAccessibleName(QStringLiteral("Connection status"));
    const auto findings = sak::auditWidgetTree(root);
    QCOMPARE(findings.hides_text.size(), 1);
    QVERIFY2(findings.hides_text.first().contains(QStringLiteral("Connection status")),
             qPrintable(findings.hides_text.first()));
    QVERIFY(!findings.passed());
}

void AccessibilityAuditTests::aLabelNameThatHoldsTheTextPasses() {
    QWidget root;
    auto* label = new QLabel(QStringLiteral("Connected"), &root);
    label->setAccessibleName(QStringLiteral("Status: connected"));
    QVERIFY(sak::auditWidgetTree(root).passed());
}

void AccessibilityAuditTests::aRichTextLabelIsJudgedByItsWords() {
    QWidget root;
    auto* label = new QLabel(QStringLiteral("<b>Fine</b> to go"), &root);
    label->setAccessibleName(QStringLiteral("Fine to go"));
    QVERIFY2(sak::auditWidgetTree(root).passed(),
             qPrintable(findingsText(sak::auditWidgetTree(root))));
    label->setAccessibleName(QStringLiteral("Status"));
    QCOMPARE(sak::auditWidgetTree(root).hides_text.size(), 1);
}

void AccessibilityAuditTests::aLabelOfSymbolsMayBeNamed() {
    QWidget root;
    auto* label = new QLabel(QStringLiteral("--"), &root);
    label->setAccessibleName(QStringLiteral("No reading"));
    QVERIFY(sak::auditWidgetTree(root).passed());
}

void AccessibilityAuditTests::anImageNeedsADescription() {
    QWidget root;
    auto* image = new QLabel(&root);
    image->setPixmap(smallPixmap());
    image->setAccessibleName(QStringLiteral("QR code"));
    const auto findings = sak::auditWidgetTree(root);
    QCOMPARE(findings.undescribed_image.size(), 1);
    QVERIFY(!findings.passed());
}

void AccessibilityAuditTests::anImageWithNameAndDescriptionPasses() {
    QWidget root;
    auto* image = new QLabel(&root);
    image->setPixmap(smallPixmap());
    image->setAccessibleName(QStringLiteral("QR code"));
    image->setAccessibleDescription(QStringLiteral("Scan it to join this network"));
    QVERIFY(sak::auditWidgetTree(root).passed());
}

// A label naming itself before it has any text will hide that text once it comes, so it cannot
// pass as "nothing to hide".
void AccessibilityAuditTests::anEmptyLabelWithANameIsHeldToTheImageRule() {
    QWidget root;
    auto* status = new QLabel(&root);
    status->setAccessibleName(QStringLiteral("Status"));
    QCOMPARE(sak::auditWidgetTree(root).undescribed_image.size(), 1);
}

void AccessibilityAuditTests::everyKindOfFindingFailsTheAudit() {
    sak::AccessibilityFindings each;
    QVERIFY(each.passed());
    each.missing << QStringLiteral("x");
    QVERIFY(!each.passed());
    each = {};
    each.mislabeled << QStringLiteral("x");
    QVERIFY(!each.passed());
    each = {};
    each.hides_text << QStringLiteral("x");
    QVERIFY(!each.passed());
    each = {};
    each.undescribed_image << QStringLiteral("x");
    QVERIFY(!each.passed());
}

void AccessibilityAuditTests::itCountsWhatItChecked() {
    QWidget root;
    new QLabel(QStringLiteral("a"), &root);
    new QLabel(QStringLiteral("b"), &root);
    QCOMPARE(sak::auditWidgetTree(root).checked, 3);
}

void AccessibilityAuditTests::whatEachWalkFoundAddsUp() {
    sak::AccessibilityFindings total;
    total.missing << QStringLiteral("a");
    total.checked = 2;
    sak::AccessibilityFindings next;
    next.missing << QStringLiteral("b");
    next.mislabeled << QStringLiteral("c");
    next.hides_text << QStringLiteral("d");
    next.undescribed_image << QStringLiteral("e");
    next.checked = 3;
    total += next;
    QCOMPARE(total.missing.size(), 2);
    QCOMPARE(total.mislabeled.size(), 1);
    QCOMPARE(total.hides_text.size(), 1);
    QCOMPARE(total.undescribed_image.size(), 1);
    QCOMPARE(total.checked, 5);
}

// The same walk the application's audit mode makes, here so it can run without the application: the
// main window with every tool tab built, then each dialog that can be built without doing anything.
void AccessibilityAuditTests::theApplicationPasses() {
    qApp->setProperty("sakAccessibilityAudit", true);
    sak::MainWindow window;
    const auto main_window_only = sak::auditWidgetTree(window);
    const auto findings = sak::auditApplication(window);
    // Line by line: a Qt Test failure message is cut short, and there may be many.
    for (const QString& line : findingsText(findings).split(QLatin1Char('\n'))) {
        qInfo("%s", qPrintable(line));
    }
    QVERIFY(findings.passed());
    // The dialogs were really looked at: they hold many more widgets than an empty set would.
    QVERIFY2(findings.checked > main_window_only.checked + 100, qPrintable(findingsText(findings)));
}

QTEST_MAIN(AccessibilityAuditTests)
#include "test_accessibility_audit.moc"
