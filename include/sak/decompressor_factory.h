// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: MIT

#pragma once

#include "sak/streaming_decompressor.h"
#include <QString>
#include <QFile>
#include <memory>

namespace sak {

/**
 * @brief Factory for creating appropriate decompressor instances
 * 
 * Auto-detects compression format from file extension and magic numbers.
 * Creates the correct decompressor type for the file.
 * 
 * Supported Formats:
 * - .gz, .gzip - Gzip (via zlib)
 * - .bz2, .bzip2 - Bzip2 (via libbz2)
 * - .xz, .lzma - XZ/LZMA (via liblzma)
 * 
 * Note: ZIP format is not supported for disk image decompression.
 * ZIP is multi-file archive format, while disk images are single compressed files.
 * 
 * Thread-Safety: Thread-safe. Can be called from multiple threads.
 * 
 * Example:
 * @code
 * auto decompressor = DecompressorFactory::create("image.iso.gz");
 * if (decompressor) {
 *     decompressor->open("image.iso.gz");
 *     // Use decompressor
 * }
 * @endcode
 */
class DecompressorFactory {
public:
    /**
     * @brief Create decompressor for file
     * @param filePath Path to compressed file
     * @return Decompressor instance, or nullptr if format not supported
     */
    static std::unique_ptr<StreamingDecompressor> create(const QString& filePath);

    /**
     * @brief Check if file is compressed
     * @param filePath Path to file
     * @return true if file is a supported compressed format
     */
    static bool isCompressed(const QString& filePath);

    /**
     * @brief Detect compression format from file
     * @param filePath Path to file
     * @return Format name ("gzip", "bzip2", "xz", "zip"), or empty if not compressed
     */
    static QString detectFormat(const QString& filePath);

private:
    /**
     * @brief Detect format by file extension
     * @param filePath Path to file
     * @return Format name, or empty if unknown
     */
    static QString detectByExtension(const QString& filePath);

    /**
     * @brief Detect format by magic number
     * @param filePath Path to file
     * @return Format name, or empty if unknown
     */
    static QString detectByMagicNumber(const QString& filePath);

    /**
     * @brief Read magic number from file
     * @param filePath Path to file
     * @param buffer Buffer to read into (at least 16 bytes)
     * @param size Number of bytes to read
     * @return true if successful
     */
    static bool readMagicNumber(const QString& filePath, unsigned char* buffer, int size);
};

} // namespace sak
