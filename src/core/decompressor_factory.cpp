// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: MIT

#include "sak/decompressor_factory.h"
#include "sak/gzip_decompressor.h"
#include "sak/bzip2_decompressor.h"
#include "sak/xz_decompressor.h"
#include "sak/logger.h"
#include <QFileInfo>
#include <QFile>

namespace sak {

std::unique_ptr<StreamingDecompressor> DecompressorFactory::create(const QString& filePath) {
    QString format = detectFormat(filePath);
    
    if (format.isEmpty()) {
        sak::logWarning(QString("Unknown compression format: %1").arg(filePath).toStdString());
        return nullptr;
    }

    sak::logInfo(QString("Creating %1 decompressor for %2")
        .arg(format).arg(filePath).toStdString());

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
    QFileInfo fileInfo(filePath);
    QString suffix = fileInfo.suffix().toLower();

    // Handle compound extensions like .tar.gz
    QString completeSuffix = fileInfo.completeSuffix().toLower();

    if (suffix == "gz" || completeSuffix.endsWith(".gz") || suffix == "gzip") {
        return "gzip";
    } else if (suffix == "bz2" || completeSuffix.endsWith(".bz2") || suffix == "bzip2") {
        return "bzip2";
    } else if (suffix == "xz" || completeSuffix.endsWith(".xz") || suffix == "lzma") {
        return "xz";
    } else if (suffix == "zip") {
        return "zip";
    }

    return QString();
}

QString DecompressorFactory::detectByMagicNumber(const QString& filePath) {
    unsigned char magic[16];
    if (!readMagicNumber(filePath, magic, sizeof(magic))) {
        return QString();
    }

    // Gzip: 1F 8B
    if (magic[0] == 0x1F && magic[1] == 0x8B) {
        return "gzip";
    }

    // Bzip2: 42 5A 68 ("BZh")
    if (magic[0] == 0x42 && magic[1] == 0x5A && magic[2] == 0x68) {
        return "bzip2";
    }

    // XZ: FD 37 7A 58 5A 00
    if (magic[0] == 0xFD && magic[1] == 0x37 && magic[2] == 0x7A &&
        magic[3] == 0x58 && magic[4] == 0x5A && magic[5] == 0x00) {
        return "xz";
    }

    // ZIP: 50 4B ("PK")
    if (magic[0] == 0x50 && magic[1] == 0x4B) {
        return "zip";
    }

    // LZMA (old format): 5D 00 00
    if (magic[0] == 0x5D && magic[1] == 0x00 && magic[2] == 0x00) {
        return "xz";  // Use xz decompressor for old LZMA format
    }

    return QString();
}

bool DecompressorFactory::readMagicNumber(const QString& filePath, unsigned char* buffer, int size) {
    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly)) {
        return false;
    }

    qint64 bytesRead = file.read(reinterpret_cast<char*>(buffer), size);
    file.close();

    return bytesRead == size;
}

} // namespace sak
