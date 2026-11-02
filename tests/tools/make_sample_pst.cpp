// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file make_sample_pst.cpp
/// @brief Writes the fictional harbor-lane-sample.pst the wiki's Email Tools screenshots open.
///
/// Usage: make_sample_pst <output.pst> [yyyy-MM]
/// The calendar's events fall in the given month, or the current month if none is given.

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
