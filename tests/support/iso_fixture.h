// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

/// @file iso_fixture.h
/// @brief A real, minimal ISO 9660 image for tests, small enough to build in memory.

#include <QByteArray>
#include <QtEndian>

#include <cstring>

namespace sak_test {

// One 2048-byte ISO 9660 directory record.
[[nodiscard]] inline QByteArray isoDirectoryRecord(quint32 lba,
                                                   quint32 size,
                                                   bool directory,
                                                   const QByteArray& name) {
    QByteArray record(33 + name.size() + (name.size() % 2 == 0 ? 1 : 0), '\0');
    record[0] = static_cast<char>(record.size());
    qToLittleEndian<quint32>(lba, record.data() + 2);
    qToBigEndian<quint32>(lba, record.data() + 6);
    qToLittleEndian<quint32>(size, record.data() + 10);
    qToBigEndian<quint32>(size, record.data() + 14);
    record[25] = directory ? 2 : 0;
    qToLittleEndian<quint16>(1, record.data() + 28);
    qToBigEndian<quint16>(1, record.data() + 30);
    record[32] = static_cast<char>(name.size());
    record.replace(33, name.size(), name);
    return record;
}

// A real, minimal ISO 9660 image holding one file at SOURCES/<file_name>.
[[nodiscard]] inline QByteArray isoWithSourcesFile(const QByteArray& file_name) {
    constexpr int kSector = 2048;
    constexpr quint32 kPvd = 16;
    constexpr quint32 kTerminator = 17;
    constexpr quint32 kRoot = 18;
    constexpr quint32 kSources = 19;
    constexpr quint32 kData = 20;
    constexpr quint32 kSectors = 21;
    QByteArray image(kSectors * kSector, '\0');
    const auto sector = [&image](quint32 lba) {
        return image.data() + (lba * kSector);
    };

    QByteArray pvd(kSector, '\0');
    pvd[0] = 1;
    pvd.replace(1, 5, "CD001");
    pvd[6] = 1;
    qToLittleEndian<quint32>(kSectors, pvd.data() + 80);
    qToBigEndian<quint32>(kSectors, pvd.data() + 84);
    qToLittleEndian<quint16>(1, pvd.data() + 120);
    qToBigEndian<quint16>(1, pvd.data() + 122);
    qToLittleEndian<quint16>(1, pvd.data() + 124);
    qToBigEndian<quint16>(1, pvd.data() + 126);
    qToLittleEndian<quint16>(kSector, pvd.data() + 128);
    qToBigEndian<quint16>(kSector, pvd.data() + 130);
    const QByteArray root_self = isoDirectoryRecord(kRoot, kSector, true, QByteArray(1, '\0'));
    pvd.replace(156, root_self.size(), root_self);
    pvd[881] = 1;
    std::memcpy(sector(kPvd), pvd.constData(), kSector);

    QByteArray terminator(kSector, '\0');
    terminator[0] = static_cast<char>(255);
    terminator.replace(1, 5, "CD001");
    terminator[6] = 1;
    std::memcpy(sector(kTerminator), terminator.constData(), kSector);

    const QByteArray dot = isoDirectoryRecord(kRoot, kSector, true, QByteArray(1, '\0'));
    const QByteArray dotdot = isoDirectoryRecord(kRoot, kSector, true, QByteArray(1, '\1'));
    const QByteArray root = dot + dotdot + isoDirectoryRecord(kSources, kSector, true, "SOURCES");
    std::memcpy(sector(kRoot), root.constData(), root.size());

    const QByteArray sources = isoDirectoryRecord(kSources, kSector, true, QByteArray(1, '\0')) +
                               dotdot + isoDirectoryRecord(kData, 4, false, file_name + ";1");
    std::memcpy(sector(kSources), sources.constData(), sources.size());

    std::memcpy(sector(kData), "data", 4);
    return image;
}

}  // namespace sak_test
