// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

// Unit tests for sak::ui::ViewEmptyState: the shared empty/loading-state
// overlay for item views (R5-G20-7). Headless; widgets are never shown, so the
// overlay's intended visibility is read via isOverlayVisible() (isHidden-based).

#include "sak/view_empty_state.h"

#include <QListWidget>
#include <QTableWidget>
#include <QtTest/QtTest>

class ViewEmptyStateTests : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void visibleWhenTableEmpty();
    void hiddenWhenRowsPresent();
    void reappearsAfterRowsCleared();
    void loadingOverridesRowCount();
    void clearLoadingRestoresRowDrivenState();
    void emptyTextIsShownAndUpdatable();
    void worksOnListWidget();
    void survivesViewDestruction();
};

void ViewEmptyStateTests::visibleWhenTableEmpty() {
    QTableWidget table(0, 3);
    sak::ui::ViewEmptyState empty(&table, QStringLiteral("No disks scanned"));
    QVERIFY(empty.isOverlayVisible());
    QCOMPARE(empty.overlayText(), QStringLiteral("No disks scanned"));
}

void ViewEmptyStateTests::hiddenWhenRowsPresent() {
    QTableWidget table(0, 3);
    sak::ui::ViewEmptyState empty(&table, QStringLiteral("No disks scanned"));
    table.insertRow(0);
    QVERIFY(!empty.isOverlayVisible());
}

void ViewEmptyStateTests::reappearsAfterRowsCleared() {
    QTableWidget table(0, 3);
    sak::ui::ViewEmptyState empty(&table, QStringLiteral("No disks scanned"));
    table.insertRow(0);
    QVERIFY(!empty.isOverlayVisible());
    table.setRowCount(0);
    QVERIFY(empty.isOverlayVisible());
}

void ViewEmptyStateTests::loadingOverridesRowCount() {
    QTableWidget table(0, 3);
    sak::ui::ViewEmptyState empty(&table, QStringLiteral("No disks scanned"));
    table.insertRow(0);
    QVERIFY(!empty.isOverlayVisible());  // rows present, not loading

    empty.setLoading(QStringLiteral("Scanning disks..."));
    QVERIFY(empty.isOverlayVisible());  // loading shows even with rows
    QCOMPARE(empty.overlayText(), QStringLiteral("Scanning disks..."));
}

void ViewEmptyStateTests::clearLoadingRestoresRowDrivenState() {
    QTableWidget table(0, 3);
    sak::ui::ViewEmptyState empty(&table, QStringLiteral("No disks scanned"));
    table.insertRow(0);
    empty.setLoading(QStringLiteral("Scanning disks..."));
    QVERIFY(empty.isOverlayVisible());

    empty.clearLoading();
    QVERIFY(!empty.isOverlayVisible());  // rows present again -> hidden
    QCOMPARE(empty.overlayText(), QStringLiteral("No disks scanned"));
}

void ViewEmptyStateTests::emptyTextIsShownAndUpdatable() {
    QTableWidget table(0, 1);
    sak::ui::ViewEmptyState empty(&table, QStringLiteral("No matches found"));
    QCOMPARE(empty.overlayText(), QStringLiteral("No matches found"));
    empty.setEmptyText(QStringLiteral("No packages found"));
    QCOMPARE(empty.overlayText(), QStringLiteral("No packages found"));
    QVERIFY(empty.isOverlayVisible());
}

void ViewEmptyStateTests::worksOnListWidget() {
    QListWidget list;
    sak::ui::ViewEmptyState empty(&list, QStringLiteral("No items"));
    QVERIFY(empty.isOverlayVisible());
    list.addItem(QStringLiteral("one"));
    QVERIFY(!empty.isOverlayVisible());
    list.clear();
    QVERIFY(empty.isOverlayVisible());
}

void ViewEmptyStateTests::survivesViewDestruction() {
    // Regression: during teardown the view's internal model emits modelReset while
    // the overlay label (a viewport grandchild) may already be gone. The model-
    // signal connections must be tied to the label's lifetime so the callback can
    // never reach a dangling setText(). Destroying a populated view must not crash.
    auto* table = new QTableWidget(0, 2);
    new sak::ui::ViewEmptyState(table, QStringLiteral("Nothing here"));
    table->insertRow(0);
    table->insertRow(1);
    delete table;  // triggers the model-reset-during-destruction path
    QVERIFY(true);
}

QTEST_MAIN(ViewEmptyStateTests)
#include "test_view_empty_state.moc"
