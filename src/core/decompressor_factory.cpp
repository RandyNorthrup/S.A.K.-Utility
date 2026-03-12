// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/decompressor_factory.h"

#include "sak/bzip2_decompressor.h"
#include "sak/gzip_decompressor.h"
#include "sak/logger.h"
#include "sak/xz_decompressor.h"

#include <QFile>
#include <QFileInfo>
#include <algorithm>

namespace sak {

std::unique_ptr<StreamingDecompressor> DecompressorFactory::create(const QString& filePath) {
    Q_ASSERT(!filePath.isEmpty());
    QString format = detectFormat(filePath);

    if (format.isEmpty()) {
        sak::logWarning(QString("Unknown compression format: %1").arg(filePath).toStdString());
        return nullptr;
    }

    sak::logInfo(
        QString("Creating %1 decompressor for %2").arg(format).arg(filePath).toStdString());

    if (format == "gzip") {
        return std::make_unique<GzipDecompressor>();
    } else if (format == "bzip2") {
        return std::make_unique<Bzip2Decompressor>();
    } else if (format == "xz") {
        return std::make_unique<XzDecompressor>();
    }

    sak::logWarning(QString("Unsupported compression format: %1").arg(format).toStdString());
    return nullptr;
}

bool DecompressorFactory::isCompressed(const QString& filePath) {
    return !detectFormat(filePath).isEmpty();
}

QString DecompressorFactory::detectFormat(const QString& filePath) {
    // Try extension first (fast)
    QString format = detectByExtension(filePath);
    if (!format.isEmpty()) {
        return format;
    }

    // Fall back to magic number detection
    return detectByMagicNumber(filePath);
}

QString DecompressorFactory::detectByExtension(const QString& filePath) {
    Q_ASSERT(!filePath.isEmpty());
    QFileInfo fileInfo(filePath);
    QString suffix = fileInfo.suffix().toLower();

    struct ExtEntry {
        const char* ext;
        const char* format;
    };
    static constexpr ExtEntry kExtensions[] = {
        {"gz", "gzip"},
        {"gzip", "gzip"},
        {"bz2", "bzip2"},
        {"bzip2", "bzip2"},
        {"xz", "xz"},
        {"lzma", "xz"},
        {"zip", "zip"},
    };

    auto it = std::find_if(std::begin(kExtensions), std::end(kExtensions),
        [&suffix](const auto& entry) {
            return suffix == QLatin1String(entry.ext);
        });
    if (it != std::end(kExtensions)) {
        return QLatin1String(it->format);
    }

    // Handle compound extensions like .tar.gz
    QString completeSuffix = fileInfo.completeSuffix().toLower();
    static constexpr ExtEntry kCompound[] = {
        {".gz", "gzip"},
        {".bz2", "bzip2"},
        {".xz", "xz"},
    };
    auto compound_it = std::find_if(std::begin(kCompound), std::end(kCompound),
        [&completeSuffix](const auto& entry) {
            return completeSuffix.endsWith(QLatin1String(entry.ext));
        });
    if (compound_it != std::end(kCompound)) {
        return QLatin1String(compound_it->format);
    }

    return QString();
}

QString DecompressorFactory::detectByMagicNumber(const QString& filePath) {
    Q_ASSERT(!filePath.isEmpty());
    unsigned char magic[16];
    if (!readMagicNumber(filePath, magic, sizeof(magic))) {
        return QString();
    }

    struct MagicEntry {
        const unsigned char bytes[6];
        int length;
        const char* format;
    };
    static constexpr MagicEntry kMagicTable[] = {
        {{0xFD, 0x37, 0x7A, 0x58, 0x5A, 0x00}, 6, "xz"},
        {{0x42, 0x5A, 0x68, 0, 0, 0}, 3, "bzip2"},
        {{0x5D, 0x00, 0x00, 0, 0, 0}, 3, "xz"},
        {{0x1F, 0x8B, 0, 0, 0, 0}, 2, "gzip"},
        {{0x50, 0x4B, 0, 0, 0, 0}, 2, "zip"},
    };

    for (const auto& entry : kMagicTable) {
        bool match = true;
        for (int i = 0; i < entry.length; ++i) {
            if (magic[i] != entry.bytes[i]) {
                match = false;
                break;
            }
        }
        if (match) {
            return QLatin1String(entry.format);
        }
    }

    return QString();
}

bool DecompressorFactory::readMagicNumber(const QString& filePath,
                                          unsigned char* buffer,
                                          int size) {
    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly)) {
        return false;
    }

    qint64 bytesRead = file.read(reinterpret_cast<char*>(buffer), size);
    file.close();

    return bytesRead == size;
}

}  // namespace sak
