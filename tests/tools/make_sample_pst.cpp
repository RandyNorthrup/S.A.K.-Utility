// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file make_sample_pst.cpp
/// @brief Writes the fictional harbor-lane-sample.pst the wiki's Email Tools screenshots open.
///
/// Usage: make_sample_pst <output.pst> [yyyy-MM]
/// The calendar's events fall in the given month, or the current month if none is given.
///
/// The file is for screenshots of this app, which reads it. Never attach it to Outlook. Its header
/// has no valid allocation fields (bidNextB and bidNextP are 0) and it has no allocation maps, so
/// Outlook's PST provider (MSPST32) stops at its corruption check and ends the Outlook process.
/// That is Outlook refusing this file, not a defect in Outlook, and it can leave an unconfigured
/// data file entry behind in the profile.

#include "../support/harbor_lane_sample.h"

#include <QCryptographicHash>
#include <QFile>
#include <QGuiApplication>

#include <cstdio>

int main(int argc, char** argv) {
    QGuiApplication app(argc, argv);
    if (argc < 2) {
        std::fprintf(stderr, "usage: make_sample_pst <output.pst> [yyyy-MM]\n");
        return 2;
    }
    QDate month = QDate::currentDate();
    if (argc > 2) {
        month = QDate::fromString(QString::fromLocal8Bit(argv[2]), QStringLiteral("yyyy-MM"));
        if (!month.isValid()) {
            std::fprintf(stderr, "the month must be written yyyy-MM\n");
            return 2;
        }
    }
    const QByteArray store = sak::pst_fixture::harbor_lane::build(month);
    QFile file(QString::fromLocal8Bit(argv[1]));
    if (!file.open(QIODevice::WriteOnly) || file.write(store) != store.size() || !file.flush()) {
        std::fprintf(stderr, "could not write the output file\n");
        return 1;
    }
    file.close();
    const QByteArray sha = QCryptographicHash::hash(store, QCryptographicHash::Sha256).toHex();
    std::printf("%lld bytes, SHA-256 %s\n", static_cast<long long>(store.size()), sha.constData());
    return 0;
}
