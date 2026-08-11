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

namespace {
constexpr int kMagicHeaderProbeBytes = 16;
constexpr int kMaxMagicSignatureBytes = 6;
}  // namespace

std::unique_ptr<StreamingDecompressor> DecompressorFactory::create(const QString& filePath) {
    // An empty path detects as no format, which is rejected with a nullptr below.
    const QString format = detectFormat(filePath);

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
    // An empty path has no suffix and matches no entry, so it returns the empty
    // "unknown format" result like any other unrecognised name.
    const QFileInfo fileInfo(filePath);
    QString suffix = fileInfo.suffix().toLower();

    struct ExtEntry {
        const char* ext;
        const char* format;
    };
    // Only single-stream formats this factory can actually create() a decompressor
    // for. A zip is a multi-member archive handled by the archive service, not a
    // stream -- advertising it here made isCompressed() return true while create()
    // returned nullptr, an inconsistency a guard-then-create caller would trip on
    // (B8-23).
    static constexpr ExtEntry kExtensions[] = {
        {.ext = "gz", .format = "gzip"},
        {.ext = "gzip", .format = "gzip"},
        {.ext = "bz2", .format = "bzip2"},
        {.ext = "bzip2", .format = "bzip2"},
        {.ext = "xz", .format = "xz"},
        {.ext = "lzma", .format = "xz"},
    };

    const auto* it = std::ranges::find_if(kExtensions, [&suffix](const auto& entry) {
        return suffix == QLatin1String(entry.ext);
    });
    if (it != std::end(kExtensions)) {
        return QLatin1String(it->format);
    }

    // Handle compound extensions like .tar.gz
    QString completeSuffix = fileInfo.completeSuffix().toLower();
    static constexpr ExtEntry kCompound[] = {
        {.ext = ".gz", .format = "gzip"},
        {.ext = ".bz2", .format = "bzip2"},
        {.ext = ".xz", .format = "xz"},
    };
    const auto* compound_it = std::ranges::find_if(kCompound, [&completeSuffix](const auto& entry) {
        return completeSuffix.endsWith(QLatin1String(entry.ext));
    });
    if (compound_it != std::end(kCompound)) {
        return QLatin1String(compound_it->format);
    }

    return QString();
}

QString DecompressorFactory::detectByMagicNumber(const QString& filePath) {
    // readMagicNumber() cannot open an empty (or unreadable) path, so it returns a
    // negative count and this reports the empty "unknown format" result. The buffer is
    // zero-initialised so a signature is only ever matched against bytes actually read.
    unsigned char magic[kMagicHeaderProbeBytes] = {};
    const int bytesRead = readMagicNumber(filePath, magic, sizeof(magic));
    if (bytesRead <= 0) {
        return QString();
    }

    struct MagicEntry {
        const unsigned char bytes[kMaxMagicSignatureBytes];
        int length;
        const char* format;
    };
    // No zip ("PK") entry: this factory only produces single-stream decompressors,
    // and create() cannot build one for a zip archive (B8-23).
    static constexpr MagicEntry kMagicTable[] = {
        {.bytes = {0xFD, 0x37, 0x7A, 0x58, 0x5A, 0x00}, .length = 6, .format = "xz"},
        {.bytes = {0x42, 0x5A, 0x68, 0, 0, 0}, .length = 3, .format = "bzip2"},
        {.bytes = {0x5D, 0x00, 0x00, 0, 0, 0}, .length = 3, .format = "xz"},
        {.bytes = {0x1F, 0x8B, 0, 0, 0, 0}, .length = 2, .format = "gzip"},
    };

    for (const auto& entry : kMagicTable) {
        // Not enough bytes were read to confirm this signature; never let the
        // zero-filled tail complete a match on a short file.
        if (entry.length > bytesRead) {
            continue;
        }
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

int DecompressorFactory::readMagicNumber(const QString& filePath, unsigned char* buffer, int size) {
    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly)) {
        return -1;
    }

    const qint64 bytesRead = file.read(reinterpret_cast<char*>(buffer), size);
    file.close();

    // A read fault reports -1; a short read reports the count actually delivered so the
    // caller matches signatures only against real bytes (size <= kMagicHeaderProbeBytes,
    // so the value always fits in int).
    if (bytesRead < 0) {
        return -1;
    }
    return static_cast<int>(bytesRead);
}

}  // namespace sak
