// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file test_file_explorer_breadcrumb.cpp
/// @brief Unit tests for breadcrumb path segmentation, incl. UNC paths.

#include "sak/file_explorer_breadcrumb.h"

#include <QtTest/QtTest>

using Segment = sak::FileExplorerBreadcrumb::Segment;

class FileExplorerBreadcrumbTests : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void uncPathKeepsBackslashTargets();
    void uncHostOnlyIsSingleSegment();
    void driveAndPosixPathsUnchanged();
};

// A UNC path must reconstruct navigable \\server\share... targets, with host and
// share collapsed into one root segment.
void FileExplorerBreadcrumbTests::uncPathKeepsBackslashTargets() {
    const auto segments =
        sak::FileExplorerBreadcrumb::splitPathSegments(QStringLiteral("\\\\srv\\share\\dir\\sub"));
    QCOMPARE(segments.size(), 3);
    QCOMPARE(segments.at(0).label, QStringLiteral("\\\\srv\\share"));
    QCOMPARE(segments.at(0).target_path, QStringLiteral("\\\\srv\\share"));
    QCOMPARE(segments.at(1).label, QStringLiteral("dir"));
    QCOMPARE(segments.at(1).target_path, QStringLiteral("\\\\srv\\share\\dir"));
    QCOMPARE(segments.at(2).target_path, QStringLiteral("\\\\srv\\share\\dir\\sub"));
    // The deepest UNC tail segment's label is the bare final component. The full
    // target_path pinned just above already starts with '\\' (never '/'), subsuming the
    // old anti-single-slash guard, so pin the previously-unchecked label instead.
    QCOMPARE(segments.at(2).label, QStringLiteral("sub"));
}

// Forward-slash UNC (//srv/share) is treated the same as backslash UNC.
void FileExplorerBreadcrumbTests::uncHostOnlyIsSingleSegment() {
    const auto host_only =
        sak::FileExplorerBreadcrumb::splitPathSegments(QStringLiteral("\\\\srv"));
    QCOMPARE(host_only.size(), 1);
    QCOMPARE(host_only.at(0).target_path, QStringLiteral("\\\\srv"));

    const auto fwd =
        sak::FileExplorerBreadcrumb::splitPathSegments(QStringLiteral("//srv/share/dir"));
    QCOMPARE(fwd.size(), 2);
    QCOMPARE(fwd.at(0).target_path, QStringLiteral("\\\\srv\\share"));
    QCOMPARE(fwd.at(1).target_path, QStringLiteral("\\\\srv\\share\\dir"));
}

// Non-UNC paths keep their existing forward-slash reconstruction.
void FileExplorerBreadcrumbTests::driveAndPosixPathsUnchanged() {
    const auto drive =
        sak::FileExplorerBreadcrumb::splitPathSegments(QStringLiteral("C:\\Work\\Reports"));
    QCOMPARE(drive.size(), 3);
    QCOMPARE(drive.at(0).label, QStringLiteral("C:"));
    QCOMPARE(drive.at(0).target_path, QStringLiteral("C:"));
    QCOMPARE(drive.at(2).target_path, QStringLiteral("C:/Work/Reports"));

    const auto posix = sak::FileExplorerBreadcrumb::splitPathSegments(QStringLiteral("/home/user"));
    QCOMPARE(posix.size(), 3);  // root "/", home, user
    QCOMPARE(posix.at(0).target_path, QStringLiteral("/"));
    QCOMPARE(posix.at(2).target_path, QStringLiteral("/home/user"));
}

QTEST_MAIN(FileExplorerBreadcrumbTests)
#include "test_file_explorer_breadcrumb.moc"
